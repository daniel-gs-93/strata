#ifndef STRATA_APPLICATION_H_
#define STRATA_APPLICATION_H_

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "strata/domain/ast.h"
#include "strata/domain/engine.h"
#include "strata/domain/import.h"
#include "strata/domain/index_inspector.h"
#include "strata/domain/value.h"

namespace strata::application {

// Executes one statement: binds it against the engine's catalog, then hands
// the plan to the engine. Owns the engine. Imports only the domain.
//
// Concurrency (docs/DesignDocument.md section 2.14). One database-level
// std::shared_mutex, above the engine, serializes statements and prevents a
// reader from seeing a half-applied write (data race), two writers from
// interleaving their read-modify-write steps (lost update), and a DDL
// statement from changing the schema between bind and execute (stale column
// positions). Execute of a SelectStatement and DescribeCatalog take it shared;
// Execute of every other statement and ImportCsv take it exclusive. A call
// holds the lock for its whole body (bind and execute are one critical section)
// in a local lock object, so the lock never crosses a call. The member
// functions are not reentrant: none calls another one that takes the lock.
// Page I/O of the engine runs under the lock.
//
// Lock order: this lock comes before every storage-layer lock (the table heap
// cache mutex of Database, the Catalog lock, the frame latch, the pool
// mutex). Code that holds any of them never calls into QueryService, so no
// thread waits for this lock while holding one of those.
//
// DescribeIndex takes the lock shared, as DescribeCatalog does. All four member
// functions may be called from several threads at the same time. The object is
// neither copyable nor movable (the mutex); callers hold it behind a
// std::unique_ptr.
class QueryService {
 public:
  // csv_source serves ImportCsv; without it ImportCsv reports kUnsupported.
  // index_inspector serves DescribeIndex. It is borrowed, not owned, and must
  // outlive this object: it is the engine itself (or a part of it), which this
  // object owns. Null for an engine that has no indexes.
  explicit QueryService(
      std::unique_ptr<domain::IEngine> engine,
      std::unique_ptr<domain::ICsvSource> csv_source = nullptr,
      domain::IIndexInspector* index_inspector = nullptr);

  // Borrows statement for the duration of the call.
  domain::ExecutionResult Execute(const domain::Statement& statement);

  // The schema of every table of the engine's catalog, in registration order.
  // The data of the `.tables` and `.schema` commands.
  std::vector<domain::TableSchema> DescribeCatalog() const;

  // Imports an external CSV file as a new table (docs/DesignDocument.md
  // sections 2.7 and 3.5): reads it through the CSV source, infers the table,
  // and has the engine create and write it in one step. Returns a
  // CommandComplete whose tag is "IMPORT <table> (imported <n> rows[, skipped
  // <m>])" and whose notes describe the skipped rows, or an ExecutionError.
  domain::ExecutionResult ImportCsv(const domain::ImportRequest& request);

  // The shape and the check of the index of a table (the data of the `.dump
  // index` command), under the shared lock. A kUnsupported error "the flatfile
  // engine has no indexes" when no inspector was given; otherwise the result of
  // IIndexInspector::DescribeIndex.
  domain::IndexDescriptionResult DescribeIndex(const std::string& table) const;

 private:
  std::unique_ptr<domain::IEngine> engine_;
  std::unique_ptr<domain::ICsvSource> csv_source_;
  domain::IIndexInspector* index_inspector_;
  // The database-level lock; mutable so that DescribeCatalog() const can take
  // it shared.
  mutable std::shared_mutex database_mutex_;
};

}  // namespace strata::application

#endif  // STRATA_APPLICATION_H_
