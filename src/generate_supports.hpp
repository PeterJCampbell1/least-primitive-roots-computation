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
 * The DFS runs one region of the tree at a time (detail::run_region), so a
 * region can be split off and handed to another worker (detail::split_region;
 * see work_stealing.hpp and docs/work_stealing.md).
 *
 * Entry points (lpr::producer): generate_supports (single-threaded, processes
 * jobs inline), generate_supports_dry_run (only counts jobs),
 * generate_supports_split_test (single-threaded splitting test) and
 * generate_supports_parallel (the queue mode: batches jobs into a JobQueue).
 **/

#pragma once

#include "bit_tricks.hpp"
#include "config.hpp"
#include "corollary4.hpp"
#include "job_queue.hpp"
#include "parameters.hpp"
#include "process_support.hpp"
#include "stats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>
#include <cassert>
#include <gmpxx.h>

namespace lpr::producer::detail { // Internal; named (not anonymous) as this is a header
struct StackFrame {
    std::size_t next_prime_idx = 0;
    mpz_class product;
    mpz_class delta_Q;
    mpz_class delta_sop;
    mpz_class cor4_bound; // Bound on Q*d for every leaf below the node (see run_region)
    std::uint64_t prefix_hash = 0; // lpr::bits::support_hash of the prefix, built one prime at a time
};

/**
 * A region of the search tree, owned by one worker (see docs/work_stealing.md):
 * the fixed prefix above base_depth, and the siblings at base_depth from
 * frames[base_depth].next_prime_idx up to (excluding) prime index base_limit.
 * Depths below base_depth are unrestricted. Its supports form one contiguous
 * block in lexicographic order.
 **/
struct Region {
    std::size_t base_depth = 1;
    std::size_t base_limit = 0;
    std::vector<StackFrame> frames;       // frames[0..base_depth], as in DfsStack
    std::vector<unsigned> support_prefix; // support[0..base_depth): the primes chosen above base_depth
};

/**
 * A resumable DFS position, as saved in checkpoints (docs/checkpointing.md):
 * the region (base_depth, base_limit) plus the stack's index path at the top of
 * a run_region iteration. path[d] for d < depth is the prime index chosen at
 * depth d (path[0] = 0, the prime 2), and path[depth] is the next index to try
 * at the current depth. Everything before this position in the region has been
 * searched and processed; the rest of the region has not. The frames' values
 * are rebuilt by DfsStack::restore, which replays the path.
 **/
struct Task {
    std::size_t base_depth = 1;
    std::size_t base_limit = 0;
    std::size_t depth = 1;
    std::vector<std::size_t> path; // depth + 1 entries

