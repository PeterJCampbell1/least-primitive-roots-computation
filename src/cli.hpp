/**
 * cli.hpp: command line parsing.
 *
 * Modes (at most one): --single (default), --parallel (work stealing),
 *          --parallel-queue (1 producer + consumers), --dry-run.
 * Options: --threads N, --log-interval SECONDS, --log-file PATH,
 *          --config PATH, --omega N, --s N, -h/--help.
 * Checkpoints: --checkpoint PATH, --checkpoint-interval SECONDS, --resume PATH,
 *          --resume-any-commit (docs/checkpointing.md).
 * Testing/tuning: --split-test N, --split-test-order lifo|fifo, --steal-stress,
 *          --steal-max-prefix K.
 * Value options accept both "--opt value" and "--opt=value".
 **/

#pragma once

#include "config.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

namespace lpr::cli {

enum class RunMode { Single, Parallel, ParallelQueue, DryRun };
enum class ThreadSource { None, CommandLine, Slurm, Hardware };

struct Options {
    RunMode mode = RunMode::Single;
    unsigned threads = 1; // Workers (--parallel, threaded --dry-run) or 1 producer + threads - 1 consumers (--parallel-queue)
    ThreadSource thread_source = ThreadSource::None;
    unsigned log_interval = 120; // Seconds between progress logs, 0 disables
    std::string log_file; // Empty means stderr
    std::string config_file; // Empty means none
    lpr::config::Overrides search; // --omega / --s (override the config file)
    unsigned split_test_interval = 0; // --split-test N: split every N DFS steps (0 = off)
    bool split_test_fifo = false; // --split-test-order fifo (default lifo)
    bool steal_stress = false; // --steal-stress: idle workers never back off
    unsigned steal_max_prefix = 0; // --steal-max-prefix K: split only regions whose prefix has < K primes (0 = omega)
    std::string checkpoint_file; // --checkpoint PATH: write checkpoints there (empty = none)
    double checkpoint_interval = 600; // --checkpoint-interval SECONDS between checkpoints (0 = only on signals and at the end)
    std::string resume_file; // --resume PATH: continue the run saved there (empty = new search)
    bool resume_any_commit = false; // --resume-any-commit: accept a checkpoint from another commit
    std::uint64_t expected_candidates = 0; // --expected-candidates N: total for the progress ETA (0 = unknown)
    bool help = false;

    bool checkpointing() const { return !checkpoint_file.empty() || !resume_file.empty(); }
};

struct ParseResult {
    std::optional<Options> options;
    std::string error; // Set when options is empty
};

inline const char* mode_name(RunMode mode) {
    switch (mode) {
    case RunMode::Single: return "single";
    case RunMode::Parallel: return "parallel";
    case RunMode::ParallelQueue: return "parallel-queue";
    case RunMode::DryRun: return "dry-run";
    }
    return "unknown";
}

inline const char* thread_source_name(ThreadSource source) {
    switch (source) {
    case ThreadSource::None: return "n/a";
    case ThreadSource::CommandLine: return "--threads";
    case ThreadSource::Slurm: return "SLURM_CPUS_PER_TASK";
    case ThreadSource::Hardware: return "hardware concurrency";
    }
    return "unknown";
}

inline std::string usage(const char* program) {
    return std::string("Usage: ") + program + R"( [--single | --parallel | --parallel-queue | --dry-run] [options]

Modes:
  --single               Single-threaded computation (default)
  --parallel             Work stealing: every thread searches its own region of the
                         tree and processes its supports; idle threads take work
                         from busy ones (see docs/work_stealing.md)
  --parallel-queue       The previous design: one producer thread plus consumer
                         threads connected by a queue (kept for comparison)
  --dry-run              Run the support enumeration only; nothing is verified

Search parameters (default omega = )" + std::to_string(parameters::DEFAULT_OMEGA) + R"(, s = omega - 3):
  --config PATH          Read omega and s from PATH ("key = value" lines, '#' comments)
  --omega N              Number of distinct prime factors of p - 1 (overrides --config)
  --s N                  Corollary 4 parameter, 1 <= s <= omega - 3 (overrides --config)

Options:
  --threads N            Threads for --parallel (N >= 1) or --parallel-queue (N >= 2).
                         Defaults to SLURM_CPUS_PER_TASK, then the hardware thread
                         count. With --dry-run (default 1), N > 1 searches the
                         tree with work stealing
  --log-interval SECONDS Seconds between progress logs (default 120, 0 disables)
  --log-file PATH        Append progress logs to PATH instead of stderr.
                         Warnings always also go to stderr
  --expected-candidates N
                         Expected total of "Candidates not covered by Corollary 4"
                         (from experimental/estimate or an earlier run): the
                         progress line then shows the percentage done and an ETA.
                         Under work stealing candidates/s is steadier than
                         supports/s, so the ETA is based on candidates
  -h, --help             Show this help

Checkpoints (with --single, --parallel or --dry-run; see docs/checkpointing.md):
  --checkpoint PATH      Save progress to PATH every --checkpoint-interval seconds,
                         on SIGTERM/SIGINT (then exit with code 4) and at the end
  --checkpoint-interval SECONDS
                         Seconds between checkpoints (default 600; decimals allowed;
                         0 = only on signals and at the end)
  --resume PATH          Continue the run saved in PATH (refused if omega, s, the
                         bound, other search settings, dry run or the commit
                         differ). The thread count and mode may differ. Usually
                         combined with --checkpoint PATH (the same file)
  --resume-any-commit    With --resume: accept a checkpoint written by another commit

Testing options:
  --split-test N         With --single or a 1-thread --dry-run: split the search
                         region every N DFS steps into a work list and run the
                         pieces afterwards (tests region splitting without threads)
  --split-test-order O   lifo (default) or fifo order for the --split-test work list
  --steal-stress         With --parallel or a threaded --dry-run: idle threads ask
                         for work continuously, without backing off
  --steal-max-prefix K   With --parallel or a threaded --dry-run: split a region only
                         while its fixed prefix has fewer than K primes (default
                         omega, i.e. any depth; e.g. omega - 3 never splits the
                         last three levels). 1 <= K <= omega

Exit codes:
  0  complete: every candidate certified and Grosswald's inequality holds
  1  complete, but a Grosswald failure or an unresolved candidate was found
  2  usage error
  3  interrupted (SIGTERM/SIGINT): partial run, nothing is verified, no checkpoint
  4  interrupted (SIGTERM/SIGINT) and a checkpoint was written: continue with --resume
)";
}

