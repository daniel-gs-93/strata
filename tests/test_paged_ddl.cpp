#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/index_inspector.h"
#include "strata/domain/value.h"
#include "strata/paged/btree_node.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/heap_access.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_engine.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"

// Database::AddColumn, Database::DropTable and the free list at open (design
// document 2.6, 2.10, 2.11, 3.1, 3.4; ADR-0045, ADR-0046).

namespace {

using strata::domain::ColumnDef;
using strata::domain::DataType;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::Database;
using strata::paged::kPageSize;
using strata::paged::MemoryPageStore;
using strata::paged::PageId;
using strata::paged::SlottedPage;
using strata::testing::BodyRow;
using strata::testing::BodySchema;
using strata::testing::ChainOf;
using strata::testing::EveryPageIsAccountedFor;
using strata::testing::IndexPagesOf;
using strata::testing::MemoryDatabase;
using strata::testing::OpenCopy;
using strata::testing::OpenNewMemoryDatabase;
using strata::testing::RowsOf;
using Operation = strata::paged::MemoryPageStore::Operation;

constexpr const char* kNotReclaimed =
    "; statement was applied but its pages were not reclaimed";

std::vector<PageId> FreePagesOf(Database& database) {
  auto free_pages = database.allocator().FreePages();
  CHECK_OK(free_pages);
  return free_pages.ok() ? free_pages.value() : std::vector<PageId>{};
}

// Creates table `name` (BodySchema) with `row_count` rows of `body_length`.
void FillTable(Database& database, const std::string& name,
               std::int64_t row_count, std::size_t body_length) {
  CHECK_OK(database.CreateTable(BodySchema(name)));
  auto heap = database.Table(name);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  for (std::int64_t id = 1; id <= row_count; ++id) {
    CHECK_OK(heap.value()->Insert(BodyRow(id, body_length)));
  }
}

std::int64_t IdOfRow(const Row& row) { return std::get<std::int64_t>(row[0]); }

void SortById(std::vector<Row>* rows) { std::ranges::sort(*rows, {}, IdOfRow); }

std::vector<std::vector<std::uint8_t>> ImagesOf(const MemoryPageStore& store) {
  std::vector<std::vector<std::uint8_t>> images;
  for (PageId page_id = 0; page_id < store.PageCount(); ++page_id) {
    images.emplace_back(store.PageBytes(page_id),
                        store.PageBytes(page_id) + kPageSize);
  }
  return images;
}

void SealAndStore(MemoryPageStore& store, PageId page_id) {
  SlottedPage::SealChecksum(store.MutablePage(page_id));
}

}  // namespace

TEST(test_paged_database_verifies_the_free_list_at_open) {
  // A closed database with a released chain of 3 pages and a live table.
  std::unique_ptr<MemoryPageStore> base;
  std::vector<PageId> chain;
  PageId live_first = 0;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    chain = strata::testing::MakeChain(*env.database, "gone", 3);
    FillTable(*env.database, "live", 4, 2000);
    live_first = *env.database->catalog().FirstPage("live");
    if (chain.size() != 3) {
      return;
    }
    CHECK_OK(env.database->allocator().Release(chain.front()));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const PageId page_count = base->PageCount();

  {
    auto healthy = OpenCopy(*base);
    CHECK(healthy.database != nullptr);
    if (healthy.database != nullptr) {
      CHECK_EQ(FreePagesOf(*healthy.database), chain);
    }
  }

  struct Damage {
    const char* name;
    PageId head;       // free_list_head to write, or kInvalidPageId to keep
    PageId next_page;  // next_page_id of chain[1] to write, or kInvalidPageId
    const char* message;
  };
  const Damage damages[] = {
      {"empty", 0, strata::paged::kInvalidPageId, nullptr},
      {"past the end", page_count + 5, strata::paged::kInvalidPageId,
       "past the end of the file"},
      {"the file header", 0xFFFFFFFE, strata::paged::kInvalidPageId, nullptr},
      {"a live table page", live_first, strata::paged::kInvalidPageId,
       "page_type 2 instead of 3"},
      {"a loop", strata::paged::kInvalidPageId, chain[1], "visits page"},
      {"a heap page in the middle", strata::paged::kInvalidPageId, live_first,
       "page_type 2 instead of 3"},
  };
  for (const Damage& damage : damages) {
    auto copy = strata::testing::CopyOfStore(*base);
    if (damage.head != strata::paged::kInvalidPageId &&
        damage.head != 0xFFFFFFFE) {
      strata::paged::WriteLe32(
          copy->MutablePage(0) + strata::paged::layout::kFreeListHead,
          damage.head);
      SealAndStore(*copy, 0);
    }
    if (std::string(damage.name) == "the file header") {
      // free_list_head 0 is "empty", so the header is reached through a link.
      strata::paged::WriteLe32(
          copy->MutablePage(chain[2]) + strata::paged::layout::kNextPageId, 0);
      SealAndStore(*copy, chain[2]);
    }
    if (damage.next_page != strata::paged::kInvalidPageId) {
      strata::paged::WriteLe32(
          copy->MutablePage(chain[1]) + strata::paged::layout::kNextPageId,
          damage.next_page);
      SealAndStore(*copy, chain[1]);
    }
    auto opened = OpenCopy(*copy);
    if (std::string(damage.name) == "empty") {
      CHECK(opened.database != nullptr);
      if (opened.database != nullptr) {
        CHECK(FreePagesOf(*opened.database).empty());
      }
      continue;
    }
    CHECK(opened.database == nullptr);
    CHECK(opened.error.code == strata::paged::PageErrorCode::kCorrupt);
    if (damage.message != nullptr) {
      CHECK(opened.error.message.find(damage.message) != std::string::npos);
    } else {
      CHECK(opened.error.message.find("page_type 1 instead of 3") !=
            std::string::npos);
    }
  }
}

