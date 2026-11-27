#include "strata/infrastructure.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "strata/csv_codec.h"

namespace strata::infrastructure {

namespace {

constexpr const char* kLfEnding = "\n";
constexpr const char* kCrLfEnding = "\r\n";
constexpr const char* kCatalogFileName = "catalog.csv";
constexpr const char* kReservedTableName = "catalog";

// ----------------------------------------------------------------------------
// Pure helpers: computation over data in hand, no file or thread access.
// ----------------------------------------------------------------------------

std::string StripTrailingCarriageReturn(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

// How a CSV file spells its header line and its line breaks.
struct FileFormat {
  std::string header;  // Without the line ending.
  std::string line_ending = kLfEnding;
  bool has_header = false;  // False when the header comes from a schema.
};

// The header line fixes the convention for the whole file: a CRLF header means
// every line written gets CRLF, so the file never mixes endings.
FileFormat FormatFromHeaderLine(const std::string& raw_header_line) {
  bool is_crlf = !raw_header_line.empty() && raw_header_line.back() == '\r';
  return FileFormat{StripTrailingCarriageReturn(raw_header_line),
                    is_crlf ? kCrLfEnding : kLfEnding, true};
}

FileFormat DefaultFileFormat(const domain::TableSchema& schema) {
  return FileFormat{HeaderLine(schema), kLfEnding, false};
}

std::string BuildTableText(const FileFormat& format,
                           const std::vector<domain::Row>& rows) {
  std::string text = format.header + format.line_ending;
  for (const auto& row : rows) {
    text += SerializeRow(row);
    text += format.line_ending;
  }
  return text;
}

// is_line_open: the file is non-empty and its last byte is not '\n', so the
// new row would be glued to the previous line without a break.
std::string BuildAppendText(const FileFormat& format, bool is_line_open,
                            const std::vector<domain::Row>& rows) {
  std::string text;
  if (is_line_open) {
    text += format.line_ending;
  }
  if (!format.has_header) {
    text += format.header;
    text += format.line_ending;
  }
  for (const domain::Row& row : rows) {
    text += SerializeRow(row);
    text += format.line_ending;
  }
  return text;
}

// "visitors.csv" for a table named Visitors: the file name is the lower-case
// table name, whatever the declared spelling.
std::string TableFileName(const std::string& table_name) {
  return domain::ToLower(table_name) + ".csv";
}

std::string NotRegularFile(const std::string& file_name) {
  return "not a regular file: " + file_name;
}

bool IsReservedTableName(const std::string& table_name) {
  return domain::ToLower(table_name) == kReservedTableName;
}

std::string ReservedTableNameMessage() {
  return std::string("reserved table name ") + kReservedTableName;
}

// The rows kept when several rows hold the same primary key (0.0 and -0.0 are
// the same key): the last one, in its own position.
std::vector<domain::Row> KeepLastOfDuplicateKeys(std::vector<domain::Row> rows,
                                                 std::size_t key_index) {
  std::unordered_map<std::string, std::size_t> last_position;
  last_position.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    last_position[domain::FormatKey(rows[i][key_index])] = i;
  }
  if (last_position.size() == rows.size()) {
    return rows;
  }
  std::vector<domain::Row> kept;
  kept.reserve(last_position.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (last_position[domain::FormatKey(rows[i][key_index])] == i) {
      kept.push_back(std::move(rows[i]));
    }
  }
  return kept;
}

// What occupies a path. A symlink is kOther: the writers never follow one.
enum class PathKind { kMissing, kRegularFile, kOther };

// False when the path cannot be inspected.
bool TryClassifyPath(const std::string& path, PathKind& kind) {
  std::error_code error;
  std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  // Checked before the error code: implementations differ on whether a
  // missing path also sets it.
  if (status.type() == std::filesystem::file_type::not_found) {
    kind = PathKind::kMissing;
    return true;
  }
  if (error) {
    return false;
  }
  kind = status.type() == std::filesystem::file_type::regular
             ? PathKind::kRegularFile
             : PathKind::kOther;
  return true;
}

// "leftover <file name>.orig" when an interrupted commit left the backup of
// path behind, unreadable when the backup's path cannot be inspected, and
// std::nullopt when there is none.
std::optional<std::string> FindLeftoverOrig(const std::string& path,
                                            const std::string& file_name,
                                            const std::string& unreadable) {
  PathKind kind = PathKind::kOther;
  if (!TryClassifyPath(path + ".orig", kind)) {
    return unreadable;
  }
  if (kind != PathKind::kMissing) {
    return "leftover " + file_name + ".orig";
  }
  return std::nullopt;
}

// A directory opens successfully as a stream and then reads as empty, so the
// type is checked first. A symlink to a regular file is accepted for reading.
bool TryOpenRegularFile(const std::string& path, std::ifstream& file) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return false;
  }
  file.open(path);
  return file.is_open();
}

