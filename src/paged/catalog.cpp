#include "strata/paged/catalog.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "strata/paged/btree.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/tuple_codec.h"

namespace strata::paged {

namespace {

// default_kind values of sys_columns.
constexpr std::int64_t kDefaultNone = 0;
constexpr std::int64_t kDefaultInt = 1;
constexpr std::int64_t kDefaultDouble = 2;
constexpr std::int64_t kDefaultText = 3;

using domain::DataType;
using domain::Row;
using domain::TableSchema;
using domain::Value;

Value IntValue(std::int64_t number) { return Value(number); }

std::int64_t IntAt(const Row& row, std::size_t column) {
  return std::get<std::int64_t>(row[column]);
}
const std::string& TextAt(const Row& row, std::size_t column) {
  return std::get<std::string>(row[column]);
}

bool IsIdentifierStart(char character) {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') || character == '_';
}
bool IsIdentifierPart(char character) {
  return IsIdentifierStart(character) || (character >= '0' && character <= '9');
}
bool IsValidIdentifier(const std::string& name) {
  if (name.empty() || !IsIdentifierStart(name[0])) {
    return false;
  }
  for (char character : name) {
    if (!IsIdentifierPart(character)) {
      return false;
    }
  }
  return true;
}

PageError Corrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, "catalog: " + detail);
}

// Why a foreign key of schema does not fit its target, or no value. target is
// the schema that ref_table names, or null when there is none. The planner
// reads the target column by position, so each of these would make it index
// past the end of a row or compare values of different types.
std::optional<std::string> ForeignKeyProblem(
    const TableSchema& schema, const domain::ForeignKey& foreign_key,
    const TableSchema* target) {
  if (domain::ToLower(foreign_key.ref_table) == domain::ToLower(schema.name)) {
    return std::string("foreign key references its own table");
  }
  if (target == nullptr) {
    return "foreign key target table " + foreign_key.ref_table +
           " does not exist";
  }
  if (foreign_key.ref_column_index >= target->columns.size()) {
    return "foreign key target column " +
           std::to_string(foreign_key.ref_column_index) +
           " is not a column of " + target->name;
  }
  const domain::ColumnDef& referenced =
      target->columns[foreign_key.ref_column_index];
  const std::string target_name = target->name + "." + referenced.name;
  if (referenced.type != schema.columns[foreign_key.column_index].type) {
    return "foreign key target " + target_name + " has a different type";
  }
  const bool is_unique =
      foreign_key.ref_column_index == target->primary_key_index ||
      std::ranges::find(target->unique_indexes, foreign_key.ref_column_index) !=
          target->unique_indexes.end();
  if (!is_unique) {
    return "foreign key target " + target_name +
           " is neither the primary key nor unique";
  }
  return std::nullopt;
}

PageError Invalid(const std::string& detail) {
  return MakeError(PageErrorCode::kInvalidArgument, detail);
}

std::string DoubleToHex(double number) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(number);
  char text[17];
  std::snprintf(text, sizeof(text), "%016" PRIx64, bits);
  return text;
}

bool HexToDouble(const std::string& text, double* number) {
  if (text.size() != 16) {
    return false;
  }
  std::uint64_t bits = 0;
  for (char digit : text) {
    bits <<= 4;
    if (digit >= '0' && digit <= '9') {
      bits |= static_cast<std::uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      bits |= static_cast<std::uint64_t>(digit - 'a' + 10);
    } else {
      return false;
    }
  }
  *number = std::bit_cast<double>(bits);
  return true;
}

bool TextToInt(const std::string& text, std::int64_t* number) {
  const char* const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, *number);
  return parsed.ec == std::errc() && parsed.ptr == end;
}

// One sys_columns row as read, before it is checked.
struct RawColumn {
  std::int64_t position;
  std::string name;
  std::int64_t type;
  std::int64_t default_kind;
  std::string default_text;
  std::int64_t is_unique;
  std::string ref_table;
  std::int64_t ref_column;
};

