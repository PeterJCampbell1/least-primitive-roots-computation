/**
 * bit_tricks.hpp: small word-level helpers: the inverse mod 2^64 shared by
 * trial_division.hpp and mont3.hpp, and the support hash used for the supports
 * checksum (generate_supports.hpp).
 **/

#pragma once

#include <cstddef>
#include <cstdint>

namespace lpr::bits {

/*
 * r^{-1} mod 2^64 for odd r, by Newton's iteration x <- x * (2 - r * x).
 *
 * x = r is correct to 3 bits (r * r == 1 mod 8 for odd r). If r * x == 1 + e * 2^k,
 * then r * x * (2 - r * x) == (1 + e * 2^k)(1 - e * 2^k) == 1 - e^2 * 2^(2k), so each
 * step doubles the number of correct low bits: 3 -> 6 -> 12 -> 24 -> 48 -> 96 >= 64.
 * Unsigned arithmetic wraps mod 2^64, which is exactly the arithmetic needed.
 */
constexpr std::uint64_t inverse_mod_2_64(std::uint64_t r) {
    std::uint64_t x = r;
    for (int i = 0; i < 5; ++i) {
        x *= 2 - r * x;
    }
    return x;
}

/*
 * The mixing step of the SplitMix64 generator: G. L. Steele Jr., D. Lea and
 * C. H. Flood, "Fast Splittable Pseudorandom Number Generators", OOPSLA 2014,
 * https://doi.org/10.1145/2660193.2660195. The shifts and multipliers are
 * David Stafford's variant "Mix13" from "Better Bit Mixing - Improving on
 * MurmurHash3's 64-bit Finalizer" (2011).
 *
 * A bijection on 64-bit values (adding a constant, x ^ (x >> k) and multiplying
 * by an odd constant mod 2^64 are each invertible) with strong avalanche: flipping
 * any input bit flips about half of the output bits.
 */
constexpr std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL; // 2^64 / golden ratio (odd)
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/*
 * Hash of one support (its primes in increasing order). Each prime is mixed into
 * the running state, so the hash depends on the whole support, not just on which
 * primes occur. Summing these hashes (mod 2^64) over all emitted supports gives a
 * checksum of the set of supports that is independent of the emission order, so
 * it can be compared across runs with any number of threads.
 */
constexpr std::uint64_t support_hash(const unsigned* support, std::size_t count) {
    std::uint64_t h = 0;
    for (std::size_t i = 0; i < count; ++i) {
        h = mix64(h ^ support[i]);
    }
    return h;
}

// Testing
static_assert(inverse_mod_2_64(3) * 3 == 1, "inverse_mod_2_64 self-check");
static_assert(inverse_mod_2_64(0xFFFFFFFFFFFFFFFFull) * 0xFFFFFFFFFFFFFFFFull == 1,
              "inverse_mod_2_64 self-check");

// mix64(0) is SplitMix64's first output for seed 0, which checks the constants
static_assert(mix64(0) == 0xe220a8397b1dcdafULL, "mix64 self-check");
static_assert(mix64(1) == 0x910a2dec89025cc1ULL, "mix64 self-check");
inline constexpr unsigned SUPPORT_HASH_TEST_PRIMES[] = {2, 3, 5};
static_assert(support_hash(SUPPORT_HASH_TEST_PRIMES, 3) == 0xb3b14f8d257d4fadULL,
              "support_hash self-check");

} // namespace lpr::bits
