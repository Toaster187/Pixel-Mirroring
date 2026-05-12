# Integrationsprobleme: Android App ↔ PC Client

Diese Datei dokumentiert potenzielle Verbindungsprobleme, Race-Conditions und Architektur-Schwachstellen bei der Interaktion zwischen der Android App und dem C++ Desktop Client in "Pixel Mirroring".

## 1. Netzwerk-Discovery (Subnetz & Timeouts)
- **Subnetz-Annahmen im PC Client:** Der C++ `NetworkScanner` extrahiert die Basis-IP (z.B. `192.168.1.`) und scannt hartcodiert die Endungen `.1` bis `.254`. Wenn sich das Gerät in einem größeren Subnetz (z.B. `/16` oder `/22`) befindet, wird das Android-Gerät eventuell überhaupt nicht gescannt.
- **Aggressive Timeouts:** Der HTTP-Client (httplib) im PC nutzt Timeouts von 300ms (Connect) und 500ms (Read). Im WLAN kann ein Wake-up des Android-Gerätes oder die Abarbeitung im Ktor-Server länger dauern, wodurch der Scan fehlschlägt, obwohl das Gerät im Netzwerk erreichbar ist.
- **Thread-Explosion:** Der C++ Scanner erstellt 254 Threads *gleichzeitig* pro gefundenem Subnetz. Auf schwächeren Systemen kann das zu "Thread Exhaustion", hoher CPU-Auslastung oder zum Blockieren durch die Windows-Firewall (Flood-Protection) führen.

## 2. Autorisierung & Silent Failures
- **Fehlendes Feedback bei "Forbidden":** Wenn das Android-Gerät bereits mit einem anderen Client gekoppelt ist, antwortet der Ktor-Server mit `403 Forbidden` (`HttpStatusCode.Forbidden`). Der C++ Client prüft nur auf Status `200 OK` und ignoriert alles andere. Dem User wird am PC nicht mitgeteilt, dass der Zugriff verweigert wurde; es sieht so aus, als ob kein Gerät gefunden wurde.

## 3. ADB & Berechtigungen
- **Das USB-Henne-Ei-Problem:** Die Android-App benötigt `WRITE_SECURE_SETTINGS`, um ADB over WiFi selbstständig zu aktivieren. Der Desktop Client hat zwar eine Funktion `auto_grant_secure_settings()`, diese setzt aber voraus, dass das Handy initial per USB-Kabel angeschlossen ist. Startet ein Nutzer die App direkt drahtlos, schlägt das Aktivieren von ADB WiFi fehl, ohne dass es einen klaren UI-Hinweis gibt.
- **Feste ADB-Ports:** Die Android-App (`AdbWifiManager`) setzt den TCP/IP-Port hart auf `5555`. Wenn eine andere App (oder ein aktiver lokaler ADB-Daemon auf dem Handy) diesen Port belegt, knallt es oder es kommt zu Kollisionen.
- **Rückgabewerte der Settings-API:** `AdbWifiManager` fängt nur `SecurityException` bei `Settings.Global.putInt` ab und gibt dann `true` zurück. Es wird nicht verifiziert, ob der ADB-Daemon wirklich erfolgreich auf dem Port gelauscht hat (z.B. durch prüfen der Properties).

## 4. Ktor Server in der Android App
- **Port-Kollision:** Der Ktor-Server ist hartcodiert auf Port `18294`. Sollte eine andere App zufällig diesen Port nutzen, schlägt `embeddedServer(Netty).start()` fehl. Die Exception wird nicht sauber gefangen, was vermutlich zum Crash des `MirroringService` führt.
- **Coroutines & runBlocking:** Im Ktor `/connect` Endpoint wird `runBlocking { clientStore.isClientPaired(...) }` aufgerufen. Wenn die Persistenzschicht im Hintergrund (z.B. DataStore/Preferences) eine Verzögerung beim I/O hat, blockiert der Netty-Worker-Thread. Dies kann in Kombination mit dem 500ms Read-Timeout des C++ Clients zum Timeout führen.

## 5. Verbindungsaufbau (PC)
- **Hardcodierte IDs:** In `main.cpp` ruft der Desktop-Client `scanner.discover_and_connect("desktop-client-1234", "Desktop-PC")` auf. Wenn zwei Desktop-Clients im selben Netzwerk laufen, haben sie die exakt gleiche ID. Dadurch bricht das Koppelungs-Konzept der Android-App.
- **Race Condition beim scrcpy Start:** Wenn ADB nach dem `connect`-Befehl kurz braucht, um in den Status "device" zu wechseln, kann `adb.get_connected_devices()` im `main.cpp` fehlschlagen, weil das Gerät zwar verbunden, aber noch im Status "offline" oder "authorizing" ist. Der Client beendet sich dann fälschlicherweise, anstatt auf das Gerät zu warten.