# Code review: fix-up list

Started 17/10/26. The review covered the Montgomery integration (committed as `c5bdad5`). **Status (17/10/26): all items resolved in the verified clean-up**, apart from the two deliberately deferred ones marked "Deferred". Each item says how it was resolved.

Every clean-up stage was verified against the previous stage's build, and the final build against `c5bdad5`. Identical results for:
- OMEGA = 33, S = 30: single-threaded, 2 and 8 threads, and debug with 8 threads;
- OMEGA = 33, S = 29 with 8 threads;
- the dry run.

Each stage was also timed against the previous one. See `docs/optimisation_notes.md` (17/10/26, clean-up) for the measurements.

## HIGH PRIORITY: correctness

- [x] **The power tree was wrong for most OMEGA values** (pre-existing; GMP and Montgomery alike). `_build_product_tree` split each range in half, but the heap-style storage and the traversal's leaf test `2i+1 >= TREE_SIZE` need a *complete* binary tree. It was wrong for OMEGA = 7, 11–15 and 19–31; OMEGA = 32–34 were fine, so all earlier runs stand.
  - **Resolved:**
    - the product tree uses the complete-tree split `complete_tree_left_size(n) = min(n − 2^(h−2), 2^(h−1))`;
    - `main` checks the layout at startup (`power_tree_layout_is_valid`, exit 2 otherwise);
    - debug builds assert that the leaves come in increasing order.
  - **Verified:**
    - OMEGA = 33 is identical, including the exponentiation counts (same tree);
    - the startup check passes for OMEGA = 3…34 and rejects the old split (test build);
    - a Python model confirms leaf counts 2…199;
    - OMEGA = 30 and 31 (previously broken; 31 aborted) now run with the per-leaf GMP cross-check and no assertion failures.
    - OMEGA ≤ 28 cannot run yet anyway: their prime limit exceeds `MAX_PRIME_LIMIT`.

## `src/mont3.hpp` (kept hand-unrolled for 3 limbs, as requested)

### Header, types and helpers
- [x] Stale "Experimental (not part of the main build)" comment. **Resolved:** the header now says where the file is used and where it is tested.
- [x] C-style casts. **Resolved:** `static_cast` throughout, plus `lo()`/`hi()` helpers for the halves of a 128-bit value.
- [x] `N = 3` could be changed silently. **Resolved:** `static_assert(N == 3, "mont3 is hand-unrolled for exactly 3 limbs")`.

### Montgomery product `mul`
- [x] **Resolved:** the `static_assert` above, plus the debug assertions `t4 == 0` (every round) and `t3 == 0` (result). They are exercised by the experimental tests, which are built with asserts, on inputs anywhere in [0, 2p), and by every `lpr_g` run.
- [x] **Resolved:** the line-98 comment now explains that only the carry of the lowest product is kept, and the overflow bound of each 128-bit sum is documented.

### Squaring, doubling, conversion, `setup`
- [x] `from_mpz`: **resolved** with a debug `assert(mpz_sgn(z) >= 0 && size <= N)`.
- [x] `to_mont`: **resolved** with a debug assert that g < 2p, written without overflow as `(g >> 1) < p` for one-limb p.
- [x] Duplicate Newton inverse. **Resolved:** `src/bit_tricks.hpp` (`lpr::bits::inverse_mod_2_64`, `constexpr`, with compile-time self-checks), used by both `mont3.hpp` and `trial_division.hpp`.
- [x] Shift written out three times. **Resolved:** a `shl1` helper.
- [x] Overlong comment. **Resolved.**

### Exponentiation
- [x] `pow2` squared 1 for nothing. **Resolved:** it starts from `dbl(one)` at the second-highest bit.
- [x] The `window_bits` thresholds were untuned. **Benchmarked** with `experimental/montgomery3/bench_window.cpp` on the full search (6 threshold sets, 2 runs of 3 repetitions). None was reliably faster than 8/24/80, so they were **kept**. The function is renamed `window_size`.
- [x] `bit()` had no bounds check. **Resolved:** `bit(e, n, i)` takes the limb count and asserts the bounds.
- [x] The window value was built bit by bit. **Resolved:** `bits_at(e, n, low, width)` reads it with one shift and mask, and handles a limb boundary.
- [x] Readability. **Resolved:** `window_low`, `width`, `window_value`, `odd_powers`, `base_squared`.

### Templated alternative
- [ ] **Deferred (by request):** `mont3` stays hand-unrolled for now. The templated `Num<N>` prototype stays in `experimental/montgomery3` (all tests pass for N = 3 and N = 4). Its notes: keep a hand-written 3-limb Fermat loop if adopting it; never use a branch-free `dbl`.

## `src/primality.cpp` / `src/primality.hpp`

### Structure (new)
- [x] Everything is now in `namespace lpr::primality` (the old "ADD A NAMESPACE" TODO). The leading-underscore global names (`_build_product_tree`, …), which are reserved identifiers in the global namespace, are gone. Internals are in an anonymous namespace in the `.cpp` file.
- [x] The perfect-power table moved from the header into the `.cpp` file (`IS_PERFECT_POWER`, `constexpr`).