bool ByPosition(const RawColumn& left, const RawColumn& right) {
  return left.position < right.position;
}

struct RawTable {
  std::uint32_t table_id;
  std::string name;
  std::int64_t first_page;
  std::int64_t primary_key_index;
  std::int64_t auto_increment_index;
  std::int64_t index_root;
  std::vector<RawColumn> columns;
};

Row TablesRow(std::uint32_t table_id, const std::string& name,
              PageId first_page, std::int64_t primary_key_index,
              std::int64_t auto_increment_index, PageId index_root) {
  return Row{IntValue(table_id),
             Value(name),
             IntValue(first_page),
             IntValue(primary_key_index),
             IntValue(auto_increment_index),
             IntValue(index_root)};
}

// A table has an index exactly when its primary key is an INT (ADR-0048).
bool HasIndex(const TableSchema& schema) {
  return schema.columns[schema.primary_key_index].type == DataType::kInt;
}

Row ColumnsRow(std::uint32_t table_id, std::size_t position,
               const TableSchema& schema) {
  const domain::ColumnDef& column = schema.columns[position];
  std::int64_t default_kind = kDefaultNone;
  std::string default_text;
  if (column.default_value.has_value()) {
    const Value& value = *column.default_value;
    if (std::holds_alternative<std::int64_t>(value)) {
      default_kind = kDefaultInt;
      default_text = std::to_string(std::get<std::int64_t>(value));
    } else if (std::holds_alternative<double>(value)) {
      default_kind = kDefaultDouble;
      default_text = DoubleToHex(std::get<double>(value));
    } else {
      default_kind = kDefaultText;
      default_text = std::get<std::string>(value);
    }
  }
  const bool is_unique = std::ranges::find(schema.unique_indexes, position) !=
                         schema.unique_indexes.end();
  std::string ref_table;
  std::int64_t ref_column = -1;
  for (const domain::ForeignKey& key : schema.foreign_keys) {
    if (key.column_index == position) {
      ref_table = key.ref_table;
      ref_column = static_cast<std::int64_t>(key.ref_column_index);
    }
  }
  return Row{IntValue(table_id),
             IntValue(static_cast<std::int64_t>(position)),
             Value(column.name),
             IntValue(static_cast<std::int64_t>(column.type)),
             IntValue(default_kind),
             Value(default_text),
             IntValue(is_unique ? 1 : 0),
             Value(ref_table),
             IntValue(ref_column)};
}

// Rules of design document 2.6 for a schema given to CreateTable.
Status ValidateSchema(const TableSchema& schema) {
  if (!IsValidIdentifier(schema.name)) {
    return Invalid("table name '" + schema.name +
                   "' is not an identifier ([A-Za-z_][A-Za-z0-9_]*)");
  }
  if (domain::ToLower(schema.name).starts_with("sys_")) {
    return Invalid("table name '" + schema.name +
                   "' starts with the reserved prefix sys_");
  }
  if (schema.columns.empty()) {
    return Invalid("table " + schema.name + " has no columns");
  }
  std::set<std::string> column_names;
  for (const domain::ColumnDef& column : schema.columns) {
    if (!column_names.insert(domain::ToLower(column.name)).second) {
      return Invalid("table " + schema.name + " has the column " + column.name +
                     " twice");
    }
  }
  if (schema.primary_key_index >= schema.columns.size()) {
    return Invalid("primary_key_index " +
                   std::to_string(schema.primary_key_index) +
                   " is outside the " + std::to_string(schema.columns.size()) +
                   " columns of table " + schema.name);
  }
  if (schema.auto_increment_index.has_value()) {
    const std::size_t index = *schema.auto_increment_index;
    if (index >= schema.columns.size() ||
        schema.columns[index].type != DataType::kInt) {
      return Invalid("auto_increment_index of table " + schema.name +
                     " must point to an INT column");
    }
  }
  std::optional<std::size_t> previous;
  for (std::size_t index : schema.unique_indexes) {
    if (index >= schema.columns.size() ||
        (previous.has_value() && index <= *previous)) {
      return Invalid("unique_indexes of table " + schema.name +
                     " must be strictly ascending column positions");
    }
    previous = index;
  }
  previous.reset();
  for (const domain::ForeignKey& key : schema.foreign_keys) {
    if (key.column_index >= schema.columns.size() ||
        (previous.has_value() && key.column_index <= *previous) ||
        key.ref_table.empty() ||
        key.ref_column_index > static_cast<std::size_t>(
                                   std::numeric_limits<std::int32_t>::max())) {
      return Invalid("foreign_keys of table " + schema.name +
                     " must have strictly ascending column positions inside "
                     "the table, a table name and a column position");
    }
    previous = key.column_index;
  }
  return std::nullopt;
}

