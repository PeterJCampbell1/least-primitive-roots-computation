/**
 * mpi_comm.hpp: one search across several MPI ranks (lpr_mpi --mpi; design and
 * verification log in docs/mpi.md).
 *
 * Shape: one rank per node. Each rank runs the work-stealing Scheduler with
 * its worker threads, plus this Node on the thread that initialised MPI (the
 * only thread that calls MPI: MPI_THREAD_FUNNELED). The Node takes part in the
 * scheduler's protocol as one more thread (Scheduler's external endpoint): it
 * adds tasks received from rank 0 and asks its own workers for splits to give
 * away. Workers never touch MPI.
 *
 * Hierarchy of where work comes from (cheapest first):
 *  L0 an idle worker steals from another worker on its node (unchanged);
 *  L1 a node asks rank 0 for a task once request_idle of its workers have been
 *     idle for a moment (or it holds no region at all), so the task usually
 *     arrives before the node runs dry. At most one request per node is open;
 *  L2 rank 0 serves requests from its pool. When the pool cannot cover the open
 *     requests plus pool_reserve, it asks the busy node with the most work (the
 *     shallowest base depth) to donate: that node splits one of its workers'
 *     regions, exactly as a local steal, and sends the piece to rank 0.
 *
 * Termination (rank 0 sees every task that moves between ranks): every message
 * to rank 0 carries the sender's Status, including how many tasks it has
 * received and how many regions it holds. A rank whose last Status says
 * "0 regions, received k" with k = tasks issued to it holds nothing and can only
 * get work from rank 0 again (messages between two ranks arrive in order, so a
 * donation sent before that Status has already arrived). When the pool is
 * empty, no donation is outstanding and every rank is in that state, no region
 * exists anywhere: rank 0 sends STOP, every rank answers STOP_ACK as its last
 * message, and the ranks' statistics are gathered and merged (Counters::merge)
 * on rank 0.
 *
 * Checkpoints (docs/mpi_checkpoint_plan.md): rank 0 coordinates them, every
 * checkpoint_interval seconds or when any rank reports a signal (SIGTERM, SIGINT,
 * SIGUSR1; every rank only reports, rank 0 acts once). Rank 0 stops handing out
 * tasks and asking for donations and sends PAUSE. Each rank finishes any donation in
 * progress, pauses its workers (Scheduler::pause) and answers PAUSED with its
 * remaining tasks (Scheduler::remaining_tasks, checked by replay) and its Counters.
 * Messages between two ranks arrive in order, so every TASK sent before PAUSE has
 * arrived before it, and every TASK_IN sent before PAUSED has reached rank 0's pool:
 * once every rank has answered, the pool plus the ranks' tasks are all the work
 * left, with nothing in transit. Rank 0 checks them (disjoint, issued = received),
 * writes one checkpoint in the single-node format through the writer callback, and
 * sends RESUME, or EXIT (code 4 after a signal; 3 if no checkpoint was written),
 * which every rank acknowledges with EXIT_ACK as its last message. A resumed run
 * may use any number of ranks: rank 0 reads the checkpoint and pools its tasks.
 *
 * Watchdog: rank 0 sends every rank a heartbeat every HEARTBEAT_TO_RANKS, and
 * every rank sends rank 0 a Status every STATUS_INTERVAL. A rank that hears
 * nothing from rank 0 for WATCHDOG (or rank 0 from a rank) calls MPI_Abort, so
 * a dead or hung rank ends the job instead of leaving the others waiting until
 * the time limit.
 **/

#pragma once

#ifdef LPR_MPI

#include "generate_supports.hpp"
#include "logging.hpp"
#include "stats.hpp"
#include "work_stealing.hpp"

#include <mpi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <unistd.h> // gethostname
#include <gmpxx.h>

