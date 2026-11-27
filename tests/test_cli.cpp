#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "strata/directory_lock.h"
#include "test_support.h"

#ifndef STRATA_SOURCE_DIR
#error "STRATA_SOURCE_DIR must be defined by the build"
#endif
#ifndef STRATA_CLI_PATH
#error "STRATA_CLI_PATH must be defined by the build"
#endif

// Environment tier: runs the strata_cli executable against a copy of the
// sample database sample_my_db/. Each test owns a work directory. Most of them
// hold a copy of sample_my_db/ whose students.csv has three lines that the
// schema rejects (see StudentsCsvWithRejectedLines); the sample database tests
// copy it unchanged, and a few tests use an empty directory.

namespace {

namespace testing = strata::testing;

const char* const kTableFiles[] = {"instructors.csv", "students.csv",
                                   "courses.csv", "enrollments.csv"};
const char* const kOpenLine = ".open sample_my_db";
// `.open` reports the rejected lines of students.csv.
const char* const kLoadedLine =
    "loaded instructors=6 students=30 (rejected 3) courses=12 enrollments=144";
const char* const kRejectedLine = "students.csv: rejected lines 12, 17, 18";

// The sample database that ships with the repository (README, Quick Start).
std::filesystem::path SampleDatabase() {
  return std::filesystem::path(STRATA_SOURCE_DIR) / "sample_my_db";
}

std::vector<std::string> SplitLines(const std::string& text);

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line + "\n";
  }
  return text;
}

// The students.csv of the repository with three lines added. `lines[0]` is the
// header and `lines[k]` the student in row k, so a line number is its index
// + 1. Line 12 is row 10 with an id that is not a number, and lines 17 and 18
// are row 15 with a semicolon in place of its last comma (4 fields where the
// schema has 5) and with a sixth field.
std::string StudentsCsvWithRejectedLines() {
  std::vector<std::string> lines =
      SplitLines(testing::ReadFile(SampleDatabase() / "students.csv"));
  if (lines.size() < 16) {
    return "";
  }
  const std::string bad_id = "x" + lines[10];
  std::string four_fields = lines[15];
  four_fields[four_fields.rfind(',')] = ';';
  const std::string six_fields = lines[15] + ",spare";
  lines.insert(lines.begin() + 11, bad_id);
  lines.insert(lines.begin() + 16, four_fields);
  lines.insert(lines.begin() + 17, six_fields);
  return JoinLines(lines);
}

// The bytes a table file has before a test touches it.
std::string ExpectedContent(const std::string& name) {
  if (name == "students.csv") {
    return StudentsCsvWithRejectedLines();
  }
  return testing::ReadFile(SampleDatabase() / name);
}

// Whether the table files in work_dir/sample_my_db still have their original
// bytes.
bool DataFilesUnchanged(const std::filesystem::path& work_dir) {
  for (const char* name : kTableFiles) {
    if (ExpectedContent(name) !=
        testing::ReadFile(work_dir / "sample_my_db" / name)) {
      return false;
    }
  }
  return true;
}

