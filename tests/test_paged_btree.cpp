#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "strata/paged/btree.h"
#include "strata/paged/btree_node.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/page_allocator.h"
#include "strata/paged/page_store.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"

// BTree over a buffer pool and a page allocator (design document 2.17, 3.8 to
// 3.10; ADR-0049, ADR-0050).

namespace strata::paged {

inline std::ostream& operator<<(std::ostream& stream, const BTreeEntry& entry) {
  return stream << "{" << entry.key << " -> " << entry.rid << "}";
}

}  // namespace strata::paged

namespace {

using strata::paged::BTree;
using strata::paged::BTreeCursor;
using strata::paged::BTreeEntry;
using strata::paged::BTreeOptions;
using strata::paged::BufferManager;
using strata::paged::KeyRange;
using strata::paged::kInvalidPageId;
using strata::paged::LatchMode;
using strata::paged::MemoryPageStore;
using strata::paged::PageAllocator;
using strata::paged::PageGuard;
using strata::paged::PageId;
using strata::paged::PageResult;
using strata::paged::ReplacementPolicyKind;
using strata::paged::Rid;
using strata::paged::SlottedPage;
using strata::paged::Status;
using Operation = strata::paged::MemoryPageStore::Operation;

constexpr std::uint32_t kTableId = 7;
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
// Capacities that give a tall tree from a few keys.
constexpr BTreeOptions kSmallOptions{4, 3};

Rid RidOfKey(std::int64_t key) {
  const std::uint64_t bits = static_cast<std::uint64_t>(key);
  return Rid{static_cast<PageId>(bits % 1000003u) + 100u,
             static_cast<std::uint16_t>(bits % 40000u)};
}

// A store, a pool and an allocator, with the file header and the two system
// pages in place so that the first user page is page 3.
class TreeEnv {
 public:
  explicit TreeEnv(std::size_t frames,
                   ReplacementPolicyKind policy = ReplacementPolicyKind::kLru)
      : store_(std::make_unique<MemoryPageStore>()) {
    Connect(frames, policy);
    if (buffer_ != nullptr) {
      CHECK_OK(strata::testing::AddFileHeaderPage(*buffer_));
    }
  }

  // Over a copy of the pages of another store, with a cold pool.
  TreeEnv(const MemoryPageStore& source, std::size_t frames,
          ReplacementPolicyKind policy = ReplacementPolicyKind::kLru)
      : store_(strata::testing::CopyOfStore(source)) {
    Connect(frames, policy);
  }

  bool IsReady() const { return allocator_ != nullptr; }
  MemoryPageStore& store() { return *store_; }
  BufferManager& buffer() { return *buffer_; }
  PageAllocator& allocator() { return *allocator_; }

 private:
  void Connect(std::size_t frames, ReplacementPolicyKind policy) {
    auto created = BufferManager::Create(*store_, frames, policy);
    CHECK_OK(created);
    if (!created.ok()) {
      return;
    }
    buffer_ = std::move(created).value();
    allocator_ = std::make_unique<PageAllocator>(*buffer_);
  }

  std::unique_ptr<MemoryPageStore> store_;
  std::unique_ptr<BufferManager> buffer_;
  std::unique_ptr<PageAllocator> allocator_;
};

std::optional<BTree> CreateTree(TreeEnv& env, BTreeOptions options = {}) {
  PageResult<BTree> created =
      BTree::Create(env.buffer(), env.allocator(), kTableId, options);
  CHECK_OK(created);
  if (!created.ok()) {
    return std::nullopt;
  }
  return std::move(created).value();
}

std::optional<BTree> OpenTree(TreeEnv& env, PageId root,
                              BTreeOptions options = {}) {
  PageResult<BTree> opened =
      BTree::Open(env.buffer(), env.allocator(), kTableId, root, options);
  CHECK_OK(opened);
  if (!opened.ok()) {
    return std::nullopt;
  }
  return std::move(opened).value();
}

// Inserts the keys first to last in ascending order, each with its Rid.
void InsertAscending(BTree& tree, std::int64_t first, std::int64_t last,
                     std::int64_t step = 1) {
  for (std::int64_t key = first; key <= last; key += step) {
    const Status status = tree.Insert(key, RidOfKey(key));
    if (status.has_value()) {
      CHECK_OK(status);
      return;
    }
  }
}

// Every entry of the range, in the order the cursor returns them.
std::vector<BTreeEntry> Collect(const BTree& tree, const KeyRange& range) {
  std::vector<BTreeEntry> entries;
  BTreeCursor cursor = tree.Scan(range);
  while (true) {
    PageResult<std::optional<BTreeEntry>> next = cursor.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      return entries;
    }
    entries.push_back(*next.value());
  }
}

