# Deliberately out of scope

Each of these is a reasonable idea. Each is also the kind of "just one more
thing" that leaves a repository unfinished.

**Adding to this file is encouraged. Acting on it before stage 6 ships is not.**

---

## Storage

- **Overflow pages.** A key plus value is currently capped at `max_payload()`,
  roughly 2 KB, because two cells must fit in an empty page or a split cannot
  make progress. Real engines spill the excess onto a chain of overflow pages.
  Until then, `insert` returns `InvalidArgument` and says so.
- **A page-cache eviction policy.** The cache currently grows to the size of the
  database, because nothing ever evicts. Fine at demo scale, wrong in principle.
  An LRU with a clean/dirty distinction is the obvious fix and needs pinning to
  be safe, since `Page` hands out raw pointers into cache buffers.
- **Freelist reuse.** `Pager::allocate` always grows the file. Pages emptied by
  deletes are never recycled, so a delete-heavy workload leaks space
  permanently. The meta page already carries a `freelist` field; nothing reads
  it.
- **B+tree node merging.** Pages are split but never merged. A tree that is
  filled and then mostly emptied keeps its height and its page count. Correct,
  but wasteful, and the reason `verify_integrity` tolerates empty leaves.
- **Prefix compression in internal pages.** Separators repeat long shared
  prefixes; compressing them raises fanout, which lowers height, which is the
  single biggest lever on lookup cost.
- **An LSM storage engine behind the same interface.** Decision 007 chose a
  B+tree for a read-dominated judge. Putting both behind one interface and
  benchmarking them against each other is a whole project, and a good one.

## Transactions

- **Serialisable isolation.** Stage 2 targets snapshot isolation, which permits
  write skew by design. Serialisable snapshot isolation is the real answer and
  is substantially harder.
- **Concurrent readers.** Everything is single-threaded today. Multi-version
  data makes readers-don't-block-writers achievable, but it needs a latch
  protocol over the page cache first.
- **Vacuum.** Dead tuple versions accumulate with nothing to reclaim them.

## SQL

- **Run the real sqllogictest corpus.** Decision 017 ships the harness and a
  corpus written here. The official SQLite corpus is several million statements
  and would be a genuinely external judge. The `NOT`-precedence bug — where the
  implementation and its own test agreed on the wrong answer — is the argument
  for it, and this is the one item on this list that arguably belongs above the
  line.
- **Streaming scans.** Decision 015 materialises every scan, so a query's
  working set is the size of the table. Fixing it needs either page pinning or
  a two-phase collect-then-modify at every write-while-scanning call site.
- **Aggregates, GROUP BY, joins, subqueries.** The four `#unsupported:` blocks
  in `testdata/select1.test` name them. Each is a real feature, and none is
  needed to demonstrate what stage 4 set out to demonstrate.
- **ORDER BY over a projected alias.** Sorting happens before projection, so
  `ORDER BY` can only name a table column today.
- ~~**Secondary indexes.**~~ **Promoted to stage 7** on 2026-09-19. Stage 6
  measured the cost of their absence at 29x slower than SQLite on a
  selective predicate, which is what moved it off this list. The rule held:
  it sat here unbuilt until stage 6 shipped. See `PLAN.md`.
- **A cost-based optimiser.** Stage 3's planner will be rule-based. Statistics,
  cardinality estimation and join ordering are a separate discipline.
- **Transactions spanning statements in the playground.** `BEGIN` / `COMMIT`
  typed by a user, with the visualiser showing two sessions interleaving, is a
  stage 5 stretch rather than a requirement.
- **Foreign keys, triggers, views, subqueries in every position.** The subset in
  stage 3 is deliberately small.

## Elsewhere

- **A network protocol.** The engine is a library and the playground runs it in
  the browser. A wire protocol implies a server, which implies hosting, which
  implies cost.
- **Replication.** Two copies of a database is a different project.
- **Compression.** Page-level compression would reduce I/O and complicate
  everything that assumes a page is 4096 bytes.
