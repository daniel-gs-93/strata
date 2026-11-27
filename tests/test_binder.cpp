// Binder tests: name resolution, type checks and plan shapes of
// docs/DesignDocument.md section 2.4.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/domain/binder.h"
#include "test_harness.h"
#include "test_support.h"

namespace strata::testing {

namespace {

constexpr std::size_t kNoSortKey = std::numeric_limits<std::size_t>::max();

// Binds sql and reports a failure at file:line, naming sql, unless it fails
// with the given kind and a message containing fragment. The macros below
// pass the location of the calling test.
void ExpectBindFailure(const char* file, int line, const std::string& sql,
                       domain::ErrorKind kind, const std::string& fragment) {
  FakeCatalog catalog;
  domain::BindResult result = domain::Bind(MustParse(sql), catalog);
  const auto* error = std::get_if<domain::ExecutionError>(&result);
  if (error == nullptr) {
    ReportFailure(file, line, "expected an error binding [" + sql + "]");
    return;
  }
  if (error->kind != kind) {
    ReportFailure(file, line,
                  "wrong error kind binding [" + sql + "]: " + error->message);
  }
  if (error->message.find(fragment) == std::string::npos) {
    ReportFailure(
        file, line,
        "message of [" + sql + "] lacks [" + fragment + "]: " + error->message);
  }
}

#define EXPECT_BIND_FAILURE(sql, kind, fragment) \
  ExpectBindFailure(__FILE__, __LINE__, (sql), (kind), (fragment))

#define EXPECT_BIND_ERROR(sql) \
  EXPECT_BIND_FAILURE((sql), domain::ErrorKind::kBind, "")

// Binds sql against the fake catalog; null (and a reported failure) on error.
domain::PlanPtr BindOrFail(const std::string& sql) {
  FakeCatalog catalog;
  return MustBind(sql, catalog);
}

// A catalog of one table, wide, whose INT columns are c0, c1, and so on.
class WideCatalog : public domain::ICatalog {
 public:
  explicit WideCatalog(std::size_t column_count) {
    schema_.name = "wide";
    for (std::size_t column = 0; column < column_count; ++column) {
      schema_.columns.push_back(domain::ColumnDef{"c" + std::to_string(column),
                                                  domain::DataType::kInt});
    }
    schema_.primary_key_index = 0;
  }

  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override {
    if (domain::ToLower(name) == schema_.name) {
      return schema_;
    }
    return std::nullopt;
  }

  std::vector<std::string> TableNames() const override { return {"wide"}; }

