# AGENTS.md — Pixel Mirroring
 📐 Projektübersicht

Pixel Mirroring ist ein Open-Source Android-Screen-Mirroring-Tool — das Apple iPhone Mirroring für die Android-Welt. Es besteht aus zwei Hauptkomponenten:

### Architektur
```
Android App (Kotlin/Jetpack Compose)  ◄──► ADB/TCP ◄──►  Desktop Client (C++/Win32)
         │                                                        │
   Background Service                                    Custom Borderless Window
   ADB WiFi Toggle                                       scrcpy Protocol Client
   Network Discovery                                     FFmpeg H.264 Decoder
   Material 3 UI                                         SDL2 Renderer
```

### Verzeichnisstruktur
```
Pixel-Mirroring/
├── Android/              ← Kotlin/Jetpack Compose App
│   └── app/src/main/java/dev/pixelmirroring/app/
│       ├── MainActivity.kt
│       ├── data/         ← PairedClientStore, Persistenz
│       ├── network/      ← NetworkScanner, ApiModels, Discovery
│       └── service/      ← MirroringService, BootReceiver, NotificationHelper
├── Client/               ← C++20 Desktop Client
│   ├── CMakeLists.txt    ← Build-Config (CMake 3.25+)
│   ├── vcpkg.json        ← Dependencies (SDL2, FFmpeg, nlohmann-json)
│   └── src/
│       ├── main.cpp      ← Entry Point (WinMain auf Windows, main auf POSIX)
│       ├── adb/          ← ADB Protocol Client
│       ├── stream/       ← scrcpy Protocol, Video Decoder/Renderer
│       ├── input/        ← Input Forwarding (Mouse, Keyboard, Touch)
│       ├── network/      ← Network Discovery (cpp-httplib, Subnet Scan)
│       ├── window/       ← Platform-spezifische Fenster (Win32, Cocoa)
│       └── tray/         ← System Tray (Win32, Cocoa)
└── scrcpy_download/      ← scrcpy Server Binary
```

---

## 🛠️ Tech Stack & Build

### Android
- **Sprache:** Kotlin
- **UI:** Jetpack Compose + Material 3
- **Min SDK:** Android 11 (API 30)
- **Target SDK:** Android 15 (API 35)
- **Build:** Gradle 8.x, JDK 17+

### Desktop Client
- **Sprache:** C++20
- **Build:** CMake 3.25+ mit vcpkg
- **Abhängigkeiten:** SDL2, FFmpeg (libavcodec/libavformat/libavutil/libswscale), nlohmann-json, cpp-httplib
- **Windows:** Win32 API, GDI+ (Anti-Aliased Rendering), DWM, UxTheme (Custom Borderless Window), WIN32_EXECUTABLE (kein Konsolenfenster), AppState-Machine (SETUP→SCANNING→CONNECTED→STREAMING)
- **macOS:** Cocoa/AppKit (Native Window)
- **Namensraum:** `pm::` (Subnamespaces: `pm::adb`, `pm::stream`, `pm::window`, `pm::input`, `pm::tray`, `pm::network`)

---

## 📏 Coding-Konventionen

### C++ (Desktop Client)
- **Standard:** C++20 — `constexpr`, `std::optional`, Structured Bindings, etc. nutzen
- **Namespaces:** Alles unter `pm::` mit Subnamespaces pro Modul
- **Header Guards:** Pragma once oder klassische Include Guards
- **Klassen:** PascalCase (`ScrcpyClient`, `VideoDecoder`, `Win32Window`)
- **Methoden:** snake_case (`get_connected_devices()`, `auto_grant_secure_settings()`)
- **Konstanten:** SCREAMING_SNAKE_CASE (`FRAME_BUFFER_SIZE`)
- **Member-Variablen:** `m_` Prefix (`m_socket`, `m_device_id`)
- **Interfaces:** Abstrakte Klasse mit `= 0` Pure Virtuals, benannt als `*Interface` (z.B. `WindowInterface`)
- **Factory Pattern:** `create_*` Funktionen für plattformspezifische Implementierungen
- **Error Handling:** Return-Werte (bool/optional), keine Exceptions
- **Ownership:** `std::unique_ptr` für Ownership, Raw Pointer nur für Non-Owning References
- **Kommentare:** Caveman-Sprache! 🦴

