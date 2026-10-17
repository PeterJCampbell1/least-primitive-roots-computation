# Code review: fix-up list

Started 17/10/26. This review covers the uncommitted Montgomery integration on top of `ceb3d08`: `src/mont3.hpp` (new), and the changes to `src/primality.cpp`, `src/primality.hpp`, `src/process_support.hpp` and `src/stats.hpp`. These are potential fixes and changes for a later hand clean-up. **None of them is a correctness problem unless marked otherwise.** Items are added as the review proceeds.

## HIGH PRIORITY: correctness

- [ ] **The power tree is wrong for most OMEGA values (pre-existing, affects GMP and Montgomery alike).**
  - **Cause:** `_build_product_tree` (`primality.cpp`) splits each range in half, rounding up, but numbers nodes heap-style (children at `2i+1`, `2i+2`). `_generate_next_residue` decides "leaf" by `2*leaf_idx + 1 >= TREE_SIZE`, which is valid only for a complete heap.
  - **Scope:** a simulation of the layout shows it is correct only for some leaf counts. **OMEGA = 32, 33 and 34 are fine**, so all runs so far are valid. It is broken for OMEGA = 7, 11–15 and 19–31: real leaves are treated as internal nodes (stale or zero exponents are used), internal nodes as leaves, and indices run past `TREE_SIZE`, e.g. 12 against 11 at OMEGA=7 (still inside `MAX_TREE_SIZE`, so there's no crash).
  - **Confirmed:** `./lpr_g --omega 31` aborts with *"mont3 power tree leaf disagrees with GMP"*. **Release builds give no warning**, so g(p) could be wrong.
  - **Why:** heap numbering needs a *complete* binary tree (every level full except the last, which is filled from the left). Only then do the leaves occupy exactly the last n slots. Splitting in half gives a *balanced* tree, which is not the same thing: e.g. with n = 6 it splits 3/3, leaving a gap in the bottom level, whereas a complete tree splits 4/2.
  - **Preferred fix (one line):** keep the recursive builder and change only the split. For n leaves with h = ⌈log₂ n⌉, give the left subtree `min(n − 2^(h−2), 2^(h−1))` leaves (and 1 leaf when n = 2).
    - A simulation over OMEGA = 3…34 shows this gives a valid heap for every OMEGA. The traversal then visits every prime exactly once, in ascending order, so the early exit still tests small primes first and the debug check's leaf order is unchanged.
    - For 31 and 32 leaves (OMEGA = 32, 33) the split is the same as now (16/15, 16/16), so those trees, results and counts are identical.
    - The traversal (an in-order walk by heap-index arithmetic) needs no change. Its invariant, node value = g^(2d·Q_odd/prod(node)), does not depend on the shape.
  - **Alternative:** build bottom-up (primes in slots n−1 … 2n−2, products upwards). It is also valid, but for n not a power of two it scrambles the order the leaves are visited in, which loses the small-primes-first ordering. The debug check would then have to take the prime from `product_tree[leaf_idx]`.
  - **Safeguard:** validate the layout at startup for the configured OMEGA, and refuse to run if it is not a valid heap.
  - **Verify:** `lpr_g` (with its per-leaf GMP cross-check) passes for every OMEGA that has supports, and results at OMEGA = 32 and 33 are unchanged.

## `src/mont3.hpp`

### Header, types and helpers (lines 1–75)
- [ ] **Stale comment (fix):** line 3 still says *"Experimental (not part of the main build)"*. It should say that the file is used by the Fermat test and the power tree with GMP as the fallback, and is tested in `experimental/montgomery3`. My scripted edit silently missed it, because the original sentence was split across two lines.
- [ ] Style: the casts are C-style (`(u128)x`, `(u64)s`), while the rest of the codebase uses `static_cast`. Pick one.
- [ ] `N = 3` is a namespace constant, but every function is hand-unrolled for 3 limbs, so changing `N` would silently break the code. Either add `static_assert(N == 3, ...)` or switch to the templated version (see below).

### Montgomery product `mul` (lines 76–108)
- [ ] Add `static_assert(N == 3, "mul is hand-unrolled for 3 limbs")`.
- [ ] Add debug-only invariant checks: `assert(t3 == 0)` before the return, and optionally `assert(t4 == 0)` inside the loop. With $a, b < 2p$ the accumulator stays below $3p + 2p \cdot 2^{64} < 2^{256}$, and the result below $2p < 2^{192}$.
- [ ] Extend the comment on line 98 to say that the low half of `s` is discarded because only its carry matters (it is zero by the choice of `m`).

### Squaring, doubling, conversion, `setup` (lines 110–211)
- [ ] `from_mpz` silently truncates values longer than 3 limbs and ignores the sign; it is safe only because `setup` checks the size first. Add a debug `assert(mpz_sgn(z) >= 0 && mpz_size(z) <= N)`.
- [ ] `to_mont` requires $g < 2p$ but doesn't check it. That always holds in `lpr` ($g \le 1000$, $p > 2^{160}$); a debug `assert` would document it.
- [ ] `neg_inverse` duplicates `trial_division::inverse_mod_2_64` (the same Newton iteration). Use one shared helper, e.g. a small `bit_tricks.hpp`.
- [ ] The 3-limb shift left by one is written out three times (`dbl`, `dbl_canonical`, and the $2p$ computation in `setup`). Factor out a `shl1` helper, or replace it with the templated version.
- [ ] Style: the comment on line 114 is overlong.

### Exponentiation (lines 213–299)
- [ ] `pow2` squares 1 once for nothing (on the first pass). Starting from `dbl(one)` at the second-highest bit saves 1 of ~180 squarings (~0.5%). Optional.
- [ ] The `window_bits` thresholds (1/2/3/4 bits for ≤ 8/24/80/more) are untuned GMP-style defaults. The power-tree exponents are mostly 5–90 bits, so benchmark alternatives (e.g. k = 3 from 16 bits).
- [ ] `bit()` has no bounds check. Pass `n` in for a debug assert, or wrap the exponent in a small struct (limb pointer, limb count, bit length).
- [ ] The window value is built bit by bit (lines 286–288); a single shift and mask (handling a limb boundary) would do. Micro-optimisation only.
- [ ] Readability: rename `l`, `k` and `value` (e.g. `window_low`, `window_bits`, `window_value`).

### Templated alternative (background prototype, `experimental/montgomery3/mont_generic.hpp`)
- [ ] Consider replacing the hand-unrolled `mont3` with the templated `Num<N>`/`Context<N>` version. It passed all tests for $N = 3$ and $N = 4$, and matched `mont3` word for word in 1,134,000 comparisons.
  - It is 3–9% faster for powers and 5% faster in the least-primitive-root search, but 11–13% slower in the Fermat loop.
  - Recommended: the templated version everywhere, plus a hand-written 3-limb Fermat loop/`dbl`.
  - Don't use a branch-free `dbl` (always subtract, then select). It made the Fermat test 69% slower.
  - Details: `experimental/montgomery3/README.md`.

## `src/primality.cpp`

### Fermat test `is_composite_base2_fermat`
- [ ] Nothing checks that the Montgomery context belongs to this `p`; a context for a different modulus would silently give wrong results in release builds. Add a debug `assert(lpr::mont3::equal(mont->p, lpr::mont3::from_mpz(p.get_mpz_t())))`, or have the context own its `mpz` p so the two can't disagree.
- [ ] `is_composite_base2_miller_rabin` is still GMP-only and unused (its call is commented out in `process_support.hpp`). Delete it, or port it to `mont3`.
- [ ] API style: "`nullptr` context means GMP" is documented in the header; two overloads (with and without a `Context&`) would make the choice explicit at each call. Taste.

### Power tree traversal `_generate_next_residue` and `_build_product_tree`
- [ ] See HIGH PRIORITY above: change the split in `_build_product_tree` to the complete-tree split. The traversal itself (template with a `pow` callback) needs no change.

### `find_least_primitive_root`
- [ ] The debug cross-check (each leaf against GMP's g^((p−1)/q)) covers only the Montgomery path. Move it after the `if (mont)/else` so the GMP fallback is checked too.
- [ ] The debug check assumes the i-th leaf visited is the i-th odd prime. That is only guaranteed once the tree layout fix is in (with the complete split, leaves are visited in ascending order).
- [ ] Duplication: `if (mont)` at the root and at every leaf, plus two arrays and two callbacks. Consider templating the whole search on a small backend type (GMP or Montgomery), which removes the per-leaf branch and the duplicate code. Cleanliness rather than speed.
- [ ] `ReuseTracker` instrumentation still runs in release builds for every tested g (`avail()` recursion, `record()`). The conclusion is known (not worth it): remove it, or put it behind a compile-time flag.
- [ ] Pre-existing tidy-ups:
  - an unused `mpz_class exponent;`;
  - a stale commented-out perfect-power list;
  - `const unsigned& d` (pass by value instead);
  - `const mpz_class base = g` allocates per tested g (use a thread-local scratch value with `mpz_set_ui`);
  - the return type could be `std::optional<unsigned>` instead of `std::optional<mpz_class>`.

## `src/process_support.hpp`, `src/stats.hpp`, `src/primality.hpp`
- [ ] `static thread_local lpr::mont3::Context mont_context` is declared inside the per-candidate loop, which means a TLS lookup every iteration (`tlv_get_addr` showed up in the earlier profile). Move it to function scope next to the other thread-local scratch values (`p`, `n`, …).
- [ ] The stale commented-out `//if (is_composite_base2_miller_rabin(p, n)) {` is now a broken fragment. Delete it, or restore it properly along with the Miller–Rabin decision above.
- [ ] `primality.hpp` includes all of `mont3.hpp` just to name `Context*`; a forward declaration (`namespace lpr::mont3 { struct Context; }`) would do.
- [ ] `mont3_fallback_count` is printed in the "Least primitive root search" section, but it covers the Fermat test too. It would fit better in the main search summary.

## Other (from earlier review steps and profiling)
- [ ] `src/trial_division.hpp` line 98 uses `unsigned __int128` for the overflow check. A portable equivalent is `current.product > MAX_U64 / r`. (GCC warns about `__int128` under `-Wpedantic`.)
- [ ] The producer spends ~7% of its time in mutex locks (samply, $\omega = 33$, 8 threads), and it is now the bottleneck. Likely causes, in order:
  1. `publish_thread_local_stats()` after every batch (master stats mutex plus a full `LPRStats` merge, and `LocalStats{}` construction). Publish on a timer instead (e.g. once per second per thread).
  2. Queue mutex contention when consumers are woken.
  3. Per-job heap allocation of `LPRJob::support` (`std::vector`), freed on another thread. Use a fixed `std::array<unsigned, MAX_OMEGA>`.
- [ ] To measure the producer bottleneck exactly: add the consumer's wait time (timed only when the queue is empty) to the stats and the progress log.