// ----------------------------------------------------------------------------
// Loading.
// ----------------------------------------------------------------------------

// The rows of one table file, or why it cannot be loaded.
struct TableFileRows {
  std::vector<domain::Row> rows;
  std::vector<std::size_t> rejected_lines;  // see LoadedTable
};
using TableRows = std::variant<TableFileRows, std::string>;

// Reads the header (the first non-empty line, which must name the schema's
// columns) and then every row that parses under the schema. A row the schema
// rejects is skipped; a file with no non-empty line has no rows. When
// is_deduplicated is set, only the last row of a repeated primary key is kept.
TableRows ReadTableFile(const std::string& path, const std::string& file_name,
                        const domain::TableSchema& schema,
                        const std::string& mismatch_reason,
                        bool is_deduplicated) {
  std::ifstream file;
  if (!TryOpenRegularFile(path, file)) {
    return "cannot read " + file_name;
  }

  std::string line;
  std::size_t line_number = 0;
  bool has_header = false;
  while (std::getline(file, line)) {
    ++line_number;
    line = StripTrailingCarriageReturn(std::move(line));
    if (!line.empty()) {
      has_header = true;
      break;
    }
  }
  std::vector<domain::Row> rows;
  std::vector<std::size_t> rejected_lines;
  if (has_header) {
    std::optional<std::vector<std::string>> header = ParseCsvRecord(line);
    if (!header || !HeaderMatches(*header, schema)) {
      return "header of " + file_name + " does not match " + mismatch_reason;
    }
    while (std::getline(file, line)) {
      ++line_number;
      line = StripTrailingCarriageReturn(std::move(line));
      if (line.empty()) {
        continue;
      }
      std::optional<std::vector<std::string>> fields = ParseCsvRecord(line);
      std::optional<domain::Row> row;
      if (fields) {
        row = ParseRow(*fields, schema);
      }
      if (row) {
        rows.push_back(std::move(*row));
      } else {
        rejected_lines.push_back(line_number);
      }
    }
  }
  if (file.bad()) {
    return "cannot read " + file_name;
  }
  if (!is_deduplicated) {
    return TableFileRows{std::move(rows), std::move(rejected_lines)};
  }
  return TableFileRows{
      KeepLastOfDuplicateKeys(std::move(rows), schema.primary_key_index),
      std::move(rejected_lines)};
}

