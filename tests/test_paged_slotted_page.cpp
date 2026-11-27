#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "strata/paged/byte_codec.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"
#include "test_paged_support.h"

// SlottedPage, page headers, checksums and corruption checks.

namespace {

using strata::paged::ByteView;
using strata::paged::kInvalidPageId;
using strata::paged::kPageSize;
using strata::paged::PageType;
using strata::paged::ReadLe16;
using strata::paged::SlottedPage;
using strata::paged::SlottedPageView;
using strata::paged::WriteLe16;
using strata::paged::WriteLe32;
using strata::testing::Filled;
using strata::testing::FormattedPage;
using strata::testing::Page;
using strata::testing::ToString;
using strata::testing::ViewOf;

constexpr std::size_t kSlotArrayStart = 32;

std::uint16_t SlotOffsetOf(const Page& page, std::size_t slot_id) {
  return ReadLe16(page.data() + kSlotArrayStart + 4 * slot_id);
}
void SetSlotOf(Page& page, std::size_t slot_id, std::uint16_t offset,
               std::uint16_t length) {
  WriteLe16(page.data() + kSlotArrayStart + 4 * slot_id, offset);
  WriteLe16(page.data() + kSlotArrayStart + 4 * slot_id + 2, length);
}

// Inserts a tuple and returns its slot; reports a failure when it is rejected.
std::uint16_t MustInsert(SlottedPage& page, const std::string& tuple) {
  auto result = page.Insert(ViewOf(tuple));
  CHECK_OK(result);
  return result.ok() ? result.value() : 0xFFFF;
}

std::string TupleAt(const SlottedPage& page, std::uint16_t slot_id) {
  auto result = page.Get(slot_id);
  CHECK_OK(result);
  return result.ok() ? ToString(result.value()) : "<error>";
}

}  // namespace

TEST(test_paged_slotted_page_format_initializes_header) {
  Page page = FormattedPage(/*page_id=*/7, /*table_id=*/3);
  SlottedPage slotted(page.data());
  CHECK(slotted.page_type() == PageType::kHeapData);
  CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(0));
  CHECK_EQ(slotted.free_end(), static_cast<std::uint16_t>(4096));
  CHECK_EQ(slotted.dead_bytes(), static_cast<std::uint16_t>(0));
  CHECK_EQ(slotted.next_page_id(), kInvalidPageId);
  CHECK_EQ(slotted.table_id(), 3u);
  CHECK_EQ(slotted.page_id(), 7u);
  CHECK_EQ(slotted.lsn(), static_cast<std::uint64_t>(0));
  CHECK_EQ(slotted.InsertCapacity(), static_cast<std::uint16_t>(4060));
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_fills_to_exact_capacity) {
  Page page = FormattedPage();
  SlottedPage slotted(page.data());
  const std::string tuple = Filled(100, 'x');
  for (std::uint16_t expected_slot = 0; expected_slot < 39; ++expected_slot) {
    auto result = slotted.Insert(ViewOf(tuple));
    CHECK_OK(result);
    if (result.ok()) {
      CHECK_EQ(result.value(), expected_slot);
    }
  }
  CHECK_ERROR(slotted.Insert(ViewOf(tuple)), kNoSpace);
  CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(39));
  CHECK_EQ(slotted.InsertCapacity(), static_cast<std::uint16_t>(4));
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_get_and_delete_states) {
  Page page = FormattedPage();
  SlottedPage slotted(page.data());
  const std::string first = Filled(10, 'a');
  const std::string second = Filled(20, 'b');
  const std::string third = Filled(30, 'c');
  const std::uint16_t first_slot = MustInsert(slotted, first);
  const std::uint16_t second_slot = MustInsert(slotted, second);
  const std::uint16_t third_slot = MustInsert(slotted, third);
  CHECK_EQ(TupleAt(slotted, first_slot), first);
  CHECK_EQ(TupleAt(slotted, second_slot), second);
  CHECK_EQ(TupleAt(slotted, third_slot), third);

  CHECK_OK(slotted.Delete(second_slot));
  CHECK_ERROR(slotted.Get(second_slot), kNotFound);
  CHECK_ERROR(slotted.Delete(second_slot), kNotFound);
  CHECK_ERROR(slotted.Get(3), kNotFound);
  CHECK_ERROR(slotted.Delete(3), kNotFound);
  CHECK_ERROR(slotted.Get(0xFFFF), kNotFound);
  CHECK_ERROR(slotted.Delete(0xFFFF), kNotFound);
  CHECK_EQ(slotted.dead_bytes(), static_cast<std::uint16_t>(second.size()));
  CHECK_EQ(TupleAt(slotted, first_slot), first);
  CHECK_EQ(TupleAt(slotted, third_slot), third);
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_reuses_lowest_dead_slot) {
  Page page = FormattedPage();
  SlottedPage slotted(page.data());
  for (int i = 0; i < 5; ++i) {
    MustInsert(slotted, Filled(20, 'a' + i));
  }
  CHECK_OK(slotted.Delete(3));
  CHECK_OK(slotted.Delete(1));
  CHECK_EQ(MustInsert(slotted, Filled(20, 'x')), static_cast<std::uint16_t>(1));
  CHECK_EQ(MustInsert(slotted, Filled(20, 'y')), static_cast<std::uint16_t>(3));
  CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(5));
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_compacts_when_needed) {
  Page page = FormattedPage();
  SlottedPage slotted(page.data());
  std::vector<std::string> tuples;
  for (int i = 0; i < 20; ++i) {
    tuples.push_back(Filled(150, static_cast<char>('A' + i)));
    MustInsert(slotted, tuples.back());
  }
  for (std::uint16_t slot_id = 0; slot_id < 20; slot_id += 2) {
    CHECK_OK(slotted.Delete(slot_id));
  }
  // Contiguous space is 984 bytes; the new tuple needs 1000 and only fits
  // after the dead space is gathered.
  const std::uint16_t new_slot = MustInsert(slotted, Filled(1000, 'Z'));
  CHECK_EQ(new_slot, static_cast<std::uint16_t>(0));
  for (std::uint16_t slot_id = 1; slot_id < 20; slot_id += 2) {
    CHECK_EQ(TupleAt(slotted, slot_id), tuples[slot_id]);
  }
  CHECK_EQ(slotted.dead_bytes(), static_cast<std::uint16_t>(0));
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_compaction_trims_trailing_dead_slots) {
  Page page = FormattedPage();
  SlottedPage slotted(page.data());
  const std::string first = Filled(900, 'a');
  const std::string second = Filled(900, 'b');
  MustInsert(slotted, first);
  MustInsert(slotted, second);
  MustInsert(slotted, Filled(900, 'c'));
  MustInsert(slotted, Filled(900, 'd'));
  CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(4));
  CHECK_OK(slotted.Delete(2));
  CHECK_OK(slotted.Delete(3));
  const std::uint16_t new_slot = MustInsert(slotted, Filled(1000, 'e'));
  CHECK_EQ(new_slot, static_cast<std::uint16_t>(2));
  CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(3));
  CHECK_EQ(TupleAt(slotted, 0), first);
  CHECK_EQ(TupleAt(slotted, 1), second);
  CHECK_OK(slotted.CheckStructure());
}

