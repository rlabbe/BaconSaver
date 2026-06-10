#pragma once
#include <string>
#include <vector>
#include <windows.h>

SIZE window_size_for_client(int cw, int ch, DWORD style, DWORD ex_style, HWND ref);
void run_modal(HWND parent, HWND dlg);

// Add Watched Directory. On OK returns true and fills out_path (resolved, wide)
// and out_patterns (the edited ignore patterns).
bool show_add_directory_dialog(
    HWND parent, const std::vector<std::wstring>& backup_locations,
    std::wstring& out_path, std::vector<std::string>& out_patterns, bool& out_skip_binary,
    std::wstring& out_shadows_base);

// Edit Ignore Patterns for an already-watched directory. current seeds the list;
// on OK returns true and fills out_patterns.
bool show_ignore_dialog(HWND parent, const std::vector<std::string>& current, std::vector<std::string>& out_patterns);

// Browse commit history and export files. Modal.
void show_restore_dialog(HWND parent, const std::wstring& watch_path, const std::wstring& shadow_path);
