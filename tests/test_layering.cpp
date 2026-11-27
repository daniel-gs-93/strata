#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "test_harness.h"

#ifndef STRATA_SOURCE_DIR
#error "STRATA_SOURCE_DIR must be defined by the build"
#endif

namespace {

enum class Layer {
  kDomain,
  kApplication,
  kInfrastructure,
  kPaged,
  kParser,
  kBootstrap,
  kCli,
  kUnknown
};

const char* const kInfrastructureHeaders[] = {
    "strata/infrastructure.h",     "strata/flat_file_engine.h",
    "strata/flat_file_executor.h", "strata/csv_codec.h",
    "strata/directory_lock.h",     "strata/csv_file_source.h"};

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.starts_with(prefix);
}

bool IsInfrastructureHeader(const std::string& header) {
  for (const char* candidate : kInfrastructureHeaders) {
    if (header == candidate) {
      return true;
    }
  }
  return false;
}

// Layer of a source or header file, by its name relative to include/ or src/.
Layer LayerOf(const std::filesystem::path& relative) {
  const std::string path = relative.generic_string();
  if (StartsWith(path, "strata/domain/")) {
    return Layer::kDomain;
  }
  const std::set<std::string> domain_sources = {
      "value.cpp", "expression.cpp", "binder.cpp", "scalar_functions.cpp",
      "import_inference.cpp"};
  if (domain_sources.contains(path)) {
    return Layer::kDomain;
  }
  if (path == "strata/application.h" || path == "application.cpp") {
    return Layer::kApplication;
  }
  if (StartsWith(path, "strata/paged/") || StartsWith(path, "paged/")) {
    return Layer::kPaged;
  }
  const std::set<std::string> infrastructure_files = {
      "strata/infrastructure.h",     "strata/flat_file_engine.h",
      "strata/flat_file_executor.h", "strata/csv_codec.h",
      "strata/directory_lock.h",     "strata/csv_file_source.h",
      "infrastructure.cpp",          "flat_file_engine.cpp",
      "flat_file_executor.cpp",      "csv_codec.cpp",
      "csv_file_source.cpp",         "directory_lock.cpp"};
  if (infrastructure_files.contains(path)) {
    return Layer::kInfrastructure;
  }
  if (path == "strata/sql_parser.h" || path == "sql_parser.cpp") {
    return Layer::kParser;
  }
  if (path == "strata/bootstrap.h" || path == "bootstrap.cpp") {
    return Layer::kBootstrap;
  }
  const std::set<std::string> cli_files = {
      "cli.cpp", "repl.cpp", "statement_buffer.cpp", "strata/repl.h",
      "strata/statement_buffer.h"};
  if (cli_files.contains(path)) {
    return Layer::kCli;
  }
  return Layer::kUnknown;
}

// The header named by an `#include "strata/..."` line, or empty.
std::string ProjectHeaderOf(const std::string& line) {
  const std::string marker = "#include \"strata/";
  if (!StartsWith(line, marker)) {
    return "";
  }
  std::size_t begin = std::string("#include \"").size();
  std::size_t end = line.find('"', begin);
  if (end == std::string::npos) {
    return "";
  }
  return line.substr(begin, end - begin);
}

bool IsAllowed(Layer layer, const std::string& header) {
  switch (layer) {
    case Layer::kDomain:
      return StartsWith(header, "strata/domain/");
    case Layer::kApplication:
      return StartsWith(header, "strata/domain/") ||
             header == "strata/application.h";
    case Layer::kInfrastructure:
      return StartsWith(header, "strata/domain/") ||
             IsInfrastructureHeader(header);
    case Layer::kPaged:
      return StartsWith(header, "strata/domain/") ||
             StartsWith(header, "strata/paged/");
    case Layer::kParser:
      return StartsWith(header, "strata/domain/") ||
             header == "strata/sql_parser.h";
    case Layer::kBootstrap:
      // The composition root: the only file outside Infrastructure that may
      // name infrastructure types, and (bootstrap.cpp alone, see
      // CheckPagedRules) paged types.
      return StartsWith(header, "strata/domain/") ||
             header == "strata/application.h" ||
             header == "strata/bootstrap.h" ||
             StartsWith(header, "strata/paged/") ||
             IsInfrastructureHeader(header);
    case Layer::kCli:
      return StartsWith(header, "strata/domain/") ||
             header == "strata/application.h" ||
             header == "strata/bootstrap.h" ||
             header == "strata/sql_parser.h" || header == "strata/repl.h" ||
             header == "strata/statement_buffer.h";
    case Layer::kUnknown:
      return false;
  }
  return false;
}

