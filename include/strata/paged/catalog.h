#ifndef STRATA_PAGED_CATALOG_H_
#define STRATA_PAGED_CATALOG_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "strata/domain/engine.h"
#include "strata/domain/value.h"
#include "strata/paged/buffer_manager.h"
#include "strata/paged/page_allocator.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/table_heap.h"

// The schemas of the user tables, stored in two system tables inside the same
// database file (design document 2.6, ADR-0032): sys_tables (table id 1) and
// sys_columns (table id 2). The catalog reads them once into an in-memory
// cache, so FindTable is O(1) plus a copy of the schema. A table whose primary
// key is an INT also has an index, and its sys_tables row records the page id
// of the root (index_root, ADR-0048); 0 means no index.
//
// FindTable, TableNames, TableId and FirstPage may be called from several
// threads at once; the cache is guarded by a shared_mutex. CreateTable,
// CommitAddColumn and DropTable calls are serialized by a mutex and update the
// cache under the exclusive lock.

namespace strata::paged {

class Catalog : public domain::ICatalog {
 public:
  // The fixed schemas of the two system tables.
  static domain::TableSchema SysTablesSchema();
  static domain::TableSchema SysColumnsSchema();

  // The first pages of the system tables are read from the file header by the
  // caller and passed in. buffer and allocator must outlive the Catalog.
  // Nothing is read until Load.
  Catalog(BufferManager& buffer, PageAllocator& allocator,
          PageId sys_tables_first_page, PageId sys_columns_first_page);

  // Reads and validates both system tables, including the target of every
  // foreign key, and the file header's next_table_id, and replaces the cache.
  // kCorrupt names the table and column; see design document 2.6 for the
  // checks.
  Status Load();

  // Adds a table: validates schema, allocates the table's first page and, for
  // an INT primary key, the root of its index (an empty leaf), writes the rows
  // in the system tables with both page ids in the sys_tables row, advances
  // next_table_id, and then updates the cache. There is no transaction: when a
  // step fails the rows already written are deleted (best effort) and the
  // first error is returned; the new table's pages stay allocated.
  // kAlreadyExists: the name is taken (compared case-insensitively).
  // kInvalidArgument: any other rule of 2.6 is violated.
  PageResult<domain::TableSchema> CreateTable(
      const domain::TableSchema& schema);

  // ADD COLUMN, first step (design document 2.6, ADR-0045): the schema that
  // the table has once the column is added, with nothing changed.
  // kNotFound: no such table. kAlreadyExists: the table has a column of that
  // name (compared case-insensitively). kInvalidArgument: the column has no
  // DEFAULT of its type, or its sys_columns row would not fit in one tuple.
  PageResult<domain::TableSchema> CheckAddColumn(
      const std::string& table, const domain::ColumnDef& column) const;

  // ADD COLUMN, last step: adds the sys_columns row at the next position,
  // points the table's sys_tables row at new_first_page, which holds the rows
  // already copied with the new schema, and at new_index_root, the root of the
  // tree built beside it (0 for a table without an index), in one row update,
  // and updates the cache. The errors are those of CheckAddColumn, and
  // kCorrupt when sys_tables has no row for the table. When the second write
  // fails the first is deleted (best effort) and the message ends with
  // `; undoing the rows also failed: ...` if that fails too. The table's old
  // pages are not touched.
  Status CommitAddColumn(const std::string& table,
                         const domain::ColumnDef& column, PageId new_first_page,
                         PageId new_index_root);

  // Deletes the sys_columns rows and the sys_tables row of the table and
  // removes it from the cache. Its table id is not used again. The table's
  // pages are not touched; the caller releases them afterwards. kNotFound:
  // no such table. kInvalidArgument: another table has a foreign key to it.
  // When a delete fails the rows already deleted are
  // inserted again (best effort), as for CommitAddColumn.
  Status DropTable(const std::string& table);

  // ICatalog: names are matched case-insensitively, and the schema is a copy.
  std::optional<domain::TableSchema> FindTable(
      const std::string& name) const override;
  // User tables only, in creation order.
  std::vector<std::string> TableNames() const override;

  std::optional<std::uint32_t> TableId(const std::string& name) const;
  std::optional<PageId> FirstPage(const std::string& name) const;
  // The root page of the table's index. No value when the table is unknown or
  // has no index (its index_root is 0).
  std::optional<PageId> IndexRoot(const std::string& name) const;

 private:
  struct Entry {
    std::uint32_t table_id;
    domain::TableSchema schema;
    PageId first_page;
    // 0 when the table has no index.
    PageId index_root;
  };

  // Undoes the rows of a failed CreateTable; returns the message to append to
  // the original error when the undo itself fails, else empty.
  std::string UndoRows(const std::vector<Rid>& table_rows,
                       const std::vector<Rid>& column_rows);

  // The rid of the sys_tables row of table_id; kCorrupt when there is none.
  PageResult<Rid> FindTablesRow(std::uint32_t table_id) const;

  BufferManager& buffer_;
  PageAllocator& allocator_;
  PageId sys_tables_first_page_;
  PageId sys_columns_first_page_;
  std::unique_ptr<TableHeap> sys_tables_;
  std::unique_ptr<TableHeap> sys_columns_;
  std::uint32_t next_table_id_ = 0;

  mutable std::shared_mutex cache_mutex_;
  // In creation order (ascending table id); and a map from lower-cased name to
  // the position in entries_ for O(1) lookup.
  std::vector<Entry> entries_;
  std::unordered_map<std::string, std::size_t> position_of_name_;

  std::mutex ddl_mutex_;
};

}  // namespace strata::paged

#endif  // STRATA_PAGED_CATALOG_H_
