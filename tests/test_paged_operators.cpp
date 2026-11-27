#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/index_reader.h"
#include "strata/paged/key_range.h"
#include "strata/paged/operators.h"
#include "strata/paged/row_keys.h"
#include "strata/paged/row_stream.h"
#include "test_harness.h"
#include "test_paged_row_fakes.h"
#include "test_support.h"

// The query operators and the hash keys, over in-memory row streams
// (docs/TestDocument.md section 3.1). No page is read.

namespace {

namespace domain = strata::domain;
namespace paged = strata::paged;

using domain::ColumnDef;
using domain::DataType;
using domain::ExecutionError;
using domain::PlanNode;
using domain::PlanPtr;
using domain::Row;
using domain::Value;
using strata::testing::FakeTableReader;
using strata::testing::MapCatalog;
using strata::testing::MustBind;
using strata::testing::ReportFailure;
using strata::testing::StreamLog;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();

// ----------------------------------------------------------------------------
// Values, rows and tables.
// ----------------------------------------------------------------------------

Value Int(int64_t value) { return Value(value); }
Value Real(double value) { return Value(value); }
Value Text(const std::string& value) { return Value(value); }

ColumnDef Column(const std::string& name, DataType type) {
  return ColumnDef{name, type};
}

using Rows = std::vector<Row>;

// A reader and a catalog over the same tables, so a test can bind SQL and run
// the plan over vector streams.
struct TestTables {
  FakeTableReader reader;
  MapCatalog catalog;

  void Add(const std::string& name, const std::vector<ColumnDef>& columns,
           const Rows& rows) {
    domain::TableSchema schema;
    schema.name = name;
    schema.columns = columns;
    schema.primary_key_index = 0;
    catalog.Add(schema);
    reader.AddTable(schema, rows);
  }

  std::shared_ptr<StreamLog> LogOf(const std::string& table) const {
    return reader.LogOf(table);
  }
};

// ----------------------------------------------------------------------------
// Hand-built plans.
// ----------------------------------------------------------------------------

PlanPtr MakePlan(PlanNode node) {
  return std::make_unique<PlanNode>(std::move(node));
}

PlanPtr ScanPlan(const std::string& table) {
  return MakePlan(PlanNode{domain::ScanNode{table}});
}

PlanPtr SortPlan(PlanPtr child, std::vector<domain::SortKey> keys) {
  return MakePlan(
      PlanNode{domain::SortNode{std::move(child), std::move(keys)}});
}

PlanPtr LimitPlan(PlanPtr child, std::optional<int64_t> limit, int64_t offset) {
  return MakePlan(PlanNode{domain::LimitNode{std::move(child), limit, offset}});
}

// The child of the ProjectNode that roots plan, so a test can look at the
// output of the operator below the projection. Reports a failure and returns
// plan itself when the root is not a ProjectNode.
const PlanNode& BelowProject(const PlanNode& plan) {
  const auto* project = std::get_if<domain::ProjectNode>(&plan.node);
  if (project == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "the plan is not rooted at a ProjectNode");
    return plan;
  }
  return *project->child;
}

// ----------------------------------------------------------------------------
// Running operators.
// ----------------------------------------------------------------------------

// What an operator returned until its end or its first error.
struct Outcome {
  std::vector<ColumnDef> columns;
  Rows rows;
  std::optional<ExecutionError> error;
};

std::unique_ptr<paged::IOperator> MustBuild(const PlanNode& plan,
                                            const FakeTableReader& reader) {
  paged::BuildResult built = paged::BuildOperatorTree(plan, reader);
  if (auto* error = std::get_if<ExecutionError>(&built)) {
    ReportFailure(__FILE__, __LINE__,
                  "BuildOperatorTree failed: " + error->message);
    return nullptr;
  }
  return std::move(std::get<std::unique_ptr<paged::IOperator>>(built));
}

// Pulls op until the end of the rows or the first error, as the engine does.
Outcome Drain(paged::IOperator& op) {
  Outcome outcome;
  outcome.columns = op.Columns();
  while (true) {
    paged::OperatorResult result = op.Next();
    if (auto* error = std::get_if<ExecutionError>(&result)) {
      outcome.error = std::move(*error);
      return outcome;
    }
    std::optional<Row>& row = std::get<std::optional<Row>>(result);
    if (!row.has_value()) {
      return outcome;
    }
    outcome.rows.push_back(std::move(*row));
  }
}

Outcome RunPlan(const PlanNode& plan, const FakeTableReader& reader) {
  std::unique_ptr<paged::IOperator> op = MustBuild(plan, reader);
  if (op == nullptr) {
    return Outcome{};
  }
  return Drain(*op);
}

Outcome RunSql(const std::string& sql, const TestTables& tables) {
  PlanPtr plan = MustBind(sql, tables.catalog);
  if (plan == nullptr) {
    return Outcome{};
  }
  return RunPlan(*plan, tables.reader);
}

// The result of one Next call as "row", "end" or "error: <message>".
std::string NextText(paged::IOperator& op) {
  paged::OperatorResult result = op.Next();
  if (const auto* error = std::get_if<ExecutionError>(&result)) {
    return "error: " + error->message;
  }
  return std::get<std::optional<Row>>(result).has_value() ? "row" : "end";
}

// ----------------------------------------------------------------------------
// Describing results.
// ----------------------------------------------------------------------------

std::string RowText(const Row& row) {
  std::string text;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i > 0) {
      text += "|";
    }
    text += domain::FormatValue(row[i]);
  }
  return text;
}

std::vector<std::string> RowTexts(const Rows& rows) {
  std::vector<std::string> texts;
  for (const Row& row : rows) {
    texts.push_back(RowText(row));
  }
  return texts;
}

const char* TypeName(DataType type) {
  switch (type) {
    case DataType::kInt:
      return "INT";
    case DataType::kDouble:
      return "DOUBLE";
    case DataType::kText:
      break;
  }
  return "TEXT";
}

// Each column as "<name>:<type>".
std::vector<std::string> ColumnTexts(const std::vector<ColumnDef>& columns) {
  std::vector<std::string> texts;
  for (const ColumnDef& column : columns) {
    texts.push_back(column.name + ":" + TypeName(column.type));
  }
  return texts;
}

// "none", or "<kind>: <message>" of the error that ended the operator.
std::string ErrorText(const Outcome& outcome) {
  if (!outcome.error.has_value()) {
    return "none";
  }
  return std::string(domain::ErrorKindName(outcome.error->kind)) + ": " +
         outcome.error->message;
}

// ----------------------------------------------------------------------------
// Shared fixtures.
// ----------------------------------------------------------------------------

// l(a, tag) and r(a, tag) for the joins on a. Right has duplicate keys and
// left has rows without a match.
void AddJoinTables(TestTables* tables) {
  tables->Add("l",
              {Column("a", DataType::kInt), Column("tag", DataType::kText)},
              Rows{Row{Int(1), Text("l1")}, Row{Int(2), Text("l2")},
                   Row{Int(3), Text("l3")}, Row{Int(2), Text("l4")}});
  tables->Add("r",
              {Column("a", DataType::kInt), Column("tag", DataType::kText)},
              Rows{Row{Int(2), Text("r1")}, Row{Int(1), Text("r2")},
                   Row{Int(2), Text("r3")}, Row{Int(5), Text("r4")}});
}

}  // namespace

