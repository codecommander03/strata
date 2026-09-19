// Strata against SQLite, on the same machine, the same data, and the same
// durability settings.
//
// SQLite is configured with `journal_mode = WAL` and `synchronous = FULL`,
// which is the closest match to what strata does unconditionally: a
// write-ahead log with an fsync on every commit. Running SQLite on its faster
// defaults and reporting the gap would be comparing a database that waits for
// the disk against one that does not.
//
// The numbers this produces are for one machine. See BENCHMARKS.md.

#include "strata/btree.hpp"
#include "strata/db.hpp"
#include "strata/pager.hpp"
#include "strata/sql/session.hpp"

#include <benchmark/benchmark.h>
#include <sqlite3.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace strata;
using namespace strata::sql;

namespace {

/// A database file in a unique temporary directory, deleted on destruction.
class Scratch {
public:
    explicit Scratch(const char* tag) {
        static int counter = 0;
        dir_ = std::filesystem::temp_directory_path() /
               ("strata_bench_" + std::string(tag) + "_" + std::to_string(++counter));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "bench.db").string();
    }

    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    const std::string& path() const { return path_; }

private:
    std::filesystem::path dir_;
    std::string path_;
};

std::string key_for(int i) {
    std::string s = std::to_string(i);
    return std::string(10 - s.size(), '0') + s;
}

/// Deterministic shuffle, so every run of every benchmark sees the same order.
std::vector<int> shuffled(int n, unsigned seed = 4242) {
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        order[static_cast<std::size_t>(i)] = i;
    }
    std::shuffle(order.begin(), order.end(), std::mt19937(seed));
    return order;
}

// --- SQLite helpers ---------------------------------------------------------

class Sqlite {
public:
    explicit Sqlite(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::abort();
        }
        // Match strata: a write-ahead log, and wait for the disk on commit.
        exec("PRAGMA journal_mode = WAL");
        exec("PRAGMA synchronous = FULL");
    }

    ~Sqlite() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    void exec(const std::string& sql) {
        char* message = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            sqlite3_free(message);
        }
    }

    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
};

// ---------------------------------------------------------------------------
// Raw storage: strata's B+tree with no SQL layer above it.
// ---------------------------------------------------------------------------

void BM_Strata_BTreeInsert(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        Scratch scratch("btree_insert");
        Pager pager;
        pager.open(scratch.path());
        BTree tree(pager);
        const std::vector<int> order = shuffled(count);
        state.ResumeTiming();

        for (const int i : order) {
            tree.insert(as_bytes(key_for(i)), as_bytes("value"));
        }
        pager.commit();
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Strata_BTreeInsert)->Arg(1000)->Arg(10000)->Unit(benchmark::kMillisecond);

