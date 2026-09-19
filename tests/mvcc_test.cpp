#include "strata/db.hpp"
#include "strata/encoding.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace strata;
using strata::test::TempDb;

namespace {

class MvccTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_ = std::make_unique<TempDb>("mvcc");
        db_ = std::make_unique<Database>();
        ASSERT_TRUE(db_->open(temp_->path()));
    }

    std::unique_ptr<TempDb> temp_;
    std::unique_ptr<Database> db_;
};

// --- key encoding -----------------------------------------------------------

TEST(Encoding, RoundTripsKeysAndVersions) {
    for (const std::string key : {std::string("a"), std::string("hello"), std::string(""),
                                  std::string("with\0nul", 8), std::string(300, 'x')}) {
        for (const Xid version : {Xid{1}, Xid{2}, Xid{1000}, Xid{0xFFFFFFFFull}}) {
            const std::string encoded = encode_key(as_bytes(key), version);
            std::string out_key;
            Xid out_version = 0;
            ASSERT_TRUE(decode_key(as_bytes(encoded), &out_key, &out_version))
                << "failed to decode key of length " << key.size();
            EXPECT_EQ(out_key, key);
            EXPECT_EQ(out_version, version);
        }
    }
}

TEST(Encoding, NewerVersionsOfAKeySortFirst) {
    const std::string v1 = encode_key(as_bytes("k"), 1);
    const std::string v2 = encode_key(as_bytes("k"), 2);
    const std::string v9 = encode_key(as_bytes("k"), 9);
    EXPECT_LT(compare_keys(as_bytes(v9), as_bytes(v2)), 0);
    EXPECT_LT(compare_keys(as_bytes(v2), as_bytes(v1)), 0);
}

TEST(Encoding, AKeyThatIsAPrefixOfAnotherStillSortsBeforeIt) {
    // The bug the terminator exists to prevent: without it, "a" + version
    // compares its version bytes against 'b' and sorts after "ab".
    const std::string a = encode_key(as_bytes("a"), 1);
    const std::string ab = encode_key(as_bytes("ab"), 1);
    EXPECT_LT(compare_keys(as_bytes(a), as_bytes(ab)), 0);

    const std::string a_newest = encode_key(as_bytes("a"), 0xFFFFFFFFull);
    EXPECT_LT(compare_keys(as_bytes(a_newest), as_bytes(ab)), 0)
        << "every version of 'a' must sort before any version of 'ab'";
}

TEST(Encoding, KeysContainingNulBytesAreNotTruncated) {
    const std::string with_nul("a\0b", 3);
    const std::string plain("a");
    EXPECT_NE(encode_key(as_bytes(with_nul), 1), encode_key(as_bytes(plain), 1));
    EXPECT_LT(compare_keys(as_bytes(encode_key(as_bytes(plain), 1)),
                           as_bytes(encode_key(as_bytes(with_nul), 1))),
              0);
}

TEST(Encoding, PrefixMatchesTheStartOfEveryVersionOfThatKey) {
    const std::string prefix = key_prefix(as_bytes("key"));
    const std::string encoded = encode_key(as_bytes("key"), 7);
    EXPECT_EQ(encoded.compare(0, prefix.size(), prefix), 0);
}

// --- transactions -----------------------------------------------------------

TEST_F(MvccTest, ReadsItsOwnWritesBeforeCommitting) {
    auto txn = db_->begin();
    ASSERT_TRUE(txn->put(as_bytes("k"), as_bytes("v")));

    std::string value;
    ASSERT_TRUE(txn->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "v");

    // But nobody else can see it yet.
    auto other = db_->begin();
    EXPECT_FALSE(other->get(as_bytes("k"), &value));
}

TEST_F(MvccTest, AnAbortedTransactionLeavesNoTrace) {
    {
        auto txn = db_->begin();
        ASSERT_TRUE(txn->put(as_bytes("k"), as_bytes("v")));
        txn->abort();
    }
    std::string value;
    EXPECT_FALSE(db_->get(as_bytes("k"), &value));
    EXPECT_EQ(db_->active_transaction_count(), 0u);
}

TEST_F(MvccTest, ADroppedTransactionAbortsItself) {
    {
        auto txn = db_->begin();
        ASSERT_TRUE(txn->put(as_bytes("k"), as_bytes("v")));
        // No commit, no abort: the destructor must clean up.
    }
    std::string value;
    EXPECT_FALSE(db_->get(as_bytes("k"), &value));
    EXPECT_EQ(db_->active_transaction_count(), 0u);
}

TEST_F(MvccTest, RepeatedReadsInOneTransactionReturnTheSameValue) {
    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("first")));

    auto reader = db_->begin();
    std::string value;
    ASSERT_TRUE(reader->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "first");

    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("second")));

    ASSERT_TRUE(reader->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "first") << "the snapshot moved underneath the reader";
}

