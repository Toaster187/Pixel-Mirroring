# Virtual Display (experimentell)

Status: **Experiment auf `virtual-display-dev-`, nicht für `main`.**

Statt den Handybildschirm zu spiegeln, legt der Server auf dem Handy ein eigenes
virtuelles Display an, das nur für diesen PC existiert. Das Handy bleibt gesperrt und
parallel benutzbar, der PC bekommt seine eigene Android-Oberfläche.

Einschalten: Rechtsklick ins Fenster → **„Eigener PC-Bildschirm (experimentell)"**.
Der Schalter liegt in `Settings::m_virtual_display` (`virtual_display=1` in
`settings.txt`) und ist standardmäßig aus. Umschalten startet den Stream neu, weil der
Server seine Optionen nur beim Start liest.

## Voraussetzung: Server-JAR 3.3.4

`Client/scrcpy-server.jar` und `scrcpy_download/scrcpy-server.jar` sind das
unveränderte Release-Artefakt `scrcpy-server-v3.3.4` von
<https://github.com/Genymobile/scrcpy/releases/tag/v3.3.4> (90.980 Bytes,
SHA-256 `8588238c9a5a00aa542906b6ec7e6d5541d9ffb9b5d0f6e1bc0e365e2303079e`).
Vorher lag dort v2.7 (71.200 Bytes).

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

### Was 4.x zusätzlich ändern würde

Nur als Notiz, 4.x ist hier bewusst nicht eingebaut: `send_codec_meta` ist weg
(`send_stream_meta`), und es gibt fünf neue Steuernachrichten (18–22), darunter
`RESIZE_DISPLAY`, mit dem sich ein virtuelles Display an die Fenstergröße anpassen
ließe. Die JAR wächst dabei von 91 KB auf 733 KB.

## Was in diesem Modus bewusst ausgeschaltet ist

Das Handy-Display ist nicht mehr das Bild, also darf auch niemand mehr daran ziehen:

* **Kein Entsperren.** `unlock_device_if_needed()` steigt sofort aus, außer die Person
  hat „Handy entsperren" selbst angeklickt. Sonst würde die PIN-Eingabe genau das
  rückgängig machen, wofür der Schalter da ist.
* **Keine Helligkeitsverstellung** und kein automatisches `SET_DISPLAY_POWER`. Das
  Zurücksetzen bleibt aktiv: ein Panel, das eine frühere Spiegelungssitzung dunkel
  gelassen hat, wird auch hier wieder hell.
* **Kein Bildschirm-Watchdog.** Der Poll-Thread wertet „Handy-Display ist aus" sonst
  als „Mensch hat das Handy weggelegt", beendet den Stream und faltet das Fenster in
  die Taskleiste. Die Zwischenablage-Abfrage im selben Thread läuft weiter.

Der scrcpy-Server selbst weckt das Handy in diesem Modus ebenfalls nicht: sein
`power_on`-Zweig greift nur bei `displayId == 0`.

## Offen

* Ob der Pixel-Launcher auf dem virtuellen Display erscheint, ist ungetestet. Wenn das
  Display schwarz bleibt, ist `ScrcpyClient::start_app()` der Weg dorthin — bisher ohne
  Bedienelement.
* Fenstergröße und Punktdichte des virtuellen Displays sind nicht einstellbar
  (`Config::new_display_width/height/dpi` existieren, werden aber nicht gesetzt).
* Aufnahme, Dateiablage per Ziehen und die Alt-Tastenkürzel sind in diesem Modus nicht
  durchgeprüft.
* Der Menüpunkt „Automatische Bildschirmdrehung (Handy)" bezieht sich weiterhin auf das
  Handy-Display und ist hier ohne Wirkung.
