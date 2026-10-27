/**
 * pool.hpp: one search shared by several independent jobs through a directory on
 * a shared file system (lpr_mpi --pool DIR; design and invariants in
 * docs/elastic_pool.md).
 *
 * Each job is one MPI world (1..n nodes) running the usual MPI search; its rank 0
 * also takes part in the pool. Jobs may start at any time (they join) and may die
 * at any time (a node failure): no work is lost beyond a job's last lease state,
 * and no work is ever counted twice.
 *
 * Layout of DIR:
 *   SEARCH            marker: the pool exists (written last by the job that creates it)
 *   ready/<id>        unclaimed tasks, one per file (checkpoint format, one task, zero
 *                     statistics); .tmp-<id> files are tasks not yet published
 *   leases/<L>/       one per running job: state (a lease record), heartbeat, claimed/<id>
 *   done/<L>          the final record of each finished lease (done/INIT: the root)
 *   requests/<L>      a job with nothing to do asks for work
 *
 * A lease record lists the task ids the lease claimed and the ids it donated (gave
 * away as new ready tasks), plus a checkpoint (checkpoint.hpp format: fingerprint,
 * statistics, remaining tasks). It is written atomically. The rules:
 *  - claim: rename ready/X -> leases/L/claimed/X (atomic: one job wins), then the
 *    next state write records "claimed X". A claimed file not yet recorded goes back
 *    to ready/ on release.
 *  - donate: write ready/.tmp-X, record "donated X" in a state write (which also
 *    drops the task from the lease's remaining work), then rename it to ready/X.
 *    Release publishes recorded .tmp files and deletes unrecorded ones.
 *  - release (a job exiting, or any job releasing a stale lease): rename the lease
 *    directory to leases/.releasing-<L> (atomic: one releaser wins; a live owner whose
 *    directory is gone stops without writing), republish the remaining tasks of its
 *    last state as new ids (donated), write done/<L>, publish, remove the directory.
 *  - complete: no ready tasks, no leases, and every donated id (over all done
 *    records) claimed by exactly one done record. Only then are the done records'
 *    statistics merged into the whole search's summary.
 *
 * Only rank 0 of each job touches the directory (from its communication thread).
 **/

#pragma once

#include "checkpoint.hpp"
#include "generate_supports.hpp"
#include "stats.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gmpxx.h>

namespace lpr::pool {

using Task = producer::detail::Task;

// A lease's record: what it claimed and donated, and its checkpoint
struct LeaseRecord {
    std::string lease;
    std::vector<std::string> claimed;
    std::vector<std::string> donated;
    checkpoint::Checkpoint ck;
};

namespace detail {

inline bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

inline double age_seconds(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return std::difftime(std::time(nullptr), st.st_mtime);
}

// A task not yet published (.tmp-<id>) or a file being written (<name>.tmp)
inline bool is_temporary(const std::string& name) {
    return name.compare(0, 5, ".tmp-") == 0 || (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0);
}

inline bool make_dir(const std::string& path) {
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// Entries of a directory (without . and ..), sorted; empty if it cannot be read
inline std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> names;
    DIR* d = ::opendir(path.c_str());
    if (d == nullptr) return names;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

inline void fsync_dir(const std::string& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

inline std::string dir_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
}

// Writes `text` to `path` atomically (path.tmp, fsync, rename, fsync of the directory)
inline bool write_text_atomic(const std::string& path, const std::string& text, std::string& error) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = "cannot create '" + tmp + "': " + std::strerror(errno);
        return false;
    }
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = "cannot write '" + tmp + "': " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        error = "cannot sync '" + tmp + "': " + std::strerror(errno);
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot rename '" + tmp + "' to '" + path + "': " + std::strerror(errno);
        return false;
    }
    fsync_dir(dir_of(path));
    return true;
}

inline bool rename_path(const std::string& from, const std::string& to) {
    return std::rename(from.c_str(), to.c_str()) == 0;
}

inline void remove_tree(const std::string& path) {
    for (const std::string& name : list_dir(path)) {
        const std::string child = path + "/" + name;
        struct stat st{};
        if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            ::unlink(child.c_str());
        }
    }
    ::rmdir(path.c_str());
}

} // namespace detail

