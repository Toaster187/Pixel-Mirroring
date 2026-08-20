# Who liked? — erste Demo

Das Partyspiel „Who liked?“ mit Instagram-Likes: Jede Person lädt ihren
Instagram-Datenexport hoch, das Spiel zeigt einen Beitrag daraus, und die Runde
rät, aus wessen Likes er stammt.

Dies ist eine **eigenständige Demo** neben den beiden Pixel-Mirroring-Komponenten.
Sie fasst weder die Android-App noch den Desktop-Client an und wird von deren
Build nicht berührt.

---

## Starten

Voraussetzung: Node ab Version 20. Keine Abhängigkeiten, kein `npm install`.

```
cd WhoLiked
node server.js
```

Dann `http://localhost:8787` im Browser öffnen.

```
WL_PORT=8787         Port
WL_MIN_GAP_MS=250    Mindestabstand zwischen zwei Anfragen an Instagram
WL_TIMEOUT_MS=12000  Zeitlimit je Anfrage
WL_CACHE_DIR=.cache  Ablage für Prüfergebnisse und Vorschaubilder
```

Die Seite **muss** über den Server laufen. Direkt aus dem Dateisystem geöffnet
kann der Browser nicht feststellen, welche Beiträge öffentlich sind — die Demo
sagt das dann auch und spielt nicht einfach los.

## Den Instagram-Export anfordern

Instagram-App → Einstellungen → Konten-Center → *Deine Informationen und
Berechtigungen* → *Informationen exportieren* → Format **JSON** (nicht HTML).
Instagram schickt den Download-Link per Mail; das dauert je nach Kontogröße von
Minuten bis zu ein paar Tagen.

Das ZIP wird unverändert in die Seite gezogen. Es muss nichts entpackt oder
vorbereitet werden.

## Was mit den Daten passiert

Der Export enthält weit mehr als Likes: private Nachrichten samt Fotos und
Videos, besuchte Stories, Werbeprofile. Davon wird nichts angefasst.

* **Genau eine Datei wird gelesen:** `your_instagram_activity/likes/liked_posts.json`.
  Der ZIP-Leser (`public/js/zip.js`) liest das Inhaltsverzeichnis des Archivs
  und entpackt danach ausschließlich diesen einen Eintrag. Alle übrigen Einträge
  bleiben komprimiert und werden nie gelesen — bei einem 5-GB-Export wandern
  dadurch nur rund hundert Kilobyte durch den Speicher. Genau deshalb liegt hier
  ein eigener ZIP-Leser statt einer fertigen Bibliothek: die üblichen laden erst
  das ganze Archiv und fragen dann, was gebraucht wird.
* **Nichts wird hochgeladen.** Das Archiv verlässt den Rechner nicht; ausgewertet
  wird es im Browser.
* **Nichts wird gespeichert.** Ein Neuladen der Seite löscht alle importierten
  Likes. Auf der Platte landet nur, *ob* ein Beitrag öffentlich abrufbar ist,
  plus dessen Vorschaubild (`.cache/`) — beides öffentliche Informationen.
* **In der Spielrunde lädt der offizielle Instagram-Embed.** Instagram sieht
  dabei, dass jemand diesen Beitrag ansieht, aber nicht, wer ihn geliked hat.
  Die Galerie kommt ohne aus, sie zeigt die lokal zwischengespeicherten
  Vorschaubilder.

## Öffentlich oder nicht — und warum das ein Server macht

Die Anforderung war: private Beiträge dürfen nicht kaputt angezeigt werden. Ein
Embed einfach einzubauen und zu hoffen, reicht dafür nicht — bei einem privaten
oder gelöschten Beitrag rendert Instagram eine Fehlerkarte, und im Browser lässt
sich das nicht auslesen: Instagram schickt keine CORS-Header, und in einen
fremden iframe darf eine Seite nicht hineinsehen.

Deshalb prüft der Server jeden Beitrag einmal vorab über dessen öffentliche
Embed-Seite und antwortet dreiwertig:

| Zustand | Bedeutung | Folge |
| --- | --- | --- |
| `public` | Es wurde eine Medien-URL gefunden. | Kommt ins Spiel und in die Galerie. |
| `unavailable` | Privat, gelöscht, gesperrt — oder Embed ohne Medien. | Wird ausgeblendet und gezählt. |
| `unknown` | Zeitlimit, Rate-Limit, Anmeldeseite, kein Netz. | Wird ausgeblendet **und benannt**. |

Der Unterschied zwischen den letzten beiden ist der wichtigste Teil: Würde ein
Rate-Limit als „privat“ durchgehen, sähe ein gebremster Prüflauf so aus, als
wären alle Freunde plötzlich auf privat gestellt — und das Spiel wäre leer, ohne
dass jemand erführe, warum. `unknown` wird deshalb auch nicht zwischengespeichert;
ein späterer Durchlauf holt diese Beiträge nach.

Die Prüfung läuft mit Abstand zwischen den Anfragen (Standard 250 ms) und wird
zwischengespeichert. Der erste Durchlauf über ~350 Likes dauert gut eine Minute,
jeder weitere ist sofort fertig.

## Spielablauf

1. **Spieler:** Jede Person zieht ihr ZIP in die Seite. Der Name wird aus dem
   Ordnernamen im Export vorgeschlagen und lässt sich überschreiben.
2. **Prüfen:** Alle Likes aller Spieler werden zusammengeworfen, doppelte
   Beiträge zu einem Eintrag verschmolzen und einmal geprüft.
3. **Raten:** Reihum ist eine Person dran. Sie sieht einen Beitrag und tippt,
   aus wessen Likes er stammt — auch die eigenen sind dabei. Treffer gibt einen
   Punkt.
