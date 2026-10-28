# SourceControl Slice 1 – src/HE_SourceControl (alle 24 Dateien: .cpp, ScLog.h, include/SourceControl/*)

## Zusammenfassung

Das Modul ist insgesamt sauber und defensiv geschrieben (Secrets-Hygiene, Timeouts, fixture-basierte Tests). Die Schwachstellen liegen weniger in der Logik als in der Anzahl der Prozess-Spawns und Dateisystem-Walks: Ein Status-Refresh startet 8 git-Prozesse (alle 3 s bei sichtbarem Panel), das Team-Sync-`scan()` ruft pro Datei `fs::relative` (Syscalls) auf und läuft pro Sync dreimal, und die Blob-Pfade halten alle Downloads mehrfach im Speicher. Dazu kommen eine UI-Thread-Verletzung des eigenen Vertrags (zwei git-Prozesse pro Frame im Branch-Dialog), ein Abbruchpfad im Sync, der eine Teil-Scan-Menge als Löschungen committen kann, und Exceptions (`stoull`, `json::value`) auf Worker-Threads. Funde: 3 Hoch, 10 Mittel, 11 Niedrig (24 gesamt; innerhalb einer Schwere nach Relevanz, SCM-111 und SCM-115 bis SCM-124 sind Niedrig). Alle Zahlen sind Schätzungen; es liegen keine Messdaten vor.

---

### SCM-101 – Status-Refresh startet 8 git-Prozesse, davon einen mit verworfenem Ergebnis
- **ID**: SCM-101
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitService.cpp:915-934; GitCli.cpp:479-509 (listBranches), 778-843 (log), 657-681, 695-715, 269-273
- **Problem**: Jeder Refresh (nach jedem Mutations-Befehl und per Poll alle 3 s bzw. 15 s, `GitController.cpp:16-17,422`) führt nacheinander aus: `status`, `remote get-url`, `log`, `rev-list @{upstream}..HEAD`, `for-each-ref refs/heads`, `branch --show-current`, `for-each-ref refs/remotes`, `stash list`. Das Ergebnis von `branch --show-current` wird sofort verworfen (`ignoredCurrent`; der Branchname steckt ohnehin in `RepoStatus::branch` aus `--branch`). Das `rev-list` läuft auch ohne Upstream (schlägt dann fehl) und bei `ahead == 0`, obwohl `status` beides bereits liefert. Prozess-Spawn kostet auf Windows typischerweise 20-60 ms, git-Startup zusätzlich Config/Repo-Discovery; alle Aufrufe laufen seriell auf dem einzigen Worker und blockieren dabei jeden Benutzer-Befehl in der Queue.
- **Code-Auszug**:
```cpp
ev.remoteUrl      = GitCli::remoteUrl(m_root);
GitCli::log(m_root, 20, ev.commits);          // log + rev-list
std::string ignoredCurrent;
GitCli::listBranches(m_root, ev.branches, ignoredCurrent);   // for-each-ref + branch --show-current
GitCli::listRemoteBranches(m_root, ev.remoteBranches);       // for-each-ref
GitCli::stashList(m_root, ev.stashes);
```
- **Lösung**: (1) `listBranches` ohne `--show-current` anbieten (Überladung ohne `outCurrent`, oder den Aufruf nur machen, wenn der Aufrufer ihn braucht). (2) Heads und Remotes in einem Aufruf: `for-each-ref --sort=refname --format=%(refname) refs/heads refs/remotes` und im Parser nach Präfix trennen (`refs/heads/` vs `refs/remotes/`, plus die bestehende `/HEAD`-Filterung). (3) `rev-list` nur ausführen, wenn `s.upstream` nicht leer und `s.ahead > 0` ist (Wert liegt vor `log()` schon im `RepoStatus`; z. B. `log(root, n, out, err, bool checkUnpushed)`). (4) Optional (messen, braucht git >= 2.35): `status --show-stash` liefert `# stash N`; bei 0 entfällt `stash list`. (5) `remoteUrl` nur nach `SetRemote`/`SetupGitHub`/`Clone`/manuellem Fetch neu lesen und sonst aus dem letzten Event übernehmen (oder per `git config --get remote.origin.url` im selben Takt wie Punkt 2 belassen). Typisch: 8 auf 4-5 Prozesse.
- **Erwarteter Effekt**: Schätzung: -3 bis -4 Prozess-Spawns je Refresh; auf Windows grob 100-200 ms Worker-Zeit weniger pro Poll, spürbar weniger Latenz für Commit/Push direkt nach einem Poll. Messen mit `HE_LOG=SourceControl=Trace` (Zeitstempel je Aufruf).
- **Risiko / Verifikation**: Beobachtbares Verhalten (Branchliste, Stashliste, `unpushed`-Flags) muss identisch bleiben. `tests/test_git_roundtrip.cpp` und `tests/test_git_panel_ops.cpp` decken Branch-/Stash-/Log-Listen ab; Test für "Repo ohne Upstream" und "Detached HEAD" ergänzen, da `listBranches` dort leere/andere Ergebnisse liefert.

### SCM-102 – Branch-Name-Prüfung startet im UI-Thread zwei git-Prozesse pro Frame
- **ID**: SCM-102
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Editor/SourceControlPanel.cpp:1751-1758 (Aufrufer); GitCli.cpp:437-449 (isValidBranchName, branchExists); GitCli.h:10-11 und GitLocator.h:36-38 (Vertrag)
- **Problem**: `GitCli.h` legt fest "Every call blocks on a subprocess ... nothing here should be called from the frame loop". Der Dialog "Neuer Branch" ruft aber in jedem Frame, solange der Name nicht leer ist, `isValidBranchName` (`git check-ref-format`, Timeout 10 s) und `branchExists` (`git rev-parse`, Timeout 10 s) im Render-Thread auf. Das sind ~120 Prozess-Spawns/s bei 60 FPS, nicht nur beim Tippen, sondern solange der Dialog offen ist. `GitLocator.h` erwähnt diesen Aufrufer ausdrücklich, der Vertrag ist also bekannt verletzt. Hängt git (Netzlaufwerk, Antivirus), friert der Editor bis zu 20 s ein.
- **Code-Auszug**:
```cpp
if (!empty && !git->projectRoot().empty())
{
    nameOk = HE::Sc::GitCli::isValidBranchName(git->projectRoot(), s_branchName);
    taken  = nameOk && HE::Sc::GitCli::branchExists(git->projectRoot(), s_branchName);
}
```
- **Lösung**: Ergebnis cachen, nur bei geändertem Text neu fragen: `static std::string s_checked; static bool s_ok, s_taken; if (s_checked != s_branchName) { ... }`. Besser: den Check über den `GitService`-Worker (`requestCheckBranchName`, Antwort per `pump()`), alternativ rein lokal prüfen: `taken` aus der bereits vorhandenen `GitService::branches()`-Liste (kein Prozess nötig) und für die Gültigkeit eine reine C++-Funktion für git's `check-ref-format`-Regeln (Zeichenklassen, `..`, `@{`, `.lock`, führender/abschließender `/`/`.`) als Vorfilter, `git check-ref-format` nur beim Anlegen (dort ruft `createBranch` ihn bereits auf).
- **Erwarteter Effekt**: Schätzung: von ~120 Spawns/s auf 0 im Leerlauf bzw. 2 pro Tastendruck; Frame-Spikes im Dialog verschwinden.
- **Risiko / Verifikation**: Das Kommentar im Panel ("git's own rules, asked of git, so the answer cannot drift") will genau keine Reimplementierung; daher Variante "Cache pro Textänderung + Prüfung bei Änderung auf dem Worker" wählen, die das Verhalten exakt erhält. Test: `test_git_panel_ops.cpp` um "Name ungültig/vergeben" erweitern; manuell: Dialog öffnen und Prozessliste beobachten.

