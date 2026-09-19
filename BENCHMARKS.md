# Benchmarks

**These numbers describe one machine and nothing else.** Absolute figures from a
laptop with thermal limits and a background operating system are not
comparable with anything measured anywhere else, and a ratio between two
engines measured in the same process on the same data is the only part of this
document worth carrying away.

Run 2026-09-19, re-run after stage 7 added secondary indexes.

| | |
|---|---|
| CPU | AMD Ryzen 5 5600H, 6 cores / 12 threads, 3.3 GHz max |
| RAM | 7.3 GB |
| Disk | SK hynix PC711 NVMe SSD |
| OS | Windows 11 Home, build 26200 |
| Compiler | MSVC 19.43, `RelWithDebInfo` |
| Comparand | SQLite 3.53.4, from vcpkg |
| Harness | Google Benchmark, `--benchmark_min_time=0.5s` |

**SQLite is configured to match strata's durability, not its own defaults:**
`journal_mode = WAL` and `synchronous = FULL`. Strata fsyncs on every commit
unconditionally, so benchmarking it against a SQLite that does not wait for
the disk would be measuring the difference between waiting and not waiting.

---

## Raw storage engine

The B+tree and pager, with no SQL above them.

| Benchmark | Time | Throughput |
|---|---:|---:|
| B+tree insert, 1,000 keys, shuffled | 5.23 ms | 508 k/s |
| B+tree insert, 10,000 keys, shuffled | 19.5 ms | 640 k/s |
| B+tree point lookup, 1,000 keys | 791 ns | 1.26 M/s |
| B+tree point lookup, 100,000 keys | 1,630 ns | 636 k/s |
| B+tree full cursor scan, 10,000 keys | 678 µs | 15.3 M/s |

Lookup roughly doubles in cost from a thousand keys to a hundred thousand — a
hundredfold increase in data for a 2.1× increase in time, which is the
logarithmic shape a B+tree is supposed to have. The scan runs at 15 M
entries/second because it follows the leaf sibling chain and never revisits an
internal page.

## SQL layer, against SQLite

| Benchmark | strata | SQLite | Ratio |
|---|---:|---:|---:|
| Bulk insert, 1,000 rows, one transaction | 13.6 ms | 9.50 ms | **1.4× slower** |
| Commit latency, one row per transaction | 2,212 µs | 2,062 µs | **1.07× slower** |
| `SELECT … WHERE a = 500`, **no index** | 1,205 µs | 64.9 µs | **18.6× slower** |
| `SELECT … WHERE a = 500`, **indexed** | **15.0 µs** | 11.7 µs | **1.28× slower** |
| `SELECT a FROM t`, whole table | 1,344 µs | 99.0 µs | **13.6× slower** |

### Where it holds up

**Commit latency is within 7%.** Both engines are waiting on the same fsync,
and at ~2.2 ms per commit the disk dominates everything either of them does in
software. This is the number that says the write-ahead log is built correctly:
strata is paying for durability at the same rate SQLite is, not cutting a
corner that would show up as a suspiciously good result.

**Bulk insert is within 1.4×**, which for a first storage engine against
twenty-five years of SQLite is closer than it has any right to be. The gap is
mostly per-statement overhead, since every `INSERT` re-parses.

### Where it loses, and why

**Queries are 13–29× slower, and it is not the parser.**

That needed measuring rather than assuming, so there are two extra benchmarks
whose only job is to rule explanations out:

| | Time |
|---|---:|
| strata, parse `SELECT a FROM t WHERE a = 500` and stop | 3.77 µs |
| SQLite, same query, *re-parsed every call* via `sqlite3_exec` | 70.4 µs |
| SQLite, same query, prepared once and stepped | 72.4 µs |

Two things fall out. SQLite's own parse cost is negligible — prepared and
re-parsed are within 3% of each other — so comparing strata's parse-every-time
API against SQLite's prepared statements was never the unfair comparison it
looked like. And strata's parser accounts for **3.77 µs of a 2,086 µs query,
which is 0.2%**. Making the parser ten times faster would be invisible.

The cost is the scan, and specifically **decision 015**: `scan_prefix`
materialises every live row of the table into a vector before the first
operator sees anything, and resolving each key's newest visible version means
walking its MVCC chain. So a `WHERE` that matches one row still pays for a
thousand, twice over — once to materialise and once to filter. SQLite streams
from a cursor and stops when it can.

This is a design cost that was written down before it was measured. Decision
015 says the working set is the size of the table rather than the size of the
result, and predicts this is "the single biggest thing standing between this
engine and a real one". The benchmark put a number on it, and stage 7 then
tested the other half of the prediction.

## What an index does to that number

Stage 6 predicted that an index scan would bypass the materialising scan for a
selective predicate. Same query, same data, same machine — the only difference
is one `CREATE INDEX`:

| `SELECT a FROM t WHERE a = 500` | strata | SQLite |
|---|---:|---:|
| No index | 1,205 µs | 64.9 µs |
| Indexed | **15.0 µs** | 11.7 µs |
| | **80× faster** | 5.5× faster |

**80× on strata's own query**, and the gap against SQLite closes from 18.6× to
**1.28×**. An index scan seeks to one entry and fetches one row, so it never
enters the code path that materialises the table — which is exactly the shape
the prediction had.

SQLite speeds up only 5.5× on the same change because its unindexed scan was
already streaming. The size of strata's win is a measure of how bad its
unindexed path is, not of how good its index is.

**Indexes are not free on writes.** Every insert now maintains one:

| Bulk insert, 1,000 rows | |
|---|---:|
| No index | 13.6 ms |
| One index | 19.0 ms |
| | **1.40× slower** |

**And they do nothing for a query with no predicate.** `SELECT a FROM t` still
takes 1,344 µs against SQLite's 99.0 µs, because there is nothing to seek to.
Closing *that* needs streaming scans, which remains the first item under
storage in `FUTURE.md`. Indexes fixed the selective case and left the
full-scan case exactly where it was.

### A regression the benchmark caught

Adding indexes made *unindexed* bulk insert 40% slower — 14.3 ms before stage
7, 19.5 ms after — and three repeat runs confirmed it was not noise.

The cause was `scan_prefix` walking the transaction's entire write set on every
call to decide which of its own uncommitted writes fell inside the prefix.
That had always been O(write set), but nothing called it per-statement until
the insert path started asking "does this table have indexes?" on every row.
A thousand inserts in one transaction turned into half a million string
comparisons.

The write set is a `std::map`, so the fix is `lower_bound(prefix)` and a break
at the end of the range. Unindexed insert returned to 13.6 ms and the indexed
path dropped from 35 ms to 19 ms. It is worth recording because the regression
was invisible to all 154 tests — every one of them still passed, and only a
benchmark noticed.

---

## What is not measured

- **Concurrency.** Strata is single-threaded by design. There is no
  multi-threaded benchmark because there is nothing to measure.
- **Large datasets.** Everything here fits in the page cache, which never
  evicts (see `FUTURE.md`). Numbers at a size that exceeds memory would be
  different and worse, and are not claimed.
- **Recovery time.** The crash harness proves recovery is *correct*; how long
  it takes to replay a large log is unmeasured.
- **The WebAssembly build.** Compiled and working, but not benchmarked —
  browser timing has its own pitfalls and a bad methodology would be worse
  than no number.

## Reproducing

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config RelWithDebInfo
./build/RelWithDebInfo/strata_bench --benchmark_min_time=0.5s
```

Your absolute numbers will differ. The ratios are the part that should not.
