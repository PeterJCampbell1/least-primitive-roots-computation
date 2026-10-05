#include "primality.hpp"
#include "parameters.hpp"
#include "stats.hpp" // TODO - REMOVE LATER

#include <numeric>
#include <cassert>

/* Primality Filters */

bool is_composite_small_prime(
    const mpz_class& p,
    LPRStats& stats
) {
    assert(p >= 3);
    
    // Alternative method that first does trial gcd
    // TODO - CHECK IF THIS IDEA CAN BE USED
    using parameters::PRIMORIAL_CHUNKS;
    using parameters::NUM_CHUNKS;
    for (size_t i = 0; i < NUM_CHUNKS; ++i) {
        unsigned long rem = mpz_tdiv_ui(p.get_mpz_t(), PRIMORIAL_CHUNKS[i]);
        if (std::gcd(rem, PRIMORIAL_CHUNKS[i]) > 1) {
            stats.composite_caught_at[i]++;
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

/**
 * The Miller-Rabin test is a strictly stronger test than the Fermat test.
 * It also enables the possibility of early exits - leading to potentially shorter
 * runtimes than the Fermat test.
 *
 * 
 *
 * Note: We assume that p > 2 
 *  (Grosswald does not need to be tested for small p)
 */
bool is_composite_base2_miller_rabin(
    const mpz_class& p,
    const mpz_class& n
) {
    assert(p >= 3 && n == p - 1);
    static const mpz_class base = 2;
    static thread_local mpz_class x_i;
    static thread_local mpz_class prev_x_i;
    static thread_local mpz_class d;

    // Write n = 2^s * d
    int s = 0;
    d = n;
    while (d > 0 && (d & 1) == 0) {
        s++;
        d >>= 1;
    }

    // Compute x0 = 2^d (mod p)
    mpz_powm(
        x_i.get_mpz_t(), 
        base.get_mpz_t(),
        d.get_mpz_t(), 
        p.get_mpz_t()
    );

    if (x_i == 1) { // Probably prime
        return false;
    }
    
    while (s > 0) {
        // if x_i == -1 == n (mod p) probably prime
        if (x_i == n) {
            return false;
        }

        // x_i = x_{i-1}^2 (mod p)
        x_i.swap(prev_x_i);
        mpz_mul(
            x_i.get_mpz_t(), 
            prev_x_i.get_mpz_t(), 
            prev_x_i.get_mpz_t()
        ); // Square
        mpz_mod(
            x_i.get_mpz_t(), 
            x_i.get_mpz_t(), 
            p.get_mpz_t()
        ); // Reduce
        /*mpz_powm_ui(
            x_i.get_mpz_t(), 
            prev_x_i.get_mpz_t(), 
            2, 
            p.get_mpz_t()
        );*/

        // Definitely composite
        if (x_i == 1) {
            return true;
        }
        s--;
    }

    return true; // Fermat's little theorem not satisfied
}


/* Finding Least Primitive Roots */

void _build_product_tree(
    std::array<mpz_class, parameters::TREE_SIZE>& product_tree,
    const std::vector<unsigned>& prime_divisors,
    const int node,
    const int left,
    const int right // Exclusive
) {
    // Product tree:
    // This tree will be a full complete binary tree.
    // Given omega, the root node contains the full product over all primes except 2.
    // We then at each stage partition the prime divisors into a left and right half,
    // with the left half being at most one greater and no less than the right in terms
    // of number of prime divisors we take the product over.
    // We repeat until we reach leaf nodes, which contain exactly one prime divisor.
    //
    // Note - this could of course be computed during the DFS loop, but it shouldn't 
    // be the bottleneck in general.
    //
    // Example:
    // Prime Divisors = [2, 3, 5, 7, 11, 13] 
    //
    // Root node: 3,5,7,11,13
    //        3,5,7       11,13
    //      3,5   7      11    13
    //     3   5
    //
    // We can represent this in an array as follows:
    // [(3,5,7,11,13), (3,5,7), (11,13), (3,5), 7, 11, 13, 3, 5]

   
    if (left == right - 1) { // Base Case:
        product_tree[node] = prime_divisors[left];
    } else { // Recursive Case:
        int lChild = 2*node + 1;
        int rChild = 2*node + 2;
        int mid = (left + right + 1) / 2; // Ceiling divide to ensure mid - left >= right - mid
        _build_product_tree(product_tree, prime_divisors, lChild, left, mid);
        _build_product_tree(product_tree, prime_divisors, rChild, mid, right);
        product_tree[node] = product_tree[lChild] * product_tree[rChild];
    }
}

void _generate_next_residue(
    std::array<mpz_class, parameters::TREE_SIZE>& power_tree,
    const std::array<mpz_class, parameters::TREE_SIZE>& product_tree,
    const mpz_class& p,
    size_t& leaf_idx
) {
    using parameters::TREE_SIZE; 

    // Starting call at root node
    if (leaf_idx == 0) {
        while (2*leaf_idx + 1 < TREE_SIZE) {
            // Take to the power of right child
            mpz_powm(
                power_tree[2*leaf_idx+1].get_mpz_t(),
                power_tree[leaf_idx].get_mpz_t(),
                product_tree[2*leaf_idx+2].get_mpz_t(), 
                p.get_mpz_t()
            );
            leaf_idx = 2*leaf_idx + 1;
        }
        return;
    }

    // At a leaf node - find next leaf node (in-order traversal, but we compute on descent)

    // Go up until we were not the right child
    // Note - we cannot reach root from the right unless we were the rightmost node
    //      and in that case we should already have completed this algorithm
    size_t parent = (leaf_idx - 1)/ 2;
    while (true) { 
        parent = (leaf_idx - 1) / 2;
        if (leaf_idx % 2 == 1) { // Left child
            break;
        }
        leaf_idx = parent;
    }

    // Go to the right child
    leaf_idx++; 
    mpz_powm(
        power_tree[leaf_idx].get_mpz_t(),
        power_tree[parent].get_mpz_t(),
        product_tree[leaf_idx-1].get_mpz_t(), 
        p.get_mpz_t()
    );

    // Go left until reaching a leaf
    while (2*leaf_idx + 1 < TREE_SIZE) {
        // Take to the power of right child
        mpz_powm(
            power_tree[2*leaf_idx+1].get_mpz_t(),
            power_tree[leaf_idx].get_mpz_t(),
            product_tree[2*leaf_idx+2].get_mpz_t(), 
            p.get_mpz_t()
        );
        leaf_idx = 2*leaf_idx + 1;
    }
}


std::optional<mpz_class> find_least_primitive_root(
    const mpz_class& p,
    const mpz_class& n,
    const unsigned& d,
    const std::vector<unsigned>& prime_divisors
) {
    assert(p > 2 && n == p - 1);
    assert(mpz_odd_p(p.get_mpz_t()));
    assert(prime_divisors[0] == 2);
    assert(prime_divisors.size() == parameters::OMEGA);

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
    

    // TODO - New idea:
    // 1. Build product tree
    // 2. Apply Jacobi filter
    // 3. Compute A = g^{2d} (mod p)
    // 4. Compute power tree, lazily

    // 1. Full binary tree with n leaves contains 2n-1 nodes - ignore prime 2
    using parameters::TREE_SIZE;
    static thread_local std::array<mpz_class, TREE_SIZE> product_tree = {};
    static thread_local std::array<mpz_class, TREE_SIZE> power_tree = {};

    // Done recursively for simplicity, can be made iterative (full complete binary tree)
    _build_product_tree(product_tree, prime_divisors, 0, 1, parameters::OMEGA);

    //mpz_class residue;
    mpz_class exponent;
    for (unsigned g = 2;
 		 g <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT;
         ++g) {

        const mpz_class base = g;

        // 2. Jacobi filter
        // If p is prime, a primitive root must be a quadratic
        // nonresidue, so its Jacobi/Legendre symbol must be -1.
        if (mpz_jacobi(
                base.get_mpz_t(),
                p.get_mpz_t()
            ) != -1) {
            continue;
        }

        bool primitive_root_if_prime = true;
        
        // 3. Compute A = g^{2d} (mod p)
        //exponent = 2 * d; // TODO - This can be mpz_powm_ui
        mpz_powm_ui(
            power_tree[0].get_mpz_t(),
            base.get_mpz_t(),
            2*d,
            p.get_mpz_t()
        );

        size_t leaf_idx = 0; // Leaf in the power tree
        for (unsigned q : prime_divisors) {

            // For prime p, this condition is already supplied by
            // Jacobi(g,p) == -1 via Euler's criterion.
            if (q == 2) {
                continue;
            }

            // 4. Lazily compute power tree with early exit
            _generate_next_residue(power_tree, product_tree, p, leaf_idx); 

            if (power_tree[leaf_idx] == 1) {
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
	const unsigned& d,
    const std::vector<unsigned>& prime_divisors
) {
    assert(p >= 3 && n == p - 1);
    // Use 3 for Legendre's Filter

    * Order criterion v1:
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
     *

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
*/

