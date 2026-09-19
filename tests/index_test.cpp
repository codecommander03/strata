#include "strata/sql/catalog.hpp"
#include "strata/sql/executor.hpp"
#include "strata/sql/keyenc.hpp"
#include "strata/sql/session.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;
using strata::test::TempDb;

namespace {

// ---------------------------------------------------------------------------
// Order-preserving encoding — the part that can be subtly wrong
// ---------------------------------------------------------------------------

/// Byte order must equal SQL order. If these ever disagree, equality lookups
/// keep working while range scans quietly return the wrong rows.
void expect_same_order(const Value& a, const Value& b) {
    const std::string ea = encode_index_value(a);
    const std::string eb = encode_index_value(b);
    const int sql = compare_values(a, b);
    const int bytes = ea.compare(eb);

    const auto sign = [](int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); };
    EXPECT_EQ(sign(sql), sign(bytes))
        << "SQL says " << sign(sql) << " but bytes say " << sign(bytes) << " for " << a.to_string()
        << " vs " << b.to_string();
}

TEST(IndexEncoding, NegativeIntegersSortBeforePositiveOnes) {
    // The classic bug: raw big-endian puts -1 (0xFFFF...) after 1.
    expect_same_order(Value(std::int64_t{-1}), Value(std::int64_t{1}));
    expect_same_order(Value(std::int64_t{-1000}), Value(std::int64_t{-1}));
    expect_same_order(Value(std::int64_t{-1}), Value(std::int64_t{0}));
}

TEST(IndexEncoding, OrdersNumbersAcrossTheWholeRange) {
    const std::vector<std::int64_t> values = {
        -9007199254740992LL, -1000000, -42, -1, 0, 1, 42, 1000000, 9007199254740992LL};
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        expect_same_order(Value(values[i]), Value(values[i + 1]));
    }
}

TEST(IndexEncoding, OrdersRealsIncludingNegativesAndFractions) {
    const std::vector<double> values = {-1e9, -1.5, -0.5, 0.0, 0.5, 1.5, 1e9};
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        expect_same_order(Value(values[i]), Value(values[i + 1]));
    }
}

TEST(IndexEncoding, IntegersAndRealsShareOneOrdering) {
    // compare_values says 2 and 2.0 are equal, so their encodings must match
    // byte for byte or an index lookup for one would miss the other.
    EXPECT_EQ(encode_index_value(Value(std::int64_t{2})), encode_index_value(Value(2.0)));
    expect_same_order(Value(std::int64_t{1}), Value(1.5));
    expect_same_order(Value(1.5), Value(std::int64_t{2}));
}

TEST(IndexEncoding, NullsSortFirstAndTextLast) {
    expect_same_order(Value::null(), Value(std::int64_t{-1000000}));
    expect_same_order(Value(std::int64_t{999999}), Value(std::string("a")));
    expect_same_order(Value::null(), Value(std::string("a")));
}

TEST(IndexEncoding, TextOrdersLexicographicallyAndAPrefixSortsFirst) {
    expect_same_order(Value(std::string("a")), Value(std::string("b")));
    expect_same_order(Value(std::string("a")), Value(std::string("ab")));
    expect_same_order(Value(std::string("")), Value(std::string("a")));

    // The terminator earns its keep here: a fixed-width row id follows the
    // value in the real key, so without it "a" would sort after "ab".
    const std::string a = encode_index_value(Value(std::string("a"))) + std::string(8, '\xFF');
    const std::string ab = encode_index_value(Value(std::string("ab"))) + std::string(8, '\x00');
    EXPECT_LT(a.compare(ab), 0);
}