TEST(test_operators_scan_yields_stream_rows_and_unknown_table) {
  TestTables tables;
  tables.Add("visitors",
             {Column("id", DataType::kInt), Column("name", DataType::kText),
              Column("score", DataType::kDouble)},
             Rows{Row{Int(3), Text("carol"), Real(2.5)},
                  Row{Int(1), Text("alice"), Real(0.0)},
                  Row{Int(2), Text("bob"), Real(-1.5)}});

  // The order of the stream is kept, and so are the columns.
  PlanPtr scan = ScanPlan("visitors");
  Outcome outcome = RunPlan(*scan, tables.reader);
  CHECK_EQ(ErrorText(outcome), "none");
  CHECK_EQ(
      RowTexts(outcome.rows),
      (std::vector<std::string>{"3|carol|2.5", "1|alice|0.0", "2|bob|-1.5"}));
  CHECK_EQ(ColumnTexts(outcome.columns),
           (std::vector<std::string>{"id:INT", "name:TEXT", "score:DOUBLE"}));

  // Table names are case-insensitive.
  PlanPtr other_spelling = ScanPlan("ViSiToRs");
  Outcome other = RunPlan(*other_spelling, tables.reader);
  CHECK_EQ(ErrorText(other), "none");
  CHECK_EQ(other.rows.size(), static_cast<std::size_t>(3));

  // A table that does not exist is a kBind error with the name as spelled.
  PlanPtr missing = ScanPlan("NoSuchTable");
  paged::BuildResult built = paged::BuildOperatorTree(*missing, tables.reader);
  const auto* error = std::get_if<ExecutionError>(&built);
  CHECK(error != nullptr);
  if (error == nullptr) {
    return;
  }
  CHECK_EQ(std::string(domain::ErrorKindName(error->kind)), "bind");
  CHECK_EQ(error->message, "unknown table: NoSuchTable");

  // The error of an unknown table below another operator is the same.
  PlanPtr below_sort = SortPlan(ScanPlan("nosuch"), {});
  paged::BuildResult sort_built =
      paged::BuildOperatorTree(*below_sort, tables.reader);
  const auto* sort_error = std::get_if<ExecutionError>(&sort_built);
  CHECK(sort_error != nullptr);
  if (sort_error == nullptr) {
    return;
  }
  CHECK_EQ(sort_error->message, "unknown table: nosuch");
}

TEST(test_operators_filter_project_and_runtime_errors) {
  TestTables tables;
  tables.Add("t",
             {Column("id", DataType::kInt), Column("x", DataType::kInt),
              Column("y", DataType::kInt)},
             Rows{Row{Int(1), Int(1), Int(10)}, Row{Int(2), Int(2), Int(-1)},
                  Row{Int(3), Int(5), Int(10)}, Row{Int(4), Int(4), Int(10)}});

  // Rows that pass and rows that do not; the projected values are computed.
  PlanPtr plan =
      MustBind("SELECT id, 100 / x FROM t WHERE y > 0", tables.catalog);
  if (plan == nullptr) {
    return;
  }
  Outcome outcome = RunPlan(*plan, tables.reader);
  CHECK_EQ(ErrorText(outcome), "none");
  CHECK_EQ(RowTexts(outcome.rows),
           (std::vector<std::string>{"1|100", "3|20", "4|25"}));
  const auto& project = std::get<domain::ProjectNode>(plan->node);
  CHECK_EQ(ColumnTexts(outcome.columns), ColumnTexts(project.output_columns));

  // A division by zero in the projection is a kRuntime error, and the
  // operator is over afterwards.
  TestTables with_zero;
  with_zero.Add("t", {Column("x", DataType::kInt)},
                Rows{Row{Int(1)}, Row{Int(2)}, Row{Int(0)}, Row{Int(5)}});
  PlanPtr divide = MustBind("SELECT 100 / x FROM t", with_zero.catalog);
  if (divide == nullptr) {
    return;
  }
  std::unique_ptr<paged::IOperator> op = MustBuild(*divide, with_zero.reader);
  if (op == nullptr) {
    return;
  }
  CHECK_EQ(NextText(*op), "row");
  CHECK_EQ(NextText(*op), "row");
  CHECK_EQ(NextText(*op), "error: division by zero");
  const std::size_t calls_at_error = with_zero.LogOf("t")->next_calls;
  CHECK_EQ(calls_at_error, static_cast<std::size_t>(3));
  CHECK_EQ(NextText(*op), "end");
  CHECK_EQ(NextText(*op), "end");
  CHECK_EQ(with_zero.LogOf("t")->next_calls, calls_at_error);

  // The kind is kRuntime, in the projection and in the filter alike.
  PlanPtr divide_again = MustBind("SELECT 100 / x FROM t", with_zero.catalog);
  if (divide_again == nullptr) {
    return;
  }
  Outcome projection_error = RunPlan(*divide_again, with_zero.reader);
  CHECK_EQ(ErrorText(projection_error), "runtime: division by zero");
  CHECK_EQ(projection_error.rows.size(), static_cast<std::size_t>(2));

  Outcome filter_error =
      RunSql("SELECT x FROM t WHERE 100 / x > 10", with_zero);
  CHECK_EQ(ErrorText(filter_error), "runtime: division by zero");
  CHECK_EQ(RowTexts(filter_error.rows), (std::vector<std::string>{"1", "2"}));
}

TEST(test_operators_hash_join_matches_and_order) {
  TestTables tables;
  AddJoinTables(&tables);
  PlanPtr plan = MustBind("SELECT l.a, l.tag, r.tag FROM l JOIN r ON l.a = r.a",
                          tables.catalog);
  if (plan == nullptr) {
    return;
  }

  // Left order; the matches of one left row follow the right order; left rows
  // without a match are not output.
  Outcome outcome = RunPlan(*plan, tables.reader);
  CHECK_EQ(ErrorText(outcome), "none");
  CHECK_EQ(RowTexts(outcome.rows),
           (std::vector<std::string>{"1|l1|r2", "2|l2|r1", "2|l2|r3", "2|l4|r1",
                                     "2|l4|r3"}));

  // The join itself: left columns followed by right columns.
  Outcome join = RunPlan(BelowProject(*plan), tables.reader);
  CHECK_EQ(ErrorText(join), "none");
  CHECK_EQ(
      ColumnTexts(join.columns),
      (std::vector<std::string>{"a:INT", "tag:TEXT", "a:INT", "tag:TEXT"}));
  CHECK_EQ(join.rows.size(), static_cast<std::size_t>(5));
  if (join.rows.empty()) {
    return;
  }
  CHECK_EQ(RowText(join.rows[0]), "1|l1|1|r2");
}

