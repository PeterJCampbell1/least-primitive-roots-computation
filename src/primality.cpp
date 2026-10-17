#include "primality.hpp"
#include "config.hpp"
#include "mont3.hpp"
#include "parameters.hpp"
#include "stats.hpp"
#include "trial_division.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

namespace lpr::primality {

namespace {

/* ---------- perfect powers ---------- */

// If g = a^m (m >= 2) is a primitive root then so is a < g, since ord(a^m) divides
// ord(a). So a perfect power is never the least primitive root and is skipped.
constexpr std::array<bool, parameters::PRIMITIVE_ROOT_SEARCH_LIMIT + 1> make_perfect_power_table() {
    std::array<bool, parameters::PRIMITIVE_ROOT_SEARCH_LIMIT + 1> table{};
    for (std::uint64_t a = 2; a * a <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT; ++a) {
        for (std::uint64_t v = a * a; v <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT; v *= a) {
            table[static_cast<std::size_t>(v)] = true;
        }
    }
    return table;
}

constexpr auto IS_PERFECT_POWER = make_perfect_power_table();

/* ---------- product tree ---------- */

using ProductTree = std::array<mpz_class, parameters::MAX_TREE_SIZE>;

/*
 * Builds the product tree over the odd primes prime_divisors[left, right):
 * leaves hold single primes, internal nodes the product of their children.
 * Nodes are stored heap-style (children of i at 2i+1 and 2i+2) and split with
 * complete_tree_left_size, so the tree is complete and its leaves are visited
 * in increasing order of their primes by advance_to_next_leaf.
 *
 * Example: prime divisors [2, 3, 5, 7, 11, 13] (2 is excluded):
 *
 *            (3,5,7,11,13)
 *        (3,5,7)       (11,13)
 *     (3,5)    7      11     13
 *    3     5
 *
 * stored as [(3,5,7,11,13), (3,5,7), (11,13), (3,5), 7, 11, 13, 3, 5].
 */
void build_product_tree(
    ProductTree& product_tree,
    const unsigned* prime_divisors,
    std::size_t node,
    std::size_t left,
    std::size_t right // Exclusive
) {
    if (right - left == 1) { // Leaf: a single prime
        product_tree[node] = prime_divisors[left];
        return;
    }
    const std::size_t left_child = 2*node + 1;
    const std::size_t right_child = 2*node + 2;
    const std::size_t mid = left + complete_tree_left_size(right - left);
    build_product_tree(product_tree, prime_divisors, left_child, left, mid);
    build_product_tree(product_tree, prime_divisors, right_child, mid, right);
    mpz_mul(product_tree[node].get_mpz_t(), product_tree[left_child].get_mpz_t(),
            product_tree[right_child].get_mpz_t());
}

// Mirrors build_product_tree on indices only (see power_tree_layout_is_valid)
bool check_tree_layout(std::size_t node, std::size_t left, std::size_t right,
                       std::size_t tree_size, std::size_t& next_leaf) {
    if (node >= tree_size) {
        return false; // Index outside the tree
    }
    const bool traversal_sees_leaf = (2*node + 1 >= tree_size);
    if (right - left == 1) {
        // A real leaf: the traversal must see a leaf, and leaves come in increasing order
        return traversal_sees_leaf && left == next_leaf++;
    }
    if (traversal_sees_leaf) {
        return false; // Internal node mistaken for a leaf
    }
    const std::size_t mid = left + complete_tree_left_size(right - left);
    return check_tree_layout(2*node + 1, left, mid, tree_size, next_leaf)
        && check_tree_layout(2*node + 2, mid, right, tree_size, next_leaf);
}

/* ---------- power tree ---------- */

/*
 * Advances the lazy power tree to its next leaf (in-order), computing nodes on
 * the way down; leaf_idx == 0 means "start at the root". Each child is its
 * parent raised to the product of its sibling's primes, so every node holds
 * root^(Q_odd / prod(node)) and the leaf for prime q holds root^(Q_odd / q).
 * pow(dst, base, exponent) sets dst = base^exponent mod p, for either value
 * representation.
 */
template <typename Value, typename Pow>
void advance_to_next_leaf(
    std::array<Value, parameters::MAX_TREE_SIZE>& power_tree,
    const ProductTree& product_tree,
    std::size_t& leaf_idx,
    std::uint64_t& powm_count,
    Pow pow
) {
    const std::size_t tree_size = lpr::config::search_config().tree_size();

    if (leaf_idx != 0) {
        // Go up while we are a right child (odd indices are left children);
        // the rightmost leaf is never advanced past
        while (leaf_idx % 2 == 0) {
            leaf_idx = (leaf_idx - 1) / 2;
        }
        // Step to the right sibling, which is raised to the left sibling's product
        const std::size_t parent = (leaf_idx - 1) / 2;
        ++leaf_idx;
        ++powm_count;
        pow(power_tree[leaf_idx], power_tree[parent], product_tree[leaf_idx - 1]);
    }

    // Go left until reaching a leaf; a left child is raised to its right sibling's product
    while (2*leaf_idx + 1 < tree_size) {
        ++powm_count;
        pow(power_tree[2*leaf_idx + 1], power_tree[leaf_idx], product_tree[2*leaf_idx + 2]);
        leaf_idx = 2*leaf_idx + 1;
    }
}

/*
 * Backends for the least primitive root search: GMP residues, or Montgomery
 * residues with a lpr::mont3::Context. Each provides
 *   set_root(base, d)       root = g^(2d) mod p
 *   advance(products, leaf, powm_count)
 *   leaf_is_one(leaf)
 *   leaf_value(leaf, out)   the leaf as an ordinary residue (debug cross-checks)
 */
class GmpBackend {
public:
    explicit GmpBackend(const mpz_class& p) : p_(p) {}

