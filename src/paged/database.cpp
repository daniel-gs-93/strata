#include "strata/paged/database.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "strata/paged/byte_codec.h"
#include "strata/paged/slotted_page.h"
#include "strata/paged/tuple_codec.h"

namespace strata::paged {

namespace {

constexpr char kMagic[layout::kMagicSize] = {'P', 'A', 'G', 'E',
                                             'D', 'D', 'B', '\0'};
// Version 2 holds the index root in sys_tables (ADR-0048).
constexpr std::uint32_t kFormatVersion = 2;
// A file holds at least the file header and the two system table pages.
constexpr PageId kMinimumPages = kFirstUserPage;
constexpr const char* kFileName = "paged.db";

struct FileHeader {
  PageId sys_tables_first_page;
  PageId sys_columns_first_page;
};

PageError Corrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, kFileName + (": " + detail));
}

// Writes the first three pages of a new database through the buffer pool and
// flushes them: the file header, and the first pages of sys_tables and
// sys_columns.
Status InitializeNewDatabase(BufferManager& buffer) {
  {
    PageResult<PageGuard> created = buffer.NewPage();
    if (!created.ok()) {
      return created.error();
    }
    PageGuard guard = std::move(created).value();
    std::uint8_t* page = guard.mutable_data();
    page[layout::kPageTypeField] =
        static_cast<std::uint8_t>(PageType::kFileHeader);
    std::memcpy(page + layout::kMagic, kMagic, layout::kMagicSize);
    WriteLe32(page + layout::kFormatVersion, kFormatVersion);
    WriteLe32(page + layout::kFilePageSize,
              static_cast<std::uint32_t>(kPageSize));
    WriteLe32(page + layout::kNextTableId, kFirstUserTableId);
    WriteLe32(page + layout::kSysTablesFirstPage, kSysTablesPage);
    WriteLe32(page + layout::kSysColumnsFirstPage, kSysColumnsPage);
    guard.MarkDirty();
  }
  const std::uint32_t system_table_ids[] = {kSysTablesId, kSysColumnsId};
  for (std::uint32_t table_id : system_table_ids) {
    PageResult<PageGuard> created = buffer.NewPage();
    if (!created.ok()) {
      return created.error();
    }
    PageGuard guard = std::move(created).value();
    SlottedPage::Format(guard.mutable_data(), guard.page_id(), table_id);
    guard.MarkDirty();
  }
  return buffer.FlushAll();
}

// Checks page 0 of an existing file in the order of design document 2.15 and
// returns the first pages of the system tables.
PageResult<FileHeader> ValidateExistingFile(IPageStore& store) {
  const PageId page_count = store.PageCount();
  if (page_count < kMinimumPages) {
    return Corrupt("size of " + std::to_string(page_count) +
                   " pages is below the minimum of " +
                   std::to_string(kMinimumPages));
  }
  std::array<std::uint8_t, kPageSize> header{};
  if (Status status = store.ReadPage(0, header.data())) {
    return *status;
  }
  if (std::memcmp(header.data() + layout::kMagic, kMagic, layout::kMagicSize) !=
      0) {
    return Corrupt("magic is not PAGEDDB");
  }
  const std::uint32_t version =
      ReadLe32(header.data() + layout::kFormatVersion);
  if (version != kFormatVersion) {
    return Corrupt("format_version " + std::to_string(version) +
                   " is not supported (expected " +
                   std::to_string(kFormatVersion) + ")");
  }
  const std::uint32_t page_size =
      ReadLe32(header.data() + layout::kFilePageSize);
  if (page_size != kPageSize) {
    return Corrupt("page_size " + std::to_string(page_size) + " differs from " +
                   std::to_string(kPageSize));
  }
  // page_type is checked before VerifyHeader: a page 0 that claims another
  // type would otherwise be judged by the rules of that type.
  if (header[layout::kPageTypeField] !=
      static_cast<std::uint8_t>(PageType::kFileHeader)) {
    return Corrupt("page 0 has page_type " +
                   std::to_string(header[layout::kPageTypeField]) +
                   " instead of 1");
  }
  if (Status status = SlottedPage::VerifyHeader(header.data(), 0)) {
    return *status;
  }
  FileHeader result;
  result.sys_tables_first_page =
      ReadLe32(header.data() + layout::kSysTablesFirstPage);
  result.sys_columns_first_page =
      ReadLe32(header.data() + layout::kSysColumnsFirstPage);
  if (result.sys_tables_first_page >= page_count) {
    return Corrupt("sys_tables_first_page " +
                   std::to_string(result.sys_tables_first_page) +
                   " is outside the file (" + std::to_string(page_count) +
                   " pages)");
  }
  if (result.sys_columns_first_page >= page_count) {
    return Corrupt("sys_columns_first_page " +
                   std::to_string(result.sys_columns_first_page) +
                   " is outside the file (" + std::to_string(page_count) +
                   " pages)");
  }
  const std::uint32_t next_table_id =
      ReadLe32(header.data() + layout::kNextTableId);
  if (next_table_id < kFirstUserTableId) {
    return Corrupt("next_table_id " + std::to_string(next_table_id) +
                   " is below " + std::to_string(kFirstUserTableId));
  }
  return result;
}

