#ifndef STRATA_PAGED_BTREE_NODE_H_
#define STRATA_PAGED_BTREE_NODE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "strata/paged/paged_types.h"

// Pure functions over the bytes of one B+Tree node and the invariant checker
// of a whole tree (design document 2.10 and 2.17). A node is one 4096-byte page
// of type 4. Nothing here pins, latches, reads a file or knows a buffer pool:
// the functions take a pointer to kPageSize bytes that the caller owns, and the
// tree checks read nodes through INodeSource.
//
// Layout after the 32-byte common header: level (2 bytes), key_count (2) and 12
// reserved bytes at offset 32; then the arrays. A leaf has keys[289] from
// offset 48 and rids[289] from offset 2360; an internal node has keys[337]
// from offset 48 and children[338] from offset 2744. The first key_count
// entries (key_count + 1 children) are valid. Keys are signed 64-bit integers.

namespace strata::paged {

namespace node_layout {
inline constexpr std::size_t kLevel = 32;
inline constexpr std::size_t kKeyCount = 34;
inline constexpr std::size_t kKeys = 48;
inline constexpr std::size_t kLeafRids = 2360;
inline constexpr std::size_t kInternalChildren = 2744;
inline constexpr std::size_t kKeySize = 8;
inline constexpr std::size_t kRidSize = 6;
inline constexpr std::size_t kChildSize = 4;
}  // namespace node_layout

inline constexpr std::size_t kLeafCapacity = 289;
inline constexpr std::size_t kInternalCapacity = 337;
// Half of each capacity, rounded down: the minimum occupancy that a later full
// delete needs. Defined here and checked by nothing while delete is lazy.
inline constexpr std::size_t kLeafMinKeys = kLeafCapacity / 2;
inline constexpr std::size_t kInternalMinKeys = kInternalCapacity / 2;
// The level of a node is below this. A valid tree has fewer than 33 levels
// (an internal node has at least 2 children and a file has at most 2^32
// pages), so the limit rejects no valid tree and stops a loop.
inline constexpr std::uint16_t kMaxNodeLevel = 40;

static_assert(node_layout::kKeys + node_layout::kKeySize * kLeafCapacity ==
              node_layout::kLeafRids);
static_assert(node_layout::kLeafRids + node_layout::kRidSize * kLeafCapacity <=
              kPageSize);
static_assert(node_layout::kKeys + node_layout::kKeySize * kInternalCapacity ==
              node_layout::kInternalChildren);
static_assert(node_layout::kInternalChildren +
                  node_layout::kChildSize * (kInternalCapacity + 1) ==
              kPageSize);

// The most keys a node may hold. A tree built with smaller values reaches a
// greater height with few keys; a node holds at most the capacity of the
// options it was built with. The values are not stored in the file.
struct NodeCapacity {
  std::size_t leaf = kLeafCapacity;
  std::size_t internal = kInternalCapacity;

  bool operator==(const NodeCapacity&) const = default;
};

// True when leaf is in [2, 289] and internal is in [3, 337], so that every
// half of a split keeps at least one key.
bool IsValidCapacity(NodeCapacity capacity);

// Read-only access to the fields of a node. The page must stay unchanged and
// alive while the view is used. KeyAt, RidAt and ChildAt require an index below
// the layout capacity (289 keys, 289 rids, 338 children); key_count of a
// damaged page can exceed it, so a caller checks the header first
// (CheckNodeHeader) before it reads entries up to key_count.
class NodeView {
 public:
  explicit NodeView(const std::uint8_t* page) : page_(page) {}

  PageId page_id() const;
  std::uint32_t table_id() const;
  // The right sibling of a leaf, or kInvalidPageId at the rightmost leaf and
  // on every internal node.
  PageId next_page_id() const;
  std::uint16_t level() const;
  std::uint16_t key_count() const;
  bool is_leaf() const { return level() == 0; }

  std::int64_t KeyAt(std::size_t index) const;
  // Leaf only.
  Rid RidAt(std::size_t index) const;
  // Internal node only; index is at most key_count.
  PageId ChildAt(std::size_t index) const;

