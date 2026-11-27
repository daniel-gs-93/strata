#include "strata/paged/btree.h"

#include <cstring>
#include <string>
#include <utility>

#include "strata/paged/byte_codec.h"

namespace strata::paged {

namespace {

PageError Corrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, detail);
}

// A node that a child pointer or a sibling link names must be inside the file.
// The store reports a page past the end as kNotFound, which callers read as an
// absent key or an unknown table, so a pointer that leaves the file is
// reported as the damage that it is.
PageError PointsOutsideFile(PageId page_id) {
  return Corrupt("page " + std::to_string(page_id) +
                 ": a pointer of the index leads past the end of the file");
}

// Reads nodes of a tree under a shared latch, one at a time, and hands back a
// copy, so the tree checks never hold a latch while they work.
class PageNodeSource : public INodeSource {
 public:
  explicit PageNodeSource(BufferManager& buffer) : buffer_(buffer) {}

  PageResult<NodeBytes> ReadNode(PageId page_id) override {
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(page_id, LatchMode::kShared);
    if (!fetched.ok()) {
      if (fetched.error().code == PageErrorCode::kNotFound) {
        return PointsOutsideFile(page_id);
      }
      return fetched.error();
    }
    NodeBytes copy;
    std::memcpy(copy.data(), fetched.value().data(), kPageSize);
    return copy;
  }

 private:
  BufferManager& buffer_;
};

bool IsFull(const std::uint8_t* page, NodeCapacity capacity) {
  const NodeView node(page);
  const std::size_t limit = node.is_leaf() ? capacity.leaf : capacity.internal;
  return node.key_count() >= limit;
}

// Formats an empty node of the given level: a leaf for level 0.
void FormatEmptyNode(std::uint8_t* page, PageId page_id, std::uint32_t table_id,
                     std::uint16_t level) {
  if (level == 0) {
    FormatLeaf(page, page_id, table_id);
  } else {
    FormatInternal(page, page_id, table_id, level);
  }
}

NodeCapacity CapacityOf(BTreeOptions options) {
  return NodeCapacity{options.leaf_capacity, options.internal_capacity};
}

std::string InvalidCapacityMessage(BTreeOptions options) {
  return "a B+Tree needs a leaf capacity from 2 to " +
         std::to_string(kLeafCapacity) +
         " and an internal capacity from 3 to " +
         std::to_string(kInternalCapacity) + ", not " +
         std::to_string(options.leaf_capacity) + " and " +
         std::to_string(options.internal_capacity);
}

}  // namespace

PageResult<BTree> BTree::Create(BufferManager& buffer, PageAllocator& allocator,
                                std::uint32_t table_id, BTreeOptions options) {
  const NodeCapacity capacity = CapacityOf(options);
  if (!IsValidCapacity(capacity)) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     InvalidCapacityMessage(options));
  }
  PageResult<PageGuard> allocated = allocator.Allocate();
  if (!allocated.ok()) {
    return allocated.error();
  }
  PageGuard root = std::move(allocated).value();
  FormatLeaf(root.mutable_data(), root.page_id(), table_id);
  root.MarkDirty();
  return BTree(buffer, allocator, table_id, root.page_id(), capacity);
}

PageResult<BTree> BTree::Open(BufferManager& buffer, PageAllocator& allocator,
                              std::uint32_t table_id, PageId root,
                              BTreeOptions options) {
  const NodeCapacity capacity = CapacityOf(options);
  if (!IsValidCapacity(capacity)) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     InvalidCapacityMessage(options));
  }
  const BTree tree(buffer, allocator, table_id, root, capacity);
  PageResult<PageGuard> fetched =
      tree.FetchNode(root, LatchMode::kShared, std::nullopt);
  if (!fetched.ok()) {
    return fetched.error();
  }
  return tree;
}

PageResult<PageGuard> BTree::FetchNode(
    PageId page_id, LatchMode mode, std::optional<std::uint16_t> level) const {
  PageResult<PageGuard> fetched = buffer_.FetchPage(page_id, mode);
  if (!fetched.ok()) {
    // A node reached through a pointer has an expected level. The root, which
    // the caller names, keeps the error of the store (Open of a page past the
    // end of the file is kNotFound).
    if (level.has_value() && fetched.error().code == PageErrorCode::kNotFound) {
      return PointsOutsideFile(page_id);
    }
    return fetched.error();
  }
  const Status status = CheckNodeHeader(fetched.value().data(), page_id,
                                        table_id_, level, capacity_);
  if (status.has_value()) {
    return *status;
  }
  return fetched;
}

