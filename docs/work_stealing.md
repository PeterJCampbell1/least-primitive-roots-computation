# Work-stealing parallelism: implementation and verification log

Status: implemented on branch `parallelise_33`, starting 23/10/26 (uncommitted).

This file describes the parallel mode as implemented and records each step and its checks. The design follows `region_based_parallelism.md` and `work_stealing_plan.md`, with the review points from 22–23/10/26 folded in:
- a single range per worker, at its base depth;
- the victim computes the split;
- the split point comes after the victim's current sibling;
- a request is never dropped while it may still be answered;
- termination is decided by the count of workers holding regions.

## 1. Why

On Rangpur (OMEGA = 32, S = 29, 1 producer + 15 consumers), the single producer capped throughput at about 640k supports/s, and consumers waited 40% of the time (`optimisation_notes.md`, 22/10/26). In the new mode, every thread runs the DFS on its own **region** of the tree and processes its own supports inline, as `--single` does. There is no producer, no queue and no consumer: a thread that runs out of work takes part of another thread's region.

## 2. Regions

The DFS walks the set-enumeration tree of increasing prime tuples starting with 2. A node at depth d is a prefix of d + 1 primes; its children append a larger prime. Stack frame d holds:
- the index of the prime chosen at depth d (`next_prime_idx`);
- the values for the prefix chosen *before* depth d: product, `delta_Q`, `delta_sop`, the inherited Corollary 4 bound and the prefix hash.

A **region** is:
- a base depth b ≥ 1 and the frames 0..b (the fixed prefix above b, and the first sibling to visit at b);
- the support prefix (primes at depths 0..b−1);
- a limit `hi`: siblings at depth b stop before prime index `hi`. Depths below b are unrestricted.

Its supports form one contiguous block in lexicographic order. The whole search is the region with b = 1, first sibling 1 and `hi = primes.size()`.

**The DFS runs a region.** `DfsStack` stores `base_depth_` and `base_limit_`:
- at depth b, an index `≥ base_limit_` ends the siblings, like the existing end-of-list test;
- `backtrack()` returns false ("region finished") when it would pop the base depth.

With b = 1 and `hi = primes.size()`, this is exactly the old traversal.

## 3. Splitting (done by the victim, on its own thread)

Let `cur = frames[b].next_prime_idx`, the sibling at the base depth that the victim is in, or about to visit. Only the **untouched** siblings `cur+1 .. hi−1` can be given away.
- **Feasible part.** For index j at depth b, the DFS's first checks are:
  1. the **inherited-bound prune:** the minimal completion's product `product(prefix) · p_j · p_(j+1) ··· p_(j+rem)` exceeds `frames[b].cor4_bound`, where rem = OMEGA − b − 1;
  2. from `DELTA_START_DEPTH` on, the **Corollary 4 prune:** the product exceeds the minimal completion's own Cor 4 bound.

  Indices with j + 1 + rem > `primes.size()` can't be completed at all. All three tests are monotone in j, so a binary search finds `feasible_end`, the first untouched index that fails one of them. Each probe calls `sibling_fate`, which repeats exactly the arithmetic of `run_region`, the exact `mpz` comparisons included.
- **If `feasible_end > cur + 1`, split:** `mid = (cur+1) + ⌊count/2⌋`, where count = `feasible_end − (cur+1)`. The thief gets the region (prefix, b, siblings `[mid, hi)`); the victim sets `hi = mid`. The ranges are disjoint and cover `[cur+1, hi)`, so no support is lost or duplicated.
- **Otherwise, step down.** Every untouched sibling at the base would be pruned at once, or there are none. The victim's remaining work is the subtree of `cur`, so it moves its base one level down (base `b+1`, `hi = primes.size()`) and tries again there. If the untouched range wasn't empty (`cur + 1 < min(hi, primes.size() − rem)`), the traversal would have visited sibling `cur + 1` and pruned it. The step-down adds that visit's counts as `run_region` would (`dfs_nodes_visited`, plus either `inherited_bound_prunes` or `cor4_evaluations`, according to `sibling_fate`). That sibling can't be a first child, since cur ≥ parent + 1, so its checks are never skipped.
- **Refuse** if stepping down reaches the current DFS depth without finding a feasible untouched sibling.

