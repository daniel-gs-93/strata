#include "strata/domain/binder.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/expression.h"

// The Binder is pure: it reads the AST and the catalog and builds a plan. The
// checks run in the order of docs/DesignDocument.md section 2.4 and the first
// error wins. Each statement kind first rejects null expression pointers where
// the AST requires one, so the later passes dereference them unchecked.

namespace strata::domain {

namespace {

// Holds a value or the ExecutionError that prevented producing it. Value()
// requires IsOk(); TakeError() requires !IsOk() and leaves the error moved out.
template <typename T>
class Result {
 public:
  // Implicit on purpose, like absl::StatusOr: a function returns a value or an
  // error directly. Both value overloads exist so that returning a local
  // variable moves it.
  Result(T&& value) : value_(std::move(value)) {}  // NOLINT(runtime/explicit)
  Result(const T& value) : value_(value) {}        // NOLINT(runtime/explicit)
  Result(ExecutionError error)                     // NOLINT(runtime/explicit)
      : error_(std::move(error)) {}

  bool IsOk() const { return value_.has_value(); }
  T& Value() { return *value_; }
  ExecutionError TakeError() { return std::move(error_); }

 private:
  std::optional<T> value_;
  ExecutionError error_{ErrorKind::kBind, ""};
};

// An empty Status means success.
using Status = std::optional<ExecutionError>;

ExecutionError BindError(const std::string& message) {
  return ExecutionError{ErrorKind::kBind, message};
}

ExprType ExprTypeOf(DataType type) {
  switch (type) {
    case DataType::kInt:
      return ExprType::kInt;
    case DataType::kDouble:
      return ExprType::kDouble;
    case DataType::kText:
      break;
  }
  return ExprType::kText;
}

bool IsNumericType(ExprType type) {
  return type == ExprType::kInt || type == ExprType::kDouble;
}

const char* TypeName(ExprType type) {
  switch (type) {
    case ExprType::kBool:
      return "BOOL";
    case ExprType::kInt:
      return "INT";
    case ExprType::kDouble:
      return "DOUBLE";
    case ExprType::kText:
      break;
  }
  return "TEXT";
}

const char* TypeName(DataType type) { return TypeName(ExprTypeOf(type)); }

const char* OperatorName(BinaryOp op) {
  switch (op) {
    case BinaryOp::kEq:
      return "=";
    case BinaryOp::kNe:
      return "<>";
    case BinaryOp::kLt:
      return "<";
    case BinaryOp::kLe:
      return "<=";
    case BinaryOp::kGt:
      return ">";
    case BinaryOp::kGe:
      return ">=";
    case BinaryOp::kAnd:
      return "AND";
    case BinaryOp::kOr:
      return "OR";
    case BinaryOp::kAdd:
      return "+";
    case BinaryOp::kMul:
      return "*";
    case BinaryOp::kDiv:
      return "/";
    case BinaryOp::kConcat:
      return "||";
    case BinaryOp::kLike:
      return "LIKE";
    case BinaryOp::kSub:
      break;
  }
  return "-";
}

const char* AggregateName(AggregateFn fn) {
  switch (fn) {
    case AggregateFn::kCount:
      return "COUNT";
    case AggregateFn::kSum:
      return "SUM";
    case AggregateFn::kAvg:
      return "AVG";
    case AggregateFn::kMin:
      return "MIN";
    case AggregateFn::kMax:
      break;
  }
  return "MAX";
}

const char* FunctionName(ScalarFn fn) {
  switch (fn) {
    case ScalarFn::kLower:
      return "LOWER";
    case ScalarFn::kUpper:
      return "UPPER";
    case ScalarFn::kLength:
      return "LENGTH";
    case ScalarFn::kSubstr:
      return "SUBSTR";
    case ScalarFn::kTrim:
      return "TRIM";
    case ScalarFn::kAbs:
      return "ABS";
    case ScalarFn::kRound:
      return "ROUND";
    case ScalarFn::kCast:
      break;
  }
  return "CAST";
}

// The only implicit conversion is INT into a DOUBLE column.
bool IsAssignable(ExprType value_type, DataType column_type) {
  if (value_type == ExprType::kBool) {
    return false;
  }
  if (ExprTypeOf(column_type) == value_type) {
    return true;
  }
  return value_type == ExprType::kInt && column_type == DataType::kDouble;
}

using BoundNode = std::variant<BoundLiteral, BoundColumn, BoundBinary,
                               BoundFunction, BoundAggregate>;

BoundExprPtr MakeBound(ExprType type, BoundNode node) {
  return std::make_unique<BoundExpr>(BoundExpr{type, std::move(node)});
}

BoundExprPtr MakeLiteral(Value value) {
  ExprType type = ExprTypeOf(TypeOf(value));
  return MakeBound(type, BoundLiteral{std::move(value)});
}

BoundExprPtr MakeColumn(ExprType type, std::size_t index) {
  return MakeBound(type, BoundColumn{index});
}

BoundExprPtr MakeBinary(ExprType type, BinaryOp op, BoundExprPtr lhs,
                        BoundExprPtr rhs) {
  return MakeBound(type, BoundBinary{op, std::move(lhs), std::move(rhs)});
}

BoundExprPtr MakeAggregate(ExprType type, AggregateFn fn, BoundExprPtr arg) {
  return MakeBound(type, BoundAggregate{fn, std::move(arg)});
}

BoundExprPtr MakeFunction(ExprType type, ScalarFn fn,
                          std::vector<BoundExprPtr> args) {
  return MakeBound(type, BoundFunction{fn, std::move(args)});
}

BoundExprPtr CloneBound(const BoundExpr& expr) {
  if (const auto* literal = std::get_if<BoundLiteral>(&expr.node)) {
    return MakeBound(expr.type, BoundLiteral{literal->value});
  }
  if (const auto* column = std::get_if<BoundColumn>(&expr.node)) {
    return MakeColumn(expr.type, column->index);
  }
  if (const auto* binary = std::get_if<BoundBinary>(&expr.node)) {
    return MakeBinary(expr.type, binary->op, CloneBound(*binary->lhs),
                      CloneBound(*binary->rhs));
  }
  if (const auto* function = std::get_if<BoundFunction>(&expr.node)) {
    std::vector<BoundExprPtr> args;
    for (const BoundExprPtr& arg : function->args) {
      args.push_back(CloneBound(*arg));
    }
    return MakeFunction(expr.type, function->fn, std::move(args));
  }
  // BoundAggregate is the only alternative left.
  const auto& aggregate = std::get<BoundAggregate>(expr.node);
  BoundExprPtr arg = aggregate.arg ? CloneBound(*aggregate.arg) : nullptr;
  return MakeAggregate(expr.type, aggregate.fn, std::move(arg));
}

// Structural equality of bound expressions; the operands of + and = are not
// treated as commutative. The std::get calls below hold because the node
// indexes of lhs and rhs are equal.
bool BoundEqual(const BoundExpr& lhs, const BoundExpr& rhs) {
  if (lhs.type != rhs.type || lhs.node.index() != rhs.node.index()) {
    return false;
  }
  if (const auto* literal = std::get_if<BoundLiteral>(&lhs.node)) {
    return literal->value == std::get<BoundLiteral>(rhs.node).value;
  }
  if (const auto* column = std::get_if<BoundColumn>(&lhs.node)) {
    return column->index == std::get<BoundColumn>(rhs.node).index;
  }
  if (const auto* binary = std::get_if<BoundBinary>(&lhs.node)) {
    const auto& other = std::get<BoundBinary>(rhs.node);
    return binary->op == other.op && BoundEqual(*binary->lhs, *other.lhs) &&
           BoundEqual(*binary->rhs, *other.rhs);
  }
  if (const auto* function = std::get_if<BoundFunction>(&lhs.node)) {
    const auto& other = std::get<BoundFunction>(rhs.node);
    if (function->fn != other.fn ||
        function->args.size() != other.args.size()) {
      return false;
    }
    for (std::size_t i = 0; i < function->args.size(); ++i) {
      if (!BoundEqual(*function->args[i], *other.args[i])) {
        return false;
      }
    }
    return true;
  }
  const auto& aggregate = std::get<BoundAggregate>(lhs.node);
  const auto& other = std::get<BoundAggregate>(rhs.node);
  if (aggregate.fn != other.fn) {
    return false;
  }
  if (!aggregate.arg || !other.arg) {
    return !aggregate.arg && !other.arg;
  }
  return BoundEqual(*aggregate.arg, *other.arg);
}

void CollectBoundColumns(const BoundExpr& expr,
                         std::vector<std::size_t>& indexes) {
  if (const auto* column = std::get_if<BoundColumn>(&expr.node)) {
    indexes.push_back(column->index);
    return;
  }
  if (const auto* binary = std::get_if<BoundBinary>(&expr.node)) {
    CollectBoundColumns(*binary->lhs, indexes);
    CollectBoundColumns(*binary->rhs, indexes);
    return;
  }
  if (const auto* function = std::get_if<BoundFunction>(&expr.node)) {
    for (const BoundExprPtr& arg : function->args) {
      CollectBoundColumns(*arg, indexes);
    }
    return;
  }
  const auto* aggregate = std::get_if<BoundAggregate>(&expr.node);
  if (aggregate != nullptr && aggregate->arg) {
    CollectBoundColumns(*aggregate->arg, indexes);
  }
}

// Rebases column positions from the joined row to a single table's row.
void ShiftColumnsDown(BoundExpr& expr, std::size_t offset) {
  if (auto* column = std::get_if<BoundColumn>(&expr.node)) {
    assert(column->index >= offset);
    column->index -= offset;
    return;
  }
  if (auto* binary = std::get_if<BoundBinary>(&expr.node)) {
    ShiftColumnsDown(*binary->lhs, offset);
    ShiftColumnsDown(*binary->rhs, offset);
    return;
  }
  if (auto* function = std::get_if<BoundFunction>(&expr.node)) {
    for (BoundExprPtr& arg : function->args) {
      ShiftColumnsDown(*arg, offset);
    }
    return;
  }
  auto* aggregate = std::get_if<BoundAggregate>(&expr.node);
  if (aggregate != nullptr && aggregate->arg) {
    ShiftColumnsDown(*aggregate->arg, offset);
  }
}

void CollectColumnRefs(const Expr& expr, std::vector<const ColumnRef*>& refs) {
  if (const auto* ref = std::get_if<ColumnRef>(&expr.node)) {
    refs.push_back(ref);
    return;
  }
  if (const auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    if (binary->lhs) {
      CollectColumnRefs(*binary->lhs, refs);
    }
    if (binary->rhs) {
      CollectColumnRefs(*binary->rhs, refs);
    }
    return;
  }
  if (const auto* function = std::get_if<FunctionCall>(&expr.node)) {
    for (const ExprPtr& arg : function->args) {
      if (arg) {
        CollectColumnRefs(*arg, refs);
      }
    }
    return;
  }
  const auto* call = std::get_if<AggregateCall>(&expr.node);
  if (call != nullptr && call->arg) {
    CollectColumnRefs(*call->arg, refs);
  }
}

bool ContainsAggregateCall(const Expr& expr) {
  if (std::holds_alternative<AggregateCall>(expr.node)) {
    return true;
  }
  if (const auto* function = std::get_if<FunctionCall>(&expr.node)) {
    for (const ExprPtr& arg : function->args) {
      if (arg && ContainsAggregateCall(*arg)) {
        return true;
      }
    }
    return false;
  }
  const auto* binary = std::get_if<BinaryExpr>(&expr.node);
  if (binary == nullptr) {
    return false;
  }
  return (binary->lhs && ContainsAggregateCall(*binary->lhs)) ||
         (binary->rhs && ContainsAggregateCall(*binary->rhs));
}

// True when some aggregate call has another aggregate call in its argument.
bool HasNestedAggregateCall(const Expr& expr) {
  if (const auto* call = std::get_if<AggregateCall>(&expr.node)) {
    return call->arg && ContainsAggregateCall(*call->arg);
  }
  if (const auto* function = std::get_if<FunctionCall>(&expr.node)) {
    for (const ExprPtr& arg : function->args) {
      if (arg && HasNestedAggregateCall(*arg)) {
        return true;
      }
    }
    return false;
  }
  const auto* binary = std::get_if<BinaryExpr>(&expr.node);
  if (binary == nullptr) {
    return false;
  }
  return (binary->lhs && HasNestedAggregateCall(*binary->lhs)) ||
         (binary->rhs && HasNestedAggregateCall(*binary->rhs));
}

std::string RefText(const ColumnRef& ref) {
  return ref.qualifier.empty() ? ref.name : ref.qualifier + "." + ref.name;
}

struct ScopeTable {
  TableSchema schema;
  std::string qualifier;  // lower-cased alias, or the table name when no alias
  std::string alias;      // as written; empty when absent
  std::size_t offset;     // position of the first column in the joined row
};

using Scope = std::vector<ScopeTable>;

struct ResolvedColumn {
  std::size_t index;  // position in the joined row
  ColumnDef def;
};

Scope SingleTableScope(const TableSchema& schema) {
  return Scope{ScopeTable{schema, ToLower(schema.name), "", 0}};
}

// The first count tables, which is what an ON condition may see.
Scope PrefixScope(const Scope& scope, std::size_t count) {
  return Scope(scope.begin(),
               scope.begin() + static_cast<std::ptrdiff_t>(count));
}

std::optional<std::size_t> FindColumnIndex(const TableSchema& schema,
                                           const std::string& lower_name) {
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (ToLower(schema.columns[i].name) == lower_name) {
      return i;
    }
  }
  return std::nullopt;
}

// Offsets ascend, so the owner is the last table starting at or before index.
std::size_t TableIndexOfColumn(const Scope& scope, std::size_t index) {
  for (std::size_t i = scope.size(); i > 0; --i) {
    if (index >= scope[i - 1].offset) {
      return i - 1;
    }
  }
  return 0;
}

const ColumnDef& ColumnAt(const Scope& scope, std::size_t index) {
  const ScopeTable& table = scope[TableIndexOfColumn(scope, index)];
  return table.schema.columns[index - table.offset];
}

// The edit distance between two names, ignoring case: the fewest insertions,
// deletions and substitutions that turn one into the other.
std::size_t EditDistance(const std::string& first, const std::string& second) {
  const std::string lower_first = ToLower(first);
  const std::string lower_second = ToLower(second);
  std::vector<std::size_t> previous(lower_second.size() + 1);
  std::vector<std::size_t> current(lower_second.size() + 1);
  for (std::size_t j = 0; j <= lower_second.size(); ++j) {
    previous[j] = j;
  }
  for (std::size_t i = 1; i <= lower_first.size(); ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= lower_second.size(); ++j) {
      const std::size_t substitution =
          previous[j - 1] + (lower_first[i - 1] == lower_second[j - 1] ? 0 : 1);
      current[j] =
          std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[lower_second.size()];
}

constexpr std::size_t kMaxSuggestionDistance = 2;

// "; did you mean <candidate>?" for the candidate closest to name within
// kMaxSuggestionDistance (the first of equally close ones), or "" when there is
// none. An exact match, ignoring case, is not a suggestion.
std::string Suggestion(const std::string& name,
                       const std::vector<std::string>& candidates) {
  const std::string* best = nullptr;
  std::size_t best_distance = kMaxSuggestionDistance + 1;
  for (const std::string& candidate : candidates) {
    // The distance is at least the difference of the lengths, so a candidate
    // that differs by more than kMaxSuggestionDistance cannot be suggested and
    // needs no edit-distance table.
    const std::size_t length_gap = name.size() > candidate.size()
                                       ? name.size() - candidate.size()
                                       : candidate.size() - name.size();
    if (length_gap > kMaxSuggestionDistance) {
      continue;
    }
    const std::size_t distance = EditDistance(name, candidate);
    if (distance == 0 || distance >= best_distance) {
      continue;
    }
    best = &candidate;
    best_distance = distance;
  }
  return best == nullptr ? std::string() : "; did you mean " + *best + "?";
}

std::vector<std::string> ColumnNames(const TableSchema& schema) {
  std::vector<std::string> names;
  for (const ColumnDef& column : schema.columns) {
    names.push_back(column.name);
  }
  return names;
}

Result<ResolvedColumn> ResolveQualified(const Scope& scope,
                                        const ColumnRef& ref) {
  std::string qualifier = ToLower(ref.qualifier);
  for (const ScopeTable& table : scope) {
    if (table.qualifier != qualifier) {
      continue;
    }
    std::optional<std::size_t> column =
        FindColumnIndex(table.schema, ToLower(ref.name));
    if (!column) {
      return BindError("unknown column '" + RefText(ref) + "'" +
                       Suggestion(ref.name, ColumnNames(table.schema)));
    }
    return ResolvedColumn{table.offset + *column,
                          table.schema.columns[*column]};
  }
  // A table with an alias is reachable only through the alias.
  for (const ScopeTable& table : scope) {
    if (!table.alias.empty() && ToLower(table.schema.name) == qualifier) {
      return BindError("table '" + ref.qualifier + "' has alias '" +
                       table.alias + "'; use the alias to qualify column '" +
                       ref.name + "'");
    }
  }
  return BindError("unknown table '" + ref.qualifier + "'");
}

Result<ResolvedColumn> ResolveUnqualified(const Scope& scope,
                                          const ColumnRef& ref) {
  std::string name = ToLower(ref.name);
  std::optional<ResolvedColumn> found;
  for (const ScopeTable& table : scope) {
    std::optional<std::size_t> column = FindColumnIndex(table.schema, name);
    if (!column) {
      continue;
    }
    if (found) {
      return BindError("ambiguous column '" + ref.name + "'");
    }
    found =
        ResolvedColumn{table.offset + *column, table.schema.columns[*column]};
  }
  if (!found) {
    std::vector<std::string> names;
    for (const ScopeTable& table : scope) {
      for (const std::string& column_name : ColumnNames(table.schema)) {
        names.push_back(column_name);
      }
    }
    return BindError("unknown column '" + ref.name + "'" +
                     Suggestion(ref.name, names));
  }
  return *found;
}

Result<ResolvedColumn> ResolveColumn(const Scope& scope, const ColumnRef& ref) {
  if (ref.qualifier.empty()) {
    return ResolveUnqualified(scope, ref);
  }
  return ResolveQualified(scope, ref);
}

Status CheckColumns(const Expr& expr, const Scope& scope) {
  std::vector<const ColumnRef*> refs;
  CollectColumnRefs(expr, refs);
  for (const ColumnRef* ref : refs) {
    Result<ResolvedColumn> resolved = ResolveColumn(scope, *ref);
    if (!resolved.IsOk()) {
      return resolved.TakeError();
    }
  }
  return std::nullopt;
}

// Flags a name that resolves only when tables after the first visible_count
// are in scope: an ON condition cannot see tables joined later.
Status CheckOnVisibility(const Expr& on, const Scope& scope,
                         std::size_t visible_count) {
  Scope visible = PrefixScope(scope, visible_count);
  std::vector<const ColumnRef*> refs;
  CollectColumnRefs(on, refs);
  for (const ColumnRef* ref : refs) {
    if (ResolveColumn(visible, *ref).IsOk()) {
      continue;
    }
    // Unknown or ambiguous names are step 3 errors.
    if (!ResolveColumn(scope, *ref).IsOk()) {
      continue;
    }
    return BindError("ON condition refers to column '" + RefText(*ref) +
                     "' of a table that is joined later");
  }
  return std::nullopt;
}

Result<BoundExprPtr> BindExpr(const Expr& expr, const Scope& scope);

Result<ExprType> TypeOfBinary(BinaryOp op, ExprType lhs, ExprType rhs) {
  std::string mismatch = std::string("operator ") + OperatorName(op) +
                         " cannot be applied to " + TypeName(lhs) + " and " +
                         TypeName(rhs);
  if (op == BinaryOp::kAnd || op == BinaryOp::kOr) {
    if (lhs != ExprType::kBool || rhs != ExprType::kBool) {
      return BindError(mismatch);
    }
    return ExprType::kBool;
  }
  bool is_numeric_pair = IsNumericType(lhs) && IsNumericType(rhs);
  if (op == BinaryOp::kAdd || op == BinaryOp::kSub || op == BinaryOp::kMul ||
      op == BinaryOp::kDiv) {
    if (!is_numeric_pair) {
      return BindError(mismatch);
    }
    return (lhs == ExprType::kInt && rhs == ExprType::kInt) ? ExprType::kInt
                                                            : ExprType::kDouble;
  }
  bool is_text_pair = lhs == ExprType::kText && rhs == ExprType::kText;
  if (op == BinaryOp::kConcat) {
    if (!is_text_pair) {
      return BindError(mismatch);
    }
    return ExprType::kText;
  }
  if (op == BinaryOp::kLike) {
    if (!is_text_pair) {
      return BindError(mismatch);
    }
    return ExprType::kBool;
  }
  if (!is_numeric_pair && !is_text_pair) {
    return BindError(mismatch);
  }
  return ExprType::kBool;
}

Result<BoundExprPtr> BindBinary(const BinaryExpr& binary, const Scope& scope) {
  if (!binary.lhs || !binary.rhs) {
    return BindError("malformed binary expression");
  }
  Result<BoundExprPtr> lhs = BindExpr(*binary.lhs, scope);
  if (!lhs.IsOk()) {
    return lhs;
  }
  Result<BoundExprPtr> rhs = BindExpr(*binary.rhs, scope);
  if (!rhs.IsOk()) {
    return rhs;
  }
  Result<ExprType> type =
      TypeOfBinary(binary.op, lhs.Value()->type, rhs.Value()->type);
  if (!type.IsOk()) {
    return type.TakeError();
  }
  return MakeBinary(type.Value(), binary.op, std::move(lhs.Value()),
                    std::move(rhs.Value()));
}

Result<BoundExprPtr> BindAggregateCall(const AggregateCall& call,
                                       const Scope& scope) {
  if (!call.arg) {
    if (call.fn != AggregateFn::kCount) {
      return BindError(std::string(AggregateName(call.fn)) +
                       " requires an argument");
    }
    return MakeAggregate(ExprType::kInt, call.fn, nullptr);
  }
  Result<BoundExprPtr> arg = BindExpr(*call.arg, scope);
  if (!arg.IsOk()) {
    return arg;
  }
  ExprType arg_type = arg.Value()->type;
  if (arg_type == ExprType::kBool) {
    return BindError("an aggregate argument cannot be a boolean expression");
  }
  const bool needs_number =
      call.fn == AggregateFn::kSum || call.fn == AggregateFn::kAvg;
  if (needs_number && !IsNumericType(arg_type)) {
    return BindError(std::string(AggregateName(call.fn)) +
                     " requires a numeric argument, got " + TypeName(arg_type));
  }
  ExprType result_type = arg_type;
  if (call.fn == AggregateFn::kCount) {
    result_type = ExprType::kInt;
  }
  if (call.fn == AggregateFn::kAvg) {
    result_type = ExprType::kDouble;
  }
  return MakeAggregate(result_type, call.fn, std::move(arg.Value()));
}

// What a function argument must be.
enum class ArgumentKind { kText, kInt, kNumber, kAnyValue };

struct FunctionSignature {
  std::size_t min_args;
  std::size_t max_args;
  std::vector<ArgumentKind> arguments;  // by position
};

FunctionSignature SignatureOf(ScalarFn fn) {
  switch (fn) {
    case ScalarFn::kLower:
    case ScalarFn::kUpper:
    case ScalarFn::kLength:
    case ScalarFn::kTrim:
      return FunctionSignature{1, 1, {ArgumentKind::kText}};
    case ScalarFn::kSubstr:
      return FunctionSignature{
          2, 3, {ArgumentKind::kText, ArgumentKind::kInt, ArgumentKind::kInt}};
    case ScalarFn::kAbs:
      return FunctionSignature{1, 1, {ArgumentKind::kNumber}};
    case ScalarFn::kRound:
      return FunctionSignature{
          1, 2, {ArgumentKind::kNumber, ArgumentKind::kInt}};
    case ScalarFn::kCast:
      break;
  }
  return FunctionSignature{1, 1, {ArgumentKind::kAnyValue}};
}

bool IsAcceptable(ArgumentKind kind, ExprType type) {
  switch (kind) {
    case ArgumentKind::kText:
      return type == ExprType::kText;
    case ArgumentKind::kInt:
      return type == ExprType::kInt;
    case ArgumentKind::kNumber:
      return IsNumericType(type);
    case ArgumentKind::kAnyValue:
      break;
  }
  return type != ExprType::kBool;
}

const char* ArgumentKindName(ArgumentKind kind) {
  switch (kind) {
    case ArgumentKind::kText:
      return "TEXT";
    case ArgumentKind::kInt:
      return "INT";
    case ArgumentKind::kNumber:
      return "INT or DOUBLE";
    case ArgumentKind::kAnyValue:
      break;
  }
  return "INT, DOUBLE or TEXT";
}

std::string ArgumentCountText(const FunctionSignature& signature) {
  if (signature.min_args == signature.max_args) {
    return std::to_string(signature.min_args) +
           (signature.min_args == 1 ? " argument" : " arguments");
  }
  return std::to_string(signature.min_args) + " or " +
         std::to_string(signature.max_args) + " arguments";
}

ExprType ResultTypeOf(const FunctionCall& call,
                      const std::vector<BoundExprPtr>& args) {
  switch (call.fn) {
    case ScalarFn::kLower:
    case ScalarFn::kUpper:
    case ScalarFn::kTrim:
    case ScalarFn::kSubstr:
      return ExprType::kText;
    case ScalarFn::kLength:
      return ExprType::kInt;
    case ScalarFn::kAbs:
      return args[0]->type;
    case ScalarFn::kRound:
      return ExprType::kDouble;
    case ScalarFn::kCast:
      break;
  }
  return ExprTypeOf(call.cast_type);
}

Result<BoundExprPtr> BindFunctionCall(const FunctionCall& call,
                                      const Scope& scope) {
  const std::string name = FunctionName(call.fn);
  const FunctionSignature signature = SignatureOf(call.fn);
  if (call.args.size() < signature.min_args ||
      call.args.size() > signature.max_args) {
    return BindError(name + " requires " + ArgumentCountText(signature) +
                     ", got " + std::to_string(call.args.size()));
  }
  std::vector<BoundExprPtr> args;
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    if (!call.args[i]) {
      return BindError("malformed function call");
    }
    Result<BoundExprPtr> bound = BindExpr(*call.args[i], scope);
    if (!bound.IsOk()) {
      return bound;
    }
    const ArgumentKind kind = signature.arguments[i];
    if (!IsAcceptable(kind, bound.Value()->type)) {
      return BindError(name + " argument " + std::to_string(i + 1) +
                       " must be " + ArgumentKindName(kind) + ", got " +
                       TypeName(bound.Value()->type));
    }
    args.push_back(std::move(bound.Value()));
  }
  ExprType type = ResultTypeOf(call, args);
  return MakeFunction(type, call.fn, std::move(args));
}

Result<BoundExprPtr> BindExpr(const Expr& expr, const Scope& scope) {
  if (const auto* literal = std::get_if<Literal>(&expr.node)) {
    return MakeLiteral(literal->value);
  }
  if (const auto* ref = std::get_if<ColumnRef>(&expr.node)) {
    Result<ResolvedColumn> resolved = ResolveColumn(scope, *ref);
    if (!resolved.IsOk()) {
      return resolved.TakeError();
    }
    return MakeColumn(ExprTypeOf(resolved.Value().def.type),
                      resolved.Value().index);
  }
  if (const auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return BindBinary(*binary, scope);
  }
  if (const auto* function = std::get_if<FunctionCall>(&expr.node)) {
    return BindFunctionCall(*function, scope);
  }
  // AggregateCall is the only alternative left.
  return BindAggregateCall(std::get<AggregateCall>(expr.node), scope);
}

// WHERE and ON: the expression must be boolean.
Result<BoundExprPtr> BindCondition(const Expr& expr, const Scope& scope,
                                   const std::string& clause) {
  Result<BoundExprPtr> bound = BindExpr(expr, scope);
  if (!bound.IsOk()) {
    return bound;
  }
  if (bound.Value()->type != ExprType::kBool) {
    return BindError(clause + " must be a boolean expression, got " +
                     TypeName(bound.Value()->type));
  }
  return bound;
}

// SELECT list, GROUP BY, SET, VALUES, ORDER BY: a boolean is not a value.
Result<BoundExprPtr> BindValue(const Expr& expr, const Scope& scope,
                               const std::string& clause) {
  Result<BoundExprPtr> bound = BindExpr(expr, scope);
  if (!bound.IsOk()) {
    return bound;
  }
  if (bound.Value()->type == ExprType::kBool) {
    return BindError("a boolean expression is not allowed in " + clause);
  }
  return bound;
}

ExecutionError AggregateNotAllowed(const std::string& clause) {
  return BindError("aggregate functions are not allowed in " + clause);
}

template <typename Node>
PlanPtr MakePlan(Node node) {
  auto plan = std::make_unique<PlanNode>();
  plan->node = std::move(node);
  return plan;
}

// Combines conjuncts[first, last) into one AND tree that splits the range at
// its middle. The leaves keep their order and AND is evaluated left to right,
// so the result and the first failing operand are those of a left-deep chain.
// The depth is log2(count) where a chain would have count levels, and the
// evaluator and the plan destructor recurse over that depth: a parenthesized
// WHERE of 100,000 balanced terms passes the parser limits.
// Precondition: first < last.
BoundExprPtr CombineRangeWithAnd(std::vector<BoundExprPtr>& conjuncts,
                                 std::size_t first, std::size_t last) {
  if (last - first == 1) {
    return std::move(conjuncts[first]);
  }
  const std::size_t middle = first + (last - first) / 2;
  BoundExprPtr lhs = CombineRangeWithAnd(conjuncts, first, middle);
  BoundExprPtr rhs = CombineRangeWithAnd(conjuncts, middle, last);
  return MakeBinary(ExprType::kBool, BinaryOp::kAnd, std::move(lhs),
                    std::move(rhs));
}

// Combines conjuncts into one balanced AND tree. Precondition: conjuncts is
// not empty. Moves every element out; the caller's vector keeps null entries.
BoundExprPtr CombineWithAnd(std::vector<BoundExprPtr>& conjuncts) {
  return CombineRangeWithAnd(conjuncts, 0, conjuncts.size());
}

// Takes ownership of child and, when conjuncts is not empty, moves them out
// into one FilterNode above it. Returns child unchanged otherwise.
PlanPtr WrapInFilter(PlanPtr child, std::vector<BoundExprPtr>& conjuncts) {
  if (conjuncts.empty()) {
    return child;
  }
  return MakePlan(FilterNode{std::move(child), CombineWithAnd(conjuncts)});
}

// One output column of a SELECT, with `*` already expanded. BuildSelectPlan
// moves output_expr into the ProjectNode.
struct OutputItem {
  BoundExprPtr input_expr;   // over the FROM row; may hold BoundAggregate
  BoundExprPtr output_expr;  // over the ProjectNode child's row
  std::string name;
  std::string alias;  // lower-cased; empty when absent
  bool is_star = false;
};

// Everything BindSelect accumulates. BuildSelectPlan consumes where,
// join_conditions, group_keys, aggregates and the output_expr of every item;
// the binding must not be read after it.
struct SelectBinding {
  Scope scope;
  std::vector<OutputItem> items;
  std::vector<BoundExprPtr> join_conditions;  // one per JOIN, in order
  BoundExprPtr where;                         // null when absent
  std::vector<BoundExprPtr> group_keys;
  std::vector<AggregateSpec> aggregates;
  bool is_aggregate_query = false;
};

// Fails on an unknown table or on a repeated table name or alias.
Result<Scope> BuildScope(const SelectStatement& select,
                         const ICatalog& catalog) {
  std::vector<const TableRef*> refs{&select.from};
  for (const JoinClause& join : select.joins) {
    refs.push_back(&join.table);
  }
  Scope scope;
  std::size_t offset = 0;
  for (const TableRef* ref : refs) {
    std::optional<TableSchema> schema = catalog.FindTable(ref->table);
    if (!schema) {
      return BindError("unknown table '" + ref->table + "'" +
                       Suggestion(ref->table, catalog.TableNames()));
    }
    std::string qualifier =
        ToLower(ref->alias.empty() ? schema->name : ref->alias);
    std::size_t column_count = schema->columns.size();
    scope.push_back(ScopeTable{std::move(*schema), std::move(qualifier),
                               ref->alias, offset});
    offset += column_count;
  }
  std::unordered_set<std::string> seen_qualifiers;
  for (std::size_t i = 0; i < scope.size(); ++i) {
    if (!seen_qualifiers.insert(scope[i].qualifier).second) {
      const std::string& written =
          refs[i]->alias.empty() ? refs[i]->table : refs[i]->alias;
      return BindError("duplicate table name or alias '" + written + "'");
    }
  }
  return scope;
}

// ORDER BY is excluded: a key can name a select alias, so ResolveOrderBy
// resolves it last.
Status CheckSelectNames(const SelectStatement& select, const Scope& scope) {
  for (std::size_t i = 0; i < select.joins.size(); ++i) {
    if (!select.joins[i].on) {
      return BindError("JOIN requires an ON condition");
    }
    if (Status status = CheckOnVisibility(*select.joins[i].on, scope, i + 2)) {
      return status;
    }
  }
  for (const SelectItem& item : select.items) {
    if (item.is_star) {
      continue;
    }
    if (!item.expr) {
      return BindError("malformed select item");
    }
    if (Status status = CheckColumns(*item.expr, scope)) {
      return status;
    }
  }
  for (std::size_t i = 0; i < select.joins.size(); ++i) {
    if (Status status =
            CheckColumns(*select.joins[i].on, PrefixScope(scope, i + 2))) {
      return status;
    }
  }
  if (select.where) {
    if (Status status = CheckColumns(*select.where, scope)) {
      return status;
    }
  }
  for (const ExprPtr& key : select.group_by) {
    if (!key) {
      return BindError("malformed GROUP BY item");
    }
    if (Status status = CheckColumns(*key, scope)) {
      return status;
    }
  }
  return std::nullopt;
}

void ExpandStar(const Scope& scope, std::vector<OutputItem>& items) {
  for (const ScopeTable& table : scope) {
    for (std::size_t i = 0; i < table.schema.columns.size(); ++i) {
      const ColumnDef& column = table.schema.columns[i];
      OutputItem output;
      output.input_expr = MakeColumn(ExprTypeOf(column.type), table.offset + i);
      output.name = column.name;
      output.is_star = true;
      items.push_back(std::move(output));
    }
  }
}

// Alias; else the declared name for a plain column; else the source text.
std::string OutputName(const SelectItem& item, const BoundExpr& bound,
                       const Scope& scope) {
  if (!item.alias.empty()) {
    return item.alias;
  }
  if (const auto* column = std::get_if<BoundColumn>(&bound.node)) {
    return ColumnAt(scope, column->index).name;
  }
  return item.text;
}

Result<std::vector<OutputItem>> BindSelectItems(const SelectStatement& select,
                                                const Scope& scope) {
  std::vector<OutputItem> items;
  for (const SelectItem& item : select.items) {
    if (item.is_star) {
      ExpandStar(scope, items);
      continue;
    }
    Result<BoundExprPtr> bound =
        BindValue(*item.expr, scope, "the SELECT list");
    if (!bound.IsOk()) {
      return bound.TakeError();
    }
    OutputItem output;
    output.name = OutputName(item, *bound.Value(), scope);
    output.alias = ToLower(item.alias);
    output.input_expr = std::move(bound.Value());
    items.push_back(std::move(output));
  }
  return items;
}

// Fills items, join_conditions, where and group_keys of binding.
Status BindSelectClauses(const SelectStatement& select,
                         SelectBinding& binding) {
  Result<std::vector<OutputItem>> items =
      BindSelectItems(select, binding.scope);
  if (!items.IsOk()) {
    return items.TakeError();
  }
  binding.items = std::move(items.Value());
  for (std::size_t i = 0; i < select.joins.size(); ++i) {
    Result<BoundExprPtr> condition = BindCondition(
        *select.joins[i].on, PrefixScope(binding.scope, i + 2), "ON condition");
    if (!condition.IsOk()) {
      return condition.TakeError();
    }
    binding.join_conditions.push_back(std::move(condition.Value()));
  }
  if (select.where) {
    Result<BoundExprPtr> where =
        BindCondition(*select.where, binding.scope, "WHERE");
    if (!where.IsOk()) {
      return where.TakeError();
    }
    binding.where = std::move(where.Value());
  }
  for (const ExprPtr& key : select.group_by) {
    Result<BoundExprPtr> bound = BindValue(*key, binding.scope, "GROUP BY");
    if (!bound.IsOk()) {
      return bound.TakeError();
    }
    binding.group_keys.push_back(std::move(bound.Value()));
  }
  return std::nullopt;
}

// Aggregates are allowed only in select items, and never inside an aggregate.
Status CheckSelectAggregates(const SelectStatement& select) {
  for (const SelectItem& item : select.items) {
    if (!item.is_star && HasNestedAggregateCall(*item.expr)) {
      return BindError("aggregate functions cannot be nested");
    }
  }
  for (const JoinClause& join : select.joins) {
    if (ContainsAggregateCall(*join.on)) {
      return AggregateNotAllowed("ON conditions");
    }
  }
  if (select.where && ContainsAggregateCall(*select.where)) {
    return AggregateNotAllowed("WHERE");
  }
  for (const ExprPtr& key : select.group_by) {
    if (ContainsAggregateCall(*key)) {
      return AggregateNotAllowed("GROUP BY");
    }
  }
  return std::nullopt;
}

// The references must outlive the context. aggregates is appended to by
// RewriteOverAggregate.
struct RewriteContext {
  const Scope& scope;
  const std::vector<BoundExprPtr>& group_keys;
  std::vector<AggregateSpec>& aggregates;
};

// Rewrites expr over the AggregateNode output: group keys first, then one
// column per aggregate.
Result<BoundExprPtr> RewriteOverAggregate(const BoundExpr& expr,
                                          const RewriteContext& context) {
  for (std::size_t i = 0; i < context.group_keys.size(); ++i) {
    if (BoundEqual(expr, *context.group_keys[i])) {
      return MakeColumn(expr.type, i);
    }
  }
  if (std::holds_alternative<BoundLiteral>(expr.node)) {
    return CloneBound(expr);
  }
  if (const auto* column = std::get_if<BoundColumn>(&expr.node)) {
    return BindError(
        "column '" + ColumnAt(context.scope, column->index).name +
        "' must appear in GROUP BY or be used in an aggregate function");
  }
  if (const auto* binary = std::get_if<BoundBinary>(&expr.node)) {
    Result<BoundExprPtr> lhs = RewriteOverAggregate(*binary->lhs, context);
    if (!lhs.IsOk()) {
      return lhs;
    }
    Result<BoundExprPtr> rhs = RewriteOverAggregate(*binary->rhs, context);
    if (!rhs.IsOk()) {
      return rhs;
    }
    return MakeBinary(expr.type, binary->op, std::move(lhs.Value()),
                      std::move(rhs.Value()));
  }
  if (const auto* function = std::get_if<BoundFunction>(&expr.node)) {
    std::vector<BoundExprPtr> args;
    for (const BoundExprPtr& arg : function->args) {
      Result<BoundExprPtr> rewritten = RewriteOverAggregate(*arg, context);
      if (!rewritten.IsOk()) {
        return rewritten;
      }
      args.push_back(std::move(rewritten.Value()));
    }
    return MakeFunction(expr.type, function->fn, std::move(args));
  }
  // BoundAggregate is the only alternative left.
  const auto& aggregate = std::get<BoundAggregate>(expr.node);
  BoundExprPtr arg = aggregate.arg ? CloneBound(*aggregate.arg) : nullptr;
  context.aggregates.push_back(AggregateSpec{aggregate.fn, std::move(arg)});
  return MakeColumn(expr.type,
                    context.group_keys.size() + context.aggregates.size() - 1);
}

// Sets output_expr of every item and, for an aggregate query, fills
// binding.aggregates.
Status RewriteSelectItems(const SelectStatement& select,
                          SelectBinding& binding) {
  binding.is_aggregate_query = !select.group_by.empty();
  for (const SelectItem& item : select.items) {
    if (!item.is_star && ContainsAggregateCall(*item.expr)) {
      binding.is_aggregate_query = true;
    }
  }
  RewriteContext context{binding.scope, binding.group_keys, binding.aggregates};
  for (OutputItem& item : binding.items) {
    if (!binding.is_aggregate_query) {
      item.output_expr = CloneBound(*item.input_expr);
      continue;
    }
    if (item.is_star) {
      return BindError(
          "SELECT * cannot be combined with GROUP BY or aggregate functions");
    }
    Result<BoundExprPtr> rewritten =
        RewriteOverAggregate(*item.input_expr, context);
    if (!rewritten.IsOk()) {
      return rewritten.TakeError();
    }
    item.output_expr = std::move(rewritten.Value());
  }
  return std::nullopt;
}

std::optional<std::size_t> FindAliasIndex(const std::vector<OutputItem>& items,
                                          const std::string& lower_name) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!items[i].alias.empty() && items[i].alias == lower_name) {
      return i;
    }
  }
  return std::nullopt;
}

