# Optimisation Notes to method.md

See method.md for original method.

---

Date: **28/09/21**   
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

Date: **28/09/21**   
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

