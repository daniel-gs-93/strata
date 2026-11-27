#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_support.h"
#include "test_support.h"

// Database over real files in temporary directories, and over a
// MemoryPageStore where a failure must be injected.

namespace {

using strata::domain::DataType;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::Database;
using strata::paged::DatabaseOptions;
using strata::paged::MemoryPageStore;
using strata::paged::PageId;
using strata::paged::ReadLe16;
using strata::paged::ReadLe32;
using strata::paged::ReadLe64;
using strata::paged::ReplacementPolicyKind;
using strata::paged::Rid;
using strata::paged::SlottedPage;
using strata::paged::TableHeap;
using strata::paged::WriteLe32;
using strata::testing::Column;
using strata::testing::MakeSchema;
using strata::testing::ReadFile;
using strata::testing::TempDir;
using strata::testing::WriteFile;
using Operation = strata::paged::MemoryPageStore::Operation;

constexpr std::size_t kPageBytes = 4096;

TableSchema PeopleSchema(const std::string& name) {
  return MakeSchema(
      name, {Column("id", DataType::kInt), Column("name", DataType::kText),
             Column("score", DataType::kDouble)});
}

Row PersonRow(std::int64_t id, std::size_t name_length = 5) {
  return Row{Value(id),
             Value(std::string(name_length, static_cast<char>('a' + id % 26))),
             Value(static_cast<double>(id) * 0.5)};
}

// 8 + (2 + 82) + 8 = 100 bytes.
Row HundredByteRow(std::int64_t id) { return PersonRow(id, 82); }

std::filesystem::path FilePathIn(const TempDir& directory) {
  return directory.path() / "paged.db";
}

std::unique_ptr<Database> OpenDatabase(
    const std::filesystem::path& directory, std::size_t frames = 256,
    ReplacementPolicyKind policy = ReplacementPolicyKind::kLru) {
  DatabaseOptions options;
  options.pool_frames = frames;
  options.policy = policy;
  auto opened = Database::Open(directory, options);
  CHECK_OK(opened);
  if (!opened.ok()) {
    return nullptr;
  }
  return std::make_unique<Database>(std::move(opened).value());
}

void CreateAndFill(Database& database, const std::string& table,
                   std::int64_t row_count, std::size_t name_length = 5) {
  CHECK_OK(database.CreateTable(PeopleSchema(table)));
  auto heap = database.Table(table);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  for (std::int64_t id = 0; id < row_count; ++id) {
    CHECK_OK(heap.value()->Insert(PersonRow(id, name_length)));
  }
}

bool RowIdLess(const Row& left, const Row& right) {
  return std::get<std::int64_t>(left[0]) < std::get<std::int64_t>(right[0]);
}

// Every row of a table, sorted by id.
std::vector<Row> RowsOf(Database& database, const std::string& table) {
  std::vector<Row> rows;
  auto heap = database.Table(table);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return rows;
  }
  auto scan = heap.value()->Scan();
  while (true) {
    auto next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    rows.push_back(std::move(next.value()->row));
  }
  std::ranges::sort(rows, RowIdLess);
  return rows;
}

// A hash that does not depend on the order in which rows are added up.
std::uint64_t HashRow(const Row& row) {
  const std::int64_t id = std::get<std::int64_t>(row[0]);
  return static_cast<std::uint64_t>(id) * 1000003ull +
         std::hash<std::string>()(std::get<std::string>(row[1]));
}

std::vector<std::uint8_t> BytesOfFile(const std::filesystem::path& path) {
  const std::string content = ReadFile(path);
  return std::vector<std::uint8_t>(content.begin(), content.end());
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
  WriteFile(path, std::string(bytes.begin(), bytes.end()));
}

// A closed database file with one table of a few rows.
std::vector<std::uint8_t> ValidFileBytes() {
  TempDir directory;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return {};
    }
    CreateAndFill(*database, "alpha", 10);
    CHECK_OK(database->Close());
  }
  return BytesOfFile(FilePathIn(directory));
}

}  // namespace