 private:
  domain::TableSchema schema_;
};

// The message of a statement that must fail to bind.
std::string BindMessage(const std::string& sql,
                        const domain::ICatalog& catalog) {
  domain::BindResult result = domain::Bind(MustParse(sql), catalog);
  const auto* error = std::get_if<domain::ExecutionError>(&result);
  CHECK(error != nullptr);
  return error == nullptr ? std::string() : error->message;
}

template <typename Node>
const Node* NodeAs(const domain::PlanPtr& plan) {
  return plan ? std::get_if<Node>(&plan->node) : nullptr;
}

// The child of a unary node, as Node; null when the parent is null or the
// child has another shape.
template <typename Node, typename Parent>
const Node* ChildAs(const Parent* parent) {
  return parent == nullptr ? nullptr : NodeAs<Node>(parent->child);
}

template <typename Node>
const Node* LeftAs(const domain::JoinNode* join) {
  return join == nullptr ? nullptr : NodeAs<Node>(join->left);
}

template <typename Node>
const Node* RightAs(const domain::JoinNode* join) {
  return join == nullptr ? nullptr : NodeAs<Node>(join->right);
}

std::string JoinNames(const domain::ProjectNode& project) {
  std::string joined;
  for (std::size_t i = 0; i < project.output_columns.size(); ++i) {
    if (i > 0) {
      joined += ",";
    }
    joined += project.output_columns[i].name;
  }
  return joined;
}

const domain::BoundBinary* AsBinary(const domain::BoundExprPtr& expr,
                                    domain::BinaryOp op) {
  if (!expr) {
    return nullptr;
  }
  const auto* binary = std::get_if<domain::BoundBinary>(&expr->node);
  if (binary == nullptr || binary->op != op) {
    return nullptr;
  }
  return binary;
}

bool IsColumn(const domain::BoundExprPtr& expr, std::size_t index) {
  if (!expr) {
    return false;
  }
  const auto* column = std::get_if<domain::BoundColumn>(&expr->node);
  return column != nullptr && column->index == index;
}

bool IsIntLiteral(const domain::BoundExprPtr& expr, int64_t value) {
  if (!expr) {
    return false;
  }
  const auto* literal = std::get_if<domain::BoundLiteral>(&expr->node);
  if (literal == nullptr) {
    return false;
  }
  const auto* integer = std::get_if<int64_t>(&literal->value);
  return integer != nullptr && *integer == value;
}

bool IsColumnVsColumn(const domain::BoundExprPtr& expr, domain::BinaryOp op,
                      std::size_t lhs, std::size_t rhs) {
  const domain::BoundBinary* binary = AsBinary(expr, op);
  return binary != nullptr && IsColumn(binary->lhs, lhs) &&
         IsColumn(binary->rhs, rhs);
}

bool IsColumnVsInt(const domain::BoundExprPtr& expr, domain::BinaryOp op,
                   std::size_t column, int64_t literal) {
  const domain::BoundBinary* binary = AsBinary(expr, op);
  return binary != nullptr && IsColumn(binary->lhs, column) &&
         IsIntLiteral(binary->rhs, literal);
}

bool IsIntVsInt(const domain::BoundExprPtr& expr, domain::BinaryOp op,
                int64_t lhs, int64_t rhs) {
  const domain::BoundBinary* binary = AsBinary(expr, op);
  return binary != nullptr && IsIntLiteral(binary->lhs, lhs) &&
         IsIntLiteral(binary->rhs, rhs);
}

// The ProjectNode column index of the single ORDER BY key of sql; failures are
// reported at file:line.
std::size_t FirstSortColumn(const char* file, int line,
                            const std::string& sql) {
  domain::PlanPtr plan = BindOrFail(sql);
  const auto* sort = NodeAs<domain::SortNode>(plan);
  if (sort == nullptr || sort->keys.size() != 1) {
    ReportFailure(file, line, "expected one sort key binding [" + sql + "]");
    return kNoSortKey;
  }
  return sort->keys[0].column_index;
}

#define FIRST_SORT_COLUMN(sql) FirstSortColumn(__FILE__, __LINE__, (sql))

bool ExprHasAggregate(const domain::BoundExprPtr& expr) {
  if (!expr) {
    return false;
  }
  if (std::holds_alternative<domain::BoundAggregate>(expr->node)) {
    return true;
  }
  if (const auto* function = std::get_if<domain::BoundFunction>(&expr->node)) {
    for (const auto& arg : function->args) {
      if (ExprHasAggregate(arg)) {
        return true;
      }
    }
    return false;
  }
  const auto* binary = std::get_if<domain::BoundBinary>(&expr->node);
  return binary != nullptr &&
         (ExprHasAggregate(binary->lhs) || ExprHasAggregate(binary->rhs));
}

// True when any expression of the plan uses the internal BoundAggregate form.
bool PlanHasAggregateForm(const domain::PlanPtr& plan) {
  if (!plan) {
    return false;
  }
  if (const auto* filter = std::get_if<domain::FilterNode>(&plan->node)) {
    return ExprHasAggregate(filter->predicate) ||
           PlanHasAggregateForm(filter->child);
  }
  if (const auto* join = std::get_if<domain::JoinNode>(&plan->node)) {
    return ExprHasAggregate(join->condition) ||
           PlanHasAggregateForm(join->left) ||
           PlanHasAggregateForm(join->right);
  }
  if (const auto* aggregate = std::get_if<domain::AggregateNode>(&plan->node)) {
    for (const auto& key : aggregate->group_keys) {
      if (ExprHasAggregate(key)) {
        return true;
      }
    }
    for (const auto& spec : aggregate->aggregates) {
      if (ExprHasAggregate(spec.arg)) {
        return true;
      }
    }
    return PlanHasAggregateForm(aggregate->child);
  }
  if (const auto* project = std::get_if<domain::ProjectNode>(&plan->node)) {
    for (const auto& expr : project->exprs) {
      if (ExprHasAggregate(expr)) {
        return true;
      }
    }
    return PlanHasAggregateForm(project->child);
  }
  if (const auto* sort = std::get_if<domain::SortNode>(&plan->node)) {
    return PlanHasAggregateForm(sort->child);
  }
  if (const auto* limit = std::get_if<domain::LimitNode>(&plan->node)) {
    return PlanHasAggregateForm(limit->child);
  }
  return false;
}

// The data types of the ProjectNode output of a SELECT, for example
// "INT,DOUBLE,TEXT"; empty, with a reported failure, when sql does not bind to
// a plan whose root (below any LimitNode or SortNode) is a ProjectNode.
std::string OutputTypes(const std::string& sql) {
  domain::PlanPtr plan = BindOrFail(sql);
  const domain::PlanNode* node = plan.get();
  while (node != nullptr) {
    if (const auto* limit = std::get_if<domain::LimitNode>(&node->node)) {
      node = limit->child.get();
    } else if (const auto* sort = std::get_if<domain::SortNode>(&node->node)) {
      node = sort->child.get();
    } else {
      break;
    }
  }
  const auto* project =
      node == nullptr ? nullptr : std::get_if<domain::ProjectNode>(&node->node);
  if (project == nullptr) {
    ReportFailure(__FILE__, __LINE__, "no ProjectNode binding [" + sql + "]");
    return std::string();
  }
  std::string types;
  for (std::size_t i = 0; i < project->output_columns.size(); ++i) {
    if (i > 0) {
      types += ",";
    }
    switch (project->output_columns[i].type) {
      case domain::DataType::kInt:
        types += "INT";
        break;
      case domain::DataType::kDouble:
        types += "DOUBLE";
        break;
      case domain::DataType::kText:
        types += "TEXT";
        break;
    }
  }
  return types;
}

}  // namespace

TEST(test_binder_unknown_table_returns_bind_error) {
  EXPECT_BIND_FAILURE("SELECT id FROM nosuch", domain::ErrorKind::kBind,
                      "nosuch");
}

TEST(test_binder_unknown_column_returns_bind_error) {
  EXPECT_BIND_FAILURE("SELECT nosuch FROM visitors", domain::ErrorKind::kBind,
                      "nosuch");
}

TEST(test_binder_ambiguous_column_returns_bind_error) {
  EXPECT_BIND_FAILURE(
      "SELECT username FROM visitors v JOIN stories s ON v.username = "
      "s.username",
      domain::ErrorKind::kBind, "ambiguous");
}

TEST(test_binder_table_name_rejected_when_alias_declared) {
  EXPECT_BIND_FAILURE("SELECT visitors.id FROM visitors v",
                      domain::ErrorKind::kBind, "visitors");

  domain::PlanPtr plan = BindOrFail("SELECT v.id FROM visitors v");

  CHECK(plan != nullptr);
}

TEST(test_binder_type_mismatch_returns_bind_error) {
  const std::vector<std::string> statements = {
      "SELECT id FROM visitors WHERE username = 1",
      "SELECT username + 1 FROM visitors",
      "SELECT SUM(username) FROM visitors",
  };

  for (const std::string& sql : statements) {
    EXPECT_BIND_ERROR(sql);
  }
}

TEST(test_binder_non_boolean_where_returns_bind_error) {
  EXPECT_BIND_ERROR("SELECT id FROM visitors WHERE id + 1");
  EXPECT_BIND_ERROR("SELECT id FROM visitors WHERE id");
}

TEST(test_binder_boolean_value_outside_condition_returns_bind_error) {
  const std::vector<std::string> statements = {
      "SELECT id = 1 FROM visitors",
      "UPDATE visitors SET location = id < 2",
      "INSERT INTO visitors VALUES (9, 'zed', 1 < 2)",
      "SELECT id FROM visitors GROUP BY id = 1",
      "SELECT id FROM visitors ORDER BY id = 1",
  };

  // Each of these fails for other reasons too (an unmatched ORDER BY item, a
  // column that is not a group key, a type that cannot be assigned), so the
  // message names the rule under test.
  for (const std::string& sql : statements) {
    EXPECT_BIND_FAILURE(sql, domain::ErrorKind::kBind,
                        "a boolean expression is not allowed in");
  }
  EXPECT_BIND_FAILURE("SELECT v.id FROM visitors v JOIN stories s ON v.id + 1",
                      domain::ErrorKind::kBind, "must be a boolean expression");
}

namespace {

// `id = first AND ... AND id = last - 1` as a balanced, parenthesized tree,
// which the parser accepts at any size: its depth is about log2(last - first).
std::string BalancedConjunction(int first, int last) {
  if (last - first == 1) {
    return "id = " + std::to_string(first);
  }
  const int middle = first + (last - first) / 2;
  return "(" + BalancedConjunction(first, middle) + " AND " +
         BalancedConjunction(middle, last) + ")";
}

// The depth of an expression and its INT literals from left to right, found
// with an explicit stack so that a deep tree cannot overflow this test.
struct TreeShape {
  std::size_t depth = 0;
  std::vector<std::int64_t> literals;
};

TreeShape MeasureTree(const domain::BoundExpr& root) {
  TreeShape shape;
  std::vector<std::pair<const domain::BoundExpr*, std::size_t>> pending;
  pending.emplace_back(&root, 1);
  while (!pending.empty()) {
    const auto [expr, depth] = pending.back();
    pending.pop_back();
    shape.depth = std::max(shape.depth, depth);
    if (const auto* literal = std::get_if<domain::BoundLiteral>(&expr->node)) {
      shape.literals.push_back(std::get<std::int64_t>(literal->value));
    }
    if (const auto* binary = std::get_if<domain::BoundBinary>(&expr->node)) {
      pending.emplace_back(binary->rhs.get(), depth + 1);
      pending.emplace_back(binary->lhs.get(), depth + 1);
    }
  }
  return shape;
}

}  // namespace

TEST(test_binder_where_with_many_terms_binds_to_a_shallow_predicate) {
  constexpr int kTermCount = 1024;
  domain::PlanPtr plan = BindOrFail("SELECT id FROM visitors WHERE " +
                                    BalancedConjunction(0, kTermCount));

  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* filter = ChildAs<domain::FilterNode>(project);

  CHECK(filter != nullptr);
  if (filter == nullptr) {
    return;
  }
  const TreeShape shape = MeasureTree(*filter->predicate);
  std::vector<std::int64_t> expected_literals;
  for (int i = 0; i < kTermCount; ++i) {
    expected_literals.push_back(i);
  }
  CHECK(shape.depth <= std::size_t{16});
  CHECK(shape.literals == expected_literals);
}

TEST(test_binder_star_expands_columns_in_schema_order) {
  domain::PlanPtr single = BindOrFail("SELECT * FROM visitors");
  domain::PlanPtr joined = BindOrFail(
      "SELECT * FROM visitors v JOIN stories s ON v.username = s.username");

  const auto* single_project = NodeAs<domain::ProjectNode>(single);
  const auto* joined_project = NodeAs<domain::ProjectNode>(joined);

  CHECK(single_project != nullptr);
  CHECK(joined_project != nullptr);
  if (single_project == nullptr || joined_project == nullptr) {
    return;
  }
  CHECK_EQ(JoinNames(*single_project), "id,username,location");
  CHECK_EQ(JoinNames(*joined_project),
           "id,username,location,id,content,username,views");
  CHECK_EQ(joined_project->exprs.size(), 7u);
  CHECK(IsColumn(joined_project->exprs[6], 6));
}

TEST(test_binder_ungrouped_column_with_aggregate_returns_bind_error) {
  EXPECT_BIND_ERROR("SELECT username, COUNT(*) FROM visitors");
  EXPECT_BIND_ERROR("SELECT *, COUNT(*) FROM visitors");
}

TEST(test_binder_group_by_builds_aggregate_node) {
  domain::PlanPtr plan =
      BindOrFail("SELECT type, COUNT(*) FROM reactions GROUP BY type");

  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* aggregate = ChildAs<domain::AggregateNode>(project);
  const auto* scan = ChildAs<domain::ScanNode>(aggregate);

  CHECK(project != nullptr);
  CHECK(aggregate != nullptr);
  CHECK(scan != nullptr);
  if (aggregate == nullptr || scan == nullptr) {
    return;
  }
  CHECK_EQ(scan->table, "reactions");
  CHECK_EQ(aggregate->group_keys.size(), 1u);
  CHECK_EQ(aggregate->aggregates.size(), 1u);
}

TEST(test_binder_group_key_matching_uses_bound_expressions) {
  const std::vector<std::string> statements = {
      "SELECT v.username, COUNT(*) FROM visitors v GROUP BY username",
      "SELECT views + 1, COUNT(*) FROM stories GROUP BY views",
      "SELECT 1, COUNT(*) FROM stories",
  };

  for (const std::string& sql : statements) {
    CHECK(BindOrFail(sql) != nullptr);
  }
}

TEST(test_binder_order_by_resolves_against_select_list) {
  const std::string select =
      "SELECT id, username AS name, location FROM visitors ORDER BY ";

  CHECK_EQ(FIRST_SORT_COLUMN(select + "name"), 1u);
  CHECK_EQ(FIRST_SORT_COLUMN(select + "1"), 0u);
  CHECK_EQ(FIRST_SORT_COLUMN(select + "3"), 2u);
  CHECK_EQ(FIRST_SORT_COLUMN(select + "location"), 2u);
  EXPECT_BIND_ERROR(select + "0");
  EXPECT_BIND_ERROR(select + "4");
  EXPECT_BIND_ERROR("SELECT id, content FROM stories ORDER BY views");
  CHECK_EQ(
      FIRST_SORT_COLUMN(
          "SELECT id AS username, username FROM visitors ORDER BY username"),
      0u);
}

TEST(test_binder_insert_validates_and_orders_values) {
  const std::vector<std::string> rejected = {
      "INSERT INTO visitors (id, username) VALUES (9, 'zed')",
      "INSERT INTO visitors (id, id, username, location) VALUES (9, 10, 'zed', "
      "'Rome')",
      "INSERT INTO visitors VALUES (id, 'zed', 'Rome')",
      "INSERT INTO visitors VALUES ('x', 'zed', 'Rome')",
  };
  for (const std::string& sql : rejected) {
    EXPECT_BIND_ERROR(sql);
  }

  domain::PlanPtr widened = BindOrFail("INSERT INTO measures VALUES (1, 2)");
  domain::PlanPtr reordered = BindOrFail(
      "INSERT INTO visitors (location, id, username) VALUES ('Rome', 9, "
      "'zed')");

  const auto* widened_insert = NodeAs<domain::InsertNode>(widened);
  const auto* reordered_insert = NodeAs<domain::InsertNode>(reordered);
  CHECK(widened_insert != nullptr);
  CHECK(reordered_insert != nullptr);
  if (widened_insert == nullptr || reordered_insert == nullptr) {
    return;
  }
  CHECK_EQ(widened_insert->table, "measures");
  CHECK_EQ(widened_insert->rows.size(), 1u);
  CHECK(std::holds_alternative<double>(widened_insert->rows[0][1]));
  CHECK(widened_insert->rows[0] ==
        (domain::Row{domain::Value(int64_t{1}), domain::Value(2.0)}));
  CHECK(
      reordered_insert->rows[0] ==
      (domain::Row{domain::Value(int64_t{9}), domain::Value(std::string("zed")),
                   domain::Value(std::string("Rome"))}));
}

TEST(test_binder_update_primary_key_returns_unsupported) {
  EXPECT_BIND_FAILURE("UPDATE visitors SET id = 9 WHERE id = 1",
                      domain::ErrorKind::kUnsupported, "");
  EXPECT_BIND_FAILURE("UPDATE visitors SET id = 'x'",
                      domain::ErrorKind::kUnsupported, "");
}

//
// The Binder keeps the INT-typed expression of an assignment and the engine
// converts it, so this test checks kInt for the value and kDouble for the
// target column.
TEST(test_binder_update_assignment_rules) {
  EXPECT_BIND_ERROR("UPDATE visitors SET location = 5");
  EXPECT_BIND_ERROR("UPDATE visitors SET location = 'a', location = 'b'");
  EXPECT_BIND_ERROR("UPDATE visitors SET nosuch = 'a'");

  domain::PlanPtr plan = BindOrFail("UPDATE measures SET score = 3");

  const auto* update = NodeAs<domain::UpdateNode>(plan);
  CHECK(update != nullptr);
  if (update == nullptr) {
    return;
  }
  CHECK_EQ(update->table, "measures");
  CHECK(update->predicate == nullptr);
  CHECK_EQ(update->assignments.size(), 1u);
  if (update->assignments.size() != 1) {
    return;
  }
  CHECK_EQ(update->assignments[0].column_index, 1u);
  CHECK(MeasuresSchema().columns[update->assignments[0].column_index].type ==
        domain::DataType::kDouble);
  CHECK(update->assignments[0].value->type == domain::ExprType::kInt);
}

TEST(test_binder_predicate_pushdown_places_filters_lowest) {
  domain::PlanPtr plan = BindOrFail(
      "SELECT * FROM visitors v JOIN stories s ON v.username = s.username "
      "WHERE v.id = 1 AND s.views > 0 AND v.id < s.views AND 1 = 1");

  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* top_filter = ChildAs<domain::FilterNode>(project);
  const auto* join = ChildAs<domain::JoinNode>(top_filter);
  const auto* visitors_filter = LeftAs<domain::FilterNode>(join);
  const auto* stories_filter = RightAs<domain::FilterNode>(join);
  const auto* visitors_scan = ChildAs<domain::ScanNode>(visitors_filter);
  const auto* stories_scan = ChildAs<domain::ScanNode>(stories_filter);

  CHECK(project != nullptr);
  CHECK(top_filter != nullptr);
  CHECK(join != nullptr);
  CHECK(visitors_filter != nullptr);
  CHECK(stories_filter != nullptr);
  CHECK(visitors_scan != nullptr);
  CHECK(stories_scan != nullptr);
  if (join == nullptr || visitors_scan == nullptr || stories_scan == nullptr) {
    return;
  }
  CHECK_EQ(visitors_scan->table, "visitors");
  CHECK_EQ(stories_scan->table, "stories");
  // v.id < s.views sits above the join, over the joined row (views is column
  // 6).
  CHECK(IsColumnVsColumn(top_filter->predicate, domain::BinaryOp::kLt, 0, 6));
  // v.id = 1 and the constant 1 = 1 are merged on the first table's scan.
  const domain::BoundBinary* merged =
      AsBinary(visitors_filter->predicate, domain::BinaryOp::kAnd);
  CHECK(merged != nullptr);
  if (merged != nullptr) {
    CHECK(IsColumnVsInt(merged->lhs, domain::BinaryOp::kEq, 0, 1));
    CHECK(IsIntVsInt(merged->rhs, domain::BinaryOp::kEq, 1, 1));
  }
  // s.views > 0 is rebased to the stories row (views is column 3).
  CHECK(IsColumnVsInt(stories_filter->predicate, domain::BinaryOp::kGt, 3, 0));
  // The ON condition stays on the join over the joined row.
  CHECK(IsColumnVsColumn(join->condition, domain::BinaryOp::kEq, 1, 5));
}

TEST(test_binder_join_builds_left_deep_tree) {
  domain::PlanPtr scan_only_plan = BindOrFail("SELECT * FROM visitors");
  domain::PlanPtr single_join_plan = BindOrFail(
      "SELECT * FROM visitors v JOIN stories s ON v.username = s.username");
  domain::PlanPtr double_join_plan = BindOrFail(
      "SELECT * FROM visitors v JOIN stories s ON v.username = s.username "
      "JOIN reactions r ON r.storyId = s.id");

  const auto* scan_only_project = NodeAs<domain::ProjectNode>(scan_only_plan);
  const auto* single_join_project =
      NodeAs<domain::ProjectNode>(single_join_plan);
  const auto* double_join_project =
      NodeAs<domain::ProjectNode>(double_join_plan);

  CHECK(ChildAs<domain::ScanNode>(scan_only_project) != nullptr);
  CHECK(ChildAs<domain::JoinNode>(scan_only_project) == nullptr);
  const auto* single_join = ChildAs<domain::JoinNode>(single_join_project);
  CHECK(single_join != nullptr);
  if (single_join != nullptr) {
    CHECK(LeftAs<domain::ScanNode>(single_join) != nullptr);
    CHECK(RightAs<domain::ScanNode>(single_join) != nullptr);
  }
  const auto* outer = ChildAs<domain::JoinNode>(double_join_project);
  const auto* inner = LeftAs<domain::JoinNode>(outer);
  const auto* outer_right = RightAs<domain::ScanNode>(outer);
  const auto* inner_left = LeftAs<domain::ScanNode>(inner);
  const auto* inner_right = RightAs<domain::ScanNode>(inner);
  CHECK(outer != nullptr);
  CHECK(inner != nullptr);
  CHECK(outer_right != nullptr);
  CHECK(inner_left != nullptr);
  CHECK(inner_right != nullptr);
  if (outer_right == nullptr || inner_left == nullptr ||
      inner_right == nullptr) {
    return;
  }
  CHECK_EQ(inner_left->table, "visitors");
  CHECK_EQ(inner_right->table, "stories");
  CHECK_EQ(outer_right->table, "reactions");
  // r.storyId is column 8 of the joined row (visitors 3, stories 4, then r.id,
  // r.storyId).
  CHECK(IsColumnVsColumn(outer->condition, domain::BinaryOp::kEq, 8, 3));
}

TEST(test_binder_identifiers_are_case_insensitive) {
  const std::vector<std::string> statements = {
      "SELECT STORYID FROM Reactions",
      "SELECT storyId FROM reactions",
  };

  for (const std::string& sql : statements) {
    domain::PlanPtr plan = BindOrFail(sql);
    const auto* project = NodeAs<domain::ProjectNode>(plan);
    const auto* scan = ChildAs<domain::ScanNode>(project);
    CHECK(project != nullptr);
    CHECK(scan != nullptr);
    if (project == nullptr || scan == nullptr) {
      continue;
    }
    CHECK_EQ(scan->table, "reactions");
    CHECK_EQ(JoinNames(*project), "storyId");
  }
}

TEST(test_binder_output_column_names_follow_naming_rule) {
  domain::PlanPtr plan =
      BindOrFail("SELECT id AS key, v.username, id + 1 FROM visitors v");

  const auto* project = NodeAs<domain::ProjectNode>(plan);

  CHECK(project != nullptr);
  if (project == nullptr) {
    return;
  }
  CHECK_EQ(JoinNames(*project), "key,username,id + 1");
}

TEST(test_binder_aggregate_outside_select_list_returns_bind_error) {
  const std::vector<std::string> statements = {
      "SELECT id FROM visitors WHERE COUNT(*) > 1",
      "SELECT v.id FROM visitors v JOIN stories s ON COUNT(*) = 1",
      "SELECT id FROM visitors GROUP BY COUNT(*)",
      "UPDATE stories SET views = COUNT(*)",
      "INSERT INTO visitors VALUES (COUNT(*), 'a', 'b')",
      "SELECT SUM(COUNT(*)) FROM visitors",
  };

  for (const std::string& sql : statements) {
    EXPECT_BIND_FAILURE(sql, domain::ErrorKind::kBind, "aggregate");
  }
}

TEST(test_binder_on_referencing_later_table_returns_bind_error) {
  EXPECT_BIND_ERROR(
      "SELECT * FROM visitors v JOIN stories s ON v.username = r.username "
      "JOIN reactions r ON r.storyId = s.id");
}

TEST(test_binder_duplicate_table_name_returns_bind_error) {
  EXPECT_BIND_ERROR("SELECT * FROM visitors u JOIN stories u ON u.id = u.id");
  EXPECT_BIND_ERROR(
      "SELECT * FROM visitors JOIN visitors ON visitors.id = visitors.id");
}

// A constant INSERT value whose evaluation overflows is kRuntime (binder.h).
TEST(test_binder_insert_constant_overflow_returns_runtime_error) {
  EXPECT_BIND_FAILURE(
      "INSERT INTO visitors VALUES (9223372036854775807 + 1, 'a', 'b')",
      domain::ErrorKind::kRuntime, "numeric overflow");
}

// ProjectNode refers to the AggregateNode output by position; BoundAggregate
// never reaches the plan.
TEST(test_binder_aggregate_items_are_rewritten_over_aggregate_output) {
  domain::PlanPtr plan = BindOrFail(
      "SELECT v.username, COUNT(*) + 1, SUM(s.views) FROM visitors v "
      "JOIN stories s ON v.username = s.username GROUP BY v.username");

  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* aggregate = ChildAs<domain::AggregateNode>(project);

  CHECK(project != nullptr);
  CHECK(aggregate != nullptr);
  if (project == nullptr || aggregate == nullptr) {
    return;
  }
  CHECK(!PlanHasAggregateForm(plan));
  CHECK_EQ(aggregate->group_keys.size(), 1u);
  CHECK(IsColumn(aggregate->group_keys[0], 1));
  CHECK_EQ(aggregate->aggregates.size(), 2u);
  if (aggregate->aggregates.size() != 2) {
    return;
  }
  CHECK(aggregate->aggregates[0].fn == domain::AggregateFn::kCount);
  CHECK(aggregate->aggregates[0].arg == nullptr);
  CHECK(aggregate->aggregates[1].fn == domain::AggregateFn::kSum);
  CHECK(IsColumn(aggregate->aggregates[1].arg, 6));
  CHECK_EQ(project->exprs.size(), 3u);
  if (project->exprs.size() != 3) {
    return;
  }
  CHECK(IsColumn(project->exprs[0], 0));
  const domain::BoundBinary* count_plus_one =
      AsBinary(project->exprs[1], domain::BinaryOp::kAdd);
  CHECK(count_plus_one != nullptr);
  if (count_plus_one != nullptr) {
    CHECK(IsColumn(count_plus_one->lhs, 1));
    CHECK(IsIntLiteral(count_plus_one->rhs, 1));
  }
  CHECK(IsColumn(project->exprs[2], 2));
  CHECK_EQ(JoinNames(*project), "username,COUNT(*) + 1,SUM(s.views)");
}

// ORDER BY case (3) compares bound expressions, which for an aggregate query
// includes the aggregate itself.
TEST(test_binder_order_by_matches_aggregate_select_item) {
  const std::string select =
      "SELECT type, COUNT(*) FROM reactions GROUP BY type ORDER BY ";

  CHECK_EQ(FIRST_SORT_COLUMN(select + "COUNT(*) DESC"), 1u);
  EXPECT_BIND_ERROR(select + "SUM(timestamp)");
}

// ---- DDL and INSERT with omitted columns -----------------------------------

std::vector<std::string> RowText(const domain::Row& row) {
  std::vector<std::string> cells;
  for (const domain::Value& value : row) {
    cells.push_back(domain::FormatValue(value));
  }
  return cells;
}

TEST(test_binder_create_table_builds_schema) {
  domain::PlanPtr plan = BindOrFail(
      "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body "
      "VARCHAR(40) NOT NULL, owner TEXT UNIQUE DEFAULT 'none', score DOUBLE "
      "DEFAULT 1, visitor_id INT REFERENCES visitors(id))");

  const auto* create = NodeAs<domain::CreateTableNode>(plan);
  CHECK(create != nullptr);
  if (create == nullptr) {
    return;
  }
  const domain::TableSchema& schema = create->schema;
  CHECK_EQ(schema.name, std::string("notes"));
  CHECK_EQ(schema.columns.size(), std::size_t{5});
  CHECK_EQ(schema.primary_key_index, std::size_t{0});
  CHECK(schema.auto_increment_index.has_value() &&
        *schema.auto_increment_index == 0);
  CHECK_EQ(schema.unique_indexes, std::vector<std::size_t>({2}));
  CHECK_EQ(schema.foreign_keys.size(), std::size_t{1});
  if (schema.columns.size() != 5 || schema.foreign_keys.size() != 1) {
    return;
  }
  CHECK_EQ(schema.foreign_keys[0].column_index, std::size_t{4});
  CHECK_EQ(schema.foreign_keys[0].ref_table, std::string("visitors"));
  CHECK_EQ(schema.foreign_keys[0].ref_column_index, std::size_t{0});
  CHECK(schema.columns[1].type == domain::DataType::kText);
  CHECK(schema.columns[2].default_value.has_value());
  CHECK(schema.columns[3].default_value.has_value());
  if (!schema.columns[2].default_value || !schema.columns[3].default_value) {
    return;
  }
  CHECK_EQ(std::get<std::string>(*schema.columns[2].default_value),
           std::string("none"));
  // The INT default 1 is converted to the DOUBLE column type.
  CHECK(std::holds_alternative<double>(*schema.columns[3].default_value));
}

TEST(test_binder_create_table_rejects_invalid_definitions) {
  EXPECT_BIND_FAILURE("CREATE TABLE visitors (id INT PRIMARY KEY)",
                      domain::ErrorKind::kBind,
                      "table visitors already exists");
  EXPECT_BIND_FAILURE("CREATE TABLE VISITORS (id INT PRIMARY KEY)",
                      domain::ErrorKind::kBind, "already exists");
  EXPECT_BIND_FAILURE("CREATE TABLE catalog (id INT PRIMARY KEY)",
                      domain::ErrorKind::kBind,
                      "table name catalog is reserved");
  EXPECT_BIND_FAILURE("CREATE TABLE CATALOG (id INT PRIMARY KEY)",
                      domain::ErrorKind::kBind, "is reserved");
  const char* const statements[] = {
      "CREATE TABLE t (id INT PRIMARY KEY, ID TEXT)",
      "CREATE TABLE t (a INT)",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)",
      "CREATE TABLE t (a INT PRIMARY KEY DEFAULT 1)",
      "CREATE TABLE t (a TEXT PRIMARY KEY AUTOINCREMENT)",
      "CREATE TABLE t (a INT PRIMARY KEY AUTOINCREMENT DEFAULT 5)",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT DEFAULT a)",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT DEFAULT 'x')",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT DEFAULT COUNT(*))",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT DEFAULT 1 < 2)",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT REFERENCES nosuch(id))",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT REFERENCES visitors(nosuch))",
      ("CREATE TABLE t (a INT PRIMARY KEY, b TEXT REFERENCES "
       "visitors(location))"),
      "CREATE TABLE t (a INT PRIMARY KEY, b TEXT REFERENCES visitors(id))",
      "CREATE TABLE t (a INT PRIMARY KEY, b INT REFERENCES t(a))",
  };
  for (const char* sql : statements) {
    EXPECT_BIND_ERROR(sql);
  }
}

