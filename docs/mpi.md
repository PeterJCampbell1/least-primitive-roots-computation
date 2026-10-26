# MPI: one search across several nodes (M1 + M2)

Status: **implemented 26/10/26** (steps M1 and M2 of [`mpi_plan.md`](mpi_plan.md); checkpoints under MPI, M4, are not done yet). Code: `src/mpi_comm.hpp` (the protocol), the external endpoint in `src/work_stealing.hpp`, the `--mpi*` options in `src/cli.hpp`, `main.cpp` (start-up, final reduction), `make lpr_mpi`, `scripts/slurm/lpr_mpi.slurm`. Verified locally (x86-64 Open MPI 5.0.3 under Rosetta, oversubscribed) and on 2 Rangpur nodes (MPICH).

## 1. Usage
```bash
make lpr_mpi                                   # with mpicxx; or MPICXX=clang++ plus the MPI include/lib paths
srun --mpi=pmi2 -N 2 -n 2 -c 24 ./lpr_mpi --parallel --mpi --threads 24   # MPICH under Slurm
mpirun -np 4 ./lpr_mpi --dry-run --mpi --threads 24                       # Open MPI
LPR_JOB=$HOME/lpr/scripts/slurm/lpr_mpi.slurm scripts/submit.sh -n mpi_s29 \
    -a "--config config/omega33.conf --s 29" -- --nodes=2 --time=00:15:00
```
- `--threads N` is per rank (per node). Run **one rank per node**; with `mpirun`, add `--map-by ppr:1:node --bind-to none` (Open MPI otherwise binds each rank, and so all its threads, to one core).
- Only rank 0 prints the header and summary. The summary is the merge of every rank's statistics, so it must equal `--single`'s exactly. An "MPI summary" table follows, with one row per rank.
- **Logs:** every rank logs, each line tagged `rank r:`.
  - Without `--log-file`, the lines go to stderr.
  - With `--log-file PATH`, rank 0 writes PATH and rank r writes `PATH.rank<r>`, so ranks never share a file on the shared filesystem. `lpr_mpi.slurm` passes `--log-file lpr_mpi_<jobid>.log`.
  - **Rank 0's progress line** starts with rank 0's own counts, then adds totals over every rank:
    - `all_ranks_emitted`, `all_ranks_processed`, `all_ranks_candidates`, `all_ranks_fermat_survivors`, `all_ranks_certified`;
    - `all_ranks_failed`, `all_ranks_unresolved`, `all_ranks_largest_g`;
    - `mpi_pool` (rank 0's pool).

    The totals come from each rank's once-per-second Status, so they lag by a second or two.
  - **`candidates_per_s`, and `done=`/`eta=` with `--expected-candidates`,** use the all-ranks candidate total on rank 0.
  - Warnings always go to stderr as well.
- **Options:**
  - `--mpi-request-idle N` (default 2, capped at `--threads`): the L1 trigger.
  - `--mpi-pool-reserve N` (default 4): the L2 reserve.
  - Testing only: `--mpi-static` (M1) and `--mpi-stress`.
  - `LPR_MPI_TRACE=1` logs every message.
- **Refused with exit 2:**
  - `--mpi` with `--checkpoint`/`--resume` (until M4), `--single`, `--parallel-queue` or `--split-test`;
  - more than one rank without `--mpi`;
  - `--mpi` in the plain `lpr`.
- **Exit codes:** as `lpr`, the same on every rank (rank 0 broadcasts its code), plus 5 when a rank aborts the job (an MPI fatal error, e.g. the watchdog).

## 2. Design
See the header comment of `src/mpi_comm.hpp` for the protocol in full. In short:
- **One rank per node.** Each rank runs the existing work-stealing `Scheduler` with N worker threads. The main thread, the one that initialised MPI (`MPI_THREAD_FUNNELED`), is the rank's communication thread (`lpr::mpi::Node`). Workers never call MPI.
- **The communication thread joins the scheduler's protocol as one more thread** (the scheduler's "external endpoint", thief id N):
  - `add_task` puts a received task in the pending list;
  - `post_external_request` asks a busy worker to split, using the same request slot and mailbox as a thief, so the split is the existing `split_region` on the victim's own thread;
  - `region_left` is called after a donated region has been sent away;
  - `finish` is called at the global end.
  - With an external endpoint, `active_ == 0` only means "nothing on this rank": idle workers back off (at most 1 ms) and keep answering requests until `finish`.
- **The hierarchy of where work comes from:**
  - **L0:** steal within the node (unchanged, microseconds).
  - **L1:** the node asks rank 0 once `request_idle` workers have been idle for 2 ms, or at once if it holds nothing. At most one request per node is open, and none while a received task is still pending here.
  - **L2:** rank 0 serves from its pool. When the pool cannot cover the open requests plus `pool_reserve`, it sends DONATE to the busy node with the shallowest base depth, preferring nodes without an open request. A node that refuses is not asked again for 20 ms.
