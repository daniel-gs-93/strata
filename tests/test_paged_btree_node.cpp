#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "strata/paged/btree_node.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_support.h"

// The node functions and the tree checks of the B+Tree: pure, over page bytes
// and a fake node source.

namespace {

using strata::paged::CheckNode;
using strata::paged::CheckNodeHeader;
using strata::paged::CheckTree;
using strata::paged::FormatInternal;
using strata::paged::FormatLeaf;
using strata::paged::InternalChildIndex;
using strata::paged::InternalInsertAt;
using strata::paged::InternalSetFirstEntry;
using strata::paged::kInternalCapacity;
using strata::paged::kInvalidPageId;
using strata::paged::kLeafCapacity;
using strata::paged::LeafInsertAt;
using strata::paged::LeafRemoveAt;
using strata::paged::LeafSearch;
using strata::paged::NodeBytes;
using strata::paged::NodeCapacity;
using strata::paged::NodeView;
using strata::paged::PageErrorCode;
using strata::paged::PageId;
using strata::paged::PageResult;
using strata::paged::ReadLe16;
using strata::paged::ReadLe32;
using strata::paged::ReadLe64;
using strata::paged::Rid;
using strata::paged::SlottedPage;
using strata::paged::SplitInternal;
using strata::paged::SplitLeaf;
using strata::paged::WriteLe16;
using strata::paged::WriteLe32;
using strata::testing::Page;

constexpr std::uint32_t kTableId = 3;
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

Rid RidOf(std::int64_t key) {
  const std::uint64_t bits = static_cast<std::uint64_t>(key);
  return Rid{static_cast<PageId>(bits % 100000u) + 3u,
             static_cast<std::uint16_t>(bits % 50u)};
}

Page LeafPage(PageId page_id, const std::vector<std::int64_t>& keys,
              PageId next_page_id = kInvalidPageId,
              std::uint32_t table_id = kTableId) {
  Page page{};
  FormatLeaf(page.data(), page_id, table_id);
  for (std::size_t index = 0; index < keys.size(); ++index) {
    CHECK(LeafInsertAt(page.data(), kLeafCapacity, index, keys[index],
                       RidOf(keys[index])));
  }
  WriteLe32(page.data() + strata::paged::layout::kNextPageId, next_page_id);
  return page;
}

// children.size() is keys.size() + 1.
Page InternalPage(PageId page_id, std::uint16_t level,
                  const std::vector<std::int64_t>& keys,
                  const std::vector<PageId>& children) {
  Page page{};
  FormatInternal(page.data(), page_id, kTableId, level);
  InternalSetFirstEntry(page.data(), children[0], keys[0], children[1]);
  for (std::size_t index = 1; index < keys.size(); ++index) {
    CHECK(InternalInsertAt(page.data(), kInternalCapacity, index, keys[index],
                           children[index + 1]));
  }
  return page;
}

void SetField16(Page& page, std::size_t offset, std::uint16_t value) {
  WriteLe16(page.data() + offset, value);
}
void SetField32(Page& page, std::size_t offset, std::uint32_t value) {
  WriteLe32(page.data() + offset, value);
}

std::vector<std::int64_t> KeysOf(const std::uint8_t* page) {
  const NodeView node(page);
  std::vector<std::int64_t> keys;
  for (std::size_t index = 0; index < node.key_count(); ++index) {
    keys.push_back(node.KeyAt(index));
  }
  return keys;
}

std::vector<Rid> RidsOf(const std::uint8_t* page) {
  const NodeView node(page);
  std::vector<Rid> rids;
  for (std::size_t index = 0; index < node.key_count(); ++index) {
    rids.push_back(node.RidAt(index));
  }
  return rids;
}

std::vector<PageId> ChildrenOf(const std::uint8_t* page) {
  const NodeView node(page);
  std::vector<PageId> children;
  for (std::size_t index = 0; index <= node.key_count(); ++index) {
    children.push_back(node.ChildAt(index));
  }
  return children;
}

// A full leaf of `count` keys 10, 20, ... with their Rids.
Page FullLeaf(PageId page_id, std::size_t count, PageId next_page_id) {
  std::vector<std::int64_t> keys;
  for (std::size_t index = 0; index < count; ++index) {
    keys.push_back(static_cast<std::int64_t>(10 * (index + 1)));
  }
  return LeafPage(page_id, keys, next_page_id);
}

// A full internal node of `count` keys 10, 20, ... and count + 1 children
// 1000, 1001, ...
Page FullInternal(PageId page_id, std::size_t count) {
  std::vector<std::int64_t> keys;
  std::vector<PageId> children;
  for (std::size_t index = 0; index < count; ++index) {
    keys.push_back(static_cast<std::int64_t>(10 * (index + 1)));
  }
  for (std::size_t index = 0; index <= count; ++index) {
    children.push_back(static_cast<PageId>(1000 + index));
  }
  return InternalPage(page_id, 1, keys, children);
}

// Nodes in a map, read by page id; counts the reads of each page.
class MemorySource : public strata::paged::INodeSource {
 public:
  void Put(PageId page_id, const Page& page) { pages_[page_id] = page; }
  Page& At(PageId page_id) { return pages_.at(page_id); }

