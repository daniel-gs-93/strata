#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/domain/binder.h"
#include "strata/domain/import.h"
#include "strata/paged/btree_node.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/database.h"
#include "strata/paged/heap_access.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_engine.h"
#include "strata/paged/slotted_page.h"
#include "strata/paged/table_heap.h"
#include "test_harness.h"
#include "test_paged_support.h"
#include "test_support.h"

// HeapAccess and PagedEngine over a Database on a MemoryPageStore, and for one
// test over real files (docs/TestDocument.md).

namespace strata::testing {

namespace {

using application::QueryService;
using domain::DataType;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExecutionResult;
using domain::Row;
using domain::Value;
using paged::Database;
using paged::DatabaseOptions;
using paged::ErrorContext;
using paged::ErrorSite;
using paged::MemoryPageStore;
using paged::PagedEngine;
using paged::PageErrorCode;
using paged::PageId;
using paged::ReplacementPolicyKind;
using paged::Rid;
using Operation = paged::MemoryPageStore::Operation;

constexpr std::size_t kMinimumFrames = 4;
constexpr const char* kSavedSuffix = "; statement was applied but not saved";

// ----------------------------------------------------------------------------
// Opening an engine over a MemoryPageStore.
// ----------------------------------------------------------------------------

// An engine and a borrowed pointer to the store inside its Database. The
// pointer stays valid for as long as `engine` lives.
struct StoreEngine {
  MemoryPageStore* store = nullptr;
  std::unique_ptr<PagedEngine> engine;
};

StoreEngine OpenStoreEngine(
    std::unique_ptr<MemoryPageStore> store, std::size_t frames,
    ReplacementPolicyKind policy = ReplacementPolicyKind::kLru) {
  StoreEngine opened;
  MemoryPageStore* const borrowed = store.get();
  DatabaseOptions options;
  options.pool_frames = frames;
  options.policy = policy;
  paged::PageResult<Database> database =
      Database::OpenWithStore(std::move(store), options);
  CHECK_OK(database);
  if (!database.ok()) {
    return opened;
  }
  opened.store = borrowed;
  opened.engine =
      std::make_unique<PagedEngine>(std::move(database).value(), nullptr);
  return opened;
}

StoreEngine NewEngine(std::size_t frames) {
  return OpenStoreEngine(std::make_unique<MemoryPageStore>(), frames);
}

// A new store with the same pages as source.
std::unique_ptr<MemoryPageStore> CopyOf(const MemoryPageStore& source) {
  auto copy = std::make_unique<MemoryPageStore>(source.PageCount());
  for (PageId page_id = 0; page_id < source.PageCount(); ++page_id) {
    std::memcpy(copy->MutablePage(page_id), source.PageBytes(page_id),
                paged::kPageSize);
  }
  return copy;
}

using PageImages = std::vector<std::vector<std::uint8_t>>;

// The bytes of every page of the store.
PageImages PagesOf(const MemoryPageStore& store) {
  PageImages images;
  for (PageId page_id = 0; page_id < store.PageCount(); ++page_id) {
    const std::uint8_t* bytes = store.PageBytes(page_id);
    images.emplace_back(bytes, bytes + paged::kPageSize);
  }
  return images;
}

// ----------------------------------------------------------------------------
// Running statements and reading results.
// ----------------------------------------------------------------------------

ExecutionResult Run(PagedEngine& engine, const std::string& sql) {
  domain::PlanPtr plan = MustBind(sql, engine.Catalog());
  if (plan == nullptr) {
    return ExecutionError{ErrorKind::kBind, "test could not bind: " + sql};
  }
  return engine.Execute(*plan);
}

ExecutionResult RunInsert(PagedEngine& engine, const std::string& table,
                          std::vector<Row> rows) {
  domain::PlanNode plan;
  plan.node = domain::InsertNode{table, std::move(rows), std::nullopt};
  return engine.Execute(plan);
}

std::string DescribeResult(const ExecutionResult& result) {
  if (const ExecutionError* error = AsError(result)) {
    return std::string("error ") + domain::ErrorKindName(error->kind) + ": " +
           error->message;
  }
  if (const domain::AffectedRows* affected = AsAffectedRows(result)) {
    return "affected=" + std::to_string(affected->count);
  }
  if (const domain::CommandComplete* complete = AsCommandComplete(result)) {
    return "complete " + complete->tag;
  }
  return "a result set";
}

void ExpectAffected(const ExecutionResult& result, std::size_t count) {
  const domain::AffectedRows* affected = AsAffectedRows(result);
  if (affected == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected affected=" + std::to_string(count) + " but got " +
                      DescribeResult(result));
    return;
  }
  CHECK_EQ(affected->count, count);
}

void ExpectComplete(const ExecutionResult& result, const std::string& tag) {
  const domain::CommandComplete* complete = AsCommandComplete(result);
  if (complete == nullptr) {
    ReportFailure(
        __FILE__, __LINE__,
        "expected complete " + tag + " but got " + DescribeResult(result));
    return;
  }
  CHECK_EQ(complete->tag, tag);
}

// The error when result is one of the given kind; otherwise reports a failure
// and returns null.
const ExecutionError* ExpectError(const ExecutionResult& result,
                                  ErrorKind kind) {
  const ExecutionError* error = AsError(result);
  if (error == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  std::string("expected error ") + domain::ErrorKindName(kind) +
                      " but got " + DescribeResult(result));
    return nullptr;
  }
  CHECK_EQ(std::string(domain::ErrorKindName(error->kind)),
           std::string(domain::ErrorKindName(kind)));
  return error;
}

void ExpectErrorMessage(const ExecutionResult& result, ErrorKind kind,
                        const std::string& message) {
  const ExecutionError* error = ExpectError(result, kind);
  if (error == nullptr) {
    return;
  }
  CHECK_EQ(error->message, message);
}

bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

std::vector<std::string> TextOf(const std::vector<Row>& rows) {
  std::vector<std::string> lines;
  for (const Row& row : rows) {
    std::string line;
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) {
        line += "\t";
      }
      line += domain::FormatValue(row[i]);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<std::string> SortedTextOf(const std::vector<Row>& rows) {
  std::vector<std::string> lines = TextOf(rows);
  std::ranges::sort(lines);
  return lines;
}

// The rows of a result set, in order; reports a failure and returns no rows
// when result is not a result set.
std::vector<Row> RowsOf(const ExecutionResult& result) {
  const domain::ResultSet* result_set = AsResultSet(result);
  if (result_set == nullptr) {
    ReportFailure(__FILE__, __LINE__,
                  "expected a result set but got " + DescribeResult(result));
    return {};
  }
  return result_set->rows;
}

std::vector<std::string> QueryText(PagedEngine& engine,
                                   const std::string& sql) {
  return TextOf(RowsOf(Run(engine, sql)));
}

std::vector<std::string> SortedQueryText(PagedEngine& engine,
                                         const std::string& sql) {
  return SortedTextOf(RowsOf(Run(engine, sql)));
}

// Every row of the table, in the order TableHeap::Scan walks the page chain.
std::vector<Row> HeapRows(PagedEngine& engine, const std::string& table) {
  std::vector<Row> rows;
  paged::PageResult<paged::TableHeap*> heap = engine.database().Table(table);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return rows;
  }
  paged::HeapScan scan = heap.value()->Scan();
  while (true) {
    paged::PageResult<std::optional<paged::ScanItem>> next = scan.Next();
    CHECK_OK(next);
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    rows.push_back(std::move(next.value()->row));
  }
  return rows;
}

void ExpectNoPins(PagedEngine& engine) {
  CHECK_EQ(engine.database().buffer().PinnedFrameCount(), std::size_t{0});
}

// ----------------------------------------------------------------------------
// The data set: the three tables of the catalog fixture (visitors, stories and
// reactions), with padded text so that every table spans several pages.
// ----------------------------------------------------------------------------

constexpr const char* kCreateVisitors =
    "CREATE TABLE visitors (id INT PRIMARY KEY, username TEXT UNIQUE, location "
    "TEXT)";
constexpr const char* kCreateStories =
    "CREATE TABLE stories (id INT PRIMARY KEY, content TEXT, username TEXT "
    "REFERENCES visitors(username), views INT)";
constexpr const char* kCreateReactions =
    "CREATE TABLE reactions (id INT PRIMARY KEY, storyId INT REFERENCES "
    "stories(id), username TEXT REFERENCES visitors(username), type TEXT, "
    "comment "
    "TEXT)";

const char* const kReactionTypes[] = {"like", "comment", "share"};

struct Dataset {
  std::int64_t visitor_count;
  std::int64_t story_count;
  std::int64_t reaction_count;
  std::size_t padding;  // bytes of stories.content and reactions.comment

  static std::string UserName(std::int64_t visitor_id) {
    return "u" + std::to_string(visitor_id);
  }
  std::int64_t AuthorOf(std::int64_t story_id) const {
    return story_id % visitor_count + 1;
  }
  std::int64_t StoryOf(std::int64_t reaction_id) const {
    return reaction_id % story_count + 1;
  }
  std::int64_t ReactorOf(std::int64_t reaction_id) const {
    return (reaction_id * 7) % visitor_count + 1;
  }
  static std::int64_t ViewsOf(std::int64_t story_id) { return story_id % 10; }
  static std::string TypeOf(std::int64_t reaction_id) {
    return kReactionTypes[reaction_id % 3];
  }

  Row Visitor(std::int64_t visitor_id) const {
    return Row{Value(visitor_id), Value(UserName(visitor_id)),
               Value("city-" + std::to_string(visitor_id % 7))};
  }
  Row Story(std::int64_t story_id) const {
    return Row{Value(story_id),
               Value("story-" + std::to_string(story_id) + "-" +
                     std::string(padding, 'c')),
               Value(UserName(AuthorOf(story_id))), Value(ViewsOf(story_id))};
  }
  Row Reaction(std::int64_t reaction_id) const {
    return Row{Value(reaction_id), Value(StoryOf(reaction_id)),
               Value(UserName(ReactorOf(reaction_id))),
               Value(TypeOf(reaction_id)), Value(std::string(padding, 'm'))};
  }

  std::vector<Row> Visitors() const {
    std::vector<Row> rows;
    for (std::int64_t visitor_id = 1; visitor_id <= visitor_count;
         ++visitor_id) {
      rows.push_back(Visitor(visitor_id));
    }
    return rows;
  }
  std::vector<Row> Stories() const {
    std::vector<Row> rows;
    for (std::int64_t story_id = 1; story_id <= story_count; ++story_id) {
      rows.push_back(Story(story_id));
    }
    return rows;
  }
  std::vector<Row> Reactions() const {
    std::vector<Row> rows;
    for (std::int64_t id = 1; id <= reaction_count; ++id) {
      rows.push_back(Reaction(id));
    }
    return rows;
  }
};

// Creates the three tables and fills them.
void LoadDataset(PagedEngine& engine, const Dataset& dataset) {
  for (const char* sql : {kCreateVisitors, kCreateStories, kCreateReactions}) {
    ExecutionResult created = Run(engine, sql);
    CHECK(AsCommandComplete(created) != nullptr);
  }
  ExpectAffected(RunInsert(engine, "visitors", dataset.Visitors()),
                 static_cast<std::size_t>(dataset.visitor_count));
  ExpectAffected(RunInsert(engine, "stories", dataset.Stories()),
                 static_cast<std::size_t>(dataset.story_count));
  ExpectAffected(RunInsert(engine, "reactions", dataset.Reactions()),
                 static_cast<std::size_t>(dataset.reaction_count));
}

// The multiset of rows of the three tables, as sorted lines prefixed with the
// table name. Equal snapshots mean equal contents whatever the Rids are.
std::vector<std::string> SnapshotOf(PagedEngine& engine) {
  std::vector<std::string> snapshot;
  for (const char* table : {"visitors", "stories", "reactions"}) {
    const std::vector<std::string> lines =
        SortedQueryText(engine, std::string("SELECT * FROM ") + table);
    for (const std::string& line : lines) {
      snapshot.push_back(std::string(table) + "|" + line);
    }
  }
  return snapshot;
}

struct MappingCase {
  PageErrorCode code;
  ErrorSite site;
  ErrorKind kind;
  std::string message;
};

std::string MappingLabel(const MappingCase& test_case) {
  return std::string(ErrorCodeName(test_case.code)) + " at site " +
         std::to_string(static_cast<int>(test_case.site));
}

void CheckMappingCase(const MappingCase& test_case) {
  const paged::PageError error{test_case.code, "m"};
  const ExecutionError mapped =
      paged::ToExecutionError(error, ErrorContext{test_case.site, "Visitors"});
  CHECK_EQ(
      MappingLabel(test_case) + " kind " + domain::ErrorKindName(mapped.kind),
      MappingLabel(test_case) + " kind " +
          domain::ErrorKindName(test_case.kind));
  CHECK_EQ(MappingLabel(test_case) + " message " + mapped.message,
           MappingLabel(test_case) + " message " + test_case.message);
}

}  // namespace

