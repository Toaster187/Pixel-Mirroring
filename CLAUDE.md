# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Pixel Mirroring is an open-source equivalent of Apple's "iPhone Mirroring" for Android/Pixel devices — it mirrors an Android screen onto a Windows/macOS PC natively (no browser tech), with low RAM usage. It has two components that talk to each other over ADB/TCP on the local network:

```
Android App (Kotlin/Jetpack Compose)  <--->  ADB/TCP  <--->  Desktop Client (C++/Win32/Cocoa)
        Background Service                              Custom Borderless Window
        ADB WiFi Toggle                                 scrcpy Protocol Client
        Material 3 UI                                   FFmpeg H.264/H.265 Decoder + SDL2 Renderer
```

There is also an `AGENTS.md` in the repo root (German) with overlapping agent guidance — this file supersedes/summarizes it in English; keep both in sync if you change conventions.

## Repository layout

```
Android/app/src/main/java/dev/pixelmirroring/app/
    MainActivity.kt
    data/     PairedClientStore (persisted pairing state, DataStore-Preferences)
    network/  ApiModels, NetworkScanner (local network discovery data)
    service/  MirroringService, BootReceiver, NotificationHelper, DiscoveryHttpServer, AdbWifiManager
Client/                        C++20 desktop client
    CMakeLists.txt              build config (CMake 3.25+, vcpkg toolchain)
    vendor/platform-tools/      bundled Android platform-tools (adb.exe etc.), copied next to the EXE at build time
    scrcpy-server.jar           unmodified upstream scrcpy server binary, pushed to the device at runtime
    src/
        main.cpp                 entry point (WinMain on Windows, main on POSIX); drives ADB/scrcpy/window/tray/settings together
        settings.{h,cpp}         pm::Settings — persisted user settings (max_fps, max_size, encrypted PIN, brightness, compatibility mode)
        adb/                    pm::adb — wraps the adb CLI as a subprocess (discovery, tcpip connect, install/start app, shell exec, push, port fwd)
        stream/                 pm::stream — scrcpy wire protocol: raw video+control sockets, VideoDecoder (FFmpeg), VideoRenderer (SDL2)
        input/                  pm::input — forwards mouse/keyboard/touch into ScrcpyClient::inject_* over the control socket
        network/                pm::network — LAN subnet scan + discovery via cpp-httplib
        window/                 pm::window — window_interface.h + win32_window.{h,cpp} / cocoa_window.{h,mm}
        tray/                   pm::tray — tray_interface.h + win32_tray.{h,cpp}
    vcpkg/                      git submodule (full vcpkg checkout) — huge; exclude from broad searches
scrcpy_download/scrcpy-server.jar   source copy of the scrcpy server jar
```

## Build & test commands

