#ifndef STRATA_PAGED_PAGED_TYPES_H_
#define STRATA_PAGED_PAGED_TYPES_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>

// Shared numbers, positions and error types of the paged engine (design
// document 2.2, 2.10). Pure: no I/O, no locks.

namespace strata::paged {

using PageId = std::uint32_t;
using FrameId = std::uint32_t;

inline constexpr std::size_t kPageSize = 4096;
// The cache line of the development machine (Apple M4) is 128 bytes; x86 uses
// 64. 128 covers both (ADR-0034).
inline constexpr std::size_t kCacheLineSize = 128;
inline constexpr std::size_t kPageHeaderSize = 32;
inline constexpr std::size_t kSlotSize = 4;
// One tuple alone on a page: page size minus the header and its one slot.
inline constexpr std::size_t kMaxTupleSize =
    kPageSize - kPageHeaderSize - kSlotSize;
inline constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

// The system tables and the pages their heap chains start on. Pages 1 and 2
// hold sys_tables and sys_columns, so the first page a user table can own is 3.
// The first user table id and the first user page are equal by accident: the
// first is a rule about table ids, the second a rule about which pages
// PageAllocator may release (design document 2.10).
inline constexpr std::uint32_t kSysTablesId = 1;
inline constexpr std::uint32_t kSysColumnsId = 2;
inline constexpr std::uint32_t kFirstUserTableId = 3;
inline constexpr PageId kSysTablesPage = 1;
inline constexpr PageId kSysColumnsPage = 2;
inline constexpr PageId kFirstUserPage = 3;

// Byte positions of the fixed-width little-endian fields (design document
// 2.10). Every page starts with the common header; page 0 continues with the
// file header.
namespace layout {
inline constexpr std::size_t kChecksum = 0;
inline constexpr std::size_t kPageIdField = 4;
inline constexpr std::size_t kPageTypeField = 8;
inline constexpr std::size_t kReserved = 9;
inline constexpr std::size_t kSlotCount = 10;
inline constexpr std::size_t kFreeEnd = 12;
inline constexpr std::size_t kDeadBytes = 14;
inline constexpr std::size_t kNextPageId = 16;
inline constexpr std::size_t kTableId = 20;
inline constexpr std::size_t kLsn = 24;

inline constexpr std::size_t kMagic = 32;
inline constexpr std::size_t kMagicSize = 8;
inline constexpr std::size_t kFormatVersion = 40;
inline constexpr std::size_t kFilePageSize = 44;
inline constexpr std::size_t kNextTableId = 48;
inline constexpr std::size_t kSysTablesFirstPage = 52;
inline constexpr std::size_t kSysColumnsFirstPage = 56;
// The first page of the free list, or 0 when no page is free (page 0 is the
// file header and is never free).
inline constexpr std::size_t kFreeListHead = 60;
}  // namespace layout

enum class PageType : std::uint8_t {
  kUnformatted = 0,
  kFileHeader = 1,
  kHeapData = 2,
  kFree = 3,
  kIndexNode = 4
};

enum class LatchMode { kShared, kExclusive };

enum class ReplacementPolicyKind { kLru, kTwoQ };

// The position of a tuple: a page and a slot on it. Slot numbers are stable
// (ADR-0026), but a slot freed by a delete may be reused by a later insert.
struct Rid {
  PageId page_id;
  std::uint16_t slot_id;

  // Equal when both fields are equal; ordered by page, then by slot.
  bool operator==(const Rid&) const = default;
  auto operator<=>(const Rid&) const = default;
};

// A read-only run of bytes. It does not own them: it is valid for as long as
// the owner of the underlying buffer keeps it unchanged.
struct ByteView {
  const std::uint8_t* data;
  std::size_t size;
};

enum class PageErrorCode {
  kIo,
  kCorrupt,
  kNoSpace,
  kNoFrame,
  kNotFound,
  kTooLarge,
  kInvalidArgument,
  kAlreadyExists,
  kClosed
};

struct PageError {
  PageErrorCode code;
  std::string message;
};

// The outcome of an operation without a value: no value means success.
using Status = std::optional<PageError>;

// A value or a PageError; the engine does not use exceptions. T must not be
// PageError. value() requires ok(); error() requires !ok().
template <typename T>
class PageResult {
 public:
  // Implicit on purpose, like absl::StatusOr: a function returns a value or an
  // error directly.
  PageResult(T value) : state_(std::move(value)) {}  // NOLINT(runtime/explicit)
  PageResult(PageError error)                        // NOLINT(runtime/explicit)
      : state_(std::move(error)) {}

  bool ok() const { return std::holds_alternative<T>(state_); }
  T& value() & { return std::get<T>(state_); }
  const T& value() const& { return std::get<T>(state_); }
  T&& value() && { return std::get<T>(std::move(state_)); }
  const PageError& error() const { return std::get<PageError>(state_); }

 private:
  std::variant<T, PageError> state_;
};

inline PageError MakeError(PageErrorCode code, std::string message) {
  return PageError{code, std::move(message)};
}

}  // namespace strata::paged

namespace std {
template <>
struct hash<strata::paged::Rid> {
  std::size_t operator()(const strata::paged::Rid& rid) const noexcept {
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(rid.page_id) << 16) | rid.slot_id;
    return std::hash<std::uint64_t>()(packed);
  }
};
}  // namespace std

#endif  // STRATA_PAGED_PAGED_TYPES_H_
