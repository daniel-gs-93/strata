#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/catalog.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/paged/table_heap.h"
#include "test_harness.h"
#include "test_paged_ddl_support.h"
#include "test_paged_support.h"

// Catalog over a Database on a MemoryPageStore.

namespace {

using strata::domain::ColumnDef;
using strata::domain::DataType;
using strata::domain::ForeignKey;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::Catalog;
using strata::paged::Database;
using strata::paged::DatabaseOptions;
using strata::paged::LatchMode;
using strata::paged::MemoryPageStore;
using strata::paged::PageGuard;
using strata::paged::PageId;
using strata::paged::Rid;
using strata::paged::TableHeap;
using Operation = strata::paged::MemoryPageStore::Operation;

struct OpenedDatabase {
  MemoryPageStore* store = nullptr;  // owned by database
  std::unique_ptr<Database> database;
};

OpenedDatabase OpenMemoryDatabase(std::size_t frames = 16) {
  auto store = std::make_unique<MemoryPageStore>();
  MemoryPageStore* const observer = store.get();
  DatabaseOptions options;
  options.pool_frames = frames;
  auto opened = Database::OpenWithStore(std::move(store), options);
  CHECK_OK(opened);
  OpenedDatabase result;
  if (opened.ok()) {
    result.store = observer;
    result.database = std::make_unique<Database>(std::move(opened).value());
  }
  return result;
}

TableSchema BasicSchema(const std::string& name) {
  TableSchema schema;
  schema.name = name;
  schema.columns = {{"id", DataType::kInt}, {"name", DataType::kText}};
  schema.primary_key_index = 0;
  return schema;
}

std::uint64_t BitsOf(double number) {
  return std::bit_cast<std::uint64_t>(number);
}

bool DefaultsEqual(const std::optional<Value>& left,
                   const std::optional<Value>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  if (left->index() != right->index()) {
    return false;
  }
  if (std::holds_alternative<double>(*left)) {
    return BitsOf(std::get<double>(*left)) == BitsOf(std::get<double>(*right));
  }
  return *left == *right;
}

bool ForeignKeysEqual(const std::vector<ForeignKey>& left,
                      const std::vector<ForeignKey>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].column_index != right[i].column_index ||
        left[i].ref_table != right[i].ref_table ||
        left[i].ref_column_index != right[i].ref_column_index) {
      return false;
    }
  }
  return true;
}

bool SchemasEqual(const TableSchema& left, const TableSchema& right) {
  if (left.name != right.name || left.columns.size() != right.columns.size() ||
      left.primary_key_index != right.primary_key_index ||
      left.unique_indexes != right.unique_indexes ||
      left.auto_increment_index != right.auto_increment_index ||
      !ForeignKeysEqual(left.foreign_keys, right.foreign_keys)) {
    return false;
  }
  for (std::size_t i = 0; i < left.columns.size(); ++i) {
    if (left.columns[i].name != right.columns[i].name ||
        left.columns[i].type != right.columns[i].type ||
        !DefaultsEqual(left.columns[i].default_value,
                       right.columns[i].default_value)) {
      return false;
    }
  }
  return true;
}

// The number of rows in one of the system tables (1 = sys_tables, 2 =
// sys_columns, whose first pages are 1 and 2 in a new database).
std::size_t SystemRowCount(Database& database, std::uint32_t table_id) {
  auto heap = TableHeap::Open(
      database.buffer(), database.allocator(),
      table_id == 1 ? Catalog::SysTablesSchema() : Catalog::SysColumnsSchema(),
      table_id, table_id);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return 0;
  }
  std::size_t count = 0;
  auto scan = heap.value().Scan();
  while (true) {
    auto next = scan.Next();
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    ++count;
  }
  return count;
}

}  // namespace

