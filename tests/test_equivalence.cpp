#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/domain/value.h"
#include "strata/paged/database.h"
#include "strata/paged/paged_engine.h"
#include "strata/repl.h"
#include "test_equivalence_generator.h"
#include "test_equivalence_support.h"
#include "test_harness.h"
#include "test_support.h"

// The flat-file engine and the paged engine run the same statements, and the
// results are compared under design document 2.8 (ADR-0044); see
// docs/TestDocument.md.

namespace strata::testing {

namespace {

namespace cli = strata::cli;

using application::QueryService;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExecutionResult;
using domain::Row;
using domain::Value;
using paged::ReplacementPolicyKind;

// ----------------------------------------------------------------------------
// Two engines on fresh storage.
// ----------------------------------------------------------------------------

// A flat-file engine over a temporary directory and a paged engine over a
// MemoryPageStore. The directory outlives both services.
struct EnginePair {
  TempDir directory;
  std::unique_ptr<QueryService> flatfile;
  PagedFixture paged;
};

// Reports a failure and returns false when either engine cannot be opened.
bool OpenPair(EnginePair* pair, std::size_t frames,
              ReplacementPolicyKind policy) {
  pair->flatfile = MakeLoadedService(pair->directory.path());
  pair->paged = MakeMemoryPagedFixture(frames, policy);
  return pair->flatfile != nullptr && pair->paged.service != nullptr;
}

const char* PolicyName(ReplacementPolicyKind policy) {
  return policy == ReplacementPolicyKind::kLru ? "LRU" : "2Q";
}

// ----------------------------------------------------------------------------
// What a corpus exercised, so that a corpus that stops reaching a rule fails.
// ----------------------------------------------------------------------------

struct Outcomes {
  std::size_t selects_with_rows = 0;
  std::size_t selects_without_rows = 0;
  std::size_t ordered_limit_selects = 0;
  std::size_t writes_applied = 0;
  std::size_t writes_matching_no_row = 0;
  std::size_t bind_errors = 0;
  std::size_t runtime_errors = 0;
  std::size_t duplicate_keys = 0;
  std::size_t missing_parents = 0;
  std::size_t restricted_deletes = 0;
  std::size_t unsupported_errors = 0;
  std::size_t other_errors = 0;
};

bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

void CountErrorOutcome(const ExecutionError& error, Outcomes* outcomes) {
  switch (error.kind) {
    case ErrorKind::kBind:
      ++outcomes->bind_errors;
      return;
    case ErrorKind::kRuntime:
      ++outcomes->runtime_errors;
      return;
    case ErrorKind::kUnsupported:
      ++outcomes->unsupported_errors;
      return;
    case ErrorKind::kConstraintViolation:
      if (error.message.starts_with("duplicate key")) {
        ++outcomes->duplicate_keys;
      } else if (error.message.starts_with("no matching row")) {
        ++outcomes->missing_parents;
      } else if (Contains(error.message, "is referenced by")) {
        ++outcomes->restricted_deletes;
      } else {
        ++outcomes->other_errors;
      }
      return;
    case ErrorKind::kIo:
      ++outcomes->other_errors;
      return;
  }
}

void CountOutcome(const std::string& sql, const ExecutionResult& result,
                  Outcomes* outcomes) {
  if (const auto* error = std::get_if<ExecutionError>(&result)) {
    CountErrorOutcome(*error, outcomes);
    return;
  }
  if (const auto* rows = std::get_if<domain::ResultSet>(&result)) {
    if (rows->rows.empty()) {
      ++outcomes->selects_without_rows;
    } else {
      ++outcomes->selects_with_rows;
    }
    if (Contains(sql, "ORDER BY") && Contains(sql, "LIMIT")) {
      ++outcomes->ordered_limit_selects;
    }
    return;
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&result)) {
    if (affected->count == 0) {
      ++outcomes->writes_matching_no_row;
    } else {
      ++outcomes->writes_applied;
    }
  }
}

// ----------------------------------------------------------------------------
// The comparator itself.
// ----------------------------------------------------------------------------

domain::ResultSet TwoColumnResult(const std::vector<Row>& rows) {
  domain::ResultSet result;
  result.columns = {domain::ColumnDef{"key", domain::DataType::kInt},
                    domain::ColumnDef{"label", domain::DataType::kText}};
  result.rows = rows;
  return result;
}

Row KeyedRow(std::int64_t key, const std::string& label) {
  return Row{Value(key), Value(label)};
}

// A comparator that reports nothing would make every equivalence test pass, so
// each rule of 2.8 is shown to be detected, and each allowed difference shown
// to be accepted.
void CheckComparatorFollowsTheRules() {
  const std::vector<std::size_t> unordered;
  const std::vector<std::size_t> ordered = {0};
  const ExecutionResult base =
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "b"), KeyedRow(2, "c")});

  // Allowed: another row order without ORDER BY; another order among rows with
  // equal sort keys.
  CHECK(!FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(2, "c"), KeyedRow(1, "b"), KeyedRow(1, "a")}),
      unordered));
  CHECK(!FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "b"), KeyedRow(1, "a"), KeyedRow(2, "c")}),
      ordered));

  // Detected: the order of the sort keys, a changed or missing row, a
  // changed multiplicity, the type of a cell.
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(2, "c"), KeyedRow(1, "b")}),
      ordered));
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "x"), KeyedRow(2, "c")}),
      ordered));
  CHECK(FindResultDifference(
      base, TwoColumnResult({KeyedRow(1, "a"), KeyedRow(2, "c")}), unordered));
  CHECK(FindResultDifference(
      base,
      TwoColumnResult({KeyedRow(1, "a"), KeyedRow(1, "a"), KeyedRow(2, "c")}),
      unordered));
  domain::ResultSet other_type = TwoColumnResult({KeyedRow(1, "a")});
  other_type.columns[0].type = domain::DataType::kDouble;
  CHECK(FindResultDifference(TwoColumnResult({KeyedRow(1, "a")}), other_type,
                             unordered));
  domain::ResultSet other_name = TwoColumnResult({KeyedRow(1, "a")});
  other_name.columns[1].name = "other";
  CHECK(FindResultDifference(TwoColumnResult({KeyedRow(1, "a")}), other_name,
                             unordered));
  CHECK(FindResultDifference(
      ExecutionResult(
          domain::ResultSet{{domain::ColumnDef{"v", domain::DataType::kDouble}},
                            {Row{Value(2.0)}}}),
      ExecutionResult(
          domain::ResultSet{{domain::ColumnDef{"v", domain::DataType::kDouble}},
                            {Row{Value(std::int64_t{2})}}}),
      unordered));

  // The other result kinds.
  CHECK(!FindResultDifference(domain::AffectedRows{3}, domain::AffectedRows{3},
                              unordered));
  CHECK(FindResultDifference(domain::AffectedRows{3}, domain::AffectedRows{4},
                             unordered));
  CHECK(FindResultDifference(domain::CommandComplete{"CREATE TABLE a"},
                             domain::CommandComplete{"CREATE TABLE b"},
                             unordered));
  CHECK(FindResultDifference(domain::AffectedRows{1},
                             domain::CommandComplete{"CREATE TABLE a"},
                             unordered));
  const ExecutionError error = {ErrorKind::kRuntime, "division by zero"};
  CHECK(!FindResultDifference(error, error, unordered));
  CHECK(FindResultDifference(
      error, ExecutionError{ErrorKind::kBind, "division by zero"}, unordered));
  CHECK(FindResultDifference(
      error, ExecutionError{ErrorKind::kRuntime, "numeric overflow"},
      unordered));
}

