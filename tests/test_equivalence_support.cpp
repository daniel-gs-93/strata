#include "test_equivalence_support.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/csv_file_source.h"
#include "strata/domain/ast.h"
#include "strata/domain/binder.h"
#include "strata/domain/engine.h"
#include "strata/domain/plan.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/sql_parser.h"
#include "test_harness.h"

namespace strata::testing {

namespace {

using domain::ExecutionError;
using domain::ExecutionResult;
using domain::Row;
using domain::TableSchema;
using domain::Value;

constexpr std::size_t kDescribedRows = 6;

// ----------------------------------------------------------------------------
// Text for the failure messages.
// ----------------------------------------------------------------------------

std::string QuoteText(const std::string& text) {
  std::string quoted = "'";
  for (char character : text) {
    if (character == '\n') {
      quoted += "\\n";
    } else if (character == '\r') {
      quoted += "\\r";
    } else {
      quoted += character;
    }
  }
  return quoted + "'";
}

std::string DescribeValue(const Value& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    // Long text is shortened: the failure message must stay readable.
    if (text->size() > 40) {
      return QuoteText(text->substr(0, 40)) + "...";
    }
    return QuoteText(*text);
  }
  return domain::FormatValue(value);
}

std::string DescribeRow(const Row& row) {
  std::string text = "(";
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += DescribeValue(row[i]);
  }
  return text + ")";
}

const char* TypeName(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      return "TEXT";
  }
  return "?";
}

std::string DescribeColumns(const std::vector<domain::ColumnDef>& columns) {
  std::string text = "[";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += columns[i].name + " " + TypeName(columns[i].type);
    if (columns[i].default_value.has_value()) {
      text += " DEFAULT " + DescribeValue(*columns[i].default_value);
    }
  }
  return text + "]";
}

std::string DescribeSchema(const TableSchema& schema) {
  std::ostringstream text;
  text << schema.name << DescribeColumns(schema.columns)
       << " pk=" << schema.primary_key_index << " fk=";
  for (const domain::ForeignKey& foreign_key : schema.foreign_keys) {
    text << foreign_key.column_index << "->" << foreign_key.ref_table << "."
         << foreign_key.ref_column_index << ";";
  }
  text << " unique=";
  for (std::size_t index : schema.unique_indexes) {
    text << index << ";";
  }
  text << " auto=";
  if (schema.auto_increment_index.has_value()) {
    text << *schema.auto_increment_index;
  } else {
    text << "-";
  }
  return text.str();
}

// ----------------------------------------------------------------------------
// Comparing rows.
// ----------------------------------------------------------------------------

bool RowLess(const Row& left, const Row& right) { return left < right; }

// The first row of the multiset difference between two sorted vectors, as a
// description, or no value when they are equal.
std::optional<std::string> FirstMultisetDifference(
    const std::vector<Row>& sorted_flatfile,
    const std::vector<Row>& sorted_paged) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < sorted_flatfile.size() && j < sorted_paged.size()) {
    if (sorted_flatfile[i] == sorted_paged[j]) {
      ++i;
      ++j;
    } else if (RowLess(sorted_flatfile[i], sorted_paged[j])) {
      return "row only in the flatfile result: " +
             DescribeRow(sorted_flatfile[i]);
    } else {
      return "row only in the paged result: " + DescribeRow(sorted_paged[j]);
    }
  }
  if (i < sorted_flatfile.size()) {
    return "row only in the flatfile result: " +
           DescribeRow(sorted_flatfile[i]);
  }
  if (j < sorted_paged.size()) {
    return "row only in the paged result: " + DescribeRow(sorted_paged[j]);
  }
  return std::nullopt;
}

bool HasSameSortKey(const Row& left, const Row& right,
                    const std::vector<std::size_t>& sort_columns) {
  for (std::size_t column : sort_columns) {
    if (!(left[column] == right[column])) {
      return false;
    }
  }
  return true;
}

