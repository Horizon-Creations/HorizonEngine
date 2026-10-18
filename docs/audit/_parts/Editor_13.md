# Editor Slice 13 – McpToolsInput.cpp … McpToolsTerrain.cpp

## Zusammenfassung

Der Slice (10 Dateien, 10 464 Zeilen) sind die MCP-Tool-Handler fuer Input, Material, Particle, Prefab, Scene, Screenshot, Sequence, Settings, TaskBoard und Terrain. Nichts davon laeuft pro Frame im Draw-Pfad; jeder Handler ist ein einmaliger Aufruf eines externen Clients. Die Schwere ist deshalb durchgehend begrenzt: 0 Hoch, 9 Mittel, 13 Niedrig. Die Dateien sind sorgfaeltig kommentiert und konsistent aufgebaut. Die groesste Pflegelast ist Duplikation: dieselben Gate-Ketten (Play/Lock/Dirty/Engine), dieselben kleinen Helfer (`uuidJson`, `vec3Arg`, `failFor`, `lowered`) und dieselben Validierungsbloecke stehen in mehreren Dateien. Die groesste Laufzeit steckt in den Terrain-Tools (Komplettkopie plus CBOR/JSON/Base64-Rundlauf pro Brush-Dab) und in `commitGraph` (doppelte Codegen pro Material-Graph-Edit). Keine Messdaten vorhanden; Laufzeitschaetzungen sind gekennzeichnet.

## Funde

### EDT-1301 – Terrain-Brush: Komplettkopie und Serialisierungs-Rundlauf pro Dab
- **ID**: EDT-1301
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:506-512, 549-555, 199-227` (sculpt); `:682-687, 725-731` (mountain); `:809-816, 866, 874-882` (paint)
- **Problem**: Jeder `terrain_sculpt`/`terrain_mountain`/`terrain_paint`-Aufruf kopiert das gesamte `TerrainComponent` (Hoehenfeld bis `kTerrainMaxResolution`², Weightmaps, Wasser). Danach serialisiert `componentsOf` die komplette Entity-Komponente nach CBOR und parst sie nach JSON (inklusive Dekodierung der Base64-Blobs der alten Daten). Die neuen Daten werden wieder Base64-kodiert, `Command::setComponents` serialisiert erneut nach CBOR, und das Gateway laedt sie ueber den Scene-Loader (wieder Base64-Dekodierung); fuer Undo kommt vermutlich noch ein Vorher-Zustand dazu. `terrain_paint` haelt zusaetzlich zwei Vorher-Kopien der Weightmaps (`before`, `before2`) nur fuer den Vergleich. Ein Client, der ein Gelaende mit 100 Dabs formt, zahlt das 100-mal; bei 1025² Hoehen sind das ~4 MB pro Kopie.
- **Code-Auszug**:
```cpp
TerrainComponent work = *r.tc;                       // 506
...
json terrain = componentsOf(*r.world, r.entity)["terrain"];   // 549
terrain["sculptHeightsB64"] = SceneSerializer::encodeBase64(...);  // 553
...
const std::vector<std::uint8_t> before  = work.layerWeights;  // 815
const std::vector<std::uint8_t> before2 = work.layerWeights2; // 816
```
- **Lösung**: (1) Statt `componentsOf` des ganzen Entity-JSON nur das `terrain`-Patch-Objekt mit den tatsaechlich geaenderten Feldern (`sculptHeightsB64` + `resolution`, bzw. Weightmaps) an `setComponents` geben, falls der Gateway Teil-Patches merged (zu pruefen, siehe Risiko). (2) `TerrainPaint::paint` / `TerrainSculpt::apply` melden bereits `changed`; fuer Paint ein `changedTexels` im Ergebnis ergaenzen, dann entfallen die beiden Vorher-Kopien. (3) Optional mehrere Dabs pro Aufruf (`dabs: [...]`) akzeptieren, damit der Rundlauf einmal pro Stroke statt einmal pro Dab anfaellt.
- **Erwarteter Effekt**: Schaetzung: Faktor 2-4 weniger Kopien/Base64-Arbeit pro Dab; (3) senkt die Kosten linear mit der Dab-Anzahl. Messen mit einer 1025²-Landschaft und 100 Aufrufen.
- **Risiko / Verifikation**: Das Verhalten (ein Undo-Schritt pro Aufruf, `changed`-Zaehlung, Carry-over der Runtime-Felder) muss exakt bleiben. Pruefen, wie `Command::setComponents` Teil-Patches behandelt (`replace` vs. `merge`), sonst gehen Felder verloren. Bestehende Terrain-MCP-Tests unter `tests/` (per `grep -l terrain_sculpt tests/`) als Absicherung; Test fuer "Undo stellt Hoehenfeld bytegenau wieder her" ergaenzen.

### EDT-1302 – `terrain_info` berechnet das komplette Hoehenfeld nur fuer Min/Max
- **ID**: EDT-1302
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:274-289`
- **Problem**: Pro Landschaft und pro Aufruf wird `computeTerrainHeightField(tc)` ueber das ganze Raster ausgewertet (bei nicht gesculpteter Landschaft fBm mit `octaves` Schichten pro Vertex) und nur `min_element`/`max_element` daraus gelesen. Ohne `uuid` laeuft das fuer alle Landschaften der Szene. Ein Client pollt `terrain_info` typischerweise vor und nach jeder Aenderung.
- **Code-Auszug**:
```cpp
const std::vector<float> field = computeTerrainHeightField(tc);
float mn = 0.0f, mx = 0.0f;
if (!field.empty())
{
    mn = *std::min_element(field.begin(), field.end());
    mx = *std::max_element(field.begin(), field.end());
}
```
- **Lösung**: Min/Max in einem Durchlauf (`std::minmax_element`) ist nur Kosmetik. Wirksam: bei `sculptHeights.size() == res*res` Min/Max direkt ueber `sculptHeights` ohne Neuberechnung; fuer die rein prozedurale Landschaft das Ergebnis in `TerrainComponent` cachen (Invalidierung bei Parameteraenderung) oder `heightRange` als optional markieren (`includeRange: true`).
- **Erwarteter Effekt**: Schaetzung: bei 1025² und 8 Oktaven mehrere Mio. Noise-Auswertungen pro Landschaft und Aufruf; mit Cache/Abkuerzung O(1) bzw. ein linearer Pass. Messen.
- **Risiko / Verifikation**: Ergebnis muss identisch bleiben: gleiche Quelle wie die Chunk-Erzeugung (`computeTerrainHeightField`). Test: `heightRange` fuer sculpted und prozedural vor/nach der Aenderung vergleichen.

