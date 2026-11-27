#include "test_equivalence_generator.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace strata::testing {

namespace {

using domain::DataType;
using domain::Row;
using domain::Value;

// ----------------------------------------------------------------------------
// The schema and the data of the setup.
// ----------------------------------------------------------------------------

struct ColumnSpec {
  std::string name;
  DataType type;
};

// The tables of the setup, in creation order.
const std::vector<std::pair<std::string, std::vector<ColumnSpec>>>& Tables() {
  static const std::vector<std::pair<std::string, std::vector<ColumnSpec>>>
      tables = {{"visitors",
                 {{"id", DataType::kInt},
                  {"username", DataType::kText},
                  {"location", DataType::kText},
                  {"rating", DataType::kDouble}}},
                {"stories",
                 {{"id", DataType::kInt},
                  {"content", DataType::kText},
                  {"username", DataType::kText},
                  {"views", DataType::kInt}}},
                {"reactions",
                 {{"id", DataType::kInt},
                  {"storyid", DataType::kInt},
                  {"username", DataType::kText},
                  {"type", DataType::kText},
                  {"weight", DataType::kDouble},
                  {"timestamp", DataType::kInt}}}};
  return tables;
}

const std::vector<ColumnSpec>& ColumnsOf(const std::string& table) {
  for (const auto& entry : Tables()) {
    if (entry.first == table) {
      return entry.second;
    }
  }
  return Tables().front().second;
}

// ----------------------------------------------------------------------------
// Value pools. Text is given as the raw value; Quote makes the SQL literal.
// ----------------------------------------------------------------------------

const std::vector<std::string>& NamePool() {
  static const std::vector<std::string> pool = {"ann", "annx", "annxx", "bob",
                                                "cat", "dan",  "eve",   "fay",
                                                "gus", "hal",  "ivy",   "jo"};
  return pool;
}

const std::vector<std::string>& LocationPool() {
  static const std::vector<std::string> pool = {
      "Paris",     "Rome",   "Oslo",         "Lima", "", "S\xC3\xA3o Paulo",
      "New, York", "O'Neil", "Z\xC3\xBCrich"};
  return pool;
}

const std::vector<std::string>& ContentPool() {
  static const std::vector<std::string> pool = {"hello world",
                                                "second story",
                                                "",
                                                "a, b and \"c\"",
                                                "h\xC3\xA9llo w\xC3\xB6rld",
                                                "  padded  ",
                                                "LONG text of words",
                                                "42",
                                                "-7",
                                                "\ttabbed\t",
                                                "back\\slash",
                                                "it's quoted"};
  return pool;
}

const std::vector<std::string>& TypePool() {
  static const std::vector<std::string> pool = {"like", "comment", "share",
                                                "view"};
  return pool;
}

// No -0.0: a DOUBLE cell that holds one zero and receives the other zero in an
// UPDATE is "unchanged" under ==, and the two engines then disagree on the sign
// that is stored (flatfile keeps the new zero whenever another cell of the
// table changes in the same statement; paged keeps the old cell, as
// DesignDocument 2.5 says). The fixed corpus covers the sign of zero where the
// engines agree.
const std::vector<std::string>& DoublePool() {
  static const std::vector<std::string> pool = {
      "0.0", "0.5", "1.0", "2.5", "3.25", "-1.5", "100.125", "4.5"};
  return pool;
}

const std::vector<std::string>& SmallIntPool() {
  static const std::vector<std::string> pool = {"0", "1", "2",  "3",
                                                "5", "7", "10", "-4"};
  return pool;
}

const std::vector<std::string>& PatternPool() {
  static const std::vector<std::string> pool = {
      "%", "%o%", "h_llo%", "%d", "_a%", "%x", "S%", "%, %", ""};
  return pool;
}

// The timestamps include one value at the 64-bit limit, so that arithmetic on
// timestamp can overflow.
const std::vector<std::string>& TimestampPool() {
  static const std::vector<std::string> pool = {"0",
                                                "1000",
                                                "1692407706",
                                                "1722355167",
                                                "1722355167",
                                                "-5",
                                                "9007199254740992",
                                                "9007199254740993",
                                                "9223372036854775807"};
  return pool;
}

std::string Quote(const std::string& text) {
  std::string quoted = "'";
  for (char character : text) {
    quoted += character;
    if (character == '\'') {
      quoted += '\'';
    }
  }
  return quoted + "'";
}

std::string LiteralOf(const Value& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    return Quote(*text);
  }
  return domain::FormatValue(value);
}

std::string Join(const std::vector<std::string>& parts,
                 const std::string& separator) {
  std::string joined;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      joined += separator;
    }
    joined += parts[i];
  }
  return joined;
}

// The statements that build the setup (see the header).
std::vector<std::string> BuildSetup() {
  return {
      "CREATE TABLE visitors (id INT PRIMARY KEY AUTOINCREMENT, username TEXT "
      "UNIQUE, location TEXT, rating DOUBLE DEFAULT 0.0)",
      "CREATE TABLE stories (id INT PRIMARY KEY AUTOINCREMENT, content TEXT, "
      "username TEXT REFERENCES visitors(username), views INT DEFAULT 0)",
      "CREATE TABLE reactions (id INT PRIMARY KEY, storyid INT REFERENCES "
      "stories(id), username TEXT REFERENCES visitors(username), type TEXT, "
      "weight "
      "DOUBLE, timestamp INT)",
      "INSERT INTO visitors VALUES (1, 'ann', 'Paris', 4.5), (2, 'bob', "
      "'Rome', "
      "3.25), (3, 'cat', 'Paris', 4.5), (4, 'dan', 'Oslo', 0.0), (5, 'eve', "
      "'Rome', 2.0), (6, 'annx', 'Oslo', 3.25)",
      "INSERT INTO stories VALUES (1, 'hello world', 'ann', 5), (2, 'second "
      "story', 'ann', 0), (3, 'hello world', 'bob', 7), (4, 'a, b and \"c\"', "
      "'cat', 7), (5, '', 'dan', 12), (6, 'second story', 'eve', 0), (7, "
      "'h\xC3\xA9llo w\xC3\xB6rld', 'bob', 3), (8, '  padded  ', 'cat', 3), "
      "(9, 'LONG text of words', 'annx', 20), (10, 'hello world', 'ann', 5)",
      "INSERT INTO reactions VALUES (1, 1, 'bob', 'like', 1.0, 1000), (2, "
      "1, 'cat', 'comment', 2.5, 1001), (3, 2, 'bob', 'like', 1.0, 1002), (4, "
      "3, 'ann', 'like', 1.0, 1003), (5, 3, 'cat', 'share', 0.5, 1004), (6, "
      "4, 'ann', 'comment', 2.5, 1005), (7, 4, 'bob', 'like', 1.0, 1006), (8, "
      "5, 'eve', 'like', 1.0, 1007), (9, 5, 'ann', 'share', 0.5, 1008), (10, "
      "6, 'bob', 'comment', 2.5, 1009), (11, 7, 'dan', 'like', 1.0, 1010), "
      "(12, 7, 'eve', 'like', 1.0, 1011), (13, 8, 'ann', 'share', 0.5, 1012), "
      "(14, 9, 'cat', 'like', 1.0, 1013), (15, 9, 'bob', 'comment', 2.5, "
      "1014), (16, 10, 'eve', 'like', 1.0, 1015), (17, 1, 'dan', 'share', "
      "0.5, 1016), (18, 2, 'cat', 'comment', 2.5, 1017), (19, 3, 'dan', "
      "'like', 1.0, 1018), (20, 6, 'ann', 'like', 1.0, 1019)"};
}

