# Running lpr on a Slurm cluster

This directory holds everything needed to run `lpr` as Slurm jobs:

| File | Role |
|---|---|
| `submit.sh` | Run on the **login node**. Prepares a run directory and submits the job. |
| `slurm/lpr.slurm` | The job script template: resources (`#SBATCH` lines), the build step and the `lpr` command. |
| `slurm/build_lpr.sh` | Run **inside the job** on the compute node. Finds a working compiler and GMP (building GMP if needed) and builds `lpr`. |
| `Makefile` | Optional shortcuts: one target per common kind of run (`make -C ~/lpr/scripts help`). |

## How a run works

`submit.sh` never builds or runs anything on the login node. For each submission it:

1. updates the repository (`git pull --ff-only`);
2. exports the chosen **commit** into a new **run directory**, `~/lpr_runs/<name>_<date>_<commit>/` (just `<date>_<commit>` without `-n`), and copies `build_lpr.sh` into it;
3. writes `RUN_INFO` there: the commit, branch, time, `lpr` arguments, build settings and job id;
4. submits the job with that directory as its working directory.

The job then builds `lpr` on the compute node, runs it, and writes its output (`lpr_<jobid>.out`, `.err`, `.log`) into the same directory.

**Why each run gets its own copy:**
- **The code is frozen.** A queued job might start hours later; a `git pull` or edit in the meantime cannot change what it runs.
- **Jobs don't interfere.** Two jobs never build into the same directory.
- **Each run is a complete record.** The directory holds the exact source, binary, settings and output, which is what you want for a proof by computation.

A copy is about 0.4 MB, mostly the docs.

**Only committed and pushed code reaches a run.** Uncommitted changes are not included (`submit.sh` prints a note if there are any).

## One-time setup on the cluster

1. **Clone the repository** on the login node, using an SSH key added to GitHub:
   ```bash
   git clone -b optimisation_33 git@github.com:PeterJCampbell1/least-primitive-roots-computation.git ~/lpr
   ```
2. **Make your own copy of the job file**, outside the repository, so that `git pull` never conflicts with your edits:
   ```bash
   cp ~/lpr/scripts/slurm/lpr.slurm ~/lpr_cluster.slurm
   ```
   In the copy:
   - set `--partition`. Find one that accepts your job with `sinfo -s` and `sbatch --test-only --partition=<p> ~/lpr_cluster.slurm`.
   - set or delete `--account`. If `sacctmgr show assoc user=$USER` lists nothing, disable the line by writing it as `##SBATCH --account=...`. A literal `<account>` makes `sbatch` fail.
   - set `--cpus-per-task`, `--time` and `--mem` defaults. Any of these can be overridden per run (see below).
   - add `module load` lines only if the cluster provides suitable compiler or GMP modules. Otherwise leave them out; `build_lpr.sh` finds what is installed.

   **Re-copy it whenever `slurm/lpr.slurm` changes in the repository**, then re-apply your edits. An old copy will not understand newer options.
3. **Add these to `~/.bashrc`**, then run `source ~/.bashrc`:
   ```bash
   export LPR_JOB=~/lpr_cluster.slurm      # your job file
   export LPR_RUN_ROOT=$HOME/lpr_runs      # where run directories go; must be visible from every node
   ```
4. **GMP:** nothing to do if the compute nodes have GMP's development files, or if they can download it. Otherwise copy the source once from your own computer:
   ```bash
   curl -LO https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
   scp gmp-6.3.0.tar.xz <user>@<cluster>:~
   ```
   The first job that needs GMP builds it into `~/opt/gmp-6.3.0`, which takes about 10–15 minutes including GMP's self-tests. Later jobs reuse it. Allow for that in the first job's `--time`.

   Before using any GMP, the build runs a self-check of its multiplication and `powm` code on the node. A GMP that doesn't work there is skipped: for example, a system GMP built for a newer CPU, which dies with "Illegal instruction" (seen on Rangpur's Haswell `r730` nodes). The GMP built in `~/opt` is a "fat" binary on x86-64, so it runs correctly on every node type sharing your home directory.

## Submitting runs

Always submit from the **login node**. `submit.sh` refuses to run inside a job (e.g. an `srun --pty` session), because that job's per-job directories and environment would leak into the new one.

