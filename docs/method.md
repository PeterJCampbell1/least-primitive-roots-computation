# Computational method for the $\omega(p-1)=33$ verification

This note describes the method implemented in release `v1.0-k33` of the least-primitive-roots computation. It follows the finite-search strategy suggested in Gustav Kjærbye Bagger's paper *A Note on Least Primitive Roots* (arXiv:2608.20431).

The aim is to resolve the case
$$
\omega(p-1)=33,
$$
where $\omega(n)$ denotes the number of distinct prime divisors of $n$. The program searches the remaining finite range using the bound
$$
B=1.8\times 10^{54}
$$
on $n=p-1$. Thus every candidate considered satisfies $p-1 \leq B$. The value $B$ is chosen to lie safely above the $\Delta_{33}$ value obtained from the displayed formula in the paper.

## 1. Enumerating the 33-prime supports

**Implementation:** `src/main.cpp`, function `dfs()`.

Write the distinct prime divisors of $p-1$ as
$$
q_1<q_2<\cdots<q_{33},
$$
and define the squarefree support product
$$
Q=q_1q_2\cdots q_{33}.
$$

The program recursively generates every possible ordered support of 33 distinct primes with
$$
Q\leq B.
$$
Each completed support is processed immediately before the search backtracks and continues to the next support.

This is done by a recursive depth-first search. At each stage, the program chooses one prime, appends it to the current support, and recursively explores all valid completions of that partial support before backtracking and trying the next prime. Only primes larger than the most recently chosen prime are allowed, so each support is generated exactly once.

Before making a recursive call, the program computes the smallest possible completed product obtained by filling the remaining support positions with the next available primes. If even this smallest completion exceeds $B$, neither the current choice nor any later choice at the same recursive level can produce a valid support, because the minimum completed product increases with the chosen prime. The program therefore terminates that level’s loop.

