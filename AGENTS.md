# AGENTS.md - Pixel Mirroring

## Projektübersicht

Pixel Mirroring ist ein Open-Source Android-Screen-Mirroring-Tool - das Apple iPhone Mirroring fur die Android-Welt. Es besteht aus zwei Hauptkomponenten:

### Architektur

```text
Android App (Kotlin/Jetpack Compose)  <--->  ADB/TCP  <--->  Desktop Client (C++/Win32)
         |                                                     |
   Background Service                                 Custom Borderless Window
   ADB WiFi Toggle                                    scrcpy Protocol Client
   Material 3 UI                                      FFmpeg H.264 Decoder
                                                      SDL2 Renderer
```

### Verzeichnisstruktur

```text
Pixel-Mirroring/
|-- Android/              Kotlin/Jetpack Compose App
|   `-- app/src/main/java/dev/pixelmirroring/app/
|       |-- MainActivity.kt
|       |-- data/         PairedClientStore, Persistenz
|       |-- network/      ApiModels, lokale Netzwerkdaten
|       `-- service/      MirroringService, BootReceiver, NotificationHelper, DiscoveryHttpServer, AdbWifiManager
|-- Client/               C++20 Desktop Client
|   |-- CMakeLists.txt    Build-Config (CMake 3.25+)
|   |-- vendor/platform-tools/  Gebuendelte Android Platform Tools fuer das Desktop-Paket
|   `-- src/
|       |-- main.cpp      Entry Point (WinMain auf Windows, main auf POSIX)
|       |-- adb/          ADB Protocol Client
|       |-- stream/       scrcpy Protocol, Video Decoder/Renderer
|       |-- input/        Input Forwarding (Mouse, Keyboard, Touch)
|       |-- network/      Network Discovery (cpp-httplib, Subnet Scan)
|       |-- window/       Plattform-spezifische Fenster (Win32, Cocoa)
|       `-- tray/         System Tray (Win32, Cocoa)
`-- scrcpy_download/      scrcpy Server Binary
```

---

## Tech Stack & Build

### Android

- Sprache: Kotlin
- UI: Jetpack Compose + Material 3
- Min SDK: Android 11 (API 30)
- Target SDK: Android 15 (API 35)
- Build: Gradle 8.x, JDK 17+

### Desktop Client

- Sprache: C++20
- Build: CMake 3.25+ mit vcpkg
- Abhangigkeiten: SDL2, FFmpeg, nlohmann-json, cpp-httplib
- Windows: Win32 API, GDI+, DWM, UxTheme, WIN32_EXECUTABLE, AppState-Maschine
- macOS: Cocoa/AppKit
- Namensraum: `pm::` mit `pm::adb`, `pm::stream`, `pm::window`, `pm::input`, `pm::tray`, `pm::network`

---

## Aktueller Verbindungsfluss

Der wichtigste Punkt ist jetzt der Einrichtungszustand:

1. Beim ersten Start zeigt der Windows-Client den Setup-Bildschirm.
2. Der Nutzer aktiviert einmal USB-Debugging am Handy.
3. Das Handy wird per USB verbunden.
4. Der Client installiert die Android-App automatisch vom PC aus.
5. Der Client vergibt `WRITE_SECURE_SETTINGS`.
6. Der Client startet die Android-App und aktiviert ADB over WiFi.
7. Erst wenn die Ersteinrichtung komplett erfolgreich war, wird sie gespeichert.
8. Ab dann verbindet sich der Client beim Start automatisch.
9. Der Button `Verbinden` ist dann nur noch fur einen erneuten Verbindungsversuch.

Wichtig:

- Wenn die Ersteinrichtung fehlschlagt, darf der Client nicht stillschweigend in die Netzwerksuche wechseln.
- Ohne gespeicherte Einrichtung bleibt USB der einzige Setup-Pfad.
- Der gespeicherte Setup-Status liegt im Benutzerprofil unter `LOCALAPPDATA\PixelMirroring`.

### ADB nur auf Anfrage (On-Demand) + 60s-Idle-Abschaltung

ADB bleibt zwischen Sitzungen **nicht** mehr dauerhaft aktiviert (Sicherheitsluecke geschlossen):

- Bei jedem Reconnect prueft der Client zuerst, ob bereits ein verbundenes TCP-Geraet existiert (warmer Reconnect innerhalb des Idle-Fensters). Sonst schickt er `POST /connect` (mit persistierter `clientId`) direkt an die gespeicherte Geraet-IP, um ADB aufzuwecken; erst wenn das fehlschlaegt, folgt der Subnetz-Scan.
- Die Android-App (`MirroringService.kt`) prueft die Autorisierung per `clientId` (Trust-on-first-use, `PairedClientStore`) und aktiviert dann `adb_enabled`, `adb_wifi_enabled` und `adb_tcp_port`.
- Waehrend des Streams sendet der Client alle ~15s `POST /heartbeat`, um die Session am Leben zu halten.
- Ein Watchdog auf dem Handy deaktiviert ADB (alle drei Settings) nach 60s ohne `/connect` oder `/heartbeat` wieder.
- Manuell aktiviertes Wireless-Debugging ohne aktive Client-Session wird vom Watchdog nie angefasst — nur Sessions, die die App selbst gestartet hat, werden wieder geschlossen.

---

## Desktop Client Details

### Eingebundene Tools

- Der Windows-Client benutzt sein eigenes gebuendeltes `adb.exe`.
- Die Android Platform Tools liegen unter `Client/vendor/platform-tools/`.
- Fur Nutzer soll Android Studio nicht notwendig sein.
- Beim Build werden `adb.exe`, `AdbWinApi.dll` und `AdbWinUsbApi.dll` neben die EXE kopiert.

### ADB-Suche

- Bevorzugt wird immer das gebuendelte ADB aus dem App-Paket oder dem lokalen Client-Ordner.
- Android-Studio-Installationen oder SDK-Pfade sollen fur den normalen Nutzer keine Voraussetzung sein.

### Zustandslogik

- `SETUP`: Erste Einrichtung oder fehlgeschlagene Einrichtung
- `SCANNING`: Automatische Verbindung oder Wiederverbindungsversuch
- `CONNECTED`: ADB steht, Stream wird vorbereitet
- `STREAMING`: Video-Stream laeuft

---

## Coding-Konventionen

### C++ (Desktop Client)

- Standard: C++20
- Namespaces: alles unter `pm::`
- Klassen: PascalCase
- Methoden: snake_case
- Konstanten: SCREAMING_SNAKE_CASE
- Member-Variablen: `m_` Prefix
- Ownership: `std::unique_ptr` fuer Ownership, Raw Pointer nur non-owning
- Error Handling: Return-Werte (`bool`, `std::optional`), keine Exceptions
- Kommentare: Caveman-Sprache

### Kotlin (Android App)

- Architektur: Service-basiert mit Foreground Service
- UI: Jetpack Compose mit Material 3
- Async: Kotlin Coroutines
- Packages: `dev.pixelmirroring.app.*`
- Kommentare: Caveman-Sprache

### Plattformuebergreifend

- Interface/Impl Pattern fuer Fenster und platform-spezifische Teile
- Bedingte Kompilierung ueber CMake, nicht mit wildem Preprocessor-Gewuehl
- Keine hartcodierten Pfade

---

## Wichtige Architektur-Regeln

1. Kein Browser-Technologie. Kein Electron, kein WebView, kein CEF.
2. Aspect Ratio immer beibehalten.
3. Windows: Custom Borderless Window mit eigenem Hit-Testing.
4. macOS: Standard Cocoa Window.
5. scrcpy-Protokoll nutzen, keine eigene Streaming-Losung erfinden.
6. Android App aktiviert ADB over WiFi selbst via `Settings.Global.putInt("adb_wifi_enabled", 1)`.
7. Minimaler Akkuverbrauch auf Android.
8. Der Client soll neue Nutzer sauber durch USB-Ersteinrichtung fuehren und danach automatisch verbinden.

---

## Testen

### Desktop Client

- CMake Build: `cmake --preset default && cmake --build build/`
- Manueller Test mit angeschlossenem Android-Gerat

### Android

- Gradle Build: `./gradlew assembleDebug`
- Manueller Test auf physischem Geraet

---

## Was der Agent nicht tun soll

- Keine neuen Frameworks einfuehren
- Keine Sprache wechseln
- Nicht den offiziellen scrcpy-Server modifizieren
- Keine neuen Build-Systeme einfuehren
- Keine externen Netzwerk-Requests an Drittanbieter

---

## Zusammenfassung der Caveman-Regeln

| Kontext | Sprache |
|---|---|
| Internes Denken / Reasoning | Caveman |
| User-Kommunikation | Normales Deutsch, professionell & technisch |
| Code-Kommentare (C++) | Caveman |
| Code-Kommentare (Kotlin) | Caveman |
| Commit Messages | Normales Deutsch oder Englisch |
| Dokumentation | Normales Deutsch oder Englisch |
