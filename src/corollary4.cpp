#include "corollary4.hpp"

#include <cassert>

Corollary4Threshold make_corollary4_threshold(
    const mpz_class& Q,
    const mpz_class& S
) {
    // TODO - Q is not correct, should not sum/prod some of the primes!
    // N_delta = Q - S (this represents delta * Q, the numerator of delta)
    mpz_class N_delta = Q - S;
    assert(N_delta > 0); // Equivalent to delta > 0

    // Compute F = num_F / den_F = (16 * N_delta + 232 * Q) / N_delta
    mpz_class num_F = 16 * N_delta + 232 * Q;
    mpz_class den_F = N_delta;

    // Reduce fraction ONCE via GCD (replacing mpq_canonicalize)
    mpz_class g;
    mpz_gcd(g.get_mpz_t(), num_F.get_mpz_t(), den_F.get_mpz_t());
    if (g > 1) {
        mpz_divexact(num_F.get_mpz_t(), num_F.get_mpz_t(), g.get_mpz_t());
        mpz_divexact(den_F.get_mpz_t(), den_F.get_mpz_t(), g.get_mpz_t());
    }

    // Pre-allocate memory for 16th powers to avoid reallocations
    Corollary4Threshold threshold;
    
    // Estimate required bits: 16 * num_bits + safety margin
    size_t num_bits = mpz_sizeinbase(num_F.get_mpz_t(), 2) * 16 + 64;
    size_t den_bits = mpz_sizeinbase(den_F.get_mpz_t(), 2) * 16 + 64;

    mpz_init2(threshold.numerator.get_mpz_t(), num_bits);
    mpz_init2(threshold.denominator.get_mpz_t(), den_bits);

    // Exponentiation
    mpz_pow_ui(
        threshold.numerator.get_mpz_t(),
        num_F.get_mpz_t(),
        16
    );
    threshold.numerator *= 4220;

    mpz_pow_ui(
        threshold.denominator.get_mpz_t(),
        den_F.get_mpz_t(),
        16
    );

    return threshold;
}

bool corollary4_proves_grosswald(
    const mpz_class& p,
    const Corollary4Threshold& threshold
) {
    return p * threshold.denominator > threshold.numerator;
}
