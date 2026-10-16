#include "corollary4.hpp"
#include "config.hpp"
#include "parameters.hpp"

#include <gmpxx.h>
#include <cassert>

Corollary4Threshold& get_thread_local_threshold() {
    thread_local static Corollary4Threshold instance;
    return instance;
}

void corollary4_qd_bound(
    const mpz_class& Q,
    const mpz_class& SOP,
    mpz_class& qd_bound,
    Corollary4Threshold& threshold
) {
    // Set up constants
    const auto& config = lpr::config::search_config();
    const unsigned C1 = config.s + 1;
    assert(config.omega > config.s && "Check configuration of OMEGA/S");
    const std::size_t SHIFT = config.omega - config.s;

    assert(Q > SOP); // Equivalent to delta > 0

    // numerator of delta
    mpz_sub(threshold.rhs.get_mpz_t(), Q.get_mpz_t(), SOP.get_mpz_t());
                        
    // Compute lhs = 2^(w-s) * ((s+1)*Q - 2*SOP)
    mpz_mul_ui(threshold.lhs.get_mpz_t(), Q.get_mpz_t(), C1); // threshold.lhs = (s+1) * Q
    mpz_submul_ui(threshold.lhs.get_mpz_t(), SOP.get_mpz_t(), 2); // threshold.lhs -= 2*SOP
    mpz_mul_2exp(threshold.lhs.get_mpz_t(), threshold.lhs.get_mpz_t(), SHIFT); // threshold.lhs *= 2^(w-s)
                                                               
    // Exponentiation
    mpz_pow_ui(
            threshold.lhs.get_mpz_t(),
            threshold.lhs.get_mpz_t(),
            16
    );
    
    /* 
     * Sieve factor is (4/9)*pi^8 + epsilon ~= 4217.13
     * We use the upper bound of 4218 for simplicity.
     */
    
    constexpr unsigned SIEVE_FACTOR_MULT = 4218;
    mpz_mul_ui(
        threshold.lhs.get_mpz_t(), 
        threshold.lhs.get_mpz_t(), 
        SIEVE_FACTOR_MULT
    );

    mpz_pow_ui(
            threshold.rhs.get_mpz_t(),
            threshold.rhs.get_mpz_t(),
            16
    );

    // Want maximum Q*d s.t. Q*d + 1 <= lhs/rhs
    // qd_bound = floor(lhs/rhs - 1) = floor( (lhs - rhs) / rhs )
    if (threshold.lhs < threshold.rhs) {
        qd_bound = 0;
        return;
    }
    mpz_sub(threshold.lhs.get_mpz_t(), threshold.lhs.get_mpz_t(), threshold.rhs.get_mpz_t());
    mpz_fdiv_q(qd_bound.get_mpz_t(), threshold.lhs.get_mpz_t(), threshold.rhs.get_mpz_t());
}

void corollary4_global_bound(
    const std::vector<unsigned>& primes,
    mpz_class& global_bound
) {
    const auto& config = lpr::config::search_config();
    const std::size_t OMEGA = config.omega;
    const std::size_t S = config.s;
    assert(primes.size() >= OMEGA);

    // Elementary symmetric polynomials of the last S of the first OMEGA primes,
    // using the same recurrence as the DFS
    mpz_class Q = 1;
    mpz_class SOP = 0;
    for (std::size_t j = OMEGA - S; j < OMEGA; ++j) {
        SOP *= primes[j];
        SOP += Q;
        Q *= primes[j];
    }
    corollary4_qd_bound(Q, SOP, global_bound);
}