// Copies sample_my_db/ to work_dir/sample_my_db, puts the students.csv with the
// rejected lines in it, and checks the copy. should_keep_catalog false removes
// catalog.csv, so that the copy is a directory of bare CSV files.
bool PrepareWorkDir(const std::filesystem::path& work_dir,
                    bool should_keep_catalog = true) {
  std::error_code error;
  std::filesystem::copy(SampleDatabase(), work_dir / "sample_my_db",
                        std::filesystem::copy_options::recursive, error);
  if (error) {
    return false;
  }
  testing::WriteFile(work_dir / "sample_my_db" / "students.csv",
                     StudentsCsvWithRejectedLines());
  if (!should_keep_catalog) {
    std::filesystem::remove(work_dir / "sample_my_db" / "catalog.csv", error);
  }
  return !error && DataFilesUnchanged(work_dir);
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

// Feeds input_lines to the CLI on stdin with work_dir as current directory and
// returns what it printed.
std::vector<std::string> RunCli(const std::filesystem::path& work_dir,
                                const std::vector<std::string>& input_lines) {
  std::string input;
  for (const auto& line : input_lines) {
    input += line + "\n";
  }
  testing::WriteFile(work_dir / "input.txt", input);
  std::string command = "cd '" + work_dir.string() + "' && '" +
                        STRATA_CLI_PATH + "' < input.txt > output.txt 2>&1";
  int status = std::system(command.c_str());
  if (status != 0) {
    strata::testing::ReportFailure(
        __FILE__, __LINE__, "cli exited with status " + std::to_string(status));
  }
  return SplitLines(testing::ReadFile(work_dir / "output.txt"));
}

// What one run of the CLI with command-line arguments printed and returned.
struct CliRun {
  std::vector<std::string> output;
  std::string error_text;
  int status;
};

// Runs the CLI with `arguments` (already shell-quoted), stdin from input_lines
// and work_dir as the current directory.
CliRun RunCliWithArguments(const std::filesystem::path& work_dir,
                           const std::string& arguments,
                           const std::vector<std::string>& input_lines) {
  std::string input;
  for (const auto& line : input_lines) {
    input += line + "\n";
  }
  testing::WriteFile(work_dir / "input.txt", input);
  std::string command = "cd '" + work_dir.string() + "' && '" +
                        STRATA_CLI_PATH + "' " + arguments +
                        " < input.txt > output.txt 2> error.txt";
  int raw_status = std::system(command.c_str());
  CliRun run;
  run.status = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
  run.output = SplitLines(testing::ReadFile(work_dir / "output.txt"));
  run.error_text = testing::ReadFile(work_dir / "error.txt");
  return run;
}

// Output lines after the `loaded ...` line of the leading .open.
std::vector<std::string> RunAfterOpen(const std::filesystem::path& work_dir,
                                      std::vector<std::string> statements) {
  std::vector<std::string> input = {kOpenLine};
  for (auto& statement : statements) {
    input.push_back(std::move(statement));
  }
  std::vector<std::string> output = RunCli(work_dir, input);
  // The `loaded ...` line and the line that lists the rejected rows.
  const std::size_t opening_lines = std::min<std::size_t>(2, output.size());
  output.erase(output.begin(), output.begin() + opening_lines);
  return output;
}

std::vector<std::string> Sorted(std::vector<std::string> lines) {
  std::ranges::sort(lines);
  return lines;
}

const char* const kFilteredJoinQuery =
    "SELECT e.course_id, e.score FROM enrollments e JOIN students s ON "
    "e.student_id = s.id "
    "WHERE s.id = 1001 AND e.status = 'completed' ORDER BY e.course_id, "
    "e.score;";
const char* const kGroupedJoinQuery =
    "SELECT e.status, COUNT(*) FROM enrollments e JOIN students s ON "
    "e.student_id = s.id "
    "WHERE s.major = 'ART' AND (e.status = 'completed' OR e.status = "
    "'enrolled') "
    "GROUP BY e.status;";

}  // namespace

TEST(test_cli_open_prints_loaded_counts) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {std::string(kOpenLine) + " --parallel"});
  CHECK_EQ(output, std::vector<std::string>({kLoadedLine, kRejectedLine}));
}

TEST(test_cli_select_prints_header_and_rows) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(), {"SELECT name, major FROM students WHERE id = 1001;"});
  CHECK_EQ(output,
           std::vector<std::string>({"name\tmajor", "Alice Johnson\tART"}));
}

TEST(test_cli_filtered_join_prints_example_rows) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {kFilteredJoinQuery});
  CHECK_EQ(output, std::vector<std::string>(
                       {"course_id\tscore", "4\t73", "9\t81", "12\t83"}));
}

TEST(test_cli_group_by_prints_example_groups) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {kGroupedJoinQuery});
  CHECK_EQ(output.size(), std::size_t{3});
  if (output.size() != 3) {
    return;
  }
  CHECK_EQ(output[0], std::string("status\tCOUNT(*)"));
  CHECK_EQ(Sorted({output[1], output[2]}),
           Sorted({"completed\t14", "enrolled\t18"}));
}

