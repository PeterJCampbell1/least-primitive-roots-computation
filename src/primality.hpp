#ifndef LPR_PRIMALITY_HPP
#define LPR_PRIMALITY_HPP

#include "stats.hpp"
#include "parameters.hpp"
#include "mont3.hpp"

#include <gmpxx.h>
#include <optional>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>

// Trial division by the primes in lpr::trial_division::table() (set in main).
// Requires p >= 3. True proves compositeness; false does not prove primality.
bool is_composite_small_prime(
    const mpz_class& p
);

// Requires p >= 3 and n = p - 1. True proves compositeness; false does not prove
// primality. With a Montgomery context for p (lpr::mont3::setup succeeded) the
// power is computed with 3-limb Montgomery arithmetic, otherwise with GMP.
bool is_composite_base2_fermat(
    const mpz_class& p,
    const mpz_class& n,
    const lpr::mont3::Context* mont = nullptr
);

// Requires p >= 2. True proves compositeness; false does not prove primality.
bool is_composite_base2_miller_rabin(
    const mpz_class& p,
    const mpz_class& n
);

// Requires p >= 2, n = p - 1, and all distinct prime divisors of n.
// Searches 2 <= g <= search_limit and returns the least primitive root if found.
// No result means unresolved within the limit, not composite or a counterexample.
// With a Montgomery context for p the power tree uses 3-limb Montgomery
// arithmetic, otherwise GMP.
std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const unsigned& d,
    const std::vector<unsigned>& prime_divisors,
    const lpr::mont3::Context* mont = nullptr
);

template <unsigned N> 
constexpr std::array<bool, N + 1> make_table() {
    std::array<bool, N + 1> t{};
    for (std::uint64_t a = 2; a * a <= N; ++a)
        for (std::uint64_t v = a * a; v <= N; v *= a)
            t[static_cast<std::size_t>(v)] = true;
    return t;
}

inline constexpr auto IS_PERFECT_POWER_TABLE = make_table<parameters::PRIMITIVE_ROOT_SEARCH_LIMIT>();


#endif
