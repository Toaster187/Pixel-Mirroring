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
Android/app/proguard-rules.pro  R8 keep rules (manifest components, kotlinx-serialization models)
Client/                        C++20 desktop client
    CMakeLists.txt              build config (CMake 3.25+, vcpkg toolchain)
    vendor/platform-tools/      bundled Android platform-tools (adb.exe etc.), copied next to the EXE at build time
    scrcpy-server.jar           unmodified upstream scrcpy server binary, pushed to the device at runtime
    src/
        main.cpp                 entry point (WinMain on Windows, main on POSIX); drives ADB/scrcpy/window/tray/settings together
        settings.{h,cpp}         pm::Settings — persisted user settings (max_fps, max_size, encrypted PIN, brightness, compatibility mode, audio on/off)
        adb/                    pm::adb — wraps the adb CLI as a subprocess (discovery, tcpip connect, install/start app, shell exec, push, port fwd)
                                 plus pm::adb::ShellProcess, a killable long-running "adb shell" child
        stream/                 pm::stream — scrcpy wire protocol: raw video+audio+control sockets, VideoDecoder (FFmpeg),
                                 VideoRenderer (SDL2), AudioPlayer (SDL2 output), CaptureController (screenshots/recording)
        input/                  pm::input — forwards mouse/keyboard/touch into ScrcpyClient::inject_* over the control socket
        network/                pm::network — LAN subnet scan + discovery via cpp-httplib
        window/                 pm::window — window_interface.h + win32_window.{h,cpp} / cocoa_window.{h,mm}
        tray/                   pm::tray — tray_interface.h + win32_tray.{h,cpp}
    vcpkg/                      git submodule (full vcpkg checkout) — huge; exclude from broad searches
scrcpy_download/scrcpy-server.jar   source copy of the scrcpy server jar
```

**macOS is not buildable today** — do not treat it as a working target. `CocoaWindow`
implements only a fraction of `IWindow`'s pure virtuals, and `src/tray/cocoa_tray.mm`
is referenced by `CMakeLists.txt` but does not exist. CI builds Android + Windows only.
Adding a method to `IWindow` therefore cannot "break macOS" — it is already broken —
but do not claim macOS support in user-facing docs.

## Build & test commands

**Desktop Client** (from `Client/`; requires the `vcpkg` submodule initialized: `git submodule update --init --recursive`, then `./vcpkg/bootstrap-vcpkg.bat` once):
```
cmake --preset default -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Packaging (installers): `cpack -C Release` from `Client/build/` (produces ZIP/NSIS/WIX on Windows, TGZ elsewhere).

Only the DLLs the binaries actually import are copied/installed, listed as
`PM_RUNTIME_DLL_PATTERNS` in `CMakeLists.txt`. Do **not** go back to copying the whole
`vcpkg_installed/<triplet>/bin` directory — that shipped `avfilter`, `avdevice`, the
build tool `pkgconf` and every `.pdb`, ~4 MB nothing ever loads. If you add a library,
add its pattern there, otherwise it is missing from the installer.

**Android** (from `Android/`):
```
gradle assembleDebug
```
Note: the Gradle wrapper scripts (`gradlew`, `gradlew.bat`, `gradle/`) are gitignored and not committed — CI installs Gradle 9.4.1 directly rather than using the wrapper.

**The debug variant is the shipped artifact** — the desktop client installs it and CI
publishes it. That is why `debug` has `isMinifyEnabled`/`isShrinkResources` turned on,
which is otherwise unusual. Without it the APK is ~60 MB instead of ~4 MB. Consequences:
- New classes reached only via the manifest or reflection need a keep rule in
  `app/proguard-rules.pro`.
- Compose tooling is deliberately not in the shipped APK. For local previews build with
  `gradle assembleDebug -PcomposeTooling`.
- R8 runs shrink-only on debuggable builds (no obfuscation), which is the low-risk mode.

**Testing:** there is no automated test suite (no C++ test framework wired into CMake; Android has only default JUnit/Espresso boilerplate). Verification is manual, with a physical/connected Android device, on both sides.

**CI** (`.github/workflows/release.yml`): triggered on `v*` tags or manual dispatch. Builds the Android APK first (`gradle assembleDebug`), then builds the Windows client with that APK bundled in, packages installers with `cpack`, and creates a GitHub Release on tag pushes.

## Architecture notes

