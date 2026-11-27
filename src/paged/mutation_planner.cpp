#include "strata/paged/mutation_planner.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/expression.h"
#include "strata/paged/key_range.h"
#include "strata/paged/row_keys.h"
#include "strata/paged/tuple_codec.h"

// Pure logic (design document 2.13): every function here reads schemas from an
// ICatalog and rows from an ITableReader and returns a value. It touches no
// page, pin or lock. Ownership: a function that takes a std::vector or a Row by
// value or by rvalue reference owns it afterwards; a pointer parameter is
// borrowed for the call.

namespace strata::paged {

namespace {

using domain::BoundExpr;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ForeignKey;
using domain::Row;
using domain::TableSchema;
using domain::Value;

// ----------------------------------------------------------------------------
// Errors.
// ----------------------------------------------------------------------------

ExecutionError MakeExecutionError(ErrorKind kind, std::string message) {
  return ExecutionError{kind, std::move(message)};
}

ExecutionError UnknownTableError(const std::string& name) {
  return MakeExecutionError(ErrorKind::kBind, "unknown table: " + name);
}

ExecutionError OverflowError() {
  return MakeExecutionError(ErrorKind::kRuntime, "numeric overflow");
}

// The kRuntime error for a failed evaluation; failure is the message that
// domain::Evaluate or domain::EvaluateCondition wrote.
ExecutionError EvaluationError(std::string failure) {
  return MakeExecutionError(ErrorKind::kRuntime, std::move(failure));
}

// Table.column in the declared spelling, as used in error messages.
std::string QualifiedName(const TableSchema& schema, std::size_t column_index) {
  return schema.name + "." + schema.columns[column_index].name;
}

ExecutionError DuplicateKeyError(const TableSchema& schema,
                                 std::size_t column_index, const Value& value) {
  return MakeExecutionError(
      ErrorKind::kConstraintViolation,
      "duplicate key: " + QualifiedName(schema, column_index) + "=" +
          domain::FormatValue(value));
}

ExecutionError NoMatchingRowError(const TableSchema& schema,
                                  const ForeignKey& foreign_key,
                                  const TableSchema& parent_schema,
                                  const Value& value) {
  return MakeExecutionError(
      ErrorKind::kConstraintViolation,
      "no matching row: " + QualifiedName(schema, foreign_key.column_index) +
          "=" + domain::FormatValue(value) + " not found in " +
          QualifiedName(parent_schema, foreign_key.ref_column_index));
}

ExecutionError RowTooLongError(const TableSchema& schema) {
  return MakeExecutionError(ErrorKind::kUnsupported,
                            "paged engine cannot store a row longer than " +
                                std::to_string(kMaxTupleSize) + " bytes in " +
                                schema.name);
}

// The tuple length check (check 1 of the paged engine, design document 2.5):
// the row must encode within kMaxTupleSize bytes. TupleCodec::Encode computes
// the length; the bytes are discarded.
std::optional<ExecutionError> CheckRowLength(const TableSchema& schema,
                                             const Row& row) {
  PageResult<std::vector<std::uint8_t>> encoded =
      TupleCodec::Encode(schema, row);
  if (encoded.ok()) {
    return std::nullopt;
  }
  const PageError& error = encoded.error();
  if (error.code == PageErrorCode::kTooLarge) {
    return RowTooLongError(schema);
  }
  // The row does not match the schema (a column count or a type): the plan
  // broke the precondition of PlanMutation.
  return MakeExecutionError(ErrorKind::kIo, "internal error: " + error.message);
}

// ----------------------------------------------------------------------------
// Reading tables.
// ----------------------------------------------------------------------------

// Receives the rows of one scan, in storage order.
class RowVisitor {
 public:
  virtual ~RowVisitor() = default;