TEST(test_operators_hash_join_cross_type_and_large_int_keys) {
  // 2 and 2.0 are in the same bucket and equal.
  TestTables mixed;
  mixed.Add("ints", {Column("v", DataType::kInt)},
            Rows{Row{Int(2)}, Row{Int(3)}, Row{Int(2)}});
  mixed.Add("doubles", {Column("v", DataType::kDouble)},
            Rows{Row{Real(2.0)}, Row{Real(3.5)}, Row{Real(2.0)}});
  Outcome cross = RunSql(
      "SELECT ints.v, doubles.v FROM ints JOIN doubles ON ints.v = doubles.v",
      mixed);
  CHECK_EQ(ErrorText(cross), "none");
  CHECK_EQ(RowTexts(cross.rows),
           (std::vector<std::string>{"2|2.0", "2|2.0", "2|2.0", "2|2.0"}));

  // And the other way round, with the DOUBLE side on the left.
  Outcome reversed = RunSql(
      "SELECT doubles.v, ints.v FROM doubles JOIN ints ON doubles.v = ints.v",
      mixed);
  CHECK_EQ(ErrorText(reversed), "none");
  CHECK_EQ(reversed.rows.size(), static_cast<std::size_t>(4));

  // 2^53 and 2^53 + 1 share one double, so they share a bucket; the hit is
  // confirmed with ValuesEqual and the two INT values do not match.
  TestTables large;
  large.Add("big_left", {Column("v", DataType::kInt)},
            Rows{Row{Int(9007199254740992)}, Row{Int(9007199254740993)}});
  large.Add("big_right", {Column("v", DataType::kInt)},
            Rows{Row{Int(9007199254740993)}, Row{Int(9007199254740992)}});
  Outcome pairs = RunSql(
      "SELECT big_left.v, big_right.v FROM big_left JOIN big_right ON "
      "big_left.v = big_right.v",
      large);
  CHECK_EQ(ErrorText(pairs), "none");
  CHECK_EQ(RowTexts(pairs.rows),
           (std::vector<std::string>{"9007199254740992|9007199254740992",
                                     "9007199254740993|9007199254740993"}));

  // TEXT keys match byte for byte.
  TestTables text;
  text.Add("words", {Column("w", DataType::kText)},
           Rows{Row{Text("a")}, Row{Text("a ")}, Row{Text("A")}});
  text.Add("others", {Column("w", DataType::kText)},
           Rows{Row{Text("a ")}, Row{Text("a")}});
  Outcome words = RunSql(
      "SELECT words.w, others.w FROM words JOIN others ON words.w = others.w",
      text);
  CHECK_EQ(ErrorText(words), "none");
  CHECK_EQ(RowTexts(words.rows), (std::vector<std::string>{"a|a", "a |a "}));
}

namespace {

using PairPredicate = bool (*)(const Row& left, const Row& right);

// The pairs for which predicate holds, by nested loops: left order, and for
// each left row the right order. This is the result computed by hand.
std::vector<std::string> ReferenceJoin(const Rows& left, const Rows& right,
                                       PairPredicate predicate) {
  std::vector<std::string> texts;
  for (const Row& left_row : left) {
    for (const Row& right_row : right) {
      if (!predicate(left_row, right_row)) {
        continue;
      }
      Row combined = left_row;
      combined.insert(combined.end(), right_row.begin(), right_row.end());
      texts.push_back(RowText(combined));
    }
  }
  return texts;
}

int64_t IntAt(const Row& row, std::size_t index) {
  return std::get<int64_t>(row[index]);
}

bool KeyAndLess(const Row& left, const Row& right) {
  return IntAt(left, 0) == IntAt(right, 0) && IntAt(left, 1) < IntAt(right, 1);
}

bool OnlyLess(const Row& left, const Row& right) {
  return IntAt(left, 0) < IntAt(right, 0);
}

bool KeyOnly(const Row& left, const Row& right) {
  return IntAt(left, 0) == IntAt(right, 0);
}

bool TwoKeys(const Row& left, const Row& right) {
  return IntAt(left, 0) == IntAt(right, 0) && IntAt(left, 1) == IntAt(right, 1);
}

}  // namespace

TEST(test_operators_join_residual_and_nested_loop) {
  const Rows left_rows{Row{Int(1), Int(5)}, Row{Int(1), Int(1)},
                       Row{Int(2), Int(7)}, Row{Int(3), Int(0)}};
  const Rows right_rows{Row{Int(1), Int(3)}, Row{Int(1), Int(9)},
                        Row{Int(2), Int(7)}, Row{Int(2), Int(8)},
                        Row{Int(4), Int(0)}};
  TestTables tables;
  tables.Add("l", {Column("a", DataType::kInt), Column("b", DataType::kInt)},
             left_rows);
  tables.Add("r", {Column("a", DataType::kInt), Column("b", DataType::kInt)},
             right_rows);

  struct Case {
    const char* condition;
    PairPredicate predicate;
  };
  const Case cases[] = {
      // Equality plus a residual.
      {"l.a = r.a AND l.b < r.b", &KeyAndLess},
      // No equality at all: a nested loop.
      {"l.a < r.a", &OnlyLess},
      // The operands of the equality in reverse order.
      {"r.a = l.a", &KeyOnly},
      // Two equality conjuncts: the first is the key, the second a residual.
      {"l.a = r.a AND l.b = r.b", &TwoKeys},
  };
  for (const Case& test_case : cases) {
    const std::string sql =
        std::string(
            "SELECT l.a AS la, l.b AS lb, r.a AS ra, r.b AS rb FROM l "
            "JOIN r ON ") +
        test_case.condition;
    Outcome outcome = RunSql(sql, tables);
    CHECK_EQ(ErrorText(outcome), "none");
    const std::vector<std::string> expected =
        ReferenceJoin(left_rows, right_rows, test_case.predicate);
    CHECK_EQ(RowTexts(outcome.rows), expected);
    CHECK(!expected.empty());
  }

  // A failing residual is a kRuntime error: 100 / (r.b - 3) divides by zero
  // for the right row (1, 3).
  Outcome failing = RunSql(
      "SELECT l.a FROM l JOIN r ON l.a = r.a AND 100 / (r.b - 3) > 0", tables);
  CHECK_EQ(ErrorText(failing), "runtime: division by zero");
}

TEST(test_operators_three_table_left_deep_join) {
  TestTables tables;
  tables.Add(
      "t1", {Column("a", DataType::kInt), Column("b", DataType::kInt)},
      Rows{Row{Int(1), Int(10)}, Row{Int(2), Int(20)}, Row{Int(3), Int(30)}});
  tables.Add("t2", {Column("a", DataType::kInt), Column("c", DataType::kInt)},
             Rows{Row{Int(1), Int(100)}, Row{Int(1), Int(101)},
                  Row{Int(2), Int(200)}, Row{Int(4), Int(400)}});
  tables.Add("t3", {Column("b", DataType::kInt), Column("d", DataType::kText)},
             Rows{Row{Int(10), Text("x")}, Row{Int(20), Text("y")},
                  Row{Int(20), Text("z")}, Row{Int(40), Text("w")}});
  // The second join takes its key from t1, the first table.
  PlanPtr plan =
      MustBind("SELECT * FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t1.b = t3.b",
               tables.catalog);
  if (plan == nullptr) {
    return;
  }

  Outcome outcome = RunPlan(*plan, tables.reader);
  CHECK_EQ(ErrorText(outcome), "none");
  CHECK_EQ(RowTexts(outcome.rows),
           (std::vector<std::string>{"1|10|1|100|10|x", "1|10|1|101|10|x",
                                     "2|20|2|200|20|y", "2|20|2|200|20|z"}));

  Outcome join = RunPlan(BelowProject(*plan), tables.reader);
  CHECK_EQ(ErrorText(join), "none");
  CHECK_EQ(join.columns.size(), static_cast<std::size_t>(6));
  CHECK_EQ(ColumnTexts(join.columns),
           (std::vector<std::string>{"a:INT", "b:INT", "a:INT", "c:INT",
                                     "b:INT", "d:TEXT"}));
}

