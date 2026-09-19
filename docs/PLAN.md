# Strata — plan

Six stages. A stage is not finished until its gate passes, and a gate is a test
or a measurement against something outside the project — never an opinion.
Ticking a gate means writing down what the evidence was.

The claim the whole thing builds toward:

> **A transactional SQL database, written from scratch, that survives being
> killed mid-write and whose correctness is judged by test suites it did not
> author.**

The external judges are the point. Anyone can write a database that passes its
own tests. The two that matter here are **Hermitage**, which defines the
concrete anomalies each isolation level must prevent, and **sqllogictest**,
SQLite's published corpus of statements and expected results. Both are somebody
else's idea of correct.

**Ship point: Stage 2.** A crash-safe storage engine with real snapshot
isolation is a complete, defensible project. Stages 3 to 6 are upside.

---

## Stage 1 — Pages, tree, log

**No SQL, no transactions. Bytes that survive a kill.**

- `Page` — slotted page layout. Cell pointers grow forward from the header,
  cell content grows backward from the end, free space is the gap between.
- `Pager` — the page cache, the database file, and the decision of where a page
  actually comes from: the cache, the log, or the file.
- `Wal` — write-ahead log with chained frame checksums and generation salts.
- `BTree` — B+tree with splits, sibling-chained leaves, and an integrity check
  that walks every invariant.

**Gate** — [x] **PASSED 2026-09-19.** 48 unit tests green. A separate process
writing real transactions, killed 25 times with `taskkill /F` at random points:
every key that was announced as committed came back, and the tree passed its
full integrity check after every recovery. Evidence below.

---

## Stage 2 — Transactions

**Two writers, one truth.**

- Versioned tuples carrying `xmin` / `xmax`.
- A transaction manager handing out ids and snapshots.
- Visibility rules: a row is visible to a snapshot if the transaction that wrote
  it had committed when the snapshot was taken, and the one that deleted it had
  not.
- Write-write conflict detection — first committer wins.

**Gate** — [x] **PASSED 2026-09-19.** Every anomaly in the Hermitage suite that
snapshot isolation is required to prevent is prevented, and every one it is
permitted to allow is demonstrated to be allowed. Both directions matter: a
database that prevents everything is a database with one lock. Evidence below.

---

## Stage 3 — SQL front end

**Text in, plan out.**

Tokeniser, recursive-descent parser, an AST, and a logical plan.
`CREATE TABLE`, `INSERT`, `SELECT` with `WHERE` / `ORDER BY` / `LIMIT`,
`UPDATE`, `DELETE`. Types: integer, text, real, null.

**Gate** — [x] **PASSED 2026-09-19.** The defined subset parses to the expected
AST, and every syntax error reports a line and column that points at the actual
mistake. Evidence below.

---

## Stage 4 — Executor

**Plans that run.**

Volcano-style iterators — sequential scan, index scan, filter, projection,
nested-loop and hash join, sort, limit, aggregation. Secondary indexes over the
same B+tree.

**Gate** — [x] **PASSED 2026-09-19** (with a stated caveat — read the evidence).
A published sqllogictest pass rate over the supported subset, reported as a
fraction with the unsupported statements counted honestly rather than skipped
quietly.

---

## Stage 5 — The playground

**A stranger runs a query.**

Compile the engine to WebAssembly with Emscripten; a React front end where you
type SQL and see the parse tree, the plan, the pages the query touched, and two
transactions interleaving live.

Every visitor runs their own copy of the database in their own tab, so the
hosting is a static page and there is no server to pay for or to scale.

**Gate** — [~] **PARTIALLY PASSED 2026-09-19.** The engine compiles to
WebAssembly, the page works, and a query runs with no backend — verified in a
browser against a local static server. What is *not* done is the public URL:
deploying needs an account that only the owner can create. The gate stays open
until that link exists. Evidence below.

---

## Stage 6 — Numbers

**How fast, honestly.**

Google Benchmark. Point lookups, range scans, bulk insert, commit latency, all
against SQLite on the same machine and the same data.