void CheckDirectory(const std::filesystem::path& root) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string extension = entry.path().extension().string();
    if (extension != ".h" && extension != ".cpp") {
      continue;
    }
    std::filesystem::path relative =
        std::filesystem::relative(entry.path(), root);
    Layer layer = LayerOf(relative);
    if (layer == Layer::kUnknown) {
      ::strata::testing::ReportFailure(
          __FILE__, __LINE__,
          "file has no layer assignment: " + relative.string());
      continue;
    }
    std::ifstream file(entry.path());
    if (!file.is_open()) {
      ::strata::testing::ReportFailure(__FILE__, __LINE__,
                                       "cannot read " + relative.string());
      continue;
    }
    std::string line;
    while (std::getline(file, line)) {
      std::string header = ProjectHeaderOf(line);
      if (header.empty() || IsAllowed(layer, header)) {
        continue;
      }
      ::strata::testing::ReportFailure(
          __FILE__, __LINE__, relative.string() + " includes " + header);
    }
  }
}

}  // namespace

TEST(test_layering_includes_follow_dependency_direction) {
  const std::filesystem::path source_dir = STRATA_SOURCE_DIR;
  CheckDirectory(source_dir / "include");
  CheckDirectory(source_dir / "src");
}

namespace {

// The only file outside paged/ that may include a paged/ header.
const char* const kPagedComposer = "bootstrap.cpp";

// The byte layer of the paged engine: pure functions on bytes. It may include
// the domain, the shared types and each other, but nothing that does I/O,
// locks or pins (docs/DesignDocument.md section 2.1).
bool IsPagedByteLayerFile(const std::string& path) {
  const char* const names[] = {"tuple_codec", "slotted_page", "crc32c",
                               "btree_node"};
  for (const char* name : names) {
    if (path == std::string("strata/paged/") + name + ".h" ||
        path == std::string("paged/") + name + ".cpp") {
      return true;
    }
  }
  return false;
}

bool IsPagedByteLayerHeader(const std::string& header) {
  const char* const allowed[] = {
      "strata/paged/paged_types.h", "strata/paged/byte_codec.h",
      "strata/paged/crc32c.h",      "strata/paged/slotted_page.h",
      "strata/paged/tuple_codec.h", "strata/paged/btree_node.h"};
  for (const char* candidate : allowed) {
    if (header == candidate) {
      return true;
    }
  }
  return false;
}

// Returns how many paged/ files were checked.
int CheckPagedRules(const std::filesystem::path& root) {
  int paged_files = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string extension = entry.path().extension().string();
    if (extension != ".h" && extension != ".cpp") {
      continue;
    }
    std::filesystem::path relative =
        std::filesystem::relative(entry.path(), root);
    const std::string path = relative.generic_string();
    const bool is_paged = LayerOf(relative) == Layer::kPaged;
    if (is_paged) {
      ++paged_files;
    }
    std::ifstream file(entry.path());
    std::string line;
    while (std::getline(file, line)) {
      std::string header = ProjectHeaderOf(line);
      if (header.empty()) {
        continue;
      }
      if (!is_paged) {
        // The composition root builds the paged engine; bootstrap.h only
        // forward-declares it (docs/DesignDocument.md section 2.1).
        if (StartsWith(header, "strata/paged/") && path != kPagedComposer) {
          ::strata::testing::ReportFailure(__FILE__, __LINE__,
                                           path + " includes " + header);
        }
        continue;
      }
      if (!StartsWith(header, "strata/domain/") &&
          !StartsWith(header, "strata/paged/")) {
        ::strata::testing::ReportFailure(__FILE__, __LINE__,
                                         path + " includes " + header);
      }
      if (IsPagedByteLayerFile(path) && StartsWith(header, "strata/paged/") &&
          !IsPagedByteLayerHeader(header)) {
        ::strata::testing::ReportFailure(
            __FILE__, __LINE__,
            path + " is in the byte layer but includes " + header);
      }
    }
  }
  return paged_files;
}

}  // namespace

TEST(test_layering_paged_depends_only_on_domain_and_itself) {
  const std::filesystem::path source_dir = STRATA_SOURCE_DIR;
  const int paged_files = CheckPagedRules(source_dir / "include") +
                          CheckPagedRules(source_dir / "src");
  // The scan must have found the layer; an empty scan would pass vacuously.
  CHECK(paged_files >= 20);
}

