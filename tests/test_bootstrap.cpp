// Integration tests of OpenFlatFileDatabase against real database directories
// (docs/TestDocument.md section 3.2), and of OpenPagedDatabase
// (docs/TestDocument.md).

#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/bootstrap.h"
#include "strata/directory_lock.h"
#include "strata/domain/value.h"
#include "strata/paged/database.h"
#include "strata/paged/paged_engine.h"
#include "strata/paged/paged_types.h"
#include "test_paged_support.h"
#include "test_support.h"

namespace {

namespace bootstrap = strata::bootstrap;
namespace domain = strata::domain;
namespace infrastructure = strata::infrastructure;
namespace testing = strata::testing;

using OpenOutcome = std::variant<bootstrap::OpenedDatabase, std::string>;

// The opened database, or null after reporting why the open failed.
std::unique_ptr<bootstrap::OpenedDatabase> MustOpen(
    const std::filesystem::path& directory, bool is_parallel,
    const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(directory.string(), is_parallel, current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    testing::ReportFailure(__FILE__, __LINE__, "open failed: " + *reason);
    return nullptr;
  }
  return std::make_unique<bootstrap::OpenedDatabase>(
      std::move(std::get<bootstrap::OpenedDatabase>(outcome)));
}

// The reason the open failed, or "<opened>" after reporting that it succeeded.
std::string OpenFailure(const std::string& directory,
                        const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(directory, false, current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    return *reason;
  }
  testing::ReportFailure(__FILE__, __LINE__, "the open succeeded");
  return "<opened>";
}

// Whether another Acquire of the directory is refused right now.
bool IsLockHeld(const std::filesystem::path& directory) {
  infrastructure::LockOutcome outcome =
      infrastructure::DirectoryLock::Acquire(directory.string());
  return outcome.error.has_value();
}

}  // namespace

TEST(test_bootstrap_opens_database_and_reports_counts) {
  // Arrange
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const bool load_modes[] = {false, true};

  for (bool is_parallel : load_modes) {
    // Act
    std::unique_ptr<bootstrap::OpenedDatabase> opened =
        MustOpen(dir.path(), is_parallel);

    // Assert
    CHECK(opened != nullptr);
    if (opened == nullptr) {
      continue;
    }
    CHECK_EQ(opened->tables.size(), std::size_t{3});
    if (opened->tables.size() == 3) {
      CHECK_EQ(opened->tables[0].name, std::string("visitors"));
      CHECK_EQ(opened->tables[0].row_count, std::size_t{4});
      CHECK_EQ(opened->tables[1].name, std::string("stories"));
      CHECK_EQ(opened->tables[1].row_count, std::size_t{3});
      CHECK_EQ(opened->tables[2].name, std::string("reactions"));
      CHECK_EQ(opened->tables[2].row_count, std::size_t{5});
    }
    CHECK_EQ(opened->directory,
             std::filesystem::canonical(dir.path()).string());
    CHECK(opened->service != nullptr);
    if (opened->service == nullptr) {
      continue;
    }
    domain::ExecutionResult result = testing::ExecuteSql(
        *opened->service, "SELECT username FROM visitors WHERE id = 1");
    const domain::ResultSet* result_set = testing::AsResultSet(result);
    CHECK(result_set != nullptr);
    if (result_set == nullptr) {
      continue;
    }
    CHECK_EQ(testing::RowsAsText(*result_set),
             std::vector<std::string>({"alice"}));
  }
}

TEST(test_bootstrap_missing_file_returns_description) {
  // Arrange
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::error_code error;
  std::filesystem::remove(dir.path() / "stories.csv", error);
  CHECK(!error);

  // Act
  const std::string reason = OpenFailure(dir.path().string());

  // Assert
  CHECK_EQ(reason, std::string("cannot read stories.csv"));
}

TEST(test_bootstrap_missing_directory_returns_description) {
  testing::TempDir dir;
  testing::WriteFile(dir.path() / "regular.txt", "x");

  const std::string missing_parent =
      OpenFailure((dir.path() / "nosuch" / "db").string());
  const std::string regular_file =
      OpenFailure((dir.path() / "regular.txt").string());

  CHECK_EQ(missing_parent, std::string("no such directory"));
  CHECK_EQ(regular_file, std::string("not a directory"));
  CHECK(!std::filesystem::exists(dir.path() / "nosuch"));
  CHECK_EQ(testing::ReadFile(dir.path() / "regular.txt"), std::string("x"));
}

