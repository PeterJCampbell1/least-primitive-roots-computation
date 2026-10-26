#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"
#include "corollary4.hpp"
#include "generate_supports.hpp"
#include "work_stealing.hpp"
#include "checkpoint.hpp"
#include "process_support.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "trial_division.hpp"
#include "logging.hpp"
#include "mpi_comm.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <gmpxx.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <cassert>
#include <chrono>
#include <csignal>
#include <functional>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h> // gethostname, access, unlink

#ifndef LPR_GIT_COMMIT
#define LPR_GIT_COMMIT "unknown"
#endif

// Anonymous namespace - restricts type visibility to this file
namespace {

constexpr int EXIT_INTERRUPTED = 3;
constexpr int EXIT_CHECKPOINTED = 4; // Interrupted after writing a checkpoint
constexpr auto PAUSE_TIMEOUT = std::chrono::seconds(120); // Workers must stop for a checkpoint within this

// Generate all primes <= limit using the Sieve of Eratosthenes.
// 64-bit indices so that limits close to UINT_MAX do not overflow.
std::vector<unsigned> generate_primes(unsigned limit)
{
    const std::uint64_t n = limit;
    std::vector<bool> is_prime(n + 1, true);

    is_prime[0] = false;
    if (n >= 1) is_prime[1] = false;

    for (std::uint64_t p = 2; p * p <= n; ++p) {
        if (!is_prime[p]) {
            continue;
        }

        for (std::uint64_t multiple = p * p;
             multiple <= n;
             multiple += p) {
            is_prime[multiple] = false;
        }
    }

    std::vector<unsigned> primes;

    for (std::uint64_t p = 2; p <= n; ++p) {
        if (is_prime[p]) {
            primes.push_back(static_cast<unsigned>(p));
        }
    }

    return primes;
} // generate_primes

// Set by the signal handler, polled by the monitor thread
volatile std::sig_atomic_t g_signal = 0;

void on_signal(int sig) {
    g_signal = sig;
}

std::string local_time_string() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S %Z");
    return out.str();
}

std::string hostname() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) {
        return "unknown";
    }
    return name;
}

// One line of key=value progress fields from a statistics snapshot. `mode_fields`
// are the mode's live fields (queue length, or workers holding a region).
std::string progress_line(
    const lpr::stats::Counters& s,
    double supports_per_second,
    const std::string& mode_fields,
    const std::string& eta_fields = ""
) {
    std::ostringstream out;
    out << "progress:"
        << " supports_emitted=" << s.supports_emitted
        << " supports_processed=" << s.total_support_count
        << " (" << std::fixed << std::setprecision(1) << supports_per_second << "/s)"
        << " candidates=" << s.candidate_count
        << " fermat_survivors=" << s.fermat_survivor_count
        << " certified=" << s.certified_count
        << " failed=" << s.certified_inequality_fail_count
        << " unresolved=" << s.unresolved_count
        << " largest_g=" << s.largest_least_primitive_root
        << ' ' << mode_fields
        << " coprocessed=" << s.batches_coprocessed
        << " consumer_waits=" << s.consumer_waits
        << " consumer_wait_s=" << std::setprecision(1) << s.consumer_wait_ns / 1e9
        << " regions_split=" << s.regions_split
        << " idle_s=" << std::setprecision(1) << s.worker_idle_ns / 1e9
        << " position=" << lpr::producer::dfs_position_string()
        << eta_fields;
    return out.str();
}