TEST(test_operators_aggregate_groups_and_functions) {
  TestTables tables;
  tables.Add(
      "sales",
      {Column("grp", DataType::kText), Column("qty", DataType::kInt),
       Column("price", DataType::kDouble), Column("label", DataType::kText)},
      Rows{Row{Text("a"), Int(1), Real(1.0), Text("x")},
           Row{Text("b"), Int(5), Real(2.5), Text("m")},
           Row{Text("a"), Int(3), Real(2.0), Text("z")},
           Row{Text("b"), Int(2), Real(4.5), Text("k")},
           Row{Text("c"), Int(7), Real(3.0), Text("q")},
           Row{Text("a"), Int(2), Real(3.0), Text("b")}});
  PlanPtr plan = MustBind(
      "SELECT grp, COUNT(*), SUM(qty), AVG(price), MIN(qty), MAX(qty), "
      "MIN(price), MAX(price), MIN(label), MAX(label) FROM sales GROUP BY grp",
      tables.catalog);
  if (plan == nullptr) {
    return;
  }

  // Groups appear in order of first occurrence.
  Outcome outcome = RunPlan(*plan, tables.reader);
  CHECK_EQ(ErrorText(outcome), "none");
  CHECK_EQ(RowTexts(outcome.rows),
           (std::vector<std::string>{"a|3|6|2.0|1|3|1.0|3.0|b|z",
                                     "b|2|7|3.5|2|5|2.5|4.5|k|m",
                                     "c|1|7|3.0|7|7|3.0|3.0|q|q"}));

  // The aggregate operator itself: group key columns, then one per aggregate.
  Outcome aggregate = RunPlan(BelowProject(*plan), tables.reader);
  CHECK_EQ(ErrorText(aggregate), "none");
  CHECK_EQ(ColumnTexts(aggregate.columns),
           (std::vector<std::string>{"group_key_0:TEXT", "aggregate_0:INT",
                                     "aggregate_1:INT", "aggregate_2:DOUBLE",
                                     "aggregate_3:INT", "aggregate_4:INT",
                                     "aggregate_5:DOUBLE", "aggregate_6:DOUBLE",
                                     "aggregate_7:TEXT", "aggregate_8:TEXT"}));
  CHECK_EQ(aggregate.rows.size(), static_cast<std::size_t>(3));

  // Two group keys are told apart by type and content.
  TestTables pairs;
  pairs.Add("p", {Column("x", DataType::kText), Column("y", DataType::kText)},
            Rows{Row{Text("ab"), Text("c")}, Row{Text("a"), Text("bc")},
                 Row{Text("ab"), Text("c")}});
  Outcome by_pair = RunSql("SELECT x, y, COUNT(*) FROM p GROUP BY x, y", pairs);
  CHECK_EQ(ErrorText(by_pair), "none");
  CHECK_EQ(RowTexts(by_pair.rows),
           (std::vector<std::string>{"ab|c|2", "a|bc|1"}));
}

TEST(test_operators_aggregate_empty_input) {
  TestTables tables;
  tables.Add("empty_t",
             {Column("g", DataType::kInt), Column("v", DataType::kInt),
              Column("d", DataType::kDouble)},
             Rows{});

  // Without grouping one row is returned: COUNT is 0 and SUM is the zero of
  // its type.
  Outcome sums = RunSql("SELECT COUNT(*), SUM(v), SUM(d) FROM empty_t", tables);
  CHECK_EQ(ErrorText(sums), "none");
  CHECK_EQ(RowTexts(sums.rows), (std::vector<std::string>{"0|0|0.0"}));

  // AVG, MIN and MAX have no result.
  Outcome average = RunSql("SELECT AVG(v) FROM empty_t", tables);
  CHECK_EQ(ErrorText(average), "runtime: AVG over no rows");
  CHECK(average.rows.empty());
  Outcome minimum = RunSql("SELECT MIN(v) FROM empty_t", tables);
  CHECK_EQ(ErrorText(minimum), "runtime: MIN over no rows");
  Outcome maximum = RunSql("SELECT MAX(d) FROM empty_t", tables);
  CHECK_EQ(ErrorText(maximum), "runtime: MAX over no rows");
  // One failing function is enough, whatever the others are.
  Outcome mixed = RunSql("SELECT COUNT(*), MAX(v) FROM empty_t", tables);
  CHECK_EQ(ErrorText(mixed), "runtime: MAX over no rows");

  // With grouping there is no group, so no row and no error.
  Outcome grouped = RunSql(
      "SELECT g, COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM empty_t "
      "GROUP BY g",
      tables);
  CHECK_EQ(ErrorText(grouped), "none");
  CHECK(grouped.rows.empty());
}

TEST(test_operators_aggregate_overflow) {
  TestTables tables;
  tables.Add("big_ints", {Column("v", DataType::kInt)},
             Rows{Row{Int(kInt64Max)}, Row{Int(kInt64Max)}});
  tables.Add("big_doubles", {Column("v", DataType::kDouble)},
             Rows{Row{Real(1.7e308)}, Row{Real(1.7e308)}});
  tables.Add("fits", {Column("v", DataType::kInt)},
             Rows{Row{Int(kInt64Max - 1)}, Row{Int(1)}});

  Outcome int_sum = RunSql("SELECT SUM(v) FROM big_ints", tables);
  CHECK_EQ(ErrorText(int_sum), "runtime: numeric overflow");
  CHECK(int_sum.rows.empty());

  Outcome double_sum = RunSql("SELECT SUM(v) FROM big_doubles", tables);
  CHECK_EQ(ErrorText(double_sum), "runtime: numeric overflow");

  // The sum that lands exactly on the boundary is not an overflow.
  Outcome boundary = RunSql("SELECT SUM(v) FROM fits", tables);
  CHECK_EQ(ErrorText(boundary), "none");
  CHECK_EQ(RowTexts(boundary.rows),
           (std::vector<std::string>{"9223372036854775807"}));
}