  // Takes the row. An error stops the scan and is returned to its caller.
  virtual std::optional<ExecutionError> Visit(StoredRow&& stored) = 0;
};

// Reads a stream from its first row to its end, handing each row to the
// visitor. Holds one row at a time, so memory does not grow with the table.
// Returns the error that opened the stream, of the stream or of the visitor.
std::optional<ExecutionError> VisitStream(ScanResult scanned,
                                          RowVisitor* visitor) {
  if (auto* error = std::get_if<ExecutionError>(&scanned)) {
    return std::move(*error);
  }
  IRowStream& stream = *std::get<std::unique_ptr<IRowStream>>(scanned);
  while (true) {
    StreamResult next = stream.Next();
    if (auto* error = std::get_if<ExecutionError>(&next)) {
      return std::move(*error);
    }
    std::optional<StoredRow>& stored = std::get<std::optional<StoredRow>>(next);
    if (!stored.has_value()) {
      return std::nullopt;
    }
    if (auto error = visitor->Visit(std::move(*stored))) {
      return error;
    }
  }
}

// Reads the table once, from its first row to its end, handing each row to the
// visitor. Returns the error of Scan, of the stream or of the visitor.
std::optional<ExecutionError> ScanRows(const ITableReader& reader,
                                       const std::string& table,
                                       RowVisitor* visitor) {
  return VisitStream(reader.Scan(table), visitor);
}

// Hands the visitor the rows that `predicate` may keep. When index_reader is
// given, the table has an indexed column and ExtractKeyRange finds a range in
// the predicate, the rows come from ScanRange, in ascending key order, and no
// other row is read; the caller still tests the whole predicate on each row.
// Otherwise it is ScanRows: every row, in storage order. A null predicate is
// every row. Borrows index_reader and predicate for the call.
std::optional<ExecutionError> ScanWhereRows(const ITableReader& reader,
                                            const IIndexReader* index_reader,
                                            const std::string& table,
                                            const BoundExpr* predicate,
                                            RowVisitor* visitor) {
  if (index_reader != nullptr && predicate != nullptr) {
    const std::optional<std::size_t> column =
        index_reader->IndexedColumn(table);
    if (column.has_value()) {
      const std::optional<KeyRange> range =
          ExtractKeyRange(*predicate, *column);
      if (range.has_value()) {
        return VisitStream(index_reader->ScanRange(table, *range), visitor);
      }
    }
  }
  return ScanRows(reader, table, visitor);
}

// The ExactKey sets of a list of columns of one table. Add grows every set with
// one row, so a row can be compared with the rows added before it. Lookup and
// insertion are O(1) on average.
class KeySets {
 public:
  explicit KeySets(std::vector<std::size_t> columns)
      : columns_(std::move(columns)), keys_(columns_.size()) {}

  void Add(const Row& row) {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      keys_[i].insert(ExactKey(row[columns_[i]]));
    }
  }

  // The first column (in the order given at construction) whose value in row is
  // already present, or std::nullopt.
  std::optional<std::size_t> FindDuplicate(const Row& row) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      if (keys_[i].count(ExactKey(row[columns_[i]])) > 0) {
        return columns_[i];
      }
    }
    return std::nullopt;
  }

  // Whether the set of the position-th column holds the value.
  bool Contains(std::size_t position, const Value& value) const {
    return keys_[position].count(ExactKey(value)) > 0;
  }

 private:
  std::vector<std::size_t> columns_;
  std::vector<std::unordered_set<std::string>> keys_;  // by position
};

// Adds every row of a scan to a KeySets. When max_column is set, also tracks
// the largest value of that INT column, and at least 0, which is the start
// value of an auto-increment column (design document 2.3). Borrows sets for
// the scan.
class KeyCollector : public RowVisitor {
 public:
  KeyCollector(KeySets* sets, std::optional<std::size_t> max_column)
      : sets_(sets), max_column_(max_column) {}

  std::optional<ExecutionError> Visit(StoredRow&& stored) override {
    sets_->Add(stored.row);
    if (!max_column_.has_value()) {
      return std::nullopt;
    }
    if (const auto* number =
            std::get_if<std::int64_t>(&stored.row[*max_column_])) {
      largest_ = std::max(largest_, *number);
    }
    return std::nullopt;
  }

  std::int64_t largest() const { return largest_; }

 private:
  KeySets* sets_;
  std::optional<std::size_t> max_column_;
  std::int64_t largest_ = 0;
};

// The key set of the referenced column of one foreign key, with the schema of
// the parent table.
struct ParentKeys {
  TableSchema schema;
  KeySets keys;  // one column: the referenced column
  // True when the referenced column is the indexed primary key of the parent:
  // `keys` is then empty and a value is looked up with IIndexReader::Contains.
  bool is_indexed = false;
};

using ParentLoad = std::variant<ParentKeys, ExecutionError>;

// The schema of the parent of foreign_key with no keys loaded, for a parent
// whose referenced column is indexed.
ParentLoad LoadIndexedParent(const domain::ICatalog& catalog,
                             const ForeignKey& foreign_key) {
  std::optional<TableSchema> parent_schema =
      catalog.FindTable(foreign_key.ref_table);
  if (!parent_schema.has_value()) {
    return UnknownTableError(foreign_key.ref_table);
  }
  ParentKeys parent{std::move(*parent_schema),
                    KeySets({foreign_key.ref_column_index})};
  parent.is_indexed = true;
  return parent;
}

