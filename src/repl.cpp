#include "strata/repl.h"

#include <algorithm>
#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/bootstrap.h"
#include "strata/domain/import.h"
#include "strata/domain/index_inspector.h"
#include "strata/domain/value.h"
#include "strata/sql_parser.h"
#include "strata/statement_buffer.h"

namespace strata::cli {

namespace {

constexpr const char* kPrompt = "strata> ";
constexpr const char* kContinuationPrompt = "   ...> ";
constexpr const char* kStringPrompt = "     '> ";
constexpr const char* kClearScreen = "\033[H\033[2J";
constexpr const char* kFullWidthSemicolonError =
    "error: full-width ';' (U+FF1B) found; use ASCII ';'\n";
constexpr const char* kImportUsage =
    ".import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]... "
    "[--skip-bad-rows]";
// The command names that are recognised when typed without the leading '.'.
constexpr const char* kCommandNames[] = {"open",   "engine", "import", "tables",
                                         "schema", "dump",   "help",   "quit",
                                         "exit",   "clear",  "cancel"};
constexpr std::size_t kMaxListedLines = 10;
constexpr const char* kNoDatabase =
    "error: no database open; run .open first\n";
constexpr const char* kEngineUsage = ".engine [flatfile|paged]";
constexpr const char* kHelp =
    ".open <directory> [--parallel]  open a database directory (created when "
    "missing)\n"
    ".engine [flatfile|paged]        show or set the engine (default "
    "flatfile)\n"
    ".import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]...\n"
    "        [--skip-bad-rows]       import a CSV file as a new table\n"
    ".tables                         list the tables\n"
    ".schema [<table>]               show the CREATE TABLE statements\n"
    ".dump index <table>             show the shape and the check of an index\n"
    ".clear                          clear the screen\n"
    ".cancel                         discard the statement being typed\n"
    ".help                           show this text\n"
    ".quit                           leave (also .exit)\n"
    "Statements end with ';' and may span several lines; '--' starts a "
    "comment.\n"
    "NOT NULL is accepted but has no effect (there is no NULL).\n";

using Database = std::optional<bootstrap::OpenedDatabase>;
using bootstrap::EngineKind;

std::string UsageError(const std::string& usage) {
  return "syntax error at column 1: usage: " + usage + "\n";
}

struct OpenArguments {
  std::string directory;
  bool is_parallel;
};

std::vector<std::string> SplitWhitespace(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

std::string StripTrailingCarriageReturn(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

// `.open <directory> [--parallel]`; std::nullopt when the arguments do not
// match, including a directory that starts with "--" (an option typed where the
// directory belongs, as `strata_cli` also refuses).
std::optional<OpenArguments> ParseOpenArguments(
    const std::vector<std::string>& tokens) {
  if (tokens.size() != 2 && tokens.size() != 3) {
    return std::nullopt;
  }
  if (tokens[1].starts_with("--")) {
    return std::nullopt;
  }
  if (tokens.size() == 3 && tokens[2] != "--parallel") {
    return std::nullopt;
  }
  return OpenArguments{tokens[1], tokens.size() == 3};
}

std::string JoinWithTabs(const std::vector<std::string>& cells) {
  std::string line;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (i > 0) {
      line += '\t';
    }
    line += cells[i];
  }
  return line;
}

std::string RenderResultSet(const domain::ResultSet& result_set) {
  std::vector<std::string> header;
  for (const auto& column : result_set.columns) {
    header.push_back(column.name);
  }
  std::string text = JoinWithTabs(header) + "\n";
  for (const auto& row : result_set.rows) {
    std::vector<std::string> cells;
    for (const auto& value : row) {
      cells.push_back(domain::FormatValue(value));
    }
    text += JoinWithTabs(cells) + "\n";
  }
  return text;
}

// Text for one execution result, ending with a newline.
std::string RenderResult(const domain::ExecutionResult& result) {
  if (const auto* result_set = std::get_if<domain::ResultSet>(&result)) {
    return RenderResultSet(*result_set);
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&result)) {
    return "OK affected=" + std::to_string(affected->count) + "\n";
  }
  if (const auto* complete = std::get_if<domain::CommandComplete>(&result)) {
    std::string text = "OK " + complete->tag + "\n";
    for (const std::string& note : complete->notes) {
      text += note + "\n";
    }
    return text;
  }
  const auto& error = std::get<domain::ExecutionError>(result);
  return std::string("error: ") + domain::ErrorKindName(error.kind) + ": " +
         error.message + "\n";
}

// `instructors=6 students=30`, or `no tables`.
std::string DescribeTables(const std::vector<bootstrap::TableCount>& tables) {
  if (tables.empty()) {
    return "no tables";
  }
  std::string text;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (i > 0) {
      text += ' ';
    }
    text += tables[i].name + "=" + std::to_string(tables[i].row_count);
    if (!tables[i].rejected_lines.empty()) {
      text +=
          " (rejected " + std::to_string(tables[i].rejected_lines.size()) + ")";
    }
  }
  return text;
}

// One line per table that rejected rows: `students.csv: rejected lines 12, 17`,
// at most kMaxListedLines numbers each.
std::string DescribeRejectedLines(
    const std::vector<bootstrap::TableCount>& tables) {
  std::string text;
  for (const bootstrap::TableCount& table : tables) {
    if (table.rejected_lines.empty()) {
      continue;
    }
    text += domain::ToLower(table.name) + ".csv: rejected lines ";
    for (std::size_t i = 0; i < table.rejected_lines.size(); ++i) {
      if (i == kMaxListedLines) {
        text += ", ...";
        break;
      }
      if (i > 0) {
        text += ", ";
      }
      text += std::to_string(table.rejected_lines[i]);
    }
    text += "\n";
  }
  return text;
}

// What opening a database printed, and whether it replaced the current one.
struct OpenReport {
  bool is_opened;
  std::string text;  // ends with a newline
};

// One `note:` line per element of `notes`.
std::string DescribeNotes(const std::vector<std::string>& notes) {
  std::string text;
  for (const std::string& note : notes) {
    text += note + "\n";
  }
  return text;
}

// Opens the database named by `arguments` with `engine` and reports the lines
// to print. A successful open replaces `database`; a failed one leaves it
// untouched. `--parallel` only has an effect on the flat-file engine.
OpenReport OpenDatabase(const OpenArguments& arguments, EngineKind engine,
                        Database& database) {
  const bootstrap::OpenedDatabase* current = database ? &*database : nullptr;
  auto opened = engine == EngineKind::kPaged
                    ? bootstrap::OpenPagedDatabase(arguments.directory, current)
                    : bootstrap::OpenFlatFileDatabase(
                          arguments.directory, arguments.is_parallel, current);
  if (const auto* reason = std::get_if<std::string>(&opened)) {
    return OpenReport{false, "error: could not open " + arguments.directory +
                                 ": " + *reason + "\n"};
  }
  database = std::move(std::get<bootstrap::OpenedDatabase>(opened));
  if (database->is_created) {
    return OpenReport{true, "created new database " + arguments.directory +
                                "\n" + DescribeNotes(database->notes)};
  }
  return OpenReport{true, "loaded " + DescribeTables(database->tables) + "\n" +
                              DescribeRejectedLines(database->tables) +
                              DescribeNotes(database->notes)};
}

std::string ExecuteSql(const std::string& statement_text, Database& database) {
  adapter::ParseResult parsed = adapter::ParseStatement(statement_text);
  if (const auto* error = std::get_if<adapter::ParseError>(&parsed)) {
    return FormatParseError(statement_text, *error) + "\n";
  }
  if (!database) {
    return kNoDatabase;
  }
  return RenderResult(
      database->service->Execute(std::get<domain::Statement>(parsed)));
}

std::string ListTables(Database& database) {
  std::string text;
  for (const domain::TableSchema& schema :
       database->service->DescribeCatalog()) {
    text += schema.name + "\n";
  }
  return text;
}

// One table when `table` is non-empty, otherwise every table, in catalog
// order.
std::string ShowSchema(const std::string& table, Database& database) {
  const std::vector<domain::TableSchema> catalog =
      database->service->DescribeCatalog();
  std::string text;
  bool is_found = false;
  for (const domain::TableSchema& schema : catalog) {
    if (!table.empty() &&
        domain::ToLower(schema.name) != domain::ToLower(table)) {
      continue;
    }
    is_found = true;
    text += FormatCreateTable(schema, catalog) + "\n";
  }
  if (!table.empty() && !is_found) {
    return "error: no such table: " + table + "\n";
  }
  return text;
}

// The text of `.dump index`: the summary line, one line per listed node, a line
// for the nodes left out and the check (design document 3.6).
std::string FormatIndexDescription(
    const domain::IndexDescription& description) {
  std::string text = "index " + description.table + "(" + description.column +
                     "): height " + std::to_string(description.height) +
                     ", nodes " + std::to_string(description.node_count) +
                     ", keys " + std::to_string(description.key_count) + "\n";
  for (const domain::IndexNodeDescription& node : description.nodes) {
    text += "level " + std::to_string(node.level) + " page " +
            std::to_string(node.page_id) + " keys " +
            std::to_string(node.key_count);
    if (node.first_key.has_value() && node.last_key.has_value()) {
      text += " [" + std::to_string(*node.first_key) + " .. " +
              std::to_string(*node.last_key) + "]";
    }
    if (node.next_page_id.has_value()) {
      text += " next " + std::to_string(*node.next_page_id);
    }
    text += "\n";
  }
  if (description.node_count > description.nodes.size()) {
    text += "... and " +
            std::to_string(description.node_count - description.nodes.size()) +
            " more nodes\n";
  }
  return text + "check: " + description.check + "\n";
}

// An unknown table is reported here, as `.schema` does; the other failures are
// the errors of QueryService::DescribeIndex.
std::string ShowIndex(const std::string& table, Database& database) {
  bool is_found = false;
  for (const domain::TableSchema& schema :
       database->service->DescribeCatalog()) {
    if (domain::ToLower(schema.name) == domain::ToLower(table)) {
      is_found = true;
    }
  }
  if (!is_found) {
    return "error: no such table: " + table + "\n";
  }
  domain::IndexDescriptionResult described =
      database->service->DescribeIndex(table);
  if (auto* error = std::get_if<domain::ExecutionError>(&described)) {
    return RenderResult(domain::ExecutionResult(std::move(*error)));
  }
  return FormatIndexDescription(std::get<domain::IndexDescription>(described));
}

// The result of a line that holds a command.
struct CommandOutcome {
  bool is_command;  // False: the line is not a command and holds SQL.
  std::string text;
  bool should_quit;
};

CommandOutcome Text(std::string text) {
  return CommandOutcome{true, std::move(text), false};
}

// The type named in `--type <column>=<type>`, in any case; std::nullopt for an
// unknown name. The names are those of a column definition.
std::optional<domain::DataType> ParseTypeName(const std::string& name) {
  const std::string lowered = domain::ToLower(name);
  if (lowered == "int" || lowered == "integer" || lowered == "bigint") {
    return domain::DataType::kInt;
  }
  if (lowered == "double" || lowered == "real" || lowered == "float") {
    return domain::DataType::kDouble;
  }
  if (lowered == "text") {
    return domain::DataType::kText;
  }
  return std::nullopt;
}

// `.import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]...
// [--skip-bad-rows]`; std::nullopt when the arguments do not match.
std::optional<domain::ImportRequest> ParseImportArguments(
    const std::vector<std::string>& tokens) {
  domain::ImportRequest request;
  std::vector<std::string> positionals;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token == "--skip-bad-rows") {
      request.should_skip_bad_rows = true;
    } else if (token == "--pk") {
      if (i + 1 >= tokens.size()) {
        return std::nullopt;
      }
      request.primary_key = tokens[++i];
    } else if (token == "--type") {
      if (i + 1 >= tokens.size()) {
        return std::nullopt;
      }
      const std::string& spec = tokens[++i];
      const std::size_t equals = spec.find('=');
      if (equals == std::string::npos || equals == 0) {
        return std::nullopt;
      }
      std::optional<domain::DataType> type =
          ParseTypeName(spec.substr(equals + 1));
      if (!type) {
        return std::nullopt;
      }
      request.column_types.emplace_back(spec.substr(0, equals), *type);
    } else if (token.starts_with("--")) {
      return std::nullopt;
    } else {
      positionals.push_back(token);
    }
  }
  if (positionals.empty() || positionals.size() > 2) {
    return std::nullopt;
  }
  request.file_path = positionals[0];
  if (positionals.size() == 2) {
    request.table = positionals[1];
  }
  return request;
}