TEST(test_binder_create_table_default_overflow_returns_runtime_error) {
  EXPECT_BIND_FAILURE(
      "CREATE TABLE t (id INT PRIMARY KEY, n INT DEFAULT "
      "9223372036854775807 + 1)",
      domain::ErrorKind::kRuntime, "numeric overflow");
}

TEST(test_binder_add_column_builds_node_and_applies_rules) {
  domain::PlanPtr plan =
      BindOrFail("ALTER TABLE visitors ADD COLUMN age INT DEFAULT 0");
  const char* const statements[] = {
      "ALTER TABLE nosuch ADD COLUMN a INT DEFAULT 0",
      "ALTER TABLE visitors ADD COLUMN USERNAME TEXT DEFAULT 'x'",
      "ALTER TABLE visitors ADD COLUMN a INT",
      "ALTER TABLE visitors ADD COLUMN a INT PRIMARY KEY",
      "ALTER TABLE visitors ADD COLUMN a INT UNIQUE DEFAULT 0",
      "ALTER TABLE visitors ADD COLUMN a INT REFERENCES visitors(id) DEFAULT 0",
      "ALTER TABLE visitors ADD COLUMN a INT PRIMARY KEY AUTOINCREMENT",
      "ALTER TABLE visitors ADD COLUMN a INT DEFAULT 'x'",
  };

  const auto* add_column = NodeAs<domain::AddColumnNode>(plan);
  CHECK(add_column != nullptr);
  if (add_column != nullptr) {
    CHECK_EQ(add_column->table, std::string("visitors"));
    CHECK_EQ(add_column->column.name, std::string("age"));
    CHECK(add_column->column.default_value.has_value());
    if (add_column->column.default_value) {
      CHECK_EQ(std::get<int64_t>(*add_column->column.default_value),
               int64_t{0});
    }
  }
  for (const char* sql : statements) {
    EXPECT_BIND_ERROR(sql);
  }
}

