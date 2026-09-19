#pragma once

#include "strata/sql/ast.hpp"
#include "strata/sql/value.hpp"
#include "strata/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace strata::sql {

using Row = std::vector<Value>;
using RowId = std::uint64_t;
using TableId = std::uint32_t;

/// The key space inside the single B+tree.
///
/// One tree holds everything, so the first byte of every key says what kind of
/// thing it is. The prefixes are 0x01 and 0x02 rather than 0x00 because the
/// MVCC layer escapes zero bytes, and keeping them out of the prefix makes the
/// encoded keys easier to read in a hex dump.
inline constexpr char kCatalogPrefix = '\x01';
inline constexpr char kRowPrefix = '\x02';
/// `0x03 index_id(BE32) encoded_value row_id(BE64)` -> empty. The row id lives
/// in the key rather than the value, so duplicate values are simply adjacent
/// entries and there is no bucket to manage.
inline constexpr char kIndexEntryPrefix = '\x03';
/// `0x04 index_name` -> IndexDef, and `0x04` alone -> the id sequence. A
/// namespace of its own rather than sharing the table catalog, so adding
/// indexes could not disturb how tables are read.
inline constexpr char kIndexPrefix = '\x04';

using IndexId = std::uint32_t;

// --- big-endian primitives ---------------------------------------------------
//
// Big-endian, unlike the little-endian page fields, because these bytes go
// *into keys*: byte order has to equal numeric order or a range scan over a
// table id or a row id walks the wrong rows.

inline void put_be32(std::string* out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

inline void put_be64(std::string* out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

inline std::uint32_t read_be32(const unsigned char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

inline std::uint64_t read_be64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

/// `0x01` alone: the record holding the next table id to allocate.
std::string catalog_sequence_key();

/// `0x01 name`: one table's definition.
std::string catalog_key(const std::string& table);

/// `0x02 table_id(BE32) row_id(BE64)`. Big-endian so that byte order over the
/// key is numeric order over the id, which is what makes a table scan a
/// contiguous range rather than a lookup per row.
std::string row_key(TableId table, RowId row);

/// The shortest key that is >= every row key of this table. Seek here and
/// every row of the table comes out in row-id order.
std::string table_range_start(TableId table);

/// True if `key` is a row of this table.
bool key_belongs_to_table(const std::string& key, TableId table);

/// Pulls the row id back out of a row key.
bool decode_row_key(const std::string& key, TableId* table, RowId* row);

/// `0x04` alone: the record holding the next index id to allocate.
std::string index_sequence_key();

/// `0x04 name`: one index's definition.
std::string index_catalog_key(const std::string& name);

/// `0x03 index_id`: everything under one index, in value order.
std::string index_range_start(IndexId index);

/// `0x03 index_id encoded_value`: everything with one value, in row-id order.
/// Seeking here lands on the first matching entry.
std::string index_seek_key(IndexId index, const std::string& encoded_value);

/// A full index entry, value and row id.
std::string index_entry_key(IndexId index, const std::string& encoded_value, RowId row);

/// Splits an entry back apart. The row id is the last eight bytes; everything
/// between the header and it is the encoded value.
bool decode_index_entry(const std::string& key, IndexId* index, std::string* encoded_value,
                        RowId* row);

/// A row on disk: a count, then a type byte and payload per value.
///
/// Values are self-describing rather than laid out from the schema, so a row
/// written before a column was added still decodes. Nothing in stage 4 alters
/// a schema, but paying one byte per value now is cheaper than a migration
/// later.
std::string encode_row(const Row& row);
bool decode_row(Bytes stored, Row* out);

} // namespace strata::sql