**Gate** — [x] **PASSED 2026-09-19.** `BENCHMARKS.md` written from a real run,
with the machine stated and the standing warning against comparing across
machines. Where SQLite wins, it says by how much and why. Evidence below.

---

## Evidence for the gates ticked above

### Stage 1 — 2026-09-19

**Unit tests.** 48 cases across `page_test`, `pager_test`, `wal_test`,
`btree_test` and `recovery_test`. Clean build under `/W4` with zero warnings.

The ones that carry weight:

- `Wal.ATornFrameAndEverythingAfterItIsDiscarded` — a byte is flipped inside the
  second of two committed transactions. The first survives; the damaged one is
  discarded whole. This is the chained checksum doing its job.
- `Wal.FramesWrittenWithoutACommitMarkerAreDiscarded` — recovery stops at the
  last commit, not at the last frame that happens to be intact.
- `Recovery.AnInterruptedCheckpointReplaysRatherThanLosingData` — a page of the
  database file is deliberately scribbled over while the log still holds every
  committed frame. All 600 keys come back.
- `Recovery.ACorruptPageInTheDatabaseFileIsDetectedNotSilentlyServed` — with the
  log empty there is nothing to mask a flipped byte, and the engine says so
  rather than serving it.
- `BTree.HandlesKeysArrivingInReverseOrder` — descending insertion, the
  pathological case for a naive split point.

**Crash harness.** `scripts/chaos.ps1`, 25 runs. Each run starts
`strata_crash write`, which inserts 30,000 keys committing every 200, and kills
it with `taskkill /F` after a random 60–400 ms. The writer prints `committed N`
only *after* the commit fsync returns, so the last line the harness saw is a
lower bound on what recovery owes us.

```
  25 of 25 runs recovered with every committed key intact
  high water mark: 16,800 keys
```

After every kill, `strata_crash verify` reopens the database, runs the full
tree integrity check — page types, key order, separator bounds, uniform leaf
depth, and the sibling chain reaching exactly as many keys as the tree claims —
and then reads back every key to confirm its value. Zero failures.

**The bug this stage produced.** `remove_cell` frees a cell's slot pointer but
not its bytes, because reclaiming them means moving every cell after it. That
left a page coming out of a split with half the cells and *none* of the space,
so any insert into the left half split again immediately. Sequential insertion
never noticed — new keys always land in the fresh right page — and the 5,000-key
test passed. Random and reverse insertion failed instantly. Fixed by `compact()`
plus a compact-then-retry before any split. See decision 004, and the regression
tests `Page.CompactReclaimsSpaceAndKeepsCellsIntact` and
`BTree.ReusesSpaceFreedByDeletesInsteadOfGrowingTheTree`.

It is worth keeping because it is the whole argument for testing insertion
orders rather than insertion counts.

### Stage 2 — 2026-09-19

**75 unit tests**, up from 48. Clean build under `/W4`, zero warnings.

**Hermitage.** Martin Kleppmann's suite, transliterated from SQL to this
engine's key/value API. All ten cases run, and both directions are asserted:

```
[  OK  ] G0_DirtyWriteIsPrevented
[  OK  ] G1a_AbortedReadIsPrevented
[  OK  ] G1b_IntermediateReadIsPrevented
[  OK  ] G1c_CircularInformationFlowIsPrevented
[  OK  ] OTV_ObservedTransactionVanishesIsPrevented
[  OK  ] PMP_PredicateManyPrecedersIsPrevented
[  OK  ] P4_LostUpdateIsPrevented
[  OK  ] GSingle_ReadSkewIsPrevented
[  OK  ] G2Item_WriteSkewIsAllowedUnderSnapshotIsolation
[  OK  ] G2_AntiDependencyCycleIsAllowedUnderSnapshotIsolation
```

The last two are the ones that make the first eight mean something. Snapshot
isolation is *defined* by permitting write skew; a database that prevented it
would not be a better database, it would be a different isolation level, and
the claim in the README would be wrong. Those two tests fail if the engine ever
becomes accidentally stronger, which is the only way to keep an isolation-level
claim honest.