### SCM-103 – `scan()` ruft pro Datei `fs::relative` auf (Syscalls je Eintrag)
- **ID**: SCM-103
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:759-791 (insb. 765)
- **Problem**: `std::filesystem::relative(p, base)` ist als `weakly_canonical(p).lexically_relative(weakly_canonical(base))` definiert und führt je Aufruf `lstat`/`stat` für die Pfadkomponenten aus (beide Pfade, auch `base` jedes Mal neu). Der Aufruf geschieht für jeden Eintrag des rekursiven Walks (Dateien und Ordner), bevor überhaupt ignoriert wird. Bei einem Projekt mit z. B. 50.000 Einträgen in Tiefe 5-8 sind das mehrere hunderttausend zusätzliche Syscalls je Scan. Dieselbe Funktion läuft bei jedem Save-Sync und jedem Reconcile (siehe SCM-104).
- **Code-Auszug**:
```cpp
const fs::directory_entry& e = *it;
std::error_code ec2;
const std::string rel = fs::relative(e.path(), settings.projectRoot, ec2).generic_string();
if (ec2 || rel.empty()) continue;
```
- **Lösung**: Der Iterator liefert Pfade, die per Konstruktion mit `settings.projectRoot` beginnen. Ein rein lexikalischer Ausdruck genügt: `const std::string rel = e.path().lexically_relative(root).generic_string();` mit `root` einmal vor der Schleife gebildet (`const fs::path& root = settings.projectRoot;`). Noch billiger: `it.depth()` nutzen oder den String-Präfix abschneiden. Zusätzlich `e.is_symlink/is_directory/is_regular_file` aus dem gecachten `directory_entry` lesen (macht der Code schon, `file_size`/`last_write_time` kosten je 1 stat; auf Windows liefert `directory_entry` beide ohne Zusatz-Syscall).
- **Erwarteter Effekt**: Schätzung: Scan-Walk 2-10x schneller auf großen Bäumen (dominiert von den entfallenden Syscalls); messen mit einem Projekt >20k Dateien.
- **Risiko / Verifikation**: Ergebnis identisch, solange `projectRoot` Präfix der Iterator-Pfade ist (gilt, der Iterator wird mit genau diesem Pfad konstruiert). Randfälle: Root mit Symlink oder Trailing-Slash; Test in `tests/test_team_sync.cpp` mit Root über Symlink und mit `./`-Präfix ergänzen.

### SCM-104 – Pro Sync-Zyklus bis zu drei vollständige Verzeichnis-Scans
- **ID**: SCM-104
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:688-700 (run), 985 (reconcile), 1114 (upload), 1244 (upload -> reconcile)
- **Problem**: Ein `requestSync` löst im Worker aus: `reconcile` (scan 1), danach `upload` (scan 2), und `upload` endet mit `return reconcile(false)` (scan 3). `notifyHead` löst zusätzlich je fremdem Save einen weiteren Reconcile-Scan aus. Die Size/mtime-Cache spart zwar das Hashen, nicht aber den Walk samt `stat` je Datei. Zwischen den Scans liegen typischerweise Millisekunden bis wenige Sekunden (Upload), die Datei-Menge ist praktisch gleich.
- **Code-Auszug**:
```cpp
if (doReconcile || (doSync && firstSync)) { ... reconcile(adopt) ... }   // scan #1
if (doSync && !firstSync) { ... upload(msg); }                            // scan #2 ...
...
return reconcile(false);   // ... und scan #3 am Ende von upload()
```
- **Lösung**: `scan()` liefert eine Momentaufnahme mit Zeitstempel; `reconcile(adopt, const std::map<...>* prescanned = nullptr)` und `upload()` nehmen sie entgegen, und `run()` hält pro Schleifendurchlauf höchstens einen Scan (Wiederverwendung, solange keine eigene Schreib-/Download-Operation dazwischenlag). Veraltete `local`-Werte sind bereits abgesichert: `Incoming::expect` wird beim Schreiben gegen den aktuellen Hash geprüft ("a save since then wins"). Alternativ den Reconcile am Ende von `upload` nur mit den vom Server gemeldeten Pfaden (`results`) und der neuen Manifest-Differenz fahren statt mit erneutem Gesamt-Scan.
- **Erwarteter Effekt**: Schätzung: -33 % bis -66 % Scan-Zeit pro Sync (bei 50k Dateien je Scan grob 0,3-2 s I/O, wenn gecacht; messen).
- **Risiko / Verifikation**: Mittel; die Reihenfolge Upload -> Reconcile bestimmt, welche Dateien der Server-Merge zurückschreibt. `tests/test_team_sync.cpp` (Fake-Server) mit "Save während Upload" und "Merge ergibt andere Datei" prüfen; ohne Test nicht ändern.

### SCM-105 – Blob-Downloads werden dreifach im RAM gehalten (Map, Kopie, Incoming)
- **ID**: SCM-105
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:931-965 (getBlobs), 1036-1061 (reconcile)
- **Problem**: `getBlobs` lädt alle fehlenden Blobs vollständig in `std::map<sha, vector<uint8_t>>`. Danach wird je Datei `in.bytes = blobs[w.sha]` **kopiert** (Zeile 1046), nicht verschoben, und das Ganze wartet unter `mu` auf die Hauptthread-Schreibphase. Peak-RAM ≈ 2x die Gesamtgröße aller zu holenden Dateien (plus Reply-Puffer). Bei einem Team-Projekt mit mehreren GB (Texturen, Meshes, `.hasset`) ist ein Erst-Sync oder ein "adopt" nach einer Session ein OOM-/Swap-Risiko. Alle Daten liegen außerdem bis zum Hauptthread-`pump()` im Speicher (bei Play-Modus unbegrenzt: `mayWrite == false`).
- **Code-Auszug**:
```cpp
std::map<std::string, std::vector<std::uint8_t>> blobs;
if (!getBlobs(shas, blobs)) return false;
...
if (!w.remove) in.bytes = blobs[w.sha];          // Kopie
```
- **Lösung**: (a) Kurzfristig: `std::shared_ptr<const std::vector<uint8_t>>` in `Incoming` (gleicher sha bei mehreren Pfaden teilt den Puffer, kein Kopieren) bzw. beim letzten Verwender `std::move`. (b) Strukturell: in Batches nach Bytebudget (z. B. 64-128 MB) herunterladen, übergeben, auf `answers` warten, Batch freigeben und weiter; oder gleich in Temp-Dateien neben dem Ziel streamen (`writeFileAtomic` existiert) und den Hauptthread nur noch umbenennen lassen.
- **Erwarteter Effekt**: Schätzung: Peak-Speicher -50 % (a) bzw. begrenzt auf Batchgröße (b).
- **Risiko / Verifikation**: Das Schreiben bleibt im Hauptthread (Race-Freiheit mit Editor-Saves ist die Kerneigenschaft, siehe Header-Kommentar). Batch-Variante verändert die Reconcile-Semantik (Teil-Erfolge, `rev = headRev` erst am Ende); `tests/test_team_sync.cpp` um mehrere Batches und `hold`/Play-Pause erweitern.

