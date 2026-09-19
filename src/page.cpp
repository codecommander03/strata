#include "strata/page.hpp"

#include <algorithm>
#include <cstring>

namespace strata {

std::uint32_t checksum_of(const std::byte* data, std::size_t len) {
    // FNV-1a, 32-bit.
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= std::to_integer<std::uint32_t>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

int compare_keys(Bytes a, Bytes b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n > 0) {
        const int c = std::memcmp(a.data(), b.data(), n);
        if (c != 0) {
            return c;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

void Page::init(PageType t) {
    std::memset(data_, 0, kPageSize);
    set_type(t);
    set_cell_count(0);
    set_free_start(kPageHeaderSize);
    set_free_end(static_cast<std::uint16_t>(kPageSize));
    set_extra(kNoPage);
    set_lsn(0);
}

LeafCell Page::leaf_cell(std::uint16_t i) const {
    const std::uint16_t off = slot_offset(i);
    const std::uint16_t key_len = get_u16(data_ + off);
    const std::uint16_t val_len = get_u16(data_ + off + 2);
    return LeafCell{
        Bytes(data_ + off + 4, key_len),
        Bytes(data_ + off + 4 + key_len, val_len),
    };
}

InternalCell Page::internal_cell(std::uint16_t i) const {
    const std::uint16_t off = slot_offset(i);
    const PageId child = get_u32(data_ + off);
    const std::uint16_t key_len = get_u16(data_ + off + 4);
    return InternalCell{
        Bytes(data_ + off + 6, key_len),
        child,
    };
}

Bytes Page::key_at(std::uint16_t i) const {
    if (type() == PageType::Leaf) {
        return leaf_cell(i).key;
    }
    return internal_cell(i).key;
}

Status Page::insert_leaf_cell(std::uint16_t i, Bytes key, Bytes value) {
    const std::uint16_t content = static_cast<std::uint16_t>(4 + key.size() + value.size());
    const std::uint16_t needed = static_cast<std::uint16_t>(content + 2);
    if (free_space() < needed) {
        return Status::full("leaf cell does not fit");
    }

    const std::uint16_t count = cell_count();
    const std::uint16_t offset = static_cast<std::uint16_t>(free_end() - content);

    std::byte* cell = data_ + offset;
    put_u16(cell, static_cast<std::uint16_t>(key.size()));
    put_u16(cell + 2, static_cast<std::uint16_t>(value.size()));
    if (!key.empty()) {
        std::memcpy(cell + 4, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(cell + 4 + key.size(), value.data(), value.size());
    }

    // Shift the slots at and after i one place right, then claim slot i.
    std::byte* slots = data_ + kPageHeaderSize;
    std::memmove(slots + (i + 1) * 2, slots + i * 2, static_cast<std::size_t>(count - i) * 2);

    set_free_end(offset);
    set_cell_count(static_cast<std::uint16_t>(count + 1));
    set_free_start(static_cast<std::uint16_t>(free_start() + 2));
    set_slot_offset(i, offset);
    return Status::ok();
}

Status Page::insert_internal_cell(std::uint16_t i, Bytes key, PageId child) {
    const std::uint16_t content = static_cast<std::uint16_t>(6 + key.size());
    const std::uint16_t needed = static_cast<std::uint16_t>(content + 2);
    if (free_space() < needed) {
        return Status::full("internal cell does not fit");
    }

    const std::uint16_t count = cell_count();
    const std::uint16_t offset = static_cast<std::uint16_t>(free_end() - content);

    std::byte* cell = data_ + offset;
    put_u32(cell, child);
    put_u16(cell + 4, static_cast<std::uint16_t>(key.size()));
    if (!key.empty()) {
        std::memcpy(cell + 6, key.data(), key.size());
    }

    std::byte* slots = data_ + kPageHeaderSize;
    std::memmove(slots + (i + 1) * 2, slots + i * 2, static_cast<std::size_t>(count - i) * 2);

    set_free_end(offset);
    set_cell_count(static_cast<std::uint16_t>(count + 1));
    set_free_start(static_cast<std::uint16_t>(free_start() + 2));
    set_slot_offset(i, offset);
    return Status::ok();
}

void Page::remove_cell(std::uint16_t i) {
    const std::uint16_t count = cell_count();
    if (i >= count) {
        return;
    }
    std::byte* slots = data_ + kPageHeaderSize;
    std::memmove(slots + i * 2, slots + (i + 1) * 2, static_cast<std::size_t>(count - i - 1) * 2);
    set_cell_count(static_cast<std::uint16_t>(count - 1));
    set_free_start(static_cast<std::uint16_t>(free_start() - 2));
}

void Page::compact() {
    const std::uint16_t n = cell_count();
    if (n == 0) {
        set_free_start(kPageHeaderSize);
        set_free_end(static_cast<std::uint16_t>(kPageSize));
        return;
    }

    // Rebuild into a scratch image rather than shuffling in place: the cells
    // being read overlap the region being written.
    std::array<std::byte, kPageSize> scratch{};
    std::memcpy(scratch.data(), data_, kPageHeaderSize);

    Page rebuilt(scratch.data(), id_);
    rebuilt.set_cell_count(0);
    rebuilt.set_free_start(kPageHeaderSize);
    rebuilt.set_free_end(static_cast<std::uint16_t>(kPageSize));

    const bool leaf = type() == PageType::Leaf;
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::uint16_t at = rebuilt.cell_count();
        if (leaf) {
            const LeafCell cell = leaf_cell(i);
            rebuilt.insert_leaf_cell(at, cell.key, cell.value);
        } else {
            const InternalCell cell = internal_cell(i);
            rebuilt.insert_internal_cell(at, cell.key, cell.child);
        }
    }

    std::memcpy(data_, scratch.data(), kPageSize);
}

std::pair<std::uint16_t, bool> Page::lower_bound(Bytes key) const {
    std::uint16_t lo = 0;
    std::uint16_t hi = cell_count();
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        const int c = compare_keys(key_at(mid), key);
        if (c < 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    const bool exact = lo < cell_count() && compare_keys(key_at(lo), key) == 0;
    return {lo, exact};
}

} // namespace strata
