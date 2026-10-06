#pragma once

#include <cstdint>
#include <cstddef>

namespace parameters {
    // See paper for details on how to use parameters:
    inline constexpr std::size_t OMEGA = 32;
    inline constexpr std::size_t TREE_SIZE = 2*(OMEGA - 1) - 1; // For finding least primitive roots
    inline constexpr unsigned S = OMEGA - 3; // For corollary 4 - default for now to OMEGA-3

    // WARNING: PRIME_LIMIT is part of the exhaustive-search guarantee.
    // It must be large enough to include every prime that can occur in a
    // K-prime support with product <= bound. This is checked at runtime in the main function.
    inline constexpr unsigned PRIME_LIMIT = 10'000'000;

    // Upper bound for testing 2, 3, ..., PRIMITIVE_ROOT_SEARCH_LIMIT
    // as least primitive root g(p) candidates.
    // WARNING: Increasing this substantially may greatly increase runtime.
    inline constexpr unsigned PRIMITIVE_ROOT_SEARCH_LIMIT = 1000;

    // Search bound B = 1.8 × 10^54 = 18 × 10^53.
    inline constexpr unsigned BOUND_MULTIPLIER = 18;
    inline constexpr unsigned BOUND_EXPONENT = 53;

    inline constexpr unsigned long long PRIMORIAL_CHUNKS[] = {
        //16294579238595022365ULL,
        //7145393598349078859ULL,
        6408001374760705163ULL,
        690862709424854779ULL,
        4312024209383942993ULL,
        //71235931512604841ULL,
        //192878245514479103ULL,
        //542676746453092519ULL,
        //1230544604996048471ULL,
        //2618501576975440661ULL,
        //4771180125133726009ULL,
        //9247077179230889629ULL
    };
    inline constexpr size_t NUM_CHUNKS = sizeof(PRIMORIAL_CHUNKS) / sizeof(PRIMORIAL_CHUNKS[0]);

    /* Hysteresis Queue Constants */
    inline constexpr size_t BATCH_SIZE = 256;
    inline constexpr size_t HWM = 512;
    inline constexpr size_t LWM = 128;
}