// True when every row of heap still encodes as one tuple of `extended` with
// the default appended. Writes no page.
Status CheckRowsFit(const TableHeap& heap, const domain::TableSchema& extended,
                    const domain::Value& default_value) {
  HeapScan scan = heap.Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      return std::nullopt;
    }
    domain::Row row = std::move(next.value()->row);
    row.push_back(default_value);
    PageResult<std::vector<std::uint8_t>> encoded =
        TupleCodec::Encode(extended, row);
    if (!encoded.ok()) {
      return encoded.error();
    }
  }
}

// The key of a row of a table whose primary key is an INT.
std::int64_t KeyOf(const domain::Row& row, std::size_t primary_key_index) {
  return std::get<std::int64_t>(row[primary_key_index]);
}

// A disagreement between an index and its heap, which names the table.
PageError IndexCorrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, detail);
}

// A corrupt tree reported for a table: the message names the table. Other
// errors (an I/O failure) pass unchanged.
PageError DamagedIndex(const std::string& table, PageError error) {
  if (error.code == PageErrorCode::kCorrupt) {
    error.message = "index of " + table + ": " + error.message;
  }
  return error;
}

// Inserts every row of source, with the default appended, into target, and the
// key of each row with its new Rid into tree when there is one.
Status CopyRowsWithDefault(const TableHeap& source, TableHeap* target,
                           const domain::Value& default_value, BTree* tree,
                           std::size_t primary_key_index) {
  HeapScan scan = source.Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      return std::nullopt;
    }
    domain::Row row = std::move(next.value()->row);
    row.push_back(default_value);
    PageResult<Rid> inserted = target->Insert(row);
    if (!inserted.ok()) {
      return inserted.error();
    }
    if (tree != nullptr) {
      if (Status status =
              tree->Insert(KeyOf(row, primary_key_index), inserted.value())) {
        return status;
      }
    }
  }
}

// The error of a step that failed before the catalog was switched: the new
// chain and the pages of the new tree go back to the free list, and a failure
// of that is added to the message.
PageError DiscardNewPages(PageAllocator& allocator, PageId first_page,
                          const BTree* new_tree, PageError error) {
  if (Status released = allocator.Release(first_page)) {
    error.message +=
        "; releasing the new pages also failed: " + released->message;
  }
  if (new_tree != nullptr) {
    PageResult<std::vector<PageId>> pages = new_tree->Pages();
    if (!pages.ok()) {
      error.message +=
          "; listing the new index pages also failed: " + pages.error().message;
    } else if (Status released = allocator.ReleasePages(pages.value())) {
      error.message +=
          "; releasing the new index pages also failed: " + released->message;
    }
  }
  return error;
}

// The error of a failed release after the statement was applied.
PageError NotReclaimed(const PageError& error) {
  return PageError{PageErrorCode::kIo,
                   error.message +
                       "; statement was applied but its pages were not "
                       "reclaimed"};
}

}  // namespace

Database::Database(std::unique_ptr<IPageStore> store,
                   std::unique_ptr<BufferManager> buffer,
                   std::unique_ptr<PageAllocator> allocator,
                   std::unique_ptr<Catalog> catalog)
    : store_(std::move(store)),
      buffer_(std::move(buffer)),
      allocator_(std::move(allocator)),
      catalog_(std::move(catalog)) {}