TEST(IndexEncoding, TextContainingNulIsNotTruncated) {
    const Value with_nul(std::string("a\0b", 3));
    const Value plain(std::string("a"));
    EXPECT_NE(encode_index_value(with_nul), encode_index_value(plain));
    expect_same_order(plain, with_nul);
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

class IndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_ = std::make_unique<TempDb>("index");
        db_ = std::make_unique<Database>();
        ASSERT_TRUE(db_->open(temp_->path()));
        session_ = std::make_unique<Session>(*db_);
    }

    Status run(const std::string& sql) {
        ResultSet result;
        return session_->run(sql, &result);
    }

    ResultSet query(const std::string& sql) {
        ResultSet result;
        const Status s = session_->run(sql, &result);
        EXPECT_TRUE(s) << sql << "\n  -> " << s.to_string();
        return result;
    }

    /// First column of every row, as text, sorted — so a test can compare
    /// result sets without depending on scan order.
    std::vector<std::string> values(const std::string& sql) {
        const ResultSet result = query(sql);
        std::vector<std::string> out;
        for (const Row& row : result.rows) {
            out.push_back(row.empty() ? "<empty>" : row[0].to_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void check_indexes() {
        auto txn = db_->begin();
        const Status s = verify_indexes(*txn);
        txn->abort();
        EXPECT_TRUE(s) << s.to_string();
    }

    void seed(int count) {
        ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
        ResultSet ignored;
        ASSERT_TRUE(session_->run("BEGIN", &ignored));
        for (int i = 0; i < count; ++i) {
            ASSERT_TRUE(run("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row" +
                            std::to_string(i % 10) + "')"));
        }
        ASSERT_TRUE(session_->run("COMMIT", &ignored));
    }

    std::unique_ptr<TempDb> temp_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Session> session_;
};

TEST_F(IndexTest, CreateAndDrop) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));

    EXPECT_FALSE(run("CREATE INDEX idx_a ON t(a)")) << "a duplicate name must be rejected";
    EXPECT_TRUE(run("CREATE INDEX IF NOT EXISTS idx_a ON t(a)"));

    EXPECT_FALSE(run("CREATE INDEX idx_x ON t(nope)"));
    EXPECT_FALSE(run("CREATE INDEX idx_y ON nosuchtable(a)"));

    ASSERT_TRUE(run("DROP INDEX idx_a"));
    EXPECT_FALSE(run("DROP INDEX idx_a"));
    EXPECT_TRUE(run("DROP INDEX IF EXISTS idx_a"));
}

TEST_F(IndexTest, RejectsCompositeIndexesRatherThanSilentlyUsingOneColumn) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
    const Status s = run("CREATE INDEX idx ON t(a, b)");
    EXPECT_FALSE(s);
    EXPECT_NE(s.message().find("composite"), std::string::npos) << s.to_string();
}

TEST_F(IndexTest, BackfillsAnIndexBuiltOnAPopulatedTable) {
    seed(200);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();

    // The index must cover rows that existed before it did.
    EXPECT_EQ(values("SELECT a FROM t WHERE a = 7"), std::vector<std::string>{"7"});
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 199").rows.size(), 1u);
}

TEST_F(IndexTest, StaysCorrectAcrossInsertsUpdatesAndDeletes) {
    seed(50);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();

    ASSERT_TRUE(run("INSERT INTO t VALUES (500, 'new')"));
    check_indexes();
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 500").rows.size(), 1u);

    // An update that moves an indexed value must retract the old entry.
    ASSERT_TRUE(run("UPDATE t SET a = 600 WHERE a = 500"));
    check_indexes();
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 500").rows.size(), 0u)
        << "the old value still found the row";
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 600").rows.size(), 1u);

    ASSERT_TRUE(run("DELETE FROM t WHERE a = 600"));
    check_indexes();
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 600").rows.size(), 0u);
}

TEST_F(IndexTest, HandlesDuplicateValuesAndNulls) {
    ASSERT_TRUE(run("CREATE TABLE t(a INTEGER, b TEXT)"));
    ASSERT_TRUE(run("INSERT INTO t VALUES (1,'x'),(1,'y'),(1,'z'),(2,'q'),(NULL,'n')"));
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();

    EXPECT_EQ(query("SELECT b FROM t WHERE a = 1").rows.size(), 3u)
        << "duplicate values must all come back";
    // A null constant matches nothing under three-valued logic, index or not.
    EXPECT_EQ(query("SELECT b FROM t WHERE a = NULL").rows.size(), 0u);
    EXPECT_EQ(query("SELECT b FROM t WHERE a IS NULL").rows.size(), 1u);
}