TEST(test_cli_empty_text_group_prints_empty_field) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"INSERT INTO instructors (name, dept) VALUES ('Dr. Blank', '');",
       "SELECT dept, COUNT(*) FROM instructors WHERE dept = '' OR dept = "
       "'CS' GROUP BY dept;"});
  CHECK_EQ(output.size(), std::size_t{4});
  if (output.size() != 4) {
    return;
  }
  CHECK_EQ(output[0], std::string("OK affected=1"));
  CHECK_EQ(output[1], std::string("dept\tCOUNT(*)"));
  CHECK_EQ(Sorted({output[2], output[3]}), Sorted({"CS\t1", "\t1"}));
}

TEST(test_cli_write_statements_print_affected_rows) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"INSERT INTO enrollments (student_id, course_id, semester) VALUES "
       "(1002, 1, '2026-Spring');",
       "UPDATE courses SET capacity = capacity + 1 WHERE id = 1;",
       "DELETE FROM enrollments WHERE id = 145;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"OK affected=1", "OK affected=1", "OK affected=1"}));
}

TEST(test_cli_delete_referenced_student_prints_constraint_error) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"DELETE FROM students WHERE id = 1001;"});
  CHECK_EQ(output,
           std::vector<std::string>(
               {"error: constraint violation: students.id=1001 is referenced "
                "by enrollments.student_id"}));
}

TEST(test_cli_three_table_join_prints_rows) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT s.name, COUNT(*) FROM students s JOIN enrollments e ON "
       "e.student_id = s.id JOIN courses c ON e.course_id = c.id WHERE s.id = "
       "1005 GROUP BY s.name;"});
  CHECK_EQ(output,
           std::vector<std::string>({"name\tCOUNT(*)", "Emma Brown\t6"}));
}

TEST(test_cli_syntax_error_prints_column_and_description) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELEC name FROM students;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"syntax error at column 1: unrecognized statement"}));
}

TEST(test_cli_sql_before_open_prints_error) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {"SELECT name FROM students WHERE id = 1001;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"error: no database open; run .open first"}));
}

TEST(test_cli_failed_open_keeps_previous_connection) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {kOpenLine, ".open nosuch/db",
                           "SELECT name FROM students WHERE id = 1001;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {kLoadedLine, kRejectedLine,
                        "error: could not open nosuch/db: no such directory",
                        "name", "Alice Johnson"}));
}

TEST(test_cli_formats_double_empty_result_and_blank_line) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id, id + 0.5, id + 1.0 FROM students WHERE id = 1001;", "",
       "SELECT id FROM students WHERE id = 0;"});
  CHECK_EQ(output, std::vector<std::string>({"id\tid + 0.5\tid + 1.0",
                                             "1001\t1001.5\t1002.0", "id"}));
}

TEST(test_cli_bind_error_prints_bind_kind) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELECT nosuch FROM students;"});
  CHECK_EQ(output.size(), std::size_t{1});
  if (output.empty()) {
    return;
  }
  CHECK(output[0].starts_with("error: bind:"));
}

TEST(test_cli_write_failure_prints_io_error) {
  if (::geteuid() == 0) {
    SKIP("running as root: directory permissions do not apply");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::error_code permission_error;
  std::filesystem::permissions(
      work.path() / "sample_my_db",
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace, permission_error);
  CHECK(!permission_error);
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"UPDATE courses SET capacity = capacity + 1 WHERE id = 1;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"error: io: could not write courses.csv"}));
  CHECK(testing::ReadFile(work.path() / "sample_my_db" / "courses.csv") ==
        testing::ReadFile(SampleDatabase() / "courses.csv"));
}

TEST(test_cli_integer_overflow_prints_runtime_error) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id + 9223372036854775807 FROM students WHERE id = 1001;"});
  CHECK_EQ(output,
           std::vector<std::string>({"error: runtime: numeric overflow"}));
}

TEST(test_cli_open_with_wrong_arguments_prints_usage) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {".open", ".open --parallel", ".open --other"});
  const std::string usage =
      "syntax error at column 1: usage: .open <directory> [--parallel]";
  CHECK_EQ(output, std::vector<std::string>({usage, usage, usage}));
}

