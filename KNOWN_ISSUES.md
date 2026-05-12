# Bekannte Probleme & Integrationsrisiken (Android <-> PC)

Diese Dokumentation fasst die potenziellen Fehlerquellen und Verbindungsprobleme zwischen der **Pixel Mirroring Android App** und dem **C++ Desktop Client** zusammen. Da die Architektur stark auf lokalen Netzwerken und ADB over WiFi basiert, ergeben sich folgende technische Hürden:

## 1. Netzwerk-Erkennung (Discovery)
*   **Starres Subnetz-Scanning:** Der `NetworkScanner` im C++ Client extrahiert aus den lokalen IPs lediglich /24-Subnetze (z.B. `192.168.1.x`) und scannt die Endungen 1 bis 254. Bei abweichenden Subnetzmasken (z.B. /16 Netze wie `10.0.x.x`) wird das Android-Gerät nicht gefunden.
*   **Android Doze-Mode / App-Kill:** Die Android App lauscht auf Port 18294 auf eingehende Discovery-Requests (via HTTP POST `/connect`). Wenn Android die App im Hintergrund beendet (Battery Optimization / Doze Mode), ist der Server offline und der C++ Client findet das Gerät nicht. 
*   **Windows Firewall:** Ausgehende Scans des Clients und eingehende Scrcpy-Streams können durch die Windows-Firewall blockiert werden. Es fehlt aktuell eine Logik, die automatische Firewall-Regeln bei der Installation registriert.
*   **Aktive VPNs:** Wenn auf dem PC oder dem Android-Gerät ein VPN (z.B. WireGuard, NordVPN) aktiv ist, ändert sich das primäre Subnetz, und Discovery schlägt sehr wahrscheinlich fehl.

## 2. ADB over WiFi (AdbWifiManager)
*   **Legacy vs. Modern Wireless Debugging:** `AdbWifiManager.kt` setzt `Settings.Global.putString(ADB_TCP_PORT, "5555")`. Auf älteren Geräten (Android 10 und tiefer) funktioniert das gut. Ab Android 11 (API 30+) verwendet das native "Wireless Debugging" jedoch dynamische, zufällige Ports. Das reine Ändern der Settings zwingt den `adbd` (ADB Daemon) auf vielen modernen Custom ROMs oder Geräten nicht zwingend zu einem Neustart auf Port 5555.
*   **`WRITE_SECURE_SETTINGS` Berechtigung:** Um ADB per Code zu aktivieren, braucht die App diese Berechtigung. Der C++ Client bietet zwar `auto_grant_secure_settings()` an, dies erfordert aber, dass der Nutzer sein Handy *initial* per USB anschließt. Wird dies vergessen oder die Berechtigung durch ein System-Update entzogen, schlägt die Verbindung stumm fehl.

## 3. Desktop Client (Abhängigkeiten)
*   **Fehlendes ADB in `PATH`:** `AdbClient::run_adb_command` ruft Befehle via `popen("adb ...")` auf. Wenn die `adb.exe` nicht im System-PATH hinterlegt ist oder nicht explizit im gleichen Ordner wie der Client liegt, schlagen alle ADB-Aufrufe direkt fehl.
*   **scrcpy-Server Pfad:** Beim Push auf das Gerät (`adb push scrcpy-server.jar /data/local/tmp/`) muss der Desktop-Client wissen, wo die `.jar` Datei exakt liegt. Es gibt im Code keine Garantie, dass der Pfad robust aufgelöst wird, wenn der Client aus unterschiedlichen Work-Directories gestartet wird.

## 4. Race Conditions & Verbindungsaufbau
*   **Timeout nach Settings-Change:** Wenn der Android-HTTP-Server den `/connect` Request erhält und ADB over WiFi aktiviert, braucht der `adbd` (Daemon) auf dem Smartphone kurz Zeit zum Starten. Wenn der C++ Client *sofort* `adb connect IP:5555` aufruft, kann die Verbindung abgelehnt werden. Ein Retry-Mechanismus fehlt hier potenziell.

## 5. Multi-Device und Fehlerbehandlung
*   **Port-Konflikte (Port Forwarding):** Wenn der Client `adb forward` oder `adb reverse` aufruft, um den Video-Stream abzugreifen, und scrcpy unerwartet abstürzt, bleiben die Ports oftmals belegt. Bei einem erneuten Verbindungsversuch schlägt das Port-Binding fehl.
*   **Fehlende Fehler-Rückmeldung an Nutzer:** Weder in der UI (C++) noch in Android wird dem User aktuell klar kommuniziert, *warum* eine Verbindung fehlgeschlagen ist (z.B. "Kein USB-Debugging aktiviert", "Berechtigung fehlt", "Falsches Netzwerk").
