# Least Primitive Roots

C++ implementation of the computational procedure described in
Gustav Kjærbye Bagger's paper *A Note on Least Primitive Roots*.

The program follows the reduction used in the paper to verify Grosswald's
conjecture over the remaining finite range. The implementation is designed
to avoid a direct scan of the full interval: it constructs integers with the
required prime-factor structure, applies Corollary 4 before more expensive
tests, and progressively subjects only the surviving candidates to primality
and primitive-root computations.

## Current scope

This version verifies the case
```math
\omega(p-1)=\omega
```
with $\omega$ chosen at runtime (`--omega`, or a config file; default 33). Every prime
$p$ with $p-1$ above the global Corollary 4 bound $B$ is covered by Corollary 4
outright, so the run covers all such primes. $B$ is computed at runtime as the
Corollary 4 bound for the first $\omega$ primes ($B \approx 1.786\times 10^{54}$
for $\omega = 33$) and printed at the start of every run.

The search uses the factorisation-specific form of Corollary 4 from Bagger's
paper and directly verifies the remaining candidates.

## Method

A full description of the current computational method is in
[`docs/current_method.md`](docs/current_method.md). The original write-up is
[`docs/method.md`](docs/method.md), and
[`docs/optimisation_notes.md`](docs/optimisation_notes.md) is a dated log of
optimisations.

## Requirements
- C++17 compiler
- GMP and GMP C++ libraries

Tested on macOS (Apple clang, Homebrew GMP) and Ubuntu 24.04 in WSL.

On Ubuntu, GMP can be installed with:
```bash
sudo apt install libgmp-dev
```
On macOS: `brew install gmp`.

## Build
From the repository root:

```bash
make            # lpr (release) and lpr_g (debug/profiling, asserts on)
make release    # lpr only
make GMP_PREFIX=/path/to/gmp   # if GMP is not in the default search paths
```

The Makefile detects macOS (Homebrew GMP) or Linux. Without `make`:
```bash
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -pthread src/main.cpp src/primality.cpp src/corollary4.cpp -lgmpxx -lgmp -o lpr
```

## Run

```bash
./lpr                          # single-threaded (same as --single)
./lpr --parallel               # multi-threaded (work stealing), thread count auto-detected
./lpr --parallel --threads 16  # 16 workers, each searching and processing its own part of the tree
./lpr --parallel-queue --threads 16  # previous design, for comparison: 1 producer + 15 consumers
./lpr --dry-run [--threads N]  # support enumeration only: nothing is verified
./lpr --help
```

Search parameters (no rebuild needed):
- `--config PATH`: read `omega` and `s` from a file of `key = value` lines (`#`
  comments), e.g. [`config/omega33.conf`](config/omega33.conf).
- `--omega N`, `--s N`: set them on the command line (these override `--config`).
  `s` defaults to `omega - 3` and must satisfy `1 <= s <= omega - 3`.

The values used, and where each came from, are printed at the start of every run.
`omega` is at most 34. The primes used are generated at runtime up to
$\lfloor B / p_{\omega-1}\# \rfloor$, the largest prime that can occur in a support
(3396 for `omega = 33`, `s = 30`), so no prime limit needs to be set; if that value
exceeds `MAX_PRIME_LIMIT` in `src/parameters.hpp` the program stops with an error.
`PRIMITIVE_ROOT_SEARCH_LIMIT` remains a compile-time setting.

Options:
- `--threads N`: threads for `--parallel` (at least 1) or `--parallel-queue` (at least 2).
  Defaults to `SLURM_CPUS_PER_TASK` (for running on HPC clusters), then the hardware
  thread count. With `--dry-run` (default 1), N > 1 enumerates with work stealing.
- Testing: `--split-test N` (with `--single` or a 1-thread `--dry-run`) splits the
  search every N DFS steps, single-threaded; `--steal-stress` makes idle workers ask
  for work without backing off. See [`docs/work_stealing.md`](docs/work_stealing.md).
- `--log-interval SECONDS`: seconds between progress logs (default 120, 0 disables).
- `--log-file PATH`: append progress logs to `PATH` instead of stderr.

The final search summary is printed to stdout. Progress lines are timestamped
and flushed immediately. Warnings (a Grosswald inequality failure, or a
candidate whose least primitive root was not found within the search limit)
always go to stderr, and also to the log file if one is given.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Complete: every candidate certified and Grosswald's inequality holds |
| 1 | Complete, but a Grosswald failure or an unresolved candidate was found |
| 2 | Usage error |
| 3 | Interrupted by SIGTERM/SIGINT: partial run, nothing is verified, no checkpoint |
| 4 | Interrupted by SIGTERM/SIGINT after writing a checkpoint: continue with `--resume` |

On SIGTERM or SIGINT the program prints the statistics so far and the current
position of the search, then exits with code 3, or with code 4 after writing a
checkpoint if `--checkpoint` was given.

## Checkpoints

```bash
./lpr --parallel --checkpoint run.ckpt                      # every 600 s, on SIGTERM/SIGINT and at the end
./lpr --parallel --resume run.ckpt --checkpoint run.ckpt    # continue (any mode or thread count)
```

A resumed run's final summary is identical to an uninterrupted run's. `--resume`
refuses a checkpoint from a different search (omega, s, bound, settings, dry run)
or commit (`--resume-any-commit` accepts another commit). See
[`docs/checkpointing.md`](docs/checkpointing.md).

## Running on a cluster

`scripts/slurm/lpr.slurm` is a template for a single-node Slurm job. Fill in the
account, partition, CPU count and time limit, build `lpr` on the cluster, then
submit from the repository root:

```bash
sbatch scripts/slurm/lpr.slurm
```

The job runs `lpr --parallel` (work stealing) with the thread count taken from
`SLURM_CPUS_PER_TASK`, logs progress every 2 minutes to `lpr_<jobid>.log`, and
asks Slurm (`--signal=TERM@300`) to send SIGTERM 5 minutes before the time
limit. It checkpoints to `lpr.ckpt` and, when stopped by the time limit, requeues
itself and continues from the checkpoint until the search is complete (see
[`scripts/README.md`](scripts/README.md) and [`docs/checkpointing.md`](docs/checkpointing.md)).
