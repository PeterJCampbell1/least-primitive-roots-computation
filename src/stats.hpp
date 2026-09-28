#ifndef STATS_HPP
#define STATS_HPP

#include <cstdint>
#include <iostream>

struct LPRStats {
	std::uint64_t total_support_count = 0;
    std::uint64_t surviving_support_count = 0;
    std::uint64_t candidate_count = 0;
    std::uint64_t fermat_composite_count = 0;
    std::uint64_t fermat_survivor_count = 0;
    std::uint64_t certified_count = 0;
    std::uint64_t unresolved_count = 0;
    std::uint64_t certified_inequality_pass_count = 0;
    std::uint64_t certified_inequality_fail_count = 0;
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
                  << "Proved composite by base-2 Fermat test: "
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
};

#endif // STATS_HPP
