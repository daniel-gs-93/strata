#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "strata/paged/btree_node.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/page_allocator.h"
#include "strata/paged/page_store.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"

// PageAllocator and the free list (design document 2.10, 2.11; ADR-0046).

namespace {

using strata::paged::Database;
using strata::paged::kInvalidPageId;
using strata::paged::LatchMode;
using strata::paged::PageGuard;
using strata::paged::PageId;
using strata::paged::PageType;
using strata::paged::SlottedPage;
using strata::paged::SlottedPageView;
using strata::testing::ChainOf;
using strata::testing::FreeListHeadOf;
using strata::testing::MakeChain;
using strata::testing::MemoryDatabase;
using strata::testing::OpenNewMemoryDatabase;
using strata::testing::PageTypeOf;
using Operation = strata::paged::MemoryPageStore::Operation;

// Sets the next_page_id of a page through an exclusive guard.
void SetNext(strata::paged::BufferManager& buffer, PageId page_id,
             PageId next) {
  auto fetched = buffer.FetchPage(page_id, LatchMode::kExclusive);
  CHECK_OK(fetched);
  if (!fetched.ok()) {
    return;
  }
  PageGuard guard = std::move(fetched).value();
  SlottedPage(guard.mutable_data()).set_next_page_id(next);
  guard.MarkDirty();
}

std::vector<PageId> FreePagesOf(Database& database) {
  auto free_pages = database.allocator().FreePages();
  CHECK_OK(free_pages);
  return free_pages.ok() ? free_pages.value() : std::vector<PageId>{};
}

}  // namespace

TEST(test_paged_allocator_extends_the_file_when_the_list_is_empty) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_EQ(database.buffer().PageCount(), PageId{3});
  std::vector<PageId> allocated;
  for (int call = 0; call < 2; ++call) {
    auto page = database.allocator().Allocate();
    CHECK_OK(page);
    if (!page.ok()) {
      return;
    }
    CHECK(page.value().is_held());
    CHECK(page.value().mode() == LatchMode::kExclusive);
    CHECK_EQ(strata::paged::ReadLe32(page.value().data() + 4),
             page.value().page_id());
    allocated.push_back(page.value().page_id());
  }
  CHECK_EQ(allocated, (std::vector<PageId>{3, 4}));
  CHECK_EQ(database.buffer().PageCount(), PageId{5});
  CHECK_EQ(FreeListHeadOf(database.buffer()), PageId{0});
  CHECK(FreePagesOf(database).empty());
}

TEST(test_paged_allocator_releases_a_chain_and_reuses_it_in_order) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  const std::vector<PageId> chain = MakeChain(database, "t", 3);
  CHECK_EQ(chain.size(), std::size_t{3});
  if (chain.size() != 3) {
    return;
  }
  const PageId page_count = database.buffer().PageCount();
  CHECK_OK(database.allocator().Release(chain.front()));
  // The checksum is written when a page leaves the pool.
  CHECK_OK(database.buffer().FlushAll());

  for (PageId page_id : chain) {
    auto fetched = database.buffer().FetchPage(page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      return;
    }
    const SlottedPageView view(fetched.value().data());
    CHECK(view.page_type() == PageType::kFree);
    CHECK_EQ(view.slot_count(), 0);
    CHECK_EQ(view.table_id(), std::uint32_t{0});
    CHECK_EQ(view.free_end(), 4096);
    CHECK(SlottedPage::VerifyHeader(fetched.value().data(), page_id) ==
          std::nullopt);
  }
  CHECK_EQ(FreePagesOf(database), chain);
  CHECK_EQ(FreeListHeadOf(database.buffer()), chain.front());

  std::vector<PageId> reused;
  for (int call = 0; call < 3; ++call) {
    auto page = database.allocator().Allocate();
    CHECK_OK(page);
    if (!page.ok()) {
      return;
    }
    // Still formatted as a free page, for the caller to format.
    CHECK(SlottedPageView(page.value().data()).page_type() == PageType::kFree);
    reused.push_back(page.value().page_id());
  }
  CHECK_EQ(reused, chain);
  CHECK_EQ(database.buffer().PageCount(), page_count);
  CHECK_EQ(FreeListHeadOf(database.buffer()), PageId{0});

  auto extended = database.allocator().Allocate();
  CHECK_OK(extended);
  if (extended.ok()) {
    CHECK_EQ(extended.value().page_id(), page_count);
  }
}