// Resolves one key in this order: position, alias, structurally equal item.
Result<std::size_t> ResolveOrderKey(const Expr& expr,
                                    const SelectBinding& binding) {
  const std::vector<OutputItem>& items = binding.items;
  if (const auto* literal = std::get_if<Literal>(&expr.node)) {
    if (const auto* position = std::get_if<int64_t>(&literal->value)) {
      if (*position < 1 || *position > static_cast<int64_t>(items.size())) {
        return BindError("ORDER BY position " + std::to_string(*position) +
                         " is out of range 1.." + std::to_string(items.size()));
      }
      return static_cast<std::size_t>(*position - 1);
    }
  }
  if (const auto* ref = std::get_if<ColumnRef>(&expr.node)) {
    if (ref->qualifier.empty()) {
      std::optional<std::size_t> alias_index =
          FindAliasIndex(items, ToLower(ref->name));
      if (alias_index) {
        return *alias_index;
      }
    }
  }
  Result<BoundExprPtr> bound = BindValue(expr, binding.scope, "ORDER BY");
  if (!bound.IsOk()) {
    return bound.TakeError();
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (BoundEqual(*bound.Value(), *items[i].input_expr)) {
      return i;
    }
  }
  std::string subject = "expression";
  if (const auto* ref = std::get_if<ColumnRef>(&expr.node)) {
    subject = "'" + RefText(*ref) + "'";
  }
  return BindError("ORDER BY " + subject +
                   " does not match any SELECT list item or alias");
}

