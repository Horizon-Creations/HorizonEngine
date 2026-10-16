# Rendering Slice 9 – Shared Rendering-Header/-Sources (CommandBuffer, Foliage/Frustum/GI/Light-Packing, RenderWorld/-Object/-Sorter, Sky*, VulkanRenderer.h, VulkanShaderManager, MaterialShaderBake)

## Zusammenfassung

Der Slice (50 Dateien, ca. 9,5k Zeilen; davon 2568 Zeilen `SkyShaderSource.h` und 1566 Zeilen `VulkanRenderer.h`) ist insgesamt gut kommentiert und bereits stark von Cross-Backend-Duplikation befreit (LightPacking, ClipSpace, GIProbeGrid, SkyFrameParams). Die relevanten Befunde liegen bei der Draw-Call-Aufzeichnung (tiefe Kopien der Instanzlisten), beim pro Frame neu aufgebauten Cluster-Licht-Grid, bei GPU-seitig wiederholt berechneten Sky-Konstanten (Atmosphere, Aurora) und bei kleineren Per-Frame-Allokationen im Vulkan-Header (String-Keys, `unordered_map` fuer TAA). Es gibt 3 Funde mit Schwere Hoch, 9 Mittel und 11 Niedrig (gesamt 23). Alle Zeilenangaben wurden gegen den Stand der Dateien geprueft; GPU-Kosten (Sky-Shader) und Rebake-Haeufigkeit sind ohne Messdaten als "messen" markiert. Backend-Rendererdateien (`*Renderer.cpp/.mm`) gehoeren nicht zu diesem Slice, wurden aber gezielt per Grep zur Verifikation von Aufrufstellen angesehen.

## Funde

### REN-901 – CommandBuffer::recordDraw/recordSkinnedDraw kopieren DrawCall samt Instanz- und Bone-Vektoren
- **ID**: REN-901
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/CommandBuffer.cpp:15-23`, `include/HorizonRendering/CommandBuffer.h:52,57,67,76-77`; Aufrufer `src/HE_Rendering/src/RenderPass.cpp:89,117,198,218,252`
- **Problem**: `recordDraw(const DrawCall&)` macht `push_back(call)` – eine tiefe Kopie von `instanceTransforms` (bis `kMaxInstancesPerDraw` = 65536 `mat4` = 4 MiB), `paramOverride` und bei `SkinnedDrawCall` von `boneMatrices`. Der Aufrufer (`RenderPass.cpp:137-140`) baut die Liste vorher selbst auf (`reserve` + `appendInstances`), kopiert sie dann in den CommandBuffer und zerstoert das Original gleich danach. Bei Foliage-Batches entstehen so pro Frame zwei volle Durchlaeufe ueber alle Matrizen plus eine Heap-Allokation pro Draw. `reset()` (`CommandBuffer.cpp:5`) ruft `clear()` und gibt damit auch die Vektor-Kapazitaeten aller DrawCalls frei, es wird nichts ueber Frames wiederverwendet.
- **Code-Auszug**:
```cpp
void CommandBuffer::recordDraw(const DrawCall& call) { m_drawCalls.push_back(call); }
void CommandBuffer::recordSkinnedDraw(const SkinnedDrawCall& call) { m_skinnedDrawCalls.push_back(call); }
```
- **Lösung**: (1) Move-Overloads `recordDraw(DrawCall&&)` / `recordSkinnedDraw(SkinnedDrawCall&&)` ergaenzen und in `RenderPass.cpp` mit `std::move(dc)` bzw. `std::move(chunk)` aufrufen (der Lambda `recordRun` muss dazu den letzten Abschnitt bewegen statt kopieren). (2) Optional: Chunk-Pfad `RenderPass.cpp:165` direkt per `emplace_back` in den CommandBuffer bauen und `instanceTransforms` dort `assign`en, statt ueber `chunk` zu gehen. (3) Optional spaeter: `instanceTransforms` aus einer pro Frame wiederverwendeten Arena (ein grosser `std::vector<glm::mat4>` im CommandBuffer + `{first,count}` im DrawCall) statt pro Draw ein Vektor; das aendert die Backend-Schnittstelle und ist ein eigener Schritt.
- **Erwarteter Effekt**: Schaetzung: bei grossen Foliage-Batches (10^4-10^5 Instanzen) fallen ein voller Matrix-Kopiervorgang (64 B x N) und eine Allokation pro Draw weg; bei Szenen ohne Instancing vernachlaessigbar. Messen mit `HE_PROFILE_SCOPE` um `GeometryPass::execute`.
- **Risiko / Verifikation**: Gering, solange die Backends den CommandBuffer nur lesen. `tests/test_skeletalmeshcomponent.cpp:52-77` ruft `recordDraw(dc)` mit einem Lvalue auf; die `const&`-Overloads muessen bleiben. Sonst vorhandene Render-Graph-/Foliage-Tests (`tests/test_foliage_cluster.cpp`) laufen lassen.

### REN-902 – Section-Expansion kopiert die komplette Instanzliste pro Material-Slot
- **ID**: REN-902
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/RenderPass.cpp:98-118` (`DrawCall sd = d;`), `:209` (`SkinnedDrawCall sd = dc;`); Struktur in `include/HorizonRendering/CommandBuffer.h:15-61`
- **Problem**: Ein Multi-Section-Mesh erzeugt pro Slot einen `DrawCall` per Wertkopie des Basis-DrawCalls – inklusive `instanceTransforms` und `paramOverride`. Ein geclustertes Mesh mit 3 Slots und N Instanzen kopiert die Liste dreimal (plus die Kopie aus REN-901). Fuer jeden Draw desselben Mesh-Runs sind die Instanzdaten aber identisch. Bei skinned Meshes wird zusaetzlich `boneMatrices` pro Section kopiert (`RenderPass.cpp:209`).
- **Code-Auszug**:
```cpp
DrawCall sd        = d;
sd.indexOffset     = sec.indexOffset;
...
outCmds.recordDraw(sd);
```
- **Lösung**: Instanz-/Bone-Daten als geteilte, unveraenderliche Referenz fuehren: `std::shared_ptr<const std::vector<glm::mat4>> instances` (oder `{const mat4* ptr; uint32_t count}` in einer CommandBuffer-eigenen Arena). Dann kopiert `sd = d` nur Zeiger. Kleinere Zwischenloesung ohne Schnittstellenaenderung: nur den ersten Slot kopieren/moven und fuer weitere Slots die Liste ebenfalls per `=` zuweisen (kein Gewinn), daher ist der Zeiger-/Arena-Weg der eigentliche Fix. Die Backends lesen `dc.instanceTransforms` an vielen Stellen (`D3D11/D3D12/Vulkan/GL/Metal`), die Umstellung betrifft alle fuenf Backends.
- **Erwarteter Effekt**: Schaetzung: Faktor = Anzahl Slots weniger Matrix-Kopien bei Multi-Section-Foliage/Instancing; fuer Einzel-Section-Meshes keine Aenderung.
- **Risiko / Verifikation**: Mittel (Backend-Schnittstelle). Absichern: `tests/test_foliage_cluster.cpp`, Section-Tests (`grep -l sectionIndex tests/`), visuelle Gleichheit per Headless-Dump auf mindestens GL und Vulkan.

