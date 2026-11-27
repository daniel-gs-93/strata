#ifndef STRATA_PAGED_INDEX_READER_H_
#define STRATA_PAGED_INDEX_READER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "strata/domain/value.h"
#include "strata/paged/key_range.h"
#include "strata/paged/row_stream.h"

// The row layer's view of the indexes of a database (design document 2.2,
// 2.13; ADR-0051): a port beside ITableReader, so that the operators and the
// mutation planner read through an index without including a storage header.
// ITableReader has no index method; a caller that wants an index scan passes
// an IIndexReader as well, or a null pointer for table scans only.

namespace strata::paged {

// Whether a key exists, or the error of the page read that could not tell.
using ContainsResult = std::variant<bool, domain::ExecutionError>;

class IIndexReader {
 public:
  virtual ~IIndexReader() = default;

  // The position of the indexed column of the table (its primary key), or no
  // value for a table without an index and for an unknown table. Reads no
  // page. The name is matched case-insensitively.
  virtual std::optional<std::size_t> IndexedColumn(
      const std::string& table) const = 0;

  // The rows whose key lies in the range, in ascending key order, as an
  // IRowStream. Reads no page until the first Next. A range that no key can
  // satisfy gives a stream that ends at the first Next. An entry that names no
  // row, or a row whose key differs from its entry, ends the stream with a
  // kIo error `corrupt: ...`. The stream belongs to the caller and must not
  // outlive the reader. const, and several threads may call it.
  virtual ScanResult ScanRange(const std::string& table,
                               const KeyRange& range) const = 0;

  // Whether a row with the key exists, from the index alone: no heap page is
  // read, so an entry that names no row is not noticed.
  virtual ContainsResult Contains(const std::string& table,
                                  std::int64_t key) const = 0;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_INDEX_READER_H_
