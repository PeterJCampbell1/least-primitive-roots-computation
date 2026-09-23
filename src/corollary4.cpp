#include "corollary4.hpp"

#include <cassert>

Corollary4Threshold make_corollary4_threshold(
    const std::vector<unsigned>& support
) {
    assert(support.size() == 33);

    // Corollary 4 with omega = 33 and s = 30.
    mpq_class delta = 1;
    for (std::size_t j = 3; j < 33; ++j) {
        delta -= mpq_class(1, support[j]);
    }

    assert(delta > 0);

    mpq_class F = 8 * (2 + 29 / delta);
    F.canonicalize();

    Corollary4Threshold threshold;

    mpz_pow_ui(
        threshold.numerator.get_mpz_t(),
        F.get_num_mpz_t(),
        16
    );
    threshold.numerator *= 4220;

    mpz_pow_ui(
        threshold.denominator.get_mpz_t(),
        F.get_den_mpz_t(),
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