// "1d 02h", "3h 25m", "12m 05s" or "40s"
std::string duration_string(double seconds) {
    const auto s = static_cast<long long>(seconds + 0.5);
    char buf[64];
    if (s >= 86400) std::snprintf(buf, sizeof buf, "%lldd %02lldh", s / 86400, (s % 86400) / 3600);
    else if (s >= 3600) std::snprintf(buf, sizeof buf, "%lldh %02lldm", s / 3600, (s % 3600) / 60);
    else if (s >= 60) std::snprintf(buf, sizeof buf, "%lldm %02llds", s / 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%llds", s);
    return buf;
}

/*
 * Candidate-based rate and ETA fields for the progress line: the average number
 * of candidates per second since this run started (steadier than supports per
 * second under work stealing, which does cheap supports first; see
 * docs/work_stealing.md section 8) and, if the expected total is known
 * (--expected-candidates), the percentage done and the time remaining.
 */
std::string eta_fields(const lpr::stats::Counters& s, std::uint64_t start_candidates, double seconds,
                       std::uint64_t expected_candidates) {
    std::ostringstream out;
    const double done = static_cast<double>(s.candidate_count - start_candidates);
    const double rate = seconds > 0 ? done / seconds : 0.0;
    out << std::fixed << std::setprecision(0) << " candidates_per_s=" << rate;
    if (expected_candidates > 0) {
        const double total = static_cast<double>(expected_candidates);
        const double have = static_cast<double>(s.candidate_count);
        out << std::setprecision(1) << " done=" << 100.0 * have / total << '%';
        if (have >= total) {
            out << " eta=past_expected_total";
        } else if (rate > 0) {
            out << " eta=" << duration_string((total - have) / rate);
        }
    }
    return out.str();
}

// The result of an attempt to write a checkpoint (see main's take_checkpoint)
struct CheckpointOutcome {
    enum Kind { Written, SearchFinishing, Failed } kind = Failed;
    lpr::stats::Counters stats; // Written: the statistics saved in the checkpoint
    std::string message;        // Failed: why
};

/*
 * Background thread that logs progress every log_interval seconds (if non-zero),
 * writes a checkpoint every checkpoint_interval seconds (if `checkpoint` is set
 * and the interval is non-zero) and handles SIGTERM/SIGINT. It wakes at least
 * once per second. On a signal it writes a checkpoint (if enabled; the workers
 * stay stopped), prints the statistics so far (stdout and log) and exits with
 * EXIT_CHECKPOINTED, or with EXIT_INTERRUPTED if there is no checkpoint. If the
 * search is already finishing it lets the run complete normally instead.
 */
class Monitor {
public:
    using CheckpointFn = std::function<CheckpointOutcome(bool stay_paused)>;

    Monitor(
        unsigned log_interval,
        std::chrono::steady_clock::time_point start,
        std::function<std::string()> mode_fields,
        CheckpointFn checkpoint = {},
        double checkpoint_interval = 0,
        std::string checkpoint_path = "",
        std::uint64_t expected_candidates = 0,
        std::function<lpr::stats::Counters()> eta_source = {}
    ) : log_interval_(log_interval), start_(start), mode_fields_(std::move(mode_fields)),
        checkpoint_(std::move(checkpoint)), checkpoint_interval_(checkpoint_interval),
        checkpoint_path_(std::move(checkpoint_path)), expected_candidates_(expected_candidates),
        eta_source_(std::move(eta_source)),
        start_candidates_(lpr::stats::get_master_stats().snapshot().candidate_count) {
        thread_ = std::thread([this]() { run(); });
    }

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    ~Monitor() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    unsigned log_interval_;
    std::chrono::steady_clock::time_point start_;
    std::function<std::string()> mode_fields_;
    CheckpointFn checkpoint_;
    double checkpoint_interval_;
    std::string checkpoint_path_;
    std::uint64_t expected_candidates_;
    // Counters for the candidate rate and ETA (default: this process's statistics; under
    // MPI, rank 0 passes the totals over every rank)
    std::function<lpr::stats::Counters()> eta_source_;
    std::uint64_t start_candidates_; // Candidates done in earlier runs (resumed), excluded from the rate
    bool ignore_signals_ = false; // A signal arrived while the search was finishing
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;

    void run() {
        using clock = std::chrono::steady_clock;
        auto last_log = clock::now();
        // Resumed runs start with the earlier runs' statistics
        std::uint64_t last_processed = lpr::stats::get_master_stats().snapshot().total_support_count;
        const bool periodic = checkpoint_ && checkpoint_interval_ > 0;
        const auto checkpoint_period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(periodic ? checkpoint_interval_ : 1.0));
        const auto wake_period = std::min<clock::duration>(std::chrono::seconds(1), checkpoint_period);
        auto next_checkpoint = clock::now() + checkpoint_period;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (cv_.wait_for(lock, wake_period, [this]() { return stop_; })) {
                    return;
                }
            }

            if (g_signal != 0 && !ignore_signals_) {
                handle_interrupt();
            }

            if (periodic && clock::now() >= next_checkpoint) {
                const CheckpointOutcome outcome = checkpoint_(false);
                if (outcome.kind == CheckpointOutcome::Written) {
                    lpr::log::info("checkpoint: written to " + checkpoint_path_ + " ("
                                   + std::to_string(outcome.stats.total_support_count) + " supports processed)");
                } else if (outcome.kind == CheckpointOutcome::Failed) {
                    lpr::log::warn("checkpoint not written: " + outcome.message);
                }
                next_checkpoint = clock::now() + checkpoint_period;
            }

            const auto now = clock::now();
            if (log_interval_ > 0 && now - last_log >= std::chrono::seconds(log_interval_)) {
                const lpr::stats::Counters snapshot = lpr::stats::get_master_stats().snapshot();
                const double seconds = std::chrono::duration<double>(now - last_log).count();
                const double rate = (snapshot.total_support_count - last_processed) / seconds;
                const double since_start = std::chrono::duration<double>(now - start_).count();
                const lpr::stats::Counters eta_counts = eta_source_ ? eta_source_() : snapshot;
                lpr::log::info(progress_line(snapshot, rate, mode_fields_(),
                                             eta_fields(eta_counts, start_candidates_, since_start, expected_candidates_)));
                last_processed = snapshot.total_support_count;
                last_log = now;
            }
        }
    }

    // Exits the process, unless the search is already finishing (then it returns)
    void handle_interrupt() {
        const int sig = g_signal;
        lpr::stats::Counters snapshot;
        int exit_code = EXIT_INTERRUPTED;
        std::string checkpoint_note = "no checkpoint was written";
        if (checkpoint_) {
            const CheckpointOutcome outcome = checkpoint_(true); // Workers stay stopped until exit
            if (outcome.kind == CheckpointOutcome::SearchFinishing) {
                lpr::log::info("signal " + std::to_string(sig)
                               + " received while the search is finishing: completing normally");
                ignore_signals_ = true;
                return;
            }
            if (outcome.kind == CheckpointOutcome::Written) {
                snapshot = outcome.stats; // Exactly the work saved in the checkpoint
                exit_code = EXIT_CHECKPOINTED;
                checkpoint_note = "checkpoint written to " + checkpoint_path_ + "; continue with --resume "
                                  + checkpoint_path_;
            } else {
                lpr::log::warn("checkpoint not written: " + outcome.message);
                snapshot = lpr::stats::get_master_stats().snapshot();
            }
        } else {
            snapshot = lpr::stats::get_master_stats().snapshot();
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();

        const std::string notice = "INTERRUPTED by signal " + std::to_string(sig)
            + " after " + std::to_string(elapsed) + " s: partial run, nothing has been verified ("
            + checkpoint_note + ")";
        lpr::log::info(notice);
        lpr::log::info(progress_line(snapshot, 0.0, mode_fields_()));

        std::cout << '\n' << notice << '\n'
                  << "DFS position: " << lpr::producer::dfs_position_string() << "\n\n";
        snapshot.print();
        std::cout << '\n';
        snapshot.print_producer();
        std::cout << "Elapsed time: " << elapsed << " s\n" << notice << '\n';
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(exit_code);
    }
};

} // namespace