// The rows of [begin, end) of both results compared as a multiset.
std::optional<std::string> CompareTieRun(const std::vector<Row>& flatfile,
                                         const std::vector<Row>& paged,
                                         std::size_t begin, std::size_t end) {
  std::vector<Row> flatfile_run(flatfile.begin() + begin,
                                flatfile.begin() + end);
  std::vector<Row> paged_run(paged.begin() + begin, paged.begin() + end);
  std::optional<std::string> difference = FirstMultisetDifference(
      SortedRows(std::move(flatfile_run)), SortedRows(std::move(paged_run)));
  if (!difference.has_value()) {
    return std::nullopt;
  }
  return "rows with equal sort keys at positions " + std::to_string(begin) +
         " to " + std::to_string(end - 1) + " differ: " + *difference;
}

// With ORDER BY: the sequences of sort keys are the same, and the rows that
// share a key are the same multiset (2.8).
std::optional<std::string> CompareOrderedRows(
    const std::vector<Row>& flatfile, const std::vector<Row>& paged,
    const std::vector<std::size_t>& sort_columns) {
  const std::size_t row_count = flatfile.size();
  std::size_t begin = 0;
  while (begin < row_count) {
    std::size_t end = begin + 1;
    while (end < row_count &&
           HasSameSortKey(flatfile[begin], flatfile[end], sort_columns)) {
      ++end;
    }
    for (std::size_t i = begin; i < end; ++i) {
      if (!HasSameSortKey(flatfile[begin], paged[i], sort_columns)) {
        return "the sort keys differ at position " + std::to_string(i) +
               ": flatfile " + DescribeRow(flatfile[i]) + ", paged " +
               DescribeRow(paged[i]);
      }
    }
    if (end < row_count &&
        HasSameSortKey(flatfile[begin], paged[end], sort_columns)) {
      return "the sort keys differ at position " + std::to_string(end) +
             ": flatfile " + DescribeRow(flatfile[end]) + ", paged " +
             DescribeRow(paged[end]);
    }
    if (auto difference = CompareTieRun(flatfile, paged, begin, end)) {
      return difference;
    }
    begin = end;
  }
  return std::nullopt;
}

std::string DescribeRows(const std::vector<Row>& rows) {
  std::string text;
  for (std::size_t i = 0; i < rows.size() && i < kDescribedRows; ++i) {
    text += " " + DescribeRow(rows[i]);
  }
  if (rows.size() > kDescribedRows) {
    text += " ...";
  }
  return text;
}

std::optional<std::string> CompareResultSets(
    const domain::ResultSet& flatfile, const domain::ResultSet& paged,
    const std::vector<std::size_t>& sort_columns) {
  if (flatfile.columns.size() != paged.columns.size()) {
    return "the column counts differ: " + DescribeColumns(flatfile.columns) +
           " vs " + DescribeColumns(paged.columns);
  }
  for (std::size_t i = 0; i < flatfile.columns.size(); ++i) {
    const domain::ColumnDef& left = flatfile.columns[i];
    const domain::ColumnDef& right = paged.columns[i];
    if (left.name != right.name || left.type != right.type ||
        left.default_value != right.default_value) {
      return "column " + std::to_string(i) +
             " differs: " + DescribeColumns({left}) + " vs " +
             DescribeColumns({right});
    }
  }
  if (flatfile.rows.size() != paged.rows.size()) {
    return "the row counts differ: " + std::to_string(flatfile.rows.size()) +
           " vs " + std::to_string(paged.rows.size());
  }
  if (sort_columns.empty()) {
    return FirstMultisetDifference(SortedRows(flatfile.rows),
                                   SortedRows(paged.rows));
  }
  return CompareOrderedRows(flatfile.rows, paged.rows, sort_columns);
}

// ----------------------------------------------------------------------------
// Binding a SELECT to find its sort keys.
// ----------------------------------------------------------------------------

// An ICatalog over a list of schemas, so that the runner can bind a statement
// without reaching into an engine.
class ListCatalog : public domain::ICatalog {
 public:
  explicit ListCatalog(std::vector<TableSchema> schemas)
      : schemas_(std::move(schemas)) {}

