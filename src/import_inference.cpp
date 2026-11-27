#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/import.h"
#include "strata/domain/scalar_functions.h"

namespace strata::domain {

namespace {

constexpr std::size_t kMaxSkippedNotes = 10;

ExecutionError BindError(const std::string& message) {
  return ExecutionError{ErrorKind::kBind, message};
}

bool IsIdentifier(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char letter = name[i];
    const bool is_letter = (letter >= 'a' && letter <= 'z') ||
                           (letter >= 'A' && letter <= 'Z') || letter == '_';
    const bool is_digit = letter >= '0' && letter <= '9';
    if (!(is_letter || (i > 0 && is_digit))) {
      return false;
    }
  }
  return true;
}

// The reserved words of docs/DesignDocument.md section 2.3 in lower case. The
// parser refuses them as names, so a table or column named like one could
// not be used in a statement. The list has a fixed size, so the scan per name
// is bounded.
constexpr const char* kReservedWords[] = {
    "select", "from",  "where",  "join", "on",     "as",   "group",
    "by",     "order", "asc",    "desc", "insert", "into", "values",
    "update", "set",   "delete", "and",  "or",     "like", "limit",
    "offset", "count", "sum",    "null"};

bool IsReservedWord(const std::string& name) {
  const std::string lowered = ToLower(name);
  for (const char* word : kReservedWords) {
    if (lowered == word) {
      return true;
    }
  }
  return false;
}

const char* TypeName(DataType type) {
  switch (type) {
    case DataType::kInt:
      return "INT";
    case DataType::kDouble:
      return "DOUBLE";
    case DataType::kText:
      break;
  }
  return "TEXT";
}

// `0` or -?[1-9][0-9]* within int64_t: writing the value back gives the same
// text.
bool IsCanonicalInt(const std::string& cell) {
  std::optional<int64_t> parsed = ParseIntegerText(cell);
  return parsed && std::to_string(*parsed) == cell;
}

// A decimal whose output format (docs/DesignDocument.md section 2.3) is the
// cell itself.
bool IsCanonicalDouble(const std::string& cell) {
  std::optional<double> parsed = ParseDecimalText(cell);
  return parsed && FormatValue(Value(*parsed)) == cell;
}

DataType InferType(const std::vector<const std::vector<std::string>*>& rows,
                   std::size_t column) {
  if (rows.empty()) {
    return DataType::kText;
  }
  bool is_int = true;
  bool is_double = true;
  for (const std::vector<std::string>* row : rows) {
    const std::string& cell = (*row)[column];
    if (is_int && !IsCanonicalInt(cell)) {
      is_int = false;
    }
    if (is_double && !IsCanonicalDouble(cell)) {
      is_double = false;
    }
    if (!is_int && !is_double) {
      return DataType::kText;
    }
  }
  return is_int ? DataType::kInt : DataType::kDouble;
}

// The value of cell under type, or std::nullopt when the text is not one.
std::optional<Value> ConvertCell(const std::string& cell, DataType type) {
  switch (type) {
    case DataType::kInt: {
      std::optional<int64_t> parsed = ParseIntegerText(cell);
      if (!parsed) {
        return std::nullopt;
      }
      return Value(*parsed);
    }
    case DataType::kDouble: {
      std::optional<double> parsed = ParseDecimalText(cell);
      if (!parsed) {
        return std::nullopt;
      }
      return Value(*parsed);
    }
    case DataType::kText:
      break;
  }
  return Value(cell);
}

std::optional<std::size_t> FindColumn(const std::vector<std::string>& names,
                                      const std::string& wanted) {
  const std::string lowered = ToLower(wanted);
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (ToLower(names[i]) == lowered) {
      return i;
    }
  }
  return std::nullopt;
}

struct SkippedRow {
  std::size_t line;
  std::string reason;
};

std::string TableNameFrom(const ImportRequest& request, bool* is_derived) {
  *is_derived = request.table.empty();
  if (!request.table.empty()) {
    return request.table;
  }
  return std::filesystem::path(request.file_path).stem().string();
}

// The header's column names, or the error.
std::variant<std::vector<std::string>, ExecutionError> ReadHeader(
    const ImportRequest& request, const std::vector<CsvLine>& lines) {
  if (lines.empty()) {
    return BindError(request.file_path + " has no header line");
  }
  if (!lines[0].fields) {
    return BindError("line " + std::to_string(lines[0].number) +
                     ": malformed header");
  }
  std::vector<std::string> names = *lines[0].fields;
  std::unordered_set<std::string> lowered_names;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!IsIdentifier(names[i])) {
      return BindError("invalid column name '" + names[i] +
                       "' in the header; names must match "
                       "[A-Za-z_][A-Za-z0-9_]*");
    }
    if (IsReservedWord(names[i])) {
      return BindError("invalid column name '" + names[i] +
                       "' in the header; it is a reserved word");
    }
    if (!lowered_names.insert(ToLower(names[i])).second) {
      return BindError("column '" + names[i] + "' appears twice in the header");
    }
  }
  return names;
}

