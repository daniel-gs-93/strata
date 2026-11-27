#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "strata/application.h"
#include "strata/domain/import.h"
#include "strata/paged/database.h"
#include "strata/paged/page_store.h"
#include "strata/paged/paged_engine.h"
#include "test_support.h"

// The database-level reader-writer lock of QueryService (design document 2.14,
// ADR-0040; docs/TestDocument.md). The unit tests run
// QueryService over a probe engine that records which calls are inside it at
// the same time; the integration test runs readers and one writer through
// QueryService over a PagedEngine and over a FlatFileEngine.
//
// Every wait on another thread uses a condition variable with a predicate. The
// hang guard below only turns a hang into a failure; it never decides a
// passing outcome. The only wait whose expiry is the expected outcome is the
// quiet window of the exclusive-lock test, where a correct lock keeps every
// other call out, so a
// short window cannot fail a correct implementation. The CHECK macros run on
// the main thread only: the harness is not thread safe.

namespace {

namespace domain = strata::domain;
namespace paged = strata::paged;
namespace testing = strata::testing;
using strata::application::QueryService;

// Upper bound of every wait whose condition must come true.
constexpr std::chrono::seconds kHangGuard(10);
// How long a test watches for a call that should be blocked to get through.
constexpr std::chrono::milliseconds kQuietWindow(150);

// ----------------------------------------------------------------------------
// A countdown that threads wait on.
// ----------------------------------------------------------------------------

class Latch {
 public:
  explicit Latch(int count) : remaining_(count) {}

  void CountDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (remaining_ > 0) {
      --remaining_;
    }
    if (remaining_ == 0) {
      opened_.notify_all();
    }
  }

  // True when the count reached 0 within the hang guard.
  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return opened_.wait_for(lock, kHangGuard, IsOpen{this});
  }

 private:
  struct IsOpen {
    const Latch* latch;
    bool operator()() const { return latch->remaining_ == 0; }
  };

  std::mutex mutex_;
  std::condition_variable opened_;
  int remaining_;
};

// ----------------------------------------------------------------------------
// The probe: what the fake engine and its catalog record.
// ----------------------------------------------------------------------------

enum class EventKind {
  kQueryEnter,
  kQueryExit,
  kWriteEnter,
  kWriteExit,
  kCatalogRead
};

struct Event {
  std::thread::id thread;
  EventKind kind;
};

// The catalog version a statement saw when it was bound and when it ran.
struct VersionPair {
  int bind_version;
  int execute_version;
};

constexpr int kNoVersion = -1;

// The version of the first catalog read since the last Execute on this thread.
thread_local int g_bind_version = kNoVersion;

bool IsWritePlan(const domain::PlanNode& plan) {
  const auto& node = plan.node;
  return std::holds_alternative<domain::InsertNode>(node) ||
         std::holds_alternative<domain::UpdateNode>(node) ||
         std::holds_alternative<domain::DeleteNode>(node) ||
         std::holds_alternative<domain::CreateTableNode>(node) ||
         std::holds_alternative<domain::AddColumnNode>(node) ||
         std::holds_alternative<domain::DropTableNode>(node) ||
         std::holds_alternative<domain::ImportTableNode>(node);
}

domain::ExecutionResult CannedResult(const domain::PlanNode& plan) {
  if (std::holds_alternative<domain::CreateTableNode>(plan.node) ||
      std::holds_alternative<domain::ImportTableNode>(plan.node)) {
    return domain::CommandComplete{"done"};
  }
  if (IsWritePlan(plan)) {
    return domain::AffectedRows{1};
  }
  domain::ResultSet result_set;
  result_set.columns.push_back(domain::ColumnDef{"id", domain::DataType::kInt});
  result_set.rows.push_back(domain::Row{domain::Value(int64_t{1})});
  return result_set;
}

// Shared by the probe engine, the probe catalog and the test. All state is
// guarded by mutex_, which is the probe's own and has nothing to do with the
// lock under test.
class Probe {
 public:
  // ---- test side ----

  // Every Execute waits inside the engine until this many calls have been
  // inside at the same time.
  void SetOverlapTarget(int target) {
    std::lock_guard<std::mutex> lock(mutex_);
    overlap_target_ = target;
  }

