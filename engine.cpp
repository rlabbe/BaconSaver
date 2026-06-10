#include "engine.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

const std::vector<std::string> always_ignored = { ".git" };
constexpr DWORD debounce_ms = 3000;

std::string wtoa(const std::wstring& ws) {
    if (ws.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring atow(const std::string& s) {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

std::string path_utf8(const fs::path& p) {
    return wtoa(p.wstring());
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

void trace_log(const std::string& msg) {
    std::error_code ec;
    std::ofstream f(app_dir() / "baconsaver.log", std::ios::app | std::ios::binary);
    f << now_ts() << "  [trace] " << msg << "\n";
}

// Quote an argument for a Windows command line (CommandLineToArgvW rules).
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

struct git_result {
    DWORD code = 0;
    std::string out;
    std::string err;
};

// Run "git <args>" capturing raw stdout/stderr bytes. Never injects --git-dir;
// callers pass whatever flags they need. Throws only if the process cannot start.
// Uses a job object so that when the job handle is closed, git and all its
// child processes are terminated — no orphans.
git_result git_exec(const std::vector<std::string>& args, DWORD timeout_ms, const wchar_t* cwd = nullptr,
                    HANDLE cancel_event = nullptr) {
    std::wstring cmd = L"git";
    for (auto& a : args) {
        cmd += L' ';
        cmd += quote_arg(atow(a));
    }

    // Job object: closing this handle kills git and all children
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

    // Drain pipes while waiting so large output doesn't deadlock (pipe buffer fills
    // -> git blocks on write -> we block on wait).
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
        // Drain pipes to prevent deadlock (pipe buffer full -> git blocks)
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
    CloseHandle(job); // kill all children

    if (r.err == "cancelled") {
        CloseHandle(out_r);
        CloseHandle(err_r);
        return r;
    }

    // Read remaining output
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

const std::vector<std::string>& default_preset() {
    for (auto& [name, pats] : g_presets)
        if (name == "General")
            return pats;
    static const std::vector<std::string> empty;
    return empty;
}

presets_t g_presets = {
    { "General", { ".svn", "x64", "*~", "*.TMP" } },
    { "C++", { ".vs", "x64", "Debug", "Release", "*.suo", "*.user", "*.sdf", "*.opensdf", "*.dll", "*.lib", "*.exe", "*.pdb", "*.ilk", "*.exp", "*.obj" } },
    { "Python", { "__pycache__", "*.pyc", ".mypy_cache", ".pytest_cache", "*.egg-info", ".venv", "venv", ".ipynb_checkpoints" } },
};

std::string run_git(const std::vector<std::string>& args, DWORD timeout_ms) {
    auto r = git_exec(args, timeout_ms);
    if (r.code != 0)
        throw std::runtime_error("git failed:\n" + r.err);
    return r.out;
}

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

// ---------------------------------------------------------------------------
// watch_engine
// ---------------------------------------------------------------------------

std::wstring watch_engine::shadow_name(const std::wstring& watch_path) {
    fs::path p(watch_path);
    std::wstring leaf = p.filename().wstring();
    if (leaf.empty())
        leaf = L"root";
    std::wstring result;
    for (wchar_t c : leaf) {
        if (iswalnum(c) || c == L'_' || c == L'-')
            result += c;
        else
            result += L'_';
    }
    while (!result.empty() && result.back() == L'_')
        result.pop_back();
    return result;
}

watch_engine::watch_engine(
    const std::wstring& watch_path,
    const std::wstring& shadow_path,
    log_fn log,
    const std::vector<std::string>& initial_patterns,
    bool skip_binary)
    : watch_path_(fs::absolute(fs::path(watch_path)).wstring()),
      shadow_path_(shadow_path), log_(std::move(log)),
      ignore_(fs::path(shadow_path_) / "ignore"), skip_binary_(skip_binary) {
    std::error_code ec;
    fs::create_directories(shadow_path_, ec);
    fs::path ignore_file = fs::path(shadow_path_) / "ignore";
    if (!fs::exists(ignore_file)) {
        const std::vector<std::string>& pats = initial_patterns.empty() ? default_preset() : initial_patterns;
        ignore_.set_patterns(pats);
    }
}

watch_engine::~watch_engine() {
    stop();
    if (stop_event_)
        CloseHandle(stop_event_);
}

void watch_engine::start() {
    if (running_)
        return;
    running_ = true;
    paused_ = false;
    if (!stop_event_)
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(stop_event_);
    // clang-format off
    thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        static_cast<watch_engine*>(p)->thread_proc();
        return 0;
    }, this, 0, nullptr);
    // clang-format on
}

void watch_engine::stop() {
    if (!running_) {
        return;
    }
    if (stopping_) {
        return;
    }
    stopping_ = true;
    if (stop_event_)
        SetEvent(stop_event_);
    if (thread_) {
        for (;;) {
            DWORD r = WaitForSingleObject(thread_, 250);
            if (r == WAIT_OBJECT_0) {
                CloseHandle(thread_);
                thread_ = nullptr;
                break;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    running_ = false;
    paused_ = false;
    cleanup_stale_lock(fs::path(shadow_path_) / ".git", log_);
    log_("Stopped: " + path_utf8(watch_path_));
}

void watch_engine::pause() {
    if (!running_ || paused_)
        return;
    if (stopping_)
        return;
    stopping_ = true;
    if (stop_event_)
        SetEvent(stop_event_);
    if (thread_) {
        for (;;) {
            DWORD r = WaitForSingleObject(thread_, 250);
            if (r == WAIT_OBJECT_0) {
                CloseHandle(thread_);
                thread_ = nullptr;
                break;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    paused_ = true;
    stopping_ = false;
    log_("Paused: " + path_utf8(watch_path_));
}

void watch_engine::resume() {
    if (!running_ || !paused_)
        return;
    paused_ = false;
    ResetEvent(stop_event_);
    // clang-format off
    thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        static_cast<watch_engine*>(p)->thread_proc();
        return 0;
    }, this, 0, nullptr);
    // clang-format on
    log_("Resumed: " + path_utf8(watch_path_));
}

void watch_engine::sync_exclude() {
    fs::path exclude = fs::path(shadow_path_) / ".git" / "info" / "exclude";
    std::error_code ec;
    fs::create_directories(exclude.parent_path(), ec);
    if (ec) {
        log_("ERROR: cannot create exclude dir: " + ec.message());
        return;
    }
    {
        std::ofstream out(exclude, std::ios::binary);
        if (!out) {
            log_("ERROR: cannot open exclude file: " + path_utf8(exclude));
            return;
        }
        for (auto& a : always_ignored)
            out << a << '\n';
        std::ifstream in(ignore_.file(), std::ios::binary);
        if (in)
            out << in.rdbuf();
        else
            log_("WARNING: ignore file not found: " + path_utf8(ignore_.file()));
        for (auto& r : nested_roots_)
            out << r << "/\n";
    }
}

std::string watch_engine::git(const std::vector<std::string>& args, bool check) {
    std::vector<std::string> full;
    full.push_back("--git-dir=" + path_utf8(fs::path(shadow_path_) / ".git"));
    full.push_back("--work-tree=" + path_utf8(watch_path_));
    full.insert(full.end(), args.begin(), args.end());
    auto r = git_exec(full, 0, nullptr, stop_event_);
    if (check && r.code != 0) {
        if (r.err == "cancelled")
            throw std::runtime_error("git cancelled");
        std::string verb = args.empty() ? "" : args[0];
        throw std::runtime_error("git " + verb + " failed:\n" + r.err);
    }
    return r.out;
}

void watch_engine::apply_perf_config() {
    git({ "config", "feature.manyFiles", "true" });
    git({ "config", "index.version", "4" });
    git({ "config", "core.untrackedCache", "true" });
    auto [maj, min, pat] = git_version();
    if (maj > 2 || (maj == 2 && min >= 37)) {
        git({ "config", "core.fsmonitor", "true" });
        log_("Performance: large-repo settings enabled");
    } else {
        log_(
            "Performance: large-repo settings enabled (fsmonitor skipped - Git " + std::to_string(maj) + "." +
            std::to_string(min) + " < 2.37)");
    }
}

// ---------------------------------------------------------------------------
// Nested git repos
// ---------------------------------------------------------------------------

// Scan one level deep for directories that contain a .git entry (could be a
// directory or a file, e.g. worktrees).  Must be called before `git add -A`.
void watch_engine::discover_nested_repos() {
    std::error_code ec;
    int count = 0;
    for (auto& entry : fs::directory_iterator(watch_path_, ec)) {
        if (ec)
            break;
        if (++count % 1000 == 0 && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
            return;
        }
        if (!entry.is_directory(ec))
            continue;
        if (!fs::exists(entry.path() / ".git", ec))
            continue;
        std::wstring relw = fs::relative(entry.path(), watch_path_, ec).wstring();
        std::replace(relw.begin(), relw.end(), L'\\', L'/');
        if (relw == L"." || relw.empty())
            continue;
        std::string key = wtoa(relw);
        if (std::find(nested_roots_.begin(), nested_roots_.end(), key) == nested_roots_.end())
            nested_roots_.push_back(key);
    }
}

// Stage files from every known nested repo so they are backed up as plain
// files (not gitlinks).
void watch_engine::stage_all_nested_repos() {
    for (auto& r : nested_roots_)
        stage_nested_repo(r);
}

// Catch any new gitlinks that `git add` created despite our exclude rules.
// This is a safety net — discover_nested_repos should prevent them.
void watch_engine::absorb_new_gitlinks() {
    std::string out = git({ "diff", "--cached", "--raw" }, false);
    std::vector<std::string> roots;
    for (auto& line : split_lines(out)) {
        if (line.empty() || line[0] != ':')
            continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        std::istringstream meta(line.substr(0, tab));
        std::string old_mode, new_mode;
        meta >> old_mode >> new_mode;
        if (new_mode != "160000")
            continue;
        std::string path = line.substr(tab + 1);
        if (std::find(nested_roots_.begin(), nested_roots_.end(), path) == nested_roots_.end()) {
            nested_roots_.push_back(path);
            git({ "rm", "--cached", "--quiet", "-r", "--ignore-unmatch", "--", path }, false);
            sync_exclude();
            stage_nested_repo(path);
        }
    }
}

bool watch_engine::is_under_nested(const std::string& posix) const {
    for (auto& r : nested_roots_) {
        if (posix == r)
            return true;
        if (posix.size() > r.size() && posix.compare(0, r.size(), r) == 0 && posix[r.size()] == '/')
            return true;
    }
    return false;
}

bool watch_engine::is_binary_file(const std::string& rel_path) const {
    fs::path abs = fs::path(watch_path_) / fs::path(atow(rel_path));
    std::error_code ec;
    if (!fs::is_regular_file(abs, ec))
        return false;
    std::ifstream f(abs, std::ios::binary);
    if (!f)
        return false;
    char buf[8192];
    f.read(buf, sizeof(buf));
    auto n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i)
        if (buf[i] == '\0')
            return true;
    return false;
}

void watch_engine::unstage_binaries() {
    std::string staged = git({ "diff", "--cached", "--name-only" }, false);
    std::vector<std::string> bins;
    for (auto& line : split_lines(staged)) {
        std::string f = trim(line);
        if (!f.empty() && is_binary_file(f))
            bins.push_back(f);
    }
    if (!bins.empty()) {
        remove_paths(bins);
    }
}

// Stage files directly into the index (git hashes them). Bypasses `git add`'s
// submodule detection, so files inside a nested repo go in as plain files.
void watch_engine::stage_files(const std::vector<std::string>& add_posix) {
    if (add_posix.empty())
        return;
    std::string gd = "--git-dir=" + path_utf8(fs::path(shadow_path_) / ".git");
    std::string wt = "--work-tree=" + path_utf8(watch_path_);
    const size_t batch = 100;
    for (size_t i = 0; i < add_posix.size(); i += batch) {
        size_t end = i + batch;
        if (end > add_posix.size())
            end = add_posix.size();
        std::vector<std::string> args = { gd, wt, "update-index", "--add", "--" };
        for (size_t j = i; j < end; ++j)
            args.push_back(add_posix[j]);
        auto r = git_exec(args, 60000, watch_path_.c_str(), stop_event_);
    }
}

void watch_engine::remove_paths(const std::vector<std::string>& del_posix) {
    if (del_posix.empty())
        return;
    std::string gd = "--git-dir=" + path_utf8(fs::path(shadow_path_) / ".git");
    std::string wt = "--work-tree=" + path_utf8(watch_path_);
    const size_t batch = 100;
    for (size_t i = 0; i < del_posix.size(); i += batch) {
        size_t end = i + batch;
        if (end > del_posix.size())
            end = del_posix.size();
        std::vector<std::string> args = { gd, wt, "rm", "-r", "--cached", "--ignore-unmatch", "--" };
        for (size_t j = i; j < end; ++j)
            args.push_back(del_posix[j]);
        git_exec(args, 60000, watch_path_.c_str(), stop_event_);
    }
}

// Walk a nested repo and stage every file (skipping .git at any depth).
void watch_engine::stage_nested_repo(const std::string& root_posix) {
    fs::path abs_root = fs::path(watch_path_) / fs::path(atow(root_posix));
    std::vector<std::string> adds;
    auto pts = ignore_.patterns();
    std::error_code ec;
    int count = 0;
    fs::recursive_directory_iterator it(abs_root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (++count % 1000 == 0 && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
            return;
        }
        if (it->is_directory(ec)) {
            if (it->path().filename() == L".git")
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        std::wstring relw = fs::relative(it->path(), watch_path_, ec).wstring();
        std::string rel = wtoa(relw);
        if (ignore_.is_ignored(rel))
            continue;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (skip_binary_ && is_binary_file(rel))
            continue;
        adds.push_back(rel);
    }
    stage_files(adds);
}

void watch_engine::init_shadow_repo() {
    fs::path shadow = shadow_path_;
    fs::path git_dir = shadow / ".git";
    cleanup_stale_lock(git_dir, log_);

    auto cancelled = [&] { return WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0; };

    if (fs::exists(git_dir)) {
        log_("Checking existing repo...");
        if (fs::exists(git_dir)) {
        discover_nested_repos();
        sync_exclude();
        apply_perf_config();
        if (cancelled()) {
            return;
        }
        log_("Initialized shadow repo: " + path_utf8(git_dir));
        log_("Taking initial snapshot...");
        git({ "add", "-A" });
        if (cancelled())
            return;
        absorb_new_gitlinks();
        stage_all_nested_repos();
        if (skip_binary_)
            unstage_binaries();
        std::string staged = git({ "diff", "--cached", "--name-status" }, false);
        if (!trim(staged).empty()) {
            auto lines = split_lines(trim(staged));
            if (cancelled())
                return;
            log_("Catching up " + std::to_string(lines.size()) + " file(s)...");
            git({ "commit", "-m", "BaconSaver " + now_ts() + " (catch-up)" });
            log_("Catch-up commit done.");
        }
        return;
    }
    }

    std::error_code ec;
    fs::create_directories(shadow, ec);
    log_("Creating new shadow repo...");
    auto init = git_exec({ "init", path_utf8(shadow) }, 30000, nullptr, stop_event_);
    if (init.code != 0)
        throw std::runtime_error("git init failed:\n" + init.err);
    if (cancelled())
        return;

    git({ "config", "user.name", "BaconSaver" });
    git({ "config", "user.email", "baconsaver@local" });
    git({ "config", "core.autocrlf", "false" });
    git({ "config", "core.worktree", path_utf8(watch_path_) });
    apply_perf_config();
    discover_nested_repos();
    sync_exclude();
    if (cancelled())
        return;

    log_("Initialized shadow repo: " + path_utf8(git_dir));
    log_("Taking initial snapshot...");
    git({ "add", "-A" });
    if (cancelled())
        return;
    absorb_new_gitlinks();
    stage_all_nested_repos();
    if (skip_binary_)
        unstage_binaries();
    std::string staged = git({ "diff", "--cached", "--name-status" }, false);
    if (!trim(staged).empty()) {
        if (cancelled())
            return;
        git({ "commit", "-m", "BaconSaver: initial snapshot" });
        log_("Initial snapshot committed.");
    }
}

void watch_engine::commit() {
    fs::path git_dir = fs::path(shadow_path_) / ".git";
    cleanup_stale_lock(git_dir, log_);

    if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0)
        return;

    std::vector<std::string> pending(pending_.begin(), pending_.end());
    pending_.clear();
    bool overflow = overflow_;
    overflow_ = false;

    discover_nested_repos();
    sync_exclude();
    if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0)
        return;
    git({ "add", "-A" });
    absorb_new_gitlinks();
    if (skip_binary_)
        unstage_binaries();

    std::string chk = git({ "diff", "--cached", "--name-only" }, false);

    if (overflow) {
        // Lost the change list — re-stage every nested repo to be safe.
        for (auto& r : nested_roots_)
            stage_nested_repo(r);
    } else {
        std::vector<std::string> adds, dels;
        for (auto& p : pending) {
            if (!is_under_nested(p))
                continue; // handled by `git add -A`
            fs::path abs = fs::path(watch_path_) / fs::path(atow(p));
            std::error_code ec;
            if (fs::is_regular_file(abs, ec)) {
                if (skip_binary_ && is_binary_file(p))
                    continue;
                adds.push_back(p);
            }
            else
                dels.push_back(p);
        }
        stage_files(adds);
        remove_paths(dels);
    }

    std::string staged = git({ "diff", "--cached", "--name-status" }, false);
    if (trim(staged).empty())
        return;
    if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0)
        return;
    auto lines = split_lines(trim(staged));
    std::string ts = now_ts();
    git({ "commit", "-m", "BaconSaver " + ts });
    log_("[" + ts + "] Committed " + std::to_string(lines.size()) + " file(s).");
}

void watch_engine::thread_proc() {
    try {
        init_shadow_repo();
    } catch (const std::exception& e) {
        log_(std::string("thread_proc: caught ") + e.what());
        running_ = false;
        paused_ = false;
        return;
    }
    sync_exclude();

    HANDLE dir = CreateFileW(
        watch_path_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        log_("ERROR: cannot open directory for watching: " + path_utf8(watch_path_));
        running_ = false;
        return;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<BYTE> buf(64 * 1024);
    const DWORD notify_flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;

    auto arm = [&]() {
        ResetEvent(ov.hEvent);
        DWORD br = 0;
        ReadDirectoryChangesW(dir, buf.data(), (DWORD)buf.size(), TRUE, notify_flags, &br, &ov, nullptr);
    };

    log_("Watching: " + path_utf8(watch_path_));
    arm();

    ULONGLONG last_change = 0;
    HANDLE waits[2] = { stop_event_, ov.hEvent };

    for (;;) {
        bool have = !pending_.empty() || overflow_;
        DWORD timeout = INFINITE;
        if (have) {
            ULONGLONG elapsed = GetTickCount64() - last_change;
            timeout = elapsed >= debounce_ms ? 0 : (DWORD)(debounce_ms - elapsed);
        }
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);

        if (w == WAIT_OBJECT_0) {
            CancelIo(dir);
            break;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            DWORD br = 0;
            if (GetOverlappedResult(dir, &ov, &br, FALSE)) {
                if (br == 0) {
                    // Buffer overflow — too many changes to enumerate. Re-scan everything.
                    overflow_ = true;
                    last_change = GetTickCount64();
                } else {
                    BYTE* p = buf.data();
                    for (;;) {
                        auto fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
                        std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                        std::string rel = wtoa(name);
                        if (!ignore_.is_ignored(rel)) {
                            std::replace(rel.begin(), rel.end(), '\\', '/');
                            pending_.insert(rel);
                            last_change = GetTickCount64();
                        }
                        if (fni->NextEntryOffset == 0)
                            break;
                        p += fni->NextEntryOffset;
                    }
                }
            }
            arm();
            continue;
        }
        if (w == WAIT_TIMEOUT) {
            if (!pending_.empty() || overflow_) {
                try {
                    commit();
                } catch (const std::exception& e) {
                    log_(std::string("Commit failed: ") + e.what());
                }
            }
            continue;
        }
        log_("ERROR: watch wait failed");
        break;
    }

    if (ov.hEvent)
        CloseHandle(ov.hEvent);
    CloseHandle(dir);
}

// ---------------------------------------------------------------------------
// History queries — read-only, used by the restore UI
// ---------------------------------------------------------------------------

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
        fs::path out = dest_dir / fs::path(atow(fp));
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(content.data(), (std::streamsize)content.size());
        exported.push_back(fp);
    }
    return exported;
}