Result<std::vector<SortKey>> ResolveOrderBy(const SelectStatement& select,
                                            const SelectBinding& binding) {
  std::vector<SortKey> keys;
  for (const OrderItem& order : select.order_by) {
    if (!order.expr) {
      return BindError("malformed ORDER BY item");
    }
    Result<std::size_t> position = ResolveOrderKey(*order.expr, binding);
    if (!position.IsOk()) {
      return position.TakeError();
    }
    keys.push_back(SortKey{position.Value(), order.is_descending});
  }
  return keys;
}

// Takes ownership of expr and appends its top-level AND operands to conjuncts,
// in left-to-right order.
void SplitConjuncts(BoundExprPtr expr, std::vector<BoundExprPtr>& conjuncts) {
  auto* binary = std::get_if<BoundBinary>(&expr->node);
  if (binary == nullptr || binary->op != BinaryOp::kAnd) {
    conjuncts.push_back(std::move(expr));
    return;
  }
  BoundExprPtr lhs = std::move(binary->lhs);
  BoundExprPtr rhs = std::move(binary->rhs);
  SplitConjuncts(std::move(lhs), conjuncts);
  SplitConjuncts(std::move(rhs), conjuncts);
}

// Where each WHERE conjunct goes, indexed by table. above_join[j] holds the
// conjuncts placed directly above the JoinNode that adds table j; entry 0 is
// always empty.
struct FilterBuckets {
  std::vector<std::vector<BoundExprPtr>> on_scan;
  std::vector<std::vector<BoundExprPtr>> above_join;
};