TEST(test_paged_allocator_puts_a_released_chain_in_front_of_the_list) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  const std::vector<PageId> first_chain = MakeChain(database, "a", 2);
  const std::vector<PageId> second_chain = MakeChain(database, "b", 3);
  CHECK_EQ(first_chain.size(), std::size_t{2});
  CHECK_EQ(second_chain.size(), std::size_t{3});
  if (first_chain.size() != 2 || second_chain.size() != 3) {
    return;
  }
  CHECK_OK(database.allocator().Release(first_chain.front()));
  CHECK_OK(database.allocator().Release(second_chain.front()));
  std::vector<PageId> expected = second_chain;
  expected.insert(expected.end(), first_chain.begin(), first_chain.end());
  CHECK_EQ(FreePagesOf(database), expected);
}

TEST(test_paged_allocator_refuses_a_chain_that_is_not_a_heap_chain) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  auto& allocator = database.allocator();
  auto& buffer = database.buffer();

  // Every table is made before the first release, because creating a table
  // takes its pages from the free list.
  const std::vector<PageId> listed = MakeChain(database, "listed", 2);
  std::vector<std::vector<PageId>> victims;
  for (int victim = 0; victim < 5; ++victim) {
    victims.push_back(
        MakeChain(database, "victim" + std::to_string(victim), 3));
  }
  CHECK_EQ(listed.size(), std::size_t{2});
  if (listed.size() != 2) {
    return;
  }
  // A list that already holds a chain, so that "unchanged" is not just 0.
  CHECK_OK(allocator.Release(listed.front()));
  const PageId head_before = FreeListHeadOf(buffer);
  CHECK_EQ(head_before, listed.front());

  struct Case {
    const char* name;
    PageId tail_next;  // what the last page of the chain is made to point at
    const char* message;
  };
  const Case cases[] = {
      {"cycle", kInvalidPageId, "page_type 3"},  // replaced below
      {"file header", 0, "page 0"},
      {"system table page", 1, "page 1"},
      {"free page", listed.front(), "page_type 3"},
      {"past the end", 9999, "past the end of the file"},
  };
  int index = 0;
  for (const Case& test_case : cases) {
    const std::vector<PageId>& chain =
        victims[static_cast<std::size_t>(index++)];
    if (chain.size() != 3) {
      return;
    }
    PageId target = test_case.tail_next;
    if (std::string(test_case.name) == "cycle") {
      target = chain.front();
    }
    SetNext(buffer, chain.back(), target);
    const auto released = allocator.Release(chain.front());
    CHECK_ERROR(released, kCorrupt);
    if (std::string(test_case.name) != "cycle") {
      CHECK_MESSAGE_CONTAINS(released, test_case.message);
    }
    CHECK_EQ(FreeListHeadOf(buffer), head_before);
  }
  CHECK_ERROR(allocator.Release(1), kInvalidArgument);
  CHECK_ERROR(allocator.Release(2), kInvalidArgument);
  CHECK_EQ(FreeListHeadOf(buffer), head_before);
}