### Kotlin (Android App)
- **Architektur:** Service-basiert mit Foreground Service
- **UI:** Jetpack Compose mit Material 3 Theming
- **Async:** Kotlin Coroutines (suspend functions, Flow)
- **Packages:** `dev.pixelmirroring.app.*` (`.service`, `.network`, `.data`, `.ui`)
- **Naming:** Kotlin-Conventions (camelCase Funktionen, PascalCase Klassen)
- **Data Classes:** Für API-Models und DTOs
- **Kommentare:** Caveman-Sprache! 🦴

### Plattformübergreifend
- **Interface/Impl Pattern:** Gemeinsames Interface (`window_interface.h`), plattformspezifische Implementierungen (`win32_window.cpp`, `cocoa_window.mm`)
- **Bedingte Kompilierung:** Über CMake Variablen (`WIN32`, `APPLE`), nicht über Präprozessor im Code
- **Keine hartcodierten Pfade:** Konfiguration über Settings/Config Module

---

## ⚠️ Wichtige Architektur-Regeln

1. **Kein Browser-Technologie.** Kein Electron, kein WebView, kein CEF. Alles nativ.
2. **Aspect Ratio immer beibehalten.** Das Fenster darf frei skaliert werden, aber der Video-Stream behält IMMER das Seitenverhältnis.
3. **Custom Borderless Window (Windows).** Kein Standard-Titlebar. Eigenes Hit-Testing via `WM_NCHITTEST`. Snap Layouts via `HTMAXBUTTON`.
4. **macOS = Standard Cocoa Window.** Kein Custom Chrome auf macOS.
5. **scrcpy-Protokoll.** Der Desktop Client kommuniziert mit dem scrcpy-Server auf dem Android-Gerät. Keine eigene Streaming-Lösung erfinden.
6. **ADB WiFi Auto-Setup.** Die Android App aktiviert ADB over WiFi selbst via `Settings.Global.putInt("adb_wifi_enabled", 1)`. Dafür braucht sie `WRITE_SECURE_SETTINGS` (einmalig per USB-ADB erteilt).
7. **Minimaler Akkuverbrauch.** Der Background Service auf Android soll so wenig wie möglich tun — nur auf eingehende Discovery-Requests reagieren.

---

## 🔄 Verbindungsfluss

```
1. Desktop Client startet
2. Sendet Discovery-Broadcast an bekannte IPs
3. Android App empfängt im Background Service
4. App aktiviert ADB WiFi
5. Desktop Client verbindet via ADB TCP/IP
6. scrcpy-Server wird gepusht + gestartet
7. Video-Stream startet im Custom Window
8. Input wird zurück an Android gesendet
```

---

## 🧪 Testen

### Desktop Client
- CMake Build testen: `cmake --preset default && cmake --build build/`
- Manuelles Testen mit angeschlossenem Android-Gerät
- Keine Test-Frameworks konfiguriert (noch)

### Android
- Gradle Build: `./gradlew assembleDebug`
- Manuelles Testen auf physischem Gerät (Emulator reicht nicht für ADB WiFi)

---

## 🚫 Was der Agent NICHT tun soll

- **Keine Frameworks einführen** die nicht schon im Stack sind (kein Qt, kein GTK, kein Electron)
- **Keine Sprache wechseln** — C++ für Desktop, Kotlin für Android
- **Nicht den scrcpy-Server modifizieren** — wir nutzen den offiziellen als Binary
- **Keine neuen Build-Systeme** — CMake für Desktop, Gradle für Android
- **Keine externen Netzwerk-Requests** an Drittanbieter-Server — alles bleibt lokal im LAN

---

## 📝 Zusammenfassung der Caveman-Regeln

| Kontext | Sprache |
|---------|---------|
| Internes Denken / Reasoning | 🦴 Caveman ("Ugg fix bug. Socket wie kaputter Ast.") |
| User-Kommunikation | 🗣️ Normales Deutsch, professionell & technisch |
| Code-Kommentare (C++) | 🦴 Caveman |
| Code-Kommentare (Kotlin) | 🦴 Caveman |
| Commit Messages | 🗣️ Normales Deutsch oder Englisch |
| Dokumentation (README, etc.) | 🗣️ Normales Deutsch oder Englisch |