TEST(test_binder_drop_table_checks_references) {
  domain::PlanPtr plan = BindOrFail("DROP TABLE measures");

  const auto* drop = NodeAs<domain::DropTableNode>(plan);
  CHECK(drop != nullptr);
  if (drop != nullptr) {
    CHECK_EQ(drop->table, std::string("measures"));
  }
  EXPECT_BIND_FAILURE("DROP TABLE visitors", domain::ErrorKind::kBind,
                      "table visitors is referenced by stories.username");
  EXPECT_BIND_FAILURE("DROP TABLE stories", domain::ErrorKind::kBind,
                      "table stories is referenced by reactions.storyId");
  EXPECT_BIND_ERROR("DROP TABLE nosuch");
}

TEST(test_binder_insert_omitted_columns_use_defaults) {
  domain::PlanPtr listed =
      BindOrFail("INSERT INTO drafts (id, body) VALUES (1, 'a')");
  domain::PlanPtr reordered =
      BindOrFail("INSERT INTO drafts (owner, id, body) VALUES ('me', 2, 'b')");

  const auto* first = NodeAs<domain::InsertNode>(listed);
  const auto* second = NodeAs<domain::InsertNode>(reordered);
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  if (first == nullptr || second == nullptr) {
    return;
  }
  CHECK_EQ(RowText(first->rows[0]),
           std::vector<std::string>({"1", "a", "none", "1.0"}));
  CHECK_EQ(RowText(second->rows[0]),
           std::vector<std::string>({"2", "b", "me", "1.0"}));
  CHECK(!first->generated_index.has_value());
  CHECK(!second->generated_index.has_value());
}