// The column names of a table that have the given type, qualified with the
// alias of the source.
std::vector<std::string> ColumnNames(const std::string& alias,
                                     const std::string& table, DataType type) {
  std::vector<std::string> names;
  for (const ColumnSpec& column : ColumnsOf(table)) {
    if (column.type == type) {
      names.push_back(alias + "." + column.name);
    }
  }
  return names;
}

}  // namespace

const std::vector<std::string>& EquivalenceSetupStatements() {
  static const std::vector<std::string> statements = BuildSetup();
  return statements;
}

// ----------------------------------------------------------------------------
// Randomness and values that exist.
// ----------------------------------------------------------------------------

StatementGenerator::StatementGenerator(std::uint64_t seed,
                                       std::size_t key_percent)
    : engine_(seed), key_percent_(key_percent) {}

std::size_t StatementGenerator::Below(std::size_t bound) {
  return static_cast<std::size_t>(engine_() % bound);
}

bool StatementGenerator::Chance(std::size_t percent) {
  return Below(100) < percent;
}

const std::string& StatementGenerator::PickText(
    const std::vector<std::string>& options) {
  return options[Below(options.size())];
}

// The SQL literal of one column of a random row of the table, or the fallback
// (already a literal) when the table is empty.
std::string StatementGenerator::ExistingLiteral(const std::string& table,
                                                std::size_t column,
                                                const std::string& fallback) {
  auto found = tables_->find(table);
  if (found == tables_->end() || found->second.empty()) {
    return fallback;
  }
  const Row& row = found->second[Below(found->second.size())];
  return LiteralOf(row[column]);
}

// An id of a row that exists (85%), or a number near the existing ids.
std::int64_t StatementGenerator::ExistingOrNewId(const std::string& table) {
  auto found = tables_->find(table);
  if (found != tables_->end() && !found->second.empty() && Chance(85)) {
    const Row& row = found->second[Below(found->second.size())];
    return std::get<std::int64_t>(row[0]);
  }
  return static_cast<std::int64_t>(1 + Below(120));
}

// ----------------------------------------------------------------------------
// Expressions.
// ----------------------------------------------------------------------------

std::string StatementGenerator::CompareOperator() {
  static const std::vector<std::string> operators = {"=",  "<>", "!=", "<",
                                                     "<=", ">",  ">="};
  return PickText(operators);
}

// An INT column of small magnitude (not timestamp), or a small literal.
std::string StatementGenerator::SmallInt(const Scope& scope) {
  std::vector<std::string> columns;
  for (const Source& source : scope) {
    for (const ColumnSpec& column : ColumnsOf(source.table)) {
      if (column.type == DataType::kInt && column.name != "timestamp") {
        columns.push_back(source.alias + "." + column.name);
      }
    }
  }
  if (!columns.empty() && Chance(75)) {
    return PickText(columns);
  }
  return PickText(SmallIntPool());
}

// An INT expression whose value stays small, so no arithmetic can overflow;
// with risk_ == kDivision it may divide by an expression that can be zero.
std::string StatementGenerator::IntExprSmall(const Scope& scope, int depth) {
  if (depth <= 0 || Chance(30)) {
    return SmallInt(scope);
  }
  const std::size_t choice = Below(8);
  if (choice <= 2) {
    static const std::vector<std::string> operators = {" + ", " - ", " * "};
    return "(" + IntExprSmall(scope, depth - 1) + operators[choice] +
           IntExprSmall(scope, depth - 1) + ")";
  }
  if (choice == 3) {
    return "LENGTH(" + TextExpr(scope, 0) + ")";
  }
  if (choice == 4) {
    return "ABS(" + IntExprSmall(scope, depth - 1) + ")";
  }
  if (choice == 5) {
    return "CAST(" + DoubleExpr(scope, 0) + " AS INT)";
  }
  if (choice == 6 && risk_ == Risk::kDivision) {
    return "(" + IntExprSmall(scope, depth - 1) + " / (" + SmallInt(scope) +
           " - " + PickText(SmallIntPool()) + "))";
  }
  static const std::vector<std::string> divisors = {"2", "3", "4", "7"};
  return "(" + IntExprSmall(scope, depth - 1) + " / " + PickText(divisors) +
         ")";
}

// An INT expression. A timestamp column takes part in arithmetic only when
// risk_ == kOverflow, because a timestamp can sit at the 64-bit limit.
std::string StatementGenerator::IntExpr(const Scope& scope, int depth) {
  std::vector<std::string> timestamps;
  for (const Source& source : scope) {
    if (source.table != "reactions") {
      continue;
    }
    timestamps.push_back(source.alias + ".timestamp");
  }
  if (!timestamps.empty() && Chance(20)) {
    const std::string& timestamp = PickText(timestamps);
    if (risk_ == Risk::kOverflow && Chance(60)) {
      static const std::vector<std::string> operators = {" + ", " - ", " * "};
      return "(" + timestamp + PickText(operators) + PickText(SmallIntPool()) +
             ")";
    }
    return timestamp;
  }
  return IntExprSmall(scope, depth);
}

