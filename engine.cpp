#include "engine.h"
#include "config.h"
#include "git.h"
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

constexpr DWORD debounce_ms = 3000;

const std::vector<std::string>& default_preset() {
    for (auto& [name, pats] : g_presets)
        if (name == "General")
            return pats;
    static const std::vector<std::string> empty;
    return empty;
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
        std::string key = to_utf8(relw);
        if (std::find(nested_roots_.begin(), nested_roots_.end(), key) == nested_roots_.end())
            nested_roots_.push_back(key);
    }
}

void watch_engine::stage_all_nested_repos() {
    for (auto& r : nested_roots_)
        stage_nested_repo(r);
}

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
    fs::path abs = fs::path(watch_path_) / fs::path(to_wide(rel_path));
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
        git_exec(args, 60000, watch_path_.c_str(), stop_event_);
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

void watch_engine::stage_nested_repo(const std::string& root_posix) {
    fs::path abs_root = fs::path(watch_path_) / fs::path(to_wide(root_posix));
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
        std::string rel = to_utf8(relw);
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
        for (auto& r : nested_roots_)
            stage_nested_repo(r);
    } else {
        std::vector<std::string> adds, dels;
        for (auto& p : pending) {
            if (!is_under_nested(p))
                continue;
            fs::path abs = fs::path(watch_path_) / fs::path(to_wide(p));
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
                    overflow_ = true;
                    last_change = GetTickCount64();
                } else {
                    BYTE* p = buf.data();
                    for (;;) {
                        auto fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
                        std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                        std::string rel = to_utf8(name);
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
