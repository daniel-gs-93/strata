#ifndef STRATA_PAGED_SLOTTED_PAGE_H_
#define STRATA_PAGED_SLOTTED_PAGE_H_

#include <cstdint>

#include "strata/paged/paged_types.h"

// Byte operations on one 4096-byte page in the heap-data format (design
// document 2.10, 2.11). Pure: the classes hold only a pointer to a buffer that
// the caller owns and keeps alive; there is no I/O, lock or pin here.
//
// Page layout: a 32-byte common header, then the slot array growing upward
// (4 bytes per slot: offset and length), and the tuples stacked downward from
// the end of the page. A dead slot has offset 0 and length 0. Slot numbers
// never change except that compaction removes the dead slots at the end of
// the array.

namespace strata::paged {

// Read-only access to a page. The ByteView returned by Get points into the
// page and is valid while the page buffer is unchanged.
class SlottedPageView {
 public:
  explicit SlottedPageView(const std::uint8_t* page) : page_(page) {}

  PageId page_id() const;
  PageType page_type() const;
  std::uint16_t slot_count() const;
  std::uint16_t free_end() const;
  std::uint16_t dead_bytes() const;
  PageId next_page_id() const;
  std::uint32_t table_id() const;
  std::uint64_t lsn() const;

  // The longest tuple Insert would accept now: contiguous free space plus
  // dead_bytes, minus 4 when no dead slot can be reused; 0 when nothing fits.
  // O(slot_count) to look for a dead slot.
  std::uint16_t InsertCapacity() const;

  // kNotFound: the slot is out of range or dead. kCorrupt: the slot's offset
  // or length lies outside the tuple area; nothing outside the page is read.
  PageResult<ByteView> Get(std::uint16_t slot_id) const;

  // Checks the page type, the free-space bounds, every slot, that the live
  // tuples do not overlap, and that live lengths plus dead_bytes equal the
  // tuple area. O(slot_count log slot_count). kCorrupt names the violation.
  Status CheckStructure() const;

 private:
  const std::uint8_t* page_;
};

class SlottedPage {
 public:
  // page must point to kPageSize writable bytes.
  explicit SlottedPage(std::uint8_t* page) : page_(page) {}

  // Zeroes the page and writes an empty heap-data page: page_type 2, no
  // slots, free_end 4096, next_page_id kInvalidPageId. The checksum is not
  // written; see SealChecksum.
  static void Format(std::uint8_t* page, PageId page_id,
                     std::uint32_t table_id);

  // Zeroes the page and writes an empty free page: page_type 3, no slots,
  // free_end 4096, table_id 0, next_page_id as given (the next free page, or
  // kInvalidPageId at the end of the free list). The checksum is not
  // written; see SealChecksum.
  static void FormatFree(std::uint8_t* page, PageId page_id,
                         PageId next_page_id);

  // Writes the CRC-32C of bytes [4, 4096) into bytes [0, 4). Applies to a page
  // of any type.
  static void SealChecksum(std::uint8_t* page);

  // The checks made after a page is read (any page type): checksum, page_id
  // equal to expected_page_id, page_type in 0..4, for heap data and free
  // pages free_start <= free_end <= 4096, and for an index node slot_count,
  // free_end and dead_bytes equal to 0. An all-zero page passes as
  // unformatted. kCorrupt messages name expected_page_id and the violation.
  // O(kPageSize).
  static Status VerifyHeader(const std::uint8_t* page, PageId expected_page_id);

  SlottedPageView view() const { return SlottedPageView(page_); }

  PageId page_id() const { return view().page_id(); }
  PageType page_type() const { return view().page_type(); }
  std::uint16_t slot_count() const { return view().slot_count(); }
  std::uint16_t free_end() const { return view().free_end(); }
  std::uint16_t dead_bytes() const { return view().dead_bytes(); }
  PageId next_page_id() const { return view().next_page_id(); }
  std::uint32_t table_id() const { return view().table_id(); }
  std::uint64_t lsn() const { return view().lsn(); }
  std::uint16_t InsertCapacity() const { return view().InsertCapacity(); }
  PageResult<ByteView> Get(std::uint16_t slot_id) const {
    return view().Get(slot_id);
  }
  Status CheckStructure() const { return view().CheckStructure(); }

  void set_next_page_id(PageId next_page_id);

  // Copies tuple into the page and returns its slot: the lowest dead slot if
  // there is one, else a new slot at the end. Compacts first when the
  // contiguous space is too small but contiguous space plus dead_bytes is
  // enough. kInvalidArgument: empty tuple. kTooLarge: longer than
  // kMaxTupleSize. kNoSpace: does not fit; the page is unchanged. kCorrupt:
  // compaction is needed and CheckStructure fails; the page is unchanged.
  PageResult<std::uint16_t> Insert(ByteView tuple);

  // Marks the slot dead and adds the tuple's length to dead_bytes; the tuple
  // bytes stay. kNotFound: out of range or already dead.
  Status Delete(std::uint16_t slot_id);

  // Replaces the tuple in the same slot. A tuple that is not longer is
  // overwritten in place (the difference becomes dead_bytes); a longer one is
  // kept in the same page when it fits after releasing the old tuple (with
  // compaction if needed). kNoSpace: it does not fit; the page is unchanged.
  // kCorrupt: compaction is needed and CheckStructure fails, checked before
  // the old tuple is released; the page is unchanged. kNotFound: dead or out
  // of range. kInvalidArgument and kTooLarge as for Insert.
  Status Update(std::uint16_t slot_id, ByteView tuple);

  // Moves the live tuples to the end of the page, sets dead_bytes to 0, and
  // removes the dead slots at the end of the slot array. Slot numbers of live
  // tuples do not change. kCorrupt: CheckStructure fails; the page is
  // unchanged.
  Status Compact();

 private:
  Status CompactKeepingSlots(bool is_trimming_dead_slots);

  std::uint8_t* page_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_SLOTTED_PAGE_H_
