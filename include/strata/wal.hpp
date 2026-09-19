#pragma once

#include "strata/file.hpp"
#include "strata/types.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata {

/// Write-ahead log.
///
/// Every page modification is appended here before the database file is
/// touched, and the database file is only updated during a checkpoint. That
/// ordering is the whole of crash safety: the database file is always either
/// the last checkpointed state, or a state the log can replay onto.
///
/// The design follows SQLite's WAL rather than ARIES-style undo/redo. See
/// decision 002 for why, and what it costs.
///
/// File layout:
///
///   header   32 bytes    magic, page size, generation salts, checksum
///   frame    24 + 4096   page number, commit marker, salts, running checksum
///   frame    ...
///
/// A frame is valid only if its salts match the header *and* its running
/// checksum matches. The checksum chains through every preceding frame, so a
/// frame left over from an earlier generation cannot be mistaken for a current
/// one even when its own bytes are intact.
///
/// A transaction's frames are staged in memory and written in one burst at
/// commit, with the commit marker on the last frame. Nothing uncommitted ever
/// reaches the file, which makes recovery a plain forward scan with no undo
/// pass. The cost is that a transaction's dirty set must fit in memory — see
/// decision 005.
class Wal {
public:
    static constexpr std::uint32_t kHeaderSize = 32;
    static constexpr std::uint32_t kFrameHeaderSize = 24;
    static constexpr std::uint32_t kFrameSize = kFrameHeaderSize + kPageSize;

    Wal() = default;

    /// Opens (or creates) the log and replays it. After open(), the index
    /// reflects every committed frame and nothing uncommitted.
    Status open(const std::string& path);

    void close() { file_.close(); }

    /// True if this page has a committed version in the log newer than the one
    /// in the database file.
    bool contains(PageId id) const { return index_.find(id) != index_.end(); }

    /// Reads a page out of the log. Only valid when contains(id) is true.
    Status read_page(PageId id, std::byte* out);

    /// Stages one page image for the current transaction. Nothing is written
    /// until commit().
    void stage(PageId id, const std::byte* page);

    /// Writes every staged frame, marks the last one as a commit, and makes the
    /// lot durable. This is the single fsync on the commit path.
    Status commit(std::uint32_t db_page_count);

    /// Throws away staged frames without writing them.
    void rollback() { stage_.clear(); }

    /// Copies every committed frame into the database file, syncs it, then
    /// resets the log. Safe to interrupt: the log is reset only after the
    /// database file is durable, so a crash mid-checkpoint simply replays.
    Status checkpoint(File& db_file);

    std::size_t committed_page_count() const { return index_.size(); }
    std::size_t staged_frame_count() const { return stage_.size(); }

    /// Page count of the database as of the last commit. Zero when the log
    /// holds no commit, in which case the database file's size is
    /// authoritative.
    std::uint32_t db_page_count() const { return db_page_count_; }

    Lsn next_lsn() const { return next_lsn_; }
    void bump_lsn() { ++next_lsn_; }

private:
    struct StagedFrame {
        PageId id;
        std::array<std::byte, kPageSize> data;
    };

    Status write_header();
    Status read_header();
    Status replay();

    /// Chained checksum: each frame folds in the previous frame's value, so
    /// validity is only ever established for a prefix of the log.
    static std::uint64_t frame_checksum(std::uint64_t prev, const std::byte* frame_header,
                                        const std::byte* page);

    File file_;
    std::unordered_map<PageId, std::uint64_t> index_; // page id -> offset of its newest frame
    std::vector<StagedFrame> stage_;

    std::uint32_t salt1_ = 0;
    std::uint32_t salt2_ = 0;
    std::uint64_t running_checksum_ = 0; // through the last committed frame
    std::uint64_t end_offset_ = kHeaderSize;
    std::uint32_t db_page_count_ = 0;
    Lsn next_lsn_ = 1;
};

} // namespace strata
