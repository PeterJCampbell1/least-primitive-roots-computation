/**
 * config.hpp: runtime search parameters (OMEGA and S).
 *
 * OMEGA and S are chosen at runtime, from (in increasing priority) the default
 * parameters::DEFAULT_OMEGA, a config file (--config PATH) and the command line
 * (--omega, --s). S defaults to OMEGA - 3. The values are fixed in main before
 * any worker thread starts and are read-only afterwards, so reads need no
 * synchronisation.
 *
 * Config file format: one "key = value" per line, '#' starts a comment.
 * Keys: omega, s. Unknown or repeated keys are errors.
 **/

#pragma once

#include "parameters.hpp"

#include <cstddef>
#include <fstream>
#include <optional>
#include <string>

namespace lpr::config {

struct SearchConfig {
    std::size_t omega = parameters::DEFAULT_OMEGA; // Number of distinct primes in each support
    std::size_t s = parameters::DEFAULT_OMEGA - 3; // Corollary 4 parameter (s largest primes)
    std::string omega_source = "default";
    std::string s_source = "default (omega - 3)";

    // Depth from which the Corollary 4 window (the last s primes) starts
    std::size_t delta_start_depth() const { return omega - s; }
    // Nodes in the product tree over the omega - 1 odd primes of a support
    std::size_t tree_size() const { return 2 * (omega - 1) - 1; }
};

namespace detail {
inline SearchConfig& instance() {
    static SearchConfig config;
    return config;
}
} // namespace detail

// The active configuration (set once in main via set_search_config)
inline const SearchConfig& search_config() {
    return detail::instance();
}

inline void set_search_config(const SearchConfig& config) {
    detail::instance() = config;
}

// Values requested from one source (file or command line); unset means "not given"
struct Overrides {
    std::optional<std::size_t> omega;
    std::optional<std::size_t> s;
};

// Strict unsigned parse: digits only
inline std::optional<std::size_t> parse_size(const std::string& text) {
    if (text.empty() || text.size() > 9) return std::nullopt;
    std::size_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Reads a config file. Returns an error message, or an empty string on success.
inline std::string read_config_file(const std::string& path, Overrides& out) {
    std::ifstream file(path);
    if (!file) return "cannot open config file '" + path + "'";

    std::string line;
    for (std::size_t line_no = 1; std::getline(file, line); ++line_no) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        const std::string where = path + ":" + std::to_string(line_no) + ": ";
        const auto eq = line.find('=');
        if (eq == std::string::npos) return where + "expected 'key = value'";
        const std::string key = trim(line.substr(0, eq));
        const auto value = parse_size(trim(line.substr(eq + 1)));
        if (!value) return where + "value for '" + key + "' must be a non-negative integer";

        std::optional<std::size_t>* target = nullptr;
        if (key == "omega") target = &out.omega;
        else if (key == "s") target = &out.s;
        else return where + "unknown key '" + key + "' (expected omega or s)";

        if (*target) return where + "'" + key + "' is given more than once";
        *target = *value;
    }
    return "";
}

/*
 * Combines the sources (command line over file over default; s defaults to
 * omega - 3) and validates the result. Returns an error message, or an empty
 * string on success with `out` filled in.
 */
inline std::string resolve(
    const Overrides& file, const std::string& file_path,
    const Overrides& cli,
    SearchConfig& out
) {
    SearchConfig config;
    if (file.omega) { config.omega = *file.omega; config.omega_source = "config " + file_path; }
    if (cli.omega) { config.omega = *cli.omega; config.omega_source = "--omega"; }

    if (cli.s) {
        config.s = *cli.s;
        config.s_source = "--s";
    } else if (file.s) {
        config.s = *file.s;
        config.s_source = "config " + file_path;
    } else {
        if (config.omega < 3) return "omega must be at least 3 (got " + std::to_string(config.omega) + ")";
        config.s = config.omega - 3;
        config.s_source = "default (omega - 3)";
    }

    if (config.omega < 3 || config.omega > parameters::MAX_OMEGA) {
        return "omega must be between 3 and " + std::to_string(parameters::MAX_OMEGA)
               + " (got " + std::to_string(config.omega) + ")";
    }
    // Corollary 4 is applied with s <= omega - 3, so the omega - s excluded primes
    // include 2 (the even divisor e). The DFS also needs s >= 1.
    if (config.s < 1 || config.s > config.omega - 3) {
        return "s must be between 1 and omega - 3 = " + std::to_string(config.omega - 3)
               + " (got " + std::to_string(config.s) + ")";
    }

    out = config;
    return "";
}

} // namespace lpr::config