std::string StatementGenerator::DoubleExpr(const Scope& scope, int depth) {
  std::vector<std::string> columns;
  for (const Source& source : scope) {
    for (const std::string& name :
         ColumnNames(source.alias, source.table, DataType::kDouble)) {
      columns.push_back(name);
    }
  }
  if (depth <= 0 || Chance(30)) {
    if (!columns.empty() && Chance(70)) {
      return PickText(columns);
    }
    return PickText(DoublePool());
  }
  const std::size_t choice = Below(8);
  switch (choice) {
    case 0:
      return "ROUND(" + DoubleExpr(scope, depth - 1) + ", " +
             std::to_string(Below(4)) + ")";
    case 1:
      return "(" + DoubleExpr(scope, depth - 1) + " + " +
             DoubleExpr(scope, depth - 1) + ")";
    case 2:
      return "(" + DoubleExpr(scope, depth - 1) + " - " +
             DoubleExpr(scope, depth - 1) + ")";
    case 3:
      return "(" + DoubleExpr(scope, depth - 1) + " * " +
             PickText(DoublePool()) + ")";
    case 4:
      return "ABS(" + DoubleExpr(scope, depth - 1) + ")";
    case 5:
      return "CAST(" + IntExprSmall(scope, depth - 1) + " AS DOUBLE)";
    case 6:
      return "(" + DoubleExpr(scope, depth - 1) + " + " +
             IntExprSmall(scope, depth - 1) + ")";
    default:
      break;
  }
  if (risk_ == Risk::kDivision) {
    return "(" + DoubleExpr(scope, depth - 1) + " / " +
           DoubleExpr(scope, depth - 1) + ")";
  }
  return "(" + DoubleExpr(scope, depth - 1) + " / 2.0)";
}

std::string StatementGenerator::TextExpr(const Scope& scope, int depth) {
  std::vector<std::string> columns;
  for (const Source& source : scope) {
    for (const std::string& name :
         ColumnNames(source.alias, source.table, DataType::kText)) {
      columns.push_back(name);
    }
  }
  if (depth <= 0 || Chance(35)) {
    if (!columns.empty() && Chance(75)) {
      return PickText(columns);
    }
    return Quote(PickText(ContentPool()));
  }
  switch (Below(7)) {
    case 0:
      return "UPPER(" + TextExpr(scope, depth - 1) + ")";
    case 1:
      return "LOWER(" + TextExpr(scope, depth - 1) + ")";
    case 2:
      return "TRIM(" + TextExpr(scope, depth - 1) + ")";
    case 3:
      return "SUBSTR(" + TextExpr(scope, depth - 1) + ", " +
             std::to_string(Below(4)) + ", " + std::to_string(Below(6)) + ")";
    case 4:
      return "(" + TextExpr(scope, depth - 1) + " || " +
             TextExpr(scope, depth - 1) + ")";
    case 5:
      return "CAST(" + IntExprSmall(scope, depth - 1) + " AS TEXT)";
    default:
      return "CAST(" + DoubleExpr(scope, depth - 1) + " AS TEXT)";
  }
}

// An expression of any type, for a SELECT list.
std::string StatementGenerator::AnyExpr(const Scope& scope) {
  switch (Below(5)) {
    case 0:
      return IntExpr(scope, 2);
    case 1:
      return DoubleExpr(scope, 2);
    case 2:
      return IntExpr(scope, 1);
    default:
      return TextExpr(scope, 2);
  }
}

std::string StatementGenerator::Predicate(const Scope& scope, int depth) {
  if (depth > 0 && Chance(30)) {
    return "(" + Predicate(scope, depth - 1) + (Chance(50) ? " AND " : " OR ") +
           Predicate(scope, depth - 1) + ")";
  }
  switch (Below(7)) {
    case 0:
    case 1:
      return IntExpr(scope, 1) + " " + CompareOperator() + " " +
             IntExpr(scope, 1);
    case 2:
      return DoubleExpr(scope, 1) + " " + CompareOperator() + " " +
             DoubleExpr(scope, 1);
    case 3:
      return IntExpr(scope, 1) + " " + CompareOperator() + " " +
             DoubleExpr(scope, 1);
    case 4:
      return TextExpr(scope, 1) + " " + CompareOperator() + " " +
             TextExpr(scope, 1);
    case 5:
      return TextExpr(scope, 1) + " LIKE " + Quote(PickText(PatternPool()));
    default:
      return SmallInt(scope) + " " + CompareOperator() + " " +
             PickText(SmallIntPool());
  }
}

// ----------------------------------------------------------------------------
// SELECT.
// ----------------------------------------------------------------------------

namespace {

// The condition of a join between two sources of different or equal tables, as
// one equality that the hash join can use.
struct EdgeColumns {
  std::string left_column;
  std::string right_column;
};

// The columns two tables are joined on. The order of the two tables does not
// matter.
EdgeColumns EdgeBetween(const std::string& first, const std::string& second) {
  if (first == second) {
    if (first == "visitors") {
      return {"location", "location"};
    }
    if (first == "stories") {
      return {"content", "content"};
    }
    return {"storyid", "storyid"};
  }
  if (first == "visitors" && second == "stories") {
    return {"username", "username"};
  }
  if (first == "stories" && second == "visitors") {
    return {"username", "username"};
  }
  if (first == "stories" && second == "reactions") {
    return {"id", "storyid"};
  }
  if (first == "reactions" && second == "stories") {
    return {"storyid", "id"};
  }
  return {"username", "username"};  // visitors and reactions
}

// The tables in the order a join lists them.
std::vector<std::string> PermutationOfTables(std::mt19937_64* engine) {
  std::vector<std::string> names = {"visitors", "stories", "reactions"};
  for (std::size_t i = names.size() - 1; i > 0; --i) {
    std::swap(names[i], names[(*engine)() % (i + 1)]);
  }
  return names;
}

}  // namespace

