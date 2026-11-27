#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/expression.h"
#include "strata/domain/scalar_functions.h"
#include "strata/domain/value.h"
#include "test_harness.h"

namespace {

namespace domain = strata::domain;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

domain::BoundExprPtr MakeLiteral(domain::ExprType type, domain::Value value) {
  return std::make_unique<domain::BoundExpr>(
      domain::BoundExpr{type, domain::BoundLiteral{std::move(value)}});
}

domain::BoundExprPtr IntLiteral(int64_t value) {
  return MakeLiteral(domain::ExprType::kInt, domain::Value(value));
}

domain::BoundExprPtr DoubleLiteral(double value) {
  return MakeLiteral(domain::ExprType::kDouble, domain::Value(value));
}

domain::BoundExprPtr TextLiteral(const std::string& value) {
  return MakeLiteral(domain::ExprType::kText, domain::Value(value));
}

domain::ExprType ResultTypeOf(domain::BinaryOp op, const domain::BoundExpr& lhs,
                              const domain::BoundExpr& rhs) {
  if (op == domain::BinaryOp::kConcat) {
    return domain::ExprType::kText;
  }
  const bool is_arithmetic =
      op == domain::BinaryOp::kAdd || op == domain::BinaryOp::kSub ||
      op == domain::BinaryOp::kMul || op == domain::BinaryOp::kDiv;
  if (!is_arithmetic) {
    return domain::ExprType::kBool;
  }
  bool has_double = lhs.type == domain::ExprType::kDouble ||
                    rhs.type == domain::ExprType::kDouble;
  return has_double ? domain::ExprType::kDouble : domain::ExprType::kInt;
}

domain::BoundExprPtr Binary(domain::BinaryOp op, domain::BoundExprPtr lhs,
                            domain::BoundExprPtr rhs) {
  domain::ExprType type = ResultTypeOf(op, *lhs, *rhs);
  return std::make_unique<domain::BoundExpr>(domain::BoundExpr{
      type, domain::BoundBinary{op, std::move(lhs), std::move(rhs)}});
}

// `1 = 1` is true, `1 = 2` is false: the smallest kBool operands.
domain::BoundExprPtr Boolean(bool is_true) {
  return Binary(domain::BinaryOp::kEq, IntLiteral(1),
                IntLiteral(is_true ? 1 : 2));
}

bool IsTrue(const domain::BoundExpr& condition) {
  std::optional<bool> result =
      domain::EvaluateCondition(condition, domain::Row{});
  return result.has_value() && *result;
}

bool IsFalse(const domain::BoundExpr& condition) {
  std::optional<bool> result =
      domain::EvaluateCondition(condition, domain::Row{});
  return result.has_value() && !*result;
}

bool IsInt(const std::optional<domain::Value>& value, int64_t expected) {
  return value.has_value() && std::holds_alternative<int64_t>(*value) &&
         std::get<int64_t>(*value) == expected;
}

bool IsDouble(const std::optional<domain::Value>& value, double expected) {
  return value.has_value() && std::holds_alternative<double>(*value) &&
         std::get<double>(*value) == expected;
}

bool IsInt(const domain::Value& value, int64_t expected) {
  return std::holds_alternative<int64_t>(value) &&
         std::get<int64_t>(value) == expected;
}

bool IsDouble(const domain::Value& value, double expected) {
  return std::holds_alternative<double>(value) &&
         std::get<double>(value) == expected;
}

}  // namespace

TEST(test_expression_numeric_comparison_mixes_int_and_double) {
  CHECK(IsTrue(*Binary(domain::BinaryOp::kLt, IntLiteral(1), IntLiteral(2))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kGt, DoubleLiteral(2.5), IntLiteral(2))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kEq, IntLiteral(2), DoubleLiteral(2.0))));
  // 2^53 + 1 has no DOUBLE representation, so the conversion makes the two
  // equal.
  CHECK(IsTrue(*Binary(domain::BinaryOp::kEq, IntLiteral(9007199254740993),
                       DoubleLiteral(9007199254740992.0))));
}