TEST(test_paged_ddl_add_column_rewrites_rows_and_survives_reopen) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(strata::testing::MakeSchema(
      "t", {strata::testing::Column("id", DataType::kInt),
            strata::testing::Column("code", DataType::kText),
            strata::testing::Column("body", DataType::kText)})));
  {
    auto heap = database.Table("t");
    CHECK_OK(heap);
    if (!heap.ok()) {
      return;
    }
    for (std::int64_t id = 1; id <= 300; ++id) {
      CHECK_OK(heap.value()->Insert(Row{
          Value(id), Value("code-" + std::to_string(id)),
          Value(std::string(static_cast<std::size_t>(id % 90) + 100, 'x'))}));
    }
  }
  std::vector<Row> expected = RowsOf(database, "t");
  SortById(&expected);
  CHECK_EQ(expected.size(), std::size_t{300});
  const auto table_id = database.catalog().TableId("t");

  const ColumnDef columns[] = {
      {"a", DataType::kInt, Value(std::int64_t{7})},
      {"b", DataType::kDouble, Value(1.5)},
      {"c", DataType::kText, Value(std::string("x"))},
  };
  bool is_first = true;
  for (const ColumnDef& column : columns) {
    const PageId old_first = *database.catalog().FirstPage("T");
    const std::vector<PageId> old_chain = ChainOf(database.buffer(), old_first);
    // The pages of the old tree go on the free list after the old chain, so
    // they come first in the list.
    std::vector<PageId> old_pages = IndexPagesOf(database, "t");
    old_pages.insert(old_pages.end(), old_chain.begin(), old_chain.end());
    auto altered = database.AddColumn(is_first ? "t" : "T", column);
    CHECK_OK(altered);
    if (!altered.ok()) {
      return;
    }
    for (Row& row : expected) {
      row.push_back(*column.default_value);
    }
    std::vector<Row> actual = RowsOf(database, "t");
    SortById(&actual);
    CHECK(actual == expected);
    const auto schema = database.catalog().FindTable("t");
    CHECK(schema.has_value());
    if (schema.has_value()) {
      CHECK(schema->columns.back().name == column.name);
      CHECK(schema->primary_key_index == 0);
    }
    CHECK(database.catalog().TableId("t") == table_id);
    CHECK(EveryPageIsAccountedFor(database));
    if (is_first) {
      CHECK_EQ(FreePagesOf(database), old_pages);
    }
    is_first = false;
  }

  CHECK_OK(database.Close());
  auto reopened = OpenCopy(*env.store);
  CHECK(reopened.database != nullptr);
  if (reopened.database == nullptr) {
    return;
  }
  const auto schema = reopened.database->catalog().FindTable("t");
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->columns.size(), std::size_t{6});
  }
  std::vector<Row> after = RowsOf(*reopened.database, "t");
  SortById(&after);
  CHECK(after == expected);
  CHECK(EveryPageIsAccountedFor(*reopened.database));
}

TEST(test_paged_ddl_add_column_boundaries) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  // A row of 8 + (2 + 4042) = 4052 bytes fits one more INT (8 bytes); a row of
  // 4053 bytes does not.
  CHECK_OK(database.CreateTable(BodySchema("fits")));
  CHECK_OK(database.CreateTable(BodySchema("too_long")));
  CHECK_OK(database.CreateTable(BodySchema("plain")));
  CHECK_OK(database.Table("fits").value()->Insert(BodyRow(1, 4042)));
  CHECK_OK(database.Table("too_long").value()->Insert(BodyRow(1, 4043)));
  CHECK_OK(database.Table("plain").value()->Insert(BodyRow(1, 10)));
  CHECK_OK(database.buffer().FlushAll());

  const ColumnDef int_column{"extra", DataType::kInt, Value(std::int64_t{0})};
  CHECK_OK(database.AddColumn("fits", int_column));

  CHECK_OK(database.buffer().FlushAll());
  const auto before = ImagesOf(*env.store);
  const std::vector<PageId> free_before = FreePagesOf(database);
  const auto too_large = database.AddColumn("too_long", int_column);
  CHECK_ERROR(too_large, kTooLarge);
  const auto long_default = database.AddColumn(
      "plain",
      ColumnDef{"note", DataType::kText, Value(std::string(4100, 'x'))});
  CHECK_ERROR(long_default, kInvalidArgument);
  CHECK_MESSAGE_CONTAINS(long_default, "column note");
  CHECK_OK(database.buffer().FlushAll());
  CHECK(ImagesOf(*env.store) == before);
  CHECK_EQ(FreePagesOf(database), free_before);
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "too_long").size(), std::size_t{1});
  CHECK_EQ(database.catalog().FindTable("too_long")->columns.size(),
           std::size_t{2});
}

