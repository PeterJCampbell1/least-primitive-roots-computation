# Plan: one search across several nodes (shared task pool), interleaved multiplication and single-node speed-ups

Status: **plan, written 24/10/26; not implemented.** Update 25/10/26: multi-node runs will first be attempted with MPI (`mpi_plan.md`); the shared task pool (§2–4) remains the fallback. It replaces the sketch in `cluster_roadmap.md` Stage 5. It builds on work stealing (`work_stealing.md`) and checkpoints (`checkpointing.md`).

## 1. Why, and why not MPI (yet)

A process lives on one node. Its threads share memory, and Slurm cannot stretch a process across nodes. Using several nodes for one search needs a layer that hands work between separate `lpr` processes.

| | Shared task pool on the cluster filesystem | MPI |
|---|---|---|
| **Dependencies** | None beyond a shared filesystem | An MPI library matching the GCC/GMP build on each cluster |
| **Scheduling** | Independent jobs or a job array: nodes start whenever they are free, and nodes can be added mid-run | One job that waits until all N nodes are free at once |
| **Failures** | A crash or time limit affects one node's tasks, which resume from their checkpoints | By default one failure ends the whole job |
| **Latency** | Milliseconds to seconds (NFS attribute caching) | Microseconds |
| **Balance at the end of the run** | Coarser, but adequate when splits can go down to the deepest level | Finest |
| **Complexity and testing** | Moderate; all state is files that can be inspected | Higher: hybrid MPI and threads, termination detection |

The work is compute-bound and long-lived: a task runs for minutes to hours, and splits are rare (about a thousand per hour on 24 threads). File latency of seconds therefore costs nothing measurable. **Plan: build the shared pool. Consider MPI only if the measured idle time at the end of the run is significant at hundreds of threads.**

## 2. Design

### 2.1 Directory layout (one per search, on shared storage)
```
pool/<search>/
  SEARCH           fingerprint (as in checkpoints) + creation time; every node checks it
  ready/           unclaimed tasks: one file each (a checkpoint with one task and zero stats)
  claimed/<node>/  tasks a node is working on, plus its checkpoint for them
  done/            finished tasks: a complete checkpoint per task (its stats)
  created/         one empty marker per task ever created (name = task id)
  nodes/<node>     heartbeat: time, state (busy/idle), tasks held; rewritten every ~30 s
  requests/<node>  an idle node asking for work (optional, see 2.3)
```
- **Task ids:** `<creating node>-<sequence>`, so no counter needs coordinating.
- **File format:** a task file uses the existing checkpoint format (`complete 0`, one `task` line, statistics zero), so parsing, fingerprint checks and replay are reused.

### 2.2 Claiming work
- A node claims a task by `rename(ready/X, claimed/<node>/X)`. Rename within a filesystem is atomic, so exactly one node succeeds. A failure to rename means another node got it; try the next file.
- The queue size is simply the number of files in `ready/`. That needs no shared counter and no file locking, which is unreliable on network filesystems.
- Inside the node, claimed tasks become pending tasks of the scheduler (`Scheduler` already takes a task list), so the node's threads share them through the existing work stealing.

### 2.3 Keeping the pool stocked (donation)
- **Busy nodes donate when the pool runs low:** every 10–30 s (`--pool-poll`), each busy node counts `ready/`. If it holds fewer than about one task per node (`--pool-low`), the node donates its largest splittable piece.
- **Donating reuses work stealing:**
  1. A pool thread in the node posts a steal request on the worker with the shallowest base depth, as an idle thread would.
  2. That worker splits its region (`split_region`) on its own thread.
  3. The thief side writes the resulting region as a task file instead of loading it.
- **Optionally, receiver-initiated:** an idle node also writes `requests/<node>`, and busy nodes check for requests at the same interval. This avoids splitting when nobody needs work, but the low-watermark rule alone is enough to start with.
- **Donation order matters for crash safety:**
  1. write the new task to `ready/.tmp-X` and fsync;
  2. write the donor's own checkpoint (its region now excludes X);
  3. write the marker `created/X`;
  4. rename `.tmp-X` to `ready/X`.

  A crash between steps leaves either the donor's old checkpoint, which still covers X, or a hidden `.tmp` file that recovery can publish. The coverage check (2.5) catches anything else.

