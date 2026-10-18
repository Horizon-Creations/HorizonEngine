# Editor Slice 4 – src/HE_Editor/EditorApplication.cpp

## Zusammenfassung

`EditorApplication.cpp` (14 759 Zeilen) besteht zu rund einem Drittel (Zeilen 5321–10075) aus `dumpFrameHeadless()`, einer 4 750 Zeilen langen Sammlung von HE_DUMP_*-Testszenen. Zu einem weiteren Siebtel besteht sie aus `OnRender()` (3015–5143, 2 129 Zeilen) und `setupMcpTools()` (10414–11223). Die Frame-Schleife ist überwiegend sauber gegated (Dirty-Checks, statische getenv-Caches, Fenstertitel nur bei Änderung). Die echten Hot-Path-Kosten stecken in den Collab-/Team-Pfaden: Sie laufen jeden Frame und sind O(Entities) bzw. machen Syscalls und String-Kopien. Dazu kommen die Pro-Aufruf-Kosten von `makeContext()` und `callApi`. Gefunden wurden 21 Punkte: 2 Hoch, 11 Mittel, 8 Niedrig. Davon ist EDT-401 ein echter Build-Fehler für Builds ohne libssh2 (nicht durch Kompilieren verifiziert), alles andere ist Struktur oder Performance. Es wurde nichts gemessen. Alle Aufwandsangaben sind Schätzungen.

---

## Funde (nach Schweregrad)

