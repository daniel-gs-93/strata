#include "strata/paged/operators.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/expression.h"
#include "strata/paged/key_range.h"
#include "strata/paged/row_keys.h"

// Pure logic: every operator reads rows from its child (or, at a leaf, from an
// IRowStream) and evaluates domain expressions. It never touches a page.
//
// Container costs (n = rows read from the child, m = rows of the right side of
// a join, g = groups):
//   Scan, Filter, Project, Limit: O(1) work and O(1) memory per row (Limit
//   skips offset rows once). Hash join: the right side is read into a
//   std::vector<Row> and an unordered_map from join key to row positions, O(m)
//   time and memory; a probe is O(1) on average plus the size of the bucket.
//   Nested-loop join: the same vector plus one position list, O(n * m)
//   predicate evaluations. Aggregate: one unordered_map lookup per row, O(n)
//   time and O(g) memory. Sort: std::stable_sort, O(n log n) time, O(n)
//   memory.

namespace strata::paged {

namespace {

using domain::BoundBinary;
using domain::BoundColumn;
using domain::BoundExpr;
using domain::BoundExprPtr;
using domain::ColumnDef;
using domain::DataType;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::PlanNode;
using domain::PlanPtr;
using domain::Row;
using domain::Value;

// ----------------------------------------------------------------------------
// Results.
// ----------------------------------------------------------------------------

OperatorResult EndOfRows() { return OperatorResult(std::optional<Row>()); }

OperatorResult RowResult(Row row) {
  return OperatorResult(std::optional<Row>(std::move(row)));
}

OperatorResult ErrorResult(ExecutionError error) {
  return OperatorResult(std::move(error));
}

// The kRuntime error for a failed evaluation; failure is the message that
// domain::Evaluate or domain::EvaluateCondition wrote.
ExecutionError RuntimeError(std::string failure) {
  return ExecutionError{ErrorKind::kRuntime, std::move(failure)};
}

ExecutionError NumericOverflowError() {
  return RuntimeError("numeric overflow");
}

const char* AggregateFnName(domain::AggregateFn fn) {
  switch (fn) {
    case domain::AggregateFn::kCount:
      return "COUNT";
    case domain::AggregateFn::kSum:
      return "SUM";
    case domain::AggregateFn::kAvg:
      return "AVG";
    case domain::AggregateFn::kMin:
      return "MIN";
    case domain::AggregateFn::kMax:
      break;
  }
  return "MAX";
}

// The row slot of a result that holds a row or the end; null when the result
// holds an error.
std::optional<Row>* RowSlotOf(OperatorResult* result) {
  return std::get_if<std::optional<Row>>(result);
}

// Pulls child to the end and appends its rows to *rows, in order. Returns the
// error that ended the child, if any; *rows then holds the rows read before
// it.
std::optional<ExecutionError> ReadAll(IOperator* child,
                                      std::vector<Row>* rows) {
  while (true) {
    OperatorResult pulled = child->Next();
    if (auto* error = std::get_if<ExecutionError>(&pulled)) {
      return std::move(*error);
    }
    std::optional<Row>& row = std::get<std::optional<Row>>(pulled);
    if (!row.has_value()) {
      return std::nullopt;
    }
    rows->push_back(std::move(*row));
  }
}

// ----------------------------------------------------------------------------
// Lifecycle.
// ----------------------------------------------------------------------------

// Fresh, Streaming, Done (design document 2.13). Next is final so that no
// operator can leave the lifecycle: a Done operator answers with the end of
// rows without calling Produce, and therefore never asks its child for a row
// again.
class LifecycleOperator : public IOperator {
 public:
  OperatorResult Next() final {
    if (state_ == State::kDone) {
      return EndOfRows();
    }
    OperatorResult result = Produce();
    state_ = StateAfter(&result);
    return result;
  }

 protected:
  // True until the first Next has returned: the operator has not yet asked
  // its child for a row.
  bool is_fresh() const { return state_ == State::kFresh; }

  // The next result, called only while the operator is not Done.
  virtual OperatorResult Produce() = 0;

 private:
  enum class State { kFresh, kStreaming, kDone };

  static State StateAfter(OperatorResult* result) {
    std::optional<Row>* slot = RowSlotOf(result);
    if (slot != nullptr && slot->has_value()) {
      return State::kStreaming;
    }
    return State::kDone;
  }