    bool operator==(const Task& other) const {
        return base_depth == other.base_depth && base_limit == other.base_limit
               && depth == other.depth && path == other.path;
    }
};

// The whole search (the state of a new DfsStack)
inline Task root_task(std::size_t num_primes) {
    return Task{1, num_primes, 1, {0, 1}};
}

// A Region handed over by split_region, as a Task (it starts at its base depth)
inline Task region_task(const Region& region) {
    Task task{region.base_depth, region.base_limit, region.base_depth, {}};
    for (std::size_t d = 0; d <= region.base_depth; ++d) {
        task.path.push_back(region.frames[d].next_prime_idx);
    }
    return task;
}

/*
 * True if no two tasks cover a common support (else false, with a reason).
 * In lexicographic order of index tuples, a task covers the interval
 * [lo, hi): lo = its path (tuples whose first depth+1 indices are >= the path)
 * and hi = (path[0..base_depth), base_limit) (tuples whose first base_depth+1
 * indices are below it). A shorter bound compares on fewer indices. Sorted by
 * lo, the intervals are disjoint iff each ends at or before the next begins.
 * Finished tasks (empty intervals) are ignored. Catches a region listed twice.
 */
inline bool tasks_disjoint(const std::vector<Task>& tasks, std::string* error) {
    struct Interval {
        std::vector<std::size_t> lo, hi;
        std::size_t task;
    };
    std::vector<Interval> intervals;
    for (std::size_t k = 0; k < tasks.size(); ++k) {
        const Task& t = tasks[k];
        if (t.depth == t.base_depth && t.path[t.base_depth] >= t.base_limit) {
            continue; // Finished: covers nothing
        }
        Interval iv{t.path, std::vector<std::size_t>(t.path.begin(), t.path.begin() + static_cast<std::ptrdiff_t>(t.base_depth)), k};
        iv.hi.push_back(t.base_limit);
        intervals.push_back(std::move(iv));
    }
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
    for (std::size_t i = 0; i + 1 < intervals.size(); ++i) {
        const Interval& a = intervals[i];
        const Interval& b = intervals[i + 1];
        // b.lo >= a.hi on their common indices, and if equal there b.lo must be at
        // least as long (a shorter lower bound would include tuples below a.hi)
        const std::size_t m = std::min(a.hi.size(), b.lo.size());
        const bool equal = std::equal(b.lo.begin(), b.lo.begin() + static_cast<std::ptrdiff_t>(m), a.hi.begin());
        const bool after = std::lexicographical_compare(a.hi.begin(), a.hi.begin() + static_cast<std::ptrdiff_t>(m),
                                                        b.lo.begin(), b.lo.begin() + static_cast<std::ptrdiff_t>(m));
        if (!(after || (equal && b.lo.size() >= a.hi.size()))) {
            if (error) {
                *error = "tasks " + std::to_string(a.task + 1) + " and " + std::to_string(b.task + 1) + " overlap";
            }
            return false;
        }
    }
    return true;
}

// Equal values (the index is compared separately, through the Task path)
inline bool same_values(const StackFrame& a, const StackFrame& b) {
    return a.product == b.product && a.delta_Q == b.delta_Q && a.delta_sop == b.delta_sop
           && a.cor4_bound == b.cor4_bound && a.prefix_hash == b.prefix_hash;
}

/**
 * Explicit DFS stack over prime indices.
 *
 * Frame k holds the index of the prime chosen at depth k (next_prime_idx)
 * and the values for the prefix of the k primes chosen before it.
 * Frame depth+1 receives the values for the prefix including the current
 * choice, and doubles as scratch space at the final depth.
 * Frame 0 corresponds to the prime 2, which every support must contain.
 *
 * The stack runs one region: siblings at base_depth_ stop before base_limit_,
 * and backtracking past base_depth_ ends the region. The root region
 * (base_depth_ = 1, base_limit_ = number of primes) is the whole search.
 **/
class DfsStack {
public:
    // The whole search: the root node {2} with the global bound
    DfsStack(const mpz_class& root_bound, std::size_t num_primes) {
        reset_root(root_bound, num_primes);
    }

    // The root state of the constructor
    void reset_root(const mpz_class& root_bound, std::size_t num_primes) {
        frames_[0].next_prime_idx = 0;
        frames_[0].product   = 1;
        frames_[0].delta_Q   = 1;
        frames_[0].delta_sop = 0;
        frames_[0].prefix_hash = 0; // Empty prefix

        frames_[1].next_prime_idx = 1;
        frames_[1].product   = 2;
        frames_[1].delta_Q   = 1;
        frames_[1].delta_sop = 0;
        frames_[1].cor4_bound = root_bound; // Bound for the root node {2}
        frames_[1].prefix_hash = lpr::bits::mix64(frames_[0].prefix_hash ^ 2); // Prefix (2)

        depth_ = 1;
        base_depth_ = 1;
        base_limit_ = num_primes;
    }

    // Become `region`: copy its frames and prefix, and start at its first sibling
    void load(const Region& region, std::vector<unsigned>& support) {
        assert(region.base_depth >= 1 && region.base_depth < frames_.size());
        assert(region.frames.size() == region.base_depth + 1);
        assert(region.support_prefix.size() == region.base_depth);
        for (std::size_t d = 0; d <= region.base_depth; ++d) {
            frames_[d] = region.frames[d];
        }
        for (std::size_t d = 0; d < region.base_depth; ++d) {
            support[d] = region.support_prefix[d];
        }
        depth_ = region.base_depth;
        base_depth_ = region.base_depth;
        base_limit_ = region.base_limit;
    }

    // The current position as a Task. Valid at the top of a run_region iteration.
    Task task() const {
        Task t{base_depth_, base_limit_, depth_, {}};
        for (std::size_t d = 0; d <= depth_; ++d) {
            t.path.push_back(frames_[d].next_prime_idx);
        }
        return t;
    }