TEST(test_paged_database_creates_a_new_file) {
  TempDir directory;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CHECK(database->catalog().TableNames().empty());
    CHECK_OK(database->Close());
  }
  const std::vector<std::uint8_t> bytes = BytesOfFile(FilePathIn(directory));
  CHECK_EQ(bytes.size(), static_cast<std::size_t>(12288));
  if (bytes.size() != 12288) {
    return;
  }
  CHECK(std::memcmp(bytes.data() + 32, "PAGEDDB", 8) == 0);
  CHECK_EQ(ReadLe32(bytes.data() + 40), 2u);
  CHECK_EQ(ReadLe32(bytes.data() + 44), 4096u);
  CHECK_EQ(ReadLe32(bytes.data() + 48), 3u);
  CHECK_EQ(ReadLe32(bytes.data() + 52), 1u);
  CHECK_EQ(ReadLe32(bytes.data() + 56), 2u);
  CHECK_EQ(static_cast<int>(bytes[8]), 1);  // page_type: file header
  CHECK_EQ(static_cast<int>(bytes[9]), 0);  // reserved
  CHECK_EQ(ReadLe64(bytes.data() + 24), static_cast<std::uint64_t>(0));  // lsn
  for (PageId page = 0; page < 3; ++page) {
    CHECK_OK(SlottedPage::VerifyHeader(bytes.data() + page * kPageBytes, page));
    CHECK_EQ(ReadLe32(bytes.data() + page * kPageBytes + 4), page);
  }

  // An existing empty file counts as a new database.
  TempDir other;
  WriteFile(FilePathIn(other), "");
  {
    auto database = OpenDatabase(other.path());
    if (database == nullptr) {
      return;
    }
    CHECK(database->catalog().TableNames().empty());
    CHECK_OK(database->Close());
  }
  CHECK_EQ(BytesOfFile(FilePathIn(other)).size(),
           static_cast<std::size_t>(12288));
}

namespace {

enum class HeaderDamage {
  kAppendByte,
  kTruncateToTwoPages,
  kMagic,
  kFormatVersion,
  kPageSize,
  kChecksum,
  kHeaderPageId,
  kSysTablesFirstPage,
  kSysColumnsFirstPage,
  kPageType,
  kNextTableId
};

struct HeaderCase {
  HeaderDamage damage;
  const char* expected_word;
};

void ApplyHeaderDamage(std::vector<std::uint8_t>& bytes, HeaderDamage damage) {
  const std::size_t page_count = bytes.size() / kPageBytes;
  switch (damage) {
    case HeaderDamage::kAppendByte:
      bytes.push_back(1);  // a zero byte would be an interrupted extension
      return;
    case HeaderDamage::kTruncateToTwoPages:
      bytes.resize(2 * kPageBytes);
      return;
    case HeaderDamage::kMagic:
      bytes[33] ^= 0x01;
      break;
    case HeaderDamage::kFormatVersion:
      WriteLe32(bytes.data() + 40, 3);
      break;
    case HeaderDamage::kPageSize:
      WriteLe32(bytes.data() + 44, 8192);
      break;
    case HeaderDamage::kChecksum:
      bytes[100] ^= 0x01;
      return;  // not resealed: the checksum no longer matches
    case HeaderDamage::kHeaderPageId:
      WriteLe32(bytes.data() + 4, 1);
      break;
    case HeaderDamage::kSysTablesFirstPage:
      WriteLe32(bytes.data() + 52, static_cast<std::uint32_t>(page_count));
      break;
    case HeaderDamage::kSysColumnsFirstPage:
      WriteLe32(bytes.data() + 56, static_cast<std::uint32_t>(page_count));
      break;
    case HeaderDamage::kPageType:
      bytes[8] = 2;
      break;
    case HeaderDamage::kNextTableId:
      WriteLe32(bytes.data() + 48, 2);
      break;
  }
  SlottedPage::SealChecksum(bytes.data());
}

}  // namespace

