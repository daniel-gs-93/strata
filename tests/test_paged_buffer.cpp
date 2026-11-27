#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "strata/paged/buffer_manager.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/page_store.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_support.h"

// BufferManager and PageGuard over a MemoryPageStore.

namespace {

using strata::paged::BufferManager;
using strata::paged::kPageSize;
using strata::paged::LatchMode;
using strata::paged::MemoryPageStore;
using strata::paged::PageGuard;
using strata::paged::PageId;
using strata::paged::ReplacementPolicyKind;
using strata::paged::SlottedPage;
using Operation = strata::paged::MemoryPageStore::Operation;

constexpr std::size_t kMarkerOffset = 100;

// A store whose pages 0 to page_count - 1 are formatted and sealed.
void FormatPages(MemoryPageStore& store, std::size_t page_count) {
  for (std::size_t page = 0; page < page_count; ++page) {
    while (store.PageCount() <= page) {
      store.AllocatePage();
    }
    SlottedPage::Format(store.MutablePage(static_cast<PageId>(page)),
                        static_cast<PageId>(page), 1);
    SlottedPage::SealChecksum(store.MutablePage(static_cast<PageId>(page)));
  }
}

std::unique_ptr<BufferManager> MakeBuffer(
    MemoryPageStore& store, std::size_t frames,
    ReplacementPolicyKind policy = ReplacementPolicyKind::kLru) {
  auto created = BufferManager::Create(store, frames, policy);
  CHECK_OK(created);
  if (!created.ok()) {
    return nullptr;
  }
  return std::move(created).value();
}

// Fetches a page shared and releases it at once; returns "ok" or the error.
std::string FetchAndRelease(BufferManager& buffer, PageId page_id) {
  auto result = buffer.FetchPage(page_id, LatchMode::kShared);
  return strata::testing::Describe(result);
}

// Writes a marker byte into a page and marks it dirty.
void Modify(BufferManager& buffer, PageId page_id, std::uint8_t marker) {
  auto result = buffer.FetchPage(page_id, LatchMode::kExclusive);
  CHECK_OK(result);
  if (!result.ok()) {
    return;
  }
  PageGuard guard = std::move(result).value();
  guard.mutable_data()[kMarkerOffset] = marker;
  guard.MarkDirty();
}

void SharedFetchWorker(BufferManager* buffer, PageId page_id,
                       std::atomic<bool>* has_acquired) {
  auto result = buffer->FetchPage(page_id, LatchMode::kShared);
  if (result.ok()) {
    has_acquired->store(true);
  }
}

}  // namespace

TEST(test_paged_buffer_counts_hits_misses_and_evictions) {
  MemoryPageStore store;
  FormatPages(store, 10);
  CHECK_ERROR(BufferManager::Create(store, 3, ReplacementPolicyKind::kLru),
              kInvalidArgument);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  const PageId sequence[] = {0, 1, 2, 3, 0, 4, 1};
  for (PageId page : sequence) {
    CHECK_EQ(FetchAndRelease(*buffer, page), "ok");
  }
  const auto stats = buffer->Stats();
  CHECK_EQ(stats.hits, static_cast<std::uint64_t>(1));
  CHECK_EQ(stats.misses, static_cast<std::uint64_t>(6));
  CHECK_EQ(stats.evictions, static_cast<std::uint64_t>(2));
  CHECK_EQ(stats.page_reads, static_cast<std::uint64_t>(6));
  CHECK_EQ(stats.page_writes, static_cast<std::uint64_t>(0));
  CHECK_EQ(stats.dirty_writebacks, static_cast<std::uint64_t>(0));
  CHECK_EQ(stats.hits + stats.misses, static_cast<std::uint64_t>(7));
  buffer->ResetStats();
  const auto reset = buffer->Stats();
  CHECK_EQ(reset.hits + reset.misses + reset.evictions + reset.page_reads +
               reset.page_writes + reset.dirty_writebacks,
           static_cast<std::uint64_t>(0));
}

