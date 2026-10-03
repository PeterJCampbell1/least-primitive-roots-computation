#pragma once

#include <cstdint>
#include <cstddef>

namespace parameters {
    // See paper for details on how to use parameters:
    inline constexpr std::size_t OMEGA = 32;
    inline constexpr unsigned S = 29; // For corollary 4

    // WARNING: PRIME_LIMIT is part of the exhaustive-search guarantee.
    // It must be large enough to include every prime that can occur in a
    // K-prime support with product <= bound. This is checked at runtime below.
    inline constexpr unsigned PRIME_LIMIT = 10'000'000;

    // Upper bound for testing 2, 3, ..., PRIMITIVE_ROOT_SEARCH_LIMIT
    // as least primitive root g(p) candidates.
    // WARNING: Increasing this substantially may greatly increase runtime.
    inline constexpr unsigned PRIMITIVE_ROOT_SEARCH_LIMIT = 1000;

    // Search bound B = 1.8 × 10^54 = 18 × 10^53.
    inline constexpr unsigned BOUND_MULTIPLIER = 18;
    inline constexpr unsigned BOUND_EXPONENT = 53;
}