**Desktop Client** (from `Client/`; requires the `vcpkg` submodule initialized: `git submodule update --init --recursive`, then `./vcpkg/bootstrap-vcpkg.bat` once):
```
cmake --preset default -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Packaging (installers): `cpack -C Release` from `Client/build/` (produces ZIP/NSIS/WIX on Windows, TGZ elsewhere).

**Android** (from `Android/`):
```
./gradlew assembleDebug
```
Note: the Gradle wrapper scripts (`gradlew`, `gradlew.bat`, `gradle/`) are gitignored and not committed — CI installs Gradle 9.4.1 directly rather than using the wrapper.

**Testing:** there is no automated test suite (no C++ test framework wired into CMake; Android has only default JUnit/Espresso boilerplate). Verification is manual, with a physical/connected Android device, on both sides.

**CI** (`.github/workflows/release.yml`): triggered on `v*` tags or manual dispatch. Builds the Android APK first (`gradle assembleDebug`), then builds the Windows client with that APK bundled in, packages installers with `cpack`, and creates a GitHub Release on tag pushes.

## Architecture notes

- **Connection/state flow** (client-side state machine: `SETUP` → `SCANNING` → `CONNECTED` → `STREAMING`):
  1. First run: client shows a setup screen; user enables USB debugging once and connects via USB.
  2. Client auto-installs the Android app, grants `WRITE_SECURE_SETTINGS` via ADB, starts the app, and enables ADB-over-WiFi (Android sets `Settings.Global.putInt("adb_wifi_enabled", 1)` itself).
  3. Setup state is only persisted once it fully succeeds, under `%LOCALAPPDATA%\PixelMirroring`. **If first-time setup fails, the client must not silently fall back to network scanning** — without saved setup, USB remains the only setup path.
  4. On subsequent launches the client auto-connects: it pings known IPs (LAN+VPN) against the Android app's embedded discovery HTTP server (`DiscoveryHttpServer.kt`, a hand-rolled minimal HTTP/1.1 server over a raw `ServerSocket`, no framework), connects via ADB TCP/IP (port 5555), pushes/starts `scrcpy-server.jar`, then opens raw video+control TCP sockets directly to the on-device scrcpy server (bypassing `adb shell` for the actual media data).
  5. Video is decoded via FFmpeg (`VideoDecoder`, H.264/H.265) and rendered via SDL2 inside a platform-native window (`VideoRenderer`); input (mouse/keyboard/touch) is forwarded back over the control socket via `ScrcpyClient::inject_*`.
- **Windows window**: a custom borderless `Win32` window (`win32_window.cpp`) using `WS_THICKFRAME|WS_CAPTION` with a `WM_NCCALCSIZE` override, custom `WM_NCHITTEST` hit-testing (drag/resize, Win11 snap via `HTMAXBUTTON`), and `DwmExtendFrameIntoClientArea` for the native shadow — SDL2 renders inside a Win32 child window. macOS uses a standard Cocoa/AppKit window instead.
- **Bundled tooling**: the Windows client ships its own `adb.exe`/`AdbWinApi.dll`/`AdbWinUsbApi.dll` under `Client/vendor/platform-tools/`, copied next to the EXE at build time (CMake post-build step also auto-downloads platform-tools from Google if missing). End users should never need Android Studio or an SDK install; ADB search always prefers the bundled copy.
- **Interface/impl pattern**: platform-specific pieces (window, tray) are split into an `_interface.h` plus per-platform implementation, selected via CMake conditional compilation — not preprocessor branching within shared files.

## Coding conventions

- **C++ (Client)**: C++20; everything under namespace `pm::` (`pm::adb`, `pm::stream`, `pm::window`, `pm::input`, `pm::tray`, `pm::network`); classes PascalCase, methods snake_case, constants SCREAMING_SNAKE_CASE, member variables prefixed `m_`; ownership via `std::unique_ptr` (raw pointers are non-owning only); no exceptions — use `bool`/`std::optional` return values for error handling.
- **Kotlin (Android)**: package `dev.pixelmirroring.app.*`; foreground-service-based architecture; Compose + Material 3 for UI; Kotlin coroutines for async.
- **Code comments** (both C++ and Kotlin) are written in a deliberately informal "caveman" register (e.g. `// Ugg! ADB not found ... Downloading from Google...`) per project convention in `AGENTS.md` — this applies only to in-code comments, not to user-facing communication, commit messages, or documentation, which should stay normal and professional (German or English).
- No hardcoded paths; conditional compilation goes through CMake, not ad-hoc preprocessor branches.

## Guardrails

- Never use browser tech (no Electron/WebView/CEF) for the desktop client.
- Always preserve aspect ratio when rendering the mirrored screen.
- Use the scrcpy protocol as-is for streaming — do not invent a replacement, and do not modify the vendored `scrcpy-server.jar`.
- Don't introduce new frameworks, languages, or build systems into either component.
- No third-party network requests beyond what's needed for ADB/scrcpy/device discovery.
- Keep Android-side battery usage minimal (this is why ADB-over-WiFi and discovery use a lightweight foreground service and a tiny hand-rolled HTTP server rather than heavier alternatives).