TEST(test_paged_allocator_keeps_the_list_whole_when_io_fails) {
  // A database with a 4-page table to release and a 2-page list, closed so
  // that every page is in the store.
  std::unique_ptr<strata::paged::MemoryPageStore> base_copy;
  PageId chain_first = 0;
  std::vector<PageId> chain;
  std::vector<PageId> old_list;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    const std::vector<PageId> listed = MakeChain(*env.database, "listed", 2);
    chain = MakeChain(*env.database, "victim", 4);
    CHECK_EQ(listed.size(), std::size_t{2});
    CHECK_EQ(chain.size(), std::size_t{4});
    if (listed.size() != 2 || chain.size() != 4) {
      return;
    }
    CHECK_OK(env.database->allocator().Release(listed.front()));
    old_list = listed;
    chain_first = chain.front();
    CHECK_OK(env.database->Close());
    base_copy = strata::testing::CopyOfStore(*env.store);
  }

  const Operation operations[] = {Operation::kRead, Operation::kWrite};
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 200 && !has_succeeded; ++nth_call) {
      auto opened = strata::testing::OpenCopy(*base_copy, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      opened.store->InjectFailure(operation, nth_call);
      const strata::paged::Status release =
          database.allocator().Release(chain_first);
      const strata::paged::Status write_back =
          release.has_value() ? strata::paged::Status()
                              : database.buffer().WriteBackAll();
      opened.store->ClearInjectedFailures();
      const bool is_applied = !release.has_value();
      if (!release.has_value() && !write_back.has_value()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(release.has_value() ? release : write_back, kIo);
        ++failures_seen;
      }
      // Whatever happened, the list on disk after a clean write-out is whole.
      CHECK_OK(database.buffer().FlushAll());
      const auto reopened = strata::testing::OpenCopy(*opened.store, 16);
      CHECK_OK(reopened.database == nullptr
                   ? strata::paged::Status(reopened.error)
                   : strata::paged::Status());
      if (reopened.database == nullptr) {
        return;
      }
      const std::vector<PageId> list = FreePagesOf(*reopened.database);
      if (is_applied) {
        std::vector<PageId> expected = chain;
        expected.insert(expected.end(), old_list.begin(), old_list.end());
        CHECK_EQ(list, expected);
      } else {
        CHECK_EQ(list, old_list);
      }
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
    }
    CHECK(has_succeeded);
    // The failures really happened: the sweep is not a run of successes.
    CHECK(failures_seen > 0);
  }

  // Allocate under the same failures leaves the list as it was. The four
  // frames hold dirty pages of the table first, so that reading the file
  // header and the head page has to write one of them back.
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 50 && !has_succeeded; ++nth_call) {
      auto opened = strata::testing::OpenCopy(*base_copy, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      for (PageId page_id : chain) {
        auto fetched =
            database.buffer().FetchPage(page_id, LatchMode::kExclusive);
        CHECK_OK(fetched);
        if (fetched.ok()) {
          fetched.value().MarkDirty();
        }
      }
      opened.store->InjectFailure(operation, nth_call);
      auto page = database.allocator().Allocate();
      opened.store->ClearInjectedFailures();
      if (page.ok()) {
        has_succeeded = true;
        CHECK_EQ(page.value().page_id(), old_list.front());
      } else {
        CHECK_ERROR(page, kIo);
        ++failures_seen;
        CHECK_EQ(FreeListHeadOf(database.buffer()), old_list.front());
        CHECK_EQ(FreePagesOf(database), old_list);
      }
    }
    CHECK(has_succeeded);
    CHECK(failures_seen > 0);
  }
}

TEST(test_paged_allocator_refuses_a_successor_below_page_3) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  const std::vector<PageId> chain = MakeChain(database, "t", 3);
  CHECK_EQ(chain.size(), std::size_t{3});
  if (chain.size() != 3) {
    return;
  }
  CHECK_OK(database.allocator().Release(chain.front()));
  // The first free page now points at page 0, which the file header would read
  // as the end of the list, and then at a system table page.
  const PageId targets[] = {0, 1};
  for (PageId target : targets) {
    SetNext(database.buffer(), chain.front(), target);
    const auto allocated = database.allocator().Allocate();
    CHECK_ERROR(allocated, kCorrupt);
    CHECK_MESSAGE_CONTAINS(allocated, "page " + std::to_string(target));
    CHECK_EQ(FreeListHeadOf(database.buffer()), chain.front());
  }
}

