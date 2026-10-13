# EngineContent-Manifest unter Windows "von einem anderen Prozess verwendet" (Thema 187, Schritt 1)

Stand 13.10.2026. Diagnose auf dem Mac aus Code, echten Editor-Logs und einem
Plattform-Zeugentest. Auf Windows selbst wurde der Editor in diesem Schritt NICHT
gestartet (kein Windows-Rechner an diesem Gerät, kein Log vom betroffenen Rechner
verfügbar, Rückfrage an die Queen ergab keins). Was Windows betrifft, stützt sich
die Diagnose auf das Fehlercode-Verhalten von MSVC/Win32, das
`tests/test_engine_content_manifest_lock.cpp` auf der Windows-CI festnagelt.

## Ergebnis in einem Satz

Der Prozess, der die Datei hält, ist der Editor selbst: Beim Start laden zwei
Threads desselben Editors das Manifest gleichzeitig in denselben festen Temp-Pfad
(`<EngineContentCache>/.manifest.json.tmp` bzw. `.manifest.json.tmp.partial`), und
unter Windows scheitert das `std::filesystem::rename` des einen an einem offenen
Handle des anderen. Die Windows-Meldung für ERROR_SHARING_VIOLATION lautet "Der
Prozess kann nicht auf die Datei zugreifen, da sie von einem anderen Prozess
verwendet wird", obwohl dieser "andere Prozess" derselbe Editor ist.

## Es gibt keinen lokalen Content-Server

Der "Engine Content Server" ist ein SFTP-Host beim Webhoster
(`engineContentEndpoint()`, `src/HE_ContentSync/src/SftpClient.cpp`). Lokal
schreibt und liest nur der Editor. Damit fallen aus dem Themenvorschlag weg:

* Zombie-Prozess eines Content-Servers: es gibt keinen lokalen Server-Prozess.
* Race "Server schreibt, Editor liest": der Server schreibt nichts auf die lokale
  Platte; der Download legt die Datei selbst an.
* Eigene Instanz-Sperre der Engine: es gibt keine (keine Lock-Datei, kein
  `LockFileEx`, kein `_SH_DENY*`, kein `FILE_SHARE_*` in `src/` ausser `HiddenShell.cpp`).

## Wo das Manifest geöffnet wird und mit welchen Flags

| Stelle | Datei | Öffnen |
|---|---|---|
| `sftpGetFile` (`SftpClient.cpp:340-382`) | `.manifest.json.tmp.partial` schreiben, dann `fs::rename` → `.manifest.json.tmp` | `std::ofstream(binary\|trunc)` |
| `refreshManifestBlocking` (`EngineContentSync.cpp:79-98`) | `.manifest.json.tmp` lesen, dann `fs::remove` | `std::ifstream(binary)` |
| `writeCachedManifest` (`EngineContentSync.cpp:30-61`) | `manifest.json.tmp` → `fs::rename` → `manifest.json` (Offline-Cache) | `std::ofstream`, unter `m_manifestMutex` |
| `loadCachedManifest` (`EngineContentSync.cpp:132-149`) | `manifest.json` lesen | `std::ifstream`, unter `m_manifestMutex` |

MSVCs `basic_filebuf::open` geht über `_Fiopen` mit `_SH_DENYNO`, das heisst
`FILE_SHARE_READ | FILE_SHARE_WRITE`, **nie `FILE_SHARE_DELETE`**. MSVCs
`std::filesystem::rename` ist `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`. Ein
Umbenennen, dessen Quelle oder Ziel jemand ohne `FILE_SHARE_DELETE` offen hält,
scheitert deshalb:

* Quelle offen → `ERROR_SHARING_VIOLATION` (32), "...von einem anderen Prozess verwendet wird"
* Ziel offen → `ERROR_ACCESS_DENIED` (5), "Zugriff verweigert"

Unter macOS/Linux tauscht `rename(2)` den Verzeichniseintrag unter jedem offenen
Deskriptor aus, dieselbe Verschränkung geht dort still durch. Das ist der Grund,
warum der Fehler nur auf Windows auffiel.

Die beiden Cache-Zugriffe (`writeCachedManifest`, `loadCachedManifest`) laufen
unter `m_manifestMutex` und können sich innerhalb des Prozesses nicht treffen. Der
Download-Pfad dagegen läuft **ohne jede Sperre**.