    /*
     * Becomes `task`: rebuilds frames 0..task.depth by replaying its path from
     * the root with the same arithmetic as run_region, and sets support[0..depth).
     * Returns false (with a reason in *error) if the task is malformed or is not
     * a position run_region can reach: indices out of range or not increasing,
     * outside its region, or a prefix that run_region would have pruned.
     * Callers that also hold the live frames compare them (same_values) to
     * cross-check the replay.
     */
    bool restore(const Task& task, const std::vector<unsigned>& primes, const mpz_class& root_bound,
                 std::vector<unsigned>& support, std::string* error) {
        const auto& config = lpr::config::search_config();
        const std::size_t OMEGA = config.omega;
        const std::size_t DELTA_START_DEPTH = config.delta_start_depth();
        const std::size_t n = primes.size();
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        if (task.depth < 1 || task.depth >= OMEGA) return fail("depth out of range");
        if (task.base_depth < 1 || task.base_depth > task.depth) return fail("base depth out of range");
        if (task.path.size() != task.depth + 1) return fail("path length does not match depth");
        if (task.base_limit > n) return fail("base limit beyond the prime list");
        if (task.path[0] != 0) return fail("path does not start with the prime 2");
        if (support.size() != OMEGA) return fail("support array has the wrong size");

        static thread_local mpz_class min_product, min_delta_Q, min_delta_sop;
        reset_root(root_bound, n);
        support[0] = primes[0];
        for (std::size_t d = 1; d < task.depth; ++d) { // Replay the choice at depth d
            const std::size_t i = task.path[d];
            const std::size_t rem = OMEGA - d - 1;
            if (i <= task.path[d - 1]) return fail("path indices are not increasing");
            if (i >= n || n - (i + 1) < rem) return fail("path index cannot be completed");
            if (d == task.base_depth && i >= task.base_limit) return fail("path leaves its region");
            frames_[d].next_prime_idx = i;
            const StackFrame& cur = frames_[d];
            StackFrame& next = frames_[d + 1];
            if (d >= DELTA_START_DEPTH) {
                mpz_mul_ui(next.delta_Q.get_mpz_t(), cur.delta_Q.get_mpz_t(), primes[i]);
                mpz_set(next.delta_sop.get_mpz_t(), cur.delta_Q.get_mpz_t());
                mpz_addmul_ui(next.delta_sop.get_mpz_t(), cur.delta_sop.get_mpz_t(), primes[i]);
            } else {
                next.delta_Q = 1;
                next.delta_sop = 0;
            }
            mpz_mul_ui(next.product.get_mpz_t(), cur.product.get_mpz_t(), primes[i]);
            const bool parent_evaluated_cor4 = d > DELTA_START_DEPTH;
            if (parent_evaluated_cor4 && i == task.path[d - 1] + 1) {
                mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t()); // First child
            } else {
                min_product = next.product;
                if (d >= DELTA_START_DEPTH) {
                    min_delta_Q = next.delta_Q;
                    min_delta_sop = next.delta_sop;
                }
                for (std::size_t j = 1; j <= rem; ++j) {
                    mpz_mul_ui(min_product.get_mpz_t(), min_product.get_mpz_t(), primes[i + j]);
                    if (d >= DELTA_START_DEPTH) {
                        min_delta_sop *= primes[i + j];
                        min_delta_sop += min_delta_Q;
                        min_delta_Q *= primes[i + j];
                    }
                }
                if (min_product > cur.cor4_bound) return fail("path prefix is pruned by its inherited bound");
                if (d >= DELTA_START_DEPTH) {
                    corollary4_qd_bound(min_delta_Q, min_delta_sop, next.cor4_bound);
                    if (min_product > next.cor4_bound) return fail("path prefix is pruned by Corollary 4");
                    if (cur.cor4_bound < next.cor4_bound) {
                        mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t());
                    }
                } else {
                    mpz_set(next.cor4_bound.get_mpz_t(), cur.cor4_bound.get_mpz_t());
                }
            }
            next.prefix_hash = lpr::bits::mix64(cur.prefix_hash ^ primes[i]);
            support[d] = primes[i];
        }
        const std::size_t last = task.path[task.depth]; // Next index to try at the current depth
        if (last <= task.path[task.depth - 1]) return fail("path indices are not increasing");
        if (last > n) return fail("path index beyond the prime list");
        frames_[task.depth].next_prime_idx = last;
        depth_ = task.depth;
        base_depth_ = task.base_depth;
        base_limit_ = task.base_limit;
        return true;
    }

    __attribute__((always_inline)) std::size_t depth() const { return depth_; }
    __attribute__((always_inline)) StackFrame& current() { return frames_[depth_]; }
    __attribute__((always_inline)) StackFrame& child() { return frames_[depth_ + 1]; }
    __attribute__((always_inline)) StackFrame& parent() { return frames_[depth_ - 1]; }
    __attribute__((always_inline)) StackFrame& frame(std::size_t d) { return frames_[d]; }
    const StackFrame& frame(std::size_t d) const { return frames_[d]; }

    __attribute__((always_inline)) std::size_t base_depth() const { return base_depth_; }
    __attribute__((always_inline)) std::size_t base_limit() const { return base_limit_; }
    void set_base(std::size_t depth, std::size_t limit) {
        assert(depth >= 1 && depth <= depth_);
        base_depth_ = depth;
        base_limit_ = limit;
    }

    // False if prime index i is past the region's end at the current depth
    __attribute__((always_inline)) bool within_region(std::size_t i) const {
        return depth_ != base_depth_ || i < base_limit_;
    }

    // Descend one level, starting the choice at first_idx
    __attribute__((always_inline)) void push(std::size_t first_idx) {
        frames_[++depth_].next_prime_idx = first_idx;
    }

    // Move to the next sibling at the current depth
    __attribute__((always_inline)) void advance() {
        ++frames_[depth_].next_prime_idx;
    }

    // Pop the current depth and advance the parent.
    // Returns false once the region is finished (it would pop the base depth).
    __attribute__((always_inline)) bool backtrack() {
        if (depth_ == base_depth_) return false;
        --depth_;
        advance();
        return true;
    }