bool IsInRange(const KeyRange& range, std::int64_t key) {
  if (range.low.has_value() &&
      (range.is_low_inclusive ? key < *range.low : key <= *range.low)) {
    return false;
  }
  if (range.high.has_value() &&
      (range.is_high_inclusive ? key > *range.high : key >= *range.high)) {
    return false;
  }
  return true;
}

std::vector<BTreeEntry> ExpectedEntries(
    const std::map<std::int64_t, Rid>& model, const KeyRange& range) {
  std::vector<BTreeEntry> entries;
  for (const auto& entry : model) {
    if (IsInRange(range, entry.first)) {
      entries.push_back(BTreeEntry{entry.first, entry.second});
    }
  }
  return entries;
}

// The nodes of a tree in breadth-first order, from Shape.
std::vector<strata::paged::NodeSummary> NodesOf(const BTree& tree) {
  const auto shape = tree.Shape(100000);
  CHECK_OK(shape);
  return shape.ok() ? shape.value().nodes
                    : std::vector<strata::paged::NodeSummary>{};
}

std::size_t LeafCountOf(const BTree& tree) {
  std::size_t leaves = 0;
  for (const strata::paged::NodeSummary& node : NodesOf(tree)) {
    if (node.level == 0) {
      ++leaves;
    }
  }
  return leaves;
}

std::size_t HeightOf(const BTree& tree) {
  const auto shape = tree.Shape(1);
  CHECK_OK(shape);
  return shape.ok() ? shape.value().height : 0;
}

enum class PageKind { kHeap, kFree, kDeepRoot };

// A page taken from BufferManager::NewPage, not from Allocate, so the free list
// is not touched.
PageId NewFormattedPage(BufferManager& buffer, PageKind kind) {
  auto created = buffer.NewPage();
  CHECK_OK(created);
  if (!created.ok()) {
    return kInvalidPageId;
  }
  PageGuard guard = std::move(created).value();
  if (kind == PageKind::kHeap) {
    SlottedPage::Format(guard.mutable_data(), guard.page_id(), kTableId);
  } else if (kind == PageKind::kFree) {
    SlottedPage::FormatFree(guard.mutable_data(), guard.page_id(),
                            kInvalidPageId);
  } else {
    strata::paged::FormatInternal(guard.mutable_data(), guard.page_id(),
                                  kTableId, 40);
    strata::paged::InternalSetFirstEntry(guard.mutable_data(), 3, 10, 4);
  }
  guard.MarkDirty();
  return guard.page_id();
}

// The FetchPage calls since the last ResetStats: hits and misses.
std::uint64_t FetchCountOf(BufferManager& buffer) {
  const strata::paged::BufferStats stats = buffer.Stats();
  return stats.hits + stats.misses;
}

// Checks that the tree is valid, that every key in `keys` is found with its
// Rid and that nothing is pinned.
void CheckTreeHolds(const BTree& tree, BufferManager& buffer,
                    const std::vector<std::int64_t>& keys) {
  CHECK_OK(tree.CheckInvariants());
  for (std::int64_t key : keys) {
    const auto found = tree.Find(key);
    CHECK_OK(found);
    if (found.ok()) {
      CHECK(found.value().has_value());
      if (found.value().has_value()) {
        CHECK_EQ(*found.value(), RidOfKey(key));
      }
    }
  }
  CHECK_EQ(buffer.PinnedFrameCount(), std::size_t{0});
}

}  // namespace

