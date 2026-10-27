# Plan: from verified code to the OMEGA=31 computation and the write-up

Status: **agreed 27/10/26**, revised the same day; the plan of record. It supersedes the "Pinned" list in `mpi_checkpoint_plan.md`: dynamic allocations and node tolerance are steps 3 and 4 here; hardware-specific kernels are a future idea (end of this file). Steps are done in order; each ends with a verification gate.

## 1. Verification on Rangpur
- **What:** job 704199, OMEGA=32 S=29 on 2 nodes with a 10-minute limit. It must checkpoint at the time-limit warning, requeue itself, resume, and repeat until complete.
- **Gate:**
  - "Time limit approaching" followed by "Requeueing" in each run;
  - "Continuing from lpr.ckpt" from run 2 on;
  - a final summary identical to job 697335 (checksum `0xc66b5efd1a7ac845`).
- **Then, optionally:** the same test with periodic checkpoints (`LPR_CHECKPOINT_INTERVAL=120`), and a 3- or 4-node OMEGA=32 run.

## 2. Tuning on Friday
- **Compilers:** 1-node OMEGA=33 S=29, GCC 12.3 against the Clang 16 module (`module load` it, then `LPR_CXX=clang++`). If Clang 16 falls well short of Rangpur's clang 21 gain, install LLVM 21 into `~/opt` (the prebuilt release, or conda-forge).
- **Scaling:** 3- and 4-node OMEGA=32 runs on 28-CPU nodes.
- **Limits:**
  - `sacctmgr show qos`, `sacctmgr show assoc user=$USER`, `scontrol show partition`;
  - `sbatch --version` (`--prefer` needs Slurm 22.05 or later);
  - `sinfo -o "%20N %4c %10m %40f"`: node features and sizes.
- **Also:** an MPI run on Friday with a short time limit, to check checkpoint and requeue there (Open MPI, `srun --mpi=pmix`).
- **Gate:** exact summaries, and a chosen build (compiler and flags) and node type for OMEGA=31.

## 3. Dynamic allocations, to use Friday better for OMEGA=31
- **`lpr_mpi.slurm`:**
  - `#SBATCH --nodes=MIN-MAX`, so a job starts with whatever nodes are free;
  - `--prefer=<feature>` for the larger nodes, and `--mincpus` to exclude small ones;
  - no fixed `--cpus-per-task`.
- **Per-node thread counts:** each rank takes its thread count from its own node (`SLURM_CPUS_ON_NODE`, read in `run_rank.sh` on that node), so mixed 28- and 36-CPU nodes each use every core. Rank 0's pool and the donations already balance nodes of different speeds.
- **Gate:** the fake-Slurm simulation with mixed thread counts and node counts, exact; then a mixed-node run on Friday.

## 4. Tolerating lost nodes, and nodes joining a running computation
**Goal:** a multi-day OMEGA=31 run that survives a node failure without restarting the whole job from the last checkpoint, and that can take on more nodes as they become free.

Design questions to settle before implementing:
- **Losing a rank.** Plain MPI aborts the whole job, and neither cluster has fault-tolerant MPI (ULFM). The options:
  - **(a) Several small jobs instead of one large one:** independent jobs of 1–2 nodes each share one search through a coordinator. That could be a rank 0 job reached over a socket, or a file-based task pool on the shared file system (`multinode_plan.md` §2). A failed job loses only its own tasks, which the coordinator re-issues from their last reported position.
  - **(b) MPI dynamic processes** (`MPI_Comm_accept`/`connect`): new jobs connect to a running rank 0. Support depends on the MPI library and on Slurm's configuration, so it must be tested on both clusters.
- **Re-issuing lost work exactly once.** Rank 0 knows which tasks it issued to each rank. Each rank must periodically report its current task positions (a light per-rank checkpoint), so that rank 0 can re-issue a lost rank's work from those positions without double-counting its statistics. Those statistics must be reported together with the positions, so the totals stay exact.
- **Nodes joining:** with (a), a newly started job simply asks the coordinator for work. Termination must then account for jobs that start after others have finished.
- **Gate:**
  - local tests that kill a rank or job mid-run, and add one;
  - every final summary identical to the reference;
  - the existing checkpoint chains still exact.

## 5. Consolidating the codebase
- **Release:** commit everything, and tag a release that all production runs use.
- **Remove dead code:** `--parallel-queue`, if it is retired; experimental leftovers.
- **Archive the reference outputs** and checksums: OMEGA=33 S=30/29 and OMEGA=32 S=29, on each cluster.
- **`.gitignore`:** add `.rangpur/` and `.friday/`, and decide about `CLAUDE.md`.
- **`docs/production_runs.md`:** how to submit, monitor, resume and verify a run, and what counts as a verified result: exit 0, 0 Grosswald failures, 0 unresolved, checksum and supports recorded.
- **Then the OMEGA=31 S=28 production run:**
  - estimated 2.6×10¹² supports and 4.6×10¹² candidates (lower bound; the calibrated upper estimate is about 3× more);
  - roughly 5 days on about 100 cores;
  - optionally an independent dry run on the other cluster, comparing supports emitted and the checksum.

## 6. Full documentation and the paper
- **The method:** `current_method.md`, updated: the mathematics and the exact Corollary 4 checks, the DFS and its pruning, the primality and least-primitive-root certification.
- **Why the search is exhaustive and correct:** domination, the region invariants, checkpoint consistency, the MPI protocol's termination and exactly-once guarantees.
- **The computational record:** clusters, builds, commits, run times, checksums, and every OMEGA verified.
- **The paper:** results, method, verification and timings.

## Future ideas (not part of the current plan)
- **Hardware-specific kernels:**
  - **An ADX kernel:** `mulx` with `adcx`/`adox`, behind `#ifdef __ADX__`, so per-node `-march=native` builds pick it automatically. Friday's Broadwell and Gold 6240 nodes support ADX.
  - **AVX-512:** unlikely to help. The Gold 6240 (Cascade Lake) lacks IFMA.
  - **Adopt only** for a clear end-to-end gain (say over 5%) that survives the existing cross-checks (`lpr_g`, `experimental/montgomery3`).
- **Further speed:** further gains beyond constant factors need improvements to the mathematics (e.g. stronger pruning bounds) rather than to the code; relevant for OMEGA=30.