TEST(test_paged_ddl_add_column_fails_cleanly) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "spare", 12, 2000);  // about 6 pages, then freed
    FillTable(*env.database, "t", 40, 2000);      // about 20 pages
    CHECK_OK(env.database->DropTable("spare"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const ColumnDef column{"c", DataType::kInt, Value(std::int64_t{7})};
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  int not_reclaimed_cases = 0;
  int rolled_back_cases = 0;
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 600 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const std::vector<Row> old_rows = RowsOf(database, "t");
      opened.store->InjectFailure(operation, nth_call);
      const auto result = database.AddColumn("t", column);
      opened.store->ClearInjectedFailures();
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      const bool is_not_reclaimed =
          !result.ok() && result.error().message.ends_with(kNotReclaimed);
      if (result.ok()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(result, kIo);
        ++failures_seen;
      }
      const bool is_new = result.ok() || is_not_reclaimed;
      not_reclaimed_cases += is_not_reclaimed ? 1 : 0;
      rolled_back_cases += (!result.ok() && !is_not_reclaimed) ? 1 : 0;
      const auto schema = database.catalog().FindTable("t");
      CHECK(schema.has_value());
      if (schema.has_value()) {
        CHECK_EQ(schema->columns.size(),
                 is_new ? std::size_t{3} : std::size_t{2});
      }
      std::vector<Row> expected = old_rows;
      if (is_new) {
        for (Row& row : expected) {
          row.push_back(*column.default_value);
        }
      }
      std::vector<Row> actual = RowsOf(database, "t");
      SortById(&actual);
      SortById(&expected);
      CHECK(actual == expected);

      CHECK_OK(database.Close());
      auto reopened = OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database != nullptr) {
        std::vector<Row> after = RowsOf(*reopened.database, "t");
        SortById(&after);
        CHECK(after == expected);
      }
    }
    CHECK(has_succeeded);
    CHECK(failures_seen > 0);
  }
  // Both kinds of failure were reached: before the switch and after it.
  CHECK(not_reclaimed_cases > 0);
  CHECK(rolled_back_cases > 0);
}

TEST(test_paged_ddl_drop_table_frees_pages_and_reuses_them) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  FillTable(database, "a", 20, 2000);
  FillTable(database, "b", 20, 2000);
  FillTable(database, "c", 20, 2000);
  const std::vector<PageId> b_chain =
      ChainOf(database.buffer(), *database.catalog().FirstPage("b"));
  // The pages of the index go on the free list after the chain, so they come
  // first in the list.
  std::vector<PageId> b_pages = IndexPagesOf(database, "b");
  b_pages.insert(b_pages.end(), b_chain.begin(), b_chain.end());
  const std::vector<Row> a_rows = RowsOf(database, "a");
  const std::vector<Row> c_rows = RowsOf(database, "c");
  const PageId page_count = database.buffer().PageCount();
  const auto b_id = database.catalog().TableId("b");

  CHECK_OK(database.DropTable("B"));
  CHECK_ERROR(database.Table("b"), kNotFound);
  CHECK_EQ(database.catalog().TableNames(),
           (std::vector<std::string>{"a", "c"}));
  CHECK(RowsOf(database, "a") == a_rows);
  CHECK(RowsOf(database, "c") == c_rows);
  CHECK_EQ(FreePagesOf(database), b_pages);
  CHECK(EveryPageIsAccountedFor(database));

  CHECK_OK(database.Close());
  auto reopened = OpenCopy(*env.store);
  CHECK(reopened.database != nullptr);
  if (reopened.database == nullptr) {
    return;
  }
  Database& second = *reopened.database;
  CHECK_ERROR(second.Table("b"), kNotFound);
  CHECK_EQ(FreePagesOf(second), b_pages);

  FillTable(second, "d", 20, 2000);
  CHECK(second.catalog().TableId("d").has_value());
  if (second.catalog().TableId("d").has_value() && b_id.has_value()) {
    CHECK(*second.catalog().TableId("d") > *second.catalog().TableId("c"));
    CHECK(*second.catalog().TableId("d") != *b_id);
  }
  CHECK_EQ(second.buffer().PageCount(), page_count);
  CHECK(EveryPageIsAccountedFor(second));
  CHECK_EQ(RowsOf(second, "d").size(), std::size_t{20});
}

TEST(test_paged_ddl_drop_table_fails_cleanly) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "keep", 5, 2000);
    FillTable(*env.database, "spare", 12, 2000);  // about 6 pages, then freed
    FillTable(*env.database, "t", 40, 2000);      // about 20 pages
    CHECK_OK(env.database->DropTable("spare"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  int not_reclaimed_cases = 0;
  int rolled_back_cases = 0;
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 600 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 4);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const std::vector<Row> old_rows = RowsOf(database, "t");
      opened.store->InjectFailure(operation, nth_call);
      const auto status = database.DropTable("t");
      opened.store->ClearInjectedFailures();
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      const bool is_not_reclaimed =
          status.has_value() && status->message.ends_with(kNotReclaimed);
      if (!status.has_value()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(status, kIo);
        ++failures_seen;
      }
      const bool is_gone = !status.has_value() || is_not_reclaimed;
      not_reclaimed_cases += is_not_reclaimed ? 1 : 0;
      rolled_back_cases += (status.has_value() && !is_not_reclaimed) ? 1 : 0;
      CHECK_EQ(database.catalog().FindTable("t").has_value(), !is_gone);
      if (!is_gone) {
        std::vector<Row> actual = RowsOf(database, "t");
        std::vector<Row> expected = old_rows;
        SortById(&actual);
        SortById(&expected);
        CHECK(actual == expected);
      }
      CHECK_EQ(RowsOf(database, "keep").size(), std::size_t{5});
      CHECK_OK(database.Close());
      auto reopened = OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database != nullptr) {
        CHECK_EQ(reopened.database->catalog().FindTable("t").has_value(),
                 !is_gone);
        CHECK_EQ(RowsOf(*reopened.database, "keep").size(), std::size_t{5});
      }
    }
    CHECK(has_succeeded);
    // DROP TABLE allocates no page, so an allocation failure never fires.
    CHECK(failures_seen > 0 || operation == Operation::kAllocate);
  }
  CHECK(not_reclaimed_cases > 0);
  CHECK(rolled_back_cases > 0);
}

