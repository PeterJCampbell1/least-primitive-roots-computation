/**
 * generate_supports.hpp: support enumeration (the producer).
 *
 * Enumerates, in lexicographic order, every increasing OMEGA-tuple of primes
 * containing 2 (a "support") whose product Q is at most the search bound and
 * for which Corollary 4 leaves some d in p = Q*d + 1 uncovered. Each one is
 * passed to a caller-supplied callable as LPRJob{support, Q, d_max}.
 *
 * A prefix's subtree is pruned using its minimal completion (the prefix
 * extended by the next consecutive primes). If that completion's product
 * exceeds the bound, or Corollary 4 covers every d for it, then by domination
 * the same holds for every completion of the prefix.
 *
 * Entry points (lpr::producer): generate_supports (single-threaded, processes
 * jobs inline), generate_supports_parallel (batches jobs into a
 * HysteresisQueue) and generate_supports_dry_run (only counts jobs).
 **/

#pragma once

#include "corollary4.hpp"
#include "hysteresis_queue.hpp"
#include "parameters.hpp"
#include "process_support.hpp"
#include "stats.hpp"

#include <array>
#include <vector>
#include <cassert>
#include <gmpxx.h>

namespace { // Anonymous namespace for internal use only
struct StackFrame {
    std::size_t next_prime_idx;
    mpz_class product;
    mpz_class delta_Q;
    mpz_class delta_sop;
    mpz_class cor4_bound; // Bound on Q*d for every leaf below the node (see dfs_iter_)
};

/**
 * Explicit DFS stack over prime indices.
 *
 * Frame k holds the index of the prime chosen at depth k (next_prime_idx)
 * and the values for the prefix of the k primes chosen before it.
 * Frame depth+1 receives the values for the prefix including the current
 * choice, and doubles as scratch space at the final depth.
 * Frame 0 corresponds to the prime 2, which every support must contain.
 **/
class DfsStack {
public:
    explicit DfsStack(const mpz_class& root_bound) {
        frames_[0].next_prime_idx = 0;
        frames_[0].product   = 1;
        frames_[0].delta_Q   = 1;
        frames_[0].delta_sop = 0;

        frames_[1].next_prime_idx = 1;
        frames_[1].product   = 2;
        frames_[1].delta_Q   = 1;
        frames_[1].delta_sop = 0;
        frames_[1].cor4_bound = root_bound; // Bound for the root node {2}
    }

    __attribute__((always_inline)) std::size_t depth() const { return depth_; }
    __attribute__((always_inline)) StackFrame& current() { return frames_[depth_]; }
    __attribute__((always_inline)) StackFrame& child() { return frames_[depth_ + 1]; }
    __attribute__((always_inline)) StackFrame& parent() { return frames_[depth_ - 1]; }

    // Descend one level, starting the choice at first_idx
    __attribute__((always_inline)) void push(std::size_t first_idx) {
        frames_[++depth_].next_prime_idx = first_idx;
    }

    // Move to the next sibling at the current depth
    __attribute__((always_inline)) void advance() {
        ++frames_[depth_].next_prime_idx;
    }

