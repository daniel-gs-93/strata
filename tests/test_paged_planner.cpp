#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/plan.h"
#include "strata/domain/value.h"
#include "strata/paged/index_reader.h"
#include "strata/paged/mutation_planner.h"
#include "strata/paged/row_stream.h"
#include "test_harness.h"
#include "test_paged_row_fakes.h"
#include "test_support.h"

// The mutation planner (constraint checks, cascades and the change set of
// INSERT, UPDATE and DELETE), over in-memory row streams
// (docs/TestDocument.md section 3.1). No page is read or written.

namespace {

namespace domain = strata::domain;
namespace paged = strata::paged;

using domain::ColumnDef;
using domain::DataType;
using domain::ExecutionError;
using domain::PlanNode;
using domain::PlanPtr;
using domain::Row;
using domain::TableSchema;
using domain::Value;
using strata::testing::FakeRid;
using strata::testing::FakeTableReader;
using strata::testing::JobsSchema;
using strata::testing::MapCatalog;
using strata::testing::MeasuresSchema;
using strata::testing::MustBind;
using strata::testing::ReactionsSchema;
using strata::testing::StoriesSchema;
using strata::testing::VisitorsSchema;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();

// ----------------------------------------------------------------------------
// Values, rows and schemas.
// ----------------------------------------------------------------------------

Value Int(int64_t value) { return Value(value); }
Value Real(double value) { return Value(value); }
Value Text(const std::string& value) { return Value(value); }

ColumnDef Column(const std::string& name, DataType type) {
  return ColumnDef{name, type};
}

using Rows = std::vector<Row>;

// A table of two columns, `id INT PRIMARY KEY` and one more, with the given
// constraints.
TableSchema MakeSchema(const std::string& name,
                       const std::vector<ColumnDef>& columns,
                       const std::vector<domain::ForeignKey>& foreign_keys,
                       const std::vector<std::size_t>& unique_indexes) {
  TableSchema schema;
  schema.name = name;
  schema.columns = columns;
  schema.primary_key_index = 0;
  schema.foreign_keys = foreign_keys;
  schema.unique_indexes = unique_indexes;
  return schema;
}

// A reader and a catalog over the same tables.
struct PlannerTables {
  FakeTableReader reader;
  MapCatalog catalog;

  void Add(const TableSchema& schema, const Rows& rows) {
    catalog.Add(schema);
    reader.AddTable(schema, rows);
  }

  // How many times the table was scanned, and how many Next calls its streams
  // received.
  std::size_t ScansOf(const std::string& table) const {
    return reader.LogOf(table)->scan_calls;
  }
  std::size_t NextCallsOf(const std::string& table) const {
    return reader.LogOf(table)->next_calls;
  }
};

// visitors, stories and reactions of the catalog fixture. visitors.username is
// UNIQUE and is referenced by stories.username and reactions.username;
// stories.id is referenced by reactions.storyId. Rows follow the fixture of the
// flat-file engine tests; `visitors` can be given in another order.
Rows ShopVisitors() {
  return Rows{Row{Int(1), Text("alice"), Text("Paris")},
              Row{Int(2), Text("carol"), Text("Paris")},
              Row{Int(3), Text("bob"), Text("Rome")},
              Row{Int(4), Text("dave"), Text("Oslo")}};
}

void AddShop(PlannerTables* tables, const Rows& visitors) {
  tables->Add(VisitorsSchema(), visitors);
  tables->Add(StoriesSchema(),
              Rows{Row{Int(10), Text("hello world"), Text("alice"), Int(5)},
                   Row{Int(11), Text("second story"), Text("alice"), Int(0)},
                   Row{Int(12), Text("bobs story"), Text("bob"), Int(7)}});
  tables->Add(ReactionsSchema(),
              Rows{Row{Int(100), Int(10), Text("bob"), Text("like"),
                       Text("None"), Int(1000)},
                   Row{Int(101), Int(10), Text("carol"), Text("comment"),
                       Text("nice"), Int(1001)},
                   Row{Int(102), Int(11), Text("bob"), Text("comment"),
                       Text("ok"), Int(1002)},
                   Row{Int(103), Int(12), Text("alice"), Text("like"),
                       Text("None"), Int(1003)},
                   Row{Int(104), Int(12), Text("carol"), Text("like"),
                       Text("None"), Int(1004)}});
}

void AddShop(PlannerTables* tables) { AddShop(tables, ShopVisitors()); }

// ----------------------------------------------------------------------------
// Hand-built nodes.
// ----------------------------------------------------------------------------

PlanPtr MakePlan(PlanNode node) {
  return std::make_unique<PlanNode>(std::move(node));
}

domain::BoundExprPtr Literal(const Value& value) {
  domain::ExprType type = domain::ExprType::kText;
  if (std::holds_alternative<int64_t>(value)) {
    type = domain::ExprType::kInt;
  } else if (std::holds_alternative<double>(value)) {
    type = domain::ExprType::kDouble;
  }
  return std::make_unique<domain::BoundExpr>(
      domain::BoundExpr{type, domain::BoundLiteral{value}});
}

// UPDATE <table> SET <column_index> = <value> over every row.
PlanPtr UpdateToLiteral(const std::string& table, std::size_t column_index,
                        const Value& value) {
  domain::UpdateNode update;
  update.table = table;
  update.assignments.push_back(
      domain::BoundAssignment{column_index, Literal(value)});
  return MakePlan(PlanNode{std::move(update)});
}

// ----------------------------------------------------------------------------
// Running the planner and describing what it returned.
// ----------------------------------------------------------------------------

// The outcome of PlanMutation. Exactly one field is set, unless a SQL text did
// not bind (the test has already failed then) and neither is.
struct Planned {
  std::optional<paged::MutationPlan> plan;
  std::optional<ExecutionError> error;
};

Planned PlanOf(const PlanNode& node, const PlannerTables& tables) {
  paged::PlanMutationResult result =
      paged::PlanMutation(node, tables.catalog, tables.reader);
  Planned planned;
  if (auto* error = std::get_if<ExecutionError>(&result)) {
    planned.error = std::move(*error);
  } else {
    planned.plan = std::move(std::get<paged::MutationPlan>(result));
  }
  return planned;
}

Planned PlanSql(const std::string& sql, const PlannerTables& tables) {
  PlanPtr node = MustBind(sql, tables.catalog);
  if (node == nullptr) {
    return Planned{};
  }
  return PlanOf(*node, tables);
}

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

std::string RidText(const paged::Rid& rid) {
  return std::to_string(rid.page_id) + "." + std::to_string(rid.slot_id);
}

std::string ErrorLine(const ExecutionError& error) {
  return std::string("error ") + domain::ErrorKindName(error.kind) + ": " +
         error.message;
}

// The whole outcome as lines: "error <kind>: <message>", or "affected <n>"
// followed by one line per change, in order of application:
//   <table> + <row>                       an insert
//   <table> ~ @<page>.<slot> <old> => <new>   an update
//   <table> - @<page>.<slot> <old>        a delete
std::vector<std::string> Report(const Planned& planned) {
  if (planned.error.has_value()) {
    return {ErrorLine(*planned.error)};
  }
  if (!planned.plan.has_value()) {
    return {"no plan and no error"};
  }
  std::vector<std::string> lines;
  lines.push_back("affected " + std::to_string(planned.plan->affected));
  for (const paged::TableChange& change : planned.plan->changes) {
    for (const Row& row : change.inserts) {
      lines.push_back(change.table + " + " + RowText(row));
    }
    for (const paged::RowUpdate& update : change.updates) {
      lines.push_back(change.table + " ~ @" + RidText(update.rid) + " " +
                      RowText(update.old_row) + " => " +
                      RowText(update.new_row));
    }
    for (const paged::RowDelete& deleted : change.deletes) {
      lines.push_back(change.table + " - @" + RidText(deleted.rid) + " " +
                      RowText(deleted.old_row));
    }
  }
  return lines;
}

// The line a single-error outcome produces.
std::vector<std::string> ErrorReport(const std::string& kind,
                                     const std::string& message) {
  return {"error " + kind + ": " + message};
}

std::size_t ChangeCount(const Planned& planned) {
  return planned.plan.has_value() ? planned.plan->changes.size() : 0;
}

}  // namespace

