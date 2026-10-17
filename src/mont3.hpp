/**
 * mont3.hpp: fixed-size Montgomery arithmetic modulo an odd p < 2^190, held in
 * three 64-bit limbs.
 *
 * Used for the base-2 Fermat test and the power tree of the least primitive
 * root search (primality.cpp); GMP is the fallback for moduli it does not
 * support. Tested against GMP in experimental/montgomery3 (`make test` there),
 * and debug builds of lpr cross-check every result against GMP.
 *
 * Montgomery form: with R = 2^192, a residue x is represented by xR mod p.
 * The Montgomery product of aR and bR is (aR)(bR)/R = abR (mod p), computed by
 * REDC without any division by p.
 *
 * Lazy reduction: every value is kept in [0, 2p) instead of [0, p). This is
 * valid because 4p < R (p < 2^190): if a, b < 2p then
 *     (a*b + m*p) / R < (4p^2 + R*p) / R = p * (4p/R + 1) < 2p,
 * so a Montgomery product of two lazily reduced values is again < 2p and fits
 * in three limbs. Values are made canonical (< p) only when compared or output.
 *
 * Requirements: p odd, 3 <= p < 2^190 (setup() checks and returns false
 * otherwise; callers should then fall back to GMP).
 *
 * All arithmetic is written out by hand for exactly three limbs (N == 3).
 **/

#pragma once

#include "bit_tricks.hpp"

#include <gmp.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

static_assert(GMP_LIMB_BITS == 64, "mont3 assumes 64-bit GMP limbs");