const char* EngineName(EngineKind engine) {
  return engine == EngineKind::kPaged ? "paged" : "flatfile";
}

// The engine named in `.engine <name>`; std::nullopt for any other word.
std::optional<EngineKind> ParseEngineName(const std::string& name) {
  if (name == "flatfile") {
    return EngineKind::kFlatFile;
  }
  if (name == "paged") {
    return EngineKind::kPaged;
  }
  return std::nullopt;
}

// `.engine` shows the engine of the session; `.engine <name>` sets it. Naming
// the current engine only prints it. With a database open, naming the other
// engine reopens its directory with that engine and prints what `.open` would;
// the engine of the session changes only when that open succeeds, so a failed
// switch prints the error alone and leaves the database and the setting as
// they were (ADR-0041).
CommandOutcome HandleEngineCommand(const std::vector<std::string>& tokens,
                                   Database& database, EngineKind& engine) {
  if (tokens.size() > 2) {
    return Text(UsageError(kEngineUsage));
  }
  if (tokens.size() == 1) {
    return Text(std::string("engine ") + EngineName(engine) + "\n");
  }
  const std::optional<EngineKind> requested = ParseEngineName(tokens[1]);
  if (!requested) {
    return Text(UsageError(kEngineUsage));
  }
  const std::string announcement =
      std::string("engine ") + EngineName(*requested) + "\n";
  if (*requested == engine) {
    return Text(announcement);
  }
  if (!database) {
    engine = *requested;
    return Text(announcement);
  }
  // The reopen replaces `database`, so the directory is copied out first.
  const OpenArguments arguments{database->directory, false};
  const OpenReport report = OpenDatabase(arguments, *requested, database);
  if (!report.is_opened) {
    return Text(report.text);
  }
  engine = *requested;
  return Text(announcement + report.text);
}