TEST(test_planner_insert_builds_change_set_with_one_scan_per_table) {
  PlannerTables tables;
  AddShop(&tables);

  // inserts are the rows of the statement in order, and affected is their
  // number.
  Planned planned = PlanSql(
      "INSERT INTO stories VALUES (20, 'a', 'alice', 1), (21, 'b', 'bob', 2), "
      "(22, 'c', 'alice', 3)",
      tables);
  CHECK_EQ(Report(planned),
           (std::vector<std::string>{"affected 3", "stories + 20|a|alice|1",
                                     "stories + 21|b|bob|2",
                                     "stories + 22|c|alice|3"}));
  CHECK_EQ(ChangeCount(planned), static_cast<std::size_t>(1));

  // The target and the parent table are each scanned once, and read to their
  // end once (the rows plus the end of the rows). A table the statement does
  // not touch is not scanned.
  CHECK_EQ(tables.ScansOf("stories"), static_cast<std::size_t>(1));
  CHECK_EQ(tables.NextCallsOf("stories"), static_cast<std::size_t>(3 + 1));
  CHECK_EQ(tables.ScansOf("visitors"), static_cast<std::size_t>(1));
  CHECK_EQ(tables.NextCallsOf("visitors"), static_cast<std::size_t>(4 + 1));
  CHECK_EQ(tables.ScansOf("reactions"), static_cast<std::size_t>(0));

  // The scan count does not grow with the number of rows of the statement.
  PlannerTables wide_tables;
  AddShop(&wide_tables);
  std::string sql = "INSERT INTO stories VALUES ";
  for (int row_number = 0; row_number < 40; ++row_number) {
    if (row_number > 0) {
      sql += ", ";
    }
    sql += "(" + std::to_string(100 + row_number) + ", 'p', '" +
           std::string(row_number % 2 == 0 ? "alice" : "bob") + "', " +
           std::to_string(row_number) + ")";
  }
  Planned wide = PlanSql(sql, wide_tables);
  CHECK_EQ(wide.error.has_value(), false);
  CHECK_EQ(wide.plan.has_value() ? wide.plan->affected : 0,
           static_cast<std::size_t>(40));
  CHECK_EQ(wide_tables.ScansOf("stories"), static_cast<std::size_t>(1));
  CHECK_EQ(wide_tables.ScansOf("visitors"), static_cast<std::size_t>(1));
  CHECK_EQ(wide_tables.NextCallsOf("stories"), static_cast<std::size_t>(3 + 1));
  CHECK_EQ(wide_tables.NextCallsOf("visitors"),
           static_cast<std::size_t>(4 + 1));

  // Two foreign keys: reactions has one to stories and one to visitors, and
  // each parent is scanned once.
  PlannerTables reaction_tables;
  AddShop(&reaction_tables);
  Planned reaction = PlanSql(
      "INSERT INTO reactions VALUES (200, 10, 'bob', 'like', 'x', 1), "
      "(201, 11, 'alice', 'like', 'y', 2)",
      reaction_tables);
  CHECK_EQ(reaction.error.has_value(), false);
  CHECK_EQ(reaction_tables.ScansOf("reactions"), static_cast<std::size_t>(1));
  CHECK_EQ(reaction_tables.ScansOf("stories"), static_cast<std::size_t>(1));
  CHECK_EQ(reaction_tables.ScansOf("visitors"), static_cast<std::size_t>(1));
}

namespace {

// The jobs table (id INT PRIMARY KEY AUTOINCREMENT, body TEXT) holding rows
// with the given ids, and the plan of the INSERT text over it.
Planned PlanJobsInsert(const std::vector<int64_t>& existing_ids,
                       const std::string& sql) {
  PlannerTables tables;
  Rows rows;
  for (int64_t id : existing_ids) {
    rows.push_back(Row{Int(id), Text("old")});
  }
  tables.Add(JobsSchema(), rows);
  return PlanSql(sql, tables);
}

}  // namespace

TEST(test_planner_insert_auto_increment) {
  // An empty table starts at 1.
  CHECK_EQ(Report(PlanJobsInsert({}, "INSERT INTO jobs (body) VALUES ('a')")),
           (std::vector<std::string>{"affected 1", "jobs + 1|a"}));

  // A table that holds only a negative value also starts at 1: the maximum is
  // taken with 0.
  CHECK_EQ(Report(PlanJobsInsert({-5}, "INSERT INTO jobs (body) VALUES ('a')")),
           (std::vector<std::string>{"affected 1", "jobs + 1|a"}));
  CHECK_EQ(
      Report(PlanJobsInsert({-5, -2}, "INSERT INTO jobs (body) VALUES ('a')")),
      (std::vector<std::string>{"affected 1", "jobs + 1|a"}));

  // The maximum of an unsorted table plus 1, not the last row plus 1.
  CHECK_EQ(
      Report(PlanJobsInsert({3, 7, 2}, "INSERT INTO jobs (body) VALUES ('a')")),
      (std::vector<std::string>{"affected 1", "jobs + 8|a"}));

  // A multi-row INSERT continues from the first generated value, one more per
  // row.
  CHECK_EQ(
      Report(PlanJobsInsert(
          {3, 7, 2}, "INSERT INTO jobs (body) VALUES ('a'), ('b'), ('c')")),
      (std::vector<std::string>{"affected 3", "jobs + 8|a", "jobs + 9|b",
                                "jobs + 10|c"}));

  // An explicit id is used as given: the column is not generated.
  CHECK_EQ(Report(PlanJobsInsert({3}, "INSERT INTO jobs VALUES (100, 'x')")),
           (std::vector<std::string>{"affected 1", "jobs + 100|x"}));

  // The maximum is the int64_t maximum: kRuntime numeric overflow.
  CHECK_EQ(Report(PlanJobsInsert({1, kInt64Max},
                                 "INSERT INTO jobs (body) VALUES ('a')")),
           ErrorReport("runtime", "numeric overflow"));

  // One below the maximum: one row fits (the value is the maximum), and the
  // second row of a multi-row INSERT overflows. No partial change set.
  CHECK_EQ(
      Report(PlanJobsInsert({kInt64Max - 1},
                            "INSERT INTO jobs (body) VALUES ('a')")),
      (std::vector<std::string>{"affected 1", "jobs + 9223372036854775807|a"}));
  CHECK_EQ(Report(PlanJobsInsert(
               {kInt64Max - 1}, "INSERT INTO jobs (body) VALUES ('a'), ('b')")),
           ErrorReport("runtime", "numeric overflow"));

  // The planner copies the rows of the node: the placeholder stays in it.
  PlannerTables tables;
  tables.Add(JobsSchema(), Rows{Row{Int(4), Text("old")}});
  PlanPtr node =
      MustBind("INSERT INTO jobs (body) VALUES ('a'), ('b')", tables.catalog);
  if (node == nullptr) {
    return;
  }
  const auto& insert = std::get<domain::InsertNode>(node->node);
  const std::vector<Row> rows_before = insert.rows;
  Planned planned = PlanOf(*node, tables);
  CHECK_EQ(Report(planned), (std::vector<std::string>{
                                "affected 2", "jobs + 5|a", "jobs + 6|b"}));
  CHECK(std::get<domain::InsertNode>(node->node).rows == rows_before);
}

namespace {

std::vector<std::string> PlanVisitorsInsert(const std::string& values) {
  PlannerTables tables;
  AddShop(&tables);
  return Report(PlanSql("INSERT INTO visitors VALUES " + values, tables));
}

}  // namespace

TEST(test_planner_insert_duplicate_checks_and_order) {
  // The primary key repeats an existing row.
  CHECK_EQ(PlanVisitorsInsert("(1, 'zed', 'Oslo')"),
           ErrorReport("constraint violation", "duplicate key: visitors.id=1"));

  // The primary key repeats an earlier row of the same statement.
  CHECK_EQ(PlanVisitorsInsert("(9, 'x', 'a'), (9, 'y', 'b')"),
           ErrorReport("constraint violation", "duplicate key: visitors.id=9"));
  CHECK_EQ(PlanVisitorsInsert("(9, 'p', 'x'), (10, 'q', 'y'), (9, 'r', 'z')"),
           ErrorReport("constraint violation", "duplicate key: visitors.id=9"));

  // A UNIQUE value repeats an existing row, or an earlier row of the statement.
  CHECK_EQ(PlanVisitorsInsert("(9, 'alice', 'a')"),
           ErrorReport("constraint violation",
                       "duplicate key: visitors.username=alice"));
  CHECK_EQ(PlanVisitorsInsert("(9, 'neo', 'a'), (10, 'neo', 'b')"),
           ErrorReport("constraint violation",
                       "duplicate key: visitors.username=neo"));

  // One row that violates both: the primary key is reported.
  CHECK_EQ(PlanVisitorsInsert("(1, 'alice', 'a')"),
           ErrorReport("constraint violation", "duplicate key: visitors.id=1"));

  // Two rows that each violate one rule: the first violating row is reported.
  CHECK_EQ(PlanVisitorsInsert("(9, 'alice', 'a'), (1, 'zed', 'b')"),
           ErrorReport("constraint violation",
                       "duplicate key: visitors.username=alice"));
  CHECK_EQ(PlanVisitorsInsert("(1, 'zed', 'a'), (9, 'alice', 'b')"),
           ErrorReport("constraint violation", "duplicate key: visitors.id=1"));

  // Distinct rows pass.
  CHECK_EQ(PlanVisitorsInsert("(9, 'p', 'x'), (10, 'q', 'y')"),
           (std::vector<std::string>{"affected 2", "visitors + 9|p|x",
                                     "visitors + 10|q|y"}));

  // Two UNIQUE columns are checked ascending by column position, whichever
  // order the row names them in.
  TableSchema accounts = MakeSchema(
      "acct",
      {Column("id", DataType::kInt), Column("email", DataType::kText),
       Column("handle", DataType::kText)},
      {}, {1, 2});
  PlannerTables account_tables;
  account_tables.Add(accounts, Rows{Row{Int(1), Text("e1"), Text("h1")}});
  CHECK_EQ(Report(PlanSql("INSERT INTO acct VALUES (2, 'e1', 'h1')",
                          account_tables)),
           ErrorReport("constraint violation", "duplicate key: acct.email=e1"));
  CHECK_EQ(
      Report(
          PlanSql("INSERT INTO acct VALUES (2, 'e2', 'h1')", account_tables)),
      ErrorReport("constraint violation", "duplicate key: acct.handle=h1"));
  CHECK_EQ(
      Report(PlanSql("INSERT INTO acct VALUES (2, 'e2', 'h2'), (3, 'e3', "
                     "'h2')",
                     account_tables)),
      ErrorReport("constraint violation", "duplicate key: acct.handle=h2"));
}

