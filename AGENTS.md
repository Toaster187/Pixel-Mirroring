# AGENTS.md - Pixel Mirroring

## Projektübersicht

Pixel Mirroring ist ein Open-Source Android-Screen-Mirroring-Tool - das Apple iPhone Mirroring für die Android-Welt. Es besteht aus zwei Hauptkomponenten:

### Architektur

```text
Android App (Kotlin/Jetpack Compose)  <--->  ADB/TCP  <--->  Desktop Client (C++/Win32)
         |                                                     |
   Background Service                                 Custom Borderless Window
   ADB WiFi Toggle                                    scrcpy Protocol Client
   Material 3 UI                                      FFmpeg H.264 Decoder
                                                      SDL2 Renderer + Audio
```

Drei getrennte TCP-Kanäle des scrcpy-Protokolls, in genau dieser Reihenfolge
geöffnet: **Video -> Audio -> Control**.

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
|   |-- vendor/platform-tools/  Gebündelte Android Platform Tools für das Desktop-Paket
|   `-- src/
|       |-- main.cpp      Entry Point (WinMain auf Windows, main auf POSIX)
|       |-- settings.*    Persistente Einstellungen inkl. Ton an/aus
|       |-- adb/          ADB Protocol Client + ShellProcess (abbrechbarer adb-shell-Prozess)
|       |-- stream/       scrcpy Protocol, VideoDecoder, VideoRenderer, AudioPlayer, CaptureController
|       |-- input/        Input Forwarding (Mouse, Keyboard, Touch) + HidKeyboard (UHID-Tastatur)
|       |-- network/      Network Discovery (cpp-httplib, Subnet Scan)
|       |-- window/       Plattform-spezifische Fenster (Win32; Cocoa unfertig)
|       `-- tray/         System Tray (Win32)
`-- scrcpy_download/      scrcpy Server Binary
```

**macOS ist derzeit nicht baubar.** `CocoaWindow` implementiert nur einen Bruchteil der
rein virtuellen `IWindow`-Methoden, und die in `CMakeLists.txt` referenzierte
`src/tray/cocoa_tray.mm` existiert gar nicht. Die CI baut ausschließlich Android und
Windows. Eine neue Methode in `IWindow` kann macOS also nicht "kaputt machen" - es ist
bereits kaputt. In nutzerseitiger Doku darf macOS trotzdem nicht als unterstützt
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
sie, die CI veröffentlicht sie. Deshalb laufen auf `debug` ausnahmsweise
`isMinifyEnabled` und `isShrinkResources`; ohne sie ist die APK rund 60 statt 4 MB groß.
Folgen davon:

- Klassen, die nur über das Manifest oder per Reflexion erreicht werden, brauchen eine
  Keep-Regel in `app/proguard-rules.pro`.
- Compose-Tooling steckt absichtlich nicht in der Auslieferung. Lokale Previews:
  `gradle assembleDebug -PcomposeTooling`.
- Bei `debuggable`-Builds macht R8 nur Tree-Shaking, keine Obfuskation - die risikoarme Variante.

### Desktop Client

- Sprache: C++20
- Build: CMake 3.25+ mit vcpkg
- Abhängigkeiten: SDL2, FFmpeg, nlohmann-json, cpp-httplib
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

- Der Windows-Client benutzt sein eigenes gebündeltes `adb.exe`.
- Die Android Platform Tools liegen unter `Client/vendor/platform-tools/`.
- Für Nutzer soll Android Studio nicht notwendig sein.
- Beim Build werden `adb.exe`, `AdbWinApi.dll` und `AdbWinUsbApi.dll` neben die EXE kopiert.

### ADB-Suche

- Bevorzugt wird immer das gebündelte ADB aus dem App-Paket oder dem lokalen Client-Ordner.
- Android-Studio-Installationen oder SDK-Pfade sollen für den normalen Nutzer keine Voraussetzung sein.

### Zustandslogik

- `SETUP`: Erste Einrichtung oder fehlgeschlagene Einrichtung
- `SCANNING`: Automatische Verbindung oder Wiederverbindungsversuch
- `CONNECTED`: ADB steht, Stream wird vorbereitet
- `STREAMING`: Video-Stream läuft

---

## Coding-Konventionen

### C++ (Desktop Client)