**Every counter matches a single-threaded run exactly.** `mid` lies among siblings that survive the first checks, so a single-threaded run visits `mid` too. The victim stops at `mid` without evaluating it, and the thief evaluates it once. Siblings at or after `feasible_end` are evaluated only by the region that contains them, exactly once, as in a single-threaded run. And step-downs count the one visit they skip. So `dfs_nodes_visited`, `cor4_evaluations`, `inherited_bound_prunes` and `cor4_reused` must equal the single-threaded values, as must supports emitted, the checksum, `d_max` and every consumer counter. The checker therefore requires **identical** output, apart from timing, mode and stealing lines.

(The first version searched with the inherited-bound test only. A thief could then start on a sibling that a single-threaded run skips, because an earlier sibling was pruned by Corollary 4, which cost one extra node and one extra Cor 4 evaluation per such split. See step 4.)

## 4. Stealing protocol

Each worker has:
- an atomic `request` slot: the id of a thief, or −1;
- a **mailbox** (a `Region` plus an atomic state: EMPTY, FILLED or REFUSED).

All workers share an atomic `active_workers`, the number holding (or being handed) a region.

**Victim** (at the top of each DFS iteration): a relaxed load of its `request`. If a request is waiting, it does `t = request.exchange(−1)`. If `t ≠ −1`, it now owes thief t an answer:
- split succeeded: `active_workers.fetch_add(1)`, write the region into t's mailbox, then store FILLED (release);
- otherwise: store REFUSED (release).

**Thief** (its region is finished; it has decremented `active_workers` once):
1. **Answer its own slot first:** refuse any request it has received, since it has no work.
2. **Exit** if `active_workers == 0`: no region exists anywhere.
3. **Ask:** pick a random other worker and CAS that worker's `request` from −1 to its own id. If that fails (the slot is taken), back off and try another.
4. **Wait for the answer,** still answering its own slot. If `active_workers` reaches 0 while waiting, it tries to **withdraw**: a CAS of the victim's slot from its id back to −1.
   - If that succeeds, nobody will answer, so it exits.
   - If it fails, the victim has already taken the request and will answer, so it keeps waiting.
5. **Act on the answer.** FILLED: load the region (acquire) and run it. REFUSED: back off (yield a few times, then sleep 1 µs doubling to 1 ms) and repeat.

**Termination:** a victim increments `active_workers` before handing over a region, and a worker decrements it when its region is finished. So the count reaches 0 only when no region exists or is in transit, and it can't rise again afterwards.

**Memory ordering:**
- the region is written before the release-store of FILLED and read after the acquire-load;
- a thief reads its mailbox only between its own requests, and posting a request (CAS, acq_rel) orders those reads before the next victim's writes.

## 5. Modes and options
- `--parallel [--threads N]`: work stealing, N ≥ 1 workers (1 behaves like `--single`).
- `--parallel-queue [--threads N]`: the previous 1 producer + N−1 consumers, kept for comparison.
- `--dry-run [--threads N]`: the DFS only; N > 1 uses work stealing. This is the most demanding test of the stealing, since there's no processing to hide it.
- `--split-test N [--split-test-order lifo|fifo]` (with `--single` or `--dry-run`): a single thread splits its own region every N DFS steps into a local work list, then runs the pieces. It tests splitting and stepping down with no threads involved.
- `--steal-stress` (with `--parallel` or a threaded `--dry-run`): idle workers never back off.

## 6. Implementation log
(Each step lists what changed and the checks that passed.)

