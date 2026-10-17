# Current computational method

This note describes the method as currently implemented (October 2026). It supersedes the description of release `v1.0-k33` in [`method.md`](method.md), whose notation it follows. Dated details of each optimisation, with measurements, are in [`optimisation_notes.md`](optimisation_notes.md).

The program follows the finite-search strategy of Gustav Kjærbye Bagger's *A Note on Least Primitive Roots* (arXiv:2608.20431). For a fixed $\omega$ (chosen at runtime with `--omega` or a `--config` file; default 33) it verifies Grosswald's inequality
```math
(g(p)+2)^2 < p,
```
where $g(p)$ is the least primitive root modulo $p$, for every prime $p$ with $\omega(p-1)=\omega$ that Corollary 4 does not already cover.

All arithmetic that affects which candidates are checked is exact integer arithmetic (GMP). The computation is a proof by exhaustion: any step that skips work is justified below, because an error there would silently produce a wrong "verified" result rather than a crash.

## 1. Notation

Write the distinct prime divisors of $p-1$ as $q_1 < q_2 < \cdots < q_\omega$ (the *support*) and let
```math
Q = q_1 q_2 \cdots q_\omega = \mathrm{rad}(p-1), \qquad p - 1 = Qd,\quad \mathrm{rad}(d) \mid Q.
```
Since $p > 2$ is odd, $2 \mid p-1$, so $q_1 = 2$ in every support. Supports are stored as increasing lists of primes.

Fix $s$ (`S`, default $\omega - 3$). Corollary 4 uses the $s$ largest support primes:
```math
\delta = 1 - \sum_{i=\omega-s+1}^{\omega} \frac{1}{q_i}, \qquad
F = 2^{\omega-s}\,\frac{2\delta + s - 1}{\delta}.
```
Corollary 4 gives the sufficient condition
```math
c\,F^{16} < p \;\Longrightarrow\; p \text{ satisfies Grosswald's conjecture},
```
where the sieve constant is $c = \tfrac{4}{9}\pi^8 + \varepsilon \approx 4217.12$ and $\varepsilon > 0$ may be taken arbitrarily small. The hypotheses on $s$, on the even divisor $e$ and on the range of $p$ are as discussed in `method.md` §2. For $\omega = 33$, $s = 30$: the three excluded primes include 2, and $10^{47} < p_{33}\# + 1 \le p \le B + 1 < 10^{56}$ with $B$ as in §3.

**Important:** because $\delta$ uses only the $s$ largest primes, $F$ depends only on $q_{\omega-s+1}, \ldots, q_\omega$.

## 2. Corollary 4 in exact integer form

**Implementation:** `src/corollary4.cpp`, `corollary4_qd_bound`.

Let $Q_\delta = \prod_{i>\omega-s} q_i$ and let $\mathrm{SOP} = Q_\delta \sum_{i>\omega-s} 1/q_i$ be the sum of the products of all but one of those primes (the $(s-1)$-th elementary symmetric polynomial). Then $\delta = (Q_\delta - \mathrm{SOP})/Q_\delta$, and multiplying through by $Q_\delta$,
```math
X := c\,F^{16} = \frac{\mathrm{lhs}}{\mathrm{rhs}}, \qquad
\mathrm{lhs} = c \cdot \bigl(2^{\omega-s}\,((s+1)Q_\delta - 2\,\mathrm{SOP})\bigr)^{16}, \qquad
\mathrm{rhs} = (Q_\delta - \mathrm{SOP})^{16}.
```
The code computes lhs and rhs exactly.

**The constant.** The code uses $c = 4218 \ge \tfrac49\pi^8 = 4217.1249\ldots$. Since $\varepsilon$ may be arbitrarily small, any rational upper bound for $\tfrac49\pi^8$ is valid. Rounding up is the safe direction: a larger $c$ makes $X$ larger, so fewer $p$ are treated as covered. (A tighter 64-bit dyadic upper bound was benchmarked. It removed only 0.06% of candidates and was about 5% slower per call; see the optimisation notes.)

**What is returned.** Corollary 4 covers $p$ when $p > X$. With $p = Qd + 1$ and $Qd$ an integer,
```math
p \le X \iff Qd \le X - 1 \iff Qd \le \Bigl\lfloor \frac{\mathrm{lhs} - \mathrm{rhs}}{\mathrm{rhs}} \Bigr\rfloor =: \beta,
```
so `corollary4_qd_bound` returns $\beta$, the largest value of $Qd$ **not** covered by Corollary 4, or $0$ if $\mathrm{lhs} < \mathrm{rhs}$ (then $X < 1$ and every $p$ is covered). The floor loses nothing, because $Qd$ is an integer. It is the only rounding after the constant. A value $Qd = \lfloor X \rfloor$ when $X$ is an integer is conservatively treated as uncovered.