TEST(test_planner_insert_foreign_key) {
  PlannerTables tables;
  AddShop(&tables);

  // A parent value that exists passes.
  CHECK_EQ(Report(PlanSql("INSERT INTO stories VALUES (20, 'c', 'alice', 1)",
                          tables)),
           (std::vector<std::string>{"affected 1", "stories + 20|c|alice|1"}));

  // A parent value that does not exist is reported with both column names.
  CHECK_EQ(
      Report(PlanSql("INSERT INTO stories VALUES (20, 'c', 'zed', 1)", tables)),
      ErrorReport("constraint violation",
                  "no matching row: stories.username=zed not found in "
                  "visitors.username"));

  // The second row of the statement is the one that fails.
  CHECK_EQ(Report(PlanSql("INSERT INTO stories VALUES (20, 'c', 'alice', 1), "
                          "(21, 'd', 'zed', 1)",
                          tables)),
           ErrorReport("constraint violation",
                       "no matching row: stories.username=zed not found in "
                       "visitors.username"));

  // Uniqueness is checked before the foreign key: id 10 exists and so does not
  // visitor zed.
  CHECK_EQ(
      Report(PlanSql("INSERT INTO stories VALUES (10, 'c', 'zed', 1)", tables)),
      ErrorReport("constraint violation", "duplicate key: stories.id=10"));

  // Two foreign keys are checked in declaration order, with the column spelled
  // as declared.
  CHECK_EQ(Report(PlanSql("INSERT INTO reactions VALUES (200, 999, 'zed', "
                          "'like', 'x', 1)",
                          tables)),
           ErrorReport("constraint violation",
                       "no matching row: reactions.storyId=999 not found in "
                       "stories.id"));
  CHECK_EQ(Report(PlanSql("INSERT INTO reactions VALUES (200, 10, 'zed', "
                          "'like', 'x', 1)",
                          tables)),
           ErrorReport("constraint violation",
                       "no matching row: reactions.username=zed not found in "
                       "visitors.username"));

  // An empty parent table has no matching row for any value.
  PlannerTables empty_parent;
  empty_parent.Add(VisitorsSchema(), Rows{});
  empty_parent.Add(StoriesSchema(), Rows{});
  CHECK_EQ(Report(PlanSql("INSERT INTO stories VALUES (20, 'c', 'alice', 1)",
                          empty_parent)),
           ErrorReport("constraint violation",
                       "no matching row: stories.username=alice not found in "
                       "visitors.username"));

  // A foreign key to a table that the catalog does not have.
  PlannerTables dangling;
  dangling.Add(MakeSchema("orphan",
                          {Column("id", DataType::kInt),
                           Column("ghost_key", DataType::kText)},
                          {domain::ForeignKey{1, "ghost", 0}}, {}),
               Rows{});
  CHECK_EQ(Report(PlanSql("INSERT INTO orphan VALUES (1, 'k')", dangling)),
           ErrorReport("bind", "unknown table: ghost"));
}

namespace {

// blobs(id INT PRIMARY KEY, body TEXT UNIQUE): a tuple is 8 + 2 + the length of
// body bytes.
TableSchema BlobsSchema() {
  return MakeSchema(
      "blobs", {Column("id", DataType::kInt), Column("body", DataType::kText)},
      {}, {1});
}

const char* kRowTooLong =
    "paged engine cannot store a row longer than 4060 bytes in ";

}  // namespace

TEST(test_planner_row_length_limit) {
  PlannerTables tables;
  tables.Add(BlobsSchema(),
             Rows{Row{Int(1), Text("x")}, Row{Int(2), Text("y")}});
  const std::string fits(4050, 'k');      // 8 + 2 + 4050 = 4060
  const std::string too_long(4051, 'k');  // 4061

  // INSERT: exactly 4060 bytes passes, 4061 is kUnsupported.
  Planned fitting_insert =
      PlanSql("INSERT INTO blobs VALUES (3, '" + fits + "')", tables);
  CHECK_EQ(fitting_insert.error.has_value(), false);
  CHECK_EQ(fitting_insert.plan.has_value() ? fitting_insert.plan->affected : 0,
           static_cast<std::size_t>(1));
  CHECK_EQ(Report(PlanSql("INSERT INTO blobs VALUES (3, '" + too_long + "')",
                          tables)),
           ErrorReport("unsupported", std::string(kRowTooLong) + "blobs"));

  // UPDATE: the changed row is checked in the same way.
  Planned fitting_update =
      PlanSql("UPDATE blobs SET body = '" + fits + "' WHERE id = 1", tables);
  CHECK_EQ(fitting_update.error.has_value(), false);
  CHECK_EQ(ChangeCount(fitting_update), static_cast<std::size_t>(1));
  CHECK_EQ(
      Report(PlanSql("UPDATE blobs SET body = '" + too_long + "' WHERE id = 1",
                     tables)),
      ErrorReport("unsupported", std::string(kRowTooLong) + "blobs"));

  // The length check comes before uniqueness: a row that duplicates the primary
  // key and is too long reports the length. The same holds for an UPDATE that
  // gives two rows the same overlong value.
  CHECK_EQ(Report(PlanSql("INSERT INTO blobs VALUES (1, '" + too_long + "')",
                          tables)),
           ErrorReport("unsupported", std::string(kRowTooLong) + "blobs"));
  CHECK_EQ(
      Report(PlanSql("UPDATE blobs SET body = '" + too_long + "'", tables)),
      ErrorReport("unsupported", std::string(kRowTooLong) + "blobs"));

  // A cascade that makes a child row too long: the child tuple is 8 + (2 + tag)
  // + (2 + pad) bytes, and the pad makes it 4060 with the one-byte tag 'a'.
  TableSchema parent = MakeSchema(
      "p", {Column("id", DataType::kInt), Column("tag", DataType::kText)}, {},
      {1});
  TableSchema child =
      MakeSchema("c",
                 {Column("id", DataType::kInt), Column("tag", DataType::kText),
                  Column("pad", DataType::kText)},
                 {domain::ForeignKey{1, "p", 1}}, {});
  const std::string pad(4047, 'q');
  PlannerTables cascade_tables;
  cascade_tables.Add(parent,
                     Rows{Row{Int(1), Text("a")}, Row{Int(2), Text("b")}});
  cascade_tables.Add(child, Rows{Row{Int(1), Text("a"), Text(pad)}});

  // The tag grows by one byte: the child row is 4061 bytes, so the statement is
  // rejected, and the message names the child table.
  CHECK_EQ(
      Report(PlanSql("UPDATE p SET tag = 'zz' WHERE id = 1", cascade_tables)),
      ErrorReport("unsupported", std::string(kRowTooLong) + "c"));

  // The tag keeps its length: the child row stays at 4060 and is rewritten.
  Planned same_length =
      PlanSql("UPDATE p SET tag = 'z' WHERE id = 1", cascade_tables);
  CHECK_EQ(same_length.error.has_value(), false);
  CHECK_EQ(ChangeCount(same_length), static_cast<std::size_t>(2));
  CHECK_EQ(Report(same_length).size(), static_cast<std::size_t>(1 + 2));
}

namespace {

// pairs(id INT PRIMARY KEY, a INT, b INT) with three rows.
void AddPairs(PlannerTables* tables) {
  tables->Add(
      MakeSchema("pairs",
                 {Column("id", DataType::kInt), Column("a", DataType::kInt),
                  Column("b", DataType::kInt)},
                 {}, {}),
      Rows{Row{Int(1), Int(1), Int(2)}, Row{Int(2), Int(5), Int(5)},
           Row{Int(3), Int(7), Int(8)}});
}

}  // namespace