std::string StatementGenerator::JoinCondition(const Scope& scope,
                                              std::size_t new_index,
                                              bool is_two_table_join) {
  const Source& right = scope[new_index];
  const Source& left = scope[Below(new_index)];
  std::string condition;
  const std::size_t form = Below(100);
  if (is_two_table_join && form < 12) {
    // No equality: a nested-loop join.
    static const std::vector<std::string> operators = {"<", ">", "<=", ">="};
    condition =
        left.alias + ".id " + PickText(operators) + " " + right.alias + ".id";
  } else if (form < 24 && left.table == "visitors" &&
             right.table == "stories") {
    // INT against DOUBLE, in both operand orders.
    condition = Chance(50) ? left.alias + ".rating = " + right.alias + ".views"
                           : right.alias + ".views = " + left.alias + ".rating";
  } else if (form < 24 && left.table == "stories" &&
             right.table == "visitors") {
    condition = Chance(50) ? left.alias + ".views = " + right.alias + ".rating"
                           : right.alias + ".rating = " + left.alias + ".views";
  } else {
    EdgeColumns columns = EdgeBetween(left.table, right.table);
    if (left.table == "reactions" && right.table == "reactions" && Chance(50)) {
      // The timestamps include two INT values that share one DOUBLE value.
      columns = {"timestamp", "timestamp"};
    }
    const std::string left_side = left.alias + "." + columns.left_column;
    const std::string right_side = right.alias + "." + columns.right_column;
    condition = Chance(50) ? left_side + " = " + right_side
                           : right_side + " = " + left_side;
  }
  if (Chance(25)) {
    // A residual condition; it never provokes an error, because a join may
    // evaluate it on a different set of row pairs in the two engines.
    const Risk saved_risk = risk_;
    risk_ = Risk::kNone;
    Scope visible(scope.begin(), scope.begin() + new_index + 1);
    condition += " AND " + Predicate(visible, 1);
    risk_ = saved_risk;
  }
  return condition;
}

// LIMIT 0 reads no row, so it hides an evaluation error below it, which the
// other engine reports (the early-termination row of 2.8). A statement that
// can fail therefore never uses it.
std::size_t StatementGenerator::LimitValue() {
  return risk_ == Risk::kNone ? Below(9) : 1 + Below(8);
}

std::string StatementGenerator::OrderAndLimit(
    const std::vector<std::size_t>& id_positions, std::size_t free_count) {
  if (!Chance(55)) {
    return "";
  }
  std::vector<std::string> keys;
  const std::size_t free_keys = free_count == 0 ? 0 : Below(3);
  for (std::size_t i = 0; i < free_keys; ++i) {
    keys.push_back(std::to_string(1 + Below(free_count)) +
                   (Chance(40) ? " DESC" : ""));
  }
  // Every table's id last, so the order is total.
  const bool is_total = id_positions.empty() ? false : Chance(75);
  if (is_total || keys.empty()) {
    for (std::size_t position : id_positions) {
      keys.push_back(std::to_string(position) + (Chance(30) ? " DESC" : ""));
    }
  }
  if (keys.empty()) {
    return "";
  }
  std::string text = " ORDER BY " + Join(keys, ", ");
  if (is_total && Chance(45)) {
    text += " LIMIT " + std::to_string(LimitValue());
    if (Chance(40)) {
      text += " OFFSET " + std::to_string(Below(7));
    }
  }
  return text;
}

// A SELECT over the scope: a list of expressions followed by the id of every
// table, an optional WHERE, and an optional ORDER BY and LIMIT.
std::string StatementGenerator::BuildSelect(const Scope& scope,
                                            const std::string& from_clause) {
  std::vector<std::string> items;
  std::vector<std::size_t> id_positions;
  std::size_t free_count = 0;
  if (Chance(10)) {
    items.push_back("*");
    std::size_t offset = 0;
    for (const Source& source : scope) {
      id_positions.push_back(offset + 1);
      offset += ColumnsOf(source.table).size();
    }
    free_count = offset;
  } else {
    const std::size_t count = 1 + Below(3);
    for (std::size_t i = 0; i < count; ++i) {
      items.push_back(AnyExpr(scope));
    }
    free_count = items.size();
    for (const Source& source : scope) {
      id_positions.push_back(items.size() + 1);
      items.push_back(source.alias + ".id");
    }
  }
  std::string text = "SELECT " + Join(items, ", ") + " FROM " + from_clause;
  if (Chance(60)) {
    text += " WHERE " + Predicate(scope, 2);
  }
  return text + OrderAndLimit(id_positions, free_count);
}

// The FROM clause of a join of `count` tables, and its scope.
std::string StatementGenerator::JoinFrom(std::size_t count, Scope* scope) {
  std::vector<std::string> tables;
  if (count == 3) {
    tables = PermutationOfTables(&engine_);
  } else {
    tables.push_back(PermutationOfTables(&engine_)[0]);
    tables.push_back(Chance(20) ? tables[0] : PermutationOfTables(&engine_)[1]);
  }
  for (std::size_t i = 0; i < tables.size(); ++i) {
    scope->push_back(Source{"t" + std::to_string(i + 1), tables[i]});
  }
  std::string from = tables[0] + " t1";
  for (std::size_t i = 1; i < tables.size(); ++i) {
    from += " JOIN " + tables[i] + " t" + std::to_string(i + 1) + " ON " +
            JoinCondition(*scope, i, count == 2);
  }
  return from;
}

std::string StatementGenerator::FilterSelect() {
  const std::string& table = Tables()[Below(Tables().size())].first;
  const Scope scope = {Source{"t1", table}};
  return BuildSelect(scope, table + " t1");
}

std::string StatementGenerator::JoinSelect() {
  Scope scope;
  const std::string from = JoinFrom(Chance(35) ? 3 : 2, &scope);
  return BuildSelect(scope, from);
}

