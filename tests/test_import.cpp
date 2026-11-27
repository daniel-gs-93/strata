#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/csv_file_source.h"
#include "strata/domain/import.h"
#include "strata/sql_parser.h"
#include "test_harness.h"
#include "test_support.h"

// `.import` (docs/DesignDocument.md sections 2.7 and 3.5): the pure inference
// over text cells (Unit tier) and the whole import through QueryService, the
// engine and the files (Integration tier).

namespace strata::testing {

namespace {

using Cells = std::vector<std::string>;

// One CSV line at the given line number.
domain::CsvLine Line(std::size_t number, Cells cells) {
  return domain::CsvLine{number, std::move(cells)};
}

domain::CsvLine MalformedLine(std::size_t number) {
  return domain::CsvLine{number, std::nullopt};
}

// A header followed by the rows, numbered from 1.
std::vector<domain::CsvLine> Table(const Cells& header,
                                   const std::vector<Cells>& rows) {
  std::vector<domain::CsvLine> lines;
  lines.push_back(Line(1, header));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    lines.push_back(Line(i + 2, rows[i]));
  }
  return lines;
}

domain::ImportRequest Request(const std::string& file_path) {
  domain::ImportRequest request;
  request.file_path = file_path;
  return request;
}

struct Inferred {
  std::optional<domain::ImportPlan> plan;
  std::string error;  // the kBind message when plan is empty
};

Inferred Infer(const domain::ImportRequest& request,
               const std::vector<domain::CsvLine>& lines) {
  FakeCatalog catalog;
  auto outcome = domain::InferImport(request, lines, catalog);
  Inferred inferred;
  if (auto* plan = std::get_if<domain::ImportPlan>(&outcome)) {
    inferred.plan = std::move(*plan);
    return inferred;
  }
  const auto& failure = std::get<domain::ExecutionError>(outcome);
  CHECK_EQ(std::string(domain::ErrorKindName(failure.kind)),
           std::string(domain::ErrorKindName(domain::ErrorKind::kBind)));
  inferred.error = failure.message;
  return inferred;
}

bool Contains(const std::string& text, const std::string& fragment) {
  return text.find(fragment) != std::string::npos;
}

const char* TypeText(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      break;
  }
  return "TEXT";
}

std::string ColumnTypes(const domain::TableSchema& schema) {
  std::string text;
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (i > 0) {
      text += ",";
    }
    text += TypeText(schema.columns[i].type);
  }
  return text;
}

std::string UpperCase(const std::string& text) {
  std::string upper = text;
  for (char& letter : upper) {
    if (letter >= 'a' && letter <= 'z') {
      letter = static_cast<char>(letter - 'a' + 'A');
    }
  }
  return upper;
}

// Every file of the directory (not the lock) with its content.
std::map<std::string, std::string> Snapshot(const std::filesystem::path& dir) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename() == ".strata.lock") {
      continue;
    }
    if (entry.is_regular_file()) {
      files[entry.path().filename().string()] = ReadFile(entry.path());
    } else {
      files[entry.path().filename().string()] = "<not a regular file>";
    }
  }
  return files;
}

}  // namespace

TEST(test_import_infers_canonical_int_double_and_text) {
  const Cells header = {"id", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};
  const std::vector<Cells> rows = {
      {"1", "0", "007", "+5", "-0", "1.5", "1.50", "1", "1",
       "9223372036854775807", "9223372036854775808"},
      {"2", "-5", "1", "1", "1", "-2.25", "2.5", "2.5", "", "1", "1"},
      {"3", "123", "2", "2", "2", "3.0", "3.0", "3.0", "3", "2", "2"},
  };

  Inferred inferred = Infer(Request("t.csv"), Table(header, rows));

  CHECK(inferred.plan.has_value());
  if (!inferred.plan) {
    return;
  }
  CHECK_EQ(ColumnTypes(inferred.plan->schema),
           std::string("INT,INT,TEXT,TEXT,TEXT,DOUBLE,TEXT,TEXT,TEXT,INT,"
                       "TEXT"));
  CHECK_EQ(inferred.plan->rows.size(), std::size_t{3});
  // Every INT and DOUBLE cell is written back as it was read.
  for (std::size_t row = 0; row < rows.size(); ++row) {
    for (std::size_t column = 0; column < header.size(); ++column) {
      const domain::DataType type = inferred.plan->schema.columns[column].type;
      if (type == domain::DataType::kText) {
        continue;
      }
      CHECK_EQ(domain::FormatValue(inferred.plan->rows[row][column]),
               rows[row][column]);
    }
  }
}

