#ifndef STRATA_PAGED_PAGE_ALLOCATOR_H_
#define STRATA_PAGED_PAGE_ALLOCATOR_H_

#include <vector>

#include "strata/paged/buffer_manager.h"
#include "strata/paged/paged_types.h"

// Hands out the pages of a database and takes released ones back (design
// document 2.11, ADR-0046). Resource-aware: every call goes through the
// BufferManager.
//
// The free list is a chain of free pages (page_type 3) linked by next_page_id.
// Its head is free_list_head in the file header, page 0; 0 means no free page,
// because page 0 is never free. A page is either free or owned by a table,
// never both: Release formats each page of a chain as a free page, so a table
// scan rejects a free page in its chain and FreePages rejects a table page on
// the list.
//
// Concurrency: Allocate and Release change page 0 and need a single writer at
// a time, which the database-level lock gives.
// Allocate holds the file header and the popped page together, so a caller
// that holds a chain tail takes the latches in the order tail, header, page.
// Release holds one latch at a time. FreePages holds one shared latch at a
// time.
//
// A page chain that Release is given must belong to no table any more: the
// caller removes the catalog entry first, so a failure between the two steps
// wastes pages and never frees a page that a table names.

namespace strata::paged {

class PageAllocator {
 public:
  // buffer must outlive the allocator. Nothing is read until a call.
  explicit PageAllocator(BufferManager& buffer) : buffer_(buffer) {}

  // The first page of the free list, latched exclusively and marked dirty,
  // still formatted as a free page, for the caller to format; the head moves
  // to the next free page. When the list is empty it returns
  // BufferManager::NewPage, which extends the file. kCorrupt: the head or its
  // successor is outside the file or below page 3, or the head page is not a
  // free page. The list is unchanged after an error.
  PageResult<PageGuard> Allocate();

  // Puts the page chain that starts at first_page on the free list: formats
  // each page as a free page linked to the next, links the last page to the
  // old head, and then sets the head to first_page. Every page of the chain
  // must be a heap page at or above page 3. kCorrupt: the chain leaves the
  // file, reaches a page that is not a heap page (the file header, a free
  // page, a page already visited) or a system table page. kInvalidArgument:
  // first_page is below 3. The head is unchanged after an error, and the pages
  // formatted before the error are free pages that no list reaches.
  Status Release(PageId first_page);

  // Puts an explicit list of pages on the free list, for pages that no chain
  // links, such as the nodes of an index tree. Formats each page as a free page
  // linked to the next in list order, links the last page to the old head and
  // sets the head to the first page, last, as Release does. Every page must be
  // a heap page or an index node at or above page 3, inside the file, and
  // listed once; the whole list is checked before the first page changes, so
  // after any of these errors the head and every page are unchanged.
  // kInvalidArgument: a page below 3. kCorrupt: a page past the end of the
  // file, a page listed twice, or a page that is not a heap page or an index
  // node. An empty list does nothing. A read or write that fails after the
  // check leaves the head unchanged and some pages formatted as free pages that
  // no list reaches. The pages must belong to no table and no tree any more.
  Status ReleasePages(const std::vector<PageId>& pages);

  // The free pages in list order, from a walk that also checks the list:
  // kCorrupt for a page outside the file, a page visited twice and a page that
  // is not a free page. O(F) page reads for F free pages.
  PageResult<std::vector<PageId>> FreePages() const;

 private:
  BufferManager& buffer_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_PAGE_ALLOCATOR_H_
