# Modulbericht Tools – HorizonTools / Importers

Pfad: `src/HE_Tools` · Teilberichte: 2 (`_parts/Tools_*.md`)

## Modulzusammenfassung

**27 Funde**: Hoch 3, Mittel 11, Niedrig 13 · Typ: Vereinfachung 9, Performance 14, Beides 4 · Kategorien: A 7, B 0, C 6, D 8, E 4, F 2

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Tools_1.md | 2 | 8 | 7 |
| Tools_2.md | 1 | 3 | 6 |

### Hoch-Funde (Index)

- **TLS-101** – `writeAsset` baut pro Asset einen kompletten `ContentManager`
- **TLS-102** – No-op-Lauf des `asset_compiler` liest jedes Mesh-`.hasset` vollständig ein
- **TLS-201** – glslang-Prozess wird bei jedem Shader-Compile neu initialisiert und abgebaut

## Zusammenfassungen der Teilberichte

**Tools_1.md**: Der Slice (33 Dateien, ca. 10,3k Zeilen) ist inhaltlich sauber und sehr gut kommentiert; die Importer sind bereits sinnvoll in einen gemeinsamen Kern (`PbrMaterialImport`, `ImporterCommon`) zerlegt. Die Schwachstellen liegen weniger in der Logik als in der **Offline-Pipeline-Performance**: Jeder geschriebene Asset konstruiert einen vollständigen `ContentManager` (inkl. Default-Assets, Icon-Rasterung und Shader-Codegen), der No-op-Lauf des `asset_compiler` liest jedes Mesh-`.hasset` komplett ein, und glTF/GLB-Dateien werden pro Datei mehrfach vollständig geparst. Dazu kommen eine serielle Import-Schleife, elementweise Accessor-Reads ohne `reserve` und eine zweite, driftende Kopie der Routing-Logik im `asset_compiler`. Funde: 2 Hoch, 8 Mittel, 7 Niedrig. Es wurden keine Messungen durchgeführt; Aufwandsschätzungen sind als solche gekennzeichnet. `ProjectManager`, `CppScaffoldTemplates`, `HcCodegenCli`, `MatFnGen` sind kalter Code (Projekt-Anlage bzw. Dev-Tools) und nur mit niedriger Schwere bewertet.

**Tools_2.md**: Der Slice (11 Dateien, ca. 3470 Zeilen) ist insgesamt sauber und gut kommentiert. Die Generatoren (`mat_gen`, `mesh_gen`, `widget_gen`, `weather_sound_gen`) laufen offline und nicht im Build-Graph, daher ist ihre Performance praktisch irrelevant. Relevant für Ladezeit und Offline-Pipeline sind vor allem `he_shadercompiler` (wird zur Laufzeit von allen Renderern über `MaterialShaderLibrary` genutzt) und `hpak_packer` (CLI-Wrapper um `HpakWriter`). Funde: 1 Hoch, 3 Mittel, 6 Niedrig. Die Hauptpunkte sind das pro Aufruf wiederholte `glslang::InitializeProcess()/FinalizeProcess()`, der fehlende Inkrementell-Cache im Packer-CLI und ein Generator, der Verdrahtungsfehler nur auf stderr meldet und trotzdem Exit-Code 0 liefert.

## Cross-Modul-Hinweise

**Tools_1.md**

- `src/HE_Core/src/ContentManager/ContentManager.cpp:3088-3090, 3380-3690`: Der `ContentManager`-Konstruktor ist für Offline-Werkzeuge zu schwer (Icon-Rasterisierung, 5x `generateFragment`). Eine Option `ContentManager(root, InitFlags::None)` oder eine lazy `initDefaultAssets()` hilft auch Tests und jedem Tool, das kurz einen Manager braucht (siehe TLS-101).
- `src/HE_Core/include/ContentManager/HAsset.h:380-430` (`Reader::open`): materialisiert alle Chunks; ein `Reader::openChunk(path, id)` (Streaming wie `readOneChunk` in `ImporterCommon.cpp:108`) sollte dort zentral leben und in `HE::AssetRefs::assetUuidOfFile` mitbenutzt werden.
- `src/HE_Tools/vendor/cgltf.h`: `cgltf_parse_file` liest bei `.glb` die gesamte Datei; Header-/JSON-Chunk-Probe für Metadatenabfragen (Skin, Animationsnamen) wäre auch für den Editor (Import-Dialog, Content Browser) nützlich.
- `src/HE_Core` `JobSystem`: Ob die Tools es linken können (für TLS-104), konnte in diesem Slice nicht geprüft werden (CMake liegt außerhalb der Dateiliste).
- Aufrufer von `Importer::reimport`/`importSource` im Editor (ContentBrowserPanel, nicht gelesen) könnten diese synchron auf dem UI-Thread ausführen; dann treffen TLS-101/103/107 direkt die Editor-Responsiveness. Prüfen.

**Tools_2.md**

- `src/HE_Rendering/src/Backends/{D3D11,D3D12,Vulkan}/*Renderer.cpp` rufen `he::shaderc` auf; TLS-201 wirkt dort auf die Ladezeit. Die Metal- und GL-Pfade laufen über `MaterialShaderLibrary` (nicht Teil dieses Slices). Ob die Ergebnisse von `compile*` pro Quelltext-Hash gecacht werden, wurde nicht geprüft; falls nicht, wäre ein Cache (GLSL-Hash plus Target plus Pins) in `MaterialShaderLibrary` der größere Hebel als TLS-201.
- `src/HE_Core/src/Hpak/ProjectExporter.cpp` enthält die Cache-Lade-/Speicherlogik für `IncrementalCache` (TLS-203). Diese sollte in eine gemeinsame Funktion in `src/HE_Core/src/Hpak/` wandern, damit das CLI sie mitnutzt.
- `src/HE_Core/src/Hpak/HpakWriter.cpp:1204-1207`: `write()` hat keinen Atomic-Rename; ein `writeAtomic(path)` im Writer selbst würde TLS-205 für Editor-Export und CLI zugleich lösen.
- Packer-Verschlüsselung (`main.cpp:113-118`) leitet den Schlüssel mit Null-Salt ab; gleiche Passphrase ergibt für alle Spiele denselben Schlüssel. Das ist im Dateikopf als bewusst beschrieben und hier nur als Hinweis für ein mögliches Sicherheits-Audit des Moduls HE_Core/Hpak (`KeyDerivation.h`) vermerkt, nicht als Fund gewertet.

## Unsicher / Messung nötig

**Tools_1.md**

- Tatsächliche Kosten eines `ContentManager`-Konstruktors (TLS-101): Profil über einen `asset_compiler`-Lauf auf einem Beispielprojekt.
- Anteil von `meshSidecarAssets` an der Dauer eines No-op-Laufs (TLS-102): Vergleich mit/ohne mit großem Mesh-Bestand (Dateisystemcache kalt/warm).
- Ob `AnimationClipImporter` nur LINEAR-Interpolation übernimmt (`:131-132`, STEP/CUBICSPLINE werden still verworfen, ohne Log): Funktionslücke oder bewusst; hier nur notiert, nicht bewertet.
- `CppScaffoldTemplates.cpp:293` schreibt `CMAKE_CXX_STANDARD 17` in generierte Projekte, während die Engine C++20 ist: unklar, ob `IGameLogic.h`/`HorizonGameServices.h` C++20-Features voraussetzen.
- `NameReserver::isFree` (`PbrMaterialImport.cpp:150-161`) macht pro Kandidat Dateisystemzugriffe (`is_regular_file` + `ifstream`) und `lower()`-Allokationen; bei Meshes mit hunderten Materialien/Texturen möglicherweise relevant, ohne Messung nicht priorisiert.
- Assimp-Flags (`AssimpMeshImport.cpp:220-222`): `JoinIdenticalVertices` dominiert vermutlich die Ladezeit großer FBX; kein Vergleich mit/ohne gemessen.
- Kein Vertex-Cache-/Overdraw-Optimizer (z.B. meshoptimizer) im Importpfad; ob die Pack-Stufe (`hpak_packer`, nicht in diesem Slice) das nachholt, ist unbekannt.

**Tools_2.md**

- TLS-201: tatsächlicher Gewinn von Einmal-Init gegenüber Init pro Aufruf (Zeit pro `compile` mit/ohne Init in Release auf macOS, Windows, Linux messen).
- TLS-203: Anteil der Texturen (BC7/ASTC) an der Packzeit realer Projekte, daraus der erwartete Gewinn durch den Cache.
- TLS-206: ob `MaterialGraph::connect` Zyklen ablehnt (nicht geprüft).
- TLS-210: ob ein Aufrufer `MultiResult::perTarget[i].spirv` liest.
- `compileGlslPinned` und `emitGlsl430`: lineare Pin-Suche pro Ressource ist O(Ressourcen·Pins); bei Pin-Listen unter 40 Einträgen vernachlässigbar, nicht gemessen.
- Ob `mat_gen`/`widget_gen`/`mesh_gen`/`weather_sound_gen` in CI regeneriert werden (dann wäre die Laufzeit relevant); laut Dateikopf nicht Teil des Build-Graphs.


---
# Funde (vollständig, nach Teilbericht)

