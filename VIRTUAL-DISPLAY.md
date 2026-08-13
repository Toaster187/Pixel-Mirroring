# Virtual Display (experimentell)

Status: **Experiment auf `virtual-display-dev-`.**

Statt den Handybildschirm zu spiegeln, legt der scrcpy-Server auf dem Handy ein eigenes
virtuelles Display an, das nur für diesen PC existiert. Der PC bekommt eine eigene
Android-Oberfläche, das Handy-Panel bleibt dunkel.

Der Modus ist **standardmäßig an** (`Settings::m_virtual_display`) und lässt sich per
Rechtsklick ins Fenster abschalten → „Eigener PC-Bildschirm (experimentell)". Umschalten
startet den Stream neu, weil der Server seine Optionen nur beim Start liest.

## Was der Modus mitbringt

| Einstellung | Schlüssel in `settings.txt` | Standard |
|---|---|---|
| Modus an/aus | `virtual_display` | `1` |
| App auf dem Display | `virtual_display_app` | leer → Fossify Launcher |

Beide Angaben gehen als Serveroptionen raus: `new_display=` (leer = Größe und
Punktdichte des Handy-Displays), `display_ime_policy=local`, `keep_active=true`,
`screen_off_timeout=3600000` und — sobald eine App benannt ist —
`vd_system_decorations=false` plus Steuernachricht 16 (`START_APP`).

`virtual_display_app` ist reine Handarbeit: der Client **schreibt nie** hinein. Bleibt es
leer, sucht er sich pro Sitzung den Fossify Launcher, installiert ihn notfalls und merkt
sich das nur in der Sitzungs-`Config` (`ScrcpyClient::new_display_app()`). Ein
zurückgeschriebener Wert würde ohnehin nur bis zum nächsten Menüklick überleben, weil
`main.cpp` eine langlebige `Settings`-Kopie hält und die ganze Datei neu schreibt.

**Der Modus allein macht das Handy nicht dunkel.** Das Abschalten des Panels hängt am
unabhängigen Schalter `screen_off` (Standard `0`, Menüpunkt „Handy-Display komplett
aus"). Ohne ihn bekommt der PC sein eigenes Display, während das Handy weiter leuchtet.
Das Menü weist darauf hin, solange nur der eine der beiden Schalter an ist.

**Ohne Launcher keine Sitzung in diesem Modus:** lässt sich Fossify nicht installieren
und ist auch von Hand keine App benannt, fällt `start_stream` auf die gewöhnliche
Spiegelung zurück und schreibt das in die Statuszeile. Die Alternative wäre, die
Systemdekorationen anzulassen und dem Zweitdisplay-Launcher des Handys das Display zu
überlassen — also genau das Bild, das zeichnet, aber nichts starten kann.

## Voraussetzung: Server-JAR 4.1

`Client/scrcpy-server.jar` und `scrcpy_download/scrcpy-server.jar` sind das
unveränderte Release-Artefakt `scrcpy-server-v4.1` von
<https://github.com/Genymobile/scrcpy/releases/tag/v4.1> (733.706 Bytes,
SHA-256 `deacb991ed2509715160ffdc7907e47b4160eb30d1566217e9047fd5b8850cae`).
Vorher lag dort v2.7 (71.200 Bytes).

Das Issue nannte 3.x als Ziel, und der Weg ging auch zuerst dorthin. 3.3.4 fehlt aber
`keep_active`, weshalb hier 4.1 steht. Beide Sprünge sind unten aufgeschrieben — der
Schritt 2.7 → 3.3.4 betrifft den Steuerkanal, der Schritt 3.3.4 → 4.1 den Videostrom.

Die Versionszeichenkette in `scrcpy_client.cpp` (`SERVER_VERSION`) muss exakt zur JAR
passen — der Server vergleicht sie mit seiner eigenen und startet sonst gar nicht.

## Der Launcher: warum ein fremder