TEST(test_paged_buffer_never_evicts_pinned_frames) {
  MemoryPageStore store;
  FormatPages(store, 10);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  std::vector<PageGuard> guards;
  for (PageId page = 0; page < 4; ++page) {
    auto result = buffer->FetchPage(page, LatchMode::kShared);
    CHECK_OK(result);
    if (result.ok()) {
      guards.push_back(std::move(result).value());
    }
  }
  CHECK_EQ(guards.size(), static_cast<std::size_t>(4));
  if (guards.size() != 4) {
    return;
  }
  CHECK_ERROR(buffer->FetchPage(4, LatchMode::kShared), kNoFrame);
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(4));

  guards.back().Release();
  guards.pop_back();
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(3));
  auto fifth = buffer->FetchPage(4, LatchMode::kShared);
  CHECK_OK(fifth);
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(4));
  // The frame that was released held page 3, so pages 0 to 2 are still
  // resident (hits) and page 3 is not (a miss, and no frame to load it).
  const std::uint64_t hits_before = buffer->Stats().hits;
  for (PageId page = 0; page < 3; ++page) {
    auto again = buffer->FetchPage(page, LatchMode::kShared);
    CHECK_OK(again);
  }
  CHECK_EQ(buffer->Stats().hits, hits_before + 3);
  CHECK_ERROR(buffer->FetchPage(3, LatchMode::kShared), kNoFrame);
}

// A page that was released and then fetched again is resident and pinned: it
// must not be chosen as a victim while its guard is held.
TEST(test_paged_buffer_keeps_a_refetched_page_pinned) {
  MemoryPageStore store;
  FormatPages(store, 10);
  store.MutablePage(0)[kMarkerOffset] = 0x5A;
  SlottedPage::SealChecksum(store.MutablePage(0));
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  for (PageId page = 0; page < 4; ++page) {
    CHECK_EQ(FetchAndRelease(*buffer, page), "ok");
  }
  auto held = buffer->FetchPage(0, LatchMode::kShared);  // a hit
  CHECK_OK(held);
  if (!held.ok()) {
    return;
  }
  std::vector<PageGuard> others;
  for (PageId page = 4; page < 7; ++page) {
    auto result = buffer->FetchPage(page, LatchMode::kShared);
    CHECK_OK(result);
    if (result.ok()) {
      others.push_back(std::move(result).value());
    }
  }
  // Pages 1, 2 and 3 were evicted; page 0 is pinned, so no frame is left.
  CHECK_ERROR(buffer->FetchPage(7, LatchMode::kShared), kNoFrame);
  CHECK_EQ(static_cast<int>(held.value().data()[kMarkerOffset]), 0x5A);
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(4));
}

TEST(test_paged_buffer_writes_back_dirty_pages_on_eviction) {
  MemoryPageStore store;
  FormatPages(store, 10);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  Modify(*buffer, 0, 0xAB);
  for (PageId page = 1; page <= 4; ++page) {
    CHECK_EQ(FetchAndRelease(*buffer, page), "ok");
  }
  auto stats = buffer->Stats();
  CHECK_EQ(stats.dirty_writebacks, static_cast<std::uint64_t>(1));
  CHECK_EQ(stats.page_writes, static_cast<std::uint64_t>(1));
  CHECK_EQ(static_cast<int>(store.PageBytes(0)[kMarkerOffset]), 0xAB);
  CHECK_OK(SlottedPage::VerifyHeader(store.PageBytes(0), 0));

  auto reread = buffer->FetchPage(0, LatchMode::kShared);
  CHECK_OK(reread);
  if (reread.ok()) {
    CHECK_EQ(static_cast<int>(reread.value().data()[kMarkerOffset]), 0xAB);
  }
  stats = buffer->Stats();
  // Fetching page 0 evicted page 1, which was only read: no further write.
  CHECK_EQ(stats.page_writes, static_cast<std::uint64_t>(1));
}