TEST_F(IndexTest, AnIndexedQueryAnswersExactlyAsAnUnindexedOneDoes) {
    seed(300);

    // Record every answer before the index exists...
    const std::vector<std::string> probes = {
        "SELECT a FROM t WHERE a = 42",
        "SELECT a FROM t WHERE a = 0",
        "SELECT a FROM t WHERE a = 299",
        "SELECT a FROM t WHERE a = 999",
        "SELECT a FROM t WHERE a > 295",
        "SELECT a FROM t WHERE a >= 295",
        "SELECT a FROM t WHERE a < 4",
        "SELECT a FROM t WHERE a <= 4",
        "SELECT a FROM t WHERE a > 10 AND a < 15",
        "SELECT a FROM t WHERE a = 5 OR a = 6",
        "SELECT a FROM t WHERE 42 = a",
        "SELECT b FROM t WHERE a = 11",
    };
    std::vector<std::vector<std::string>> before;
    for (const std::string& sql : probes) {
        before.push_back(values(sql));
    }

    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();

    // ...and demand the same answers after. An index that changes an answer
    // is a bug; the only thing it may change is how long the answer took.
    for (std::size_t i = 0; i < probes.size(); ++i) {
        EXPECT_EQ(values(probes[i]), before[i]) << "index changed the answer to " << probes[i];
    }
}

TEST_F(IndexTest, ThePlanSaysWhichScanItChose) {
    seed(100);

    EXPECT_NE(query("SELECT a FROM t WHERE a = 5").plan.find("SeqScan"), std::string::npos);

    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));

    const std::string equality = query("SELECT a FROM t WHERE a = 5").plan;
    EXPECT_NE(equality.find("IndexScan idx_a"), std::string::npos) << equality;
    EXPECT_NE(equality.find("seek"), std::string::npos) << equality;

    const std::string range = query("SELECT a FROM t WHERE a > 95").plan;
    EXPECT_NE(range.find("IndexScan idx_a"), std::string::npos) << range;
    EXPECT_NE(range.find("range"), std::string::npos) << range;

    // An unindexed column, and a predicate no index can help with, both fall
    // back rather than pretending.
    EXPECT_NE(query("SELECT a FROM t WHERE b = 'row1'").plan.find("SeqScan"), std::string::npos);
    EXPECT_NE(query("SELECT a FROM t WHERE a <> 5").plan.find("SeqScan"), std::string::npos);
    EXPECT_NE(query("SELECT a FROM t").plan.find("SeqScan"), std::string::npos);
}

TEST_F(IndexTest, AnEqualitySeekFetchesOneRowNotTheWholeTable) {
    seed(500);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));

    // The whole point: the scan node reports how many rows it pulled off the
    // heap, and for a unique match that is one.
    const std::string plan = query("SELECT a FROM t WHERE a = 250").plan;
    EXPECT_NE(plan.find("1 of 1 candidates"), std::string::npos) << plan;
}

TEST_F(IndexTest, DroppingAnIndexLeavesTheTableAnsweringCorrectly) {
    seed(100);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    const std::vector<std::string> indexed = values("SELECT a FROM t WHERE a = 50");

    ASSERT_TRUE(run("DROP INDEX idx_a"));
    check_indexes();

    EXPECT_EQ(values("SELECT a FROM t WHERE a = 50"), indexed);
    EXPECT_NE(query("SELECT a FROM t WHERE a = 50").plan.find("SeqScan"), std::string::npos);
}

TEST_F(IndexTest, SurvivesCommitAndReopen) {
    seed(120);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    ASSERT_TRUE(db_->pager().commit());

    session_.reset();
    db_->close();
    ASSERT_TRUE(db_->open(temp_->path()));
    session_ = std::make_unique<Session>(*db_);

    check_indexes();
    EXPECT_EQ(query("SELECT a FROM t WHERE a = 99").rows.size(), 1u);
    EXPECT_NE(query("SELECT a FROM t WHERE a = 99").plan.find("IndexScan"), std::string::npos);
}

