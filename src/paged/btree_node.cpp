#include "strata/paged/btree_node.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "strata/paged/byte_codec.h"

namespace strata::paged {

namespace {

std::size_t KeyPosition(std::size_t index) {
  return node_layout::kKeys + node_layout::kKeySize * index;
}
std::size_t RidPosition(std::size_t index) {
  return node_layout::kLeafRids + node_layout::kRidSize * index;
}
std::size_t ChildPosition(std::size_t index) {
  return node_layout::kInternalChildren + node_layout::kChildSize * index;
}

std::int64_t ReadKey(const std::uint8_t* page, std::size_t index) {
  return static_cast<std::int64_t>(ReadLe64(page + KeyPosition(index)));
}
void WriteKey(std::uint8_t* page, std::size_t index, std::int64_t key) {
  WriteLe64(page + KeyPosition(index), static_cast<std::uint64_t>(key));
}
Rid ReadRid(const std::uint8_t* page, std::size_t index) {
  const std::uint8_t* source = page + RidPosition(index);
  return Rid{ReadLe32(source), ReadLe16(source + 4)};
}
void WriteRid(std::uint8_t* page, std::size_t index, Rid rid) {
  std::uint8_t* destination = page + RidPosition(index);
  WriteLe32(destination, rid.page_id);
  WriteLe16(destination + 4, rid.slot_id);
}
PageId ReadChild(const std::uint8_t* page, std::size_t index) {
  return ReadLe32(page + ChildPosition(index));
}
void WriteChild(std::uint8_t* page, std::size_t index, PageId child) {
  WriteLe32(page + ChildPosition(index), child);
}

std::uint16_t LevelOf(const std::uint8_t* page) {
  return ReadLe16(page + node_layout::kLevel);
}
void SetKeyCount(std::uint8_t* page, std::size_t key_count) {
  WriteLe16(page + node_layout::kKeyCount,
            static_cast<std::uint16_t>(key_count));
}

// key_count of a page that may be damaged, cut to the layout capacity of its
// kind, so that a search or a shift never leaves the arrays.
std::size_t LeafCount(const std::uint8_t* page) {
  return std::min<std::size_t>(ReadLe16(page + node_layout::kKeyCount),
                               kLeafCapacity);
}
std::size_t InternalCount(const std::uint8_t* page) {
  return std::min<std::size_t>(ReadLe16(page + node_layout::kKeyCount),
                               kInternalCapacity);
}

void ZeroBytes(std::uint8_t* destination, std::size_t size) {
  std::memset(destination, 0, size);
}

void FormatCommon(std::uint8_t* page, PageId page_id, std::uint32_t table_id,
                  std::uint16_t level) {
  ZeroBytes(page, kPageSize);
  WriteLe32(page + layout::kPageIdField, page_id);
  page[layout::kPageTypeField] =
      static_cast<std::uint8_t>(PageType::kIndexNode);
  WriteLe32(page + layout::kNextPageId, kInvalidPageId);
  WriteLe32(page + layout::kTableId, table_id);
  WriteLe16(page + node_layout::kLevel, level);
}

PageError Corrupt(PageId page_id, const std::string& rule) {
  return MakeError(PageErrorCode::kCorrupt,
                   "page " + std::to_string(page_id) + ": index node " + rule);
}

// Move the entries [from, count) of a leaf to start at to, from > to or
// from < to alike.
void MoveLeafEntries(std::uint8_t* page, std::size_t from, std::size_t to,
                     std::size_t count) {
  std::memmove(page + KeyPosition(to), page + KeyPosition(from),
               node_layout::kKeySize * count);
  std::memmove(page + RidPosition(to), page + RidPosition(from),
               node_layout::kRidSize * count);
}

}  // namespace

bool IsValidCapacity(NodeCapacity capacity) {
  return capacity.leaf >= 2 && capacity.leaf <= kLeafCapacity &&
         capacity.internal >= 3 && capacity.internal <= kInternalCapacity;
}

PageId NodeView::page_id() const {
  return ReadLe32(page_ + layout::kPageIdField);
}
std::uint32_t NodeView::table_id() const {
  return ReadLe32(page_ + layout::kTableId);
}
PageId NodeView::next_page_id() const {
  return ReadLe32(page_ + layout::kNextPageId);
}
std::uint16_t NodeView::level() const { return LevelOf(page_); }
std::uint16_t NodeView::key_count() const {
  return ReadLe16(page_ + node_layout::kKeyCount);
}
std::int64_t NodeView::KeyAt(std::size_t index) const {
  return ReadKey(page_, index);
}
Rid NodeView::RidAt(std::size_t index) const { return ReadRid(page_, index); }
PageId NodeView::ChildAt(std::size_t index) const {
  return ReadChild(page_, index);
}

void FormatLeaf(std::uint8_t* page, PageId page_id, std::uint32_t table_id) {
  FormatCommon(page, page_id, table_id, 0);
}

void FormatInternal(std::uint8_t* page, PageId page_id, std::uint32_t table_id,
                    std::uint16_t level) {
  FormatCommon(page, page_id, table_id, level);
}

LeafPosition LeafSearch(const std::uint8_t* page, std::int64_t key) {
  const std::size_t key_count = LeafCount(page);
  std::size_t low = 0;
  std::size_t high = key_count;
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (ReadKey(page, middle) < key) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return LeafPosition{low, low < key_count && ReadKey(page, low) == key};
}

bool LeafInsertAt(std::uint8_t* page, std::size_t capacity,
                  std::size_t position, std::int64_t key, Rid rid) {
  const std::size_t key_count = LeafCount(page);
  if (key_count >= capacity || key_count >= kLeafCapacity ||
      position > key_count) {
    return false;
  }
  MoveLeafEntries(page, position, position + 1, key_count - position);
  WriteKey(page, position, key);
  WriteRid(page, position, rid);
  SetKeyCount(page, key_count + 1);
  return true;
}

void LeafRemoveAt(std::uint8_t* page, std::size_t position) {
  const std::size_t key_count = LeafCount(page);
  if (position >= key_count) {
    return;
  }
  MoveLeafEntries(page, position + 1, position, key_count - position - 1);
  ZeroBytes(page + KeyPosition(key_count - 1), node_layout::kKeySize);
  ZeroBytes(page + RidPosition(key_count - 1), node_layout::kRidSize);
  SetKeyCount(page, key_count - 1);
}

void LeafSetRid(std::uint8_t* page, std::size_t position, Rid rid) {
  if (position < LeafCount(page)) {
    WriteRid(page, position, rid);
  }
}

std::size_t InternalChildIndex(const std::uint8_t* page, std::int64_t key) {
  const std::size_t key_count = InternalCount(page);
  std::size_t low = 0;
  std::size_t high = key_count;
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (ReadKey(page, middle) <= key) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

void InternalSetFirstEntry(std::uint8_t* page, PageId left_child,
                           std::int64_t key, PageId right_child) {
  WriteKey(page, 0, key);
  WriteChild(page, 0, left_child);
  WriteChild(page, 1, right_child);
  SetKeyCount(page, 1);
}

bool InternalInsertAt(std::uint8_t* page, std::size_t capacity,
                      std::size_t position, std::int64_t key,
                      PageId right_child) {
  const std::size_t key_count = InternalCount(page);
  if (key_count >= capacity || key_count >= kInternalCapacity ||
      position > key_count) {
    return false;
  }
  std::memmove(page + KeyPosition(position + 1), page + KeyPosition(position),
               node_layout::kKeySize * (key_count - position));
  // Children position + 1 .. key_count move up by one.
  std::memmove(page + ChildPosition(position + 2),
               page + ChildPosition(position + 1),
               node_layout::kChildSize * (key_count - position));
  WriteKey(page, position, key);
  WriteChild(page, position + 1, right_child);
  SetKeyCount(page, key_count + 1);
  return true;
}

std::optional<std::int64_t> SplitLeaf(std::uint8_t* left, std::uint8_t* right) {
  const std::size_t key_count = LeafCount(left);
  if (key_count < 2) {
    return std::nullopt;
  }
  const std::size_t kept = (key_count + 1) / 2;
  const std::size_t moved = key_count - kept;
  std::memcpy(right + KeyPosition(0), left + KeyPosition(kept),
              node_layout::kKeySize * moved);
  std::memcpy(right + RidPosition(0), left + RidPosition(kept),
              node_layout::kRidSize * moved);
  SetKeyCount(right, moved);
  ZeroBytes(left + KeyPosition(kept), node_layout::kKeySize * moved);
  ZeroBytes(left + RidPosition(kept), node_layout::kRidSize * moved);
  SetKeyCount(left, kept);
  // right takes over the old link of left, and left links to right.
  WriteLe32(right + layout::kNextPageId, ReadLe32(left + layout::kNextPageId));
  WriteLe32(left + layout::kNextPageId, ReadLe32(right + layout::kPageIdField));
  return ReadKey(right, 0);
}

std::optional<std::int64_t> SplitInternal(std::uint8_t* left,
                                          std::uint8_t* right) {
  const std::size_t key_count = InternalCount(left);
  if (key_count < 3) {
    return std::nullopt;
  }
  const std::size_t kept = (key_count - 1) / 2;
  const std::size_t moved = key_count - kept - 1;
  const std::int64_t middle = ReadKey(left, kept);
  std::memcpy(right + KeyPosition(0), left + KeyPosition(kept + 1),
              node_layout::kKeySize * moved);
  std::memcpy(right + ChildPosition(0), left + ChildPosition(kept + 1),
              node_layout::kChildSize * (moved + 1));
  SetKeyCount(right, moved);
  // The middle key and the moved keys, and the moved children, leave left.
  ZeroBytes(left + KeyPosition(kept), node_layout::kKeySize * (moved + 1));
  ZeroBytes(left + ChildPosition(kept + 1),
            node_layout::kChildSize * (moved + 1));
  SetKeyCount(left, kept);
  return middle;
}

void CopyEntries(const std::uint8_t* source, std::uint8_t* destination) {
  const std::uint16_t level = LevelOf(source);
  WriteLe16(destination + node_layout::kLevel, level);
  if (level == 0) {
    const std::size_t key_count = LeafCount(source);
    std::memcpy(destination + KeyPosition(0), source + KeyPosition(0),
                node_layout::kKeySize * key_count);
    std::memcpy(destination + RidPosition(0), source + RidPosition(0),
                node_layout::kRidSize * key_count);
    SetKeyCount(destination, key_count);
    WriteLe32(destination + layout::kNextPageId,
              ReadLe32(source + layout::kNextPageId));
    return;
  }
  const std::size_t key_count = InternalCount(source);
  std::memcpy(destination + KeyPosition(0), source + KeyPosition(0),
              node_layout::kKeySize * key_count);
  std::memcpy(destination + ChildPosition(0), source + ChildPosition(0),
              node_layout::kChildSize * (key_count + 1));
  SetKeyCount(destination, key_count);
}

Status CheckNodeHeader(const std::uint8_t* page, PageId page_id,
                       std::uint32_t table_id,
                       std::optional<std::uint16_t> expected_level,
                       NodeCapacity capacity) {
  const std::uint8_t page_type = page[layout::kPageTypeField];
  if (page_type != static_cast<std::uint8_t>(PageType::kIndexNode)) {
    return Corrupt(page_id, "has page_type " + std::to_string(page_type) +
                                " instead of 4");
  }
  const PageId own_page_id = ReadLe32(page + layout::kPageIdField);
  if (own_page_id != page_id) {
    return Corrupt(page_id,
                   "carries the page id " + std::to_string(own_page_id));
  }
  const std::uint32_t owner = ReadLe32(page + layout::kTableId);
  if (owner != table_id) {
    return Corrupt(page_id, "belongs to table " + std::to_string(owner) +
                                " instead of " + std::to_string(table_id));
  }
  if (ReadLe16(page + layout::kSlotCount) != 0 ||
      ReadLe16(page + layout::kFreeEnd) != 0 ||
      ReadLe16(page + layout::kDeadBytes) != 0) {
    return Corrupt(page_id, "has a nonzero slot_count, free_end or dead_bytes");
  }
  const std::uint16_t level = LevelOf(page);
  if (expected_level.has_value()) {
    if (level != *expected_level) {
      return Corrupt(page_id, "has level " + std::to_string(level) +
                                  " instead of " +
                                  std::to_string(*expected_level));
    }
  } else if (level >= kMaxNodeLevel) {
    return Corrupt(page_id, "has level " + std::to_string(level) +
                                ", which is not below " +
                                std::to_string(kMaxNodeLevel));
  }
  const std::size_t key_count = ReadLe16(page + node_layout::kKeyCount);
  const std::size_t limit = level == 0 ? capacity.leaf : capacity.internal;
  if (key_count > limit) {
    return Corrupt(page_id, "has key_count " + std::to_string(key_count) +
                                " above its capacity " + std::to_string(limit));
  }
  if (level != 0 && ReadLe32(page + layout::kNextPageId) != kInvalidPageId) {
    return Corrupt(page_id, "is an internal node with a sibling link");
  }
  return std::nullopt;
}

Status CheckNode(const std::uint8_t* page, PageId page_id,
                 std::uint32_t table_id,
                 std::optional<std::uint16_t> expected_level,
                 NodeCapacity capacity) {
  Status header =
      CheckNodeHeader(page, page_id, table_id, expected_level, capacity);
  if (header.has_value()) {
    return header;
  }
  const std::size_t key_count = ReadLe16(page + node_layout::kKeyCount);
  for (std::size_t index = 1; index < key_count; ++index) {
    if (ReadKey(page, index - 1) >= ReadKey(page, index)) {
      return Corrupt(page_id, "has keys that do not increase strictly at " +
                                  std::to_string(index));
    }
  }
  return std::nullopt;
}

namespace {

// A depth-first walk of a tree that reads each node once, checks it and
// collects the pages and the leaf links. The recursion is as deep as the level
// of the root, which CheckNodeHeader keeps below kMaxNodeLevel.
class TreeWalker {
 public:
  TreeWalker(INodeSource& source, std::uint32_t table_id, NodeCapacity capacity,
             bool is_checking_contents)
      : source_(source),
        table_id_(table_id),
        capacity_(capacity),
        is_checking_contents_(is_checking_contents) {}

  Status Walk(PageId root) {
    return Visit(root, std::nullopt, std::nullopt, std::nullopt);
  }

  // The leaves, left to right, each name the next: the chain visits every leaf
  // once and ends at kInvalidPageId.
  Status CheckLeafChain() const {
    for (std::size_t index = 0; index < leaf_links_.size(); ++index) {
      const PageId expected = index + 1 < leaf_links_.size()
                                  ? leaf_links_[index + 1].page_id
                                  : kInvalidPageId;
      if (leaf_links_[index].next_page_id != expected) {
        return Corrupt(leaf_links_[index].page_id,
                       "has next_page_id " +
                           std::to_string(leaf_links_[index].next_page_id) +
                           " where the walk of the tree expects " +
                           std::to_string(expected));
      }
    }
    return std::nullopt;
  }

  std::vector<PageId> TakePages() { return std::move(pages_); }

 private:
  struct LeafLink {
    PageId page_id;
    PageId next_page_id;
  };

  // Every key of a node lies at or above low and below high, when they exist.
  Status CheckBounds(const NodeView& node, PageId page_id,
                     std::optional<std::int64_t> low,
                     std::optional<std::int64_t> high) const {
    if (node.key_count() == 0) {
      return std::nullopt;
    }
    if (low.has_value() && node.KeyAt(0) < *low) {
      return Corrupt(page_id, "has the key " + std::to_string(node.KeyAt(0)) +
                                  " below the separator " +
                                  std::to_string(*low) + " of its parent");
    }
    const std::int64_t last = node.KeyAt(node.key_count() - 1);
    if (high.has_value() && last >= *high) {
      return Corrupt(page_id, "has the key " + std::to_string(last) +
                                  " at or above the separator " +
                                  std::to_string(*high) + " of its parent");
    }
    return std::nullopt;
  }

  Status Visit(PageId page_id, std::optional<std::uint16_t> expected_level,
               std::optional<std::int64_t> low,
               std::optional<std::int64_t> high) {
    if (!visited_.insert(page_id).second) {
      return Corrupt(page_id, "is reached twice");
    }
    PageResult<NodeBytes> read = source_.ReadNode(page_id);
    if (!read.ok()) {
      return read.error();
    }
    const NodeBytes& bytes = read.value();
    Status status = is_checking_contents_
                        ? CheckNode(bytes.data(), page_id, table_id_,
                                    expected_level, capacity_)
                        : CheckNodeHeader(bytes.data(), page_id, table_id_,
                                          expected_level, capacity_);
    if (status.has_value()) {
      return status;
    }
    pages_.push_back(page_id);
    const NodeView node(bytes.data());
    if (is_checking_contents_) {
      Status bounds = CheckBounds(node, page_id, low, high);
      if (bounds.has_value()) {
        return bounds;
      }
    }
    if (node.is_leaf()) {
      leaf_links_.push_back(LeafLink{page_id, node.next_page_id()});
      return std::nullopt;
    }
    // Invariant 7 costs O(1), so the walk that only lists pages checks it too.
    if (node.key_count() == 0) {
      return Corrupt(page_id, "is an internal node without a key");
    }
    return VisitChildren(node, low, high);
  }

  // Child index of an internal node covers the keys from key index - 1 (or
  // the lower bound of the node) up to key index (or its upper bound).
  Status VisitChildren(const NodeView& node, std::optional<std::int64_t> low,
                       std::optional<std::int64_t> high) {
    const std::size_t key_count = node.key_count();
    const std::uint16_t child_level =
        static_cast<std::uint16_t>(node.level() - 1);
    for (std::size_t index = 0; index <= key_count; ++index) {
      const std::optional<std::int64_t> child_low =
          index == 0 ? low : std::optional<std::int64_t>(node.KeyAt(index - 1));
      const std::optional<std::int64_t> child_high =
          index == key_count ? high
                             : std::optional<std::int64_t>(node.KeyAt(index));
      Status status =
          Visit(node.ChildAt(index), child_level, child_low, child_high);
      if (status.has_value()) {
        return status;
      }
    }
    return std::nullopt;
  }

  INodeSource& source_;
  const std::uint32_t table_id_;
  const NodeCapacity capacity_;
  const bool is_checking_contents_;
  std::unordered_set<PageId> visited_;
  std::vector<PageId> pages_;
  std::vector<LeafLink> leaf_links_;
};

}  // namespace

Status CheckTree(INodeSource& source, PageId root, std::uint32_t table_id,
                 NodeCapacity capacity) {
  TreeWalker walker(source, table_id, capacity, true);
  Status walked = walker.Walk(root);
  if (walked.has_value()) {
    return walked;
  }
  return walker.CheckLeafChain();
}

PageResult<std::vector<PageId>> TreePages(INodeSource& source, PageId root,
                                          std::uint32_t table_id,
                                          NodeCapacity capacity) {
  TreeWalker walker(source, table_id, capacity, false);
  Status walked = walker.Walk(root);
  if (walked.has_value()) {
    return *walked;
  }
  return walker.TakePages();
}

namespace {

struct PendingNode {
  PageId page_id;
  std::optional<std::uint16_t> expected_level;
};

NodeSummary Summarize(const NodeView& node, PageId page_id) {
  NodeSummary summary{node.level(), page_id,      node.key_count(),
                      std::nullopt, std::nullopt, kInvalidPageId};
  if (node.key_count() > 0) {
    summary.first_key = node.KeyAt(0);
    summary.last_key = node.KeyAt(node.key_count() - 1);
  }
  if (node.is_leaf()) {
    summary.next_page_id = node.next_page_id();
  }
  return summary;
}

}  // namespace

PageResult<TreeShape> Shape(INodeSource& source, PageId root,
                            std::uint32_t table_id, NodeCapacity capacity,
                            std::size_t node_limit) {
  TreeShape shape{0, 0, 0, 0, {}, true};
  std::deque<PendingNode> pending;
  pending.push_back(PendingNode{root, std::nullopt});
  std::unordered_set<PageId> visited;
  while (!pending.empty()) {
    const PendingNode current = pending.front();
    pending.pop_front();
    const bool is_root = current.page_id == root && shape.node_count == 0;
    if (!visited.insert(current.page_id).second) {
      shape.is_complete = false;
      break;
    }
    PageResult<NodeBytes> read = source.ReadNode(current.page_id);
    if (!read.ok()) {
      if (is_root) {
        return read.error();
      }
      shape.is_complete = false;
      break;
    }
    const NodeBytes& bytes = read.value();
    Status status = CheckNodeHeader(bytes.data(), current.page_id, table_id,
                                    current.expected_level, capacity);
    if (status.has_value()) {
      if (is_root) {
        return *status;
      }
      shape.is_complete = false;
      break;
    }
    const NodeView node(bytes.data());
    if (is_root) {
      shape.root_level = node.level();
      shape.height = static_cast<std::size_t>(node.level()) + 1;
    }
    ++shape.node_count;
    if (shape.nodes.size() < node_limit) {
      shape.nodes.push_back(Summarize(node, current.page_id));
    }
    if (node.is_leaf()) {
      shape.key_count += node.key_count();
      continue;
    }
    const std::uint16_t child_level =
        static_cast<std::uint16_t>(node.level() - 1);
    for (std::size_t index = 0; index <= node.key_count(); ++index) {
      pending.push_back(PendingNode{node.ChildAt(index), child_level});
    }
  }
  return shape;
}

}  // namespace strata::paged
