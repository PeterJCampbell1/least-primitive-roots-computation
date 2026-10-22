# Design: parallel producer and checkpoint/restart (not implemented)

Status: **superseded for parallelism (22/10/26)** by [`work_stealing_plan.md`](work_stealing_plan.md): every thread runs the DFS on its own region and processes its own supports, which removes the producer bottleneck measured on Rangpur and the need for a completion buffer. The checkpoint fingerprint and file format below still apply. Original status: design notes for later (October 2026). At present there is one producer thread, with co-processing (`--parallel`), and no checkpointing: SIGTERM only prints the statistics so far. This file records the agreed design so it can be picked up later. The staged roadmap for getting onto a cluster, including this work, is in [`cluster_roadmap.md`](cluster_roadmap.md).

## Why

- **Producer throughput.** At $\omega = 33$ the DFS takes about 1.2 s of a 16 s single-threaded run. With enough consumers (roughly 14 or more at $\omega = 33$; this should be measured at $\omega = 32$ with `--dry-run`), the single producer becomes the bottleneck. The `consumer_waits` and queue-length fields in the progress log show when this happens: frequent waits with an empty queue mean the consumers are starved.
- **Wall-time limits.** $\omega = 32$ takes about 6 h on 8 threads. Cluster jobs have time limits and can be pre-empted, so a long run must be able to stop and resume without losing or double-counting work.

Both needs come down to the same thing: dividing the search into pieces whose completion can be tracked.

## Regions: pieces of the search tree

A **region** is a subtree described by a prefix $(q_1, \ldots, q_k)$ together with a half-open range $[lo, hi)$ of prime indices for the next position. Because the DFS is lexicographic, every region is a **contiguous interval of supports in lexicographic order**, bounded by two boundary supports. The whole search is one region: prefix $(2)$, all indices.

The pruned tree is heavily imbalanced, because early prefixes (small primes) have far larger subtrees than late ones. So regions cannot sensibly be chosen in advance. Static splitting at a fixed depth would give a few huge regions and many tiny ones.

## Splitting by work stealing

- Each producer works on its own region with its own `DfsStack`.
- When a producer finishes its region, it **steals** from a busy producer. It takes the upper half of the untouched siblings at the busy producer's *shallowest* level that still has at least two untouched siblings. The thief's new region is (that level's prefix, $[mid, hi)$), and the victim's range at that level shrinks to $[lo, mid)$.
- A stolen region consists of whole untouched subtrees, so the imbalance does not matter. If it turns out tiny, the thief finishes quickly and steals again. Regions are therefore split according to the actual remaining work, not a guessed size.
- This is standard work stealing for irregular trees; the Unbalanced Tree Search (UTS) benchmark is the reference case. A Chase–Lev deque is the usual lock-free structure, but a mutex per producer is likely enough, since steals are rare relative to the work.
- Stealing only happens when a producer runs dry, so it adds little contention.

**`DfsStack` extensions needed:**
- **start** from a given boundary support, rebuilding each frame's product, window values ($Q_\delta$, SOP) and stored bound $b$ by replaying the prefix with the same updates and checks;
- **stop** at an end boundary (an index limit at the region's top level);
- **split** off the upper half of its siblings at the shallowest level with spare siblings, under the producer's lock.

## Completion is decided by the consumers

A support only counts as done once a **consumer** (or a co-processing producer) has finished processing it. The producer having emitted it is not enough: consumers may lag far behind, and work they have not finished must be redone after a restart.

Per region:
- The producer numbers its batches $0, 1, 2, \ldots$ and records the first support of each.
- When a batch has been processed, its statistics are placed in the region's **completion buffer**, indexed by sequence number. This applies to consumer and co-processed batches alike.
- The region's **watermark** is the lowest sequence number not yet processed. Whenever the batch at the watermark arrives, the watermark advances. That batch's statistics are merged into the committed totals, and the process repeats for the next sequence number.
- Out-of-order batches wait in the buffer until the watermark reaches them. There are at most about `HWM` + threads of them.
- A region is **finished** when its DFS has ended and its watermark has passed its last batch. It is then removed, and its statistics are already committed.

Each region has its own watermark, so a slow region (for example, a huge early subtree) never holds back the saved progress of other regions later in lexicographic order.

## Checkpoint file

Written atomically (to a temporary file, then renamed) every log interval and on SIGTERM:
- a configuration fingerprint: `OMEGA`, `S`, `PRIME_LIMIT`, `PRIMITIVE_ROOT_SEARCH_LIMIT`, the sieve constant, the global bound $B$, and the git commit. `--resume` refuses to continue a run made with different settings;
- the committed statistics (all fields of `LPRStats`);
- one entry per **unfinished** region: its end boundary (prefix and $hi$) and its watermark support (the first support of its oldest unprocessed batch).

Its size is roughly the number of producers plus any stolen-but-unstarted regions, so it stays small. With today's single producer it is a single region with a single watermark, so the format can be adopted before the parallel producer exists.

## Resume

`--resume FILE` checks the fingerprint, loads the committed statistics, and runs the DFS for each listed region from its watermark to its end boundary.
- **Nothing is skipped:** everything from the watermark onward is regenerated.
- **Nothing is counted twice:** batches that had been processed beyond the watermark, but not yet committed, are redone, and their earlier statistics were never committed.
- The work lost is at most the batches in flight per region (a few seconds).
- At the end, the program checks that the finished regions, together with any pruned subtrees, cover the whole search, which also serves as an exhaustiveness check.

## Contention

- **Per batch:** one lock to pop from the queue (as now) and one lock on the region's completion buffer. A batch is about 3 ms of work at $\omega = 33$ (256 supports at about 12 µs each), so even 64 threads generate only about 20,000 lock operations per second.
- **Merging statistics** (`LPRStats` has two arrays of about 1,000 counters, so a merge takes a few µs) should happen **outside** the lock: take the ready records out under the lock, then merge them afterwards. If it still shows up in profiles, increase `BATCH_SIZE`.
- **Producers:** each producer pushes to the shared queue. If that becomes contended, use one queue per producer, with consumers stealing between queues.

## Suggested order of implementation

1. **Checkpointing with one producer:** batch sequence numbers, the completion buffer and watermark, the checkpoint file and `--resume`, plus `DfsStack` start/stop at boundaries. Verify by interrupting runs at random points, resuming, and checking that the final summary matches an uninterrupted run exactly.
2. **Several producers:** a static initial split, for example the first-level siblings divided among producers, with per-region watermarks.
3. **Work stealing** between producers, for balance.
4. **Optionally, consumer co-generation:** a consumer that finds the queue empty steals a region and produces from it, so the producer/consumer balance adjusts itself.

Further ideas from the notes: pin threads to cores; on multi-socket (NUMA) nodes, keep each producer's queue local to its socket.
