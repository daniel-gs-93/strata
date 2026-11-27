#include "strata/bootstrap.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/csv_file_source.h"
#include "strata/directory_lock.h"
#include "strata/flat_file_engine.h"
#include "strata/infrastructure.h"
#include "strata/paged/database.h"
#include "strata/paged/heap_access.h"
#include "strata/paged/paged_engine.h"

namespace strata::bootstrap {

namespace {

constexpr const char* kFlatFileCatalogName = "catalog.csv";
constexpr const char* kPagedFileName = "paged.db";
constexpr const char* kNotWritable = "directory is not writable";
constexpr const char* kNoteOfPagedFile =
    "note: this directory also has a paged database (paged.db); .engine paged "
    "opens it";
constexpr const char* kNoteOfFlatFileCatalog =
    "note: this directory also has a flatfile catalog (catalog.csv); .engine "
    "flatfile opens it";

// A database directory that exists after the checks of docs/DesignDocument.md
// section 3.1.
struct ResolvedDirectory {
  std::string path;  // normalized
  bool is_created;   // this call created it
};

// Creates the directory when it does not exist and its parent does (one
// level), and normalizes the path. The description of the first failure
// otherwise.
std::variant<ResolvedDirectory, std::string> ResolveDirectory(
    const std::string& directory) {
  std::error_code error;
  bool is_created = false;
  std::filesystem::path given(directory);
  if (!std::filesystem::exists(given, error)) {
    // A path that ends in a separator names the directory before it.
    if (!given.has_filename()) {
      given = given.parent_path();
    }
    std::filesystem::path parent = given.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    if (!std::filesystem::is_directory(parent, error)) {
      return std::string("no such directory");
    }
    std::filesystem::create_directory(given, error);
    if (error) {
      return std::string("cannot create directory");
    }
    is_created = true;
  } else if (!std::filesystem::is_directory(given, error)) {
    return std::string("not a directory");
  }
  std::filesystem::path normalized = std::filesystem::canonical(given, error);
  if (error) {
    return std::string("no such directory");
  }
  return ResolvedDirectory{normalized.string(), is_created};
}

// Only whether the file exists, not its content.
bool HasFile(const std::string& directory, const char* name) {
  std::error_code error;
  return std::filesystem::exists(std::filesystem::path(directory) / name,
                                 error);
}

// The description of a failure of the storage layer, as a statement reports
// it: "corrupt: ..." for a file that fails its checks (design document 2.13).
std::string DescribePageError(const paged::PageError& error) {
  return paged::ToExecutionError(
             error, paged::ErrorContext{paged::ErrorSite::kRowAccess, ""})
      .message;
}

}  // namespace

std::variant<OpenedDatabase, std::string> OpenFlatFileDatabase(
    const std::string& directory, bool is_parallel,
    const OpenedDatabase* current) {
  std::variant<ResolvedDirectory, std::string> resolved =
      ResolveDirectory(directory);
  if (const auto* reason = std::get_if<std::string>(&resolved)) {
    return *reason;
  }
  const auto& [path, is_created] = std::get<ResolvedDirectory>(resolved);

  // A held lock of `current` is reused: a second flock from this process would
  // be refused. An empty one (the directory was not writable at that open) is
  // not, so the lock is taken once the directory is writable.
  std::shared_ptr<void> lock;
  if (current != nullptr && current->directory == path &&
      current->lock != nullptr) {
    lock = current->lock;
  } else {
    infrastructure::LockOutcome outcome =
        infrastructure::DirectoryLock::Acquire(path);
    if (outcome.error) {
      return *outcome.error;
    }
    lock = outcome.lock;
  }

  auto repository = std::make_unique<infrastructure::CsvRepository>(path);
  auto engine =
      std::make_unique<infrastructure::FlatFileEngine>(std::move(repository));
  infrastructure::LoadResult loaded = engine->Load(is_parallel);
  if (!loaded.is_success) {
    return loaded.error;
  }

  std::vector<TableCount> tables;
  for (const infrastructure::TableCount& table : loaded.tables) {
    tables.push_back(
        TableCount{table.name, table.row_count, table.rejected_lines});
  }
  std::vector<std::string> notes;
  if (tables.empty() && HasFile(path, kPagedFileName)) {
    notes.push_back(kNoteOfPagedFile);
  }
  return OpenedDatabase{
      std::make_unique<application::QueryService>(
          std::move(engine), std::make_unique<infrastructure::FileCsvSource>()),
      path,
      std::move(lock),
      std::move(tables),
      is_created,
      EngineKind::kFlatFile,
      nullptr,
      std::move(notes)};
}

std::variant<OpenedDatabase, std::string> OpenPagedDatabase(
    const std::string& directory, const OpenedDatabase* current) {
  std::variant<ResolvedDirectory, std::string> resolved =
      ResolveDirectory(directory);
  if (const auto* reason = std::get_if<std::string>(&resolved)) {
    return *reason;
  }
  const auto& [path, is_created] = std::get<ResolvedDirectory>(resolved);

  // The engine writes paged.db, so the lock is always needed. A held lock of
  // `current` is reused: a second flock from this process would be refused.
  std::shared_ptr<void> lock;
  if (current != nullptr && current->directory == path &&
      current->lock != nullptr) {
    lock = current->lock;
  } else {
    infrastructure::LockOutcome outcome =
        infrastructure::DirectoryLock::Acquire(path);
    if (outcome.error) {
      return *outcome.error;
    }
    if (outcome.lock == nullptr) {
      return std::string(kNotWritable);
    }
    lock = outcome.lock;
  }

  // The new Database reads the file, so the pages that the current instance
  // has not written out (after a failed statement) must be in it first. This
  // Flush runs without the QueryService lock, so the caller must not run a
  // statement on `current` at the same time (true for the single-threaded CLI).
  if (current != nullptr && current->paged_engine != nullptr &&
      current->directory == path) {
    if (std::optional<domain::ExecutionError> error =
            current->paged_engine->Flush()) {
      return error->message;
    }
  }

  const paged::DatabaseOptions options{};
  paged::PageResult<paged::Database> opened =
      paged::Database::Open(path, options);
  if (!opened.ok()) {
    return DescribePageError(opened.error());
  }
  auto engine =
      std::make_unique<paged::PagedEngine>(std::move(opened).value(), lock);
  paged::RowCountResult counts = engine->RowCounts();
  if (const auto* error = std::get_if<domain::ExecutionError>(&counts)) {
    return error->message;
  }

  std::vector<TableCount> tables;
  for (const paged::RowCount& table :
       std::get<std::vector<paged::RowCount>>(counts)) {
    tables.push_back(TableCount{table.name, table.count});
  }
  std::vector<std::string> notes;
  if (tables.empty() && HasFile(path, kFlatFileCatalogName)) {
    notes.push_back(kNoteOfFlatFileCatalog);
  }
  paged::PagedEngine* const paged_engine = engine.get();
  return OpenedDatabase{
      std::make_unique<application::QueryService>(
          std::move(engine), std::make_unique<infrastructure::FileCsvSource>(),
          paged_engine),
      path,
      std::move(lock),
      std::move(tables),
      is_created,
      EngineKind::kPaged,
      paged_engine,
      std::move(notes)};
}

}  // namespace strata::bootstrap
