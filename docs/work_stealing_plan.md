# Plan: parallel search by work stealing

Status: **phases 0–5 implemented (23/10/26)**; see [`work_stealing.md`](work_stealing.md) for the implementation as built and its verification. As implemented, each region has a single range at its base depth (stepping the base down when that level has nothing to give), rather than the per-depth limits described in §2.2–2.3 below. Phase 6 (checkpointing) is not implemented yet. Original status: plan, written 22/10/26. It replaces the producer/consumer design of [`parallelise_producer.md`](parallelise_producer.md) for parallelism. Checkpointing (phase 6) becomes simpler as a result. The plan is meant to be implemented by hand, step by step, with each phase checked before the next starts.

## 1. Why change the design

Measured on 22/10/26 at OMEGA = 32, S = 29 (`docs/optimisation_notes.md`, "cluster test"):
- **Rangpur, 1 producer + 15 consumers** (Xeon E5-2680 v3): throughput was flat at ~640k supports/s. Consumers spent **28.5%** of their time waiting on an empty queue, rising to 42%, so only about 11 of the 15 were useful. The producer's single core is the limit.
- **M2, 1 producer + 7 consumers:** consumers wait only 0.2–1.8% of the time, because the M2's single core is much faster. That margin disappears on slower cores or with more threads.

Any design with **one** DFS thread has this ceiling. The options considered:

| Design | Verdict |
|---|---|
| Several producers feeding one shared queue (the old stages 2–3) | Keeps the queue and the producer/consumer ratio, which has to be tuned per machine; checkpointing still needs a completion buffer and watermark per region. |
| Static split of the top-level siblings | The tree is very unbalanced (small-prime prefixes have huge subtrees), so most threads would go idle early. |
| Locking the victim's DFS stack so thieves can read it | Puts a lock in the hot loop, or races on the `mpz` frames while the victim modifies them. |
| **Every thread is a worker: it runs the DFS on its own region, processes its own supports inline (as `--single` does), and asks another worker for half its remaining work when it runs out** | **Chosen.** No queue and no producer to starve the others; balance adapts to the real tree. A region's progress is simply its DFS position, which makes checkpointing easy. |

This is the standard approach for searching unbalanced trees: the *Unbalanced Tree Search* benchmark (Olivier et al., "UTS: An Unbalanced Tree Search Benchmark", LCPC 2006), with *receiver-initiated* stealing where the victim answers steal requests itself (Acar, Charguéraud and Rainey, "Scheduling parallel programs by work stealing with private deques", PPoPP 2013). Our "tasks" are the untouched siblings implicit in each worker's DFS stack, so we steal by splitting the stack rather than keeping an explicit deque.

## 2. The design

