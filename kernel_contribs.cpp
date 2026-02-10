#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static const char* OFFICIAL_REPO_URL = "https://github.com/torvalds/linux.git";
static bool g_verbose = false;

static void log_info(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "INFO: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void log_debug(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "DEBUG: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// ── run_cmd ──────────────────────────────────────────────────────────
static std::string run_cmd(const std::string& cmd) {
    log_debug("Running command: %s", cmd.c_str());
    std::string full_cmd = cmd + " 2>/dev/null";
    FILE* fp = popen(full_cmd.c_str(), "r");
    if (!fp) {
        throw std::runtime_error("popen failed: " + cmd);
    }
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        output += buf;
    }
    int status = pclose(fp);
    if (status != 0) {
        throw std::runtime_error("Command failed (exit " + std::to_string(status) + "): " + cmd);
    }
    return output;
}

// shell-escape a single argument
static std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string build_git_cmd(const std::vector<std::string>& args,
                                  const std::string& git_dir) {
    std::string cmd = "git";
    if (!git_dir.empty()) {
        cmd += " --git-dir " + shell_escape(git_dir);
    }
    for (auto& a : args) {
        cmd += " " + shell_escape(a);
    }
    return cmd;
}

// ── ensure_repo ──────────────────────────────────────────────────────
static void ensure_repo(const std::string& repo_path, bool refresh) {
    fs::path dot_git = fs::path(repo_path) / ".git";
    if (fs::is_directory(dot_git) || fs::is_regular_file(dot_git)) {
        if (refresh) {
            log_info("Fetching updates in %s", repo_path.c_str());
            std::string cmd = "cd " + shell_escape(repo_path)
                + " && git fetch --all --tags --prune";
            run_cmd(cmd);
        }
        return;
    }
    fs::path parent = fs::path(repo_path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    log_info("Cloning official Linux repo to %s", repo_path.c_str());
    std::string cmd = "git clone --no-tags --filter=blob:none "
        + shell_escape(OFFICIAL_REPO_URL) + " " + shell_escape(repo_path);
    run_cmd(cmd);
}

// ── resolve_git_dir ──────────────────────────────────────────────────
static std::string resolve_git_dir(const std::string& git_file) {
    if (fs::is_directory(git_file)) return git_file;
    if (fs::is_regular_file(git_file)) {
        std::ifstream f(git_file);
        std::string data((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        // trim
        while (!data.empty() && (data.back() == '\n' || data.back() == '\r' || data.back() == ' '))
            data.pop_back();
        if (data.rfind("gitdir:", 0) == 0) {
            std::string raw = data.substr(7);
            // trim leading spaces
            size_t start = raw.find_first_not_of(' ');
            if (start != std::string::npos) raw = raw.substr(start);
            if (raw.empty() || raw[0] != '/') {
                raw = (fs::path(git_file).parent_path() / raw).string();
            }
            if (fs::is_directory(raw)) return raw;
        }
    }
    throw std::runtime_error("Invalid git file or directory: " + git_file);
}

// ── email_to_org ─────────────────────────────────────────────────────
static std::string email_to_org(const std::string& email) {
    std::string e = email;
    // trim
    while (!e.empty() && (e.back() == ' ' || e.back() == '\n' || e.back() == '\r'))
        e.pop_back();
    size_t start = e.find_first_not_of(' ');
    if (start != std::string::npos) e = e.substr(start);
    // lowercase
    for (auto& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto at = e.find('@');
    if (at == std::string::npos) return "unknown";
    return e.substr(at + 1);
}

// ── OrgStats ─────────────────────────────────────────────────────────
struct OrgStats {
    long long commits = 0;
    long long added   = 0;
    long long deleted = 0;
    long long total   = 0;
};

// ── get_commit_count ─────────────────────────────────────────────────
static long long get_commit_count(const std::string& repo_path,
                                   const std::string& git_dir) {
    std::string cmd;
    if (!git_dir.empty()) {
        cmd = build_git_cmd({"rev-list", "--count", "HEAD"}, git_dir);
    } else {
        cmd = "cd " + shell_escape(repo_path) + " && git rev-list --count HEAD";
    }
    std::string out = run_cmd(cmd);
    // trim
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    if (out.empty()) return 0;
    return std::stoll(out);
}

// ── collect_stats ────────────────────────────────────────────────────
static std::unordered_map<std::string, OrgStats> collect_stats(
    const std::string& repo_path,
    const std::string& git_dir,
    bool fast,
    int progress_every)
{
    std::string location = git_dir.empty() ? repo_path : git_dir;
    log_info("Collecting commit stats from %s", location.c_str());

    long long total_commits = -1;
    try {
        total_commits = get_commit_count(repo_path, git_dir);
    } catch (...) {
        log_debug("Could not determine total commit count.");
    }

    std::string cmd;
    if (!git_dir.empty()) {
        cmd = "git --git-dir " + shell_escape(git_dir)
            + " log --numstat '--format=@@@%ae'";
    } else {
        cmd = "cd " + shell_escape(repo_path)
            + " && git log --numstat '--format=@@@%ae'";
    }
    if (fast) cmd += " --no-renames --no-ext-diff";
    cmd += " 2>/dev/null";

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) throw std::runtime_error("popen failed for git log");

    std::unordered_map<std::string, OrgStats> stats;
    std::string current_org;
    bool has_org = false;
    long long commit_index = 0;

    char* line_buf = nullptr;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line_buf, &line_cap, fp)) != -1) {
        // strip trailing newline
        if (line_len > 0 && line_buf[line_len - 1] == '\n') {
            line_buf[line_len - 1] = '\0';
            line_len--;
        }

        if (line_len >= 3 && line_buf[0] == '@' && line_buf[1] == '@' && line_buf[2] == '@') {
            std::string email(line_buf + 3);
            current_org = email_to_org(email);
            has_org = true;
            stats[current_org].commits++;
            commit_index++;
            if (progress_every > 0 && commit_index % progress_every == 0) {
                if (total_commits > 0) {
                    double pct = (static_cast<double>(commit_index) / total_commits) * 100.0;
                    log_info("Progress: %lld/%lld commits (%.1f%%)",
                             commit_index, total_commits, pct);
                } else {
                    log_info("Progress: %lld commits", commit_index);
                }
            }
            continue;
        }

        if (line_len == 0 || !has_org) continue;

        // parse numstat line: added\tdeleted\tfilename
        char* p1 = strchr(line_buf, '\t');
        if (!p1) continue;
        char* p2 = strchr(p1 + 1, '\t');
        if (!p2) continue;

        *p1 = '\0';
        *p2 = '\0';
        const char* added_s = line_buf;
        const char* deleted_s = p1 + 1;

        if (added_s[0] == '-' || deleted_s[0] == '-') continue;

        char* end1;
        char* end2;
        long long added_v = strtoll(added_s, &end1, 10);
        long long deleted_v = strtoll(deleted_s, &end2, 10);
        if (*end1 != '\0' || *end2 != '\0') continue;

        OrgStats& os = stats[current_org];
        os.added   += added_v;
        os.deleted += deleted_v;
        os.total   += added_v + deleted_v;
    }

    free(line_buf);
    int status = pclose(fp);
    if (status != 0) {
        throw std::runtime_error("git log failed (exit " + std::to_string(status) + ")");
    }

    if (total_commits > 0 && commit_index > 0 && commit_index != total_commits) {
        log_info("Progress: %lld/%lld commits (100%%)", commit_index, total_commits);
    }
    log_info("Aggregated stats for %zu orgs", stats.size());
    return stats;
}

