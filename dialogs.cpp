#include "dialogs.h"
#include "engine.h"
#include "json.h"
#include "util.h"

#include <algorithm>
#include <commctrl.h>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <richedit.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <sstream>

#pragma comment(lib, "comctl32.lib")

namespace {
// ---- generic (shared by simple dialogs) ----
enum {
    ID_SIMPLE_TEXT = 100
};

// ---- Add Directory dialog ----
enum {
    IDC_ADD_BROWSE = 200,
    IDC_ADD_PRESET_BASE = 300,
    IDC_ADD_PATTERNS = 400,
    IDC_ADD_ADD = 401,
    IDC_ADD_REMOVE = 402,
    IDC_ADD_PREVIEW = 403,
    IDC_ADD_SKIP_BINARY = 500,
};

// ---- Ignore dialog ----
enum {
    IDC_IGN_PRESET_BASE = 300,
    IDC_IGN_PATTERNS = 400,
    IDC_IGN_ADD = 401,
    IDC_IGN_REMOVE = 402,
};

// ---- Restore dialog ----
enum {
    IDC_RES_SNAP_LBL = 500,
    IDC_RES_COMMITS = 501,
    IDC_RES_MODE_CHANGED = 502,
    IDC_RES_MODE_ALL = 503,
    IDC_RES_SEL_ALL = 504,
    IDC_RES_SEL_NONE = 505,
    IDC_RES_FILES = 506,
    IDC_RES_VIEW_CONTENT = 507,
    IDC_RES_FONT = 508,
    IDC_RES_VIEW_DIFF = 509,
    IDC_RES_PREVIEW = 510,
    IDC_RES_COUNT_LBL = 511,
    IDC_RES_EXPORT = 512,
};
} // anonymous namespace
#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// Colors (matching the PyQt dark theme)
// ---------------------------------------------------------------------------

namespace {

const COLORREF COL_BG = RGB(0x1e, 0x1e, 0x1e);
const COLORREF COL_TEXT = RGB(0xcc, 0xcc, 0xcc);
const COLORREF COL_HEADER = RGB(0x56, 0x9c, 0xd6);
const COLORREF COL_HUNK = RGB(0xc5, 0x86, 0xc0);
const COLORREF COL_ADD = RGB(0x4e, 0xc9, 0xb0);
const COLORREF COL_DEL = RGB(0xf4, 0x47, 0x47);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ')
        ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        --b;
    return s.substr(a, b - a);
}

bool fnmatch(const std::string& pattern, const std::string& str) {
    if (pattern.find('[') != std::string::npos)
        return PathMatchSpecA(str.c_str(), pattern.c_str()) == TRUE;
    size_t pi = 0, si = 0, pstar = std::string::npos, sstar = 0;
    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == str[si])) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            pstar = pi++;
            sstar = si;
        } else if (pstar != std::string::npos) {
            pi = pstar + 1;
            si = ++sstar;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;
    return pi == pattern.size();
}

HFONT ui_font() {
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

void set_font(HWND h, HFONT f) {
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

HWND make(LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h,
          HWND parent, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    set_font(c, ui_font());
    return c;
}

// Generic modal pump: disable parent, show dlg, run until it is destroyed.
void run_modal(HWND parent, HWND dlg) {
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// Compute the window size needed to obtain a given client area.
SIZE window_size_for_client(int cw, int ch, DWORD style, DWORD ex_style) {
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, style, FALSE, ex_style);
    return SIZE{ r.right - r.left, r.bottom - r.top };
}

std::vector<std::wstring> listbox_items(HWND lb) {
    std::vector<std::wstring> out;
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        int len = (int)SendMessageW(lb, LB_GETTEXTLEN, i, 0);
        std::wstring s(len, L'\0');
        SendMessageW(lb, LB_GETTEXT, i, (LPARAM)s.data());
        out.push_back(s);
    }
    return out;
}

void listbox_set(HWND lb, const std::vector<std::string>& items) {
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (auto& s : items)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)to_wide(s).c_str());
}

std::vector<std::string> listbox_get_utf8(HWND lb) {
    std::vector<std::string> out;
    for (auto& w : listbox_items(lb))
        out.push_back(to_utf8(w));
    return out;
}

const std::vector<std::string>* preset_patterns(const std::string& name) {
    for (auto& [n, pats] : g_presets)
        if (n == name)
            return &pats;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Folder picker
// ---------------------------------------------------------------------------

bool pick_folder(HWND parent, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return false;
    bool ok = false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(parent))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Text input prompt (replacement for QInputDialog.getText / getInt)
// ---------------------------------------------------------------------------

struct input_state {
    std::wstring title;
    std::wstring prompt;
    std::wstring text;
    bool ok = false;
    HWND edit = nullptr;
};

LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<input_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<input_state*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        make(L"STATIC", st->prompt.c_str(), 0, 12, 12, 340, 18, hwnd, -1);
        st->edit = make(L"EDIT", st->text.c_str(),
                        WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 12, 34, 340, 24, hwnd, ID_SIMPLE_TEXT);
        make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 178, 70, 80, 26, hwnd, IDOK);
        make(L"BUTTON", L"Cancel", WS_TABSTOP, 272, 70, 80, 26, hwnd, IDCANCEL);
        SetFocus(st->edit);
        SendMessageW(st->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int len = GetWindowTextLengthW(st->edit);
            std::wstring s(len, L'\0');
            GetWindowTextW(st->edit, s.data(), len + 1);
            st->text = s;
            st->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            st->ok = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool show_text_input(HWND parent, const std::wstring& title, const std::wstring& prompt,
                     std::wstring& text) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = input_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconInputDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    input_state st;
    st.title = title;
    st.prompt = prompt;
    st.text = text;
    RECT pr;
    GetWindowRect(parent, &pr);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    SIZE ws = window_size_for_client(364, 112, style, WS_EX_DLGMODALFRAME);
    int x = pr.left + 60, y = pr.top + 80;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BaconInputDlg", title.c_str(),
                               style, x, y, ws.cx, ws.cy, parent, nullptr, GetModuleHandleW(nullptr), &st);
    run_modal(parent, dlg);
    if (st.ok)
        text = st.text;
    return st.ok;
}