TEST(test_paged_database_rejects_invalid_headers) {
  const std::vector<std::uint8_t> valid = ValidFileBytes();
  CHECK(valid.size() >= 3 * kPageBytes);
  if (valid.size() < 3 * kPageBytes) {
    return;
  }
  const HeaderCase cases[] = {
      {HeaderDamage::kAppendByte, "size"},
      {HeaderDamage::kTruncateToTwoPages, "size"},
      {HeaderDamage::kMagic, "magic"},
      {HeaderDamage::kFormatVersion, "format_version"},
      {HeaderDamage::kPageSize, "page_size"},
      {HeaderDamage::kChecksum, "checksum"},
      {HeaderDamage::kHeaderPageId, "page_id"},
      {HeaderDamage::kSysTablesFirstPage, "sys_tables_first_page"},
      {HeaderDamage::kSysColumnsFirstPage, "sys_columns_first_page"},
      {HeaderDamage::kPageType, "page_type"},
      {HeaderDamage::kNextTableId, "next_table_id"}};
  for (const HeaderCase& item : cases) {
    std::vector<std::uint8_t> bytes = valid;
    ApplyHeaderDamage(bytes, item.damage);
    TempDir directory;
    WriteBytes(FilePathIn(directory), bytes);
    DatabaseOptions options;
    auto opened = Database::Open(directory.path(), options);
    CHECK_ERROR(opened, kCorrupt);
    CHECK_MESSAGE_CONTAINS(opened, item.expected_word);
    CHECK(BytesOfFile(FilePathIn(directory)) == bytes);
  }
}

TEST(test_paged_database_refuses_format_version_1) {
  const std::vector<std::uint8_t> valid = ValidFileBytes();
  CHECK(valid.size() >= 3 * kPageBytes);
  if (valid.size() < 3 * kPageBytes) {
    return;
  }
  // A new file has format version 2, and the bytes of page 0 from offset 64
  // are reserved and zero.
  CHECK_EQ(ReadLe32(valid.data() + 40), 2u);
  bool is_reserved_zero = true;
  for (std::size_t offset = 64; offset < kPageBytes; ++offset) {
    is_reserved_zero = is_reserved_zero && valid[offset] == 0;
  }
  CHECK(is_reserved_zero);

  // Version 1 and version 3, each with a valid checksum, are refused.
  const std::uint32_t versions[] = {1, 3};
  for (std::uint32_t version : versions) {
    std::vector<std::uint8_t> bytes = valid;
    WriteLe32(bytes.data() + 40, version);
    SlottedPage::SealChecksum(bytes.data());
    TempDir directory;
    WriteBytes(FilePathIn(directory), bytes);
    DatabaseOptions options;
    auto opened = Database::Open(directory.path(), options);
    CHECK_ERROR(opened, kCorrupt);
    CHECK_MESSAGE_CONTAINS(opened, "format_version " + std::to_string(version) +
                                       " is not supported (expected 2)");
    CHECK(BytesOfFile(FilePathIn(directory)) == bytes);
  }
}