TEST(test_paged_btree_create_and_open_validate_the_root) {
  TreeEnv env(16);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env);
  if (!tree.has_value()) {
    return;
  }
  {
    auto fetched = env.buffer().FetchPage(tree->root(), LatchMode::kShared);
    CHECK_OK(fetched);
    if (fetched.ok()) {
      const strata::paged::NodeView node(fetched.value().data());
      CHECK_EQ(node.table_id(), kTableId);
      CHECK_EQ(node.next_page_id(), kInvalidPageId);
      CHECK_EQ(node.level(), std::uint16_t{0});
      CHECK_EQ(node.key_count(), std::uint16_t{0});
      CHECK(
          strata::paged::SlottedPageView(fetched.value().data()).page_type() ==
          strata::paged::PageType::kIndexNode);
    }
  }
  CHECK(OpenTree(env, tree->root()).has_value());
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});

  // Five pages that are not the root of a tree of this table.
  const PageResult<BTree> other_table =
      BTree::Create(env.buffer(), env.allocator(), kTableId + 1);
  CHECK_OK(other_table);
  if (!other_table.ok()) {
    return;
  }
  struct OpenCase {
    PageId root;
    const char* code;
  };
  const OpenCase cases[] = {
      {NewFormattedPage(env.buffer(), PageKind::kHeap), "kCorrupt"},
      {NewFormattedPage(env.buffer(), PageKind::kFree), "kCorrupt"},
      {other_table.value().root(), "kCorrupt"},
      {NewFormattedPage(env.buffer(), PageKind::kDeepRoot), "kCorrupt"},
      {env.buffer().PageCount() + 10, "kNotFound"}};
  for (const OpenCase& test_case : cases) {
    const PageResult<BTree> opened = BTree::Open(env.buffer(), env.allocator(),
                                                 kTableId, test_case.root, {});
    CHECK_EQ(strata::testing::ErrorName(opened), std::string(test_case.code));
    CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
  }

  // Capacities outside [2, 289] for a leaf and [3, 337] for an internal node.
  const PageId page_count = env.buffer().PageCount();
  const BTreeOptions bad_options[] = {
      {1, 337}, {290, 337}, {289, 2}, {289, 338}};
  for (const BTreeOptions& options : bad_options) {
    const PageResult<BTree> created =
        BTree::Create(env.buffer(), env.allocator(), kTableId, options);
    CHECK_ERROR(created, kInvalidArgument);
    const PageResult<BTree> opened = BTree::Open(
        env.buffer(), env.allocator(), kTableId, tree->root(), options);
    CHECK_ERROR(opened, kInvalidArgument);
    CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
  }
  CHECK_EQ(env.buffer().PageCount(), page_count);
}

TEST(test_paged_btree_ascending_inserts_reach_height_3_and_stay_valid) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env);
  if (!tree.has_value()) {
    return;
  }
  for (std::int64_t block = 0; block < 12; ++block) {
    InsertAscending(*tree, block * 5000 + 1, (block + 1) * 5000);
    CHECK_OK(tree->CheckInvariants());
  }
  CHECK_EQ(HeightOf(*tree), std::size_t{3});
  CHECK_EQ(LeafCountOf(*tree), std::size_t{413});
  // Ascending inserts leave every leaf but the last with the 145 keys that the
  // left half of a split keeps.
  for (const strata::paged::NodeSummary& node : NodesOf(*tree)) {
    if (node.level == 0) {
      CHECK(node.key_count >= 145);
    }
  }

  for (std::int64_t key = 1; key <= 60000; ++key) {
    const auto found = tree->Find(key);
    if (!found.ok() || !found.value().has_value() ||
        *found.value() != RidOfKey(key)) {
      CHECK_EQ(key, std::int64_t{0});  // reports the first wrong key
      break;
    }
  }
  const std::vector<BTreeEntry> all = Collect(*tree, KeyRange{});
  CHECK_EQ(all.size(), std::size_t{60000});
  bool is_ascending = true;
  for (std::size_t index = 0; index < all.size(); ++index) {
    is_ascending =
        is_ascending &&
        all[index] == BTreeEntry{static_cast<std::int64_t>(index) + 1,
                                 RidOfKey(index + 1)};
  }
  CHECK(is_ascending);
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

