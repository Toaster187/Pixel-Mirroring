# Probleme zwischen der Android App und dem Desktop Client (PC-Seite)

Hier ist eine Auflistung der identifizierten Architektur- und Integrationsprobleme, die dazu führen, dass die beiden Komponenten aktuell nicht miteinander funktionieren:

## 1. ADB over WiFi Aktivierung funktioniert so nicht (Android)
**Wo:** `AdbWifiManager.kt`
**Problem:** Die Android-App versucht ADB over WiFi zu aktivieren, indem sie `Settings.Global.putInt(..., "adb_wifi_enabled", 1)` setzt und den Port mit `"adb_tcp_port"` erzwingt. 
**Warum es fehlschlägt:** Ab Android 11 (Wireless Debugging) reicht das reine Setzen dieses Werts nicht aus, um den `adbd` Daemon auf einen spezifischen, statischen Port (z.B. aus der Range 5555-5595) zu binden und zu starten. Das moderne Wireless Debugging nutzt dynamische Ports (via mDNS) und erfordert ein explizites TLS-Pairing. Selbst wenn der alte Modus genutzt werden soll (`service.adb.tcp.port`), erfordert dies das Neustarten des ADB-Daemons (`stop adbd && start adbd`), was ohne Root-Rechte aus einer App heraus nicht möglich ist. Der PC-Client versucht sich folglich auf einen Port zu verbinden, auf dem der Android-ADB-Daemon gar nicht lauscht.

## 2. Scrcpy SCID Formatfehler führt zu Server-Absturz (Desktop)
**Wo:** `ScrcpyClient::start()` und `setup_tunnel()` in `scrcpy_client.cpp`
**Problem:** Der Desktop-Client generiert eine 31-bit Zufallszahl für die SCID (Scrcpy Client ID) und formatiert sie als normalen Dezimal-String (z.B. `"2147483647"`): `scid_ = std::to_string(dis(gen));`.
**Warum es fehlschlägt:** Der Client übergibt Parameter für Scrcpy 2.7. Der Scrcpy 2.0+ Java-Server auf Android erwartet die SCID als zwingend **8-stelligen Hexadezimal-String** und liest ihn via `Integer.parseInt(scid, 16)` ein. Die Übergabe einer 10-stelligen Dezimalzahl wird beim Parsen als Hex-Zahl zu einer `NumberFormatException` führen, da sie die 32-bit Grenze überschreitet. Der Scrcpy-Server auf dem Smartphone crasht somit unmittelbar beim Start.

## 3. Scrcpy Protokoll-Inkompatibilität beim Verbindungsaufbau (Desktop)
**Wo:** `ScrcpyClient::read_metadata()` in `scrcpy_client.cpp`
**Problem:** Der Code versucht zunächst 64 Bytes als Gerätenamen (`device_name`) aus dem Video-Socket zu lesen (`recv(video_socket_, (char*)device_name, 64, 0)`). 
**Warum es fehlschlägt:** Der Client pusht ausdrücklich Version 2.7 des Scrcpy-Servers (`com.genymobile.scrcpy.Server 2.7`). Seit Scrcpy Version 2.0 wird der 64-Byte Gerätename **nicht mehr** am Anfang des Video-Sockets übertragen. Der Video-Socket sendet sofort die Codec-Metadaten (12 Bytes). Da der Client 64 Bytes erwartet, liest er versehentlich die Codec-Infos und die ersten Videoframes in die Variable `device_name` ein, wodurch die anschließenden Codec-Infos korrupt sind und das Video nicht dekodiert werden kann.

## 4. Race-Condition beim Starten des scrcpy-servers (Desktop)
**Wo:** `ScrcpyClient::start_server_process()` in `scrcpy_client.cpp`
**Problem:** Der `scrcpy-server` wird asynchron über `app_process` in einem abgetrennten Thread gestartet (`detach()`). Direkt im Anschluss versucht der Client sofort, die Socket-Verbindung herzustellen (`connect_sockets()`).
**Warum es fehleranfällig ist:** Zwar existiert eine kleine Retry-Schleife (5 Sekunden), jedoch kann der initiale Start der Dalvik-VM (`app_process`) auf manchen älteren Android-Geräten deutlich länger dauern. Ein blockierender Wait oder das Warten auf eine erste Ausgabe (z.B. Logcat/Stdout) wäre robuster.

## 5. Fragiler Dateipfad für adb.exe und scrcpy-server.jar (Desktop)
**Wo:** `get_adb_path()` in `adb_client.cpp`
**Problem:** Die Pfadauflösung nutzt relative Verzeichnisse wie `exe_dir / ".." / "scrcpy_download"`. 
**Warum es fehleranfällig ist:** Wenn das Projekt mit CMake als Out-of-Source Build (z.B. in `out/build/x64-debug/`) kompiliert wird, führt das einfache `..` nicht ins Stammverzeichnis des Repositories, und der Client findet die `adb.exe` und das `scrcpy-server.jar` nicht.

## 6. Input Injection Protokoll veraltet (Desktop)
**Wo:** `ScrcpyClient::inject_touch` in `scrcpy_client.cpp`
**Problem:** Die Byte-Struktur für Touch-Events entspricht nicht exakt dem Scrcpy 2.0+ Control-Protokoll.
**Warum es fehlschlägt:** Da Scrcpy 2.7 erzwungen wird, ignoriert der Server unpassende Control-Messages oder schließt im schlimmsten Fall die Verbindung, wenn die Länge der Pakete oder die Pointer-IDs/Action-Codes nicht zum aktuellen Protokoll passen.
