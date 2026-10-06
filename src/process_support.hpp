#pragma once

#include "corollary4.hpp"
#include "hysteresis_queue.hpp"
#include "stats.hpp"

#include <gmpxx.h>
#include <vector>
#include <iostream>
#include <cassert>

namespace lpr::consumer {

/**
 * For a given support, processes to determine if any primes may violate 
 * Grosswald's inequality.
 **/
inline void process_support(
    const lpr::parallel::LPRJob& job
    /*
    const std::vector<unsigned>& support,
    const unsigned d_max, 
    const mpz_class& Q
    */
) {
    const std::vector<unsigned>& support = job.support;
    const unsigned d_max = job.d_max;
    const mpz_class& Q = job.Q;

    assert(support[0] == 2);
    auto& stats = lpr::stats::get_thread_local_stats();
    ++stats.total_support_count;
    ++stats.cor4_proves_grosswald_count;

    
    // Reusable memory for heap allocated objects only
    // mpz_class calls the automatic constructor/destructor if defined locally
    //static thread_local mpz_class m;
    static thread_local mpz_class p;
    static thread_local mpz_class n;
    static thread_local mpz_class g_plus_two;
    static thread_local mpz_class g_plus_two_sqr;

    for (unsigned d = 1; d <= d_max; ++d) { // Propagate d = 1 down to Fermat checks
        unsigned remaining_factor = d;

        // Strip from d all prime factors that belong to the support.
        for (unsigned q : support) {
            while (remaining_factor % q == 0) {
                remaining_factor /= q;
            }

            if (remaining_factor == 1) {
                break;
            }
        }
        
        // If remaining_factor != 1, then d contains a prime factor
        // not already in the support, so d * Q has more than omega distinct prime factors.
        if (remaining_factor == 1) {
            n = Q * d;
            p = n + 1;
            ++stats.cor4_proves_grosswald_count;

            /* Already know this is true for d <= d_max now
            if (corollary4_proves_grosswald(d-prev_d)) {
                prev_d = d;
                if (stats.max_d < d) {
                    stats.max_d = d;
                }
                break;
            }
            */
            //prev_d = d;
            ++stats.candidate_count;

            if (is_composite_small_prime(p)) {
                ++stats.small_prime_composite_count;
            } else if (is_composite_base2_fermat(p, n)) {
            //} else if (is_composite_base2_miller_rabin(p, n)) {
                ++stats.fermat_composite_count;
            } else {
                ++stats.fermat_survivor_count;

                const auto g = find_least_primitive_root(
                    p, d, support
                );
                if (g) {
                    ++stats.certified_count;

                    if (*g > stats.largest_least_primitive_root) {
                        stats.largest_least_primitive_root = *g;
                    }

                    g_plus_two = *g + 2;
                    mpz_mul(g_plus_two_sqr.get_mpz_t(), g_plus_two.get_mpz_t(), g_plus_two.get_mpz_t());
                    if (g_plus_two_sqr < p) {
                        ++stats.certified_inequality_pass_count;
                    } else {
                        ++stats.certified_inequality_fail_count;
                        std::cout << "Grosswald inequality failure: p = "
                                  << p
                                  << ", g(p) = "
                                  << *g
                                  << '\n';
                    }
                } else {
                    ++stats.unresolved_count;
                    std::cout << "Unresolved candidate p = "
                              << p
                              << '\n';
                }
            }
        }
    }

    return;
} // process_support

/*
 * Wrapper around process_support that automatically handles interacting
 * with the concurrent queue.
 */
template <typename QueueType>
void start_consumer(QueueType& queue) {
    using parallel::LPRBatch;
    LPRBatch batch;
    while (queue.pop(batch)) {
        for (auto& job : batch) {
            process_support(job);
        }
    }
}

} // lpr::consumer

