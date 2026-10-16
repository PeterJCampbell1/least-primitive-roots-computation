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

#include <atomic>
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

/* A single support to be processed with associated data. */
struct LPRJob {
    std::vector<unsigned> support;
    mpz_class Q;
    unsigned d_max; // Maximum value of d allowed by corollary 4 in p = Q*d + 1
};
using LPRBatch = std::vector<LPRJob>;

/* The concurrent queue structure (name kept from its earlier hysteresis design) */
template <std::size_t HWM = parameters::HWM>
class HysteresisQueue {
private:
    std::queue<LPRBatch> queue_;
    std::mutex mutex_;
    std::condition_variable cv_consumer_;
    bool finished_ = false;
    std::atomic<std::size_t> size_{0}; // Approximate size, for progress logging

public:
    HysteresisQueue() = default;

    /* No Copying Queue's */
    HysteresisQueue(const HysteresisQueue&) = delete;
    HysteresisQueue& operator=(const HysteresisQueue&) = delete;

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
            ++lpr::stats::get_thread_local_stats().consumer_waits;
            while (queue_.empty() && !finished_) {
                cv_consumer_.wait(lock);
            }
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