TEST(test_heap_access_error_mapping) {
  const std::string too_long =
      "paged engine cannot store a row longer than 4060 bytes in Visitors";
  const ErrorSite open = ErrorSite::kOpenTable;
  const ErrorSite create = ErrorSite::kCreateTable;
  const ErrorSite add_column = ErrorSite::kAddColumn;
  const ErrorSite access = ErrorSite::kRowAccess;
  const std::vector<MappingCase> cases = {
      {PageErrorCode::kIo, access, ErrorKind::kIo, "m"},
      {PageErrorCode::kIo, open, ErrorKind::kIo, "m"},
      {PageErrorCode::kIo, create, ErrorKind::kIo, "m"},
      {PageErrorCode::kCorrupt, access, ErrorKind::kIo, "corrupt: m"},
      {PageErrorCode::kCorrupt, open, ErrorKind::kIo, "corrupt: m"},
      {PageErrorCode::kNoFrame, access, ErrorKind::kIo,
       "buffer pool exhausted: m"},
      {PageErrorCode::kNoFrame, open, ErrorKind::kIo,
       "buffer pool exhausted: m"},
      {PageErrorCode::kNoSpace, access, ErrorKind::kIo, "m"},
      {PageErrorCode::kClosed, access, ErrorKind::kIo, "database is closed"},
      {PageErrorCode::kClosed, open, ErrorKind::kIo, "database is closed"},
      {PageErrorCode::kNotFound, open, ErrorKind::kBind,
       "unknown table: Visitors"},
      {PageErrorCode::kNotFound, access, ErrorKind::kIo, "internal error: m"},
      {PageErrorCode::kNotFound, create, ErrorKind::kIo, "internal error: m"},
      {PageErrorCode::kTooLarge, access, ErrorKind::kUnsupported, too_long},
      {PageErrorCode::kTooLarge, create, ErrorKind::kUnsupported, too_long},
      {PageErrorCode::kInvalidArgument, create, ErrorKind::kUnsupported, "m"},
      {PageErrorCode::kInvalidArgument, access, ErrorKind::kIo,
       "internal error: m"},
      {PageErrorCode::kInvalidArgument, open, ErrorKind::kIo,
       "internal error: m"},
      {PageErrorCode::kAlreadyExists, create, ErrorKind::kBind,
       "table Visitors already exists"},
      {PageErrorCode::kAlreadyExists, access, ErrorKind::kIo,
       "internal error: m"},
      {PageErrorCode::kInvalidArgument, add_column, ErrorKind::kUnsupported,
       "m"},
      {PageErrorCode::kNotFound, add_column, ErrorKind::kBind,
       "unknown table: Visitors"},
      {PageErrorCode::kAlreadyExists, add_column, ErrorKind::kIo,
       "internal error: m"},
  };
  for (const MappingCase& test_case : cases) {
    CheckMappingCase(test_case);
  }
}

namespace {

std::vector<Row> ExpectedJoinRows(const Dataset& dataset) {
  std::vector<Row> rows;
  for (std::int64_t id = 1; id <= dataset.reaction_count; ++id) {
    const std::int64_t story_id = dataset.StoryOf(id);
    rows.push_back(Row{Value(Dataset::UserName(dataset.AuthorOf(story_id))),
                       Value(story_id), Value(id)});
  }
  return rows;
}

std::vector<Row> ExpectedTypeCounts(const Dataset& dataset) {
  std::map<std::string, std::int64_t> counts;
  for (std::int64_t id = 1; id <= dataset.reaction_count; ++id) {
    ++counts[Dataset::TypeOf(id)];
  }
  std::vector<Row> rows;
  for (const auto& entry : counts) {
    rows.push_back(Row{Value(entry.first), Value(entry.second)});
  }
  return rows;
}

// views descending, then id ascending.
bool ViewsDescendingThenId(const std::pair<std::int64_t, std::int64_t>& left,
                           const std::pair<std::int64_t, std::int64_t>& right) {
  if (left.first != right.first) {
    return left.first > right.first;
  }
  return left.second < right.second;
}

std::vector<std::string> ExpectedStoriesByViews(const Dataset& dataset) {
  std::vector<std::pair<std::int64_t, std::int64_t>> views_and_ids;
  for (std::int64_t id = 1; id <= dataset.story_count; ++id) {
    views_and_ids.emplace_back(Dataset::ViewsOf(id), id);
  }
  std::ranges::sort(views_and_ids, ViewsDescendingThenId);
  std::vector<std::string> ids;
  for (const auto& entry : views_and_ids) {
    ids.push_back(std::to_string(entry.second) + "\t" +
                  std::to_string(entry.first));
  }
  return ids;
}

}  // namespace

TEST(test_paged_engine_queries_with_minimum_pool_leave_no_pins) {
  const Dataset dataset{60, 240, 360, 80};
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  CHECK_EQ(engine.database().buffer().frame_count(), kMinimumFrames);
  LoadDataset(engine, dataset);
  CHECK(fixture.store->PageCount() > 12);
  ExpectNoPins(engine);

  // The row counts, in catalog order.
  const paged::RowCountResult counted = engine.RowCounts();
  const auto* counts = std::get_if<std::vector<paged::RowCount>>(&counted);
  CHECK(counts != nullptr);
  if (counts != nullptr && counts->size() == 3) {
    CHECK_EQ((*counts)[0].name, std::string("visitors"));
    CHECK_EQ((*counts)[0].count, std::size_t{60});
    CHECK_EQ((*counts)[1].name, std::string("stories"));
    CHECK_EQ((*counts)[1].count, std::size_t{240});
    CHECK_EQ((*counts)[2].name, std::string("reactions"));
    CHECK_EQ((*counts)[2].count, std::size_t{360});
  } else {
    CHECK_EQ(counts == nullptr ? std::size_t{0} : counts->size(),
             std::size_t{3});
  }
  ExpectNoPins(engine);

  // A scan returns the rows in the order TableHeap::Scan walks the chain, and
  // the rows are the ones that were loaded.
  const std::vector<Row> scanned = RowsOf(Run(engine, "SELECT * FROM stories"));
  CHECK(scanned == HeapRows(engine, "stories"));
  CHECK_EQ(SortedTextOf(scanned), SortedTextOf(dataset.Stories()));
  ExpectNoPins(engine);

  const std::string join_sql =
      "SELECT v.username, s.id, r.id FROM visitors v JOIN stories s ON "
      "v.username "
      "= s.username JOIN reactions r ON r.storyId = s.id";
  CHECK_EQ(SortedQueryText(engine, join_sql),
           SortedTextOf(ExpectedJoinRows(dataset)));
  ExpectNoPins(engine);

  CHECK_EQ(SortedQueryText(
               engine, "SELECT type, COUNT(*) FROM reactions GROUP BY type"),
           SortedTextOf(ExpectedTypeCounts(dataset)));
  std::int64_t total_views = 0;
  for (std::int64_t id = 1; id <= dataset.story_count; ++id) {
    total_views += Dataset::ViewsOf(id);
  }
  CHECK_EQ(QueryText(engine, "SELECT COUNT(*), SUM(views) FROM stories"),
           TextOf({Row{Value(dataset.story_count), Value(total_views)}}));
  ExpectNoPins(engine);

  CHECK_EQ(QueryText(engine,
                     "SELECT id, views FROM stories ORDER BY views DESC, id"),
           ExpectedStoriesByViews(dataset));
  ExpectNoPins(engine);

  // LIMIT stops pulling: the five rows are the first five of the chain.
  std::vector<std::string> first_ids;
  const std::vector<Row> chain = HeapRows(engine, "stories");
  for (std::size_t i = 0; i < 5; ++i) {
    first_ids.push_back(domain::FormatValue(chain[i][0]));
  }
  CHECK_EQ(QueryText(engine, "SELECT id FROM stories LIMIT 5"), first_ids);
  ExpectNoPins(engine);

  // A table the engine does not have, and an evaluation error in the middle
  // of a stream.
  domain::PlanNode ghost;
  ghost.node = domain::ScanNode{"ghost"};
  ExpectErrorMessage(engine.Execute(ghost), ErrorKind::kBind,
                     "unknown table: ghost");
  ExpectNoPins(engine);
  ExpectErrorMessage(Run(engine, "SELECT 1 / (views - views) FROM stories"),
                     ErrorKind::kRuntime, "division by zero");
  ExpectNoPins(engine);
}

namespace {

std::string NoteInsertInto(const std::string& table, std::int64_t first_id,
                           std::int64_t last_id, std::size_t body_size) {
  std::string sql = "INSERT INTO " + table + " VALUES ";
  for (std::int64_t id = first_id; id <= last_id; ++id) {
    if (id > first_id) {
      sql += ", ";
    }
    sql +=
        "(" + std::to_string(id) + ", '" + std::string(body_size, 'n') + "')";
  }
  return sql;
}

std::string NoteInsert(std::int64_t first_id, std::int64_t last_id,
                       std::size_t body_size) {
  return NoteInsertInto("notes", first_id, last_id, body_size);
}

std::vector<std::string> IdsOf(PagedEngine& engine, const std::string& table) {
  return QueryText(engine, "SELECT id FROM " + table + " ORDER BY id");
}

std::vector<std::string> IdRange(std::int64_t first_id, std::int64_t last_id) {
  std::vector<std::string> ids;
  for (std::int64_t id = first_id; id <= last_id; ++id) {
    ids.push_back(std::to_string(id));
  }
  return ids;
}

}  // namespace

TEST(test_paged_engine_writes_are_visible_and_reuse_slots) {
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(Run(engine,
                     "CREATE TABLE notes (id INT PRIMARY KEY, body "
                     "TEXT)"),
                 "CREATE TABLE notes");

  // 20 rows of 400 bytes: about 9 rows fit in a page.
  ExpectAffected(Run(engine, NoteInsert(1, 20, 400)), 20);
  CHECK_EQ(IdsOf(engine, "notes"), IdRange(1, 20));
  ExpectNoPins(engine);
  const PageId pages_after_load = engine.database().buffer().PageCount();
  CHECK(pages_after_load >= 3 + 2);

  ExpectAffected(Run(engine, "DELETE FROM notes WHERE id <= 5"), 5);
  CHECK_EQ(IdsOf(engine, "notes"), IdRange(6, 20));
  ExpectNoPins(engine);
  CHECK_EQ(engine.database().buffer().PageCount(), pages_after_load);

  // The freed space is reused: no page is added.
  ExpectAffected(Run(engine, NoteInsert(101, 105, 400)), 5);
  std::vector<std::string> expected_ids = IdRange(6, 20);
  for (const std::string& id : IdRange(101, 105)) {
    expected_ids.push_back(id);
  }
  CHECK_EQ(IdsOf(engine, "notes"), expected_ids);
  CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM notes"),
           std::vector<std::string>{"20"});
  CHECK_EQ(engine.database().buffer().PageCount(), pages_after_load);
  ExpectNoPins(engine);
}

namespace {

// The row with the given id, with its Rid; reports a failure when the table
// has no such row.
paged::ScanItem ItemOfId(PagedEngine& engine, const std::string& table,
                         std::int64_t id) {
  paged::PageResult<paged::TableHeap*> heap = engine.database().Table(table);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return paged::ScanItem{Rid{0, 0}, Row{}};
  }
  paged::HeapScan scan = heap.value()->Scan();
  while (true) {
    paged::PageResult<std::optional<paged::ScanItem>> next = scan.Next();
    if (!next.ok() || !next.value().has_value()) {
      break;
    }
    if (std::get<std::int64_t>(next.value()->row[0]) == id) {
      return std::move(*next.value());
    }
  }
  ReportFailure(__FILE__, __LINE__, "no row with id " + std::to_string(id));
  return paged::ScanItem{Rid{0, 0}, Row{}};
}

Rid RidOfId(PagedEngine& engine, const std::string& table, std::int64_t id) {
  return ItemOfId(engine, table, id).rid;
}

std::int64_t CountWhere(PagedEngine& engine, const std::string& sql) {
  const std::vector<Row> rows = RowsOf(Run(engine, sql));
  if (rows.size() != 1) {
    return -1;
  }
  return std::get<std::int64_t>(rows[0][0]);
}

}  // namespace

TEST(test_paged_engine_update_relocates_rows_and_cascades) {
  {
    // One page holds nine 400-byte rows with 338 bytes to spare. Growing a
    // row to 1000 bytes cannot be done in place, so the engine deletes the
    // row and inserts it again.
    StoreEngine fixture = NewEngine(kMinimumFrames);
    PagedEngine& engine = *fixture.engine;
    ExpectComplete(Run(engine,
                       "CREATE TABLE notes (id INT PRIMARY KEY, body "
                       "TEXT)"),
                   "CREATE TABLE notes");
    ExpectAffected(Run(engine, NoteInsert(1, 9, 400)), 9);
    const Rid before = RidOfId(engine, "notes", 3);

    ExpectAffected(Run(engine, "UPDATE notes SET body = '" +
                                   std::string(1000, 'g') + "' WHERE id = 3"),
                   1);
    CHECK(RidOfId(engine, "notes", 3) != before);  // it did move
    CHECK_EQ(IdsOf(engine, "notes"), IdRange(1, 9));
    CHECK_EQ(QueryText(engine, "SELECT LENGTH(body) FROM notes WHERE id = 3"),
             std::vector<std::string>{"1000"});
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM notes WHERE "
                        "LENGTH(body) = 400"),
             std::int64_t{8});
    ExpectNoPins(engine);
  }
  {
    // A cascade through two child tables, in pages that are nearly full: the
    // renamed visitor name is longer than the old one.
    const Dataset dataset{6, 90, 90, 60};
    StoreEngine fixture = NewEngine(kMinimumFrames);
    PagedEngine& engine = *fixture.engine;
    LoadDataset(engine, dataset);
    const std::vector<std::string> before_ids_visitors =
        IdsOf(engine, "visitors");
    const std::vector<std::string> before_ids_stories =
        IdsOf(engine, "stories");
    const std::vector<std::string> before_ids_reactions =
        IdsOf(engine, "reactions");
    const std::int64_t stories_of_u1 = CountWhere(
        engine, "SELECT COUNT(*) FROM stories WHERE username = 'u1'");
    const std::int64_t reactions_of_u1 = CountWhere(
        engine, "SELECT COUNT(*) FROM reactions WHERE username = 'u1'");
    CHECK(stories_of_u1 > 0);
    CHECK(reactions_of_u1 > 0);

    ExpectAffected(
        Run(engine,
            "UPDATE visitors SET username = 'a_much_longer_name' WHERE id = 1"),
        1);

    // Rows are neither lost nor duplicated, and every reference follows the
    // renamed key.
    CHECK_EQ(IdsOf(engine, "visitors"), before_ids_visitors);
    CHECK_EQ(IdsOf(engine, "stories"), before_ids_stories);
    CHECK_EQ(IdsOf(engine, "reactions"), before_ids_reactions);
    const std::string renamed = "'a_much_longer_name'";
    CHECK_EQ(
        CountWhere(engine,
                   "SELECT COUNT(*) FROM stories WHERE username = " + renamed),
        stories_of_u1);
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM reactions WHERE "
                        "username = " +
                            renamed),
             reactions_of_u1);
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM stories WHERE username = 'u1'"),
             std::int64_t{0});
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM reactions WHERE "
                        "username = 'u1'"),
             std::int64_t{0});
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM stories s JOIN visitors v ON "
                        "s.username = v.username"),
             dataset.story_count);
    CHECK_EQ(CountWhere(engine,
                        "SELECT COUNT(*) FROM reactions r JOIN visitors v ON "
                        "r.username = v.username"),
             dataset.reaction_count);
    CHECK_EQ(QueryText(engine, "SELECT content FROM stories WHERE id = 6"),
             TextOf({Row{dataset.Story(6)[1]}}));
    ExpectNoPins(engine);
  }
}

