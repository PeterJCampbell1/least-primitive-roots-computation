# Checkpoint and resume: design, invariants and verification log

Status: **implemented 24/10/26** (phase 6 of [`work_stealing_plan.md`](work_stealing_plan.md)). Code: `src/checkpoint.hpp` (file format), `src/work_stealing.hpp` (pausing the workers), `src/generate_supports.hpp` (`Task`, `DfsStack::task`, `DfsStack::restore`), `src/main.cpp` (options, monitor, final checkpoint), `scripts/slurm/lpr.slurm` and `scripts/submit.sh` (cluster workflow).

## 1. Usage

```bash
./lpr --parallel --checkpoint run.ckpt                     # checkpoint every 600 s, on SIGTERM/SIGINT and at the end
./lpr --parallel --checkpoint run.ckpt --checkpoint-interval 60
./lpr --parallel --resume run.ckpt --checkpoint run.ckpt   # continue (any mode and thread count) and keep checkpointing
./lpr --single   --resume run.ckpt                         # continue without writing further checkpoints
```

- **On SIGTERM or SIGINT** `lpr` writes a checkpoint, prints the statistics saved in it, and exits with **code 4**. Without `--checkpoint` it exits with code 3 as before.
- **Works with** `--single`, `--parallel` and `--dry-run` (any thread count), but not with `--parallel-queue` or `--split-test`. A checkpoint may be resumed in a different mode or with a different thread count, but a dry-run checkpoint only by a dry run, and a full run's only by a full run.
- **A finished search** writes a final checkpoint marked `complete 1` with no tasks. Resuming it prints the same final summary again, so "resume if the file exists" is always safe.
- **Refused with exit 2:**
  - the checkpoint was written by a different search: omega, s, the prime list, the bound B, `PRIMITIVE_ROOT_SEARCH_LIMIT`, the trial-division range, or dry run versus full run;
  - it was written by a different commit (accept that with `--resume-any-commit`; the header then prints a warning);
  - the file is damaged, truncated or edited (checksum);
  - a task is not a position the DFS can reach (checked by replay, §4), or two tasks overlap.