### 2.4 Termination and idle nodes
- **A node with no work does not exit just because `ready/` is empty:** another node may soon donate. It waits, polls and keeps its heartbeat.
- **The search is complete exactly when every name in `created/` also has a file in `done/`.** Only an unfinished task can create new tasks, so the count of unfinished tasks cannot reach zero while work remains. This is the same rule as the in-node `active_` count.
- **Idle timeout** (`--pool-idle-timeout`, e.g. 15 min): a node that has had nothing to claim for this long exits with a distinct code (e.g. 5, "idle, search unfinished"), so an allocation isn't held idle. It is a cost limit, not a correctness rule.

### 2.5 Failures, recovery and the final merge
- **Dead nodes:** a node whose heartbeat is older than `--pool-stale` (e.g. 10 min) is presumed dead. Any other node moves its `claimed/<node>/*` tasks back to `ready/`, from their latest checkpoints, and records this in a log.
- **Time limits:** a time-limited node checkpoints (exit 4, as now) and returns its claimed tasks to `ready/` from those checkpoints, then exits; or its requeued job reclaims them.
- **Merge (`lpr --pool-merge pool/<search>`):**
  - read every `done/` file and check its fingerprint;
  - check that the finished tasks **tile the whole search exactly**: each task is a contiguous interval in lexicographic order (`tasks_disjoint` already sorts and compares these), and together they must run from the root task's start to its end with no gaps and no overlaps;
  - only then add up the statistics and print the usual summary.

  A run counts as verified only if the merge passes and every task's run completed with exit 0.

### 2.6 Filesystem load
With hundreds of nodes, polling every 10–30 s with one directory listing each is a few hundred metadata operations per minute, which is acceptable for Lustre, GPFS or NFS. Keep heartbeats at 30 s or more. Never poll in a tight loop.

## 3. Implementation steps
1. **Task files:** write and read single-task files, and `--tasks-out DIR N`: split the root into N tasks by repeated `split_region` with no processing (a static split, also useful on its own as a job array).
2. **Merge and coverage check** (`--pool-merge`), tested first on static splits: a job array of the N tasks, merged, must give a summary identical to `--single`.
3. **Pool node** (`--pool DIR`): claim, process, write `done/`, heartbeat, wait while unfinished tasks exist, idle timeout.
4. **Donation** at the low watermark, with the crash-safe order of 2.3.
5. **Recovery** of stale nodes and time-limited nodes.
6. **Slurm:** a `submit_pool.sh` that creates the pool and submits a job array of node jobs (each requeueable), plus `lpr --pool-status DIR` to show the queue, unfinished tasks and per-node state.

## 4. Verification plan (as for work stealing and checkpoints)
- **Correctness, always:** the merged summary must equal `--single` exactly (supports checksum, DFS counters, every consumer counter).
- **On one machine:** many pool "nodes" as separate processes sharing a directory (e.g. 8 processes × 1 thread, 3 × 4, 2 × 8) on OMEGA=33 S=30 and S=29.
- **Faults:**
  - kill nodes at random times, both cleanly (SIGTERM) and hard (SIGKILL);
  - start nodes late and stop them early;
  - simulate a dead node by stopping its heartbeat;
  - delay directory visibility (an artificial sleep around listings) to mimic NFS caching;
  - recover with the stale-node rule.
- **Merge refusals:** gaps, overlaps, duplicate `done/` files and a fingerprint mismatch must all be refused (exit 2).
- **Cluster:** two r730 nodes on Rangpur at OMEGA=32 S=29 (compare with the 24-thread single-node run), then the research cluster at increasing node counts, recording end-of-run idle time. That measurement decides whether MPI is ever needed.

## 5. Still on the TODO list: interleaved multiplications
> **Measured 25/10/26** (`experimental/legendre_pair/`, scratch builds in `.bench/pair` and `.bench/batch`). All runs gave EXACT results; option B even matched the search-effort counters, as it does no wasted work.
> - **Profile** (OMEGA=33 S=29, single thread, M2):
>   - root certification 49% of the time (the power tree 48%: about 97 exponentiations per prime, averaging 17 bits);
>   - Fermat 30%;
>   - DFS about 15%;
>   - trial division 2.6%;
>   - the Jacobi filter is almost free (18 ns per call).
> - **Option A (Legendre survivors of one prime in lockstep):** on the M2 K=2 is −6% single-threaded, and K=3–4 are worse. On Rangpur r730 (SMT off): with GCC 8.5 +3.5%, with clang −0.6%.
> - **Option B (different primes interleaved, operation-stream executor):** on the M2 −9 to −13% single-threaded. On Rangpur +13–18% with every compiler. Its microbenchmark speed-up is at most 1.08× on x86, so the executor's overhead is too high.
> - **Compiler effect on Rangpur** (`lpr`, current code):