namespace {

// One random run against a std::map: insert, delete, find, replace and range
// scan, with the invariants checked every 500 operations.
void RunRandomOperations(BTreeOptions options, std::int64_t key_space,
                         int operations, std::uint32_t seed) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env, options);
  if (!tree.has_value()) {
    return;
  }
  std::map<std::int64_t, Rid> model;
  std::mt19937 random(seed);
  for (int step = 1; step <= operations; ++step) {
    const unsigned choice = random() % 100;
    const std::int64_t key = static_cast<std::int64_t>(random() % key_space);
    const Rid rid = RidOfKey(key + step);
    if (choice < 35) {
      const Status status = tree->Insert(key, rid);
      if (model.contains(key)) {
        CHECK_ERROR(status, kAlreadyExists);
      } else {
        CHECK_OK(status);
        model[key] = rid;
      }
    } else if (choice < 60) {
      const Status status = tree->Delete(key);
      if (model.erase(key) == 1) {
        CHECK_OK(status);
      } else {
        CHECK_ERROR(status, kNotFound);
      }
    } else if (choice < 80) {
      const auto found = tree->Find(key);
      CHECK_OK(found);
      if (found.ok()) {
        const auto expected = model.find(key);
        CHECK_EQ(found.value().has_value(), expected != model.end());
        if (found.value().has_value() && expected != model.end()) {
          CHECK_EQ(*found.value(), expected->second);
        }
      }
    } else if (choice < 90) {
      const Status status = tree->Replace(key, rid);
      if (model.contains(key)) {
        CHECK_OK(status);
        model[key] = rid;
      } else {
        CHECK_ERROR(status, kNotFound);
      }
    } else {
      KeyRange range;
      range.low = key;
      range.is_low_inclusive = (random() % 2) == 0;
      if (random() % 10 != 0) {
        range.high = key + static_cast<std::int64_t>(random() % 200);
        range.is_high_inclusive = (random() % 2) == 0;
      }
      CHECK_EQ(Collect(*tree, range), ExpectedEntries(model, range));
    }
    if (step % 500 == 0) {
      CHECK_OK(tree->CheckInvariants());
    }
  }
  CHECK_EQ(Collect(*tree, KeyRange{}), ExpectedEntries(model, KeyRange{}));
  CHECK_OK(tree->CheckInvariants());
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

}  // namespace

TEST(test_paged_btree_random_operations_agree_with_std_map) {
  const std::uint32_t seeds[] = {1, 20261123, 987654321};
  for (std::uint32_t seed : seeds) {
    RunRandomOperations(kSmallOptions, 3000, 30000, seed);
    RunRandomOperations(BTreeOptions{}, 20000, 20000, seed);
  }
}