// ---------------------------------------------------------------------------
// Read-only text viewer (preview file list)
// ---------------------------------------------------------------------------

struct view_state {
    HWND edit = nullptr;
};

LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<view_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<view_state*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        st->edit = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        0, 0, 10, 10, hwnd, ID_SIMPLE_TEXT);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(st->edit, 4, 4, rc.right - 8, rc.bottom - 40, TRUE);
        HWND close = GetDlgItem(hwnd, IDCANCEL);
        if (close)
            MoveWindow(close, rc.right - 92, rc.bottom - 32, 80, 26, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_text_view(HWND parent, const std::wstring& title, const std::wstring& text) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = view_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconViewDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    view_state st;
    HWND dlg = CreateWindowExW(0, L"BaconViewDlg", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 460, parent, nullptr,
                               GetModuleHandleW(nullptr), &st);
    HWND close = make(L"BUTTON", L"Close", WS_TABSTOP, 0, 0, 80, 26, dlg, IDCANCEL);
    (void)close;
    HFONT mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    set_font(st.edit, mono);
    SetWindowTextW(st.edit, text.c_str());
    RECT rc;
    GetClientRect(dlg, &rc);
    SendMessageW(dlg, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    run_modal(parent, dlg);
    DeleteObject(mono);
}

} // anonymous namespace

// ===========================================================================
// Add Directory dialog
// ===========================================================================

namespace {

struct add_state {
    std::wstring path;
    bool have_path = false;
    bool ok = false;
    bool skip_binary = false;
    HWND dir_label = nullptr;
    HWND patterns = nullptr;
    HWND ok_btn = nullptr;
    HWND preview_btn = nullptr;
    std::vector<HWND> preset_checks; // index aligns with g_presets
    std::vector<std::string> result; // captured before the window is destroyed
};

void add_rebuild_from_presets(add_state* st) {
    std::vector<std::string> merged;
    for (size_t i = 0; i < g_presets.size(); ++i) {
        if (SendMessageW(st->preset_checks[i], BM_GETCHECK, 0, 0) != BST_CHECKED)
            continue;
        for (auto& p : g_presets[i].second)
            if (std::find(merged.begin(), merged.end(), p) == merged.end())
                merged.push_back(p);
    }
    listbox_set(st->patterns, merged);
}

void add_preview(HWND hwnd, add_state* st) {
    if (!st->have_path)
        return;
    auto pats = listbox_get_utf8(st->patterns);
    std::vector<std::string> comp = { ".git" };
    std::vector<std::string> path_pats;
    for (auto& p : pats) {
        std::string q = p;
        while (!q.empty() && q.back() == '/')
            q.pop_back();
        if (q.find('/') != std::string::npos)
            path_pats.push_back(q);
        else
            comp.push_back(p);
    }

    std::vector<std::wstring> watched;
    int ignored = 0;
    fs::path root = st->path;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            break;
        const auto& entry = *it;
        std::string name = to_utf8(entry.path().filename().wstring());
        if (entry.is_directory(ec)) {
            for (auto& pat : comp) {
                if (fnmatch(pat, name)) {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }
        if (!entry.is_regular_file(ec))
            continue;
        std::wstring relw = fs::relative(entry.path(), root, ec).wstring();
        std::string rel = to_utf8(relw);
        bool skip = false;
        // component patterns vs each path part
        {
            std::string cur;
            std::vector<std::string> parts;
            for (char c : rel) {
                if (c == '\\' || c == '/') {
                    if (!cur.empty())
                        parts.push_back(cur);
                    cur.clear();
                } else
                    cur += c;
            }
            if (!cur.empty())
                parts.push_back(cur);
            for (auto& pat : comp) {
                for (auto& part : parts)
                    if (fnmatch(pat, part)) {
                        skip = true;
                        break;
                    }
                if (skip)
                    break;
            }
        }
        if (!skip) {
            std::string norm = rel;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            for (auto& pat : path_pats)
                if (fnmatch(pat, norm)) {
                    skip = true;
                    break;
                }
        }
        if (skip)
            ++ignored;
        else
            watched.push_back(relw);
    }

    std::wstring body;
    for (auto& w : watched) {
        body += w;
        body += L"\r\n";
    }
    std::wstring title = L"Preview: " + std::to_wstring(watched.size()) +
                         L" files watched, " + std::to_wstring(ignored) + L" ignored";
    show_text_view(hwnd, title, body);
}

LRESULT CALLBACK add_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<add_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<add_state*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        make(L"STATIC", L"Directory:", 0, 12, 14, 65, 18, hwnd, -1);
        st->dir_label = make(L"STATIC", L"<none>", SS_PATHELLIPSIS, 80, 14, 320, 18, hwnd, -1);
        make(L"BUTTON", L"Browse...", WS_TABSTOP, 406, 10, 90, 26, hwnd, IDC_ADD_BROWSE);

        make(L"STATIC", L"Presets:", 0, 12, 48, 60, 18, hwnd, -1);
        int px = 78;
        for (size_t i = 0; i < g_presets.size(); ++i) {
            HWND cb = make(L"BUTTON", to_wide(g_presets[i].first).c_str(),
                           WS_TABSTOP | BS_AUTOCHECKBOX, px, 46, 90, 22, hwnd, IDC_ADD_PRESET_BASE + (int)i);
            st->preset_checks.push_back(cb);
            px += 96;
        }

        make(L"BUTTON", L"Skip binary files", WS_TABSTOP | BS_AUTOCHECKBOX,
             12, 76, 160, 22, hwnd, IDC_ADD_SKIP_BINARY);

        make(L"STATIC",
             L"Patterns without / match any path component.\r\n"
             L"Patterns with / match the full relative path.\r\n"
             L"Wildcards:  *  ?  [seq]",
             0, 12, 104, 480, 52, hwnd, -1);

        st->patterns = make(L"LISTBOX", L"",
                            WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                            12, 160, 484, 220, hwnd, IDC_ADD_PATTERNS);

        make(L"BUTTON", L"Add...", WS_TABSTOP, 12, 388, 80, 26, hwnd, IDC_ADD_ADD);
        make(L"BUTTON", L"Remove", WS_TABSTOP, 98, 388, 80, 26, hwnd, IDC_ADD_REMOVE);

        st->preview_btn = make(L"BUTTON", L"Preview Files...", WS_TABSTOP, 12, 426, 130, 26, hwnd, IDC_ADD_PREVIEW);
        EnableWindow(st->preview_btn, FALSE);

        st->ok_btn = make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 320, 426, 80, 26, hwnd, IDOK);
        EnableWindow(st->ok_btn, FALSE);
        make(L"BUTTON", L"Cancel", WS_TABSTOP, 414, 426, 80, 26, hwnd, IDCANCEL);

