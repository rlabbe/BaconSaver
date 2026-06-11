#include "git.h"
#include "util.h"
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

// ---- quote_arg ----

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

// ---- git_exec ----

git_result git_exec(const std::vector<std::string>& args, DWORD timeout_ms, const wchar_t* cwd,
                    HANDLE cancel_event) {
    std::wstring cmd = L"git";
    for (auto& a : args) {
        cmd += L' ';
        cmd += quote_arg(to_wide(a));
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&err_r, &err_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd, &si, &pi)) {
        DWORD gle = GetLastError();
        CloseHandle(job);
        CloseHandle(out_r);
        CloseHandle(out_w);
        CloseHandle(err_r);
        CloseHandle(err_w);
        throw std::runtime_error("Failed to run git (error " + std::to_string(gle) + ")");
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(out_w);
    CloseHandle(err_w);

    git_result r;
    DWORD deadline = timeout_ms ? GetTickCount() + timeout_ms : 0;
    HANDLE handles[2] = { pi.hProcess, cancel_event ? cancel_event : pi.hProcess };
    DWORD handle_count = cancel_event ? 2 : 1;
    for (;;) {
        DWORD remain = 100;
        if (deadline) {
            DWORD now = GetTickCount();
            if (now >= deadline) {
                TerminateProcess(pi.hProcess, 1);
                r.code = 1;
                r.err = "timeout or error";
                break;
            }
            DWORD left = deadline - now;
            if (left < remain)
                remain = left;
        }
        DWORD which = WaitForMultipleObjects(handle_count, handles, FALSE, remain);
        if (which == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &r.code);
            break;
        }
        if (which == WAIT_OBJECT_0 + 1) {
            TerminateProcess(pi.hProcess, 1);
            r.code = 1;
            r.err = "cancelled";
            break;
        }
        char buf[8192];
        DWORD n = 0;
        if (PeekNamedPipe(out_r, nullptr, 0, nullptr, &n, nullptr) && n) {
            DWORD to_read = n > sizeof(buf) - 1 ? sizeof(buf) - 1 : n;
            if (ReadFile(out_r, buf, to_read, &n, nullptr) && n)
                r.out.append(buf, n);
        }
        if (PeekNamedPipe(err_r, nullptr, 0, nullptr, &n, nullptr) && n) {
            DWORD to_read = n > sizeof(buf) - 1 ? sizeof(buf) - 1 : n;
            if (ReadFile(err_r, buf, to_read, &n, nullptr) && n)
                r.err.append(buf, n);
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(job);

    if (r.err == "cancelled") {
        CloseHandle(out_r);
        CloseHandle(err_r);
        return r;
    }

    char buf[8192];
    DWORD n = 0;
    while (ReadFile(out_r, buf, sizeof(buf), &n, nullptr) && n)
        r.out.append(buf, n);
    while (ReadFile(err_r, buf, sizeof(buf), &n, nullptr) && n)
        r.err.append(buf, n);
    CloseHandle(out_r);
    CloseHandle(err_r);
    return r;
}

// ---- cleanup_stale_lock ----

void cleanup_stale_lock(const fs::path& git_dir, const log_fn& log) {
    fs::path lock = git_dir / "index.lock";
    std::error_code ec;
    if (!fs::exists(lock, ec))
        return;

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt > 0)
            Sleep(200);
        fs::remove(lock, ec);
        if (!ec) {
            if (attempt > 0)
                log("Removed stale index.lock (retry " + std::to_string(attempt) + ")");
            else
                log("Removed stale index.lock");
            return;
        }
    }
    log("ERROR: cannot remove index.lock after 5 attempts: " + ec.message());
    log("  " + path_utf8(lock));
    log("  Close any other programs using this repo, or delete the file manually.");
}

// ---- run_git ----

std::string run_git(const std::vector<std::string>& args, DWORD timeout_ms) {
    auto r = git_exec(args, timeout_ms);
    if (r.code != 0)
        throw std::runtime_error("git failed:\n" + r.err);
    return r.out;
}