PageResult<PageGuard> BTree::DescendShared(
    std::optional<std::int64_t> key,
    std::optional<std::int64_t>* upper_fence) const {
  if (upper_fence != nullptr) {
    upper_fence->reset();
  }
  PageResult<PageGuard> fetched =
      FetchNode(root_, LatchMode::kShared, std::nullopt);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard current = std::move(fetched).value();
  while (true) {
    const NodeView node(current.data());
    if (node.is_leaf()) {
      return PageResult<PageGuard>(std::move(current));
    }
    const std::size_t child_index =
        key.has_value() ? InternalChildIndex(current.data(), *key) : 0;
    // The separator after the chosen child bounds it from above. A deeper
    // separator is smaller than one above it, so the last one taken is the
    // tightest; the rightmost child keeps the bound of its parent.
    if (upper_fence != nullptr && child_index < node.key_count()) {
      *upper_fence = node.KeyAt(child_index);
    }
    const PageId child_id = node.ChildAt(child_index);
    const std::uint16_t child_level =
        static_cast<std::uint16_t>(node.level() - 1);
    PageResult<PageGuard> child =
        FetchNode(child_id, LatchMode::kShared, child_level);
    if (!child.ok()) {
      return child.error();
    }
    // The child is latched, so the move releases the parent.
    current = std::move(child).value();
  }
}

PageResult<PageGuard> BTree::DescendForWrite(std::int64_t key) const {
  PageResult<PageGuard> fetched =
      FetchNode(root_, LatchMode::kShared, std::nullopt);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard current = std::move(fetched).value();
  if (NodeView(current.data()).is_leaf()) {
    // The root is the leaf. It is read as level 0 under the shared latch and
    // latched again exclusively; nothing can split it in between, because the
    // tree has one writer at a time.
    current.Release();
    return FetchNode(root_, LatchMode::kExclusive, 0);
  }
  while (true) {
    const NodeView node(current.data());
    const std::size_t child_index = InternalChildIndex(current.data(), key);
    const PageId child_id = node.ChildAt(child_index);
    const std::uint16_t child_level =
        static_cast<std::uint16_t>(node.level() - 1);
    const LatchMode mode =
        child_level == 0 ? LatchMode::kExclusive : LatchMode::kShared;
    PageResult<PageGuard> child = FetchNode(child_id, mode, child_level);
    if (!child.ok()) {
      return child.error();
    }
    current = std::move(child).value();
    if (child_level == 0) {
      return PageResult<PageGuard>(std::move(current));
    }
  }
}

PageResult<std::optional<Rid>> BTree::Find(std::int64_t key) const {
  PageResult<PageGuard> descended = DescendShared(key);
  if (!descended.ok()) {
    return descended.error();
  }
  const PageGuard& leaf = descended.value();
  const LeafPosition position = LeafSearch(leaf.data(), key);
  if (!position.is_found) {
    return std::optional<Rid>();
  }
  return std::optional<Rid>(NodeView(leaf.data()).RidAt(position.index));
}

Status BTree::Delete(std::int64_t key) {
  PageResult<PageGuard> descended = DescendForWrite(key);
  if (!descended.ok()) {
    return descended.error();
  }
  PageGuard leaf = std::move(descended).value();
  const LeafPosition position = LeafSearch(leaf.data(), key);
  if (!position.is_found) {
    return MakeError(PageErrorCode::kNotFound,
                     "the index of table " + std::to_string(table_id_) +
                         " has no key " + std::to_string(key));
  }
  LeafRemoveAt(leaf.mutable_data(), position.index);
  leaf.MarkDirty();
  return std::nullopt;
}

Status BTree::Replace(std::int64_t key, Rid rid) {
  PageResult<PageGuard> descended = DescendForWrite(key);
  if (!descended.ok()) {
    return descended.error();
  }
  PageGuard leaf = std::move(descended).value();
  const LeafPosition position = LeafSearch(leaf.data(), key);
  if (!position.is_found) {
    return MakeError(PageErrorCode::kNotFound,
                     "the index of table " + std::to_string(table_id_) +
                         " has no key " + std::to_string(key));
  }
  LeafSetRid(leaf.mutable_data(), position.index, rid);
  leaf.MarkDirty();
  return std::nullopt;
}

PageResult<PageId> BTree::AllocateEmptyNode(std::uint16_t level) {
  PageResult<PageGuard> allocated = allocator_.Allocate();
  if (!allocated.ok()) {
    return allocated.error();
  }
  PageGuard page = std::move(allocated).value();
  FormatEmptyNode(page.mutable_data(), page.page_id(), table_id_, level);
  page.MarkDirty();
  return page.page_id();
}