TEST(test_cli_create_tables_adopts_existing_csvs) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> first = RunCli(
      work.path(),
      {".open sample_my_db",
       "CREATE TABLE instructors (id INT PRIMARY KEY AUTOINCREMENT, name TEXT, "
       "dept TEXT);",
       "CREATE TABLE students (id INT PRIMARY KEY, name TEXT, major TEXT, year "
       "INT, email TEXT UNIQUE);",
       "CREATE TABLE courses (id INT PRIMARY KEY AUTOINCREMENT, code TEXT "
       "UNIQUE, title TEXT, credits INT, dept TEXT, capacity INT, "
       "instructor_id INT REFERENCES instructors(id));",
       "CREATE TABLE enrollments (id INT PRIMARY KEY AUTOINCREMENT, student_id "
       "INT REFERENCES students(id), course_id INT REFERENCES courses(id), "
       "semester TEXT, status TEXT DEFAULT 'enrolled', score INT DEFAULT 0);",
       "SELECT name, major FROM students WHERE id = 1001;"});
  std::vector<std::string> second =
      RunCli(work.path(),
             {kOpenLine, "SELECT name, major FROM students WHERE id = 1001;"});

  CHECK_EQ(first, std::vector<std::string>(
                      {"loaded no tables",
                       "OK CREATE TABLE instructors (adopted 6 rows)",
                       "OK CREATE TABLE students (adopted 30 rows)",
                       "OK CREATE TABLE courses (adopted 12 rows)",
                       "OK CREATE TABLE enrollments (adopted 144 rows)",
                       "name\tmajor", "Alice Johnson\tART"}));
  CHECK_EQ(second,
           std::vector<std::string>({kLoadedLine, kRejectedLine, "name\tmajor",
                                     "Alice Johnson\tART"}));
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_ddl_statements_print_command_complete) {
  testing::TempDir work;
  std::filesystem::create_directory(work.path() / "db");

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open db",
       "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT "
       "NOT NULL, owner TEXT DEFAULT 'none');",
       "INSERT INTO notes (body) VALUES ('a, \"quoted\" text');",
       "ALTER TABLE notes ADD COLUMN pinned INT DEFAULT 0;",
       "SELECT id, body, owner, pinned FROM notes;", "DROP TABLE notes;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded no tables", "OK CREATE TABLE notes", "OK affected=1",
                "OK ALTER TABLE notes", "id\tbody\towner\tpinned",
                "1\ta, \"quoted\" text\tnone\t0", "OK DROP TABLE notes"}));
}

TEST(test_cli_ddl_errors_print_bind_error) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"CREATE TABLE students (id INT PRIMARY KEY);",
                                 "CREATE TABLE catalog (id INT PRIMARY KEY);",
                                 "DROP TABLE students;",
                                 "ALTER TABLE students ADD COLUMN x INT;"});

  CHECK_EQ(output.size(), std::size_t{4});
  if (output.size() != 4) {
    return;
  }
  CHECK_EQ(output[0],
           std::string("error: bind: table students already exists"));
  CHECK_EQ(output[1],
           std::string("error: bind: table name catalog is reserved"));
  CHECK_EQ(output[2], std::string("error: bind: table students is referenced "
                                  "by enrollments.student_id"));
  CHECK(output[3].starts_with("error: bind:"));
}

TEST(test_cli_open_locked_directory_prints_error) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> while_locked;
  {
    strata::infrastructure::LockOutcome held =
        strata::infrastructure::DirectoryLock::Acquire(
            (work.path() / "sample_my_db").string());
    CHECK(held.lock != nullptr);
    while_locked = RunCli(work.path(), {kOpenLine});
  }

  std::vector<std::string> after_release = RunCli(work.path(), {kOpenLine});

  CHECK_EQ(while_locked,
           std::vector<std::string>(
               {"error: could not open sample_my_db: database is locked by "
                "another process"}));
  CHECK_EQ(after_release,
           std::vector<std::string>({kLoadedLine, kRejectedLine}));
}

TEST(test_cli_open_same_directory_twice_succeeds) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunCli(
      work.path(),
      {kOpenLine, kOpenLine, "SELECT name FROM students WHERE id = 1001;"});

  CHECK_EQ(output,
           std::vector<std::string>({kLoadedLine, kRejectedLine, kLoadedLine,
                                     kRejectedLine, "name", "Alice Johnson"}));
}