4. **Galerie:** Alle öffentlichen Likes als Raster, nach Spieler filterbar,
   Klick öffnet den Beitrag.

Runden werden bevorzugt aus Beiträgen gebildet, die genau eine Person geliked
hat — dort gibt es eine eindeutige Antwort. Reichen die nicht, kommen gemeinsame
Likes dazu; dort zählt jede der beteiligten Personen als richtig.

## Aufbau

```
WhoLiked/
  server.js              HTTP-Server: statische Auslieferung, Prüfung, Vorschaubilder, Cache
  lib/embed.js           Embed-Seite auswerten -> public / unavailable / unknown (ohne Netz, testbar)
  public/
    index.html           alle Bildschirme
    styles.css
    js/zip.js            minimaler ZIP-Leser, entpackt genau einen Eintrag
    js/likes.js          liked_posts.json lesen (drei Formate, mehrsprachig, Mojibake-Reparatur)
    js/api.js            Client für den Prüf-Server
    js/game.js           Pool, Runden, Punkte (ohne DOM, testbar)
    js/app.js            Verdrahtung der Bildschirme
  test/run-tests.mjs     Selbsttests
  test/make-zip.mjs      ZIP-Schreiber, nur für Testdaten
```

Zwei Dinge im Export sind unangenehmer, als sie aussehen, und beide erledigt
`likes.js`:

* **Das Dateiformat hat sich mehrfach geändert.** Aktuell ist eine flache Liste
  mit `label_values`, davor `likes_media_likes`. Beide werden gelesen, und wenn
  keins passt, werden als letzte Rettung einfach alle Beitrags-URLs aus dem Text
  gefischt — dann fehlen Datum und Urheber, aber der Import scheitert nicht.
* **Die Feldnamen sind übersetzt.** Ein deutscher Export sagt „Eigentümer“ und
  „Benutzername“, ein englischer „Owner“ und „Username“. Erkannt wird über eine
  Liste bekannter Bezeichnungen und, wenn die nicht greift, über die Position im
  Datensatz. Bei einer Party mit Exporten aus verschiedenen Sprachen ist das kein
  Randfall.
* **Die Kodierung ist kaputt.** Instagram schreibt UTF-8, als wäre es Latin-1;
  „Grüße“ kommt als „GrÃ¼ÃŸe“ an, japanische Zeichen als drei Buchstaben Kauderwelsch.
  Das wird zurückgerechnet — aber nur, wenn das Ergebnis gültiges UTF-8 ist,
  damit ein sauberer Text nicht zerstört wird.

## Tests

```
node test/run-tests.mjs
```

87 Prüfungen ohne Test-Framework, im Stil der Selbsttests unter `Client/tests/`:
ZIP-Leser (auch ZIP64 und unkomprimierte Einträge), die drei Export-Formate, die
Kodierungs-Reparatur, die Dreiwertigkeit der Prüfung, die Host-Freigabeliste für
Vorschaubilder, Pool- und Rundenbildung, Punktevergabe sowie die HTTP-Oberfläche
inklusive Pfad-Ausbruchsversuchen.

Liegt eine Datei `test/sample-export.zip` daneben, wird zusätzlich gegen diesen
echten Export geprüft. Sie ist über `.gitignore` ausgeschlossen — echte
Export-Archive gehören nicht ins Repository.

**Nicht abgedeckt:** die tatsächliche Anfrage an Instagram. Sie braucht das
offene Netz, und Instagram kann sein Embed-Markup jederzeit ändern. Genau
deshalb liegt die Auswertung in `lib/embed.js` getrennt vom Netzwerkteil — wenn
sich dort etwas ändert, ist eine neue Fixture in `testEmbed()` die einzige
nötige Anpassung.

## Grenzen dieser Demo

* Die Prüfung liest die öffentliche Embed-Seite. Das ist keine zugesicherte
  Schnittstelle: Instagram kann das Markup ändern oder stärker bremsen. Dann
  landen Beiträge auf `unknown` — sichtbar, nicht stillschweigend.
* Ohne Internet lässt sich nichts prüfen und nichts einbetten. Bereits geprüfte
  Beiträge bleiben im Cache, die Galerie funktioniert dann weiter, die
  Spielrunde zeigt statt des Embeds das Vorschaubild.
* Es gibt keine Sitzungsverwaltung: alle Spieler importieren an einem Gerät, und
  ein Neuladen fängt von vorn an. Das ist Absicht — nichts wird gespeichert.
* Getestet mit Exporten von 2026 (neues Format, deutschsprachig). Ältere und
  fremdsprachige Exporte sind im Code berücksichtigt, aber nicht an echten
  Dateien erprobt.

## Was als Nächstes drankommt

* **Eigene Medien aus dem Export.** Der Export enthält auch die Beiträge und
  Medien der Person selbst. Die ließen sich anzeigen, ohne Instagram zu fragen —
  aber nur, wenn die Person, von der der Originalbeitrag stammt, mitspielt.
  `likes.js` liest den Urheber-Handle bereits aus jedem Like, und `buildPool`
  kennt alle Spieler; der Abgleich ist damit vorbereitet.
* **Weitere Spielarten.** Statt reihum: alle raten gleichzeitig, jede Person auf
  dem eigenen Telefon. Oder „Wer alles?“ auf gemeinsam gelikte Beiträge.
* **Mehr aus dem Export.** Gespeicherte Beiträge (`saved_posts.json`) und
  gelikte Kommentare wären zusätzliche Fragerunden — nach denselben Regeln:
  eine Datei lesen, den Rest liegen lassen.

---

Nicht mit Instagram oder Meta verbunden. „Instagram“ ist eine Marke von Meta
Platforms, Inc.