namespace lpr::mont3 {

using u64 = std::uint64_t;
__extension__ typedef unsigned __int128 u128; // GCC/Clang extension (no -Wpedantic warning)
constexpr int N = 3; // Limbs
static_assert(N == 3, "mont3 is hand-unrolled for exactly 3 limbs");

// Low and high 64-bit halves of a 128-bit value
inline u64 lo(u128 x) { return static_cast<u64>(x); }
inline u64 hi(u128 x) { return static_cast<u64>(x >> 64); }

// A 192-bit value, little-endian limbs: value = w[0] + w[1]*2^64 + w[2]*2^128
struct Num {
    u64 w[N];
};

struct Context {
    Num p;     // The modulus
    Num p2;    // 2p (lazy reduction bound)
    u64 pinv;  // -p^{-1} mod 2^64
    Num one;   // R mod p: Montgomery form of 1 (canonical)
    Num r2;    // R^2 mod p: converts into Montgomery form (canonical)
};

/* ---------- 3-limb helpers ---------- */

inline bool geq(const Num& a, const Num& b) {
    if (a.w[2] != b.w[2]) return a.w[2] > b.w[2];
    if (a.w[1] != b.w[1]) return a.w[1] > b.w[1];
    return a.w[0] >= b.w[0];
}

inline bool equal(const Num& a, const Num& b) {
    return a.w[0] == b.w[0] && a.w[1] == b.w[1] && a.w[2] == b.w[2];
}

// a -= b (requires a >= b). A borrow out of a limb wraps the 128-bit
// difference, setting bit 64, which is exactly the borrow into the next limb.
inline void sub_in(Num& a, const Num& b) {
    u128 d = static_cast<u128>(a.w[0]) - b.w[0];
    a.w[0] = lo(d);
    u64 borrow = hi(d) & 1;
    d = static_cast<u128>(a.w[1]) - b.w[1] - borrow;
    a.w[1] = lo(d);
    borrow = hi(d) & 1;
    a.w[2] = a.w[2] - b.w[2] - borrow; // No borrow out, as a >= b
}

// x << 1 (requires x < 2^191, so no bit is lost)
inline Num shl1(Num x) {
    x.w[2] = (x.w[2] << 1) | (x.w[1] >> 63);
    x.w[1] = (x.w[1] << 1) | (x.w[0] >> 63);
    x.w[0] = x.w[0] << 1;
    return x;
}

// Reduce a value in [0, 2p) to [0, p)
inline Num canonical(Num a, const Context& c) {
    if (geq(a, c.p)) sub_in(a, c.p);
    return a;
}

/* ---------- Montgomery product (CIOS) ---------- */

// Returns a*b/R mod p, lazily reduced. Inputs < 2p, output < 2p.
// Coarsely Integrated Operand Scanning: for each limb b[i], add a*b[i] to the
// accumulator t, then add m*p with m chosen so the lowest limb becomes zero,
// and shift t down one limb (dividing by 2^64). After 3 rounds t = (ab + Mp)/R.
// Each 128-bit sum holds one product, one limb and one carry, and
// (2^64 - 1)^2 + 2(2^64 - 1) = 2^128 - 1, so it cannot overflow.
inline Num mul(const Num& a, const Num& b, const Context& c) {
    u64 t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    for (int i = 0; i < N; ++i) {
        const u64 bi = b.w[i];
        // t += a * b[i]
        u128 s = static_cast<u128>(a.w[0]) * bi + t0;
        t0 = lo(s);
        s = static_cast<u128>(a.w[1]) * bi + t1 + hi(s);
        t1 = lo(s);
        s = static_cast<u128>(a.w[2]) * bi + t2 + hi(s);
        t2 = lo(s);
        s = static_cast<u128>(t3) + hi(s);
        t3 = lo(s);
        t4 = hi(s);
        // Between rounds t < 3p, so t + a*b[i] < 2^256: t4 is always zero
        assert(t4 == 0);
        // t += m * p, with m = t0 * (-p^{-1}) so the low limb cancels; then t >>= 64.
        // Only the carry of the lowest product is kept: its low half is zero.
        const u64 m = t0 * c.pinv;
        s = static_cast<u128>(m) * c.p.w[0] + t0;
        s = static_cast<u128>(m) * c.p.w[1] + t1 + hi(s);
        t0 = lo(s);
        s = static_cast<u128>(m) * c.p.w[2] + t2 + hi(s);
        t1 = lo(s);
        s = static_cast<u128>(t3) + hi(s);
        t2 = lo(s);
        t3 = t4 + hi(s);
    }
    assert(t3 == 0); // The result is < 2p < 2^192
    return Num{{t0, t1, t2}};
}

/* ---------- Montgomery square ---------- */

// Returns a^2/R mod p, lazily reduced (input < 2p, output < 2p).
// Simply mul(a, a): on Apple M-series this measured as fast as a dedicated
// squaring with 6 instead of 9 limb products (sqr_unrolled in
// experimental/montgomery3), because the carry chains, not the multiplications,
// are the bottleneck. Re-measure on the cluster's CPUs before switching.
inline Num sqr(const Num& a, const Context& c) {
    return mul(a, a, c);
}

/* ---------- doubling ---------- */

// x -> 2x, lazily reduced: input < 2p, output < 2p (2x < 4p < 2^192 fits).
inline Num dbl(Num x, const Context& c) {
    x = shl1(x);
    if (geq(x, c.p2)) sub_in(x, c.p2);
    return x;
}

// x -> 2x mod p, fully reduced: input < p, output < p (used in setup)
inline Num dbl_canonical(Num x, const Context& c) {
    x = shl1(x);
    if (geq(x, c.p)) sub_in(x, c.p);
    return x;
}

/* ---------- conversion ---------- */

// Copies a non-negative value of at most 3 limbs
inline Num from_mpz(const mpz_t z) {
    const std::size_t size = mpz_size(z);
    assert(mpz_sgn(z) >= 0 && size <= static_cast<std::size_t>(N));
    const mp_limb_t* limbs = mpz_limbs_read(z);
    Num n{{0, 0, 0}};
    for (std::size_t i = 0; i < size; ++i) n.w[i] = limbs[i];
    return n;
}

inline void to_mpz(mpz_t out, const Num& n) {
    mp_limb_t* limbs = mpz_limbs_write(out, N);
    for (int i = 0; i < N; ++i) limbs[i] = n.w[i];
    int size = N;
    while (size > 0 && limbs[size - 1] == 0) --size;
    mpz_limbs_finish(out, size);
}

/*
 * Prepares the context for modulus p, without dividing by p.
 *   R mod p: start from 2^k < p (k = bit length - 1) and double 192 - k times,
 *            reducing fully each time (about 12 steps for a 181-bit p).
 *   R^2 mod p: double R mod p six times to get R*2^6, then square five times
 *            in Montgomery form (each square maps R*2^j to R*2^(2j)):
 *            2^6 -> 2^12 -> 2^24 -> 2^48 -> 2^96 -> 2^192, i.e. R*R.
 * Returns false (context unusable) unless p is odd and 3 <= p < 2^190.
 */
inline bool setup(Context& c, const mpz_t p_mpz) {
    if (mpz_size(p_mpz) > static_cast<std::size_t>(N) || mpz_even_p(p_mpz)
        || mpz_cmp_ui(p_mpz, 3) < 0) {
        return false;
    }
    c.p = from_mpz(p_mpz);
    if (c.p.w[2] >> 62) return false; // Need p < 2^190, so that 4p < R
    c.p2 = shl1(c.p);
    c.pinv = static_cast<u64>(0) - lpr::bits::inverse_mod_2_64(c.p.w[0]);

    // R mod p
    const int bits = static_cast<int>(mpz_sizeinbase(p_mpz, 2));
    const int k = bits - 1; // 2^k < p because p is odd and > 1
    Num x{{0, 0, 0}};
    x.w[k / 64] = static_cast<u64>(1) << (k % 64);
    for (int i = k; i < 192; ++i) x = dbl_canonical(x, c);
    c.one = x;

    // R^2 mod p
    for (int i = 0; i < 6; ++i) x = dbl_canonical(x, c);
    for (int i = 0; i < 5; ++i) x = sqr(x, c);
    c.r2 = canonical(x, c);
    return true;
}

// Montgomery form of a small integer g (requires g < 2p): g*R mod p = REDC(g * R^2)
inline Num to_mont(u64 g, const Context& c) {
    assert(c.p.w[1] != 0 || c.p.w[2] != 0 || (g >> 1) < c.p.w[0]); // g < 2p, without overflow
    return mul(Num{{g, 0, 0}}, c.r2, c);
}

// Ordinary residue x/R mod p of a Montgomery value (canonical)
inline Num from_mont(const Num& x, const Context& c) {
    return canonical(mul(x, Num{{1, 0, 0}}, c), c);
}

/* ---------- exponentiation ---------- */

// Number of significant bits of the n-limb exponent e (0 for e == 0)
inline int bit_length(const mp_limb_t* e, std::size_t n) {
    while (n > 0 && e[n - 1] == 0) --n;
    if (n == 0) return 0;
    return static_cast<int>(64 * (n - 1) + 64 - __builtin_clzll(e[n - 1]));
}

// Bit i of the n-limb exponent e
inline int bit(const mp_limb_t* e, std::size_t n, int i) {
    assert(i >= 0 && static_cast<std::size_t>(i) < 64 * n);
    (void)n;
    return static_cast<int>((e[i / 64] >> (i % 64)) & 1);
}

// Bits [low, low + width) of the n-limb exponent e (width <= 8), as an integer
inline unsigned bits_at(const mp_limb_t* e, std::size_t n, int low, int width) {
    assert(low >= 0 && width >= 1 && width <= 8 && static_cast<std::size_t>(low + width) <= 64 * n);
    const int limb = low / 64, shift = low % 64;
    u64 value = e[limb] >> shift;
    if (shift + width > 64 && static_cast<std::size_t>(limb + 1) < n) {
        value |= e[limb + 1] << (64 - shift); // The window crosses into the next limb
    }
    return static_cast<unsigned>(value & ((1u << width) - 1));
}

/*
 * 2^e in Montgomery form (lazily reduced), for the base-2 Fermat test.
 * Left-to-right binary: square for every bit, and double when the bit is 1.
 * Doubling is a shift and a conditional subtraction, much cheaper than a
 * multiplication; in Montgomery form 2*(xR) = (2x)R, so it needs no conversion.
 * The top bit is always 1, so start from 2 = dbl(one) at the next bit.
 */
inline Num pow2(const mp_limb_t* e, std::size_t n, const Context& c) {
    const int bits = bit_length(e, n);
    if (bits == 0) return c.one;
    Num x = dbl(c.one, c);
    for (int i = bits - 2; i >= 0; --i) {
        x = sqr(x, c);
        if (bit(e, n, i)) x = dbl(x, c);
    }
    return x;
}

// Base-2 Fermat test: true if 2^(p-1) != 1 mod p (p proven composite)
inline bool fermat_composite(const mpz_t p_minus_1, const Context& c) {
    const Num x = canonical(pow2(mpz_limbs_read(p_minus_1), mpz_size(p_minus_1), c), c);
    return !equal(x, c.one);
}

// Window size for sliding-window exponentiation, by exponent length
inline int window_size(int bits) {
    if (bits <= 8) return 1;
    if (bits <= 24) return 2;
    if (bits <= 80) return 3;
    return 4;
}

/*
 * base^e for a Montgomery-form base (lazily reduced in, lazily reduced out).
 * Sliding window: precompute the odd powers base^1, base^3, ..., base^(2^k - 1);
 * scan the exponent from the top, squaring once per bit, and at each 1 bit take
 * the longest window of at most k bits that ends in a 1, multiplying by the
 * matching odd power. This needs about bits squarings and bits/(k+1) products.
 */
inline Num powm(const Num& base, const mp_limb_t* e, std::size_t n, const Context& c) {
    const int bits = bit_length(e, n);
    if (bits == 0) return c.one;
    const int k = window_size(bits);

    Num odd_powers[8]; // odd_powers[i] = base^(2i+1), i < 2^(k-1) <= 8
    odd_powers[0] = base;
    if (k > 1) {
        const Num base_squared = sqr(base, c);
        for (int i = 1; i < (1 << (k - 1)); ++i) {
            odd_powers[i] = mul(odd_powers[i - 1], base_squared, c);
        }
    }

    Num x = c.one;
    bool started = false;
    int i = bits - 1;
    while (i >= 0) {
        if (!bit(e, n, i)) {
            if (started) x = sqr(x, c);
            --i;
            continue;
        }
        // Window [window_low, i]: at most k bits, and its lowest bit is 1
        int window_low = i - k + 1;
        if (window_low < 0) window_low = 0;
        while (!bit(e, n, window_low)) ++window_low;
        const int width = i - window_low + 1;
        const unsigned window_value = bits_at(e, n, window_low, width); // Odd, < 2^k
        if (started) {
            for (int j = 0; j < width; ++j) x = sqr(x, c);
            x = mul(x, odd_powers[window_value >> 1], c);
        } else {
            x = odd_powers[window_value >> 1]; // First window: no squarings of 1 needed
            started = true;
        }
        i = window_low - 1;
    }
    return x;
}

} // namespace lpr::mont3