// SELECT <group keys>, <aggregates> FROM ... [WHERE ...] [GROUP BY ...]
// [ORDER BY ... [LIMIT ...]].
std::string StatementGenerator::AggregateSelect() {
  Scope scope;
  std::string from;
  if (Chance(60)) {
    const std::string& table = Tables()[Below(Tables().size())].first;
    scope.push_back(Source{"t1", table});
    from = table + " t1";
  } else {
    from = JoinFrom(2, &scope);
  }

  // Group keys: text columns, the length of a text column, or small INT
  // columns.
  std::vector<std::string> key_candidates;
  for (const Source& source : scope) {
    for (const std::string& name :
         ColumnNames(source.alias, source.table, DataType::kText)) {
      key_candidates.push_back(name);
      key_candidates.push_back("LENGTH(" + name + ")");
    }
    if (source.table == "stories") {
      key_candidates.push_back(source.alias + ".views");
    }
    if (source.table == "reactions") {
      key_candidates.push_back(source.alias + ".storyid");
    }
  }
  std::vector<std::string> keys;
  const std::size_t key_count = Chance(30) ? 0 : (Chance(70) ? 1 : 2);
  for (std::size_t i = 0; i < key_count; ++i) {
    const std::string& key = PickText(key_candidates);
    if (std::ranges::find(keys, key) == keys.end()) {
      keys.push_back(key);
    }
  }
  const bool is_grouped = !keys.empty();

  // Aggregates. Without GROUP BY the input can be empty, and then AVG, MIN and
  // MAX each fail with their own message, so at most one of them is used.
  std::vector<std::string> aggregates;
  const std::size_t aggregate_count = 1 + Below(3);
  bool has_fragile_aggregate = false;
  for (std::size_t i = 0; i < aggregate_count; ++i) {
    const std::size_t form = Below(6);
    if (form == 0) {
      aggregates.push_back("COUNT(*)");
    } else if (form == 1) {
      aggregates.push_back("COUNT(" + AnyExpr(scope) + ")");
    } else if (form == 2) {
      aggregates.push_back("SUM(" + IntExprSmall(scope, 1) + ")");
    } else if (is_grouped || !has_fragile_aggregate) {
      has_fragile_aggregate = true;
      if (form == 3) {
        aggregates.push_back("AVG(" + IntExprSmall(scope, 1) + ")");
      } else {
        aggregates.push_back(std::string(form == 4 ? "MIN(" : "MAX(") +
                             AnyExpr(scope) + ")");
      }
    } else {
      aggregates.push_back("COUNT(*)");
    }
  }

  std::vector<std::string> items = keys;
  items.insert(items.end(), aggregates.begin(), aggregates.end());
  std::string text = "SELECT " + Join(items, ", ") + " FROM " + from;
  if (Chance(55)) {
    text += " WHERE " + Predicate(scope, 2);
  }
  if (!is_grouped) {
    return text;
  }
  text += " GROUP BY " + Join(keys, ", ");
  if (!Chance(65)) {
    return text;
  }

  // Ordered by the group keys, which are unique per row, so a LIMIT is safe.
  std::vector<std::string> order;
  if (Chance(50)) {
    order.push_back(std::to_string(keys.size() + 1 + Below(aggregates.size())) +
                    (Chance(50) ? " DESC" : ""));
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    order.push_back(std::to_string(i + 1) + (Chance(30) ? " DESC" : ""));
  }
  text += " ORDER BY " + Join(order, ", ");
  if (Chance(40)) {
    text += " LIMIT " + std::to_string(LimitValue());
    if (Chance(40)) {
      text += " OFFSET " + std::to_string(Below(5));
    }
  }
  return text;
}

// A conversion of TEXT that fails on one row: the message names the text, so
// the statement selects a single row.
std::string StatementGenerator::SingleRowCastSelect() {
  const std::int64_t id = ExistingOrNewId("stories");
  const std::string target = Chance(50) ? "INT" : "DOUBLE";
  return "SELECT id, CAST(content AS " + target +
         ") FROM stories WHERE id = " + std::to_string(id);
}

std::string StatementGenerator::Select() {
  const std::size_t form = Below(100);
  if (form < 30) {
    return FilterSelect();
  }
  if (form < 60) {
    return JoinSelect();
  }
  if (form < 92) {
    return AggregateSelect();
  }
  return SingleRowCastSelect();
}

// ----------------------------------------------------------------------------
// INSERT.
// ----------------------------------------------------------------------------

// A username for a new row: from the pool (which collides with existing rows)
// or a name never used before.
std::string StatementGenerator::NewUserName() {
  if (Chance(45)) {
    return Quote(PickText(NamePool()));
  }
  return Quote("n" + std::to_string(next_name_number_++));
}

// An id above every id of the table, which collides with none of them.
std::int64_t StatementGenerator::NextFreeId(const std::string& table,
                                            std::size_t row_offset) {
  std::int64_t largest = 0;
  auto found = tables_->find(table);
  if (found != tables_->end()) {
    for (const Row& row : found->second) {
      largest = std::max(largest, std::get<std::int64_t>(row[0]));
    }
  }
  return largest + 1 + static_cast<std::int64_t>(row_offset);
}

// The rows of a multi-row INSERT: usually 1, sometimes 2 or 3.
std::size_t StatementGenerator::RowCount() {
  return Chance(25) ? 2 + Below(2) : 1;
}

std::string StatementGenerator::InsertVisitors() {
  const bool has_id = Chance(50);
  const bool has_rating = !has_id && Chance(50);
  std::vector<std::string> rows;
  const std::size_t row_count = RowCount();
  for (std::size_t i = 0; i < row_count; ++i) {
    std::vector<std::string> cells;
    if (has_id) {
      cells.push_back(std::to_string(Chance(12) ? ExistingOrNewId("visitors")
                                                : NextFreeId("visitors", i)));
    }
    cells.push_back(NewUserName());
    cells.push_back(Quote(PickText(LocationPool())));
    if (has_id || has_rating) {
      cells.push_back(PickText(DoublePool()));
    }
    rows.push_back("(" + Join(cells, ", ") + ")");
  }
  std::string columns;
  if (!has_id) {
    columns =
        has_rating ? " (username, location, rating)" : " (username, location)";
  }
  return "INSERT INTO visitors" + columns + " VALUES " + Join(rows, ", ");
}

