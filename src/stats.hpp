#pragma once

#include "parameters.hpp"
#include "trial_division.hpp"
#include <cstdint>
#include <iostream>
#include <gmpxx.h>
#include <mutex>
#include <array>
#include <type_traits>

namespace lpr::stats::detail { // Internal; named (not anonymous) as this header is included from several .cpp files

// Dummy lock for single-threaded/thread-local case 
struct NullMutex {
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

// TODO - CHECK THIS CODE OVER!

// Add mutex to struct if master statistics struct
// If not a master, then the struct should be only used thread-locally
template <bool IsMaster = false>
struct LPRStats {
    // Select real std::mutex or zero-cost NullMutex at compile time
    using MutexType = std::conditional_t<IsMaster, std::mutex, NullMutex>;
    mutable MutexType mutex_;

    std::uint64_t total_support_count = 0;
    std::uint64_t candidate_count = 0;
    std::uint64_t cor4_proves_grosswald_count = 0;
    mpz_class max_d = 0;
    std::uint64_t small_prime_composite_count = 0;
    std::uint64_t fermat_composite_count = 0;
    std::uint64_t fermat_survivor_count = 0;
    std::uint64_t certified_count = 0;
    std::uint64_t unresolved_count = 0;
    std::uint64_t certified_inequality_pass_count = 0;
    std::uint64_t certified_inequality_fail_count = 0;
    std::array<int, parameters::PRIMITIVE_ROOT_SEARCH_LIMIT+2> prim_roots_count = {};
    std::array<int, parameters::PRIMITIVE_ROOT_SEARCH_LIMIT+2> surviving_legendre_count = {};
    // Trial division composites by chunk (index as in lpr::trial_division::table().chunks)
    std::array<std::uint64_t, parameters::MAX_TRIAL_DIVISION_CHUNKS> composite_caught_at = {};
    mpz_class largest_least_primitive_root = 0;

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

    // Least primitive root search statistics (find_least_primitive_root)
    std::uint64_t lpr_jacobi_calls = 0; // Candidates g reaching the Jacobi filter
    std::uint64_t lpr_legendre_survivors = 0; // Candidates g tested with the power tree
    std::uint64_t lpr_root_powm_count = 0; // g^(2d) computations (root of the power tree)
    std::uint64_t lpr_tree_powm_count = 0; // mpz_powm calls inside the power tree
    std::uint64_t lpr_reuse_candidates = 0; // Tested g with some nodes derivable from earlier g (same p)
    std::uint64_t lpr_reuse_tree_powm = 0; // Upper bound on tree mpz_powm calls avoidable by reuse
    std::array<std::uint64_t, 16> winners_by_class = {}; // Index (p mod 8)*2 + (d mod 2)

    LPRStats() = default;

    // Custom copy constructor & assignment operator (needed because std::mutex is non-copyable)
    LPRStats(const LPRStats& other) {
        std::lock_guard<MutexType> lock(other.mutex_);
        copy_fields_from(other);
    }

    LPRStats& operator=(const LPRStats& other) {
        if (this != &other) {
            // Lock both to ensure thread safety during assignment
            std::scoped_lock lock(mutex_, other.mutex_);
            copy_fields_from(other);
        }
        return *this;
    }

    void merge(const LPRStats<false>& other) {
        std::lock_guard<MutexType> lock(mutex_);

        total_support_count += other.total_support_count;
        candidate_count += other.candidate_count;
        cor4_proves_grosswald_count += other.cor4_proves_grosswald_count;
        small_prime_composite_count += other.small_prime_composite_count;
        fermat_composite_count += other.fermat_composite_count;
        fermat_survivor_count += other.fermat_survivor_count;
        certified_count += other.certified_count;
        unresolved_count += other.unresolved_count;
        certified_inequality_pass_count += other.certified_inequality_pass_count;
        certified_inequality_fail_count += other.certified_inequality_fail_count;
        dfs_nodes_visited += other.dfs_nodes_visited;
        cor4_evaluations += other.cor4_evaluations;
        cor4_reused += other.cor4_reused;
        inherited_bound_prunes += other.inherited_bound_prunes;
        supports_emitted += other.supports_emitted;
        d_max_sum += other.d_max_sum;
        batches_pushed += other.batches_pushed;
        batches_coprocessed += other.batches_coprocessed;
        consumer_waits += other.consumer_waits;
        lpr_jacobi_calls += other.lpr_jacobi_calls;
        lpr_legendre_survivors += other.lpr_legendre_survivors;
        lpr_root_powm_count += other.lpr_root_powm_count;
        lpr_tree_powm_count += other.lpr_tree_powm_count;
        lpr_reuse_candidates += other.lpr_reuse_candidates;
        lpr_reuse_tree_powm += other.lpr_reuse_tree_powm;
        for (size_t i = 0; i < winners_by_class.size(); ++i) {
            winners_by_class[i] += other.winners_by_class[i];
        }
        if (other.d_max_max > d_max_max) {
            d_max_max = other.d_max_max;
        }

        for (size_t i = 0; i < composite_caught_at.size(); ++i) {
            composite_caught_at[i] += other.composite_caught_at[i];
        }

        for (size_t i = 0; i < prim_roots_count.size(); ++i) {
            prim_roots_count[i] += other.prim_roots_count[i];
        }

        for (size_t i = 0; i < surviving_legendre_count.size(); ++i) {
            surviving_legendre_count[i] += other.surviving_legendre_count[i];
        }

        if (other.max_d > max_d) {
            max_d = other.max_d;
        }
        if (other.largest_least_primitive_root > largest_least_primitive_root) {
            largest_least_primitive_root = other.largest_least_primitive_root;
        }
    }