  // The next Execute that enters the engine waits there until Release.
  void HoldNextExecute() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_hold_pending_ = true;
    is_released_ = false;
  }

  // True when a held call is waiting inside the engine.
  bool WaitUntilParked() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kHangGuard, IsParked{this});
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_released_ = true;
    changed_.notify_all();
  }

  std::size_t EventCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
  }

  // True when more than count events exist, or appear within window.
  bool WaitForEventBeyond(std::size_t count, std::chrono::milliseconds window) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, window, HasEventsBeyond{this, count});
  }

  std::vector<Event> Events() {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

  int MaxInside() {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_inside_;
  }

  int CatalogVersion() {
    std::lock_guard<std::mutex> lock(mutex_);
    return catalog_version_;
  }

  std::vector<VersionPair> QueryVersions() {
    std::lock_guard<std::mutex> lock(mutex_);
    return query_versions_;
  }

  // ---- engine and catalog side ----

  domain::ExecutionResult RunExecute(const domain::PlanNode& plan) {
    const bool is_write = IsWritePlan(plan);
    std::unique_lock<std::mutex> lock(mutex_);
    Record(is_write ? EventKind::kWriteEnter : EventKind::kQueryEnter);
    ++inside_;
    max_inside_ = std::max(max_inside_, inside_);
    changed_.notify_all();
    if (is_hold_pending_) {
      is_hold_pending_ = false;
      is_parked_ = true;
      changed_.notify_all();
      changed_.wait_for(lock, kHangGuard, IsReleased{this});
      is_parked_ = false;
    } else if (overlap_target_ > 0) {
      changed_.wait_for(lock, kHangGuard, PeakAtLeast{this, overlap_target_});
    }
    if (std::holds_alternative<domain::CreateTableNode>(plan.node)) {
      ++catalog_version_;
    }
    if (!is_write) {
      query_versions_.push_back(VersionPair{g_bind_version, catalog_version_});
    }
    g_bind_version = kNoVersion;
    --inside_;
    Record(is_write ? EventKind::kWriteExit : EventKind::kQueryExit);
    return CannedResult(plan);
  }

  void NoteCatalogRead() {
    std::lock_guard<std::mutex> lock(mutex_);
    Record(EventKind::kCatalogRead);
    if (g_bind_version == kNoVersion) {
      g_bind_version = catalog_version_;
    }
  }

 private:
  struct IsParked {
    const Probe* probe;
    bool operator()() const { return probe->is_parked_; }
  };
  struct IsReleased {
    const Probe* probe;
    bool operator()() const { return probe->is_released_; }
  };
  // The peak, not the current count: a call that arrived second may already
  // have left when the first one wakes up.
  struct PeakAtLeast {
    const Probe* probe;
    int target;
    bool operator()() const { return probe->max_inside_ >= target; }
  };
  struct HasEventsBeyond {
    const Probe* probe;
    std::size_t count;
    bool operator()() const { return probe->events_.size() > count; }
  };

  // Callers hold mutex_.
  void Record(EventKind kind) {
    events_.push_back(Event{std::this_thread::get_id(), kind});
    changed_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<Event> events_;
  std::vector<VersionPair> query_versions_;
  int inside_ = 0;
  int max_inside_ = 0;
  int overlap_target_ = 0;
  int catalog_version_ = 0;
  bool is_hold_pending_ = false;
  bool is_parked_ = false;
  bool is_released_ = false;
};

// The tables of FakeCatalog; every read is logged and stamps the bind version.
class ProbeCatalog : public domain::ICatalog {
 public:
  explicit ProbeCatalog(std::shared_ptr<Probe> probe)
      : probe_(std::move(probe)) {}

  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override {
    probe_->NoteCatalogRead();
    return inner_.FindTable(name);
  }

  std::vector<std::string> TableNames() const override {
    probe_->NoteCatalogRead();
    return inner_.TableNames();
  }

 private:
  std::shared_ptr<Probe> probe_;
  testing::FakeCatalog inner_;
};

class ProbeEngine : public domain::IEngine {
 public:
  explicit ProbeEngine(std::shared_ptr<Probe> probe)
      : probe_(probe), catalog_(std::move(probe)) {}

  const domain::ICatalog& Catalog() const override { return catalog_; }

  domain::ExecutionResult Execute(const domain::PlanNode& plan) override {
    return probe_->RunExecute(plan);
  }

 private:
  std::shared_ptr<Probe> probe_;
  ProbeCatalog catalog_;
};

// Serves two CSV lines, a header and one row, whatever the path.
class FixedCsvSource : public domain::ICsvSource {
 public:
  std::variant<std::vector<domain::CsvLine>, std::string> Read(
      const std::string& path) override {
    static_cast<void>(path);
    std::vector<domain::CsvLine> lines;
    lines.push_back(domain::CsvLine{1, std::vector<std::string>{"id", "name"}});
    lines.push_back(domain::CsvLine{2, std::vector<std::string>{"1", "alpha"}});
    return lines;
  }
};

// Checks the log: no query or writer enters while a writer is inside, no
// writer enters while a query is inside, and no other thread reads the catalog
// while a writer is inside. Returns one line per violation.
std::vector<std::string> FindLockViolations(const std::vector<Event>& events) {
  std::vector<std::string> violations;
  int queries_inside = 0;
  bool is_writer_inside = false;
  std::thread::id writer_thread;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    const std::string where = " (event " + std::to_string(i) + ")";
    switch (event.kind) {
      case EventKind::kQueryEnter:
        if (is_writer_inside) {
          violations.push_back("query entered while a writer is inside" +
                               where);
        }
        ++queries_inside;
        break;
      case EventKind::kQueryExit:
        --queries_inside;
        break;
      case EventKind::kWriteEnter:
        if (is_writer_inside) {
          violations.push_back("writer entered while a writer is inside" +
                               where);
        }
        if (queries_inside > 0) {
          violations.push_back("writer entered while a query is inside" +
                               where);
        }
        is_writer_inside = true;
        writer_thread = event.thread;
        break;
      case EventKind::kWriteExit:
        is_writer_inside = false;
        break;
      case EventKind::kCatalogRead:
        if (is_writer_inside && event.thread != writer_thread) {
          violations.push_back(
              "another thread read the catalog while a writer is inside" +
              where);
        }
        break;
    }
  }
  return violations;
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string joined;
  for (const std::string& line : lines) {
    joined += line + "; ";
  }
  return joined;
}

