/*
 * Bounded concurrent queue of support batches, with producer co-processing.
 *
 * The producer never sleeps. It calls try_push with each full batch; if the
 * queue already holds HWM batches (the consumers are behind), try_push fails
 * and the producer processes that batch itself before continuing the DFS.
 * This bounds memory (at most HWM batches are queued) while keeping every
 * thread busy. Once the DFS is finished the producer calls set_finished and
 * then becomes a consumer to help drain the queue.
 *
 * Consumers block while the queue is empty and the producer has not
 * finished. Each push wakes exactly one waiting consumer, which avoids
 * several consumers contending over the same batch.
 *
 * A job is a support (OMEGA primes), its product Q (~200 bits) and d_max, so
 * jobs are small; batching (BATCH_SIZE jobs per batch) keeps lock traffic to
 * one lock per batch on each side.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>
#include <gmpxx.h>

#include "parameters.hpp"
#include "stats.hpp"

namespace lpr::parallel {

/*
 * A support to be processed: its primes, its product Q and d_max. Fixed size,
 * with no heap allocation, so emitting, queueing and freeing jobs costs no
 * malloc/free (a std::vector support and an mpz_class Q each allocated before).
 */
struct LPRJob {
    std::array<unsigned, parameters::MAX_OMEGA> support; // support[0..omega), increasing, support[0] == 2
    std::uint32_t omega;
    std::uint32_t d_max; // Maximum value of d allowed by corollary 4 in p = Q*d + 1
    std::array<mp_limb_t, parameters::MAX_Q_LIMBS> q_limbs; // Q, little-endian limbs
    std::uint32_t q_size; // Limbs of Q in use (normalised: the top one is non-zero)

    // Read-only GMP view of Q (no allocation; `storage` must outlive the result)
    mpz_srcptr Q(mpz_t storage) const {
        return mpz_roinit_n(storage, q_limbs.data(), static_cast<mp_size_t>(q_size));
    }
};

inline LPRJob make_job(const std::vector<unsigned>& support, const mpz_class& Q, unsigned d_max) {
    LPRJob job;
    assert(support.size() <= job.support.size());
    assert(mpz_sgn(Q.get_mpz_t()) > 0 && mpz_size(Q.get_mpz_t()) <= job.q_limbs.size());
    job.omega = static_cast<std::uint32_t>(support.size());
    std::copy(support.begin(), support.end(), job.support.begin());
    job.d_max = d_max;
    job.q_size = static_cast<std::uint32_t>(mpz_size(Q.get_mpz_t()));
    std::copy_n(mpz_limbs_read(Q.get_mpz_t()), job.q_size, job.q_limbs.begin());
    return job;
}
using LPRBatch = std::vector<LPRJob>;

/* The concurrent queue of support batches */
template <std::size_t HWM = parameters::HWM>
class JobQueue {
private:
    std::queue<LPRBatch> queue_;
    std::mutex mutex_;
    std::condition_variable cv_consumer_;
    bool finished_ = false;
    std::atomic<std::size_t> size_{0}; // Approximate size, for progress logging

public:
    JobQueue() = default;

    /* No Copying Queue's */
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    /*
     * Producer offers a batch without blocking.
     * Returns true and moves from `batch` if it was queued.
     * Returns false and leaves `batch` untouched if the queue is full.
     */
    bool try_push(LPRBatch& batch) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= HWM) {
                return false;
            }
            queue_.push(std::move(batch));
            size_.store(queue_.size(), std::memory_order_relaxed);
        }
        cv_consumer_.notify_one(); // Wake AN idle consumer
        return true;
    }

    /*
     * Consumers pull a batch of supports.
     * Provided `out_batch` stores the popped batch.
     *
     * Returns whether there was anything to pop.
     */
    bool pop(LPRBatch& out_batch) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (queue_.empty() && !finished_) {
            // The producer is behind: count the wait and how long it takes
            auto& stats = lpr::stats::get_thread_local_stats();
            ++stats.consumer_waits;
            const auto wait_start = std::chrono::steady_clock::now();
            while (queue_.empty() && !finished_) {
                cv_consumer_.wait(lock);
            }
            stats.consumer_wait_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
        }

        if (queue_.empty()) {
            return false; // Finished and drained
        }

        out_batch = std::move(queue_.front());
        queue_.pop();
        size_.store(queue_.size(), std::memory_order_relaxed);
        return true;
    }

    // Signal consumers that DFS traversal is done
    void set_finished() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        cv_consumer_.notify_all();
    }

    std::size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }
};

} // namespace lpr::parallel