namespace lpr::mpi {

using Task = producer::detail::Task;
using Words = std::vector<std::uint64_t>;
using Clock = std::chrono::steady_clock;

// MPI_Init_thread (FUNNELED) on construction, MPI_Finalize on destruction
class Session {
public:
    Session(int& argc, char**& argv) {
        int provided = 0;
        MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
        if (provided < MPI_THREAD_FUNNELED) {
            std::cerr << "Error: the MPI library does not support MPI_THREAD_FUNNELED\n";
            MPI_Abort(MPI_COMM_WORLD, 2);
        }
        MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
        MPI_Comm_size(MPI_COMM_WORLD, &size_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { MPI_Finalize(); }

    int rank() const { return rank_; }
    int size() const { return size_; }

private:
    int rank_ = 0;
    int size_ = 1;
};

// Messages to rank 0 have tags below TAG_TO_RANKS; every one starts with a Status
enum Tag : int {
    TAG_REQUEST = 1,  // rank -> 0: Status; wants a task
    TAG_STATUS,       // rank -> 0: Status only
    TAG_TASK_IN,      // rank -> 0: Status + a donated task
    TAG_NO_DONATION,  // rank -> 0: Status; nothing to give
    TAG_STOP_ACK,     // rank -> 0: Status; the last message from this rank
    TAG_SIGNAL,       // rank -> 0: Status + signal number; this rank received a signal
    TAG_PAUSED,       // rank -> 0: Status + remaining tasks + Counters (checkpoint)
    TAG_PAUSE_FAILED, // rank -> 0: Status; the workers did not stop in time
    TAG_EXIT_ACK,     // rank -> 0: Status; the last message before exiting
    TAG_TO_RANKS = 20,
    TAG_TASK = TAG_TO_RANKS, // 0 -> rank: a task
    TAG_DONATE,       // 0 -> rank: split off a task for the pool
    TAG_HEARTBEAT,    // 0 -> rank: rank 0 is alive
    TAG_STOP,         // 0 -> rank: no work is left anywhere
    TAG_PAUSE,        // 0 -> rank: pause for a checkpoint
    TAG_RESUME,       // 0 -> rank: continue after a checkpoint
    TAG_EXIT,         // 0 -> rank: exit with the code given
};

inline const char* tag_name(int tag) {
    switch (tag) {
    case TAG_REQUEST: return "REQUEST";
    case TAG_STATUS: return "STATUS";
    case TAG_TASK_IN: return "TASK_IN";
    case TAG_NO_DONATION: return "NO_DONATION";
    case TAG_STOP_ACK: return "STOP_ACK";
    case TAG_TASK: return "TASK";
    case TAG_DONATE: return "DONATE";
    case TAG_HEARTBEAT: return "HEARTBEAT";
    case TAG_STOP: return "STOP";
    case TAG_SIGNAL: return "SIGNAL";
    case TAG_PAUSED: return "PAUSED";
    case TAG_PAUSE_FAILED: return "PAUSE_FAILED";
    case TAG_EXIT_ACK: return "EXIT_ACK";
    case TAG_PAUSE: return "PAUSE";
    case TAG_RESUME: return "RESUME";
    case TAG_EXIT: return "EXIT";
    }
    return "?";
}

// What a rank tells rank 0 with every message
struct Status {
    std::uint64_t received = 0;     // Tasks received from rank 0 so far
    std::uint64_t regions = 0;      // Scheduler::regions() when sent (0: holds nothing)
    std::uint64_t idle_workers = 0;
    std::uint64_t base_depth = 0;   // Shallowest base depth of a busy worker (0: none)
    std::uint64_t supports_processed = 0; // Progress (from the last statistics snapshot)
    std::uint64_t supports_emitted = 0;
    std::uint64_t candidates = 0;
    std::uint64_t fermat_survivors = 0;
    std::uint64_t certified = 0;
    std::uint64_t failed = 0;            // Grosswald inequality failures
    std::uint64_t unresolved = 0;        // No least primitive root within the search limit
    std::uint64_t largest_g = 0;
    static constexpr std::size_t WORDS = 12;

    void append_to(Words& w) const {
        w.insert(w.end(), {received, regions, idle_workers, base_depth, supports_processed, supports_emitted,
                           candidates, fermat_survivors, certified, failed, unresolved, largest_g});
    }
    static Status read(const Words& w) {
        Status s;
        s.received = w[0];
        s.regions = w[1];
        s.idle_workers = w[2];
        s.base_depth = w[3];
        s.supports_processed = w[4];
        s.supports_emitted = w[5];
        s.candidates = w[6];
        s.fermat_survivors = w[7];
        s.certified = w[8];
        s.failed = w[9];
        s.unresolved = w[10];
        s.largest_g = w[11];
        return s;
    }
};

// A Task as words: base_depth, base_limit, depth, path[0..depth]
inline void append_task(Words& w, const Task& t) {
    w.push_back(t.base_depth);
    w.push_back(t.base_limit);
    w.push_back(t.depth);
    for (std::size_t i : t.path) w.push_back(i);
}

inline bool read_task(const Words& w, std::size_t offset, Task& t) {
    if (w.size() < offset + 3) return false;
    t.base_depth = static_cast<std::size_t>(w[offset]);
    t.base_limit = static_cast<std::size_t>(w[offset + 1]);
    t.depth = static_cast<std::size_t>(w[offset + 2]);
    if (t.depth > parameters::MAX_OMEGA || w.size() != offset + 3 + t.depth + 1) return false;
    t.path.assign(w.begin() + static_cast<std::ptrdiff_t>(offset + 3), w.end());
    return true;
}

// A list of Tasks as words: count, then (length, task words) for each
inline void append_tasks(Words& w, const std::vector<Task>& tasks) {
    w.push_back(tasks.size());
    for (const Task& t : tasks) {
        Words one;
        append_task(one, t);
        w.push_back(one.size());
        w.insert(w.end(), one.begin(), one.end());
    }
}

// Reads a task list at w[offset]; offset moves past it. False if malformed.
inline bool read_tasks(const Words& w, std::size_t& offset, std::vector<Task>& tasks) {
    if (offset >= w.size()) return false;
    const std::uint64_t n = w[offset++];
    for (std::uint64_t k = 0; k < n; ++k) {
        if (offset >= w.size()) return false;
        const std::uint64_t len = w[offset++];
        if (len > w.size() - offset) return false;
        const Words one(w.begin() + static_cast<std::ptrdiff_t>(offset),
                        w.begin() + static_cast<std::ptrdiff_t>(offset + len));
        Task t;
        if (!read_task(one, 0, t)) return false;
        tasks.push_back(std::move(t));
        offset += len;
    }
    return true;
}

// Counters as words (Counters::for_each_field order) and back
inline Words counters_words(stats::Counters c) {
    Words w;
    stats::Counters::for_each_field(c, [&](const std::string&, std::uint64_t& v) { w.push_back(v); });
    return w;
}

inline stats::Counters counters_from(const std::uint64_t* w) {
    stats::Counters c;
    std::size_t i = 0;
    stats::Counters::for_each_field(c, [&](const std::string&, std::uint64_t& v) { v = w[i++]; });
    return c;
}

// The rank's MPI activity, for the summary (one row per rank)
struct NodeCounters {
    std::uint64_t tasks_received = 0;
    std::uint64_t requests_sent = 0;
    std::uint64_t request_wait_ns = 0;   // Time with a request open
    std::uint64_t donations_asked = 0;   // DONATE messages received
    std::uint64_t donations_sent = 0;
    std::uint64_t donations_refused = 0;
    std::uint64_t messages_sent = 0;     // Including rank 0's messages to itself
    std::uint64_t messages_received = 0;
    std::uint64_t tasks_issued = 0;      // Rank 0: tasks handed out from the pool
    std::uint64_t donations_requested = 0; // Rank 0: DONATE messages sent
    std::uint64_t pool_max = 0;          // Rank 0: largest pool size
    std::uint64_t worker_idle_ns = 0;    // Filled in from the rank's Counters at the end
    std::uint64_t supports_processed = 0;
    std::uint64_t supports_emitted = 0;
    std::uint64_t elapsed_ns = 0;
    std::uint64_t paused_ns = 0;         // Time this rank's workers were paused for checkpoints
    std::uint64_t checkpoints_written = 0; // Rank 0
    std::uint64_t checkpoints_failed = 0;  // Rank 0: collected but not written
    static constexpr std::size_t WORDS = 18;

    Words words() const {
        return {tasks_received, requests_sent, request_wait_ns, donations_asked, donations_sent, donations_refused,
                messages_sent, messages_received, tasks_issued, donations_requested, pool_max, worker_idle_ns,
                supports_processed, supports_emitted, elapsed_ns, paused_ns, checkpoints_written, checkpoints_failed};
    }
    static NodeCounters from(const std::uint64_t* w) {
        NodeCounters c;
        c.tasks_received = w[0]; c.requests_sent = w[1]; c.request_wait_ns = w[2]; c.donations_asked = w[3];
        c.donations_sent = w[4]; c.donations_refused = w[5]; c.messages_sent = w[6]; c.messages_received = w[7];
        c.tasks_issued = w[8]; c.donations_requested = w[9]; c.pool_max = w[10]; c.worker_idle_ns = w[11];
        c.supports_processed = w[12]; c.supports_emitted = w[13]; c.elapsed_ns = w[14];
        c.paused_ns = w[15]; c.checkpoints_written = w[16]; c.checkpoints_failed = w[17];
        return c;
    }
};

struct NodeOptions {
    unsigned request_idle = 1;  // L1: ask rank 0 once this many workers are idle
    unsigned pool_reserve = 1;  // L2: tasks rank 0 keeps beyond the open requests
    bool stress = false;        // Random delays, late start, constant donations
    bool static_split = false;  // M1: hand out a fixed split, never rebalance
    bool checkpoints = false;   // Write checkpoints (through the writer callback)
    double checkpoint_interval = 0; // Seconds between periodic checkpoints (0: only on signals)
    std::chrono::milliseconds pause_timeout{60000}; // Workers must stop within this (watchdog: 120 s)
};

// Rank 0: writes a checkpoint of `tasks` and `stats` (the earlier runs' work
// included); false with a reason if it could not
using CheckpointWriter = std::function<bool(const std::vector<Task>& tasks, const stats::Counters& stats,
                                            std::string& error)>;

inline std::string hostname() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return "unknown";
    return name;
}

// Every rank's host name (collective; valid on rank 0)
inline std::vector<std::string> gather_hostnames(int rank, int size) {
    constexpr int LEN = 64;
    char mine[LEN] = {};
    std::snprintf(mine, LEN, "%s", hostname().c_str());
    std::vector<char> all(static_cast<std::size_t>(LEN) * static_cast<std::size_t>(size));
    MPI_Gather(mine, LEN, MPI_CHAR, all.data(), LEN, MPI_CHAR, 0, MPI_COMM_WORLD);
    std::vector<std::string> names;
    if (rank == 0) {
        for (int r = 0; r < size; ++r) {
            names.emplace_back(all.data() + static_cast<std::size_t>(r) * LEN);
        }
    }
    return names;
}

/*
 * M1 (--mpi-static): splits the root into up to `pieces` tasks without running
 * the DFS (repeated split_region on a fresh root stack, on the calling thread;
 * the split counters land in its statistics). Poorly balanced by design: the
 * pieces halve in size. Only for testing the task plumbing.
 */
inline std::vector<Task> static_split(const std::vector<unsigned>& primes, const mpz_class& bound,
                                      std::size_t pieces) {
    producer::detail::DfsStack stack(bound, primes.size());
    std::vector<unsigned> support = producer::initial_support(primes);
    std::vector<Task> tasks;
    producer::detail::Region region;
    const std::size_t omega = lpr::config::search_config().omega;
    while (tasks.size() + 1 < pieces && producer::detail::split_region(stack, support, primes, omega - 1, region)) {
        tasks.push_back(producer::detail::region_task(region));
    }
    tasks.push_back(stack.task());
    return tasks;
}

class Node {
public:
    static constexpr auto STATUS_INTERVAL = std::chrono::seconds(1);
    static constexpr auto HEARTBEAT_TO_RANKS = std::chrono::seconds(10);
    static constexpr auto WATCHDOG = std::chrono::seconds(120);
    static constexpr auto DONOR_RETRY = std::chrono::milliseconds(20); // After a NO_DONATION
    static constexpr auto REQUEST_DEBOUNCE = std::chrono::milliseconds(2); // Idle this long before L1