TEST(test_planner_update_assignments_and_noop) {
  PlannerTables tables;
  AddPairs(&tables);

  // Assignments are evaluated against the old row: a and b are swapped.
  CHECK_EQ(
      Report(PlanSql("UPDATE pairs SET a = b, b = a WHERE id = 1", tables)),
      (std::vector<std::string>{"affected 1", "pairs ~ @1.0 1|1|2 => 1|2|1"}));

  // A row whose swap gives the same values (5, 5) produces no RowUpdate, but
  // counts in affected.
  CHECK_EQ(
      Report(PlanSql("UPDATE pairs SET a = b, b = a", tables)),
      (std::vector<std::string>{"affected 3", "pairs ~ @1.0 1|1|2 => 1|2|1",
                                "pairs ~ @1.2 3|7|8 => 3|8|7"}));

  // Several rows are selected and only one changes.
  CHECK_EQ(
      Report(PlanSql("UPDATE pairs SET a = 5 WHERE id <= 2", tables)),
      (std::vector<std::string>{"affected 2", "pairs ~ @1.0 1|1|2 => 1|5|2"}));

  // An assignment equal to the stored value: nothing changes, and affected is
  // the number of selected rows.
  Planned unchanged = PlanSql("UPDATE pairs SET a = a", tables);
  CHECK_EQ(Report(unchanged), (std::vector<std::string>{"affected 3"}));
  CHECK_EQ(ChangeCount(unchanged), static_cast<std::size_t>(0));

  // No row selected: affected is 0 and nothing changes.
  CHECK_EQ(Report(PlanSql("UPDATE pairs SET a = 1 WHERE id = 99", tables)),
           (std::vector<std::string>{"affected 0"}));

  // An INT assigned to a DOUBLE column is stored as a DOUBLE.
  PlannerTables measures;
  measures.Add(MeasuresSchema(),
               Rows{Row{Int(1), Real(0.0)}, Row{Int(2), Real(2.5)}});
  Planned converted =
      PlanSql("UPDATE measures SET score = 3 WHERE id = 1", measures);
  CHECK_EQ(Report(converted),
           (std::vector<std::string>{"affected 1",
                                     "measures ~ @1.0 1|0.0 => 1|3.0"}));
  if (converted.plan.has_value() && !converted.plan->changes.empty() &&
      !converted.plan->changes[0].updates.empty()) {
    CHECK(std::holds_alternative<double>(
        converted.plan->changes[0].updates[0].new_row[1]));
  } else {
    CHECK(false);
  }

  // -0.0 is the same value as 0.0: no RowUpdate, in either direction.
  PlanPtr to_negative_zero = UpdateToLiteral("measures", 1, Real(-0.0));
  CHECK_EQ(Report(PlanOf(*to_negative_zero, measures)),
           (std::vector<std::string>{"affected 2",
                                     "measures ~ @1.1 2|2.5 => 2|-0.0"}));
  PlannerTables zeros;
  zeros.Add(MeasuresSchema(), Rows{Row{Int(1), Real(0.0)}});
  PlanPtr negative_zero_assigned = UpdateToLiteral("measures", 1, Real(-0.0));
  CHECK_EQ(Report(PlanOf(*negative_zero_assigned, zeros)),
           (std::vector<std::string>{"affected 1"}));
  PlannerTables negative_zeros;
  negative_zeros.Add(MeasuresSchema(), Rows{Row{Int(1), Real(-0.0)}});
  PlanPtr zero_assigned = UpdateToLiteral("measures", 1, Real(0.0));
  CHECK_EQ(Report(PlanOf(*zero_assigned, negative_zeros)),
           (std::vector<std::string>{"affected 1"}));

  // The stored cell is kept when the assigned value equals it, also when
  // another cell of the same row changes: the row is rewritten, and the sign
  // of the zero stays as stored.
  PlannerTables readings;
  readings.Add(
      MakeSchema("readings",
                 {Column("id", DataType::kInt), Column("d", DataType::kDouble),
                  Column("n", DataType::kInt)},
                 {}, {}),
      Rows{Row{Int(1), Real(0.0), Int(1)}, Row{Int(2), Real(-0.0), Int(1)}});
  CHECK_EQ(Report(PlanSql("UPDATE readings SET d = -0.0, n = 2 WHERE id = 1",
                          readings)),
           (std::vector<std::string>{"affected 1",
                                     "readings ~ @1.0 1|0.0|1 => 1|0.0|2"}));
  CHECK_EQ(Report(PlanSql("UPDATE readings SET d = 0, n = 6 WHERE id = 2",
                          readings)),
           (std::vector<std::string>{"affected 1",
                                     "readings ~ @1.1 2|-0.0|1 => 2|-0.0|6"}));

  // A value that cannot be converted to the column type is a kBind error (the
  // Binder rejects it first, so this plan is built by hand).
  PlanPtr text_to_int = UpdateToLiteral("pairs", 1, Text("seven"));
  CHECK_EQ(Report(PlanOf(*text_to_int, tables)),
           ErrorReport("bind", "type mismatch assigning to pairs.a"));
}

namespace {

// slots(id INT PRIMARY KEY, pos INT UNIQUE, label TEXT).
TableSchema SlotsSchema() {
  return MakeSchema(
      "slots",
      {Column("id", DataType::kInt), Column("pos", DataType::kInt),
       Column("label", DataType::kText)},
      {}, {1});
}

std::vector<std::string> PlanSlotsUpdate(const Rows& rows,
                                         const std::string& sql) {
  PlannerTables tables;
  tables.Add(SlotsSchema(), rows);
  return Report(PlanSql(sql, tables));
}

}  // namespace

TEST(test_planner_update_uniqueness) {
  const Rows distinct{Row{Int(1), Int(1), Text("x")},
                      Row{Int(2), Int(2), Text("y")},
                      Row{Int(3), Int(3), Text("z")}};

  // A change to the value of another row.
  CHECK_EQ(PlanSlotsUpdate(distinct, "UPDATE slots SET pos = 2 WHERE id = 1"),
           ErrorReport("constraint violation", "duplicate key: slots.pos=2"));

  // A swap of two rows is legal: the final state is distinct.
  CHECK_EQ(
      PlanSlotsUpdate(distinct, "UPDATE slots SET pos = 3 - pos WHERE id < 3"),
      (std::vector<std::string>{"affected 2", "slots ~ @1.0 1|1|x => 1|2|x",
                                "slots ~ @1.1 2|2|y => 2|1|y"}));

  // A selected row that does not change still holds its value in the final
  // state: row 1 moves onto the value of row 2, which keeps it.
  CHECK_EQ(PlanSlotsUpdate(distinct, "UPDATE slots SET pos = 2 WHERE id <= 2"),
           ErrorReport("constraint violation", "duplicate key: slots.pos=2"));

  // Two selected rows that change to the same value.
  CHECK_EQ(PlanSlotsUpdate(distinct, "UPDATE slots SET pos = 9 WHERE id < 3"),
           ErrorReport("constraint violation", "duplicate key: slots.pos=9"));

  // All rows change to distinct values.
  CHECK_EQ(
      PlanSlotsUpdate(distinct, "UPDATE slots SET pos = pos + 10"),
      (std::vector<std::string>{"affected 3", "slots ~ @1.0 1|1|x => 1|11|x",
                                "slots ~ @1.1 2|2|y => 2|12|y",
                                "slots ~ @1.2 3|3|z => 3|13|z"}));

  // The table already holds a duplicate (load does not check UNIQUE): an update
  // that leaves the column unchanged is not blocked by it.
  const Rows with_duplicate{Row{Int(1), Int(5), Text("x")},
                            Row{Int(2), Int(5), Text("y")},
                            Row{Int(3), Int(6), Text("z")}};
  CHECK_EQ(PlanSlotsUpdate(with_duplicate, "UPDATE slots SET label = 'w'"),
           (std::vector<std::string>{
               "affected 3", "slots ~ @1.0 1|5|x => 1|5|w",
               "slots ~ @1.1 2|5|y => 2|5|w", "slots ~ @1.2 3|6|z => 3|6|w"}));
  CHECK_EQ(
      PlanSlotsUpdate(with_duplicate, "UPDATE slots SET pos = 5 WHERE id = 1"),
      (std::vector<std::string>{"affected 1"}));
  // A changed value that joins the existing duplicate is one.
  CHECK_EQ(
      PlanSlotsUpdate(with_duplicate, "UPDATE slots SET pos = 5 WHERE id = 3"),
      ErrorReport("constraint violation", "duplicate key: slots.pos=5"));

  // Only the changed cells are checked: rows 1 and 2 keep their duplicate
  // value 5, and row 3 changes to a value nobody holds.
  const Rows keeps_duplicate{Row{Int(1), Int(5), Text("x")},
                             Row{Int(2), Int(5), Text("y")},
                             Row{Int(3), Int(-6), Text("z")}};
  CHECK_EQ(
      PlanSlotsUpdate(keeps_duplicate, "UPDATE slots SET pos = ABS(pos)"),
      (std::vector<std::string>{"affected 3", "slots ~ @1.2 3|-6|z => 3|6|z"}));

  // Two UNIQUE columns: the one with the lower position is reported first.
  TableSchema accounts = MakeSchema(
      "acct",
      {Column("id", DataType::kInt), Column("email", DataType::kText),
       Column("handle", DataType::kText)},
      {}, {1, 2});
  PlannerTables account_tables;
  account_tables.Add(accounts, Rows{Row{Int(1), Text("e1"), Text("h1")},
                                    Row{Int(2), Text("e2"), Text("h2")}});
  CHECK_EQ(
      Report(PlanSql("UPDATE acct SET handle = 'h2', email = 'e2' WHERE id "
                     "= 1",
                     account_tables)),
      ErrorReport("constraint violation", "duplicate key: acct.email=e2"));
  CHECK_EQ(
      Report(PlanSql("UPDATE acct SET handle = 'h2' WHERE id = 1",
                     account_tables)),
      ErrorReport("constraint violation", "duplicate key: acct.handle=h2"));
}

