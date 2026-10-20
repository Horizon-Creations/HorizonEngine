# ContentSync Slice 1 – src/HE_ContentSync (alle 15 Dateien: Header, EngineContentManifest/Publish/Sync, SftpClient/Credentials/Probe, CsRename)

## Zusammenfassung
Das Modul ist klein (1922 Zeilen), editor-only und läuft komplett auf Worker-Threads; es gibt keinen Frame-Hot-Path. Der Code ist gut kommentiert und im Kern sauber (RAII-`Session`, Temp-then-Rename, Lock-Disziplin um den Failure-Listener). Die relevanten Schwächen liegen bei I/O und Robustheit: eine neue TCP+SSH+SFTP-Session pro Operation (Publish/Rebuild/Download-Queue zahlen dadurch pro Datei einen oder zwei Handshakes), kein libssh2-Timeout (ein hängender Server blockiert den einzigen Download-Worker dauerhaft), Datei-I/O unter `m_manifestMutex`, fehlende Integritätsprüfungen bei Down-/Upload und ein nicht abgefangener `json::type_error` im Manifest-Parser. Dazu kommen Duplikate zwischen Publish und Rebuild. Funde: 2 Hoch, 6 Mittel, 8 Niedrig. Es gibt keine Tests für `SftpClient`, `EngineContentPublish` und die Download-Queue; vorhanden sind nur `tests/test_engine_content_manifest_refresh.cpp` und `tests/test_engine_content_manifest_lock.cpp`.

---

### CSY-101 – Neue SSH-Session pro Operation: Publish, Rebuild und Download-Queue zahlen pro Datei 1–2 Handshakes
- **ID**: CSY-101
- **Kategorie**: D (E)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/SftpClient.cpp:114-195` (`Session`), `:314-318`, `:393-399`, `:435-438`; Aufrufer `src/EngineContentPublish.cpp:122-135`, `:218`; `src/EngineContentSync.cpp:304`
- **Problem**: Jede öffentliche Funktion öffnet TCP-Connect, SSH-Handshake (Schlüsselaustausch), Passwort-Auth und SFTP-Init und baut alles wieder ab. Im Publish-Loop läuft pro geänderter Datei erst `sftpEnsureRemoteDir` (eigene Session, auch wenn das Verzeichnis schon im Vorlauf angelegt wurde) und dann `sftpPutFile` (zweite Session). `rebuildManifestFromServerBlocking` und `drainQueue` laden je Datei mit einer frischen Session. Bei N Dateien ergeben sich 2N (Publish) bzw. N (Rebuild/Download) Handshakes, jeweils mehrere Round-Trips plus der Auth-Delay mancher Hoster. Der Header begründet das mit „Thread-Sicherheit ohne Lock“ und „one-file-at-a-time“. Das gilt aber nicht für Publish/Rebuild, die explizit viele Dateien am Stück in einem einzigen Worker verarbeiten.
- **Code-Auszug**:
```cpp
// EngineContentPublish.cpp:122-135 (pro geänderter Datei)
const SftpResult mk  = sftpEnsureRemoteDir(endpoint, relDir.generic_string()); // Session #1
...
const SftpResult put = sftpPutFile(endpoint, localAbs, entry.relativePath);     // Session #2
```
- **Lösung**: (1) Öffentliche Klasse `SftpConnection` (RAII, nicht thread-safe, ein Besitzer) mit `open(endpoint)`, `getFile`, `putFile`, `ensureDir`, `listTree`. Die bestehenden freien Funktionen bleiben als dünne Wrapper (`SftpConnection c; c.open(); return c.getFile(...)`), damit Aufrufer und Tests unverändert bleiben. (2) `publishEngineContentBlocking`, `rebuildManifestFromServerBlocking` und `drainQueue` (eine Verbindung pro Batch, bei Fehler/Transient neu öffnen) halten eine Connection über den ganzen Lauf. (3) Im Publish einen `std::unordered_set<std::string>` bereits angelegter Verzeichnisse führen und `ensureDir` nur beim ersten Mal pro Verzeichnis aufrufen. Das spart auch bei Verbindungsabbruch nichts, ist aber trivial.
- **Erwarteter Effekt**: Schätzung: Publish/Rebuild mit mehreren hundert Dateien von Minuten auf einen Bruchteil, da pro Handshake typisch 0,2–1 s anfallen (Annahme, nicht gemessen). Download-Queue: gleiche Ersparnis pro Datei bei Szenen mit vielen Passive-Referenzen.
- **Risiko / Verifikation**: Lange offene Verbindung kann vom Server getrennt werden (Idle-Timeout, Max-Requests pro Session). Daher Reconnect-on-error im Wrapper. Per Fake-Server oder libssh2-Testserver prüfen; die bestehende Entscheidung „ein Fehler ohne Reuse“ soll nicht regressieren. Messen: Zeit für Publish von 200 Dateien vorher/nachher.

### CSY-102 – Kein libssh2-Timeout: ein stehender Server blockiert den Download-Worker für immer
- **ID**: CSY-102
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/SftpClient.cpp:137-143` (Session-Init), `:145`, `:152`, `:163`, Lese-/Schreibschleifen `:354-371`, `:411-426`; Folge in `src/EngineContentSync.cpp:272-349`
- **Problem**: Nur der TCP-Connect hat ein Zeitlimit (`kConnectTimeoutMs`). Danach läuft die Session mit `libssh2_session_set_blocking(ssh, 1)` ohne `libssh2_session_set_timeout` (kein Treffer für `set_timeout` im Modul). Ein Server, der nach dem Connect nicht mehr antwortet (Handshake, Auth, `sftp_read`), lässt den Aufruf unbegrenzt blockieren. Für `drainQueue` heißt das: `m_draining` bleibt `true`, jede weitere `enqueueDownload` hängt nur an, nie wieder wird ein Download gestartet, und alle zugehörigen Completion-Callbacks feuern nie. Der Doc-Kommentar in `EngineContentSync.h:148-155` nennt genau diese Folge („wedges that asset as permanently-pending“). Ebenso hängt `probeSftp` am Startup.
- **Code-Auszug**:
```cpp
ssh = libssh2_session_init();
...
libssh2_session_set_blocking(ssh, 1);   // kein libssh2_session_set_timeout
```
- **Lösung**: Direkt nach `libssh2_session_init()` `libssh2_session_set_timeout(ssh, 30'000);` (ms, ab libssh2 1.2.9; Wert als Konstante neben `kConnectTimeoutMs`). Ein Timeout liefert dann `LIBSSH2_ERROR_TIMEOUT` und läuft durch den vorhandenen Fehlerpfad. Beim Handshake-Timeout `transient = true` setzen (ist dort schon so). Optional `libssh2_keepalive_config` für lange Uploads.
- **Erwarteter Effekt**: Kein unbegrenztes Hängen mehr; Downloads/Probe scheitern deterministisch nach ≤ Timeout und lösen die Callbacks aus. Eine Ein-Zeilen-Änderung.
- **Risiko / Verifikation**: Bei sehr langsamen Verbindungen und großen Dateien darf das Timeout nur pro Netzwerk-Operation gelten (so ist die libssh2-Semantik), nicht pro Datei; Wert nicht zu klein wählen. Test: lokaler TCP-Server, der annimmt und schweigt, `sftpTestConnection` muss nach dem Timeout `ok=false` liefern.