**Monotonicity (domination).** $F$, hence $X$ and $\beta$, is an increasing function of $\sum_{i>\omega-s} 1/q_i$. Say a support $(p_i)$ *dominates* $(q_i)$ if $p_i \le q_i$ for all $i$. If $(p_i)$ dominates $(q_i)$, then its reciprocal sum is at least that of $(q_i)$, so
```math
\beta(q) \le \beta(p) \quad\text{and}\quad \prod q_i \ge \prod p_i.
```
This is the fact behind all of the pruning in §4. It is a slightly stronger form of the domination argument in the "Local Delta" note in `optimisation_notes.md`: it bounds the value $\beta$, not only the yes/no outcome of Corollary 4.

## 3. The global bound

**Implementation:** `corollary4_global_bound`, called from `main`.

Every support is dominated by the first $\omega$ primes $(p_1, \ldots, p_\omega)$, so
```math
B := \beta(p_1, \ldots, p_\omega) \ge \beta(\text{any support}).
```
Every $p$ with $\omega(p-1) = \omega$ and $p - 1 > B$ is covered by Corollary 4 outright. $B$ is computed at runtime and printed at the start of each run. For $\omega = 33$, $s = 30$:
```math
B = 1786365837793136794888149451900656359669584286707872106 \approx 1.786366 \times 10^{54}.
```
(Release `v1.0-k33` used the hand-chosen $1.8 \times 10^{54}$, a decimal upper bound for this value.) Hence the run covers **every** prime $p$ with $\omega(p-1) = \omega$, not only those below a fixed bound.

**Prime-list completeness.** If $r$ is the largest prime of a support with $Q \le B$, then $p_{\omega-1}\#\, r \le Q \le B$, so $r \le \lfloor B / p_{\omega-1}\# \rfloor$. `main` therefore sieves all primes up to $\lfloor B / p_{\omega-1}\# \rfloor$ (3396, i.e. 478 primes, for $\omega = 33$, $s = 30$), after first sieving a small range to obtain the first $\omega$ primes needed for $B$. It then validates that the list starts with the first $\omega$ primes and covers that limit, and stops otherwise; the limit is capped by `MAX_PRIME_LIMIT`. Supports needing a prime beyond the end of the list are skipped by the DFS, which is safe because any such support has $Q > B$.

## 4. Enumerating supports

**Implementation:** `src/generate_supports.hpp`, `dfs_iter_` and `DfsStack`.

The supports are enumerated by an iterative depth-first search, in lexicographic order, over increasing $\omega$-tuples of primes from the generated list, with $q_1 = 2$ fixed. A node at depth $k$ is a prefix $(q_1, \ldots, q_{k+1})$, where the choice at depth $k$ is the prime index `next_prime_idx`. The stack (`DfsStack`, a fixed-size array of frames) supports three operations:
- `push` descends with the next prime index;
- `advance` moves to the next sibling;
- `backtrack` pops and advances the parent, returning false when the search is complete.

Each frame holds the incremental prefix product, and, from depth $\omega - s$ onward, the incremental $Q_\delta$ and SOP of the window. They are updated with $\mathrm{SOP} \leftarrow \mathrm{SOP}\cdot q + Q_\delta$ and then $Q_\delta \leftarrow Q_\delta \cdot q$.

**Minimal completion.** The minimal completion of a prefix extends it by the next consecutive primes. Every completion of the prefix is dominated by its minimal completion. Moving to a later sibling gives a larger minimal-completion product and a smaller $\beta$. So whenever a node can be pruned, all its later siblings can be too, and the search backtracks rather than advancing.

**Stored bounds.** Each node stores a bound $b$ on $Qd$ for every support below it:
- the root $\{2\}$ is seeded with $b = B$;
- a node shallower than $\omega - s$ (where the window is not yet complete) copies its parent's $b$;
- a deeper node stores $b = \min(b_{\text{parent}}, \beta(\text{its minimal completion}))$.

By domination, every support $L$ below a node satisfies $\beta(L) \le b$. The minimum is a safety net that keeps $b \le B$ for any parameter choice. For the current parameters every local $\beta$ is already at most $B$, so it never changes a value.