### EDT-401 – `onProjectWriteRefused` ist nur unter `HE_HAVE_LIBSSH2` definiert, aber unbedingt benutzt
- **ID**: EDT-401
- **Kategorie**: F (Build-Fehler / #ifdef-Struktur)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: EditorApplication.cpp:69, 536, 2412–2435, 2670
- **Problem**: Die Funktion wird in Zeile 69 unbedingt deklariert und in `OnInit` (Z. 536) unbedingt benutzt. Ihre Definition liegt aber im `#ifdef HE_HAVE_LIBSSH2`-Block, der in Zeile 2412 beginnt und in Zeile 2670 endet. Das ist der Block der EngineContent-SFTP-Funktionen. Ohne libssh2 gibt es eine `static` Funktion, die benutzt, aber nie definiert wird. Das ergibt eine Warnung und danach einen Linker-Fehler. CLAUDE.md nennt einen Build ohne SFTP-Credentials/libssh2 "normal und supported". Die Funktion hat keinen SFTP-Bezug. Der Fehler stammt aus Commit 5319767 ("Lock the project's files while an export runs"), der die Definition zwischen die SFTP-Funktionen gesetzt hat. In dieser Umgebung wurde nicht kompiliert. Verifiziert ist nur per Grep, dass es keine zweite Definition gibt.
- **Code-Auszug**:
```cpp
static void onProjectWriteRefused(const char* what);   // 69, unbedingt
HE::ProjectWriteGate::install(&BuildProgressDialog::exportRunning, &onProjectWriteRefused);  // 536, unbedingt
#ifdef HE_HAVE_LIBSSH2                                   // 2412
static void onProjectWriteRefused(const char* what)     // 2418, Definition nur hier
```
- **Lösung**: `#ifdef HE_HAVE_LIBSSH2` von Zeile 2412 hinter das Ende von `onProjectWriteRefused` (nach Zeile 2435) verschieben. `onContentSyncFailure` und alles danach bleiben im Block. Danach `#include <atomic>`/`<mutex>` prüfen, falls der Header sie nur transitiv liefert.
- **Erwarteter Effekt**: Build ohne libssh2 linkt wieder. Reine Korrektur.
- **Risiko / Verifikation**: Sehr gering. Verifikation: CMake ohne libssh2 konfigurieren (`HE_HAVE_LIBSSH2` nicht gesetzt) und `HorizonEditor` linken.

### EDT-402 – `syncStructuralChanges()` baut jeden Frame das komplette Entity-Set neu auf
- **ID**: EDT-402
- **Kategorie**: D
- **Schweregrad**: Hoch (in einer Session; sonst unkritisch)
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:4976 (Aufruf), 11400–11618 (Funktion), konkret 11416–11419, 11529–11532, 11594–11598
- **Problem**: In einer Collab-Session läuft jeden Frame `syncStructuralChanges()`. Es legt ein neues `std::unordered_set<Entity> current` über alle Entities der Welt an, wobei jede Einfügung eine Heap-Allokation ist. Danach laufen `newSubtreeRoots`, ein Durchlauf über `m_structureKnown` (mit `m_structureParent.count` pro Entity), `recordMissingParents`, `reparentedSince` und eine Löschprüfung `current.count(e)` für jedes bekannte Entity. Pro Frame sind das einige Hash-Operationen pro Entity und eine große Allokation. Bei 50 000–200 000 Entities (Streaming-Szenen, Foliage-Instanzen als Entities, Terrain-Chunks sind ausgenommen) sind das vermutlich mehrere Millisekunden pro Frame. Es skaliert linear mit der Szene, obwohl sich im Normalfall nichts geändert hat. Der Kommentar "complete by construction" begründet die Diff-Methode, aber nicht, warum sie jeden Frame voll laufen muss.
- **Code-Auszug**:
```cpp
std::unordered_set<Entity> current;
reg.view<entt::entity>().each([&](auto e) {
	if (!isLocalOnly(e)) current.insert(e);
});
...
for (const Entity e : m_structureKnown)
	if (!current.count(e)) gone.push_back(e);
```
- **Lösung**: Den Diff nur ausführen, wenn sich die Struktur geändert haben kann.
  1. Dirty-Flags über `reg.on_construct<EntityIdComponent>()` und `on_destroy<EntityIdComponent>()` setzen. Alternativ ein Zähler in `HorizonWorld::createEntity/destroyEntity`. Dazu die vorhandene Hierarchie-Dirty-Information (`markHierarchyDirty`, Hierarchie-Revision) für Umhängungen nutzen.
  2. `if (!structDirty && now - lastFullDiff < 1000 ms) return;` Der volle Diff läuft als Sicherheitsnetz weiterhin einmal pro Sekunde. Er behält damit die Garantie "vollständig, auch bei direkter `registry().create`".
  3. `current` als Member halten (`clear()` behält die Buckets) oder die Prüfung ganz ohne Set über `reg.valid(e)` für die `gone`-Schleife lösen. `valid` genügt, weil `isLocalOnly` bei toten Entities irrelevant ist.
- **Erwarteter Effekt**: Im Ruhezustand von O(N) pro Frame auf O(1). Schätzung: einige hundert µs bis mehrere ms pro Frame gespart bei großen Szenen. Messen mit `HE_PROFILE_SCOPE_N`.
- **Risiko / Verifikation**: Mittel. Ein Pfad, der Entities ohne die Flags erzeugt, würde um bis zu 1 s verzögert gepublisht. Die Retry-Logik (kSendRetryFrames = 60) hängt an Frames, nicht an Sekunden, und müsste entsprechend angepasst werden. Es gibt keinen Test für das Verhalten (die Regeln liegen in StructuralSync.h und sind getestet, die Frame-Schleife nicht). Ein Test, der N Frames ohne Änderung laufen lässt und Create/Destroy/Reparent einzeln auslöst, wäre nötig. Dafür müsste die Schleife in eine testbare Klasse wandern (z. B. `StructuralSyncPass` neben StructuralSync.h).

### EDT-403 – Pro Frame: `projectRoot()` (Dateisystem-Stat) und mehrere String-Kopien für Team-Sync, auch ohne Team-Server
- **ID**: EDT-403
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:4906–4927, 10077–10084
- **Problem**: In `OnRender` wird jeden Frame `TeamSyncController::Project project` gebaut. Dabei werden `projectRoot()` aufgerufen, das ein `std::filesystem::is_regular_file(p)` macht (Syscall) und einen neuen String liefert, sowie `sc.teamServer()`, `sc.team` und `sc.project` kopiert. Das passiert unabhängig davon, ob ein Team-Server konfiguriert ist. Davor kopiert `m_git.setTeamServer(sc.teamServer(), sc.team, sc.project)` dieselben Strings noch einmal. `projectRoot()` wird außerdem in `collabSyncKey` pro Tab und Frame benutzt (siehe EDT-405). Der Wert ändert sich nur beim Projektwechsel.
- **Code-Auszug**:
```cpp
TeamSyncController::Project project;
project.root       = projectRoot();      // is_regular_file() pro Frame
project.teamServer = sc.teamServer();
project.team       = sc.team;
project.project    = sc.project;
for (const std::string& rel : m_teamSync.update(nowMs, project, m_isPlaying))
```
- **Lösung**: Den Projektroot in `setOnProjectLoaded` (und nach `saveProject`) in ein Member `m_projectRootCached` schreiben. `projectRoot()` gibt dann eine `const std::string&` zurück. Wenn `!sc.teamServer()` und der Controller idle ist, den `Project`-Aufbau überspringen oder `Project` als Member halten und nur bei geänderter `ProjectData`-Revision neu füllen. `setTeamServer` nur bei Änderung aufrufen (Vergleich vor der Kopie, oder die Parameter als `string_view`).
- **Erwarteter Effekt**: Ein Syscall und etwa 4 kleine Allokationen/Kopien pro Frame weniger. Schätzung: wenige µs, bei langsamen Netzlaufwerken als Projektordner deutlich mehr (Stat auf einer Netzfreigabe kann ms dauern).
- **Risiko / Verifikation**: Gering. Der Cache muss bei Projektwechsel/-neuanlage invalidiert werden (`gameInstancePath()` hat dasselbe Muster, ebenfalls stat pro Aufruf). Wenn das `.heproj`-Verzeichnis zur Laufzeit ersetzt wird (Datei wird zu Verzeichnis), wäre der Cache stale. Das ist praktisch nie der Fall.

### EDT-404 – Ausgewählte Entity wird jeden Frame der Session komplett nach CBOR serialisiert
- **ID**: EDT-404
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5004–5072 (konkret 5061–5070)
- **Problem**: Solange die Session aktiv ist und etwas ausgewählt ist, ruft `OnRender` jeden Frame `TerrainSync::serializeWithoutGrids(*m_editorWorld, selected)` auf. Das legt `GridsAside` an (Raster beiseite schieben und zurück), serialisiert alle Komponenten der Entity per `serializeEntityComponents` in einen `std::vector<uint8_t>` (Heap) und übergibt ihn an `publishComponents`, das den Blob hasht und verwirft, wenn er unverändert ist. Der Kommentar ("Serializing one entity is cheap") ist für ein Light plausibel. Für ein Terrain/Foliage/Skeletal-Entity mit großen Komponenten (Listen, Pfade, Overrides) ist es das nicht. Es läuft auch, wenn niemand etwas ändert.
- **Code-Auszug**:
```cpp
const std::vector<std::uint8_t> comps =
	HE::Ed::TerrainSync::serializeWithoutGrids(*m_editorWorld, selected);
if (!comps.empty())
	m_collab.publishComponents(static_cast<std::uint32_t>(entt::to_integral(selected)), comps);
```
- **Lösung**: Nur serialisieren, wenn sich etwas geändert haben kann: `m_undo.revision()` hat sich bewegt oder die Auswahl hat gewechselt, oder ein Inspector-Widget hat die Entity als editiert markiert (`noteEntityEdited` existiert bereits für Prefabs). Dazu als Rückfall eine Drossel auf z. B. 10 Hz (`nowMs - lastComponentsMs >= 100`). Der Hash in `publishComponents` bleibt als zweite Sicherung.
- **Erwarteter Effekt**: Statt 60× pro Sekunde nur bei Änderungen bzw. 10× pro Sekunde. Schätzung: je nach Entity 20–500 µs pro Frame eingespart. Messen.
- **Risiko / Verifikation**: Mittel. Änderungen, die keine Undo-Revision erzeugen (Gizmo-Drag, Slider vor dem Loslassen), würden von der Revision nicht erfasst. Darum zusätzlich die Drossel statt nur der Revision. Das Live-Gefühl für Peers bleibt bei 10 Hz erhalten. Tests: vorhandene Collab-Tests (`tests/`) auf Live-Komponenten-Sync prüfen. Neuer Test: ein Komponentenwert wird geändert, ohne dass Revision/Flag angefasst wird, und kommt innerhalb des Throttles beim Peer an.

### EDT-405 – `updateAssetCollabSync` macht pro Frame und Tab Stat, Stringbau, Hash-Sets und Kopien
- **ID**: EDT-405
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Vereinfachung
- **Ort**: EditorApplication.cpp:10195–10315, 10086–10134 (`collabSyncKey`)
- **Problem**: Die Funktion läuft jeden Frame in der Session. Sie
  1. baut mit `makeContext()` einen kompletten `AppContext` (siehe EDT-406), obwohl `ctx` nur beim Autosave eines Tabs in Zeile 10261 gebraucht wird,
  2. hält `const std::string& root` (Z. 10218), das nie benutzt wird (`(void)root;` in Z. 10221),
  3. legt pro Frame ein `std::unordered_set<std::string> openRel` an und ruft für jeden offenen Tab `collabSyncKey()` auf. Das macht für C++-Klassen und Ordner `projectRoot()` (Stat, siehe EDT-403), `projectRelativeAssetPath` (String-Bau), `isSyncableAsset` und `EditorAssetTypeCache::assetTypeOf(tabPath)`. Der Schlüssel eines Tabs ändert sich nur, wenn sich der Tab-Pfad ändert,
  4. kopiert `m_collab.heldAssetLocks()` in einen Vektor,
  5. fügt am Ende alle `openRel` in `m_docMirrorPaths` ein (String-Kopien pro Tab und Frame).
- **Code-Auszug**:
```cpp
const std::string& root = contentManager().contentRoot();
AppContext ctx = makeContext();
(void)root;
std::unordered_set<std::string> openRel;
for (const auto& tab : m_tabs)
{
	const std::string key = collabSyncKey(tab.assetPath);
```
- **Lösung**: (a) `ctx` erst vor dem `EditorUI::saveAsset` bauen und `root` löschen. (b) Die Tab→Key-Abbildung in `AppContext::EditorTab` oder einer `unordered_map<string,string>` cachen und bei Änderung des Tab-Pfads (oder Projektwechsel) invalidieren. (c) `openRel` und `m_docMirrorPaths` als Member halten und nur verändern, wenn sich die Tab-Liste ändert (die Signatur aus `saveOpenTabs` kann dafür benutzt werden). (d) Rate begrenzen: Delta-Publikation ist nötig, aber Lock-Verwaltung und Aufräumen (`heldAssetLocks`, `forgetAssetEditSession`) nur bei Tab-Änderung oder alle 250 ms.
- **Erwarteter Effekt**: Bei 10–30 offenen Tabs spart das pro Frame zehn bis hundert kleine Allokationen und ein paar Stats. Schätzung: 10–100 µs pro Frame, nur in Sessions.
- **Risiko / Verifikation**: Mittel. Die Lock-Freigabe ("Tab geschlossen, Lock zurück") darf nicht verzögert werden, sonst blockiert man Peers. Mit Tab-Revision-Trigger plus Zeitgrenze von 250 ms ist das ausreichend. `tests/test_collab*.cpp` prüfen. Die Funktion ist dort vermutlich nicht abgedeckt, weil `EditorApplication` nicht in `he_tests` liegt.

### EDT-406 – `makeContext()` baut bei jedem Aufruf ~60 `std::function`s und ruft `prune`
- **ID**: EDT-406
- **Kategorie**: B / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Vereinfachung
- **Ort**: EditorApplication.cpp:12005–12429 (Aufrufer u. a. 5095, 10219, 4843ff; in MCP-Hooks 10719, 10803, 10808 …)
- **Problem**: `makeContext()` wird mindestens einmal pro Frame aufgerufen (Z. 5095 `EditorUI::render`), in Sessions ein zweites Mal (EDT-405) und bei jedem MCP-Widget-/Task-Aufruf. Jeder Aufruf macht `m_selection.prune(registry)`, setzt vier Bridge-Pointer neu und baut ein Aggregat mit rund 60 `std::function`-Membern (jeweils `[this]`-Lambda, passt ins SBO, also vermutlich keine Heap-Allokation, aber 60 Konstruktionen, Typ-Erasure-Manager und beim Verlassen 60 Destruktoren). Dazu kommen String-/Vektor-Kopien nicht, weil die Datenmember Referenzen sind. Die Lambdas sind über die Lebenszeit der Anwendung konstant, nur die Wertfelder (`isPlaying`, `sceneDirty`, `projectLoaded`, Probe-Pointer, `undoSys`) ändern sich.
- **Code-Auszug**:
```cpp
return AppContext{
	.imguiReady          = m_imguiReady,
	.quit                = [this]{ m_quitConfirmed = true; Quit(); },
	.toggleProfilerCapture = [this]{ toggleProfilerCapture(); },
	... // ~60 weitere std::function-Felder
```
- **Lösung**: `AppContext m_ctx` als Member einmalig in `OnInit` mit allen Callbacks füllen. Pro Frame nur die Wertfelder aktualisieren (`refreshContext()`), und `makeContext()` gibt `m_ctx` zurück (Referenz). Alternativ die Callbacks in ein `AppContext::Callbacks`-Teilstruct auslagern, das einmal gebaut und per Zeiger referenziert wird. Der Teilbau wäre eine reine Umstrukturierung ohne Verhaltensänderung.
- **Erwarteter Effekt**: Pro Aufruf etwa 60 Funktions-Konstruktionen und -Destruktionen weniger. Schätzung: 1–3 µs pro Aufruf, ein paar µs pro Frame. Wichtiger ist die Lesbarkeit, weil die 420 Zeilen aus einer Funktion verschwinden, die nur Verdrahtung ist. Als reine Performance-Maßnahme ohne Messung nicht priorisieren.
- **Risiko / Verifikation**: Mittel. `AppContext` enthält Referenzen auf Member (`editorConfig`, `selection`, `tabs`), die bei einer langlebigen Instanz gültig bleiben müssten. Einige Panels könnten den Context kopieren und dabei annehmen, dass er frisch ist (z. B. `isPlaying` als Wert). Prüfen, ob `isPlaying`, `sceneDirty` usw. Werte oder Referenzen sind. Die UI-Tests (`tests/test_*_ui.cpp`) bauen eigene Contexts und decken den Pfad nicht ab. Ein Smoke-Lauf mit `HE_HIDDEN_WINDOW=1` ist nötig.

### EDT-407 – `callApi` (EngineCall-Knoten) setzt pro Aufruf Sandbox-Pfad, Berechtigungen und kopiert vier `std::function`s
- **ID**: EDT-407
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:334–353 (`apiCtx`), 1978–2022 (`svc.callApi`)
- **Problem**: Jeder EngineCall-Knoten in einem laufenden Graphen (PIE oder App-Vorschau, pro Frame potenziell hunderte) macht Folgendes:
  - `find(id)` per String-Lookup,
  - `std::filesystem::path(projPath).parent_path() / "Saved"` plus `.string()` (drei Allokationen) für `setSandboxRoot`,
  - `perm::Grants` neu füllen und `perm::set(g)`,
  - `apiCtx(...)`: kopiert `createObject`, `destroyObject`, `requestQuit`, `windowSize`, `requestRedraw` (fünf `std::function`-Kopien) in ein frisches `Ctx`.
  Sandbox-Wurzel und Grants ändern sich nur bei Projektwechsel oder wenn der Nutzer die Projekteinstellungen ändert. Der Kommentar ("setSandboxRoot is a cheap string assign") stimmt für die Zuweisung, aber nicht für die vorher gebaute Pfadkette.
- **Code-Auszug**:
```cpp
const std::string& projPath = m_projectManager.currentProject().path;
if (!projPath.empty())
	HE::api::fs::setSandboxRoot(
		(std::filesystem::path(projPath).parent_path() / "Saved").string());
...
HE::api::perm::set(g);
HE::api::Ctx c = apiCtx(m_editorWorld.get(), m_physicsWorld.get(), &contentManager(), self);
```
- **Lösung**: Sandbox-Root und Grants in `setOnProjectLoaded` und beim Speichern der Projekteinstellungen setzen, sowie einmal beim Play-Start (dort wird `setSandboxRoot` ohnehin schon gesetzt, Z. 12686). `callApi` ruft dann nur noch `apiCtx`. Für `Ctx` die Host-Funktionen per `const HostCtxParts*` oder `std::shared_ptr`/Referenz teilen, statt sie zu kopieren (falls `HE::api::Ctx` das zulässt, sonst Cross-Modul-Hinweis).
- **Erwarteter Effekt**: 3–4 Allokationen und 5 Funktionskopien weniger pro EngineCall. Schätzung: etwa 0,3–1 µs pro Aufruf. Relevant erst bei tausenden Aufrufen pro Frame (Schleifen im Graph). Messen mit einem PIE-Szenario.
- **Risiko / Verifikation**: Gering bis mittel. Wenn jemand `allowFiles` zur Laufzeit im Projekt-Dialog ändert, muss der Cache aktualisiert werden. Die Berechtigungen stehen ausdrücklich nur in `perm::Grants`. Ein Test mit geänderten Projekteinstellungen bei laufendem PIE fehlt.

### EDT-408 – `dumpFrameHeadless()` ist 4 750 Zeilen Testszenen in der Produktions-Übersetzungseinheit
- **ID**: EDT-408
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:5321–10075 (1 Funktion), Includes 24–120 für diese Zwecke
- **Problem**: Ein Drittel der Datei sind rund 30 Witness-Szenen (SKYTEST, MATERIALTEST, SECTIONTEST, ICONTEST, OCCLUSIONTEST, DOFTEST, SSRTEST, WATERTEST, WATERDROP, TEXARRAY, TEXBOMB, FOLIAGETEST, AUTOLAND, WATERLAKE, WATERBRUSH, LAKEEDIT, MOUNTAINTEST, THUMB, UITEST, ROPETEST, SELBOXTEST, LIGHTGIZMOTEST, SPLINETEST, SCENEIMAGE, MCPGIZMO …), jede mit eigener Verzweigung auf `std::getenv("HE_DUMP_…")`. Es sind 122 getenv-Aufrufe in der Datei. Sie sind nur für Screenshots/CI relevant, ziehen aber MaterialGraph, Terrain, Water, Foliage, Physics, Rope, Spline und Cinematic-Header in diese TU (Includes 70–140 gehören zum großen Teil dazu) und machen sie zur teuersten Übersetzungseinheit des Editors. Jede Änderung an einem Witness kompiliert auch `OnRender` neu und umgekehrt. Die Funktion ist nicht unit-testbar und schwer zu lesen (ein Block "sky test" beeinflusst Kamera und Environment für alle folgenden Blöcke).
- **Code-Auszug**:
```cpp
void EditorApplication::dumpFrameHeadless()
{ ... 4 750 Zeilen ...
	if (const char* st = std::getenv("HE_DUMP_SKYTEST"); st && *st && m_editorWorld) { ... }
	if (const char* mt = std::getenv("HE_DUMP_MATERIALTEST"); mt && *mt && m_editorWorld) { ... }
```
- **Lösung**:
  1. Neue Datei `EditorDumpWitnesses.cpp` (+ kleiner Header) mit `namespace HE::Ed::Dump`. Jedes Witness wird eine freie Funktion `void addWaterLakeWitness(DumpScene&, const char* mode)` mit einem kleinen `DumpScene`-Kontext (World, ContentManager, Renderer, Kamera, Selection, Undo).
  2. Eine Tabelle `{ "HE_DUMP_WATERLAKE", &addWaterLakeWitness }` statt 30 hintereinander stehender `if`-Blöcke.
  3. `dumpFrameHeadless` bleibt als Orchestrator: Settings → Witnesses → Settle → Capture → Nachwitnesses (GPUTIME, SCENEIMAGE, MCPGIZMO).
  4. Die Witness-Datei nur in Editor-Targets mit `HE_DUMP`-Option oder immer, aber in einer eigenen TU.
- **Erwarteter Effekt**: `EditorApplication.cpp` schrumpft um etwa 32 % (auf ca. 10 000 Zeilen), das Inkrement-Build von OnRender-Änderungen wird schneller (Schätzung, nicht gemessen), die Witnesses werden einzeln lesbar und wiederverwendbar (siehe EDT-409).
- **Risiko / Verifikation**: Gering für die Funktionalität, mittel für den Aufwand. Verhalten muss bit-genau bleiben, weil `scripts/he_shot.py` und die dokumentierten Baselines (Thema-Nummern in den Kommentaren) auf die Env-Namen und Reihenfolge der Blöcke bauen. Verifikation: pro Witness ein Referenzbild vor/nach dem Umzug (`scripts/he_shot.py`) und `md5`-Vergleich der "dump counters"-Zeile. Die Reihenfolge ist bedeutsam (z. B. `HE_DUMP_SKYTEST` setzt die Kamera vor allen anderen Blöcken, `WarmupMaterials` liegt bei Z. 8624 zwischen Szenenaufbau und Re-Authoring).

### EDT-409 – Wiederholter Boilerplate in den Witnesses (Kamera-Basis ×9, Landscape ×8, Sphere ×2, diffPercent ×2, Checkerboard ×4 …)
- **ID**: EDT-409
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp, u. a. 5913–5915, 6098–6100, 6155–6157, 6197–6199, 6231–6233, 9205–9207, 9363–9365, 9462–9464, 9547–9549 (Kamera-Vorwärtsvektor); 6466–6469, 6751–6754, 7185–7191, 7331–7334, 7457–7460, 7660–7663, 7784–7787, 8236–8239, 8568–8574 (Landscape bei y=300); 5885–5906 und 6066–6086 (UV-Sphere); 9888–9895 und 9977–9984 (`diffPercent`); 8820–8830, 8908–8914, 8950–8956 (Checkerboard-Composite); 7797–7810 und 8255–8267 (Bank-Höhenfeld); 8166–8168, 7920–7922, 8542–8544, 8171–8173 (`HE_DUMP_WATERHIDEGROUND/SHEET`)
- **Problem**: Der gleiche Code steht an vielen Stellen kopiert. Das gilt für die Kamera-Basis aus Pitch/Yaw (9 Mal wortgleich), die Landscape-Entity mit `position.y = 300`, `resolution`, `heightScale = 0`, `seed = 0`, `dirty = true` (8 Mal), die UV-Sphere mit `segU = 48, segV = 24` (2 Mal), den Bild-Diff in Prozent (2 Mal) und das Composite über Schachbrett (4 Mal). Für das Bank-Höhenfeld steht im Code sogar der Hinweis "same shape, so the two witnesses agree". Das ist ein Kopierzwang, den nur ein Helfer lösen kann. `envF`-Lambdas gibt es in drei Varianten (5508, 7496, 8566).
- **Code-Auszug**:
```cpp
const float cp = std::cos(m_editorCamera.pitch()), sp = std::sin(m_editorCamera.pitch());
const float cy = std::cos(m_editorCamera.yaw()),   sy = std::sin(m_editorCamera.yaw());
const glm::vec3 camFwd(cp * sy, sp, -cp * cy);
```
- **Lösung**: Helfer in der neuen Witness-Datei (EDT-408): `glm::vec3 EditorCamera::forward()` (existiert privat, wird hier mit Hinweis nachgebaut: "EditorCamera::forward is private" in Z. 5911; stattdessen öffentlich machen), `Entity makeFlatLandscape(world, name, size, res, y)`, `StaticMeshAsset makeUvSphere(segU, segV, r)`, `double diffPercent(a, b)`, `void writeOverCheckerPpm(path, rgba, w, h)`, `void hideTerrainChunks(world)`, `float envF(name, default)`.
- **Erwarteter Effekt**: Etwa 350–450 Zeilen weniger. Kein Laufzeiteffekt. Weniger Fehler bei neuen Witnesses.
- **Risiko / Verifikation**: Gering, wenn mechanisch umgesetzt. Referenzbilder pro Witness wie bei EDT-408. Die Landscape-Parameter unterscheiden sich im Detail (`resolution` 33/65/129, `water.res`), also nicht alles in einen Helfer mit Defaults pressen, sondern nur die Entity-Anlage.

### EDT-410 – `OnRender()` ist 2 129 Zeilen lang und enthält lokale Typen, Lambdas und statische Zustände
- **ID**: EDT-410
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:3015–5143, darin 3674–3772 (lokale Structs `PlayPhysicsInputs`/`PlayPoseWatch`, `static PlayPoseWatch s_playPose`, Lambda `samplePhysicsInputs`), 4329–4756 (Debug-Overlays), 3128–3272 (Input-Snapshot und Szenen-Requests), 4864–5093 (Collab/MCP/Team-Pumpe)
- **Problem**: Die Frame-Schleife ist eine einzige Funktion mit vielen Zuständen (`simulating`, `uiLive`, `stepping`, `gameDt`). Einzelne Abschnitte hängen über lokale Statics zusammen (`s_playPose` wird in Zeile 3726 deklariert, aber erst in 4125–4162 neu geschrieben). Das macht die Ordnungsbedingung ("Vergleich oben, Cache unten") unsichtbar und führt zu den langen erklärenden Kommentaren. Zwei Pfade (`renderer()`-Block und Collab-Block) liegen inkonsistent eingerückt (Z. 4964–4974). Eine Instanzvariable wäre richtig, weil `static` zwischen mehreren `EditorApplication`-Instanzen (Tests, Hub) geteilt würde.
- **Code-Auszug**:
```cpp
struct PlayPoseWatch { uint32_t entity = 0u; bool valid = false; glm::vec3 position{}; glm::vec3 rotation{}; PlayPhysicsInputs build{}; };
static PlayPoseWatch s_playPose;
```
- **Lösung**: Aufteilen in private Methoden in Frame-Reihenfolge: `pumpTransportAndBreakpoints()`, `pumpPlayInput()`, `syncAuthorEditsToPhysics()` (mit `PlayPoseWatch m_playPose` als Member und die Typen in einen anonymen Namespace), `tickWorldAndPhysics()`, `tickWidgetsAndPointer()`, `buildDebugOverlays()`, `pumpCollabAndMcp()`, `pushRendererSettings()`. Die Reihenfolge bleibt exakt, es gibt nur Methodenaufrufe statt Blöcke. Danach ist jede Phase einzeln profilierbar (`HE_PROFILE_SCOPE_N` pro Methode).
- **Erwarteter Effekt**: Keine Laufzeitänderung (bis auf Inlining). Lesbarkeit, Reviewbarkeit und die Möglichkeit, die Phasen in Tests zu treiben (z. B. `syncAuthorEditsToPhysics` mit einer Fake-Physikwelt).
- **Risiko / Verifikation**: Gering, solange keine Logik wandert. Die Reihenfolge (physics → camera → animation → compare) ist dokumentiert und muss erhalten bleiben. Vor/nach: PIE-Smoke (Play, Gizmo-Verschieben einer Kiste, Stop) mit `HE_HIDDEN_WINDOW=1`.

### EDT-411 – Config laden und speichern: ~120 Schlüssel zweimal von Hand aufgelistet
- **ID**: EDT-411
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:1500–1636 (laden) und 14298–14417 (`writeEditorConfig`)
- **Problem**: Für jedes Feld in `EditorConfig` gibt es zwei Zeilen mit demselben Schlüsselstring. Der Kommentar in Z. 14287–14291 sagt, die Funktion sei herausgezogen worden, weil "a second copy of that mapping is one that stops matching the first". Es gibt aber weiterhin zwei Kopien (Laden und Speichern), die sich nur in `getCustomConfig*`/`setCustomConfigEntry` unterscheiden. Ein Beispiel für die Gefahr: `RewardsReducedMotion` wird beim Laden geklemmt (Z. 1539), beim Speichern nicht. Beim Laden steht `KeepCPUAssetsInfoAcknoleged` als Altlast. Neue Felder in nur einer Richtung zu vergessen, bleibt unbemerkt, bis ein Nutzer einen Neustart macht.
- **Code-Auszug**:
```cpp
m_editorConfig.BloomEnabled   = globalstate.getCustomConfigBool("BloomEnabled",   m_editorConfig.BloomEnabled);   // 1598
globalstate.setCustomConfigEntry("BloomEnabled", m_editorConfig.BloomEnabled);                                  // 14380
```
- **Lösung**: Eine konstexpr-Tabelle in `EditorConfig.h` oder `EditorConfigIo.cpp`:
  `struct Field { const char* key; std::variant<bool EditorConfig::*, int EditorConfig::*, float EditorConfig::*, std::string EditorConfig::*> member; }`, plus `forEachField(cfg, visitor)`. Laden und Speichern sind dann je eine Schleife (`std::visit`). Die Sonderfälle (Legacy-Key, Clamp von RewardsReducedMotion, Kamera, ShowFlags, Shortcuts, Graphics-Rückgabe) bleiben davor/danach. Die Tabelle kann zusätzlich vom MCP-Settings-Tool und `settings_set` genutzt werden (siehe Cross-Modul).
- **Erwarteter Effekt**: Etwa 200 Zeilen weniger, Laden und Speichern können nicht mehr auseinanderlaufen. Kein Laufzeiteffekt (einmalig).
- **Risiko / Verifikation**: Gering. Unit-Test: Round-Trip `EditorConfig` → JSON → `EditorConfig` (alle Felder auf Nicht-Default setzen) und Vergleich. Ein Test für das Laden einer alten `config.json` ohne neue Schlüssel (Defaults bleiben) schützt die Abwärtskompatibilität.

### EDT-412 – `setupMcpTools()` ist 810 Zeilen mit zehn gleichen Hook-Lambdas
- **ID**: EDT-412
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:10414–11223; `lockedByOther`: 10588, 10641, 10796, 10823, 10843, 10871, 10889, 10916, 10943, 10974, 10988; `publishCreate`: 10655, 10769, 10857, 10874; `onAssetAppeared`: 10725, 10772, 10860, 10877; `isPlaying`: 10429, 10458, 10634, 10755 … ; `entityCount`: 10431–10439 und 10757–10762; Zeit in ms: 4881, 5100, 11250, 11310
- **Problem**: Jede Tool-Familie bekommt eine eigene Hook-Struktur. Die Lambdas `isPlaying`, `lockedByOther` (`m_collab.assetLockedByOther(rel)`), `publishCreate` und `onAssetAppeared` sind jeweils zeichengleich, bis zu elfmal. Die Entity-Zählung (`view<entt::entity>().each(++n)`, O(N)) steht doppelt. Ein neues Tool-Set muss die Familie von Hand ergänzen. Die gleichzeitige Pflegeliste ("Mcp-Tool-Registry") ist damit verteilt auf 800 Zeilen. Der Millisekunden-Zeitstempel `duration_cast<milliseconds>(steady_clock::now()…)` ist vier Mal ausgeschrieben.
- **Code-Auszug**:
```cpp
widget.lockedByOther = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
input.lockedByOther  = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
material.lockedByOther = [this](const std::string& rel) { return m_collab.assetLockedByOther(rel); };
...
```
- **Lösung**: Gemeinsame Helfer in einem anonymen Namespace oder als Lambdas am Funktionskopf: `auto isPlaying = [this]{...}; auto lockedByOther = ...; auto publishCreate = ...; auto appeared = ...;`, dann `widget.lockedByOther = lockedByOther;`. Alternativ eine `McpCommonHooks`-Basisstruktur in `McpTools*.h`, die die Familien erben. `static std::uint64_t nowMs()` als freie Funktion (auch für EDT-403/405). Für `entityCount` `reg.storage<entt::entity>().size()` verwenden (O(1), `free_list`-abhängig, prüfen) oder `registry.alive()`. Die Familien in je eine Methode (`registerAssetFamily`, `registerParticleFamily` …) teilen.
- **Erwarteter Effekt**: Etwa 100–150 Zeilen weniger, eine Stelle für das gemeinsame Verhalten. Die Entity-Zählung wird O(1) (nur bei Statusabfragen, nicht pro Frame).
- **Risiko / Verifikation**: Gering. Bestehende MCP-Tests (`tests/test_mcp_*.cpp`) laufen gegen die Hooks, nicht gegen `EditorApplication`. Daher ein Smoke-Lauf mit `HE_MCP=1` und einem scripted Client (`scripts/he_mcp_multiclient.py`) nach der Umstellung.

### EDT-413 – Play-Start/-Stop schreibt und liest den Szenen-Snapshot über eine feste Temp-Datei
- **ID**: EDT-413
- **Kategorie**: E / C
- **Schweregrad**: Mittel
- **Typ**: Performance + Robustheit
- **Ort**: EditorApplication.cpp:12640–12651 (Save beim Play-Start), 12984–12999 (Load beim Stop); `saveToMemory`/`loadFromMemory` existieren (SceneSerializer.h:118–119) und werden in Z. 13408 schon benutzt
- **Problem**: Beim Drücken von Play wird die gesamte Welt synchron im UI-Thread binär in `temp_directory_path() / "he_play_snapshot.hescene_bin"` geschrieben und beim Stop wieder gelesen. Das ist blockierende Disk-I/O für etwas, das nur im Prozess gebraucht wird. Bei großen Szenen (dutzende MB) kann das spürbar sein. Außerdem ist der Pfad fest: Zwei laufende Editor-Instanzen (zwei Projekte, oder ein Editor plus ein CI-Dump-Lauf) überschreiben gegenseitig ihren Snapshot, und beim Stop würde eine Instanz die Welt der anderen laden. Auf Systemen mit schreibgeschütztem/vollem Temp schlägt Play fehl, obwohl nichts davon nötig wäre.
- **Code-Auszug**:
```cpp
const std::filesystem::path snapshot =
	std::filesystem::temp_directory_path() / "he_play_snapshot.hescene_bin";
...
if (!serializer.save(*m_editorWorld, snapshot, SerializeFormat::Binary))
...
if (!serializer.load(*m_editorWorld, snapshot, SerializeFormat::Binary))
```
- **Lösung**: `std::vector<uint8_t> m_playSnapshot` als Member. Beim Play `serializer.saveToMemory(*m_editorWorld, m_playSnapshot)`, beim Stop `serializer.loadFromMemory(*m_editorWorld, m_playSnapshot)` und danach `m_playSnapshot.clear(); shrink_to_fit()`. Alternativ bei Speicherdruck eine Datei mit PID im Namen. Das Binärformat bleibt dasselbe, falls `saveToMemory` dasselbe Format wie `SerializeFormat::Binary` schreibt (prüfen: `saveToMemory` wird für Undo-Snapshots benutzt).
- **Erwarteter Effekt**: Play-Start/-Stop ohne Datei-I/O (Schätzung: bei 50 MB Szene 50–300 ms eingespart je Richtung, abhängig vom Datenträger). Wegfall der Instanz-Kollision.
- **Risiko / Verifikation**: Mittel. Wenn `saveToMemory` ein anderes Layout als `save(..., Binary)` hat, muss `loadFromMemory` es lesen können (gleiches Paar benutzen). Hinweis auf CLAUDE.md: Szenen werden verlustfrei serialisiert. Test: `Save→Play→Edit→Stop`-Round-Trip mit allen Komponenten (existierende `tests/test_scene*` ergänzen, ein Test "play snapshot round trip" über `saveToMemory`/`loadFromMemory`).

### EDT-414 – Renderer-Einstellungen: dieselben ~100 Zeilen in `OnRender` und `dumpFrameHeadless`, und ein Push von 12 Settings pro Frame
- **ID**: EDT-414
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: EditorApplication.cpp:3528–3635 (`OnRender`), 5339–5498 (`dumpFrameHeadless`)
- **Problem**: Beide Blöcke bauen Bloom, SSAO, DoF, MotionBlur, AA, GI, SSR, GI-Reflections, RenderPath, Occlusion aus `m_editorConfig` auf und wenden die `HE_DUMP_*`-Overrides an. Die Override-Logik ist doppelt (einmal mit `static const char*`-Cache, einmal mit `getenv` pro Aufruf). Wer ein neues Setting ergänzt, muss beide pflegen. Dazu: `OnRender` ruft pro Frame zwölf `renderer()->Set*Settings(...)` auf. Ob die Renderer-Backends auf "unverändert" prüfen und die Pipelines nicht neu bauen, ist hier nicht sichtbar (siehe Unsicher).
- **Code-Auszug**:
```cpp
renderer()->SetBloomSettings(IRenderer::BloomSettings{ m_editorConfig.BloomEnabled, ... });
renderer()->SetSSAOSettings(IRenderer::SSAOSettings{ ... });
...
renderer()->SetGIReflectionSettings(gr);
```
- **Lösung**: Eine Methode `void pushRendererSettings(IRenderer& r, const EnvOverrides& ov)`, die beide Pfade aufrufen. Die Overrides werden einmal beim Start in ein `EnvOverrides`-Struct geparst (optional-Felder). Pro Frame nur pushen, wenn sich `m_editorConfig` gegenüber dem letzten Push geändert hat (ein `RendererSettingsSnapshot` mit `operator==`, gebaut aus den Config-Feldern). Der Frame-Push bleibt eine Zeile.
- **Erwarteter Effekt**: Etwa 120 Zeilen weniger. Pro Frame zwölf Setter-Aufrufe gespart, vermutlich mikroskopisch (µs). Nur wegen der Wartbarkeit angehen.
- **Risiko / Verifikation**: Gering. Die Kommentare in Z. 3604–3611 betonen, dass die Overrides bei jedem Frame gelten müssen. Das bleibt erhalten, wenn der Snapshot die Overrides schon enthält. Verifikation: `he_shot.py` mit `HE_DUMP_GI`, `HE_DUMP_SSR`, `HE_DUMP_RENDERPATH` vor/nach.

### EDT-415 – `saveOpenTabs()` baut jeden Frame einen Signatur-String
- **ID**: EDT-415
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5097 (Aufruf), 13242–13250
- **Problem**: Jeden Frame wird `sig` aus allen Tab-Pfaden plus `std::to_string(m_activeTab)` zusammengesetzt und mit `m_lastTabSig` verglichen. Bei 20 Tabs mit je 100 Zeichen sind das 2 KB String-Bau pro Frame. Danach schreibt eine Änderung die gesamte Config (`writeConfig`) synchron im UI-Thread.
- **Code-Auszug**:
```cpp
std::string sig;
for (const auto& t : m_tabs) sig += t.assetPath + "|";
sig += std::to_string(m_activeTab);
if (sig == m_lastTabSig) return;
```
- **Lösung**: 64-Bit-Hash über die Pfade und den Index statt eines Strings (`m_lastTabHash`). Noch besser: `EditorUI`/`EditorTabs` zählen eine `tabRevision` hoch, wenn sie Tabs ändern. Das gleiche Signal löst EDT-405.
- **Erwarteter Effekt**: Pro Frame null Allokationen (Schätzung: 1–3 µs). Der Config-Write bleibt.
- **Risiko / Verifikation**: Sehr gering. Hash-Kollision hätte nur zur Folge, dass ein Speichern ausfällt. Ein Test über die `EditorTabs`-Funktionen existiert nicht für diesen Pfad.

### EDT-416 – Texture-Hot-Reload invalidiert pro geänderter Textur alle Materialien
- **ID**: EDT-416
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:3476–3481
- **Problem**: Für jede geänderte Textur wird `contentManager().enumerateIds(HE::AssetType::Material)` neu aufgebaut (Vektor) und alle Materialien werden invalidiert. Bei einem `git pull` mit 200 geänderten Texturen und 5 000 Materialien sind das 200 Vektor-Allokationen und 1 Mio. `InvalidateMaterial`-Aufrufe auf dem UI-Thread in einem Poll. Das Ergebnis ist nach dem ersten Mal identisch.
- **Code-Auszug**:
```cpp
case HE::AssetType::Texture:
	for (const HE::UUID& matId : contentManager().enumerateIds(HE::AssetType::Material))
		renderer()->InvalidateMaterial(matId);
	break;
```
- **Lösung**: Flag `bool texturesChanged` im Switch setzen, nach der Schleife einmal über alle Materialien invalidieren. Besser: nur die Materialien invalidieren, die die geänderte Textur referenzieren (`MaterialAsset::textureIds`, `graphTextureIds`), wenn die Reverse-Map billig verfügbar ist (Cross-Modul).
- **Erwarteter Effekt**: Von O(Texturen × Materialien) auf O(Materialien) pro Poll.
- **Risiko / Verifikation**: Gering. Verhalten identisch. Test: `pollHotReload` mit zwei geänderten Texturen, `InvalidateMaterial` für jedes Material höchstens einmal (Fake-Renderer).

### EDT-417 – Kleine Pro-Frame-Allokationen in Collab-/Prefab-/Landscape-Pfaden
- **ID**: EDT-417
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: EditorApplication.cpp:5085–5091 (Selektion für Presence), 13422–13436 (`recordPrefabEdits`, Z. 3293 aufgerufen), 4874–4880 (`m_retargetJobs`), 4545–4549 (Landscape-Vorschau), 4626–4637 (Ringe)
- **Problem**:
  - `m_collab.active()` legt pro Frame einen `std::vector<uint64_t> selection` an (reserve + `subjectFor` pro Eintrag), obwohl sich die Auswahl selten ändert.
  - `recordPrefabEdits()` baut jeden Frame `current` (UUID-Vektor der Auswahl, inkl. `try_get<EntityIdComponent>` pro Eintrag), selbst wenn sich die Revision nicht bewegt hat. Der Vektor wird nur gebraucht, um `m_prefabEditLastSelection` zu aktualisieren.
  - `m_retargetJobs.erase(remove_if(... wait_for(0s)))` läuft jeden Frame, auch wenn die Liste leer ist (billig, aber nutzlos).
  - Die Landscape-Vorschau (nur bei offenem "Neue Landschaft"-Formular) allokiert pro Frame `hpre` und bewertet `terrainHeightAt` für bis zu 33×33 Punkte (mit Oktaven).
  - Presence-Ringe: 3 × 28 `cos/sin` pro Teilnehmer und Frame.
  Jeder Punkt für sich ist klein. Bei 1 000+ ausgewählten Entities (Auswahl-Rahmen) wachsen die ersten beiden linear.
- **Code-Auszug**:
```cpp
std::vector<std::uint64_t> selection;
selection.reserve(m_selection.size());
for (Entity sel : m_selection.entities())
	selection.push_back(m_collab.subjectFor(static_cast<std::uint32_t>(entt::to_integral(sel))));
m_collab.setLocalPresence(p, r, selection);
```
- **Lösung**: Die Presence-Auswahl nur neu berechnen, wenn sich eine `m_selection.revision()` oder die Entity-Menge geändert hat (Member-Vektor, `clear()` + wiederverwenden). `recordPrefabEdits`: Auswahl nur bei Änderung der Selektion/Revision aktualisieren (Selection-Revision oder Größenvergleich). `m_retargetJobs`: `if (!m_retargetJobs.empty())`. Landscape-Vorschau: Höhenraster in Member cachen, Schlüssel aus den `newTerrain`-Parametern. Ringe: statische 28er Sin/Cos-Tabelle (`constexpr`-fähig).
- **Erwarteter Effekt**: Pro Frame wenige Allokationen weniger, bei großen Auswahlen mehr. Schätzung: unter 10 µs im Normalfall.
- **Risiko / Verifikation**: Gering. Beim Selektionsvergleich muss auch Entity-Löschung (Handle-Wiederverwendung) erkannt werden: `prune` in `makeContext` ändert die Auswahl, daher die Revision dort mitzählen.

### EDT-418 – `OnEvent`: redundante Backend-Weiche und doppelte Text-Edit-Tastentabelle
- **ID**: EDT-418
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:14435–14455, 14550–14578, 14598–14669
- **Problem**: (1) Die `switch (m_backend)` ruft in jedem Zweig (OpenGL, Vulkan, Metal, D3D11, D3D12) `ImGui_ImplSDL3_ProcessEvent(&event)`. Nur `default` tut nichts. Der Kommentar der D3D-Zweige widerspricht dem Code ("Win32 backend … handles Win32 messages … but we still forward SDL events"). (2) Die Taste-für-Taste-Behandlung eines fokussierten Labels (Z. 14550–14578) ist eine Teilmenge der Behandlung eines Textfelds (Z. 14598–14669). Die Cases für Links/Rechts/Home/End/Up/Down/Ctrl+A/Ctrl+C stehen zweimal.
- **Code-Auszug**:
```cpp
case HE::RendererBackend::OpenGL: case HE::RendererBackend::Vulkan: case HE::RendererBackend::Metal:
	consumed = ImGui_ImplSDL3_ProcessEvent(&event); break;
case HE::RendererBackend::D3D11: case HE::RendererBackend::D3D12:
	consumed = ImGui_ImplSDL3_ProcessEvent(&event); break;
```
- **Lösung**: (1) `bool consumed = ImGui_ImplSDL3_ProcessEvent(&event);` ohne Weiche (der Software-Backend-Fall prüfen: `default` hat bisher nicht weitergeleitet, `HE::RendererBackend::Software` könnte absichtlich ausgenommen sein; dann `if (m_backend != Software)`). (2) Eine Funktion `bool routeTextEditKey(WidgetManager&, const SDL_KeyboardEvent&, bool allowEdit)`, die für beide Fälle die gemeinsame Tabelle benutzt und `allowEdit == false` die Schreibtasten auslässt.
- **Erwarteter Effekt**: Etwa 40 Zeilen weniger. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering bis mittel. Das Verhalten bei Software-Backend und Tastatur-Routing ist nur manuell testbar. `tests/test_ui_widgets.cpp` testet das WidgetManager-Verhalten, nicht diese Weiche.

### EDT-419 – Testszenen-Fehler: `HE_DUMP_LAKEWATERRES` wird sofort überschrieben
- **ID**: EDT-419
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Fehler in Testcode)
- **Ort**: EditorApplication.cpp:8245–8254
- **Problem**: Z. 8249–8250 setzen `ltc.water.res` aus `HE_DUMP_LAKEWATERRES`. Vier Zeilen später setzt Z. 8254 unbedingt `ltc.water.res = 128;`. Die Umgebungsvariable hat damit keine Wirkung, obwohl der Kommentar (8245–8246) sie als Teil des Messaufbaus für den 1-km-Fall nennt (`HE_DUMP_LAKESIZE=1000 HE_DUMP_LAKERES=1025 HE_DUMP_LAKEWATERRES=256`). Die dort gemessenen "drag"-Zeiten gelten damit für 128 statt 256 Wasserzellen.
- **Code-Auszug**:
```cpp
if (const char* lw = std::getenv("HE_DUMP_LAKEWATERRES"); lw && std::atoi(lw) > 1)
	ltc.water.res = static_cast<uint32_t>(std::atoi(lw));
ltc.heightScale = 0.0f;
ltc.seed        = 0;
ltc.dirty       = true;
ltc.water.res   = 128;
```
- **Lösung**: Zeile 8254 durch `if (ltc.water.res == 0 /* kein Env */) ltc.water.res = 128;` ersetzen oder vor die Env-Zeilen ziehen (Default zuerst, Override danach).
- **Erwarteter Effekt**: Die LAKEEDIT-Messreihe "drag" mit `HE_DUMP_LAKEWATERRES` wird gültig.
- **Risiko / Verifikation**: Sehr gering. Bestehende Baselines mit der Variable gesetzt wären vorher nicht gültig gewesen. Dokumentierte Zahlen (docs, Thema 188) prüfen.

