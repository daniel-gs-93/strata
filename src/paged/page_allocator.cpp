#include "strata/paged/page_allocator.h"

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "strata/paged/byte_codec.h"
#include "strata/paged/slotted_page.h"

namespace strata::paged {

namespace {

PageError Corrupt(const std::string& detail) {
  return MakeError(PageErrorCode::kCorrupt, detail);
}

// The head of the free list as the file header stores it: 0 means no page.
PageId HeadOf(const PageGuard& header) {
  return ReadLe32(header.data() + layout::kFreeListHead);
}

PageId ListLinkOf(PageId head) { return head == 0 ? kInvalidPageId : head; }

}  // namespace

PageResult<PageGuard> PageAllocator::Allocate() {
  PageResult<PageGuard> fetched_header =
      buffer_.FetchPage(0, LatchMode::kExclusive);
  if (!fetched_header.ok()) {
    return fetched_header.error();
  }
  PageGuard header = std::move(fetched_header).value();
  const PageId head = HeadOf(header);
  if (head == 0) {
    header.Release();
    return buffer_.NewPage();
  }
  if (head >= buffer_.PageCount()) {
    return Corrupt("the free list leads past the end of the file: page " +
                   std::to_string(head) + " of " +
                   std::to_string(buffer_.PageCount()));
  }
  PageResult<PageGuard> fetched_page =
      buffer_.FetchPage(head, LatchMode::kExclusive);
  if (!fetched_page.ok()) {
    return fetched_page.error();
  }
  PageGuard page = std::move(fetched_page).value();
  const SlottedPageView view(page.data());
  if (view.page_type() != PageType::kFree) {
    return Corrupt("page " + std::to_string(head) +
                   " is the head of the free list but has page_type " +
                   std::to_string(static_cast<int>(view.page_type())) +
                   " instead of 3");
  }
  const PageId next = view.next_page_id();
  if (next != kInvalidPageId && next >= buffer_.PageCount()) {
    return Corrupt("the free list leads past the end of the file: page " +
                   std::to_string(next) + " of " +
                   std::to_string(buffer_.PageCount()));
  }
  // Page 0 would read as the end of the list in the file header, and pages 1
  // and 2 belong to the system tables.
  if (next != kInvalidPageId && next < kFirstUserPage) {
    return Corrupt("the free list reaches page " + std::to_string(next) +
                   ", which belongs to the file header or a system table");
  }
  WriteLe32(header.mutable_data() + layout::kFreeListHead,
            next == kInvalidPageId ? 0 : next);
  header.MarkDirty();
  page.MarkDirty();
  return PageResult<PageGuard>(std::move(page));
}

Status PageAllocator::Release(PageId first_page) {
  if (first_page < kFirstUserPage) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "page " + std::to_string(first_page) +
                         " belongs to the file header or a system table");
  }
  PageId link_after_chain = kInvalidPageId;
  {
    PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kShared);
    if (!header.ok()) {
      return header.error();
    }
    link_after_chain = ListLinkOf(HeadOf(header.value()));
  }
  const PageId page_count = buffer_.PageCount();
  PageId current = first_page;
  while (current != kInvalidPageId) {
    if (current >= page_count) {
      return Corrupt("the page chain leads past the end of the file: page " +
                     std::to_string(current) + " of " +
                     std::to_string(page_count));
    }
    if (current < kFirstUserPage) {
      return Corrupt("the page chain reaches page " + std::to_string(current) +
                     ", which belongs to the file header or a system table");
    }
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(current, LatchMode::kExclusive);
    if (!fetched.ok()) {
      return fetched.error();
    }
    PageGuard page = std::move(fetched).value();
    const SlottedPageView view(page.data());
    if (view.page_type() != PageType::kHeapData) {
      return Corrupt("page " + std::to_string(current) +
                     " is in a page chain but has page_type " +
                     std::to_string(static_cast<int>(view.page_type())) +
                     " instead of 2");
    }
    const PageId next = view.next_page_id();
    SlottedPage::FormatFree(page.mutable_data(), current,
                            next == kInvalidPageId ? link_after_chain : next);
    page.MarkDirty();
    current = next;
  }
  PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kExclusive);
  if (!header.ok()) {
    return header.error();
  }
  PageGuard header_guard = std::move(header).value();
  WriteLe32(header_guard.mutable_data() + layout::kFreeListHead, first_page);
  header_guard.MarkDirty();
  return std::nullopt;
}