### SCM-106 – Upload: O(m·n)-Suche je fehlendem Blob und rein serielle PUTs
- **ID**: SCM-106
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:1136-1173
- **Problem**: Für jeden vom Server als fehlend gemeldeten sha wird `entries` linear durchsucht (`for (const Entry& x : entries) if (x.sha == sha)`), also O(missing × entries) mit String-Vergleichen; beim Erst-Sync mit z. B. 20.000 neuen Dateien bis ~4·10^8 Vergleiche. Außerdem wird jede Datei nacheinander vollständig gelesen, gehasht und per `call()` (Request + Warten auf Reply) hochgeladen, während `getBlobs` für Downloads bereits pipelined arbeitet. Latenz je Datei ≈ 1 RTT + Poll-Granularität (siehe SCM-107). Die Datei wird zudem erneut komplett gehasht (`shaOf(bytes)`), was als TOCTOU-Schutz gewollt ist.
- **Code-Auszug**:
```cpp
for (const std::string& sha : missing)
{
    const Entry* e = nullptr;
    for (const Entry& x : entries)
        if (x.sha == sha) { e = &x; break; }
    ...
    if (!call([&](ScClient::ReplyFn fn) { return client->put(sha, bytes, std::move(fn)); }, r))
```
- **Lösung**: (1) Beim Aufbau von `shas` eine `std::unordered_map<std::string, const Entry*> bySha` füllen (dort wird bereits per `uniq.insert` dedupliziert) und im Loop `bySha.find(sha)` nutzen. (2) PUTs mit kleinem Fenster (z. B. 4 gleichzeitig / 32 MB) pipelinen wie in `getBlobs`; Fehlerbehandlung wie dort (`pending`-Zähler). Für (2) vorab prüfen, ob `ScClient::put` mehrere offene Requests verträgt (Slice HE_Net, nicht geprüft).
- **Erwarteter Effekt**: (1) Schätzung: von Sekunden bis Minuten (Erst-Sync, große Projekte) auf vernachlässigbar. (2) Schätzung: bei kleinen Dateien und WAN-RTT 50 ms 4-10x schneller; messen.
- **Risiko / Verifikation**: (1) null Risiko, Verhalten identisch. (2) Reihenfolge der Fehlermeldung und `wantSync`-Wiederholung bei geänderter Datei müssen erhalten bleiben; Fake-Server-Test mit abgebrochenem PUT.

### SCM-107 – `pumpUntil` pollt im 1-ms-Takt (Yield-Spin, dann 1000 Wakeups/s)
- **ID**: SCM-107
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:808-826
- **Problem**: Während jeder Anfrage (HAVE, PUT, COMMIT, MANIFEST, GET und beim Verbindungsaufbau) ruft der Worker `client->update()` in einer Schleife auf: 32x `yield`, danach `sleep_for(1ms)`. Wartet der Server lange (Commit mit Merge, Sekunden), erzeugt das ~1000 Wakeups/s auf einem Core samt Context-Switches, ohne dass Daten ankommen. Die Antwortlatenz ist durch die 1-ms-Granularität zusätzlich nach unten begrenzt.
- **Code-Auszug**:
```cpp
if (++idle > 32) std::this_thread::sleep_for(std::chrono::milliseconds(1));
else std::this_thread::yield();
```
- **Lösung**: Falls `ScClient` einen Wait/Event-Mechanismus (Socket-`poll`, Condvar bei Eingang) anbietet oder ergänzt werden kann: darauf warten (Timeout z. B. 50 ms). Ohne API-Änderung: Backoff, z. B. Schlaf 1 -> 2 -> 5 -> 10 ms bis zum ersten Eingang, nach jedem Fortschritt zurück auf 1 ms. Weil der Aufruf im Worker (nicht im Frame) läuft, ist die Wirkung CPU-/Akku-seitig, nicht Frame-seitig.
- **Erwarteter Effekt**: Schätzung: CPU-Last während Netzwerkwarten von ~1 Wakeup/ms auf ≤100/s; Latenz +0 bis 9 ms je Antwort bei Backoff-Variante. Unsicher/messen.
- **Risiko / Verifikation**: `ScClient::update()` gehört zu HE_Net (anderer Slice); ob es blockierend warten kann, dort klären. Fake-Server-Tests in `tests/test_team_sync.cpp` laufen weiter.

### SCM-108 – Mehrfache vollständige `git status`-Läufe innerhalb einer Operation
- **ID**: SCM-108
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitService.cpp:439-465 (CommitAll: 446, 459, 919), 494-535 (CommitStaged: 498, 528, 919); GitCli.cpp:551-573 (unstage: 558), 575-615 (discard: 579)
- **Problem**: `git status --untracked-files=all` über einen Baum mit tausenden Assets ist der teuerste lokale git-Aufruf. `CommitAll` führt ihn dreimal aus: vor dem Commit (Größenprüfung), danach nur um `probe.upstream` zu lesen (der Upstream hat sich durch den Commit nicht geändert, `pre.upstream` enthält ihn schon), und ein drittes Mal als abschließender Refresh. `CommitStaged` ebenso. `unstage` und `discard` rufen intern jeweils einen eigenen Status auf, obwohl das UI den aktuellen Snapshot besitzt. Zusätzlich `remoteUrl` zweimal (456 und Refresh 925).
- **Code-Auszug**:
```cpp
if (cmd.flag && !GitCli::remoteUrl(m_root).empty())
{
    RepoStatus probe;
    const bool upstream = GitCli::status(m_root, probe) && !probe.upstream.empty();
    if (GitCli::push(m_root, upstream, &err)) ev.info = "Committed and pushed.";
```
- **Lösung**: In `CommitAll`/`CommitStaged` `pre.upstream.empty()` statt `probe` verwenden (Commit ändert den Upstream nie). Für `unstage`/`discard` die benötigten Rename-Informationen (`origPath`) bzw. die Klassifikation im Worker aus dem zuletzt gelieferten Snapshot beziehen (`m_lastStatus` als Worker-Kopie, am Ende von `wantStatus` gesetzt), wenn dieser höchstens wenige Sekunden alt ist, sonst frisch lesen. Wichtig bei `discard`: die Klassifikation entscheidet über destruktive Aktionen, daher bei Zweifel frisch lesen.
- **Erwarteter Effekt**: Schätzung: -1 Status-Lauf pro Commit (bei großen Repos mehrere hundert ms bis Sekunden), -1 pro Unstage/Discard.
- **Risiko / Verifikation**: `pre.upstream`-Variante ist verhaltensgleich. Snapshot-Variante birgt Stale-Risiko bei destruktivem `discard` -> nur mit Frischegarantie. Tests: `test_git_panel_ops.cpp` (Commit mit/ohne Upstream, Rename-Unstage, Discard von Added/Renamed).