// The runner also compares the content of every table, and checks that a
// rejected statement changed none. Each case changes one engine behind the
// runner's back and shows that the next statement reports it.
void CheckRunnerComparesTables() {
  const std::string create = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)";
  {
    // An INSERT that both engines accept leaves the tables different.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(AsAffectedRows(ExecuteSql(
              *pair.flatfile, "INSERT INTO t VALUES (1, 'x')")) != nullptr);
    const std::optional<std::string> difference =
        runner.Run("INSERT INTO t VALUES (2, 'y')");
    CHECK(difference.has_value() && Contains(*difference, "table t"));
  }
  for (bool is_flatfile : {true, false}) {
    // A rejected statement after one engine changed: the change is reported as
    // made by the rejected statement.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(!runner.Run("INSERT INTO t VALUES (1, 'x')"));
    QueryService* changed =
        is_flatfile ? pair.flatfile.get() : pair.paged.service.get();
    CHECK(AsAffectedRows(ExecuteSql(
              *changed, "INSERT INTO t VALUES (3, 'z')")) != nullptr);
    const std::optional<std::string> difference =
        runner.Run("INSERT INTO t VALUES (1, 'duplicate')");
    CHECK(difference.has_value() &&
          Contains(*difference, "a rejected statement changed table t"));
  }
  {
    // A statement that does not parse, and a LIMIT without ORDER BY, are
    // reported as outside the comparable set rather than compared.
    EnginePair pair;
    if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
      return;
    }
    EquivalenceRunner runner(*pair.flatfile, *pair.paged.service, "self-check");
    CHECK(!runner.Run(create));
    CHECK(!runner.Run("INSERT INTO t VALUES (1, 'x'), (2, 'x'), (3, 'y')"));
    CHECK(runner.Run("SELEKT 1").has_value());
    CHECK(runner.Run("SELECT id FROM t LIMIT 1").has_value());
    // Sorted by a key that ties across the cut.
    CHECK(
        runner.Run("SELECT v FROM t ORDER BY v LIMIT 1 OFFSET 1").has_value());
    CHECK(!runner.Run("SELECT v, id FROM t ORDER BY v, id LIMIT 1 OFFSET 1"));
  }
}

// ----------------------------------------------------------------------------
// The fixed corpus.
// ----------------------------------------------------------------------------

// Reads: filters, expressions, functions, LIKE, errors from one source.
const std::vector<std::string>& ReadCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT * FROM visitors",
      "SELECT username, location FROM visitors WHERE location = 'Paris'",
      "SELECT id, username FROM visitors WHERE rating >= 3.25 AND location <> "
      "'Oslo'",
      "SELECT id FROM stories WHERE views > 3 OR content = ''",
      "SELECT id, views * 2 + 1, views / 2, views - 10, -views FROM stories "
      "WHERE id <= 5",
      "SELECT id, content || '!' FROM stories WHERE content LIKE '%world%'",
      "SELECT id FROM stories WHERE content LIKE 'h_llo%'",
      "SELECT UPPER(username), LOWER(content), LENGTH(content), "
      "SUBSTR(content, 2, 3), TRIM(content) FROM stories",
      "SELECT ABS(rating - 5), ROUND(rating, 1), CAST(rating AS INT), "
      "CAST(id AS TEXT), CAST(rating AS TEXT) FROM visitors",
      "SELECT id, CAST(content AS INT) FROM stories WHERE id = 1",
      "SELECT id, 1 / (views - 7) FROM stories WHERE id < 6",
      "SELECT id, 10 / views FROM stories WHERE views > 0",
      "SELECT id FROM stories WHERE username = 'nobody'",
      "SELECT id FROM nosuch",
      "SELECT nosuch FROM visitors",
      "SELECT id FROM visitors WHERE username > 5",
  };
  return corpus;
}

// ORDER BY with ties, LIMIT and OFFSET at the boundaries, on unique keys.
const std::vector<std::string>& OrderCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT id, username FROM visitors ORDER BY username DESC",
      "SELECT id, location FROM visitors ORDER BY location, id DESC",
      "SELECT id, views FROM stories ORDER BY views DESC, id LIMIT 4",
      "SELECT id FROM stories ORDER BY id LIMIT 3 OFFSET 2",
      "SELECT id FROM stories ORDER BY id LIMIT 0",
      "SELECT id FROM stories ORDER BY id LIMIT 5 OFFSET 20",
      "SELECT id, rating FROM visitors ORDER BY rating, id",
      "SELECT id, views FROM stories ORDER BY 2 DESC, 1 LIMIT 100",
      "SELECT location, COUNT(*) AS n FROM visitors GROUP BY location ORDER BY "
      "n "
      "DESC, location",
      "SELECT id, content FROM stories ORDER BY content, id",
      "SELECT 1 / views FROM stories ORDER BY 1 LIMIT 1",
  };
  return corpus;
}

// Two-table and three-table joins; hash joins, residual conditions, nested
// loops, INT against DOUBLE, a self join, an empty side, an ambiguous column.
const std::vector<std::string>& JoinCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT s.id, v.location FROM stories s JOIN visitors v ON s.username = "
      "v.username",
      "SELECT s.id, r.type FROM stories s JOIN reactions r ON r.storyid = s.id "
      "WHERE r.type = 'like'",
      "SELECT v.username, s.id, r.id FROM visitors v JOIN stories s ON "
      "s.username "
      "= v.username JOIN reactions r ON r.storyid = s.id",
      "SELECT r.id, r.username, s.username FROM reactions r JOIN stories s ON "
      "r.storyid = s.id AND r.username <> s.username",
      "SELECT v1.id, v2.id FROM visitors v1 JOIN visitors v2 ON v1.rating < "
      "v2.rating",
      "SELECT v.id, s.id FROM visitors v JOIN stories s ON v.rating = s.views",
      "SELECT v.id, s.id FROM visitors v JOIN stories s ON s.views = v.rating",
      "SELECT a.id, b.id FROM stories a JOIN stories b ON a.content = "
      "b.content "
      "AND a.id < b.id",
      "SELECT r.id, v.location, s.content FROM reactions r JOIN visitors v ON "
      "r.username = v.username JOIN stories s ON r.storyid = s.id ORDER BY "
      "r.id "
      "LIMIT 5",
      "SELECT * FROM visitors v JOIN stories s ON s.username = v.username",
      "SELECT s.id FROM stories s JOIN visitors v ON s.username = v.username "
      "WHERE "
      "v.location = 'Rome' AND s.views > 2",
      "SELECT s.id FROM stories s JOIN visitors v ON s.username = v.username "
      "WHERE "
      "v.location = 'Nowhere'",
      "SELECT id FROM visitors JOIN stories ON stories.username = "
      "visitors.username",
      "SELECT r.id, s.id FROM reactions r JOIN stories s ON r.storyid = s.id "
      "ORDER BY s.id DESC, r.id LIMIT 6 OFFSET 3",
  };
  return corpus;
}

// Aggregates with and without GROUP BY, an empty input, an overflow.
const std::vector<std::string>& AggregateCorpus() {
  static const std::vector<std::string> corpus = {
      "SELECT COUNT(*), SUM(views), MIN(views), MAX(views), AVG(views) FROM "
      "stories",
      "SELECT MIN(content), MAX(content), MIN(rating), MAX(rating) FROM "
      "stories "
      "JOIN visitors ON stories.username = visitors.username",
      "SELECT username, COUNT(*), SUM(views) FROM stories GROUP BY username "
      "ORDER BY username",
      "SELECT type, COUNT(*), SUM(weight), MIN(weight), MAX(timestamp) FROM "
      "reactions GROUP BY type ORDER BY type",
      "SELECT COUNT(*), SUM(views) FROM stories WHERE id < 0",
      "SELECT COUNT(*), AVG(views) FROM stories WHERE id < 0",
      "SELECT MIN(views) FROM stories WHERE id < 0",
      "SELECT MAX(content) FROM stories WHERE id < 0",
      "SELECT username, COUNT(*) FROM stories WHERE id < 0 GROUP BY username",
      "SELECT 1, COUNT(*) FROM visitors",
      "SELECT LENGTH(content), COUNT(*) FROM stories GROUP BY LENGTH(content) "
      "ORDER BY 1",
      "SELECT v.location, COUNT(*) FROM visitors v JOIN stories s ON "
      "s.username = "
      "v.username GROUP BY v.location ORDER BY 2 DESC, 1",
      "SELECT storyid, COUNT(*), SUM(timestamp) FROM reactions GROUP BY "
      "storyid ORDER BY storyid LIMIT 4",
  };
  return corpus;
}

