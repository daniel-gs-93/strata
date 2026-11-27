#ifndef STRATA_PAGED_HEAP_ACCESS_H_
#define STRATA_PAGED_HEAP_ACCESS_H_

#include <optional>
#include <string>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/database.h"
#include "strata/paged/index_reader.h"
#include "strata/paged/key_range.h"
#include "strata/paged/mutation_planner.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/row_stream.h"
#include "strata/paged/table_heap.h"

// The resource-aware half of paged execution (design document
// 2.13): the code that reads and writes pages on behalf of the pure row layer.
// It implements IRowStream and ITableReader over TableHeap and Database,
// applies a MutationPlan to the heaps with an undo from the before-images, and
// maps the storage layer's PageError to the engine's ExecutionError.
//
// Ownership: HeapRowStream borrows a TableHeap, DatabaseTableReader borrows a
// Database; both must outlive what they hand out. Nothing here keeps a
// PageGuard between two calls: every pin is released inside the TableHeap
// operation that took it.

namespace strata::paged {

// Which call produced a PageError. The same PageErrorCode means different
// things at different call sites (design document 2.13).
enum class ErrorSite {
  // Database::Table: kNotFound is an unknown table. A kNotFound for a table
  // that the catalog has comes from its page chain; OpenTableError maps that
  // one as an internal error.
  kOpenTable,
  kCreateTable,  // Database::CreateTable: kInvalidArgument, kAlreadyExists
  // Database::AddColumn: kInvalidArgument (the catalog row or the default),
  // kNotFound (an unknown table)
  kAddColumn,
  kRowAccess  // a TableHeap read, insert, update or delete
};

struct ErrorContext {
  ErrorSite site;
  // The table the call concerned, as the statement or the schema spells it.
  // Used in the messages "unknown table: <table>", "table <table> already
  // exists" and "... in <table>".
  std::string table;
};

// Maps error to the ExecutionError of design document 2.13. Pure.
domain::ExecutionError ToExecutionError(const PageError& error,
                                        const ErrorContext& context);

// The rows of one table in page chain order, then slot order. Wraps HeapScan.
// Borrows the heap, which must outlive the stream. Not thread-safe.
class HeapRowStream : public IRowStream {
 public:
  explicit HeapRowStream(const TableHeap* heap);

  // A page error is mapped with ErrorSite::kRowAccess. A HeapScan is over
  // after an error, so every later call returns the end of the rows.
  StreamResult Next() override;
  std::vector<domain::ColumnDef> Columns() const override;

 private:
  const TableHeap* heap_;
  HeapScan scan_;
};

// ITableReader over a Database. Borrows the database for its whole life.
class DatabaseTableReader : public ITableReader {
 public:
  explicit DatabaseTableReader(Database& database) : database_(database) {}

  // Opens the table's heap through Database::Table (on first use it walks the
  // page chain) and returns a HeapRowStream over it. kBind "unknown table:
  // <table>" when the catalog has no such table. Several threads may call it
  // at the same time.
  ScanResult Scan(const std::string& table) const override;

 private:
  Database& database_;
};

// IIndexReader over a Database (ADR-0051). Borrows the database for its whole
// life. An index range is read through BTree::Scan, and each row through
// TableHeap::Get; the stream checks that the row exists and that its primary
// key is the key of the entry, and reports a mismatch as a kIo error
// `corrupt: index of <table> names no row <page>:<slot>` or
// `corrupt: index of <table> disagrees with its row <page>:<slot>`.
class DatabaseIndexReader : public IIndexReader {
 public:
  explicit DatabaseIndexReader(Database& database) : database_(database) {}

  std::optional<std::size_t> IndexedColumn(
      const std::string& table) const override;
  ScanResult ScanRange(const std::string& table,
                       const KeyRange& range) const override;
  ContainsResult Contains(const std::string& table,
                          std::int64_t key) const override;

 private:
  Database& database_;
};

// Applies the changes of plan to the heaps of database, in the order of
// plan.changes, and within a TableChange its inserts, updates and deletes.
// plan is borrowed and must outlive the call. Every action that succeeds is
// recorded in an undo log; when an action fails, the log is undone newest
// first, and the error of the action is returned (mapped by ToExecutionError
// with ErrorSite::kRowAccess). When an undo step fails, the remaining steps
// still run, and the message of the first failed step is appended as
// "; rollback failed: <message>". A successful undo restores the content and
// the number of rows; the Rids can differ. No value means every change is
// applied. Needs a single writer; the caller holds the exclusive DB-level lock.
std::optional<domain::ExecutionError> ApplyChangeSet(Database& database,
                                                     const MutationPlan& plan);

// Inserts rows into table, all or (best effort) none: the same undo as
// ApplyChangeSet, without copying the rows into a MutationPlan. rows is
// borrowed for the call. The table name is matched case-insensitively. Used by
// `.import`, whose rows can be many.
std::optional<domain::ExecutionError> InsertRows(
    Database& database, const std::string& table,
    const std::vector<domain::Row>& rows);

}  // namespace strata::paged

#endif  // STRATA_PAGED_HEAP_ACCESS_H_