// A lease record as text: header lines, then the embedded checkpoint, then a checksum
inline std::string serialize_record(const LeaseRecord& r) {
    std::ostringstream out;
    out << "lpr-lease 1\n" << "lease " << r.lease << '\n' << "claimed " << r.claimed.size() << '\n';
    for (const std::string& id : r.claimed) out << "id " << id << '\n';
    out << "donated " << r.donated.size() << '\n';
    for (const std::string& id : r.donated) out << "id " << id << '\n';
    out << "checkpoint\n" << checkpoint::serialize(r.ck);
    const std::string body = out.str();
    return body + "record-checksum " + checkpoint::detail::hex64(checkpoint::detail::fnv1a(body)) + '\n';
}

inline bool parse_record(const std::string& text, LeaseRecord& r, std::string& error) {
    const std::string marker = "record-checksum ";
    const std::size_t pos = text.rfind(marker);
    if (pos == std::string::npos || (pos != 0 && text[pos - 1] != '\n')) {
        error = "no record checksum (file truncated?)";
        return false;
    }
    const std::string body = text.substr(0, pos);
    std::string stored = text.substr(pos + marker.size());
    if (!stored.empty() && stored.back() == '\n') stored.pop_back();
    if (stored != checkpoint::detail::hex64(checkpoint::detail::fnv1a(body))) {
        error = "record checksum mismatch (file damaged or edited)";
        return false;
    }
    std::istringstream in(body);
    std::string line;
    auto expect = [&](const std::string& key, std::string& value) -> bool {
        if (!std::getline(in, line)) {
            error = "missing '" + key + "'";
            return false;
        }
        const std::size_t sp = line.find(' ');
        const std::string k = line.substr(0, sp);
        if (k != key) {
            error = "expected '" + key + "', got '" + line + "'";
            return false;
        }
        value = sp == std::string::npos ? "" : line.substr(sp + 1);
        return true;
    };
    std::string v;
    if (!expect("lpr-lease", v) || v != "1") {
        if (error.empty()) error = "unsupported lease record version";
        return false;
    }
    LeaseRecord result;
    if (!expect("lease", result.lease)) return false;
    for (std::vector<std::string>* list : {&result.claimed, &result.donated}) {
        if (!expect(list == &result.claimed ? "claimed" : "donated", v)) return false;
        std::uint64_t n = 0;
        if (!checkpoint::detail::parse_u64(v, n)) {
            error = "bad id count";
            return false;
        }
        for (std::uint64_t k = 0; k < n; ++k) {
            if (!expect("id", v) || v.empty()) {
                if (error.empty()) error = "empty id";
                return false;
            }
            list->push_back(v);
        }
    }
    if (!expect("checkpoint", v)) return false;
    std::ostringstream rest;
    rest << in.rdbuf();
    if (!checkpoint::parse(rest.str(), result.ck, error)) {
        error = "embedded checkpoint: " + error;
        return false;
    }
    r = std::move(result);
    return true;
}

inline bool load_record(const std::string& path, LeaseRecord& r, std::string& error) {
    std::string text;
    if (!checkpoint::read_file(path, text, error)) return false;
    if (!parse_record(text, r, error)) {
        error = "'" + path + "': " + error;
        return false;
    }
    return true;
}

// Statistics for subtracting a base from cumulative counters: sums are subtracted
// (the checksum wraps), maxima are kept (the merged maximum is still the true one)
inline stats::Counters subtract(stats::Counters cumulative, stats::Counters base) {
    std::vector<std::uint64_t> b;
    stats::Counters::for_each_field(base, [&](const std::string&, std::uint64_t& v) { b.push_back(v); });
    std::size_t i = 0;
    stats::Counters::for_each_field(cumulative, [&](const std::string& name, std::uint64_t& v) {
        const std::uint64_t base_value = b[i++];
        if (name == "largest_least_primitive_root" || name == "d_max_max") return;
        v -= base_value;
    });
    return cumulative;
}

// The state of a pool directory, for --pool-status and for completion checks
struct Status {
    std::size_t ready = 0;
    std::size_t leases = 0;        // Active (including ones being released)
    std::size_t stale_leases = 0;
    std::size_t done = 0;
    std::size_t requests = 0;
    std::uint64_t leases_remaining_tasks = 0; // Over the active leases' last states
    bool complete = false;         // Every donated task claimed exactly once, nothing left
    std::string problem;           // Why the accounting does not (yet) close
    stats::Counters merged;        // Sum over done records (the whole search when complete)
    std::uint64_t processed_in_leases = 0; // Supports processed by active leases (last states)
};

struct Options {
    double poll_seconds = 10;      // How often rank 0 looks at the directory
    double heartbeat_seconds = 30;
    double stale_seconds = 600;    // A lease without a heartbeat for this long is released
    double idle_timeout_seconds = 1800; // A job with nothing to do for this long leaves
};