Database::~Database() {
  if (buffer_ == nullptr || is_closed_) {
    return;
  }
  if (Status status = buffer_->FlushAll()) {
    std::cerr << "paged: failed to write the database on destruction: "
              << status->message << "\n";
  }
}

PageResult<Database> Database::Open(const std::filesystem::path& directory,
                                    const DatabaseOptions& options) {
  std::error_code directory_error;
  if (!std::filesystem::is_directory(directory, directory_error)) {
    return MakeError(PageErrorCode::kIo,
                     directory.string() + " is not an existing directory");
  }
  PageResult<FilePageStore> opened =
      FilePageStore::Open(directory / kFileName, OpenMode::kOpenOrCreate);
  if (!opened.ok()) {
    return opened.error();
  }
  return OpenWithStore(
      std::make_unique<FilePageStore>(std::move(opened).value()), options);
}

PageResult<Database> Database::OpenWithStore(std::unique_ptr<IPageStore> store,
                                             const DatabaseOptions& options) {
  PageResult<std::unique_ptr<BufferManager>> created =
      BufferManager::Create(*store, options.pool_frames, options.policy);
  if (!created.ok()) {
    return created.error();
  }
  std::unique_ptr<BufferManager> buffer = std::move(created).value();

  FileHeader header{kSysTablesPage, kSysColumnsPage};
  if (store->PageCount() == 0) {
    if (Status status = InitializeNewDatabase(*buffer)) {
      return *status;
    }
  } else {
    PageResult<FileHeader> validated = ValidateExistingFile(*store);
    if (!validated.ok()) {
      return validated.error();
    }
    header = validated.value();
  }
  auto allocator = std::make_unique<PageAllocator>(*buffer);
  auto catalog = std::make_unique<Catalog>(*buffer, *allocator,
                                           header.sys_tables_first_page,
                                           header.sys_columns_first_page);
  if (Status status = catalog->Load()) {
    return *status;
  }
  PageResult<std::vector<PageId>> free_pages = allocator->FreePages();
  if (!free_pages.ok()) {
    if (free_pages.error().code == PageErrorCode::kCorrupt) {
      return Corrupt(free_pages.error().message);
    }
    return free_pages.error();
  }
  return Database(std::move(store), std::move(buffer), std::move(allocator),
                  std::move(catalog));
}

PageResult<TableHeap*> Database::Table(const std::string& name) {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  return TableLocked(name);
}

PageResult<TableHeap*> Database::TableLocked(const std::string& name) {
  if (is_closed_) {
    return MakeError(PageErrorCode::kClosed, "the database is closed");
  }
  const std::string key = domain::ToLower(name);
  auto cached = tables_.find(key);
  if (cached != tables_.end()) {
    return cached->second.get();
  }
  const std::optional<domain::TableSchema> schema = catalog_->FindTable(name);
  const std::optional<std::uint32_t> table_id = catalog_->TableId(name);
  const std::optional<PageId> first_page = catalog_->FirstPage(name);
  if (!schema.has_value() || !table_id.has_value() || !first_page.has_value()) {
    return MakeError(PageErrorCode::kNotFound, "no table named " + name);
  }
  PageResult<TableHeap> opened =
      TableHeap::Open(*buffer_, *allocator_, *schema, *table_id, *first_page);
  if (!opened.ok()) {
    return opened.error();
  }
  // The index is opened with its heap; a root that fails BTree::Open fails the
  // table, and neither object is cached.
  std::unique_ptr<BTree> tree;
  const std::optional<PageId> index_root = catalog_->IndexRoot(name);
  if (index_root.has_value()) {
    PageResult<BTree> opened_tree =
        BTree::Open(*buffer_, *allocator_, *table_id, *index_root);
    if (!opened_tree.ok()) {
      return DamagedIndex(schema->name, opened_tree.error());
    }
    tree = std::make_unique<BTree>(std::move(opened_tree).value());
  }
  auto heap = std::make_unique<TableHeap>(std::move(opened).value());
  TableHeap* const pointer = heap.get();
  tables_.emplace(key, std::move(heap));
  if (tree != nullptr) {
    trees_.emplace(key, std::move(tree));
  }
  return pointer;
}