PageResult<PageGuard> BTree::SplitRoot(PageGuard root, std::int64_t key) {
  const std::uint16_t level = NodeView(root.data()).level();
  // Two new pages, one Allocate at a time, so that no new page is held while
  // Allocate runs again. The root is untouched until both are latched.
  PageResult<PageId> left_page = AllocateEmptyNode(level);
  if (!left_page.ok()) {
    return left_page.error();
  }
  PageResult<PageId> right_page = AllocateEmptyNode(level);
  if (!right_page.ok()) {
    // Best effort: the error of the allocation is the one to report.
    allocator_.ReleasePages({left_page.value()});
    return right_page.error();
  }
  const PageId left_id = left_page.value();
  const PageId right_id = right_page.value();
  PageResult<PageGuard> left_fetched =
      FetchNode(left_id, LatchMode::kExclusive, level);
  if (!left_fetched.ok()) {
    allocator_.ReleasePages({left_id, right_id});
    return left_fetched.error();
  }
  PageGuard left = std::move(left_fetched).value();
  PageResult<PageGuard> right_fetched =
      FetchNode(right_id, LatchMode::kExclusive, level);
  if (!right_fetched.ok()) {
    left.Release();
    allocator_.ReleasePages({left_id, right_id});
    return right_fetched.error();
  }
  PageGuard right = std::move(right_fetched).value();

  // The root, the left page and the right page are latched, and nothing below
  // can fail. The upper half of the root goes to the right page, what is left
  // goes to the left page, and the root becomes an internal node of one key.
  std::uint8_t* const root_bytes = root.mutable_data();
  const std::optional<std::int64_t> separator =
      level == 0 ? SplitLeaf(root_bytes, right.mutable_data())
                 : SplitInternal(root_bytes, right.mutable_data());
  if (!separator.has_value()) {
    // The root is full, so it has at least 2 keys (3 on an internal node).
    left.Release();
    right.Release();
    allocator_.ReleasePages({left_id, right_id});
    return Corrupt("the root of the index of table " +
                   std::to_string(table_id_) + " is full but cannot be split");
  }
  CopyEntries(root_bytes, left.mutable_data());
  // The log sequence number of the root page is not index state: keep it.
  const std::uint64_t lsn = ReadLe64(root_bytes + layout::kLsn);
  FormatInternal(root_bytes, root_, table_id_,
                 static_cast<std::uint16_t>(level + 1));
  WriteLe64(root_bytes + layout::kLsn, lsn);
  InternalSetFirstEntry(root_bytes, left_id, *separator, right_id);
  root.MarkDirty();
  left.MarkDirty();
  right.MarkDirty();

  // Continue in the half that the key enters; the root and the other half are
  // released when this function returns.
  if (InternalChildIndex(root.data(), key) == 0) {
    return PageResult<PageGuard>(std::move(left));
  }
  return PageResult<PageGuard>(std::move(right));
}

PageResult<PageGuard> BTree::SplitChild(PageGuard parent,
                                        std::size_t child_index,
                                        PageGuard child, std::int64_t key) {
  // The parent has room, because every full node on the path was split on the
  // way down. Check it before anything changes.
  if (IsFull(parent.data(), capacity_)) {
    return Corrupt("page " + std::to_string(parent.page_id()) +
                   " is full although its child is being split");
  }
  const std::uint16_t level = NodeView(child.data()).level();
  PageResult<PageGuard> allocated = allocator_.Allocate();
  if (!allocated.ok()) {
    return allocated.error();
  }
  PageGuard sibling = std::move(allocated).value();
  FormatEmptyNode(sibling.mutable_data(), sibling.page_id(), table_id_, level);
  const std::optional<std::int64_t> separator =
      level == 0 ? SplitLeaf(child.mutable_data(), sibling.mutable_data())
                 : SplitInternal(child.mutable_data(), sibling.mutable_data());
  if (!separator.has_value()) {
    return Corrupt("page " + std::to_string(child.page_id()) +
                   " is full but cannot be split");
  }
  InternalInsertAt(parent.mutable_data(), capacity_.internal, child_index,
                   *separator, sibling.page_id());
  parent.MarkDirty();
  child.MarkDirty();
  sibling.MarkDirty();
  // A key equal to the separator belongs to the right node, as in the parent.
  if (key >= *separator) {
    return PageResult<PageGuard>(std::move(sibling));
  }
  return PageResult<PageGuard>(std::move(child));
}

Status BTree::InsertIntoLeaf(PageGuard& leaf, std::int64_t key, Rid rid) {
  const LeafPosition position = LeafSearch(leaf.data(), key);
  if (position.is_found) {
    return MakeError(PageErrorCode::kAlreadyExists,
                     "the index of table " + std::to_string(table_id_) +
                         " already has key " + std::to_string(key));
  }
  if (!LeafInsertAt(leaf.mutable_data(), capacity_.leaf, position.index, key,
                    rid)) {
    return Corrupt("page " + std::to_string(leaf.page_id()) +
                   " is full although it was not split");
  }
  leaf.MarkDirty();
  return std::nullopt;
}