std::optional<ExecutionError> CheckTableName(const std::string& name,
                                             bool is_derived,
                                             const ICatalog& catalog) {
  const bool is_reserved = IsReservedWord(name);
  if (!IsIdentifier(name) || (is_derived && is_reserved)) {
    if (is_derived) {
      return BindError("cannot derive a table name from '" + name +
                       "'; give one: .import <file> <table>");
    }
    return BindError("invalid table name '" + name + "'");
  }
  if (is_reserved) {
    return BindError("invalid table name '" + name +
                     "'; it is a reserved word");
  }
  if (ToLower(name) == "catalog") {
    return BindError("table name catalog is reserved");
  }
  if (catalog.FindTable(name)) {
    return BindError("table " + name + " already exists");
  }
  return std::nullopt;
}

}  // namespace

std::variant<ImportPlan, ExecutionError> InferImport(
    const ImportRequest& request, const std::vector<CsvLine>& lines,
    const ICatalog& catalog) {
  bool is_derived = false;
  const std::string table = TableNameFrom(request, &is_derived);
  auto header = ReadHeader(request, lines);
  if (auto* error = std::get_if<ExecutionError>(&header)) {
    return *error;
  }
  const std::vector<std::string>& names =
      std::get<std::vector<std::string>>(header);
  if (auto error = CheckTableName(table, is_derived, catalog)) {
    return *error;
  }

  // The declared types, by column position.
  std::map<std::size_t, DataType> declared;
  for (const auto& [column_name, type] : request.column_types) {
    std::optional<std::size_t> column = FindColumn(names, column_name);
    if (!column) {
      return BindError("--type names unknown column '" + column_name + "'");
    }
    if (!declared.emplace(*column, type).second) {
      return BindError("--type names column '" + column_name + "' twice");
    }
  }
  std::optional<std::size_t> explicit_key;
  if (!request.primary_key.empty()) {
    explicit_key = FindColumn(names, request.primary_key);
    if (!explicit_key) {
      return BindError("--pk names unknown column '" + request.primary_key +
                       "'");
    }
  }

  // Pass 1, in line order: rows with the wrong shape or a value that is not of
  // its declared type are bad.
  std::vector<SkippedRow> skipped;
  std::vector<const CsvLine*> good;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const CsvLine& line = lines[i];
    std::string reason;
    if (!line.fields) {
      reason = "malformed quoting";
    } else if (line.fields->size() != names.size()) {
      reason = "expected " + std::to_string(names.size()) + " fields, found " +
               std::to_string(line.fields->size());
    } else {
      for (const auto& [column, type] : declared) {
        if (!ConvertCell((*line.fields)[column], type)) {
          reason = names[column] + " '" + (*line.fields)[column] + "' is not " +
                   TypeName(type);
          break;
        }
      }
    }
    if (reason.empty()) {
      good.push_back(&line);
      continue;
    }
    if (!request.should_skip_bad_rows) {
      return BindError("line " + std::to_string(line.number) + ": " + reason);
    }
    skipped.push_back(SkippedRow{line.number, reason});
  }

  // Pass 2: types of the undeclared columns, from the good rows.
  std::vector<const std::vector<std::string>*> good_cells;
  good_cells.reserve(good.size());
  for (const CsvLine* line : good) {
    good_cells.push_back(&*line->fields);
  }
  std::vector<DataType> types(names.size(), DataType::kText);
  for (std::size_t column = 0; column < names.size(); ++column) {
    auto given = declared.find(column);
    types[column] =
        given != declared.end() ? given->second : InferType(good_cells, column);
  }

  // The primary key.
  std::size_t key_index = 0;
  if (explicit_key) {
    key_index = *explicit_key;
  } else {
    std::optional<std::size_t> id_column = FindColumn(names, "id");
    if (!id_column || types[*id_column] != DataType::kInt) {
      return BindError(
          "no primary key: name a column with --pk, or use an INT column "
          "named id");
    }
    key_index = *id_column;
  }

  // Pass 3: typed rows and the check that the key is unique.
  ImportPlan plan;
  plan.rows.reserve(good.size());
  std::map<std::string, std::size_t> key_lines;  // FormatKey to line
  for (const CsvLine* line : good) {
    Row row;
    row.reserve(names.size());
    for (std::size_t column = 0; column < names.size(); ++column) {
      row.push_back(*ConvertCell((*line->fields)[column], types[column]));
    }
    auto inserted = key_lines.emplace(FormatKey(row[key_index]), line->number);
    if (!inserted.second) {
      return BindError("primary key column " + names[key_index] +
                       " has duplicate value '" + FormatValue(row[key_index]) +
                       "' at lines " + std::to_string(inserted.first->second) +
                       " and " + std::to_string(line->number));
    }
    plan.rows.push_back(std::move(row));
  }

  plan.schema.name = table;
  for (std::size_t column = 0; column < names.size(); ++column) {
    plan.schema.columns.push_back(ColumnDef{names[column], types[column]});
  }
  plan.schema.primary_key_index = key_index;

  plan.skipped_count = skipped.size();
  for (std::size_t i = 0; i < skipped.size() && i < kMaxSkippedNotes; ++i) {
    plan.notes.push_back("skipped line " + std::to_string(skipped[i].line) +
                         ": " + skipped[i].reason);
  }
  if (skipped.size() > kMaxSkippedNotes) {
    plan.notes.push_back("... and " +
                         std::to_string(skipped.size() - kMaxSkippedNotes) +
                         " more skipped");
  }
  return plan;
}

}  // namespace strata::domain