TEST(test_paged_ddl_double_faults_report_the_undo_failure) {
  std::unique_ptr<MemoryPageStore> base;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "t", 40, 2000);  // about 20 pages
    // A table of 300 columns: its sys_columns rows span several pages, so a
    // DROP TABLE touches more pages than the pool of 4 frames holds.
    TableSchema wide = BodySchema("wide");
    wide.columns.clear();
    for (int index = 0; index < 300; ++index) {
      wide.columns.push_back(
          strata::testing::Column("c" + std::to_string(index), DataType::kInt));
    }
    CHECK_OK(env.database->CreateTable(wide));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const ColumnDef column{"c", DataType::kInt, Value(std::int64_t{7})};
  int undo_failures = 0;
  int release_failures = 0;
  for (int read_call = 1; read_call < 160; ++read_call) {
    for (int write_call = 1; write_call <= 8; ++write_call) {
      for (int statement = 0; statement < 2; ++statement) {
        auto opened = OpenCopy(*base, 4);
        CHECK(opened.database != nullptr);
        if (opened.database == nullptr) {
          return;
        }
        Database& database = *opened.database;
        opened.store->InjectFailure(Operation::kRead, read_call);
        opened.store->InjectFailure(Operation::kWrite, write_call);
        std::string message;
        if (statement == 0) {
          const auto result = database.AddColumn("t", column);
          message = strata::testing::MessageOf(result);
        } else {
          message = strata::testing::MessageOf(database.DropTable("wide"));
        }
        opened.store->ClearInjectedFailures();
        // Whatever happened, no pin is left and the call returned.
        CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
        undo_failures += message.find("; undoing the rows also failed:") !=
                         std::string::npos;
        release_failures +=
            message.find("; releasing the new pages also failed:") !=
            std::string::npos;
      }
    }
  }
  // The two undo branches were reached at least once.
  CHECK(undo_failures > 0);
  CHECK(release_failures > 0);
}

TEST(test_paged_ddl_cycles_keep_the_file_bounded) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  PageId after_first_cycle = 0;
  for (int cycle = 0; cycle < 40; ++cycle) {
    FillTable(database, "cycle", 100, 1000);
    CHECK_OK(database.AddColumn(
        "cycle", ColumnDef{"extra", DataType::kInt, Value(std::int64_t{0})}));
    CHECK_OK(database.DropTable("cycle"));
    if (cycle == 0) {
      after_first_cycle = database.buffer().PageCount();
    }
  }
  CHECK(database.buffer().PageCount() <= after_first_cycle + 2);
  CHECK(EveryPageIsAccountedFor(database));
}

TEST(test_paged_ddl_refuses_a_corrupt_table) {
  std::unique_ptr<MemoryPageStore> base;
  std::vector<PageId> damaged_chain;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    FillTable(*env.database, "damaged", 12, 2000);
    FillTable(*env.database, "healthy", 6, 2000);
    damaged_chain = ChainOf(env.database->buffer(),
                            *env.database->catalog().FirstPage("damaged"));
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  if (damaged_chain.size() < 3) {
    return;
  }
  // One flipped byte in a middle page, without a new checksum.
  base->MutablePage(damaged_chain[1])[kPageSize - 1] ^= 0xFF;

  auto opened = OpenCopy(*base);
  CHECK(opened.database != nullptr);
  if (opened.database == nullptr) {
    return;
  }
  Database& database = *opened.database;
  const std::vector<PageId> free_before = FreePagesOf(database);
  CHECK_ERROR(database.AddColumn("damaged", ColumnDef{"c", DataType::kInt,
                                                      Value(std::int64_t{0})}),
              kCorrupt);
  CHECK_ERROR(database.DropTable("damaged"), kCorrupt);
  CHECK(database.catalog().FindTable("damaged").has_value());
  CHECK_EQ(database.catalog().FindTable("damaged")->columns.size(),
           std::size_t{2});
  CHECK_EQ(FreePagesOf(database), free_before);

  CHECK_OK(database.AddColumn(
      "healthy", ColumnDef{"c", DataType::kInt, Value(std::int64_t{0})}));
  CHECK_OK(database.DropTable("healthy"));
}

TEST(test_paged_ddl_runs_with_four_frames) {
  MemoryDatabase env = OpenNewMemoryDatabase(4);
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  FillTable(database, "t", 40, 2000);  // about 20 pages
  CHECK_OK(database.AddColumn(
      "t", ColumnDef{"c", DataType::kInt, Value(std::int64_t{3})}));
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "t").size(), std::size_t{40});
  CHECK_OK(database.DropTable("t"));
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});

  const PageId page_count = database.buffer().PageCount();
  FillTable(database, "u", 40, 2000);
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
  CHECK_EQ(RowsOf(database, "u").size(), std::size_t{40});
  CHECK_EQ(database.buffer().PageCount(), page_count);
}

namespace {

// A closed copy of a database with table "t" of `rows` rows whose heap and
// index agree: 300-byte bodies, so about 12 rows to a page.
std::unique_ptr<MemoryPageStore> IndexedBase(std::int64_t rows) {
  MemoryDatabase env = OpenNewMemoryDatabase();
  if (env.database == nullptr) {
    return nullptr;
  }
  strata::testing::FillIndexedTable(*env.database, "t", rows, 300);
  CHECK_OK(env.database->CheckIndex("t"));
  CHECK_OK(env.database->Close());
  return strata::testing::CopyOfStore(*env.store);
}

bool Contains(const std::vector<PageId>& pages, PageId page_id) {
  return std::ranges::find(pages, page_id) != pages.end();
}

}  // namespace

