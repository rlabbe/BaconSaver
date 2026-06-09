#pragma once
#include "ignore_filter.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
using log_fn = std::function<void(const std::string&)>;

using presets_t = std::vector<std::pair<std::string, std::vector<std::string>>>;
extern presets_t g_presets;

class watch_engine {
public:
    watch_engine(
        const std::wstring& watch_path,
        const std::wstring& shadows_base,
        log_fn log,
        const std::vector<std::string>& initial_patterns = {},
        bool skip_binary = false);
    ~watch_engine();

    void start();
    void stop();
    void pause();
    void resume();

    bool is_running() const { return _running; }
    bool is_paused() const { return _paused; }
    const std::wstring& watch_path() const { return _watch_path; }
    const std::wstring& shadow_path() const { return _shadow_path; }
    IgnoreFilter& ignore() { return _ignore; }

    void sync_exclude();

private:
    std::wstring _watch_path;
    std::wstring _shadow_path;
    log_fn _log;
    IgnoreFilter _ignore;
    HANDLE _thread = nullptr;
    HANDLE _stop_event = nullptr;
    std::atomic<bool> _running{ false };
    std::atomic<bool> _paused{ false };
    bool _skip_binary = false;

    // Directories that contain their own .git. Git would treat these as
    // submodules; instead we record their roots, exclude them from `git add`,
    // and stage their files directly so they are backed up as plain files.
    std::vector<std::string> _nested_roots;
    std::set<std::string> _pending; // changed paths (POSIX, relative) since last commit
    bool _overflow = false;         // watcher buffer overflowed; re-scan nested repos

    void _thread_proc();
    void _init_shadow_repo();
    void _apply_perf_config();
    void _commit();
    std::string _git(const std::vector<std::string>& args, bool check = true, DWORD timeout_ms = 30000);

    void _discover_nested_repos();
    void _stage_all_nested_repos();
    void _absorb_new_gitlinks();
    void _stage_nested_repo(const std::string& root_posix);
    void _stage_files(const std::vector<std::string>& add_posix);
    void _remove_paths(const std::vector<std::string>& del_posix);
    bool _is_under_nested(const std::string& posix) const;
    bool _is_binary_file(const std::string& rel_path) const;
    void _unstage_binaries();

    static std::wstring _shadow_name(const std::wstring& watch_path);
};

struct commit_entry {
    std::string hash;
    std::string timestamp;
    std::string message;
};

struct commit_file {
    std::string status;
    std::string path;
};

std::string run_git(const std::vector<std::string>& args, DWORD timeout_ms = 30000);
std::tuple<int, int, int> git_version();
std::vector<commit_entry> get_commit_log(const fs::path& git_dir, const fs::path& work_tree);
std::vector<commit_file> get_commit_files(const fs::path& git_dir, const fs::path& work_tree, const std::string& hash);
std::string get_file_at_commit(const fs::path& git_dir, const std::string& hash, const std::string& file_path);
std::vector<std::string> get_full_tree_at_commit(const fs::path& git_dir, const std::string& hash);
std::string get_diff_for_commit(
    const fs::path& git_dir, const fs::path& work_tree, const std::string& hash, const std::string& file_path);
std::vector<std::string> export_files(
    const fs::path& git_dir,
    const std::string& hash,
    const std::vector<std::string>& file_paths,
    const fs::path& dest_dir);