TEST(test_import_infers_subnormal_cells_as_double) {
  const std::string tiny = domain::FormatValue(domain::Value(1e-310));
  domain::ImportRequest forced = Request("t.csv");
  forced.column_types = {{"v", domain::DataType::kDouble}};

  Inferred inferred =
      Infer(Request("t.csv"), Table({"id", "v"}, {{"1", tiny}, {"2", "1.5"}}));
  Inferred converted = Infer(forced, Table({"id", "v"}, {{"1", tiny}}));

  CHECK(inferred.plan.has_value());
  if (inferred.plan) {
    CHECK_EQ(ColumnTypes(inferred.plan->schema), std::string("INT,DOUBLE"));
    CHECK_EQ(domain::FormatValue(inferred.plan->rows[0][1]), tiny);
  }
  CHECK(converted.plan.has_value());
}

TEST(test_import_primary_key_rules) {
  Inferred by_id =
      Infer(Request("t.csv"), Table({"ID", "name"}, {{"1", "a"}, {"2", "b"}}));
  domain::ImportRequest named = Request("t.csv");
  named.primary_key = "name";
  Inferred by_name =
      Infer(named, Table({"code", "name"}, {{"x", "a"}, {"y", "b"}}));
  domain::ImportRequest unknown = Request("t.csv");
  unknown.primary_key = "nosuch";
  Inferred bad_name = Infer(unknown, Table({"id", "name"}, {{"1", "a"}}));
  Inferred no_id =
      Infer(Request("t.csv"), Table({"code", "name"}, {{"x", "a"}}));
  Inferred text_id =
      Infer(Request("t.csv"), Table({"id", "name"}, {{"a", "x"}, {"b", "y"}}));

  CHECK(by_id.plan.has_value());
  if (by_id.plan) {
    CHECK_EQ(by_id.plan->schema.primary_key_index, std::size_t{0});
  }
  CHECK(by_name.plan.has_value());
  if (by_name.plan) {
    CHECK_EQ(by_name.plan->schema.primary_key_index, std::size_t{1});
    CHECK_EQ(ColumnTypes(by_name.plan->schema), std::string("TEXT,TEXT"));
  }
  CHECK(!bad_name.plan.has_value());
  CHECK(Contains(bad_name.error, "nosuch"));
  CHECK(!no_id.plan.has_value());
  CHECK(no_id.error.starts_with("no primary key"));
  CHECK(!text_id.plan.has_value());
  CHECK(text_id.error.starts_with("no primary key"));
}

TEST(test_import_validates_table_and_column_names) {
  domain::ImportRequest key = Request("notes.csv");
  key.primary_key = "a";

  Inferred existing =
      Infer(Request("visitors.csv"), Table({"id", "name"}, {{"1", "a"}}));
  Inferred fine = Infer(key, Table({"a", "b"}, {{"1", "x"}}));
  Inferred spaced =
      Infer(Request("my notes.csv"), Table({"id", "name"}, {{"1", "a"}}));
  Inferred reserved =
      Infer(Request("catalog.csv"), Table({"id", "name"}, {{"1", "a"}}));
  domain::ImportRequest explicit_reserved = Request("x.csv");
  explicit_reserved.table = "Catalog";
  Inferred named_reserved =
      Infer(explicit_reserved, Table({"id", "name"}, {{"1", "a"}}));
  Inferred twice = Infer(key, Table({"a", "A"}, {{"1", "x"}}));
  Inferred with_space = Infer(key, Table({"first name", "b"}, {{"1", "x"}}));
  Inferred digit_first = Infer(key, Table({"1a", "b"}, {{"1", "x"}}));
  Inferred empty_name = Infer(key, Table({"a", ""}, {{"1", "x"}}));

  CHECK(!existing.plan.has_value());
  CHECK(Contains(existing.error, "table visitors already exists"));
  CHECK(fine.plan.has_value());
  if (fine.plan) {
    CHECK_EQ(fine.plan->schema.name, std::string("notes"));
  }
  CHECK(!spaced.plan.has_value());
  CHECK(Contains(spaced.error, "cannot derive a table name from 'my notes'"));
  CHECK(!reserved.plan.has_value());
  CHECK(Contains(reserved.error, "catalog"));
  CHECK(!named_reserved.plan.has_value());
  CHECK(Contains(named_reserved.error, "table name catalog is reserved"));
  CHECK(Contains(twice.error, "column 'A' appears twice"));
  CHECK(Contains(with_space.error, "invalid column name 'first name'"));
  CHECK(Contains(digit_first.error, "invalid column name '1a'"));
  CHECK(Contains(empty_name.error, "invalid column name ''"));
}