TEST(test_paged_slotted_page_tuple_size_boundaries) {
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    CHECK_ERROR(slotted.Insert(ViewOf(std::string())), kInvalidArgument);
  }
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    CHECK_OK(slotted.Insert(ViewOf(Filled(1, 'x'))));
  }
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    CHECK_OK(slotted.Insert(ViewOf(Filled(4060, 'x'))));
    CHECK_ERROR(slotted.Insert(ViewOf(Filled(1, 'y'))), kNoSpace);
    CHECK_EQ(slotted.InsertCapacity(), static_cast<std::uint16_t>(0));
    CHECK_OK(slotted.CheckStructure());
  }
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    CHECK_ERROR(slotted.Insert(ViewOf(Filled(4061, 'x'))), kTooLarge);
  }
}

TEST(test_paged_slotted_page_update_cases) {
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    const std::uint16_t first = MustInsert(slotted, Filled(200, 'a'));
    const std::uint16_t second = MustInsert(slotted, Filled(300, 'b'));
    const std::uint16_t third = MustInsert(slotted, Filled(400, 'c'));
    const std::uint16_t second_offset = SlotOffsetOf(page, second);

    // Shorter: same slot, same offset, the difference becomes dead space.
    CHECK_OK(slotted.Update(second, ViewOf(Filled(100, 'B'))));
    CHECK_EQ(SlotOffsetOf(page, second), second_offset);
    CHECK_EQ(TupleAt(slotted, second), Filled(100, 'B'));
    CHECK_EQ(slotted.dead_bytes(), static_cast<std::uint16_t>(200));

    // Equal length: in place, dead space unchanged.
    CHECK_OK(slotted.Update(second, ViewOf(Filled(100, 'C'))));
    CHECK_EQ(SlotOffsetOf(page, second), second_offset);
    CHECK_EQ(TupleAt(slotted, second), Filled(100, 'C'));
    CHECK_EQ(slotted.dead_bytes(), static_cast<std::uint16_t>(200));

    // Longer, fits in the contiguous space: same slot, new bytes, others kept.
    CHECK_OK(slotted.Update(first, ViewOf(Filled(500, 'D'))));
    CHECK_EQ(TupleAt(slotted, first), Filled(500, 'D'));
    CHECK_EQ(TupleAt(slotted, second), Filled(100, 'C'));
    CHECK_EQ(TupleAt(slotted, third), Filled(400, 'c'));
    CHECK_OK(slotted.CheckStructure());

    // A dead slot cannot be updated.
    CHECK_OK(slotted.Delete(third));
    CHECK_ERROR(slotted.Update(third, ViewOf(Filled(10, 'x'))), kNotFound);
  }
  {
    // Longer and fits only after compaction.
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    for (int i = 0; i < 10; ++i) {
      MustInsert(slotted, Filled(300, static_cast<char>('0' + i)));
    }
    CHECK_OK(slotted.Delete(1));
    CHECK_OK(slotted.Delete(3));
    CHECK_OK(slotted.Delete(5));
    CHECK_OK(slotted.Update(8, ViewOf(Filled(1500, 'W'))));
    CHECK_EQ(TupleAt(slotted, 8), Filled(1500, 'W'));
    const int kept_slots[] = {0, 2, 4, 6, 7, 9};
    for (int kept : kept_slots) {
      CHECK_EQ(TupleAt(slotted, static_cast<std::uint16_t>(kept)),
               Filled(300, static_cast<char>('0' + kept)));
    }
    CHECK_ERROR(slotted.Get(1), kNotFound);
    CHECK_EQ(slotted.slot_count(), static_cast<std::uint16_t>(10));
    CHECK_OK(slotted.CheckStructure());
  }
  {
    // Longer and does not fit: the page is not changed.
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    for (int i = 0; i < 13; ++i) {
      MustInsert(slotted, Filled(300, 'k'));
    }
    const Page before = page;
    CHECK_ERROR(slotted.Update(0, ViewOf(Filled(500, 'z'))), kNoSpace);
    CHECK(before == page);
  }
}