// ----------------------------------------------------------------------------
// The calls the threads make.
// ----------------------------------------------------------------------------

enum class CallKind {
  kSelect,
  kInsert,
  kUpdate,
  kDelete,
  kCreateTable,
  kImportCsv,
  kDescribeCatalog
};

const char* CallName(CallKind kind) {
  switch (kind) {
    case CallKind::kSelect:
      return "SELECT";
    case CallKind::kInsert:
      return "INSERT";
    case CallKind::kUpdate:
      return "UPDATE";
    case CallKind::kDelete:
      return "DELETE";
    case CallKind::kCreateTable:
      return "CREATE TABLE";
    case CallKind::kImportCsv:
      return "ImportCsv";
    case CallKind::kDescribeCatalog:
      return "DescribeCatalog";
  }
  return "?";
}

// The statements of the calls, parsed on the main thread and only read by the
// workers.
struct CallSet {
  domain::Statement select;
  domain::Statement insert;
  domain::Statement update;
  domain::Statement delete_rows;
  domain::Statement create_table;
  domain::ImportRequest import_request;
};

CallSet MakeCallSet() {
  CallSet calls;
  calls.select = testing::MustParse("SELECT id FROM visitors");
  calls.insert =
      testing::MustParse("INSERT INTO visitors VALUES (9, 'zed', 'Oslo')");
  calls.update =
      testing::MustParse("UPDATE visitors SET location = 'Rome' WHERE id = 1");
  calls.delete_rows = testing::MustParse("DELETE FROM visitors WHERE id = 1");
  calls.create_table =
      testing::MustParse("CREATE TABLE fresh (id INT PRIMARY KEY)");
  calls.import_request.file_path = "fresh.csv";
  calls.import_request.table = "imported";
  calls.import_request.primary_key = "id";
  return calls;
}

// What one worker thread got back.
struct CallOutcome {
  domain::ExecutionResult result = domain::AffectedRows{0};
  std::size_t schema_count = 0;  // DescribeCatalog only
  bool is_finished = false;
};

const domain::Statement& StatementOf(const CallSet& calls, CallKind kind) {
  switch (kind) {
    case CallKind::kInsert:
      return calls.insert;
    case CallKind::kUpdate:
      return calls.update;
    case CallKind::kDelete:
      return calls.delete_rows;
    case CallKind::kCreateTable:
      return calls.create_table;
    default:
      return calls.select;
  }
}

// Thread body: waits for the gate (when there is one), counts down started
// just before the call (when there is one), makes the call, then counts down
// finished. Writes only to *outcome.
void RunCall(QueryService* service, const CallSet* calls, CallKind kind,
             Latch* gate, Latch* started, Latch* finished,
             CallOutcome* outcome) {
  if (gate != nullptr) {
    gate->Wait();
  }
  if (started != nullptr) {
    started->CountDown();
  }
  switch (kind) {
    case CallKind::kImportCsv:
      outcome->result = service->ImportCsv(calls->import_request);
      break;
    case CallKind::kDescribeCatalog:
      outcome->schema_count = service->DescribeCatalog().size();
      break;
    default:
      outcome->result = service->Execute(StatementOf(*calls, kind));
      break;
  }
  outcome->is_finished = true;
  if (finished != nullptr) {
    finished->CountDown();
  }
}