- Standard: C++20
- Namespaces: alles unter `pm::`
- Klassen: PascalCase
- Methoden: snake_case
- Konstanten: SCREAMING_SNAKE_CASE
- Member-Variablen: `m_` Prefix
- Ownership: `std::unique_ptr` für Ownership, Raw Pointer nur non-owning
- Error Handling: Return-Werte (`bool`, `std::optional`), keine Exceptions
- Kommentare: Caveman-Sprache

### Kotlin (Android App)

- Architektur: Service-basiert mit Foreground Service
- UI: Jetpack Compose mit Material 3
- Async: Kotlin Coroutines
- Packages: `dev.pixelmirroring.app.*`
- Kommentare: Caveman-Sprache

### Plattformübergreifend

- Interface/Impl Pattern für Fenster und platform-spezifische Teile
- Bedingte Kompilierung über CMake, nicht mit wildem Preprocessor-Gewühl
- Keine hartcodierten Pfade

### Textkodierung / Umlaute (PFLICHT)

Alle deutschen Umlaute (ä ö ü Ä Ö Ü ß) und sonstigen Nicht-ASCII-Zeichen müssen in
jedem Textfeld korrekt dargestellt werden. Damit das dauerhaft hält, gilt:

- **Alle Quelldateien sind UTF-8** (ohne BOM). `.editorconfig` im Repo-Root setzt `charset = utf-8`.
- **MSVC braucht `/utf-8`** (in `Client/CMakeLists.txt` gesetzt). Ohne diesen Schalter liest MSVC
  UTF-8-Dateien als cp1252 - dann werden sowohl `"..."` als auch `L"..."` Literale falsch kodiert
  und die gesamte deutsche UI zeigt Buchstabensalat. Diesen Schalter niemals entfernen.
- **Windows-API immer die `W`-Variante** verwenden (`MessageBoxW`, `DrawTextW`, `CreateFontW`,
  `CreateWindowExW`, `RegisterClassExW`, ...). Nie `...A` mit deutschem Text.
- **Nachrichtenschleifen brauchen `GetMessageW`/`DispatchMessageW`.** `TranslateMessage` erzeugt
  `WM_CHAR` in der Variante dessen, der die Nachricht *abgeholt* hat - nicht in der der
  Fensterklasse. Ein `GetMessage` ohne Suffix (= `...A`) schickt also jeden Tastendruck durch
  cp1252, selbst bei einem Unicode-Fenster. Umlaute überleben das nur zufällig, weil cp1252 sie
  auf dieselben Zahlen wie Unicode legt; alles außerhalb von Latin-1 wird zu `?`. `UNICODE` ist
  in `Client/CMakeLists.txt` bewusst **nicht** definiert - jeder suffixlose Win32-Aufruf ist die
  ANSI-Variante. Immer explizit schreiben.
- Zwischen `std::string` (immer UTF-8) und `std::wstring` nur über `MultiByteToWideChar` /
  `WideCharToMultiByte` mit `CP_UTF8` konvertieren - nie `CP_ACP`. Die vier Umwandlungen
  stehen in `Client/src/util/encoding.{h,cpp}` (`pm::util::path_from_utf8`,
  `path_to_utf8`, `utf8_to_wide`, `wide_to_utf8`) - diese benutzen, keine fünfte Kopie
  von Hand schreiben.
- **`std::filesystem::path::string()` unter Windows nie aufrufen.** Es liefert die
  ANSI-Codepage, in der Kyrillisch, CJK und Emoji zu `?` werden. Stattdessen
  `pm::util::path_to_utf8()`; der Weg zurück ist `path_from_utf8()`, nie
  `std::filesystem::path(narrow_string)`.
- **Jeder `std::string`, der ein Modul verlässt, ist UTF-8** - auch jeder Pfad, der nach
  `pm::adb` geht. `AdbClient` startet adb mit `CreateProcessW`, `get_executable_dir()`
  benutzt `GetModuleFileNameW`; damit spricht die ganze Kette PC -> adb -> Handy
  dieselbe Tabelle. FFmpeg (`avio_open`, `avformat_alloc_output_context2`) erwartet
  unter Windows ebenfalls UTF-8.
- GDI+ zeichnet nur `wchar_t`: narrow Strings vorher nach UTF-16 konvertieren.
- **Android**: `-Dfile.encoding=UTF-8` in `gradle.properties`, `compileOptions.encoding = "UTF-8"`
  und `options.encoding = "UTF-8"` für alle `JavaCompile`-Tasks.