TEST(test_paged_catalog_round_trips_schemas) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("b")));

  TableSchema schema;
  schema.name = "T";
  schema.columns = {
      ColumnDef{"id", DataType::kInt, std::nullopt},
      ColumnDef{"email", DataType::kText, std::nullopt},
      ColumnDef{"owner_id", DataType::kInt,
                Value(std::numeric_limits<std::int64_t>::min())},
      ColumnDef{"score", DataType::kDouble, Value(0.1)},
      ColumnDef{"offset", DataType::kDouble, Value(-0.0)},
      ColumnDef{"note", DataType::kText, Value(std::string("it's\nok"))},
      ColumnDef{"tag", DataType::kText, Value(std::string())}};
  schema.primary_key_index = 0;
  schema.unique_indexes = {1};
  schema.foreign_keys = {ForeignKey{2, "b", 0}};
  schema.auto_increment_index = 0;
  auto created = database.CreateTable(schema);
  CHECK_OK(created);

  Catalog fresh(database.buffer(), database.allocator(), 1, 2);
  CHECK_OK(fresh.Load());
  auto found = fresh.FindTable("t");  // names are case-insensitive
  CHECK(found.has_value());
  if (found.has_value()) {
    CHECK(SchemasEqual(*found, schema));
  }
  CHECK(fresh.FindTable("missing") == std::nullopt);
  CHECK(fresh.FindTable("sys_tables") == std::nullopt);
  const std::vector<std::string> expected_names = {"b", "T"};
  CHECK(fresh.TableNames() == expected_names);
  CHECK(fresh.TableId("T").has_value());
  CHECK(fresh.FirstPage("T").has_value());
}

TEST(test_paged_catalog_create_table_validation) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("visitors")));

  struct Case {
    TableSchema schema;
    const char* expected_error;
  };
  std::vector<Case> cases;
  cases.push_back({BasicSchema("visitors"), "kAlreadyExists"});
  cases.push_back({BasicSchema("VISITORS"), "kAlreadyExists"});
  cases.push_back({BasicSchema("sys_tables"), "kInvalidArgument"});
  cases.push_back({BasicSchema("SYS_x"), "kInvalidArgument"});
  cases.push_back({BasicSchema(""), "kInvalidArgument"});
  cases.push_back({BasicSchema("1a"), "kInvalidArgument"});
  cases.push_back({BasicSchema("a b"), "kInvalidArgument"});
  TableSchema no_columns = BasicSchema("t1");
  no_columns.columns.clear();
  cases.push_back({no_columns, "kInvalidArgument"});
  TableSchema duplicate_column = BasicSchema("t2");
  duplicate_column.columns = {{"a", DataType::kInt}, {"A", DataType::kInt}};
  cases.push_back({duplicate_column, "kInvalidArgument"});
  TableSchema bad_primary_key = BasicSchema("t3");
  bad_primary_key.primary_key_index = 5;
  cases.push_back({bad_primary_key, "kInvalidArgument"});
  TableSchema auto_on_text = BasicSchema("t4");
  auto_on_text.auto_increment_index = 1;
  cases.push_back({auto_on_text, "kInvalidArgument"});
  TableSchema bad_unique = BasicSchema("t5");
  bad_unique.unique_indexes = {5};
  cases.push_back({bad_unique, "kInvalidArgument"});
  TableSchema unsorted_unique = BasicSchema("t6");
  unsorted_unique.unique_indexes = {1, 0};
  cases.push_back({unsorted_unique, "kInvalidArgument"});
  TableSchema bad_foreign_key = BasicSchema("t7");
  bad_foreign_key.foreign_keys = {ForeignKey{5, "visitors", 0}};
  cases.push_back({bad_foreign_key, "kInvalidArgument"});

  const PageId pages_before = env.store->PageCount();
  const std::size_t tables_before = SystemRowCount(database, 1);
  const std::size_t columns_before = SystemRowCount(database, 2);
  for (const Case& item : cases) {
    auto result = database.CreateTable(item.schema);
    CHECK_EQ(strata::testing::ErrorName(result),
             std::string(item.expected_error));
    CHECK_EQ(env.store->PageCount(), pages_before);
    CHECK_EQ(SystemRowCount(database, 1), tables_before);
    CHECK_EQ(SystemRowCount(database, 2), columns_before);
  }
  const std::vector<std::string> names = {"visitors"};
  CHECK(database.catalog().TableNames() == names);
}

TEST(test_paged_catalog_refuses_a_catalog_row_longer_than_a_tuple) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  TableSchema long_default = BasicSchema("t1");
  long_default.columns[1].default_value = Value(std::string(4100, 'd'));
  const TableSchema long_name = BasicSchema(std::string(4100, 'n'));
  const PageId pages_before = env.store->PageCount();

  auto by_default = database.CreateTable(long_default);
  auto by_name = database.CreateTable(long_name);

  CHECK_ERROR(by_default, kInvalidArgument);
  CHECK_MESSAGE_CONTAINS(by_default, "column name of table t1");
  CHECK_MESSAGE_CONTAINS(by_default, "the limit is 4060");
  CHECK_ERROR(by_name, kInvalidArgument);
  CHECK_MESSAGE_CONTAINS(by_name, "the limit is 4060");
  // Refused before the first page of the table is allocated.
  CHECK_EQ(env.store->PageCount(), pages_before);
  CHECK(database.catalog().TableNames().empty());
}