### 2.1 Regions
A **region** is a part of the search tree that one worker owns:
- a **base depth** b ≥ 1;
- the **prefix**: the primes chosen at depths 0..b−1, with the DFS frames 0..b (each frame's `product`, `delta_Q`, `delta_sop`, `cor4_bound` and `next_prime_idx`);
- a range of sibling indices **[lo, hi)** at depth b.

Its supports are every completion of the prefix whose depth-b prime has an index in [lo, hi). In lexicographic order these form one contiguous block. The whole search is the region with b = 1, lo = 1 and hi = `primes.size()`.

### 2.2 The DFS runs a region
`DfsStack` gains two things:
- **Per-depth upper limits** `limit_[d]`: siblings at depth d stop at `limit_[d]` as well as at the existing end conditions. A newly pushed level gets `limit_ = primes.size()` (no restriction).
- **A base depth:** `backtrack()` returns false ("region finished") when it would pop the base depth, instead of when it would pop depth 1.

Nothing else in the loop changes:
- **Prune with `backtrack`:** a prune still skips all later siblings, by monotonicity. That holds within a region, since its siblings are a suffix-closed piece of the same range.
- **First-child reuse:** it reads the parent frame's `next_prime_idx`, which is copied with the region, so it behaves as before.

### 2.3 Splitting: giving away half the untouched work
At any moment a worker at depth `t` has, at each shallower depth `d` (base ≤ d < t), **untouched siblings** `frames_[d].next_prime_idx + 1 .. limit_[d] − 1`. The current sibling at depth d is the one whose subtree is being explored.

To split, take the **shallowest** depth d with at least 2 untouched siblings; shallow levels hold the most work per sibling. Then:
- the thief's region is base depth d, frames 0..d (copied, with `frames_[d].next_prime_idx = mid`), the support prefix, and range `[mid, limit_[d])`;
- the victim sets `limit_[d] = mid` and carries on.

Here `mid` is the middle of the untouched range. Any `mid` is **correct**: the two ranges are disjoint and cover the old one. The choice of `mid` only affects balance. At shallow depths most high indices are pruned immediately, so splitting by index count can give the thief a range that dies at once. A later refinement (phase 4) chooses `mid` from an estimate of the *feasible* range.

### 2.4 Stealing by request (the victim does the split)
Each worker has:
- `std::atomic<int> request`: the id of a worker asking it for work, or −1;
- a **mailbox**: a `Region`, plus `std::atomic<int> mailbox_state` (EMPTY / FILLED / REFUSED).

- **The victim**, at the top of every DFS iteration, does a relaxed load of `request`. This costs about a nanosecond against hundreds per node. If the slot is set, it takes the request (`exchange(-1)`) and tries to split:
  - **if it can split:** it first increments `active_workers`, then writes the region into the thief's mailbox, then stores FILLED with release ordering;
  - **otherwise:** it stores REFUSED.
- **The thief** (a worker whose region is finished): decrements `active_workers` once, then loops:
  1. if `active_workers == 0`, exit: nobody holds work, so the search is complete;
  2. pick a victim (random, not itself) and CAS its `request` from −1 to its own id; if that fails, pick another;
  3. wait until its own `mailbox_state` is not EMPTY, while **answering its own `request` slot with REFUSED** (it has no work to give);
  4. on FILLED, load the region (acquire), reset the mailbox to EMPTY, and run it; on REFUSED, reset and go back to 1.

Answering requests while waiting is what prevents deadlock: two idle workers asking each other would otherwise wait forever. Only the victim ever modifies its own stack, so the DFS loop needs no lock.

**Termination:** `active_workers` counts workers that hold a region. It starts at 1, with worker 0 holding the whole search. A victim increments it *before* handing over a region, so it can only reach 0 when no region exists anywhere.

### 2.5 Processing inline
Each worker processes its own supports as they are emitted, as `generate_supports` does in `--single` mode. There is no queue. Each worker publishes its statistics every `BATCH_SIZE` supports (`maybe_publish_thread_local_stats`) and when it exits. All the per-thread state in the processing code is already `thread_local`, so no other change is needed there.

### 2.6 What must stay identical, and what may not
Compared with `--single` on the same build:
- **Must be identical:**
  - supports emitted and the sum and maximum of `d_max`;
  - every consumer-side counter: candidates, trial-division and Fermat counts, certified, failures, unresolved, largest g, both histograms, and the power-tree and Jacobi counts.
- **May grow slightly:** the DFS counters `dfs_nodes_visited`, `inherited_bound_prunes` and `cor4_evaluations`. A thief evaluates its first sibling `mid` even when the single-threaded DFS would have skipped it because an earlier sibling already pruned. That's at most one extra node, and one extra check, **per successful steal**. Check that the excess is at most the steal count.
- **New check, `supports_hash`:** the sum over emitted supports of a 64-bit hash of the support, which doesn't depend on order. Equal hashes, together with equal counts, rule out a support being emitted twice while another is lost.

## 3. Implementation phases

Work on a branch. Each phase ends with its checks, before the next phase starts.

**Reference outputs** come from `--single` on the current build:
- OMEGA = 33, S = 30: 1,343,862 supports, 114,064 certified, largest 454;
- OMEGA = 33, S = 29: 43,601,360 supports, 3,605,989 certified, largest 502.

Save their complete summaries to diff against.

### Phase 0: preparation (small)
1. Create a branch, e.g. `work_stealing`.
2. Add the `supports_hash` counter to `Counters` (`merge` adds the values with wrap-around; `print_producer` prints it), and update it in `dfs_iter_` for every emitted support. Use any fixed 64-bit mix of the support's primes, e.g. a splitmix64 of each prime folded together.
3. Record the reference summaries, now including the hash.

**Check:** nothing else changes. Reference results are identical to the current build.

### Phase 1: limits and base depth in `DfsStack` (no behaviour change)
1. Add `std::array<std::size_t, MAX_OMEGA + 1> limit_` and `std::size_t base_depth_`. The constructor sets `base_depth_ = 1` and `limit_[1] = primes.size()`; `push()` sets `limit_[depth_] = primes.size()` for the new level.
2. In `dfs_iter_`, treat `i >= limit_[depth]` like the existing end-of-siblings test.
3. `backtrack()` returns false when `depth_ == base_depth_`.

**Check:** OMEGA = 33 (S = 30 and 29) in `--single`, `--parallel --threads 8` and `--dry-run`. *Every* line is identical, the DFS counters included, because nothing is split yet.

### Phase 2: regions and splitting, single-threaded
This tests all the splitting logic with no threads involved.
1. Define `struct Region { std::size_t base_depth; std::array<StackFrame, MAX_OMEGA + 1> frames; std::vector<unsigned> support_prefix; std::size_t hi; }`. Only frames 0..base_depth are meaningful.
2. Add a `DfsStack` constructor from a `Region`: copy frames 0..b and set `base_depth_ = b`, `depth_ = b` and `limit_[b] = hi`. Also set the support array's prefix.
3. Add `std::optional<Region> DfsStack::split()`, implementing §2.3 (shallowest depth below the current one with ≥ 2 untouched siblings).
4. Turn `dfs_iter_` into a function that runs one region, `run_region(DfsStack&, emit, poll)`, where `poll` is called at the top of every iteration. In the old modes `poll` does nothing.
5. **Test mode** (for example a hidden `--split-test N`): a single thread keeps a work list of regions. Every N nodes, `poll` splits the current stack and pushes the new region onto the list. When the current region finishes, the next region is popped and run.

**Check, with N = 1000, 100 and 7:**
- supports, `d_max`, the hash and all consumer counters equal the reference;
- DFS counters exceed the reference by at most the number of splits;
- try both LIFO and FIFO work lists.

Splitting every few nodes exercises every depth, including splits right next to prunes.

### Phase 3: workers with stealing (the threaded part)
1. A `Worker` struct holding its id, a `DfsStack` (or `std::optional`), `request`, the mailbox and its state, and steal statistics.
2. A shared `std::atomic<int> active_workers` and the worker array.
3. The worker loop: run its region, with `poll` answering steal requests (§2.4); when the region finishes, become a thief (§2.4) until it receives a region or the run terminates.
4. A new mode `--workers N` (keep `--parallel` working for comparison) that starts N workers, with worker 0 holding the whole search, then joins them.
5. Statistics: `steal_requests`, `steals_succeeded`, `steals_refused`, `worker_idle_ns` (time spent as a thief), in `Counters`, `merge` and `print_producer`.

**Checks:**
- OMEGA = 33, S = 30 and S = 29 with `--workers 2, 4, 8, 16`, each **20 times**. Every run must satisfy the §2.6 invariants: identical consumer counters and hash, and DFS-counter excess ≤ `steals_succeeded`.
- A **ThreadSanitizer** build (`clang++ -fsanitize=thread -O1 -g`, no LTO), with 4 and 8 workers: no reported races.
- The `lpr_g` debug build, 8 workers: no assertion failures.
- A stress option that sends steal requests very often (e.g. idle workers retry at once and victims split down to depth OMEGA − 2): same invariants.
- SIGTERM during a run still prints partial statistics and exits 3.

### Phase 4: observability and tuning
1. Progress line: active workers, steals so far, and idle time instead of queue length and consumer waits. Replace the single "position" with, for example, worker 0's position or the number of regions.
2. **Better `mid`:** pick it from the *feasible* part of the untouched range. Siblings past the first index whose minimal completion certainly exceeds the inherited bound will be pruned. A `double` estimate (sum of logs of the prefix and the consecutive primes, with a safety margin) and a binary search find that index cheaply. Correctness doesn't depend on it.
3. **Victim choice:** random first. If steal counts are high, try "steal from the worker that has been running its region longest".

**Measure:**
- M2 with 8 workers against `--parallel --threads 8`;
- Rangpur `largecpu` with 16 and 24 workers.

Report throughput, idle share and steals per second. Expected: throughput close to linear in the number of workers, and an idle share well under 1% except at the very end.

### Phase 5: make it the default
1. Once phases 3–4 are verified on both machines, make `--parallel` use workers and retire the producer/consumer code (`JobQueue`, `generate_supports_parallel`, `start_consumer`), or keep it as `--parallel-legacy` for one release.
2. Update `CLAUDE.md`, `docs/current_method.md`, `docs/cluster_roadmap.md` and `parallelise_producer.md` (mark it superseded), and add a dated entry to `optimisation_notes.md`.

### Phase 6: checkpoint and resume (later; now much simpler)
Processing is inline, so all of a worker's supports before its current DFS position have been processed. A region's progress **is** its position, so no completion buffer or watermark is needed.
- **Checkpoint:** for each worker, its region (base depth, prefix indices, the full current index path, and `limit_` at each depth), plus the committed statistics. Workers pause at a safe point (for example, between supports), the checkpoint is written atomically (temporary file, then rename), and they continue.
- **Resume:** rebuild each region's frames by **replaying** its index path with the same arithmetic as the DFS. In debug builds, compare replayed frames against copied frames for regions produced by `split()`; that cross-checks the replay.
- The checkpoint file format, fingerprint and Slurm workflow are as in `cluster_roadmap.md`.

## 4. Pitfalls to watch for
- **Memory ordering:**
  - the mailbox region must be fully written before the FILLED store (release), and read after the acquire load;
  - `active_workers` must be incremented before the handover.
- **Never touch another worker's stack.** Only the victim's own thread reads or writes its frames; thieves only touch `request` and their own mailbox.
- **Copying `mpz_class` frames allocates.** That's fine: steals are rare. But don't split in `poll` unless a request is pending.
- **Two idle workers asking each other:** each must keep answering its own `request` while waiting, or they deadlock.
- **Splitting at the current depth:** don't. Only depths strictly shallower than the current depth have a well-defined current sibling.
- **`dfs_position`** is a single global "latest support" for the progress line. With several workers it mixes values from different workers; rework it in phase 4.
- **Batched Fermat** (`experimental/consumer_ilp`), if adopted later, fits naturally: each worker buffers `BATCH_SIZE` of its own supports and processes them as a batch.

## 5. How we'll work
You write the code. For each phase I can:
- explain any part of the design in more detail before you start;
- review your diff (correctness, thread safety, style);
- run the phase's checks (reference diffs, repeated threaded runs, ThreadSanitizer, the debug build) and report anything that differs;
- write test scaffolding if you want it (e.g. a `scripts/verify.sh` that runs a phase's checks and diffs against the reference outputs), keeping the core code yours.

A good starting point is phase 0 then phase 1: they're small, change no behaviour, and set up the comparisons that every later phase relies on.