PageResult<BTree*> Database::Index(const std::string& name) {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  PageResult<TableHeap*> opened = TableLocked(name);
  if (!opened.ok()) {
    return opened.error();
  }
  const auto found = trees_.find(domain::ToLower(name));
  BTree* const tree = found == trees_.end() ? nullptr : found->second.get();
  return tree;
}

Status Database::CheckIndex(const std::string& name) {
  TableHeap* heap = nullptr;
  BTree* tree = nullptr;
  {
    std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
    PageResult<TableHeap*> opened = TableLocked(name);
    if (!opened.ok()) {
      return opened.error();
    }
    heap = opened.value();
    const auto found = trees_.find(domain::ToLower(name));
    tree = found == trees_.end() ? nullptr : found->second.get();
  }
  const std::optional<domain::TableSchema> schema = catalog_->FindTable(name);
  if (tree == nullptr || !schema.has_value()) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "table " + name + " has no index");
  }
  if (Status status = tree->CheckInvariants()) {
    return DamagedIndex(schema->name, *status);
  }
  // Every row has an entry with its key and its Rid.
  std::size_t row_count = 0;
  HeapScan scan = heap->Scan();
  while (true) {
    PageResult<std::optional<ScanItem>> next = scan.Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      break;
    }
    ++row_count;
    const std::int64_t key =
        KeyOf(next.value()->row, schema->primary_key_index);
    const Rid rid = next.value()->rid;
    PageResult<std::optional<Rid>> found = tree->Find(key);
    if (!found.ok()) {
      return DamagedIndex(schema->name, found.error());
    }
    if (!found.value().has_value()) {
      return IndexCorrupt("index of " + schema->name +
                          " has no entry for key " + std::to_string(key));
    }
    if (*found.value() != rid) {
      return IndexCorrupt("index of " + schema->name + " names the row " +
                          std::to_string(found.value()->page_id) + ":" +
                          std::to_string(found.value()->slot_id) + " for key " +
                          std::to_string(key) + ", but the row is at " +
                          std::to_string(rid.page_id) + ":" +
                          std::to_string(rid.slot_id));
    }
  }
  // As many entries as rows, so no entry is left without a row.
  std::size_t entry_count = 0;
  BTreeCursor cursor = tree->Scan(KeyRange{});
  while (true) {
    PageResult<std::optional<BTreeEntry>> entry = cursor.Next();
    if (!entry.ok()) {
      return DamagedIndex(schema->name, entry.error());
    }
    if (!entry.value().has_value()) {
      break;
    }
    ++entry_count;
  }
  if (entry_count != row_count) {
    return IndexCorrupt("index of " + schema->name + " has " +
                        std::to_string(entry_count) + " entries for " +
                        std::to_string(row_count) + " rows");
  }
  return std::nullopt;
}

PageResult<domain::TableSchema> Database::CreateTable(
    const domain::TableSchema& schema) {
  {
    std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
    if (is_closed_) {
      return MakeError(PageErrorCode::kClosed, "the database is closed");
    }
  }
  return catalog_->CreateTable(schema);
}