  PageResult<NodeBytes> ReadNode(PageId page_id) override {
    ++read_counts_[page_id];
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
      return strata::paged::MakeError(PageErrorCode::kNotFound,
                                      "no page " + std::to_string(page_id));
    }
    return found->second;
  }

  int MostReadsOfOnePage() const {
    int most = 0;
    for (const auto& entry : read_counts_) {
      most = std::max(most, entry.second);
    }
    return most;
  }

 private:
  std::map<PageId, Page> pages_;
  std::map<PageId, int> read_counts_;
};

constexpr NodeCapacity kSmall{3, 4};

// A leaf of three keys: page 10.
MemorySource TreeOfOneLeaf() {
  MemorySource source;
  source.Put(10, LeafPage(10, {1, 2, 3}));
  return source;
}

// Height 2: root 20 with keys 10 and 20 over the leaves 21, 22 and 23.
MemorySource TreeOfHeightTwo() {
  MemorySource source;
  source.Put(20, InternalPage(20, 1, {10, 20}, {21, 22, 23}));
  source.Put(21, LeafPage(21, {1, 5, 9}, 22));
  source.Put(22, LeafPage(22, {10, 15}, 23));
  source.Put(23, LeafPage(23, {20, 30, 40}));
  return source;
}

// Height 3 with one empty leaf: root 30 over the internal nodes 31 and 34.
MemorySource TreeOfHeightThree() {
  MemorySource source;
  source.Put(30, InternalPage(30, 2, {100}, {31, 34}));
  source.Put(31, InternalPage(31, 1, {50}, {32, 33}));
  source.Put(32, LeafPage(32, {10, 20}, 33));
  source.Put(33, LeafPage(33, {}, 35));
  source.Put(34, InternalPage(34, 1, {150}, {35, 36}));
  source.Put(35, LeafPage(35, {100, 110}, 36));
  source.Put(36, LeafPage(36, {150, 160}));
  return source;
}

bool IsCorruptNaming(const strata::paged::Status& status,
                     const std::string& page, const std::string& rule) {
  if (!status.has_value() || status->code != PageErrorCode::kCorrupt) {
    return false;
  }
  return status->message.find("page " + page + ":") != std::string::npos &&
         status->message.find(rule) != std::string::npos;
}

// A leaf and the map that says what it must hold. Everything outside the key
// array, the Rid array and key_count stays as the format left it.
class LeafFixture {
 public:
  LeafFixture() {
    FormatLeaf(page_.data(), 7, kTableId);
    formatted_ = page_;
  }

  Page& page() { return page_; }

  void Insert(std::int64_t key) {
    const strata::paged::LeafPosition position = LeafSearch(page_.data(), key);
    CHECK(!position.is_found);
    CHECK(LeafInsertAt(page_.data(), kLeafCapacity, position.index, key,
                       RidOf(key)));
    expected_[key] = RidOf(key);
  }

  void RemoveAt(std::size_t position) {
    const std::int64_t key = NodeView(page_.data()).KeyAt(position);
    LeafRemoveAt(page_.data(), position);
    expected_.erase(key);
    CHECK(!LeafSearch(page_.data(), key).is_found);
  }

  bool IsOutsideArraysUnchanged() const {
    const bool is_header_same =
        std::equal(page_.begin(), page_.begin() + 34, formatted_.begin()) &&
        std::equal(page_.begin() + 36, page_.begin() + 48,
                   formatted_.begin() + 36);
    const bool is_tail_same = std::equal(page_.begin() + 4094, page_.end(),
                                         formatted_.begin() + 4094);
    return is_header_same && is_tail_same;
  }