  std::optional<TableSchema> FindTable(const std::string& name) const override {
    const std::string lowered = domain::ToLower(name);
    for (const TableSchema& schema : schemas_) {
      if (domain::ToLower(schema.name) == lowered) {
        return schema;
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> TableNames() const override {
    std::vector<std::string> names;
    for (const TableSchema& schema : schemas_) {
      names.push_back(schema.name);
    }
    return names;
  }

 private:
  std::vector<TableSchema> schemas_;
};

// What the plan of a SELECT says about its order.
struct OrderInfo {
  bool is_bound = false;  // false: the Binder rejected the statement
  std::vector<std::size_t> sort_columns;
};

OrderInfo FindOrder(const domain::Statement& statement,
                    const domain::ICatalog& catalog) {
  OrderInfo info;
  domain::BindResult bound = domain::Bind(statement, catalog);
  auto* plan = std::get_if<domain::PlanPtr>(&bound);
  if (plan == nullptr) {
    return info;
  }
  info.is_bound = true;
  const domain::PlanNode* node = plan->get();
  if (const auto* limit = std::get_if<domain::LimitNode>(&node->node)) {
    node = limit->child.get();
  }
  if (const auto* sort = std::get_if<domain::SortNode>(&node->node)) {
    for (const domain::SortKey& key : sort->keys) {
      info.sort_columns.push_back(key.column_index);
    }
  }
  return info;
}

// Whether the cut at `position` falls between two rows with the same sort key.
bool SplitsTie(const std::vector<Row>& rows,
               const std::vector<std::size_t>& sort_columns,
               std::size_t position) {
  if (position == 0 || position >= rows.size()) {
    return false;
  }
  return HasSameSortKey(rows[position - 1], rows[position], sort_columns);
}

// 2.8: LIMIT and OFFSET are compared only when the sort key decides which rows
// are cut. all_rows is the result of the statement without LIMIT and OFFSET.
bool IsCutDeterministic(const std::vector<Row>& all_rows,
                        const std::vector<std::size_t>& sort_columns,
                        const domain::SelectStatement& select) {
  if (select.limit.has_value() && *select.limit == 0) {
    return true;
  }
  const std::size_t begin = std::min<std::size_t>(
      static_cast<std::size_t>(select.offset), all_rows.size());
  std::size_t end = all_rows.size();
  if (select.limit.has_value()) {
    end = std::min<std::size_t>(begin + static_cast<std::size_t>(*select.limit),
                                all_rows.size());
  }
  return !SplitsTie(all_rows, sort_columns, begin) &&
         !SplitsTie(all_rows, sort_columns, end);
}

// ----------------------------------------------------------------------------
// Table contents.
// ----------------------------------------------------------------------------

using Snapshot = std::variant<TableContents, std::string>;

// SELECT * of every table of the catalog, sorted. The failure is the
// description of the first statement that did not return a result set.
Snapshot TakeSnapshot(application::QueryService& service) {
  TableContents contents;
  for (const TableSchema& schema : service.DescribeCatalog()) {
    domain::SelectStatement select;
    select.items.push_back(domain::SelectItem{true, nullptr, "", "*"});
    select.from.table = schema.name;
    ExecutionResult result =
        service.Execute(domain::Statement(std::move(select)));
    auto* rows = std::get_if<domain::ResultSet>(&result);
    if (rows == nullptr) {
      return "SELECT * FROM " + schema.name + " returned " +
             DescribeResult(result);
    }
    contents[domain::ToLower(schema.name)] = SortedRows(std::move(rows->rows));
  }
  return contents;
}

std::optional<std::string> FindContentsDifference(const TableContents& flatfile,
                                                  const TableContents& paged) {
  for (const auto& [name, flatfile_rows] : flatfile) {
    auto found = paged.find(name);
    if (found == paged.end()) {
      return "table " + name + " exists only in the flatfile engine";
    }
    if (flatfile_rows.size() != found->second.size()) {
      return "table " + name + " has " + std::to_string(flatfile_rows.size()) +
             " rows in the flatfile engine and " +
             std::to_string(found->second.size()) + " in the paged engine";
    }
    if (auto difference =
            FirstMultisetDifference(flatfile_rows, found->second)) {
      return "table " + name + ": " + *difference;
    }
  }
  for (const auto& entry : paged) {
    if (!flatfile.contains(entry.first)) {
      return "table " + entry.first + " exists only in the paged engine";
    }
  }
  return std::nullopt;
}

// The name of the first table whose rows differ between two snapshots.
std::optional<std::string> FindChangedTable(const TableContents& before,
                                            const TableContents& after) {
  for (const auto& [name, rows] : after) {
    auto found = before.find(name);
    if (found == before.end() || !(found->second == rows)) {
      return name;
    }
  }
  for (const auto& entry : before) {
    if (!after.contains(entry.first)) {
      return entry.first;
    }
  }
  return std::nullopt;
}

}  // namespace

// ----------------------------------------------------------------------------
// Public comparison functions.
// ----------------------------------------------------------------------------

std::vector<Row> SortedRows(std::vector<Row> rows) {
  std::ranges::sort(rows, RowLess);
  return rows;
}

std::string DescribeResult(const ExecutionResult& result) {
  if (const auto* rows = std::get_if<domain::ResultSet>(&result)) {
    return "result set " + DescribeColumns(rows->columns) + " with " +
           std::to_string(rows->rows.size()) +
           " rows:" + DescribeRows(rows->rows);
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&result)) {
    return "affected=" + std::to_string(affected->count);
  }
  if (const auto* complete = std::get_if<domain::CommandComplete>(&result)) {
    std::string text = "complete " + complete->tag;
    for (const std::string& note : complete->notes) {
      text += " | " + note;
    }
    return text;
  }
  const auto& error = std::get<ExecutionError>(result);
  return std::string("error ") + domain::ErrorKindName(error.kind) + ": " +
         error.message;
}

std::optional<std::string> FindResultDifference(
    const ExecutionResult& flatfile, const ExecutionResult& paged,
    const std::vector<std::size_t>& sort_columns) {
  if (flatfile.index() != paged.index()) {
    return "the result kinds differ";
  }
  if (const auto* rows = std::get_if<domain::ResultSet>(&flatfile)) {
    return CompareResultSets(*rows, std::get<domain::ResultSet>(paged),
                             sort_columns);
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&flatfile)) {
    if (affected->count == std::get<domain::AffectedRows>(paged).count) {
      return std::nullopt;
    }
    return "the affected row counts differ";
  }
  if (const auto* complete = std::get_if<domain::CommandComplete>(&flatfile)) {
    const auto& other = std::get<domain::CommandComplete>(paged);
    if (complete->tag != other.tag) {
      return "the command tags differ";
    }
    if (complete->notes != other.notes) {
      return "the command notes differ";
    }
    return std::nullopt;
  }
  const auto& error = std::get<ExecutionError>(flatfile);
  const auto& other = std::get<ExecutionError>(paged);
  if (error.kind != other.kind) {
    return "the error kinds differ";
  }
  if (error.message != other.message) {
    return "the error messages differ";
  }
  return std::nullopt;
}