PageResult<domain::TableSchema> Database::AddColumn(
    const std::string& table, const domain::ColumnDef& column) {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  if (is_closed_) {
    return MakeError(PageErrorCode::kClosed, "the database is closed");
  }
  PageResult<TableHeap*> opened = TableLocked(table);
  if (!opened.ok()) {
    return opened.error();
  }
  PageResult<domain::TableSchema> extended =
      catalog_->CheckAddColumn(table, column);
  if (!extended.ok()) {
    return extended.error();
  }
  TableHeap* const old_heap = opened.value();
  const auto old_tree_entry = trees_.find(domain::ToLower(table));
  const BTree* const old_tree =
      old_tree_entry == trees_.end() ? nullptr : old_tree_entry->second.get();
  const domain::Value& default_value = *column.default_value;
  // The pages of the old tree are listed before anything changes, so a
  // damaged tree fails the statement here and not when the old pages are
  // released.
  std::vector<PageId> old_tree_pages;
  if (old_tree != nullptr) {
    PageResult<std::vector<PageId>> listed = old_tree->Pages();
    if (!listed.ok()) {
      return DamagedIndex(extended.value().name, listed.error());
    }
    old_tree_pages = std::move(listed).value();
  }
  if (Status status =
          CheckRowsFit(*old_heap, extended.value(), default_value)) {
    return *status;
  }
  const std::uint32_t table_id = old_heap->table_id();
  PageResult<PageId> created = TableHeap::Create(*allocator_, table_id);
  if (!created.ok()) {
    return created.error();
  }
  const PageId new_first_page = created.value();
  PageResult<TableHeap> reopened = TableHeap::Open(
      *buffer_, *allocator_, extended.value(), table_id, new_first_page);
  if (!reopened.ok()) {
    return DiscardNewPages(*allocator_, new_first_page, nullptr,
                           reopened.error());
  }
  auto new_heap = std::make_unique<TableHeap>(std::move(reopened).value());
  // A new tree beside the new chain: every row gets a new Rid, so nothing is
  // visible through the catalog until one row update switches both.
  std::unique_ptr<BTree> new_tree;
  if (old_tree != nullptr) {
    PageResult<BTree> created_tree =
        BTree::Create(*buffer_, *allocator_, table_id);
    if (!created_tree.ok()) {
      return DiscardNewPages(*allocator_, new_first_page, nullptr,
                             created_tree.error());
    }
    new_tree = std::make_unique<BTree>(std::move(created_tree).value());
  }
  if (Status status = CopyRowsWithDefault(*old_heap, new_heap.get(),
                                          default_value, new_tree.get(),
                                          extended.value().primary_key_index)) {
    return DiscardNewPages(*allocator_, new_first_page, new_tree.get(),
                           *status);
  }
  const PageId new_index_root = new_tree == nullptr ? 0 : new_tree->root();
  if (Status status = catalog_->CommitAddColumn(table, column, new_first_page,
                                                new_index_root)) {
    return DiscardNewPages(*allocator_, new_first_page, new_tree.get(),
                           *status);
  }
  const PageId old_first_page = old_heap->first_page();
  const std::string key = domain::ToLower(table);
  tables_[key] = std::move(new_heap);
  if (new_tree != nullptr) {
    trees_[key] = std::move(new_tree);
  }
  if (Status status = allocator_->Release(old_first_page)) {
    return NotReclaimed(*status);
  }
  if (Status status = allocator_->ReleasePages(old_tree_pages)) {
    return NotReclaimed(*status);
  }
  return extended;
}

Status Database::DropTable(const std::string& table) {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  if (is_closed_) {
    return MakeError(PageErrorCode::kClosed, "the database is closed");
  }
  PageResult<TableHeap*> opened = TableLocked(table);
  if (!opened.ok()) {
    return opened.error();
  }
  const PageId first_page = opened.value()->first_page();
  const std::string key = domain::ToLower(table);
  // The pages of the index are listed before anything changes, so a damaged
  // tree fails the statement before the catalog rows are deleted.
  std::vector<PageId> index_pages;
  const auto tree_entry = trees_.find(key);
  if (tree_entry != trees_.end()) {
    PageResult<std::vector<PageId>> listed = tree_entry->second->Pages();
    if (!listed.ok()) {
      return DamagedIndex(table, listed.error());
    }
    index_pages = std::move(listed).value();
  }
  if (Status status = catalog_->DropTable(table)) {
    return status;
  }
  tables_.erase(key);
  trees_.erase(key);
  if (Status status = allocator_->Release(first_page)) {
    return NotReclaimed(*status);
  }
  if (Status status = allocator_->ReleasePages(index_pages)) {
    return NotReclaimed(*status);
  }
  return std::nullopt;
}

Status Database::Close() {
  std::lock_guard<std::mutex> tables_lock(*tables_mutex_);
  if (is_closed_) {
    return std::nullopt;
  }
  if (Status status = buffer_->FlushAll()) {
    return status;
  }
  trees_.clear();
  tables_.clear();
  is_closed_ = true;
  return std::nullopt;
}

}  // namespace strata::paged