Status PageAllocator::ReleasePages(const std::vector<PageId>& pages) {
  if (pages.empty()) {
    return std::nullopt;
  }
  // The whole list is checked before any page changes, so a refused list
  // leaves the file as it was. Each page is latched shared, alone, and the
  // guard ends with the iteration.
  const PageId page_count = buffer_.PageCount();
  std::unordered_set<PageId> listed;
  for (PageId page_id : pages) {
    if (page_id < kFirstUserPage) {
      return MakeError(PageErrorCode::kInvalidArgument,
                       "page " + std::to_string(page_id) +
                           " belongs to the file header or a system table");
    }
    if (page_id >= page_count) {
      return Corrupt("the page list leads past the end of the file: page " +
                     std::to_string(page_id) + " of " +
                     std::to_string(page_count));
    }
    if (!listed.insert(page_id).second) {
      return Corrupt("the page list holds page " + std::to_string(page_id) +
                     " twice");
    }
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(page_id, LatchMode::kShared);
    if (!fetched.ok()) {
      return fetched.error();
    }
    const PageType page_type =
        SlottedPageView(fetched.value().data()).page_type();
    if (page_type != PageType::kHeapData && page_type != PageType::kIndexNode) {
      return Corrupt("page " + std::to_string(page_id) +
                     " is in a page list but has page_type " +
                     std::to_string(static_cast<int>(page_type)) +
                     " instead of 2 or 4");
    }
  }
  PageId link_after_list = kInvalidPageId;
  {
    PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kShared);
    if (!header.ok()) {
      return header.error();
    }
    link_after_list = ListLinkOf(HeadOf(header.value()));
  }
  for (std::size_t position = 0; position < pages.size(); ++position) {
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(pages[position], LatchMode::kExclusive);
    if (!fetched.ok()) {
      return fetched.error();
    }
    PageGuard page = std::move(fetched).value();
    const PageId next =
        position + 1 < pages.size() ? pages[position + 1] : link_after_list;
    SlottedPage::FormatFree(page.mutable_data(), pages[position], next);
    page.MarkDirty();
  }
  PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kExclusive);
  if (!header.ok()) {
    return header.error();
  }
  PageGuard header_guard = std::move(header).value();
  WriteLe32(header_guard.mutable_data() + layout::kFreeListHead, pages.front());
  header_guard.MarkDirty();
  return std::nullopt;
}

PageResult<std::vector<PageId>> PageAllocator::FreePages() const {
  PageId current = kInvalidPageId;
  {
    PageResult<PageGuard> header = buffer_.FetchPage(0, LatchMode::kShared);
    if (!header.ok()) {
      return header.error();
    }
    current = ListLinkOf(HeadOf(header.value()));
  }
  const PageId page_count = buffer_.PageCount();
  std::vector<PageId> pages;
  std::unordered_set<PageId> visited;
  while (current != kInvalidPageId) {
    if (current >= page_count) {
      return Corrupt("the free list leads past the end of the file: page " +
                     std::to_string(current) + " of " +
                     std::to_string(page_count));
    }
    if (!visited.insert(current).second) {
      return Corrupt("the free list visits page " + std::to_string(current) +
                     " twice");
    }
    PageResult<PageGuard> fetched =
        buffer_.FetchPage(current, LatchMode::kShared);
    if (!fetched.ok()) {
      return fetched.error();
    }
    const SlottedPageView view(fetched.value().data());
    if (view.page_type() != PageType::kFree) {
      return Corrupt("page " + std::to_string(current) +
                     " is on the free list but has page_type " +
                     std::to_string(static_cast<int>(view.page_type())) +
                     " instead of 3");
    }
    pages.push_back(current);
    current = view.next_page_id();
  }
  return pages;
}

}  // namespace strata::paged