Ein Pixel 9 unter Android 17 legt seinen eigenen Launcher für Zweitdisplays
(`SecondaryDisplayLauncher`) sehr wohl auf das neue Display. Der ist dort aber
unbrauchbar:

* Die App-Übersicht öffnet sich und listet alle Apps.
* **Ein Tippen auf einen App-Eintrag startet nichts.** Kein Logeintrag, keine Ablehnung,
  die Übersicht schließt sich nur wieder. Mehrfach reproduziert, auf die exakten
  anklickbaren Knotengrenzen gezielt, mit Touch- und mit Maus-Ereignissen.
* An der Eingabe liegt es nicht: `dumpsys input` meldet unser Display als
  `FocusedDisplayId`, das Launcher-Fenster als fokussiert, Zustellung `result='OK'`.
  Eine per `START_APP` gestartete App reagiert auf demselben Display einwandfrei auf
  Fingereingaben.

Deshalb bringt der Client **Fossify Launcher** mit (quelloffen, F-Droid). Er wird beim
Bauen geladen (`Client/CMakeLists.txt`, mit Prüfsummenkontrolle), liegt neben der EXE
und im Installationspaket, und der Client schiebt ihn bei der Ersteinrichtung über USB
aufs Handy — später notfalls auch über WLAN. Aus Fossify heraus lassen sich Apps
nachweislich starten (Chrome, verifiziert).

Installiert wird **ohne `-g`**, über `push_file` + `install_pushed_app`: das
Vorab-Erteilen aller Laufzeitberechtigungen ist für die eigene App vertretbar, für einen
Launcher aus einem fremden Repository nicht.

Beim allerersten Start fragt Fossify, ob es Standard-Launcher des ganzen Handys werden
soll. Der Client schickt direkt nach einer frischen Installation einmal `BACK`, was dem
„Abbrechen" entspricht — nur dann, nicht in jeder Sitzung.

### Lizenz

Fossify Launcher steht unter **GPL-3.0-only**. Mitgeliefert wird eine unveränderte APK,
also gelten die Pflichten aus Abschnitt 4 und 6 der Lizenz. Erfüllt sind sie so:

* `Client/vendor/licenses/GPL-3.0.txt` — der vollständige Lizenztext.
* `Client/vendor/licenses/FossifyLauncher.md` — Version, Prüfsumme und das
  Quellenangebot nach Abschnitt 6d, samt des genauen Upstream-Commits, aus dem F-Droid
  diese Binärdatei gebaut hat.
* `CMakeLists.txt` legt beide Dateien neben die EXE **und** ins Installationspaket. Sie
  müssen überall mitreisen, wo die APK mitreist — auch in einem selbst gebauten ZIP.

Die APK ist ein eigenständiges Programm auf einem anderen Gerät, nicht gelinkt und nicht
mit dem Client zu einem Werk verbunden: eine bloße Zusammenstellung nach Abschnitt 5. Die
Lizenz von Pixel Mirroring bleibt davon unberührt.

Die Prüfsumme wird in CMake **von Hand** verglichen, nicht über
`file(DOWNLOAD ... EXPECTED_HASH)`. Das bricht bei Abweichung den ganzen Configure-Lauf
ab, erreicht den `STATUS`-Zweig nie und lässt die schlechte Datei liegen — der nächste
Lauf findet sie über `EXISTS`, überspringt jede Prüfung und liefert sie ungeprüft aus.

**Offen:** die APK vergrößert das Installationspaket um gut 5 MB, was dem Grundsatz „die
Größe der Auslieferung ist ein Feature" zuwiderläuft.

## Schlaf, Sperre, Panel

Drei Zustände des Handys wirken auf das virtuelle Display, und sie werden gern
verwechselt:

* **Panel aus** (`Settings::m_screen_off`, `SET_DISPLAY_POWER`): geht über SurfaceControl
  an Androids Buchhaltung vorbei. Das Gerät bleibt wach, das virtuelle Display lebt, der
  Handybildschirm ist schwarz. **Das ist der gewollte Zustand.** `dumpsys` meldet das
  Panel dabei weiter als `ON` — der Zustand lässt sich von außen nicht messen.