// Writes: INSERT (auto-increment, defaults, every constraint, a multi-row
// statement that fails midway), CREATE TABLE, and the overflow of the
// auto-increment value.
const std::vector<std::string>& InsertCorpus() {
  static const std::vector<std::string> corpus = {
      "INSERT INTO visitors (username, location) VALUES ('fay', 'Rome')",
      "INSERT INTO visitors (username, location) VALUES ('gus', 'Lima'), "
      "('hal', 'Oslo'), ('ivy', 'Paris')",
      "INSERT INTO visitors VALUES (4, 'dup', 'x', 1.0)",
      "INSERT INTO visitors (username, location) VALUES ('ann', 'x')",
      "INSERT INTO visitors VALUES (50, 'ann', 'x', 1.0)",
      "INSERT INTO visitors VALUES (4, 'ann', 'x', 1.0)",
      "INSERT INTO visitors (username, location) VALUES ('zed', 'x'), ('zed', "
      "'y')",
      "INSERT INTO visitors VALUES (20, 'jo', 'x', 7)",
      "INSERT INTO visitors (username) VALUES ('nolocation')",
      "INSERT INTO stories (content, username) VALUES ('new story', 'ann')",
      "INSERT INTO stories (content, username) VALUES ('p', 'nobody')",
      "INSERT INTO stories VALUES (100, 'x', 'ann', 1), (101, 'y', 'ghost', 2)",
      "INSERT INTO stories (content, username, views) VALUES ('x, \"y\" "
      "\xC3\xBCn\xC3\xAF', 'bob', -3), ('', 'cat', 0)",
      "INSERT INTO reactions VALUES (21, 1, 'bob', 'like', 1, 2000)",
      "INSERT INTO reactions VALUES (22, 999, 'bob', 'like', 1.0, 1)",
      "INSERT INTO reactions VALUES (23, 1, 'ghost', 'like', 1.0, 1)",
      "INSERT INTO reactions VALUES (21, 1, 'bob', 'like', 1.0, 1)",
      "INSERT INTO reactions VALUES (24, 1, 'bob', 'like', 1.0, "
      "9223372036854775807)",
      // Several violations in one row: the primary key, then UNIQUE columns in
      // column order, then foreign keys in declaration order.
      "INSERT INTO stories VALUES (1, 'x', 'ghost', 1)",
      "INSERT INTO reactions VALUES (21, 999, 'ghost', 'like', 1.0, 1)",
      "INSERT INTO reactions VALUES (30, 999, 'ghost', 'like', 1.0, 1)",
      "CREATE TABLE twin (id INT PRIMARY KEY, a TEXT UNIQUE, b TEXT UNIQUE)",
      "INSERT INTO twin VALUES (1, 'x', 'y')",
      "INSERT INTO twin VALUES (2, 'x', 'y')",
      "INSERT INTO twin VALUES (2, 'z', 'y')",
      "INSERT INTO twin VALUES (1, 'x', 'y')",
      "CREATE TABLE tagged (id INT PRIMARY KEY, tag TEXT UNIQUE, owner TEXT "
      "REFERENCES visitors(username))",
      "INSERT INTO tagged VALUES (1, 't', 'ann')",
      "INSERT INTO tagged VALUES (2, 't', 'ghost')",
      "INSERT INTO tagged VALUES (2, 'u', 'ghost')",
      // Two INT values above 2^53 share one DOUBLE value: a join on them has
      // to compare the INT values, not only the hash keys.
      "INSERT INTO reactions VALUES (25, 1, 'bob', 'like', 1.0, "
      "9007199254740992), (26, 1, 'bob', 'like', 1.0, 9007199254740993)",
      "SELECT a.id, b.id FROM reactions a JOIN reactions b ON a.timestamp "
      "= b.timestamp ORDER BY a.id, b.id",
      "INSERT INTO stories (content, username, views) VALUES ('o', 'ann', "
      "9223372036854775807 + 1)",
      // The auto-increment value overflows after the largest INT.
      "INSERT INTO visitors VALUES (9223372036854775807, 'big', 'x', 1.0)",
      "INSERT INTO visitors (username, location) VALUES ('after', 'y')",
      "SELECT MAX(id) FROM visitors",
      "DELETE FROM visitors WHERE id = 9223372036854775807",
      "INSERT INTO visitors (username, location) VALUES ('after', 'y')",
      "SELECT id, username FROM visitors ORDER BY id",
  };
  return corpus;
}

// CREATE TABLE and the auto-increment start value.
const std::vector<std::string>& DefinitionCorpus() {
  static const std::vector<std::string> corpus = {
      "CREATE TABLE jobs (id INT PRIMARY KEY AUTOINCREMENT, body TEXT)",
      "INSERT INTO jobs (body) VALUES ('first')",
      "INSERT INTO jobs VALUES (-5, 'neg')",
      "DELETE FROM jobs WHERE id = 1",
      "INSERT INTO jobs (body) VALUES ('after negative'), ('second')",
      "SELECT * FROM jobs",
      "DELETE FROM jobs",
      "INSERT INTO jobs (body) VALUES ('empty again')",
      "SELECT * FROM jobs",
      "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT DEFAULT 'none', "
      "score DOUBLE DEFAULT 1.5, tag TEXT UNIQUE)",
      "INSERT INTO notes (id, tag) VALUES (1, 't1')",
      "INSERT INTO notes (id, tag) VALUES (2, 't1')",
      "INSERT INTO notes VALUES (3, 'b', 2, 't3')",
      "SELECT id, body, score, tag FROM notes ORDER BY id",
      "CREATE TABLE visitors (id INT PRIMARY KEY)",
      "CREATE TABLE bad (id INT PRIMARY KEY, a INT, a TEXT)",
      "CREATE TABLE nopk (a INT)",
      "CREATE TABLE refs (id INT PRIMARY KEY, uid INT REFERENCES visitors(id), "
      "who TEXT REFERENCES visitors(username))",
      "INSERT INTO refs VALUES (1, 1, 'ann')",
      "INSERT INTO refs VALUES (2, 999, 'ann')",
      "INSERT INTO refs VALUES (3, 8, 'gus')",
      "DELETE FROM visitors WHERE id = 8",
      "UPDATE visitors SET username = 'gus2' WHERE id = 8",
      "SELECT id, uid, who FROM refs ORDER BY id",
      "DELETE FROM refs WHERE id > 100",
  };
  return corpus;
}

// ALTER TABLE ADD COLUMN of each type, DROP TABLE of a table that nothing
// references, a refused DROP of a referenced table, and a table created again
// under a dropped name. The visitors table is referenced by stories and
// reactions.
const std::vector<std::string>& SchemaChangeCorpus() {
  static const std::vector<std::string> corpus = {
      "CREATE TABLE scratch (id INT PRIMARY KEY, name TEXT, score DOUBLE)",
      "INSERT INTO scratch VALUES (1, 'a', 1.5), (2, 'b', 2.5), (3, 'c', 3.5)",
      "ALTER TABLE scratch ADD COLUMN flag INT DEFAULT 0",
      "ALTER TABLE scratch ADD COLUMN ratio DOUBLE DEFAULT 0.25",
      "ALTER TABLE scratch ADD COLUMN note TEXT DEFAULT 'none'",
      "SELECT id, name, score, flag, ratio, note FROM scratch ORDER BY id",
      "INSERT INTO scratch (id, name) VALUES (4, 'd')",
      "UPDATE scratch SET flag = 1, note = 'changed' WHERE id = 2",
      "SELECT id, flag, ratio, note FROM scratch ORDER BY id",
      "SELECT COUNT(*), SUM(flag) FROM scratch",
      "SELECT s.id, v.username FROM scratch s JOIN visitors v ON v.id = s.id "
      "ORDER BY s.id",
      "ALTER TABLE scratch ADD COLUMN flag INT DEFAULT 5",
      "ALTER TABLE missing ADD COLUMN x INT DEFAULT 0",
      "ALTER TABLE scratch ADD COLUMN y INT",
      "DROP TABLE visitors",
      "DROP TABLE scratch",
      "SELECT * FROM scratch",
      "CREATE TABLE scratch (id INT PRIMARY KEY, other TEXT)",
      "SELECT * FROM scratch",
      "INSERT INTO scratch VALUES (1, 'again')",
      "SELECT id, other FROM scratch",
      "DROP TABLE scratch",
      "DROP TABLE scratch",
  };
  return corpus;
}