namespace {

// The row layer of the paged engine: the operators, the mutation planner and
// the keys they use, which read rows only through IRowStream (design document
// 2.1). It may include the domain, the shared types, the byte layer's tuple
// codec and its own files, and nothing that pins, locks or does I/O.
const char* const kRowLayerNames[] = {"row_stream",   "row_keys",
                                      "operators",    "mutation_planner",
                                      "index_reader", "key_range"};

bool IsRowLayerFile(const std::string& path) {
  for (const char* name : kRowLayerNames) {
    if (path == std::string("strata/paged/") + name + ".h" ||
        path == std::string("paged/") + name + ".cpp") {
      return true;
    }
  }
  return false;
}

bool IsRowLayerAllowedHeader(const std::string& header) {
  if (StartsWith(header, "strata/domain/")) {
    return true;
  }
  if (header == "strata/paged/paged_types.h" ||
      header == "strata/paged/tuple_codec.h") {
    return true;
  }
  for (const char* name : kRowLayerNames) {
    if (header == std::string("strata/paged/") + name + ".h") {
      return true;
    }
  }
  return false;
}

bool IsApplicationFile(const std::string& path) {
  return path == "strata/application.h" || path == "application.cpp";
}

bool IsBootstrapHeader(const std::string& path) {
  return path == "strata/bootstrap.h";
}

bool IsPagedFile(const std::string& path) {
  return StartsWith(path, "strata/paged/") || StartsWith(path, "paged/");
}

// What the scan of one root found.
struct RowLayerScan {
  int row_layer_files = 0;
  int application_files = 0;
  int bootstrap_files = 0;
};

void ReportViolation(const std::string& path, const std::string& header,
                     const std::string& rule) {
  ::strata::testing::ReportFailure(__FILE__, __LINE__,
                                   path + " includes " + header + ": " + rule);
}

// Checks one include line of one file against the rules that concern the row
// layer, Application and Bootstrap.
void CheckRowLayerRulesFor(const std::string& path, const std::string& header) {
  if (IsRowLayerFile(path) && !IsRowLayerAllowedHeader(header)) {
    ReportViolation(path, header, "the row layer must not include it");
  }
  if (IsApplicationFile(path) && !StartsWith(header, "strata/domain/") &&
      header != "strata/application.h") {
    ReportViolation(path, header,
                    "Application includes only the domain and itself");
  }
  if (IsBootstrapHeader(path) && StartsWith(header, "strata/paged/")) {
    ReportViolation(path, header, "bootstrap.h only forward-declares paged");
  }
  if (!IsPagedFile(path) && path != kPagedComposer &&
      StartsWith(header, "strata/paged/")) {
    ReportViolation(path, header, "only bootstrap.cpp includes paged/");
  }
}

RowLayerScan ScanRowLayerRules(const std::filesystem::path& root) {
  RowLayerScan scan;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string extension = entry.path().extension().string();
    if (extension != ".h" && extension != ".cpp") {
      continue;
    }
    const std::string path =
        std::filesystem::relative(entry.path(), root).generic_string();
    if (IsRowLayerFile(path)) {
      ++scan.row_layer_files;
    }
    if (IsApplicationFile(path)) {
      ++scan.application_files;
    }
    if (IsBootstrapHeader(path) || path == kPagedComposer) {
      ++scan.bootstrap_files;
    }
    std::ifstream file(entry.path());
    std::string line;
    while (std::getline(file, line)) {
      const std::string header = ProjectHeaderOf(line);
      if (header.empty()) {
        continue;
      }
      CheckRowLayerRulesFor(path, header);
    }
  }
  return scan;
}

}  // namespace

TEST(test_layering_row_layer_and_bootstrap_rules) {
  const std::filesystem::path source_dir = STRATA_SOURCE_DIR;
  const RowLayerScan include_scan = ScanRowLayerRules(source_dir / "include");
  const RowLayerScan source_scan = ScanRowLayerRules(source_dir / "src");

  // An empty scan would pass vacuously: the six row layer headers, the four
  // row layer sources (row_stream and index_reader have none), application.h
  // and .cpp, and bootstrap.h and .cpp must all have been found.
  CHECK_EQ(include_scan.row_layer_files, 6);
  CHECK_EQ(source_scan.row_layer_files, 4);
  CHECK_EQ(include_scan.application_files, 1);
  CHECK_EQ(source_scan.application_files, 1);
  CHECK_EQ(include_scan.bootstrap_files, 1);
  CHECK_EQ(source_scan.bootstrap_files, 1);
}
