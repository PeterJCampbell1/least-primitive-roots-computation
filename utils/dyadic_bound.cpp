#include <gmpxx.h>
#include <iostream>

// Computes 'a' given 'y' such that ((a-1)/2^y)^16 <= 4220 <= (a/2^y)^16
mpz_class compute_dyadic_a(unsigned y) {
    mpz_class target;
    // target = 4220 * 2^(16 * y)
    mpz_mul_2exp(target.get_mpz_t(), mpz_class(4220).get_mpz_t(), 16 * y);

    mpz_class a;
    // Computes floor(target^(1/16))
    mpz_root(a.get_mpz_t(), target.get_mpz_t(), 16);

    // Increment to get ceiling: a = ceil( (4220 * 2^(16y))^(1/16) )
    a += 1;
    return a;
}

int main() {
    mpz_class a;
    for (unsigned y = 4; y <= 64; y++) {
        a = compute_dyadic_a(y);
        std::cout << "For y = " << y << ", a = " << a << '\n';
    }
    return 0;
}