- **Connection/state flow** (client-side state machine: `SETUP` → `SCANNING` → `CONNECTED` → `STREAMING`):
  1. **First-run setup (USB):** User enables USB debugging, connects the device via USB. Client auto-detects it via ADB, installs the Android app from the PC, grants `WRITE_SECURE_SETTINGS` via ADB, starts the app, and enables ADB-over-WiFi (Android sets `Settings.Global.putInt("adb_wifi_enabled", 1)` itself). Setup state is persisted under `%LOCALAPPDATA%\PixelMirroring` **only when fully successful**. If first-time setup fails, state is NOT saved — USB remains the only setup path.
  2. **Automatic reconnection (on-demand ADB + 60s idle shutdown):** ADB is off by default between sessions (security: no longer left permanently enabled). On subsequent launches, the client auto-connects: first it checks whether an already-connected TCP device matches the saved IP (warm reconnect within the idle window); otherwise it POSTs `/connect` (with its persisted `clientId`) directly to the saved device's discovery HTTP server (`DiscoveryHttpServer.kt`, a hand-rolled minimal HTTP/1.1 server over a raw `ServerSocket`, no framework) to wake ADB on-demand. If the saved IP doesn't respond, fallback to a LAN subnet scan. The Android app authorizes the request (trust-on-first-use pairing by `clientId` in `PairedClientStore`), enables `adb_enabled`/`adb_wifi_enabled`/`adb_tcp_port`, and starts a session. The client then connects via ADB TCP/IP, pushes/starts `scrcpy-server.jar`, and opens raw video+control TCP sockets directly to the on-device scrcpy server (bypassing `adb shell` for the actual media data).
  3. **Streaming & keepalive:** Video is decoded via FFmpeg (`VideoDecoder`, H.264/H.265) and rendered via SDL2 inside a platform-native window (`VideoRenderer`); input (mouse/keyboard/touch) is forwarded back over the control socket via `ScrcpyClient::inject_*`. While streaming, the client sends `POST /heartbeat` every ~15s to keep the phone-side session alive. `MirroringService` runs a watchdog that disables ADB (wifi + tcp port + the global `adb_enabled` toggle) after 60s without a `/connect` or `/heartbeat` call — closing the ADB attack surface again once the PC disconnects. A session started this way is tracked separately from ADB the user enables manually (e.g. for development): manually-enabled wireless debugging with no active client session is never touched by the watchdog.
- **Windows window**: a custom borderless `Win32` window (`win32_window.cpp`) using `WS_THICKFRAME|WS_CAPTION` with a `WM_NCCALCSIZE` override, custom `WM_NCHITTEST` hit-testing (drag/resize, Win11 snap via `HTMAXBUTTON`), and `DwmExtendFrameIntoClientArea` for the native shadow — SDL2 renders inside a Win32 child window. macOS uses a standard Cocoa/AppKit window instead.
- **Bundled tooling**: the Windows client ships its own `adb.exe`/`AdbWinApi.dll`/`AdbWinUsbApi.dll` under `Client/vendor/platform-tools/`, copied next to the EXE at build time (CMake post-build step also auto-downloads platform-tools from Google if missing). End users should never need Android Studio or an SDK install; ADB search always prefers the bundled copy.
- **Interface/impl pattern**: platform-specific pieces (window, tray) are split into an `_interface.h` plus per-platform implementation, selected via CMake conditional compilation — not preprocessor branching within shared files.
- **scrcpy socket order is fixed**: video → audio → control, opened in exactly that order in `connect_sockets()` and matching the server side. Audio is only opened when enabled. Getting this order wrong makes every stream read the wrong socket.
- **Audio uses raw PCM, deliberately**: `audio_codec=raw` (48 kHz, stereo, s16) means no decoder, no codec-config/extradata handling and no extra FFmpeg dependency on the client; `AudioPlayer` just queues the bytes into SDL2. It costs ~1.5 Mbit/s next to ~20 Mbit/s of video. Do not "optimise" this to Opus without a reason — the config-packet handling is exactly the part that breaks silently.
- **Audio must never take video down**: every failure (device cannot capture, codec id 0/-1, no PC sound device, unexpected codec) logs to `stream.log` and continues video-only. `Settings::m_audio_enabled` turning audio off reproduces the previous server command line exactly, which makes it a usable escape hatch.
- **Socket teardown order**: `ScrcpyClient::stop()` does `shutdown()` → `join()` → `closesocket()`. Closing first would let a reading thread sit on a socket number the OS has already handed to someone else. Control-socket writes are serialised through `send_control()` (mutex + full-write loop) because the UI thread and the screen-poll thread both send.
- **The server process is owned**: the `adb shell app_process ...` that carries the scrcpy server runs in a `pm::adb::ShellProcess` and is killed in `stop()`. Do not go back to a detached thread — that leaked a thread and an `adb.exe` per session.
- **Renderer holds a frame reference, not a copy**: `VideoRenderer` keeps an `av_frame_ref` of the decoded frame and lets SDL upload from it directly. Do not reintroduce per-frame `memcpy` of the YUV planes (~180 MB/s at 1080p60).
- **Recording prefers a hardware encoder**: `CaptureController` tries `h264_mf` (and NVENC/QSV/AMF if present) before falling back to `libx264`, and picks the pixel format the chosen encoder advertises (hardware usually wants NV12).
- **Keyboard shortcuts**: navigation uses **Alt** (`Alt+B` back, `Alt+H` home, `Alt+S` app switch) because Ctrl+C/V carry the clipboard and Ctrl+U/L unlock/lock. Alt keys arrive as `WM_SYSKEYDOWN`/`WM_SYSKEYUP`, not `WM_KEYDOWN`; only B/H/S are claimed so `Alt+F4` still reaches `DefWindowProc`.