## Die zwei gleichzeitigen Aufrufer

1. `EditorApplication::startSftpProbe()` (`EditorApplication.cpp:2191`, gestartet
   am Ende von OnInit, `:2060`): `probeSftp()` (eigene SSH-Sitzung, fasst keine
   Dateien an, `SftpProbe.cpp`), danach `refreshManifestBlocking()` (`:2214`).
2. `ProjectPreflight::run()` (`ProjectPreflight.cpp:117`, seit Commit 6574ccc4 vom
   09.10.2026): sobald ein letztes Projekt oder ein beim Start übergebenes Projekt
   da ist, setzt `requestAtStartup()` es, und der **erste Frame** startet den Job,
   der sofort `refreshManifestBlocking()` aufruft.

Beide benutzen `GlobalState::engineContentCacheDir() / ".manifest.json.tmp"`
(`EngineContentSync.cpp:79`) und `sftpGetFile` hängt daran `.partial` an
(`SftpClient.cpp:340`). Jeder Editorstart mit zuletzt geöffnetem Projekt macht also
zwei Downloads auf dieselben zwei Dateinamen.

### Beleg aus echten Starts (macOS, `out/deploy/Editor/HorizonEngine*.log`)

Jeder der vier vorhandenen Logs zeigt zwei Refreshes auf zwei Threads:

```
[08:12:22.578] OnInit complete — entering main loop
[08:12:22.668] [Main ] Project preflight: checking .../Catania.heproj
[08:12:22.995] [t06ca] EngineContent: ...webspace-host.com:22 reachable      <- Probe fertig, Probe-Thread startet seinen Refresh
[08:12:23.089] [td911] EngineContent manifest refreshed (87 entries)          <- Preflight-Thread
[08:12:23.469] [t06ca] EngineContent manifest refreshed (87 entries)          <- Probe-Thread
```

Abstände der beiden "refreshed" in den vier Logs: 380, 252, 427 und 355 ms. Eine
SSH-Sitzung bis zum Download dauert hier ~400 ms. Die Fenster liegen also direkt
aneinander. Ob sie sich treffen, hängt davon ab, wann der erste Frame kommt und wie
lange Anmeldung und Download dauern. Auf dem Mac verpassen sie sich meist knapp,
und wenn nicht, merkt es POSIX nicht.

## Wie der Fehler beim Nutzer ankommt

Die einzige Stelle auf diesem Weg, die einen Windows-Systemtext in eine sichtbare
Meldung bringt, ist `ec.message()` in `sftpGetFile`:

```
Could not finalize downloaded file '<cache>\.manifest.json.tmp': <Systemtext>
```

(`writeCachedManifest` verschluckt sein `ec`, "Could not create local file" trägt
keinen Systemtext.) Der Text landet über `m_lastManifestError` / `lastManifestError()` in:

* Startmeldung `"The EngineContent catalogue could not be read."` ... `"reading its
  manifest failed: Could not finalize downloaded file ...: Der Prozess kann nicht
  auf die Datei zugreifen, da sie von einem anderen Prozess verwendet wird."`
  (`EditorApplication.cpp:2251-2263`), wenn der Probe-Thread verliert;
* den Preflight-Dialog (`ProjectPreflight.cpp:313`, `serverDetail`), wenn der
  Preflight-Thread verliert.

### Folgefehler, die Schritt 2 mit erledigen muss

* **Falsches "kein Cache"**: Verliert der Probe-Thread, ruft er
  `loadCachedManifest()` auf, das `false` liefert, weil der Gewinner schon
  `m_manifestIsLive` gesetzt hat. Die Meldung sagt dann "There is no cached copy on
  this machine", obwohl das Manifest geladen ist, und die Engine-Ansicht wird von
  diesem Thread nicht neu gemischt.
* **`m_lastManifestError` gewinnt der Letzte**: Der Gewinner löscht ihn, der
  Verlierer setzt ihn, je nach Reihenfolge steht danach der falsche Zustand drin.
* **Auch auf POSIX nicht sauber**: Beide Threads `trunc`-öffnen dieselbe `.partial`
  und schreiben hinein; das `fs::remove(.manifest.json.tmp)` des einen kann die
  gerade umbenannte Datei des anderen löschen, bevor der sie liest (leer gelesen →
  "the manifest on the server could not be parsed"). Ein Wiederholen des Rename nur
  unter Windows reicht deshalb nicht.