std::unique_ptr<QueryService> MakeProbeService(
    const std::shared_ptr<Probe>& probe) {
  return std::make_unique<QueryService>(std::make_unique<ProbeEngine>(probe),
                                        std::make_unique<FixedCsvSource>());
}

void JoinAll(std::vector<std::thread>& threads) {
  for (std::thread& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

// Reports a failure when the outcome is an error or a call did not finish.
void CheckOutcomeIsSuccess(CallKind kind, const CallOutcome& outcome) {
  if (!outcome.is_finished) {
    testing::ReportFailure(__FILE__, __LINE__,
                           std::string(CallName(kind)) + " did not finish");
    return;
  }
  if (const domain::ExecutionError* error = testing::AsError(outcome.result)) {
    testing::ReportFailure(__FILE__, __LINE__,
                           std::string(CallName(kind)) + " returned error " +
                               domain::ErrorKindName(error->kind) + ": " +
                               error->message);
  }
}

}  // namespace

TEST(test_query_service_lock_readers_overlap) {
  CallSet calls = MakeCallSet();

  // Part 1: two SELECT calls wait inside the engine until both are inside.
  {
    auto probe = std::make_shared<Probe>();
    std::unique_ptr<QueryService> service = MakeProbeService(probe);
    probe->SetOverlapTarget(2);
    std::vector<CallOutcome> outcomes(2);
    std::vector<std::thread> threads;
    for (CallOutcome& outcome : outcomes) {
      threads.emplace_back(RunCall, service.get(), &calls, CallKind::kSelect,
                           nullptr, nullptr, nullptr, &outcome);
    }
    JoinAll(threads);
    CHECK(probe->MaxInside() >= 2);
    for (const CallOutcome& outcome : outcomes) {
      CheckOutcomeIsSuccess(CallKind::kSelect, outcome);
    }
  }

  // Part 2: a SELECT is held inside the engine; a second SELECT and
  // DescribeCatalog both finish while it is still inside.
  {
    auto probe = std::make_shared<Probe>();
    std::unique_ptr<QueryService> service = MakeProbeService(probe);
    probe->HoldNextExecute();
    CallOutcome held;
    std::thread held_thread(RunCall, service.get(), &calls, CallKind::kSelect,
                            nullptr, nullptr, nullptr, &held);
    const bool is_parked = probe->WaitUntilParked();
    CHECK(is_parked);

    Latch finished(2);
    CallOutcome second_select;
    CallOutcome describe;
    std::vector<std::thread> threads;
    if (is_parked) {
      threads.emplace_back(RunCall, service.get(), &calls, CallKind::kSelect,
                           nullptr, nullptr, &finished, &second_select);
      threads.emplace_back(RunCall, service.get(), &calls,
                           CallKind::kDescribeCatalog, nullptr, nullptr,
                           &finished, &describe);
    }
    // A blocked call keeps this from opening, and the hang guard ends the wait.
    const bool is_both_finished = is_parked && finished.Wait();
    CHECK(is_both_finished);
    probe->Release();
    held_thread.join();
    JoinAll(threads);
    CheckOutcomeIsSuccess(CallKind::kSelect, held);
    if (is_parked) {
      CheckOutcomeIsSuccess(CallKind::kSelect, second_select);
      CheckOutcomeIsSuccess(CallKind::kDescribeCatalog, describe);
      CHECK_EQ(describe.schema_count, std::size_t{6});
    }
  }
}

namespace {

// Holds one call of writer_kind inside the engine, starts four other calls,
// and checks that none gets through until the writer leaves.
void CheckWriterIsExclusive(CallKind writer_kind) {
  const std::string name = CallName(writer_kind);
  CallSet calls = MakeCallSet();
  auto probe = std::make_shared<Probe>();
  std::unique_ptr<QueryService> service = MakeProbeService(probe);

  probe->HoldNextExecute();
  CallOutcome writer_outcome;
  std::thread writer(RunCall, service.get(), &calls, writer_kind, nullptr,
                     nullptr, nullptr, &writer_outcome);
  if (!probe->WaitUntilParked()) {
    testing::ReportFailure(__FILE__, __LINE__,
                           name + ": the writer never entered the engine");
    probe->Release();
    writer.join();
    return;
  }
  const std::size_t events_while_parked = probe->EventCount();

  const CallKind other_kinds[] = {CallKind::kSelect, CallKind::kDescribeCatalog,
                                  CallKind::kDelete, CallKind::kImportCsv};
  constexpr std::size_t kOtherCount = 4;
  Latch started(static_cast<int>(kOtherCount));
  std::vector<CallOutcome> other_outcomes(kOtherCount);
  std::vector<std::thread> others;
  for (std::size_t i = 0; i < kOtherCount; ++i) {
    others.emplace_back(RunCall, service.get(), &calls, other_kinds[i], nullptr,
                        &started, nullptr, &other_outcomes[i]);
  }
  const bool is_all_started = started.Wait();
  CHECK(is_all_started);

  // The quiet window ends early when another call logs an event, which is the
  // failure; a correct lock keeps them all waiting for the whole window.
  const bool has_call_got_through =
      probe->WaitForEventBeyond(events_while_parked, kQuietWindow);
  if (has_call_got_through) {
    testing::ReportFailure(
        __FILE__, __LINE__,
        name + ": another call got through while the writer was inside");
  }

  probe->Release();
  writer.join();
  JoinAll(others);

  std::vector<std::string> violations = FindLockViolations(probe->Events());
  if (!violations.empty()) {
    testing::ReportFailure(__FILE__, __LINE__,
                           name + ": lock violations: " + Joined(violations));
  }
  // The only calls that enter the engine are one reader and the writers, so a
  // correct lock never lets more than one call be inside at any moment.
  CHECK_EQ(probe->MaxInside(), 1);
  CheckOutcomeIsSuccess(writer_kind, writer_outcome);
  for (std::size_t i = 0; i < kOtherCount; ++i) {
    CheckOutcomeIsSuccess(other_kinds[i], other_outcomes[i]);
  }
}

}  // namespace

TEST(test_query_service_lock_writer_is_exclusive) {
  const CallKind writer_kinds[] = {CallKind::kInsert, CallKind::kUpdate,
                                   CallKind::kDelete, CallKind::kCreateTable,
                                   CallKind::kImportCsv};
  for (CallKind writer_kind : writer_kinds) {
    CheckWriterIsExclusive(writer_kind);
  }
}

namespace {

// Runs iterations calls of one kind, one after another.
void RunCallRepeatedly(QueryService* service, const CallSet* calls,
                       CallKind kind, int iterations, Latch* gate) {
  gate->Wait();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    static_cast<void>(service->Execute(StatementOf(*calls, kind)));
  }
}

}  // namespace

