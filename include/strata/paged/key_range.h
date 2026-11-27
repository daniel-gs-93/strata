#ifndef STRATA_PAGED_KEY_RANGE_H_
#define STRATA_PAGED_KEY_RANGE_H_

#include <cstddef>
#include <cstdint>
#include <optional>

// A range of INT keys: an optional low and an optional high bound, each
// inclusive or exclusive (design document 2.2, 2.13). A value type pure of
// pages: the index scan reads through it, and the planner builds it from a
// predicate.

namespace strata::domain {
struct BoundExpr;
}  // namespace strata::domain

namespace strata::paged {

struct KeyRange {
  std::optional<std::int64_t> low;
  bool is_low_inclusive = true;
  std::optional<std::int64_t> high;
  bool is_high_inclusive = true;

  bool operator==(const KeyRange&) const = default;

  // True when no INT key can lie in the range: the low bound is above the high
  // bound, the bounds meet and one of them is exclusive, or an exclusive bound
  // lies beyond the end of the 64-bit keys.
  bool IsEmpty() const;

  // The bounds as inclusive keys: the smallest key the low bound admits, and
  // the largest key the high bound admits. No value when that side is
  // unbounded. Meaningful only when IsEmpty() is false.
  std::optional<std::int64_t> FirstKey() const;
  std::optional<std::int64_t> LastKey() const;
};

// The key range that predicate implies on the INT column at position `column`
// (design document 2.13, ADR-0052). Pure. It walks the top-level AND tree, in
// any nesting, and takes each conjunct of the form `column op literal` or
// `literal op column`, where the literal is an INT and op is =, <, <=, > or
// >= (the operator flips when the literal is on the left). It skips every other
// conjunct: <>, OR, a DOUBLE or TEXT literal, and any expression, such as
// `id + 0 = 5` or `ABS(id) = 5`. The bounds of the conjuncts taken are
// intersected, so bounds that cannot hold at once give a range for which
// IsEmpty() is true. No value when no conjunct was taken. The rows of the range
// are a superset of the rows that satisfy predicate: the caller still tests the
// whole predicate.
std::optional<KeyRange> ExtractKeyRange(const domain::BoundExpr& predicate,
                                        std::size_t column);

}  // namespace strata::paged

#endif  // STRATA_PAGED_KEY_RANGE_H_