// ADR-0009: the lowest position where every column of the conjunct exists.
// Takes ownership of where (null is allowed). Precondition: scope is not
// empty and every column index in where is inside the joined row of scope.
FilterBuckets PlaceConjuncts(BoundExprPtr where, const Scope& scope) {
  FilterBuckets buckets;
  buckets.on_scan.resize(scope.size());
  buckets.above_join.resize(scope.size());
  if (!where) {
    return buckets;
  }
  std::vector<BoundExprPtr> conjuncts;
  SplitConjuncts(std::move(where), conjuncts);
  for (BoundExprPtr& conjunct : conjuncts) {
    std::vector<std::size_t> columns;
    CollectBoundColumns(*conjunct, columns);
    if (columns.empty()) {
      buckets.on_scan[0].push_back(std::move(conjunct));
      continue;
    }
    std::size_t lowest = scope.size();
    std::size_t highest = 0;
    for (std::size_t column : columns) {
      std::size_t table_index = TableIndexOfColumn(scope, column);
      lowest = std::min(lowest, table_index);
      highest = std::max(highest, table_index);
    }
    if (lowest == highest) {
      ShiftColumnsDown(*conjunct, scope[lowest].offset);
      buckets.on_scan[lowest].push_back(std::move(conjunct));
      continue;
    }
    buckets.above_join[highest].push_back(std::move(conjunct));
  }
  return buckets;
}

