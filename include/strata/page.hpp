#pragma once

#include "strata/types.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace strata {

enum class PageType : std::uint8_t {
    Free = 0,
    Meta = 1,
    Internal = 2,
    Leaf = 3,
};

/// Page header, 24 bytes, little-endian on disk.
///
///   0  u32  checksum   over bytes [4, kPageSize)
///   4  u8   type
///   5  u8   flags      (reserved, always 0)
///   6  u16  cell_count
///   8  u16  free_start end of the cell-pointer array
///  10  u16  free_end   start of the cell content area, grows downward
///  12  u32  extra      internal: rightmost child. leaf: next leaf, for scans.
///  16  u64  lsn        last WAL frame that modified this page
inline constexpr std::uint16_t kPageHeaderSize = 24;

/// Offsets into the header, named so the encode/decode calls read as prose.
inline constexpr std::uint16_t kOffChecksum = 0;
inline constexpr std::uint16_t kOffType = 4;
inline constexpr std::uint16_t kOffFlags = 5;
inline constexpr std::uint16_t kOffCellCount = 6;
inline constexpr std::uint16_t kOffFreeStart = 8;
inline constexpr std::uint16_t kOffFreeEnd = 10;
inline constexpr std::uint16_t kOffExtra = 12;
inline constexpr std::uint16_t kOffLsn = 16;

// --- little-endian primitives ------------------------------------------------

inline void put_u16(std::byte* p, std::uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}

inline void put_u32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

inline void put_u64(std::byte* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

inline std::uint16_t get_u16(const std::byte* p) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(p[0]) |
                                      (std::to_integer<std::uint16_t>(p[1]) << 8));
}

inline std::uint32_t get_u32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= std::to_integer<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

inline std::uint64_t get_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= std::to_integer<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

/// FNV-1a over the page body. Not cryptographic — it exists to catch a torn
/// write, which is a bit-level accident rather than an adversary.
std::uint32_t checksum_of(const std::byte* data, std::size_t len);

/// A key/value pair as it appears inside a leaf page.
struct LeafCell {
    Bytes key;
    Bytes value;
};

/// A separator key and the child it points at, inside an internal page.
struct InternalCell {
    Bytes key;
    PageId child;
};

/// A slotted page.
///
/// The cell-pointer array grows forward from the header; cell content grows
/// backward from the end of the page. Free space is whatever sits between them,
/// which makes "does this fit" a single subtraction.
///
/// Page does not own its memory. It is a view over a 4 KiB buffer owned by the
/// Pager's cache, so that writing through a Page marks that buffer dirty
/// without any copying.
class Page {
public:
    Page(std::byte* data, PageId id) : data_(data), id_(id) {}

    PageId id() const { return id_; }
    std::byte* data() { return data_; }
    const std::byte* data() const { return data_; }

    // --- header ---
    PageType type() const {
        return static_cast<PageType>(std::to_integer<std::uint8_t>(data_[kOffType]));
    }
    void set_type(PageType t) { data_[kOffType] = static_cast<std::byte>(t); }

    std::uint16_t cell_count() const { return get_u16(data_ + kOffCellCount); }
    void set_cell_count(std::uint16_t n) { put_u16(data_ + kOffCellCount, n); }

    std::uint16_t free_start() const { return get_u16(data_ + kOffFreeStart); }
    void set_free_start(std::uint16_t v) { put_u16(data_ + kOffFreeStart, v); }

    std::uint16_t free_end() const { return get_u16(data_ + kOffFreeEnd); }
    void set_free_end(std::uint16_t v) { put_u16(data_ + kOffFreeEnd, v); }

    PageId extra() const { return get_u32(data_ + kOffExtra); }
    void set_extra(PageId v) { put_u32(data_ + kOffExtra, v); }

    Lsn lsn() const { return get_u64(data_ + kOffLsn); }
    void set_lsn(Lsn v) { put_u64(data_ + kOffLsn, v); }

    /// Bytes available for one more cell, accounting for its slot pointer.
    std::uint16_t free_space() const {
        const std::uint16_t start = free_start();
        const std::uint16_t end = free_end();
        return end > start ? static_cast<std::uint16_t>(end - start) : std::uint16_t{0};
    }

    void init(PageType t);

    // --- checksum ---
    void seal() { put_u32(data_ + kOffChecksum, checksum_of(data_ + 4, kPageSize - 4)); }
    bool verify() const {
        return get_u32(data_ + kOffChecksum) == checksum_of(data_ + 4, kPageSize - 4);
    }

    // --- cells ---
    std::uint16_t slot_offset(std::uint16_t i) const {
        return get_u16(data_ + kPageHeaderSize + i * 2);
    }
    void set_slot_offset(std::uint16_t i, std::uint16_t off) {
        put_u16(data_ + kPageHeaderSize + i * 2, off);
    }

    LeafCell leaf_cell(std::uint16_t i) const;
    InternalCell internal_cell(std::uint16_t i) const;

    /// Repoints an existing separator at a different child. The cell's size
    /// does not change, so this is an in-place overwrite — which is what makes
    /// a split cheap on the parent side.
    void set_internal_child(std::uint16_t i, PageId child) {
        put_u32(data_ + slot_offset(i), child);
    }

    /// Largest key+value a cell may carry. Two must fit in an empty page or a
    /// split cannot make progress. Anything larger needs overflow pages, which
    /// are deliberately out of scope — see FUTURE.md.
    static constexpr std::uint16_t max_payload() {
        return static_cast<std::uint16_t>((kPageSize - kPageHeaderSize) / 2 - 8);
    }

    /// Key of cell i regardless of page type — the comparison path does not
    /// care which kind of page it is walking.
    Bytes key_at(std::uint16_t i) const;

    /// Bytes a leaf cell with these lengths will occupy, slot pointer included.
    static std::uint16_t leaf_cell_size(std::size_t key_len, std::size_t value_len) {
        return static_cast<std::uint16_t>(4 + key_len + value_len + 2);
    }
    static std::uint16_t internal_cell_size(std::size_t key_len) {
        return static_cast<std::uint16_t>(6 + key_len + 2);
    }

    /// Insert at slot index i, shifting later slots right. Returns Full if the
    /// cell does not fit; the caller splits and retries.
    Status insert_leaf_cell(std::uint16_t i, Bytes key, Bytes value);
    Status insert_internal_cell(std::uint16_t i, Bytes key, PageId child);

    /// Remove slot i. The cell's bytes are orphaned rather than reclaimed,
    /// because reclaiming them means moving every cell after it. Call compact()
    /// to get the space back. See decision 004.
    void remove_cell(std::uint16_t i);

    /// Rewrites the content area so that the orphaned bytes left behind by
    /// remove_cell become free space again. Cell order and contents are
    /// unchanged, so any index computed before the call is still valid.
    ///
    /// Without this a page that has been split is unusable: it has half the
    /// cells but none of the space, because free_end never moved back.
    void compact();

    /// First slot whose key is >= the probe, plus whether it matched exactly.
    /// Binary search: pages hold hundreds of cells and this runs on every level
    /// of every descent.
    std::pair<std::uint16_t, bool> lower_bound(Bytes key) const;

private:
    std::byte* data_;
    PageId id_;
};

/// Lexicographic byte comparison. Keys are opaque, so this is the only ordering
/// the storage engine knows about; typed ordering belongs to the SQL layer.
int compare_keys(Bytes a, Bytes b);

} // namespace strata