std::optional<std::string> FindCatalogDifference(
    const std::vector<TableSchema>& flatfile,
    const std::vector<TableSchema>& paged) {
  const std::size_t shared = std::min(flatfile.size(), paged.size());
  for (std::size_t i = 0; i < shared; ++i) {
    const std::string left = DescribeSchema(flatfile[i]);
    const std::string right = DescribeSchema(paged[i]);
    if (left != right) {
      return "catalog entry " + std::to_string(i) + " differs: flatfile " +
             left + ", paged " + right;
    }
  }
  if (flatfile.size() != paged.size()) {
    return "the catalogs list " + std::to_string(flatfile.size()) +
           " tables in the flatfile engine and " +
           std::to_string(paged.size()) + " in the paged engine";
  }
  return std::nullopt;
}

// ----------------------------------------------------------------------------
// Fixtures.
// ----------------------------------------------------------------------------

namespace {

PagedFixture WrapEngine(paged::Database database) {
  auto engine =
      std::make_unique<paged::PagedEngine>(std::move(database), nullptr);
  PagedFixture fixture;
  fixture.engine = engine.get();
  // The engine is also the index inspector, as Bootstrap passes it.
  fixture.service = std::make_unique<application::QueryService>(
      std::move(engine), std::make_unique<infrastructure::FileCsvSource>(),
      fixture.engine);
  return fixture;
}

}  // namespace

