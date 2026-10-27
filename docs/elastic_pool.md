# Elastic runs: one search shared by many jobs (`--pool`), and dynamic allocations

Status: **implemented and verified locally, 27/10/26** (steps 3 and 4 of `production_plan.md`). Code: `src/pool.hpp` (the pool directory), pool mode in `src/mpi_comm.hpp` (rank 0's `pool_step` and `finish_pool_collecting`), `src/main.cpp` and `src/cli.hpp` (`--pool`, `--pool-status`, `--threads 0`), `scripts/slurm/lpr_mpi.slurm` (`LPR_POOL`, dynamic node counts).

## 1. Why
A multi-day run (OMEGA=31) should:
- use whatever nodes become free, while it runs;
- survive a node failure without losing more than a few minutes;
- fit within each cluster's per-job limits.

Plain MPI can do none of that inside one job: a world has a fixed size, and losing a rank aborts it. So the search is shared by **several independent jobs**, each an ordinary MPI job of 1 to n nodes (`mpi.md`), cooperating through a directory on the shared file system. Jobs join at any time, leave when done or at their time limit, and may die at any time.

## 2. Usage
```bash
# Every job names the same directory (on storage all nodes see); the first creates it
LPR_POOL=$HOME/pools/k31 sbatch --array=1-4 --nodes=1-2 scripts/slurm/lpr_mpi.slurm    # (job file filled in)
mpirun -np 2 ./lpr_mpi --parallel --mpi --threads 24 --omega 31 --pool $HOME/pools/k31
# Seed a new pool from a single-node checkpoint (its statistics are carried over)
mpirun -np 2 ./lpr_mpi --parallel --mpi --omega 31 --pool $HOME/pools/k31 --resume k31.ckpt
# Report, and once complete verify and print the whole search's summary (any machine, no MPI)
./lpr --omega 31 --pool-status $HOME/pools/k31
```
- **Options:**
  - `--pool-poll` (10 s): how often rank 0 looks at the directory;
  - `--pool-stale` (600 s): when another job's lease counts as dead;
  - `--pool-idle-timeout` (1800 s): when a job with nothing to do leaves while others still work;
  - `--checkpoint-interval` (600 s): how often the lease state is written.
- **Not allowed:** `--checkpoint`; the pool holds the state.
- **Each job prints** its own share of the search. The job that finds the pool complete also prints the **merged summary of the whole search**, which is the result to compare. `--pool-status` prints the same merged summary any time later, and exits with 0 (complete and verified), 1 (complete, with a failure) or 3 (not complete).
- **Exit codes in pool mode:**
  - 0: the pool has nothing left for this job;
  - 4: signal or time limit; the job returned its work to the pool and requeues itself to rejoin;
  - 5: fatal, e.g. the job's lease was taken over because its heartbeat stopped.
- **Dynamic allocations** (`lpr_mpi.slurm`):
  - **Node counts:** `--nodes=MIN-MAX` starts with as many nodes as are free.
  - **Thread counts:** without `--cpus-per-task`, every rank uses all the CPUs of its own node (`srun --whole` and `--threads 0`, which counts the CPUs the process may use), so a job may mix node sizes. The MPI summary shows each rank's threads.
  - **Node types:** prefer the large nodes with `--prefer=<feature>` (Slurm 22.05+), and exclude small ones with `--mincpus`.

## 3. The pool directory
```
SEARCH            the pool exists (written last by the job that creates it)
ready/<id>        unclaimed tasks (checkpoint format, one task, zero statistics)
ready/.tmp-<id>   a task written but not yet published
leases/<L>/       one per running job: state (its lease record), heartbeat, claimed/<id>
leases/.releasing-<L>...  a lease being released (by its job, or by another job if stale)
done/<L>          the final record of every finished lease; done/INIT donates the root
requests/<L>      a job with nothing to do asks for work
```
**A lease record** is written atomically (temporary file, fsync, rename) and carries a checksum. It holds:
- the lease's id;
- the ids of the tasks it **claimed**;
- the ids it **donated**, i.e. gave away as new ready tasks;
- an embedded checkpoint (single-node format): the fingerprint, the lease's statistics, and its remaining tasks.

A job has one lease for its lifetime, so its statistics are simply the job's totals, collected from every rank.

## 4. The rules, and why nothing is lost or counted twice
- **Recording.** Rank 0 records every change to what the job holds with a coordinated collection (PAUSE, then PAUSED from every rank; `mpi.md` §4), which writes the lease state. It also writes it every `--checkpoint-interval`. So a lease state always describes one consistent cut: the remaining tasks plus statistics covering exactly the work done before them.
- **Claim:** `rename(ready/X, leases/L/claimed/X)`. The rename is atomic, so exactly one job gets X. The next state write records "claimed X".
- **Donation:** write `ready/.tmp-X`, then a state write that records "donated X" and no longer lists X among the remaining tasks, then rename to `ready/X`.
- **Release** (a job leaving, or any job releasing a lease whose heartbeat is older than `--pool-stale`):
  1. Rename the lease directory to `.releasing-…`. Only one releaser wins. A live owner whose directory has vanished fails its next write and stops (exit 5) without recording anything else.
  2. Return to `ready/` any claimed file not recorded in the state.
  3. Publish recorded `.tmp` donations; delete unrecorded ones.
  4. Write the state's remaining tasks as new donated ids.
  5. Write `done/L`, publish, and remove the directory.

  A releaser that finds `done/L` already written only completes the publishing, so a release that stopped halfway can be finished by anyone. Releases left unfinished for longer than the stale limit are finished by the next job.
- **Exactly once:**
  - **Statistics:** every lease's statistics enter the result once, through `done/L`, taken from its last recorded state.
  - **Work after that state** is lost when a job dies: it was never recorded, and its tasks are republished from that state, so they are processed again exactly once.
  - **Every task id is created once,** as a donation recorded in exactly one record (ids are `<lease>.<n>`, and the root is `INIT.0`).
- **Complete when:**
  - `ready/` and `leases/` are empty;
  - every donated id is claimed by exactly one done record;
  - every claimed id was donated.

  Only then are the done records' statistics merged (`Counters::merge`). The merged summary must equal a single-node run's exactly, including the supports checksum and the DFS counters.

## 5. How rank 0 drives a job (`pool_step`, every `--pool-poll`)
- **Heartbeat:** written every `min(30 s, stale/4)`. If its lease directory has vanished, the job stops at once.
- **Stale leases:** each poll releases other jobs' stale leases, so their tasks become ready.
- **Claiming:**
  - **The job holds nothing:** it claims up to one task per rank.
  - **Ranks are hungry and nothing in the job can be split:** it claims one task.
  - **Every claim** is followed by a collection that records it.
- **Nothing to claim:**
  1. The job records its empty state, so other jobs see it as idle, and posts a request.
  2. It leaves (exit 0) when nothing is ready and no other lease still has work.
  3. Otherwise it leaves after `--pool-idle-timeout`.
- **Giving work:** while another job's request is open and `ready/` is empty, the job gives away one spare task per poll, from rank 0's pool (otherwise it first asks a rank to split one off).
- **Signals** (SIGTERM, SIGINT, SIGUSR1, or Slurm's time-limit warning): a final collection, then release the lease, returning the remaining work to the pool, then exit 4.

## 6. Verification log (local, x86-64 Open MPI 5.0.3 under Rosetta, 27/10/26)
Each "job" is a separate `mpirun`; all of them share one pool directory. Test options: `--pool-poll 0.3 --pool-stale 4 --checkpoint-interval 2`. Every pool's merged summary is compared with the single-threaded reference.

| Test | Result |
|---|---|
| One 2-rank job, S=30 | exact |
| 3 jobs joining at 0, 3 and 6 s; S=29 full and dry | exact; tasks moved between jobs in both directions |
| A job SIGKILLed mid-run (a node failure) while another works; S=29 full and dry | exact; the survivor released the dead lease |
| The only job SIGKILLed; a new job takes over after the stale limit | exact |
| A job signalled (SIGUSR1) to leave, then a new job rejoins; S=29 dry, S=30 full | exact; exits 0, 4, 0 |
| A pool seeded (`--resume`) from a single-node S=29 checkpoint taken after 15 s | exact |
| Chaos: 6 random schedules of up to 8 jobs (1–3 ranks), with random kills and SIGUSR1s, S=29 dry and S=30 with `--mpi-stress` | exact |
| Debug build (`lpr_mpi_g`): 4 jobs, one killed, one signalled | exact, no assertion failures |
| Regression after the pool changes: the MPI matrix (104 runs), periodic checkpoints, 11 interrupt-and-resume chains, edge cases and refusals | all exact |
| `lpr_mpi.slurm` (fake Slurm, 2 fake nodes) without `--cpus-per-task`, `SLURM_JOB_CPUS_PER_NODE="3,2"` | each rank counted its own CPUs (`--threads 0`); exact |
| `lpr_mpi.slurm` with `LPR_POOL`: two jobs; both get the time-limit warning, return their work and requeue; one rejoins and finishes | the merged summary printed by the last job, and `--pool-status`, exact; 4 done records |

**Not yet tested:** on a real cluster (`srun --whole`, `--nodes=MIN-MAX`, `--prefer`, a pool shared over NFS or Lustre, `scancel --signal=KILL` as a node failure). See `rangpur_step34.md`.