    // initial: rank 0's pool at the start (the root task, M1's pieces, or a
    // checkpoint's tasks). signal_flag: set (non-zero) by the process's signal
    // handler. writer: rank 0's checkpoint writer (needed if options.checkpoints).
    Node(stealing::Scheduler& scheduler, const std::vector<unsigned>& primes, const mpz_class& bound,
         int rank, int size, NodeOptions options, std::vector<Task> initial,
         const volatile std::sig_atomic_t* signal_flag = nullptr, CheckpointWriter writer = {})
        : scheduler_(scheduler), primes_(primes), bound_(bound), rank_(rank), size_(size), options_(options),
          check_stack_(bound, primes.size()), check_support_(producer::initial_support(primes)),
          rng_(0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(rank + 1)),
          trace_(std::getenv("LPR_MPI_TRACE") != nullptr), signal_flag_(signal_flag), writer_(std::move(writer)) {
        if (rank_ == 0) {
            pool_.assign(initial.begin(), initial.end());
            ranks_.resize(static_cast<std::size_t>(size_));
            pool_size_.store(pool_.size(), std::memory_order_relaxed);
        }
    }

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    /*
     * Runs until STOP (on the thread that initialised MPI) and returns -1, or until
     * rank 0 ends the run after a signal and returns the exit code (4: checkpoint
     * written; 3: none). After an exit code the workers may still be paused or
     * running: the caller prints, finalises MPI and exits without joining them.
     */
    int run() {
        const auto start = Clock::now();
        if (options_.stress) { // Late start: up to 200 ms
            std::this_thread::sleep_for(std::chrono::milliseconds(random_below(200)));
        }
        last_from_root_ = Clock::now();
        if (rank_ == 0) {
            for (auto& view : ranks_) view.last_heard = Clock::now();
            last_heartbeat_sent_ = Clock::now();
            next_checkpoint_ = Clock::now() + checkpoint_period();
            if (options_.static_split) { // M1: hand out every piece now, round-robin
                int r = 0;
                while (!pool_.empty()) {
                    issue(r, pool_.front());
                    pool_.pop_front();
                    r = (r + 1) % size_;
                }
            }
        }
        unsigned quiet_loops = 0;
        while (true) {
            bool acted = false;
            int source = 0, tag = 0;
            Words data;
            while (receive(source, tag, data)) {
                acted = true;
                if (tag < TAG_TO_RANKS) {
                    server_receive(source, tag, data);
                } else {
                    client_receive(tag, data);
                }
            }
            if (rank_ == 0) server_step(acted);
            client_step(acted);
            complete_sends();
            if (stopped_ && (rank_ != 0 || stop_acks_ == static_cast<unsigned>(size_))) {
                break;
            }
            if (exiting_ && (rank_ != 0 || exit_acks_ == static_cast<unsigned>(size_))) {
                break;
            }
            watchdog();
            // Poll quickly while something is happening, back off when quiet
            quiet_loops = acted ? 0 : quiet_loops + 1;
            const bool urgent = rank_ == 0 || request_open_ || donation_.active || pause_pending_ || paused_;
            const unsigned max_us = urgent ? 1000 : 5000;
            unsigned us = std::min<unsigned>(max_us, 20u << std::min(quiet_loops, 8u));
            if (options_.stress) us += static_cast<unsigned>(random_below(2000));
            std::this_thread::sleep_for(std::chrono::microseconds(us));
        }
        while (!outgoing_.empty()) {
            complete_sends();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        counters_.elapsed_ns = elapsed_ns(start);
        return exiting_ ? exit_code_ : -1;
    }

    // After run() returned an exit code (rank 0): the statistics collected at the
    // last pause (every rank's work, earlier runs included), and what happened
    const stats::Counters& saved_stats() const { return saved_stats_; }
    const std::string& exit_reason() const { return exit_reason_; }

    // For progress lines (any thread; rank 0's values cover every rank)
    std::uint64_t pool_size() const { return pool_size_.load(std::memory_order_relaxed); }
    // Rank 0: progress over every rank, from each rank's latest Status (at most a second
    // or two old). Fills the progress fields of Counters: supports processed and emitted,
    // candidates, Fermat survivors, certified, failed, unresolved and the largest g(p).
    stats::Counters global_progress() const {
        std::lock_guard<std::mutex> lock(global_mutex_);
        return global_;
    }

    NodeCounters& counters() { return counters_; }

private:
    struct RankView { // Rank 0's view of one rank
        Status status;
        bool known = false;     // A Status has arrived
        std::uint64_t issued = 0; // Tasks sent to it
        bool hungry = false;    // Its request is open
        bool donating = false;  // A DONATE to it is unanswered
        bool acked = false;     // STOP_ACK received
        bool exited = false;    // EXIT_ACK received
        Clock::time_point retry_at{}; // No DONATE before this (after a refusal)
        Clock::time_point last_heard{};
    };
    struct Outgoing {
        Words data;
        MPI_Request request = MPI_REQUEST_NULL;
    };
    struct Donation {
        bool active = false;
        bool posted = false;
        std::vector<unsigned> victims; // Busy workers, most work first
        std::size_t next = 0;
    };

    stealing::Scheduler& scheduler_;
    const std::vector<unsigned>& primes_;
    const mpz_class& bound_;
    const int rank_;
    const int size_;
    const NodeOptions options_;
    producer::detail::DfsStack check_stack_; // Replays received tasks
    std::vector<unsigned> check_support_;
    std::uint64_t rng_;
    const bool trace_; // LPR_MPI_TRACE set: log every message (testing)
    const volatile std::sig_atomic_t* signal_flag_;
    CheckpointWriter writer_;
    NodeCounters counters_;

    // Transport
    std::list<Outgoing> outgoing_;
    std::deque<std::pair<int, Words>> self_inbox_; // Rank 0's messages to itself

    // Client (every rank)
    std::uint64_t received_ = 0;
    bool request_open_ = false;
    Clock::time_point request_opened_{};
    bool want_since_set_ = false;
    Clock::time_point want_since_{};
    bool reported_ = false;
    bool reported_empty_ = false;
    bool reported_busy_ = false; // Last Status had a busy worker (base_depth > 0)
    std::uint64_t reported_received_ = 0;
    Clock::time_point last_status_{};
    Clock::time_point last_from_root_{};
    Donation donation_;
    bool stopped_ = false;
    stats::Counters progress_{}; // Last statistics snapshot (for Status)
    bool signal_reported_ = false;
    bool pause_pending_ = false;  // PAUSE received; pausing once any donation is done
    bool paused_ = false;         // Workers paused, PAUSED sent; waiting for RESUME or EXIT
    Clock::time_point paused_since_{};
    bool exiting_ = false;        // EXIT received: this rank stops after EXIT_ACK
    int exit_code_ = -1;

    // Server (rank 0)
    std::deque<Task> pool_;
    std::vector<RankView> ranks_;
    bool stop_sent_ = false;
    unsigned stop_acks_ = 0;
    Clock::time_point last_heartbeat_sent_{};
    std::atomic<std::uint64_t> pool_size_{0};
    mutable std::mutex global_mutex_; // Guards global_ (read by the monitor thread)
    stats::Counters global_;
    // Checkpoints (rank 0)
    bool collecting_ = false;     // PAUSE sent, waiting for every rank's answer
    bool exit_after_ = false;     // Exit once this checkpoint is done (a signal)
    unsigned replies_ = 0;
    bool pause_failed_ = false;
    std::vector<Task> collected_;
    stats::Counters collected_stats_;
    Clock::time_point collect_started_{};
    Clock::time_point next_checkpoint_{};
    bool exit_sent_ = false;      // EXIT sent to every rank (including this one)
    unsigned exit_acks_ = 0;
    stats::Counters saved_stats_;
    std::string exit_reason_;
    int signal_number_ = 0;
    int signal_rank_ = -1;

    static std::uint64_t elapsed_ns(Clock::time_point since) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
    }