TEST(test_planner_update_foreign_key) {
  // stories holds one row (13) whose visitor does not exist: loaded data is not
  // checked against foreign keys.
  PlannerTables tables;
  AddShop(&tables);
  tables.reader.AddTable(
      StoriesSchema(),
      Rows{Row{Int(10), Text("hello world"), Text("alice"), Int(5)},
           Row{Int(11), Text("second story"), Text("alice"), Int(0)},
           Row{Int(13), Text("orphan"), Text("ghost"), Int(1)}});

  // A new value that exists in the parent passes.
  CHECK_EQ(
      Report(
          PlanSql("UPDATE stories SET username = 'bob' WHERE id = 10", tables)),
      (std::vector<std::string>{
          "affected 1",
          "stories ~ @1.0 10|hello world|alice|5 => 10|hello world|bob|5"}));
  CHECK_EQ(tables.ScansOf("visitors"), static_cast<std::size_t>(1));

  // A new value that does not exist is reported with both column names.
  CHECK_EQ(Report(PlanSql("UPDATE stories SET username = 'zed' WHERE id = 10",
                          tables)),
           ErrorReport("constraint violation",
                       "no matching row: stories.username=zed not found in "
                       "visitors.username"));

  // Assigning the stored value skips the check, even when the parent row is
  // missing; the parent is not scanned.
  PlannerTables skipped;
  AddShop(&skipped);
  skipped.reader.AddTable(StoriesSchema(), Rows{Row{Int(13), Text("orphan"),
                                                    Text("ghost"), Int(1)}});
  CHECK_EQ(Report(PlanSql("UPDATE stories SET username = 'ghost' WHERE id = 13",
                          skipped)),
           (std::vector<std::string>{"affected 1"}));
  CHECK_EQ(skipped.ScansOf("visitors"), static_cast<std::size_t>(0));

  // Updating another column of that row does not check the foreign key either.
  CHECK_EQ(
      Report(PlanSql("UPDATE stories SET views = 2 WHERE id = 13", skipped)),
      (std::vector<std::string>{
          "affected 1",
          "stories ~ @1.0 13|orphan|ghost|1 => 13|orphan|ghost|2"}));
  CHECK_EQ(skipped.ScansOf("visitors"), static_cast<std::size_t>(0));

  // Only the changed cells are checked: the statement repairs 'alicex' and
  // leaves the orphan 'ghost' as it is.
  PlannerTables repaired;
  AddShop(&repaired);
  repaired.reader.AddTable(
      StoriesSchema(), Rows{Row{Int(10), Text("p"), Text("alicex"), Int(0)},
                            Row{Int(13), Text("q"), Text("ghost"), Int(0)}});
  CHECK_EQ(
      Report(PlanSql("UPDATE stories SET username = SUBSTR(username, 1, 5)",
                     repaired)),
      (std::vector<std::string>{"affected 2",
                                "stories ~ @1.0 10|p|alicex|0 => "
                                "10|p|alice|0"}));

  // Many rows change the foreign key: the parent is scanned once.
  PlannerTables many;
  AddShop(&many);
  Planned all_to_bob = PlanSql("UPDATE stories SET username = 'bob'", many);
  CHECK_EQ(all_to_bob.error.has_value(), false);
  CHECK_EQ(Report(all_to_bob).size(), static_cast<std::size_t>(1 + 2));
  CHECK_EQ(many.ScansOf("visitors"), static_cast<std::size_t>(1));

  // A column that is both UNIQUE and a foreign key: uniqueness is checked
  // before the foreign key, so two rows that get a value that is a duplicate
  // and also missing from the parent report the duplicate.
  PlannerTables members;
  AddShop(&members);
  members.Add(MakeSchema("members",
                         {Column("id", DataType::kInt),
                          Column("username", DataType::kText)},
                         {domain::ForeignKey{1, "visitors", 1}}, {1}),
              Rows{Row{Int(1), Text("alice")}, Row{Int(2), Text("bob")}});
  CHECK_EQ(Report(PlanSql("UPDATE members SET username = 'zed'", members)),
           ErrorReport("constraint violation",
                       "duplicate key: members.username=zed"));
  CHECK_EQ(Report(PlanSql("UPDATE members SET username = 'zed' WHERE id = 1",
                          members)),
           ErrorReport("constraint violation",
                       "no matching row: members.username=zed not found in "
                       "visitors.username"));

  // Two foreign keys changed together: the first in declaration order is
  // reported.
  CHECK_EQ(Report(PlanSql("UPDATE reactions SET storyId = 999, username = "
                          "'zed' WHERE id = 100",
                          many)),
           ErrorReport("constraint violation",
                       "no matching row: reactions.storyId=999 not found in "
                       "stories.id"));
  CHECK_EQ(Report(PlanSql("UPDATE reactions SET username = 'zed' WHERE id = "
                          "100",
                          many)),
           ErrorReport("constraint violation",
                       "no matching row: reactions.username=zed not found in "
                       "visitors.username"));
}

TEST(test_planner_update_cascade) {
  // A to B and B to C in one statement: Parent.code + 1 renames 10 to 11 and 11
  // to 12. The rows that referenced 10 become 11 (not 12), the rows that
  // referenced 11 become 12, and the row that referenced 20 is not in the
  // change set. Mixed-case names: TableChange::table is lowercase.
  TableSchema parent = MakeSchema(
      "Parent", {Column("id", DataType::kInt), Column("code", DataType::kInt)},
      {}, {1});
  TableSchema child =
      MakeSchema("Child",
                 {Column("id", DataType::kInt), Column("pcode", DataType::kInt),
                  Column("note", DataType::kText)},
                 {domain::ForeignKey{1, "Parent", 1}}, {});
  PlannerTables chain;
  chain.Add(parent, Rows{Row{Int(1), Int(10)}, Row{Int(2), Int(11)},
                         Row{Int(3), Int(20)}});
  chain.Add(
      child,
      Rows{Row{Int(1), Int(10), Text("a")}, Row{Int(2), Int(11), Text("b")},
           Row{Int(3), Int(10), Text("c")}, Row{Int(4), Int(20), Text("d")}});
  Planned renamed =
      PlanSql("UPDATE Parent SET code = code + 1 WHERE id <= 2", chain);
  CHECK_EQ(
      Report(renamed),
      (std::vector<std::string>{
          "affected 2", "parent ~ @1.0 1|10 => 1|11",
          "parent ~ @1.1 2|11 => 2|12", "child ~ @1.0 1|10|a => 1|11|a",
          "child ~ @1.1 2|11|b => 2|12|b", "child ~ @1.2 3|10|c => 3|11|c"}));
  // The target comes first, then the child; each holds one kind of change.
  CHECK_EQ(ChangeCount(renamed), static_cast<std::size_t>(2));
  CHECK_EQ(chain.ScansOf("Child"), static_cast<std::size_t>(1));

  // A child table with two foreign keys that point at different columns of the
  // same parent: a row rewritten by both is one RowUpdate, a row rewritten by
  // one is another, and a row without a reference is absent.
  TableSchema pair_parent =
      MakeSchema("p2",
                 {Column("id", DataType::kInt), Column("a", DataType::kText),
                  Column("b", DataType::kText)},
                 {}, {1, 2});
  TableSchema pair_child = MakeSchema(
      "c2",
      {Column("id", DataType::kInt), Column("fa", DataType::kText),
       Column("fb", DataType::kText)},
      {domain::ForeignKey{1, "p2", 1}, domain::ForeignKey{2, "p2", 2}}, {});
  PlannerTables two_keys;
  two_keys.Add(pair_parent, Rows{Row{Int(1), Text("a1"), Text("b1")},
                                 Row{Int(2), Text("a2"), Text("b2")}});
  two_keys.Add(pair_child, Rows{Row{Int(1), Text("a1"), Text("b1")},
                                Row{Int(2), Text("a2"), Text("b1")},
                                Row{Int(3), Text("a2"), Text("b2")}});
  Planned both =
      PlanSql("UPDATE p2 SET a = 'a9', b = 'b9' WHERE id = 1", two_keys);
  CHECK_EQ(Report(both), (std::vector<std::string>{
                             "affected 1", "p2 ~ @1.0 1|a1|b1 => 1|a9|b9",
                             "c2 ~ @1.0 1|a1|b1 => 1|a9|b9",
                             "c2 ~ @1.1 2|a2|b1 => 2|a2|b9"}));
  // One scan of the child per foreign key that references an assigned column.
  CHECK_EQ(two_keys.ScansOf("c2"), static_cast<std::size_t>(2));
  CHECK_EQ(two_keys.ScansOf("p2"), static_cast<std::size_t>(1));

  // One old value that maps to two new values takes the first in scan order:
  // visitors 1 and 2 both hold 'dup' (UNIQUE is not checked on load) and are
  // renamed to n1 and n2.
  PlannerTables duplicates;
  AddShop(&duplicates, Rows{Row{Int(1), Text("dup"), Text("x")},
                            Row{Int(2), Text("dup"), Text("y")},
                            Row{Int(3), Text("solo"), Text("z")}});
  duplicates.reader.AddTable(
      StoriesSchema(), Rows{Row{Int(10), Text("p"), Text("dup"), Int(0)}});
  duplicates.reader.AddTable(ReactionsSchema(), Rows{});
  Planned first_wins = PlanSql(
      "UPDATE visitors SET username = 'n' || CAST(id AS TEXT) WHERE id <= 2",
      duplicates);
  CHECK_EQ(Report(first_wins),
           (std::vector<std::string>{
               "affected 2", "visitors ~ @1.0 1|dup|x => 1|n1|x",
               "visitors ~ @1.1 2|dup|y => 2|n2|y",
               "stories ~ @1.0 10|p|dup|0 => 10|p|n1|0"}));
  // reactions was scanned and has no referencing row, so it has no change.
  CHECK_EQ(duplicates.ScansOf("reactions"), static_cast<std::size_t>(1));
  CHECK_EQ(ChangeCount(first_wins), static_cast<std::size_t>(2));

  // The shop: renaming alice cascades into stories and reactions; affected
  // counts the selected visitor only.
  PlannerTables shop;
  AddShop(&shop);
  Planned alice =
      PlanSql("UPDATE visitors SET username = 'alicia' WHERE id = 1", shop);
  CHECK_EQ(
      Report(alice),
      (std::vector<std::string>{
          "affected 1", "visitors ~ @1.0 1|alice|Paris => 1|alicia|Paris",
          "stories ~ @1.0 10|hello world|alice|5 => 10|hello world|alicia|5",
          "stories ~ @1.1 11|second story|alice|0 => 11|second story|alicia|0",
          "reactions ~ @1.3 103|12|alice|like|None|1003 => "
          "103|12|alicia|like|None|1003"}));
  CHECK_EQ(ChangeCount(alice), static_cast<std::size_t>(3));

  // A column that nothing references, or an assignment that changes nothing,
  // reads no child table.
  PlannerTables unreferenced;
  AddShop(&unreferenced);
  Planned moved = PlanSql(
      "UPDATE visitors SET location = 'Berlin' WHERE id = 1", unreferenced);
  CHECK_EQ(ChangeCount(moved), static_cast<std::size_t>(1));
  Planned same_name =
      PlanSql("UPDATE visitors SET username = username", unreferenced);
  CHECK_EQ(ChangeCount(same_name), static_cast<std::size_t>(0));
  CHECK_EQ(unreferenced.ScansOf("stories"), static_cast<std::size_t>(0));
  CHECK_EQ(unreferenced.ScansOf("reactions"), static_cast<std::size_t>(0));
}