namespace {

// Runs a statement that must be rejected and checks that no page changes: the
// pages are the same, and the second FlushAll writes nothing.
void ExpectRejectedWithoutPageChange(StoreEngine& fixture,
                                     const std::string& sql, ErrorKind kind) {
  PagedEngine& engine = *fixture.engine;
  CHECK(!engine.Flush().has_value());
  const PageImages before_pages = PagesOf(*fixture.store);
  const std::uint64_t writes_before =
      engine.database().buffer().Stats().page_writes;

  ExecutionResult result = Run(engine, sql);
  ExpectError(result, kind);

  CHECK(!engine.Flush().has_value());
  CHECK(PagesOf(*fixture.store) == before_pages);
  CHECK_EQ(engine.database().buffer().Stats().page_writes, writes_before);
  ExpectNoPins(engine);
}

}  // namespace

TEST(test_paged_engine_rejected_statement_changes_no_page) {
  const Dataset dataset{10, 30, 40, 20};
  StoreEngine fixture = NewEngine(8);
  LoadDataset(*fixture.engine, dataset);

  ExpectRejectedWithoutPageChange(
      fixture, "INSERT INTO visitors VALUES (1, 'other', 'x')",
      ErrorKind::kConstraintViolation);
  ExpectRejectedWithoutPageChange(
      fixture, "INSERT INTO stories VALUES (1000, 'c', 'nobody', 1)",
      ErrorKind::kConstraintViolation);
  ExpectRejectedWithoutPageChange(fixture, "DELETE FROM visitors WHERE id = 1",
                                  ErrorKind::kConstraintViolation);
  ExpectRejectedWithoutPageChange(
      fixture, "UPDATE stories SET views = 1 / (views - views)",
      ErrorKind::kRuntime);
  ExpectRejectedWithoutPageChange(
      fixture,
      "INSERT INTO visitors VALUES (500, 'long', '" + std::string(4100, 'x') +
          "')",
      ErrorKind::kUnsupported);

  // The messages are those of design document 2.5.
  PagedEngine& engine = *fixture.engine;
  ExpectErrorMessage(
      Run(engine, "INSERT INTO visitors VALUES (1, 'other', 'x')"),
      ErrorKind::kConstraintViolation, "duplicate key: visitors.id=1");
  ExpectErrorMessage(Run(engine, "DELETE FROM visitors WHERE id = 1"),
                     ErrorKind::kConstraintViolation,
                     "visitors.id=1 is referenced by stories.username");
}

namespace {

// ----------------------------------------------------------------------------
// Fault injection.
// ----------------------------------------------------------------------------

constexpr std::size_t kOperationCount = 4;

struct Fault {
  Operation operation;
  int nth_call;
};

// One statement run on a fresh copy of a loaded database with faults armed.
struct FaultRun {
  StoreEngine fixture;
  ExecutionResult result = ExecutionError{ErrorKind::kIo, "not run"};
  // How many calls of each Operation (by enum value) the statement made.
  std::array<int, kOperationCount> calls{};
};

StoreEngine OpenCopy(const MemoryPageStore& loaded) {
  return OpenStoreEngine(CopyOf(loaded), kMinimumFrames);
}

// Opens a copy of `loaded` with a pool of 4 frames, arms the faults, runs sql,
// and disarms the faults, so that the caller can read the state afterwards.
FaultRun RunWithFaults(const MemoryPageStore& loaded, const std::string& sql,
                       const std::vector<Fault>& faults) {
  FaultRun run;
  run.fixture = OpenCopy(loaded);
  if (run.fixture.engine == nullptr) {
    return run;
  }
  MemoryPageStore& store = *run.fixture.store;
  std::array<int, kOperationCount> calls_before{};
  for (std::size_t i = 0; i < kOperationCount; ++i) {
    calls_before[i] = store.CallCount(static_cast<Operation>(i));
  }
  for (const Fault& fault : faults) {
    store.InjectFailure(fault.operation, fault.nth_call);
  }
  run.result = Run(*run.fixture.engine, sql);
  store.ClearInjectedFailures();
  for (std::size_t i = 0; i < kOperationCount; ++i) {
    run.calls[i] = store.CallCount(static_cast<Operation>(i)) - calls_before[i];
  }
  return run;
}

int CallsOf(const FaultRun& run, Operation operation) {
  return run.calls[static_cast<std::size_t>(operation)];
}

// A loaded and flushed database that fault runs copy. The engine is kept so
// that the store stays alive.
StoreEngine LoadedTemplate(const Dataset& dataset) {
  StoreEngine loaded = NewEngine(64);
  LoadDataset(*loaded.engine, dataset);
  CHECK(!loaded.engine->Flush().has_value());
  return loaded;
}

std::string ReactionInsert(std::int64_t first_id, std::int64_t count,
                           std::int64_t story_count, std::size_t padding) {
  std::string sql = "INSERT INTO reactions VALUES ";
  for (std::int64_t i = 0; i < count; ++i) {
    if (i > 0) {
      sql += ", ";
    }
    sql += "(" + std::to_string(first_id + i) + ", " +
           std::to_string(1 + i % story_count) + ", 'u3', 'like', '" +
           std::string(padding, 'm') + "')";
  }
  return sql;
}

constexpr const char* kCascadeUpdate =
    "UPDATE visitors SET username = 'renamed_user' WHERE id = 1";
constexpr int kMaxFaultPositions = 400;

struct FailureCounts {
  int apply_failures = 0;
  int write_back_failures = 0;
};

// CheckIndex on every table of the engine: the tree is valid and agrees with
// its heap.
void CheckEveryIndex(PagedEngine& engine) {
  for (const std::string& table : engine.Catalog().TableNames()) {
    CHECK_OK(engine.database().CheckIndex(table));
  }
}

// Fails the n-th WritePage of sql for n = 1, 2, ... until the statement no
// longer fails, and checks the state after each run.
void CheckEveryWritePosition(const MemoryPageStore& loaded,
                             const std::string& sql, FailureCounts* counts) {
  FaultRun clean = RunWithFaults(loaded, sql, {});
  CHECK(AsAffectedRows(clean.result) != nullptr);
  if (clean.fixture.engine == nullptr) {
    return;
  }
  const std::vector<std::string> after = SnapshotOf(*clean.fixture.engine);
  StoreEngine untouched = OpenCopy(loaded);
  const std::vector<std::string> before = SnapshotOf(*untouched.engine);
  CHECK(before != after);

  for (int nth_write = 1; nth_write <= kMaxFaultPositions; ++nth_write) {
    FaultRun run = RunWithFaults(loaded, sql, {{Operation::kWrite, nth_write}});
    if (run.fixture.engine == nullptr) {
      return;
    }
    PagedEngine& engine = *run.fixture.engine;
    ExpectNoPins(engine);
    CheckEveryIndex(engine);
    if (CallsOf(run, Operation::kWrite) < nth_write) {
      // The statement made fewer writes: nothing failed, and it succeeded.
      CHECK(AsAffectedRows(run.result) != nullptr);
      CHECK(SnapshotOf(engine) == after);
      return;
    }
    const ExecutionError* error = ExpectError(run.result, ErrorKind::kIo);
    if (error == nullptr) {
      continue;
    }
    CHECK(Contains(error->message, "injected write failure"));
    CHECK(!Contains(error->message, "rollback failed"));
    if (error->message.ends_with(kSavedSuffix)) {
      ++counts->write_back_failures;
      CHECK(SnapshotOf(engine) == after);
    } else {
      ++counts->apply_failures;
      CHECK(SnapshotOf(engine) == before);
    }
    ExpectNoPins(engine);
  }
  ReportFailure(__FILE__, __LINE__, "the statement kept failing: " + sql);
}

// ----------------------------------------------------------------------------
// Hand-made change sets: the undo of ApplyChangeSet, with pages that are full
// to the byte, so that the order of the undo steps decides the outcome.
// ----------------------------------------------------------------------------

Row TextRow(std::int64_t id, std::size_t body_size) {
  return Row{Value(id), Value(std::string(body_size, 'x'))};
}

Row WideRow(std::int64_t id, std::size_t body_size, std::size_t alt_size) {
  return Row{Value(id), Value(std::string(body_size, 'x')),
             Value(std::string(alt_size, 'y'))};
}

paged::TableChange UpdateChange(const std::string& table,
                                const paged::ScanItem& item, Row new_row) {
  paged::TableChange change;
  change.table = table;
  change.updates.push_back(paged::RowUpdate{item.rid, item.row, new_row});
  return change;
}

paged::TableChange DeleteChange(const std::string& table,
                                const paged::ScanItem& item) {
  paged::TableChange change;
  change.table = table;
  change.deletes.push_back(paged::RowDelete{item.rid, item.row});
  return change;
}

// A change for a table the database does not have: applying it fails, after
// the changes before it were applied.
paged::TableChange GhostChange() {
  paged::TableChange change;
  change.table = "ghost";
  change.inserts.push_back(TextRow(1, 1));
  return change;
}

paged::MutationPlan PlanOf(std::vector<paged::TableChange> changes) {
  paged::MutationPlan plan;
  plan.changes = std::move(changes);
  plan.affected = 0;
  return plan;
}

// The free bytes the heap's map records for the first page of a table.
std::uint16_t FirstPageCapacity(PagedEngine& engine, const std::string& table) {
  paged::PageResult<paged::TableHeap*> heap = engine.database().Table(table);
  CHECK_OK(heap);
  if (!heap.ok()) {
    return 0;
  }
  for (const paged::PageCapacity& entry : heap.value()->FreeSpaceSnapshot()) {
    if (entry.page_id == heap.value()->first_page()) {
      return entry.capacity;
    }
  }
  return 0;
}

// Applies `changes` and the failing GhostChange after them, and checks that
// the undo restored the table: the error is the ghost's, without a rollback
// failure, and the rows are the rows of `before`.
void ExpectUndoRestores(StoreEngine& fixture, const std::string& table,
                        std::vector<paged::TableChange> changes,
                        const std::vector<std::string>& before) {
  PagedEngine& engine = *fixture.engine;
  changes.push_back(GhostChange());
  const std::optional<ExecutionError> error =
      paged::ApplyChangeSet(engine.database(), PlanOf(std::move(changes)));
  CHECK(error.has_value());
  if (error.has_value()) {
    CHECK_EQ(error->message, std::string("unknown table: ghost"));
    CHECK(error->kind == ErrorKind::kBind);
  }
  CHECK_EQ(SortedQueryText(engine, "SELECT * FROM " + table), before);
  ExpectNoPins(engine);
}

// Two rows swap their sizes in a page with no free byte: the first shrinks by
// 800 bytes, and the second grows into them. The undo must grow the first row
// back only after the second shrank, or the first row no longer fits. The
// store cannot allocate a page, so a wrong order cannot recover by moving it.
void CheckUndoOrder() {
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(Run(engine,
                     "CREATE TABLE swap (id INT PRIMARY KEY, body "
                     "TEXT, alt TEXT)"),
                 "CREATE TABLE swap");
  ExpectAffected(RunInsert(engine, "swap",
                           {WideRow(1, 900, 100), WideRow(2, 100, 900),
                            WideRow(3, 1008, 1008)}),
                 3);
  CHECK(!engine.Flush().has_value());
  CHECK_EQ(FirstPageCapacity(engine, "swap"), std::uint16_t{0});
  const std::vector<std::string> before =
      SortedQueryText(engine, "SELECT * FROM swap");
  const paged::ScanItem first = ItemOfId(engine, "swap", 1);
  const paged::ScanItem second = ItemOfId(engine, "swap", 2);
  const Row shrunk = WideRow(1, 100, 100);
  const Row grown = WideRow(2, 900, 900);

  // Without the failing change both updates are applied in place.
  {
    StoreEngine control = OpenCopy(*fixture.store);
    CHECK(!paged::ApplyChangeSet(control.engine->database(),
                                 PlanOf({UpdateChange("swap", first, shrunk),
                                         UpdateChange("swap", second, grown)}))
               .has_value());
    CHECK(ItemOfId(*control.engine, "swap", 1).rid == first.rid);
    CHECK(ItemOfId(*control.engine, "swap", 2).rid == second.rid);
    CHECK(ItemOfId(*control.engine, "swap", 2).row == grown);
  }

  fixture.store->InjectFailure(Operation::kAllocate, 1);
  ExpectUndoRestores(fixture, "swap",
                     {UpdateChange("swap", first, shrunk),
                      UpdateChange("swap", second, grown)},
                     before);
  fixture.store->ClearInjectedFailures();
}

// The undo of an update that no longer fits: a delete in another page is
// undone first and takes the space that the shrinking update freed, so the
// update is undone as a delete followed by an insert.
void CheckUndoUpdateFallsBackToDeleteAndInsert() {
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(
      Run(engine, "CREATE TABLE fall (id INT PRIMARY KEY, body TEXT)"),
      "CREATE TABLE fall");
  // Page one is full with the first two rows; the third lands in a new page.
  ExpectAffected(
      RunInsert(engine, "fall",
                {TextRow(1, 800), TextRow(2, 3236), TextRow(3, 590)}),
      3);
  CHECK(!engine.Flush().has_value());
  CHECK_EQ(FirstPageCapacity(engine, "fall"), std::uint16_t{0});
  const std::vector<std::string> before =
      SortedQueryText(engine, "SELECT * FROM fall");
  const paged::ScanItem shrinking = ItemOfId(engine, "fall", 1);
  const paged::ScanItem deleted = ItemOfId(engine, "fall", 3);
  CHECK(deleted.rid.page_id != shrinking.rid.page_id);
  const Row shrunk = TextRow(1, 10);

  {
    StoreEngine control = OpenCopy(*fixture.store);
    CHECK(
        !paged::ApplyChangeSet(control.engine->database(),
                               PlanOf({UpdateChange("fall", shrinking, shrunk),
                                       DeleteChange("fall", deleted)}))
             .has_value());
    CHECK_EQ(SortedQueryText(*control.engine, "SELECT id FROM fall"),
             (std::vector<std::string>{"1", "2"}));
  }

  ExpectUndoRestores(
      fixture, "fall",
      {UpdateChange("fall", shrinking, shrunk), DeleteChange("fall", deleted)},
      before);
}

// An update that moves a row (delete, then insert in another page), followed
// by a failure: the undo deletes the moved copy and brings the old row back.
void CheckUndoMovedUpdate() {
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(
      Run(engine, "CREATE TABLE move (id INT PRIMARY KEY, body TEXT)"),
      "CREATE TABLE move");
  ExpectAffected(
      RunInsert(engine, "move", {TextRow(1, 1000), TextRow(2, 3036)}), 2);
  CHECK(!engine.Flush().has_value());
  CHECK_EQ(FirstPageCapacity(engine, "move"), std::uint16_t{0});
  const std::vector<std::string> before =
      SortedQueryText(engine, "SELECT * FROM move");
  const paged::ScanItem moving = ItemOfId(engine, "move", 1);
  const Row grown = TextRow(1, 1500);

  {
    StoreEngine control = OpenCopy(*fixture.store);
    CHECK(!paged::ApplyChangeSet(control.engine->database(),
                                 PlanOf({UpdateChange("move", moving, grown)}))
               .has_value());
    CHECK(ItemOfId(*control.engine, "move", 1).rid.page_id !=
          moving.rid.page_id);
    CHECK_EQ(CountWhere(*control.engine, "SELECT COUNT(*) FROM move"),
             std::int64_t{2});
  }

  ExpectUndoRestores(fixture, "move", {UpdateChange("move", moving, grown)},
                     before);
}

}  // namespace

