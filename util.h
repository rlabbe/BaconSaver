#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

// ---- Encoding ----

inline std::string to_utf8(const std::wstring& ws) {
    if (ws.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
    return s;
}

inline std::wstring to_wide(const std::string& s) {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

inline std::string path_utf8(const fs::path& p) {
    return to_utf8(p.wstring());
}

// ---- DPI ----

inline int dpi_for(HWND hwnd) {
    return GetDpiForWindow(hwnd);
}
inline int scale_px(int px, HWND hwnd) {
    return MulDiv(px, dpi_for(hwnd), 96);
}

// ---- Paths ----

inline fs::path app_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(std::wstring(buf, n)).parent_path();
}

inline fs::path config_path() {
    return app_dir() / "config.json";
}

// ---- Strings ----

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ')
        ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        --b;
    return s.substr(a, b - a);
}

inline std::string now_ts() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ---- Non-inline helpers ----

std::vector<std::string> split_lines(const std::string& s);
bool fnmatch(const std::string& pattern, const std::string& str);

// ---- Always-ignored paths ----

inline const std::vector<std::string> always_ignored = { ".git" };
