# Roadmap: getting onto a cluster with parallelised producers

Status: future plan, not implemented (written 16/10/26). More testing and other optimisations come first. The detailed design for stages 1–3 is in [`parallelise_producer.md`](parallelise_producer.md).

## Context
What exists now:
- one producer with co-processing, plus N−1 consumers (`--parallel --threads N`);
- live progress logging; SIGTERM/SIGINT snapshot (exit code 3);
- a portable Makefile and a generic Slurm template (`scripts/slurm/lpr.slurm`).

There is **no checkpoint/resume** and only **one producer**. The design for both is in `docs/parallelise_producer.md`. This roadmap orders the remaining work. Each stage is usable on its own, and each must leave the search summary identical to an uninterrupted single-threaded run.

## Stage 0: Commit and run a smoke test on the cluster (no new code)
1. Commit the current work: `git add` the `Makefile`, `src/cli.hpp`, `src/logging.hpp`, `scripts/`, the new docs and `CLAUDE.md`.
2. On the cluster:
   - load a compiler and GMP (`module load gcc gmp`), or build GMP locally and use `make GMP_PREFIX=...`;
   - fill in the `#SBATCH` placeholders;
   - run OMEGA=33 and check the summary matches the laptop run (1,343,862 supports, 114,064 certified, largest root 454).
3. **Measure scaling, to decide whether the parallel producer is needed:**
   - run `--dry-run` at OMEGA=32 to get the producer's time on its own;
   - run full jobs at `--threads 8/16/32/64`;
   - watch `consumer_waits` and queue length in the log. A queue that stays near empty, with waits growing, means the producer is the bottleneck.
   - If the producer keeps up at the core count you have, stages 2 and 3 can wait.
4. Find out the cluster's wall-time limit, cores per node, and whether jobs can be requeued or pre-empted. These decide how urgent stage 1 is.

## Stage 1: Checkpoint/resume with the single producer (the first code change)
Needed as soon as a run can exceed the wall-time limit. OMEGA=32 takes about 6 h on 8 threads.
- **Batch sequence numbers:** the producer numbers each batch and records its first support.
- **Completion buffer and watermark:** consumers (and the co-processing producer) hand each finished batch's stats to a buffer indexed by sequence number. The watermark advances over consecutive finished batches, and only then are their stats committed. Merge the stats outside the lock.
- **`DfsStack` changes:**
  - start from a given support, rebuilding each frame's product, window values and bound by replaying the prefix;
  - stop at an end boundary.
- **Checkpoint file:**
  - contents: a config fingerprint (OMEGA, S, PRIME_LIMIT, search limit, sieve constant, B, commit), the committed stats including the mpz and array fields, and the list of regions (just one for now), each with an end boundary and a watermark support;
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