int main(int argc, char* argv[])
{
    using lpr::cli::RunMode;

#ifdef LPR_MPI
    // Before anything else (MPI may change argv). Only rank 0 prints to stdout;
    // warnings and logs from every rank go to stderr, tagged with the rank.
    lpr::mpi::Session mpi_session(argc, argv);
    const int mpi_rank = mpi_session.rank();
    const int mpi_size = mpi_session.size();
    if (mpi_rank != 0) {
        std::cout.setstate(std::ios::failbit);
    }
#else
    constexpr int mpi_rank = 0;
    constexpr int mpi_size = 1;
#endif

    // First of all, so a signal during startup is not fatal: it is recorded and
    // handled by the monitor once the search starts (a checkpoint, if enabled)
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    const lpr::cli::ParseResult parsed = lpr::cli::parse(argc, argv);
    if (!parsed.options) {
        std::cerr << "Error: " << parsed.error << "\n\n" << lpr::cli::usage(argv[0]);
        return 2;
    }
    const lpr::cli::Options options = *parsed.options;
    if (options.help) {
        std::cout << lpr::cli::usage(argv[0]);
        return 0;
    }
    if (mpi_size > 1 && !options.mpi) {
        std::cerr << "Error: running as " << mpi_size << " MPI ranks: pass --mpi (otherwise every rank would "
                  << "run the whole search)\n";
        return 2;
    }
    if (options.mpi) {
        lpr::log::set_tag("rank " + std::to_string(mpi_rank) + ": ");
    }
    // Under MPI each rank writes its own log (ranks on different nodes appending to one
    // file on a shared filesystem can interleave or lose lines): rank 0 the given path,
    // rank r the path plus ".rank<r>"
    std::string log_path = options.log_file;
    if (options.mpi && mpi_rank != 0 && !log_path.empty()) {
        log_path += ".rank" + std::to_string(mpi_rank);
    }
    if (!log_path.empty() && !lpr::log::open_file(log_path)) {
        std::cerr << "Error: cannot open log file '" << log_path << "'\n";
        return 2;
    }

    // Resolve omega and s (command line over config file over defaults)
    lpr::config::Overrides file_overrides;
    if (!options.config_file.empty()) {
        const std::string error = lpr::config::read_config_file(options.config_file, file_overrides);
        if (!error.empty()) {
            std::cerr << "Error: " << error << '\n';
            return 2;
        }
    }
    lpr::config::SearchConfig search;
    {
        const std::string error = lpr::config::resolve(
            file_overrides, options.config_file, options.search, search
        );
        if (!error.empty()) {
            std::cerr << "Error: " << error << '\n';
            return 2;
        }
    }
    lpr::config::set_search_config(search); // Read-only from here on

    // The power tree used by the least primitive root search must have a valid
    // heap layout for omega - 1 leaves (it does for every omega with the
    // complete-tree split; this guards against regressions)
    if (!lpr::primality::testing::power_tree_layout_is_valid(search.omega - 1)) {
        std::cerr << "Error: invalid power tree layout for omega = " << search.omega << '\n';
        return 2;
    }

    // Checkpoints save every statistic through Counters::for_each_field
    if (lpr::stats::Counters::field_bytes() != sizeof(lpr::stats::Counters)) {
        std::cerr << "Error: Counters::for_each_field does not list every counter\n";
        return 2;
    }

    // Trial division table (read-only from here on)
    {
        constexpr unsigned min_prime = parameters::TRIAL_DIVISION_MIN_PRIME;
        constexpr unsigned max_prime = parameters::TRIAL_DIVISION_MAX_PRIME;
        if (min_prime < 3 || min_prime > max_prime) {
            std::cerr << "Error: trial division needs 3 <= TRIAL_DIVISION_MIN_PRIME <= "
                      << "TRIAL_DIVISION_MAX_PRIME (got " << min_prime << ", " << max_prime << ")\n";
            return 2;
        }
        lpr::trial_division::Table table = lpr::trial_division::build(min_prime, max_prime);
        if (table.chunks.size() > parameters::MAX_TRIAL_DIVISION_CHUNKS) {
            std::cerr << "Error: trial division up to " << max_prime << " needs " << table.chunks.size()
                      << " chunks, more than MAX_TRIAL_DIVISION_CHUNKS ("
                      << parameters::MAX_TRIAL_DIVISION_CHUNKS << ")\n";
            return 2;
        }
        if (!lpr::trial_division::verify(table)) {
            std::cerr << "Error: trial division table failed verification\n";
            return 2;
        }
        lpr::trial_division::set_table(std::move(table));
    }
    const std::size_t OMEGA = search.omega;
    if (options.steal_max_prefix > OMEGA) {
        std::cerr << "Error: --steal-max-prefix must be between 1 and omega = " << OMEGA
                  << " (got " << options.steal_max_prefix << ")\n";
        return 2;
    }
    const std::size_t steal_max_prefix = options.steal_max_prefix > 0 ? options.steal_max_prefix : OMEGA;

    const RunMode mode = options.mode;
    switch (mode) {
    case RunMode::Single:
        std::cout << "Running single-threaded computation" << '\n' << '\n';
        break;
    case RunMode::Parallel:
        std::cout << "Running multi-threaded computation (work stealing)" << '\n'
                  << "Using: " << options.threads << " threads" << '\n' << '\n';
        break;
    case RunMode::ParallelQueue:
        std::cout << "Running multi-threaded computation (1 producer + consumers)" << '\n'
                  << "Using: " << options.threads << " threads" << '\n' << '\n';
        break;
    case RunMode::DryRun:
        std::cout << "DRY RUN: no primes were tested or verified" << '\n' << '\n';
        break;
    }

    // The global bound needs the first omega primes: sieve a small range first
    std::vector<unsigned> first_primes;
    for (unsigned small_limit = 256; first_primes.size() < OMEGA; small_limit *= 2) {
        first_primes = generate_primes(small_limit);
    }

    // Largest Q*d (p = Q*d + 1) not covered by Corollary 4 for any support.
    // Every p with omega(p-1) = omega and p - 1 > bound satisfies Grosswald by Cor 4.
    mpz_class bound;
    corollary4_global_bound(first_primes, bound);

    // If r is the largest prime of a support with product Q <= B, then
    // p_(omega-1)# * r <= Q <= B, so r <= floor(B / p_(omega-1)#). Generating every
    // prime up to this limit makes the support enumeration exhaustive.
    mpz_class smallest_product = 1; // p_(omega-1)#
    for (std::size_t i = 0; i < OMEGA - 1; ++i) {
        smallest_product *= first_primes[i];
    }
    const mpz_class required_prime_limit = bound / smallest_product;

    // Every emitted support has Q <= B, and jobs store Q in MAX_Q_LIMBS limbs
    if (mpz_size(bound.get_mpz_t()) > parameters::MAX_Q_LIMBS) {
        std::cerr << "Error: the search bound B needs " << mpz_size(bound.get_mpz_t())
                  << " limbs, more than MAX_Q_LIMBS (" << parameters::MAX_Q_LIMBS << ")\n";
        return 2;
    }
    if (required_prime_limit > parameters::MAX_PRIME_LIMIT) {
        std::cerr << "Error: the prime limit floor(B / p_(omega-1)#) = " << required_prime_limit
                  << " for omega = " << OMEGA << ", s = " << search.s
                  << " exceeds MAX_PRIME_LIMIT (" << parameters::MAX_PRIME_LIMIT
                  << ") in src/parameters.hpp.\n";
        return 2;
    }
    // Also cover the first omega primes (needed when no support is possible)
    const unsigned prime_limit = std::max<unsigned>(
        static_cast<unsigned>(required_prime_limit.get_ui()), first_primes[OMEGA - 1]
    );

    std::vector<unsigned> primes =
        generate_primes(prime_limit);

    // Validation: the list must contain the first omega primes (the global bound
    // and the minimal completions use them) and every prime up to the required limit.
    if (primes.size() < OMEGA
        || !std::equal(first_primes.begin(), first_primes.begin() + OMEGA, primes.begin())) {
        std::cerr << "Error: the generated prime list does not start with the first omega primes.\n";
        return 2;
    }
    if (mpz_class(prime_limit) < required_prime_limit
        || mpz_class(primes.back()) > mpz_class(prime_limit)) {
        std::cerr << "Error: the generated prime list does not cover floor(B / p_(omega-1)#) = "
                  << required_prime_limit << "; support enumeration would not be exhaustive.\n";
        return 2;
    }

    // Checkpoints: this run's fingerprint, and the run to continue (--resume)
    lpr::checkpoint::Fingerprint fingerprint;
    {
        std::string commit = LPR_GIT_COMMIT;
        std::replace(commit.begin(), commit.end(), ' ', '_'); // One word in the file
        fingerprint.commit = commit;
        fingerprint.omega = OMEGA;
        fingerprint.s = search.s;
        fingerprint.prime_limit = prime_limit;
        fingerprint.num_primes = primes.size();
        fingerprint.bound_hex = bound.get_str(16);
        fingerprint.primitive_root_search_limit = parameters::PRIMITIVE_ROOT_SEARCH_LIMIT;
        fingerprint.trial_division_min = lpr::trial_division::table().min_prime;
        fingerprint.trial_division_max = lpr::trial_division::table().max_prime;
        fingerprint.processing = mode != RunMode::DryRun;
    }
    std::vector<lpr::producer::detail::Task> tasks{lpr::producer::detail::root_task(primes.size())};
    lpr::checkpoint::Checkpoint resumed; // Valid if resuming
    const bool resuming = !options.resume_file.empty();
    bool commit_differs = false;
    if (resuming) {
        std::string error;
        if (!lpr::checkpoint::load(options.resume_file, resumed, error)) {
            std::cerr << "Error: cannot resume: " << error << '\n';
            return 2;
        }
        const std::string differences = lpr::checkpoint::compare(resumed.fingerprint, fingerprint, &commit_differs);
        if (!differences.empty()) {
            std::cerr << "Error: cannot resume from '" << options.resume_file
                      << "': it was written by a different search:\n" << differences;
            return 2;
        }
        if (commit_differs && !options.resume_any_commit) {
            std::cerr << "Error: cannot resume from '" << options.resume_file << "': it was written by commit "
                      << resumed.fingerprint.commit << ", this is " << fingerprint.commit
                      << " (pass --resume-any-commit to accept it)\n";
            return 2;
        }
        // Every task must be a position the DFS can reach (checked by replaying it)
        lpr::producer::detail::DfsStack check(bound, primes.size());
        std::vector<unsigned> check_support = lpr::producer::initial_support(primes);
        for (std::size_t k = 0; k < resumed.tasks.size(); ++k) {
            if (!check.restore(resumed.tasks[k], primes, bound, check_support, &error)) {
                std::cerr << "Error: cannot resume from '" << options.resume_file << "': task " << k + 1
                          << " is invalid: " << error << '\n';
                return 2;
            }
        }
        if (!lpr::producer::detail::tasks_disjoint(resumed.tasks, &error)) {
            std::cerr << "Error: cannot resume from '" << options.resume_file << "': " << error << '\n';
            return 2;
        }
        tasks = resumed.tasks;
    }
    if (!options.checkpoint_file.empty()) {
        // Never overwrite another run's progress: an existing checkpoint file may only
        // be replaced by the run that continues it
        if (access(options.checkpoint_file.c_str(), F_OK) == 0 && options.resume_file != options.checkpoint_file) {
            std::cerr << "Error: checkpoint file '" << options.checkpoint_file << "' already exists. Continue that "
                      << "run with --resume " << options.checkpoint_file << ", or delete the file to start a new search\n";
            return 2;
        }
        // Fail now rather than hours later: the checkpoint's temporary file must be creatable
        const std::string probe = options.checkpoint_file + ".tmp";
        const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::cerr << "Error: cannot write checkpoints: cannot create '" << probe << "': "
                      << std::strerror(errno) << '\n';
            return 2;
        }
        ::close(fd);
        ::unlink(probe.c_str());
    }
    const std::uint64_t segment = resuming ? resumed.segments + 1 : 1;
    const double earlier_elapsed = resuming ? resumed.elapsed_seconds : 0.0;

    // Print run and configuration settings
    std::string log_target = options.log_file.empty() ? "stderr" : options.log_file;
    if (options.mpi && mpi_size > 1) {
        log_target += options.log_file.empty() ? " (every rank)" : " (rank 0; rank r to " + options.log_file + ".rank<r>)";
    }
    std::cout << "Run:\n"
              << "mode: " << lpr::cli::mode_name(mode) << '\n';
    const bool threaded = mode == RunMode::Parallel || mode == RunMode::ParallelQueue
                          || (mode == RunMode::DryRun && options.threads > 1);
    if (threaded) {
        std::cout << "threads: " << options.threads
                  << " (from " << lpr::cli::thread_source_name(options.thread_source) << ")\n";
    }
    if (options.split_test_interval > 0) {
        std::cout << "split test: every " << options.split_test_interval << " DFS steps, "
                  << (options.split_test_fifo ? "fifo" : "lifo") << " work list\n";
    }
    if (options.steal_stress) {
        std::cout << "steal stress: idle workers never back off\n";
    }
    if (mode == RunMode::Parallel || (mode == RunMode::DryRun && options.threads > 1)) {
        std::cout << "split regions while prefix < " << steal_max_prefix << " primes"
                  << (options.steal_max_prefix > 0 ? " (--steal-max-prefix)" : " (default: omega)") << '\n';
    }
    if (!options.checkpoint_file.empty()) {
        std::cout << "checkpoint: " << options.checkpoint_file << " (";
        if (options.checkpoint_interval > 0) {
            std::cout << "every " << options.checkpoint_interval << " s, ";
        }
        std::cout << "on SIGTERM/SIGINT and at the end)\n";
    }
    if (resuming) {
        std::cout << "resumed from: " << options.resume_file << " (run " << segment << "; "
                  << (resumed.complete ? "search already complete" : std::to_string(tasks.size()) + " tasks")
                  << "; " << resumed.stats.total_support_count << " supports already processed, "
                  << resumed.stats.supports_emitted << " emitted; " << earlier_elapsed << " s in earlier runs"
                  << (commit_differs ? "; WARNING: written by commit " + resumed.fingerprint.commit : "")
                  << ")\n";
    }
    if (options.expected_candidates > 0) {
        std::cout << "expected candidates: " << options.expected_candidates << " (for the progress ETA)\n";
    }
    std::cout << "progress log: ";
    if (options.log_interval == 0) {
        std::cout << "disabled\n";
    } else {
        std::cout << "every " << options.log_interval << " s to " << log_target << '\n';
    }