// Scans the parent table of foreign_key once.
ParentLoad LoadParentKeys(const domain::ICatalog& catalog,
                          const ITableReader& reader,
                          const ForeignKey& foreign_key) {
  std::optional<TableSchema> parent_schema =
      catalog.FindTable(foreign_key.ref_table);
  if (!parent_schema.has_value()) {
    return UnknownTableError(foreign_key.ref_table);
  }
  KeySets keys({foreign_key.ref_column_index});
  KeyCollector collector(&keys, std::nullopt);
  if (auto error = ScanRows(reader, parent_schema->name, &collector)) {
    return std::move(*error);
  }
  return ParentKeys{std::move(*parent_schema), std::move(keys)};
}

// One foreign key seen from the table it references: the child table's schema
// and the key.
struct Reference {
  TableSchema child;
  ForeignKey foreign_key;
};

// Every foreign key that references the table named target_key (lowercase), in
// catalog order of the child table and then declaration order, which is the
// order DELETE reports and a cascade follows.
std::vector<Reference> FindReferences(const domain::ICatalog& catalog,
                                      const std::string& target_key) {
  std::vector<Reference> references;
  for (const std::string& name : catalog.TableNames()) {
    std::optional<TableSchema> child = catalog.FindTable(name);
    if (!child.has_value()) {
      continue;
    }
    for (const ForeignKey& foreign_key : child->foreign_keys) {
      if (domain::ToLower(foreign_key.ref_table) != target_key) {
        continue;
      }
      references.push_back(Reference{*child, foreign_key});
    }
  }
  return references;
}

// Selects the rows a WHERE predicate keeps (all rows when it is null) and
// records them with their Rid. For the rows it does not select, it counts the
// ExactKey of each counted column, so a uniqueness check can see "the other
// rows" without a second scan.
class RowSelector : public RowVisitor {
 public:
  // Borrows predicate for the scan; null selects every row.
  RowSelector(const BoundExpr* predicate, std::vector<std::size_t> counted)
      : predicate_(predicate),
        counted_columns_(std::move(counted)),
        other_counts_(counted_columns_.size()) {}

  std::optional<ExecutionError> Visit(StoredRow&& stored) override {
    if (predicate_ == nullptr) {
      selected_.push_back(std::move(stored));
      return std::nullopt;
    }
    std::string failure;
    std::optional<bool> is_match =
        domain::EvaluateCondition(*predicate_, stored.row, &failure);
    if (!is_match.has_value()) {
      return EvaluationError(std::move(failure));
    }
    if (*is_match) {
      selected_.push_back(std::move(stored));
      return std::nullopt;
    }
    for (std::size_t i = 0; i < counted_columns_.size(); ++i) {
      ++other_counts_[i][ExactKey(stored.row[counted_columns_[i]])];
    }
    return std::nullopt;
  }

  // The selected rows in scan order; the caller owns them. Call once.
  std::vector<StoredRow> TakeSelected() { return std::move(selected_); }

  // For each counted column, how many unselected rows hold each ExactKey. The
  // caller owns them. Call once.
  std::vector<std::unordered_map<std::string, std::size_t>> TakeOtherCounts() {
    return std::move(other_counts_);
  }

 private:
  const BoundExpr* predicate_;
  std::vector<std::size_t> counted_columns_;
  std::vector<StoredRow> selected_;
  std::vector<std::unordered_map<std::string, std::size_t>> other_counts_;
};

// ----------------------------------------------------------------------------
// INSERT.
// ----------------------------------------------------------------------------

// The primary key followed by the UNIQUE columns, ascending by position: the
// order in which an INSERT reports a duplicate.
std::vector<std::size_t> UniqueColumns(const TableSchema& schema) {
  std::vector<std::size_t> columns{schema.primary_key_index};
  columns.insert(columns.end(), schema.unique_indexes.begin(),
                 schema.unique_indexes.end());
  return columns;
}