  State state_ = State::kFresh;
};

// ----------------------------------------------------------------------------
// Streaming operators.
// ----------------------------------------------------------------------------

class ScanOperator final : public LifecycleOperator {
 public:
  explicit ScanOperator(std::unique_ptr<IRowStream> stream)
      : stream_(std::move(stream)) {}

  std::vector<ColumnDef> Columns() const override { return stream_->Columns(); }

 protected:
  OperatorResult Produce() override {
    StreamResult result = stream_->Next();
    if (auto* error = std::get_if<ExecutionError>(&result)) {
      return ErrorResult(std::move(*error));
    }
    std::optional<StoredRow>& stored =
        std::get<std::optional<StoredRow>>(result);
    if (!stored.has_value()) {
      return EndOfRows();
    }
    return RowResult(std::move(stored->row));
  }

 private:
  std::unique_ptr<IRowStream> stream_;
};

class FilterOperator final : public LifecycleOperator {
 public:
  // predicate is borrowed from the plan.
  FilterOperator(std::unique_ptr<IOperator> child, const BoundExpr* predicate)
      : child_(std::move(child)), predicate_(predicate) {}

  std::vector<ColumnDef> Columns() const override { return child_->Columns(); }

 protected:
  OperatorResult Produce() override {
    while (true) {
      OperatorResult pulled = child_->Next();
      std::optional<Row>* slot = RowSlotOf(&pulled);
      // An error or the end of rows passes up unchanged.
      if (slot == nullptr || !slot->has_value()) {
        return pulled;
      }
      std::string failure;
      std::optional<bool> is_match =
          domain::EvaluateCondition(*predicate_, **slot, &failure);
      if (!is_match.has_value()) {
        return ErrorResult(RuntimeError(std::move(failure)));
      }
      if (*is_match) {
        return pulled;
      }
    }
  }

 private:
  std::unique_ptr<IOperator> child_;
  const BoundExpr* predicate_;
};

class ProjectOperator final : public LifecycleOperator {
 public:
  // project is borrowed from the plan.
  ProjectOperator(std::unique_ptr<IOperator> child,
                  const domain::ProjectNode* project)
      : child_(std::move(child)), project_(project) {}

  std::vector<ColumnDef> Columns() const override {
    return project_->output_columns;
  }

 protected:
  OperatorResult Produce() override {
    OperatorResult pulled = child_->Next();
    std::optional<Row>* slot = RowSlotOf(&pulled);
    if (slot == nullptr || !slot->has_value()) {
      return pulled;
    }
    Row output;
    output.reserve(project_->exprs.size());
    for (const BoundExprPtr& expr : project_->exprs) {
      std::string failure;
      std::optional<Value> value = domain::Evaluate(*expr, **slot, &failure);
      if (!value.has_value()) {
        return ErrorResult(RuntimeError(std::move(failure)));
      }
      output.push_back(std::move(*value));
    }
    return RowResult(std::move(output));
  }

 private:
  std::unique_ptr<IOperator> child_;
  const domain::ProjectNode* project_;
};

// Skips the first offset rows on the first Next, then returns at most limit
// rows. Once limit rows have been returned, and when limit is 0, it asks the
// child for nothing.
class LimitOperator final : public LifecycleOperator {
 public:
  LimitOperator(std::unique_ptr<IOperator> child, std::optional<int64_t> limit,
                int64_t offset)
      : child_(std::move(child)), remaining_(limit), offset_(offset) {}

  std::vector<ColumnDef> Columns() const override { return child_->Columns(); }

 protected:
  OperatorResult Produce() override {
    if (HasReturnedEnough()) {
      return EndOfRows();
    }
    if (is_fresh()) {
      std::optional<OperatorResult> ended = SkipOffset();
      if (ended.has_value()) {
        return std::move(*ended);
      }
    }
    OperatorResult pulled = child_->Next();
    std::optional<Row>* slot = RowSlotOf(&pulled);
    if (slot != nullptr && slot->has_value() && remaining_.has_value()) {
      --*remaining_;
    }
    return pulled;
  }

 private:
  // offset and limit are not negative (the parser accepts no sign); a limit
  // that is not positive is treated as 0.
  bool HasReturnedEnough() const {
    return remaining_.has_value() && *remaining_ <= 0;
  }