TEST(test_paged_catalog_create_table_undoes_partial_rows) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  TableSchema first;
  first.name = "wide_a";
  TableSchema second;
  second.name = "wide_b";
  for (int i = 0; i < 100; ++i) {
    first.columns.push_back({"c" + std::to_string(1000 + i), DataType::kInt});
    second.columns.push_back({"c" + std::to_string(1000 + i), DataType::kInt});
  }
  first.primary_key_index = 0;
  second.primary_key_index = 0;
  CHECK_OK(database.CreateTable(first));

  const std::size_t tables_before = SystemRowCount(database, 1);
  const std::size_t columns_before = SystemRowCount(database, 2);
  // The first allocation is wide_b's first page, the second the root of its
  // index, and the third a new page of sys_columns, which the 100 column rows
  // of wide_b need. The sys_tables row of wide_b exists by then, so the failure
  // has a row to undo.
  env.store->InjectFailure(Operation::kAllocate, 3);
  CHECK_ERROR(database.CreateTable(second), kIo);
  CHECK_EQ(SystemRowCount(database, 1), tables_before);
  CHECK_EQ(SystemRowCount(database, 2), columns_before);
  Catalog fresh(database.buffer(), database.allocator(), 1, 2);
  CHECK_OK(fresh.Load());
  CHECK(fresh.FindTable("wide_b") == std::nullopt);
  CHECK(fresh.FindTable("wide_a").has_value());
  env.store->ClearInjectedFailures();
  CHECK_OK(database.CreateTable(second));
}

