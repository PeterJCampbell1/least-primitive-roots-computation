#ifndef LPR_COROLLARY4_HPP
#define LPR_COROLLARY4_HPP

#include <gmpxx.h>
#include <vector>

struct Corollary4Threshold {
    mpz_class numerator;
    mpz_class denominator;
};

// Warning: only 1 Corollary4Threshold should exist per thread at any time!
Corollary4Threshold& get_thread_local_threshold();

void update_corollary4_threshold(
    const mpz_class& Q,
    const mpz_class& S,
    Corollary4Threshold& threshold = get_thread_local_threshold()
);

bool corollary4_proves_grosswald(
    const mpz_class& p,
    const Corollary4Threshold& threshold = get_thread_local_threshold()
);

#endif
