#ifndef STRATA_PAGED_BUFFER_MANAGER_H_
#define STRATA_PAGED_BUFFER_MANAGER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "strata/paged/page_store.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/replacement_policy.h"

// The buffer pool of the paged engine (design document 2.12, ADR-0028): a fixed
// number of frames, each holding one page, in front of an IPageStore.
//
// Synchronization. A pool mutex protects the page table, the frame metadata
// (page id, pin count, dirty flag), the free list, the replacement policy, the
// counters, and every call into the IPageStore. A shared_mutex per frame (its
// latch) protects the bytes of the page in the frame. The lock order is latch
// first, pool mutex second: a thread that holds a latch may take the pool
// mutex, and a thread that holds the pool mutex never waits for a latch,
// because a frame it evicts has no pin and so no guard holds its latch. This
// prevents data races on the metadata and deadlock between the two kinds of
// lock. A miss does its disk I/O while holding the pool mutex, so misses
// serialize each other; hits wait only for the short critical sections.

namespace strata::paged {

struct BufferStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t dirty_writebacks = 0;
  std::uint64_t page_reads = 0;
  std::uint64_t page_writes = 0;
};

class BufferManager;

// One pin and one latch on a frame, released when the guard is destroyed: the
// latch first, then the pin. A guard cannot be copied; a moved-from guard is
// empty and releases nothing. It holds a reference to the BufferManager that
// issued it, which must outlive every guard, and must be released by the
// thread that acquired it (std::shared_mutex requires the same thread to
// unlock). The page bytes are valid only while the guard is held.
class PageGuard {
 public:
  PageGuard() = default;
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  ~PageGuard();

  bool is_held() const { return buffer_ != nullptr; }
  PageId page_id() const { return page_id_; }
  LatchMode mode() const { return mode_; }

  // The kPageSize bytes of the page. A shared guard must not write through
  // mutable_data().
  const std::uint8_t* data() const { return data_; }
  // Precondition: the guard holds the exclusive latch.
  std::uint8_t* mutable_data() const;

  // Records that the page was changed; the frame becomes dirty when the guard
  // is released. Precondition: the guard holds the exclusive latch.
  void MarkDirty();

  // Releases the latch, then the pin. Does nothing on an empty guard.
  void Release();

 private:
  friend class BufferManager;
  PageGuard(BufferManager* buffer, FrameId frame, PageId page_id,
            std::shared_lock<std::shared_mutex> latch, std::uint8_t* data);
  PageGuard(BufferManager* buffer, FrameId frame, PageId page_id,
            std::unique_lock<std::shared_mutex> latch, std::uint8_t* data);

  BufferManager* buffer_ = nullptr;
  FrameId frame_ = 0;
  PageId page_id_ = kInvalidPageId;
  LatchMode mode_ = LatchMode::kShared;
  std::uint8_t* data_ = nullptr;
  bool is_dirty_ = false;
  std::shared_lock<std::shared_mutex> shared_latch_;
  std::unique_lock<std::shared_mutex> exclusive_latch_;
};

class BufferManager {
 public:
  // One latch per frame, aligned to a cache line so that two frames' latches
  // never share one (false sharing, ADR-0034).
  struct alignas(kCacheLineSize) FrameLatch {
    std::shared_mutex mutex;
  };

  // A pool of `frames` frames over store, with the given replacement policy.
  // store is not owned and must outlive the BufferManager. frames must be at
  // least 4 (kInvalidArgument): at most four latches are held at once. A table
  // heap append holds three (the chain tail, the file header and the new page),
  // and a fourth frame must be free or evictable to read the new page. An index
  // insert that splits holds four (the parent, the child, the file header and
  // the new page), which uses every frame of a pool of 4. Frame memory is one
  // block aligned to kPageSize.
  static PageResult<std::unique_ptr<BufferManager>> Create(
      IPageStore& store, std::size_t frames, ReplacementPolicyKind policy);

  ~BufferManager();
  BufferManager(const BufferManager&) = delete;
  BufferManager& operator=(const BufferManager&) = delete;

  // Pins page_id, verifies its header when it is read, and returns a guard
  // holding the latch in the given mode. kNoFrame: every frame is pinned (no
  // waiting). kNotFound, kIo: from the store. kCorrupt: the page failed
  // SlottedPage::VerifyHeader. After an error nothing stays pinned.
  PageResult<PageGuard> FetchPage(PageId page_id, LatchMode mode);

  // Appends a page to the store and returns an exclusive guard on a frame of
  // zeros whose header has only page_id set; the frame is dirty. The caller
  // formats the page. Errors as FetchPage; nothing stays pinned.
  PageResult<PageGuard> NewPage();

  // Writes every dirty frame and does not call Sync. The caller must not hold
  // a guard (it waits for the latches). Stops at the first error; frames
  // written before it stay clean. Not safe to run concurrently with another
  // WriteBackAll or FlushAll.
  Status WriteBackAll();

  // WriteBackAll, then Sync. A failed WriteBackAll skips the Sync. The same
  // preconditions as WriteBackAll.
  Status FlushAll();

  BufferStats Stats() const;
  void ResetStats();

  // The number of frames whose pin count is above 0. For tests: after any
  // sequence of operations with no guard alive it is 0.
  std::size_t PinnedFrameCount() const;

  std::size_t frame_count() const { return frame_count_; }

  // The page count of the store.
  PageId PageCount() const;

  // The address of a frame's latch. For tests that check the memory layout.
  const void* FrameLatchAddress(FrameId frame) const;

 private:
  friend class PageGuard;

  struct FrameMeta {
    PageId page_id = kInvalidPageId;
    std::uint32_t pin_count = 0;
    bool is_dirty = false;
  };

  struct AlignedFrameDeleter {
    void operator()(std::uint8_t* frames) const;
  };

  BufferManager(IPageStore& store, std::size_t frame_count,
                ReplacementPolicyKind policy_kind,
                std::unique_ptr<std::uint8_t, AlignedFrameDeleter> frames,
                std::unique_ptr<FrameLatch[]> latches);

  std::uint8_t* FrameData(FrameId frame) const;

  // The following need pool_mutex_.
  // Pins page_id and returns its frame (hit, or miss with a read).
  PageResult<FrameId> PinPage(PageId page_id);
  // A frame holding no page: a free frame, or an evicted one (written back
  // first when dirty). kNoFrame when every frame is pinned.
  PageResult<FrameId> AcquireFrame();
  void UnpinLocked(FrameId frame);

  // Called by PageGuard::Release after the latch is released.
  void ReleasePin(FrameId frame, bool is_dirty);

  IPageStore& store_;
  const std::size_t frame_count_;
  std::unique_ptr<std::uint8_t, AlignedFrameDeleter> frames_;
  std::unique_ptr<FrameLatch[]> latches_;

  mutable std::mutex pool_mutex_;
  // Page id to frame: lookup, insert and erase are O(1) on average.
  std::unordered_map<PageId, FrameId> page_table_;
  std::vector<FrameMeta> metas_;
  // Frames with no page; take and return are O(1).
  std::vector<FrameId> free_frames_;
  std::unique_ptr<IReplacementPolicy> policy_;
  BufferStats stats_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_BUFFER_MANAGER_H_
