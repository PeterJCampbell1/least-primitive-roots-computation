/**
 * cli.hpp: command line parsing.
 *
 * Modes (at most one): --single (default), --parallel, --dry-run.
 * Options: --threads N, --log-interval SECONDS, --log-file PATH, -h/--help.
 * Value options accept both "--opt value" and "--opt=value".
 **/

#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

namespace lpr::cli {

enum class RunMode { Single, Parallel, DryRun };
enum class ThreadSource { None, CommandLine, Slurm, Hardware };

struct Options {
    RunMode mode = RunMode::Single;
    unsigned threads = 1; // Total threads (1 producer + threads - 1 consumers) in parallel mode
    ThreadSource thread_source = ThreadSource::None;
    unsigned log_interval = 120; // Seconds between progress logs, 0 disables
    std::string log_file; // Empty means stderr
    bool help = false;
};

struct ParseResult {
    std::optional<Options> options;
    std::string error; // Set when options is empty
};

inline const char* mode_name(RunMode mode) {
    switch (mode) {
    case RunMode::Single: return "single";
    case RunMode::Parallel: return "parallel";
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
    return std::string("Usage: ") + program + R"( [--single | --parallel | --dry-run] [options]

Modes:
  --single               Single-threaded computation (default)
  --parallel             One producer thread plus consumer threads
  --dry-run              Run the support enumeration only; nothing is verified

Options:
  --threads N            Total threads for --parallel (N >= 2). Defaults to
                         SLURM_CPUS_PER_TASK, then the hardware thread count
  --log-interval SECONDS Seconds between progress logs (default 120, 0 disables)
  --log-file PATH        Append progress logs to PATH instead of stderr.
                         Warnings always also go to stderr
  -h, --help             Show this help

Exit codes:
  0  complete: every candidate certified and Grosswald's inequality holds
  1  complete, but a Grosswald failure or an unresolved candidate was found
  2  usage error
  3  interrupted (SIGTERM/SIGINT): partial run, nothing is verified
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

inline ParseResult parse(int argc, char* argv[]) {
    Options options;
    int modes_given = 0;
    bool threads_given = false;

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
        } else if (arg == "--log-file") {
            const auto v = value();
            if (!v || v->empty()) return {std::nullopt, "--log-file needs a path"};
            options.log_file = *v;
        } else {
            return {std::nullopt, "unrecognised argument '" + std::string(argv[i]) + "'"};
        }
    }

    if (modes_given > 1) {
        return {std::nullopt, "give at most one of --single, --parallel, --dry-run"};
    }
    if (threads_given && options.mode != RunMode::Parallel) {
        return {std::nullopt, "--threads is only valid with --parallel"};
    }

    if (options.mode == RunMode::Parallel && !threads_given) {
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
    if (options.mode == RunMode::Parallel && options.threads < 2) {
        return {std::nullopt, "--parallel needs at least 2 threads (1 producer + 1 consumer), got "
                              + std::to_string(options.threads) + " from "
                              + thread_source_name(options.thread_source) + "; use --single instead"};
    }

    return {options, ""};
}

} // namespace lpr::cli
