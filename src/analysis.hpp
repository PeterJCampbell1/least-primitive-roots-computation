#pragma once

#include <gmpxx.h>
#include <iostream>
#include "corollary4.hpp"

inline void corollary4_eval_and_log(
    const mpz_class& p
) {
    thread_local static mpz_class lhs;
    thread_local static mpz_class difference;
    const Corollary4Threshold threshold = get_thread_local_threshold();

    // lhs = p * denominator
    mpz_mul(lhs.get_mpz_t(), p.get_mpz_t(), threshold.denominator.get_mpz_t());

    // difference = lhs - numerator
    mpz_sub(difference.get_mpz_t(), lhs.get_mpz_t(), threshold.numerator.get_mpz_t());

    const bool pass = (mpz_sgn(difference.get_mpz_t()) > 0);
}

