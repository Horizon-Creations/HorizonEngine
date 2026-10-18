# Editor Slice 11 – InputMappingModel.h … MaterialEditorPanel.cpp

## Zusammenfassung

Slice 11 umfasst 10 Dateien (10 975 Zeilen): `InputMappingModel.h`, `InspectorPanel.{h,cpp}`, `InspectorPrefabKeys.cpp`, `LevelScriptPanel.{h,cpp}`, `MacMenuBar.{h,mm}`, `main.cpp`, `MaterialEditorPanel.cpp`. Der Code ist insgesamt sorgfältig kommentiert; echte Probleme sind selten Algorithmusfehler, sondern Arbeit im Draw-Pfad, die pro Frame wiederholt wird: rekursive Dateisystem-Scans in offenen Popups/Combos, ein quadratisches Model-Adapter-Muster im Material-Canvas, Lua-/Listen-Abfragen im Details-Panel und komplette Komponenten-Auflistungen pro Baumknoten. Strukturell dominieren eine 2 900 Zeilen lange Funktion (`renderForImpl`) mit 41-fach kopiertem Entfernen-Muster und drei parallel gepflegte Komponentenlisten. Es gibt 22 Funde: 2 Hoch, 9 Mittel, 11 Niedrig (nach Schweregrad sortiert, mit Ausnahme von EDT-1114, das als Mittel nachgeordnet steht). Alle Funde stammen aus Draw()-Code (Editor-Hot-Path), die Schweregrade berücksichtigen, ob der Pfad immer oder nur bei geöffneter Sektion/Popup läuft. Gemessen wurde nichts; wo die Wirkung von Datenmengen abhängt, ist das vermerkt.

## Funde

### EDT-1101 – Rekursiver Content-Scan (plus Datei-Header lesen) pro Frame, solange Combo/Popup/Submenü offen ist
- **ID**: EDT-1101
- **Kategorie**: D (auch E: I/O im Draw)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:3206-3219`, `src/HE_Editor/LevelScriptPanel.cpp:3031-3046`, `src/HE_Editor/MaterialEditorPanel.cpp:1317-1327` (Aufrufer-Implementierung: `src/HE_Editor/HcEditorUtil.cpp:55-92`, `93-103`)
- **Problem**: `HcEditorUtil::listAssets()` läuft per `recursive_directory_iterator` über Content- UND Engine-Content-Baum, liest von jeder `.hasset` den Header (`readAssetTypeFromFile`) und ruft `file_size` auf. Drei Stellen rufen das innerhalb des Draw-Codes eines offenen Popups auf, also jeden Frame, solange es offen ist:
  1. Script-Komponente, Klassen-Combo: zusätzlich `loadAsset(c.path)` für JEDE Klasse im Projekt, nur um die Selectable-ID zu vergleichen (die Klassen werden dadurch alle geladen).
  2. HorizonCode-Klassen-Tab, Base-Class-Popup: Scan + pro Klasse `resolveClassAsset(...)` (Ancestor-Kette lesen) pro Frame.
  3. Material-Canvas, Kontextmenü "Set Texture": Scan nach allen Texturen pro Frame.
  Dagegen macht derselbe Panel-Code in `drawPreviewSubjectPicker` (MaterialEditorPanel.cpp:1755-1768) es richtig ("Scanned once per popup-open, not per frame … silly at 60 Hz") – das Muster ist also bekannt, aber nicht überall angewendet.
- **Code-Auszug**:
```cpp
// InspectorPanel.cpp:3212
for (const auto& c : HcEditorUtil::listHorizonCodeClasses(ctx.contentManager))
{
	const HE::UUID id = ctx.contentManager ? ctx.contentManager->loadAsset(c.path) : HE::UUID{};
// MaterialEditorPanel.cpp:1323
for (const auto& a : HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::Texture))
	if (ImGui::MenuItem((a.label + "##" + a.path).c_str(), nullptr, n->s == a.path))
```
- **Lösung**: Gemeinsamen Mechanismus für "Liste beim Öffnen einfrieren" einführen, z.B. in `HcEditorUtil`: `const std::vector<ClassRef>& cachedAssets(ContentManager*, AssetType, bool refresh)`, wobei `refresh = ImGui::IsWindowAppearing()` (Popup) bzw. beim `BeginMenu`-Öffnen. Für die Script-Combo die ID erst beim Klick laden (`loadAsset` nur wenn `ImGui::Selectable` true liefert) und die Selected-Prüfung über `c.path == curPath` statt über die ID machen. Für das Base-Class-Popup die `wouldCycle`-Auflösung einmalig beim Öffnen in einen `std::vector<bool>` schreiben. Bei Texturen/Meshes zusätzlich Zeilen-Clipping (`ImGuiListClipper`), siehe EDT-1111. Besser noch: ein einziger asset-type-indexierter Cache im `ContentManager`/`EditorAssetTypeCache`, den Refresh/Import invalidiert.
- **Erwarteter Effekt** (Schätzung): Pro offenem Popup Wegfall eines kompletten Verzeichnisbaum-Walks mit N Header-Reads pro Frame; bei Projekten mit mehreren tausend Assets der Unterschied zwischen sichtbarem Stocken (>10 ms/Frame, Festplatten-/Cache-abhängig) und ~0. Reihenfolge der Einträge und Anzeige bleiben gleich.
- **Risiko / Verifikation**: Neu angelegte Assets erscheinen erst beim nächsten Öffnen – entspricht dem Verhalten des Mesh-Pickers. Manuell: Popup öffnen, Asset im Content Browser anlegen, Popup schließen/öffnen. Messen mit dem Profiler-Panel (Frame-Zeit bei offenem Popup, Projekt mit >2000 Assets).

### EDT-1102 – Material-Canvas: Model-Adapter mit linearem `findNode` je Callback (O(N²)), Vektor-/String-Kopien und ~25 `std::function` pro Frame
- **ID**: EDT-1102
- **Kategorie**: D (auch C, B)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1090-1117`, `135-148`, `1024-1075`; Aufrufer `src/HE_Editor/GraphEditor.cpp:283, 320, 340, 356, 677, 703, 728, 876` (außerhalb des Slice, nur zur Bestätigung gelesen)
- **Problem**: `GraphEditor::draw` ruft pro Frame und pro Node `model.getPos`, `model.pins`, `model.nodeBodyHeight`, `model.title`, `model.headerColor` (teils mehrfach). Jede dieser Lambdas macht `st.graph.findNode(id)`, und `findNode` ist eine lineare Suche (`GraphCommon/GraphModel.h:44-57`, Kommentar: "no index is kept"). Ergebnis: ca. 5–6 · N²/2 Vergleiche pro Frame. Dazu kommen: `m.nodeIds` und `m.links` bauen jeden Frame neue Vektoren, `m.title` liefert einen `std::string` (für FunctionCall zusätzlich `std::filesystem::path(...).stem()`), `categoryColor(const char*)` konstruiert pro Node und Frame einen `std::string` und vergleicht bis zu 10 Mal, `matNodePins` baut pro Node einen neuen `std::vector<Pin>` (Landscape-/Output-/FunctionCall-Nodes zusätzlich weitere Vektoren und `loadFunctionGraph`). Die Model-Struktur mit ~25 capturing `std::function` wird jedes Frame neu aufgebaut (Captures >16 Byte → Heap).
- **Code-Auszug**:
```cpp
m.getPos = [&st](int id, float& x, float& y){ if (const MatGraphNode* n = st.graph.findNode(id)) { x = n->x; y = n->y; } };
m.title  = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id); return n ? matNodeTitle(*n) : std::string(); };
m.headerColor = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id);
	return categoryColor(n ? HE::matNodeDesc(n->type).category : ""); };
ImU32 categoryColor(const char* cat) { const std::string c = cat; if (c == "Material") ...
```
- **Lösung**: (1) Zu Beginn von `drawMaterialCanvas` einmal eine Tabelle `id → MatGraphNode*` bauen (`std::unordered_map<int, MatGraphNode*>` oder, weil `nodeIds` ohnehin zuerst läuft, ein sortierter `std::vector<std::pair<int,MatGraphNode*>>` bzw. ein Index-Vektor `ids[i]`); alle Lambdas lesen daraus. Alternativ `MaterialGraph::findNode` O(1) machen (Id→Index-Cache, bei Laden/Undo neu aufbauen – das betrifft auch HorizonCode/ParticleGraph, siehe Cross-Modul). (2) `categoryColor` auf `strcmp` bzw. eine `constexpr`-Tabelle `{const char*, ImU32}` umstellen, oder die Farbe im `MatNodeDesc` hinterlegen. (3) `m.nodeIds`/`m.links` unverändert lassen, aber `reserve`-Puffer im State halten (`st.idScratch`) statt neu zu allozieren, sobald `GraphEditor::Model` Referenzen zulässt (API-Änderung, optional).
- **Erwarteter Effekt** (Schätzung): Bei 300 Nodes ~250k Vergleiche/Frame weniger, bei 1000 Nodes ~2,5 Mio.; dazu 3N–5N kleine Heap-Allokationen pro Frame. Für kleine Graphen (<50 Nodes) vernachlässigbar → deshalb "messen" (Profiler-Panel mit großem Landscape-Material).
- **Risiko / Verifikation**: Die Tabelle muss pro Frame (nicht pro State) gebaut werden, weil Callbacks (`removeNode`, `connect`, Paste) die Node-Liste ändern; Zeiger aus der Tabelle dürfen nach einer Mutation nicht mehr benutzt werden (die bestehenden Lambdas holen heute pro Aufruf frisch). Tests: `tests/test_material_graph.cpp` deckt Graph-Logik ab, aber nicht den Canvas; Sichtprüfung mit `HE_UI_DUMP_DIR` bzw. `scripts/he_uishot.py`.