### EDT-420 – Acht fast gleiche Worker-Thread-Muster (Probes/Installer)
- **ID**: EDT-420
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:2324–2339 (Toolchain), 2349–2372 (Claude), 2396–2410 (Git), 2529–2623 (SFTP), 2630–2669 (Manifest), 2672–2689 (Router), 2693–2725 (Git-Identität), 2734–2777 (Git-Install), 2783–2811 (Toolchain-Install); Joins in `OnShutdown` 14132–14171
- **Problem**: Jedes Muster: `if (m_xThread.joinable()) m_xThread.join(); m_xChecked.store(false); m_xThread = std::thread([this]{ result = probe(); m_xChecked.store(true); });` plus ein Join in `OnShutdown`. Jedes Muster braucht Member (Thread, Atomics, Ergebnis). Einzelne Varianten haben Cancel-Flags oder Mutex-Logs. Neue Probes (wie `startClaudeProbe`) kopieren das Muster. Die Shutdown-Liste muss von Hand vollständig sein, sonst terminiert `std::thread` in `~thread`. Beim erneuten Recheck aus der UI (Callback aus dem Frame, in dem `ctx.toolchainProbe` noch gelesen wird) wird das Ergebnis-Member vom Worker beschrieben, während die UI den Zeiger noch hält (siehe Unsicher).
- **Code-Auszug**:
```cpp
if (m_toolchainThread.joinable()) m_toolchainThread.join();
m_toolchainChecked.store(false, std::memory_order_release);
m_toolchainThread = std::thread([this] { ...; m_toolchainChecked.store(true, std::memory_order_release); });
```
- **Lösung**: Kleine Vorlage `template<class T> class ProbeJob { void start(std::function<T()>); bool done() const; const T* result() const; ~ProbeJob() { join(); } }` (Ergebnis erst nach `done`, Join im Destruktor, optionale Cancel-Flag). Das ersetzt je Probe vier Member und eine Zeile in `OnShutdown`. `std::jthread` mit `std::stop_token` (C++20) erledigt das Join und das Abbrechen.
- **Erwarteter Effekt**: Etwa 100 Zeilen weniger, Shutdown-Liste entfällt, die Datenrennen-Frage wird an einer Stelle gelöst.
- **Risiko / Verifikation**: Gering bis mittel. Die Probes haben unterschiedliche Lebensdauern (`m_gitThread` wird aus anderen Worker-Threads re-probed). Beim Umstellen die Reihenfolge der Joins in `OnShutdown` erhalten (Kommentare dort sind sicherheitsrelevant).