TEST(test_binder_insert_omitted_auto_increment_sets_generated_index) {
  domain::PlanPtr omitted = BindOrFail("INSERT INTO jobs (body) VALUES ('a')");
  domain::PlanPtr given =
      BindOrFail("INSERT INTO jobs (id, body) VALUES (7, 'a')");

  const auto* generated = NodeAs<domain::InsertNode>(omitted);
  const auto* explicit_id = NodeAs<domain::InsertNode>(given);
  CHECK(generated != nullptr);
  CHECK(explicit_id != nullptr);
  if (generated == nullptr || explicit_id == nullptr) {
    return;
  }
  CHECK(generated->generated_index.has_value() &&
        *generated->generated_index == 0);
  CHECK(!explicit_id->generated_index.has_value());
  CHECK_EQ(RowText(explicit_id->rows[0]), std::vector<std::string>({"7", "a"}));
}

TEST(test_binder_insert_omitted_column_without_default_returns_bind_error) {
  EXPECT_BIND_FAILURE("INSERT INTO drafts (body) VALUES ('a')",
                      domain::ErrorKind::kBind, "no value for column id");
  EXPECT_BIND_ERROR("INSERT INTO drafts VALUES (1, 'a')");
}

// ---- Operators, functions, aggregates, LIMIT, multi-row INSERT -------------