    void set_root(const mpz_class& base, unsigned d) {
        mpz_powm_ui(tree()[0].get_mpz_t(), base.get_mpz_t(), 2ul * d, p_.get_mpz_t());
    }

    void advance(const ProductTree& products, std::size_t& leaf, std::uint64_t& powm_count) {
        advance_to_next_leaf(tree(), products, leaf, powm_count,
            [this](mpz_class& dst, const mpz_class& base, const mpz_class& exponent) {
                mpz_powm(dst.get_mpz_t(), base.get_mpz_t(), exponent.get_mpz_t(), p_.get_mpz_t());
            });
    }

    bool leaf_is_one(std::size_t leaf) const { return tree()[leaf] == 1; }

    void leaf_value(std::size_t leaf, mpz_class& out) const { out = tree()[leaf]; }

private:
    const mpz_class& p_;

    // Thread-local node storage, reused across calls to avoid GMP allocations
    static std::array<mpz_class, parameters::MAX_TREE_SIZE>& tree() {
        static thread_local std::array<mpz_class, parameters::MAX_TREE_SIZE> nodes;
        return nodes;
    }
};

class MontBackend {
public:
    explicit MontBackend(const lpr::mont3::Context& context) : context_(context) {}

    void set_root(const mpz_class& base, unsigned d) {
        const mp_limb_t two_d = 2 * static_cast<mp_limb_t>(d);
        tree()[0] = lpr::mont3::powm(lpr::mont3::to_mont(mpz_get_ui(base.get_mpz_t()), context_),
                                     &two_d, 1, context_);
    }

    void advance(const ProductTree& products, std::size_t& leaf, std::uint64_t& powm_count) {
        advance_to_next_leaf(tree(), products, leaf, powm_count,
            [this](lpr::mont3::Num& dst, const lpr::mont3::Num& base, const mpz_class& exponent) {
                dst = lpr::mont3::powm(base, mpz_limbs_read(exponent.get_mpz_t()),
                                       mpz_size(exponent.get_mpz_t()), context_);
            });
    }

    // The leaf is 1 iff its canonical Montgomery form equals that of 1
    bool leaf_is_one(std::size_t leaf) const {
        return lpr::mont3::equal(lpr::mont3::canonical(tree()[leaf], context_), context_.one);
    }

    void leaf_value(std::size_t leaf, mpz_class& out) const {
        lpr::mont3::to_mpz(out.get_mpz_t(), lpr::mont3::from_mont(tree()[leaf], context_));
    }

private:
    const lpr::mont3::Context& context_;