### EDT-1103 – 2 900-Zeilen-Funktion `renderForImpl`, 41× Kopiervorlage, drei parallele Komponentenlisten
- **ID**: EDT-1103
- **Kategorie**: A
- **Schweregrad**: Mittel (Wartbarkeit; kein Laufzeitproblem)
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:845-3710` (`renderForImpl`), Wiederholungen z.B. `1425, 1439, 1478, 1511, … 3674`; Listen: `src/HE_Editor/InspectorPrefabKeys.cpp:226-273`, `InspectorPanel.cpp:3765-3847` (`kAddGroups`), die `if (auto* x = registry.try_get<T>(entity))`-Kette
- **Problem**: Jede Komponente ist ein eigener Block `if (auto* x = try_get<T>) { if (componentHeader("Label", removable, removed)) {…} if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<T>(entity); } }`. Das Entfernen-Muster steht 41-mal wörtlich im Code. Daneben existieren drei Listen, die dieselbe Komponentenmenge beschreiben und manuell synchron zu halten sind: die Header-Sektionen in `renderForImpl` (43 `componentHeader("…")`-Aufrufe), `kComponentKeys` (Label → Scene-Key, 44 Einträge, `InspectorPrefabKeys.cpp`) und `kAddGroups` (Label + `has/add`, `InspectorPanel.cpp`). Die Prefab-Sync-Tabelle wird zwar durch einen Test gegen `SceneSerializer::isKnownComponentKey` gehalten, aber nichts hält die drei Listen gegeneinander. Mehr als eine Bildschirmseite Funktionskörper pro Komponente macht Review und Verzweigung (collect/only/removeMatching/quiet in jeder Sektion) schwer lesbar.
- **Code-Auszug**:
```cpp
if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<TransformComponent>(entity); }   // 1425
if (removed) { if (undo) undo->snapshotNow(removeLabel.c_str()); registry.remove<Transform2DComponent>(entity); } // 1439
```
- **Lösung**: Schrittweise, ohne Verhaltensänderung: (a) Hilfs-Template `template<class T> void removeIfAsked(...)`/Makro-freie Lambda `finishSection<T>(removed)`; die 41 Zeilen werden zu `endSection<TransformComponent>()`. (b) Pro Komponente eine kleine freie Funktion `void drawLight(Ctx&, LightComponent&, Tracker&)` in eigenen Translation Units (`InspectorSections_*.cpp`: Animation, Physics, Environment, UI), `renderForImpl` bleibt eine Tabelle `{label, key, removable, has<T>, draw}`. (c) Aus derselben Tabelle `kComponentKeys` und die Add-Menü-Zeilen (`kAddGroups`) ableiten; nur Gruppe/"needsSkeleton" bleibt Zusatzdaten. Das passt auch zur Anforderung in CLAUDE.md (Panel kompakt, zentrale Pflege-Listen).
- **Erwarteter Effekt**: Weniger Fehlerquellen beim Hinzufügen einer Komponente (heute 3 Stellen + `EditorHelp.cpp`); Kompilierzeit pro Änderung sinkt (die Datei ist 4 060 Zeilen).
- **Risiko / Verifikation**: Hoch für Regressionen bei Verschieben der ImGui-ID-Struktur (`PushID`, `##`-Suffixe, `Help::Scope`-Reihenfolge, `trackEdit`-Positionen). Absicherung: `python3 scripts/editor_help_audit.py --check`, vorhandene `tests/test_*_ui.cpp` mit `HE_UI_DUMP_DIR` vor/nach vergleichen (Bitmap-Diff), plus Test für `listComponents`-Reihenfolge (heute implizit durch Code-Reihenfolge).

### EDT-1104 – Script-Sektion: Lua-Property-Abfrage und Script-Lookup pro Frame
- **ID**: EDT-1104
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:3241-3290` (Implementierung `src/HE_Core/src/Scripting/ScriptEngine.cpp:511-560`)
- **Problem**: Solange die Script-Komponente einer ausgewählten Entity aufgeklappt ist, läuft pro Frame: `isScriptLoaded(moduleName)` (String-Hash), `getScriptProperties(moduleName)` (`lua_rawgeti`, `lua_next` über die Property-Tabelle, pro Property `ScriptPropDef` mit `std::string`, Rückgabe als `std::vector` per Wert) und je Property `s->properties.find(def.name)`. Die Property-Definitionen ändern sich nur, wenn sich der Quelltext ändert. Außerdem kompiliert `loadScript(...)` im Draw (L3249), falls das Modul nicht geladen ist (einmalig, aber auf dem UI-Thread; bei Fehler im Skript jedes Frame erneut, weil `isScriptLoaded` dann false bleibt – unsicher, hängt von `loadScript`).
- **Code-Auszug**:
```cpp
if (!ctx.propScriptEngine->isScriptLoaded(s->moduleName))
	ctx.propScriptEngine->loadScript(s->moduleName, asset->sourceCode);
auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);
```
- **Lösung**: Statischen Cache `{moduleName, sourceCode.size()/hash, std::vector<ScriptPropDef>}` im Panel halten (oder in `EditorApplication` neben `propScriptEngine`); Neuberechnung nur wenn Modulname oder Quelltext-Hash (bzw. ContentManager-Revision des Scripts) sich ändern. Fehlgeschlagenes `loadScript` ebenfalls merken (negativer Cache pro Hash), damit nicht jedes Frame neu kompiliert wird.
- **Erwarteter Effekt** (Schätzung): Pro Frame ein Lua-Tabellen-Walk + N String-Allokationen weniger; sichtbar nur bei Skripten mit vielen Properties.
- **Risiko / Verifikation**: Hot-Reload eines Scripts muss den Cache ungültig machen (Hash des `sourceCode` genügt). Test: Script ändern, Inspector offen lassen, Properties müssen nachziehen.

### EDT-1105 – Entity-Picker-Liste (Name + UUID aller Entities) viermal kopiert, pro Frame O(Entities)
- **ID**: EDT-1105
- **Kategorie**: A / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:1859-1875` (IK Look Target), `2468-2484` (Camera Rig Target), `2645-2673` (Rope Attach), `2993-3016` (Joint Target)
- **Problem**: Vier fast identische Blöcke bauen bei jedem Frame, an dem die jeweilige Sektion offen ist, `std::vector<const char*> names` und `std::vector<HE::UUID> ids` über alle benannten Entities (`registry.view<NameComponent>()`, `all_of<TransformComponent>`, `world.entityId(e)`). Bei einer Szene mit 10–100 k Entities sind das pro Frame zehn- bis hunderttausende Registry-Zugriffe und zwei wachsende Vektoren (kein `reserve`), obwohl die Liste nur bei geöffneter Combo gebraucht wird (nur dann werden die Namen gezeichnet; der Combo-Vorschauwert braucht nur den aktuellen Namen).
- **Code-Auszug**:
```cpp
std::vector<const char*> names{ "(none)" };
std::vector<HE::UUID>    ids{ HE::UUID{} };
for (auto [e, name] : registry.view<NameComponent>().each())
{
	if (e == entity || e == world.rootEntity()) continue;
	if (!registry.all_of<TransformComponent>(e))  continue;
	const HE::UUID id = world.entityId(e);
```
- **Lösung**: Eine Hilfsfunktion `bool entityCombo(HorizonWorld&, Entity self, const char* label, HE::UUID& target, const char* noneText)` in `EditorWidgets` (oder lokal), die (1) die Vorschau aus `world.findByEntityId(target)` ermittelt (ein Lookup) und (2) die Liste nur in `BeginCombo(...)`-Rumpf iteriert, mit `ImGuiListClipper` für große Szenen und optionaler Suche. Das ersetzt vier Kopien und liefert Clipping nach CLAUDE.md Abschnitt 4.
- **Erwarteter Effekt** (Schätzung): Pro Frame mit offener Sektion in großen Szenen mehrere ms weniger; Code −60 Zeilen.
- **Risiko / Verifikation**: Reihenfolge der Einträge (View-Reihenfolge) muss gleich bleiben; ImGui-Combo-Verhalten (Auswahl-Commit, `snapshotNow`) beibehalten. Test: Rope/Joint in Szene mit gesetztem Ziel öffnen, Wert bleibt; UI-Test mit Dump.

### EDT-1106 – Komponentenbaum der Klassen-Tab ruft für jede Entity pro Frame `listComponents` (kompletter Details-Body im Collect-Modus)
- **ID**: EDT-1106
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:2599-2605`, `2623-2640`
- **Problem**: In `drawComponentsBody` läuft pro Frame (1) `listComponents` für die fokussierte Entity (L2602) und (2) pro Baumknoten `listComponents(…, comps)` (L2639) plus Kopie der Kinderliste (`const std::vector<Entity> kids = hier ? hier->children : …`) und eines `std::string label`. `listComponents` ruft `renderForImpl` mit `collect` auf, d.h. es werden alle ~43 `try_get`-Sektionen und je `componentHeader` (inkl. `Help::setScope`, `std::string removeLabel`-Bau, EditorUndo-Context-Konstruktion) durchlaufen. Das Ergebnis ändert sich nur bei strukturellen Änderungen (Add/Remove/Paste/Reset, diese sind alle dem Panel bekannt: `st.dirty = true`/Rückgabewert von `renderFor`). Dasselbe gilt für `refreshMultiSelection` (InspectorPanel.cpp:171-176), das aber gecached ist.
- **Code-Auszug**:
```cpp
std::vector<std::string> comps;
InspectorPanel::listComponents(ctx, *st.compWorld, e, comps);
```
- **Lösung**: Pro Entity ein `{std::vector<std::string> comps; uint64_t structuralRev;}` im `ClassState` halten; `structuralRev` wird bei Add/Remove (Rückgabewert `renderFor`, `addComponentMenu`, `removeComponent`, `Add Child`, `Remove`) erhöht. Alternativ: `listComponents` selbst auf eine schlanke Funktion `componentLabels(registry, entity)` umstellen, die ohne ImGui-Kontext und ohne `Help::Scope`/`removeLabel` auskommt (siehe EDT-1103: bei tabellengetriebenem Design ist das ein Einzeiler).
- **Erwarteter Effekt** (Schätzung): Linear in Anzahl Baumknoten; bei typischen Klassen (3–10 Entities) gering, bei großen Hierarchien merklich. Ein Messen im Class-Tab mit 50+ Kindern lohnt sich.
- **Risiko / Verifikation**: Vergessene Invalidierung zeigt veraltete Baumzeilen. Absicherung: Fallback-Revalidierung alle 30 Frames (wie `MultiSelectionCache`), UI-Test "Komponente im Details entfernen → Baum aktualisiert".

### EDT-1107 – Mehrfachauswahl: pro Frame Kopien, O(n·m)-Suchen und Collab-Lookups für alle Mitglieder
- **ID**: EDT-1107
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:250-251`, `257`, `351-357`, `121-139`, `316-321`
- **Problem**: `renderMultiSelection` ist ausdrücklich für Ctrl+A mit Tausenden Entities ausgelegt (Kommentar L141-149), hat aber pro Frame noch lineare bzw. quadratische Anteile: (a) `const std::vector<std::string> common = s_multi.common; … partial = s_multi.partial;` kopiert beide Listen jeden Frame; (b) `membersToSkip(...)` läuft jeden Frame über alle Mitglieder (`registry.all_of<Terrain>`, in Session zusätzlich `subjectFor` + `lockFor` pro Mitglied); (c) `targets` (L351-357) wird jeden Frame berechnet, obwohl es nur im Add-Component-Popup gebraucht wird, mit `std::find(skip…)` pro Mitglied – in einer Collab-Session ist `skip` nahezu die ganze Auswahl, also O(n²) (1000 Mitglieder → ~10⁶ Vergleiche/Frame); (d) beim strukturellen Entfernen `std::find(held…)` in einer Schleife über alle Mitglieder (selten, nur bei Klick).
- **Code-Auszug**:
```cpp
const std::vector<std::string> common  = s_multi.common;
const std::vector<std::string> partial = s_multi.partial;
...
for (Entity e : members)
	if (std::find(skip.begin(), skip.end(), e) == skip.end()) targets.push_back(e);
```
- **Lösung**: (a) `const auto&` statt Kopie (Vorsicht: `s_multi` wird in der Schleife über `stale` nur markiert, nicht neu aufgebaut → Referenz bleibt gültig; sonst `refreshMultiSelection` erst nach der Schleife). (b)+(c) `held` zusammen mit dem `MultiSelectionCache` ablegen (neu berechnen bei Revision-Wechsel/`stale`/Periode, in Session zusätzlich bei Lock-Änderung), `skip`/`held` als `std::unordered_set<Entity>` bzw. sortierter Vektor + `binary_search`; `targets` erst innerhalb von `if (ImGui::BeginPopup(...))` berechnen.
- **Erwarteter Effekt** (Schätzung): Bei 1000+ ausgewählten Entities in Session spürbarer (ms-Bereich); ohne Session nur die Kopien.
- **Risiko / Verifikation**: Lock-Wechsel durch Peers muss `held` ungültig machen, sonst werden Edits an inzwischen fremd-gesperrte Entities propagiert (Korrektheit!). Daher nur zusammen mit Lock-Revision cachen oder `held` weiterhin pro Frame, aber O(n) ohne Kopien. Test: `tests/test_*collab*` bzw. neuer UI-Test mit Mock-Collab.