  bool IsAsExpected() const {
    std::vector<std::int64_t> expected_keys;
    std::vector<Rid> expected_rids;
    for (const auto& entry : expected_) {
      expected_keys.push_back(entry.first);
      expected_rids.push_back(entry.second);
    }
    return KeysOf(page_.data()) == expected_keys &&
           RidsOf(page_.data()) == expected_rids &&
           NodeView(page_.data()).key_count() == expected_.size();
  }

 private:
  Page page_{};
  Page formatted_{};
  std::map<std::int64_t, Rid> expected_;
};

// One damaged node of the node-check test.
struct DamagedNode {
  const char* name;
  Page page;
  std::uint16_t expected_level;
  const char* rule;
};

void AddDamagedNode(std::vector<DamagedNode>& cases, const char* name,
                    const Page& page, const char* rule,
                    std::uint16_t expected_level) {
  cases.push_back(DamagedNode{name, page, expected_level, rule});
}

}  // namespace

TEST(test_paged_btree_node_layout_fills_the_page) {
  CHECK_EQ(kLeafCapacity, std::size_t{289});
  CHECK_EQ(kInternalCapacity, std::size_t{337});
  CHECK_EQ(strata::paged::kLeafMinKeys, std::size_t{144});
  CHECK_EQ(strata::paged::kInternalMinKeys, std::size_t{168});
  namespace node_layout = strata::paged::node_layout;
  CHECK_EQ(node_layout::kKeys + 8 * kLeafCapacity, std::size_t{2360});
  CHECK_EQ(node_layout::kLeafRids + 6 * kLeafCapacity, std::size_t{4094});
  CHECK_EQ(node_layout::kInternalChildren + 4 * (kInternalCapacity + 1),
           std::size_t{4096});
  CHECK_EQ(node_layout::kKeys + 8 * kInternalCapacity,
           node_layout::kInternalChildren);
  CHECK_EQ(node_layout::kKeys % 8, std::size_t{0});

  // A full leaf: every key and Rid is where the layout says.
  Page leaf = FullLeaf(7, kLeafCapacity, kInvalidPageId);
  CHECK_EQ(ReadLe16(leaf.data() + 34), std::uint16_t{289});
  for (std::size_t index = 0; index < kLeafCapacity; ++index) {
    const std::size_t key_offset = 48 + 8 * index;
    CHECK_EQ(key_offset % 8, std::size_t{0});
    CHECK_EQ(static_cast<std::int64_t>(ReadLe64(leaf.data() + key_offset)),
             static_cast<std::int64_t>(10 * (index + 1)));
    const std::int64_t key = static_cast<std::int64_t>(10 * (index + 1));
    CHECK_EQ(ReadLe32(leaf.data() + 2360 + 6 * index), RidOf(key).page_id);
    CHECK_EQ(ReadLe16(leaf.data() + 2360 + 6 * index + 4), RidOf(key).slot_id);
  }
  // A full internal node: the last child ends at the end of the page.
  Page internal = FullInternal(8, kInternalCapacity);
  CHECK_EQ(ReadLe16(internal.data() + 32), std::uint16_t{1});
  CHECK_EQ(ReadLe16(internal.data() + 34), std::uint16_t{337});
  CHECK_EQ(ReadLe32(internal.data() + 2744 + 4 * kInternalCapacity),
           std::uint32_t{1000 + 337});
  CHECK_EQ(ReadLe64(internal.data() + 48 + 8 * 336), std::uint64_t{3370});

  // VerifyHeader accepts both pages once the checksum is written.
  Page empty_leaf{};
  FormatLeaf(empty_leaf.data(), 7, kTableId);
  SlottedPage::SealChecksum(empty_leaf.data());
  CHECK_OK(SlottedPage::VerifyHeader(empty_leaf.data(), 7));
  Page empty_internal{};
  FormatInternal(empty_internal.data(), 8, kTableId, 1);
  SlottedPage::SealChecksum(empty_internal.data());
  CHECK_OK(SlottedPage::VerifyHeader(empty_internal.data(), 8));

  // Type 5 is refused; type 4 with a heap field set is refused, one run each.
  Page unknown_type = empty_leaf;
  unknown_type[8] = 5;
  SlottedPage::SealChecksum(unknown_type.data());
  CHECK_ERROR(SlottedPage::VerifyHeader(unknown_type.data(), 7), kCorrupt);
  CHECK_MESSAGE_CONTAINS(SlottedPage::VerifyHeader(unknown_type.data(), 7),
                         "page_type 5");
  const std::size_t heap_fields[] = {strata::paged::layout::kSlotCount,
                                     strata::paged::layout::kFreeEnd,
                                     strata::paged::layout::kDeadBytes};
  for (std::size_t offset : heap_fields) {
    Page damaged = empty_leaf;
    WriteLe16(damaged.data() + offset, 1);
    SlottedPage::SealChecksum(damaged.data());
    CHECK_ERROR(SlottedPage::VerifyHeader(damaged.data(), 7), kCorrupt);
  }
}

