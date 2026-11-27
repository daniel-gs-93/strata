#ifndef STRATA_PAGED_BTREE_H_
#define STRATA_PAGED_BTREE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "strata/paged/btree_node.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/key_range.h"
#include "strata/paged/page_allocator.h"
#include "strata/paged/paged_types.h"

// A B+Tree over the pages of a paged database (design document 2.17, ADR-0049
// and ADR-0050). Keys are unique signed 64-bit integers, an entry is a key and
// the Rid of its row. Resource-aware: it fetches nodes through the
// BufferManager, takes pages from the PageAllocator and calls the pure node
// functions of btree_node.h on the frame bytes. It holds no page between two
// calls, and every node is held by a local PageGuard, so each pin and latch is
// released on every path.
//
// Latch order. Tree latches are taken from the root toward the leaf and from
// left to right along the leaves, then the file header inside
// PageAllocator::Allocate, then the new page. A reader latches a child shared
// before it releases the parent (two latches at most while it descends, one
// while it moves along the leaves). A writer descends with exclusive latches
// and splits every full node it meets, so the parent of a node being split
// always has room: it holds the parent, the child, the file header and the new
// page, four latches at most. A split node and its parent stay latched until
// the new sibling, the sibling link and the separator are written, so a reader
// sees a node before the split or after it, never between. Delete and Replace
// descend shared and latch the leaf exclusively.
//
// Failure order. The fetch of a node and Allocate are the only steps that can
// fail, and each comes before the first byte of the nodes it concerns changes,
// so a failed Insert leaves a valid tree.
//
// Concurrency. Any number of readers may run beside one writer. A tree needs
// one writer at a time, which the database-level lock gives. Two BTree objects
// over the same root must not write at once.

namespace strata::paged {

// The capacities of a tree. A node holds at most its capacity of keys. A tree
// built with smaller values reaches a greater height with fewer keys, which
// tests use. The values are not stored in the file: a file is always read with
// the defaults, and Create and Open take the same options.
struct BTreeOptions {
  std::size_t leaf_capacity = kLeafCapacity;
  std::size_t internal_capacity = kInternalCapacity;
};

struct BTreeEntry {
  std::int64_t key;
  Rid rid;

  bool operator==(const BTreeEntry&) const = default;
};

class BTreeCursor;

class BTree {
 public:
  // Allocates the root, formats it as an empty leaf and returns the tree.
  // kInvalidArgument: a capacity outside [2, 289] for a leaf or [3, 337] for an
  // internal node. Errors of Allocate otherwise.
  static PageResult<BTree> Create(BufferManager& buffer,
                                  PageAllocator& allocator,
                                  std::uint32_t table_id,
                                  BTreeOptions options = {});

  // Checks the root (page type 4, this table, level below 40, key_count within
  // the capacity) and returns the tree. kInvalidArgument as Create. kCorrupt:
  // the root fails the check. kNotFound, kIo: from the store.
  static PageResult<BTree> Open(BufferManager& buffer, PageAllocator& allocator,
                                std::uint32_t table_id, PageId root,
                                BTreeOptions options = {});

  PageId root() const { return root_; }
  std::uint32_t table_id() const { return table_id_; }

  // Adds the entry. kAlreadyExists: the key is in the tree, and no key was
  // added or removed; full nodes on the way down may already have been split,
  // because the writer splits before it looks at the leaf. kNoFrame, kIo,
  // kCorrupt: from a fetch or Allocate; the tree is valid after an error, with
  // at most the splits that were already done. A page that a pointer of the
  // tree names outside the file is kCorrupt.
  Status Insert(std::int64_t key, Rid rid);

  // The Rid of the key, or no value when the key is absent. Reads one page per
  // level.
  PageResult<std::optional<Rid>> Find(std::int64_t key) const;

  // Removes the key from its leaf. Lazy: no node is merged, redistributed or
  // freed, and the leaf may become empty. kNotFound: the key is absent.
  Status Delete(std::int64_t key);

  // Points the key at a new Rid. kNotFound: the key is absent.
  Status Replace(std::int64_t key, Rid rid);

