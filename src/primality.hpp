#ifndef LPR_PRIMALITY_HPP
#define LPR_PRIMALITY_HPP

#include <gmpxx.h>
#include <optional>
#include <vector>

// Requires p >= 2. True proves compositeness; false does not prove primality.
bool is_composite_base2_fermat(const mpz_class& p);

// Requires p >= 2, n = p - 1, and all distinct prime divisors of n.
// Searches 2 <= g <= search_limit and returns the least primitive root if found.
// No result means unresolved within the limit, not composite or a counterexample.
std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const mpz_class& n,
    const std::vector<unsigned>& prime_divisors,
    unsigned search_limit
);

#endif