TEST(test_paged_btree_node_leaf_search_returns_position_and_presence) {
  struct Case {
    std::vector<std::int64_t> keys;
    std::vector<std::int64_t> probes;
  };
  std::vector<std::int64_t> full_keys;
  std::vector<std::int64_t> full_probes = {kInt64Min, kInt64Max, -1, 0, 1};
  for (std::size_t index = 0; index < kLeafCapacity; ++index) {
    full_keys.push_back(static_cast<std::int64_t>(10 * (index + 1)));
    full_probes.push_back(static_cast<std::int64_t>(10 * (index + 1)));
    full_probes.push_back(static_cast<std::int64_t>(10 * (index + 1)) - 5);
    full_probes.push_back(static_cast<std::int64_t>(10 * (index + 1)) + 5);
  }
  const std::vector<Case> cases = {
      {{}, {kInt64Min, 0, kInt64Max}},
      {{-5, 10, 20}, {kInt64Min, -6, -5, -4, 9, 10, 11, 19, 20, 21, kInt64Max}},
      {{kInt64Min, 0, kInt64Max}, {kInt64Min, -1, 0, 1, kInt64Max}},
      {full_keys, full_probes}};
  for (const Case& test_case : cases) {
    const Page page = LeafPage(7, test_case.keys);
    for (std::int64_t probe : test_case.probes) {
      const auto lower =
          std::lower_bound(test_case.keys.begin(), test_case.keys.end(), probe);
      const std::size_t expected_index =
          static_cast<std::size_t>(lower - test_case.keys.begin());
      const bool is_present = lower != test_case.keys.end() && *lower == probe;
      const strata::paged::LeafPosition found = LeafSearch(page.data(), probe);
      CHECK_EQ(found.index, expected_index);
      CHECK_EQ(found.is_found, is_present);
    }
  }
}

TEST(test_paged_btree_node_leaf_insert_and_remove_keep_order_and_rids) {
  std::vector<std::int64_t> keys;
  for (std::size_t index = 0; index < kLeafCapacity; ++index) {
    keys.push_back(static_cast<std::int64_t>(7 * index) - 100);
  }
  std::mt19937 random(12345);
  std::shuffle(keys.begin(), keys.end(), random);

  LeafFixture leaf;
  for (std::int64_t key : keys) {
    leaf.Insert(key);
    CHECK(leaf.IsAsExpected());
    CHECK(leaf.IsOutsideArraysUnchanged());
  }

  // A full leaf refuses one more insert and stays as it is.
  const Page full = leaf.page();
  CHECK(
      !LeafInsertAt(leaf.page().data(), kLeafCapacity, 0, kInt64Min, RidOf(1)));
  CHECK(leaf.page() == full);

  // Remove from the front, the middle and the end.
  const std::size_t removals[] = {0, 100, 286};
  for (std::size_t position : removals) {
    leaf.RemoveAt(position);
    CHECK(leaf.IsAsExpected());
    CHECK(leaf.IsOutsideArraysUnchanged());
  }
  // A position past the last key changes nothing.
  const Page after_removals = leaf.page();
  LeafRemoveAt(leaf.page().data(), kLeafCapacity);
  CHECK(leaf.page() == after_removals);
}