private:
    std::array<StackFrame, parameters::MAX_OMEGA + 1> frames_; // Uses search_config().omega + 1
    std::size_t depth_ = 1;       // Depth 0 (the prime 2) is fixed
    std::size_t base_depth_ = 1;  // The region's base depth
    std::size_t base_limit_ = 0;  // Siblings at base_depth_ stop before this prime index
};

/**
 * Runs the DFS over the region in `stack`, calling emit(job) for every support.
 *
 * `support` holds the primes chosen so far (support[0..depth]); its prefix above
 * the base depth must already be set (DfsStack::load does this). poll(stack,
 * support) is called at the top of every iteration, when the stack is between
 * nodes; it may split the region (split_region), which only moves the base
 * depth down or lowers the base limit.
 *
 * Maintains that support contains an increasing list of primes.
 * Supports are iterated through in lexicographic order.
 *
 * Prunes branches based on Corollary 4 of G. K. Bagger, "A Note on Least
 * Primitive Roots" (see docs/current_method.md, sections 2 and 4).
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
template <typename Emit, typename Poll>
__attribute__((always_inline)) inline void run_region(
    DfsStack& stack,
    std::vector<unsigned>& support,
    const std::vector<unsigned>& primes,
    Emit&& process_job,
    Poll&& poll
) {
    const auto& config = lpr::config::search_config();
    const std::size_t OMEGA = config.omega;

    assert(OMEGA > 2 && OMEGA <= parameters::MAX_OMEGA && "Must have at least 2 prime factors to use Cor 4");
    assert(!primes.empty() && primes[0] == 2);
    assert(support.size() == OMEGA && support[0] == 2);
    auto& stats = lpr::stats::get_thread_local_stats();

    // Number of primes to ignore
    const std::size_t DELTA_START_DEPTH = config.delta_start_depth();
    assert(DELTA_START_DEPTH > 0 && "We cannot have OMEGA == S as 2 must be excluded");
    assert(DELTA_START_DEPTH < OMEGA && "Need S >= 1 so leaves always have a Cor 4 bound");

    // Scratch values, reused across regions to avoid GMP allocations
    static thread_local mpz_class min_complete_product;
    static thread_local mpz_class min_complete_delta_Q;
    static thread_local mpz_class min_complete_delta_sop;
    static thread_local mpz_class d_max_mpz;

    while (true) {
        poll(stack, support); // May split the region (between nodes, so the stack is consistent)

        const std::size_t depth = stack.depth();
        const std::size_t remaining_after_choice = OMEGA - depth - 1;
        const std::size_t i = stack.current().next_prime_idx; // The prime chosen at this depth

        if (!stack.within_region(i) || i >= primes.size() || primes.size() - (i + 1) < remaining_after_choice) {
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

        // The node survived pruning: extend the prefix hash by this prime (one mix64
        // per node instead of a full support_hash per leaf)
        next.prefix_hash = lpr::bits::mix64(cur.prefix_hash ^ primes[i]);

        if (depth == OMEGA - 1) {
            mpz_fdiv_q(
                d_max_mpz.get_mpz_t(),
                next.cor4_bound.get_mpz_t(),
                next.product.get_mpz_t()
            ); // d_max = floor(bound / Q)
            assert(d_max_mpz.fits_uint_p() && "d_max exceeds unsigned integer range");
            const unsigned d_max = d_max_mpz.get_ui();

            ++stats.supports_emitted;
            assert(next.prefix_hash == lpr::bits::support_hash(support.data(), support.size())
                   && "incremental prefix hash disagrees with support_hash");
            stats.supports_checksum += next.prefix_hash;
            stats.d_max_sum += d_max;
            if (d_max > stats.d_max_max) {
                stats.d_max_max = d_max;
            }
            process_job(parallel::make_job(support, next.product, d_max)); // Works for every mode
            stack.advance(); // Move to sibling leaf node at same depth
        } else {
            stack.push(i + 1); // Move to next depth and use later primes
        }
    } // while (true)
}

// What the DFS's first checks decide for one sibling (see sibling_fate)
enum class SiblingFate { Survives, InheritedPrune, Cor4Prune };

/*
 * The outcome of run_region's checks for choosing prime index j at depth `depth`
 * under the prefix whose values are in `frame` (the stack frame at that depth),
 * computed exactly as run_region computes them for a sibling that is not a first
 * child: the inherited-bound prune (minimal completion's product exceeds
 * frame.cor4_bound), then, from DELTA_START_DEPTH on, the Corollary 4 prune
 * (product exceeds the minimal completion's own Cor 4 bound). Both are monotone
 * in j. Requires j + 1 + (OMEGA - depth - 1) <= primes.size().
 */