// kInvalidArgument when the sys_columns row of the column at position cannot
// be stored as one tuple (a long name or TEXT DEFAULT).
Status CheckColumnRowFits(const TableSchema& schema, std::size_t position) {
  PageResult<std::vector<std::uint8_t>> column_row = TupleCodec::Encode(
      Catalog::SysColumnsSchema(), ColumnsRow(0, position, schema));
  if (!column_row.ok()) {
    return Invalid(
        "column " + schema.columns[position].name + " of table " + schema.name +
        " cannot be stored in the catalog: " + column_row.error().message);
  }
  return std::nullopt;
}

// kInvalidArgument when the sys_tables row of the schema, or one of its
// sys_columns rows, cannot be stored as one tuple (a long name or TEXT
// DEFAULT). Encoding is pure, so CreateTable runs it before it allocates the
// table's first page.
Status CheckCatalogRowsFit(const TableSchema& schema) {
  PageResult<std::vector<std::uint8_t>> table_row = TupleCodec::Encode(
      Catalog::SysTablesSchema(), TablesRow(0, schema.name, 0, 0, -1, 0));
  if (!table_row.ok()) {
    return Invalid(
        "table " + schema.name +
        " cannot be stored in the catalog: " + table_row.error().message);
  }
  for (std::size_t position = 0; position < schema.columns.size(); ++position) {
    if (Status status = CheckColumnRowFits(schema, position)) {
      return status;
    }
  }
  return std::nullopt;
}

// True when the value has the storage type of the column.
bool HasColumnType(const Value& value, DataType type) {
  return value.index() == static_cast<std::size_t>(type);
}

// Restores the rows a failed DROP TABLE had deleted; the text to add to the
// original error when that fails too, else empty.
std::string ReinsertRows(TableHeap& heap, const std::vector<Row>& rows) {
  std::string failure;
  for (const Row& row : rows) {
    PageResult<Rid> inserted = heap.Insert(row);
    if (!inserted.ok() && failure.empty()) {
      failure = inserted.error().message;
    }
  }
  return failure.empty() ? std::string()
                         : "; undoing the rows also failed: " + failure;
}

}  // namespace

TableSchema Catalog::SysTablesSchema() {
  TableSchema schema;
  schema.name = "sys_tables";
  schema.columns = {{"table_id", DataType::kInt},
                    {"name", DataType::kText},
                    {"first_page", DataType::kInt},
                    {"primary_key_index", DataType::kInt},
                    {"auto_increment_index", DataType::kInt},
                    {"index_root", DataType::kInt}};
  schema.primary_key_index = 0;
  return schema;
}

TableSchema Catalog::SysColumnsSchema() {
  TableSchema schema;
  schema.name = "sys_columns";
  schema.columns = {
      {"table_id", DataType::kInt},     {"position", DataType::kInt},
      {"name", DataType::kText},        {"type", DataType::kInt},
      {"default_kind", DataType::kInt}, {"default_text", DataType::kText},
      {"is_unique", DataType::kInt},    {"ref_table", DataType::kText},
      {"ref_column", DataType::kInt}};
  schema.primary_key_index = 0;
  return schema;
}

