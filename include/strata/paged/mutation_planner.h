#ifndef STRATA_PAGED_MUTATION_PLANNER_H_
#define STRATA_PAGED_MUTATION_PLANNER_H_

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/index_reader.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/row_stream.h"

// Constraint checks, cascades and the change set of INSERT, UPDATE and DELETE
// (design document 2.13). Pure: PlanMutation reads rows only through
// ITableReader and schemas only through domain::ICatalog, evaluates
// expressions with domain::Evaluate, and modifies nothing. The change set it
// returns is applied to the pages by HeapAccess::ApplyChangeSet, and only when
// every check has passed, so a rejected statement leaves no page changed.
//
// The rules are the contract of docs/DesignDocument.md 2.3 and 2.5, with the
// error kinds and messages verbatim. The flatfile executor implements the same
// rules separately (ADR-0039); the equivalence tests cross-check the two.
//
// Container cost (n = rows of the target table, m = rows of a parent table,
// c = rows of a child table, k = rows of an INSERT, r = rows a cascade
// rewrites), without an IIndexReader or for a WHERE that has no key range:
// INSERT scans the target once and each foreign key's parent table once, the
// first time a row needs it, O(n + m + k) with hash sets of ExactKey. UPDATE
// scans the target once, O(n), each parent of a changed foreign key once, and
// each child table once per foreign key that references an assigned column,
// O(c), producing O(r) RowUpdates. DELETE scans the target once and each
// referencing child table once, O(n + c). With an IIndexReader, a WHERE on the
// indexed key reads O(log n + s) index entries and s rows instead of the O(n)
// scan of the target, s being the rows of its key range.

namespace strata::paged {

// A row of the target or of a child table that is to be rewritten. old_row is
// the before-image that ApplyChangeSet undoes from.
struct RowUpdate {
  Rid rid;
  domain::Row old_row;
  domain::Row new_row;
};

struct RowDelete {
  Rid rid;
  domain::Row old_row;
};

// The changes one statement makes to one table. `table` is the lowercase table
// name. One TableChange of a statement holds one kind of change.
struct TableChange {
  std::string table;
  std::vector<domain::Row> inserts;
  std::vector<RowUpdate> updates;
  std::vector<RowDelete> deletes;
};

// What a write statement does to the pages. `changes` is in the order of
// application: the target table first, then the cascaded child tables. It is
// empty when nothing changes (no row selected, or every assignment equals the
// stored value), and `affected` can still be greater than 0 then. `affected`
// is the number of rows the statement reports: the inserted rows, or the rows
// selected by WHERE, not counting rows a cascade rewrites. A MutationPlan owns
// copies of all its rows.
struct MutationPlan {
  std::vector<TableChange> changes;
  std::size_t affected = 0;
};

using PlanMutationResult = std::variant<MutationPlan, domain::ExecutionError>;

// Plans an InsertNode, UpdateNode or DeleteNode:
//   * every other node yields kUnsupported "not a write plan node";
//   * a table that the catalog does not have yields kBind "unknown table:
//     <name>" with the name as written in the node;
//   * a table the catalog has but the reader cannot scan yields the error of
//     reader.Scan;
//   * a violated rule yields the error of docs/DesignDocument.md section
//     2.5, the first one in the order given there: kRuntime for a failed
//     evaluation or an auto-increment overflow, kBind for a type that an
//     UPDATE cannot convert, kUnsupported for a row longer than kMaxTupleSize
//     bytes (checked before uniqueness and foreign keys), kConstraintViolation
//     for a duplicate key, a missing parent row or a referenced row to delete.
// With a non-null index_reader, an INSERT into a table whose primary key is
// indexed, with no UNIQUE column and no generated value, does not scan the
// target: each row asks IIndexReader::Contains whether its key exists, and a
// foreign key whose target is an indexed primary key is checked the same way.
// The plan and the errors, messages included, are those of the scan. UPDATE and
// DELETE select the rows of a WHERE that has a key range (ExtractKeyRange) with
// IIndexReader::ScanRange, so the rows come in ascending key order and a row
// outside the range is not read; an UPDATE that assigns a UNIQUE or referenced
// column, and every other WHERE, read the whole table through reader.
// Borrows plan, catalog, reader and index_reader for the call. Precondition:
// plan is a plan that the Binder produced (each InsertNode row has one value
// per column, of the column type; an assignment never targets the primary
// key), and the streams of the reader yield rows with one value per column of
// the schema.
PlanMutationResult PlanMutation(const domain::PlanNode& plan,
                                const domain::ICatalog& catalog,
                                const ITableReader& reader,
                                const IIndexReader* index_reader = nullptr);

}  // namespace strata::paged

#endif  // STRATA_PAGED_MUTATION_PLANNER_H_
