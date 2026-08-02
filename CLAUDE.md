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
        input/                  pm::input — forwards mouse/keyboard/touch into ScrcpyClient::inject_* over the control socket,
                                 plus HidKeyboard: PC set-1 scancodes → USB HID boot-keyboard reports for the optional UHID keyboard
        network/                pm::network — LAN subnet scan + discovery via cpp-httplib
        window/                 pm::window — window_interface.h + win32_window.{h,cpp} / cocoa_window.{h,mm}
        tray/                   pm::tray — tray_interface.h + win32_tray.{h,cpp}
        util/                   pm::util — encoding.{h,cpp}: the ONLY conversions between UTF-8
                                 std::string, std::wstring and std::filesystem::path
    tests/                      standalone self-checks (plain main() + check(), no test framework)
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

**Testing:** verification is essentially manual, with a physical/connected Android
device, on both sides. The one exception is `Client/tests/hid_keyboard_test.cpp` — a
standalone `main()` with a hand-rolled `check()` (deliberately **not** `assert`: the
shipped build defines `NDEBUG` and would compile every assertion away). It pins the
PC-scancode → HID-usage table, the AltGr phantom-Ctrl mask, extended-vs-keypad pairs
and 6-key rollover. It builds as target `pm_hid_keyboard_test` (option
`PM_BUILD_TESTS`, default ON), links nothing but `hid_keyboard.cpp`, and is neither
installed nor packaged:

```
ctest --test-dir build --output-on-failure
```

Android has only default JUnit/Espresso boilerplate. Do not add a test framework —
extend the same pattern if something else needs pinning.

**CI**: two workflows.
- `.github/workflows/build.yml` — on every pull request, on pushes to `main`, and on
  manual dispatch. Builds Android and the Windows client with MSVC (same compiler and
  same `/utf-8` as the release) and runs `ctest`. No packaging, no artifacts, no
  release. This exists because a release that builds only on tags means large PRs get
  merged having never been compiled by the toolchain that actually ships them.
- `.github/workflows/release.yml` — on `v*` tags or manual dispatch. Builds the Android
  APK first (`gradle assembleDebug`), then the Windows client with that APK bundled in,
  packages installers with `cpack`, and creates a GitHub Release on tag pushes.

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
- **Dropped files are transferred deliberately slowly**: files dragged onto the window go out through `AdbClient::push_file_paced` in 1 MiB pieces, and after each piece the transfer sleeps for exactly as long as that piece took. The stream therefore keeps at least half the airtime no matter how fast the WiFi is today. Do not "optimise" this back into a single `adb push` — the picture stutters for the whole transfer. Pieces grow under `<target>.pmpart` and only get the real name via `mv` at the end, so an aborted transfer cannot destroy an existing file. A dropped APK the user wants installed goes to `/data/local/tmp` and is unpacked there with `pm install`: `adb install` would send it a second time at full speed, and the package installer cannot read out of `/sdcard`. `install_pushed_app()` deliberately omits the `-g` that `install_app()` uses — pre-granting every runtime permission is defensible for our own APK, which the user installed on purpose, but a file dragged into the window is a stranger and must ask the phone for camera/mic/location like any other app. Leftover `.pmpart`/`.pmglue` are swept with `rm -f` before the first piece: the tidy-up at the end of `push_file_paced` only runs when the transfer *ends*, so a hard-killed client leaves them visible in the phone's file manager forever.
- **Auto-pause while minimized** (`Settings::m_auto_pause_minimized`, default **on**): a window folded down to the taskbar shows nothing, but the phone keeps encoding and the WLAN keeps carrying ~20 Mbit/s. `Win32Window` fires `set_minimize_callback` on the `SIZE_MINIMIZED`/`SIZE_RESTORED` edge only (plain `WM_SIZE` also fires on every resize drag), and `main.cpp` reconciles from there. Rules:
  - **The heartbeat must outlive the stream.** `POST /heartbeat` is what stops the phone-side watchdog from disabling ADB after 60s, and that is the entire reason the way back is warm — resume is just `start_stream()` again, with no discovery, no `/connect`, no pairing. The heartbeat loop therefore runs while `scrcpy.is_running() || auto_paused`, and `auto_paused` is raised **before** `scrcpy.stop()`; the other order leaves a window in which both are false and the heartbeat thread exits for good.
  - **The window thread only writes a wish.** `pause_wanted` is an atomic set on the UI thread; one background worker (`reconcile_pause_state`) walks until the world matches it. Tearing a stream down or building one up on the window thread would freeze the UI.
  - **Pause is skipped, never forced.** Not while recording (a torn stream leaves a broken file) and not during `apply_quality_now` (it already owns the stream). If pause is skipped, `auto_paused` stays false, so restoring does nothing either. An in-flight connect is deliberately *not* in that list: it only raises `is_running()` once the stream really stands, and it re-runs the reconcile at the end of its own path.
  - **Resume falls back to the cold road.** If `start_stream()` fails on restore — phone asleep, WLAN moved, ADB shut anyway — the worker calls `start_connection(true)` instead of leaving a dead window.
