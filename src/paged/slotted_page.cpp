#include "strata/paged/slotted_page.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "strata/paged/byte_codec.h"
#include "strata/paged/crc32c.h"

namespace strata::paged {

namespace {

std::size_t SlotCountOf(const std::uint8_t* page) {
  return ReadLe16(page + layout::kSlotCount);
}
std::size_t FreeEndOf(const std::uint8_t* page) {
  return ReadLe16(page + layout::kFreeEnd);
}
std::size_t DeadBytesOf(const std::uint8_t* page) {
  return ReadLe16(page + layout::kDeadBytes);
}
std::size_t FreeStartOf(const std::uint8_t* page) {
  return kPageHeaderSize + kSlotSize * SlotCountOf(page);
}
std::size_t SlotPosition(std::size_t slot_id) {
  return kPageHeaderSize + kSlotSize * slot_id;
}
std::uint16_t SlotOffsetOf(const std::uint8_t* page, std::size_t slot_id) {
  return ReadLe16(page + SlotPosition(slot_id));
}
std::uint16_t SlotLengthOf(const std::uint8_t* page, std::size_t slot_id) {
  return ReadLe16(page + SlotPosition(slot_id) + 2);
}
void SetSlot(std::uint8_t* page, std::size_t slot_id, std::uint16_t offset,
             std::uint16_t length) {
  WriteLe16(page + SlotPosition(slot_id), offset);
  WriteLe16(page + SlotPosition(slot_id) + 2, length);
}
bool IsDeadSlot(std::uint16_t offset, std::uint16_t length) {
  return offset == 0 && length == 0;
}

PageError Corrupt(const std::uint8_t* page, const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt,
                   "page " +
                       std::to_string(ReadLe32(page + layout::kPageIdField)) +
                       ": " + detail);
}

// A live slot's tuple must lie inside [free_end, kPageSize) and be non-empty.
bool IsLiveSlotInBounds(const std::uint8_t* page, std::uint16_t offset,
                        std::uint16_t length) {
  return length >= 1 && offset >= FreeEndOf(page) &&
         static_cast<std::size_t>(offset) + length <= kPageSize;
}

// The lowest-numbered dead slot, or no value when every slot is live.
std::optional<std::uint16_t> LowestDeadSlot(const std::uint8_t* page) {
  const std::size_t slot_count = SlotCountOf(page);
  for (std::size_t slot_id = 0; slot_id < slot_count; ++slot_id) {
    if (IsDeadSlot(SlotOffsetOf(page, slot_id), SlotLengthOf(page, slot_id))) {
      return static_cast<std::uint16_t>(slot_id);
    }
  }
  return std::nullopt;
}

bool IsAllZero(const std::uint8_t* page) {
  for (std::size_t i = 0; i < kPageSize; ++i) {
    if (page[i] != 0) {
      return false;
    }
  }
  return true;
}

struct LiveSlot {
  std::uint16_t slot_id;
  std::uint16_t offset;
  std::uint16_t length;
};

// Compaction moves the tuple nearest the end first, so no move overwrites a
// tuple that has not moved yet.
bool OffsetGreater(const LiveSlot& left, const LiveSlot& right) {
  return left.offset > right.offset;
}

}  // namespace

PageId SlottedPageView::page_id() const {
  return ReadLe32(page_ + layout::kPageIdField);
}
PageType SlottedPageView::page_type() const {
  return static_cast<PageType>(page_[layout::kPageTypeField]);
}
std::uint16_t SlottedPageView::slot_count() const {
  return ReadLe16(page_ + layout::kSlotCount);
}
std::uint16_t SlottedPageView::free_end() const {
  return ReadLe16(page_ + layout::kFreeEnd);
}
std::uint16_t SlottedPageView::dead_bytes() const {
  return ReadLe16(page_ + layout::kDeadBytes);
}
PageId SlottedPageView::next_page_id() const {
  return ReadLe32(page_ + layout::kNextPageId);
}
std::uint32_t SlottedPageView::table_id() const {
  return ReadLe32(page_ + layout::kTableId);
}
std::uint64_t SlottedPageView::lsn() const {
  return ReadLe64(page_ + layout::kLsn);
}