### REN-903 – BuildClusterLights legt pro Frame 3456 Vektoren an und sammelt Indizes per `push_back`
- **ID**: REN-903
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/LightPacking.cpp:131-245` (besonders `:139-146`, `:224-227`, `:231-237`)
- **Problem**: `std::vector<std::vector<uint32_t>> cells(kClusterCount)` (16x9x24 = 3456 Vektoren, ~83 KB, alle zero-initialisiert) wird in jedem Aufruf angelegt, und jede beruehrte Zelle wird per `push_back` ohne `reserve` gefuellt (bis `kMaxClusterIndices` = 65536 Eintraege, also viele einzelne kleine Allokationen/Reallokationen). Danach wird alles nochmals in `out.indices` kopiert. Der Aufruf passiert pro Frame in jedem Backend (`OpenGLRenderer.cpp:11416`, `D3D11Renderer.cpp:6332`, `D3D12Renderer.cpp:10318`, `VulkanRenderer.cpp:7875`, `MetalRenderer.mm:14920`). Zusaetzlich `glm::inverse(rw.camera.view)` (Z. 141) nur um die Kamera-Vorwaertsrichtung zu holen.
- **Code-Auszug**:
```cpp
std::vector<std::vector<uint32_t>> cells(kClusterCount);
...
cells[(z * kClusterGridY + y) * kClusterGridX + x].push_back(li);
...
out.indices.insert(out.indices.end(), cells[cIdx].begin(), cells[cIdx].end());
```
- **Lösung**: Zwei-Pass-Counting-Sort statt Vektor-pro-Zelle: Pass 1 zaehlt pro Zelle (`std::array<uint32_t,kClusterCount> counts`) waehrend die Lichter durchlaufen werden und merkt sich pro Licht `{z0,z1,y0,y1,x0,x1,li}` (max. 256 Eintraege, Stack); Praefixsumme liefert `grid`-Offsets; Pass 2 schreibt `indices[offset[cell]++] = li`. Keine Heap-Allokation ausser `out.*`; Reihenfolge der Indizes je Zelle (aufsteigend nach Lichtindex) bleibt identisch, weil Lichter in derselben Reihenfolge eingetragen werden. Die Forward-Richtung kann aus der View-Matrix direkt gelesen werden: `-glm::vec3(view[0][2], view[1][2], view[2][2])` ist die dritte Zeile der Rotation und ist gleich `-normalize(inverse(view)[2].xyz)` bei orthonormaler View. Das ist bit-identisch nicht garantiert; Test pruefen.
- **Erwarteter Effekt**: Schaetzung: einige Dutzend bis einige hundert Allokationen pro Frame und Backend weniger, ca. 83 KB weniger Speicher-Churn. Gewinn bei 0-2 Lichtern gering, bei vielen Punktlichtern spuerbar. Messen.
- **Risiko / Verifikation**: Gering-mittel: Ausgabe ist vertragsrelevant (Shader indexieren positionell). Es gibt Tests zu BuildClusterLights (`grep -l BuildClusterLights tests/`); neuen Vergleichstest "alt vs. neu" mit zufaelligen Lichtern schreiben, bevor die Implementierung ersetzt wird. Die `inverse`-Aenderung separat und nur, wenn der Test bit-gleich bleibt.

### REN-904 – Sky: Atmosphaerenintegral wird pro Pixel gerechnet, obwohl es nur von der Richtung und der Sonne abhaengt
- **ID**: REN-904
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:2278-2330` (`atmoScatter`), `:2331-2427` (`skyColor`), Aufrufer `:2179`, `:2166`, `:471` (`overcastDeck`); CPU-Spiegel `include/HorizonRendering/SkyEnvBake.h:47-97`
- **Problem**: `atmoScatter` macht pro Pixel 12 View-Samples mit je 2 `exp` plus einem 5-Sample-Sonnenmarsch mit je 3 `exp` plus `exp(-tau)`, also grob 200 `exp` und mehrere Kugelschnitte je Fragment. `skyColor` wird zusaetzlich in `overcastDeck` (pro Cloud-Pixel unter Overcast), im Cloud-Prepass (`:2166`) und in der Scene-IBL-Variante aufgerufen. Das Ergebnis haengt nur von `dir` (2 Winkel) und `sunDir` (1 Winkel: Sonnenhoehe, da der Aufpunkt `ro` fix ist) ab, aendert sich also nur wenn die Sonne wandert.
- **Code-Auszug**:
```glsl
for (int i = 0; i < 12; ++i) { ... float dR = exp(-hgt / HR) * ds; ... for (int j = 0; j < 5; ++j) { ... exp(-hq / HR) ... } ... exp(-tau) ... }
```
- **Lösung**: Transmittance/Sky-View-LUT nach Hillaire: eine kleine Textur (z. B. 128x64, Azimut-unabhaengig, da nur Hoehenwinkel und Sonnen-Azimutdifferenz zaehlen) wird nur neu gerendert wenn sich `sunDir` aendert (derselbe Trigger wie der IBL-Rebake), `skyColor` sampelt dann. Gleicher Pfad fuer GL/Vulkan/D3D (geteilter Shader) und Metal (Spiegel in `MetalRenderer.mm`). Das ist ein Look-aendernder Eingriff (LUT-Interpolation, Horizont-Clamp), deshalb eigener Schritt mit visuellem A/B.
- **Erwarteter Effekt**: Schaetzung: grosse Entlastung der GPU-ALU bei Vollbild-Sky (Faktor mehrere; die Wolken-/Stern-Marsche dominieren aber im Nachtbetrieb). Messen mit GPU-Timer (`kGpuTimerRing`) bei 1080p/4K, Wolken aus.
- **Risiko / Verifikation**: Hoch fuer Bildgleichheit. `tests/test_sky_shader.cpp` und der Hash-Pin in `tests/test_culling.cpp` (IBL-Bake) muessen bewusst neu gepinnt werden. Nicht ohne Messdaten angehen; vorher pruefen, ob die Sky-Pass-Zeit ueberhaupt relevant ist (Low-Res-Cloud-Pfad existiert bereits).