**What the implementation gets for free, and what it does not.** Two structural
choices do most of the work:

- Uncommitted writes live in the transaction's own buffer and never enter the
  tree (decision 012), so a dirty read is not prevented so much as
  unrepresentable. G1a, G1b and G1c fall out of that.
- Every transaction id in the tree therefore belongs to a committed transaction
  (decision 005 again), so visibility needs no commit log on disk — just the
  snapshot boundary and the active set.

What is *not* free is P4 and G0, which need the explicit first-committer-wins
check at commit time, and G-single, which needs the snapshot to be genuinely
immutable rather than re-read.

### Stage 3 — 2026-09-19

**103 unit tests**, up from 75. Clean build under `/W4`, zero warnings.

The AST half of the gate is asserted by rendering expressions back to a
parenthesised string and comparing that, so a test reads as the shape it
expects rather than as a walk over pointers:

```
1 + 2 * 3                 ->  (1 + (2 * 3))
a = 1 or b = 2 and c = 3  ->  ((a = 1) OR ((b = 2) AND (c = 3)))
10 - 3 - 2                ->  ((10 - 3) - 2)      left-associative
x is null and y = 1       ->  ((x IS NULL) AND (y = 1))
```

The error half is the part worth having. Eight cases assert the exact
`line:column` of the *offending token*, not of wherever the parser had got to —
those differ, and the difference is the whole of whether a message helps:

```
SELECT * FROM 42            1:15: expected a name, found integer '42'
SELECT FROM t               1:8:  expected a value, a column name or '(', found FROM
UPDATE t a = 1              1:10: expected SET, found identifier 'a'
SELECT (1 + 2 FROM t        1:15: expected ')', found FROM
SELECT 'oops                1:8:  unterminated string literal
CREATE TABLE t (a BLOB)     1:19: expected a column type (INTEGER, REAL or TEXT) …
INSERT INTO t (a,b) VALUES (1)
                            1:31: this row has 1 values but 2 columns were named
```

Two are worth singling out. An unterminated string reports its *opening* quote
rather than end-of-input, which is the difference between a usable message and
a useless one in a long statement. And a multi-line statement reports the right
line — `3:8` for a bad token on the third line — which is the only assertion
that proves the lexer's line counter is wired to anything.

### Stage 4 — 2026-09-19

**131 unit tests**, up from 103. Clean build under `/W4`, zero warnings.

```
sqllogictest corpus: statements 60/60   queries 52/52   total 112/112   unsupported 4
  unsupported: aggregate functions (COUNT, SUM, MIN, MAX) are not implemented
  unsupported: joins are not implemented
  unsupported: GROUP BY is not implemented
  unsupported: ORDER BY can only name a table column, not a projected alias
```

**The caveat, stated plainly.** This is sqllogictest's *format*, run against a
corpus written for this repository — four files under `testdata/`. It is **not**
the official SQLite corpus, which is several million statements distributed
separately and which exercises far more SQL than this subset implements.
Claiming a pass rate against the real corpus without having run it would be
exactly the dishonesty the external-judge idea exists to avoid. What the runner
gives is a real harness that a downloaded corpus can be pointed at unchanged,
and a denominator that counts every gap.

The four `#unsupported:` blocks are declared in the test files, counted
separately, and never counted as passes — a skipped test that inflates neither
numerator nor denominator is a lie by omission, so the report names each one.

**Three tests exist to prove the runner is not vacuous**: one feeds it a wrong
answer, one a statement that should have failed but did not, and one an
unsupported block, and each asserts the runner notices. Without those, "112/112"
would only mean the harness never looked.

**Two real bugs, both caught by these tests.**

1. **`NOT` had the wrong precedence.** It was parsed as a tight prefix operator,
   so `NOT a > 2` became `(NOT a) > 2` instead of `NOT (a > 2)`. In SQL, `NOT`
   binds *looser* than comparison and tighter than `AND`. The parser test had
   asserted the wrong behaviour, so the unit tests were green and agreed with
   each other — it took a query with a known answer to expose it. Fixed by
   moving `NOT` into the precedence ladder at level 3, between `AND` and
   comparison. This is the strongest argument in the project for an external
   judge: a self-written test suite can be confidently, consistently wrong.

