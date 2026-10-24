/**
 * stats.hpp: run statistics.
 *
 * Every thread counts into its own thread-local Counters (no locking on the hot
 * path) and periodically merges them into the mutex-guarded MasterStats, which
 * the monitor thread reads for progress logs and main prints at the end.
 *
 * Counters is a plain aggregate, so it copies correctly by default. When adding
 * a counter, add it to merge(), to for_each_field() (checkpoints) and to print()
 * or print_producer().
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
#include <string>

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
    // Sum (mod 2^64) of lpr::bits::support_hash over emitted supports: a checksum of
    // the set of supports, independent of emission order, to compare runs
    std::uint64_t supports_checksum = 0;
    std::uint64_t d_max_sum = 0;
    std::uint64_t d_max_max = 0;
    std::uint64_t batches_pushed = 0; // Batches handed to the queue
    std::uint64_t batches_coprocessed = 0; // Batches the producer processed itself (queue full)
    std::uint64_t consumer_waits = 0; // Consumer pops that found the queue empty
    std::uint64_t consumer_wait_ns = 0; // Total time consumers spent waiting on an empty queue

    // Region splitting and work stealing (generate_supports.hpp, work_stealing.hpp)
    std::uint64_t regions_split = 0; // Regions split off and handed out (stolen, or split-tested)
    std::uint64_t region_step_downs = 0; // Base depth moved down because the base level had nothing to give
    std::uint64_t steal_requests = 0; // Steal requests posted by idle workers
    std::uint64_t steal_refusals = 0; // Requests answered "nothing to give"
    std::uint64_t steal_withdrawals = 0; // Requests withdrawn unanswered at termination
    std::uint64_t worker_idle_ns = 0; // Time workers spent without a region (looking for work)
    std::uint64_t steal_wait_ns = 0; // Part of idle time spent waiting for answers to requests
    std::uint64_t steal_backoff_ns = 0; // Part of idle time spent backing off after failed attempts
    std::uint64_t split_ns = 0; // Time victims spent in split_region answering requests
    std::uint64_t requests_refused_idle = 0; // Requests refused by a worker without a region (avoidable)
    std::uint64_t requests_refused_busy = 0; // Requests refused by a busy worker with nothing to give
    std::array<std::uint64_t, parameters::MAX_OMEGA + 1> splits_at_depth = {}; // Regions split off, by base depth

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
        supports_checksum += other.supports_checksum; // Wraps mod 2^64
        d_max_sum += other.d_max_sum;
        d_max_max = std::max(d_max_max, other.d_max_max);
        batches_pushed += other.batches_pushed;
        batches_coprocessed += other.batches_coprocessed;
        consumer_waits += other.consumer_waits;
        consumer_wait_ns += other.consumer_wait_ns;
        regions_split += other.regions_split;
        region_step_downs += other.region_step_downs;
        steal_requests += other.steal_requests;
        steal_refusals += other.steal_refusals;
        steal_withdrawals += other.steal_withdrawals;
        worker_idle_ns += other.worker_idle_ns;
        steal_wait_ns += other.steal_wait_ns;
        steal_backoff_ns += other.steal_backoff_ns;
        split_ns += other.split_ns;
        requests_refused_idle += other.requests_refused_idle;
        requests_refused_busy += other.requests_refused_busy;
        add(splits_at_depth, other.splits_at_depth);

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
                  << "Supports checksum: 0x" << std::hex << supports_checksum << std::dec << '\n'
                  << "Sum of d_max over emitted supports: " << d_max_sum << '\n'
                  << "Largest d_max: " << d_max_max << '\n'
                  << "Batches pushed to queue: " << batches_pushed << '\n'
                  << "Batches co-processed by producer: " << batches_coprocessed << '\n'
                  << "Consumer waits on empty queue: " << consumer_waits
                  << " (" << consumer_wait_ns / 1e9 << " s in total)\n"
                  << "Regions split off (work stealing or split test): " << regions_split
                  << " (base depth stepped down " << region_step_downs << " times)\n"
                  << "Steal requests: " << steal_requests << " (" << steal_refusals << " refused, "
                  << steal_withdrawals << " withdrawn at termination)\n"
                  << "Worker idle time: " << worker_idle_ns / 1e9 << " s in total ("
                  << steal_wait_ns / 1e9 << " s waiting for answers, "
                  << steal_backoff_ns / 1e9 << " s backing off)\n"
                  << "Requests refused: " << requests_refused_idle << " by idle workers, "
                  << requests_refused_busy << " by busy workers with nothing to give\n"
                  << "Time spent splitting regions: " << split_ns / 1e9 << " s\n";
        std::cout << "Regions split off by base depth:";
        bool any = false;
        for (std::size_t d = 0; d < splits_at_depth.size(); ++d) {
            if (splits_at_depth[d] == 0) continue;
            std::cout << "  " << d << ": " << splits_at_depth[d];
            any = true;
        }
        std::cout << (any ? "\n" : " none\n");
    }

    /*
     * Calls f(name, field) for every counter (array entries as "name.i"), in a
     * fixed order. Used to save and load statistics in checkpoints
     * (checkpoint.hpp). Every member must be listed: field_bytes() checks that
     * the listed fields cover sizeof(Counters) (main checks it at startup).
     */
    template <typename Self, typename F>
    static void for_each_field(Self& c, F&& f) {
        f("total_support_count", c.total_support_count);
        f("candidate_count", c.candidate_count);
        f("small_prime_composite_count", c.small_prime_composite_count);
        each(c.composite_caught_at, "composite_caught_at", f);
        f("fermat_composite_count", c.fermat_composite_count);
        f("fermat_survivor_count", c.fermat_survivor_count);
        f("mont3_fallback_count", c.mont3_fallback_count);
        f("certified_count", c.certified_count);
        f("unresolved_count", c.unresolved_count);
        f("certified_inequality_pass_count", c.certified_inequality_pass_count);
        f("certified_inequality_fail_count", c.certified_inequality_fail_count);
        f("largest_least_primitive_root", c.largest_least_primitive_root);
        each(c.winners_by_g, "winners_by_g", f);

        f("dfs_nodes_visited", c.dfs_nodes_visited);
        f("cor4_evaluations", c.cor4_evaluations);
        f("cor4_reused", c.cor4_reused);
        f("inherited_bound_prunes", c.inherited_bound_prunes);
        f("supports_emitted", c.supports_emitted);
        f("supports_checksum", c.supports_checksum);
        f("d_max_sum", c.d_max_sum);
        f("d_max_max", c.d_max_max);
        f("batches_pushed", c.batches_pushed);
        f("batches_coprocessed", c.batches_coprocessed);
        f("consumer_waits", c.consumer_waits);
        f("consumer_wait_ns", c.consumer_wait_ns);
        f("regions_split", c.regions_split);
        f("region_step_downs", c.region_step_downs);
        f("steal_requests", c.steal_requests);
        f("steal_refusals", c.steal_refusals);
        f("steal_withdrawals", c.steal_withdrawals);
        f("worker_idle_ns", c.worker_idle_ns);
        f("steal_wait_ns", c.steal_wait_ns);
        f("steal_backoff_ns", c.steal_backoff_ns);
        f("split_ns", c.split_ns);
        f("requests_refused_idle", c.requests_refused_idle);
        f("requests_refused_busy", c.requests_refused_busy);
        each(c.splits_at_depth, "splits_at_depth", f);

        f("lpr_jacobi_calls", c.lpr_jacobi_calls);
        f("lpr_legendre_survivors", c.lpr_legendre_survivors);
        f("lpr_root_powm_count", c.lpr_root_powm_count);
        f("lpr_tree_powm_count", c.lpr_tree_powm_count);
        each(c.winners_by_class, "winners_by_class", f);
    }

    // Bytes covered by for_each_field (must equal sizeof(Counters))
    static std::size_t field_bytes() {
        Counters c;
        std::size_t bytes = 0;
        for_each_field(c, [&](const std::string&, std::uint64_t& v) { bytes += sizeof(v); });
        return bytes;
    }

private:
    template <typename Array>
    static void add(Array& into, const Array& from) {
        for (std::size_t i = 0; i < into.size(); ++i) into[i] += from[i];
    }

    template <typename Array, typename F>
    static void each(Array& array, const char* name, F& f) {
        for (std::size_t i = 0; i < array.size(); ++i) {
            f(std::string(name) + "." + std::to_string(i), array[i]);
        }
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
