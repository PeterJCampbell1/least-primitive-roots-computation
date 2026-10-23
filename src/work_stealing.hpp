/**
 * work_stealing.hpp: the parallel search (--parallel, and --dry-run --threads N).
 *
 * Every thread is a worker: it runs the DFS on its own region of the tree
 * (producer::detail::run_region) and processes its own supports inline, as
 * --single does. There is no producer, queue or consumer. A worker whose region
 * is finished asks a random other worker for work; that victim splits its own
 * region (producer::detail::split_region) and hands the split-off part over.
 * Design, invariants and verification: docs/work_stealing.md.
 *
 * Protocol (only a worker's own thread ever touches its DfsStack):
 *  - each worker has an atomic `request` slot (a thief's id, or -1) and a
 *    mailbox (a Region plus an atomic state EMPTY / FILLED / REFUSED);
 *  - a victim polls its slot at the top of every DFS iteration; if a thief is
 *    waiting, it takes the request (exchange to -1) and answers: on a split it
 *    increments active_ first, writes the region, then stores FILLED (release);
 *    otherwise it stores REFUSED;
 *  - a thief posts a request with a CAS on the victim's slot and waits for the
 *    answer, answering requests to itself with REFUSED meanwhile (otherwise two
 *    idle workers asking each other would wait forever);
 *  - active_ counts workers holding (or being handed) a region; it reaches 0
 *    only when no region exists, and then every worker exits. A thief waiting
 *    when active_ reaches 0 withdraws its request with a CAS; if that fails the
 *    victim has already taken it and will answer.
 **/

#pragma once

#include "config.hpp"
#include "generate_supports.hpp"
#include "parameters.hpp"
#include "stats.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include <gmpxx.h>

namespace lpr::stealing {

namespace detail {

enum MailboxState : int { EMPTY = 0, FILLED = 1, REFUSED = 2 };

// One worker's shared state (its DfsStack is private to its thread)
struct alignas(64) Worker {
    std::atomic<int> request{-1};         // Id of a thief waiting for an answer, or -1
    std::atomic<int> mailbox_state{EMPTY}; // Written by a victim, reset by this worker
    producer::detail::Region mailbox;     // Written by a victim before FILLED
};

} // namespace detail

class Scheduler {
public:
    // max_prefix: regions are split only while their fixed prefix (the primes above
    // the base depth) has fewer than max_prefix primes, i.e. at base depths
    // < max_prefix (tunable with --steal-max-prefix; OMEGA = no limit)
    Scheduler(const std::vector<unsigned>& primes, const mpz_class& bound, unsigned workers, bool stress,
              std::size_t max_prefix)
        : primes_(primes), bound_(bound), num_workers_(workers), stress_(stress), max_prefix_(max_prefix) {
        assert(max_prefix >= 1);
        assert(workers >= 1);
        workers_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            workers_.push_back(std::make_unique<detail::Worker>());
        }
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Runs the whole search with num_workers threads and returns when it is complete.
    // Process = false is a dry run (supports are counted but not processed).
    template <bool Process>
    void run() {
        producer::set_dfs_first_primes(primes_);
        active_.store(1); // Worker 0 starts with the whole search
        std::vector<std::thread> threads;
        threads.reserve(num_workers_);
        for (unsigned id = 0; id < num_workers_; ++id) {
            threads.emplace_back([this, id]() { worker_main<Process>(id); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        assert(active_.load() == 0);
    }

    // Workers currently holding a region (for progress logs; approximate)
    unsigned active_workers() const {
        return active_.load(std::memory_order_relaxed);
    }

private:
    using DfsStack = producer::detail::DfsStack;

    const std::vector<unsigned>& primes_;
    const mpz_class& bound_;
    const unsigned num_workers_;
    const bool stress_;
    const std::size_t max_prefix_;
    std::vector<std::unique_ptr<detail::Worker>> workers_;
    std::atomic<unsigned> active_{0};

    template <bool Process>
    void worker_main(unsigned id) {
        detail::Worker& self = *workers_[id];
        DfsStack stack(bound_, primes_.size()); // Worker 0: the whole search
        std::vector<unsigned> support = producer::initial_support(primes_);
        producer::InlineEmit<Process> emit;
        std::uint64_t rng = 0x9e3779b97f4a7c15ULL * (id + 1); // Victim choice (xorshift64)

        // Victim side: answer a waiting thief between DFS iterations
        auto poll = [this, &self](DfsStack& st, std::vector<unsigned>& sup) {
            if (self.request.load(std::memory_order_relaxed) != -1) {
                answer(self, &st, &sup);
            }
        };

        bool have_region = (id == 0);
        while (true) {
            if (have_region) {
                producer::detail::run_region(stack, support, primes_, emit, poll);
                active_.fetch_sub(1); // This worker's region is finished
            }
            have_region = find_work(id, stack, support, rng);
            if (!have_region) {
                break; // The search is complete
            }
        }
        stats::publish_thread_local_stats();
    }

    /*
     * Takes the request waiting in `self`'s slot, if any, and answers it: with a
     * region split off `stack` if possible, otherwise REFUSED. stack == nullptr
     * means this worker has no region (it is idle), so it always refuses.
     */
    void answer(detail::Worker& self, DfsStack* stack, const std::vector<unsigned>* support) {
        const int thief_id = self.request.exchange(-1, std::memory_order_acq_rel);
        if (thief_id < 0) {
            return; // Withdrawn in the meantime
        }
        detail::Worker& thief = *workers_[static_cast<unsigned>(thief_id)];
        auto& stats = lpr::stats::get_thread_local_stats();
        if (stack == nullptr) {
            ++stats.requests_refused_idle;
            thief.mailbox_state.store(detail::REFUSED, std::memory_order_release);
            return;
        }
        const auto split_start = std::chrono::steady_clock::now();
        const bool split = producer::detail::split_region(*stack, *support, primes_, max_prefix_ - 1, thief.mailbox);
        stats.split_ns += elapsed_ns(split_start);
        if (split) {
            active_.fetch_add(1); // Before the handover, so active_ never reaches 0 while a region is in transit
            thief.mailbox_state.store(detail::FILLED, std::memory_order_release);
        } else {
            ++stats.requests_refused_busy;
            thief.mailbox_state.store(detail::REFUSED, std::memory_order_release);
        }
    }

    static std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point since) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - since).count());
    }