#ifdef LPR_MPI
    std::vector<std::string> mpi_hosts;
    if (options.mpi) {
        mpi_hosts = lpr::mpi::gather_hostnames(mpi_rank, mpi_size);
        std::cout << "mpi: " << mpi_size << " ranks x " << options.threads << " threads"
                  << (options.mpi_static ? ", static split (M1)" : "")
                  << ", request when " << options.mpi_request_idle << " workers idle, pool reserve "
                  << options.mpi_pool_reserve
                  << (options.mpi_stress ? ", STRESS" : "") << '\n'
                  << "mpi hosts:";
        for (std::size_t r = 0; r < mpi_hosts.size(); ++r) std::cout << ' ' << r << '=' << mpi_hosts[r]; // Rank 0 only
        std::cout << '\n';
    }
#endif
    std::cout << "host: " << hostname() << '\n'
              << "started: " << local_time_string() << '\n'
              << "build: commit " << LPR_GIT_COMMIT
              << ", compiler " << __VERSION__
              << ", GMP " << gmp_version << "\n\n";

    std::cout << "Configuration:\n"
              << "omega: " << OMEGA << " (" << search.omega_source << ")\n"
              << "s: " << search.s << " (" << search.s_source << ")\n"
              << "Search bound B (Cor 4 global bound on p - 1): "
              << std::scientific << std::setprecision(6) << bound.get_d()
              << std::defaultfloat << '\n'
              << "B (exact): " << bound << '\n'
              << "Prime generation limit: " << prime_limit
              << " (floor(B / p_(omega-1)#) = " << required_prime_limit << ", "
              << primes.size() << " primes)\n"
              << "Trial division: primes " << lpr::trial_division::table().min_prime << ".."
              << lpr::trial_division::table().max_prime << " ("
              << lpr::trial_division::table().tests.size() << " primes, "
              << lpr::trial_division::table().chunks.size() << " chunks)\n"
              << "Primitive root search limit: "
              << parameters::PRIMITIVE_ROOT_SEARCH_LIMIT
              << "\n\n";
    std::cout.flush();

    const auto start_time = std::chrono::steady_clock::now();
    lpr::log::set_start(start_time);
    lpr::log::info(std::string("started: mode=") + lpr::cli::mode_name(mode)
                   + " threads=" + std::to_string(threaded ? options.threads : 1)
                   + " omega=" + std::to_string(OMEGA)
                   + " s=" + std::to_string(search.s)
                   + " commit=" + LPR_GIT_COMMIT
                   + (resuming ? " resumed_from=" + options.resume_file + " run=" + std::to_string(segment) : ""));
    if (resuming) {
        lpr::stats::get_master_stats().merge(resumed.stats); // The earlier runs' work
    }

    lpr::parallel::JobQueue<> queue; // Only used in queue mode
    const bool work_stealing = mode == RunMode::Parallel || (mode == RunMode::DryRun && options.threads > 1)
                               || options.mpi;
    // Checkpointing runs single-threaded modes through the scheduler with one worker
    const bool use_scheduler = work_stealing || options.checkpointing();
    // Under MPI the tasks start in rank 0's pool, not in the scheduler
    lpr::stealing::Scheduler scheduler(primes, bound, work_stealing ? options.threads : 1, options.steal_stress,
                                       steal_max_prefix, options.mpi ? std::vector<lpr::producer::detail::Task>{} : tasks,
                                       options.mpi);
