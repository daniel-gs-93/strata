#include "strata/paged/key_range.h"

#include <limits>
#include <variant>

#include "strata/domain/plan.h"

namespace strata::paged {

namespace {

struct Bound {
  std::int64_t value;
  bool is_inclusive;
};

// The bounds taken so far, and whether any conjunct was taken at all.
struct RangeBuilder {
  std::optional<Bound> low;
  std::optional<Bound> high;
  bool has_conjunct = false;

  // The tighter of two lower bounds: the greater value, and exclusive when the
  // values are equal.
  void RaiseLow(Bound candidate) {
    if (!low.has_value() || candidate.value > low->value ||
        (candidate.value == low->value && !candidate.is_inclusive)) {
      low = candidate;
    }
  }
  void LowerHigh(Bound candidate) {
    if (!high.has_value() || candidate.value < high->value ||
        (candidate.value == high->value && !candidate.is_inclusive)) {
      high = candidate;
    }
  }
};

// The operator that holds when the two sides of a comparison swap places.
domain::BinaryOp Flipped(domain::BinaryOp op) {
  switch (op) {
    case domain::BinaryOp::kLt:
      return domain::BinaryOp::kGt;
    case domain::BinaryOp::kLe:
      return domain::BinaryOp::kGe;
    case domain::BinaryOp::kGt:
      return domain::BinaryOp::kLt;
    case domain::BinaryOp::kGe:
      return domain::BinaryOp::kLe;
    default:
      return op;
  }
}

// Takes `column op literal` into the builder; any other operator is skipped.
void TakeComparison(domain::BinaryOp op, std::int64_t literal,
                    RangeBuilder* builder) {
  switch (op) {
    case domain::BinaryOp::kEq:
      builder->RaiseLow(Bound{literal, true});
      builder->LowerHigh(Bound{literal, true});
      break;
    case domain::BinaryOp::kGt:
      builder->RaiseLow(Bound{literal, false});
      break;
    case domain::BinaryOp::kGe:
      builder->RaiseLow(Bound{literal, true});
      break;
    case domain::BinaryOp::kLt:
      builder->LowerHigh(Bound{literal, false});
      break;
    case domain::BinaryOp::kLe:
      builder->LowerHigh(Bound{literal, true});
      break;
    default:
      return;
  }
  builder->has_conjunct = true;
}

bool IsColumn(const domain::BoundExpr& expression, std::size_t column) {
  const auto* bound_column = std::get_if<domain::BoundColumn>(&expression.node);
  return bound_column != nullptr && bound_column->index == column;
}

// The INT value of a literal, or no value for any other expression.
std::optional<std::int64_t> IntLiteral(const domain::BoundExpr& expression) {
  const auto* literal = std::get_if<domain::BoundLiteral>(&expression.node);
  if (literal == nullptr ||
      !std::holds_alternative<std::int64_t>(literal->value)) {
    return std::nullopt;
  }
  return std::get<std::int64_t>(literal->value);
}

void Collect(const domain::BoundExpr& expression, std::size_t column,
             RangeBuilder* builder) {
  const auto* binary = std::get_if<domain::BoundBinary>(&expression.node);
  if (binary == nullptr) {
    return;
  }
  if (binary->op == domain::BinaryOp::kAnd) {
    Collect(*binary->lhs, column, builder);
    Collect(*binary->rhs, column, builder);
    return;
  }
  if (IsColumn(*binary->lhs, column)) {
    if (const std::optional<std::int64_t> literal = IntLiteral(*binary->rhs)) {
      TakeComparison(binary->op, *literal, builder);
    }
  } else if (IsColumn(*binary->rhs, column)) {
    if (const std::optional<std::int64_t> literal = IntLiteral(*binary->lhs)) {
      TakeComparison(Flipped(binary->op), *literal, builder);
    }
  }
}

}  // namespace

std::optional<KeyRange> ExtractKeyRange(const domain::BoundExpr& predicate,
                                        std::size_t column) {
  RangeBuilder builder;
  Collect(predicate, column, &builder);
  if (!builder.has_conjunct) {
    return std::nullopt;
  }
  KeyRange range;
  if (builder.low.has_value()) {
    range.low = builder.low->value;
    range.is_low_inclusive = builder.low->is_inclusive;
  }
  if (builder.high.has_value()) {
    range.high = builder.high->value;
    range.is_high_inclusive = builder.high->is_inclusive;
  }
  return range;
}

std::optional<std::int64_t> KeyRange::FirstKey() const {
  if (!low.has_value() || is_low_inclusive ||
      *low == std::numeric_limits<std::int64_t>::max()) {
    return low;
  }
  return *low + 1;
}

std::optional<std::int64_t> KeyRange::LastKey() const {
  if (!high.has_value() || is_high_inclusive ||
      *high == std::numeric_limits<std::int64_t>::min()) {
    return high;
  }
  return *high - 1;
}

bool KeyRange::IsEmpty() const {
  // An exclusive bound at the end of the 64-bit keys admits nothing.
  if (low.has_value() && !is_low_inclusive &&
      *low == std::numeric_limits<std::int64_t>::max()) {
    return true;
  }
  if (high.has_value() && !is_high_inclusive &&
      *high == std::numeric_limits<std::int64_t>::min()) {
    return true;
  }
  const std::optional<std::int64_t> first = FirstKey();
  const std::optional<std::int64_t> last = LastKey();
  return first.has_value() && last.has_value() && *first > *last;
}

}  // namespace strata::paged