**Pruning.** At each node, with $Q_{\min}$ the minimal-completion product:
1. If $Q_{\min} > b_{\text{parent}}$, backtrack **without evaluating Corollary 4**. This covers the old global check $Q_{\min} > B$, since $b \le B$. It is sound because every support $L$ below has $Q_L d \ge Q_L \ge Q_{\min} > b_{\text{parent}} \ge \beta(L)$ for every $d \ge 1$.
2. Otherwise, at depth $\ge \omega - s$, evaluate $\beta$ for the minimal completion. If $Q_{\min} > \beta$, backtrack. This is the local Corollary 4 prune: by domination, Corollary 4 covers every $d$ for every completion.
3. Otherwise store $b = \min(b_{\text{parent}}, \beta)$ and descend (or emit, at a leaf).

Since the node's own $\beta \le b_{\text{parent}}$, anything removed by step 1 would also be removed by step 2. Step 1 only avoids evaluations.

**First-child reuse.** If a node's choice is the prime immediately after its parent's choice, its minimal completion is the same $\omega$-tuple as its parent's. So $Q_{\min}$, the window values and $\beta$ are all identical, and the parent has already passed every check. Such nodes copy the parent's $b$ and skip both the minimal-completion loop and Corollary 4. This requires the parent to have evaluated $\beta$ itself (parent depth $\ge \omega - s$).

**Leaves.** A leaf is a complete support with product $Q$ and bound $b$. It is emitted as a job $(\text{support}, Q, d_{\max})$ with
```math
d_{\max} = \lfloor b / Q \rfloor \ge 1,
```
which is exactly the largest $d$ for which $p = Qd + 1$ is not covered by Corollary 4. Emitted supports and their $d_{\max}$ values are identical to those of the earlier code, which evaluated Corollary 4 at every node and checked $B$ separately.

**Exhaustiveness.** Every $n = p - 1$ with $\omega(n) = \omega$ has a unique form $n = Qd$ with $Q = \mathrm{rad}(n)$. Its support is either emitted, or pruned because every $Qd$ under it lies above the relevant bound, i.e. Corollary 4 covers $p$.

## 5. Enumerating $p - 1$ for a fixed support

**Implementation:** `src/process_support.hpp`, `process_support`.

For $d = 1, \ldots, d_{\max}$, the value $d$ is kept only if all of its prime factors belong to the support ($\mathrm{rad}(d) \mid Q$). This is checked by dividing out every support prime and testing that 1 remains. Otherwise $\omega(Qd) > \omega$. For each kept $d$, $p = Qd + 1$. Every $d > d_{\max}$ is covered by Corollary 4 (§2), so no further $d$ need be considered.

## 6. Compositeness filters

**Implementation:** `src/primality.cpp`.

1. **Trial division:** for every prime $r$ in a fixed range (`TRIAL_DIVISION_MIN_PRIME`..`TRIAL_DIVISION_MAX_PRIME`; primes below 103 are usually in the support, where $p \equiv 1 \pmod r$), test whether $r \mid p$. The primes are grouped into chunks of consecutive primes whose product $m$ fits in 64 bits, and $\mathrm{rem} = p \bmod m$ is computed once per chunk; since $r \mid m$, $r \mid p \iff r \mid \mathrm{rem}$. Each prime is then tested with the Granlund–Montgomery check: with $r^{-1} \bmod 2^{64}$ and $\lfloor (2^{64}-1)/r \rfloor$ precomputed at startup (`src/trial_division.hpp`), $r \mid \mathrm{rem} \iff \mathrm{rem} \cdot r^{-1} \bmod 2^{64} \le \lfloor (2^{64}-1)/r \rfloor$. This holds because multiplication by $r^{-1}$ is a bijection modulo $2^{64}$ that sends the multiples $kr$ ($0 \le k \le \lfloor (2^{64}-1)/r \rfloor$) onto $\{0, \ldots, \lfloor (2^{64}-1)/r \rfloor\}$. If $r \mid p$ then, since $p > r$, $p$ is composite.
2. **Base-2 Fermat test:** if $2^{p-1} \not\equiv 1 \pmod p$, then $p$ is composite.

Both filters only ever discard composites, so no prime is lost. A survivor of both is a *Fermat survivor*; it is not proven prime by these tests.

## 7. The least primitive root

**Implementation:** `src/primality.cpp`, `find_least_primitive_root`.

For each Fermat survivor, $g = 2, 3, \ldots$ up to `PRIMITIVE_ROOT_SEARCH_LIMIT` (1000) is tested in increasing order. With $n = p - 1 = Qd$ and $p$ prime, $g$ is a primitive root if and only if $g^{n/\ell} \not\equiv 1 \pmod p$ for every prime $\ell \mid Q$.

**Perfect powers are skipped.** If $g = a^m$ with $m \ge 2$ is a primitive root, then so is $a < g$, since $\mathrm{ord}(a^m) \mid \mathrm{ord}(a)$. A perfect power is therefore never the *least* primitive root. A static table marks the 40 perfect powers up to 1000.