  // Pulls and drops offset rows. Returns the result to hand up when the child
  // ends or fails before the offset is reached; no value otherwise.
  std::optional<OperatorResult> SkipOffset() {
    for (int64_t skipped = 0; skipped < offset_; ++skipped) {
      OperatorResult pulled = child_->Next();
      std::optional<Row>* slot = RowSlotOf(&pulled);
      if (slot == nullptr || !slot->has_value()) {
        return pulled;
      }
    }
    return std::nullopt;
  }

  std::unique_ptr<IOperator> child_;
  std::optional<int64_t> remaining_;  // rows still to return; no value: no cap
  int64_t offset_;
};

// ----------------------------------------------------------------------------
// Joins.
// ----------------------------------------------------------------------------

// Positions of the two columns of an equality join condition, the second
// relative to the start of the right row.
struct EquiColumns {
  std::size_t left_index;
  std::size_t right_index;
};

// condition is one conjunct. Returns the columns when it is an equality
// between one left column and one right column, in either operand order.
std::optional<EquiColumns> FindEquiColumns(const BoundExpr& condition,
                                           std::size_t left_width,
                                           std::size_t right_width) {
  const auto* binary = std::get_if<BoundBinary>(&condition.node);
  if (binary == nullptr || binary->op != domain::BinaryOp::kEq) {
    return std::nullopt;
  }
  const auto* lhs = std::get_if<BoundColumn>(&binary->lhs->node);
  const auto* rhs = std::get_if<BoundColumn>(&binary->rhs->node);
  if (lhs == nullptr || rhs == nullptr) {
    return std::nullopt;
  }

  const std::size_t total_width = left_width + right_width;
  if (lhs->index >= total_width || rhs->index >= total_width) {
    return std::nullopt;
  }
  if (lhs->index < left_width && rhs->index >= left_width) {
    return EquiColumns{lhs->index, rhs->index - left_width};
  }
  if (rhs->index < left_width && lhs->index >= left_width) {
    return EquiColumns{rhs->index, lhs->index - left_width};
  }
  return std::nullopt;
}

// Appends the operands of a top-level AND chain, left to right. expr is
// borrowed and must outlive conjuncts.
void CollectConjuncts(const BoundExpr& expr,
                      std::vector<const BoundExpr*>* conjuncts) {
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  if (binary == nullptr || binary->op != domain::BinaryOp::kAnd) {
    conjuncts->push_back(&expr);
    return;
  }
  CollectConjuncts(*binary->lhs, conjuncts);
  CollectConjuncts(*binary->rhs, conjuncts);
}

// How one join runs: an optional hash key and the conditions that every pair
// must satisfy besides it. The conditions are borrowed from the plan.
struct JoinSpec {
  std::optional<EquiColumns> equi_columns;
  std::vector<const BoundExpr*> residuals;
};

// ON is split on AND; the first conjunct that equates a left column with a
// right column is the hash key and every other conjunct is a residual, in
// order (ADR-0011). condition may be null (a join without ON).
JoinSpec MakeJoinSpec(const BoundExpr* condition, std::size_t left_width,
                      std::size_t right_width) {
  JoinSpec spec;
  std::vector<const BoundExpr*> conjuncts;
  if (condition != nullptr) {
    CollectConjuncts(*condition, &conjuncts);
  }
  for (const BoundExpr* conjunct : conjuncts) {
    if (!spec.equi_columns.has_value()) {
      spec.equi_columns = FindEquiColumns(*conjunct, left_width, right_width);
      if (spec.equi_columns.has_value()) {
        continue;
      }
    }
    spec.residuals.push_back(conjunct);
  }
  return spec;
}

// No value when a condition fails to evaluate (*failure holds the message);
// otherwise whether every condition holds, evaluated in order.
std::optional<bool> HoldsAll(const std::vector<const BoundExpr*>& conditions,
                             const Row& row, std::string* failure) {
  for (const BoundExpr* condition : conditions) {
    std::optional<bool> holds =
        domain::EvaluateCondition(*condition, row, failure);
    if (!holds.has_value()) {
      return std::nullopt;
    }
    if (!*holds) {
      return false;
    }
  }
  return true;
}

// The part that the two joins share. The first Next reads the whole right side
// into memory; after that every left row is pulled once, and the right rows
// that the subclass offers as candidates are paired with it in right order.
// A pair is returned when every residual condition holds. The output follows
// the order of left, and the pairs of one left row follow the order of right.
class JoinOperatorBase : public LifecycleOperator {
 public:
  std::vector<ColumnDef> Columns() const override { return columns_; }