TEST(test_expression_text_comparison_uses_byte_order) {
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral("a"), TextLiteral("b"))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral("B"), TextLiteral("a"))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral(""), TextLiteral("a"))));
}

TEST(test_expression_arithmetic_keeps_int_or_promotes_to_double) {
  domain::Row row;
  auto sum = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(1), IntLiteral(2)), row);
  auto mixed = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(1), DoubleLiteral(2.5)), row);
  auto difference = domain::Evaluate(
      *Binary(domain::BinaryOp::kSub, IntLiteral(5), IntLiteral(7)), row);
  CHECK(IsInt(sum, 3));
  CHECK(IsDouble(mixed, 3.5));
  CHECK(IsInt(difference, -2));
}

TEST(test_expression_and_or_truth_table) {
  const bool truth_values[] = {true, false};
  for (bool lhs_is_true : truth_values) {
    for (bool rhs_is_true : truth_values) {
      auto and_expr = Binary(domain::BinaryOp::kAnd, Boolean(lhs_is_true),
                             Boolean(rhs_is_true));
      auto or_expr = Binary(domain::BinaryOp::kOr, Boolean(lhs_is_true),
                            Boolean(rhs_is_true));
      CHECK_EQ(IsTrue(*and_expr), lhs_is_true && rhs_is_true);
      CHECK_EQ(IsFalse(*and_expr), !(lhs_is_true && rhs_is_true));
      CHECK_EQ(IsTrue(*or_expr), lhs_is_true || rhs_is_true);
      CHECK_EQ(IsFalse(*or_expr), !(lhs_is_true || rhs_is_true));
    }
  }
}

TEST(test_aggregate_sum_keeps_argument_type) {
  domain::AggregateAccumulator int_sum(domain::AggregateFn::kSum,
                                       domain::DataType::kInt);
  for (int64_t value : {1, 2, 3}) {
    CHECK(int_sum.Add(domain::Value(value)));
  }
  domain::AggregateAccumulator double_sum(domain::AggregateFn::kSum,
                                          domain::DataType::kDouble);
  for (double value : {1.5, 2.5}) {
    CHECK(double_sum.Add(domain::Value(value)));
  }
  CHECK(IsInt(int_sum.Result(), 6));
  CHECK(IsDouble(double_sum.Result(), 4.0));
}

TEST(test_aggregate_empty_and_single_input) {
  domain::AggregateAccumulator empty_count(domain::AggregateFn::kCount,
                                           domain::DataType::kInt);
  domain::AggregateAccumulator empty_int_sum(domain::AggregateFn::kSum,
                                             domain::DataType::kInt);
  domain::AggregateAccumulator empty_double_sum(domain::AggregateFn::kSum,
                                                domain::DataType::kDouble);
  CHECK(IsInt(empty_count.Result(), 0));
  CHECK(IsInt(empty_int_sum.Result(), 0));
  CHECK(IsDouble(empty_double_sum.Result(), 0.0));

  domain::AggregateAccumulator one_count(domain::AggregateFn::kCount,
                                         domain::DataType::kInt);
  domain::AggregateAccumulator one_int_sum(domain::AggregateFn::kSum,
                                           domain::DataType::kInt);
  domain::AggregateAccumulator one_double_sum(domain::AggregateFn::kSum,
                                              domain::DataType::kDouble);
  CHECK(one_count.Add(domain::Value(int64_t{7})));
  CHECK(one_int_sum.Add(domain::Value(int64_t{7})));
  CHECK(one_double_sum.Add(domain::Value(7.25)));
  CHECK(IsInt(one_count.Result(), 1));
  CHECK(IsInt(one_int_sum.Result(), 7));
  CHECK(IsDouble(one_double_sum.Result(), 7.25));
}