namespace {

enum class NewPageKind { kHeap, kIndexNode, kUnformatted };

// A page that no chain links, taken from BufferManager::NewPage and not from
// Allocate, so the free list is not touched.
PageId AddPage(strata::paged::BufferManager& buffer, NewPageKind kind) {
  auto created = buffer.NewPage();
  CHECK_OK(created);
  if (!created.ok()) {
    return kInvalidPageId;
  }
  PageGuard guard = std::move(created).value();
  if (kind == NewPageKind::kHeap) {
    SlottedPage::Format(guard.mutable_data(), guard.page_id(), 77);
  } else if (kind == NewPageKind::kIndexNode) {
    strata::paged::FormatLeaf(guard.mutable_data(), guard.page_id(), 77);
  }
  guard.MarkDirty();
  return guard.page_id();
}

// The bytes of every page, read through the pool.
std::vector<strata::testing::Page> SnapshotOf(
    strata::paged::BufferManager& buffer) {
  std::vector<strata::testing::Page> pages;
  for (PageId page_id = 0; page_id < buffer.PageCount(); ++page_id) {
    auto fetched = buffer.FetchPage(page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    strata::testing::Page copy{};
    if (fetched.ok()) {
      std::copy(fetched.value().data(),
                fetched.value().data() + strata::paged::kPageSize,
                copy.begin());
    }
    pages.push_back(copy);
  }
  return pages;
}

}  // namespace

TEST(test_paged_allocator_release_pages_frees_an_explicit_list) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  const std::vector<PageId> earlier = MakeChain(database, "t", 2);
  CHECK_EQ(earlier.size(), std::size_t{2});
  CHECK_OK(database.allocator().Release(earlier.front()));

  // Three heap pages and two index nodes, in an order that is not page order.
  std::vector<PageId> created;
  const NewPageKind kinds[] = {NewPageKind::kHeap, NewPageKind::kIndexNode,
                               NewPageKind::kHeap, NewPageKind::kIndexNode,
                               NewPageKind::kHeap};
  for (NewPageKind kind : kinds) {
    created.push_back(AddPage(database.buffer(), kind));
  }
  const std::size_t order[] = {3, 0, 4, 2, 1};
  std::vector<PageId> listed;
  for (std::size_t position : order) {
    listed.push_back(created[position]);
  }
  const PageId page_count = database.buffer().PageCount();

  CHECK_OK(database.allocator().ReleasePages(listed));
  CHECK_OK(database.buffer().FlushAll());

  std::vector<PageId> expected = listed;
  expected.insert(expected.end(), earlier.begin(), earlier.end());
  CHECK_EQ(FreePagesOf(database), expected);
  CHECK_EQ(FreeListHeadOf(database.buffer()), listed.front());
  for (PageId page_id : listed) {
    auto fetched = database.buffer().FetchPage(page_id, LatchMode::kShared);
    CHECK_OK(fetched);
    if (fetched.ok()) {
      const SlottedPageView view(fetched.value().data());
      CHECK(view.page_type() == PageType::kFree);
      CHECK_EQ(view.table_id(), std::uint32_t{0});
      CHECK_EQ(view.slot_count(), 0);
    }
  }

  std::vector<PageId> reused;
  for (std::size_t call = 0; call < expected.size(); ++call) {
    auto page = database.allocator().Allocate();
    CHECK_OK(page);
    if (!page.ok()) {
      return;
    }
    reused.push_back(page.value().page_id());
  }
  CHECK_EQ(reused, expected);
  CHECK_EQ(database.buffer().PageCount(), page_count);
  CHECK_EQ(FreeListHeadOf(database.buffer()), PageId{0});
  // An empty list changes nothing.
  CHECK_OK(database.allocator().ReleasePages({}));
  CHECK_EQ(FreeListHeadOf(database.buffer()), PageId{0});
}