// Runs the line when its first token is a command; otherwise reports that it is
// not one, so that the caller reads the line as SQL or, when it starts with
// '.', rejects it as an unknown command.
CommandOutcome HandleCommand(const std::string& line, Database& database,
                             EngineKind& engine, bool is_interactive) {
  std::vector<std::string> tokens = SplitWhitespace(line);
  if (tokens.empty()) {
    return CommandOutcome{false, "", false};
  }
  const std::string& name = tokens[0];
  if (name == ".open") {
    std::optional<OpenArguments> arguments = ParseOpenArguments(tokens);
    if (!arguments) {
      return Text(UsageError(".open <directory> [--parallel]"));
    }
    return Text(OpenDatabase(*arguments, engine, database).text);
  }
  if (name == ".engine") {
    return HandleEngineCommand(tokens, database, engine);
  }
  if (name == ".import") {
    std::optional<domain::ImportRequest> request = ParseImportArguments(tokens);
    if (!request) {
      return Text(UsageError(kImportUsage));
    }
    if (!database) {
      return Text(kNoDatabase);
    }
    return Text(RenderResult(database->service->ImportCsv(*request)));
  }
  if (name == ".clear") {
    if (tokens.size() != 1) {
      return Text(UsageError(".clear"));
    }
    return Text(is_interactive ? kClearScreen : "");
  }
  if (name == ".cancel") {
    // Nothing is in progress when a line is read as a command.
    if (tokens.size() != 1) {
      return Text(UsageError(".cancel"));
    }
    return Text("");
  }
  if (name == ".tables") {
    if (tokens.size() != 1) {
      return Text(UsageError(".tables"));
    }
    return Text(database ? ListTables(database) : kNoDatabase);
  }
  if (name == ".schema") {
    if (tokens.size() > 2) {
      return Text(UsageError(".schema [<table>]"));
    }
    if (!database) {
      return Text(kNoDatabase);
    }
    return Text(
        ShowSchema(tokens.size() == 2 ? tokens[1] : std::string(), database));
  }
  if (name == ".dump") {
    if (tokens.size() != 3 || tokens[1] != "index") {
      return Text(UsageError(".dump index <table>"));
    }
    if (!database) {
      return Text(kNoDatabase);
    }
    return Text(ShowIndex(tokens[2], database));
  }
  if (name == ".help") {
    return Text(tokens.size() == 1 ? std::string(kHelp) : UsageError(".help"));
  }
  if (name == ".quit" || name == ".exit") {
    if (tokens.size() != 1) {
      return Text(UsageError(".quit"));
    }
    return CommandOutcome{true, "", true};
  }
  return CommandOutcome{false, "", false};
}