```bash
~/lpr/scripts/submit.sh [options] [-- sbatch options]
```

### Choosing the search: `-a` / `--lpr-args`

These arguments replace the job file's default (`--config config/omega33.conf`). The job adds `--parallel` (work stealing) unless you give a mode yourself. `lpr` accepts only one of `--single`, `--parallel`, `--parallel-queue` (the previous producer/consumer design) and `--dry-run`.

```bash
submit.sh -n w33-s29   -a "--config config/omega33.conf --s 29"        # OMEGA=33, S=29, parallel
submit.sh -n w32       -a "--omega 32 --s 29" -- --time=08:00:00       # OMEGA=32 (hours)
submit.sh -n single    -a "--config config/omega33.conf --single" -- --cpus-per-task=1
submit.sh -n dry32     -a "--omega 32 --s 29 --dry-run"                # producer only
```

A new config file must be committed and pushed before a run can use it, because runs use the committed source. Command-line flags such as `--omega` and `--s` need no commit.

### Resources: options after `--`

Everything after `--` is passed to `sbatch` and overrides the job file's `#SBATCH` lines:

```bash
submit.sh -n big -- --cpus-per-task=16 --time=02:00:00 --mem=8G --partition=<p>
```

`lpr` takes its thread count from `--cpus-per-task`. With `--parallel` (work stealing) every thread searches and processes its own part of the tree, so there is no single-producer limit. With `--parallel-queue` (the old design), more than about 12–16 threads mostly adds idle consumers at OMEGA=33.

### Compiler, flags and PGO

| Option | Effect |
|---|---|
| (none) | Try, in order: clang++ with LTO (if `ld.lld` exists); g++ with `-flto=auto`; g++ with `-flto` (older GCC); then either without LTO. The first that compiles, links and runs a GMP test program is used. |
| `--cxx g++` / `--cxx clang++` | Use only that compiler. |
| `--cxxflags "-march=native"` | Extra compiler flags, added to the defaults. The job builds on the node it runs on, so `-march=native` is safe. |
| `--pgo` | Profile-guided build: an instrumented build, a training run, then the final build. On the M2, clang + PGO was ~10% faster and GCC + PGO ~8% slower. Clang PGO needs `llvm-profdata` on the node. |
| `--pgo-train "..."` | `lpr` arguments for the training run (default `--single`, OMEGA=33, ~7 s). |
| `--gmp-tarball FILE` | Where the GMP source is, if not `~/gmp-6.3.0.tar.xz`. |

The output's `[build_lpr]` lines and the run header's `build:` line show what was actually used (PGO builds show `commit <sha>-pgo`).

### Other options

| Option | Effect |
|---|---|
| `-n NAME` | Label at the start of the run directory name: `-n w32` gives `~/lpr_runs/w32_<date>_<commit>/`. |
| `-c REF` | Run a specific commit, e.g. `-c 2aca1ee`, instead of the branch tip. |
| `-b BRANCH` | Pull and run another branch. |
| `--no-pull` | Don't update the repository first. |
| `--dry-run` | Prepare everything and print the `sbatch` command without submitting. |
| `--help` | Full description. |

## Long runs: checkpoints and automatic requeueing

Every job (except `--parallel-queue`) saves its progress to `lpr.ckpt` in its run directory: every 600 s (`LPR_CHECKPOINT_INTERVAL`), and when Slurm sends SIGTERM 5 minutes before the time limit (`--signal=TERM@300`). `lpr` then exits with code 4, and the job **requeues itself**. The requeued job keeps its job id, rebuilds `lpr` on whichever node it gets, finds `lpr.ckpt` and continues from it. This repeats until the search is complete, or until 20 runs (`LPR_MAX_RUNS`). So a search longer than the partition's time limit, such as OMEGA=32, can be submitted once with any `--time`.

- **Output:** every run is appended to the same `lpr_<jobid>.out`. Its header says `resumed from: lpr.ckpt (run k; …)`, and the final summary is identical to an uninterrupted run's, with an extra line `Elapsed time over all k runs`.
- **Continuing by hand** (if requeueing is disabled with `LPR_REQUEUE=0`, `scontrol requeue` is not allowed on your cluster, or the run limit was reached): run `resubmit.sh` in the run directory, optionally with new `sbatch` options:
  ```bash
  ~/lpr_runs/<run>/resubmit.sh --time=12:00:00
  ```
  It repeats the original submission in the same directory, using the job file as it was submitted (`job.slurm` there).