class Pool {
public:
    Pool(std::string dir, checkpoint::Fingerprint fingerprint, Options options)
        : dir_(std::move(dir)), fingerprint_(std::move(fingerprint)), options_(options) {}

    const std::string& dir() const { return dir_; }
    const std::string& lease() const { return lease_; }
    const Options& options() const { return options_; }

    /*
     * Opens the pool, creating it if it does not exist: done/INIT donates the root
     * task, or the given seed tasks (with seed statistics, e.g. from --resume).
     * Checks the fingerprint against done/INIT. Waits up to a minute for another job
     * that is creating it.
     */
    bool open(const std::vector<Task>& seed, const stats::Counters& seed_stats, bool any_commit, std::string& error) {
        if (!detail::make_dir(dir_)) {
            error = "cannot create pool directory '" + dir_ + "': " + std::strerror(errno);
            return false;
        }
        if (::mkdir((dir_ + "/init.lock").c_str(), 0755) == 0) { // This job creates the pool
            for (const char* sub : {"ready", "leases", "done", "requests"}) {
                if (!detail::make_dir(dir_ + "/" + sub)) {
                    error = "cannot create '" + dir_ + "/" + sub + "'";
                    return false;
                }
            }
            LeaseRecord init;
            init.lease = "INIT";
            init.ck.fingerprint = fingerprint_;
            init.ck.stats = seed_stats;
            init.ck.complete = true;
            for (std::size_t k = 0; k < seed.size(); ++k) {
                const std::string id = "INIT." + std::to_string(k);
                if (!write_task(dir_ + "/ready/.tmp-" + id, seed[k], error)) return false;
                init.donated.push_back(id);
            }
            if (!detail::write_text_atomic(dir_ + "/done/INIT", serialize_record(init), error)) return false;
            for (const std::string& id : init.donated) {
                detail::rename_path(dir_ + "/ready/.tmp-" + id, dir_ + "/ready/" + id);
            }
            if (!detail::write_text_atomic(dir_ + "/SEARCH", "lpr-pool 1\n", error)) return false;
            created_ = true;
        } else {
            for (int waited = 0; !detail::exists(dir_ + "/SEARCH"); ++waited) {
                if (waited >= 600) {
                    error = "pool '" + dir_ + "' exists but was never completed (no SEARCH after 60 s; remove init.lock?)";
                    return false;
                }
                ::usleep(100000);
            }
        }
        LeaseRecord init;
        if (!load_record(dir_ + "/done/INIT", init, error)) return false;
        bool commit_differs = false;
        const std::string differences = checkpoint::compare(init.ck.fingerprint, fingerprint_, &commit_differs);
        if (!differences.empty()) {
            error = "pool '" + dir_ + "' belongs to a different search:\n" + differences;
            return false;
        }
        if (commit_differs && !any_commit) {
            error = "pool '" + dir_ + "' was created by commit " + init.ck.fingerprint.commit + ", this is "
                    + fingerprint_.commit + " (pass --resume-any-commit to join it anyway)";
            return false;
        }
        return true;
    }

    bool created() const { return created_; }

    // Starts this job's lease (an empty state, claimed nothing)
    bool begin_lease(const std::string& id, std::string& error) {
        lease_ = id;
        const std::string ld = lease_dir();
        if (::mkdir(ld.c_str(), 0755) != 0 || !detail::make_dir(ld + "/claimed")) {
            error = "cannot create lease '" + ld + "': " + std::strerror(errno);
            return false;
        }
        if (!touch_heartbeat(error)) return false;
        return write_state({}, stats::Counters{}, error);
    }

    /*
     * Claims up to `max` ready tasks into this lease (each checked by `check`, which
     * must replay the task). The claims are recorded by the next write_state.
     */
    template <typename Check>
    std::vector<Task> claim(std::size_t max, Check&& check, std::string& error) {
        std::vector<Task> tasks;
        for (const std::string& name : detail::list_dir(dir_ + "/ready")) {
            if (tasks.size() >= max) break;
            if (detail::is_temporary(name)) continue;
            const std::string to = lease_dir() + "/claimed/" + name;
            if (!detail::rename_path(dir_ + "/ready/" + name, to)) continue; // Another job got it
            checkpoint::Checkpoint ck;
            if (!checkpoint::load(to, ck, error) || ck.tasks.size() != 1 || !check(ck.tasks[0], error)) {
                // A damaged task file must not be lost silently: stop (the claim is undone on release)
                error = "claimed task '" + name + "' is invalid: " + error;
                return tasks;
            }
            claimed_.push_back(name);
            tasks.push_back(ck.tasks[0]);
            ++claims_;
        }
        return tasks;
    }