TEST(test_binder_multiplication_and_division_types) {
  CHECK_EQ(OutputTypes("SELECT id * 2, id * 2.0, id / 2, id / 2.0, 1.5 / 2 "
                       "FROM visitors"),
           std::string("INT,DOUBLE,INT,DOUBLE,DOUBLE"));
  EXPECT_BIND_FAILURE("SELECT username * 2 FROM visitors",
                      domain::ErrorKind::kBind,
                      "operator * cannot be applied to TEXT and INT");
  EXPECT_BIND_FAILURE("SELECT id / username FROM visitors",
                      domain::ErrorKind::kBind,
                      "operator / cannot be applied to INT and TEXT");
  EXPECT_BIND_FAILURE("SELECT (id = 1) * 2 FROM visitors",
                      domain::ErrorKind::kBind, "BOOL");
}

TEST(test_binder_concat_and_like_typing) {
  CHECK_EQ(OutputTypes("SELECT username || location FROM visitors"),
           std::string("TEXT"));
  domain::PlanPtr plan =
      BindOrFail("SELECT id FROM visitors WHERE username LIKE 'a%'");
  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* filter = ChildAs<domain::FilterNode>(project);
  CHECK(filter != nullptr);
  if (filter != nullptr) {
    CHECK(filter->predicate->type == domain::ExprType::kBool);
    CHECK(AsBinary(filter->predicate, domain::BinaryOp::kLike) != nullptr);
  }
  EXPECT_BIND_FAILURE("SELECT username || id FROM visitors",
                      domain::ErrorKind::kBind,
                      "operator || cannot be applied to TEXT and INT");
  EXPECT_BIND_FAILURE("SELECT id FROM visitors WHERE id LIKE 1",
                      domain::ErrorKind::kBind,
                      "operator LIKE cannot be applied to INT and INT");
  EXPECT_BIND_FAILURE("SELECT username LIKE 'a' FROM visitors",
                      domain::ErrorKind::kBind, "boolean");
  EXPECT_BIND_FAILURE("SELECT id FROM visitors WHERE username || 'x'",
                      domain::ErrorKind::kBind, "boolean");
}

TEST(test_binder_function_arguments_are_checked) {
  struct ResultCase {
    const char* expression;
    const char* types;
  };
  const ResultCase results[] = {
      {"LOWER(username)", "TEXT"},
      {"UPPER(username)", "TEXT"},
      {"TRIM(username)", "TEXT"},
      {"SUBSTR(username, 1)", "TEXT"},
      {"SUBSTR(username, 1, 2)", "TEXT"},
      {"LENGTH(username)", "INT"},
      {"ABS(id)", "INT"},
      {"ABS(id + 0.5)", "DOUBLE"},
      {"ROUND(id)", "DOUBLE"},
      {"ROUND(1.5, 1)", "DOUBLE"},
      {"CAST(id AS INT)", "INT"},
      {"CAST(id AS DOUBLE)", "DOUBLE"},
      {"CAST(id AS TEXT)", "TEXT"},
      {"CAST(username AS INT)", "INT"},
  };
  for (const ResultCase& result_case : results) {
    CHECK_EQ(OutputTypes(std::string("SELECT ") + result_case.expression +
                         " FROM visitors"),
             std::string(result_case.types));
  }

  struct ErrorCase {
    const char* expression;
    const char* fragment;
  };
  const ErrorCase errors[] = {
      {"LOWER(1)", "LOWER argument 1 must be TEXT, got INT"},
      {"LOWER(username, location)", "LOWER requires 1 argument, got 2"},
      {"SUBSTR(username)", "SUBSTR requires 2 or 3 arguments, got 1"},
      {"SUBSTR(username, 'a')", "SUBSTR argument 2 must be INT, got TEXT"},
      {"SUBSTR(username, 1, 2, 3)", "SUBSTR requires 2 or 3 arguments, got 4"},
      {"ROUND(username)", "ROUND argument 1 must be INT or DOUBLE, got TEXT"},
      {"ROUND(1.5, 1.5)", "ROUND argument 2 must be INT, got DOUBLE"},
      {"ABS(username)", "ABS argument 1 must be INT or DOUBLE, got TEXT"},
      {"TRIM(1)", "TRIM argument 1 must be TEXT, got INT"},
      {"LENGTH(1.5)", "LENGTH argument 1 must be TEXT, got DOUBLE"},
      {"LOWER(id = 1)", "LOWER argument 1 must be TEXT, got BOOL"},
      {"CAST(id = 1 AS INT)",
       "CAST argument 1 must be INT, DOUBLE or TEXT, got BOOL"},
  };
  for (const ErrorCase& error_case : errors) {
    EXPECT_BIND_FAILURE(
        std::string("SELECT ") + error_case.expression + " FROM visitors",
        domain::ErrorKind::kBind, error_case.fragment);
  }
}

