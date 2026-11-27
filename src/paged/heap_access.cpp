#include "strata/paged/heap_access.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Resource-aware (design document 2.13): this file is where the row layer meets
// the pages. Error mapping is pure; the stream and the reader hold a pointer
// to a TableHeap or a reference to a Database; ApplyChangeSet writes pages.

namespace strata::paged {

namespace {

using domain::ErrorKind;
using domain::ExecutionError;
using domain::Row;

ExecutionError MakeExecutionError(ErrorKind kind, std::string message) {
  return ExecutionError{kind, std::move(message)};
}

ExecutionError RowTooLongError(const std::string& table) {
  return MakeExecutionError(ErrorKind::kUnsupported,
                            "paged engine cannot store a row longer than " +
                                std::to_string(kMaxTupleSize) + " bytes in " +
                                table);
}

ExecutionError InternalError(const PageError& error) {
  return MakeExecutionError(ErrorKind::kIo, "internal error: " + error.message);
}

// ----------------------------------------------------------------------------
// The table as the write path sees it, and the undo log of ApplyChangeSet
// (design document 2.13, ADR-0053).
// ----------------------------------------------------------------------------

// A table's heap and, when its primary key is an INT, its index. Both belong
// to the Database. Every change of a row goes through the functions below, so
// the heap and the index agree after each action: a step on the heap is
// followed by the matching step on the tree, and an undo step is paired the
// same way.
struct TableAccess {
  TableHeap* heap;
  // Null for a table without an index.
  BTree* tree;
};

const std::string& NameOf(const TableAccess& access) {
  return access.heap->schema().name;
}

// The primary key of a row of a table that has an index.
std::int64_t KeyOf(const TableAccess& access, const Row& row) {
  return std::get<std::int64_t>(row[access.heap->schema().primary_key_index]);
}

enum class UndoKind { kInserted, kDeleted, kUpdated };

// One action that succeeded. `old_row` belongs to the MutationPlan, which
// outlives the log. kInserted undoes with `rid`, the position the insert
// returned; kDeleted undoes by inserting `old_row`; kUpdated undoes with `rid`
// and `old_row`. A field the kind does not use is ignored. `key` is the primary
// key of the row, taken when the action ran, so no undo step reads a page to
// find it; it is 0 for a table without an index.
struct UndoEntry {
  UndoKind kind;
  TableAccess access;
  Rid rid;
  const Row* old_row;
  std::int64_t key;
};

using UndoLog = std::vector<UndoEntry>;

ExecutionError RowAccessError(const PageError& error, const TableHeap& heap) {
  return ToExecutionError(
      error, ErrorContext{ErrorSite::kRowAccess, heap.schema().name});
}

// The error of a failed index step. A kNotFound from BTree::Delete or
// BTree::Replace, and a kAlreadyExists from BTree::Insert, mean that the index
// disagrees with the heap.
ExecutionError IndexStepError(const PageError& error, const TableAccess& access,
                              std::int64_t key) {
  if (error.code == PageErrorCode::kNotFound) {
    return MakeExecutionError(
        ErrorKind::kIo, "corrupt: index of " + NameOf(access) +
                            " has no entry for key " + std::to_string(key));
  }
  if (error.code == PageErrorCode::kAlreadyExists) {
    return MakeExecutionError(ErrorKind::kIo,
                              "corrupt: index of " + NameOf(access) +
                                  " already has an entry for key " +
                                  std::to_string(key));
  }
  return RowAccessError(error, *access.heap);
}

void AppendRollbackFailure(ExecutionError* error, const PageError& failure) {
  error->message += "; rollback failed: " + failure.message;
}

// The error of a failed Database::Table or Database::Index. A kNotFound is an
// unknown table only when the catalog has no such table. TableHeap::Open
// reports a chain that leads past the end of the file as kCorrupt, so a
// kNotFound for a table the catalog has is not expected and is an internal
// error (design document 2.13, error mapping). Catalog::FindTable is safe to
// call from several threads.
ExecutionError OpenTableError(Database& database, const PageError& error,
                              const std::string& table) {
  if (error.code == PageErrorCode::kNotFound &&
      database.catalog().FindTable(table).has_value()) {
    return InternalError(error);
  }
  return ToExecutionError(error, ErrorContext{ErrorSite::kOpenTable, table});
}

// The heap and the index of a table, opened together.
PageResult<TableAccess> OpenAccess(Database& database,
                                   const std::string& table) {
  PageResult<TableHeap*> heap = database.Table(table);
  if (!heap.ok()) {
    return heap.error();
  }
  PageResult<BTree*> tree = database.Index(table);
  if (!tree.ok()) {
    return tree.error();
  }
  return TableAccess{heap.value(), tree.value()};
}

// Puts a row back after the heap lost it: it gets a new Rid, and the key, when
// the index still holds it (pointing at the old Rid), follows the row. A key
// that the index lacks is not added: that is damage, which the statement
// reports and CheckIndex names. Returns the first failure, or no value.
Status RestoreRow(const TableAccess& access, const Row& row, std::int64_t key,
                  bool is_key_indexed) {
  PageResult<Rid> inserted = access.heap->Insert(row);
  if (!inserted.ok()) {
    return inserted.error();
  }
  if (access.tree != nullptr && is_key_indexed) {
    return access.tree->Replace(key, inserted.value());
  }
  return std::nullopt;
}

// Each Apply function runs one action. Only after the action succeeded does it
// append the matching entry to *log, so the log never names an action that did
// not happen. Returns the mapped error of the failed action, or no value. When
// the index step of an insert or a delete fails, the heap step is undone before
// the error returns and no entry is appended.

std::optional<ExecutionError> ApplyInsert(const TableAccess& access,
                                          const Row& row, UndoLog* log) {
  PageResult<Rid> inserted = access.heap->Insert(row);
  if (!inserted.ok()) {
    return RowAccessError(inserted.error(), *access.heap);
  }
  std::int64_t key = 0;
  if (access.tree != nullptr) {
    key = KeyOf(access, row);
    if (Status status = access.tree->Insert(key, inserted.value())) {
      ExecutionError error = IndexStepError(*status, access, key);
      if (Status undone = access.heap->Delete(inserted.value())) {
        AppendRollbackFailure(&error, *undone);
      }
      return error;
    }
  }
  log->push_back(
      UndoEntry{UndoKind::kInserted, access, inserted.value(), nullptr, key});
  return std::nullopt;
}

std::optional<ExecutionError> ApplyDelete(const TableAccess& access, Rid rid,
                                          const Row& old_row, UndoLog* log) {
  if (Status status = access.heap->Delete(rid)) {
    return RowAccessError(*status, *access.heap);
  }
  std::int64_t key = 0;
  if (access.tree != nullptr) {
    key = KeyOf(access, old_row);
    if (Status status = access.tree->Delete(key)) {
      ExecutionError error = IndexStepError(*status, access, key);
      // A key the index lacks stays missing; after an I/O failure the key is
      // still there and follows the row to its new Rid.
      const bool is_key_indexed = status->code != PageErrorCode::kNotFound;
      if (Status restored = RestoreRow(access, old_row, key, is_key_indexed)) {
        AppendRollbackFailure(&error, *restored);
      }
      return error;
    }
  }
  log->push_back(UndoEntry{UndoKind::kDeleted, access, rid, &old_row, key});
  return std::nullopt;
}

// An update that fits in place keeps its Rid and its key. One that returns
// kNoSpace is a delete followed by an insert in the heap and a Replace of the
// key in the index, and the three steps are one action: when a later step
// fails, the earlier ones are undone before the error returns and no entry is
// recorded, as for a single insert or delete. After the action succeeds, its
// two heap steps are recorded as "Deleted(old_row)" and "Inserted(new_rid)",
// with the key now pointing at the new Rid, so the log undoes them as the
// delete of the key followed by its insertion again.
std::optional<ExecutionError> ApplyUpdate(const TableAccess& access,
                                          const RowUpdate& update,
                                          UndoLog* log) {
  const std::int64_t key =
      access.tree != nullptr ? KeyOf(access, update.old_row) : 0;
  Status status = access.heap->Update(update.rid, update.new_row);
  if (!status.has_value()) {
    log->push_back(UndoEntry{UndoKind::kUpdated, access, update.rid,
                             &update.old_row, key});
    return std::nullopt;
  }
  if (status->code != PageErrorCode::kNoSpace) {
    return RowAccessError(*status, *access.heap);
  }
  if (Status deleted = access.heap->Delete(update.rid)) {
    return RowAccessError(*deleted, *access.heap);
  }
  PageResult<Rid> inserted = access.heap->Insert(update.new_row);
  if (!inserted.ok()) {
    // The key still names the old Rid, so it follows the row that comes back.
    ExecutionError error = RowAccessError(inserted.error(), *access.heap);
    if (Status restored = RestoreRow(access, update.old_row, key, true)) {
      AppendRollbackFailure(&error, *restored);
    }
    return error;
  }
  if (access.tree != nullptr) {
    if (Status replaced = access.tree->Replace(key, inserted.value())) {
      ExecutionError error = IndexStepError(*replaced, access, key);
      if (Status undone = access.heap->Delete(inserted.value())) {
        AppendRollbackFailure(&error, *undone);
      }
      const bool is_key_indexed = replaced->code != PageErrorCode::kNotFound;
      if (Status restored =
              RestoreRow(access, update.old_row, key, is_key_indexed)) {
        AppendRollbackFailure(&error, *restored);
      }
      return error;
    }
  }
  log->push_back(
      UndoEntry{UndoKind::kDeleted, access, update.rid, &update.old_row, key});
  log->push_back(
      UndoEntry{UndoKind::kInserted, access, inserted.value(), nullptr, key});
  return std::nullopt;
}

std::optional<ExecutionError> ApplyInserts(const TableAccess& access,
                                           const std::vector<Row>& rows,
                                           UndoLog* log) {
  for (const Row& row : rows) {
    if (auto error = ApplyInsert(access, row, log)) {
      return error;
    }
  }
  return std::nullopt;
}

// True for an update of a table with an index that gives the row another key.
// A direct UPDATE never does (design document 2.4); a cascade can.
bool ChangesKey(const TableAccess& access, const RowUpdate& update) {
  return access.tree != nullptr &&
         KeyOf(access, update.old_row) != KeyOf(access, update.new_row);
}

// The updates of one TableChange whose key changes run as deletes and inserts
// through the helper, in two sweeps: a cascade can move keys in a chain (A to
// B and B to C), and an insert of B must not meet the row whose B is about to
// become C. The rows get new Rids, which the index records as any insert does.
std::optional<ExecutionError> ApplyKeyChanges(
    const TableAccess& access, const std::vector<const RowUpdate*>& updates,
    UndoLog* log) {
  for (const RowUpdate* update : updates) {
    if (auto error = ApplyDelete(access, update->rid, update->old_row, log)) {
      return error;
    }
  }
  for (const RowUpdate* update : updates) {
    if (auto error = ApplyInsert(access, update->new_row, log)) {
      return error;
    }
  }
  return std::nullopt;
}

// One TableChange holds one kind of change; the loops run in the order
// inserts, updates (with the key changes after the others), deletes.
std::optional<ExecutionError> ApplyTableChange(Database& database,
                                               const TableChange& change,
                                               UndoLog* log) {
  PageResult<TableAccess> opened = OpenAccess(database, change.table);
  if (!opened.ok()) {
    return OpenTableError(database, opened.error(), change.table);
  }
  const TableAccess access = opened.value();
  if (auto error = ApplyInserts(access, change.inserts, log)) {
    return error;
  }
  std::vector<const RowUpdate*> key_changes;
  for (const RowUpdate& update : change.updates) {
    if (ChangesKey(access, update)) {
      key_changes.push_back(&update);
    } else if (auto error = ApplyUpdate(access, update, log)) {
      return error;
    }
  }
  if (auto error = ApplyKeyChanges(access, key_changes, log)) {
    return error;
  }
  for (const RowDelete& deletion : change.deletes) {
    if (auto error = ApplyDelete(access, deletion.rid, deletion.old_row, log)) {
      return error;
    }
  }
  return std::nullopt;
}

// Puts a row and its key back after an undo: the row gets a new Rid, and the
// key is added.
Status UndoRestoreRow(const UndoEntry& entry) {
  PageResult<Rid> inserted = entry.access.heap->Insert(*entry.old_row);
  if (!inserted.ok()) {
    return inserted.error();
  }
  if (entry.access.tree == nullptr) {
    return std::nullopt;
  }
  return entry.access.tree->Insert(entry.key, inserted.value());
}

// Undoes one entry through the same pairing of heap and index as the action.
// An update that no longer fits in its page (an undo insert executed earlier
// may have taken the space) is undone as a delete followed by an insert, and
// the key follows the row. Both steps of an undo run even when the first
// fails; the first failure is returned.
Status UndoEntryAction(const UndoEntry& entry) {
  switch (entry.kind) {
    case UndoKind::kInserted: {
      Status deleted = entry.access.heap->Delete(entry.rid);
      Status unindexed;
      if (entry.access.tree != nullptr) {
        unindexed = entry.access.tree->Delete(entry.key);
      }
      return deleted.has_value() ? deleted : unindexed;
    }
    case UndoKind::kDeleted:
      return UndoRestoreRow(entry);
    case UndoKind::kUpdated: {
      Status status = entry.access.heap->Update(entry.rid, *entry.old_row);
      if (!status.has_value() || status->code != PageErrorCode::kNoSpace) {
        return status;
      }
      if (Status deleted = entry.access.heap->Delete(entry.rid)) {
        return deleted;
      }
      PageResult<Rid> inserted = entry.access.heap->Insert(*entry.old_row);
      if (!inserted.ok()) {
        return inserted.error();
      }
      if (entry.access.tree != nullptr) {
        return entry.access.tree->Replace(entry.key, inserted.value());
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// Undoes the log newest first. A failed step does not stop the others. Returns
// the message of the first failed step, or no value when every step worked.
std::optional<std::string> UndoAll(const UndoLog& log) {
  std::optional<std::string> first_failure;
  for (std::size_t i = log.size(); i > 0; --i) {
    Status status = UndoEntryAction(log[i - 1]);
    if (status.has_value() && !first_failure.has_value()) {
      first_failure = status->message;
    }
  }
  return first_failure;
}

// The error of a failed apply, after the log has been undone.
ExecutionError AfterUndo(ExecutionError error, const UndoLog& log) {
  if (std::optional<std::string> failure = UndoAll(log)) {
    error.message += "; rollback failed: " + *failure;
  }
  return error;
}

}  // namespace

ExecutionError ToExecutionError(const PageError& error,
                                const ErrorContext& context) {
  switch (error.code) {
    case PageErrorCode::kIo:
    case PageErrorCode::kNoSpace:
      return MakeExecutionError(ErrorKind::kIo, error.message);
    case PageErrorCode::kCorrupt:
      return MakeExecutionError(ErrorKind::kIo, "corrupt: " + error.message);
    case PageErrorCode::kNoFrame:
      return MakeExecutionError(ErrorKind::kIo,
                                "buffer pool exhausted: " + error.message);
    case PageErrorCode::kClosed:
      return MakeExecutionError(ErrorKind::kIo, "database is closed");
    case PageErrorCode::kNotFound:
      if (context.site == ErrorSite::kOpenTable ||
          context.site == ErrorSite::kAddColumn) {
        return MakeExecutionError(ErrorKind::kBind,
                                  "unknown table: " + context.table);
      }
      return InternalError(error);
    case PageErrorCode::kTooLarge:
      return RowTooLongError(context.table);
    case PageErrorCode::kInvalidArgument:
      if (context.site == ErrorSite::kCreateTable ||
          context.site == ErrorSite::kAddColumn) {
        return MakeExecutionError(ErrorKind::kUnsupported, error.message);
      }
      return InternalError(error);
    case PageErrorCode::kAlreadyExists:
      if (context.site == ErrorSite::kCreateTable) {
        return MakeExecutionError(ErrorKind::kBind,
                                  "table " + context.table + " already exists");
      }
      return InternalError(error);
  }
  return InternalError(error);
}

HeapRowStream::HeapRowStream(const TableHeap* heap)
    : heap_(heap), scan_(heap->Scan()) {}

StreamResult HeapRowStream::Next() {
  PageResult<std::optional<ScanItem>> next = scan_.Next();
  if (!next.ok()) {
    return ToExecutionError(next.error(), ErrorContext{ErrorSite::kRowAccess,
                                                       heap_->schema().name});
  }
  std::optional<ScanItem>& item = next.value();
  if (!item.has_value()) {
    return std::optional<StoredRow>();
  }
  return std::optional<StoredRow>(StoredRow{item->rid, std::move(item->row)});
}

std::vector<domain::ColumnDef> HeapRowStream::Columns() const {
  return heap_->schema().columns;
}

ScanResult DatabaseTableReader::Scan(const std::string& table) const {
  PageResult<TableHeap*> opened = database_.Table(table);
  if (!opened.ok()) {
    return OpenTableError(database_, opened.error(), table);
  }
  return std::unique_ptr<IRowStream>(
      std::make_unique<HeapRowStream>(opened.value()));
}

namespace {

// The rows of an index range, in key order. The entries come from the cursor,
// and each row is read with TableHeap::Get, which reads one heap page. An entry
// whose Rid names no row, and a row whose key is not the key of its entry, are
// damage and end the stream with an error; after an error every later call
// returns the end of the rows. Borrows the heap, which must outlive the stream.
class IndexRowStream : public IRowStream {
 public:
  IndexRowStream(const TableHeap* heap, BTreeCursor cursor)
      : heap_(heap), cursor_(std::move(cursor)) {}

  StreamResult Next() override {
    if (is_done_) {
      return std::optional<StoredRow>();
    }
    PageResult<std::optional<BTreeEntry>> next = cursor_.Next();
    if (!next.ok()) {
      return Fail(RowAccessError(next.error(), *heap_));
    }
    if (!next.value().has_value()) {
      is_done_ = true;
      return std::optional<StoredRow>();
    }
    const BTreeEntry entry = *next.value();
    const std::string position = std::to_string(entry.rid.page_id) + ":" +
                                 std::to_string(entry.rid.slot_id);
    PageResult<Row> read = heap_->Get(entry.rid);
    if (!read.ok()) {
      // A dead slot, a slot out of range, a page past the end of the file and
      // a page that is not a heap page of the table all mean that the entry
      // names no row.
      const PageErrorCode code = read.error().code;
      if (code == PageErrorCode::kNotFound ||
          code == PageErrorCode::kInvalidArgument) {
        return Fail(MakeExecutionError(
            ErrorKind::kIo, "corrupt: index of " + heap_->schema().name +
                                " names no row " + position));
      }
      return Fail(RowAccessError(read.error(), *heap_));
    }
    Row row = std::move(read).value();
    if (std::get<std::int64_t>(row[heap_->schema().primary_key_index]) !=
        entry.key) {
      return Fail(MakeExecutionError(
          ErrorKind::kIo, "corrupt: index of " + heap_->schema().name +
                              " disagrees with its row " + position));
    }
    return std::optional<StoredRow>(StoredRow{entry.rid, std::move(row)});
  }

  std::vector<domain::ColumnDef> Columns() const override {
    return heap_->schema().columns;
  }

 private:
  StreamResult Fail(ExecutionError error) {
    is_done_ = true;
    return error;
  }

  const TableHeap* heap_;
  BTreeCursor cursor_;
  bool is_done_ = false;
};

}  // namespace

std::optional<std::size_t> DatabaseIndexReader::IndexedColumn(
    const std::string& table) const {
  if (!database_.catalog().IndexRoot(table).has_value()) {
    return std::nullopt;
  }
  const std::optional<domain::TableSchema> schema =
      database_.catalog().FindTable(table);
  if (!schema.has_value()) {
    return std::nullopt;
  }
  return schema->primary_key_index;
}

ScanResult DatabaseIndexReader::ScanRange(const std::string& table,
                                          const KeyRange& range) const {
  PageResult<TableAccess> opened = OpenAccess(database_, table);
  if (!opened.ok()) {
    return OpenTableError(database_, opened.error(), table);
  }
  const TableAccess access = opened.value();
  if (access.tree == nullptr) {
    return MakeExecutionError(
        ErrorKind::kIo, "internal error: table " + table + " has no index");
  }
  return std::unique_ptr<IRowStream>(
      std::make_unique<IndexRowStream>(access.heap, access.tree->Scan(range)));
}

ContainsResult DatabaseIndexReader::Contains(const std::string& table,
                                             std::int64_t key) const {
  PageResult<BTree*> opened = database_.Index(table);
  if (!opened.ok()) {
    return OpenTableError(database_, opened.error(), table);
  }
  BTree* const tree = opened.value();
  if (tree == nullptr) {
    return MakeExecutionError(
        ErrorKind::kIo, "internal error: table " + table + " has no index");
  }
  PageResult<std::optional<Rid>> found = tree->Find(key);
  if (!found.ok()) {
    return ToExecutionError(found.error(),
                            ErrorContext{ErrorSite::kRowAccess, table});
  }
  return found.value().has_value();
}

std::optional<ExecutionError> ApplyChangeSet(Database& database,
                                             const MutationPlan& plan) {
  UndoLog log;
  for (const TableChange& change : plan.changes) {
    if (auto error = ApplyTableChange(database, change, &log)) {
      return AfterUndo(std::move(*error), log);
    }
  }
  return std::nullopt;
}

std::optional<ExecutionError> InsertRows(Database& database,
                                         const std::string& table,
                                         const std::vector<Row>& rows) {
  PageResult<TableAccess> opened = OpenAccess(database, table);
  if (!opened.ok()) {
    return OpenTableError(database, opened.error(), table);
  }
  UndoLog log;
  if (auto error = ApplyInserts(opened.value(), rows, &log)) {
    return AfterUndo(std::move(*error), log);
  }
  return std::nullopt;
}

}  // namespace strata::paged