- **Messages:** every message to rank 0 carries the sender's Status: tasks received, regions held, idle workers, the shallowest busy base depth and progress counts. Tasks travel as checkpoint index paths and are verified by replay on arrival (`DfsStack::restore`); a donated region is also compared frame by frame with its replay.
- **Termination:** a rank whose last Status says "0 regions, received k" with k = tasks issued to it holds nothing. Messages between two ranks arrive in order, so its earlier donations have arrived. When the pool is empty, no DONATE is unanswered and every rank is in that state, rank 0 sends STOP. Every rank answers STOP_ACK as its last message; then `MPI_Gather` merges the statistics with `Counters::merge` (sums, maxima, the wrapping checksum).
- **The watchdog:** rank 0 sends a heartbeat every 10 s and every rank sends a Status every 1 s. Silence for 120 s in either direction means `MPI_Abort`, so a dead or hung rank ends the job instead of idling until the time limit.
- **Failures:**
  - Any rank dying ends the job (plain MPI has no fault tolerance), and rank 0 is no worse than the others.
  - Until M4 the run's work is lost. With M4 (coordinated checkpoints by rank 0) plus `--requeue`, the job resumes from the last checkpoint on any number of nodes.

## 3. Verification log (25/10/26, M2 Air under Rosetta, x86-64 Open MPI 5.0.3, oversubscribed)
Every test compares the full search and producer summaries with the single-threaded reference (`cmp.sh`: every count, the winners histograms, the supports checksum and the DFS counters; only timing and stealing lines are ignored). The test harness (`run_matrix.sh`, `cmp.sh`, `overhead_bench.sh`) lived in the untracked scratch copy `.bench/mpi/`.

| Matrix | What | Round 1 | Round 2 (final) |
|---|---|---|---|
| plumbing | S=30: M1 static at 1/2/4 ranks × 1/2 threads; M2 at 1/2/3/4/8 ranks × 1/2/4 threads | 21/21 | 21/21 |
| dry | dry runs, S=30 and S=29, 1/2/4/8 ranks × 1/3 threads, M2 and M1 | 32/32 | 32/32 |
| stress | `--mpi-stress` (random delays, late starts, constant donations) 3 reps × 2/4/8 ranks × 1/2/3 threads; `--mpi-request-idle 1`; pool reserve 0 and 8; `--steal-stress`; dry stress at S=29 | 37/37 | 37/37 |
| s29 | full S=29 runs (43,601,360 supports) at 1×4, 2×2, 4×2, 8×1, and stress 4×2 | 5/5 | 5/5 |
| overhead | S=29 with 8 workers: plain `lpr` against 1×8, 2×4 and 4×2 ranks, twice each | — | 8/8 |

- **Other checks:**
  - OMEGA=34 (no supports) completes with 3 ranks, dynamic and static.
  - Usage errors exit 2 on every rank: more than one rank without `--mpi`; `--single --mpi`; `--mpi --checkpoint`; `--mpi` in the plain `lpr`.
  - The unmodified `src/` (x86 build) and the scratch `lpr` (with the scheduler hooks, no MPI) take the same time at S=29 with 8 threads: 86.5/87.0 s against 86.4/85.9 s.
- **Speed:**
  - **Scaling, one thread per rank, S=30:** 14.0 s on 1 rank; 7.2 s on 2 (97% efficient); 5.0 s on 3 (93%); 3.8 s on 4 (91%).
  - **Dry run at S=29:** 61.2 s, 31.4 s and 16.7 s on 1, 2 and 4 ranks.
  - **Beyond 4 ranks** the M2's efficiency cores and the oversubscription limit it.
  - **The MPI layer adds no measurable cost:** S=29 with 8 workers takes 85–91 s for every split between ranks and threads, the same as plain `lpr`.
- **Found and fixed:**
  1. Ranks other than 0 printed the host list with an empty vector, a segfault.
  2. **About 1.2 s of idle time per rank at the start.**
     - **Cause:** the rank holding the root had reported "no busy worker", as its task had only just arrived, and rank 0 doesn't ask such ranks to donate. The next Status came only with the 1 s heartbeat.
     - **Fix:** a rank now reports as soon as it first has a busy worker, and ranks with an unknown base depth are last-resort donors.
     - **Effect:** per-rank idle time went from 1.2 s to 0.1 s, and 8 ranks × 1 thread at S=30 from 3.5 s to 2.7 s.
- **Signals:**
  - **Under `srun`,** every rank receives SIGTERM directly. Locally we sent it to each rank: a rank exits 3, and the launcher reports 3.
  - **Under `mpirun`,** SIGTERM to `mpirun` kills the ranks before they handle it: the job exits 1 with no summary, and no processes are left.
  - **So M4** should use `srun`, or `--signal=USR1` plus a deadline option, as in the plan.
