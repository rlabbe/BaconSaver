#include "config.h"
#include "dialogs.h"
#include "engine.h"
#include "git.h"
#include "json.h"
#include "log.h"
#include "util.h"

#include <commctrl.h>
#include <ctime>
#include <fstream>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants / IDs
// ---------------------------------------------------------------------------

const wchar_t* MAIN_CLASS = L"BaconSaverMain";
const wchar_t* MUTEX_NAME = L"BaconSaver-SingleInstance-Mutex";
const UINT WM_TRAYICON = WM_APP + 2;

const COLORREF COL_BG = RGB(0x1e, 0x1e, 0x1e);
const COLORREF COL_TEXT = RGB(0xcc, 0xcc, 0xcc);

enum {
    ID_LIST = 1000,
    ID_ADD,
    ID_REMOVE,
    ID_PAUSE,
    ID_IGNORES,
    ID_RESTORE,
    ID_REPO,
    ID_CONFIG,
    ID_STATUS,
    ID_TRAY_SHOW = 2001,
    ID_TRAY_QUIT = 2002,
};

struct entry {
    std::wstring path;
    std::wstring shadow;
    std::unique_ptr<watch_engine> engine;
    bool paused = false;
    bool skip_binary = false;
};

// Globals (single-window app)
HWND g_main = nullptr;
HWND g_list = nullptr;
HWND g_console = nullptr;
HWND g_status = nullptr;
HBRUSH g_dark_brush = nullptr;
HFONT g_mono_font = nullptr;
int g_splitter_x = 224;
bool g_dragging = false;
bool g_quitting = false;
bool g_busy = false;
int g_window_w = 0;
int g_window_h = 0;
std::vector<std::unique_ptr<entry>> g_entries;
std::vector<std::wstring> g_backup_locations;
std::vector<std::wstring> g_pending_deletions;
NOTIFYICONDATAW g_nid = {};
UINT g_show_msg = 0; // registered message used to surface a second instance

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------

void save_state();

void start_pending_cleanup() {
    if (g_pending_deletions.empty())
        return;
    auto paths = g_pending_deletions; // copy
    std::thread([paths] {
        for (auto& p : paths) {
            std::error_code ec;
            for (int retry = 0; retry < 10; ++retry) {
                if (retry > 0)
                    Sleep(500);
                fs::remove_all(p, ec);
                if (!ec)
                    break;
            }
            // On success, remove from global list and re-save
            auto it = std::find(g_pending_deletions.begin(), g_pending_deletions.end(), p);
            if (it != g_pending_deletions.end()) {
                g_pending_deletions.erase(it);
                save_state();
            }
        }
    }).detach();
}

void update_status() {
    size_t n = g_entries.size();
    std::wstring msg;
    if (n == 0)
        msg = L"Not watching any directories";
    else if (n == 1)
        msg = L"Watching 1 directory";
    else
        msg = L"Watching " + std::to_wstring(n) + L" directories";
    SetWindowTextW(g_status, msg.c_str());
}

void list_set_text(int idx, const std::wstring& text) {
    SendMessageW(g_list, LB_DELETESTRING, idx, 0);
    SendMessageW(g_list, LB_INSERTSTRING, idx, (LPARAM)text.c_str());
    SendMessageW(g_list, LB_SETCURSEL, idx, 0);
}

void start_engine(const std::wstring& path, const std::wstring& shadow, const std::vector<std::string>& patterns, bool skip_binary = false) {
    auto e = std::make_unique<entry>();
    e->path = path;
    e->shadow = shadow;
    e->skip_binary = skip_binary;
    e->engine = std::make_unique<watch_engine>(path, shadow, make_log(path), patterns, skip_binary);
    e->engine->start();
    SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)path.c_str());
    g_entries.push_back(std::move(e));
    update_status();
}