TEST(test_paged_ddl_add_column_rebuilds_the_index) {
  const std::unique_ptr<MemoryPageStore> base = IndexedBase(700);
  if (base == nullptr) {
    return;
  }
  const ColumnDef column{"extra", DataType::kInt, Value(std::int64_t{5})};

  // Without a failure: the new tree finds every row in the new chain.
  {
    auto opened = OpenCopy(*base, 16);
    CHECK(opened.database != nullptr);
    if (opened.database == nullptr) {
      return;
    }
    Database& database = *opened.database;
    const PageId old_root = database.catalog().IndexRoot("t").value_or(0);
    const std::vector<PageId> old_pages = IndexPagesOf(database, "t");
    CHECK(old_pages.size() >= 3);  // the root and at least two leaves
    CHECK_OK(database.AddColumn("t", column));
    CHECK_OK(database.CheckIndex("t"));
    const PageId new_root = database.catalog().IndexRoot("t").value_or(0);
    CHECK(new_root != 0 && new_root != old_root);
    const std::vector<PageId> free_pages = FreePagesOf(database);
    for (PageId page_id : old_pages) {
      CHECK(Contains(free_pages, page_id));
    }
    CHECK(EveryPageIsAccountedFor(database));
    CHECK_OK(database.Close());
    auto reopened = OpenCopy(*opened.store, 16);
    CHECK(reopened.database != nullptr);
    if (reopened.database != nullptr) {
      CHECK_OK(reopened.database->CheckIndex("t"));
      CHECK_EQ(RowsOf(*reopened.database, "t").size(), std::size_t{700});
    }
  }

  // With the n-th read, write and allocation failing, in a pool of 5 frames.
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 400 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 5);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      opened.store->InjectFailure(operation, nth_call);
      const auto altered = database.AddColumn("t", column);
      opened.store->ClearInjectedFailures();
      if (altered.ok()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(altered, kIo);
        ++failures_seen;
      }
      // The table is the old one with its old index, or the new one with its
      // new index.
      const auto schema = database.catalog().FindTable("t");
      CHECK(schema.has_value());
      if (schema.has_value()) {
        const std::size_t column_count = schema->columns.size();
        CHECK(column_count == 2 || column_count == 3);
        if (altered.ok()) {
          CHECK_EQ(column_count, std::size_t{3});
        }
      }
      CHECK_OK(database.CheckIndex("t"));
      CHECK_EQ(RowsOf(database, "t").size(), std::size_t{700});
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
      // The one injected failure has fired, so the release of the pages of a
      // failed attempt works, unless it is the step that failed: then the
      // message says that releasing also failed or that pages were not
      // reclaimed. Otherwise no page is left without an owner.
      const bool is_release_failure =
          !altered.ok() &&
          (altered.error().message.find("also failed") != std::string::npos ||
           altered.error().message.find("not reclaimed") != std::string::npos);
      if (!is_release_failure) {
        CHECK(EveryPageIsAccountedFor(database));
      }
      CHECK_OK(database.Close());
      auto reopened = OpenCopy(*opened.store, 16);
      CHECK(reopened.database != nullptr);
      if (reopened.database != nullptr) {
        CHECK_OK(reopened.database->CheckIndex("t"));
      }
    }
    CHECK(has_succeeded);
    CHECK(failures_seen > 0);
  }
}

// A table whose primary key is TEXT has no index: ADD COLUMN builds only a new
// chain, and DROP TABLE frees only the chain.
TEST(test_paged_ddl_a_table_without_an_index_adds_a_column_and_drops) {
  MemoryDatabase memory = OpenNewMemoryDatabase(16);
  if (memory.database == nullptr) {
    return;
  }
  Database& database = *memory.database;
  TableSchema schema;
  schema.name = "words";
  schema.columns = {ColumnDef{"word", DataType::kText},
                    ColumnDef{"count", DataType::kInt}};
  schema.primary_key_index = 0;
  CHECK_OK(database.CreateTable(schema));
  CHECK(!database.catalog().IndexRoot("words").has_value());
  auto heap = database.Table("words");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  constexpr std::int64_t kRowCount = 600;
  for (std::int64_t number = 0; number < kRowCount; ++number) {
    CHECK_OK(heap.value()->Insert(
        Row{Value("word" + std::to_string(number) + std::string(40, 'x')),
            Value(number)}));
  }
  CHECK(strata::testing::ChainOf(database.buffer(),
                                 database.catalog().FirstPage("words").value())
            .size() > 3);

  CHECK_OK(database.AddColumn(
      "words", ColumnDef{"extra", DataType::kInt, Value(std::int64_t{5})}));
  CHECK(!database.catalog().IndexRoot("words").has_value());
  const std::vector<Row> rows = RowsOf(database, "words");
  CHECK_EQ(rows.size(), static_cast<std::size_t>(kRowCount));
  for (const Row& row : rows) {
    CHECK_EQ(row.size(), std::size_t{3});
    if (row.size() == 3) {
      CHECK(row[2] == Value(std::int64_t{5}));
    }
  }
  CHECK_ERROR(database.CheckIndex("words"), kInvalidArgument);
  CHECK(EveryPageIsAccountedFor(database));

  CHECK_OK(database.DropTable("words"));
  CHECK(database.catalog().TableNames().empty());
  CHECK(EveryPageIsAccountedFor(database));
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
}