### EDT-421 – Kleinigkeiten: doppelte Includes, unbenutzte Variable/Parameter, irreführender Parametername
- **ID**: EDT-421
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: EditorApplication.cpp:144 und 155 (`<algorithm>` doppelt), 140 und 166 (`<SDL3/SDL.h>` doppelt), 10218/10221 (`root` + `(void)root`), 13964–13968 (`GameLogicDeltaTime(float rawDt)` mit `(void)rawDt`), 1027 (`onRemoteTransform` nimmt `subject`, bekommt aber einen lokalen Entity-Handle, siehe CollabController.cpp:1457–1461)
- **Problem**: Reine Aufräumpunkte. Besonders der Parametername in Z. 1027 führt in die Irre: Weiter oben (Z. 660, 4665) sind "Subjects" uuid-abgeleitete 64-Bit-IDs, die über `entityForNetId` aufgelöst werden. Hier wird derselbe Name für den schon aufgelösten 32-Bit-Handle benutzt und per `static_cast<entt::id_type>` gecastet. Das Verhalten ist korrekt (nur der Name ist falsch), aber ein Leser verdächtigt einen Fehler.
- **Code-Auszug**:
```cpp
m_collab.onRemoteTransform([this](std::uint64_t subject, const float pos[3], ...) {
	const auto e = static_cast<entt::entity>(static_cast<entt::id_type>(subject));
```
- **Lösung**: Doppelte `#include`s entfernen, `root`/`(void)root` löschen, den Parameter in `handle` umbenennen (Signatur im Header bleibt, nur der Lambda-Parametername). `GameLogicDeltaTime` ist vermutlich eine virtuelle Überschreibung, dann den Parameter unbenannt lassen (`float /*rawDt*/`).
- **Erwarteter Effekt**: Lesbarkeit. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Keines.