    unsigned pick_victim(unsigned id, std::uint64_t& rng) const {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        const unsigned v = static_cast<unsigned>(rng % (num_workers_ - 1));
        return v >= id ? v + 1 : v; // Any worker but this one
    }

    // After a failed attempt: yield, then sleep 1 us doubling up to STEAL_MAX_BACKOFF_US
    void back_off(unsigned failures) const {
        constexpr unsigned YIELDS = 16;
        const auto start = std::chrono::steady_clock::now();
        if (stress_ || failures < YIELDS) {
            std::this_thread::yield();
        } else {
            const unsigned shift = std::min(failures - YIELDS, 20u);
            const unsigned micros = std::min<unsigned>(1u << shift, parameters::STEAL_MAX_BACKOFF_US);
            std::this_thread::sleep_for(std::chrono::microseconds(micros));
        }
        lpr::stats::get_thread_local_stats().steal_backoff_ns += elapsed_ns(start);
    }

    /*
     * Thief side: this worker has no region. Asks random other workers until one
     * hands over a region (loaded into `stack`; returns true) or the search is
     * complete (returns false).
     */
    bool find_work(unsigned id, DfsStack& stack, std::vector<unsigned>& support, std::uint64_t& rng) {
        auto& stats = lpr::stats::get_thread_local_stats();
        detail::Worker& self = *workers_[id];
        const auto idle_start = std::chrono::steady_clock::now();
        auto finish = [&](bool found) {
            stats.worker_idle_ns += elapsed_ns(idle_start);
            return found;
        };

        unsigned failures = 0;
        while (true) {
            answer(self, nullptr, nullptr); // Nothing to give
            if (active_.load() == 0) {
                return finish(false);
            }
            const unsigned victim_id = pick_victim(id, rng);
            detail::Worker& victim = *workers_[victim_id];
            int expected = -1;
            if (!victim.request.compare_exchange_strong(expected, static_cast<int>(id),
                                                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                back_off(failures++); // Someone else is already asking this victim
                continue;
            }
            ++stats.steal_requests;

            const auto wait_start = std::chrono::steady_clock::now();
            int state;
            while ((state = self.mailbox_state.load(std::memory_order_acquire)) == detail::EMPTY) {
                answer(self, nullptr, nullptr);
                if (active_.load() == 0) {
                    int mine = static_cast<int>(id);
                    if (victim.request.compare_exchange_strong(mine, -1,
                                                               std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        ++stats.steal_withdrawals;
                        stats.steal_wait_ns += elapsed_ns(wait_start);
                        return finish(false); // Nobody will answer: the search is complete
                    }
                    // The victim has taken the request and will answer
                }
                std::this_thread::yield();
            }
            stats.steal_wait_ns += elapsed_ns(wait_start);
            self.mailbox_state.store(detail::EMPTY, std::memory_order_relaxed);
            if (state == detail::FILLED) {
                stack.load(self.mailbox, support); // active_ was incremented by the victim
                return finish(true);
            }
            ++stats.steal_refusals;
            back_off(failures++);
            stats::maybe_publish_thread_local_stats();
        }
    }
};

} // namespace lpr::stealing