// UPDATE: assignments, no-ops, uniqueness, foreign keys, the cascade (a swap,
// a chained rename), evaluation errors, and rows that grow until they move to
// another page.
const std::vector<std::string>& UpdateCorpus() {
  static const std::vector<std::string> corpus = {
      "UPDATE visitors SET rating = rating + 1 WHERE location = 'Paris'",
      "UPDATE visitors SET location = 'Berlin' WHERE id = 99",
      "UPDATE stories SET views = views + 1",
      "UPDATE stories SET views = views",
      "UPDATE visitors SET rating = rating * -1 WHERE id = 4",
      "SELECT id, rating FROM visitors WHERE id = 4",
      "UPDATE visitors SET rating = 7 WHERE id = 2",
      "UPDATE visitors SET rating = 'x'",
      "UPDATE visitors SET id = 77 WHERE id = 1",
      // Two columns exchanged; the changed username cascades.
      "UPDATE visitors SET location = username, username = location WHERE id = "
      "5",
      "SELECT id, username, location FROM visitors WHERE id = 5",
      "SELECT id, username FROM stories WHERE id = 6",
      "SELECT id, username FROM reactions WHERE id = 8",
      "UPDATE visitors SET location = username, username = location WHERE id = "
      "5",
      // A chain: A becomes B while B becomes C; references follow one step.
      "UPDATE visitors SET username = username || 'x' WHERE id = 1 OR id = 6",
      "SELECT username, COUNT(*) FROM stories GROUP BY username ORDER BY 1",
      "SELECT username, COUNT(*) FROM reactions GROUP BY username ORDER BY "
      "1",
      "UPDATE visitors SET username = SUBSTR(username, 1, LENGTH(username) - "
      "1) "
      "WHERE id = 1 OR id = 6",
      "SELECT username, COUNT(*) FROM stories GROUP BY username ORDER BY 1",
      "UPDATE visitors SET username = 'bob' WHERE id = 3",
      "UPDATE visitors SET username = username || 'x' WHERE id = 2",
      "UPDATE visitors SET username = SUBSTR(username, 1, 3) WHERE id = 2",
      "UPDATE stories SET username = 'ghost' WHERE id = 1",
      "UPDATE stories SET username = 'eve' WHERE id = 1",
      "UPDATE reactions SET storyid = 999 WHERE id = 1",
      "UPDATE reactions SET storyid = 2 WHERE id = 1",
      "UPDATE stories SET views = 100 / (views - views)",
      "UPDATE stories SET views = 0 WHERE 10 / (views - views) > 1",
      "UPDATE reactions SET timestamp = timestamp + 1",
      "UPDATE stories SET views = views * 9223372036854775807 WHERE views > 1",
      // Rows that grow from a few bytes to over a thousand, then shrink.
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "UPDATE stories SET content = content || content WHERE LENGTH(content) < "
      "1000",
      "SELECT id, LENGTH(content) FROM stories ORDER BY id",
      "SELECT username, COUNT(*), MAX(LENGTH(content)) FROM stories GROUP BY "
      "username ORDER BY 1",
      "UPDATE stories SET content = SUBSTR(content, 1, 12)",
      "SELECT id, content FROM stories ORDER BY id",
  };
  return corpus;
}

// DELETE: RESTRICT, a delete that frees a slot for the next INSERT.
const std::vector<std::string>& DeleteCorpus() {
  static const std::vector<std::string> corpus = {
      "DELETE FROM reactions WHERE type = 'share'",
      "DELETE FROM visitors WHERE id = 4",
      "DELETE FROM stories WHERE id = 5",
      "DELETE FROM reactions WHERE storyid = 5",
      "DELETE FROM stories WHERE id = 5",
      "DELETE FROM visitors WHERE id = 4",
      "DELETE FROM reactions WHERE username = 'dan'",
      "DELETE FROM visitors WHERE id = 4",
      "DELETE FROM reactions WHERE id = 9999",
      "DELETE FROM stories WHERE id = 8",
      "INSERT INTO stories VALUES (8, 'reused', 'bob', 1)",
      "SELECT * FROM stories ORDER BY id",
      "DELETE FROM nosuch",
      "DELETE FROM visitors WHERE username = 1",
      "SELECT COUNT(*) FROM visitors",
      "SELECT COUNT(*), SUM(views) FROM stories",
      "SELECT COUNT(*) FROM reactions",
  };
  return corpus;
}

// The sign of a DOUBLE zero. -0.0 and 0.0 compare equal, so an assignment equal
// to the stored value keeps the stored cell, also when another cell of the row
// changes. The comparison of cells cannot see the sign, so CAST prints it.
const std::vector<std::string>& ZeroCorpus() {
  static const std::vector<std::string> corpus = {
      "CREATE TABLE readings (id INT PRIMARY KEY, d DOUBLE, n INT)",
      "INSERT INTO readings VALUES (1, 0.0, 1), (2, -0.0, 1), (3, -0.0, 1), "
      "(4, -1.5, 1)",
      "UPDATE readings SET d = -0.0, n = 2 WHERE id = 1",
      "UPDATE readings SET d = 0, n = 6 WHERE id = 2",
      "UPDATE readings SET d = ABS(d)",
      "SELECT id, CAST(d AS TEXT), n FROM readings ORDER BY id",
  };
  return corpus;
}

// Runs the whole corpus on both engines over a pool of 4 frames and the given
// replacement policy, then compares every table once more through SELECT *.
void RunFixedCorpus(ReplacementPolicyKind policy) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           std::string("fixed corpus, ") + PolicyName(policy));
  Outcomes outcomes;
  const std::vector<const std::vector<std::string>*> sections = {
      &EquivalenceSetupStatements(),
      &ReadCorpus(),
      &OrderCorpus(),
      &JoinCorpus(),
      &AggregateCorpus(),
      &InsertCorpus(),
      &DefinitionCorpus(),
      &SchemaChangeCorpus(),
      &UpdateCorpus(),
      &ZeroCorpus(),
      &DeleteCorpus()};
  for (const std::vector<std::string>* section : sections) {
    for (const std::string& sql : *section) {
      if (!RUN_EQUIVALENT(runner, sql)) {
        return;
      }
      CountOutcome(sql, runner.last_flatfile_result(), &outcomes);
    }
  }
  for (const domain::TableSchema& schema : pair.flatfile->DescribeCatalog()) {
    if (!RUN_EQUIVALENT(runner, "SELECT * FROM " + schema.name)) {
      return;
    }
  }

  // The corpus reaches every kind of outcome of 2.8 and the constraint rules.
  CHECK(outcomes.selects_with_rows >= 50);
  CHECK(outcomes.selects_without_rows >= 3);
  CHECK(outcomes.ordered_limit_selects >= 6);
  CHECK(outcomes.writes_applied >= 40);
  CHECK(outcomes.writes_matching_no_row >= 3);
  CHECK(outcomes.bind_errors >= 8);
  CHECK(outcomes.runtime_errors >= 6);
  CHECK(outcomes.duplicate_keys >= 6);
  CHECK(outcomes.missing_parents >= 5);
  CHECK(outcomes.restricted_deletes >= 4);
  // The corpus avoids the statements of the difference table, which have
  // their own test.
  CHECK_EQ(outcomes.unsupported_errors, std::size_t{1});  // UPDATE of the key
  CHECK_EQ(outcomes.other_errors, std::size_t{0});
}

// ----------------------------------------------------------------------------
// The known differences, each as the design document records it.
// ----------------------------------------------------------------------------

// The two results of one statement.
struct BothResults {
  ExecutionResult flatfile;
  ExecutionResult paged;
};

BothResults RunBoth(EnginePair* pair, const std::string& sql) {
  return BothResults{ExecuteSql(*pair->flatfile, sql),
                     ExecuteSql(*pair->paged.service, sql)};
}

// Runs sql on both engines and reports a failure unless they are equivalent.
bool ExpectSame(EnginePair* pair, const std::string& sql) {
  const BothResults results = RunBoth(pair, sql);
  return ReportDifference(
      __FILE__, __LINE__,
      FindResultDifference(results.flatfile, results.paged, {}));
}

// Reports a failure unless result is the error of this kind and message.
void ExpectError(const ExecutionResult& result, ErrorKind kind,
                 const std::string& message) {
  const ExecutionError* error = AsError(result);
  if (error == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected an error, got " + DescribeResult(result));
    return;
  }
  CHECK_EQ(std::string(domain::ErrorKindName(error->kind)),
           std::string(domain::ErrorKindName(kind)));
  CHECK_EQ(error->message, message);
}

void ExpectAffected(const ExecutionResult& result, std::size_t count) {
  const domain::AffectedRows* affected = AsAffectedRows(result);
  if (affected == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected affected=" + std::to_string(count) + ", got " +
                      DescribeResult(result));
    return;
  }
  CHECK_EQ(affected->count, count);
}