    std::uint64_t random_below(std::uint64_t n) {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return n ? rng_ % n : 0;
    }

    [[noreturn]] void fatal(const std::string& why) {
        lpr::log::warn("MPI: fatal: " + why);
        std::cerr.flush();
        MPI_Abort(MPI_COMM_WORLD, 5);
        std::abort();
    }

    // ---- Transport ----

    void trace(const char* dir, int peer, int tag, const Words& data) {
        if (!trace_) return;
        std::ostringstream out;
        const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count() / 1000.0;
        out << "mpi t=" << std::fixed << std::setprecision(1) << ms << ' ' << dir << ' ' << peer << ' ' << tag_name(tag);
        if (tag <= TAG_STOP_ACK && data.size() >= Status::WORDS) {
            const Status st = Status::read(data);
            out << " received=" << st.received << " regions=" << st.regions << " idle=" << st.idle_workers
                << " base_depth=" << st.base_depth;
        }
        if (rank_ == 0) out << " pool=" << pool_.size();
        lpr::log::info(out.str());
    }

    void send(int dest, int tag, Words data) {
        trace("->", dest, tag, data);
        ++counters_.messages_sent;
        if (dest == rank_) {
            self_inbox_.emplace_back(tag, std::move(data));
            return;
        }
        outgoing_.push_back(Outgoing{std::move(data), MPI_REQUEST_NULL});
        Outgoing& out = outgoing_.back();
        MPI_Isend(out.data.data(), static_cast<int>(out.data.size()), MPI_UINT64_T, dest, tag, MPI_COMM_WORLD,
                  &out.request);
    }

    // Frees the buffers of sends that have completed
    void complete_sends() {
        for (auto it = outgoing_.begin(); it != outgoing_.end();) {
            int done = 0;
            MPI_Test(&it->request, &done, MPI_STATUS_IGNORE);
            it = done ? outgoing_.erase(it) : std::next(it);
        }
    }