    void print() const {
        std::lock_guard<MutexType> lock(mutex_);

        std::cout << "Search summary:\n"
                  << "---------------\n"
                  << "K-prime supports examined: "
                  << total_support_count << '\n'
                  << "Candidates not covered by Corollary 4: "
                  << candidate_count << '\n'
                  << "Number of times proves grosswald was called: " 
                  << cor4_proves_grosswald_count << '\n'
                  << "Largest number of repeated calls to proves grosswald: "
                  << max_d << '\n'
                  << "Proved composite by small prime trial division test: "
                  << small_prime_composite_count << '\n';
        const auto& td = lpr::trial_division::table();
        for (std::size_t k = 0; k < td.chunks.size() && k < composite_caught_at.size(); ++k) {
            const auto& chunk = td.chunks[k];
            std::cout << "  by primes " << td.tests[chunk.first].prime << ".."
                      << td.tests[chunk.first + chunk.count - 1].prime << ": "
                      << composite_caught_at[k] << '\n';
        }
        std::cout << "Proved composite by base-2 Fermat test: "
                  << fermat_composite_count << '\n'
                  << "Surviving base-2 Fermat test: "
                  << fermat_survivor_count << '\n'
                  << "Least primitive root certified: "
                  << certified_count << '\n'
                  << "Grosswald inequality verified: "
                  << certified_inequality_pass_count << '\n'
                  << "Grosswald inequality failed: "
                  << certified_inequality_fail_count << '\n'
                  << "No least primitive root certified within search limit: "
                  << unresolved_count << '\n'
                  << "Largest least primitive root found: "
                  << largest_least_primitive_root << '\n';

        const double certified = certified_count ? static_cast<double>(certified_count) : 1.0;
        std::cout << "\nLeast primitive root search:\n"
                  << "Jacobi filter calls: " << lpr_jacobi_calls
                  << " (" << lpr_jacobi_calls / certified << " per prime)\n"
                  << "Legendre survivors tested: " << lpr_legendre_survivors
                  << " (" << lpr_legendre_survivors / certified << " per prime, the last being g(p))\n"
                  << "Root powm (g^(2d)) calls: " << lpr_root_powm_count << '\n'
                  << "Power tree powm calls: " << lpr_tree_powm_count
                  << " (" << lpr_tree_powm_count / certified << " per prime)\n"
                  << "Tested g with nodes derivable from earlier g: " << lpr_reuse_candidates << '\n'
                  << "Upper bound on power tree powm calls avoidable by reuse: "
                  << lpr_reuse_tree_powm << '\n'
                  << "Winners by p mod 8 (d even / d odd):\n";
        for (unsigned r = 1; r < 8; r += 2) {
            std::cout << "  p = " << r << " mod 8: "
                      << winners_by_class[2*r] << " / " << winners_by_class[2*r + 1] << '\n';
        }

        /*
        std::cout << "Least primitive roots were: " << '\n';
        int i = 0;
        for (int x : prim_roots_count) {
            std::cout << i << " : " << x << '\n';  
            if (i > largest_least_primitive_root) {
                break;
            }
            i++;
        }  

        std::cout << '\n' << '\n' << "Checking for least primitive root count: " << '\n';
        i = 0;
        for (int x : surviving_legendre_count) {
            std::cout << i << " : " << x << '\n';  
            if (i > largest_least_primitive_root) {
                break;
            }
            i++;
        }*/
    }

