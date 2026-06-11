#pragma once
#include "log.h"
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

// ---- Types ----

struct commit_entry {
    std::string hash;
    std::string timestamp;
    std::string message;
};

struct commit_file {
    std::string status;
    std::string path;
};

// ---- Low-level git process management ----

std::wstring quote_arg(const std::wstring& arg);

struct git_result {
    DWORD code = 0;
    std::string out;
    std::string err;
};

git_result git_exec(const std::vector<std::string>& args, DWORD timeout_ms, const wchar_t* cwd = nullptr,
                    HANDLE cancel_event = nullptr);

std::string run_git(const std::vector<std::string>& args, DWORD timeout_ms = 30000);
std::tuple<int, int, int> git_version();
void cleanup_stale_lock(const fs::path& git_dir, const log_fn& log);

// ---- Read-only history queries ----

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
