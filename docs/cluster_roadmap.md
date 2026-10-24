# Roadmap: getting onto a cluster with parallelised producers

Status: future plan, not implemented (written 16/10/26). More testing and other optimisations come first. The first section covers running on a cluster with the existing single producer; the stages after it cover parallel producers. The detailed design for stages 1–3 is in [`parallelise_producer.md`](parallelise_producer.md).

## Context
What exists now:
- one producer with co-processing, plus N−1 consumers (`--parallel --threads N`);
- live progress logging; SIGTERM/SIGINT snapshot (exit code 3);
- a portable Makefile and a generic Slurm template (`scripts/slurm/lpr.slurm`).

There is **no checkpoint/resume** and only **one producer**. The design for both is in `docs/parallelise_producer.md`. This roadmap orders the remaining work. Each stage is usable on its own, and each must leave the search summary identical to an uninterrupted single-threaded run.

## Single-producer cluster readiness (do this before the stages below)

Everything needed to run on a cluster **without** a parallel producer. Items are in priority order. Each must leave the search summary identical to the current results:
- $s = 30$: 1,343,862 supports, 114,064 certified, largest root 454;
- $s = 29$: 43,601,360 supports, 3,605,989 certified, largest root 502.

### 1. Linux build fixes (small)
- **`always_inline` without `inline`.** `dfs_iter_` (`src/generate_supports.hpp`) is declared `__attribute__((always_inline))` without `inline`, and GCC warns *"'always_inline' function might not be inlinable unless also declared 'inline' [-Wattributes]"*. Add `inline`. **Done (17/10/26 clean-up).**
- **LTO with GCC.** GCC prints *"lto-wrapper: warning: using serial compilation of 2 LTRANS jobs"* with plain `-flto`. **Done (20/10/26):** the Makefile uses `-flto=auto`, which GCC runs as parallel LTO jobs (no warning). Clang treats it as `-flto=full`, the same as plain `-flto`: Apple clang produced a byte-identical binary.
- **Thread count from the CPU affinity mask.** On a cluster node `std::thread::hardware_concurrency()` reports **every** core on the node, not the job's allocation. If `SLURM_CPUS_PER_TASK` is unset (e.g. `srun` without `--cpus-per-task`), `--parallel` would start far too many threads. On Linux, count the CPUs allowed by `sched_getaffinity`, which respects Slurm's cpuset/cgroup limits. Fall back to `hardware_concurrency()` elsewhere (macOS), and report the source in the run header ("CPU affinity mask").

### 2. GMP on the cluster (the largest performance lever)
- About 88% of runtime is `mpz_powm`, so how GMP was built matters more than any code change. A generic or module GMP may be noticeably slower than one built for the node's CPU.
- Build GMP 6.3 from source **on a compute node**: `./configure --enable-cxx --prefix=$HOME/opt/gmp && make && make check && make install`. The default configure selects assembly for the host CPU. `--enable-cxx` is required for `libgmpxx`, which some module builds lack.
- Then `make GMP_PREFIX=$HOME/opt/gmp`, and at runtime either set `LD_LIBRARY_PATH=$HOME/opt/gmp/lib` in the Slurm script or link GMP statically.
- Benchmark the module GMP against the self-built one on the same node with `--single` at $\omega = 33$.
- Add a `scripts/build_gmp.sh` helper and README notes.

### 2b. Compare compilers on small cases before long runs
On the M2, GCC 15.2 was about 4–7% faster than Apple clang 16 on consumer work: single-threaded CPU time 6.84–6.93 s against 7.10–7.27 s, and 8.9 s against 9.6 s with 8 threads. Both gave identical results. Clang was ~15% slower without LTO, while GCC was not. Apple clang on ARM says little about upstream clang on the cluster's x86-64 CPUs, so **measure on the cluster with small cases** before committing a long run to one toolchain:
- **Builds:** on one compute node, build with the default GCC module and with an LLVM/Clang module:
  - GCC: `make`;
  - Clang: `make CXX=clang++ LDFLAGS+=-fuse-ld=lld`. LTO with clang needs a linker that understands LLVM bitcode; without lld, LTO fails or is silently lost.
  - Use a GMP built with the same toolchain family. Clang on Linux uses GCC's libstdc++ by default, which matches a GCC-built `libgmpxx`; do not add `-stdlib=libc++`.