- **Netzwerk**: HTTP-Bodies immer explizit mit `StandardCharsets.UTF_8` lesen/schreiben und
  `charset=utf-8` im Content-Type mitschicken.
- Umlaute nicht durch `ue`/`oe`/`ae` ersetzen, um ein Encoding-Problem zu umgehen - die Ursache fixen.

---

## Wichtige Architektur-Regeln

1. Kein Browser-Technologie. Kein Electron, kein WebView, kein CEF.
2. Aspect Ratio immer beibehalten.
3. Windows: Custom Borderless Window mit eigenem Hit-Testing.
4. scrcpy-Protokoll nutzen, keine eigene Streaming-Lösung erfinden.
5. Android App aktiviert ADB over WiFi selbst via `Settings.Global.putInt("adb_wifi_enabled", 1)`.
6. Minimaler Akkuverbrauch auf Android. Konkret: keine Poll-Schleifen, die pro Runde
   einen `adb`-Prozess starten, und Netzwerk-Callbacks entprellt und auf WLAN begrenzt.
7. Der Client soll neue Nutzer sauber durch USB-Ersteinrichtung führen und danach automatisch verbinden.

### Invarianten im Stream-Pfad (nicht aus Versehen zurückdrehen)

- **Socket-Reihenfolge liegt fest:** Video -> Audio -> Control, exakt so in
  `connect_sockets()` und passend zur Server-Seite. Audio nur, wenn aktiviert.
  Falsche Reihenfolge = jeder Stream liest aus dem falschen Kanal.
- **Ton läuft bewusst als rohes PCM** (`audio_codec=raw`, 48 kHz, Stereo, s16): kein
  Decoder, keine Codec-Konfiguration, keine zusätzliche FFmpeg-Abhängigkeit auf der
  Client-Seite. Kostet ~1,5 Mbit/s neben ~20 Mbit/s Video. Nicht ohne Grund auf Opus
  "optimieren" - die Config-Pakete sind genau der Teil, der still kaputtgeht.
- **Ton darf das Bild nie mitreißen:** jeder Fehlerfall (Handy kann nicht aufnehmen,
  Codec-Kennung 0/-1, kein PC-Audiogerät) landet in `stream.log` und läuft video-only
  weiter. Ton ausschalten stellt exakt die alte Server-Befehlszeile wieder her.
- **Socket-Abbau:** `stop()` macht `shutdown()` -> `join()` -> `closesocket()`. Zuerst
  schließen würde einen lesenden Thread auf einer Socket-Nummer sitzen lassen, die das
  Betriebssystem schon neu vergeben hat. Schreibzugriffe auf den Control-Socket laufen
  über `send_control()` (Mutex + vollständige Schreibschleife), weil UI-Thread und
  Screen-Poll-Thread beide senden.
- **Der Serverprozess gehört uns:** das `adb shell app_process ...` läuft in einem
  `pm::adb::ShellProcess` und wird in `stop()` beendet. Kein detached Thread mehr - das
  hat pro Sitzung einen Thread und eine `adb.exe` liegengelassen.
- **Der Renderer hält eine Referenz, keine Kopie** (`av_frame_ref`). Kein
  Pro-Bild-`memcpy` der YUV-Ebenen wieder einbauen (~180 MB/s bei 1080p60).
- **Aufnahmen bevorzugen den Hardware-Encoder** (`h264_mf`, sonst NVENC/QSV/AMF), mit
  automatischem Rückfall auf `libx264` und passendem Pixelformat.
- **Tastenkürzel:** Navigation liegt auf **Alt** (`Alt+B` zurück, `Alt+H` Start,
  `Alt+S` Übersicht, `Alt+↓` Benachrichtigungen, `Alt+Umschalt+↓` Schnelleinstellungen,
  `Alt+↑` schließt wieder), weil Strg+C/V die Zwischenablage und Strg+U/L
  Sperren/Entsperren belegen. Alt-Tasten kommen als `WM_SYSKEYDOWN`/`WM_SYSKEYUP`; nur
  B/H/S und die Pfeiltasten hoch/runter werden abgefangen, damit `Alt+F4` weiter bei
  `DefWindowProc` ankommt. B/H/S sind Android-Keycodes und laufen über
  `set_key_callback`; die Pfeiltasten sind scrcpy-Control-Messages 5/6/7 und laufen
  deshalb über den `MenuAction`-Weg, genau wie Strg+U/L. Bei `Alt+↑`/`Alt+↓` müssen
  Druck **und** jede Wiederholung geschluckt werden — ein einzelnes Alt, das bis
  `DefWindowProc` durchkommt, lässt Windows nach einer Menüleiste piepsen, die es
  nicht gibt.