TEST_F(IndexTest, AnIndexCreatedInARolledBackTransactionDoesNotExist) {
    seed(20);
    ResultSet ignored;
    ASSERT_TRUE(session_->run("BEGIN", &ignored));
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    ASSERT_TRUE(session_->run("ROLLBACK", &ignored));

    check_indexes();
    EXPECT_NE(query("SELECT a FROM t WHERE a = 5").plan.find("SeqScan"), std::string::npos)
        << "a rolled-back index is still being planned against";
    // And the entries it wrote during backfill went with it.
    EXPECT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();
}

// ---------------------------------------------------------------------------
// The gate: a drifted index must be detected, not served
// ---------------------------------------------------------------------------

TEST_F(IndexTest, VerifyCatchesAMissingIndexEntry) {
    seed(30);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    check_indexes();

    // Reach past SQL and delete one entry by hand, the way a maintenance bug
    // would. Nothing at the query level would notice: the row is still there
    // and every other query still works.
    {
        auto txn = db_->begin();
        Catalog catalog(*txn);
        IndexDef def;
        ASSERT_TRUE(catalog.lookup_index("idx_a", &def));

        std::vector<std::pair<std::string, std::string>> entries;
        ASSERT_TRUE(txn->scan_prefix(index_range_start(def.id), &entries));
        ASSERT_FALSE(entries.empty());
        ASSERT_TRUE(txn->remove(as_bytes(entries.front().first)));
        ASSERT_TRUE(txn->commit());
    }

    auto txn = db_->begin();
    const Status s = verify_indexes(*txn);
    txn->abort();
    EXPECT_FALSE(s) << "a missing index entry was not detected";
    EXPECT_EQ(s.code(), Code::Corruption);
    EXPECT_NE(s.message().find("missing"), std::string::npos) << s.to_string();
}

TEST_F(IndexTest, VerifyCatchesAStaleIndexEntry) {
    seed(30);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));

    // An entry for a value no row holds — what an update leaves behind if it
    // forgets to retract the old one.
    {
        auto txn = db_->begin();
        Catalog catalog(*txn);
        IndexDef def;
        ASSERT_TRUE(catalog.lookup_index("idx_a", &def));
        const std::string bogus =
            index_entry_key(def.id, encode_index_value(Value(std::int64_t{99999})), 1);
        ASSERT_TRUE(txn->put(as_bytes(bogus), Bytes{}));
        ASSERT_TRUE(txn->commit());
    }

    auto txn = db_->begin();
    const Status s = verify_indexes(*txn);
    txn->abort();
    EXPECT_FALSE(s) << "a stale index entry was not detected";
    EXPECT_EQ(s.code(), Code::Corruption);
    EXPECT_NE(s.message().find("stale"), std::string::npos) << s.to_string();
}

TEST_F(IndexTest, VerifyPassesOnAHealthyDatabaseWithSeveralIndexes) {
    seed(100);
    ASSERT_TRUE(run("CREATE INDEX idx_a ON t(a)"));
    ASSERT_TRUE(run("CREATE INDEX idx_b ON t(b)"));
    ASSERT_TRUE(run("CREATE TABLE u(x TEXT)"));
    ASSERT_TRUE(run("INSERT INTO u VALUES ('one'),('two')"));
    ASSERT_TRUE(run("CREATE INDEX idx_x ON u(x)"));
    check_indexes();

    // And a text index actually answers.
    EXPECT_EQ(query("SELECT x FROM u WHERE x = 'two'").rows.size(), 1u);
    EXPECT_NE(query("SELECT x FROM u WHERE x = 'two'").plan.find("IndexScan idx_x"),
              std::string::npos);
    EXPECT_EQ(query("SELECT a FROM t WHERE b = 'row3'").rows.size(), 10u);
}

} // namespace