## Tools Slice 1 – AssetCompiler, AssetImporter, FileOps, HcCodegenCli, LandscapeTexGen, MatFnGen

## Zusammenfassung

Der Slice (33 Dateien, ca. 10,3k Zeilen) ist inhaltlich sauber und sehr gut kommentiert; die Importer sind bereits sinnvoll in einen gemeinsamen Kern (`PbrMaterialImport`, `ImporterCommon`) zerlegt. Die Schwachstellen liegen weniger in der Logik als in der **Offline-Pipeline-Performance**: Jeder geschriebene Asset konstruiert einen vollständigen `ContentManager` (inkl. Default-Assets, Icon-Rasterung und Shader-Codegen), der No-op-Lauf des `asset_compiler` liest jedes Mesh-`.hasset` komplett ein, und glTF/GLB-Dateien werden pro Datei mehrfach vollständig geparst. Dazu kommen eine serielle Import-Schleife, elementweise Accessor-Reads ohne `reserve` und eine zweite, driftende Kopie der Routing-Logik im `asset_compiler`. Funde: 2 Hoch, 8 Mittel, 7 Niedrig. Es wurden keine Messungen durchgeführt; Aufwandsschätzungen sind als solche gekennzeichnet. `ProjectManager`, `CppScaffoldTemplates`, `HcCodegenCli`, `MatFnGen` sind kalter Code (Projekt-Anlage bzw. Dev-Tools) und nur mit niedriger Schwere bewertet.

## Funde

### TLS-101 – `writeAsset` baut pro Asset einen kompletten `ContentManager`
- **ID**: TLS-101
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetImporter/ImporterCommon.cpp:357-358` (zusätzlich `:300-310` in `setTextureSrgb`); Ursache in `src/HE_Core/src/ContentManager/ContentManager.cpp:3088-3090`, `initDefaultAssets()` ab `:3380` (Icon-Block ab ca. `:3606`)
- **Problem**: Jeder Importer-Output geht durch `Importer::writeAsset`, und das legt jedes Mal `ContentManager cm(contentRoot)` an, nur um `saveAsset` aufzurufen. Der Konstruktor ruft `initDefaultAssets()`: Default-Meshes, 128x128-Grid-Textur, Terrain-Material und fünf Editor-Icons (je `uiRasterizeIcon` + 64x64-Dilatation mit 3x3-Nachbarschaft, je ein `MaterialGraph`, `generateFragment` und `materialGraphToJson`). Ein Mesh-Import mit N Materialien und M Texturen erzeugt N+M+1 solcher Manager, ein `asset_compiler`-Lauf über ein Projekt entsprechend Tausende. Die gesamte Arbeit wird sofort verworfen.
- **Code-Auszug**:
```cpp
// ImporterCommon.cpp:357
ContentManager cm(contentRoot.string());
if (!cm.saveAsset(asset)) { ... }
// ContentManager.cpp:3088
ContentManager::ContentManager(std::string contentPath)
	: m_contentRoot(std::move(contentPath)) { initDefaultAssets(); }
```
- **Lösung**: (a) Pro Prozess und Content-Root einen wiederverwendeten Writer-`ContentManager` halten (`static`/`thread_local` Cache in `ImporterCommon.cpp`, Schlüssel = `contentRoot`), oder (b) besser: in HE_Core eine schlanke Methode anbieten, die ohne Default-Assets speichert (z.B. `ContentManager::ContentManager(std::string, NoDefaultAssets)` oder freie Funktion `saveAssetFile(root, asset)` auf Basis von `writeAssetFile`/`resolveSavePath`). `saveAsset` benötigt für den Pfad nur `m_contentRoot`, `m_engineContentRoot` und `noteContentChanged`. Das Gleiche gilt für `setTextureSrgb`, das den Manager ohnehin für `loadAsset` braucht (dort Manager wiederverwenden).
- **Erwarteter Effekt**: Schätzung: pro Asset einige hundert µs bis wenige ms Konstruktionskosten entfallen (Icon-Raster + 5x Shader-Codegen); bei 1000+ Outputs eine Sekundenordnung. Unsicher, messen (Profil von `ContentManager::ContentManager` in einem `asset_compiler`-Lauf).
- **Risiko / Verifikation**: `saveAsset` löst `m_onAssetSaved`/`noteContentChanged` aus (Instanz-Zustand) und prüft `ProjectWriteGate`; ein geteilter Manager darf keine Residency-Caches anwachsen lassen (er lädt nichts). Verifikation: `test_meshimporter`, `test_skeletalmeshimporter`, `test_assimpimport`, `test_gltf_material_import` laufen unverändert; Byte-Vergleich der erzeugten `.hasset` vor/nach.

### TLS-102 – No-op-Lauf des `asset_compiler` liest jedes Mesh-`.hasset` vollständig ein
- **ID**: TLS-102
- **Kategorie**: E / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetImporter/ImporterCommon.cpp:882-959` (`meshSidecarAssets`), aufgerufen aus `:979` (`importOutputsUpToDate`)
- **Problem**: Der Up-to-date-Check ruft pro Quelldatei `meshSidecarAssets(primaryOutput, ...)`, und das öffnet das Mesh-Asset mit `HAsset::Reader::open`. Dieser Reader materialisiert laut eigenem Kommentar (`ImporterCommon.cpp:90-96`) und laut `HAsset.h` (`c.data.resize(ch.size)` + `f.read`) **jeden Chunk-Payload**, also auch die kompletten Vertex-/Index-Puffer. Nur der MREF-Chunk (ein String) wird gebraucht. Das gleiche Problem hat die Datei an anderer Stelle bereits behoben (`readOneChunk`, "sourceFileOf wird einmal pro Frame aufgerufen"), `meshSidecarAssets` wurde dabei vergessen. Ein inkrementeller Lauf ohne Änderungen liest damit alle Meshes (oft mehrere GB) vollständig von der Platte.
- **Code-Auszug**:
```cpp
HAsset::Reader reader;
if (!reader.open(meshAsset.string()))
	return out;
const auto* mref = reader.findChunk(HAsset::CHUNK_MREF);
...
HAsset::Reader matReader;
if (!matReader.open((contentRoot / materialPath).string()))
```
- **Lösung**: `readOneChunk(meshAsset, HAsset::CHUNK_MREF, buf)` und `readOneChunk(contentRoot / materialPath, HAsset::CHUNK_MTRL, buf)` verwenden (existiert schon, Seek über fremde Payloads, Größenprüfung inklusive). Danach unverändert mit `Reader::readString/readPOD/readVec` auf `buf` weiterlesen (die Funktionen arbeiten auf `std::vector<uint8_t>`). Optional `readOneChunk` in `HAsset::Reader::openChunk(path, id)` überführen, damit `ContentManager` es auch nutzen kann.
- **Erwarteter Effekt**: Schätzung: I/O des No-op-Laufs sinkt von "Summe aller Mesh-Assets" auf wenige KB pro Mesh; bei kaltem Cache Größenordnung Faktor 100-1000 weniger gelesene Bytes.
- **Risiko / Verifikation**: Ergebnisliste muss identisch bleiben (Reihenfolge Material, dann Texturen, dedupliziert). Zu prüfen: Verhalten bei defekten Dateien (`readOneChunk` liefert false statt Teilergebnis, wie `Reader::open`). Test ergänzen: `importOutputsUpToDate` für Mesh mit gelöschter Normal-Map liefert false (Regression des Kommentars `:928-933`); vorhandene Importer-Tests decken `meshSidecarAssets` nur indirekt ab.

