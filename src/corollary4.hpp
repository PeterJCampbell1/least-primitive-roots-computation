#pragma once

#include <gmpxx.h>

struct Corollary4Threshold {
    mpz_class lhs; // 4220 * Fn^16
    mpz_class rhs; // d*(Q*Fd) + Fd
    mpz_class increment; // Q*Fd
};

// Warning: only 1 Corollary4Threshold should exist per thread at any time!
Corollary4Threshold& get_thread_local_threshold();

unsigned create_corollary4_threshold(
    const mpz_class& full_prod,
    const mpz_class& Q,
    const mpz_class& SOP,
    Corollary4Threshold& threshold = get_thread_local_threshold()
);

/*
bool corollary4_proves_grosswald(
    const unsigned& d,
    Corollary4Threshold& threshold = get_thread_local_threshold()
);
*/

