# BaconSaver

Watches directories and auto-commits changes to shadow git repositories.
Browse and restore previous versions from the GUI.

## Requirements

- Windows 10 or later
- Git 2.37 or later on PATH
- Visual Studio 2022 (to build)

## Build

```
msbuild BaconSaver.vcxproj /p:Configuration=Release /p:Platform=x64
```

Produces `x64\Release\BaconSaver.exe` — a single self-contained executable.

## Usage

Run `BaconSaver.exe`.

**Setup.** On first run you are prompted for a backup location — where
shadow git repos will be stored. Then click **Add Directory...** to watch
a directory. A preset list (C++, Python, General) lets you skip common
build artifacts and caches.

Each watched directory appears in the list on the left. BaconSaver
auto-commits file changes to its shadow repo every few seconds. The
console on the right shows what is happening.

**Restore.** Select a watched directory and click **Restore...** to browse
its commit history. Choose a commit to see which files changed (or a full
snapshot of the entire tree at that point). Select files and click Export
to save them. Double-click a file to view its content or diff.

**Ignore patterns.** Select a watched directory and click **Edit Ignores...**
to add patterns for files you want excluded from the backup. Patterns
support wildcards (`*`, `?`, `[seq]`). The C++ and Python presets cover
common build artifacts and caches.
