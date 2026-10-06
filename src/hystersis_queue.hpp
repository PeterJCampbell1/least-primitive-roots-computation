/*
 * A low-water/high-water hystersis queue has a low and high water mark.
 * This is a modification to the basic producer/consumer architecture.
 *
 * When the queue size exceeds the high water mark the producer stops creating batches
 * (in order to save memory). If the producer is sleeping, when passing the low
 * water mark the producer awakens and returns to creating job batches.
 *
 * The consumers stay awake consuming from the queue unless the queue is empty.
 * If empty, the producer notifies exactly one consumer when adding a new
 * batch (this prevents multiple consumers contending over the same batch too often).
 *
 * If the producer has finished producing, it waits until the queue is empty before
 * aggregating results and ending the program. Each consumer thread will add it's 
 * statistics to a master statistics thread upon ending it's lifetime.
 *
 * For our purposes, our producer completes DFS with local delta pruning. A single
 * job, is a Corollary4Threshold (lhs, rhs, increment), a support which is a static
 * array of size OMEGA (< 34) containing primes and a value Q (the product of all
 * primes in the support). There should be a globally accessible value `bound`. The
 * value Q is bounded by ~200 bits, and lhs/rhs/increment are bounded by ~3500 bits.
 * The support is bounded by 3000 bits, so in total each job should be bounded above
 * (conservatively) by 15000 bits (around 235 words and fewer than 2KB).
 *
 * Given these values, we add parameters to control the batch size and high/low water
 * marks. Note - we want batches to be reasonably large to avoid contention between
 * threads.
 *
 * The BATCH_SIZE is set to 256 - which provides an upper bound of 512KB per batch.
 * We set the HVM (high water mark) to 512 - providing an upper bound on memory of 
 * 256MB which is acceptable on modern hardware. This may be raised in future for 
 * operation on different hardware.
 * We set the LVM (low water mark) to 128 (1/4 of the HVM). This provides a runway
 * of 384 batches for queue build up or consumption to avoid contention.
 */

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <utility>
#include <gmp.h>
#include <gmpxx.h>

#include "corollary4.hpp"
#include "parameters.hpp"

namespace lpr_parallel {

/* A single support to be processed with associated data. */
struct LPRJob {
    std::vector<unsigned> support;
    mpz_class Q;
    Corollary4Threshold threshold;
}
using LPRBatch = std::vector<LPRJob>;

/* The concurrent queue structure */
template <
    size_t HWM = parameters::HWM,
    size_t LWM = parameters::LWM
>
class HysteresisQueue {
private:
    std::queue<LPRBatch> queue_;
    std::mutex mutex_;
    std::condition_variable cv_consumer_;
    std::condition_variable cv_producer_;
    bool producer_paused_ = false;
    bool finished_ = false;

public:
    HysteresisQueue() = default;

    /* No Copying Queue's */
    HysteresisQueue(const HysteresisQueue&) = delete;
    HysteresisQueue& operator=(const HysteresisQueue&) = delete;

    // Producer pushes a batch of supports
    void push(LPRBatch&& batch) {
        std::unique_lock<std::mutex> lock(mutex_); // Auto release by RAII

        // Block producer if we hit HVM
        while (queue_.size() >= HVM) {
            producer_paused_ = true;
            cv_producer_.wait(lock); // Auto release/take lock
        }

        queue_.push(std::move(batch));
        
        // Notify AN idle consumer thread that work is available
        cv_consumer_.notify_one();
    }

    /*
     * Consumers pull a batch of supports.
     * Provided `out_batch` stores the popped batch.
     * 
     * Returns whether there was anything to pop.
     */
    bool pop(LPRBatch& out_batch) {
        std::unique_lock<std::mutex> lock(mutex_);

        while (queue_.empty() && !finished_) {
            cv_consumer_.wait(lock);
        }

        if (queue_.empty() && finished_) {
            return false; // Work complete
        }

        out_batch = std::move(queue_.front());
        queue_.pop();

        // If producer was paused and queue fell to LVM, wake producer up
        if (producer_paused_ && queue_.size() <= LVM) {
            producer_paused_ = false;
            cv_producer_.notify_all();
        }

        return true;
    }

    // Signal consumers that DFS traversal is done
    void set_finished() {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_ = true;
        cv_consumer_.notify_all();

        // This next line is not necessary (since the unique producer must call it).
        // But in general we would signal any sleeping producers if we have multiple,
        // in the event that one is sleeping after pushing on HVM, but the other 
        // completes all remaining production.
        // This is kept in case we use this queue in future with multiple producers.
        cv_producer_.notify_all();
    }
};

}

/* TEMP {
	#include <thread>
	#include <vector>

	constexpr size_t BATCH_SIZE = 64;       // 64 supports per batch
	constexpr size_t HWM_BATCHES = 512;     // High watermark: 512 * 64 = 32,768 supports max
	constexpr size_t LWM_BATCHES = 128;     // Low watermark:  128 * 64 = 8,192 supports min

	void producer_thread_func(HysteresisQueue& queue) {
		CandidateBatch current_batch;
		current_batch.reserve(BATCH_SIZE);

		// Lambda passed into dfs_iter to emit candidate supports
		auto emit_candidate = [&](std::vector<unsigned>&& prime_divisors, mpz_class&& p, mpz_class&& n) {
			current_batch.push_back({std::move(prime_divisors), std::move(p), std::move(n)});
			
			if (current_batch.size() == BATCH_SIZE) {
				queue.push(std::move(current_batch));
				current_batch = CandidateBatch(); // Reset batch buffer
				current_batch.reserve(BATCH_SIZE);
			}
		};

		// Run DFS candidate generator
		run_dfs_generation(emit_candidate);

		// Flush remaining candidates in final batch
		if (!current_batch.empty()) {
			queue.push(std::move(current_batch));
		}

		queue.set_finished();
	}

	void consumer_worker_func(HysteresisQueue& queue) {
		CandidateBatch batch;
		while (queue.pop(batch)) {
			for (auto& candidate : batch) {
				process_support(candidate.prime_divisors, candidate.p, candidate.n);
			}
		}
	}

	void run_parallel_pipeline(size_t num_consumers) {
		HysteresisQueue queue(HWM_BATCHES, LWM_BATCHES);

		// Spawn 1 Producer thread for DFS
		std::thread producer(producer_thread_func, std::ref(queue));

		// Spawn N Consumer worker threads for process_support checks
		std::vector<std::thread> consumers;
		consumers.reserve(num_consumers);
		for (size_t i = 0; i < num_consumers; ++i) {
			consumers.emplace_back(consumer_worker_func, std::ref(queue));
		}

		producer.join();
		for (auto& worker : consumers) {
			worker.join();
		}
	}
}
*/

