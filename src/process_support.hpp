/**
 * process_support.hpp: the consumer side. For one support (job), enumerates the
 * candidates p = Q*d + 1 not covered by Corollary 4 and runs each through
 * trial division, the base-2 Fermat test and the least primitive root search,
 * then checks Grosswald's inequality (g(p) + 2)^2 < p.
 **/

#pragma once

#include "job_queue.hpp"
#include "logging.hpp"
#include "mont3.hpp"
#include "primality.hpp"
#include "stats.hpp"

#include <gmpxx.h>

#include <cassert>
#include <optional>
#include <sstream>

namespace lpr::consumer {

/**
 * Checks every prime p = Q*d + 1 with 1 <= d <= d_max whose prime factors all
 * lie in the support (otherwise omega(p - 1) > omega). Larger d are covered by
 * Corollary 4 (d_max is computed by the producer).
 **/
inline void process_support(const lpr::parallel::LPRJob& job) {
    const unsigned* support = job.support.data();
    const std::size_t omega = job.omega;
    mpz_t q_storage;
    const mpz_srcptr Q = job.Q(q_storage); // Read-only view of the job's Q
    assert(support[0] == 2);

    auto& stats = lpr::stats::get_thread_local_stats();
    ++stats.total_support_count;

    // Thread-local scratch values, reused to avoid GMP allocations
    static thread_local mpz_class p;
    static thread_local mpz_class n;
    static thread_local mpz_class g_plus_two_squared;
    // Montgomery context for p, shared by the Fermat test and the power tree
    static thread_local lpr::mont3::Context mont_context;

    for (unsigned d = 1; d <= job.d_max; ++d) {
        // Strip from d all prime factors that belong to the support; anything
        // left over is a new prime factor, so omega(Q*d) > omega
        unsigned remaining_factor = d;
        for (std::size_t i = 0; i < omega; ++i) {
            const unsigned q = support[i];
            while (remaining_factor % q == 0) {
                remaining_factor /= q;
            }
            if (remaining_factor == 1) {
                break;
            }
        }
        if (remaining_factor != 1) {
            continue;
        }

        mpz_mul_ui(n.get_mpz_t(), Q, d); // n = p - 1 = Q*d
        mpz_add_ui(p.get_mpz_t(), n.get_mpz_t(), 1);
        ++stats.candidate_count;

        if (lpr::primality::is_composite_small_prime(p)) {
            ++stats.small_prime_composite_count;
            continue;
        }

        // Set up only after trial division, which removes many candidates cheaply.
        // If p is too large for 3-limb Montgomery arithmetic, both tests use GMP.
        const lpr::mont3::Context* mont = nullptr;
        if (lpr::mont3::setup(mont_context, p.get_mpz_t())) {
            mont = &mont_context;
        } else {
            ++stats.mont3_fallback_count;
        }

        if (lpr::primality::is_composite_base2_fermat(p, n, mont)) {
            ++stats.fermat_composite_count;
            continue;
        }
        ++stats.fermat_survivor_count;

        const std::optional<unsigned> g = lpr::primality::find_least_primitive_root(p, d, support, omega, mont);
        if (!g) {
            ++stats.unresolved_count;
            std::ostringstream msg;
            msg << "Unresolved candidate p = " << p;
            lpr::log::warn(msg.str());
            continue;
        }

        ++stats.certified_count;
        ++stats.winners_by_g[*g];
        if (*g > stats.largest_least_primitive_root) {
            stats.largest_least_primitive_root = *g;
        }

        // Grosswald's inequality: (g + 2)^2 < p
        mpz_set_ui(g_plus_two_squared.get_mpz_t(), *g + 2);
        mpz_mul(g_plus_two_squared.get_mpz_t(), g_plus_two_squared.get_mpz_t(), g_plus_two_squared.get_mpz_t());
        if (g_plus_two_squared < p) {
            ++stats.certified_inequality_pass_count;
        } else {
            ++stats.certified_inequality_fail_count;
            std::ostringstream msg;
            msg << "Grosswald inequality failure: p = " << p << ", g(p) = " << *g;
            lpr::log::warn(msg.str());
        }
    }
}

/*
 * Consumer loop: processes batches from the queue until it is finished and
 * empty. Statistics are published at most once per STATS_PUBLISH_INTERVAL_MS.
 */
template <typename QueueType>
void start_consumer(QueueType& queue) {
    lpr::parallel::LPRBatch batch;
    while (queue.pop(batch)) {
        for (const auto& job : batch) {
            process_support(job);
        }
        lpr::stats::maybe_publish_thread_local_stats();
    }
}

} // namespace lpr::consumer