### SCM-109 – Abbruch während `scan()` kann eine Teilmenge als Löschungen committen
- **ID**: SCM-109
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:762 (break bei `stopping`), 792-793, 1114-1128, 1182
- **Problem**: `scan()` bricht bei `stopping` ab und gibt die bis dahin gesehene (Teil-)Menge zurück. `upload()` prüft `stopping` danach nicht und berechnet `entries`: jede Datei aus `base`, die der abgebrochene Scan noch nicht erreicht hat, erscheint als `'D'`. Gibt es keine A/M-Einträge (`shas.empty()`), wird der HAVE-Call übersprungen und direkt `client->commit(...)` gesendet; `call()` sendet vor `pumpUntil`, und `pumpUntil` kehrt wegen `stopping` sofort zurück, die Anfrage ist aber raus. Szenario: Editor wird während eines Syncs beendet, der Scan ist halb durch. Auch die ersten Einträge mit `shas` != leer sind nur durch den zufälligen HAVE-Call vor dem Commit geschützt (er schlägt wegen `stopping` fehl). Ob der Server die Massenlöschung annimmt, hängt von `ScClient::commit` (anderer Slice) ab; die Historie bleibt erhalten, aber es wäre ein unerwünschter Commit "alles gelöscht" für das Team. Unsicher, weil ohne Test mit Fake-Server nicht verifiziert.
- **Code-Auszug**:
```cpp
if (stopping) break;                // scan(): liefert Teilergebnis
...
const std::map<std::string, std::string> local = scan();   // upload(): kein stopping-Check
...
entries.push_back(Entry{ l.empty() ? 'D' : (b.empty() ? 'A' : 'M'), p, b, l });
```
- **Lösung**: `scan()` als `bool scan(std::map&)` oder `std::optional<map>` zurückgeben (leer = abgebrochen) und in `upload()`/`reconcile()` direkt `return false` bei Abbruch. Alternativ am Anfang von `upload()` nach dem Scan `if (stopping) return false;`.
- **Erwarteter Effekt**: keine Performance-Änderung; entfernt ein seltenes, aber teures Fehlverhalten.
- **Risiko / Verifikation**: Gering. Test in `tests/test_team_sync.cpp`: `stop()` aus einem Fake-Server-Callback während des Scans (viele Dateien) und prüfen, dass kein COMMIT beim Server ankommt.

### SCM-110 – Ungefangene Exceptions im Sync-Worker (`stoull`/`stoll`)
- **ID**: SCM-110
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Robustheit)
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:727, 730, 234
- **Problem**: `loadState()` läuft als erstes im Worker-Thread (`run()` Zeile 611) und parst mit `std::stoull`/`std::stoll`. Eine beschädigte oder von Hand/anderer Version bearbeitete `.state`-Datei (leeres Feld, Nicht-Ziffer) wirft `std::invalid_argument`/`out_of_range`; es gibt kein `try/catch` in `run()`, also `std::terminate` und Absturz des gesamten Editors beim Projektöffnen. Ebenso `parseAgainst` (Zeile 234) auf Serverdaten: nur Ziffern geprüft, aber Überlauf (>20 Ziffern) wirft. `fetchLog` nutzt dagegen schon `strtoull` (kein Throw), das ist inkonsistent.
- **Code-Auszug**:
```cpp
if (f.size() == 2 && f[0] == "rev") rev = std::stoull(f[1]);
...
cache[f[1]] = Cache{ std::stoull(f[3]), std::stoll(f[4]), f[2] };
```
- **Lösung**: `std::from_chars` (exception-frei, schneller) mit Prüfung; bei Fehler Zeile überspringen bzw. State als ungültig behandeln (`return false` -> Erst-Sync). Für `parseAgainst` ebenfalls `from_chars`, bei Fehler `return false`. Zusätzlich `Impl::run` in `try { ... } catch (...)` kapseln, damit ein unerwarteter Fehler den Status auf `Offline` setzt statt den Prozess zu beenden.
- **Erwarteter Effekt**: Absturzfreiheit bei beschädigtem State; vernachlässigbarer Speed-Gewinn beim Laden großer State-Dateien (`from_chars` vs. `stoull` je Zeile).
- **Risiko / Verifikation**: Verhalten bei gültigen Dateien identisch. Test: abgeschnittene/korrupte State-Datei in `test_team_sync.cpp` (erwartet: Erst-Sync, kein Crash).

### SCM-111 – GitHubApi: `json::value()` wirft bei `null`/falschem Typ, entgegen dem eigenen Kommentar
- **ID**: SCM-111
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Robustheit)
- **Ort**: src/HE_SourceControl/src/GitHubApi.cpp:31-32, 40, 139, 310, 353, 406-407; Kommentar 194-195
- **Problem**: `jsonString()` existiert genau, weil `json::value()` bei vorhandenem Schlüssel mit anderem Typ (z. B. `"message": null`) wirft ("GitHub does send null for some of these"). Die Parser für Create-Repo, User, Gist, Issue und `apiMessage` nutzen trotzdem `j.value(...)`. Eine Exception läuft im Worker (`SetupGitHub`) bzw. im Aufrufer-Thread aus und beendet den Prozess.
- **Code-Auszug**:
```cpp
out.cloneUrl = j.value("clone_url", "");
...
if (j.is_object()) out.login = j.value("login", "");
```
- **Lösung**: `jsonString` an den Dateianfang ziehen (vor `parseCreateRepoResponse`) und überall verwenden; für `number` ein `jsonInt`-Pendant (existiert in GitHubOAuth.cpp, siehe SCM-118).
- **Erwarteter Effekt**: Robustheit; keine Performance-Änderung.
- **Risiko / Verifikation**: Gering; `tests/test_github_oauth.cpp` sowie die Response-Parser-Tests um `null`-Felder ergänzen.

