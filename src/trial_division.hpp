/**
 * trial_division.hpp: tables for trial division by small primes, using the
 * Granlund-Montgomery divisibility test.
 *
 * For each odd prime r in [min_prime, max_prime] we precompute
 *     inverse = r^{-1} mod 2^64      and      limit = floor((2^64 - 1) / r).
 * For any 64-bit x,   r | x   <=>   x * inverse (mod 2^64) <= limit.
 *
 * Proof: since r is odd, inverse exists and x -> x * inverse (mod 2^64) is a
 * bijection on 64-bit values. The multiples of r below 2^64 are k*r for
 * 0 <= k <= limit, and they map to k, i.e. onto exactly {0, ..., limit}. As
 * the map is a bijection, every non-multiple maps above limit.
 *
 * The primes are grouped into chunks of consecutive primes whose product fits
 * in 64 bits. For a candidate p we compute rem = p mod (chunk product) once per
 * chunk; since r divides the product, p == rem (mod r), so r | p <=> r | rem.
 *
 * The table is built once at runtime (a few KB) in main, before any worker
 * thread starts, and is read-only afterwards, so reads need no locking.
 *
 * Reference: T. Granlund and P. L. Montgomery, "Division by Invariant Integers
 * using Multiplication", Proceedings of the ACM SIGPLAN '94 Conference on
 * Programming Language Design and Implementation (PLDI), 1994, pp. 61-72.
 **/

#pragma once

#include "bit_tricks.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace lpr::trial_division {

// One prime r used for trial division, with its precomputed test constants
struct PrimeTest {
    std::uint64_t inverse; // r^{-1} mod 2^64 (exists because r is odd)
    std::uint64_t limit;   // floor((2^64 - 1) / r): the largest k with k*r < 2^64
    unsigned prime;        // r itself (for statistics and debug checks)
};

// Consecutive primes whose product fits in 64 bits
struct Chunk {
    std::uint64_t product; // product of the chunk's primes, used for p mod product
    std::size_t first;     // index of the chunk's first prime in Table::tests
    std::size_t count;     // number of primes in the chunk
};

struct Table {
    std::vector<PrimeTest> tests; // every prime, in increasing order
    std::vector<Chunk> chunks;    // smallest primes first: they are the most likely divisors
    unsigned min_prime = 0;
    unsigned max_prime = 0;
};

/*
 * Builds the table for every prime in [min_prime, max_prime] (requires
 * 3 <= min_prime <= max_prime). Uses its own small sieve, independent of the
 * search's prime list, which can be shorter than max_prime (e.g. omega = 34).
 */
inline Table build(unsigned min_prime, unsigned max_prime) {
    assert(3 <= min_prime && min_prime <= max_prime);
    Table table;
    table.min_prime = min_prime;
    table.max_prime = max_prime;

    // Sieve of Eratosthenes up to max_prime (a few thousand at most)
    std::vector<bool> is_prime(static_cast<std::size_t>(max_prime) + 1, true);
    for (std::size_t q = 2; q * q <= max_prime; ++q) {
        if (!is_prime[q]) continue;
        for (std::size_t m = q * q; m <= max_prime; m += q) {
            is_prime[m] = false;
        }
    }

    constexpr std::uint64_t MAX_U64 = std::numeric_limits<std::uint64_t>::max();
    Chunk current{1, 0, 0};
    for (unsigned r = min_prime; r <= max_prime; ++r) {
        if (!is_prime[r]) continue;

        PrimeTest test;
        test.prime = r;
        test.inverse = lpr::bits::inverse_mod_2_64(r);
        test.limit = MAX_U64 / r;
        assert(static_cast<std::uint64_t>(r) * test.inverse == 1 && "bad inverse mod 2^64");

        // Greedy packing: start a new chunk when the product would overflow 64 bits
        // (product * r > MAX  <=>  product > floor(MAX / r), for integers)
        if (current.product > MAX_U64 / r) {
            table.chunks.push_back(current);
            current = Chunk{1, table.tests.size(), 0};
        }
        current.product *= r;
        ++current.count;
        table.tests.push_back(test);
    }
    if (current.count > 0) {
        table.chunks.push_back(current);
    }
    return table;
}

namespace detail {
inline Table& instance() {
    static Table table;
    return table;
}
} // namespace detail

// The active table (set once in main via set_table)
inline const Table& table() {
    return detail::instance();
}

inline void set_table(Table table) {
    detail::instance() = std::move(table);
}

// Checks every prime's constants (called once at startup, also in release builds).
// Returns false if any inverse or limit is wrong.
inline bool verify(const Table& table) {
    for (const PrimeTest& t : table.tests) {
        if (static_cast<std::uint64_t>(t.prime) * t.inverse != 1) return false;
        if (t.limit != std::numeric_limits<std::uint64_t>::max() / t.prime) return false;
    }
    return true;
}

} // namespace lpr::trial_division