TEST(test_operators_sort_is_stable_and_multi_key) {
  // Two keys, ascending then descending. Rows with equal keys carry a sequence
  // number in the last column and must keep the input order.
  TestTables items;
  items.Add("items",
            {Column("k1", DataType::kInt), Column("k2", DataType::kInt),
             Column("seq", DataType::kInt)},
            Rows{Row{Int(2), Int(1), Int(1)}, Row{Int(1), Int(5), Int(2)},
                 Row{Int(2), Int(3), Int(3)}, Row{Int(1), Int(5), Int(4)},
                 Row{Int(2), Int(3), Int(5)}, Row{Int(1), Int(9), Int(6)}});
  PlanPtr two_keys = SortPlan(
      ScanPlan("items"), {domain::SortKey{0, false}, domain::SortKey{1, true}});
  Outcome ordered = RunPlan(*two_keys, items.reader);
  CHECK_EQ(ErrorText(ordered), "none");
  CHECK_EQ(RowTexts(ordered.rows),
           (std::vector<std::string>{"1|9|6", "1|5|2", "1|5|4", "2|3|3",
                                     "2|3|5", "2|1|1"}));

  // A single key with many ties keeps the input order, also when descending.
  PlanPtr ties = SortPlan(ScanPlan("items"), {domain::SortKey{0, true}});
  Outcome descending = RunPlan(*ties, items.reader);
  CHECK_EQ(RowTexts(descending.rows),
           (std::vector<std::string>{"2|1|1", "2|3|3", "2|3|5", "1|5|2",
                                     "1|5|4", "1|9|6"}));

  // Stability also holds for an input large enough that an unstable sort
  // would reorder ties: 90 rows with 3 distinct keys.
  Rows many_rows;
  for (int64_t sequence = 0; sequence < 90; ++sequence) {
    many_rows.push_back(Row{Int(sequence % 3), Int(sequence)});
  }
  TestTables many;
  many.Add("many", {Column("k", DataType::kInt), Column("seq", DataType::kInt)},
           many_rows);
  PlanPtr many_plan = SortPlan(ScanPlan("many"), {domain::SortKey{0, false}});
  Outcome many_sorted = RunPlan(*many_plan, many.reader);
  std::vector<std::string> expected_many;
  for (int64_t key = 0; key < 3; ++key) {
    for (int64_t sequence = key; sequence < 90; sequence += 3) {
      expected_many.push_back(std::to_string(key) + "|" +
                              std::to_string(sequence));
    }
  }
  CHECK_EQ(RowTexts(many_sorted.rows), expected_many);

  // INT and DOUBLE compare by value; 2 (INT) and 2.0 (DOUBLE) are equal, so
  // the INT first in the input stays first.
  TestTables numbers;
  numbers.Add("numbers", {Column("v", DataType::kDouble)},
              Rows{Row{Int(2)}, Row{Real(1.5)}, Row{Int(1)}, Row{Real(2.0)},
                   Row{Int(3)}});
  PlanPtr by_value = SortPlan(ScanPlan("numbers"), {domain::SortKey{0, false}});
  Outcome mixed = RunPlan(*by_value, numbers.reader);
  CHECK_EQ(RowTexts(mixed.rows),
           (std::vector<std::string>{"1", "1.5", "2", "2.0", "3"}));

  // TEXT sorts in byte order.
  TestTables words;
  words.Add("words", {Column("w", DataType::kText)},
            Rows{Row{Text("b")}, Row{Text("B")}, Row{Text("a")}, Row{Text("")},
                 Row{Text("ab")}, Row{Text("A")}});
  PlanPtr by_bytes = SortPlan(ScanPlan("words"), {domain::SortKey{0, false}});
  Outcome texts = RunPlan(*by_bytes, words.reader);
  CHECK_EQ(RowTexts(texts.rows),
           (std::vector<std::string>{"", "A", "B", "a", "ab", "b"}));
  // The output columns are the columns of the child.
  CHECK_EQ(ColumnTexts(texts.columns), (std::vector<std::string>{"w:TEXT"}));
}

TEST(test_operators_limit_and_offset_boundaries) {
  TestTables tables;
  tables.Add(
      "five", {Column("v", DataType::kInt)},
      Rows{Row{Int(1)}, Row{Int(2)}, Row{Int(3)}, Row{Int(4)}, Row{Int(5)}});

  struct Case {
    std::optional<int64_t> limit;
    int64_t offset;
    std::vector<std::string> expected;
  };
  const Case cases[] = {
      {0, 0, {}},                                    // LIMIT 0
      {std::nullopt, 0, {"1", "2", "3", "4", "5"}},  // no limit
      {std::nullopt, 5, {}},                         // OFFSET 5
      {std::nullopt, 9, {}},              // OFFSET beyond the row count
      {2, 4, {"5"}},                      // LIMIT 2 OFFSET 4
      {10, 3, {"4", "5"}},                // LIMIT 10 OFFSET 3
      {0, 3, {}},                         // LIMIT 0 with an offset
      {5, 0, {"1", "2", "3", "4", "5"}},  // LIMIT equal to the row count
      {1, 4, {"5"}},                      // the last row only
  };
  for (const Case& test_case : cases) {
    PlanPtr plan =
        LimitPlan(ScanPlan("five"), test_case.limit, test_case.offset);
    Outcome outcome = RunPlan(*plan, tables.reader);
    CHECK_EQ(ErrorText(outcome), "none");
    CHECK_EQ(RowTexts(outcome.rows), test_case.expected);
    CHECK_EQ(ColumnTexts(outcome.columns), (std::vector<std::string>{"v:INT"}));
  }

  // The same through SQL.
  Outcome sql =
      RunSql("SELECT v FROM five ORDER BY v LIMIT 2 OFFSET 1", tables);
  CHECK_EQ(ErrorText(sql), "none");
  CHECK_EQ(RowTexts(sql.rows), (std::vector<std::string>{"2", "3"}));
}

namespace {

// nums(v) with the rows 1 to 6 and a fresh log.
void AddNumbers(TestTables* tables) {
  tables->Add("nums", {Column("v", DataType::kInt)},
              Rows{Row{Int(1)}, Row{Int(2)}, Row{Int(3)}, Row{Int(4)},
                   Row{Int(5)}, Row{Int(6)}});
}

}  // namespace

TEST(test_operators_limit_stops_pulling) {
  // LIMIT 3: the stream is called 3 times and the end is never requested.
  {
    TestTables tables;
    AddNumbers(&tables);
    PlanPtr plan = LimitPlan(ScanPlan("nums"), 3, 0);
    Outcome outcome = RunPlan(*plan, tables.reader);
    CHECK_EQ(RowTexts(outcome.rows), (std::vector<std::string>{"1", "2", "3"}));
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(3));
  }
  // LIMIT 3 OFFSET 2: 2 skipped rows plus 3 returned rows.
  {
    TestTables tables;
    AddNumbers(&tables);
    PlanPtr plan = LimitPlan(ScanPlan("nums"), 3, 2);
    Outcome outcome = RunPlan(*plan, tables.reader);
    CHECK_EQ(RowTexts(outcome.rows), (std::vector<std::string>{"3", "4", "5"}));
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(5));
  }
  // LIMIT 0: the stream is not called, also with an offset.
  {
    TestTables tables;
    AddNumbers(&tables);
    PlanPtr plan = LimitPlan(ScanPlan("nums"), 0, 0);
    Outcome outcome = RunPlan(*plan, tables.reader);
    CHECK(outcome.rows.empty());
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(0));

    PlanPtr with_offset = LimitPlan(ScanPlan("nums"), 0, 4);
    Outcome skipped = RunPlan(*with_offset, tables.reader);
    CHECK(skipped.rows.empty());
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(0));
  }
  // A Sort below the Limit has read everything: 6 rows plus the end.
  {
    TestTables tables;
    AddNumbers(&tables);
    PlanPtr plan =
        LimitPlan(SortPlan(ScanPlan("nums"), {domain::SortKey{0, true}}), 1, 0);
    Outcome outcome = RunPlan(*plan, tables.reader);
    CHECK_EQ(RowTexts(outcome.rows), (std::vector<std::string>{"6"}));
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(7));
  }
  // Rows below a Limit that are not read are not evaluated: the third row
  // would divide by zero, but LIMIT 1 never reaches it.
  {
    TestTables tables;
    tables.Add("t", {Column("x", DataType::kInt)},
               Rows{Row{Int(1)}, Row{Int(2)}, Row{Int(0)}});
    Outcome outcome = RunSql("SELECT 10 / x FROM t LIMIT 1", tables);
    CHECK_EQ(ErrorText(outcome), "none");
    CHECK_EQ(RowTexts(outcome.rows), (std::vector<std::string>{"10"}));
    CHECK_EQ(tables.LogOf("t")->next_calls, static_cast<std::size_t>(1));
  }
}