// Scan, Filter and Join nodes: the part of the plan that produces the FROM row.
// Moves binding.where and binding.join_conditions out. Precondition: scope is
// not empty and join_conditions has one entry per table after the first.
PlanPtr BuildInputPlan(SelectBinding& binding) {
  FilterBuckets buckets =
      PlaceConjuncts(std::move(binding.where), binding.scope);
  PlanPtr plan = WrapInFilter(MakePlan(ScanNode{binding.scope[0].schema.name}),
                              buckets.on_scan[0]);
  for (std::size_t i = 1; i < binding.scope.size(); ++i) {
    PlanPtr right = WrapInFilter(
        MakePlan(ScanNode{binding.scope[i].schema.name}), buckets.on_scan[i]);
    plan = MakePlan(JoinNode{std::move(plan), std::move(right),
                             std::move(binding.join_conditions[i - 1])});
    plan = WrapInFilter(std::move(plan), buckets.above_join[i]);
  }
  return plan;
}

// Moves the remaining plan inputs out of binding and takes sort_keys.
// Precondition: every item's output_expr is set and none is boolean.
PlanPtr BuildSelectPlan(SelectBinding& binding, std::vector<SortKey> sort_keys,
                        const SelectStatement& select) {
  PlanPtr plan = BuildInputPlan(binding);
  if (binding.is_aggregate_query) {
    plan =
        MakePlan(AggregateNode{std::move(plan), std::move(binding.group_keys),
                               std::move(binding.aggregates)});
  }
  ProjectNode project;
  for (OutputItem& item : binding.items) {
    project.output_columns.push_back(
        ColumnDef{item.name, DataTypeOf(item.output_expr->type)});
    project.exprs.push_back(std::move(item.output_expr));
  }
  project.child = std::move(plan);
  plan = MakePlan(std::move(project));
  if (!sort_keys.empty()) {
    plan = MakePlan(SortNode{std::move(plan), std::move(sort_keys)});
  }
  if (!select.limit) {
    return plan;
  }
  return MakePlan(LimitNode{std::move(plan), select.limit, select.offset});
}