TEST(
    test_paged_btree_node_internal_child_for_key_uses_the_count_of_keys_at_or_below) {
  Page page = InternalPage(7, 1, {10, 20, 30}, {100, 101, 102, 103});
  CHECK_EQ(InternalChildIndex(page.data(), 5), std::size_t{0});
  CHECK_EQ(InternalChildIndex(page.data(), 10), std::size_t{1});
  CHECK_EQ(InternalChildIndex(page.data(), 15), std::size_t{1});
  CHECK_EQ(InternalChildIndex(page.data(), 30), std::size_t{3});
  CHECK_EQ(InternalChildIndex(page.data(), 31), std::size_t{3});

  // Separator 25 goes where its search key points, and its right child sits
  // right of the child of 20.
  const std::size_t position = InternalChildIndex(page.data(), 25);
  CHECK(InternalInsertAt(page.data(), kInternalCapacity, position, 25, 104));
  CHECK_EQ(KeysOf(page.data()), (std::vector<std::int64_t>{10, 20, 25, 30}));
  CHECK_EQ(ChildrenOf(page.data()),
           (std::vector<PageId>{100, 101, 102, 104, 103}));
  // A position past key_count and a full node are refused.
  CHECK(!InternalInsertAt(page.data(), kInternalCapacity, 9, 99, 105));
  CHECK(!InternalInsertAt(page.data(), 4, 0, 5, 106));
}

TEST(test_paged_btree_node_split_leaf_halves_and_links) {
  struct Case {
    std::size_t capacity;
    std::size_t expected_left;
    std::size_t expected_right;
  };
  const Case cases[] = {{kLeafCapacity, 145, 144}, {2, 1, 1}};
  for (const Case& test_case : cases) {
    Page left = FullLeaf(10, test_case.capacity, 77);
    Page right{};
    FormatLeaf(right.data(), 11, kTableId);
    const std::vector<std::int64_t> original_keys = KeysOf(left.data());
    const std::vector<Rid> original_rids = RidsOf(left.data());

    const std::optional<std::int64_t> separator =
        SplitLeaf(left.data(), right.data());
    CHECK(separator.has_value());
    CHECK_EQ(NodeView(left.data()).key_count(),
             static_cast<std::uint16_t>(test_case.expected_left));
    CHECK_EQ(NodeView(right.data()).key_count(),
             static_cast<std::uint16_t>(test_case.expected_right));
    CHECK_EQ(NodeView(right.data()).next_page_id(), PageId{77});
    CHECK_EQ(NodeView(left.data()).next_page_id(), PageId{11});
    CHECK_EQ(separator.value_or(0), NodeView(right.data()).KeyAt(0));

    std::vector<std::int64_t> joined_keys = KeysOf(left.data());
    const std::vector<std::int64_t> right_keys = KeysOf(right.data());
    joined_keys.insert(joined_keys.end(), right_keys.begin(), right_keys.end());
    CHECK_EQ(joined_keys, original_keys);
    std::vector<Rid> joined_rids = RidsOf(left.data());
    const std::vector<Rid> right_rids = RidsOf(right.data());
    joined_rids.insert(joined_rids.end(), right_rids.begin(), right_rids.end());
    CHECK_EQ(joined_rids, original_rids);
    // Both halves still pass the node check with that capacity.
    CHECK_OK(CheckNode(left.data(), 10, kTableId, 0,
                       NodeCapacity{test_case.capacity, kInternalCapacity}));
    CHECK_OK(CheckNode(right.data(), 11, kTableId, 0,
                       NodeCapacity{test_case.capacity, kInternalCapacity}));
  }
  // A leaf of one key cannot be split.
  Page tiny = LeafPage(10, {5});
  Page spare{};
  FormatLeaf(spare.data(), 11, kTableId);
  const Page tiny_before = tiny;
  CHECK(!SplitLeaf(tiny.data(), spare.data()).has_value());
  CHECK(tiny == tiny_before);
}