TEST(test_bootstrap_open_creates_missing_directory) {
  testing::TempDir dir;

  OpenOutcome created = bootstrap::OpenFlatFileDatabase(
      (dir.path() / "fresh").string(), false, nullptr);
  OpenOutcome trailing = bootstrap::OpenFlatFileDatabase(
      (dir.path() / "fresh2").string() + "/", false, nullptr);
  OpenOutcome existing =
      bootstrap::OpenFlatFileDatabase(dir.path().string(), false, nullptr);

  auto* first = std::get_if<bootstrap::OpenedDatabase>(&created);
  auto* second = std::get_if<bootstrap::OpenedDatabase>(&trailing);
  auto* third = std::get_if<bootstrap::OpenedDatabase>(&existing);
  CHECK(first != nullptr && second != nullptr && third != nullptr);
  if (first == nullptr || second == nullptr || third == nullptr) {
    return;
  }
  CHECK(first->is_created);
  CHECK(second->is_created);
  CHECK(!third->is_created);
  CHECK(first->tables.empty());
  CHECK(std::filesystem::is_directory(dir.path() / "fresh"));
  CHECK(std::filesystem::is_directory(dir.path() / "fresh2"));
  CHECK(std::filesystem::exists(dir.path() / "fresh" / ".strata.lock"));
}

TEST(test_bootstrap_create_failure_returns_description) {
  if (::geteuid() == 0) {
    SKIP("running as root: directory permissions do not apply");
  }
  testing::TempDir dir;
  std::filesystem::permissions(
      dir.path(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);

  const std::string reason = OpenFailure((dir.path() / "fresh").string());

  CHECK_EQ(reason, std::string("cannot create directory"));
  CHECK(!std::filesystem::exists(dir.path() / "fresh"));
}

TEST(test_bootstrap_open_reports_rejected_lines) {
  testing::TempDir dir;
  testing::WriteFixtureB(dir.path());

  OpenOutcome outcome =
      bootstrap::OpenFlatFileDatabase(dir.path().string(), false, nullptr);

  auto* opened = std::get_if<bootstrap::OpenedDatabase>(&outcome);
  CHECK(opened != nullptr);
  if (opened == nullptr) {
    return;
  }
  CHECK_EQ(opened->tables.size(), std::size_t{3});
  if (opened->tables.size() != 3) {
    return;
  }
  CHECK_EQ(opened->tables[0].name, std::string("visitors"));
  CHECK_EQ(opened->tables[0].row_count, std::size_t{2});
  CHECK_EQ(opened->tables[0].rejected_lines, std::vector<std::size_t>({3, 4}));
  CHECK_EQ(opened->tables[1].rejected_lines, std::vector<std::size_t>({3}));
  CHECK(opened->tables[2].rejected_lines.empty());
}

TEST(test_bootstrap_locked_directory_returns_description) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  infrastructure::LockOutcome held =
      infrastructure::DirectoryLock::Acquire(dir.path().string());
  CHECK(held.lock != nullptr);

  const std::string reason = OpenFailure(dir.path().string());

  CHECK_EQ(reason, std::string("database is locked by another process"));
}

TEST(test_bootstrap_reopen_same_directory_reuses_lock) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::unique_ptr<bootstrap::OpenedDatabase> first =
      MustOpen(dir.path(), false);
  CHECK(first != nullptr);
  if (first == nullptr) {
    return;
  }

  std::unique_ptr<bootstrap::OpenedDatabase> second =
      MustOpen(dir.path(), false, first.get());

  CHECK(second != nullptr);
  if (second == nullptr) {
    return;
  }
  CHECK(first->lock != nullptr);
  CHECK(second->lock == first->lock);
  CHECK(IsLockHeld(dir.path()));
  first.reset();
  CHECK(IsLockHeld(dir.path()));
  second.reset();
  CHECK(!IsLockHeld(dir.path()));
}

TEST(test_bootstrap_failed_open_keeps_current_lock_and_releases_new_one) {
  testing::TempDir good_dir;
  testing::WriteFixtureA(good_dir.path());
  testing::TempDir broken_dir;
  testing::WriteFixtureA(broken_dir.path());
  std::error_code error;
  std::filesystem::remove(broken_dir.path() / "stories.csv", error);
  std::unique_ptr<bootstrap::OpenedDatabase> current =
      MustOpen(good_dir.path(), false);
  CHECK(current != nullptr);
  if (current == nullptr) {
    return;
  }

  const std::string reason =
      OpenFailure(broken_dir.path().string(), current.get());

  CHECK_EQ(reason, std::string("cannot read stories.csv"));
  CHECK(IsLockHeld(good_dir.path()));
  CHECK(!IsLockHeld(broken_dir.path()));
}

