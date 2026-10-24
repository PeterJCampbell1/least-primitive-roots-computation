/**
 * checkpoint.hpp: checkpoint files (--checkpoint, --resume).
 *
 * A checkpoint records a run's progress so a later run can finish it:
 *  - a fingerprint of everything that determines the results (omega, s, the
 *    prime list, the search bound B, the primitive root search limit, the
 *    trial division range, dry run or not, and the commit); --resume refuses a
 *    checkpoint whose fingerprint differs from the current run's;
 *  - the statistics so far (every Counters field), covering exactly the work
 *    before the saved positions;
 *  - the remaining work as Tasks (generate_supports.hpp), each a region plus a
 *    DFS position, rebuilt on resume by DfsStack::restore;
 *  - the number of runs (segments) and their total elapsed time.
 * A checkpoint of a finished search has no tasks and "complete 1"; resuming it
 * just prints the final summary.
 *
 * The file is text, one "key value" per line, ending with a checksum line
 * (FNV-1a 64 of everything before it), so truncated or edited files are
 * rejected. It is written atomically: temporary file, fsync, read back and
 * compared, rename, fsync of the directory. See docs/checkpointing.md.
 **/

#pragma once

#include "generate_supports.hpp"
#include "stats.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace lpr::checkpoint {

constexpr unsigned FORMAT_VERSION = 1;

struct Fingerprint {
    std::string commit;
    std::uint64_t omega = 0;
    std::uint64_t s = 0;
    std::uint64_t prime_limit = 0;
    std::uint64_t num_primes = 0;
    std::string bound_hex; // B in hexadecimal
    std::uint64_t primitive_root_search_limit = 0;
    std::uint64_t trial_division_min = 0;
    std::uint64_t trial_division_max = 0;
    bool processing = true; // false for a dry run
};

struct Checkpoint {
    Fingerprint fingerprint;
    std::uint64_t segments = 0;   // Runs that have contributed to it
    double elapsed_seconds = 0;   // Their total elapsed time
    bool complete = false;        // The search has finished (no tasks)
    stats::Counters stats;
    std::vector<producer::detail::Task> tasks;
};

namespace detail {

inline std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

inline std::string hex64(std::uint64_t v) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << v;
    return out.str();
}

// Strict decimal: digits only, at most 20, no overflow
inline bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

inline std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream in(line);
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

} // namespace detail

// The checkpoint as text (including its checksum line)
inline std::string serialize(const Checkpoint& ck) {
    std::ostringstream out;
    const Fingerprint& f = ck.fingerprint;
    out << "lpr-checkpoint " << FORMAT_VERSION << '\n'
        << "commit " << f.commit << '\n'
        << "omega " << f.omega << '\n'
        << "s " << f.s << '\n'
        << "prime_limit " << f.prime_limit << '\n'
        << "num_primes " << f.num_primes << '\n'
        << "bound_hex " << f.bound_hex << '\n'
        << "primitive_root_search_limit " << f.primitive_root_search_limit << '\n'
        << "trial_division_min " << f.trial_division_min << '\n'
        << "trial_division_max " << f.trial_division_max << '\n'
        << "processing " << (f.processing ? 1 : 0) << '\n'
        << "segments " << ck.segments << '\n'
        << "elapsed_seconds " << std::fixed << std::setprecision(3) << ck.elapsed_seconds << '\n'
        << "complete " << (ck.complete ? 1 : 0) << '\n';
    stats::Counters::for_each_field(ck.stats, [&](const std::string& name, const std::uint64_t& v) {
        out << "stat " << name << ' ' << v << '\n';
    });
    out << "tasks " << ck.tasks.size() << '\n';
    for (const auto& t : ck.tasks) {
        out << "task " << t.base_depth << ' ' << t.base_limit << ' ' << t.depth;
        for (std::size_t idx : t.path) out << ' ' << idx;
        out << '\n';
    }
    const std::string body = out.str();
    return body + "checksum " + detail::hex64(detail::fnv1a(body)) + '\n';
}

/*
 * Parses a checkpoint. Strict: the checksum must match, every key must appear
 * exactly once in the expected order, every statistic must be present, and the
 * task count must match. Does not check the fingerprint or the tasks against
 * the current run (see compare and DfsStack::restore).
 */