TEST(test_expression_integer_overflow_returns_nullopt) {
  domain::Row row;
  auto above_max = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(kInt64Max), IntLiteral(1)),
      row);
  auto below_min = domain::Evaluate(
      *Binary(domain::BinaryOp::kSub, IntLiteral(kInt64Min), IntLiteral(1)),
      row);
  CHECK(!above_max.has_value());
  CHECK(!below_min.has_value());

  domain::AggregateAccumulator sum(domain::AggregateFn::kSum,
                                   domain::DataType::kInt);
  CHECK(sum.Add(domain::Value(kInt64Max)));
  CHECK(!sum.Add(domain::Value(int64_t{1})));
}

TEST(test_expression_comparison_boundaries) {
  CHECK(IsTrue(*Binary(domain::BinaryOp::kLe, IntLiteral(1), IntLiteral(1))));
  CHECK(IsTrue(*Binary(domain::BinaryOp::kGe, IntLiteral(1), IntLiteral(1))));
  CHECK(IsFalse(*Binary(domain::BinaryOp::kNe, IntLiteral(1), IntLiteral(1))));
}

TEST(test_value_format_double_is_fixed_with_decimal_point) {
  CHECK_EQ(domain::FormatValue(domain::Value(2.0)), std::string("2.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(100000.0)),
           std::string("100000.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(0.00001)), std::string("0.00001"));
  CHECK_EQ(domain::FormatValue(domain::Value(1e20)),
           std::string("100000000000000000000.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(-0.5)), std::string("-0.5"));
}

TEST(test_expression_non_finite_double_returns_nullopt) {
  domain::Row row;
  auto sum =
      domain::Evaluate(*Binary(domain::BinaryOp::kAdd, DoubleLiteral(1e308),
                               DoubleLiteral(1e308)),
                       row);
  CHECK(!sum.has_value());

  domain::AggregateAccumulator accumulator(domain::AggregateFn::kSum,
                                           domain::DataType::kDouble);
  CHECK(accumulator.Add(domain::Value(1e308)));
  CHECK(!accumulator.Add(domain::Value(1e308)));
}

// ---- Arithmetic, concatenation, LIKE, functions, aggregates ----------------

namespace {

// A literal of the type of value.
domain::BoundExprPtr LiteralOf(const domain::Value& value) {
  if (std::holds_alternative<int64_t>(value)) {
    return IntLiteral(std::get<int64_t>(value));
  }
  if (std::holds_alternative<double>(value)) {
    return DoubleLiteral(std::get<double>(value));
  }
  return TextLiteral(std::get<std::string>(value));
}

struct Evaluated {
  std::optional<domain::Value> value;
  std::string failure;  // set when value is empty
};

Evaluated EvaluateBinary(domain::BinaryOp op, const domain::Value& lhs,
                         const domain::Value& rhs) {
  Evaluated evaluated;
  domain::BoundExprPtr expr = Binary(op, LiteralOf(lhs), LiteralOf(rhs));
  evaluated.value = domain::Evaluate(*expr, domain::Row{}, &evaluated.failure);
  return evaluated;
}

bool IsText(const std::optional<domain::Value>& value,
            const std::string& expected) {
  return value.has_value() && std::holds_alternative<std::string>(*value) &&
         std::get<std::string>(*value) == expected;
}

bool FailedWith(const Evaluated& evaluated, const std::string& message) {
  return !evaluated.value.has_value() && evaluated.failure == message;
}

Evaluated Apply(domain::ScalarFn fn, domain::ExprType result_type,
                const std::vector<domain::Value>& arguments) {
  Evaluated evaluated;
  evaluated.value = domain::ApplyScalarFunction(fn, result_type, arguments,
                                                &evaluated.failure);
  return evaluated;
}

domain::Value Text(const std::string& text) { return domain::Value(text); }
domain::Value Int(int64_t number) { return domain::Value(number); }
domain::Value Real(double number) { return domain::Value(number); }

}  // namespace

TEST(test_expression_multiplication_and_division_semantics) {
  const std::string overflow = "numeric overflow";
  const std::string zero = "division by zero";
  using Op = domain::BinaryOp;

  CHECK(IsInt(EvaluateBinary(Op::kMul, Int(7), Int(6)).value, 42));
  CHECK(FailedWith(EvaluateBinary(Op::kMul, Int(kInt64Max), Int(2)), overflow));
  CHECK(IsInt(EvaluateBinary(Op::kDiv, Int(7), Int(2)).value, 3));
  CHECK(IsInt(EvaluateBinary(Op::kDiv, Int(-7), Int(2)).value, -3));
  CHECK(IsInt(EvaluateBinary(Op::kDiv, Int(7), Int(-2)).value, -3));
  CHECK(FailedWith(EvaluateBinary(Op::kDiv, Int(7), Int(0)), zero));
  CHECK(
      FailedWith(EvaluateBinary(Op::kDiv, Int(kInt64Min), Int(-1)), overflow));
  CHECK(IsDouble(EvaluateBinary(Op::kDiv, Int(7), Real(2.0)).value, 3.5));
  CHECK(FailedWith(EvaluateBinary(Op::kDiv, Real(1.0), Real(0.0)), zero));
  CHECK(FailedWith(EvaluateBinary(Op::kDiv, Real(1.0), Real(-0.0)), zero));
  CHECK(
      FailedWith(EvaluateBinary(Op::kMul, Real(1e308), Real(10.0)), overflow));
  CHECK(IsInt(EvaluateBinary(Op::kDiv, Int(0), Int(5)).value, 0));
}

TEST(test_expression_concat_joins_text) {
  using Op = domain::BinaryOp;

  CHECK(IsText(EvaluateBinary(Op::kConcat, Text("a"), Text("b")).value, "ab"));
  CHECK(IsText(EvaluateBinary(Op::kConcat, Text(""), Text("")).value, ""));
  CHECK(IsText(EvaluateBinary(Op::kConcat, Text("é"), Text("€")).value, "é€"));
  CHECK(IsText(EvaluateBinary(Op::kConcat, Text("x"), Text("")).value, "x"));

  // A result of exactly 16777216 bytes is allowed; one byte more is refused, so
  // a chain of concatenations cannot multiply a long value without bound.
  const std::string half(8388608, 'a');
  const std::string failure_message =
      "result of || is longer than 16777216 bytes";
  const auto at_the_limit = EvaluateBinary(Op::kConcat, Text(half), Text(half));
  CHECK(at_the_limit.value.has_value());
  CHECK(FailedWith(EvaluateBinary(Op::kConcat, Text(half), Text(half + "b")),
                   failure_message));
  CHECK(FailedWith(EvaluateBinary(Op::kConcat, Text(half + "b"), Text(half)),
                   failure_message));
}

TEST(test_like_matches_wildcards_case_and_utf8) {
  struct LikeCase {
    const char* text;
    const char* pattern;
    bool is_match;
  };
  const LikeCase cases[] = {
      {"abc", "abc", true},     {"abc", "abd", false},
      {"", "%", true},          {"abc", "%", true},
      {"abc", "a%", true},      {"abc", "b%", false},
      {"abc", "%c", true},      {"abc", "%b", false},
      {"abc", "%b%", true},     {"abc", "a_c", true},
      {"abc", "a_d", false},    {"", "_", false},
      {"a", "_", true},         {"ab", "_", false},
      {"", "", true},           {"a", "", false},
      {"ABC", "abc", true},     {"abc", "ABC", true},
      {"é", "É", false},        {"é", "_", true},
      {"€", "_", true},         {"\xF0\x9F\x98\x80", "_", true},
      {"é", "__", false},       {"é", "%", true},
      {"axb", "a%%b", true},    {"aXbYbZc", "a%b%c", true},
      {"abab", "%ab", true},    {"abac", "%ab", false},
      {"a.c", "a.c", true},     {"abc", "a.c", false},
      {"a*c", "a*c", true},     {"a[b]c", "a[b]c", true},
      {"a\\c", "a\\c", true},   {"\xFF", "_", true},
      {"\xFF\xFF", "__", true}, {"\xFF\xFF", "_", false},
      {"é€", "%_", true},       {"é€", "_%", true},
      {"éx", "%x", true},       {"éx", "_x", true},
  };

  for (const LikeCase& like_case : cases) {
    if (domain::LikeMatches(like_case.text, like_case.pattern) !=
        like_case.is_match) {
      strata::testing::ReportFailure(
          __FILE__, __LINE__,
          std::string("LIKE [") + like_case.text + "] [" + like_case.pattern +
              "] expected " + (like_case.is_match ? "true" : "false"));
    }
  }
}

TEST(test_like_worst_case_input_completes) {
  const std::string long_text(20000, 'a');

  CHECK(!domain::LikeMatches(long_text, "%a%a%a%a%a%a%a%a%b"));
  CHECK(domain::LikeMatches(long_text, "%a%a%a%a%a%a%a%a"));
}

TEST(test_expression_string_functions) {
  using Fn = domain::ScalarFn;
  const domain::ExprType kText = domain::ExprType::kText;
  const domain::ExprType kInt = domain::ExprType::kInt;

  CHECK(IsText(Apply(Fn::kLower, kText, {Text("AbÀ")}).value, "abÀ"));
  CHECK(IsText(Apply(Fn::kUpper, kText, {Text("abé")}).value, "ABé"));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("")}).value, 0));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("abc")}).value, 3));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("é€")}).value, 2));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("\xFF")}).value, 1));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("\xE2\x82")}).value, 2));
  CHECK(IsInt(Apply(Fn::kLength, kInt, {Text("\xC0\x80")}).value, 2));
  CHECK(IsText(Apply(Fn::kTrim, kText, {Text("  a b\t")}).value, "a b"));
  CHECK(IsText(Apply(Fn::kTrim, kText, {Text("   ")}).value, ""));
  CHECK(IsText(Apply(Fn::kTrim, kText, {Text("\na\n")}).value, "\na\n"));

  // The same function through Evaluate and a BoundFunction node.
  std::vector<domain::BoundExprPtr> args;
  args.push_back(TextLiteral("abc"));
  domain::BoundExpr call{kText,
                         domain::BoundFunction{Fn::kUpper, std::move(args)}};
  CHECK(IsText(domain::Evaluate(call, domain::Row{}), "ABC"));
}