---

## Cross-Modul-Hinweise

- **HE_Editor/ViewportOverlays.cpp** (`appendColliderWireframes`, `appendSelectionMarkers`): läuft per Frame über alle Collider/Selektionen (Aufruf Z. 4344–4357). Dort auf Caching (nur bei Änderung neu bauen) prüfen. `show.colliders` ist aus dem Menü schaltbar, aber an.
- **HE_Editor/ViewportPanel.cpp:91–** (`appendGroundGrid`): Das Gitter ist bereits gecacht (`s_grid`, nur bei Kameraänderung neu). Pro Frame kommen aber zwei Kopien der ~2 000 Linien hinzu: `out.append(s_grid.lines())` und `m_lastDebugLines.assign(...)` (Z. 4744), dann kopiert vermutlich `SetDebugLines` noch einmal in der Backend-Schicht. Billig, aber ein "Static-Lines-Layer" im Renderer würde alle drei sparen (HE_Rendering).
- **HE_Core/ContentManager** (`pollHotReload` alle 1,5 s im UI-Thread, Z. 3435; periodisches `refreshContentFolder` auf einem `std::async`-Thread alle `ContentBrowserRefreshRate` Sekunden, Z. 3342): Der Hot-Reload-Poll stat't vermutlich alle geladenen Assets. Ein File-Watcher (inotify/ReadDirectoryChangesW/FSEvents) würde das ersetzen. Messen auf großen Projekten.
- **HE_Editor/TerrainSync.cpp:297–303** (`serializeWithoutGrids`): `GridsAside` verschiebt pro Aufruf Raster. Wenn die Raster per Move zur Seite gelegt werden, ist es billig, sonst (Kopie) wäre EDT-404 teurer als beschrieben.
- **HE_Editor/EditorUI.cpp, `EditorUI::unsavedAssetPaths()`, `tabHasUnsavedEdits`**: werden im `updateAssetCollabSync`-Schleifenkörper pro Tab und Frame aufgerufen (Z. 10252, 10341), Kosten dort prüfen.
- **HE_Scene/EngineApi.cpp**: `HE::api::fs::setSandboxRoot`/`perm::set` sind globale Setter und werden pro EngineCall gerufen (EDT-407). Ein `HE::api::Ctx`, der die Host-Funktionen als Zeiger statt als `std::function`-Kopien trägt, spart die fünf Kopien an jeder Aufrufstelle (Lua/Python/HC/MCP).
- **HE_Editor/EditorConfig.h**: Eine Feldtabelle (EDT-411) kann zugleich die Quelle für Preferences, MCP-Settings und die Hilfe-Audits sein.