## Coding conventions

- **C++ (Client)**: C++20; everything under namespace `pm::` (`pm::adb`, `pm::stream`, `pm::window`, `pm::input`, `pm::tray`, `pm::network`); classes PascalCase, methods snake_case, constants SCREAMING_SNAKE_CASE, member variables prefixed `m_`; ownership via `std::unique_ptr` (raw pointers are non-owning only); no exceptions — use `bool`/`std::optional` return values for error handling.
- **Kotlin (Android)**: package `dev.pixelmirroring.app.*`; foreground-service-based architecture; Compose + Material 3 for UI; Kotlin coroutines for async.
- **Code comments** (both C++ and Kotlin) are written in a deliberately informal "caveman" register (e.g. `// Ugg! ADB not found ... Downloading from Google...`) per project convention in `AGENTS.md` — this applies only to in-code comments, not to user-facing communication, commit messages, or documentation, which should stay normal and professional (German or English).
- No hardcoded paths; conditional compilation goes through CMake, not ad-hoc preprocessor branches.

### Text encoding / German umlauts (mandatory)

The UI is German — every text field must render `ä ö ü Ä Ö Ü ß` correctly. Rules that keep it that way:

- **All source files are UTF-8 (no BOM).** `.editorconfig` at the repo root pins `charset = utf-8`.
- **MSVC must be built with `/utf-8`** (set in `Client/CMakeLists.txt`). Without it MSVC reads UTF-8
  files as cp1252, which corrupts *both* `"..."` and `L"..."` literals — that was the root cause of
  the mojibake in release 3.67. Never drop this flag.
- **Use the `W` variants of every Win32 text API** (`MessageBoxW`, `DrawTextW`, `CreateFontW`,
  `CreateWindowExW`, `RegisterClassExW`, `GetWindowTextW`, …). Never pass German text to an `...A` call.
- **Message loops must be `GetMessageW`/`DispatchMessageW`.** `TranslateMessage` builds `WM_CHAR`
  in the flavour of whatever *fetched* the message, not of the window class — so a suffix-less
  `GetMessage` (= `...A`) routes every keystroke through cp1252 even for a Unicode window.
  Umlauts happen to survive that detour because cp1252 maps them onto the same numbers as
  Unicode; everything outside Latin-1 turns into `?`. `UNICODE` is deliberately **not** defined
  in `Client/CMakeLists.txt`, so every suffix-less Win32 call is the ANSI one — always be explicit.
- Convert between `std::string` (always UTF-8) and `std::wstring` only via `MultiByteToWideChar` /
  `WideCharToMultiByte` with `CP_UTF8` — never `CP_ACP`.
- GDI+ draws `wchar_t` only; convert narrow strings to UTF-16 before `DrawString`.
- **Android:** `-Dfile.encoding=UTF-8` in `gradle.properties`, `compileOptions.encoding = "UTF-8"`,
  and `options.encoding = "UTF-8"` for all `JavaCompile` tasks.
- **Wire format:** read/write HTTP bodies explicitly with `StandardCharsets.UTF_8` and send
  `charset=utf-8` in the Content-Type.
- Never work around an encoding bug by spelling umlauts as `ue`/`oe`/`ae` — fix the cause.

## Guardrails

- Never use browser tech (no Electron/WebView/CEF) for the desktop client.
- Always preserve aspect ratio when rendering the mirrored screen.
- Use the scrcpy protocol as-is for streaming — do not invent a replacement, and do not modify the vendored `scrcpy-server.jar`.
- Don't introduce new frameworks, languages, or build systems into either component.
- No third-party network requests beyond what's needed for ADB/scrcpy/device discovery.
- Keep Android-side battery usage minimal (this is why ADB-over-WiFi and discovery use a lightweight foreground service and a tiny hand-rolled HTTP server rather than heavier alternatives). Concretely: don't add polling loops that spawn an `adb` process per tick, and keep network-callback handling debounced and WLAN-scoped.
- **Hardware video decoding is intentionally absent.** `d3d11va`/`dxva2` exist in the bundled FFmpeg, but SDL2 cannot adopt a foreign D3D11 texture, so each frame would need a GPU→CPU readback that is likely slower than the current software decode. Real zero-copy means replacing SDL2 in the video path — measure before attempting it.
- **The size of shipped artifacts is a feature.** Before adding an Android dependency, check whether it is actually used; before widening the DLL copy rules, check what the binaries import (`dumpbin /DEPENDENTS`).
