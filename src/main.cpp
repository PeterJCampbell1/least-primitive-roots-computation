//#include "corollary4.hpp"
#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"
#include "generate_supports.hpp"
#include "process_support.hpp"

#include <omp.h> // TODO - Probably remove this and replace with lower primitives for merge
#include <iostream>
#include <vector>
#include <gmpxx.h>
#include <thread>
#include <stdexcept>
#include <cassert>

// Anonymous namespace - restricts type visibility to this file
namespace {

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

/*
 * TODO - If --s or --single passed, return one. 
 *  If --threads n or --t n specified, return that number
 *  Otherwise return number of available physical cores if obtainable.
 *  Otherwise if this failed, choose to fall back to single thread or default multi-thread 
 * (maybe give an option --multi that makes program crash if we can't obtain number of threads)
 */
unsigned get_target_thread_count(int argc, char* argv[]) {
    (void)argv; // TODO - REMOVE
    if (argc > 1) {
        return 8; // TODO - MULTI-THREAD PROPERLY
    } else {
        return 1;
    }
}

} // namespace


int main(int argc, char* argv[])
{
    (void)argv;
	unsigned threadCount = get_target_thread_count(argc, argv); 
    if (threadCount == 1) {
        std::cout << "Running single-threaded computation" << '\n' << '\n';
    } else {
        std::cout << "Running multi-threaded computation" << '\n'
                  << "Using: " << threadCount << " threads" << '\n' << '\n';
    }

    using parameters::PRIME_LIMIT;

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

    if (threadCount == 1) {
        lpr::producer::generate_supports(primes, bound);
    } else {
        lpr::parallel::HysteresisQueue<> queue;

        // Spawn single producer
        std::thread producer([&primes, &bound, &queue]() {
            lpr::producer::generate_supports_parallel(primes, bound, queue);
        });
        threadCount--;

        // Spawn specified number of consumer threads
        std::vector<std::thread> consumers;
        consumers.reserve(threadCount);
        for (unsigned i = 0; i < threadCount; ++i) {
            consumers.emplace_back([&queue]() {
                lpr::consumer::start_consumer(queue);
            });
        }
        
        producer.join(); // End producer thread
        for (auto& consumer : consumers) { // End consumer threads
            consumer.join();
        }
    }

    // Stats for other threads automatically merged (see stats.hpp)
    lpr::stats::flush_thread_local_stats();
    auto& master_stats = lpr::stats::get_master_stats();
    master_stats.print();

    if (master_stats.unresolved_count > 0 ||
        master_stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