### Fermat test
- [x] Context/p mismatch went unchecked. **Resolved:** a debug `assert(context_matches(*mont, p))` in the Fermat test and the search.
- [x] Unused `is_composite_base2_miller_rabin`. **Resolved:** removed, along with its commented-out call. It is still in the git history, and the optimisation notes record the earlier experiment.
- [ ] **Deferred (kept as is):** the "`nullptr` context means GMP" API style. It is documented in the header and keeps a single function per test.

### Power tree and search
- [x] Tree split: see HIGH PRIORITY.
- [x] The debug cross-check covered only the Montgomery path. **Resolved:** the search is templated on a backend (`GmpBackend`, `MontBackend`), and the per-leaf check (against GMP's g^((p−1)/q), plus the leaf-order assert) runs for both. Verified with a forced-fallback debug build: identical results with 1,112,602 GMP fallbacks.
- [x] Duplication and a per-leaf `if (mont)`. **Resolved** by the backend template: one `search<Backend>()`, with no per-leaf branch, and no slower (7.55 s against 7.56 s single-threaded).
- [x] `ReuseTracker` ran in release builds. **Resolved:** removed, with its stats and summary lines (the conclusion that reuse is not worth it is in the optimisation notes).
- [x] Pre-existing tidy-ups. **Resolved:**
  - the unused `exponent` and the commented-out perfect-power list are removed;
  - `d` is passed by value;
  - a thread-local scratch `base` with `mpz_set_ui`;
  - the function returns `std::optional<unsigned>`;
  - the primes are passed as a pointer and count (the job's inline array);
  - the traversal is restructured into one "go left to a leaf" loop shared by both cases (renamed `advance_to_next_leaf`).

## `src/process_support.hpp`, `src/stats.hpp`, the queue
- [x] Thread-local Montgomery context declared inside the loop. **Resolved:** moved to function scope with the other scratch values. `process_support` is flattened with early `continue`s, and the commented-out parameter block is removed.
- [x] Stale commented-out Miller–Rabin line. **Resolved:** removed.
- [x] `primality.hpp` included all of `mont3.hpp`. **Resolved:** it forward-declares `lpr::mont3::Context`.
- [x] `mont3_fallback_count` was printed in the wrong section. **Resolved:** it is now in the main search summary.
- [x] The stats "TODO - CHECK THIS CODE OVER!". **Resolved by a redesign:**
  - `Counters` is a plain aggregate (it copies correctly by default); `MasterStats` holds a mutex plus `Counters`, with `merge`/`snapshot`;
  - the `NullMutex` template, the custom copy constructor and assignment, and `copy_fields_from` are gone, so a new counter needs adding only to `merge` and `print`;
  - `largest_least_primitive_root` is a `uint64_t` instead of an `mpz_class`.
- [x] Legacy summary lines. **Resolved:** removed "Number of times proves grosswald was called", "Largest number of repeated calls to proves grosswald" and the reuse lines; removed the never-printed `surviving_legendre_count`. `prim_roots_count` became `winners_by_g` (64-bit) and is printed as a compact "Winners by g" histogram; its total equals "Least primitive root certified".
- [x] `HysteresisQueue` (a historical name). **Resolved:** renamed `JobQueue`, in `job_queue.hpp`.

## Other
- [x] `unsigned __int128` in `trial_division.hpp`'s overflow check. **Resolved:** `current.product > MAX_U64 / r`, which is exact for integers. The chunks are unchanged (verified).
- [x] The producer spent ~7% of its time in mutex locks. **Resolved:**
  1. statistics are published at most once per second per thread, staggered (`maybe_publish_thread_local_stats`, `STATS_PUBLISH_INTERVAL_MS`), instead of after every batch;
  2. jobs are fixed size with no heap allocation: the support is an inline array, and Q is stored as inline limbs (`MAX_Q_LIMBS`), read through `mpz_roinit_n`.
  - Result: the dry run is 3.6% faster and 8 threads are 4.5% faster, with identical results.
  - Queue-mutex contention when consumers are woken was not changed. It is the inherent single-producer limit, which the parallel producer would address.
- [x] Measure the producer bottleneck exactly. **Resolved:** `consumer_wait_ns`, timed only when the queue is empty, appears in the producer summary and the progress line. At OMEGA = 33 with 8 threads, consumers wait ~1.1 s in total in a ~1.4 s run, i.e. ~10% of consumer time.
- [x] Re-tune the trial-division range after Montgomery. **Benchmarked** MAX ∈ {233, 401, 613, 811, 1151} (MIN = 3). 1151 and 811 were tied single-threaded and both beat 613, so the range was **kept** at 3..1151. Survivors were identical for every range.
- [x] Stale files. **Resolved:** removed `src/analysis.hpp`, `.fermat_lab/`, `utils/__pycache__/` and `CONVERSATION_SUMMARY.md`, and added `__pycache__/` to `.gitignore`.
- [x] `experimental/montgomery3` kept its own copy of `mont3.hpp`. **Resolved:** it includes `src/mont3.hpp` through `mont3_src.hpp`, so it tests exactly the code `lpr` runs.