TEST(test_paged_btree_delete_leaves_empty_leaves_in_place) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env);
  if (!tree.has_value()) {
    return;
  }
  InsertAscending(*tree, 1, 1000);
  std::vector<strata::paged::NodeSummary> leaves;
  for (const strata::paged::NodeSummary& node : NodesOf(*tree)) {
    if (node.level == 0) {
      leaves.push_back(node);
    }
  }
  CHECK(leaves.size() >= 3);
  if (leaves.size() < 3) {
    return;
  }
  const strata::paged::NodeSummary middle = leaves[leaves.size() / 2];
  const std::int64_t first_key = middle.first_key.value_or(0);
  const std::int64_t last_key = middle.last_key.value_or(0);
  const std::size_t height = HeightOf(*tree);
  const PageId page_count = env.buffer().PageCount();
  const auto before = tree->Shape(100000);
  CHECK_OK(before);

  for (std::int64_t key = first_key; key <= last_key; ++key) {
    CHECK_OK(tree->Delete(key));
  }
  CHECK_OK(tree->CheckInvariants());
  CHECK_EQ(HeightOf(*tree), height);
  CHECK_EQ(env.buffer().PageCount(), page_count);
  bool is_empty_leaf_found = false;
  for (const strata::paged::NodeSummary& node : NodesOf(*tree)) {
    if (node.page_id == middle.page_id) {
      is_empty_leaf_found = true;
      CHECK_EQ(node.key_count, std::uint16_t{0});
      CHECK_EQ(node.next_page_id, middle.next_page_id);
    }
  }
  CHECK(is_empty_leaf_found);
  if (before.ok()) {
    const auto after = tree->Shape(100000);
    CHECK_OK(after);
    if (after.ok()) {
      CHECK_EQ(after.value().node_count, before.value().node_count);
    }
  }

  // The scan skips the empty leaf, and the deleted keys are absent.
  const std::vector<BTreeEntry> remaining = Collect(*tree, KeyRange{});
  CHECK_EQ(remaining.size(),
           static_cast<std::size_t>(1000 - (last_key - first_key + 1)));
  for (const BTreeEntry& entry : remaining) {
    CHECK(entry.key < first_key || entry.key > last_key);
  }
  CHECK_EQ(
      Collect(*tree, KeyRange{first_key - 1, true, last_key + 1, true}).size(),
      std::size_t{2});
  for (std::int64_t key = first_key; key <= last_key; ++key) {
    const auto found = tree->Find(key);
    CHECK_OK(found);
    CHECK(found.ok() && !found.value().has_value());
    CHECK_ERROR(tree->Delete(key), kNotFound);
  }

  // Reinserting the keys works, and so does deleting everything.
  InsertAscending(*tree, first_key, last_key);
  CHECK_EQ(Collect(*tree, KeyRange{}).size(), std::size_t{1000});
  CHECK_OK(tree->CheckInvariants());
  for (std::int64_t key = 1; key <= 1000; ++key) {
    CHECK_OK(tree->Delete(key));
  }
  CHECK(Collect(*tree, KeyRange{}).empty());
  CHECK_OK(tree->CheckInvariants());
  CHECK_EQ(HeightOf(*tree), height);
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

TEST(test_paged_btree_scan_respects_each_bound_kind) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env, kSmallOptions);
  if (!tree.has_value()) {
    return;
  }
  InsertAscending(*tree, 2, 2000, 2);
  std::map<std::int64_t, Rid> model;
  for (std::int64_t key = 2; key <= 2000; key += 2) {
    model[key] = RidOfKey(key);
  }

  // Probes: present and absent keys, the ends, the 64-bit extremes and the
  // first and last key of a few leaves.
  std::vector<std::int64_t> probes = {
      kInt64Min, -1,   0,    1,    2,    3,    4,    5,
      1000,      1001, 1998, 1999, 2000, 2001, 5000, kInt64Max};
  std::vector<strata::paged::NodeSummary> leaves;
  for (const strata::paged::NodeSummary& node : NodesOf(*tree)) {
    if (node.level == 0 && node.key_count > 0) {
      leaves.push_back(node);
    }
  }
  CHECK(leaves.size() > 10);
  if (leaves.size() <= 10) {
    return;
  }
  const std::size_t picks[] = {0, leaves.size() / 3, leaves.size() / 2,
                               leaves.size() - 1};
  for (std::size_t pick : picks) {
    probes.push_back(leaves[pick].first_key.value_or(0));
    probes.push_back(leaves[pick].last_key.value_or(0));
  }

  std::vector<KeyRange> ranges;
  std::vector<std::optional<std::int64_t>> bounds = {std::nullopt};
  for (std::int64_t probe : probes) {
    bounds.push_back(probe);
  }
  for (const std::optional<std::int64_t>& low : bounds) {
    for (bool is_low_inclusive : {true, false}) {
      for (const std::optional<std::int64_t>& high : bounds) {
        for (bool is_high_inclusive : {true, false}) {
          ranges.push_back(
              KeyRange{low, is_low_inclusive, high, is_high_inclusive});
        }
      }
    }
  }
  std::size_t checked = 0;
  for (const KeyRange& range : ranges) {
    const std::vector<BTreeEntry> entries = Collect(*tree, range);
    CHECK_EQ(entries, ExpectedEntries(model, range));
    ++checked;
  }
  CHECK(checked > 2000);
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

