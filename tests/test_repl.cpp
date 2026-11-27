#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "strata/paged/btree_node.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/slotted_page.h"
#include "strata/repl.h"
#include "strata/sql_parser.h"
#include "test_support.h"

// Unit and Integration tiers for the REPL (docs/TestDocument.md sections 3.1
// and 3.2): RunRepl runs over string streams, so statement splitting, commands,
// prompts and `.schema` are checked without a terminal.

namespace {

namespace adapter = strata::adapter;
namespace cli = strata::cli;
namespace domain = strata::domain;
namespace testing = strata::testing;

using Lines = std::vector<std::string>;

const char* const kNoDatabaseLine = "error: no database open; run .open first";
const char* const kFixtureALoaded = "loaded visitors=4 stories=3 reactions=5";
const char* const kIncompleteLine =
    "error: incomplete statement at end of input";

const char* const kVisitorsCreate =
    "CREATE TABLE visitors (\n"
    "  id INT PRIMARY KEY,\n"
    "  username TEXT UNIQUE,\n"
    "  location TEXT\n"
    ");";
const char* const kStoriesCreate =
    "CREATE TABLE stories (\n"
    "  id INT PRIMARY KEY,\n"
    "  content TEXT,\n"
    "  username TEXT REFERENCES visitors(username),\n"
    "  views INT\n"
    ");";
const char* const kReactionsCreate =
    "CREATE TABLE reactions (\n"
    "  id INT PRIMARY KEY,\n"
    "  storyId INT REFERENCES stories(id),\n"
    "  username TEXT REFERENCES visitors(username),\n"
    "  type TEXT,\n"
    "  comment TEXT,\n"
    "  timestamp INT\n"
    ");";

std::string Joined(const Lines& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line + "\n";
  }
  return text;
}

Lines SplitLines(const std::string& text) {
  Lines lines;
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

// Runs the REPL over input_lines and returns everything it wrote.
std::string RunText(const Lines& input_lines, bool is_interactive = false) {
  std::istringstream input(Joined(input_lines));
  std::ostringstream output;
  cli::RunRepl(input, output, is_interactive);
  return output.str();
}

Lines Run(const Lines& input_lines) { return SplitLines(RunText(input_lines)); }

// Runs the REPL with a database to open first.
Lines RunWithInitialOpen(const cli::InitialOpen& initial,
                         const Lines& input_lines) {
  std::istringstream input(Joined(input_lines));
  std::ostringstream output;
  cli::RunRepl(input, output, false, initial);
  return SplitLines(output.str());
}

std::string OpenLine(const std::filesystem::path& directory) {
  return ".open " + directory.string();
}

std::string ParseErrorText(const std::string& statement, std::size_t column) {
  return cli::FormatParseError(statement, adapter::ParseError{column, "d"});
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.starts_with(prefix);
}

// How many lines start with marker followed by a space.
int CountLinesStartingWith(const Lines& lines, const std::string& marker) {
  int count = 0;
  for (const std::string& line : lines) {
    if (line.starts_with(marker + " ")) {
      ++count;
    }
  }
  return count;
}

std::vector<domain::TableSchema> FakeSchemas() {
  testing::FakeCatalog catalog;
  std::vector<domain::TableSchema> schemas;
  for (const std::string& name : catalog.TableNames()) {
    schemas.push_back(*catalog.FindTable(name));
  }
  return schemas;
}

const domain::TableSchema& SchemaNamed(
    const std::vector<domain::TableSchema>& schemas, const std::string& name) {
  for (const domain::TableSchema& schema : schemas) {
    if (schema.name == name) {
      return schema;
    }
  }
  return schemas.front();
}

}  // namespace

TEST(test_repl_parse_error_position_is_reported_per_line) {
  CHECK_EQ(ParseErrorText("SELEC x;", 1),
           std::string("syntax error at column 1: d"));
  CHECK_EQ(ParseErrorText("SELECT FROM\nt;", 8),
           std::string("syntax error at column 8: d"));
  CHECK_EQ(ParseErrorText("SELECT a\nFROM ;", 15),
           std::string("syntax error at line 2, column 6: d"));
  CHECK_EQ(ParseErrorText("SELECT a\nFROM", 14),
           std::string("syntax error at line 2, column 5: d"));
}