#ifdef LPR_MPI
    std::optional<lpr::mpi::Node> mpi_node;
    if (options.mpi) {
        lpr::mpi::NodeOptions node_options;
        node_options.request_idle = options.mpi_request_idle;
        node_options.pool_reserve = options.mpi_pool_reserve;
        node_options.stress = options.mpi_stress;
        node_options.static_split = options.mpi_static;
        std::vector<lpr::producer::detail::Task> initial;
        if (mpi_rank == 0) {
            initial = options.mpi_static
                ? lpr::mpi::static_split(primes, bound, 4 * static_cast<std::size_t>(mpi_size))
                : tasks;
        }
        mpi_node.emplace(scheduler, primes, bound, mpi_rank, mpi_size, node_options, std::move(initial));
    }
#endif

    // Writes a checkpoint of the current state (monitor thread). With stay_paused
    // the workers are left stopped (the process is about to exit).
    auto take_checkpoint = [&](bool stay_paused) -> CheckpointOutcome {
        CheckpointOutcome outcome;
        const auto paused = scheduler.pause(PAUSE_TIMEOUT);
        if (paused == lpr::stealing::Scheduler::PauseResult::SearchFinishing) {
            outcome.kind = CheckpointOutcome::SearchFinishing;
            return outcome;
        }
        if (paused == lpr::stealing::Scheduler::PauseResult::TimedOut) {
            outcome.message = "the workers did not stop within " + std::to_string(PAUSE_TIMEOUT.count()) + " s";
            return outcome;
        }
        lpr::checkpoint::Checkpoint ck;
        ck.fingerprint = fingerprint;
        ck.segments = segment;
        ck.elapsed_seconds = earlier_elapsed
            + std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        ck.stats = lpr::stats::get_master_stats().snapshot(); // Every worker published before stopping
        std::string error;
        bool ok = scheduler.remaining_tasks(ck.tasks, error);
        if (ok && ck.tasks.empty()) { // The last region has just finished
            scheduler.resume();
            outcome.kind = CheckpointOutcome::SearchFinishing;
            return outcome;
        }
        ok = ok && lpr::checkpoint::write_atomic(options.checkpoint_file, ck, error);
        if (!ok || !stay_paused) {
            scheduler.resume();
        }
        if (!ok) {
            outcome.message = error;
            return outcome;
        }
        outcome.kind = CheckpointOutcome::Written;
        outcome.stats = ck.stats;
        return outcome;
    };

    {
        Monitor monitor(options.log_interval, start_time, [&]() -> std::string {
            if (mode == RunMode::ParallelQueue) return "queue=" + std::to_string(queue.size());
#ifdef LPR_MPI
            if (mpi_node && mpi_rank == 0) {
                // Rank 0's own counts come first in the line; these cover every rank
                const lpr::stats::Counters all = mpi_node->global_progress();
                return "queue=- active_workers=" + std::to_string(scheduler.active_workers())
                       + " mpi_pool=" + std::to_string(mpi_node->pool_size())
                       + " all_ranks_emitted=" + std::to_string(all.supports_emitted)
                       + " all_ranks_processed=" + std::to_string(all.total_support_count)
                       + " all_ranks_candidates=" + std::to_string(all.candidate_count)
                       + " all_ranks_fermat_survivors=" + std::to_string(all.fermat_survivor_count)
                       + " all_ranks_certified=" + std::to_string(all.certified_count)
                       + " all_ranks_failed=" + std::to_string(all.certified_inequality_fail_count)
                       + " all_ranks_unresolved=" + std::to_string(all.unresolved_count)
                       + " all_ranks_largest_g=" + std::to_string(all.largest_least_primitive_root);
            }
#endif
            if (work_stealing) return "queue=- active_workers=" + std::to_string(scheduler.active_workers());
            return "queue=-";
        }, options.checkpoint_file.empty() ? Monitor::CheckpointFn{} : Monitor::CheckpointFn(take_checkpoint),
           options.checkpoint_interval, options.checkpoint_file, options.expected_candidates,
#ifdef LPR_MPI
           // Under MPI, rank 0's candidate rate and ETA count every rank
           mpi_node && mpi_rank == 0
               ? std::function<lpr::stats::Counters()>([&]() { return mpi_node->global_progress(); })
               : std::function<lpr::stats::Counters()>{}
#else
           std::function<lpr::stats::Counters()>{}
#endif
           );

        if (options.mpi) {
#ifdef LPR_MPI
            // The workers on their own threads; this thread (which initialised MPI)
            // is the rank's communication thread
            std::thread workers([&]() {
                if (mode == RunMode::DryRun) scheduler.run<false>();
                else scheduler.run<true>();
            });
            mpi_node->run();
            workers.join();
#endif
        } else if (use_scheduler && mode == RunMode::DryRun) {
            scheduler.run<false>();
        } else if (use_scheduler) {
            scheduler.run<true>();
        } else if (mode == RunMode::DryRun && options.split_test_interval > 0) {
            lpr::producer::generate_supports_split_test<false>(
                primes, bound, options.split_test_interval, options.split_test_fifo);
        } else if (mode == RunMode::DryRun) {
            lpr::producer::generate_supports_dry_run(primes, bound);
        } else if (mode == RunMode::Single && options.split_test_interval > 0) {
            lpr::producer::generate_supports_split_test<true>(
                primes, bound, options.split_test_interval, options.split_test_fifo);
        } else if (mode == RunMode::Single) {
            lpr::producer::generate_supports(primes, bound);
        } else {
            // Spawn single producer (it also co-processes, and consumes once finished)
            std::thread producer([&primes, &bound, &queue]() {
                lpr::producer::generate_supports_parallel(primes, bound, queue);
            });

            // Spawn the remaining threads as consumers
            std::vector<std::thread> consumers;
            consumers.reserve(options.threads - 1);
            for (unsigned i = 0; i + 1 < options.threads; ++i) {
                consumers.emplace_back([&queue]() {
                    lpr::consumer::start_consumer(queue);
                });
            }

            producer.join(); // End producer thread
            for (auto& consumer : consumers) { // End consumer threads
                consumer.join();
            }
        }
        monitor.stop();
    }

    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - start_time;

    // Worker threads publish their counters when they exit (see stats.hpp);
    // this thread's own counters (single and dry-run modes) are published here
    lpr::stats::publish_thread_local_stats();
