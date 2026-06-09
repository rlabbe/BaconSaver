#include "dialogs.h"
#include "engine.h"
#include "json.h"
#include "util.h"

#include <commctrl.h>
#include <ctime>
#include <fstream>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants / IDs
// ---------------------------------------------------------------------------

namespace {

const wchar_t* MAIN_CLASS = L"BaconSaverMain";
const wchar_t* MUTEX_NAME = L"BaconSaver-SingleInstance-Mutex";
const UINT WM_APP_LOG = WM_APP + 1;
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
    ID_STATUS,
    ID_TRAY_SHOW = 2001,
    ID_TRAY_QUIT = 2002,
};

struct entry {
    std::wstring path;
    std::unique_ptr<watch_engine> engine;
    bool paused = false;
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
std::vector<std::unique_ptr<entry>> g_entries;
std::wstring g_shadows_base;
bool g_have_base = false;
NOTIFYICONDATAW g_nid = {};
UINT g_show_msg = 0; // registered message used to surface a second instance

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

std::string now_ts() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void file_log(const std::string& msg) {
    std::error_code ec;
    fs::path dir = app_dir();
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "baconsaver.log", std::ios::app | std::ios::binary);
    f << now_ts() << "  " << msg << "\n";
}

void console_append(const std::wstring& line) {
    int len = GetWindowTextLengthW(g_console);
    SendMessageW(g_console, EM_SETSEL, len, len);
    std::wstring l = line + L"\r\n";
    SendMessageW(g_console, EM_REPLACESEL, FALSE, (LPARAM)l.c_str());
}

void log_local(const std::string& msg) {
    console_append(to_wide("[BaconSaver] " + msg));
    file_log("[BaconSaver] " + msg);
}

// Engine callback factory — thread-safe (posts to the UI thread).
log_fn make_log(const std::wstring& path) {
    std::string label = to_utf8(fs::path(path).filename().wstring());
    return [label](const std::string& msg) {
        auto* s = new std::string("[" + label + "] " + msg);
        if (!PostMessageW(g_main, WM_APP_LOG, 0, (LPARAM)s))
            delete s;
    };
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

fs::path config_path() {
    return app_dir() / "config.json";
}

bool load_config_value(json::value& out) {
    std::ifstream in(config_path(), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str(), out);
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

void start_engine(const std::wstring& path, const std::vector<std::string>& patterns, bool skip_binary = false) {
    auto e = std::make_unique<entry>();
    e->path = path;
    e->engine = std::make_unique<watch_engine>(path, g_shadows_base, make_log(path), patterns, skip_binary);
    e->engine->start();
    SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)path.c_str());
    g_entries.push_back(std::move(e));
    update_status();
}

void save_config() {
    if (g_have_base) {
        std::error_code ec;
        fs::create_directories(g_shadows_base, ec);
    }
    json::value cfg;
    if (!load_config_value(cfg))
        cfg = json::value::make_object();
    json::value arr = json::value::make_array();
    for (auto& e : g_entries) {
        json::value o = json::value::make_object();
        o.set("path", json::value(to_utf8(e->path)));
        o.set("paused", json::value(e->paused));
        arr.arr->push_back(o);
    }
    if (g_have_base)
        cfg.set("shadows_base", json::value(to_utf8(g_shadows_base)));
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
    std::ofstream out(config_path(), std::ios::binary);
    out << json::dump(cfg, 2);
}