TEST(test_paged_btree_node_split_internal_pushes_up_the_middle_key) {
  struct Case {
    std::size_t capacity;
    std::size_t expected_left;
    std::size_t expected_right;
  };
  // The odd counts split evenly; an even count of n keys keeps (n - 1) / 2 on
  // the left and moves the larger half, n - 1 - (n - 1) / 2, to the right.
  const Case cases[] = {
      {kInternalCapacity, 168, 168}, {3, 1, 1}, {4, 1, 2}, {6, 2, 3}};
  for (const Case& test_case : cases) {
    Page left = FullInternal(10, test_case.capacity);
    Page right{};
    FormatInternal(right.data(), 11, kTableId, 1);
    const std::vector<std::int64_t> original_keys = KeysOf(left.data());
    const std::vector<PageId> original_children = ChildrenOf(left.data());

    const std::optional<std::int64_t> middle =
        SplitInternal(left.data(), right.data());
    CHECK(middle.has_value());
    CHECK_EQ(NodeView(left.data()).key_count(),
             static_cast<std::uint16_t>(test_case.expected_left));
    CHECK_EQ(NodeView(right.data()).key_count(),
             static_cast<std::uint16_t>(test_case.expected_right));
    CHECK_EQ(ChildrenOf(left.data()).size(), test_case.expected_left + 1);
    CHECK_EQ(ChildrenOf(right.data()).size(), test_case.expected_right + 1);
    CHECK_EQ(middle.value_or(0), original_keys[test_case.expected_left]);

    std::vector<std::int64_t> joined_keys = KeysOf(left.data());
    joined_keys.push_back(middle.value_or(0));
    const std::vector<std::int64_t> right_keys = KeysOf(right.data());
    joined_keys.insert(joined_keys.end(), right_keys.begin(), right_keys.end());
    CHECK_EQ(joined_keys, original_keys);
    std::vector<PageId> joined_children = ChildrenOf(left.data());
    const std::vector<PageId> right_children = ChildrenOf(right.data());
    joined_children.insert(joined_children.end(), right_children.begin(),
                           right_children.end());
    CHECK_EQ(joined_children, original_children);
    CHECK_EQ(joined_children.size(), test_case.capacity + 1);
  }
  Page tiny = InternalPage(10, 1, {5, 6}, {1, 2, 3});
  Page spare{};
  FormatInternal(spare.data(), 11, kTableId, 1);
  const Page tiny_before = tiny;
  CHECK(!SplitInternal(tiny.data(), spare.data()).has_value());
  CHECK(tiny == tiny_before);
}

TEST(test_paged_btree_node_check_rejects_each_violation) {
  const NodeCapacity capacity;
  const Page valid_leaf = LeafPage(5, {1, 2, 3});
  const Page valid_internal = InternalPage(5, 1, {10, 20}, {1, 2, 3});
  CHECK_OK(CheckNode(valid_leaf.data(), 5, kTableId, 0, capacity));
  CHECK_OK(CheckNode(valid_internal.data(), 5, kTableId, 1, capacity));
  CHECK_OK(CheckNode(valid_leaf.data(), 5, kTableId, std::nullopt, capacity));

  std::vector<DamagedNode> cases;
  Page out_of_order = valid_leaf;
  WriteLe32(out_of_order.data() + 48 + 8 * 1, 9);  // keys 1, 9, 3
  AddDamagedNode(cases, "keys out of order", out_of_order,
                 "do not increase strictly", 0);
  Page equal_keys = valid_leaf;
  WriteLe32(equal_keys.data() + 48 + 8 * 2, 2);  // keys 1, 2, 2
  AddDamagedNode(cases, "equal keys", equal_keys, "do not increase strictly",
                 0);
  Page leaf_overfull = valid_leaf;
  SetField16(leaf_overfull, 34, 290);
  AddDamagedNode(cases, "leaf key_count 290", leaf_overfull, "key_count 290",
                 0);
  Page internal_overfull = valid_internal;
  SetField16(internal_overfull, 34, 338);
  AddDamagedNode(cases, "internal key_count 338", internal_overfull,
                 "key_count 338", 1);
  Page leaf_with_level_one = valid_leaf;
  SetField16(leaf_with_level_one, 32, 1);
  AddDamagedNode(cases, "leaf with level 1", leaf_with_level_one,
                 "level 1 instead of 0", 0);
  Page with_slots = valid_leaf;
  SetField16(with_slots, strata::paged::layout::kSlotCount, 1);
  AddDamagedNode(cases, "slot_count", with_slots, "slot_count", 0);
  Page with_free_end = valid_leaf;
  SetField16(with_free_end, strata::paged::layout::kFreeEnd, 1);
  AddDamagedNode(cases, "free_end", with_free_end, "free_end", 0);
  Page with_dead_bytes = valid_leaf;
  SetField16(with_dead_bytes, strata::paged::layout::kDeadBytes, 1);
  AddDamagedNode(cases, "dead_bytes", with_dead_bytes, "dead_bytes", 0);
  Page other_table = valid_leaf;
  SetField32(other_table, strata::paged::layout::kTableId, 9);
  AddDamagedNode(cases, "another table", other_table, "belongs to table 9", 0);
  Page other_page = valid_leaf;
  SetField32(other_page, strata::paged::layout::kPageIdField, 6);
  AddDamagedNode(cases, "another page id", other_page, "carries the page id 6",
                 0);
  Page heap_page = valid_leaf;
  heap_page[strata::paged::layout::kPageTypeField] = 2;
  AddDamagedNode(cases, "heap page", heap_page, "page_type 2 instead of 4", 0);
  Page internal_with_link = valid_internal;
  SetField32(internal_with_link, strata::paged::layout::kNextPageId, 8);
  AddDamagedNode(cases, "internal with a link", internal_with_link,
                 "sibling link", 1);

  for (const DamagedNode& test_case : cases) {
    const auto status = CheckNode(test_case.page.data(), 5, kTableId,
                                  test_case.expected_level, capacity);
    CHECK(IsCorruptNaming(status, "5", test_case.rule));
    if (!IsCorruptNaming(status, "5", test_case.rule)) {
      // Shows which case failed and what it returned.
      CHECK_EQ(std::string(test_case.name), strata::testing::Describe(status));
    }
  }
  // The O(1) check does not look at the key order.
  CHECK_OK(CheckNodeHeader(out_of_order.data(), 5, kTableId, 0, capacity));
}