**Prime-list completeness check:** `src/main.cpp`, function `main()`.
The generated prime list must also be long enough to contain every prime that could occur in such a support. If $r$ is the largest prime in a 33-prime support, then
$$
p_{32}\#\,r\leq B,
$$
so
$$
r\leq \left\lfloor\frac{B}{p_{32}\#}\right\rfloor=3422.
$$

The program generates all primes up to `PRIME_LIMIT = 4000` and checks at runtime that this limit is large enough for exhaustive support enumeration.

Internally, the variable `next_index` ensures that once a prime has been chosen, only later primes in the ordered prime list may be appended. This forces each support to be generated in increasing order and prevents duplicates such as different permutations of the same set of primes.

When a recursive call returns, all descendants of that choice have been explored. The chosen prime is then removed from the working support, and the previous recursive level continues with the next available prime. This is the backtracking step.

### Toy example of the depth-first search

The following example is intended only to illustrate the recursive depth-first search and backtracking mechanism. It deliberately uses the restricted prime list
$$
2,3,5,7
$$
and therefore does **not** satisfy the program's separate check that the generated prime list is long enough for exhaustive enumeration.

Suppose, purely for illustrating the DFS, that we want to enumerate three-prime supports drawn from
$$
2,3,5,7,
$$
with product bounded by
$$
B=100.
$$

The search begins with the empty support. Then $2$ is chosen first, the smallest possible completion is
$$
2\cdot3\cdot5=30\leq 100,
$$
so the search descends into the branch beginning with $2$. The program makes a recursive call with working support $\{2\}$. Within that call it chooses the next prime and recursively descends again, continuing until the support contains $k=3$ primes.

From the support $\{2,3\}$, the choices $5$ and $7$ give
$$
2\cdot3\cdot5=30
\qquad\text{and}\qquad
2\cdot3\cdot7=42,
$$
so both supports are retained with
$$
\{2,3,5\},
\qquad
\{2,3,7\}.
$$

After these branches have been exhausted, the search backtracks to $\{2\}$ and tries $5$ as the second prime. The smallest possible
completion is then
$$
2\cdot5\cdot7=70\leq 100,
$$
giving the additional support
$$
\{2,5,7\}.
$$

Finally, after backtracking to the empty support, the search considers starting with $3$. The smallest possible completion would be
$$
3\cdot5\cdot7=105>100.
$$

Hence this branch is discarded. Since all later choices for the first prime would only increase the product, the search can stop completely at this point.

Thus the depth-first search produces exactly
$$
\{2,3,5\},\qquad
\{2,3,7\},\qquad
\{2,5,7\},
$$
which are precisely the three-prime supports from the given prime list whose product is at most $100$.

The actual $k=33$ search uses exactly the same mechanism, but with a much larger prime list and search bound.

## 2. Applying Corollary 4 at the support level

**Implementation:** `src/corollary4.cpp`, functions `make_corollary4_threshold()` and `corollary4_proves_grosswald()`.

For each completed support, the program precomputes the factor appearing in Corollary 4 using
$$
\omega=33,\qquad s=30.
$$

Corollary 4 does not require $s=30$. The value of $s$ may be chosen subject to the hypotheses of the corollary. In this implementation, $s=30$ is fixed throughout the $k=33$ computation. This choice is sufficient for the verification and keeps the support-dependent threshold simple. Optimising $s$ separately for each support could potentially eliminate additional candidates through Corollary 4, but is not required for correctness.

With $s = 30$, the 30 largest support primes are used in the reciprocal sum, so
$$
\delta
=
1-\sum_{i=4}^{33}\frac1{q_i}.
$$
The code verifies that $\delta>0$.

If a candidate $p$ is prime, then $p>2$ implies that $p-1$ is even, so $2$ is one of the support primes. Thus the three primes excluded from the reciprocal sum include $2$, and their product may be taken as the even divisor $e$ in Corollary 4. None of the 30 primes appearing in the reciprocal sum defining $\delta$ divides $e$. Moreover,
$$
p\ge p_{33}\#+1>10^{47},
\qquad
p\le B+1<10^{56},
$$
so every prime candidate lies in the range required by Corollary 4.

For this fixed choice,
$$
F
=
\left(\frac{2\delta+s-1}{\delta}\right)2^{\omega-s}
=
8\left(2+\frac{29}{\delta}\right).
$$

Corollary 4 gives the sufficient condition
$$
4220F^{16}<p.
$$

All arithmetic used to form this threshold is exact. If
$$
F=\frac ab
$$
in lowest terms, the program stores
$$
4220a^{16}
\quad\text{and}\quad
b^{16}
$$
and tests the equivalent integer inequality
$$
pb^{16}>4220a^{16}.
$$

For a fixed support, the Corollary 4 threshold is fixed. The smallest possible value arising from that support is $Q+1$. Therefore, if $Q+1$ already satisfies the inequality, every later value $Qd+1$ also satisfies it, and the entire support can be discarded immediately.

## 3. Enumerating all $p-1$ with a fixed support

**Implementation:** `src/main.cpp`, the completed-support branch of `dfs()`.

If the support is not eliminated at $Q+1$, write
$$
p-1=Qd.
$$

Since $Qd\leq B$, it is enough to consider
$$
1\leq d\leq \left\lfloor\frac BQ\right\rfloor.
$$

However, only those $d$ whose prime divisors already belong to the support are admissible. Equivalently,
$$
\operatorname{rad}(d)=\prod_{\substack{\ell \mid d \\ \ell \text{ prime}}} \ell \mid Q.
$$
This avoids introducing any new distinct prime factor into the value
$$
p-1=Qd
$$
being checked. The program checks this by copying $d$ into a variable `remaining_factor` and repeatedly dividing out every support prime. The value $d$ is retained if and only if the remaining factor is 1. Therefore $Qd$ has exactly the same 33 distinct prime divisors as $Q$, although their exponents may be larger.

Conversely, every integer $n\leq B$ with $\omega(n)=33$ has a unique representation
$$
n=Qd,
$$
where $Q=\operatorname{rad}(n)$. The support search generates this $Q$, and $d=n/Q$ satisfies
$$
d\leq \left\lfloor\frac{B}{Q}\right\rfloor
\qquad\text{and}\qquad
\operatorname{rad}(d)\mid Q.
$$
Hence every such $n$ is covered by the search.

For every admissible $d$, the program forms
$$
p=Qd+1.
$$

Corollary 4 is tested again. Since $p=Qd+1$ increases with $d$ while the support-specific threshold remains fixed, once one admissible $p$ satisfies Corollary 4, all later values do as well and the multiplier loop terminates.

## 4. Base-2 Fermat compositeness filter

**Implementation:** `src/primality.cpp`, function `is_composite_base2_fermat()`.

Candidates not covered by Corollary 4 are first tested using
$$
2^{p-1}\pmod p.
$$

If
$$
2^{p-1}\not\equiv1\pmod p,
$$
then Fermat's little theorem proves that $p$ is composite, so the candidate is discarded.

A candidate satisfying
$$
2^{p-1}\equiv1\pmod p
$$
is only a Fermat survivor. This does not by itself prove primality.

## 5. Certifying the least primitive root

**Implementation:** `src/primality.cpp`, function `find_least_primitive_root()`.

For each Fermat survivor, set
$$
n=p-1.
$$

The support already contains every distinct prime divisor of $n$. The program iterates over integers
$$
g=2,3,\ldots,
$$
up to `PRIMITIVE_ROOT_SEARCH_LIMIT = 1000`.

The order criterion used is
$$
g^k\equiv1\pmod p
\iff
\operatorname{ord}_p(g)\mid k,
$$

once $(g,p)=1$.

The program first checks
$$
g^n\equiv1\pmod p.
$$

Passing this test establishes $(g,p)=1$, so $\operatorname{ord}_p(g)$ is defined and divides $n$.

The program then checks, for every distinct prime divisor $q\mid n$, that
$$
g^{n/q}\not\equiv1\pmod p.
$$

If $\operatorname{ord}_p(g)$ were a proper divisor of $n$, then for some prime $q\mid n$ one would have
$$
\operatorname{ord}_p(g)\mid \frac nq,
$$
which would force $g^{n/q}\equiv1\pmod p$. Therefore, if none of these tests returns 1, then
$$
\operatorname{ord}_p(g)=n=p-1.
$$

An element of order $p-1$ also certifies that $p$ is prime: the order of a unit modulo $p$ divides $\varphi(p)$, and $\varphi(p)\leq p-1$, so order $p-1$ forces $\varphi(p)=p-1$. Since $\varphi(p)=p-1$ only when $p$ is prime, this certifies primality.

Because the program tests $g$ in increasing order, the first successful value is the least primitive root $g(p)$.

Finally, the program verifies Grosswald's inequality directly by checking
$$
(g(p)+2)^2<p.
$$

If no full-order element is found up to the search limit, the candidate is recorded as unresolved rather than silently discarded.

## 6. Successful completion criterion

The program exits successfully only if

- there are no unresolved candidates, and
- no certified prime fails $(g(p)+2)^2<p$.

The complete output of the final `v1.0-k33` run is recorded in `k33_run.txt`.

Combined with the analytic bounds outside the finite search range, a successful run eliminates the remaining $\omega(p-1)=33$ case.

## 7. Code layout

- `src/main.cpp` — prime generation, exhaustive support enumeration, multiplier enumeration, and coordination of the full computation.
- `src/corollary4.cpp` / `src/corollary4.hpp` — exact support-specific Corollary 4 threshold calculation and testing.
- `src/primality.cpp` / `src/primality.hpp` — base-2 Fermat compositeness filtering and least-primitive-root certification.

The current Corollary 4 implementation is deliberately specialized to $\omega=33$ and $s=30$. Adapting the program to smaller values of $\omega$ requires changing this support-specific part of the implementation rather than only changing `K` in `main.cpp`.
