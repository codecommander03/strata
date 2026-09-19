#pragma once

#include "strata/file.hpp"
#include "strata/page.hpp"
#include "strata/types.hpp"
#include "strata/wal.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace strata {

/// Meta page field offsets, laid out after the common page header so that the
/// checksum and type fields work on page 0 exactly as they do everywhere else.
inline constexpr std::uint16_t kMetaMagic = kPageHeaderSize; // 8 bytes
inline constexpr std::uint16_t kMetaPageSize = kPageHeaderSize + 8;
inline constexpr std::uint16_t kMetaPageCount = kPageHeaderSize + 12;
inline constexpr std::uint16_t kMetaRoot = kPageHeaderSize + 16;
inline constexpr std::uint16_t kMetaFreelist = kPageHeaderSize + 20;
inline constexpr std::uint16_t kMetaNextXid = kPageHeaderSize + 24; // 8 bytes

/// Owns the database file, the page cache and the log.
///
/// Everything above this layer works in terms of Page objects and never touches
/// a file offset. Everything below is bytes.
class Pager {
public:
    Pager() = default;
    ~Pager() { close(); }

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    /// Opens or creates the database at `path`, with its log at `path + "-wal"`.
    /// A newly created database gets a meta page and an empty leaf root.
    Status open(const std::string& path);
    void close();

    /// Returns a view of the page, reading it from the log or the file if it is
    /// not already cached. The view stays valid until close().
    Status fetch(PageId id, Page* out);

    /// Grows the file by one page and returns it, already initialised and
    /// marked dirty.
    Status allocate(PageType type, Page* out);

    /// Records that a cached page has been modified and must be logged at
    /// commit. Cheap to call repeatedly.
    void mark_dirty(PageId id) { dirty_.insert(id); }

    /// Seals every dirty page, stages it in the log, and commits. One fsync.
    Status commit();

    /// Drops uncommitted changes. Dirty pages are evicted so the next fetch
    /// re-reads the committed image.
    void rollback();

    /// Folds the log back into the database file and resets it.
    Status checkpoint();

    std::uint32_t page_count() const { return page_count_; }

    PageId root() const { return root_; }
    void set_root(PageId id);

    /// The next transaction id to hand out. Lives on the meta page because
    /// reusing an id across a restart would make old versions visible to new
    /// transactions that should not see them.
    std::uint64_t next_xid() const { return next_xid_; }
    void set_next_xid(std::uint64_t value);

    /// Test and diagnostic hooks.
    std::size_t cached_page_count() const { return cache_.size(); }
    std::size_t dirty_page_count() const { return dirty_.size(); }
    Wal& wal() { return wal_; }

    /// How many pages a query asked for, and how many of those had to be read
    /// rather than served from cache. The playground shows both, because the
    /// gap between them is the whole reason a page cache exists.
    std::uint64_t fetches() const { return fetches_; }
    std::uint64_t page_loads() const { return loads_; }
    void reset_counters() {
        fetches_ = 0;
        loads_ = 0;
    }

private:
    using PageBuffer = std::array<std::byte, kPageSize>;

    Status load_page(PageId id, PageBuffer* buffer);
    Status init_new_database();
    Status read_meta();
    Status write_meta();

    File file_;
    Wal wal_;
    std::unordered_map<PageId, std::unique_ptr<PageBuffer>> cache_;
    std::unordered_set<PageId> dirty_;
    std::uint32_t page_count_ = 0;
    PageId root_ = kNoPage;
    PageId freelist_ = kNoPage;
    std::uint64_t next_xid_ = 1;
    std::uint64_t fetches_ = 0;
    std::uint64_t loads_ = 0;
    bool open_ = false;
};

} // namespace strata