TEST(test_paged_page_guard_lifetime_and_latching) {
  static_assert(!std::is_copy_constructible_v<PageGuard>);
  static_assert(!std::is_copy_assignable_v<PageGuard>);
  static_assert(std::is_move_constructible_v<PageGuard>);
  static_assert(std::is_move_assignable_v<PageGuard>);

  MemoryPageStore store;
  FormatPages(store, 10);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  {
    auto first = buffer->FetchPage(0, LatchMode::kShared);
    CHECK_OK(first);
    if (!first.ok()) {
      return;
    }
    PageGuard source = std::move(first).value();
    PageGuard moved = std::move(source);
    CHECK(!source.is_held());
    CHECK(moved.is_held());
    source.Release();  // releasing an emptied guard does nothing
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(1));
    PageGuard assigned;
    assigned = std::move(moved);
    CHECK(!moved.is_held());
    assigned.Release();
    assigned.Release();  // a second Release does nothing
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
  }
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));

  // Two shared guards on one page occupy one frame.
  {
    auto first = buffer->FetchPage(1, LatchMode::kShared);
    auto second = buffer->FetchPage(1, LatchMode::kShared);
    CHECK_OK(first);
    CHECK_OK(second);
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(1));
    if (first.ok()) {
      first.value().Release();
    }
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(1));
  }
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));

  // A shared fetch waits while another thread holds the exclusive latch.
  std::atomic<bool> has_acquired{false};
  {
    auto exclusive = buffer->FetchPage(2, LatchMode::kExclusive);
    CHECK_OK(exclusive);
    if (!exclusive.ok()) {
      return;
    }
    std::thread reader(SharedFetchWorker, buffer.get(), 2, &has_acquired);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!has_acquired.load());
    exclusive.value().Release();
    reader.join();
  }
  CHECK(has_acquired.load());
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
}

TEST(test_paged_buffer_new_page_allocates_zeroed_pages) {
  MemoryPageStore store;
  FormatPages(store, 3);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  std::vector<PageGuard> guards;
  for (PageId expected = 3; expected < 6; ++expected) {
    auto result = buffer->NewPage();
    CHECK_OK(result);
    if (!result.ok()) {
      return;
    }
    PageGuard guard = std::move(result).value();
    CHECK_EQ(guard.page_id(), expected);
    const std::uint8_t* bytes = guard.data();
    CHECK_EQ(strata::paged::ReadLe32(bytes + 4), expected);
    CHECK_EQ(static_cast<int>(bytes[8]), 0);
    bool is_rest_zero = true;
    for (std::size_t i = 0; i < kPageSize; ++i) {
      if ((i < 4 || i >= 8) && bytes[i] != 0) {
        is_rest_zero = false;
      }
    }
    CHECK(is_rest_zero);
    SlottedPage::Format(guard.mutable_data(), expected, 1);
    guard.MarkDirty();
    guards.push_back(std::move(guard));
  }
  guards.clear();
  CHECK_OK(buffer->FlushAll());
  for (PageId page = 3; page < 6; ++page) {
    CHECK_OK(SlottedPage::VerifyHeader(store.PageBytes(page), page));
    CHECK_EQ(static_cast<int>(store.PageBytes(page)[8]), 2);
  }
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
}

TEST(test_paged_buffer_flush_all_writes_each_dirty_page_once) {
  MemoryPageStore store;
  FormatPages(store, 10);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  for (PageId page = 0; page < 3; ++page) {
    Modify(*buffer, page, 0x11);
  }
  const int syncs_before = store.CallCount(Operation::kSync);
  CHECK_OK(buffer->FlushAll());
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(3));
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_before + 1);
  CHECK_OK(buffer->FlushAll());
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(3));
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_before + 2);
  for (PageId page = 0; page < 3; ++page) {
    CHECK_OK(SlottedPage::VerifyHeader(store.PageBytes(page), page));
    CHECK_EQ(static_cast<int>(store.PageBytes(page)[kMarkerOffset]), 0x11);
  }
}

TEST(test_paged_buffer_write_back_all_writes_without_sync) {
  MemoryPageStore store;
  FormatPages(store, 10);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  for (PageId page = 0; page < 3; ++page) {
    Modify(*buffer, page, 0x11);
  }
  const int syncs_before = store.CallCount(Operation::kSync);
  CHECK_OK(buffer->WriteBackAll());
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(3));
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_before);
  // The pages are clean now: FlushAll writes nothing and only syncs.
  CHECK_OK(buffer->FlushAll());
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(3));
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_before + 1);
  for (PageId page = 0; page < 3; ++page) {
    CHECK_OK(SlottedPage::VerifyHeader(store.PageBytes(page), page));
  }

  // A failed write leaves the remaining pages dirty and skips the Sync; the
  // next WriteBackAll writes them.
  for (PageId page = 3; page < 6; ++page) {
    Modify(*buffer, page, 0x22);
  }
  const int syncs_mid = store.CallCount(Operation::kSync);
  store.InjectFailure(Operation::kWrite, 2);
  CHECK_ERROR(buffer->WriteBackAll(), kIo);
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_mid);
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(4));
  CHECK_OK(buffer->WriteBackAll());
  CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(6));
  CHECK_EQ(store.CallCount(Operation::kSync), syncs_mid);
  for (PageId page = 3; page < 6; ++page) {
    CHECK_OK(SlottedPage::VerifyHeader(store.PageBytes(page), page));
    CHECK_EQ(static_cast<int>(store.PageBytes(page)[kMarkerOffset]), 0x22);
  }
}