inline SiblingFate sibling_fate(
    const StackFrame& frame,
    std::size_t depth,
    std::size_t j,
    const std::vector<unsigned>& primes
) {
    const auto& config = lpr::config::search_config();
    const std::size_t rem = config.omega - depth - 1;
    const bool evaluates_cor4 = depth >= config.delta_start_depth();
    assert(j + 1 + rem <= primes.size());
    static thread_local mpz_class product, delta_Q, delta_sop, bound;
    mpz_mul_ui(product.get_mpz_t(), frame.product.get_mpz_t(), primes[j]);
    if (evaluates_cor4) {
        mpz_mul_ui(delta_Q.get_mpz_t(), frame.delta_Q.get_mpz_t(), primes[j]);
        mpz_set(delta_sop.get_mpz_t(), frame.delta_Q.get_mpz_t());
        mpz_addmul_ui(delta_sop.get_mpz_t(), frame.delta_sop.get_mpz_t(), primes[j]);
    }
    for (std::size_t t = 1; t <= rem; ++t) {
        mpz_mul_ui(product.get_mpz_t(), product.get_mpz_t(), primes[j + t]);
        if (evaluates_cor4) {
            delta_sop *= primes[j + t];
            delta_sop += delta_Q;
            delta_Q *= primes[j + t];
        }
    }
    if (product > frame.cor4_bound) {
        return SiblingFate::InheritedPrune;
    }
    if (evaluates_cor4) {
        corollary4_qd_bound(delta_Q, delta_sop, bound);
        if (product > bound) {
            return SiblingFate::Cor4Prune;
        }
    }
    return SiblingFate::Survives;
}

/*
 * Splits off part of the region in `stack` into `out` (see docs/work_stealing.md,
 * section 3). Called between DFS iterations, on the stack's own thread.
 *
 * Only untouched siblings at the base depth b (indices cur+1 .. base_limit-1,
 * where cur = frames[b].next_prime_idx is the sibling in progress or about to
 * be visited) are given away: `out` gets [mid, base_limit) and the stack keeps
 * [cur, mid). mid is chosen among the untouched siblings that survive the DFS's
 * first checks (sibling_fate), so a single-threaded run visits it too and every
 * DFS counter stays identical. If no untouched sibling at b survives, the
 * stack's remaining work is the subtree of cur, so the base moves one level
 * down (behaviour-preserving) and the search repeats there; the one visit the
 * traversal would have made to sibling cur+1 (and its prune) is counted, so
 * the DFS counters still match a single-threaded run exactly. Depths deeper than max_split_depth are
 * never split. Returns false (and changes nothing in `out`) if nothing can be
 * given away.
 */