TEST(test_cli_text_with_comma_and_quote_round_trips) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> first = RunAfterOpen(
      work.path(),
      {"INSERT INTO instructors (name, dept) VALUES ('Dr. \"Q\", Jr.', "
       "'CS');",
       "SELECT name FROM instructors WHERE name LIKE 'Dr. \"Q\"%';"});
  std::vector<std::string> second = RunAfterOpen(
      work.path(), {"SELECT name FROM instructors WHERE name LIKE 'Dr. "
                    "\"Q\"%';"});

  CHECK_EQ(first, std::vector<std::string>(
                      {"OK affected=1", "name", "Dr. \"Q\", Jr."}));
  CHECK_EQ(second, std::vector<std::string>({"name", "Dr. \"Q\", Jr."}));
}

TEST(test_cli_multi_line_statement_prints_result) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT name,", "  major", "FROM students", "WHERE id = 1001;"});

  CHECK_EQ(output,
           std::vector<std::string>({"name\tmajor", "Alice Johnson\tART"}));
}

TEST(test_cli_schema_output_can_be_replayed) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::filesystem::create_directory(work.path() / "fresh");

  std::vector<std::string> original_schema =
      RunAfterOpen(work.path(), {".schema"});
  testing::WriteFile(work.path() / "schema.sql", JoinLines(original_schema));
  std::vector<std::string> replay_input = {".open fresh"};
  replay_input.insert(replay_input.end(), original_schema.begin(),
                      original_schema.end());
  replay_input.push_back(".schema");
  std::vector<std::string> replay = RunCli(work.path(), replay_input);

  const std::size_t table_count = 4;
  CHECK_EQ(replay.size(), 1 + table_count + original_schema.size());
  if (replay.size() != 1 + table_count + original_schema.size()) {
    return;
  }
  CHECK_EQ(replay[0], std::string("loaded no tables"));
  CHECK_EQ(replay[1], std::string("OK CREATE TABLE instructors"));
  CHECK_EQ(replay[2], std::string("OK CREATE TABLE students"));
  CHECK_EQ(replay[3], std::string("OK CREATE TABLE courses"));
  CHECK_EQ(replay[4], std::string("OK CREATE TABLE enrollments"));
  CHECK_EQ(
      std::vector<std::string>(replay.begin() + 1 + table_count, replay.end()),
      original_schema);
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_piped_input_prints_no_prompt) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELECT name FROM students WHERE id = 1001;"});
  std::string raw = testing::ReadFile(work.path() / "output.txt");

  CHECK_EQ(output, std::vector<std::string>({"name", "Alice Johnson"}));
  CHECK_EQ(raw, std::string(kLoadedLine) + "\n" + kRejectedLine +
                    "\nname\nAlice Johnson\n");
  CHECK(raw.find("strata>") == std::string::npos);
}

TEST(test_cli_quit_stops_reading_with_status_zero) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  // RunCli reports a failure when the exit status is not 0.
  std::vector<std::string> output = RunCli(
      work.path(),
      {kOpenLine, ".quit", "SELECT name FROM students WHERE id = 1001;"});

  CHECK_EQ(output, std::vector<std::string>({kLoadedLine, kRejectedLine}));
}

TEST(test_cli_limit_offset_prints_window) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id, name FROM students ORDER BY id LIMIT 3 OFFSET 1;"});

  CHECK_EQ(output,
           std::vector<std::string>({"id\tname", "1002\tBob Smith",
                                     "1003\tCarol Davis", "1004\tDavid Kim"}));
}

TEST(test_cli_aggregates_print_min_max_avg) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT COUNT(*), MIN(capacity), MAX(capacity), AVG(capacity) FROM "
       "courses;"});

  CHECK_EQ(output, std::vector<std::string>(
                       {"COUNT(*)\tMIN(capacity)\tMAX(capacity)\tAVG(capacity)",
                        "12\t4\t10\t6.916666666666667"}));
}

TEST(test_cli_like_and_functions_print_example_rows) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id, UPPER(name), LENGTH(name) FROM students WHERE name LIKE "
       "'s%' ORDER BY id LIMIT 2;"});

  CHECK_EQ(output, std::vector<std::string>({"id\tUPPER(name)\tLENGTH(name)",
                                             "1019\tSAMUEL INGRAM\t13",
                                             "1028\tSOPHIE RIVERA\t13"}));
}

