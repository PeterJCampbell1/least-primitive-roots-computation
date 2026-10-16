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

#include "config.hpp"
#include "corollary4.hpp"
#include "hysteresis_queue.hpp"
#include "parameters.hpp"
#include "process_support.hpp"
#include "stats.hpp"

#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <cassert>
#include <gmpxx.h>

namespace lpr::producer::detail { // Internal; named (not anonymous) as this is a header
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
    std::array<StackFrame, parameters::MAX_OMEGA + 1> frames_; // Uses search_config().omega + 1
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
    const auto& config = lpr::config::search_config();
    const std::size_t OMEGA = config.omega;

    assert(OMEGA > 2 && OMEGA <= parameters::MAX_OMEGA && "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);
    auto& stats = lpr::stats::get_thread_local_stats();

    // Initialise DFS data
    DfsStack stack(global_bound);
    std::vector<unsigned> support(OMEGA);
    support[0] = primes[0]; // primes[0] == 2
    
    // Number of primes to ignore
    const std::size_t DELTA_START_DEPTH = config.delta_start_depth(); 
    assert(DELTA_START_DEPTH > 0 && "We cannot have OMEGA == S as 2 must be excluded");
    assert(DELTA_START_DEPTH < OMEGA && "Need S >= 1 so leaves always have a Cor 4 bound");

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

} // namespace lpr::producer::detail

namespace lpr::producer {

// The most recently emitted support, as a progress indicator for logging
// (relaxed atomics: the value is only informational, and may mix two supports).
inline std::array<std::atomic<unsigned>, parameters::MAX_OMEGA> dfs_position{};
inline std::array<std::atomic<unsigned>, parameters::MAX_OMEGA> dfs_first_primes{};

inline void set_dfs_first_primes(const std::vector<unsigned>& primes) {
    for (std::size_t i = 0; i < dfs_first_primes.size() && i < primes.size(); ++i) {
        dfs_first_primes[i].store(primes[i], std::memory_order_relaxed);
    }
}

inline void update_dfs_position(const std::vector<unsigned>& support) {
    for (std::size_t i = 0; i < dfs_position.size() && i < support.size(); ++i) {
        dfs_position[i].store(support[i], std::memory_order_relaxed);
    }
}

// E.g. "2..61,71,73,79,83,89,97,..." : the leading run of consecutive primes
// is abbreviated, then the next few primes of the support are shown
inline std::string dfs_position_string() {
    constexpr std::size_t SHOWN = 6;
    const std::size_t n = lpr::config::search_config().omega; // Entries in use
    std::size_t k = 0; // First index where the support leaves the consecutive primes
    while (k < n
           && dfs_position[k].load(std::memory_order_relaxed)
              == dfs_first_primes[k].load(std::memory_order_relaxed)) {
        ++k;
    }
    if (dfs_position[0].load(std::memory_order_relaxed) == 0) {
        return "none yet";
    }
    std::string out;
    if (k > 0) {
        out += "2.." + std::to_string(dfs_position[k - 1].load(std::memory_order_relaxed));
    }
    for (std::size_t i = k; i < n && i < k + SHOWN; ++i) {
        out += (out.empty() ? "" : ",") + std::to_string(dfs_position[i].load(std::memory_order_relaxed));
    }
    if (k + SHOWN < n) {
        out += ",...";
    }
    return out;
}

/*
 * Single-threaded: processes each support as soon as it is emitted.
 * Statistics are published every BATCH_SIZE supports for progress logging.
 */
inline void generate_supports(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    set_dfs_first_primes(primes);
    std::size_t since_publish = 0;
    detail::dfs_iter_(primes, global_bound, [&](const parallel::LPRJob& job) {
        consumer::process_support(job);
        if (++since_publish == parameters::BATCH_SIZE) {
            since_publish = 0;
            update_dfs_position(job.support);
            stats::publish_thread_local_stats();
        }
    });
    stats::publish_thread_local_stats();
}

/*
 * Runs the DFS without processing any supports, to measure producer speed.
 * Producer statistics (including supports emitted) are gathered by dfs_iter_.
 */
inline void generate_supports_dry_run(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    set_dfs_first_primes(primes);
    std::size_t since_publish = 0;
    detail::dfs_iter_(primes, global_bound, [&](const parallel::LPRJob& job) {
        if (++since_publish == parameters::BATCH_SIZE) {
            since_publish = 0;
            update_dfs_position(job.support);
            stats::publish_thread_local_stats();
        }
    });
    stats::publish_thread_local_stats();
}

/*
 * Producer for the multi-threaded mode.
 *
 * Batches jobs and offers each full batch to the queue. If the queue is full
 * the producer processes the batch itself (producer co-processing), so it never
 * sleeps. After the DFS it marks the queue finished and helps drain it.
 */
template <typename QueueType>
void generate_supports_parallel(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound,
    QueueType& queue
) {
    set_dfs_first_primes(primes);
    using parameters::BATCH_SIZE;
    using parallel::LPRBatch;
    using parallel::LPRJob;
    auto& stats = lpr::stats::get_thread_local_stats();
    LPRBatch current_batch;
    current_batch.reserve(BATCH_SIZE);

    auto submit_batch = [&]() {
        update_dfs_position(current_batch.back().support);
        if (queue.try_push(current_batch)) {
            ++stats.batches_pushed;
        } else { // Consumers are behind: process this batch here
            for (const LPRJob& job : current_batch) {
                consumer::process_support(job);
            }
            ++stats.batches_coprocessed;
        }
        current_batch = LPRBatch(); // Reset batch buffer
        current_batch.reserve(BATCH_SIZE);
        stats::publish_thread_local_stats();
    };

    // Lambda passed into dfs_iter_ to emit candidate supports
    auto emit_job = [&](LPRJob job) {
        current_batch.push_back(std::move(job));
        if (current_batch.size() == BATCH_SIZE) {
            submit_batch();
        }
    };

    // Run DFS candidate generator
    detail::dfs_iter_(primes, global_bound, emit_job);

    // Flush remaining candidates in final batch
    if (!current_batch.empty()) {
        submit_batch();
    }

    queue.set_finished();
    consumer::start_consumer(queue); // Help drain the queue
}

} // lpr::producer