Result<PlanPtr> BindSelect(const SelectStatement& select,
                           const ICatalog& catalog) {
  // The checks run in the order of DesignDocument.md 2.4 steps 1 to 7, so the
  // first error returned is the one the design names.
  Result<Scope> scope = BuildScope(select, catalog);
  if (!scope.IsOk()) {
    return scope.TakeError();
  }
  SelectBinding binding;
  binding.scope = std::move(scope.Value());
  if (Status status = CheckSelectNames(select, binding.scope)) {
    return *status;
  }
  if (Status status = BindSelectClauses(select, binding)) {
    return *status;
  }
  if (Status status = CheckSelectAggregates(select)) {
    return *status;
  }
  if (Status status = RewriteSelectItems(select, binding)) {
    return *status;
  }
  Result<std::vector<SortKey>> sort_keys = ResolveOrderBy(select, binding);
  if (!sort_keys.IsOk()) {
    return sort_keys.TakeError();
  }
  return BuildSelectPlan(binding, std::move(sort_keys.Value()), select);
}

ExecutionError UnknownTable(const std::string& name, const ICatalog& catalog) {
  return BindError("unknown table '" + name + "'" +
                   Suggestion(name, catalog.TableNames()));
}

// Returns a null predicate when where is absent.
Result<BoundExprPtr> BindOptionalWhere(const ExprPtr& where,
                                       const Scope& scope) {
  if (!where) {
    return BoundExprPtr();
  }
  Result<BoundExprPtr> predicate = BindCondition(*where, scope, "WHERE");
  if (!predicate.IsOk()) {
    return predicate;
  }
  if (ContainsAggregateCall(*where)) {
    return AggregateNotAllowed("WHERE");
  }
  return predicate;
}

Result<PlanPtr> BindDelete(const DeleteStatement& statement,
                           const ICatalog& catalog) {
  std::optional<TableSchema> schema = catalog.FindTable(statement.table);
  if (!schema) {
    return UnknownTable(statement.table, catalog);
  }
  Scope scope = SingleTableScope(*schema);
  if (statement.where) {
    if (Status status = CheckColumns(*statement.where, scope)) {
      return *status;
    }
  }
  Result<BoundExprPtr> predicate = BindOptionalWhere(statement.where, scope);
  if (!predicate.IsOk()) {
    return predicate.TakeError();
  }
  return MakePlan(DeleteNode{schema->name, std::move(predicate.Value())});
}

// Position of the first entry that repeats an earlier one.
std::optional<std::size_t> FindRepeatedTarget(
    const std::vector<std::size_t>& targets) {
  std::unordered_set<std::size_t> seen;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (!seen.insert(targets[i]).second) {
      return i;
    }
  }
  return std::nullopt;
}

// The targets exist and are distinct, and none is the primary key. Returns
// column positions in assignment order.
Result<std::vector<std::size_t>> ResolveAssignmentTargets(
    const std::vector<Assignment>& assignments, const TableSchema& schema) {
  std::vector<std::size_t> targets;
  for (const Assignment& assignment : assignments) {
    std::optional<std::size_t> column =
        FindColumnIndex(schema, ToLower(assignment.column));
    if (!column) {
      return BindError("unknown column '" + assignment.column + "'" +
                       Suggestion(assignment.column, ColumnNames(schema)));
    }
    targets.push_back(*column);
  }
  if (std::optional<std::size_t> repeated = FindRepeatedTarget(targets)) {
    return BindError("column '" + assignments[*repeated].column +
                     "' is assigned more than once");
  }
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (targets[i] == schema.primary_key_index) {
      return ExecutionError{ErrorKind::kUnsupported,
                            "updating the primary key column '" +
                                assignments[i].column + "' is not supported"};
    }
  }
  return targets;
}

Status CheckAssignable(const ColumnDef& column, ExprType value_type) {
  if (IsAssignable(value_type, column.type)) {
    return std::nullopt;
  }
  return BindError(std::string("cannot assign ") + TypeName(value_type) +
                   " to column '" + column.name + "' of type " +
                   TypeName(column.type));
}