// The schemas of catalog.csv; none when the file does not exist.
std::variant<std::vector<domain::TableSchema>, std::string> ReadCatalog(
    const std::string& directory) {
  const std::string path = directory + "/" + kCatalogFileName;
  PathKind kind = PathKind::kOther;
  if (!TryClassifyPath(path, kind)) {
    return std::string("cannot read ") + kCatalogFileName;
  }
  if (kind == PathKind::kMissing) {
    return std::vector<domain::TableSchema>();
  }
  std::ifstream file;
  if (!TryOpenRegularFile(path, file)) {
    return std::string("cannot read ") + kCatalogFileName;
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (file.bad()) {
    return std::string("cannot read ") + kCatalogFileName;
  }
  auto parsed = ParseCatalog(text);
  if (auto* error = std::get_if<std::string>(&parsed)) {
    return std::string("invalid ") + kCatalogFileName + ": " + *error;
  }
  return std::move(std::get<std::vector<domain::TableSchema>>(parsed));
}

// One table to load. Each worker thread writes only the result of the jobs it
// takes, so the jobs share nothing mutable besides the job counter.
struct TableJob {
  const domain::TableSchema* schema = nullptr;
  std::string path;
  std::string file_name;
  TableRows result;
};

// Thread entry point: takes jobs by increasing index until none is left.
void RunTableJobs(std::vector<TableJob>* jobs,
                  std::atomic<std::size_t>* next_job) {
  while (true) {
    std::size_t index = next_job->fetch_add(1);
    if (index >= jobs->size()) {
      return;
    }
    TableJob& job = (*jobs)[index];
    job.result = ReadTableFile(job.path, job.file_name, *job.schema,
                               "the catalog", true);
  }
}

// The jobs and the job counter, passed to a worker thread as one pointer. The
// caller owns both and keeps them alive until every worker is joined.
struct TableJobQueue {
  std::vector<TableJob>* jobs = nullptr;
  std::atomic<std::size_t>* next_job = nullptr;
};

// pthread_create entry point for RunTableJobs.
void* RunTableJobsThread(void* argument) {
  auto* queue = static_cast<TableJobQueue*>(argument);
  RunTableJobs(queue->jobs, queue->next_job);
  return nullptr;
}

// Owns the worker threads it starts and joins each one exactly once, in Join
// or on destruction. pthread_create is used instead of std::thread because it
// reports a thread it cannot start with an error code, while std::thread can
// report it only by throwing.
class WorkerThreads {
 public:
  explicit WorkerThreads(std::size_t capacity) { threads_.reserve(capacity); }
  WorkerThreads(const WorkerThreads&) = delete;
  WorkerThreads& operator=(const WorkerThreads&) = delete;
  ~WorkerThreads() { Join(); }

  // Starts one thread running RunTableJobsThread(queue). Returns false when
  // the reserved capacity is used up or the thread cannot be started. Checking
  // the capacity first means push_back never allocates, so a started thread is
  // always recorded and joined.
  bool Start(TableJobQueue* queue) {
    if (threads_.size() == threads_.capacity()) {
      return false;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, RunTableJobsThread, queue) != 0) {
      return false;
    }
    threads_.push_back(thread);
    return true;
  }

  // Waits for every started thread. The join makes the results a worker wrote
  // visible to the calling thread.
  void Join() {
    for (pthread_t thread : threads_) {
      pthread_join(thread, nullptr);
    }
    threads_.clear();
  }

  std::size_t StartedCount() const { return threads_.size(); }

 private:
  std::vector<pthread_t> threads_;
};

// A worker that cannot be started stops the starting of further workers, and
// the calling thread takes the jobs left over, so the result does not depend
// on how many workers started.
void RunJobsInParallel(std::vector<TableJob>& jobs) {
  std::size_t hardware = std::thread::hardware_concurrency();
  std::size_t worker_count =
      std::min(jobs.size(), std::max<std::size_t>(hardware, 1));
  std::atomic<std::size_t> next_job{0};
  TableJobQueue queue{&jobs, &next_job};
  WorkerThreads workers(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    if (!workers.Start(&queue)) {
      break;
    }
  }
  if (workers.StartedCount() < worker_count) {
    RunTableJobs(&jobs, &next_job);
  }
  workers.Join();
}

// Reads the header line and line ending of an existing file. A file with no
// non-empty line yields the schema default; an unreadable file yields
// std::nullopt. The header is the first line that is not empty once a trailing
// '\r' is removed, as the loaders see it, so a rewritten file reloads with the
// same rows.
std::optional<FileFormat> ReadFileFormat(const std::string& path,
                                         const domain::TableSchema& schema) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (!StripTrailingCarriageReturn(line).empty()) {
      return FormatFromHeaderLine(line);
    }
  }
  if (file.bad()) {
    return std::nullopt;
  }
  return DefaultFileFormat(schema);
}

