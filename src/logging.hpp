/**
 * logging.hpp: thread-safe, timestamped logging.
 *
 * Progress messages (info) go to the log file given by --log-file, or to
 * stderr if none is given. Warnings (counterexamples, unresolved candidates)
 * always go to stderr, and are also copied into the log file if one is open.
 * Every line is flushed immediately so logs stay current on a cluster.
 **/

#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace lpr::log {

namespace detail {

struct LogState {
    std::mutex mutex;
    std::ofstream file;
    bool to_file = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::string tag; // E.g. "rank 3: " under MPI (empty otherwise)
};

inline LogState& state() {
    static LogState instance;
    return instance;
}

// "[YYYY-mm-dd HH:MM:SS +elapsed s] "
inline std::string prefix(const LogState& s) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s.start
    ).count();

    std::ostringstream out;
    out << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " +" << elapsed << "s] " << s.tag;
    return out.str();
}

// Writes msg with the prefix on every line
inline void write_lines(std::ostream& out, const std::string& prefix, const std::string& msg) {
    std::istringstream lines(msg);
    std::string line;
    while (std::getline(lines, line)) {
        out << prefix << line << '\n';
    }
    out.flush();
}

} // namespace detail

// Sends info messages to path (appending) instead of stderr. Returns false on failure.
inline bool open_file(const std::string& path) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.file.open(path, std::ios::app);
    s.to_file = s.file.is_open();
    return s.to_file;
}

// Elapsed times in the prefix are measured from this point
inline void set_start(std::chrono::steady_clock::time_point start) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.start = start;
}

// Text after the timestamp on every line (e.g. the MPI rank)
inline void set_tag(const std::string& tag) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.tag = tag;
}

inline void info(const std::string& msg) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::write_lines(s.to_file ? static_cast<std::ostream&>(s.file) : std::cerr, detail::prefix(s), msg);
}

inline void warn(const std::string& msg) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::string prefix = detail::prefix(s) + "WARNING: ";
    detail::write_lines(std::cerr, prefix, msg);
    if (s.to_file) {
        detail::write_lines(s.file, prefix, msg);
    }
}

} // namespace lpr::log
