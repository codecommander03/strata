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

private:
    Status allocate_table_id(TableId* out);

    Transaction* txn_;
};

} // namespace strata::sql