    bool receive(int& source, int& tag, Words& data) {
        if (!self_inbox_.empty()) {
            source = rank_;
            tag = self_inbox_.front().first;
            data = std::move(self_inbox_.front().second);
            self_inbox_.pop_front();
            ++counters_.messages_received;
            trace("<-", source, tag, data);
            return true;
        }
        int flag = 0;
        MPI_Status status;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        if (!flag) return false;
        int count = 0;
        MPI_Get_count(&status, MPI_UINT64_T, &count);
        data.resize(static_cast<std::size_t>(count));
        MPI_Recv(data.data(), count, MPI_UINT64_T, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        source = status.MPI_SOURCE;
        tag = status.MPI_TAG;
        ++counters_.messages_received;
        trace("<-", source, tag, data);
        return true;
    }

    // ---- Client (every rank) ----

    Status current_status() {
        Status s;
        s.received = received_;
        s.regions = scheduler_.regions();
        s.idle_workers = scheduler_.idle_workers();
        int best = 0;
        for (unsigned w = 0; w < scheduler_.num_workers(); ++w) {
            const int d = scheduler_.worker_base_depth(w);
            if (d >= 1 && (best == 0 || d < best)) best = d;
        }
        s.base_depth = static_cast<std::uint64_t>(best);
        s.supports_processed = progress_.total_support_count;
        s.supports_emitted = progress_.supports_emitted;
        s.candidates = progress_.candidate_count;
        s.fermat_survivors = progress_.fermat_survivor_count;
        s.certified = progress_.certified_count;
        s.failed = progress_.certified_inequality_fail_count;
        s.unresolved = progress_.unresolved_count;
        s.largest_g = progress_.largest_least_primitive_root;
        return s;
    }

    // Sends a message to rank 0 that starts with this rank's Status
    void send_to_root(int tag, const Task* task = nullptr) {
        const Status s = current_status();
        Words w;
        s.append_to(w);
        if (task) append_task(w, *task);
        send(0, tag, std::move(w));
        reported_ = true;
        reported_empty_ = s.regions == 0;
        reported_busy_ = s.base_depth > 0;
        reported_received_ = s.received;
        last_status_ = Clock::now();
    }

    void client_receive(int tag, const Words& data) {
        last_from_root_ = Clock::now();
        if (stopped_ || exiting_) {
            fatal(std::string("message ") + tag_name(tag) + (stopped_ ? " after STOP" : " after EXIT"));
        }
        switch (tag) {
        case TAG_PAUSE:
            if (pause_pending_ || paused_) fatal("PAUSE while already pausing");
            pause_pending_ = true; // client_step pauses once any donation in progress is done
            break;
        case TAG_RESUME:
            if (paused_) {
                scheduler_.resume();
                counters_.paused_ns += elapsed_ns(paused_since_);
                paused_ = false;
            } // Else this rank's pause had failed: it is running already
            break;
        case TAG_EXIT:
            if (data.size() != 1) fatal("malformed EXIT message");
            exit_code_ = static_cast<int>(data[0]);
            exiting_ = true;
            if (paused_) counters_.paused_ns += elapsed_ns(paused_since_);
            send_to_root(TAG_EXIT_ACK); // The last message from this rank
            break;
        case TAG_TASK: {
            Task task;
            std::string why;
            if (!read_task(data, 0, task)) fatal("malformed TASK message");
            if (!check_stack_.restore(task, primes_, bound_, check_support_, &why)) {
                fatal("received task does not restore: " + why);
            }
            scheduler_.add_task(std::move(task));
            ++received_;
            ++counters_.tasks_received;
            if (request_open_) {
                request_open_ = false;
                counters_.request_wait_ns += elapsed_ns(request_opened_);
            }
            break;
        }
        case TAG_DONATE:
            ++counters_.donations_asked;
            if (donation_.active) fatal("DONATE while a donation is in progress");
            start_donation();
            break;
        case TAG_HEARTBEAT:
            break;
        case TAG_STOP:
            if (scheduler_.regions() != 0 || donation_.active || scheduler_.pending_tasks() != 0
                || pause_pending_ || paused_) {
                fatal("STOP while this rank still holds work or is paused");
            }
            stopped_ = true;
            if (request_open_) {
                request_open_ = false;
                counters_.request_wait_ns += elapsed_ns(request_opened_);
            }
            scheduler_.finish();
            send_to_root(TAG_STOP_ACK);
            break;
        default:
            fatal("unexpected message tag " + std::to_string(tag));
        }
    }

    void start_donation() {
        donation_ = Donation{};
        donation_.active = true;
        std::vector<std::pair<int, unsigned>> busy; // (base depth, worker)
        for (unsigned w = 0; w < scheduler_.num_workers(); ++w) {
            const int d = scheduler_.worker_base_depth(w);
            if (d >= 1) busy.emplace_back(d, w);
        }
        std::sort(busy.begin(), busy.end()); // Shallowest base depth (most work) first
        for (const auto& b : busy) donation_.victims.push_back(b.second);
    }

    // Advances a donation in progress: post requests to victims in turn, send the
    // first region handed over, or refuse once every victim has refused
    void donation_step(bool& acted) {
        if (!donation_.active) return;
        if (donation_.posted) {
            const int state = scheduler_.external_mailbox_state();
            if (state == stealing::detail::EMPTY) return; // Not answered yet
            acted = true;
            donation_.posted = false;
            if (state == stealing::detail::FILLED) {
                const auto& region = scheduler_.external_mailbox();
                Task task = producer::detail::region_task(region);
                std::string why;
                if (!check_stack_.restore(task, primes_, bound_, check_support_, &why)) {
                    fatal("donated region does not restore: " + why);
                }
                for (std::size_t d = 0; d <= task.depth; ++d) {
                    if (!producer::detail::same_values(check_stack_.frame(d), region.frames[d])) {
                        fatal("donated region's frame " + std::to_string(d) + " differs from its replay");
                    }
                }
                scheduler_.reset_external_mailbox();
                scheduler_.region_left(); // Before the Status is taken: the task travels in this message
                send_to_root(TAG_TASK_IN, &task);
                ++counters_.donations_sent;
                donation_ = Donation{};
                return;
            }
            scheduler_.reset_external_mailbox(); // REFUSED: try the next victim
        }
        while (donation_.next < donation_.victims.size()) {
            const unsigned victim = donation_.victims[donation_.next++];
            if (scheduler_.post_external_request(victim)) {
                donation_.posted = true;
                return;
            }
        }
        acted = true;
        ++counters_.donations_refused;
        donation_ = Donation{};
        send_to_root(TAG_NO_DONATION);
    }

    // Pauses this rank's workers and answers PAUSED (or PAUSE_FAILED)
    void pause_workers() {
        pause_pending_ = false;
        const auto result = scheduler_.pause(options_.pause_timeout);
        if (result != stealing::Scheduler::PauseResult::Paused) {
            // TimedOut (SearchFinishing cannot happen: workers wait for finish())
            lpr::log::warn("MPI: checkpoint: this rank's workers did not stop within "
                           + std::to_string(options_.pause_timeout.count() / 1000) + " s");
            send_to_root(TAG_PAUSE_FAILED);
            return;
        }
        paused_ = true;
        paused_since_ = Clock::now();
        stats::publish_thread_local_stats(); // This thread's own counters (e.g. M1's splits)
        const stats::Counters stats = stats::get_master_stats().snapshot(); // Workers published when parking
        std::vector<Task> tasks;
        std::string error;
        if (!scheduler_.remaining_tasks(tasks, error)) {
            lpr::log::warn("MPI: checkpoint: this rank's tasks failed their checks: " + error);
            scheduler_.resume();
            paused_ = false;
            send_to_root(TAG_PAUSE_FAILED);
            return;
        }
        const Status st = current_status();
        Words w;
        st.append_to(w);
        append_tasks(w, tasks);
        const Words c = counters_words(stats);
        w.insert(w.end(), c.begin(), c.end());
        send(0, TAG_PAUSED, std::move(w));
        reported_ = true;
        reported_empty_ = st.regions == 0;
        reported_busy_ = st.base_depth > 0;
        reported_received_ = st.received;
        last_status_ = Clock::now();
    }

    void client_step(bool& acted) {
        if (exiting_ || stopped_) return; // (After STOP a signal changes nothing: the search is complete)
        // A signal: report it once; rank 0 runs the checkpoint and ends the run
        if (signal_flag_ != nullptr && *signal_flag_ != 0 && !signal_reported_) {
            signal_reported_ = true;
            acted = true;
            if (rank_ == 0) {
                server_signal(0, *signal_flag_);
            } else {
                Status st = current_status();
                Words w;
                st.append_to(w);
                w.push_back(static_cast<std::uint64_t>(*signal_flag_));
                send(0, TAG_SIGNAL, std::move(w));
            }
        }
        if (paused_) return; // Nothing until RESUME or EXIT
        donation_step(acted);
        if (pause_pending_) {
            if (!donation_.active) {
                pause_workers();
                acted = true;
            }
            return;
        }
        const auto now = Clock::now();
        const bool periodic = !reported_ || now - last_status_ >= (options_.stress
            ? std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(50))
            : std::chrono::duration_cast<Clock::duration>(STATUS_INTERVAL));
        if (periodic) {
            progress_ = stats::get_master_stats().snapshot();
        }
        const unsigned regions = scheduler_.regions();
        // L1: ask rank 0 once enough workers have been idle for a moment, or at
        // once if this rank holds nothing. Not while a received task is still
        // pending here, and never in M1.
        const bool starving = regions == 0
            || (scheduler_.idle_workers() >= options_.request_idle && scheduler_.pending_tasks() == 0);
        if (!starving || request_open_) {
            want_since_set_ = false;
        } else if (!want_since_set_) {
            want_since_set_ = true;
            want_since_ = now;
        }
        const bool debounced = regions == 0 || options_.stress || now - want_since_ >= REQUEST_DEBOUNCE;
        if (!options_.static_split && !request_open_ && want_since_set_ && debounced && !donation_.active) {
            send_to_root(TAG_REQUEST);
            request_open_ = true;
            request_opened_ = now;
            want_since_set_ = false;
            ++counters_.requests_sent;
            acted = true;
            return;
        }
        // Rank 0 must learn when this rank empties (termination), what it has
        // received, and when it first has a busy worker (it can donate); plus a
        // periodic Status with progress
        bool busy = false;
        for (unsigned w = 0; w < scheduler_.num_workers() && !busy; ++w) {
            busy = scheduler_.worker_base_depth(w) >= 1;
        }
        if (periodic || (regions == 0) != reported_empty_ || received_ != reported_received_ || busy != reported_busy_) {
            send_to_root(TAG_STATUS);
        }
    }

