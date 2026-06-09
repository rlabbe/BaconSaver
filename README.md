# BaconSaver

Watches directories and auto-commits changes to shadow git repositories.
Browse and restore previous versions from the tray icon.

## Build

Open `BaconSaver.vcxproj` in Visual Studio 2022, or build from the command line:

```
msbuild BaconSaver.vcxproj /p:Configuration=Release /p:Platform=x64
```

Produces `x64\Release\BaconSaver.exe` — a single self-contained executable.
No installer, no runtime dependencies beyond Windows.

## Requirements

- Windows 10 or later
- Git 2.37 or later on PATH
- Visual Studio 2022 (to build)

## Usage

Run `BaconSaver.exe`. It lives in the system tray.

Right-click the tray icon to add directories to watch. Each watched directory
gets a shadow git repo under the backup location (`E:\BaconSaverData\Shadows`
by default). Files are auto-committed as they change.

Use Restore to browse commit history, view file content and diffs,
and export previous versions.

Ignore patterns use wildcards (`*`, `?`, `[seq]`). The C++ and Python
presets cover common build artifacts and caches.
