#include "strata/paged/paged_engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/paged/heap_access.h"
#include "strata/paged/mutation_planner.h"
#include "strata/paged/operators.h"
#include "strata/paged/row_stream.h"
#include "strata/paged/tuple_codec.h"

// Resource-aware (design document 2.13): the engine decides which pure function
// handles a plan node, hands it a reader over the Database, and writes the
// dirty pages out after a write statement succeeded. Every function below
// returns a value; a failure of a page call is mapped by ToExecutionError.

namespace strata::paged {

namespace {

using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExecutionResult;
using domain::PlanNode;
using domain::Row;
using domain::TableSchema;

// The context of a call that is not about one table: write-back and flush.
ErrorContext WholeDatabase() { return ErrorContext{ErrorSite::kRowAccess, ""}; }

bool IsQueryNode(const PlanNode& plan) {
  return std::holds_alternative<domain::ScanNode>(plan.node) ||
         std::holds_alternative<domain::FilterNode>(plan.node) ||
         std::holds_alternative<domain::JoinNode>(plan.node) ||
         std::holds_alternative<domain::AggregateNode>(plan.node) ||
         std::holds_alternative<domain::ProjectNode>(plan.node) ||
         std::holds_alternative<domain::SortNode>(plan.node) ||
         std::holds_alternative<domain::LimitNode>(plan.node);
}

bool IsWriteNode(const PlanNode& plan) {
  return std::holds_alternative<domain::InsertNode>(plan.node) ||
         std::holds_alternative<domain::UpdateNode>(plan.node) ||
         std::holds_alternative<domain::DeleteNode>(plan.node);
}

// Builds the operator tree and pulls it dry. Touches only locals and the
// reader, so several threads may run it at the same time. The tree is
// destroyed before the function returns.
ExecutionResult RunQuery(const PlanNode& plan, const ITableReader& reader,
                         const IIndexReader& index_reader) {
  BuildResult built = BuildOperatorTree(plan, reader, &index_reader);
  if (auto* error = std::get_if<ExecutionError>(&built)) {
    return std::move(*error);
  }
  // The error alternative was ruled out above.
  std::unique_ptr<IOperator> root =
      std::move(std::get<std::unique_ptr<IOperator>>(built));
  domain::ResultSet result_set;
  result_set.columns = root->Columns();
  while (true) {
    OperatorResult next = root->Next();
    if (auto* error = std::get_if<ExecutionError>(&next)) {
      return std::move(*error);
    }
    // The error alternative was ruled out above.
    std::optional<Row>& row = std::get<std::optional<Row>>(next);
    if (!row.has_value()) {
      break;
    }
    result_set.rows.push_back(std::move(*row));
  }
  return result_set;
}

// Writes every dirty page (no Sync). The statement is already applied in the
// pool when this fails, and the message says so (design document 2.15).
std::optional<ExecutionError> WriteBack(Database& database) {
  Status status = database.buffer().WriteBackAll();
  if (!status.has_value()) {
    return std::nullopt;
  }
  ExecutionError error = ToExecutionError(*status, WholeDatabase());
  error.kind = ErrorKind::kIo;
  error.message += "; statement was applied but not saved";
  return error;
}

// INSERT, UPDATE or DELETE: plan every change first (reads only), apply them,
// then write the pages out. A failed plan, a failed apply and a failed
// write-back each return before the next step. A statement whose change set is
// empty (no row selected, or every assignment equals the stored value) applied
// nothing, so it saves nothing: writing back would retry the dirty pages of an
// earlier failed statement and fail this one for a change it never made.
ExecutionResult RunWrite(const PlanNode& plan, Database& database) {
  DatabaseTableReader reader(database);
  const DatabaseIndexReader index_reader(database);
  PlanMutationResult planned =
      PlanMutation(plan, database.catalog(), reader, &index_reader);
  if (auto* error = std::get_if<ExecutionError>(&planned)) {
    return std::move(*error);
  }
  // The error alternative was ruled out above.
  const MutationPlan& mutation = std::get<MutationPlan>(planned);
  if (mutation.changes.empty()) {
    return domain::AffectedRows{mutation.affected};
  }
  if (auto error = ApplyChangeSet(database, mutation)) {
    return std::move(*error);
  }
  if (auto error = WriteBack(database)) {
    return std::move(*error);
  }
  return domain::AffectedRows{mutation.affected};
}

ExecutionResult RunCreateTable(const domain::CreateTableNode& create,
                               Database& database) {
  PageResult<TableSchema> created = database.CreateTable(create.schema);
  if (!created.ok()) {
    return ToExecutionError(
        created.error(),
        ErrorContext{ErrorSite::kCreateTable, create.schema.name});
  }
  if (auto error = WriteBack(database)) {
    return std::move(*error);
  }
  return domain::CommandComplete{"CREATE TABLE " + create.schema.name};
}

ExecutionResult RunAddColumn(const domain::AddColumnNode& add_column,
                             Database& database) {
  PageResult<TableSchema> altered =
      database.AddColumn(add_column.table, add_column.column);
  if (!altered.ok()) {
    return ToExecutionError(
        altered.error(), ErrorContext{ErrorSite::kAddColumn, add_column.table});
  }
  if (auto error = WriteBack(database)) {
    return std::move(*error);
  }
  return domain::CommandComplete{"ALTER TABLE " + add_column.table};
}

ExecutionResult RunDropTable(const domain::DropTableNode& drop,
                             Database& database) {
  if (Status status = database.DropTable(drop.table)) {
    return ToExecutionError(*status,
                            ErrorContext{ErrorSite::kOpenTable, drop.table});
  }
  if (auto error = WriteBack(database)) {
    return std::move(*error);
  }
  return domain::CommandComplete{"DROP TABLE " + drop.table};
}

// The first row that TupleCodec cannot encode (a row longer than
// kMaxTupleSize, which is the case that matters), as the mapped error.
std::optional<ExecutionError> CheckRowLengths(const TableSchema& schema,
                                              const std::vector<Row>& rows) {
  for (const Row& row : rows) {
    PageResult<std::vector<std::uint8_t>> encoded =
        TupleCodec::Encode(schema, row);
    if (!encoded.ok()) {
      return ToExecutionError(encoded.error(),
                              ErrorContext{ErrorSite::kRowAccess, schema.name});
    }
  }
  return std::nullopt;
}

// `.import` (design document 2.7): check every tuple length first, so an
// overlong row creates no table; then create the table and insert the rows.
// When a write fails, InsertRows has removed the rows already written and the
// table remains, empty; the message says so.
ExecutionResult RunImport(const domain::ImportTableNode& import,
                          Database& database) {
  const TableSchema& schema = import.schema;
  if (auto error = CheckRowLengths(schema, import.rows)) {
    return std::move(*error);
  }
  PageResult<TableSchema> created = database.CreateTable(schema);
  if (!created.ok()) {
    return ToExecutionError(created.error(),
                            ErrorContext{ErrorSite::kCreateTable, schema.name});
  }
  if (auto error = InsertRows(database, schema.name, import.rows)) {
    error->message += "; table " + schema.name + " was created";
    return std::move(*error);
  }
  if (auto error = WriteBack(database)) {
    return std::move(*error);
  }
  return domain::CommandComplete{"IMPORT " + schema.name + " (imported " +
                                 std::to_string(import.rows.size()) + " rows)"};
}

ExecutionResult Unsupported(const std::string& message) {
  return ExecutionError{ErrorKind::kUnsupported, message};
}

}  // namespace

PagedEngine::PagedEngine(Database database,
                         std::shared_ptr<void> directory_lock)
    : directory_lock_(std::move(directory_lock)),
      database_(std::move(database)),
      catalog_(&database_.catalog()) {}

const domain::ICatalog& PagedEngine::Catalog() const { return *catalog_; }

ExecutionResult PagedEngine::Execute(const PlanNode& plan) {
  if (IsQueryNode(plan)) {
    const DatabaseTableReader reader(database_);
    const DatabaseIndexReader index_reader(database_);
    return RunQuery(plan, reader, index_reader);
  }
  if (IsWriteNode(plan)) {
    return RunWrite(plan, database_);
  }
  if (const auto* create = std::get_if<domain::CreateTableNode>(&plan.node)) {
    return RunCreateTable(*create, database_);
  }
  if (const auto* import = std::get_if<domain::ImportTableNode>(&plan.node)) {
    return RunImport(*import, database_);
  }
  if (const auto* add_column = std::get_if<domain::AddColumnNode>(&plan.node)) {
    return RunAddColumn(*add_column, database_);
  }
  if (const auto* drop = std::get_if<domain::DropTableNode>(&plan.node)) {
    return RunDropTable(*drop, database_);
  }
  return Unsupported("unknown plan node");
}

namespace {

domain::IndexNodeDescription DescribeNode(const NodeSummary& node) {
  domain::IndexNodeDescription description;
  description.level = node.level;
  description.page_id = static_cast<std::uint32_t>(node.page_id);
  description.key_count = node.key_count;
  description.first_key = node.first_key;
  description.last_key = node.last_key;
  if (node.next_page_id != kInvalidPageId) {
    description.next_page_id = static_cast<std::uint32_t>(node.next_page_id);
  }
  return description;
}

}  // namespace

domain::IndexDescriptionResult PagedEngine::DescribeIndex(
    const std::string& table) {
  const ErrorContext context{ErrorSite::kOpenTable, table};
  PageResult<BTree*> opened = database_.Index(table);
  if (!opened.ok()) {
    return ToExecutionError(opened.error(), context);
  }
  const BTree* const tree = opened.value();
  if (tree == nullptr) {
    return ExecutionError{ErrorKind::kBind, "table " + table + " has no index"};
  }
  const std::optional<TableSchema> schema =
      database_.catalog().FindTable(table);
  if (!schema.has_value()) {
    return ToExecutionError(PageError{PageErrorCode::kNotFound, table},
                            context);
  }
  PageResult<TreeShape> shape = tree->Shape(domain::kMaxDescribedIndexNodes);
  if (!shape.ok()) {
    return ToExecutionError(shape.error(), context);
  }
  const Status check = database_.CheckIndex(table);

  domain::IndexDescription description;
  description.table = schema->name;
  description.column = schema->columns[schema->primary_key_index].name;
  description.height = shape.value().height;
  description.node_count = shape.value().node_count;
  description.key_count = shape.value().key_count;
  for (const NodeSummary& node : shape.value().nodes) {
    description.nodes.push_back(DescribeNode(node));
  }
  description.check = check.has_value() ? check->message : "ok";
  return description;
}

RowCountResult PagedEngine::RowCounts() {
  const DatabaseTableReader reader(database_);
  std::vector<RowCount> counts;
  for (const std::string& name : database_.catalog().TableNames()) {
    ScanResult scanned = reader.Scan(name);
    if (auto* error = std::get_if<ExecutionError>(&scanned)) {
      return std::move(*error);
    }
    // The error alternative was ruled out above.
    std::unique_ptr<IRowStream> stream =
        std::move(std::get<std::unique_ptr<IRowStream>>(scanned));
    std::size_t row_count = 0;
    while (true) {
      StreamResult next = stream->Next();
      if (auto* error = std::get_if<ExecutionError>(&next)) {
        return std::move(*error);
      }
      // The error alternative was ruled out above.
      if (!std::get<std::optional<StoredRow>>(next).has_value()) {
        break;
      }
      ++row_count;
    }
    counts.push_back(RowCount{name, row_count});
  }
  return counts;
}

std::optional<ExecutionError> PagedEngine::Flush() {
  Status status = database_.buffer().FlushAll();
  if (!status.has_value()) {
    return std::nullopt;
  }
  return ToExecutionError(*status, WholeDatabase());
}

std::optional<ExecutionError> PagedEngine::Close() {
  Status status = database_.Close();
  if (!status.has_value()) {
    return std::nullopt;
  }
  return ToExecutionError(*status, WholeDatabase());
}

}  // namespace strata::paged