    void watchdog() {
        const auto now = Clock::now();
        if (rank_ != 0 && !stopped_ && now - last_from_root_ > WATCHDOG) {
            fatal("no message from rank 0 for " + std::to_string(WATCHDOG.count()) + " s");
        }
        if (rank_ == 0) {
            for (int r = 0; r < size_; ++r) {
                const RankView& v = ranks_[static_cast<std::size_t>(r)];
                // (A paused rank sends nothing until RESUME, which follows promptly once
                // every rank has answered; each answer comes within the pause timeout.)
                if (!v.acked && !v.exited && now - v.last_heard > WATCHDOG) {
                    fatal("no message from rank " + std::to_string(r) + " for " + std::to_string(WATCHDOG.count())
                          + " s");
                }
            }
        }
    }

    // ---- Server (rank 0) ----

    void issue(int r, const Task& task) {
        Words w;
        append_task(w, task);
        send(r, TAG_TASK, std::move(w));
        RankView& v = ranks_[static_cast<std::size_t>(r)];
        ++v.issued;
        v.hungry = false;
        ++counters_.tasks_issued;
    }

    void server_receive(int source, int tag, const Words& data) {
        if (rank_ != 0) fatal(std::string("rank ") + std::to_string(rank_) + " received " + tag_name(tag));
        if (data.size() < Status::WORDS) fatal("message without a status");
        RankView& v = ranks_[static_cast<std::size_t>(source)];
        v.status = Status::read(data);
        v.known = true;
        v.last_heard = Clock::now();
        {
            stats::Counters total;
            for (const RankView& view : ranks_) {
                const Status& st = view.status;
                total.total_support_count += st.supports_processed;
                total.supports_emitted += st.supports_emitted;
                total.candidate_count += st.candidates;
                total.fermat_survivor_count += st.fermat_survivors;
                total.certified_count += st.certified;
                total.certified_inequality_fail_count += st.failed;
                total.unresolved_count += st.unresolved;
                total.largest_least_primitive_root = std::max(total.largest_least_primitive_root, st.largest_g);
            }
            std::lock_guard<std::mutex> lock(global_mutex_);
            global_ = total;
        }
        if (v.status.received > v.issued) fatal("rank " + std::to_string(source) + " received more than issued");
        switch (tag) {
        case TAG_REQUEST:
            if (!stop_sent_) v.hungry = true;
            break;
        case TAG_STATUS:
            break;
        case TAG_TASK_IN: {
            if (!v.donating) fatal("unrequested donation from rank " + std::to_string(source));
            v.donating = false;
            Task task;
            std::string why;
            if (!read_task(data, Status::WORDS, task)) fatal("malformed TASK_IN message");
            if (!check_stack_.restore(task, primes_, bound_, check_support_, &why)) {
                fatal("donated task does not restore: " + why);
            }
            pool_.push_back(std::move(task));
            break;
        }
        case TAG_NO_DONATION:
            if (!v.donating) fatal("unrequested refusal from rank " + std::to_string(source));
            v.donating = false;
            v.retry_at = Clock::now() + (options_.stress ? Clock::duration::zero()
                                                          : std::chrono::duration_cast<Clock::duration>(DONOR_RETRY));
            break;
        case TAG_STOP_ACK:
            if (!stop_sent_ || v.acked) fatal("unexpected STOP_ACK from rank " + std::to_string(source));
            v.acked = true;
            ++stop_acks_;
            break;
        case TAG_SIGNAL:
            if (data.size() != Status::WORDS + 1) fatal("malformed SIGNAL message");
            server_signal(source, static_cast<int>(data[Status::WORDS]));
            break;
        case TAG_PAUSED: {
            if (!collecting_) fatal("unexpected PAUSED from rank " + std::to_string(source));
            // Every task issued to it arrived before PAUSE, and any donation it made
            // arrived before PAUSED (messages between two ranks keep their order)
            if (v.status.received != v.issued || v.donating) {
                fatal("rank " + std::to_string(source) + " paused with work in transit");
            }
            std::size_t offset = Status::WORDS;
            std::vector<Task> tasks;
            if (!read_tasks(data, offset, tasks)) fatal("malformed PAUSED message (tasks)");
            const std::size_t fields = counters_words(stats::Counters{}).size();
            if (data.size() - offset != fields) fatal("malformed PAUSED message (statistics)");
            collected_stats_.merge(counters_from(data.data() + offset));
            for (Task& t : tasks) collected_.push_back(std::move(t));
            ++replies_;
            break;
        }
        case TAG_PAUSE_FAILED:
            if (!collecting_) fatal("unexpected PAUSE_FAILED from rank " + std::to_string(source));
            pause_failed_ = true;
            ++replies_;
            break;
        case TAG_EXIT_ACK:
            if (!exit_sent_ || v.exited) fatal("unexpected EXIT_ACK from rank " + std::to_string(source));
            v.exited = true;
            ++exit_acks_;
            break;
        default:
            fatal("unexpected message tag " + std::to_string(tag));
        }
    }

