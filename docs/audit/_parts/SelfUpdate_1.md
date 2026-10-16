# SelfUpdate Slice 1 – src/HE_SelfUpdate (alle 21 Dateien: include/SelfUpdate/*.h, src/*.cpp, updater/main.cpp)

## Zusammenfassung

Das Modul ist klein (ca. 3300 Zeilen), editor-only und läuft ausschließlich auf Worker-Threads bzw. einmalig beim Neustart (kein Frame-Loop-, ECS-, Render- oder Physik-Bezug). Die Struktur ist sauber (Transport-Interface, Plan/Result-Header ohne Engine-Abhängigkeit, plattformspezifischer Code in genau einem `#if`-Block). Es gibt keine Performance-Hotspots im Sinne des Briefs; die Funde betreffen vor allem (a) einen Thread-Fehler im Install-Pfad (Extraktion/Hash auf dem UI-Thread), (b) Windows-Pfadkonvertierungen, die die eigene UTF-8-Regel des Moduls verletzen, (c) Duplikate (Preflight/Begin, Atomic-Write, `startsWith`, Namens-Allow-List, Extract-Zweige) und (d) kleinere Robustheitslücken im Updater (Rollback ohne Retry, ungeprüfter Schreibfehler). Anzahl: 0 Hoch, 6 Mittel, 14 Niedrig. Alle Schweregrade sind wegen des Editor-only-/Einmal-Charakters bewusst niedrig angesetzt.

## Funde