TEST(test_repl_format_create_table_renders_every_constraint) {
  const std::vector<domain::TableSchema> schemas = FakeSchemas();

  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "visitors"), schemas),
           std::string(kVisitorsCreate));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "stories"), schemas),
           std::string(kStoriesCreate));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "drafts"), schemas),
           std::string("CREATE TABLE drafts (\n"
                       "  id INT PRIMARY KEY,\n"
                       "  body TEXT,\n"
                       "  owner TEXT DEFAULT 'none',\n"
                       "  score DOUBLE DEFAULT 1.0\n"
                       ");"));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "jobs"), schemas),
           std::string("CREATE TABLE jobs (\n"
                       "  id INT PRIMARY KEY AUTOINCREMENT,\n"
                       "  body TEXT\n"
                       ");"));

  domain::ColumnDef note{"note", domain::DataType::kText,
                         domain::Value(std::string("it's"))};
  domain::TableSchema quoted{
      "quoted", {domain::ColumnDef{"id", domain::DataType::kInt}, note}, 0, {}};
  CHECK_EQ(cli::FormatCreateTable(quoted, {quoted}),
           std::string("CREATE TABLE quoted (\n"
                       "  id INT PRIMARY KEY,\n"
                       "  note TEXT DEFAULT 'it''s'\n"
                       ");"));
}

TEST(test_repl_multi_line_statement_executes_once_complete) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output = Run({OpenLine(dir.path()), "SELECT username", "FROM visitors",
                      "WHERE id = 1;"});

  CHECK_EQ(output, Lines({kFixtureALoaded, "username", "alice"}));
}

TEST(test_repl_incomplete_statement_at_end_of_input_reports_error) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines unfinished = Run({OpenLine(dir.path()), "SELECT 1"});
  Lines open_literal = Run({OpenLine(dir.path()), "SELECT 'a;"});

  CHECK_EQ(unfinished, Lines({kFixtureALoaded, kIncompleteLine}));
  CHECK_EQ(open_literal, Lines({kFixtureALoaded, kIncompleteLine}));
}

TEST(test_repl_commands_start_only_at_the_beginning_of_a_statement) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output =
      Run({OpenLine(dir.path()), "SELECT 1", ".quit", ";", ".tables"});

  CHECK_EQ(output.size(), std::size_t{5});
  if (output.size() != 5) {
    return;
  }
  CHECK_EQ(output[0], std::string(kFixtureALoaded));
  CHECK(StartsWith(output[1], "syntax error at line 2, column 1: "));
  CHECK_EQ(Lines(output.begin() + 2, output.end()),
           Lines({"visitors", "stories", "reactions"}));
}

TEST(test_repl_tables_lists_tables_in_catalog_order) {
  testing::TempDir fixture;
  testing::WriteFixtureA(fixture.path());
  testing::TempDir empty;

  Lines with_tables = Run({OpenLine(fixture.path()), ".tables"});
  Lines without_tables = Run({OpenLine(empty.path()), ".tables"});

  CHECK_EQ(with_tables,
           Lines({kFixtureALoaded, "visitors", "stories", "reactions"}));
  CHECK_EQ(without_tables, Lines({"loaded no tables"}));
}

TEST(test_repl_schema_prints_executable_statements) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines lower = Run({OpenLine(dir.path()), ".schema stories"});
  Lines upper = Run({OpenLine(dir.path()), ".schema STORIES"});
  Lines every = Run({OpenLine(dir.path()), ".schema"});
  Lines missing = Run({OpenLine(dir.path()), ".schema nosuch"});

  const Lines stories_lines = SplitLines(std::string(kStoriesCreate) + "\n");
  Lines expected_stories = {kFixtureALoaded};
  expected_stories.insert(expected_stories.end(), stories_lines.begin(),
                          stories_lines.end());
  CHECK_EQ(lower, expected_stories);
  CHECK_EQ(upper, expected_stories);

  Lines expected_every = {kFixtureALoaded};
  for (const char* text : {kVisitorsCreate, kStoriesCreate, kReactionsCreate}) {
    const Lines table_lines = SplitLines(std::string(text) + "\n");
    expected_every.insert(expected_every.end(), table_lines.begin(),
                          table_lines.end());
  }
  CHECK_EQ(every, expected_every);
  CHECK_EQ(missing, Lines({kFixtureALoaded, "error: no such table: nosuch"}));
}