// ---- git_version ----

std::tuple<int, int, int> git_version() {
    git_result r;
    try {
        r = git_exec({ "--version" }, 10000);
    } catch (...) {
        return { 0, 0, 0 };
    }
    std::istringstream ss(r.out);
    std::string word;
    while (ss >> word) {
        if (!word.empty() && std::isdigit((unsigned char)word[0])) {
            int maj = 0, min = 0, pat = 0;
            if (sscanf_s(word.c_str(), "%d.%d.%d", &maj, &min, &pat) >= 2)
                return { maj, min, pat };
        }
    }
    return { 0, 0, 0 };
}

// ---- History queries ----

std::vector<commit_entry> get_commit_log(const fs::path& git_dir, const fs::path& work_tree) {
    auto r = git_exec(
        { "--git-dir=" + path_utf8(git_dir), "--work-tree=" + path_utf8(work_tree), "log", "--format=%H%x09%ai%x09%s" },
        30000);
    std::vector<commit_entry> commits;
    for (auto& line : split_lines(r.out)) {
        if (trim(line).empty())
            continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos)
            continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;
        commit_entry e;
        e.hash = line.substr(0, t1);
        e.timestamp = line.substr(t1 + 1, t2 - t1 - 1);
        e.message = line.substr(t2 + 1);
        commits.push_back(std::move(e));
    }
    return commits;
}

std::vector<commit_file> get_commit_files(const fs::path& git_dir, const fs::path& work_tree, const std::string& hash) {
    auto r = git_exec(
        { "--git-dir=" + path_utf8(git_dir), "--work-tree=" + path_utf8(work_tree), "diff-tree", "--root",
          "--no-commit-id", "-r", "--name-status", hash },
        30000);
    std::vector<commit_file> files;
    for (auto& line : split_lines(r.out)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        commit_file f;
        f.status = trim(line.substr(0, tab));
        f.path = trim(line.substr(tab + 1));
        files.push_back(std::move(f));
    }
    return files;
}

std::string get_file_at_commit(const fs::path& git_dir, const std::string& hash, const std::string& file_path) {
    auto r = git_exec({ "--git-dir=" + path_utf8(git_dir), "show", hash + ":" + file_path }, 30000);
    if (r.code != 0)
        throw std::runtime_error("Could not retrieve " + file_path + " at " + hash);
    return r.out;
}

std::vector<std::string> get_full_tree_at_commit(const fs::path& git_dir, const std::string& hash) {
    auto r = git_exec({ "--git-dir=" + path_utf8(git_dir), "ls-tree", "-r", "--name-only", hash }, 30000);
    std::vector<std::string> out;
    for (auto& line : split_lines(r.out)) {
        std::string t = trim(line);
        if (!t.empty())
            out.push_back(t);
    }
    return out;
}

std::string get_diff_for_commit(
    const fs::path& git_dir, const fs::path& work_tree, const std::string& hash, const std::string& file_path) {
    std::vector<std::string> args = {
        "--git-dir=" + path_utf8(git_dir), "--work-tree=" + path_utf8(work_tree), "diff", hash + "~1", hash, "--"
    };
    if (!file_path.empty())
        args.push_back(file_path);
    auto r = git_exec(args, 30000);
    return r.out;
}

std::vector<std::string> export_files(
    const fs::path& git_dir,
    const std::string& hash,
    const std::vector<std::string>& file_paths,
    const fs::path& dest_dir) {
    std::vector<std::string> exported;
    for (auto& fp : file_paths) {
        std::string content;
        try {
            content = get_file_at_commit(git_dir, hash, fp);
        } catch (const std::exception&) {
            continue;
        }
        fs::path out = dest_dir / fs::path(to_wide(fp));
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(content.data(), (std::streamsize)content.size());
        exported.push_back(fp);
    }
    return exported;
}
