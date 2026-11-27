// Tests of the pure CSV and catalog conversion in csv_codec.h.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/csv_codec.h"
#include "test_harness.h"

namespace {

namespace domain = strata::domain;
namespace infrastructure = strata::infrastructure;

using Fields = std::vector<std::string>;

constexpr const char* kCatalogHeader =
    "table,position,column,type,primary_key,unique,auto_increment,has_default,"
    "default,ref_table,ref_column\n";

void ReportError(const std::string& message) {
  strata::testing::ReportFailure(__FILE__, __LINE__, message);
}

// The fields of line, or an empty list after reporting a failure.
Fields MustParseRecord(const std::string& line) {
  std::optional<Fields> fields = infrastructure::ParseCsvRecord(line);
  if (!fields) {
    ReportError("could not parse the record [" + line + "]");
    return Fields();
  }
  return *fields;
}

domain::ColumnDef Column(const std::string& name, domain::DataType type) {
  return domain::ColumnDef{name, type};
}

domain::TableSchema MixedSchema() {
  return domain::TableSchema{"t",
                             {Column("a", domain::DataType::kInt),
                              Column("b", domain::DataType::kDouble),
                              Column("c", domain::DataType::kText)},
                             0,
                             {}};
}

// visitors, stories and a table with an auto-increment key and defaults.
std::vector<domain::TableSchema> CatalogSchemas() {
  domain::TableSchema visitors{"visitors",
                               {Column("id", domain::DataType::kInt),
                                Column("username", domain::DataType::kText),
                                Column("location", domain::DataType::kText)},
                               0,
                               {},
                               {1}};
  domain::TableSchema stories{"stories",
                              {Column("id", domain::DataType::kInt),
                               Column("content", domain::DataType::kText),
                               Column("username", domain::DataType::kText),
                               Column("views", domain::DataType::kInt)},
                              0,
                              {domain::ForeignKey{2, "visitors", 1}}};
  domain::ColumnDef name = Column("name", domain::DataType::kText);
  name.default_value = domain::Value(std::string("a,b"));
  domain::ColumnDef ratio = Column("ratio", domain::DataType::kDouble);
  ratio.default_value = domain::Value(0.5);
  domain::ColumnDef count = Column("n", domain::DataType::kInt);
  count.default_value = domain::Value(int64_t{-3});
  domain::TableSchema jobs{
      "jobs", {Column("id", domain::DataType::kInt), name, ratio, count},
      0,      {},
      {},     std::optional<std::size_t>(0)};
  return {visitors, stories, jobs};
}

bool SameForeignKey(const domain::ForeignKey& lhs,
                    const domain::ForeignKey& rhs) {
  return lhs.column_index == rhs.column_index &&
         lhs.ref_table == rhs.ref_table &&
         lhs.ref_column_index == rhs.ref_column_index;
}

bool SameSchema(const domain::TableSchema& lhs,
                const domain::TableSchema& rhs) {
  if (lhs.name != rhs.name || lhs.columns.size() != rhs.columns.size() ||
      lhs.primary_key_index != rhs.primary_key_index ||
      lhs.unique_indexes != rhs.unique_indexes ||
      lhs.auto_increment_index != rhs.auto_increment_index ||
      lhs.foreign_keys.size() != rhs.foreign_keys.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.columns.size(); ++i) {
    if (lhs.columns[i].name != rhs.columns[i].name ||
        lhs.columns[i].type != rhs.columns[i].type ||
        lhs.columns[i].default_value != rhs.columns[i].default_value) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.foreign_keys.size(); ++i) {
    if (!SameForeignKey(lhs.foreign_keys[i], rhs.foreign_keys[i])) {
      return false;
    }
  }
  return true;
}

// The description ParseCatalog gives for text, or empty after reporting that it
// parsed.
std::string CatalogError(const std::string& text) {
  auto parsed = infrastructure::ParseCatalog(text);
  if (const auto* error = std::get_if<std::string>(&parsed)) {
    return *error;
  }
  ReportError("the catalog parsed although it is invalid:\n" + text);
  return std::string();
}

}  // namespace