TEST(test_binder_avg_min_max_types) {
  CHECK_EQ(OutputTypes("SELECT AVG(id), AVG(score), MIN(id), MAX(score) FROM "
                       "measures"),
           std::string("DOUBLE,DOUBLE,INT,DOUBLE"));
  CHECK_EQ(OutputTypes("SELECT MIN(username), MAX(username) FROM visitors"),
           std::string("TEXT,TEXT"));
  EXPECT_BIND_FAILURE("SELECT AVG(username) FROM visitors",
                      domain::ErrorKind::kBind,
                      "AVG requires a numeric argument, got TEXT");
  EXPECT_BIND_FAILURE("SELECT SUM(username) FROM visitors",
                      domain::ErrorKind::kBind,
                      "SUM requires a numeric argument, got TEXT");
  EXPECT_BIND_FAILURE("SELECT id FROM visitors WHERE MAX(id) > 1",
                      domain::ErrorKind::kBind, "aggregate");
  EXPECT_BIND_FAILURE("SELECT MIN(MAX(id)) FROM visitors",
                      domain::ErrorKind::kBind, "nested");
  EXPECT_BIND_FAILURE("SELECT username, MAX(id) FROM visitors",
                      domain::ErrorKind::kBind, "must appear in GROUP BY");
  domain::PlanPtr grouped =
      BindOrFail("SELECT username, MAX(id) FROM visitors GROUP BY username");
  CHECK(grouped != nullptr);
  CHECK(!PlanHasAggregateForm(grouped));
}

TEST(test_binder_limit_builds_limit_node_above_sort) {
  domain::PlanPtr plain = BindOrFail("SELECT id FROM visitors LIMIT 5");
  domain::PlanPtr sorted =
      BindOrFail("SELECT id FROM visitors ORDER BY id LIMIT 5 OFFSET 2");
  domain::PlanPtr zero = BindOrFail("SELECT id FROM visitors LIMIT 0");
  domain::PlanPtr grouped = BindOrFail(
      "SELECT username, COUNT(*) FROM visitors GROUP BY username ORDER BY "
      "username LIMIT 3");
  domain::PlanPtr unlimited = BindOrFail("SELECT id FROM visitors ORDER BY id");

  const auto* plain_limit = NodeAs<domain::LimitNode>(plain);
  const auto* sorted_limit = NodeAs<domain::LimitNode>(sorted);
  const auto* zero_limit = NodeAs<domain::LimitNode>(zero);
  const auto* grouped_limit = NodeAs<domain::LimitNode>(grouped);
  CHECK(plain_limit != nullptr);
  CHECK(sorted_limit != nullptr);
  CHECK(zero_limit != nullptr);
  CHECK(grouped_limit != nullptr);
  if (plain_limit == nullptr || sorted_limit == nullptr ||
      zero_limit == nullptr || grouped_limit == nullptr) {
    return;
  }
  CHECK(plain_limit->limit.has_value() && *plain_limit->limit == 5);
  CHECK_EQ(plain_limit->offset, int64_t{0});
  CHECK(ChildAs<domain::ProjectNode>(plain_limit) != nullptr);
  CHECK(sorted_limit->limit.has_value() && *sorted_limit->limit == 5);
  CHECK_EQ(sorted_limit->offset, int64_t{2});
  CHECK(ChildAs<domain::SortNode>(sorted_limit) != nullptr);
  CHECK(zero_limit->limit.has_value() && *zero_limit->limit == 0);
  CHECK(grouped_limit->limit.has_value() && *grouped_limit->limit == 3);
  const auto* grouped_sort = ChildAs<domain::SortNode>(grouped_limit);
  const auto* grouped_project = ChildAs<domain::ProjectNode>(grouped_sort);
  CHECK(ChildAs<domain::AggregateNode>(grouped_project) != nullptr);
  CHECK(NodeAs<domain::LimitNode>(unlimited) == nullptr);
  CHECK(NodeAs<domain::SortNode>(unlimited) != nullptr);
}

TEST(test_binder_function_in_group_by_and_order_by_matches_structurally) {
  domain::PlanPtr grouped = BindOrFail(
      "SELECT UPPER(username), COUNT(*) FROM visitors GROUP BY UPPER(username) "
      "ORDER BY UPPER(username)");
  domain::PlanPtr by_column =
      BindOrFail("SELECT UPPER(username) FROM visitors GROUP BY username");
  CHECK(grouped != nullptr);
  CHECK(by_column != nullptr);
  CHECK(!PlanHasAggregateForm(grouped));
  CHECK_EQ(FIRST_SORT_COLUMN("SELECT LENGTH(username) FROM visitors ORDER BY "
                             "LENGTH(username)"),
           0u);
  CHECK_EQ(
      FIRST_SORT_COLUMN("SELECT UPPER(username), COUNT(*) FROM visitors GROUP "
                        "BY UPPER(username) ORDER BY UPPER(username)"),
      0u);
  EXPECT_BIND_FAILURE("SELECT LOWER(location) FROM visitors GROUP BY username",
                      domain::ErrorKind::kBind, "must appear in GROUP BY");
  EXPECT_BIND_FAILURE("SELECT username FROM visitors ORDER BY LENGTH(username)",
                      domain::ErrorKind::kBind,
                      "does not match any SELECT list item");
  EXPECT_BIND_FAILURE(
      "SELECT UPPER(username) FROM visitors ORDER BY LOWER(username)",
      domain::ErrorKind::kBind, "does not match any SELECT list item");
}

TEST(test_binder_multi_row_insert_builds_rows_and_prefixes_errors) {
  domain::PlanPtr drafts =
      BindOrFail("INSERT INTO drafts (id, body) VALUES (1, 'a'), (2, 'b')");
  domain::PlanPtr jobs =
      BindOrFail("INSERT INTO jobs (body) VALUES ('a'), ('b'), ('c')");

  const auto* drafts_insert = NodeAs<domain::InsertNode>(drafts);
  const auto* jobs_insert = NodeAs<domain::InsertNode>(jobs);
  CHECK(drafts_insert != nullptr);
  CHECK(jobs_insert != nullptr);
  if (drafts_insert == nullptr || jobs_insert == nullptr) {
    return;
  }
  CHECK_EQ(drafts_insert->rows.size(), std::size_t{2});
  if (drafts_insert->rows.size() == 2) {
    CHECK_EQ(RowText(drafts_insert->rows[0]),
             std::vector<std::string>({"1", "a", "none", "1.0"}));
    CHECK_EQ(RowText(drafts_insert->rows[1]),
             std::vector<std::string>({"2", "b", "none", "1.0"}));
  }
  CHECK_EQ(jobs_insert->rows.size(), std::size_t{3});
  CHECK(jobs_insert->generated_index.has_value() &&
        *jobs_insert->generated_index == 0);

  EXPECT_BIND_FAILURE("INSERT INTO drafts (id, body) VALUES (1, 'a'), (2)",
                      domain::ErrorKind::kBind,
                      "row 2: INSERT has 1 values for 2 columns");
  EXPECT_BIND_FAILURE("INSERT INTO drafts (id, body) VALUES (1, 'a'), (2, 3)",
                      domain::ErrorKind::kBind,
                      "row 2: cannot assign INT to column 'body'");
  EXPECT_BIND_FAILURE(
      "INSERT INTO drafts (id, body) VALUES (1, 'a'), (id, 'b')",
      domain::ErrorKind::kBind, "row 2: INSERT values must be constants");
  EXPECT_BIND_FAILURE(
      "INSERT INTO drafts (id, body) VALUES (1, 'a'), (1 / 0, 'b')",
      domain::ErrorKind::kRuntime, "row 2: division by zero");
  EXPECT_BIND_FAILURE("INSERT INTO drafts (id, body) VALUES (1)",
                      domain::ErrorKind::kBind, "INSERT has 1 values");
  FakeCatalog catalog;
  domain::BindResult single = domain::Bind(
      MustParse("INSERT INTO drafts (id, body) VALUES (1)"), catalog);
  const auto* single_error = std::get_if<domain::ExecutionError>(&single);
  CHECK(single_error != nullptr);
  if (single_error != nullptr) {
    CHECK(single_error->message.find("row ") == std::string::npos);
  }
}

