#include "strata/application.h"

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/binder.h"

namespace strata::application {

namespace {

// Binds statement against the engine's catalog and runs the plan. Takes no
// lock: the caller holds the database-level lock for the whole call.
domain::ExecutionResult BindAndExecute(domain::IEngine& engine,
                                       const domain::Statement& statement) {
  domain::BindResult bound = domain::Bind(statement, engine.Catalog());
  if (auto* error = std::get_if<domain::ExecutionError>(&bound)) {
    return std::move(*error);
  }
  return engine.Execute(*std::get<domain::PlanPtr>(bound));
}

}  // namespace

QueryService::QueryService(std::unique_ptr<domain::IEngine> engine,
                           std::unique_ptr<domain::ICsvSource> csv_source,
                           domain::IIndexInspector* index_inspector)
    : engine_(std::move(engine)),
      csv_source_(std::move(csv_source)),
      index_inspector_(index_inspector) {}

domain::ExecutionResult QueryService::Execute(
    const domain::Statement& statement) {
  // The statement kind picks the lock mode before the lock is taken.
  if (std::holds_alternative<domain::SelectStatement>(statement)) {
    std::shared_lock<std::shared_mutex> read_lock(database_mutex_);
    return BindAndExecute(*engine_, statement);
  }
  std::unique_lock<std::shared_mutex> write_lock(database_mutex_);
  return BindAndExecute(*engine_, statement);
}

std::vector<domain::TableSchema> QueryService::DescribeCatalog() const {
  std::shared_lock<std::shared_mutex> read_lock(database_mutex_);
  const domain::ICatalog& catalog = engine_->Catalog();
  std::vector<domain::TableSchema> schemas;
  for (const std::string& name : catalog.TableNames()) {
    std::optional<domain::TableSchema> schema = catalog.FindTable(name);
    if (schema) {
      schemas.push_back(std::move(*schema));
    }
  }
  return schemas;
}

domain::IndexDescriptionResult QueryService::DescribeIndex(
    const std::string& table) const {
  std::shared_lock<std::shared_mutex> read_lock(database_mutex_);
  if (index_inspector_ == nullptr) {
    return domain::ExecutionError{domain::ErrorKind::kUnsupported,
                                  "the flatfile engine has no indexes"};
  }
  return index_inspector_->DescribeIndex(table);
}

domain::ExecutionResult QueryService::ImportCsv(
    const domain::ImportRequest& request) {
  // Exclusive for the whole call: the file read and the inference run under the
  // lock because the inference reads the catalog that the import changes.
  std::unique_lock<std::shared_mutex> write_lock(database_mutex_);
  if (!csv_source_) {
    return domain::ExecutionError{domain::ErrorKind::kUnsupported,
                                  "import is not available"};
  }
  auto read = csv_source_->Read(request.file_path);
  if (auto* reason = std::get_if<std::string>(&read)) {
    return domain::ExecutionError{domain::ErrorKind::kIo, std::move(*reason)};
  }
  auto inferred =
      domain::InferImport(request, std::get<std::vector<domain::CsvLine>>(read),
                          engine_->Catalog());
  if (auto* error = std::get_if<domain::ExecutionError>(&inferred)) {
    return std::move(*error);
  }
  domain::ImportPlan& plan = std::get<domain::ImportPlan>(inferred);

  const std::string table = plan.schema.name;
  const std::size_t row_count = plan.rows.size();
  domain::PlanNode node;
  node.node =
      domain::ImportTableNode{std::move(plan.schema), std::move(plan.rows)};
  domain::ExecutionResult result = engine_->Execute(node);
  if (!std::holds_alternative<domain::CommandComplete>(result)) {
    return result;
  }

  std::string tag =
      "IMPORT " + table + " (imported " + std::to_string(row_count) + " rows";
  if (plan.skipped_count > 0) {
    tag += ", skipped " + std::to_string(plan.skipped_count);
  }
  tag += ")";
  return domain::CommandComplete{std::move(tag), std::move(plan.notes)};
}

}  // namespace strata::application