### SUP-101 – `beginTakeover` (Hash + Entpacken bis 10 min) läuft auf dem UI-Thread
- **ID**: SUP-101
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_SelfUpdate/src/Takeover.cpp:107-155`, `SelfUpdateInstall.cpp:61-74`; Aufrufer `src/HE_Editor/SelfUpdateDialog.cpp:142` -> `EditorSelfUpdate.cpp:178-194`
- **Problem**: `Session::install()` ruft `m_backend.begin(o)` synchron im Aufrufer auf. Der kommt aus dem ImGui-Draw-Code (`SelfUpdateDialog.cpp:142`), also aus dem UI-Thread. `beginTakeover` hasht die komplette gestagte Datei erneut (`verifyStagedUpdate`, ca. 150 MB), startet `tar`/`hdiutil`/`ditto` und wartet mit `waitForExitMs = 10 min` (`runToolSync`). Der Editor friert für die ganze Dauer ein (Windows: "keine Rückmeldung"). Die Header-Kommentare (`SelfUpdateInstall.h:5-7`, `SelfUpdateCheck.h:4`) dokumentieren "blocking: worker threads only" nur für Check/Download, nicht für Takeover; `Takeover.h:59-64` sagt nichts über Blockieren.
- **Code-Auszug**:
```cpp
so.waitForExitMs = 10 * 60 * 1000;           // SelfUpdateInstall.cpp:68
const ExtractResult extracted = extractStagedBuild(staged.filePath, workRoot / "payload", staged.platform); // Takeover.cpp:154
```
- **Lösung**: Extraktion in den Download-Worker (direkt hinter `downloadUpdate`/Preflight) verlegen und das Ergebnis (`payload`-Verzeichnis) im Staging halten; `beginTakeover` macht dann nur noch Plan bauen + Updater kopieren + spawnen (Millisekunden). Alternativ `install()` auf `runWorker` umstellen und per Snapshot-Phase `Installing` den Erfolg melden (UI beendet sich im Erfolgsfall aus dem Poll). Im Header "Blocking" dokumentieren. Hinweis: Das Payload muss Geschwister von `installRoot` liegen (gleiches Volume), die Extraktion kann also erst nach Kenntnis von `installRoot` (`currentExecutablePath()`, steht schon im Preflight) stattfinden.
- **Erwarteter Effekt**: (Schätzung) UI-Freeze von mehreren Sekunden bis Minuten (Hash 0,3-1 s + Entpacken 150 MB je nach Platte/AV 2-60 s) entfällt.
- **Risiko / Verifikation**: Zustandsautomat der `Session` muss die Phase "Extracting" kennen; Aufräumen bei Abbruch (Payload-Verzeichnis) beachten. Messen: Dauer von `extractStagedBuild` auf Windows mit Defender. Bestehende Tests der Session/Takeover (`tests/` – Namen per `grep -l SelfUpdate tests/*.cpp`) erweitern.

### SUP-102 – `path.string() + ".tmp"/".partial"` verletzt die UTF-8-Pfadregel des Moduls (Windows)
- **ID**: SUP-102
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateCheck.cpp:70`, `SelfUpdateTransport.cpp:32` und `:67`
- **Problem**: `SelfUpdateProcess.h:24-27` begründet `pathFromUtf8/utf8FromPath` genau damit, dass ein Benutzername mit Umlaut unter Windows sonst einen anderen Pfad ergibt. Das Staging-Verzeichnis liegt aber unter dem per-User-Datenverzeichnis (`defaultStagingDir()` -> `GlobalState::userDataDir()`), enthält also typischerweise den Benutzernamen. `fs::path::string()` liefert unter Windows die ANSI-Codepage (nicht darstellbare Zeichen werden `?` bzw. werfen je nach STL), `fs::path(narrow)` liest sie wieder in der ACP: der Pfad kann auf eine andere Datei zeigen oder nicht existieren. Betroffen: Temp-Datei von `writeFileAtomically` (pending.json), `.partial` im LocalDirTransport und der an `sftpGetFile` übergebene `localPath.string()`.
- **Code-Auszug**:
```cpp
const fs::path tmp = path.string() + ".tmp";                         // Check.cpp:70
const fs::path partial = localPath.string() + ".partial";            // Transport.cpp:67
HE::Cs::sftpGetFile(m_endpoint, remoteRelPath, localPath.string(), ...) // Transport.cpp:32
```
- **Lösung**: `fs::path tmp = path; tmp += ".tmp";` (wie schon `SelfUpdateApplyCore.cpp:232-233`). Für den SFTP-Aufruf: `sftpGetFile` nimmt `const std::string&` (`SftpClient.h:77`) und macht intern selbst `local.string() + ".partial"` (`SftpClient.cpp:337-341`) -> dort derselbe Fehler; Signatur auf `fs::path` ändern oder UTF-8 vereinbaren (`utf8FromPath`) und intern `pathFromUtf8`.
- **Erwarteter Effekt**: Korrektheit auf Windows-Konten mit Nicht-ASCII-Namen; keine Laufzeitänderung.
- **Risiko / Verifikation**: Exakter Fehlermodus (`?`-Ersetzung vs. Exception) hängt von der MSVC-STL ab: unsicher, auf Windows mit Benutzer "Jörg" testen. Linux/macOS (UTF-8) unverändert. Test: `LocalDirTransport` + `writeFileAtomically` mit Verzeichnis `u8"ü"`.

### SUP-103 – Rollback nach Health-Fehlschlag verschiebt die neue Installation ohne Retry
- **ID**: SUP-103
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateApplyCore.cpp:172-186`
- **Problem**: Alle anderen Renames gehen durch `retryRename` (Virenscanner, Indexer, `EBUSY`, `ERROR_SHARING_VIOLATION`). Im Rollback ist `fs::rename(install, payload, mec)` ein einzelner Versuch mit ignoriertem Fehler. Der Fall tritt genau dann auf, wenn der neue Editor gerade gestartet und gecrasht ist (frisch geöffnete DLLs, Crash-Dump-Handler, AV-Scan des neuen Prozesses) – die Wahrscheinlichkeit für ein gehaltenes Handle ist dort am höchsten. Scheitert er, scheitert auch das folgende `retryRename(backup, install)` (Ziel existiert noch, nicht leer) nach 30 s und das Ergebnis ist `Failed` ohne laufenden Editor, obwohl der Rollback mit einem Retry geklappt hätte.
- **Code-Auszug**:
```cpp
std::error_code mec;
fs::rename(install, payload, mec);   // best-effort; install is left as-is if this fails too
```
- **Lösung**: `std::string moveErr; retryRename(install, payload, plan.renameRetryMs, moveErr);` Bei endgültigem Fehler im Text von `Failed` mit ausgeben (aktuell wird `mec` verworfen, die Meldung nennt nur den zweiten Fehler).
- **Erwarteter Effekt**: Robusterer Rollback auf Windows; kein Mehraufwand im Normalfall (erster Versuch gelingt).
- **Risiko / Verifikation**: Verhalten ändert sich nur im Fehlerfall (verlängert diesen um bis zu `renameRetryMs`). Test mit gehaltenem Handle (Windows) bzw. Fake über ein Verzeichnis, das nach X ms freigegeben wird; die bestehenden ApplyCore-Tests (in-process, siehe Header-Kommentar) um "Neuer Build stirbt + Install gesperrt" erweitern.

### SUP-104 – `preflightTakeover` und `beginTakeover` duplizieren ~25 Zeilen Vorprüfung
- **ID**: SUP-104
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `Takeover.cpp:65-105` vs. `:107-136`
- **Problem**: Beide Funktionen lesen `pending.json`, verifizieren (Hash), bestimmen `currentExe`, `installRoot`, prüfen `updaterSrc` mit identischem Code und identischen Fehlertexten. Preflight prüft zusätzlich `directoryIsWritable`, Begin nicht (Drift-Risiko: Begin ist die Stelle, an der es zählt, und gerät bei Änderungen am Preflight leicht auseinander).
- **Code-Auszug**:
```cpp
if (!readPendingUpdate(options.stagingRoot, staged, error)) { ... }
if (!verifyStagedUpdate(staged, error)) { ... }
const fs::path currentExe = currentExecutablePath();
...
const fs::path installRoot = installRootForRunningBuild(currentExe, staged.platform);
```
- **Lösung**: Interne Struktur `struct TakeoverContext { StagedUpdate staged; fs::path currentExe, installRoot, updaterSrc; }` und `bool resolveContext(const TakeoverOptions&, TakeoverContext&, std::string& error, bool verifyHash)`; `preflightTakeover` = `resolveContext` + `directoryIsWritable`, `beginTakeover` = `resolveContext` + Rest. ~30 Zeilen weniger.
- **Erwarteter Effekt**: Weniger Code, ein Ort für die Fehlertexte.
- **Risiko / Verifikation**: Fehlertexte müssen gleich bleiben (ggf. in Tests verglichen). Bestehende Takeover-Tests genügen.

### SUP-105 – Gestagte Datei wird bis zu dreimal vollständig gehasht
- **ID**: SUP-105
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `SelfUpdateCheck.cpp:323-327` (`verifyStagedUpdate`), Aufrufe `:377`, `:428`; `Takeover.cpp:76` (Preflight) und `:118` (Begin); `Takeover.h:47-50`
- **Problem**: Ablauf eines Updates: `downloadUpdate` hasht nach dem Download (`:428`) -> derselbe Worker ruft sofort `preflightTakeover` auf (`EditorSelfUpdate.cpp:166`) -> erneuter Voll-Hash (`Takeover.cpp:76`) -> beim Klick `beginTakeover` -> dritter Voll-Hash. Zusätzlich hasht "Auf Updates prüfen" bei bereits gestagtem Build (`:377`) noch einmal. `Takeover.h:49-50` nennt den Preflight "cheap enough to call before showing 'Restart to update?'" – das stimmt nicht, wenn er 150 MB liest. Das zweite Hashen in `beginTakeover` ist sicherheitsrelevant gewollt (TOCTOU zwischen Download und Install, `SelfUpdateCheck.h:149-152`), der unmittelbar folgende Preflight direkt nach dem Download dagegen redundant.
- **Code-Auszug**:
```cpp
if (!verifyStagedUpdate(staged, error)) {            // Takeover.cpp:76 und :118
    result.error = "the staged build failed verification: " + error;
```
- **Lösung**: `TakeoverOptions` um `bool verifyHash = true` erweitern (oder zwei Funktionen) – Preflight direkt nach Download ruft ohne Hash (nur Größe, `readPendingUpdate` prüft Größe schon), `beginTakeover` behält den Hash. Im Header ehrlich beschreiben. Flag-Parameter (Kategorie A) vermeiden, wenn stattdessen `preflightTakeover` grundsätzlich nur `readPendingUpdate` nutzt und Hash nur in `beginTakeover` – das ist die einfachste Variante und passt zu "cheap".
- **Erwarteter Effekt**: (Schätzung) 0,3-1 s Worker-Zeit gespart (SHA-256 in Software ca. 200-500 MB/s, `Sha256.cpp:134-152` nutzt 64-KiB-Puffer über `ifstream`; keine Hardware-Beschleunigung sichtbar). Kein Frame-Einfluss.
- **Risiko / Verifikation**: Preflight meldet dann nicht mehr "hash mismatch" vorab (kommt erst im Begin, Dialog zeigt Fehler dann beim Klick). Messen: tatsächliche Hash-Zeit für 150 MB auf Zielplattformen.

### SUP-106 – `writeApplyResult` prüft den Schreibfehler nicht; Atomic-Write doppelt implementiert
- **ID**: SUP-106
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `SelfUpdateApplyCore.cpp:232-249` vs. `SelfUpdateCheck.cpp:68-85`
- **Problem**: Der Header (`SelfUpdateApplyCore.h:33-34`) verspricht, dass ein Schreibfehler (Platte voll) gemeldet wird. In `writeApplyResult` wird nach `f << j.dump(2)` weder `f` geprüft noch geschlossen; bei Platte-voll wird eine leere/abgeschnittene `last-apply.json` umbenannt und `true` zurückgegeben – der nächste Editor liest "malformed result file" oder gar nichts statt des Grundes. Bei fehlgeschlagenem Rename bleibt `.tmp` liegen (die Check-Variante räumt auf). Beide Funktionen sind sonst dasselbe Muster.
- **Code-Auszug**:
```cpp
std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
...
f << j.dump(2);        // ApplyCore.cpp:241 – Zustand von f wird nie geprüft
}
fs::rename(tmp, p, ec);
```
- **Lösung**: Ein gemeinsamer `writeFileAtomically(const fs::path&, const std::string&, std::string& error)` in der standalone-fähigen Datei (z. B. `SelfUpdateProcess.cpp` oder neue `SuFile.cpp`, ohne Engine-Abhängigkeit, weil HorizonUpdater sie ebenfalls braucht – vgl. CMake `SelfUpdateProcess.cpp`/`ApplyCore.cpp` in `HorizonUpdater`) mit `out.flush(); if (!out) ...` und `.tmp`-Aufräumen; beide Stellen nutzen sie. Gleichzeitig `readWholeFile` (Check.cpp:58-66) dorthin.
- **Erwarteter Effekt**: Ein Fehlerpfad weniger, Verhalten entspricht endlich der Dokumentation.
- **Risiko / Verifikation**: Updater-CMake (`src/HE_SelfUpdate/CMakeLists.txt`, Zeilen ca. 156-170) muss die neue Datei kennen. Test: Schreiben in volles Dateisystem schwer; per `/dev/full` (Linux) als `resultFile`-Ziel prüfbar.

### SUP-107 – `applyUpdate` startet den neuen Editor mit Copy-Paste von `relaunch()`; `launcher` ist ein ungeprüfter String
- **ID**: SUP-107
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateApplyCore.cpp:72-95` und `:188-209`; `SelfUpdateApplyPlan.h:55`, `:125`
- **Problem**: Der "exec"-Zweig in `applyUpdate` (SpawnOptions mit `exe/args/cwd/logFile`) wiederholt den "exec"-Zweig von `relaunch()` und fügt nur `waitForExitMs` hinzu. `launcher` ist `std::string` ("exec"/"open") und wird in `fromArgs` nicht validiert: Tippfehler (`--launcher opne`) fällt still auf "exec" zurück (unter macOS startet das dann die Mach-O direkt statt über LaunchServices).
- **Code-Auszug**:
```cpp
if (launcher == "open") {...} else { so.exe = root / pathFromUtf8(mainExe); so.args = relaunchArgs; }  // :78-93
...
SpawnOptions so; so.exe = install / mainExeRel; so.args = plan.relaunchArgs; so.cwd = install; ... // :198-204
```
- **Lösung**: `relaunch(..., std::uint32_t waitForExitMs = 0)`; `applyUpdate` ruft für beide Launcher `relaunch(install, ..., plan.launcher == "open" ? 0 : plan.healthMs)` auf. `enum class Launcher { Exec, Open }` in `ApplyPlan`, `toArgs/fromArgs` wandeln, unbekannter Wert -> Fehler wie bei unbekannten Flags.
- **Erwarteter Effekt**: ~12 Zeilen weniger, Fehlkonfiguration wird sichtbar.
- **Risiko / Verifikation**: Kommandozeilenformat muss stabil bleiben ("exec"/"open" als Text). Round-Trip-Test `toArgs -> fromArgs` mit beiden Werten und einem Tippfehler.

### SUP-108 – `extractStagedBuild`: Windows- und Linux-Zweig sind fast identisch; Funktion ~100 Zeilen
- **ID**: SUP-108
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateInstall.cpp:120-217` (Zweige `:127-140` und `:141-154`)
- **Problem**: Beide Zweige: Tool suchen, `extract/` anlegen, `runToolSync(tar, {flag, file, "-C", into})`, `collapseSingleTopLevelDir`. Unterschiede nur Tool-Kandidaten, `-xf`/`-xzf`, Meldungstexte. macOS-Zweig (mount/ditto/detach) steht inline dazwischen.
- **Code-Auszug**:
```cpp
if (!runToolSync(tar, { "-xf",  utf8FromPath(artifactFile), "-C", utf8FromPath(into) }, destParentDir, err))   // :134
if (!runToolSync(tar, { "-xzf", utf8FromPath(artifactFile), "-C", utf8FromPath(into) }, destParentDir, err))  // :148
```
- **Lösung**: `extractTar(artifact, destParent, tarCandidates, flag, toolLabel)` und `extractDmg(...)` als eigene Funktionen; `extractStagedBuild` wird zur Verteilung + gemeinsamem Schlussprüfen von `mainExe`. Optional `-xf` auch für Linux: GNU tar erkennt gzip beim Lesen selbst (Verhalten dann geringfügig anders, nicht nötig).
- **Erwarteter Effekt**: ~15 Zeilen weniger, bessere Lesbarkeit.
- **Risiko / Verifikation**: Reines Refactoring; Plattform-Tests laufen nur nativ (macOS-Zweig in Linux-CI nicht testbar -> Review).

### SUP-109 – Allow-List-Prüfungen nutzen locale-abhängige `<cctype>`-Funktionen
- **ID**: SUP-109
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateManifest.cpp:15-30` (`std::isalnum`, `std::islower`); `SelfUpdateVersion.cpp:17,42` (`std::isdigit`)
- **Problem**: Die Sicherheitsaussage von `isSafeRelativePath` ("nur `[A-Za-z0-9._+-]`", `SelfUpdateManifest.h:75-78`) gilt nur im "C"-Locale; `isalnum/islower` sind für Bytes >127 locale-abhängig. Aktuell ruft im Repo niemand `setlocale`/`std::locale::global` auf (grep über `src/` ohne Treffer, Vendor ausgenommen), es ist also latent, aber ein Einbinden einer Library, die `setlocale(LC_ALL, "")` ruft, würde die Allow-List aufweichen. Ohne Nutzen für die Mehrdeutigkeit der Locale-Abfrage.
- **Code-Auszug**:
```cpp
return std::isalnum(u) || c == '.' || c == '_' || c == '+' || c == '-';   // :18
```
- **Lösung**: ASCII-only Hilfsfunktionen `isAsciiAlnum(char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z'); }` etc. (constexpr, ein Satz in `SuCommon.h` oder anonymem Namespace); `<cctype>` entfällt. Gleiches für `isHex64`, `parseField`, `allDigits`, Kleinschreibung (`SelfUpdateManifest.cpp:181`, `SelfUpdateEndpoint.cpp:34`).
- **Erwarteter Effekt**: Deterministisches Verhalten unabhängig von Locale, minimal weniger Code.
- **Risiko / Verifikation**: Keines bei ASCII-Eingaben; Test mit Bytes 0x80-0xFF in Pfaden (soll ablehnen).

### SUP-110 – "Name ohne Slash und sicher" 3x kopiert, `isSafeRelativePath` alloziert pro Segment
- **ID**: SUP-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateManifest.cpp:141`, `SelfUpdateEndpoint.cpp:79`, `SelfUpdateCheck.cpp:355`; `SelfUpdateManifest.cpp:70-90`
- **Problem**: `x.find('/') != npos || !isSafeRelativePath(x)` steht dreimal (Version, Channel, dirName) – gemeint ist "einzelnes sicheres Segment". `isSafeRelativePath` baut pro Segment ein `std::string` (`:79 path.substr`), obwohl `std::string_view` genügt. Nicht heiß (Manifest ~ 10 Pfade), nur Lesbarkeit.
- **Code-Auszug**:
```cpp
if (m.version.find('/') != std::string::npos || !isSafeRelativePath(m.version))   // Manifest.cpp:141
```
- **Lösung**: `HE_SU_API bool isSafeNameSegment(std::string_view)` (kein '/', nicht leer, Allow-List) neben `isSafeRelativePath`, das darauf aufbaut (`for each segment via string_view`). Die drei Aufrufer nutzen sie.
- **Erwarteter Effekt**: Weniger Wiederholung; keine messbare Laufzeit.
- **Risiko / Verifikation**: Gleiche Tests wie für `isSafeRelativePath` (Traversal, Backslash, ':', trailing '.').

### SUP-111 – `fs::temp_directory_path(ec)` ohne Fehlerprüfung (3 Stellen)
- **ID**: SUP-111
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateCheck.cpp:175-177`, `Takeover.cpp:157` und `:193`
- **Problem**: Bei Fehler liefert `temp_directory_path(ec)` einen leeren Pfad. Check: Manifest-Scratchdatei landet dann relativ im CWD (`"he-selfupdate-manifest-….json"`), Takeover: `HorizonUpdater-…` wird relativ kopiert/gestartet. Zudem wird `temp_directory_path` zweimal in `beginTakeover` abgefragt.
- **Code-Auszug**:
```cpp
fs::path scratchDir = options.scratchDir.empty() ? fs::temp_directory_path(ec) : options.scratchDir;  // Check.cpp:176
const fs::path updaterTmp = fs::temp_directory_path(ec) / (...)   // Takeover.cpp:157
so.cwd = fs::temp_directory_path(ec);                             // Takeover.cpp:193
```
- **Lösung**: Einmal ermitteln, leeren Wert als Fehler ("no temp directory") melden; in `beginTakeover` die Variable `tmpDir` für Updater-Kopie und `cwd` nutzen.
- **Erwarteter Effekt**: Fehler statt Schreiben in unerwartetes CWD; eine Syscall-/Env-Abfrage weniger.
- **Risiko / Verifikation**: Praktisch nie ausgelöst (selten leeres TEMP); Test nur durch Setzen von `TMPDIR=""` unter Linux.

### SUP-112 – Kopie von HorizonUpdater im Temp wird nie gelöscht, auch nicht im Fehlerpfad
- **ID**: SUP-112
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `Takeover.cpp:146-152`, `:157-165`, `:195-198`
- **Problem**: `cleanupAndFail` entfernt nur `workRoot`. Scheitert `spawnDetached` oder beendet sich der Updater sofort (`:196-198`), bleibt `HorizonUpdater-<version>-<hex>(.exe)` im Temp liegen. Im Erfolgsfall kann der Updater sich selbst unter Windows nicht löschen und nichts räumt später auf; jedes Update hinterlässt eine Kopie (Größe unbekannt, vermutlich < 1 MB). `pruneTakeoverLeftovers` kennt nur `.heselfupdate-*` neben der Installation.
- **Code-Auszug**:
```cpp
fs::copy_file(updaterSrc, updaterTmp, fs::copy_options::overwrite_existing, ec);   // :164
...
if (!started.ok) return cleanupAndFail(...);   // updaterTmp bleibt
```
- **Lösung**: Im Fehlerpfad `fs::remove(updaterTmp, rec)`; beim Editor-Start in `pruneTakeoverLeftoversForRunningBuild` zusätzlich `HorizonUpdater-*` im Temp-Verzeichnis löschen, sofern älter als `grace` (läuft schon auf Worker). Alternativ die Kopie ins `workRoot` legen (dann räumt der bestehende Prune sie mit ab; Windows: Updater läuft aus `workRoot`, muss aber bei `rename(install, backup)` nicht blockieren – `workRoot` ist nicht `installRoot`, passt).
- **Erwarteter Effekt**: Keine Datei-Akkumulation; Letztere Lösung vereinfacht (keine zweite Aufräumstelle).
- **Risiko / Verifikation**: Wenn Updater im `workRoot` liegt, würde `backup/` + Updater-Exe in einem Verzeichnis liegen, das der Updater später von `pruneTakeoverLeftovers` löschen lässt – nur nach Ende des Updaters (grace 15 min). Test: Prune-Test um Updater-Kopie erweitern.

### SUP-113 – Mehrfach vorhandene Hilfsfunktionen und ein unnötiger Header-Include
- **ID**: SUP-113
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateInstall.cpp:12-16` = `Takeover.cpp:17-21` (`startsWith`); `SelfUpdateCheck.cpp:49-56` ~ `Takeover.cpp:23-29` (`randomHex`/`randomSuffix`); `Takeover.h:19`; `SelfUpdateApplyCore.cpp:10` (`<sstream>` ungenutzt)
- **Problem**: (1) `startsWith` doppelt, in C++20 ersetzbar durch `std::string::starts_with` (`platformKey.starts_with("macos")`). (2) Zwei Zufalls-Hex-Generatoren mit je 2 `std::random_device`-Aufrufen; `randomSuffix` gibt unpadded Hex aus (`rd() << rd()` konkateniert variabel lange Blöcke, `"1"+"23"` == `"12"+"3"`; Kollisionswahrscheinlichkeit minimal, aber die Länge ist nicht fix). (3) `Takeover.h` inkludiert `SelfUpdateCheck.h`, nutzt daraus nichts (Typen im Header: `TakeoverOptions`, `PruneOptions`…); dadurch zieht jeder Takeover-Konsument `SelfUpdateEndpoint.h` -> `ContentSync/SftpCredentials.h`, `SelfUpdateTransport.h`, `SelfUpdateManifest.h` mit. Nur die `.cpp` braucht `runningBuild()`/`readPendingUpdate`. (4) `<sstream>` in `SelfUpdateApplyCore.cpp` wird nicht verwendet (kein `stringstream`); `<ctime>`/`<cstdio>` werden genutzt.
- **Code-Auszug**:
```cpp
#include "SelfUpdate/SelfUpdateCheck.h"    // Takeover.h:19
os << std::hex << rd() << rd();            // Takeover.cpp:27
```
- **Lösung**: `starts_with` nutzen; ein `randomHex64()` (z. B. in `SelfUpdateProcess.cpp`, standalone-fähig) mit `snprintf("%016llx")`; `#include "SelfUpdate/SelfUpdateCheck.h"` nach `Takeover.cpp` verschieben; `<sstream>` aus ApplyCore entfernen.
- **Erwarteter Effekt**: Kürzere Compile-Zeit der Konsumenten (gering), weniger Duplikat.
- **Risiko / Verifikation**: Konsumenten, die über `Takeover.h` indirekt `SelfUpdateCheck.h` bekamen, müssen es selbst inkludieren – Build prüfen (`EditorSelfUpdate.cpp` nutzt beides wahrscheinlich ohnehin).

### SUP-114 – `buildConfig`: Port wird als String durchgereicht, von Hand geparst; Magic Number in `scrubSecret`
- **ID**: SUP-114
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateEndpoint.cpp:61-75`, `:180-181`, `:137-142`
- **Problem**: Die eingebaute Konfiguration wird zuerst via `std::to_string(builtIn.port)` in einen String gewandelt, nur um in `buildConfig` wieder von Hand (`isdigit`-Schleife + `strtoul`) geparst zu werden. Der Fehlertext nennt dabei `HE_SELFUPDATE_PORT` auch für den Fall, dass die Zahl aus dem Build stammt. `scrubSecret` hat `pos += 10; // length of "<redacted>"` als Literal.
- **Code-Auszug**:
```cpp
const RawAccount baked { builtIn.host, builtIn.username, builtIn.password, std::to_string(builtIn.port), ... }; // :180
text.replace(pos, secret.size(), "<redacted>"); pos += 10;                                                       // :140-141
```
- **Lösung**: `RawAccount::port` als `std::optional<unsigned long>`-ähnlicher Wert; Umgebungspfad parst mit `std::from_chars` (kein Locale, kein Overflow-Trick über `size()<=5`); Built-in übergibt die Zahl direkt. In `scrubSecret` `constexpr std::string_view kMask = "<redacted>"` und `pos += kMask.size()`.
- **Erwarteter Effekt**: ca. 10 Zeilen weniger, korrekte Fehlermeldung je Quelle.
- **Risiko / Verifikation**: Bestehende Endpoint-Tests (Port "0", "65536", "22a", " 22") unverändert grün halten.

### SUP-115 – `applyUpdate` ist eine lange Funktion mit drei Lambdas, die per Referenz erfassen
- **ID**: SUP-115
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateApplyCore.cpp:99-213`
- **Problem**: ~115 Zeilen, Zustand liegt in `result` und drei Lambdas (`finish`, `abortButRelaunchInstall`, `rollback`). Die Reihenfolge der Seiteneffekte (wer schreibt wann `result.newPid`) ist schwer zu überblicken. Der Ablauf ist ein linearer Zustandsautomat (Wait -> Validate -> Backup -> Swap -> Launch -> Health).
- **Code-Auszug**:
```cpp
const auto finish = [&](ApplyStatus s, std::string err) {...};
const auto abortButRelaunchInstall = [&](std::string err) {...};
const auto rollback = [&](std::string why) {...};
```
- **Lösung**: `class Applier { const ApplyPlan& plan; ApplyResult result; ... ApplyResult run(); bool validate(); bool swap(); ApplyResult launchAndWatch(); ApplyResult rollback(std::string); }` in der anonymen Namespace der `.cpp` (öffentliche API `applyUpdate` bleibt). Keine Verhaltensänderung.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit der Einzelphasen. Keine Laufzeit.
- **Risiko / Verifikation**: Hohes Regressionsgewicht (Updater ist der einzige Weg zurück); nur mit vollständigen bestehenden ApplyCore-Tests refactoren, kein Gewinn, wenn diese fehlen -> dann lieber lassen.

### SUP-116 – Versionsvergleich: viele kurze Allokationen und doppeltes Parsen
- **ID**: SUP-116
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateVersion.cpp:24-36`, `:47-76`, `:87-116`; `SelfUpdateCheck.cpp:204-206`
- **Problem**: `split` liefert `vector<string>` mit Teilkopien; `comparePrerelease` kopiert Identifier (`std::string x = pa[i]`) und entfernt führende Nullen per wiederholtem `erase(begin())` (quadratisch in der Länge, hier irrelevant). In `checkForUpdate` wird die Manifest-Version nach `parseUpdateManifest` ein zweites Mal geparst (`parseVersion(result.manifest.version, remoteVersion); // vouched for it`), Rückgabewert ignoriert. Laufzeit liegt bei Mikrosekunden einmal pro Check -> nur Vereinfachung.
- **Code-Auszug**:
```cpp
while (x.size() > 1 && x.front() == '0') x.erase(x.begin());   // :60
parseVersion(result.manifest.version, remoteVersion);           // Check.cpp:205
```
- **Lösung**: `std::string_view` + `find` statt `split`-Vektor; führende Nullen per `substr(first_nonzero)` (View) überspringen; `UpdateManifest` hält zusätzlich `Version parsedVersion` (von `parseUpdateManifest` gefüllt), `checkForUpdate` verwendet es.
- **Erwarteter Effekt**: Weniger Code; Laufzeit nicht messbar relevant.
- **Risiko / Verifikation**: Semver-Vergleichstests (rc.1 < rc.2 < rc.10, numerisch < alphanumerisch, Länge) unverändert.

### SUP-117 – POSIX `processExists`: `ifstream`+`ostringstream` pro Poll, 50-ms-Polling bis 120 s
- **ID**: SUP-117
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateProcess.cpp:376-411`, `:352-370`
- **Problem**: Jeder Poll (bis 2400-mal bei 120 s) öffnet `/proc/<pid>/stat`, kopiert über `ostringstream` und sucht `rfind(')')`. Das Warten auf den Editor in `waitForProcessExit` und das Gesundheitsfenster (20 ms Poll bis `healthMs`) sind Busy-Waits mit Sleep; verbrauchen praktisch keine CPU. Eine Umstellung auf `pidfd_open`/`kqueue(EVFILT_PROC)` wäre exakter, aber plattformspezifisch und ohne messbaren Gewinn – nicht empfohlen.
- **Code-Auszug**:
```cpp
std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
std::ostringstream os; os << stat.rdbuf();
```
- **Lösung**: Nur kosmetisch: `std::string text; std::getline(stat, text);` statt `ostringstream` (eine Zeile reicht, `<sstream>` im POSIX-Zweig entfällt).
- **Erwarteter Effekt**: Minimal; Vereinfachung um 2 Zeilen.
- **Risiko / Verifikation**: `comm` darf keinen Zeilenumbruch enthalten (Kernel maskiert nicht, kann aber theoretisch `\n` im Prozessnamen haben – `getline` würde dann vor `)` abbrechen; dann `read` bis EOF beibehalten). Dann besser belassen.

### SUP-118 – `copyPreserved`: toter Copy-Option, `".."`-Teilstringprüfung, absolute Pfade nicht abgefangen
- **ID**: SUP-118
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateApplyCore.cpp:58-70`
- **Problem**: (1) `dst` existiert nachweislich nicht (Zeile 66 `continue` sonst), `copy_options::overwrite_existing` ist also wirkungslos. (2) `rel.find("..") != npos` weist auch legitime Namen wie `a..b.log` ab und lässt dagegen absolute Pfade (`/etc/passwd`, `C:\x`) zu: `fromRoot / absolute` ergibt den absoluten Pfad selbst, `toRoot / absolute` ebenfalls -> Kopie von `src` auf sich selbst bzw. in Fremdverzeichnis. Quelle der Liste ist der Editor (`preserveListFor`, fest einkompiliert) -> kein Angriffsvektor, nur Inkonsistenz zur Prüfung von `mainExe` (`:144`, dort `is_absolute()` geprüft).
- **Code-Auszug**:
```cpp
if (rel.empty() || rel.find("..") != std::string::npos) continue;
fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
```
- **Lösung**: Gemeinsamer Helfer `isSafeRelative(const std::string&)` (nicht leer, `!pathFromUtf8(x).is_absolute()`, kein Segment `..` via `lexically_normal`/Segmentiteration) für `mainExe` und `preserve`; `overwrite_existing` entfernen. (Aus der Bibliothek `isSafeRelativePath` zu verwenden ist im Updater nicht möglich, wenn `SelfUpdateManifest.cpp` nicht standalone gelinkt wird – Helper deshalb in `ApplyPlan.h` oder `ApplyCore.cpp` selbst.)
- **Erwarteter Effekt**: Konsistente Pfadprüfung, 1 Option weniger.
- **Risiko / Verifikation**: Verhalten für die heutige Liste (`config.json`, `imgui.ini`, `HorizonEngine*.log`) identisch; Test mit `..`, `a..b`, absolutem Pfad.

### SUP-119 – Manifest wird über eine Zufalls-Temp-Datei geladen statt im Speicher
- **ID**: SUP-119
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateCheck.cpp:173-194`, `SelfUpdateTransport.h:51-53`
- **Problem**: Das Interface kennt nur "Datei holen", daher: Temp-Pfad bauen (`randomHex`), laden, komplett lesen (`readWholeFile`), löschen. Das sind Dateisystem-Roundtrip, Zufallszahl, Cleanup in allen Pfaden plus die Fehlerbehandlung `readOk` – für ~1 KB. Netzwerk dominiert, daher keine Performance-Relevanz; es ist Komplexität, die eine In-Memory-Variante sparen würde. `sftpGetFile` schreibt aber selbst nur in Dateien (`SftpClient.h:77`), ein `fetchText` bräuchte dort eine neue Funktion – daher nur bei ohnehin anstehender Änderung von SftpClient lohnend.
- **Code-Auszug**:
```cpp
const fs::path scratch = scratchDir / ("he-selfupdate-manifest-" + randomHex() + ".json");
const SuResult fetched = fetchWithRetry(transport, manifestRemotePath, scratch, {}, options);
std::string text; const bool readOk = fetched.ok && readWholeFile(scratch, text);
fs::remove(scratch, ec);
```
- **Lösung**: `virtual SuResult fetchText(const std::string& relPath, std::string& out)` im Interface (Default-Implementierung per Datei, `LocalDirTransport` direkt) oder belassen. Empfehlung: belassen, bis `SftpClient` einen Speicher-Sink bekommt.
- **Erwarteter Effekt**: Wenige Zeilen weniger, `CheckOptions::scratchDir` entfiele.
- **Risiko / Verifikation**: `CheckOptions::scratchDir` ist öffentliche API (Tests). Nur ändern, wenn Tests angepasst werden.

### SUP-120 – Redundante `std::function`-Konvertierung und weitere Include-Details
- **ID**: SUP-120
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `SelfUpdateTransport.cpp:32-33`; `SelfUpdateTransport.h:21`, `SelfUpdateEndpoint.h:51`; `SelfUpdateProcess.cpp:4`
- **Problem**: `SuProgressFn` und `HE::Cs::SftpProgressFn` sind beide `std::function<void(std::uint64_t, std::uint64_t)>` (`SftpClient.h:67`, `SelfUpdateTransport.h:42`); `progress ? SftpProgressFn(progress) : SftpProgressFn()` kopiert die `std::function` (ggf. Heap-Allokation des Callable) für nichts. Auch ein leeres `progress` ist bereits ein leeres `SftpProgressFn`. `SelfUpdateTransport.h` zieht `ContentSync/SftpCredentials.h` nur für die Signatur von `makeSftpTransport` in alle Konsumenten (Forward-Declaration von `HE::Cs::SftpEndpoint` genügt dort; die .cpp inkludiert ohnehin `SftpClient.h`). `<cstring>` in `SelfUpdateProcess.cpp` wird nur im macOS-Zweig (`memset`) gebraucht.
- **Code-Auszug**:
```cpp
progress ? HE::Cs::SftpProgressFn(progress) : HE::Cs::SftpProgressFn());
```
- **Lösung**: `sftpGetFile(m_endpoint, remoteRelPath, localPath.string(), progress)` direkt (identischer Typ); falls die Typen später auseinanderlaufen, statisch absichern (`static_assert(std::is_same_v<SuProgressFn, HE::Cs::SftpProgressFn>)`). Forward-Declaration in `SelfUpdateTransport.h` statt Include.
- **Erwarteter Effekt**: Eine Funktionsobjekt-Kopie pro Download weniger (vernachlässigbar), kleinerer Include-Baum.
- **Risiko / Verifikation**: `SftpCredentials.h` könnte `SftpEndpoint` als Struct mit Default-Member-Initialisierern definieren – Forward-Declaration geht für Referenz-/Pointer-Parameter. Build prüfen.

## Cross-Modul-Hinweise

- `src/HE_ContentSync/src/SftpClient.cpp:337-341` und `SftpClient.h:77`: `localPath` ist `std::string`, intern `local.string() + ".partial"` – derselbe Windows-Pfadfehler wie SUP-102; betrifft auch den EngineContent-Download (`EngineContentPublish.cpp:105,218` ruft mit `tmp.string()`). Empfehlung: Signatur auf `std::filesystem::path` (oder dokumentiert UTF-8).
- `src/HE_Editor/SelfUpdateDialog.cpp:142` ruft `Session::install` (-> `beginTakeover`) im UI-Thread auf (SUP-101); `src/HE_Editor/EditorSelfUpdate.cpp:166` ruft `preflightTakeover` direkt nach `downloadUpdate` (SUP-105).
- `Takeover.cpp:38-39` kodiert die rotierenden Log-Dateinamen `HorizonEngine.{,1,2,3}.log` fest; sie müssen mit der Rotation in `src/HE_Core/src/Diagnostics/GlobalState.cpp` übereinstimmen (kein gemeinsamer Konstantensatz sichtbar, nicht geprüft).
- `src/HE_Core/src/Crypto/Sha256.cpp:134-152`: `hashFile` liest über `std::ifstream` mit 64-KiB-Puffer; bei Nutzung für Gigabyte-Dateien Messung lohnend (Hardware-SHA nicht sichtbar). Für die ~150-MB-Updates unkritisch.

## Unsicher / Messung nötig

- Dauer von Hash (`Sha256::hashFile`) und `tar`/`ditto`-Entpacken für ca. 150 MB je Zielplattform (SUP-101, SUP-105); ohne Messung bleiben die Zeitangaben Schätzungen.
- Verhalten von `fs::path::string()` mit nicht-ANSI-Zeichen in der verwendeten MSVC-STL (SUP-102): Ersetzung durch `?` oder Exception.
- `posix_spawn_file_actions_addchdir_np` (`SelfUpdateProcess.cpp:312`) ist anders als `addclosefrom_np` (`:313-318`) nicht versionsgeschützt: erfordert glibc >= 2.29 bzw. macOS >= 10.15; Mindestversionen der Zielplattformen/musl nicht geprüft.
- `findTool` fällt unter Windows auf `PATH` zurück (`SelfUpdateInstall.cpp:28-50`); ein dort zuerst gefundenes GNU-`tar` (Git/MSYS) liest `C:\...` als Remote-Host. Wird nur erreicht, wenn `System32\tar.exe` fehlt; nicht geprüft, ob das realistisch ist.
- macOS: Bei fehlschlagendem `hdiutil detach` (Ergebnis wird verworfen, `:197-198`) bleibt das dmg gemountet, und `cleanupAndFail` ruft `remove_all` über den Mountpunkt; ob `-force` als zweiter Versuch nötig ist, braucht einen Test auf echter Hardware.
- Ob der SFTP-Download nach transientem Fehler komplett neu beginnt (kein Resume, `fetchWithRetry` `:32-47`): im Code von `sftpGetFile` nicht auf Resume geprüft; bei 150 MB über instabile Leitungen relevant.

## Gelesen

Ja, alle 21 Dateien des Slice vollständig gelesen (11 Header, 9 Quelldateien, `updater/main.cpp`); zusätzlich gezielt `SftpClient.h/.cpp` (Auszug), `Sha256.cpp:134-152`, `EditorSelfUpdate.cpp:85-200` und `SelfUpdateDialog.cpp:142` für die Cross-Modul-Belege. `CMakeLists.txt` des Moduls nur per grep.