TEST(test_paged_database_reopen_preserves_catalog_and_rows) {
  TempDir directory;
  std::vector<std::vector<strata::paged::PageCapacity>> maps_before;
  const std::vector<std::string> names = {"alpha", "beta", "gamma"};
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    for (const std::string& name : names) {
      CreateAndFill(*database, name, 500);
    }
    for (const std::string& name : names) {
      auto heap = database->Table(name);
      CHECK_OK(heap);
      if (heap.ok()) {
        maps_before.push_back(heap.value()->FreeSpaceSnapshot());
      }
    }
    CHECK_OK(database->Close());
  }
  const std::size_t frames_for_pass[] = {256, 4};
  const ReplacementPolicyKind policy_for_pass[] = {
      ReplacementPolicyKind::kLru, ReplacementPolicyKind::kTwoQ};
  for (int pass = 0; pass < 2; ++pass) {
    auto database = OpenDatabase(directory.path(), frames_for_pass[pass],
                                 policy_for_pass[pass]);
    if (database == nullptr) {
      return;
    }
    CHECK(database->catalog().TableNames() == names);
    for (std::size_t index = 0; index < names.size(); ++index) {
      const std::vector<Row> rows = RowsOf(*database, names[index]);
      CHECK_EQ(rows.size(), static_cast<std::size_t>(500));
      for (std::int64_t id = 0;
           id < 500 && id < static_cast<std::int64_t>(rows.size()); ++id) {
        CHECK(rows[static_cast<std::size_t>(id)] == PersonRow(id));
      }
      auto heap = database->Table(names[index]);
      if (heap.ok() && index < maps_before.size()) {
        const auto after = heap.value()->FreeSpaceSnapshot();
        CHECK_EQ(after.size(), maps_before[index].size());
        if (after.size() == maps_before[index].size()) {
          for (std::size_t i = 0; i < after.size(); ++i) {
            CHECK_EQ(after[i].page_id, maps_before[index][i].page_id);
            CHECK_EQ(after[i].capacity, maps_before[index][i].capacity);
          }
        }
      }
    }
    CHECK_OK(database->Close());
  }
}

TEST(test_paged_database_isolates_corrupt_table_pages) {
  TempDir directory;
  PageId damaged_page = 0;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CreateAndFill(*database, "a_table", 500);
    CreateAndFill(*database, "b_table", 500);
    auto heap = database->Table("b_table");
    CHECK_OK(heap);
    if (!heap.ok()) {
      return;
    }
    const auto snapshot = heap.value()->FreeSpaceSnapshot();
    CHECK(snapshot.size() >= 2);
    if (snapshot.size() < 2) {
      return;
    }
    damaged_page = snapshot[1].page_id;
    CHECK_OK(database->Close());
  }
  std::vector<std::uint8_t> bytes = BytesOfFile(FilePathIn(directory));
  bytes[damaged_page * kPageBytes + 200] ^= 0x01;
  WriteBytes(FilePathIn(directory), bytes);
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CHECK_EQ(RowsOf(*database, "a_table").size(),
             static_cast<std::size_t>(500));
    auto damaged = database->Table("b_table");
    CHECK_ERROR(damaged, kCorrupt);
    CHECK_MESSAGE_CONTAINS(damaged, "page " + std::to_string(damaged_page));
  }
  // A damaged page of the system tables keeps the database from opening.
  std::vector<std::uint8_t> system_damage = BytesOfFile(FilePathIn(directory));
  bytes = system_damage;
  bytes[damaged_page * kPageBytes + 200] ^= 0x01;  // restore the data page
  bytes[1 * kPageBytes + 200] ^= 0x01;
  WriteBytes(FilePathIn(directory), bytes);
  DatabaseOptions options;
  CHECK_ERROR(Database::Open(directory.path(), options), kCorrupt);
}

namespace {

// The next_page_id field of page page_id in the file bytes.
PageId NextPageOf(const std::vector<std::uint8_t>& bytes, PageId page_id) {
  return ReadLe32(bytes.data() + page_id * kPageBytes +
                  strata::paged::layout::kNextPageId);
}

}  // namespace