TEST(test_csv_parse_record_splits_plain_fields) {
  CHECK_EQ(MustParseRecord("1,alice,Paris"), Fields({"1", "alice", "Paris"}));
  CHECK_EQ(MustParseRecord("a,,c"), Fields({"a", "", "c"}));
  CHECK_EQ(MustParseRecord(","), Fields({"", ""}));
  CHECK_EQ(MustParseRecord(""), Fields({""}));
}

TEST(test_csv_parse_record_reads_quoted_fields) {
  CHECK_EQ(MustParseRecord("1,\"a,b\",c"), Fields({"1", "a,b", "c"}));
  CHECK_EQ(MustParseRecord("\"say \"\"hi\"\"\""), Fields({"say \"hi\""}));
  CHECK_EQ(MustParseRecord("\"\",x"), Fields({"", "x"}));
  CHECK_EQ(MustParseRecord("\"a\"\"\",b"), Fields({"a\"", "b"}));
}

TEST(test_csv_parse_record_rejects_malformed_quotes) {
  const char* const malformed[] = {"\"abc", "\"ab\"c", "a,\"b", "\"a\"\""};

  for (const char* line : malformed) {
    CHECK(!infrastructure::ParseCsvRecord(line).has_value());
  }
  CHECK_EQ(MustParseRecord("a\"b,c"), Fields({"a\"b", "c"}));
}

TEST(test_csv_format_field_quotes_only_when_needed) {
  CHECK_EQ(infrastructure::FormatCsvField("plain"), std::string("plain"));
  CHECK_EQ(infrastructure::FormatCsvField("a,b"), std::string("\"a,b\""));
  CHECK_EQ(infrastructure::FormatCsvField("say \"hi\""),
           std::string("\"say \"\"hi\"\"\""));
  CHECK_EQ(infrastructure::FormatCsvField(""), std::string(""));
  CHECK_EQ(infrastructure::FormatCsvField(" lead"), std::string(" lead"));
}

TEST(test_csv_serialize_row_quotes_a_lone_empty_field) {
  const domain::Row lone_empty = {domain::Value(std::string())};
  const domain::Row lone_text = {domain::Value(std::string("x"))};
  const domain::Row two_empty = {domain::Value(std::string()),
                                 domain::Value(std::string())};
  const domain::Row trailing_empty = {domain::Value(std::int64_t{1}),
                                      domain::Value(std::string())};

  // Only a row whose single field is empty would make a blank line.
  CHECK_EQ(infrastructure::SerializeRow(lone_empty), std::string("\"\""));
  CHECK_EQ(infrastructure::SerializeRow(lone_text), std::string("x"));
  CHECK_EQ(infrastructure::SerializeRow(two_empty), std::string(","));
  CHECK_EQ(infrastructure::SerializeRow(trailing_empty), std::string("1,"));
  CHECK_EQ(MustParseRecord("\"\""), Fields({""}));
}

TEST(test_csv_field_round_trip_preserves_text_values) {
  const char* const values[] = {"",   "plain", "a,b",    "say \"hi\"",
                                "\"", ",",     "a\"\"b", " lead "};

  for (const char* value : values) {
    std::string written = infrastructure::FormatCsvField(value);
    CHECK_EQ(MustParseRecord(written), Fields({value}));
  }
}

TEST(test_csv_parse_row_applies_column_types) {
  const domain::TableSchema schema = MixedSchema();
  const std::vector<Fields> rejected = {
      {"+1", "2.5", "x"},     {"9223372036854775808", "1", "x"},
      {"1", "abc", "x"},      {"1", "inf", "x"},
      {"1", "nan", "x"},      {"1", "2.5"},
      {"1", "2.5", "x", "y"}, {"1", " 2", "x"},
      {"1", "1e-400", "x"},   {"1", "1e999", "x"},
  };

  std::optional<domain::Row> largest = infrastructure::ParseRow(
      Fields({"9223372036854775807", "-0.5", "a"}), schema);
  std::optional<domain::Row> smallest = infrastructure::ParseRow(
      Fields({"-9223372036854775808", "1", "x"}), schema);

  CHECK(largest.has_value());
  CHECK(smallest.has_value());
  if (largest && smallest) {
    CHECK_EQ(std::get<int64_t>((*largest)[0]), int64_t{9223372036854775807});
    CHECK_EQ(std::get<double>((*largest)[1]), -0.5);
    CHECK_EQ(std::get<std::string>((*largest)[2]), std::string("a"));
    CHECK_EQ(std::get<int64_t>((*smallest)[0]),
             std::numeric_limits<int64_t>::min());
  }
  for (const Fields& fields : rejected) {
    CHECK(!infrastructure::ParseRow(fields, schema).has_value());
  }
}