### EDT-1303 – `commitGraph`: Shader-Codegen laeuft zweimal pro Graph-Edit
- **ID**: EDT-1303
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1414-1449` (Aufrufer: add_node, remove_node, connect, disconnect, set_pin_default, set_node)
- **Problem**: Der "Trockenlauf" ruft `HE::generateFragment` auf und verwirft das Ergebnis (nur `gen.glsl.empty()` wird geprueft). Danach schreibt der Code `materialGraphToJson(g)` in das Asset, und `regenerateMaterialFromGraph` (`ContentManager.cpp:790-818`) parst diesen JSON erneut und ruft `generateFragment` ein zweites Mal (inklusive der Funktions-Loader-Aufrufe). Jeder einzelne Verbindungs-/Knoten-Edit eines Clients zahlt zweimal Codegen plus Graph-Serialisierung/Parsen. Ein Client, der ein Material mit 30 Knoten und 40 Links aufbaut, ruft das ~70-mal.
- **Code-Auszug**:
```cpp
const HE::MatShaderGen gen = HE::generateFragment(g, loader, nullptr);
if (gen.glsl.empty()) return ToolResult::fail(...);
...
cm.regenerateMaterialFromGraph(m.id);   // 1441: generateFragment erneut
```
- **Lösung**: `ContentManager::regenerateMaterialFromGraph` liefert heute `void` und bricht intern bei leerem GLSL still ab. Variante A: Rueckgabewert `bool` (false = Codegen leer, Asset unveraendert) und den Trockenlauf streichen; da die Funktion bei leerem GLSL vor jeder Schreibaktion zurueckkehrt, bleibt "Refusal ist No-op" erhalten, solange `nodeGraphJson` erst bei `true` gesetzt wird (Reihenfolge in der ContentManager-Funktion anpassen oder den Graph-JSON dort uebergeben). Variante B: Overload `regenerateMaterialFromGraph(id, const MatShaderGen&)`, der das vorberechnete `gen` anwendet.
- **Erwarteter Effekt**: Schaetzung: halbe Codegen-Zeit pro Graph-Edit; dazu entfaellt ein JSON-Parse. Messen mit einem grossen Master-Material.
- **Risiko / Verifikation**: `regenerateMaterialFromGraph` wird auch beim Laden aufgerufen (Cross-Modul, `HE_Core`); Signaturaenderung muss kompatibel bleiben (Rueckgabewert ignorierbar). Tests: `tests/` mit `material_add_node`/`material_connect` (Grep nach den Tool-Namen) und ein Test, der einen Graph ohne Output-Node (Codegen leer) uebergibt und prueft, dass Datei und Asset unveraendert bleiben.

### EDT-1304 – `material_add_node` und `material_set_node` duplizieren ~90 Zeilen Validierung
- **ID**: EDT-1304
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1763-1843` (add) und `:2767-2888` (set)
- **Problem**: Beide Handler pruefen `s` (FunctionCall-Pfad, Textur-Pfad), `p` (Array 1-4 Zahlen, `paramCount == 0`), `min/max` (nur ParamFloat, beide, `min < max`), `group/tooltip` (nur Param-Knoten) und `position` ([x, y]) mit praktisch identischem Code und Meldungen. Aenderungen (z. B. neue Knotenart mit eigenem `s`-Format) muessen an zwei Stellen erfolgen; der Beleg dafuer ist, dass die Fehlertexte schon leicht auseinanderlaufen ("wired with material_connect" vs. "... or material_set_pin_default", `set_node` kennt zusaetzlich die Output-Sonderfaelle).
- **Code-Auszug**:
```cpp
if (nt == HE::MatNodeType::FunctionCall) { ... checkPath(*cm, s, true, "s") ... }   // 1765
if (HE::matNodeSamplesTexture(nt) && !s.empty()) { ... }                           // 1780
if (haveS && nt == HE::MatNodeType::FunctionCall) { ... }                          // 2769 (Kopie)
if (haveS && HE::matNodeSamplesTexture(nt) && !s.empty()) { ... }                  // 2784 (Kopie)
```
- **Lösung**: Eine `struct NodeEditArgs { bool haveS; std::string s; std::vector<float> p; ...; }` plus `bool parseNodeEditArgs(ContentManager&, const json&, HE::MatNodeType, ToolResult& failure, NodeEditArgs& out)`. Add und Set rufen sie auf; nur die Unterschiede (Output-Regeln, `type`-Pruefung, Default-Position) bleiben im jeweiligen Handler. Zusaetzlich `nodeIdArg` (2146) auch fuer `id` in remove/set verwenden (1923-1927, 2718-2722) und die doppelte "Id ist eine Comment-Box"-Schleife (1931-1935, 2726-2730) in `findNodeOrFail` ziehen.
- **Erwarteter Effekt**: ca. -90 Zeilen, eine Wahrheit fuer Argumentformen (Schaetzung).
- **Risiko / Verifikation**: Fehlertext und Fehlercode muessen identisch bleiben (Clients parsen Codes). Bestehende MCP-Material-Tests laufen lassen; Snapshot der Fehlermeldungen vorher/nachher diffen.

