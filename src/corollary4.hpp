#pragma once

#include <gmpxx.h>
#include <vector>

struct Corollary4Threshold {
    mpz_class lhs; // SIEVE_FACTOR * (2^(w-s) * ((s+1)*Q - 2*SOP))^16
    mpz_class rhs; // (Q - SOP)^16
};

// Warning: only 1 Corollary4Threshold should exist per thread at any time!
Corollary4Threshold& get_thread_local_threshold();

/**
 * Uses corollary 4 to compute the largest value of Q*d (for p = Q*d + 1)
 * that corollary 4 does not automatically prove Grosswald's conjecture for,
 * given the elementary symmetric polynomials of the last S primes of a support.
 * Writes the bound into qd_bound (0 if corollary 4 covers every p).
 *
 * The bound only decreases as the support's primes increase, so the bound
 * for a dominating support also bounds every support it dominates.
 **/
void corollary4_qd_bound(
    const mpz_class& Q,
    const mpz_class& SOP,
    mpz_class& qd_bound,
    Corollary4Threshold& threshold = get_thread_local_threshold()
);

/**
 * Global bound: the Corollary 4 bound on Q*d for the support made of the
 * first OMEGA primes. Every support is dominated by this one, so its bound
 * is at least every support's bound, and Corollary 4 covers every p with
 * p - 1 > global bound. Requires primes to contain the first OMEGA primes.
 **/
void corollary4_global_bound(
    const std::vector<unsigned>& primes,
    mpz_class& global_bound
);