## Zwei Hypothesen und wie man sie unterscheidet

1. **Thread-Race im Editor (Hauptbefund).** Strukturell nachgewiesen (zwei
   Aufrufer, gleicher Pfad, keine Sperre), zeitlich durch die Logs belegt, das
   Windows-Verhalten durch den Zeugentest. Erklärt Windows-only, "anderer Prozess"
   ohne zweiten Editor und das Auftreten beim Start.
2. **Virenscanner/Indexer (Nebenkandidat).** Defender und der Suchindexer öffnen
   frisch geschlossene Dateien kurz. Halten sie dabei kein `FILE_SHARE_DELETE`,
   scheitert dasselbe `MoveFileExW` auch ohne zweiten Thread. Das ist von Git,
   Cargo und npm bekannt. Ausschliessen lässt es sich nur auf dem betroffenen
   Rechner.

**Erster Diskriminator: welcher Build.** Der Preflight (Commit 6574ccc4, 09.10.2026)
steht auf `release/0.7.0`, aber (Stand 13.10.) **nicht** auf `main`. Ein Editor aus
`main` oder aus einem Release vor 0.7.0 hat nur einen Aufrufer beim Start. Dort
ist die Race unmöglich und Hypothese 2 rückt nach vorn. Die Race gilt nur für
Builds ab dem 09.10. mit dem Preflight.

**Diskriminator im Windows-Log** (`HorizonEngine.log` neben `HorizonEditor.exe`,
nach Selbst-Update auch `HorizonEngine.1.log`):

```
findstr /C:"Could not finalize downloaded file" /C:"Could not fetch EngineContent manifest" ^
        /C:"EngineContent manifest refreshed" /C:"Project preflight: checking" HorizonEngine*.log
```

* Fehlerpfad endet auf `.manifest.json.tmp` UND innerhalb ~1 s steht ein
  `manifest refreshed` eines **anderen** Thread-Kürzels → Race bestätigt.
* Fehler ohne zweiten Refresh in der Nähe, oder auf einem anderen Pfad (einem
  Asset-Download, `manifest.json`) → Scanner/Indexer. Dann mit `handle.exe
  -a .manifest.json` bzw. Process Monitor (Filter Path contains `.manifest.json`,
  Result SHARING VIOLATION) den Halter bestimmen.

Mit `handle.exe` würde man im Race-Fall `HorizonEditor.exe` selbst als Halter von
`.manifest.json.tmp.partial` sehen. Das Fenster ist Millisekunden kurz, deshalb ist
Process Monitor dafür besser geeignet als `handle.exe`.

## Zeugentest

`tests/test_engine_content_manifest_lock.cpp` spielt die zwei Stellen, an denen
sich die Threads treffen, deterministisch nach, mit einem zweiten `std::fstream`
als zweitem Thread:

* `.partial` umbenennen, während ein `ofstream` darauf offen ist → Windows erwartet 32, POSIX Erfolg;
* auf `.manifest.json.tmp` umbenennen, während ein `ifstream` darauf offen ist → Windows erwartet 5, POSIX Erfolg.

Auf dem Mac (eigenständig gebaut mit doctest) 2/2 Fälle grün. Die Windows-Hälfte
bestätigt die Windows-CI: grün heisst, MSVC verhält sich wie oben beschrieben.

## Was Schritt 2 daraus macht (Vorschlag, nicht umgesetzt)

* Den Download-Teil von `refreshManifestBlocking()` serialisieren (eigene Mutex
  über Download + Lesen + Entfernen) oder je Aufruf einen eindeutigen Temp-Namen
  verwenden. Besser beides: die Mutex verhindert auch den doppelten Download, ein
  zweiter Aufrufer kann das gerade geholte Manifest übernehmen statt neu zu laden.
* `loadCachedManifest()`-Fallback nicht als "kein Cache" melden, wenn das Manifest
  in dieser Sitzung schon live ist (`haveManifest` = Manifest vorhanden, nicht
  "dieser Aufruf hat geladen").
* Gegen Hypothese 2 zusätzlich unter Windows ein kurzes Wiederholen von
  `rename` bei 32/5 (wenige Versuche, ~50-100 ms), wie es Git for Windows macht.
  macOS/Linux bleiben davon unberührt.