### EDT-1305 – Terrain: Brush-Rahmen viermal kopiert, `terrain_lake`-Handler mit 180 Zeilen und fuenf Aktionen
- **ID**: EDT-1305
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:506-512 / 557-568`, `:682-687 / 733-742`, `:809-813 / 884-894`, `:984-989 / 1123-1131`; Lake-Handler `:957-1138`
- **Problem**: Jedes Mutations-Tool wiederholt (a) die "Work-Kopie + keepWeightmap/keepBuiltRes/keepBuiltCps/wasDirty/dirty=false"-Einleitung und (b) das Fuellen von `CarryOver` plus `writeBack`. Jede neue Runtime-Eigenschaft von `TerrainComponent`, die erhalten bleiben muss, muss viermal nachgezogen werden. Zusaetzlich ist `terrain_lake` ein einziger Handler, der per String-Vergleich `action == "create"/"reshape"/"dig"/"level"` verzweigt und die Parameter jeweils selbst liest (Flag-Parameter-Muster).
- **Code-Auszug**:
```cpp
TerrainComponent work = *r.tc;
const HE::UUID      keepWeightmap = work.weightmapTextureId;
const std::uint32_t keepBuiltRes  = work.builtRes;
const std::uint32_t keepBuiltCps  = work.builtChunksPerSide;
const bool          wasDirty      = work.dirty;
work.dirty = false;
```
- **Lösung**: `struct TerrainEdit { TerrainComponent work; CarryOver keep; bool wasDirty; }` mit `TerrainEdit begin(const Terrain&)` und `ToolResult commit(EditorCommands&, Terrain&, TerrainEdit&, json patch, bool weightsDirty, const McpTerrainHooks&)`, das `fullRebuild`/`regionDirty`/Dirty-Rechteck aus `work` ableitet. Den Lake-Handler in `lakeCreate/lakeReshape/lakeDig/lakeLevel/lakeRemove` aufteilen (je ~25 Zeilen) und nur Plan/Validierung gemeinsam halten.
- **Erwarteter Effekt**: ca. -60 Zeilen im Rahmen, Handler < 60 Zeilen (Schaetzung).
- **Risiko / Verifikation**: `terrain_paint` setzt `weightsDirty = true`, `regionDirty = false` und lässt `work.dirty` nicht zurueck (kein `work.dirty = false` in Zeile 809-813) - dieser Unterschied muss im Helfer erhalten bleiben. Terrain-MCP-Tests und Undo-Test vor/nach.

### EDT-1306 – `sequence_info`/`sequence_write` bauen pro Aufruf einen Index ueber alle Clips und Sounds des Projekts
- **ID**: EDT-1306
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsSequence.cpp:137-152, 157-165`
- **Problem**: Sobald die Sequenz auch nur einen Asset-Verweis enthaelt, laeuft `assetPaths`: `walkContentAssets` (rekursiver Dateibaum, Sortierung, Header-Sniff aller `.hasset`, Limit 5000) und dann pro Treffer `AssetRefs::assetUuidOfFile` (Datei oeffnen). Das passiert bei jedem `sequence_info` und jedem `sequence_write` (via `stateJson`), obwohl meist nur wenige UUIDs aufzuloesen sind. `ContentManager::pathForId(UUID)` existiert (ContentManager.h:73) und liefert genau diese Rueckrichtung als reinen Lookup ueber die Disk-Registry.
- **Code-Auszug**:
```cpp
for (const ContentAsset& a : walkContentAssets(
         content, { HE::AssetType::AnimationClip, HE::AssetType::Audio }, 5000, truncated))
{
    const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);
    if (id != HE::UUID{}) out[id] = a.rel;
}
```
- **Lösung**: `refs` (bereits per `sequenceAssetRefs` gesammelt) einzeln mit `content.pathForId(id)` aufloesen und `unresolved` fuellen, wenn leer; `assetPaths` und `PathIndex` entfallen. Zusaetzliche Typpruefung (Clip/Audio) nur falls noetig ueber `EditorAssetTypeCache::assetTypeOf`.
- **Erwarteter Effekt**: O(#Verweise) statt O(#Assets im Projekt) pro Aufruf; Schaetzung: bei Projekten mit mehreren tausend Assets Sekundenbruchteile bis Sekunden gespart. Messen.
- **Risiko / Verifikation**: Pruefen, ob die Disk-Registry (`m_diskRegistry`) aktuell ist, wenn Dateien gerade neu erzeugt wurden und ob Engine-Content ("Engine/...") identisch aufgeloest wird; sonst waere `unresolvedAssets` ein Verhaltenswechsel. Test: Sequenz mit Clip aus Projekt und aus Engine-Content, frisch kopierte Datei ohne Refresh.

### EDT-1307 – Gate-Kette (Engine/Play/Lock/Dirty) und `no_project`-Pruefung in jedem Tool-Modul neu geschrieben
- **ID**: EDT-1307
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolsInput.cpp:248-275` (openDoc), `McpToolsMaterial.cpp:268-297` (openMat), `McpToolsParticle.cpp:169-197` (openEmitter), `McpToolsSequence.cpp:539-564` (openSequence); `no_project` in `McpToolsMaterial.cpp:1016, 1507, 1742, 1916, 2191, 2312, 2493, 2711, 3567` (9 Handler, wortgleicher Text)
- **Problem**: Vier Module implementieren dieselbe Abfolge `p.engine → failEngineReadOnly; isPlaying → play_mode; lockedByOther → locked_by_other; isDirty → dirty` mit je eigenen, nur im Satzbau abweichenden Meldungen, jeweils mit einem eigenen Hook-Struct (`McpInputHooks`, `McpMaterialHooks`, `McpParticleHooks`, `McpSequenceHooks`, ...), die dieselben vier `std::function`-Felder tragen. Ein neues Gate (z. B. "Asset ist schreibgeschuetzt durch Source Control") muesste an allen Stellen ergaenzt werden. Das Material-Modul wiederholt ausserdem 9-mal die `contentRoot().empty()` → `no_project`-Abfrage.
- **Code-Auszug**:
```cpp
if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
if (h.isPlaying && h.isPlaying()) { d.failure = ToolResult::fail("play_mode", ...); return d; }
if (h.lockedByOther && h.lockedByOther(p.rel)) { ... "locked_by_other" ... }
if (d.openDirty) { ... "dirty" ... }
```
- **Lösung**: In `McpToolCommon` eine `struct McpWriteGate { isPlaying, lockedByOther, isDirty, ... }` (die Hook-Structs erben oder enthalten sie) und `ToolResult checkWriteGate(const McpWriteGate&, const PathCheck&, const char* tabName, const char* playWhy)`; die kontextspezifische Begruendung (warum Play-Mode hier zaehlt, welcher Tab) bleibt als Parameter. Fuer `no_project` eine `requireProject(ContentManager&, const char* what)`-Funktion, die auch in `openMat`/`openGraphForEdit` aufgerufen wird.
- **Erwarteter Effekt**: ca. -120 Zeilen ueber den Slice (Schaetzung); einheitliche Codes/Texte.
- **Risiko / Verifikation**: Texte sind Teil der Client-Schnittstelle (das Modell liest sie). Parametrisierte Begruendungen muessen die heutigen Saetze erhalten; Tests, die `"play_mode"`/`"dirty"` pruefen, laufen lassen (Grep nach den Codes in `tests/`). Die Hook-Structs liegen in `McpToolRegistry.h` (ausserhalb des Slice).

### EDT-1308 – Kleine Helfer in bis zu fuenf Dateien kopiert
- **ID**: EDT-1308
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `uuidJson`: `McpToolsMaterial.cpp:97`, `McpToolsParticle.cpp:236`, `McpToolsPrefab.cpp:113` (ausserdem `McpToolsAnimator.cpp:79`, `McpToolsAsset.cpp:78`); `vec3Arg`/`vec3Prop`: `McpToolsScreenshot.cpp:70-92`, `McpToolsPrefab.cpp:126-148`; `vec3Json`: `McpToolsScreenshot.cpp:94`, `McpToolsTerrain.cpp:177`; `failFor(CmdError, ...)`: `McpToolsPrefab.cpp:70-110`, `McpToolsTerrain.cpp:59-98`; `failNoProject`: `McpToolsSettings.cpp:23`, `McpToolsTaskBoard.cpp:112`; `lowered`: `McpToolsMaterial.cpp:1529`, `McpToolsSettings.cpp:43`; Entity-UUID als Hex: `McpToolsSequence.cpp:58-65` und `EditorCommands.cpp:406-417`; Link-JSON: `linkJson` (`McpToolsMaterial.cpp:2012`), `linkJsonOf` (`:2627`) plus Inline-Kopien `:1249`, `:1956`; `readPayload`: `McpToolsInput.cpp:118`, `McpToolsParticle.cpp:123` (ausserdem Animator, Type).
- **Problem**: Dieselben Funktionen mit gleichem Verhalten (Prefab-Kommentar 119-125 begruendet die Kopie ausdruecklich mit "nur acht Zeilen"; ueber den ganzen Editor sind es aber Dutzende). `uuidJson` weicht schon ab (Particle castet nach `uint64_t`, die anderen nicht): bei Zahlen > 2^53 koennte ein Client unterschiedliche Darstellungen sehen (unsicher, wie `json` `uint64` ausgibt - pruefen). In Material existieren `linkJson` und `linkJsonOf` im selben File mit identischem Koerper.
- **Code-Auszug**:
```cpp
json linkJson(const HE::MatGraphLink& l)   { return json{ {"srcNode", l.srcNode}, ... }; }   // 2012
json linkJsonOf(const HE::MatGraphLink& l) { return json{ {"srcNode", l.srcNode}, ... }; }   // 2627
```
- **Lösung**: `uuidJson`, `vec3Arg`, `vec3Prop`, `vec3Json`, `failNoProject`, `lowered` nach `McpToolCommon.h/.cpp` (dort liegen schon `strArg`, `intArg`). Die uuid-Hex-Funktionen (`uuidHex`/`uuidFromHex`) existieren bereits als `HE::uuidToHex`/`HE::uuidFromHex` (TaskBoard nutzt sie, `McpToolsTaskBoard.cpp:35, 128`) - `uuidOf` und Sequence darauf umstellen. Der Material-Duplikat `linkJsonOf` wird gestrichen. `failFor` als gemeinsame Funktion mit einer kleinen Tabelle (`CmdError` → Satzvorlage) und familienspezifischem `what`/Zusatzsatz.
- **Erwarteter Effekt**: ca. -150 Zeilen, ein Ort fuer Fixes (Schaetzung).
- **Risiko / Verifikation**: `uuidJson`-Formatunterschied zuerst klaeren; `HE::uuidToHex` muss dieselbe 32-stellige Kleinbuchstaben-Form liefern (`%016llx%016llx`) - gegen `uuidOf` testen. Kleine Unit-Tests auf `McpToolCommon`.

### EDT-1309 – Katalog-Listen oeffnen jede Datei zweimal bzw. parsen den vollen Inhalt nur fuer Zaehler
- **ID**: EDT-1309
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolsInput.cpp:550-576` (input_actions), `:737-751` (input_mappings); `McpToolsParticle.cpp:399-419` (particle_info); `McpToolsSequence.cpp:589-612` (sequence_info); `McpToolsPrefab.cpp:317-329` (prefab_info); gemeinsame Walk-Funktion `McpToolCommon.cpp:197-237` (ausserhalb des Slice)
- **Problem**: `walkContentAssets` durchlaeuft bei jedem Aufruf den gesamten Content-Baum (`recursive_directory_iterator`, Sortierung, `EditorAssetTypeCache::assetTypeOf` fuer jede `.hasset`, bis das Limit der gewuenschten Typen erreicht ist). Danach oeffnet jeder Katalog-Handler jedes Treffer-File erneut (`readPayload`/`HAsset::Reader`/`assetUuidOfFile`) und parst bei Particle und Sequence den vollstaendigen Graph/Dokument-JSON, nur um `nodeCount/linkCount` bzw. `bindingCount/trackCount` auszugeben. Bei 200 Katalogeintraegen sind das 200 Datei-Opens und 200 JSON-Parses, auf dem Thread, der die MCP-Anfrage bedient (je nach Bridge: UI-Thread? unsicher, siehe unten).
- **Code-Auszug**:
```cpp
for (const ContentAsset& a : found)
{
    std::string payload;
    ParticleGraph g = ParticleGraph::makeDefault();
    if (readPayload(a.abs, payload) && !payload.empty())
    { ParticleGraph parsed; if (particleGraphFromJson(payload, parsed)) g = std::move(parsed); }
    list.push_back(json{ { "path", a.rel }, { "nodeCount", ... }, { "linkCount", ... } });
}
```
- **Lösung**: (1) `ContentAsset` um die beim Sniff ohnehin gelesene UUID erweitern (Prefab/Sequence brauchen `assetUuidOfFile` nur dafuer). (2) Fuer die Zaehler entweder `limit` kleiner defaulten (z. B. 50) oder die Zaehlung als Opt-in (`counts: true`) anbieten; ansonsten sind Pfad + Typ guenstig. (3) In `walkContentAssets` den Verzeichnis-Scan mit `ContentManager::contentChangeCounter` (laut `ContentManager.h` "bewegt sich, wenn Dateien erscheinen koennen") cachen.
- **Erwarteter Effekt**: Schaetzung: Katalogaufrufe von O(Dateien im Projekt) auf O(Treffer) bzw. O(1) bei Cache-Treffer. Messen mit einem Projekt mit >5000 Assets.
- **Risiko / Verifikation**: Cache-Invalidierung (externe Dateiaenderungen, Collab-Pull) muss stimmen, sonst zeigen Kataloge veraltete Listen. Ob die Bridge die Handler auf dem UI-Thread ausfuehrt, ist in diesem Slice nicht sichtbar (`McpBridge`/`McpToolRegistry` pruefen).

### EDT-1310 – Textur-Knotenlisten und der 80-Faelle-`nodeTypeName`-Switch
- **ID**: EDT-1310
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:813-821` (nodeDetail), `:957-964` (nodeJson), `:1198-1206` (graph_info), `:1597-1607` (node_types), Gegenbeispiel `:1780`/`:2784` (`HE::matNodeSamplesTexture(nt)`); Switch `:619-705`
- **Problem**: Die acht Textur-Sample-Typen sind viermal von Hand als `case`-Liste aufgezaehlt, obwohl `HE::matNodeSamplesTexture` schon existiert und in add/set_node benutzt wird. Ein neuer Sample-Knoten muss hier an vier Stellen und im Registry-Sweep-Test ergaenzt werden. `nodeTypeName` ist eine zweite, manuell gepflegte 80-zeilige Namenstabelle neben `HE::matNodeRegistry()` (Kommentar 617-618 nennt den Grund: der Test faengt Vergessenes ab).
- **Code-Auszug**:
```cpp
case HE::MatNodeType::TextureSample:
case HE::MatNodeType::NormalMapSample:
case HE::MatNodeType::TextureArraySample:
...                                         // 4x kopiert
```
- **Lösung**: Statt der Case-Listen `if (HE::matNodeSamplesTexture(n.type))` (in `nodeDetail` und `nodeJson` direkt; in `node_types` bleibt der Bomb-Unterschied, dort `isBombSample(type)` ergaenzen). Den Enum-Namen langfristig als Feld in `MatNodeDesc` fuehren (Cross-Modul) und `nodeTypeName` streichen.
- **Erwarteter Effekt**: ca. -50 Zeilen, ein Ort fuer neue Knoten (Schaetzung).
- **Risiko / Verifikation**: Pruefen, dass `matNodeSamplesTexture` genau diese acht Typen deckt (Definition lesen, ausserhalb des Slice). Registry-Sweep-Test (`tests/test_material_graph.cpp`, nur per Grep lesen) bleibt als Absicherung.

### EDT-1311 – Particle: Output-Knoten-Suche viermal inline, obwohl `outputNode()` existiert; Hinweis wird ueberschrieben
- **ID**: EDT-1311
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsParticle.cpp:241-246` (outputNode), Kopien `:542-543`, `:575-577`, `:634-636`, `:714-716`; Hinweis `:475-484`
- **Problem**: `outputNode(const ParticleGraph&)` ist nur als const-Variante vorhanden; die mutierenden Handler suchen den Knoten per Hand (viermal dieselbe Schleife, in `particle_set` sogar dreimal hintereinander, obwohl `reset`/`set` den Output-Knoten nicht entfernen). Zusaetzlich: in `particle_info` wird `result["note"]` fuer "kein Emitter Output" gesetzt und danach bei `randomised` ohne Pruefung ueberschrieben (Zeilen 475 und 482), sodass die wichtigere Warnung verloren geht.
- **Code-Auszug**:
```cpp
ParticleGraphNode* out = nullptr;
for (ParticleGraphNode& n : e.graph.nodes)
    if (n.type == ParticleNodeType::EmitterOutput) { out = &n; break; }
```
- **Lösung**: Mutierende Overload `ParticleGraphNode* outputNode(ParticleGraph&)`; in `particle_set` den Pointer einmal holen und nach `removeNode`/`addNode` ueber `outId` erneut mit `findNode` aufloesen (Pointer koennen nach `addNode` ungueltig sein - deshalb sind die Neu-Suchen vermutlich so entstanden; `findNode(outId)` ist O(n) und genauso einfach). Beide Hinweise anhaengen (`note += ...`).
- **Erwarteter Effekt**: -12 Zeilen, ein Hinweis weniger verloren.
- **Risiko / Verifikation**: Verhalten identisch bis auf den zusaetzlich erhaltenen Hinweis; Test: Graph ohne EmitterOutput und mit RandomRange liefert beide Hinweise.

### EDT-1312 – Material: `Mat` kopiert 15 Vektoren pro Aufruf, `set_param` kopiert danach zweimal alles, um einen Wert zu lesen
- **ID**: EDT-1312
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:134-211, 3163-3164, 3293-3298`
- **Problem**: `openMat` kopiert das gesamte Asset (Graph-JSON, Param-Namen, Typen, Min/Max, Daten, Gruppen, Tooltips, Overrides, Switches, Texturen, Layer) in `Mat`; die Begruendung (dichter Pool wird beim naechsten Load bewegt) ist korrekt. `material_set_param` kopiert aber danach `Mat fresh = m;` (kopiert alles erneut) und `copyOut(fresh, *a2)` (ueberschreibt alles), nur um `paramJson(...)["value"]` fuer einen Slot zu erzeugen.
- **Code-Auszug**:
```cpp
Mat fresh = m;
if (const MaterialAsset* a2 = cm->getMaterial(m.id)) copyOut(fresh, *a2);
const int newSlot = fresh.slotOf(name);
if (newSlot >= 0) out["value"] = paramJson(fresh, ...)["value"];
```
- **Lösung**: Hilfsfunktion `json readParamValue(const MaterialAsset&, int slot)` die direkt aus `shaderParamData`/`graphParamTypes` liest (es sind nur `paramData`, `paramTypes` noetig), ohne `Mat`.
- **Erwarteter Effekt**: Schaetzung: einige KB Kopien weniger je Aufruf, vernachlaessigbar ohne Messung; Wert liegt in der Lesbarkeit.
- **Risiko / Verifikation**: `paramJson` und die Direktfassung muessen dieselbe Bool/Float/Vec-Darstellung erzeugen; vorhandene `material_set_param`-Tests.

### EDT-1313 – Material-Graph-Abfragen sind O(Knoten × Pins × Links)
- **ID**: EDT-1313
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:793-806, 906-921, 358-367, 2106-2122`
- **Problem**: `linkInto`/`connectedInputs` laufen linear ueber alle Links und werden in `nodeJson` pro Eingangs-Pin (`sourceOf`), in `chainInto` pro Schritt und beim Output-Summary aufgerufen. `paramJson` durchsucht pro Parameter alle Knoten (`inGraph`). `reaches` baut `std::set<int>` und scannt bei jedem Pop alle Links (O(V·E)).
- **Code-Auszug**:
```cpp
for (const HE::MatGraphLink& l : g.links)
    if (l.dstNode == node && l.dstPin == pin) return &l;
```
- **Lösung**: Nur wenn gemessen relevant: einmalig eine `std::unordered_map<uint64_t, const MatGraphLink*>` (Key `dstNode<<32|dstPin`) und fuer `inGraph` ein `std::set<std::string>` der effektiven Param-Namen pro `materialJson` aufbauen. Fuer typische Materialien (< 100 Knoten) ist der Gewinn klein.
- **Erwarteter Effekt**: unsicher / messen; erst bei Graphen mit > 500 Knoten sichtbar.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe darf sich nicht aendern (die Links-Reihenfolge bestimmt, welcher bei doppelten `dst` gewinnt - `connect` erzwingt einen pro Pin).

### EDT-1314 – `openGraphForEdit` prueft Pfad/Typ doppelt; `material_node_types` laedt jede Funktion
- **ID**: EDT-1314
- **Kategorie**: A / E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/McpToolsMaterial.cpp:1357-1376, 1642-1674`
- **Problem**: `openGraphForEdit` ruft `checkPath` und `assetTypeOf`, danach ruft `openMat` beides noch einmal. Der Zweck (eigene Funktions-Meldung) laesst sich in `openMat` ueber einen Parameter oder einen `functionRefusal`-Text erreichen. `material_node_types` ruft pro gelistetem Funktions-Asset `cm->loadAsset(a.rel)` (laedt und registriert bis zu 80 Funktionen im ContentManager, was den Pool bewegt) und parst jeden Graph, nur um die Pin-Beschreibungen zu liefern; die Beschreibung (Zeile 1491) gibt das offen zu.
- **Code-Auszug**:
```cpp
const HE::UUID id = cm->loadAsset(a.rel);
const MaterialFunctionAsset* fn = id == HE::UUID{} ? nullptr : cm->getMaterialFunction(id);
```
- **Lösung**: `openMat(..., Refuse::FunctionWithGraphEditText)`; fuer `node_types` die Funktionsgraphen ueber die FILE-Chunk-Variante lesen (wie Input/Particle: `HAsset::Reader`, kein Load), das haelt die Aussage "eine Frage veraendert ihre Antwort nicht" ein.
- **Erwarteter Effekt**: ein Datei-Open und ein Sniff weniger pro Edit; node_types ohne Pool-Seiteneffekt (Schaetzung: klein).
- **Risiko / Verifikation**: Chunk-Name/Format fuer MaterialFunction-Graphen im Reader pruefen (CHUNK-Konstante in `HAsset.h`). Test: `material_node_types` darf `isLoaded` nicht aendern.

### EDT-1315 – Prefab: toter `blob`-Puffer, doppelte CBOR-Umwandlung; `prefab_instances` mit hartem 500er-Limit
- **ID**: EDT-1315
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsPrefab.cpp:154-163, 229-230, 457-458, 586-615, 706-713`
- **Problem**: `Prefab::blob` wird in `openPrefab` aus dem Chunk kopiert und nur fuer `json::from_cbor` gebraucht; kein Aufrufer liest `p.blob` (Grep: nur Zeilen 229-230). Zusaetzlich wird in `prefab_instantiate` der gepatchte `tree` wieder nach CBOR serialisiert. In `prefab_save` wird `serializeSubtree` → `from_cbor` → ggf. Re-`to_cbor` ausgefuehrt, obwohl nur Zaehler/Root gebraucht werden. `prefab_instances` begrenzt die Pfadsuche hart auf 500 Prefabs (`walkContentAssets(..., 500, ...)`); ein Projekt mit mehr Prefabs erhaelt fuer die uebrigen `"prefab": ""`, obwohl sie existieren (kein `truncated`-Hinweis).
- **Code-Auszug**:
```cpp
p.blob = chunk->data;
p.tree = json::from_cbor(p.blob, true, false);   // blob wird danach nicht mehr gelesen
...
walkContentAssets(*cm, { HE::AssetType::Prefab }, 500, truncated)   // truncated wird ignoriert
```
- **Lösung**: `blob` aus der Struct entfernen und `from_cbor(chunk->data)` direkt (Iteratorpaar) aufrufen. Beim 500er-Limit `truncated` auswerten und im Ergebnis `prefabPathsTruncated: true` melden oder die Zuordnung ueber `cm->pathForId` loesen (siehe EDT-1306).
- **Erwarteter Effekt**: eine Kopie des Prefab-Blobs weniger (Schaetzung: klein); das Limit behebt eine stille Fehlinformation.
- **Risiko / Verifikation**: Nur additive Ausgabefelder; Test mit > 500 Prefabs entfaellt, sinnvoll als Unit-Test mit Limit-Konstante.

### EDT-1316 – Screenshot: PNG wird beim Ausliefern kopiert
- **ID**: EDT-1316
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/McpToolsScreenshot.cpp:573-604`
- **Problem**: `png` ist `const std::vector<uint8_t>`, daher kopiert `r.imageBytes = png;` bis zu ~2,5 MB (Inline-Limit) vor dem Senden. Das Roh-RGBA (`rgba`, bis 3840×2160×4 = 33 MB) wird pro Aufruf neu allokiert (`std::vector` lokal). `generatedBaseName` nutzt ein nicht-atomares `static unsigned s_seq` (Zeile 401); ob zwei Bridge-Threads Handler parallel ausfuehren, ist in diesem Slice nicht erkennbar (unsicher).
- **Code-Auszug**:
```cpp
const std::vector<std::uint8_t> png = hePngEncode(...);
...
r.imageBytes  = png;
```
- **Lösung**: `std::vector<std::uint8_t> png = ...;` und `r.imageBytes = std::move(png);` (nach dem `png.size()`-Check). `rgba` als wiederverwendeten Member im `ScreenshotState` halten (`reserve` bei Groessenwechsel). `s_seq` zu `std::atomic<unsigned>`, falls Handler nebenlaeufig laufen koennen.
- **Erwarteter Effekt**: ein Memcpy von <= 2,5 MB weniger je Screenshot; `rgba`-Wiederverwendung spart bis zu 33 MB Allokation je Aufruf (Schaetzung).
- **Risiko / Verifikation**: `png.size()` vor dem Move in `result["pngBytes"]` (Zeile 577) bereits gelesen - Reihenfolge beibehalten. Kein Verhaltensunterschied.

### EDT-1317 – Input: Value-Type-Ableitung viermal, Pairs-Lambda doppelt, Tastenliste pro Aufruf neu aufgebaut
- **ID**: EDT-1317
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsInput.cpp:149-151, 537-538, 565-566, 629-630`; `:348-359`; `:197-204, 470-472`; `:671-672`
- **Problem**: Der Ausdruck `inputActionIsAxis2D(p) ? kVtAxis2D : inputActionIsAxis(p) ? kVtAxis : kVtButton` steht an vier Stellen (einer davon als `valueTypeOfActionFile`). `collectDeadNames` definiert zwei fast gleiche Lambdas (`add`/`addPad`), die sich nur im Praedikat unterscheiden. `allKeyNames()` allokiert pro `input_bindable`-Aufruf ~240 `std::string`, obwohl die Liste konstant ist. `input_action_set` liest bei Retype die Kontexte mit hartem Limit 200 (`findInputAssets(..., 200, ...)`) ohne `truncated` auszuwerten; bei mehr Kontexten bleibt `affectedMappings` unvollstaendig.
- **Code-Auszug**:
```cpp
const int vt = HE::inputActionIsAxis2D(d.payload) ? kVtAxis2D
             : HE::inputActionIsAxis(d.payload)   ? kVtAxis : kVtButton;
```
- **Lösung**: `int valueTypeOfPayload(const std::string& json)` einmal definieren und an den vier Stellen aufrufen. Ein Lambda `addDead(field, name, pred)` mit `bool(*)(const std::string&)`. `allKeyNames()` als `static const std::vector<std::string>` (Funktions-Static, thread-sicher in C++11). `truncated` bei `affectedMappings` melden.
- **Erwarteter Effekt**: -15 Zeilen, ~240 Allokationen weniger je `input_bindable` (Schaetzung).
- **Risiko / Verifikation**: Reine Umformung; Ausgabe identisch. SDL-Namenstabelle ist zur Laufzeit konstant. Vorhandene Input-MCP-Tests.

### EDT-1318 – Settings: Drei-Zustands-Rueckgabe `setCollisionLayerKey`, doppelter Katalog-Lookup, Schreiben ohne Aenderung
- **ID**: EDT-1318
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsSettings.cpp:381-429, 685-704, 706-714, 728-730`
- **Problem**: `setCollisionLayerKey` kodiert drei Ausgaenge ueber `bool`-Rueckgabe, `handled`-Out-Parameter und `err`: "nicht meine Form" (false, handled=false), "meine Form, Wert falsch" (false, handled=true), "gesetzt" (true). Der Aufrufer verzweigt darauf in einem `if / else if / else`-Gebilde. `findProjectSetting(key)` wird in `settings_set` zweimal ausgefuehrt (692 und 729) und linear gesucht. Das Manifest wird auch dann geschrieben, wenn `changed == false` (Kommentar 706-708 begruendet es); das ist eine Dateischreibung pro "unveraendertem" Aufruf (Schaetzung: vernachlaessigbar).
- **Code-Auszug**:
```cpp
bool handled = false;
if (setCollisionLayerKey(*proj, key, value, err, changed, handled)) touchedLayers = true;
else if (handled) return ToolResult::fail("invalid_argument", err);
else { const ProjSetting* d = findProjectSetting(key); ... }
```
- **Lösung**: `enum class LayerKeyResult { NotMine, Rejected, Applied }` als Rueckgabe, `err`/`changed` als Out-Parameter. Den gefundenen `ProjSetting*` einmal halten und fuer die Wertausgabe wiederverwenden. Optional: `saveProject` nur bei `changed` (Verhaltensaenderung - nicht ohne Absprache).
- **Erwarteter Effekt**: bessere Lesbarkeit, -1 Lookup.
- **Risiko / Verifikation**: Nur die Rueckgabeform aendern; Settings-MCP-Tests.

### EDT-1319 – Scene: Ordner-Anlage doppelt, `checkPlaying` gibt Zeiger auf Scratch zurueck
- **ID**: EDT-1319
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsScene.cpp:54-63, 171-172, 250-251, 333-334, 187-201, 272-280`
- **Problem**: `ToolResult* checkPlaying(const McpSceneHooks&, ToolResult& scratch)` liefert einen Zeiger auf einen vom Aufrufer bereitgestellten Scratch zurueck; jeder Aufrufer braucht drei Zeilen (`ToolResult scratch; if (ToolResult* r = ...) return *r;`). Die "Elternordner anlegen"-Sequenz (`exists(parent)`, `create_directories`, Fehlertext) steht in `scene_save` und `scene_create` fast wortgleich.
- **Code-Auszug**:
```cpp
ToolResult scratch;
if (ToolResult* r = checkPlaying(*h, scratch)) return *r;
```
- **Lösung**: `std::optional<ToolResult> checkPlaying(const McpSceneHooks&)` (oder `bool playing` plus gemeinsame `failPlayMode()`); `ToolResult ensureParentDir(const PathCheck&)` fuer den Ordner-Block (Rueckgabe "ok" oder "failed").
- **Erwarteter Effekt**: -20 Zeilen, einfacher Aufrufstil.
- **Risiko / Verifikation**: Reine Umformung; Scene-MCP-Tests.

### EDT-1320 – TaskBoard: Status-/Prioritaets-Parsing dreifach, fehlende Includes
- **ID**: EDT-1320
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/McpToolsTaskBoard.cpp:251-258, 304-309, 1-9, 154-161`
- **Problem**: Das Muster "Name parsen, `Other` ablehnen, Fehlertext" steht fuer `status` in `task_create` und `task_update` und fuer `priority` in `task_create`. `std::function` und `std::int64_t` werden benutzt (`runEdit`, Zeile 266-268), aber `<functional>`/`<cstdint>` nicht eingebunden - es kompiliert nur ueber transitive Includes (F). `taskAnswer` ruft bei jeder Antwort `teamJson`, das `team->members()` kopiert; fuer jede Task-Aenderung ein Vektor-Copy (Schaetzung: unbedeutend).
- **Code-Auszug**:
```cpp
if (!sw.empty() && (!HE::taskStatusFromName(sw, status) || status == HE::TaskStatus::Other))
    return ToolResult::fail("invalid_payload", "'status' must be open, in-progress, blocked or done");
```
- **Lösung**: `bool parseStatusArg(const json&, const char* key, TaskStatus&, ToolResult&)` bzw. fuer Prioritaet; fehlende Includes ergaenzen.
- **Erwarteter Effekt**: -10 Zeilen, robuster Build.
- **Risiko / Verifikation**: Texte unveraendert lassen; Taskboard-MCP-Tests.

### EDT-1321 – Terrain: tote Bedingungen im Heightmap-Tool, Wegwerf-`HorizonWorld` im Lake-Tool
- **ID**: EDT-1321
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/McpToolsTerrain.cpp:394-400, 429-430, 1113-1119`
- **Problem**: `n` wird in Zeile 394 auf 2..128 geklemmt, danach pruefen Zeile 399-400 `(n > 1)` und Zeile 429-430 `heights.empty()` - beide Bedingungen sind immer wahr/falsch (tote Zweige). Im Lake-Tool wird fuer jeden Aufruf ein eigenes `HorizonWorld scratch` angelegt, dort ein `TerrainComponent` kopiert (`emplace<TerrainComponent>(se, work)`) und serialisiert, um das `terrain`-JSON zu erhalten; die anderen drei Tools holen es aus der echten Entity und patchen nur Felder (`componentsOf(*r.world, r.entity)`). Die Kosten von Konstruktor/Destruktor einer `HorizonWorld` sind nicht bekannt (unsicher / messen).
- **Code-Auszug**:
```cpp
const float stepX = (n > 1) ? (maxX - minX) / static_cast<float>(n - 1) : 0.0f;   // n >= 2
...
{ "min", heights.empty() ? 0.0f : mn },                                           // nie leer
...
HorizonWorld scratch;
scratch.registry().emplace<TerrainComponent>(se, work);
```
- **Lösung**: Tote Bedingungen entfernen. Fuer Lake dieselbe Methode wie in sculpt (Patch auf dem echten Entity-JSON mit `sculptHeightsB64`, Water-Felder) oder eine kleine `terrainToJson(const TerrainComponent&)` in `SceneSerializer` anbieten (Cross-Modul).
- **Erwarteter Effekt**: -6 Zeilen; Lake spart eine Welt-Allokation (unsicher).
- **Risiko / Verifikation**: Lake schreibt auch `water` (das nur im Serializer liegt) - der Patch-Weg muss dieselben Felder liefern; Lake-MCP-Tests (Create/Reshape/Dig/Level/Remove) vorher/nachher vergleichen.

### EDT-1322 – `limit`-Behandlung: sechs Varianten
- **ID**: EDT-1322
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolsInput.cpp:550, 737`; `McpToolsMaterial.cpp:544, 1638-1640`; `McpToolsParticle.cpp:403`; `McpToolsPrefab.cpp:317-318`; `McpToolsSequence.cpp:593`
- **Problem**: Jeder Katalog klemmt `limit` anders: `std::max(1, intArg(...))` (Input, Material), manuelle `args.contains`+`is_number_integer` (Material node_types), `if (limit <= 0) limit = 200` (Prefab), gar nicht (Particle, Sequence). `walkContentAssets` mit `limit <= 0` liefert dann eine leere Liste mit `truncated = true` ohne Fehler.
- **Code-Auszug**:
```cpp
walkContentAssets(*cm, { HE::AssetType::ParticleSystem }, intArg(args, "limit", 200), truncated);
```
- **Lösung**: `int limitArg(const json&, int def)` in `McpToolCommon` (klemmt auf [1, 5000]) und ueberall benutzen.
- **Erwarteter Effekt**: einheitliches Verhalten, -10 Zeilen.
- **Risiko / Verifikation**: Aenderung des Verhaltens fuer `limit <= 0` bei Particle/Sequence (heute: leere Liste); fuer Clients unproblematisch, aber als Aenderung vermerken.

## Cross-Modul-Hinweise

- `src/HE_Editor/McpToolCommon.cpp:197-237` (`walkContentAssets`): jeder Katalog-Aufruf scannt den gesamten Content-Baum rekursiv, sortiert und sniffed alle `.hasset`. Ein durch `ContentManager`s Content-Change-Zaehler invalidierter Index wuerde alle Kataloge (asset_list, material_info, input_*, sequence_info, prefab_info, particle_info) beschleunigen. Siehe EDT-1309.
- `src/HE_Core/src/ContentManager/ContentManager.cpp:790-818` (`regenerateMaterialFromGraph`): Rueckgabetyp `void`, bricht bei leerem GLSL still ab; ein `bool`/Overload mit vorberechnetem `MatShaderGen` wuerde EDT-1303 ermoeglichen.
- `src/HE_Editor/EditorApplication.cpp:10782-10785`: der Terrain-`regenerate`-Hook ruft `TerrainSystem::updateTerrains(*m_editorWorld, ...)` fuer die gesamte Welt nach jedem Dab; mit EDT-1301 (mehrere Dabs pro Aufruf) laesst sich das ebenfalls reduzieren. Ob `updateTerrains` nur dirty Chunks anfasst, ist dort zu pruefen.
- `McpToolsAnimator.cpp`, `McpToolsAsset.cpp`, `McpToolsType.cpp`, `McpToolsEntity.cpp`: tragen dieselben Kopien von `uuidJson`/`readPayload`/`failFor`/`vec3Arg` (siehe EDT-1308); die Gate-Kette (EDT-1307) kommt vermutlich auch in `McpToolsWidget.cpp`/`McpToolsClip.cpp` vor.
- `src/HE_Editor/McpToolRegistry.h`: die Hook-Structs `McpInputHooks`, `McpMaterialHooks`, `McpParticleHooks`, `McpSequenceHooks`, `McpPrefabHooks` tragen gleichartige `isPlaying/lockedByOther/isDirty/reloadFromDisk`-Felder (Basis `McpAssetWriteHooks` bietet sich an).
- `EditorCommands.cpp:406-417` (`uuidOf`) formatiert die UUID per `snprintf`, obwohl `HE::uuidToHex` existiert (TaskBoard nutzt es).

## Unsicher / Messung nötig

- Tatsaechliche Kosten eines Terrain-Dabs (EDT-1301) und der Gewinn eines Teil-Patches: mit 1025²-Landschaft und 100 Aufrufen messen; ebenso `HorizonWorld`-Konstruktor (EDT-1321).
- `computeTerrainHeightField` fuer prozedurale Landschaften (EDT-1302) bei hoher Aufloesung und vielen Oktaven.
- Codegen-Zeit eines grossen Material-Graphen (EDT-1303) und Verhaeltnis zu JSON-Parse/Serialisierung.
- Ob die MCP-Bridge Handler auf dem UI-Thread oder einem Worker ausfuehrt (bestimmt, ob EDT-1301/1306/1309 Frame-Hitches oder nur Antwortzeit verursachen) und ob Handler nebenlaeufig laufen (EDT-1316, `s_seq`).
- Aktualitaet der `m_diskRegistry` fuer `pathForId` (EDT-1306).
- `uuidJson`-Ausgabeformat bei `uint64` > 2^53 (EDT-1308).
- Graph-Groessen in der Praxis fuer EDT-1313.

## Gelesen

Alle zehn Dateien des Slice vollstaendig gelesen: ja (McpToolsInput, Material, Particle, Prefab, Scene, Screenshot, Sequence, Settings, TaskBoard, Terrain); zur Verifikation nur gezielt gelesen: `McpToolCommon.cpp:18-45, 197-237`, `ContentManager.h:60-95`, `ContentManager.cpp:770-818`, `EditorCommands.cpp:400-420`, `EditorApplication.cpp:10775-10790`.