* **Gerät schläft** (Seitentaste oder Zeitüberschreitung): WindowManager hängt auch an
  unser Display ein `Display-off`-Token, der Launcher hört auf zu zeichnen, jede Eingabe
  verpufft. Nachgemessen:

  ```
  Going to sleep due to power_button
  Add SleepToken: tag=Display-off, displayId=0
  Add SleepToken: tag=Display-off, displayId=17   <- unseres, 200 ms später
  ```

  `keep_active` allein rettet das nicht: der Server weckt damit alle 4 Sekunden über
  `PowerManager.userActivity()`, das ergibt einen Sägezahn, und nach 30 Sekunden ohne
  Eingabe steht das Display auf `isSleeping=true`.

  Weil das Abschalten des Panels Androids Untätigkeitsuhr **nicht** anhält (es glaubt
  weiter, sein Bildschirm sei an), schlief das Gerät nach 60 Sekunden von selbst ein.
  Dagegen setzt der Client `screen_off_timeout=3600000`; der Server stellt den alten
  Wert über seinen eigenen Aufräumprozess zurück, auch nach einem Absturz.
* **Gerät gesperrt:** `FLAG_ALWAYS_UNLOCKED` sorgt dafür, dass auf unserem Display kein
  Sperrbildschirm erscheint. Ein automatisches Entsperren gab es hier zwischenzeitlich,
  es ist wieder entfernt.

  `unlock_device_if_needed` fragt dafür die **Sitzung**, nicht
  `Settings::m_virtual_display`: die Einstellung sagt nur, was gewollt war, und
  `start_stream` fällt ohne Launcher auf gewöhnliche Spiegelung zurück. Weil das
  Entsparren parallel zu `start_stream` läuft, kennt es das Ergebnis noch nicht — es
  bekommt die Erwartung übergeben, und beide Verbindungswege rufen es ein zweites Mal,
  wenn die Sitzung anders ausgegangen ist. Sonst spiegelt der PC einen Sperrbildschirm,
  den niemand entsperrt.

**Geweckt wird trotzdem, entsperrt nicht.** Ein dösendes Gerät nimmt das virtuelle
Display mit in den Schlaf, das Fenster bleibt dann schwarz. `start_stream` schickt in
diesem Modus deshalb einmal `KEYCODE_WAKEUP`, bevor das Display gebaut wird — das holt
das Handy an seinen Sperrbildschirm und keinen Schritt weiter, es wird keine PIN
getippt. Wachhalten übernimmt danach `screen_off_timeout`, das Abdunkeln `m_screen_off`.
Endzustand: ein dunkles, gesperrtes, waches Handy. Nachgemessen vom ausgeschalteten
Handy aus: Sitzung kommt hoch, 55–60 fps bei 14–17 Mbit/s, 80 s durchgehend `Awake`,
`isKeyguardShowing=true`, Fossify mit `visibleRequested=true`.

**Reihenfolge ist wichtig:** die Helligkeitsabsenkung läuft in diesem Modus *vor* dem
Stream. Das Schreiben von `screen_brightness` lässt Android den Displayzustand neu setzen
und schaltet das gerade abgedunkelte Panel wieder ein; als Hintergrundaufgabe landete das
mal vor, mal nach dem Abschalten.

**Handy aus = Sitzung aus:** der Bildschirm-Watchdog ist in diesem Modus aktiv. Er
verwechselt das nicht mit dem Panel, das der Client selbst abdunkelt, weil das Gerät
dabei interaktiv bleibt. Das Fenster faltet sich dann in die Taskleiste statt in die
Ablage — beim gewöhnlichen Spiegeln bleibt es beim Verstecken in die Ablage.

## Protokoll-Deltas 2.7 → 3.3.4 (Steuerkanal)

Alles aus einem Quelltextvergleich der beiden Tags, nicht aus dem Änderungsprotokoll.