std::wstring make_shadow_path(const std::wstring& watch_path, const std::wstring& shadows_base) {
    std::wstring name = watch_engine::shadow_name(watch_path);
    std::wstring shadow = shadows_base + L"\\" + name;
    int counter = 1;
    std::error_code ec;
    while (fs::exists(shadow, ec)) {
        ++counter;
        shadow = shadows_base + L"\\" + name + L"_" + std::to_wstring(counter);
    }
    return shadow;
}

void save_state() {
    for (auto& loc : g_backup_locations) {
        std::error_code ec;
        fs::create_directories(loc, ec);
    }
    json::value cfg;
    if (!load_config(cfg))
        cfg = json::value::make_object();
    json::value arr = json::value::make_array();
    for (auto& e : g_entries) {
        json::value o = json::value::make_object();
        o.set("path", json::value(to_utf8(e->path)));
        o.set("shadow", json::value(to_utf8(e->shadow)));
        o.set("paused", json::value(e->paused));
        if (e->skip_binary)
            o.set("skip_binary", json::value(true));
        arr.arr->push_back(o);
    }
    {
        json::value locs = json::value::make_array();
        for (auto& loc : g_backup_locations)
            locs.arr->push_back(json::value(to_utf8(loc)));
        cfg.set("backup_locations", locs);
    }
    cfg.set("watched", arr);
    json::value preset_arr = json::value::make_array();
    for (auto& [name, pats] : g_presets) {
        json::value po = json::value::make_object();
        po.set("name", json::value(name));
        json::value pa = json::value::make_array();
        for (auto& p : pats)
            pa.arr->push_back(json::value(p));
        po.set("patterns", pa);
        preset_arr.arr->push_back(po);
    }
    cfg.set("presets", preset_arr);
    if (g_main && IsWindow(g_main)) {
        RECT r;
        if (GetWindowRect(g_main, &r)) {
            json::value sz = json::value::make_array();
            sz.arr->push_back(json::value((int)r.left));
            sz.arr->push_back(json::value((int)r.top));
            sz.arr->push_back(json::value((int)(r.right - r.left)));
            sz.arr->push_back(json::value((int)(r.bottom - r.top)));
            cfg.set("main_size", sz);
        }
    }
    cfg.set("splitter_x", json::value(g_splitter_x));
    {
        json::value pd = json::value::make_array();
        for (auto& p : g_pending_deletions)
            pd.arr->push_back(json::value(to_utf8(p)));
        cfg.set("pending_deletions", pd);
    }
    save_config(cfg);
}