    // True if rank r certainly holds no region (see the header comment)
    bool holds_nothing(const RankView& v) const {
        return v.known && v.status.regions == 0 && v.status.received == v.issued && !v.donating;
    }

    // The busy rank to ask for a donation, or -1. Prefers ranks without an open
    // request, then the shallowest base depth (most work), then fewer idle workers.
    int pick_donor(bool allow_hungry) {
        const auto now = Clock::now();
        int best = -1;
        // Base depth 0 (no busy worker when it last reported, e.g. a task just
        // arrived) sorts last: it may refuse, and is then retried after DONOR_RETRY
        auto key = [&](const RankView& v) {
            const std::uint64_t depth = v.status.base_depth == 0 ? parameters::MAX_OMEGA + 1 : v.status.base_depth;
            return std::make_tuple(v.hungry ? 1 : 0, depth, v.status.idle_workers);
        };
        for (int r = 0; r < size_; ++r) {
            const RankView& v = ranks_[static_cast<std::size_t>(r)];
            if (!v.known || v.donating || v.status.regions == 0 || now < v.retry_at) {
                continue;
            }
            if (v.hungry && !allow_hungry) continue;
            if (best < 0 || key(v) < key(ranks_[static_cast<std::size_t>(best)])) best = r;
        }
        return best;
    }

    Clock::duration checkpoint_period() const {
        const double seconds = options_.checkpoint_interval > 0 ? options_.checkpoint_interval : 1e9;
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }

    // Rank 0: a rank (or rank 0 itself) received a signal
    void server_signal(int source, int sig) {
        if (exit_sent_) return;
        if (signal_rank_ < 0) {
            signal_rank_ = source;
            signal_number_ = sig;
        }
        if (stop_sent_) {
            lpr::log::info("MPI: signal " + std::to_string(sig) + " on rank " + std::to_string(source)
                           + " while the search is finishing: completing normally");
            return;
        }
        lpr::log::info("MPI: signal " + std::to_string(sig) + " on rank " + std::to_string(source)
                       + ": " + (options_.checkpoints ? "checkpointing and stopping" : "stopping (no checkpoints)"));
        exit_after_ = true;
        if (!collecting_) start_collecting();
    }

    // Rank 0: pause every rank and collect its tasks and statistics
    void start_collecting() {
        collecting_ = true;
        replies_ = 0;
        pause_failed_ = false;
        collected_.clear();
        collected_stats_ = stats::Counters{};
        collect_started_ = Clock::now();
        for (int r = 0; r < size_; ++r) send(r, TAG_PAUSE, {});
    }

    // Rank 0: every rank has answered PAUSE. Writes the checkpoint (if enabled and
    // the collection is complete), then sends RESUME or EXIT.
    void finish_collecting() {
        collecting_ = false;
        std::vector<Task> tasks = collected_;
        tasks.insert(tasks.end(), pool_.begin(), pool_.end());
        bool written = false;
        std::string error;
        bool consistent = !pause_failed_;
        if (pause_failed_) {
            error = "a rank's workers did not stop in time, or its tasks failed their checks";
        } else {
            for (int r = 0; r < size_ && consistent; ++r) {
                const RankView& v = ranks_[static_cast<std::size_t>(r)];
                if (v.status.received != v.issued || v.donating) {
                    consistent = false;
                    error = "rank " + std::to_string(r) + " has work in transit";
                }
            }
            if (consistent && !producer::detail::tasks_disjoint(tasks, &error)) {
                consistent = false;
                error = "tasks overlap: " + error;
            }
        }
        const double pause_s = std::chrono::duration<double>(Clock::now() - collect_started_).count();
        if (consistent && tasks.empty()) {
            // Nothing is left: the search is finishing (as Scheduler's SearchFinishing)
            lpr::log::info("MPI: checkpoint skipped: no work is left, the search is finishing");
            exit_after_ = false;
        } else if (consistent && options_.checkpoints) {
            written = writer_ && writer_(tasks, collected_stats_, error);
            if (written) {
                ++counters_.checkpoints_written;
                lpr::log::info("MPI: checkpoint written: " + std::to_string(tasks.size()) + " tasks, "
                               + std::to_string(collected_stats_.total_support_count) + " supports processed ("
                               + std::to_string(pause_s) + " s paused)");
            }
        }
        if (!written && options_.checkpoints && !(consistent && tasks.empty())) {
            ++counters_.checkpoints_failed;
            lpr::log::warn("MPI: checkpoint not written: " + error);
        }
        if (exit_after_) {
            saved_stats_ = collected_stats_;
            exit_reason_ = written ? "checkpoint written" : (options_.checkpoints ? "checkpoint NOT written: " + error
                                                                                  : "no checkpoint (--checkpoint not given)");
            if (pause_failed_) exit_reason_ += "; the statistics may be incomplete";
            exit_sent_ = true; // This rank's own copy of EXIT ends its client side too
            for (int r = 0; r < size_; ++r) send(r, TAG_EXIT, {static_cast<std::uint64_t>(written ? 4 : 3)});
        } else {
            for (int r = 0; r < size_; ++r) send(r, TAG_RESUME, {});
        }
        next_checkpoint_ = Clock::now() + checkpoint_period();
    }

