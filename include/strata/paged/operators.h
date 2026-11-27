#ifndef STRATA_PAGED_OPERATORS_H_
#define STRATA_PAGED_OPERATORS_H_

#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/index_reader.h"
#include "strata/paged/row_stream.h"

// Pull-based query operators (design document 2.13). Pure: an operator
// reads rows only from its child operator or, at a leaf, from an IRowStream,
// and evaluates expressions with domain::Evaluate. It touches no page, pin or
// lock.
//
// Lifecycle of every operator (the state diagram of design document 2.13):
//   Fresh: no row has been requested from the child yet. A blocking operator
//     (Sort, Aggregate, the right side of a join) reads its child to the end
//     only on the first Next, so building the tree reads no row.
//   Streaming: at least one row has been returned.
//   Done: a no-value result or an error has been returned. Every later Next
//     returns no value and requests nothing from the child.
//
// Ownership: BuildOperatorTree takes streams from the reader and gives the
// caller the root operator, which owns its child operators and, through the
// scans, the streams. An operator borrows the BoundExpr nodes inside the plan
// by reference, so the plan must outlive the operator tree. The reader is only
// used while the tree is built; the streams must not outlive it.

namespace strata::paged {

// A row, the end of the rows (the optional has no value), or the error that
// ended the operator. The same semantics as StreamResult.
using OperatorResult =
    std::variant<std::optional<domain::Row>, domain::ExecutionError>;

class IOperator {
 public:
  virtual ~IOperator() = default;

  // The next row, moved to the caller. The caller stops at the first error.
  // Used by one thread only.
  virtual OperatorResult Next() = 0;

  // The output columns. For a join, the left columns followed by the right
  // columns.
  virtual std::vector<domain::ColumnDef> Columns() const = 0;
};

// The root operator, or the error that prevented building the tree.
using BuildResult =
    std::variant<std::unique_ptr<IOperator>, domain::ExecutionError>;

// Builds the operator tree of a query plan: ScanNode, FilterNode, JoinNode,
// AggregateNode, ProjectNode, SortNode and LimitNode. Any other node yields
// kUnsupported "not a query plan node". A ScanNode whose table does not exist
// yields the error of reader.Scan (kBind "unknown table: <name>"). Reads no
// data row. A JoinNode picks HashJoinOperator when its condition has a
// conjunct that equates one left column with one right column (ADR-0011), and
// NestedLoopJoinOperator otherwise.
// With a non-null index_reader, a FilterNode directly over a ScanNode of a
// table with an index reads through IIndexReader::ScanRange when
// ExtractKeyRange finds a range in its predicate (ADR-0052); the filter stays
// and tests the whole predicate. A null index_reader reads only through reader.
// Precondition: plan is a plan that the Binder produced, so column indexes are
// inside the rows they are evaluated against and sort keys are inside the
// output of their child.
BuildResult BuildOperatorTree(const domain::PlanNode& plan,
                              const ITableReader& reader,
                              const IIndexReader* index_reader = nullptr);

}  // namespace strata::paged

#endif  // STRATA_PAGED_OPERATORS_H_