namespace {

enum class SystemDamage {
  kNone,
  kDuplicateTableId,
  kDuplicateName,
  kFirstPageOutsideFile,
  kMissingPosition,
  kBadColumnType,
  kBadDefaultKind,
  kBadPrimaryKeyIndex,
  kBadAutoIncrementIndex,
  kBadRefColumn,
  kNextTableIdTooLow,
  kOrphanColumnRow,
  kBadDefaultText,
  kMissingRefTable,
  kSelfReference,
  kRefColumnOutOfRange,
  kRefTypeMismatch,
  kRefNotUnique
};

// The row of a system table whose column `column` equals `value` and, when
// position is given, whose column 1 equals position (sys_columns).
std::optional<strata::paged::ScanItem> FindSystemRow(
    TableHeap& heap, std::size_t column, std::int64_t value,
    std::optional<std::int64_t> position = std::nullopt) {
  auto scan = heap.Scan();
  while (true) {
    auto next = scan.Next();
    if (!next.ok() || !next.value().has_value()) {
      return std::nullopt;
    }
    const Row& row = next.value()->row;
    if (std::get<std::int64_t>(row[column]) != value) {
      continue;
    }
    if (position.has_value() && std::get<std::int64_t>(row[1]) != *position) {
      continue;
    }
    return std::move(*next.value());
  }
}

void SetIntColumn(TableHeap& heap, const strata::paged::ScanItem& item,
                  std::size_t column, std::int64_t value) {
  Row row = item.row;
  row[column] = Value(value);
  CHECK_OK(heap.Update(item.rid, row));
}

// Rewrites the foreign key of T (sys_columns, column position 2).
void SetForeignKeyOfT(TableHeap& sys_columns, const std::string& ref_table,
                      std::int64_t ref_column) {
  const auto target = FindSystemRow(sys_columns, 0, 3, 2);
  CHECK(target.has_value());
  if (!target.has_value()) {
    return;
  }
  Row row = target->row;
  row[7] = Value(ref_table);
  row[8] = Value(ref_column);
  CHECK_OK(sys_columns.Update(target->rid, row));
}

// Outcome of Catalog::Load after the system tables or the file header were
// damaged: "ok" or the error name and message.
std::string LoadAfterDamage(SystemDamage damage) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return "no database";
  }
  Database& database = *env.database;
  TableSchema schema = BasicSchema("T");
  schema.columns.push_back({"ref", DataType::kInt});
  schema.foreign_keys = {ForeignKey{2, "b", 0}};
  CHECK_OK(database.CreateTable(schema));  // table_id 3
  // The target of the foreign key: id INT primary key, name TEXT, code INT,
  // label TEXT UNIQUE.
  TableSchema target = BasicSchema("b");
  target.columns.push_back({"code", DataType::kInt});
  target.columns.push_back({"label", DataType::kText});
  target.unique_indexes = {3};
  CHECK_OK(database.CreateTable(target));  // table_id 4

  auto tables = TableHeap::Open(database.buffer(), database.allocator(),
                                Catalog::SysTablesSchema(), 1, 1);
  auto columns = TableHeap::Open(database.buffer(), database.allocator(),
                                 Catalog::SysColumnsSchema(), 2, 2);
  CHECK_OK(tables);
  CHECK_OK(columns);
  if (!tables.ok() || !columns.ok()) {
    return "no system tables";
  }
  TableHeap& sys_tables = tables.value();
  TableHeap& sys_columns = columns.value();
  const auto table_row = FindSystemRow(sys_tables, 0, 3);
  CHECK(table_row.has_value());
  if (!table_row.has_value()) {
    return "no row";
  }

  switch (damage) {
    case SystemDamage::kNone:
      break;
    case SystemDamage::kDuplicateTableId:
      CHECK_OK(sys_tables.Insert(
          Row{Value(std::int64_t{3}), Value(std::string("other")),
              table_row->row[2], Value(std::int64_t{0}),
              Value(std::int64_t{-1}), table_row->row[5]}));
      break;
    case SystemDamage::kDuplicateName:
      CHECK_OK(sys_tables.Insert(Row{
          Value(std::int64_t{5}), Value(std::string("t")), table_row->row[2],
          Value(std::int64_t{0}), Value(std::int64_t{-1}), table_row->row[5]}));
      break;
    case SystemDamage::kFirstPageOutsideFile:
      SetIntColumn(sys_tables, *table_row, 2,
                   static_cast<std::int64_t>(env.store->PageCount()));
      break;
    case SystemDamage::kMissingPosition: {
      const auto doomed = FindSystemRow(sys_columns, 0, 3, 1);
      CHECK(doomed.has_value());
      if (doomed.has_value()) {
        CHECK_OK(sys_columns.Delete(doomed->rid));
      }
      break;
    }
    case SystemDamage::kBadColumnType: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 3, 9);
      }
      break;
    }
    case SystemDamage::kBadDefaultKind: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 4, 9);
      }
      break;
    }
    case SystemDamage::kBadPrimaryKeyIndex:
      SetIntColumn(sys_tables, *table_row, 3, 3);
      break;
    case SystemDamage::kBadAutoIncrementIndex:
      SetIntColumn(sys_tables, *table_row, 4, 7);
      break;
    case SystemDamage::kBadRefColumn: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 2);
      CHECK(target.has_value());
      if (target.has_value()) {
        SetIntColumn(sys_columns, *target, 8, -1);
      }
      break;
    }
    case SystemDamage::kOrphanColumnRow:
      CHECK_OK(sys_columns.Insert(Row{
          Value(std::int64_t{99}), Value(std::int64_t{0}),
          Value(std::string("x")), Value(std::int64_t{0}),
          Value(std::int64_t{0}), Value(std::string()), Value(std::int64_t{0}),
          Value(std::string()), Value(std::int64_t{-1})}));
      break;
    case SystemDamage::kBadDefaultText: {
      const auto target = FindSystemRow(sys_columns, 0, 3, 0);
      CHECK(target.has_value());
      if (target.has_value()) {
        Row row = target->row;
        row[4] = Value(std::int64_t{1});     // default_kind: INT
        row[5] = Value(std::string("abc"));  // default_text: not a number
        CHECK_OK(sys_columns.Update(target->rid, row));
      }
      break;
    }
    case SystemDamage::kMissingRefTable:
      SetForeignKeyOfT(sys_columns, "nowhere", 0);
      break;
    case SystemDamage::kSelfReference:
      SetForeignKeyOfT(sys_columns, "t", 0);
      break;
    case SystemDamage::kRefColumnOutOfRange:
      SetForeignKeyOfT(sys_columns, "b", 9);
      break;
    case SystemDamage::kRefTypeMismatch:
      SetForeignKeyOfT(sys_columns, "b", 3);  // label: TEXT, UNIQUE
      break;
    case SystemDamage::kRefNotUnique:
      SetForeignKeyOfT(sys_columns, "b", 2);  // code: INT, not unique
      break;
    case SystemDamage::kNextTableIdTooLow: {
      auto header = database.buffer().FetchPage(0, LatchMode::kExclusive);
      CHECK_OK(header);
      if (header.ok()) {
        PageGuard guard = std::move(header).value();
        strata::paged::WriteLe32(guard.mutable_data() + 48, 3);
        guard.MarkDirty();
      }
      break;
    }
  }
  Catalog fresh(database.buffer(), database.allocator(), 1, 2);
  const strata::paged::Status loaded = fresh.Load();
  return loaded.has_value()
             ? std::string(strata::testing::ErrorCodeName(loaded->code)) +
                   ": " + loaded->message
             : "ok";
}

}  // namespace