 protected:
  // residuals are borrowed from the plan.
  JoinOperatorBase(std::unique_ptr<IOperator> left,
                   std::unique_ptr<IOperator> right,
                   std::vector<const BoundExpr*> residuals)
      : left_(std::move(left)),
        right_(std::move(right)),
        residuals_(std::move(residuals)),
        columns_(left_->Columns()),
        left_width_(columns_.size()) {
    std::vector<ColumnDef> right_columns = right_->Columns();
    columns_.insert(columns_.end(), right_columns.begin(), right_columns.end());
    combined_.resize(columns_.size());
  }

  OperatorResult Produce() final {
    if (is_fresh()) {
      if (auto error = ReadAll(right_.get(), &right_rows_)) {
        return ErrorResult(std::move(*error));
      }
      IndexRightRows();
    }
    while (true) {
      if (!has_left_row_) {
        std::optional<OperatorResult> ended = PullLeftRow();
        if (ended.has_value()) {
          return std::move(*ended);
        }
      }
      std::optional<OperatorResult> pair = NextPairOfLeftRow();
      if (pair.has_value()) {
        return std::move(*pair);
      }
      has_left_row_ = false;
    }
  }

  // Called once, after the right side has been read; right_rows() is final.
  virtual void IndexRightRows() = 0;

  // The positions in right_rows() to try for the current left row, in
  // ascending order. left_row holds the left row in its first columns. The
  // result stays valid until the next call.
  virtual const std::vector<std::size_t>& CandidatesFor(
      const Row& left_row) = 0;

  // Whether the join key of the candidate equals the join key of the left row.
  virtual bool KeysMatch(const Row& left_row, const Row& right_row) const = 0;

  const std::vector<Row>& right_rows() const { return right_rows_; }

 private:
  // Pulls the next left row into the left half of combined_ and starts its
  // candidates. Returns the result to hand up when left ends or fails.
  std::optional<OperatorResult> PullLeftRow() {
    OperatorResult pulled = left_->Next();
    std::optional<Row>* slot = RowSlotOf(&pulled);
    if (slot == nullptr || !slot->has_value()) {
      return pulled;
    }
    Row& left_row = **slot;
    assert(left_row.size() == left_width_);
    std::ranges::move(left_row, combined_.begin());
    candidates_ = &CandidatesFor(combined_);
    candidate_index_ = 0;
    has_left_row_ = true;
    return std::nullopt;
  }

  // The next pair of the current left row that passes the key check and every
  // residual, as a result to hand up. No value when the candidates are used
  // up.
  std::optional<OperatorResult> NextPairOfLeftRow() {
    while (candidate_index_ < candidates_->size()) {
      const Row& right_row = right_rows_[(*candidates_)[candidate_index_]];
      ++candidate_index_;
      if (!KeysMatch(combined_, right_row)) {
        continue;
      }
      std::ranges::copy(
          right_row,
          combined_.begin() + static_cast<std::ptrdiff_t>(left_width_));
      std::string failure;
      std::optional<bool> holds = HoldsAll(residuals_, combined_, &failure);
      if (!holds.has_value()) {
        return ErrorResult(RuntimeError(std::move(failure)));
      }
      if (*holds) {
        return RowResult(combined_);
      }
    }
    return std::nullopt;
  }

  std::unique_ptr<IOperator> left_;
  std::unique_ptr<IOperator> right_;
  std::vector<const BoundExpr*> residuals_;
  std::vector<ColumnDef> columns_;  // left columns, then right columns
  std::size_t left_width_;
  std::vector<Row> right_rows_;
  // The scratch row that residuals are evaluated on and that a result is
  // copied from: the left row, then the current right row.
  Row combined_;
  bool has_left_row_ = false;
  const std::vector<std::size_t>* candidates_ = nullptr;
  std::size_t candidate_index_ = 0;
};

// Right-row positions per join key, each list ascending.
using JoinBuckets = std::unordered_map<std::string, std::vector<std::size_t>>;

// A join with an equality key: the right side is bucketed by JoinKey, and a
// left row probes only its own bucket. Two large INT values can share a double
// key, so a bucket hit is confirmed with ValuesEqual.
class HashJoinOperator final : public JoinOperatorBase {
 public:
  HashJoinOperator(std::unique_ptr<IOperator> left,
                   std::unique_ptr<IOperator> right, EquiColumns equi_columns,
                   std::vector<const BoundExpr*> residuals)
      : JoinOperatorBase(std::move(left), std::move(right),
                         std::move(residuals)),
        equi_columns_(equi_columns) {}