std::string StatementGenerator::InsertStories() {
  const std::size_t form = Below(100);
  const bool has_id = form >= 70;
  const bool has_views = form >= 50;
  std::vector<std::string> rows;
  const std::size_t row_count = RowCount();
  for (std::size_t i = 0; i < row_count; ++i) {
    std::vector<std::string> cells;
    if (has_id) {
      cells.push_back(std::to_string(Chance(12) ? ExistingOrNewId("stories")
                                                : NextFreeId("stories", i)));
    }
    std::string content = Quote(PickText(ContentPool()));
    if (Chance(6)) {
      // A long text, so that pages fill up and updates relocate rows.
      std::string words;
      const std::size_t length = 200 + Below(800);
      while (words.size() < length) {
        words += "word ";
      }
      content = Quote(words);
    }
    cells.push_back(content);
    cells.push_back(Chance(88) ? ExistingLiteral("visitors", 1, "'ann'")
                               : Quote(PickText(NamePool())));
    if (has_views) {
      cells.push_back(PickText(SmallIntPool()));
    }
    rows.push_back("(" + Join(cells, ", ") + ")");
  }
  std::string columns;
  if (!has_id) {
    columns =
        has_views ? " (content, username, views)" : " (content, username)";
  }
  return "INSERT INTO stories" + columns + " VALUES " + Join(rows, ", ");
}

std::string StatementGenerator::InsertReactions() {
  std::vector<std::string> rows;
  const std::size_t row_count = RowCount();
  for (std::size_t i = 0; i < row_count; ++i) {
    std::vector<std::string> cells;
    cells.push_back(std::to_string(Chance(12) ? ExistingOrNewId("reactions")
                                              : NextFreeId("reactions", i)));
    cells.push_back(Chance(88) ? ExistingLiteral("stories", 0, "1")
                               : std::to_string(1 + Below(40)));
    cells.push_back(Chance(88) ? ExistingLiteral("visitors", 1, "'ann'")
                               : Quote(PickText(NamePool())));
    cells.push_back(Quote(PickText(TypePool())));
    cells.push_back(Chance(20) ? PickText(SmallIntPool())
                               : PickText(DoublePool()));
    cells.push_back(Chance(6) ? TimestampPool().back()
                              : PickText(TimestampPool()));
    rows.push_back("(" + Join(cells, ", ") + ")");
  }
  return "INSERT INTO reactions VALUES " + Join(rows, ", ");
}

std::string StatementGenerator::Insert() {
  const std::size_t form = Below(100);
  if (form < 30) {
    return InsertVisitors();
  }
  if (form < 65) {
    return InsertStories();
  }
  return InsertReactions();
}

// ----------------------------------------------------------------------------
// UPDATE.
// ----------------------------------------------------------------------------

// UPDATE of columns that no constraint covers, over any set of rows. Growth
// is bounded by a guard in the WHERE clause, so that no row nears the 4060
// bytes of the paged engine.
std::string StatementGenerator::UpdatePlain() {
  const std::size_t form = Below(100);
  std::string table;
  std::vector<std::string> assignments;
  std::string guard;
  if (form < 30) {
    table = "visitors";
    const Scope scope = {Source{"visitors", "visitors"}};
    // None of these yields a zero of either sign from a non-zero cell (see
    // DoublePool): no ROUND of a small negative number, no division of zero.
    static const std::vector<std::string> ratings = {
        "(rating + 0.25)", "(rating * 0.5)",     "(rating - 1)",
        "ABS(rating)",     "CAST(id AS DOUBLE)", "rating"};
    if (risk_ == Risk::kDivision && Chance(50)) {
      assignments.push_back("rating = (1.0 / (rating - " +
                            PickText(DoublePool()) + "))");
    } else {
      assignments.push_back("rating = " + (Chance(30) ? PickText(DoublePool())
                                                      : PickText(ratings)));
    }
    if (Chance(40)) {
      static const std::vector<std::string> locations = {
          "UPPER(location)",        "LOWER(location)",   "TRIM(location)",
          "SUBSTR(location, 1, 5)", "(location || 'x')", "username"};
      const std::string& expression = PickText(locations);
      if (expression == "(location || 'x')") {
        guard = "LENGTH(location) < 30";
      }
      assignments.push_back(
          "location = " +
          (Chance(30) ? Quote(PickText(LocationPool())) : expression));
    }
    return UpdateText(table, assignments, guard, scope);
  }
  if (form < 70) {
    table = "stories";
    const Scope scope = {Source{"stories", "stories"}};
    static const std::vector<std::string> views = {
        "(views + 1)", "(views - 3)", "(views / 2)", "LENGTH(content)",
        "id",          "ABS(views)",  "views"};
    if (risk_ == Risk::kDivision && Chance(50)) {
      assignments.push_back("views = (60 / (views - " +
                            PickText(SmallIntPool()) + "))");
    } else if (Chance(15)) {
      assignments.push_back("views = (views * 2)");
      guard = "views < 1000 AND views > -1000";
    } else {
      assignments.push_back("views = " + (Chance(25) ? PickText(SmallIntPool())
                                                     : PickText(views)));
    }
    if (Chance(45)) {
      static const std::vector<std::string> contents = {
          "(content || 'x')",  "(content || content)", "UPPER(content)",
          "LOWER(content)",    "TRIM(content)",        "SUBSTR(content, 1, 20)",
          "SUBSTR(content, 2)"};
      const std::string& expression = PickText(contents);
      if (Chance(25)) {
        assignments.push_back("content = " + Quote(PickText(ContentPool())));
      } else {
        assignments.push_back("content = " + expression);
        if (expression == "(content || 'x')") {
          guard = "LENGTH(content) < 600";
        } else if (expression == "(content || content)") {
          guard = "LENGTH(content) < 300";
        }
      }
    }
    return UpdateText(table, assignments, guard, scope);
  }
  table = "reactions";
  const Scope scope = {Source{"reactions", "reactions"}};
  assignments.push_back("type = " + (Chance(50) ? Quote(PickText(TypePool()))
                                                : std::string("UPPER(type)")));
  if (Chance(50)) {
    static const std::vector<std::string> weights = {
        "(weight * 0.5)", "(weight + 0.5)", "ABS(weight)", "weight"};
    assignments.push_back("weight = " + (Chance(30) ? PickText(DoublePool())
                                                    : PickText(weights)));
  }
  if (Chance(40)) {
    if (risk_ == Risk::kOverflow) {
      assignments.push_back("timestamp = (timestamp + " +
                            PickText(SmallIntPool()) + ")");
    } else {
      assignments.push_back("timestamp = " + PickText(TimestampPool()));
    }
  }
  return UpdateText(table, assignments, guard, scope);
}

