# Decisions

Append-only. A decision that turns out to be wrong is superseded by a new entry
that says so and why; entries are never rewritten, because the reasoning at the
time is the thing worth keeping.

Each entry records what was decided, what else was considered, and what the
choice costs. The cost matters — a decision with no downside was not a decision.

---

## 001 — Build a database, and let other people's test suites judge it

**Decided 2026-09-19.**

Write a transactional SQL database from scratch: pages, B+tree, write-ahead log,
MVCC, parser, executor. Correctness is measured against **Hermitage** (isolation
anomalies) and **sqllogictest** (SQLite's published statement corpus).

**Alternatives considered.** A collaborative CRDT editor — strong, and still on
the shelf, but its correctness claim is one property test rather than a
published standard. A distributed key-value store — rejected in the waterline
session already, as complexity chosen for its own sake. A dozen product ideas
with frontends: rejected because every one of them either already existed in a
better form or had no checkable claim at its centre.

**Why this one.** It is the only idea on the list where the hard part is a
correctness property rather than a feature, and where somebody else has already
written down what correct means. A project that can be proved wrong and is not
is worth more than one that cannot be evaluated at all.

**Cost.** It is the largest scope considered, and this owner's failure mode is
starting rather than finishing. Mitigated by making stage 2 the ship point: a
crash-safe engine with snapshot isolation stands on its own, and stages 3 to 6
are explicitly upside rather than commitments.

---

## 002 — Write-ahead logging, not ARIES undo/redo

**Decided 2026-09-19.**

Every page modification is appended to a log before the database file is
touched; the database file is only updated at a checkpoint. Recovery is a
forward scan that stops at the last commit.

**Alternatives considered.** ARIES-style undo/redo with a dirty page table and
an analysis pass: the textbook answer, and what most real engines do. Rejected
because the undo pass exists to roll back changes that reached the data file
before their transaction committed, and this design never lets that happen.
Shadow paging: simpler still, but it destroys locality and makes every commit
rewrite the tree spine.

**Why.** Recovery becomes a forward scan with no undo pass and no analysis pass,
which is a few hundred lines instead of a few thousand, and is far easier to
argue is correct.

**Cost.** Readers must consult the log as well as the file, so every page fetch
carries a hash lookup. Checkpointing is a separate operation that has to be
scheduled, and a log that is never checkpointed grows without bound.

---

## 003 — Buffered stdio with an explicit fsync, not std::fstream

**Decided 2026-09-19.**

`File` wraps `std::FILE*` and exposes `sync()`, implemented as `fflush` followed
by `_commit` on Windows or `fsync` elsewhere.

**Alternatives considered.** `std::fstream`, which is the idiomatic choice and
was the first instinct. It has no portable way to say "these bytes are on
stable storage" — `flush()` only moves bytes from the stream's buffer into the
operating system's, which is exactly the buffer a power cut empties.

**Why.** Crash safety is the entire claim of stage 1. A commit path that cannot
guarantee durability is not a commit path.

**Cost.** A hand-rolled wrapper, one `#ifdef`, and no iostream conveniences.

---

## 004 — Deleting a cell orphans its bytes; space is reclaimed by compaction

**Decided 2026-09-19.**

`remove_cell` removes the slot pointer and leaves the cell's content in place.
`compact()` rebuilds the content area to reclaim it, and is called before any
split and after the truncating half of one.

**Alternatives considered.** Reclaiming immediately, by moving every cell after
the removed one — O(page) on every delete, and it invalidates every outstanding
pointer into the page. A per-page free list threading the gaps together, which
is what SQLite does: better space behaviour, considerably more state to keep
correct.

**Why.** Delete becomes a two-byte `memmove`. Compaction is paid only when a
page is actually about to split, which is rare relative to deletes.

**Cost.** A page can report free space it cannot use until compacted, and that
cost was paid immediately — see the bug recorded in `PLAN.md` under stage 1's
evidence. A page coming out of a split had half the cells and none of the space.
Sequential insertion never noticed; random insertion failed at once. The rule
that came out of it: **any code path that concludes a page is full must compact
and retry before it believes itself.**

---

## 005 — A transaction's frames are staged in memory and written at commit

**Decided 2026-09-19.**

`Wal::stage` buffers page images; `Wal::commit` writes them all in one burst
with the commit marker on the last frame, then syncs once.

**Alternatives considered.** Streaming frames to the log as they are produced
and marking the last one at commit. That is what a production engine does, and
it is why production engines need recovery to discard an uncommitted tail.

**Why.** Nothing uncommitted ever reaches the file. That makes recovery a plain
forward scan with no cleanup, and it makes `rollback()` free — drop the buffer.

**Cost.** A transaction's dirty set must fit in memory. A bulk load of a million
pages in one transaction would not. Accepted: the workloads this engine is meant
to demonstrate are interactive.

---

## 006 — Splits propagate upward by return value, not by re-descending

**Decided 2026-09-19.**

`insert_rec` is recursive and hands its caller an optional `Split{separator,
right}`. The caller places the separator, splitting itself if it must and
passing its own split further up.

**Alternatives considered.** Preemptive splitting on the way down, as in CLRS:
split any full child before descending into it, so insertion never cascades.
Rejected because "full" is not a fixed quantity with variable-length cells — the
test would have to assume a worst-case key, and would split pages that had ample
room. Recording the path on the way down and walking back up it: equivalent, but
needs the path kept valid across allocations that may rehash the page cache.

**Why.** Every page on the path is fetched once. A cascading split up a tall
tree is still a single pass, and the correctness argument is local: each level
either absorbs the separator or hands one up.

**Cost.** Recursion depth equals tree height. Fine for a B+tree — a million keys
is roughly three levels — but it is a stack, and a corrupt page claiming a cycle
would run off the end of it. The integrity check exists partly to catch that.

---

## 007 — B+tree, not an LSM tree

**Decided 2026-09-19.**

**Alternatives considered.** An LSM tree with memtable, SSTables and levelled
compaction. Genuinely more interesting to write, and better for write-heavy
workloads.

**Why.** sqllogictest is the external judge in stage 4, and it is SQLite's
corpus — a read-dominated, point-lookup-heavy workload that a B+tree is the
right shape for. Crash recovery is also far simpler with one mutable file plus a
log than with a compaction process that must be restartable.

**Cost.** Write amplification on random insertion, and no natural answer to
write-heavy workloads. An LSM is on the shelf in `FUTURE.md`.

---

## 008 — Errors are returned as Status, never thrown

**Decided 2026-09-19.**

**Alternatives considered.** Exceptions, which are the C++ default and read more
cleanly. `std::expected`, which is C++23 and would have been the choice if the
toolchain were newer.

**Why.** Stage 5 compiles this engine to WebAssembly, where exception support
costs binary size and startup time for no benefit. And a storage engine's
failures — key not found, page full, checksum mismatch — are expected control
flow rather than exceptional conditions.

**Cost.** Every call site carries an `if`. The code is noisier than it would be
with exceptions, and a forgotten check is silent.

---

## 009 — FNV-1a checksums, not CRC32 or a cryptographic hash

**Decided 2026-09-19.**

Pages carry a 32-bit FNV-1a checksum; log frames carry a chained 64-bit one.

**Alternatives considered.** CRC32C, which has hardware support and better
guarantees against burst errors. A cryptographic hash, which would also defend
against deliberate tampering.

**Why.** The threat is a torn write — a bit-level accident — not an adversary.
FNV-1a is twenty lines with no dependency and no intrinsic, which matters for
the WebAssembly build.

**Cost.** Weaker error detection than CRC32C, and no defence at all against
someone editing the file on purpose. If a benchmark in stage 6 shows checksums
on the hot path, CRC32C with an intrinsic is the upgrade.

---

## 010 — Log frames chain their checksums, and the log carries generation salts

**Decided 2026-09-19.**

Each frame's checksum folds in the previous frame's value, and every frame
repeats two random salts from the log header. A checkpoint rewrites the header
with fresh salts.

**Alternatives considered.** Per-frame independent checksums, which is simpler.

**Why.** After a checkpoint the log is reset, but the old bytes may still be on
disk. An independently-checksummed stale frame from a previous generation is
perfectly valid on its own terms and would be replayed as if it were current.
The salts make generations distinguishable; the chain makes validity a property
of a prefix rather than of an individual frame, so a torn frame invalidates
everything after it rather than leaving a hole.

**Cost.** Recovery is strictly sequential and cannot be parallelised, and a
single damaged frame discards every later transaction even if those are intact.
That is the correct trade for a log — a hole in the middle is worse than a short
tail.

---

## 011 — Version chains are append-only with tombstones, not mutable xmin/xmax tuples

**Decided 2026-09-19.**

All versions of a key live in the B+tree under a composite key of
`escape(user_key) || terminator || ~version`, so they sort adjacently and
newest-first. A version record is a flag byte plus the payload. A delete
appends a tombstone rather than stamping `xmax` on the version it supersedes.

**Alternatives considered.** Postgres's layout: one mutable tuple carrying both
`xmin` and `xmax`, where a delete rewrites the row it deletes. A separate
version store with the tree holding only chain heads, which is closer to what
MySQL does with its undo log.

**Why.** Writes stay append-only, which matters because the page beneath them is
a B+tree with a write-ahead log: a delete that had to find and rewrite an older
version would dirty a second page and log it too. Sorting newest-first means the
visibility scan finds its answer on the first cell it looks at in the common
case, using the forward-only cursor the tree already has.

**Cost.** Dead versions are never reclaimed — there is no vacuum, so a key
updated a million times keeps a million versions and its chain scan degrades
linearly. `xmin` is only in the key, so anything wanting it must decode the key
rather than read a field. And the composite key needs escaping, because
appending a fixed-width suffix to a variable-length key does not preserve
order: `"a" + version` would otherwise sort after `"ab" + version`. That
escaping is tested directly in `Encoding.AKeyThatIsAPrefixOfAnotherStillSortsBeforeIt`.

---

## 012 — Uncommitted writes live in the transaction, not in the tree

**Decided 2026-09-19.**

A transaction buffers its writes in a private map. Nothing reaches the B+tree
until commit, and commit applies the whole buffer then calls through to the
pager's single fsync.

**Alternatives considered.** Writing versions into the tree as they happen and
filtering them out on read by consulting a commit log — which is what a real
MVCC engine does, and is why a real MVCC engine needs a vacuum process and a
transaction status table that must itself be crash-safe.

**Why.** It makes three of Hermitage's eight required cases unrepresentable
rather than merely prevented: there is no code path by which one transaction
can observe another's uncommitted state, because that state is not in any
shared structure. It also makes `abort()` free, and it means every transaction
id found in the tree belongs to a transaction that committed — so visibility
needs no durable commit log at all.

**Cost.** A transaction's write set must fit in memory, compounding the same
limit decision 005 already accepted for the log. A long-running bulk update is
not expressible. Reads of own writes take a different path from reads of
committed data, which is a second code path to keep correct — covered by
`MvccTest.ReadsItsOwnWritesBeforeCommitting`.

---

## 013 — Snapshot isolation, and saying so rather than claiming serialisable

**Decided 2026-09-19.**

The engine targets snapshot isolation. Write skew and anti-dependency cycles are
permitted, and two tests assert that they actually occur.

**Alternatives considered.** Serialisable snapshot isolation, which tracks
read/write anti-dependencies and aborts transactions that would close a
dangerous cycle. It is the correct answer and it is a substantially larger piece
of work — it needs a read set per transaction and conflict tracking across
concurrent transactions, both of which this design currently has no place for.

**Why.** Claiming an isolation level the implementation does not provide is the
most common dishonesty in a database project, and it is the thing an interviewer
is most likely to catch. Snapshot isolation is a real, named, widely-deployed
level — it is what Oracle and Postgres's REPEATABLE READ actually give you — and
stating it precisely is worth more than overclaiming.

**Cost.** Write skew is a genuine correctness hazard for applications with
multi-row invariants, and this engine will not save them from it. The two tests
that assert the anomaly occurs exist to make that permanent: if the engine ever
becomes accidentally stronger, they fail, and the README has to be updated
rather than quietly drifting out of date.

---

## 014 — One B+tree for everything, namespaced by a first key byte

**Decided 2026-09-19.**

The catalog and every table's rows live in the same tree. A key's first byte
says what it is: `0x01` for a catalog entry, `0x02` for a row. Row keys are
`0x02 table_id(BE32) row_id(BE64)`, big-endian so byte order is numeric order
and a table scan is one contiguous range.

**Alternatives considered.** A tree per table, which is what SQLite does — each
table gets a root page recorded in the schema. Cleaner separation, and it makes
`DROP TABLE` free. Rejected because the pager has no freelist yet (see
`FUTURE.md`), so a per-table tree would leak its pages on every drop, and
because one tree means one MVCC mechanism rather than one per table.

**Why.** The catalog is then not special: it is read and written through an
ordinary transaction, under the same visibility rules as the data. `CREATE
TABLE` rolls back with everything else in its transaction, and a reader with an
old snapshot sees the schema that matches its data. That property came free and
would have needed deliberate work otherwise.

**Cost.** `DROP TABLE` must delete every row by hand — tested by
`DropTableRemovesItsRowsSoANewTableCannotInheritThem`, because a table id is
reused and the next table would otherwise inherit the corpse. There is also no
physical separation between tables, so one table's rows sit between another's
in the file.

---

## 015 — Scans materialise instead of streaming from a cursor

**Decided 2026-09-19.**

`Transaction::scan_prefix` collects rows into a vector; `SeqScan` reads from
that vector rather than holding a live B+tree cursor.

**Alternatives considered.** A streaming cursor, which is what the volcano
model is for and what a real engine does — constant memory regardless of table
size.

**Why.** A tree cursor is invalidated by any write, and `UPDATE` and `DELETE`
write *while they scan*. Streaming would need either a snapshot of the pages
under the cursor or a two-phase collect-then-modify at every call site. The
version chain also has to be walked to find each key's newest visible version,
which means the scan is already doing more than a straight cursor walk.

**Cost.** A query's working set is the size of the table, not the size of the
result. `LIMIT` still stops the *operators* above the scan early — which is
what the `Limit` test asserts — but the scan itself has already read
everything. This is the single biggest thing standing between this engine and a
real one, and it is the first entry in `FUTURE.md` under storage.

---

## 016 — SQL semantics follow SQLite where the standard leaves room

**Decided 2026-09-19.**

Three-valued logic, type affinity, null ordering, and division by zero all
behave as SQLite does: `1/0` is null rather than an error, a numeric string
stored in an INTEGER column is parsed, nulls sort before everything, and
arithmetic on text yields null.

**Alternatives considered.** Strict typing with errors on mismatch, which is
defensible and arguably better design. Postgres semantics, which differ on
several of these.

**Why.** sqllogictest is the external judge, and its expected output encodes
SQLite's answers. Choosing different semantics would mean either failing the
suite or maintaining a translation layer, and in both cases the judge stops
judging.

**Cost.** Some of these are bad semantics inherited on purpose. `1/0` returning
null hides a real bug in a query; silent affinity conversion hides a type
error. Both are recorded here so the choice reads as deliberate rather than
accidental.

---

## 017 — The sqllogictest corpus is written here, and the README says so

**Decided 2026-09-19.**

The runner implements sqllogictest's format. The corpus it runs is four files
in `testdata/`, written for this repository. The official SQLite corpus is not
vendored.

**Alternatives considered.** Downloading and vendoring the real corpus, which
is several million statements. It would be a far stronger claim — and it would
mostly measure how much SQL is unimplemented rather than how much of the
implemented subset is correct, because the official files lean on joins,
aggregates, subqueries and views that stage 4 does not have.

**Why.** The harness is the reusable part: it reads the real format, so a
downloaded corpus can be pointed at it unchanged. And an honest 112/112 over a
stated subset with four declared gaps is worth more than a misleading
percentage over a suite that was never run.

**Cost.** The judge is weaker than advertised in decision 001, which named
sqllogictest as an external standard. A corpus written alongside the
implementation can share its blind spots — and did: `NOT` had the wrong
precedence and the parser's own test asserted the wrong answer, so both agreed
with each other. It took a query with a known-correct result to break the tie.
That bug is the argument for eventually running the real thing; it is now the
first entry under SQL in `FUTURE.md`.

---

## 018 — The playground is a static folder, not an application

**Decided 2026-09-19.**

The engine compiles to WebAssembly and the front end is three hand-written
files: HTML, one stylesheet, one script. No framework, no bundler, no build
step on the host. The deployment is `web/` uploaded anywhere that serves static
files.

**Alternatives considered.** React with Vite, which is what `PLAN.md` originally
said and what most people would reach for. It would have meant a `node_modules`
tree, a build command in the host's settings, and a toolchain that rots — for a
page with one text area, one table and three tabs.

**Why.** The constraint that mattered was cost, and the way to make hosting
free and unscalable-by-construction is to have nothing to run. Every visitor
downloads 306 KB once and then runs their own database in their own tab, so
there is no server to pay for, no database to provision, and no traffic spike
that can cost money. It also means the demo cannot break because a backend is
down.

**Cost.** Hand-rolled DOM code instead of a component model. It is fine at this
size and would not be at five times it.

---

## 019 — A C ABI returning JSON, not embind

**Decided 2026-09-19.**

Four `extern "C"` functions — `strata_open`, `strata_exec`, `strata_parse`,
`strata_schema` — each returning a JSON string owned by a static buffer.

**Alternatives considered.** embind, which would expose C++ classes to
JavaScript directly and is far pleasanter to write against. It requires RTTI
and pulls in a sizeable runtime.

**Why.** Decision 008 returns `Status` rather than throwing, specifically so
this build could pass `-fno-exceptions -fno-rtti`. embind would have given
both back and undone the reason the error handling looks the way it does.
Four functions and a JSON string cost nothing and keep the binary at 251 KB.

**Cost.** Hand-written JSON serialisation, with hand-written escaping — a
category of bug that a library would not have. And the boundary is stringly
typed, so a field renamed in C++ fails silently in JavaScript.

---

## 020 — The playground shows the plan and the page counters, not just results

**Decided 2026-09-19.**

Every query returns its plan and a set of pager counters alongside its rows,
and the page devotes a panel to each.

**Why.** A page that only shows results is a form. The plan and the page counts
are the parts that make it an argument about how a database works, and they are
what an interviewer would actually ask about. Both were nearly free: the plan
is `Operator::describe`, which the executor needed for its own tests, and the
counters are two integers in the pager.

**Cost.** The counters are honest in a way that is slightly unflattering — see
the note in `PLAN.md` under stage 5. An early example invited the reader to
watch the page cache produce a hit on a second run, which it never does,
because nothing evicts. Instrumentation that is visible to users will expose
the engine's limitations whether or not that was the intention, and the fix was
to say so on the page rather than to pick a friendlier metric.

---

## 021 — An index narrows candidates; the predicate still decides

**Decided 2026-09-19.**

An `IndexScan` produces rows the index says *might* match. The original `WHERE`
clause is still evaluated on every one of them by the `Filter` above it. The
index is never the authority on whether a row qualifies.

**Alternatives considered.** Trusting the index and dropping the filter, which
is what a mature engine does and is strictly faster — one less evaluation per
row.

**Why.** It makes a whole category of bug impossible instead of merely unlikely.
The index key encodes numbers as doubles, so two integers above 2^53 collide;
a future encoding change could introduce other collisions. Under this rule a
collision costs a wasted row fetch. Under the alternative it returns a row that
does not match the query, and no test at the SQL level would obviously catch
it — the answer is simply, quietly wrong.

It also means the planner can be wrong without being unsafe. If
`find_index_plan` ever picks an index that does not actually satisfy the
predicate, the result is slow, not incorrect.

**Cost.** Every row produced through an index is evaluated twice — once
implicitly by the seek, once explicitly by the filter. Measured against the
alternative that cost is real but small next to the row fetch it accompanies,
and the benchmark still shows an 80x improvement over no index at all.

---

## 022 — One column per index, and composite syntax is rejected rather than truncated

**Decided 2026-09-19.**

`CREATE INDEX name ON table(column)` takes exactly one column.
`CREATE INDEX name ON table(a, b)` is a parse error.

**Alternatives considered.** Accepting the syntax and indexing only the first
column, which is less code and keeps more SQL parsing. Implementing composite
indexes properly, which needs a tuple encoding with per-column terminators and
a planner that understands prefix matching.

**Why.** Silently indexing one column of two is the worst of the three: the
statement appears to succeed, queries return correct answers, and the index
simply never helps the way the author expected. An error says what is true.

**Cost.** No composite indexes, so a query filtering on two columns uses an
index for one of them and filters the rest. The `#unsupported:` block in
`testdata/indexes.test` names the gap and it is counted in the corpus report.

---

## 023 — Index maintenance happens in the writing transaction, and drift is checkable

**Decided 2026-09-19.**

Every `INSERT`, `UPDATE` and `DELETE` writes its index entries through the same
`Transaction` that writes the row. `verify_indexes()` cross-checks every index
against its table in both directions.

**Alternatives considered.** Rebuilding indexes lazily or on a background pass,
which is cheaper on the write path.

**Why.** An index in a different transaction from its row can be half-applied
by a rollback, and an index that disagrees with its table returns confidently
wrong answers — the failure that no query-level test catches, because every row
involved still exists and every other query still works. Sharing the
transaction makes the two atomic for free, since the MVCC layer already
guarantees it.

The verification is the other half. `BTree::verify_integrity` proves the tree
is internally consistent; that says nothing about whether the *index* agrees
with it. `verify_indexes` walks both directions — every row has its entry, and
no entry points at anything absent — and two tests damage an index by hand to
prove the check is not vacuous.

**Cost.** Writes are **1.40x slower** with one index to maintain, measured, and
that multiplies with each additional index. Also, the delete path treats a
missing entry as corruption rather than ignoring it, so a pre-existing drift
surfaces as an error on the next delete instead of being silently repaired —
deliberately, because silently repairing it would hide the bug that caused it.

---

## 024 — scan_prefix seeks into the write set instead of walking it

**Decided 2026-09-19, superseding the implementation in decision 012.**

`Transaction::scan_prefix` merges the transaction's uncommitted writes over the
committed rows. It now uses `lower_bound(prefix)` on the ordered write set and
stops at the end of the range, rather than examining every key.

**Why this is recorded.** The original walked the whole map. That was O(write
set) per call and invisible until stage 7 made the insert path ask "does this
table have indexes?" once per statement — at which point a thousand inserts in
one transaction became half a million string comparisons, and unindexed insert
got 40% slower.

**What it cost to find.** Nothing in the test suite noticed. All 154 tests
passed before the fix and after it; correctness never changed. Three repeat
benchmark runs are what separated a real regression from noise.

The general lesson is recorded because it will recur: a data structure chosen
for correctness (an ordered map, so that merge order is deterministic) had an
access pattern nobody had needed yet. When a new caller changes the frequency
of an existing call, its complexity becomes a new question, and the tests will
not ask it.
