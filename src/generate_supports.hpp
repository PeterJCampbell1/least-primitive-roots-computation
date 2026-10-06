/**
 * FILE DESCRIPTION - TODO
 *
 *
 * THE MAIN PARALLELISATION PIPELINE CAN HAPPEN IN main.cpp
 **/

#include "hysteresis_queue.hpp"
#include "parameters.hpp"
#include "process_support.hpp" 

#include <vector>
#include <cassert>
#include <gmpxx.h>
#include <limits>

namespace { // Anonymous namespace for internal use only
struct StackFrame {
    std::size_t next_prime_idx;
    mpz_class product;
    mpz_class delta_Q;
    mpz_class delta_sop;
};

/** 
 * Iterative implementation of DFS 
 *
 * Maintains that support contains an increasing list of primes.
 * Supports are iterated through in lexicographic order.
 * The following stack frame is utilised as scratch space if needed.
 *
 * Prunes branches based on Corollary 4 (TODO - CITE PAPER)
 * In particular, prunes via the minimum suffix extensions of each prefix
 * alongside the fact that if one support dominates another, and
 * satisfies corollary 4, then so does the other.
 *
 * Note: We know that support must always contain 2 (as Q*d + 1 must be odd).
 **/
template <typename Callable>
__attribute__((always_inline)) void dfs_iter_(
    const std::vector<unsigned>& primes,
    const mpz_class& bound,
    Callable process_job
) {
    using parameters::OMEGA;
    using parameters::S;

    static_assert(OMEGA > 2, "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);

    // Initialise DFS data
    std::vector<StackFrame> stack(OMEGA+1); // +1 is scratch space for final stack frame
    std::vector<unsigned> support(OMEGA);
    stack[0].next_prime_idx = 0;
    stack[0].product   = 1;
    stack[0].delta_Q   = 1;
    stack[0].delta_sop = 0;

    // We know 2 \in support and the 0th frame corresponds to this
    std::size_t depth  = 1; 
    support[0] = primes[0]; // primes[0] == 2
    stack[1].next_prime_idx = 1; 
    stack[1].product = 2;
    stack[1].delta_Q = 1;
    stack[1].delta_sop = 0;
    
    // Number of primes to ignore
    constexpr std::size_t DELTA_START_DEPTH = OMEGA - S; 
    static_assert(DELTA_START_DEPTH > 0, "We cannot have OMEGA == S as 2 must be excluded");
    std::size_t remaining_after_choice, i;
    mpz_class min_complete_product;

    // Naive approach to local bounding
    mpz_class min_complete_delta_Q;
    mpz_class min_complete_delta_sop;

    // Reserve space to avoid reallocations later
    mpz_realloc2(min_complete_product.get_mpz_t(), 1024);
    
    unsigned d_max = 0; // DO THIS IN A BETTER WAY
    while (true) {
        remaining_after_choice = OMEGA - depth - 1;
        i = stack[depth].next_prime_idx; // The last prime chosen at this depth
        support[depth] = primes[i];
        
        if (i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        }

        if (depth >= DELTA_START_DEPTH) {
            mpz_mul_ui(
                stack[depth+1].delta_Q.get_mpz_t(),
                stack[depth].delta_Q.get_mpz_t(),
                primes[i]
            ); // delta_Q_j = delta_Q_{j-1} * p_j

            mpz_set(
                stack[depth+1].delta_sop.get_mpz_t(),
                stack[depth].delta_Q.get_mpz_t()
            );
            mpz_addmul_ui(
                stack[depth+1].delta_sop.get_mpz_t(),
                stack[depth].delta_sop.get_mpz_t(),
                primes[i]
            ); // delta_SOP_j = delta_Q_{j-1} + p_j * delta_SOP_{j-1} 
        } else { // Ignore first few primes.
            stack[depth+1].delta_Q = 1;
            stack[depth+1].delta_sop = 0;
        }

        stack[depth+1].product = stack[depth].product * primes[i];
        min_complete_product = stack[depth+1].product;
        if (depth >= DELTA_START_DEPTH) {
            min_complete_delta_Q = stack[depth+1].delta_Q;
            min_complete_delta_sop = stack[depth+1].delta_sop;
        }
        for (std::size_t j = 1; j <= remaining_after_choice; ++j) {
            mpz_mul_ui(
                min_complete_product.get_mpz_t(), 
                min_complete_product.get_mpz_t(),
                primes[i+j]
            ); // min_prod *= primes[i+j];
               
            // Naive local bounding approach
            // TODO: figure out an optimal way to implement this
            if (depth >= DELTA_START_DEPTH) {
                // Calculate delta_sop first, as this uses the previous delta_Q
                // Then after update delta_Q
                min_complete_delta_sop *= primes[i+j];
                min_complete_delta_sop += min_complete_delta_Q;
                min_complete_delta_Q *= primes[i+j];
            }
        }
        if (min_complete_product > bound) {
            if (depth == 1) break; // Traversal complete
            --depth;
            ++stack[depth].next_prime_idx; // Move to next subset through backtracking
            continue;
        } // TODO - With local bounding this now is somewhat obsolete (might still be useful to keep)
        if (depth >= DELTA_START_DEPTH) {
            d_max = create_corollary4_threshold(min_complete_product, min_complete_delta_Q, min_complete_delta_sop);
            //if (corollary4_proves_grosswald(0)) { 
            if (d_max == 0) { // No values of d escape Cor4 in p = Qd + 1
                // Local Delta means all suffix's from this prefix must satisfy corollary 4, 
                // As any larger suffix can only widen the inequality (lower lhs, raise rhs)
                // Thus, we can prune this subtree
                if (depth == 1) break; // Traversal complete
                --depth;
                ++stack[depth].next_prime_idx; // Move to next subset through backtracking
                continue;
            }
        }

        if (depth == OMEGA - 1) {
            // depth+1 is now scratch space
            process_job(
                {
                support,
                stack[depth+1].product,
                d_max
                }
            ); // Works for both single/multi-threaded calls
            ++stack[depth].next_prime_idx; // Move to sibling leaf node at same depth
        } else {
            stack[++depth].next_prime_idx = i + 1; // Move to next depth and use later primes 
        }
    } // while (true)
}

} // namespace

namespace lpr::producer {

void generate_supports(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    dfs_iter_(primes, global_bound, consumer::process_support);
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