TEST(test_paged_catalog_load_rejects_inconsistent_system_tables) {
  CHECK_EQ(LoadAfterDamage(SystemDamage::kNone), "ok");
  const SystemDamage damages[] = {
      SystemDamage::kDuplicateTableId,     SystemDamage::kDuplicateName,
      SystemDamage::kFirstPageOutsideFile, SystemDamage::kMissingPosition,
      SystemDamage::kBadColumnType,        SystemDamage::kBadDefaultKind,
      SystemDamage::kBadPrimaryKeyIndex,   SystemDamage::kBadAutoIncrementIndex,
      SystemDamage::kBadRefColumn,         SystemDamage::kNextTableIdTooLow,
      SystemDamage::kOrphanColumnRow,      SystemDamage::kBadDefaultText,
      SystemDamage::kMissingRefTable,      SystemDamage::kSelfReference,
      SystemDamage::kRefColumnOutOfRange,  SystemDamage::kRefTypeMismatch,
      SystemDamage::kRefNotUnique};
  for (SystemDamage damage : damages) {
    const std::string outcome = LoadAfterDamage(damage);
    CHECK(outcome.starts_with("kCorrupt"));
    if (!outcome.starts_with("kCorrupt")) {
      CHECK_EQ(outcome, std::string("kCorrupt"));  // shows which case failed
    }
  }
  const SystemDamage foreign_key_damages[] = {
      SystemDamage::kMissingRefTable, SystemDamage::kSelfReference,
      SystemDamage::kRefColumnOutOfRange, SystemDamage::kRefTypeMismatch,
      SystemDamage::kRefNotUnique};
  for (SystemDamage damage : foreign_key_damages) {
    const std::string outcome = LoadAfterDamage(damage);
    CHECK(outcome.find("table T, column position 2: foreign key") !=
          std::string::npos);
  }
  CHECK(
      LoadAfterDamage(SystemDamage::kNextTableIdTooLow).find("next_table_id") !=
      std::string::npos);
}