### SCM-112 – Jeder Poll ersetzt den Snapshot und erhöht `generation`, auch ohne Änderung
- **ID**: SCM-112
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitService.cpp:1001-1008; RepoStatus.h:94-96; Verbraucher: src/HE_Editor/SourceControlPanel.cpp:340, 1957; GitController.cpp:356
- **Problem**: `pump()` setzt bei jedem erfolgreichen Refresh `generation = ++m_generation` und ruft `m_onStatus`. Der Header-Kommentar behauptet, damit könne ein Verbraucher "nothing changed" erkennen ohne Vergleich der Maps, doch weil die Zahl bei jedem Poll steigt (alle 3 s), kann er das nie. Die Verbraucher bauen bei jeder neuen Generation ihre Modelle neu (`s_model.generation != st.generation`, `s_generation != st.generation`, `m_aliasGeneration`), z. B. bei 10k+ Einträgen alle 3 s eine komplette Neuberechnung im Frame. Ohne Messung unsicher, wie teuer der Rebuild im Panel ist.
- **Code-Auszug**:
```cpp
ev.status.generation = ++m_generation;
m_status = std::move(ev.status);
if (m_onStatus) m_onStatus(m_status);
```
- **Lösung**: Der Worker hält den letzten gelieferten Snapshot und vergleicht den neuen (Felder + `files`-Map; O(n) und billiger als jeder Verbraucher-Rebuild) -> Event bekommt `statusChanged`. `pump()` erhöht `generation` und ruft den Callback nur bei Änderung (Listen wie `m_commits`/`m_branches` weiterhin immer übernehmen oder analog vergleichen). Alternativ Hash über die Rohausgabe von `git status` (`r.out`) im `GitCli::status`.
- **Erwarteter Effekt**: Schätzung: UI-seitige Rebuilds im Normalfall (nichts hat sich geändert) von 1/3 s auf 0; Gewinn proportional zur Rebuild-Kosten im Panel/ContentBrowser. Messen.
- **Risiko / Verifikation**: Verbraucher dürfen sich nicht auf "neue Generation = frisch refresht" stützen (z. B. `lastRefresh`-Anzeige). Grep nach `generation` in `HE_Editor` (3 Stellen oben) prüfen; `test_git_status_parse.cpp`/`test_git_panel_ops.cpp`.

### SCM-113 – Team-Sync-Accessors kopieren pro Frame Status, History, Lost und Results
- **ID**: SCM-113
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:558-584, 1239; Header 183-193; Aufrufer src/HE_Editor/TeamSyncView.cpp:58, 61 (stateLine -> TeamSyncController.cpp:187), 107, 123, 143
- **Problem**: `status()`, `lastResults()`, `lost()` und `history()` geben Kopien unter `mu` zurück und werden in `TeamSyncView::draw` pro Frame aufgerufen (`status()` sogar doppelt: View und `stateLine()`). `history()` kopiert bis zu 100 `Revision`s inklusive je einem `vector<string>` Merges; `lastResults()` ist unbegrenzt (`this->results = std::move(results)`, nach einem Erst-Sync 20k+ `FileResult` mit je drei Strings) und wird jedes Frame kopiert und mit `TextWrapped` komplett gezeichnet (ohne Clipping). Zusätzlich blockiert `mu` den Worker kurz bei jeder Kopie, wenn dieser `setStatus` aufruft.
- **Code-Auszug**:
```cpp
std::vector<TeamSyncService::FileResult> TeamSyncService::lastResults() const
{
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->results;
}
```
- **Lösung**: Im Service einen Versionszähler je Datenart führen (`resultsVersion`, `historyVersion`, `statusVersion`) und `std::shared_ptr<const std::vector<...>>` (copy-on-write beim Worker) herausgeben; die View hält den Zeiger und holt ihn nur bei geänderter Version. `results` auf z. B. `kHistory`-ähnlichen Deckel (200) begrenzen, Rest als "…und N weitere" zählen. UI-seitig Zeilen-Clipping (`ImGuiListClipper`) wie `OutlinerPanel.cpp`.
- **Erwarteter Effekt**: Schätzung: pro Frame bei sichtbarem Panel ~hunderte bis tausende Allokationen weniger; nach Erst-Sync deutlich (20k Zeilen/Frame Zeichnen verhindert Interaktivität).
- **Risiko / Verifikation**: API-Änderung (`Status`-Kopie bleibt für Tests praktisch); `tests/test_team_sync_ui.cpp` anpassen. Der Deckel ändert sichtbares Verhalten (nur die letzten N Ergebnisse) und gehört abgestimmt.

### SCM-114 – `caseIndex` wird für jede Statuszeile eager gebaut, obwohl nur Fallback
- **ID**: SCM-114
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/StatusParser.cpp:130-133; RepoStatus.cpp (StatusParser.cpp) 77-87; RepoStatus.h:98-102
- **Problem**: Der Parser legt für jeden Eintrag `caseIndex[toLower(path)] = path` an: eine String-Kopie fürs Lowercasing, eine zweite als Wert, ein Hash-Insert. `find()` nutzt den Index laut eigenem Kommentar nur, wenn der exakte Lookup scheitert. Bei `--untracked-files=all` und z. B. 10.000 Einträgen fallen pro Poll (3 s) ~20.000 Allokationen plus Hashing an, plus `record(std::string(rest), e)`, das `FileEntry` (mit `origPath`-String) per Wert kopiert. Auf Linux (case-sensitives FS) ist der Index praktisch nie nötig.
- **Code-Auszug**:
```cpp
auto record = [&](std::string path, FileEntry entry) {
    out.caseIndex[toLower(path)] = path;
    out.files.emplace(std::move(path), std::move(entry));
};
```
- **Lösung**: `caseIndex` lazy beim ersten Miss in `find()` aufbauen (`mutable std::unordered_map caseIndex; mutable bool caseIndexBuilt`; Hinweis: `RepoStatus` wird nach dem Swap nur im Hauptthread gelesen, aber Thread-Sicherheit bei parallelen Readern beachten -> `std::once_flag` oder Aufbau im Worker nur wenn `HE_CASE_INSENSITIVE_FS`). Alternativ nur bauen, wenn Plattform Windows/macOS. `files.reserve(raw.size() / 80)` als Heuristik, `record` per `emplace`/Move.
- **Erwarteter Effekt**: Schätzung: Parser-Zeit -40-50 % und halbierte Allokationen je Poll; im Vergleich zu den Prozess-Spawns (SCM-101) klein, aber Dauerlast.
- **Risiko / Verifikation**: `find()`-Semantik unverändert. `test_git_status_parse.cpp` hat Case-Tests (prüfen), sonst ergänzen (Treffer im Fallback; wiederholtes `find` nach Lazy-Build).

