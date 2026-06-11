#pragma once
#include <functional>
#include <string>
#include <windows.h>

const UINT WM_APP_LOG = WM_APP + 1;

using log_fn = std::function<void(const std::string&)>;

// File-only logging (no UI dependency).
void file_log(const std::string& msg);
void trace_log(const std::string& msg);

// Console logging (requires g_console to exist).
void console_append(const std::wstring& line);
void console_update_scroll();
void log_local(const std::string& msg);

// Factory: returns a log_fn that posts to the UI thread.
log_fn make_log(const std::wstring& path);
