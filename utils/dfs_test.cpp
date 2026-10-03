#include <gmpxx.h>
#include "parameters.hpp"

// Analysis Tool
#include <cstdint>
#include <cmath>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <limits>

// Anonymous namespace - restricts type visibility to this file
namespace {

template <typename T>
void print_vector(const std::vector<T>& vec) {
    std::cout << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        std::cout << vec[i];
        if (i + 1 < vec.size()) std::cout << ", ";
    }
    std::cout << "]\n";
}

struct StackFrame {
    std::size_t next_prime_idx;
    double log_sum;
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
    const std::vector<double>& log_primes,
    const double log_bound
) {
    using parameters::OMEGA;
    using parameters::S;
    const uint64_t MILESTONE_INTERVAL = 10'000'000'000;

    static_assert(OMEGA > 2, "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);
    std::cout << "Preparing DFS:\n" << "  Omega: " << OMEGA << '\n' << "  S:" << S << '\n';

    // Initialise DFS data
    std::vector<StackFrame> stack(OMEGA+1); // +1 is scratch space for final stack frame
    std::vector<unsigned> support(OMEGA);
    stack[0].next_prime_idx = 0;
    stack[0].log_sum = 0;

    // We know 2 \in support and the 0th frame corresponds to this
    std::size_t depth  = 1; 
    support[0] = primes[0]; // primes[0] == 2
    stack[1].next_prime_idx = 1; 
    stack[1].log_sum = std::log(2.0);
    
    // Number of primes to ignore
    std::size_t remaining_after_choice, i;
    double min_complete_log_sum;
    uint64_t total_supports = 0;

    while (true) {
        remaining_after_choice = OMEGA - depth - 1;
        i = stack[depth].next_prime_idx; // The last prime chosen at this depth
        
        if (i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        stack[depth+1].log_sum = stack[depth].log_sum + log_primes[i];
        min_complete_log_sum = stack[depth+1].log_sum;
        for (std::size_t j = 1; j <= remaining_after_choice; ++j) {
            min_complete_log_sum += log_primes[i+j];
        }
        if (min_complete_log_sum > log_bound) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        support[depth] = primes[i];
        if (depth == OMEGA - 1) {
            // depth+1 is now scratch space
            ++total_supports;
            if (total_supports == 1 || total_supports % MILESTONE_INTERVAL == 0) {
                std::cout << "Milestone at subset #" << total_supports << ": ";
                print_vector(support);
            }

            ++stack[depth].next_prime_idx; // Move to sibling leaf node at same depth
        } else {
            stack[++depth].next_prime_idx = i + 1; // Move to next depth and use later primes 
        }
    } // while (true)
    
    std::cout << "Total supports: " << total_supports << '\n';
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

    std::cout << "The required prime limit was: " << required_prime_limit << '\n'
        << "The given prime limit was: " << PRIME_LIMIT << '\n';

    std::vector<double> log_primes;
    for (unsigned p : primes) {
        log_primes.push_back(std::log(static_cast<double>(p))); 
    }

    double log_bound = std::log(static_cast<double>(parameters::BOUND_MULTIPLIER)) 
                 + parameters::BOUND_EXPONENT * std::log(10.0);

    // Master statistics block that threads will merge with 
    // when complete.
    dfs_iter(primes, log_primes, log_bound);

    return 0;
}
