#ifndef STATS_HPP
#define STATS_HPP

#include "parameters.hpp"
#include <cstdint>
#include <iostream>
#include <gmpxx.h>

struct LPRStats {
	std::uint64_t total_support_count = 0;
    std::uint64_t surviving_support_count = 0;
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

    void print() const {
		std::cout << "Search summary:\n"
                  << "---------------\n"
                  << "K-prime supports examined: "
                  << total_support_count << '\n'
                  << "Supports requiring candidate search after Corollary 4: "
                  << surviving_support_count << '\n'
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

	void merge(const LPRStats& other) {
		total_support_count += other.total_support_count;
		surviving_support_count += other.surviving_support_count;
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
};

inline LPRStats& get_thread_local_stats() {
    static thread_local LPRStats stats;
    return stats;
}

#endif // STATS_HPP
