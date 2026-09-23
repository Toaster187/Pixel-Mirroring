# Mitgelieferte Komponenten

Die einzelnen Quelldateien von Pixel Mirroring behalten ihre Apache-2.0-Lizenz.
Der ausgelieferte Desktop-Build verlinkt FFmpeg mit dem GPL-Encoder x264. Für die
Weitergabe dieses kombinierten Builds gelten deshalb die Bedingungen der GPLv3;
die GPLv3 liegt als `GPL-3.0.txt` bei. Dieser Build ist **kein reiner LGPL-Build**.
Die originalen Hinweise der Bibliotheken stehen unter `licenses/vcpkg/<Paket>/`.

| Bestandteil | Lizenz / Herkunft |
|---|---|
| FFmpeg einschließlich x264-Anbindung | GPL-2.0-or-later; für den kombinierten Build GPLv3 |
| x264 | GPL-2.0-or-later; für den kombinierten Build GPLv3 |
| SDL2 | zlib |
| nlohmann-json, cpp-httplib, Brotli | MIT; maßgeblich sind die beigelegten Originalhinweise |
| scrcpy-Server 4.1 | Apache-2.0, Genymobile und Mitwirkende; unverändert |
| Android Platform Tools | Originalhinweise in `Android-Platform-Tools-NOTICE.txt` |
| Fossify Launcher | GPL-3.0-only; Version und Quellen in `FossifyLauncher.md` |

## Quellcode und Bauvorschriften

Zu jeder neuen Veröffentlichung gehört neben den Installationspaketen das Archiv
`PixelMirroring-<Version>-sources.zip` auf derselben
[Release-Seite](https://github.com/Toaster187/Pixel-Mirroring/releases).
Es enthält den Projektstand, den festgelegten vcpkg-Stand mit Bauvorschriften und
Patches sowie die beim Build verwendeten entpackten Bibliotheksquellen.
`build-manifest.txt` nennt Commit und aktivierte Pakete. Die Binärpakete dürfen erst
veröffentlicht werden, wenn auch dieses Quellcodearchiv verfügbar ist.

Die DLLs sind austauschbar. Kompatible Änderungen und Reverse Engineering zum
Debuggen solcher Bibliotheksänderungen werden durch dieses Projekt nicht untersagt.
Die Bibliotheksquellen können von vcpkg gepatcht sein; die frühere Behauptung,
FFmpeg werde ohne Änderungen und ausschließlich unter LGPL eingebunden, war falsch.

Quellen: [FFmpeg-Lizenzhinweise](https://ffmpeg.org/legal.html),
[scrcpy 4.1](https://github.com/Genymobile/scrcpy/tree/v4.1),
[vcpkg](https://github.com/microsoft/vcpkg).
