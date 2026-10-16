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
with $\omega$ set by `OMEGA` in `src/parameters.hpp` (currently 33). Every prime
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
./lpr --parallel               # multi-threaded, thread count auto-detected
./lpr --parallel --threads 16  # 1 producer + 15 consumers
./lpr --dry-run                # support enumeration only: nothing is verified
./lpr --help
```

Options:
- `--threads N`: total threads for `--parallel` (at least 2). Defaults to
  `SLURM_CPUS_PER_TASK` (for running on HPC clusters), then the hardware thread count.
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
| 3 | Interrupted by SIGTERM/SIGINT: partial run, nothing is verified |

On SIGTERM or SIGINT the program prints the statistics so far and the current
position of the search, then exits with code 3.

## Running on a cluster

`scripts/slurm/lpr.slurm` is a template for a single-node Slurm job. Fill in the
account, partition, CPU count and time limit, build `lpr` on the cluster, then
submit from the repository root:

```bash
sbatch scripts/slurm/lpr.slurm
```

The job runs `lpr --parallel` with the thread count taken from
`SLURM_CPUS_PER_TASK`, logs progress every 2 minutes to `lpr_<jobid>.log`, and
asks Slurm (`--signal=TERM@300`) to send SIGTERM 5 minutes before the time
limit so that the statistics so far are recorded. Runs cannot yet be resumed;
see [`docs/parallelise_producer.md`](docs/parallelise_producer.md) for the
planned checkpointing design.
