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
 * The chunk remainder of a p of up to 3 limbs is computed by remainder_3
 * (folding with constants precomputed per chunk), about 4x faster than
 * mpz_tdiv_ui, which recomputes its reciprocal on every call.
 *
 * The table is built once at runtime (a few KB) in main, before any worker
 * thread starts, and is read-only afterwards, so reads need no locking.
 *
 * References:
 *   T. Granlund and P. L. Montgomery, "Division by Invariant Integers using
 *   Multiplication", Proc. ACM SIGPLAN '94 Conference on Programming Language
 *   Design and Implementation (PLDI), 1994, pp. 61-72 (divisibility test).
 *   W. Hasenplaugh, G. Gaubatz and V. Gopal, "Fast Modular Reduction", Proc.
 *   18th IEEE Symposium on Computer Arithmetic (ARITH-18), 2007, pp. 225-229,
 *   and R. P. Brent and P. Zimmermann, "Modern Computer Arithmetic", CUP 2010,
 *   sections 1.4.6 and 2.4.2 (folding with precomputed B^k mod d).
 *   N. Moller and T. Granlund, "Improved Division by Invariant Integers", IEEE
 *   Trans. Computers 60(2), 2011, pp. 165-175, Alg. 4 (the final 2-by-1 step).
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

__extension__ typedef unsigned __int128 u128;

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
    // Constants for remainder_3, set by set_remainder_constants (B = 2^64)
    std::uint64_t c1 = 0;  // B mod product
    std::uint64_t c2 = 0;  // B^2 mod product
    std::uint64_t dn = 0;  // product << shift (normalised: top bit set)
    std::uint64_t v = 0;   // Moller-Granlund reciprocal of dn: floor((B^2 - 1) / dn) - B
    unsigned shift = 0;    // leading zeros of product
};

inline void set_remainder_constants(Chunk& c) {
    const std::uint64_t d = c.product;
    c.c1 = static_cast<std::uint64_t>((static_cast<u128>(1) << 64) % d);
    c.c2 = static_cast<std::uint64_t>((static_cast<u128>(c.c1) << 64) % d);
    c.shift = static_cast<unsigned>(__builtin_clzll(d));
    c.dn = d << c.shift;
    c.v = static_cast<std::uint64_t>(~static_cast<u128>(0) / c.dn - (static_cast<u128>(1) << 64));
}

/*
 * x mod d for x = n2*B^2 + n1*B + n0 (B = 2^64) and d = c.product >= 2, by
 * folding (see the references above and docs/optimisation_notes.md, 19/10/26):
 *   1. T = (n1:n0) + n2*c2 == x (mod d). As n2*c2 <= (B-1)(d-1), T < B^2 + (B-1)(d-1).
 *      On a carry out of B^2 (== c2 mod d), T' = T - B^2 <= (B-1)(d-1) - 1 and
 *      T' + c2 < B*d <= B^2, so adding c2 back cannot carry again.
 *   2. U = th*c1 + tl == T, and U <= (B-1)(d-1) + (B-1) = (B-1)d < B*d, so uh < d.
 *   3. One Moller-Granlund 2-by-1 step (Alg. 4) on U * 2^shift < B*dn, whose
 *      high limb is therefore < dn as the step requires; shift the remainder back.
 * Critical path: about four multiplications, against six for three 2-by-1 steps.
 */
inline std::uint64_t remainder_3(std::uint64_t n0, std::uint64_t n1, std::uint64_t n2, const Chunk& c) {
    const u128 low = (static_cast<u128>(n1) << 64) | n0;
    u128 t = low + static_cast<u128>(n2) * c.c2;
    const std::uint64_t carry = t < low;
    t += static_cast<u128>(-carry & c.c2);
    const u128 u = static_cast<u128>(static_cast<std::uint64_t>(t >> 64)) * c.c1 + static_cast<std::uint64_t>(t);
    std::uint64_t uh = static_cast<std::uint64_t>(u >> 64), ul = static_cast<std::uint64_t>(u);
    if (c.shift != 0) {
        uh = (uh << c.shift) | (ul >> (64 - c.shift));
        ul <<= c.shift;
    }
    // 2-by-1 step (uh < dn, dn normalised): candidate quotient, then two adjustments
    const u128 q = static_cast<u128>(uh) * c.v + ((static_cast<u128>(uh + 1) << 64) | ul);
    const std::uint64_t qh = static_cast<std::uint64_t>(q >> 64), ql = static_cast<std::uint64_t>(q);
    std::uint64_t r = ul - qh * c.dn;
    r += -static_cast<std::uint64_t>(r > ql) & c.dn;
    if (__builtin_expect(r >= c.dn, 0)) r -= c.dn;
    return r >> c.shift;
}

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
    for (Chunk& c : table.chunks) {
        set_remainder_constants(c);
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

// Checks every prime's and chunk's constants, and remainder_3 on edge-case
// inputs against plain 128-bit arithmetic (called once at startup, also in
// release builds). Returns false if anything is wrong.
inline bool verify(const Table& table) {
    for (const PrimeTest& t : table.tests) {
        if (static_cast<std::uint64_t>(t.prime) * t.inverse != 1) return false;
        if (t.limit != std::numeric_limits<std::uint64_t>::max() / t.prime) return false;
    }
    constexpr std::uint64_t MAX_U64 = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t limbs[] = {0, 1, 2, 0x5555555555555555, 1ull << 63, MAX_U64 - 1, MAX_U64};
    for (const Chunk& c : table.chunks) {
        Chunk expect = c;
        set_remainder_constants(expect);
        if (c.c1 != expect.c1 || c.c2 != expect.c2 || c.dn != expect.dn || c.v != expect.v || c.shift != expect.shift) {
            return false;
        }
        for (std::uint64_t n2 : limbs) {
            for (std::uint64_t n1 : limbs) {
                for (std::uint64_t n0 : limbs) {
                    // Reference: Horner's rule with 128-bit remainders
                    const u128 top = ((static_cast<u128>(n2 % c.product) << 64) | n1) % c.product;
                    const std::uint64_t want = static_cast<std::uint64_t>(((top << 64) | n0) % c.product);
                    if (remainder_3(n0, n1, n2, c) != want) return false;
                }
            }
        }
    }
    return true;
}

} // namespace lpr::trial_division