    // Writes a task, to be published after the next state write, as donated by this lease
    bool prepare_donation(const Task& task, std::string& error) {
        const std::string id = lease_ + "." + std::to_string(next_id_++);
        if (!write_task(dir_ + "/ready/.tmp-" + id, task, error)) return false;
        donated_.push_back(id);
        unpublished_.push_back(id);
        ++donations_;
        return true;
    }

    /*
     * Writes this lease's state: remaining tasks (the lease's work not yet done, not
     * including tasks prepared for donation) and statistics, with every claim and
     * donation so far. Then publishes the donations prepared before it. False (and
     * nothing published) if the lease was taken away (released as stale): the caller
     * must stop without writing anything else.
     */
    bool write_state(const std::vector<Task>& remaining, const stats::Counters& stats, std::string& error) {
        if (!detail::exists(lease_dir())) {
            error = "this job's lease '" + lease_ + "' is gone (released as stale by another job)";
            return false;
        }
        LeaseRecord r;
        r.lease = lease_;
        r.claimed = claimed_;
        r.donated = donated_;
        r.ck.fingerprint = fingerprint_;
        r.ck.stats = stats;
        r.ck.tasks = remaining;
        r.ck.complete = remaining.empty();
        r.ck.segments = 1;
        if (!detail::write_text_atomic(lease_dir() + "/state", serialize_record(r), error)) return false;
        for (const std::string& id : unpublished_) {
            detail::rename_path(dir_ + "/ready/.tmp-" + id, dir_ + "/ready/" + id);
        }
        unpublished_.clear();
        remaining_tasks_ = remaining.size();
        return true;
    }

    /*
     * Ends this job's lease: its remaining tasks (as of its last state) are republished
     * for other jobs, and done/<lease> records its claims, donations and statistics.
     * Call after a final write_state. False if the lease was taken away.
     */
    bool release_own(std::string& error) {
        const std::string releasing = dir_ + "/leases/.releasing-" + lease_;
        if (!detail::rename_path(lease_dir(), releasing)) {
            error = "this job's lease '" + lease_ + "' is gone (released as stale by another job)";
            return false;
        }
        return finish_release(releasing, error);
    }

    bool touch_heartbeat(std::string& error) {
        if (!detail::exists(lease_dir())) {
            error = "this job's lease '" + lease_ + "' is gone (released as stale by another job)";
            return false;
        }
        return detail::write_text_atomic(lease_dir() + "/heartbeat", std::to_string(std::time(nullptr)) + "\n", error);
    }

    // Asks other jobs for work (or withdraws the request)
    void set_request(bool on) {
        const std::string path = dir_ + "/requests/" + lease_;
        if (on && !requesting_) {
            std::string error;
            detail::write_text_atomic(path, "1\n", error);
        } else if (!on && requesting_) {
            ::unlink(path.c_str());
        }
        requesting_ = on;
    }

    std::size_t ready_count() const {
        std::size_t n = 0;
        for (const std::string& name : detail::list_dir(dir_ + "/ready")) {
            if (!detail::is_temporary(name)) ++n;
        }
        return n;
    }

    // Requests from other jobs (not this one)
    std::size_t other_requests() const {
        std::size_t n = 0;
        for (const std::string& name : detail::list_dir(dir_ + "/requests")) {
            if (name != lease_ && name.find(".tmp") == std::string::npos) ++n;
        }
        return n;
    }

    /*
     * Releases leases whose heartbeat is older than the stale limit (their job is
     * presumed dead), and finishes releases that a job began but did not complete.
     * Returns how many were released.
     */
    std::size_t release_stale(std::string& log) {
        std::size_t released = 0;
        for (const std::string& name : detail::list_dir(dir_ + "/leases")) {
            if (name == lease_) continue;
            const std::string path = dir_ + "/leases/" + name;
            if (name.compare(0, 11, ".releasing-") == 0) {
                // A release in progress: finish it if it has been abandoned
                if (detail::age_seconds(path) > options_.stale_seconds) {
                    std::string error;
                    const std::string mine = path + ".by-" + lease_;
                    if (detail::rename_path(path, mine) && finish_release(mine, error)) ++released;
                    log += "finished an abandoned release of " + name + (error.empty() ? "" : " (" + error + ")") + "; ";
                }
                continue;
            }
            double age = detail::age_seconds(path + "/heartbeat");
            if (age < 0) age = detail::age_seconds(path);
            if (age < options_.stale_seconds) continue;
            const std::string releasing = dir_ + "/leases/.releasing-" + name + ".by-" + lease_;
            if (!detail::rename_path(path, releasing)) continue; // Another job is releasing it
            std::string error;
            if (finish_release(releasing, error)) {
                ++released;
                log += "released stale lease " + name + " (no heartbeat for " + std::to_string(static_cast<long>(age))
                       + " s); ";
            } else {
                log += "could not release " + name + ": " + error + "; ";
            }
        }
        return released;
    }

