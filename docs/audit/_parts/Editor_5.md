# Editor Slice 5 – EditorApplication.h … EditorGuides.h (16 Dateien, ~4 200 Zeilen)

## Zusammenfassung
Der Slice besteht überwiegend aus Headern, Daten und kleinen, gut kommentierten Hilfsmodulen (Kamera, Dock-State, Command-Gateway, Guides-Bauteile). Es gibt keine eigentlichen Per-Frame-Hot-Paths in diesen Dateien selbst; die belegbaren Kosten liegen in dem, was die Header auslösen (`AppContext`-Neuaufbau, Struktur-Diff, Typ-Cache-Kaltstart) und in `EditorCommands.cpp` (unnötige Serialisierung, O(n)-UUID-Suche mit String-Bau pro Entity). Insgesamt 11 Funde: 0 Hoch, 5 Mittel, 6 Niedrig. Alle Zeilenangaben wurden gegen den Ist-Stand geprüft; Aussagen zu Dateien außerhalb des Slice (EditorApplication.cpp, EditorUI.cpp, TaskBoardPanel.cpp, ContentBrowserPanel.cpp) sind als solche gekennzeichnet und gezielt per grep/Ausschnitt verifiziert, nicht vollständig gelesen.

## Funde

### EDT-501 – Collab-Struktur-Diff baut jeden Frame ein komplettes `unordered_set<Entity>` und hält vier knotenbasierte Hash-Container
- **ID**: EDT-501
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorApplication.h:923-936` (Container); Verbraucher `EditorApplication.cpp:11400-11420`, `11529-11532`, `11595-11600` (nur Ausschnitte gelesen)
- **Problem**: Solange eine Collab-Session läuft (`EditorApplication.cpp:4976`: `if (m_collab.inSession()) syncStructuralChanges();`), wird pro Frame über alle Entities ein neues `std::unordered_set<Entity> current` aufgebaut (ein Heap-Knoten pro Entity, kein `reserve`), danach mehrfach über `m_structureKnown` iteriert (`m_structureParent.count(e)`, `current.count(e)`). Dazu kommen drei weitere knotenbasierte Maps mit demselben Schlüssel (`m_structureParent`, `m_structureUnsendable`, `m_structureUndeletable`). Bei einigen tausend Entities sind das tausende Allokationen/Frees und Hash-Lookups pro Frame, obwohl sich in aller Regel nichts geändert hat.
- **Code-Auszug**:
```cpp
std::unordered_set<Entity> current;
reg.view<entt::entity>().each([&](auto e) {
    if (!isLocalOnly(e)) current.insert(e);
});
...
for (const Entity e : m_structureKnown)
    if (reg.valid(e) && !m_structureParent.count(e)) ...