### TLS-103 – glTF/GLB wird pro Datei mehrfach vollständig geparst (GLB jedes Mal komplett gelesen)
- **ID**: TLS-103
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ImporterCommon.cpp:55-75` (`gltfHasSkin`), `:718`, `:843`, `:863`; `AnimationClipImporter.cpp:93-166` und `:203-231`; `AssetCompiler/main.cpp:53`; `SkeletalMeshImporter.cpp:61-73`, `MeshImporter.cpp:210-223`
- **Problem**: Die Kommentare behaupten, `cgltf_parse_file` lese "nur das JSON, ohne die .bin-Puffer". Für `.gltf` mit externer `.bin` stimmt das, für `.glb` nicht: `cgltf_default_file_read` (`vendor/cgltf.h:1042ff`) liest die **gesamte Datei** inklusive BIN-Chunk in den Speicher. Die Parser-Aufrufe pro Datei: `asset_compiler` (`classify` → `gltfHasSkin`) 1x; bei geriggten Dateien zusätzlich `AnimationClipImporter::outputPaths` im Up-to-date-Check 1x; im Importfall `SkeletalMeshImporter::import` (parse + load_buffers) 1x und `AnimationClipImporter::importAndWrite` (parse + load_buffers) nochmals 1x. `reimport()` ruft `gltfHasSkin` dreimal (`:718` via `importSource`, `:843`, `:863`) und `outputPaths` zweimal. Bei einer 200-MB-GLB sind das 3-5 volle Dateilesungen und Parses.
- **Code-Auszug**:
```cpp
// ImporterCommon.cpp:843 / :863 (reimport)
if (classifySource(source) == SourceKind::Mesh && gltfHasSkin(source))
...
if (classifySource(source) == SourceKind::Mesh && gltfHasSkin(source))
```
- **Lösung**: (1) In `reimport` das Ergebnis einmal in `const bool rigged` ablegen. (2) Für `.glb` den Skin-Check ohne Voll-Read: nur Header (12 Byte) + ersten JSON-Chunk lesen und `cgltf_parse` auf diesen Puffer anwenden (`cgltf_parse(&opt, jsonBytes, size, &data)` kann ein GLB-Gerüst nicht ohne BIN lesen, daher JSON-Chunk als reines `.gltf` parsen oder per `"skins"`-Key im JSON-Chunk prüfen). (3) `AnimationClipImporter::importAndWrite` ein bereits geladenes `cgltf_data*` übergeben können (Overload), damit Skeletal- und Clip-Import eine Datei teilen; `outputPaths` und `gltfHasSkin` können dieselbe Parse-Instanz teilen (kleine Struktur `GltfProbe{hasSkin, clipNames}`, pro Quelldatei einmal im `asset_compiler`).
- **Erwarteter Effekt**: Schätzung: bei großen GLB 2-4x weniger Lese-/Parse-Zeit pro Rig-Datei im Import und im No-op-Lauf.
- **Risiko / Verifikation**: Namensgebung der Clips muss exakt gleich bleiben (`clipFileStem` bleibt gemeinsame Quelle). `test_skeletalmeshimporter` plus ein Test "GLB mit Skin wird als Skeletal erkannt" (heute nur indirekt). JSON-Chunk-Variante braucht Test mit GLB ohne/mit `skins`.

### TLS-104 – `asset_compiler` importiert strikt seriell
- **ID**: TLS-104
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetCompiler/main.cpp:119-180`
- **Problem**: Die Schleife `for entry : recursive_directory_iterator` dekodiert Texturen (stb), liest/bakt Meshes, resampled Audio und schreibt Assets nacheinander in einem Thread. Der Offline-Build ist typischerweise CPU-bound (PNG/JPG-Decode, Assimp, Shader-Codegen) und die Dateien sind weitgehend unabhängig. Es gibt keinerlei Parallelität und keinen Job-Einsatz (HE_Core hat ein `JobSystem`, die Tools linken es evtl. nicht).
- **Code-Auszug**:
```cpp
for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
{
	...
	ok = TextureImporter::import(entry.path(), outputDir, relOut, settings) != nullptr;
```
- **Lösung**: Phase 1 sammelt (Quelle, `kind`, `relOut`, `primary`) in einen `std::vector`. Phase 2 verarbeitet Texturen, Audio, Fonts und `.hmat` parallel (`std::for_each(std::execution::par)` oder ein einfacher Thread-Pool mit atomaren Zählern); Meshes zuletzt oder seriell, da sie Sidecar-Materialien/-Texturen schreiben und über `NameReserver` kollidieren können (gleiche Ausgabedatei aus zwei Meshes mit gemeinsamer Textur). Voraussetzungen: `stbi_set_flip_vertically_on_load` ist ein **globales** Flag (`TextureImporter.cpp:34,49`) und müsste `thread_local` (`STBI_THREAD_LOCAL`) oder durch `stbi_load_..._flip`-freie Variante ersetzt werden; Logger und `ContentManager` (TLS-101) müssen threadsicher sein.
- **Erwarteter Effekt**: Schätzung: nahezu linear mit Kernzahl für texturlastige Projekte (4-8x auf typischen Build-Maschinen).
- **Risiko / Verifikation**: Hoch für Meshes (Namenskollisionen, siehe oben), daher nur nicht-mesh-Typen parallelisieren. Deterministische Ausgabe sicherstellen (UUID-Vergabe in `writeAsset` hängt am vorhandenen File, nicht an der Reihenfolge). Test: zwei Läufe seriell/parallel ergeben byte-identische Ausgabeverzeichnisse (außer neu erzeugte UUIDs).

### TLS-105 – Vertex-/Index-/Animationsdaten elementweise gelesen, ohne `reserve`
- **ID**: TLS-105
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ImporterCommon.cpp:404-448` (`appendPrimitive`), `:460-477` (`appendSkinning`); `AssimpMeshImport.cpp:39-107` (`appendMesh`); `AnimationClipImporter.cpp:34-46` (`readFloats`)
- **Problem**: Für jeden Vertex werden bis zu vier einzelne `cgltf_accessor_read_float`-Aufrufe (jeweils mit Typ-/Komponenten-Dispatch und Normalisierungs-Pfad) gemacht und das Ergebnis mit `out.positions.insert(end, {x,y,z})` an Vektoren ohne `reserve` angehängt (Reallokationen + Initializer-List pro Vertex). Indizes laufen über `cgltf_accessor_read_index` pro Index plus `push_back`. `readFloats` ruft pro Element `cgltf_accessor_read_float`. cgltf bietet Bulk-Pfade (`cgltf_accessor_unpack_floats`, `cgltf_accessor_unpack_indices`, `vendor/cgltf.h:886-887`), die intern dasselbe schneller und in einem Aufruf tun. Beim Assimp-Pfad fehlt die Vorabsumme der Vertexzahl ebenfalls.
- **Code-Auszug**:
```cpp
for (cgltf_size v = 0; v < attrs.position->count; ++v)
{
	float p[3] = {};
	cgltf_accessor_read_float(attrs.position, v, p, 3);
	glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f)) * uniformScale;
	out.positions.insert(out.positions.end(), { wp.x, wp.y, wp.z });
```
- **Lösung**: Pro Primitive: `const size_t n = position->count; out.positions.reserve(size+3n)` (analog normals/uvs); Rohdaten einmalig mit `cgltf_accessor_unpack_floats(acc, tmp.data(), n*3)` in einen wiederverwendeten Scratch-Vektor lesen, danach nur die Transformationsschleife. Indizes: `indices.resize(old + count); cgltf_accessor_unpack_indices(prim.indices, &indices[old], sizeof(uint32_t), count)` und anschließend `baseVertex` addieren. Für den Assimp-Pfad in `bake()` vorab `sum(mNumVertices)` über die Mesh-Referenzen der Nodes berechnen und `reserve`n. `readFloats`: ein `unpack_floats`.
- **Erwarteter Effekt**: Schätzung: Geometrie-Bake-Anteil 2-4x schneller; bei Meshes mit mehreren Millionen Vertices Sekundenbruchteile bis Sekunden. Gesamtimport wird zusätzlich von Decode/Schreiben dominiert, daher messen.
- **Risiko / Verifikation**: Normalisierte/Sparse-Accessoren: `unpack_floats` behandelt sie wie `read_float`; Ergebnis muss bitgleich sein. `test_meshimporter`, `test_skeletalmeshimporter`, `test_assimpimport` (inkl. OBJ-/Metre-FBX-Fixtures) laufen lassen; zusätzlich Golden-Vergleich der Vertexpuffer vor/nach.

### TLS-106 – `asset_compiler` dupliziert Routing/Dispatch von `importSource` (Drift: `.psd`)
- **ID**: TLS-106
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Tools/src/AssetCompiler/main.cpp:37-67, 71-88, 142-175` gegen `ImporterCommon.cpp:582-661, 706-759`
- **Problem**: `ImporterCommon.cpp:11-12` nennt `importSource` ausdrücklich "the one place that decides which importer a file belongs to", `AnimationClipImporter`-Kommentare und `ImporterCommon.h:301-308` verlangen dasselbe. Der `asset_compiler` hat trotzdem einen eigenen `SourceKind`-Enum, `classify` mit eigener Extension-Liste und eine eigene `switch`-Dispatch. Die Listen sind bereits gedriftet: `classify` akzeptiert `.psd` (`main.cpp:46`), `kSourceFamilies` ("png;jpg;jpeg;tga;bmp;hdr", `ImporterCommon.cpp:611`) nicht. Der `ProjectWriteGate`-Check aus `importSource` fehlt im Compiler. Zusätzlich löst `fs::relative` (`main.cpp:128`) pro Datei über `weakly_canonical` mehrere Dateisystemzugriffe aus.
- **Code-Auszug**:
```cpp
// main.cpp:45-46
if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
    ext == ".bmp" || ext == ".psd" || ext == ".hdr")
// ImporterCommon.cpp:611
{ "Textures",  "png;jpg;jpeg;tga;bmp;hdr", SourceKind::Texture  },
```
- **Lösung**: Im `asset_compiler` `Importer::isImportableSource` für die Auswahl und `Importer::importSource` (mit `ImportOptions{.textureSrgb = textureSrgbOf(primary)}`) für den Import verwenden; nur `primaryOutput`/`extraOutputs` für den Up-to-date-Check bleiben lokal (am besten als `Importer::expectedOutputs(source)` neben `classifySource`, damit auch diese Logik nur einmal existiert). Die Entscheidung `.psd` ja/nein treffen und in eine Liste schreiben. `relOut` mit `entry.path().parent_path().lexically_relative(sourceDir)` statt `fs::relative`.
- **Erwarteter Effekt**: Weniger Code (ca. 60 Zeilen), kein Auseinanderlaufen von Editor- und Offline-Pfad; kleiner Gewinn durch weniger Dateisystemaufrufe.
- **Risiko / Verifikation**: Verhaltensänderung beim Skeletal-Pfad: `importSource` ignoriert die `AnimationClipImporter`-Zählung (`clips.written/failed` fließen heute in die Statistik); Zählung ggf. per Rückgabewert erweitern. `.psd`-Entscheidung dokumentieren. Test: `asset_compiler` auf Fixture-Verzeichnis, Ausgabeliste vergleichen.

