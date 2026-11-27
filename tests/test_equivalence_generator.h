#ifndef STRATA_TESTS_TEST_EQUIVALENCE_GENERATOR_H_
#define STRATA_TESTS_TEST_EQUIVALENCE_GENERATOR_H_

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "strata/domain/value.h"
#include "test_equivalence_support.h"

// The statements of the equivalence tests (docs/TestDocument.md):
// the schema and data they start from, and the random statement generator.

namespace strata::testing {

// CREATE TABLE and INSERT statements for visitors (6 rows), stories (10 rows)
// and reactions (20 rows), in an order the foreign keys allow. The data repeats
// text values, mixes DOUBLE values, and holds one pair of usernames (ann,
// annx) that a rename can chain.
const std::vector<std::string>& EquivalenceSetupStatements();

// A deterministic stream of SELECT, INSERT, UPDATE and DELETE statements over
// the tables of EquivalenceSetupStatements. The mixing probabilities are fixed
// in the implementation; a seed fixes the whole stream.
//
// The generator stays inside what design document 2.8 makes comparable:
//   - a LIMIT or OFFSET only follows an ORDER BY that ends with the primary key
//     of every table of the FROM clause (or with every group key), so the cut
//     is deterministic, and LIMIT 0 is used only by a statement that cannot
//     fail, because paged reads no row below it;
//   - an UPDATE or DELETE that can violate uniqueness or a foreign key selects
//     one row by primary key or unique key (or renames a pair of rows whose
//     outcome does not depend on the scan order), so the reported row does not
//     depend on the scan order;
//   - one statement has at most one kind of evaluation error (division by
//     zero, or numeric overflow), and at most one of AVG, MIN and MAX over an
//     input that can be empty;
//   - no TEXT with a line break, no row near 4060 bytes, no SUM or AVG over a
//     DOUBLE or over INT values near the 64-bit limit, no -0.0 in a stored
//     cell (see DoublePool), and no ALTER TABLE, DROP TABLE or table named
//     sys_*.
//
// key_percent is the share of statements that are drawn from the key
// statements instead: a SELECT, UPDATE or DELETE whose WHERE compares the INT
// primary key `id` of visitors, stories or reactions with literals (equality
// with the literal on either side, one-sided and two-sided ranges, zero,
// negative and 64-bit boundary literals, <>, OR, a DOUBLE literal, a non-key
// conjunct). An UPDATE of such a statement writes a column that no key, UNIQUE
// column or foreign key involves, and a DELETE of a referenced table selects at
// most one row, so the rule above still holds. With key_percent 0 the stream
// is the one without key statements.
class StatementGenerator {
 public:
  explicit StatementGenerator(std::uint64_t seed, std::size_t key_percent = 0);

  // The next statement. tables is the current content of every table (the
  // flat-file engine's, from EquivalenceRunner::flatfile_tables), which lets a
  // statement name rows that exist.
  std::string Next(const TableContents& tables);

 private:
  // A table of the FROM clause under its alias.
  struct Source {
    std::string alias;
    std::string table;
  };
  using Scope = std::vector<Source>;

  // The kind of evaluation error the running statement may provoke.
  enum class Risk { kNone, kOverflow, kDivision };

  // Randomness.
  std::size_t Below(std::size_t bound);
  bool Chance(std::size_t percent);
  const std::string& PickText(const std::vector<std::string>& options);

  // Values and ids that exist in the current tables.
  std::string ExistingLiteral(const std::string& table, std::size_t column,
                              const std::string& fallback);
  std::int64_t ExistingOrNewId(const std::string& table);
  std::int64_t NextFreeId(const std::string& table, std::size_t row_offset);
  std::string NewUserName();
  std::size_t RowCount();
  std::size_t LimitValue();

  // Expressions over a scope. Every one is total unless risk_ allows an error.
  std::string SmallInt(const Scope& scope);
  std::string IntExprSmall(const Scope& scope, int depth);
  std::string IntExpr(const Scope& scope, int depth);
  std::string DoubleExpr(const Scope& scope, int depth);
  std::string TextExpr(const Scope& scope, int depth);
  std::string AnyExpr(const Scope& scope);
  std::string Predicate(const Scope& scope, int depth);
  std::string CompareOperator();

  // SELECT.
  std::string Select();
  std::string FilterSelect();
  std::string JoinSelect();
  std::string AggregateSelect();
  std::string SingleRowCastSelect();
  std::string JoinFrom(std::size_t count, Scope* scope);
  std::string JoinCondition(const Scope& scope, std::size_t new_index,
                            bool is_two_table_join);
  std::string BuildSelect(const Scope& scope, const std::string& from_clause);
  std::string OrderAndLimit(const std::vector<std::size_t>& id_positions,
                            std::size_t free_count);

  // INSERT, UPDATE, DELETE.
  std::string Insert();
  std::string InsertVisitors();
  std::string InsertStories();
  std::string InsertReactions();
  std::string Update();
  std::string UpdatePlain();
  std::string UpdateText(const std::string& table,
                         const std::vector<std::string>& assignments,
                         const std::string& guard, const Scope& scope);
  std::string UpdateKey();
  std::string UpdateChainRename();
  std::string UpdateSwapColumns();
  std::string Delete();

  // Key statements (see the class comment).
  std::string KeyLiteral(const std::string& table);
  std::string KeyPredicate(const std::string& table);
  std::string NarrowKeyPredicate(const std::string& table);
  std::string SingleKeyPredicate(const std::string& table);
  std::string KeySelect(const std::string& table);
  std::string KeyUpdate(const std::string& table);
  std::string KeyDelete(const std::string& table);
  std::string KeyStatement();

  std::mt19937_64 engine_;
  std::size_t key_percent_;
  const TableContents* tables_ = nullptr;
  Risk risk_ = Risk::kNone;
  std::size_t next_name_number_ = 0;
};

}  // namespace strata::testing

#endif  // STRATA_TESTS_TEST_EQUIVALENCE_GENERATOR_H_