- **Display komplett aus ist Opt-in und muss IMMER rückgängig gemacht werden:**
  `Settings::m_screen_off` (Standard aus) schickt Control-Message 10
  `SET_SCREEN_POWER_MODE(OFF)`, sobald der Stream steht - die ehrliche Version von
  `m_lowest_brightness`, das nur per ADB dimmt. Das Handy bleibt wach und steuerbar,
  nur das Panel ist dunkel; `PowerManager.isInteractive` bleibt deshalb true und der
  Screen-Poll-Wächter schlägt nicht fälschlich an. Zurückgestellt wird auf drei
  Wegen, keiner davon ist überflüssig: (1) `ScrcpyClient::stop()` schickt `NORMAL`
  *bevor* die Sockets abgebaut werden, (2) der scrcpy-Server hält auf dem Handy einen
  eigenen CleanUp-Prozess, der auch beim Serverabsturz greift, (3) ist
  `screen_forced_off()` beim Beenden noch true, schickt `main.cpp` per ADB
  `input keyevent 223; sleep 0.5; input keyevent 224`. Der Schlaf-Weck-Doppelschlag in
  (3) ist nötig: ein bloßes Wecken tut nichts, weil Android einen Display-Power-Mode
  nur dann neu setzt, wenn sich sein *eigener* Bildschirmzustand ändert - und der ist
  aus seiner Sicht schon an. `screen_forced_off_` wird in `start()` absichtlich **nicht**
  zurückgesetzt: ein Panel, das eine Sitzung mit totem Socket dunkel gelassen hat, muss
  von der nächsten noch repariert werden können.
- **Dateiübertragung ist absichtlich langsam:** ins Fenster gezogene Dateien gehen
  über `AdbClient::push_file_paced` in 1-MiB-Stücken raus, und nach jedem Stück wartet
  der Transfer genau so lange, wie das Stück gedauert hat. Damit gehört dem Stream
  mindestens die Hälfte der Funkzeit, unabhängig davon, wie schnell das WLAN heute ist.
  Nicht auf ein einzelnes `adb push` "optimieren" - dann ruckelt das Bild während der
  Übertragung. Die Stücke wachsen unter `<ziel>.pmpart` und bekommen erst per `mv` den
  echten Namen, damit eine abgebrochene Übertragung keine vorhandene Datei zerstört.
  Eine abgelegte APK, die installiert werden soll, wandert nach `/data/local/tmp` und
  wird von dort per `pm install` entpackt - `adb install` würde sie ein zweites Mal
  ungebremst übertragen, und der Paketinstallierer kommt nicht an `/sdcard` heran.
  `install_pushed_app()` lässt das `-g` von `install_app()` bewusst weg: bei der
  eigenen APK, die der Mensch absichtlich einrichtet, sind vorab erteilte
  Berechtigungen vertretbar - eine ins Fenster gezogene Datei ist ein Fremder und
  muss Kamera, Mikrofon und Standort genauso beim Handy erfragen wie jede andere App.
  Liegengebliebene `.pmpart`/`.pmglue` werden vor dem ersten Stück per `rm -f`
  weggeräumt: das Aufräumen am Ende von `push_file_paced` läuft nur, wenn die
  Übertragung *endet* - ein hart beendeter Client lässt sie sonst für immer im
  Dateimanager des Handys liegen.