TEST(test_repl_schema_round_trip_recreates_the_same_schema) {
  testing::TempDir original;
  testing::WriteFixtureA(original.path());
  Run({OpenLine(original.path()),
       "CREATE TABLE jobs (id INT PRIMARY KEY AUTOINCREMENT, title TEXT UNIQUE "
       "DEFAULT 'it''s; new', owner TEXT REFERENCES visitors(username), ratio "
       "DOUBLE DEFAULT 1.5);"});
  Lines first = Run({OpenLine(original.path()), ".schema"});
  CHECK(!first.empty());
  if (first.empty()) {
    return;
  }
  first.erase(first.begin());  // the `loaded ...` line

  testing::TempDir fresh;
  Lines replay = {OpenLine(fresh.path())};
  replay.insert(replay.end(), first.begin(), first.end());
  replay.push_back(".schema");
  Lines second = Run(replay);

  CHECK_EQ(second.size(), 1 + 4 + first.size());
  if (second.size() != 1 + 4 + first.size()) {
    return;
  }
  CHECK_EQ(second[0], std::string("loaded no tables"));
  CHECK_EQ(second[1], std::string("OK CREATE TABLE visitors"));
  CHECK_EQ(second[4], std::string("OK CREATE TABLE jobs"));
  CHECK_EQ(Lines(second.begin() + 5, second.end()), first);
}

TEST(test_repl_prompts_are_printed_only_when_interactive) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const Lines input = {OpenLine(dir.path()), "SELECT username",
                       "FROM visitors WHERE id = 1;"};

  std::string plain = RunText(input, false);
  std::string prompted = RunText(input, true);

  CHECK(plain.find("strata>") == std::string::npos);
  CHECK_EQ(plain, std::string(kFixtureALoaded) + "\nusername\nalice\n");
  CHECK_EQ(prompted, std::string("strata> ") + kFixtureALoaded +
                         "\nstrata>    ...> username\nalice\n"
                         "strata> ");
}

TEST(test_repl_help_lists_commands_and_quit_stops_reading) {
  Lines quit_first = Run({".help", ".quit", ".help"});
  Lines exit_first = Run({".exit", ".help"});

  CHECK(!quit_first.empty());
  for (const char* marker :
       {".open", ".engine", ".import", ".tables", ".schema", ".dump", ".clear",
        ".cancel", ".help", ".quit"}) {
    CHECK_EQ(CountLinesStartingWith(quit_first, marker), 1);
  }
  bool has_not_null_line = false;
  for (const std::string& line : quit_first) {
    if (line.find("NOT NULL is accepted but has no effect") !=
        std::string::npos) {
      has_not_null_line = true;
    }
  }
  CHECK(has_not_null_line);
  Lines once = Run({".help"});
  CHECK_EQ(quit_first, once);
  CHECK_EQ(exit_first, Lines());
}

TEST(test_repl_commands_without_open_print_error) {
  Lines output = Run({".tables", ".schema", ".schema visitors"});

  CHECK_EQ(output, Lines({kNoDatabaseLine, kNoDatabaseLine, kNoDatabaseLine}));
}

TEST(test_repl_command_with_extra_arguments_prints_usage) {
  Lines output =
      Run({".tables x", ".schema a b", ".help x", ".quit x", ".help"});

  CHECK(output.size() > std::size_t{4});
  if (output.size() <= 4) {
    return;
  }
  CHECK_EQ(Lines(output.begin(), output.begin() + 4),
           Lines({"syntax error at column 1: usage: .tables",
                  "syntax error at column 1: usage: .schema [<table>]",
                  "syntax error at column 1: usage: .help",
                  "syntax error at column 1: usage: .quit"}));
  CHECK_EQ(Lines(output.begin() + 4, output.end()), Run({".help"}));
}