// UPDATE <table> SET <assignments> [WHERE <guard> AND (<predicate>)].
std::string StatementGenerator::UpdateText(
    const std::string& table, const std::vector<std::string>& assignments,
    const std::string& guard, const Scope& scope) {
  std::string text = "UPDATE " + table + " SET " + Join(assignments, ", ");
  std::vector<std::string> conditions;
  if (!guard.empty()) {
    conditions.push_back(guard);
  }
  if (Chance(65)) {
    conditions.push_back("(" + Predicate(scope, 2) + ")");
  }
  if (!conditions.empty()) {
    text += " WHERE " + Join(conditions, " AND ");
  }
  return text;
}

// UPDATE of a UNIQUE or foreign key column of one row, chosen by its primary
// key or its unique key, so that the row that violates a constraint does not
// depend on the scan order.
std::string StatementGenerator::UpdateKey() {
  const std::size_t form = Below(100);
  if (form < 40) {
    std::string value;
    const std::size_t source = Below(100);
    if (source < 35) {
      value = NewUserName();
    } else if (source < 55) {
      value = ExistingLiteral("visitors", 1, "'bob'");
    } else if (source < 80) {
      value = "(username || 'x')";
    } else {
      value = "SUBSTR(username, 1, 3)";
    }
    if (Chance(80)) {
      return "UPDATE visitors SET username = " + value +
             " WHERE id = " + std::to_string(ExistingOrNewId("visitors"));
    }
    return "UPDATE visitors SET username = " + value +
           " WHERE username = " + ExistingLiteral("visitors", 1, "'bob'");
  }
  if (form < 65) {
    return "UPDATE stories SET username = " +
           (Chance(85) ? ExistingLiteral("visitors", 1, "'ann'")
                       : Quote(PickText(NamePool()))) +
           " WHERE id = " + std::to_string(ExistingOrNewId("stories"));
  }
  if (form < 85) {
    return "UPDATE reactions SET storyid = " +
           (Chance(85) ? ExistingLiteral("stories", 0, "1")
                       : std::to_string(1 + Below(40))) +
           " WHERE id = " + std::to_string(ExistingOrNewId("reactions"));
  }
  return "UPDATE reactions SET username = " +
         (Chance(85) ? ExistingLiteral("visitors", 1, "'ann'")
                     : Quote(PickText(NamePool()))) +
         " WHERE id = " + std::to_string(ExistingOrNewId("reactions"));
}

// A rename that shifts a chain of usernames (A to Ax, Ax to Axx) in one
// statement. The rows that reference A must end up referencing Ax, not Axx.
// Falls back to UpdateKey when the table holds no pair A and Ax. The selected
// rows are exactly the chain, so at most one row (the end of the chain, when
// the next name is taken) can violate uniqueness.
std::string StatementGenerator::UpdateChainRename() {
  auto found = tables_->find("visitors");
  if (found == tables_->end()) {
    return UpdateKey();
  }
  std::vector<std::pair<std::int64_t, std::string>> visitors;
  for (const Row& row : found->second) {
    visitors.emplace_back(std::get<std::int64_t>(row[0]),
                          std::get<std::string>(row[1]));
  }
  std::vector<std::vector<std::int64_t>> chains;
  for (const auto& start : visitors) {
    std::vector<std::int64_t> chain = {start.first};
    std::string name = start.second;
    bool is_extended = true;
    while (is_extended) {
      is_extended = false;
      for (const auto& visitor : visitors) {
        if (visitor.second == name + "x") {
          chain.push_back(visitor.first);
          name = visitor.second;
          is_extended = true;
          break;
        }
      }
    }
    if (chain.size() >= 2) {
      chains.push_back(chain);
    }
  }
  if (chains.empty()) {
    return UpdateKey();
  }
  const std::vector<std::int64_t>& chain = chains[Below(chains.size())];
  std::vector<std::string> conditions;
  for (std::int64_t id : chain) {
    conditions.push_back("id = " + std::to_string(id));
  }
  return "UPDATE visitors SET username = (username || 'x') WHERE " +
         Join(conditions, " OR ");
}

// Two TEXT columns of one row exchanged: for visitors the username changes (a
// cascade), for stories a username that is unlikely to exist.
std::string StatementGenerator::UpdateSwapColumns() {
  if (Chance(60)) {
    return "UPDATE visitors SET location = username, username = location WHERE "
           "id = " +
           std::to_string(ExistingOrNewId("visitors"));
  }
  return "UPDATE stories SET content = username, username = content WHERE id "
         "= " +
         std::to_string(ExistingOrNewId("stories"));
}

std::string StatementGenerator::Update() {
  const std::size_t form = Below(100);
  if (form < 55) {
    return UpdatePlain();
  }
  if (form < 78) {
    return UpdateKey();
  }
  if (form < 86) {
    return UpdateChainRename();
  }
  if (form < 92) {
    return UpdateSwapColumns();
  }
  if (form < 96) {
    return "UPDATE visitors SET rating = (rating * 1.0)";
  }
  return "UPDATE stories SET views = views WHERE " +
         Predicate({Source{"stories", "stories"}}, 1);
}

// ----------------------------------------------------------------------------
// DELETE.
// ----------------------------------------------------------------------------

// A DELETE that may reject a row of a referenced table (RESTRICT) selects one
// row. Reactions are referenced by nobody, so any predicate is safe there.
std::string StatementGenerator::Delete() {
  const std::size_t form = Below(100);
  auto reactions = tables_->find("reactions");
  const bool has_reactions =
      reactions != tables_->end() && reactions->second.size() > 4;
  if (form < 30 && has_reactions) {
    const Scope scope = {Source{"reactions", "reactions"}};
    if (Chance(4)) {
      return "DELETE FROM reactions";
    }
    return "DELETE FROM reactions WHERE " + Predicate(scope, 2);
  }
  if (form < 55) {
    return "DELETE FROM reactions WHERE id = " +
           std::to_string(ExistingOrNewId("reactions"));
  }
  if (form < 77) {
    return "DELETE FROM stories WHERE id = " +
           std::to_string(ExistingOrNewId("stories"));
  }
  if (form < 95) {
    return "DELETE FROM visitors WHERE id = " +
           std::to_string(ExistingOrNewId("visitors"));
  }
  return "DELETE FROM visitors WHERE username = " +
         ExistingLiteral("visitors", 1, "'bob'");
}

// ----------------------------------------------------------------------------
// Key statements.
// ----------------------------------------------------------------------------