std::string SqlTypeName(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      return "TEXT";
  }
  return "";
}

// A default value as an SQL literal: TEXT in single quotes, a quote written
// twice.
std::string SqlLiteral(const domain::Value& value) {
  const auto* text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    return domain::FormatValue(value);
  }
  std::string literal = "'";
  for (char letter : *text) {
    if (letter == '\'') {
      literal += '\'';
    }
    literal += letter;
  }
  return literal + "'";
}

const domain::TableSchema* FindSchema(
    const std::vector<domain::TableSchema>& catalog, const std::string& name) {
  for (const domain::TableSchema& schema : catalog) {
    if (domain::ToLower(schema.name) == domain::ToLower(name)) {
      return &schema;
    }
  }
  return nullptr;
}

std::string ColumnConstraints(const domain::TableSchema& schema,
                              std::size_t column_index,
                              const std::vector<domain::TableSchema>& catalog) {
  std::string text;
  if (column_index == schema.primary_key_index) {
    text += " PRIMARY KEY";
    if (schema.auto_increment_index == column_index) {
      text += " AUTOINCREMENT";
    }
  }
  if (std::ranges::find(schema.unique_indexes, column_index) !=
      schema.unique_indexes.end()) {
    text += " UNIQUE";
  }
  const domain::ColumnDef& column = schema.columns[column_index];
  if (column.default_value) {
    text += " DEFAULT " + SqlLiteral(*column.default_value);
  }
  for (const domain::ForeignKey& foreign_key : schema.foreign_keys) {
    if (foreign_key.column_index != column_index) {
      continue;
    }
    const domain::TableSchema* target =
        FindSchema(catalog, foreign_key.ref_table);
    text +=
        " REFERENCES " + foreign_key.ref_table + "(" +
        (target != nullptr ? target->columns[foreign_key.ref_column_index].name
                           : std::to_string(foreign_key.ref_column_index)) +
        ")";
  }
  return text;
}

}  // namespace