// ── format_table ─────────────────────────────────────────────────────
static void format_table(
    const std::vector<std::vector<std::string>>& rows,
    const std::vector<std::string>& headers)
{
    std::vector<size_t> widths(headers.size());
    for (size_t i = 0; i < headers.size(); i++)
        widths[i] = headers[i].size();
    for (auto& row : rows)
        for (size_t i = 0; i < row.size() && i < widths.size(); i++)
            widths[i] = std::max(widths[i], row[i].size());

    auto print_row = [&](const std::vector<std::string>& row) {
        for (size_t i = 0; i < row.size(); i++) {
            if (i > 0) std::cout << "  ";
            std::cout << row[i];
            if (i + 1 < row.size()) {
                size_t pad = widths[i] - row[i].size();
                for (size_t p = 0; p < pad; p++) std::cout << ' ';
            }
        }
        std::cout << '\n';
    };

    print_row(headers);
    std::vector<std::string> sep(headers.size());
    for (size_t i = 0; i < widths.size(); i++)
        sep[i] = std::string(widths[i], '-');
    print_row(sep);
    for (auto& row : rows) print_row(row);
}

// ── main ─────────────────────────────────────────────────────────────
static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -v, --verbose          Enable verbose logging\n"
        "      --repo-path PATH   Path to linux repo (default: ./linux-github)\n"
        "      --git-file PATH    Path to .git dir or gitfile\n"
        "      --refresh          Fetch updates if repo exists\n"
        "      --top N            Number of orgs to show (default: 100)\n"
        "      --loc-metric M     added|deleted|total (default: total)\n"
        "      --fast             Disable rename detection and ext diff (default)\n"
        "      --no-fast          Enable rename detection and ext diff\n"
        "      --progress-every N Log progress every N commits (default: 5000)\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string repo_path = (fs::current_path() / "linux-github").string();
    std::string git_file;
    bool refresh = false;
    int top_n = 100;
    std::string loc_metric = "total";
    bool fast = true;
    int progress_every = 5000;

    enum LongOpt {
        OPT_REPO_PATH = 1000,
        OPT_GIT_FILE,
        OPT_REFRESH,
        OPT_TOP,
        OPT_LOC_METRIC,
        OPT_FAST,
        OPT_NO_FAST,
        OPT_PROGRESS_EVERY,
    };

    static struct option long_options[] = {
        {"verbose",        no_argument,       nullptr, 'v'},
        {"repo-path",      required_argument, nullptr, OPT_REPO_PATH},
        {"git-file",       required_argument, nullptr, OPT_GIT_FILE},
        {"refresh",        no_argument,       nullptr, OPT_REFRESH},
        {"top",            required_argument, nullptr, OPT_TOP},
        {"loc-metric",     required_argument, nullptr, OPT_LOC_METRIC},
        {"fast",           no_argument,       nullptr, OPT_FAST},
        {"no-fast",        no_argument,       nullptr, OPT_NO_FAST},
        {"progress-every", required_argument, nullptr, OPT_PROGRESS_EVERY},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "v", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'v':
            g_verbose = true;
            break;
        case OPT_REPO_PATH:
            repo_path = optarg;
            break;
        case OPT_GIT_FILE:
            git_file = optarg;
            break;
        case OPT_REFRESH:
            refresh = true;
            break;
        case OPT_TOP:
            top_n = std::stoi(optarg);
            break;
        case OPT_LOC_METRIC:
            loc_metric = optarg;
            if (loc_metric != "added" && loc_metric != "deleted" && loc_metric != "total") {
                fprintf(stderr, "Error: --loc-metric must be added, deleted, or total\n");
                return 1;
            }
            break;
        case OPT_FAST:
            fast = true;
            break;
        case OPT_NO_FAST:
            fast = false;
            break;
        case OPT_PROGRESS_EVERY:
            progress_every = std::stoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    try {
        std::string git_dir;
        std::string rp = repo_path;

        if (!git_file.empty()) {
            git_dir = resolve_git_dir(git_file);
            rp.clear();
        } else {
            ensure_repo(rp, refresh);
        }

        auto stats = collect_stats(rp, git_dir, fast, progress_every);

        // sort
        using Entry = std::pair<std::string, OrgStats>;
        std::vector<Entry> entries(stats.begin(), stats.end());

        auto get_metric = [&](const OrgStats& s) -> long long {
            if (loc_metric == "added") return s.added;
            if (loc_metric == "deleted") return s.deleted;
            return s.total;
        };

        std::sort(entries.begin(), entries.end(),
            [&](const Entry& a, const Entry& b) {
                if (a.second.commits != b.second.commits)
                    return a.second.commits > b.second.commits;
                long long ma = get_metric(a.second), mb = get_metric(b.second);
                if (ma != mb) return ma > mb;
                return a.first < b.first;
            });

        if (static_cast<int>(entries.size()) > top_n)
            entries.resize(top_n);

        // build table
        std::vector<std::string> headers = {"#", "org", "commits", "added", "deleted", "total"};
        std::vector<std::vector<std::string>> rows;
        for (size_t i = 0; i < entries.size(); i++) {
            auto& [org, s] = entries[i];
            rows.push_back({
                std::to_string(i + 1),
                org,
                std::to_string(s.commits),
                std::to_string(s.added),
                std::to_string(s.deleted),
                std::to_string(s.total),
            });
        }

        format_table(rows, headers);
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