### CSY-103 – Datei-I/O (inkl. Windows-Rename-Retry bis ~620 ms) unter `m_manifestMutex`
- **ID**: CSY-103
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/EngineContentSync.cpp:138-152`, `:167-188`, `:190-218`, `src/CsRename.h:36-49`
- **Problem**: `writeCachedManifest` (Serialisieren, Datei schreiben, `flush`, `renameReplacing` mit bis zu 5 Retries à 20…320 ms auf Windows) läuft unter `m_manifestMutex`, ebenso das Lesen und Parsen der Cache-Datei in `loadCachedManifest`. `manifest()` wird laut Kommentar vom UI-Thread aufgerufen (`:99`, „must never wait for the network“), weitere Aufrufer: `EditorApplication.cpp:2524`, `:3378` (Haupt-Thread). Der UI-Thread kann also durch Plattenzugriff oder Defender-Retry bis zu mehreren hundert ms blockieren. Die Absicht (Zeile 144-146: Datei und Speicher dürfen sich nicht widersprechen) lässt sich ohne den Lock über die I/O erreichen.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lock(m_manifestMutex);
...
if (!writeCachedManifest(m_manifest))   // Datei-I/O + Rename-Retries unter dem Lock
```
- **Lösung**: Zweiten Mutex `m_cacheFileMutex` einführen (Reihenfolge: erst `m_cacheFileMutex`, dann kurz `m_manifestMutex`). Ablauf: `lock(cacheFile)`; `{ lock(manifest); adoptieren/ändern; snapshot = m_manifest; }`; `writeCachedManifest(snapshot)`. Da `noteLocalCopyRemoved` und `refreshManifestWith` beide durch `m_cacheFileMutex` serialisiert sind, bleibt die Schreibreihenfolge identisch zur Speicherreihenfolge. `loadCachedManifest` analog: Datei außerhalb des Manifest-Locks lesen/parsen, nur das Zuweisen (mit erneuter `m_manifestIsLive`-Prüfung) unter dem Lock.
- **Erwarteter Effekt**: UI-Thread wartet in `manifest()` nie mehr auf Platte oder Retry-Sleeps (Schätzung: Ausreißer von 10–600 ms entfallen, nur auf Windows/Defender relevant).
- **Risiko / Verifikation**: Lock-Order sauber halten (`m_refreshMutex` → `m_cacheFileMutex` → `m_manifestMutex`). `test_engine_content_manifest_lock.cpp` und `_refresh.cpp` decken die Nebenläufigkeit ab und müssen weiter laufen; ein Test mit parallelem `noteLocalCopyRemoved` + Refresh ergänzen.