void ExpectComplete(const ExecutionResult& result, const std::string& tag) {
  const domain::CommandComplete* complete = AsCommandComplete(result);
  if (complete == nullptr) {
    ReportFailure(
        __FILE__, __LINE__,
        "expected complete " + tag + ", got " + DescribeResult(result));
    return;
  }
  CHECK_EQ(complete->tag, tag);
}

// The text of the first column of every row, in the engine's own order.
std::vector<std::string> FirstColumn(const ExecutionResult& result) {
  std::vector<std::string> cells;
  const domain::ResultSet* rows = AsResultSet(result);
  if (rows == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected a result set, got " + DescribeResult(result));
    return cells;
  }
  for (const Row& row : rows->rows) {
    cells.push_back(domain::FormatValue(row[0]));
  }
  return cells;
}

// The first element of `order` that is one of `wanted`.
std::string FirstOf(const std::vector<std::string>& order,
                    const std::vector<std::string>& wanted) {
  for (const std::string& cell : order) {
    for (const std::string& candidate : wanted) {
      if (cell == candidate) {
        return cell;
      }
    }
  }
  return "";
}

// TEXT with a line break (ADR-0012 against ADR-0027).
void CheckLineBreakDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT)"));
  const BothResults newline =
      RunBoth(pair, "INSERT INTO notes VALUES (1, 'line one\nline two')");
  ExpectError(newline.flatfile, ErrorKind::kUnsupported,
              "flatfile engine cannot store a line break in notes.body");
  ExpectAffected(newline.paged, 1);
  const BothResults carriage_return =
      RunBoth(pair, "INSERT INTO notes VALUES (2, 'carriage\rreturn')");
  ExpectError(carriage_return.flatfile, ErrorKind::kUnsupported,
              "flatfile engine cannot store a line break in notes.body");
  ExpectAffected(carriage_return.paged, 1);

  // flatfile stored neither row; paged stores both and returns them intact.
  const BothResults counts = RunBoth(pair, "SELECT id FROM notes");
  CHECK_EQ(FirstColumn(counts.flatfile).size(), std::size_t{0});
  CHECK_EQ(FirstColumn(counts.paged).size(), std::size_t{2});
  const BothResults body = RunBoth(pair, "SELECT body FROM notes WHERE id = 1");
  const domain::ResultSet* body_rows = AsResultSet(body.paged);
  CHECK(body_rows != nullptr && body_rows->rows.size() == 1 &&
        body_rows->rows[0][0] == Value(std::string("line one\nline two")));
}

// A tuple above 4060 bytes (ADR-0027).
void CheckRowLengthDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE wide (id INT PRIMARY KEY, body TEXT)"));
  const std::string insert_long =
      "INSERT INTO wide VALUES (1, '" + std::string(4100, 'x') + "')";
  const BothResults too_long = RunBoth(pair, insert_long);
  ExpectAffected(too_long.flatfile, 1);
  ExpectError(too_long.paged, ErrorKind::kUnsupported,
              "paged engine cannot store a row longer than 4060 bytes in wide");
  // 4050 bytes of TEXT make a 4060-byte tuple, which both engines store.
  const BothResults at_limit = RunBoth(
      pair, "INSERT INTO wide VALUES (2, '" + std::string(4050, 'y') + "')");
  ExpectAffected(at_limit.flatfile, 1);
  ExpectAffected(at_limit.paged, 1);
  const BothResults lengths = RunBoth(pair, "SELECT LENGTH(body) FROM wide");
  CHECK_EQ(FirstColumn(lengths.flatfile),
           std::vector<std::string>({"4100", "4050"}));
  CHECK_EQ(FirstColumn(lengths.paged), std::vector<std::string>({"4050"}));
}

// The reserved prefix, which only the paged engine refuses, and the two
// statements that both engines run alike (ALTER TABLE ADD COLUMN and DROP
// TABLE).
void CheckDdlDifferences(EnginePair* pair) {
  const BothResults reserved =
      RunBoth(pair, "CREATE TABLE sys_x (id INT PRIMARY KEY)");
  ExpectComplete(reserved.flatfile, "CREATE TABLE sys_x");
  const ExecutionError* reserved_error = AsError(reserved.paged);
  CHECK(reserved_error != nullptr &&
        reserved_error->kind == ErrorKind::kUnsupported &&
        Contains(reserved_error->message, "sys_"));

  CHECK(ExpectSame(pair, "CREATE TABLE spare (id INT PRIMARY KEY, v INT)"));
  CHECK(ExpectSame(pair, "INSERT INTO spare VALUES (1, 10)"));
  CHECK(ExpectSame(pair, "ALTER TABLE spare ADD COLUMN extra INT DEFAULT 0"));
  CHECK(ExpectSame(pair, "DROP TABLE spare"));
}

// Rows below a LIMIT that are never read (ADR-0036).
void CheckEarlyTerminationDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair, "CREATE TABLE ratio (id INT PRIMARY KEY, x INT)"));
  CHECK(ExpectSame(pair, "INSERT INTO ratio VALUES (1, 5), (2, 0), (3, 2)"));
  const BothResults limited = RunBoth(pair, "SELECT 1 / x FROM ratio LIMIT 1");
  ExpectError(limited.flatfile, ErrorKind::kRuntime, "division by zero");
  const domain::ResultSet* one_row = AsResultSet(limited.paged);
  CHECK(one_row != nullptr && one_row->rows.size() == 1);
  // Under a Sort every row is read, so both report the error.
  CHECK(ExpectSame(pair, "SELECT 1 / x FROM ratio ORDER BY 1 LIMIT 1"));
  // LIMIT 0 reads no row at all, not even below a Sort: only flatfile reports.
  const BothResults none =
      RunBoth(pair, "SELECT 1 / x FROM ratio ORDER BY 1 LIMIT 0");
  ExpectError(none.flatfile, ErrorKind::kRuntime, "division by zero");
  const domain::ResultSet* no_rows = AsResultSet(none.paged);
  CHECK(no_rows != nullptr && no_rows->rows.empty());
}

// When several rows violate a constraint, each engine reports the first one in
// its own order; the kind is the same. A delete and an insert make the order
// of paged (slot order) differ from the order of flatfile (file order).
void CheckViolationOrderDifference(EnginePair* pair) {
  CHECK(ExpectSame(pair,
                   "CREATE TABLE parents (id INT PRIMARY KEY, name TEXT "
                   "UNIQUE)"));
  CHECK(ExpectSame(pair,
                   "CREATE TABLE children (id INT PRIMARY KEY, parent "
                   "TEXT REFERENCES parents(name))"));
  CHECK(ExpectSame(pair,
                   "INSERT INTO parents VALUES (1, 'filler'), (2, 'a'), "
                   "(3, 'ax'), (4, 'bx')"));
  CHECK(ExpectSame(pair, "DELETE FROM parents WHERE id = 1"));
  CHECK(ExpectSame(pair, "INSERT INTO parents VALUES (5, 'b')"));
  CHECK(ExpectSame(pair, "INSERT INTO children VALUES (1, 'a'), (2, 'b')"));

  const BothResults order = RunBoth(pair, "SELECT id FROM parents");
  const std::vector<std::string> flatfile_order = FirstColumn(order.flatfile);
  const std::vector<std::string> paged_order = FirstColumn(order.paged);
  CHECK_EQ(flatfile_order, std::vector<std::string>({"2", "3", "4", "5"}));
  // The scenario only means something when the two orders differ.
  CHECK(flatfile_order != paged_order);

  // DELETE with two referenced rows (a: id 2, b: id 5).
  const BothResults restricted = RunBoth(pair, "DELETE FROM parents");
  const ExecutionError* flatfile_error = AsError(restricted.flatfile);
  const ExecutionError* paged_error = AsError(restricted.paged);
  CHECK(flatfile_error != nullptr && paged_error != nullptr);
  if (flatfile_error != nullptr && paged_error != nullptr) {
    CHECK(flatfile_error->kind == ErrorKind::kConstraintViolation);
    CHECK(paged_error->kind == ErrorKind::kConstraintViolation);
    CHECK_EQ(flatfile_error->message,
             std::string("parents.id=2 is referenced by children.parent"));
    // Each engine names the first referenced row of its own order.
    CHECK_EQ(paged_error->message,
             "parents.id=" + FirstOf(paged_order, {"2", "5"}) +
                 " is referenced by children.parent");
    CHECK(flatfile_error->message != paged_error->message);
  }

  // UPDATE where 'a' and 'b' both become a name that exists: 'ax' and 'bx'.
  const BothResults renamed = RunBoth(
      pair,
      "UPDATE parents SET name = name || 'x' WHERE name = 'a' OR name = 'b'");
  const ExecutionError* flatfile_rename = AsError(renamed.flatfile);
  const ExecutionError* paged_rename = AsError(renamed.paged);
  CHECK(flatfile_rename != nullptr && paged_rename != nullptr);
  if (flatfile_rename != nullptr && paged_rename != nullptr) {
    CHECK(flatfile_rename->kind == ErrorKind::kConstraintViolation);
    CHECK(paged_rename->kind == ErrorKind::kConstraintViolation);
    // Row 2 (a) becomes ax and row 5 (b) becomes bx; both names exist.
    CHECK_EQ(flatfile_rename->message,
             FirstOf(flatfile_order, {"2", "5"}) == "2"
                 ? std::string("duplicate key: parents.name=ax")
                 : std::string("duplicate key: parents.name=bx"));
    CHECK_EQ(paged_rename->message,
             FirstOf(paged_order, {"2", "5"}) == "2"
                 ? std::string("duplicate key: parents.name=ax")
                 : std::string("duplicate key: parents.name=bx"));
    CHECK(flatfile_rename->message != paged_rename->message);
  }
}

