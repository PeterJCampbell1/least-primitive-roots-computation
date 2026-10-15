#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"
#include "corollary4.hpp"
#include "generate_supports.hpp"
#include "process_support.hpp"

#include <iomanip>
#include <iostream>
#include <vector>
#include <gmpxx.h>
#include <thread>
#include <stdexcept>
#include <cassert>
#include <chrono>
#include <optional>
#include <string>

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

enum class RunMode { Single, Parallel, DryRun };

/*
 * --single (default): producer processes each support inline.
 * --parallel: one producer thread and PARALLEL_THREAD_COUNT - 1 consumers.
 * --dry-run: runs the producer only, emitted supports are discarded.
 * Returns std::nullopt on an unrecognised argument.
 */
std::optional<RunMode> parse_run_mode(int argc, char* argv[]) {
    RunMode mode = RunMode::Single;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--single") {
            mode = RunMode::Single;
        } else if (arg == "--parallel") {
            mode = RunMode::Parallel;
        } else if (arg == "--dry-run") {
            mode = RunMode::DryRun;
        } else {
            return std::nullopt;
        }
    }
    return mode;
}

// TODO - Choose thread count properly (e.g. --threads n, or detected cores
// for the UQ HPC clusters) once the parallel design is settled.
constexpr unsigned PARALLEL_THREAD_COUNT = 8;

} // namespace


int main(int argc, char* argv[])
{
    const std::optional<RunMode> parsed_mode = parse_run_mode(argc, argv);
    if (!parsed_mode) {
        std::cerr << "Usage: " << argv[0] << " [--single | --parallel | --dry-run]\n";
        return 2;
    }
    const RunMode mode = *parsed_mode;

    unsigned threadCount = 1;
    switch (mode) {
    case RunMode::Single:
        std::cout << "Running single-threaded computation" << '\n' << '\n';
        break;
    case RunMode::Parallel:
        threadCount = PARALLEL_THREAD_COUNT;
        std::cout << "Running multi-threaded computation" << '\n'
                  << "Using: " << threadCount << " threads" << '\n' << '\n';
        break;
    case RunMode::DryRun:
        std::cout << "DRY RUN: no primes were tested or verified" << '\n' << '\n';
        break;
    }

    using parameters::PRIME_LIMIT;

    std::vector<unsigned> primes =
        generate_primes(PRIME_LIMIT);

    // The global bound needs the first omega primes.
    if (primes.size() < parameters::OMEGA) {
        throw std::runtime_error(
            "PRIME_LIMIT is too small to generate the first omega primes."
        );
    }

    // Largest Q*d (p = Q*d + 1) not covered by Corollary 4 for any support.
    // Every p with omega(p-1) = omega and p - 1 > bound satisfies Grosswald by Cor 4.
    mpz_class bound;
    corollary4_global_bound(primes, bound);

    // Print initial configuration settings
    std::cout << "Configuration:\n"
              << "omega: " << parameters::OMEGA  << '\n'
              << "s: " << parameters::S << '\n'
              << "Search bound B (Cor 4 global bound on p - 1): "
              << std::scientific << std::setprecision(6) << bound.get_d()
              << std::defaultfloat << '\n'
              << "B (exact): " << bound << '\n'
              << "Prime generation limit: " << PRIME_LIMIT << '\n'
              << "Primitive root search limit: "
              << parameters::PRIMITIVE_ROOT_SEARCH_LIMIT
              << "\n\n";

    // We need to check that floor(bound / p_(omega-1)#) <= PRIME_LIMIT,
    // as the largest prime of any support with product <= bound is at most this.
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

    const auto start_time = std::chrono::steady_clock::now();

    if (mode == RunMode::DryRun) {
        lpr::producer::generate_supports_dry_run(primes, bound);
    } else if (mode == RunMode::Single) {
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
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start_time;

    lpr::stats::flush_thread_local_stats();
    auto& master_stats = lpr::stats::get_master_stats();
    if (mode == RunMode::DryRun) {
        master_stats.print_producer();
        std::cout << "Elapsed time: " << elapsed.count() << " s\n"
                  << "DRY RUN: no primes were tested or verified\n";
        return 0;
    }
    master_stats.print();
    std::cout << '\n';
    master_stats.print_producer();
    std::cout << "Elapsed time: " << elapsed.count() << " s\n";

    if (master_stats.unresolved_count > 0 ||
        master_stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