// A missing file takes the schema default; an unreadable one is an error.
std::optional<FileFormat> ResolveFormat(const std::string& path,
                                        bool is_present,
                                        const domain::TableSchema& schema) {
  if (!is_present) {
    return DefaultFileFormat(schema);
  }
  return ReadFileFormat(path, schema);
}

// Precondition: the file is non-empty.
bool TryReadLastByte(const std::string& path, char& last_byte) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  file.seekg(-1, std::ios::end);
  return static_cast<bool>(file.get(last_byte));
}

// kNotOpened means the file was not touched; kFailed means it may hold a
// partial write.
enum class WriteResult { kWritten, kNotOpened, kFailed };

// mode is std::ios::trunc or std::ios::app. close() flushes, so the failure
// check after it also covers an error raised by the final write.
WriteResult WriteText(const std::string& path, const std::string& text,
                      std::ios::openmode mode) {
  std::ofstream file(path, std::ios::binary | mode);
  if (!file.is_open()) {
    return WriteResult::kNotOpened;
  }
  file << text;
  file.close();
  return file.fail() ? WriteResult::kFailed : WriteResult::kWritten;
}

bool TryCopyPermissions(const std::string& from, const std::string& to) {
  std::error_code error;
  std::filesystem::file_status status = std::filesystem::status(from, error);
  if (error) {
    return false;
  }
  std::filesystem::permissions(to, status.permissions(), error);
  return !error;
}

bool RenameFile(const std::string& from, const std::string& to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  return !error;
}