// An index entry whose row is gone: every row still has its entry, so only the
// count of entries against the count of rows shows the damage.
TEST(test_paged_ddl_check_index_counts_entries_that_have_no_row) {
  MemoryDatabase memory = OpenNewMemoryDatabase(16);
  if (memory.database == nullptr) {
    return;
  }
  Database& database = *memory.database;
  strata::testing::FillIndexedTable(database, "t", 50, 20);
  CHECK_OK(database.CheckIndex("t"));
  auto heap = database.Table("t");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  strata::paged::HeapScan scan = heap.value()->Scan();
  auto first = scan.Next();
  CHECK_OK(first);
  if (!first.ok() || !first.value().has_value()) {
    CHECK(false);
    return;
  }
  CHECK_OK(heap.value()->Delete(first.value()->rid));

  const auto status = database.CheckIndex("t");

  CHECK_ERROR(status, kCorrupt);
  CHECK_MESSAGE_CONTAINS(status, "has 50 entries for 49 rows");
  CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
}

TEST(test_paged_ddl_drop_table_frees_the_index_pages) {
  const std::unique_ptr<MemoryPageStore> base = IndexedBase(700);
  if (base == nullptr) {
    return;
  }
  // Without a failure: the free list holds the pages of the tree and then the
  // chain, and the table created again takes its first two pages from them.
  {
    auto opened = OpenCopy(*base, 16);
    CHECK(opened.database != nullptr);
    if (opened.database == nullptr) {
      return;
    }
    Database& database = *opened.database;
    std::vector<PageId> expected = IndexPagesOf(database, "t");
    CHECK(expected.size() >= 3);
    const std::vector<PageId> chain =
        ChainOf(database.buffer(), *database.catalog().FirstPage("t"));
    expected.insert(expected.end(), chain.begin(), chain.end());
    const PageId page_count = database.buffer().PageCount();
    CHECK_OK(database.DropTable("t"));
    CHECK_EQ(FreePagesOf(database), expected);
    CHECK(EveryPageIsAccountedFor(database));
    CHECK_OK(database.CreateTable(BodySchema("t")));
    CHECK_EQ(database.catalog().FirstPage("t").value_or(0), expected[0]);
    CHECK_EQ(database.catalog().IndexRoot("t").value_or(0), expected[1]);
    CHECK_EQ(database.buffer().PageCount(), page_count);
  }

  // With the n-th read, write and allocation failing, in a pool of 5 frames.
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  for (const Operation operation : operations) {
    bool has_succeeded = false;
    int failures_seen = 0;
    for (int nth_call = 1; nth_call < 400 && !has_succeeded; ++nth_call) {
      auto opened = OpenCopy(*base, 5);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      opened.store->InjectFailure(operation, nth_call);
      const auto status = database.DropTable("t");
      opened.store->ClearInjectedFailures();
      if (!status.has_value()) {
        has_succeeded = true;
      } else {
        CHECK_ERROR(status, kIo);
        ++failures_seen;
      }
      // The table is gone, or intact with a valid index.
      if (database.catalog().FindTable("t").has_value()) {
        CHECK_OK(database.CheckIndex("t"));
        CHECK_EQ(RowsOf(database, "t").size(), std::size_t{700});
      } else {
        CHECK_ERROR(database.Table("t"), kNotFound);
      }
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
    }
    CHECK(has_succeeded);
    // A drop allocates no page, so an injected allocation failure never fires.
    if (operation == Operation::kAllocate) {
      CHECK_EQ(failures_seen, 0);
    } else {
      CHECK(failures_seen > 0);
    }
  }
}

namespace {

// One thing rewritten in a closed database of table t (300 rows: an internal
// root over two leaves) and table other, each page written with a valid
// checksum.
enum class Damage {
  kRootIsHeapPage,
  kRootOfAnotherTable,
  kLeafKeyCountTooLarge,
  kChildOfAnotherTable,
  kChildPastTheEnd,
  kRidNamesDeadSlot,
  kKeyDiffersFromRow,
  kLeafLinkReturnsToItself
};

struct DamageCase {
  Damage damage;
  const char* name;
  // Database::Index fails, because the root does not open.
  bool is_root_damaged;
  // DropTable and AddColumn fail with kCorrupt before a page changes.
  bool is_statement_refused;
  // Contains(290) returns an error.
  bool does_contains_fail;
  // ScanRange from key 290 (the whole index for the link) ends with an error.
  bool does_scan_fail;
};

// The key that the damage concerns: in the second leaf, the one that holds the
// keys 146 to 300 after ascending inserts.
constexpr std::int64_t kDamagedKey = 290;

void ApplyDamage(MemoryPageStore& store, PageId root, PageId other_root,
                 Damage damage) {
  const strata::paged::NodeView root_node(store.PageBytes(root));
  const PageId first_leaf = root_node.ChildAt(0);
  const PageId second_leaf = root_node.ChildAt(1);
  std::uint8_t* const root_page = store.MutablePage(root);
  std::uint8_t* const leaf_page = store.MutablePage(second_leaf);
  switch (damage) {
    case Damage::kRootIsHeapPage:
      root_page[strata::paged::layout::kPageTypeField] = 2;
      break;
    case Damage::kRootOfAnotherTable:
      strata::paged::WriteLe32(root_page + strata::paged::layout::kTableId, 99);
      break;
    case Damage::kLeafKeyCountTooLarge:
      strata::paged::WriteLe16(
          leaf_page + strata::paged::node_layout::kKeyCount, 290);
      break;
    case Damage::kChildOfAnotherTable:
      strata::paged::WriteLe32(
          root_page + strata::paged::node_layout::kInternalChildren + 4,
          other_root);
      break;
    case Damage::kChildPastTheEnd:
      strata::paged::WriteLe32(
          root_page + strata::paged::node_layout::kInternalChildren + 4,
          store.PageCount() + 5);
      break;
    case Damage::kRidNamesDeadSlot: {
      const strata::paged::LeafPosition position =
          strata::paged::LeafSearch(leaf_page, kDamagedKey);
      strata::paged::LeafSetRid(leaf_page, position.index,
                                strata::paged::Rid{3, 60000});
      break;
    }
    case Damage::kKeyDiffersFromRow: {
      // The last key of the leaf becomes a key that no row has, and stays above
      // the others.
      const strata::paged::NodeView leaf(leaf_page);
      const std::size_t last = leaf.key_count() - 1;
      strata::paged::WriteLe64(
          leaf_page + strata::paged::node_layout::kKeys + 8 * last, 99999);
      break;
    }
    case Damage::kLeafLinkReturnsToItself:
      strata::paged::WriteLe32(
          store.MutablePage(first_leaf) + strata::paged::layout::kNextPageId,
          first_leaf);
      break;
  }
  strata::paged::SlottedPage::SealChecksum(root_page);
  strata::paged::SlottedPage::SealChecksum(leaf_page);
  strata::paged::SlottedPage::SealChecksum(store.MutablePage(first_leaf));
}

// Every page of the store, to see that a refused statement changed none.
std::vector<std::vector<std::uint8_t>> PageImagesOf(
    const MemoryPageStore& store) {
  std::vector<std::vector<std::uint8_t>> images;
  for (PageId page_id = 0; page_id < store.PageCount(); ++page_id) {
    images.emplace_back(store.PageBytes(page_id),
                        store.PageBytes(page_id) + kPageSize);
  }
  return images;
}

}  // namespace

