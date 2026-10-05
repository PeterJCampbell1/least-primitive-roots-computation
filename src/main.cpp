#include <gmpxx.h>
#include "corollary4.hpp"
#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"

// Analysis Tool
//#include "analysis.hpp"

#include <omp.h>
#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <limits>

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
 * Note: with the local delta bounding update, we no longer need to compute
 * Corollary 4 threshold's in this function.
 **/
inline void process_support(
    const std::vector<unsigned>& support,
    const mpz_class& bound,
    const mpz_class& Q
    // TODO - don't need these anymore as corollary 4 already satisfied
    //const mpz_class& delta_Q,
    //const mpz_class& delta_sop
) {
    assert(support[0] == 2);
    auto& stats = get_thread_local_stats();
    ++stats.total_support_count;

    // TODO - this has already been updated
    //update_corollary4_threshold(Q, delta_Q, delta_sop);
    ++stats.cor4_proves_grosswald_count;
    unsigned prev_d = 1;
    unsigned d = 1;
    // TODO - this has already been checked at DFS stage
    /* if (corollary4_proves_grosswald(d-prev_d)) {
        return;
    }
    */
    // TODO - total support count is now the same as surviving support count
    //++stats.surviving_support_count;

    // Reusable memory for heap allocated objects only
    // mpz_class calls the automatic constructor/destructor if defined locally
    static thread_local mpz_class m;
    static thread_local mpz_class p;
    static thread_local mpz_class n;
    static thread_local mpz_class g_plus_two;
    static thread_local mpz_class g_plus_two_sqr;

    // NOTE: We assume m fits within a 64 bit integer
    m = bound / Q;
    assert(m.fits_ulong_p() && "m exceeds 64-bit integer range");

    for (d = 1; d <= m.get_ui(); ++d) { // Propagate d = 1 down to Fermat checks
        // TODO - generate d via Q-smooth number generator since knowing the factorisation
        // will be useful.
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
        
        // TODO - Is corollary 4 guaranteed to be less than the bound?
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

            if (is_composite_small_prime(p)) {
                ++stats.small_prime_composite_count;
            } else if (is_composite_base2_fermat(p, n)) {
            //} else if (is_composite_base2_miller_rabin(p, n)) {
                ++stats.fermat_composite_count;
            } else {
                ++stats.fermat_survivor_count;

                const auto g = find_least_primitive_root(
                    p, n, d, support
                );
                if (g) {
                    ++stats.certified_count;

                    if (*g > stats.largest_least_primitive_root) {
                        stats.largest_least_primitive_root = *g;
                    }

                    g_plus_two = *g + 2;
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
 * Note: We know that support must always contain 2 (as Q*d + 1 must be odd).
 **/
void dfs_iter(
    const std::vector<unsigned>& primes,
    const mpz_class& bound
) {
    using parameters::OMEGA;
    using parameters::S;

    static_assert(OMEGA > 2, "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);

    // Initialise DFS data
    std::vector<StackFrame> stack(OMEGA+1); // +1 is scratch space for final stack frame
    std::vector<unsigned> support(OMEGA);
    stack[0].next_prime_idx = 0;
    stack[0].product   = 1;
    stack[0].delta_Q   = 1;
    stack[0].delta_sop = 0;

    // We know 2 \in support and the 0th frame corresponds to this
    std::size_t depth  = 1; 
    support[0] = primes[0]; // primes[0] == 2
    stack[1].next_prime_idx = 1; 
    stack[1].product = 2;
    stack[1].delta_Q = 1;
    stack[1].delta_sop = 0;
    
    // Number of primes to ignore
    constexpr std::size_t DELTA_START_DEPTH = OMEGA - S; 
    static_assert(DELTA_START_DEPTH > 0, "We cannot have OMEGA == S as 2 must be excluded");
    std::size_t remaining_after_choice, i;
    mpz_class min_complete_product;

    // Naive approach to local bounding
    mpz_class min_complete_delta_Q;
    mpz_class min_complete_delta_sop;

    // Reserve space to avoid reallocations later
    mpz_realloc2(min_complete_product.get_mpz_t(), 1024);

    while (true) {
        remaining_after_choice = OMEGA - depth - 1;
        i = stack[depth].next_prime_idx; // The last prime chosen at this depth
        support[depth] = primes[i];
        
        if (i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        if (depth >= DELTA_START_DEPTH) {
            mpz_mul_ui(
                stack[depth+1].delta_Q.get_mpz_t(),
                stack[depth].delta_Q.get_mpz_t(),
                primes[i]
            ); // delta_Q_j = delta_Q_{j-1} * p_j

            mpz_set(
                stack[depth+1].delta_sop.get_mpz_t(),
                stack[depth].delta_Q.get_mpz_t()
            );
            mpz_addmul_ui(
                stack[depth+1].delta_sop.get_mpz_t(),
                stack[depth].delta_sop.get_mpz_t(),
                primes[i]
            ); // delta_SOP_j = delta_Q_{j-1} + p_j * delta_SOP_{j-1} 
        } else { // Ignore first few primes.
            stack[depth+1].delta_Q = 1;
            stack[depth+1].delta_sop = 0;
        }

        stack[depth+1].product = stack[depth].product * primes[i];
        min_complete_product = stack[depth+1].product;
        if (depth >= DELTA_START_DEPTH) {
            min_complete_delta_Q = stack[depth+1].delta_Q;
            min_complete_delta_sop = stack[depth+1].delta_sop;
        }
        for (std::size_t j = 1; j <= remaining_after_choice; ++j) {
            mpz_mul_ui(
                min_complete_product.get_mpz_t(), 
                min_complete_product.get_mpz_t(),
                primes[i+j]
            ); // min_prod *= primes[i+j];
               
            // Naive local bounding approach
            // TODO: figure out an optimal way to implement this
            if (depth >= DELTA_START_DEPTH) {
                // Calculate delta_sop first, as this uses the previous delta_Q
                // Then after update delta_Q
                min_complete_delta_sop *= primes[i+j];
                min_complete_delta_sop += min_complete_delta_Q;
                min_complete_delta_Q *= primes[i+j];
            }
        }
        if (min_complete_product > bound) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        } // TODO - With local bounding this now is somewhat obsolete
        if (depth >= DELTA_START_DEPTH) {
            update_corollary4_threshold(min_complete_product, min_complete_delta_Q, min_complete_delta_sop);
            if (corollary4_proves_grosswald(0)) { 
                // Local Delta means all suffix's from this prefix must satisfy corollary 4, 
                // As any larger suffix can only widen the inequality (lower lhs, raise rhs)
                // Thus, we can prune this subtree
                if (depth == 1) break; // Traversal complete
                --depth;
                ++stack[depth].next_prime_idx; // Move to next subset through backtracking
                continue;
            }
        }

        if (depth == OMEGA - 1) {
            // depth+1 is now scratch space
            process_support(
                support,
                bound,
                stack[depth+1].product
                // TODO - don't need these anymore as corollary 4 already satisfied
                //stack[depth+1].delta_Q,
                //stack[depth+1].delta_sop
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

    // Master statistics block that threads will merge with 
    // when complete.
    LPRStats master_stats;

    #pragma omp parallel
    {
        
        // For now will still be single threaded TODO
        #pragma omp single
        {
            dfs_iter(primes, bound);
        }

        // Merge stats from each thread.
        // Stats initialised to 0 ensuring merging will
        // always be correct.
        #pragma omp critical
        {
            master_stats.merge(get_thread_local_stats());
        }
    }

    master_stats.print();

    if (master_stats.unresolved_count > 0 ||
        master_stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
