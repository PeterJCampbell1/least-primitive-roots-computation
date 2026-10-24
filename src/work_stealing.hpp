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
 *  - active_ counts regions that exist: held by a worker, being handed over, or
 *    pending (a task not yet taken, e.g. from a checkpoint). It reaches 0 only
 *    when no region exists, and then every worker exits. A thief waiting when
 *    active_ reaches 0 withdraws its request with a CAS; if that fails the
 *    victim has already taken it and will answer.
 *
 * Pausing for checkpoints (docs/checkpointing.md): pause() asks every worker to
 * stop at a safe point (a busy worker between DFS nodes, an idle one in its
 * steal loop). A stopping worker first publishes its statistics, so while all
 * are stopped the master statistics cover exactly the work done before their
 * positions. remaining_tasks() then lists every region as a Task (verified by
 * replay against the live frames), and resume() lets the workers continue.
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
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
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
    using Task = producer::detail::Task;

    // max_prefix: regions are split only while their fixed prefix (the primes above
    // the base depth) has fewer than max_prefix primes, i.e. at base depths
    // < max_prefix (tunable with --steal-max-prefix; OMEGA = no limit).
    // tasks: the work to do (the root task for a new search, or a checkpoint's
    // tasks); each must restore (DfsStack::restore), which main checks beforehand.
    Scheduler(const std::vector<unsigned>& primes, const mpz_class& bound, unsigned workers, bool stress,
              std::size_t max_prefix, std::vector<Task> tasks)
        : primes_(primes), bound_(bound), num_workers_(workers), stress_(stress), max_prefix_(max_prefix),
          pending_(std::move(tasks)), parked_stacks_(workers, nullptr) {
        assert(max_prefix >= 1);
        assert(workers >= 1);
        workers_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            workers_.push_back(std::make_unique<detail::Worker>());
        }
        active_.store(static_cast<unsigned>(pending_.size()));
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Runs the whole search with num_workers threads and returns when it is complete.
    // Process = false is a dry run (supports are counted but not processed).
    template <bool Process>
    void run() {
        producer::set_dfs_first_primes(primes_);
        std::vector<std::thread> threads;
        threads.reserve(num_workers_);
        for (unsigned id = 0; id < num_workers_; ++id) {
            threads.emplace_back([this, id]() { worker_main<Process>(id); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        assert(active_.load() == 0);
        assert(pending_.empty());
    }

    // Workers currently holding a region (for progress logs; approximate)
    unsigned active_workers() const {
        return active_.load(std::memory_order_relaxed);
    }

    enum class PauseResult { Paused, SearchFinishing, TimedOut };

    /*
     * Called by one other thread (the monitor). Stops every worker at a safe
     * point and returns Paused with them stopped; call remaining_tasks(), read
     * the master statistics, then resume(). SearchFinishing: a worker has
     * already exited, so the search is complete (no region is left) and the
     * workers keep running to the end. TimedOut: not every worker stopped
     * within `timeout`; the workers keep running.
     */
    PauseResult pause(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(pause_mutex_);
        pause_requested_.store(true, std::memory_order_relaxed);
        const bool all = pause_cv_.wait_for(lock, timeout, [this]() {
            return parked_ + exited_ == num_workers_;
        });
        if (!all || exited_ > 0) {
            release_locked();
            return all ? PauseResult::SearchFinishing : PauseResult::TimedOut;
        }
        return PauseResult::Paused;
    }

    // Lets paused workers continue (after a Paused result)
    void resume() {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        release_locked();
    }

    /*
     * While paused: every region that still exists, as Tasks. Each is checked by
     * replaying it (DfsStack::restore) and comparing the rebuilt frames with the
     * live ones, their number must equal active_ (none lost), and they must be
     * disjoint (none listed twice). Returns false with a reason
     * if any check fails (the checkpoint must then not be written).
     */
    bool remaining_tasks(std::vector<Task>& out, std::string& error) {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        assert(pause_requested_.load() && parked_ == num_workers_);
        out.clear();
        DfsStack replay(bound_, primes_.size());
        std::vector<unsigned> replay_support = producer::initial_support(primes_);
        for (unsigned id = 0; id < num_workers_; ++id) {
            const DfsStack* stack = parked_stacks_[id];
            const detail::Worker& worker = *workers_[id];
            const int state = worker.mailbox_state.load(std::memory_order_acquire);
            if (stack != nullptr) { // Busy: its stack is at a safe point
                if (state == detail::FILLED) {
                    error = "worker " + std::to_string(id) + " holds a region and has a filled mailbox";
                    return false;
                }
                Task task = stack->task();
                std::string why;
                if (!replay.restore(task, primes_, bound_, replay_support, &why)) {
                    error = "worker " + std::to_string(id) + ": position does not replay: " + why;
                    return false;
                }
                for (std::size_t d = 0; d <= task.depth; ++d) {
                    if (!producer::detail::same_values(replay.frame(d), stack->frame(d))) {
                        error = "worker " + std::to_string(id) + ": replayed frame " + std::to_string(d)
                                + " differs from the live frame";
                        return false;
                    }
                }
                out.push_back(std::move(task));
            } else if (state == detail::FILLED) { // Idle, with a region handed over but not yet loaded
                Task task = producer::detail::region_task(worker.mailbox);
                std::string why;
                if (!replay.restore(task, primes_, bound_, replay_support, &why)) {
                    error = "worker " + std::to_string(id) + ": mailbox region does not replay: " + why;
                    return false;
                }
                for (std::size_t d = 0; d <= task.depth; ++d) {
                    if (!producer::detail::same_values(replay.frame(d), worker.mailbox.frames[d])) {
                        error = "worker " + std::to_string(id) + ": mailbox frame " + std::to_string(d)
                                + " differs from its replay";
                        return false;
                    }
                }
                out.push_back(std::move(task));
            }
        }
        {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            out.insert(out.end(), pending_.begin(), pending_.end());
        }
        const unsigned active = active_.load();
        if (out.size() != active) {
            error = "found " + std::to_string(out.size()) + " regions but " + std::to_string(active) + " exist";
            return false;
        }
        std::string why;
        if (!producer::detail::tasks_disjoint(out, &why)) {
            error = "regions overlap: " + why;
            return false;
        }
        return true;
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

    // Tasks not yet taken by a worker (counted in active_)
    std::mutex pending_mutex_;
    std::vector<Task> pending_;

    // Pausing (see pause()): guarded by pause_mutex_, except the request flag,
    // which workers poll without the lock
    std::atomic<bool> pause_requested_{false};
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    unsigned parked_ = 0;               // Workers stopped in the current pause
    unsigned exited_ = 0;               // Workers that have finished (the search is complete)
    std::uint64_t pause_generation_ = 0; // Incremented when a pause ends
    std::vector<const DfsStack*> parked_stacks_; // A stopped worker's stack (nullptr if idle)

    void release_locked() {
        pause_requested_.store(false, std::memory_order_relaxed);
        parked_ = 0;
        ++pause_generation_;
        pause_cv_.notify_all();
    }

    // Worker side: stops here until the pause ends. stack: this worker's stack
    // (at a safe point), or nullptr if it holds no region.
    void park(unsigned id, const DfsStack* stack) {
        stats::publish_thread_local_stats(); // The pause's statistics include all work so far
        std::unique_lock<std::mutex> lock(pause_mutex_);
        if (!pause_requested_.load(std::memory_order_relaxed)) {
            return; // Already ended
        }
        const std::uint64_t generation = pause_generation_;
        parked_stacks_[id] = stack;
        ++parked_;
        pause_cv_.notify_all();
        pause_cv_.wait(lock, [&]() { return pause_generation_ != generation; });
        parked_stacks_[id] = nullptr;
    }

    __attribute__((always_inline)) bool pause_requested() const {
        return pause_requested_.load(std::memory_order_relaxed);
    }

    template <bool Process>
    void worker_main(unsigned id) {
        detail::Worker& self = *workers_[id];
        DfsStack stack(bound_, primes_.size());
        std::vector<unsigned> support = producer::initial_support(primes_);
        producer::InlineEmit<Process> emit;
        std::uint64_t rng = 0x9e3779b97f4a7c15ULL * (id + 1); // Victim choice (xorshift64)

        // Between DFS iterations: stop for a pause, or answer a waiting thief
        auto poll = [this, &self, id](DfsStack& st, std::vector<unsigned>& sup) {
            if (pause_requested()) {
                park(id, &st);
            }
            if (self.request.load(std::memory_order_relaxed) != -1) {
                answer(self, &st, &sup);
            }
        };

        while (find_work(id, stack, support, rng)) {
            producer::detail::run_region(stack, support, primes_, emit, poll);
            active_.fetch_sub(1); // This worker's region is finished
        }
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            ++exited_;
            pause_cv_.notify_all();
        }
        stats::publish_thread_local_stats();
    }

    // Takes a pending task, if any, and loads it into `stack`
    bool take_pending(DfsStack& stack, std::vector<unsigned>& support) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_.empty()) {
                return false;
            }
            task = std::move(pending_.back());
            pending_.pop_back();
        }
        std::string why;
        if (!stack.restore(task, primes_, bound_, support, &why)) {
            // main checks every task before the search starts, so this cannot happen
            std::cerr << "Fatal: a pending task does not restore: " << why << std::endl;
            std::abort();
        }
        return true;
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
        assert(num_workers_ > 1);
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
     * This worker has no region. Takes a pending task if there is one, otherwise
     * asks random other workers until one hands over a region (loaded into
     * `stack`; returns true) or the search is complete (returns false).
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
            if (pause_requested()) {
                park(id, nullptr);
            }
            answer(self, nullptr, nullptr); // Nothing to give
            if (take_pending(stack, support)) {
                return finish(true); // active_ already counts it
            }
            if (active_.load() == 0) {
                return finish(false);
            }
            if (num_workers_ == 1) {
                // The only worker holds nothing and no task is pending, so no region
                // can exist. Stopping here would silently skip work: abort instead.
                std::cerr << "Fatal: " << active_.load() << " regions exist but none can be found" << std::endl;
                std::abort();
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
                if (pause_requested()) {
                    park(id, nullptr); // The request stays posted; the victim answers after the pause
                }
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
