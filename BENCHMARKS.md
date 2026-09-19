# Benchmarks

**These numbers describe one machine and nothing else.** Absolute figures from a
laptop with thermal limits and a background operating system are not
comparable with anything measured anywhere else, and a ratio between two
engines measured in the same process on the same data is the only part of this
document worth carrying away.

Run 2026-09-19.

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
| Bulk insert, 1,000 rows, one transaction | 14.3 ms | 10.4 ms | **1.4× slower** |
| Commit latency, one row per transaction | 2,353 µs | 2,192 µs | **1.07× slower** |
| `SELECT a FROM t WHERE a = 500`, 1,000 rows | 2,086 µs | 70 µs | **29× slower** |
| `SELECT a FROM t`, 1,000 rows | 1,565 µs | 118 µs | **13× slower** |

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
engine and a real one". The benchmark puts a number on it: **29×**.

The secondary cost is the absence of secondary indexes — `WHERE a = 500` is a
full scan in both engines here, but SQLite's full scan is 13× faster than
strata's, so indexing would not close the gap on its own. Streaming would.

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
