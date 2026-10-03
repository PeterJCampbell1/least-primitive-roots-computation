#include "primality.hpp"
#include "parameters.hpp"

#include <numeric>
#include <cassert>

bool is_composite_small_prime(
    const mpz_class& p
) {
    assert(p >= 3);
    
    // Alternative method that first does trial gcd
    // TODO - CHECK IF THIS IDEA CAN BE USED
    using parameters::PRIMORIAL_CHUNKS;
    using parameters::NUM_CHUNKS;
    for (size_t i = 0; i < NUM_CHUNKS; ++i) {
        unsigned long rem = mpz_tdiv_ui(p.get_mpz_t(), PRIMORIAL_CHUNKS[i]);
        if (std::gcd(rem, PRIMORIAL_CHUNKS[i]) > 1) {
            return true;
        }
    }
    return false;
    // return mpz_probab_prime_p(p.get_mpz_t(), 1) == 0; // Alternative idea
}

/**
 * If p>2 is prime, then gcd(2, p) = 1
 * so by Fermat's little theorem, 
 * 2^n == 1 (mod p) where n = p-1
 *
 * True means p is definitely composite.
 * False means this filter provides no definite result.
 * 
 * Note: We assume that p > 2 
 *  (Grosswald does not need to be tested for small p)
 */
bool is_composite_base2_fermat(
    const mpz_class& p,
    const mpz_class& n
) {
    assert(p >= 3 && n == p - 1);
    static const mpz_class base = 2;
    static thread_local mpz_class residue;

    mpz_powm(
        residue.get_mpz_t(), 
        base.get_mpz_t(),
        n.get_mpz_t(), 
        p.get_mpz_t()
    );
    return residue != 1;
}


std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const mpz_class& n,
    const std::vector<unsigned>& prime_divisors
) {
    assert(p > 2 && n == p - 1);
    assert(mpz_odd_p(p.get_mpz_t()));

    // Conditional order criterion:
    //
    // This function is required to identify the least primitive root
    // correctly when p is prime. It does not certify that p is prime.
    //
    // Suppose p is prime. Then Fermat's theorem gives
    //
    //     g^n == 1 (mod p),    where n = p - 1.
    //
    // A primitive root modulo p must be a quadratic nonresidue.
    // Since the Jacobi symbol equals the Legendre symbol for prime p,
    //
    //     Jacobi(g,p) == -1
    //
    // implies, by Euler's criterion,
    //
    //     g^(n/2) == -1 (mod p).
    //
    // Thus, conditional on p being prime, the Jacobi test handles
    // the q = 2 order condition without a modular exponentiation.
    //
    // For every odd prime divisor q of n, we explicitly require
    //
    //     g^(n/q) != 1 (mod p).
    //
    // Therefore, if p is prime and all these tests pass,
    // ord_p(g) = n = p - 1, so g is a primitive root modulo p.
    //
    // Since g is tested in increasing order, the first successful g
    // is the least primitive root g(p), provided p is prime.
    //
    // For composite p, these tests need not have this interpretation;
    // the behaviour on composite inputs is irrelevant to Grosswald's
    // conjecture, which concerns prime p only.
    //
    // A failure of this finding least primitive root either implies:
    //   1) p is prime and it's least primitive root is greater than PRIMITIVE_ROOT_SEARCH_LIMIT
    //   2) p is composite
    // Both cases must be checked by hand for any failures.

    mpz_class residue;

    for (unsigned g = 2;
 		 g <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT;
         ++g) {

        const mpz_class base = g;

        // If p is prime, a primitive root must be a quadratic
        // nonresidue, so its Jacobi/Legendre symbol must be -1.
        if (mpz_jacobi(
                base.get_mpz_t(),
                p.get_mpz_t()
            ) != -1) {
            continue;
        }

        bool primitive_root_if_prime = true;

        for (unsigned q : prime_divisors) {

            // For prime p, this condition is already supplied by
            // Jacobi(g,p) == -1 via Euler's criterion.
            if (q == 2) {
                continue;
            }

            const mpz_class exponent = n / q;

            mpz_powm(
                residue.get_mpz_t(),
                base.get_mpz_t(),
                exponent.get_mpz_t(),
                p.get_mpz_t()
            );

            if (residue == 1) {
                primitive_root_if_prime = false;
                break;
            }
        }

        if (primitive_root_if_prime) {
            return mpz_class(g);
        }
    }

    return std::nullopt;
}


/*
std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const mpz_class& n,
    const std::vector<unsigned>& prime_divisors
) {
    assert(p > 2 && n == p - 1);
    assert(mpz_odd_p(p.get_mpz_t()));

    const mpz_class half_n = n / 2;

    // Order criterion:
    //
    // Let n = p - 1. We do not yet assume that p is prime.
    //
    // First, the Jacobi symbol is used only as a cheap filter.
    // If p is prime and g is a primitive root modulo p, then g is a
    // quadratic nonresidue, so (g/p) = -1. Thus a value with Jacobi
    // symbol != -1 cannot be the least primitive root of a prime p.
    //
    // For a surviving g, explicitly require
    //
    //     g^(n/2) == -1 (mod p).
    //
    // This implies gcd(g,p) = 1 and, after squaring,
    //
    //     g^n == 1 (mod p).
    //
    // Hence ord_p(g) is defined and divides n. Also,
    // ord_p(g) does not divide n/2.
    //
    // For every odd prime divisor q of n, we then require
    //
    //     g^(n/q) != 1 (mod p).
    //
    // If ord_p(g) were a proper divisor of n, then ord_p(g) would
    // divide n/q for some prime q | n. The q = 2 case has already
    // been excluded by g^(n/2) == -1, and the remaining odd q are
    // checked explicitly.
    //
    // Therefore ord_p(g) = n = p - 1.
    //
    // An element modulo p of order p - 1 also certifies that p is prime.
    // Since g is tested in increasing order, the first successful g is
    // the least primitive root g(p).
    mpz_class residue;
    for (unsigned g = 2; g <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT; ++g) {
        const mpz_class base = g;

        // A primitive root modulo a prime must be a quadratic nonresidue.
        // For prime p, the Jacobi symbol is the Legendre symbol.
        if (mpz_jacobi(
                base.get_mpz_t(),
                p.get_mpz_t()
            ) != -1) {
            continue;
        }

        mpz_powm(
            residue.get_mpz_t(),
            base.get_mpz_t(),
            half_n.get_mpz_t(),
            p.get_mpz_t()
        );

        // Require g^(n/2) == -1 == p - 1 == n (mod p).
        if (residue != n) {
            continue;
        }

        // Reaching this point means g^n == 1 (mod p), which implies
        // gcd(g, p) = 1. Hence ord_p(g) is defined and divides n.
        // Since g^(n/2) == -1 (mod p),
        //     g^n == 1 (mod p),
        // and g^(n/2) != 1 (mod p).
        //
        // Thus ord_p(g) is defined, divides n,
        // and does not divide n/2.
        bool full_order = true;

        for (unsigned q : prime_divisors) {
            // Already handled by g^(n/2) == -1.
            if (q == 2) {
                continue;
            }

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
*/