### REN-905 – Aurora: pro-Kurtinen-Parameter werden pro Pixel neu aus ~27 Hashes berechnet
- **ID**: REN-905
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:1682-1731` (22 Kurtinen x `auroraRnd(fk, s)`), `:1592-1595`
- **Problem**: `auroraRnd(k, s)` ist eine reine Funktion von `k` und `s` (`starHash(vec3(k*0.7331+1.13, s*1.9137+7.71, 19.37))`) – unabhaengig vom Pixel. Trotzdem werden pro Fragment und Kurtine ca. 27 Hashes sowie 6 `sin`/`cos` fuer Layout/Heading/Meander-Parameter berechnet, bevor die drei `auroraSlab`-Tests (`:1739-1741`) die meisten Kurtinen mit `continue` verwerfen. Das sind rund 600 Hashes pro Fragment bei aktiver Aurora (`intensity > 0`, nachts), allein fuer Parameter.
- **Code-Auszug**:
```glsl
for (int k = 0; k < 22; ++k) {
    float rr = (fk + auroraRnd(fk, 0.0)) / 22.0; ... float a1 = scl * (0.55 + 0.85 * auroraRnd(fk, 5.0)); ... (insgesamt auroraRnd(fk,0..26))
```
- **Lösung**: Parameter einmalig auf der CPU (oder pro Draw im Vertex-Shader) berechnen und als Array (`vec4 curtain[22*7]`) im Sky-UBO bzw. als Konstanten-Array ablegen. `SkyFrameParams` hat feste Layout-Garantie (336 B, `static_assert`), also ein zweiter UBO/SSBO oder ein Const-Array im Shader, das beim Shader-Build per String-Splice erzeugt wird (Muster wie `causticsWaveTrainsList()`, `UnderwaterCausticsShared.h:19`). Die Werte sind zeitunabhaengig und deterministisch, damit bleibt das Bild gleich (bis auf Float-Rundung der `sin/cos`, die bei der CPU-Berechnung in Double sogar genauer wird).
- **Erwarteter Effekt**: Schaetzung: Aurora-Nachtframes sparen einen grossen Teil der Per-Kurtine-ALU vor dem Early-Out. Betrifft nur Nacht mit Aurora > 0, daher nicht jeder Frame. Messen.
- **Risiko / Verifikation**: Mittel: Float-Genauigkeit CPU vs. GPU-`fract` im Hash (`starHash` nutzt `fract`/`dot` – bitgleiche CPU-Reproduktion ist schwer). Alternative ohne Bit-Anspruch: Parameter in einem Vorpass in eine 1D-Textur rendern. Visuell vergleichen (Aurora-Szene, mehrere `uTime`).

### REN-906 – graphTexKey baut pro Draw und Texturslot einen std::string
- **ID**: REN-906
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:612,624-627`; Aufrufer `src/Backends/Vulkan/VulkanRenderer.cpp:3609-3624` (`resolveGraphTexture`), `:8338,8349,8351` (pro Graph-Material-Draw)
- **Problem**: `graphTexKey` erzeugt aus der UUID `std::to_string(id.hi) + ":" + std::to_string(id.lo)` (bis ~40 Zeichen, also ueber der SSO-Grenze → Heap-Allokation), `resolveGraphTexture` haengt fuer Array-Slots noch `"#arr"` an und sucht dann in einer `unordered_map<std::string,…>` (`m_graphTexCache`). Das passiert pro Draw fuer bis zu 4 Projekt-Texturen plus die Landscape-Weightmap (leerer `std::string{}` wird je Aufruf mitgebaut). Dieselbe Funktion bedient `m_uiImageCache` (`VulkanRenderer.cpp:7592-7595,14555`) pro UI-Quad mit Bild.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
```
- **Lösung**: Schluessel als Struct `{HE::UUID id; bool array;}` fuer den UUID-Fall (`std::hash<HE::UUID>` existiert bereits fuer `m_materialTexCache`) und eine zweite, seltener genutzte `unordered_map<std::string,…>` nur fuer den Pfad-Fall (lose Editor-Assets). Alternativ die aufgeloesten `VkImageView`s pro Material einmal cachen (Material-ID → `std::array<VkImageView,4>`), dann entfaellt der Lookup pro Draw ganz. Die Invalidierung (`InvalidateTexture`) muss beide Maps bedienen.
- **Erwarteter Effekt**: Schaetzung: pro Graph-Material-Draw 4-5 Heap-Allokationen + String-Hashing weniger; skaliert mit der Draw-Zahl (`k_matMaxDraws` = 1024). Gering bei wenigen Graph-Materialien. Messen mit Draw-lastiger Szene.
- **Risiko / Verifikation**: Gering. Die Parallelstellen in `OpenGLRenderer.cpp` (`ResolveGraphTexture`, gleicher "hi:lo"-Schluessel laut Kommentar `VulkanRenderer.h:607`) und Metal haben vermutlich dasselbe Muster; in einem Zug angleichen.

### REN-907 – GIProbeSceneSignature laeuft jeden Frame ueber alle Objekte, in jedem Backend
- **ID**: REN-907
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/GIProbeGrid.h:121-132`, `:140-150` (Tracker); Aufrufer `OpenGLRenderer.cpp:6366`, `VulkanRenderer.cpp:11327`, `D3D11Renderer.cpp:3417`, `D3D12Renderer.cpp:7601`, `MetalRenderer.mm:8977`
- **Problem**: Der "O(1)-Skip" des Trackers (`canSkip`, Kommentar `:135`) ist nur O(1) *nach* dem Signaturaufruf; die Signatur selbst ist O(n) mit einem Splitmix-Finalizer (zwei 64-Bit-Multiplikationen) je Objekt, jeden Frame, auch wenn nichts geaendert wurde. Bei 50k Objekten (Zahl aus `RenderExtractor.h:190`) sind das 50k Hashes pro Frame nur fuer einen Gleichheitsvergleich. Der Extractor weiss bereits, ob sich die Struktur geaendert hat (`HorizonWorld::structureEpoch`, `RenderExtractor.h:203,349`).
- **Code-Auszug**:
```cpp
for (const RenderObject& obj : objects) { uint64_t h = (uint64_t(obj.entityId) << 32) ^ obj.meshAssetId.hi ^ (...); h ^= h >> 31; h *= 0x94D0...; h ^= h >> 29; sum += h; }
```
- **Lösung**: Signatur nur neu berechnen, wenn `RenderExtractor::fullExtractCount()` sich aendert (oder besser die Signatur im Extractor waehrend der Objektschleife mitzaehlen und in `RenderWorld` ablegen: ein Feld `uint64_t geometrySignature`). `GIProbeSceneSignature` bleibt als Referenz fuer Tests. Die LOD-/Mesh-Wechsel-Erkennung bleibt erhalten, weil die Signatur weiter von (entityId, meshAssetId) abhaengt.
- **Erwarteter Effekt**: Schaetzung: O(n) → O(1) pro Frame und Backend (nur einer der fuenf Backends laeuft zur Laufzeit). Relevant erst bei sehr grossen Szenen; Wert ~ n x ~2 ns, also ~0,1 ms bei 50k. Niedrige Prioritaet, wenn nicht GI-lastig.
- **Risiko / Verifikation**: Gering; Tests zu `GIProbeGridTracker` (`grep -l GIProbeGridTracker tests/`) decken Signatur-Aenderungen ab.

### REN-908 – Sky-IBL-Rebake: 98k CPU-Atmosphaerenintegrale, frische Allokation, 5-fach kopierte Treiberlogik
- **ID**: REN-908
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `include/HorizonRendering/SkyEnvBake.h:47-97,198-209`; Aufrufer `VulkanRenderer.cpp:3021-3040` (Parallelstellen `OpenGLRenderer.cpp:77`, `D3D11Renderer.cpp:4626`, `D3D12Renderer.cpp:8980`, `MetalRenderer.mm:13833`)
- **Problem**: (1) Jede Sonnenbewegung > 2e-3 loest einen Rebake von 6 x 128 x 128 = 98304 Texeln aus; jeder Texel ruft `SkyColorCPU` → `AtmoScatterCPU` (12x(2 `exp` + 5x3 `exp`) plus `pow` fuer Mie-Phase und drei `pow(s, …)` fuer die Sonne). Bei Zeitraffer/Tag-Nacht-Zyklus mit schneller Sonne wird das jeden Frame fällig. Die Ambient-Cubemap ist niederfrequent (Kommentar `VulkanRenderer.cpp:3015-3019`). (2) `std::vector<float> px` (1,5 MiB) wird pro Rebake neu angelegt (`:3027`), danach serielle Halbfloat-Konvertierung ueber 393k Floats (`:3036-3037`). (3) `BuildSkyEnvFaceRow` normalisiert `dir` doppelt (`SkyEnvBake.h:205` und `:103`). (4) Dieselbe "px-Vektor + parallel_for + Konvertierung"-Logik steht fuenfmal in den Backends.
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(static_cast<size_t>(6) * N, [&](size_t idx){ ... HE::BuildSkyEnvFaceRow(N, f, t, sunDir, &px[...]); }, "SkyEnvBake", 1);
for (size_t i = 0; i < px.size(); ++i) dst[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: (a) Einen geteilten Helfer `HE::BakeSkyEnvCube(N, sunDir, scratch)` in `SkyEnvBake.h` (oder `SkyEnvBake.cpp`) mit wiederverwendetem Scratch-Buffer ergaenzen; die Backends rufen nur noch den Helfer. (b) Doppelte `normalize` in `BuildSkyEnvFaceRow` entfernen (`SkyColorCPU` normalisiert intern). (c) Konvertierung nach Halffloat in die Zeilenfunktion verlagern (parallel). (d) Messen, ob 64² Faces statt 128² visuell reichen (Faktor 4); das aendert Hash-Pin in `tests/test_culling.cpp`.
- **Erwarteter Effekt**: Schaetzung: (a)-(c) sparen die 1,5 MiB-Allokation pro Rebake und die serielle Konvertierung (einige 100 µs bis ms); (d) viertelt die Bake-Zeit. Ohne Messdaten (Rebake-Frequenz bei Zeitraffer, Zeit pro Bake) nicht priorisieren.
- **Risiko / Verifikation**: (a)-(c) ohne Bildaenderung (Hash-Pin in `tests/test_culling.cpp` muss unveraendert bleiben). (d) aendert die Ambient-Beleuchtung auf allen Plattformen.

### REN-909 – Fette AoS-Strukturen: Culling liest 24 Byte pro ~300-Byte-Objekt, DrawCall dupliziert RenderObject
- **ID**: REN-909
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/RenderObject.h:30-129`, `include/HorizonRendering/CommandBuffer.h:15-61`, `src/HE_Rendering/src/FrustumCuller.cpp:61-65`, `include/HorizonRendering/RenderWorld.h:362`
- **Problem**: `RenderObject` enthaelt u. a. `mat4` (64 B), zwei UUIDs (32 B), `HE::AABB`, `vector<RenderSection>` (24 B), `vector<float> paramOverride` (24 B), Handles, PBR-Skalare, Tint, Flags: grob 300+ Byte. `FrustumCuller::cull` iteriert parallel ueber `world.objects[i].worldBounds` und liest pro Objekt nur die AABB, aber jede Zeile liegt in einer eigenen Cache-Line-Gruppe; bei 50k Objekten sind das 15 MB Stride-Durchlauf fuer 1,2 MB Nutzdaten. `DrawCall` dupliziert danach ca. 17 dieser Felder (`GeometryPass`) inklusive zweier Vektoren, obwohl die meisten Objekte keine Sections/Overrides haben (leere Vektoren kosten nur 48 B, aber verhindern trivial-copyable Strukturen).
- **Code-Auszug**:
```cpp
parallel_for(count, [&](size_t i) {
    const HE::AABB& bounds = world.objects[i].worldBounds;
    if (bounds.isValid()) outVisible[i] = frustum.intersects(bounds) ? 1u : 0u;
}, "FrustumCull");
```
- **Lösung**: Hot/Cold-Split: `RenderWorld` fuehrt eine parallel gepflegte `std::vector<HE::AABB> objectBounds` (vom Extractor gefuellt, von `refineWorldBounds` aktualisiert) fuer den Cull; oder `RenderObject` in `RenderObjectHot` (Bounds, Transform, ids) und `RenderObjectCold` (sections, paramOverride, weightmap, landscape) teilen. Erst nach Messung (`FrustumCuller::cull`-Profilscope, bei >10k Objekten) angehen; die Schnittstelle `RenderObject` wird von allen Backends direkt gelesen.
- **Erwarteter Effekt**: Schaetzung: Cull-Durchlauf bei vielen Objekten 2-4x schneller (Speicherbandbreite). Bei < 5k Objekten irrelevant. Messen.
- **Risiko / Verifikation**: Mittel-hoch (breite Auswirkung). `RenderObject.h:139-141` erwaehnt einen Test (`tests/test_foliage_cluster.cpp`), der Backend-Quellen auf handgeschriebenes "refine" prueft; Aenderungen an `refineWorldBounds` muessen diesen Test erfuellen.

### REN-910 – FoliageExtract: pro Bucket zwei InstanceBlock-Kopien (Atomic-Refcount, 96 B) und pro Layer temporaere Vektoren
- **ID**: REN-910
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/FoliageExtract.cpp:332-335,366-383,385-391,396-423`; `include/HorizonRendering/RenderWorld.h:229-241`
- **Problem**: Pro Bucket (O(Buckets), explizites Designziel der Datei) passiert `InstanceBlock block = layer;` (Zeile 387; kopiert `shared_ptr` mit atomarem Zaehler und eine 64-B-`mat4`) und danach in `emit` `out.instanceBlocks.push_back(block)` (Zeile 380; zweite Kopie, ohne `reserve` des Zielvektors). Dazu pro Layer und Frame neue `std::vector`s `straddlers`/`wholes` (Zeilen 332-335) und ein `std::make_shared` fuer `scratch` bei Straddlern. `out.objects.push_back(obj)` ohne `reserve` mit einem `RenderObject` von ~300 B.
- **Code-Auszug**:
```cpp
InstanceBlock block = layer;
block.first = w.bucket->first;
block.count = w.bucket->count;
emit(block, clusterBounds(*w.bucket), w.nearSq);   // -> out.instanceBlocks.push_back(block);
```
- **Lösung**: `emit(InstanceBlock&& block, …)` und `push_back(std::move(block))`; `out.objects.reserve(out.objects.size() + wholes.size() + straddlers.size())` und dasselbe fuer `out.instanceBlocks`; `wholes`/`straddlers` als `thread_local` oder per Aufrufer uebergebene Scratch-Vektoren (`clear()` statt neu anlegen). Langfristig: `parent` (64 B) nur einmal pro Layer in `RenderWorld` halten und `InstanceBlock` auf `{layerIndex, first, count}` reduzieren; das aendert die `InstanceBlock`-API.
- **Erwarteter Effekt**: Schaetzung: pro Bucket 2 atomare Inkrement/Dekrement-Paare und 128 B Kopie weniger; bei 10^3-10^4 Buckets pro Frame im Mikrosekundenbereich (messen). Niedrige bis mittlere Prioritaet.
- **Risiko / Verifikation**: Gering; `tests/test_foliage_cluster.cpp` pruefen Bucket-Ausgabe (Reihenfolge der Objekte unveraendert lassen).

### REN-911 – TAA: zwei unordered_map<uint32_t, mat4> pro Frame fuer Objekt-Transforms
- **ID**: REN-911
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:874`; Nutzung `src/Backends/Vulkan/VulkanRenderer.cpp:5593-5594,5647,5660-5662,5674`
- **Problem**: `m_taaPrevTransforms`/`m_taaCurTransforms` sind `std::unordered_map<uint32_t, glm::mat4>`; pro sichtbarem Objekt pro Frame passieren ein `find` und ein `operator[]` (Knotenallokation, 80 B), danach `clear()` (Knoten-Freigabe) und `swap`. Das sind O(Objekte) Heap-Operationen pro TAA-Frame. Die D3D11/D3D12/GL/Metal-Backends haben laut Kommentaren (`TemporalAA.h:5-8`) dieselbe Funktion; vermutlich dasselbe Muster.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms, m_taaCurTransforms;
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flacher Speicher: `std::vector<glm::mat4>` indiziert nach `RenderWorld::objects`-Index plus `std::vector<uint32_t>` der entityIds, oder ein sortiertes `vector<pair<uint32_t,mat4>>` (Objekte kommen bereits sortiert aus dem Extractor, falls entityId-Reihenfolge stabil). Alternative mit Minimalaenderung: `reserve(objects.size())` und `reserve` statt `clear` ist bei `unordered_map` wirkungslos; daher Flat-Map.
- **Erwarteter Effekt**: Schaetzung: Pro TAA-Frame O(n) Allokationen weg (n = sichtbare Objekte); bei 5k Objekten ca. 5k Mallocs. Nur wenn TAA aktiv ist.
- **Risiko / Verifikation**: Gering-mittel (Verhalten bei neuen/verschwundenen Entities muss gleich bleiben: fehlender Eintrag → aktuelle Transform). Visuell: TAA-Szene mit bewegten Objekten (Velocity-Dump).

### REN-912 – World-Preview: zwei vkDeviceWaitIdle pro Aufruf, frischer RenderExtractor und RenderWorld pro Frame
- **ID**: REN-912
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h:991-1000,99-105`; `src/Backends/Vulkan/VulkanRenderer.cpp:6551`; `include/HorizonRendering/WorldPreviewFrame.h:35-65`
- **Problem**: Der Header dokumentiert "Every call waits for the device before and after"; `VulkanRenderer.cpp:6551` ruft `vkDeviceWaitIdle`. Mehrere Preview-Viewports (Class Editor, Mesh Viewer, Sekundaer-Scene-Views) rufen das pro Frame und Slot, d. h. die CPU wartet pro Preview auf die komplette GPU-Pipeline des Hauptframes. Zusaetzlich baut `buildWorldPreviewFrame` pro Aufruf einen neuen `RenderExtractor` (Konstruktor mit `unordered_set`, `setContentManager` verwirft die "fehlend"-Cache-Eintraege) und weist `out.snapshot = RenderWorld{}` zu (gibt alle Vektor-Kapazitaeten des letzten Frames frei, statt `clear()`). Editor-only, daher Mittel.
- **Code-Auszug**:
```cpp
RenderExtractor extractor;
extractor.setContentManager(cm);
...
out.snapshot = RenderWorld{};
extractor.extract(world, out.snapshot, aspect, &previewCam);
```
- **Lösung**: (1) `out.snapshot.clear()` statt Neuzuweisung (die Methode existiert: `RenderWorld.h:290`); `RenderExtractor` als Member in `WorldPreviewFrame`/Renderer halten. (2) Die `vkDeviceWaitIdle`-Aufrufe durch Fence-/Serial-Tracking ersetzen (`m_frameSerial`/`GetCompletedFrameSerial` existieren, `VulkanRenderer.h:115-123`) und die gemeinsamen Sky/Debug-Puffer pro Preview-Slot separat halten. (2) ist invasiv; erst (1).
- **Erwarteter Effekt**: Schaetzung: (1) kleine Allokationsersparnis pro Preview-Frame; (2) kann den Editor bei mehreren offenen Previews merklich glaetten (Stalls von GPU-Frame-Dauer). Messen (GPU-Wait-Zeit im Profiler).
- **Risiko / Verifikation**: (1) gering; (2) hoch (Synchronisation). Parallelstellen: D3D11/D3D12 (`D3D11Renderer.cpp:8267`, `D3D12Renderer.cpp:12430`), GL/Metal haben eigene Kopien.

### REN-913 – giAutoLandscapeSliceMeans: pow() pro Texel bei jedem Extract
- **ID**: REN-913
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/GiAutoLandscape.cpp:64-103` (Aufrufer `src/RenderExtractor.cpp:349`, `EditorApplication.cpp:7740`); Kommentar `include/HorizonRendering/GiLandscape.h:126-130`
- **Problem**: Pro Auto-Landscape und pro `extract` (jeden Frame, laut Kommentar absichtlich "cheap enough to redo every frame") werden 5 Slices x bis zu 16x16 Texel x 3 Kanaele durch die sRGB-Dekodierung geschickt (`std::pow` pro Wert, ca. 3840 pow). Das Ergebnis aendert sich nur, wenn die Textur neu importiert wird. `decode` arbeitet auf `uint8_t`, also gibt es nur 256 moegliche Eingaben.
- **Code-Auszug**:
```cpp
auto decode = [&](uint8_t b) { const float c = b / 255.0f; if (!t.srgb) return c; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
```
- **Lösung**: Statisches `std::array<float,256>` (einmal beim ersten Aufruf gefuellt) fuer sRGB-Dekodierung; optional Ergebnis pro `(TextureAsset*, contentEpoch)` cachen. Ergebnis bit-identisch zur jetzigen Berechnung, wenn die LUT mit demselben Ausdruck gefuellt wird.
- **Erwarteter Effekt**: Schaetzung: ~3840 `pow` pro Landscape und Extract weniger (einige 10 µs). Gering; wegen der Einfachheit trotzdem sinnvoll.
- **Risiko / Verifikation**: Praktisch keines; vorhandener Test zu `giAutoLandscapeSliceMeans` (`grep -l giAutoLandscape tests/`) prueft das Ergebnis.

### REN-914 – GPUMemoryAllocator: Doku und Verhalten widersprechen sich, drei Container fuer einen LRU
- **ID**: REN-914
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/GPUMemoryAllocator.h:22-24,51-53`, `src/HE_Rendering/src/GPUMemoryAllocator.cpp:9-29`
- **Problem**: Der Header sagt "Returns false if the allocation would exceed the budget even after evicting all LRU candidates", die Implementierung prueft aber nur `sizeBytes > m_budget` (Zeile 11) und addiert zu `m_used`; `m_used` kann das Budget beliebig ueberschreiten, ohne dass evictiert oder abgelehnt wird. Die Verwaltung braucht drei Strukturen (`m_sizes`, `m_lruList`, `m_lruIndex`), obwohl `m_sizes` und `m_lruIndex` denselben Schluessel haben (ein `unordered_map<key, {size, list::iterator}>` genuegt). Der Typ ist laut Kommentar "STAGED API", ohne Produktionsnutzer (verifiziert: `grep` findet nur `RenderResourceManager.cpp`).
- **Code-Auszug**:
```cpp
bool GPUMemoryAllocator::requestAllocation(uint64_t sizeBytes, RenderHandle handle)
{ if (sizeBytes > m_budget) return false; ... m_used += sizeBytes; return true; }
```
- **Lösung**: Entweder Doku an die Implementierung anpassen oder die Budget-Pruefung `while (m_used + size > m_budget && !m_lruList.empty()) evictLRU();` implementieren, bevor der Typ einen Produktionsnutzer bekommt. Container zu einem `unordered_map<uint64_t, Entry{size, it}>` zusammenfassen. Da Staged, geringe Prioritaet; wichtig ist nur, dass der erste Nutzer nicht auf die Doku vertraut.
- **Erwarteter Effekt**: Kein Laufzeiteffekt; verhindert falsche Annahme beim spaeteren Upload-Budgeting.
- **Risiko / Verifikation**: Bestehende Tests (`grep -l GPUMemoryAllocator tests/`) pruefen das jetzige Verhalten; vor einer Aenderung klaeren, ob ein Test auf "kein Evict in requestAllocation" baut.

### REN-915 – InstanceBlock::world()/position() verzweigen pro Instanz auf `kind`
- **ID**: REN-915
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `include/HorizonRendering/RenderWorld.h:264-286` (`position`, `world`), `:344-362` (`appendInstances`, `forEachInstance`)
- **Problem**: `kind` ist fuer den ganzen Block konstant, wird aber in jeder Instanz neu per `switch` ausgewertet; bei `Identity` (haeufigster Fall: Terrain ohne Verschiebung bzw. gestreamtes Scratch, `FoliageExtract.cpp:416-418`) ist `appendInstances` effektiv ein `memcpy` von N x 64 B, laeuft aber als Schleife mit Zuweisung der Rueckgabe-`mat4` pro Element. `out.resize(base + count)` initialisiert zudem erst auf Identitaet (vom `mat4`-Konstruktor), bevor sie ueberschrieben wird.
- **Code-Auszug**:
```cpp
out.resize(base + b->count);
for (uint32_t i = 0; i < b->count; ++i) out[base + i] = b->world(i);
```
- **Lösung**: In `appendInstances` nach `kind` verzweigen: `Identity` → `out.insert(out.end(), src.begin()+first, src.begin()+first+count)` (kein Vorinitialisieren), `Translation` → eine Schleife mit `w[3] += offset`, `General` → `parent * m`. Das Ergebnis bleibt bitgleich (Kommentar `:230-231` verlangt Bit-Identitaet; die Addition `m[3] + parent[3]` muss dieselbe Reihenfolge behalten).
- **Erwarteter Effekt**: Schaetzung: bis ca. 2x schneller beim Entfalten sehr grosser Foliage-Batches; sonst unbedeutend. Messen vor Umsetzung (Compiler kann die Verzweigung evtl. schon herausziehen).
- **Risiko / Verifikation**: Gering; `tests/test_foliage_cluster.cpp` vergleicht bereits bitgenau.

### REN-916 – VulkanShaderManager ist praktisch tot, ohne `#pragma once`, und fuehrt ein nie bereinigtes Tracking
- **ID**: REN-916
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/Backends/Vulkan/VulkanShaderManager.h:1-25`, `src/Backends/Vulkan/VulkanShaderManager.cpp:13-122`; Nutzung nur `VulkanRenderer.cpp:216` (`m_shaderManager = VulkanShaderManager();`) und `VulkanRenderer.h:914`
- **Problem**: Verifiziert per Grep: ausser Konstruktion/Zuweisung wird `m_shaderManager` im Vulkan-Backend nirgends benutzt (Module werden ueber `loadShaderModule`/MaterialShaderLibrary gebaut). Die Datei hat keinen Include-Guard (`#pragma once` fehlt in Zeile 1, Datei endet ohne Newline), `load()` macht `fs::exists` plus `ifstream` (doppelter Dateizugriff), `static unsigned nextId` ist nicht thread-sicher, `release()` entfernt den Handle nicht aus `m_loadedShaders` (nur `cleanup()` leert die Liste, spaetes Doppel-`release` ist harmlos), und `m_loadedPrograms` speichert Kopien der `ShaderProgramHandle`-Vektoren ohne Zweck (`releaseProgram` ist ein No-op). Der Header zieht zusaetzlich `<vulkan/vulkan.h>` und `<unordered_map>` in `VulkanRenderer.h` (und damit in jeden Includer).
- **Code-Auszug**:
```cpp
void VulkanShaderManager::releaseProgram(ShaderProgramHandle handle) { (void)handle; }
```
- **Lösung**: Pruefen, ob irgendein Test/Tool den Manager braucht (`grep` in `tests/` fand nur die D3D-Manager). Wenn nicht: Klasse und Member entfernen (Interface `ShaderManager.h` bleibt fuer die anderen Backends). Wenn behalten: `#pragma once` ergaenzen, Tracking-Vektoren streichen.
- **Erwarteter Effekt**: Kleinere Header-Abhaengigkeit (kein `vulkan.h`-Include mehr ueber diesen Pfad), weniger toter Code.
- **Risiko / Verifikation**: Gering; Build aller Backends (Vulkan nur auf Windows/Linux) und `he_tests`.

### REN-917 – VulkanRenderer.h: fest verdrahtete Frame-Zahlen [2]/[3] und Monolith-Header
- **ID**: REN-917
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/Backends/Vulkan/VulkanRenderer.h` (z. B. `:349-356,381,411,426-428,446-451,580,1350-1356,1371,1482-1485`); `src/Backends/Vulkan/VulkanRenderer.cpp:44` (`static constexpr uint32_t k_maxFramesInFlight = 2;`)
- **Problem**: Der Header kommentiert mehrfach `k_maxFramesInFlight` (`:580,1371,1424,1478,1549`), aber die Konstante ist file-lokal in der .cpp. Im Header stehen deshalb ~40 Literale `[2]`, und die GI-Ringe verwenden `[3]` ("static_assert at use site", `:1371`) – zwei verschiedene Frame-Zahlen im selben Backend, die nur durch Kommentar-Disziplin zusammenpassen. Die Klasse umfasst 1566 Zeilen mit ~600 Members, vom Swapchain bis zur Hardware-RT-Beschleunigung, und `VulkanRenderer.h` inkludiert `RenderExtractor.h`, `GiBvh.h`, `GIProbeGrid.h`, `MaterialShaderLibrary.h`, `VulkanShaderManager.h`, `vulkan.h` – jeder Includer zahlt das.
- **Code-Auszug**:
```cpp
VkSemaphore m_imageReady[2]{};   ... GiBuffer m_giInstanceBuf[3]; // k_maxFramesInFlight (static_assert at use site)
```
- **Lösung**: `inline constexpr uint32_t kMaxFramesInFlight` in `RenderConstants.h` (neben `kGpuTimerRing`) einfuehren, im Header verwenden und das `static_assert`-Muster vereinheitlichen. Mittelfristig Teilsysteme (SSAO, SSR, GI, TAA, Decals) als eigene Strukturen/Klassen mit eigener Header-Datei ausgliedern (Pimpl), damit Aenderungen am GI-Block nicht alle Includer neu uebersetzen. Reine Struktur, kein Laufzeiteffekt.
- **Erwarteter Effekt**: Weniger Fehlerrisiko bei einer Aenderung der Frame-Zahl; schnellere inkrementelle Builds (nicht gemessen).
- **Risiko / Verifikation**: Gering; Vulkan-Build. Die Drei-Slot-GI-Ringe haengen laut Kommentar an `k_maxFramesInFlight`; vor dem Ersetzen im .cpp klaeren, warum GI 3 Slots nutzt (Swapchain-Anzahl?).

### REN-918 – RenderSorter::partitionByOpacity: Flag-Parameter mit totem Default-Zweig
- **ID**: REN-918
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/RenderSorter.h:42-60`; Aufrufer `D3D11Renderer.cpp:6721-6722`, `D3D12Renderer.cpp:10736-10737`, `VulkanRenderer.cpp:8141-8142`
- **Problem**: `partitionByOpacity(..., bool sectionAware = false)` unterscheidet zwei Modi, aber alle drei Aufrufer (verifiziert per Grep) uebergeben `true`; der `false`-Modus ("conservative guard", droppt `sectionIndex > 0`) wird in Produktionscode nie ausgefuehrt. Ein bool-Flag-Parameter mit gefaehrlichem Default ist eine Fehlerquelle fuer einen sechsten Backend-Port; der Kommentar rechtfertigt das selbst.
- **Code-Auszug**:
```cpp
static void partitionByOpacity(const std::vector<DrawCall>& drawCalls, std::vector<const DrawCall*>& outOpaque,
                               std::vector<const DrawCall*>& outTransparent, bool sectionAware = false);
```
- **Lösung**: Parameter entfernen, immer section-aware; den `false`-Pfad in `RenderSorter.cpp:44ff` und die zugehoerigen Tests (`grep -l sectionAware tests/`) streichen. Falls der Guard fuer externe Backends gedacht ist: Default auf `true` drehen.
- **Erwarteter Effekt**: Weniger Verzweigung/Doku; kein Laufzeitgewinn.
- **Risiko / Verifikation**: Gering; Tests, die den `false`-Modus pruefen, anpassen.

### REN-919 – SkyShaderSource.h: 146 KB Literal im Header in vier Backend-TUs, drei grosse String-Kopien beim Bau
- **ID**: REN-919
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `include/HorizonRendering/SkyShaderSource.h:36-2248` (`kSkyFS`), `:2541-2548` (`BuildSkyFragmentGLSL450`); Includer: `OpenGLRenderer.cpp`, `VulkanRenderer.cpp`, `D3D11Renderer.cpp`, `D3D12Renderer.cpp`, `tests/test_sky_shader.cpp`, `tests/test_culling.cpp`
- **Problem**: Die Datei ist ein reiner Daten-Header mit 146 071 Byte; jede Includer-Uebersetzungseinheit parst das Literal. `BuildSkyFragmentGLSL450` kopiert `kSkyFS` in `const std::string fs` (Zeile 2543), erzeugt `substr` (weitere Kopie) und konkateniert Prelude + `kSkyFuncGLSL` + Rest (dritte): drei volle Kopien bei jeder Pipeline-Erzeugung, in jedem der drei cross-kompilierenden Backends. Einmalig beim Start, daher unkritisch. Ausserdem ist `skyColor` im Shader (`:2331-2427`) mit `sunGlare` (`:2087-2103`) bewusst doppelt gerechnet ("col -= sunGlare … byte-identical", Kommentar `:1977-1982`).
- **Code-Auszug**:
```cpp
const std::string fs = kSkyFS;
...
return std::string(kSkyVulkanPrelude) + kSkyFuncGLSL + fs.substr(at + marker.size());
```
- **Lösung**: `std::string_view` auf `kSkyFS` benutzen (Suche mit `find` ohne Kopie) und das Ergebnis per `reserve` + `append` bauen. Die grosse Quelle in eine `.cpp` (`SkyShaderSource.cpp`) mit `extern const char*`-Deklaration verschieben, damit nur eine TU das Literal enthaelt – Achtung `ClipSpace.h`-Kommentar: Backend-Static-Libs linken nicht gegen HorizonRendering, eine neue `.cpp` braucht deshalb Build-Plumbing (siehe Argument in `SkyEnvBake.h:22-24`). Sonst Header lassen.
- **Erwarteter Effekt**: Geringe Compile-Zeit- und Startup-Ersparnis. Nicht priorisieren.
- **Risiko / Verifikation**: Gering; `tests/test_sky_shader.cpp` pruefen die Prelude-Zuordnung.

### REN-920 – Schwere Header-Abhaengigkeiten (ParticleGraph.h in RenderWorld.h, <fstream> in WorldPreviewGrid.h)
- **ID**: REN-920
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `include/HorizonRendering/RenderWorld.h:5-6`, `include/HorizonRendering/ParticleShaderTemplates.h:20`, `include/HorizonRendering/WorldPreviewGrid.h:6,94-113`, `include/HorizonRendering/ShaderManager.h:19-40`
- **Problem**: `RenderWorld.h` (von allen Backends, Extractor, Culler, Sorter, Passes inkludiert) zieht `<ParticleGraph/ParticleGraph.h>` und `<Renderer/UIRenderObject.h>`, obwohl nur `HE::ParticleEmitterConfig` (Wert-Member in `ParticleBatch`) gebraucht wird. `ParticleShaderTemplates.h` inkludiert dasselbe nur fuer `hashParticleShaderConfig`. `WorldPreviewGrid.h` zieht `<fstream>` fuer `writeWorldPreviewDump`, einen Debug-Dump, in einen Header, den jedes Backend inkludiert. `ShaderManager::releaseProgram(ShaderProgramHandle handle)` (Zeile 38) nimmt eine Struktur mit `std::vector` by value, jede Uebergabe kopiert den Vektor.
- **Code-Auszug**:
```cpp
virtual void releaseProgram(ShaderProgramHandle handle) = 0;   // ShaderProgramHandle enthaelt std::vector<ShaderHandle>
```
- **Lösung**: `ParticleEmitterConfig` in einen leichten Header (`ParticleEmitterConfig.h`) auslagern und nur diesen inkludieren; `writeWorldPreviewDump` nach `WorldPreviewGrid.cpp`/ein Backend-gemeinsames `.cpp` oder `inline` in eigener Datei; `releaseProgram(const ShaderProgramHandle&)`. Geringe Compile-Zeit-Ersparnis, keine Laufzeitauswirkung.
- **Erwarteter Effekt**: Schaetzung: weniger Rebuild-Kaskaden beim Aendern von `ParticleGraph.h`.
- **Risiko / Verifikation**: Gering; Header-Aenderungen mit vollem Build aller Targets pruefen (HE_Editor, HE_Game, `he_tests`).

### REN-921 – giBvhIntersect: Stack-Ueberlauf verwirft Kinder stillschweigend, Baum ohne Tiefenbegrenzung
- **ID**: REN-921
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/GiBvh.cpp:174-217` (insbesondere `:182,210-214`), Median-Split `:36-81`
- **Problem**: Bei vollem 64er-Stack wird der Kindknoten per `else if (sp + 2 <= …)` einfach nicht gepusht – der Teilbaum ist dann fuer den Strahl unsichtbar, ohne Hinweis. Der Median-Split (`nth_element` ueber `order` mit indirektem Zugriff auf `tris[a].centroid`) garantiert zwar Tiefe ~log2(N), aber Duplikat-Centroids brechen mit `extent <= 1e-12f` ab und erzeugen uebergrosse Blaetter (`:57`), was die Traversal-Laufzeit pro Blatt erhoeht, aber die Tiefe nicht. Die Obergrenze wird also praktisch nie erreicht; trotzdem verdeckt der stille Drop jeden zukuenftigen Fehler im Build. Zusaetzlich liegt `BuildTri` (84 B) mit `v0,v1,v2,centroid,bmin,bmax` neben `order`-Indirektion, was die Build-Zeit durch Cache-Misses verlangsamt; nur beim Mesh-Upload relevant.
- **Code-Auszug**:
```cpp
else if (sp + 2 <= static_cast<int>(sizeof(stack) / sizeof(stack[0])))
{ stack[sp++] = static_cast<uint32_t>(node.leftFirst); stack[sp++] = static_cast<uint32_t>(node.leftFirst + 1); }
```
- **Lösung**: In `buildGiBvh` die maximale Tiefe messen und bei > 60 den Split abbrechen (Blatt) oder asserten; Traversal dann ohne Guard (Shader-Spiegel ebenso). Bei Bedarf Test mit entarteter Geometrie (alle Dreiecke mit gleichem Centroid, sehr viele Dreiecke) ergaenzen. Build-Optimierung (Dreiecke direkt in Leaf-Reihenfolge umsortieren statt `order`-Indirektion) nur nach Messung.
- **Erwarteter Effekt**: Kein Performance-Effekt; verhindert stille Fehlerbilder bei degenerierten Meshes.
- **Risiko / Verifikation**: Gering; Tests zu `giBvhIntersect` (`grep -l GiBvh tests/`) und Shader-Kernel bleiben die Referenz ("mirrored 1:1").

### REN-922 – FoliageExtract: thread_local Bounds-Cache waechst ohne Verdraengung
- **ID**: REN-922
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/FoliageExtract.cpp:92-108`
- **Problem**: `thread_local std::unordered_map<HE::UUID, BoundsEntry> cache` wird nie bereinigt; ein neu geladenes Asset (anderer Zeiger) ueberschreibt nur denselben Schluessel. Bei langen Editor-Sitzungen mit vielen Foliage-Meshes oder Projektwechsel bleiben Eintraege fuer nicht mehr vorhandene UUIDs stehen. Zusaetzlich wird fuer jeden Layer und Frame `cm->getStaticMesh(id)` aufgerufen (Hash-Lookup), obwohl der Cache bereits eine Pruefung auf `asset == m` macht.
- **Code-Auszug**:
```cpp
thread_local std::unordered_map<HE::UUID, BoundsEntry> cache;
...
cache[id] = e;
```
- **Lösung**: Cache an `ContentManager::contentEpoch()` koppeln (leeren bei Epoch-Wechsel, wie bereits `RenderExtractor::setContentManager` es fuer `m_sectionMaterialMissing` tut, `RenderExtractor.h:263-267`) oder die Bounds im `StaticMeshAsset` selbst vorhalten (`boundsMin/Max` existieren schon, `scanMeshBounds` fuellt nur Luecken).
- **Erwarteter Effekt**: Vernachlaessigbar (Speicher im KB-Bereich); Sauberkeit.
- **Risiko / Verifikation**: Gering.

### REN-923 – FillMaterialDirectionalWindow: Zwischenstruktur plus verschachtelte Schleife mit Pro-Element-Verzweigung
- **ID**: REN-923
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/src/LightPacking.cpp:247-278`
- **Problem**: `FillMaterialDirectionalWindow` baut zuerst ein `DirectionalLightWindow` (4 Arrays x 8 `vec4`, 512 B) und schreibt es dann in 8x4-Schleife elementweise in `Lighting` um, mit `on`-Pruefung pro Skalar und `(on && k == 0)` fuer `lightParams`. Das ist schwer lesbar (die 4er-Schleife `k` verdeckt, dass `lightParams[i][1..3]` immer 0 sind).
- **Code-Auszug**:
```cpp
for (int i = 0; i < kMaxLightWindow; ++i)
    for (int k = 0; k < 4; ++k) { const bool on = i < w.count; out.lightPos[i][k] = on ? w.pos[i][k] : 0.0f; ... out.lightParams[i][k] = (on && k == 0) ? w.params[i][0] : 0.0f; }
```
- **Lösung**: Zwei kurze Schleifen: `for i < w.count` kopiert per `std::memcpy(out.lightPos[i], &w.pos[i], 16)` (`vec4` und `float[4]` sind layout-kompatibel), `for i >= w.count` nullt; `lightParams[i][0] = w.params[i][0]; [1..3] = 0`. Verhalten gleich.
- **Erwarteter Effekt**: Lesbarkeit; Laufzeit unbedeutend (einmal pro Frame).
- **Risiko / Verifikation**: Gering; `tests/test_light_packing*.cpp` (falls vorhanden, `grep -l FillMaterialDirectionalWindow tests/`).

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/RenderPass.cpp:62-118,137-140,165,209` (nicht in diesem Slice): hier entstehen die in REN-901/902 beschriebenen Kopien; die Aenderung gehoert in den Slice, der `RenderPass.cpp` pruefen wird.
- `src/HE_Rendering/src/RenderExtractor.cpp:349` ruft `giAutoLandscapeSliceMeans` pro Auto-Landscape und Extract (REN-913); `src/HE_Editor/EditorApplication.cpp:7740` ebenfalls.
- `src/HE_Rendering/src/Backends/{OpenGL,Metal,D3D11,D3D12}/*`: gleiche Muster wie REN-906 (Graph-Textur-Schluessel als String), REN-911 (TAA-Transform-Map), REN-908 (Bake-Treiber pro Backend, fuenfmal) und REN-912 (Preview-Frame-Aufbau und Wait) sind dort sehr wahrscheinlich ebenfalls vorhanden; gezielt mit `grep` pruefen (`graphTexKey`, `m_taaCurTransforms`, `BuildSkyEnvFaceRow`, `buildWorldPreviewFrame`).
- `src/HE_Rendering/include/HorizonRendering/ParticleShaderTemplates.h:132-150` vs. `:70-89`: das Metal-Partikel-Fragment (`heParticleGraphFragment`) hat kein Aequivalent zu `if (result.a < 0.003) discard;` der GLSL-Variante (Zeile 85). Das ist ein Verhaltensunterschied (Overdraw bei fast transparenten Partikeln, ggf. kein sichtbarer Unterschied bei Alpha-Blending), nicht Teil dieser Pruefung, aber fuer den Metal-/Partikel-Auditor relevant.
- `include/HorizonRendering/BloomKernel.h:21-24`: nur Metal nutzt den 5-Tap-Fold, die anderen Backends (`OpenGL postfx_bloom_blur.frag`, D3D11, D3D12) fahren noch den 9-Tap-Loop (laut Kommentar) – der Gewinn kann dort uebernommen werden (Shader-Auditoren der Backends).
- `include/HorizonRendering/SkyFrameParams.h:26-32`: OpenGL mappt ~54 Sky-Uniforms von Hand pro Frame (`OpenGLRenderer::DrawScene`), mit dem Hinweis "A NEW FIELD MUST BE ADDED THERE TOO". Das ist Pflege- und Performance-Thema (jeder `glUniform*`-Aufruf pro Frame) fuer den OpenGL-Auditor; ein UBO-Pfad fuer GL 4.1 ist moeglich (UBOs sind in 4.1 Kern).
- `tests/test_culling.cpp` pinnt Hashes fuer `BuildSkyEnvFace` und `BuildSkyNoise3D`; jede Aenderung an SkyEnvBake/SkyNoise3D (REN-904/908) betrifft diese Pins.

## Unsicher / Messung nötig

- REN-904: Anteil der Sky-Pass-Zeit an der GPU-Frame-Zeit (1080p/4K, Wolken aus vs. an, Low-Res-Cloud-Pfad); erst dann entscheiden, ob eine Sky-View-LUT lohnt.
- REN-905: Kosten der Aurora-Parameterberechnung pro Fragment (GPU-Timer, Aurora an/aus, Nacht); der Shader-Compiler kann `auroraRnd(fk, c)` bei entrolltem Loop teilweise konstant falten; auf Metal/Vulkan/D3D-HLSL unterschiedlich.
- REN-908: Haeufigkeit des Rebake (Zeitraffer, Day-Night-Zyklus) und Dauer pro Bake (parallel_for ueber 768 Zeilen); ob 64² Faces optisch ausreichen.
- REN-909: Dauer von `FrustumCuller::cull` bei 10k/50k Objekten; Nutzen eines Hot/Cold-Splits.
- REN-910/915: Mikrokosten (atomare Refcounts, Switch pro Instanz) – bei typischen Foliage-Szenen vermutlich < 0,1 ms; nur mit Profiler (`ExtractFoliage`-Scope existiert) entscheiden.
- REN-903: Einfluss der Cluster-Vektor-Allokation auf die CPU-Frame-Zeit in Szenen mit 50-256 Lichtern; `glm::inverse` → View-Spalte nur umsetzen, wenn der Test bitgleich bleibt.
- REN-912: GPU-Wartezeit der beiden `vkDeviceWaitIdle` bei mehreren offenen Preview-Fenstern (Profiler-Scope um den Aufruf).
- `VulkanRenderer.h:443`: Kommentar "key = hash ^ hdr-bit" passt nicht zur Signatur `GetOrBuildMaterialPipeline(…, hdr, transparent, gbuffer)` (Zeile 152-155); ob `transparent`/`gbuffer` im Schluessel stecken, steht in der .cpp (nicht in diesem Slice) – bei Abweichung Kollisionsrisiko bzw. veralteter Kommentar.
- `VulkanRenderer.h:1478-1481`: Kommentar zur Einzel-Slot-Bone-UBO ("only one skinned mesh pose renders correctly per frame") passt eventuell nicht mehr zum aktuellen Code (`m_previewBoneRing` hat pro Draw einen Block); in der .cpp pruefen.

## Gelesen

Alle 50 Dateien der Slice-Liste wurden vollstaendig gelesen (auch `SkyShaderSource.h` und `VulkanRenderer.h` komplett in Bloecken); zur Verifikation von Aufrufstellen wurden zusaetzlich einzelne Ausschnitte aus `RenderPass.cpp`, `RenderExtractor.cpp`, `VulkanRenderer.cpp` und `OpenGLRenderer.cpp` per Grep/`sed` angesehen, nicht vollstaendig.