TEST(test_query_service_lock_covers_bind_and_execute) {
  constexpr int kReaderThreads = 4;
  constexpr int kDdlThreads = 2;
  constexpr int kIterations = 300;

  CallSet calls = MakeCallSet();
  auto probe = std::make_shared<Probe>();
  std::unique_ptr<QueryService> service = MakeProbeService(probe);

  Latch gate(1);
  std::vector<std::thread> threads;
  for (int i = 0; i < kReaderThreads; ++i) {
    threads.emplace_back(RunCallRepeatedly, service.get(), &calls,
                         CallKind::kSelect, kIterations, &gate);
  }
  for (int i = 0; i < kDdlThreads; ++i) {
    threads.emplace_back(RunCallRepeatedly, service.get(), &calls,
                         CallKind::kCreateTable, kIterations, &gate);
  }
  gate.CountDown();
  JoinAll(threads);

  const std::vector<VersionPair> versions = probe->QueryVersions();
  CHECK_EQ(versions.size(), static_cast<std::size_t>(kReaderThreads) *
                                static_cast<std::size_t>(kIterations));
  std::size_t mismatch_count = 0;
  for (const VersionPair& pair : versions) {
    if (pair.bind_version != pair.execute_version) {
      ++mismatch_count;
    }
  }
  CHECK_EQ(mismatch_count, std::size_t{0});
  // Every DDL ran, so the interleaving the test relies on did happen.
  CHECK_EQ(probe->CatalogVersion(), kDdlThreads * kIterations);
  std::vector<std::string> violations = FindLockViolations(probe->Events());
  CHECK_EQ(Joined(violations), std::string(""));
}