### TLS-107 – Referenzierte Texturen werden bei jedem Mesh-Import komplett neu dekodiert und geschrieben
- **ID**: TLS-107
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetImporter/PbrMaterialImport.cpp:195-246` (`ImageCache::get/importImage`), `:701-702` (`resolveTextures` läuft vor dem "Material existiert schon"-Check)
- **Problem**: Beim Re-Import eines Meshes (oder wenn das Mesh neuer als das Asset ist) werden alle von den Materialien referenzierten Bilder mit `TextureImporter::import` neu von der Platte dekodiert (stb, bei 4K-PNG je ca. 100 ms) und neu als `.hasset` geschrieben, obwohl für nicht-primäre Materialien extra festgelegt ist, dass "nichts neu geschrieben" wird (`:733-740`). Der Kommentar `:716-717` sagt "Textures are refreshed either way", das ist aber ein reiner Kostenfaktor: das Bild hat sich in aller Regel nicht geändert.
- **Code-Auszug**:
```cpp
const ResolvedOutput out = resolveOutput(explicitPath, relativeOutputDir, derivedStem);
const MaterialTextures tex = resolveTextures(m, images, out.name, baseColorOverride, log);
```
- **Lösung**: In `importImage` für externe Dateien vor dem Dekodieren prüfen: Ziel-`.hasset` existiert, ist nicht älter als die Bilddatei und `textureSrgbOf(target) == srgb` (beides existiert bereits: `textureSrgbOf`, `importOutputsUpToDate`-Logik) und den Pfad ohne Neuimport zurückgeben. Eingebettete Bilder (kein mtime) bleiben wie bisher.
- **Erwarteter Effekt**: Schätzung: Reimport eines texturierten Meshes von "alle Texturen neu" auf "keine" (Faktor 5-50 für texturlastige Assets); `asset_compiler` nach Mesh-Änderung entsprechend.
- **Risiko / Verifikation**: Änderung von `srgb` bei bestehendem Asset muss weiterhin schreiben (Test: `test_gltf_material_import` + Fall "srgb geflippt"); `sourcePath`-Provenienz des Assets bleibt erhalten, wenn nicht geschrieben wird. Bewusste Gegenentscheidung im Kommentar prüfen (Absicht: Texturen aktuell halten).

### TLS-108 – Dateien per `istreambuf_iterator` gelesen, Dekodier-Puffer mehrfach kopiert
- **ID**: TLS-108
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `AudioImporter.cpp:88-89` (`loadOgg`), `:70-71` (`decodeWav`); `LandscapeTexGen/main.cpp:277-279, 286` (`loadImage`)
- **Problem**: `std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {})` liest Byte für Byte über den Stream-Buffer und wächst ohne bekannte Größe (mehrfaches Reallokieren, bei MSVC deutlich langsamer als ein `read`). Für mehrere zehn MB Ogg/PNG ist das messbar. `FontImporter.cpp:15-25` macht es bereits richtig (`ate` + `tellg` + `read`). `decodeWav` kopiert die von drwav allokierten Samples noch einmal in `audioData`; `loadImage` kopiert `asset->data` in `out.px`, obwohl `asset` ein lokaler Besitzer ist (`const auto asset` verhindert den Move).
- **Code-Auszug**:
```cpp
std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
```
- **Lösung**: Gemeinsame Hilfsfunktion `bool readWholeFile(const path&, std::vector<uint8_t>&)` (Größe via `file_size` oder `tellg`, ein `read`) in `ImporterCommon` und in `FontImporter`, `AudioImporter`, Landscape-Tool nutzen. `const auto asset` → `auto asset` und `out.px = std::move(asset->data)`.
- **Erwarteter Effekt**: Schätzung: 3-10x schnelleres Einlesen großer Dateien (I/O-bound bei warmem Cache); für typische Projekte klein, daher Mittel.
- **Risiko / Verifikation**: Gering. Leere Dateien weiterhin als Fehler melden; `test_assimpimport` enthält die Audio-Routing-Pins, Audio-Tests laufen lassen.

### TLS-109 – Normalen-Generierung überschreibt alle Normalen; glTF-Pfad erzeugt NaN; `generateNormals` bei Skeletal wirkungslos
- **ID**: TLS-109
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `MeshImporter.cpp:58-94, 171-172, 260-261`; `ImporterCommon.cpp:414-420`; `SkeletalMeshImporter.h:18`, `SkeletalMeshImporter.cpp` (kein Aufruf)
- **Problem**: (a) `generateNormals` setzt zuerst **alle** Normalen auf 0 und berechnet sie für das ganze Mesh neu, sobald **ein** Vertex den Platzhalter (0,0,0) hat. Ein Mesh aus mehreren Primitiven, von denen nur eines keine Normalen trägt, verliert damit die authored (z.B. harte) Normalen aller anderen. (b) `hasMissingNormals` scannt zusätzlich das gesamte Array, obwohl der Assimp-Pfad dieselbe Information bereits als Flag mitbringt (`baked.missingNormals`); beide Pfade sind uneinheitlich. (c) `appendPrimitive` ruft `glm::normalize(normalMat * n)` ohne Prüfung: eine Null-Normale aus der glTF wird NaN (Assimp-Pfad schützt das, `AssimpMeshImport.cpp:63`), und NaN wird von `hasMissingNormals` nicht als "fehlend" erkannt. (d) `SkeletalMeshImporter::ImportSettings::generateNormals` ist deklariert und wird nie gelesen; Skinned-Meshes ohne Normalen bleiben bei (0,0,0).
- **Code-Auszug**:
```cpp
// ImporterCommon.cpp:418
glm::vec3 wn = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
// MeshImporter.cpp:70
std::fill(mesh.normals.begin(), mesh.normals.end(), 0.0f);
```
- **Lösung**: `appendPrimitive` übernimmt die Prüfung aus dem Assimp-Pfad (`dot(wn,wn) > 1e-12`, sonst 0 + Flag am Rückgabewert); `generateNormals` füllt nur Vertices mit (0,0,0) (Akkumulation für alle, Schreiben nur für fehlende) und wird für Static und Skeletal aus derselben Funktion in `ImporterCommon` aufgerufen; ungenutzten Settings-Parameter entfernen oder verdrahten. `hasMissingNormals` entfällt (Flag aus `appendPrimitive`).
- **Erwarteter Effekt**: Weniger Code (eine Normalen-Logik statt drei Varianten); kein O(n)-Zusatzscan; korrektere Ergebnisse für Mischmeshes.
- **Risiko / Verifikation**: **Beobachtbares Verhalten ändert sich** (authored Normalen bleiben erhalten; Skeletal ohne Normalen bekommt erstmals welche). Absichern mit neuen Tests in `test_meshimporter` (Mesh mit zwei Primitiven, eines ohne Normalen, das andere mit harten Kanten) und `test_skeletalmeshimporter` (Rig ohne NORMAL-Attribut).

### TLS-110 – `floatLit` in `CppTypesHeaderGen` verliert Genauigkeit und ist locale-abhängig
- **ID**: TLS-110
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Tools/src/FileOps/CppTypesHeaderGen.cpp:45-53` (Nutzung `:161-178`); Kopien `:198-199, 255, 182-183`
- **Problem**: `std::ostringstream o; o << f;` verwendet Standardpräzision 6: `0.1234567f` wird als `0.123457f` ausgegeben, das ist ein anderer float als der authored Default. Außerdem nutzt `ostringstream` das globale Locale; ist es gesetzt, kann `0,5f` im generierten C++ landen (ungültiger Code). Zusätzlich: `std::vector<StructDef> pending = structs;` kopiert alle Struct-Definitionen, `reg.enums()/structs()` liefern Kopien, und `getEnum(f.typeName, def)` kopiert pro Enum-Feld die ganze `EnumDef`.
- **Code-Auszug**:
```cpp
std::string floatLit(float f)
{
    std::ostringstream o;
    o << f;
    std::string s = o.str();
```
- **Lösung**: `std::to_chars(buf, buf+32, f)` (shortest round-trip, locale-unabhängig, C++17) statt `ostringstream`; Rest wie bisher ("." anhängen falls kein `.eE`). `pending` als Indexliste/`vector<const StructDef*>`, `EnumDef` einmal in einer `unordered_map` nachschlagen.
- **Erwarteter Effekt**: Korrektheit (Default-Werte stimmen exakt); weniger Allokationen (vernachlässigbar, kalter Pfad).
- **Risiko / Verifikation**: Generierte Header ändern sich textlich (längere, exakte Literale) und lösen einmalig Rebuilds aus. `test_cpp_scaffold` prüfen/ergänzen: Round-trip-Test `strtof(floatLit(x)) == x` für Stichproben (0.1f, 1e-7f, 16777216.f, 3.4028235e38f).