Status CheckUpdateNames(const UpdateStatement& statement, const Scope& scope) {
  for (const Assignment& assignment : statement.assignments) {
    if (!assignment.value) {
      return BindError("malformed assignment");
    }
    if (Status status = CheckColumns(*assignment.value, scope)) {
      return status;
    }
  }
  if (!statement.where) {
    return std::nullopt;
  }
  return CheckColumns(*statement.where, scope);
}

// Returns the bound values in assignment order.
Result<std::vector<BoundExprPtr>> BindAssignmentValues(
    const std::vector<Assignment>& assignments, const Scope& scope) {
  std::vector<BoundExprPtr> values;
  for (const Assignment& assignment : assignments) {
    Result<BoundExprPtr> value = BindValue(*assignment.value, scope, "SET");
    if (!value.IsOk()) {
      return value.TakeError();
    }
    values.push_back(std::move(value.Value()));
  }
  for (const Assignment& assignment : assignments) {
    if (ContainsAggregateCall(*assignment.value)) {
      return AggregateNotAllowed("SET");
    }
  }
  return values;
}

Result<PlanPtr> BindUpdate(const UpdateStatement& statement,
                           const ICatalog& catalog) {
  std::optional<TableSchema> schema = catalog.FindTable(statement.table);
  if (!schema) {
    return UnknownTable(statement.table, catalog);
  }
  Scope scope = SingleTableScope(*schema);
  if (Status status = CheckUpdateNames(statement, scope)) {
    return *status;
  }
  Result<std::vector<BoundExprPtr>> values =
      BindAssignmentValues(statement.assignments, scope);
  if (!values.IsOk()) {
    return values.TakeError();
  }
  Result<BoundExprPtr> predicate = BindOptionalWhere(statement.where, scope);
  if (!predicate.IsOk()) {
    return predicate.TakeError();
  }
  Result<std::vector<std::size_t>> targets =
      ResolveAssignmentTargets(statement.assignments, *schema);
  if (!targets.IsOk()) {
    return targets.TakeError();
  }
  std::vector<BoundExprPtr>& bound_values = values.Value();
  UpdateNode update{schema->name, {}, std::move(predicate.Value())};
  for (std::size_t i = 0; i < bound_values.size(); ++i) {
    std::size_t column_index = targets.Value()[i];
    const ColumnDef& column = schema->columns[column_index];
    if (Status status = CheckAssignable(column, bound_values[i]->type)) {
      return *status;
    }
    update.assignments.push_back(
        BoundAssignment{column_index, std::move(bound_values[i])});
  }
  return MakePlan(std::move(update));
}

// Whether an INSERT may leave the column out: it has a default or is the
// auto-increment column.
bool CanOmit(const TableSchema& schema, std::size_t column_index) {
  return schema.columns[column_index].default_value.has_value() ||
         schema.auto_increment_index == column_index;
}

// The column list names each column at most once and covers every column that
// has neither a default nor auto-increment. Returns, for each value of a row,
// its schema column position.
Result<std::vector<std::size_t>> ResolveInsertColumns(
    const InsertStatement& statement, const TableSchema& schema) {
  std::vector<std::size_t> targets;
  if (statement.columns.empty()) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      targets.push_back(i);
    }
  }
  for (const std::string& name : statement.columns) {
    std::optional<std::size_t> column = FindColumnIndex(schema, ToLower(name));
    if (!column) {
      return BindError("unknown column '" + name + "'" +
                       Suggestion(name, ColumnNames(schema)));
    }
    targets.push_back(*column);
  }
  if (std::optional<std::size_t> repeated = FindRepeatedTarget(targets)) {
    return BindError("column '" + statement.columns[*repeated] +
                     "' is listed more than once");
  }
  std::vector<bool> is_supplied(schema.columns.size(), false);
  for (std::size_t target : targets) {
    is_supplied[target] = true;
  }
  for (std::size_t i = 0; i < is_supplied.size(); ++i) {
    if (is_supplied[i] || CanOmit(schema, i)) {
      continue;
    }
    return BindError("no value for column " + schema.columns[i].name);
  }
  return targets;
}

// The error of VALUES row row_number (1-based); a statement with several rows
// names the row in front of the message.
ExecutionError RowError(ExecutionError error, std::size_t row_number,
                        bool has_several_rows) {
  if (has_several_rows) {
    error.message = "row " + std::to_string(row_number) + ": " + error.message;
  }
  return error;
}

// A row of the target table before the values are known: the DEFAULT of every
// omitted column, and a placeholder for the omitted auto-increment column,
// whose position goes to *generated_index.
Row MakeRowTemplate(const TableSchema& schema,
                    const std::vector<std::size_t>& targets,
                    std::optional<std::size_t>* generated_index) {
  Row row(schema.columns.size());
  std::vector<bool> is_supplied(schema.columns.size(), false);
  for (std::size_t target : targets) {
    is_supplied[target] = true;
  }
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (is_supplied[i]) {
      continue;
    }
    if (schema.columns[i].default_value) {
      row[i] = *schema.columns[i].default_value;
    } else {
      *generated_index = i;
      row[i] = int64_t{0};
    }
  }
  return row;
}

// The values of one VALUES row must be as many as the targets and constant
// expressions; they are evaluated and converted to the column types, so the
// result is a finished row. row_template supplies the omitted columns.
Result<Row> BindInsertRow(const std::vector<ExprPtr>& values,
                          const std::vector<std::size_t>& targets,
                          const TableSchema& schema, const Row& row_template) {
  if (values.size() != targets.size()) {
    return BindError("INSERT has " + std::to_string(values.size()) +
                     " values for " + std::to_string(targets.size()) +
                     " columns");
  }
  for (const ExprPtr& value : values) {
    if (!value) {
      return BindError("malformed INSERT value");
    }
    if (ContainsAggregateCall(*value)) {
      return AggregateNotAllowed("VALUES");
    }
    std::vector<const ColumnRef*> refs;
    CollectColumnRefs(*value, refs);
    if (!refs.empty()) {
      return BindError("INSERT values must be constants, found column '" +
                       RefText(*refs[0]) + "'");
    }
  }
  Scope no_tables;
  std::vector<BoundExprPtr> bound_values;
  for (const ExprPtr& value : values) {
    Result<BoundExprPtr> bound = BindValue(*value, no_tables, "VALUES");
    if (!bound.IsOk()) {
      return bound.TakeError();
    }
    bound_values.push_back(std::move(bound.Value()));
  }
  for (std::size_t i = 0; i < bound_values.size(); ++i) {
    const ColumnDef& column = schema.columns[targets[i]];
    if (Status status = CheckAssignable(column, bound_values[i]->type)) {
      return *status;
    }
  }
  Row row = row_template;
  for (std::size_t i = 0; i < bound_values.size(); ++i) {
    const ColumnDef& column = schema.columns[targets[i]];
    std::string failure;
    std::optional<Value> constant = Evaluate(*bound_values[i], Row(), &failure);
    if (!constant) {
      return ExecutionError{ErrorKind::kRuntime, failure};
    }
    // CheckAssignable above rules out a mismatch, so this guards only against
    // the two checks drifting apart.
    std::optional<Value> converted = CoerceValue(*constant, column.type);
    if (!converted) {
      return BindError("cannot convert value for column '" + column.name + "'");
    }
    row[targets[i]] = std::move(*converted);
  }
  return row;
}

// The rows are bound in order and the first error is returned.
Result<PlanPtr> BindInsert(const InsertStatement& statement,
                           const ICatalog& catalog) {
  std::optional<TableSchema> schema = catalog.FindTable(statement.table);
  if (!schema) {
    return UnknownTable(statement.table, catalog);
  }
  Result<std::vector<std::size_t>> targets =
      ResolveInsertColumns(statement, *schema);
  if (!targets.IsOk()) {
    return targets.TakeError();
  }
  if (statement.rows.empty()) {
    return BindError("INSERT has no VALUES rows");
  }
  std::optional<std::size_t> generated_index;
  const Row row_template =
      MakeRowTemplate(*schema, targets.Value(), &generated_index);
  const bool has_several_rows = statement.rows.size() > 1;
  std::vector<Row> rows;
  for (std::size_t i = 0; i < statement.rows.size(); ++i) {
    Result<Row> row = BindInsertRow(statement.rows[i], targets.Value(), *schema,
                                    row_template);
    if (!row.IsOk()) {
      return RowError(row.TakeError(), i + 1, has_several_rows);
    }
    rows.push_back(std::move(row.Value()));
  }
  return MakePlan(InsertNode{schema->name, std::move(rows), generated_index});
}

// ---- DDL -------------------------------------------------------------------

constexpr const char* kReservedTableName = "catalog";

