#pragma once

#include "parameters.hpp"
#include <cstdint>
#include <iostream>
#include <gmpxx.h>
#include <mutex>
#include <array>
#include <type_traits>

namespace { // Anonymous namespace for internal usage

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
    std::array<int, parameters::NUM_CHUNKS> composite_caught_at = {};
    mpz_class largest_least_primitive_root = 0;

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

        for (size_t i = 0; i < composite_caught_at.size(); ++i) {
            composite_caught_at[i] += other.composite_caught_at[i];
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
                  << small_prime_composite_count << '\n'
                  << "Primes were each caught at: ";
        for (int x : composite_caught_at) {
            std::cout << x << " ";
        }
        std::cout << "\n";
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
        composite_caught_at = other.composite_caught_at;
        largest_least_primitive_root = other.largest_least_primitive_root;
    }
};

} // namespace

namespace lpr::stats {

// 1. Declarations / Aliases
using LocalStats = LPRStats<false>;
using MasterStats = LPRStats<true>;

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

} // namespace lpr::stats