namespace {

enum class Scenario { kFullTargetLeaf, kFullRootLeaf, kFullRootInternal };

struct BuiltTree {
  std::vector<std::int64_t> keys;
  std::int64_t next_key = 0;
  PageId root = kInvalidPageId;
};

// Builds a small tree whose next ascending insert has to split: a full leaf
// below the root, a root that is a full leaf, or a root that is a full
// internal node.
BuiltTree BuildForSplit(TreeEnv& env, Scenario scenario) {
  BuiltTree built;
  std::optional<BTree> tree = CreateTree(env, kSmallOptions);
  if (!tree.has_value()) {
    return built;
  }
  built.root = tree->root();
  std::int64_t key = 10;
  while (built.keys.size() < 200) {
    CHECK_OK(tree->Insert(key, RidOfKey(key)));
    built.keys.push_back(key);
    key += 10;
    bool is_ready = false;
    if (scenario == Scenario::kFullRootLeaf) {
      is_ready = built.keys.size() == 4;
    } else if (scenario == Scenario::kFullTargetLeaf) {
      is_ready = built.keys.size() == 6;
    } else {
      const auto described = tree->Shape(1);
      is_ready = described.ok() && described.value().nodes[0].level >= 1 &&
                 described.value().nodes[0].key_count == 3;
    }
    if (is_ready) {
      break;
    }
  }
  built.next_key = key;
  CHECK_OK(env.buffer().FlushAll());
  return built;
}

}  // namespace

TEST(test_paged_btree_insert_failures_leave_a_valid_tree) {
  const Scenario scenarios[] = {Scenario::kFullTargetLeaf,
                                Scenario::kFullRootLeaf,
                                Scenario::kFullRootInternal};
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  int failures_by_operation[3] = {0, 0, 0};
  for (Scenario scenario : scenarios) {
    TreeEnv built_env(64);
    if (!built_env.IsReady()) {
      return;
    }
    const BuiltTree built = BuildForSplit(built_env, scenario);
    CHECK(built.root != kInvalidPageId);
    CHECK(!built.keys.empty());
    for (int operation_index = 0; operation_index < 3; ++operation_index) {
      bool has_succeeded = false;
      for (int nth_call = 1; nth_call < 100 && !has_succeeded; ++nth_call) {
        TreeEnv cold(built_env.store(), 5);
        if (!cold.IsReady()) {
          return;
        }
        std::optional<BTree> tree = OpenTree(cold, built.root, kSmallOptions);
        if (!tree.has_value()) {
          return;
        }
        cold.store().InjectFailure(operations[operation_index], nth_call);
        const Status status =
            tree->Insert(built.next_key, RidOfKey(built.next_key));
        cold.store().ClearInjectedFailures();
        if (status.has_value()) {
          CHECK_ERROR(status, kIo);
          ++failures_by_operation[operation_index];
        } else {
          has_succeeded = true;
        }
        CheckTreeHolds(*tree, cold.buffer(), built.keys);
        const auto found = tree->Find(built.next_key);
        CHECK_OK(found);
        if (found.ok() && found.value().has_value()) {
          CHECK_EQ(*found.value(), RidOfKey(built.next_key));
          CHECK(!status.has_value());
        }
        if (status.has_value()) {
          // With the injection off the same insert succeeds.
          CHECK_OK(tree->Insert(built.next_key, RidOfKey(built.next_key)));
          CHECK_OK(tree->CheckInvariants());
        }
      }
      CHECK(has_succeeded);
    }
  }
  // Reads and allocations were failed at least once, so the loops did not pass
  // vacuously.
  CHECK(failures_by_operation[0] > 0);
  CHECK(failures_by_operation[2] > 0);
}