// The DEFAULT of a column: a constant expression, evaluated and converted to
// the column type.
Result<Value> BindDefault(const Expr& expr, const ColumnDef& column) {
  if (ContainsAggregateCall(expr)) {
    return AggregateNotAllowed("DEFAULT");
  }
  std::vector<const ColumnRef*> refs;
  CollectColumnRefs(expr, refs);
  if (!refs.empty()) {
    return BindError("DEFAULT must be a constant, found column '" +
                     RefText(*refs[0]) + "'");
  }
  Scope no_tables;
  Result<BoundExprPtr> bound = BindValue(expr, no_tables, "DEFAULT");
  if (!bound.IsOk()) {
    return bound.TakeError();
  }
  if (Status status = CheckAssignable(column, bound.Value()->type)) {
    return *status;
  }
  std::string failure;
  std::optional<Value> constant = Evaluate(*bound.Value(), Row(), &failure);
  if (!constant) {
    return ExecutionError{ErrorKind::kRuntime, failure};
  }
  std::optional<Value> converted = CoerceValue(*constant, column.type);
  if (!converted) {
    return BindError("cannot convert DEFAULT for column '" + column.name + "'");
  }
  return *converted;
}

Status CheckColumnNamesAreUnique(const CreateTableStatement& statement) {
  std::unordered_set<std::string> seen;
  for (const ColumnDefinition& column : statement.columns) {
    if (!seen.insert(ToLower(column.name)).second) {
      return BindError("column '" + column.name + "' is defined twice");
    }
  }
  return std::nullopt;
}

Status CheckKeyDefinitions(const CreateTableStatement& statement) {
  std::size_t primary_keys = 0;
  for (const ColumnDefinition& column : statement.columns) {
    if (column.is_primary_key) {
      ++primary_keys;
    }
  }
  if (primary_keys != 1) {
    return BindError("table " + statement.table +
                     " must have exactly one PRIMARY KEY column");
  }
  for (const ColumnDefinition& column : statement.columns) {
    if (column.is_primary_key && column.default_value) {
      return BindError("PRIMARY KEY column '" + column.name +
                       "' cannot have a DEFAULT");
    }
    if (!column.is_auto_increment) {
      continue;
    }
    if (column.type != DataType::kInt) {
      return BindError("AUTOINCREMENT column '" + column.name +
                       "' must be INT");
    }
  }
  return std::nullopt;
}

// The foreign key declared by column, or an error when its target is not a
// table already in the catalog, a column of it, of the same type, and unique.
Result<ForeignKey> BindReference(const ColumnDefinition& column,
                                 std::size_t column_index,
                                 const ICatalog& catalog) {
  std::optional<TableSchema> target = catalog.FindTable(column.ref_table);
  if (!target) {
    return UnknownTable(column.ref_table, catalog);
  }
  std::optional<std::size_t> target_index =
      FindColumnIndex(*target, ToLower(column.ref_column));
  if (!target_index) {
    return BindError("unknown column '" + column.ref_table + "." +
                     column.ref_column + "'");
  }
  if (target->columns[*target_index].type != column.type) {
    return BindError("column '" + column.name + "' and '" + target->name + "." +
                     target->columns[*target_index].name +
                     "' have different types");
  }
  bool is_unique = *target_index == target->primary_key_index ||
                   std::ranges::find(target->unique_indexes, *target_index) !=
                       target->unique_indexes.end();
  if (!is_unique) {
    return BindError("'" + target->name + "." +
                     target->columns[*target_index].name +
                     "' is neither a PRIMARY KEY nor UNIQUE");
  }
  return ForeignKey{column_index, target->name, *target_index};
}

Result<PlanPtr> BindCreateTable(const CreateTableStatement& statement,
                                const ICatalog& catalog) {
  if (ToLower(statement.table) == kReservedTableName) {
    return BindError("table name " + std::string(kReservedTableName) +
                     " is reserved");
  }
  if (catalog.FindTable(statement.table)) {
    return BindError("table " + statement.table + " already exists");
  }
  if (Status status = CheckColumnNamesAreUnique(statement)) {
    return *status;
  }
  if (Status status = CheckKeyDefinitions(statement)) {
    return *status;
  }

  TableSchema schema;
  schema.name = statement.table;
  schema.primary_key_index = 0;
  for (std::size_t i = 0; i < statement.columns.size(); ++i) {
    const ColumnDefinition& definition = statement.columns[i];
    ColumnDef column{definition.name, definition.type, std::nullopt};
    if (definition.default_value) {
      Result<Value> value = BindDefault(*definition.default_value, column);
      if (!value.IsOk()) {
        return value.TakeError();
      }
      column.default_value = std::move(value.Value());
    }
    schema.columns.push_back(std::move(column));
  }
  for (std::size_t i = 0; i < statement.columns.size(); ++i) {
    const ColumnDefinition& definition = statement.columns[i];
    if (definition.is_primary_key) {
      schema.primary_key_index = i;
    } else if (definition.is_unique) {
      schema.unique_indexes.push_back(i);
    }
    if (definition.is_auto_increment) {
      schema.auto_increment_index = i;
    }
    if (definition.ref_table.empty()) {
      continue;
    }
    Result<ForeignKey> foreign_key = BindReference(definition, i, catalog);
    if (!foreign_key.IsOk()) {
      return foreign_key.TakeError();
    }
    schema.foreign_keys.push_back(std::move(foreign_key.Value()));
  }
  return MakePlan(CreateTableNode{std::move(schema)});
}

Result<PlanPtr> BindAddColumn(const AddColumnStatement& statement,
                              const ICatalog& catalog) {
  std::optional<TableSchema> schema = catalog.FindTable(statement.table);
  if (!schema) {
    return UnknownTable(statement.table, catalog);
  }
  const ColumnDefinition& definition = statement.column;
  if (FindColumnIndex(*schema, ToLower(definition.name))) {
    return BindError("column '" + definition.name +
                     "' already exists in table " + schema->name);
  }
  if (definition.is_primary_key || definition.is_unique ||
      definition.is_auto_increment || !definition.ref_table.empty()) {
    return BindError(
        "ADD COLUMN cannot declare PRIMARY KEY, UNIQUE, REFERENCES or "
        "AUTOINCREMENT");
  }
  if (!definition.default_value) {
    return BindError("ADD COLUMN requires a DEFAULT for column '" +
                     definition.name + "'");
  }
  ColumnDef column{definition.name, definition.type, std::nullopt};
  Result<Value> value = BindDefault(*definition.default_value, column);
  if (!value.IsOk()) {
    return value.TakeError();
  }
  column.default_value = std::move(value.Value());
  return MakePlan(AddColumnNode{schema->name, std::move(column)});
}

Result<PlanPtr> BindDropTable(const DropTableStatement& statement,
                              const ICatalog& catalog) {
  std::optional<TableSchema> schema = catalog.FindTable(statement.table);
  if (!schema) {
    return UnknownTable(statement.table, catalog);
  }
  for (const std::string& name : catalog.TableNames()) {
    std::optional<TableSchema> child = catalog.FindTable(name);
    if (!child || ToLower(child->name) == ToLower(schema->name)) {
      continue;
    }
    for (const ForeignKey& foreign_key : child->foreign_keys) {
      if (ToLower(foreign_key.ref_table) != ToLower(schema->name)) {
        continue;
      }
      return BindError("table " + schema->name + " is referenced by " +
                       child->name + "." +
                       child->columns[foreign_key.column_index].name);
    }
  }
  return MakePlan(DropTableNode{schema->name});
}

// Statement has exactly seven alternatives, so the last one needs no test.
Result<PlanPtr> BindStatement(const Statement& statement,
                              const ICatalog& catalog) {
  if (const auto* select = std::get_if<SelectStatement>(&statement)) {
    return BindSelect(*select, catalog);
  }
  if (const auto* insert = std::get_if<InsertStatement>(&statement)) {
    return BindInsert(*insert, catalog);
  }
  if (const auto* update = std::get_if<UpdateStatement>(&statement)) {
    return BindUpdate(*update, catalog);
  }
  if (const auto* delete_statement = std::get_if<DeleteStatement>(&statement)) {
    return BindDelete(*delete_statement, catalog);
  }
  if (const auto* create = std::get_if<CreateTableStatement>(&statement)) {
    return BindCreateTable(*create, catalog);
  }
  if (const auto* add_column = std::get_if<AddColumnStatement>(&statement)) {
    return BindAddColumn(*add_column, catalog);
  }
  return BindDropTable(std::get<DropTableStatement>(statement), catalog);
}

}  // namespace

BindResult Bind(const Statement& statement, const ICatalog& catalog) {
  Result<PlanPtr> bound = BindStatement(statement, catalog);
  if (!bound.IsOk()) {
    return bound.TakeError();
  }
  return std::move(bound.Value());
}

}  // namespace strata::domain