TEST(test_paged_btree_check_tree_accepts_valid_trees) {
  MemorySource one_leaf = TreeOfOneLeaf();
  MemorySource height_two = TreeOfHeightTwo();
  MemorySource height_three = TreeOfHeightThree();
  CHECK_OK(CheckTree(one_leaf, 10, kTableId, kSmall));
  CHECK_OK(CheckTree(height_two, 20, kTableId, kSmall));
  CHECK_OK(CheckTree(height_three, 30, kTableId, kSmall));

  struct Expected {
    MemorySource* source;
    PageId root;
    std::size_t height;
    std::size_t node_count;
    std::size_t key_count;
  };
  const Expected expectations[] = {{&one_leaf, 10, 1, 1, 3},
                                   {&height_two, 20, 2, 4, 8},
                                   {&height_three, 30, 3, 7, 6}};
  for (const Expected& expected : expectations) {
    const PageResult<strata::paged::TreeShape> shape = strata::paged::Shape(
        *expected.source, expected.root, kTableId, kSmall, 40);
    CHECK_OK(shape);
    if (!shape.ok()) {
      continue;
    }
    CHECK_EQ(shape.value().height, expected.height);
    CHECK_EQ(static_cast<std::size_t>(shape.value().root_level) + 1,
             expected.height);
    CHECK_EQ(shape.value().node_count, expected.node_count);
    CHECK_EQ(shape.value().key_count, expected.key_count);
    CHECK(shape.value().is_complete);
    CHECK_EQ(shape.value().nodes.size(), expected.node_count);
  }

  // The nodes come breadth first and an empty leaf has no first or last key.
  const auto shape =
      strata::paged::Shape(height_three, 30, kTableId, kSmall, 5);
  CHECK_OK(shape);
  if (shape.ok()) {
    std::vector<PageId> order;
    for (const strata::paged::NodeSummary& node : shape.value().nodes) {
      order.push_back(node.page_id);
    }
    CHECK_EQ(order, (std::vector<PageId>{30, 31, 34, 32, 33}));
    CHECK_EQ(shape.value().node_count, std::size_t{7});
    CHECK(!shape.value().nodes[4].first_key.has_value());
    CHECK_EQ(shape.value().nodes[3].last_key.value_or(0), std::int64_t{20});
    CHECK_EQ(shape.value().nodes[3].next_page_id, PageId{33});
    CHECK_EQ(shape.value().nodes[0].next_page_id, kInvalidPageId);
  }

  // The pages come depth first, root first, in the order of a left-to-right
  // walk.
  const auto pages =
      strata::paged::TreePages(height_three, 30, kTableId, kSmall);
  CHECK_OK(pages);
  if (pages.ok()) {
    CHECK_EQ(pages.value(), (std::vector<PageId>{30, 31, 32, 33, 34, 35, 36}));
  }
}