TEST(test_paged_database_reports_a_cut_page_chain_as_corrupt) {
  TempDir directory;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    // The first table of a new file starts at page 3; 100 rows of 218 bytes
    // need several pages.
    CreateAndFill(*database, "a_table", 100, 200);
    CHECK_OK(database->Close());
  }
  const PageId second_page = NextPageOf(BytesOfFile(FilePathIn(directory)), 3);
  CHECK(second_page != strata::paged::kInvalidPageId);
  // The first page still names the second one, which the file no longer has.
  std::error_code error;
  std::filesystem::resize_file(FilePathIn(directory), second_page * kPageBytes,
                               error);
  CHECK(!error);
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    auto cut = database->Table("a_table");
    CHECK_ERROR(cut, kCorrupt);
    CHECK_MESSAGE_CONTAINS(cut, "page " + std::to_string(second_page));
  }

  // The same cut in the chain of sys_columns keeps the database from opening.
  TempDir system_directory;
  {
    auto database = OpenDatabase(system_directory.path());
    if (database == nullptr) {
      return;
    }
    for (int index = 0; index < 40; ++index) {
      CHECK_OK(
          database->CreateTable(PeopleSchema("t" + std::to_string(index))));
    }
    CHECK_OK(database->Close());
  }
  const PageId next_of_columns =
      NextPageOf(BytesOfFile(FilePathIn(system_directory)), 2);
  CHECK(next_of_columns != strata::paged::kInvalidPageId);
  std::filesystem::resize_file(FilePathIn(system_directory),
                               next_of_columns * kPageBytes, error);
  CHECK(!error);
  DatabaseOptions options;
  auto reopened = Database::Open(system_directory.path(), options);
  CHECK_ERROR(reopened, kCorrupt);
  CHECK_MESSAGE_CONTAINS(reopened, "sys_columns");
}

TEST(test_paged_database_accepts_trailing_unformatted_page) {
  TempDir directory;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CHECK_OK(database->Close());
  }
  std::vector<std::uint8_t> bytes = BytesOfFile(FilePathIn(directory));
  bytes.resize(bytes.size() + kPageBytes, 0);
  WriteBytes(FilePathIn(directory), bytes);
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CreateAndFill(*database, "alpha", 100);
    CHECK_OK(database->Close());
  }
  auto database = OpenDatabase(directory.path());
  if (database == nullptr) {
    return;
  }
  CHECK_EQ(RowsOf(*database, "alpha").size(), static_cast<std::size_t>(100));
}

TEST(test_paged_database_ignores_a_partial_zero_page_at_the_end) {
  // What an extension cut off by SIGXFSZ leaves: part of a page of zeros.
  TempDir directory;
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CHECK_OK(database->Close());
  }
  std::vector<std::uint8_t> bytes = BytesOfFile(FilePathIn(directory));
  bytes.resize(bytes.size() + 1024, 0);
  WriteBytes(FilePathIn(directory), bytes);
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CreateAndFill(*database, "alpha", 100);
    CHECK_OK(database->Close());
  }
  CHECK_EQ(std::filesystem::file_size(FilePathIn(directory)) % kPageBytes,
           std::uintmax_t{0});
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    CHECK_EQ(RowsOf(*database, "alpha").size(), static_cast<std::size_t>(100));
  }

  // A file that holds nothing but such zeros opens as a new database.
  TempDir fresh;
  WriteBytes(FilePathIn(fresh), std::vector<std::uint8_t>(100, 0));
  {
    auto database = OpenDatabase(fresh.path());
    if (database == nullptr) {
      return;
    }
    CHECK_OK(database->Close());
  }
  CHECK_EQ(std::filesystem::file_size(FilePathIn(fresh)),
           std::uintmax_t{3 * kPageBytes});
}

TEST(test_paged_database_reports_directory_problems) {
  TempDir parent;
  DatabaseOptions options;
  auto missing = Database::Open(parent.path() / "no_such_directory", options);
  CHECK_ERROR(missing, kIo);
  CHECK(!strata::testing::MessageOf(missing).empty());
  WriteFile(parent.path() / "a_file", "x");
  CHECK_ERROR(Database::Open(parent.path() / "a_file", options), kIo);
  if (::geteuid() != 0) {
    const std::filesystem::path locked = parent.path() / "locked";
    std::filesystem::create_directories(locked);
    ::chmod(locked.c_str(), 0555);
    CHECK_ERROR(Database::Open(locked, options), kIo);
    CHECK(!std::filesystem::exists(locked / "paged.db"));
    ::chmod(locked.c_str(), 0755);
  }
}