### TLS-111 – Audio-Resampler wertet sin/cos pro Tap aus
- **ID**: TLS-111
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetImporter/AudioImporter.cpp:199-252`
- **Problem**: Pro Ausgabesample werden ca. `2*24/cutoff` Taps berechnet, jeder mit `std::sin` und zwei `std::cos`. Der Kommentar nennt das selbst ("a 3-minute stereo track takes seconds"). Rechnerisch: 3 min * 48 kHz = 8,6 Mio. Ausgabeframes * 48 Taps = ca. 410 Mio. Kernel-Auswertungen, einmal pro Kanal wiederverwendet, aber nicht pro Phase. Läuft nur, wenn `targetSampleRate` gesetzt ist (Default 0), also kein Standardpfad.
- **Code-Auszug**:
```cpp
const double sinc   = (d == 0.0) ? 1.0 : std::sin(kPi * d) / (kPi * d);
const double window = 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2.0 * kPi * w);
```
- **Lösung**: Wie im Kommentar vorgeschlagen eine Gewichtstabelle pro Phase (ganzzahliges Verhältnis `toRate/gcd`), nur bei Bedarf (gcd klein genug), sonst bei Fallback bleiben; Gewichte pro Phase vorab normalisieren. Alternativ `std::execution::par` über Ausgabeblöcke.
- **Erwarteter Effekt**: Schätzung: 10-30x schneller bei 44,1 -> 48 kHz (147:160 -> 160 Phasen). Nur relevant, falls der Audio-Editor `convert` interaktiv aufruft.
- **Risiko / Verifikation**: Ausgabe muss bitgleich bzw. innerhalb von 1 LSB bleiben; Tests für `AudioImporter::convert` (Rate/Kanäle, Pass-through-Identität) mit Referenzsignal. Messen, ob ein Aufrufer im UI-Thread ist (Cross-Modul).

### TLS-112 – `findParent` ist O(J²) statt `node->parent`
- **ID**: TLS-112
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Tools/src/AssetImporter/SkeletalMeshImporter.cpp:35-47, 161`
- **Problem**: Für jeden Joint werden alle Joints samt deren Kindern durchsucht, obwohl `cgltf_node::parent` direkt verfügbar ist und `jmap` ohnehin schon existiert. Bei 500 Joints ca. 250k Vergleiche, vernachlässigbar, aber unnötig komplex.
- **Code-Auszug**:
```cpp
for (cgltf_size j = 0; j < skin.joints_count; ++j) { ... for (c ...) if (node->children[c] == target) return j; }
```
- **Lösung**: `const cgltf_node* p = skin.joints[i]->parent; while (p) { auto it = jmap.find(p); if (it != jmap.end()) return it->second; p = p->parent; } return -1;` (läuft auch über Nicht-Joint-Zwischenknoten, was die heutige Schleife nicht tut).
- **Erwarteter Effekt**: Weniger Code, O(J).
- **Risiko / Verifikation**: Verhalten ändert sich bei Skeletten mit Nicht-Joint-Zwischenknoten (heute: `-1`/Root; neu: nächster Joint-Vorfahre). Das ist vermutlich korrekter, aber Test mit so einem Rig in `test_skeletalmeshimporter` ergänzen.

### TLS-113 – Mehrfach duplizierte Kleinhelfer
- **ID**: TLS-113
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `lowerExtension`/`tolower`-Transform: `AssetCompiler/main.cpp:41-43`, `AssimpMeshImport.cpp:19-25`, `AudioImporter.cpp:43-49`, `ImporterCommon.cpp:60-62, 199-201, 625-627`; `sanitizeFileStem` (`AnimationClipImporter.cpp:51-59`) gegen `sanitizeStem` (`PbrMaterialImport.cpp:48-63`); `buildJointMap` (`AnimationClipImporter.cpp:22-31`, `SkeletalMeshImporter.cpp:24-31`)
- **Problem**: Mindestens sechs Kopien derselben Lower-Case-Extension-Logik, zwei fast gleiche Dateinamen-Sanitizer (unterschiedlich: `sanitizeFileStem` trimmt nur hinten und liefert "clip", `sanitizeStem` trimmt beidseitig und liefert ""), zwei leicht verschiedene `buildJointMap`.
- **Code-Auszug**:
```cpp
std::string ext = p.extension().string();
std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
```
- **Lösung**: `Importer::lowerExtension(path)` und `Importer::asciiLower(std::string)` in `ImporterCommon.h` einführen und überall verwenden; `buildJointMap` ins gemeinsame Header (Skeletal nutzt `int32_t`, Clips `uint32_t`, Template oder `int32_t`). Die beiden Sanitizer nur zusammenlegen, wenn die Dateinamen bestehender Projekte unverändert bleiben (Re-Import-Ziele hängen daran).
- **Erwarteter Effekt**: Ca. 40 Zeilen weniger, eine Wahrheit pro Regel.
- **Risiko / Verifikation**: Sanitizer-Zusammenlegung ändert Dateinamen -> nicht ohne Migrationstest. Nur die rein mechanischen Duplikate (Extension, JointMap) sofort anfassen.

### TLS-114 – `writeAsset` liest die META-Chunks doppelt und kanonisiert den Quellpfad pro Asset
- **ID**: TLS-114
- **Kategorie**: E / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Tools/src/AssetImporter/ImporterCommon.cpp:330-355, 173-187`
- **Problem**: Bei einem Re-Import wird die Zieldatei zweimal geöffnet und gescannt: einmal in `existingUUID(target)` für die UUID, dann (bei leerer `sourceFile`) in `sourceFileOf(target)` für die Provenienz. Beides liefert `readMetaFields` (identischer Chunk). `weakly_canonical(sourceFile)` (mehrere Stat-Aufrufe) wird für jedes Asset desselben Imports wiederholt, obwohl die Quelle dieselbe ist.
- **Code-Auszug**:
```cpp
if (asset.id == HE::UUID{}) asset.id = existingUUID(target);
...
else if (asset.sourcePath.empty()) asset.sourcePath = sourceFileOf(target);
```
- **Lösung**: Einmal `MetaFields m; const bool have = readMetaFields(target, m);` und daraus UUID und `source` nehmen. Kanonisierung nur einmal pro `import*`-Aufruf (Parameter oder kleiner Cache).
- **Erwarteter Effekt**: Eine Datei-Öffnung weniger pro Re-Import-Asset; gering gegenüber TLS-101.
- **Risiko / Verifikation**: Keine Verhaltensänderung; `test_meshimporter` (Re-Import behält UUID/Source).

### TLS-115 – Vermeidbare Pixelkopien in Texturpfaden
- **ID**: TLS-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `TextureImporter.cpp:25`; `PbrMaterialImport.cpp:269`; `AssimpMaterialImport.cpp:94, 103-115`; `LandscapeTexGen/main.cpp:292-295, 441`
- **Problem**: `fromPixels` kopiert den von stb allokierten Puffer (`asset->data.assign(pixels, ...)`) und gibt ihn danach frei (Spitzenspeicher = 2x Bild; bei 8K-RGBA ca. 512 MB). `ImageCache` kopiert `img.rgba` in `tex->data`. `resized(src, w, h)` gibt bei gleicher Größe eine **Kopie** von `src` zurück und `fit()` in `packReal` ruft es für alle fünf Maps jedes Layers. Zusätzlich liest `describe()` embedded Bytes kopierend.
- **Code-Auszug**:
```cpp
asset->data.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
stbi_image_free(pixels);
```
- **Lösung**: stb-Allokator auf einen Allokator umstellen, dessen Block direkt in den Vektor übergeht ist aufwändig; praktikabel: bei `ImageCache` `PbrImage` nicht-`const` übergeben und `std::move(img.rgba)` nutzen; `resized` als `Image resized(Image&&, ...)`/Rückgabe per Referenz bei Gleichheit. Für `fromPixels` erst messen, ob Spitzenspeicher ein Problem ist.
- **Erwarteter Effekt**: Schätzung: weniger Speicherspitzen und je ein memcpy pro Bild; gering.
- **Risiko / Verifikation**: Gering; `PbrImage` wird von mehreren Materialien gelesen, daher Move nur beim letzten Verbraucher (Cache-Eintrag nach erstem `get` ist bereits "erledigt").

### TLS-116 – `ProjectManager.cpp`: gemischte Verantwortlichkeiten, doppeltes Include, ungeschützte `fs`-Aufrufe
- **ID**: TLS-116
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ProjectManager.cpp:4 und :14` (doppeltes `#include <ContentManager/HAsset.h>`); `:22-610, 852-1496` (UI-Templates, Graph-Builder, Starter-Assets); `:1690-1768, 1917`; `ProjectManager.h:7-10`
- **Problem**: 2218 Zeilen mischen Manifest-IO (`load/saveProject`, ca. 400 Zeilen), Startszene-JSON, fünf Widget-Templates, zwei Graph-Builder und das Third-Person-Starterpaket. Die throwing-Overloads `fs::create_directories(root / ...)` (`:1713-1767`) und `fs::exists(projectPath)` (`:1917`) werfen bei Rechtefehlern `filesystem_error`; der Rest der Datei nutzt konsequent `error_code`. `sceneOut` (`:1789-1791`) prüft den Schreibfehler nicht, das Manifest verweist dann auf eine nicht vorhandene Startszene. `ProjectManager.h` zieht vier Engine-Header (`DocumentTypes`, `CollisionLayers`, `AudioBusConfig`, `ProjectSettings`) in jeden Konsumenten, nur weil `ProjectData` Werte davon hält.
- **Code-Auszug**:
```cpp
fs::create_directories(root / "Content");
...
std::ofstream sceneOut(scenePath);
if (sceneOut.is_open())
    sceneOut << startupSceneJson(preset).dump(4);
```
- **Lösung**: Templates/Starter-Assets in `ProjectTemplates.cpp` (und optional Graph-Builder in `ThirdPersonTemplate.cpp`) auslagern, `ProjectManager.cpp` bleibt Manifest-IO. Doppeltes Include entfernen, `std::error_code`-Overloads verwenden, `out.good()` prüfen. Header-Includes durch Forward-Declarations nur möglich, wenn `ProjectData` die Typen nicht by-value hält (hier der Fall, also nicht einfach).
- **Erwarteter Effekt**: Kürzere Compile-Einheiten, lesbarer; keine Laufzeitwirkung (kalter Pfad).
- **Risiko / Verifikation**: Rein mechanisches Verschieben; `test_third_person_template`, `test_cpp_scaffold` laufen lassen; Ausgabe von `createNewProject` byte-identisch.