  // A cursor over the entries in the range, in ascending key order. Reads no
  // page until the first Next. The cursor holds a copy of this tree handle, so
  // the BufferManager and the PageAllocator must outlive it.
  BTreeCursor Scan(const KeyRange& range) const;

  // Every page of the tree, root first, depth first, from a walk that fails
  // as CheckTree does on a repeated page, a foreign page or a wrong level.
  PageResult<std::vector<PageId>> Pages() const;

  // The seven invariants of design document 2.17, reading each node under a
  // shared latch. This, Pages and Shape copy one node at a time and give a
  // false kCorrupt when a split runs between two reads, so the caller keeps
  // writers out (QueryService holds its lock shared; a writer takes it
  // exclusive).
  Status CheckInvariants() const;

  // Height, node and key counts and the first node_limit nodes in breadth-first
  // order. An error only when the root cannot be read or opened.
  PageResult<TreeShape> Shape(std::size_t node_limit) const;

 private:
  friend class BTreeCursor;

  BTree(BufferManager& buffer, PageAllocator& allocator, std::uint32_t table_id,
        PageId root, NodeCapacity capacity)
      : buffer_(buffer),
        allocator_(allocator),
        table_id_(table_id),
        root_(root),
        capacity_(capacity) {}

  // Fetches a node and makes the O(1) check of design document 2.17 (damaged
  // nodes): kCorrupt for a node that fails it, so a damaged key_count never
  // reads past the arrays.
  PageResult<PageGuard> FetchNode(PageId page_id, LatchMode mode,
                                  std::optional<std::uint16_t> level) const;

  // The leaf that covers key, or the leftmost leaf when there is no key,
  // latched shared; each child is latched before its parent is released. When
  // upper_fence is not null it receives the smallest separator that bounds the
  // leaf from above: every key of a later leaf is at or above it. It receives
  // no value for the rightmost leaf.
  PageResult<PageGuard> DescendShared(
      std::optional<std::int64_t> key,
      std::optional<std::int64_t>* upper_fence = nullptr) const;

  // The leaf that covers key, latched exclusively, for Delete and Replace. The
  // nodes above it are latched shared, each child before its parent is
  // released, so a descent holds two latches at most.
  PageResult<PageGuard> DescendForWrite(std::int64_t key) const;

  // A new empty node of the given level (a leaf for level 0), unlatched.
  PageResult<PageId> AllocateEmptyNode(std::uint16_t level);

  // Splits the full root in place and returns the half that key enters,
  // latched exclusively; the root and the other half are released.
  PageResult<PageGuard> SplitRoot(PageGuard root, std::int64_t key);

  // Splits the full child of parent into a new right sibling, writes the
  // separator into parent and returns the node that key enters; the parent
  // and the other node are released.
  PageResult<PageGuard> SplitChild(PageGuard parent, std::size_t child_index,
                                   PageGuard child, std::int64_t key);

  Status InsertIntoLeaf(PageGuard& leaf, std::int64_t key, Rid rid);

  BufferManager& buffer_;
  PageAllocator& allocator_;
  std::uint32_t table_id_;
  PageId root_;
  NodeCapacity capacity_;
};

// An ascending cursor over a key range. Next copies the qualifying entries of
// one leaf into a buffer under a shared latch, releases the latch and returns
// the entries one by one, so no latch and no pin is held between two Next
// calls. It sees each leaf as it was when it read it.
class BTreeCursor {
 public:
  // The next entry, or no value at the end of the range. An error ends the
  // cursor: later calls return no value. kCorrupt includes a leaf chain longer
  // than the file.
  PageResult<std::optional<BTreeEntry>> Next();

 private:
  friend class BTree;

  BTreeCursor(const BTree& tree, const KeyRange& range);

  Status FillBuffer();

  BTree tree_;
  std::optional<std::int64_t> first_key_;
  std::optional<std::int64_t> last_key_;
  bool is_started_ = false;
  bool is_finished_ = false;
  PageId next_leaf_ = kInvalidPageId;
  std::size_t leaves_read_ = 0;
  std::vector<BTreeEntry> buffer_;
  std::size_t position_ = 0;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_BTREE_H_