TEST(test_paged_database_reports_a_damaged_index) {
  // A closed database: table t with 300 indexed rows, and a small table other.
  std::unique_ptr<MemoryPageStore> base;
  PageId root = 0;
  PageId other_root = 0;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    strata::testing::FillIndexedTable(*env.database, "t", 300, 100);
    strata::testing::FillIndexedTable(*env.database, "other", 5, 100);
    root = env.database->catalog().IndexRoot("t").value_or(0);
    other_root = env.database->catalog().IndexRoot("other").value_or(0);
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  const strata::paged::NodeView root_node(base->PageBytes(root));
  CHECK_EQ(root_node.level(), std::uint16_t{1});
  CHECK_EQ(root_node.key_count(), std::uint16_t{1});

  const DamageCase cases[] = {
      {Damage::kRootIsHeapPage, "root is a heap page", true, true, true, true},
      {Damage::kRootOfAnotherTable, "root of another table", true, true, true,
       true},
      {Damage::kLeafKeyCountTooLarge, "leaf key_count 290", false, true, true,
       true},
      {Damage::kChildOfAnotherTable, "child of another table", false, true,
       true, true},
      {Damage::kChildPastTheEnd, "child past the end of the file", false, true,
       true, true},
      {Damage::kRidNamesDeadSlot, "Rid names a dead slot", false, false, false,
       true},
      {Damage::kKeyDiffersFromRow, "key differs from its row", false, false,
       false, true},
      {Damage::kLeafLinkReturnsToItself, "leaf link returns to itself", false,
       false, false, true}};
  for (const DamageCase& test_case : cases) {
    std::unique_ptr<MemoryPageStore> damaged =
        strata::testing::CopyOfStore(*base);
    ApplyDamage(*damaged, root, other_root, test_case.damage);

    // The read paths: Index, Contains, ScanRange and CheckIndex.
    {
      auto opened = OpenCopy(*damaged, 16);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const auto index = database.Index("t");
      if (test_case.is_root_damaged) {
        CHECK_ERROR(index, kCorrupt);
      } else {
        CHECK_OK(index);
      }
      const strata::paged::DatabaseIndexReader reader(database);
      const strata::paged::ContainsResult contains =
          reader.Contains("t", kDamagedKey);
      CHECK_EQ(std::holds_alternative<strata::domain::ExecutionError>(contains),
               test_case.does_contains_fail);
      if (const auto* error =
              std::get_if<strata::domain::ExecutionError>(&contains)) {
        CHECK(error->message.starts_with("corrupt: ") ||
              error->message.find("corrupt") != std::string::npos);
      }
      strata::paged::KeyRange range;
      if (test_case.damage != Damage::kLeafLinkReturnsToItself) {
        range.low = kDamagedKey;
      }
      std::optional<std::string> scan_error;
      strata::paged::ScanResult scanned = reader.ScanRange("t", range);
      if (auto* error = std::get_if<strata::domain::ExecutionError>(&scanned)) {
        scan_error = error->message;
      } else {
        auto& stream =
            std::get<std::unique_ptr<strata::paged::IRowStream>>(scanned);
        while (!scan_error.has_value()) {
          strata::paged::StreamResult next = stream->Next();
          if (auto* failure =
                  std::get_if<strata::domain::ExecutionError>(&next)) {
            scan_error = failure->message;
          } else if (!std::get<std::optional<strata::paged::StoredRow>>(next)
                          .has_value()) {
            break;
          }
        }
      }
      CHECK_EQ(scan_error.has_value(), test_case.does_scan_fail);
      if (scan_error.has_value()) {
        CHECK(scan_error->find("corrupt") != std::string::npos);
        // The two entries that the stream checks against their row have the
        // messages of the design.
        if (test_case.damage == Damage::kRidNamesDeadSlot) {
          CHECK(scan_error->starts_with("corrupt: index of t names no row "));
        } else if (test_case.damage == Damage::kKeyDiffersFromRow) {
          CHECK(scan_error->starts_with(
              "corrupt: index of t disagrees with its row "));
        }
      }
      const auto check = database.CheckIndex("t");
      CHECK_ERROR(check, kCorrupt);
      // Another table still reads, and nothing stays pinned.
      CHECK_EQ(RowsOf(database, "other").size(), std::size_t{5});
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
    }

    // DropTable and AddColumn: refused before any page changes, or applied.
    const ColumnDef column{"extra", DataType::kInt, Value(std::int64_t{1})};
    for (bool is_drop : {true, false}) {
      auto opened = OpenCopy(*damaged, 16);
      CHECK(opened.database != nullptr);
      if (opened.database == nullptr) {
        return;
      }
      Database& database = *opened.database;
      const auto before = PageImagesOf(*opened.store);
      strata::paged::Status status;
      if (is_drop) {
        status = database.DropTable("t");
      } else {
        const auto altered = database.AddColumn("t", column);
        if (!altered.ok()) {
          status = altered.error();
        }
      }
      if (test_case.is_statement_refused) {
        CHECK_ERROR(status, kCorrupt);
        CHECK_OK(database.buffer().FlushAll());
        CHECK(PageImagesOf(*opened.store) == before);
      } else {
        CHECK_OK(status);
      }
      CHECK_EQ(database.buffer().PinnedFrameCount(), std::size_t{0});
    }
  }
}