std::uint16_t SlottedPageView::InsertCapacity() const {
  const std::size_t free_start = FreeStartOf(page_);
  const std::size_t tuple_start = FreeEndOf(page_);
  if (tuple_start < free_start || tuple_start > kPageSize) {
    return 0;
  }
  std::size_t capacity = (tuple_start - free_start) + DeadBytesOf(page_);
  if (!LowestDeadSlot(page_).has_value()) {
    capacity = capacity >= kSlotSize ? capacity - kSlotSize : 0;
  }
  return static_cast<std::uint16_t>(std::min(capacity, kMaxTupleSize));
}

PageResult<ByteView> SlottedPageView::Get(std::uint16_t slot_id) const {
  const std::size_t slot_count = SlotCountOf(page_);
  if (slot_id >= slot_count) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id()) + ": slot " +
                         std::to_string(slot_id) +
                         " is out of range (slot_count " +
                         std::to_string(slot_count) + ")");
  }
  if (SlotPosition(slot_id) + kSlotSize > kPageSize) {
    return Corrupt(
        page_, "slot " + std::to_string(slot_id) + " lies outside the page");
  }
  const std::uint16_t offset = SlotOffsetOf(page_, slot_id);
  const std::uint16_t length = SlotLengthOf(page_, slot_id);
  if (IsDeadSlot(offset, length)) {
    return MakeError(PageErrorCode::kNotFound,
                     "page " + std::to_string(page_id()) + ": slot " +
                         std::to_string(slot_id) + " is dead");
  }
  if (!IsLiveSlotInBounds(page_, offset, length)) {
    return Corrupt(page_, "slot " + std::to_string(slot_id) + " has offset " +
                              std::to_string(offset) + " and length " +
                              std::to_string(length) +
                              " outside the tuple area");
  }
  return ByteView{page_ + offset, length};
}

Status SlottedPageView::CheckStructure() const {
  if (page_type() != PageType::kHeapData) {
    return Corrupt(page_, "page_type is not heap data");
  }
  const std::size_t free_start = FreeStartOf(page_);
  const std::size_t tuple_start = FreeEndOf(page_);
  if (free_start > tuple_start || tuple_start > kPageSize) {
    return Corrupt(page_, "free_start " + std::to_string(free_start) +
                              " and free_end " + std::to_string(tuple_start) +
                              " are not ordered inside the page");
  }
  const std::size_t slot_count = SlotCountOf(page_);
  std::vector<std::pair<std::uint16_t, std::uint16_t>> live_tuples;
  std::size_t live_bytes = 0;
  for (std::size_t slot_id = 0; slot_id < slot_count; ++slot_id) {
    const std::uint16_t offset = SlotOffsetOf(page_, slot_id);
    const std::uint16_t length = SlotLengthOf(page_, slot_id);
    if (IsDeadSlot(offset, length)) {
      continue;
    }
    if (!IsLiveSlotInBounds(page_, offset, length)) {
      return Corrupt(page_, "slot " + std::to_string(slot_id) + " has offset " +
                                std::to_string(offset) + " and length " +
                                std::to_string(length) +
                                " outside the tuple area");
    }
    live_tuples.emplace_back(offset, length);
    live_bytes += length;
  }
  std::ranges::sort(live_tuples);
  for (std::size_t i = 1; i < live_tuples.size(); ++i) {
    const std::size_t previous_end =
        static_cast<std::size_t>(live_tuples[i - 1].first) +
        live_tuples[i - 1].second;
    if (live_tuples[i].first < previous_end) {
      return Corrupt(page_, "tuples at offsets " +
                                std::to_string(live_tuples[i - 1].first) +
                                " and " + std::to_string(live_tuples[i].first) +
                                " overlap");
    }
  }
  const std::size_t tuple_area = kPageSize - tuple_start;
  if (live_bytes + DeadBytesOf(page_) != tuple_area) {
    return Corrupt(
        page_, "live bytes " + std::to_string(live_bytes) +
                   " plus dead_bytes " + std::to_string(DeadBytesOf(page_)) +
                   " differ from the tuple area " + std::to_string(tuple_area));
  }
  return std::nullopt;
}