TEST(test_import_rejects_reserved_words_as_names) {
  // The reserved words of docs/DesignDocument.md section 2.3.
  const char* const reserved_words[] = {
      "select", "from",  "where",  "join", "on",     "as",   "group",
      "by",     "order", "asc",    "desc", "insert", "into", "values",
      "update", "set",   "delete", "and",  "or",     "like", "limit",
      "offset", "count", "sum",    "null"};
  // Function names, DDL words and unsupported words are identifiers.
  const char* const ordinary_words[] = {"min",    "max",   "avg", "lower",
                                        "create", "table", "key", "default",
                                        "not",    "in"};
  domain::ImportRequest keyed = Request("t.csv");
  keyed.primary_key = "id";

  for (const char* word : reserved_words) {
    const std::string name(word);
    Inferred column = Infer(keyed, Table({"id", name}, {{"1", "a"}}));
    domain::ImportRequest named = Request("t.csv");
    named.table = UpperCase(name);
    Inferred table = Infer(named, Table({"id", "v"}, {{"1", "a"}}));
    Inferred derived =
        Infer(Request(name + ".csv"), Table({"id", "v"}, {{"1", "a"}}));
    auto parsed = adapter::ParseStatement("CREATE TABLE " + name +
                                          " (id INT PRIMARY KEY)");

    CHECK_EQ(column.error, "invalid column name '" + name +
                               "' in the header; it is a reserved word");
    CHECK_EQ(table.error, "invalid table name '" + UpperCase(name) +
                              "'; it is a reserved word");
    CHECK_EQ(derived.error, "cannot derive a table name from '" + name +
                                "'; give one: .import <file> <table>");
    // The parser refuses the same word, so the two lists agree.
    CHECK(std::holds_alternative<adapter::ParseError>(parsed));
  }
  for (const char* word : ordinary_words) {
    const std::string name(word);
    Inferred column = Infer(keyed, Table({"id", name}, {{"1", "a"}}));
    Inferred derived =
        Infer(Request(name + ".csv"), Table({"id", "v"}, {{"1", "a"}}));
    auto parsed = adapter::ParseStatement("CREATE TABLE " + name +
                                          " (id INT PRIMARY KEY)");

    CHECK(column.plan.has_value());
    CHECK(derived.plan.has_value());
    CHECK(!std::holds_alternative<adapter::ParseError>(parsed));
  }
}

TEST(test_import_strict_rejects_bad_rows_and_skip_reports_them) {
  std::vector<domain::CsvLine> lines = {
      Line(1, {"id", "name", "score"}),
      Line(2, {"1", "a", "5"}),
      Line(3, {"2", "b"}),
      Line(4, {"3", "c", "7"}),
      MalformedLine(5),
      Line(6, {"5", "e", "1", "extra"}),
  };
  domain::ImportRequest skip = Request("t.csv");
  skip.should_skip_bad_rows = true;

  Inferred strict = Infer(Request("t.csv"), lines);
  Inferred skipped = Infer(skip, lines);

  CHECK(!strict.plan.has_value());
  CHECK_EQ(strict.error, std::string("line 3: expected 3 fields, found 2"));
  CHECK(skipped.plan.has_value());
  if (!skipped.plan) {
    return;
  }
  CHECK_EQ(skipped.plan->rows.size(), std::size_t{2});
  CHECK_EQ(skipped.plan->skipped_count, std::size_t{3});
  CHECK_EQ(
      skipped.plan->notes,
      std::vector<std::string>({"skipped line 3: expected 3 fields, found 2",
                                "skipped line 5: malformed quoting",
                                "skipped line 6: expected 3 fields, found 4"}));

  // Twelve bad rows: ten are listed, the rest are counted.
  std::vector<domain::CsvLine> many = {Line(1, {"id", "v"})};
  for (std::size_t line = 2; line <= 13; ++line) {
    many.push_back(Line(line, {"x", "1", "2"}));
  }
  many.push_back(Line(14, {"1", "a"}));
  Inferred truncated = Infer(skip, many);
  CHECK(truncated.plan.has_value());
  if (!truncated.plan) {
    return;
  }
  CHECK_EQ(truncated.plan->skipped_count, std::size_t{12});
  CHECK_EQ(truncated.plan->notes.size(), std::size_t{11});
  CHECK_EQ(truncated.plan->notes[0],
           std::string("skipped line 2: expected 2 fields, found 3"));
  CHECK_EQ(truncated.plan->notes[9],
           std::string("skipped line 11: expected 2 fields, found 3"));
  CHECK_EQ(truncated.plan->notes[10], std::string("... and 2 more skipped"));
}