// Best effort for cleanup after the outcome is decided: the description the
// caller returns has no place for a second failure.
void RemoveFile(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

// ----------------------------------------------------------------------------
// Staged commit (docs/DesignDocument.md section 3.3, ADR-0008).
// ----------------------------------------------------------------------------

enum class StagedKind { kCatalog, kTable, kDrop };

// One file of a Commit call and how far its commit has progressed.
struct StagedFile {
  StagedKind kind = StagedKind::kTable;
  const TableWrite* write = nullptr;  // kTable; not owned
  const std::vector<domain::TableSchema>* catalog = nullptr;  // kCatalog
  std::string display_name;  // "visitors.csv", as the descriptions name it
  std::string path;
  std::string temp_path;
  std::string orig_path;
  bool has_original = false;  // The file existed before the commit.
  bool is_backed_up = false;  // path was renamed to orig_path.
  bool is_installed = false;  // temp_path was renamed to path.
};

StagedFile MakeStagedFile(StagedKind kind, std::string display_name,
                          std::string path) {
  StagedFile file;
  file.kind = kind;
  file.display_name = std::move(display_name);
  file.temp_path = path + ".temp";
  file.orig_path = path + ".orig";
  file.path = std::move(path);
  return file;
}

std::string CouldNotWrite(const StagedFile& file) {
  return "could not write " + file.display_name;
}

void RemoveTempFiles(const std::vector<StagedFile>& staged) {
  for (const auto& file : staged) {
    RemoveFile(file.temp_path);
  }
}

// A file whose owner-write bit is cleared is read-only by intent; replacing it
// through a rename would bypass that protection.
bool IsOwnerWritable(const std::string& path) {
  std::error_code error;
  std::filesystem::file_status status = std::filesystem::status(path, error);
  if (error) {
    return false;
  }
  return (status.permissions() & std::filesystem::perms::owner_write) !=
         std::filesystem::perms::none;
}

// The files of the commit in the order they are swapped: catalog.csv, the
// tables to write, the tables to drop. A table named twice, or the reserved
// name, is refused: the second entry would back up the first one's new content
// over the original.
std::variant<std::vector<StagedFile>, std::string> BuildStagedFiles(
    const std::string& directory, const CommitSet& set) {
  std::vector<StagedFile> staged;
  std::set<std::string> names;
  if (set.catalog) {
    StagedFile file = MakeStagedFile(StagedKind::kCatalog, kCatalogFileName,
                                     directory + "/" + kCatalogFileName);
    file.catalog = &*set.catalog;
    staged.push_back(std::move(file));
  }
  for (const TableWrite& write : set.writes) {
    const std::string name = domain::ToLower(write.schema.name);
    if (IsReservedTableName(name)) {
      return ReservedTableNameMessage();
    }
    if (!names.insert(name).second) {
      return "duplicate table " + name;
    }
    StagedFile file = MakeStagedFile(StagedKind::kTable, TableFileName(name),
                                     directory + "/" + TableFileName(name));
    file.write = &write;
    staged.push_back(std::move(file));
  }
  for (const std::string& drop : set.drops) {
    const std::string name = domain::ToLower(drop);
    if (IsReservedTableName(name)) {
      return ReservedTableNameMessage();
    }
    if (!names.insert(name).second) {
      return "duplicate table " + name;
    }
    staged.push_back(MakeStagedFile(StagedKind::kDrop, TableFileName(name),
                                    directory + "/" + TableFileName(name)));
  }
  return staged;
}

bool IsNewFile(const StagedFile& file) {
  return file.write != nullptr && file.write->is_new_file;
}

// A symbolic link at the .temp path is refused: the staged write opens that
// path and would follow the link, so the new content would land in the link's
// target. A leftover regular file or a directory there is not refused; the
// write replaces the file or fails.
std::optional<std::string> RefuseLinkedTemp(const StagedFile& file) {
  if (file.kind == StagedKind::kDrop) {
    return std::nullopt;
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(file.temp_path, error);
  if (status.type() == std::filesystem::file_type::symlink) {
    return NotRegularFile(file.display_name + ".temp");
  }
  return std::nullopt;
}

// Refuses a leftover .orig, an existing target of a new file, a target that is
// not a regular file, a read-only target and a symbolic link at a .temp path,
// and records which targets exist.
std::optional<std::string> InspectTargets(std::vector<StagedFile>& staged) {
  for (auto& file : staged) {
    PathKind orig_kind = PathKind::kOther;
    if (!TryClassifyPath(file.orig_path, orig_kind)) {
      return CouldNotWrite(file);
    }
    if (orig_kind != PathKind::kMissing) {
      return "leftover " + file.display_name + ".orig";
    }
    PathKind kind = PathKind::kOther;
    if (!TryClassifyPath(file.path, kind)) {
      return CouldNotWrite(file);
    }
    // CREATE TABLE can adopt only a regular file, so only then is it named.
    if (IsNewFile(file) && kind == PathKind::kRegularFile) {
      return file.display_name +
             " already exists; use CREATE TABLE to adopt it";
    }
    if (IsNewFile(file) && kind == PathKind::kOther) {
      return file.display_name + " already exists and is not a regular file";
    }
    if (kind == PathKind::kOther) {
      return NotRegularFile(file.display_name);
    }
    file.has_original = kind == PathKind::kRegularFile;
    if (file.has_original && !IsOwnerWritable(file.path)) {
      return CouldNotWrite(file);
    }
    if (std::optional<std::string> linked = RefuseLinkedTemp(file)) {
      return linked;
    }
  }
  return std::nullopt;
}

// The full text of the new file, or std::nullopt when the original's format
// cannot be read.
std::optional<std::string> BuildNewText(const StagedFile& file) {
  if (file.kind == StagedKind::kCatalog) {
    return SerializeCatalog(*file.catalog);
  }
  const domain::TableSchema& schema = file.write->schema;
  std::optional<FileFormat> format =
      ResolveFormat(file.path, file.has_original, schema);
  if (!format) {
    return std::nullopt;
  }
  if (file.write->should_regenerate_header) {
    format->header = HeaderLine(schema);
    format->has_header = true;
  }
  return BuildTableText(*format, file.write->rows);
}

// Writes the new content to the .temp file. The replacement takes the
// original's permissions, because the rename would otherwise swap in default
// ones. A dropped table has nothing to write.
bool TryStageFile(const StagedFile& file) {
  if (file.kind == StagedKind::kDrop) {
    return true;
  }
  std::optional<std::string> text = BuildNewText(file);
  if (!text) {
    return false;
  }
  if (WriteText(file.temp_path, *text, std::ios::trunc) !=
      WriteResult::kWritten) {
    return false;
  }
  return !file.has_original || TryCopyPermissions(file.path, file.temp_path);
}

std::optional<std::string> WriteTempFiles(
    const std::vector<StagedFile>& staged) {
  for (const auto& file : staged) {
    if (!TryStageFile(file)) {
      RemoveTempFiles(staged);
      return CouldNotWrite(file);
    }
  }
  return std::nullopt;
}

// Undoes a partly finished swap and returns the failure description. A file
// that did not exist before the commit has no .orig, so its installed copy is
// removed to return to the original state. A failed restore is reported over
// the original failure because the user must act on it; the other files are
// still restored.
std::string RollBack(const std::vector<StagedFile>& staged,
                     const StagedFile& failed) {
  std::optional<std::string> restore_failure;
  for (const auto& file : staged) {
    if (file.is_backed_up) {
      if (!RenameFile(file.orig_path, file.path) && !restore_failure) {
        restore_failure =
            "restore failed: " + file.display_name + ".orig holds the original";
      }
    } else if (file.is_installed) {
      RemoveFile(file.path);
    }
  }
  RemoveTempFiles(staged);
  return restore_failure ? *restore_failure : CouldNotWrite(failed);
}

// rename() replaces its target atomically, so the original path never holds a
// partly written file. A dropped table is only moved to .orig.
std::optional<std::string> SwapFiles(std::vector<StagedFile>& staged) {
  for (auto& file : staged) {
    if (file.has_original) {
      if (!RenameFile(file.path, file.orig_path)) {
        return RollBack(staged, file);
      }
      file.is_backed_up = true;
    }
    if (file.kind == StagedKind::kDrop) {
      continue;
    }
    if (!RenameFile(file.temp_path, file.path)) {
      return RollBack(staged, file);
    }
    file.is_installed = true;
  }
  return std::nullopt;
}

// A failed removal leaves an .orig that fails the next load and that the next
// commit reports as a leftover; it does not change the result of this commit.
// For a dropped table this is the step that deletes its data.
void RemoveBackups(const std::vector<StagedFile>& staged) {
  for (const auto& file : staged) {
    if (file.is_backed_up) {
      RemoveFile(file.orig_path);
    }
  }
}

// ----------------------------------------------------------------------------
// Append.
// ----------------------------------------------------------------------------

// What an append needs to know about the target file.
struct AppendState {
  FileFormat format;
  std::uintmax_t size_before = 0;
  bool is_line_open = false;
};

bool TryReadAppendState(const std::string& path, bool is_present,
                        const domain::TableSchema& schema, AppendState& state) {
  std::optional<FileFormat> format = ResolveFormat(path, is_present, schema);
  if (!format) {
    return false;
  }
  state.format = std::move(*format);
  if (!is_present) {
    return true;
  }

  std::error_code error;
  state.size_before = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  if (state.size_before == 0) {
    return true;
  }
  char last_byte = '\n';
  if (!TryReadLastByte(path, last_byte)) {
    return false;
  }
  state.is_line_open = last_byte != '\n';
  return true;
}

// Returns the file to its state before the append: cuts a partial line off an
// existing file, or removes a file the append created.
bool TryRollBackAppend(const std::string& path, bool is_present,
                       std::uintmax_t size_before) {
  std::error_code error;
  if (is_present) {
    std::filesystem::resize_file(path, size_before, error);
  } else {
    std::filesystem::remove(path, error);
  }
  return !error;
}

}  // namespace

CsvRepository::CsvRepository(std::string directory)
    : directory_(std::move(directory)) {}

// A leftover .orig of catalog.csv or of a table means an interrupted commit,
// which can leave a mix of old and new files, so the load fails. Each stamp
// is taken before its file is read, so a change made while reading is caught
// by the next write.
LoadOutcome CsvRepository::Load(bool is_parallel) {
  const std::string catalog_path = directory_ + "/" + kCatalogFileName;
  if (auto leftover = FindLeftoverOrig(
          catalog_path, kCatalogFileName,
          std::string("cannot read ") + kCatalogFileName + ".orig")) {
    return *leftover;
  }
  std::map<std::string, FileStamp> stamps = {
      {kCatalogFileName, StampOf(kCatalogFileName)}};
  auto catalog = ReadCatalog(directory_);
  if (auto* error = std::get_if<std::string>(&catalog)) {
    return *error;
  }
  const std::vector<domain::TableSchema>& schemas =
      std::get<std::vector<domain::TableSchema>>(catalog);

  std::vector<TableJob> jobs(schemas.size());
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    jobs[i].schema = &schemas[i];
    jobs[i].file_name = TableFileName(schemas[i].name);
    jobs[i].path = directory_ + "/" + jobs[i].file_name;
    if (auto leftover =
            FindLeftoverOrig(jobs[i].path, jobs[i].file_name,
                             "cannot read " + jobs[i].file_name + ".orig")) {
      return *leftover;
    }
    stamps[jobs[i].file_name] = StampOf(jobs[i].file_name);
  }
  if (is_parallel && jobs.size() > 1) {
    RunJobsInParallel(jobs);
  } else {
    for (TableJob& job : jobs) {
      job.result = ReadTableFile(job.path, job.file_name, *job.schema,
                                 "the catalog", true);
    }
  }

  std::vector<LoadedTable> tables;
  tables.reserve(jobs.size());
  for (TableJob& job : jobs) {
    if (auto* error = std::get_if<std::string>(&job.result)) {
      return *error;
    }
    TableFileRows& file_rows = std::get<TableFileRows>(job.result);
    tables.push_back(LoadedTable{*job.schema, std::move(file_rows.rows),
                                 std::move(file_rows.rejected_lines)});
  }
  for (const auto& [file_name, stamp] : stamps) {
    stamps_[file_name] = stamp;
  }
  return tables;
}