    /*
     * The pool's state. With `merge`, also checks the accounting and sums the done
     * records' statistics (Status::complete: every donated id claimed exactly once,
     * nothing left; then merged is the whole search's statistics).
     */
    Status status(bool merge) const {
        Status s;
        s.ready = ready_count();
        s.requests = detail::list_dir(dir_ + "/requests").size();
        for (const std::string& name : detail::list_dir(dir_ + "/leases")) {
            ++s.leases;
            const std::string path = dir_ + "/leases/" + name;
            double age = detail::age_seconds(path + "/heartbeat");
            if (age < 0) age = detail::age_seconds(path);
            if (age >= options_.stale_seconds) ++s.stale_leases;
            LeaseRecord r;
            std::string error;
            if (load_record(path + "/state", r, error)) {
                s.leases_remaining_tasks += r.ck.tasks.size();
                s.processed_in_leases += r.ck.stats.total_support_count;
            }
        }
        const std::vector<std::string> done = detail::list_dir(dir_ + "/done");
        if (!merge) {
            for (const std::string& name : done) {
                if (name.find(".tmp") == std::string::npos) ++s.done;
            }
            return s;
        }
        std::map<std::string, int> donated, claimed;
        for (const std::string& name : done) {
            if (name.find(".tmp") != std::string::npos) continue;
            ++s.done;
            LeaseRecord r;
            std::string error;
            if (!load_record(dir_ + "/done/" + name, r, error)) {
                s.problem = "unreadable done record: " + error;
                return s;
            }
            bool differs = false;
            if (!checkpoint::compare(r.ck.fingerprint, fingerprint_, &differs).empty()) {
                s.problem = "done record " + name + " belongs to a different search";
                return s;
            }
            if (!r.ck.tasks.empty()) {
                s.problem = "done record " + name + " still lists tasks";
                return s;
            }
            for (const std::string& id : r.donated) ++donated[id];
            for (const std::string& id : r.claimed) ++claimed[id];
            s.merged.merge(r.ck.stats);
        }
        if (s.ready > 0 || s.leases > 0) {
            s.problem = std::to_string(s.ready) + " ready tasks and " + std::to_string(s.leases) + " active leases";
            return s;
        }
        for (const auto& [id, n] : donated) {
            if (n != 1) { s.problem = "task " + id + " donated " + std::to_string(n) + " times"; return s; }
            const auto it = claimed.find(id);
            if (it == claimed.end()) { s.problem = "task " + id + " was never finished"; return s; }
            if (it->second != 1) { s.problem = "task " + id + " claimed " + std::to_string(it->second) + " times"; return s; }
        }
        for (const auto& [id, n] : claimed) {
            if (donated.find(id) == donated.end()) { s.problem = "task " + id + " claimed but never created"; return s; }
            (void)n;
        }
        s.complete = true;
        return s;
    }

    /*
     * Other leases that may still produce work for this job: their count, and the
     * remaining tasks in their last states. A lease being released counts as busy (its
     * tasks are about to appear in ready/).
     */
    std::size_t others_busy(std::uint64_t& remaining) const {
        std::size_t busy = 0;
        remaining = 0;
        for (const std::string& name : detail::list_dir(dir_ + "/leases")) {
            if (name == lease_) continue;
            const std::string path = dir_ + "/leases/" + name;
            if (name.compare(0, 11, ".releasing-") == 0) {
                ++busy;
                continue;
            }
            LeaseRecord r;
            std::string error;
            if (!load_record(path + "/state", r, error) || !r.ck.tasks.empty()) {
                ++busy; // (Unreadable while being written: assume busy)
                if (error.empty()) remaining += r.ck.tasks.size();
            }
        }
        return busy;
    }