namespace {

// The columns of a table with an INT primary key `id` that a key statement
// reads and writes. None is a key, a UNIQUE column or a foreign key.
struct NonKeyColumns {
  const char* selected;        // read next to id
  const char* conjuncts[2];    // predicates over non-key columns
  const char* numeric_update;  // an assignment that adds to a column
  const char* text_column;     // a TEXT column that an UPDATE rewrites
};

NonKeyColumns NonKeyColumnsOf(const std::string& table) {
  if (table == "visitors") {
    return {"location",
            {"location = 'Paris'", "rating >= 3.0"},
            "rating = rating + 1.0",
            "location"};
  }
  if (table == "stories") {
    return {"views",
            {"views > 3", "content = 'hello world'"},
            "views = views + 1",
            "content"};
  }
  return {"type",
          {"type = 'like'", "weight < 2.0"},
          "timestamp = timestamp + 1",
          "type"};
}

}  // namespace

// A literal to compare the key with: a key that exists, one beside it, zero, a
// negative number or the largest INT.
std::string StatementGenerator::KeyLiteral(const std::string& table) {
  switch (Below(9)) {
    case 0:
      return "0";
    case 1:
      return "-1";
    case 2:
      return "9223372036854775807";
    case 3:
      return std::to_string(ExistingOrNewId(table) + 1);
    case 4:
      return std::to_string(ExistingOrNewId(table) - 1);
    default:
      return std::to_string(ExistingOrNewId(table));
  }
}

// Any predicate over the key: it may select every row, one row or none.
std::string StatementGenerator::KeyPredicate(const std::string& table) {
  static const std::vector<std::string> double_literals = {"2.5", "3.0",
                                                           "-1.5"};
  const std::string low = KeyLiteral(table);
  const std::string high = KeyLiteral(table);
  switch (Below(12)) {
    case 0:
      return "id = " + low;
    case 1:
      return low + " = id";
    case 2:
      return "id < " + low;
    case 3:
      return low + " < id";
    case 4:
      return "id <= " + low;
    case 5:
      return "id >= " + low;
    case 6:
      return "id >= " + low + " AND id <= " + high;
    case 7:
      return "id > " + low + " AND id < " + high;
    case 8:
      return "id <> " + low;
    case 9:
      return "id = " + low + " OR id = " + high;
    case 10:
      return "id = " + PickText(double_literals);
    default:
      return "id >= " + low + " AND " +
             NonKeyColumnsOf(table).conjuncts[Below(2)];
  }
}

// A predicate that selects at most a few rows.
std::string StatementGenerator::NarrowKeyPredicate(const std::string& table) {
  const std::string literal = KeyLiteral(table);
  switch (Below(5)) {
    case 0:
      return "id = " + literal;
    case 1:
      return literal + " = id";
    case 2: {
      const std::int64_t first = ExistingOrNewId(table);
      return "id >= " + std::to_string(first) +
             " AND id <= " + std::to_string(first + 2);
    }
    case 3:
      return "id = " + literal + " OR id = " + KeyLiteral(table);
    default:
      return "id = " + literal + " AND " +
             NonKeyColumnsOf(table).conjuncts[Below(2)];
  }
}

// A predicate that selects at most one row: an equality on the key.
std::string StatementGenerator::SingleKeyPredicate(const std::string& table) {
  const std::string literal = KeyLiteral(table);
  switch (Below(3)) {
    case 0:
      return "id = " + literal;
    case 1:
      return literal + " = id";
    default:
      return "id = " + literal + " AND " +
             NonKeyColumnsOf(table).conjuncts[Below(2)];
  }
}

std::string StatementGenerator::KeySelect(const std::string& table) {
  if (Chance(30)) {
    // The order ends with the primary key, so the cut is deterministic.
    return std::string("SELECT id, ") + NonKeyColumnsOf(table).selected +
           " FROM " + table + " WHERE " + KeyPredicate(table) +
           " ORDER BY id LIMIT " + std::to_string(1 + Below(4));
  }
  return "SELECT * FROM " + table + " WHERE " + KeyPredicate(table);
}

// Rewrites a column that no key involves; a long text makes a row grow and
// move to another page.
std::string StatementGenerator::KeyUpdate(const std::string& table) {
  static const std::vector<std::string> texts = {
      "''", "'x'", "'Paris'", "'" + std::string(300, 'k') + "'"};
  const NonKeyColumns columns = NonKeyColumnsOf(table);
  const std::string assignment =
      Chance(50) ? std::string(columns.numeric_update)
                 : std::string(columns.text_column) + " = " + PickText(texts);
  return "UPDATE " + table + " SET " + assignment + " WHERE " +
         KeyPredicate(table);
}

// reactions is referenced by nobody, so a DELETE of a few rows cannot fail;
// the other tables get a DELETE of at most one row, whose RESTRICT error does
// not depend on the scan order.
std::string StatementGenerator::KeyDelete(const std::string& table) {
  if (table == "reactions") {
    return "DELETE FROM reactions WHERE " + NarrowKeyPredicate(table);
  }
  return "DELETE FROM " + table + " WHERE " + SingleKeyPredicate(table);
}

std::string StatementGenerator::KeyStatement() {
  static const std::vector<std::string> tables = {"visitors", "stories",
                                                  "reactions"};
  const std::string& table = PickText(tables);
  const std::size_t form = Below(100);
  if (form < 50) {
    return KeySelect(table);
  }
  if (form < 80) {
    return KeyUpdate(table);
  }
  return KeyDelete(table);
}

// ----------------------------------------------------------------------------
// The next statement.
// ----------------------------------------------------------------------------

std::string StatementGenerator::Next(const TableContents& tables) {
  tables_ = &tables;
  risk_ = Risk::kNone;
  if (key_percent_ > 0 && Chance(key_percent_)) {
    return KeyStatement();
  }
  if (Chance(25)) {
    risk_ = Chance(50) ? Risk::kOverflow : Risk::kDivision;
  }
  const std::size_t form = Below(100);
  if (form < 48) {
    return Select();
  }
  if (form < 72) {
    risk_ = Risk::kNone;
    return Insert();
  }
  if (form < 88) {
    return Update();
  }
  risk_ = Risk::kNone;
  return Delete();
}

}  // namespace strata::testing
