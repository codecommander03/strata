#pragma once

#include "strata/db.hpp"
#include "strata/sql/ast.hpp"
#include "strata/sql/record.hpp"

#include <optional>
#include <string>
#include <vector>

namespace strata::sql {

/// A table's schema, as stored in the catalog.
struct TableDef {
    std::string name;
    TableId id = 0;
    std::vector<ColumnDef> columns;
    RowId next_row_id = 1;

    /// Index of a column by name, or -1.
    int column_index(const std::string& column) const;
};

std::string encode_table_def(const TableDef& def);
bool decode_table_def(Bytes stored, TableDef* out);

/// One index, over one column of one table.
///
/// Single-column only. Composite indexes need a tuple encoding and a planner
/// that understands prefix matching, and neither is needed to demonstrate what
/// stage 7 set out to demonstrate.
struct IndexDef {
    std::string name;
    std::string table;
    TableId table_id = 0;
    IndexId id = 0;
    int column = 0; ///< position in the table's column list
};

std::string encode_index_def(const IndexDef& def);
bool decode_index_def(Bytes stored, IndexDef* out);

/// Table definitions, read and written through an ordinary transaction.
///
/// The catalog is not a special structure: it lives in the same tree, under the
/// same MVCC rules, in the same transaction as the data. So `CREATE TABLE`
/// rolls back with everything else, and a reader with an old snapshot sees the
/// schema that matched its data.
class Catalog {
public:
    explicit Catalog(Transaction& txn) : txn_(&txn) {}

    Status create(const CreateTable& statement, TableDef* out);
    Status drop(const DropTable& statement);

    /// NotFound if there is no such table.
    Status lookup(const std::string& table, TableDef* out);

    /// Writes back a definition whose `next_row_id` has moved.
    Status save(const TableDef& def);

    // --- indexes ---

    Status create_index(const CreateIndex& statement, IndexDef* out);
    Status drop_index(const DropIndex& statement);

    Status lookup_index(const std::string& name, IndexDef* out);

    /// Every index defined over this table, which is what the write path needs
    /// on each insert, update and delete.
    Status indexes_for(TableId table, std::vector<IndexDef>* out);

    /// Every index in the database, for `verify_indexes` and the shell.
    Status all_indexes(std::vector<IndexDef>* out);

private:
    Status allocate_table_id(TableId* out);
    Status allocate_index_id(IndexId* out);

    Transaction* txn_;
};

/// Writes the index entries for one row of one table.
Status index_row(Transaction& txn, const TableDef& table, const std::vector<IndexDef>& indexes,
                 RowId row_id, const Row& row);

/// Removes them again.
Status unindex_row(Transaction& txn, const TableDef& table, const std::vector<IndexDef>& indexes,
                   RowId row_id, const Row& row);

} // namespace strata::sql