 private:
  const std::uint8_t* page_;
};

// Zero the page and write an empty node of type 4. The checksum is not
// written (see SlottedPage::SealChecksum). An empty leaf has next_page_id
// kInvalidPageId; an internal node has level at least 1.
void FormatLeaf(std::uint8_t* page, PageId page_id, std::uint32_t table_id);
void FormatInternal(std::uint8_t* page, PageId page_id, std::uint32_t table_id,
                    std::uint16_t level);

struct LeafPosition {
  // The number of keys below the search key: where the key is or would go.
  std::size_t index;
  bool is_found;
};

// Lower-bound binary search over the key array, read in place. O(log
// key_count); copies nothing.
LeafPosition LeafSearch(const std::uint8_t* page, std::int64_t key);

// Insert key and rid at position (at most key_count). Returns false and
// changes nothing when the leaf already holds capacity keys or position is
// past key_count. The caller found the position with LeafSearch and knows the
// key is absent.
bool LeafInsertAt(std::uint8_t* page, std::size_t capacity,
                  std::size_t position, std::int64_t key, Rid rid);

// Remove the entry at position and shift the later entries down; does nothing
// when position is not below key_count. The vacated entry is zeroed.
void LeafRemoveAt(std::uint8_t* page, std::size_t position);

// Overwrite the Rid of the entry at position (below key_count).
void LeafSetRid(std::uint8_t* page, std::size_t position, Rid rid);

// The number of keys at or below key: the index of the child that covers key,
// so a key equal to a separator goes to the right child. Upper-bound binary
// search over the key array.
std::size_t InternalChildIndex(const std::uint8_t* page, std::int64_t key);

// Set the only entry of an empty internal node: one key and its two children.
void InternalSetFirstEntry(std::uint8_t* page, PageId left_child,
                           std::int64_t key, PageId right_child);

// Insert separator key at position (at most key_count) and right_child as the
// child on its right (child position + 1). Returns false and changes nothing
// when the node already holds capacity keys or position is past key_count.
bool InternalInsertAt(std::uint8_t* page, std::size_t capacity,
                      std::size_t position, std::int64_t key,
                      PageId right_child);

// Move the upper half of a full leaf into right, which FormatLeaf wrote and
// whose page id is in its header. left keeps ceil(n / 2) of its n keys and
// right gets floor(n / 2). right links to the old right sibling of left, and
// left links to right. Returns the first key of right, the separator, which
// stays in the leaf. No value, and nothing changed, when left has fewer than 2
// keys.
std::optional<std::int64_t> SplitLeaf(std::uint8_t* left, std::uint8_t* right);

// Move the upper half of a full internal node into right, which FormatInternal
// wrote with the level of left. left keeps floor((n - 1) / 2) of its n keys,
// right gets the n - floor((n - 1) / 2) - 1 keys after the middle one and the
// children that go with them, and the middle key is returned and stays in
// neither node. No value, and nothing changed, when left has fewer than 3 keys.
std::optional<std::int64_t> SplitInternal(std::uint8_t* left,
                                          std::uint8_t* right);

// Copy the level, key_count, the entry arrays and, on a leaf, the link to the
// right sibling from source into destination, which is a formatted node of
// the same kind. The page id and the owner of destination are kept.
void CopyEntries(const std::uint8_t* source, std::uint8_t* destination);

// The O(1) part of a node check, made at every fetch of a node: page type 4,
// the page id, the owner table, the three heap fields of the header at 0, the
// level (equal to expected_level, or below kMaxNodeLevel when there is none),
// key_count within the capacity of its kind, and no sibling link on an internal
// node. kCorrupt names the page and the rule.
Status CheckNodeHeader(const std::uint8_t* page, PageId page_id,
                       std::uint32_t table_id,
                       std::optional<std::uint16_t> expected_level,
                       NodeCapacity capacity);

// CheckNodeHeader, then keys that increase strictly. O(key_count).
Status CheckNode(const std::uint8_t* page, PageId page_id,
                 std::uint32_t table_id,
                 std::optional<std::uint16_t> expected_level,
                 NodeCapacity capacity);

// A copy of the bytes of one node.
using NodeBytes = std::array<std::uint8_t, kPageSize>;

// Where the tree checks read nodes from. ReadNode returns a copy of the page,
// or the error of the read.
class INodeSource {
 public:
  virtual ~INodeSource() = default;
  virtual PageResult<NodeBytes> ReadNode(PageId page_id) = 0;
};

// The seven invariants of design document 2.17, over the tree whose root is
// root. Reports the first violation as kCorrupt naming the page and the rule,
// and passes on the error of a failed read. Reads each page of the tree once.
// The minimum occupancy is not checked.
Status CheckTree(INodeSource& source, PageId root, std::uint32_t table_id,
                 NodeCapacity capacity);

// Every page of the tree, depth first, root first, in the order of a
// left-to-right walk. Fails as CheckTree does on a page reached twice, a page
// of another table or a child whose level is not one below its parent, and
// on an internal node with no key; it does not check key order.
PageResult<std::vector<PageId>> TreePages(INodeSource& source, PageId root,
                                          std::uint32_t table_id,
                                          NodeCapacity capacity);

struct NodeSummary {
  std::uint16_t level;
  PageId page_id;
  std::uint16_t key_count;
  // Absent on an empty node.
  std::optional<std::int64_t> first_key;
  std::optional<std::int64_t> last_key;
  // The right sibling of a leaf, kInvalidPageId otherwise.
  PageId next_page_id;
};

struct TreeShape {
  std::uint16_t root_level;
  // root_level + 1: the number of pages a lookup reads.
  std::size_t height;
  // Nodes and leaf keys counted by the walk.
  std::size_t node_count;
  std::size_t key_count;
  // The first node_limit nodes in breadth-first order.
  std::vector<NodeSummary> nodes;
  // False when the walk stopped at a node that failed to read, failed its
  // header check or was reached twice; the counts then cover what was read.
  bool is_complete;
};

// Describe the tree breadth first. An error only when the root cannot be read
// or fails its header check; a failure further down ends the walk with
// is_complete false.
PageResult<TreeShape> Shape(INodeSource& source, PageId root,
                            std::uint32_t table_id, NodeCapacity capacity,
                            std::size_t node_limit);

}  // namespace strata::paged

#endif  // STRATA_PAGED_BTREE_NODE_H_