```
- **Lösung**: (1) `current` als Member behalten und pro Frame `clear()` + `reserve(reg.storage<entt::entity>().size())`, damit die Bucket-Arrays nicht neu wachsen (Knoten bleiben, aber die Rehash-Kosten entfallen); besser (2) gar keine Menge bauen: `reg.valid(e)` für `m_structureKnown` reicht für "Destroyed", und "Created" kann über die Differenz `registry.size()` gegen `m_structureKnown.size()` plus einen Early-Out (Anzahl und Entity-Storage-Revision unverändert → nur Reparent-Check) abgekürzt werden. (3) Die drei Nebencontainer zu einer `unordered_map<Entity, StructInfo{parent, unsendableFrames, undeletableFrames}>` zusammenführen (ein Lookup statt drei).
- **Erwarteter Effekt**: Schätzung: 1–3 Größenordnungen weniger Allokationen/Frame in Sessions mit großen Szenen; im Solo-Betrieb kein Effekt. Messen mit Szene ≥ 5 000 Entities und aktiver Session.
- **Risiko / Verifikation**: Die Regeln liegen in `StructuralSync.h` und sind getestet (`tests/` Struktur-Sync-Tests suchen); jede Änderung muss "Echo-Schutz" (`forgetStructureOf`) und die Retry-Zähler erhalten. Early-Out nur, wenn Storage-Größe UND ein Struktur-Revisionszähler unverändert sind, sonst werden Reparents verpasst.

### EDT-502 – `EditorCommands` serialisiert Undo-Daten, die nur eine von zwei Senken liest
- **ID**: EDT-502
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorCommands.cpp:257-288` (applyCreate), `290-306` (applyDestroy), `363-402` (applyComponents); `EditorCommands.h:174-178`
- **Problem**: `execute()` füllt `Recorded` immer vollständig (`serializeSubtree`, `serializeEntityComponents` je zweimal), obwohl das Ergebnis ausschließlich in `if (sink) sink->recordCommand(rec, origin)` (Zeile 250) landet und `SnapshotUndoSink::recordCommand` die Default-No-op-Implementierung hat (nur `CollabUndoSink` liest `rec`). Außerhalb einer Session (Solo, `sink == m_snapshotSink`) und bei `Origin::Remote` (`sink == nullptr`) ist die Serialisierung vollständig verschwendet. Zusätzlich wird in `applyComponents` der Zustand nach dem Schreiben ein zweites Mal serialisiert (Zeile 382 und erneut 399 für `publishComponents`), obwohl `rec.afterComponents` identisch ist.
- **Code-Auszug**:
```cpp
rec.beforeComponents = serializer.serializeEntityComponents(*m_world, cmd.target);
...
rec.afterComponents = serializer.serializeEntityComponents(*m_world, cmd.target);
...
m_hooks.publishComponents(..., serializer.serializeEntityComponents(*m_world, cmd.target));
```
- **Lösung**: `execute()` bestimmt `sink` bereits vor dem Apply; ein `bool wantRecord = sink && sink != m_snapshotSink` (oder eine virtuelle `bool IUndoSink::needsRecord() const`) an die `apply*`-Funktionen durchreichen und die Serialisierung nur dann ausführen. In `applyComponents` für `publishComponents` `rec.afterComponents` wiederverwenden (bei `!wantRecord` einmal lokal serialisieren). Der Destroy-Fall bleibt in Sessions nötig, weil `rec.subtree` dort die Undo-Daten ist.
- **Erwarteter Effekt**: Schätzung: pro MCP-Create/Destroy/SetComponents ein bis zwei vollständige Subtree-/Komponenten-Serialisierungen weniger (bei großen Prefabs Millisekunden). Aktuell gehen nur MCP-Tools durch das Gateway (Kommentar `EditorApplication.h:880-885`), daher selten; wächst, sobald UI und Remote-Handler migriert werden.
- **Risiko / Verifikation**: Bestehende Gateway-Tests (`tests/` zu EditorCommands/CollabUndo) müssen unverändert grün bleiben; neuer Test: Solo-Pfad ruft nie `serializeSubtree` (Zähler im Test-Hook) und `publishComponents` erhält denselben Blob wie bisher.