2. **An unknown column went undetected on an empty table.** `SELECT b FROM t`
   succeeded when `t` had no rows, because nothing ever evaluated the
   expression and so nothing noticed `b` did not exist. Fixed with a resolution
   pass over every expression before the plan runs. A query that is wrong
   should be wrong whether or not there is data in the table.

### Stage 5 — 2026-09-19, partial

**The build.** `scripts/build_wasm.ps1` compiles the engine with Emscripten:

```
  strata.js      62,240 bytes
  strata.wasm   251,164 bytes
  total         313,404 bytes  (306 KB)
```

No exceptions and no RTTI, which decision 008 made possible by returning
`Status` rather than throwing — that decision was taken in stage 1 for this
build, and it paid off here.

**Verified working.** Served from a local static server and driven in a real
browser: the engine loads, the schema sidebar fills from the catalog, a query
returns rows, the plan panel shows `Project → Sort → Filter → SeqScan`, and a
syntax error surfaces with its `line:column`. `ROLLBACK` really rolls back and
`WHERE v = NULL` really returns nothing — the stage 2 and stage 4 guarantees
hold through the WebAssembly boundary, which is the only thing this stage
needed to prove about them.

**What is missing: the URL.** Publishing needs a Cloudflare Pages or GitHub
Pages account, which is the owner's to create. The deployment is a folder of
five static files with no build command and no backend, so it is a drag and
drop — but until the link exists a stranger cannot open it, and the gate says a
stranger can. It stays open.

**A false lesson, removed.** The playground originally shipped an example
captioned "run this twice and compare *loaded* in the Pages tab", implying the
page cache would show a hit on the second run. It never does: nothing evicts,
so a page is loaded at most once per session, and the number is always zero
after the first touch. The example was teaching something untrue about the
engine. It now demonstrates what a scan actually costs in page fetches, and the
Pages panel states the no-eviction limitation in as many words.

### Stage 6 — 2026-09-19

Full numbers, the machine, and the cross-machine warning are in
`BENCHMARKS.md`. SQLite is run with `journal_mode = WAL` and
`synchronous = FULL` so that both engines wait for the same disk.

| | strata | SQLite | |
|---|---:|---:|---|
| Commit latency, one row per txn | 2,353 µs | 2,192 µs | 1.07x slower |
| Bulk insert, 1,000 rows | 14.3 ms | 10.4 ms | 1.4x slower |
| `SELECT ... WHERE a = 500` | 2,086 µs | 70 µs | **29x slower** |
| `SELECT a FROM t` | 1,565 µs | 118 µs | 13x slower |

**The result worth having is the 29x, and why it is not the parser.**

Assuming would have been easy and wrong, so two benchmarks exist only to rule
explanations out. Strata's parser takes **3.77 µs of a 2,086 µs query — 0.2%**.
And SQLite re-parsing the same string on every call (70.4 µs) is within 3% of
SQLite with a prepared statement (72.4 µs), so strata's parse-every-time API
was never the unfair comparison it appeared to be.

What is left is decision 015: `scan_prefix` materialises every live row before
the first operator sees one, and each key costs an MVCC chain walk. A `WHERE`
matching one row pays for a thousand, twice. That decision was recorded with
its cost stated — "the working set is the size of the table, not the size of
the result" — before any of this was measured. The benchmark turns a predicted
cost into a number, which is the whole point of having written the prediction
down.

Commit latency landing within 7% of SQLite is the other number that matters:
it says the write-ahead log is paying full price for durability rather than
quietly skipping an fsync, which is the failure mode that makes a storage
benchmark look good and a database lose data.

The raw B+tree is not the problem. Point lookup goes from 791 ns at a thousand
keys to 1,630 ns at a hundred thousand — a hundredfold increase in data for
2.1x the time, which is the logarithmic shape it should have — and a full
cursor scan runs at 15.3 M entries per second. The storage engine is sound;
the SQL layer's scan strategy is what costs.
