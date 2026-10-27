# Plan: coordinated checkpoints under MPI, and job-template updates

Status: **Parts 1 and 2 implemented and verified locally, 27/10/26** (`mpi.md` §4); Part 3 is for runs on the clusters; the pinned items are deferred. Builds on `mpi.md` (the MPI design), `checkpointing.md` (the single-node checkpoints and file format) and `mpi_plan.md` §3.5 (the M4 outline).

## Part 1: coordinated checkpoints for MPI runs (M4), in `src/`
**Writing a checkpoint.** Rank 0 coordinates it, every `--checkpoint-interval` seconds or on a signal:
1. **Pause:** rank 0 broadcasts PAUSE. Each rank stops requesting and donating work, finishes or refuses any donation in progress, and pauses its workers with the existing `Scheduler::pause`.
2. **Drain:** rank 0 waits until nothing is in transit. Its per-rank counts of tasks issued and received say exactly when every TASK and TASK_IN has arrived.
3. **Collect:** each rank sends rank 0 its remaining tasks, checked by replay as today (`Scheduler::remaining_tasks`), and its `Counters`.
4. **Write:** rank 0 adds its pool, checks that no two tasks overlap (`tasks_disjoint`) and that their number matches, merges the statistics, and writes **one file in today's format** (atomic rename, fingerprint, checksum).
5. **Finish:** everyone resumes after a periodic checkpoint, or exits with code 4 after a signal. A final checkpoint marked complete is written at the end, as today.

**Resuming:**
- Only rank 0 reads the file. It checks the fingerprint and replays every task, as `main` does now, then puts all the tasks into its pool. The other ranks start empty.
- The earlier runs' statistics are merged on rank 0 only, so the final reduction counts them once.
- The number of ranks and threads may differ between runs. A single-node checkpoint can be resumed under MPI and the other way round: it is the same format.

**Signals:**
- Every rank that receives SIGTERM, SIGINT or SIGUSR1 reports it to rank 0 through its communication thread. Rank 0 runs one checkpoint and tells every rank to exit with code 4.
- Duplicate reports are ignored.
- Without a checkpoint file, every rank exits with code 3, as now.

**Edge cases:**
- a signal during start-up, before tasks are handed out;
- a signal while the search is finishing (STOP already sent): complete normally, as single-node runs do;
- a pause timeout: log it, skip that checkpoint and keep running;
- the watchdog during a long pause: pauses must stay well under its 120 s.

**Tests, locally with Open MPI under Rosetta, each compared exactly with the references:**
- interrupt-and-resume chains in which the rank count changes between segments (e.g. 2 → 4 → 1 → 3), for full and dry runs at S=30 and S=29, with and without `--mpi-stress`;
- periodic checkpoints every 1 s;
- cross-mode resumes (MPI ↔ `--parallel` ↔ `--single`);
- refusals: a different fingerprint, a damaged file, a file from another search;
- the debug build (`lpr_mpi_g`) for every scenario;
- the existing MPI matrix (plumbing, stress, dry, debug, S=29) as a regression check;
- speed: no slowdown against the current build with checkpoints off, and the pause cost with them on.

## Part 2: `scripts/slurm/lpr_mpi.slurm`
- **Checkpoint and requeue, as in `lpr.slurm`:**
  - `#SBATCH --signal=TERM@300` and `--requeue`;
  - `--checkpoint lpr.ckpt --checkpoint-interval ${LPR_CHECKPOINT_INTERVAL:-600}`, plus `--resume lpr.ckpt` if the file exists;
  - `scontrol requeue` on exit code 4, up to `LPR_MAX_RUNS`; `LPR_REQUEUE=0` turns requeueing off.
- **Rebuilds on requeue:** a requeued job rebuilds on its new nodes, so `-march=native` stays correct when the nodes change.
- **`export PMIX_MCA_psec=native`** when the MPI library is Open MPI, which silences PMIx's "psec: munge component not found" warnings.
- **Tests:** the fake-Slurm simulation, with a fake `scontrol requeue` that re-runs the job: interrupt, requeue, resume and finish, with an identical summary.
- **Docs:** `mpi.md`, `checkpointing.md`, `scripts/README.md`, `CLAUDE.md`, and a dated entry in `optimisation_notes.md`.

## Part 3: on the clusters (no code changes)
1. **Friday compilers:** 1-node OMEGA=33 S=29 with g++ and with the Clang 16 module. If Clang 16 falls well short of Rangpur's gain, install LLVM 21 into `~/opt` (the prebuilt release, or conda-forge) and build with `LPR_CXX`.
2. **Scaling:** 3- and 4-node OMEGA=32 runs on Friday and Rangpur.
3. **Limits:** check each cluster's (`sacctmgr show qos`, `scontrol show partition`, `sbatch --version`, `sinfo -o "%20N %4c %40f"`) to size OMEGA=31's job segments.
4. **Requeue under MPI:** after Parts 1 and 2, an MPI OMEGA=32 run with a short `--time`, so that it checkpoints, requeues and resumes at least once.

## Pinned (deferred)
- **Dynamic allocations:**
  - `--nodes=2-4`;
  - `--prefer=<feature>` for the larger nodes, `--mincpus`;
  - per-node thread counts from `SLURM_CPUS_ON_NODE` in `run_rank.sh`;
  - nodes joining or leaving a running job.
- **An ADX kernel** (`mulx` with `adcx`/`adox`, behind `#ifdef __ADX__`, so per-node `-march=native` builds pick it automatically). Benchmark it on Friday's Broadwell nodes, which have ADX.
- **AVX-512:** low priority. The Gold 6240 (Cascade Lake) lacks IFMA, and 32×32-bit vector multiplies fit this arithmetic poorly.
- **Tolerating the loss of a node** while the others keep running (see `mpi.md`, failures): needs fault-tolerant MPI or re-issuing a lost rank's work. Covered for now by periodic checkpoints and requeue.