 protected:
  void IndexRightRows() override {
    const std::vector<Row>& rows = right_rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      buckets_[JoinKey(rows[i][equi_columns_.right_index])].push_back(i);
    }
  }

  const std::vector<std::size_t>& CandidatesFor(const Row& left_row) override {
    auto bucket = buckets_.find(JoinKey(left_row[equi_columns_.left_index]));
    return bucket == buckets_.end() ? no_positions_ : bucket->second;
  }

  bool KeysMatch(const Row& left_row, const Row& right_row) const override {
    return domain::ValuesEqual(left_row[equi_columns_.left_index],
                               right_row[equi_columns_.right_index]);
  }

 private:
  EquiColumns equi_columns_;
  JoinBuckets buckets_;
  const std::vector<std::size_t> no_positions_;
};

// A join without an equality key: every left row is paired with every right
// row, and the residual conditions (all of ON) decide.
class NestedLoopJoinOperator final : public JoinOperatorBase {
 public:
  NestedLoopJoinOperator(std::unique_ptr<IOperator> left,
                         std::unique_ptr<IOperator> right,
                         std::vector<const BoundExpr*> residuals)
      : JoinOperatorBase(std::move(left), std::move(right),
                         std::move(residuals)) {}

 protected:
  void IndexRightRows() override {
    every_position_.resize(right_rows().size());
    std::iota(every_position_.begin(), every_position_.end(), std::size_t{0});
  }

  const std::vector<std::size_t>& CandidatesFor(
      const Row& /*left_row*/) override {
    return every_position_;
  }

  bool KeysMatch(const Row& /*left_row*/,
                 const Row& /*right_row*/) const override {
    return true;
  }

 private:
  std::vector<std::size_t> every_position_;
};

// ----------------------------------------------------------------------------
// Blocking operators.
// ----------------------------------------------------------------------------

struct Group {
  Row keys;
  std::vector<domain::AggregateAccumulator> accumulators;
};

// Reads the child to the end on the first Next, groups the rows by the exact
// key of the group-key values, and returns one row per group in order of first
// appearance. Without group keys there is exactly one group, also for empty
// input.
class AggregateOperator final : public LifecycleOperator {
 public:
  // aggregate is borrowed from the plan.
  AggregateOperator(std::unique_ptr<IOperator> child,
                    const domain::AggregateNode* aggregate)
      : child_(std::move(child)),
        aggregate_(aggregate),
        columns_(MakeColumns(*aggregate)),
        prototypes_(MakeAccumulators(*aggregate)) {}

  std::vector<ColumnDef> Columns() const override { return columns_; }

 protected:
  OperatorResult Produce() override {
    if (is_fresh()) {
      if (auto error = ComputeOutputRows()) {
        return ErrorResult(std::move(*error));
      }
    }
    if (next_output_ >= output_rows_.size()) {
      return EndOfRows();
    }
    return RowResult(std::move(output_rows_[next_output_++]));
  }

 private:
  // group_key_<i> then aggregate_<i>, as the plan documents.
  static std::vector<ColumnDef> MakeColumns(
      const domain::AggregateNode& aggregate) {
    std::vector<ColumnDef> columns;
    for (std::size_t i = 0; i < aggregate.group_keys.size(); ++i) {
      columns.push_back(ColumnDef{"group_key_" + std::to_string(i),
                                  DataTypeOf(aggregate.group_keys[i]->type)});
    }
    for (std::size_t i = 0; i < aggregate.aggregates.size(); ++i) {
      const domain::AggregateSpec& spec = aggregate.aggregates[i];
      DataType type = spec.arg ? DataTypeOf(spec.arg->type) : DataType::kInt;
      if (spec.fn == domain::AggregateFn::kCount) {
        type = DataType::kInt;
      }
      if (spec.fn == domain::AggregateFn::kAvg) {
        type = DataType::kDouble;
      }
      columns.push_back(ColumnDef{"aggregate_" + std::to_string(i), type});
    }
    return columns;
  }

  static std::vector<domain::AggregateAccumulator> MakeAccumulators(
      const domain::AggregateNode& aggregate) {
    std::vector<domain::AggregateAccumulator> accumulators;
    for (const domain::AggregateSpec& spec : aggregate.aggregates) {
      // COUNT(*) has no argument; the type is only read for SUM.
      const DataType argument_type =
          spec.arg ? DataTypeOf(spec.arg->type) : DataType::kInt;
      accumulators.emplace_back(spec.fn, argument_type);
    }
    return accumulators;
  }

