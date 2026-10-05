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
    size_t parent = leaf_idx / 2;
    while (true) { 
        parent = leaf_idx / 2;
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
    _build_product_tree(product_tree, prime_divisors, 0, 0, parameters::OMEGA - 1);

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
        exponent = 2 * d;
        mpz_powm(
            power_tree[0].get_mpz_t(),
            base.get_mpz_t(),
            exponent.get_mpz_t(), 
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
            //residue = power_tree[leaf_idx];

            /*
            exponent = n / q;

            mpz_powm(
                residue.get_mpz_t(),
                base.get_mpz_t(),
                exponent.get_mpz_t(),
                p.get_mpz_t()
            );*/

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

