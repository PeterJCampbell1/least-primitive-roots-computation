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

    // Trial division before the Fermat test: every prime in [MIN, MAX], tested with
    // Granlund-Montgomery divisibility checks (tables built at runtime, see
    // trial_division.hpp). Small primes are usually in the support (then p == 1
    // mod them, so they never divide p), but supports can skip one and the checks
    // are cheap, so they are included. MIN must be odd (>= 3). Tuned at omega = 33
    // (see optimisation notes, 16/10/26): gains flatten beyond ~1151.
    inline constexpr unsigned TRIAL_DIVISION_MIN_PRIME = 3;
    inline constexpr unsigned TRIAL_DIVISION_MAX_PRIME = 1151;
    inline constexpr std::size_t MAX_TRIAL_DIVISION_CHUNKS = 128; // Sizes the statistics array

    /* Job queue constants: jobs per batch, and maximum queued batches before
       the producer processes batches itself (producer co-processing) */
    inline constexpr size_t BATCH_SIZE = 256;
    // Limbs stored inline for a support's product Q in each job (Q <= B; checked in main)
    inline constexpr size_t MAX_Q_LIMBS = 4;
    // Each thread merges its statistics into the shared totals at most this often
    inline constexpr unsigned STATS_PUBLISH_INTERVAL_MS = 1000;
    inline constexpr size_t HWM = 512;

    // Work stealing (work_stealing.hpp): an idle worker whose steal requests keep
    // failing yields a few times, then sleeps 1 us, doubling up to this cap
    inline constexpr unsigned STEAL_MAX_BACKOFF_US = 1000;
}

