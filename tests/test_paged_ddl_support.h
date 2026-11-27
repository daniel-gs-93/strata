#ifndef STRATA_TESTS_TEST_PAGED_DDL_SUPPORT_H_
#define STRATA_TESTS_TEST_PAGED_DDL_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/table_heap.h"
#include "test_harness.h"
#include "test_paged_support.h"

// Helpers for the tests of the free list and of the paged DDL (ADR-0045,
// ADR-0046): a Database over a MemoryPageStore that the test can copy, and
// readers that follow a page chain or the free list through the pool.

namespace strata::testing {

struct MemoryDatabase {
  paged::MemoryPageStore* store = nullptr;  // owned by database
  std::unique_ptr<paged::Database> database;
};

// A new empty database with the given number of frames.
inline MemoryDatabase OpenNewMemoryDatabase(std::size_t frames = 16) {
  auto store = std::make_unique<paged::MemoryPageStore>();
  paged::MemoryPageStore* const observer = store.get();
  paged::DatabaseOptions options;
  options.pool_frames = frames;
  auto opened = paged::Database::OpenWithStore(std::move(store), options);
  CHECK_OK(opened);
  MemoryDatabase result;
  if (opened.ok()) {
    result.store = observer;
    result.database =
        std::make_unique<paged::Database>(std::move(opened).value());
  }
  return result;
}

// A copy of the bytes of a store: what a second process would read from the
// file after the writes in `source` have reached it.
inline std::unique_ptr<paged::MemoryPageStore> CopyOfStore(
    const paged::MemoryPageStore& source) {
  auto copy = std::make_unique<paged::MemoryPageStore>(source.PageCount());
  for (paged::PageId page_id = 0; page_id < source.PageCount(); ++page_id) {
    std::memcpy(copy->MutablePage(page_id), source.PageBytes(page_id),
                paged::kPageSize);
  }
  return copy;
}

// Opens a Database over a copy of source, with a cold pool of `frames`.
// database is null when the open fails; `error` then says why.
struct OpenedCopy {
  paged::MemoryPageStore* store = nullptr;
  std::unique_ptr<paged::Database> database;
  paged::PageError error{paged::PageErrorCode::kIo, ""};
};

inline OpenedCopy OpenCopy(const paged::MemoryPageStore& source,
                           std::size_t frames = 16) {
  std::unique_ptr<paged::MemoryPageStore> copy = CopyOfStore(source);
  paged::MemoryPageStore* const observer = copy.get();
  paged::DatabaseOptions options;
  options.pool_frames = frames;
  auto opened = paged::Database::OpenWithStore(std::move(copy), options);
  OpenedCopy result;
  if (!opened.ok()) {
    result.error = opened.error();
    return result;
  }
  result.store = observer;
  result.database =
      std::make_unique<paged::Database>(std::move(opened).value());
  return result;
}

inline domain::TableSchema BodySchema(const std::string& name) {
  return MakeSchema(name, {Column("id", domain::DataType::kInt),
                           Column("body", domain::DataType::kText)});
}

inline domain::Row BodyRow(std::int64_t id, std::size_t body_length) {
  return domain::Row{domain::Value(id),
                     domain::Value(std::string(body_length, 'b'))};
}

inline paged::PageType PageTypeOf(paged::BufferManager& buffer,
                                  paged::PageId page_id) {
  auto fetched = buffer.FetchPage(page_id, paged::LatchMode::kShared);
  CHECK_OK(fetched);
  if (!fetched.ok()) {
    return paged::PageType::kUnformatted;
  }
  return paged::SlottedPageView(fetched.value().data()).page_type();
}

inline paged::PageId FreeListHeadOf(paged::BufferManager& buffer) {
  auto fetched = buffer.FetchPage(0, paged::LatchMode::kShared);
  CHECK_OK(fetched);
  if (!fetched.ok()) {
    return 0;
  }
  return paged::ReadLe32(fetched.value().data() + paged::layout::kFreeListHead);
}

// The pages of the chain that starts at `first_page`, in chain order.
inline std::vector<paged::PageId> ChainOf(paged::BufferManager& buffer,
                                          paged::PageId first_page) {
  std::vector<paged::PageId> pages;
  paged::PageId current = first_page;
  while (current != paged::kInvalidPageId && pages.size() <= 100000) {
    pages.push_back(current);
    auto fetched = buffer.FetchPage(current, paged::LatchMode::kShared);
    CHECK_OK(fetched);
    if (!fetched.ok()) {
      break;
    }
    current = paged::SlottedPageView(fetched.value().data()).next_page_id();
  }
  return pages;
}

// Creates table `name` with BodySchema and inserts rows of 2000 bytes (two per
// page) until its chain has `page_count` pages. Returns the pages of the chain.
inline std::vector<paged::PageId> MakeChain(paged::Database& database,
                                            const std::string& name,
                                            std::size_t page_count) {
  CHECK_OK(database.CreateTable(BodySchema(name)));
  auto heap = database.Table(name);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return {};
  }
  std::int64_t id = 1;
  while (ChainOf(database.buffer(), heap.value()->first_page()).size() <
         page_count) {
    CHECK_OK(heap.value()->Insert(BodyRow(id++, 2000)));
    if (id > 1000) {
      break;
    }
  }
  return ChainOf(database.buffer(), heap.value()->first_page());
}