>   | Build | S=30, single thread | S=29, 24 threads |
>   |---|---|---|
>   | GCC 8.5 | 17.37 s | 24.90 s |
>   | GCC 8.5 `-march=native` | 16.62 s | 23.71 s |
>   | clang 21 `-march=native` | 13.27 s | 18.97 s |

>   The multiply's code generation dominates.
> - **Conclusion:** on x86 the multiply is instruction-bound rather than latency-bound, so interleaving has no idle units to fill. Revisit A, and B with a tighter executor, after the handwritten kernel (priority 2).
- **What exists:** `experimental/consumer_ilp/` runs two independent Fermat exponentiations in lockstep, so the core overlaps their dependency chains. On the M2 that measured 1.95 → 1.37 µs per Fermat test, and 14–16% faster end to end with identical summaries (notes entry 18/10/26). Pairing Legendre survivors in the power tree is modelled at about 5% more.
- **Not adopted yet:** the prototype diff must be rebased onto the current code, and the debug cross-check of the paired path is missing.
- **Interaction with SMT:** hyper-threading and interleaving fill the same idle multiplier slots. Measure the 2×2 grid {interleaving off, on} × {1, 2 threads per core}:
  - on an SMT-off node (Rangpur r730), where interleaving should help most;
  - on an SMT-on research node.

  If the best choice depends on SMT, choose it at startup from `/sys/devices/system/cpu/smt/active` (the job script already prints it), or with a flag.
- **After adopting it,** re-tune `TRIAL_DIVISION_MAX_PRIME`: cheaper Fermat tests move the best trial-division limit.

## Priorities (25/10/26, set by the user)
1. **Highest: multi-node runs with MPI** (`mpi_plan.md`); the shared task pool (§2–4) is the fallback.
2. **Revised 25/10/26, lower on Haswell: a handwritten Montgomery kernel.** Measured on r730-3 (job 696192). A hand-written `mulx` CIOS kernel (74 instructions) is correct on hardware, but has 14% *worse* latency than clang's code (17.1 against 15.1 ns per product). One product already takes about 48 cycles with clang, close to Haswell's latency floor. Haswell is also limited by instruction throughput: `adc` and `mulx` are 2 micro-operations each, about 150 per product. The dedicated squaring (job 696241) gives −2.5% (Fermat) and −3.3% (powm) under clang, so adopt it for x86 clang builds; it is worse under GCC. Next: re-measure kernels and interleaving on Broadwell or newer (1-micro-operation `adc`, ADX). Original note: **a handwritten Montgomery kernel** for `mont3::mul`/`sqr` (x86 `mulx`/`adc`, ARM `mul`/`umulh`/`adcs`; intrinsics first, inline assembly where needed).
   - **Why:** exponentiation is about 80% of run time.
   - **Headroom:** on Rangpur's Haswell nodes, clang 21 spends about 70 cycles per 3-limb product and GCC 8.5 about 95, against a realistic 35–45.
   - **Expected gain:** perhaps 20–30% on whole runs.
   - **Verification:** randomised cross-checks in `experimental/montgomery3` and the debug build.
3. **Low but definite: affine trial division** with a per-thread prefix cache (§6.1 note; `.bench/affine`). Measured 1.9% faster single-threaded at OMEGA=33 S=29, with the limit raised to about 5000. The OMEGA=32 comparison is running locally.
4. **Done (25/10/26): clang with `-march=native`** in `build_lpr.sh`: −24% on Rangpur (below).
5. **Interleaving (§5):** revisit after item 2 (results below).

## 6. Single-node speed-ups to do first (planned 24/10/26)
The search-size estimates (`experimental/estimate`, 24/10/26) put OMEGA=31 S=28 at roughly 2×10^12 supports, about 3 weeks on 24 Rangpur cores, and OMEGA=30 S=27 at roughly 4–5×10^13 supports, about 1–1.5 years on 24 cores (lower bounds; see `experimental/estimate/README.md`). Each step down in OMEGA costs roughly 20–30× more. The ideas below are verifiable exactly, like everything else: identical summaries, supports checksum and DFS counters.