namespace {

constexpr int kConcurrentReaders = 8;
constexpr int kPoolFrames = 16;  // not fewer than the readers (design 2.13)
constexpr int kRowCount = 240;
constexpr int kJoinedRowCount = 60;
constexpr int kReaderIterations = 40;
constexpr int kWriterUpdates = 20;

// What one reader saw: one (MIN, MAX) pair per query.
struct ReaderLog {
  std::vector<std::int64_t> mins;
  std::vector<std::int64_t> maxes;
  std::vector<std::string> problems;
};

// Appends the pair in the single row of result, or a problem.
void RecordMinMax(const domain::ExecutionResult& result, const char* label,
                  ReaderLog* log) {
  if (const domain::ExecutionError* error = testing::AsError(result)) {
    log->problems.push_back(std::string(label) +
                            " failed: " + domain::ErrorKindName(error->kind) +
                            ": " + error->message);
    return;
  }
  const domain::ResultSet* result_set = testing::AsResultSet(result);
  if (result_set == nullptr || result_set->rows.size() != 1 ||
      result_set->rows[0].size() != 2) {
    log->problems.push_back(std::string(label) + " did not return one pair");
    return;
  }
  const domain::Row& row = result_set->rows[0];
  const std::int64_t* min_value = std::get_if<std::int64_t>(&row[0]);
  const std::int64_t* max_value = std::get_if<std::int64_t>(&row[1]);
  if (min_value == nullptr || max_value == nullptr) {
    log->problems.push_back(std::string(label) + " returned a non-INT value");
    return;
  }
  log->mins.push_back(*min_value);
  log->maxes.push_back(*max_value);
}

// Thread body of a reader: alternates the aggregate query and the join query.
void RunReader(QueryService* service, const domain::Statement* aggregate_query,
               const domain::Statement* join_query, Latch* gate,
               ReaderLog* log) {
  gate->Wait();
  for (int iteration = 0; iteration < kReaderIterations; ++iteration) {
    RecordMinMax(service->Execute(*aggregate_query), "aggregate query", log);
    RecordMinMax(service->Execute(*join_query), "join query", log);
  }
}

// What the writer saw.
struct WriterLog {
  std::vector<std::string> problems;
};

// Thread body of the writer: kWriterUpdates updates of every row of t.
void RunWriter(QueryService* service, const domain::Statement* update,
               Latch* gate, WriterLog* log) {
  gate->Wait();
  for (int iteration = 0; iteration < kWriterUpdates; ++iteration) {
    domain::ExecutionResult result = service->Execute(*update);
    const domain::AffectedRows* affected = testing::AsAffectedRows(result);
    if (affected == nullptr ||
        affected->count != static_cast<std::size_t>(kRowCount)) {
      log->problems.push_back("update " + std::to_string(iteration) +
                              " did not affect every row");
      return;
    }
  }
}

// Creates t (kRowCount rows over several pages, v = 0) and u (kJoinedRowCount
// rows that point at every fourth row of t). Reports a failure and returns
// false when a statement fails.
bool FillTables(QueryService& service) {
  const char* const setup[] = {
      "CREATE TABLE t (id INT PRIMARY KEY, v INT, pad TEXT)",
      "CREATE TABLE u (id INT PRIMARY KEY, t_id INT)"};
  for (const char* sql : setup) {
    if (testing::AsError(testing::ExecuteSql(service, sql)) != nullptr) {
      testing::ReportFailure(__FILE__, __LINE__,
                             std::string("setup failed: ") + sql);
      return false;
    }
  }
  const std::string pad(120, 'x');
  constexpr int kBatchRows = 40;
  for (int first = 1; first <= kRowCount; first += kBatchRows) {
    std::string sql = "INSERT INTO t VALUES ";
    for (int id = first; id < first + kBatchRows; ++id) {
      if (id > first) {
        sql += ", ";
      }
      sql += "(" + std::to_string(id) + ", 0, '" + pad + "')";
    }
    if (testing::AsError(testing::ExecuteSql(service, sql)) != nullptr) {
      testing::ReportFailure(__FILE__, __LINE__, "insert into t failed");
      return false;
    }
  }
  std::string sql = "INSERT INTO u VALUES ";
  for (int id = 1; id <= kJoinedRowCount; ++id) {
    if (id > 1) {
      sql += ", ";
    }
    sql += "(" + std::to_string(id) + ", " + std::to_string(id * 4) + ")";
  }
  if (testing::AsError(testing::ExecuteSql(service, sql)) != nullptr) {
    testing::ReportFailure(__FILE__, __LINE__, "insert into u failed");
    return false;
  }
  return true;
}

// The MIN and MAX of t.v read after all threads finished; empty on a problem.
std::vector<std::int64_t> FinalMinMax(QueryService& service) {
  ReaderLog log;
  RecordMinMax(testing::ExecuteSql(service, "SELECT MIN(v), MAX(v) FROM t"),
               "final query", &log);
  if (!log.problems.empty()) {
    return {};
  }
  return {log.mins[0], log.maxes[0]};
}

// Runs one writer and kConcurrentReaders readers through service and checks
// what the readers saw. engine_name labels the failures.
void RunReadersAndWriter(const std::string& engine_name,
                         QueryService& service) {
  if (!FillTables(service)) {
    return;
  }
  domain::Statement aggregate_query =
      testing::MustParse("SELECT MIN(v), MAX(v) FROM t");
  domain::Statement join_query = testing::MustParse(
      "SELECT MIN(t.v), MAX(t.v) FROM t JOIN u ON t.id = u.t_id");
  domain::Statement update = testing::MustParse("UPDATE t SET v = v + 1");

  Latch gate(1);
  std::vector<ReaderLog> reader_logs(kConcurrentReaders);
  WriterLog writer_log;
  std::vector<std::thread> threads;
  threads.emplace_back(RunWriter, &service, &update, &gate, &writer_log);
  for (ReaderLog& log : reader_logs) {
    threads.emplace_back(RunReader, &service, &aggregate_query, &join_query,
                         &gate, &log);
  }
  gate.CountDown();
  JoinAll(threads);

  for (const std::string& problem : writer_log.problems) {
    testing::ReportFailure(__FILE__, __LINE__,
                           engine_name + ": writer: " + problem);
  }
  for (std::size_t reader = 0; reader < reader_logs.size(); ++reader) {
    const ReaderLog& log = reader_logs[reader];
    const std::string label =
        engine_name + ": reader " + std::to_string(reader) + ": ";
    for (const std::string& problem : log.problems) {
      testing::ReportFailure(__FILE__, __LINE__, label + problem);
    }
    // One aggregate and one join result per iteration.
    CHECK_EQ(log.mins.size(), static_cast<std::size_t>(kReaderIterations) * 2);
    for (std::size_t i = 0; i < log.mins.size(); ++i) {
      if (log.mins[i] != log.maxes[i]) {
        testing::ReportFailure(__FILE__, __LINE__,
                               label + "half-updated table: MIN " +
                                   std::to_string(log.mins[i]) + " vs MAX " +
                                   std::to_string(log.maxes[i]));
        break;
      }
    }
    // The results are in the order the reader read them; none goes down.
    for (std::size_t i = 1; i < log.mins.size(); ++i) {
      if (log.mins[i] < log.mins[i - 1]) {
        testing::ReportFailure(
            __FILE__, __LINE__,
            label + "value went down: " + std::to_string(log.mins[i - 1]) +
                " then " + std::to_string(log.mins[i]));
        break;
      }
    }
    for (std::int64_t value : log.mins) {
      if (value < 0 || value > kWriterUpdates) {
        testing::ReportFailure(
            __FILE__, __LINE__,
            label + "value out of range: " + std::to_string(value));
        break;
      }
    }
  }
  // Every update was applied exactly once, so no update was lost.
  const std::vector<std::int64_t> final_pair = FinalMinMax(service);
  if (final_pair.size() != 2) {
    testing::ReportFailure(__FILE__, __LINE__,
                           engine_name + ": the final query failed");
    return;
  }
  CHECK_EQ(final_pair[0], std::int64_t{kWriterUpdates});
  CHECK_EQ(final_pair[1], std::int64_t{kWriterUpdates});
}

}  // namespace

