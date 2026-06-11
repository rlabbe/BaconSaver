#pragma once
#include "ignore_filter.h"
#include "log.h"
#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

class watch_engine {
public:
    watch_engine(
        const std::wstring& watch_path,
        const std::wstring& shadow_path,
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
    bool is_stopping() const { return stopping_; }
    const std::wstring& watch_path() const { return watch_path_; }
    const std::wstring& shadow_path() const { return shadow_path_; }
    IgnoreFilter& ignore() { return ignore_; }

    static std::wstring shadow_name(const std::wstring& watch_path);
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
    std::atomic<bool> stopping_{ false };
    bool skip_binary_ = false;

    std::vector<std::string> nested_roots_;
    std::set<std::string> pending_;
    bool overflow_ = false;

    void thread_proc();
    void init_shadow_repo();
    void apply_perf_config();
    void commit();
    std::string git(const std::vector<std::string>& args, bool check = true);

    void discover_nested_repos();
    void stage_all_nested_repos();
    void absorb_new_gitlinks();
    void stage_nested_repo(const std::string& root_posix);
    void stage_files(const std::vector<std::string>& add_posix);
    void remove_paths(const std::vector<std::string>& del_posix);
    bool is_under_nested(const std::string& posix) const;
    bool is_binary_file(const std::string& rel_path) const;
    void unstage_binaries();
};