- **Refusals:**
  - `lpr` refuses to resume a checkpoint from a different search or commit. Each run directory is one commit, so this only matters if files are copied between directories.
  - It refuses to start a new search over an existing checkpoint without `--resume`.
- **After completion** `lpr.ckpt` is marked `complete 1`. A stray requeue just prints the final summary again.
- **Job state:** `sacct -j <jobid>` shows a requeued job as `REQUEUED`, then `PENDING`/`RUNNING` again. `scontrol show job <jobid> | grep Restarts` counts the restarts.

Details: [`docs/checkpointing.md`](../docs/checkpointing.md).

## Checking on runs

```bash
squeue -u $USER                                   # PD = waiting, R = running
sacct -j <jobid> --format=JobID,State,ExitCode,Elapsed,NodeList
cd "$(ls -td ~/lpr_runs/*/ | head -1)"            # newest run directory
cat RUN_INFO
grep -E "^\[build_lpr\]|^build:|^mode:|^threads:|examined|certified:|Largest least|inequality failed|Elapsed time|^lpr " lpr_*.out
```

A successful verification run ends with `lpr finished: verification complete` and exit code 0. Grosswald failures and unresolved candidates must both be 0. For reference, on OMEGA=33:
- **S=30:** 1,343,862 supports, 114,064 certified, largest root 454;
- **S=29:** 43,601,360 supports, 3,605,989 certified, largest root 502.

## Common problems

| Symptom | Cause and fix |
|---|---|
| Job `FAILED` within seconds, with no `.out` file in the run directory | The compute node couldn't enter the run directory. `LPR_RUN_ROOT` must be shared storage, such as your home directory, and not a per-job `$SCRATCH`. Submit from the login node. |
| `sbatch` rejects the job | Partition, account line or limits in your job file. Check them with `sbatch --test-only`. |
| Job stays pending (`PD`) | Partition busy or restricted; `squeue -u $USER -o "%i %T %r"` shows the reason. |
| `Build failed on <node>` | The `[build_lpr]` lines in `lpr_<jobid>.out` show which step failed: compiler, GMP download, GMP build or `make`. |
| `internal compiler error: Illegal instruction` in `lpr_<jobid>.err` | The *compiler* crashed: GCC uses the system GMP (through MPFR) to fold constants, and on the r730 nodes that GMP dies with SIGILL. It happens when a source file includes `<cmath>`, which `lpr` therefore avoids (job 692610, 24/10/26). `build_lpr.sh` now tries the next compiler when a build fails, and warns if the compiler cannot handle `<cmath>`. |
| `cannot download GMP from this node` | The compute nodes have no internet access. Copy `gmp-6.3.0.tar.xz` to `~` (see setup step 4). |
| `lpr did not start: usage error` | Bad `--lpr-args`, e.g. two mode flags. `lpr_<jobid>.err` has `lpr`'s message. |
| Results differ from the reference values | A real problem: stop and inspect the full output. |
| `lpr was interrupted and saved its progress` but the job did not come back | Requeueing failed or was disabled (the `.out` file says which). Run `resubmit.sh` in the run directory. |
| `cannot resume ... written by a different search` | `lpr.ckpt` belongs to another search (omega, s or a dry run). Use the run directory it came from. |
| `checkpoint file 'lpr.ckpt' already exists` | A new search was started in a directory that already holds one. Don't reuse run directories; `submit.sh` always makes a new one. |

## The optional Makefile

`scripts/Makefile` wraps common runs, one `submit.sh` call per target:

```bash
make -C ~/lpr/scripts help                          # list the targets
make -C ~/lpr/scripts single PARTITION=<p>           # OMEGA=33 S=30 single-threaded
make -C ~/lpr/scripts s29 CPUS=16                    # OMEGA=33 S=29
make -C ~/lpr/scripts compare                        # gcc, clang and clang + PGO
make -C ~/lpr/scripts test OPTS=--dry-run            # show what would be submitted
make -n -C ~/lpr/scripts omega32                     # print the exact submit.sh command
```