// Foreign key check of one INSERT row, in declaration order. parents holds one
// slot per foreign key; a slot is filled the first time a row reaches that
// foreign key and reused by the later rows: by scanning the parent table, or,
// when index_reader is given and the referenced column is the indexed primary
// key of the parent, by taking its schema only, and each row is then looked up
// with Contains. Borrows parents for the call.
std::optional<ExecutionError> CheckInsertForeignKeys(
    const domain::ICatalog& catalog, const ITableReader& reader,
    const IIndexReader* index_reader, const TableSchema& schema, const Row& row,
    std::vector<std::optional<ParentKeys>>* parents) {
  for (std::size_t i = 0; i < schema.foreign_keys.size(); ++i) {
    const ForeignKey& foreign_key = schema.foreign_keys[i];
    std::optional<ParentKeys>& parent = (*parents)[i];
    if (!parent.has_value()) {
      const bool is_indexed =
          index_reader != nullptr &&
          index_reader->IndexedColumn(foreign_key.ref_table) ==
              std::optional<std::size_t>(foreign_key.ref_column_index);
      ParentLoad loaded = is_indexed
                              ? LoadIndexedParent(catalog, foreign_key)
                              : LoadParentKeys(catalog, reader, foreign_key);
      if (auto* error = std::get_if<ExecutionError>(&loaded)) {
        return std::move(*error);
      }
      parent = std::move(std::get<ParentKeys>(loaded));
    }
    const Value& value = row[foreign_key.column_index];
    if (parent->is_indexed) {
      // The foreign key column has the type of the referenced INT column.
      if (const auto* key = std::get_if<std::int64_t>(&value)) {
        ContainsResult present =
            index_reader->Contains(parent->schema.name, *key);
        if (auto* error = std::get_if<ExecutionError>(&present)) {
          return std::move(*error);
        }
        if (std::get<bool>(present)) {
          continue;
        }
      }
      return NoMatchingRowError(schema, foreign_key, parent->schema, value);
    }
    if (parent->keys.Contains(0, value)) {
      continue;
    }
    return NoMatchingRowError(schema, foreign_key, parent->schema, value);
  }
  return std::nullopt;
}

// Rows are processed one at a time in statement order. For each row: the
// auto-increment value, then the checks of design document 2.5 in order (tuple
// length, primary key, UNIQUE columns, foreign keys); a row that passes joins
// the key sets so that later rows compare against it. The first violation is
// returned. insert.rows is copied; the node is not modified.
PlanMutationResult PlanInsert(const domain::InsertNode& insert,
                              const domain::ICatalog& catalog,
                              const ITableReader& reader,
                              const IIndexReader* index_reader) {
  std::optional<TableSchema> found = catalog.FindTable(insert.table);
  if (!found.has_value()) {
    return UnknownTableError(insert.table);
  }
  const TableSchema& schema = *found;

  // With the primary key indexed, no UNIQUE column and no generated value, the
  // target is not scanned: the only key that can collide is the primary key,
  // and each row asks the index with Contains. A UNIQUE column needs the other
  // rows' values, and a generated value needs the largest key, which a lazy
  // delete can leave far from the rightmost leaf (ADR-0051).
  const bool is_key_looked_up =
      index_reader != nullptr &&
      index_reader->IndexedColumn(schema.name) ==
          std::optional<std::size_t>(schema.primary_key_index) &&
      schema.unique_indexes.empty() && !insert.generated_index.has_value();
  KeySets unique_keys(UniqueColumns(schema));
  KeyCollector existing(&unique_keys, insert.generated_index);
  if (!is_key_looked_up) {
    if (auto error = ScanRows(reader, schema.name, &existing)) {
      return std::move(*error);
    }
  }

  std::vector<std::optional<ParentKeys>> parents(schema.foreign_keys.size());
  std::vector<Row> rows = insert.rows;
  std::int64_t last_generated = existing.largest();
  for (Row& row : rows) {
    assert(row.size() == schema.columns.size());
    if (insert.generated_index.has_value()) {
      if (last_generated == std::numeric_limits<std::int64_t>::max()) {
        return OverflowError();
      }
      ++last_generated;
      row[*insert.generated_index] = Value(last_generated);
    }
    if (auto error = CheckRowLength(schema, row)) {
      return std::move(*error);
    }
    if (std::optional<std::size_t> column = unique_keys.FindDuplicate(row)) {
      return DuplicateKeyError(schema, *column, row[*column]);
    }
    if (is_key_looked_up) {
      // The key sets hold the earlier rows of this statement; the index holds
      // the rows that were there before it.
      const Value& key = row[schema.primary_key_index];
      ContainsResult present =
          index_reader->Contains(schema.name, std::get<std::int64_t>(key));
      if (auto* error = std::get_if<ExecutionError>(&present)) {
        return std::move(*error);
      }
      if (std::get<bool>(present)) {
        return DuplicateKeyError(schema, schema.primary_key_index, key);
      }
    }
    if (auto error = CheckInsertForeignKeys(catalog, reader, index_reader,
                                            schema, row, &parents)) {
      return std::move(*error);
    }
    unique_keys.Add(row);
  }

  const std::size_t affected = rows.size();
  TableChange change;
  change.table = domain::ToLower(schema.name);
  change.inserts = std::move(rows);
  MutationPlan plan;
  plan.changes.push_back(std::move(change));
  plan.affected = affected;
  return plan;
}