TEST(test_paged_buffer_rejects_corrupt_pages_without_leaking_frames) {
  MemoryPageStore store;
  FormatPages(store, 10);
  store.MutablePage(5)[200] ^= 0x01;  // CRC no longer matches
  strata::paged::WriteLe32(store.MutablePage(6) + 4, 9);  // wrong page id
  SlottedPage::SealChecksum(store.MutablePage(6));
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
  if (buffer == nullptr) {
    return;
  }
  auto first = buffer->FetchPage(5, LatchMode::kShared);
  CHECK_ERROR(first, kCorrupt);
  CHECK_MESSAGE_CONTAINS(first, "page 5");
  auto second = buffer->FetchPage(6, LatchMode::kShared);
  CHECK_ERROR(second, kCorrupt);
  CHECK_MESSAGE_CONTAINS(second, "page 6");
  CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
  std::vector<PageGuard> guards;
  for (PageId page = 0; page < 4; ++page) {
    auto result = buffer->FetchPage(page, LatchMode::kShared);
    CHECK_OK(result);
    if (result.ok()) {
      guards.push_back(std::move(result).value());
    }
  }
  CHECK_EQ(guards.size(), static_cast<std::size_t>(4));
}

TEST(test_paged_buffer_propagates_io_failures_without_losing_data) {
  // (a) A read fails: no pin is left, and the next fetch of the page works.
  {
    MemoryPageStore store;
    FormatPages(store, 10);
    std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
    if (buffer == nullptr) {
      return;
    }
    store.InjectFailure(Operation::kRead, 1);
    auto failed_read = buffer->FetchPage(3, LatchMode::kShared);
    CHECK_ERROR(failed_read, kIo);
    CHECK_MESSAGE_CONTAINS(failed_read, "page 3");
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
    CHECK_EQ(FetchAndRelease(*buffer, 3), "ok");
    const auto stats = buffer->Stats();
    CHECK_EQ(stats.hits + stats.misses, static_cast<std::uint64_t>(2));
    // The frame of the failed read went back to the pool: all four frames are
    // still usable, so four other pages can be pinned at once.
    std::vector<PageGuard> guards;
    for (PageId page = 4; page < 8; ++page) {
      auto result = buffer->FetchPage(page, LatchMode::kShared);
      CHECK_OK(result);
      if (result.ok()) {
        guards.push_back(std::move(result).value());
      }
    }
    CHECK_EQ(guards.size(), static_cast<std::size_t>(4));
  }
  // (b) The write-back of a dirty victim fails: the victim stays resident and
  // dirty, no eviction is counted, and nothing is lost.
  {
    MemoryPageStore store;
    FormatPages(store, 10);
    std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
    if (buffer == nullptr) {
      return;
    }
    Modify(*buffer, 0, 0xCD);
    for (PageId page = 1; page <= 3; ++page) {
      CHECK_EQ(FetchAndRelease(*buffer, page), "ok");
    }
    // The pool holds pages 0 to 3 and page 0 is the least recently used.
    store.InjectFailure(Operation::kWrite, 1);
    auto failed_write = buffer->FetchPage(4, LatchMode::kShared);
    CHECK_ERROR(failed_write, kIo);
    CHECK_MESSAGE_CONTAINS(failed_write, "page 0");
    CHECK_EQ(buffer->Stats().evictions, static_cast<std::uint64_t>(0));
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
    const std::uint64_t hits_before = buffer->Stats().hits;
    CHECK_EQ(FetchAndRelease(*buffer, 0), "ok");  // still resident: a hit
    CHECK_EQ(buffer->Stats().hits, hits_before + 1);
    store.ClearInjectedFailures();
    CHECK_OK(buffer->FlushAll());
    CHECK_EQ(static_cast<int>(store.PageBytes(0)[kMarkerOffset]), 0xCD);
    // Calls: Modify, pages 1 to 3, the failed fetch, the hit.
    const auto stats = buffer->Stats();
    CHECK_EQ(stats.hits + stats.misses, static_cast<std::uint64_t>(6));
  }
}