namespace {

// Pulls op to the end and then three more times; each of those answers the end
// of the rows.
void CheckDoneStaysDone(paged::IOperator& op) {
  while (NextText(op) == "row") {
  }
  CHECK_EQ(NextText(op), "end");
  CHECK_EQ(NextText(op), "end");
  CHECK_EQ(NextText(op), "end");
}

}  // namespace

TEST(test_operators_lifecycle_fresh_and_done) {
  // Sort: building reads nothing; the first Next reads the child to the end
  // (3 rows plus the end); nothing is requested afterwards.
  {
    TestTables tables;
    tables.Add("t", {Column("id", DataType::kInt)},
               Rows{Row{Int(3)}, Row{Int(1)}, Row{Int(2)}});
    PlanPtr plan = MustBind("SELECT id FROM t ORDER BY id", tables.catalog);
    if (plan == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> op = MustBuild(*plan, tables.reader);
    if (op == nullptr) {
      return;
    }
    const std::shared_ptr<StreamLog> log = tables.LogOf("t");
    CHECK_EQ(log->scan_calls, static_cast<std::size_t>(1));
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(0));
    CHECK_EQ(NextText(*op), "row");
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(4));
    CheckDoneStaysDone(*op);
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(4));
  }
  // Aggregate: the same, with one output row.
  {
    TestTables tables;
    tables.Add("t", {Column("id", DataType::kInt)},
               Rows{Row{Int(3)}, Row{Int(1)}, Row{Int(2)}});
    PlanPtr plan = MustBind("SELECT COUNT(*) FROM t", tables.catalog);
    if (plan == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> op = MustBuild(*plan, tables.reader);
    if (op == nullptr) {
      return;
    }
    const std::shared_ptr<StreamLog> log = tables.LogOf("t");
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(0));
    CHECK_EQ(NextText(*op), "row");
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(4));
    CheckDoneStaysDone(*op);
    CHECK_EQ(log->next_calls, static_cast<std::size_t>(4));
  }
  // Hash join: building reads nothing; the first Next reads all of right (4
  // rows plus the end) and only as much of left as it needs for one pair.
  {
    TestTables tables;
    AddJoinTables(&tables);
    PlanPtr plan = MustBind("SELECT l.tag, r.tag FROM l JOIN r ON l.a = r.a",
                            tables.catalog);
    if (plan == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> op = MustBuild(*plan, tables.reader);
    if (op == nullptr) {
      return;
    }
    const std::shared_ptr<StreamLog> left_log = tables.LogOf("l");
    const std::shared_ptr<StreamLog> right_log = tables.LogOf("r");
    CHECK_EQ(left_log->next_calls, static_cast<std::size_t>(0));
    CHECK_EQ(right_log->next_calls, static_cast<std::size_t>(0));
    CHECK_EQ(NextText(*op), "row");
    CHECK_EQ(right_log->next_calls, static_cast<std::size_t>(5));
    CHECK_EQ(left_log->next_calls, static_cast<std::size_t>(1));
    CheckDoneStaysDone(*op);
    CHECK_EQ(right_log->next_calls, static_cast<std::size_t>(5));
    CHECK_EQ(left_log->next_calls, static_cast<std::size_t>(5));
  }
  // Scan, Filter and Limit: after the end of the rows the stream is not asked
  // again.
  {
    TestTables tables;
    AddNumbers(&tables);
    PlanPtr plan = MustBind("SELECT v FROM nums WHERE v > 3", tables.catalog);
    if (plan == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> op = MustBuild(*plan, tables.reader);
    if (op == nullptr) {
      return;
    }
    CheckDoneStaysDone(*op);
    CHECK_EQ(tables.LogOf("nums")->next_calls, static_cast<std::size_t>(7));
  }
  // An error ends the operator: the next call returns no value and the stream
  // is not called again. Sort, Aggregate, hash join and Scan alike.
  {
    TestTables tables;
    AddJoinTables(&tables);
    tables.Add("t", {Column("id", DataType::kInt)},
               Rows{Row{Int(3)}, Row{Int(1)}, Row{Int(2)}});
    const char* statements[] = {
        "SELECT id FROM t ORDER BY id",
        "SELECT COUNT(*) FROM t",
        "SELECT id FROM t",
    };
    for (const char* statement : statements) {
      tables.reader.LogOf("t")->next_calls = 0;
      tables.reader.LogOf("t")->fail_on_call = 2;
      PlanPtr plan = MustBind(statement, tables.catalog);
      if (plan == nullptr) {
        return;
      }
      std::unique_ptr<paged::IOperator> op = MustBuild(*plan, tables.reader);
      if (op == nullptr) {
        return;
      }
      std::string first = NextText(*op);
      if (std::string(statement) == "SELECT id FROM t") {
        CHECK_EQ(first, "row");
        first = NextText(*op);
      }
      CHECK_EQ(first, "error: injected stream failure");
      CHECK_EQ(NextText(*op), "end");
      CHECK_EQ(NextText(*op), "end");
      CHECK_EQ(tables.LogOf("t")->next_calls, static_cast<std::size_t>(2));
    }

    // Hash join: the failure is in the right stream, which is read first.
    tables.reader.LogOf("r")->fail_on_call = 3;
    PlanPtr join = MustBind("SELECT l.tag, r.tag FROM l JOIN r ON l.a = r.a",
                            tables.catalog);
    if (join == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> op = MustBuild(*join, tables.reader);
    if (op == nullptr) {
      return;
    }
    CHECK_EQ(NextText(*op), "error: injected stream failure");
    CHECK_EQ(NextText(*op), "end");
    CHECK_EQ(tables.LogOf("r")->next_calls, static_cast<std::size_t>(3));
    CHECK_EQ(tables.LogOf("l")->next_calls, static_cast<std::size_t>(0));

    // And in the left stream, after the right side was read.
    TestTables left_fails;
    AddJoinTables(&left_fails);
    left_fails.reader.LogOf("l")->fail_on_call = 2;
    PlanPtr join_again = MustBind(
        "SELECT l.tag, r.tag FROM l JOIN r ON l.a = r.a", left_fails.catalog);
    if (join_again == nullptr) {
      return;
    }
    std::unique_ptr<paged::IOperator> left_op =
        MustBuild(*join_again, left_fails.reader);
    if (left_op == nullptr) {
      return;
    }
    // The first left row has one match; the second left call fails.
    std::vector<std::string> results;
    for (int call = 0; call < 5; ++call) {
      results.push_back(NextText(*left_op));
    }
    CHECK_EQ(results,
             (std::vector<std::string>{"row", "error: injected stream failure",
                                       "end", "end", "end"}));
    CHECK_EQ(left_fails.LogOf("l")->next_calls, static_cast<std::size_t>(2));
  }
}

TEST(test_row_keys_exact_and_join_keys) {
  using paged::AppendExactKey;
  using paged::ExactKey;
  using paged::JoinKey;

  // ExactKey tells type and content apart.
  CHECK(ExactKey(Int(1)) != ExactKey(Real(1.0)));
  CHECK(ExactKey(Int(1)) != ExactKey(Text("1")));
  CHECK(ExactKey(Real(1.0)) != ExactKey(Text("1")));
  CHECK(ExactKey(Int(1)) == ExactKey(Int(1)));
  CHECK(ExactKey(Int(1)) != ExactKey(Int(2)));
  CHECK(ExactKey(Text("a")) != ExactKey(Text("a ")));
  CHECK(ExactKey(Text("a")) == ExactKey(Text("a")));
  CHECK(ExactKey(Text("")) != ExactKey(Text(" ")));
  CHECK(ExactKey(Int(9007199254740992)) != ExactKey(Int(9007199254740993)));

  // 0.0 and -0.0 are the same content.
  CHECK(ExactKey(Real(0.0)) == ExactKey(Real(-0.0)));
  CHECK(ExactKey(Real(0.5)) != ExactKey(Real(-0.5)));

  // The encodings of several values can be appended without a collision.
  std::string ab_then_c;
  AppendExactKey(Text("ab"), &ab_then_c);
  AppendExactKey(Text("c"), &ab_then_c);
  std::string a_then_bc;
  AppendExactKey(Text("a"), &a_then_bc);
  AppendExactKey(Text("bc"), &a_then_bc);
  CHECK(ab_then_c != a_then_bc);
  CHECK(ExactKey(Text("ab")) + ExactKey(Text("c")) == ab_then_c);

  std::string int_then_empty;
  AppendExactKey(Int(1), &int_then_empty);
  AppendExactKey(Text(""), &int_then_empty);
  std::string empty_then_int;
  AppendExactKey(Text(""), &empty_then_int);
  AppendExactKey(Int(1), &empty_then_int);
  CHECK(int_then_empty != empty_then_int);

  // One value whose bytes look like the encoding of two values.
  std::string two_values;
  AppendExactKey(Text("x"), &two_values);
  AppendExactKey(Text("y"), &two_values);
  CHECK(ExactKey(Text(two_values)) != two_values);

  // JoinKey: INT and DOUBLE share one key space, TEXT does not.
  CHECK(JoinKey(Int(2)) == JoinKey(Real(2.0)));
  CHECK(JoinKey(Int(2)) != JoinKey(Real(2.5)));
  CHECK(JoinKey(Int(2)) != JoinKey(Int(3)));
  CHECK(JoinKey(Real(0.0)) == JoinKey(Real(-0.0)));
  CHECK(JoinKey(Int(0)) == JoinKey(Real(-0.0)));
  CHECK(JoinKey(Text("2")) != JoinKey(Int(2)));
  CHECK(JoinKey(Text("2")) != JoinKey(Real(2.0)));
  CHECK(JoinKey(Text("a")) != JoinKey(Text("a ")));
  CHECK(JoinKey(Text("a")) == JoinKey(Text("a")));
  CHECK(JoinKey(Text("")) != JoinKey(Int(0)));
  // Two INT values above 2^53 can share one double, hence one JoinKey; the
  // join confirms a bucket hit with ValuesEqual.
  CHECK(JoinKey(Int(9007199254740992)) == JoinKey(Int(9007199254740993)));
}

TEST(test_operators_build_rejects_non_query_node) {
  FakeTableReader reader;

  domain::InsertNode insert;
  insert.table = "t";
  insert.rows.push_back(Row{Int(1)});
  const PlanNode insert_plan{std::move(insert)};

  domain::CreateTableNode create;
  create.schema.name = "t";
  create.schema.columns.push_back(Column("id", DataType::kInt));
  create.schema.primary_key_index = 0;
  const PlanNode create_plan{std::move(create)};

  const PlanNode delete_plan{domain::DeleteNode{"t", nullptr}};
  const PlanNode drop_plan{domain::DropTableNode{"t"}};

  const PlanNode* const plans[] = {&insert_plan, &create_plan, &delete_plan,
                                   &drop_plan};
  for (const PlanNode* plan : plans) {
    paged::BuildResult built = paged::BuildOperatorTree(*plan, reader);
    const auto* error = std::get_if<ExecutionError>(&built);
    CHECK(error != nullptr);
    if (error == nullptr) {
      continue;
    }
    CHECK_EQ(std::string(domain::ErrorKindName(error->kind)), "unsupported");
    CHECK_EQ(error->message, "not a query plan node");
  }

  // A write node below a query operator is rejected the same way.
  PlanPtr below_limit = LimitPlan(
      MakePlan(PlanNode{domain::DropTableNode{"t"}}), std::nullopt, 0);
  paged::BuildResult nested = paged::BuildOperatorTree(*below_limit, reader);
  const auto* nested_error = std::get_if<ExecutionError>(&nested);
  CHECK(nested_error != nullptr);
  if (nested_error == nullptr) {
    return;
  }
  CHECK_EQ(nested_error->message, "not a query plan node");
}

namespace {

// ----------------------------------------------------------------------------
// Key ranges and the choice of the index scan (ADR-0052).
// ----------------------------------------------------------------------------

// The first FilterNode of a plan, found top down and left before right.
const domain::FilterNode* FindFilter(const PlanNode& plan) {
  if (const auto* filter = std::get_if<domain::FilterNode>(&plan.node)) {
    return filter;
  }
  if (const auto* project = std::get_if<domain::ProjectNode>(&plan.node)) {
    return FindFilter(*project->child);
  }
  if (const auto* sort = std::get_if<domain::SortNode>(&plan.node)) {
    return FindFilter(*sort->child);
  }
  if (const auto* limit = std::get_if<domain::LimitNode>(&plan.node)) {
    return FindFilter(*limit->child);
  }
  if (const auto* join = std::get_if<domain::JoinNode>(&plan.node)) {
    if (const domain::FilterNode* left = FindFilter(*join->left)) {
      return left;
    }
    return FindFilter(*join->right);
  }
  return nullptr;
}

// "none", or the range as an interval such as "[3, 9]" or "(-inf, 5)", with
// " empty" when no key can lie in it.
std::string DescribeRange(const std::optional<paged::KeyRange>& range) {
  if (!range.has_value()) {
    return "none";
  }
  std::string text =
      range->low.has_value()
          ? (range->is_low_inclusive ? "[" : "(") + std::to_string(*range->low)
          : "(-inf";
  text += ", ";
  text += range->high.has_value() ? std::to_string(*range->high) +
                                        (range->is_high_inclusive ? "]" : ")")
                                  : "+inf)";
  if (range->IsEmpty()) {
    text += " empty";
  }
  return text;
}

// A table for the key tests: the key in column 0, a TEXT and another INT.
void AddKeyTable(TestTables* tables, const std::string& name) {
  tables->Add(name,
              {Column("id", DataType::kInt), Column("name", DataType::kText),
               Column("other", DataType::kInt)},
              {Row{Int(1), Text("a"), Int(10)}, Row{Int(3), Text("b"), Int(20)},
               Row{Int(5), Text("c"), Int(30)}, Row{Int(7), Text("d"), Int(40)},
               Row{Int(9), Text("e"), Int(50)}});
}

// The terms first to last (last excluded) as a balanced tree of AND with
// parentheses, so that its depth is about log2 of the number of terms; a flat
// chain of 1000 terms is deeper than the parser accepts. Term 500 is the key
// term and the others do not name the key.
std::string BalancedConjunction(int first, int last) {
  if (last - first == 1) {
    return first == 500 ? "id = 77" : "other <> " + std::to_string(first);
  }
  const int middle = first + (last - first) / 2;
  return "(" + BalancedConjunction(first, middle) + " AND " +
         BalancedConjunction(middle, last) + ")";
}

// The WHERE clause with 1000 terms of which one is a key term.
std::string LargeConjunction() { return BalancedConjunction(0, 1000); }

}  // namespace

TEST(test_paged_key_range_extracts_bounds_from_conjuncts) {
  TestTables tables;
  AddKeyTable(&tables, "t");
  struct Case {
    std::string where;
    std::string expected;
  };
  const std::vector<Case> cases = {{"id = 5", "[5, 5]"},
                                   {"5 = id", "[5, 5]"},
                                   {"id < 5", "(-inf, 5)"},
                                   {"5 < id", "(5, +inf)"},
                                   {"id <= 5", "(-inf, 5]"},
                                   {"id > 5", "(5, +inf)"},
                                   {"id >= 5", "[5, +inf)"},
                                   {"5 <= id", "[5, +inf)"},
                                   {"5 > id", "(-inf, 5)"},
                                   {"5 >= id", "(-inf, 5]"},
                                   {"id = -5", "[-5, -5]"},
                                   {"id >= 3 AND id <= 9", "[3, 9]"},
                                   {"id > 3 AND id > 7", "(7, +inf)"},
                                   {"id >= 7 AND id >= 3", "[7, +inf)"},
                                   {"id < 3 AND id < 9", "(-inf, 3)"},
                                   {"id >= 5 AND id > 5", "(5, +inf)"},
                                   {"id < 7 AND id <= 7", "(-inf, 7)"},
                                   {"id = 3 AND id = 4", "[4, 3] empty"},
                                   {LargeConjunction(), "[77, 77]"},
                                   {"id = 5 AND name = 'x'", "[5, 5]"},
                                   {"id <> 5", "none"},
                                   {"id = 5 OR id = 6", "none"},
                                   {"id = 2.5", "none"},
                                   {"id + 0 = 5", "none"},
                                   {"id = 2 + 3", "none"},
                                   {"id = other", "none"},
                                   {"name = 'x'", "none"}};
  for (const Case& test_case : cases) {
    PlanPtr plan =
        MustBind("SELECT * FROM t WHERE " + test_case.where, tables.catalog);
    if (plan == nullptr) {
      continue;
    }
    const domain::FilterNode* filter = FindFilter(*plan);
    CHECK(filter != nullptr);
    if (filter == nullptr) {
      continue;
    }
    const std::string actual =
        DescribeRange(paged::ExtractKeyRange(*filter->predicate, 0));
    if (actual != test_case.expected) {
      // Shows which predicate gave which range.
      CHECK_EQ(test_case.where.substr(0, 60) + " -> " + actual,
               test_case.where.substr(0, 60) + " -> " + test_case.expected);
    }
  }
}

namespace {

// An IIndexReader over in-memory tables that records every ScanRange call. It
// returns all rows of the table whatever the range, so that the filter above it
// has to drop the rows outside the predicate. Only the key column 0 is indexed.
class FakeIndexReader : public paged::IIndexReader {
 public:
  void AddIndexedTable(const domain::TableSchema& schema, const Rows& rows) {
    tables_[domain::ToLower(schema.name)] =
        Table{schema.columns, strata::testing::WithFakeRids(rows)};
  }

  std::optional<std::size_t> IndexedColumn(
      const std::string& table) const override {
    if (tables_.contains(domain::ToLower(table))) {
      return std::size_t{0};
    }
    return std::nullopt;
  }

  paged::ScanResult ScanRange(const std::string& table,
                              const paged::KeyRange& range) const override {
    calls_.push_back(std::make_pair(domain::ToLower(table), range));
    const Table& found = tables_.at(domain::ToLower(table));
    return std::unique_ptr<paged::IRowStream>(
        std::make_unique<strata::testing::VectorRowStream>(found.columns,
                                                           found.rows));
  }

  paged::ContainsResult Contains(const std::string&,
                                 std::int64_t) const override {
    return false;
  }

  const std::vector<std::pair<std::string, paged::KeyRange>>& calls() const {
    return calls_;
  }

 private:
  struct Table {
    std::vector<ColumnDef> columns;
    std::vector<paged::StoredRow> rows;
  };

  std::map<std::string, Table> tables_;
  mutable std::vector<std::pair<std::string, paged::KeyRange>> calls_;
};

}  // namespace

TEST(test_paged_operators_choose_index_scan_by_plan_shape) {
  struct Case {
    const char* sql;
    std::size_t range_calls;
    std::size_t scans_of_t;
    std::size_t scans_of_u;
    std::size_t rows;
  };
  const Case cases[] = {
      {"SELECT * FROM t WHERE id = 5", 1, 0, 0, 1},
      {"SELECT * FROM t WHERE name = 'c'", 0, 1, 0, 1},
      {"SELECT * FROM t", 0, 1, 0, 5},
      {"SELECT * FROM t JOIN u ON t.id = u.id WHERE t.id + u.id = 10", 0, 1, 1,
       1},
      {"SELECT * FROM t JOIN u ON t.id = u.id WHERE t.id = 5", 1, 0, 1, 1},
      {"SELECT * FROM t WHERE id = 3 AND id = 4", 1, 0, 0, 0},
      {"SELECT * FROM u WHERE id = 5", 0, 0, 1, 1}};
  for (const Case& test_case : cases) {
    TestTables tables;
    AddKeyTable(&tables, "t");
    AddKeyTable(&tables, "u");
    // Only t has an index.
    FakeIndexReader index_reader;
    index_reader.AddIndexedTable(
        *tables.catalog.FindTable("t"),
        {Row{Int(1), Text("a"), Int(10)}, Row{Int(3), Text("b"), Int(20)},
         Row{Int(5), Text("c"), Int(30)}, Row{Int(7), Text("d"), Int(40)},
         Row{Int(9), Text("e"), Int(50)}});
    PlanPtr plan = MustBind(test_case.sql, tables.catalog);
    if (plan == nullptr) {
      continue;
    }
    paged::BuildResult built =
        paged::BuildOperatorTree(*plan, tables.reader, &index_reader);
    CHECK(std::holds_alternative<std::unique_ptr<paged::IOperator>>(built));
    if (!std::holds_alternative<std::unique_ptr<paged::IOperator>>(built)) {
      continue;
    }
    // Building the tree reads no row.
    CHECK_EQ(tables.LogOf("t")->next_calls, std::size_t{0});
    const Outcome outcome =
        Drain(*std::get<std::unique_ptr<paged::IOperator>>(built));
    CHECK(!outcome.error.has_value());
    CHECK_EQ(outcome.rows.size(), test_case.rows);
    CHECK_EQ(index_reader.calls().size(), test_case.range_calls);
    CHECK_EQ(tables.LogOf("t")->scan_calls, test_case.scans_of_t);
    CHECK_EQ(tables.LogOf("u")->scan_calls, test_case.scans_of_u);
    if (std::string(test_case.sql).ends_with("id = 3 AND id = 4")) {
      CHECK(index_reader.calls().size() == 1 &&
            index_reader.calls()[0].second.IsEmpty());
    }
    if (std::string(test_case.sql).ends_with("FROM t WHERE id = 5")) {
      CHECK_EQ(DescribeRange(index_reader.calls()[0].second),
               std::string("[5, 5]"));
    }
  }

  // A null index reader reads through the table reader only.
  TestTables tables;
  AddKeyTable(&tables, "t");
  PlanPtr plan = MustBind("SELECT * FROM t WHERE id = 5", tables.catalog);
  if (plan != nullptr) {
    paged::BuildResult built = paged::BuildOperatorTree(*plan, tables.reader);
    CHECK(std::holds_alternative<std::unique_ptr<paged::IOperator>>(built));
    CHECK_EQ(tables.LogOf("t")->scan_calls, std::size_t{1});
  }
}