## Unsicher / Messung nötig

- EDT-402: Wie viele Entities sind in realen Sessions typisch, und wie lange dauert `syncStructuralChanges` tatsächlich pro Frame? Mit `HE_PROFILE_SCOPE_N("StructSync")` messen.
- EDT-404: Kosten von `serializeWithoutGrids` für Terrain-/Foliage-/Skeletal-Entities, abhängig davon, ob `GridsAside` verschiebt oder kopiert.
- EDT-406: ob die 60 `[this]`-Lambdas im SBO der verwendeten `std::function` liegen (libstdc++: bis 16 Byte, MSVC: bis 32 Byte, libc++: bis 24 Byte). Wenn ja, keine Heap-Kosten, dann nur die Konstruktion.
- EDT-414: Ob die Backends bei unveränderten Settings ihre Pipelines/Targets neu bauen. Falls sie selbst vergleichen, ist der Push pro Frame wertlos teuer nur im Aufbau der Structs.
- Joint-Overlay (Z. 4370–4461): pro Joint zwei `worldMatrixOf` (Parent-Kette) und `findByEntityId` pro Frame. Bei tausenden Joints und aktivem `show.joints` messen.
- Rope-/Trail-Guides und `appendRootMotionPreview`/`appendLookAtPreview` (Z. 2858–2966) allokieren einen `std::vector<glm::vec3> path` pro Frame für ein Entity. Der Kommentar rechtfertigt das. Nur relevant bei vielen Frames mit großer Auswahl.
- Datenrennen-Verdacht (nicht verifiziert): `startToolchainProbe()` (Z. 2324) und `startGitProbe()` (Z. 2396) werden aus UI-Callbacks mitten im Frame gestartet, in dem der Dialog noch `ctx.toolchainProbe`/`ctx.gitProbe` (Zeiger auf `m_toolchainProbe`/`m_gitProbe`) liest, während der Worker das Objekt neu zuweist. Das Flag wird zwar vorher auf `false` gesetzt, aber der Zeiger im laufenden Frame bleibt gültig. Prüfen, ob der Dialog das Ergebnis nach dem Klick im selben Frame noch liest.
- `healStuckImGuiKeys` (Z. 2975): einmal pro Sekunde 512 Scancodes mit `SDL_GetKeyFromScancode`. Vermutlich vernachlässigbar.

## Gelesen

Ja, `src/HE_Editor/EditorApplication.cpp` wurde vollständig gelesen (Zeilen 1–14 759) in Blöcken von 650–1 000 Zeilen, weil das Lesewerkzeug 1 500-Zeilen-Blöcke wegen des Token-Limits abgelehnt hat. Die Witness-Abschnitte 5321–10075 wurden gelesen, aber wegen ihrer Gleichförmigkeit nur auf Duplikate, Fehler und Struktur hin analysiert, nicht auf die Korrektheit ihrer Bildlogik. Es wurde kein Quellcode geändert und nichts kompiliert oder gemessen.
