// The index half of the catalog: definitions, creation with backfill, drop,
// and the maintenance the write path calls on every row it touches.
//
// Kept apart from catalog.cpp so the table catalog stays readable; both write
// through the same transaction and obey the same MVCC rules, so an index is
// created, populated and dropped inside whatever transaction asked for it.

#include "strata/sql/catalog.hpp"
#include "strata/sql/keyenc.hpp"
#include "strata/sql/record.hpp"

namespace strata::sql {
namespace {

/// The value a row presents to an index. A row written before a column existed
/// is short rather than corrupt, and reads as null.
const Value& indexed_value(const Row& row, int column) {
    static const Value kNull;
    const auto at = static_cast<std::size_t>(column);
    return at < row.size() ? row[at] : kNull;
}

Status apply_entries(Transaction& txn, const std::vector<IndexDef>& indexes, RowId row_id,
                     const Row& row, bool add) {
    for (const IndexDef& def : indexes) {
        const std::string key =
            index_entry_key(def.id, encode_index_value(indexed_value(row, def.column)), row_id);
        // The entry carries no payload: everything it says is in the key.
        const Status s = add ? txn.put(as_bytes(key), Bytes{}) : txn.remove(as_bytes(key));
        if (!s) {
            // A missing entry on the delete path means the index had already
            // drifted from its table. Report it rather than swallowing it —
            // silently tolerating drift is how a wrong answer gets served.
            if (!add && s.code() == Code::NotFound) {
                return Status::corruption("index '" + def.name +
                                          "' was missing its entry for row " +
                                          std::to_string(row_id));
            }
            return s;
        }
    }
    return Status::ok();
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::string encode_index_def(const IndexDef& def) {
    std::string out;
    put_be32(&out, def.id);
    put_be32(&out, def.table_id);
    put_be32(&out, static_cast<std::uint32_t>(def.column));
    put_be32(&out, static_cast<std::uint32_t>(def.name.size()));
    out.append(def.name);
    put_be32(&out, static_cast<std::uint32_t>(def.table.size()));
    out.append(def.table);
    return out;
}

bool decode_index_def(Bytes stored, IndexDef* out) {
    const auto* p = reinterpret_cast<const unsigned char*>(stored.data());
    const std::size_t n = stored.size();
    std::size_t at = 0;
    auto need = [&](std::size_t bytes) { return at + bytes <= n; };

    if (!need(16)) {
        return false;
    }
    out->id = read_be32(p + at);
    at += 4;
    out->table_id = read_be32(p + at);
    at += 4;
    out->column = static_cast<int>(read_be32(p + at));
    at += 4;

    const std::uint32_t name_len = read_be32(p + at);
    at += 4;
    if (!need(name_len)) {
        return false;
    }
    out->name.assign(reinterpret_cast<const char*>(p + at), name_len);
    at += name_len;

    if (!need(4)) {
        return false;
    }
    const std::uint32_t table_len = read_be32(p + at);
    at += 4;
    if (!need(table_len)) {
        return false;
    }
    out->table.assign(reinterpret_cast<const char*>(p + at), table_len);
    return true;
}

// ---------------------------------------------------------------------------
// Catalog operations
// ---------------------------------------------------------------------------

Status Catalog::allocate_index_id(IndexId* out) {
    const std::string key = index_sequence_key();
    IndexId next = 1;

    std::string stored;
    if (Status s = txn_->get(as_bytes(key), &stored); s) {
        if (stored.size() != 4) {
            return Status::corruption("index sequence record is malformed");
        }
        next = read_be32(reinterpret_cast<const unsigned char*>(stored.data()));
    } else if (s.code() != Code::NotFound) {
        return s;
    }

    *out = next;
    std::string updated;
    put_be32(&updated, next + 1);
    return txn_->put(as_bytes(key), as_bytes(updated));
}

Status Catalog::lookup_index(const std::string& name, IndexDef* out) {
    std::string stored;
    if (Status s = txn_->get(as_bytes(index_catalog_key(name)), &stored); !s) {
        return s;
    }
    if (!decode_index_def(as_bytes(stored), out)) {
        return Status::corruption("index entry for '" + name + "' is malformed");
    }
    return Status::ok();
}

Status Catalog::all_indexes(std::vector<IndexDef>* out) {
    out->clear();
    std::vector<std::pair<std::string, std::string>> entries;
    if (Status s = txn_->scan_prefix(std::string(1, kIndexPrefix), &entries); !s) {
        return s;
    }
    for (const auto& [key, value] : entries) {
        if (key.size() <= 1) {
            continue; // the id sequence record shares this prefix
        }
        IndexDef def;
        if (!decode_index_def(as_bytes(value), &def)) {
            return Status::corruption("malformed index catalog entry");
        }
        out->push_back(std::move(def));
    }
    return Status::ok();
}

Status Catalog::indexes_for(TableId table, std::vector<IndexDef>* out) {
    std::vector<IndexDef> all;
    if (Status s = all_indexes(&all); !s) {
        return s;
    }
    out->clear();
    for (IndexDef& def : all) {
        if (def.table_id == table) {
            out->push_back(std::move(def));
        }
    }
    return Status::ok();
}

Status Catalog::create_index(const CreateIndex& statement, IndexDef* out) {
    IndexDef existing;
    const Status found = lookup_index(statement.name, &existing);
    if (found) {
        if (statement.if_not_exists) {
            *out = existing;
            return Status::ok();
        }
        return Status::invalid_argument("index '" + statement.name + "' already exists");
    }
    if (found.code() != Code::NotFound) {
        return found;
    }

    TableDef table;
    if (Status s = lookup(statement.table, &table); !s) {
        return s.code() == Code::NotFound
                   ? Status::invalid_argument("no such table: " + statement.table)
                   : s;
    }
    const int column = table.column_index(statement.column);
    if (column < 0) {
        return Status::invalid_argument("no such column: " + statement.column);
    }

    IndexDef def;
    def.name = statement.name;
    def.table = table.name;
    def.table_id = table.id;
    def.column = column;
    if (Status s = allocate_index_id(&def.id); !s) {
        return s;
    }

    // Backfill. An index built on a populated table must cover the rows that
    // are already there, or every query that uses it silently misses them —
    // and being silently incomplete is worse than not existing.
    std::vector<std::pair<std::string, std::string>> raw;
    if (Status s = txn_->scan_prefix(table_range_start(table.id), &raw); !s) {
        return s;
    }
    for (const auto& [key, value] : raw) {
        TableId owner = 0;
        RowId row_id = 0;
        if (!decode_row_key(key, &owner, &row_id) || owner != table.id) {
            continue;
        }
        Row row;
        if (!decode_row(as_bytes(value), &row)) {
            return Status::corruption("malformed row while building index '" + def.name + "'");
        }
        const std::string entry =
            index_entry_key(def.id, encode_index_value(indexed_value(row, column)), row_id);
        if (Status s = txn_->put(as_bytes(entry), Bytes{}); !s) {
            return s;
        }
    }

    const std::string encoded = encode_index_def(def);
    if (Status s = txn_->put(as_bytes(index_catalog_key(def.name)), as_bytes(encoded)); !s) {
        return s;
    }
    *out = std::move(def);
    return Status::ok();
}

Status Catalog::drop_index(const DropIndex& statement) {
    IndexDef def;
    const Status found = lookup_index(statement.name, &def);
    if (!found) {
        if (found.code() == Code::NotFound && statement.if_exists) {
            return Status::ok();
        }
        return found.code() == Code::NotFound
                   ? Status::invalid_argument("no such index: " + statement.name)
                   : found;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    if (Status s = txn_->scan_prefix(index_range_start(def.id), &entries); !s) {
        return s;
    }
    for (const auto& [key, value] : entries) {
        (void)value;
        if (Status s = txn_->remove(as_bytes(key)); !s) {
            return s;
        }
    }
    return txn_->remove(as_bytes(index_catalog_key(statement.name)));
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

Status index_row(Transaction& txn, const TableDef& table, const std::vector<IndexDef>& indexes,
                 RowId row_id, const Row& row) {
    (void)table;
    return apply_entries(txn, indexes, row_id, row, true);
}

Status unindex_row(Transaction& txn, const TableDef& table, const std::vector<IndexDef>& indexes,
                   RowId row_id, const Row& row) {
    (void)table;
    return apply_entries(txn, indexes, row_id, row, false);
}

} // namespace strata::sql