TEST(test_paged_engine_apply_failure_rolls_back_at_every_write_position) {
  const Dataset dataset{4, 200, 200, 100};
  StoreEngine loaded = LoadedTemplate(dataset);
  CHECK(loaded.store->PageCount() > 12);

  FailureCounts cascade_counts;
  CheckEveryWritePosition(*loaded.store, kCascadeUpdate, &cascade_counts);
  CHECK(cascade_counts.apply_failures > 0);
  CHECK(cascade_counts.write_back_failures > 0);

  FailureCounts insert_counts;
  CheckEveryWritePosition(
      *loaded.store,
      ReactionInsert(1000, 300, dataset.story_count, dataset.padding),
      &insert_counts);
  CHECK(insert_counts.apply_failures > 0);
  CHECK(insert_counts.write_back_failures > 0);

  FailureCounts delete_counts;
  CheckEveryWritePosition(
      *loaded.store, "DELETE FROM reactions WHERE id <= 150", &delete_counts);
  CHECK(delete_counts.apply_failures > 0);
  CHECK(delete_counts.write_back_failures > 0);

  CheckUndoOrder();
  CheckUndoUpdateFallsBackToDeleteAndInsert();
  CheckUndoMovedUpdate();

  // The relocation: the update does not fit in the page, so the row is
  // deleted and inserted again, and the allocation of the page for the insert
  // fails. The delete has succeeded, and the row must come back.
  StoreEngine fixture = NewEngine(kMinimumFrames);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(
      Run(engine, "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT)"),
      "CREATE TABLE notes");
  ExpectAffected(Run(engine, NoteInsert(1, 9, 400)), 9);
  CHECK(!engine.Flush().has_value());
  const std::vector<std::string> before =
      SortedQueryText(engine, "SELECT * FROM notes");
  const PageId pages_before = fixture.store->PageCount();
  const int writes_before = fixture.store->CallCount(Operation::kWrite);

  fixture.store->InjectFailure(Operation::kAllocate, 1);
  ExecutionResult result =
      Run(engine, "UPDATE notes SET body = '" + std::string(1000, 'g') +
                      "' WHERE id = 3");
  fixture.store->ClearInjectedFailures();
  const ExecutionError* error = ExpectError(result, ErrorKind::kIo);
  if (error != nullptr) {
    CHECK(Contains(error->message, "injected allocate failure"));
    CHECK(!Contains(error->message, "rollback failed"));
    CHECK(!error->message.ends_with(kSavedSuffix));
  }
  CHECK_EQ(fixture.store->PageCount(), pages_before);
  // A failed statement writes no page, although its pages are dirty.
  CHECK_EQ(fixture.store->CallCount(Operation::kWrite), writes_before);
  CHECK_EQ(SortedQueryText(engine, "SELECT * FROM notes"), before);
  CHECK_EQ(IdsOf(engine, "notes"), IdRange(1, 9));
  ExpectNoPins(engine);
}

namespace {

constexpr int kMaxReadPositions = 400;

// The number of lines that are in exactly one of the two snapshots; a row that
// differs counts twice (its old and its new line).
std::size_t DifferingLines(std::vector<std::string> left,
                           std::vector<std::string> right) {
  std::ranges::sort(left);
  std::ranges::sort(right);
  std::vector<std::string> difference;
  std::set_symmetric_difference(left.begin(), left.end(), right.begin(),
                                right.end(), std::back_inserter(difference));
  return difference.size();
}

// How a run of the rollback-failure test ended.
struct RollbackCounts {
  int rollback_failures = 0;
  int combinations = 0;
};

void CheckCombination(const MemoryPageStore& loaded, const std::string& sql,
                      int nth_write, int nth_read,
                      const std::vector<std::string>& before,
                      const std::vector<std::string>& after,
                      RollbackCounts* counts, bool* is_read_fired) {
  FaultRun run = RunWithFaults(
      loaded, sql,
      {{Operation::kWrite, nth_write}, {Operation::kRead, nth_read}});
  if (run.fixture.engine == nullptr) {
    return;
  }
  PagedEngine& engine = *run.fixture.engine;
  ExpectNoPins(engine);
  *is_read_fired = CallsOf(run, Operation::kRead) >= nth_read;
  if (!*is_read_fired) {
    return;
  }
  ++counts->combinations;
  // A failed read fails the statement, in the plan, the apply or the undo.
  const ExecutionError* error = ExpectError(run.result, ErrorKind::kIo);
  if (error == nullptr) {
    return;
  }
  CHECK(error->message.starts_with("injected "));
  if (Contains(error->message, "; rollback failed: ")) {
    ++counts->rollback_failures;
    CHECK(Contains(error->message, "; rollback failed: injected "));
    // One failed step does not stop the others: at most the one row of the
    // failed step differs from the state before the statement.
    CHECK(DifferingLines(SnapshotOf(engine), before) <= 2);
    ExpectNoPins(engine);
    return;
  }
  // The undo worked, so the state is the one before the statement, unless the
  // failure was the write-back, which leaves the statement applied.
  if (error->message.ends_with(kSavedSuffix)) {
    CHECK(SnapshotOf(engine) == after);
  } else {
    CHECK(SnapshotOf(engine) == before);
  }
  ExpectNoPins(engine);
}

}  // namespace

TEST(test_paged_engine_rollback_failure_is_reported) {
  const Dataset dataset{4, 60, 60, 100};
  StoreEngine loaded = LoadedTemplate(dataset);
  const std::string sql = kCascadeUpdate;

  FaultRun clean = RunWithFaults(*loaded.store, sql, {});
  CHECK(AsAffectedRows(clean.result) != nullptr);
  if (clean.fixture.engine == nullptr) {
    return;
  }
  const std::vector<std::string> after = SnapshotOf(*clean.fixture.engine);
  StoreEngine untouched = OpenCopy(*loaded.store);
  const std::vector<std::string> before = SnapshotOf(*untouched.engine);
  const int clean_writes = CallsOf(clean, Operation::kWrite);
  CHECK(clean_writes > 2);

  // Every position of a failed write combined with every position of a failed
  // read. The read that fails lands in the plan, in the apply or in the undo.
  RollbackCounts counts;
  for (int nth_write = 1; nth_write <= clean_writes; ++nth_write) {
    for (int nth_read = 1; nth_read <= kMaxReadPositions; ++nth_read) {
      bool is_read_fired = false;
      CheckCombination(*loaded.store, sql, nth_write, nth_read, before, after,
                       &counts, &is_read_fired);
      if (!is_read_fired) {
        break;
      }
    }
  }
  CHECK(counts.combinations > 20);
  CHECK(counts.rollback_failures > 0);
}

namespace {

// Creates alpha (30 rows) and beta (100 rows) of 400-byte bodies and flushes.
StoreEngine TwoTableTemplate() {
  StoreEngine loaded = NewEngine(64);
  PagedEngine& engine = *loaded.engine;
  ExpectComplete(
      Run(engine, "CREATE TABLE alpha (id INT PRIMARY KEY, body TEXT)"),
      "CREATE TABLE alpha");
  ExpectAffected(Run(engine, NoteInsertInto("alpha", 1, 30, 400)), 30);
  ExpectComplete(
      Run(engine, "CREATE TABLE beta (id INT PRIMARY KEY, body TEXT)"),
      "CREATE TABLE beta");
  ExpectAffected(Run(engine, NoteInsertInto("beta", 1, 100, 400)), 100);
  CHECK(!engine.Flush().has_value());
  return loaded;
}

void FlipByteOfFirstPage(StoreEngine& fixture, const std::string& table) {
  std::optional<PageId> first_page =
      fixture.engine->database().catalog().FirstPage(table);
  CHECK(first_page.has_value());
  if (!first_page.has_value()) {
    return;
  }
  fixture.store->MutablePage(*first_page)[100] ^= 0xFF;
}

}  // namespace