namespace {

// Reader threads of the concurrency test. They record a problem in `problems`
// instead of calling CHECK, because the harness reports for the test thread.
void ReaderLoop(const BTree& tree, const std::atomic<std::int64_t>& published,
                const std::atomic<bool>& is_done, std::uint32_t seed,
                std::atomic<int>& problems) {
  std::mt19937 random(seed);
  int rounds = 0;
  while (!is_done.load()) {
    const std::int64_t known = published.load();
    if (known < 1) {
      continue;
    }
    const std::int64_t key =
        1 + static_cast<std::int64_t>(random() % static_cast<unsigned>(known));
    const auto found = tree.Find(key);
    if (!found.ok() || !found.value().has_value() ||
        *found.value() != RidOfKey(key)) {
      problems.fetch_add(1);
    }
    if (++rounds % 8 == 0) {
      const std::int64_t high = std::min(known, key + 60);
      BTreeCursor cursor = tree.Scan(KeyRange{key, true, high, true});
      std::int64_t expected = key;
      while (true) {
        PageResult<std::optional<BTreeEntry>> next = cursor.Next();
        if (!next.ok()) {
          problems.fetch_add(1);
          break;
        }
        if (!next.value().has_value()) {
          break;
        }
        if (next.value()->key != expected) {
          problems.fetch_add(1);
          break;
        }
        ++expected;
      }
      if (expected != high + 1) {
        problems.fetch_add(1);
      }
    }
  }
}

}  // namespace

TEST(test_paged_btree_readers_run_beside_a_writer) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env);
  if (!tree.has_value()) {
    return;
  }
  const BTree reader_tree = *tree;
  std::atomic<std::int64_t> published{0};
  std::atomic<bool> is_done{false};
  std::atomic<int> problems{0};
  std::vector<std::thread> readers;
  for (std::uint32_t reader = 0; reader < 4; ++reader) {
    readers.emplace_back(ReaderLoop, std::cref(reader_tree),
                         std::cref(published), std::cref(is_done), 100 + reader,
                         std::ref(problems));
  }
  int insert_failures = 0;
  for (std::int64_t key = 1; key <= 20000; ++key) {
    if (tree->Insert(key, RidOfKey(key)).has_value()) {
      ++insert_failures;
      break;
    }
    published.store(key);
  }
  is_done.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  CHECK_EQ(insert_failures, 0);
  CHECK_EQ(problems.load(), 0);
  CHECK_OK(tree->CheckInvariants());
  CHECK_EQ(Collect(*tree, KeyRange{}).size(), std::size_t{20000});
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

namespace {

// 2000 ascending inserts and then finds, deletes and a scan, in a pool of 4
// frames. `has_free_list` first puts 2000 pages on the free list, so that
// Allocate takes the path that holds the file header and the popped page.
void RunInFourFrames(ReplacementPolicyKind policy, bool has_free_list) {
  TreeEnv env(4, policy);
  if (!env.IsReady()) {
    return;
  }
  if (has_free_list) {
    std::vector<PageId> pages;
    for (int count = 0; count < 2000; ++count) {
      pages.push_back(NewFormattedPage(env.buffer(), PageKind::kHeap));
    }
    CHECK_OK(env.allocator().ReleasePages(pages));
  }
  std::optional<BTree> tree = CreateTree(env, kSmallOptions);
  if (!tree.has_value()) {
    return;
  }
  for (std::int64_t key = 1; key <= 2000; ++key) {
    const Status status = tree->Insert(key, RidOfKey(key));
    if (status.has_value()) {
      CHECK_OK(status);
      return;
    }
    if (env.buffer().PinnedFrameCount() != 0) {
      CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
      return;
    }
  }
  CHECK_EQ(HeightOf(*tree), std::size_t{10});
  CHECK_OK(tree->CheckInvariants());
  for (std::int64_t key = 1; key <= 2000; ++key) {
    const auto found = tree->Find(key);
    CHECK_OK(found);
    CHECK(found.ok() && found.value().has_value());
  }
  for (std::int64_t key = 3; key <= 2000; key += 3) {
    CHECK_OK(tree->Delete(key));
  }
  CHECK_EQ(Collect(*tree, KeyRange{}).size(), std::size_t{2000 - 666});
  CHECK_OK(tree->CheckInvariants());
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

}  // namespace

TEST(test_paged_btree_insert_with_a_split_runs_in_four_frames) {
  const ReplacementPolicyKind policies[] = {ReplacementPolicyKind::kLru,
                                            ReplacementPolicyKind::kTwoQ};
  for (ReplacementPolicyKind policy : policies) {
    RunInFourFrames(policy, false);
    RunInFourFrames(policy, true);
  }
}

TEST(test_paged_btree_find_touches_one_page_per_level) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env);
  if (!tree.has_value()) {
    return;
  }
  InsertAscending(*tree, 1, 60000);
  const std::size_t height = HeightOf(*tree);
  CHECK_EQ(height, std::size_t{3});

  std::mt19937 random(7);
  std::vector<std::int64_t> found_keys;
  std::vector<std::int64_t> deleted_keys;
  std::vector<std::int64_t> scanned_keys;
  for (int count = 0; count < 100; ++count) {
    found_keys.push_back(1 + static_cast<std::int64_t>(random() % 20000));
    deleted_keys.push_back(20001 + static_cast<std::int64_t>(count) * 3);
    scanned_keys.push_back(40001 + static_cast<std::int64_t>(random() % 20000));
  }

  // Find, Delete and a one-key scan each make one FetchPage call per level.
  for (std::int64_t key : found_keys) {
    env.buffer().ResetStats();
    const auto found = tree->Find(key);
    CHECK_OK(found);
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{height});
  }
  for (std::int64_t key : deleted_keys) {
    env.buffer().ResetStats();
    CHECK_OK(tree->Delete(key));
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{height});
  }
  for (std::int64_t key : scanned_keys) {
    env.buffer().ResetStats();
    BTreeCursor cursor = tree->Scan(KeyRange{key, true, key, true});
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{0});
    const auto first = cursor.Next();
    CHECK_OK(first);
    CHECK(first.ok() && first.value().has_value());
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{height});
    // The key is unique, so the scan has nothing left to read.
    const auto second = cursor.Next();
    CHECK(second.ok() && !second.value().has_value());
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{height});
  }
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}