### CSY-104 – Keine Integritätsprüfung bei Down-/Upload; Upload ist nicht atomar
- **ID**: CSY-104
- **Kategorie**: F (Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/SftpClient.cpp:354-371` (Download-Loop), `:383-390`, `:409-432` (Upload)
- **Problem**: (a) Download: `n == 0` gilt als EOF und wird ohne Vergleich mit `totalBytes` (Zeile 328-334) als Erfolg gewertet und per Rename als „komplette“ Datei freigegeben. Bei einem vorzeitigen EOF ist die Datei abgeschnitten. Die Manifest-`size`/`contentHash` stehen dem Aufrufer zur Verfügung, werden aber nie geprüft. (b) Upload: Ein Lesefehler der lokalen Datei beendet `while (in)` wie ein EOF, `failed` bleibt `false`, die gekürzte Datei meldet `ok`. Das Ergebnis von `libssh2_sftp_close_handle` (hier wird der Server-Flush bestätigt) wird ignoriert. (c) Upload öffnet das Ziel direkt mit `LIBSSH2_FXF_TRUNC`; ein Client, der während eines Publish lädt, kann eine halbe Datei sehen. Das Manifest-zuletzt-Argument (Publish.cpp:145-148) schützt nur neue Dateien, nicht überschriebene.
- **Code-Auszug**:
```cpp
if (n == 0) break; // EOF          // kein doneBytes == totalBytes Check
...
while (in) { in.read(...); const auto got = in.gcount(); if (got <= 0) break; ... }
libssh2_sftp_close_handle(handle);   // Rückgabewert ignoriert
```
- **Lösung**: Download: nach der Schleife `if (!failed && totalBytes && doneBytes != totalBytes) failed = true` mit Meldung „truncated“. Upload: nach der Schleife `if (in.bad()) failed = true`; `close_handle`-Rückgabe prüfen. Upload zuerst nach `<remote>.partial`, danach `libssh2_sftp_rename_ex` mit `OVERWRITE|ATOMIC` (nicht alle Server atomar; dann Fallback). Optional im Aufrufer `size` aus dem Manifest vergleichen.
- **Erwarteter Effekt**: Keine Performance-Änderung; verhindert stille Korruptionen im Engine-Content-Cache.
- **Risiko / Verifikation**: Rename-Support/Overwrite-Semantik unterscheidet sich je SFTP-Server; per Fallback abfangen. Verhalten bei Dateien, für die der Server keine Größe meldet (`totalBytes == 0`), bleibt unverändert.

### CSY-105 – Temp-Dateien als Umweg: SFTP-Byte-API fehlt, Daten werden mehrfach kopiert
- **ID**: CSY-105
- **Kategorie**: C (A)
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/EngineContentPublish.cpp:40-46`, `:104-109`, `:149-156`, `:217-228`, `:246-253`; `src/EngineContentSync.cpp:103-122`, `:176-179`
- **Problem**: Das Manifest und die `.hasset`-Scans der Rebuild-Funktion gehen immer über eine Festplatten-Temp-Datei: remote → `.partial` → Rename → `readWholeFile` (ifstream → ostringstream → `ss.str()`: zwei Kopien) → `remove`. Im Rebuild dann nochmal `std::string` → `std::vector<uint8_t>` (Zeile 228, dritte Kopie) und das für jede Datei auf dem Server. Der Upload der Manifest-Datei schreibt `serializeManifest(...)` erst auf Platte, dann liest ihn `sftpPutFile` wieder. Die festen Temp-Namen (`engine_content_rebuild_scan.tmp`, `engine_content_manifest.json.tmp` in `temp_directory_path`) kollidieren, wenn Publish und Rebuild (oder zwei Editoren auf einer Maschine) gleichzeitig laufen; `fs::temp_directory_path(ec)` kann bei Fehler zudem einen leeren Pfad liefern, ohne dass `ec` geprüft wird. Der Rename-Retry (`CsRename.h`) existiert gerade wegen solcher Temp-Rennen.
- **Code-Auszug**:
```cpp
const std::string bytesStr = readWholeFile(tmp);
...
const std::vector<uint8_t> bytes(bytesStr.begin(), bytesStr.end());
```
- **Lösung**: `SftpResult sftpGetBytes(endpoint, remoteRel, std::vector<uint8_t>& out)` und `sftpPutBytes(endpoint, data, remoteRel)` ergänzen (gemeinsamer Kern mit `sftpGetFile`/`sftpPutFile`, Quelle/Senke als Template- oder Lambda-Parameter). Damit entfallen `readWholeFile`, alle festen Temp-Namen und der Temp-Datei-Umweg in Publish (Remote-Manifest), Rebuild (pro Datei) und `refreshManifestWith` (dort bleibt das Cache-Schreiben mit temp-then-rename). Das vereinfacht außerdem die Rename-Konkurrenz aus Thema 187.
- **Erwarteter Effekt**: Schätzung: pro Rebuild-Datei eine Plattenrunde (write + read + remove) und zwei Kopien weniger; deutlich einfacherer Code (~40 Zeilen weniger).
- **Risiko / Verifikation**: `refreshManifestWith(ManifestFetchFn)` ist die Test-Naht (`ManifestFetchFn` erhält einen Pfad); bei Änderung auf Bytes die Tests `test_engine_content_manifest_refresh.cpp`/`_lock.cpp` anpassen oder den Pfad-Fetch dort belassen. Speicher: `.hasset` ganz im RAM ist bereits heute so.

### CSY-106 – Lokaler Publish-Scan liest jede Datei per `istreambuf_iterator`
- **ID**: CSY-106
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/EngineContentPublish.cpp:79-82`
- **Problem**: `std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())` liest zeichenweise über den Streambuf, ohne die Größe vorab zu kennen (mehrfaches Umallozieren, ~Faktor 5–20 langsamer als ein Block-Read, abhängig von der Standardbibliothek). Der Scan liest bei jedem Publish **alle** `.hasset`-Dateien, um UUID und Hash zu bestimmen.
- **Code-Auszug**:
```cpp
std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
```
- **Lösung**: `f.seekg(0, end)`/`tellg` bzw. `fs::file_size(p)`, `bytes.resize(size)`, `f.read(reinterpret_cast<char*>(bytes.data()), size)`. Als gemeinsamer Helper `readFileBytes(path, vector&)` (ersetzt auch `readWholeFile`).
- **Erwarteter Effekt**: Schätzung: Scan-Zeit bei großem EngineContent-Ordner (GB) von dominant CPU-bound auf Platten-bound.
- **Risiko / Verifikation**: Verhalten identisch; bei Lesefehlern (`gcount() != size`) Datei überspringen wie bisher bei `!f`. Messen mit realem EngineContent-Ordner.

### CSY-107 – `parseManifest` fängt nur `parse_error`; falsche Feldtypen werfen `type_error`
- **ID**: CSY-107
- **Kategorie**: F (Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/EngineContentManifest.cpp:43-77`
- **Problem**: Nur `json::parse_error` wird abgefangen. `e.value("path", "")` wirft `json::type_error`, wenn `path` kein String ist (gleiches gilt für `hash`/`size`/`mtime`, `u.value("hi", …)` bei `uuid` ≠ Objekt). Die Funktion ist dokumentiert als „bool“-Parser (`ok/failed`); der Aufrufer in `refreshManifestWith` (Zeile 125) und `loadCachedManifest` (Zeile 182) rechnet nur mit `false`. Eine beschädigte/gefälschte `manifest.json` auf dem Server (oder ein halbgeschriebener Cache) lässt die Exception durch den Worker-Thread laufen (`std::terminate` im Job, je nach `ThreadPool`) – dasselbe Argument wie das `keeping previous manifest`-Verhalten.
- **Code-Auszug**:
```cpp
catch (const json::parse_error&) { return false; }
...
entry.relativePath = e.value("path", "");   // type_error möglich
```
- **Lösung**: Die gesamte Funktion in `try { … } catch (const json::exception&) { return false; }` fassen (ein Handler statt zwei), `json::parse(text, nullptr, /*allow_exceptions*/ false)` plus `is_discarded()` als Alternative nur für den Parse-Teil. Die Tolerantlogik für fehlende Felder bleibt.
- **Erwarteter Effekt**: Kein Performance-Effekt; kein Absturz bei kaputtem Manifest.
- **Risiko / Verifikation**: Test in `test_engine_content_manifest_refresh.cpp`: `{"entries":[{"path":5}]}`, `{"entries":[{"path":"a","uuid":3}]}` → `false`, kein Throw, `out` unverändert.