TEST(test_import_type_overrides_convert_or_reject) {
  const Cells header = {"id", "score", "name"};
  std::vector<domain::CsvLine> lines = Table(
      header, {{"007", "10", "x"}, {"+5", "2.5", "y"}, {"12x", "1", "z"}});
  domain::ImportRequest declared = Request("t.csv");
  declared.column_types = {{"ID", domain::DataType::kInt},
                           {"score", domain::DataType::kDouble},
                           {"name", domain::DataType::kText}};
  domain::ImportRequest skipping = declared;
  skipping.should_skip_bad_rows = true;
  domain::ImportRequest unknown = declared;
  unknown.column_types.emplace_back("nosuch", domain::DataType::kInt);
  domain::ImportRequest twice = declared;
  twice.column_types.emplace_back("score", domain::DataType::kInt);

  Inferred strict = Infer(declared, lines);
  Inferred skipped = Infer(skipping, lines);
  Inferred bad_column = Infer(unknown, lines);
  Inferred repeated = Infer(twice, lines);

  CHECK(!strict.plan.has_value());
  CHECK_EQ(strict.error, std::string("line 4: id '12x' is not INT"));
  CHECK(skipped.plan.has_value());
  if (skipped.plan) {
    CHECK_EQ(skipped.plan->rows.size(), std::size_t{2});
    CHECK_EQ(domain::FormatValue(skipped.plan->rows[0][0]), std::string("7"));
    CHECK_EQ(domain::FormatValue(skipped.plan->rows[1][0]), std::string("5"));
    CHECK_EQ(domain::FormatValue(skipped.plan->rows[0][1]),
             std::string("10.0"));
    CHECK_EQ(skipped.plan->notes,
             std::vector<std::string>({"skipped line 4: id '12x' is not INT"}));
  }
  CHECK(!bad_column.plan.has_value());
  CHECK(Contains(bad_column.error, "unknown column 'nosuch'"));
  CHECK(!repeated.plan.has_value());
  CHECK(Contains(repeated.error, "column 'score' twice"));
}

TEST(test_import_duplicate_primary_key_is_an_error_even_when_skipping) {
  std::vector<domain::CsvLine> lines = Table(
      {"id", "name"},
      {{"1", "a"}, {"5", "b"}, {"2", "c"}, {"3", "d"}, {"4", "e"}, {"5", "f"}});
  domain::ImportRequest skip = Request("t.csv");
  skip.should_skip_bad_rows = true;

  Inferred strict = Infer(Request("t.csv"), lines);
  Inferred skipping = Infer(skip, lines);

  CHECK(!strict.plan.has_value());
  CHECK(!skipping.plan.has_value());
  for (const std::string& message : {strict.error, skipping.error}) {
    CHECK(Contains(message, "duplicate value '5'"));
    CHECK(Contains(message, "lines 3 and 7"));
  }
}

TEST(test_import_treats_negative_zero_as_a_repeated_key) {
  domain::ImportRequest request = Request("t.csv");
  request.primary_key = "k";

  Inferred inferred =
      Infer(request, Table({"k", "v"}, {{"0.0", "a"}, {"-0.0", "b"}}));

  CHECK(!inferred.plan.has_value());
  CHECK_EQ(inferred.error,
           std::string("primary key column k has duplicate value '-0.0' at "
                       "lines 2 and 3"));
}

