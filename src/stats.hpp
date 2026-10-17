/**
 * stats.hpp: run statistics.
 *
 * Every thread counts into its own thread-local Counters (no locking on the hot
 * path) and periodically merges them into the mutex-guarded MasterStats, which
 * the monitor thread reads for progress logs and main prints at the end.
 *
 * Counters is a plain aggregate, so it copies correctly by default. When adding
 * a counter, add it to merge() and to print() or print_producer().
 **/

#pragma once

#include "parameters.hpp"
#include "trial_division.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>

namespace lpr::stats {

struct Counters {
    // Per-candidate pipeline (process_support)
    std::uint64_t total_support_count = 0; // Supports processed
    std::uint64_t candidate_count = 0; // p = Q*d + 1 with rad(d) | Q and d <= d_max
    std::uint64_t small_prime_composite_count = 0;
    // Trial division composites by chunk (index as in lpr::trial_division::table().chunks)
    std::array<std::uint64_t, parameters::MAX_TRIAL_DIVISION_CHUNKS> composite_caught_at = {};
    std::uint64_t fermat_composite_count = 0;
    std::uint64_t fermat_survivor_count = 0;
    std::uint64_t mont3_fallback_count = 0; // Candidates too large for mont3 (GMP used instead)
    std::uint64_t certified_count = 0;
    std::uint64_t unresolved_count = 0;
    std::uint64_t certified_inequality_pass_count = 0;
    std::uint64_t certified_inequality_fail_count = 0;
    std::uint64_t largest_least_primitive_root = 0;
    std::array<std::uint64_t, parameters::PRIMITIVE_ROOT_SEARCH_LIMIT + 1> winners_by_g = {}; // g(p) histogram

    // Producer (support enumeration) statistics
    std::uint64_t dfs_nodes_visited = 0;
    std::uint64_t cor4_evaluations = 0;
    std::uint64_t cor4_reused = 0; // First child reuses its parent's minimal completion
    std::uint64_t inherited_bound_prunes = 0; // Pruned by parent's Cor 4 bound before evaluating Cor 4
    std::uint64_t supports_emitted = 0;
    std::uint64_t d_max_sum = 0;
    std::uint64_t d_max_max = 0;
    std::uint64_t batches_pushed = 0; // Batches handed to the queue
    std::uint64_t batches_coprocessed = 0; // Batches the producer processed itself (queue full)
    std::uint64_t consumer_waits = 0; // Consumer pops that found the queue empty
    std::uint64_t consumer_wait_ns = 0; // Total time consumers spent waiting on an empty queue

    // Least primitive root search statistics (find_least_primitive_root)
    std::uint64_t lpr_jacobi_calls = 0; // Candidates g reaching the Jacobi filter
    std::uint64_t lpr_legendre_survivors = 0; // Candidates g tested with the power tree
    std::uint64_t lpr_root_powm_count = 0; // g^(2d) computations (root of the power tree)
    std::uint64_t lpr_tree_powm_count = 0; // Exponentiations inside the power tree
    std::array<std::uint64_t, 16> winners_by_class = {}; // Index (p mod 8)*2 + (d mod 2)

    void merge(const Counters& other) {
        total_support_count += other.total_support_count;
        candidate_count += other.candidate_count;
        small_prime_composite_count += other.small_prime_composite_count;
        add(composite_caught_at, other.composite_caught_at);
        fermat_composite_count += other.fermat_composite_count;
        fermat_survivor_count += other.fermat_survivor_count;
        mont3_fallback_count += other.mont3_fallback_count;
        certified_count += other.certified_count;
        unresolved_count += other.unresolved_count;
        certified_inequality_pass_count += other.certified_inequality_pass_count;
        certified_inequality_fail_count += other.certified_inequality_fail_count;
        largest_least_primitive_root = std::max(largest_least_primitive_root, other.largest_least_primitive_root);
        add(winners_by_g, other.winners_by_g);

        dfs_nodes_visited += other.dfs_nodes_visited;
        cor4_evaluations += other.cor4_evaluations;
        cor4_reused += other.cor4_reused;
        inherited_bound_prunes += other.inherited_bound_prunes;
        supports_emitted += other.supports_emitted;
        d_max_sum += other.d_max_sum;
        d_max_max = std::max(d_max_max, other.d_max_max);
        batches_pushed += other.batches_pushed;
        batches_coprocessed += other.batches_coprocessed;
        consumer_waits += other.consumer_waits;
        consumer_wait_ns += other.consumer_wait_ns;

        lpr_jacobi_calls += other.lpr_jacobi_calls;
        lpr_legendre_survivors += other.lpr_legendre_survivors;
        lpr_root_powm_count += other.lpr_root_powm_count;
        lpr_tree_powm_count += other.lpr_tree_powm_count;
        add(winners_by_class, other.winners_by_class);
    }