| Typ | 2.7 | 3.3.4 | Auswirkung auf den Client |
|----|----|----|----|
| 10 | `SET_SCREEN_POWER_MODE`, Nutzlast = Power-Mode-Zahl (0 aus, 2 normal) | `SET_DISPLAY_POWER`, Nutzlast = Boolean | Angepasst: sendet 1 statt 2. Die alte 2 wäre über `readBoolean()` zufällig auch „an" gewesen. |
| 3 | `INJECT_SCROLL_EVENT`, Festkommawert im Bereich [-1, 1] | derselbe Wert, wird serverseitig **mal 16** genommen | Angepasst: Client skaliert mit 8192/16. Ohne das scrollt jede Radrastung 16-fach. |
| 12 | `UHID_CREATE`: `typ(1) id(2) name_len(1) name rd_size(2) rd_data` | `typ(1) id(2) vendor(2) product(2) name_len(1) name rd_size(2) rd_data` | Angepasst: 4 Bytes eingefügt, beide 0 (wie scrcpy es für Tastatur und Maus tut; nur Gamepads tragen echte IDs). Ohne das liest der Server die Namenslänge mitten aus der Report-Beschreibung und reißt den kompletten Control-Thread mit — Tasten, Maus, Touch und Zwischenablage still, Bild läuft weiter. |
| 16 | – | `START_APP`, `typ(1) len(1) name` | Implementiert, wird für den Launcher gebraucht. |
| 17 | – | `RESET_VIDEO` | Nicht implementiert. |

Alle übrigen Typen (0–9, 11, 13, 14, 15) sind unverändert. Die Geräte-Nachrichten
Handy → PC (`CLIPBOARD` 0, `ACK_CLIPBOARD` 1, `UHID_OUTPUT` 2) sind byteweise identisch.

Entfallene Option: `lock_video_orientation` (ersetzt durch `capture_orientation`) — der
Client hat sie nie gesendet. Unbekannte Optionen sind ab 3.x kein Startfehler mehr, der
Server warnt nur; ein Tippfehler im Schlüssel fällt also nicht mehr sofort auf.

## Protokoll-Deltas 3.3.4 → 4.1 (Videostrom)

Hier sitzt die Änderung nicht im Steuerkanal. Wer nur die Versionszeichenkette hochzieht,
bekommt ein Bild aus Müll.

* **Der Videokopf schrumpft von 12 auf 4 Bytes** — nur noch die Codec-ID.
* **Die Bildgröße kommt im Strom**, als „Session-Paket": zwölf Bytes aus Flags, Breite,
  Höhe, erkennbar am gesetzten obersten Bit. Einmal vor dem ersten Bild, danach erneut
  nach jeder Drehung oder Größenänderung. Ein Session-Paket ist die ganze Nachricht, es
  folgt keine Nutzlast.
* **Die Paketflags sind ein Bit nach unten gerutscht:** `SESSION` neu auf Bit 63,
  `CONFIG` von 63 auf 62, `KEY_FRAME` von 62 auf 61.
* `send_codec_meta` heißt jetzt `send_stream_meta` und deckt beides ab.
* Neue Steuernachrichten 18–22 (Kamera-Zoom/Blitz, `RESIZE_DISPLAY`, `SCAN_FILE`) —
  additiv, keine wird benutzt.
* `UHID_CREATE`, Scroll-Skalierung und `SET_DISPLAY_POWER` sind gegenüber 3.3.4
  unverändert.

Der Client liest das Session-Paket in `read_metadata()` (Anfangsgröße) und in
`video_thread_loop()` (spätere Änderungen) und meldet Letztere über
`ScrcpyClient::set_resolution_callback` weiter, damit Mauszeiger-Umrechnung und
Fensterseitenverhältnis nicht auf dem alten Maß hängenbleiben.

Nicht benutzt, aber verfügbar: `flex_display` (Display folgt der Fenstergröße) und
`RESIZE_DISPLAY`.

## Nachgemessen auf dem Pixel 9 (Android 17, SDK 37)