### EDT-1108 – Material-Vorschau: bei animiertem Node-Preview pro Frame Graph-Kopie + Shader-Generierung + String-Swaps
- **ID**: EDT-1108
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:2385`, `2399-2464`, `2469-2519`
- **Problem**: `previewAnimated` (Material liest die Engine-Uhr) setzt `previewDirty` in jedem Frame. Läuft gleichzeitig ein Per-Node-Preview (`previewNodeId != 0`) oder fehlt dem Asset noch ein gebackener Shader (`noBakedShader`), so wird in jedem Frame: `HE::MaterialGraph pg = st.graph;` (Deep-Copy), Output-Verdrahtung neu aufgebaut, `HE::generateFragment(pg, loader)` ausgeführt, `customShaderFragGlsl`, `shaderParamData`, `graphTexturePaths`, `graphLayerNames` in `orig*` kopiert, überschrieben und zurückgeschoben (dann `matGlslUsesTime` zweimal über den GLSL-Text). Das Ergebnis ist nur vom Graphen und `previewNodeId` abhängig, nicht von der Uhr (die Uhr geht als Parameter `previewClock` in den Renderer). Der Material-Function-Zweig (L2469-2519) hat dasselbe Muster (Scratch-Graph + `generateFragment` bei jeder Neuzeichnung, inkl. Animation).
- **Code-Auszug**:
```cpp
if (st.previewAnimated) st.previewDirty = true;
...
HE::MaterialGraph pg = st.graph;
...
const HE::MatShaderGen pgen = outId ? HE::generateFragment(pg, loader) : HE::MatShaderGen{};
```
- **Lösung**: `MatShaderGen` für das Preview im State cachen (`st.previewGen`, `st.previewGenKey` = (Graph-Revision bzw. `undoPos`+`pushUndo`-Zähler, `previewNodeId`, `isFunction`)). Bei reiner Zeit-Animation nur `RenderMaterialPreview` aufrufen und `previewAnimated` aus gecachten `usesTime`-Flags nehmen (einmal pro neuem Shader berechnen). Das Swap in/out der Asset-Felder kann bei cached Gen entfallen, wenn der Renderer die Quelle als Parameter nimmt (API-Änderung, optional) – ohne die API-Änderung wenigstens `std::swap` statt Kopie+Move.
- **Erwarteter Effekt** (Schätzung): Pro Frame bei aktiver Zeit-Animation + Node-Preview ein Graph-Copy + Codegen weniger (abhängig von Graphgröße: ~0,1–several ms); gilt nur für den Fall "animiert und Node-Preview/ungebackenes Asset".
- **Risiko / Verifikation**: Cache-Schlüssel muss jede Graph-Änderung erfassen, auch Peer-Edits (`peerEdited`) und Live-Drag (`liveEdit`, ändert Positionen, nicht Shader – unkritisch). Test: Node-Preview an, Node verschieben/verbinden, Preview muss aktualisieren.

### EDT-1109 – Preview-Mesh-Picker: kein Zeilen-Clipping, pro Frame Lowercase/Konkatenation/`isLoaded` für alle Meshes
- **ID**: EDT-1109
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1746-1753`, `1780-1806`
- **Problem**: Die gecachte Liste (`s_engineMeshes/s_projectMeshes`) wird zwar nur beim Öffnen geladen, aber `section(...)` iteriert pro Frame über ALLE Einträge: `matches(m.label)` lowercased jedes Label (Kopie + `transform`), `ctx.contentManager->isLoaded(m.path)` (String-Hash) pro Eintrag, `m.label + "##" + m.path` baut je Zeile einen String, `formatBytes` einen weiteren. Nur ~20 Zeilen sind sichtbar. CLAUDE.md Abschnitt 4 fordert Row-Clipping für zeilenlastige Panels.
- **Code-Auszug**:
```cpp
for (const auto& m : list)
{
	if (!matches(m.label)) continue;
	const bool resident = ctx.contentManager && ctx.contentManager->isLoaded(m.path);
	if (HcEditorUtil::searchMenuItem(m.label + "##" + m.path))
	...
	const std::string size = formatBytes(m.bytes);
```
- **Lösung**: Beim Öffnen und bei Query-Änderung eine gefilterte Index-Liste (`std::vector<int>`) aufbauen (Label vorab in `labelLower` speichern); in `section` `ImGuiListClipper` über die gefilterte Liste verwenden und erst für sichtbare Zeilen `isLoaded`/Strings berechnen. Die Section-Header und Zeilenhöhe bleiben konstant (eine Zeile = `GetTextLineHeightWithSpacing`). Selbes Muster für den Texture-Picker aus EDT-1101.
- **Erwarteter Effekt** (Schätzung): Bei 5 000 Meshes ~10 000 Strings/Frame weniger, solange das Popup offen ist.
- **Risiko / Verifikation**: `searchMenuItem` hebt "bestes Match" hervor (Enter nimmt es) – das Clipping darf dessen Logik (`HcEditorUtil::searchMenuItem`) nicht aushebeln; vor Änderung `HcEditorUtil::searchMenuItem` lesen. UI-Test mit vielen Dummy-Meshes.