#ifdef LPR_MPI
    // Every rank's statistics, merged on rank 0
    const lpr::stats::Counters local_stats = lpr::stats::get_master_stats().snapshot();
    const lpr::stats::Counters master_stats = options.mpi
        ? lpr::mpi::reduce_counters(local_stats, mpi_rank, mpi_size) : local_stats;
    std::vector<lpr::mpi::NodeCounters> mpi_rows;
    if (options.mpi) {
        lpr::mpi::NodeCounters& row = mpi_node->counters();
        row.worker_idle_ns = local_stats.worker_idle_ns;
        row.supports_processed = local_stats.total_support_count;
        row.supports_emitted = local_stats.supports_emitted;
        mpi_rows = lpr::mpi::gather_node_counters(row, mpi_rank, mpi_size);
    }
#else
    const lpr::stats::Counters master_stats = lpr::stats::get_master_stats().snapshot();
#endif
    lpr::log::info("finished after " + std::to_string(elapsed.count()) + " s");
    lpr::log::info(progress_line(master_stats, 0.0, "queue=-"));
    const double total_elapsed = earlier_elapsed + elapsed.count();

    // The final checkpoint: the search is complete (resuming it prints this summary)
    if (!options.checkpoint_file.empty()) {
        lpr::checkpoint::Checkpoint ck;
        ck.fingerprint = fingerprint;
        ck.segments = segment;
        ck.elapsed_seconds = total_elapsed;
        ck.complete = true;
        ck.stats = master_stats;
        std::string error;
        if (lpr::checkpoint::write_atomic(options.checkpoint_file, ck, error)) {
            lpr::log::info("checkpoint: final state (search complete) written to " + options.checkpoint_file);
        } else {
            lpr::log::warn("final checkpoint not written: " + error);
        }
    }
    auto print_elapsed = [&]() {
        std::cout << "Elapsed time: " << elapsed.count() << " s\n";
        if (segment > 1) {
            std::cout << "Elapsed time over all " << segment << " runs: " << total_elapsed << " s\n";
        }
    };

    auto print_mpi = [&]() {
#ifdef LPR_MPI
        if (options.mpi) {
            std::cout << '\n';
            lpr::mpi::print_summary(mpi_rows, mpi_hosts);
        }
#endif
    };

    if (mode == RunMode::DryRun) {
        master_stats.print_producer();
        print_mpi();
        print_elapsed();
        std::cout << "DRY RUN: no primes were tested or verified\n";
        return 0;
    }
    master_stats.print();
    std::cout << '\n';
    master_stats.print_producer();
    print_mpi();
    print_elapsed();

    int exit_code = 0;
    if (master_stats.unresolved_count > 0 ||
        master_stats.certified_inequality_fail_count > 0) {
        exit_code = 1;
    }
#ifdef LPR_MPI
    if (options.mpi) { // Rank 0 decides (it has every rank's counts)
        MPI_Bcast(&exit_code, 1, MPI_INT, 0, MPI_COMM_WORLD);
    }
#endif
    return exit_code;
}