* Das Display entsteht mit Größe und Punktdichte des Handy-Displays (1080x2424, 420 dpi).
* `dumpsys display` zeigt den Flag-Satz, den `NewDisplayCapture` ab Android 14 setzt:
  `FLAG_ALWAYS_UNLOCKED`, `FLAG_OWN_FOCUS`, `FLAG_OWN_DISPLAY_GROUP`, `FLAG_TRUSTED`,
  `FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS`, `FLAG_OWN_CONTENT_ONLY`,
  `FLAG_DESTROY_CONTENT_ON_REMOVAL`, `FLAG_PRESENTATION`, `FLAG_TOUCH_FEEDBACK_DISABLED`.
* Video (H.264, 1080x2424) und Ton (Roh-PCM) laufen, stabil um 60 fps bei ~2 ms
  Dekodierzeit.
* Aus Fossify heraus gestartete Apps laufen und reagieren normal.

## Sackgassen (damit sie niemand zweimal geht)

* **Voller Pixel-Launcher** (`START_APP` mit `com.google.android.apps.nexuslauncher`):
  Display bleibt schwarz. Android reserviert Zweitdisplays für die Intent-Kategorie
  `SECONDARY_HOME`.
* **Desktop-Modus** (`force_desktop_mode_on_external_displays=1`): Android prüft unser
  Display und lehnt ab — erst `keyguardLocked=true; aborting`, nach dem Entsperren dann
  `shouldCreateOrWarmUpDesk skipping reason: desktop ineligible`. Vermutlich wegen
  `FLAG_OWN_DISPLAY_GROUP`, das der Server setzt und das nur in der JAR änderbar wäre.
* **Simuliertes Zweitdisplay** (`overlay_display_devices` + `display_id=`): Der
  System-Launcher **funktioniert** dort, Apps starten. Aber dieses Display ist eine
  Simulation, die auf den Handybildschirm gezeichnet wird — es rendert nur, solange das
  Handy entsperrt und sein Bildschirm an ist, und trägt dauerhaft die Beschriftung
  „Overlay-Nr. 1" im Bild. Damit fällt weg, wofür der Modus da ist. Code wieder entfernt.
* **`adb shell am start --display <id>`** wird auf Android 17 mit
  `SecurityException: Permission Denial ... with launchDisplayId=` abgelehnt. Der
  `START_APP`-Weg des Servers **nicht** — die Sperre trifft nur fremde Shell-Prozesse,
  nicht den Server, der das Display selbst angelegt hat.
* **`force_resizable_activities=1`** wurde probiert und hat nichts geändert; auf dem
  Testgerät wieder auf 0 gestellt.

## Offen

* Keine Navigationsleiste, weil die Systemdekorationen aus sind. Zurück über Alt+B, die
  Start-Taste (Alt+H) holt in diesem Modus den Launcher zurück auf das Display.
* Die Wischgeste „von oben nach unten" im Launcher öffnet das Benachrichtigungsmenü auf
  dem **Handy**, nicht auf unserem Display: `StatusBarManager` kennt keine Display-ID,
  und ohne Systemdekorationen gibt es bei uns keine Statusleiste. Alt+Auf sollte es
  wieder schließen (Steuernachricht 7), ungeprüft. Sauberer wäre, die Geste in Fossifys
  Einstellungen abzuschalten.
* Die Seitentaste beendet die Sitzung, siehe „Schlaf, Sperre, Panel". Dagegen wurde auf
  dieser Android-Version kein Hebel gefunden.
* Größe und Punktdichte des virtuellen Displays sind nicht einstellbar
  (`Config::new_display_width/height/dpi` existieren, werden aber nicht gesetzt).
* Aufnahme, Datei-Ziehen und die UHID-Tastatur sind in diesem Modus nicht durchgeprüft.
* Der Menüpunkt „Automatische Bildschirmdrehung (Handy)" bezieht sich weiterhin auf das
  Handy-Display und ist hier ohne Wirkung.