### CSY-108 – Publish ersetzt das Manifest durch eine reine `.hasset`-Liste und verwirft damit Raw-Einträge
- **ID**: CSY-108
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/EngineContentPublish.cpp:77`, `:150-154` vs. `:201-212` (Rebuild)
- **Problem**: Der Publish-Scan nimmt nur `.hasset` auf (`extension() != ".hasset"` → `continue`) und lädt `serializeManifest(local)` als neues `manifest.json` hoch. Raw-Einträge, die `rebuildManifestFromServerBlocking` angelegt hat (z. B. Roh-WAVs), verschwinden beim nächsten Publish aus dem Manifest, auch wenn die Dateien auf dem Server bleiben. Nicht-geänderte Dateien (`unchanged`) sind unkritisch, aber das Verhalten ist asymmetrisch zu Rebuild und im Header (`EngineContentPublish.h`) nicht erwähnt. Unklar, ob gewollt.
- **Code-Auszug**:
```cpp
if (!p.is_regular_file(fileEc) || p.path().extension() != ".hasset") continue;
...
out << serializeManifest(local);
```
- **Lösung**: Entscheidung dokumentieren; falls ungewollt: Raw-Einträge aus `remote` (uuid leer) in `local` übernehmen, bevor das Manifest hochgeladen wird (`for (auto& e : remote.entries) if (e.uuid == HE::UUID{} && !local.findByPath(e.relativePath)) local.entries.push_back(e);`).
- **Erwarteter Effekt**: Keine Performance; konsistentes Manifest.
- **Risiko / Verifikation**: Ändert beobachtbares Manifest; mit dem Verantwortlichen klären. Test: Remote-Manifest mit Raw-Eintrag + Publish → Raw-Eintrag bleibt.

### CSY-109 – `drainQueue`: dreifacher „aktiv“-Zustand, Doppelzählung in `totalInBatch`, O(n) `erase(begin)`
- **ID**: CSY-109
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/EngineContentSync.cpp:231-270` (enqueue), `:272-349` (drain), `include/ContentSync/EngineContentSync.h:189-198`
- **Problem**: „Es läuft etwas“ steckt in drei Variablen (`m_draining`, `m_status.active`, `m_current.relativePath.empty()`), die an mehreren Stellen synchron gehalten werden müssen. Folge: Nachdem Job A fertig ist, aber Job B noch in `m_pending` liegt, bleibt `m_status.active` während der Callback-Phase `true` (Zeile 346), obwohl nichts läuft. Ein `enqueueDownload(C)` in diesem Fenster rechnet `completed + pending.size() + (active ? 1 : 0)` (Zeile 260) = 1 + 2 + 1 = 4 statt 3, und `totalInBatch` wird erst beim nächsten Enqueue neu berechnet. Das ist genau der „Phantom in flight“-Fall, den der Kommentar in Zeile 321-328 verhindern will. Der Fortschrittsbalken erreicht dann nicht 100 %, bis die Batch endet. Außerdem: `m_pending.erase(m_pending.begin())` (O(n) pro Job) und die lineare Duplikat-Suche in Zeile 242 machen einen Batch von n Jobs O(n²) (n ≈ Hunderte, praktisch vernachlässigbar). Der Kommentar in Zeile 280-283 ist selbstkorrigierend und schwer lesbar.
- **Code-Auszug**:
```cpp
m_status.totalInBatch = m_status.completedInBatch + m_pending.size() + (m_status.active ? 1 : 0);
...
if (m_pending.empty()) m_status.active = false;
```
- **Lösung**: `m_pending` als `std::deque<Job>` (`pop_front`). „In flight“ ausschließlich aus `!m_current.relativePath.empty()` ableiten und `m_status.active` nur beim Start/Ende eines Downloads setzen (nach dem Fertigstellen immer `false` setzen, im nächsten Schleifendurchlauf wieder `true`). `totalInBatch` im Enqueue mit `m_current`-Zustand statt `active` berechnen. Kommentar 280-283 durch einen Satz ersetzen („m_draining wird nur hier unter dem Lock gelöscht, damit kein zweiter Drain startet“).
- **Erwarteter Effekt**: Kleine Laufzeitersparnis (vernachlässigbar), korrekter Fortschritt, weniger Zustand.
- **Risiko / Verifikation**: Reihenfolge/Fehlerrückgabe der Callbacks muss gleich bleiben (alle Callbacks genau einmal). Neuer Test mit gefaktem `sftpGetFile` (Naht nötig, siehe CSY-105) für `status()` während einer Batch.