**$\ell = 2$ via the Jacobi symbol.** A primitive root is a quadratic non-residue. If $\left(\tfrac{g}{p}\right) = -1$ and $p$ is prime, Euler's criterion gives $g^{n/2} \equiv -1 \not\equiv 1$. So the program skips $g$ unless `mpz_jacobi(g, p) == -1`, which needs no exponentiation. This also correctly rejects every quadratic residue $g$, which cannot be a primitive root.

**Odd $\ell$ via a product tree.** The odd primes of the support are arranged, in increasing order, as the leaves of a *product tree*, in which each node stores the product of the primes below it. The tree is stored heap-style (the children of node $i$ at $2i+1$ and $2i+2$), so it must be a *complete* binary tree: every level full except the last, which is filled from the left. With $n$ leaves and $h = \lceil \log_2 n \rceil$, each node's left subtree therefore gets $\min(n - 2^{h-2}, 2^{h-1})$ of its leaves (`complete_tree_left_size`). Halving each range is not enough (for $n = 6$ it splits 3/3 instead of 4/2): that layout was wrong for most $\omega$ other than 32–34, and `main` now checks the layout at startup. The *power tree* starts from the root value $g^{2d} \bmod p$. A node's child takes the node's value to the power of the product held by its sibling. So leaf $\ell$ receives
```math
\bigl(g^{2d}\bigr)^{Q_{\text{odd}}/\ell} = g^{dQ/\ell} = g^{n/\ell},
```
using $Q = 2Q_{\text{odd}}$. The tree is evaluated lazily, leaf by leaf, by an in-order traversal that computes nodes on the way down. The node values (and the Fermat test's $2^{p-1}$) are computed with fixed 3-limb Montgomery arithmetic (`src/mont3.hpp`, $R = 2^{192}$, values kept in $[0, 2p)$, valid since $p < 2^{190}$), falling back to GMP otherwise; debug builds check every Fermat result and every leaf against GMP. The traversal stops at the first leaf equal to 1, at which point $g$ is rejected. A full traversal costs about $\log_2 \omega$ full-size exponentiations, instead of one per prime.

**What is certified.** Assume $p$ is prime. Every $g$ below $g(p)$ that is not a perfect power is either a residue (rejected by Jacobi) or a non-residue whose order misses some odd $\ell$ (rejected by the tree). $g(p)$ itself passes every test. Hence the first $g$ accepted is exactly $g(p)$. This is **conditional on primality**: unlike release `v1.0-k33`, which also tested $g^{n} \equiv 1$ and so certified primality, the Jacobi shortcut is only valid for prime $p$. This is enough for the verification: every prime $p$ receives its true $g(p)$. A composite Fermat pseudoprime may receive a meaningless value, which can only cause a spurious failure or unresolved report, never a missed prime. "Least primitive root certified" therefore counts Fermat survivors for which a value was found.

**Grosswald check.** Finally $(g + 2)^2 < p$ is checked. If no $g$ up to the limit is accepted, the candidate is reported as unresolved and must be examined by hand.

## 8. Success criterion

A run verifies the case only if it **completes** (exit code 0) with
- `Grosswald inequality failed: 0`, and
- `No least primitive root certified within search limit: 0`.

Otherwise the program exits with code 1, and prints each offending $p$ to stderr. An interrupted run (exit code 3) verifies nothing.

## 9. Parallel architecture

**Implementation:** `src/main.cpp`, `src/job_queue.hpp`, `src/generate_supports.hpp`, `src/process_support.hpp`.

- **Single mode** (`--single`, the default): the DFS calls `process_support` on each emitted support directly.
- **Parallel mode** (`--parallel --threads N`): one producer thread runs the DFS and groups jobs into batches of `BATCH_SIZE` (256). $N - 1$ consumer threads take batches from a bounded queue (at most `HWM` = 512 batches).
  - **Producer co-processing:** if the queue is full, the producer processes the batch itself instead of waiting, so no thread sleeps while there is work.
  - After the DFS, the producer marks the queue finished and helps drain it.
  - Every emitted batch is processed exactly once, by a consumer or by the producer, so the results are identical to single mode.
  - The thread count defaults to `SLURM_CPUS_PER_TASK`, then the hardware thread count.
- **Jobs:** each job (`LPRJob`) is fixed size, with the support and $Q$ stored inline, so producing and consuming jobs needs no heap allocation.
- **Statistics:** each thread counts into thread-local `Counters` and merges them into a mutex-guarded master copy at most once per second (staggered across threads), and finally when the thread exits. Consumers also record how long they wait on an empty queue (`consumer_wait_ns`), which measures how far the single producer limits the run.
- **Monitoring:** a monitor thread logs a progress line every `--log-interval` seconds (default 120) to `--log-file`, or to stderr. Each line gives the supports emitted and processed, the rate, candidates, Fermat survivors, certified, failed, unresolved, the largest $g(p)$, the queue length, co-processed batches, consumer waits, and the current DFS position.
- **Signals:** on SIGTERM or SIGINT the monitor prints the statistics so far and exits with code 3. Runs cannot yet be resumed; the planned design is in [`parallelise_producer.md`](parallelise_producer.md).
- **Dry run** (`--dry-run`): runs the DFS only and discards the supports. It measures the producer and checks the number of supports emitted, which must equal "K-prime supports examined" in a full run.

## 10. Current results ($\omega = 33$, $s = 30$)

| Quantity | Value |
|---|---|
| DFS nodes visited | 1,617,129 |
| Corollary 4 evaluations (first child reused / inherited-bound prunes) | 1,457,518 (136,631 / 22,978) |
| Supports emitted | 1,343,862 |
| Candidates $p = Qd + 1$ with $\mathrm{rad}(d) \mid Q$, $d \le d_{\max}$ | 1,593,113 |
| Composite by trial division (primes 3–1151) / by Fermat | 480,511 / 998,538 |
| Fermat survivors, least primitive root found | 114,064 |
| Grosswald failures / unresolved | 0 / 0 |
| Largest least primitive root | 454 |
| Jacobi calls / Legendre survivors tested, per prime | 9.4 / 4.5 |
| Power-tree exponentiations, per prime | 97.5 |

Winners by $p \bmod 8$ (all $d$ even / all $d$ odd): $p \equiv 1$: 1,280 / 0; $p \equiv 3$: 0 / 50,064; $p \equiv 5$: 12,432 / 0; $p \equiv 7$: 0 / 50,288. These match $p \equiv 1 \pmod 4 \iff d$ even, since $Q$ contains exactly one factor of 2.

Profile of a single-threaded run (macOS `sample`): least-primitive-root power tree about 45%, base-2 Fermat about 43%, trial division about 5%, Corollary 4 about 5%, Jacobi about 0.1%. (Profile taken before the trial division change of 16/10/26, which moved about 321,000 composites from Fermat to trial division.) Runtime on an Apple M2 laptop (4 performance + 4 efficiency cores) is about 7.6 s single-threaded and about 1.4 s with 8 threads, after the trial division (16/10/26), 3-limb Montgomery (17/10/26) and clean-up changes; with 8 threads the single producer is the bottleneck (consumers wait about 10% of the time). $\omega = 32$, $s = 29$ took 2 h 15 min on 8 threads (`k32_s29_run.txt`).

## 11. Code layout

- `src/main.cpp`: prime sieve up to the computed limit and its validation, global bound, run modes, threads, monitor and signal handling, final summary.
- `src/cli.hpp`: command-line parsing.
- `src/logging.hpp`: timestamped progress log and warnings.
- `src/config.hpp`: runtime $\omega$ and $s$ (`--config`, `--omega`, `--s`; $s$ defaults to $\omega - 3$).
- `src/parameters.hpp`: compile-time settings: `DEFAULT_OMEGA`, `MAX_OMEGA` (34), `MAX_PRIME_LIMIT`, `PRIMITIVE_ROOT_SEARCH_LIMIT`, the trial-division range, batch and queue sizes, `MAX_Q_LIMBS`, `STATS_PUBLISH_INTERVAL_MS`.
- `src/corollary4.{hpp,cpp}`: $\beta$ (`corollary4_qd_bound`) and $B$ (`corollary4_global_bound`).
- `src/generate_supports.hpp`: the DFS (`DfsStack`, `dfs_iter_`) and the single, dry-run and parallel producers.
- `src/process_support.hpp`: enumerating $d$ per support and the filter pipeline; consumer loop.
- `src/primality.{hpp,cpp}` (`lpr::primality`): trial division, Fermat, least-primitive-root search (templated on a GMP or Montgomery backend), product tree layout.
- `src/mont3.hpp`: 3-limb Montgomery arithmetic.
- `src/trial_division.hpp`: trial division tables (Granlund–Montgomery divisibility test).
- `src/bit_tricks.hpp`: the shared Newton inverse modulo $2^{64}$.
- `src/job_queue.hpp`: fixed-size jobs and the bounded batch queue (`try_push`/`pop`).
- `src/stats.hpp`: thread-local `Counters` and the shared `MasterStats`.