// CREATE TABLE next to a CSV file of the same name: flatfile adopts its rows,
// paged has no concept of CSV and creates an empty table.
void CheckAdoptedCsvDifference() {
  TempDir directory;
  WriteFile(directory.path() / "t.csv", "id,name\n1,a\n2,b\n");
  std::unique_ptr<QueryService> flatfile = MakeLoadedService(directory.path());
  PagedFixture paged =
      MakeFilePagedFixture(directory.path(), 16, ReplacementPolicyKind::kLru);
  if (flatfile == nullptr || paged.service == nullptr) {
    return;
  }
  const std::string create = "CREATE TABLE t (id INT PRIMARY KEY, name TEXT)";
  ExpectComplete(ExecuteSql(*flatfile, create),
                 "CREATE TABLE t (adopted 2 rows)");
  ExpectComplete(ExecuteSql(*paged.service, create), "CREATE TABLE t");
  CHECK_EQ(FirstColumn(ExecuteSql(*flatfile, "SELECT COUNT(*) FROM t")),
           std::vector<std::string>{"2"});
  CHECK_EQ(FirstColumn(ExecuteSql(*paged.service, "SELECT COUNT(*) FROM t")),
           std::vector<std::string>{"0"});
}

std::vector<std::string> RunRepl(const std::vector<std::string>& input_lines) {
  std::string text;
  for (const std::string& line : input_lines) {
    text += line + "\n";
  }
  std::istringstream input(text);
  std::ostringstream output;
  cli::RunRepl(input, output, false);
  std::vector<std::string> lines;
  std::istringstream reader(output.str());
  for (std::string line; std::getline(reader, line);) {
    lines.push_back(line);
  }
  return lines;
}

bool AnyLineStartsWith(const std::vector<std::string>& lines,
                       const std::string& prefix) {
  for (const std::string& line : lines) {
    if (line.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

// `.open <directory> --parallel`: flatfile loads its tables in parallel and
// paged, which has no load phase, accepts the option and ignores it.
void CheckParallelOption() {
  TempDir directory;
  WriteFixtureA(directory.path());
  const std::string open = ".open " + directory.path().string() + " --parallel";

  const std::vector<std::string> flatfile = RunRepl({open});
  CHECK_EQ(flatfile,
           std::vector<std::string>{"loaded visitors=4 stories=3 reactions=5"});

  const std::vector<std::string> paged = RunRepl({".engine paged", open});
  CHECK(paged.size() >= 2 && paged[0] == "engine paged" &&
        paged[1] == "loaded no tables");
  CHECK(!AnyLineStartsWith(paged, "error"));
}

// What reaches the file when a write statement returns (design document 2.15):
// both engines write it out at once, paged without a sync. The storage layer
// states the file is complete after Close, so the file is read again after
// Close too.
void CheckDurabilityDifference() {
  TempDir flatfile_directory;
  TempDir paged_directory;
  TempDir copy_directory;
  std::unique_ptr<QueryService> flatfile =
      MakeLoadedService(flatfile_directory.path());
  PagedFixture paged = MakeFilePagedFixture(paged_directory.path(), 16,
                                            ReplacementPolicyKind::kLru);
  if (flatfile == nullptr || paged.service == nullptr) {
    return;
  }
  const std::filesystem::path paged_file = paged_directory.path() / "paged.db";
  const std::string bytes_at_open = ReadFile(paged_file);

  const std::string create = "CREATE TABLE d (id INT PRIMARY KEY, v TEXT)";
  const std::string insert = "INSERT INTO d VALUES (1, 'persisted')";
  CHECK(AsCommandComplete(ExecuteSql(*flatfile, create)) != nullptr);
  CHECK(AsCommandComplete(ExecuteSql(*paged.service, create)) != nullptr);
  ExpectAffected(ExecuteSql(*flatfile, insert), 1);
  ExpectAffected(ExecuteSql(*paged.service, insert), 1);

  // Before Close: the CSV file holds the row, and so does paged.db.
  CHECK(Contains(ReadFile(flatfile_directory.path() / "d.csv"), "1,persisted"));
  CHECK(ReadFile(paged_file) != bytes_at_open);
  std::error_code error;
  std::filesystem::copy_file(paged_file, copy_directory.path() / "paged.db",
                             error);
  CHECK(!error);
  {
    PagedFixture copy = MakeFilePagedFixture(copy_directory.path(), 16,
                                             ReplacementPolicyKind::kLru);
    if (copy.service == nullptr) {
      return;
    }
    CHECK_EQ(FirstColumn(ExecuteSql(*copy.service, "SELECT v FROM d")),
             std::vector<std::string>{"persisted"});
  }

  // After Close (the destruction of the engine) the file still holds the row.
  paged.service.reset();
  paged = MakeFilePagedFixture(paged_directory.path(), 16,
                               ReplacementPolicyKind::kLru);
  if (paged.service == nullptr) {
    return;
  }
  CHECK_EQ(FirstColumn(ExecuteSql(*paged.service, "SELECT v FROM d")),
           std::vector<std::string>{"persisted"});
}

// ----------------------------------------------------------------------------
// Random statements.
// ----------------------------------------------------------------------------

constexpr std::size_t kStatementsPerSeed = 1500;
// A scratch table is created, altered and dropped after every this many
// generated statements, so that the DDL runs among the random writes.
constexpr std::size_t kStatementsPerScratchCycle = 100;

// Creates `table`, fills it, adds an INT and a TEXT column, reads it back,
// updates it and drops it, comparing the engines after each statement. The
// table is gone at the end, so the generator never meets it. False after the
// first difference.
bool RunScratchCycle(EquivalenceRunner* runner, const std::string& table) {
  const std::string statements[] = {
      "CREATE TABLE " + table + " (id INT PRIMARY KEY, v TEXT)",
      "INSERT INTO " + table + " VALUES (1, 'a'), (2, 'b'), (3, 'c')",
      "ALTER TABLE " + table + " ADD COLUMN n INT DEFAULT 4",
      "ALTER TABLE " + table + " ADD COLUMN t TEXT DEFAULT 'z'",
      "INSERT INTO " + table + " (id, v) VALUES (4, 'd')",
      "UPDATE " + table + " SET n = n + id WHERE id > 1",
      "SELECT id, v, n, t FROM " + table + " ORDER BY id",
      "DROP TABLE " + table,
  };
  for (const std::string& sql : statements) {
    if (!RUN_EQUIVALENT(*runner, sql)) {
      return false;
    }
  }
  return true;
}

// "ok", or the message of the first violation Database::CheckIndex found.
std::string IndexCheckText(const paged::Status& status) {
  return status.has_value() ? status->message : "ok";
}

struct RandomRun {
  std::uint64_t seed;
  ReplacementPolicyKind policy;
  std::size_t frames;
  std::size_t key_percent;  // the share of key statements (0 for none)
};

// The setup, then kStatementsPerSeed generated statements, compared one by one.
// The sequence number in a difference counts the setup statements too.
void RunRandomStatements(const RandomRun& run) {
  EnginePair pair;
  if (!OpenPair(&pair, run.frames, run.policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           "seed " + std::to_string(run.seed) + ", " +
                               PolicyName(run.policy) + ", " +
                               std::to_string(run.frames) + " frames");
  for (const std::string& sql : EquivalenceSetupStatements()) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  StatementGenerator generator(run.seed, run.key_percent);
  Outcomes outcomes;
  for (std::size_t i = 0; i < kStatementsPerSeed; ++i) {
    if (i % kStatementsPerScratchCycle == 0 &&
        !RunScratchCycle(&runner, "scratch" + std::to_string(i))) {
      return;
    }
    const std::string sql = generator.Next(runner.flatfile_tables());
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
    CountOutcome(sql, runner.last_flatfile_result(), &outcomes);
  }
  for (const domain::TableSchema& schema : pair.flatfile->DescribeCatalog()) {
    if (!RUN_EQUIVALENT(runner, "SELECT * FROM " + schema.name)) {
      return;
    }
  }

  if (run.key_percent > 0) {
    // The index of every table agrees with its rows after the whole stream,
    // and no pin is left.
    paged::PagedEngine& engine = *pair.paged.engine;
    for (const std::string& table : engine.Catalog().TableNames()) {
      CHECK_EQ(IndexCheckText(engine.database().CheckIndex(table)),
               std::string("ok"));
    }
    CHECK_EQ(engine.database().buffer().PinnedFrameCount(), std::size_t{0});
    // Key statements keep the tables populated, read rows, match no row and
    // change rows.
    CHECK(outcomes.selects_with_rows >= 300);
    CHECK(outcomes.selects_without_rows >= 40);
    CHECK(outcomes.writes_applied >= 150);
    CHECK(outcomes.writes_matching_no_row >= 30);
    CHECK_EQ(outcomes.other_errors, std::size_t{0});
    return;
  }

  // A stream that stopped reaching the rules (every statement failing to bind,
  // say) would pass vacuously, so the outcomes are checked.
  CHECK(outcomes.selects_with_rows >= 200);
  CHECK(outcomes.selects_without_rows >= 20);
  CHECK(outcomes.ordered_limit_selects >= 30);
  CHECK(outcomes.writes_applied >= 250);
  CHECK(outcomes.writes_matching_no_row >= 10);
  CHECK(outcomes.runtime_errors >= 15);
  CHECK(outcomes.duplicate_keys >= 15);
  CHECK(outcomes.missing_parents >= 8);
  CHECK(outcomes.restricted_deletes >= 5);
  CHECK_EQ(outcomes.other_errors, std::size_t{0});
}

}  // namespace

TEST(test_equivalence_fixed_corpus_matches_on_both_engines) {
  CheckComparatorFollowsTheRules();
  CheckRunnerComparesTables();
  RunFixedCorpus(ReplacementPolicyKind::kLru);
  RunFixedCorpus(ReplacementPolicyKind::kTwoQ);
}

TEST(test_equivalence_known_differences_are_as_documented) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, ReplacementPolicyKind::kLru)) {
    return;
  }
  CheckLineBreakDifference(&pair);
  CheckRowLengthDifference(&pair);
  CheckDdlDifferences(&pair);
  CheckEarlyTerminationDifference(&pair);
  CheckViolationOrderDifference(&pair);
  CheckAdoptedCsvDifference();
  CheckParallelOption();
  CheckDurabilityDifference();
}