- **Keyboard shortcuts**: navigation uses **Alt** (`Alt+B` back, `Alt+H` home, `Alt+S` app switch, `Alt+Down` notification panel, `Alt+Shift+Down` quick settings, `Alt+Up` collapse) because Ctrl+C/V carry the clipboard and Ctrl+U/L unlock/lock. Alt keys arrive as `WM_SYSKEYDOWN`/`WM_SYSKEYUP`, not `WM_KEYDOWN`; only B/H/S and the up/down arrows are claimed so `Alt+F4` still reaches `DefWindowProc`. B/H/S are Android keycodes and go through `set_key_callback`; the arrows are scrcpy control messages 5/6/7 and therefore ride the `MenuAction` path instead, like Ctrl+U/L do. Both the press and every auto-repeat of `Alt+Up`/`Alt+Down` must be swallowed — a lone Alt reaching `DefWindowProc` makes Windows beep at a menu bar that does not exist.
- **Turning the phone's panel off is opt-in and must always be undone.** `Settings::m_screen_off` (default false) makes the client send scrcpy control message 10 `SET_SCREEN_POWER_MODE(OFF)` once the stream is up — the honest version of `m_lowest_brightness`, which only dims via ADB. The phone stays awake and steerable; only the panel is dark, so `PowerManager.isInteractive` stays true and the screen-poll watchdog does not misfire. Restoring it has three layers and none of them are redundant: (1) `ScrcpyClient::stop()` sends `NORMAL` *before* the sockets are torn down, (2) the scrcpy server's own device-side CleanUp restores normal power mode when the server dies, (3) if `screen_forced_off()` is still true at app teardown, `main.cpp` sends `input keyevent 223; sleep 0.5; input keyevent 224` over ADB. Layer 3 needs the sleep/wake pair — a bare wakeup does nothing because Android only re-issues a display power mode when its *own* screen state changes, and it already thinks the screen is on. `screen_forced_off_` is deliberately **not** reset in `start()`: a panel left dark by a session whose socket died must still be fixable by the next one.
- **Optional UHID keyboard** (`Settings::m_uhid_keyboard`, off by default): instead of `inject_text`, a virtual USB HID keyboard is attached to the device (scrcpy control messages 12 `UHID_CREATE` / 13 `UHID_INPUT` / 14 `UHID_DESTROY`, plus 15 `OPEN_HARD_KEYBOARD_SETTINGS`). Rules that keep it working:
  - **Probe before creating.** `ScrcpyClient::device_supports_uhid()` checks `/dev/uhid` over ADB first. If the server's `UhidManager.open()` throws, the exception escapes `handleEvent()` and kills the server's *entire* control thread — mouse, touch, keys and clipboard all die silently while video keeps running. Never send `UHID_CREATE` unprobed. The probe opens **read-write** (`(exec 3<>/dev/uhid)`), because the server reads the phone's UHID output reports back out of the same descriptor — a write-only probe would pass on a device that refuses reads and then tear the very thread it exists to protect. It runs inside a subshell because a failing `exec` kills the shell it sits in. The result is cached per `device_id`, so reconnects and quality restarts do not pay for another `adb` child.
  - **Wire format is version-specific.** The bundled server 2.7 expects `type(1) id(2 BE) name_len(1) name rd_size(2 BE) rd_data`; the `name` field does not exist before 2.7 and gained vendor/product ids in later scrcpy releases. Verify against the vendored jar before changing it.
  - **Exactly one path may fire per key.** `Win32Window::uhid_keyboard_` routes: while it is on, `WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` go to `send_raw_key()` and `WM_CHAR` is dropped; while it is off, nothing changes. Volume keys and the Alt/Ctrl shortcuts are the deliberate exceptions.
  - **A refused key must fall back, not vanish.** The raw-key callback returns `bool` all the way from `HidKeyboard::process_key()` through `InputHandler::handle_raw_key()` to `Win32Window::send_raw_key()`; `false` means "a boot keyboard has no such key" and the window then walks the Android-keycode path. `send_raw_key()` remembers which scancodes the HID keyboard took (`hid_held_keys_`) purely so auto-repeats of a *refused* key keep falling through as well. A key whose press was eaten by a shortcut (Ctrl+U/L) has its release eaten too (`swallowed_shortcuts_`), otherwise the phone gets a release for a key it never saw pressed.
  - **Scancodes, not virtual keys.** The point of the feature is that the *phone's* layout decides the character, so the window sends the physical key position from `lParam`, never `wParam`.
  - **AltGr** on Windows always comes with a phantom left Ctrl; `HidKeyboard::build_report()` masks it out while right Alt is held. Removing that makes `AltGr+Q` arrive as `Ctrl+AltGr+Q`.
  - Key repeat is the device's job (that is how a real keyboard works) — the client sends each press once.

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
  `WideCharToMultiByte` with `CP_UTF8` — never `CP_ACP`. The four conversions live in
  `Client/src/util/encoding.{h,cpp}` (`pm::util::path_from_utf8` / `path_to_utf8` /
  `utf8_to_wide` / `wide_to_utf8`) — use those, do not hand-roll a fifth copy.
- **Never call `std::filesystem::path::string()` on Windows.** It hands out the ANSI code page,
  where Cyrillic/CJK/emoji become `?`. Use `pm::util::path_to_utf8()`; the road back is
  `path_from_utf8()`, never `std::filesystem::path(narrow_string)`.
- **Every `std::string` crossing a module boundary is UTF-8**, including all paths going into
  `pm::adb`. `AdbClient` spawns adb with `CreateProcessW` and `get_executable_dir()` uses
  `GetModuleFileNameW`, so the whole chain PC → adb → phone is one encoding. FFmpeg's file
  opener (`avio_open`, `avformat_alloc_output_context2`) also expects UTF-8 on Windows.
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