## Was Schritt 2 umgesetzt hat

* **Serialisiert statt eindeutiger Namen.** `EngineContentSync::m_refreshMutex`
  hält einen ganzen Refresh, vom Download über Lesen und `fs::remove` bis zur
  Übernahme (`refreshManifestWith`, `EngineContentSync.cpp`). Damit treffen sich
  die zwei Startaufrufer nie mehr auf `.manifest.json.tmp(.partial)`, weder unter
  Windows (Rename gegen offenes Handle) noch unter POSIX (`remove` des einen löscht
  die Datei des anderen). Der feste Temp-Name bleibt: eindeutige Namen hätten nur
  Absturzreste zum Aufräumen gebracht. Nicht `m_manifestMutex`, denn den hält
  `manifest()` auf dem UI-Thread, und der darf nie aufs Netz warten.
* **Kein Zusammenlegen der zwei Aufrufe.** Der zweite Aufrufer lädt nach dem ersten
  selbst neu (~400 ms auf einem Worker-Thread). Ihm das Ergebnis des ersten zu
  geben, könnte einem Refresh nach einem Publish den Katalog von davor liefern.
* **Windows-Rename mit kurzem Wiederholen** (`src/HE_ContentSync/src/CsRename.h`,
  `detail::renameReplacing`): nur unter `_WIN32`, nur bei 32/5, fünf Versuche mit
  20/40/80/160/320 ms. Jeder Versuch schreibt eine WARN-Zeile ("Renaming '...'
  failed (32: ...) — retry n of 5"); taucht sie in einem Windows-Log auf, ist das
  ein Fremdhalter (Hypothese 2), denn die Eigen-Race gibt es nicht mehr. Benutzt
  von `sftpGetFile` (Manifest und jeder Asset-Download) und `writeCachedManifest`.
  Unter macOS/Linux ist es genau ein `fs::rename` wie vorher.
* **"Kein Cache" gelogen behoben:** `loadCachedManifest()` gibt `true` zurück,
  wenn das Manifest in dieser Sitzung schon live ist (ohne neu zu laden). Beide
  Aufrufer werten es als "Manifest vorhanden".
* **Testnaht für Schritt 3:** `refreshManifestWith(fetch)` ist
  `refreshManifestBlocking()` mit austauschbarem Download; alles danach ist der
  echte Code. Ein Test kann so zwei Threads gegen das echte EngineContentSync
  laufen lassen, ohne Server. `engineContentCacheDir()` folgt `HOME`
  (macOS/Linux) bzw. `APPDATA` (Windows) und wird einmal pro Prozess aufgelöst.

Bekannte Restkante: Wiederholt `writeCachedManifest` unter Windows (Fremdhalter
auf `manifest.json`), wartet es bis ~0,6 s unter `m_manifestMutex`; ein
`manifest()` des UI-Threads wartet dann mit. Nur im Fehlerfall, vorher scheiterte
derselbe Fall stattdessen.

## Regressionstest (Schritt 3)

`tests/test_engine_content_manifest_refresh.cpp` treibt den echten
`EngineContentSync`-Code über `refreshManifestWith`; nur der SFTP-Download ist
ersetzt, durch eine Nachbildung von `sftpGetFile` (`ofstream` auf `.partial`,
in zwei Hälften geschrieben, dazwischen 3 ms offen, dann wie das Original
`detail::renameReplacing`). Drei Fälle:

1. **Die zwei Startaufrufer** (SFTP-Probe und Projekt-Preflight): 100 Runden, je
   zwei Threads, die gleichzeitig losgehen. Erwartet: kein Refresh scheitert, nie
   laufen zwei Downloads zugleich, `lastManifestError()` leer,
   `loadCachedManifest()` sagt `true`, das `manifest.json` auf der Platte hat den
   neuen Inhalt, kein `.manifest.json.tmp` bleibt liegen. Das ist der
   ursprüngliche Lock-Konflikt.
2. **Start nach einem gekillten Editor:** garbage in `.manifest.json.tmp.partial`,
   `.manifest.json.tmp` und `manifest.json.tmp` (was ein Kill mitten im Download
   oder im Cache-Schreiben hinterlässt), dann ein Refresh. Erwartet: Erfolg, alle
   drei Reste weg. Dazu ein Refresh bei unerreichbarem Server: scheitert, behält
   das vorige Manifest im Speicher und auf der Platte.
3. **Fremdhalter für einen Moment** (Defender, Indexer): ein zweiter Thread hält
   60 ms lang die Quelle eines `detail::renameReplacing` bzw. das `manifest.json`,
   während ein Refresh das neue darüber schreibt. Erwartet überall: Erfolg und der
   neue Inhalt auf der Platte. Unter Windows zusätzlich mindestens eine
   "Renaming '...' retry"-WARN-Zeile im Log-Ring (der Retry-Pfad lief wirklich),
   unter macOS/Linux keine. Das ist der einzige Test, der den Windows-Retry ausführt.

**Isolation.** `engineContentCacheDir()` löst sich einmal pro Prozess aus `HOME`
(macOS), `XDG_CONFIG_HOME` (Linux) bzw. `APPDATA` (Windows) auf und enthält auf
einem Entwicklerrechner den echten Katalog. ctest startet die Datei deshalb mit
allen dreien im eigenen Temp-Ordner und setzt `HE_TEST_ENGINE_CONTENT_HOME`
(`tests/CMakeLists.txt`, nach der Registrierungsschleife). Ohne die Variable (ein
nackter `he_tests`-Lauf) tun die Fälle nichts und sagen das per MESSAGE; mit ihr
bricht ein Fall ab, wenn der Cache-Ordner trotzdem ausserhalb liegt. Gebaut wird
die Datei nur, wenn es `HorizonContentSync` gibt (libssh2 gefunden).

**Gegenprobe auf macOS (13.10., MacBook Air, auf Akku):** eigenständig mit doctest
gegen die gebaute `libHorizonContentSync.dylib`: 3/3 Fälle, 39/39 Assertions grün,
fünfmal hintereinander, ~1,2 s. Ohne Isolation: 3 Fälle "not run", das echte
`manifest.json` unverändert (mtime/Grösse verglichen). **Negativkontrolle:** mit
auskommentiertem `lock_guard(m_refreshMutex)` scheitern 100 von 200 Refreshes
(`rename: 2 No such file or directory`, also auch unter POSIX) und zwei Downloads
laufen gleichzeitig; Quelle danach wiederhergestellt. Der Test ist damit kein
Windows-only-Test: er fällt auf jeder Plattform, sobald die Refreshes wieder
überlappen. `he_tests` selbst wurde lokal nicht gebaut; die Windows/Linux-Hälfte
liefert die CI-Matrix (`ci.yml` führt jedes `tests/test_*.cpp` per ctest aus).

Falle beim Schreiben: `sleep_for(3 ms)` wurde auf dem Mac im Akkubetrieb zu ~90 ms
(Timer-Coalescing), 200 Refreshes dauerten 18 s. Die Pause ist deshalb eine
`yield`-Schleife gegen `steady_clock`.

### Manuelle Windows-Abnahme (Editor, nicht automatisierbar)

Die drei Startszenarien aus dem Thema brauchen einen echten Windows-Editor mit
konfiguriertem EngineContent-Endpunkt (`cmake/EngineContentCredentials.cmake`) und
ein Projekt, damit der Preflight läuft (Build ab release/0.7.0 mit diesem Zweig).
Log: `HorizonEngine.log` neben der exe (ist der Ordner nicht beschreibbar, nennt
eine Zeile "Log file: ..." den Ausweichort). Cache: `%APPDATA%\HorizonEngine\EngineContentCache`.

1. **Normaler Start** mit Projekt. Erwartet: zweimal "EngineContent manifest
   refreshed (N entries)" (Probe und Preflight, nacheinander), keine Meldung "The
   EngineContent catalogue could not be read", kein Preflight-Serverfehler, die
   Engine-Assets im Content Browser.
2. **Schneller Neustart:** Editor schliessen und sofort wieder starten, dreimal
   hintereinander. Erwartet wie 1. Taucht "Renaming '...' failed (32|5: ...) —
   retry n of 5" auf, hielt jemand anderes die Datei (Defender/Indexer oder der
   noch nicht ganz beendete vorige Editor); solange danach "refreshed" folgt, hat
   der Retry gegriffen. Tritt "retry 5 of 5" mit Fehlermeldung auf: Prozess mit
   Handle suchen (`handle.exe manifest` aus Sysinternals) und hier eintragen.