TEST(test_equivalence_randomized_statements_match) {
  RunRandomStatements({20260101, ReplacementPolicyKind::kLru, 4, 0});
  RunRandomStatements({20260202, ReplacementPolicyKind::kTwoQ, 4, 0});
  RunRandomStatements({20260303, ReplacementPolicyKind::kLru, 16, 0});
}

namespace {

// FetchPage calls made by the paged engine since the last ResetStats.
std::uint64_t FetchPageCount(paged::BufferManager& buffer) {
  const paged::BufferStats stats = buffer.Stats();
  return stats.hits + stats.misses;
}

// A table of more than 20 pages: a point SELECT of an existing key reads at
// most 2 index pages and 1 heap page, whatever the pool size, where a scan
// reads every page of the table.
void CheckPointLookupCost(ReplacementPolicyKind policy, std::size_t frames) {
  EnginePair pair;
  if (!OpenPair(&pair, frames, policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           std::string("point lookups, ") + PolicyName(policy) +
                               ", " + std::to_string(frames) + " frames");
  if (!RUN_EQUIVALENT(runner,
                      "CREATE TABLE wide (id INT PRIMARY KEY, pad TEXT)")) {
    return;
  }
  const std::string pad = "'" + std::string(400, 'p') + "'";
  constexpr int kRows = 300;
  constexpr int kRowsPerInsert = 30;
  for (int first = 1; first <= kRows; first += kRowsPerInsert) {
    std::string sql = "INSERT INTO wide VALUES ";
    for (int id = first; id < first + kRowsPerInsert; ++id) {
      sql += (id > first ? ", (" : "(") + std::to_string(id) + ", " + pad + ")";
    }
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  paged::BufferManager& buffer = pair.paged.engine->database().buffer();
  CHECK(buffer.PageCount() > 20);
  constexpr int kLookups = 20;
  for (int lookup = 0; lookup < kLookups; ++lookup) {
    const int id = 1 + lookup * (kRows / kLookups) + lookup % 7;
    buffer.ResetStats();
    if (!RUN_EQUIVALENT(
            runner, "SELECT id FROM wide WHERE id = " + std::to_string(id))) {
      return;
    }
    CHECK(FetchPageCount(buffer) <= 3);
    CHECK_EQ(
        std::get<domain::ResultSet>(runner.last_paged_result()).rows.size(),
        std::size_t{1});
  }
  // The same statement without a key range reads the whole table.
  buffer.ResetStats();
  if (!RUN_EQUIVALENT(runner, "SELECT id FROM wide WHERE id + 0 = 5")) {
    return;
  }
  CHECK(FetchPageCount(buffer) > 20);
}

}  // namespace

TEST(test_equivalence_generated_key_predicates_match_flatfile) {
  RunRandomStatements({20261101, ReplacementPolicyKind::kLru, 4, 60});
  RunRandomStatements({20261102, ReplacementPolicyKind::kTwoQ, 4, 60});
  RunRandomStatements({20261103, ReplacementPolicyKind::kLru, 16, 60});
  CheckPointLookupCost(ReplacementPolicyKind::kLru, 16);
  CheckPointLookupCost(ReplacementPolicyKind::kTwoQ, 4);
}

// ----------------------------------------------------------------------------
// The corpus of key predicates.
// ----------------------------------------------------------------------------

namespace {

// A TEXT literal of `length` characters that starts with `head`.
std::string PaddedText(const std::string& head, std::size_t length) {
  return "'" + head + std::string(length - head.size(), 'p') + "'";
}

// The table `ix` with 8 rows of 450-character labels, which fill its first
// page, and the 8 key SELECTs that every phase of the corpus repeats.
std::vector<std::string> BuildIndexCorpusStart() {
  std::vector<std::string> corpus = {
      "CREATE TABLE ix (id INT PRIMARY KEY, label TEXT, n INT)",
      "INSERT INTO ix VALUES (1, " + PaddedText("a", 450) + ", 10), (2, " +
          PaddedText("b", 450) + ", 20), (3, " + PaddedText("c", 450) +
          ", 30), (4, " + PaddedText("d", 450) + ", 40), (5, " +
          PaddedText("e", 450) + ", 50), (6, " + PaddedText("f", 450) +
          ", 60), (7, " + PaddedText("g", 450) + ", 70), (8, " +
          PaddedText("h", 450) + ", 80)"};
  return corpus;
}

void AppendKeySelects(std::vector<std::string>* corpus) {
  corpus->push_back("SELECT id, n FROM ix WHERE id = 3");
  corpus->push_back("SELECT n FROM ix WHERE 6 = id");
  corpus->push_back("SELECT id FROM ix WHERE id > 2 AND id <= 5 ORDER BY id");
  corpus->push_back("SELECT id, n FROM ix WHERE id < 4");
  corpus->push_back("SELECT id, n FROM ix WHERE id >= 7");
  corpus->push_back("SELECT id FROM ix WHERE id = 3 OR id = 7");
  corpus->push_back("SELECT id, n FROM ix WHERE id = 5 AND n > 0");
  corpus->push_back("SELECT id, n FROM ix WHERE id = 99");
}

// The statements after the first page of the corpus: an UPDATE that grows a
// label so that its row moves to another page, a delete and an insert of the
// same key, ADD COLUMN, a DROP TABLE with the table created again, and key
// literals at the edges of the INT range.
std::vector<std::string> BuildIndexCorpusRest() {
  std::vector<std::string> corpus;
  corpus.push_back("UPDATE ix SET label = " + PaddedText("moved", 900) +
                   " WHERE id = 3");
  corpus.push_back("UPDATE ix SET n = n + 1 WHERE id >= 6 AND id <= 7");
  AppendKeySelects(&corpus);
  corpus.push_back("DELETE FROM ix WHERE id = 5");
  corpus.push_back("INSERT INTO ix VALUES (5, 'e2', 55)");
  corpus.push_back("DELETE FROM ix WHERE id > 7");
  corpus.push_back("INSERT INTO ix VALUES (9, 'i', 90), (8, 'h2', 81)");
  AppendKeySelects(&corpus);
  corpus.push_back("ALTER TABLE ix ADD COLUMN extra INT DEFAULT 1");
  corpus.push_back("UPDATE ix SET extra = id * 2 WHERE id >= 6");
  corpus.push_back("DELETE FROM ix WHERE id = 2");
  AppendKeySelects(&corpus);
  const std::vector<std::string> recreated = {
      "DROP TABLE ix",
      "CREATE TABLE ix (id INT PRIMARY KEY, label TEXT, n INT)",
      "INSERT INTO ix VALUES (3, 'z', 1), (1, 'y', 2), (2, 'x', 3)",
      "SELECT * FROM ix WHERE id = 3",
      "SELECT * FROM ix WHERE id <= 2",
      "DELETE FROM ix WHERE id = 1",
      "UPDATE ix SET n = n + 1 WHERE id >= 2",
      "SELECT * FROM ix WHERE id >= 0",
      "SELECT * FROM ix WHERE id <> 3",
      "INSERT INTO ix VALUES (-9223372036854775808, 'lo', 0), "
      "(9223372036854775807, 'hi', 0)",
      "SELECT id FROM ix WHERE id = -1",
      "SELECT id FROM ix WHERE id < 9223372036854775807",
      "SELECT id FROM ix WHERE id > -1",
      "SELECT id FROM ix WHERE id >= 9223372036854775807",
      "SELECT id FROM ix WHERE id <= -9223372036854775808",
      "SELECT id FROM ix WHERE id > 1 AND id < 2",
      "UPDATE ix SET n = 0 WHERE id = 9223372036854775807"};
  corpus.insert(corpus.end(), recreated.begin(), recreated.end());
  return corpus;
}

// A conjunct that fails comes before the key conjunct. Flatfile reads every
// row, meets the row with id 3 and reports the error; paged reads only the
// rows of the key range and returns what the predicate keeps there (ADR-0052).
void CheckErrorBeforeKeyDifference(EnginePair* pair) {
  struct Case {
    const char* key_conjunct;
    std::vector<std::string> paged_ids;
  };
  const Case cases[] = {
      {"id = 5", {"5"}}, {"id = 4", {"4"}}, {"id >= 6", {"6", "7", "8"}},
      {"id < 3", {}},    {"id = 99", {}},   {"7 = id", {"7"}}};
  for (const Case& test_case : cases) {
    const BothResults results = RunBoth(
        pair, std::string("SELECT id FROM ix WHERE 10 / (id - 3) > 0 AND ") +
                  test_case.key_conjunct);
    ExpectError(results.flatfile, ErrorKind::kRuntime, "division by zero");
    CHECK_EQ(FirstColumn(results.paged), test_case.paged_ids);
  }
}

// The statements that have no difference run through the comparator; the
// phase of the UPDATE that moves a row is checked to have grown the file.
void RunIndexCorpus(ReplacementPolicyKind policy) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, policy)) {
    return;
  }
  EquivalenceRunner runner(*pair.flatfile, *pair.paged.service,
                           std::string("index corpus, ") + PolicyName(policy));
  std::vector<std::string> start = BuildIndexCorpusStart();
  AppendKeySelects(&start);
  const std::vector<std::string> rest = BuildIndexCorpusRest();
  CHECK_EQ(start.size() + rest.size(), std::size_t{60});
  for (const std::string& sql : start) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  CheckErrorBeforeKeyDifference(&pair);
  paged::BufferManager& buffer = pair.paged.engine->database().buffer();
  for (const std::string& sql : rest) {
    const bool is_moving_update = sql.starts_with("UPDATE ix SET label");
    const paged::PageId pages_before = buffer.PageCount();
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
    if (is_moving_update) {
      CHECK(buffer.PageCount() > pages_before);
    }
  }
  CHECK_EQ(IndexCheckText(pair.paged.engine->database().CheckIndex("ix")),
           std::string("ok"));
  CHECK_EQ(buffer.PinnedFrameCount(), std::size_t{0});
}