- **Not verified yet:**
  - ThreadSanitizer (it doesn't run under Rosetta);
  - long multi-node runs (OMEGA=32) and more than 2 nodes.

### Overhead benchmark (26/10/26, quiet M2, 76/76 runs exact; medians, ranks × threads)
| Search | Plain lpr (unmodified src/) | Scratch lpr | MPI 1×N | MPI 2×N/2 | MPI 4×N/4 | MPI 8×1 |
|---|---|---|---|---|---|---|
| S=30, 8 workers (5 runs each) | 2.51 s | 2.48 s | 2.50 s | 2.54 s | 2.60 s | 2.58 s |
| S=30, 4 workers (5 runs) | 3.83 s | — | 3.65 s | 3.78 s | 3.80 s | — |
| S=29, 8 workers (3 runs) | 84.8 s | 85.1 s | 80.8 s | 82.5 s | 83.9 s | 83.8 s |
| S=29, 4 workers (2 runs) | 129.0 s | — | 126.1 s | 127.7 s | 128.5 s | — |

- **On long runs the MPI overhead is within noise.** The spread inside a configuration is about ±6%.
- **On 2.5 s runs, 4–8 ranks cost about 0.05–0.1 s:** start-up and the first handovers.

### Rangpur hello test (jobs 698724 and 698725, 2 r730 nodes, 26/10/26)
- **Slurm offers PMI-2 only** (`srun --mpi=list`: none, cray_shasta, pmi2), with no PMIx.
- **MPICH (`mpi/mpich-x86_64`):** `srun --mpi=pmi2` works, and FUNNELED is available.
- **Open MPI 4.1.1 (`mpi/openmpi-x86_64`):** it isn't built with Slurm's PMI, so `srun` fails. `mpirun` works, with harmless "No available port on bond0" warnings.
- **Common to both:** the ping-pong takes 34 µs per round trip between nodes, and the reduce is correct.
- **Choice:** MPICH with `srun --mpi=pmi2` is the default in `lpr_mpi.slurm`, because `srun` signals every rank directly, which M4 needs. The build calls clang directly with MPICH's include and library paths, not `mpicxx`: the wrapper adds Red Hat hardening flags (`-D_GLIBCXX_ASSERTIONS`, `-specs=...`).

### Tuning and donation size (26/10/26, Rangpur, OMEGA=33 S=29, 2 nodes x 24 threads; all runs exact)
- **Job 698769 (r730-2 and r730-3):**
  - defaults (request at 6 idle, reserve 1): 10.82 s, 10.72 s;
  - reserve 4: 12.18 s (unstable), 10.28 s;
  - request at 2 idle: 10.52 s, 10.60 s;
  - **both: 10.40 s, 10.44 s.**

  So the defaults are now reserve 4 and request at 2 idle.
- **Job 698790 (same nodes; 1 node takes 21.08 s):**
  - donating the upper 2/3: 10.50 s and 10.62 s, with 405 and 472 tasks moved;
  - donating the upper 1/2: 10.45 s and 10.28 s, with 307 and 203 tasks moved.
- **Why 2/3 doesn't help:** a split gives away only untouched base-level siblings, and the sibling in progress (and its subtree, usually most of the work) stays with the donor. A bigger fraction of the untouched siblings adds little work per piece.
- **What would help:** a work-aware split that steps into the current subtree.
- **Local re-verification with the new defaults:** 111/111 exact. This used a temporary `--mpi-donate N/D` option, run at 1/2, 1/3, 9/10 and 63/64 with and without stress.
- **The result:** donations went back to the same half split as local steals, and the option was removed.

### OMEGA=32 S=29 on 2 nodes (job 699035, r730-2 and r730-3, commit 31e950f, 26/10/26)
- **Exact:** the full summary (108 lines) is identical to the 1-node runs 697335 (clang) and 690984 (GCC):
  - 6,057,734,933 supports, checksum 0xc66b5efd1a7ac845;
  - 0 failures, 0 unresolved, largest g(p) 678.
- **Time:** 1,638.6 s, against 3,452 s on 1 node with clang (r730-0), so 2.11×. The nodes differ: r730-3 is an E5-2680 v3 at 2.5 GHz, and r730-0 and r730-2 are E5-2670 v3 at 2.3 GHz.
- **Idle time is negligible on a long run:** 36.2 s on rank 0 and 1.3 s on rank 1, out of 78,650 worker-seconds (0.05%). The donation-size question above matters only for short runs.
- **Transfers:** 636 tasks issued and 687 donations requested.

## 4. Next steps
1. **Rangpur:** 4 nodes (OMEGA=32 on 2 nodes is done: job 699035, exact, 2.11×).
2. **M4:** coordinated checkpoints (rank 0 PAUSE → drain → gather tasks and counters → one file) plus `--stop-at`/USR1. Then requeue on multiple nodes.
3. **Bigger donations:** a work-aware split that can step into the donor's current subtree. Today the rank without the large part of the search idles about 5–10% of its worker time on 2 nodes.
4. **M3 (peer stealing):** only if rank 0 limits scaling.