    static std::array<lpr::mont3::Num, parameters::MAX_TREE_SIZE>& tree() {
        static thread_local std::array<lpr::mont3::Num, parameters::MAX_TREE_SIZE> nodes;
        return nodes;
    }
};

/*
 * Least primitive root search with the given backend (see find_least_primitive_root).
 *
 * Conditional order criterion: suppose p is prime and n = p - 1. A primitive
 * root is a quadratic non-residue, and for prime p the Jacobi symbol is the
 * Legendre symbol, so Jacobi(g, p) == -1 gives g^(n/2) == -1 by Euler's
 * criterion: the q = 2 condition costs no exponentiation. For every odd prime
 * q | n we require g^(n/q) != 1, read off the power tree's leaves. Then
 * ord_p(g) = n, so g is a primitive root, and since g is tested in increasing
 * order the first success is g(p). For composite p these tests mean nothing,
 * which is irrelevant: Grosswald's conjecture concerns prime p only.
 */
template <typename Backend>
std::optional<unsigned> search(
    const mpz_class& p,
    unsigned d,
    [[maybe_unused]] const unsigned* prime_divisors, // Used by the debug cross-check only
    std::size_t omega,
    const ProductTree& product_tree,
    Backend& backend
) {
    auto& stats = lpr::stats::get_thread_local_stats();
    static thread_local mpz_class base;
    const std::size_t odd_primes = omega - 1;
#ifndef NDEBUG
    const mpz_class n = p - 1;
    mpz_class expect, leaf_value;
#endif

    for (unsigned g = 2; g <= parameters::PRIMITIVE_ROOT_SEARCH_LIMIT; ++g) {
        if (IS_PERFECT_POWER[g]) {
            continue;
        }

        mpz_set_ui(base.get_mpz_t(), g);
        ++stats.lpr_jacobi_calls;
        if (mpz_jacobi(base.get_mpz_t(), p.get_mpz_t()) != -1) {
            continue; // A quadratic residue is never a primitive root
        }
        ++stats.lpr_legendre_survivors;

        ++stats.lpr_root_powm_count;
        backend.set_root(base, d);

        bool primitive_root_if_prime = true;
        std::size_t leaf = 0;
        std::uint64_t tree_powm = 0;
        for (std::size_t i = 1; i <= odd_primes; ++i) {
            backend.advance(product_tree, leaf, tree_powm);
#ifndef NDEBUG
            // Leaves come in increasing order, and the leaf for prime q must equal
            // g^(n/q) mod p, computed directly with GMP (independent of the tree)
            const unsigned q = prime_divisors[i];
            assert(product_tree[leaf] == q && "power tree leaves out of order");
            mpz_powm(expect.get_mpz_t(), base.get_mpz_t(), mpz_class(n / q).get_mpz_t(), p.get_mpz_t());
            backend.leaf_value(leaf, leaf_value);
            assert(leaf_value == expect && "power tree leaf disagrees with GMP");
#endif
            if (backend.leaf_is_one(leaf)) {
                primitive_root_if_prime = false;
                break;
            }
        }
        stats.lpr_tree_powm_count += tree_powm;

        if (primitive_root_if_prime) {
            const unsigned p_mod_8 = static_cast<unsigned>(mpz_fdiv_ui(p.get_mpz_t(), 8));
            ++stats.winners_by_class[2*p_mod_8 + (d & 1)];
            return g;
        }
    }
    return std::nullopt;
}

#ifndef NDEBUG
bool context_matches(const lpr::mont3::Context& context, const mpz_class& p) {
    return lpr::mont3::equal(context.p, lpr::mont3::from_mpz(p.get_mpz_t()));
}
#endif

} // namespace

/* ---------- public interface ---------- */

/**
 * Trial division by the primes of the trial division table (trial_division.hpp).
 *
 * For each chunk (consecutive primes whose product fits in 64 bits) we compute
 * rem = p mod product with one multi-limb by single-limb division. Each prime r of
 * the chunk divides p iff it divides rem, which the Granlund-Montgomery test
 * decides with one multiplication and one comparison (no division). Chunks are
 * tested smallest primes first and we return at the first divisor found.
 **/
bool is_composite_small_prime(const mpz_class& p) {
    assert(p >= 3);
    static_assert(sizeof(unsigned long) == sizeof(std::uint64_t),
                  "mpz_tdiv_ui must take a 64-bit divisor");

    auto& stats = lpr::stats::get_thread_local_stats();
    const auto& table = lpr::trial_division::table();

    for (std::size_t k = 0; k < table.chunks.size(); ++k) {
        const auto& chunk = table.chunks[k];
        // p mod (product of the chunk's primes)
        const std::uint64_t rem = mpz_tdiv_ui(p.get_mpz_t(), chunk.product);
        for (std::size_t j = chunk.first; j < chunk.first + chunk.count; ++j) {
            const auto& t = table.tests[j];
            // Granlund-Montgomery: r | rem  <=>  rem * r^{-1} (mod 2^64) <= floor((2^64 - 1) / r)
            const bool divisible = rem * t.inverse <= t.limit;
            assert(divisible == (rem % t.prime == 0)); // Debug builds: exact cross-check
            if (divisible) {
                ++stats.composite_caught_at[k];
                return true; // t.prime divides p and p > t.prime, so p is composite
            }
        }
    }
    return false;
}

/**
 * If p > 2 is prime then gcd(2, p) = 1, so 2^n == 1 (mod p) with n = p - 1 by
 * Fermat's little theorem. 2^n != 1 therefore proves p composite.
 **/
bool is_composite_base2_fermat(
    const mpz_class& p,
    const mpz_class& n,
    const lpr::mont3::Context* mont
) {
    assert(p >= 3 && n == p - 1);
    static const mpz_class two = 2;
    static thread_local mpz_class residue;

    if (mont) {
        assert(context_matches(*mont, p) && "Montgomery context is for a different p");
        // 2^n in Montgomery form, compared with the Montgomery form of 1
        const bool composite = lpr::mont3::fermat_composite(n.get_mpz_t(), *mont);
#ifndef NDEBUG
        // Debug builds: exact cross-check against GMP
        mpz_powm(residue.get_mpz_t(), two.get_mpz_t(), n.get_mpz_t(), p.get_mpz_t());
        assert(composite == (residue != 1) && "mont3 Fermat test disagrees with GMP");
#endif
        return composite;
    }

    mpz_powm(residue.get_mpz_t(), two.get_mpz_t(), n.get_mpz_t(), p.get_mpz_t());
    return residue != 1;
}

std::optional<unsigned> find_least_primitive_root(
    const mpz_class& p,
    unsigned d,
    const unsigned* prime_divisors,
    std::size_t omega,
    const lpr::mont3::Context* mont
) {
    assert(p > 2 && mpz_odd_p(p.get_mpz_t()));
    assert(omega == lpr::config::search_config().omega && prime_divisors[0] == 2);

    // Product tree over the odd prime divisors (prime_divisors[0] == 2 is excluded)
    static thread_local ProductTree product_tree;
    build_product_tree(product_tree, prime_divisors, 0, 1, omega);

    if (mont) {
        assert(context_matches(*mont, p) && "Montgomery context is for a different p");
        MontBackend backend(*mont);
        return search(p, d, prime_divisors, omega, product_tree, backend);
    }
    GmpBackend backend(p);
    return search(p, d, prime_divisors, omega, product_tree, backend);
}

/*
 * Number of leaves in the left subtree of a complete binary tree with n >= 2 leaves.
 *
 * The power tree is stored heap-style (children of node i at 2i+1 and 2i+2) and
 * advance_to_next_leaf identifies leaves by "2i+1 >= TREE_SIZE". Both are valid
 * only for a COMPLETE binary tree: every level full except the last, which is
 * filled from the left, so that the n leaves occupy exactly the last n slots.
 * Splitting each range in half (balanced) is not enough: e.g. n = 6 would split
 * 3/3 and leave a gap in the bottom level, whereas the complete tree splits 4/2.
 *
 * With h = ceil(log2 n), the bottom level holds b = 2(n - 2^(h-1)) leaves, filled
 * from the left. If b >= 2^(h-1) (half the bottom level), the left subtree is
 * full: 2^(h-1) leaves. Otherwise all b bottom leaves are in the left subtree:
 * of its 2^(h-2) nodes on level h-1, b/2 are parents of two bottom leaves and
 * the rest are leaves, giving 2^(h-2) + b/2 = n - 2^(h-2) leaves.
 * Hence left = min(n - 2^(h-2), 2^(h-1)).
 * For n a power of two this is n/2; for 31 leaves (omega = 32) it is 16.
 */
std::size_t complete_tree_left_size(std::size_t n) {
    assert(n >= 2);
    if (n == 2) {
        return 1;
    }
    const unsigned h = 64 - static_cast<unsigned>(__builtin_clzll(n - 1)); // ceil(log2 n)
    return std::min<std::size_t>(n - (std::size_t{1} << (h - 2)), std::size_t{1} << (h - 1));
}

bool power_tree_layout_is_valid(std::size_t leaves) {
    if (leaves < 2) {
        return false;
    }
    const std::size_t tree_size = 2*leaves - 1;
    if (tree_size > parameters::MAX_TREE_SIZE) {
        return false;
    }
    std::size_t next_leaf = 0;
    return check_tree_layout(0, 0, leaves, tree_size, next_leaf) && next_leaf == leaves;
}

} // namespace lpr::primality