TEST(test_catalog_round_trip_preserves_schema_fields) {
  const std::vector<domain::TableSchema> schemas = CatalogSchemas();

  const std::string text = infrastructure::SerializeCatalog(schemas);
  auto parsed = infrastructure::ParseCatalog(text);

  CHECK_EQ(text.substr(0, std::string(kCatalogHeader).size()),
           std::string(kCatalogHeader));
  CHECK(text.find("\"a,b\"") != std::string::npos);
  const auto* round_trip =
      std::get_if<std::vector<domain::TableSchema>>(&parsed);
  CHECK(round_trip != nullptr);
  if (round_trip == nullptr || round_trip->size() != schemas.size()) {
    CHECK(false);
    return;
  }
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    CHECK(SameSchema(schemas[i], (*round_trip)[i]));
  }
}

TEST(test_catalog_parse_rejects_invalid_files) {
  const std::string header = kCatalogHeader;
  const std::string visitors =
      "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,username,TEXT,0,1,0,0,,,\n"
      "visitors,2,location,TEXT,0,0,0,0,,,\n";
  struct Case {
    std::string text;
    std::string mentions;  // a name the description must contain
  };
  const std::vector<Case> cases = {
      {header + "../victim,0,id,INT,1,0,0,0,,,\n", "../victim"},
      {header +
           "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,\"a,b\",TEXT,0,0,0,0,,,\n",
       "a,b"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" + visitors,
       "visitors"},
      {header + "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,ID,TEXT,0,0,0,0,,,\n",
       "ID"},
      {header +
           "visitors,0,id,INT,1,0,0,0,,,\nvisitors,2,name,TEXT,0,0,0,0,,,\n",
       "visitors"},
      {header + "visitors,0,id,BIGINT,1,0,0,0,,,\n", "BIGINT"},
      {header + "visitors,0,id,INT,0,0,0,0,,,\n", "visitors"},
      {header + "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,b,INT,1,0,0,0,,,\n",
       "visitors"},
      {header + "visitors,0,id,TEXT,1,0,1,0,,,\n", "id"},
      {header +
           "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,n,INT,0,0,0,1,abc,,\n",
       "n"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" +
           "stories,1,owner,TEXT,0,0,0,0,,nosuch,name\n",
       "nosuch"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" +
           "stories,1,owner,TEXT,0,0,0,0,,visitors,missing\n",
       "missing"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" +
           "stories,1,place,TEXT,0,0,0,0,,visitors,location\n",
       "location"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" +
           "stories,1,owner,INT,0,0,0,0,,visitors,username\n",
       "username"},
      {header + "catalog,0,id,INT,1,0,0,0,,,\n", "catalog"},
      {header + "visitors,0,id,INT,1,0,0,0,,\n", "fields"},
      {header + ",0,id,INT,1,0,0,0,,,\n", "must not be empty"},
      {header + "visitors,x,id,INT,1,0,0,0,,,\n", "is not a number"},
      {header + "visitors,0,id,INT,2,0,0,0,,,\n", "must be 0 or 1"},
      {header + "visitors,0,id,INT,1,0,0,0,,,\nvisitors,1,n,INT,0,0,0,0,5,,\n",
       "has a default but no flag"},
      {header + visitors + "stories,0,id,INT,1,0,0,0,,,\n" +
           "stories,1,owner,TEXT,0,0,0,0,,visitors,\n",
       "go together"},
      {header + "visitors,0,id,INT,1,1,0,0,,,\n",
       "cannot be marked unique or have a default"},
      {header + "visitors,0,id,INT,1,0,0,1,5,,\n",
       "cannot be marked unique or have a default"},
  };

  for (const Case& test_case : cases) {
    const std::string description = CatalogError(test_case.text);
    CHECK(description.find(test_case.mentions) != std::string::npos);
  }
}