void SlottedPage::Format(std::uint8_t* page, PageId page_id,
                         std::uint32_t table_id) {
  std::memset(page, 0, kPageSize);
  WriteLe32(page + layout::kPageIdField, page_id);
  page[layout::kPageTypeField] = static_cast<std::uint8_t>(PageType::kHeapData);
  WriteLe16(page + layout::kSlotCount, 0);
  WriteLe16(page + layout::kFreeEnd, static_cast<std::uint16_t>(kPageSize));
  WriteLe16(page + layout::kDeadBytes, 0);
  WriteLe32(page + layout::kNextPageId, kInvalidPageId);
  WriteLe32(page + layout::kTableId, table_id);
  WriteLe64(page + layout::kLsn, 0);
}

void SlottedPage::FormatFree(std::uint8_t* page, PageId page_id,
                             PageId next_page_id) {
  std::memset(page, 0, kPageSize);
  WriteLe32(page + layout::kPageIdField, page_id);
  page[layout::kPageTypeField] = static_cast<std::uint8_t>(PageType::kFree);
  WriteLe16(page + layout::kFreeEnd, static_cast<std::uint16_t>(kPageSize));
  WriteLe32(page + layout::kNextPageId, next_page_id);
}

void SlottedPage::SealChecksum(std::uint8_t* page) {
  WriteLe32(page + layout::kChecksum, Crc32c(page + layout::kPageIdField,
                                             kPageSize - layout::kPageIdField));
}

Status SlottedPage::VerifyHeader(const std::uint8_t* page,
                                 PageId expected_page_id) {
  const std::string prefix = "page " + std::to_string(expected_page_id) + ": ";
  if (IsAllZero(page)) {
    return std::nullopt;
  }
  const std::uint32_t stored = ReadLe32(page + layout::kChecksum);
  const std::uint32_t computed =
      Crc32c(page + layout::kPageIdField, kPageSize - layout::kPageIdField);
  if (stored != computed) {
    return MakeError(PageErrorCode::kCorrupt,
                     prefix + "checksum mismatch (stored " +
                         std::to_string(stored) + ", computed " +
                         std::to_string(computed) + ")");
  }
  const PageId header_page_id = ReadLe32(page + layout::kPageIdField);
  if (header_page_id != expected_page_id) {
    return MakeError(
        PageErrorCode::kCorrupt,
        prefix + "header page_id is " + std::to_string(header_page_id));
  }
  const std::uint8_t page_type = page[layout::kPageTypeField];
  if (page_type > static_cast<std::uint8_t>(PageType::kIndexNode)) {
    return MakeError(
        PageErrorCode::kCorrupt,
        prefix + "page_type " + std::to_string(page_type) + " is not 0 to 4");
  }
  if (page_type == static_cast<std::uint8_t>(PageType::kIndexNode)) {
    // An index node has no slots and no tuple area: the three heap fields stay
    // zero (design document 2.10).
    if (SlotCountOf(page) != 0 || FreeEndOf(page) != 0 ||
        DeadBytesOf(page) != 0) {
      return MakeError(
          PageErrorCode::kCorrupt,
          prefix + "an index node has slot_count " +
              std::to_string(SlotCountOf(page)) + ", free_end " +
              std::to_string(FreeEndOf(page)) + " and dead_bytes " +
              std::to_string(DeadBytesOf(page)) + " instead of 0, 0 and 0");
    }
  }
  if (page_type == static_cast<std::uint8_t>(PageType::kHeapData) ||
      page_type == static_cast<std::uint8_t>(PageType::kFree)) {
    const std::size_t free_start = FreeStartOf(page);
    const std::size_t tuple_start = FreeEndOf(page);
    if (free_start > tuple_start || tuple_start > kPageSize) {
      return MakeError(PageErrorCode::kCorrupt,
                       prefix + "free_start " + std::to_string(free_start) +
                           " and free_end " + std::to_string(tuple_start) +
                           " are not ordered inside the page");
    }
  }
  return std::nullopt;
}

void SlottedPage::set_next_page_id(PageId next_page_id) {
  WriteLe32(page_ + layout::kNextPageId, next_page_id);
}