- **Auto-Pause bei minimiertem Fenster** (`Settings::m_auto_pause_minimized`,
  standardmäßig **an**): ein zur Taskleiste gefaltetes Fenster zeigt nichts, das Handy
  kodiert aber weiter und das WLAN trägt weiter ~20 Mbit/s. `Win32Window` meldet über
  `set_minimize_callback` nur die Flanke `SIZE_MINIMIZED`/`SIZE_RESTORED` (`WM_SIZE`
  feuert auch bei jedem Ziehen am Fensterrand), `main.cpp` gleicht daraufhin ab:
  - **Der Heartbeat muss den Stream überleben.** `POST /heartbeat` verhindert, dass der
    Watchdog am Handy ADB nach 60 s abschaltet - genau deshalb ist der Rückweg warm:
    Fortsetzen ist nur noch ein weiteres `start_stream()`, ohne Suche, ohne `/connect`,
    ohne Pairing. Die Heartbeat-Schleife läuft daher, solange
    `scrcpy.is_running() || auto_paused` gilt, und `auto_paused` wird **vor**
    `scrcpy.stop()` gesetzt; in der anderen Reihenfolge gibt es einen Moment, in dem
    beides falsch ist und der Heartbeat-Thread endgültig aussteigt.
  - **Der Fensterthread schreibt nur einen Wunsch auf.** `pause_wanted` ist ein Atomic,
    ein einzelner Hintergrund-Task (`reconcile_pause_state`) gleicht die Welt daran an.
    Einen Stream im Fensterthread abzubauen oder aufzubauen würde die Oberfläche
    einfrieren.
  - **Pausiert wird übersprungen, nie erzwungen.** Nicht während einer Aufnahme (ein
    abgerissener Stream hinterlässt eine kaputte Datei) und nicht während
    `apply_quality_now` (das hält den Stream bereits in beiden Händen). Wird das
    Pausieren übersprungen, bleibt `auto_paused` unten - dann tut auch das
    Wiederherstellen nichts. Ein noch laufender Verbindungsaufbau steht bewusst nicht in
    dieser Liste: er meldet `is_running()` erst, wenn der Stream wirklich steht, und
    stößt den Abgleich am Ende seines eigenen Weges noch einmal an.
  - **Beim Fortsetzen gibt es den kalten Weg als Rückfall.** Scheitert `start_stream()`
    beim Wiederherstellen - Handy eingeschlafen, WLAN gewechselt, ADB doch abgeschaltet -
    ruft der Task `start_connection(true)` auf, statt ein totes Fenster stehen zu lassen.
- **Optionale UHID-Tastatur** (`Settings::m_uhid_keyboard`, standardmäßig aus): statt
  `inject_text` hängt eine virtuelle USB-HID-Tastatur am Handy (scrcpy-Control-Messages
  12 `UHID_CREATE`, 13 `UHID_INPUT`, 14 `UHID_DESTROY` sowie 15
  `OPEN_HARD_KEYBOARD_SETTINGS`). Was dabei nicht kaputtgehen darf:
  - **Erst fragen, dann bauen.** `ScrcpyClient::device_supports_uhid()` prüft `/dev/uhid`
    per ADB. Wirft `UhidManager.open()` auf der Handy-Seite, fliegt die Exception aus
    `handleEvent()` und reißt den **kompletten Control-Thread** des Servers mit - Maus,
    Touch, Tastatur und Zwischenablage sind still, während das Bild weiterläuft.
    `UHID_CREATE` niemals ungeprüft senden. Die Probe öffnet **lesend und schreibend**
    (`(exec 3<>/dev/uhid)`), weil der Server die UHID-Output-Reports aus demselben
    Deskriptor zurückliest - eine reine Schreibprobe käme auf einem Gerät durch, das
    Lesen verweigert, und würde genau den Thread reißen, den sie schützen soll. Die
    Subshell ist nötig, weil ein fehlgeschlagenes `exec` die Shell beendet, in der es
    steht. Das Ergebnis wird pro `device_id` gemerkt, damit Neuverbindungen und
    Qualitäts-Neustarts keinen weiteren `adb`-Prozess kosten.
  - **Das Wire-Format hängt an der Server-Version.** Der gebündelte Server 2.7 erwartet
    `type(1) id(2 BE) name_len(1) name rd_size(2 BE) rd_data`; das `name`-Feld gibt es vor
    2.7 nicht und spätere scrcpy-Versionen ergänzen Vendor-/Product-IDs. Vor Änderungen
    gegen die mitgelieferte JAR prüfen.
  - **Pro Taste darf genau ein Pfad feuern.** `Win32Window::uhid_keyboard_` schaltet um:
    ist er an, gehen `WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` an
    `send_raw_key()` und `WM_CHAR` wird verworfen; ist er aus, bleibt alles wie vorher.
    Lautstärketasten und die Alt-/Strg-Kürzel sind die bewussten Ausnahmen.
  - **Eine abgelehnte Taste muss zurückfallen, nicht verschwinden.** Der Rückgabewert
    reicht von `HidKeyboard::process_key()` über `InputHandler::handle_raw_key()` bis
    `Win32Window::send_raw_key()` durch; `false` heißt "die Boot-Tastatur hat diese
    Taste nicht", und das Fenster schickt sie dann über den Android-Keycode-Pfad.
    `send_raw_key()` merkt sich in `hid_held_keys_`, welche Scancodes die HID-Tastatur
    genommen hat - allein damit auch die Auto-Repeats einer *abgelehnten* Taste weiter
    durchfallen. Bei einer Taste, deren Druck ein Kürzel (Strg+U/L) geschluckt hat,
    wird auch das Loslassen geschluckt (`swallowed_shortcuts_`), sonst bekommt das
    Handy ein Release für eine Taste, die es nie gedrückt gesehen hat.
  - **Scancodes statt Virtual Keys.** Der ganze Sinn ist, dass das *Handy* das Zeichen
    bestimmt - also wird die physische Tastenposition aus `lParam` geschickt, nie `wParam`.
  - **AltGr** erzeugt unter Windows immer zusätzlich ein linkes Strg;
    `HidKeyboard::build_report()` filtert es heraus, solange rechtes Alt gehalten wird.
    Ohne das kommt `AltGr+Q` als `Strg+AltGr+Q` an.
  - Tastenwiederholung macht das Handy selbst (so wie bei einer echten Tastatur) - der
    Client schickt jeden Druck genau einmal.