### EDT-503 – `entityByUuid`: O(n) mit `snprintf` + `std::string`-Allokation pro Entity
- **ID**: EDT-503
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorCommands.cpp:406-429`; Aufrufer `TaskBoardPanel.cpp:547,550,579,603` (Orphan-Check), `McpToolsEntity.cpp:206,443,573`
- **Problem**: Die Suche formatiert für jede Entity die UUID neu in einen `std::string` (`uuidOf`: `snprintf` + Konstruktion) und vergleicht Strings, statt die gesuchte UUID einmal zu parsen und die zwei `uint64` zu vergleichen. Der Header begründet den fehlenden Index zwar ("Aufruf einmal pro Request"), aber `TaskBoardPanel::refreshOrphans` ruft sie pro Entity-Link und Sekunde (`TaskBoardPanel.cpp:520-526` throttled auf 1 s) auf, also O(Links × Entities) Allokationen pro Sekunde.
- **Code-Auszug**:
```cpp
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;
    if (uuidOf(world, e) == uuid) found = e;
});
```
- **Lösung**: Überladung `Entity entityByUuid(HorizonWorld&, const HE::UUID&)` die direkt `c.id.hi/lo` vergleicht; die String-Variante parst einmal (`HE::uuidFromHex` existiert, genutzt in `TaskBoardPanel.cpp:675`) und delegiert. `TaskBoardPanel` kann dann `l.id` direkt übergeben statt `HE::uuidToHex(l.id)`. Außerdem Schleife mit `break` beenden (entt `each` kann nicht abbrechen; stattdessen `for (auto [e, c] : view.each())` mit `return e`).
- **Erwarteter Effekt**: Schätzung: 20–50× schneller pro Suche, keine Heap-Allokationen. Messen mit 5 000 Entities × 20 Entity-Links.
- **Risiko / Verifikation**: Verhalten identisch, solange die String-Variante exakt dasselbe Format akzeptiert wie `uuidOf` erzeugt (32 Zeichen, Kleinbuchstaben). `uuidFromHex` akzeptiert evtl. Großbuchstaben/Bindestriche; dann würden bisher erfolglose Eingaben plötzlich matchen. Test: bestehende MCP-Entity-Tests + je ein Fall für ungültige/Großschreibung.

### EDT-504 – Kalter Asset-Typ-Cache liest nach jedem Content-Refresh synchron Dateiheader im Draw-Pfad
- **ID**: EDT-504
- **Kategorie**: E / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorAssetTypeCache.cpp:18-40`; `EditorAssetTypeCache.h:5-15`; Aufrufer `ContentBrowserPanel.cpp:1361`, `1851` (nur Ausschnitt gelesen); Invalidierung `ContentBrowserPanel.cpp:2976,3844`, `EditorApplication.cpp:830`, `EditorUI.cpp:770`
- **Problem**: `invalidateAll()` leert den Cache bei jedem Content-Refresh. Danach öffnet `sniffHeader` im UI-Thread pro unbekanntem Pfad eine `std::ifstream` (Open + 32-Byte-Read). Besonders kritisch ist `ContentBrowserPanel.cpp:1851`: der Typfilter läuft über den ganzen (rekursiven) Dateibaum, nicht nur über sichtbare Zeilen. Der Kommentar dort ("costs one header read per asset ONCE") stimmt nur bis zur nächsten Invalidierung. Auf einem Projekt mit tausenden Assets (oder Netzlaufwerk/Antivirus unter Windows) ergibt das einen Frame-Hänger direkt nach jedem Refresh.
- **Code-Auszug**:
```cpp
HE::AssetType sniffHeader(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    ...
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
```
- **Lösung**: Der Refresh läuft bereits auf einem Worker (`m_contentRefreshFuture`, `EditorApplication.h:1226`). Dort den Typ beim Scan mitlesen (die Datei wird ohnehin per `directory_iterator` angefasst) und per `prime(path, type)` in den Cache schreiben bzw. den Cache nicht leeren, sondern pro Eintrag über `(mtime,size)` validieren. Alternativ `invalidateAll()` nur als Generationszähler erhöhen und Einträge lazy neu validieren.
- **Erwarteter Effekt**: Schätzung: Wegfall von N synchronen Opens im Frame nach jedem Refresh; Größenordnung ms bis 100 ms bei 5–10 k Dateien. **Unsicher / messen**, siehe unten.
- **Risiko / Verifikation**: Cache-Korrektheit nach Löschen/Umbenennen (genau der Bug, den die Datei dokumentiert) muss erhalten bleiben; `tests/` zum Typ-Cache (Suche `AssetTypeCache`) um "Datei ersetzt durch anderen Typ" ergänzen. Thread-Sicherheit: Cache ist bewusst UI-only und lock-frei; Priming vom Worker bräuchte eine Übergabeliste, die der UI-Thread einträgt.