  // Reads the child to the end and fills output_rows_. Returns the error that
  // ended the child, an evaluation failure, an overflow, or "<FN> over no
  // rows".
  std::optional<ExecutionError> ComputeOutputRows() {
    std::vector<Group> groups;
    // Maps the exact key of a group's key values to its index in groups.
    std::unordered_map<std::string, std::size_t> group_positions;
    const bool has_group_keys = !aggregate_->group_keys.empty();
    if (!has_group_keys) {
      groups.push_back(Group{Row(), prototypes_});
    }

    while (true) {
      OperatorResult pulled = child_->Next();
      if (auto* error = std::get_if<ExecutionError>(&pulled)) {
        return std::move(*error);
      }
      std::optional<Row>& row = std::get<std::optional<Row>>(pulled);
      if (!row.has_value()) {
        break;
      }
      std::size_t position = 0;
      if (has_group_keys) {
        std::optional<ExecutionError> error =
            FindOrAddGroup(*row, &groups, &group_positions, &position);
        if (error.has_value()) {
          return error;
        }
      }
      if (auto error = AddRowToGroup(*row, &groups[position])) {
        return error;
      }
    }

    if (auto error = CheckGroupsHaveResults(groups)) {
      return error;
    }
    output_rows_.reserve(groups.size());
    for (Group& group : groups) {
      Row output = std::move(group.keys);
      for (const domain::AggregateAccumulator& accumulator :
           group.accumulators) {
        output.push_back(accumulator.Result());
      }
      output_rows_.push_back(std::move(output));
    }
    return std::nullopt;
  }

  // Evaluates the group keys of row and sets *position to the index of its
  // group, adding the group when its key has not been seen.
  std::optional<ExecutionError> FindOrAddGroup(
      const Row& row, std::vector<Group>* groups,
      std::unordered_map<std::string, std::size_t>* group_positions,
      std::size_t* position) const {
    Row keys;
    std::string key_text;
    for (const BoundExprPtr& key_expr : aggregate_->group_keys) {
      std::string failure;
      std::optional<Value> key = domain::Evaluate(*key_expr, row, &failure);
      if (!key.has_value()) {
        return RuntimeError(std::move(failure));
      }
      AppendExactKey(*key, &key_text);
      keys.push_back(std::move(*key));
    }
    auto entry = group_positions->emplace(std::move(key_text), groups->size());
    if (entry.second) {
      groups->push_back(Group{std::move(keys), prototypes_});
    }
    *position = entry.first->second;
    return std::nullopt;
  }

  // Adds row to every accumulator of *group. Fails when an argument cannot be
  // evaluated or a running sum overflows.
  std::optional<ExecutionError> AddRowToGroup(const Row& row,
                                              Group* group) const {
    for (std::size_t i = 0; i < aggregate_->aggregates.size(); ++i) {
      const domain::AggregateSpec& spec = aggregate_->aggregates[i];
      if (!spec.arg) {
        // COUNT(*) cannot fail.
        group->accumulators[i].Add(Value(int64_t{0}));
        continue;
      }
      std::string failure;
      std::optional<Value> argument =
          domain::Evaluate(*spec.arg, row, &failure);
      if (!argument.has_value()) {
        return RuntimeError(std::move(failure));
      }
      if (!group->accumulators[i].Add(*argument)) {
        return NumericOverflowError();
      }
    }
    return std::nullopt;
  }

  // AVG, MIN and MAX have no result for a group without rows. Only the single
  // group of a query without GROUP BY can be empty.
  std::optional<ExecutionError> CheckGroupsHaveResults(
      const std::vector<Group>& groups) const {
    for (const Group& group : groups) {
      for (std::size_t i = 0; i < group.accumulators.size(); ++i) {
        const domain::AggregateFn fn = aggregate_->aggregates[i].fn;
        const bool has_no_result_when_empty = fn == domain::AggregateFn::kAvg ||
                                              fn == domain::AggregateFn::kMin ||
                                              fn == domain::AggregateFn::kMax;
        if (has_no_result_when_empty && group.accumulators[i].IsEmpty()) {
          return RuntimeError(std::string(AggregateFnName(fn)) +
                              " over no rows");
        }
      }
    }
    return std::nullopt;
  }