TEST(test_paged_buffer_reports_allocate_and_sync_failures) {
  // An allocation failure leaves no frame occupied.
  {
    MemoryPageStore store;
    FormatPages(store, 6);
    std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
    if (buffer == nullptr) {
      return;
    }
    store.InjectFailure(Operation::kAllocate, 1);
    CHECK_ERROR(buffer->NewPage(), kIo);
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
    std::vector<PageGuard> guards;
    for (PageId page = 0; page < 4; ++page) {
      auto result = buffer->FetchPage(page, LatchMode::kShared);
      CHECK_OK(result);
      if (result.ok()) {
        guards.push_back(std::move(result).value());
      }
    }
    CHECK_EQ(guards.size(), static_cast<std::size_t>(4));
  }
  // A sync failure is reported after the dirty page was written.
  {
    MemoryPageStore store;
    FormatPages(store, 6);
    std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
    if (buffer == nullptr) {
      return;
    }
    Modify(*buffer, 1, 0x77);
    store.InjectFailure(Operation::kSync, 1);
    CHECK_ERROR(buffer->FlushAll(), kIo);
    CHECK_EQ(static_cast<int>(store.PageBytes(1)[kMarkerOffset]), 0x77);
    CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(1));
    CHECK_OK(buffer->FlushAll());
    CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(1));
  }
  // A write failure stops FlushAll at the first error; the page written
  // before it stays clean and the others stay dirty.
  {
    MemoryPageStore store;
    FormatPages(store, 6);
    std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 4);
    if (buffer == nullptr) {
      return;
    }
    for (PageId page = 0; page < 3; ++page) {
      Modify(*buffer, page, 0x33);
    }
    store.InjectFailure(Operation::kWrite, 2);
    CHECK_ERROR(buffer->FlushAll(), kIo);
    CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(1));
    CHECK_EQ(buffer->PinnedFrameCount(), static_cast<std::size_t>(0));
    store.ClearInjectedFailures();
    CHECK_OK(buffer->FlushAll());
    CHECK_EQ(buffer->Stats().page_writes, static_cast<std::uint64_t>(3));
    for (PageId page = 0; page < 3; ++page) {
      CHECK_EQ(static_cast<int>(store.PageBytes(page)[kMarkerOffset]), 0x33);
    }
  }
}

TEST(test_paged_buffer_frame_latches_occupy_separate_cache_lines) {
  static_assert(alignof(BufferManager::FrameLatch) == 128);
  static_assert(sizeof(BufferManager::FrameLatch) % 128 == 0);
  MemoryPageStore store;
  FormatPages(store, 16);
  std::unique_ptr<BufferManager> buffer = MakeBuffer(store, 16);
  if (buffer == nullptr) {
    return;
  }
  std::uintptr_t previous = 0;
  for (strata::paged::FrameId frame = 0; frame < 16; ++frame) {
    const auto address =
        reinterpret_cast<std::uintptr_t>(buffer->FrameLatchAddress(frame));
    CHECK_EQ(address % 128, static_cast<std::uintptr_t>(0));
    if (frame > 0) {
      CHECK(address - previous >= 128);
    }
    previous = address;
  }
  // Frame memory starts on a 4096-byte boundary.
  std::vector<PageGuard> guards;
  for (PageId page = 0; page < 16; ++page) {
    auto result = buffer->FetchPage(page, LatchMode::kShared);
    CHECK_OK(result);
    if (!result.ok()) {
      return;
    }
    const auto address =
        reinterpret_cast<std::uintptr_t>(result.value().data());
    CHECK_EQ(address % 4096, static_cast<std::uintptr_t>(0));
    guards.push_back(std::move(result).value());
  }
}
