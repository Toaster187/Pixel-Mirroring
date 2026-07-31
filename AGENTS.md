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
                                                      SDL2 Renderer + Audio
```

Drei getrennte TCP-Kanaele des scrcpy-Protokolls, in genau dieser Reihenfolge
geoeffnet: **Video -> Audio -> Control**.

### Verzeichnisstruktur

```text
Pixel-Mirroring/
|-- Android/              Kotlin/Jetpack Compose App
|   |-- app/proguard-rules.pro  R8 Keep-Regeln (Manifest-Klassen, Serializer)
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
|       |-- settings.*    Persistente Einstellungen inkl. Ton an/aus
|       |-- adb/          ADB Protocol Client + ShellProcess (abbrechbarer adb-shell-Prozess)
|       |-- stream/       scrcpy Protocol, VideoDecoder, VideoRenderer, AudioPlayer, CaptureController
|       |-- input/        Input Forwarding (Mouse, Keyboard, Touch)
|       |-- network/      Network Discovery (cpp-httplib, Subnet Scan)
|       |-- window/       Plattform-spezifische Fenster (Win32; Cocoa unfertig)
|       `-- tray/         System Tray (Win32)
`-- scrcpy_download/      scrcpy Server Binary
```

**macOS ist derzeit nicht baubar.** `CocoaWindow` implementiert nur einen Bruchteil der
rein virtuellen `IWindow`-Methoden, und die in `CMakeLists.txt` referenzierte
`src/tray/cocoa_tray.mm` existiert gar nicht. Die CI baut ausschliesslich Android und
Windows. Eine neue Methode in `IWindow` kann macOS also nicht "kaputt machen" - es ist
bereits kaputt. In nutzerseitiger Doku darf macOS trotzdem nicht als unterstuetzt
auftauchen.

---

## Tech Stack & Build

### Android

- Sprache: Kotlin
- UI: Jetpack Compose + Material 3
- Min SDK: Android 11 (API 30)
- Target SDK: Android 15 (API 35)
- Build: Gradle 9.4.1, JDK 17+ (Wrapper ist bewusst nicht eingecheckt)

**Die Debug-Variante ist das ausgelieferte Artefakt** - der Desktop-Client installiert
sie, die CI veroeffentlicht sie. Deshalb laufen auf `debug` ausnahmsweise
`isMinifyEnabled` und `isShrinkResources`; ohne sie ist die APK rund 60 statt 4 MB gross.
Folgen davon:

- Klassen, die nur ueber das Manifest oder per Reflexion erreicht werden, brauchen eine
  Keep-Regel in `app/proguard-rules.pro`.
- Compose-Tooling steckt absichtlich nicht in der Auslieferung. Lokale Previews:
  `gradle assembleDebug -PcomposeTooling`.
- Bei `debuggable`-Builds macht R8 nur Tree-Shaking, keine Obfuskation - die risikoarme Variante.

### Desktop Client

- Sprache: C++20
- Build: CMake 3.25+ mit vcpkg
- Abhangigkeiten: SDL2, FFmpeg, nlohmann-json, cpp-httplib
- Windows: Win32 API, GDI+, DWM, UxTheme, WIN32_EXECUTABLE, AppState-Maschine
- macOS: Cocoa/AppKit
- Namensraum: `pm::` mit `pm::adb`, `pm::stream`, `pm::window`, `pm::input`, `pm::tray`, `pm::network`

---

## Aktueller Verbindungsfluss

**Übersicht der Zustände:** `SETUP` → `SCANNING` → `CONNECTED` → `STREAMING`

### 1. Ersteinrichtung (USB-Setup, Einmalig)

1. Benutzer aktiviert USB-Debugging am Android-Gerät
2. Gerät wird per USB an den PC angeschlossen
3. Desktop Client erkennt das Gerät automatisch via ADB
4. Client installiert die Android App vom PC aus
5. Client erteilt `WRITE_SECURE_SETTINGS` Berechtigung via ADB (keine Terminal-Eingabe nötig!)
6. Client startet die Android App
7. App aktiviert ADB over WiFi (setzt `Settings.Global.putInt("adb_wifi_enabled", 1)` selbst)
8. Einrichtung ist komplett → wird dauerhaft unter `%LOCALAPPDATA%\PixelMirroring` gespeichert

**Kritisch:** Falls Ersteinrichtung fehlschlägt, wird der Status NICHT gespeichert. USB bleibt der einzige Setup-Pfad.

### 2. Automatische Verbindung (Ab dem 2. Mal) — On-Demand ADB + 60s Idle-Abschaltung

ADB **bleibt nicht dauerhaft aktiviert** (Sicherheitslücke geschlossen). Stattdessen:

- Desktop Client startet → prüft zuerst, ob bereits ein verbundenes TCP-Gerät existiert (warmer Reconnect innerhalb des Idle-Fensters)
- Falls nicht: Client sendet `POST /connect` (mit persistierter `clientId`) direkt an die gespeicherte Geräte-IP zur Hand-Roll HTTP Discovery-Schnittstelle (`DiscoveryHttpServer.kt`)
- Falls die gespeicherte IP nicht antwortet, folgt LAN-Subnetz-Scan
- Android App (`MirroringService.kt`) prüft Autorisierung via `clientId` (Trust-on-first-use in `PairedClientStore`)
- App aktiviert dann `adb_enabled`, `adb_wifi_enabled` und `adb_tcp_port`
- Desktop Client verbindet sich via ADB TCP/IP
- Client pusht und startet `scrcpy-server.jar`
- Video-, Audio- und Control-Sockets werden direkt zum scrcpy-Server geöffnet (ADB Shell wird für Mediendaten umgangen)
- Während Stream: Client sendet alle ~15s `POST /heartbeat` um Session aktiv zu halten
- **Watchdog auf dem Gerät:** ADB wird nach 60s ohne `/connect` oder `/heartbeat` automatisch deaktiviert (alle drei Settings)
- **Wichtig:** Manuell aktiviertes Wireless Debugging wird vom Watchdog nie angefasst — nur Sessions, die die App selbst gestartet hat

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

### Textkodierung / Umlaute (PFLICHT)

Alle deutschen Umlaute (ä ö ü Ä Ö Ü ß) und sonstigen Nicht-ASCII-Zeichen muessen in
jedem Textfeld korrekt dargestellt werden. Damit das dauerhaft haelt, gilt:

- **Alle Quelldateien sind UTF-8** (ohne BOM). `.editorconfig` im Repo-Root setzt `charset = utf-8`.
- **MSVC braucht `/utf-8`** (in `Client/CMakeLists.txt` gesetzt). Ohne diesen Schalter liest MSVC
  UTF-8-Dateien als cp1252 - dann werden sowohl `"..."` als auch `L"..."` Literale falsch kodiert
  und die gesamte deutsche UI zeigt Buchstabensalat. Diesen Schalter niemals entfernen.
- **Windows-API immer die `W`-Variante** verwenden (`MessageBoxW`, `DrawTextW`, `CreateFontW`,
  `CreateWindowExW`, `RegisterClassExW`, ...). Nie `...A` mit deutschem Text.
- Zwischen `std::string` (immer UTF-8) und `std::wstring` nur ueber `MultiByteToWideChar` /
  `WideCharToMultiByte` mit `CP_UTF8` konvertieren - nie `CP_ACP`.
- GDI+ zeichnet nur `wchar_t`: narrow Strings vorher nach UTF-16 konvertieren.
- **Android**: `-Dfile.encoding=UTF-8` in `gradle.properties`, `compileOptions.encoding = "UTF-8"`
  und `options.encoding = "UTF-8"` fuer alle `JavaCompile`-Tasks.
- **Netzwerk**: HTTP-Bodies immer explizit mit `StandardCharsets.UTF_8` lesen/schreiben und
  `charset=utf-8` im Content-Type mitschicken.
- Umlaute nicht durch `ue`/`oe`/`ae` ersetzen, um ein Encoding-Problem zu umgehen - die Ursache fixen.

---

## Wichtige Architektur-Regeln

1. Kein Browser-Technologie. Kein Electron, kein WebView, kein CEF.
2. Aspect Ratio immer beibehalten.
3. Windows: Custom Borderless Window mit eigenem Hit-Testing.
4. scrcpy-Protokoll nutzen, keine eigene Streaming-Losung erfinden.
5. Android App aktiviert ADB over WiFi selbst via `Settings.Global.putInt("adb_wifi_enabled", 1)`.
6. Minimaler Akkuverbrauch auf Android. Konkret: keine Poll-Schleifen, die pro Runde
   einen `adb`-Prozess starten, und Netzwerk-Callbacks entprellt und auf WLAN begrenzt.
7. Der Client soll neue Nutzer sauber durch USB-Ersteinrichtung fuehren und danach automatisch verbinden.

### Invarianten im Stream-Pfad (nicht aus Versehen zurueckdrehen)

- **Socket-Reihenfolge liegt fest:** Video -> Audio -> Control, exakt so in
  `connect_sockets()` und passend zur Server-Seite. Audio nur, wenn aktiviert.
  Falsche Reihenfolge = jeder Stream liest aus dem falschen Kanal.
- **Ton laeuft bewusst als rohes PCM** (`audio_codec=raw`, 48 kHz, Stereo, s16): kein
  Decoder, keine Codec-Konfiguration, keine zusaetzliche FFmpeg-Abhaengigkeit auf der
  Client-Seite. Kostet ~1,5 Mbit/s neben ~20 Mbit/s Video. Nicht ohne Grund auf Opus
  "optimieren" - die Config-Pakete sind genau der Teil, der still kaputtgeht.
- **Ton darf das Bild nie mitreissen:** jeder Fehlerfall (Handy kann nicht aufnehmen,
  Codec-Kennung 0/-1, kein PC-Audiogeraet) landet in `stream.log` und laeuft video-only
  weiter. Ton ausschalten stellt exakt die alte Server-Befehlszeile wieder her.
- **Socket-Abbau:** `stop()` macht `shutdown()` -> `join()` -> `closesocket()`. Zuerst
  schliessen wuerde einen lesenden Thread auf einer Socket-Nummer sitzen lassen, die das
  Betriebssystem schon neu vergeben hat. Schreibzugriffe auf den Control-Socket laufen
  ueber `send_control()` (Mutex + vollstaendige Schreibschleife), weil UI-Thread und
  Screen-Poll-Thread beide senden.
- **Der Serverprozess gehoert uns:** das `adb shell app_process ...` laeuft in einem
  `pm::adb::ShellProcess` und wird in `stop()` beendet. Kein detached Thread mehr - das
  hat pro Sitzung einen Thread und eine `adb.exe` liegengelassen.
- **Der Renderer haelt eine Referenz, keine Kopie** (`av_frame_ref`). Kein
  Pro-Bild-`memcpy` der YUV-Ebenen wieder einbauen (~180 MB/s bei 1080p60).
- **Aufnahmen bevorzugen den Hardware-Encoder** (`h264_mf`, sonst NVENC/QSV/AMF), mit
  automatischem Rueckfall auf `libx264` und passendem Pixelformat.
- **Tastenkuerzel:** Navigation liegt auf **Alt** (`Alt+B` zurueck, `Alt+H` Start,
  `Alt+S` Uebersicht, `Alt+N` Benachrichtigungen, `Alt+Q` Schnelleinstellungen,
  `Alt+Umschalt+N/Q` schliesst wieder), weil Strg+C/V die Zwischenablage und Strg+U/L
  Sperren/Entsperren belegen. Alt-Tasten kommen als `WM_SYSKEYDOWN`/`WM_SYSKEYUP`; nur
  B/H/S/N/Q werden abgefangen, damit `Alt+F4` weiter bei `DefWindowProc` ankommt.
  B/H/S sind Android-Keycodes und laufen ueber `set_key_callback`; N/Q sind
  scrcpy-Control-Messages 5/6/7 und laufen deshalb ueber den `MenuAction`-Weg,
  genau wie Strg+U/L.
- **Display komplett aus ist Opt-in und muss IMMER rueckgaengig gemacht werden:**
  `Settings::m_screen_off` (Standard aus) schickt Control-Message 10
  `SET_SCREEN_POWER_MODE(OFF)`, sobald der Stream steht - die ehrliche Version von
  `m_lowest_brightness`, das nur per ADB dimmt. Das Handy bleibt wach und steuerbar,
  nur das Panel ist dunkel; `PowerManager.isInteractive` bleibt deshalb true und der
  Screen-Poll-Waechter schlaegt nicht faelschlich an. Zurueckgestellt wird auf drei
  Wegen, keiner davon ist ueberfluessig: (1) `ScrcpyClient::stop()` schickt `NORMAL`
  *bevor* die Sockets abgebaut werden, (2) der scrcpy-Server haelt auf dem Handy einen
  eigenen CleanUp-Prozess, der auch beim Serverabsturz greift, (3) ist
  `screen_forced_off()` beim Beenden noch true, schickt `main.cpp` per ADB
  `input keyevent 223; sleep 0.5; input keyevent 224`. Der Schlaf-Weck-Doppelschlag in
  (3) ist noetig: ein blosses Wecken tut nichts, weil Android einen Display-Power-Mode
  nur dann neu setzt, wenn sich sein *eigener* Bildschirmzustand aendert - und der ist
  aus seiner Sicht schon an. `screen_forced_off_` wird in `start()` absichtlich **nicht**
  zurueckgesetzt: ein Panel, das eine Sitzung mit totem Socket dunkel gelassen hat, muss
  von der naechsten noch repariert werden koennen.
- **Dateiuebertragung ist absichtlich langsam:** ins Fenster gezogene Dateien gehen
  ueber `AdbClient::push_file_paced` in 1-MiB-Stuecken raus, und nach jedem Stueck wartet
  der Transfer genau so lange, wie das Stueck gedauert hat. Damit gehoert dem Stream
  mindestens die Haelfte der Funkzeit, unabhaengig davon, wie schnell das WLAN heute ist.
  Nicht auf ein einzelnes `adb push` "optimieren" - dann ruckelt das Bild waehrend der
  Uebertragung. Die Stuecke wachsen unter `<ziel>.pmpart` und bekommen erst per `mv` den
  echten Namen, damit eine abgebrochene Uebertragung keine vorhandene Datei zerstoert.
  Eine abgelegte APK, die installiert werden soll, wandert nach `/data/local/tmp` und
  wird von dort per `pm install` entpackt - `adb install` wuerde sie ein zweites Mal
  ungebremst uebertragen, und der Paketinstallierer kommt nicht an `/sdcard` heran.
- **Hardware-Dekodierung fehlt mit Absicht:** `d3d11va`/`dxva2` sind vorhanden, aber
  SDL2 kann keine fremde D3D11-Textur uebernehmen. Die noetige GPU->CPU-Rueckkopie
  waere vermutlich langsamer als die jetzige Software-Dekodierung. Erst messen.
- **Artefaktgroesse ist ein Feature:** vor einer neuen Android-Abhaengigkeit pruefen, ob
  sie ueberhaupt benutzt wird; vor dem Aufweichen der DLL-Kopierregeln pruefen, was die
  Binaries wirklich importieren (`dumpbin /DEPENDENTS`). Nicht wieder das komplette
  `vcpkg_installed/<triplet>/bin` kopieren.

---

## Testen

### Desktop Client

- CMake Build: `cmake --preset default && cmake --build build/`
- Manueller Test mit angeschlossenem Android-Gerat

### Android

- Gradle Build: `gradle assembleDebug`
- Manueller Test auf physischem Geraet

### Was sich nicht automatisiert testen laesst

Es gibt keine Testsuite. Alles Verhalten am Geraet ist manuell zu pruefen - besonders
Verbindungsaufbau/Reconnect, Tonwiedergabe (falsche Abtastrate faellt sofort als
falsche Tonhoehe auf), Umlauteingabe und die Aufnahme. Kompilieren heisst hier nicht
"funktioniert".

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