TEST(test_expression_substr_positions_and_lengths) {
  using Fn = domain::ScalarFn;
  const domain::ExprType kText = domain::ExprType::kText;
  const std::string word = "abcdef";
  const domain::Value text = Text(word);

  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(2), Int(3)}).value, "bcd"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(1), Int(100)}).value,
               "abcdef"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(0), Int(2)}).value, "a"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(-1), Int(3)}).value, "a"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(-5), Int(3)}).value, ""));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(7), Int(1)}).value, ""));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(6), Int(1)}).value, "f"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(3)}).value, "cdef"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(0)}).value, "abcdef"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(-3)}).value, "abcdef"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {text, Int(2), Int(0)}).value, ""));
  CHECK(FailedWith(Apply(Fn::kSubstr, kText, {text, Int(1), Int(-1)}),
                   "negative substring length"));
  CHECK(IsText(Apply(Fn::kSubstr, kText, {Text("é€x"), Int(2), Int(1)}).value,
               "€"));
  CHECK(IsText(
      Apply(Fn::kSubstr, kText, {Text("abc"), Int(2), Int(kInt64Max)}).value,
      "bc"));
  CHECK(IsText(
      Apply(Fn::kSubstr, kText, {Text("abc"), Int(kInt64Min), Int(5)}).value,
      ""));
}