TEST(test_paged_btree_check_tree_names_each_violation) {
  struct Case {
    const char* name;
    MemorySource source;
    PageId root;
    PageId broken_page;
    const char* rule;
  };
  std::vector<Case> cases;

  MemorySource beyond_separator = TreeOfHeightTwo();
  beyond_separator.Put(21, LeafPage(21, {1, 5, 12}, 22));
  cases.push_back(Case{"key beyond its separator", beyond_separator, 20, 21,
                       "at or above the separator 10"});

  MemorySource below_separator = TreeOfHeightTwo();
  below_separator.Put(23, LeafPage(23, {15, 30, 40}));
  cases.push_back(Case{"key below its separator", below_separator, 20, 23,
                       "below the separator 20"});

  MemorySource wrong_depth = TreeOfHeightThree();
  wrong_depth.Put(34, LeafPage(34, {150, 160}));
  cases.push_back(Case{"leaf at another depth", wrong_depth, 30, 34,
                       "level 0 instead of 1"});

  MemorySource skipped_leaf = TreeOfHeightTwo();
  skipped_leaf.Put(21, LeafPage(21, {1, 5, 9}, 23));
  cases.push_back(
      Case{"chain skips a leaf", skipped_leaf, 20, 21, "next_page_id 23"});

  MemorySource backward_chain = TreeOfHeightTwo();
  backward_chain.Put(23, LeafPage(23, {20, 30, 40}, 21));
  cases.push_back(
      Case{"chain points backward", backward_chain, 20, 23, "next_page_id 21"});

  MemorySource sibling_on_internal = TreeOfHeightTwo();
  sibling_on_internal.At(20)[strata::paged::layout::kNextPageId] = 21;
  cases.push_back(Case{"internal node with a link", sibling_on_internal, 20, 20,
                       "sibling link"});

  MemorySource shared_child = TreeOfHeightThree();
  shared_child.Put(34, InternalPage(34, 1, {150}, {33, 36}));
  cases.push_back(
      Case{"child of two parents", shared_child, 30, 33, "is reached twice"});

  MemorySource foreign_table = TreeOfHeightTwo();
  foreign_table.Put(22, LeafPage(22, {10, 15}, 23, 9));
  cases.push_back(
      Case{"foreign table", foreign_table, 20, 22, "belongs to table 9"});

  MemorySource no_key = TreeOfHeightTwo();
  Page keyless_root = InternalPage(20, 1, {10, 20}, {21, 22, 23});
  SetField16(keyless_root, 34, 0);
  no_key.Put(20, keyless_root);
  cases.push_back(
      Case{"internal node without a key", no_key, 20, 20, "without a key"});

  for (Case& test_case : cases) {
    const auto status =
        CheckTree(test_case.source, test_case.root, kTableId, kSmall);
    const std::string broken = std::to_string(test_case.broken_page);
    CHECK(IsCorruptNaming(status, broken, test_case.rule));
  }

  // The walk that only lists the pages rejects a node without a key too, so a
  // DROP TABLE does not free a tree that it cannot list whole.
  MemorySource keyless = TreeOfHeightTwo();
  Page keyless_page = InternalPage(20, 1, {10, 20}, {21, 22, 23});
  SetField16(keyless_page, 34, 0);
  keyless.Put(20, keyless_page);
  const auto pages = strata::paged::TreePages(keyless, 20, kTableId, kSmall);
  CHECK_ERROR(pages, kCorrupt);
  CHECK_MESSAGE_CONTAINS(pages, "without a key");
}

TEST(test_paged_btree_check_tree_stops_loops_and_deep_roots) {
  // A root of level 40.
  MemorySource deep_root;
  deep_root.Put(20, InternalPage(20, 40, {10}, {21, 22}));
  const auto deep_status = CheckTree(deep_root, 20, kTableId, kSmall);
  CHECK(IsCorruptNaming(deep_status, "20", "not below 40"));

  // A leaf chain that returns to its first leaf.
  MemorySource looping_chain = TreeOfHeightTwo();
  looping_chain.Put(23, LeafPage(23, {20, 30, 40}, 21));
  const auto chain_status = CheckTree(looping_chain, 20, kTableId, kSmall);
  CHECK_ERROR(chain_status, kCorrupt);
  CHECK(looping_chain.MostReadsOfOnePage() <= 2);

  // A child that points to its own parent.
  MemorySource child_is_parent = TreeOfHeightTwo();
  child_is_parent.Put(20, InternalPage(20, 1, {10, 20}, {21, 20, 23}));
  const auto parent_status = CheckTree(child_is_parent, 20, kTableId, kSmall);
  CHECK(IsCorruptNaming(parent_status, "20", "is reached twice"));
  CHECK(child_is_parent.MostReadsOfOnePage() <= 2);
}