TEST(test_import_handles_header_only_and_empty_input) {
  domain::ImportRequest keyed = Request("t.csv");
  keyed.primary_key = "id";

  Inferred header_only = Infer(keyed, Table({"id", "name"}, {}));
  Inferred header_only_without_key =
      Infer(Request("t.csv"), Table({"id", "name"}, {}));
  Inferred empty = Infer(keyed, {});
  Inferred malformed = Infer(keyed, {MalformedLine(1)});

  CHECK(header_only.plan.has_value());
  if (header_only.plan) {
    CHECK_EQ(ColumnTypes(header_only.plan->schema), std::string("TEXT,TEXT"));
    CHECK(header_only.plan->rows.empty());
  }
  CHECK(!header_only_without_key.plan.has_value());
  CHECK(header_only_without_key.error.starts_with("no primary key"));
  CHECK(!empty.plan.has_value());
  CHECK(Contains(empty.error, "has no header line"));
  CHECK(!malformed.plan.has_value());
  CHECK(Contains(malformed.error, "malformed header"));
}

// ---- Integration ------------------------------------------------------------

TEST(test_import_checks_a_wide_header_in_linear_time) {
  // A header of 60000 names costs tens of seconds when each name is compared
  // with every earlier one, and milliseconds when the names are looked up in a
  // set.
  constexpr std::size_t kColumnCount = 60000;
  constexpr double kSecondsLimit = 2.0;
  Cells header = {"id"};
  for (std::size_t column = 1; column < kColumnCount; ++column) {
    header.push_back("c" + std::to_string(column));
  }
  Cells repeated = header;
  repeated.back() = "C5";  // the same name as c5, in the other case
  const domain::ImportRequest request = Request("wide.csv");

  const auto start = std::chrono::steady_clock::now();
  Inferred unique_names = Infer(request, Table(header, {}));
  Inferred repeated_names = Infer(request, Table(repeated, {}));
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;

  CHECK(!Contains(unique_names.error, "appears twice"));
  CHECK(!Contains(unique_names.error, "invalid column name"));
  CHECK(Contains(repeated_names.error, "column 'C5' appears twice"));
  CHECK(elapsed.count() < kSecondsLimit);
}

TEST(test_service_import_creates_table_and_round_trips_bytes) {
  TempDir db;
  TempDir source_dir;
  const std::string source_text =
      "id,name,code,qty,ratio,note\n"
      "1,\"Smith, J\",007,10,1.5,\"say \"\"hi\"\"\"\n"
      "2,Bo,008,20,2.25,plain\n";
  const std::filesystem::path source = source_dir.path() / "items.csv";
  WriteFile(source, source_text);
  auto service = MakeLoadedService(db.path());
  if (!service) {
    return;
  }

  domain::ExecutionResult result = service->ImportCsv(Request(source.string()));

  const domain::CommandComplete* complete = AsCommandComplete(result);
  CHECK(complete != nullptr);
  if (complete != nullptr) {
    CHECK_EQ(complete->tag, std::string("IMPORT items (imported 2 rows)"));
    CHECK(complete->notes.empty());
  }
  std::vector<domain::TableSchema> schemas = service->DescribeCatalog();
  CHECK_EQ(schemas.size(), std::size_t{1});
  if (schemas.size() == 1) {
    CHECK_EQ(ColumnTypes(schemas[0]),
             std::string("INT,TEXT,TEXT,INT,DOUBLE,TEXT"));
  }
  CHECK_EQ(ReadFile(db.path() / "items.csv"), source_text);
  CHECK(Contains(ReadFile(db.path() / "catalog.csv"), "items,0,id,INT,1"));
  CHECK_EQ(ReadFile(source), source_text);

  auto reloaded = MakeLoadedService(db.path());
  if (!reloaded) {
    return;
  }
  CHECK_EQ(RowsAsText(*AsResultSet(
               ExecuteSql(*reloaded,
                          "SELECT id, name, code, qty, ratio, note FROM items "
                          "ORDER BY id"))),
           std::vector<std::string>({"1\tSmith, J\t007\t10\t1.5\tsay \"hi\"",
                                     "2\tBo\t008\t20\t2.25\tplain"}));

  // Rewriting the table by an UPDATE changes only the updated cell.
  ExecuteSql(*reloaded, "UPDATE items SET qty = 11 WHERE id = 1");
  std::string expected = source_text;
  expected.replace(expected.find(",10,"), 4, ",11,");
  CHECK_EQ(ReadFile(db.path() / "items.csv"), expected);
}

