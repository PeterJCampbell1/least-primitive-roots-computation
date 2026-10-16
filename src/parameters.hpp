#pragma once

#include <cstdint>
#include <cstddef>

namespace parameters {
    // See paper for details on how to use parameters:
    // OMEGA and S are chosen at runtime (see config.hpp: --config FILE, --omega, --s).
    // DEFAULT_OMEGA is used when neither a config file nor --omega gives one, and S
    // defaults to OMEGA - 3. MAX_OMEGA (34, from the mathematics) bounds OMEGA and
    // sizes the fixed-size per-depth arrays.
    inline constexpr std::size_t DEFAULT_OMEGA = 33;
    inline constexpr std::size_t MAX_OMEGA = 34;
    inline constexpr std::size_t MAX_TREE_SIZE = 2*(MAX_OMEGA - 1) - 1; // Product tree over the odd primes

    // The prime list is generated at runtime up to floor(B / p_(omega-1)#), the
    // largest prime that can occur in a support with product <= B (see main).
    // MAX_PRIME_LIMIT caps it: primes are stored as unsigned, and the sieve needs
    // about limit/8 bytes (plus 4 bytes per prime), i.e. ~1.3 GB at the cap.
    inline constexpr unsigned MAX_PRIME_LIMIT = 4'000'000'000u;

    // Upper bound for testing 2, 3, ..., PRIMITIVE_ROOT_SEARCH_LIMIT
    // as least primitive root g(p) candidates.
    // WARNING: Increasing this substantially may greatly increase runtime.
    inline constexpr unsigned PRIMITIVE_ROOT_SEARCH_LIMIT = 1000;

    // The search bound is no longer a parameter: it is computed at runtime as
    // the Corollary 4 bound for the first OMEGA primes (corollary4_global_bound).

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

    /* Job queue constants: jobs per batch, and maximum queued batches before
       the producer processes batches itself (producer co-processing) */
    inline constexpr size_t BATCH_SIZE = 256;
    inline constexpr size_t HWM = 512;
}

