#include "log.h"
#include "util.h"
#include <fstream>
#include <windows.h>

extern HWND g_console;
extern HWND g_main;

void file_log(const std::string& msg) {
    std::error_code ec;
    fs::path dir = app_dir();
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "baconsaver.log", std::ios::app | std::ios::binary);
    f << now_ts() << "  " << msg << "\n";
}

void trace_log(const std::string& msg) {
    std::error_code ec;
    std::ofstream f(app_dir() / "baconsaver.log", std::ios::app | std::ios::binary);
    f << now_ts() << "  [trace] " << msg << "\n";
}

void console_update_scroll() {
    int lines = (int)SendMessageW(g_console, EM_GETLINECOUNT, 0, 0);
    RECT rc;
    GetClientRect(g_console, &rc);
    HDC dc = GetDC(g_console);
    HFONT f = (HFONT)SendMessageW(g_console, WM_GETFONT, 0, 0);
    TEXTMETRICW tm = {};
    if (dc && f) {
        SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
    }
    if (dc)
        ReleaseDC(g_console, dc);
    int visible = rc.bottom / (tm.tmHeight > 0 ? tm.tmHeight : 16);
    ShowScrollBar(g_console, SB_VERT, lines > visible);
}

void console_append(const std::wstring& line) {
    int len = GetWindowTextLengthW(g_console);
    SendMessageW(g_console, EM_SETSEL, len, len);
    std::wstring l = line + L"\r\n";
    SendMessageW(g_console, EM_REPLACESEL, FALSE, (LPARAM)l.c_str());
    console_update_scroll();
}

void log_local(const std::string& msg) {
    console_append(to_wide("[BaconSaver] " + msg));
    file_log("[BaconSaver] " + msg);
}

log_fn make_log(const std::wstring& path) {
    std::string label = to_utf8(fs::path(path).filename().wstring());
    return [label](const std::string& msg) {
        auto* s = new std::string("[" + label + "] " + msg);
        if (!PostMessageW(g_main, WM_APP_LOG, 0, (LPARAM)s))
            delete s;
    };
}