TEST(test_planner_update_error_precedence) {
  PlannerTables tables;
  tables.Add(
      MakeSchema("t",
                 {Column("id", DataType::kInt), Column("v", DataType::kInt)},
                 {}, {}),
      Rows{Row{Int(1), Int(1)}, Row{Int(2), Int(2)}, Row{Int(3), Int(3)},
           Row{Int(4), Int(4)}, Row{Int(5), Int(5)}});

  // The assignment of row 1 overflows and the WHERE of row 5 divides by zero:
  // WHERE is evaluated on every row first, so its error is the one reported.
  CHECK_EQ(Report(PlanSql("UPDATE t SET v = v + 9223372036854775807 WHERE 10 / "
                          "(id - 5) > -100",
                          tables)),
           ErrorReport("runtime", "division by zero"));

  // Without the WHERE error the assignment error is reported.
  CHECK_EQ(Report(PlanSql("UPDATE t SET v = v + 9223372036854775807 WHERE id < "
                          "5",
                          tables)),
           ErrorReport("runtime", "numeric overflow"));

  // The same holds for DELETE: a WHERE error is a kRuntime error.
  CHECK_EQ(Report(PlanSql("DELETE FROM t WHERE 10 / (id - 5) > -100", tables)),
           ErrorReport("runtime", "division by zero"));
}

TEST(test_planner_delete_restrict_and_order) {
  // visitors in the order dave, bob, alice, carol: scan order is not primary
  // key order. dave is referenced by nobody, bob by stories and reactions,
  // alice by stories and reactions, carol by reactions only.
  const Rows scrambled{Row{Int(4), Text("dave"), Text("Oslo")},
                       Row{Int(3), Text("bob"), Text("Rome")},
                       Row{Int(1), Text("alice"), Text("Paris")},
                       Row{Int(2), Text("carol"), Text("Paris")}};
  PlannerTables tables;
  AddShop(&tables, scrambled);

  // No WHERE and nothing references the table: every row is deleted, with its
  // Rid and before-image.
  CHECK_EQ(Report(PlanSql("DELETE FROM reactions", tables)),
           (std::vector<std::string>{
               "affected 5", "reactions - @1.0 100|10|bob|like|None|1000",
               "reactions - @1.1 101|10|carol|comment|nice|1001",
               "reactions - @1.2 102|11|bob|comment|ok|1002",
               "reactions - @1.3 103|12|alice|like|None|1003",
               "reactions - @1.4 104|12|carol|like|None|1004"}));

  // Nothing is selected: no change, affected 0.
  Planned none = PlanSql("DELETE FROM visitors WHERE id = 99", tables);
  CHECK_EQ(Report(none), (std::vector<std::string>{"affected 0"}));
  CHECK_EQ(ChangeCount(none), static_cast<std::size_t>(0));

  // A row that nothing references is deleted although the table is referenced.
  CHECK_EQ(
      Report(PlanSql("DELETE FROM visitors WHERE id = 4", tables)),
      (std::vector<std::string>{"affected 1", "visitors - @1.0 4|dave|Oslo"}));

  // A referenced row: the message names the child column.
  CHECK_EQ(Report(PlanSql("DELETE FROM visitors WHERE id = 2", tables)),
           ErrorReport("constraint violation",
                       "visitors.id=2 is referenced by reactions.username"));

  // Several violating rows: the first in scan order is reported (bob, which is
  // before alice), and of the two child tables that reference it, the first in
  // catalog order (stories, which is not first alphabetically).
  CHECK_EQ(Report(PlanSql("DELETE FROM visitors", tables)),
           ErrorReport("constraint violation",
                       "visitors.id=3 is referenced by stories.username"));
  CHECK_EQ(
      Report(PlanSql("DELETE FROM visitors WHERE id = 1 OR id = 3", tables)),
      ErrorReport("constraint violation",
                  "visitors.id=3 is referenced by stories.username"));

  // A referenced row is reported even though other selected rows are free.
  CHECK_EQ(
      Report(PlanSql("DELETE FROM visitors WHERE id = 4 OR id = 2", tables)),
      ErrorReport("constraint violation",
                  "visitors.id=2 is referenced by reactions.username"));

  // Two foreign keys of one child that reference the same parent: the first in
  // declaration order is reported. The foreign key names its table in another
  // spelling.
  PlannerTables messages;
  AddShop(&messages, scrambled);
  messages.Add(MakeSchema("msgs",
                          {Column("id", DataType::kInt),
                           Column("sender", DataType::kText),
                           Column("receiver", DataType::kText)},
                          {domain::ForeignKey{1, "VISITORS", 1},
                           domain::ForeignKey{2, "VISITORS", 1}},
                          {}),
               Rows{Row{Int(1), Text("dave"), Text("dave")},
                    Row{Int(2), Text("bob"), Text("dave")}});
  CHECK_EQ(Report(PlanSql("DELETE FROM visitors WHERE id = 4", messages)),
           ErrorReport("constraint violation",
                       "visitors.id=4 is referenced by msgs.sender"));

  // Each referencing child table is scanned once, however many rows are
  // selected.
  CHECK_EQ(messages.ScansOf("stories"), static_cast<std::size_t>(1));
  CHECK_EQ(messages.ScansOf("reactions"), static_cast<std::size_t>(1));
  CHECK_EQ(messages.ScansOf("msgs"), static_cast<std::size_t>(2));
  CHECK_EQ(messages.ScansOf("visitors"), static_cast<std::size_t>(1));
}

