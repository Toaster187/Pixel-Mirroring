# Bearbeitung der offenen Issues

## Funktion und Bedienung

- **#55:** Das virtuelle Display ist seit `2b436ef` auf `main` enthalten:
  unveränderter scrcpy-Server 4.1, eigener Launcher und separate Eingabe.
- **#56:** Die Android-Übersicht liegt auf **Alt+N**, einschließlich Menühinweis.
- **#57:** Die Einstellungen sind in drei aufklappbare Bereiche gegliedert:
  „Bild, Ton und Leistung“, „Tastatur und Steuerung“, „Gerät und Verbindung“.
  Es bleibt jeweils nur ein Bereich offen; Qualitätspresets bleiben darunter erreichbar.
- **#64:** Vorhandene Einstellungsdateien ohne `virtual_display` behalten Spiegelung.
  Frische Installationen verwenden weiterhin den virtuellen Bildschirm; ausdrücklich
  gespeicherte Werte gewinnen. Ein Regressionstest deckt alle drei Fälle ab.
- **#74:** Nur nach einer authentifizierten Verbindungsanfrage bzw. einem Heartbeat
  wird ein lokaler Android-Live-Status angefordert. Der Bereitschaftsdienst erhält
  keinen Live-Status. Das Sitzungsende entfernt die Anforderung; bei Verbindungsverlust
  greift der bestehende 60-Sekunden-Watchdog. Ältere Android-Versionen behalten die
  normale Benachrichtigung. Android bzw. die Benachrichtigungseinstellungen entscheiden,
  ob der Status-Chip angezeigt wird. Es gibt keine Weiterleitung an Wear OS.

## Auslieferung

- **#65 – Entscheidung:** Fossify bleibt gebündelt. Die gepinnte APK umfasst
  5.222.266 Bytes. Dies ist der bewusst akzeptierte Preis eines ohne zusätzliche
  Laufzeitdownloads funktionierenden virtuellen Bildschirms. Ein zweiter Installer
  würde die Installation komplizierter machen, ein eigener Launcher neue Wartung
  verursachen. Version und Prüfsumme bleiben festgelegt; ein Versionswechsel benötigt
  eine erneute Größenprüfung. Es werden keine zusätzlichen DLL-Verzeichnisse kopiert.
- **#66:** Die Versionsangaben im Fossify-Quellenangebot werden beim Konfigurieren mit
  der APK-Konfiguration verglichen. Alle Paketformate müssen dieselben Lizenzdateien
  enthalten; ZIP, NSIS und MSI werden nach dem Entpacken anhand ihrer SHA-256-Werte
  gegen die Installation geprüft. Android Platform Tools erhalten ihren Original-NOTICE,
  vcpkg-Bibliotheken ihre tatsächlichen Copyright-Dateien. FFmpeg mit x264 ist GPL,
  nicht LGPL: die Dokumentation stellt das klar. Neue Releases enthalten zusätzlich
  die Projektquellen, die verwendeten gepatchten Bibliotheksquellen und vcpkg-Bauvorschriften
  in einem getrennten Quellcodearchiv. Bereits veröffentlichte Pakete werden durch
  diese Änderung nicht rückwirkend ersetzt.
- **#70:** Bereits auf `main`: Release-Konfiguration verlangt Fossify, prüft die
  APK-Prüfsumme und probiert sowohl das F-Droid-Repository als auch dessen Archiv.

## Stream-Korrekturen und vorhandene Fixes

- **#67:** Bereits auf `main`: Helligkeit und Rotation liegen im mutex-geschützten
  `SessionPhoneState`; Wiederherstellung erfolgt nach dem Beenden der Hintergrundtasks.
- **#68:** Bereits auf `main`: Benutzerdefinierte Paketnamen werden getrimmt und vor
  dem Start geprüft. Fehlende Apps führen zur Spiegelung zurück; fehlgeschlagenes
  Senden von `START_APP` wird angezeigt.
- **#69:** Ergänzt den vorherigen Fix: Displaygröße wird erst beim Ausführen der
  UI-Aufgabe gelesen. Auch die Eingabekoordinaten werden auf diesem Thread aktualisiert,
  damit eine verspätete Startaufgabe keine neuere Größe überschreibt.
- **#71:** Vorhandene Abfrage des Wachzustands ergänzt: Nur eine ausdrücklich positive
  `mInteractive`-Antwort überspringt das Wecken. Leere oder unbekannte Antworten tun das nicht.
- **#72:** Bereits auf `main`: UHID_INPUT, UHID_DESTROY, Tastatureinstellungen und Scroll
  verwenden dieselben getesteten Builder wie der Versand; alle Paketflags werden verglichen.
- **#73:** Vorhandene Minimier-Sperre ergänzt: Sobald der Sitzungsabbau beginnt, beendet
  auch die Heartbeat-Bedingung die Schleife unabhängig von einem verbleibenden Pause-Flag.

Automatisierte Tests ersetzen keinen Gerätetest von Sperren, Aufwecken, Eingabe,
Live-Status und virtueller Anzeige auf einem echten Telefon.

## Verifikation

- Vollständiger Windows-Release-Build mit MSVC; alle drei CTest-Suites bestanden.
- Android `assembleDebug testDebugUnitTest`: 11 Tests, keine Fehler.
- ZIP, NSIS und MSI gebaut und entpackt; 26 Pflicht- und Lizenzdateien je Format
  per SHA-256 gegen den Installationsbaum geprüft.
- Fossify-Konfigurationsprüfung: korrekte Angaben akzeptiert; absichtlich veraltete
  Versionsangabe abgelehnt.
- APK: 4.470.705 Bytes; Windows-ZIP des Prüfbuilds: 20.278.499 Bytes.