TEST(test_paged_engine_storage_errors_map_to_io) {
  StoreEngine loaded = TwoTableTemplate();
  CHECK(loaded.store->PageCount() > 12);

  // A corrupt page: the table that owns it fails when it is opened, and the
  // other table is not affected. No frame holds the good page yet.
  StoreEngine reopened = OpenCopy(*loaded.store);
  FlipByteOfFirstPage(reopened, "alpha");
  PagedEngine& engine = *reopened.engine;

  // The row counts of `.open` fail as a whole when one table is corrupt.
  const paged::RowCountResult counted = engine.RowCounts();
  const auto* count_error = std::get_if<ExecutionError>(&counted);
  CHECK(count_error != nullptr);
  if (count_error != nullptr) {
    CHECK(count_error->kind == ErrorKind::kIo);
    CHECK(count_error->message.starts_with("corrupt: "));
  }
  ExpectNoPins(engine);

  // One failed read, on the first read of the first table that is opened.
  reopened.store->InjectFailure(Operation::kRead, 1);
  const ExecutionResult read_failure = Run(engine, "SELECT * FROM beta");
  reopened.store->ClearInjectedFailures();
  const ExecutionError* failure = ExpectError(read_failure, ErrorKind::kIo);
  if (failure != nullptr) {
    CHECK(Contains(failure->message, "injected read failure"));
  }
  ExpectNoPins(engine);

  const ExecutionResult corrupt_result = Run(engine, "SELECT * FROM alpha");
  const ExecutionError* corrupt_error =
      ExpectError(corrupt_result, ErrorKind::kIo);
  if (corrupt_error != nullptr) {
    CHECK(corrupt_error->message.starts_with("corrupt: "));
  }
  ExpectNoPins(engine);

  // The other table works, also after the corrupt one failed twice.
  CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM beta"),
           std::vector<std::string>{"100"});
  const ExecutionResult corrupt_again = Run(engine, "SELECT * FROM alpha");
  const ExecutionError* repeated = ExpectError(corrupt_again, ErrorKind::kIo);
  if (repeated != nullptr) {
    CHECK(repeated->message.starts_with("corrupt: "));
  }
  ExpectNoPins(engine);

  // A failure in the middle of a scan, with the heap already open: the stream
  // reports it, and the next statement works.
  StoreEngine scanning = OpenCopy(*loaded.store);
  CHECK_EQ(QueryText(*scanning.engine, "SELECT COUNT(*) FROM beta"),
           std::vector<std::string>{"100"});
  scanning.store->InjectFailure(Operation::kRead, 2);
  const ExecutionResult mid_scan = Run(*scanning.engine, "SELECT * FROM beta");
  scanning.store->ClearInjectedFailures();
  const ExecutionError* mid_scan_error = ExpectError(mid_scan, ErrorKind::kIo);
  if (mid_scan_error != nullptr) {
    CHECK(Contains(mid_scan_error->message, "injected read failure"));
  }
  ExpectNoPins(*scanning.engine);
  CHECK_EQ(QueryText(*scanning.engine, "SELECT COUNT(*) FROM beta"),
           std::vector<std::string>{"100"});

  // The stream under the operators: columns, case-insensitive names, and the
  // end of the rows after an error.
  StoreEngine streaming = OpenCopy(*loaded.store);
  const paged::DatabaseTableReader reader(streaming.engine->database());
  paged::ScanResult missing = reader.Scan("ghost");
  const auto* missing_error = std::get_if<ExecutionError>(&missing);
  CHECK(missing_error != nullptr);
  if (missing_error != nullptr) {
    CHECK(missing_error->kind == ErrorKind::kBind);
    CHECK_EQ(missing_error->message, std::string("unknown table: ghost"));
  }
  paged::ScanResult opened_scan = reader.Scan("BETA");
  auto* stream = std::get_if<std::unique_ptr<paged::IRowStream>>(&opened_scan);
  CHECK(stream != nullptr);
  if (stream != nullptr) {
    const std::vector<domain::ColumnDef> columns = (*stream)->Columns();
    CHECK_EQ(columns.size(), std::size_t{2});
    if (columns.size() == 2) {
      CHECK_EQ(columns[1].name, std::string("body"));
    }
    std::size_t streamed = 0;
    bool has_error = false;
    int guard = 0;
    streaming.store->InjectFailure(Operation::kRead, 3);
    while (guard++ < 1000) {
      paged::StreamResult next = (*stream)->Next();
      if (std::holds_alternative<ExecutionError>(next)) {
        has_error = true;
        break;
      }
      if (!std::get<std::optional<paged::StoredRow>>(next).has_value()) {
        break;
      }
      ++streamed;
    }
    streaming.store->ClearInjectedFailures();
    CHECK(has_error);
    CHECK(streamed > 0 && streamed < 100);
    // After the error the stream is over: no value, and no second error.
    for (int call = 0; call < 3; ++call) {
      paged::StreamResult after_error = (*stream)->Next();
      const auto* row =
          std::get_if<std::optional<paged::StoredRow>>(&after_error);
      CHECK(row != nullptr && !row->has_value());
    }
  }
  stream = nullptr;
  opened_scan = paged::ScanResult(std::unique_ptr<paged::IRowStream>());
  ExpectNoPins(*streaming.engine);

  // A page that goes bad after the heap was opened and left the pool: the
  // stream reports corruption.
  StoreEngine decaying = OpenCopy(*loaded.store);
  CHECK_EQ(QueryText(*decaying.engine, "SELECT COUNT(*) FROM beta"),
           std::vector<std::string>{"100"});
  FlipByteOfFirstPage(decaying, "beta");
  const ExecutionResult decayed = Run(*decaying.engine, "SELECT * FROM beta");
  const ExecutionError* decayed_error = ExpectError(decayed, ErrorKind::kIo);
  if (decayed_error != nullptr) {
    CHECK(decayed_error->message.starts_with("corrupt: "));
  }
  ExpectNoPins(*decaying.engine);
  CHECK_EQ(QueryText(*decaying.engine, "SELECT COUNT(*) FROM alpha"),
           std::vector<std::string>{"30"});
}

namespace {

// ----------------------------------------------------------------------------
// A chain that points past the end of the file (design document 2.13, 2.15).
// ----------------------------------------------------------------------------

// A copy of the store of `loaded` in which the first page of `table` names a
// next page that the file does not have. The checksum is sealed again, so the
// page still passes its header check and only the chain is wrong.
std::unique_ptr<MemoryPageStore> WithChainPastTheEnd(StoreEngine& loaded,
                                                     const std::string& table) {
  std::unique_ptr<MemoryPageStore> bytes = CopyOf(*loaded.store);
  const std::optional<PageId> first_page =
      loaded.engine->database().catalog().FirstPage(table);
  CHECK(first_page.has_value());
  if (!first_page.has_value()) {
    return bytes;
  }
  std::uint8_t* page = bytes->MutablePage(*first_page);
  paged::WriteLe32(page + paged::layout::kNextPageId, bytes->PageCount());
  paged::SlottedPage::SealChecksum(page);
  return bytes;
}

// The error is damage to a table the catalog has: kIo with "corrupt: " from
// the error mapping of design document 2.13, never "unknown table".
void ExpectCorruptChain(const ExecutionError& error, const std::string& label) {
  CHECK_EQ(label + " " + domain::ErrorKindName(error.kind),
           label + " " + domain::ErrorKindName(ErrorKind::kIo));
  CHECK(error.message.starts_with("corrupt: "));
  CHECK(Contains(error.message, "leads past the end of the file"));
  CHECK(!Contains(error.message, "unknown table"));
}

void ExpectCorruptChain(const ExecutionResult& result,
                        const std::string& label) {
  const ExecutionError* error = ExpectError(result, ErrorKind::kIo);
  if (error != nullptr) {
    ExpectCorruptChain(*error, label);
  }
}

void ExpectCorruptChain(const std::optional<ExecutionError>& error,
                        const std::string& label) {
  CHECK(error.has_value());
  if (error.has_value()) {
    ExpectCorruptChain(*error, label);
  }
}

void ExpectUnknownTable(const std::optional<ExecutionError>& error,
                        const std::string& table, const std::string& label) {
  CHECK(error.has_value());
  if (!error.has_value()) {
    return;
  }
  CHECK_EQ(label + " " + domain::ErrorKindName(error->kind),
           label + " " + domain::ErrorKindName(ErrorKind::kBind));
  CHECK_EQ(error->message, "unknown table: " + table);
}

}  // namespace

TEST(test_paged_engine_truncated_chain_is_not_an_unknown_table) {
  StoreEngine loaded = TwoTableTemplate();
  CHECK(loaded.store->PageCount() > 12);
  StoreEngine reopened =
      OpenStoreEngine(WithChainPastTheEnd(loaded, "alpha"), kMinimumFrames);
  if (reopened.engine == nullptr) {
    return;
  }
  PagedEngine& engine = *reopened.engine;
  Database& database = engine.database();
  const paged::DatabaseTableReader reader(database);

  // The three call sites of Database::Table, for a table that the catalog has
  // but whose chain cannot be walked: a scan (a query and the reader), a
  // write (a statement, a change set and a row insert), and the row counts.
  ExpectCorruptChain(Run(engine, "SELECT * FROM alpha"), "SELECT");
  paged::ScanResult scanned = reader.Scan("alpha");
  const auto* scan_error = std::get_if<ExecutionError>(&scanned);
  CHECK(scan_error != nullptr);
  if (scan_error != nullptr) {
    ExpectCorruptChain(*scan_error, "Scan");
  }
  paged::ScanResult scanned_upper = reader.Scan("ALPHA");
  const auto* upper_error = std::get_if<ExecutionError>(&scanned_upper);
  CHECK(upper_error != nullptr);
  if (upper_error != nullptr) {
    ExpectCorruptChain(*upper_error, "ALPHA");
  }

  ExpectCorruptChain(Run(engine, "INSERT INTO alpha VALUES (1000, 'x')"),
                     "INSERT");
  ExpectCorruptChain(Run(engine, "DELETE FROM alpha WHERE id = 1"), "DELETE");
  paged::TableChange change;
  change.table = "alpha";
  change.inserts.push_back(TextRow(1000, 1));
  ExpectCorruptChain(paged::ApplyChangeSet(database, PlanOf({change})),
                     "ApplyChangeSet");
  ExpectCorruptChain(paged::InsertRows(database, "alpha", {TextRow(1000, 1)}),
                     "InsertRows");

  const paged::RowCountResult counted = engine.RowCounts();
  const auto* count_error = std::get_if<ExecutionError>(&counted);
  CHECK(count_error != nullptr);
  if (count_error != nullptr) {
    ExpectCorruptChain(*count_error, "RowCounts");
  }
  ExpectNoPins(engine);

  // The other table is not affected.
  CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM beta"),
           std::vector<std::string>{"100"});

  // A table that the catalog does not have is still an unknown table.
  paged::ScanResult missing = reader.Scan("ghost");
  const auto* missing_error = std::get_if<ExecutionError>(&missing);
  CHECK(missing_error != nullptr);
  if (missing_error != nullptr) {
    ExpectUnknownTable(*missing_error, "ghost", "Scan");
  }
  ExpectUnknownTable(paged::ApplyChangeSet(database, PlanOf({GhostChange()})),
                     "ghost", "ApplyChangeSet");
  ExpectUnknownTable(paged::InsertRows(database, "ghost", {TextRow(1, 1)}),
                     "ghost", "InsertRows");
  ExpectNoPins(engine);
}

namespace {

// ----------------------------------------------------------------------------
// DDL and import through a QueryService.
// ----------------------------------------------------------------------------

// A CSV source that serves the same lines whatever path it is asked for.
class FixedCsvSource : public domain::ICsvSource {
 public:
  explicit FixedCsvSource(std::vector<domain::CsvLine> lines)
      : lines_(std::move(lines)) {}

  std::variant<std::vector<domain::CsvLine>, std::string> Read(
      const std::string& path) override {
    static_cast<void>(path);
    return lines_;
  }