TEST(test_planner_rejects_non_write_node_and_unknown_table) {
  PlannerTables tables;
  AddShop(&tables);

  // A query node and a DDL node are not write nodes.
  PlanPtr scan = MakePlan(PlanNode{domain::ScanNode{"visitors"}});
  CHECK_EQ(Report(PlanOf(*scan, tables)),
           ErrorReport("unsupported", "not a write plan node"));
  PlanPtr select = MustBind("SELECT id FROM visitors", tables.catalog);
  if (select == nullptr) {
    return;
  }
  CHECK_EQ(Report(PlanOf(*select, tables)),
           ErrorReport("unsupported", "not a write plan node"));
  PlanPtr create =
      MakePlan(PlanNode{domain::CreateTableNode{MeasuresSchema()}});
  CHECK_EQ(Report(PlanOf(*create, tables)),
           ErrorReport("unsupported", "not a write plan node"));

  // A table the catalog does not have, for each write node, with the name as
  // written.
  PlanPtr delete_plan =
      MakePlan(PlanNode{domain::DeleteNode{"Ghost", nullptr}});
  CHECK_EQ(Report(PlanOf(*delete_plan, tables)),
           ErrorReport("bind", "unknown table: Ghost"));
  PlanPtr insert_plan = MakePlan(
      PlanNode{domain::InsertNode{"ghost", Rows{Row{Int(1)}}, std::nullopt}});
  CHECK_EQ(Report(PlanOf(*insert_plan, tables)),
           ErrorReport("bind", "unknown table: ghost"));
  PlanPtr update_plan = UpdateToLiteral("ghost", 0, Int(1));
  CHECK_EQ(Report(PlanOf(*update_plan, tables)),
           ErrorReport("bind", "unknown table: ghost"));

  // A table the catalog has and the reader cannot scan: the error of the
  // reader.
  PlannerTables unreadable;
  unreadable.catalog.Add(MeasuresSchema());
  PlanPtr delete_measures =
      MakePlan(PlanNode{domain::DeleteNode{"measures", nullptr}});
  CHECK_EQ(Report(PlanOf(*delete_measures, unreadable)),
           ErrorReport("bind", "unknown table: measures"));

  // Table names are case-insensitive, and TableChange::table is lowercase.
  PlanPtr upper = MakePlan(PlanNode{domain::DeleteNode{"REACTIONS", nullptr}});
  Planned deleted = PlanOf(*upper, tables);
  CHECK_EQ(deleted.error.has_value(), false);
  CHECK_EQ(ChangeCount(deleted), static_cast<std::size_t>(1));
  if (deleted.plan.has_value() && !deleted.plan->changes.empty()) {
    CHECK_EQ(deleted.plan->changes[0].table, "reactions");
    CHECK_EQ(deleted.plan->affected, static_cast<std::size_t>(5));
  }
}

namespace {

// An IIndexReader over key sets that counts the Contains and ScanRange calls of
// each table and can make every call of either kind fail. A table given its
// rows with SetRows answers ScanRange with the rows of the range in ascending
// key order and their Rids FakeRid(position in the vector given), as a real
// index does; a table without rows answers with an empty stream.
class FakeKeyIndex : public paged::IIndexReader {
 public:
  void AddIndexedTable(const std::string& name, std::size_t column,
                       std::set<int64_t> keys) {
    tables_[domain::ToLower(name)] =
        Table{column, std::move(keys), 0, 0, {}, {}};
  }
  // Gives an indexed table its columns and rows, which ScanRange returns.
  void SetRows(const std::string& name, const std::vector<ColumnDef>& columns,
               const Rows& rows) {
    Table& table = tables_.at(domain::ToLower(name));
    table.columns = columns;
    for (std::size_t position = 0; position < rows.size(); ++position) {
      const int64_t key = std::get<int64_t>(rows[position][table.column]);
      table.rows_by_key[key] =
          paged::StoredRow{FakeRid(position), rows[position]};
    }
  }
  void FailEveryContains(const std::string& message) { failure_ = message; }
  void FailEveryScanRange(const std::string& message) {
    range_failure_ = message;
  }

  std::optional<std::size_t> IndexedColumn(
      const std::string& table) const override {
    const auto found = tables_.find(domain::ToLower(table));
    if (found == tables_.end()) {
      return std::nullopt;
    }
    return found->second.column;
  }

  paged::ScanResult ScanRange(const std::string& table,
                              const paged::KeyRange& range) const override {
    Table& found = tables_.at(domain::ToLower(table));
    ++found.range_calls;
    if (range_failure_.has_value()) {
      return ExecutionError{domain::ErrorKind::kIo, *range_failure_};
    }
    std::vector<paged::StoredRow> in_range;
    if (!range.IsEmpty()) {
      const std::optional<int64_t> first = range.FirstKey();
      const std::optional<int64_t> last = range.LastKey();
      for (const auto& [key, stored] : found.rows_by_key) {
        if ((!first.has_value() || key >= *first) &&
            (!last.has_value() || key <= *last)) {
          in_range.push_back(stored);
        }
      }
    }
    return std::unique_ptr<paged::IRowStream>(
        std::make_unique<strata::testing::VectorRowStream>(
            found.columns, std::move(in_range)));
  }

  paged::ContainsResult Contains(const std::string& table,
                                 int64_t key) const override {
    Table& found = tables_.at(domain::ToLower(table));
    ++found.contains_calls;
    if (failure_.has_value()) {
      return ExecutionError{domain::ErrorKind::kIo, *failure_};
    }
    return found.keys.contains(key);
  }

  std::size_t ContainsCallsOf(const std::string& table) const {
    return tables_.at(domain::ToLower(table)).contains_calls;
  }
  std::size_t RangeCallsOf(const std::string& table) const {
    return tables_.at(domain::ToLower(table)).range_calls;
  }

 private:
  struct Table {
    std::size_t column = 0;
    std::set<int64_t> keys;
    std::size_t contains_calls = 0;
    std::size_t range_calls = 0;
    std::vector<ColumnDef> columns;
    std::map<int64_t, paged::StoredRow> rows_by_key;
  };

  mutable std::map<std::string, Table> tables_;
  std::optional<std::string> failure_;
  std::optional<std::string> range_failure_;
};

Planned PlanSqlWithIndex(const std::string& sql, const PlannerTables& tables,
                         const paged::IIndexReader* index_reader) {
  PlanPtr node = MustBind(sql, tables.catalog);
  if (node == nullptr) {
    return Planned{};
  }
  paged::PlanMutationResult result =
      paged::PlanMutation(*node, tables.catalog, tables.reader, index_reader);
  Planned planned;
  if (auto* error = std::get_if<ExecutionError>(&result)) {
    planned.error = std::move(*error);
  } else {
    planned.plan = std::move(std::get<paged::MutationPlan>(result));
  }
  return planned;
}

}  // namespace

namespace {

// items: an INT key, indexed. users: a UNIQUE column, so the other rows' values
// are needed. counters: an auto-increment key. kids: a foreign key to items.id
// (an indexed primary key) and one to users.email (UNIQUE, not the key).
std::vector<ColumnDef> LabelColumns() {
  return {Column("id", DataType::kInt), Column("label", DataType::kText)};
}

Rows ItemRows() {
  return Rows{Row{Int(1), Text("a")}, Row{Int(2), Text("b")},
              Row{Int(3), Text("c")}};
}

// The rows of `shuffled` are stored out of key order, so an index range read
// and a table scan return them in different orders.
Rows ShuffledRows() {
  return Rows{Row{Int(3), Text("c")}, Row{Int(1), Text("a")},
              Row{Int(2), Text("b")}};
}

PlannerTables IndexedTestTables() {
  PlannerTables tables;
  tables.Add(MakeSchema("items", LabelColumns(), {}, {}), ItemRows());
  tables.Add(MakeSchema("shuffled", LabelColumns(), {}, {}), ShuffledRows());
  tables.Add(MakeSchema("users",
                        {Column("id", DataType::kInt),
                         Column("email", DataType::kText)},
                        {}, {1}),
             Rows{Row{Int(1), Text("a@x")}});
  TableSchema counters = MakeSchema(
      "counters",
      {Column("id", DataType::kInt), Column("label", DataType::kText)}, {}, {});
  counters.auto_increment_index = 0;
  tables.Add(counters, Rows{Row{Int(1), Text("a")}});
  tables.Add(MakeSchema("kids",
                        {Column("id", DataType::kInt),
                         Column("item_id", DataType::kInt),
                         Column("email", DataType::kText)},
                        {domain::ForeignKey{1, "items", 0},
                         domain::ForeignKey{2, "users", 1}},
                        {}),
             Rows{});
  return tables;
}

FakeKeyIndex IndexedTestIndex() {
  FakeKeyIndex index;
  index.AddIndexedTable("items", 0, {1, 2, 3});
  index.AddIndexedTable("shuffled", 0, {1, 2, 3});
  index.AddIndexedTable("users", 0, {1});
  index.AddIndexedTable("counters", 0, {1});
  index.AddIndexedTable("kids", 0, {});
  index.SetRows("items", LabelColumns(), ItemRows());
  index.SetRows("shuffled", LabelColumns(), ShuffledRows());
  return index;
}

// How often a table was scanned and asked with Contains, in one planning.
struct TableCounts {
  const char* table;
  std::size_t scans;
  std::size_t contains;
};

// How often a table was scanned and read through ScanRange, in one planning.
struct RangeCounts {
  const char* table;
  std::size_t scans;
  std::size_t ranges;
};

}  // namespace

