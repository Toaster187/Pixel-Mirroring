# 📱 Pixel Mirroring

> **Dein Android-Bildschirm — nativ auf deinem PC.**  
> Ein Open-Source-Pendant zu Apples iPhone Mirroring für Android/Pixel-Geräte.  
> **Version 5.1.0** — Vollständig implementiert, produktionsreif, aktiv maintained.

---

## ✨ Highlights

- 🎥 **Echtzeitstreaming** – H.264/H.265/AV1-Dekodierung via FFmpeg, GPU-beschleunigtes Rendering
- 🔊 **Tonübertragung** – Der Handy-Ton kommt aus den PC-Lautsprechern, abschaltbar
- 🖱️ **Vollständige Input-Steuerung** – Maus, Tastatur, Touch, Multi-Touch
- ⌨️ **Tastenkürzel** – Zurück, Startbildschirm, App-Übersicht, Sperren/Entsperren
- 📋 **Bidirektionale Clipboard-Synchronisation** – Text einfach kopieren/einfügen
- 📸 **Screenshot & Screen Recording** – Aufnahmen mit Hardware-Encoder, optional direkt aufs Handy
- 📥 **Drag & Drop aufs Handy** – Dateien ins Fenster ziehen, APKs wahlweise direkt installieren
- 🔄 **Auto-Reconnect** – Automatische Wiederverbindung beim Start
- 🎨 **Custom Borderless Window** – Win11 Snap Layouts, Auto-Rotation, Aspect-Ratio Lock
- 🔒 **On-Demand ADB** – ADB WiFi wird nur bei Bedarf aktiviert, nach 60s automatisch deaktiviert
- 🌍 **Mehrsprachig** – Deutsche & englische UI mit vollständiger UTF-8 Umlaute-Unterstützung
- ⚡ **Minimaler Ressourcenverbrauch** – Niedriger RAM, optimierter Akkuverbrauch auf Android

---

## 🚀 Schnellstart