### 6.1 Trial division by the residue of d (`process_support`, `trial_division.hpp`)
> **Under review (25/10/26): this plan has issues, so don't implement it as written.**
> - **Per-support setup doesn't pay:** it needs one residue per trial prime (189 now, about 1,200 for a limit of 10^4) for every support, but there are only about 1.5–2 candidates per support (1.48 at OMEGA=32, about 1.9 estimated at OMEGA=30), so there is almost nothing to spread that cost over. The "almost free at OMEGA=30" claim below confused large `d_max` with many candidates.
> - **The current method is already economical:** one 3-limb remainder per chunk of 6–15 primes, then one multiply per prime.
> - **Ideas to reconsider:**
>   - incremental prefix residues along the DFS;
>   - sieving the last-level siblings by residue class (the only variant whose cost falls relative to today's);
>   - first, measuring the time split and the `TRIAL_DIVISION_MAX_PRIME` tuning curve with the current code.
>
> The user is reconsidering this separately.
- **Now:** each candidate p = Q·d + 1, a number of up to 190 bits, is reduced modulo 64-bit products of small primes (`remainder_3` per chunk), then tested per prime.
- **Idea:** for a small prime r not dividing Q, r | p ⟺ Q·d ≡ −1 (mod r) ⟺ d ≡ t_r := −Q⁻¹ (mod r). So, per support:
  - compute Q mod r for every trial prime (one pass over Q's limbs per chunk, as now, but per support rather than per candidate);
  - invert it (a table of inverses mod r, or one extended-gcd step);
  - after that, each candidate needs only `d % r == t_r` on a 32-bit d, or a Granlund–Montgomery divisibility test of d − t_r. That's no multiprecision arithmetic per candidate, and it vectorises across primes.
- **Primes dividing Q** (support primes) never divide p and are skipped, as now.
- **Gain:** grows with candidates per support. OMEGA=32 has 1.48 per support (modest gain); OMEGA=30 has much larger d_max (up to about 2×10^5 seen, against 965 at OMEGA=32), so trial division would become almost free there.
- **Also:** re-tune `TRIAL_DIVISION_MAX_PRIME` afterwards. Cheaper trial division moves the best limit up, which removes more Fermat tests.
- **Verification:**
  - identical counts per trial-division chunk;
  - a debug cross-check against `remainder_3` for every candidate;
  - an exhaustive test of the residue computation for every r in the table.

### 6.2 Log-domain DFS checks with an exact fallback (`run_region`, `sibling_fate`, `split_region`)
- **The identity:** with r = SOP/Q = Σ 1/p over the Corollary 4 window, Q cancels:
  - lhs/rhs = 4218 · 2^(16(ω−s)) · ((s+1−2r)/(1−r))^16;
  - bound = floor(lhs/rhs − 1).
- **The check:** each node needs only log(product), log(stored bound) and r, so a check costs about 60 floating-point operations (about 30 log and reciprocal additions plus one log) instead of about 30 GMP multiplications and two 16th powers.
- **Exactness:** every comparison whose margin is below 1e-9 in log space is decided by the exact GMP check (float error is about 1e-12). The DFS's decisions therefore stay exact.
- **Still exact where it matters:** leaves still compute `d_max` and Q exactly for the job, because `process_support` needs the exact values. The exact frames can be rebuilt lazily for leaves only, or kept for the last levels.
- **Evidence:** in `experimental/estimate` this is implemented and validated:
  - it reproduces OMEGA=33 S=30 exactly with 6 fallbacks;
  - its children ranges match exact ones at 9,600 of 9,600 random nodes.
- **Expected gain:** the DFS is about 15% of a run at OMEGA=33, and less where processing per support grows. Also faster splits and estimates.
- **Verification:**
  - identical DFS counters and checksum;
  - debug builds run both checks on every node and compare;
  - a count of fallbacks per run in the producer summary.

### 6.3 Later, if still needed: GPU Fermat tests
- **Why the Fermat test:** it is a fixed-size (3-limb) Montgomery exponentiation over huge independent batches, the ideal GPU workload. Montgomery arithmetic is about 79% of the work (notes, 18/10/26).
- **Split of work:** the CPU keeps the DFS, trial division and certification of g(p).
- **The proof needs safeguards:** a wrongly reported "composite" silently drops a prime. Use ECC hardware, CPU recomputation of a random sample (or of all survivors), and per-batch checksums.
- **When:** consider it after 6.1, 6.2, interleaving (§5) and the shared pool (§2) are in, if the remaining gap justifies it.