Status BTree::Insert(std::int64_t key, Rid rid) {
  PageResult<PageGuard> fetched =
      FetchNode(root_, LatchMode::kExclusive, std::nullopt);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard current = std::move(fetched).value();
  if (IsFull(current.data(), capacity_)) {
    PageResult<PageGuard> half = SplitRoot(std::move(current), key);
    if (!half.ok()) {
      return half.error();
    }
    current = std::move(half).value();
  }
  while (true) {
    const NodeView node(current.data());
    if (node.is_leaf()) {
      return InsertIntoLeaf(current, key, rid);
    }
    const std::size_t child_index = InternalChildIndex(current.data(), key);
    const PageId child_id = node.ChildAt(child_index);
    const std::uint16_t child_level =
        static_cast<std::uint16_t>(node.level() - 1);
    PageResult<PageGuard> fetched_child =
        FetchNode(child_id, LatchMode::kExclusive, child_level);
    if (!fetched_child.ok()) {
      return fetched_child.error();
    }
    PageGuard child = std::move(fetched_child).value();
    if (IsFull(child.data(), capacity_)) {
      PageResult<PageGuard> half =
          SplitChild(std::move(current), child_index, std::move(child), key);
      if (!half.ok()) {
        return half.error();
      }
      current = std::move(half).value();
    } else {
      // The child is latched, so the move releases the parent.
      current = std::move(child);
    }
  }
}

BTreeCursor BTree::Scan(const KeyRange& range) const {
  return BTreeCursor(*this, range);
}

PageResult<std::vector<PageId>> BTree::Pages() const {
  PageNodeSource source(buffer_);
  return TreePages(source, root_, table_id_, capacity_);
}

Status BTree::CheckInvariants() const {
  PageNodeSource source(buffer_);
  return CheckTree(source, root_, table_id_, capacity_);
}

PageResult<TreeShape> BTree::Shape(std::size_t node_limit) const {
  PageNodeSource source(buffer_);
  return ::strata::paged::Shape(source, root_, table_id_, capacity_,
                                node_limit);
}

BTreeCursor::BTreeCursor(const BTree& tree, const KeyRange& range)
    : tree_(tree),
      first_key_(range.FirstKey()),
      last_key_(range.LastKey()),
      is_finished_(range.IsEmpty()) {}

Status BTreeCursor::FillBuffer() {
  buffer_.clear();
  position_ = 0;
  // Only the leaf that the descent reaches has a known upper bound; a leaf
  // reached through next_page_id has none.
  std::optional<std::int64_t> upper_fence;
  PageResult<PageGuard> fetched =
      is_started_ ? tree_.FetchNode(next_leaf_, LatchMode::kShared, 0)
                  : tree_.DescendShared(first_key_, &upper_fence);
  if (!fetched.ok()) {
    return fetched.error();
  }
  PageGuard leaf = std::move(fetched).value();
  ++leaves_read_;
  if (leaves_read_ > tree_.buffer_.PageCount()) {
    return Corrupt("the leaf chain of the index of table " +
                   std::to_string(tree_.table_id_) +
                   " is longer than the file");
  }
  const NodeView node(leaf.data());
  std::size_t start = 0;
  if (!is_started_ && first_key_.has_value()) {
    start = LeafSearch(leaf.data(), *first_key_).index;
  }
  is_started_ = true;
  for (std::size_t index = start; index < node.key_count(); ++index) {
    const std::int64_t key = node.KeyAt(index);
    if (last_key_.has_value() && key > *last_key_) {
      is_finished_ = true;
      return std::nullopt;
    }
    buffer_.push_back(BTreeEntry{key, node.RidAt(index)});
    // Keys are unique: nothing after the last key of the range can qualify.
    if (last_key_.has_value() && key == *last_key_) {
      is_finished_ = true;
      return std::nullopt;
    }
  }
  // Every key of a later leaf is at or above the separator that bounds this
  // leaf. A range that ends below it has nothing left to read, however many
  // empty leaves a delete left in the chain after this one.
  if (upper_fence.has_value() && last_key_.has_value() &&
      *last_key_ < *upper_fence) {
    is_finished_ = true;
    return std::nullopt;
  }
  next_leaf_ = node.next_page_id();
  if (next_leaf_ == kInvalidPageId) {
    is_finished_ = true;
  }
  return std::nullopt;
}

PageResult<std::optional<BTreeEntry>> BTreeCursor::Next() {
  while (position_ >= buffer_.size()) {
    if (is_finished_) {
      return std::optional<BTreeEntry>();
    }
    const Status status = FillBuffer();
    if (status.has_value()) {
      is_finished_ = true;
      buffer_.clear();
      position_ = 0;
      return *status;
    }
  }
  return std::optional<BTreeEntry>(buffer_[position_++]);
}

}  // namespace strata::paged