TEST(test_binder_functions_in_constants_and_default) {
  domain::PlanPtr insert = BindOrFail(
      "INSERT INTO drafts (id, body) VALUES (1, UPPER('a') || CAST(1 + 2 * 3 "
      "AS TEXT))");
  const auto* insert_node = NodeAs<domain::InsertNode>(insert);
  CHECK(insert_node != nullptr);
  if (insert_node != nullptr && insert_node->rows.size() == 1) {
    CHECK_EQ(RowText(insert_node->rows[0]),
             std::vector<std::string>({"1", "A7", "none", "1.0"}));
  }

  domain::PlanPtr create = BindOrFail(
      "CREATE TABLE t (id INT PRIMARY KEY, n TEXT DEFAULT LOWER('ABC'))");
  const auto* create_node = NodeAs<domain::CreateTableNode>(create);
  CHECK(create_node != nullptr);
  if (create_node != nullptr && create_node->schema.columns.size() == 2) {
    const auto& default_value = create_node->schema.columns[1].default_value;
    CHECK(default_value.has_value());
    if (default_value.has_value()) {
      CHECK_EQ(domain::FormatValue(*default_value), std::string("abc"));
    }
  }
  EXPECT_BIND_FAILURE(
      "CREATE TABLE t (id INT PRIMARY KEY, n INT DEFAULT 1 / 0)",
      domain::ErrorKind::kRuntime, "division by zero");
  EXPECT_BIND_FAILURE(
      "CREATE TABLE t (id INT PRIMARY KEY, n INT DEFAULT LENGTH(id))",
      domain::ErrorKind::kBind, "constant");
  EXPECT_BIND_FAILURE(
      "CREATE TABLE t (id INT PRIMARY KEY, n TEXT DEFAULT LOWER(1))",
      domain::ErrorKind::kBind, "LOWER argument 1 must be TEXT");
}

TEST(test_binder_predicate_pushdown_handles_function_arguments) {
  domain::PlanPtr plan = BindOrFail(
      "SELECT v.id FROM visitors v JOIN stories s ON s.username = v.username "
      "WHERE "
      "LOWER(v.username) = 'a' AND LENGTH(s.content) > 1 AND "
      "LENGTH(v.username || s.content) > 3");

  const auto* project = NodeAs<domain::ProjectNode>(plan);
  const auto* above_join = ChildAs<domain::FilterNode>(project);
  const auto* join = above_join == nullptr
                         ? nullptr
                         : NodeAs<domain::JoinNode>(above_join->child);
  const auto* visitors_filter = LeftAs<domain::FilterNode>(join);
  const auto* stories_filter = RightAs<domain::FilterNode>(join);
  CHECK(above_join != nullptr);
  CHECK(visitors_filter != nullptr);
  CHECK(stories_filter != nullptr);
  if (above_join == nullptr || visitors_filter == nullptr ||
      stories_filter == nullptr) {
    return;
  }
  CHECK(NodeAs<domain::ScanNode>(visitors_filter->child) != nullptr);
  CHECK(NodeAs<domain::ScanNode>(stories_filter->child) != nullptr);

  // visitors.username is position 1 of a visitors row; stories.content is
  // position 1 of a stories row, and position 4 (3 + 1) of the joined row.
  const domain::BoundBinary* visitors_predicate =
      AsBinary(visitors_filter->predicate, domain::BinaryOp::kEq);
  const domain::BoundBinary* stories_predicate =
      AsBinary(stories_filter->predicate, domain::BinaryOp::kGt);
  CHECK(visitors_predicate != nullptr);
  CHECK(stories_predicate != nullptr);
  if (visitors_predicate == nullptr || stories_predicate == nullptr) {
    return;
  }
  const auto* lower =
      std::get_if<domain::BoundFunction>(&visitors_predicate->lhs->node);
  const auto* length =
      std::get_if<domain::BoundFunction>(&stories_predicate->lhs->node);
  CHECK(lower != nullptr && lower->fn == domain::ScalarFn::kLower);
  CHECK(length != nullptr && length->fn == domain::ScalarFn::kLength);
  if (lower != nullptr && length != nullptr) {
    CHECK(IsColumn(lower->args[0], 1));
    CHECK(IsColumn(length->args[0], 1));
  }
  const domain::BoundBinary* cross_predicate =
      AsBinary(above_join->predicate, domain::BinaryOp::kGt);
  CHECK(cross_predicate != nullptr);
}

TEST(test_binder_suggests_close_names_for_unknown_tables_and_columns) {
  EXPECT_BIND_FAILURE("SELECT id FROM visitor", domain::ErrorKind::kBind,
                      "unknown table 'visitor'; did you mean visitors?");
  EXPECT_BIND_FAILURE("SELECT usrname FROM visitors", domain::ErrorKind::kBind,
                      "; did you mean username?");
  EXPECT_BIND_FAILURE("INSERT INTO visitors (usernam) VALUES ('a')",
                      domain::ErrorKind::kBind, "; did you mean username?");
  EXPECT_BIND_FAILURE("UPDATE visitors SET locaton = 'x'",
                      domain::ErrorKind::kBind, "; did you mean location?");
  EXPECT_BIND_FAILURE("SELECT v.usrname FROM visitors v",
                      domain::ErrorKind::kBind,
                      "unknown column 'v.usrname'; did you mean username?");
  // The comparison ignores case.
  EXPECT_BIND_FAILURE("SELECT USRNAME FROM visitors", domain::ErrorKind::kBind,
                      "; did you mean username?");
  // Edit distance 2 still suggests; edit distance 3 does not.
  EXPECT_BIND_FAILURE("SELECT usrnme FROM visitors", domain::ErrorKind::kBind,
                      "; did you mean username?");

  // Too far from every name, or an exact match of another table's column.
  FakeCatalog catalog;
  domain::BindResult far =
      domain::Bind(MustParse("SELECT id FROM zzzzzz"), catalog);
  const auto* error = std::get_if<domain::ExecutionError>(&far);
  CHECK(error != nullptr);
  if (error != nullptr) {
    CHECK(error->message.find("did you mean") == std::string::npos);
  }
  domain::BindResult three_edits =
      domain::Bind(MustParse("SELECT usrnm FROM visitors"), catalog);
  const auto* three_edits_error =
      std::get_if<domain::ExecutionError>(&three_edits);
  CHECK(three_edits_error != nullptr);
  if (three_edits_error != nullptr) {
    CHECK(three_edits_error->message.find("did you mean") == std::string::npos);
  }
}

TEST(test_binder_suggestion_skips_a_name_far_longer_than_the_columns) {
  // A table of 4000 columns and a name of 100000 characters: an edit distance
  // against every column takes seconds, and no column is within two edits of a
  // name that is 99990 characters longer.
  constexpr std::size_t kColumnCount = 4000;
  constexpr std::size_t kNameLength = 100000;
  constexpr double kSecondsLimit = 1.0;
  const WideCatalog catalog(kColumnCount);
  const std::string long_name(kNameLength, 'a');

  const auto start = std::chrono::steady_clock::now();
  const std::string far_message =
      BindMessage("SELECT " + long_name + " FROM wide", catalog);
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;
  const std::string close_message =
      BindMessage("SELECT c12x FROM wide", catalog);

  CHECK(far_message.find("unknown column") != std::string::npos);
  CHECK(far_message.find("did you mean") == std::string::npos);
  CHECK(close_message.find("; did you mean c12?") != std::string::npos);
  CHECK(elapsed.count() < kSecondsLimit);
}

}  // namespace strata::testing
