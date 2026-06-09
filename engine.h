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

    bool is_running() const { return running_; }
    bool is_paused() const { return paused_; }
    const std::wstring& watch_path() const { return watch_path_; }
    const std::wstring& shadow_path() const { return shadow_path_; }
    IgnoreFilter& ignore() { return ignore_; }

    void sync_exclude();

private:
    std::wstring watch_path_;
    std::wstring shadow_path_;
    log_fn log_;
    IgnoreFilter ignore_;
    HANDLE thread_ = nullptr;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> running_{ false };
    std::atomic<bool> paused_{ false };
    bool skip_binary_ = false;

    // Directories that contain their own .git. Git would treat these as
    // submodules; instead we record their roots, exclude them from `git add`,
    // and stage their files directly so they are backed up as plain files.
    std::vector<std::string> nested_roots_;
    std::set<std::string> pending_; // changed paths (POSIX, relative) since last commit
    bool overflow_ = false;         // watcher buffer overflowed; re-scan nested repos

    void thread_proc();
    void init_shadow_repo();
    void apply_perf_config();
    void commit();
    std::string git(const std::vector<std::string>& args, bool check = true, DWORD timeout_ms = 30000);

    void discover_nested_repos();
    void stage_all_nested_repos();
    void absorb_new_gitlinks();
    void stage_nested_repo(const std::string& root_posix);
    void stage_files(const std::vector<std::string>& add_posix);
    void remove_paths(const std::vector<std::string>& del_posix);
    bool is_under_nested(const std::string& posix) const;
    bool is_binary_file(const std::string& rel_path) const;
    void unstage_binaries();

    static std::wstring shadow_name(const std::wstring& watch_path);
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