Catalog::Catalog(BufferManager& buffer, PageAllocator& allocator,
                 PageId sys_tables_first_page, PageId sys_columns_first_page)
    : buffer_(buffer),
      allocator_(allocator),
      sys_tables_first_page_(sys_tables_first_page),
      sys_columns_first_page_(sys_columns_first_page) {}

Status Catalog::Load() {
  // The header's next_table_id, to be checked against the tables found.
  std::uint32_t header_next_table_id = 0;
  {
    PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kShared);
    if (!header.ok()) {
      return header.error();
    }
    header_next_table_id =
        ReadLe32(header.value().data() + layout::kNextTableId);
  }
  PageResult<TableHeap> opened_tables =
      TableHeap::Open(buffer_, allocator_, SysTablesSchema(), kSysTablesId,
                      sys_tables_first_page_);
  if (!opened_tables.ok()) {
    return opened_tables.error();
  }
  PageResult<TableHeap> opened_columns =
      TableHeap::Open(buffer_, allocator_, SysColumnsSchema(), kSysColumnsId,
                      sys_columns_first_page_);
  if (!opened_columns.ok()) {
    return opened_columns.error();
  }
  auto sys_tables =
      std::make_unique<TableHeap>(std::move(opened_tables).value());
  auto sys_columns =
      std::make_unique<TableHeap>(std::move(opened_columns).value());

  const PageId page_count = buffer_.PageCount();
  std::map<std::uint32_t, RawTable> tables;  // ordered by table_id
  std::set<std::string> lower_names;
  HeapScan table_scan = sys_tables->Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = table_scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      break;
    }
    const Row& row = next.value()->row;
    RawTable table;
    const std::int64_t raw_id = IntAt(row, 0);
    if (raw_id < kFirstUserTableId ||
        raw_id > std::numeric_limits<std::uint32_t>::max()) {
      return Corrupt("sys_tables has the invalid table_id " +
                     std::to_string(raw_id));
    }
    table.table_id = static_cast<std::uint32_t>(raw_id);
    table.name = TextAt(row, 1);
    table.first_page = IntAt(row, 2);
    table.primary_key_index = IntAt(row, 3);
    table.auto_increment_index = IntAt(row, 4);
    table.index_root = IntAt(row, 5);
    if (tables.contains(table.table_id)) {
      return Corrupt("table_id " + std::to_string(table.table_id) +
                     " appears twice in sys_tables");
    }
    if (!lower_names.insert(domain::ToLower(table.name)).second) {
      return Corrupt("table name " + table.name +
                     " appears twice in sys_tables");
    }
    if (table.first_page < 0 || table.first_page >= page_count) {
      return Corrupt("table " + table.name + " has first_page " +
                     std::to_string(table.first_page) + " outside the file (" +
                     std::to_string(page_count) + " pages)");
    }
    tables.emplace(table.table_id, std::move(table));
  }
  HeapScan column_scan = sys_columns->Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = column_scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      break;
    }
    const Row& row = next.value()->row;
    auto owner = tables.find(static_cast<std::uint32_t>(IntAt(row, 0)));
    if (owner == tables.end()) {
      return Corrupt("sys_columns has a row for the unknown table_id " +
                     std::to_string(IntAt(row, 0)));
    }
    owner->second.columns.push_back(RawColumn{
        IntAt(row, 1), TextAt(row, 2), IntAt(row, 3), IntAt(row, 4),
        TextAt(row, 5), IntAt(row, 6), TextAt(row, 7), IntAt(row, 8)});
  }

  std::vector<Entry> entries;
  std::unordered_map<std::string, std::size_t> position_of_name;
  std::uint32_t highest_table_id = 0;
  for (auto& [table_id, table] : tables) {
    highest_table_id = std::max(highest_table_id, table_id);
    std::ranges::sort(table.columns, ByPosition);
    TableSchema schema;
    schema.name = table.name;
    if (table.columns.empty()) {
      return Corrupt("table " + table.name + " has no columns");
    }
    for (std::size_t position = 0; position < table.columns.size();
         ++position) {
      const RawColumn& raw = table.columns[position];
      const std::string where = "table " + table.name + ", column position " +
                                std::to_string(position);
      if (raw.position != static_cast<std::int64_t>(position)) {
        return Corrupt(where + ": the positions are not 0 to " +
                       std::to_string(table.columns.size() - 1) + " each once");
      }
      if (raw.type < 0 || raw.type > 2) {
        return Corrupt(where + ": type " + std::to_string(raw.type) +
                       " is not 0, 1 or 2");
      }
      if (raw.default_kind < kDefaultNone || raw.default_kind > kDefaultText) {
        return Corrupt(where + ": default_kind " +
                       std::to_string(raw.default_kind) + " is not 0 to 3");
      }
      if (raw.is_unique != 0 && raw.is_unique != 1) {
        return Corrupt(where + ": is_unique is not 0 or 1");
      }
      if (!raw.ref_table.empty() && raw.ref_column < 0) {
        return Corrupt(where + ": ref_table is set but ref_column is " +
                       std::to_string(raw.ref_column));
      }
      domain::ColumnDef column{raw.name, static_cast<DataType>(raw.type)};
      if (raw.default_kind == kDefaultInt) {
        std::int64_t number = 0;
        if (!TextToInt(raw.default_text, &number)) {
          return Corrupt(where + ": default_text is not an integer");
        }
        column.default_value = Value(number);
      } else if (raw.default_kind == kDefaultDouble) {
        double number = 0;
        if (!HexToDouble(raw.default_text, &number)) {
          return Corrupt(where + ": default_text is not 16 hex digits");
        }
        column.default_value = Value(number);
      } else if (raw.default_kind == kDefaultText) {
        column.default_value = Value(raw.default_text);
      }
      schema.columns.push_back(std::move(column));
      if (raw.is_unique == 1) {
        schema.unique_indexes.push_back(position);
      }
      if (!raw.ref_table.empty()) {
        schema.foreign_keys.push_back(domain::ForeignKey{
            position, raw.ref_table, static_cast<std::size_t>(raw.ref_column)});
      }
    }
    if (table.primary_key_index < 0 ||
        table.primary_key_index >=
            static_cast<std::int64_t>(schema.columns.size())) {
      return Corrupt("table " + table.name + ": primary_key_index " +
                     std::to_string(table.primary_key_index) +
                     " does not point to a column");
    }
    schema.primary_key_index =
        static_cast<std::size_t>(table.primary_key_index);
    if (table.auto_increment_index != -1) {
      if (table.auto_increment_index < 0 ||
          table.auto_increment_index >=
              static_cast<std::int64_t>(schema.columns.size())) {
        return Corrupt("table " + table.name + ": auto_increment_index " +
                       std::to_string(table.auto_increment_index) +
                       " does not point to a column");
      }
      schema.auto_increment_index =
          static_cast<std::size_t>(table.auto_increment_index);
    }
    // A table has a root exactly when its primary key is an INT, and a root is
    // a page of the file above the system pages (ADR-0048).
    if (!HasIndex(schema) && table.index_root != 0) {
      return Corrupt("table " + table.name + ": index_root " +
                     std::to_string(table.index_root) +
                     " is set but the primary key is not an INT");
    }
    if (HasIndex(schema) && table.index_root == 0) {
      return Corrupt("table " + table.name +
                     ": index_root is 0 but the primary key is an INT");
    }
    if (table.index_root != 0 &&
        (table.index_root < kFirstUserPage || table.index_root >= page_count)) {
      return Corrupt("table " + table.name + ": index_root " +
                     std::to_string(table.index_root) + " is below page " +
                     std::to_string(kFirstUserPage) + " or outside the file (" +
                     std::to_string(page_count) + " pages)");
    }
    position_of_name[domain::ToLower(table.name)] = entries.size();
    entries.push_back(Entry{table_id, std::move(schema),
                            static_cast<PageId>(table.first_page),
                            static_cast<PageId>(table.index_root)});
  }
  // A foreign key target is checked once every table is known. The lookup by
  // lowercase name is O(1) on average.
  for (const Entry& entry : entries) {
    for (const domain::ForeignKey& foreign_key : entry.schema.foreign_keys) {
      auto target =
          position_of_name.find(domain::ToLower(foreign_key.ref_table));
      const TableSchema* target_schema = target == position_of_name.end()
                                             ? nullptr
                                             : &entries[target->second].schema;
      if (std::optional<std::string> problem =
              ForeignKeyProblem(entry.schema, foreign_key, target_schema)) {
        return Corrupt("table " + entry.schema.name + ", column position " +
                       std::to_string(foreign_key.column_index) + ": " +
                       *problem);
      }
    }
  }
  if (header_next_table_id <= highest_table_id ||
      header_next_table_id < kFirstUserTableId) {
    return Corrupt("the file header's next_table_id " +
                   std::to_string(header_next_table_id) +
                   " is not above the highest table_id " +
                   std::to_string(highest_table_id));
  }

  std::unique_lock<std::shared_mutex> cache_lock(cache_mutex_);
  sys_tables_ = std::move(sys_tables);
  sys_columns_ = std::move(sys_columns);
  entries_ = std::move(entries);
  position_of_name_ = std::move(position_of_name);
  next_table_id_ = header_next_table_id;
  return std::nullopt;
}