- **Hardware-Dekodierung fehlt mit Absicht:** `d3d11va`/`dxva2` sind vorhanden, aber
  SDL2 kann keine fremde D3D11-Textur übernehmen. Die nötige GPU->CPU-Rückkopie
  wäre vermutlich langsamer als die jetzige Software-Dekodierung. Erst messen.
- **Artefaktgröße ist ein Feature:** vor einer neuen Android-Abhängigkeit prüfen, ob
  sie überhaupt benutzt wird; vor dem Aufweichen der DLL-Kopierregeln prüfen, was die
  Binaries wirklich importieren (`dumpbin /DEPENDENTS`). Nicht wieder das komplette
  `vcpkg_installed/<triplet>/bin` kopieren.

---

## Testen

### Desktop Client

- CMake Build: `cmake --preset default && cmake --build build/`
- Selbsttest: `ctest --test-dir build --output-on-failure`
- Manueller Test mit angeschlossenem Android-Gerät

### Android

- Gradle Build: `gradle assembleDebug`
- Manueller Test auf physischem Gerät

### CI

`.github/workflows/build.yml` übersetzt bei jedem Pull Request Android und den
Windows-Client mit MSVC (gleicher Compiler, gleiches `/utf-8` wie die Auslieferung)
und lässt `ctest` laufen - ohne Packaging. `release.yml` baut zusätzlich die
Installer und nur auf `v*`-Tags.

### Der einzige automatische Test

`Client/tests/hid_keyboard_test.cpp` - ein eigenständiges `main()` mit
handgeschriebenem `check()` (bewusst **kein** `assert`: die Auslieferung ist ein
Release-Build mit `NDEBUG`, dort wäre jede Zusicherung wegoptimiert). Er nagelt die
Scancode-Tabelle, die AltGr-Phantom-Strg-Maske, Extended-vs-Numpad-Paare und den
6-Tasten-Rollover fest. Ziel `pm_hid_keyboard_test` (Option `PM_BUILD_TESTS`,
standardmäßig an), hängt nur an `hid_keyboard.cpp` und wird weder installiert noch
paketiert. Kein Test-Framework hinzufügen - für weitere Absicherungen dasselbe
Muster benutzen.

### Was sich nicht automatisiert testen lässt

Alles andere. Verhalten am Gerät ist manuell zu prüfen - besonders
Verbindungsaufbau/Reconnect, Tonwiedergabe (falsche Abtastrate fällt sofort als
falsche Tonhöhe auf), Umlauteingabe und die Aufnahme. Kompilieren heißt hier nicht
"funktioniert".

---

## Was der Agent nicht tun soll

- Keine neuen Frameworks einführen
- Keine Sprache wechseln
- Nicht den offiziellen scrcpy-Server modifizieren
- Keine neuen Build-Systeme einführen
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