void load_state() {
    json::value cfg;
    if (!load_config(cfg))
        return;
    if (auto* pr = cfg.find("presets")) {
        if (pr->is_array() && pr->arr) {
            g_presets.clear();
            for (auto& item : *pr->arr) {
                std::string name;
                std::vector<std::string> pats;
                if (auto* n = item.find("name"))
                    name = n->as_string();
                if (auto* pa = item.find("patterns")) {
                    if (pa->is_array() && pa->arr)
                        for (auto& p : *pa->arr)
                            pats.push_back(p.as_string());
                }
                if (!name.empty())
                    g_presets.push_back({ name, pats });
            }
        }
    }
    if (auto* locs = cfg.find("backup_locations")) {
        if (locs->is_array() && locs->arr) {
            for (auto& item : *locs->arr)
                g_backup_locations.push_back(to_wide(item.as_string()));
        }
    }
    // Backward compat: old single shadows_base
    if (g_backup_locations.empty()) {
        if (auto* sb = cfg.find("shadows_base")) {
            std::string s = sb->as_string();
            if (!s.empty())
                g_backup_locations.push_back(to_wide(s));
        }
    }
    if (g_backup_locations.empty())
        return;
    if (auto* pd = cfg.find("pending_deletions")) {
        if (pd->is_array() && pd->arr) {
            for (auto& item : *pd->arr)
                if (item.is_string())
                    g_pending_deletions.push_back(to_wide(item.as_string()));
        }
    }
    if (auto* w = cfg.find("watched")) {
        if (w->is_array() && w->arr) {
            for (auto& item : *w->arr) {
                std::wstring path;
                std::wstring shadow;
                bool paused = false;
                bool skip_binary = false;
                if (item.is_string()) {
                    path = to_wide(item.str);
                } else {
                    if (auto* p = item.find("path"))
                        path = to_wide(p->as_string());
                    if (auto* s = item.find("shadow"))
                        shadow = to_wide(s->as_string());
                    if (auto* pa = item.find("paused"))
                        paused = pa->as_bool();
                    if (auto* sb = item.find("skip_binary"))
                        skip_binary = sb->as_bool();
                }
                if (path.empty())
                    continue;
                std::error_code ec;
                if (!fs::is_directory(path, ec))
                    continue;
                if (shadow.empty())
                    shadow = make_shadow_path(path, g_backup_locations[0]);
                start_engine(path, shadow, {}, skip_binary);
                if (paused) {
                    auto& e = g_entries.back();
                    e->engine->pause();
                    e->paused = true;
                    list_set_text((int)g_entries.size() - 1, path + L"  (paused)");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

int selected_index() {
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    return sel == LB_ERR ? -1 : sel;
}

struct loc_state {
    HWND lb;
    bool done = false;
};

LRESULT CALLBACK loc_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    loc_state* st = (loc_state*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        st = (loc_state*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;
        st->lb = CreateWindowW(L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
            12, 12, cw - 24, ch - 60, hwnd, (HMENU)101, GetModuleHandleW(nullptr), nullptr);
        for (auto& loc : g_backup_locations)
            SendMessageW(st->lb, LB_ADDSTRING, 0, (LPARAM)loc.c_str());
        CreateWindowW(L"BUTTON", L"Add...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            12, ch - 40, 75, 24, hwnd, (HMENU)102, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            95, ch - 40, 75, 24, hwnd, (HMENU)103, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            cw - 95, ch - 40, 75, 24, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK || id == IDCANCEL) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == 102) {
            IFileOpenDialog* fdlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fdlg)))) {
                DWORD opts = 0;
                fdlg->GetOptions(&opts);
                fdlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                if (SUCCEEDED(fdlg->Show(hwnd))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(fdlg->GetResult(&item))) {
                        PWSTR p = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                            std::error_code ec;
                            std::wstring abs = fs::absolute(fs::path(p), ec).wstring();
                            CoTaskMemFree(p);
                            bool dup = false;
                            for (auto& loc : g_backup_locations)
                                if (fs::equivalent(fs::path(loc), fs::path(abs), ec)) { dup = true; break; }
                            if (!dup) {
                                g_backup_locations.push_back(abs);
                                SendMessageW(st->lb, LB_ADDSTRING, 0, (LPARAM)abs.c_str());
                                save_state();
                            }
                        }
                        item->Release();
                    }
                }
                fdlg->Release();
            }
            return 0;
        }
        if (id == 103) {
            int sel = (int)SendMessageW(st->lb, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_backup_locations.size() && g_backup_locations.size() > 1) {
                g_backup_locations.erase(g_backup_locations.begin() + sel);
                SendMessageW(st->lb, LB_DELETESTRING, sel, 0);
                save_state();
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void manage_locations() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = loc_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconManageLocDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    loc_state st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    int cw = 450, ch = 300;
    SIZE ws = window_size_for_client(cw, ch, style, WS_EX_DLGMODALFRAME, g_main);
    RECT pr;
    GetWindowRect(g_main, &pr);
    int x = pr.left + ((pr.right - pr.left) - ws.cx) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - ws.cy) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BaconManageLocDlg", L"Manage Backup Locations",
        style, x, y, ws.cx, ws.cy, g_main, nullptr, GetModuleHandleW(nullptr), &st);
    run_modal(g_main, dlg);
}

