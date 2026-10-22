#include "primality.hpp"
#include "stats.hpp"
#include "parameters.hpp"
#include "corollary4.hpp"
#include "generate_supports.hpp"
#include "process_support.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "trial_division.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cstdint>
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
#include <unistd.h> // gethostname

#ifndef LPR_GIT_COMMIT
#define LPR_GIT_COMMIT "unknown"
#endif

// Anonymous namespace - restricts type visibility to this file
namespace {

constexpr int EXIT_INTERRUPTED = 3;

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

// One line of key=value progress fields from a statistics snapshot
std::string progress_line(
    const lpr::stats::Counters& s,
    double supports_per_second,
    const std::optional<std::size_t>& queue_size
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
        << " queue=" << (queue_size ? std::to_string(*queue_size) : std::string("-"))
        << " coprocessed=" << s.batches_coprocessed
        << " consumer_waits=" << s.consumer_waits
        << " consumer_wait_s=" << std::setprecision(1) << s.consumer_wait_ns / 1e9
        << " position=" << lpr::producer::dfs_position_string();
    return out.str();
}

/*
 * Background thread that logs progress every log_interval seconds (if non-zero)
 * and handles SIGTERM/SIGINT. It wakes once per second; on a signal it prints
 * the statistics so far (stdout and log) and exits with EXIT_INTERRUPTED.
 */
class Monitor {
public:
    Monitor(
        unsigned log_interval,
        std::chrono::steady_clock::time_point start,
        std::function<std::optional<std::size_t>()> queue_size
    ) : log_interval_(log_interval), start_(start), queue_size_(std::move(queue_size)) {
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
    std::function<std::optional<std::size_t>()> queue_size_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;

    void run() {
        using clock = std::chrono::steady_clock;
        auto last_log = clock::now();
        std::uint64_t last_processed = 0;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return stop_; })) {
                    return;
                }
            }

            if (g_signal != 0) {
                handle_interrupt();
            }

            const auto now = clock::now();
            if (log_interval_ > 0 && now - last_log >= std::chrono::seconds(log_interval_)) {
                const lpr::stats::Counters snapshot = lpr::stats::get_master_stats().snapshot();
                const double seconds = std::chrono::duration<double>(now - last_log).count();
                const double rate = (snapshot.total_support_count - last_processed) / seconds;
                lpr::log::info(progress_line(snapshot, rate, queue_size_()));
                last_processed = snapshot.total_support_count;
                last_log = now;
            }
        }
    }

    [[noreturn]] void handle_interrupt() {
        const int sig = g_signal;
        const lpr::stats::Counters snapshot = lpr::stats::get_master_stats().snapshot();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();

        const std::string notice = "INTERRUPTED by signal " + std::to_string(sig)
            + " after " + std::to_string(elapsed) + " s: partial run, nothing has been verified";
        lpr::log::info(notice);
        lpr::log::info(progress_line(snapshot, 0.0, queue_size_()));

        std::cout << '\n' << notice << '\n'
                  << "DFS position: " << lpr::producer::dfs_position_string() << "\n\n";
        snapshot.print();
        std::cout << '\n';
        snapshot.print_producer();
        std::cout << "Elapsed time: " << elapsed << " s\n" << notice << '\n';
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(EXIT_INTERRUPTED);
    }
};

} // namespace


int main(int argc, char* argv[])
{
    using lpr::cli::RunMode;

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
    if (!options.log_file.empty() && !lpr::log::open_file(options.log_file)) {
        std::cerr << "Error: cannot open log file '" << options.log_file << "'\n";
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

    const RunMode mode = options.mode;
    switch (mode) {
    case RunMode::Single:
        std::cout << "Running single-threaded computation" << '\n' << '\n';
        break;
    case RunMode::Parallel:
        std::cout << "Running multi-threaded computation" << '\n'
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

    // Print run and configuration settings
    const std::string log_target = options.log_file.empty() ? "stderr" : options.log_file;
    std::cout << "Run:\n"
              << "mode: " << lpr::cli::mode_name(mode) << '\n';
    if (mode == RunMode::Parallel) {
        std::cout << "threads: " << options.threads
                  << " (from " << lpr::cli::thread_source_name(options.thread_source) << ")\n";
    }
    std::cout << "progress log: ";
    if (options.log_interval == 0) {
        std::cout << "disabled\n";
    } else {
        std::cout << "every " << options.log_interval << " s to " << log_target << '\n';
    }
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
                   + " threads=" + std::to_string(mode == RunMode::Parallel ? options.threads : 1)
                   + " omega=" + std::to_string(OMEGA)
                   + " s=" + std::to_string(search.s)
                   + " commit=" + LPR_GIT_COMMIT);

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    lpr::parallel::JobQueue<> queue; // Only used in parallel mode
    {
        Monitor monitor(options.log_interval, start_time, [&]() -> std::optional<std::size_t> {
            if (mode != RunMode::Parallel) return std::nullopt;
            return queue.size();
        });

        if (mode == RunMode::DryRun) {
            lpr::producer::generate_supports_dry_run(primes, bound);
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
    const lpr::stats::Counters master_stats = lpr::stats::get_master_stats().snapshot();
    lpr::log::info("finished after " + std::to_string(elapsed.count()) + " s");
    lpr::log::info(progress_line(master_stats, 0.0, std::nullopt));

    if (mode == RunMode::DryRun) {
        master_stats.print_producer();
        std::cout << "Elapsed time: " << elapsed.count() << " s\n"
                  << "DRY RUN: no primes were tested or verified\n";
        return 0;
    }
    master_stats.print();
    std::cout << '\n';
    master_stats.print_producer();
    std::cout << "Elapsed time: " << elapsed.count() << " s\n";

    if (master_stats.unresolved_count > 0 ||
        master_stats.certified_inequality_fail_count > 0) {
        return 1;
    }

    return 0;
}