std::optional<TableSchema> Catalog::FindTable(const std::string& name) const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  auto found = position_of_name_.find(domain::ToLower(name));
  if (found == position_of_name_.end()) {
    return std::nullopt;
  }
  return entries_[found->second].schema;
}

std::vector<std::string> Catalog::TableNames() const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    names.push_back(entry.schema.name);
  }
  return names;
}

std::optional<std::uint32_t> Catalog::TableId(const std::string& name) const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  auto found = position_of_name_.find(domain::ToLower(name));
  if (found == position_of_name_.end()) {
    return std::nullopt;
  }
  return entries_[found->second].table_id;
}

std::optional<PageId> Catalog::FirstPage(const std::string& name) const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  auto found = position_of_name_.find(domain::ToLower(name));
  if (found == position_of_name_.end()) {
    return std::nullopt;
  }
  return entries_[found->second].first_page;
}

std::optional<PageId> Catalog::IndexRoot(const std::string& name) const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  auto found = position_of_name_.find(domain::ToLower(name));
  if (found == position_of_name_.end() ||
      entries_[found->second].index_root == 0) {
    return std::nullopt;
  }
  return entries_[found->second].index_root;
}

std::string Catalog::UndoRows(const std::vector<Rid>& table_rows,
                              const std::vector<Rid>& column_rows) {
  std::string failure;
  for (const Rid& rid : column_rows) {
    if (Status status = sys_columns_->Delete(rid)) {
      if (failure.empty()) {
        failure = status->message;
      }
    }
  }
  for (const Rid& rid : table_rows) {
    if (Status status = sys_tables_->Delete(rid)) {
      if (failure.empty()) {
        failure = status->message;
      }
    }
  }
  return failure.empty() ? std::string()
                         : "; undoing the rows also failed: " + failure;
}