TEST(test_query_service_concurrent_readers_see_whole_statements) {
  // PagedEngine over an in-memory store.
  {
    paged::DatabaseOptions options;
    options.pool_frames = kPoolFrames;
    paged::PageResult<paged::Database> database =
        paged::Database::OpenWithStore(
            std::make_unique<paged::MemoryPageStore>(), options);
    if (!database.ok()) {
      testing::ReportFailure(
          __FILE__, __LINE__,
          "cannot open the paged database: " + database.error().message);
      return;
    }
    auto engine = std::make_unique<paged::PagedEngine>(
        std::move(database).value(), nullptr);
    paged::PagedEngine* const borrowed_engine = engine.get();
    QueryService service(std::move(engine));
    RunReadersAndWriter("paged", service);
    // Every statement released its pins.
    CHECK_EQ(borrowed_engine->database().buffer().PinnedFrameCount(),
             std::size_t{0});
  }
  // FlatFileEngine over a temporary directory.
  {
    testing::TempDir directory;
    std::unique_ptr<QueryService> service =
        testing::MakeLoadedService(directory.path());
    if (service == nullptr) {
      return;
    }
    RunReadersAndWriter("flatfile", *service);
  }
}

// ----------------------------------------------------------------------------
// DescribeIndex.
// ----------------------------------------------------------------------------

