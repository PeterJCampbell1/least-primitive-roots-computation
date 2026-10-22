# Optimisation Notes to method.md

See method.md for original method.

---

Date: **28/09/26**   
Author: **Mittun Sudhahar**  
Commit: f83d8b30413553fd5a34c0fd261f6a24459b6fc9  
OMEGA: 33  

This note describes initial optimisations/changes made to the method in `method.md`.

## 1. Using GMP Primitives

The function `make_corollary4_threshold` now uses lower level GMP operations such as `mpz_submul_ui`.
This allows GMP to make further optimisations.

## 2. Thread Local Variables 

`make_corollary4_threshold` uses thread local static variables to avoid overly reallocating memory for variables such as `num_F`. These are made thread local in preparation for parallelisation across cores. In particular, once a threshold is created by a thread, the function should not be called again until a new support is generated (as the same threshold is reused). This means in future we can create a (per thread) single location for `Corollary4Threshold` to avoid reallocation.

## 3. Removing Rational Arithmetic (rewriting corollary 4)

The largest improvement was implemented here. We first redefine $\delta$:

$$\delta := 1 - \sum_{i=1}^s p_i^{-1}$$
$$ Q_j := \prod_{i=1}^j p_i; \, Q_0 := 1; \, Q := Q_s $$
$$ S_j := \sum_{i=1}^j Q_j \mathbin{//} p_i; \, S_0 := 0; \, S := S_s $$ 
where $\mathbin{//}$ means exact integer division.

Note that $Q_j$ and $S_j$ are elementary symmetric polynomials in $j$ variables of degree $j$ and $j-1$ respectively. It follows then that,

$$ \delta = \frac{Q - S}{Q}$$
$$ Q_j = p_j \cdot Q_{j-1}$$
$$ S_j = Q_{j-1} + p_j \cdot S_{j-1}$$

We then can compute these variables during DFS (noting that we need to omit the first few primes in the support as $s < \omega$. When moving to an iterative implementation, these can be reformulated into utilising a pair of static length arrays. 

Next, 

$$ F := \frac{2\delta + s -1}{\delta} \cdot 2^{\omega - s} $$
$$ = 2^{\omega - s} \left( 2 + \frac{s - 1}{\delta}\right) $$
$$ = 2^{\omega - s} \left( 2 + \frac{(s - 1)Q}{Q-S}\right) $$
$$ = 2^{\omega - s} \frac{(2(Q - S) + \frac{(s - 1)Q})}{Q-S} $$
$$ = 2^{\omega - s} \frac((s+1)Q - 2S}{Q-S} $$

This is the computation implemented within `make_corollary4_threshold` with `Q, S` inputs and `\omega, s` constant parameters for the algorithm. Multiplying by $2^{\omega - s}$ is implemented as a bit shift.

## 4. Cleaning up w/ parameters/stats.hpp

Header files have been added to contain global parameters for the run type, as well as statistics. These will be propagated in future commits. 

## Results:

Initial timing on M2 (base chip) mac, single threaded took ~18:30 mins. Following the above optimisations, resulting runs took 8:34 mins representing ~2.16 times speed up over initial code. Profiling demonstrates 67% of runtime in `make_corollary4_threshold`, 5% in `corollary4_proves_grosswald` and 10% in `find_least_primitive_root`. 29/35% of runtime respectively is spent doing `gcd` and `pow` on multi-precision integers. 

## Future Notes:

1. In preparation for parallelising, we will convert to statically allocated (DFS) stack datum, and perform DFS iteratively.
2. Given the largest percentage of time is spent within `gcd`/`pow` we should attempt to optimise this as much as possible. It may be possible to weaken corollary 4 to avoid taking gcd/powers as much as possible - this will be tested and compared to the original version.
3. In general, it is preferrable to make the parameter `S` as large as possible to strengthen corollary 4.

---

Date: **28/09/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note describes optimisation results when modifying DFS to an iterative version from the original recursive implementation, as well as further optimisations.

## Iteration Details:

The iteration matches the recursive structure by performing a branch-pruned DFS. A `StackFrame` struct is used, with the relevant information tracked within this. At leaf nodes, a function `process_support` is implemented to separately handle verifying supports. A depth counter is used with statically allocated stack space for the support and DFS stack. This removes memory handling and function stack frame handling.

We have not yet utilised the fact that 2 is a known part of every support, for every OMEGA/S. We can utilise this to reduce the size of the bound (albeit we need to check that floor division is acceptable here), as well as to reduce the depth we need to search in DFS (this is also necessary for correctness, since this condition is implicitly used by the fact that 2 cannot be removed without exceeding bound for OMEGA = 33 - at OMEGA = 32 this may not be the case). 

## Thread Local Singleton

The `Corollary4Threshold` variable previously was returned by copy and created at each invokation of `make_corollary4_threshold`. This called constructor and destructors rather than reutilising memory allocated by GMP. Removing this and creating a unique variable (per thread) saved time (see below) and the function name was updated to `update_corollary4_threshold` as no thread needs to utilise multiple thresholds at any one time (the threshold only needs to change when the support is modified).

## Results:

Prior to this optimisation, a pass at OMEGA = 33, S = 30 took 8:34 mins on an M2 (base) chip. On the same device and parameters, after converting to an iterative implementation the runtime dropped to 5:25 mins. This represents a ~1.55x speedup between versions, and a ~3.415x speedup from the original method.

Further optimisations dropped runtime to 3:43 mins - a ~1.46x speedup from the iterative implementation and ~4.98x speedup from the original method.

###  Profiling:

Making `Corollary4Threshold` a unique pointer saved ~25s with that 3.6% of time. Removing computation of `gcd` entirely made the power slightly slower (7% slower) but made the overall computation around 1.35x faster. Current run took 3:43 mins with this optimisation.

Following optimisations, taking powers in `update_corollary4_threshold` is 51% of runtime and the function itself is 57% of runtime. 6.4% of runtime is spent in multiplication in `corollary4_proves_grosswald` and 20% is spent in modular exponentation in `find_least_primitive_root`.

---

Date: **30/09/21**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note regards the utilisation of legendre/jacobi symbols for primitve root filtering. Further, code has been further modified in preparation for parallelisation by adding thread local variables when possible for heap allocated mpz_class variables and statistics. Merge has been added for statistics as a utility tool for managing parallelism. Finally, it is now enforced that 2 is part of any support.

## Updated Support

We enforce that 2 is part of the initial support, and hence partially cut out some of the DFS iteration. This is also important for correctness in future, as a prime $p$ must be odd, and hence $p-1$ must be even.

## Legendre/Jacobi Symbols

The Legendre symbol $(g p)$ (note - non-standard notation) is defined as the quadratic residue of $g$ in *$\mathbb{Z}_p$* (in particular, this function $(\cdot\, p)$ is unique irreducible character of order 2). In particular, it is 0 if $\gcd(g, p) != 1$ and 1 if $g$ has a square root modulo $p$, otherwise it is -1. By Euler's criterion, $(g p) = g^{(p-1)/2} (mod p)$ and hence if $(g p) = 0, 1$ $g$ cannot be a primitive root. This is a stronger filter than testing $g^{p-1} == 1 (mod p)$ as we filter additional candidate primitive roots which have a square root. In addition, this means we do not need to test $g^{n/2}$ as we know this is -1.

When $p$ is not prime (as we cannot guarantee $p$ is prime in `find_least_primitive_root`) the Legendre symbol becomes the Jacobi symbol. CLAIM: This is sufficient for our purposes and will still filter soundly numbers that cannot be primitive roots (since no number is a primitive root) and all other numbers passing through the filter will be rejected regardless. The only claim we need to check, is to ensure that if $\gcd(g, p) != 1$ then the Jacobi filter will also reject this.

Note: Since we extend to integers where $\gcd(g, p) != 1$, we are extending to a Dirichlet character.

## Thread Local Storage (prep for parallelism/avoid heap allocation)

For the purpose of parallelism, statistics are accumulated at a thread local level with a merge routine provided for usage at the end of the run. Further, mpz_class objects will often require heap allocations as they grow. As a fixed number of 'scratch' multi-precision integers are required, and they do not grow to an unbounded level, we reuse these variables via static thread_local variables so that the automatic constructors/destructors are not called.

## Other Experimentation

Attempts have been made to take the logarithm of both sides of corollary 4. It is possible this could speed up the code, if clever tricks are used to extract an approximation of the logarithm using the top 53 bits of the integer representation, but this depends on how fast the base 2 logarithm can be taken. This would provide a small approximation error but may be sufficient in most cases.

Attempted: using mpfr_t to do the above, this was extremely slow and did not work.

Idea:
Consider: $z$ an integer with $B$ bits. We can write this as $z = m_0 * 2^{B-1}$ where $m_0 \in [1, 2)$. Then, let $k < B$. Then $z >> k = z * 2^{-k} = m_0 * 2^{B - k - 1} =: m$. This gives us $m$, a $B-k$ bit integer representing an approximation of the mantissa $m_0$. Bit shifting provides under approximations naturally, so this is an under approximation. $m+1$ is an over-approximation since $m << (B-k-1) < z < (m+1) << (B-k-1)$.

We then compute bounds as $\log_2 m + k < \log_2 z < \log_2 (m+1) + k$. This may or may not improve computation speed - we would need the value $B-k$ to be relatively small for this to have any possibility of being faster.

## Results:

After the above modifications, runtimes dropped to between 3:10 and 3:30 mins across runs, but statistical noise makes it difficult to infer how much improvement has been made. We expect these changes to become more significant in the OMEGA = 32 case as more values are likely to pass through the initial corollary 4 filter.

## Future notes:

At this stage, optimisations other than the log_2 change above are likely to be minor, or too small to verify. The only other optimisation currently being reviewed is the possibility of a smarter choise of S and $\delta$ within corollary 4 to maximise $S$ and minimise $\delta$ in a more dynamic manner. 

The next step will be to parallelise with a work-stealing mode via OpenMP that can be run on multiple cores, with the possibility of running OMEGA = 32.


---

Date: **03/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note details corrected applications of the Jacobi filter, and a **significant structural improvement** to pruning by applying a refined version of Corollary 4 for local pruning. The code at this commit has not been cleaned up, but initial results show correctness and major speed ups have been observed (the only remaining bottleneck being finding least primitive roots and filtering composites where this is unavoidable). 

## Trial GCD filter

The improvements here are inspired from the function mpz_probab_prime_p.

The GMP prime probabilistic test does the following. Firstly, it applies a series of gcd's with products of small primes, followed by the Miller-Rabin test, followed by the Lucas test. The Miller-Rabin test with base = 2 is a strictly stronger test than the Fermat test with only a minor performance penalty and hence may be worth implementing if many composites slip through (although up to now the Fermat test has worked well enough).

The Lucas test is also unecessary for our purposes, however, we have hard coded several products of primes which fit within 64 bits to provide an initial filter by running gcd(prod, p) > 1. This filter runs approximately 30x faster than the fermat test, and hence provided it filters at least 3% of inputs is worth using. Current empirical estimates show anywhere from 10-15% of inputs are filtered, and hence it is worth keeping. It is important to note, with smaller OMEGA the efficacy of this filter should increase, as fewer small primes are known to not be factor. This does incur some wasted work as we know a certain number of small primes cannot be a factor and hence do not need to be tested in the GCD, but it is fast enough to simply use that filter anyway.

## Correction to Jacobi Filter

The Jacobi symbol (g p) in the case where p is prime tells us that g cannot be a primitive root unless (g p) = -1, and that g^((p-1)/2) == -1 (mod p). Thus, in the case where p is prime if g passes the Jacobi filter we can skip computing g^(n/2). If p is composite, not detecting this and falsely claiming to have found a primitive is acceptable as it does not affect the correctness of Grosswald's conjecture (and if the composite slips through and is printed as an error we can simply attempt to verify whether it is composite via g^((p-1) / 2) after the verification has run).

## Local Delta bound creation and improved pruning during DFS

Corollary 4 provides a global bound which is used for pruning during DFS. However, this global bound can be improved at a cost at each prefix during the DFS, allowing for significantly greater pruning.

### Idea:

Let, $\omega > 11$, $s \leq \omega - 3$ ($s$ can be refined, but in general choosing $s = \omega-3$ is sufficient for all possible supports) and define some global bound $B \in \mathbb{N}$.
```math
P := \{ (p_i)_{i=1}^\omega = (p_1,\ldots,p_\omega) \mid p_i > p_{i-1}, \, p_i \text{ prime }, \, p_i \leq B \}
```
This is the set of possible prefixes of support which we wish to prune - we define an ordering on this set lexicographically. Given $\tilde{p}, \tilde{q} \in P$ we say $\tilde{p} := (p_1,\ldots,p_\omega)$ *dominates* $\tilde{q} := (q_1,\ldots,q_\omega$ if $p_i \leq q_i$ for all $i$. 

Next, define the following functions:
```math
W(x) := 2^{\omega-s}\cdot(2 + (s-1)/(1-x)); \, x \in (0, 1)
T(y) := (y/4220)^{1/16}; \, y > 0
```
Clearly $W, T$ are monotone increasing functions. Given $\tilde{p} \in P$, define also the following,
```
A(\tilde{p}) := W( \sum_{i=\omega-s+1}^\omega 1/p_i )
B(\tilde{p}) := T( \prod_{i=1}^\omega p_i )
```

We can then apply corollary 4, by saying for any prime $p$, where $\omega(p-1) = \omega$ and $\tilde{p} := (p_1,\ldots,p_\omega)$ where $p_i \mid p-1$, that if $A(\tilde{p}) < B(\tilde{p})$ then $p$ satisfies Grosswalds conjecture.

The main improvement then comes from the following fact. If $\tilde{p}, \tilde{q} \in P$ such that $\tilde{p}$ dominates $\tilde{q}$, then $A(\tilde{p}) < B(\tilde{p}$ implies that $A(\tilde{q}) < B(\tilde{q})$.

Proof:
We know $\tilde{p}$ dominates $\tilde{q}$, so $\prod_{i=1}^\omega p_i \leq \prod_{i=1}^\omega q_i$ and since $T$ is monotonic, it follows that $B(\tilde{p}) \leq B(\tilde{q})$. Similarly, $ \sum_{i=\omega-s+1}^\omega 1/p_i \geq  \sum_{i=\omega-s+1}^\omega 1/q_i$ and by monotonicity of $W$, $A(\tilde{p}) \geq A(\tilde{q})$. Thus $A(\tilde{q}) \leq A(\tilde{p}) < B(\tilde{p}) \leq B(\tilde{q})$ as required $\square$.

### Application:

Fix some prefix $q_1,\ldots,q_k$, $k \leq \omega$. Let $P' := \{(p_1,\ldots,p_\omega \in P \mid p_i = q_i \, \forall i \leq k\}$. Then there exists a minimum element *$\tilde{p}_{\min} \in P'$*, which we call the *minimum extension of $(q_1,\ldots,q_k)$*. By the above, it follows that if *$A(\tilde{p}_{\min}) < B(\tilde{p}_{\min})$ then $A(\tilde{p}) < B(\tilde{p})$* for every $\tilde{p} \in P'$. For any prime with support in $P'$, this property is sufficient to imply corollary 4, and hence implies that Grosswald's conjecture is true for every $\tilde{p} \in P'$. 

We can apply this, by constructing the minimum extension for each new prefix during DFS. Since we are constructing supports lexicographically, if the minimum extension *$\tilde{p}_{\min}$* of the current prefix satisfies *$A(\tilde{p}_{\min}) < B(\tilde{p}_{\min})$* then we can immediately conclude that the entire subtree from this prefix satisfies Grosswald's conjecture and can be pruned, leading to us returning to the previous depth and moving to the next prefix in lexicographic order.

This in practise, produces enormous speed ups as large sections of the DFS tree are pruned immediately since we have effectively found a local version of $\Delta_\omega$ that may be significantly smaller. 

## Results

A naive version of the local delta bounding idea has been applied, to great effect. Following this idea, the total number of supports surviving the DFS filters dropped from ~200 million at $s=30$, $\omega=33$ down to just ~1.3 million. The runtime dropped from 3:15-3:30 minutes down to 40-45s. The proportion of runtime spent in `update_corollary4_threshold` (the previous major bottleneck) dropped from ~70% to ~2% of runtime (effectively being entirely cut out). Trial gcd composite tests filtered 207908 out of 1345681 supports, and accounted for ~5.5% of runtime. The fermat composite check then accounted for 17% of runtime, with the `find_least_primitive_root` function now being the major bottleneck at 72% of runtime.

## Future Steps

The current code merely implements these ideas with no particular thought to optimisation. The code needs to be cleaned up, and it may be the case that some redundant work is now being done. In particular, corollary 4 is being run tice at each support, and can be entirely removed from the function `process_support` as it is implemented during the DFS stage. Further, the minimum global bound could also be updated to be the minimum bound discovered in prefixes of the current partial support, further leading to improvements.

Given that finding the least primitive root is now the main bottleneck, it makes sense to parallelise in a producer/consumer fashion, with a main producer thread applying the DFS, and any surviving supports can be passed to a concurrent task queue. Several consumer threads (pinned to cores) can then extract these tasks and perform the least primiitive root check. In fact, we can also have a two-tiered task queue system, where the first queue contains those potential primes/supports that need to be passed to the composite (gcd and Fermat) tests, and the second queue containing those remaining values for which we need to explicitly find least primitive roots. Threads may then extract batches from either queue depending on some tuned scaling factors and the lengths of each queue. 

---

Date: **05/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 32/33  

This note focuses on the idea of adapting the Pippenger algorithm specifically with the use of binary product trees to accelerate finding the least primitive root.

## Binary Product Trees:

We know that $p = Qd + 1$ where $Q := \prod_{i=1}^\omega q_i$ is the squarefree core of $Qd$. We wish to compute for each $i$, $g^(p-1/q_i) (\mod p)$ with early termination if this equals 1, and we also note that we can skip $q_i = 2$. The current algorithm computes this directly, and the modular exponentation within `find_least_primitive_root` is 71% of runtime.

Instead, we can exploit the structure of our problem as follows. Define,
```math
A := g^(p-1/Q) = g^d (\mod p)
Q_j := Q/q_j
```

Then it follows that,
```math
g^(p-1/q_i) = A^{Q_j} (\mod p)
```

We can then structure the computation of A^{Q_j} as follows (example with $\omega=4$):

Initial Product Tree:  
(Root) Node $q1q2q3q4$. Left child $q1q2$. Right child $q3q4$.  
Node $q1q2$. Left child $q1$. Right child $q2$.  
Node $q3q4$. Left child $q3$. Right child $q4$.  

We then associate to each node of this product tree a value by raising each child to the power of the other child to construct the following:

Power Tree:
(Root) Node $A$. Left child $A^{q3q4}$. Right child $A^{q1q4}$.  
Node $A^{q3q4}$. Left child $(A^{q3q4})^q2$. Right child $(A^{q3q4})^q1$.  
Node $A^{q1q2}$. Left child $(A^{q1q2})^q4$. Right child $(A^{q1q2})^q3$.  

This allows us to lazily compute each $g^(p-1/q_i)$ whilst reusing previous information to significantly reduce the amount of major modular exponentiations (each step into the tree becomes exponentially cheaper).

In the case where $\omega=2^k$ this tree forms a perfect binary tree. Even if not, the algorithm can be adapated to be computable via an array (rather than a linked tree structure) using the same techniques as would be done for a heap and pushing all larger products to the left (splitting by the ceiling and floor of half the remaining product).

Finally, we can account for the fact that we do not need to recompute $g^(p-1/2)$ (via the previous Jacobi filter) by seeding the root of the power tree with $A^2 = g^{2d}$ instead.

## Results:

Results are significant. The percentage of runtime from `find_least_primitive_root` has dropped from ~70% down to 42%, and now the Fermat test is 38% of runtime whilst the trial gcd is 13% of runtime and updating the corollary 4 threshold is now around 5.5% of runtime. The overall runtime dropped from ~40s to ~18s and whilst the code needs to be verified, initial computations agree with previous computations.

The potential parallelisation architecture now needs to be rethought.

## BUG:

Initial implementation of product tree had bugs. This has since been fixed and produces the same results as previously found.

## Note:
It would make life easier to convert this to C++20 so the use of generators is possible.

---

Date: **05/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note regards replacing the Fermat test with the Miller-Rabin test.

## Miller-Rabin Base 2

The Miller-Rabin test with base 2 is as follows. Let $p$ (odd) be some possible prime, $n = p-1$. Fermat's little theorem tells us that if $p$ is prime, then $2^{p-1} = 1 \pmod p$. Write $n = 2^a \cdot b$ for largest possible $a$. We then incrementally construct $2^n$ starting with $2^d$ and repeatedly squaring. As *$\mathbb{Z}_p$* is a field for prime $p$, we have the only solutions to $x^2 = 1 (\mod p)$ are 1 and $-1$. Thus, we create a sequence, $x_0,\ldots,x_s$, $x_0 := 2^b (\mod p)$ , $x_i := (x_{i-1})^2 (\mod p)$ (so $x_s = 2^n (\mod p)$). We construct the sequence lazily in order, with following early exits (based on *$\mathbb{Z}_p$* being a field and Fermat's little theorem). 

* If $x_0 = 1$, then we conclude $p$ is *probably* prime. 
* For each $x_i$, 
    * if $x_i = -1$, we return also that $p$ is *probably* prime. 
    * If $x_i = 1$, (but $x_{i=1} != -1$ - otherwise we would have exited already) then we return that $p$ is composite.

This enables earlier exiting than the base-2 Fermat test alone, with minimal additional overhead. In particular, if we never reached $x_i = -1$, then the Fermat test is unsatisfied meaning this is a strictly stronger test than Fermat with base 2. If the end of this loop is reached, we return that $p$ is definitely composite.

## Results

This made minimal to no difference, likely because the computation $2^a (\mod p)$ still dominates the majority of the computation as $d$ has at most only a few prime factors of $2$. This has been removed for now - but it is possible it makes more of a difference for smaller OMEGA, where a broader range of $d$ can be found.

---

Date: **06/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 32/33  

This note focuses on the first multi-threaded version of the code base (this is still a very rough version, but appears to work).

## General Parallelisation Architecture

A low/high water mark hysteresis queue structure is used. This is a concurrent queue with several producer/consumers. We utilise it with a single producer, and aim to have sufficient consumers to use all other physical cores whilst minimising thread contention.

The singular producer follows the pruned DFS (using corollary 4) to generate supports, and passes the resulting supports as jobs. Alongside this, it also passed the precomputed product of the support, and the number of valid $d$ such that $p = Qd + 1$ is not already verified. Thus, the `LPRJob` struct contains a small vector of primes 

TODO...
 
## Converting Update Corollary 4

For the purpose of parallelism, we do not wish to have significant amounts of data within the concurrent queue - and in particular, we do not wish to have the large values within `Corollary4Threshold`. As such, we immediately in the producer use `Corollary4Threshold` to obtain `d_max`, the maximum $d$ as stated above. This occurs via a division - although it has not yet been optimised or fully checked for correctness (although initial results match single-threaded cases).

TODO...

## New API:

TODO - Explain new api with command line args for doing both multi and single threaded work (this isn't cleaned up properly yet).

## Results:

Significant speed up - on my mac with 8 physical cores, using 7 consumer threads and 1 producer threads with batch size of 256 and HVM of 512, LVM of 128 brings time down to 3.3s on OMEGA=33. With 8 total threads and BATCH_SIZE of 256, HVM of 512 also only used ~40MB of memory at it's peak. Based on this, increasing HVM is very reasonable, and maximum batch size is probably around 80KB (we could record some statistics to check this if we wished - but it probably isn't necessary) and job size is probably around 315 bytes in size (as rough upper bounds). Memory can of course be optimised further (see below), and may be worth doing if completing this on more cores with multiple producers becoming necessary. In particular, it would be useful to see if any performance increases appear if we can keep an entire job within fewer cache lines by separating out data that is common across all supports (or batched supports).
TODO...

## Future Notes:

If we wish, we could precompute the longest unique prefix and store this separately as a global read only variable to save space (or just pass a copy to each thread tbh). Significant clean up of the source code itself should be done now to avoid technical debt and make this presentable, as it is probably close to the point where running other values of OMEGA is reasonable. Still need to decide how to work with this if we adapt to a situtation where there can be many more consumer threads (e.g., on a HPC cluster) and then the producer is outpaced by the number of consumers (just need a way to split the work in a hybrid DFS/BFS manner). Still also need to clean up how command line arguments are handled and how namespaces are organised to make coding easier. It is also possible to implement a strategy where rather than falling asleep, the producer does Producer Co-Processing where it handles it's own batch if the push to queue fails (due to HVM being hit) and that way you don't need to have any thread go to sleep at all in the event the producer is too fast for the consumers. We can also have consumer co-generation once multiple producers are implemented (this is where a consumer turns into a producer when LVM is hit or (a probably better choice) if queue.pop fails - would need to change behaviour here a bit) This allows us to have more flexibility with the number of consumers/producers in such a way that they self balance with minimal overhead and little to no context switching. Should also look into thread pinning to avoid threads jumping cores. Further, this is still only ready for a single server - switching to a HPC cluster would require more advanced code likely wiht an OpenMP wrapper to handle NUMA nodes. Should also implement work stealing if we want multiple producers (where we steal the oldest modifiable stack frame - where stack frame prefixes form jobs for the producers). Perhaps implement a Chase-Lev Work Stealing Deque, with exponential backoff for termination condition (or distributed Dijkstra ring token for multi-node clusters). Also add logging and binary file state saving (necessary for later use on Slurm on HPC cluster). This needs to be buffered to occur on some master thread periodically (say every few minutes) so that we can track states. We also need to be able to use lexicographic ordering to maintain what is the latest support for which all earlier jobs have been definitely completed (i.e., such that we can restart and save state with minimal overhead). Also remove/update/store global bound within stack frames - the global bound isn't really necessary.
TODO...

## Idea Used: 

Peter mentioned if a is a perfect power a = b^k and b generated a proper subgroup, then so does a (easy proof). Can use this to cut out several numbers like 8 = 2^3, 32 = 2^5, 27 = 3^3 etc., and this saved 7% of runtime. 

Separate idea, is to maintain availability sets with products of factors, and use the multiplicativity of characters to fill in tables of the availability sets (seeded with things that escaped Legendre) and then determining exactly which products could generate the number before replacing some exponentations with multiplications.


---

Date: **15/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note covers a clean up of the producer (`generate_supports.hpp`), a dry-run mode for measuring the producer on its own, and two reductions in the number of Corollary 4 evaluations. Emitted supports and their `d_max` values are unchanged, and the search summary matches the previous run exactly.

## Dry Run and Command Line Modes

`./lpr` now takes `--single` (default), `--parallel` (still the hardcoded 8 thread logic) or `--dry-run`. A dry run performs the full DFS but discards each emitted support, then prints a producer summary (DFS nodes visited, Corollary 4 evaluations/reuses/inherited prunes, supports emitted, sum and maximum of `d_max`) and the wall time. The sum of `d_max` gives an upper bound on the number of $d$ the consumers loop over. Producer statistics are also printed after normal runs. "Supports emitted" in a dry run must equal "K-prime supports examined" in a full run.

At $\omega = 33$ the producer takes ~1.2s of a ~16s single-threaded run, so the producer is not currently the bottleneck on a single machine (it matters once many consumers are available).

## DfsStack

The DFS stack is now a small class (`DfsStack`) holding a fixed-size `std::array` of frames with `push` (descend), `advance` (next sibling) and `backtrack` (pop and advance, returning false when the traversal is complete), removing three duplicated backtracking blocks. Everything is force-inlined, and dry-run timings are unchanged (1.19s before and after). The bounds check on `next_prime_idx` now happens before `support[depth]` is written, fixing an out-of-bounds read of `primes`.

## Inherited Corollary 4 Bound

`create_corollary4_threshold` has been replaced with `corollary4_qd_bound`, which returns the largest $Qd$ not covered by Corollary 4, $\lfloor (\text{lhs} - \text{rhs}) / \text{rhs} \rfloor$, rather than $d_{\max}$. At a leaf, $d_{\max} = \lfloor \text{bound} / Q \rfloor$, which equals the previous value since $\lfloor \lfloor a/b \rfloor / Q \rfloor = \lfloor a / (Qb) \rfloor$. The bound depends only on the last $s$ primes and decreases under domination (by the argument in the local delta note), and every completion of a child is a completion of its parent. So each frame stores its node's bound, and a child whose minimal completion product already exceeds its parent's bound is pruned (backtracking, as the minimal completion only grows across siblings) without evaluating Corollary 4. The global bound is kept as a separate check, so leaf `d_max` values are not capped by it and consumer counts are unchanged.

## First Child Reuse

If a node chooses the prime immediately after its parent's choice, its minimal completion is the same tuple as its parent's minimal completion, so its product, deltas and Corollary 4 bound are identical and the parent has already passed every check. These nodes skip both the minimal completion loop and Corollary 4, and copy the parent's bound.

## Results

At $\omega = 33$: DFS nodes visited 1617129, Corollary 4 evaluations dropped from 1617125 to 1457518 (136631 reused by first children, 22976 inherited bound prunes). Dry-run time dropped from ~1.19s to ~1.14s. The full search summary (single, parallel and debug builds) is identical to the previous run.

## Computed Global Bound

The search bound $B$ is no longer a parameter (`BOUND_MULTIPLIER`/`BOUND_EXPONENT` have been removed). Every support is dominated by the first $\omega$ primes, so `corollary4_global_bound` computes $B$ as the Corollary 4 bound on $Qd$ for that support, $\lfloor X_{root} - 1 \rfloor \approx 1.786366 \times 10^{54}$ at $\omega = 33$, $s = 30$ (previously the hand-chosen $1.8 \times 10^{54}$). Every $p$ with $p - 1 > B$ is covered by Corollary 4, so the computation now covers every $p$ with $\omega(p-1) = \omega$. $B$ is printed in the configuration and used for the `PRIME_LIMIT` check.

$B$ seeds the root frame's bound, so the separate global check in the DFS is gone: nodes shallower than $\omega - s$ copy their parent's bound, and deeper nodes store $\min(\text{parent's bound}, \text{own Cor 4 bound})$ (the minimum keeps every bound $\leq B$ regardless of parameters; at the current parameters it never changes a value, since every local bound is already $\leq B$). The floor in each bound loses nothing since $Qd$ is an integer, and is monotone, so the floored global bound is still at least every floored local bound. Emitted supports and the search summary are unchanged; the two nodes previously pruned by the global check are now counted as inherited bound prunes (22978).

## Sieve Constant: 4218 vs Upper Dyadic Approximation

Benchmarked (not adopted) replacing `mpz_mul_ui(lhs, 4218)` with $M / 2^{64}$, $M = \lceil \tfrac{4}{9}\pi^8 \cdot 2^{64} \rceil$ (a 77-bit, 2 limb constant), either rounding $\lceil lhs \cdot M / 2^{64} \rceil$ or multiplying $rhs$ by $2^{64}$ instead (both sound). Both variants are ~5.5% slower per Corollary 4 call (~790 vs ~835 ns in a microbenchmark) and ~5% slower in a dry run (~1.15s vs ~1.21s), despite emitting 758 fewer supports (1343104 vs 1343862; certified 113994 vs 114064). Full single (~15.8s) and parallel (~2.8s) runtimes were indistinguishable within noise, since the 0.06% fewer candidates do not offset the extra cost. Not worth adopting at $\omega = 33$.

## Profile and Least Primitive Root Instrumentation

Profiled a single-threaded $\omega = 33$ run with macOS `sample` (release build with symbols, ~10,300 samples): `find_least_primitive_root` ~45% (almost entirely `mpz_powm` in the power tree), base-2 Fermat ~43%, small prime trial division ~5%, `corollary4_qd_bound` ~5%, `mpz_jacobi` ~0.1%.

New counters in the search summary ("Least primitive root search"): 9.4 Jacobi calls and 4.5 Legendre survivors tested per prime (the last being $g(p)$), ~97 power tree `mpz_powm` calls per prime (11,118,678 total), and winners by $p \bmod 8$ and parity of $d$ ($p \equiv 1 \bmod 8$: 1280, all $d$ even; $3$: 50064, all $d$ odd; $5$: 12432, all $d$ even; $7$: 50288, all $d$ odd).

## Reuse of Power Tree Nodes Across Candidates (measured, not adopted)

Idea: within one $p$, a node of the power tree for $g = ab$ is the product of the same node for $a$ and $b$, so nodes already computed for earlier candidates could replace `mpz_powm` calls. The lazy traversal computes the paths to leaves $0..k-1$, so the stored nodes of a candidate are described by $k$ alone, and availability is $\text{avail}(y) = \max(\text{own}(y), \max_{ab = y} \min(\text{avail}(a), \text{avail}(b)))$. Counting only (no change to the computation), the upper bound on avoidable power tree `mpz_powm` calls is 173,748 of 11,118,678 (1.6%), from 32,138 of 510,955 tested candidates, i.e. at most ~0.7% of total runtime. The main limitation: quadratic residues are never tested so never have nodes, and most non-residue composites contain one (e.g. $5 \mid Q$ is always a residue, and 6 is a non-residue only when exactly one of 2, 3 is). Not worth implementing.

## Namespace Fix

`LPRStats`/`NullMutex` (`stats.hpp`) and `StackFrame`/`DfsStack`/`dfs_iter_` (`generate_supports.hpp`) were in anonymous namespaces in headers but used by `inline` functions with external linkage. `stats.hpp` is included from both `main.cpp` and `primality.cpp`, so each translation unit had its own `LPRStats` type behind the same accessors (an ODR violation that only worked as the layouts matched). They are now in `lpr::stats::detail` and `lpr::producer::detail`.

## Future Notes

Incrementally updating the minimal completion across siblings (it changes by exactly one prime, so $Q$, $\delta_Q$ and $\delta_{SOP}$ can be updated with an exact division and multiplication from the previous set of values if those are stored) was not done: the minimal completion loop is short near the leaves where most nodes are. Worth revisiting only if profiling shows it matters, e.g. once the producer is parallelised and becomes the bottleneck.

---

Date: **15/10/26** (second entry)   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

This note covers producer co-processing, live progress logging, signal handling, a new command line interface and cluster readiness. Search results are unchanged in every mode.

## Producer Co-Processing

The queue (`hysteresis_queue.hpp`) is now a bounded queue with a non-blocking `try_push`. When the queue holds `HWM` batches, the producer processes the batch itself instead of sleeping, then continues the DFS. After the DFS it marks the queue finished and becomes a consumer to help drain it. The low water mark (`LWM`) and the producer sleep/wake path are gone. New producer statistics: batches pushed, batches co-processed, and consumer waits (a consumer found the queue empty, i.e. the producer is behind). With 2 threads, 2254 of 5250 batches were co-processed (one consumer cannot keep up); with 8 threads, 175.

## Live Statistics and Logging

Thread-local statistics are now published into the master statistics after every batch (`publish_thread_local_stats`, a merge and reset that costs a few µs per ~3 ms batch), so they can be read during the run. A monitor thread logs a single `progress:` line every `--log-interval` seconds (default 120) to `--log-file` or stderr. The line gives supports emitted/processed and rate, candidates, Fermat survivors, certified, failed, unresolved, largest $g(p)$, queue length, co-processed batches, consumer waits, and the DFS position (e.g. `2..101,109,113,127,...`, the leading run of consecutive primes abbreviated). Grosswald failures and unresolved candidates are reported through `log::warn`, which always writes to stderr (and to the log file if set) instead of stdout.

## Signals and Exit Codes

On SIGTERM/SIGINT the monitor prints the statistics so far and the DFS position, marks the run as partial, and exits with code 3. Exit codes: 0 complete, 1 failure/unresolved, 2 usage error, 3 interrupted. There is no resume yet; the design for checkpointing together with a parallel producer is in `docs/parallelise_producer.md`.

## Command Line Interface

`--single` (default), `--parallel`, `--dry-run`, `--threads N` (total threads for `--parallel`, at least 2; defaults to `SLURM_CPUS_PER_TASK`, then the hardware thread count), `--log-interval SECONDS`, `--log-file PATH`, `--help`. The run header prints the mode, thread count and its source, log target, host, start time, and the build (git commit, compiler, GMP version).

## Cluster Readiness

The `Makefile` is now tracked and portable (macOS/Homebrew or Linux with `-pthread`; `GMP_PREFIX` for non-standard GMP installs; unused OpenMP flags removed; git commit compiled in). `scripts/slurm/lpr.slurm` is a generic single-node Slurm template using `--signal=TERM@300`. `docs/current_method.md` is a full write-up of the current method.

---

Date: **16/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

## Runtime OMEGA and S

`OMEGA` and `S` are no longer `constexpr`. They are set at runtime (`src/config.hpp`) from `--config FILE` (`omega = N`, `s = N` lines, e.g. `config/omega33.conf`), overridden by `--omega N` / `--s N`; `omega` defaults to `DEFAULT_OMEGA` (33) and `s` to `omega - 3`, validated as $3 \le \omega \le$ `MAX_OMEGA` (64) and $1 \le s \le \omega - 3$. Per-depth arrays (`DfsStack` frames, product/power trees) keep fixed sizes from `MAX_OMEGA`/`MAX_TREE_SIZE`, so there are no extra allocations. `PRIME_LIMIT` and `PRIMITIVE_ROOT_SEARCH_LIMIT` stay compile-time. A `PRIME_LIMIT` that is too small for the chosen parameters is now a clean error (exit 2, giving the required value) instead of an uncaught exception.

Correctness: the runtime build matches compile-time builds exactly (search and producer summaries) at $\omega = 33$ with $s = 30$ (default) and $s = 29$ (via a config file: 43,601,360 supports, 3,605,989 certified, largest least primitive root 502). $\omega = 34$ has no supports (Corollary 4 covers everything), and $s = 27$ needs primes up to $8{,}234{,}997{,}866$.

Benchmark (alternating builds, load < 3 at start; medians): dry run 1.141s compile-time vs 1.139s runtime (5 runs each), single-threaded 15.78s vs 15.87s, 8 threads 2.95s vs 2.75s (3 runs each). All differences are within run-to-run noise: as expected, since ~95% of the runtime is in GMP, making these parameters runtime values costs nothing measurable.

## Dynamic Prime Limit and MAX_OMEGA = 34

`PRIME_LIMIT` is gone. `main` first sieves a small range to get the first $\omega$ primes, computes $B$, and then sieves every prime up to $\lfloor B / p_{\omega-1}\# \rfloor$, the largest prime that can occur in a support with $Q \le B$ (3396, i.e. 478 primes, at $\omega = 33$, $s = 30$; 5717 at $s = 29$), instead of a fixed $10^7$ (664,579 primes). The validation checks remain: the list must start with the first $\omega$ primes and cover the computed limit, which is capped by `MAX_PRIME_LIMIT` ($4 \times 10^9$; primes are stored as `unsigned`, and the sieve now uses 64-bit indices). The DFS treats running off the end of the list as exhausting the branch, which is safe because any support with a prime beyond the limit has $Q > B$; with the tight limit this boundary is now actually reached, and results match the $10^7$ limit exactly (search and producer summaries) for $s = 30$ (single and 8 threads) and $s = 29$ (43,601,360 supports). Runtime is unchanged (15.8s single, ~3.0s with 8 threads, ~94s for $s = 29$).

`MAX_OMEGA` is now 34 (from the mathematics); $\omega = 34$ runs and emits no supports ($\lfloor B / p_{33}\# \rfloor = 87 < p_{34} = 139$).

## Granlund–Montgomery Trial Division

Trial division used `rem = p mod m` for three 64-bit products $m$ of the primes 103–233, then `std::gcd(rem, m) > 1`. The gcd dominated: on 64-bit values Euclid's algorithm takes about 37 dependent hardware divisions (each step needs the previous remainder, so the CPU cannot overlap them), measured at ~140 ns per chunk, i.e. ~18.6 ns per prime. That made testing more primes not worth it: a prime $r$ removes about $1/r$ of the remaining composites, saving a ~4.8 µs Fermat test, so at 18.6 ns per prime the break-even is $r \approx 260$, which is where the chunks stopped.

The gcd is replaced by the Granlund–Montgomery divisibility test per prime (`src/trial_division.hpp`; T. Granlund and P. L. Montgomery, *Division by Invariant Integers using Multiplication*, PLDI 1994, pp. 61–72). For odd $r$, precompute $r^{-1} \bmod 2^{64}$ (Newton's iteration $x \leftarrow x(2 - rx)$ from $x = r$, doubling the correct bits each step) and $L = \lfloor (2^{64}-1)/r \rfloor$. Then $r \mid \mathrm{rem} \iff \mathrm{rem} \cdot r^{-1} \bmod 2^{64} \le L$, since multiplication by $r^{-1}$ is a bijection modulo $2^{64}$ sending the multiples $kr$ ($0 \le k \le L$) onto $\{0, \ldots, L\}$. Each check is one multiply and one compare, and the checks for the primes of a chunk are independent, so they pipeline; the one `mpz_tdiv_ui` per chunk is now the main cost (~1.7–2.6 ns per prime overall in a microbenchmark). The table (primes, inverses, limits, greedy 64-bit chunks) is built at runtime in `main` from `TRIAL_DIVISION_MIN_PRIME`/`TRIAL_DIVISION_MAX_PRIME` and verified at startup (`r * inverse == 1`); debug builds `assert` every check against `rem % r == 0`. `composite_caught_at` is now 64-bit and printed per prime range.

Correctness: with the original range 103–233 the greedy chunks reproduce the old constants exactly, and the search summary (including the per-chunk counts 51105 / 57562 / 50764) is identical in single, 8-thread and debug runs. For every range tested, Fermat survivors, certified counts and everything downstream are identical; only the split of composites between trial division and Fermat moves (the sum stays 1,479,049).

Tuning ($\omega = 33$, single-threaded, 3 alternating runs each, load < 3; medians):

| Range | Time | Trial division / Fermat composites |
|---|---|---|
| Old code (gcd, 103–233) | 15.83 s | 159,431 / 1,319,618 |
| 103–233 | 15.25 s (−3.7%) | 159,431 / 1,319,618 |
| 103–613 | 14.49 s | 364,405 / 1,114,644 |
| 103–1151 | 14.15 s | 471,985 / 1,007,064 |
| 103–2161 | 14.04 s | 563,246 / 915,803 |
| 3–233 | 15.11 s | 170,261 / 1,308,788 |
| 3–613 | 14.34 s | 373,724 / 1,105,325 |
| **3–1151 (chosen)** | **14.04 s (−11.3%)** | 480,511 / 998,538 |
| 3–2161 | 14.00 s | 571,119 / 907,930 |

Gains flatten beyond ~1151, and including the primes below 103 (usually in the support, but not always) helps slightly at no real cost. 3–1151 was chosen over 2161 because the planned 3-limb Montgomery exponentiation makes each Fermat test cheaper, which moves the optimum lower (the prototype predicts ~613). $s = 29$ ($\omega = 33$, 8 threads) dropped from ~94 s to 84.2 s with identical results.

Future: replacing `mpz_tdiv_ui` per chunk with a remainder using a precomputed reciprocal (or precomputed $2^{64}, 2^{128} \bmod m$) could save ~5 ns per chunk (~1% of runtime); worth revisiting after the Montgomery change, when $p$ is already held as three 64-bit words.

---

Date: **17/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO
OMEGA: 33  

## 3-limb Montgomery Arithmetic for the Fermat Test and Power Tree

About 88% of runtime was `mpz_powm` (base-2 Fermat test ~43%, least primitive root power tree ~45%). Every $p$ here is below $2^{181}$, i.e. three 64-bit limbs, where GMP's general-size overhead dominates. `src/mont3.hpp` implements fixed-size Montgomery arithmetic ($R = 2^{192}$): an inlined CIOS Montgomery product, lazy reduction (values in $[0, 2p)$, valid because $4p < R$, i.e. $p < 2^{190}$), division-free setup ($R \bmod p$ by doubling, $R^2 \bmod p$ by five Montgomery squarings; ~130 ns), doubling for the base-2 Fermat test, and sliding-window exponentiation for the tree. A dedicated squaring was not faster than `mul(x, x)` on Apple M-series (the carry chains, not the multiplications, are the bottleneck), so `sqr` is `mul(a, a)`.

`process_support` sets up one Montgomery context per candidate after trial division and passes it to `is_composite_base2_fermat` and `find_least_primitive_root`; the tree traversal is now a template over the value type (`mpz_class` or `mont3::Num`). If `setup` refuses a modulus ($p \ge 2^{190}$) both fall back to GMP (counted as `mont3_fallback_count`; 0 at $\omega = 33$ and $\omega = 32$, where $B \approx 2^{178}$–$2^{181}$).

Correctness: standalone tests in `experimental/montgomery3` (`make test`) compare every operation with GMP on random moduli of 2–190 bits, edge moduli, 200,000 realistic Fermat tests and 20,007 complete power trees (0 mismatches; a planted bug was caught). In `lpr`, the search and producer summaries are identical to the GMP build in single, 2- and 8-thread runs and at $s = 29$; the debug build cross-checks every Fermat result and every power tree leaf against GMP (no assertion failures); and a test build with the fallback forced for every candidate also gives identical results (1,112,602 fallbacks, as expected).

Results (alternating builds, 3 runs each, medians; load < 3 at start):

| Run | GMP | Montgomery | Speedup |
|---|---|---|---|
| $\omega = 33$, single-threaded | 14.00 s | 7.67 s | 1.83x |
| $\omega = 33$, 8 threads | 2.52 s | 1.51 s | 1.67x |
| $\omega = 33$, $s = 29$, 8 threads | 84.9 s | 48.1–49.6 s | ~1.73x |

With 8 threads the single producer is now the bottleneck: the DFS alone takes 1.18 s of the 1.49 s run, consumer waits rose from 7 to 1,508 and no batches are co-processed. One producer can now keep only about 7 threads busy (previously ~14), which makes the parallel producer (`docs/parallelise_producer.md`) more important. The trial division cut-off (3..1151) was tuned for the slower GMP Fermat test and should be re-tuned (the prototype predicts ~613).

---

Date: **17/10/26** (clean-up)   
Author: **Mittun Sudhahar**  
Commit: TODO (uncommitted, on top of `c5bdad5`)
OMEGA: 33 (and 29–32 for checks)  

A full clean-up of the codebase, done in stages, with each stage verified for correctness and speed against the previous one. The work list was `docs/code_review_fixups.md`; every item there is now marked resolved or deliberately deferred.

## Verification protocol
At every stage:
- **Runs:** OMEGA=33, S=30 with `--single`, `--parallel --threads 2`, `--parallel --threads 8` and the debug build with 8 threads (asserts on: every Fermat result and power-tree leaf checked against GMP); OMEGA=33, S=29 with 8 threads; and the dry run.
- **Must be identical:** supports, candidates, Fermat survivors, certified, Grosswald failed/unresolved, largest g(p), winners by p mod 8, power-tree and Jacobi counts, and the producer's DFS, Cor 4 and supports-emitted counts.
- **Speed:** single-threaded and 8-thread runs, alternating with the previous stage's binary, 3 runs each, medians.

All stages matched exactly. The final build also matches `c5bdad5` exactly.

## Stages
1. **Power tree layout (correctness).** The product tree was split in half but stored and traversed heap-style, which is only valid for a complete binary tree. It was wrong for OMEGA = 7, 11–15 and 19–31 (OMEGA = 32–34 were fine, so earlier results stand).
   - The split is now `complete_tree_left_size(n) = min(n − 2^(h−2), 2^(h−1))`, and `main` checks the layout at startup (`power_tree_layout_is_valid`).
   - OMEGA=33 is identical, including the exponentiation counts.
   - The check passes for OMEGA 3–34 and rejects the old split.
   - Debug runs at OMEGA = 29 (primes up to 3,904,921,367), 30 and 31 have no assertion failures (1.44M, 0.55M and 0.75M primes certified in 6, 1 and 1 min). OMEGA = 31 previously aborted.
2. **`mont3.hpp` tidy (still hand-unrolled).**
   - Correct header comment, `static_assert(N == 3)`, `static_cast`, `lo`/`hi` and `shl1` helpers.
   - Debug asserts for the product bounds and the preconditions.
   - The shared Newton inverse is now in `bit_tricks.hpp`.
   - `pow2` saves a squaring, and `bits_at` reads the window value directly.
   - `experimental/montgomery3` now tests `src/mont3.hpp` itself (0 mismatches, and the planted bug is still caught).
   - The window thresholds were benchmarked on the real search: no set was reliably faster than 8/24/80, so they were kept.
   - Single-threaded 8.03 → 7.92 s, 8 threads 1.63 → 1.58 s (alternating, under some load).
3. **`primality.cpp`.**
   - `namespace lpr::primality`, with no reserved `_names`.
   - The search is a template over a GMP or Montgomery backend (no per-leaf branch; no slower: 7.56 → 7.55 s single-threaded, 1.475 → 1.473 s with 8 threads).
   - The debug leaf cross-check and a leaf-order assert apply to both backends; a forced-GMP-fallback debug build gives identical results.
   - Removed `ReuseTracker`, Miller–Rabin and dead code. The search returns `std::optional<unsigned>`.
4. **Stats, `process_support`, queue.**
   - `stats.hpp` is redesigned as a plain `Counters` aggregate plus a `MasterStats` (mutex + `merge`/`snapshot`). That removes the custom copies, `copy_fields_from` and the `NullMutex` template.
   - Removed the legacy summary lines. Added a "Winners by g" histogram (it sums to the certified count).
   - `HysteresisQueue` → `JobQueue` (`job_queue.hpp`). `process_support` is flattened.
   - Single-threaded 7.64 → 7.59 s, 8 threads 1.51 → 1.48 s.
5. **Producer speed-ups.**
   - Statistics are published at most once per second per thread, staggered (previously after every batch).
   - Jobs are fixed size with no heap allocation: an inline support array, and Q as inline limbs read through `mpz_roinit_n`.
   - New stat `consumer_wait_ns`: at OMEGA=33 with 8 threads, consumers wait ~1.1 s in total in a ~1.4 s run (~10% of their time, matching `samply`).
   - Dry run 1.127 → 1.086 s (−3.6%), 8 threads 1.478 → 1.412 s (−4.5%).
6. **Trial division range re-tuned with the Montgomery Fermat test.**

   | Max prime | Single-threaded | 8 threads |
   |---|---|---|
   | 233 | 7.93 s | 1.476 s |
   | 401 | 7.70 s | 1.452 s |
   | 613 | 7.69 s | 1.414 s |
   | 811 | 7.60 s | 1.414 s |
   | 1151 | 7.58 s | 1.452 s |

   1151 and 811 are tied single-threaded (the measure of total CPU work), so the range is **kept at 3..1151**. The prototype's prediction of ~613 did not hold, because trial division also became cheaper. Survivors were identical for every range.
7. **Remaining tidy-ups.**
   - A portable overflow check in `trial_division.hpp` (no `__int128`); the chunks are unchanged.
   - Cited Bagger's Corollary 4 in `generate_supports.hpp`. Removed trailing whitespace.
   - Deleted `src/analysis.hpp`, `.fermat_lab/`, `utils/__pycache__/` and `CONVERSATION_SUMMARY.md`.
   - Updated `CLAUDE.md`, `docs/current_method.md`, `docs/code_review_fixups.md` and `experimental/montgomery3/README.md`.
8. **Final checks.**
   - The last three compiler warnings are fixed: `always_inline` without `inline` on `dfs_iter_`; `-Wpedantic` on `unsigned __int128` (now declared with `__extension__`); and a false-positive `-Warray-bounds` in `bits_at` (the bounds condition is now explicit).
   - Clang and GCC 15 both build with **no warnings** under `-Wall -Wextra -Wpedantic`, and GCC's release and debug builds give identical results.

## Final benchmark (alternating, 3 runs each for single and 8 threads, 2 each for the rest; load rising during the last two)

| Run | `c5bdad5` | Final | Change |
|---|---|---|---|
| OMEGA=33 single-threaded | 7.69 s | 7.64 s | −0.7% |
| OMEGA=33, 8 threads | 1.504 s | 1.431 s | −4.9% |
| OMEGA=33, S=29, 8 threads | 49.1 s | 46.2 s | −5.9% |
| Dry run | 1.195 s | 1.140 s | −4.6% |

The gains are in the multi-threaded runs, where the single producer is the bottleneck; single-threaded work is dominated by the (unchanged) arithmetic.

**OMEGA=32, S=29, 8 threads (full run, final build):** every key line, and the trial-division/Fermat composite counts, are identical to `k32_s29_run.txt` (6,057,734,933 supports; 639,796,795 certified; largest g(p) 678; 0 failed; 0 unresolved). Elapsed 7,902 s against 8,094 s (−2.4%). With the faster producer, consumer waits halved (1,903,644 against 3,676,158) and co-processed batches doubled (197,281 against 89,788). The new `consumer_wait_ns` shows the consumers waited 1,017 s in total, i.e. **~1.8% of consumer time**, so on this 8-thread machine the single producer limits OMEGA=32 only slightly. The ~10% at OMEGA=33 is mostly the short run's start-up and tail.

---

Date: **18/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO (experiments only; nothing in `src/` changed; scripts in `experimental/consumer_ilp/`)
OMEGA: 33  

Experiments on further speed-ups, mainly for the consumers (the producer is to be parallelised). All measurements were taken on the M2, alternating with the current build, under a load of about 2–3.

## Where consumer time goes
Single-threaded profile at OMEGA=33, S=30:

| Stage | Share of runtime | Count | Cost each |
|---|---|---|---|
| Base-2 Fermat | 37% | 1,112,602 tests | ~179 Montgomery squarings |
| Least primitive root search | 42% | 114,064 primes | ~2,240 Montgomery multiplications per prime |
| Trial division | 5% | | |
| `mont3::setup` | 2% | | |

Montgomery `mul`/`powm`/`pow2` make up ~79% of all work. The search figures come from a Python model of the search, run on 3,000 of the real primes (`experimental/consumer_ilp/sim.py`); its counts of tests per prime and failure positions match the program's own counters.
- **The winner's full tree:** ~1,230 multiplications.
- **The ~3.5 failed g per prime:** ~1,010 multiplications, ~290 each. Failures happen at q = 3 (44%), 5 (17%), 7 (9%), and so on.

A failure is expensive because the first leaf, g^(n/q), always needs exponents totalling about log2 n − log2 q ≈ 178 bits along its path, whatever the shape of the tree.

## Interleaving independent exponentiations (measured, promising)
- **Why:** a Montgomery product is a chain of dependent carries, and each squaring in an exponentiation needs the previous result. One exponentiation therefore runs at the *latency* of the chain, leaving the core's multipliers idle much of the time. Two independent exponentiations written in the same loop body (lockstep) let the out-of-order core overlap the two chains.
- **Microbenchmark** (`experimental/consumer_ilp/ilp.cpp`, 20,000 realistic candidates, best of 7):

  | Variant | Time |
  |---|---|
  | One squaring chain | 10.8 ns per squaring |
  | Two chains | 7.6 ns per squaring |
  | Four chains | 7.3 ns per squaring |
  | `pow2` Fermat (current) | 1.95 µs |
  | Branch-free doubling, one chain | 2.17 µs (slower) |
  | `sqr_unrolled`, one chain | 2.10 µs (slower) |
  | 2-way lockstep, `mul` as squaring | 1.70 µs |
  | 4-way lockstep, `mul` as squaring | 1.56–1.60 µs |
  | **2-way lockstep, `sqr_unrolled`** | **1.36–1.40 µs** |
  | 4-way lockstep, `sqr_unrolled` | 1.36 µs |

- **Squaring:** interleaved, the multipliers become the limit. The dedicated squaring (6 limb products instead of 9), which was not worth it on a single chain, now pays off.
- **Batched Fermat prototype** (`experimental/consumer_ilp/batched_fermat_prototype.diff`):
  - `process_batch(jobs, count)` works in three phases: (1) enumerate the candidates of every job in the batch, trial-divide them, and keep each survivor's p, n and Montgomery context; (2) run the Fermat tests two at a time (`fermat_composite2`: lockstep `sqr_unrolled`, branch-free conditional doubling, the shorter exponent padded at the top by starting from `one`); (3) run the search and the Grosswald check on each Fermat survivor.
  - Single-threaded mode buffers `BATCH_SIZE` jobs; consumers and producer co-processing pass their batch straight through.
  - Search summaries were identical in every run:

  | Mode (3 alternating runs) | Current | Batched Fermat |
  |---|---|---|
  | Single-threaded | 8.59–8.68 s | 7.34–7.38 s (−15%) |
  | 2 threads | 4.43–4.49 s | 3.75–3.77 s (−16%) |
  | 8 threads | 1.57–1.68 s | 1.39–1.43 s (−14%) |

- **Still to do before adopting:** a debug (`lpr_g`) GMP cross-check for the paired path, a test of `fermat_composite2` in `experimental/montgomery3`, and a re-measurement on the cluster's x86 CPUs (multiply latency and throughput differ from the M2).

## Pairing Legendre survivors in the power tree (modelled, not implemented)
- **Idea:** for a fixed p, the exponents of the power tree (root g^(2d), then products of the sibling subtrees) do not depend on g. Two Legendre survivors g_a < g_b can therefore be run through the same lazy traversal with a `powm2`, i.e. one sliding-window pass over the exponent with two base tables and two accumulators. The control flow is identical for both lanes.
- **When a lane exits:** when g_a or g_b hits a leaf equal to 1, the other lane continues alone. If g_a is the winner, g_b's lane is dropped. Correctness is unaffected: g_b's result is used only if every smaller survivor failed.
- **Model** (`experimental/consumer_ilp/sim2.py`, 2,000 real primes, lazy cost per leaf from the real layout; a paired step costs 1.40× a single step):
  - The tree is **13% faster** (16% at 1.25×), i.e. about 5% of total runtime.
  - The gain is limited because, when g_a is the winner (about half the time), its full tree runs at the paired step's speed while g_b is still alive.

## SIMD on the cluster (idea)
The batch structure above is what an AVX-512 IFMA Fermat test needs: 8 candidates per vector, with 4 × 52-bit limbs per number (208 bits > 2^181). This could be several times faster than scalar Fermat on Ice Lake, Sapphire Rapids or Zen 4. Check the cluster's CPUs first.

## Ruled out (measured or modelled)
- **Huffman-shaped power tree** (weights log q): 12% more multiplications. Its many small exponents have high window overhead, and the first leaf's cost does not depend on the tree's shape.
- **Skipping composite g via multiplicativity** (all prime factors already known to be q-th residues): 0.077 skippable tests per prime, saving ~18 of ~2,430 multiplications per prime (< 1%). This agrees with the 1.6% bound from node reuse (15/10/26).
- **More trial division:** each chunk near 1151 removes 0.5% of the Fermat calls, which is about its own cost. A batch remainder tree up to 2^16 would remove ~36% of them, but would cost about as much as the tests it saves. (Superseded in part by 19/10/26: precomputing GMP's per-chunk constants makes trial division 2.5× cheaper and moves the best limit to ~2000–4000.)
- **Branch-free conditional doubling on a single chain:** 11% slower; the branchy version is faster there.
- **Doing Fermat through the g = 2 tree path** (reusing 2^(n/3)): saves at most ~2%.
- **A cubic-reciprocity prefilter for q = 3:** at most ~4%, at research-level complexity.

## Producer: a log-free floating-point Corollary 4 filter (prototype, not adopted)
An earlier attempt (`utils/interval_check.cpp`) used `fesetround` around `std::log2`, which is not rigorous:
- libm need not be correctly rounded or honour the rounding mode;
- the compiler may move code across `fesetround` without `-frounding-math`.

The Python log dry run decided near-ties with no fallback, which explains its inaccurate support counts.

The new prototype uses no `log`, only IEEE +, −, ×, ÷ (correctly rounded by the standard), built with `-ffp-contract=off`.
- **What it computes:** since SOP/Q = r = Σ 1/p over the window, T := lhs/rhs = 4218 · 2^(16(ω−s)) · ((s+1) − 2r)^16 / (1 − r)^16, and the exact prune test P > floor(T) − 1 is equivalent to P + 1 > T.
- **Error bound:** P̂ (a double product) and T̂ have a-priori relative errors e1 = γ_ω and e2 ≈ 16·(rel. error of the ratio) + 16u. These come from the standard γ_n = nu/(1 − nu) analysis. For every node, r ≤ r_max (that of the first ω primes), which bounds the error of 1 − r. At ω=33: e2 = 3.0e-13 (S=30) and 1.4e-13 (S=29); the margin is M = 2(e1 + e2) + 16u.
- **Decisions:** a node is pruned if P̂ > T̂(1+M) and kept if P̂(1+M) + 2 < T̂(1−M); anything else goes to the exact `corollary4_qd_bound`. Each node stores T̂_min = min of T̂ along its path. At a leaf, d_max = floor((T_min − 1)/Q) is used only when floor(ŷ(1−M)) = floor(ŷ(1+M)); otherwise it is computed exactly.
- **Results:**
  - S=30: dry run 1.11 s → 0.02 s. Every decision was also checked against the exact computation: 0 mismatches, 0 fallbacks.
  - S=29: dry run 35.7 s → 0.75 s, with 0 fallbacks and identical producer counts. The full exact cross-check was not finished.
- **Still needed:** a written proof of the error bound before this could be used.

---

Date: **19/10/26**   
Author: **Mittun Sudhahar**  
Commit: TODO (scripts in `experimental/trial_division/`; the folding remainder is in `src/`, uncommitted, see the end of this entry)
OMEGA: 33  

Experiments on trial division. Questions:
1. Can libdivide replace the hand-written Granlund–Montgomery test without a penalty?
2. Does GMP keep the reciprocal for each primorial chunk, and what would precomputing it save?
3. Do Lemire's direct-remainder and divisibility methods (Lemire, Kaser and Kurz, "Faster Remainder by Direct Computation", arXiv:1902.01961) speed things up enough to divide by more primes?

All measurements were on the M2, on the 1,593,113 real candidates that reach trial division at OMEGA=33, S=30 (`experimental/trial_division/td.cpp`). Every variant found exactly the same divisor for every candidate as the current code.

## Where the time goes
Trial division is ~5–7% of runtime (~350 ns per candidate × 1.59M candidates ≈ 0.56 s of ~8 s single-threaded). With primes 3..1151 there are 189 primes in 27 chunks, and ~70% of candidates survive every chunk. So the cost is about 27 chunk remainders plus 189 per-prime tests per candidate, and the **chunk remainders dominate**:
- one remainder (`mpz_tdiv_ui` of a 3-limb p by a 64-bit chunk product) costs 10.9 ns;
- one per-prime test costs about one multiply and compare.

## GMP recomputes the reciprocal on every call (adopt, after a portability decision)
`mpz_tdiv_ui` calls `mpn_mod_1`. For a single-limb divisor this computes the divisor's normalisation, its reciprocal (`invert_limb`) and, for the folding variants, B^k mod b (`mpn_mod_1_1p_cps` etc.) on **every call**, then discards them. Nothing is cached, although the same 27 chunk products are used for every candidate. GMP exports routines that take the constants precomputed (`__gmpn_mod_1_1p_cps`/`__gmpn_mod_1_1p`, `__gmpn_mod_1s_2p`, `__gmpn_preinv_mod_1`), but they are declared only in the internal `gmp-impl.h`.

| Chunk remainder | Per remainder |
|---|---|
| `mpz_tdiv_ui` (current) | 10.9 ns |
| `mpn_mod_1` | 8.8 ns (so ~2 ns of `mpz_tdiv_ui` is mpz overhead) |
| GMP `mod_1_1p`, constants precomputed per chunk | **2.8 ns** |
| GMP `mod_1s_2p`, precomputed | 2.9 ns |
| Inline Möller–Granlund 2-by-1 (`udiv_rnnd_preinv`), normalised divisor and reciprocal precomputed | 3.6 ns |

So ~6 ns of the 8.8 ns is recomputing the constants. Full trial division drops from 344–354 ns to **142 ns per candidate** with GMP's routine (2.5×), or 171–176 ns with the inline version (2×).

**End to end** (`td_precomputed_mod_1_1p.diff`, single-threaded, alternating, 3 rounds, load 2–3):

| `TRIAL_DIVISION_MAX_PRIME` | Change vs current | Trial-division composites | Fermat composites |
|---|---|---|---|
| 1151 (current method) | 0 | 480,511 | 998,538 |
| 1151, precomputed | −2 to −3% | 480,511 | 998,538 |
| 1601 | −3 to −5% | 529,619 | 949,430 |
| 2003 | −3 to −5% | 560,175 | 918,874 |
| 2503 | −3 to −5% | 589,189 | 889,860 |
| 3001 | −4 to −5% (best) | 611,966 | 867,083 |
| 4001 | −4 to −5% | 645,012 | 834,037 |
| 6007 | about −3% | 688,775 | |
| 10007 | about 0% | 739,343 | |

Fermat survivors (114,064), certified primes and everything after them were identical in every run, and the trial-division and Fermat composites always sum to 1,479,049. The gain levels off because the chance that a prime r divides p falls like 1/r, while each extra chunk (4–5 primes of 12–13 bits) costs ~3 ns more per candidate.

Limits above 4001 need `MAX_TRIAL_DIVISION_CHUNKS` raised from 128 (4001 gives 97 chunks).

## Portable folding remainder (prototype, recommended)
**Why my first inline version was slower than GMP's.** It does schoolbook division: shift p's three limbs, then three *dependent* Möller–Granlund 2-by-1 steps. That chain is about six multiplies deep. GMP's `mod_1_1p` (generic C, disassembled from the Homebrew arm64 build) instead folds the high limbs using precomputed B mod b and B² mod b, then does a single 2-by-1 step, about three multiplies deep. Copying GMP's source into this MIT-licensed repository would make the copied code LGPL v3+/GPL v2+, so we implemented the folding from our own derivation instead.

**Folding** (`experimental/trial_division/td.cpp`, `fold_mod3`; prototype `td_folding.diff`, `trial_division::remainder_3`). Let x = n₂B² + n₁B + n₀ with B = 2⁶⁴, and let d ≥ 2 be a chunk product. Precompute once per chunk:
- c₁ = B mod d and c₂ = B² mod d;
- s = clz(d) and d_n = d·2^s;
- the Möller–Granlund reciprocal v = ⌊(B² − 1)/d_n⌋ − B, by one 128-bit division at startup (no GMP internals).

Then:
1. **Fold the top limb:** T = (n₁:n₀) + n₂·c₂ ≡ x (mod d). Since n₂c₂ ≤ (B−1)(d−1), we have T < B² + (B−1)(d−1). On a carry out of B², use B² ≡ c₂ (mod d): T′ = T − B² ≤ (B−1)(d−1) − 1, and T′ + c₂ < B·d ≤ B², so adding c₂ back cannot carry again. This is done branch-free.
2. **Fold the high limb:** U = T_hi·c₁ + T_lo ≡ T. Then U ≤ (B−1)(d−1) + (B−1) = (B−1)d < B·d, so U_hi < d.
3. **One 2-by-1 step:** U·2^s < B·d_n, so its high limb is below d_n, which is what the step requires. Apply one step (Möller and Granlund 2011, Alg. 4) by d_n and shift the remainder right by s.

This holds for any d ≥ 2 and any 3-limb x; p of more limbs falls back to `mpz_tdiv_ui`. The critical path is about four multiplies deep.

**Results:**
- **Correctness:**
  - `make test` checks it against GMP on 2,385 divisors (edge cases including d = 2, 2⁶³ ± 1 and 2⁶⁴ − 1; every chunk product for limits 1151, 4001 and 10007; random d) with 5.5M inputs (all-ones and other edge limbs, random): 0 mismatches.
  - The debug build of the prototype asserts every remainder against `mpz_tdiv_ui`. A full OMEGA=33 run with 8 threads had no assertion failures and identical results.
- **Speed:** 2.46 ns per remainder, against 2.93 ns for GMP's precomputed `mod_1_1p` and 10.9 ns for `mpz_tdiv_ui`. Full trial division takes 137.5 ns per candidate (GMP's: 142 ns; current: 343 ns).
- **End to end** (single-threaded, alternating, 3 rounds; load started at 6, so differences under ~2% are noise):

  | Version | Single-threaded | Change |
  |---|---|---|
  | Current | 8.68–8.87 s | 0 |
  | Folding at 1151 | 8.40–8.45 s | −3.5% |
  | Folding at 2003 | 8.23–8.37 s | about −5% |
  | Folding at 3001 | 8.21–8.36 s | about −5% |

  With 8 threads, at 3001: 1.61–1.62 s → 1.53–1.55 s. All key lines were identical in every run.

**Before adopting:** the batched Fermat test (18/10/26) makes Fermat ~1.4× cheaper, which moves the best limit back down, so re-tune the limit after both changes are in. This version uses no GMP internals, so it does not depend on which GMP the cluster provides. The run header already prints the runtime GMP version (`main.cpp`).

## Sources for the folding reduction
The congruence behind folding is elementary: if x = Σ xᵢBⁱ, then x ≡ Σ xᵢ·(Bⁱ mod d) (mod d). It is the same idea as the divisibility tests for 9 and 11 (10 ≡ ±1). The engineering, i.e. precomputing Bᵏ mod d once and folding a wide value into a narrower one with the same remainder, has these published sources:
- **W. Hasenplaugh, G. Gaubatz and V. Gopal, "Fast Modular Reduction", Proc. 18th IEEE Symposium on Computer Arithmetic (ARITH-18), Montpellier, 2007, pp. 225–229, doi:10.1109/ARITH.2007.18.** This is the primary source for "folding": a 2m-word N is replaced by N′ = N_H·(2^f mod M) + N_L with 2^f mod M precomputed, and N′ ≡ N (mod M). They fold twice, then finish with Barrett reduction. Our `remainder_3` has the same structure, with a Möller–Granlund 2-by-1 step instead of Barrett.
- **R. P. Brent and P. Zimmermann, *Modern Computer Arithmetic*, Cambridge University Press, 2010** (free author PDF, version 0.5.9, from Brent's ANU page).
  - §1.4.6: Svoboda's algorithm, which reduces with a precomputed multiple of the divisor when the remainder is wanted for many divisions by the same divisor.
  - §2.4.2: generalises this to repeated "folding", with Table 2.2 comparing 1-, 2- and 3-folding.
  - §2.9 (Notes and references): attributes the folding optimisation to Hasenplaugh, Gaubatz and Gopal [118].

  Read directly (pp. 23 and 62–63 and the Chapter 2 notes of the 0.5.9 PDF).
- **GMP's implementation:** `mpn/generic/mod_1_1.c` (Granlund and Möller) and `mod_1_4.c` (Granlund), both "Based on a suggestion by Peter L. Montgomery" (header comments, read in the official 6.3.0 release). They give no paper reference.
- **The final 2-by-1 step:** N. Möller and T. Granlund, "Improved division by invariant integers", *IEEE Trans. Computers* 60(2), 2011, pp. 165–175, Alg. 4 and Theorem 2. This paper covers 2-by-1 and 3-by-2 division and n-by-1 division by repeated 2-by-1 steps, **not** folding. Its reference [4] (Granlund and Möller, "Division of integers large and small", 2009, "to appear") could not be found in print.

**Patent (checked 19/10/26, not legal advice).** The folding technique was patented by Intel:
- US 8,229,109 B2, "Modular reduction using folding" (Hasenplaugh, Gaubatz, Gopal; filed 2006-06-27, granted 2012-07-24). Its claims describe precomputing 2^f mod M and folding twice before a final reduction.
- Google Patents lists it as **lapsed for non-payment of maintenance fees** (effective 2020-07-24).
- Every family member is listed as expired, withdrawn or ceased: CN101097511B, EP2033357A2 (withdrawn), WO2008002828 (ceased), KR101062558B1, JP5116770B2, JP5378579B2 and TWI350094B. There is no Australian family member.

So the technique is free to use.

## Failed ideas
- **libdivide instead of Granlund–Montgomery: slower.**

  | Per-prime test (same inline remainder) | Full trial division |
  |---|---|
  | Granlund–Montgomery | 171–176 ns per candidate |
  | `x % r` | 205–210 ns |
  | libdivide, branchy | 247–252 ns |
  | libdivide, branch-free | 274–306 ns |

  With `mpz_tdiv_ui`, libdivide branch-free gives 454 ns against 344–354 ns. libdivide has no divisibility test: it computes the quotient (a high multiply, a shift and, branchy, a branch on the algorithm type), so divisibility needs a further `q * r == x`, i.e. two dependent multiplies. Granlund–Montgomery needs one multiply and one compare, the minimum for an odd divisor.
- **Lemire's direct remainder / divisibility test: slower.**
  - The paper's 25–50% gains are for **32-bit** operands on 64-bit hardware: with n < 2^32 and c = ⌈2^64/d⌉, d | n ⟺ n·c mod 2^64 ≤ c − 1 is one 64-bit multiply. For *even* divisors it beats the modular-inverse (Granlund–Montgomery) test, which then needs an extra rotate. Our divisors are odd, so Granlund–Montgomery is already one multiply and one compare.
  - Our remainders are **64-bit** chunk remainders. Lemire's test then needs a 128-bit c and a 64 × 128-bit product: 267–280 ns per candidate against 171–176 ns (with `mpz_tdiv_ui`: 476–500 ns against 344–354 ns).
  - Packing into 32-bit sub-chunks so that Lemire's 32-bit test applies costs a hardware divide per sub-chunk: 217 ns.
  - The direct remainder (fastmod) is no help either: only divisibility is needed, and the expensive step is the multi-limb chunk remainder, which Lemire's method does not address.
- **No chunks at all: slower.** p mod r computed directly for each prime, by folding p's six 32-bit words with precomputed 2^(32i) mod r and then applying Granlund–Montgomery to the < 2^47 sum: 251 ns. Six multiplies per prime outweigh one 3-ns remainder per 5–7 primes.
- **Much higher trial-division limits:** no further gain past ~4001, as above (10007 is back to the current speed). This agrees with 18/10/26: a batch remainder tree up to 2^16 would also cost about as much as the Fermat tests it removes.

## Adopted: folding remainder in `src/` (uncommitted, 19/10/26)
**Changes:**
- `src/trial_division.hpp`: `Chunk` gains c₁, c₂, d_n, v and the shift, set by `set_remainder_constants` in `build`; `remainder_3` is the folding remainder.
- `verify` (run at startup, also in release builds) now also recomputes every chunk's constants and checks `remainder_3` on 343 edge-case inputs per chunk against Horner's rule in 128-bit arithmetic.
- `src/primality.cpp`: `is_composite_small_prime` uses `remainder_3` for p of up to 3 limbs and `mpz_tdiv_ui` otherwise. Debug builds assert every remainder against `mpz_tdiv_ui`.
- `TRIAL_DIVISION_MAX_PRIME` is unchanged (1151), so the per-chunk statistics are directly comparable.

**Correctness:**
- **`remainder_3` against GMP** (a standalone test including `src/trial_division.hpp`): 12,014,200 remainders over 5,588 divisors, 0 mismatches. The divisors were edge cases (2, 3, 2³² ± …, 2⁶³ ± 1, 2⁶⁴ − 1, …), every chunk product for limits 3–10007, and 5,000 random d. The inputs were edge limbs, random limbs, and values qd, qd + 1 and qd + d − 1.
- **`verify`** passes on all 9 tables. Mutation check: 15 of 15 deliberately corrupted constants (c₁, c₂, v, d_n, shift) were caught.
- **Full runs against a reference build of `2aca1ee`** (`git archive`): the **entire** search summary was identical, including every per-chunk trial-division line, the Jacobi and power-tree counts and both winner histograms, as were the producer's DFS and Cor 4 counts and the exit codes. Modes:
  - `--single`;
  - `--parallel --threads 2` and `--threads 8`;
  - the debug build with 8 threads (every remainder asserted);
  - `--dry-run`;
  - S=29 with 8 threads;
  - `--config config/omega33.conf`.
- **Debug runs at OMEGA = 32, 31 and 30** (8 threads, 60 s each, stopped by SIGTERM): 28.0M candidates, 0 assertion failures, 0 Grosswald failures.
- **Builds:** clang (release and debug) and GCC 15 (`-Wall -Wextra -Wpedantic`), with no warnings.

**Speed** (alternating with the reference build; another process had pushed the load average to ~36 when the timings started):

| Run | `2aca1ee` | Folding | Change |
|---|---|---|---|
| Single-threaded | 7.56–7.66 s | 7.29–7.38 s | −3.5% |
| 8 threads | 1.38–1.40 s (CPU 10.2 s) | 1.33 s (CPU 9.65 s) | −4.5% (CPU −5%) |
| S=29, 8 threads | 44.8–45.4 s (CPU 324–327 s) | 43.3–44.1 s (CPU 307–313 s) | −3% (CPU −5%) |

The prototype measurements (above) suggest a further ~1.5% from raising `TRIAL_DIVISION_MAX_PRIME` to ~2000–3000. That is left for when the limit is re-tuned together with batched Fermat.