TEST(test_paged_catalog_adds_columns_and_reloads_them) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  Catalog& catalog = database.catalog();
  TableSchema schema;
  schema.name = "T";
  schema.columns = {{"id", DataType::kInt}, {"code", DataType::kText}};
  schema.primary_key_index = 0;
  schema.unique_indexes = {1};
  CHECK_OK(database.CreateTable(schema));  // table_id 3

  const ColumnDef int_column{"a", DataType::kInt, Value(std::int64_t{7})};
  const ColumnDef double_column{"b", DataType::kDouble, Value(1.5)};
  const ColumnDef text_column{"c", DataType::kText, Value(std::string("x"))};
  PageId last_first_page = 0;
  for (const ColumnDef& column : {int_column, double_column, text_column}) {
    auto extended = catalog.CheckAddColumn("t", column);
    CHECK_OK(extended);
    auto new_first = TableHeap::Create(database.allocator(), 3);
    CHECK_OK(new_first);
    if (!extended.ok() || !new_first.ok()) {
      return;
    }
    // This test changes the catalog only, so the index root stays as it is.
    CHECK_OK(catalog.CommitAddColumn("T", column, new_first.value(),
                                     catalog.IndexRoot("T").value_or(0)));
    last_first_page = new_first.value();
    const auto found = catalog.FindTable("t");
    CHECK(found.has_value());
    if (found.has_value()) {
      CHECK(SchemasEqual(*found, extended.value()));
    }
  }
  const auto altered = catalog.FindTable("T");
  CHECK(altered.has_value());
  if (altered.has_value()) {
    CHECK_EQ(altered->columns.size(), std::size_t{5});
    CHECK_EQ(altered->columns[2].name, std::string("a"));
    CHECK_EQ(altered->columns[4].name, std::string("c"));
    CHECK(altered->primary_key_index == 0);
    CHECK(altered->unique_indexes == std::vector<std::size_t>{1});
    CHECK(altered->columns[3].default_value == std::optional<Value>(1.5));
  }
  CHECK(catalog.FirstPage("t") == std::optional<PageId>(last_first_page));
  CHECK(catalog.TableId("t") == std::optional<std::uint32_t>(3));

  Catalog fresh(database.buffer(), database.allocator(), 1, 2);
  CHECK_OK(fresh.Load());
  const auto reloaded = fresh.FindTable("t");
  CHECK(reloaded.has_value());
  if (reloaded.has_value() && altered.has_value()) {
    CHECK(SchemasEqual(*reloaded, *altered));
  }
  CHECK(fresh.FirstPage("t") == std::optional<PageId>(last_first_page));

  CHECK_ERROR(catalog.CheckAddColumn(
                  "t", ColumnDef{"A", DataType::kInt, Value(std::int64_t{0})}),
              kAlreadyExists);
  CHECK_ERROR(catalog.CheckAddColumn("nosuch", int_column), kNotFound);
  CHECK_ERROR(catalog.CheckAddColumn(
                  "t", ColumnDef{"d", DataType::kInt, Value(std::string("x"))}),
              kInvalidArgument);
  CHECK_ERROR(
      catalog.CheckAddColumn("t", ColumnDef{"e", DataType::kInt, std::nullopt}),
      kInvalidArgument);
}

TEST(test_paged_catalog_add_column_failure_leaves_the_old_schema) {
  // A closed database with one table, read back through a cold pool.
  const ColumnDef column{"extra", DataType::kInt, Value(std::int64_t{1})};
  std::unique_ptr<MemoryPageStore> base;
  PageId table_page = 0;
  PageId index_root = 0;
  {
    OpenedDatabase env = OpenMemoryDatabase();
    if (env.database == nullptr) {
      return;
    }
    CHECK_OK(env.database->CreateTable(BasicSchema("T")));
    table_page = *env.database->catalog().FirstPage("T");
    index_root = env.database->catalog().IndexRoot("T").value_or(0);
    // Tables are added until the sys_columns row of one more column needs a new
    // page there, which a probe on a copy shows by the growth of the store. The
    // allocation of that page is the step that the sweep below fails: a read
    // never fails here, because every page that the call touches is in the
    // pool.
    bool does_add_allocate = false;
    for (int filler = 0; filler < 200 && !does_add_allocate; ++filler) {
      CHECK_OK(env.database->buffer().FlushAll());
      auto probe = strata::testing::OpenCopy(*env.store, 16);
      if (probe.database == nullptr) {
        return;
      }
      const PageId pages_before = probe.store->PageCount();
      CHECK_OK(probe.database->catalog().CommitAddColumn(
          "T", column, table_page, index_root));
      does_add_allocate = probe.store->PageCount() > pages_before;
      if (!does_add_allocate) {
        CHECK_OK(env.database->CreateTable(
            BasicSchema("F" + std::to_string(filler))));
      }
    }
    CHECK(does_add_allocate);
    CHECK_OK(env.database->Close());
    base = strata::testing::CopyOfStore(*env.store);
  }
  bool has_succeeded = false;
  int failures_seen = 0;
  for (int nth_call = 1; nth_call < 100 && !has_succeeded; ++nth_call) {
    auto opened = strata::testing::OpenCopy(*base, 4);
    if (opened.database == nullptr) {
      return;
    }
    Database& database = *opened.database;
    const std::optional<TableSchema> before = database.catalog().FindTable("T");
    opened.store->InjectFailure(Operation::kAllocate, nth_call);
    const auto status =
        database.catalog().CommitAddColumn("T", column, table_page, index_root);
    opened.store->ClearInjectedFailures();
    if (!status.has_value()) {
      has_succeeded = true;
      continue;
    }
    CHECK_ERROR(status, kIo);
    ++failures_seen;
    const std::optional<TableSchema> after = database.catalog().FindTable("T");
    CHECK(before.has_value() && after.has_value());
    if (before.has_value() && after.has_value()) {
      CHECK(SchemasEqual(*before, *after));
    }
    // The pages hold the old schema, unless the undo itself failed.
    CHECK_OK(database.buffer().FlushAll());
    Catalog fresh(database.buffer(), database.allocator(), 1, 2);
    const auto loaded = fresh.Load();
    const bool is_undo_failure =
        strata::testing::MessageOf(status).find(
            "; undoing the rows also failed:") != std::string::npos;
    if (!is_undo_failure) {
      CHECK_OK(loaded);
      const auto reloaded = fresh.FindTable("T");
      CHECK(reloaded.has_value());
      if (reloaded.has_value() && before.has_value()) {
        CHECK(SchemasEqual(*reloaded, *before));
      }
    }
  }
  CHECK(has_succeeded);
  // The sweep reached a failure, so the checks above ran at least once.
  CHECK(failures_seen > 0);
}