// A delete leaves empty leaves in the chain. A key that is absent from the
// emptied region lies inside the key interval of the leaf that the descent
// reached, so the cursor must not walk the empty leaves after it.
TEST(test_paged_btree_absent_key_in_an_emptied_region_reads_one_leaf) {
  TreeEnv env(64);
  if (!env.IsReady()) {
    return;
  }
  std::optional<BTree> tree = CreateTree(env, kSmallOptions);
  if (!tree.has_value()) {
    return;
  }
  constexpr std::int64_t kLastKey = 2000;
  constexpr std::int64_t kFirstEmptied = 301;
  constexpr std::int64_t kLastEmptied = 1700;
  InsertAscending(*tree, 1, kLastKey);
  const std::size_t height = HeightOf(*tree);
  for (std::int64_t key = kFirstEmptied; key <= kLastEmptied; ++key) {
    CHECK_OK(tree->Delete(key));
  }
  CHECK_OK(tree->CheckInvariants());
  CHECK(LeafCountOf(*tree) > 500);

  // An absent one-key range reads one page per level and never a leaf more.
  const std::vector<std::int64_t> absent_keys = {kFirstEmptied, 1000,
                                                 kLastEmptied};
  for (std::int64_t key : absent_keys) {
    env.buffer().ResetStats();
    CHECK(Collect(*tree, KeyRange{key, true, key, true}).empty());
    CHECK_EQ(FetchCountOf(env.buffer()), std::uint64_t{height});
  }
  // The early end changes no result: ranges inside, across and beside the
  // emptied region agree with a model.
  std::map<std::int64_t, Rid> model;
  for (std::int64_t key = 1; key <= kLastKey; ++key) {
    if (key < kFirstEmptied || key > kLastEmptied) {
      model.emplace(key, RidOfKey(key));
    }
  }
  const std::vector<KeyRange> ranges = {
      KeyRange{250, true, kLastKey, true},
      KeyRange{std::nullopt, true, 5, true},
      KeyRange{1690, true, 1710, true},
      KeyRange{1000, true, 1001, true},
      KeyRange{299, false, 1701, false},
      KeyRange{std::nullopt, true, std::nullopt, true},
  };
  for (const KeyRange& range : ranges) {
    CHECK(Collect(*tree, range) == ExpectedEntries(model, range));
  }
  CHECK_EQ(env.buffer().PinnedFrameCount(), std::size_t{0});
}