TEST(test_repl_commands_without_open_still_parse_sql_first) {
  Lines output = Run({"SELEC 1;", "SELECT username FROM visitors;"});

  CHECK_EQ(output.size(), std::size_t{2});
  if (output.size() != 2) {
    return;
  }
  CHECK(StartsWith(output[0], "syntax error at column 1: "));
  CHECK_EQ(output[1], std::string(kNoDatabaseLine));
}

// ---- Clear, cancel, input hints, comments, open and import -----------------

const char* const kClearCodes = "\033[H\033[2J";
const char* const kFullWidthError =
    "error: full-width ';' (U+FF1B) found; use ASCII ';'";

TEST(test_repl_clear_prints_control_codes_only_when_interactive) {
  std::string interactive = RunText({".clear"}, true);
  std::string piped = RunText({".clear"}, false);
  Lines with_argument = Run({".clear x"});

  CHECK(interactive.find(kClearCodes) != std::string::npos);
  CHECK(interactive.find("\033[3J") == std::string::npos);
  CHECK(piped.find('\033') == std::string::npos);
  CHECK_EQ(piped, std::string());
  CHECK_EQ(with_argument, Lines({"syntax error at column 1: usage: .clear"}));
}

TEST(test_repl_cancel_discards_pending_statement_but_not_string_content) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines pending = Run({OpenLine(dir.path()), "SELECT username", ".cancel",
                       "SELECT username FROM visitors WHERE id = 1;"});
  Lines in_string = Run({OpenLine(dir.path()), "SELECT 'a", ".cancel", "';"});
  Lines idle = Run({OpenLine(dir.path()), ".cancel", ".cancel x"});

  CHECK_EQ(pending, Lines({kFixtureALoaded, "username", "alice"}));
  CHECK_EQ(in_string.size(), std::size_t{2});
  if (in_string.size() == 2) {
    CHECK(StartsWith(in_string[1], "syntax error at line 3"));
  }
  CHECK_EQ(idle, Lines({kFixtureALoaded,
                        "syntax error at column 1: usage: .cancel"}));
}

TEST(test_repl_missing_dot_command_names_print_hint_and_are_not_executed) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output = Run({OpenLine(dir.path()), "open shop", "TABLES",
                      "import a.csv", "quit", "help me", "SELECT 1;"});
  Lines inside_statement =
      Run({OpenLine(dir.path()), "SELECT 1", "open x", ";"});

  CHECK_EQ(output.size(), std::size_t{7});
  if (output.size() == 7) {
    CHECK_EQ(output[1],
             std::string("error: commands start with '.'; did you mean .open"));
    CHECK_EQ(output[2], std::string("error: commands start with '.'; did you "
                                    "mean .tables"));
    CHECK_EQ(output[3], std::string("error: commands start with '.'; did you "
                                    "mean .import"));
    CHECK_EQ(output[4],
             std::string("error: commands start with '.'; did you mean .quit"));
    CHECK_EQ(output[5],
             std::string("error: commands start with '.'; did you mean .help"));
    CHECK(StartsWith(output[6], "syntax error at column"));
  }
  CHECK_EQ(inside_statement.size(), std::size_t{2});
  if (inside_statement.size() == 2) {
    CHECK(StartsWith(inside_statement[1], "syntax error"));
  }
}

TEST(test_repl_unknown_dot_command_prints_error_and_is_not_executed) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output =
      Run({OpenLine(dir.path()), ".tbles",
           "INSERT INTO visitors (id, username, location) VALUES (9, 'zed', "
           "'Bern');",
           "SELECT username FROM visitors WHERE username = 'zed';",
           ".foo extra words"});
  Lines inside_statement =
      Run({OpenLine(dir.path()), "SELECT 1", ".tbles", ";"});

  CHECK_EQ(output.size(), std::size_t{6});
  if (output.size() == 6) {
    CHECK_EQ(output[1],
             std::string("error: unknown command .tbles; .help lists the "
                         "commands"));
    CHECK_EQ(output[2], std::string("OK affected=1"));
    CHECK_EQ(output[3], std::string("username"));
    CHECK_EQ(output[4], std::string("zed"));
    CHECK_EQ(output[5],
             std::string("error: unknown command .foo; .help lists the "
                         "commands"));
  }
  CHECK_EQ(inside_statement.size(), std::size_t{2});
  if (inside_statement.size() == 2) {
    CHECK(StartsWith(inside_statement[1], "syntax error"));
  }
}