// A table of 1000 keys in 7 leaves whose first leaf and last leaf are emptied
// by deletes (a delete frees no node), read through ranges with and without a
// low bound, then filled again, then emptied altogether.
void RunEmptiedLeaves(ReplacementPolicyKind policy) {
  EnginePair pair;
  if (!OpenPair(&pair, 4, policy)) {
    return;
  }
  EquivalenceRunner runner(
      *pair.flatfile, *pair.paged.service,
      std::string("emptied leaves, ") + PolicyName(policy));
  std::vector<std::string> statements = {
      "CREATE TABLE leaves (id INT PRIMARY KEY, v INT)"};
  for (int first = 1; first <= 1000; first += 500) {
    std::string insert = "INSERT INTO leaves VALUES ";
    for (int id = first; id < first + 500; ++id) {
      insert += (id > first ? ", (" : "(") + std::to_string(id) + ", 0)";
    }
    statements.push_back(insert);
  }
  const std::vector<std::string> reads = {
      "SELECT COUNT(*) FROM leaves",
      "SELECT id FROM leaves WHERE id < 400 ORDER BY id LIMIT 3",
      "SELECT id FROM leaves WHERE id >= 700 ORDER BY id LIMIT 3",
      "SELECT COUNT(*) FROM leaves WHERE id > 100",
      "SELECT COUNT(*) FROM leaves WHERE id <= 5000",
      "SELECT id FROM leaves WHERE id >= 900",
      "SELECT id FROM leaves WHERE id = 5",
      "SELECT id FROM leaves WHERE id = 870"};
  statements.push_back("DELETE FROM leaves WHERE id <= 160");
  statements.push_back("DELETE FROM leaves WHERE id >= 850");
  statements.insert(statements.end(), reads.begin(), reads.end());
  statements.push_back(
      "INSERT INTO leaves VALUES (1, 1), (2, 2), (900, 900), (1000, 1000)");
  statements.insert(statements.end(), reads.begin(), reads.end());
  statements.push_back("DELETE FROM leaves");
  statements.push_back("SELECT COUNT(*) FROM leaves WHERE id > 0");
  statements.push_back("SELECT id FROM leaves WHERE id < 5");
  statements.push_back("INSERT INTO leaves VALUES (7, 7)");
  statements.push_back("SELECT id FROM leaves WHERE id < 100");
  for (const std::string& sql : statements) {
    if (!RUN_EQUIVALENT(runner, sql)) {
      return;
    }
  }
  CHECK_EQ(IndexCheckText(pair.paged.engine->database().CheckIndex("leaves")),
           std::string("ok"));
  CHECK_EQ(pair.paged.engine->database().buffer().PinnedFrameCount(),
           std::size_t{0});
}

}  // namespace

TEST(test_equivalence_corpus_with_index_predicates) {
  RunIndexCorpus(ReplacementPolicyKind::kLru);
  RunIndexCorpus(ReplacementPolicyKind::kTwoQ);
  RunEmptiedLeaves(ReplacementPolicyKind::kLru);
  RunEmptiedLeaves(ReplacementPolicyKind::kTwoQ);
}

}  // namespace strata::testing
