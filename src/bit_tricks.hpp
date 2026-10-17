/**
 * bit_tricks.hpp: small word-level helpers shared by trial_division.hpp and
 * mont3.hpp.
 **/

#pragma once

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

static_assert(inverse_mod_2_64(3) * 3 == 1, "inverse_mod_2_64 self-check");
static_assert(inverse_mod_2_64(0xFFFFFFFFFFFFFFFFull) * 0xFFFFFFFFFFFFFFFFull == 1,
              "inverse_mod_2_64 self-check");

} // namespace lpr::bits
