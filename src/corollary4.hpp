#ifndef LPR_COROLLARY4_HPP
#define LPR_COROLLARY4_HPP

#include <gmpxx.h>
#include <vector>

struct Corollary4Threshold {
    mpz_class numerator;
    mpz_class denominator;
};

Corollary4Threshold make_corollary4_threshold(
    const mpz_class& Q,
    const mpz_class& S
);

bool corollary4_proves_grosswald(
    const mpz_class& p,
    const Corollary4Threshold& threshold
);

#endif