 private:
  std::vector<domain::CsvLine> lines_;
};

domain::CsvLine CsvLineOf(std::size_t number,
                          const std::vector<std::string>& fields) {
  return domain::CsvLine{number, fields};
}

// The header id,name,score and `row_count` rows: ids from 1, a 40-byte name
// and a score with a fraction, so that the columns infer as INT, TEXT, DOUBLE.
std::vector<domain::CsvLine> ImportLines(std::int64_t row_count,
                                         const std::string& long_name = "") {
  std::vector<domain::CsvLine> lines;
  lines.push_back(CsvLineOf(1, {"id", "name", "score"}));
  for (std::int64_t id = 1; id <= row_count; ++id) {
    const std::string name =
        (id == row_count && !long_name.empty())
            ? long_name
            : "name-" + std::to_string(id) + std::string(35, 'x');
    lines.push_back(
        CsvLineOf(static_cast<std::size_t>(id) + 1,
                  {std::to_string(id), name, std::to_string(id) + ".5"}));
  }
  return lines;
}

struct ServiceFixture {
  MemoryPageStore* store = nullptr;
  PagedEngine* engine = nullptr;  // borrowed from the service
  std::unique_ptr<QueryService> service;
};

ServiceFixture OpenService(std::size_t frames,
                           std::vector<domain::CsvLine> csv_lines = {}) {
  StoreEngine opened =
      OpenStoreEngine(std::make_unique<MemoryPageStore>(), frames);
  ServiceFixture fixture;
  if (opened.engine == nullptr) {
    return fixture;
  }
  fixture.store = opened.store;
  fixture.engine = opened.engine.get();
  fixture.service = std::make_unique<QueryService>(
      std::move(opened.engine),
      std::make_unique<FixedCsvSource>(std::move(csv_lines)));
  return fixture;
}

std::vector<std::string> ServiceQuery(QueryService& service,
                                      const std::string& sql) {
  ExecutionResult result = ExecuteSql(service, sql);
  return TextOf(RowsOf(result));
}

bool HasTable(QueryService& service, const std::string& name) {
  for (const domain::TableSchema& schema : service.DescribeCatalog()) {
    if (schema.name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(test_paged_engine_ddl_scope) {
  ServiceFixture fixture = OpenService(kMinimumFrames);
  QueryService& service = *fixture.service;
  ExpectComplete(
      ExecuteSql(service,
                 "CREATE TABLE parents (id INT PRIMARY KEY AUTOINCREMENT, "
                 "name TEXT UNIQUE, note TEXT DEFAULT 'none')"),
      "CREATE TABLE parents");
  ExpectComplete(
      ExecuteSql(service,
                 "CREATE TABLE children (id INT PRIMARY KEY, parent_name "
                 "TEXT REFERENCES parents(name), score DOUBLE DEFAULT 1.5)"),
      "CREATE TABLE children");

  // The catalog holds every constraint of both statements, in creation order.
  const std::vector<domain::TableSchema> schemas = service.DescribeCatalog();
  CHECK_EQ(schemas.size(), std::size_t{2});
  if (schemas.size() == 2) {
    const domain::TableSchema& parents = schemas[0];
    CHECK_EQ(parents.name, std::string("parents"));
    CHECK_EQ(parents.columns.size(), std::size_t{3});
    CHECK_EQ(parents.primary_key_index, std::size_t{0});
    CHECK(parents.auto_increment_index == std::optional<std::size_t>(0));
    CHECK(parents.unique_indexes == std::vector<std::size_t>{1});
    if (parents.columns.size() == 3) {
      CHECK(parents.columns[2].default_value ==
            std::optional<Value>(std::string("none")));
      CHECK(parents.columns[0].type == DataType::kInt);
    }
    const domain::TableSchema& children = schemas[1];
    CHECK_EQ(children.name, std::string("children"));
    CHECK_EQ(children.foreign_keys.size(), std::size_t{1});
    if (children.foreign_keys.size() == 1) {
      CHECK_EQ(children.foreign_keys[0].column_index, std::size_t{1});
      CHECK_EQ(children.foreign_keys[0].ref_table, std::string("parents"));
      CHECK_EQ(children.foreign_keys[0].ref_column_index, std::size_t{1});
    }
    if (children.columns.size() == 3) {
      CHECK(children.columns[2].type == DataType::kDouble);
      CHECK(children.columns[2].default_value == std::optional<Value>(1.5));
    }
  }

  // The constraints work on the stored data.
  ExpectAffected(ExecuteSql(service, "INSERT INTO parents (name) VALUES ('a')"),
                 1);
  CHECK_EQ(ServiceQuery(service, "SELECT id, name, note FROM parents"),
           std::vector<std::string>{"1\ta\tnone"});
  ExpectAffected(ExecuteSql(service,
                            "INSERT INTO children (id, parent_name) "
                            "VALUES (1, 'a')"),
                 1);
  CHECK_EQ(ServiceQuery(service, "SELECT score FROM children"),
           std::vector<std::string>{"1.5"});
  ExpectError(ExecuteSql(service,
                         "INSERT INTO children (id, parent_name) "
                         "VALUES (2, 'zzz')"),
              ErrorKind::kConstraintViolation);
  ExpectError(ExecuteSql(service, "INSERT INTO parents (name) VALUES ('a')"),
              ErrorKind::kConstraintViolation);

  // The refused statements change no page.
  CHECK(!fixture.engine->Flush().has_value());
  const PageImages before_pages = PagesOf(*fixture.store);
  ExpectError(ExecuteSql(service, "CREATE TABLE sys_x (id INT PRIMARY KEY)"),
              ErrorKind::kUnsupported);
  CHECK(!fixture.engine->Flush().has_value());
  CHECK(PagesOf(*fixture.store) == before_pages);
  CHECK(!HasTable(service, "sys_x"));
  CHECK(HasTable(service, "children"));
  ExpectNoPins(*fixture.engine);
}

namespace {

ExecutionResult ImportTable(QueryService& service, const std::string& table) {
  domain::ImportRequest request;
  request.file_path = "ignored.csv";
  request.table = table;
  request.primary_key = "id";
  return service.ImportCsv(request);
}

}  // namespace

TEST(test_paged_engine_import_table) {
  // A CSV of 1000 rows: about 17 pages of data.
  {
    ServiceFixture fixture = OpenService(kMinimumFrames, ImportLines(1000));
    QueryService& service = *fixture.service;
    const PageId pages_before = fixture.store->PageCount();
    ExpectComplete(ImportTable(service, "imported"),
                   "IMPORT imported (imported 1000 rows)");
    CHECK(fixture.store->PageCount() > pages_before + 10);

    // The import is written out before the call returns: a second database
    // over the bytes of the store sees it, before any query evicts a page.
    StoreEngine second = OpenStoreEngine(CopyOf(*fixture.store), 16);
    CHECK_EQ(QueryText(*second.engine, "SELECT COUNT(*) FROM imported"),
             std::vector<std::string>{"1000"});

    CHECK_EQ(ServiceQuery(service, "SELECT COUNT(*) FROM imported"),
             std::vector<std::string>{"1000"});
    CHECK_EQ(
        ServiceQuery(service, "SELECT id, score FROM imported WHERE id = 1000"),
        std::vector<std::string>{"1000\t1000.5"});
    ExpectNoPins(*fixture.engine);
  }
  // The third page allocation fails, which is the second page of the data
  // (the first page of the table and the root of its index come before it):
  // the table stays, empty.
  {
    ServiceFixture fixture = OpenService(kMinimumFrames, ImportLines(1000));
    QueryService& service = *fixture.service;
    fixture.store->InjectFailure(Operation::kAllocate, 3);
    ExecutionResult result = ImportTable(service, "partial");
    fixture.store->ClearInjectedFailures();
    const ExecutionError* error = ExpectError(result, ErrorKind::kIo);
    if (error != nullptr) {
      CHECK(Contains(error->message, "injected allocate failure"));
      CHECK(error->message.ends_with("; table partial was created"));
      CHECK(!Contains(error->message, "rollback failed"));
    }
    CHECK(HasTable(service, "partial"));
    CHECK_EQ(ServiceQuery(service, "SELECT COUNT(*) FROM partial"),
             std::vector<std::string>{"0"});
    ExpectNoPins(*fixture.engine);
  }
  // A row longer than 4060 bytes: no table is created and no page changes.
  {
    ServiceFixture fixture =
        OpenService(kMinimumFrames, ImportLines(3, std::string(4100, 'x')));
    QueryService& service = *fixture.service;
    CHECK(!fixture.engine->Flush().has_value());
    const PageImages before_pages = PagesOf(*fixture.store);
    ExpectErrorMessage(ImportTable(service, "toolong"), ErrorKind::kUnsupported,
                       "paged engine cannot store a row longer than 4060 "
                       "bytes in toolong");
    CHECK(!HasTable(service, "toolong"));
    CHECK(!fixture.engine->Flush().has_value());
    CHECK(PagesOf(*fixture.store) == before_pages);
    ExpectNoPins(*fixture.engine);
  }
}

namespace {

std::unique_ptr<PagedEngine> OpenFileEngine(
    const std::filesystem::path& directory) {
  DatabaseOptions options;
  options.pool_frames = 16;
  paged::PageResult<Database> database = Database::Open(directory, options);
  CHECK_OK(database);
  if (!database.ok()) {
    return nullptr;
  }
  return std::make_unique<PagedEngine>(std::move(database).value(), nullptr);
}

// Writes with one engine, ends it by Close or by destruction, and checks what
// a second engine over the same directory sees.
void CheckDurability(const std::filesystem::path& directory, bool is_closed) {
  {
    std::unique_ptr<PagedEngine> engine = OpenFileEngine(directory);
    if (engine == nullptr) {
      return;
    }
    ExpectComplete(Run(*engine,
                       "CREATE TABLE items (id INT PRIMARY KEY "
                       "AUTOINCREMENT, name TEXT UNIQUE, qty INT)"),
                   "CREATE TABLE items");
    ExpectAffected(
        Run(*engine,
            "INSERT INTO items (name, qty) VALUES ('a', 1), ('b', 2), "
            "('c', 3), ('d', 4), ('e', 5)"),
        5);
    ExpectAffected(
        Run(*engine, "UPDATE items SET qty = qty + 10 WHERE id <= 2"), 2);
    ExpectAffected(Run(*engine, "DELETE FROM items WHERE id = 3"), 1);
    if (is_closed) {
      CHECK(!engine->Close().has_value());
      ExpectErrorMessage(Run(*engine, "SELECT * FROM items"), ErrorKind::kIo,
                         "database is closed");
    }
  }
  std::unique_ptr<PagedEngine> reopened = OpenFileEngine(directory);
  if (reopened == nullptr) {
    return;
  }
  PagedEngine& engine = *reopened;
  const std::optional<domain::TableSchema> schema =
      engine.Catalog().FindTable("items");
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->columns.size(), std::size_t{3});
    CHECK(schema->auto_increment_index == std::optional<std::size_t>(0));
    CHECK(schema->unique_indexes == std::vector<std::size_t>{1});
  }
  CHECK_EQ(
      QueryText(engine, "SELECT id, name, qty FROM items ORDER BY id"),
      (std::vector<std::string>{"1\ta\t11", "2\tb\t12", "4\td\t4", "5\te\t5"}));
  // The auto-increment continues from the largest id plus one.
  ExpectAffected(Run(engine, "INSERT INTO items (name, qty) VALUES ('f', 6)"),
                 1);
  CHECK_EQ(QueryText(engine, "SELECT id FROM items WHERE name = 'f'"),
           std::vector<std::string>{"6"});
  ExpectNoPins(engine);
}

}  // namespace

TEST(test_paged_engine_data_survives_close_and_reopen) {
  TempDir closed_directory;
  CheckDurability(closed_directory.path(), true);
  TempDir destroyed_directory;
  CheckDurability(destroyed_directory.path(), false);
}

namespace {

// What a new process would see: an engine over a copy of the store's bytes.
StoreEngine OpenSecondEngine(const MemoryPageStore& store) {
  return OpenStoreEngine(CopyOf(store), 16);
}

std::vector<std::string> TableLines(const MemoryPageStore& store,
                                    const std::string& sql) {
  StoreEngine second = OpenSecondEngine(store);
  if (second.engine == nullptr) {
    return {};
  }
  return QueryText(*second.engine, sql);
}

}  // namespace

TEST(test_paged_engine_write_statement_writes_back_dirty_pages) {
  StoreEngine fixture = NewEngine(64);
  PagedEngine& engine = *fixture.engine;
  const int syncs_after_open = fixture.store->CallCount(Operation::kSync);

  ExpectComplete(Run(engine, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
                 "CREATE TABLE t");
  StoreEngine created = OpenSecondEngine(*fixture.store);
  CHECK(created.engine != nullptr &&
        created.engine->Catalog().FindTable("t").has_value());
  CHECK_EQ(TableLines(*fixture.store, "SELECT COUNT(*) FROM t"),
           std::vector<std::string>{"0"});

  ExpectAffected(Run(engine,
                     "INSERT INTO t VALUES (1, 'a'), (2, 'b'), "
                     "(3, 'c')"),
                 3);
  CHECK_EQ(TableLines(*fixture.store, "SELECT id, v FROM t ORDER BY id"),
           (std::vector<std::string>{"1\ta", "2\tb", "3\tc"}));

  ExpectAffected(Run(engine, "UPDATE t SET v = 'z' WHERE id = 2"), 1);
  CHECK_EQ(TableLines(*fixture.store, "SELECT id, v FROM t ORDER BY id"),
           (std::vector<std::string>{"1\ta", "2\tz", "3\tc"}));

  ExpectAffected(Run(engine, "DELETE FROM t WHERE id = 1"), 1);
  CHECK_EQ(TableLines(*fixture.store, "SELECT id, v FROM t ORDER BY id"),
           (std::vector<std::string>{"2\tz", "3\tc"}));

  ExpectComplete(Run(engine, "CREATE TABLE u (id INT PRIMARY KEY)"),
                 "CREATE TABLE u");
  CHECK_EQ(TableLines(*fixture.store, "SELECT COUNT(*) FROM u"),
           std::vector<std::string>{"0"});
  // No statement synced the file.
  CHECK_EQ(fixture.store->CallCount(Operation::kSync), syncs_after_open);

  // A failed statement and a query write nothing.
  const std::uint64_t writes_before =
      engine.database().buffer().Stats().page_writes;
  ExpectError(Run(engine, "INSERT INTO t VALUES (2, 'dup')"),
              ErrorKind::kConstraintViolation);
  CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM t"),
           std::vector<std::string>{"2"});
  CHECK_EQ(engine.database().buffer().Stats().page_writes, writes_before);
  CHECK_EQ(fixture.store->CallCount(Operation::kSync), syncs_after_open);

  CHECK(!engine.Flush().has_value());
  CHECK_EQ(fixture.store->CallCount(Operation::kSync), syncs_after_open + 1);
}

TEST(test_paged_engine_write_back_failure_reports_applied_but_not_saved) {
  StoreEngine fixture = NewEngine(64);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(Run(engine, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
                 "CREATE TABLE t");
  ExpectAffected(Run(engine, "INSERT INTO t VALUES (1, 'a')"), 1);

  fixture.store->InjectFailure(Operation::kWrite, 1);
  ExecutionResult failed = Run(engine, "INSERT INTO t VALUES (2, 'b')");
  const ExecutionError* error = ExpectError(failed, ErrorKind::kIo);
  if (error != nullptr) {
    CHECK(error->message.ends_with(kSavedSuffix));
    CHECK(Contains(error->message, "injected write failure"));
  }
  // The statement is applied in the pool, and the file still has one row.
  CHECK_EQ(QueryText(engine, "SELECT id FROM t ORDER BY id"),
           (std::vector<std::string>{"1", "2"}));
  CHECK_EQ(TableLines(*fixture.store, "SELECT id FROM t ORDER BY id"),
           std::vector<std::string>{"1"});

  // A rejected statement does not write the dirty page either.
  const std::uint64_t writes_before =
      engine.database().buffer().Stats().page_writes;
  ExpectError(Run(engine, "INSERT INTO t VALUES (1, 'dup')"),
              ErrorKind::kConstraintViolation);
  CHECK_EQ(engine.database().buffer().Stats().page_writes, writes_before);
  CHECK_EQ(TableLines(*fixture.store, "SELECT id FROM t ORDER BY id"),
           std::vector<std::string>{"1"});

  // The dirty page stays, and the next write statement writes it out.
  fixture.store->ClearInjectedFailures();
  ExpectAffected(Run(engine, "INSERT INTO t VALUES (3, 'c')"), 1);
  CHECK(!engine.Flush().has_value());
  CHECK_EQ(TableLines(*fixture.store, "SELECT id FROM t ORDER BY id"),
           (std::vector<std::string>{"1", "2", "3"}));

  // The failed statement did take effect.
  ExpectErrorMessage(Run(engine, "INSERT INTO t VALUES (2, 'b')"),
                     ErrorKind::kConstraintViolation, "duplicate key: t.id=2");
  ExpectNoPins(engine);
}

// A statement that changes nothing has nothing to save. While an earlier
// write-back failure left dirty pages behind, such a statement must not retry
// them: it would fail with "applied but not saved" for a change it never made.
TEST(test_paged_engine_statement_without_changes_does_not_write_back) {
  StoreEngine fixture = NewEngine(64);
  PagedEngine& engine = *fixture.engine;
  ExpectComplete(Run(engine, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
                 "CREATE TABLE t");
  ExpectAffected(Run(engine, "INSERT INTO t VALUES (1, 'a')"), 1);

  // The write-back of this statement fails and leaves the dirty page behind.
  fixture.store->InjectFailure(Operation::kWrite, 1);
  ExecutionResult failed = Run(engine, "INSERT INTO t VALUES (2, 'b')");
  const ExecutionError* error = ExpectError(failed, ErrorKind::kIo);
  if (error != nullptr) {
    CHECK(error->message.ends_with(kSavedSuffix));
  }

  // The next write call fails too. A statement that writes nothing never makes
  // that call: it succeeds, and neither counter moves.
  fixture.store->InjectFailure(Operation::kWrite, 1);
  const std::uint64_t page_writes_before =
      engine.database().buffer().Stats().page_writes;
  const int store_writes_before = fixture.store->CallCount(Operation::kWrite);
  ExpectAffected(Run(engine, "DELETE FROM t WHERE id = 999"), 0);
  ExpectAffected(Run(engine, "UPDATE t SET v = 'none' WHERE id = 999"), 0);
  // Every assignment equals the stored value: one row selected, none changed.
  ExpectAffected(Run(engine, "UPDATE t SET v = 'a' WHERE id = 1"), 1);
  CHECK_EQ(engine.database().buffer().Stats().page_writes, page_writes_before);
  CHECK_EQ(fixture.store->CallCount(Operation::kWrite), store_writes_before);
  CHECK_EQ(QueryText(engine, "SELECT id FROM t ORDER BY id"),
           (std::vector<std::string>{"1", "2"}));

  // A statement that changes something still writes back: it meets the armed
  // failure and reports applied-but-not-saved, and the next one saves the
  // pages that were left dirty.
  ExecutionResult changing = Run(engine, "DELETE FROM t WHERE id = 1");
  const ExecutionError* changing_error = ExpectError(changing, ErrorKind::kIo);
  if (changing_error != nullptr) {
    CHECK(changing_error->message.ends_with(kSavedSuffix));
  }
  CHECK_EQ(fixture.store->CallCount(Operation::kWrite),
           store_writes_before + 1);
  fixture.store->ClearInjectedFailures();
  ExpectAffected(Run(engine, "INSERT INTO t VALUES (3, 'c')"), 1);
  CHECK(engine.database().buffer().Stats().page_writes > page_writes_before);
  CHECK_EQ(TableLines(*fixture.store, "SELECT id FROM t ORDER BY id"),
           (std::vector<std::string>{"2", "3"}));
  ExpectNoPins(engine);
}

TEST(test_paged_engine_alter_table_add_column) {
  ServiceFixture fixture = OpenService(16);
  QueryService& service = *fixture.service;
  ExpectComplete(
      ExecuteSql(service, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
      "CREATE TABLE t");
  ExpectAffected(
      ExecuteSql(service, "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')"),
      3);

  ExpectComplete(
      ExecuteSql(service, "ALTER TABLE t ADD COLUMN c INT DEFAULT 7"),
      "ALTER TABLE t");
  ExpectComplete(
      ExecuteSql(service, "ALTER TABLE t ADD COLUMN d DOUBLE DEFAULT 1.5"),
      "ALTER TABLE t");
  ExpectComplete(
      ExecuteSql(service, "ALTER TABLE t ADD COLUMN e TEXT DEFAULT 'x'"),
      "ALTER TABLE t");
  CHECK_EQ(ServiceQuery(service, "SELECT id, v, c, d, e FROM t ORDER BY id"),
           (std::vector<std::string>{"1\ta\t7\t1.5\tx", "2\tb\t7\t1.5\tx",
                                     "3\tc\t7\t1.5\tx"}));

  // The new columns take part in statements like any other.
  ExpectAffected(ExecuteSql(service, "INSERT INTO t (id, v) VALUES (4, 'd')"),
                 1);
  ExpectAffected(ExecuteSql(service, "UPDATE t SET c = 9 WHERE id = 1"), 1);
  CHECK_EQ(ServiceQuery(service, "SELECT id, c, d, e FROM t ORDER BY id"),
           (std::vector<std::string>{"1\t9\t1.5\tx", "2\t7\t1.5\tx",
                                     "3\t7\t1.5\tx", "4\t7\t1.5\tx"}));
  const std::vector<domain::TableSchema> schemas = service.DescribeCatalog();
  CHECK_EQ(schemas.size(), std::size_t{1});
  if (schemas.size() == 1) {
    CHECK_EQ(schemas[0].columns.size(), std::size_t{5});
    if (schemas[0].columns.size() == 5) {
      CHECK(schemas[0].columns[2].default_value ==
            std::optional<Value>(std::int64_t{7}));
      CHECK(schemas[0].columns[3].default_value == std::optional<Value>(1.5));
      CHECK(schemas[0].columns[4].default_value ==
            std::optional<Value>(std::string("x")));
    }
  }

  // A table with no row.
  ExpectComplete(
      ExecuteSql(service, "CREATE TABLE empty_t (id INT PRIMARY KEY)"),
      "CREATE TABLE empty_t");
  ExpectComplete(
      ExecuteSql(service, "ALTER TABLE empty_t ADD COLUMN c INT DEFAULT 1"),
      "ALTER TABLE empty_t");
  CHECK(ServiceQuery(service, "SELECT id, c FROM empty_t").empty());
  ExpectAffected(ExecuteSql(service, "INSERT INTO empty_t (id) VALUES (5)"), 1);
  CHECK_EQ(ServiceQuery(service, "SELECT id, c FROM empty_t"),
           std::vector<std::string>{"5\t1"});

  // Errors: the Binder refuses these.
  ExpectError(
      ExecuteSql(service, "ALTER TABLE nosuch ADD COLUMN z INT DEFAULT 0"),
      ErrorKind::kBind);
  ExpectError(ExecuteSql(service, "ALTER TABLE t ADD COLUMN C INT DEFAULT 0"),
              ErrorKind::kBind);
  ExpectError(ExecuteSql(service, "ALTER TABLE t ADD COLUMN z INT"),
              ErrorKind::kBind);

  // Errors: the engine refuses these, and changes no page.
  ExpectComplete(ExecuteSql(service,
                            "CREATE TABLE big (id INT PRIMARY KEY, "
                            "body TEXT)"),
                 "CREATE TABLE big");
  ExpectAffected(ExecuteSql(service, "INSERT INTO big VALUES (1, '" +
                                         std::string(4043, 'x') + "')"),
                 1);
  CHECK(!fixture.engine->Flush().has_value());
  const PageImages before = PagesOf(*fixture.store);
  ExpectErrorMessage(
      ExecuteSql(service, "ALTER TABLE big ADD COLUMN x INT DEFAULT 0"),
      ErrorKind::kUnsupported,
      "paged engine cannot store a row longer than 4060 bytes in big");
  const ExecutionResult long_default =
      ExecuteSql(service, "ALTER TABLE t ADD COLUMN y TEXT DEFAULT '" +
                              std::string(4100, 'x') + "'");
  const ExecutionError* long_error =
      ExpectError(long_default, ErrorKind::kUnsupported);
  if (long_error != nullptr) {
    CHECK(long_error->message.starts_with(
        "column y of table t cannot be stored in the catalog:"));
  }
  CHECK(!fixture.engine->Flush().has_value());
  CHECK(PagesOf(*fixture.store) == before);
  ExpectNoPins(*fixture.engine);
}

TEST(test_paged_engine_drop_table) {
  ServiceFixture fixture = OpenService(16);
  QueryService& service = *fixture.service;
  ExpectComplete(ExecuteSql(service, "CREATE TABLE a (id INT PRIMARY KEY)"),
                 "CREATE TABLE a");
  ExpectComplete(
      ExecuteSql(service,
                 "CREATE TABLE b (id INT PRIMARY KEY, a_id INT REFERENCES "
                 "a(id))"),
      "CREATE TABLE b");
  ExpectComplete(ExecuteSql(service, "CREATE TABLE lone (id INT PRIMARY KEY)"),
                 "CREATE TABLE lone");
  ExpectAffected(ExecuteSql(service, "INSERT INTO a VALUES (1), (2)"), 2);
  ExpectAffected(ExecuteSql(service, "INSERT INTO b VALUES (10, 1)"), 1);
  ExpectAffected(ExecuteSql(service, "INSERT INTO lone VALUES (5)"), 1);

  ExpectComplete(ExecuteSql(service, "DROP TABLE lone"), "DROP TABLE lone");
  CHECK(!HasTable(service, "lone"));

  // A referenced table cannot be dropped, even before any row refers to it,
  // and the refusal changes no page.
  CHECK(!fixture.engine->Flush().has_value());
  const PageImages before = PagesOf(*fixture.store);
  ExpectErrorMessage(ExecuteSql(service, "DROP TABLE a"), ErrorKind::kBind,
                     "table a is referenced by b.a_id");
  CHECK(!fixture.engine->Flush().has_value());
  CHECK(PagesOf(*fixture.store) == before);

  ExpectComplete(ExecuteSql(service, "DROP TABLE b"), "DROP TABLE b");
  ExpectError(ExecuteSql(service, "SELECT * FROM b"), ErrorKind::kBind);
  ExpectComplete(ExecuteSql(service, "DROP TABLE a"), "DROP TABLE a");
  CHECK(service.DescribeCatalog().empty());
  ExpectError(ExecuteSql(service, "DROP TABLE a"), ErrorKind::kBind);

  // A table created again under a dropped name has the new schema and no row.
  ExpectComplete(
      ExecuteSql(service, "CREATE TABLE b (id INT PRIMARY KEY, z TEXT)"),
      "CREATE TABLE b");
  CHECK(ServiceQuery(service, "SELECT * FROM b").empty());
  ExpectAffected(ExecuteSql(service, "INSERT INTO b VALUES (1, 'z')"), 1);
  CHECK_EQ(ServiceQuery(service, "SELECT id, z FROM b"),
           std::vector<std::string>{"1\tz"});
  ExpectNoPins(*fixture.engine);
}

namespace {

// The rows of table t as text, from an engine opened over a copy of the store.
std::vector<std::string> RowsFromCopy(const MemoryPageStore& store,
                                      const std::string& sql) {
  return TableLines(store, sql);
}

}  // namespace

TEST(test_paged_engine_ddl_is_written_out_and_fails_cleanly) {
  // After each statement a second engine over a copy of the store sees it.
  {
    StoreEngine fixture = NewEngine(16);
    PagedEngine& engine = *fixture.engine;
    ExpectComplete(Run(engine, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
                   "CREATE TABLE t");
    ExpectAffected(Run(engine, "INSERT INTO t VALUES (1, 'a'), (2, 'b')"), 2);
    ExpectComplete(Run(engine, "ALTER TABLE t ADD COLUMN c INT DEFAULT 4"),
                   "ALTER TABLE t");
    CHECK_EQ(RowsFromCopy(*fixture.store, "SELECT id, c FROM t ORDER BY id"),
             (std::vector<std::string>{"1\t4", "2\t4"}));
    ExpectComplete(Run(engine, "DROP TABLE t"), "DROP TABLE t");
    StoreEngine second = OpenSecondEngine(*fixture.store);
    CHECK(second.engine != nullptr &&
          !second.engine->Catalog().FindTable("t").has_value());
  }

  // A failure at each read, write and allocation: the table is the old one or
  // the new one, and the message tells which.
  std::unique_ptr<MemoryPageStore> base;
  {
    StoreEngine fixture = NewEngine(16);
    PagedEngine& engine = *fixture.engine;
    ExpectComplete(Run(engine, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"),
                   "CREATE TABLE t");
    std::vector<Row> rows;
    for (std::int64_t id = 1; id <= 40; ++id) {
      rows.push_back(Row{Value(id), Value(std::string(2000, 'x'))});
    }
    ExpectAffected(RunInsert(engine, "t", std::move(rows)), 40);
    CHECK(!engine.Close().has_value());
    base = CopyOf(*fixture.store);
  }
  const char* const statements[] = {"ALTER TABLE t ADD COLUMN c INT DEFAULT 7",
                                    "DROP TABLE t"};
  const Operation operations[] = {Operation::kRead, Operation::kWrite,
                                  Operation::kAllocate};
  for (const char* statement : statements) {
    const bool is_drop = std::string(statement).starts_with("DROP");
    for (const Operation operation : operations) {
      bool has_succeeded = false;
      for (int nth_call = 1; nth_call < 600 && !has_succeeded; ++nth_call) {
        StoreEngine fixture = OpenStoreEngine(CopyOf(*base), kMinimumFrames);
        if (fixture.engine == nullptr) {
          return;
        }
        PagedEngine& engine = *fixture.engine;
        fixture.store->InjectFailure(operation, nth_call);
        const ExecutionResult result = Run(engine, statement);
        fixture.store->ClearInjectedFailures();
        ExpectNoPins(engine);
        bool is_applied = false;
        if (const ExecutionError* error = AsError(result)) {
          CHECK(error->kind == ErrorKind::kIo);
          const bool is_saved_failure = error->message.ends_with(kSavedSuffix);
          const bool is_not_reclaimed = Contains(
              error->message,
              "; statement was applied but its pages were not reclaimed");
          is_applied = is_saved_failure || is_not_reclaimed;
        } else {
          has_succeeded = true;
          is_applied = true;
        }
        const auto schema = engine.Catalog().FindTable("t");
        if (is_drop) {
          CHECK_EQ(schema.has_value(), !is_applied);
        } else {
          CHECK(schema.has_value());
          if (schema.has_value()) {
            CHECK_EQ(schema->columns.size(),
                     is_applied ? std::size_t{3} : std::size_t{2});
          }
        }
        if (!is_drop) {
          CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM t"),
                   std::vector<std::string>{"40"});
          if (is_applied) {
            CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM t WHERE c = 7"),
                     std::vector<std::string>{"40"});
          }
        }
      }
      CHECK(has_succeeded);
    }
  }
}

namespace {

constexpr const char* kIndexedTables[] = {
    "CREATE TABLE a (id INT PRIMARY KEY, v INT, t TEXT)",
    "CREATE TABLE b (id INT PRIMARY KEY, a_id INT REFERENCES a(id), t TEXT)",
    "CREATE TABLE p (id INT PRIMARY KEY, code INT UNIQUE)",
    "CREATE TABLE q (code INT PRIMARY KEY REFERENCES p(code), t TEXT)"};

// One random statement over the tables of kIndexedTables. `next_id` is the id
// that the next insert uses. Sets *is_cascade for a statement that renames
// the codes of p, which rewrites the primary keys of q.
std::string RandomStatement(std::mt19937& random, std::int64_t* next_id,
                            bool* is_cascade) {
  const unsigned choice = random() % 100;
  const std::int64_t known =
      1 + static_cast<std::int64_t>(random() % static_cast<unsigned>(*next_id));
  *is_cascade = false;
  if (choice < 25) {
    return "INSERT INTO a VALUES (" + std::to_string((*next_id)++) + ", " +
           std::to_string(random() % 10) + ", '" +
           std::string(random() % 600, 'x') + "')";
  }
  if (choice < 35) {
    return "INSERT INTO b VALUES (" + std::to_string((*next_id)++) + ", " +
           std::to_string(known) + ", 'y')";
  }
  if (choice < 45) {
    // A TEXT that grows moves rows to other pages.
    return "UPDATE a SET t = '" + std::string(200 + random() % 1500, 'z') +
           "' WHERE v = " + std::to_string(random() % 10);
  }
  if (choice < 53) {
    return "UPDATE a SET v = v + 1 WHERE id <= " + std::to_string(known);
  }
  if (choice < 61) {
    return "DELETE FROM a WHERE id = " + std::to_string(known);
  }
  if (choice < 67) {
    return "DELETE FROM b WHERE id <= " + std::to_string(known / 2);
  }
  if (choice < 75) {
    const std::int64_t id = (*next_id)++;
    return "INSERT INTO p VALUES (" + std::to_string(id) + ", " +
           std::to_string(id) + ")";
  }
  if (choice < 83) {
    return "INSERT INTO q VALUES (" + std::to_string(known) + ", 'q')";
  }
  if (choice < 89) {
    *is_cascade = true;
    return "UPDATE p SET code = code + 1";
  }
  if (choice < 92) {
    *is_cascade = true;
    return "UPDATE p SET code = code - 1";
  }
  if (choice < 96) {
    return "DELETE FROM q WHERE code <= " + std::to_string(known);
  }
  return "DELETE FROM p WHERE id = " + std::to_string(known);
}

}  // namespace

TEST(test_paged_engine_keeps_index_and_heap_in_step) {
  StoreEngine fixture = NewEngine(64);
  if (fixture.engine == nullptr) {
    return;
  }
  PagedEngine& engine = *fixture.engine;
  for (const char* sql : kIndexedTables) {
    CHECK(AsCommandComplete(Run(engine, sql)) != nullptr);
  }
  // Thirty parents and thirty children with consecutive codes, so that a
  // cascade renames A to B and B to C in the keys of q.
  std::string parents = "INSERT INTO p VALUES ";
  std::string children = "INSERT INTO q VALUES ";
  for (int code = 1; code <= 30; ++code) {
    parents += (code > 1 ? ", (" : "(") + std::to_string(code) + ", " +
               std::to_string(code) + ")";
    children += (code > 1 ? ", (" : "(") + std::to_string(code) + ", 'q')";
  }
  ExpectAffected(Run(engine, parents), 30);
  ExpectAffected(Run(engine, children), 30);

  std::mt19937 random(31);
  std::int64_t next_id = 100;
  int statements_applied = 0;
  int cascades_applied = 0;
  for (int step = 0; step < 2000; ++step) {
    bool is_cascade = false;
    const std::string sql = RandomStatement(random, &next_id, &is_cascade);
    const ExecutionResult result = Run(engine, sql);
    if (AsAffectedRows(result) != nullptr) {
      ++statements_applied;
      if (is_cascade) {
        ++cascades_applied;
      }
    }
    CheckEveryIndex(engine);
    if (engine.database().buffer().PinnedFrameCount() != 0) {
      ExpectNoPins(engine);
      return;
    }
  }
  CHECK(statements_applied > 800);
  CHECK(cascades_applied > 10);

  // With 4 frames, a write that fails at each position leaves the heap and the
  // index in step: a cascade, a 300-row insert that splits leaves, a 150-row
  // delete.
  const Dataset dataset{4, 200, 200, 100};
  StoreEngine loaded = LoadedTemplate(dataset);
  FailureCounts cascade_counts;
  CheckEveryWritePosition(*loaded.store, kCascadeUpdate, &cascade_counts);
  CHECK(cascade_counts.apply_failures > 0);
  FailureCounts insert_counts;
  CheckEveryWritePosition(
      *loaded.store,
      ReactionInsert(1000, 300, dataset.story_count, dataset.padding),
      &insert_counts);
  CHECK(insert_counts.apply_failures > 0);
  FailureCounts delete_counts;
  CheckEveryWritePosition(
      *loaded.store, "DELETE FROM reactions WHERE id <= 150", &delete_counts);
  CHECK(delete_counts.apply_failures > 0);
}

namespace {

// A copy of `loaded` in which the leaf entry of `key` is missing from the index
// of `table`, written with a valid checksum, and an engine over it.
StoreEngine OpenWithoutIndexEntry(const MemoryPageStore& loaded,
                                  const std::string& table, std::int64_t key) {
  StoreEngine reference = OpenCopy(loaded);
  StoreEngine none;
  if (reference.engine == nullptr) {
    return none;
  }
  const std::optional<PageId> root =
      reference.engine->database().catalog().IndexRoot(table);
  CHECK(root.has_value());
  if (!root.has_value()) {
    return none;
  }
  std::unique_ptr<MemoryPageStore> copy = CopyOf(loaded);
  std::uint8_t* const page = copy->MutablePage(*root);
  CHECK(paged::NodeView(page).is_leaf());  // 50 keys fit in the root
  const paged::LeafPosition position = paged::LeafSearch(page, key);
  CHECK(position.is_found);
  paged::LeafRemoveAt(page, position.index);
  paged::SlottedPage::SealChecksum(page);
  return OpenStoreEngine(std::move(copy), 16);
}

}  // namespace

TEST(test_paged_engine_reports_an_index_entry_that_is_missing) {
  StoreEngine loaded = NewEngine(64);
  if (loaded.engine == nullptr) {
    return;
  }
  // 50 rows of about 1000 bytes: three to a page, each page nearly full.
  ExpectComplete(
      Run(*loaded.engine,
          "CREATE TABLE gems (id INT PRIMARY KEY, tag INT, body TEXT)"),
      "CREATE TABLE gems");
  std::string insert = "INSERT INTO gems VALUES ";
  for (int id = 1; id <= 50; ++id) {
    insert += (id > 1 ? ", (" : "(") + std::to_string(id) + ", " +
              std::to_string(id) + ", '" + std::string(1000, 'g') + "')";
  }
  ExpectAffected(Run(*loaded.engine, insert), 50);
  CHECK(!loaded.engine->Flush().has_value());

  // The statements read the row through a non-key column, because a key
  // predicate reads through the index and would skip it.
  const std::string statements[] = {
      "DELETE FROM gems WHERE tag = 7",
      "UPDATE gems SET body = '" + std::string(2500, 'h') + "' WHERE tag = 7"};
  for (const std::string& sql : statements) {
    StoreEngine damaged = OpenWithoutIndexEntry(*loaded.store, "gems", 7);
    if (damaged.engine == nullptr) {
      return;
    }
    PagedEngine& engine = *damaged.engine;
    const std::vector<std::string> others_before =
        SortedQueryText(engine, "SELECT id, tag FROM gems WHERE tag <> 7");
    CHECK_EQ(others_before.size(), std::size_t{49});

    const ExecutionResult result = Run(engine, sql);
    const ExecutionError* error = ExpectError(result, ErrorKind::kIo);
    if (error != nullptr) {
      CHECK_EQ(error->message,
               std::string("corrupt: index of gems has no entry for key 7"));
    }
    // The row is still in the table, and the other 49 rows read as before.
    CHECK_EQ(QueryText(engine, "SELECT COUNT(*) FROM gems"),
             std::vector<std::string>{"50"});
    CHECK_EQ(QueryText(engine, "SELECT id FROM gems WHERE tag = 7"),
             std::vector<std::string>{"7"});
    CHECK_EQ(SortedQueryText(engine, "SELECT id, tag FROM gems WHERE tag <> 7"),
             others_before);
    // The index is still damaged, and CheckIndex names the key.
    const paged::Status check = engine.database().CheckIndex("gems");
    CHECK_ERROR(check, kCorrupt);
    CHECK_MESSAGE_CONTAINS(check, "has no entry for key 7");
    ExpectNoPins(engine);
  }
}

namespace {

// The outcome of one statement as text: the sorted lines of a result set,
// "affected=<n>", or the error.
std::string OutcomeText(PagedEngine& engine, const std::string& sql) {
  const ExecutionResult result = Run(engine, sql);
  if (const domain::ResultSet* rows = AsResultSet(result)) {
    std::string text;
    for (const std::string& line : SortedTextOf(rows->rows)) {
      text += line + "\n";
    }
    return text;
  }
  return DescribeResult(result);
}

// Loads 1500 rows with ascending keys (the last leaf then holds 195 keys) and
// runs statements that read and change the index; returns each outcome.
std::vector<std::string> IndexedStatementOutcomes(StoreEngine& fixture) {
  PagedEngine& engine = *fixture.engine;
  std::vector<std::string> outcomes;
  CHECK(AsCommandComplete(Run(
            engine,
            "CREATE TABLE wide (id INT PRIMARY KEY, grp INT, body TEXT)")) !=
        nullptr);
  CHECK(AsCommandComplete(Run(
            engine, "CREATE TABLE narrow (id INT PRIMARY KEY, label TEXT)")) !=
        nullptr);
  for (int first = 1; first <= 1500; first += 100) {
    std::string wide = "INSERT INTO wide VALUES ";
    std::string narrow = "INSERT INTO narrow VALUES ";
    for (int id = first; id < first + 100; ++id) {
      const std::string suffix = id < first + 99 ? ", " : "";
      wide += "(" + std::to_string(id) + ", " + std::to_string(id % 7) + ", '" +
              std::string(40, 'w') + "')" + suffix;
      narrow += "(" + std::to_string(id) + ", 'n" + std::to_string(id) + "')" +
                suffix;
    }
    ExpectAffected(Run(engine, wide), 100);
    ExpectAffected(Run(engine, narrow), 100);
  }
  std::string more = "INSERT INTO wide VALUES ";
  for (int id = 1501; id <= 1600; ++id) {
    more += "(" + std::to_string(id) + ", 0, 'm')" + (id < 1600 ? ", " : "");
  }
  const std::string statements[] = {
      "SELECT id, grp FROM wide WHERE id = 1234",
      "SELECT COUNT(*), MIN(id), MAX(id) FROM wide WHERE id >= 100 AND id < "
      "400",
      more,
      "UPDATE wide SET body = '" + std::string(3000, 'u') + "' WHERE id = 1234",
      "DELETE FROM wide WHERE id >= 1550 AND id < 1560",
      "SELECT wide.id, narrow.label FROM wide JOIN narrow ON wide.id = "
      "narrow.id WHERE wide.id = 700 AND narrow.id = 700",
      "SELECT COUNT(*) FROM wide"};
  for (const std::string& sql : statements) {
    outcomes.push_back(OutcomeText(engine, sql));
    CHECK(!Contains(outcomes.back(), "buffer pool exhausted"));
    CHECK(!Contains(outcomes.back(), "error"));
    ExpectNoPins(engine);
  }
  CheckEveryIndex(engine);
  return outcomes;
}

}  // namespace

TEST(test_paged_engine_runs_indexed_statements_in_four_frames) {
  StoreEngine large = NewEngine(256);
  if (large.engine == nullptr) {
    return;
  }
  const std::vector<std::string> expected = IndexedStatementOutcomes(large);
  CHECK_EQ(expected.size(), std::size_t{7});
  CHECK_EQ(expected[0],
           std::string("1234\t") + std::to_string(1234 % 7) + "\n");
  const ReplacementPolicyKind policies[] = {ReplacementPolicyKind::kLru,
                                            ReplacementPolicyKind::kTwoQ};
  for (ReplacementPolicyKind policy : policies) {
    StoreEngine small = OpenStoreEngine(std::make_unique<MemoryPageStore>(),
                                        kMinimumFrames, policy);
    if (small.engine == nullptr) {
      return;
    }
    CHECK(IndexedStatementOutcomes(small) == expected);
  }
}

}  // namespace strata::testing
