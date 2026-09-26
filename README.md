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
\omega(p-1)=33
```
for the remaining range below $1.8\times 10^{54}$.

The search uses the factorisation-specific form of Corollary 4 from Bagger's
paper and directly verifies the remaining candidates.

## Method

A detailed description of the computational method is available in
[`docs/method.md`](docs/method.md).

## Requirements
- C++17 compiler
- GMP and GMP C++ libraries

Tested under Ubuntu 24.04 in WSL.

On Ubuntu, GMP can be installed with:
```bash
sudo apt install libgmp-dev
```

## Build
Compile from the repository root with:

```bash
g++ -O3 -std=c++17 -Wall -Wextra -Wpedantic src/main.cpp src/primality.cpp src/corollary4.cpp -lgmpxx -lgmp -o lpr
```

## Run

Run with:
```bash
./lpr
```

For detailed timing and memory information:
```bash
/usr/bin/time -v ./lpr
```