- **Small test cases** (seconds to minutes):
  - `--single` and `--parallel --threads 8` at OMEGA=33, S=30;
  - `--dry-run`;
  - OMEGA=33, S=29 with the job's thread count.
- **Check:** search summaries must be identical across compilers (and to the laptop's). Compare CPU time (`/usr/bin/time`, user) and wall time, alternating builds, 3 runs each.
- **Optionally:** `-march=native` (or `-march=znver3`/`znver4` for Bunya's Milan/Genoa nodes) with each compiler. It may matter more on x86 (MULX/ADX) than it did on the M2 (no gain). Build on, and run on, the same node type.
- **PGO:** also try profile-guided builds (`experimental/pgo/pgo_build.sh clang|gcc`). On the M2, clang + PGO was ~10% faster than plain clang (including on an input it was not trained on) and the fastest build overall, while GCC + PGO was ~8% *slower* than plain GCC (`docs/optimisation_notes.md`, 22/10/26). Verify the PGO binary's summary like any build.
- **Re-run** whenever the GMP build, compiler version or node type changes. The run header records the compiler and GMP version.

### 3. Size jobs to the single producer
- At $\omega = 33$ the producer alone (`--dry-run`) takes 1.14 s, against about 14.7 s of consumer CPU time, so one producer can keep only about **14 threads** busy. Co-processing does not raise this: once the producer is busy producing, nothing else generates supports.
- Measure the ratio at $\omega = 32$: compare `--dry-run --omega 32` with the `supports_processed` rate from a short `--parallel` run.
- Request `--cpus-per-task` up to that ceiling. Extra cores only add consumers that wait, which shows up as rising `consumer_waits` and a queue near 0 in the progress log.
- Optionally, log a warning when the queue stays empty, so an oversized job is obvious.

### 4. Checkpoint and resume (single producer; needed once a run exceeds the wall time)
$\omega = 32$ takes about 6 h on 8 threads, and even at the 14-thread ceiling that's several hours. This is Stage 1 below, with one region. The design is in [`parallelise_producer.md`](parallelise_producer.md):
- **Batch sequence numbers:** each batch gets a sequence number and its first support. Single mode also numbers its `BATCH_SIZE` groups.
- **Completion buffer and watermark:** consumers and the co-processing producer deposit the stats of each finished batch by sequence number. The watermark advances over consecutive finished batches, and only then are their stats committed. Merge the stats outside the lock.
- **`DfsStack` resume:** start from a given support by replaying the prefix, rebuilding each frame's `product`, `delta_Q`, `delta_sop` and `cor4_bound` with the same arithmetic as the main loop.
- **Checkpoint file:** `--checkpoint PATH`, written every log interval and on SIGTERM.
  - It holds a fingerprint (omega, s, prime limit, $B$, `PRIMITIVE_ROOT_SEARCH_LIMIT`, sieve constant, `PRIMORIAL_CHUNKS`, git commit), the committed stats (every `LPRStats` field, as text), and the resume support.
  - It is written atomically: temporary file, `fsync`, then `rename`.
- **`--resume PATH`:** refuse to continue on a fingerprint mismatch (exit 2), load the stats, and continue from the watermark support.
- **New exit code 4: "interrupted, checkpoint written".** It is distinct from 3, which means interrupted without a checkpoint.
- **Test:** interrupt $\omega = 33$ runs ($s = 30$ and $s = 29$) at random points, several times each, in single mode and with `--parallel --threads 2/8`. Resume until done, and require a summary identical to an uninterrupted run.

### 5. Slurm workflow
- **`scripts/slurm/lpr.slurm`:**
  - `--checkpoint lpr.ckpt`, plus `--resume lpr.ckpt` when that file exists;
  - `#SBATCH --requeue`, or a `submit_chain.sh` queueing N jobs with `--dependency=afterany`;
  - a small `--mem` (under 1 GB is plenty);
  - keep `--signal=TERM@300`;
  - the GMP library path from item 2.
- **A results directory per run** (`results/omega<N>_s<S>_<jobid>/`) holding stdout, stderr, the log, the checkpoint and a copy of the config.
- **Optionally, `--summary-json PATH`:** every stat plus the fingerprint, for archiving and for comparing runs.

### 6. Production-run checklist
1. Build GMP and `lpr` on a compute node.
2. Rerun $\omega = 33$ at $s = 30$ and $s = 29$ and compare with the numbers above.
3. Run the interrupt/resume test once on the cluster.
4. Size `--cpus-per-task` from item 3.
5. Submit $\omega = 32$.
6. A run counts as verified only if its **final** job exits 0 and every job in a resumed chain has a matching fingerprint.
7. Archive the results directory, as with `k32_run.txt`.

### Testing Linux locally (on the Mac)
- **Homebrew GCC (`g++-15`), done 16/10/26.** The code builds with GCC 15.2 under `-Wall -Wextra -Wpedantic -pthread`, in release (`-flto`) and debug (asserts on) builds. The only warnings are the two in item 1.
  - Homebrew's `libgmpxx` is built against libc++, so GCC (libstdc++) cannot link its `operator<<(std::ostream&, mpz_srcptr)`. The test build therefore used a small replacement for that one operator and linked only `libgmp`. This is a macOS packaging quirk and won't happen on Linux, where GMP and the program use the same C++ standard library.
  - Results are identical to the clang build: dry run 1,343,862 supports; single (16.1 s), `--parallel --threads 8` (2.83 s) and the debug build with 4 threads all give the same search and producer summaries.
- **A Linux container** (OrbStack, or `brew install colima docker`; not yet installed) can test:
  - the real GCC/glibc build with `libgmp-dev` and the Makefile's Linux branch;
  - the affinity-based thread count (`taskset -c 0-3 ./lpr --parallel` should report 4 threads);
  - signals, exit codes, log files and checkpoint/resume;
  - Slurm itself, through a single-node Slurm container image such as `slurm-docker-cluster`: `sbatch` with a deliberately short `--time`, `--signal`, `--requeue` and dependency chains.
  - `--platform linux/amd64` (Rosetta or QEMU) checks an x86-64 build, but is slow, so use it for correctness only.
- **Only the real cluster can show** performance and the GMP build (item 2), the producer ceiling at real core counts (item 3), which modules exist (whether GMP includes `libgmpxx`), scheduler policy (pre-emption, requeue, cpuset enforcement), and the shared filesystem.

## Stage 0: Commit and run a smoke test on the cluster (no new code)
1. Commit the current work: `git add` the `Makefile`, `src/cli.hpp`, `src/logging.hpp`, `scripts/`, the new docs and `CLAUDE.md`.
2. On the cluster:
   - load a compiler and GMP (`module load gcc gmp`), or build GMP locally and use `make GMP_PREFIX=...`;
   - compare compilers (and GMP builds) on the small cases of item 2b before choosing the toolchain for long runs;
   - fill in the `#SBATCH` placeholders;
   - run OMEGA=33 and check the summary matches the laptop run (1,343,862 supports, 114,064 certified, largest root 454).
3. **Measure scaling, to decide whether the parallel producer is needed:**
   - run `--dry-run` at OMEGA=32 to get the producer's time on its own;
   - run full jobs at `--threads 8/16/32/64`;
   - watch `consumer_waits` and queue length in the log. A queue that stays near empty, with waits growing, means the producer is the bottleneck.
   - If the producer keeps up at the core count you have, stages 2 and 3 can wait.
4. Find out the cluster's wall-time limit, cores per node, and whether jobs can be requeued or pre-empted. These decide how urgent stage 1 is.

> **Update 23/10/26:** parallelism is now work stealing (`--parallel`; `docs/work_stealing.md`), which replaces stages 2–3 below: every thread searches its own region, so there is no single producer to limit scaling. Checkpointing (stage 1) becomes simpler: a checkpoint is each worker's region and DFS position, with no completion buffer (`docs/work_stealing_plan.md`, phase 6). Item 3 ("size jobs to the single producer") applies only to `--parallel-queue`.

> **Update 24/10/26:** checkpoint/resume (stage 1 below, and items 4–5 of the earlier list) is implemented for `--single`, `--parallel` and `--dry-run`: `--checkpoint`, `--checkpoint-interval`, `--resume`, exit code 4, and a Slurm job that requeues itself after the time-limit signal. See `docs/checkpointing.md`.

## Stage 1: Checkpoint/resume with the single producer (the first code change)
Needed as soon as a run can exceed the wall-time limit. OMEGA=32 takes about 6 h on 8 threads.
- **Batch sequence numbers:** the producer numbers each batch and records its first support.
- **Completion buffer and watermark:** consumers (and the co-processing producer) hand each finished batch's stats to a buffer indexed by sequence number. The watermark advances over consecutive finished batches, and only then are their stats committed. Merge the stats outside the lock.
- **`DfsStack` changes:**
  - start from a given support, rebuilding each frame's product, window values and bound by replaying the prefix;
  - stop at an end boundary.
- **Checkpoint file:**
  - contents: a config fingerprint (OMEGA, S, the prime limit, search limit, sieve constant, B, commit), the committed stats including the mpz and array fields, and the list of regions (just one for now), each with an end boundary and a watermark support;
  - written atomically (temporary file, then rename) every log interval and on SIGTERM.
- **`--resume FILE`:** check the fingerprint, load the stats, and continue each region from its watermark.
- **Slurm:** either `--requeue`, or a chain of jobs (`--dependency=afterany`) where each one runs `--resume` on the previous checkpoint.
- **Testing:** interrupt OMEGA=33 runs at random points (repeatedly), resume, and require a final summary **identical** to an uninterrupted run. Also check that resuming with mismatched parameters is refused.

## Stage 2: Several producers with a static split
- Divide the top-level siblings, or the first few levels, into regions, with one producer per region.
- Each region has its own sequence numbers, completion buffer and watermark, and the checkpoint lists the unfinished regions. A slow region never holds back saved progress in the others.
- Start with one shared queue. If it becomes contended, switch to one queue per producer, with consumers stealing between queues.
- **Testing:** summaries identical across 1–8 producers, and resume works with several regions in flight.

## Stage 3: Work stealing for balance
- The pruned tree is heavily imbalanced, so a static split will leave producers idle.
- An idle producer takes the upper half of the untouched siblings at a busy producer's shallowest level that still has spare siblings. This needs a split operation on `DfsStack`, done under that producer's lock.
- New regions appear in the checkpoint automatically.
- Optionally, consumer co-generation: a consumer that finds the queue empty steals a region and produces from it, so the producer/consumer balance adjusts itself.
- At the end, check that the finished regions cover the whole search, which also serves as an exhaustiveness check.

## Stage 4: Tuning for large nodes
- Tune `BATCH_SIZE`/`HWM` for high core counts.
- Pin threads to cores.
- On multi-socket (NUMA) nodes, keep each producer's queue local to its socket.
- Profile lock contention at 64+ threads.

## Stage 5 (only if one node is not enough): several nodes
> **Update 24/10/26:** superseded by the detailed plan in [`multinode_plan.md`](multinode_plan.md): a shared task pool on the cluster filesystem, with donation when the pool runs low, a termination rule and a coverage-checked merge. It also keeps interleaved multiplication (with an SMT test) on the TODO list. The original sketch follows.

The simplest route is a Slurm job array **without MPI**:
- split the tree statically into disjoint regions (for example by first-level sibling ranges, using the stage 2 code), and give each array task one region;
- each task runs its own multi-threaded `lpr` with checkpointing;
- a merge step sums the per-task stats and checks that the regions cover the whole search.

MPI with stealing between nodes is only worth it if the array tasks turn out badly unbalanced.

## For every production run
- Keep the stdout summary, log, stderr and exit code (as with `k32_run.txt`).
- The run header already records the commit, compiler, GMP version and parameters.
- A run counts as verified only if it **completes** with exit code 0. A run assembled from resumed checkpoints counts only after the coverage check passes.

## Key files for this work
- `src/generate_supports.hpp` (`DfsStack`, the producers)
- `src/hysteresis_queue.hpp`
- `src/stats.hpp` (serialising stats; per-batch stats)
- `src/main.cpp` / `src/cli.hpp` (`--resume`)
- `scripts/slurm/`
- `docs/parallelise_producer.md` (the design)