3. **Start nach Absturz:** Editor in den ersten Sekunden des Starts, bevor
   "EngineContent manifest refreshed" im Log steht, im Task-Manager beenden, in `EngineContentCache` nachsehen, ob
   `.manifest.json.tmp*` liegen geblieben ist, dann normal starten. Erwartet wie
   1., die Reste sind danach weg. Einen eigenen Content-Server-Prozess, der
   abstürzen könnte, gibt es nicht (siehe "Es gibt keinen lokalen Content-Server").
4. **Server nicht erreichbar** (Netz aus): Start meldet "The EngineContent server
   could not be reached." mit "Working from the catalogue of the last successful
   connection", der Content Browser zeigt die Engine-Assets aus dem Cache-Manifest.

Ergebnis je Szenario mit Logauszug hier nachtragen. Stand 13.10.: noch nicht
durchgeführt (kein Windows-Rechner an diesem Gerät; die Queen organisiert einen
Lauf auf NN-WS03).

## Build und Tests (Schritt 4)

Stand 371e2462, MacBook Air (Release, Ninja `-j8`, `HE_BUILD_TESTS`/shaderc ON,
`out/build/release` im Worktree), 13.10.:

- **Vollbau** 930/930 Schritte, rc 0, Editor gelinkt. Keine Warnung aus den
  geänderten Dateien (nur die bekannten Vendor-/Deprecation-Warnungen).