### SCM-115 – Reconcile/Upload/State: drei Maps plus `std::set` aller Pfade, State-Datei wird jedes Mal komplett neu geschrieben
- **ID**: SCM-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:736-745 (saveState), 756-793 (scan), 979-1026, 1117-1129
- **Problem**: (1) `reconcile` und `upload` bilden `std::set<std::string> paths` als Vereinigung der Schlüssel von `team`, `local`, `base` (alle Pfad-Strings kopiert) und schlagen dann dreimal pro Pfad in den Maps nach (O(n log n) mehrfach). Die drei `std::map`s sind bereits sortiert gleich geordnet; ein 3-Wege-Merge-Join ohne Set und ohne Lookups genügt. (2) `scan()` hält `local`, `seen` und `cache` parallel (je Pfad dreimal gespeichert, `seen[rel] = c->second` kopiert den Hash-String); `local` ließe sich aus `cache` ableiten. (3) `saveState()` baut den gesamten Zustand mit `"B\t" + p + "\t" + sha + "\n"`-Kettenaddition (je Zeile mehrere temporäre Strings), kopiert ihn dann in einen `vector<uint8_t>` und schreibt ihn bei **jedem** Reconcile/Upload komplett neu, auch wenn sich nichts geändert hat (z. B. jeder `notifyHead`).
- **Code-Auszug**:
```cpp
for (const auto& [p, sha] : base) out += "B\t" + p + "\t" + sha + "\n";
...
if (!writeFileAtomic(settings.stateFile, std::vector<std::uint8_t>(out.begin(), out.end())))
```
- **Lösung**: `saveState` nur bei Änderung (Dirty-Flag in `base`/`cache`/`rev`), `out.reserve(...)`, `out += "B\t"; out += p; ...`, `writeFileAtomic` um eine `string_view`-Überladung erweitern (keine Kopie). Merge-Join statt Set. Cache und `local` zusammenführen.
- **Erwarteter Effekt**: Schätzung: bei 50k Dateien State ~8 MB: Schreiben/Allokation je Reconcile von ~20-60 ms auf ~0 im Leerlaufpfad; Merge-Join spart ein Mehrfaches an Allokationen. Unsicher/messen, läuft im Worker.
- **Risiko / Verifikation**: Dirty-Flag muss alle Mutationen von `base`/`cache` erfassen (Crash-Sicherheit des Zustands bleibt wichtig: Atomic-Write bleibt). Test: Start/Stop-Roundtrip mit State in `test_team_sync.cpp`.

### SCM-116 – `Command`/`Event` als Sammelstrukturen, 600-Zeilen-`switch` in `workerMain`
- **ID**: SCM-116
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_SourceControl/include/SourceControl/GitService.h:209-244, 263-267; GitService.cpp:358-956 (insb. 202, 212, 377/890, 456-462 vs. 525-532)
- **Problem**: `Command` ist ein Universalcontainer: `text` bedeutet je nach `Kind` Nachricht/URL/Repo-Name/Host/Branch/Commit, `secret` wird für `RestoreTo` und `CreateBranch` als "zweiter String-Slot" zweckentfremdet (im Code selbst kommentiert), `flag`/`flag2` bedeuten je Befehl etwas anderes. `workerMain` ist eine ~600 Zeilen lange Funktion mit einem `switch` über 23 Fälle, ihre Zustandsflüsse (`wantStatus`, `ev`) werden über `break` gesteuert. Duplikate: "Push nach Commit" (zwei Kopien mit minimal anderem Text), Token-Wipe (`std::fill`+`clear` inline in 648, 672, 774), `Quit` doppelt behandelt (`if (cmd.kind == Kind::Quit) return;` und `case Kind::Quit: return;`) plus zusätzlich `m_quit`-Flag.
- **Code-Auszug**:
```cpp
c.text   = shortOid ... // requestRestoreTo:
// Reuses the secret field purely as a second string slot; nothing secret here
c.secret = shortOid;
```
- **Lösung**: Pro Befehl ein kleines Struct (`struct CommitAllCmd { std::string message; bool push; }` usw.) in einem `std::variant` und je Befehl eine Methode `handle(const CommitAllCmd&, Event&)`; `countsAsWork` wird zu einer Eigenschaft des Typs. Gemeinsame Hilfen: `pushAfterCommit(Event&, const RepoStatus&)`, `wipe(std::string&)` (siehe SCM-118). Reine Struktur-Umformung, nicht auf Performance gerichtet.
- **Erwarteter Effekt**: Lesbarkeit/Wartbarkeit, weniger Verwechslungsgefahr (ein Geheimnisfeld mit Nicht-Geheimnis); kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel wegen der Größe; schrittweise Extraktion der Handler (zuerst nur Funktionen aus den `case`-Blöcken ziehen). `test_git_panel_ops.cpp` und `test_git_roundtrip.cpp` müssen grün bleiben.

### SCM-117 – Race in `GitService::push`: `m_busy` wird nach dem Lock gesetzt
- **ID**: SCM-117
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: src/HE_SourceControl/src/GitService.cpp:19-28 und 951-954
- **Problem**: `push()` hängt den Befehl unter `m_inMutex` an, gibt das Lock frei und setzt erst dann `m_busy = true`. Der Worker kann den Befehl in dieser Lücke bereits abarbeiten und am Ende (unter `m_inMutex`, Queue leer) `m_busy = false` setzen; danach schreibt `push()` `true`. Ergebnis: `busy()` bleibt `true` bei leerer Queue bis zum nächsten Befehl (Spinner hängt). Sehr kleine Zeitlücke (ein git-Prozess dazwischen), aber real möglich bei Preemption des Hauptthreads.
- **Code-Auszug**:
```cpp
{
    std::lock_guard<std::mutex> lock(m_inMutex);
    m_in.push_back(std::move(c));
}
m_busy.store(true, std::memory_order_release);   // nach dem Unlock
```
- **Lösung**: `m_busy.store(true, ...)` innerhalb des Lock-Scopes vor `push_back` setzen.
- **Erwarteter Effekt**: keiner messbar; entfernt einen schwer reproduzierbaren UI-Fehler.
- **Risiko / Verifikation**: Gering. Stresstest: 10^5 `requestStatus`/Warten bis `!busy()` in `test_git_panel_ops.cpp` (nicht vorhanden, ergänzen).

### SCM-118 – Mehrfach duplizierte Hilfsfunktionen im Modul
- **ID**: SCM-118
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `trimTrailing` GitCli.cpp:17, `trimmedLine` GitInstall.cpp:34, `trimmed` GitProbe.cpp:17; `pathFromUtf8` GitCli.cpp:299 und GitLocator.cpp:26; `wipe` GitHubOAuth.cpp:38 und GitHubTokenStore.cpp:12 (+ inline in GitService.cpp:648, 672, 774); `jsonString` GitHubApi.cpp:196 und GitHubOAuth.cpp:46; `lower`/`toLower` TeamSyncService.cpp:43 und StatusParser.cpp:12 (+ RepoConfig.cpp:73); Zeilen-/NUL-Splitter in GitCli.cpp:495-504, 668-679, 705-714, 736-741, 864-870, 800-829
- **Problem**: Je drei bis vier Kopien derselben Mini-Funktionen und fünf handgeschriebene Split-Schleifen. Jede kleine Abweichung (nur `'\n'/'\r'/' '` in `trimTrailing`, `isspace` in `trimmed`) ist unbeabsichtigtes Verhalten, das später auseinanderläuft.
- **Code-Auszug**:
```cpp
// GitCli.cpp:17
std::string trimTrailing(std::string s) { while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back(); return s; }
// GitInstall.cpp:34
std::string trimmedLine(const std::string& s) { ...  (s[e-1]=='\n'||'\r'||' ') ... }
```
- **Lösung**: Interner Header `src/ScText.h` (nicht installiert) mit `trimTrailing`, `wipe`, `pathFromUtf8`, `toLowerAscii`, `forEachSeparated(std::string_view, char sep, F)`; Aufrufer umstellen.
- **Erwarteter Effekt**: weniger Code (grob -80 Zeilen), ein Verhalten; keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering; trim-Semantik der drei Varianten vor dem Zusammenführen abgleichen (Whitespace-Satz). Bestehende Tests decken alles ab (`test_git_roundtrip.cpp`, `test_sourcecontrol_probe.cpp`).