namespace {

struct ScanResult {
  std::uint64_t rows = 0;
  std::uint64_t hash = 0;
  bool is_ok = true;
};

// Scans every table `repeats` times. Runs on its own thread, so it reports
// through result instead of CHECK.
void ScanWorker(const std::vector<TableHeap*>* tables, int repeats,
                ScanResult* result) {
  for (int repeat = 0; repeat < repeats; ++repeat) {
    for (TableHeap* heap : *tables) {
      auto scan = heap->Scan();
      while (true) {
        auto next = scan.Next();
        if (!next.ok()) {
          result->is_ok = false;
          return;
        }
        if (!next.value().has_value()) {
          break;
        }
        ++result->rows;
        result->hash += HashRow(next.value()->row);
      }
    }
  }
}

ScanResult ScanOnce(TableHeap* heap) {
  ScanResult result;
  auto scan = heap->Scan();
  while (true) {
    auto next = scan.Next();
    if (!next.ok()) {
      result.is_ok = false;
      return result;
    }
    if (!next.value().has_value()) {
      break;
    }
    ++result.rows;
    result.hash += HashRow(next.value()->row);
  }
  return result;
}

struct SharedRids {
  std::mutex mutex;
  std::vector<Rid> rids;
};

bool RowMatchesId(const Row& row) {
  const std::int64_t id = std::get<std::int64_t>(row[0]);
  return row == PersonRow(id, static_cast<std::size_t>(id % 50));
}

void WriterWorker(TableHeap* heap, int count, SharedRids* shared,
                  std::atomic<bool>* is_done, std::atomic<int>* failures) {
  for (int id = 0; id < count; ++id) {
    auto inserted =
        heap->Insert(PersonRow(id, static_cast<std::size_t>(id % 50)));
    if (!inserted.ok()) {
      failures->fetch_add(1);
      break;
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->rids.push_back(inserted.value());
  }
  is_done->store(true);
}

void ReaderWorker(TableHeap* heap, SharedRids* shared,
                  const std::atomic<bool>* is_done,
                  std::atomic<int>* failures) {
  while (true) {
    const bool was_done = is_done->load();
    std::vector<Rid> completed;
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      completed = shared->rids;
    }
    std::set<Rid> seen;
    auto scan = heap->Scan();
    while (true) {
      auto next = scan.Next();
      if (!next.ok()) {
        failures->fetch_add(1);
        return;
      }
      if (!next.value().has_value()) {
        break;
      }
      if (!RowMatchesId(next.value()->row)) {
        failures->fetch_add(1);
      }
      if (!seen.insert(next.value()->rid).second) {
        failures->fetch_add(1);
      }
    }
    if (seen.size() < completed.size()) {
      failures->fetch_add(1);
    }
    const std::size_t stride = std::max<std::size_t>(1, completed.size() / 20);
    for (std::size_t i = 0; i < completed.size(); i += stride) {
      auto got = heap->Get(completed[i]);
      if (!got.ok() || !RowMatchesId(got.value())) {
        failures->fetch_add(1);
      }
    }
    if (was_done) {
      return;
    }
  }
}

}  // namespace