    std::size_t claims() const { return claims_; }
    std::size_t donations() const { return donations_; }

private:
    std::string dir_;
    checkpoint::Fingerprint fingerprint_;
    Options options_;
    bool created_ = false;
    std::string lease_;
    std::vector<std::string> claimed_;
    std::vector<std::string> donated_;
    std::vector<std::string> unpublished_; // Prepared donations, published after the next state write
    std::uint64_t next_id_ = 0;
    std::size_t remaining_tasks_ = 0;
    bool requesting_ = false;
    std::size_t claims_ = 0, donations_ = 0;

    std::string lease_dir() const { return dir_ + "/leases/" + lease_; }

    bool write_task(const std::string& path, const Task& task, std::string& error) const {
        checkpoint::Checkpoint ck;
        ck.fingerprint = fingerprint_;
        ck.tasks = {task};
        ck.complete = false;
        ck.segments = 0;
        return detail::write_text_atomic(path, checkpoint::serialize(ck), error);
    }

    /*
     * Completes the release of a lease directory already renamed by this job: undo
     * unrecorded claims, publish recorded donations, republish its remaining tasks,
     * write done/<lease>, remove the directory.
     */
    bool finish_release(const std::string& path, std::string& error) {
        LeaseRecord r;
        // A release that already wrote its done record (and then stopped) only has its
        // republished tasks left to publish: never write a second record
        {
            LeaseRecord state, done;
            std::string ignored;
            if (load_record(path + "/state", state, ignored) && detail::exists(dir_ + "/done/" + state.lease)
                && load_record(dir_ + "/done/" + state.lease, done, error)) {
                for (const std::string& id : done.donated) {
                    if (detail::exists(dir_ + "/ready/.tmp-" + id)) {
                        detail::rename_path(dir_ + "/ready/.tmp-" + id, dir_ + "/ready/" + id);
                    }
                }
                ::unlink((dir_ + "/requests/" + state.lease).c_str());
                detail::remove_tree(path);
                return true;
            }
        }
        if (!load_record(path + "/state", r, error)) {
            // Without a readable state nothing of this lease can be accounted for: leave it
            // for inspection (the pool will not report completion)
            error = "lease state unreadable: " + error;
            return false;
        }
        const std::set<std::string> recorded(r.claimed.begin(), r.claimed.end());
        for (const std::string& name : detail::list_dir(path + "/claimed")) {
            if (recorded.count(name) == 0) { // Claimed but never recorded: back to ready/
                detail::rename_path(path + "/claimed/" + name, dir_ + "/ready/" + name);
            }
        }
        // Donations recorded but not yet published; delete prepared-but-unrecorded ones
        const std::set<std::string> donated(r.donated.begin(), r.donated.end());
        for (const std::string& name : detail::list_dir(dir_ + "/ready")) {
            if (name.compare(0, 5, ".tmp-") != 0) continue;
            const std::string id = name.substr(5);
            if (id.compare(0, r.lease.size() + 1, r.lease + ".") != 0) continue;
            if (donated.count(id) != 0) {
                detail::rename_path(dir_ + "/ready/" + name, dir_ + "/ready/" + id);
            } else {
                ::unlink((dir_ + "/ready/" + name).c_str());
            }
        }
        // Republish the remaining tasks as new donated ids
        std::uint64_t next = 0;
        for (const std::string& id : r.donated) { // Continue after the lease's own ids
            const std::size_t dot = id.rfind('.');
            std::uint64_t n = 0;
            if (dot != std::string::npos && checkpoint::detail::parse_u64(id.substr(dot + 1), n)) {
                next = std::max(next, n + 1);
            }
        }
        std::vector<std::string> fresh;
        for (const Task& t : r.ck.tasks) {
            const std::string id = r.lease + "." + std::to_string(next++);
            if (!write_task(dir_ + "/ready/.tmp-" + id, t, error)) return false;
            fresh.push_back(id);
        }
        LeaseRecord done = r;
        done.donated.insert(done.donated.end(), fresh.begin(), fresh.end());
        done.ck.tasks.clear();
        done.ck.complete = true;
        if (!detail::write_text_atomic(dir_ + "/done/" + r.lease, serialize_record(done), error)) return false;
        for (const std::string& id : fresh) {
            detail::rename_path(dir_ + "/ready/.tmp-" + id, dir_ + "/ready/" + id);
        }
        ::unlink((dir_ + "/requests/" + r.lease).c_str());
        detail::remove_tree(path);
        return true;
    }
};

} // namespace lpr::pool