### TLS-117 – Generatoren (`landscape_tex_gen`, `matfn_gen`, `hc_codegen`): Wiederholte Manager-Konstruktion und kleine Unsauberkeiten
- **ID**: TLS-117
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `LandscapeTexGen/main.cpp:217, 239, 245, 340, 358, 654, 684`; `:447-458`; `HcCodegenCli/main.cpp:23-39, 57`
- **Problem**: `landscape_tex_gen` konstruiert pro Textur zwei Manager (Save + Readback) plus je drei in `writeArrays`, mit demselben Konstruktorpreis wie TLS-101 (ca. 40 Konstruktionen pro Lauf); der Readback-Manager ist gewollt (prüft die Datei), der Save-Manager nicht. In `packReal` wird `valid()` pro Pixel in der Schleife ausgewertet statt davor (`:449-457`). `hc_codegen/main.cpp` benutzt `std::fprintf`, ohne `<cstdio>` einzubinden (kompiliert nur durch transitive Includes); `writeIfChanged` liest die Zieldatei über `stringstream` (zwei Kopien). `matfn_gen` kopiert die `Fx`-Graphen beim `push_back` in `entries` (`:506-514`).
- **Code-Auszug**:
```cpp
for (size_t i = 0; i < n; i += 4)
{
    if (a.valid()) { ... } else { ... }
```
- **Lösung**: Einen Save-Manager pro Lauf wiederverwenden (`saveLayerTexture` bekommt `ContentManager&`), Readback bleibt frisch. Pixel-Schleife in vier Varianten vorab verzweigen oder Fallbackwerte vorher in die Images schreiben. `#include <cstdio>` ergänzen; `entries.emplace_back` mit Move.
- **Erwarteter Effekt**: Vernachlässigbar (Dev-Tools, werden selten laufen); Hygienepunkte.
- **Risiko / Verifikation**: Gering; die Tools schreiben deterministische, committete Dateien: Byte-Vergleich der Ausgabe vor/nach als Absicherung (die Tools sagen das selbst).

## Gelesen

Alle 33 Dateien des Slice wurden vollständig gelesen: ja (inklusive `ProjectManager.cpp` 2218 Zeilen in zwei Blöcken); zur Verifikation zusätzlich gezielt `ContentManager.cpp` (Konstruktor, `initDefaultAssets`, `saveAsset`), `HAsset.h` (`Reader::open`) und `vendor/cgltf.h` (`cgltf_parse_file`).

## Tools Slice 2 – MatGen, MeshGen, Packer, ShaderCompiler, WeatherSoundGen, WidgetGen

## Zusammenfassung

Der Slice (11 Dateien, ca. 3470 Zeilen) ist insgesamt sauber und gut kommentiert. Die Generatoren (`mat_gen`, `mesh_gen`, `widget_gen`, `weather_sound_gen`) laufen offline und nicht im Build-Graph, daher ist ihre Performance praktisch irrelevant. Relevant für Ladezeit und Offline-Pipeline sind vor allem `he_shadercompiler` (wird zur Laufzeit von allen Renderern über `MaterialShaderLibrary` genutzt) und `hpak_packer` (CLI-Wrapper um `HpakWriter`). Funde: 1 Hoch, 3 Mittel, 6 Niedrig. Die Hauptpunkte sind das pro Aufruf wiederholte `glslang::InitializeProcess()/FinalizeProcess()`, der fehlende Inkrementell-Cache im Packer-CLI und ein Generator, der Verdrahtungsfehler nur auf stderr meldet und trotzdem Exit-Code 0 liefert.

## Funde

### TLS-201 – glslang-Prozess wird bei jedem Shader-Compile neu initialisiert und abgebaut
- **ID**: TLS-201
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:194-204, 206-258, 260-304, 306-329, 331-354
- **Problem**: Jede der fünf Einstiegsfunktionen ruft `glslang::InitializeProcess()` und am Ende `FinalizeProcess()`. glslang baut dabei seine Built-in-Symboltabellen (Funktions- und Variablentabellen je Stage/Profil) komplett neu auf. Diese Initialisierung ist der teuerste Teil kleiner Shader-Compiles. Der Kommentar "glslang's process init/teardown is global, not re-entrant — serialise it" begründet nur den Mutex, nicht, warum pro Aufruf neu initialisiert werden muss. glslang unterstützt Init einmal pro Prozess (intern referenzgezählt). Die Funktionen laufen zur Laufzeit, wenn `MaterialShaderLibrary` ein Material ohne vorkompilierte Variante cross-kompiliert (Fallback), sowie für Pinned-Varianten von SSR/Decals/Clustered-GL. Jeder Material-/Pass-Wechsel beim Laden zahlt also die Initialisierung erneut.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lock(g_glslangMutex);
glslang::InitializeProcess();
if (glslToSpirv(glsl, stage, r.spirv, r.log))
    r.ok = spirvToSource(r.spirv, target, r.source, r.log);