### SCM-119 – GitHubApi: Header, Transportfehler und Fehlermapping dreifach
- **ID**: SCM-119
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_SourceControl/src/GitHubApi.cpp:93-101 vs. 123-134; 109-114, 188-190, 269-273, 335-339; 44-70 vs. 144-179
- **Problem**: `createRepo` baut denselben Header-Satz selbst, den `apiHeaders()` weiter unten (Zeile 123) liefert; der "could not reach"-Block steht viermal; `parseCreateRepoResponse` dupliziert das Fehlermapping von `mapCommonError` (nur Texte unterscheiden sich leicht). `listRepos` und `currentUser` rufen `httpsRequest` direkt statt über ein gemeinsames `apiGet`.
- **Code-Auszug**:
```cpp
if (!resp.ok) { if (err) *err = resp.error.empty() ? "could not reach api.github.com"
                                                 : "could not reach GitHub: " + resp.error; return false; }
```
- **Lösung**: `apiRequest(method, url, token, body, timeoutMs, err)` mit einer Fehlerzeile; `createRepo` über `apiPost` + `mapCommonError(…, "repo")`. Falls bestehende Tests die genauen 404/422-Texte von `createRepo` pinnen, den Unterschied als bewusst per-Funktion `missingScope`/`name exists`-Parameter erhalten.
- **Erwarteter Effekt**: ca. -60 Zeilen, einheitliche Meldungen.
- **Risiko / Verifikation**: Texte im UI/Test ändern sich evtl.; `test_github_oauth.cpp` und Parser-Tests prüfen Wortlaut.

### SCM-120 – `switchBranch` und `createBranch` fragen vorab 3-4 Prozesse ab
- **ID**: SCM-120
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitCli.cpp:626-655, 451-477
- **Problem**: `switchBranch` ruft `listBranches` (2 Prozesse, davon `branch --show-current` verworfen), danach `listRemoteBranches` (1 Prozess) und erst dann `git switch`. Es genügt zu wissen, ob `name` eine lokale Ref ist (`rev-parse --verify --quiet refs/heads/<name>`, existiert als `branchExists`) und ob `refs/remotes/<name>` existiert. `createBranch` fragt `check-ref-format`, `rev-parse`, ggf. `commitExists` und dann `switch/branch` (4 Prozesse), bewusst für saubere Meldungen.
- **Code-Auszug**:
```cpp
std::vector<std::string> locals; std::string current;
listBranches(root, locals, current);
const bool isLocal = std::find(locals.begin(), locals.end(), name) != locals.end();
```
- **Lösung**: `isLocal = branchExists(root, name)`; `isRemote = run(root,{"rev-parse","--verify","--quiet","refs/remotes/"+name})`. Statt zwei Listen und `std::find`. `local`-Zielprüfung ebenfalls über `branchExists`. Benutzerausgelöst, daher niedrige Priorität.
- **Erwarteter Effekt**: Schätzung: -1 bis -2 Prozess-Spawns (~50-100 ms unter Windows) je Branchwechsel.
- **Risiko / Verifikation**: Randfälle: Name mit `origin/` Präfix, der auch ein lokaler Branch `origin/x` sein könnte; Test in `test_git_roundtrip.cpp` für "nur remote", "lokal vorhanden", "lokal+remote", "Ref mit Slash".

### SCM-121 – Kleine Allokations- und Kopierstellen in `GitCli`
- **ID**: SCM-121
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitCli.cpp:97-103, 811-829, 735-741, 873-876
- **Problem**: (1) `runImpl` hält das Ergebnis als `const HE::Proc::Result r` und kopiert `r.out`/`r.err` in `GitResult` (bei `status` mit tausenden Einträgen mehrere hundert KB pro Poll) statt zu verschieben. (2) `log()` legt je Record einen neuen `std::vector<std::string> fields` und zerschneidet über `substr`-Kopien an; `commitFiles` baut zuerst `tok` komplett und kopiert dann in `ChangedFile`. (3) `usesLfs` liest jede `.gitattributes` per `istreambuf_iterator` Zeichen für Zeichen in einen String.
- **Code-Auszug**:
```cpp
const HE::Proc::Result r = HE::Proc::run(o);
...
g.out = r.out;  g.err = r.err;
```
- **Lösung**: `HE::Proc::Result r = ...; g.out = std::move(r.out);`. Parser auf `std::string_view`-Splits (`out.back()` direkt befüllen), `std::ifstream::read` in einen vorab dimensionierten String bzw. `std::filesystem::file_size`.
- **Erwarteter Effekt**: Schätzung: wenige µs bis ms pro Aufruf; nur der `status`-Move hat messbaren Nutzen. Keine Mikrooptimierung ohne Messung durchführen.
- **Risiko / Verifikation**: Gering; `r.launchFailed`/`r.timedOut` werden danach noch gelesen, also nur `out`/`err` verschieben.

### SCM-122 – Idle-Polling im Sync-Worker und Ignore-Matching mit Allokationen
- **ID**: SCM-122
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/TeamSyncService.cpp:626-631, 190-203, 749-755
- **Problem**: (1) Der Worker wacht im Leerlauf alle 500 ms auf, nur damit Retry-Zeitpunkte (`retryAtMs`) bedient werden; ohne anstehenden Retry gibt es keinen Grund. (2) `IgnoreRules::ignored` erzeugt je Eintrag eine `base`-Kopie (`relPath.substr`) auch für verankerte Regeln, und `scan()` baut bei jedem Aufruf die Regeln neu (Parsen von `gitignoreText()` plus `.gitignore` als Datei lesen -> `vector` -> `string`-Kopie). `glob` ist rekursiv (nicht exponentiell für die vorhandenen Muster, aber ohne Memoisierung).
- **Code-Auszug**:
```cpp
cv.wait_for(lock, std::chrono::milliseconds(500), [&] { ... });
...
const std::string base = slash == std::string::npos ? relPath : relPath.substr(slash + 1);
```
- **Lösung**: `wait_until(retryAtMs != 0 ? deadline : time_point::max())` bzw. `wait`; `base` als `std::string_view`/`const char*` in `relPath`, `glob` mit Zeiger statt Kopie; Regeln pro `Impl` cachen und nur bei geänderter `.gitignore`-mtime neu laden.
- **Erwarteter Effekt**: gering (2 Wakeups/s, wenige µs je Eintrag); unsicher/messen, nur mitnehmen, wenn SCM-103/104 angefasst werden.
- **Risiko / Verifikation**: Die 500-ms-Kommentarbegründung ("retries need that") und die Doppelprüfung `status.firstSyncDiffers.empty()` müssen erhalten bleiben; Tests in `test_team_sync.cpp` (Backoff, Pause).