namespace {

// A sealed page with tuples of 100, 200, 300 and 400 bytes and slot 1 dead.
Page SealedSamplePage() {
  Page page = FormattedPage(/*page_id=*/9, /*table_id=*/4);
  SlottedPage slotted(page.data());
  MustInsert(slotted, Filled(100, 'a'));
  MustInsert(slotted, Filled(200, 'b'));
  MustInsert(slotted, Filled(300, 'c'));
  MustInsert(slotted, Filled(400, 'd'));
  CHECK_OK(slotted.Delete(1));
  SlottedPage::SealChecksum(page.data());
  return page;
}

}  // namespace

TEST(test_paged_slotted_page_detects_corruption) {
  const Page base = SealedSamplePage();
  CHECK_OK(SlottedPage::VerifyHeader(base.data(), 9));
  CHECK_OK(SlottedPageView(base.data()).CheckStructure());

  // Header fields, found by VerifyHeader.
  {
    Page broken = base;
    broken[0] ^= 0x01;
    CHECK_ERROR(SlottedPage::VerifyHeader(broken.data(), 9), kCorrupt);
  }
  {
    Page broken = base;
    WriteLe32(broken.data() + 4, 10);
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage::VerifyHeader(broken.data(), 9), kCorrupt);
    CHECK_MESSAGE_CONTAINS(SlottedPage::VerifyHeader(broken.data(), 9),
                           "page 9");
  }
  {
    Page broken = base;
    broken[8] = 4;
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage::VerifyHeader(broken.data(), 9), kCorrupt);
  }
  {
    Page broken = base;
    WriteLe16(broken.data() + 12, 40);  // free_end below free_start (48)
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage::VerifyHeader(broken.data(), 9), kCorrupt);
  }
  {
    Page broken = base;
    WriteLe16(broken.data() + 12, 4097);
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage::VerifyHeader(broken.data(), 9), kCorrupt);
  }

  // Slot and accounting damage, found by CheckStructure.
  const std::uint16_t free_end = SlottedPageView(base.data()).free_end();
  {
    Page broken = base;
    SetSlotOf(broken, 0, static_cast<std::uint16_t>(free_end - 1), 100);
    SlottedPage::SealChecksum(broken.data());
    CHECK_OK(SlottedPage::VerifyHeader(broken.data(), 9));
    SlottedPage slotted(broken.data());
    CHECK_ERROR(slotted.CheckStructure(), kCorrupt);
    CHECK_ERROR(slotted.Get(0), kCorrupt);
  }
  {
    Page broken = base;
    SetSlotOf(broken, 0, SlotOffsetOf(broken, 0), 200);  // past the page end
    SlottedPage::SealChecksum(broken.data());
    SlottedPage slotted(broken.data());
    CHECK_ERROR(slotted.CheckStructure(), kCorrupt);
    CHECK_ERROR(slotted.Get(0), kCorrupt);
  }
  {
    Page broken = base;
    SetSlotOf(broken, 2, SlotOffsetOf(broken, 0), 100);  // overlaps slot 0
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage(broken.data()).CheckStructure(), kCorrupt);
  }
  {
    Page broken = base;
    WriteLe16(broken.data() + 14, 199);  // dead_bytes should be 200
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage(broken.data()).CheckStructure(), kCorrupt);
  }
  {
    Page broken = base;
    SetSlotOf(broken, 1, 0, 5);  // a dead slot must have length 0
    SlottedPage::SealChecksum(broken.data());
    CHECK_ERROR(SlottedPage(broken.data()).CheckStructure(), kCorrupt);
  }

  // A page of zeros is an unformatted page and passes.
  const Page zeros{};
  CHECK_OK(SlottedPage::VerifyHeader(zeros.data(), 123));
}