TEST(test_repl_string_continuation_prompt_is_distinct) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const std::string idle = "strata> ";
  const std::string in_string = "     '> ";
  const std::string pending = "   ...> ";

  const std::string prompted = RunText(
      {OpenLine(dir.path()),
       "SELECT username FROM visitors WHERE username = 'a", "b';", "SELECT 1"},
      true);

  CHECK_EQ(idle.size(), std::size_t{8});
  CHECK_EQ(in_string.size(), idle.size());
  CHECK_EQ(pending.size(), idle.size());
  CHECK_EQ(prompted, idle + kFixtureALoaded + "\n" + idle + in_string +
                         "username\n" + idle + pending +
                         "error: incomplete statement at end of input\n");
}

TEST(test_repl_full_width_semicolon_prints_hint_and_drops_the_line) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const std::string full_width = "\xEF\xBC\x9B";

  Lines output =
      Run({OpenLine(dir.path()), "SELECT 1" + full_width, "SELECT username",
           "FROM visitors" + full_width, "FROM visitors WHERE id = 1;"});
  Lines literal = Run({OpenLine(dir.path()),
                       "SELECT username FROM visitors WHERE username = 'x" +
                           full_width + "';"});

  CHECK_EQ(output, Lines({kFixtureALoaded, kFullWidthError, kFullWidthError,
                          "username", "alice"}));
  CHECK_EQ(literal, Lines({kFixtureALoaded, "username"}));
}

TEST(test_repl_comment_lines_and_trailing_comments_are_ignored) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const std::string ideographic_space = "\xE3\x80\x80";

  Lines output =
      Run({OpenLine(dir.path()), "-- a comment",
           "SELECT username FROM visitors WHERE id = 1; -- trailing",
           "SELECT username -- c", "FROM visitors WHERE id = 2;",
           "  " + ideographic_space, "SELECT 1 FROM visitors LIMIT 0;"});

  CHECK_EQ(output, Lines({kFixtureALoaded, "username", "alice", "username",
                          "carol", "1"}));
}

TEST(test_repl_open_reports_created_database_and_rejected_rows) {
  testing::TempDir work;
  testing::TempDir rejects;
  testing::WriteFixtureB(rejects.path());
  testing::TempDir clean;
  testing::WriteFixtureA(clean.path());
  const std::string fresh = (work.path() / "fresh").string();

  Lines created = Run({".open " + fresh});
  Lines with_rejects = Run({OpenLine(rejects.path())});
  Lines without = Run({OpenLine(clean.path())});

  CHECK_EQ(created, Lines({"created new database " + fresh}));
  CHECK(std::filesystem::is_directory(fresh));
  CHECK_EQ(with_rejects,
           Lines({"loaded visitors=2 (rejected 2) stories=1 (rejected 1) "
                  "reactions=4",
                  "visitors.csv: rejected lines 3, 4",
                  "stories.csv: rejected lines 3"}));
  CHECK_EQ(without, Lines({kFixtureALoaded}));

  // Twelve rejected lines (2 to 13): ten are listed, then an ellipsis.
  testing::TempDir many;
  testing::WriteFixtureA(many.path());
  std::string visitors = "id,username,location\n";
  for (int line = 2; line <= 13; ++line) {
    visitors += "bad\n";
  }
  testing::WriteFile(many.path() / "visitors.csv", visitors);

  Lines truncated = Run({OpenLine(many.path())});

  CHECK_EQ(
      truncated,
      Lines({"loaded visitors=0 (rejected 12) stories=3 reactions=5",
             "visitors.csv: rejected lines 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, "
             "..."}));
}