### Step 1: the DFS runs regions (`src/generate_supports.hpp`)
- `dfs_iter_` became `run_region(stack, support, primes, emit, poll)`. The loop body is unchanged, apart from the base-range test (`within_region`) and the `poll` call at the top of each iteration. Its GMP scratch values are now `static thread_local`, so starting many regions doesn't allocate.
- `DfsStack` gained `base_depth_`/`base_limit_`, `load(region)`, `frame(d)` and `set_base`. `backtrack()` ends at the base depth.
- New: `Region`, `pruned_by_inherited_bound` (the DFS's own inherited-bound test, for one sibling), and `split_region` (§3).
- The single-threaded, dry-run and queue modes run the root region with a no-op poll.
- New counters in `stats.hpp`: `regions_split`, `region_step_downs`, `steal_requests`, `steal_refusals`, `steal_withdrawals`, `worker_idle_ns` (in `merge` and the producer summary).

**Check:** OMEGA=33, S=30 and S=29, `--single` and `--dry-run`, against a reference build of `ee72013`: **every line identical**, the DFS counters and checksum included (apart from the three new summary lines).

### Step 2: command line, scheduler and modes
- **`src/cli.hpp`:**
  - mode `--parallel-queue`;
  - `--threads` now also applies to `--dry-run`;
  - testing options `--split-test N`, `--split-test-order lifo|fifo`, `--steal-stress`;
  - validation: `--parallel` needs at least 1 thread, `--parallel-queue` at least 2; `--split-test` is single-threaded only; `--steal-stress` needs a threaded search.
- **`src/generate_supports.hpp`:** `generate_supports_split_test<Process>` (the single-threaded split test, with a work list capped at 100,000 regions), and the `InlineEmit`/`NoPoll` helpers shared by the modes.
- **`src/work_stealing.hpp` (new):** `lpr::stealing::Scheduler`, implementing the §4 protocol.
- **`src/main.cpp`:**
  - dispatch for the new modes;
  - the run header shows the thread source, split test and steal stress;
  - the progress line has `active_workers=`, `regions_split=` and `idle_s=`, and `queue=` only in queue mode.
- **`src/parameters.hpp`:** `STEAL_MAX_BACKOFF_US = 1000`.

**The checker** (`.bench/verify.sh REF NEW`, a scratch script) compares a run against the single-threaded reference:
- every result line, the consumer counters and the checksum must be identical;
- each DFS counter must be ≥ the reference and ≤ the reference plus the regions split off.

It correctly fails on a deliberately corrupted checksum.

**Smoke tests (S=30), all PASS** with checksum `0x1956b51a10b50e45`:

| Run | Regions split | DFS excess |
|---|---|---|
| `--parallel --threads 8` | 386 | 23 |
| `--single --split-test 1000` | 1,613 | 51 |
| `--dry-run --threads 4` | 207 | 8 |

### Step 3: sanitizers, error handling, signals and the empty search
- **ThreadSanitizer** (`-fsanitize=thread -O1 -g`, OMEGA=33, S=30): **0 reports**. Every result passed the checker:

  | Run | Regions split |
  |---|---|
  | `--dry-run --threads 4` | 206 |
  | `--dry-run --threads 8 --steal-stress` | 292 |
  | `--dry-run --threads 16 --steal-stress` | 551 |
  | `--parallel --threads 4` | 205 |
  | `--parallel --threads 8 --steal-stress` | 479 |
  | `--parallel-queue --threads 4` | 0 |

- **AddressSanitizer + UBSan** (`-fsanitize=address,undefined`): **0 reports**, and every result passed:

  | Run | Regions split |
  |---|---|
  | `--single --split-test 7` | 218,831 |
  | `--dry-run --split-test 1 --split-test-order fifo` | 473,087 |
  | `--parallel --threads 8` | 438 |
  | `--dry-run --threads 16 --steal-stress` | 489 |

- **Command-line errors:** 12 invalid combinations each exit 2 with a specific message, e.g. `--parallel --threads 0`, `--parallel-queue --threads 1`, `--single --threads 4`, `--split-test` with a threaded mode, `--split-test 0`, `--split-test-order` without `--split-test` or with a bad value, `--steal-stress` without a threaded search, and two modes at once.
- **OMEGA = 34 (no supports):** `--parallel --threads 8`, `--parallel --threads 1`, `--dry-run --threads 8` and `--single` all terminate with exit 0 and 0 supports.
- **SIGTERM** after 8 s of `--parallel --threads 8 --s 29`: exit 3, with the partial statistics and the notice printed. The progress line shows `active_workers=8`, `regions_split=129` and `idle_s`.
- **Scripts:** `lpr.slurm`'s mode detection now recognises `--parallel-queue` (so `-a "--parallel-queue"` no longer gives two modes), and the `submit.sh` help says `--parallel` is work stealing.

### Step 4: exact DFS counters
- **First campaign** (237 runs, previous split rule, invariant checker `verify.sh`): **237 passed, 0 failed.**
  - **A:** split tests at N = 1–10,000, LIFO and FIFO, S=30 dry runs and full runs, and S=29.
  - **B:** `--parallel` at 1, 2, 3, 4, 5, 8, 16, 32 and 64 threads, 10 runs each, plus 30 stress runs.
  - **C:** threaded dry runs at 2–64 threads, 10 each, plus 20 stress runs.
  - **D:** S=29 (8 runs).
  - **E:** `--parallel-queue`, exact match.
  - **H:** the `lpr_g` debug build (6 runs).

  The DFS excess was always within the bound. Only `DFS nodes visited` and `Corollary 4 evaluations` were higher, never `Pruned by inherited Corollary 4 bound`. That showed the excess came from siblings pruned by Corollary 4, which the inherited-only search ignored.
- **Change:** `pruned_by_inherited_bound` → `sibling_fate`, which applies both checks; the binary search uses it; and step-downs count the skipped sibling according to its fate, with a debug assert that it really is pruned.
- **Strict checker** (`.bench/verify_exact.sh`): requires identical output, apart from timing, mode and stealing lines.
- **First strict runs** (S=30), all **EXACT**, with DFS nodes visited 1,617,129 = `--single`:

  | Run | Regions split |
  |---|---|
  | `--parallel --threads 8` | 308 |
  | `--parallel --threads 2` | 41 |
  | `--dry-run --threads 16 --steal-stress` | 358 |
  | `--single --split-test 1000` | 1,601 |
  | `--dry-run --split-test 7` | 190,134 |
  | `--dry-run --split-test 1` | 817,150 |
  | debug build, `--dry-run --split-test 7` | 190,134 |

### Step 5: full strict campaign and sanitizers on the final split rule
- **Campaign** (`.bench/campaign.sh` with `verify_exact.sh`): **237 passed, 0 failed**, every run **identical** to `--single` (DFS counters included). Sections:
  - **A (18 runs):** split test at N = 1, 2, 7, 13, 100, 1,000 and 10,000 (LIFO and FIFO), on S=30 dry runs and full runs, plus S=29.
  - **B (120):** `--parallel` at 1, 2, 3, 4, 5, 8, 16, 32 and 64 threads (10 runs each), plus `--steal-stress` at 2, 8 and 32 (10 each).
  - **C (80):** threaded dry runs at 2–64 threads, plus stress at 8 and 64.
  - **D (8):** S=29 with `--parallel --threads 8`, 8-thread dry runs, and 16-thread stress dry runs.
  - **E (3):** `--parallel-queue` at 2 and 8 threads, and at S=29.
  - **H (6):** the `lpr_g` debug build (all assertions, including the incremental hash and the step-down prune assert).
- **ThreadSanitizer** (6 runs, up to 16 threads with stress) and **ASan+UBSan** (4 runs, including 462,347 splits): **0 reports**, all identical.

### Step 6: performance (M2: 4 performance + 4 efficiency cores; alternating runs)
**1. Refactor cost, single-threaded** (CPU time, reference `ee72013` against new): none measurable.

| Run | Reference | New |
|---|---|---|
| Dry run, S=30 | 1.09–1.12 s | 1.11–1.13 s |
| Dry run, S=29 | 34.23–34.24 s | 34.29–34.32 s (+0.2%) |
| `--single`, S=30 | 7.25–7.26 s | 7.27–7.34 s |

**2. 8 threads, `--parallel-queue` against `--parallel`** (wall time):

| | Queue | Work stealing |
|---|---|---|
| S=30 | 1.35–1.42 s | 1.38–1.42 s |
| S=29, three rounds | 44.3 / 45.9 / 50.4 s | **43.6 / 45.5 / 48.3 s** |
| Time without work, S=29 | consumers waiting 15–23 s (5–6.5%) | workers idle 1.6–1.8 s (0.5%) |

The rounds slowed progressively in both modes (heat and load); stealing was faster in every pair. On the M2 the gain is small, because its single producer nearly keeps up with 7 consumers. The decisive measurement is on slower or more numerous cores (Rangpur's queue mode: 40% waiting).

**3. Scaling of `--parallel`, S=29:**

| Threads | Wall time | Speed-up | CPU time | Idle | Splits |
|---|---|---|---|---|---|
| 1 | 233.1 s | 1.00× | 231.9 s | 0 | 0 |
| 2 | 119.3 s | 1.95× | 237.2 s | 0.002 s | 52 |
| 4 | 62.0 s | 3.76× | 246.1 s | 0.007 s | 175 |
| 8 | 43.4 s | 5.37× | 311.4 s | 1.29 s | 432 |

Up to 4 threads (the performance cores) efficiency is 94–98%. Beyond that, threads run on the efficiency cores, at roughly half speed, hence the CPU-time inflation.

**4. Threaded dry run (DFS only), S=29:**

| Threads | Wall time | Speed-up |
|---|---|---|
| 1 | 34.6 s | 1.00× |
| 2 | 18.1 s | 1.91× |
| 4 | 10.3 s | 3.38× |
| 8 | 7.7 s | 4.49× |

Idle time stayed ≤ 1.2 s even though there's no processing to hide the stealing.

### Step 7: tuning option and finer statistics
- **`--steal-max-prefix K`** (with `--parallel` or a threaded `--dry-run`): a region is split only while its fixed prefix has fewer than K primes, i.e. at base depths < K. The default is K = OMEGA (any depth); K = OMEGA − 3 never splits the last three levels; K = 1 disables splitting. Values outside 1..OMEGA exit 2. The run header shows the value in use.
- **New counters** (producer summary):
  - idle time split into **waiting for answers** and **backing off**;
  - **requests refused by idle workers** (avoidable with a published "has work" flag) and **by busy workers with nothing to give**;
  - **time spent in `split_region`**;
  - **regions split off by base depth** (a histogram).
- **Checks:** results identical at K = 30, 33 and 1 (S=30, 4 threads) and with the default at 8 threads.
- **First observations** (S=30, 4 threads, tiny runs):
  - splits happen at base depths 11–29, so K = 30 barely changes anything at S=30;
  - idle workers caused about 4× as many refusals as busy ones (58 against 10);
  - splitting cost under 2 ms in total.

## 7. Planned tuning experiments and refactoring

The aim: decide which features to implement, which choices become tunable parameters, and their defaults, from measurements on the M2 **and** a uniform-core cluster node (Rangpur `largecpu`, 24 cores). The M2's 4 + 4 heterogeneous cores make scaling past 4 threads hard to read.

### 7.1 Metrics (all printed by `lpr`)
| Metric | From | Good |
|---|---|---|
| Wall time, CPU time | `/usr/bin/time` | lower; CPU ≈ single-threaded CPU on uniform cores |
| Idle fraction | `Worker idle time / (threads × elapsed)` | ≪ 1%, concentrated at the end |
| Wait vs back-off | idle-time breakdown | waiting ≪ back-off means answers are fast |
| Avoidable requests | `Requests refused … by idle workers` | small; if large, add the published flag (7.3) |
| Steal cost | `Time spent splitting regions`, regions split | ≪ 0.1% of CPU time |
| Split depth profile | `Regions split off by base depth` | most work moves at shallow depths |
| Over time | progress-log `active_workers=`, `idle_s=` growth | `active_workers` = threads until the end |

Every experiment run is also checked with `verify_exact.sh` (identical output).

### 7.2 Experiment T1: maximum split prefix (**done on the M2**, results in step 9; Rangpur still to do)
- **Matrix:** OMEGA=33, S=29:
  - `--parallel --threads 8` at K ∈ {33, 32, 31, 30, 28, 25, 20}, 2 repetitions;
  - `--threads 4` and `--threads 16` at K ∈ {33, 30};
  - `--dry-run --threads 8` at all K, 3 repetitions.
- **Question:** is never splitting the last levels (your proposal, K = OMEGA − 3 = 30) neutral, better or worse? Deep splits hand over little work, while a cutoff risks idle workers near the end of the run.
- **Decision:** make K a parameter with the best default (expected: OMEGA − 3 or OMEGA, if they are indistinguishable).
- **Repeat on Rangpur** with 24 threads.

### 7.3 Experiment T2: published "has work" flag (refactor A, small)
- **Change:** each `Worker` gets `std::atomic<bool> has_region` (set when a region is loaded, cleared when it is finished). Thieves pick victims only among workers with the flag set, falling back to any worker if none is set. A victim's answer path is unchanged, so correctness is unaffected (the protocol already tolerates a stale flag).
- **Measure:** avoidable requests (should fall to about 0), idle time, wall time; at 8, 16, 24 and 64 threads, on the M2 and Rangpur.
- **Expected:** little change on the M2 (idle already ~0.5%), more at high thread counts.

### 7.4 Refactor B: publishing per-worker information and choosing victims
Each worker publishes, in its own cache line, cheap-to-maintain values that change only when it gets a region, splits, or steps down:
- `has_region`;
- `base_depth` (shallower usually means more work to give);
- an estimate of what it has to give: the feasible untouched siblings at its base, `feasible_end − (cur+1)` (computed during `split_region` anyway; updated after each split, step-down or region start; may be stale between them).

**Victim-selection strategies to compare** (an option, e.g. `--steal-victims random|active|shallowest|weighted|pq`):
1. **`random`:** the current strategy.
2. **`active`:** uniform among workers with `has_region` (= T2).
3. **`shallowest`:** scan all published slots (O(threads), cheap for ≤ 64 threads) and ask the worker with the shallowest base, breaking ties at random.
4. **`weighted`:** random, weighted by the estimate of untouched work.
5. **`pq`** (adaptable priority queue): a shared structure ordered by (base depth, estimated work), updated by victims when they split or step down and by workers when they get or finish a region. Thieves take the best entry and ask that worker. Implementation options:
   - a mutex-protected indexed binary heap with decrease-key; updates are rare (only on splits and step-downs);
   - a lock-free approximation (per-depth buckets of worker ids).

   Only worthwhile if the O(threads) scan of strategy 3 shows up in profiles, i.e. at hundreds of threads (multi-node or very large nodes).

**Measure:** idle fraction, avoidable requests, splits per second and wall time, at 8–64 threads, on the M2 and Rangpur.

### 7.5 Experiment T3: how much to hand over
Currently the thief gets ⌈count/2⌉ of the feasible untouched siblings, i.e. the upper half by index. Upper siblings (larger primes) have smaller subtrees, so the thief gets less than half the work.
- **Option `--steal-fraction F`:** the thief gets the upper fraction F of the feasible siblings, with F ∈ {0.5, 0.67, 0.8}.
- **Alternative:** split by an estimate of the subtree size. The log-domain Corollary 4 estimate would serve, since it affects only balance, never correctness.
- **Measure:** regions split (fewer is better), idle time, wall time.

### 7.6 Experiment T4: back-off and polling
- **Back-off:** `STEAL_MAX_BACKOFF_US` ∈ {100, 1000, 5000} and the yield count before sleeping (16), by idle time and CPU time spent idle, especially at 64 threads.
- **Polling:** every DFS iteration (now) against every k iterations. Polling costs one relaxed load and is not expected to matter; confirm with the dry run.

### 7.7 Parameters and features: decision table (to be filled from T1–T4)
| Item | Kind | Default now | Decide by |
|---|---|---|---|
| Max split prefix K | runtime option | OMEGA | T1 on the M2 (step 9): keep OMEGA (no limit); OMEGA − 3 is no faster and K ≤ 25 starves workers. Confirm on Rangpur |
| Published `has_region` flag | feature | absent | T2: keep if avoidable requests or idle drop measurably at ≥ 16 threads |
| Victim strategy | runtime option | random | 7.4 comparison |
| Steal fraction F | runtime option | 0.5 | T3 |
| Max back-off, yield count | compile-time | 1000 µs, 16 | T4 |
| Poll interval | compile-time | every iteration | T4 |
| `--parallel-queue` | mode | present | remove after the cluster test cycle confirms `--parallel` |

### Step 8: full OMEGA=32, S=29 run (`--parallel --threads 8`, M2, 23:30–01:38)
- **Exit 0. Every count is identical to the reference `outputs/k32_s29_run.txt`:**
  - 6,057,734,933 supports;
  - 8,986,188,918 candidates;
  - trial-division and Fermat composites;
  - 639,796,795 certified;
  - 0 failed, 0 unresolved;
  - largest g(p) = 678;
  - `d_max` sum and maximum;
  - **DFS counters:** 6,494,306,726 nodes visited, 6,250,479,930 Cor 4 evaluations, 25,540,900 inherited prunes.
- **The full 121-line summary**, both histograms included, is identical to the 22/10/26 `--parallel-queue` run.
- **Supports checksum (reference for OMEGA=32, S=29): `0xc66b5efd1a7ac845`.**
- **Elapsed 7,664.8 s,** against 7,832.6 s for the queue mode on the same machine (−2.1%).
- **Stealing:** 572 regions split (290 step-downs), 779 requests (207 refused, 0 withdrawn). Workers idle **2.7 s in total**, 0.004% of 8 × 7,665 thread-seconds, against 1,909 s of consumer waiting (3.5%) in the queue mode.

### Step 9: experiment T1, maximum split prefix K (M2, OMEGA=33, S=29, 24/10/26)
- **Scripts and logs:** `.bench/tune1.sh` → `.bench/tune1.log` (timings: 2 repetitions at 8 threads, plus 4 and 16 threads and 3 dry-run repetitions), and `.bench/tune1b.sh` → `.bench/tune1b.log` (one repetition, rerun for the stealing statistics; the first pass's parser lost them). **All 52 runs are EXACT against `--single`.**
- **Sleep caveat:** the Mac slept repeatedly during `tune1b` (`pmset -g log`), so three of its wall times are invalid and left out below: K=33 (969 s), K=20 (1,155 s) and the K=25 dry run (945 s). Their counts are still valid. Long runs on the laptop need `caffeinate -i` or `pmset` settings.
- **Results, 8 threads.** Wall times are from `tune1` (2 repetitions); statistics are from `tune1b`:

| K | wall (s) | idle total (s) | requests | refused idle / busy | regions split | split time (ms) |
|---|---|---|---|---|---|---|
| 33 (no limit) | 44.6 / 44.0 | 1.3 | 575 | 157 / 17 | 401 | 4 |
| 32 | 49.3 / 48.5 (tune1b: 43.7) | 2.4 | 642 | 222 / 16 | 401 | 5 |
| 31 | 48.5 / 46.3 | 2.5 | 551 | 136 / 27 | 385 | 11 |
| 30 (OMEGA − 3) | 46.3 / 45.8 | 2.2 | 700 | 258 / 105 | 335 | 9 |
| 28 | 48.7 / 47.2 | 15.6 | 7,121 | 3,978 / 2,860 | 282 | 4 |
| 25 | 93.2 / 89.3 | 416 | 173,973 | 129,360 / 44,422 | 189 | 3 |
| 20 | 209.3 / 210.2 (rerun: 208.8) | 1,422 (rerun) | 586,709 | 482,045 / 104,602 | 60 | 4 |

- **Other thread counts:**
  - 4 threads: K=33 66.8 / 66.1 s, K=30 65.6 / 65.8 s;
  - 16 threads: K=33 44.0 s, K=30 45.1 s (8 performance + efficiency cores, so 16 threads add nothing on the M2).
- **Dry run, 8 threads** (3 repetitions):
  - 8.4–8.7 s for every K from 28 to 33;
  - K=25: 14.2–14.6 s;
  - K=20: 33.7–33.9 s.
  - At K=30: 320 splits, with 147 idle and 81 busy refusals. At K=33: 453 splits, with 71 idle and 15 busy refusals.
- **Where splits happen (K=33):** base depths 2–32, mostly 20–31. About 15% of splits (61 of 401) are at depths ≥ 30, the ones a K = OMEGA − 3 cutoff forbids.
- **Conclusions:**
  1. **Splitting is cheap at every depth:** under 12 ms in total per run. Forbidding deep splits saves nothing measurable.
  2. **A cutoff only hurts:**
     - At K = 30 it is neutral to slightly worse: +0–4% wall time, but idle doubles and busy refusals rise sixfold, because the tail of the run can no longer be shared.
     - K = 28 is the edge: idle is 12× larger.
     - From K ≤ 25 workers starve: wall time doubles, then reaches 5×, and requests grow 300–1000×.
  3. **Default: K = OMEGA (no limit).** Keep `--steal-max-prefix` as a tuning and diagnostic option, and repeat the two settings K = OMEGA and K = OMEGA − 3 on Rangpur with 24 threads.
  4. **Effect on T2 (published `has_region` flag):** at 8 threads, with no limit, only 174 requests are refused in 44 s and idle time totals 1.3 s, so the flag can save at most about 1.3 s of 352 thread-seconds (0.4%). It is worth testing only at ≥ 16 real cores (Rangpur), where refusals grow with the number of thieves.

### Step 10: final checks before the Rangpur cycle (24/10/26, `caffeinate -i`, `.bench/final.sh` → `.bench/final.log`)
- **Fresh build** (`make clean && make`): no warnings, and byte-identical to the binary T1 tested.
- **The T1 rows the Mac's sleep invalidated, rerun:**
  - K=33: 47.8 / 42.2 s, idle 1.0 s;
  - K=20: 208.8 s, idle 1,422 s, 682,827 requests;
  - dry run at K=25: 12.0 / 12.1 s.
  - These confirm step 9.
- **Regression sweep, all 23 runs EXACT against `--single`:**
  - **S=30, release:** `--single`; `--parallel` at 1, 2, 8 and 24 threads (oversubscribed); `--steal-stress`; K=30; `--parallel-queue`; split tests (100 LIFO and 37 FIFO); dry runs at 1, 8 and 16 threads (stress).
  - **Debug `lpr_g`:** `--parallel` at 8 threads, and a dry run at 8 threads with stress.
  - **S=29, 8 threads:** `--parallel` 42.0 s, `--parallel-queue` 43.2 s, dry run 7.1 s.
- **Exit codes, all correct:**
  - **Usage errors (exit 2):** `--steal-max-prefix` 0 and 34; `--single --threads`; two modes at once; `--parallel --split-test`; a bad `--split-test-order`.
  - **Exit 0:** OMEGA=34 with 4 threads (0 supports, checksum 0x0).
  - **Exit 3:** SIGTERM.

## 8. Observation: work stealing reverses the processing order (24/10/26)
The two OMEGA=32, S=29 runs on Rangpur process the same 6,057,734,933 supports (8,986,188,918 candidates, 1.483 per support overall) in very different orders. Candidates per support over time, from the progress logs:

| Supports done | Queue mode, cumulative (job 685755, 16 threads) | Queue mode, latest interval | Work stealing, cumulative (job 690984, 24 threads) | Work stealing, latest interval |
|---|---|---|---|---|
| first ~2% | 2.25 | 2.25 | 1.38 | 1.38 |
| ~30% | 1.67 | 1.57 | 1.38 | 1.38 |
| ~60% | 1.58 | 1.45 | 1.41 | 1.52 |
| ~85% | 1.52 | 1.25 | 1.46 | 1.63 |
| 100% | 1.483 | 1.14 | 1.483 | 1.63 |

- **Queue mode** emits supports in global lexicographic order. The first supports (consecutive small primes with a late deviation) have the most candidates, and later ones, with larger primes earlier in the support, have fewer. Candidates per support fall steadily (2.25 → 1.14 per interval), as intuition suggests.
- **Work stealing** splits off the *upper* half of the untouched siblings at the shallowest possible level. So the first steals hand the large-prime, late-lexicographic regions, which are cheap per support, to 23 of the 24 workers, while worker 0 continues from the start. Early on, most of the machine processes cheap supports; the expensive early-lexicographic part is shared out later, as those regions are split again. Candidates per support therefore *rise* over time (1.38 → 1.63 per interval).
- **Consequences:**
  - **Progress estimates:** under work stealing, an ETA from supports per second is optimistic. Job 690984's rate fell from 1.45M to 1.26M supports/s, and its projected 08:52 finish became 08:56. Candidates per second stayed flat at about 2.0M/s and are the better progress measure. A later improvement: an ETA in the progress line based on candidates, or on `experimental/estimate`'s estimate of the total.
  - **Nothing changes for correctness or for the totals,** which are identical between the modes.
  - **A checkpoint taken mid-run holds a mix of expensive and cheap regions,** not a lexicographic prefix. "x% of supports done" is therefore not "x% of the time".
  - **For the multi-node pool** (`multinode_plan.md`): tasks split off early are cheap per support, so "largest first" must mean largest by estimated *work* (candidates), not by number of supports or by index range.