PagedFixture MakeMemoryPagedFixture(std::size_t frames,
                                    paged::ReplacementPolicyKind policy) {
  paged::DatabaseOptions options;
  options.pool_frames = frames;
  options.policy = policy;
  paged::PageResult<paged::Database> database = paged::Database::OpenWithStore(
      std::make_unique<paged::MemoryPageStore>(), options);
  if (!database.ok()) {
    ReportFailure(__FILE__, __LINE__,
                  "cannot open a memory database: " + database.error().message);
    return PagedFixture();
  }
  return WrapEngine(std::move(database).value());
}

PagedFixture MakeFilePagedFixture(const std::filesystem::path& directory,
                                  std::size_t frames,
                                  paged::ReplacementPolicyKind policy) {
  paged::DatabaseOptions options;
  options.pool_frames = frames;
  options.policy = policy;
  paged::PageResult<paged::Database> database =
      paged::Database::Open(directory, options);
  if (!database.ok()) {
    ReportFailure(__FILE__, __LINE__,
                  "cannot open " + directory.string() +
                      "/paged.db: " + database.error().message);
    return PagedFixture();
  }
  return WrapEngine(std::move(database).value());
}

// ----------------------------------------------------------------------------
// EquivalenceRunner.
// ----------------------------------------------------------------------------

EquivalenceRunner::EquivalenceRunner(application::QueryService& flatfile,
                                     application::QueryService& paged,
                                     std::string label)
    : flatfile_(flatfile), paged_(paged), label_(std::move(label)) {
  Snapshot flatfile_snapshot = TakeSnapshot(flatfile_);
  Snapshot paged_snapshot = TakeSnapshot(paged_);
  if (auto* failure = std::get_if<std::string>(&flatfile_snapshot)) {
    snapshot_failure_ = "flatfile engine: " + *failure;
  } else {
    flatfile_tables_ = std::move(std::get<TableContents>(flatfile_snapshot));
  }
  if (auto* failure = std::get_if<std::string>(&paged_snapshot)) {
    snapshot_failure_ = "paged engine: " + *failure;
  } else {
    paged_tables_ = std::move(std::get<TableContents>(paged_snapshot));
  }
}

std::string EquivalenceRunner::Describe(const std::string& statement_text,
                                        const std::string& difference) const {
  return "[" + label_ + "] statement " + std::to_string(statement_count_) +
         ": " + statement_text + "\n    " + difference;
}

std::optional<std::string> EquivalenceRunner::Run(const std::string& sql) {
  ++statement_count_;
  if (snapshot_failure_.has_value()) {
    return Describe(sql, "the first snapshot failed: " + *snapshot_failure_);
  }
  adapter::ParseResult parsed = adapter::ParseStatement(sql);
  if (auto* parse_error = std::get_if<adapter::ParseError>(&parsed)) {
    return Describe(
        sql, "the statement does not parse: " + parse_error->description);
  }
  const domain::Statement& statement = std::get<domain::Statement>(parsed);
  const auto* select = std::get_if<domain::SelectStatement>(&statement);

  OrderInfo order;
  if (select != nullptr) {
    order = FindOrder(statement, ListCatalog(flatfile_.DescribeCatalog()));
  }
  const bool has_cut =
      select != nullptr && (select->limit.has_value() || select->offset > 0);
  if (has_cut && order.is_bound) {
    if (order.sort_columns.empty()) {
      return Describe(sql,
                      "LIMIT or OFFSET without ORDER BY: which rows are cut "
                      "is undefined (2.8), so the statement is not "
                      "comparable");
    }
    // The statement without LIMIT and OFFSET shows where the cut falls.
    adapter::ParseResult unlimited_parsed = adapter::ParseStatement(sql);
    domain::Statement unlimited =
        std::move(std::get<domain::Statement>(unlimited_parsed));
    auto& unlimited_select = std::get<domain::SelectStatement>(unlimited);
    unlimited_select.limit = std::nullopt;
    unlimited_select.offset = 0;
    ExecutionResult all_rows = flatfile_.Execute(unlimited);
    const auto* all = std::get_if<domain::ResultSet>(&all_rows);
    if (all != nullptr &&
        !IsCutDeterministic(all->rows, order.sort_columns, *select)) {
      return Describe(sql,
                      "LIMIT or OFFSET cuts between rows with the same sort "
                      "key (2.8), so the statement is not comparable");
    }
  }

  last_flatfile_result_ = flatfile_.Execute(statement);
  last_paged_result_ = paged_.Execute(statement);
  return Compare(sql, select == nullptr, order.sort_columns);
}