TEST(test_expression_abs_and_round) {
  using Fn = domain::ScalarFn;
  const domain::ExprType kInt = domain::ExprType::kInt;
  const domain::ExprType kDouble = domain::ExprType::kDouble;

  CHECK(IsInt(Apply(Fn::kAbs, kInt, {Int(-5)}).value, 5));
  CHECK(IsInt(Apply(Fn::kAbs, kInt, {Int(5)}).value, 5));
  CHECK(IsDouble(Apply(Fn::kAbs, kDouble, {Real(-2.5)}).value, 2.5));
  CHECK(
      FailedWith(Apply(Fn::kAbs, kInt, {Int(kInt64Min)}), "numeric overflow"));

  struct RoundCase {
    double value;
    int64_t digits;
    double expected;
  };
  const RoundCase cases[] = {
      {2.675, 2, 2.67},
      {2.5, 0, 3.0},
      {-2.5, 0, -3.0},
      {0.5, 0, 1.0},
      {1.005, 2, 1.0},
      {0.285, 2, 0.28},
      {0.125, 2, 0.13},
      {-0.125, 2, -0.13},
      {1234.5678, 2, 1234.57},
      {1234.5678, 0, 1235.0},
      {1.5, 15, 1.5},
      {4503599627370497.0, 2, 4503599627370497.0},
      // The scaled value is above 2^52 while the argument is not: a double
      // here has a step of 1/128, so rounding to 2 digits can move it.
      {45035996273704.984375, 2, 45035996273704.9765625},
  };
  for (const RoundCase& round_case : cases) {
    Evaluated evaluated = Apply(
        Fn::kRound, kDouble, {Real(round_case.value), Int(round_case.digits)});
    CHECK(IsDouble(evaluated.value, round_case.expected));
  }
  CHECK(IsDouble(Apply(Fn::kRound, kDouble, {Int(15), Int(3)}).value, 15.0));
  CHECK(IsDouble(Apply(Fn::kRound, kDouble, {Real(2.5)}).value, 3.0));
  CHECK(FailedWith(Apply(Fn::kRound, kDouble, {Real(1.5), Int(16)}),
                   "ROUND digits out of range"));
  CHECK(FailedWith(Apply(Fn::kRound, kDouble, {Real(1.5), Int(-1)}),
                   "ROUND digits out of range"));
}