inline bool parse(const std::string& text, Checkpoint& ck, std::string& error) {
    // Checksum: the last line covers everything before it
    const std::string marker = "checksum ";
    const std::size_t pos = text.rfind(marker);
    if (pos == std::string::npos || (pos != 0 && text[pos - 1] != '\n')) {
        error = "no checksum line (file truncated?)";
        return false;
    }
    const std::string body = text.substr(0, pos);
    std::string stored = text.substr(pos + marker.size());
    if (!stored.empty() && stored.back() == '\n') stored.pop_back();
    if (stored != detail::hex64(detail::fnv1a(body))) {
        error = "checksum mismatch (file damaged or edited)";
        return false;
    }

    std::istringstream in(body);
    std::string line;
    std::size_t line_number = 0;
    auto next = [&](const std::string& key, std::vector<std::string>& values) -> bool {
        if (!std::getline(in, line)) {
            error = "missing '" + key + "' line";
            return false;
        }
        ++line_number;
        std::vector<std::string> words = detail::split_words(line);
        if (words.empty() || words[0] != key) {
            error = "line " + std::to_string(line_number) + ": expected '" + key + "'";
            return false;
        }
        values.assign(words.begin() + 1, words.end());
        return true;
    };
    std::vector<std::string> v;
    auto number = [&](const std::string& key, std::uint64_t& out) -> bool {
        if (!next(key, v)) return false;
        if (v.size() != 1 || !detail::parse_u64(v[0], out)) {
            error = "line " + std::to_string(line_number) + ": bad value for '" + key + "'";
            return false;
        }
        return true;
    };
    auto text_value = [&](const std::string& key, std::string& out) -> bool {
        if (!next(key, v)) return false;
        if (v.size() != 1) {
            error = "line " + std::to_string(line_number) + ": bad value for '" + key + "'";
            return false;
        }
        out = v[0];
        return true;
    };

    Checkpoint result;
    Fingerprint& f = result.fingerprint;
    std::uint64_t version = 0, processing = 0, complete = 0, task_count = 0;
    std::string elapsed;
    if (!number("lpr-checkpoint", version)) return false;
    if (version != FORMAT_VERSION) {
        error = "unsupported checkpoint format version " + std::to_string(version);
        return false;
    }
    if (!text_value("commit", f.commit) || !number("omega", f.omega) || !number("s", f.s)
        || !number("prime_limit", f.prime_limit) || !number("num_primes", f.num_primes)
        || !text_value("bound_hex", f.bound_hex)
        || !number("primitive_root_search_limit", f.primitive_root_search_limit)
        || !number("trial_division_min", f.trial_division_min)
        || !number("trial_division_max", f.trial_division_max)
        || !number("processing", processing) || !number("segments", result.segments)
        || !text_value("elapsed_seconds", elapsed) || !number("complete", complete)) {
        return false;
    }
    if (processing > 1 || complete > 1) {
        error = "bad flag value";
        return false;
    }
    f.processing = processing == 1;
    result.complete = complete == 1;
    {
        char* end = nullptr;
        result.elapsed_seconds = std::strtod(elapsed.c_str(), &end);
        if (end == elapsed.c_str() || *end != '\0' || !(result.elapsed_seconds >= 0)) {
            error = "bad elapsed_seconds";
            return false;
        }
    }

    bool stats_ok = true;
    stats::Counters::for_each_field(result.stats, [&](const std::string& name, std::uint64_t& field) {
        if (!stats_ok) return;
        if (!next("stat", v)) {
            stats_ok = false;
            return;
        }
        if (v.size() != 2 || v[0] != name || !detail::parse_u64(v[1], field)) {
            error = "line " + std::to_string(line_number) + ": expected statistic '" + name + "'";
            stats_ok = false;
        }
    });
    if (!stats_ok) return false;

    if (!number("tasks", task_count)) return false;
    for (std::uint64_t k = 0; k < task_count; ++k) {
        if (!next("task", v)) return false;
        producer::detail::Task t;
        std::uint64_t base_depth = 0, base_limit = 0, depth = 0;
        if (v.size() < 3 || !detail::parse_u64(v[0], base_depth) || !detail::parse_u64(v[1], base_limit)
            || !detail::parse_u64(v[2], depth) || depth > parameters::MAX_OMEGA || v.size() != 3 + depth + 1) {
            error = "line " + std::to_string(line_number) + ": malformed task";
            return false;
        }
        t.base_depth = base_depth;
        t.base_limit = base_limit;
        t.depth = depth;
        for (std::size_t j = 3; j < v.size(); ++j) {
            std::uint64_t idx = 0;
            if (!detail::parse_u64(v[j], idx)) {
                error = "line " + std::to_string(line_number) + ": malformed task index";
                return false;
            }
            t.path.push_back(idx);
        }
        result.tasks.push_back(std::move(t));
    }
    if (std::getline(in, line)) {
        error = "unexpected line after the tasks: '" + line + "'";
        return false;
    }
    if (result.complete != result.tasks.empty()) {
        error = result.complete ? "a complete checkpoint lists tasks" : "an incomplete checkpoint lists no tasks";
        return false;
    }
    ck = std::move(result);
    return true;
}