namespace {

// An engine over a copy of `base` with the damage applied, or null.
std::unique_ptr<strata::paged::PagedEngine> EngineOverDamagedCopy(
    const MemoryPageStore& base, PageId root, PageId other_root,
    std::optional<Damage> damage) {
  std::unique_ptr<MemoryPageStore> copy = strata::testing::CopyOfStore(base);
  if (damage.has_value()) {
    ApplyDamage(*copy, root, other_root, *damage);
  }
  auto opened = Database::OpenWithStore(std::move(copy),
                                        strata::paged::DatabaseOptions{});
  CHECK_OK(opened);
  if (!opened.ok()) {
    return nullptr;
  }
  return std::make_unique<strata::paged::PagedEngine>(std::move(opened).value(),
                                                      nullptr);
}

}  // namespace

// PagedEngine::DescribeIndex on the database of the test above: a sound tree, a
// damaged node below the root, a damaged root and an unknown table.
TEST(test_paged_engine_describe_index_reports_a_damaged_tree) {
  std::unique_ptr<MemoryPageStore> base;
  PageId root = 0;
  PageId other_root = 0;
  {
    MemoryDatabase env = OpenNewMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    strata::testing::FillIndexedTable(*env.database, "t", 300, 100);
    strata::testing::FillIndexedTable(*env.database, "other", 5, 100);
    root = env.database->catalog().IndexRoot("t").value_or(0);
    other_root = env.database->catalog().IndexRoot("other").value_or(0);
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  struct Case {
    std::optional<Damage> damage;
    std::size_t node_count;  // the nodes that the walk read
    bool is_check_ok;
  };
  const Case cases[] = {{std::nullopt, 3, true},
                        {Damage::kLeafKeyCountTooLarge, 2, false},
                        {Damage::kChildPastTheEnd, 2, false}};
  for (const Case& test_case : cases) {
    auto engine =
        EngineOverDamagedCopy(*base, root, other_root, test_case.damage);
    if (engine == nullptr) {
      return;
    }
    const strata::domain::IndexDescriptionResult result =
        engine->DescribeIndex("T");
    const auto* description =
        std::get_if<strata::domain::IndexDescription>(&result);
    CHECK(description != nullptr);
    if (description != nullptr) {
      CHECK_EQ(description->table, std::string("t"));
      CHECK_EQ(description->column, std::string("id"));
      CHECK_EQ(description->node_count, test_case.node_count);
      CHECK_EQ(description->nodes.size(), test_case.node_count);
      CHECK_EQ(description->check == "ok", test_case.is_check_ok);
      if (test_case.is_check_ok) {
        CHECK_EQ(description->height, std::size_t{2});
        CHECK_EQ(description->key_count, std::size_t{300});
      }
    }
    CHECK_EQ(engine->database().buffer().PinnedFrameCount(), std::size_t{0});
  }

  auto engine =
      EngineOverDamagedCopy(*base, root, other_root, Damage::kRootIsHeapPage);
  if (engine == nullptr) {
    return;
  }
  const strata::domain::IndexDescriptionResult damaged_root =
      engine->DescribeIndex("t");
  const auto* root_error =
      std::get_if<strata::domain::ExecutionError>(&damaged_root);
  CHECK(root_error != nullptr);
  if (root_error != nullptr) {
    CHECK(root_error->kind == strata::domain::ErrorKind::kIo);
    CHECK(root_error->message.starts_with("corrupt: "));
  }
  const strata::domain::IndexDescriptionResult unknown =
      engine->DescribeIndex("nosuch");
  const auto* unknown_error =
      std::get_if<strata::domain::ExecutionError>(&unknown);
  CHECK(unknown_error != nullptr);
  if (unknown_error != nullptr) {
    CHECK(unknown_error->kind == strata::domain::ErrorKind::kBind);
    CHECK_EQ(unknown_error->message, std::string("unknown table: nosuch"));
  }
  CHECK_EQ(engine->database().buffer().PinnedFrameCount(), std::size_t{0});
}