- **Also refused at startup:**
  - `--checkpoint F` when `F` already exists and `--resume F` was not given, so an earlier run's progress is never overwritten by mistake;
  - a checkpoint path whose temporary file cannot be created (so a typo doesn't surface hours later).
- **Header lines:** `checkpoint: …` and `resumed from: … (run k; T tasks; N supports already processed; E s in earlier runs)`. With more than one run, "Elapsed time over all k runs" follows "Elapsed time". Every other line of the summary is identical to an uninterrupted run.

## 2. What a checkpoint holds

Work stealing processes supports inline, so a worker's progress **is** its DFS position: everything before the position has been searched and processed, and nothing after it has. No completion buffer or watermark is needed (unlike the queue design in `parallelise_producer.md`).

- **A `Task` per region that still exists:** `base_depth`, `base_limit`, the current `depth` and the index path `path[0..depth]`. `path[d]` for `d < depth` is the prime index chosen at depth `d`, and `path[depth]` is the next index to try at the current depth. This is exactly the `DfsStack` state at the top of a `run_region` iteration.
  - A worker holding a region contributes its stack's position (`DfsStack::task`).
  - A region in transit, handed over by a victim but not yet loaded by the thief (mailbox `FILLED`), contributes `region_task(mailbox)`.
  - Tasks not yet taken (from a resumed checkpoint) contribute themselves.
- **The statistics:** every `Counters` field, listed by `Counters::for_each_field`. `main` checks at startup that the listed fields cover `sizeof(Counters)`, so a counter added later without being listed stops the program instead of being silently dropped from checkpoints.
- **The fingerprint** (§1), the number of runs so far (`segments`), their total elapsed time, and `complete`.

## 3. Taking a consistent snapshot (`Scheduler::pause`)

The monitor thread is the only coordinator; periodic and signal checkpoints both run on it, so they never overlap.

1. **Request:** `pause()` sets `pause_requested_`.
2. **Workers stop at safe points.** Every worker checks the flag at:
   - the top of every `run_region` iteration (in `poll`, before answering steal requests), where its stack is between nodes;
   - every iteration of its idle loop (`find_work`), including while it waits for an answer to a steal request.

   A stopping worker first publishes its thread-local statistics. It then records its stack pointer (`nullptr` if idle), increments `parked_` and waits for the pause generation to change.
3. **The coordinator waits until every worker has stopped,** with a timeout of 120 s. While all are stopped, nothing changes: no DFS step, no support processed, no split, no handover. So the master statistics cover exactly the work before the recorded positions.
4. **Collect and check** (`remaining_tasks`). The coordinator collects the tasks as in §2 and checks:
   - **each task, by replay** (§4), against the live frames, or against the mailbox frames for a region in transit;
   - **the number of tasks against `active_`,** the scheduler's count of existing regions (held, in transit or pending). This catches a region that would be lost.
   - **that the tasks are disjoint** (`tasks_disjoint`), which catches a region listed twice. In lexicographic order of index tuples, a task covers `[path, (path[0..base_depth), base_limit))`. Sorted by their start, each such interval must end at or before the next one begins.
5. **Write** the checkpoint (§5), then `resume()` increments the generation and the workers continue. On a signal the workers are left stopped and the process exits after printing the saved statistics.

Edge cases:
- **A steal request** posted before the pause stays posted; the victim answers it after the pause.
- **A victim already inside `answer()`** when the pause starts finishes the handover first (`FILLED`). It then stops at its next poll, so the coordinator sees the region in the thief's mailbox.
- **The search is finishing:** a worker exits only when no region exists, so if any worker has exited when the coordinator looks, the search is complete. `pause()` then returns `SearchFinishing`, and so does a pause that finds no tasks (the last region has just finished but no worker has exited yet). On a signal the run then completes normally and writes the final checkpoint.
- **Single-threaded runs** with `--checkpoint` or `--resume` go through the scheduler with one worker. That is the same search (`--parallel --threads 1` is identical to `--single`) and the same pause code.
- **Signals:** handlers are installed at the start of `main`, so a signal during startup is handled once the monitor runs. Only a signal in the first few milliseconds, while the process is still loading, kills it (exit 143); the previous checkpoint is untouched, and the Slurm script treats that case like exit 4. The monitor checks for signals once a second; a run that finishes in that time completes normally with exit 0.

## 4. Resuming: rebuilding frames by replay (`DfsStack::restore`)

A task stores only indices. `restore` starts from the root (`reset_root`, as the constructor does) and replays the path:
- each choice `path[d]` is applied with the same arithmetic as `run_region`: `delta_Q`, `delta_sop` (from depth `OMEGA − S`), the product, the first-child reuse of the parent's bound, the minimal-completion checks and the `min` with the parent's bound, and the prefix hash;
- it refuses any path that `run_region` could not be at:
  - indices out of range or not increasing;
  - a prefix that cannot be completed;
  - a choice outside the region at the base depth;
  - a prefix that the inherited bound or Corollary 4 would have pruned.

The replay duplicates `run_region`'s node logic rather than sharing it, so the hot loop stays untouched. **Every checkpoint write cross-checks the two:** each task is replayed and its frames are compared with the live frames (`product`, `delta_Q`, `delta_sop`, `cor4_bound`, `prefix_hash` at every depth up to the current one). Any difference refuses the checkpoint, and the previous checkpoint stays in place. `main` also replays every task of a checkpoint, and checks that the tasks are disjoint, before starting a resumed run.

## 5. File format and atomic writes (`src/checkpoint.hpp`)

The file is text with one `key value` per line in a fixed order: `lpr-checkpoint 1`, the fingerprint, `segments`, `elapsed_seconds`, `complete`, then one `stat <name> <value>` line per counter, `tasks N` and N lines `task <base_depth> <base_limit> <depth> <path…>`. A final `checksum <hex>` line holds the FNV-1a 64 hash of everything before it. The parser is strict: every key exactly once and in order, every statistic present, the right number of tasks, nothing after them, and `complete` equivalent to "no tasks". At OMEGA=33 a file is about 31 KB, mostly the `winners_by_g` histogram.

`write_atomic`:
1. writes `PATH.tmp` and fsyncs it;
2. **reads it back** and checks that it parses and re-serialises to the same text;
3. renames it over `PATH`;
4. fsyncs the directory.

A failure at any step leaves the previous checkpoint intact. A failed periodic checkpoint logs a warning and the run continues. A failed checkpoint on a signal falls back to exit code 3.

## 6. Cluster workflow (`scripts/slurm/lpr.slurm`, `scripts/submit.sh`)

- **Checkpointing is always on:** the job runs `lpr --checkpoint lpr.ckpt --checkpoint-interval ${LPR_CHECKPOINT_INTERVAL:-600}` in the run directory, plus `--resume lpr.ckpt` when that file exists. It is not available with `--parallel-queue`.
- **Requeueing at the time limit:** `--signal=TERM@300` makes Slurm send SIGTERM 5 minutes before the limit, and `lpr` checkpoints and exits 4. The job then calls `scontrol requeue $SLURM_JOB_ID` (`#SBATCH --requeue`), as long as `LPR_REQUEUE=1` (the default) and fewer than `LPR_MAX_RUNS` (default 20) runs have been made. The requeued job rebuilds `lpr` on its new node (same commit, so the fingerprint matches) and continues. `--open-mode=append` keeps every run's output in `lpr_<jobid>.out`.
- **By hand:** `submit.sh` writes `resubmit.sh` into each run directory. It repeats the same `sbatch` submission, with the same environment and a copy of the job file (`job.slurm`), and accepts extra `sbatch` options, e.g. `resubmit.sh --time=12:00:00`.
- **Re-copy the job file:** if you use your own copy of the job file (`~/lpr_cluster.slurm`), copy `scripts/slurm/lpr.slurm` again and re-apply your edits.

## 7. Verification log

### Step 1: building blocks (24/10/26, M2)
- Periodic checkpoints in uninterrupted runs gave identical summaries (EXACT):
  - `--single` with a checkpoint every 0.2 s;
  - `--parallel --threads 8` every 0.05 s: 23 checkpoints in 1.3 s.
- **Error handling, all correct:**
  - A run killed after 0.6 s exits 4, and its printed summary equals the saved statistics (supports and DFS nodes checked).
  - **Refused with exit 2:**
    - a truncated file;
    - an edited statistic (checksum);
    - a different s;
    - a dry run resuming a full run;
    - a missing file;
    - a different commit;
    - a task edited to an unreachable position (with a valid checksum): "pruned by its inherited bound";
    - each invalid option combination;
    - an unwritable path;
    - an existing checkpoint without `--resume`.
  - **Exit 0:** `--resume-any-commit` accepts the other commit (with a header warning, and EXACT), and `--resume F --checkpoint F` on the same file completes and leaves `complete 1`.
- **Slurm simulation** (fake `srun` and `scontrol`, `.bench/slurm_sim`):
  - job 1 was killed by SIGTERM after 2.5 s, exited 4 and requeued itself;
  - job 2 resumed from `lpr.ckpt` and completed (EXACT);
  - a third, stray requeue resumed the complete checkpoint and printed the same summary (EXACT).
  - `submit.sh --dry-run` wrote `resubmit.sh` and `job.slurm`, and `resubmit.sh --time=08:00:00` passed the original settings plus the override.
- **Two problems found and fixed:**
  1. A SIGTERM during startup, before the handler was installed, killed the process (exit 143). The handlers are now installed first in `main`. Only signals in the first milliseconds, before `main` runs, can still kill it; the checkpoint is then untouched (tested), and the Slurm script and the test harness treat 143 with an existing checkpoint like exit 4.
  2. An unwritable checkpoint path only produced warnings. It is now refused at startup.
3. There was no check that the saved tasks do not overlap; now `tasks_disjoint` runs at every write and on resume. A checkpoint with a task listed twice (valid checksum) is refused with "tasks 8 and 9 overlap", and a real 24-task checkpoint (24 threads, stress) is accepted and resumed with 3 threads (EXACT).

### Step 2: final campaign (24/10/26, M2, `.bench/ck_campaign.sh` → `.bench/ck_campaign.log`, under `caffeinate -i`)
All on the final code; **0 failures**.
- **Interrupt/resume chains** (`.bench/ck_chain.sh`):
  - **What a chain does:** each run is interrupted by SIGTERM or SIGINT after a random delay, except every fourth, which runs to the end. The next run resumes in a random mode and thread count (`--single`, or `--parallel` with 2, 3, 8 or 16 threads; dry runs with 1, 2, 5 or 8 threads), with checkpoints either every 0.05 s or only on signals.
  - **What must hold:** the final summary must be EXACT against an uninterrupted `--single` run, and resuming the final (complete) checkpoint must print the same summary again.
  - **Results:**

    | Build and search | Chains passed |
    |---|---|
    | Release, S=30 full | 40 of 40 |
    | Release, S=30 dry | 30 of 30 |
    | Release, S=29 dry | 6 of 6 |
    | Release, S=29 full | 3 of 3 (4 runs each) |
    | Debug build `lpr_g` | 4 of 4 |
    | ThreadSanitizer, dry | 6 of 6 |
    | ThreadSanitizer, full | 3 of 3 |
    | ASan + UBSan, full | 3 of 3 |

  - No sanitizer reports, assertion failures or refused checkpoints.
- **Frequent periodic checkpoints in uninterrupted runs:** all EXACT, with 0 sanitizer reports and 0 refused checkpoints.

  | Build and run | Checkpoint interval | Checkpoints |
  |---|---|---|
  | TSan, dry run, 8 threads, stress | 10 ms | 31 |
  | TSan, parallel, 4 threads | 20 ms | 989 |
  | TSan, single | 20 ms | 3,446 |
  | ASan, dry run, 16 threads, stress | 10 ms | 20 |
  | Release, parallel, 8 threads | 10 ms | 98 |
  | Release, parallel, 24 threads, stress | 10 ms | 99 |
  | Debug, parallel, 8 threads | 50 ms | 155 |

- **Error handling** (`.bench/ck_errors.sh`): 19 of 19 checks correct, and the killed run's printed summary equals the saved statistics.
- **Signals during startup** (`.bench/ck_early.sh`, 60 trials killed 0–40 ms after start): 56 fresh runs checkpointed and finished EXACT. In 4, the resumed run was killed before `main` started (exit 143), leaving its checkpoint unchanged byte for byte, and then finished EXACT.
- **Cost** (OMEGA=33, S=29, 8 threads, alternating):

  | Checkpoint interval | Wall time (s) |
  |---|---|
  | none | 42.14, 43.14 |
  | 600 s (none taken) | 42.20, 43.49 |
  | 1 s (42–46 checkpoints) | 46.51, 43.21 |

  At the default interval, checkpointing is free. Each checkpoint pauses the workers for a few ms (a 31 KB file plus fsync).
- **Regression without checkpoints** (`.bench/final2.sh`): 18 of 18 EXACT (every mode, the debug build, S=29), and all exit codes correct. S=29 with 8 threads takes 42.2 s, unchanged.