TEST(test_paged_database_concurrent_scans_with_small_pool) {
  const ReplacementPolicyKind policies[] = {ReplacementPolicyKind::kLru,
                                            ReplacementPolicyKind::kTwoQ};
  for (ReplacementPolicyKind policy : policies) {
    TempDir directory;
    const std::vector<std::string> names = {"alpha", "beta", "gamma"};
    {
      auto database = OpenDatabase(directory.path());
      if (database == nullptr) {
        return;
      }
      for (const std::string& name : names) {
        CreateAndFill(*database, name, 3000, 40);
      }
      CHECK_OK(database->Close());
    }
    auto database = OpenDatabase(directory.path(), 16, policy);
    if (database == nullptr) {
      return;
    }
    std::vector<TableHeap*> tables;
    std::size_t total_pages = 0;
    for (const std::string& name : names) {
      auto heap = database->Table(name);
      CHECK_OK(heap);
      if (!heap.ok()) {
        return;
      }
      tables.push_back(heap.value());
      total_pages += heap.value()->FreeSpaceSnapshot().size();
    }
    ScanResult expected;
    for (TableHeap* heap : tables) {
      const ScanResult one = ScanOnce(heap);
      expected.rows += one.rows;
      expected.hash += one.hash;
    }
    CHECK_EQ(expected.rows, static_cast<std::uint64_t>(9000));
    database->buffer().ResetStats();
    constexpr int kThreads = 8;
    constexpr int kRepeats = 5;
    std::vector<ScanResult> results(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back(ScanWorker, &tables, kRepeats, &results[i]);
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    for (const ScanResult& result : results) {
      CHECK(result.is_ok);
      CHECK_EQ(result.rows, expected.rows * kRepeats);
      CHECK_EQ(result.hash, expected.hash * kRepeats);
    }
    const auto stats = database->buffer().Stats();
    CHECK_EQ(stats.hits + stats.misses,
             static_cast<std::uint64_t>(kThreads * kRepeats * total_pages));
    CHECK_EQ(database->buffer().PinnedFrameCount(),
             static_cast<std::size_t>(0));
  }
}

TEST(test_paged_database_one_writer_with_concurrent_readers) {
  TempDir directory;
  auto database = OpenDatabase(directory.path(), 16);
  if (database == nullptr) {
    return;
  }
  CHECK_OK(database->CreateTable(PeopleSchema("people")));
  auto heap = database->Table("people");
  CHECK_OK(heap);
  if (!heap.ok()) {
    return;
  }
  SharedRids shared;
  std::atomic<bool> is_done{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.emplace_back(WriterWorker, heap.value(), 2000, &shared, &is_done,
                       &failures);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(ReaderWorker, heap.value(), &shared, &is_done,
                         &failures);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CHECK_EQ(failures.load(), 0);
  const std::vector<Row> rows = RowsOf(*database, "people");
  CHECK_EQ(rows.size(), static_cast<std::size_t>(2000));
  for (const Row& row : rows) {
    CHECK(RowMatchesId(row));
  }
  CHECK_EQ(database->buffer().PinnedFrameCount(), static_cast<std::size_t>(0));
}

TEST(test_paged_database_policy_switch_changes_misses_not_results) {
  TempDir directory;
  const std::vector<std::pair<std::string, std::int64_t>> tables = {
      {"h_table", 156}, {"f_table", 468}, {"g_table", 156}, {"b_table", 2340}};
  {
    auto database = OpenDatabase(directory.path());
    if (database == nullptr) {
      return;
    }
    for (const auto& [name, rows] : tables) {
      CHECK_OK(database->CreateTable(PeopleSchema(name)));
      auto heap = database->Table(name);
      CHECK_OK(heap);
      if (!heap.ok()) {
        return;
      }
      for (std::int64_t id = 0; id < rows; ++id) {
        CHECK_OK(heap.value()->Insert(HundredByteRow(id)));
      }
    }
    CHECK_OK(database->Close());
  }
  const ReplacementPolicyKind policies[] = {ReplacementPolicyKind::kLru,
                                            ReplacementPolicyKind::kTwoQ};
  std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> outcomes;
  std::vector<std::uint64_t> last_misses;
  for (ReplacementPolicyKind policy : policies) {
    auto database = OpenDatabase(directory.path(), 16, policy);
    if (database == nullptr) {
      return;
    }
    std::vector<TableHeap*> heaps;
    for (const auto& entry : tables) {
      auto heap = database->Table(entry.first);
      CHECK_OK(heap);
      if (!heap.ok()) {
        return;
      }
      heaps.push_back(heap.value());
    }
    // H, F, G, H, B, H
    const std::size_t order[] = {0, 1, 2, 0, 3, 0};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> scans;
    for (std::size_t step = 0; step < 6; ++step) {
      if (step == 5) {
        database->buffer().ResetStats();
      }
      const ScanResult result = ScanOnce(heaps[order[step]]);
      CHECK(result.is_ok);
      scans.emplace_back(result.rows, result.hash);
    }
    last_misses.push_back(database->buffer().Stats().misses);
    outcomes.push_back(scans);
  }
  CHECK(outcomes[0] == outcomes[1]);
  CHECK_EQ(last_misses[0], static_cast<std::uint64_t>(4));
  CHECK_EQ(last_misses[1], static_cast<std::uint64_t>(0));
}

namespace {

// Redirects std::cerr into a string for the lifetime of the object.
class StderrCapture {
 public:
  StderrCapture() : previous_(std::cerr.rdbuf(stream_.rdbuf())) {}
  ~StderrCapture() { std::cerr.rdbuf(previous_); }
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;
  std::string text() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
  std::streambuf* previous_;
};

}  // namespace

TEST(test_paged_database_close_and_destructor_flush) {
  // (a) Close, then reopen.
  {
    TempDir directory;
    {
      auto database = OpenDatabase(directory.path());
      if (database == nullptr) {
        return;
      }
      CreateAndFill(*database, "alpha", 50);
      CHECK_OK(database->Close());
      // (c) A second Close succeeds.
      CHECK_OK(database->Close());
      // (d) Nothing else works after Close.
      CHECK_ERROR(database->Table("alpha"), kClosed);
      CHECK_ERROR(database->CreateTable(PeopleSchema("beta")), kClosed);
    }
    auto reopened = OpenDatabase(directory.path());
    if (reopened == nullptr) {
      return;
    }
    CHECK_EQ(RowsOf(*reopened, "alpha").size(), static_cast<std::size_t>(50));
  }
  // (b) No Close: the destructor writes the pages and prints nothing.
  {
    TempDir directory;
    StderrCapture capture;
    {
      auto database = OpenDatabase(directory.path());
      if (database == nullptr) {
        return;
      }
      CreateAndFill(*database, "alpha", 50);
    }
    CHECK_EQ(capture.text(), std::string());
    auto reopened = OpenDatabase(directory.path());
    if (reopened == nullptr) {
      return;
    }
    CHECK_EQ(RowsOf(*reopened, "alpha").size(), static_cast<std::size_t>(50));
  }
}

TEST(test_paged_database_close_reports_sync_failure) {
  auto store = std::make_unique<MemoryPageStore>();
  MemoryPageStore* const observer = store.get();
  DatabaseOptions options;
  auto opened = Database::OpenWithStore(std::move(store), options);
  CHECK_OK(opened);
  if (!opened.ok()) {
    return;
  }
  Database database = std::move(opened).value();
  CHECK_OK(database.CreateTable(PeopleSchema("alpha")));
  auto heap = database.Table("alpha");
  CHECK_OK(heap);
  if (heap.ok()) {
    for (std::int64_t id = 0; id < 20; ++id) {
      CHECK_OK(heap.value()->Insert(PersonRow(id)));
    }
  }
  observer->InjectFailure(Operation::kSync, 1);
  CHECK_ERROR(database.Close(), kIo);
  CHECK_OK(database.Close());  // the injected failure is used up

  TempDir directory;
  auto file = strata::paged::FilePageStore::Open(
      directory.path() / "sync_probe.db",
      strata::paged::OpenMode::kOpenOrCreate);
  CHECK_OK(file);
  if (file.ok()) {
    CHECK_OK(file.value().Sync());
  }
}
