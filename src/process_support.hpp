#pragma once

#include "corollary4.hpp"
#include "stats.hpp"

#include <gmpxx.h>
#include <vector>
#include <iostream>
#include <cassert>

/**
 * For a given support, processes to determine if any primes may violate 
 * Grosswald's inequality.
 **/
inline void process_support(
    const std::vector<unsigned>& support,
    const mpz_class& bound, 
    const mpz_class& Q
) {
    assert(support[0] == 2);
    auto& stats = get_thread_local_stats();
    ++stats.total_support_count;
    ++stats.cor4_proves_grosswald_count;

    
    // Reusable memory for heap allocated objects only
    // mpz_class calls the automatic constructor/destructor if defined locally
    static thread_local mpz_class m;
    static thread_local mpz_class p;
    static thread_local mpz_class n;
    static thread_local mpz_class g_plus_two;
    static thread_local mpz_class g_plus_two_sqr;

    // NOTE: We assume m fits within a 64 bit integer
    m = bound / Q; 
    // TODO - Ask Peter, is there even a point in this bound, wouldn't
    //  the local bound via proves_grosswald stop us from ever exceeding m
    //  anyway? Probably useful to not compute an extra value of d each time
    //  though.
    assert(m.fits_ulong_p() && "m exceeds 64-bit integer range");

    unsigned prev_d = 1;
    unsigned d = 0;
    for (d = 1; d <= m.get_ui(); ++d) { // Propagate d = 1 down to Fermat checks
    //while (true) {
        //d++;
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
            if (corollary4_proves_grosswald(d-prev_d)) {
                prev_d = d;
                if (stats.max_d < d) {
                    stats.max_d = d;
                }
                break;
            }
            prev_d = d;
            ++stats.candidate_count;

            if (is_composite_small_prime(p, stats)) {
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