    void server_step(bool& acted) {
        const auto now = Clock::now();
        if (now - last_heartbeat_sent_ >= HEARTBEAT_TO_RANKS && !stop_sent_ && !exit_sent_) {
            for (int r = 1; r < size_; ++r) send(r, TAG_HEARTBEAT, {});
            last_heartbeat_sent_ = now;
        }
        if (stop_sent_ || exit_sent_) return;
        // Checkpoints: while collecting, nothing moves (no tasks issued, no donations
        // asked); every rank's answer completes it
        if (collecting_) {
            if (replies_ == static_cast<unsigned>(size_)) {
                finish_collecting();
                acted = true;
            }
            return;
        }
        if (options_.checkpoints && options_.checkpoint_interval > 0 && now >= next_checkpoint_) {
            start_collecting();
            acted = true;
            return;
        }
        // Serve open requests: ranks holding nothing first, then most idle workers
        while (!pool_.empty()) {
            int pick = -1;
            for (int r = 0; r < size_; ++r) {
                const RankView& v = ranks_[static_cast<std::size_t>(r)];
                if (!v.hungry) continue;
                if (pick < 0) { pick = r; continue; }
                const RankView& p = ranks_[static_cast<std::size_t>(pick)];
                const auto k = std::make_tuple(v.status.regions == 0 ? 0 : 1, -static_cast<long long>(v.status.idle_workers));
                const auto kp = std::make_tuple(p.status.regions == 0 ? 0 : 1, -static_cast<long long>(p.status.idle_workers));
                if (k < kp) pick = r;
            }
            if (pick < 0) break;
            issue(pick, pool_.front());
            pool_.pop_front();
            acted = true;
        }
        // L2: refill the pool by donations
        if (!options_.static_split) {
            long long hungry = 0, donating = 0;
            bool someone_empty = false;
            for (const RankView& v : ranks_) {
                hungry += v.hungry ? 1 : 0;
                donating += v.donating ? 1 : 0;
                if (v.hungry && v.status.regions == 0) someone_empty = true;
            }
            // (With one rank a reserve would only move work out of the rank and back)
            const long long reserve = options_.stress ? 2LL * size_
                                    : size_ == 1 ? 0 : static_cast<long long>(options_.pool_reserve);
            long long wanted = hungry + reserve - static_cast<long long>(pool_.size()) - donating;
            while (wanted-- > 0) {
                const int donor = pick_donor(someone_empty);
                if (donor < 0) break;
                ranks_[static_cast<std::size_t>(donor)].donating = true;
                send(donor, TAG_DONATE, {});
                ++counters_.donations_requested;
                acted = true;
            }
        }
        counters_.pool_max = std::max<std::uint64_t>(counters_.pool_max, pool_.size());
        pool_size_.store(pool_.size(), std::memory_order_relaxed);
        // Termination: nothing in the pool, in flight, or on any rank
        if (pool_.empty() && std::all_of(ranks_.begin(), ranks_.end(),
                                         [this](const RankView& v) { return holds_nothing(v); })) {
            for (int r = 0; r < size_; ++r) send(r, TAG_STOP, {});
            stop_sent_ = true;
            acted = true;
        }
    }
};

/*
 * Collective, at the end: merges every rank's Counters (Counters::merge: sums,
 * maxima, the wrapping checksum) into the result on rank 0 (other ranks get
 * their own counters back), and gathers the NodeCounters rows.
 */
inline stats::Counters reduce_counters(const stats::Counters& mine, int rank, int size) {
    const Words w = counters_words(mine);
    std::vector<std::uint64_t> all(rank == 0 ? w.size() * static_cast<std::size_t>(size) : 0);
    MPI_Gather(w.data(), static_cast<int>(w.size()), MPI_UINT64_T, all.data(), static_cast<int>(w.size()),
               MPI_UINT64_T, 0, MPI_COMM_WORLD);
    if (rank != 0) return mine;
    stats::Counters total;
    for (int r = 0; r < size; ++r) {
        total.merge(counters_from(all.data() + static_cast<std::size_t>(r) * w.size()));
    }
    return total;
}

inline std::vector<NodeCounters> gather_node_counters(const NodeCounters& mine, int rank, int size) {
    const Words w = mine.words();
    std::vector<std::uint64_t> all(rank == 0 ? w.size() * static_cast<std::size_t>(size) : 0);
    MPI_Gather(w.data(), static_cast<int>(w.size()), MPI_UINT64_T, all.data(), static_cast<int>(w.size()),
               MPI_UINT64_T, 0, MPI_COMM_WORLD);
    std::vector<NodeCounters> rows;
    if (rank == 0) {
        for (int r = 0; r < size; ++r) rows.push_back(NodeCounters::from(all.data() + static_cast<std::size_t>(r) * w.size()));
    }
    return rows;
}

// The MPI summary (rank 0, stdout)
inline void print_summary(const std::vector<NodeCounters>& rows, const std::vector<std::string>& hosts) {
    std::cout << "MPI summary:\n"
              << "------------\n"
              << "rank  host                 supports   emitted    idle_s  tasks_in  requests  wait_s  "
                 "donated  refused  asked  msgs_out  msgs_in\n";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const NodeCounters& c = rows[r];
        char line[512];
        std::snprintf(line, sizeof line,
                      "%4zu  %-18.18s %11llu %9llu %9.2f %9llu %9llu %7.3f %8llu %8llu %6llu %9llu %8llu\n",
                      r, r < hosts.size() ? hosts[r].c_str() : "?",
                      static_cast<unsigned long long>(c.supports_processed),
                      static_cast<unsigned long long>(c.supports_emitted), c.worker_idle_ns / 1e9,
                      static_cast<unsigned long long>(c.tasks_received),
                      static_cast<unsigned long long>(c.requests_sent), c.request_wait_ns / 1e9,
                      static_cast<unsigned long long>(c.donations_sent),
                      static_cast<unsigned long long>(c.donations_refused),
                      static_cast<unsigned long long>(c.donations_asked),
                      static_cast<unsigned long long>(c.messages_sent),
                      static_cast<unsigned long long>(c.messages_received));
        std::cout << line;
    }
    if (!rows.empty()) {
        const NodeCounters& r0 = rows[0];
        std::cout << "Rank 0 pool: " << r0.tasks_issued << " tasks issued, " << r0.donations_requested
                  << " donations requested, largest pool " << r0.pool_max << '\n';
        double paused = 0;
        for (const NodeCounters& c : rows) paused = std::max(paused, c.paused_ns / 1e9);
        std::cout << "Checkpoints: " << r0.checkpoints_written << " written, " << r0.checkpoints_failed
                  << " not written; workers paused " << paused << " s in total (longest rank)\n";
    }
}

} // namespace lpr::mpi

#endif // LPR_MPI