PageResult<TableSchema> Catalog::CreateTable(const TableSchema& schema) {
  std::lock_guard<std::mutex> ddl_lock(ddl_mutex_);
  if (sys_tables_ == nullptr) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "the catalog has not been loaded");
  }
  if (Status status = ValidateSchema(schema)) {
    return *status;
  }
  if (FindTable(schema.name).has_value()) {
    return MakeError(PageErrorCode::kAlreadyExists,
                     "table " + schema.name + " already exists");
  }
  if (Status status = CheckCatalogRowsFit(schema)) {
    return *status;
  }
  const std::uint32_t table_id = next_table_id_;
  PageResult<PageId> first_page = TableHeap::Create(allocator_, table_id);
  if (!first_page.ok()) {
    return first_page.error();
  }
  // The root of the index, after the first page of the heap, so that a new
  // database gives its first table the pages 3 and 4.
  PageId index_root = 0;
  if (HasIndex(schema)) {
    PageResult<BTree> tree = BTree::Create(buffer_, allocator_, table_id);
    if (!tree.ok()) {
      return tree.error();
    }
    index_root = tree.value().root();
  }

  std::vector<Rid> table_rows;
  std::vector<Rid> column_rows;
  PageError failure{PageErrorCode::kIo, ""};
  bool has_failed = false;

  const std::int64_t auto_increment =
      schema.auto_increment_index.has_value()
          ? static_cast<std::int64_t>(*schema.auto_increment_index)
          : -1;
  PageResult<Rid> table_row = sys_tables_->Insert(
      TablesRow(table_id, schema.name, first_page.value(),
                static_cast<std::int64_t>(schema.primary_key_index),
                auto_increment, index_root));
  if (table_row.ok()) {
    table_rows.push_back(table_row.value());
  } else {
    failure = table_row.error();
    has_failed = true;
  }
  for (std::size_t position = 0;
       !has_failed && position < schema.columns.size(); ++position) {
    PageResult<Rid> inserted =
        sys_columns_->Insert(ColumnsRow(table_id, position, schema));
    if (inserted.ok()) {
      column_rows.push_back(inserted.value());
    } else {
      failure = inserted.error();
      has_failed = true;
    }
  }
  if (!has_failed) {
    PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kExclusive);
    if (header.ok()) {
      PageGuard guard = std::move(header).value();
      WriteLe32(guard.mutable_data() + layout::kNextTableId, table_id + 1);
      guard.MarkDirty();
    } else {
      failure = header.error();
      has_failed = true;
    }
  }
  if (has_failed) {
    failure.message += UndoRows(table_rows, column_rows);
    return failure;
  }
  std::unique_lock<std::shared_mutex> cache_lock(cache_mutex_);
  position_of_name_[domain::ToLower(schema.name)] = entries_.size();
  entries_.push_back(Entry{table_id, schema, first_page.value(), index_root});
  next_table_id_ = table_id + 1;
  return schema;
}