glslang::FinalizeProcess();
```
- **Lösung**: Einmalige Initialisierung per Function-Local-Static-Guard, ohne Finalize pro Aufruf:
  1. `struct GlslangProcess { GlslangProcess(){ glslang::InitializeProcess(); } ~GlslangProcess(){ glslang::FinalizeProcess(); } };`
  2. In einem gemeinsamen Helfer (siehe TLS-202) `static GlslangProcess proc;` anlegen, danach Mutex nehmen (der Mutex bleibt für `TShader`/`TProgram`-Nutzung vorerst bestehen, so ändert sich das Verhalten nicht).
  3. Optional später prüfen, ob der Mutex für das reine Parsen entfallen kann (glslang ist nach Init für getrennte TShader/TProgram-Instanzen thread-safe). Das ist ein eigener Schritt und nicht Teil dieses Fundes.
- **Erwarteter Effekt**: Schätzung: pro Compile ein- bis zweistellige Millisekunden gespart (Symboltabellen-Aufbau). Bei vielen Material-Varianten und Pinned-Shadern beim Laden ergibt das spürbare Ladezeit. Ohne Messung nicht belegt.
- **Risiko / Verifikation**: Statisches Destruktor-Timing beim Prozessende (Finalize nach Renderer-Teardown ist unkritisch, da kein glslang-Objekt überlebt). Offline-Tools, die `compile` aus mehreren Threads nutzen, bleiben durch den Mutex serialisiert. Messen mit `compileMany` des Spike (`spike_main.cpp`) in einer Schleife mit 100 Aufrufen vor/nach der Änderung; Material-Tests (`tests/` mit Shaderc, z. B. `grep -l shaderc tests/*.cpp`) laufen lassen.

### TLS-202 – Fünf nahezu identische Compile-Pipelines mit kopiertem Rahmen
- **ID**: TLS-202
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:116-176, 194-354
- **Problem**: `compile`, `compileMslPinned`, `compileHlslPinned`, `compileGlslPinned` und `compileMany` wiederholen jeweils: Mutex, `InitializeProcess`, `glslToSpirv`, `try { SPIRV-Cross } catch`, `FinalizeProcess`, `reportShaderResult`. Zusätzlich ist die Stage-zu-`spv::ExecutionModel`-Umrechnung (Zeilen 235-237 und 277-279) kopiert, und die MSL-Optionen (`platform = macOS`, `set_msl_version(2, 3)`) stehen doppelt (Zeilen 132-135 und 219-221). `spirvToSource` ist ein `switch` mit Sonderfall `Glsl430` (Zeile 124-128), der `emitGlsl430(spirv, {}, Stage::Fragment)` mit Dummy-Stage aufruft, und die gepinnten Varianten umgehen `spirvToSource` komplett. Ein neues Target (Pin/Option) braucht also vier Stellen, und genau diese Fehlerklasse hat laut `ShaderCompilerStub.cpp:18-23` schon einmal drei Plattformen unbuildbar gemacht.
- **Code-Auszug**:
```cpp
b.stage = (p.stage == Stage::Vertex)   ? spv::ExecutionModelVertex
        : (p.stage == Stage::Fragment) ? spv::ExecutionModelFragment
                                       : spv::ExecutionModelGLCompute;
```
- **Lösung**:
  1. Helfer `spv::ExecutionModel toExecModel(Stage)` neben `toEsh`.
  2. Helfer `template<class Emit> Result runPipeline(const char* what, const std::string& glsl, Stage, Emit&& emit)`: hält Mutex und (nach TLS-201) Process-Guard, ruft `glslToSpirv`, führt `emit(Result&)` in `try/catch` aus (Prefix für die Log-Zeile als Parameter), loggt via `reportShaderResult`.
  3. `compile` = `runPipeline(..., [&](Result& r){ r.ok = spirvToSource(...); })`; die gepinnten Varianten übergeben nur ihren Emit-Lambda. `compileMany` bleibt eigen (mehrere Targets), nutzt aber denselben Guard.
  4. `makeMslOptions(const MslOptions*)` für die gemeinsamen MSL-Grundoptionen.
- **Erwarteter Effekt**: ca. 60-80 Zeilen weniger, eine Stelle für Init/Locking/Fehlerbehandlung. Keine Laufzeitänderung (Schätzung).
- **Risiko / Verifikation**: Reines Refactoring; die Log-Texte ("SPIRV-Cross(MSL pinned): ...") sollten unverändert bleiben, falls Tests darauf matchen (`grep -rn "SPIRV-Cross(" tests/`). Gleichzeitig `ShaderCompilerStub.cpp` unverändert lassen; die Header-Signaturen ändern sich nicht.

### TLS-203 – Packer-CLI nutzt den Inkrementell-Cache des HpakWriter nicht
- **ID**: TLS-203
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Tools/src/Packer/main.cpp:110-126; Gegenstück src/HE_Core/include/Hpak/HpakWriter.h:90-108
- **Problem**: `HpakWriter::addDirectories` akzeptiert einen `Hpak::IncrementalCache`, mit dem unveränderte Einträge (gleicher Hash des umgeschriebenen Blobs) ohne erneutes Komprimieren/Verschlüsseln übernommen werden. Laut Kommentar in `HpakWriter.cpp:1047` ist genau dieser Durchlauf (Rewrite, Cook, Kompression, Verschlüsselung; dazu BC7/ASTC-Encoding der Texturen) der teure Teil. Der Editor-Export (`ProjectExporter.cpp`) nutzt den Cache, das CLI nicht: `addDirectories(roots, settings)` ohne `progress` und `cache`. Jeder CI-/Build-Skript-Lauf von `hpak_packer` packt also immer von null, auch wenn sich ein Asset geändert hat. Es gibt zudem keine Fortschrittsausgabe, bei großen Projekten läuft das Tool minutenlang stumm.
- **Code-Auszug**:
```cpp
HpakWriter packer;
const int added = packer.addDirectories(roots, settings);
```
- **Lösung**:
  1. Option `--cache <file>`: Cache laden (falls vorhanden), an `addDirectories` übergeben, nach dem `write` aus `packer.entryInfos()` wieder speichern. Die Lade-/Speicherroutinen aus `ProjectExporter.cpp` in eine gemeinsame Funktion in HE_Core ziehen, statt sie im Tool zu duplizieren.
  2. Optional `--progress` mit dem `AddProgressFn`-Callback (stderr, nur bei TTY oder Flag).
- **Erwarteter Effekt**: Bei Wiederholungsläufen ohne oder mit wenigen Änderungen Packzeit von "voll" auf "Hash plus Kopie" (Schätzung; Faktor hängt stark vom Texturanteil ab, messen).
- **Risiko / Verifikation**: Der Header verlangt, dass der Aufrufer nur einen zu den `PackSettings` passenden Cache liefert (Codec, Verschlüsselung, Schlüssel). Der Cache braucht daher einen Settings-Fingerprint; ohne diesen darf kein Cache geladen werden. Test: gleicher Pack mit und ohne Cache muss byte-identisch sein, plus Test mit geänderten `--codec`/`--secret` (Cache muss verworfen werden). Es existiert bereits Testabdeckung für den Cache im Editor-Pfad (`grep -n IncrementalCache tests/*.cpp`, nicht geprüft).

### TLS-204 – widget_gen meldet Verdrahtungsfehler nur auf stderr und endet mit Exit-Code 0
- **ID**: TLS-204
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WidgetGen/main.cpp:701-733, 806, 840, 847-879
- **Problem**: Die `chain`-Lambda in `dialogGraph` liefert `bool wired`, beide Aufrufstellen verwerfen den Rückgabewert. Schlägt `g.connect` fehl, steht nur "could not wire" auf stderr, `dialogFrame` schreibt trotzdem einen Dialog, dessen Buttons nichts tun, und `main` endet mit 0, weil `saveAsset` gelingt. `mat_gen` (Zeile 20-31) hat dagegen ausdrücklich die Philosophie "Failing, not warning" und bricht ab. Das Generator-Ergebnis wird committet, ein stiller Fehler würde in jedem Projekt landen. Die Kommentar-Absicht im Code ("shipping a dialog whose buttons do nothing") wird vom Code selbst nicht eingelöst.
- **Code-Auszug**:
```cpp
chain(okButton,     "Confirmed", 0.0f);
chain(cancelButton, "Cancelled", 200.0f);
return g;
```
- **Lösung**: `dialogGraph` gibt `bool` oder `std::optional<Graph>` zurück (oder nimmt `bool& ok`); `dialogFrame` reicht den Fehler durch, `main` überspringt den Eintrag und zählt ihn als Fehler (wie `mat_gen`: `if (!good) { ...; continue; }`). Skizze: `bool ok = chain(...) & chain(...); if (!ok) failedGraph = true;`.
- **Erwarteter Effekt**: Kein Laufzeiteffekt; verhindert still defekte Engine-Widgets (Korrektheit).
- **Risiko / Verifikation**: Heute verdrahten beide Ketten erfolgreich, das beobachtbare Ergebnis des Normalfalls bleibt byte-identisch. Test: vorhandener Widget-Library-Test (`grep -ln DialogFrame tests/`), nicht verifiziert.

### TLS-205 – hpak_packer löscht die alte Ausgabedatei, bevor er weiß, ob die neue geschrieben werden kann
- **ID**: TLS-205
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Tools/src/Packer/main.cpp:130-137; src/HE_Core/src/Hpak/HpakWriter.cpp:1204-1207
- **Problem**: Das Tool entfernt `outputFile` vor `write()`, und `write()` öffnet ohnehin mit `std::ios::trunc`. Das `exists`+`remove` ist redundant. Schlimmer: Bricht `write` mittendrin ab (Platte voll, Prozess beendet), bleibt weder das alte noch ein gültiges neues Archiv zurück. In einer Build-Pipeline, die dasselbe Ziel wiederverwendet, ist das ein Robustheitsproblem (Teil-Hpak, der erst zur Laufzeit auffällt).
- **Code-Auszug**:
```cpp
if (std::filesystem::exists(outputFile))
    std::filesystem::remove(outputFile);
if (!packer.write(outputFile)) { ... }
```
- **Lösung**: In ein Temp-File im selben Verzeichnis schreiben (`outputFile + ".tmp"`), bei Erfolg `std::filesystem::rename` (atomar auf demselben Dateisystem; unter Windows `std::filesystem::rename` ersetzt vorhandene Dateien ebenfalls), bei Fehler Temp löschen. Das `exists`/`remove` entfällt.
- **Erwarteter Effekt**: Keine Performance, aber Robustheit der Offline-Pipeline; zwei Syscalls weniger.
- **Risiko / Verifikation**: Kleiner Unterschied: Ein vorhandenes, geöffnetes Zielarchiv (gemappt vom laufenden Spiel) ließe sich unter Windows nicht mehr ersetzen. Das gilt heute aber ebenso für `remove`. Test: Packer-Test (falls vorhanden) mit schreibgeschütztem Zielverzeichnis, erwartetes Verhalten: Exit 1, altes Archiv unverändert.

### TLS-206 – mat_gen: `longestChain` ist O(N·L) mit `std::map` und Mehrfach-Push
- **ID**: TLS-206
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Tools/src/MatGen/main.cpp:204-233
- **Problem**: Für jeden Knoten wird die komplette Link-Liste durchsucht (`for (const MatGraphLink& l : g.links) if (l.dstNode != id) continue;`), und ein Knoten, dessen Vorgänger noch nicht in `memo` liegt, wird bei jedem Wiederbesuch erneut gescannt. Ein Knoten kann außerdem mehrfach auf den Stack gelangen (Mehrfach-Fan-out). `std::map<int,int>` mit `count` plus `find` ist unnötig teuer. Bei Zyklen im Graph liefe die Schleife endlos (nur `connect` verhindert das; ob es Zyklen ablehnt, wurde nicht geprüft). Das Water-Material hat grob 1000-2000 Knoten; offline, also unkritisch, aber die Funktion kann einfacher.
- **Code-Auszug**:
```cpp
for (const MatGraphLink& l : g.links)
{
    if (l.dstNode != id) continue;
    auto it = memo.find(l.srcNode);
    if (it == memo.end()) { stack.push_back(l.srcNode); ready = false; }
```
- **Lösung**: Einmal Eingangs-Adjazenz bauen (`std::unordered_map<int, std::vector<int>> in;` oder Vektor nach Knotenindex), `memo` als `std::unordered_map`/Vektor; oder topologisch per Kahn über die Knoten und ein Durchlauf über die Links. Optional einen Zähler als Zyklusschutz (`if (++guard > nodes*links) break`), falls `connect` Zyklen zulässt.
- **Erwarteter Effekt**: Von O(N·L) auf O(N+L); in der Praxis Millisekunden, nur Lesbarkeit und Robustheit relevant (Schätzung).
- **Risiko / Verifikation**: Ergebnis (`best`) muss identisch bleiben: Vergleich alt/neu auf `waterMaterial()` (Zeile 854 gibt "chain N" aus). Wert vorher notieren.

### TLS-207 – mat_gen: Doppelte Codegen- und Persistenzläufe pro Material
- **ID**: TLS-207
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Tools/src/MatGen/main.cpp:842-884
- **Problem**: Pro Material wird `generateFragment(e.mb.g)` einmal in `main` zur Vorprüfung ausgeführt, ein zweites Mal beim Zurücklesen durch `regenerateMaterialFromGraph` im frischen `ContentManager`. Außerdem wird zweimal gespeichert (Pass 1 Graph, Pass 2 gebackene Felder) und zwei `ContentManager`-Instanzen gebaut. Die Zwei-Pass-Struktur ist laut Dateikopf (Zeile 17-23) gewollt, weil der Packer nur die gebackenen Felder ausliefert. Der Mehraufwand ist bei einem Material offline vernachlässigbar. Der Fund ist nur relevant, falls die Liste wächst (`entries` ist als "append only" gebaut).
- **Code-Auszug**:
```cpp
{ ContentManager cm(outDir); written = cm.saveAsset(a); }
if (written) { ContentManager cm(outDir); ... cm.saveAsset(*m); }
```
- **Lösung**: Keine Änderung nötig, solange es bei einem Material bleibt. Bei Wachstum: Pass 1 überspringen und die gebackenen Felder direkt aus `gen` ins `MaterialAsset` schreiben (nur falls die Felder öffentlich und identisch zu `regenerateMaterialFromGraph` zu füllen sind; das ist riskant, weil der Editor-Ladepfad der Maßstab sein soll).
- **Erwarteter Effekt**: Vernachlässigbar (unter einer Sekunde, Schätzung).
- **Risiko / Verifikation**: Bewusst nicht ändern; die Doppelpass-Absicht ist das Sicherheitsnetz für Byte-Gleichheit mit dem Editor. Als "keine Aktion" dokumentiert.

### TLS-208 – weather_sound_gen: Statistik und Peak-Normalisierung vierfach neu geschrieben
- **ID**: TLS-208
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WeatherSoundGen/main.cpp:192-205, 326-334, 382-387, 399-400, 500-503
- **Problem**: RMS und Peak werden an vier Stellen von Hand berechnet: `finish` (Zeile 192-205), `makeStorm` (Peak-Schleife Zeile 331-333), `makeThunder` (`peakOf`-Lambda, Zeile 382-386, und Normalisierung 399-400) und `main` (Zeile 500-502). `makeStorm` dupliziert die "wenn Peak über Grenze, herunterskalieren"-Logik aus `finish` (Zeile 203-204 gegenüber 333). Zusätzlich rechnet `lfo` pro Sample mehrfach `std::sin` (bis zu 3 Aufrufe je Modulationsterm, in `circular` dreimal durchlaufen), und `svfG` ruft `std::tan` pro Sample bei konstanter Frequenz nicht, aber bei `makeWind`/`makeStorm` pro Sample (mit sich ändernder Sweep-Frequenz, legitim). Die Rechenzeit liegt bei wenigen hunderttausend Samples im Millisekundenbereich, daher nur ein Vereinfachungsthema.
- **Code-Auszug**:
```cpp
double peak = 0.0;
for (double v : x) peak = std::max(peak, std::fabs(v));
if (peak > 0.95) for (double& v : x) v *= 0.95 / peak;
```
- **Lösung**: Zwei kleine Helfer: `double peakOf(const std::vector<double>&)` (aus dem Thunder-Lambda herausziehen) und `void limitPeak(std::vector<double>&, double ceiling)`; `finish` und `makeStorm` nutzen sie, `main` nutzt `peakOf` plus eine `rmsOf`-Funktion. Die Gleitkomma-Reihenfolge der Operationen darf nicht geändert werden (siehe Dateikopf Zeile 39-43: Byte-Gleichheit auf derselben Plattform).
- **Erwarteter Effekt**: Ca. 15-20 Zeilen weniger, keine Laufzeitänderung.
- **Risiko / Verifikation**: Summationsreihenfolge für RMS bleibt gleich (vorwärts, `double`), dann sind die erzeugten Bytes identisch. Prüfen durch Regenerieren in ein Temp-Verzeichnis und Byte-Vergleich mit dem vorherigen Lauf auf derselben Plattform; `tests/test_engine_weather_sounds.cpp` prüft die Naht der committeten Dateien.

### TLS-209 – widget_gen: Magic-Numbers für Anker-Presets und Out-Parameter-Konstruktion
- **ID**: TLS-209
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WidgetGen/main.cpp:99, 466, 512, 567, 605, 763, 160-189 und 202-268
- **Problem**: Anker-Presets stehen als nackte Zahlen im Code (`uiSetAnchorPreset(e, 3)`, `14`, `11`, `4`), nur durch Zeilenkommentare erklärt, während `kUIAnchorFill` eine benannte Konstante hat (Zeile 91). `formRowShell` liefert drei IDs über `int&`-Out-Parameter und jede der drei Form-Row-Funktionen deklariert `int label = 0, help = 0, line = 0;` nur zum Durchreichen. Ein kleines Struct (`struct FormRow { Comp c; int label, help, line; };`) wäre klarer. Außerdem steht Zeile 270 ("A heading with a rule under it...") als verwaister Kommentar über dem Divider-Block, gehört aber zu `sectionHeader`.
- **Code-Auszug**:
```cpp
uiSetAnchorPreset(e, 14);            // right edge, stretched down
...
Comp formRowShell(int& labelId, int& helpId, int& lineId)
```
- **Lösung**: Falls `UIWidget` keine benannten Konstanten für die Presets hat, lokal `constexpr int kAnchorTopStrip = 3, kAnchorRightStretch = 14, kAnchorBottomStrip = 11, kAnchorMiddleLeft = 4;` anlegen (die Werte müssen zur Preset-Tabelle in `UIElements.h` passen, vor der Änderung nachsehen). `FormRow`-Struct statt Out-Parameter. Kommentar an `sectionHeader` verschieben.
- **Erwarteter Effekt**: Lesbarkeit, keine Laufzeitwirkung.
- **Risiko / Verifikation**: Ausgabedateien müssen byte-identisch bleiben (Regenerieren und Diff gegen `EditorDeps/EngineContent/Widgets`).

### TLS-210 – compileMany kopiert das SPIR-V pro Ziel
- **ID**: TLS-210
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:341-348; Header ShaderCompiler.h:179-185, 273-279
- **Problem**: `r.spirv = mr.spirv;` kopiert für jedes angeforderte Target den vollständigen SPIR-V-Vektor, obwohl `MultiResult::spirv` dieselben Worte schon hält. Die Header-Doku von `Result::spirv` ("populated for every target") verlangt es so, aber `MultiResult` dupliziert die Daten damit n-fach. Bei vier Targets und typisch einigen zehn bis hundert KB pro Shader eine unnötige Heap-Kopie. Außerdem werden die Ziele sequenziell erzeugt, obwohl SPIRV-Cross-Compiler unabhängige Objekte sind (aber siehe TLS-201: der Mutex hält ohnehin alles seriell).
- **Code-Auszug**:
```cpp
Result r;
r.spirv = mr.spirv;
r.ok = spirvToSource(mr.spirv, t, r.source, r.log);
```
- **Lösung**: Prüfen, ob irgendein Aufrufer `perTarget[i].spirv` liest (`grep -rn "perTarget" src tests`). Falls nicht, Kopie weglassen und im Header dokumentieren "nur in `MultiResult::spirv`". Sonst unverändert lassen.
- **Erwarteter Effekt**: Kleine Einsparung pro Material-Cook (Schätzung, nur offline/Cook relevant).
- **Risiko / Verifikation**: Öffentliche Verhaltensänderung der API; nur nach Aufrufer-Check. Unter `HE_ENABLE_SHADERC=OFF` liefert der Stub schon kein SPIR-V.

## Gelesen

Alle elf Dateien des Slice 2 vollständig gelesen: ja (`MatGen/main.cpp`, `MeshGen/main.cpp`, `Packer/HpakWriter.h`, `Packer/KeyDerivation.h`, `Packer/main.cpp`, `ShaderCompiler/ShaderCompiler.cpp`, `.h`, `ShaderCompilerStub.cpp`, `spike_main.cpp`, `WeatherSoundGen/main.cpp`, `WidgetGen/main.cpp`). `src/HE_Tools/src/MeshGen/main.cpp` hat keine Funde mit Gewicht: die Kopie `Mesh mesh = meshConst;` (Zeile 369) und fehlendes `reserve` in `Mesh::addVertex` sind offline irrelevant.
