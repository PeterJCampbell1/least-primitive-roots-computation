#include <gmpxx.h>
#include "corollary4.hpp"
#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"

// Analysis Tool
//#include "analysis.hpp"

#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <cassert>

// Anonymous namespace - restricts type visibility to this file
namespace {

struct StackFrame {
    std::size_t next_prime_idx;
    mpz_class product;
    mpz_class delta_Q;
    mpz_class delta_sop;
};
// Generate all primes <= limit using the Sieve of Eratosthenes.
std::vector<unsigned> generate_primes(unsigned limit)
{
    std::vector<bool> is_prime(limit + 1, true);

    is_prime[0] = false;
    is_prime[1] = false;

    for (unsigned p = 2; p * p <= limit; ++p) {
        if (!is_prime[p]) {
            continue;
        }

        for (unsigned multiple = p * p;
             multiple <= limit;
             multiple += p) {
            is_prime[multiple] = false;
        }
    }

    std::vector<unsigned> primes;

    for (unsigned p = 2; p <= limit; ++p) {
        if (is_prime[p]) {
            primes.push_back(p);
        }
    }

    return primes;
} // generate_primes

/**
 * For a given support, processes to determine if any primes may violate 
 * Grosswald's inequality.
 *
 * TODO - Hoist variables to thread local static variables for parallelism.
 **/
inline void process_support(
    const std::vector<unsigned>& support,
    const mpz_class& bound,
    const mpz_class& Q,
    const mpz_class& delta_Q,
    const mpz_class& delta_sop,
    LPRStats& stats
) {
    assert(support[0] == 2);
    ++stats.total_support_count;

    update_corollary4_threshold(delta_Q, delta_sop);
    //corollary4_eval_and_log(Q+1);
    if (corollary4_proves_grosswald(Q + 1)) {
        return;
    }
    ++stats.surviving_support_count;

    const mpz_class m = bound / Q;

    for (mpz_class d = 1; d <= m; ++d) {
        mpz_class remaining_factor = d;

        // Strip from d all prime factors that belong to the support.
        for (unsigned q : support) {
            while (mpz_divisible_ui_p(remaining_factor.get_mpz_t(), q)) {
                mpz_divexact_ui(remaining_factor.get_mpz_t(), remaining_factor.get_mpz_t(), q);
            }

            if (remaining_factor == 1) {
                break;
            }
        }
        
        // If remaining_factor != 1, then d contains a prime factor
        // not already in the support, so d * Q has more than omega distinct prime factors.
        if (remaining_factor == 1) {
            const mpz_class p = Q * d + 1;
            //corollary4_eval_and_log(p);
            if (corollary4_proves_grosswald(p)) {
                break;
            }
            ++stats.candidate_count;

            if (is_composite_base2_fermat(p)) {
                ++stats.fermat_composite_count;
            } else {
                ++stats.fermat_survivor_count;

                const mpz_class n = p - 1;
                const auto g = find_least_primitive_root(
                    p, n, support
                );
                if (g) {
                    ++stats.certified_count;

                    if (*g > stats.largest_least_primitive_root) {
                        stats.largest_least_primitive_root = *g;
                    }

                    const mpz_class g_plus_two = *g + 2;
                    mpz_class g_plus_two_sqr;
                    mpz_mul(g_plus_two_sqr.get_mpz_t(), g_plus_two.get_mpz_t(), g_plus_two.get_mpz_t());
                    //if (g_plus_two * g_plus_two < p) {
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

/** 
 * Iterative implementation of DFS 
 *
 * Maintains that support contains an increasing list of primes.
 * Supports are iterated through in lexicographic order.
 * The following stack frame is utilised as scratch space if needed.
 *
 * TODO - Add in the fact that we know 2 is part of the support.
 * TODO - Potentially hoist out and precompute all min_complete_products
 **/
void dfs_iter(
    const std::vector<unsigned>& primes,
    const mpz_class& bound,
    LPRStats& stats
) {
    using parameters::OMEGA;
    using parameters::S;

    // Initialise DFS data
    std::vector<StackFrame> stack(OMEGA+1); // +1 is scratch space for final stack frame
    std::vector<unsigned> support(OMEGA);
    std::size_t depth  = 0;
    stack[0].next_prime_idx = 0;
    stack[0].product   = 1;
    stack[0].delta_Q   = 1;
    stack[0].delta_sop = 0;

    constexpr std::size_t DELTA_START_DEPTH = OMEGA - S;
    std::size_t remaining_after_choice, i;

    mpz_class min_complete_product;

    // Reserve space to avoid reallocations later
    mpz_realloc2(min_complete_product.get_mpz_t(), 1024);

    while (true) {
        remaining_after_choice = OMEGA - depth - 1;
        i = stack[depth].next_prime_idx; // The last chosen prime
        
        if (i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
            if (depth == 0) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        stack[depth+1].product = stack[depth].product * primes[i];
        min_complete_product = stack[depth+1].product;
        for (std::size_t j = 1; j <= remaining_after_choice; ++j) {
            min_complete_product *= primes[i + j];
        }
        if (min_complete_product > bound) {
            if (depth == 0) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        support[depth] = primes[i];
        if (depth >= DELTA_START_DEPTH) {
            stack[depth+1].delta_Q = stack[depth].delta_Q * primes[i];
            stack[depth+1].delta_sop = stack[depth].delta_Q + (primes[i] * stack[depth].delta_sop);
        } else {
            stack[depth+1].delta_Q = stack[depth].delta_Q;
            stack[depth+1].delta_sop = stack[depth].delta_sop;
        }

        if (depth == OMEGA - 1) {
            // depth+1 is now scratch space
            process_support(
                support,
                bound,
                stack[depth+1].product,
                stack[depth+1].delta_Q,
                stack[depth+1].delta_sop,
                stats
            );
            ++stack[depth].next_prime_idx; // Move to sibling leaf node at same depth
        } else {
            stack[++depth].next_prime_idx = i + 1; // Move to next depth and use later primes 
        }
    } // while (true)
}

} // namespace


int main()
{
    using parameters::PRIME_LIMIT;

    // WARNING: PRIME_LIMIT is part of the exhaustive-search guarantee.
    // It must be large enough to include every prime that can occur in a
    // K-prime support with product <= bound. This is checked at runtime below.

    mpz_class bound;
    mpz_ui_pow_ui(bound.get_mpz_t(), 10, parameters::BOUND_EXPONENT);
    bound *= parameters::BOUND_MULTIPLIER;

    // Print initial configuration settings
    std::cout << "Configuration:\n"
              << "omega: " << parameters::OMEGA  << '\n'
              << "Search bound B: "
              << parameters::BOUND_MULTIPLIER / 10.0
              << " x 10^" << parameters::BOUND_EXPONENT + 1 << '\n'
              << "Prime generation limit: " << PRIME_LIMIT << '\n'
              << "Primitive root search limit: "
              << parameters::PRIMITIVE_ROOT_SEARCH_LIMIT
              << "\n\n";

    std::vector<unsigned> primes =
        generate_primes(PRIME_LIMIT);

    // We need to check that floor(bound / p_(omega-1)#) <= PRIME_LIMIT.
    // First check that primes contains enough primes to compute p_(omega-1)#.
    if (primes.size() < parameters::OMEGA - 1) {
        throw std::runtime_error(
            "PRIME_LIMIT is too small to generate the first omega - 1 primes."
        );
    }

    // Then check that floor(bound / p_(omega-1)#) <= PRIME_LIMIT.
    mpz_class smallest_product = 1;
    for (std::size_t i = 0; i < parameters::OMEGA - 1; ++i) {
        smallest_product *= primes[i];
    }

    const mpz_class required_prime_limit =
        bound / smallest_product;

    if (mpz_class(PRIME_LIMIT) < required_prime_limit) {
        throw std::runtime_error(
            "PRIME_LIMIT is too small to guarantee exhaustive support enumeration."
        );
    }

    // Working support vector used by dfs() as it recursively builds
    // and backtracks through omega-prime supports.
    std::vector<unsigned> support; // TODO - seed support with 2

    // Counters updated by dfs() through reference parameters.
    LPRStats stats;
    dfs_iter(primes, bound, stats);

    stats.print();

    if (stats.unresolved_count > 0 ||
        stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