  std::unique_ptr<IOperator> child_;
  const domain::AggregateNode* aggregate_;
  std::vector<ColumnDef> columns_;
  // One empty accumulator per aggregate, copied for each new group.
  std::vector<domain::AggregateAccumulator> prototypes_;
  std::vector<Row> output_rows_;
  std::size_t next_output_ = 0;
};

// Strict weak ordering over the sort keys; rows that compare equal keep their
// input order because the caller uses std::stable_sort.
class RowOrder {
 public:
  // keys is borrowed from the plan.
  explicit RowOrder(const std::vector<domain::SortKey>* keys) : keys_(keys) {}

  bool operator()(const Row& lhs, const Row& rhs) const {
    for (const domain::SortKey& key : *keys_) {
      const int ordering =
          domain::CompareValues(lhs[key.column_index], rhs[key.column_index]);
      if (ordering == 0) {
        continue;
      }
      return key.is_descending ? ordering > 0 : ordering < 0;
    }
    return false;
  }

 private:
  const std::vector<domain::SortKey>* keys_;
};

// Reads the child to the end on the first Next, sorts the rows with
// std::stable_sort, and returns them one at a time.
class SortOperator final : public LifecycleOperator {
 public:
  // sort is borrowed from the plan.
  SortOperator(std::unique_ptr<IOperator> child, const domain::SortNode* sort)
      : child_(std::move(child)), sort_(sort) {}

  std::vector<ColumnDef> Columns() const override { return child_->Columns(); }

 protected:
  OperatorResult Produce() override {
    if (is_fresh()) {
      if (auto error = ReadAll(child_.get(), &rows_)) {
        return ErrorResult(std::move(*error));
      }
      std::ranges::stable_sort(rows_, RowOrder(&sort_->keys));
    }
    if (next_row_ >= rows_.size()) {
      return EndOfRows();
    }
    return RowResult(std::move(rows_[next_row_++]));
  }

