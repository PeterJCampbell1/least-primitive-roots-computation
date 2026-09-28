#include "corollary4.hpp"
#include "parameters.hpp"

#include <cassert>
#include <iostream> // TODO - REMOVE

Corollary4Threshold make_corollary4_threshold(
    const mpz_class& Q,
    const mpz_class& SOP
) {
    // Reusable memory 
    // WARNING: the function should not be called again before these variables need to be reused
    thread_local static mpz_class num_F;
    thread_local static mpz_class num_delta;
    thread_local static mpz_class g;

    // Set up constants
    using parameters::OMEGA;
    using parameters::S;
    constexpr unsigned C1 = S + 1;
    static_assert(OMEGA > S, "Check configuration of OMEGA/S");
    constexpr std::size_t SHIFT = OMEGA - S;
    
    /*std::cout << "Config\n:" << "OMEGA: " << OMEGA << '\n'
        << "S: " << S << '\n' 
        << "SHIFT: " << SHIFT << '\n'
        << "C1: " << C1 << '\n'
        << "Q:   " << Q << '\n'
        << "SOP: " << SOP << '\n';*/

    assert(Q > SOP); // Equivalent to delta > 0

    // numerator of delta
    num_delta = Q - SOP;
                        
    // Compute num_F = 2^(w-s) * ((s+1)*Q - 2*SOP)
    mpz_mul_ui(num_F.get_mpz_t(), Q.get_mpz_t(), C1); // num_F = (s+1) * Q
    mpz_submul_ui(num_F.get_mpz_t(), SOP.get_mpz_t(), 2); // num_F -= 2*SOP
    mpz_mul_2exp(num_F.get_mpz_t(), num_F.get_mpz_t(), SHIFT); // num_F *= 2^(w-s)

    // Reduce fraction via GCD
    mpz_gcd(g.get_mpz_t(), num_F.get_mpz_t(), num_delta.get_mpz_t());
    if (mpz_cmp_ui(g.get_mpz_t(), 1) > 0) {
        mpz_divexact(num_F.get_mpz_t(), num_F.get_mpz_t(), g.get_mpz_t());
        mpz_divexact(num_delta.get_mpz_t(), num_delta.get_mpz_t(), g.get_mpz_t());
    }

    /* TODO can we optimise this out since every thread should use
          its own threshold until it's finished with the current support?
          Should add to DFS State tracker.
    */
    Corollary4Threshold threshold;

    // Exponentiation
    mpz_pow_ui(
            threshold.numerator.get_mpz_t(),
            num_F.get_mpz_t(),
            16
    );
    mpz_mul_ui(threshold.numerator.get_mpz_t(), threshold.numerator.get_mpz_t(), 4220);

    mpz_pow_ui(
            threshold.denominator.get_mpz_t(),
            num_delta.get_mpz_t(),
            16
    );

    return threshold;
}

bool corollary4_proves_grosswald(
    const mpz_class& p,
    const Corollary4Threshold& threshold
) {
    thread_local static mpz_class lhs;

    // p * den_F > num_F
    mpz_mul(lhs.get_mpz_t(), p.get_mpz_t(), threshold.denominator.get_mpz_t());
    return mpz_cmp(lhs.get_mpz_t(), threshold.numerator.get_mpz_t()) > 0;
}