    // Pop the current depth and advance the parent.
    // Returns false once the traversal is complete.
    __attribute__((always_inline)) bool backtrack() {
        if (depth_ == 1) return false;
        --depth_;
        advance();
        return true;
    }

private:
    std::array<StackFrame, parameters::OMEGA + 1> frames_;
    std::size_t depth_ = 1; // Depth 0 (the prime 2) is fixed
};

/** 
 * Iterative implementation of DFS 
 *
 * Maintains that support contains an increasing list of primes.
 * Supports are iterated through in lexicographic order.
 *
 * Prunes branches based on Corollary 4 (TODO - CITE PAPER)
 * In particular, prunes via the minimum suffix extensions of each prefix
 * alongside the fact that if one support dominates another, and
 * satisfies corollary 4, then so does the other.
 * Since minimal completions only grow across siblings, any prune
 * also discards all later siblings (hence backtrack, not advance).
 *
 * Each node stores a bound on Q*d for every leaf below it: the root is
 * seeded with the global bound (Cor 4 for the first OMEGA primes), nodes
 * shallower than DELTA_START_DEPTH copy their parent's bound, and deeper
 * nodes store min(parent's bound, Cor 4 bound of their minimal completion).
 * The Cor 4 bound only decreases under domination, so a child whose minimal
 * completion already exceeds its parent's bound is pruned without
 * evaluating Corollary 4. A first child (the next consecutive prime) has
 * the same minimal completion as its parent, so it reuses the parent's
 * bound and skips all checks.
 *
 * Note: We know that support must always contain 2 (as Q*d + 1 must be odd).
 **/
template <typename Callable>
__attribute__((always_inline)) void dfs_iter_(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound,
    Callable process_job
) {
    using parameters::OMEGA;
    using parameters::S;

    static_assert(OMEGA > 2, "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);
    auto& stats = lpr::stats::get_thread_local_stats();

    // Initialise DFS data
    DfsStack stack(global_bound);
    std::vector<unsigned> support(OMEGA);
    support[0] = primes[0]; // primes[0] == 2
    
    // Number of primes to ignore
    constexpr std::size_t DELTA_START_DEPTH = OMEGA - S; 
    static_assert(DELTA_START_DEPTH > 0, "We cannot have OMEGA == S as 2 must be excluded");
    static_assert(DELTA_START_DEPTH < OMEGA, "Need S >= 1 so leaves always have a Cor 4 bound");

    mpz_class min_complete_product;
    mpz_class min_complete_delta_Q;
    mpz_class min_complete_delta_sop;

    // Reserve space to avoid reallocations later
    mpz_realloc2(min_complete_product.get_mpz_t(), 1024);
    
    mpz_class d_max_mpz;
    while (true) {
        const std::size_t depth = stack.depth();
        const std::size_t remaining_after_choice = OMEGA - depth - 1;
        const std::size_t i = stack.current().next_prime_idx; // The prime chosen at this depth

        if (i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
            if (!stack.backtrack()) break;
            continue;
        }
        support[depth] = primes[i];
        ++stats.dfs_nodes_visited;

        StackFrame& cur = stack.current();
        StackFrame& next = stack.child();
        if (depth >= DELTA_START_DEPTH) {
            mpz_mul_ui(
                next.delta_Q.get_mpz_t(),
                cur.delta_Q.get_mpz_t(),
                primes[i]
            ); // delta_Q_j = delta_Q_{j-1} * p_j

            mpz_set(
                next.delta_sop.get_mpz_t(),
                cur.delta_Q.get_mpz_t()
            );
            mpz_addmul_ui(
                next.delta_sop.get_mpz_t(),
                cur.delta_sop.get_mpz_t(),
                primes[i]
            ); // delta_SOP_j = delta_Q_{j-1} + p_j * delta_SOP_{j-1} 
        } else { // Ignore first few primes.
            next.delta_Q = 1;
            next.delta_sop = 0;
        }

        mpz_mul_ui(next.product.get_mpz_t(), cur.product.get_mpz_t(), primes[i]);

        // The parent node (depth - 1) stored its bound in cur.cor4_bound.
        // It evaluated Cor 4 itself only if its depth >= DELTA_START_DEPTH.
        const bool parent_evaluated_cor4 = depth > DELTA_START_DEPTH;
        if (parent_evaluated_cor4 && i == stack.parent().next_prime_idx + 1) {
            // Same minimal completion as the parent, which passed every check
            ++stats.cor4_reused;
            mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t());
        } else {
            min_complete_product = next.product;
            if (depth >= DELTA_START_DEPTH) {
                min_complete_delta_Q = next.delta_Q;
                min_complete_delta_sop = next.delta_sop;
            }
            for (std::size_t j = 1; j <= remaining_after_choice; ++j) {
                mpz_mul_ui(
                    min_complete_product.get_mpz_t(), 
                    min_complete_product.get_mpz_t(),
                    primes[i+j]
                ); // min_prod *= primes[i+j];
                   
                if (depth >= DELTA_START_DEPTH) {
                    // Calculate delta_sop first, as this uses the previous delta_Q
                    // Then after update delta_Q
                    min_complete_delta_sop *= primes[i+j];
                    min_complete_delta_sop += min_complete_delta_Q;
                    min_complete_delta_Q *= primes[i+j];
                }
            }
            if (min_complete_product > cur.cor4_bound) { // Parent's (or the global) bound
                ++stats.inherited_bound_prunes;
                if (!stack.backtrack()) break;
                continue;
            }
            if (depth >= DELTA_START_DEPTH) {
                ++stats.cor4_evaluations;
                corollary4_qd_bound(
                    min_complete_delta_Q, 
                    min_complete_delta_sop,
                    next.cor4_bound
                );
                if (min_complete_product > next.cor4_bound) { // No values of d escape Cor4 in p = Qd + 1
                    if (!stack.backtrack()) break;
                    continue;
                }
                if (cur.cor4_bound < next.cor4_bound) { // Keep bounds <= the global bound
                    mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t());
                }
            } else {
                mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t());
            }
        }

        if (depth == OMEGA - 1) {
            mpz_fdiv_q(
                d_max_mpz.get_mpz_t(),
                next.cor4_bound.get_mpz_t(),
                next.product.get_mpz_t()
            ); // d_max = floor(bound / Q)
            assert(d_max_mpz.fits_uint_p() && "d_max exceeds unsigned integer range");
            const unsigned d_max = d_max_mpz.get_ui();

            ++stats.supports_emitted;
            stats.d_max_sum += d_max;
            if (d_max > stats.d_max_max) {
                stats.d_max_max = d_max;
            }
            process_job({support, next.product, d_max}); // Works for both single/multi-threaded calls
            stack.advance(); // Move to sibling leaf node at same depth
        } else {
            stack.push(i + 1); // Move to next depth and use later primes 
        }
    } // while (true)
}

} // namespace

namespace lpr::producer {

inline void generate_supports(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    dfs_iter_(primes, global_bound, consumer::process_support);
}

/*
 * Runs the DFS without processing any supports, to measure producer speed.
 * Producer statistics (including supports emitted) are gathered by dfs_iter_.
 */
inline void generate_supports_dry_run(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    dfs_iter_(primes, global_bound, [](const parallel::LPRJob&) {});
}

/*
 * Wrapper around DFS to generate supports.
 *
 * Auto-handles batching jobs, and pushing batches to the concurrent queue.
 */
template <typename QueueType>
void generate_supports_parallel(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound,
    QueueType& queue
) {
    using parameters::BATCH_SIZE;
    using parallel::LPRBatch;
    using parallel::LPRJob;
    LPRBatch current_batch;
    current_batch.reserve(BATCH_SIZE);

    // Lambda passed into generate_supports to emit candidate supports
    auto emit_job = [&](LPRJob job) {
        current_batch.push_back(job);

        if (current_batch.size() == BATCH_SIZE) {
            queue.push(std::move(current_batch));
            current_batch = LPRBatch(); // Reset batch buffer
            current_batch.reserve(BATCH_SIZE);
        }
    };

    // Run DFS candidate generator
    dfs_iter_(primes, global_bound, emit_job);

    // Flush remaining candidates in final batch
    if (!current_batch.empty()) {
        queue.push(std::move(current_batch));
    }

    queue.set_finished();
}

} // lpr::producer