### EDT-505 – `AppContext`/`makeContext()`: ~60 `std::function`-Lambdas und ~100 Referenz-/Feldinitialisierungen pro Aufruf, zweimal pro Frame in Sessions
- **ID**: EDT-505
- **Kategorie**: A / C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorApplication.h:92-586` (AppContext), `EditorApplication.cpp:12005-12429` (makeContext, nur Anfang/Struktur gelesen); Aufrufe `EditorApplication.cpp:5095` (jeder Frame), `10219` (`updateAssetCollabSync`, jeder Frame in Session, `:4977`)
- **Problem**: `AppContext` ist ein God-Struct mit rund 60 `std::function`-Feldern und Dutzenden Referenzen. `makeContext()` (≈ 410 Zeilen, 61 `[this`-Lambdas gezählt) baut es bei jedem Aufruf komplett neu, obwohl sich fast nichts ändert. In einer Session passiert das zweimal pro Frame, weil `updateAssetCollabSync` sich einen eigenen Kontext baut, der zudem nur zur Weitergabe dient (`const std::string& root = ...; (void)root;`, Zeilen 10218-10221 sind toter Code). Jedes `std::function` mit `[this]`-Capture passt in den Small-Buffer (kein Heap), die Kosten sind also nur Konstruktion/Destruktion ≈ 4–6 KB, eher niedrig; dafür ist die Struktur der eigentliche Komplexitätstreiber (jedes neue Feature fügt hier ein Feld plus Lambda hinzu).
- **Code-Auszug**:
```cpp
AppContext ctx = makeContext();

(void)root;
std::unordered_set<std::string> openRel;
```
- **Lösung**: Kurzfristig `updateAssetCollabSync(nowMs, ctx)` den Frame-Kontext aus `OnRender` weiterreichen (eine Konstruktion/Frame) und die toten Zeilen 10218/10221 entfernen. Mittelfristig `AppContext` in einen langlebigen Member aufteilen (Callbacks einmal in `OnInit` binden) und pro Frame nur die veränderlichen Werte (`isPlaying`, `sceneDirty`, `recoveryOffer`, `toolchainProbe`, …) aktualisieren; Callback-Gruppen als Interfaces (`IProjectActions`, `IGitActions`) statt 60 Einzel-`std::function`.
- **Erwarteter Effekt**: Schätzung: einige µs pro Frame, nicht messbar relevant; Nutzen ist Vereinfachung/Wartbarkeit. **Messen**, bevor mehr als die Kurzfrist-Variante umgesetzt wird.
- **Risiko / Verifikation**: `makeContext()` ruft außerdem `m_selection.prune(...)` auf (Zeile 12011); wird der Aufruf in `updateAssetCollabSync` entfernt, muss das Prune vorher im Frame schon gelaufen sein (ist es, `OnRender` ruft es früher auf Zeile 5095 – vor dem Weiterreichen prüfen, dass die Reihenfolge Zeile 4977 vs. 5095 stimmt, denn `updateAssetCollabSync` läuft VOR dem UI-Kontext).

### EDT-506 – Asset-Tab-Dispatch: Kette von bis zu 19 `isXAsset(path)`-Prädikaten, jedes mit eigenem Path-Hash und Map-Lookup
- **ID**: EDT-506
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorAssetTypeCache.cpp:30-45`, `EditorAssetTypeCache.h:22-25`; Aufrufer `EditorUI.cpp:1245`, `4119-4150ff` (19 Treffer `Panel::is…Asset(` in EditorUI.cpp)
- **Problem**: Der aktive Tab fragt pro Frame in einer `else if`-Kette nacheinander, ob der Pfad "mein Typ" ist; jede Abfrage hasht den Pfad-String (`std::unordered_map<std::string,…>::find`) erneut. Die Information ist ein einziger `AssetType`-Wert.
- **Code-Auszug**:
```cpp
else if (MaterialEditorPanel::isMaterialAsset(tabPath) || MaterialEditorPanel::isMaterialFunctionAsset(tabPath)) ...
else if (UIEditorPanel::isWidgetAsset(tabPath)) ...
else if (HorizonCodeClassPanel::isClassAsset(tabPath)) ...
```
- **Lösung**: Einmal `const auto type = EditorAssetTypeCache::assetTypeOf(tabPath);` und `switch (type)` (oder eine Tabelle `AssetType → render-Funktion`). Die `isXAsset`-Funktionen bleiben für andere Aufrufer erhalten.
- **Erwarteter Effekt**: Schätzung: bis zu ~18 Hashes/Lookups pro Frame weniger (Sub-µs-Bereich); überwiegend Lesbarkeit.
- **Risiko / Verifikation**: Reihenfolge der Kette darf sich nicht ändern, wo ein Prädikat mehrere Typen akzeptiert (z. B. `isMaterialAsset || isMaterialFunctionAsset`, Partikel-Graph); Aufruf aus `EditorUI.cpp` liegt außerhalb dieses Slice.

### EDT-507 – `EditorCamera`: Basisvektoren werden mehrfach neu berechnet; dreifach wiederholter Bewegungs-Check
- **ID**: EDT-507
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorCamera.cpp:14-41`, `146-204`, `221-226`, `98-116`
- **Problem**: `forward()` (4 Trig-Aufrufe + `normalize`) wird von `right()`, `up()`, `viewMatrix()` jeweils neu aufgerufen: `viewMatrix()` rechnet `forward()` dreimal und `upReference()` zweimal (`up()` → `right()` → `forward()`). `update()` ruft bei Pan `right()` und `up()` (also `forward()` dreimal), bei Fly-Move zusätzlich `forward()` direkt. `currentPreset()` rechnet für jede der 6 Presets erneut `sin`/`cos`. Außerdem steht `in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f` dreimal in `update()`.
- **Code-Auszug**:
```cpp
return glm::lookAt(m_position, m_position + forward(), up());
...
if (in.orbit && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
if (in.look  && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
if (in.pan   && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
```
- **Lösung**: `struct Basis { vec3 f, r, u; }` und `Basis basis() const` (eine `forward()`-Berechnung); `viewMatrix`, Pan und Fly-Move nutzen `basis()`. In `update()` `const bool moved = in.mouseDelta != glm::vec2(0.0f);`. `currentPreset()` bleibt wegen Yaw-Wrap (Back: yaw = π vs. −π) beim Vektorvergleich; die Presets könnten als `constexpr`-Tabelle der Vorwärtsvektoren vorliegen.
- **Erwarteter Effekt**: Schätzung: wenige hundert ns pro Frame; reine Aufräumarbeit, kein messbarer Gewinn zu erwarten.
- **Risiko / Verifikation**: Float-Ergebnisse bitgleich, solange dieselben Ausdrücke verwendet werden; Tests für die Kamera/Presets (`tests/`, Suche `EditorCamera`) laufen lassen.

### EDT-508 – `EditorApplication.h`: Icon-Liste vierfach gepflegt, schwerer Header in 81 Dateien
- **ID**: EDT-508
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:475-497` (`AppContext::CbIcons`), `1383-1405` (`m_icon*`), `EditorApplication.cpp:1712` (Ladetabelle), `12372` (Kopie in `makeContext`); Includes `EditorApplication.h:2-59`
- **Problem**: Jeder neue Asset-Typ-Icon braucht Einträge an vier Stellen (Struct-Feld, Member, Ladeliste, Kopie in `makeContext`); 20 `ImTextureID`-Paare sind derselbe Datensatz zweimal. Der Header zieht ca. 45 Includes (`HorizonScene/*`, `ContentSync`, `Net`, `Mcp*`, `GitController`, …) nach, und wird in 81 Dateien (`grep -l`) des Editors eingebunden; jede Änderung an einem dieser Header kompiliert den halben Editor neu.
- **Code-Auszug**:
```cpp
struct CbIcons { ImTextureID folder = 0; ImTextureID material = 0; ... } cbIcons;   // AppContext
...
ImTextureID m_iconFolder = 0; ImTextureID m_iconMaterial = 0; ...                   // EditorApplication
```
- **Lösung**: `CbIcons` als frei stehendes Struct definieren und `EditorApplication` genau eine Instanz `m_cbIcons` halten; `AppContext` bekommt eine Referenz/Kopie (`ctx.cbIcons = m_cbIcons`) statt 20 Einzelkopien; Ladetabelle als `{ "Folder.png", &m_cbIcons.folder }`. Für die Includes: `AppContext` und `EditorConfig`-artige Daten in eigene Header (`AppContext.h`), die Panels nur diese einbinden; Mitglieder wie `m_gameServicesBinding`, `m_netServices` etc. hinter ein Pimpl.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere inkrementelle Builds, 40–60 Zeilen weniger Pflegecode; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Rein mechanisch; Build aller Ziele (`HorizonEditor`, `he_tests`) reicht. Das Pimpl-Vorhaben ist größer und sollte separat geplant werden.

### EDT-509 – Veraltete/verrutschte Kommentare und Kodierungsfehler in `EditorApplication.h`
- **ID**: EDT-509
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:36`, `665-677`, `832-836`, `EditorApplication.cpp:10218-10221`
- **Problem**: (a) Zeile 36 enthält Mojibake: `// the multiplayer session (plan Â§5.6)` (UTF-8 als Latin-1 gelesen/gespeichert; gemeint `§5.6`). (b) Der Kommentarblock "Per-project open-tab persistence … restoreOpenTabs runs on project load; saveOpenTabs …" (665-667) steht vor `applyAssetBytes` und gehört zu `saveOpenTabs`/`restoreOpenTabs` (676-677). (c) Der Kommentar "The local MCP listener. Pumped once per frame…" (832-835) steht über `m_mcpCameras`, der Member `m_mcp` folgt erst Zeile 861. (d) `m_hubProjectName/m_hubProjectDir` werden als Roh-`char*` + Größe durch `AppContext` gereicht (519-522), obwohl `std::string` genügte (nur Hub-Dialog, daher Niedrig).
- **Code-Auszug**:
```cpp
#include <HorizonScene/Net/NetGameSession.h>        // the multiplayer session (plan Â§5.6)
```
- **Lösung**: Zeichen korrigieren, Kommentarblöcke an die zugehörigen Deklarationen verschieben, toten `root`/`(void)root` entfernen.
- **Erwarteter Effekt**: Nur Lesbarkeit.
- **Risiko / Verifikation**: Keines (Kommentare). Datei als UTF-8 speichern.

### EDT-510 – Guides: eager gebauter, doppelt gehaltener Text und Mehrfachkopien beim Seitenbau; ungenutztes `numbers()`
- **ID**: EDT-510
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorGuides.cpp:57-71`, `101-110`, `150-197`, `588-616`; `EditorGuides.h:102`; Aufruf `DocsPanel.cpp:240`
- **Problem**: (a) `install()` baut beim Laden der Docs alle vier Tracks (~1 700 Zeilen Inhalt in `Guides/*.cpp`) sofort; `section()` legt zusätzlich den gesamten Suchtext (`s.text`) als zweite Kopie jedes Strings an. Das ist Einmalarbeit, aber der Speicher ist dauerhaft doppelt belegt. (b) `bullets({...})`, `table(..., {{...}})` und `steps({...})` nehmen `std::vector<std::string>`/`initializer_list`-Elemente, die `const` sind: jede Literal-Zeichenkette wird beim Aufbau mehrfach kopiert (Literal → `std::string` in der Initializer-Liste → Kopie in den Vektor → Move in die Zelle). Nur Einmalkosten beim Docs-Laden. (c) `Guides::numbers()` (`EditorGuides.cpp:79-82`, `.h:102`) hat keinen Aufrufer (kein `numbers({` im Repo außer dem unabhängigen `McpToolsApi::numbers`).
- **Code-Auszug**:
```cpp
Docs::Block numbers(std::vector<std::string> items)
{
    return list(Docs::BlockKind::Numbers, std::move(items));
}
```
- **Lösung**: (a) Suchtext lazy beim ersten Suchen bauen, oder `Section::text` ganz weglassen und die Suche über die Blöcke laufen lassen (nur wenn der Speicher stört). (b) Nur bei nachgewiesenem Startzeit-Beitrag ändern. (c) `numbers()` entfernen oder bewusst als API für die Content-Tracks stehen lassen (Header-Kommentar nennt es Teil der "whole surface a track needs").
- **Erwarteter Effekt**: Schätzung: wenige hundert KB Speicher und < 10 ms einmalig. **Messen**, bevor angefasst wird.
- **Risiko / Verifikation**: Die Suche ist getestet (`tests/test_editor_help.cpp:900,970` ruft `Guides::install` und prüft Seiten-IDs); jede Änderung an `Section::text` braucht einen Test, dass Callout-/Flow-Texte weiter gefunden werden (das ist die dokumentierte Falle des Moduls).

### EDT-511 – `EditorDockState`: Haltezeit in Frames statt Zeit; `isGraphicsRow` baut pro Aufruf temporäre Liste
- **ID**: EDT-511
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorDockState.cpp:54-57,158`; `src/HE_Editor/EditorGraphics.cpp:55-62`
- **Problem**: (a) `kHoldMaxFrames = 60 * 90` ("90 Sekunden") ist frameabhängig: bei unbegrenzter FPS (vsync aus) endet die Haltezeit nach wenigen Sekunden, bei 30 FPS nach 3 Minuten; Kommentar und Verhalten weichen dann ab. Gleiches gilt für `kHoldSettleFrames = 12`. (b) `isGraphicsRow` (wird in `EditorSettingsPanel.cpp:244` pro gezeichneter Zeile aufgerufen) legt pro Aufruf eine `initializer_list<RowSet>` an und vergleicht bis zu 11 Strings per `strcmp`; ohne Allokation, aber unnötig kompliziert gegenüber einem `constexpr`-Array der Keys.
- **Code-Auszug**:
```cpp
constexpr int kHoldMaxFrames = 60 * 90;
...
for (const RowSet& r : { qualityRows(), postProcessingRows(), reflectionsAndGiRows() })
```
- **Lösung**: (a) Zeitbasis über `ImGui::GetTime()` (`s_heldSince`) statt Frame-Zähler; `kHoldSettleFrames` bleibt frameweise (es geht um ImGui-interne Fokus-Latenz in Frames). (b) Ein gemeinsames `constexpr std::array<const char*, 11> kAllRows` und `std::any_of`; die drei `RowSet`-Funktionen als Slices darauf.
- **Erwarteter Effekt**: Verhaltenskonstanz der Haltezeit über FPS-Werte; (b) vernachlässigbar.
- **Risiko / Verifikation**: `tests/test_editor_dock_state.cpp` und `tests/test_editor_graphics.cpp` treiben beides headless; für (a) Test mit simulierter Zeit statt Frames anpassen (der Test arbeitet vermutlich mit Frame-Zählern: vorher prüfen).

## Cross-Modul-Hinweise
- `src/HE_Editor/ContentBrowserPanel.cpp:1838-1856`: Beim Filtern/Suchen wird pro Datei und Frame `std::string name = f->name` kopiert und per `tolower` umgewandelt (Zeilen 1844-1845), und der Typfilter läuft über den gesamten Baum, nicht nur über sichtbare Zeilen (siehe EDT-504). Ein vorberechneter Kleinschreib-Name pro Datei (im Scan) würde Kopie und Schleife sparen; gehört in den Content-Browser-Slice.
- `src/HE_Editor/TaskBoardPanel.cpp:547-550,579,603`: nutzt `entityByUuid(…, HE::uuidToHex(l.id))` (siehe EDT-503), also Formatierung auf beiden Seiten.
- `src/HE_Editor/EditorApplication.cpp:10218-10221`: toter Code (`root`, `(void)root`) in `updateAssetCollabSync`.
- `src/HE_Editor/EditorUI.cpp:4119ff`: Tab-Dispatch per Prädikat-Kette (EDT-506).
- `src/HE_Editor/EditorApplication.cpp:11400-11420`: `syncStructuralChanges` (EDT-501) gehört in den Slice, der diese Datei abdeckt; dort die Umsetzung koordinieren.

## Unsicher / Messung nötig
- EDT-501: tatsächliche Kosten von `syncStructuralChanges` pro Frame bei großen Szenen in einer Session (Zeitmessung um die Funktion, Allokationszähler).
- EDT-504: Dauer der Kaltstart-Sniffs nach `invalidateAll()` bei 5–10 k Assets auf Windows (Defender) und auf Netzlaufwerken; ob der Refresh-Worker den Typ ohnehin schon kennt.
- EDT-505: reale Kosten von `makeContext()` (Konstruktion + Destruktion von ~60 `std::function`); vermutlich < 10 µs.
- EDT-502: Anteil der Gateway-Aufrufe im Alltag; heute nur MCP, daher Nutzen erst bei Migration der UI-Pfade groß.
- EDT-510: Speicher/Startzeit der Guides-Seiten tatsächlich messen, bevor die Suchtext-Doppelung angefasst wird.

## Gelesen
Alle 16 Dateien des Slice vollständig gelesen (EditorApplication.h in Blöcken bis Zeile 1462); die Verifikation von Aufrufern erfolgte nur über gezielte grep-/Ausschnitt-Lesungen in EditorApplication.cpp, EditorUI.cpp, TaskBoardPanel.cpp, ContentBrowserPanel.cpp, DocsPanel.cpp (nicht vollständig gelesen, da außerhalb des Slice).