void load_config() {
    json::value cfg;
    if (!load_config_value(cfg))
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
    if (auto* sb = cfg.find("shadows_base")) {
        std::string s = sb->as_string();
        if (!s.empty()) {
            g_shadows_base = to_wide(s);
            g_have_base = true;
        }
    }
    if (!g_have_base)
        return;
    if (auto* w = cfg.find("watched")) {
        if (w->is_array() && w->arr) {
            for (auto& item : *w->arr) {
                std::wstring path;
                bool paused = false;
                if (item.is_string()) {
                    path = to_wide(item.str);
                } else {
                    if (auto* p = item.find("path"))
                        path = to_wide(p->as_string());
                    if (auto* pa = item.find("paused"))
                        paused = pa->as_bool();
                }
                if (path.empty())
                    continue;
                std::error_code ec;
                if (!fs::is_directory(path, ec))
                    continue;
                start_engine(path, {});
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

void set_repo_location() {
    // Reuse the folder picker indirectly via the add dialog's picker is internal;
    // implement directly here.
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring chosen;
    if (SUCCEEDED(dlg->Show(g_main))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                chosen = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (chosen.empty())
        return;

    std::error_code ec;
    std::wstring new_base = fs::absolute(fs::path(chosen), ec).wstring();
    if (g_have_base && new_base == g_shadows_base)
        return;
    if (!g_entries.empty()) {
        int r = MessageBoxW(
            g_main,
            L"Changing the repo location only affects newly added directories.\n"
            L"Existing watched directories keep their current repos.\n\nContinue?",
            L"Repo Location Changed", MB_YESNO | MB_ICONQUESTION);
        if (r != IDYES)
            return;
    }
    g_shadows_base = new_base;
    g_have_base = true;
    save_config();
    log_local("Repo location set to: " + to_utf8(new_base));
    if (g_entries.empty())
        SetWindowTextW(g_status, (L"Repo location: " + new_base).c_str());
}

void add_directory() {
    if (!g_have_base) {
        int r = MessageBoxW(
            g_main, L"Set the repo location before adding directories.\n\nSet it now?", L"Repo Location Required",
            MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES)
            set_repo_location();
        if (!g_have_base)
            return;
    }
    std::wstring path;
    std::vector<std::string> patterns;
    bool skip_binary = false;
    if (!show_add_directory_dialog(g_main, path, patterns, skip_binary))
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
    auto [maj, min, pat] = git_version();
    if (maj < 2 || (maj == 2 && min < 37)) {
        std::wstring msg = L"Git version " + std::to_wstring(maj) + L"." + std::to_wstring(min) +
                           L" detected.\n\nFile-system monitor (core.fsmonitor) requires Git 2.37 or newer.\n"
                           L"Without it, git status may be slower on very large directories.\n\nProceed anyway?";
        if (MessageBoxW(g_main, msg.c_str(), L"Git Version Notice", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    start_engine(path, patterns, skip_binary);
    log_local("Added: " + to_utf8(path));
    save_config();
}

void remove_directory() {
    int idx = selected_index();
    if (idx < 0)
        return;
    std::wstring path = g_entries[idx]->path;
    std::wstring msg = L"Stop watching " + path +
                       L"?\n\n"
                       L"This will delete the backup history (shadow repo) for this directory.\n"
                       L"The original files are NOT affected.";
    if (MessageBoxW(g_main, msg.c_str(), L"Remove Directory", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    std::wstring shadow = g_entries[idx]->engine->shadow_path();
    g_entries[idx]->engine->stop();
    g_entries.erase(g_entries.begin() + idx);
    SendMessageW(g_list, LB_DELETESTRING, idx, 0);
    std::error_code ec;
    fs::remove_all(shadow, ec);
    if (ec)
        log_local("WARNING: could not delete shadow repo: " + to_utf8(shadow));
    else
        log_local("Removed: " + to_utf8(path) + " (shadow repo deleted)");
    save_config();
    update_status();
}

void toggle_pause() {
    int idx = selected_index();
    if (idx < 0)
        return;
    auto& e = g_entries[idx];
    if (e->paused) {
        e->engine->resume();
        e->paused = false;
        list_set_text(idx, e->path);
    } else {
        e->engine->pause();
        e->paused = true;
        list_set_text(idx, e->path + L"  (paused)");
    }
    save_config();
}

void edit_ignores() {
    int idx = selected_index();
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
    if (!g_have_base)
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
    file_log("BaconSaver shutting down");
    tray_remove();
    save_config();
    for (auto& e : g_entries)
        e->engine->stop();
    g_entries.clear();
    PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int margin = 8;
    int status_h = 22;
    int left_w = g_splitter_x;
    int splitter_w = 6;
    int body_top = margin + 18; // below the "Watched Directories" label
    int body_h = H - status_h - body_top - margin;

    const int btn_h = 28, btn_gap = 4;
    int btn_area = 6 * (btn_h + btn_gap);
    int list_h = body_h - btn_area - btn_gap;
    if (list_h < 60)
        list_h = 60;

    MoveWindow(GetDlgItem(hwnd, 900), margin, margin, left_w, 18, TRUE); // label
    MoveWindow(g_list, margin, body_top, left_w, list_h, TRUE);

    int by = body_top + list_h + btn_gap;
    int ids[6] = { ID_ADD, ID_REMOVE, ID_PAUSE, ID_IGNORES, ID_RESTORE, ID_REPO };
    for (int i = 0; i < 6; ++i) {
        MoveWindow(GetDlgItem(hwnd, ids[i]), margin, by, left_w, btn_h, TRUE);
        by += btn_h + btn_gap;
    }

    int cx = margin + left_w + splitter_w + margin;
    MoveWindow(g_console, cx, margin, W - cx - margin, H - status_h - margin * 2, TRUE);
    MoveWindow(g_status, 0, H - status_h, W, status_h, TRUE);
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
            { ID_IGNORES, L"Edit Ignores..." }, { ID_RESTORE, L"Restore..." }, { ID_REPO, L"Set Repo Location..." },
        };
        for (auto& b : btns)
            CreateWindowExW(
                0, L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)b.id, hi,
                nullptr);

        g_console = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 10,
            10, hwnd, nullptr, hi, nullptr);
        SendMessageW(g_console, EM_SETLIMITTEXT, 0, 0);

        g_status = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, hi,
            nullptr);

        // Fonts
        HFONT gui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(GetDlgItem(hwnd, 900), WM_SETFONT, (WPARAM)gui, TRUE);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)gui, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)gui, TRUE);
        for (auto& b : btns)
            SendMessageW(GetDlgItem(hwnd, b.id), WM_SETFONT, (WPARAM)gui, TRUE);
        g_mono_font = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_console, WM_SETFONT, (WPARAM)g_mono_font, TRUE);

        tray_add(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        layout(hwnd);
        return 0;
    case WM_SETCURSOR: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int gutter_x = 8 + g_splitter_x;
        int gutter_w = 6;
        int body_top = 8 + 18;
        int body_h = rc.bottom - 22 - body_top - 8;
        RECT gutter = { gutter_x, body_top, gutter_x + gutter_w, body_top + body_h };
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
        int gutter_x = 8 + g_splitter_x;
        int gutter_w = 6;
        int body_top = 8 + 18;
        int body_h = rc.bottom - 22 - body_top - 8;
        RECT gutter = { gutter_x, body_top, gutter_x + gutter_w, body_top + body_h };
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
            int x = LOWORD(lp) - 3;
            if (x < 80)
                x = 80;
            if (x > rc.right - 280)
                x = rc.right - 280;
            if (x != g_splitter_x) {
                g_splitter_x = x;
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
            set_repo_location();
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
        if (g_mono_font)
            DeleteObject(g_mono_font);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anonymous namespace

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

    HWND hwnd = CreateWindowExW(
        0, MAIN_CLASS, L"BaconSaver", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 600,
        nullptr, nullptr, hInstance, nullptr);

    file_log("BaconSaver started");
    load_config();
    if (g_have_base)
        file_log("Repo location: " + to_utf8(g_shadows_base));
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
            set_repo_location();
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