std::string FormatCreateTable(const domain::TableSchema& schema,
                              const std::vector<domain::TableSchema>& catalog) {
  std::string text = "CREATE TABLE " + schema.name + " (\n";
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    text += "  " + schema.columns[i].name + " " +
            SqlTypeName(schema.columns[i].type) +
            ColumnConstraints(schema, i, catalog);
    text += (i + 1 < schema.columns.size()) ? ",\n" : "\n";
  }
  return text + ");";
}

std::string FormatParseError(const std::string& statement,
                             const adapter::ParseError& error) {
  const std::size_t offset = std::min(
      error.column == 0 ? std::size_t{0} : error.column - 1, statement.size());
  const std::size_t newlines = static_cast<std::size_t>(
      std::count(statement.begin(), statement.begin() + offset, '\n'));
  if (newlines == 0) {
    return "syntax error at column " + std::to_string(error.column) + ": " +
           error.description;
  }
  const std::size_t line_start = statement.rfind('\n', offset - 1);
  return "syntax error at line " + std::to_string(newlines + 1) + ", column " +
         std::to_string(offset - line_start) + ": " + error.description;
}

// The text to print when `line` is a command name typed without its '.'; empty
// when it is not (ADR-0023).
std::string MissingDotHint(const std::string& line) {
  std::vector<std::string> tokens = SplitWhitespace(line);
  if (tokens.empty()) {
    return std::string();
  }
  const std::string word = domain::ToLower(tokens[0]);
  for (const char* name : kCommandNames) {
    if (word == name) {
      return "error: commands start with '.'; did you mean ." + word + "\n";
    }
  }
  return std::string();
}

const char* PromptFor(const StatementBuffer& buffer) {
  if (buffer.IsInString()) {
    return kStringPrompt;
  }
  return buffer.IsEmpty() ? kPrompt : kContinuationPrompt;
}

void RunRepl(std::istream& input, std::ostream& output, bool is_interactive,
             const std::optional<InitialOpen>& initial_open) {
  Database database;
  EngineKind engine = EngineKind::kFlatFile;  // not kept across runs
  StatementBuffer buffer;
  if (initial_open) {
    output << OpenDatabase(OpenArguments{initial_open->directory,
                                         initial_open->is_parallel},
                           engine, database)
                  .text;
  }
  std::string line;
  while (true) {
    if (is_interactive) {
      output << PromptFor(buffer) << std::flush;
    }
    if (!std::getline(input, line)) {
      break;
    }
    line = StripTrailingCarriageReturn(std::move(line));
    if (buffer.IsEmpty()) {
      const std::string trimmed = TrimSqlWhitespace(line);
      if (trimmed.empty() || trimmed.starts_with("--")) {
        continue;
      }
      const std::string hint = MissingDotHint(trimmed);
      if (!hint.empty()) {
        output << hint;
        continue;
      }
      CommandOutcome command =
          HandleCommand(line, database, engine, is_interactive);
      if (command.is_command) {
        output << command.text;
        if (command.should_quit) {
          break;
        }
        continue;
      }
      // No SQL statement starts with '.', so the line is a mistyped command.
      // Reading it as SQL would absorb the lines up to the next ';'.
      if (trimmed.starts_with('.')) {
        output << "error: unknown command " << SplitWhitespace(trimmed)[0]
               << "; .help lists the commands\n";
        continue;
      }
    } else if (!buffer.IsInString() && TrimSqlWhitespace(line) == ".cancel") {
      buffer.Cancel();
      continue;
    }
    if (buffer.HasFullWidthSemicolon(line)) {
      output << kFullWidthSemicolonError;
      continue;
    }
    for (const std::string& statement : buffer.AddLine(line)) {
      output << ExecuteSql(statement, database);
    }
  }
  if (!buffer.IsEmpty()) {
    output << "error: incomplete statement at end of input\n";
  }
}

}  // namespace strata::cli