TEST(test_repl_import_command_runs_import_and_prints_notes) {
  testing::TempDir dir;
  testing::TempDir sources;
  const std::string good = (sources.path() / "notes.csv").string();
  const std::string dirty = (sources.path() / "dirty.csv").string();
  testing::WriteFile(sources.path() / "notes.csv", "id,body\n1,a\n2,b\n");
  testing::WriteFile(sources.path() / "dirty.csv", "id,body\n1,a\n2\n");
  const std::string usage =
      "syntax error at column 1: usage: .import <file.csv> [<table>] [--pk "
      "<column>] [--type <column>=<type>]... [--skip-bad-rows]";

  Lines output =
      Run({".import " + good, OpenLine(dir.path()),
           ".import " + good + " --pk id", ".tables", ".schema notes",
           ".import " + good, ".import", ".import a b c",
           ".import " + good + " --pk", ".import " + good + " --type id",
           ".import " + dirty + " dirty --skip-bad-rows"});

  const Lines expected = {"error: no database open; run .open first",
                          "loaded no tables",
                          "OK IMPORT notes (imported 2 rows)",
                          "notes",
                          "CREATE TABLE notes (",
                          "  id INT PRIMARY KEY,",
                          "  body TEXT",
                          ");",
                          "error: bind: table notes already exists",
                          usage,
                          usage,
                          usage,
                          usage,
                          "OK IMPORT dirty (imported 1 rows, skipped 1)",
                          "skipped line 3: expected 2 fields, found 1"};
  CHECK_EQ(output, expected);
}

TEST(test_repl_initial_open_acts_like_dot_open) {
  testing::TempDir fixture;
  testing::WriteFixtureA(fixture.path());
  testing::TempDir work;
  const std::string missing_parent = (work.path() / "nosuch" / "db").string();
  const std::string fresh = (work.path() / "fresh").string();

  Lines existing =
      RunWithInitialOpen({fixture.path().string(), false}, {".tables"});
  Lines failed = RunWithInitialOpen({missing_parent, false}, {".tables"});
  Lines created = RunWithInitialOpen({fresh, true}, {".tables"});

  CHECK_EQ(existing,
           Lines({kFixtureALoaded, "visitors", "stories", "reactions"}));
  CHECK_EQ(failed, Lines({"error: could not open " + missing_parent +
                              ": no such directory",
                          kNoDatabaseLine}));
  CHECK_EQ(created, Lines({"created new database " + fresh}));
}

// ----------------------------------------------------------------------------
// `.dump index`.
// ----------------------------------------------------------------------------

namespace {

namespace paged = strata::paged;

const char* const kUsageDumpIndex =
    "syntax error at column 1: usage: .dump index <table>";

// The first table of a new paged database has its heap at kFirstUserPage and
// its index root on the next page.
constexpr paged::PageId kFirstIndexRoot = paged::kFirstUserPage + 1;

std::filesystem::path PagedFile(const std::filesystem::path& directory) {
  return directory / "paged.db";
}

std::vector<std::uint8_t> ReadPageFromFile(const std::filesystem::path& file,
                                           paged::PageId page_id) {
  std::vector<std::uint8_t> bytes(paged::kPageSize);
  std::ifstream stream(file, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(page_id) * paged::kPageSize);
  stream.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

// Overwrites one page of the file with the given bytes and nothing else.
void WritePageToFile(const std::filesystem::path& file, paged::PageId page_id,
                     const std::vector<std::uint8_t>& bytes) {
  std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
  stream.seekp(static_cast<std::streamoff>(page_id) * paged::kPageSize);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void WriteText(const std::filesystem::path& file, const std::string& text) {
  std::ofstream stream(file, std::ios::binary);
  stream << text;
}

// The index of the first line that starts with `prefix`, or lines.size().
std::size_t IndexOfLineStartingWith(const Lines& lines,
                                    const std::string& prefix) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].starts_with(prefix)) {
      return i;
    }
  }
  return lines.size();
}

}  // namespace