TEST(test_paged_allocator_release_pages_refuses_an_invalid_list) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  const std::vector<PageId> free_chain = MakeChain(database, "t", 2);
  CHECK_EQ(free_chain.size(), std::size_t{2});
  CHECK_OK(database.allocator().Release(free_chain.front()));
  const PageId heap_page = AddPage(database.buffer(), NewPageKind::kHeap);
  const PageId other_heap_page = AddPage(database.buffer(), NewPageKind::kHeap);
  const PageId unformatted_page =
      AddPage(database.buffer(), NewPageKind::kUnformatted);
  const PageId page_count = database.buffer().PageCount();
  CHECK_OK(database.buffer().FlushAll());

  struct Refusal {
    std::vector<PageId> pages;
    const char* code;
    std::string named_page;
  };
  const std::vector<Refusal> refusals = {
      {{heap_page, other_heap_page, heap_page},
       "kCorrupt",
       "page " + std::to_string(heap_page)},
      {{heap_page, 2}, "kInvalidArgument", "page 2"},
      {{0}, "kInvalidArgument", "page 0"},
      {{heap_page, page_count + 5},
       "kCorrupt",
       "page " + std::to_string(page_count + 5)},
      {{heap_page, free_chain.back()},
       "kCorrupt",
       "page " + std::to_string(free_chain.back())},
      {{unformatted_page},
       "kCorrupt",
       "page " + std::to_string(unformatted_page)}};

  const PageId head_before = FreeListHeadOf(database.buffer());
  const std::vector<strata::testing::Page> pages_before =
      SnapshotOf(database.buffer());
  for (const Refusal& refusal : refusals) {
    const auto status = database.allocator().ReleasePages(refusal.pages);
    CHECK_EQ(strata::testing::ErrorName(status), std::string(refusal.code));
    CHECK_MESSAGE_CONTAINS(status, refusal.named_page);
    CHECK_EQ(FreeListHeadOf(database.buffer()), head_before);
    CHECK(SnapshotOf(database.buffer()) == pages_before);
    CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  }
}

TEST(test_paged_allocator_release_pages_keeps_the_list_whole_when_io_fails) {
  // A closed database with a 2-page free list and six pages that no chain
  // links, so that every page is in the store.
  std::unique_ptr<strata::paged::MemoryPageStore> base_copy;
  std::vector<PageId> listed;
  std::vector<PageId> old_list;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    Database& database = *env.database;
    old_list = MakeChain(database, "earlier", 2);
    CHECK_EQ(old_list.size(), std::size_t{2});
    if (old_list.size() != 2) {
      return;
    }
    CHECK_OK(database.allocator().Release(old_list.front()));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kHeap));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kIndexNode));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kHeap));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kIndexNode));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kHeap));
    listed.push_back(AddPage(database.buffer(), NewPageKind::kIndexNode));
    CHECK_OK(database.Close());
    base_copy = strata::testing::CopyOfStore(*env.store);
  }

  // The pool has 4 frames and the list has 6 pages, so the pages that the
  // check reads first are evicted and read again while the list is formatted.
  const Operation operations[] = {Operation::kRead, Operation::kWrite};
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 200 && !has_succeeded; ++nth_call) {
      auto opened = strata::testing::OpenCopy(*base_copy, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      opened.store->InjectFailure(operation, nth_call);
      const strata::paged::Status release =
          database.allocator().ReleasePages(listed);
      const strata::paged::Status write_back =
          release.has_value() ? strata::paged::Status()
                              : database.buffer().WriteBackAll();
      opened.store->ClearInjectedFailures();
      const bool is_applied = !release.has_value();
      if (!release.has_value() && !write_back.has_value()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(release.has_value() ? release : write_back, kIo);
        ++failures_seen;
      }
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      // The free list on disk after a clean write-out is whole: the new list
      // when the call applied, and the old list when it failed, because the
      // head changes last and the pages already formatted are only unreachable.
      CHECK_OK(database.buffer().FlushAll());
      const auto reopened = strata::testing::OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database == nullptr) {
        return;
      }
      std::vector<PageId> expected = old_list;
      if (is_applied) {
        expected = listed;
        expected.insert(expected.end(), old_list.begin(), old_list.end());
      }
      CHECK_EQ(FreePagesOf(*reopened.database), expected);
    }
    CHECK(has_succeeded);
    // The failures really happened: the sweep is not a run of successes.
    CHECK(failures_seen > 0);
  }
}