inline bool split_region(
    DfsStack& stack,
    const std::vector<unsigned>& support,
    const std::vector<unsigned>& primes,
    std::size_t max_split_depth,
    Region& out
) {
    const std::size_t OMEGA = lpr::config::search_config().omega;
    const std::size_t num_primes = primes.size();
    auto& stats = lpr::stats::get_thread_local_stats();

    while (true) {
        const std::size_t b = stack.base_depth();
        if (b > max_split_depth || b >= OMEGA) {
            return false;
        }
        const std::size_t rem = OMEGA - b - 1;
        const std::size_t cur = stack.frame(b).next_prime_idx;
        const std::size_t lo = cur + 1; // First untouched sibling
        // Untouched siblings that can be completed at all: j + 1 + rem <= num_primes
        const std::size_t completable_end = num_primes >= rem ? num_primes - rem : 0;
        const std::size_t range_end = std::min(stack.base_limit(), completable_end);

        // feasible_end: first index in [lo, range_end) pruned by the DFS's first
        // checks (monotone in the index), or range_end if none is
        std::size_t feasible_end = lo;
        if (lo < range_end) {
            std::size_t left = lo, right = range_end; // Invariant: indices < left survive, >= right are pruned
            while (left < right) {
                const std::size_t probe = left + (right - left) / 2;
                if (sibling_fate(stack.frame(b), b, probe, primes) != SiblingFate::Survives) {
                    right = probe;
                } else {
                    left = probe + 1;
                }
            }
            feasible_end = left;
        }

        if (feasible_end > lo) { // At least one untouched sibling survives the first checks
            const std::size_t count = feasible_end - lo;
            const std::size_t mid = lo + count / 2; // The thief gets ceil(count / 2) of them
            out.base_depth = b;
            out.base_limit = stack.base_limit();
            out.frames.resize(b + 1);
            for (std::size_t d = 0; d <= b; ++d) {
                out.frames[d] = stack.frame(d);
            }
            out.frames[b].next_prime_idx = mid;
            out.support_prefix.assign(support.begin(), support.begin() + static_cast<std::ptrdiff_t>(b));
            stack.set_base(b, mid);
            ++stats.regions_split;
            ++stats.splits_at_depth[b];
            return true;
        }

        // Nothing worth giving away at b. Step down if the DFS is below b, so all
        // remaining work is in the subtree of cur.
        if (stack.depth() <= b) {
            return false;
        }
        if (lo < range_end) {
            // The traversal would visit sibling lo after finishing cur and prune it
            // (it can't be a first child: cur >= parent + 1, so the checks are not
            // skipped). Count that visit as run_region would, so the DFS counters
            // match a single-threaded run exactly.
            assert(b == 1 || lo > stack.frame(b - 1).next_prime_idx + 1);
            ++stats.dfs_nodes_visited;
            const SiblingFate fate = sibling_fate(stack.frame(b), b, lo, primes);
            assert(fate != SiblingFate::Survives && "feasible_end == lo means sibling lo is pruned");
            if (fate == SiblingFate::InheritedPrune) {
                ++stats.inherited_bound_prunes;
            } else {
                ++stats.cor4_evaluations; // Pruned by its own Corollary 4 bound
            }
        }
        stack.set_base(b + 1, num_primes);
        ++stats.region_step_downs;
    }
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

inline void update_dfs_position(const parallel::LPRJob& job) {
    for (std::size_t i = 0; i < dfs_position.size() && i < job.omega; ++i) {
        dfs_position[i].store(job.support[i], std::memory_order_relaxed);
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

// The support array for a new search: OMEGA entries, support[0] = 2 (fixed)
inline std::vector<unsigned> initial_support(const std::vector<unsigned>& primes) {
    std::vector<unsigned> support(lpr::config::search_config().omega);
    support[0] = primes[0]; // primes[0] == 2
    return support;
}

// A poll that never splits (single-threaded modes)
struct NoPoll {
    void operator()(detail::DfsStack&, std::vector<unsigned>&) const {}
};

/*
 * The emit callables of the single-threaded modes: process each support inline
 * (or, for dry runs, only count it, which run_region does), and every
 * BATCH_SIZE supports update the progress position and publish statistics (at
 * most once per STATS_PUBLISH_INTERVAL_MS).
 */
template <bool Process>
struct InlineEmit {
    std::size_t since_publish = 0;
    void operator()(const parallel::LPRJob& job) {
        if constexpr (Process) {
            consumer::process_support(job);
        }
        if (++since_publish == parameters::BATCH_SIZE) {
            since_publish = 0;
            update_dfs_position(job);
            stats::maybe_publish_thread_local_stats();
        }
    }
};

/*
 * Single-threaded: processes each support as soon as it is emitted.
 */
inline void generate_supports(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    set_dfs_first_primes(primes);
    detail::DfsStack stack(global_bound, primes.size());
    std::vector<unsigned> support = initial_support(primes);
    detail::run_region(stack, support, primes, InlineEmit<true>{}, NoPoll{});
    stats::publish_thread_local_stats();
}

/*
 * Runs the DFS without processing any supports, to measure producer speed.
 * Producer statistics (including supports emitted) are gathered by run_region.
 */
inline void generate_supports_dry_run(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound
) {
    set_dfs_first_primes(primes);
    detail::DfsStack stack(global_bound, primes.size());
    std::vector<unsigned> support = initial_support(primes);
    detail::run_region(stack, support, primes, InlineEmit<false>{}, NoPoll{});
    stats::publish_thread_local_stats();
}

/*
 * Split test (--split-test N, single-threaded): every `interval` DFS steps the
 * current region is split (detail::split_region) and the split-off region is
 * put on a work list, which is run once the current region is finished: last
 * in, first out, or first in, first out with `fifo`. This exercises region
 * splitting and stepping down without threads: the supports checksum and every
 * consumer counter must equal a plain single-threaded run, and the DFS counters
 * may exceed it by at most the number of regions split off. At most
 * MAX_PENDING regions are kept; beyond that, splitting pauses.
 */
template <bool Process>
void generate_supports_split_test(
    const std::vector<unsigned>& primes,
    const mpz_class& global_bound,
    std::size_t interval,
    bool fifo
) {
    constexpr std::size_t MAX_PENDING = 100000;
    set_dfs_first_primes(primes);
    const std::size_t OMEGA = lpr::config::search_config().omega;
    detail::DfsStack stack(global_bound, primes.size());
    std::vector<unsigned> support = initial_support(primes);
    std::deque<detail::Region> pending;
    std::size_t steps = 0;
    InlineEmit<Process> emit;

    auto poll = [&](detail::DfsStack& st, std::vector<unsigned>& sup) {
        if (++steps < interval || pending.size() >= MAX_PENDING) return;
        steps = 0;
        pending.emplace_back();
        if (!detail::split_region(st, sup, primes, OMEGA - 1, pending.back())) {
            pending.pop_back();
        }
    };

    detail::run_region(stack, support, primes, emit, poll);
    while (!pending.empty()) {
        if (fifo) {
            stack.load(pending.front(), support);
            pending.pop_front();
        } else {
            stack.load(pending.back(), support);
            pending.pop_back();
        }
        detail::run_region(stack, support, primes, emit, poll);
    }
    stats::publish_thread_local_stats();
}

/*
 * Producer for the queue mode (--parallel-queue): one producer thread and
 * consumer threads connected by a JobQueue (the design before work stealing,
 * kept for comparison).
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
        update_dfs_position(current_batch.back());
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
        stats::maybe_publish_thread_local_stats();
    };

    // Called by run_region to emit candidate supports
    auto emit_job = [&](const LPRJob& job) {
        current_batch.push_back(job);
        if (current_batch.size() == BATCH_SIZE) {
            submit_batch();
        }
    };

    // Run DFS candidate generator over the whole search
    detail::DfsStack stack(global_bound, primes.size());
    std::vector<unsigned> support = initial_support(primes);
    detail::run_region(stack, support, primes, emit_job, NoPoll{});

    // Flush remaining candidates in final batch
    if (!current_batch.empty()) {
        submit_batch();
    }

    queue.set_finished();
    consumer::start_consumer(queue); // Help drain the queue
}

} // lpr::producer