### EDT-1110 – Rekompilierung/Serialisierung beim Commit synchron im UI-Thread
- **ID**: EDT-1110
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:196-254`, `302-311`, `2704-2711`
- **Problem**: Jede abgeschlossene Bearbeitung (`structuralEdit || paramEdit || panelEdit || commentEdit`) ruft `applyToMaterial` → `regenerateMaterial` (volle `generateFragment`, `materialGraphToJson`, `syncMaterialInstancesOf`, Metal-Cross-Compile über `MaterialShaderLibrary`) und danach `pushUndo` (nochmals `materialGraphToJson` + String-Vergleich + Push eines Voll-Snapshots, bis 64 Stück) – alles synchron auf dem UI-Thread. Das JSON wird pro Commit zweimal erzeugt (`mat->nodeGraphJson` und Snapshot). `commentEdit` (Kommentarbox verschoben) löst dabei ebenfalls vollständige Shader-Regenerierung und Cross-Compile aus, obwohl der Kommentar den Shader nicht ändert (Kommentar im Code selbst: "persist (no shader change)").
- **Code-Auszug**:
```cpp
if ((structuralEdit || paramEdit || panelEdit || commentEdit) && assetOk && !st.isInstance)
{
	applyToMaterial(st, ctx);
	pushUndo(st);
```
- **Lösung**: (1) `commentEdit` ohne Shader-Regenerierung behandeln: nur `nodeGraphJson` aktualisieren + `dirty`, Undo-Push. (2) JSON einmal erzeugen und in `regenerateMaterial` sowie `pushUndo` wiederverwenden (z.B. `regenerateMaterial` gibt/übernimmt `std::string json`). (3) Cross-Compile-Check (`s_checkLib.fragment`) optional in einen Job auslagern (`JobSystem`) und das Ergebnis im nächsten Frame abholen. (1) und (2) sind rein lokal.
- **Erwarteter Effekt** (Schätzung): Komfort bei großen Materialien (Commit-Latenz); kein Per-Frame-Effekt.
- **Risiko / Verifikation**: (1) ändert Verhalten nur, wenn Kommentare das Shader-Ergebnis beeinflussen – laut Code nicht; prüfen mit Test, der `generateFragment` mit/ohne Kommentare vergleicht. (3) braucht Thread-Sicherheit der Shader-Library (siehe „Unsicher“).

### EDT-1111 – Material-Sektion: Mesh-Section-Tabelle und Strings pro Frame kopiert
- **ID**: EDT-1111
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:2056-2107`
- **Problem**: Solange die Material-Sektion offen ist, wird `slots = HE::meshSectionsOf(*sm)` (Vektor von `MeshSection` mit Strings) jedes Frame neu gebaut, dazu pro Slot `std::string emptyText` mit `substr`/Konkatenation. Der Kommentar begründet die Kopie ("the picker may load, and a load moves every mesh pointer") – die Begründung trägt aber nur für Zeiger auf das Asset, nicht für den Aufbau pro Frame.
- **Code-Auszug**:
```cpp
std::vector<MeshSection> slots;
...
slots = HE::meshSectionsOf(*sm);
...
std::string emptyText = "(mesh's own)";
```
- **Lösung**: Pro Entity gecachter Satz `{meshId, revision} → slots` (Cache im Panel mit der ContentManager-Revision oder `meshAssetId` + Anzahl Sections als Schlüssel) und `emptyText` nur erzeugen, wenn der Slot-Picker tatsächlich gezeichnet wird (er wird immer gezeichnet, aber Texte lassen sich als `char[96]` per `snprintf` ohne Heap bauen).
- **Erwarteter Effekt** (Schätzung): Einige Allokationen pro Frame; nur bei Meshes mit mehreren Slots.
- **Risiko / Verifikation**: Gering; Cache-Invalidierung bei Mesh-Reload (Asset-Pointer ändert sich, `meshAssetId` nicht) beachten.

### EDT-1112 – Komponenten-Header: String-Bau und Help-Key pro Header und Frame
- **ID**: EDT-1112
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:908`, `913-917`, `930`
- **Problem**: In `componentHeader` (läuft pro Komponente und Frame): `removeLabel = std::string("Remove ") + label;` (Heap-Allokation ab "Remove " + >8 Zeichen, z.B. "Animator State Machine"), `snprintf("Component/%s")` + `helpForKey(key)` (Hash-Lookup). Beides nur im (seltenen) Klick-/Hover-Fall nötig: `removeLabel` wird erst bei `removed` gelesen, und Hilfe nur bei Hover.
- **Code-Auszug**:
```cpp
removeLabel = std::string("Remove ") + label;
...
std::snprintf(key, sizeof(key), "Component/%s", label);
EditorWidgets::helpForKey(key);
```
- **Lösung**: `removeLabel` erst beim Entfernen bauen (der Aufrufer kennt `label` nicht, aber `componentHeader` kann `const char* removeLabelFor` speichern, das auf `label` zeigt, und der Aufrufer baut den String); bei EDT-1103 (Tabelle) entfällt das Problem ohnehin. Hilfe nur bei `ImGui::IsItemHovered()` (falls `helpForKey` das nicht intern tut – prüfen).
- **Erwarteter Effekt** (Schätzung): 1 Heap-Allokation und 1 Hash pro offener Komponente und Frame; vernachlässigbar, aber trivial zu beheben.
- **Risiko / Verifikation**: Gering. `editor_help_audit.py --check` muss weiter grün sein.

### EDT-1113 – Mehrfach pro Frame aufgebaute Hilfsobjekte: `CollisionLayerNames`, Spline-`Curve`, Audio-Attenuation
- **ID**: EDT-1113
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:441-457` + `2848-2849`, `3137-3138`; `2728-2732`; `3428-3447`
- **Problem**: (a) `CollisionLayerNames(ctx)` kopiert pro Aufruf 16 Layer-Namen (`cfg.layerName(i)` liefert `std::string` per Wert; bei leerem Namen `"Layer " + to_string(i)`) in neue Strings – bei Rigid Body UND Character Controller, pro Frame. (b) Der Spline-Abschnitt baut pro Frame `HE::spline::Curve(*spline).length()` (Kopie der Kontrollpunkte, Spans, ggf. Längentabellen; Implementierung `SplineCurve.cpp:60-85`) nur für eine Textzeile. (c) Audio Source: `setSoundAttenuation(...)` wird jeden Frame an die Audio-Engine gegeben, solange `a->handle != 0` (ob die Engine lockt: nicht geprüft), die Falloff-Kurve mit 64 `attenuationGain`-Aufrufen wird jedes Frame neu berechnet.
- **Code-Auszug**:
```cpp
names[i] = cfg.layerName(i);          // 441-457, 16 Strings pro Konstruktion
HE::spline::Curve(*spline).length()   // 2732
ctx.audioEngine->setSoundAttenuation(a->handle, a->attenuation, a->innerRange, a->range, a->rolloffFactor); // 3446
```
- **Lösung**: (a) Statischer Cache `{revision of collisionLayers, names[], items[]}` oder lazy: Namen erst bei geöffneter Combo bauen (`Row::combo` bekommt `const char*`-Array; Alternative `BeginCombo` + Selectable aus `cfg.layerName`). (b) Länge nur bei Änderung der Punktliste neu berechnen (Hash über `controlPoints`/`closed` oder `SplineComponent`-Dirty). (c) `setSoundAttenuation` nur aufrufen, wenn sich einer der vier Werte gegenüber dem letzten Aufruf geändert hat (Werte im Widget-Zweig `trackEdit`/`IsItemEdited` bündeln) – Verhalten bleibt bei Änderungen identisch.
- **Erwarteter Effekt** (Schätzung): Je ein paar Mikrosekunden; Summe klein.
- **Risiko / Verifikation**: (c) ändert das Verhalten, wenn die Audio-Engine das Attenuation-Modell sonst überschreibt (z.B. beim Neustart des Voice) – prüfen.

### EDT-1114 – Klassen-Tab: Input-Action-Scan alle 3 s auf dem UI-Thread, Datei-I/O je InputAction
- **ID**: EDT-1114
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:1828-1848`, `3071-3080`
- **Problem**: Für Player-Klassen (`isPlayer`) wird der Katalog `scanInputActions` alle 3 Sekunden komplett neu gebaut: `listAssets(InputAction)` (rekursiver Walk über Content + Engine-Content mit Header-Read jeder `.hasset`, siehe EDT-1101) und danach pro Action `HAsset::Reader::open` (liest alle Chunks!) + JSON-Sniff. Das passiert synchron im Draw des Tabs, solange der Tab sichtbar ist, und wiederholt sich unabhängig davon, ob sich etwas geändert hat.
- **Code-Auszug**:
```cpp
if (st.eventsScanTime < 0.0 || (isPlayer && now - st.eventsScanTime > 3.0))
{
	...
	st.inputActions = isPlayer ? scanInputActions(ctx.contentManager) : std::vector<InputActionRef>{};
```
- **Lösung**: Die Liste nur dann neu bauen, wenn sich der Content geändert hat: ContentManager/`EditorAssetTypeCache` hat bereits Invalidierung bei Refresh/Import/Löschen/Umbenennen (`EditorAssetTypeCache.h`, Kommentar in MaterialEditorPanel L1867-1869). Alternativ nur beim Fokus-Gewinn des Tabs und beim Öffnen des Add-Menüs scannen (Lazy Evaluation: die Liste wird nur im Add-Menü gebraucht, `drawAddMenu`). Die Typ-Sniffs (`IsAxis2D`) pro Datei im Cache nach Dateigröße+mtime ablegen.
- **Erwarteter Effekt** (Schätzung): Wegfall periodischer Frame-Spitzen (alle 3 s) im Tab; Größe hängt vom Projekt ab ("messen").
- **Risiko / Verifikation**: Neue InputActions müssen weiter im Add-Menü erscheinen: Test, in dem eine Action während offenem Tab angelegt wird.

### EDT-1115 – `LevelScriptPanel` – globaler Zustand `g` mit Kontext-Swap und 230-Zeilen-`drawGraphBody`
- **ID**: EDT-1115
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:126-190`, `1392-1620`, `540-553`, `1663-1722`
- **Problem**: Alle Helfer (`drawVariables`, `drawFunctions`, `drawNodeDetails`, …) greifen auf eine globale `LSState g` zu; ein Wechsel zwischen Level-Script/Game-Instance/Klassen-Tab wird durch `std::move` des gesamten Zustands in/aus `std::map<std::string, LSState>` realisiert. Der Kommentar rechtfertigt das mit "ein einziger Ort, an dem der Zustand gewählt wird", aber es ist ein verstecktes Singleton: Funktionen können nicht isoliert getestet werden, und `forget(...)` muss `g.graphFor` manuell vergleichen (L2801). `drawGraphBody` hat 14 Parameter (davon 5 mit Default) und wird in drei Rollen verwendet. Das Kommentarblock-Verhältnis (z.B. L1447-1471 für drei Zeilen Code) erschwert die Lesbarkeit; die Event-Listen (`kEvents`) stehen als `static const` in den `render`-Funktionen.
- **Code-Auszug**:
```cpp
std::map<std::string, LSState> s_graphStates;
LSState g;
...
if (!g.graphFor.empty()) s_graphStates[g.graphFor] = std::move(g);
```
- **Lösung**: Mittelfristig `LSState&` als Parameter bzw. ein `struct GraphFrame { HC::Graph& graph; LSState& g; Host& … }`, das `drawGraphBody` aufbaut; `s_graphStates[key]` liefert direkt eine stabile Referenz (std::map-Knoten sind stabil), kein Move nötig. Damit entfallen Swap und das Vergessen-Sonderfall. Zusätzlich die 14 Parameter in eine `struct GraphBodyDesc` bündeln.
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeit-Änderung.
- **Risiko / Verifikation**: Mittel – viele Aufrufstellen (>100 Zugriffe auf `g.`). Mechanisch ersetzbar; Tests: vorhandene HorizonCode-Panel-UI-Tests (`tests/test_*hc*`) vor/nach.

### EDT-1116 – `drawFunctions` O(F·N) und String-Bau pro Zeile/Frame in der Sidebar
- **ID**: EDT-1116
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/LevelScriptPanel.cpp:515-537`, `348-358`, `474-486`
- **Problem**: Für jede FunctionEntry-Node wird die gesamte Node-Liste erneut nach gleichnamigen Funktionen durchsucht (`same`), und alle Nodes jedes Frames mindestens einmal durchlaufen, nur um FunctionEntries zu finden. `label` wird je Funktion und Frame als `std::string` aus drei Teilen gebaut; `varRow` ruft `HcExtractUi::listNote(...)` (liefert `std::string`) pro Variable und Frame. Unclipped-Listen (Variablen, Events, Funktionen).
- **Code-Auszug**:
```cpp
for (const auto& n : graph.nodes) {
	if (n.type != NT::FunctionEntry) continue;
	int same = 0;
	for (const auto& o : graph.nodes) if (o.type == NT::FunctionEntry && o.s == n.s) ++same;
```
- **Lösung**: Pro Frame einmal die FunctionEntry-Nodes in einen `std::vector<const HC::Node*>` einsammeln (oder ein `std::unordered_map<std::string,int>` der Namenszähler), dann `same` per Lookup; Labels per `snprintf` in einen Stack-Puffer. Bei Graphen mit tausenden Nodes (Level-Script) ist der Durchlauf über alle Nodes ohnehin der Hauptanteil – dann Funktionsliste im `LSState` cachen und bei `edited` neu aufbauen.
- **Erwarteter Effekt** (Schätzung): Bei Graphen mit >1000 Nodes und >10 Funktionen merkbar; sonst klein.
- **Risiko / Verifikation**: Gering; Test mit Doppelnamen ("duplicate name!").

### EDT-1117 – Material-Add-Menü: zehn Registry-Durchläufe mit String-Konstruktion pro Frame
- **ID**: EDT-1117
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:1366-1370`, `1395-1433`, `1436-1462`
- **Problem**: Solange das Add-Node-Popup offen ist, ruft `emitCategory` für jede der 10 Kategorien einmal `matNodeRegistry()` ab und konstruiert pro Node zweimal `std::string(d.category)`/`std::string(lastCat)` zum Vergleichen, `matches()` ruft zweimal `lower()` (Kopie + `transform`) pro Node, dazu ein Tail-Pass mit weiteren `std::string(d.category)`-Vergleichen, und pro MaterialFunction `std::filesystem::path(fn->path).stem().string()` (+ ein `enumerateIds`). Etwa ~150 Nodes × 10 Kategorien.
- **Code-Auszug**:
```cpp
for (const auto& d : HE::matNodeRegistry())
{
	if (std::string(d.category) != cat || !listed(d)) continue;
	if (std::string(lastCat) != cat)
```
- **Lösung**: Einmalig (static) eine nach Kategorie gruppierte Index-Liste der Registry bauen (`std::vector<std::pair<const char*, std::vector<const MatNodeDesc*>>>`), Kategorienvergleich per `strcmp`; Kleinbuchstaben-Varianten der Namen/Kategorien vorberechnen. Funktionsnamen beim Popup-Öffnen (`IsWindowAppearing`) in einer Liste cachen.
- **Erwarteter Effekt** (Schätzung): ~3 000 kleine String-Konstruktionen pro Frame im offenen Menü entfallen.
- **Risiko / Verifikation**: Gering; Reihenfolge der Kategorien (`kCatOrder` + Tail) beibehalten.

### EDT-1118 – `trackEdit()` außerhalb von `if` bei Light-Range/Spot-Angle (irreführende Einrückung, doppelter `stashPre`/`commitPending`)
- **ID**: EDT-1118
- **Kategorie**: A / B
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:2574-2577`, `1276-1278`
- **Problem**: Die Zeilen
  `if (l->type != LightType::Directional) Row::dragFloat("Range", …); trackEdit();` und
  `if (l->type == LightType::Spot) Row::dragFloat("Spot Angle", …); trackEdit();`
  führen `trackEdit()` unabhängig von der Bedingung aus. Wird das Widget nicht gezeichnet, bezieht sich `IsItemActivated()`/`IsItemDeactivatedAfterEdit()` auf das zuletzt gezeichnete Item (Intensity bzw. Range) und ruft `stashPre`/`commitPending` ein zweites Mal auf; im besten Fall ein No-op, im ungünstigen Fall ein doppelter Undo-Eintrag oder überschriebener Pre-State (hängt von `EditorUndo::stashPre` ab: unsicher, nicht geprüft). Außerdem: im Weather-Zweig `if (registry.all_of<WeatherComponent>(entity)) { if (auto* w = registry.try_get<…>)` – doppelte Abfrage, die zweite Verzweigung ist immer wahr.
- **Code-Auszug**:
```cpp
if (l->type != LightType::Directional)
	Row::dragFloat("Range", &l->range, 0.1f, 0.0f, 10000.0f); trackEdit();
if (l->type == LightType::Spot)
	Row::dragFloat("Spot Angle", &l->spotAngle, 0.5f, 1.0f, 179.0f); trackEdit();
```
- **Lösung**: Klammern setzen: `if (…) { Row::dragFloat(…); trackEdit(); }`. Weather: `if (auto* w = registry.try_get<WeatherComponent>(entity)) { … }`, die äußere `all_of` entfällt.
- **Erwarteter Effekt**: Korrektheit/Lesbarkeit; keine Performance.
- **Risiko / Verifikation**: Gering; Undo-Test: Light Directional Intensity ziehen → genau ein Undo-Schritt.

### EDT-1119 – Material-Lösch-/Weitere Frame-Kosten: `drawParamConstPanel` und `drawInstanceOverridePanel` kopieren Gruppen-Strings und suchen linear
- **ID**: EDT-1119
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MaterialEditorPanel.cpp:910-923`, `2046-2067`, `2033-2037`
- **Problem**: `groupOf` liefert `std::string` per Wert; im Instanz-Panel wird `groupOf(i) != grp` für alle G×P Paare pro Frame mit je einer String-Kopie ausgewertet, `ovIndex(nm)` ist linear über die Overrides (zweimal pro Zeile). Bei 100 Parametern und 10 Gruppen sind das ~1 000 String-Kopien und ~10 000 String-Vergleiche pro Frame. Im Master-Panel (`drawParamConstPanel`) wird zusätzlich jedes Frame ein `std::vector<std::string> groupOrder` und zwei Zeigerlisten aufgebaut.
- **Code-Auszug**:
```cpp
auto groupOf = [&](size_t i) -> std::string
{ return i < inst.graphParamGroups.size() ? inst.graphParamGroups[i] : std::string(); };
...
if (groupOf(i) != grp) continue;
```
- **Lösung**: `groupOf` als `const std::string&` zurückgeben (statisches leeres `kEmpty`), `ovIndex` einmal in ein `std::vector<char> overridden(inst.graphParamNames.size())` vorberechnen (eine Schleife mit `unordered_set`), die Gruppen-Reihenfolge nur bei Änderung der Parameteranzahl neu bauen. Wegen der fehlenden Clip-Nutzung (alle Zeilen werden gezeichnet) ggf. `ImGuiListClipper` pro Gruppe, wenn Instanzen mit >200 Parametern vorkommen (unsicher).
- **Erwarteter Effekt** (Schätzung): Gering, nur bei vielen Parametern.
- **Risiko / Verifikation**: Gering.

### EDT-1120 – macOS-Menü: `setProjectLoaded` schreibt jeden Frame ~35 `NSMenuItem.enabled`; `setToggleState` linear
- **ID**: EDT-1120
- **Kategorie**: D / B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/MacMenuBar.mm:506-510`, `521-532`, `536-563`
- **Problem**: `setProjectLoaded` hat im Gegensatz zu `setAppProject`, `setToggleState` und `setItemTitle` keine Änderungsprüfung: Jeden Frame werden alle `s_projectItems` (rund 35) per ObjC-Setter beschrieben (`it.enabled = …`); AppKit markiert das Menü ggf. als neu zu zeichnen (der Kommentar in `setAppProject` nennt genau dieses Problem für `hidden`). `setToggleState` durchsucht pro Aufruf `s_toggleItems` (~45 Einträge) linear; bei ~45 Aufrufen pro Frame (EditorUI) sind das ~2 000 Vergleiche plus ObjC-Property-Reads (`t.item.state`).
- **Code-Auszug**:
```cpp
void setProjectLoaded(bool loaded)
{
	s_projectLoaded = loaded;   // a few dozen items; cheap enough to set every frame
	for (NSMenuItem* it : s_projectItems) it.enabled = loaded ? YES : NO;
}
```
- **Lösung**: `if (s_installed && loaded == s_projectLoaded) return;` – Achtung: `install()` ruft `setProjectLoaded(false)` bei initial `s_projectLoaded = false` und muss den ersten Durchlauf erzwingen (z.B. `std::optional<bool>` oder Flag `s_projectStateApplied`). Toggle-Lookup: `std::unordered_map<uint64_t, NSMenuItem*>` (Schlüssel `cmd<<32|arg`) oder gecachtes `bool s_lastState`-Feld im `ToggleItem`, um den ObjC-Read zu sparen (`if (t.last == on) return;`).
- **Erwarteter Effekt** (Schätzung): ~80 ObjC-Messages pro Frame weniger; nur macOS. Ob AppKit bei gleichem Wert wirklich neu zeichnet: nicht gemessen.
- **Risiko / Verifikation**: Gering; die Änderung muss berücksichtigen, dass der Menübaum außerhalb (Hub/Projektwechsel) nicht ohne Zutun zurückgesetzt wird. Manuell auf macOS prüfen.

### EDT-1121 – Schwere Header für kleine Abhängigkeiten
- **ID**: EDT-1121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/InputMappingModel.h:2`, `src/HE_Editor/LevelScriptPanel.h:2`, `src/HE_Editor/InspectorPanel.h:2`, `src/HE_Editor/MacMenuBar.h:3-4`
- **Problem**: `InputMappingModel.h` zieht `<Application/InputMapping.h>` (und damit `Input.h`, `<SDL3/SDL.h>`, `<unordered_map>`, `<algorithm>`) nur für das Enum `AxisSource` (Kommentar sagt: "free of ImGui", aber nicht von SDL). `LevelScriptPanel.h` inkludiert das volle `<imgui.h>` für `ImVec2` (Vorwärtsdeklaration geht nicht für Wertparameter; `const ImVec2&` genügt jedoch nicht ohne Typ – hier wäre `struct ImVec2;` ausreichend, da nur Referenzen verwendet werden). `InspectorPanel.h` zieht `HorizonScene/HorizonWorld.h` (entt-Header) für `Entity`/`HorizonWorld`. Jede Translation Unit, die diese Header inkludiert (Tests, MCP-Tools, EditorUI), bezahlt dafür.
- **Code-Auszug**:
```cpp
#include <Application/InputMapping.h>   // AxisSource
#include <imgui.h>        // LevelScriptPanel.h, nur für ImVec2&
```
- **Lösung**: `AxisSource` in einen kleinen Header `Application/InputAxisSource.h` auslagern (`enum class AxisSource : unsigned char` o.Ä.), `InputMapping.h` inkludiert ihn. `LevelScriptPanel.h`: `struct ImVec2;` statt `<imgui.h>`. `InspectorPanel.h`: `using Entity = entt::entity;` über schlankeren Header, falls vorhanden; sonst belassen.
- **Erwarteter Effekt** (Schätzung): Kürzere Kompilierzeiten, v.a. `tests/test_inputmapping.cpp`.
- **Risiko / Verifikation**: Gering; Build aller Targets (`he_tests`, `HorizonEditor`).

### EDT-1122 – Eingabeprüfung `inputMayEditThisFrame` iteriert alle ImGui-Tasten pro Frame
- **ID**: EDT-1122
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/InspectorPanel.cpp:88-105`
- **Problem**: Pro Frame in der Mehrfachauswahl werden alle `ImGuiKey_NamedKey_*` (~150 Tasten) per `IsKeyDown` abgefragt, bevor entschieden wird, ob serialisiert werden muss. Einfacher und billiger: `io.KeysData`-Scan ist nicht nötig, wenn `io.WantTextInput`, `io.InputQueueCharacters`, oder `ImGui::IsAnyItemActive()` schon Aktivität anzeigen; für das Panel genügt in der Praxis `ImGui::IsAnyItemActive() || mouse || key-event-this-frame` (`io.KeysDown`-Änderung via `io.InputQueueCharacters`/`ImGui::IsKeyPressed`).
- **Code-Auszug**:
```cpp
bool key = io.InputQueueCharacters.Size > 0;
for (int k = ImGuiKey_NamedKey_BEGIN; !key && k < ImGuiKey_NamedKey_END; ++k)
	key = ImGui::IsKeyDown(static_cast<ImGuiKey>(k));
```
- **Lösung**: Entweder `ImGui::IsAnyItemActive()` ergänzen und die Schleife nur ausführen, wenn kein Item aktiv ist (häufiger Fall: nichts passiert → Schleife läuft trotzdem ganz). Alternativ Schleife durch ImGuis `io.KeyMods != 0 || ImGui::IsKeyPressed(any)` ersetzen. Wirkung ohne Messung sehr klein – nur aufnehmen, wenn die Funktion am Frame-Profil auffällt.
- **Erwarteter Effekt**: Vernachlässigbar (<10 µs).
- **Risiko / Verifikation**: Tastaturbedienung der Slider (Cursortasten bei fokussiertem Widget) muss weiter propagieren: UI-Test mit Tastatur-Edit (Pfeile in `DragFloat` per Ctrl+Click-Eingabe).

## Cross-Modul-Hinweise

- `src/HE_Core/include/GraphCommon/GraphModel.h:44-64`: `findNodeById` ist linear, und wird von `MaterialGraph`, `ParticleGraph`, `HorizonCode::Graph` (und `GraphEditor`-Adaptern der anderen Editoren) pro Node und Frame benutzt. Ein Id→Index-Cache an zentraler Stelle (Rebuild bei Load/Undo/Mutation) würde EDT-1102 und die analogen Muster in `UIEditorPanel`/Particle-Panel gleichzeitig beheben (Slice-Auditoren für HE_Core/Editor-Graph-Panels informieren).
- `src/HE_Editor/HcEditorUtil.cpp:55-103` (`listAssets`/`scanAssetsInto`): Jeder Aufruf scannt Content- und Engine-Content-Baum rekursiv und liest Header; ein gemeinsamer, über `EditorAssetTypeCache`/Refresh invalidierter Index würde alle Picker (Mesh, Textur, Klassen, InputAction, Widget, Theme, Audio, Szenen: `HcEditorUtil.cpp:1850-1866`) beschleunigen. `listScenes` (Z.105-131) macht dasselbe ohne Extension-Filter vor dem Walk.
- `src/HE_Editor/GraphEditor.cpp:283-360`: Layout aller Nodes pro Frame ohne Sichtbarkeits-Culling (`d.pins = model.pins(nid)` für jede Node), ruft pro Node `model.title`, `headerColor` (siehe EDT-1102). Relevant für alle Graph-Panels.
- `src/HE_Editor/EditorUI.cpp`: Die Menüstruktur existiert doppelt (ImGui-Bar + `MacMenuBar.mm`, `Cmd`-Enum). Das ist im Header ausdrücklich als Wartungsfalle beschrieben (`MacMenuBar.h:15-20`). Eine gemeinsame Tabelle `{Menü, Label, Cmd, Shortcut, needsProject, gameOnly}` würde beide Pfade (ImGui + Cocoa) speisen und die Fehlerklasse "nur im ImGui-Menü vorhanden" beseitigen.
- `src/HE_Editor/EditorMultiEdit.*`: `EditorMultiEdit::state(world, entity)` serialisiert die komplette Entity (JSON) und wird pro Edit-Frame zweimal aufgerufen (`InspectorPanel.cpp:278, 326`); bei Verlinkung über viele Komponenten teuer. Auditor von `EditorMultiEdit.cpp` sollte prüfen, ob nur die betroffene Komponente (`only`) serialisiert werden kann.
- `src/HE_Core/src/Scripting/ScriptEngine.cpp:511-560` (`getScriptProperties`): Lua-Walk pro Aufruf, siehe EDT-1104; ein `ScriptEngine`-seitiger Cache pro Modul-Revision wäre die zentrale Lösung.
- `src/HE_Editor/InputMappingModel.cpp` (nicht in diesem Slice): `decodeMapping`/`encodeMapping`/`applyDetected` – im Header sind die Zeilenindizes `kAxisRowBaseY = 500` fest kodiert; ein Axis-Listenindex ≥ 500 würde fälschlich als Y-Liste gelesen (theoretisch, kein praktischer Fall). Auditor der .cpp prüfen lassen, ob eine Obergrenze erzwungen wird.

## Unsicher / Messung nötig

- EDT-1102: tatsächliche Frame-Kosten des quadratischen `findNode`-Musters bei 300/1 000 Nodes (Profiler-Panel); ggf. ist `GraphEditor::draw` selbst der Engpass.
- EDT-1101: Dauer des Walks mit warmem/kaltem Dateisystem-Cache bei einem 5 000-Asset-Projekt; wie oft `loadAsset` in der Script-Combo tatsächlich neu von der Platte liest (vermutlich nur beim ersten Öffnen, da residente Assets gecacht werden).
- EDT-1104: Verhalten von `ScriptEngine::loadScript` bei Syntaxfehler (wird der Fehler jedes Frame neu erzeugt und geloggt?) – nicht gelesen.
- EDT-1108: Wie oft `RenderMaterialPreview` bei `previewAnimated` Pipelines pro Frame neu bildet (Hash-Cache laut Kommentar) – nicht gemessen.
- EDT-1110: Thread-Sicherheit von `HE::MaterialShaderLibrary` (statische Instanz in `regenerateMaterial`), falls der Cross-Compile in einen Job verlegt wird.
- EDT-1113 (c): ob `AudioEngine::setSoundAttenuation` einen Mutex nimmt oder eine Voice neu parametriert.
- EDT-1113 (b): Kosten von `HE::spline::Curve` (Arc-Length-Tabelle) bei vielen Kontrollpunkten – `SplineCurve.cpp` nur ab Zeile 60-85 gelesen.
- EDT-1118: Auswirkung von zwei `trackEdit()`-Aufrufen auf dasselbe Item in `EditorUndo::stashPre/commitPending` (nicht gelesen).
- `LevelScriptPanel.cpp:2301-2326` `previewMeshBounds`: statischer Cache `s_cache` wird nie invalidiert (Mesh-Reload, Asset-Ersatz mit gleicher UUID, Projektwechsel) und wächst unbegrenzt; Speicher klein, aber veraltete Bounding Boxes möglich. Verhalten ändern ist trivial (Cache beim Projektwechsel/Content-Refresh leeren), aber ob es praktisch auftritt, nicht verifiziert.
- `MaterialEditorPanel.cpp:2651` (`InputTextMultiline` mit dem gesamten GLSL als `std::string`, ReadOnly): ImGui layoutet bei Multiline-InputText den gesamten Text pro Frame; bei 100 KB+ Shader ggf. messbar. Alternative wäre ein `ImGuiListClipper` über Zeilenoffsets. Keine Messung vorhanden.

## Gelesen

Ja, alle 10 Dateien des Slice wurden vollständig gelesen (`InputMappingModel.h`, `InspectorPanel.cpp/.h`, `InspectorPrefabKeys.cpp`, `LevelScriptPanel.cpp/.h`, `MacMenuBar.h/.mm`, `main.cpp`, `MaterialEditorPanel.cpp`); `main.cpp` ergab keinen Fund (22 Zeilen, einmalige Initialisierung). Zur Bestätigung einzelner Funde wurden nur gezielte Ausschnitte außerhalb des Slice gelesen (`HcEditorUtil.cpp` listAssets, `GraphEditor.cpp` Layout-Schleife, `GraphModel.h`, `ScriptEngine.cpp::getScriptProperties`, `CollisionLayers.cpp::layerName`, `SplineCurve.cpp` Konstruktor, `InputMapping.h` Includes).
