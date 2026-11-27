#include "strata/domain/expression.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "strata/domain/scalar_functions.h"

namespace strata::domain {

namespace {

constexpr const char* kOverflowMessage = "numeric overflow";
constexpr const char* kDivisionByZeroMessage = "division by zero";
// The longest TEXT that `||` may produce. A chain of at most 500 operators can
// otherwise turn one long value into hundreds of copies of it.
constexpr std::size_t kMaxConcatBytes = std::size_t{16} * 1024 * 1024;
constexpr const char* kConcatTooLongMessage =
    "result of || is longer than 16777216 bytes";

std::nullopt_t Fail(std::string* failure, const char* message) {
  if (failure != nullptr) {
    *failure = message;
  }
  return std::nullopt;
}

// INT with INT stays INT and can overflow; any DOUBLE operand promotes the
// result to DOUBLE.
std::optional<Value> IntArithmetic(BinaryOp op, int64_t lhs, int64_t rhs,
                                   std::string* failure) {
  int64_t result = 0;
  bool is_overflow = false;
  switch (op) {
    case BinaryOp::kAdd:
      is_overflow = __builtin_add_overflow(lhs, rhs, &result);
      break;
    case BinaryOp::kSub:
      is_overflow = __builtin_sub_overflow(lhs, rhs, &result);
      break;
    case BinaryOp::kMul:
      is_overflow = __builtin_mul_overflow(lhs, rhs, &result);
      break;
    default:
      // kDiv: truncates toward zero, as C++ does.
      if (rhs == 0) {
        return Fail(failure, kDivisionByZeroMessage);
      }
      if (lhs == std::numeric_limits<int64_t>::min() && rhs == -1) {
        is_overflow = true;
      } else {
        result = lhs / rhs;
      }
      break;
  }
  if (is_overflow) {
    return Fail(failure, kOverflowMessage);
  }
  return Value(result);
}

std::optional<Value> DoubleArithmetic(BinaryOp op, double lhs, double rhs,
                                      std::string* failure) {
  double result = 0.0;
  switch (op) {
    case BinaryOp::kAdd:
      result = lhs + rhs;
      break;
    case BinaryOp::kSub:
      result = lhs - rhs;
      break;
    case BinaryOp::kMul:
      result = lhs * rhs;
      break;
    default:
      // kDiv: 0.0 and -0.0 both count as zero.
      if (rhs == 0.0) {
        return Fail(failure, kDivisionByZeroMessage);
      }
      result = lhs / rhs;
      break;
  }
  // An infinite or NaN result would print and compare inconsistently.
  if (!std::isfinite(result)) {
    return Fail(failure, kOverflowMessage);
  }
  return Value(result);
}

std::optional<Value> Arithmetic(BinaryOp op, const Value& lhs, const Value& rhs,
                                std::string* failure) {
  const auto* lhs_int = std::get_if<int64_t>(&lhs);
  const auto* rhs_int = std::get_if<int64_t>(&rhs);
  if (lhs_int != nullptr && rhs_int != nullptr) {
    return IntArithmetic(op, *lhs_int, *rhs_int, failure);
  }
  return DoubleArithmetic(op, ToDouble(lhs), ToDouble(rhs), failure);
}

// What MIN and MAX return while empty; the caller checks IsEmpty() first.
Value ZeroOf(DataType type) {
  switch (type) {
    case DataType::kInt:
      return Value(int64_t{0});
    case DataType::kDouble:
      return Value(0.0);
    case DataType::kText:
      break;
  }
  return Value(std::string());
}

bool CompareWith(BinaryOp op, int ordering) {
  switch (op) {
    case BinaryOp::kEq:
      return ordering == 0;
    case BinaryOp::kNe:
      return ordering != 0;
    case BinaryOp::kLt:
      return ordering < 0;
    case BinaryOp::kLe:
      return ordering <= 0;
    case BinaryOp::kGt:
      return ordering > 0;
    case BinaryOp::kGe:
      return ordering >= 0;
    default:
      break;
  }
  assert(false && "not a comparison operator");
  return false;
}

std::optional<Value> EvaluateBinary(const BoundBinary& binary, const Row& row,
                                    std::string* failure) {
  std::optional<Value> lhs = Evaluate(*binary.lhs, row, failure);
  if (!lhs) {
    return std::nullopt;
  }
  std::optional<Value> rhs = Evaluate(*binary.rhs, row, failure);
  if (!rhs) {
    return std::nullopt;
  }
  if (binary.op == BinaryOp::kConcat) {
    const std::string& left = std::get<std::string>(*lhs);
    const std::string& right = std::get<std::string>(*rhs);
    if (left.size() > kMaxConcatBytes ||
        right.size() > kMaxConcatBytes - left.size()) {
      return Fail(failure, kConcatTooLongMessage);
    }
    return Value(left + right);
  }
  return Arithmetic(binary.op, *lhs, *rhs, failure);
}

std::optional<Value> EvaluateFunction(const BoundExpr& expr,
                                      const BoundFunction& function,
                                      const Row& row, std::string* failure) {
  std::vector<Value> arguments;
  arguments.reserve(function.args.size());
  for (const BoundExprPtr& arg : function.args) {
    std::optional<Value> value = Evaluate(*arg, row, failure);
    if (!value) {
      return std::nullopt;
    }
    arguments.push_back(std::move(*value));
  }
  return ApplyScalarFunction(function.fn, expr.type, arguments, failure);
}

}  // namespace

std::optional<Value> Evaluate(const BoundExpr& expr, const Row& row,
                              std::string* failure) {
  assert(expr.type != ExprType::kBool);
  if (const auto* literal = std::get_if<BoundLiteral>(&expr.node)) {
    return literal->value;
  }
  if (const auto* column = std::get_if<BoundColumn>(&expr.node)) {
    assert(column->index < row.size());
    return row[column->index];
  }
  if (const auto* function = std::get_if<BoundFunction>(&expr.node)) {
    return EvaluateFunction(expr, *function, row, failure);
  }
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  assert(binary != nullptr && "aggregates are rewritten before evaluation");
  return EvaluateBinary(*binary, row, failure);
}

std::optional<bool> EvaluateCondition(const BoundExpr& expr, const Row& row,
                                      std::string* failure) {
  assert(expr.type == ExprType::kBool);
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  assert(binary != nullptr);
  if (binary->op == BinaryOp::kAnd || binary->op == BinaryOp::kOr) {
    std::optional<bool> lhs = EvaluateCondition(*binary->lhs, row, failure);
    if (!lhs) {
      return std::nullopt;
    }
    if (binary->op == BinaryOp::kAnd && !*lhs) {
      return false;
    }
    if (binary->op == BinaryOp::kOr && *lhs) {
      return true;
    }
    return EvaluateCondition(*binary->rhs, row, failure);
  }
  std::optional<Value> lhs = Evaluate(*binary->lhs, row, failure);
  if (!lhs) {
    return std::nullopt;
  }
  std::optional<Value> rhs = Evaluate(*binary->rhs, row, failure);
  if (!rhs) {
    return std::nullopt;
  }
  if (binary->op == BinaryOp::kLike) {
    return LikeMatches(std::get<std::string>(*lhs),
                       std::get<std::string>(*rhs));
  }
  return CompareWith(binary->op, CompareValues(*lhs, *rhs));
}

AggregateAccumulator::AggregateAccumulator(AggregateFn fn,
                                           DataType argument_type)
    : fn_(fn), argument_type_(argument_type) {}

bool AggregateAccumulator::Add(const Value& value) {
  ++count_;
  switch (fn_) {
    case AggregateFn::kCount:
      return true;
    case AggregateFn::kAvg:
      double_sum_ += ToDouble(value);
      return std::isfinite(double_sum_);
    case AggregateFn::kMin:
      if (!extreme_ || CompareValues(value, *extreme_) < 0) {
        extreme_ = value;
      }
      return true;
    case AggregateFn::kMax:
      if (!extreme_ || CompareValues(value, *extreme_) > 0) {
        extreme_ = value;
      }
      return true;
    case AggregateFn::kSum:
      break;
  }
  assert(argument_type_ != DataType::kText);
  if (argument_type_ == DataType::kDouble) {
    double_sum_ += ToDouble(value);
    return std::isfinite(double_sum_);
  }
  return !__builtin_add_overflow(int_sum_, std::get<int64_t>(value), &int_sum_);
}

Value AggregateAccumulator::Result() const {
  switch (fn_) {
    case AggregateFn::kCount:
      return Value(count_);
    case AggregateFn::kAvg:
      return Value(count_ == 0 ? 0.0
                               : double_sum_ / static_cast<double>(count_));
    case AggregateFn::kMin:
    case AggregateFn::kMax:
      if (extreme_) {
        return *extreme_;
      }
      return ZeroOf(argument_type_);
    case AggregateFn::kSum:
      break;
  }
  if (argument_type_ == DataType::kDouble) {
    return Value(double_sum_);
  }
  return Value(int_sum_);
}

}  // namespace strata::domain