TEST(test_service_import_failures_leave_the_database_unchanged) {
  TempDir db;
  WriteFixtureA(db.path());
  TempDir source_dir;
  WriteFile(source_dir.path() / "visitors.csv", "id,name\n1,a\n");
  WriteFile(source_dir.path() / "bad.csv", "id,name\n1,a\n2\n");
  WriteFile(source_dir.path() / "dup.csv", "id,name\n1,a\n1,b\n");
  WriteFile(source_dir.path() / "fine.csv", "id,name\n1,a\n");
  auto log = std::make_shared<RepositoryLog>();
  auto service = MakeLoadedService(db.path(), log);
  if (!service) {
    return;
  }
  const std::map<std::string, std::string> before = Snapshot(db.path());
  const std::string path = source_dir.path().string() + "/";

  domain::ExecutionResult missing =
      service->ImportCsv(Request(path + "missing.csv"));
  domain::ExecutionResult existing =
      service->ImportCsv(Request(path + "visitors.csv"));
  domain::ExecutionResult bad_row =
      service->ImportCsv(Request(path + "bad.csv"));
  domain::ExecutionResult duplicate =
      service->ImportCsv(Request(path + "dup.csv"));
  log->write_failure = "injected commit failure";
  domain::ExecutionResult commit_failure =
      service->ImportCsv(Request(path + "fine.csv"));

  const domain::ExecutionError* errors[] = {
      AsError(missing), AsError(existing), AsError(bad_row), AsError(duplicate),
      AsError(commit_failure)};
  const char* kinds[] = {"io", "bind", "bind", "bind", "io"};
  const char* fragments[] = {"cannot read ", "table visitors already exists",
                             "line 3: expected 2 fields, found 1",
                             "duplicate value '1'", "injected commit failure"};
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(errors[i] != nullptr);
    if (errors[i] == nullptr) {
      continue;
    }
    CHECK_EQ(std::string(domain::ErrorKindName(errors[i]->kind)),
             std::string(kinds[i]));
    CHECK(Contains(errors[i]->message, fragments[i]));
  }
  CHECK(Snapshot(db.path()) == before);
  CHECK_EQ(service->DescribeCatalog().size(), std::size_t{3});
}

TEST(test_service_import_refuses_an_existing_target_file) {
  // visitors.csv lies in the database directory and is the source itself; a
  // directory takes the place of stories.csv.
  TempDir db;
  const std::string visitors_text = "id,name\n1,a\n2,b\nx,bad\n";
  WriteFile(db.path() / "visitors.csv", visitors_text);
  std::filesystem::create_directory(db.path() / "stories.csv");
  TempDir source_dir;
  WriteFile(source_dir.path() / "stories.csv", "id,body\n1,x\n");
  auto service = MakeLoadedService(db.path());
  if (!service) {
    return;
  }
  const std::map<std::string, std::string> before = Snapshot(db.path());
  domain::ImportRequest itself = Request((db.path() / "visitors.csv").string());
  itself.primary_key = "id";
  itself.column_types = {{"id", domain::DataType::kInt}};
  itself.should_skip_bad_rows = true;

  domain::ExecutionResult source_in_place = service->ImportCsv(itself);
  domain::ExecutionResult directory_in_place =
      service->ImportCsv(Request((source_dir.path() / "stories.csv").string()));

  const domain::ExecutionError* errors[] = {AsError(source_in_place),
                                            AsError(directory_in_place)};
  const char* messages[] = {
      "visitors.csv already exists; use CREATE TABLE to adopt it",
      "stories.csv already exists and is not a regular file"};
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(errors[i] != nullptr);
    if (errors[i] == nullptr) {
      continue;
    }
    CHECK_EQ(std::string(domain::ErrorKindName(errors[i]->kind)),
             std::string("io"));
    CHECK_EQ(errors[i]->message, std::string(messages[i]));
  }
  CHECK(Snapshot(db.path()) == before);
  CHECK_EQ(ReadFile(db.path() / "visitors.csv"), visitors_text);
  CHECK(service->DescribeCatalog().empty());

  // The alternative the message names: adoption keeps the file as it is.
  const domain::ExecutionResult create = ExecuteSql(
      *service, "CREATE TABLE visitors (id INT PRIMARY KEY, name TEXT)");
  const domain::CommandComplete* adopted = AsCommandComplete(create);
  CHECK(adopted != nullptr);
  if (adopted != nullptr) {
    CHECK_EQ(adopted->tag,
             std::string("CREATE TABLE visitors (adopted 2 rows)"));
  }
  CHECK_EQ(ReadFile(db.path() / "visitors.csv"), visitors_text);
}

