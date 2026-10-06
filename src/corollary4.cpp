#include "corollary4.hpp"
#include "parameters.hpp"

#include <gmpxx.h>
#include <cassert>

Corollary4Threshold& get_thread_local_threshold() {
    thread_local static Corollary4Threshold instance;
    return instance;
}

// TODO - UPDATE SO THIS RETURNS THE MAXIMUM VALUE OF d IN p = Q*d + 1
/**
 * Uses corollary 4 to determine the maximum value of d in p = Q*d+1 where
 * corollary 4 does not automatically prove Grosswald's conjecture.
 **/
unsigned create_corollary4_threshold(
    const mpz_class& full_prod,
    const mpz_class& Q,
    const mpz_class& SOP,
    Corollary4Threshold& threshold
) {
    // Set up constants
    using parameters::OMEGA;
    using parameters::S;
    constexpr unsigned C1 = S + 1;
    static_assert(OMEGA > S, "Check configuration of OMEGA/S");
    constexpr std::size_t SHIFT = OMEGA - S;

    static thread_local mpz_class d_max;


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
    mpz_mul_ui(
        threshold.lhs.get_mpz_t(), 
        threshold.lhs.get_mpz_t(), 
        4220
    );

    mpz_pow_ui(
            threshold.rhs.get_mpz_t(),
            threshold.rhs.get_mpz_t(),
            16
    );

    // RHS(d) = d*(Q*Fd) + Fd (begin with d = 1) TODO - EDIT THIS - DON'T WANT THIS ANYMORE!
    /*
    mpz_mul(
        threshold.increment.get_mpz_t(), 
        threshold.rhs.get_mpz_t(), 
        full_prod.get_mpz_t()
    );
    mpz_add(
        threshold.rhs.get_mpz_t(), 
        threshold.rhs.get_mpz_t(), 
        threshold.increment.get_mpz_t()
    );*/
    
    // TODO - CHECK IF WE CAN DO THIS MORE EFFECTIVELY, JUST A SIMPLE THING TO MAKE IT WORK FOR NOW

    // TODO - CHECK IF THIS ACTUALLY WORKS:
    /*if (threshold.lhs < threshold.rhs) {
        return false; // Using bool to indicate if any d are possible
    }*/

    // Want maximum d s.t. Q*d + 1 <= lhs/rhs
    // d_max = floor( (lhs/rhs - 1) / Q ) = floor( (lhs - rhs) / (Q*rhs) )
    // TODO - Can threshold.rhs ever be greater than threshold.lhs?
    if (threshold.lhs < threshold.rhs) {
        return 0;
    }
    d_max = (threshold.lhs - threshold.rhs) / (full_prod * threshold.rhs);

    // We assume d_max fits in a uint64_t for now
    assert(d_max.fits_ulong_p() && "d_max exceeds unsigned integer range");
    return d_max.get_ui();
}

/*
bool corollary4_proves_grosswald(
    const unsigned& d,
    Corollary4Threshold& threshold
) {
    // lhs < (Q*d + 1)*p
    // d is delta d - TODO UPDATE NAMING FOR ALL THIS
    if (d > 0) {
        // RHS(d) = d*(Q*Fd) + Fd (begin with d = 1)
        mpz_addmul_ui(
            threshold.rhs.get_mpz_t(),     
            threshold.increment.get_mpz_t(), 
            d                     
        );
    }
    return mpz_cmp(threshold.lhs.get_mpz_t(), threshold.rhs.get_mpz_t()) < 0;
}
*/