// Strict unsigned parse: digits only, no sign, no trailing characters
inline std::optional<unsigned> parse_unsigned(const std::string& text) {
    if (text.empty() || text.size() > 9) return std::nullopt;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}

// Strict unsigned 64-bit decimal: digits only, no overflow
inline std::optional<std::uint64_t> parse_u64(const std::string& text) {
    if (text.empty() || text.size() > 20) return std::nullopt;
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

// Strict non-negative decimal (e.g. "600", "0.5"); at most 10^7
inline std::optional<double> parse_seconds(const std::string& text) {
    if (text.empty() || text.size() > 20) return std::nullopt;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || c == '.')) return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    // (Only digits and dots are accepted, so value cannot be NaN; > 1e7 also rejects
    // infinity. No <cmath>: with GCC 8, including it makes the compiler constant-fold
    // through MPFR/GMP, which crashes on Rangpur's r730 nodes; see build_lpr.sh.)
    if (end == text.c_str() || *end != '\0' || !(value >= 0) || value > 1e7) {
        return std::nullopt;
    }
    return value;
}

inline ParseResult parse(int argc, char* argv[]) {
    Options options;
    int modes_given = 0;
    bool threads_given = false;
    bool split_order_given = false;
    bool checkpoint_interval_given = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::optional<std::string> inline_value;
        const auto eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            inline_value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        }

        // Fetches the value of a value option
        auto value = [&]() -> std::optional<std::string> {
            if (inline_value) return inline_value;
            if (i + 1 < argc) return std::string(argv[++i]);
            return std::nullopt;
        };
        auto flag_only = [&]() { return !inline_value; };

        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return {options, ""};
        } else if (arg == "--single" && flag_only()) {
            options.mode = RunMode::Single;
            ++modes_given;
        } else if (arg == "--parallel" && flag_only()) {
            options.mode = RunMode::Parallel;
            ++modes_given;
        } else if (arg == "--parallel-queue" && flag_only()) {
            options.mode = RunMode::ParallelQueue;
            ++modes_given;
        } else if (arg == "--dry-run" && flag_only()) {
            options.mode = RunMode::DryRun;
            ++modes_given;
        } else if (arg == "--threads") {
            const auto v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n) return {std::nullopt, "--threads needs a non-negative integer"};
            options.threads = *n;
            options.thread_source = ThreadSource::CommandLine;
            threads_given = true;
        } else if (arg == "--log-interval") {
            const auto v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n) return {std::nullopt, "--log-interval needs a non-negative integer (seconds)"};
            options.log_interval = *n;
        } else if (arg == "--config") {
            const auto v = value();
            if (!v || v->empty()) return {std::nullopt, "--config needs a path"};
            options.config_file = *v;
        } else if (arg == "--omega" || arg == "--s") {
            const auto v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n) return {std::nullopt, arg + " needs a non-negative integer"};
            (arg == "--omega" ? options.search.omega : options.search.s) = *n;
        } else if (arg == "--split-test") {
            const auto v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n == 0) return {std::nullopt, "--split-test needs a positive integer (DFS steps)"};
            options.split_test_interval = *n;
        } else if (arg == "--split-test-order") {
            const auto v = value();
            if (!v || (*v != "lifo" && *v != "fifo")) return {std::nullopt, "--split-test-order needs lifo or fifo"};
            options.split_test_fifo = (*v == "fifo");
            split_order_given = true;
        } else if (arg == "--steal-stress" && flag_only()) {
            options.steal_stress = true;
        } else if (arg == "--steal-max-prefix") {
            const auto v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n == 0) return {std::nullopt, "--steal-max-prefix needs a positive integer"};
            options.steal_max_prefix = *n;
        } else if (arg == "--checkpoint") {
            const auto v = value();
            if (!v || v->empty()) return {std::nullopt, "--checkpoint needs a path"};
            options.checkpoint_file = *v;
        } else if (arg == "--checkpoint-interval") {
            const auto v = value();
            const auto t = v ? parse_seconds(*v) : std::nullopt;
            if (!t) return {std::nullopt, "--checkpoint-interval needs a non-negative number of seconds"};
            options.checkpoint_interval = *t;
            checkpoint_interval_given = true;
        } else if (arg == "--resume") {
            const auto v = value();
            if (!v || v->empty()) return {std::nullopt, "--resume needs a path"};
            options.resume_file = *v;
        } else if (arg == "--resume-any-commit" && flag_only()) {
            options.resume_any_commit = true;
        } else if (arg == "--expected-candidates") {
            const auto v = value();
            const auto n = v ? parse_u64(*v) : std::nullopt;
            if (!n || *n == 0) return {std::nullopt, "--expected-candidates needs a positive integer"};
            options.expected_candidates = *n;
        } else if (arg == "--log-file") {
            const auto v = value();
            if (!v || v->empty()) return {std::nullopt, "--log-file needs a path"};
            options.log_file = *v;
        } else {
            return {std::nullopt, "unrecognised argument '" + std::string(argv[i]) + "'"};
        }
    }

    if (modes_given > 1) {
        return {std::nullopt, "give at most one of --single, --parallel, --parallel-queue, --dry-run"};
    }
    const bool parallel_mode = options.mode == RunMode::Parallel || options.mode == RunMode::ParallelQueue;
    if (threads_given && !parallel_mode && options.mode != RunMode::DryRun) {
        return {std::nullopt, "--threads is only valid with --parallel, --parallel-queue or --dry-run"};
    }

    if (parallel_mode && !threads_given) {
        const char* slurm = std::getenv("SLURM_CPUS_PER_TASK");
        const auto slurm_threads = slurm ? parse_unsigned(slurm) : std::nullopt;
        if (slurm_threads) {
            options.threads = *slurm_threads;
            options.thread_source = ThreadSource::Slurm;
        } else if (std::thread::hardware_concurrency() > 0) {
            options.threads = std::thread::hardware_concurrency();
            options.thread_source = ThreadSource::Hardware;
        } else {
            return {std::nullopt, "could not detect the thread count; pass --threads N"};
        }
    }
    if (options.mode == RunMode::ParallelQueue && options.threads < 2) {
        return {std::nullopt, "--parallel-queue needs at least 2 threads (1 producer + 1 consumer), got "
                              + std::to_string(options.threads) + " from "
                              + thread_source_name(options.thread_source) + "; use --single instead"};
    }
    if ((options.mode == RunMode::Parallel || options.mode == RunMode::DryRun) && options.threads < 1) {
        return {std::nullopt, std::string("--") + mode_name(options.mode) + " needs at least 1 thread"};
    }

    // Testing options
    const bool threaded_search = options.mode == RunMode::Parallel
                                 || (options.mode == RunMode::DryRun && options.threads > 1);
    if (options.split_test_interval > 0 && threaded_search) {
        return {std::nullopt, "--split-test is single-threaded: use it with --single or a 1-thread --dry-run"};
    }
    if (options.split_test_interval > 0 && options.mode == RunMode::ParallelQueue) {
        return {std::nullopt, "--split-test is single-threaded: use it with --single or a 1-thread --dry-run"};
    }
    if (split_order_given && options.split_test_interval == 0) {
        return {std::nullopt, "--split-test-order needs --split-test N"};
    }
    if (options.steal_stress && !threaded_search) {
        return {std::nullopt, "--steal-stress needs --parallel or a threaded --dry-run (--threads N, N > 1)"};
    }
    if (options.steal_max_prefix > 0 && !threaded_search) {
        return {std::nullopt, "--steal-max-prefix needs --parallel or a threaded --dry-run (--threads N, N > 1)"};
    }

    // Checkpoints
    if (checkpoint_interval_given && options.checkpoint_file.empty()) {
        return {std::nullopt, "--checkpoint-interval needs --checkpoint PATH"};
    }
    if (options.resume_any_commit && options.resume_file.empty()) {
        return {std::nullopt, "--resume-any-commit needs --resume PATH"};
    }
    if (options.checkpointing() && options.mode == RunMode::ParallelQueue) {
        return {std::nullopt, "--checkpoint and --resume work with --single, --parallel or --dry-run, "
                              "not --parallel-queue"};
    }
    if (options.checkpointing() && options.split_test_interval > 0) {
        return {std::nullopt, "--checkpoint and --resume cannot be combined with --split-test"};
    }

    return {options, ""};
}

} // namespace lpr::cli