std::optional<std::string> EquivalenceRunner::RunImport(
    const domain::ImportRequest& request) {
  ++statement_count_;
  const std::string text = ".import " + request.file_path + " " + request.table;
  if (snapshot_failure_.has_value()) {
    return Describe(text, "the first snapshot failed: " + *snapshot_failure_);
  }
  last_flatfile_result_ = flatfile_.ImportCsv(request);
  last_paged_result_ = paged_.ImportCsv(request);
  return Compare(text, true, {});
}

std::optional<std::string> EquivalenceRunner::Compare(
    const std::string& statement_text, bool is_write,
    const std::vector<std::size_t>& sort_keys) {
  if (auto difference = FindResultDifference(last_flatfile_result_,
                                             last_paged_result_, sort_keys)) {
    return Describe(
        statement_text,
        *difference +
            "\n    flatfile: " + DescribeResult(last_flatfile_result_) +
            "\n    paged:    " + DescribeResult(last_paged_result_));
  }
  if (!is_write) {
    return std::nullopt;
  }
  const bool is_rejected =
      std::holds_alternative<ExecutionError>(last_flatfile_result_);
  if (auto difference = CompareState(is_rejected)) {
    return Describe(
        statement_text,
        *difference +
            "\n    flatfile: " + DescribeResult(last_flatfile_result_) +
            "\n    paged:    " + DescribeResult(last_paged_result_));
  }
  return std::nullopt;
}

std::optional<std::string> EquivalenceRunner::CompareState(bool is_rejected) {
  if (auto difference = FindCatalogDifference(flatfile_.DescribeCatalog(),
                                              paged_.DescribeCatalog())) {
    return "after the statement: " + *difference;
  }
  Snapshot flatfile_snapshot = TakeSnapshot(flatfile_);
  Snapshot paged_snapshot = TakeSnapshot(paged_);
  if (auto* failure = std::get_if<std::string>(&flatfile_snapshot)) {
    return "after the statement, flatfile engine: " + *failure;
  }
  if (auto* failure = std::get_if<std::string>(&paged_snapshot)) {
    return "after the statement, paged engine: " + *failure;
  }
  TableContents& flatfile_after = std::get<TableContents>(flatfile_snapshot);
  TableContents& paged_after = std::get<TableContents>(paged_snapshot);
  if (is_rejected) {
    if (auto table = FindChangedTable(flatfile_tables_, flatfile_after)) {
      return "a rejected statement changed table " + *table +
             " of the flatfile engine";
    }
    if (auto table = FindChangedTable(paged_tables_, paged_after)) {
      return "a rejected statement changed table " + *table +
             " of the paged engine";
    }
  }
  flatfile_tables_ = std::move(flatfile_after);
  paged_tables_ = std::move(paged_after);
  if (auto difference =
          FindContentsDifference(flatfile_tables_, paged_tables_)) {
    return "after the statement, " + *difference;
  }
  return std::nullopt;
}

bool ReportDifference(const char* file, int line,
                      const std::optional<std::string>& difference) {
  if (!difference.has_value()) {
    return true;
  }
  ReportFailure(file, line, *difference);
  return false;
}

}  // namespace strata::testing