PageResult<std::uint16_t> SlottedPage::Insert(ByteView tuple) {
  if (tuple.size == 0) {
    return MakeError(PageErrorCode::kInvalidArgument, "tuple is empty");
  }
  if (tuple.size > kMaxTupleSize) {
    return MakeError(PageErrorCode::kTooLarge,
                     "tuple has " + std::to_string(tuple.size) +
                         " bytes; the limit is " +
                         std::to_string(kMaxTupleSize));
  }
  std::optional<std::uint16_t> reusable_slot = LowestDeadSlot(page_);
  std::size_t slot_cost = reusable_slot.has_value() ? 0 : kSlotSize;
  std::size_t contiguous = FreeEndOf(page_) - FreeStartOf(page_);
  if (contiguous < tuple.size + slot_cost) {
    if (contiguous + DeadBytesOf(page_) < tuple.size + slot_cost) {
      return MakeError(PageErrorCode::kNoSpace,
                       "page " + std::to_string(page_id()) +
                           " has no room for " + std::to_string(tuple.size) +
                           " bytes");
    }
    if (Status status = CompactKeepingSlots(/*is_trimming_dead_slots=*/true)) {
      return *status;
    }
    reusable_slot = LowestDeadSlot(page_);
    slot_cost = reusable_slot.has_value() ? 0 : kSlotSize;
    contiguous = FreeEndOf(page_) - FreeStartOf(page_);
    if (contiguous < tuple.size + slot_cost) {
      return MakeError(PageErrorCode::kNoSpace,
                       "page " + std::to_string(page_id()) +
                           " has no room after compaction");
    }
  }
  const std::size_t tuple_start = FreeEndOf(page_) - tuple.size;
  std::memcpy(page_ + tuple_start, tuple.data, tuple.size);
  std::uint16_t slot_id = 0;
  if (reusable_slot.has_value()) {
    slot_id = *reusable_slot;
  } else {
    slot_id = static_cast<std::uint16_t>(SlotCountOf(page_));
    WriteLe16(page_ + layout::kSlotCount,
              static_cast<std::uint16_t>(SlotCountOf(page_) + 1));
  }
  SetSlot(page_, slot_id, static_cast<std::uint16_t>(tuple_start),
          static_cast<std::uint16_t>(tuple.size));
  WriteLe16(page_ + layout::kFreeEnd, static_cast<std::uint16_t>(tuple_start));
  return slot_id;
}

Status SlottedPage::Delete(std::uint16_t slot_id) {
  PageResult<ByteView> existing = Get(slot_id);
  if (!existing.ok()) {
    return existing.error();
  }
  const std::uint16_t length = SlotLengthOf(page_, slot_id);
  WriteLe16(page_ + layout::kDeadBytes,
            static_cast<std::uint16_t>(DeadBytesOf(page_) + length));
  SetSlot(page_, slot_id, 0, 0);
  return std::nullopt;
}

