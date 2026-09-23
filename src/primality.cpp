#include "primality.hpp"

#include <cassert>

bool is_composite_base2_fermat(const mpz_class& p)
{
    assert(p >= 2);
    if (p == 2) {
        return false;
    }

    const mpz_class base = 2;
    const mpz_class exponent = p - 1;
    mpz_class residue;
    mpz_powm(residue.get_mpz_t(), base.get_mpz_t(),
             exponent.get_mpz_t(), p.get_mpz_t());
    return residue != 1;
}

std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const mpz_class& n,
    const std::vector<unsigned>& prime_divisors,
    unsigned search_limit
) {
    assert(p >= 2 && n == p - 1);

    // Order criterion:
    // For gcd(g, p) = 1,
    //     g^k == 1 (mod p)  <=>  ord_p(g) divides k.
    //
    // Here n = p - 1. First check g^n == 1 (mod p), so ord_p(g) divides n.
    // If ord_p(g) were a proper divisor of n, then for some prime q | n,
    //     ord_p(g) divides n / q,
    // and hence g^(n/q) == 1 (mod p).
    //
    // Therefore, if g^n == 1 (mod p) but g^(n/q) != 1 (mod p)
    // for every distinct prime divisor q of n, then ord_p(g) = n = p - 1.
    //
    // An element of order p - 1 also certifies that p is prime.
    // Since g is tested in increasing order, the first such g is g(p),
    // the least primitive root modulo p.
    mpz_class residue;
    for (unsigned g = 2; g <= search_limit; ++g) {
        const mpz_class base = g;

        mpz_powm(
            residue.get_mpz_t(),
            base.get_mpz_t(),
            n.get_mpz_t(),
            p.get_mpz_t()
        );

        if (residue != 1) {
            continue;
        }

        // Reaching this point means g^n == 1 (mod p), which implies
        // gcd(g, p) = 1. Hence ord_p(g) is defined and divides n.
        bool full_order = true;

        for (unsigned q : prime_divisors) {
            const mpz_class exponent = n / q;

            mpz_powm(
                residue.get_mpz_t(),
                base.get_mpz_t(),
                exponent.get_mpz_t(),
                p.get_mpz_t()
            );

            if (residue == 1) {
                full_order = false;
                break;
            }
        }

        if (full_order) {
            return mpz_class(g);
        }
    }

    return std::nullopt;
}
