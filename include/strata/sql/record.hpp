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

/// A row on disk: a count, then a type byte and payload per value.
///
/// Values are self-describing rather than laid out from the schema, so a row
/// written before a column was added still decodes. Nothing in stage 4 alters
/// a schema, but paying one byte per value now is cheaper than a migration
/// later.
std::string encode_row(const Row& row);
bool decode_row(Bytes stored, Row* out);

} // namespace strata::sql