PageResult<TableSchema> Catalog::CheckAddColumn(
    const std::string& table, const domain::ColumnDef& column) const {
  std::shared_lock<std::shared_mutex> cache_lock(cache_mutex_);
  auto found = position_of_name_.find(domain::ToLower(table));
  if (found == position_of_name_.end()) {
    return MakeError(PageErrorCode::kNotFound, "no table named " + table);
  }
  TableSchema extended = entries_[found->second].schema;
  const std::string lower_column = domain::ToLower(column.name);
  for (const domain::ColumnDef& existing : extended.columns) {
    if (domain::ToLower(existing.name) == lower_column) {
      return MakeError(
          PageErrorCode::kAlreadyExists,
          "table " + extended.name + " already has the column " + column.name);
    }
  }
  if (!column.default_value.has_value() ||
      !HasColumnType(*column.default_value, column.type)) {
    return Invalid("column " + column.name + " of table " + extended.name +
                   " needs a DEFAULT of its own type");
  }
  extended.columns.push_back(column);
  if (Status status =
          CheckColumnRowFits(extended, extended.columns.size() - 1)) {
    return *status;
  }
  return extended;
}

PageResult<Rid> Catalog::FindTablesRow(std::uint32_t table_id) const {
  HeapScan scan = sys_tables_->Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      return Corrupt("sys_tables has no row for table_id " +
                     std::to_string(table_id));
    }
    if (IntAt(next.value()->row, 0) == static_cast<std::int64_t>(table_id)) {
      return next.value()->rid;
    }
  }
}

