# Interoperabilitätsprobleme zwischen Android App und PC Client

Nach einer Analyse des Quellcodes wurden folgende Probleme gefunden, die verhindern, dass die Android-App und der PC-Client reibungslos miteinander funktionieren:

## 1. 🚨 [KRITISCH] Einmal-Nutzung / Lockout durch zufällige Client-ID
**Wo:** `main.cpp` (PC) & `MirroringService.kt` (Android)
**Problem:** Der Desktop-Client generiert bei jedem Programmstart eine komplett neue, zufällige `clientId` (`"desktop-client-" + rand()`). Die Android-App speichert beim ersten erfolgreichen Verbindungsaufbau diese `clientId` dauerhaft im `PairedClientStore`. 
**Auswirkung:** Beim ersten Start funktioniert die Koppelung einwandfrei. Startet man den PC-Client jedoch neu (und er hat eine neue ID), verweigert die Android-App mit `403 Forbidden` die Verbindung, da die neue ID nicht mit der gespeicherten übereinstimmt. Der Nutzer wird dauerhaft ausgesperrt, bis die App-Daten auf dem Handy gelöscht werden.
**Lösung:** Die generierte `clientId` muss auf dem Desktop (z.B. in der Registry, APPDATA oder einer config-Datei) persistiert werden, damit der PC bei jedem Start dieselbe ID sendet.

## 2. 🚨 [KRITISCH] Inkompatibler Scrcpy-Handshake (Protokoll-Version)
**Wo:** `scrcpy_client.cpp` (PC)
**Problem:** Der C++ Client pusht und startet explizit `scrcpy-server.jar` in Version `2.7`. Die Logik, wie die Video-Metadaten vom Socket gelesen werden (die ersten 12 Bytes direkt nach dem Verbinden als Codec, Breite und Höhe), stammt jedoch aus der alten **Scrcpy Version 1.x**.
**Auswirkung:** Ab Scrcpy 2.0 wurde das Verbindungsprotokoll (Handshake, Dummy-Bytes, Audio-Socket) massiv geändert. Das Lesen der 12 Bytes wird auf Scrcpy 2.7 fehlschlagen oder Mülldaten produzieren. Der Video-Decoder erhält falsche Codec-Infos und das Video wird nicht starten oder das Programm stürzt ab.
**Lösung:** Der C++ Client muss das Handshake-Protokoll von Scrcpy 2.7 korrekt implementieren (z.B. 68-Byte Handshake parsen, Dummy-Bytes abfangen, Control-Socket Header aktualisieren) oder die App muss eine ältere Scrcpy 1.x Version nutzen.

## 3. ⚠️ [Potenziell] Race-Condition beim ADB Daemon (adbd) Neustart
**Wo:** `AdbWifiManager.kt` (Android) & `adb_client.cpp` (PC)
**Problem:** Wenn die Android-App über `Settings.Global` das ADB TCP/IP-Protokoll aktiviert, startet Android den internen `adbd` Prozess neu. Dieser Vorgang dauert eine gewisse Zeit. Der PC-Client versucht fast zeitgleich (`NetworkScanner` gibt IP zurück -> sofortiges `adb connect`) sich zu verbinden, mit maximal 3 Retries im Abstand von 1 Sekunde (also max. 3 Sekunden Wartezeit).
**Auswirkung:** Auf etwas langsameren Android-Geräten kann der Neustart von `adbd` länger als 3 Sekunden dauern. Der PC-Client gibt in diesem Fall mit einem Timeout auf, obwohl das Gerät kurz darauf bereit wäre.
**Lösung:** Die Anzahl der Retries im `AdbClient::connect_device` sollte erhöht werden (z.B. 10 Retries mit 1 Sekunde Abstand).

## 4. ⚠️ [Potenziell] Harte Port-Kodierung für Scrcpy-Sockets
**Wo:** `scrcpy_client.cpp` (PC)
**Problem:** Der PC-Client verwendet fest den lokalen Port `27183` für das ADB-Forwarding/Reversing (`tcp:27183`).
**Auswirkung:** Falls dieser Port zufällig durch ein anderes Programm auf dem PC belegt ist oder der Nutzer zwei Instanzen des Pixel-Mirroring Clients starten will, schlägt das Port-Binding / Tunneling fehl.
**Lösung:** Ein Fallback auf einen dynamischen Port oder das inkrementelle Durchsuchen freier Ports (z.B. `27183` bis `27199`) sollte implementiert werden.

## 5. ⚠️ [Potenziell] Ktor-Server Port-Bindung unter Android
**Wo:** `MirroringService.kt` (Android)
**Problem:** Der Background Service der Android-App bindet den Ktor-Webserver fest an Port `18294`. Wenn der Service vom Android-System gekillt wird und Netty den Port im TCP-State `TIME_WAIT` hinterlässt, kann ein schneller Neustart des Service zu einer `BindException` (Port already in use) führen.
**Auswirkung:** Die App startet keinen Server mehr auf dem Port und der PC kann die App im Netzwerk nicht finden. Die App muss per Force-Stop beendet werden.
**Lösung:** Sicherstellen, dass Sockets SO_REUSEADDR verwenden (in Ktor konfigurieren) oder zumindest einen Retry nach ein paar Sekunden beim Serverstart versuchen.

---
**Fazit:** Aufgrund von Problem 1 und Problem 2 sind die Android-App und der PC-Client im jetzigen Zustand **nicht miteinander funktionsfähig**.