// ----------------------------------------------------------------------------
// UPDATE.
// ----------------------------------------------------------------------------

bool IsAssigned(const domain::UpdateNode& update, std::size_t column_index) {
  for (const domain::BoundAssignment& assignment : update.assignments) {
    if (assignment.column_index == column_index) {
      return true;
    }
  }
  return false;
}

// Whether the statement changed the value of one column. Equality is
// operator== of domain::Value, so -0.0 and 0.0 are the same value.
bool IsCellChanged(const RowUpdate& candidate, std::size_t column_index) {
  return candidate.old_row[column_index] != candidate.new_row[column_index];
}

bool IsRowChanged(const RowUpdate& candidate) {
  return candidate.old_row != candidate.new_row;
}

// Assigned columns that some foreign key references, ascending.
std::set<std::size_t> AssignedReferencedColumns(
    const domain::UpdateNode& update,
    const std::vector<Reference>& references) {
  std::set<std::size_t> referenced;
  for (const Reference& reference : references) {
    referenced.insert(reference.foreign_key.ref_column_index);
  }
  std::set<std::size_t> assigned_referenced;
  for (const domain::BoundAssignment& assignment : update.assignments) {
    if (referenced.contains(assignment.column_index)) {
      assigned_referenced.insert(assignment.column_index);
    }
  }
  return assigned_referenced;
}

// Assigned columns that must stay unique: the assigned UNIQUE columns and the
// assigned referenced columns (unique by declaration), ascending. The primary
// key is never assigned (the Binder rejects it).
std::set<std::size_t> AssignedUniqueColumns(
    const domain::UpdateNode& update, const TableSchema& schema,
    const std::set<std::size_t>& assigned_referenced) {
  std::set<std::size_t> unique_columns = assigned_referenced;
  for (std::size_t column_index : schema.unique_indexes) {
    if (IsAssigned(update, column_index)) {
      unique_columns.insert(column_index);
    }
  }
  return unique_columns;
}

using ComputeResult = std::variant<std::vector<RowUpdate>, ExecutionError>;

