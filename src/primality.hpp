/**
 * primality.hpp: the per-candidate tests applied to p = Q*d + 1.
 *
 *   is_composite_small_prime   trial division (trial_division.hpp tables)
 *   is_composite_base2_fermat  base-2 Fermat test
 *   find_least_primitive_root  least primitive root search (power tree)
 *
 * The two power-based tests take an optional Montgomery context for p
 * (lpr::mont3::setup succeeded): with it they use 3-limb Montgomery
 * arithmetic, without it (nullptr) GMP.
 **/

#pragma once

#include <gmpxx.h>

#include <cstddef>
#include <optional>

namespace lpr::mont3 {
struct Context;
}

namespace lpr::primality {

// Trial division by the primes in lpr::trial_division::table() (set in main).
// Requires p >= 3. True proves p composite; false proves nothing.
bool is_composite_small_prime(const mpz_class& p);

// Base-2 Fermat test. Requires p >= 3 and n = p - 1, and `mont` (if given) must
// be the Montgomery context of this p. True proves p composite; false does not
// prove p prime.
bool is_composite_base2_fermat(
    const mpz_class& p,
    const mpz_class& n,
    const lpr::mont3::Context* mont = nullptr
);

// Least primitive root of p = Q*d + 1, assuming p is prime. Requires p odd,
// p > 2, and prime_divisors[0..omega) = the support of p - 1 (all omega distinct
// primes, increasing, starting with 2). `mont` (if given) must be the Montgomery context
// of this p. Searches 2 <= g <= PRIMITIVE_ROOT_SEARCH_LIMIT; no result means
// unresolved within the limit (to be checked by hand), not a counterexample.
std::optional<unsigned> find_least_primitive_root(
    const mpz_class& p,
    unsigned d,
    const unsigned* prime_divisors,
    std::size_t omega,
    const lpr::mont3::Context* mont = nullptr
);

// Number of leaves in the left subtree of a complete binary tree with n >= 2
// leaves: the split used by the product tree (see primality.cpp).
std::size_t complete_tree_left_size(std::size_t n);

// True if the power tree over `leaves` odd primes has a valid heap layout: every
// node index is inside the tree, the traversal's leaf test matches the real
// leaves, and the leaves are visited in increasing order. Checked once in main.
bool power_tree_layout_is_valid(std::size_t leaves);

} // namespace lpr::primality
