#include "strata/btree.hpp"
#include "strata/file.hpp"
#include "strata/pager.hpp"
#include "strata/wal.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace strata;
using strata::test::TempDb;

namespace {

std::string key_for(int i) {
    std::string s = std::to_string(i);
    return std::string(10 - s.size(), '0') + s;
}

/// Writes `count` keys in batches, committing each batch, and returns without
/// checkpointing — which is exactly the state a process leaves behind when it
/// is killed between commits.
void write_batches(const std::string& path, int count, int batch) {
    Pager pager;
    ASSERT_TRUE(pager.open(path));
    BTree tree(pager);
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(tree.insert(as_bytes(key_for(i)), as_bytes("v" + std::to_string(i))));
        if ((i + 1) % batch == 0) {
            ASSERT_TRUE(pager.commit());
        }
    }
    ASSERT_TRUE(pager.commit());
}

/// Chops `bytes` off the end of a file, simulating a write that was interrupted
/// partway through a frame.
void truncate_by(const std::string& path, std::uint64_t bytes) {
    const auto size = std::filesystem::file_size(path);
    ASSERT_GT(size, bytes);
    std::filesystem::resize_file(path, size - bytes);
}

TEST(Recovery, DataCommittedButNeverCheckpointedComesBack) {
    TempDb db("recover_wal_only");
    write_batches(db.path(), 800, 100);

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    BTree tree(pager);

    std::size_t keys = 0;
    ASSERT_TRUE(tree.verify_integrity(&keys));
    EXPECT_EQ(keys, 800u);

    std::string value;
    ASSERT_TRUE(tree.get(as_bytes(key_for(799)), &value));
    EXPECT_EQ(value, "v799");
}

TEST(Recovery, APartiallyWrittenFrameCostsOnlyItsOwnTransaction) {
    TempDb db("recover_torn_frame");
    write_batches(db.path(), 500, 100);

    std::size_t before = 0;
    {
        Pager pager;
        ASSERT_TRUE(pager.open(db.path()));
        BTree tree(pager);
        ASSERT_TRUE(tree.verify_integrity(&before));
    }

    // Lop off most of the final frame: the last transaction is now torn.
    truncate_by(db.wal_path(), kPageSize / 2);

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    BTree tree(pager);

    std::size_t after = 0;
    ASSERT_TRUE(tree.verify_integrity(&after)) << "tree is corrupt after recovery";
    // Some keys may be gone with the torn transaction, but the tree is intact
    // and no earlier transaction was lost.
    EXPECT_LE(after, before);
    EXPECT_GE(after, 400u) << "recovery lost more than the final batch";
}

TEST(Recovery, AnInterruptedCheckpointReplaysRatherThanLosingData) {
    TempDb db("recover_checkpoint");
    write_batches(db.path(), 600, 200);

    // A checkpoint copies pages into the database file and only then resets the
    // log. Simulate dying in the middle: scribble over one page of the database
    // file while the log still holds every committed frame.
    {
        File data_file;
        ASSERT_TRUE(data_file.open(db.path()));
        std::vector<std::byte> garbage(kPageSize, std::byte{0xEE});
        ASSERT_TRUE(data_file.write_at(kPageSize, garbage.data(), garbage.size()));
        ASSERT_TRUE(data_file.sync());
    }

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    BTree tree(pager);

    std::size_t keys = 0;
    ASSERT_TRUE(tree.verify_integrity(&keys))
        << "the log failed to mask a half-written database file";
    EXPECT_EQ(keys, 600u);
}

TEST(Recovery, SurvivesRepeatedOpenCommitCloseCycles) {
    TempDb db("recover_cycles");

    int written = 0;
    for (int cycle = 0; cycle < 12; ++cycle) {
        Pager pager;
        ASSERT_TRUE(pager.open(db.path()));
        BTree tree(pager);
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(tree.insert(as_bytes(key_for(written)), as_bytes("v")));
            ++written;
        }
        ASSERT_TRUE(pager.commit());
        // Checkpoint on alternate cycles so both paths get exercised.
        if (cycle % 2 == 1) {
            ASSERT_TRUE(pager.checkpoint());
        }
    }

    Pager pager;
    ASSERT_TRUE(pager.open(db.path()));
    BTree tree(pager);
    std::size_t keys = 0;
    ASSERT_TRUE(tree.verify_integrity(&keys));
    EXPECT_EQ(keys, static_cast<std::size_t>(written));
}

TEST(Recovery, ACorruptPageInTheDatabaseFileIsDetectedNotSilentlyServed) {
    TempDb db("recover_detect_corruption");
    {
        Pager pager;
        ASSERT_TRUE(pager.open(db.path()));
        BTree tree(pager);
        for (int i = 0; i < 400; ++i) {
            ASSERT_TRUE(tree.insert(as_bytes(key_for(i)), as_bytes("v")));
        }
        ASSERT_TRUE(pager.commit());
        ASSERT_TRUE(pager.checkpoint()); // everything now lives in the db file
    }

    // Flip a byte in a data page. With the log empty, nothing can mask it.
    {
        File data_file;
        ASSERT_TRUE(data_file.open(db.path()));
        std::byte b{};
        ASSERT_TRUE(data_file.read_at(kPageSize + 100, &b, 1));
        b ^= std::byte{0xFF};
        ASSERT_TRUE(data_file.write_at(kPageSize + 100, &b, 1));
        ASSERT_TRUE(data_file.sync());
    }

    Pager pager;
    const Status open_status = pager.open(db.path());
    if (open_status) {
        BTree tree(pager);
        const Status s = tree.verify_integrity();
        EXPECT_FALSE(s) << "corruption was served as if it were data";
        EXPECT_EQ(s.code(), Code::Corruption);
    } else {
        EXPECT_EQ(open_status.code(), Code::Corruption);
    }
}

} // namespace