TEST(test_paged_slotted_page_refuses_to_compact_an_inconsistent_page) {
  // Overlapping live tuples: slot 1 points at the tuple of slot 0.
  {
    Page page = FormattedPage();
    SlottedPage slotted(page.data());
    MustInsert(slotted, Filled(1000, 'a'));
    MustInsert(slotted, Filled(1000, 'b'));
    MustInsert(slotted, Filled(1000, 'c'));
    SetSlotOf(page, 1, SlotOffsetOf(page, 0), 1000);
    const Page before = page;

    CHECK_ERROR(slotted.CheckStructure(), kCorrupt);
    CHECK_ERROR(slotted.Compact(), kCorrupt);
    CHECK(page == before);
  }

  // Four tuples of 1000 bytes leave 48 contiguous bytes, and dead_bytes
  // claims 2000 more that the page does not have.
  Page inflated = FormattedPage();
  {
    SlottedPage slotted(inflated.data());
    MustInsert(slotted, Filled(1000, 'a'));
    MustInsert(slotted, Filled(1000, 'b'));
    MustInsert(slotted, Filled(1000, 'c'));
    MustInsert(slotted, Filled(1000, 'd'));
  }
  WriteLe16(inflated.data() + strata::paged::layout::kDeadBytes, 2000);
  {
    Page page = inflated;
    SlottedPage slotted(page.data());

    CHECK_ERROR(slotted.Insert(ViewOf(Filled(500, 'e'))), kCorrupt);
    CHECK(page == inflated);
  }
  {
    Page page = inflated;
    SlottedPage slotted(page.data());

    // Grows slot 0, which needs compaction: the old row stays.
    CHECK_ERROR(slotted.Update(0, ViewOf(Filled(1050, 'f'))), kCorrupt);
    CHECK(page == inflated);
    CHECK_EQ(TupleAt(slotted, 0), Filled(1000, 'a'));
  }
}

TEST(test_paged_checksum_detects_every_single_byte_change) {
  Page page = SealedSamplePage();
  CHECK_OK(SlottedPage::VerifyHeader(page.data(), 9));
  for (std::size_t index = 0; index < kPageSize; ++index) {
    page[index] ^= static_cast<std::uint8_t>(1u << (index % 8));
    if (!SlottedPage::VerifyHeader(page.data(), 9).has_value()) {
      CHECK_EQ(index, static_cast<std::size_t>(-1));  // reports the index
    }
    page[index] ^= static_cast<std::uint8_t>(1u << (index % 8));
  }
  CHECK_OK(SlottedPage::VerifyHeader(page.data(), 9));
}

TEST(test_paged_slotted_page_verifies_free_pages) {
  Page page{};
  SlottedPage::FormatFree(page.data(), 7, kInvalidPageId);
  CHECK(SlottedPageView(page.data()).page_type() == PageType::kFree);
  CHECK_EQ(SlottedPageView(page.data()).next_page_id(), kInvalidPageId);
  CHECK_EQ(SlottedPageView(page.data()).table_id(), std::uint32_t{0});
  SlottedPage::SealChecksum(page.data());
  CHECK(SlottedPage::VerifyHeader(page.data(), 7) == std::nullopt);

  // Page type 5 does not exist (type 4 is an index node).
  Page unknown_type = page;
  unknown_type[8] = 5;
  SlottedPage::SealChecksum(unknown_type.data());
  const auto unknown_status = SlottedPage::VerifyHeader(unknown_type.data(), 7);
  CHECK(unknown_status.has_value());
  if (unknown_status.has_value()) {
    CHECK(unknown_status->code == strata::paged::PageErrorCode::kCorrupt);
    CHECK(unknown_status->message.find("page_type 5") != std::string::npos);
  }

  // A slot array that reaches past free_end.
  Page bad_slots = page;
  WriteLe16(bad_slots.data() + 10, 2000);
  SlottedPage::SealChecksum(bad_slots.data());
  const auto slot_status = SlottedPage::VerifyHeader(bad_slots.data(), 7);
  CHECK(slot_status.has_value());
  if (slot_status.has_value()) {
    CHECK(slot_status->code == strata::paged::PageErrorCode::kCorrupt);
  }
}