TEST(test_service_import_skip_bad_rows_reports_notes) {
  TempDir db;
  TempDir source_dir;
  std::string text = "id,v\n";
  for (int i = 0; i < 12; ++i) {
    text += "x,1,2\n";
  }
  for (int i = 1; i <= 5; ++i) {
    text += std::to_string(i) + ",a\n";
  }
  WriteFile(source_dir.path() / "bad.csv", text);
  auto service = MakeLoadedService(db.path());
  if (!service) {
    return;
  }
  domain::ImportRequest request =
      Request((source_dir.path() / "bad.csv").string());
  request.should_skip_bad_rows = true;

  domain::ExecutionResult result = service->ImportCsv(request);

  const domain::CommandComplete* complete = AsCommandComplete(result);
  CHECK(complete != nullptr);
  if (complete == nullptr) {
    return;
  }
  CHECK_EQ(complete->tag,
           std::string("IMPORT bad (imported 5 rows, skipped 12)"));
  CHECK_EQ(complete->notes.size(), std::size_t{11});
  CHECK_EQ(complete->notes[0],
           std::string("skipped line 2: expected 2 fields, found 3"));
  CHECK_EQ(complete->notes[10], std::string("... and 2 more skipped"));
  CHECK_EQ(ReadFile(db.path() / "bad.csv"),
           std::string("id,v\n1,a\n2,a\n3,a\n4,a\n5,a\n"));
}

TEST(test_file_csv_source_reads_lines_bom_crlf_and_quotes) {
  TempDir dir;
  WriteFile(dir.path() / "bom.csv",
            "\xEF\xBB\xBF"
            "id,name\n1,a\n");
  WriteFile(dir.path() / "crlf.csv", "id,name\r\n1,a\r\n");
  WriteFile(dir.path() / "quotes.csv",
            "id,name\n\n1,\"a,b\"\n2,\"say \"\"x\"\"\"\n3,\"open\n");
  infrastructure::FileCsvSource source;

  auto bom = source.Read((dir.path() / "bom.csv").string());
  auto crlf = source.Read((dir.path() / "crlf.csv").string());
  auto quotes = source.Read((dir.path() / "quotes.csv").string());
  auto missing = source.Read((dir.path() / "missing.csv").string());
  auto directory = source.Read(dir.path().string());

  auto* bom_lines = std::get_if<std::vector<domain::CsvLine>>(&bom);
  CHECK(bom_lines != nullptr && bom_lines->size() == 2);
  if (bom_lines != nullptr && bom_lines->size() == 2) {
    CHECK_EQ(*(*bom_lines)[0].fields, Cells({"id", "name"}));
  }
  auto* crlf_lines = std::get_if<std::vector<domain::CsvLine>>(&crlf);
  CHECK(crlf_lines != nullptr && crlf_lines->size() == 2);
  if (crlf_lines != nullptr && crlf_lines->size() == 2) {
    CHECK_EQ(*(*crlf_lines)[1].fields, Cells({"1", "a"}));
  }
  auto* quote_lines = std::get_if<std::vector<domain::CsvLine>>(&quotes);
  CHECK(quote_lines != nullptr && quote_lines->size() == 5 - 1);
  if (quote_lines != nullptr && quote_lines->size() == 4) {
    CHECK_EQ((*quote_lines)[1].number, std::size_t{3});
    CHECK_EQ(*(*quote_lines)[1].fields, Cells({"1", "a,b"}));
    CHECK_EQ(*(*quote_lines)[2].fields, Cells({"2", "say \"x\""}));
    CHECK_EQ((*quote_lines)[3].number, std::size_t{5});
    CHECK(!(*quote_lines)[3].fields.has_value());
  }
  CHECK(std::holds_alternative<std::string>(missing));
  CHECK(std::holds_alternative<std::string>(directory));
  if (auto* reason = std::get_if<std::string>(&missing)) {
    CHECK(Contains(*reason, "cannot read "));
  }
}

}  // namespace strata::testing