### SCM-123 – Toter Code und Kommentar-Drift im Modul
- **ID**: SCM-123
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ScCommon.h:11, 28-38; GitProbe.cpp:91-101; GitCli.cpp:188-192; RepoStatus.h:139-142; StatusParser.cpp:249; RepoConfig.h:12-15; RepoStatus.h:94-96; ScLog.h:31-35, 119-121
- **Problem**: (1) `ProviderKind`/`providerName()` sind exportiert, werden aber nirgends im Repo verwendet (`grep` in `src/` und `tests/` ohne Treffer außer der Definition); `ScCommon.h` spricht von "three provider REST clients", es gibt nur GitHub. (2) `parsePorcelainV2` gibt immer `true` zurück, trotz Header-Kommentar "Returns false only on input that cannot be interpreted"; der Fehlerzweig `"could not interpret git status output"` in `GitCli::status` ist damit tot. (3) `RepoConfig.h` behauptet LFS-Routing für "paks, video" und "Authored graph formats ... diffable text/CBOR", der Code (`kMedia`, `.gitattributes`) enthält weder Paks noch Video und setzt `merge=binary` für die Graph-Formate. (4) `RepoStatus.h:94` (siehe SCM-112) beschreibt `generation` anders, als es sich verhält. (5) `ScLog.h` benutzt `std::snprintf`, inkludiert aber nicht `<cstdio>`; das Format-Puffer-Limit 2048 kürzt lange Zeilen stillschweigend.
- **Code-Auszug**:
```cpp
// StatusParser.cpp:248-249
buildDirtyFolders(out);
return true;
```
- **Lösung**: `ProviderKind`/`providerName` entfernen (oder bis zum ersten Nutzer verschieben); `parsePorcelainV2` auf `void` bzw. echten Fehlerfall (z. B. Header ohne `branch.oid` bei `--branch`) umstellen; Kommentare an den Code anpassen; `<cstdio>` ergänzen.
- **Erwarteter Effekt**: weniger Verwirrung, ca. -40 Zeilen; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Entfernen exportierter Symbole nur nach Grep in `src/HE_Editor` und `tests/` (oben: keine Nutzer); `test_git_status_parse.cpp` nutzt `parsePorcelainV2` (Rückgabewert prüfen).

### SCM-124 – `probeGit` startet 5 git-Prozesse sequenziell
- **ID**: SCM-124
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_SourceControl/src/GitProbe.cpp:155, 172, 181-188
- **Problem**: Beim Start/Recheck laufen `git --version`, `git lfs version`, `config --get user.name`, `config --get user.email`, `config --get credential.helper` einzeln hintereinander (je Timeout 5 s). Bei langsamem Startup (Antivirus, kaltes Dateisystem) addiert sich das spürbar im Start-Check.
- **Code-Auszug**:
```cpp
p.userName  = runCapture({ "config", "--get", "user.name" },  ok);
p.userEmail = runCapture({ "config", "--get", "user.email" }, ok);
p.credentialHelper = runCapture({ "config", "--get", "credential.helper" }, ok);
```
- **Lösung**: Ein Aufruf `git config --null --get-regexp "^(user\.(name|email)|credential\.helper)$"` und Parsen der NUL-getrennten `key\nvalue`-Paare (Mehrfach-`credential.helper`-Werte: bisher liefert `--get` den letzten, das beim Parsen nachbilden). Die beiden Versions-Calls bleiben getrennt. Schätzung 5 auf 3 Prozesse.
- **Erwarteter Effekt**: Schätzung: -2 Spawns, einmalig beim Start/Recheck (~50-100 ms unter Windows).
- **Risiko / Verifikation**: Semantik von `--get` (letzter Wert, Scope-Prioritäten) muss erhalten bleiben; `test_sourcecontrol_probe.cpp` mit Fixtures für mehrere `credential.helper`-Einträge.

---

## Cross-Modul-Hinweise

- `src/HE_Editor/SourceControlPanel.cpp:1756-1757` ruft `GitCli::isValidBranchName`/`branchExists` pro Frame im UI-Thread (SCM-102). Weitere Aufrufer von `GitCli::*` im Frame-Thread sollten per Grep auf `GitCli::` in `src/HE_Editor` geprüft werden (nicht Teil dieses Slices).
- `src/HE_Editor/TeamSyncView.cpp:58-143` und `TeamSyncController.cpp:187`: Pro-Frame-Kopien von `status()`, `lastResults()`, `lost()`, `history()` und unbegrenztes Zeichnen der Results ohne Clipping (SCM-113).
- `src/HE_Editor/SourceControlPanel.cpp:340, 1957` und `GitController.cpp:356`: Rebuilds an `RepoStatus::generation` gebunden (SCM-112). Der Poll-Intervall 3 s steht in `GitController.cpp:16-17`.
- `src/HE_Net` (`Net/ScClient.h`): ob `update()` blockierend warten kann (SCM-107), ob mehrere `put()` gleichzeitig offen sein dürfen (SCM-106) und ob `commit()` nach `stopping` noch sendet (SCM-109).

## Unsicher / Messung nötig

- SCM-101: tatsächliche Dauer je git-Aufruf (Windows vs. Linux, großes Repo); Nutzen von `status --show-stash` hängt von der Mindest-git-Version.
- SCM-103/104: Zeitanteil von `fs::relative` und des Walks an `scan()` bei 20k-100k Dateien (libstdc++ / MSVC / libc++ verhalten sich bei `weakly_canonical` unterschiedlich).
- SCM-107: CPU-Last des 1-ms-Pollings und mögliche Latenzzunahme bei Backoff.
- SCM-109: ob `ScClient::commit` bei gesetztem `stopping` die Anfrage tatsächlich noch an den Server sendet (nur mit Fake-Server-Test zu klären).
- SCM-112: Kosten des Modell-Rebuilds in `SourceControlPanel` je neuer Generation.
- SCM-115/122: Gewinn nur im Leerlauf/State-I/O, nicht gemessen.

## Gelesen

Alle 24 Dateien des Slice vollständig gelesen: ja (GitCli.cpp, GitHubApi.cpp, GitHubOAuth.cpp, GitHubTokenStore.cpp, GitInstall.cpp, GitLocator.cpp, GitProbe.cpp, GitService.cpp, RepoConfig.cpp, ScLog.h, StatusParser.cpp, TeamSyncService.cpp und alle 12 Header). Zusätzlich punktuell gelesen (nur für Aufrufer-Belege): `SourceControlPanel.cpp` (Zeilen 1725-1762), `GitController.cpp` (Poll-Konstanten), `TeamSyncView.cpp` (Zeilen 50-150), `TeamSyncController.cpp` (Zeile 187).