TEST_F(MvccTest, DeleteHidesTheKeyFromLaterSnapshotsOnly) {
    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("v")));

    auto reader = db_->begin(); // snapshot before the delete
    ASSERT_TRUE(db_->remove(as_bytes("k")));

    std::string value;
    EXPECT_TRUE(reader->get(as_bytes("k"), &value)) << "an old snapshot lost a row it could see";
    EXPECT_EQ(value, "v");
    EXPECT_FALSE(db_->get(as_bytes("k"), &value));
}

TEST_F(MvccTest, DeletingAnAbsentKeyReportsNotFound) {
    auto txn = db_->begin();
    const Status s = txn->remove(as_bytes("nothing"));
    EXPECT_FALSE(s);
    EXPECT_EQ(s.code(), Code::NotFound);
}

TEST_F(MvccTest, ReinsertingAfterADeleteWorks) {
    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("one")));
    ASSERT_TRUE(db_->remove(as_bytes("k")));
    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("two")));

    std::string value;
    ASSERT_TRUE(db_->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "two");
}

TEST_F(MvccTest, ScanReturnsOnlyTheNewestLiveVersionOfEachKey) {
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 10; ++i) {
            const std::string key = "k" + std::to_string(i);
            ASSERT_TRUE(db_->put(as_bytes(key), as_bytes("round" + std::to_string(round))));
        }
    }
    ASSERT_TRUE(db_->remove(as_bytes("k3")));

    std::vector<std::pair<std::string, std::string>> rows;
    ASSERT_TRUE(db_->scan(&rows));

    EXPECT_EQ(rows.size(), 9u) << "scan returned dead versions or the deleted key";
    for (const auto& [key, value] : rows) {
        EXPECT_NE(key, "k3");
        EXPECT_EQ(value, "round4");
    }
    // And in key order.
    for (std::size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LT(rows[i - 1].first, rows[i].first);
    }
}

TEST_F(MvccTest, DisjointWriteSetsDoNotConflict) {
    auto t1 = db_->begin();
    auto t2 = db_->begin();
    ASSERT_TRUE(t1->put(as_bytes("a"), as_bytes("1")));
    ASSERT_TRUE(t2->put(as_bytes("b"), as_bytes("2")));
    EXPECT_TRUE(t1->commit());
    EXPECT_TRUE(t2->commit());
}

TEST_F(MvccTest, ALoserCanRetryAndSucceed) {
    ASSERT_TRUE(db_->put(as_bytes("counter"), as_bytes("0")));

    auto t1 = db_->begin();
    auto t2 = db_->begin();
    std::string value;
    ASSERT_TRUE(t1->get(as_bytes("counter"), &value));
    ASSERT_TRUE(t2->get(as_bytes("counter"), &value));

    ASSERT_TRUE(t1->put(as_bytes("counter"), as_bytes("1")));
    ASSERT_TRUE(t1->commit());

    ASSERT_TRUE(t2->put(as_bytes("counter"), as_bytes("1")));
    EXPECT_EQ(t2->commit().code(), Code::Conflict);

    // Retrying on a fresh snapshot sees the winner's value and proceeds.
    auto t3 = db_->begin();
    ASSERT_TRUE(t3->get(as_bytes("counter"), &value));
    EXPECT_EQ(value, "1");
    ASSERT_TRUE(t3->put(as_bytes("counter"), as_bytes("2")));
    EXPECT_TRUE(t3->commit());
}

TEST_F(MvccTest, TransactionIdsSurviveAReopen) {
    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("before")));
    const Xid before = db_->pager().next_xid();
    ASSERT_GT(before, kFirstXid);

    db_->close();
    ASSERT_TRUE(db_->open(temp_->path()));

    // Reusing ids after a restart would make old versions visible to new
    // snapshots that must not see them.
    EXPECT_GE(db_->pager().next_xid(), before);

    ASSERT_TRUE(db_->put(as_bytes("k"), as_bytes("after")));
    std::string value;
    ASSERT_TRUE(db_->get(as_bytes("k"), &value));
    EXPECT_EQ(value, "after");
}

TEST_F(MvccTest, VersionsSurviveCommitAndReopen) {
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(db_->put(as_bytes("k" + std::to_string(i)), as_bytes("v")));
    }
    ASSERT_TRUE(db_->pager().commit());

    db_->close();
    ASSERT_TRUE(db_->open(temp_->path()));

    std::vector<std::pair<std::string, std::string>> rows;
    ASSERT_TRUE(db_->scan(&rows));
    EXPECT_EQ(rows.size(), 300u);
}

} // namespace
