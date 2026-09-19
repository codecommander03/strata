#include "strata/wal.hpp"

#include "strata/page.hpp"

#include <chrono>
#include <cstring>
#include <random>

namespace strata {
namespace {

constexpr std::string_view kWalMagic = "STRATAWL";

// Header field offsets.
constexpr std::uint32_t kHdrMagic = 0;
constexpr std::uint32_t kHdrPageSize = 8;
constexpr std::uint32_t kHdrSalt1 = 12;
constexpr std::uint32_t kHdrSalt2 = 16;
constexpr std::uint32_t kHdrReserved = 20;
constexpr std::uint32_t kHdrChecksum = 24;

// Frame header field offsets.
constexpr std::uint32_t kFrPageNo = 0;
constexpr std::uint32_t kFrDbSize = 4; // non-zero marks a commit frame
constexpr std::uint32_t kFrSalt1 = 8;
constexpr std::uint32_t kFrSalt2 = 12;
constexpr std::uint32_t kFrChecksum = 16;

std::uint64_t fnv1a64(std::uint64_t seed, const std::byte* data, std::size_t len) {
    std::uint64_t hash = seed != 0 ? seed : 1469598103934665603ull;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= std::to_integer<std::uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint32_t random_salt() {
    static std::mt19937 rng(
        static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return rng();
}

} // namespace

std::uint64_t Wal::frame_checksum(std::uint64_t prev, const std::byte* frame_header,
                                  const std::byte* page) {
    // Everything but the checksum field itself, then the page image.
    const std::uint64_t h = fnv1a64(prev, frame_header, kFrChecksum);
    return fnv1a64(h, page, kPageSize);
}

Status Wal::open(const std::string& path) {
    if (Status s = file_.open(path); !s) {
        return s;
    }

    if (file_.size() < kHeaderSize) {
        salt1_ = random_salt();
        salt2_ = random_salt();
        if (Status s = write_header(); !s) {
            return s;
        }
        index_.clear();
        end_offset_ = kHeaderSize;
        db_page_count_ = 0;
        return Status::ok();
    }

    if (Status s = read_header(); !s) {
        return s;
    }
    return replay();
}

Status Wal::write_header() {
    std::array<std::byte, kHeaderSize> hdr{};
    std::memcpy(hdr.data() + kHdrMagic, kWalMagic.data(), kWalMagic.size());
    put_u32(hdr.data() + kHdrPageSize, kPageSize);
    put_u32(hdr.data() + kHdrSalt1, salt1_);
    put_u32(hdr.data() + kHdrSalt2, salt2_);
    put_u32(hdr.data() + kHdrReserved, 0);
    const std::uint64_t sum = fnv1a64(0, hdr.data(), kHdrChecksum);
    put_u64(hdr.data() + kHdrChecksum, sum);

    if (Status s = file_.write_at(0, hdr.data(), hdr.size()); !s) {
        return s;
    }
    running_checksum_ = sum;
    return file_.sync();
}

Status Wal::read_header() {
    std::array<std::byte, kHeaderSize> hdr{};
    if (Status s = file_.read_at(0, hdr.data(), hdr.size()); !s) {
        return s;
    }
    if (std::memcmp(hdr.data() + kHdrMagic, kWalMagic.data(), kWalMagic.size()) != 0) {
        return Status::corruption("wal magic mismatch");
    }
    if (get_u32(hdr.data() + kHdrPageSize) != kPageSize) {
        return Status::corruption("wal page size mismatch");
    }
    const std::uint64_t stored = get_u64(hdr.data() + kHdrChecksum);
    const std::uint64_t computed = fnv1a64(0, hdr.data(), kHdrChecksum);
    if (stored != computed) {
        return Status::corruption("wal header checksum mismatch");
    }
    salt1_ = get_u32(hdr.data() + kHdrSalt1);
    salt2_ = get_u32(hdr.data() + kHdrSalt2);
    running_checksum_ = computed;
    return Status::ok();
}

Status Wal::replay() {
    index_.clear();
    db_page_count_ = 0;

    const std::uint64_t file_size = file_.size();
    std::uint64_t offset = kHeaderSize;
    std::uint64_t running = running_checksum_;

    // Frames seen since the last commit. They only become visible if a commit
    // frame follows; otherwise they were a torn transaction and are discarded.
    std::vector<std::pair<PageId, std::uint64_t>> tentative;

    std::array<std::byte, kFrameHeaderSize> fh{};
    std::vector<std::byte> page(kPageSize);

    std::uint64_t committed_end = kHeaderSize;

    while (offset + kFrameSize <= file_size) {
        if (Status s = file_.read_at(offset, fh.data(), fh.size()); !s) {
            break;
        }
        if (Status s = file_.read_at(offset + kFrameHeaderSize, page.data(), page.size()); !s) {
            break;
        }

        // A frame from an earlier generation of the log has stale salts.
        if (get_u32(fh.data() + kFrSalt1) != salt1_ || get_u32(fh.data() + kFrSalt2) != salt2_) {
            break;
        }

        const std::uint64_t expect = frame_checksum(running, fh.data(), page.data());
        if (get_u64(fh.data() + kFrChecksum) != expect) {
            break; // torn write: this frame and everything after it is gone
        }

        running = expect;
        const PageId id = get_u32(fh.data() + kFrPageNo);
        tentative.emplace_back(id, offset);

        const std::uint32_t db_size = get_u32(fh.data() + kFrDbSize);
        if (db_size != 0) {
            for (const auto& [pid, off] : tentative) {
                index_[pid] = off;
            }
            tentative.clear();
            db_page_count_ = db_size;
            running_checksum_ = running;
            committed_end = offset + kFrameSize;
        }

        offset += kFrameSize;
    }

    // Anything past the last commit never happened.
    end_offset_ = committed_end;
    return Status::ok();
}

Status Wal::read_page(PageId id, std::byte* out) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return Status::not_found("page not in wal");
    }
    return file_.read_at(it->second + kFrameHeaderSize, out, kPageSize);
}

void Wal::stage(PageId id, const std::byte* page) {
    // Last write of a page within a transaction wins; no point logging both.
    for (auto& f : stage_) {
        if (f.id == id) {
            std::memcpy(f.data.data(), page, kPageSize);
            return;
        }
    }
    StagedFrame frame;
    frame.id = id;
    std::memcpy(frame.data.data(), page, kPageSize);
    stage_.push_back(std::move(frame));
}

Status Wal::commit(std::uint32_t db_page_count) {
    if (stage_.empty()) {
        return Status::ok();
    }

    std::uint64_t offset = end_offset_;
    std::uint64_t running = running_checksum_;
    std::array<std::byte, kFrameHeaderSize> fh{};

    for (std::size_t i = 0; i < stage_.size(); ++i) {
        const bool last = (i + 1 == stage_.size());
        std::memset(fh.data(), 0, fh.size());
        put_u32(fh.data() + kFrPageNo, stage_[i].id);
        put_u32(fh.data() + kFrDbSize, last ? db_page_count : 0u);
        put_u32(fh.data() + kFrSalt1, salt1_);
        put_u32(fh.data() + kFrSalt2, salt2_);
        running = frame_checksum(running, fh.data(), stage_[i].data.data());
        put_u64(fh.data() + kFrChecksum, running);

        if (Status s = file_.write_at(offset, fh.data(), fh.size()); !s) {
            return s;
        }
        if (Status s = file_.write_at(offset + kFrameHeaderSize, stage_[i].data.data(), kPageSize);
            !s) {
            return s;
        }
        offset += kFrameSize;
    }

    // The frames exist on disk only after this returns. Before it, a crash
    // loses the whole transaction, which is the correct outcome.
    if (Status s = file_.sync(); !s) {
        return s;
    }

    std::uint64_t at = end_offset_;
    for (const auto& f : stage_) {
        index_[f.id] = at;
        at += kFrameSize;
    }
    end_offset_ = offset;
    running_checksum_ = running;
    db_page_count_ = db_page_count;
    stage_.clear();
    return Status::ok();
}

Status Wal::checkpoint(File& db_file) {
    if (index_.empty()) {
        return Status::ok();
    }

    std::vector<std::byte> page(kPageSize);
    for (const auto& [id, offset] : index_) {
        if (Status s = file_.read_at(offset + kFrameHeaderSize, page.data(), page.size()); !s) {
            return s;
        }
        if (Status s = db_file.write_at(static_cast<std::uint64_t>(id) * kPageSize, page.data(),
                                        page.size());
            !s) {
            return s;
        }
    }

    // The database file must be durable before the log is thrown away.
    // Interrupting anywhere before this line just means replaying again.
    if (Status s = db_file.sync(); !s) {
        return s;
    }

    if (Status s = file_.truncate_to_empty(); !s) {
        return s;
    }
    salt1_ = random_salt();
    salt2_ = random_salt();
    if (Status s = write_header(); !s) {
        return s;
    }
    index_.clear();
    end_offset_ = kHeaderSize;
    return Status::ok();
}

} // namespace strata
