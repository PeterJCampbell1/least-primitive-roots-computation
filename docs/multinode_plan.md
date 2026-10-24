# Plan: one search across several nodes (shared task pool), and interleaved multiplication

Status: **plan, written 24/10/26; not implemented.** It replaces the sketch in `cluster_roadmap.md` Stage 5. It builds on work stealing (`work_stealing.md`) and checkpoints (`checkpointing.md`).

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
- **What exists:** `experimental/consumer_ilp/` runs two independent Fermat exponentiations in lockstep, so the core overlaps their dependency chains. On the M2 that measured 1.95 → 1.37 µs per Fermat test, and 14–16% faster end to end with identical summaries (notes entry 18/10/26). Pairing Legendre survivors in the power tree is modelled at about 5% more.
- **Not adopted yet:** the prototype diff must be rebased onto the current code, and the debug cross-check of the paired path is missing.
- **Interaction with SMT:** hyper-threading and interleaving fill the same idle multiplier slots. Measure the 2×2 grid {interleaving off, on} × {1, 2 threads per core}:
  - on an SMT-off node (Rangpur r730), where interleaving should help most;
  - on an SMT-on research node.

  If the best choice depends on SMT, choose it at startup from `/sys/devices/system/cpu/smt/active` (the job script already prints it), or with a flag.
- **After adopting it,** re-tune `TRIAL_DIVISION_MAX_PRIME`: cheaper Fermat tests move the best trial-division limit.
