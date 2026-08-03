# Virtual Display (experimentell)

Status: **Experiment auf `virtual-display-dev-`, nicht für `main`.**

Statt den Handybildschirm zu spiegeln, legt der Server auf dem Handy ein eigenes
virtuelles Display an, das nur für diesen PC existiert. Das Handy bleibt gesperrt und
parallel benutzbar, der PC bekommt seine eigene Android-Oberfläche.

Einschalten: Rechtsklick ins Fenster → **„Eigener PC-Bildschirm (experimentell)"**.
Der Schalter liegt in `Settings::m_virtual_display` (`virtual_display=1` in
`settings.txt`) und ist standardmäßig aus. Umschalten startet den Stream neu, weil der
Server seine Optionen nur beim Start liest.

## Voraussetzung: Server-JAR 4.1

`Client/scrcpy-server.jar` und `scrcpy_download/scrcpy-server.jar` sind das
unveränderte Release-Artefakt `scrcpy-server-v4.1` von
<https://github.com/Genymobile/scrcpy/releases/tag/v4.1> (733.706 Bytes,
SHA-256 `deacb991ed2509715160ffdc7907e47b4160eb30d1566217e9047fd5b8850cae`).
Vorher lag dort v2.7 (71.200 Bytes).

Das Issue nannte 3.x als Ziel, und der Weg ging auch zuerst dorthin. 3.3.4 hat sich am
Pixel 9 aber als unbrauchbar erwiesen (siehe „Nachgemessen"), weil ihm `keep_active`
fehlt. Deshalb steht hier 4.1. Beide Sprünge sind unten aufgeschrieben — der Schritt
2.7 → 3.3.4 betrifft den Steuerkanal, der Schritt 3.3.4 → 4.1 den Videostrom.

Die Versionszeichenkette in `scrcpy_client.cpp` (`SERVER_VERSION`) muss exakt zur JAR
passen — der Server vergleicht sie mit seiner eigenen und startet sonst gar nicht.

Serverseitige Mindestanforderung für `new_display` ist laut `Server.java` Android 10.
Erst ab Android 14 setzt `NewDisplayCapture` aber `VIRTUAL_DISPLAY_FLAG_OWN_FOCUS` und
`…_DEVICE_DISPLAY_GROUP`, und erst damit ist das virtuelle Display wirklich unabhängig
vom Handy-Display (eigener Fokus, eigene Displaygruppe). Ab Android 13 kommen
`…_TRUSTED` und `…_ALWAYS_UNLOCKED` dazu — Letzteres ist der Grund, warum das
gesperrte Handy das virtuelle Display nicht mit ausblendet. Getestet wird auf einem
Pixel 9 mit Android 17.

## Protokoll-Deltas 2.7 → 3.3.4

Alles unten stammt aus einem Quelltextvergleich der beiden Tags, nicht aus dem
Änderungsprotokoll.

### Steuerkanal (PC → Handy)

| Typ | 2.7 | 3.3.4 | Auswirkung auf den Client |
|----|----|----|----|
| 10 | `SET_SCREEN_POWER_MODE`, Nutzlast = Power-Mode-Zahl (0 aus, 2 normal) | `SET_DISPLAY_POWER`, Nutzlast = Boolean | Angepasst: sendet jetzt 1 statt 2. Die alte 2 wäre über `readBoolean()` zufällig auch „an" gewesen. |
| 3 | `INJECT_SCROLL_EVENT`, Festkommawert im Bereich [-1, 1] | derselbe Wert, wird serverseitig **mal 16** genommen | Angepasst: Client skaliert mit 8192/16. Ohne das scrollt jede Radrastung 16-fach. |
| 12 | `UHID_CREATE`: `typ(1) id(2) name_len(1) name rd_size(2) rd_data` | `typ(1) id(2) vendor(2) product(2) name_len(1) name rd_size(2) rd_data` | Angepasst: 4 Bytes eingefügt, beide 0 (genau wie scrcpy selbst es für Tastatur und Maus tut; nur Gamepads tragen echte IDs). Ohne das liest der Server die Namenslänge mitten aus der Report-Beschreibung und reißt beim Anlegen den kompletten Control-Thread mit — Tasten, Maus, Touch und Zwischenablage sind still, das Bild läuft weiter. |
| 16 | – | `START_APP`, `typ(1) len(1) name` | Neu implementiert (`ScrcpyClient::start_app`), bisher nicht verdrahtet. Wird gebraucht, falls ein Gerät keinen Launcher auf ein zweites Display legt. |
| 17 | – | `RESET_VIDEO` | Nicht implementiert. |

Alle übrigen Typen (0–9, 11, 13, 14, 15) sind unverändert. Die Geräte-Nachrichten
Handy → PC (`CLIPBOARD` 0, `ACK_CLIPBOARD` 1, `UHID_OUTPUT` 2) sind byteweise
identisch, `DeviceMessageWriter.java` ist zwischen beiden Tags unverändert.

### Datenströme

`Streamer.java` ist zwischen 2.7 und 3.3.4 unverändert: Gerätename, Codec-Meta
(12 Bytes) und Frame-Header (8 Byte PTS + 4 Byte Länge) bleiben, wie sie waren. Am
Video-, Audio- und Kontrollsocket ändert sich nichts, auch die Reihenfolge nicht.

### Serveroptionen

Entfallen: `lock_video_orientation` (ersetzt durch `capture_orientation`). Der Client
hat sie nie gesendet.

Neu und hier benutzt:

* `new_display=` — leer bedeutet „Größe und Punktdichte des Handy-Displays". Sonst
  `<breite>x<höhe>`, `/<dpi>` oder beides.
* `display_ime_policy=local` — ohne das erscheint die Bildschirmtastatur auf dem
  **Handy**, während am PC getippt wird. Auf einem Handy, das dunkel und gesperrt
  bleiben soll, findet die niemand wieder.

Neu und hier nicht benutzt: `vd_system_decorations`, `vd_destroy_content`, `angle`,
`capture_orientation`, `screen_off_timeout`, `list_apps`.

Unbekannte Optionen sind ab 3.x kein Startfehler mehr, der Server schreibt nur eine
Warnung — ein falsch geschriebener Schlüssel fällt also nicht mehr sofort auf.

## Protokoll-Deltas 3.3.4 → 4.1

Hier sitzt die Änderung nicht im Steuerkanal, sondern im **Videostrom**. Wer nur die
Versionszeichenkette hochzieht, bekommt ein Bild aus Müll.

* **Der Videokopf schrumpft von 12 auf 4 Bytes.** Früher standen Codec-ID, Breite und
  Höhe zusammen im Kopf. Ab 4.0 steht dort nur noch die Codec-ID.
* **Die Bildgröße kommt stattdessen im Strom**, als „Session-Paket": zwölf Bytes aus
  Flags, Breite, Höhe, erkennbar am gesetzten obersten Bit. Es kommt einmal vor dem
  ersten Bild und danach erneut nach jeder Drehung oder Größenänderung. Ein
  Session-Paket ist die ganze Nachricht — es folgt keine Nutzlast.
* **Die Paketflags sind ein Bit nach unten gerutscht:** `SESSION` ist neu auf Bit 63,
  `CONFIG` wanderte von 63 auf 62, `KEY_FRAME` von 62 auf 61. Wer weiter auf Bit 63
  prüft, hält jedes Session-Paket für ein Konfigurationspaket.
* `send_codec_meta` heißt jetzt `send_stream_meta` und deckt beides ab.
* Neue Steuernachrichten 18–22 (Kamera-Zoom/Blitz, `RESIZE_DISPLAY`, `SCAN_FILE`) —
  additiv, keine davon wird bisher benutzt.
* `UHID_CREATE`, Scroll-Skalierung und `SET_DISPLAY_POWER` sind gegenüber 3.3.4
  unverändert.

Der Client liest das Session-Paket in `read_metadata()` (Anfangsgröße) und in
`video_thread_loop()` (spätere Änderungen) und meldet Letztere über
`ScrcpyClient::set_resolution_callback` weiter, damit Mauszeiger-Umrechnung und
Fensterseitenverhältnis nicht auf dem alten Maß hängenbleiben.

Nicht benutzt, aber jetzt verfügbar: `flex_display` (Display folgt der Fenstergröße)
und `RESIZE_DISPLAY`.

## Was in diesem Modus bewusst ausgeschaltet ist

Das Handy-Display ist nicht mehr das Bild, also darf auch niemand mehr daran ziehen:

* **Kein Entsperren.** `unlock_device_if_needed()` steigt sofort aus, außer die Person
  hat „Handy entsperren" selbst angeklickt. Sonst würde die PIN-Eingabe genau das
  rückgängig machen, wofür der Schalter da ist.
* **Keine Helligkeitsverstellung.** `SET_DISPLAY_POWER` dagegen bleibt aktiv und ist in
  diesem Modus sogar der empfohlene Begleiter — siehe „Nachgemessen".
* **Kein Bildschirm-Watchdog.** Der Poll-Thread wertet „Handy-Display ist aus" sonst
  als „Mensch hat das Handy weggelegt", beendet den Stream und faltet das Fenster in
  die Taskleiste. Die Zwischenablage-Abfrage im selben Thread läuft weiter.

Der scrcpy-Server selbst weckt das Handy in diesem Modus ebenfalls nicht: sein
`power_on`-Zweig greift nur bei `displayId == 0`.

## Nachgemessen auf dem Pixel 9 (Android 17, SDK 37)

Erster Lauf am 3.8.2026, `new_display=` mit `display_ime_policy=local`:

* Das Display entsteht als `displayId=14`, Name `scrcpy`, 1080x2424 bei 420 dpi — also
  Größe und Punktdichte des Handy-Displays, wie beim leeren Optionswert erwartet.
* `dumpsys display` zeigt genau den Flag-Satz, den `NewDisplayCapture` ab Android 14
  setzt: `FLAG_ALWAYS_UNLOCKED`, `FLAG_OWN_FOCUS`, `FLAG_OWN_DISPLAY_GROUP`,
  `FLAG_TRUSTED`, `FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS`, `FLAG_OWN_CONTENT_ONLY`,
  `FLAG_DESTROY_CONTENT_ON_REMOVAL`, `FLAG_PRESENTATION`, `FLAG_TOUCH_FEEDBACK_DISABLED`.
* **Der Pixel-Launcher erscheint von selbst**, als
  `com.google.android.apps.nexuslauncher/com.android.launcher3.secondarydisplay.SecondaryDisplayLauncher`
  und als `topResumedActivity`. `start_app()` wird auf diesem Gerät also nicht gebraucht.
* Video (H.264, 1080x2424) und Ton (Roh-PCM) laufen beide.

### Das virtuelle Display schläft mit dem Handy ein

Der wichtigste Befund, und der Grund für 4.1. Sobald das Handy selbst einschläft, hängt
WindowManager auch an unser Display ein Schlaf-Token, der Launcher hört auf zu zeichnen
und jede Eingabe verpufft:

```
09:36:21.120 PowerManagerService: Going to sleep due to power_button
09:36:21.660 Add SleepToken: tag=Display-off, displayId=0
09:36:21.868 Add SleepToken: tag=Display-off, displayId=17   <- unseres, 200 ms später
09:36:21.891 VRI[SecondaryDisplayLauncher]: visibilityChanged true -> false
```

`FLAG_ALWAYS_UNLOCKED` geht nur am Sperrbildschirm vorbei, nicht am Schlaf.

**`keep_active` allein reicht nicht.** Der Server weckt damit alle 4 Sekunden
(`KEEP_ACTIVE_INTERVAL_MS`) über `PowerManager.userActivity()` — gemessen ergibt das
einen Sägezahn (Token weg, 7 s später wieder da), und nach 30 Sekunden ohne Eingabe
steht das Display auf `isSleeping=true` und liefert kein einziges Bild mehr.

**Was wirklich hilft:** das Gerät wach lassen und nur sein *Panel* abschalten, also
`Settings::m_screen_off` („Handy-Display komplett aus"). Das geht über
`SET_DISPLAY_POWER` direkt an SurfaceControl vorbei an der Schlaflogik. Gemessen mit
beiden Schaltern an: `mWakefulness=Awake`, virtuelles Display `isSleeping=false`,
Launcher `topResumedActivity`, in 30 Sekunden **null** Schlaf-Token-Wechsel — und das
Handy-Panel physisch dunkel. Deshalb ist `m_screen_off` in diesem Modus ausdrücklich
**nicht** abgeschaltet, obwohl es das Handy-Panel anfasst.

Bleibt die Lücke: drückt jemand die Seitentaste, schläft das Gerät und das virtuelle
Display mit ihm. Dagegen gibt es auf dieser Android-Version keinen Hebel im Client.

## Offen

* **`force_resizable_activities`** steht auf dem Testgerät jetzt auf 1 (vorher 0),
  gesetzt per `adb shell settings put global force_resizable_activities 1`. Der
  Launcher-Task auf dem virtuellen Display ist als `nonResizable` markiert, und die
  Gemeinde berichtet, dass Apps sich sonst nicht auf ein Zweitdisplay starten lassen.
  Ob das hier wirklich nötig war, ist **nicht sauber isoliert** — es wurde zusammen mit
  dem Schlafproblem geändert. Zurück mit `settings put global
  force_resizable_activities 0`. Falls es nötig bleibt, muss der Client es selbst setzen
  und beim Beenden zurückstellen, so wie er es mit der Helligkeit tut.
* `adb shell am start --display <id>` scheitert auf Android 17 mit
  `SecurityException: Permission Denial ... with launchDisplayId=`. Betrifft alles, was
  aus der Shell heraus auf das Display starten will — also auch
  `ScrcpyClient::start_app()`, das damit auf diesem Gerät vermutlich wirkungslos ist.
* Auf Geräten ohne Launcher für Zweitdisplays bleibt das Display schwarz. Dafür gibt es
  `ScrcpyClient::start_app()` — implementiert, aber siehe den Punkt darüber.
* Fenstergröße und Punktdichte des virtuellen Displays sind nicht einstellbar
  (`Config::new_display_width/height/dpi` existieren, werden aber nicht gesetzt).
* Aufnahme, Dateiablage per Ziehen und die Alt-Tastenkürzel sind in diesem Modus nicht
  durchgeprüft.
* Der Menüpunkt „Automatische Bildschirmdrehung (Handy)" bezieht sich weiterhin auf das
  Handy-Display und ist hier ohne Wirkung.
