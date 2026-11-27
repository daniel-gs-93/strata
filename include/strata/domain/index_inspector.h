#ifndef STRATA_DOMAIN_INDEX_INSPECTOR_H_
#define STRATA_DOMAIN_INDEX_INSPECTOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/value.h"

// The Domain's view of the shape of an index, for the `.dump index` command
// (design document 2.2, 3.6; ADR-0056). Data only: a test checks a description
// without a terminal, and the command handler only prints it. An engine that
// has indexes implements IIndexInspector beside IEngine; one that has none
// provides no inspector.

namespace strata::domain {

// At most this many nodes are listed in an IndexDescription.
inline constexpr std::size_t kMaxDescribedIndexNodes = 40;

// One node of the tree: a leaf is at level 0 and the root has the greatest
// level.
struct IndexNodeDescription {
  std::uint16_t level;
  std::uint32_t page_id;
  std::size_t key_count;
  // Absent on a node without a key.
  std::optional<std::int64_t> first_key;
  std::optional<std::int64_t> last_key;
  // The right sibling of a leaf that has one.
  std::optional<std::uint32_t> next_page_id;
};

struct IndexDescription {
  // As the catalog spells them.
  std::string table;
  std::string column;
  // Pages a lookup reads: the level of the root plus one.
  std::size_t height;
  // Nodes and leaf keys counted by the walk. A damaged tree makes the walk stop
  // early, so the counts then cover what was read.
  std::size_t node_count;
  std::size_t key_count;
  // The first kMaxDescribedIndexNodes nodes in breadth-first order from the
  // root.
  std::vector<IndexNodeDescription> nodes;
  // "ok", or the first violation that the check of the index found.
  std::string check;
};

using IndexDescriptionResult = std::variant<IndexDescription, ExecutionError>;

class IIndexInspector {
 public:
  virtual ~IIndexInspector() = default;

  // The description of the index of the table (names are case-insensitive).
  // An ExecutionError when the table is unknown (kBind), when it has no index
  // (kBind), or when its index cannot be opened or its root cannot be read
  // (kIo). A tree that opens but fails its check is not an error: the
  // description carries the violation in `check`. Called under the shared lock
  // of QueryService, so several threads may call it at the same time.
  virtual IndexDescriptionResult DescribeIndex(const std::string& table) = 0;
};

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_INDEX_INSPECTOR_H_