TEST(test_bootstrap_reopen_takes_the_lock_once_the_directory_is_writable) {
  if (::geteuid() == 0) {
    SKIP("running as root: directory permissions do not apply");
  }
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::filesystem::permissions(
      dir.path(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
  std::unique_ptr<bootstrap::OpenedDatabase> read_only =
      MustOpen(dir.path(), false);
  CHECK(read_only != nullptr);
  if (read_only == nullptr) {
    return;
  }
  std::filesystem::permissions(dir.path(), std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);

  std::unique_ptr<bootstrap::OpenedDatabase> writable =
      MustOpen(dir.path(), false, read_only.get());

  CHECK(read_only->lock == nullptr);
  CHECK(writable != nullptr);
  if (writable == nullptr) {
    return;
  }
  CHECK(writable->lock != nullptr);
  CHECK(IsLockHeld(dir.path()));
}

TEST(test_bootstrap_empty_directory_has_no_tables) {
  testing::TempDir dir;

  std::unique_ptr<bootstrap::OpenedDatabase> opened =
      MustOpen(dir.path(), false);

  CHECK(opened != nullptr);
  if (opened == nullptr) {
    return;
  }
  CHECK(opened->tables.empty());
  domain::ExecutionResult result = testing::ExecuteSql(
      *opened->service, "CREATE TABLE t (id INT PRIMARY KEY)");
  CHECK(testing::AsCommandComplete(result) != nullptr);
}

// ---- OpenPagedDatabase -----------------------------------------------------

namespace {

constexpr std::size_t kPageBytes = strata::paged::kPageSize;
const char* const kNotWritableReason = "directory is not writable";
const char* const kLockedReason = "database is locked by another process";

std::unique_ptr<bootstrap::OpenedDatabase> MustOpenPaged(
    const std::filesystem::path& directory,
    const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenPagedDatabase(directory.string(), current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    testing::ReportFailure(__FILE__, __LINE__, "paged open failed: " + *reason);
    return nullptr;
  }
  return std::make_unique<bootstrap::OpenedDatabase>(
      std::move(std::get<bootstrap::OpenedDatabase>(outcome)));
}

// The reason the paged open failed, or "<opened>" after reporting that it
// succeeded.
std::string PagedOpenFailure(
    const std::filesystem::path& directory,
    const bootstrap::OpenedDatabase* current = nullptr) {
  OpenOutcome outcome =
      bootstrap::OpenPagedDatabase(directory.string(), current);
  if (const auto* reason = std::get_if<std::string>(&outcome)) {
    return *reason;
  }
  testing::ReportFailure(__FILE__, __LINE__, "the paged open succeeded");
  return "<opened>";
}

// Executes sql and reports a failure when it is an error.
void MustExecute(strata::application::QueryService& service,
                 const std::string& sql) {
  domain::ExecutionResult result = testing::ExecuteSql(service, sql);
  if (const domain::ExecutionError* error = testing::AsError(result)) {
    testing::ReportFailure(__FILE__, __LINE__,
                           sql + " failed: " + error->message);
  }
}

// `visitors=2 stories=0`: the tables of an opened database as the CLI lists
// them.
std::string TablesText(const bootstrap::OpenedDatabase& opened) {
  std::string text;
  for (const bootstrap::TableCount& table : opened.tables) {
    if (!text.empty()) {
      text += ' ';
    }
    text += table.name + "=" + std::to_string(table.row_count);
  }
  return text;
}

// A paged database in `directory` with visitors (2 rows) and stories (no row),
// closed again. Returns the first page of visitors, or 0 after a failure.
strata::paged::PageId WritePagedFixture(
    const std::filesystem::path& directory) {
  std::unique_ptr<bootstrap::OpenedDatabase> opened = MustOpenPaged(directory);
  if (opened == nullptr) {
    return 0;
  }
  MustExecute(
      *opened->service,
      "CREATE TABLE visitors (id INT PRIMARY KEY, username TEXT UNIQUE)");
  MustExecute(*opened->service,
              "CREATE TABLE stories (id INT PRIMARY KEY, body TEXT)");
  MustExecute(*opened->service,
              "INSERT INTO visitors VALUES (1, 'ann'), (2, 'bob')");
  const std::optional<strata::paged::PageId> first_page =
      opened->paged_engine->database().catalog().FirstPage("visitors");
  CHECK(first_page.has_value());
  return first_page.value_or(0);
}

void FlipByte(const std::filesystem::path& file, std::size_t offset) {
  std::string bytes = testing::ReadFile(file);
  CHECK(offset < bytes.size());
  if (offset >= bytes.size()) {
    return;
  }
  bytes[offset] = static_cast<char>(bytes[offset] ^ 0x01);
  testing::WriteFile(file, bytes);
}

// A directory that does not exist (its parent does) and an existing empty
// one: both get a paged.db, a lock and no table.
void CheckOpenCreatesDirectoryAndFile() {
  testing::TempDir dir;
  const std::filesystem::path fresh = dir.path() / "fresh";
  {
    std::unique_ptr<bootstrap::OpenedDatabase> created = MustOpenPaged(fresh);
    CHECK(created != nullptr);
    if (created == nullptr) {
      return;
    }
    CHECK(created->is_created);
    CHECK(created->tables.empty());
    CHECK(created->engine == bootstrap::EngineKind::kPaged);
    CHECK(created->paged_engine != nullptr);
    CHECK(created->service != nullptr);
    CHECK(created->lock != nullptr);
    // One share in the OpenedDatabase and one in the PagedEngine, which is
    // declared before the Database so that the lock outlives its write-out.
    CHECK_EQ(created->lock.use_count(), 2);
    CHECK_EQ(created->directory, std::filesystem::canonical(fresh).string());
    CHECK(std::filesystem::is_regular_file(fresh / "paged.db"));
    CHECK(std::filesystem::exists(fresh / ".strata.lock"));
    CHECK(IsLockHeld(fresh));
  }
  CHECK(!IsLockHeld(fresh));

  testing::TempDir empty_dir;
  std::unique_ptr<bootstrap::OpenedDatabase> existing =
      MustOpenPaged(empty_dir.path());
  CHECK(existing != nullptr);
  if (existing == nullptr) {
    return;
  }
  CHECK(!existing->is_created);
  CHECK(existing->tables.empty());
  CHECK(existing->notes.empty());
  CHECK(std::filesystem::is_regular_file(empty_dir.path() / "paged.db"));

  OpenOutcome missing_parent = bootstrap::OpenPagedDatabase(
      (dir.path() / "nosuch" / "db").string(), nullptr);
  CHECK(std::holds_alternative<std::string>(missing_parent));
  if (const auto* reason = std::get_if<std::string>(&missing_parent)) {
    CHECK_EQ(*reason, std::string("no such directory"));
  }
}

// An existing paged.db: the row count of every table, in catalog order, comes
// from RowCounts, and the data is queryable.
void CheckOpenLoadsRowCounts() {
  testing::TempDir dir;
  WritePagedFixture(dir.path());

  std::unique_ptr<bootstrap::OpenedDatabase> opened = MustOpenPaged(dir.path());

  CHECK(opened != nullptr);
  if (opened == nullptr) {
    return;
  }
  CHECK(!opened->is_created);
  CHECK_EQ(TablesText(*opened), std::string("visitors=2 stories=0"));
  for (const bootstrap::TableCount& table : opened->tables) {
    CHECK(table.rejected_lines.empty());
  }
  domain::ExecutionResult result = testing::ExecuteSql(
      *opened->service, "SELECT id, username FROM visitors ORDER BY id");
  const domain::ResultSet* result_set = testing::AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(testing::RowsAsText(*result_set),
           std::vector<std::string>({"1\tann", "2\tbob"}));
}

// `current` on the same directory: the lock is the same object and survives
// the destruction of either owner, and the dirty pages of `current` are in the
// file before the new Database reads it.
void CheckReopenReusesLockAndFlushesCurrent() {
  testing::TempDir dir;
  WritePagedFixture(dir.path());
  std::unique_ptr<bootstrap::OpenedDatabase> first = MustOpenPaged(dir.path());
  CHECK(first != nullptr);
  if (first == nullptr) {
    return;
  }
  // CreateTable on the Database changes pages in the pool and writes none of
  // them, as a statement that failed after its undo would leave them.
  const domain::TableSchema schema = testing::MakeSchema(
      "extra", {testing::Column("id", domain::DataType::kInt)});
  CHECK(first->paged_engine->database().CreateTable(schema).ok());

  std::unique_ptr<bootstrap::OpenedDatabase> second =
      MustOpenPaged(dir.path(), first.get());

  CHECK(second != nullptr);
  if (second == nullptr) {
    return;
  }
  CHECK(first->lock != nullptr);
  CHECK(second->lock == first->lock);
  CHECK(second->paged_engine != first->paged_engine);
  CHECK_EQ(TablesText(*second), std::string("visitors=2 stories=0 extra=0"));
  first.reset();
  CHECK(IsLockHeld(dir.path()));
  second.reset();
  CHECK(!IsLockHeld(dir.path()));
}

// A flat-file database on the same directory hands over its lock too, and its
// result names the flat-file engine.
void CheckSwitchFromFlatFileReusesLock() {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  std::unique_ptr<bootstrap::OpenedDatabase> flat_file =
      MustOpen(dir.path(), false);
  CHECK(flat_file != nullptr);
  if (flat_file == nullptr) {
    return;
  }
  CHECK(flat_file->engine == bootstrap::EngineKind::kFlatFile);
  CHECK(flat_file->paged_engine == nullptr);

  std::unique_ptr<bootstrap::OpenedDatabase> paged_database =
      MustOpenPaged(dir.path(), flat_file.get());

  CHECK(paged_database != nullptr);
  if (paged_database == nullptr) {
    return;
  }
  CHECK(paged_database->lock == flat_file->lock);
  CHECK(paged_database->engine == bootstrap::EngineKind::kPaged);
  CHECK(paged_database->tables.empty());
  flat_file.reset();
  CHECK(IsLockHeld(dir.path()));
}

// A paged.db with a damaged header, and one with a damaged table page: the
// whole open fails with a description that says corrupt, and the database the
// caller already has is untouched.
void CheckCorruptFileFailsWholeOpen() {
  testing::TempDir good_dir;
  WritePagedFixture(good_dir.path());
  std::unique_ptr<bootstrap::OpenedDatabase> current =
      MustOpenPaged(good_dir.path());
  CHECK(current != nullptr);
  if (current == nullptr) {
    return;
  }

  testing::TempDir header_dir;
  WritePagedFixture(header_dir.path());
  FlipByte(header_dir.path() / "paged.db", strata::paged::layout::kMagic);
  testing::TempDir page_dir;
  const strata::paged::PageId table_page = WritePagedFixture(page_dir.path());
  CHECK(table_page != 0);
  FlipByte(page_dir.path() / "paged.db", table_page * kPageBytes + 200);

  const std::string header_reason =
      PagedOpenFailure(header_dir.path(), current.get());
  const std::string page_reason =
      PagedOpenFailure(page_dir.path(), current.get());

  CHECK(header_reason.starts_with("corrupt: "));
  CHECK(page_reason.starts_with("corrupt: "));
  CHECK(IsLockHeld(good_dir.path()));
  CHECK(!IsLockHeld(header_dir.path()));
  CHECK(!IsLockHeld(page_dir.path()));
  CHECK_EQ(TablesText(*current), std::string("visitors=2 stories=0"));
  domain::ExecutionResult result =
      testing::ExecuteSql(*current->service, "SELECT COUNT(*) FROM visitors");
  const domain::ResultSet* result_set = testing::AsResultSet(result);
  CHECK(result_set != nullptr);
  if (result_set == nullptr) {
    return;
  }
  CHECK_EQ(testing::RowsAsText(*result_set), std::vector<std::string>({"2"}));
}

// Another holder of the directory lock refuses the open; so does a directory in
// which no lock file can be created, where flat-file would read without one.
void CheckUnavailableDirectoryIsRefused() {
  testing::TempDir locked_dir;
  infrastructure::LockOutcome held =
      infrastructure::DirectoryLock::Acquire(locked_dir.path().string());
  CHECK(held.lock != nullptr);
  CHECK_EQ(PagedOpenFailure(locked_dir.path()), std::string(kLockedReason));
  CHECK(!std::filesystem::exists(locked_dir.path() / "paged.db"));

  // As root a 0555 directory stays writable, so this case does not apply.
  if (::geteuid() == 0) {
    return;
  }
  testing::TempDir read_only_dir;
  std::filesystem::permissions(
      read_only_dir.path(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
  CHECK_EQ(PagedOpenFailure(read_only_dir.path()),
           std::string(kNotWritableReason));
  CHECK(!std::filesystem::exists(read_only_dir.path() / "paged.db"));
  // Flat-file opens the same directory (no lock) and the lock is not held.
  std::unique_ptr<bootstrap::OpenedDatabase> flat_file =
      MustOpen(read_only_dir.path(), false);
  CHECK(flat_file != nullptr);
  if (flat_file == nullptr) {
    return;
  }
  CHECK(flat_file->lock == nullptr);
  CHECK_EQ(PagedOpenFailure(read_only_dir.path(), flat_file.get()),
           std::string(kNotWritableReason));
}

}  // namespace

TEST(test_bootstrap_paged_open_create_and_reuse_lock) {
  CheckOpenCreatesDirectoryAndFile();
  CheckOpenLoadsRowCounts();
  CheckReopenReusesLockAndFlushesCurrent();
  CheckSwitchFromFlatFileReusesLock();
  CheckCorruptFileFailsWholeOpen();
  CheckUnavailableDirectoryIsRefused();
}