namespace {

// An engine that is also an index inspector. A DescribeIndex call waits inside
// until `target` calls are inside and the test has not asked to hold them; an
// Execute notes whether a DescribeIndex call was inside when it entered.
class InspectorEngine : public domain::IEngine, public domain::IIndexInspector {
 public:
  // The next calls leave once `target` are inside, and not before Release when
  // `is_held` is set.
  void Expect(int target, bool is_held) {
    std::lock_guard<std::mutex> lock(mutex_);
    target_ = target;
    is_held_ = is_held;
    peak_inside_ = 0;
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_held_ = false;
    changed_.notify_all();
  }

  // True when `count` calls are inside the inspector.
  bool WaitUntilInside(int count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kHangGuard, IsInside{this, count});
  }

  int PeakInside() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_inside_;
  }
  int WritesEntered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writes_entered_;
  }
  bool IsWriteDuringDescribe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_write_during_describe_;
  }

  const domain::ICatalog& Catalog() const override { return catalog_; }

  domain::ExecutionResult Execute(const domain::PlanNode&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inside_ > 0) {
      is_write_during_describe_ = true;
    }
    ++writes_entered_;
    return domain::AffectedRows{1};
  }

  domain::IndexDescriptionResult DescribeIndex(
      const std::string& table) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++inside_;
    peak_inside_ = std::max(peak_inside_, inside_);
    changed_.notify_all();
    changed_.wait_for(lock, kHangGuard, CanLeave{this});
    --inside_;
    changed_.notify_all();
    domain::IndexDescription description;
    description.table = table;
    description.column = "id";
    description.height = 1;
    description.node_count = 1;
    description.key_count = 0;
    description.check = "ok";
    return description;
  }

 private:
  struct IsInside {
    const InspectorEngine* engine;
    int count;
    bool operator()() const { return engine->inside_ >= count; }
  };
  struct CanLeave {
    const InspectorEngine* engine;
    bool operator()() const {
      return engine->inside_ >= engine->target_ && !engine->is_held_;
    }
  };

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  testing::FakeCatalog catalog_;
  int target_ = 1;
  bool is_held_ = false;
  int inside_ = 0;
  int peak_inside_ = 0;
  int writes_entered_ = 0;
  bool is_write_during_describe_ = false;
};

void RunDescribeIndex(QueryService* service,
                      domain::IndexDescriptionResult* out) {
  *out = service->DescribeIndex("visitors");
}

void RunExecute(QueryService* service, const domain::Statement* statement,
                domain::ExecutionResult* out) {
  *out = service->Execute(*statement);
}

bool IsDescription(const domain::IndexDescriptionResult& result) {
  return std::holds_alternative<domain::IndexDescription>(result);
}

}  // namespace

TEST(test_query_service_describe_index_takes_the_shared_lock) {
  auto owned = std::make_unique<InspectorEngine>();
  InspectorEngine* engine = owned.get();
  QueryService service(std::move(owned), nullptr, engine);

  // Two DescribeIndex calls wait inside until both are inside, so they can only
  // finish when the lock lets them in together.
  engine->Expect(2, false);
  domain::IndexDescriptionResult first;
  domain::IndexDescriptionResult second;
  std::thread first_thread(RunDescribeIndex, &service, &first);
  std::thread second_thread(RunDescribeIndex, &service, &second);
  first_thread.join();
  second_thread.join();
  CHECK_EQ(engine->PeakInside(), 2);
  CHECK(IsDescription(first) && IsDescription(second));

  // A writer statement waits outside while a DescribeIndex call is inside.
  engine->Expect(1, true);
  domain::IndexDescriptionResult held;
  std::thread reader_thread(RunDescribeIndex, &service, &held);
  CHECK(engine->WaitUntilInside(1));
  const domain::Statement insert =
      testing::MustParse("INSERT INTO visitors VALUES (9, 'zed', 'Oslo')");
  domain::ExecutionResult written = domain::AffectedRows{0};
  std::thread writer_thread(RunExecute, &service, &insert, &written);
  std::this_thread::sleep_for(kQuietWindow);
  CHECK_EQ(engine->WritesEntered(), 0);
  engine->Release();
  reader_thread.join();
  writer_thread.join();
  CHECK_EQ(engine->WritesEntered(), 1);
  CHECK(!engine->IsWriteDuringDescribe());
  CHECK(IsDescription(held));

  // A service whose engine has no inspector.
  QueryService bare(std::make_unique<InspectorEngine>());
  const domain::IndexDescriptionResult unsupported =
      bare.DescribeIndex("visitors");
  const auto* error = std::get_if<domain::ExecutionError>(&unsupported);
  CHECK(error != nullptr);
  if (error != nullptr) {
    CHECK(error->kind == domain::ErrorKind::kUnsupported);
    CHECK_EQ(error->message, std::string("the flatfile engine has no indexes"));
  }
}