TEST(
    test_paged_planner_uses_contains_and_ranges_when_an_index_reader_is_given) {
  struct Case {
    const char* sql;
    std::vector<TableCounts> counts;  // the tables not listed have 0 and 0
  };
  const Case cases[] = {
      // No UNIQUE column, no generated value: no scan, one Contains per row.
      {"INSERT INTO items VALUES (4, 'x'), (5, 'y')", {{"items", 0, 2}}},
      // A duplicate of an existing key, and of an earlier row of the statement:
      // the second is found in the key sets, without a Contains call.
      {"INSERT INTO items VALUES (2, 'x')", {{"items", 0, 1}}},
      {"INSERT INTO items VALUES (7, 'x'), (7, 'y')", {{"items", 0, 1}}},
      // A UNIQUE column and a generated value still scan the target.
      {"INSERT INTO users VALUES (2, 'b@x')", {{"users", 1, 0}}},
      {"INSERT INTO counters (label) VALUES ('x')", {{"counters", 1, 0}}},
      // A foreign key to an indexed primary key is checked by Contains, and its
      // parent is not scanned; a foreign key to a UNIQUE column scans its
      // parent. kids has no UNIQUE column, so its own key is looked up too.
      {"INSERT INTO kids VALUES (5, 2, 'a@x')",
       {{"kids", 0, 1}, {"items", 0, 1}, {"users", 1, 0}}},
      // A missing parent row ends the checks before the second foreign key.
      {"INSERT INTO kids VALUES (5, 9, 'a@x')",
       {{"kids", 0, 1}, {"items", 0, 1}}}};
  for (const Case& test_case : cases) {
    // The run without an index reader is the reference.
    const PlannerTables reference_tables = IndexedTestTables();
    const Planned expected =
        PlanSqlWithIndex(test_case.sql, reference_tables, nullptr);
    const PlannerTables tables = IndexedTestTables();
    const FakeKeyIndex index = IndexedTestIndex();
    const Planned actual = PlanSqlWithIndex(test_case.sql, tables, &index);
    CHECK(Report(actual) == Report(expected));
    for (const char* name : {"items", "users", "counters", "kids"}) {
      std::size_t expected_scans = 0;
      std::size_t expected_contains = 0;
      for (const TableCounts& counts : test_case.counts) {
        if (std::string(counts.table) == name) {
          expected_scans = counts.scans;
          expected_contains = counts.contains;
        }
      }
      // An INSERT never reads a range.
      if (tables.ScansOf(name) != expected_scans ||
          index.ContainsCallsOf(name) != expected_contains ||
          index.RangeCallsOf(name) != 0) {
        // Shows which statement and table differ.
        CHECK_EQ(std::string(test_case.sql) + " " + name + " scans " +
                     std::to_string(tables.ScansOf(name)) + " contains " +
                     std::to_string(index.ContainsCallsOf(name)) + " ranges " +
                     std::to_string(index.RangeCallsOf(name)),
                 std::string(test_case.sql) + " " + name + " scans " +
                     std::to_string(expected_scans) + " contains " +
                     std::to_string(expected_contains) + " ranges 0");
      }
    }
  }

  // A Contains that fails makes PlanMutation return its error.
  const PlannerTables tables = IndexedTestTables();
  FakeKeyIndex failing = IndexedTestIndex();
  failing.FailEveryContains("injected contains failure");
  const Planned failed =
      PlanSqlWithIndex("INSERT INTO items VALUES (4, 'x')", tables, &failing);
  CHECK(Report(failed) == ErrorReport("io", "injected contains failure"));
}

// UPDATE and DELETE read the rows of a WHERE that has a key range with
// ScanRange, and every other WHERE with Scan.
TEST(test_paged_planner_reads_where_through_the_index_for_update_and_delete) {
  struct RangeCase {
    const char* sql;
    std::vector<RangeCounts> counts;  // the tables not listed have 0 and 0
  };
  const RangeCase range_cases[] = {
      {"UPDATE items SET label = 'z' WHERE id = 2", {{"items", 0, 1}}},
      {"UPDATE items SET label = 'z' WHERE id >= 2 AND label <> 'c'",
       {{"items", 0, 1}}},
      {"UPDATE items SET label = 'z' WHERE label = 'b'", {{"items", 1, 0}}},
      {"UPDATE items SET label = 'z'", {{"items", 1, 0}}},
      // An assigned UNIQUE column needs the rows that the WHERE does not
      // select, so the target is scanned; kids is scanned for the cascade.
      {"UPDATE users SET email = 'b@x' WHERE id = 1",
       {{"users", 1, 0}, {"kids", 1, 0}}},
      // A DELETE also scans each table that references the target.
      {"DELETE FROM items WHERE id = 2", {{"items", 0, 1}, {"kids", 1, 0}}},
      {"DELETE FROM items WHERE id = 1 OR id = 3",
       {{"items", 1, 0}, {"kids", 1, 0}}},
      {"DELETE FROM items", {{"items", 1, 0}, {"kids", 1, 0}}},
      // An empty range still reads ScanRange, selects nothing and scans no
      // child table.
      {"DELETE FROM items WHERE id = 3 AND id = 4", {{"items", 0, 1}}}};
  for (const RangeCase& test_case : range_cases) {
    const PlannerTables reference_tables = IndexedTestTables();
    const Planned expected =
        PlanSqlWithIndex(test_case.sql, reference_tables, nullptr);
    const PlannerTables tables = IndexedTestTables();
    const FakeKeyIndex index = IndexedTestIndex();
    const Planned actual = PlanSqlWithIndex(test_case.sql, tables, &index);
    CHECK(Report(actual) == Report(expected));
    for (const char* name : {"items", "users", "kids"}) {
      std::size_t expected_scans = 0;
      std::size_t expected_ranges = 0;
      for (const RangeCounts& counts : test_case.counts) {
        if (std::string(counts.table) == name) {
          expected_scans = counts.scans;
          expected_ranges = counts.ranges;
        }
      }
      if (tables.ScansOf(name) != expected_scans ||
          index.RangeCallsOf(name) != expected_ranges ||
          index.ContainsCallsOf(name) != 0) {
        CHECK_EQ(std::string(test_case.sql) + " " + name + " scans " +
                     std::to_string(tables.ScansOf(name)) + " ranges " +
                     std::to_string(index.RangeCallsOf(name)) + " contains " +
                     std::to_string(index.ContainsCallsOf(name)),
                 std::string(test_case.sql) + " " + name + " scans " +
                     std::to_string(expected_scans) + " ranges " +
                     std::to_string(expected_ranges) + " contains 0");
      }
    }
  }

  // The rows of a range come in key order with their own Rids, where a scan
  // returns them in storage order.
  const PlannerTables storage_tables = IndexedTestTables();
  const Planned in_storage_order = PlanSqlWithIndex(
      "DELETE FROM shuffled WHERE id >= 1", storage_tables, nullptr);
  CHECK(
      Report(in_storage_order) ==
      (std::vector<std::string>{"affected 3", "shuffled - @1.0 3|c",
                                "shuffled - @1.1 1|a", "shuffled - @1.2 2|b"}));
  const PlannerTables key_tables = IndexedTestTables();
  const FakeKeyIndex key_index = IndexedTestIndex();
  const Planned in_key_order = PlanSqlWithIndex(
      "DELETE FROM shuffled WHERE id >= 1", key_tables, &key_index);
  CHECK(
      Report(in_key_order) ==
      (std::vector<std::string>{"affected 3", "shuffled - @1.1 1|a",
                                "shuffled - @1.2 2|b", "shuffled - @1.0 3|c"}));
  const PlannerTables update_tables = IndexedTestTables();
  const FakeKeyIndex update_index = IndexedTestIndex();
  const Planned updated_in_key_order =
      PlanSqlWithIndex("UPDATE shuffled SET label = 'z' WHERE id >= 2",
                       update_tables, &update_index);
  CHECK(Report(updated_in_key_order) ==
        (std::vector<std::string>{"affected 2", "shuffled ~ @1.2 2|b => 2|z",
                                  "shuffled ~ @1.0 3|c => 3|z"}));

  // A ScanRange that fails makes PlanMutation return its error, for both.
  FakeKeyIndex failing_range = IndexedTestIndex();
  failing_range.FailEveryScanRange("injected range failure");
  const PlannerTables range_tables = IndexedTestTables();
  CHECK(Report(PlanSqlWithIndex("DELETE FROM items WHERE id = 2", range_tables,
                                &failing_range)) ==
        ErrorReport("io", "injected range failure"));
  CHECK(Report(PlanSqlWithIndex("UPDATE items SET label = 'z' WHERE id = 2",
                                range_tables, &failing_range)) ==
        ErrorReport("io", "injected range failure"));
}