InspectOutcome CsvRepository::InspectTable(const domain::TableSchema& schema) {
  if (IsReservedTableName(schema.name)) {
    return ReservedTableNameMessage();
  }
  const std::string file_name = TableFileName(schema.name);
  const std::string path = directory_ + "/" + file_name;
  // Taken before the read, so a change made while reading is caught by the
  // next write. Recorded only for an outcome the engine can act on.
  const FileStamp stamp = StampOf(file_name);
  PathKind kind = PathKind::kOther;
  if (!TryClassifyPath(path, kind)) {
    return "cannot read " + file_name;
  }
  if (kind == PathKind::kMissing) {
    stamps_[file_name] = stamp;
    return InspectOutcome(std::optional<std::vector<domain::Row>>());
  }
  if (kind == PathKind::kOther) {
    return NotRegularFile(file_name);
  }
  // The rows are not de-duplicated: adopting the file must see a repeated key.
  TableRows rows =
      ReadTableFile(path, file_name, schema, "the declared columns", false);
  if (auto* error = std::get_if<std::string>(&rows)) {
    return *error;
  }
  stamps_[file_name] = stamp;
  return InspectOutcome(std::optional<std::vector<domain::Row>>(
      std::move(std::get<TableFileRows>(rows).rows)));
}

CsvRepository::FileStamp CsvRepository::StampOf(
    const std::string& file_name) const {
  const std::string path = directory_ + "/" + file_name;
  FileStamp stamp;
  std::error_code error;
  stamp.is_present = std::filesystem::exists(path, error) && !error;
  if (!stamp.is_present) {
    return stamp;
  }
  stamp.size = std::filesystem::file_size(path, error);
  if (error) {
    stamp.size = 0;
  }
  stamp.modified = std::filesystem::last_write_time(path, error);
  if (error) {
    stamp.modified = {};
  }
  return stamp;
}

