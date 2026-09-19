#include "strata/pager.hpp"

#include <cstring>

namespace strata {

Status Pager::open(const std::string& path) {
    close();

    if (Status s = file_.open(path); !s) {
        return s;
    }
    if (Status s = wal_.open(path + "-wal"); !s) {
        return s;
    }

    const std::uint64_t file_size = file_.size();
    // The log is authoritative when it holds a commit, because a committed
    // transaction may have grown the database beyond what the file shows.
    const std::uint32_t from_wal = wal_.db_page_count();
    const std::uint32_t from_file = static_cast<std::uint32_t>(file_size / kPageSize);
    page_count_ = from_wal != 0 ? from_wal : from_file;

    open_ = true;

    if (page_count_ == 0) {
        return init_new_database();
    }
    return read_meta();
}

void Pager::close() {
    if (!open_) {
        return;
    }
    wal_.close();
    file_.close();
    cache_.clear();
    dirty_.clear();
    page_count_ = 0;
    root_ = kNoPage;
    freelist_ = kNoPage;
    open_ = false;
}

Status Pager::init_new_database() {
    // Page 0 is meta, page 1 is an empty leaf that is the initial root.
    page_count_ = 2;

    auto meta_buf = std::make_unique<PageBuffer>();
    Page meta(meta_buf->data(), kMetaPage);
    meta.init(PageType::Meta);
    std::memcpy(meta.data() + kMetaMagic, kMagic.data(), kMagic.size());
    put_u32(meta.data() + kMetaPageSize, kPageSize);
    put_u32(meta.data() + kMetaPageCount, page_count_);
    put_u32(meta.data() + kMetaRoot, 1);
    put_u32(meta.data() + kMetaFreelist, kNoPage);
    put_u64(meta.data() + kMetaNextXid, next_xid_);
    cache_[kMetaPage] = std::move(meta_buf);

    auto root_buf = std::make_unique<PageBuffer>();
    Page root(root_buf->data(), 1);
    root.init(PageType::Leaf);
    cache_[1] = std::move(root_buf);

    root_ = 1;
    freelist_ = kNoPage;
    dirty_.insert(kMetaPage);
    dirty_.insert(1);

    return commit();
}

Status Pager::read_meta() {
    Page meta(nullptr, kMetaPage);
    if (Status s = fetch(kMetaPage, &meta); !s) {
        return s;
    }
    if (std::memcmp(meta.data() + kMetaMagic, kMagic.data(), kMagic.size()) != 0) {
        return Status::corruption("not a strata database");
    }
    if (get_u32(meta.data() + kMetaPageSize) != kPageSize) {
        return Status::corruption("page size mismatch");
    }
    // The log's page count wins when it has one; see open().
    if (wal_.db_page_count() == 0) {
        page_count_ = get_u32(meta.data() + kMetaPageCount);
    }
    root_ = get_u32(meta.data() + kMetaRoot);
    freelist_ = get_u32(meta.data() + kMetaFreelist);
    next_xid_ = get_u64(meta.data() + kMetaNextXid);
    if (next_xid_ == 0) {
        next_xid_ = 1;
    }
    return Status::ok();
}

Status Pager::write_meta() {
    Page meta(nullptr, kMetaPage);
    if (Status s = fetch(kMetaPage, &meta); !s) {
        return s;
    }
    put_u32(meta.data() + kMetaPageCount, page_count_);
    put_u32(meta.data() + kMetaRoot, root_);
    put_u32(meta.data() + kMetaFreelist, freelist_);
    put_u64(meta.data() + kMetaNextXid, next_xid_);
    mark_dirty(kMetaPage);
    return Status::ok();
}

void Pager::set_next_xid(std::uint64_t value) {
    next_xid_ = value;
    (void)write_meta();
}

Status Pager::load_page(PageId id, PageBuffer* buffer) {
    // The log holds the newest committed image of any page it has seen.
    if (wal_.contains(id)) {
        return wal_.read_page(id, buffer->data());
    }

    const std::uint64_t offset = static_cast<std::uint64_t>(id) * kPageSize;
    if (offset + kPageSize > file_.size()) {
        // Committed but never checkpointed, and not in the log either: this is
        // a page the current transaction grew into. Hand back a blank.
        std::memset(buffer->data(), 0, kPageSize);
        return Status::ok();
    }
    if (Status s = file_.read_at(offset, buffer->data(), kPageSize); !s) {
        return s;
    }

    Page p(buffer->data(), id);
    if (!p.verify()) {
        return Status::corruption("page checksum mismatch on page " + std::to_string(id));
    }
    return Status::ok();
}

Status Pager::fetch(PageId id, Page* out) {
    if (!open_) {
        return Status::io_error("pager is not open");
    }
    if (id >= page_count_) {
        return Status::invalid_argument("page " + std::to_string(id) + " is past end of database");
    }

    ++fetches_;
    if (const auto it = cache_.find(id); it != cache_.end()) {
        *out = Page(it->second->data(), id);
        return Status::ok();
    }

    ++loads_;
    auto buffer = std::make_unique<PageBuffer>();
    if (Status s = load_page(id, buffer.get()); !s) {
        return s;
    }
    std::byte* data = buffer->data();
    cache_[id] = std::move(buffer);
    *out = Page(data, id);
    return Status::ok();
}

Status Pager::allocate(PageType type, Page* out) {
    if (!open_) {
        return Status::io_error("pager is not open");
    }

    const PageId id = page_count_;
    ++page_count_;

    auto buffer = std::make_unique<PageBuffer>();
    std::byte* data = buffer->data();
    cache_[id] = std::move(buffer);

    Page p(data, id);
    p.init(type);
    dirty_.insert(id);

    *out = p;
    return write_meta().ok_status() ? Status::ok() : Status::io_error("meta update failed");
}

void Pager::set_root(PageId id) {
    root_ = id;
    (void)write_meta();
}

Status Pager::commit() {
    if (dirty_.empty()) {
        return Status::ok();
    }

    // The meta page carries page_count and root, both of which may have moved.
    if (Status s = write_meta(); !s) {
        return s;
    }

    const Lsn lsn = wal_.next_lsn();
    for (const PageId id : dirty_) {
        const auto it = cache_.find(id);
        if (it == cache_.end()) {
            continue;
        }
        Page p(it->second->data(), id);
        p.set_lsn(lsn);
        p.seal();
        wal_.stage(id, it->second->data());
    }

    if (Status s = wal_.commit(page_count_); !s) {
        return s;
    }
    wal_.bump_lsn();
    dirty_.clear();
    return Status::ok();
}

void Pager::rollback() {
    wal_.rollback();
    // Evict the modified images so the next fetch reloads the committed state.
    for (const PageId id : dirty_) {
        cache_.erase(id);
    }
    dirty_.clear();
    // page_count_ and root_ live on the meta page, which was just evicted if it
    // was dirty; re-read them from the committed image.
    (void)read_meta();
}

Status Pager::checkpoint() {
    if (!dirty_.empty()) {
        if (Status s = commit(); !s) {
            return s;
        }
    }
    return wal_.checkpoint(file_);
}

} // namespace strata