// Evaluates every assignment against the old row of every selected row, in scan
// order, and converts the value to the column type. Returns one RowUpdate per
// selected row, in the same order; new_row equals old_row where nothing
// changed. Takes ownership of selected.
ComputeResult ComputeNewRows(const domain::UpdateNode& update,
                             const TableSchema& schema,
                             std::vector<StoredRow>&& selected) {
  std::vector<RowUpdate> candidates;
  candidates.reserve(selected.size());
  for (StoredRow& stored : selected) {
    RowUpdate candidate{stored.rid, std::move(stored.row), Row()};
    candidate.new_row = candidate.old_row;
    for (const domain::BoundAssignment& assignment : update.assignments) {
      std::string failure;
      std::optional<Value> value =
          domain::Evaluate(*assignment.value, candidate.old_row, &failure);
      if (!value.has_value()) {
        return EvaluationError(std::move(failure));
      }
      const domain::ColumnDef& column = schema.columns[assignment.column_index];
      std::optional<Value> coerced = domain::CoerceValue(*value, column.type);
      if (!coerced.has_value()) {
        return MakeExecutionError(
            ErrorKind::kBind,
            "type mismatch assigning to " +
                QualifiedName(schema, assignment.column_index));
      }
      // A value equal to the stored one keeps the stored cell, so that -0.0
      // and 0.0, which compare equal, do not overwrite each other when another
      // cell of the row changes.
      Value& cell = candidate.new_row[assignment.column_index];
      if (*coerced != cell) {
        cell = std::move(*coerced);
      }
    }
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

// Tuple length of every row whose value changed, in scan order.
std::optional<ExecutionError> CheckChangedLengths(
    const TableSchema& schema, const std::vector<RowUpdate>& candidates) {
  for (const RowUpdate& candidate : candidates) {
    if (!IsRowChanged(candidate)) {
      continue;
    }
    if (auto error = CheckRowLength(schema, candidate.new_row)) {
      return error;
    }
  }
  return std::nullopt;
}

// Uniqueness on the final state of the table: for each unique column ascending,
// a changed value that appears more than once among the unselected rows and
// all selected rows (changed or not) after the update is a duplicate. Swapping
// two unique values is legal, and a duplicate that the statement did not touch
// is not reported. other_counts[i] belongs to unique_columns[i] and is
// consumed.
std::optional<ExecutionError> CheckChangedUniqueness(
    const TableSchema& schema, const std::vector<std::size_t>& unique_columns,
    std::vector<std::unordered_map<std::string, std::size_t>>* other_counts,
    const std::vector<RowUpdate>& candidates) {
  for (std::size_t i = 0; i < unique_columns.size(); ++i) {
    const std::size_t column_index = unique_columns[i];
    std::unordered_map<std::string, std::size_t> occurrences =
        std::move((*other_counts)[i]);
    for (const RowUpdate& candidate : candidates) {
      ++occurrences[ExactKey(candidate.new_row[column_index])];
    }
    for (const RowUpdate& candidate : candidates) {
      if (!IsCellChanged(candidate, column_index)) {
        continue;
      }
      const Value& value = candidate.new_row[column_index];
      if (occurrences[ExactKey(value)] > 1) {
        return DuplicateKeyError(schema, column_index, value);
      }
    }
  }
  return std::nullopt;
}

// Foreign key check for each assigned foreign key column whose value changed,
// in declaration order; the parent table is scanned once per such foreign key.
std::optional<ExecutionError> CheckChangedForeignKeys(
    const domain::ICatalog& catalog, const ITableReader& reader,
    const TableSchema& schema, const domain::UpdateNode& update,
    const std::vector<RowUpdate>& candidates) {
  for (const ForeignKey& foreign_key : schema.foreign_keys) {
    if (!IsAssigned(update, foreign_key.column_index)) {
      continue;
    }
    std::vector<const RowUpdate*> changed;
    for (const RowUpdate& candidate : candidates) {
      if (IsCellChanged(candidate, foreign_key.column_index)) {
        changed.push_back(&candidate);
      }
    }
    if (changed.empty()) {
      continue;
    }

    ParentLoad loaded = LoadParentKeys(catalog, reader, foreign_key);
    if (auto* error = std::get_if<ExecutionError>(&loaded)) {
      return std::move(*error);
    }
    const ParentKeys& parent = std::get<ParentKeys>(loaded);
    for (const RowUpdate* candidate : changed) {
      const Value& value = candidate->new_row[foreign_key.column_index];
      if (parent.keys.Contains(0, value)) {
        continue;
      }
      return NoMatchingRowError(schema, foreign_key, parent.schema, value);
    }
  }
  return std::nullopt;
}

// The rewrites planned so far for one child table of a cascade.
struct ChildStage {
  TableSchema schema;
  std::vector<RowUpdate> updates;
  // Position in updates of the row with each Rid, so a row that two foreign
  // keys rewrite becomes one RowUpdate.
  std::unordered_map<Rid, std::size_t> position_by_rid;
};

// The stage of the child table, created at the end of stages on first use. The
// reference is valid until the next call that creates a stage.
ChildStage& StageFor(const TableSchema& child,
                     std::vector<ChildStage>* stages) {
  const std::string child_key = domain::ToLower(child.name);
  for (ChildStage& stage : *stages) {
    if (domain::ToLower(stage.schema.name) == child_key) {
      return stage;
    }
  }
  stages->push_back(ChildStage{child, {}, {}});
  return stages->back();
}

// Old value to new value for the rows whose value in the column changed. When
// one old value maps to several new values, the first in scan order wins.
std::unordered_map<std::string, Value> BuildRenames(
    const std::vector<RowUpdate>& candidates, std::size_t column_index) {
  std::unordered_map<std::string, Value> renames;
  for (const RowUpdate& candidate : candidates) {
    if (!IsCellChanged(candidate, column_index)) {
      continue;
    }
    renames.emplace(ExactKey(candidate.old_row[column_index]),
                    candidate.new_row[column_index]);
  }
  return renames;
}

// Rewrites, in a ChildStage, every child row whose foreign key value is in the
// rename map. A value is looked up once, so A to B and B to C in one map never
// chains A to C. Borrows renames and stage for the scan.
class CascadeRewriter : public RowVisitor {
 public:
  CascadeRewriter(const std::unordered_map<std::string, Value>* renames,
                  std::size_t column_index, ChildStage* stage)
      : renames_(renames), column_index_(column_index), stage_(stage) {}

  std::optional<ExecutionError> Visit(StoredRow&& stored) override {
    auto rename = renames_->find(ExactKey(stored.row[column_index_]));
    if (rename == renames_->end()) {
      return std::nullopt;
    }
    auto staged = stage_->position_by_rid.find(stored.rid);
    if (staged != stage_->position_by_rid.end()) {
      stage_->updates[staged->second].new_row[column_index_] = rename->second;
      return std::nullopt;
    }
    RowUpdate update{stored.rid, stored.row, stored.row};
    update.new_row[column_index_] = rename->second;
    stage_->position_by_rid.emplace(stored.rid, stage_->updates.size());
    stage_->updates.push_back(std::move(update));
    return std::nullopt;
  }

 private:
  const std::unordered_map<std::string, Value>* renames_;
  std::size_t column_index_;
  ChildStage* stage_;
};

// The cascade: for each assigned referenced column (ascending) whose value
// changed in some row, every child table that references it (catalog order,
// foreign key declaration order) is scanned once and its rows are rewritten
// into *stages. Rows rewritten by a cascade do not count in `affected`.
std::optional<ExecutionError> PlanCascade(
    const ITableReader& reader, const std::vector<Reference>& references,
    const std::set<std::size_t>& referenced_columns,
    const std::vector<RowUpdate>& candidates, std::vector<ChildStage>* stages) {
  for (std::size_t column_index : referenced_columns) {
    const std::unordered_map<std::string, Value> renames =
        BuildRenames(candidates, column_index);
    if (renames.empty()) {
      continue;
    }
    for (const Reference& reference : references) {
      if (reference.foreign_key.ref_column_index != column_index) {
        continue;
      }
      ChildStage& stage = StageFor(reference.child, stages);
      CascadeRewriter rewriter(&renames, reference.foreign_key.column_index,
                               &stage);
      if (auto error = ScanRows(reader, reference.child.name, &rewriter)) {
        return error;
      }
    }
  }
  return std::nullopt;
}

// Tuple length of every cascaded child row, after all its rewrites are merged.
// The message names the child table.
std::optional<ExecutionError> CheckStagedLengths(
    const std::vector<ChildStage>& stages) {
  for (const ChildStage& stage : stages) {
    for (const RowUpdate& update : stage.updates) {
      if (auto error = CheckRowLength(stage.schema, update.new_row)) {
        return error;
      }
    }
  }
  return std::nullopt;
}

// The order of the checks is the order of design document 2.13 and 2.5: WHERE
// on every row (first pass), the assignments (second pass), then, on the rows
// whose value changed, tuple length, uniqueness, foreign keys, and finally the
// cascade with the tuple length of the rewritten child rows. The first pass
// reads through the index when the WHERE has a key range, unless an assigned
// column must stay unique: the uniqueness check counts the rows the WHERE does
// not select, so that statement reads the whole table.
PlanMutationResult PlanUpdate(const domain::UpdateNode& update,
                              const domain::ICatalog& catalog,
                              const ITableReader& reader,
                              const IIndexReader* index_reader) {
  std::optional<TableSchema> found = catalog.FindTable(update.table);
  if (!found.has_value()) {
    return UnknownTableError(update.table);
  }
  const TableSchema& schema = *found;
  const std::string target_key = domain::ToLower(schema.name);

  const std::vector<Reference> references = FindReferences(catalog, target_key);
  const std::set<std::size_t> referenced_columns =
      AssignedReferencedColumns(update, references);
  const std::set<std::size_t> unique_set =
      AssignedUniqueColumns(update, schema, referenced_columns);
  const std::vector<std::size_t> unique_columns(unique_set.begin(),
                                                unique_set.end());

  // First pass: one scan of the target.
  RowSelector selector(update.predicate.get(), unique_columns);
  if (auto error =
          ScanWhereRows(reader, unique_columns.empty() ? index_reader : nullptr,
                        schema.name, update.predicate.get(), &selector)) {
    return std::move(*error);
  }
  std::vector<StoredRow> selected = selector.TakeSelected();
  if (selected.empty()) {
    return MutationPlan{};
  }

  // Second pass: in memory.
  ComputeResult computed = ComputeNewRows(update, schema, std::move(selected));
  if (auto* error = std::get_if<ExecutionError>(&computed)) {
    return std::move(*error);
  }
  std::vector<RowUpdate> candidates =
      std::move(std::get<std::vector<RowUpdate>>(computed));
  const std::size_t affected = candidates.size();

  // No-op rule: when no row changes there is nothing to check, write or
  // cascade.
  if (std::ranges::none_of(candidates, IsRowChanged)) {
    MutationPlan unchanged;
    unchanged.affected = affected;
    return unchanged;
  }

  if (auto error = CheckChangedLengths(schema, candidates)) {
    return std::move(*error);
  }
  std::vector<std::unordered_map<std::string, std::size_t>> other_counts =
      selector.TakeOtherCounts();
  if (auto error = CheckChangedUniqueness(schema, unique_columns, &other_counts,
                                          candidates)) {
    return std::move(*error);
  }
  if (auto error = CheckChangedForeignKeys(catalog, reader, schema, update,
                                           candidates)) {
    return std::move(*error);
  }

  std::vector<ChildStage> stages;
  if (auto error = PlanCascade(reader, references, referenced_columns,
                               candidates, &stages)) {
    return std::move(*error);
  }
  if (auto error = CheckStagedLengths(stages)) {
    return std::move(*error);
  }

  MutationPlan plan;
  plan.affected = affected;
  TableChange target_change;
  target_change.table = target_key;
  for (RowUpdate& candidate : candidates) {
    if (IsRowChanged(candidate)) {
      target_change.updates.push_back(std::move(candidate));
    }
  }
  plan.changes.push_back(std::move(target_change));
  for (ChildStage& stage : stages) {
    if (stage.updates.empty()) {
      continue;
    }
    TableChange child_change;
    child_change.table = domain::ToLower(stage.schema.name);
    child_change.updates = std::move(stage.updates);
    plan.changes.push_back(std::move(child_change));
  }
  return plan;
}

// ----------------------------------------------------------------------------
// DELETE.
// ----------------------------------------------------------------------------

ExecutionError ReferencedError(const TableSchema& schema, const Row& row,
                               const Reference& reference) {
  return MakeExecutionError(
      ErrorKind::kConstraintViolation,
      QualifiedName(schema, schema.primary_key_index) + "=" +
          domain::FormatValue(row[schema.primary_key_index]) +
          " is referenced by " +
          QualifiedName(reference.child, reference.foreign_key.column_index));
}

// Scans the target to select rows, through the index when the WHERE has a key
// range, then scans each referencing child table once. The selected rows are
// checked in scan order and the references in catalog order and then
// declaration order; the first hit is reported. Nothing is scanned beyond the
// target when no row is selected.
PlanMutationResult PlanDelete(const domain::DeleteNode& delete_node,
                              const domain::ICatalog& catalog,
                              const ITableReader& reader,
                              const IIndexReader* index_reader) {
  std::optional<TableSchema> found = catalog.FindTable(delete_node.table);
  if (!found.has_value()) {
    return UnknownTableError(delete_node.table);
  }
  const TableSchema& schema = *found;

  RowSelector selector(delete_node.predicate.get(), {});
  if (auto error = ScanWhereRows(reader, index_reader, schema.name,
                                 delete_node.predicate.get(), &selector)) {
    return std::move(*error);
  }
  std::vector<StoredRow> selected = selector.TakeSelected();
  if (selected.empty()) {
    return MutationPlan{};
  }

  const std::vector<Reference> references =
      FindReferences(catalog, domain::ToLower(schema.name));
  std::vector<KeySets> child_keys;
  child_keys.reserve(references.size());
  for (const Reference& reference : references) {
    KeySets keys({reference.foreign_key.column_index});
    KeyCollector collector(&keys, std::nullopt);
    if (auto error = ScanRows(reader, reference.child.name, &collector)) {
      return std::move(*error);
    }
    child_keys.push_back(std::move(keys));
  }

  for (const StoredRow& stored : selected) {
    for (std::size_t i = 0; i < references.size(); ++i) {
      const Value& referenced_value =
          stored.row[references[i].foreign_key.ref_column_index];
      if (!child_keys[i].Contains(0, referenced_value)) {
        continue;
      }
      return ReferencedError(schema, stored.row, references[i]);
    }
  }

  const std::size_t affected = selected.size();
  TableChange change;
  change.table = domain::ToLower(schema.name);
  change.deletes.reserve(selected.size());
  for (StoredRow& stored : selected) {
    change.deletes.push_back(RowDelete{stored.rid, std::move(stored.row)});
  }
  MutationPlan plan;
  plan.changes.push_back(std::move(change));
  plan.affected = affected;
  return plan;
}

}  // namespace

PlanMutationResult PlanMutation(const domain::PlanNode& plan,
                                const domain::ICatalog& catalog,
                                const ITableReader& reader,
                                const IIndexReader* index_reader) {
  if (const auto* insert = std::get_if<domain::InsertNode>(&plan.node)) {
    return PlanInsert(*insert, catalog, reader, index_reader);
  }
  if (const auto* update = std::get_if<domain::UpdateNode>(&plan.node)) {
    return PlanUpdate(*update, catalog, reader, index_reader);
  }
  if (const auto* delete_node = std::get_if<domain::DeleteNode>(&plan.node)) {
    return PlanDelete(*delete_node, catalog, reader, index_reader);
  }
  return MakeExecutionError(ErrorKind::kUnsupported, "not a write plan node");
}

}  // namespace strata::paged