std::optional<std::string> CsvRepository::FindChangedFile(
    const std::vector<std::string>& file_names) const {
  for (const std::string& file_name : file_names) {
    auto recorded = stamps_.find(file_name);
    if (recorded == stamps_.end()) {
      continue;
    }
    if (StampOf(file_name) != recorded->second) {
      return file_name +
             " changed on disk since it was loaded; run .open again";
    }
  }
  return std::nullopt;
}

void CsvRepository::RecordStamps(const std::vector<std::string>& file_names) {
  for (const std::string& file_name : file_names) {
    stamps_[file_name] = StampOf(file_name);
  }
}

std::optional<std::string> CsvRepository::Commit(const CommitSet& set) {
  auto built = BuildStagedFiles(directory_, set);
  if (auto* error = std::get_if<std::string>(&built)) {
    return *error;
  }
  std::vector<StagedFile>& staged = std::get<std::vector<StagedFile>>(built);

  // A new file is checked for existence by InspectTargets instead of against
  // a record, which can predate the removal that made room for it.
  std::vector<std::string> file_names;      // every file the commit changes
  std::vector<std::string> recorded_files;  // those compared with a record
  if (set.catalog) {
    file_names.push_back(kCatalogFileName);
    recorded_files.push_back(kCatalogFileName);
  }
  for (const TableWrite& write : set.writes) {
    file_names.push_back(TableFileName(write.schema.name));
    if (!write.is_new_file) {
      recorded_files.push_back(file_names.back());
    }
  }
  for (const std::string& name : set.drops) {
    file_names.push_back(TableFileName(name));
    recorded_files.push_back(file_names.back());
  }
  if (auto failure = FindChangedFile(recorded_files)) {
    return failure;
  }
  if (auto failure = InspectTargets(staged)) {
    return failure;
  }
  if (auto failure = WriteTempFiles(staged)) {
    return failure;
  }
  if (auto failure = SwapFiles(staged)) {
    return failure;
  }
  RemoveBackups(staged);
  RecordStamps(file_names);
  return std::nullopt;
}