// Creates table `name` (BodySchema) and inserts `rows` rows of `body_length`
// bytes into its heap and, with the Rid of each, into its index, so that the
// two agree. The tests that call it work on Database and not on the engine,
// which keeps the index of a table it writes.
inline void FillIndexedTable(paged::Database& database, const std::string& name,
                             std::int64_t rows, std::size_t body_length) {
  CHECK_OK(database.CreateTable(BodySchema(name)));
  auto heap = database.Table(name);
  auto tree = database.Index(name);
  CHECK_OK(heap);
  CHECK_OK(tree);
  if (!heap.ok() || !tree.ok() || tree.value() == nullptr) {
    CHECK(false);
    return;
  }
  for (std::int64_t id = 1; id <= rows; ++id) {
    auto rid = heap.value()->Insert(BodyRow(id, body_length));
    CHECK_OK(rid);
    if (!rid.ok()) {
      return;
    }
    CHECK_OK(tree.value()->Insert(id, rid.value()));
  }
}

// The pages of the index of a table in the order of BTree::Pages, or none when
// the table has no index.
inline std::vector<paged::PageId> IndexPagesOf(paged::Database& database,
                                               const std::string& name) {
  std::vector<paged::PageId> pages;
  auto tree = database.Index(name);
  CHECK_OK(tree);
  if (!tree.ok() || tree.value() == nullptr) {
    return pages;
  }
  auto listed = tree.value()->Pages();
  CHECK_OK(listed);
  if (listed.ok()) {
    pages = listed.value();
  }
  return pages;
}

// True when the file header, the system tables, the live tables with their
// indexes and the free list together name every page of the store exactly
// once.
inline bool EveryPageIsAccountedFor(paged::Database& database) {
  std::vector<paged::PageId> pages{0};
  for (paged::PageId system_first : {paged::PageId{1}, paged::PageId{2}}) {
    for (paged::PageId page : ChainOf(database.buffer(), system_first)) {
      pages.push_back(page);
    }
  }
  for (const std::string& name : database.catalog().TableNames()) {
    const auto first = database.catalog().FirstPage(name);
    if (!first.has_value()) {
      return false;
    }
    for (paged::PageId page : ChainOf(database.buffer(), *first)) {
      pages.push_back(page);
    }
    for (paged::PageId page : IndexPagesOf(database, name)) {
      pages.push_back(page);
    }
  }
  auto free_pages = database.allocator().FreePages();
  CHECK_OK(free_pages);
  if (!free_pages.ok()) {
    return false;
  }
  for (paged::PageId page : free_pages.value()) {
    pages.push_back(page);
  }
  const std::set<paged::PageId> unique(pages.begin(), pages.end());
  return unique.size() == pages.size() &&
         pages.size() == database.buffer().PageCount();
}

// Every row of the table, in scan order.
inline std::vector<domain::Row> RowsOf(paged::Database& database,
                                       const std::string& name) {
  std::vector<domain::Row> rows;
  auto heap = database.Table(name);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return rows;
  }
  paged::HeapScan scan = heap.value()->Scan();
  while (true) {
    auto next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    rows.push_back(std::move(next.value()->row));
  }
  return rows;
}

}  // namespace strata::testing

#endif  // STRATA_TESTS_TEST_PAGED_DDL_SUPPORT_H_