TEST(test_cli_new_syntax_errors_print_messages) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT 1 / 0 FROM students WHERE id = 1001;",
       "SELECT AVG(id) FROM students WHERE id = 0;",
       "SELECT CAST(name AS INT) FROM students WHERE id = 1001;",
       "SELECT id FROM students LIMIT -1;", "SELECT FOO(1) FROM students;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"error: runtime: division by zero",
                "error: runtime: AVG over no rows",
                "error: runtime: cannot cast 'Alice Johnson' to INT",
                "syntax error at column 31: LIMIT requires a non-negative "
                "integer literal",
                "syntax error at column 8: unknown function FOO"}));
}

TEST(test_cli_multi_row_insert_persists_and_fails_as_a_whole) {
  testing::TempDir work;
  std::filesystem::create_directory(work.path() / "db");

  std::vector<std::string> first = RunCli(
      work.path(),
      {".open db",
       "CREATE TABLE notes (id INT PRIMARY KEY AUTOINCREMENT, body TEXT);",
       "INSERT INTO notes (body) VALUES ('a'), ('b'), ('c');",
       "INSERT INTO notes (id, body) VALUES (10, 'x'), (10, 'y');"});
  std::vector<std::string> second = RunCli(
      work.path(), {".open db", "SELECT id, body FROM notes ORDER BY id;"});

  CHECK_EQ(first,
           std::vector<std::string>(
               {"loaded no tables", "OK CREATE TABLE notes", "OK affected=3",
                "error: constraint violation: duplicate key: "
                "notes.id=10"}));
  CHECK_EQ(second, std::vector<std::string>(
                       {"loaded notes=3", "id\tbody", "1\ta", "2\tb", "3\tc"}));
}

TEST(test_cli_sample_database_answers_readme_queries) {
  testing::TempDir work;
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open sample_my_db", ".tables", ".schema courses",
       "SELECT code, title, credits, capacity FROM courses ORDER BY code LIMIT "
       "4;",
       "SELECT code, COUNT(*) FROM courses JOIN enrollments ON course_id = "
       "courses.id WHERE semester = '2026-Spring' AND status = 'enrolled' "
       "GROUP BY code ORDER BY 2 DESC, 1 LIMIT 3;",
       "SELECT name, code, score FROM students JOIN enrollments ON student_id "
       "= students.id JOIN courses ON course_id = courses.id WHERE semester = "
       "'2025-Fall' AND status = 'completed' ORDER BY 3 DESC, 1 LIMIT 3;",
       "SELECT id, name, LENGTH(name), UPPER(major) FROM students WHERE name "
       "LIKE 'K%';"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "instructors",
                "students",
                "courses",
                "enrollments",
                "CREATE TABLE courses (",
                "  id INT PRIMARY KEY AUTOINCREMENT,",
                "  code TEXT UNIQUE,",
                "  title TEXT,",
                "  credits INT,",
                "  dept TEXT,",
                "  capacity INT,",
                "  instructor_id INT REFERENCES instructors(id)",
                ");",
                "code\ttitle\tcredits\tcapacity",
                "ART101\tArt Appreciation\t2\t10",
                "CS101\tIntro to Programming\t3\t6",
                "CS201\tData Structures\t3\t5",
                "CS301\tDatabase Systems\t3\t5",
                "code\tCOUNT(*)",
                "HIST101\t8",
                "STAT101\t8",
                "ECON101\t7",
                "name\tcode\tscore",
                "Ivy Chen\tCS301\t97",
                "Wendy Morgan\tHIST101\t97",
                "Frank Miller\tCS301\t95",
                "id\tname\tLENGTH(name)\tUPPER(major)",
                "1011\tKevin Anderson\t14\tCS"}));
}

