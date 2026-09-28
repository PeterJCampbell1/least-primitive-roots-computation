#include <gmpxx.h>
#include "corollary4.hpp"
#include "primality.hpp"
#include "stats.hpp"

#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>

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
}


void dfs(
    std::size_t next_index,
    const mpz_class& current_product,
    const mpz_class& current_sop, // Elementary symmetric poly of deg = len(supp)-1, eval at support
    std::vector<unsigned>& support,
    const std::vector<unsigned>& primes,
    std::size_t k,
    const mpz_class& bound,
    unsigned primitive_root_search_limit,
    LPRStats& stats
) {
    if (support.size() == k) {
        if (support[0] != 2) {
            throw std::runtime_error(
                "Support must contain 2!"
            );
        }

        ++stats.total_support_count;

        const mpz_class& Q = current_product;
        const mpz_class& S = current_sop;

        const Corollary4Threshold corollary4_threshold = make_corollary4_threshold(Q, S);
        if (corollary4_proves_grosswald(Q + 1, corollary4_threshold)) {
            return;
        }
        ++stats.surviving_support_count;

        const mpz_class m = bound / Q;

        for (mpz_class d = 1; d <= m; ++d) {
            mpz_class remaining_factor = d;

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
            // not already in the support, so d * Q has more than k distinct prime factors.
            if (remaining_factor == 1) {
                const mpz_class p = Q * d + 1;
                if (corollary4_proves_grosswald(p, corollary4_threshold)) {
                    break;
                }
                ++stats.candidate_count;

                if (is_composite_base2_fermat(p)) {
                    ++stats.fermat_composite_count;
                } else {
                    ++stats.fermat_survivor_count;

                    const mpz_class n = p - 1;
                    const auto g = find_least_primitive_root(
                        p, n, support, primitive_root_search_limit
                    );
                    if (g) {
                        ++stats.certified_count;

                        if (*g > stats.largest_least_primitive_root) {
                            stats.largest_least_primitive_root = *g;
                        }

                        const mpz_class g_plus_two = *g + 2;
                        if (g_plus_two * g_plus_two < p) {
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
    }

    std::size_t remaining_after_choice =
        k - support.size() - 1;

    for (std::size_t i = next_index;
         i < primes.size();
         ++i) {

        if (primes.size() - (i + 1) < remaining_after_choice) {
            break;
        }

        mpz_class new_product =
            current_product * primes[i];

        mpz_class new_sop = 
            current_product + (primes[i] * current_sop);

        mpz_class minimum_complete_product =
            new_product;

        for (std::size_t j = 1;
             j <= remaining_after_choice;
             ++j) {
            minimum_complete_product *= primes[i + j];
        }

        if (minimum_complete_product > bound) {
            break;
        }

        support.push_back(primes[i]);

        dfs(
            i + 1,
            new_product,
            new_sop,
            support,
            primes,
            k,
            bound,
            primitive_root_search_limit,
            stats
        );

        support.pop_back();
    }
}


int main()
{
    constexpr std::size_t K = 33;
    // WARNING: PRIME_LIMIT is part of the exhaustive-search guarantee.
    // It must be large enough to include every prime that can occur in a
    // K-prime support with product <= bound. This is checked at runtime below.
    constexpr unsigned PRIME_LIMIT = 4000;

    // Upper bound for testing 2, 3, ..., PRIMITIVE_ROOT_SEARCH_LIMIT
    // as least primitive root g(p) candidates.
    // WARNING: Increasing this substantially may greatly increase runtime.
    constexpr unsigned PRIMITIVE_ROOT_SEARCH_LIMIT = 1000;

    // Search bound B = 1.8 × 10^54 = 18 × 10^53.
    constexpr unsigned BOUND_MULTIPLIER = 18;
    constexpr unsigned BOUND_EXPONENT = 53;
    mpz_class bound;
    mpz_ui_pow_ui(bound.get_mpz_t(), 10, BOUND_EXPONENT);
    bound *= BOUND_MULTIPLIER;

    // Print initial configuration settings
    std::cout << "Configuration:\n"
              << "K: " << K << '\n'
              << "Search bound B: "
              << BOUND_MULTIPLIER / 10.0
              << " x 10^" << BOUND_EXPONENT + 1 << '\n'
              << "Prime generation limit: " << PRIME_LIMIT << '\n'
              << "Primitive root search limit: "
              << PRIMITIVE_ROOT_SEARCH_LIMIT
              << "\n\n";

    std::vector<unsigned> primes =
        generate_primes(PRIME_LIMIT);

    // We need to check that floor(bound / p_(K-1)#) <= PRIME_LIMIT.
    // First check that primes contains enough primes to compute p_(K-1)#.
    if (primes.size() < K - 1) {
        throw std::runtime_error(
            "PRIME_LIMIT is too small to generate the first K - 1 primes."
        );
    }

    // Then check that floor(bound / p_(K-1)#) <= PRIME_LIMIT.
    mpz_class smallest_product = 1;
    for (std::size_t i = 0; i < K - 1; ++i) {
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
    // and backtracks through K-prime supports.
    std::vector<unsigned> support;

    // Counters updated by dfs() through reference parameters.
    LPRStats stats;

    dfs(
        0,
        1,
        0,
        support,
        primes,
        K,
        bound,
        PRIMITIVE_ROOT_SEARCH_LIMIT,
        stats
    );

    stats.print();

    if (stats.unresolved_count > 0 ||
        stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