### CSY-110 – Transfer-Puffer 64 KiB (Durchsatz auf Hochlatenzstrecken)
- **ID**: CSY-110
- **Kategorie**: D (E)
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/SftpClient.cpp:351`, `:409`
- **Problem**: libssh2 pipelined SFTP-Reads/Writes in Abhängigkeit von der Größe des übergebenen Puffers; mit 64 KiB bleibt die Anzahl gleichzeitiger Requests klein, der Durchsatz sinkt bei höherer RTT merklich. Ob das hier relevant ist, hängt vom Hoster ab, **nicht gemessen**.
- **Code-Auszug**:
```cpp
std::vector<char> buf(64 * 1024);
```
- **Lösung**: Konstante `kTransferBufBytes` (z. B. 512 KiB–1 MiB) für beide Pfade; messen vor Übernahme. Der Progress-Callback nimmt pro Chunk `m_mutex` (Sync.cpp:307) und wird bei größeren Chunks sogar seltener aufgerufen.
- **Erwarteter Effekt**: Unsicher, nur bei hoher Latenz/großen Dateien.
- **Risiko / Verifikation**: Keine funktionalen Änderungen; Messung mit einer 50-MB-Datei gegen den echten Host.

### CSY-111 – Publish/Rebuild: duplizierter Code
- **ID**: CSY-111
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/EngineContentPublish.cpp:145-161` vs. `:246-258`; `:91-96` vs. `:236-241`; `:53`/`:173`; `src/EngineContentSync.cpp:117-120` vs. `:176-179`
- **Problem**: Der Block „Manifest serialisieren → Temp-Datei → `sftpPutFile` → Fehlermeldung“ steht wörtlich zweimal, ebenso das Anlegen eines `.hasset`-Eintrags (UUID, Hash, Größe) und das `log`-Lambda. Zusammen mit `readWholeFile` entstehen drei Stellen, die bei Änderungen (z. B. CSY-105) gleichzeitig angepasst werden müssen.
- **Code-Auszug**:
```cpp
SftpResult putManifest = sftpPutFile(endpoint, tmpManifest.string(), "manifest.json");   // 2x
```
- **Lösung**: Helper im anonymen Namespace: `SftpResult uploadManifest(const SftpEndpoint&, const EngineContentManifest&)` (nutzt später `sftpPutBytes`), `bool makeHassetEntry(relPath, bytes, EngineContentManifestEntry&)`. `onLog` bleibt als `const std::function<…>&`-Parameter (heute Kopie pro Aufruf, vernachlässigbar).
- **Erwarteter Effekt**: ~40 Zeilen weniger, kein Laufzeiteffekt.
- **Risiko / Verifikation**: Nur Refactoring; vorhandene Fehlermeldungstexte unverändert lassen (UI zeigt sie an).