/*
 * Differences between the fingerprint of a checkpoint and of the current run,
 * one per line (empty if they agree). The commit is reported separately in
 * *commit_differs, as --resume-any-commit may accept it.
 */
inline std::string compare(const Fingerprint& saved, const Fingerprint& now, bool* commit_differs) {
    std::ostringstream out;
    auto check = [&](const char* name, const auto& a, const auto& b) {
        if (!(a == b)) out << "  " << name << ": checkpoint " << a << ", this run " << b << '\n';
    };
    check("omega", saved.omega, now.omega);
    check("s", saved.s, now.s);
    check("prime limit", saved.prime_limit, now.prime_limit);
    check("number of primes", saved.num_primes, now.num_primes);
    check("search bound B (hex)", saved.bound_hex, now.bound_hex);
    check("primitive root search limit", saved.primitive_root_search_limit, now.primitive_root_search_limit);
    check("trial division min prime", saved.trial_division_min, now.trial_division_min);
    check("trial division max prime", saved.trial_division_max, now.trial_division_max);
    if (saved.processing != now.processing) {
        out << "  mode: checkpoint is " << (saved.processing ? "a full run" : "a dry run")
            << ", this run is " << (now.processing ? "a full run" : "a dry run") << '\n';
    }
    *commit_differs = saved.commit != now.commit;
    return out.str();
}

inline bool read_file(const std::string& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        error = "cannot read '" + path + "'";
        return false;
    }
    text = buffer.str();
    return true;
}

inline bool load(const std::string& path, Checkpoint& ck, std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) return false;
    if (!parse(text, ck, error)) {
        error = "'" + path + "': " + error;
        return false;
    }
    return true;
}

/*
 * Writes `ck` to `path` atomically: to path + ".tmp", fsync, read back and
 * check that it parses to the same checkpoint, rename over `path`, then fsync
 * the directory. On failure `path` is untouched (a previous checkpoint stays valid).
 */
inline bool write_atomic(const std::string& path, const Checkpoint& ck, std::string& error) {
    const std::string text = serialize(ck);
    const std::string tmp = path + ".tmp";
    auto os_error = [&](const std::string& what) {
        error = what + " '" + tmp + "': " + std::strerror(errno);
        return false;
    };
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return os_error("cannot create");
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            ::close(fd);
            errno = saved;
            return os_error("cannot write");
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return os_error("cannot fsync");
    }
    if (::close(fd) != 0) return os_error("cannot close");

    // Read back and compare before replacing the previous checkpoint
    std::string back;
    Checkpoint parsed;
    if (!read_file(tmp, back, error)) return false;
    if (back != text || !parse(back, parsed, error) || serialize(parsed) != text) {
        if (error.empty()) error = "read-back of '" + tmp + "' differs from what was written";
        return false;
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot rename '" + tmp + "' to '" + path + "': " + std::strerror(errno);
        return false;
    }
    const std::size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const int dir_fd = ::open(dir.c_str(), O_RDONLY);
    if (dir_fd >= 0) { // Best effort: makes the rename durable
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return true;
}

} // namespace lpr::checkpoint
