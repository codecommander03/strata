#include "strata/sql/catalog.hpp"

#include "strata/sql/record.hpp"

#include <cstring>

namespace strata::sql {
namespace {

void put_be32(std::string* out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

void put_be64(std::string* out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

std::uint32_t read_be32(const unsigned char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

std::uint64_t read_be64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

std::string catalog_sequence_key() { return std::string(1, kCatalogPrefix); }

std::string catalog_key(const std::string& table) { return std::string(1, kCatalogPrefix) + table; }

std::string row_key(TableId table, RowId row) {
    std::string key(1, kRowPrefix);
    put_be32(&key, table);
    put_be64(&key, row);
    return key;
}

std::string table_range_start(TableId table) {
    std::string key(1, kRowPrefix);
    put_be32(&key, table);
    return key;
}

bool key_belongs_to_table(const std::string& key, TableId table) {
    const std::string prefix = table_range_start(table);
    return key.size() == prefix.size() + 8 && key.compare(0, prefix.size(), prefix) == 0;
}

bool decode_row_key(const std::string& key, TableId* table, RowId* row) {
    if (key.size() != 13 || key[0] != kRowPrefix) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(key.data());
    *table = read_be32(p + 1);
    *row = read_be64(p + 5);
    return true;
}

// ---------------------------------------------------------------------------
// Row encoding
// ---------------------------------------------------------------------------

std::string encode_row(const Row& row) {
    std::string out;
    out.push_back(static_cast<char>((row.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(row.size() & 0xFF));

    for (const Value& value : row) {
        out.push_back(static_cast<char>(value.type()));
        switch (value.type()) {
        case Type::Null:
            break;
        case Type::Integer:
            put_be64(&out, static_cast<std::uint64_t>(value.integer()));
            break;
        case Type::Real: {
            std::uint64_t bits = 0;
            const double d = value.real();
            std::memcpy(&bits, &d, sizeof(bits));
            put_be64(&out, bits);
            break;
        }
        case Type::Text: {
            const std::string& text = value.text();
            put_be32(&out, static_cast<std::uint32_t>(text.size()));
            out.append(text);
            break;
        }
        }
    }
    return out;
}

bool decode_row(Bytes stored, Row* out) {
    const auto* p = reinterpret_cast<const unsigned char*>(stored.data());
    const std::size_t n = stored.size();
    if (n < 2) {
        return false;
    }

    const std::size_t count = (static_cast<std::size_t>(p[0]) << 8) | p[1];
    std::size_t at = 2;
    out->clear();
    out->reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        if (at >= n) {
            return false;
        }
        const auto type = static_cast<Type>(p[at++]);
        switch (type) {
        case Type::Null:
            out->push_back(Value::null());
            break;
        case Type::Integer:
            if (at + 8 > n) {
                return false;
            }
            out->push_back(Value(static_cast<std::int64_t>(read_be64(p + at))));
            at += 8;
            break;
        case Type::Real: {
            if (at + 8 > n) {
                return false;
            }
            const std::uint64_t bits = read_be64(p + at);
            double d = 0;
            std::memcpy(&d, &bits, sizeof(d));
            out->push_back(Value(d));
            at += 8;
            break;
        }
        case Type::Text: {
            if (at + 4 > n) {
                return false;
            }
            const std::uint32_t len = read_be32(p + at);
            at += 4;
            if (at + len > n) {
                return false;
            }
            out->push_back(Value(std::string(reinterpret_cast<const char*>(p + at), len)));
            at += len;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// TableDef
// ---------------------------------------------------------------------------

int TableDef::column_index(const std::string& column) const {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == column) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string encode_table_def(const TableDef& def) {
    std::string out;
    put_be32(&out, def.id);
    put_be64(&out, def.next_row_id);
    put_be32(&out, static_cast<std::uint32_t>(def.name.size()));
    out.append(def.name);
    put_be32(&out, static_cast<std::uint32_t>(def.columns.size()));
    for (const ColumnDef& column : def.columns) {
        put_be32(&out, static_cast<std::uint32_t>(column.name.size()));
        out.append(column.name);
        out.push_back(static_cast<char>(column.type));
        out.push_back(static_cast<char>((column.primary_key ? 1 : 0) | (column.not_null ? 2 : 0)));
    }
    return out;
}

bool decode_table_def(Bytes stored, TableDef* out) {
    const auto* p = reinterpret_cast<const unsigned char*>(stored.data());
    const std::size_t n = stored.size();
    std::size_t at = 0;

    auto need = [&](std::size_t bytes) { return at + bytes <= n; };

    if (!need(16)) {
        return false;
    }
    out->id = read_be32(p + at);
    at += 4;
    out->next_row_id = read_be64(p + at);
    at += 8;

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
    const std::uint32_t column_count = read_be32(p + at);
    at += 4;

    out->columns.clear();
    out->columns.reserve(column_count);
    for (std::uint32_t i = 0; i < column_count; ++i) {
        if (!need(4)) {
            return false;
        }
        const std::uint32_t len = read_be32(p + at);
        at += 4;
        if (!need(len + 2)) {
            return false;
        }
        ColumnDef column;
        column.name.assign(reinterpret_cast<const char*>(p + at), len);
        at += len;
        column.type = static_cast<Type>(p[at++]);
        const unsigned char flags = p[at++];
        column.primary_key = (flags & 1) != 0;
        column.not_null = (flags & 2) != 0;
        out->columns.push_back(std::move(column));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

Status Catalog::allocate_table_id(TableId* out) {
    const std::string key = catalog_sequence_key();
    TableId next = 1;

    std::string stored;
    if (Status s = txn_->get(as_bytes(key), &stored); s) {
        if (stored.size() != 4) {
            return Status::corruption("catalog sequence record is malformed");
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

Status Catalog::create(const CreateTable& statement, TableDef* out) {
    TableDef existing;
    const Status found = lookup(statement.table, &existing);
    if (found) {
        if (statement.if_not_exists) {
            *out = existing;
            return Status::ok();
        }
        return Status::invalid_argument("table '" + statement.table + "' already exists");
    }
    if (found.code() != Code::NotFound) {
        return found;
    }

    // Duplicate column names would make every later lookup ambiguous.
    for (std::size_t i = 0; i < statement.columns.size(); ++i) {
        for (std::size_t j = i + 1; j < statement.columns.size(); ++j) {
            if (statement.columns[i].name == statement.columns[j].name) {
                return Status::invalid_argument("duplicate column name '" +
                                                statement.columns[i].name + "'");
            }
        }
    }

    TableDef def;
    def.name = statement.table;
    def.columns = statement.columns;
    def.next_row_id = 1;
    if (Status s = allocate_table_id(&def.id); !s) {
        return s;
    }
    if (Status s = save(def); !s) {
        return s;
    }
    *out = std::move(def);
    return Status::ok();
}

Status Catalog::drop(const DropTable& statement) {
    TableDef def;
    const Status found = lookup(statement.table, &def);
    if (!found) {
        if (found.code() == Code::NotFound && statement.if_exists) {
            return Status::ok();
        }
        return found.code() == Code::NotFound
                   ? Status::invalid_argument("no such table: " + statement.table)
                   : found;
    }

    // The rows go too. Without this their keys would be inherited by whatever
    // table is allocated that id next.
    std::vector<std::string> to_delete;
    const std::string prefix = table_range_start(def.id);
    // Collect first: deleting through a live cursor is not supported.
    // (Cursors are invalidated by any write — see BTree::Cursor.)
    {
        std::vector<std::pair<std::string, std::string>> rows;
        // Scanning the whole key space is acceptable here because DROP is rare
        // and the alternative is a second index over table ids.
        if (Status s = txn_->scan_prefix(prefix, &rows); !s) {
            return s;
        }
        for (const auto& [key, value] : rows) {
            (void)value;
            to_delete.push_back(key);
        }
    }
    for (const std::string& key : to_delete) {
        if (Status s = txn_->remove(as_bytes(key)); !s) {
            return s;
        }
    }

    return txn_->remove(as_bytes(catalog_key(statement.table)));
}

Status Catalog::lookup(const std::string& table, TableDef* out) {
    std::string stored;
    if (Status s = txn_->get(as_bytes(catalog_key(table)), &stored); !s) {
        return s;
    }
    if (!decode_table_def(as_bytes(stored), out)) {
        return Status::corruption("catalog entry for '" + table + "' is malformed");
    }
    return Status::ok();
}

Status Catalog::save(const TableDef& def) {
    const std::string encoded = encode_table_def(def);
    return txn_->put(as_bytes(catalog_key(def.name)), as_bytes(encoded));
}

} // namespace strata::sql