TEST(test_paged_catalog_drops_a_table_and_never_reuses_its_id) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  Catalog& catalog = database.catalog();
  CHECK_OK(database.CreateTable(BasicSchema("a")));  // 3
  CHECK_OK(database.CreateTable(BasicSchema("b")));  // 4
  CHECK_OK(database.CreateTable(BasicSchema("c")));  // 5
  CHECK_OK(catalog.DropTable("B"));
  CHECK(!catalog.FindTable("b").has_value());
  CHECK_EQ(catalog.TableNames(), (std::vector<std::string>{"a", "c"}));

  Catalog fresh(database.buffer(), database.allocator(), 1, 2);
  CHECK_OK(fresh.Load());
  CHECK(!fresh.FindTable("b").has_value());
  CHECK_EQ(fresh.TableNames(), (std::vector<std::string>{"a", "c"}));

  CHECK_OK(database.CreateTable(BasicSchema("d")));
  CHECK(catalog.TableId("d") == std::optional<std::uint32_t>(6));
  CHECK_ERROR(catalog.DropTable("b"), kNotFound);
  CHECK_EQ(SystemRowCount(database, 1), std::size_t{3});

  // A table that another table references stays, because a foreign key to a
  // missing table would fail the next Load.
  TableSchema kid = BasicSchema("kid");
  kid.columns.push_back({"parent", DataType::kInt});
  kid.foreign_keys = {ForeignKey{2, "a", 0}};
  CHECK_OK(database.CreateTable(kid));
  const auto refused = catalog.DropTable("A");
  CHECK_ERROR(refused, kInvalidArgument);
  CHECK_MESSAGE_CONTAINS(refused, "table a is referenced by kid.parent");
  CHECK(catalog.FindTable("a").has_value());
  CHECK_OK(catalog.DropTable("kid"));
  CHECK_OK(catalog.DropTable("a"));
  Catalog after(database.buffer(), database.allocator(), 1, 2);
  CHECK_OK(after.Load());
  CHECK_EQ(after.TableNames(), (std::vector<std::string>{"c", "d"}));
}

namespace {

// A database with the table "t" (INT key, table id 3, pages 3 and 4) and the
// table "k" (TEXT key, table id 4, page 5), whose sys_tables row of table_id
// gets index_root = new_root. It is closed and opened again over a copy of the
// store, so that Catalog::Load meets the change.
strata::testing::OpenedCopy OpenAfterRootRewrite(std::int64_t table_id,
                                                 std::int64_t new_root) {
  OpenedDatabase env = OpenMemoryDatabase();
  strata::testing::OpenedCopy none;
  if (env.database == nullptr) {
    return none;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("t")));
  TableSchema text_key;
  text_key.name = "k";
  text_key.columns = {{"code", DataType::kText}, {"v", DataType::kInt}};
  text_key.primary_key_index = 0;
  CHECK_OK(database.CreateTable(text_key));
  auto tables = TableHeap::Open(database.buffer(), database.allocator(),
                                Catalog::SysTablesSchema(), 1, 1);
  CHECK_OK(tables);
  if (!tables.ok()) {
    return none;
  }
  const auto row = FindSystemRow(tables.value(), 0, table_id);
  CHECK(row.has_value());
  if (!row.has_value()) {
    return none;
  }
  SetIntColumn(tables.value(), *row, 5, new_root);
  CHECK_OK(database.Close());
  return strata::testing::OpenCopy(*env.store);
}

}  // namespace

