/**
 * mont3.hpp: fixed-size Montgomery arithmetic modulo an odd p < 2^190, held in
 * three 64-bit limbs. Experimental (not part of the main build).
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
 **/

#pragma once

#include <gmp.h>

#include <cstddef>
#include <cstdint>

static_assert(GMP_LIMB_BITS == 64, "mont3 assumes 64-bit GMP limbs");

namespace lpr::mont3 {

using u64 = std::uint64_t;
using u128 = unsigned __int128;
constexpr int N = 3; // Limbs

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

// a -= b (requires a >= b)
inline void sub_in(Num& a, const Num& b) {
    u128 d = (u128)a.w[0] - b.w[0];
    a.w[0] = (u64)d;
    u64 borrow = (u64)(d >> 64) & 1;
    d = (u128)a.w[1] - b.w[1] - borrow;
    a.w[1] = (u64)d;
    borrow = (u64)(d >> 64) & 1;
    a.w[2] = a.w[2] - b.w[2] - borrow;
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
inline Num mul(const Num& a, const Num& b, const Context& c) {
    u64 t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    for (int i = 0; i < N; ++i) {
        const u64 bi = b.w[i];
        // t += a * b[i]
        u128 s = (u128)a.w[0] * bi + t0;
        t0 = (u64)s;
        s = (u128)a.w[1] * bi + t1 + (u64)(s >> 64);
        t1 = (u64)s;
        s = (u128)a.w[2] * bi + t2 + (u64)(s >> 64);
        t2 = (u64)s;
        s = (u128)t3 + (u64)(s >> 64);
        t3 = (u64)s;
        t4 = (u64)(s >> 64);
        // t += m * p, with m = t0 * (-p^{-1}) so the low limb cancels; then t >>= 64
        const u64 m = t0 * c.pinv;
        s = (u128)m * c.p.w[0] + t0; // Low 64 bits are zero by choice of m
        s = (u128)m * c.p.w[1] + t1 + (u64)(s >> 64);
        t0 = (u64)s;
        s = (u128)m * c.p.w[2] + t2 + (u64)(s >> 64);
        t1 = (u64)s;
        s = (u128)t3 + (u64)(s >> 64);
        t2 = (u64)s;
        t3 = t4 + (u64)(s >> 64);
    }
    return Num{{t0, t1, t2}}; // t3 == 0 because the result is < 2p < 2^192
}

/* ---------- Montgomery square ---------- */

// Returns a^2/R mod p, lazily reduced (input < 2p, output < 2p).
// Simply mul(a, a): on Apple M-series this measured as fast as a dedicated
// squaring with 6 instead of 9 limb products (sqr_unrolled in experimental/montgomery3), because the
// carry chains, not the multiplications, are the bottleneck. Re-measure on the
// cluster's CPUs before switching (experimental/montgomery3/README.md).
inline Num sqr(const Num& a, const Context& c) {
    return mul(a, a, c);
}

/* ---------- doubling ---------- */

// x -> 2x, lazily reduced: input < 2p, output < 2p (2x < 4p < 2^192 fits).
inline Num dbl(Num x, const Context& c) {
    x.w[2] = (x.w[2] << 1) | (x.w[1] >> 63);
    x.w[1] = (x.w[1] << 1) | (x.w[0] >> 63);
    x.w[0] = x.w[0] << 1;
    if (geq(x, c.p2)) sub_in(x, c.p2);
    return x;
}

// x -> 2x mod p, fully reduced: input < p, output < p (used in setup)
inline Num dbl_canonical(Num x, const Context& c) {
    x.w[2] = (x.w[2] << 1) | (x.w[1] >> 63);
    x.w[1] = (x.w[1] << 1) | (x.w[0] >> 63);
    x.w[0] = x.w[0] << 1;
    if (geq(x, c.p)) sub_in(x, c.p);
    return x;
}

/* ---------- conversion ---------- */

inline Num from_mpz(const mpz_t z) {
    Num n{{0, 0, 0}};
    const std::size_t size = mpz_size(z);
    const mp_limb_t* limbs = mpz_limbs_read(z);
    for (std::size_t i = 0; i < size && i < N; ++i) n.w[i] = limbs[i];
    return n;
}

inline void to_mpz(mpz_t out, const Num& n) {
    mp_limb_t* limbs = mpz_limbs_write(out, N);
    for (int i = 0; i < N; ++i) limbs[i] = n.w[i];
    int size = N;
    while (size > 0 && limbs[size - 1] == 0) --size;
    mpz_limbs_finish(out, size);
}

// -p^{-1} mod 2^64 by Newton's iteration (see trial_division.hpp)
inline u64 neg_inverse(u64 p0) {
    u64 x = p0;
    for (int i = 0; i < 5; ++i) x *= 2 - p0 * x;
    return (u64)0 - x;
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
    c.p2 = c.p;
    c.p2.w[2] = (c.p2.w[2] << 1) | (c.p2.w[1] >> 63);
    c.p2.w[1] = (c.p2.w[1] << 1) | (c.p2.w[0] >> 63);
    c.p2.w[0] = c.p2.w[0] << 1;
    c.pinv = neg_inverse(c.p.w[0]);

    // R mod p
    const int bits = static_cast<int>(mpz_sizeinbase(p_mpz, 2));
    const int k = bits - 1; // 2^k < p because p is odd and > 1
    Num x{{0, 0, 0}};
    x.w[k / 64] = (u64)1 << (k % 64);
    for (int i = k; i < 192; ++i) x = dbl_canonical(x, c);
    c.one = x;

    // R^2 mod p
    for (int i = 0; i < 6; ++i) x = dbl_canonical(x, c);
    for (int i = 0; i < 5; ++i) x = sqr(x, c);
    c.r2 = canonical(x, c);
    return true;
}

// Montgomery form of a small integer g (g < 2p): g*R mod p = REDC(g * R^2)
inline Num to_mont(u64 g, const Context& c) {
    return mul(Num{{g, 0, 0}}, c.r2, c);
}

// Ordinary residue x/R mod p of a Montgomery value (canonical)
inline Num from_mont(const Num& x, const Context& c) {
    return canonical(mul(x, Num{{1, 0, 0}}, c), c);
}

/* ---------- exponentiation ---------- */

inline int bit_length(const mp_limb_t* e, std::size_t n) {
    while (n > 0 && e[n - 1] == 0) --n;
    if (n == 0) return 0;
    return static_cast<int>(64 * (n - 1) + 64 - __builtin_clzll(e[n - 1]));
}

inline int bit(const mp_limb_t* e, int i) {
    return static_cast<int>((e[i / 64] >> (i % 64)) & 1);
}

/*
 * 2^e in Montgomery form (lazily reduced), for the base-2 Fermat test.
 * Left-to-right binary: square for every bit, and double when the bit is 1.
 * Doubling is a shift and a conditional subtraction, much cheaper than a
 * multiplication; in Montgomery form 2*(xR) = (2x)R, so it needs no conversion.
 */
inline Num pow2(const mp_limb_t* e, std::size_t n, const Context& c) {
    const int bits = bit_length(e, n);
    Num x = c.one;
    for (int i = bits - 1; i >= 0; --i) {
        x = sqr(x, c);
        if (bit(e, i)) x = dbl(x, c);
    }
    return x;
}

// Base-2 Fermat test: true if 2^(p-1) != 1 mod p (p proven composite)
inline bool fermat_composite(const mpz_t p_minus_1, const Context& c) {
    const Num x = canonical(pow2(mpz_limbs_read(p_minus_1), mpz_size(p_minus_1), c), c);
    return !equal(x, c.one);
}

// Window size for sliding-window exponentiation, by exponent length
inline int window_bits(int bits) {
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
    const int k = window_bits(bits);

    Num table[8]; // Odd powers base^(2i+1), i < 2^(k-1) <= 8
    table[0] = base;
    if (k > 1) {
        const Num base2 = sqr(base, c);
        for (int i = 1; i < (1 << (k - 1)); ++i) table[i] = mul(table[i - 1], base2, c);
    }

    Num x = c.one;
    bool started = false;
    int i = bits - 1;
    while (i >= 0) {
        if (!bit(e, i)) {
            if (started) x = sqr(x, c);
            --i;
            continue;
        }
        // Window [l, i]: at most k bits, lowest bit l is 1
        int l = i - k + 1;
        if (l < 0) l = 0;
        while (!bit(e, l)) ++l;
        unsigned value = 0;
        for (int j = i; j >= l; --j) value = (value << 1) | static_cast<unsigned>(bit(e, j));
        if (started) {
            for (int j = i; j >= l; --j) x = sqr(x, c);
            x = mul(x, table[value >> 1], c);
        } else {
            x = table[value >> 1]; // First window: no squarings of 1 needed
            started = true;
        }
        i = l - 1;
    }
    return x;
}

} // namespace lpr::mont3