std::optional<std::string> CsvRepository::AppendRows(
    const domain::TableSchema& schema, const std::vector<domain::Row>& rows) {
  if (rows.empty()) {
    return std::nullopt;
  }
  if (IsReservedTableName(schema.name)) {
    return ReservedTableNameMessage();
  }
  const std::string file_name = TableFileName(schema.name);
  const std::string path = directory_ + "/" + file_name;
  const std::string failure = "could not append to " + file_name;
  if (auto changed = FindChangedFile({file_name})) {
    return changed;
  }
  if (auto leftover = FindLeftoverOrig(path, file_name, failure)) {
    return leftover;
  }

  PathKind kind = PathKind::kOther;
  if (!TryClassifyPath(path, kind)) {
    return failure;
  }
  if (kind == PathKind::kOther) {
    return NotRegularFile(file_name);
  }
  bool is_present = kind == PathKind::kRegularFile;
  AppendState state;
  if (!TryReadAppendState(path, is_present, schema, state)) {
    return failure;
  }

  WriteResult write_result =
      WriteText(path, BuildAppendText(state.format, state.is_line_open, rows),
                std::ios::app);
  if (write_result == WriteResult::kWritten) {
    RecordStamps({file_name});
    return std::nullopt;
  }
  // An unopened file was not touched, so there is nothing to undo.
  if (write_result == WriteResult::kNotOpened) {
    return failure;
  }
  if (!TryRollBackAppend(path, is_present, state.size_before)) {
    return failure + (is_present ? "; truncating back failed"
                                 : "; removing the new file failed");
  }
  // The file holds its content from before the call again, but its
  // modification time changed; the old record would refuse the next write.
  RecordStamps({file_name});
  return failure;
}

}  // namespace strata::infrastructure