- **Gezielt:** `test_engine_content_manifest_refresh` 3/3 Fälle, 39 Assertions (also
  wirklich gelaufen, nicht "not run"); `test_engine_content_manifest_lock` 2/2, 6;
  `test_contentmanager` 85/85; die drei Self-Update-Suiten (SftpClient-Mitnutzer) 30/30.
- **Volle Suite** (`ctest -j4`, `HE_CONFIG_DIR` ungesetzt, Wegwerf-`HOME`): 280
  Einträge, 278 bestanden, 0 rot, 2 übersprungen (`runtime_size_app_basic/advanced`:
  kein `out/deploy/App*` zum Messen, unabhängig von diesem Thema), 548 s.
- **Negativkontrolle in `he_tests`** (nicht nur im Einzel-Build von Schritt 3):
  `lock_guard(m_refreshMutex)` auskommentiert → `test_engine_content_manifest_refresh`
  rot, 103 von 200 Refreshes scheitern (`rename: 2 No such file or directory`),
  `maxInFetch == 2`, `lastManifestError()` gesetzt. Quelle zurückgesetzt, neu gebaut,
  wieder grün.
- **Editor-Start** (versteckt, `HE_EXIT_AFTER_FRAMES=120`, Wegwerf-`HOME`): 120 Frames,
  sauberes Herunterfahren, 0 Fehler. Der Manifest-Pfad läuft dabei NICHT: dieser
  Build hat kein EngineContent-Konto einkompiliert (`probe.configured == false`).
- **CI** Run 38379510241 (371e2462): Windows, macOS, Linux, Linux-lavapipe grün; im
  Windows-Job `test_engine_content_manifest_lock` 0,12 s und
  `test_engine_content_manifest_refresh` 1,65 s bestanden. ctest zeigt bestandene
  Ausgabe nicht, die Fallzahl unter Windows ist also nicht sichtbar. Die Laufzeit
  spricht dafür, dass die Fälle liefen (ohne Isolation kehren sie sofort zurück),
  ein Beweis ist sie nicht.

Was offen bleibt: (1) Die Lock-Quelle beim Nutzer ist aus Code und
Fehlercode-Semantik erschlossen, nicht an einem Log der betroffenen Maschine
gesehen. Welcher Build dort lief, ist ungeklärt (die Startup-Race gibt es nur mit
dem Projekt-Preflight). (2) Die manuelle Windows-Abnahme oben steht aus. (3)
`loadCachedManifest()` antwortet jetzt auf allen Plattformen `true`, wenn diese
Sitzung schon einen Live-Katalog hat. Das ändert macOS/Linux im Randfall "erster
Refresh ok, späterer scheitert" (vorher fälschlich "kein Katalog"); gewollt, aber
kein reiner Windows-Fix. (4) Schneller Neustart über Prozessgrenzen (alter Editor
lädt noch) deckt nur der Windows-Rename-Retry (~0,6 s) ab, nicht der Mutex.