void add_directory() {
    if (g_backup_locations.empty()) {
        int r = MessageBoxW(
            g_main, L"Set a backup location before adding directories.\n\nSet one now?", L"Backup Location Required",
            MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES)
            manage_locations();
        if (g_backup_locations.empty())
            return;
    }
    std::wstring path;
    std::vector<std::string> patterns;
    bool skip_binary = false;
    std::wstring shadows_base;
    if (!show_add_directory_dialog(g_main, g_backup_locations, path, patterns, skip_binary, shadows_base))
        return;
    if (path.empty())
        return;
    for (auto& e : g_entries) {
        if (e->path == path) {
            MessageBoxW(
                g_main, (path + L" is already being watched.").c_str(), L"Already Watching", MB_ICONINFORMATION);
            return;
        }
    }
    // Prevent watching inside the repo location or vice versa
    {
        std::error_code ec;
        fs::path wp = fs::absolute(fs::path(path), ec);
        fs::path rp = fs::absolute(fs::path(shadows_base), ec);
        if (!ec && wp.root_name() == rp.root_name()) {
            auto inside = [&](const fs::path& a, const fs::path& b) -> bool {
                auto rel = fs::relative(a, b, ec);
                if (ec)
                    return false;
                auto s = rel.wstring();
                return s == L"." || s.size() < 2 || s[0] != L'.' || s[1] != L'.';
            };
            if (!ec && (inside(wp, rp) || inside(rp, wp))) {
                MessageBoxW(g_main, L"The watched directory cannot be the same as (or inside) the backup repo location.",
                            L"Invalid Directory", MB_ICONWARNING);
                return;
            }
        }
    }
    auto [maj, min, pat] = git_version();
    if (maj < 2 || (maj == 2 && min < 37)) {
        std::wstring msg = L"Git version " + std::to_wstring(maj) + L"." + std::to_wstring(min) +
                           L" detected.\n\nFile-system monitor (core.fsmonitor) requires Git 2.37 or newer.\n"
                           L"Without it, git status may be slower on very large directories.\n\nProceed anyway?";
        if (MessageBoxW(g_main, msg.c_str(), L"Git Version Notice", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    std::wstring shadow = make_shadow_path(path, shadows_base);
    start_engine(path, shadow, patterns, skip_binary);
    log_local("Added: " + to_utf8(path));
    save_state();
}

void remove_directory() {
    if (g_busy)
        return;
    log_local("Remove clicked");
    int idx = selected_index();
    if (idx < 0)
        return;
    auto& e = g_entries[idx];
    if (e->engine->is_stopping())
        return;
    g_busy = true;
    std::wstring path = e->path;
    std::wstring msg = L"Stop watching " + path +
                       L"?\n\n"
                       L"This will delete the backup history (shadow repo) for this directory.\n"
                       L"The original files are NOT affected.";
    if (MessageBoxW(g_main, msg.c_str(), L"Remove Directory", MB_YESNO | MB_ICONWARNING) != IDYES) {
        g_busy = false;
        return;
    }
    // Stop the engine (fast — cancellation is immediate now)
    std::wstring shadow = e->engine->shadow_path();
    e->engine->stop();

    // Remove from the UI and config instantly — don't wait for filesystem cleanup.
    g_entries.erase(g_entries.begin() + idx);
    SendMessageW(g_list, LB_DELETESTRING, idx, 0);
    log_local("Removed: " + to_utf8(path));
    save_state();
    update_status();

    // Mark for background deletion — reaps orphaned dirs even if app shuts down before thread finishes.
    g_pending_deletions.push_back(shadow);
    save_state();
    start_pending_cleanup();

    g_busy = false;
}

void toggle_pause() {
    if (g_busy)
        return;
    int idx = selected_index();
    if (idx < 0 && g_entries.size() == 1)
        idx = 0;
    if (idx < 0)
        return;
    auto& e = g_entries[idx];
    if (e->engine->is_stopping())
        return;
    g_busy = true;
    if (e->paused) {
        e->engine->resume();
        e->paused = false;
        list_set_text(idx, e->path);
    } else {
        e->engine->pause();
        e->paused = true;
        list_set_text(idx, e->path + L"  (paused)");
    }
    save_state();
    g_busy = false;
}

void edit_ignores() {
    int idx = selected_index();
    if (idx < 0 && g_entries.size() == 1)
        idx = 0;
    if (idx < 0) {
        MessageBoxW(g_main, L"Select a directory first.", L"No Selection", MB_ICONINFORMATION);
        return;
    }
    auto& e = g_entries[idx];
    std::vector<std::string> out;
    if (show_ignore_dialog(g_main, e->engine->ignore().patterns(), out)) {
        e->engine->ignore().set_patterns(out);
        e->engine->sync_exclude();
        std::string label = to_utf8(fs::path(e->path).filename().wstring());
        console_append(to_wide("[" + label + "] Ignore patterns updated."));
        file_log("[" + label + "] Ignore patterns updated.");
    }
}

void restore_dialog() {
    if (g_entries.empty())
        return;
    int idx = selected_index();
    if (idx < 0 && g_entries.size() == 1)
        idx = 0;
    if (idx < 0) {
        MessageBoxW(g_main, L"Select a directory first.", L"No Selection", MB_ICONINFORMATION);
        return;
    }
    auto& e = g_entries[idx];
    fs::path shadow = e->engine->shadow_path();
    if (!fs::exists(shadow / ".git")) {
        MessageBoxW(g_main, L"No shadow repo found for this directory.", L"No History", MB_ICONWARNING);
        return;
    }
    show_restore_dialog(g_main, e->path, e->engine->shadow_path());
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

void tray_add(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, 0);
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"BaconSaver");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void tray_remove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void show_from_tray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

void quit_app(HWND) {
    if (g_quitting) {
        file_log("quit_app: already quitting");
        return;
    }
    g_quitting = true;
    g_busy = true;
    file_log("quit_app: shutting down, " + std::to_string(g_entries.size()) + " engines");
    tray_remove();
    save_state();
    for (int i = (int)g_entries.size() - 1; i >= 0; --i) {
        if (g_entries[i]->engine->is_stopping()) {
            file_log("quit_app: engine " + std::to_string(i) + " already stopping, skip");
            continue;
        }
        file_log("quit_app: stopping engine " + std::to_string(i));
        g_entries[i]->engine->stop();
        file_log("quit_app: engine " + std::to_string(i) + " stopped");
    }
    g_entries.clear();
    file_log("quit_app: calling PostQuitMessage");
    PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void layout(HWND hwnd) {
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = scale_px(8, hwnd);
    int sh = scale_px(22, hwnd);
    int lbl_h = scale_px(18, hwnd);
    int splitter_w = scale_px(4, hwnd);
    int btn_h = scale_px(28, hwnd);
    int btn_gap = scale_px(4, hwnd);
    int left_w = scale_px(g_splitter_x, hwnd);
    int body_top = m + lbl_h;
    int body_h = H - sh - body_top - m;

    int btn_area = 7 * (btn_h + btn_gap);
    int list_h = body_h - btn_area - btn_gap;
    if (list_h < scale_px(60, hwnd))
        list_h = scale_px(60, hwnd);

    MoveWindow(GetDlgItem(hwnd, 900), m, m, left_w, lbl_h, TRUE);
    MoveWindow(g_list, m, body_top, left_w, list_h, TRUE);

    int by = body_top + list_h + btn_gap;
    int ids[7] = { ID_ADD, ID_REMOVE, ID_PAUSE, ID_IGNORES, ID_RESTORE, ID_REPO, ID_CONFIG };
    for (int i = 0; i < 7; ++i) {
        MoveWindow(GetDlgItem(hwnd, ids[i]), m, by, left_w, btn_h, TRUE);
        by += btn_h + btn_gap;
    }

    int cx = m + left_w + splitter_w + m;
    MoveWindow(g_console, cx, m, W - cx - m, H - sh - m * 2, TRUE);
    MoveWindow(g_status, 0, H - sh, W, sh, TRUE);
    console_update_scroll();
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_show_msg && g_show_msg != 0) {
        show_from_tray(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        HINSTANCE hi = GetModuleHandleW(nullptr);

        CreateWindowExW(
            0, L"STATIC", L"Watched Directories", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)900, hi, nullptr);
        g_list = CreateWindowExW(
            0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 10, 10,
            hwnd, (HMENU)ID_LIST, hi, nullptr);

        struct {
            int id;
            const wchar_t* text;
        } btns[] = {
            { ID_ADD, L"Add Directory..." },    { ID_REMOVE, L"Remove" },      { ID_PAUSE, L"Pause / Resume" },
            { ID_IGNORES, L"Edit Ignores..." }, { ID_RESTORE, L"Restore..." }, { ID_REPO, L"Manage Backup Locations..." },
            { ID_CONFIG, L"Edit Config..." },
        };
        for (auto& b : btns)
            CreateWindowExW(
                0, L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)b.id, hi,
                nullptr);

        g_console = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10,
            10, hwnd, nullptr, hi, nullptr);
        SendMessageW(g_console, EM_SETLIMITTEXT, 0, 0);

        g_status = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, hi,
            nullptr);

        // Fonts
        HFONT gui = CreateFontW(
            -MulDiv(11, dpi_for(hwnd), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, L"MS Shell Dlg 2");
        SendMessageW(GetDlgItem(hwnd, 900), WM_SETFONT, (WPARAM)gui, TRUE);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)gui, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)gui, TRUE);
        for (auto& b : btns)
            SendMessageW(GetDlgItem(hwnd, b.id), WM_SETFONT, (WPARAM)gui, TRUE);
        g_mono_font = CreateFontW(
            -scale_px(15, hwnd), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_console, WM_SETFONT, (WPARAM)g_mono_font, TRUE);

        tray_add(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (wp == SIZE_RESTORED) {
            RECT r;
            if (GetWindowRect(hwnd, &r)) {
                g_window_w = r.right - r.left;
                g_window_h = r.bottom - r.top;
            }
        }
        layout(hwnd);
        return 0;
    case WM_SETCURSOR: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int m = scale_px(8, hwnd);
        int sh = scale_px(22, hwnd);
        int lbl_h = scale_px(18, hwnd);
        int splitter_w = scale_px(4, hwnd);
        int left_w = scale_px(g_splitter_x, hwnd);
        int gutter_x = m + left_w;
        int body_top = m + lbl_h;
        int body_h = rc.bottom - sh - body_top - m;
        RECT gutter = { gutter_x, body_top, gutter_x + splitter_w, body_top + body_h };
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&gutter, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
    } break;
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int m = scale_px(8, hwnd);
        int sh = scale_px(22, hwnd);
        int lbl_h = scale_px(18, hwnd);
        int splitter_w = scale_px(4, hwnd);
        int left_w = scale_px(g_splitter_x, hwnd);
        int gutter_x = m + left_w;
        int body_top = m + lbl_h;
        int body_h = rc.bottom - sh - body_top - m;
        RECT gutter = { gutter_x, body_top, gutter_x + splitter_w, body_top + body_h };
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (PtInRect(&gutter, pt)) {
            g_dragging = true;
            SetCapture(hwnd);
            return 0;
        }
    } break;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int x = LOWORD(lp) - scale_px(3, hwnd);
            int min_x = scale_px(80, hwnd);
            int max_x = rc.right - scale_px(280, hwnd);
            if (x < min_x)
                x = min_x;
            if (x > max_x)
                x = max_x;
            int logical = MulDiv(x, 96, dpi_for(hwnd));
            if (logical != g_splitter_x) {
                g_splitter_x = logical;
                layout(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        if ((HWND)lp == g_console) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, COL_TEXT);
            SetBkColor(dc, COL_BG);
            return (LRESULT)g_dark_brush;
        }
        break;
    }
    case WM_APP_LOG: {
        auto* s = reinterpret_cast<std::string*>(lp);
        if (s) {
            console_append(to_wide(*s));
            file_log(*s);
            delete s;
        }
        return 0;
    }
    case WM_TRAYICON: {
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            show_from_tray(hwnd);
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
            SetForegroundWindow(hwnd);
            int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == ID_TRAY_SHOW)
                show_from_tray(hwnd);
            else if (cmd == ID_TRAY_QUIT)
                quit_app(hwnd);
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case ID_ADD:
            add_directory();
            return 0;
        case ID_REMOVE:
            remove_directory();
            return 0;
        case ID_PAUSE:
            toggle_pause();
            return 0;
        case ID_IGNORES:
            edit_ignores();
            return 0;
        case ID_RESTORE:
            restore_dialog();
            return 0;
        case ID_REPO:
            manage_locations();
            return 0;
        case ID_CONFIG:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", config_path().wstring().c_str(), nullptr, SW_SHOW);
            return 0;
        case ID_TRAY_SHOW:
            show_from_tray(hwnd);
            return 0;
        case ID_TRAY_QUIT:
            quit_app(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        quit_app(hwnd);
        return 0;
    case WM_DESTROY:
        save_state();
        if (g_mono_font)
            DeleteObject(g_mono_font);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_show_msg = RegisterWindowMessageW(L"BaconSaver-Show-Window");

    HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(MAIN_CLASS, nullptr);
        if (existing)
            PostMessageW(existing, g_show_msg, 0, 0);
        else
            MessageBoxW(nullptr, L"BaconSaver is already running.", L"BaconSaver", MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    g_dark_brush = CreateSolidBrush(COL_BG);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MAIN_CLASS;
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    RegisterClassW(&wc);

    // Restore saved window size/position and splitter before creating window
    int main_w = MulDiv(1000, GetDpiForSystem(), 96);
    int main_h = MulDiv(600, GetDpiForSystem(), 96);
    int main_x = CW_USEDEFAULT, main_y = CW_USEDEFAULT;
    {
        json::value cfg;
        if (load_config(cfg)) {
            if (auto* s = cfg.find("main_size")) {
                if (s->is_array() && s->arr && s->arr->size() >= 4) {
                    main_x = (*s->arr)[0].as_int(main_x);
                    main_y = (*s->arr)[1].as_int(main_y);
                    main_w = (*s->arr)[2].as_int(main_w);
                    main_h = (*s->arr)[3].as_int(main_h);
                }
            }
            if (auto* sx = cfg.find("splitter_x"))
                g_splitter_x = sx->as_int(g_splitter_x);
        }
    }

    HWND hwnd = CreateWindowExW(
        0, MAIN_CLASS, L"BaconSaver", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, main_x, main_y, main_w, main_h,
        nullptr, nullptr, hInstance, nullptr);

    file_log("BaconSaver started");
    load_state();
    for (auto& loc : g_backup_locations)
        file_log("Repo location: " + to_utf8(loc));
    for (auto& p : g_pending_deletions)
        file_log("Pending cleanup: " + to_utf8(p));
    start_pending_cleanup();
    update_status();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // First-run: no config yet -> offer to set repo location.
    if (!fs::exists(config_path())) {
        int r = MessageBoxW(
            hwnd,
            L"No configuration found.\n\n"
            L"Choose a location for the backup repository.\n"
            L"This is where file history (git repos) will be stored.",
            L"Welcome to BaconSaver", MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES)
            manage_locations();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_dark_brush)
        DeleteObject(g_dark_brush);
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return (int)msg.wParam;
}