TEST(test_repl_dump_index_prints_shape_and_check) {
  // The example of the design: three keys fit in the root, which is a leaf.
  {
    testing::TempDir directory;
    const Lines output =
        Run({".engine paged", OpenLine(directory.path()),
             "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body "
             "TEXT);",
             "INSERT INTO notes (body) VALUES ('a'), ('b'), ('c');",
             ".dump index notes"});
    CHECK_EQ(Lines(output.end() - 3, output.end()),
             Lines({"index notes(id): height 1, nodes 1, keys 3",
                    "level 0 page 4 keys 3 [1 .. 3]", "check: ok"}));
  }

  // 6000 keys in ascending order: the root and 41 leaves, of which 40 nodes
  // are listed.
  {
    testing::TempDir directory;
    Lines input = {".engine paged", OpenLine(directory.path()),
                   "CREATE TABLE big (id INT PRIMARY KEY, v INT);"};
    for (int first = 1; first <= 6000; first += 500) {
      std::string insert = "INSERT INTO big VALUES ";
      for (int id = first; id < first + 500; ++id) {
        insert += (id > first ? ", (" : "(") + std::to_string(id) + ", 0)";
      }
      input.push_back(insert + ";");
    }
    input.push_back(".dump index big");
    const Lines output = Run(input);
    const std::size_t first_line =
        IndexOfLineStartingWith(output, "index big(id)");
    CHECK_EQ(output.size(), first_line + 43);
    if (output.size() == first_line + 43) {
      CHECK_EQ(output[first_line],
               std::string("index big(id): height 2, nodes 42, keys 6000"));
      CHECK(output[first_line + 1].starts_with("level 1 page "));
      CHECK(output[first_line + 1].find(" next ") == std::string::npos);
      for (std::size_t i = first_line + 2; i <= first_line + 40; ++i) {
        CHECK(output[i].starts_with("level 0 page "));
        CHECK(output[i].find(" keys 145 [") != std::string::npos);
        CHECK(output[i].find(" next ") != std::string::npos);
      }
      CHECK_EQ(output[first_line + 41], std::string("... and 2 more nodes"));
      CHECK_EQ(output[first_line + 42], std::string("check: ok"));
    }
  }

  // A leaf that lost an entry: the check line carries the violation.
  {
    testing::TempDir directory;
    CHECK(!Run({".engine paged", OpenLine(directory.path()),
                "CREATE TABLE gems (id INT PRIMARY KEY, v INT);",
                "INSERT INTO gems VALUES (1, 1), (2, 2), (3, 3);"})
               .empty());
    std::vector<std::uint8_t> root =
        ReadPageFromFile(PagedFile(directory.path()), kFirstIndexRoot);
    paged::LeafRemoveAt(root.data(), 0);
    paged::SlottedPage::SealChecksum(root.data());
    WritePageToFile(PagedFile(directory.path()), kFirstIndexRoot, root);

    const Lines output =
        Run({".engine paged", OpenLine(directory.path()), ".dump index gems"});
    CHECK_EQ(output.size(), std::size_t{5});
    if (output.size() == 5) {
      CHECK_EQ(output[2],
               std::string("index gems(id): height 1, nodes 1, keys 2"));
      CHECK_EQ(output[3], std::string("level 0 page 4 keys 2 [2 .. 3]"));
      CHECK(output[4].starts_with("check: "));
      CHECK(output[4].find("has no entry for key 1") != std::string::npos);
    }
  }
}

TEST(test_repl_dump_index_reports_errors_and_empty_trees) {
  testing::TempDir directory;
  const Lines paged_output =
      Run({".dump index t", ".engine paged", OpenLine(directory.path()),
           "CREATE TABLE words (code TEXT PRIMARY KEY, n INT);",
           "CREATE TABLE emptied (id INT PRIMARY KEY, v INT);",
           "INSERT INTO emptied VALUES (1, 1), (2, 2);", "DELETE FROM emptied;",
           ".dump index words", ".dump index nosuch", ".dump index emptied",
           ".dump", ".dump index", ".dump tables x", ".dump index a b"});
  CHECK_EQ(paged_output.front(), std::string(kNoDatabaseLine));
  CHECK(paged_output.size() > std::size_t{9});
  if (paged_output.size() <= 9) {
    return;
  }
  const Lines tail(paged_output.end() - 9, paged_output.end());
  CHECK_EQ(Lines(tail.begin(), tail.begin() + 3),
           Lines({"error: bind: table words has no index",
                  "error: no such table: nosuch",
                  "index emptied(id): height 1, nodes 1, keys 0"}));
  // An emptied tree keeps its root: one node line, without a bracket.
  CHECK(tail[3].starts_with("level 0 page "));
  CHECK(tail[3].ends_with(" keys 0"));
  CHECK_EQ(tail[4], std::string("check: ok"));
  CHECK_EQ(Lines(tail.begin() + 5, tail.end()),
           Lines(4, std::string(kUsageDumpIndex)));

  // The flatfile engine has no indexes, whatever the table.
  testing::TempDir flat_directory;
  const Lines flat_output = Run({OpenLine(flat_directory.path()),
                                 "CREATE TABLE t (id INT PRIMARY KEY);",
                                 ".dump index t", ".dump index nosuch"});
  CHECK_EQ(Lines(flat_output.end() - 2, flat_output.end()),
           Lines({"error: unsupported: the flatfile engine has no indexes",
                  "error: no such table: nosuch"}));
}