Status Catalog::CommitAddColumn(const std::string& table,
                                const domain::ColumnDef& column,
                                PageId new_first_page, PageId new_index_root) {
  std::lock_guard<std::mutex> ddl_lock(ddl_mutex_);
  if (sys_tables_ == nullptr) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "the catalog has not been loaded");
  }
  PageResult<TableSchema> extended = CheckAddColumn(table, column);
  if (!extended.ok()) {
    return extended.error();
  }
  // Only the thread that holds ddl_mutex_ changes the cache, so reading it
  // here needs no lock.
  const std::size_t entry_index = position_of_name_.at(domain::ToLower(table));
  const Entry& entry = entries_[entry_index];
  const std::size_t position = extended.value().columns.size() - 1;
  PageResult<Rid> tables_rid = FindTablesRow(entry.table_id);
  if (!tables_rid.ok()) {
    return tables_rid.error();
  }
  const std::int64_t auto_increment =
      entry.schema.auto_increment_index.has_value()
          ? static_cast<std::int64_t>(*entry.schema.auto_increment_index)
          : -1;
  PageResult<Rid> column_rid = sys_columns_->Insert(
      ColumnsRow(entry.table_id, position, extended.value()));
  if (!column_rid.ok()) {
    return column_rid.error();
  }
  Status updated = sys_tables_->Update(
      tables_rid.value(),
      TablesRow(entry.table_id, entry.schema.name, new_first_page,
                static_cast<std::int64_t>(entry.schema.primary_key_index),
                auto_increment, new_index_root));
  if (updated.has_value()) {
    PageError failure = *updated;
    if (Status undone = sys_columns_->Delete(column_rid.value())) {
      failure.message += "; undoing the rows also failed: " + undone->message;
    }
    return failure;
  }
  std::unique_lock<std::shared_mutex> cache_lock(cache_mutex_);
  entries_[entry_index].schema = std::move(extended).value();
  entries_[entry_index].first_page = new_first_page;
  entries_[entry_index].index_root = new_index_root;
  return std::nullopt;
}

Status Catalog::DropTable(const std::string& table) {
  std::lock_guard<std::mutex> ddl_lock(ddl_mutex_);
  if (sys_tables_ == nullptr) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "the catalog has not been loaded");
  }
  auto found = position_of_name_.find(domain::ToLower(table));
  if (found == position_of_name_.end()) {
    return MakeError(PageErrorCode::kNotFound, "no table named " + table);
  }
  const std::size_t entry_index = found->second;
  const Entry& entry = entries_[entry_index];
  // A catalog with a foreign key to a missing table fails Load, so a table
  // that another table references stays.
  for (const Entry& other : entries_) {
    if (other.table_id == entry.table_id) {
      continue;
    }
    for (const domain::ForeignKey& key : other.schema.foreign_keys) {
      if (domain::ToLower(key.ref_table) == domain::ToLower(table)) {
        return Invalid("table " + entry.schema.name + " is referenced by " +
                       other.schema.name + "." +
                       other.schema.columns[key.column_index].name);
      }
    }
  }
  PageResult<Rid> tables_rid = FindTablesRow(entry.table_id);
  if (!tables_rid.ok()) {
    return tables_rid.error();
  }
  std::vector<ScanItem> column_items;
  HeapScan scan = sys_columns_->Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      break;
    }
    if (IntAt(next.value()->row, 0) ==
        static_cast<std::int64_t>(entry.table_id)) {
      column_items.push_back(std::move(*next.value()));
    }
  }
  std::vector<Row> deleted_columns;
  for (const ScanItem& item : column_items) {
    if (Status status = sys_columns_->Delete(item.rid)) {
      PageError failure = *status;
      failure.message += ReinsertRows(*sys_columns_, deleted_columns);
      return failure;
    }
    deleted_columns.push_back(item.row);
  }
  if (Status status = sys_tables_->Delete(tables_rid.value())) {
    PageError failure = *status;
    failure.message += ReinsertRows(*sys_columns_, deleted_columns);
    return failure;
  }
  std::unique_lock<std::shared_mutex> cache_lock(cache_mutex_);
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(entry_index));
  position_of_name_.clear();
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    position_of_name_[domain::ToLower(entries_[index].schema.name)] = index;
  }
  return std::nullopt;
}

}  // namespace strata::paged
