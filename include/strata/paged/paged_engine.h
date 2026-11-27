#ifndef STRATA_PAGED_PAGED_ENGINE_H_
#define STRATA_PAGED_PAGED_ENGINE_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/index_inspector.h"
#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/database.h"

// The second implementation of domain::IEngine (design document 2.13): it
// runs the plans the Binder produces on the paged storage layer. Resource-
// aware; it dispatches on the plan node and leaves the logic to the pure row
// layer (operators, mutation planner) and the page access to heap_access.h.
//
// Ownership. The engine owns the Database and one share of the directory lock.
// The members are declared lock first, Database second, so on destruction the
// Database writes out the dirty pages before the lock is released. The
// operator tree of a query is built and destroyed inside Execute; the plan is
// borrowed for the call.
//
// Concurrency. The caller (QueryService) serializes statements with the
// DB-level lock: Execute of a query node may run on several threads at the
// same time and modifies no member; every other call, and Execute of any other
// node, needs the exclusive lock.
//
// Durability (design document 2.15). After a write node (INSERT, UPDATE,
// DELETE, CREATE TABLE, ALTER TABLE ADD COLUMN, DROP TABLE, import) was
// applied, Execute calls BufferManager::WriteBackAll before it returns, so a
// completed statement is in the file. A query and a failed statement never call
// it; neither does an ADD COLUMN or DROP TABLE whose old pages could not be
// released (the statement is applied and the error says its pages were not
// reclaimed).

namespace strata::paged {

struct RowCount {
  std::string name;  // as the catalog spells it
  std::size_t count;
};

// The row count of every table in catalog order, or the error of the first
// table that cannot be read.
using RowCountResult =
    std::variant<std::vector<RowCount>, domain::ExecutionError>;

// IEngine runs plans; IIndexInspector (design document 2.2, 3.6) describes an
// index for `.dump index`. QueryService borrows the engine as the inspector.
class PagedEngine : public domain::IEngine, public domain::IIndexInspector {
 public:
  // Takes ownership of database and shares ownership of directory_lock, which
  // keeps the directory locked for the life of the engine (it may be null in a
  // test). Not copyable and not movable: QueryService and Bootstrap hold it
  // behind a pointer.
  PagedEngine(Database database, std::shared_ptr<void> directory_lock);
  PagedEngine(const PagedEngine&) = delete;
  PagedEngine& operator=(const PagedEngine&) = delete;

  // Database::catalog(). Valid for the life of the engine.
  const domain::ICatalog& Catalog() const override;

  // Query nodes yield a ResultSet (the columns of the root operator, the rows
  // in pull order). InsertNode, UpdateNode and DeleteNode yield AffectedRows
  // and change no page when a check fails. CreateTableNode yields
  // CommandComplete "CREATE TABLE <name>"; ImportTableNode yields
  // CommandComplete "IMPORT <name> (imported <n> rows)". AddColumnNode yields
  // CommandComplete "ALTER TABLE <name>" and DropTableNode yields
  // CommandComplete "DROP TABLE <name>". A write that was applied but could not
  // be written out yields kIo
  // "<message>; statement was applied but not saved"; the dirty pages stay in
  // the pool and the next successful write, Flush or Close writes them out.
  domain::ExecutionResult Execute(const domain::PlanNode& plan) override;

  // The shape of the index of the table: height, node and key counts, the
  // first kMaxDescribedIndexNodes nodes in breadth-first order, and the result
  // of Database::CheckIndex as text. kBind "unknown table: <table>" or "table
  // <table> has no index" (a primary key that is not an INT); kIo "corrupt:
  // ..." when the index cannot be opened or its root cannot be read; a
  // violation that the check finds is in `check`, not an error. Reads the
  // pages of the table and its index; needs no writer, so it may run beside
  // other readers under the shared lock.
  domain::IndexDescriptionResult DescribeIndex(
      const std::string& table) override;

  // Scans every table in catalog order. Opens each table's heap on the way, so
  // a corrupt page makes the whole call fail. Needs no writer.
  RowCountResult RowCounts();

  // Writes the dirty pages and syncs the file (BufferManager::FlushAll).
  std::optional<domain::ExecutionError> Flush();

  // Database::Close: writes the dirty pages, syncs and closes. After a
  // successful Close every call that touches a table yields kIo "database is
  // closed".
  std::optional<domain::ExecutionError> Close();

  // The storage layer, for tests and measurements (buffer().Stats(),
  // PinnedFrameCount()). Borrowed from the engine.
  Database& database() { return database_; }

 private:
  // Declared in this order: see the ownership paragraph above.
  std::shared_ptr<void> directory_lock_;
  Database database_;
  // Points at the Catalog inside database_; Database::catalog() is not const.
  const domain::ICatalog* catalog_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_PAGED_ENGINE_H_