### CSY-112 – `recursive_directory_iterator` in Range-for: `if (ec) break` ist wirkungslos, Inkrement kann werfen
- **ID**: CSY-112
- **Kategorie**: A (F)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/EngineContentPublish.cpp:72-76`
- **Problem**: Der Range-for nutzt `operator++` der Throwing-Variante; ein Fehler beim Weiterlaufen (Ordner verschwindet, Berechtigung) schreibt nicht in `ec`, sondern wirft `filesystem_error` aus `publishEngineContentBlocking` (Worker-Thread). `ec` wird nur vom Konstruktor gesetzt, `if (ec) break;` innerhalb der Schleife ist damit effektiv tot. Bei einem Konstruktorfehler liefert der Iterator `end()`, die Schleife läuft nicht, und der Publish läuft bis zum Manifest-Upload mit leerer `local` – er meldet dann `ok` und lädt ein **leeres Manifest** hoch.
- **Code-Auszug**:
```cpp
for (const auto& p : fs::recursive_directory_iterator(engineContentRoot, fs::directory_options::skip_permission_denied, ec))
{
    if (ec) break;
```
- **Lösung**: Iterator als Variable, nach der Konstruktion `if (ec) { result.error = …; return result; }`, Schleife mit `it.increment(ec)`. Zusätzlich vor dem Manifest-Upload bei `local.entries.empty()` abbrechen (Schutz gegen versehentliches Leeren des Server-Manifests).
- **Erwarteter Effekt**: Keine Performance; verhindert Exception/leeres Manifest.
- **Risiko / Verifikation**: Das leere-Manifest-Verhalten ist ggf. gewollt (alles gelöscht)? Absichern per Test mit gesperrtem/nicht lesbarem Root.

### CSY-113 – `findByPath` im Publish-Loop: O(n·m)
- **ID**: CSY-113
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/EngineContentPublish.cpp:115`, `src/EngineContentManifest.cpp:16-21`
- **Problem**: Pro lokalem Eintrag wird das Remote-Manifest linear durchsucht (String-Vergleiche). Bei z. B. 5 000 Assets sind das ~25 Mio. Vergleiche (Schätzung, deutlich unter 1 s, aber gegenüber dem Netz-Teil vernachlässigbar). Header-Kommentar 53-54 („a handful of times per session“) stimmt für diesen Aufrufer nicht.
- **Code-Auszug**:
```cpp
const auto* remoteEntry = remote.findByPath(entry.relativePath);
```
- **Lösung**: Vor dem Loop `std::unordered_map<std::string, std::uint64_t> remoteHash` aus `remote.entries` bauen. Nur umsetzen, wenn CSY-101 umgesetzt ist; sonst nicht der Engpass.
- **Erwarteter Effekt**: Gering.
- **Risiko / Verifikation**: Keines.

### CSY-114 – `listRemoteDirRecursive`: Dateinamen > 511 Zeichen kappen still den Rest des Verzeichnisses
- **ID**: CSY-114
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/SftpClient.cpp:226-232`
- **Problem**: `nameBuf[512]`; liefert `libssh2_sftp_readdir_ex` bei zu langem Namen einen Fehler (`rc < 0`), beendet `if (rc <= 0) break;` die Verzeichnisauflistung ohne Meldung. Der Rebuild-Manifest-Lauf verliert so alle Einträge hinter dem Namen. Das ist sehr selten (typische Pfade < 255), aber der Code behauptet „degrade, don't abort“ ohne Log.
- **Code-Auszug**:
```cpp
if (rc <= 0) break; // 0 = end of directory, <0 = error — nothing more to read either way
```
- **Lösung**: `rc < 0` loggen (`HE_LOG_WARN(ContentSync, "Listing of '%s' stopped: %d", …)`); Puffer auf 1024 oder 4096 erhöhen (Kosten: Stack).
- **Erwarteter Effekt**: Keine Performance; Diagnose.
- **Risiko / Verifikation**: Gering.

### CSY-115 – `scrub`: Magic Number, doppelte Anwendung, ungenutzte Includes
- **ID**: CSY-115
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/ContentSync/CsLog.h:20-33`, `src/EngineContentSync.cpp:107`, `:314`, `src/SftpProbe.cpp:3`, `include/ContentSync/CsLog.h:11-13`
- **Problem**: (1) `pos += 10; // length of "<redacted>"` ist eine Magic Number; mit `constexpr std::string_view kMask = "<redacted>"` und `pos += kMask.size()` ist der Kommentar überflüssig. (2) `SftpResult::error` ist laut `SftpClient.h:31` schon bereinigt, wird aber in `EngineContentSync.cpp:107` und `:314` nochmals durch `scrub()` geschickt (jeweils Kopie plus `find`); sinnvoll nur als Gürtel-und-Hosenträger für den Test-Fetch (`ManifestFetchFn`). Entweder einheitlich dokumentieren oder auf eine Stelle reduzieren. (3) `scrub` kopiert den Text auch dann, wenn kein Passwort konfiguriert ist (`return out` nach Kopie); `std::string(text)` ist dort gewollt (Rückgabe), also nur Niedrig. (4) `SftpProbe.cpp` inkludiert `SftpCredentials.h` doppelt (über `SftpClient.h` und `CsLog.h`). Es sind aber Aufrufstellen, keine heißen Pfade.
- **Code-Auszug**:
```cpp
pos += 10; // length of "<redacted>" — skip past the replacement
```
- **Lösung**: Siehe oben, ca. 5 Zeilen.
- **Erwarteter Effekt**: Lesbarkeit.
- **Risiko / Verifikation**: Keines (Redaktionstest für Passwort als Teilstring von `<redacted>` prüfen: z. B. Passwort „red“).

### CSY-116 – Veraltete/irreführende Kommentare
- **ID**: CSY-116
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/ContentSync/SftpCredentials.h:53-55`, `src/EngineContentManifest.cpp:66-67` vs. `:28-34`, `src/EngineContentSync.cpp:280-283`, `include/ContentSync/EngineContentManifest.h:53-54`
- **Problem**: (a) „Returned by value (this is a handful of short strings …)“ – die Funktion liefert `const SftpEndpoint&`. (b) Der Parser-Kommentar sagt „`uuid` is absent for a raw entry“; `serializeManifest` schreibt `uuid` aber immer (auch `{0,0}`) und `mtime` immer (auch bei `.hasset`-Einträgen). Das ist harmlos, aber das Format ist größer als beschrieben (und `dump(2)` pretty-printed). (c) Der Kommentar bei `m_draining` beschreibt Gedankengang statt Absicht (siehe CSY-109). (d) „linear scan is fine … once per Editor session“ gilt für `findByPath` im Publish nicht (CSY-113).
- **Code-Auszug**:
```cpp
// The single source of truth ... Returned by value ...
HE_CS_API const SftpEndpoint& engineContentEndpoint();
```
- **Lösung**: Kommentare korrigieren; optional `uuid` nur schreiben, wenn nicht leer, und `dump()` ohne Einrückung. Letzteres ändert das Dateiformat leicht (Parser toleriert beides) und ist nur bei sehr großen Manifesten sinnvoll.
- **Erwarteter Effekt**: Lesbarkeit.
- **Risiko / Verifikation**: Format-Änderung nur mit Test, dass altes und neues Manifest geladen werden (CLAUDE.md, Abschnitt 4).

---

## Cross-Modul-Hinweise
- `src/HE_Editor/EditorApplication.cpp:2524`, `:3378`, `src/HE_Editor/ContentBrowserPanel.cpp:2944`, `src/HE_Editor/ProjectPreflight.cpp:124`: `EngineContentSync::manifest()` kopiert den gesamten Vektor (alle Strings) unter Lock und wird danach sofort in ein anderes DTO (`RemoteEngineAsset`) umkopiert. Seltene Pfade (nach Refresh), daher Niedrig. Alternative: `std::shared_ptr<const EngineContentManifest>` als Snapshot oder `forEachEntry(fn)`.
- `ContentManager` setzt laut `EngineContentSync.h:148-155` ein „materializing“-Flag bis zum Callback; wegen CSY-102 bleibt dieses Flag bei hängender Verbindung dauerhaft gesetzt (`src/HE_Core/…/ContentManager`, nicht gelesen).
- `drainQueue` belegt einen Thread aus `globalPool()` über die gesamte Batch mit blockierendem Netzwerk-I/O (`EngineContentSync.cpp:269`); bei kleinem Pool konkurriert das mit Asset-Streaming-Jobs. Pool-Größe/Dedizierter-I/O-Thread in `src/HE_Core/include/JobSystem/JobSystem.h` prüfen (nicht verifiziert).
- `HE_Net::socketConnectPoll`/`socketWaitReadable` (`src/HE_Net/include/Net/Socket.h:146`, `:161`): es gibt kein `socketWaitWritable` mit Timeout; deshalb pollt `connectBlocking` mit `sleep_for(10 ms)` (`SftpClient.cpp:78`). Ein `select/poll`-Wrapper in HE_Net würde die Poll-Schleife und bis zu 10 ms Latenz pro Connect entfernen (Niedrig).

## Unsicher / Messung nötig
- Tatsächliche Handshake-Dauer des Hosters und der Gewinn aus CSY-101 (Publish von N Dateien vorher/nachher messen).
- Optimale Puffergröße für `libssh2_sftp_read/write` (CSY-110).
- Wirkung von `libssh2_session_set_timeout` auf sehr große Dateien über langsame Leitungen (CSY-102: gilt pro Operation).
- Ob `ThreadPool::submit` Exceptions aus Jobs abfängt (relevant für Schwere von CSY-107 und CSY-112); `JobSystem.h` nicht gelesen.
- Ob das Verwerfen von Raw-Einträgen beim Publish (CSY-108) und das Nicht-Löschen entfernter Remote-Dateien beabsichtigt sind.
- Ob `fs::temp_directory_path(ec)` in Publish/Rebuild je mit `ec` fehlschlägt (Windows-Dienstkonten); nicht geprüft.
- Kein Test für `SftpClient`, Publish oder die Download-Queue vorhanden (nur `test_engine_content_manifest_refresh.cpp`, `test_engine_content_manifest_lock.cpp`); für CSY-101/105/109 wäre ein austauschbarer Transport (Interface oder `std::function`-Naht) die Voraussetzung.

## Gelesen
Alle 15 Dateien des Slice vollständig gelesen: ja (CsCommon.h, CsLog.h, EngineContentManifest.h/.cpp, EngineContentPublish.h/.cpp, EngineContentSync.h/.cpp, SftpClient.h/.cpp, SftpCredentials.h/.cpp, SftpProbe.h/.cpp, CsRename.h); zusätzlich nur gezielte Greps in `HE_Editor` (Aufrufer von `manifest()`) und `HE_Net/include/Net/Socket.h`.
