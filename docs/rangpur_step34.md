# Steps 3 and 4 on Rangpur: dynamic allocations and the shared pool (instructions)

Part of `production_plan.md` (steps 3 and 4; design in `elastic_pool.md`). Runs only, no code changes.
- **Prerequisites:** the commit with `src/pool.hpp` (commit and push it first), and step 1 verified (job 704199).
- **Rangpur facts used:** partition `largecpu` (24-core r730 nodes, QOS `spot`: at most **3 of your jobs at once**); MPICH with `srun --mpi=pmi2`; no `--mem` line.

Run everything on Rangpur's login node, from `~/lpr`. Every pool directory must be on storage all nodes see (your home is), and is named by an **absolute** path.

## 0. Preparation
1. **Update the repository:**
   ```bash
   cd ~/lpr && git pull --ff-only && git log --oneline -1
   ```
2. **Two job files** from the updated template. Copy it twice, outside the repository:
   ```bash
   cp ~/lpr/scripts/slurm/lpr_mpi.slurm ~/rangpur_lpr_mpi.slurm       # fixed 24 threads per rank
   cp ~/lpr/scripts/slurm/lpr_mpi.slurm ~/rangpur_lpr_mpi_dyn.slurm   # each rank takes its node's CPUs
   ```
   In **both**:
   - set `#SBATCH --partition=largecpu`;
   - comment out the `--account` and `--mem` lines (`##SBATCH …`);
   - set `#SBATCH --time=01:00:00`;
   - set `MPI_MODULE=${MPI_MODULE:-mpi/mpich-x86_64}`.

   Then:
   - in `~/rangpur_lpr_mpi.slurm`, set `#SBATCH --cpus-per-task=24`;
   - in `~/rangpur_lpr_mpi_dyn.slurm`, comment out the `--cpus-per-task` line (`##SBATCH --cpus-per-task=<cpus>`).
3. **A directory for the pools:** `mkdir -p ~/pools`.

## A. Dynamic thread counts (no pool), 2 nodes, OMEGA=33 S=29
```bash
LPR_JOB=$HOME/rangpur_lpr_mpi_dyn.slurm scripts/submit.sh -n t34_dyn -a "--config config/omega33.conf --s 29" -- --nodes=2 --time=00:15:00
```
Check `lpr_mpi_<jobid>.out`:
- the job line says `each rank uses its node's CPUs (24(x2))`;
- `Launch:` shows `--whole` (or `-c 24` if this Slurm has no `--whole`);
- `Running:` shows `--threads 0`;
- in the `MPI summary` table, the `threads` column shows 24 for both ranks;
- `Supports checksum: 0xf3964d4fe09f4fa3`, about 10–12 s.

## B. A pool shared by two jobs, OMEGA=33 S=29
Two single-node jobs (a job array) share one pool; the first to start creates it.
```bash
LPR_POOL=$HOME/pools/t34_b LPR_JOB=$HOME/rangpur_lpr_mpi.slurm scripts/submit.sh -n t34_b \
    -a "--config config/omega33.conf --s 29" -- --array=1-2 --nodes=1 --time=00:20:00
```
Check:
- both `.out` files: a `Pool: …/t34_b` line, then `lpr_mpi left the pool: no work left for this job`;
- **the job that finished last** ends with `Pool … is complete: every task created was finished by exactly one lease. Whole search:`, followed by the merged summary. It must show `Supports checksum: 0xf3964d4fe09f4fa3`, `K-prime supports examined: 43601360`, 0 failures and 0 unresolved;
- each job's own share ("This job's share of the search") adds up to the whole: its `K-prime supports examined` values sum to 43,601,360;
- `ls ~/pools/t34_b/done`: `INIT` plus one record per job; `~/pools/t34_b/ready` and `leases` are empty.

## C. A job killed outright (a node failure), OMEGA=32 S=29
Three single-node jobs share the pool. After a few minutes one is killed without any warning, which is what a node failure looks like. The others must release its lease once its heartbeat is 2 minutes old (`LPR_POOL_STALE=120` for the test; the default is 600 s), then take over its remaining work.
```bash
LPR_POOL=$HOME/pools/t34_c LPR_POOL_STALE=120 LPR_CHECKPOINT_INTERVAL=120 LPR_JOB=$HOME/rangpur_lpr_mpi.slurm \
    scripts/submit.sh -n t34_c -a "--omega 32 --s 29" -- --array=1-3 --nodes=1 --time=01:30:00
# Once all three are RUNNING (squeue -u $USER), wait about 5 minutes, then kill one with no grace period:
scancel --signal=KILL --full <jobid>_2
```
Check:
- one of the surviving jobs' `lpr_mpi_<jobid>.log` has a line `pool: released stale lease …`, about 2 minutes after the kill;
- the last job to finish prints `Pool … is complete`, with `Supports checksum: 0xc66b5efd1a7ac845` and `K-prime supports examined: 6057734933`. Expect roughly 20–30 minutes in total;
- `ls ~/pools/t34_c/done` holds `INIT` plus 3 records, including the killed job's.

If the merged summary doesn't appear (e.g. the last job hit its time limit), check the pool with any job's binary (the ranks' build directory is in the run directory):
```bash
srun -p largecpu -N1 -n1 --mpi=pmi2 --time=00:05:00 ~/lpr_runs/<a t34_c run dir>/build_<host>/lpr_mpi --omega 32 --s 29 --pool-status $HOME/pools/t34_c
```

## D. Time limits: leave, requeue, rejoin; OMEGA=32 S=29
Two single-node jobs with a 10-minute limit. At each time-limit warning a job returns its work to the pool, requeues itself and rejoins, until the search is done.
```bash
LPR_POOL=$HOME/pools/t34_d LPR_CHECKPOINT_INTERVAL=120 LPR_JOB=$HOME/rangpur_lpr_mpi.slurm \
    scripts/submit.sh -n t34_d -a "--omega 32 --s 29" -- --array=1-2 --nodes=1 --time=00:10:00
```
Check each `.out`:
- every run ends with `Time limit approaching`, `lpr_mpi left the pool at the time limit, returning its work`, and `Requeueing job … to rejoin the pool`;
- the final run ends with `lpr_mpi left the pool: no work left for this job`;
- the job that finished last prints the complete merged summary (checksum `0xc66b5efd1a7ac845`).

This takes roughly an hour, depending on queueing between runs.

## E. (Optional) a flexible node count with mixed sizes
```bash
LPR_POOL=$HOME/pools/t34_e LPR_JOB=$HOME/rangpur_lpr_mpi_dyn.slurm scripts/submit.sh -n t34_e \
    -a "--config config/omega33.conf --s 29" -- --nodes=1-3 --time=00:20:00
```
The job starts with however many nodes (1–3) are free; the job line shows `N nodes`. The merged summary must again show checksum `0xf3964d4fe09f4fa3`.

## What to send back
For each test, copy every run directory and the pool directory, each into its own folder, leaving out the sources and builds:
```bash
# on your own machine
for d in $(ssh rangpur 'ls -d ~/lpr_runs/t34_*'); do
  rsync -a --exclude src --exclude docs --exclude utils --exclude experimental --exclude 'build_*' rangpur:$d/ .rangpur/t34/$(basename $d)/
done
rsync -a rangpur:pools/ .rangpur/t34/pools/
```

## Expected results
| Test | Search | Checksum | Supports |
|---|---|---|---|
| A, B, E | OMEGA=33, S=29 | `0xf3964d4fe09f4fa3` | 43,601,360 |
| C, D | OMEGA=32, S=29 | `0xc66b5efd1a7ac845` | 6,057,734,933 |