 private:
  std::unique_ptr<IOperator> child_;
  const domain::SortNode* sort_;
  std::vector<Row> rows_;
  std::size_t next_row_ = 0;
};

// ----------------------------------------------------------------------------
// Building the tree.
// ----------------------------------------------------------------------------

BuildResult Built(std::unique_ptr<IOperator> root) {
  return BuildResult(std::move(root));
}

// What building a tree reads from: the table reader and, when the engine has
// indexes, the index reader (null otherwise).
struct BuildContext {
  const ITableReader& reader;
  const IIndexReader* index_reader;
};

BuildResult BuildNode(const PlanNode& plan, const BuildContext& context);

// Builds the operator of the child plan into *child. Returns the error when
// the child cannot be built.
std::optional<ExecutionError> BuildChild(const PlanPtr& plan,
                                         const BuildContext& context,
                                         std::unique_ptr<IOperator>* child) {
  BuildResult built = BuildNode(*plan, context);
  if (auto* error = std::get_if<ExecutionError>(&built)) {
    return std::move(*error);
  }
  *child = std::move(std::get<std::unique_ptr<IOperator>>(built));
  return std::nullopt;
}

BuildResult BuildScan(const domain::ScanNode& scan,
                      const BuildContext& context) {
  ScanResult opened = context.reader.Scan(scan.table);
  if (auto* error = std::get_if<ExecutionError>(&opened)) {
    return std::move(*error);
  }
  return Built(std::make_unique<ScanOperator>(
      std::move(std::get<std::unique_ptr<IRowStream>>(opened))));
}

// A filter directly over the scan of a table with an index reads through the
// index when its predicate gives a key range (ADR-0052). The filter stays above
// the scan and tests the whole predicate on every row, so the range only
// narrows the rows. No value means the filter is built the ordinary way.
std::optional<BuildResult> BuildIndexScan(const domain::FilterNode& filter,
                                          const BuildContext& context) {
  if (context.index_reader == nullptr) {
    return std::nullopt;
  }
  const auto* scan = std::get_if<domain::ScanNode>(&filter.child->node);
  if (scan == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::size_t> column =
      context.index_reader->IndexedColumn(scan->table);
  if (!column.has_value()) {
    return std::nullopt;
  }
  const std::optional<KeyRange> range =
      ExtractKeyRange(*filter.predicate, *column);
  if (!range.has_value()) {
    return std::nullopt;
  }
  ScanResult opened = context.index_reader->ScanRange(scan->table, *range);
  if (auto* error = std::get_if<ExecutionError>(&opened)) {
    return BuildResult(std::move(*error));
  }
  std::unique_ptr<IOperator> source = std::make_unique<ScanOperator>(
      std::move(std::get<std::unique_ptr<IRowStream>>(opened)));
  return Built(std::make_unique<FilterOperator>(std::move(source),
                                                filter.predicate.get()));
}

BuildResult BuildFilter(const domain::FilterNode& filter,
                        const BuildContext& context) {
  if (std::optional<BuildResult> indexed = BuildIndexScan(filter, context)) {
    return std::move(*indexed);
  }
  std::unique_ptr<IOperator> child;
  if (auto error = BuildChild(filter.child, context, &child)) {
    return std::move(*error);
  }
  return Built(std::make_unique<FilterOperator>(std::move(child),
                                                filter.predicate.get()));
}

BuildResult BuildJoin(const domain::JoinNode& join,
                      const BuildContext& context) {
  std::unique_ptr<IOperator> left;
  if (auto error = BuildChild(join.left, context, &left)) {
    return std::move(*error);
  }
  std::unique_ptr<IOperator> right;
  if (auto error = BuildChild(join.right, context, &right)) {
    return std::move(*error);
  }
  JoinSpec spec = MakeJoinSpec(join.condition.get(), left->Columns().size(),
                               right->Columns().size());
  if (spec.equi_columns.has_value()) {
    return Built(std::make_unique<HashJoinOperator>(
        std::move(left), std::move(right), *spec.equi_columns,
        std::move(spec.residuals)));
  }
  return Built(std::make_unique<NestedLoopJoinOperator>(
      std::move(left), std::move(right), std::move(spec.residuals)));
}

BuildResult BuildAggregate(const domain::AggregateNode& aggregate,
                           const BuildContext& context) {
  std::unique_ptr<IOperator> child;
  if (auto error = BuildChild(aggregate.child, context, &child)) {
    return std::move(*error);
  }
  return Built(
      std::make_unique<AggregateOperator>(std::move(child), &aggregate));
}

BuildResult BuildProject(const domain::ProjectNode& project,
                         const BuildContext& context) {
  std::unique_ptr<IOperator> child;
  if (auto error = BuildChild(project.child, context, &child)) {
    return std::move(*error);
  }
  return Built(std::make_unique<ProjectOperator>(std::move(child), &project));
}

BuildResult BuildSort(const domain::SortNode& sort,
                      const BuildContext& context) {
  std::unique_ptr<IOperator> child;
  if (auto error = BuildChild(sort.child, context, &child)) {
    return std::move(*error);
  }
  for (const domain::SortKey& key : sort.keys) {
    assert(key.column_index < child->Columns().size());
  }
  return Built(std::make_unique<SortOperator>(std::move(child), &sort));
}

BuildResult BuildLimit(const domain::LimitNode& limit,
                       const BuildContext& context) {
  std::unique_ptr<IOperator> child;
  if (auto error = BuildChild(limit.child, context, &child)) {
    return std::move(*error);
  }
  return Built(std::make_unique<LimitOperator>(std::move(child), limit.limit,
                                               limit.offset));
}

BuildResult BuildNode(const PlanNode& plan, const BuildContext& context) {
  if (const auto* scan = std::get_if<domain::ScanNode>(&plan.node)) {
    return BuildScan(*scan, context);
  }
  if (const auto* filter = std::get_if<domain::FilterNode>(&plan.node)) {
    return BuildFilter(*filter, context);
  }
  if (const auto* join = std::get_if<domain::JoinNode>(&plan.node)) {
    return BuildJoin(*join, context);
  }
  if (const auto* aggregate = std::get_if<domain::AggregateNode>(&plan.node)) {
    return BuildAggregate(*aggregate, context);
  }
  if (const auto* project = std::get_if<domain::ProjectNode>(&plan.node)) {
    return BuildProject(*project, context);
  }
  if (const auto* sort = std::get_if<domain::SortNode>(&plan.node)) {
    return BuildSort(*sort, context);
  }
  if (const auto* limit = std::get_if<domain::LimitNode>(&plan.node)) {
    return BuildLimit(*limit, context);
  }
  return ExecutionError{ErrorKind::kUnsupported, "not a query plan node"};
}

}  // namespace

BuildResult BuildOperatorTree(const PlanNode& plan, const ITableReader& reader,
                              const IIndexReader* index_reader) {
  return BuildNode(plan, BuildContext{reader, index_reader});
}

}  // namespace strata::paged