TEST(test_paged_catalog_records_the_index_root) {
  OpenedDatabase env = OpenMemoryDatabase();
  if (env.database == nullptr) {
    return;
  }
  Database& database = *env.database;
  CHECK_OK(database.CreateTable(BasicSchema("t")));
  TableSchema text_key;
  text_key.name = "k";
  text_key.columns = {{"code", DataType::kText}, {"v", DataType::kInt}};
  text_key.primary_key_index = 0;
  CHECK_OK(database.CreateTable(text_key));

  // The INT table has its first page 3 and its root 4, the TEXT table only a
  // first page, and no page more.
  CHECK(database.catalog().FirstPage("t") == std::optional<PageId>(3));
  CHECK(database.catalog().IndexRoot("t") == std::optional<PageId>(4));
  CHECK(database.catalog().FirstPage("k") == std::optional<PageId>(5));
  CHECK(!database.catalog().IndexRoot("k").has_value());
  CHECK_EQ(database.buffer().PageCount(), PageId{6});
  auto tables = TableHeap::Open(database.buffer(), database.allocator(),
                                Catalog::SysTablesSchema(), 1, 1);
  CHECK_OK(tables);
  if (!tables.ok()) {
    return;
  }
  const auto int_row = FindSystemRow(tables.value(), 0, 3);
  const auto text_row = FindSystemRow(tables.value(), 0, 4);
  CHECK(int_row.has_value() && text_row.has_value());
  if (int_row.has_value() && text_row.has_value()) {
    CHECK_EQ(int_row->row.size(), std::size_t{6});
    CHECK_EQ(std::get<std::int64_t>(int_row->row[2]), std::int64_t{3});
    CHECK_EQ(std::get<std::int64_t>(int_row->row[5]), std::int64_t{4});
    CHECK_EQ(std::get<std::int64_t>(text_row->row[5]), std::int64_t{0});
  }
  // The root is an empty leaf of its table.
  {
    auto fetched = database.buffer().FetchPage(4, LatchMode::kShared);
    CHECK_OK(fetched);
    if (fetched.ok()) {
      const strata::paged::NodeView node(fetched.value().data());
      CHECK_EQ(node.table_id(), std::uint32_t{3});
      CHECK_EQ(node.level(), std::uint16_t{0});
      CHECK_EQ(node.key_count(), std::uint16_t{0});
      CHECK_EQ(node.next_page_id(), strata::paged::kInvalidPageId);
    }
  }
  // After a reopen the catalog agrees.
  CHECK_OK(database.Close());
  auto reopened = strata::testing::OpenCopy(*env.store);
  CHECK(reopened.database != nullptr);
  if (reopened.database != nullptr) {
    CHECK(reopened.database->catalog().IndexRoot("t") ==
          std::optional<PageId>(4));
    CHECK(!reopened.database->catalog().IndexRoot("k").has_value());
  }

  // A root that the load rules refuse: a TEXT key with a root, an INT key
  // without one, a root of 2, a root past the end of the file. Each is kCorrupt
  // and names the table.
  struct Rewrite {
    std::int64_t table_id;
    std::int64_t new_root;
    const char* table_name;
  };
  const Rewrite rewrites[] = {{4, 4, "table k"},
                              {3, 0, "table t"},
                              {3, 2, "table t"},
                              {3, 100, "table t"}};
  for (const Rewrite& rewrite : rewrites) {
    auto opened = OpenAfterRootRewrite(rewrite.table_id, rewrite.new_root);
    CHECK(opened.database == nullptr);
    CHECK_ERROR(strata::paged::Status(opened.error), kCorrupt);
    CHECK_MESSAGE_CONTAINS(strata::paged::Status(opened.error),
                           rewrite.table_name);
  }
  // A root that is a heap page passes the load and fails when the index is
  // opened.
  auto heap_root = OpenAfterRootRewrite(3, 5);
  CHECK(heap_root.database != nullptr);
  if (heap_root.database != nullptr) {
    const auto index = heap_root.database->Index("t");
    CHECK_ERROR(index, kCorrupt);
    CHECK_MESSAGE_CONTAINS(index,
                           "index of t: page 5: index node has page_type 2");
    CHECK_EQ(heap_root.database->buffer().PinnedFrameCount(), std::size_t{0});
  }
}