TEST(test_expression_cast_conversions) {
  using Fn = domain::ScalarFn;
  const domain::ExprType kInt = domain::ExprType::kInt;
  const domain::ExprType kDouble = domain::ExprType::kDouble;
  const domain::ExprType kText = domain::ExprType::kText;

  CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Int(5)}).value, 5.0));
  CHECK(IsText(Apply(Fn::kCast, kText, {Int(5)}).value, "5"));
  CHECK(IsText(Apply(Fn::kCast, kText, {Real(2.0)}).value, "2.0"));
  CHECK(IsInt(Apply(Fn::kCast, kInt, {Real(2.9)}).value, 2));
  CHECK(IsInt(Apply(Fn::kCast, kInt, {Real(-2.9)}).value, -2));
  CHECK(FailedWith(Apply(Fn::kCast, kInt, {Real(1e19)}), "numeric overflow"));

  struct IntCase {
    const char* text;
    bool is_valid;
    int64_t expected;
  };
  const IntCase int_cases[] = {
      {"42", true, 42},
      {"-7", true, -7},
      {"+7", true, 7},
      {"007", true, 7},
      {"", false, 0},
      {" 1", false, 0},
      {"1 ", false, 0},
      {"1.0", false, 0},
      {"9223372036854775807", true, kInt64Max},
      {"9223372036854775808", false, 0},
      {"-9223372036854775808", true, kInt64Min},
  };
  for (const IntCase& int_case : int_cases) {
    Evaluated evaluated = Apply(Fn::kCast, kInt, {Text(int_case.text)});
    if (int_case.is_valid) {
      CHECK(IsInt(evaluated.value, int_case.expected));
    } else {
      CHECK(FailedWith(evaluated, std::string("cannot cast '") + int_case.text +
                                      "' to INT"));
    }
  }

  CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Text("3.5")}).value, 3.5));
  CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Text("-0.25")}).value, -0.25));
  CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Text("10")}).value, 10.0));
  const std::string huge = "1" + std::string(400, '0');
  const char* const bad_doubles[] = {".5", "5.", "1e3", "abc", ""};
  for (const char* bad : bad_doubles) {
    CHECK(FailedWith(Apply(Fn::kCast, kDouble, {Text(bad)}),
                     std::string("cannot cast '") + bad + "' to DOUBLE"));
  }
  CHECK(FailedWith(Apply(Fn::kCast, kDouble, {Text(huge)}),
                   "cannot cast '" + huge + "' to DOUBLE"));

  CHECK(IsText(Apply(Fn::kCast, kText, {Text("x")}).value, "x"));
  CHECK(IsInt(Apply(Fn::kCast, kInt, {Int(3)}).value, 3));
  CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Real(1.5)}).value, 1.5));
}