TEST(test_paged_engine_index_follows_primary_key_type_and_survives_reopen) {
  testing::TempDir directory;
  testing::TempDir sources;
  WriteText(sources.path() / "ids.csv", "id,name\n1,a\n2,b\n3,c\n");
  WriteText(sources.path() / "words.csv", "code,n\nalpha,1\nbeta,2\n");
  const std::string ids_path = (sources.path() / "ids.csv").string();
  const std::string words_path = (sources.path() / "words.csv").string();
  const Lines queries = {".dump index made",
                         ".dump index ids",
                         ".dump index words",
                         "SELECT v FROM made WHERE id = 7;",
                         "SELECT name FROM ids WHERE id = 2;",
                         "SELECT n FROM words WHERE code = 'beta';"};
  Lines first_session = {".engine paged",
                         OpenLine(directory.path()),
                         "CREATE TABLE made (id INT PRIMARY KEY, v INT);",
                         "INSERT INTO made VALUES (7, 70), (8, 80);",
                         ".import " + ids_path + " ids --pk id --type id=INT",
                         ".import " + words_path + " words --pk code"};
  first_session.insert(first_session.end(), queries.begin(), queries.end());
  Lines second_session = {".engine paged", OpenLine(directory.path())};
  second_session.insert(second_session.end(), queries.begin(), queries.end());
  const Lines first_output = Run(first_session);
  const Lines second_output = Run(second_session);

  // The queries print 13 lines: two index dumps of 3, the error of the TEXT
  // key, and three 2-line results.
  constexpr std::size_t kQueryLines = 13;
  CHECK(first_output.size() > kQueryLines);
  CHECK(second_output.size() > kQueryLines);
  if (first_output.size() <= kQueryLines ||
      second_output.size() <= kQueryLines) {
    return;
  }
  const Lines before(first_output.end() - kQueryLines, first_output.end());
  const Lines after(second_output.end() - kQueryLines, second_output.end());
  CHECK_EQ(before, after);
  CHECK_EQ(before[0], std::string("index made(id): height 1, nodes 1, keys 2"));
  CHECK(before[1].starts_with("level 0 page "));
  CHECK_EQ(before[2], std::string("check: ok"));
  CHECK_EQ(before[3], std::string("index ids(id): height 1, nodes 1, keys 3"));
  CHECK(before[4].starts_with("level 0 page "));
  CHECK_EQ(Lines(before.begin() + 5, before.end()),
           Lines({"check: ok", "error: bind: table words has no index", "v",
                  "70", "name", "b", "n", "2"}));

  // The root of `made` rewritten to the first page of its heap: opening the
  // directory fails and the open database stays.
  WritePageToFile(
      PagedFile(directory.path()), kFirstIndexRoot,
      ReadPageFromFile(PagedFile(directory.path()), paged::kFirstUserPage));
  testing::TempDir healthy;
  const Lines damaged_output = Run({".engine paged", OpenLine(healthy.path()),
                                    "CREATE TABLE fine (id INT PRIMARY KEY);",
                                    OpenLine(directory.path()), ".tables"});
  CHECK(damaged_output.size() > std::size_t{2});
  if (damaged_output.size() > 2) {
    const std::string& failure = damaged_output[damaged_output.size() - 2];
    CHECK(failure.starts_with("error: could not open " +
                              directory.path().string() + ": corrupt: "));
    CHECK_EQ(damaged_output.back(), std::string("fine"));
  }

  // The flatfile engine does not read paged.db.
  const Lines flat_output = Run({OpenLine(directory.path())});
  for (const std::string& line : flat_output) {
    CHECK(!line.starts_with("error"));
  }
}