Status SlottedPage::Update(std::uint16_t slot_id, ByteView tuple) {
  if (tuple.size == 0) {
    return MakeError(PageErrorCode::kInvalidArgument, "tuple is empty");
  }
  if (tuple.size > kMaxTupleSize) {
    return MakeError(PageErrorCode::kTooLarge,
                     "tuple has " + std::to_string(tuple.size) +
                         " bytes; the limit is " +
                         std::to_string(kMaxTupleSize));
  }
  PageResult<ByteView> existing = Get(slot_id);
  if (!existing.ok()) {
    return existing.error();
  }
  const std::size_t old_length = existing.value().size;
  const std::uint16_t old_offset = SlotOffsetOf(page_, slot_id);

  if (tuple.size <= old_length) {
    std::memcpy(page_ + old_offset, tuple.data, tuple.size);
    SetSlot(page_, slot_id, old_offset, static_cast<std::uint16_t>(tuple.size));
    WriteLe16(page_ + layout::kDeadBytes,
              static_cast<std::uint16_t>(DeadBytesOf(page_) + old_length -
                                         tuple.size));
    return std::nullopt;
  }

  const std::size_t contiguous = FreeEndOf(page_) - FreeStartOf(page_);
  if (contiguous + DeadBytesOf(page_) + old_length < tuple.size) {
    return MakeError(PageErrorCode::kNoSpace,
                     "page " + std::to_string(page_id()) +
                         " has no room to grow slot " +
                         std::to_string(slot_id) + " to " +
                         std::to_string(tuple.size) + " bytes");
  }
  if (contiguous >= tuple.size) {
    const std::size_t tuple_start = FreeEndOf(page_) - tuple.size;
    std::memcpy(page_ + tuple_start, tuple.data, tuple.size);
    WriteLe16(page_ + layout::kDeadBytes,
              static_cast<std::uint16_t>(DeadBytesOf(page_) + old_length));
    SetSlot(page_, slot_id, static_cast<std::uint16_t>(tuple_start),
            static_cast<std::uint16_t>(tuple.size));
    WriteLe16(page_ + layout::kFreeEnd,
              static_cast<std::uint16_t>(tuple_start));
    return std::nullopt;
  }
  // The new tuple fits only after compaction. The structure is checked before
  // the slot is released, so a corrupt page keeps the old row. Compaction
  // moves bytes, so the new tuple is staged first; the slot is released for
  // the compaction and keeps its number because dead slots at the end are not
  // removed here.
  if (Status status = view().CheckStructure()) {
    return status;
  }
  const std::vector<std::uint8_t> staged(tuple.data, tuple.data + tuple.size);
  WriteLe16(page_ + layout::kDeadBytes,
            static_cast<std::uint16_t>(DeadBytesOf(page_) + old_length));
  SetSlot(page_, slot_id, 0, 0);
  if (Status status = CompactKeepingSlots(/*is_trimming_dead_slots=*/false)) {
    return status;
  }
  const std::size_t tuple_start = FreeEndOf(page_) - staged.size();
  std::memcpy(page_ + tuple_start, staged.data(), staged.size());
  SetSlot(page_, slot_id, static_cast<std::uint16_t>(tuple_start),
          static_cast<std::uint16_t>(staged.size()));
  WriteLe16(page_ + layout::kFreeEnd, static_cast<std::uint16_t>(tuple_start));
  return std::nullopt;
}

Status SlottedPage::Compact() {
  return CompactKeepingSlots(/*is_trimming_dead_slots=*/true);
}

// A page read from the file passes its CRC even when its header and slots were
// written inconsistently. Moving tuples by lengths that do not add up would
// write outside the tuple area, so the whole structure is checked before any
// byte moves. With live tuples inside [free_end, kPageSize), not overlapping,
// the packed tuples start at or above free_end, which is at or above
// free_start.
Status SlottedPage::CompactKeepingSlots(bool is_trimming_dead_slots) {
  if (Status status = view().CheckStructure()) {
    return status;
  }
  const std::size_t slot_count = SlotCountOf(page_);
  std::vector<LiveSlot> live_slots;
  for (std::size_t slot_id = 0; slot_id < slot_count; ++slot_id) {
    const std::uint16_t offset = SlotOffsetOf(page_, slot_id);
    const std::uint16_t length = SlotLengthOf(page_, slot_id);
    if (IsDeadSlot(offset, length)) {
      continue;
    }
    live_slots.push_back(
        LiveSlot{static_cast<std::uint16_t>(slot_id), offset, length});
  }
  std::ranges::sort(live_slots, OffsetGreater);
  std::size_t new_end = kPageSize;
  for (const LiveSlot& live : live_slots) {
    new_end -= live.length;
    if (new_end != live.offset) {
      std::memmove(page_ + new_end, page_ + live.offset, live.length);
    }
    SetSlot(page_, live.slot_id, static_cast<std::uint16_t>(new_end),
            live.length);
  }
  WriteLe16(page_ + layout::kFreeEnd, static_cast<std::uint16_t>(new_end));
  WriteLe16(page_ + layout::kDeadBytes, 0);
  if (is_trimming_dead_slots) {
    std::size_t kept_slots = slot_count;
    while (kept_slots > 0 && IsDeadSlot(SlotOffsetOf(page_, kept_slots - 1),
                                        SlotLengthOf(page_, kept_slots - 1))) {
      --kept_slots;
    }
    WriteLe16(page_ + layout::kSlotCount,
              static_cast<std::uint16_t>(kept_slots));
  }
  return std::nullopt;
}

}  // namespace strata::paged