### Windows
1. Laden Sie die neueste Version von [Releases](https://github.com/Toaster187/Pixel-Mirroring/releases) herunter
2. Führen Sie das Installer (`.exe` oder `.msi`) aus
3. Verbinden Sie Ihr Android-Gerät per USB
4. Die App startet automatisch die Einrichtung und installiert die Android-Begleiter-App
5. Nach erfolgreicher Einrichtung: **Automatische Verbindung** beim nächsten Start

### Android App
- **Wird automatisch vom Desktop Client während der Einrichtung installiert**
- Benötigt: Android 11 (API 30) oder höher
- Läuft als Foreground Service mit persistenter Notification

---

## 🏗️ Architektur-Übersicht

Das System besteht aus **zwei Komponenten**, die über das lokale Netzwerk kommunizieren:

```
┌─────────────────────┐         ┌──────────────────────────────┐
│   📱 Android App    │◄───────►│   🖥️ Desktop Client          │
│   (Kotlin/Jetpack)  │  ADB    │   (C++ / Win32 + SDL2)      │
│                     │  TCP/IP │                              │
│  • Background       │         │  • Custom Window (borderless)│
│    Service          │         │  • Aspect-Ratio Lock         │
│  • ADB WiFi Toggle  │         │  • System Tray Integration   │
│  • Ping/Discovery   │         │  • scrcpy Protocol Client    │
│  • Material 3 UI    │         │  • FFmpeg Video Decoder      │
│  • Clipboard Sync   │         │  • Audio-Wiedergabe (SDL2)   │
│  • Capture Helper   │         │  • Input Forwarding          │
│                     │         │  • Capture & Recording       │
└─────────────────────┘         └──────────────────────────────┘
```

Zwischen beiden laufen drei getrennte TCP-Kanäle des scrcpy-Protokolls — in dieser
festen Reihenfolge geöffnet: **Video → Audio → Control**.

---

## 🔄 Verbindungsablauf (Detailliert)

### Phase 1: Ersteinrichtung (Einmalig) — USB-Setup mit Autorisierung

1. **USB-Erkennung**: Benutzer verbindet Android-Gerät per USB, aktiviert USB-Debugging
2. **Auto-Install**: Desktop Client erkennt Gerät via ADB, installiert die Android App automatisch
3. **Permissions**: Client erteilt `WRITE_SECURE_SETTINGS` automatisch via ADB (keine Terminal-Eingabe!)
4. **ADB WiFi**: App aktiviert ADB over WiFi selbstständig via `Settings.Global`
5. **Pairing**: Client & App speichern gegenseitig ihre IPs und `clientId` (vertrauensbasiertes Pairing)
6. ✅ **Setup komplett**: Persistiert unter `%LOCALAPPDATA%\PixelMirroring`
   - ⚠️ **Wichtig**: Nur bei **vollständigem erfolgreichen Setup** gespeichert. Fehlerfall = USB bleibt einziger Weg

### Phase 2: Automatische Verbindung (Ab 2. Start) — On-Demand ADB + 60s Idle-Shutdown

1. **Client-Start**: Desktop Client prüft sofort, ob ein bereits verbundenes TCP-Gerät existiert (Reconnect im Idle-Fenster)
2. **Fallback zu gespeicherter IP**: Falls nicht → POST `/connect` (mit persistierter `clientId`) an die letzte bekannte Geräte-IP
3. **LAN-Scan**: Falls IP nicht antwortet → automatischer Subnet-Scan zur Geräte-Suche
4. **App-Autorisierung**: Android App (`MirroringService`) validiert `clientId` gegen `PairedClientStore`
5. **ADB On-Demand**: App aktiviert `adb_enabled`, `adb_wifi_enabled`, `adb_tcp_port` nur für diese Session
6. **TCP-Verbindung**: Client verbindet via ADB TCP/IP → pusht & startet `scrcpy-server.jar`
7. **Stream-Öffnung**: Video-, Audio- und Control-Sockets öffnen direkt zum scrcpy-Server (ADB Shell nur für Setup)
8. **Keep-Alive**: Client sendet alle ~15s `POST /heartbeat` um Session aktiv zu halten
9. **Auto-Shutdown**: Watchdog auf dem Gerät deaktiviert ADB nach 60s ohne `/connect` oder `/heartbeat` (Sicherheit)

**Sicherheits-Features**:
- ✅ **Manuell aktiviertes Wireless Debugging bleibt unangetastet** – nur vom Client gestartet Sessions werden deaktiviert
- ✅ **Trust-on-first-use (TOFU)** – Neue Clients müssen via USB gepaart werden
- ✅ **60s Idle-Timeout** – Verhindert, dass alte ADB-Sessions im Netzwerk offen bleiben

### Phase 3: Streaming — Live-Mirroring mit Input-Forwarding

1. **Video-Dekodierung**: FFmpeg dekodiert H.264/H.265 Streams in Echtzeit
2. **Rendering**: SDL2 rendert das Video im Custom Borderless Window mit Aspect-Ratio Lock. Die
   dekodierten Bilder werden dabei nicht kopiert, sondern referenziert und direkt von der GPU gelesen
3. **Audio**: Roh-PCM (48 kHz, Stereo) läuft über einen eigenen Socket direkt in die SDL2-Ausgabe
4. **Input-Forwarding**: Maus-, Tastatur- & Touch-Eingaben werden zurück über Control-Socket geschickt
5. **Clipboard-Sync**: Text wird bidirektional zwischen PC & Handy synchronisiert
6. **Capture/Recording**: Screenshots & Screen-Recordings möglich (speichern lokal oder aufs Handy senden)
7. **Dateiübertragung**: Ins Fenster gezogene Dateien wandern in 1-MiB-Häppchen aufs Handy — zwischen den
   Häppchen pausiert der Transfer so lange, wie er gedauert hat, damit der Stream Vorrang behält

---

## 🎨 Features im Detail

### Android App (Kotlin/Jetpack Compose)
| Feature | Beschreibung |
|---------|-------------|
| **Background Service** | Foreground Service mit persistenter Notification, lauscht auf Discovery-Requests |
| **ADB WiFi Toggle** | Aktiviert `adb_wifi_enabled` via `Settings.Global` (benötigt `WRITE_SECURE_SETTINGS`) |
| **Netzwerk-Discovery** | Hand-rolled HTTP/1.1 Server für `/connect`, `/heartbeat`, `/disconnect` Endpoints |
| **Akku-Optimierung** | Minimaler Wakelock, nur aktiv bei eingehendem Request; ADB wird nach 60s deaktiviert. Netzwerkwechsel starten die Discovery-Server entprellt und nur für WLAN neu |
| **Pairing-Verwaltung** | Speichert gekoppelte Client-IDs in `PairedClientStore` (DataStore-Preferences) |
| **Ersteinrichtung UI** | Material 3 Wizard für Setup, Permission-Grant, Fehlerbehandlung |
| **Bidirektionales Clipboard** | Text zwischen PC & Handy automatisch synchronisiert |

### Desktop Client (C++20 / Win32 + SDL2)
| Feature | Beschreibung |
|---------|-------------|
| **Custom Borderless Window** | Win32 `WM_NCCALCSIZE` + `DwmExtendFrameIntoClientArea` für native Schatten |
| **Aspect Ratio Lock** | Video behält immer korrektes Seitenverhältnis, Fenster frei skalierbar |
| **Auto-Rotate** | Fenster dreht sich automatisch bei Handy-Rotation (Portrait ↔ Landscape) |
| **Bildschirmdrehung-Schalter** | Menü-Schalter für die automatische Bildschirmdrehung des Handys; startet synchron zum aktuellen Wert des Handys, wird nie von der App automatisch aktiviert |
| **Win11 Snap Layouts** | Custom Hit-Testing für Drag, Resize, Snap-Buttons (`HTMAXBUTTON`) |
| **System Tray** | Icon in der Taskleiste für schnellen Zugriff |
| **FFmpeg H.264/H.265/AV1** | Video-Dekodierung in Software, Skalierung & Farbraum auf der GPU |
| **Tonübertragung** | Roh-PCM über eigenen Socket, Wiedergabe via SDL2, im Menü abschaltbar |
| **Input Forwarding** | Maus, Tastatur, Multi-Touch über scrcpy Control-Socket |
| **Screenshot/Recording** | Captures als PNG, Videos mit Hardware-H.264 (`h264_mf`), optional aufs Handy senden |
| **Drag & Drop** | Dateien ins Fenster ziehen → `/sdcard/Download`, Pfad landet in der Handy-Zwischenablage; APKs auf Nachfrage direkt installieren. Fortschritt in einer eigenen Bubble, Übertragung gedrosselt zugunsten des Streams |
| **Auto-Reconnect** | Bei Start automatische Verbindung zum letzten bekannten Gerät |
| **Portable Build Mode** | Optional: Konfiguration im App-Ordner statt AppData (Entwicklung/USB-Stick) |

### Netzwerk & Protokolle
- **ADB TCP/IP**: Standardmäßiges Android Debug Bridge Protokoll
- **scrcpy Protocol**: Video, Audio und Control über Raw TCP Sockets, FFmpeg für Dekodierung
- **Custom Discovery**: Hand-rolled HTTP/1.1 Server (keine Framework-Abhängigkeiten)
- **Clipboard**: Polling-basierte Synchronisation zwischen PC & Handy

---

## ⌨️ Tastenkürzel

Alle Kürzel wirken nur, solange der Stream läuft.

| Kürzel | Aktion |
|--------|--------|
| **Alt + B** | Zurück |
| **Alt + H** | Startbildschirm |
| **Alt + S** | App-Übersicht (Task-Manager) |
| **Alt + ↓** | Benachrichtigungen aufziehen |
| **Alt + Umschalt + ↓** | Schnelleinstellungen aufziehen |
| **Alt + ↑** | Aufgezogenes Panel wieder schließen |
| **Strg + U** | Handy entsperren (benötigt gespeicherte PIN) |
| **Strg + L** | Handy sperren & Bildschirm aus |
| **Strg + C** | Zwischenablage vom Handy holen |
| **Strg + V** | Text aus der PC-Zwischenablage aufs Handy einfügen |

Für die Navigation ist bewusst **Alt** statt Strg belegt: Strg+C/V werden für die
Zwischenablage gebraucht, und Strg+U/L für Sperren und Entsperren. `Alt+F4` und
`Alt+Tab` funktionieren wie gewohnt weiter.

---

## 🌑 Handy-Display während der Spiegelung

Standardmäßig dimmt der Client das Handy-Display beim Verbinden auf die niedrigste
Helligkeit (**„Bildschirm auf niedrigste Helligkeit"** im Kontextmenü) und stellt die
ursprüngliche Helligkeit beim Trennen wieder her.

Wer das Display wirklich komplett dunkel haben möchte — weniger Akkuverbrauch, und im
Büro liest niemand mit —, aktiviert zusätzlich **„Handy-Display komplett aus"**. Diese
Option ist **bewusst Opt-in** und standardmäßig aus.

Das Handy bleibt dabei wach und vollständig steuerbar, nur das Panel ist dunkel. Damit
niemand mit einem scheinbar defekten Handy dasteht, wird das Display auf drei Wegen
wieder eingeschaltet:

1. Der Client schickt beim Trennen `SET_SCREEN_POWER_MODE(NORMAL)` über den
   Control-Socket, **bevor** die Sockets abgebaut werden.
2. Der scrcpy-Server hält dafür einen eigenen Aufräum-Prozess auf dem Handy bereit, der
   auch dann greift, wenn der Server abstürzt.
3. Schlägt beides fehl (Socket schon tot, Server mitgerissen), schickt der Client beim
   Beenden über ADB einmal Schlafen + Aufwecken. Android vergibt den Display-Zustand
   dann selbst neu und wirft die erzwungene Dunkelheit weg.

---

## 🔊 Ton

Der Ton des Handys wird standardmäßig mit übertragen und kann im Kontextmenü
(Rechtsklick auf den Ziehgriff) unter **„Ton vom Handy übertragen"** abgeschaltet
werden. Die Änderung greift ab der nächsten Verbindung.

**Technische Hinweise:**
- Übertragen wird **unkomprimiertes PCM** (48 kHz, Stereo, 16 Bit) statt Opus. Das
  spart auf der PC-Seite den kompletten Decoder samt Codec-Konfiguration und kostet
  im lokalen Netz nur rund 1,5 Mbit/s — neben etwa 20 Mbit/s Video vernachlässigbar.
- Benötigt **Android 11 oder neuer** (Voraussetzung der Audio-Aufnahme von scrcpy).
- Kann das Handy keinen Ton aufnehmen, fehlt am PC ein Audiogerät, oder meldet der
  Server einen unerwarteten Codec, **läuft das Bild unverändert weiter** — Ton ist
  nie ein Grund, die Sitzung abzubrechen. Details stehen in `stream.log`.
- Aufgestauter Ton wird ab 250 ms verworfen, damit Bild und Ton nicht auseinanderlaufen.

---

## 📁 Projektstruktur

```
Pixel-Mirroring/
├── README.md                    ← Diese Datei (Benutzer-Dokumentation)
├── CLAUDE.md                    ← Agent-Guidance für Claude (englisch)
├── AGENTS.md                    ← Agent-Guidance für Agents (deutsch)
├── LICENSE                      ← Apache 2.0 Hauptlizenz
│
├── Android/                     ← Android App (Kotlin/Jetpack Compose)
│   ├── app/
│   │   ├── src/main/
│   │   │   ├── java/dev/pixelmirroring/app/
│   │   │   │   ├── MainActivity.kt
│   │   │   │   ├── service/     ← MirroringService, AdbWifiManager, DiscoveryHttpServer
│   │   │   │   ├── data/        ← PairedClientStore (DataStore Persistierung)
│   │   │   │   ├── network/     ← ApiModels, NetworkScanner
│   │   │   │   └── ui/          ← Material 3 Compose Screens
│   │   │   └── AndroidManifest.xml
│   │   ├── proguard-rules.pro   ← R8 Keep-Regeln (Manifest-Klassen, Serializer)
│   │   └── build.gradle.kts     ← Version 5.1, API 35, UTF-8, R8 + Resource-Shrinking
│   └── build.gradle.kts
│
├── Client/                      ← Desktop Client (C++20 / CMake)
│   ├── CMakeLists.txt           ← Build-Config (Version 5.1.0, C++20, UTF-8)
│   ├── src/
│   │   ├── main.cpp             ← Entry Point, State Machine (SETUP→SCANNING→CONNECTED→STREAMING)
│   │   ├── settings.{h,cpp}     ← Persistente Einstellungen (max_fps, max_size, PIN, Ton, etc.)
│   │   ├── adb/                 ← ADB Protocol Client (Subprocess Wrapper, ShellProcess)
│   │   ├── stream/              ← scrcpy Protocol, VideoDecoder, VideoRenderer, AudioPlayer, CaptureController
│   │   ├── input/               ← Input Forwarding (Mouse, Keyboard, Touch)
│   │   ├── network/             ← LAN Discovery (Subnet Scan, cpp-httplib)
│   │   ├── window/              ← Custom Borderless Window (win32_window.cpp)
│   │   └── tray/                ← System Tray (win32_tray.cpp)
│   ├── vendor/platform-tools/   ← Gebündelte adb.exe, AdbWinApi.dll, AdbWinUsbApi.dll
│   └── vcpkg/                   ← Git Submodule: SDL2, FFmpeg, nlohmann-json, cpp-httplib
│
├── scrcpy_download/
│   └── scrcpy-server.jar        ← Offizielle, unmodifizierte scrcpy-Server JAR
│
├── .github/
│   └── workflows/
│       └── release.yml          ← CI: APK Build → Windows Client Build → Installers → GitHub Release
```

---

## 🛠️ Build-Anforderungen & Entwicklung

### Android (Kotlin/Jetpack Compose)

**Anforderungen:**
- Android Studio Ladybug+ oder IDE mit Gradle-Support
- JDK 17+
- Android SDK API 35
- Gradle 9.4.1 (die CI installiert diese Version direkt; die Wrapper-Dateien sind
  bewusst nicht eingecheckt)

**Build:**
```bash
cd Android
gradle assembleDebug   # Debug APK — das ist das Artefakt, das ausgeliefert wird
```

**Wichtig zur Debug-APK:** Sie wird mit **R8 und Resource-Shrinking** gebaut, weil
genau sie vom Desktop-Client installiert und von der CI veröffentlicht wird. Ohne das
wäre sie rund 60 statt 4 MB groß. Keep-Regeln stehen in `app/proguard-rules.pro` —
wer eine Klasse ergänzt, die nur über das Manifest oder per Reflexion erreicht wird,
trägt sie dort nach.

Compose-Previews brauchen das Tooling, das absichtlich nicht in der Auslieferung
steckt. Für die lokale Entwicklung zurückholen mit:
```bash
gradle assembleDebug -PcomposeTooling
```

**Encoding-Hinweis:** `-Dfile.encoding=UTF-8` ist in `gradle.properties` gesetzt; alle Quelldateien müssen UTF-8 sein, um deutsche Umlaute korrekt zu rendern.

### Desktop Client (C++20 / CMake)

**Anforderungen (Windows):**
- Visual Studio 2022+ oder MinGW (MSVC bevorzugt)
- CMake 3.25+
- Windows SDK 10.0.22621+
- Git (für vcpkg Submodule)

**Setup (Einmalig):**
```bash
cd Client
git submodule update --init --recursive  # Lädt vcpkg herunter (~2 GB, nur einmalig)
./vcpkg/bootstrap-vcpkg.bat
```

**Build (Release):**
```bash
cd Client
cmake --preset default -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**Build (Debug):**
```bash
cmake --preset default -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

**Portable Build (Konfiguration im App-Ordner statt AppData):**
```bash
cmake --preset default -DPM_PORTABLE_BUILD=ON
cmake --build build
```

**Packaging (Installers):**
```bash
cd Client/build
cpack -C Release  # Erzeugt ZIP + NSIS + WIX auf Windows
```

**Hinweise:**
- MSVC wird mit `/utf-8` Compiler-Flag gebaut (für deutsche Umlaute in Strings; **niemals entfernen**)
- FFmpeg, SDL2, nlohmann-json, cpp-httplib werden automatisch via vcpkg heruntergeladen und gelinkt
- Das bundled `adb.exe` wird automatisch bei jedem Build neben die EXE kopiert
- Neben die EXE kopiert werden **nur die tatsächlich geladenen** vcpkg-DLLs. Die Liste
  steht als `PM_RUNTIME_DLL_PATTERNS` in `CMakeLists.txt`. Kommt eine neue Bibliothek
  dazu, muss sie dort ergänzt werden — sonst fehlt sie im Installer

---

## 📦 Abhängigkeiten & Lizenzen

### Desktop Client (C++)
- **FFmpeg 6.x+** (LGPL v2.1+) – Video-Dekodierung, Skalierung, MP4-Muxing und
  H.264-Encoding der Aufnahmen (`h264_mf` per Hardware, `libx264` als Rückfall)
- **SDL2 2.28+** (zlib) – Window Management, Rendering & Audio-Ausgabe
- **nlohmann-json** (MIT) – JSON-Konfiguration
- **cpp-httplib** (MIT) – Leichtgewichtiger HTTP-Server (Discovery)
- **Android Platform Tools** (Apache 2.0) – Gebündelte adb.exe

### Android App (Kotlin)
- **Jetpack Compose** (Apache 2.0)
- **Material 3** (Apache 2.0)
- **Kotlin Coroutines** (Apache 2.0)
- **DataStore Preferences** (Apache 2.0)
- **Android Core KTX** (Apache 2.0)

### Lizenzkonformität
Das Projekt ist unter **Apache License 2.0** lizenziert. Alle Abhängigkeiten sind in `LICENSE` dokumentiert mit ihren jeweiligen Lizenztexten und LGPL-Konformitätserklärungen für FFmpeg (Dynamic Linking).

---

## 📋 Status & Roadmap

### ✅ Produktionsreife Features (v5.1.0)

| Komponente | Status | Notizen |
|-----------|--------|---------|
| Android App | ✅ Produktionsreif | Vollständig, Material 3, UTF-8 |
| Desktop Client (Windows) | ✅ Produktionsreif | Custom Window, Snap Layouts, Tray |
| scrcpy Server Integration | ✅ Produktionsreif | H.264/H.265/AV1, FFmpeg Decoder |
| ADB On-Demand + 60s Idle | ✅ Produktionsreif | Sicherheit & Batteriesparen |
| Clipboard Sync (bidirektional) | ✅ Produktionsreif | Text PC ↔ Handy |
| Screenshot & Recording | ✅ Produktionsreif | PNG + H.264, Hardware-Encoder, Send-to-Phone |
| UTF-8 Umlaute (UI & Netzwerk) | ✅ Produktionsreif | Ganz Projekt UTF-8 |
| Input Forwarding | ✅ Produktionsreif | Maus, Tastatur, Multi-Touch |
| Tastenkürzel | ✅ Produktionsreif | Navigation, Sperren/Entsperren, Zwischenablage |
| Auto-Reconnect | ✅ Produktionsreif | Nächster Start verbindet automatisch |
| Tonübertragung | 🆕 Neu | Roh-PCM, abschaltbar; noch wenig Feldpraxis |

### 🎯 Mögliche zukünftige Erweiterungen
- Gamepad-Unterstützung
- Macro Recording & Playback
- Cloud Backup der Einstellungen
- Multi-Device Support (mehrere Geräte gleichzeitig)
- Hardware-**Dekodierung** (siehe „Bewusst nicht umgesetzt")

### 🚫 Bewusst nicht umgesetzt

**Hardware-Dekodierung des Videos** (`d3d11va`/`dxva2`): Die Beschleuniger sind im
gebündelten FFmpeg vorhanden, aber SDL2 kann keine fremde D3D11-Textur übernehmen.
Jedes dekodierte Bild müsste also von der GPU zurück in den Hauptspeicher kopiert
werden — und diese Rückkopie ist teuer genug, dass das Ergebnis bei einem
Handybildschirm realistisch **langsamer** wäre als die jetzige Software-Dekodierung.
Echtes Zero-Copy hieße, SDL2 im Videopfad zu ersetzen. Vorher messen, nicht raten.

---

## 🤝 Contributing & Entwicklung

Pixel Mirroring ist Open-Source und freut sich über Beiträge! Hier sind die wichtigsten Punkte:

### Coding-Konventionen
- **C++**: C++20, alles unter Namespace `pm::`, PascalCase für Klassen, snake_case für Methoden, SCREAMING_SNAKE_CASE für Konstanten
- **Kotlin**: Jetpack Compose, Coroutines, Material 3, Package `dev.pixelmirroring.app.*`
- **Encoding**: Alle Dateien UTF-8 ohne BOM; deutsche Umlaute müssen überall korrekt rendern
- **Kommentare**: Caveman-Stil in Code-Kommentaren (z.B. `// Ugg! ADB nicht gefunden...`); normale Sprache in PRs/Issues

### Branching & PRs
1. Fork das Projekt
2. Feature-Branch: `git checkout -b feature/my-feature`
3. Code-Änderungen mit Tests/Validation
4. PR gegen `main` mit aussagekräftiger Beschreibung
5. Code Review & CI-Checks müssen grün sein

### Build & Testing
```bash
# Desktop Client
cmake --preset default && cmake --build build/

# Android
gradle assembleDebug

# Mit physischem Gerät testen (keine Unit-Tests, nur Integrationstests auf echter Hardware)
```

### Bekannte Einschränkungen & Fallstricke
- **vcpkg Submodule ist groß** (~2 GB) – `git submodule update --init --recursive` dauert
- **MSVC `/utf-8` Flag niemals entfernen** – sonst kaputte Umlaute in der ganzen UI
- **Windows 7 nicht unterstützt** – mindestens Windows 10 (SDK 10.0.22621+)
- **Der Debug-Build wird mit R8 verkleinert** – ungewöhnlich, aber Absicht: die
  Debug-APK ist das Artefakt, das ausgeliefert und vom Client installiert wird.
  Compose-Tooling für lokale Previews holt man sich mit `-PcomposeTooling` zurück
- **Kein Blanko-Kopieren der vcpkg-DLLs** – `CMakeLists.txt` listet die benötigten
  Bibliotheken bewusst einzeln auf. Wieder alles zu kopieren bläht das Paket um
  mehrere MB mit Dateien auf, die nie geladen werden
- **Beenden kann kurz warten** – läuft noch ein Upload einer Aufnahme aufs Handy,
  wird dieser Hintergrund-Thread sauber abgewartet statt abgeschnitten

---

## 🐛 Troubleshooting

### Problem: „ADB nicht gefunden" beim Setup
**Lösung:**
- Stellen Sie sicher, dass USB-Debugging auf dem Handy aktiviert ist
- Gerät sollte im Windows Geräte-Manager oder `adb devices` sichtbar sein
- Treiber für das Gerät installieren (Hersteller-Website)

### Problem: Automatische Verbindung schlägt fehl
**Lösung:**
- Setup über USB wiederholen
- PC und Handy müssen im **gleichen WLAN** sein
- Firewall-Regeln überprüfen (Port 5555 für ADB, Port 8080+ für Discovery)
- Handy neu starten; PC-Client neu starten

### Problem: Eingabe funktioniert nicht (Maus/Tastatur)
**Lösung:**
- ADB-Verbindung überprüfen: `adb shell input text "test"`
- Screen-Mirroring komplett beenden und neu verbinden
- Android-App neu starten

### Problem: Deutsche Umlaute werden falsch angezeigt
**Lösung:**
- Sicherstellen, dass alle Dateien UTF-8 sind (`.editorconfig` setzen)
- MSVC Compiler-Flag `/utf-8` überprüfen
- App neu compilieren mit `cmake --build build/`

### Problem: Framedrops oder Laggy Video
**Lösung:**
- Weniger andere Apps auf dem PC laufen lassen
- Video-Qualität im Settings reduzieren (max_size, max_fps)
- WiFi-Verbindung prüfen (5 GHz besser als 2.4 GHz)

### Problem: Kein Ton
**Lösung:**
- Im Kontextmenü prüfen, ob **„Ton vom Handy übertragen"** aktiviert ist. Die
  Einstellung greift erst ab der **nächsten Verbindung** — einmal neu verbinden
- Am Handy die Medienlautstärke prüfen: übertragen wird die Wiedergabe, nicht das Mikrofon
- Benötigt **Android 11 oder neuer**
- `stream.log` (neben `adb.log` im Konfigurationsordner) sagt, woran es lag —
  z.B. „Phone cannot capture audio" oder „No PC sound device"
- Manche Apps markieren ihre Wiedergabe als nicht mitschneidbar; dann bleibt es
  systemseitig stumm, das ist keine Fehlfunktion des Clients

### Problem: Ton stockt oder läuft dem Bild hinterher
**Lösung:**
- Der Client verwirft aufgestauten Ton ab 250 ms automatisch — kurzes Aussetzen
  nach einem WLAN-Hänger ist daher normal
- Bei dauerhaftem Stottern die WLAN-Verbindung prüfen (5 GHz) oder die
  Videoqualität reduzieren, damit mehr Bandbreite für den Ton bleibt

---

## 📚 Weitere Ressourcen

- **scrcpy Upstream**: https://github.com/Genymobile/scrcpy – Basis des Video-Streaming-Protokolls
- **Android Debug Bridge (ADB)**: https://developer.android.com/studio/command-line/adb
- **Material Design 3**: https://m3.material.io/
- **FFmpeg**: https://ffmpeg.org/
- **SDL2**: https://www.libsdl.org/

---

## 📞 Support & Kontakt

- **Issues & Feature Requests**: https://github.com/Toaster187/Pixel-Mirroring/issues
- **Discussions**: https://github.com/Toaster187/Pixel-Mirroring/discussions
- **Lizenz & Rechtliches**: Siehe [LICENSE](./LICENSE) für Details zu LGPL (FFmpeg), Apache 2.0 und Attributionen

---

## 📜 Lizenz

**Pixel Mirroring** ist unter der **Apache License 2.0** lizenziert — siehe [LICENSE](./LICENSE) für den vollständigen Text.

**Hinweis zu Abhängigkeiten:**
- **FFmpeg** (LGPL v2.1+): Wird dynamisch gelinkt; siehe [LICENSE](./LICENSE) für LGPL-Konformität
- **SDL2** (zlib), **nlohmann-json** (MIT), **cpp-httplib** (MIT): Siehe [LICENSE](./LICENSE)
- **Android Platform Tools** (Apache 2.0): Gebündelt im Windows-Client
- **scrcpy-server** (Apache 2.0): Unmodifizierter offizielle JAR

Eine vollständige Liste aller Abhängigkeiten, ihrer Lizenzen und erforderlichen Lizenztexte findet sich in [LICENSE](./LICENSE).

---

## 🎯 Danksagungen

- **[Genymobile/scrcpy](https://github.com/Genymobile/scrcpy)** – Das Video-Streaming-Protokoll, auf dem dieses Projekt aufbaut
- **Jetpack Compose & Material 3** – Moderne Android UI
- **FFmpeg & SDL2** – Echtzeit-Video-Dekodierung und Cross-Platform Rendering
- **Die Open-Source-Community** – für Feedback, Bugs & Patches