    // Search summary (stdout)
    void print() const {
        std::cout << "Search summary:\n"
                  << "---------------\n"
                  << "K-prime supports examined: " << total_support_count << '\n'
                  << "Candidates not covered by Corollary 4: " << candidate_count << '\n'
                  << "Proved composite by small prime trial division test: " << small_prime_composite_count << '\n';
        const auto& td = lpr::trial_division::table();
        for (std::size_t k = 0; k < td.chunks.size() && k < composite_caught_at.size(); ++k) {
            const auto& chunk = td.chunks[k];
            std::cout << "  by primes " << td.tests[chunk.first].prime << ".."
                      << td.tests[chunk.first + chunk.count - 1].prime << ": " << composite_caught_at[k] << '\n';
        }
        std::cout << "Proved composite by base-2 Fermat test: " << fermat_composite_count << '\n'
                  << "Surviving base-2 Fermat test: " << fermat_survivor_count << '\n'
                  << "Candidates using GMP instead of Montgomery arithmetic (p >= 2^190): "
                  << mont3_fallback_count << '\n'
                  << "Least primitive root certified: " << certified_count << '\n'
                  << "Grosswald inequality verified: " << certified_inequality_pass_count << '\n'
                  << "Grosswald inequality failed: " << certified_inequality_fail_count << '\n'
                  << "No least primitive root certified within search limit: " << unresolved_count << '\n'
                  << "Largest least primitive root found: " << largest_least_primitive_root << '\n';

        const double certified = certified_count ? static_cast<double>(certified_count) : 1.0;
        std::cout << "\nLeast primitive root search:\n"
                  << "Jacobi filter calls: " << lpr_jacobi_calls
                  << " (" << lpr_jacobi_calls / certified << " per prime)\n"
                  << "Legendre survivors tested: " << lpr_legendre_survivors
                  << " (" << lpr_legendre_survivors / certified << " per prime, the last being g(p))\n"
                  << "Root powm (g^(2d)) calls: " << lpr_root_powm_count << '\n'
                  << "Power tree powm calls: " << lpr_tree_powm_count
                  << " (" << lpr_tree_powm_count / certified << " per prime)\n"
                  << "Winners by p mod 8 (d even / d odd):\n";
        for (unsigned r = 1; r < 8; r += 2) {
            std::cout << "  p = " << r << " mod 8: "
                      << winners_by_class[2*r] << " / " << winners_by_class[2*r + 1] << '\n';
        }

        // Histogram of g(p), non-zero entries only, eight per line
        std::cout << "Winners by g (g: count):\n";
        int on_line = 0;
        for (std::size_t g = 0; g < winners_by_g.size(); ++g) {
            if (winners_by_g[g] == 0) continue;
            std::cout << "  " << g << ": " << winners_by_g[g];
            if (++on_line == 8) {
                std::cout << '\n';
                on_line = 0;
            }
        }
        if (on_line != 0) std::cout << '\n';
    }

    // Producer summary (stdout)
    void print_producer() const {
        std::cout << "Producer summary:\n"
                  << "-----------------\n"
                  << "DFS nodes visited: " << dfs_nodes_visited << '\n'
                  << "Corollary 4 evaluations: " << cor4_evaluations << '\n'
                  << "Corollary 4 results reused by first child: " << cor4_reused << '\n'
                  << "Pruned by inherited Corollary 4 bound: " << inherited_bound_prunes << '\n'
                  << "Supports emitted: " << supports_emitted << '\n'
                  << "Sum of d_max over emitted supports: " << d_max_sum << '\n'
                  << "Largest d_max: " << d_max_max << '\n'
                  << "Batches pushed to queue: " << batches_pushed << '\n'
                  << "Batches co-processed by producer: " << batches_coprocessed << '\n'
                  << "Consumer waits on empty queue: " << consumer_waits
                  << " (" << consumer_wait_ns / 1e9 << " s in total)\n";
    }

private:
    template <typename Array>
    static void add(Array& into, const Array& from) {
        for (std::size_t i = 0; i < into.size(); ++i) into[i] += from[i];
    }
};

// The shared totals: Counters behind a mutex
class MasterStats {
public:
    void merge(const Counters& counters) {
        std::lock_guard<std::mutex> lock(mutex_);
        totals_.merge(counters);
    }

    // A consistent copy of the totals so far
    Counters snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return totals_;
    }

private:
    mutable std::mutex mutex_;
    Counters totals_;
};

inline MasterStats& get_master_stats() {
    static MasterStats master;
    return master;
}

/*
 * A thread's own counters. publish() merges them into the master and resets
 * them; the destructor publishes whatever is left when the thread exits, so the
 * final totals are always complete.
 *
 * During the run, maybe_publish() (called at batch boundaries) publishes at
 * most once per STATS_PUBLISH_INTERVAL_MS, so the master mutex is taken a few
 * times per second instead of after every batch. Each thread's schedule starts
 * at a different offset (by creation order), spreading publishes evenly.
 */
struct ThreadStats {
    Counters counters;
    std::chrono::steady_clock::time_point next_publish;

    ThreadStats() {
        using namespace std::chrono;
        static std::atomic<unsigned> threads_created{0};
        constexpr unsigned SLOTS = 8;
        const unsigned slot = threads_created.fetch_add(1, std::memory_order_relaxed) % SLOTS;
        next_publish = steady_clock::now()
                     + milliseconds(parameters::STATS_PUBLISH_INTERVAL_MS * (slot + 1) / SLOTS);
    }

    void publish() {
        get_master_stats().merge(counters);
        counters = Counters{};
    }

    void maybe_publish() {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_publish) {
            publish();
            next_publish = now + std::chrono::milliseconds(parameters::STATS_PUBLISH_INTERVAL_MS);
        }
    }

    ~ThreadStats() {
        publish();
    }
};

inline ThreadStats& get_thread_stats() {
    static thread_local ThreadStats instance;
    return instance;
}

// This thread's counters (the hot-path accessor)
inline Counters& get_thread_local_stats() {
    return get_thread_stats().counters;
}

// Publishes this thread's statistics if its publish interval has passed (cheap
// otherwise: one clock read). Call at batch boundaries.
inline void maybe_publish_thread_local_stats() {
    get_thread_stats().maybe_publish();
}

// Makes this thread's statistics so far visible in the master statistics.
// Also called by main before reading the final totals (main's own thread-local
// counters would otherwise only be published when the program exits).
inline void publish_thread_local_stats() {
    get_thread_stats().publish();
}

} // namespace lpr::stats
