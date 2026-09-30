#include "primality.hpp"
#include "parameters.hpp"

#include <cassert>

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
    assert(p >= 3 && n == p - 1);
    // Use 3 for Legendre's Filter

    /* Order criterion v1:
     Part 1:
     For gcd(g, p) = 1,
         g^k == 1 (mod p)  <=>  ord_p(g) divides k.
    
     Here n = p - 1. First check g^n == 1 (mod p), so ord_p(g) divides n.

     Part 2:
     If ord_p(g) were a proper divisor of n, then for some prime q | n,
         ord_p(g) divides n / q,
     and hence g^(n/q) == 1 (mod p).
    
     Therefore, if g^n == 1 (mod p) but g^(n/q) != 1 (mod p)
     for every distinct prime divisor q of n, then ord_p(g) = n = p - 1.
    
     An element of order p - 1 also certifies that p is prime.
     Since g is tested in increasing order, the first such g is g(p),
     the least primitive root modulo p.
    */

    /* Order criterion v2:
     * The Legendre symbol of g in Zp is (g p). 
     * It is equal to 0 if gcd(g, p) != 1, 1 if g has a square root mod p and
     * -1 if it does not have a square root mod p.
     *
     * Euler's criterion states that (g p) = g^(n/2) (mod p) where n = p-1
     * Thus, g^n == 1 (mod p) <=> g^(n/2) == -1 (mod p) <==> (g p) == -1
     * 
     * We compute this symbol via mpz_legendre before continuing 
     * with remaining candidates via the method described in v1 part 2.
     * (with the knowledge that gcd(g, p) == 1 and g is not a quadratic residue).
     *
     * Note: this is actually the Jacobi filter which satisfies the same criterion for our
     * purposes (but does not require p be prime - which we do not know for certain).
     */

    // TODO - we can optimise heap allocations in exponent etc using thread_local buffers
    //  slightly, but we'll see how good that is.
    mpz_class residue;
    mpz_class base;
    for (unsigned g = 2; g <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT; ++g) {
        mpz_set_ui(base.get_mpz_t(), g);

        if (mpz_legendre(base.get_mpz_t(), p.get_mpz_t()) != -1) {
            continue;
        }

        // Reaching this point means g^n == 1 (mod p), which implies
        // gcd(g, p) = 1. Hence ord_p(g) is defined and divides n.
        // In addition, we know that g is not a quadratic residue, 
        // ie, g^(n/2) != 1 (mod p) and hence we can skip this prime.
        // TODO - use that 2 is a definite prime factor of p-1
        bool full_order = true;

        for (unsigned q : prime_divisors) {
            if (q == 2) { // Checked via Legendre filter
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