void BM_Strata_BTreeLookup(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("btree_lookup");
    Pager pager;
    pager.open(scratch.path());
    BTree tree(pager);
    for (const int i : shuffled(count)) {
        tree.insert(as_bytes(key_for(i)), as_bytes("value"));
    }
    pager.commit();

    const std::vector<int> probes = shuffled(count, 99);
    std::size_t at = 0;
    for (auto _ : state) {
        std::string value;
        const int i = probes[at++ % probes.size()];
        benchmark::DoNotOptimize(tree.get(as_bytes(key_for(i)), &value));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Strata_BTreeLookup)->Arg(1000)->Arg(100000);

void BM_Strata_BTreeScan(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("btree_scan");
    Pager pager;
    pager.open(scratch.path());
    BTree tree(pager);
    for (int i = 0; i < count; ++i) {
        tree.insert(as_bytes(key_for(i)), as_bytes("value"));
    }
    pager.commit();

    for (auto _ : state) {
        auto cursor = tree.cursor();
        cursor.seek_first();
        int seen = 0;
        while (cursor.valid()) {
            benchmark::DoNotOptimize(cursor.key());
            ++seen;
            cursor.next();
        }
        benchmark::DoNotOptimize(seen);
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Strata_BTreeScan)->Arg(10000)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// SQL: strata against SQLite, same statements.
// ---------------------------------------------------------------------------

void BM_Strata_BulkInsert(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        Scratch scratch("sql_insert");
        Database db;
        db.open(scratch.path());
        Session session(db);
        ResultSet result;
        session.run("CREATE TABLE t(a INTEGER, b TEXT)", &result);
        state.ResumeTiming();

        session.run("BEGIN", &result);
        for (int i = 0; i < count; ++i) {
            session.run("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')", &result);
        }
        session.run("COMMIT", &result);
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Strata_BulkInsert)->Arg(1000)->Unit(benchmark::kMillisecond);

void BM_Sqlite_BulkInsert(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        Scratch scratch("sqlite_insert");
        Sqlite db(scratch.path());
        db.exec("CREATE TABLE t(a INTEGER, b TEXT)");
        state.ResumeTiming();

        db.exec("BEGIN");
        for (int i = 0; i < count; ++i) {
            db.exec("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')");
        }
        db.exec("COMMIT");
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Sqlite_BulkInsert)->Arg(1000)->Unit(benchmark::kMillisecond);

/// One row per transaction. This is the fsync benchmark: both engines are
/// waiting on the disk, so it measures the commit path and nothing else.
void BM_Strata_CommitLatency(benchmark::State& state) {
    Scratch scratch("sql_commit");
    Database db;
    db.open(scratch.path());
    Session session(db);
    ResultSet result;
    session.run("CREATE TABLE t(a INTEGER)", &result);

    int i = 0;
    for (auto _ : state) {
        session.run("INSERT INTO t VALUES (" + std::to_string(i++) + ")", &result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Strata_CommitLatency)->Unit(benchmark::kMicrosecond);

void BM_Sqlite_CommitLatency(benchmark::State& state) {
    Scratch scratch("sqlite_commit");
    Sqlite db(scratch.path());
    db.exec("CREATE TABLE t(a INTEGER)");

    int i = 0;
    for (auto _ : state) {
        db.exec("INSERT INTO t VALUES (" + std::to_string(i++) + ")");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Sqlite_CommitLatency)->Unit(benchmark::kMicrosecond);

void BM_Strata_SelectWhere(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("sql_select");
    Database db;
    db.open(scratch.path());
    Session session(db);
    ResultSet result;
    session.run("CREATE TABLE t(a INTEGER, b TEXT)", &result);
    session.run("BEGIN", &result);
    for (int i = 0; i < count; ++i) {
        session.run("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')", &result);
    }
    session.run("COMMIT", &result);

    for (auto _ : state) {
        ResultSet rows;
        session.run("SELECT a FROM t WHERE a = " + std::to_string(count / 2), &rows);
        benchmark::DoNotOptimize(rows.rows.size());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Strata_SelectWhere)->Arg(1000)->Unit(benchmark::kMicrosecond);

void BM_Sqlite_SelectWhere(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("sqlite_select");
    Sqlite db(scratch.path());
    db.exec("CREATE TABLE t(a INTEGER, b TEXT)");
    db.exec("BEGIN");
    for (int i = 0; i < count; ++i) {
        db.exec("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')");
    }
    db.exec("COMMIT");

    const std::string sql = "SELECT a FROM t WHERE a = " + std::to_string(count / 2);
    for (auto _ : state) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr);
        int rows = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ++rows;
        }
        sqlite3_finalize(stmt);
        benchmark::DoNotOptimize(rows);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Sqlite_SelectWhere)->Arg(1000)->Unit(benchmark::kMicrosecond);

/// SQLite parsing the same string every time, as strata has to.
///
/// The prepared-statement benchmark above is what SQLite is actually capable
/// of; this one is the fair comparison, because strata has no prepare API and
/// pays for the parse on every call. Reporting only the first would flatter
/// SQLite; reporting only this one would flatter strata. Both are here.
void BM_Sqlite_SelectWhereReparsed(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("sqlite_select_reparse");
    Sqlite db(scratch.path());
    db.exec("CREATE TABLE t(a INTEGER, b TEXT)");
    db.exec("BEGIN");
    for (int i = 0; i < count; ++i) {
        db.exec("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')");
    }
    db.exec("COMMIT");

    const std::string sql = "SELECT a FROM t WHERE a = " + std::to_string(count / 2);
    for (auto _ : state) {
        db.exec(sql); // sqlite3_exec prepares, steps and finalises each call
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Sqlite_SelectWhereReparsed)->Arg(1000)->Unit(benchmark::kMicrosecond);

/// Parsing alone, so the SQL-layer gap can be split between "parsing is slow"
/// and "the scan is slow" instead of being reported as one number.
void BM_Strata_ParseOnly(benchmark::State& state) {
    const std::string sql = "SELECT a FROM t WHERE a = 500";
    for (auto _ : state) {
        ParseError error;
        Parser parser(sql);
        Statement statement;
        benchmark::DoNotOptimize(parser.parse_one(&statement, &error));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Strata_ParseOnly)->Unit(benchmark::kMicrosecond);

void BM_Strata_FullScan(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("sql_scan");
    Database db;
    db.open(scratch.path());
    Session session(db);
    ResultSet result;
    session.run("CREATE TABLE t(a INTEGER, b TEXT)", &result);
    session.run("BEGIN", &result);
    for (int i = 0; i < count; ++i) {
        session.run("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')", &result);
    }
    session.run("COMMIT", &result);

    for (auto _ : state) {
        ResultSet rows;
        session.run("SELECT a FROM t", &rows);
        benchmark::DoNotOptimize(rows.rows.size());
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Strata_FullScan)->Arg(1000)->Unit(benchmark::kMicrosecond);

void BM_Sqlite_FullScan(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    Scratch scratch("sqlite_scan");
    Sqlite db(scratch.path());
    db.exec("CREATE TABLE t(a INTEGER, b TEXT)");
    db.exec("BEGIN");
    for (int i = 0; i < count; ++i) {
        db.exec("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row')");
    }
    db.exec("COMMIT");

    for (auto _ : state) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db.handle(), "SELECT a FROM t", -1, &stmt, nullptr);
        int rows = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ++rows;
        }
        sqlite3_finalize(stmt);
        benchmark::DoNotOptimize(rows);
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Sqlite_FullScan)->Arg(1000)->Unit(benchmark::kMicrosecond);

} // namespace

BENCHMARK_MAIN();