    void print_producer() const {
        std::lock_guard<MutexType> lock(mutex_);

        std::cout << "Producer summary:\n"
                  << "-----------------\n"
                  << "DFS nodes visited: "
                  << dfs_nodes_visited << '\n'
                  << "Corollary 4 evaluations: "
                  << cor4_evaluations << '\n'
                  << "Corollary 4 results reused by first child: "
                  << cor4_reused << '\n'
                  << "Pruned by inherited Corollary 4 bound: "
                  << inherited_bound_prunes << '\n'
                  << "Supports emitted: "
                  << supports_emitted << '\n'
                  << "Sum of d_max over emitted supports: "
                  << d_max_sum << '\n'
                  << "Largest d_max: "
                  << d_max_max << '\n'
                  << "Batches pushed to queue: "
                  << batches_pushed << '\n'
                  << "Batches co-processed by producer: "
                  << batches_coprocessed << '\n'
                  << "Consumer waits on empty queue: "
                  << consumer_waits << '\n';
    }

private:
    void copy_fields_from(const LPRStats& other) {
        total_support_count = other.total_support_count;
        candidate_count = other.candidate_count;
        cor4_proves_grosswald_count = other.cor4_proves_grosswald_count;
        max_d = other.max_d;
        small_prime_composite_count = other.small_prime_composite_count;
        fermat_composite_count = other.fermat_composite_count;
        fermat_survivor_count = other.fermat_survivor_count;
        certified_count = other.certified_count;
        unresolved_count = other.unresolved_count;
        certified_inequality_pass_count = other.certified_inequality_pass_count;
        certified_inequality_fail_count = other.certified_inequality_fail_count;
        prim_roots_count = other.prim_roots_count;
        surviving_legendre_count = other.surviving_legendre_count;
        composite_caught_at = other.composite_caught_at;
        largest_least_primitive_root = other.largest_least_primitive_root;
        dfs_nodes_visited = other.dfs_nodes_visited;
        cor4_evaluations = other.cor4_evaluations;
        cor4_reused = other.cor4_reused;
        inherited_bound_prunes = other.inherited_bound_prunes;
        supports_emitted = other.supports_emitted;
        d_max_sum = other.d_max_sum;
        d_max_max = other.d_max_max;
        batches_pushed = other.batches_pushed;
        batches_coprocessed = other.batches_coprocessed;
        consumer_waits = other.consumer_waits;
        lpr_jacobi_calls = other.lpr_jacobi_calls;
        lpr_legendre_survivors = other.lpr_legendre_survivors;
        lpr_root_powm_count = other.lpr_root_powm_count;
        lpr_tree_powm_count = other.lpr_tree_powm_count;
        lpr_reuse_candidates = other.lpr_reuse_candidates;
        lpr_reuse_tree_powm = other.lpr_reuse_tree_powm;
        winners_by_class = other.winners_by_class;
    }
};

} // namespace lpr::stats::detail

namespace lpr::stats {

// 1. Declarations / Aliases
using LocalStats = detail::LPRStats<false>;
using MasterStats = detail::LPRStats<true>;

// 2. Master Stats Singleton
inline MasterStats& get_master_stats() {
    static MasterStats master;
    return master;
}

// 3. ManagedStats RAII wrapper
struct ManagedStats {
    LocalStats stats;
    bool flushed = false;
    
    void flush() {
        if (!flushed) {
            get_master_stats().merge(stats);
            stats = LocalStats{};
            flushed = true;
        }
    }

    // Merge into the master and reset, keeping the thread able to publish again
    void publish() {
        if (!flushed) {
            get_master_stats().merge(stats);
            stats = LocalStats{};
        }
    }

    ~ManagedStats() { // Destructor
        flush();
    }
};

// Thread-Local Accessors
inline ManagedStats& get_managed_stats() {
    static thread_local ManagedStats instance;
    return instance;
}

inline LocalStats& get_thread_local_stats() {
    return get_managed_stats().stats;
}

// Global flush helper for the current thread 
// Used for single threaded case, or if we wish to see intermediate
// statistics we can set up a signal handler to print stats on Ctrl+C
inline void flush_thread_local_stats() {
    get_managed_stats().flush();
}

// Makes this thread's statistics so far visible in the master statistics
// (used for periodic progress logging). Call at batch granularity.
inline void publish_thread_local_stats() {
    get_managed_stats().publish();
}

} // namespace lpr::stats