        // All presets checked by default
        for (size_t i = 0; i < g_presets.size(); ++i)
            SendMessageW(st->preset_checks[i], BM_SETCHECK, BST_CHECKED, 0);
        add_rebuild_from_presets(st);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_ADD_BROWSE) {
            std::wstring chosen;
            if (pick_folder(hwnd, chosen)) {
                std::error_code ec;
                fs::path resolved = fs::absolute(fs::path(chosen), ec);
                st->path = resolved.wstring();
                st->have_path = true;
                SetWindowTextW(st->dir_label, st->path.c_str());
                EnableWindow(st->ok_btn, TRUE);
                EnableWindow(st->preview_btn, TRUE);
            }
            return 0;
        }
        if (id >= IDC_ADD_PRESET_BASE && id < IDC_ADD_PRESET_BASE + (int)g_presets.size()) {
            add_rebuild_from_presets(st);
            return 0;
        }
        if (id == IDC_ADD_ADD) {
            std::wstring text;
            if (show_text_input(hwnd, L"Add Pattern", L"Pattern:", text)) {
                std::wstring t = text;
                size_t a = t.find_first_not_of(L" \t");
                if (a != std::wstring::npos) {
                    size_t b = t.find_last_not_of(L" \t");
                    t = t.substr(a, b - a + 1);
                    SendMessageW(st->patterns, LB_ADDSTRING, 0, (LPARAM)t.c_str());
                }
            }
            return 0;
        }
        if (id == IDC_ADD_REMOVE) {
            int sel = (int)SendMessageW(st->patterns, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR)
                SendMessageW(st->patterns, LB_DELETESTRING, sel, 0);
            return 0;
        }
        if (id == IDC_ADD_PREVIEW) {
            add_preview(hwnd, st);
            return 0;
        }
        if (id == IDOK) {
            if (!st->have_path)
                return 0;
            st->result = listbox_get_utf8(st->patterns);
            st->skip_binary = (SendMessageW(GetDlgItem(hwnd, IDC_ADD_SKIP_BINARY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            st->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDCANCEL) {
            st->ok = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anonymous namespace

bool show_add_directory_dialog(HWND parent, std::wstring& out_path,
                               std::vector<std::string>& out_patterns,
                               bool& out_skip_binary) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = add_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconAddDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    add_state st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    SIZE ws = window_size_for_client(512, 470, style, WS_EX_DLGMODALFRAME);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BaconAddDlg", L"Add Watched Directory",
                               style, CW_USEDEFAULT, CW_USEDEFAULT, ws.cx, ws.cy, parent, nullptr,
                               GetModuleHandleW(nullptr), &st);
    // Center on parent
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    GetWindowRect(dlg, &dr);
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
                 pr.left + ((pr.right - pr.left) - w) / 2,
                 pr.top + ((pr.bottom - pr.top) - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    run_modal(parent, dlg);
    if (st.ok) {
        out_path = st.path;
        out_patterns = st.result;
        out_skip_binary = st.skip_binary;
    }
    return st.ok;
}

// ===========================================================================
// Ignore dialog
// ===========================================================================

namespace {

struct ignore_state {
    bool ok = false;
    HWND patterns = nullptr;
    std::vector<std::string> result; // captured before the window is destroyed
};

void ignore_add_preset(ignore_state* st, const std::string& name) {
    auto* pats = preset_patterns(name);
    if (!pats)
        return;
    auto existing = listbox_get_utf8(st->patterns);
    for (auto& p : *pats) {
        if (std::find(existing.begin(), existing.end(), p) == existing.end()) {
            SendMessageW(st->patterns, LB_ADDSTRING, 0, (LPARAM)to_wide(p).c_str());
            existing.push_back(p);
        }
    }
}

LRESULT CALLBACK ignore_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ignore_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<ignore_state*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        make(L"STATIC", L"Add preset:", 0, 12, 14, 80, 18, hwnd, -1);
        int px = 92;
        for (size_t i = 0; i < g_presets.size(); ++i) {
            make(L"BUTTON", to_wide(g_presets[i].first).c_str(),
                 WS_TABSTOP, px, 10, 90, 26, hwnd, IDC_IGN_PRESET_BASE + (int)i);
            px += 96;
        }
        make(L"STATIC",
             L"Patterns without / match any path component.\r\n"
             L"Patterns with / match the full relative path.\r\n"
             L"Wildcards:  *  ?  [seq]",
             0, 12, 44, 430, 52, hwnd, -1);
        st->patterns = make(L"LISTBOX", L"",
                            WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                            12, 100, 434, 230, hwnd, IDC_IGN_PATTERNS);
        make(L"BUTTON", L"Add...", WS_TABSTOP, 12, 338, 80, 26, hwnd, IDC_IGN_ADD);
        make(L"BUTTON", L"Remove", WS_TABSTOP, 98, 338, 80, 26, hwnd, IDC_IGN_REMOVE);
        make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 270, 338, 80, 26, hwnd, IDOK);
        make(L"BUTTON", L"Cancel", WS_TABSTOP, 364, 338, 80, 26, hwnd, IDCANCEL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= IDC_IGN_PRESET_BASE && id < IDC_IGN_PRESET_BASE + (int)g_presets.size()) {
            ignore_add_preset(st, g_presets[id - IDC_IGN_PRESET_BASE].first);
            return 0;
        }
        if (id == IDC_IGN_ADD) {
            std::wstring text;
            if (show_text_input(hwnd, L"Add Pattern", L"Pattern:", text)) {
                size_t a = text.find_first_not_of(L" \t");
                if (a != std::wstring::npos) {
                    size_t b = text.find_last_not_of(L" \t");
                    text = text.substr(a, b - a + 1);
                    SendMessageW(st->patterns, LB_ADDSTRING, 0, (LPARAM)text.c_str());
                }
            }
            return 0;
        }
        if (id == IDC_IGN_REMOVE) {
            int sel = (int)SendMessageW(st->patterns, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR)
                SendMessageW(st->patterns, LB_DELETESTRING, sel, 0);
            return 0;
        }
        if (id == IDOK) {
            st->result = listbox_get_utf8(st->patterns);
            st->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDCANCEL) {
            st->ok = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anonymous namespace

bool show_ignore_dialog(HWND parent, const std::vector<std::string>& current,
                        std::vector<std::string>& out_patterns) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ignore_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconIgnoreDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    ignore_state st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    SIZE ws = window_size_for_client(462, 380, style, WS_EX_DLGMODALFRAME);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BaconIgnoreDlg", L"Edit Ignore Patterns",
                               style, CW_USEDEFAULT, CW_USEDEFAULT, ws.cx, ws.cy, parent, nullptr,
                               GetModuleHandleW(nullptr), &st);
    listbox_set(st.patterns, current);
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    GetWindowRect(dlg, &dr);
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
                 pr.left + ((pr.right - pr.left) - w) / 2,
                 pr.top + ((pr.bottom - pr.top) - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    run_modal(parent, dlg);
    if (st.ok)
        out_patterns = st.result;
    return st.ok;
}

// ===========================================================================
// Restore dialog
// ===========================================================================

namespace {

fs::path config_path() {
    return app_dir() / "config.json";
}

bool load_config(json::value& out) {
    std::ifstream in(config_path(), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str(), out);
}

void save_config(const json::value& v) {
    std::ofstream out(config_path(), std::ios::binary);
    out << json::dump(v, 2);
}

std::string fmt_timestamp(const std::string& raw) {
    // '2026-05-05 14:03:53 -0700' -> '2026-05-05  14:03:53'
    std::istringstream ss(raw);
    std::string date, time;
    ss >> date >> time;
    if (!date.empty() && !time.empty())
        return date + "  " + time;
    return raw;
}

bool is_binary(const std::string& data) {
    if (data.empty())
        return false;
    if (data.size() >= 2 && ((unsigned char)data[0] == 0xff && (unsigned char)data[1] == 0xfe))
        return false;
    if (data.size() >= 2 && ((unsigned char)data[0] == 0xfe && (unsigned char)data[1] == 0xff))
        return false;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf)
        return false;
    size_t n = data.size() < 8192 ? data.size() : 8192;
    for (size_t i = 0; i < n; ++i)
        if (data[i] == '\0')
            return true;
    return false;
}

std::wstring decode_text(const std::string& content, int tab_width) {
    std::wstring text;
    if (content.size() >= 2 && (unsigned char)content[0] == 0xff && (unsigned char)content[1] == 0xfe)
        text.assign(reinterpret_cast<const wchar_t*>(content.data() + 2),
                    (content.size() - 2) / 2);
    else if (content.size() >= 2 && (unsigned char)content[0] == 0xfe && (unsigned char)content[1] == 0xff)
        for (size_t i = 2; i + 1 < content.size(); i += 2)
            text += (wchar_t)(((unsigned char)content[i] << 8) | (unsigned char)content[i + 1]);
    else
        text = to_wide(content);
    if (tab_width > 0) {
        std::wstring spaces(tab_width, L' ');
        std::wstring out;
        for (wchar_t c : text)
            if (c == L'\t')
                out += spaces;
            else
                out += c;
        text.swap(out);
    }
    return text;
}

struct restore_state {
    std::wstring watch_path;
    fs::path git_dir;
    fs::path work_tree;
    std::vector<commit_entry> commits;
    std::string current_hash;
    std::string selected_file;
    std::string selected_status;
    int tab_width = 4;

    HWND commits_lb = nullptr;
    HWND mode_changed = nullptr;
    HWND mode_all = nullptr;
    HWND files_lv = nullptr;
    HWND view_content = nullptr;
    HWND view_diff = nullptr;
    HWND preview = nullptr;
    HWND count_lbl = nullptr;
    HWND export_btn = nullptr;

    int splitter1_x = 240;
    int splitter2_x = 600;
    bool dragging1 = false;
    bool dragging2 = false;

    LOGFONTW font{};
    HFONT preview_font = nullptr;
};

void rich_set_default_color(HWND rich, COLORREF col) {
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = col;
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

void rich_append(HWND rich, const std::wstring& text, COLORREF col) {
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = col;
    // move caret to end
    int len = GetWindowTextLengthW(rich);
    SendMessageW(rich, EM_SETSEL, len, len);
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(rich, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

void rich_clear(HWND rich) {
    SetWindowTextW(rich, L"");
}

void show_diff(HWND rich, const std::string& diff) {
    rich_clear(rich);
    std::istringstream ss(diff);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        COLORREF col = COL_TEXT;
        if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0)
            col = COL_HEADER;
        else if (line.rfind("@@", 0) == 0)
            col = COL_HUNK;
        else if (!line.empty() && line[0] == '+')
            col = COL_ADD;
        else if (!line.empty() && line[0] == '-')
            col = COL_DEL;
        rich_append(rich, to_wide(line) + L"\r\n", col);
    }
    SendMessageW(rich, EM_SETSEL, 0, 0);
    SendMessageW(rich, WM_VSCROLL, SB_TOP, 0);
}

void set_preview_plain(HWND rich, const std::wstring& text) {
    rich_clear(rich);
    rich_set_default_color(rich, COL_TEXT);
    SetWindowTextW(rich, text.c_str());
}

void restore_refresh_preview(restore_state* st) {
    if (st->selected_file.empty() || st->current_hash.empty()) {
        rich_clear(st->preview);
        return;
    }
    try {
        if (st->selected_status == "D") {
            set_preview_plain(st->preview, L"[File deleted in this snapshot]");
            return;
        }
        bool diff = SendMessageW(st->view_diff, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (diff) {
            std::string d = get_diff_for_commit(st->git_dir, st->work_tree,
                                                st->current_hash, st->selected_file);
            if (!trim(d).empty())
                show_diff(st->preview, d);
            else
                set_preview_plain(st->preview, L"[No diff - file unchanged or binary]");
            return;
        }
        std::string content = get_file_at_commit(st->git_dir, st->current_hash, st->selected_file);
        if (is_binary(content))
            set_preview_plain(st->preview,
                              L"[Binary file - " + std::to_wstring(content.size()) + L" bytes]");
        else
            set_preview_plain(st->preview, decode_text(content, st->tab_width));
    } catch (const std::exception& e) {
        set_preview_plain(st->preview, L"Error: " + to_wide(e.what()));
    }
}

void restore_refresh_files(restore_state* st) {
    ListView_DeleteAllItems(st->files_lv);
    rich_clear(st->preview);
    st->selected_file.clear();
    st->selected_status.clear();
    if (st->current_hash.empty())
        return;

    bool changed = SendMessageW(st->mode_changed, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int n = 0;
    try {
        if (changed) {
            auto files = get_commit_files(st->git_dir, st->work_tree, st->current_hash);
            for (auto& f : files) {
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = n;
                std::wstring path = to_wide(f.path);
                it.pszText = path.data();
                ListView_InsertItem(st->files_lv, &it);
                ListView_SetItemText(st->files_lv, n, 1, (LPWSTR)to_wide(f.status).c_str());
                ListView_SetCheckState(st->files_lv, n, TRUE);
                ++n;
            }
        } else {
            auto files = get_full_tree_at_commit(st->git_dir, st->current_hash);
            for (auto& fp : files) {
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = n;
                std::wstring path = to_wide(fp);
                it.pszText = path.data();
                ListView_InsertItem(st->files_lv, &it);
                ListView_SetCheckState(st->files_lv, n, TRUE);
                ++n;
            }
        }
    } catch (const std::exception& e) {
        set_preview_plain(st->preview, L"Error loading files: " + to_wide(e.what()));
        return;
    }

    SetWindowTextW(st->count_lbl, (std::to_wstring(n) + L" file(s)").c_str());
    EnableWindow(st->export_btn, n > 0);

    if (n == 1) {
        wchar_t buf[1024];
        ListView_GetItemText(st->files_lv, 0, 0, buf, 1024);
        st->selected_file = to_utf8(buf);
        wchar_t sbuf[64];
        ListView_GetItemText(st->files_lv, 0, 1, sbuf, 64);
        st->selected_status = to_utf8(sbuf);
        ListView_SetItemState(st->files_lv, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        restore_refresh_preview(st);
    }
}

void restore_load_commits(restore_state* st) {
    try {
        st->commits = get_commit_log(st->git_dir, st->work_tree);
    } catch (const std::exception& e) {
        set_preview_plain(st->preview, L"Error loading commits: " + to_wide(e.what()));
        return;
    }
    SendMessageW(st->commits_lb, LB_RESETCONTENT, 0, 0);
    for (auto& c : st->commits) {
        std::string ts = fmt_timestamp(c.timestamp);
        std::string detail;
        try {
            auto changed = get_commit_files(st->git_dir, st->work_tree, c.hash);
            if (changed.size() == 1)
                detail = fs::path(to_wide(changed[0].path)).filename().string();
            else
                detail = std::to_string(changed.size()) + " files";
        } catch (...) {
            detail = "?";
        }
        std::wstring entry = to_wide(ts + "   (" + detail + ")");
        SendMessageW(st->commits_lb, LB_ADDSTRING, 0, (LPARAM)entry.c_str());
    }
    if (!st->commits.empty()) {
        SendMessageW(st->commits_lb, LB_SETCURSEL, 0, 0);
        st->current_hash = st->commits[0].hash;
        restore_refresh_files(st);
    }
}

void restore_export(HWND hwnd, restore_state* st) {
    if (st->current_hash.empty())
        return;
    std::vector<std::string> checked;
    int n = ListView_GetItemCount(st->files_lv);
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(st->files_lv, i)) {
            wchar_t buf[1024];
            ListView_GetItemText(st->files_lv, i, 0, buf, 1024);
            checked.push_back(to_utf8(buf));
        }
    }
    if (checked.empty()) {
        MessageBoxW(hwnd, L"No files are checked.", L"Nothing Selected", MB_ICONINFORMATION);
        return;
    }
    wchar_t home[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
    fs::path dest = fs::path(home) / "Downloads" / ("BaconSaver_restore_" + std::string(ts));

    auto exported = export_files(st->git_dir, st->current_hash, checked, dest);
    if (!exported.empty()) {
        std::wstring msg = L"Exported " + std::to_wstring(exported.size()) +
                           L" file(s) to:\n" + dest.wstring() + L"\n\nOpen the folder?";
        if (MessageBoxW(hwnd, msg.c_str(), L"Export Complete", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(nullptr, L"open", dest.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        MessageBoxW(hwnd, L"No files could be exported.", L"Export Failed", MB_ICONWARNING);
    }
}

void restore_pick_font(HWND hwnd, restore_state* st) {
    CHOOSEFONTW cf = {};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &st->font;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FIXEDPITCHONLY;
    if (!ChooseFontW(&cf))
        return;
    if (st->preview_font)
        DeleteObject(st->preview_font);
    st->preview_font = CreateFontIndirectW(&st->font);
    set_font(st->preview, st->preview_font);

    json::value cfg;
    if (!load_config(cfg))
        cfg = json::value::make_object();
    json::value f = json::value::make_object();
    f.set("family", json::value(to_utf8(st->font.lfFaceName)));
    // points = -lfHeight * 72 / 96 (assuming 96 dpi); store positive size
    HDC dc = GetDC(hwnd);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(hwnd, dc);
    int pts = st->font.lfHeight < 0 ? MulDiv(-st->font.lfHeight, 72, dpi)
                                    : MulDiv(st->font.lfHeight, 72, dpi);
    f.set("size", json::value(pts));
    cfg.set("restore_font", f);
    save_config(cfg);
    restore_refresh_preview(st);
}

void restore_load_font(HWND hwnd, restore_state* st) {
    std::string family = "Consolas";
    int size = 9;
    json::value cfg;
    if (load_config(cfg)) {
        if (auto* f = cfg.find("restore_font")) {
            if (auto* fam = f->find("family"))
                family = fam->as_string("Consolas");
            if (auto* sz = f->find("size"))
                size = sz->as_int(9);
        }
    }
    st->font = {};
    HDC dc = GetDC(hwnd);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(hwnd, dc);
    st->font.lfHeight = -MulDiv(size, dpi, 72);
    st->font.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcsncpy_s(st->font.lfFaceName, to_wide(family).c_str(), _TRUNCATE);
    st->preview_font = CreateFontIndirectW(&st->font);
}

void restore_load_size(int& w, int& h) {
    w = 1000;
    h = 640;
    json::value cfg;
    if (load_config(cfg)) {
        if (auto* s = cfg.find("restore_size")) {
            if (s->is_array() && s->arr && s->arr->size() == 2) {
                w = (*s->arr)[0].as_int(w);
                h = (*s->arr)[1].as_int(h);
            }
        }
    }
}

void restore_save_size(HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    json::value cfg;
    if (!load_config(cfg))
        cfg = json::value::make_object();
    json::value arr = json::value::make_array();
    arr.arr->push_back(json::value((int)(rc.right - rc.left)));
    arr.arr->push_back(json::value((int)(rc.bottom - rc.top)));
    cfg.set("restore_size", arr);
    save_config(cfg);
}

void restore_layout(HWND hwnd, restore_state* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int margin = 8;
    int splitter_w = 6;
    int bottom_h = 36;
    int top = margin;
    int body_h = H - bottom_h - margin * 2;

    int left_w = st->splitter1_x;
    int mid_w = st->splitter2_x - (margin + left_w + splitter_w + margin);
    int right_x = margin + left_w + splitter_w + margin + mid_w + splitter_w + margin;
    int right_w = W - right_x - margin;
    if (right_w < 120)
        right_w = 120;

    // left: label + commits
    HWND lbl_snap = GetDlgItem(hwnd, IDC_RES_SNAP_LBL);
    if (lbl_snap)
        MoveWindow(lbl_snap, margin, top, left_w, 18, TRUE);
    MoveWindow(st->commits_lb, margin, top + 22, left_w, body_h - 22, TRUE);

    // mid: mode row + buttons + list
    int mx = margin + left_w + splitter_w + margin;
    MoveWindow(st->mode_changed, mx, top, 110, 22, TRUE);
    MoveWindow(st->mode_all, mx + 112, top, 110, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_RES_SEL_ALL), mx + mid_w - 150, top, 72, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_RES_SEL_NONE), mx + mid_w - 74, top, 74, 24, TRUE);
    MoveWindow(st->files_lv, mx, top + 28, mid_w, body_h - 28, TRUE);
    ListView_SetColumnWidth(st->files_lv, 1, 70);
    ListView_SetColumnWidth(st->files_lv, 0, mid_w - 70 - 24);

    // right: view row + font + preview
    MoveWindow(st->view_content, right_x, top, 80, 22, TRUE);
    MoveWindow(st->view_diff, right_x + 84, top, 80, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_RES_FONT), right_x + right_w - 80, top, 80, 24, TRUE);
    MoveWindow(st->preview, right_x, top + 28, right_w, body_h - 28, TRUE);

    // bottom
    int by = H - bottom_h;
    MoveWindow(st->count_lbl, margin, by + 6, 160, 20, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), W - margin - 90, by, 90, 28, TRUE);
    MoveWindow(st->export_btn, W - margin - 90 - 130, by, 124, 28, TRUE);
}

void restore_goto_line(HWND hwnd, restore_state* st) {
    int total = (int)SendMessageW(st->preview, EM_GETLINECOUNT, 0, 0);
    if (total <= 0)
        return;
    std::wstring s = L"1";
    if (!show_text_input(hwnd, L"Go to Line",
                         (L"Line (1-" + std::to_wstring(total) + L"):"), s))
        return;
    int line = _wtoi(s.c_str());
    if (line < 1)
        line = 1;
    if (line > total)
        line = total;
    int idx = (int)SendMessageW(st->preview, EM_LINEINDEX, line - 1, 0);
    if (idx >= 0) {
        SendMessageW(st->preview, EM_SETSEL, idx, idx);
        SendMessageW(st->preview, EM_SCROLLCARET, 0, 0);
        SetFocus(st->preview);
    }
}

LRESULT CALLBACK restore_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<restore_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<restore_state*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        make(L"STATIC", L"Snapshots", 0, 0, 0, 10, 10, hwnd, IDC_RES_SNAP_LBL);
        st->commits_lb = make(L"LISTBOX", L"",
                              WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                              0, 0, 10, 10, hwnd, IDC_RES_COMMITS);

        st->mode_changed = make(L"BUTTON", L"Changed files",
                                WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 0, 10, 10, hwnd, IDC_RES_MODE_CHANGED);
        st->mode_all = make(L"BUTTON", L"Full snapshot",
                            BS_AUTORADIOBUTTON, 0, 0, 10, 10, hwnd, IDC_RES_MODE_ALL);
        SendMessageW(st->mode_changed, BM_SETCHECK, BST_CHECKED, 0);
        make(L"BUTTON", L"Select All", WS_TABSTOP, 0, 0, 10, 10, hwnd, IDC_RES_SEL_ALL);
        make(L"BUTTON", L"Select None", WS_TABSTOP, 0, 0, 10, 10, hwnd, IDC_RES_SEL_NONE);

        st->files_lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_RES_FILES, GetModuleHandleW(nullptr), nullptr);
        set_font(st->files_lv, ui_font());
        ListView_SetExtendedListViewStyle(st->files_lv,
                                          LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        {
            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)L"File";
            col.cx = 260;
            ListView_InsertColumn(st->files_lv, 0, &col);
            col.pszText = (LPWSTR)L"Status";
            col.cx = 70;
            ListView_InsertColumn(st->files_lv, 1, &col);
        }

        st->view_content = make(L"BUTTON", L"Content",
                                WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 0, 10, 10, hwnd, IDC_RES_VIEW_CONTENT);
        st->view_diff = make(L"BUTTON", L"Diff", BS_AUTORADIOBUTTON, 0, 0, 10, 10, hwnd, IDC_RES_VIEW_DIFF);
        SendMessageW(st->view_content, BM_SETCHECK, BST_CHECKED, 0);
        make(L"BUTTON", L"Font...", WS_TABSTOP, 0, 0, 10, 10, hwnd, IDC_RES_FONT);

        st->preview = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                                          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_RES_PREVIEW, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(st->preview, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
        SendMessageW(st->preview, EM_SETTARGETDEVICE, 0, 1); // disable word wrap
        restore_load_font(hwnd, st);
        set_font(st->preview, st->preview_font);
        rich_set_default_color(st->preview, COL_TEXT);

        st->count_lbl = make(L"STATIC", L"", 0, 0, 0, 10, 10, hwnd, IDC_RES_COUNT_LBL);
        st->export_btn = make(L"BUTTON", L"Export Selected", WS_TABSTOP, 0, 0, 10, 10, hwnd, IDC_RES_EXPORT);
        EnableWindow(st->export_btn, FALSE);
        make(L"BUTTON", L"Close", WS_TABSTOP, 0, 0, 10, 10, hwnd, IDCANCEL);

        restore_layout(hwnd, st);
        restore_load_commits(st);
        return 0;
    }
    case WM_SIZE:
        if (st)
            restore_layout(hwnd, st);
        return 0;
    case WM_SETCURSOR:
        if (st) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int margin = 8;
            int gutter_w = 6;
            int top = margin;
            int body_h = rc.bottom - 36 - margin * 2;
            int spl_y = top + 22, spl_h = body_h - 22;
            RECT g1 = { margin + st->splitter1_x, spl_y, margin + st->splitter1_x + gutter_w, spl_y + spl_h };
            int mid_left = margin + st->splitter1_x + gutter_w + margin;
            RECT g2 = { mid_left + (st->splitter2_x - mid_left), spl_y,
                        mid_left + (st->splitter2_x - mid_left) + gutter_w, spl_y + spl_h };
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g1, pt) || PtInRect(&g2, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (st) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int margin = 8;
            int gutter_w = 6;
            int top = margin;
            int body_h = rc.bottom - 36 - margin * 2;
            int spl_y = top + 22, spl_h = body_h - 22;
            RECT g1 = { margin + st->splitter1_x, spl_y, margin + st->splitter1_x + gutter_w, spl_y + spl_h };
            int mid_left = margin + st->splitter1_x + gutter_w + margin;
            RECT g2 = { mid_left + (st->splitter2_x - mid_left), spl_y,
                        mid_left + (st->splitter2_x - mid_left) + gutter_w, spl_y + spl_h };
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            if (PtInRect(&g1, pt)) {
                st->dragging1 = true;
                SetCapture(hwnd);
                return 0;
            }
            if (PtInRect(&g2, pt)) {
                st->dragging2 = true;
                SetCapture(hwnd);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (st && st->dragging1) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int x = LOWORD(lp) - 3;
            if (x < 100)
                x = 100;
            if (x > rc.right - 500)
                x = rc.right - 500;
            if (x != st->splitter1_x) {
                st->splitter1_x = x;
                restore_layout(hwnd, st);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        if (st && st->dragging2) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int x = LOWORD(lp) - 3;
            int min_x = st->splitter1_x + 250;
            if (x < min_x)
                x = min_x;
            if (x > rc.right - 200)
                x = rc.right - 200;
            if (x != st->splitter2_x) {
                st->splitter2_x = x;
                restore_layout(hwnd, st);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (st && st->dragging1) {
            st->dragging1 = false;
            ReleaseCapture();
            return 0;
        }
        if (st && st->dragging2) {
            st->dragging2 = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == IDC_RES_FILES && nm->code == LVN_ITEMCHANGED) {
            auto* nv = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((nv->uChanged & LVIF_STATE) && (nv->uNewState & LVIS_SELECTED)) {
                wchar_t buf[1024];
                ListView_GetItemText(st->files_lv, nv->iItem, 0, buf, 1024);
                st->selected_file = to_utf8(buf);
                wchar_t sbuf[64];
                ListView_GetItemText(st->files_lv, nv->iItem, 1, sbuf, 64);
                st->selected_status = to_utf8(sbuf);
                restore_refresh_preview(st);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDC_RES_COMMITS && code == LBN_SELCHANGE) {
            int row = (int)SendMessageW(st->commits_lb, LB_GETCURSEL, 0, 0);
            if (row >= 0 && row < (int)st->commits.size()) {
                st->current_hash = st->commits[row].hash;
                restore_refresh_files(st);
            } else {
                st->current_hash.clear();
                restore_refresh_files(st);
            }
            return 0;
        }
        if (id == IDC_RES_MODE_CHANGED || id == IDC_RES_MODE_ALL) {
            restore_refresh_files(st);
            return 0;
        }
        if (id == IDC_RES_VIEW_CONTENT || id == IDC_RES_VIEW_DIFF) {
            restore_refresh_preview(st);
            return 0;
        }
        if (id == IDC_RES_SEL_ALL) { // select all
            int n = ListView_GetItemCount(st->files_lv);
            for (int i = 0; i < n; ++i)
                ListView_SetCheckState(st->files_lv, i, TRUE);
            return 0;
        }
        if (id == IDC_RES_SEL_NONE) {
            int n = ListView_GetItemCount(st->files_lv);
            for (int i = 0; i < n; ++i)
                ListView_SetCheckState(st->files_lv, i, FALSE);
            return 0;
        }
        if (id == IDC_RES_FONT) {
            restore_pick_font(hwnd, st);
            return 0;
        }
        if (id == IDC_RES_EXPORT) {
            restore_export(hwnd, st);
            return 0;
        }
        if (id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        restore_save_size(hwnd);
        if (st && st->preview_font)
            DeleteObject(st->preview_font);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anonymous namespace

void show_restore_dialog(HWND parent, const std::wstring& watch_path,
                         const std::wstring& shadow_path) {
    LoadLibraryW(L"Msftedit.dll");
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = restore_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"BaconRestoreDlg";
        RegisterClassW(&wc);
        registered = true;
    }
    restore_state st;
    st.watch_path = watch_path;
    st.git_dir = fs::path(shadow_path) / ".git";
    st.work_tree = watch_path;

    int w, h;
    restore_load_size(w, h);
    std::wstring title = L"Restore - " + watch_path;
    HWND dlg = CreateWindowExW(0, L"BaconRestoreDlg", title.c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, w, h, parent, nullptr,
                               GetModuleHandleW(nullptr), &st);

    // Modal loop with Ctrl+G handling
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == 'G' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            restore_goto_line(dlg, &st);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
