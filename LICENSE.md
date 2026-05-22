# Lizenzen und Drittanbieter-Konformität (Licenses & Third-Party Compliance)

Dieses Dokument bietet eine vollständige Übersicht über die Lizenzierung des **Pixel Mirroring**-Projekts sowie aller verwendeten Drittanbieter-Bibliotheken, Abhängigkeiten, Werkzeuge und Komponenten.

---

## 1. Hauptprojekt (Pixel Mirroring)

Das Pixel Mirroring-Hauptprojekt (sowohl der C++-Desktop-Client als auch die Android-Begleit-App) ist lizenziert unter der **Apache License, Version 2.0**.

**Copyright-Hinweis:**
```text
Copyright (c) 2026 Pixel Mirroring Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
Der vollständige Lizenztext der Apache-Lizenz 2.0 befindet sich in der Datei `LICENSE` im Hauptverzeichnis des Repositories.

---

## 2. Desktop-Client (C++20) Abhängigkeiten

Der Desktop-Client nutzt verschiedene Open-Source-Bibliotheken, die über das Build-System CMake und vcpkg eingebunden werden.

### A. SDL2 (Simple DirectMedia Layer)
* **Verwendung:** Hardware-beschleunigtes Rendering und Event-Handling.
* **Lizenz:** **zlib License** (Freie Nutzung, auch in kommerziellen Projekten).
* **Copyright:** Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>
* **Lizenztext:**
  ```text
  This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
  ```

### B. FFmpeg (libavcodec, libavformat, libavutil, etc.)
* **Verwendung:** Dekodierung des H.264/H.265-Videostreams vom Android-Gerät.
* **Lizenz:** **GNU Lesser General Public License (LGPL), Version 2.1 oder höher** (sofern nicht mit `--enable-gpl` kompiliert).
* **Copyright:** Copyright (c) the FFmpeg developers.
* **Einhaltung der LGPL (Konformitäts-Erklärung):**
  1. **Dynamische Verlinkung:** Der Desktop-Client verlinkt FFmpeg ausschließlich **dynamisch** über DLLs (`libavcodec.dll`, `libavformat.dll` usw.). Dies stellt sicher, dass Nutzer die FFmpeg-Bibliotheken durch eigene, kompatible Versionen ersetzen können (gemäß LGPL v2.1, Sektion 6).
  2. **Unveränderter Code:** Es wurden keine Modifikationen am FFmpeg-Quellcode vorgenommen.
  3. **Quellcode-Verweis:** Der Quellcode von FFmpeg ist unter [https://ffmpeg.org](https://ffmpeg.org) frei verfügbar.
  4. **Lizenz-Kopie:** Der vollständige Text der LGPL v2.1 ist unter [https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) einsehbar.

### C. nlohmann-json
* **Verwendung:** JSON-Parsing für Konfigurationsdateien.
* **Lizenz:** **MIT License**
* **Copyright:** Copyright (c) 2013-2022 Niels Lohmann <https://nlohmann.me>
* **Lizenztext:**
  ```text
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
  ```

### D. cpp-httplib
* **Verwendung:** Subnetz-Scan und Netzwerk-Kommunikation für die automatische Verbindungssuche.
* **Lizenz:** **MIT License**
* **Copyright:** Copyright (c) 2020 yhirose
* **Lizenztext:** (Gleicher MIT-Lizenztext wie bei nlohmann-json. Die Copyright-Zeile lautet `Copyright (c) 2020 yhirose`).

### E. Microsoft vcpkg (Git-Submodul)
* **Verwendung:** C++ Paketmanager zum Beziehen der C++ Abhängigkeiten.
* **Lizenz:** **MIT License**
* **Copyright:** Copyright (c) Microsoft Corporation. All rights reserved.

---

## 3. Mitgelieferte Binärdateien & Tools (Bundled Tools)

Um eine Installation ohne zusätzliche Entwickler-Tools für den Endbenutzer zu ermöglichen, werden folgende offizielle Binärdateien mitgeliefert:

### A. Android Platform-Tools (ADB / AdbWinApi / AdbWinUsbApi)
* **Verzeichnis:** `Client/vendor/platform-tools/`
* **Verwendung:** ADB-Kommunikation mit dem Smartphone (z.B. Installation der Companion-App, Vergabe von Berechtigungen, Starten des Dienstes).
* **Lizenz:** **Apache License, Version 2.0**
* **Copyright:** Copyright (c) The Android Open Source Project
* **Attribution & Konformität:**
  Gemäß den Bedingungen der Apache License 2.0 wird darauf verwiesen, dass die Platform-Tools eigene Lizenzen für Subkomponenten enthalten. Diese sind vollständig in der Datei [Client/vendor/platform-tools/NOTICE.txt](./Client/vendor/platform-tools/NOTICE.txt) dokumentiert und werden unverändert im Paket mitgeliefert.

### B. scrcpy-server.jar
* **Verzeichnis:** `scrcpy_download/scrcpy-server.jar`
* **Verwendung:** Läuft auf dem Android-Gerät, nimmt Bildschirmdaten auf und streamt sie via H.264 over Socket zum Desktop-Client.
* **Lizenz:** **Apache License, Version 2.0**
* **Copyright:** Copyright (c) Genymobile (Teil des [scrcpy](https://github.com/Genymobile/scrcpy) Projekts)
* **Attribution:** Dieses Projekt nutzt die offizielle, vorkompilierte `scrcpy-server.jar` von Genymobile. Es wurden keine Modifikationen an der Server-Binärdatei vorgenommen.

---

## 4. Android Companion-App (Kotlin / Compose) Abhängigkeiten

Die Android-App verwendet Bibliotheken aus dem Android Jetpack Ecosystem sowie Kotlinx-Komponenten.

### A. AndroidX & Jetpack Compose Bibliotheken
* **Komponenten:** `core-ktx`, `lifecycle-runtime`, `activity-compose`, `compose-bom`, `compose-ui`, `material3`, `work-runtime` etc.
* **Lizenz:** **Apache License, Version 2.0**
* **Copyright:** Copyright (c) The Android Open Source Project

### B. JetBrains Kotlin Standard-Bibliotheken & Coroutines
* **Komponenten:** `kotlinx-serialization-json`, `kotlinx-coroutines-android`
* **Lizenz:** **Apache License, Version 2.0**
* **Copyright:** Copyright (c) JetBrains s.r.o.

### C. Google Material Components for Android
* **Komponenten:** `com.google.android.material:material`
* **Lizenz:** **Apache License, Version 2.0**
* **Copyright:** Copyright (c) Google LLC

### D. JUnit (Nur zu Testzwecken)
* **Komponenten:** `junit:junit`
* **Lizenz:** **Eclipse Public License 1.0 (EPL 1.0)**
* **Copyright:** Copyright (c) JUnit contributors.
* **Hinweis:** Wird ausschließlich in Testumgebungen verwendet (`testImplementation`) und nicht mit der App-Produktions-APK ausgeliefert.

---

## 5. Rechtlicher Disclaimer (Legal Disclaimer)

**Dieses Softwareprojekt wird "wie besehen" (AS IS) ohne jegliche ausdrückliche oder implizierte Garantie zur Verfügung gestellt.** Die Entwickler übernehmen keinerlei Haftung für direkte, indirekte oder zufällige Schäden, die aus der Nutzung, Installation oder Fehlfunktion dieser Software resultieren.

Die Nutzung von ADB (Android Debug Bridge) und die Aktivierung von USB- bzw. Wireless-Debugging geschehen auf eigene Verantwortung des Benutzers.
