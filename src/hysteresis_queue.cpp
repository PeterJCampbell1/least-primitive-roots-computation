#include "hysteresis_queue.hpp"
#include "parameters.hpp" // TODO - REMOVE IF NOT NEEDED
                          // TODO - INCLUDE OTHER FILES FOR PROCESSING AND GENERATING SUPPORTS!

#include <thread> 
#include <vector>

namespace lpr_parallel { // Namespace auto-merged

/* 
 * Wrapper around DFS to generate supports.
 *
 * Auto-handles batching jobs, and pushing batches to the concurrent queue.
 */
void producer_thread_func(HysteresisQueue& queue) {
    using parameters::BATCH_SIZE;
    LPRBatch current_batch;
    current_batch.reserve(BATCH_SIZE);

    // Lambda passed into dfs_iter to emit candidate supports
    auto emit_job = [&](std::vector<unsigned>&& prime_divisors, mpz_class&& p, mpz_class&& n) {
        current_batch.push_back({std::move(prime_divisors), std::move(p), std::move(n)});
        
        if (current_batch.size() == BATCH_SIZE) {
            queue.push(std::move(current_batch));
            current_batch = LPRBatch(); // Reset batch buffer
            current_batch.reserve(BATCH_SIZE);
        }
    };

    // This can be made more generic with template parameters (but no need here)
    generate_supports(emit_job); // TODO - REWRITE FUNCTION NAME

    // Flush all jobs in final batch if required
    if (!current_batch.empty()) {
        queue.push(std::move(current_batch));
    }

    queue.set_finished();
}

/*
 * Wrapper around process_support that automatically handles interacting
 * with the concurrent queue.
 */
void consumer_worker_func(HysteresisQueue& queue) {
    LPRBatch batch;
    while (queue.pop(batch)) {
        for (auto& job : batch) {
            // TODO - ADD A WAY TO SET COROLLARY 4 THRESHOLD!
            process_support();
        }
    }
}

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