TEST(test_cli_sample_database_constraints_and_update_work_on_a_copy) {
  testing::TempDir work;
  const std::string source_enrollments =
      testing::ReadFile(SampleDatabase() / "enrollments.csv");
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open sample_my_db",
       "SELECT id, student_id, course_id, status, score FROM enrollments "
       "WHERE id = 100;",
       "INSERT INTO enrollments (student_id, course_id, semester) VALUES "
       "(9999, 1, '2026-Spring');",
       "DELETE FROM students WHERE id = 1001;",
       "UPDATE enrollments SET status = 'completed', score = 90 WHERE id = "
       "100;",
       "SELECT id, student_id, course_id, status, score FROM enrollments "
       "WHERE id = 100;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "id\tstudent_id\tcourse_id\tstatus\tscore",
                "100\t1008\t3\tenrolled\t0",
                "error: constraint violation: no matching row: "
                "enrollments.student_id=9999 not found in students.id",
                "error: constraint violation: students.id=1001 is referenced "
                "by enrollments.student_id",
                "OK affected=1", "id\tstudent_id\tcourse_id\tstatus\tscore",
                "100\t1008\t3\tcompleted\t90"}));
  CHECK(testing::ReadFile(work.path() / "sample_my_db" / "enrollments.csv") !=
        source_enrollments);
  CHECK(testing::ReadFile(SampleDatabase() / "enrollments.csv") ==
        source_enrollments);
}

TEST(test_cli_database_path_argument_opens_the_database) {
  testing::TempDir work;
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  CliRun existing =
      RunCliWithArguments(work.path(), "sample_my_db", {".tables"});
  CliRun created =
      RunCliWithArguments(work.path(), "fresh_db --parallel", {".tables"});

  CHECK_EQ(existing.status, 0);
  CHECK_EQ(existing.output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "instructors", "students", "courses", "enrollments"}));
  CHECK_EQ(created.status, 0);
  CHECK_EQ(created.output,
           std::vector<std::string>({"created new database fresh_db"}));
  CHECK(std::filesystem::is_directory(work.path() / "fresh_db"));
}

TEST(test_cli_bad_arguments_print_usage_and_exit_with_status_two) {
  testing::TempDir work;

  CliRun too_many = RunCliWithArguments(work.path(), "a b c", {".tables"});
  CliRun bad_option =
      RunCliWithArguments(work.path(), "a --bogus", {".tables"});
  CliRun option_first =
      RunCliWithArguments(work.path(), "--parallel", {".tables"});
  // A single-dash word is an option too, not a directory to create.
  CliRun short_option = RunCliWithArguments(work.path(), "-h", {".tables"});

  for (const CliRun* run :
       {&too_many, &bad_option, &option_first, &short_option}) {
    CHECK_EQ(run->status, 2);
    CHECK_EQ(run->error_text,
             std::string("usage: strata_cli [<directory> [--parallel]]\n"));
    CHECK(run->output.empty());
  }
  CHECK(!std::filesystem::exists(work.path() / "a"));
  CHECK(!std::filesystem::exists(work.path() / "-h"));
}

TEST(test_cli_import_loads_courses_and_enrollments) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open fresh", ".import sample_my_db/courses.csv",
       ".import sample_my_db/enrollments.csv", "SELECT COUNT(*) FROM courses;",
       "SELECT COUNT(*) FROM enrollments;", ".schema courses"});

  CHECK_EQ(
      output,
      std::vector<std::string>(
          {"created new database fresh", "OK IMPORT courses (imported 12 rows)",
           "OK IMPORT enrollments (imported 144 rows)", "COUNT(*)", "12",
           "COUNT(*)", "144", "CREATE TABLE courses (", "  id INT PRIMARY KEY,",
           "  code TEXT,", "  title TEXT,", "  credits INT,", "  dept TEXT,",
           "  capacity INT,", "  instructor_id INT", ");"}));
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_import_students_needs_type_and_skip_options) {
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open fresh", ".import sample_my_db/students.csv",
       ".import sample_my_db/students.csv students --pk id --type id=INT "
       "--skip-bad-rows",
       "SELECT COUNT(*) FROM students;",
       "SELECT name FROM students WHERE id = 1001;"});

  CHECK_EQ(output, std::vector<std::string>(
                       {"created new database fresh",
                        "error: bind: line 17: expected 5 fields, found 4",
                        "OK IMPORT students (imported 30 rows, skipped 3)",
                        "skipped line 12: id 'x1010' is not INT",
                        "skipped line 17: expected 5 fields, found 4",
                        "skipped line 18: expected 5 fields, found 6",
                        "COUNT(*)", "30", "name", "Alice Johnson"}));
  CHECK(DataFilesUnchanged(work.path()));
}