TEST(test_expression_cast_reads_subnormal_doubles) {
  using Fn = domain::ScalarFn;
  const domain::ExprType kDouble = domain::ExprType::kDouble;
  const double smallest = std::numeric_limits<double>::denorm_min();
  const double tiny = 1e-310;

  // A subnormal DOUBLE prints in fixed notation and reads back unchanged.
  for (double value : {smallest, tiny, -tiny}) {
    const std::string text = domain::FormatValue(domain::Value(value));
    CHECK(IsDouble(Apply(Fn::kCast, kDouble, {Text(text)}).value, value));
  }
  // Below the smallest subnormal the value would round to 0, so it is refused
  // as an overflow is.
  const std::string vanishing = "0." + std::string(400, '0') + "1";
  CHECK(FailedWith(Apply(Fn::kCast, kDouble, {Text(vanishing)}),
                   "cannot cast '" + vanishing + "' to DOUBLE"));
}

TEST(test_aggregate_accumulator_avg_min_max) {
  domain::AggregateAccumulator average(domain::AggregateFn::kAvg,
                                       domain::DataType::kInt);
  CHECK(average.IsEmpty());
  CHECK(average.Add(Int(1)));
  CHECK(average.Add(Int(2)));
  CHECK(average.Add(Int(4)));
  CHECK(!average.IsEmpty());
  CHECK(IsDouble(average.Result(), 7.0 / 3.0));

  domain::AggregateAccumulator huge(domain::AggregateFn::kAvg,
                                    domain::DataType::kDouble);
  CHECK(huge.Add(Real(1e308)));
  CHECK(!huge.Add(Real(1e308)));

  domain::AggregateAccumulator int_min(domain::AggregateFn::kMin,
                                       domain::DataType::kInt);
  domain::AggregateAccumulator int_max(domain::AggregateFn::kMax,
                                       domain::DataType::kInt);
  for (int64_t number : {3, 1, 4}) {
    CHECK(int_min.Add(Int(number)));
    CHECK(int_max.Add(Int(number)));
  }
  CHECK(IsInt(int_min.Result(), 1));
  CHECK(IsInt(int_max.Result(), 4));

  domain::AggregateAccumulator double_min(domain::AggregateFn::kMin,
                                          domain::DataType::kDouble);
  CHECK(double_min.Add(Real(2.5)));
  CHECK(double_min.Add(Real(-0.5)));
  CHECK(IsDouble(double_min.Result(), -0.5));

  domain::AggregateAccumulator text_min(domain::AggregateFn::kMin,
                                        domain::DataType::kText);
  domain::AggregateAccumulator text_max(domain::AggregateFn::kMax,
                                        domain::DataType::kText);
  for (const char* word : {"b", "a", "c"}) {
    CHECK(text_min.Add(Text(word)));
    CHECK(text_max.Add(Text(word)));
  }
  CHECK(IsText(text_min.Result(), "a"));
  CHECK(IsText(text_max.Result(), "c"));
}
