# Rendering Slice 3 (+ Slice 5) – D3D12-Header/-ShaderManager, D3D_Shared/HlslSources.h, MetalRenderer.h (+ MetalShaderManager)

## Zusammenfassung

Slice 3 besteht fast nur aus Deklarationen und Shader-Quelltext. Die Logik sitzt in den zugehörigen `.cpp`/`.mm`, die in anderen Slices liegen. Die Funde betreffen deshalb vor allem Struktur und Wartbarkeit (`MetalRenderer.h`) und GPU-seitige Kosten in `HlslSources.h`. Wo ein Header-Kommentar eine Laufzeitannahme macht, habe ich sie gegen das `.mm` geprüft (Zeilen angegeben). `D3D12Renderer.h` ist mit 120 Zeilen und Pimpl sauber. `HlslSources.h` ist durchdacht und gut dokumentiert. Slice 5 (Metal-Shader-Manager) ist klein und hat kein Laufzeitproblem, aber toten Code und eine Vertrags-Inkonsistenz zu den anderen vier Managern.

Anzahl Funde: Hoch 2, Mittel 7, Niedrig 12 (gesamt 21; davon Slice 5: REN-351 bis REN-354, 0 Hoch / 1 Mittel / 3 Niedrig).

## Funde (Slice 3)

### REN-301 – `MetalRenderer` ist eine 1400-Zeilen-Gottklasse mit offenem Innenleben im öffentlichen Header
- **ID**: REN-301
- **Kategorie**: A (auch F: schwerer Header)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/Metal/MetalRenderer.h:77-1415` (Includes 5-19)
- **Problem**: Der gesamte Renderzustand von rund 25 Features steht als Member im Header: etwa 340 `m_*`-Felder, davon 164 rohe `void*` für Objective-C-Objekte, die manuell mit `CFBridgingRetain`/`Release` verwaltet werden. Jedes Feature (SSR, GI-Reflexionen, DDGI, TAA, DoF, MotionBlur, Bloom, Wolken, SSAO, Decals, Previews, Thumbnails, Partikel …) wiederholt dasselbe Muster aus Pipeline-`void*`, Textur-`void*`, `m_xxxW/H`, `bool m_xxxTried`, `EnsureXxx()` und `DestroyXxx()`. Allein acht „run-once"-Flags (`...Tried`/`...Checked`: 253, 274, 364, 400, 540, 694, 1051, 1135) bilden dasselbe Lazy-Build-Muster ab. Der Header beschreibt außerdem das ganze Innenleben für jeden Includer (`RendererFactory.cpp:25`, `EditorUI.cpp:168`, `EditorApplication.cpp:271`). Die Parallelstellen D3D11 (96 Zeilen) und D3D12 (120 Zeilen) nutzen Pimpl. Der Header-Kommentar „This header stays plain C++" erreicht dasselbe Ziel schlechter.
- **Code-Auszug**:
```cpp
void* m_ssrTracePipeline     = nullptr; // id<MTLRenderPipelineState>
void* m_ssrCompositePipeline = nullptr;
void* m_ssrBlurPipeline      = nullptr;
bool  m_ssrPipelinesTried    = false;
void* m_ssrReflTex = nullptr;  int m_ssrReflW = 0, m_ssrReflH = 0;
...
bool  EnsureSSRPipelines();  void EnsureSSRTarget(int,int);  void DestroySSRTarget();
```
- **Lösung**:
  1. Schritt 1, ohne Verhaltensänderung: kleines Handle-Template (`MtlRef`, nur im `.mm`) für retained Objective-C-Objekte, das im Destruktor `CFBridgingRelease` aufruft. Damit entfallen die ~160 Destroy-Zeilen und die Leak-Gefahr bei neuen Features.
  2. Schritt 2: Feature-Substrukturen im `.mm` (`struct SsrState`, `GiReflState`, `TaaState`, `DofState`, …) mit eigenen `Ensure`/`Destroy`. Der Header hält nur noch `std::unique_ptr<MetalRendererImpl> m_impl` wie D3D11/D3D12. Public API (die `override`-Zeilen) bleibt unverändert.
  3. Ein `LazyPipeline`-Helfer (`tried`, `ptr`, `build()`) ersetzt die acht Tried-Flags.
- **Erwarteter Effekt** (Schätzung): Header um etwa 85 % kleiner. Inkrementelle Rebuilds der Editor-TUs (`EditorUI.cpp`, `EditorApplication.cpp`) werden spürbar kürzer, denn heute zieht jede Änderung an einem Feature-Member alle drei Includer nach. Neue Features brauchen deutlich weniger Boilerplate.
- **Risiko / Verifikation**: Rein mechanisch, aber riesige Diff-Fläche (`.mm` hat 18158 Zeilen). In Etappen pro Feature umsetzen. Absicherung mit den vorhandenen Culling- und Render-Tests (`tests/test_culling.cpp` greift laut Kommentar Zeile 662 auf `ExtractFrame` zu), dazu Screenshot-Vergleich (`HE_HIDDEN_WINDOW=1`) pro Feature.

### REN-302 – Pro Frame frische `MTLBuffer` trotz vorhandenem `FrameUploadRing`; Header-Kommentar rechtfertigt es mit veraltetem Stand
- **ID**: REN-302
- **Kategorie**: C / D
- **Schweregrad**: Hoch (Hot Path: skinned Objekte, Partikel, Ribbons; Größenordnung unsicher, messen)
- **Typ**: Performance
- **Ort**: Header `MetalRenderer.h:810-820` (Kommentar), `1146-1153`, `1200-1201`, `1212`, `1372`; Gegenstück `1377-1393` (Ring). Nutzstellen in `MetalRenderer.mm:13947` (Bone-Buffer je skinned Objekt), `17832` (Instanzbuffer je Partikel-Batch), `7183-7191` (Ribbon-VB/IB), `8279`, `8476-8479`, `8556-8559` (GI-Instanzbuffer je Frame).
- **Problem**: Der Kommentar bei `DrawParticleGraphBatches` behauptet, Metal gebe keine implizite Synchronisierung und „a few emitters' worth of small per-frame allocations is a non-issue". Mit dem Upload-Ring (Perf-Audit B6, `UploadTransient`, Zeilen 1377-1393) gibt es aber seit dem Kommentar eine Lösung für genau diesen Fall. Trotzdem legen mindestens diese Pfade pro Frame `newBufferWithBytes` an. Beim Bone-Buffer ist es ein 128×64 B (`kMaxBones * sizeof(glm::mat4)`) großer Buffer pro skinned Objekt und Frame. Das ist in Szenen mit vielen Charakteren ein Hot Path (Allokator-Roundtrip plus Autorelease-Druck im Pool).
- **Code-Auszug**:
```cpp
// .mm:13947
id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
                                            length:kMaxBones * sizeof(glm::mat4)
                                           options:MTLResourceStorageModeShared];
// .mm:17832
id<MTLBuffer> instBuf = [device newBufferWithBytes:inst.data() ...
```
- **Lösung**: Diese Stellen auf `UploadTransientBytes(data,len,buf,offset)` umstellen. Dann `setVertexBuffer:buf offset:offset`. Den Ribbon-Pfad zusätzlich vom Retain-Vektor `m_ribbonBuffers` lösen. Danach den veralteten Kommentar 810-820 ersetzen. Für `m_giTlas`/`m_giInstanceBuffer` (1152-1153) bleibt die „frisch pro Frame"-Strategie wegen der Acceleration-Structure-Lebensdauer ggf. nötig. Der reine Instanz-/Farb-/Land-Index-Upload kann trotzdem in den Ring.
- **Erwarteter Effekt** (Schätzung, nicht gemessen): CPU-Zeit im Encoding um einen niedrigen einstelligen Mikrosekundenbetrag pro skinned Objekt weniger. Bei einigen hundert animierten Objekten ist das ein messbarer Anteil der Frame-Zeit. Weniger Pool-Druck.
- **Risiko / Verifikation**: Offsets müssen Metal-Anforderungen erfüllen (Konstantenpuffer-Alignment 256 B bei `setVertexBuffer` nicht nötig, bei Argument-Buffern prüfen). Ring-Frame muss geöffnet sein, sonst Fallback (steht schon im Kommentar). Vorher/nachher mit dem Profiler (`GetFrameGpuStats`, CPU-Zähler) und Szene mit vielen Skinned Meshes messen.

### REN-303 – Blockierendes `waitUntilCompleted` pro neuem Mesh beim BLAS-Aufbau (Hitch bei GI-Aktivierung)
- **ID**: REN-303
- **Kategorie**: E / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: Deklaration `MetalRenderer.h:1229` (`void* BuildBLAS(const GpuMesh&)`) und `185-187`; Implementierung `MetalRenderer.mm:8150-8164`
- **Problem**: Jedes bisher ungesehene Mesh bekommt einen eigenen Scratch-Buffer, einen eigenen Command Buffer und danach ein hartes `waitUntilCompleted` auf dem Render-Thread. Beim ersten GI-Frame einer Szene mit N Meshes sind das N serialisierte CPU/GPU-Roundtrips. Der Kommentar nimmt den „kleinen Hitch" bewusst in Kauf, skaliert aber linear mit N und wird im Editor beim Einschalten von GI sichtbar.
- **Code-Auszug**:
```cpp
id<MTLBuffer> scratch = [device newBufferWithLength:... options:MTLResourceStorageModePrivate];
id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
... buildAccelerationStructure:blas ...
[cmdBuf commit];
[cmdBuf waitUntilCompleted];
```
- **Lösung**: `EncodeGIAccelBuild(cmdBuf, aspect)` bekommt schon den Frame-`cmdBuf` (Header 1235). Alle fehlenden BLAS in einem Acceleration-Structure-Encoder dieses Command Buffers bauen, mit einem gemeinsamen Scratch-Buffer (Größe = Maximum, seriell wiederverwendet; der Encoder serialisiert Builds mit Barriere). Der Scratch-Buffer geht per `RetireGIObject` (Header 1225) ans Ende. Das TLAS-Build im selben Command Buffer sieht die BLAS durch Metals Hazard-Tracking. `waitUntilCompleted` entfällt. Alternativ pro Frame auf K Meshes budgetieren (Aufbau über mehrere Frames, TLAS überspringt Instanzen ohne fertige BLAS).
- **Erwarteter Effekt** (Schätzung): Einmalige Spitze von N × (Submit + GPU-Latenz) wird zu einer Spitze von einer GPU-Zeit. Szenenabhängig, bei dutzenden Meshes im Bereich zweistelliger Millisekunden. Unsicher / messen.
- **Risiko / Verifikation**: Ordnungs- und Residency-Fragen (`useResource` für BLAS in Compute-Encodern ist in `m_giUniqueBlas` schon vorgesehen, Header 1240). Mit `HE_GI_FORCE_SW` unberührt. Prüfen mit Metal-API-Validation und einer Szene mit vielen Meshes.

### REN-304 – Bis zu vierfaches Extract/Cull/Sort pro Metal-Frame
- **ID**: REN-304
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `MetalRenderer.h:591-606` (`CullCameraObjects`), `438-441` (`EncodeGBuffer`: „extract/cull/sort of its own"), `1120-1123` (`EncodeSSAO`: „Runs its own extract/cull/sort"), `1279-1282`
- **Problem**: Der Kommentar nennt es selbst: „the Metal frame runs it up to four times (GI G-buffer, SSAO pre-pass, scene, deferred G-buffer), each after its own extraction". Gecacht ist nur der teure Occlusion-Anteil (`m_occlusionCache*`). Frustum-Culling und Sort laufen weiterhin pro Pass. Sie sind deterministisch und liefern denselben Draw-Satz, solange Kamera, Aspect und Weltstand gleich sind (der Extractor hat dafür schon einen Key-Vergleich, Header-Kommentar 655-663). Bei großen Szenen ist das ein pro-Frame-Vielfaches.
- **Code-Auszug**:
```cpp
// Header 591-594
// ... the Metal frame runs it up to four times (GI G-buffer, SSAO pre-pass,
// scene, deferred G-buffer), each after its own extraction.
void  CullCameraObjects();
```
- **Lösung**: Das Ergebnis (`m_visible` + `m_sortedIndices`) analog zum Occlusion-Cache mit Schlüssel (Frame-Stamp, Walk-Zähler des Extractors, View-Proj, Objektanzahl) halten und bei Treffer weiterverwenden. Wichtig: SSAO und GI nutzen ggf. andere Filter (z. B. Skinned oder Caster-Flags). Der Cache gilt nur für den reinen Kamera-Cull, wie `CullCameraObjects` ihn schon kapselt. Ein Trefferzähler im Profiler zeigt den Nutzen.
- **Erwarteter Effekt** (Schätzung): Bei N Objekten bis zu zwei bis drei von vier Frustum-Cull-Läufen und Sorts pro Frame gespart. Absolute Zahl hängt von N ab. Messen mit dem vorhandenen CPU-Zähler.
- **Risiko / Verifikation**: Mittel. Deferred und Forward unterscheiden sich im Aspect/Jitter (`JitteredViewProj`). Ein falscher Cache-Treffer würde Objekte fälschlich verwerfen. Test: `tests/test_culling.cpp` um Fälle mit gleichem/anderem Key erweitern; Bildvergleich Deferred/Forward/TAA an.

### REN-305 – GI: lineare Instanzschleife pro Strahl ohne grobe Vorabprüfung
- **ID**: REN-305
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h:829-859` (`giSceneAnyHit`, `giSceneClosestHit`), Aufrufer `944-951`, `919`, `1236`, `1258`
- **Problem**: Jeder Strahl (Sun-Cone, Local-Lights, Probe-Shadow-Rays, Probe-Hauptstrahl) testet alle Instanzen nacheinander. Pro Instanz werden eine 4×4- und eine 3×3-Matrixmultiplikation ausgeführt (Transformation des Strahls in den Objektraum), erst danach der Root-AABB-Test im BLAS-Traversal. Die Kosten wachsen also mit der Instanzzahl × Strahlzahl, und die Transformation ist dort die Haupt-Eintrittskosten. Der Kommentar nennt es offen „TLAS analogue: linear instance loop". `giInsts[i]` wird zudem je Iteration zweimal indiziert.
- **Code-Auszug**:
```hlsl
for (int i = 0; i < uGiCount.x; ++i)
{
    float3 oL = mul(giInsts[i].invTransform, float4(o, 1.0)).xyz;
    float3 dL = mul((float3x3)giInsts[i].invTransform, d);
    ...
    if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, tMax, true, t))
```
- **Lösung**: (a) kurzfristig: `GiInst inst = giInsts[i];` einmal laden; (b) World-Space-AABB pro Instanz (CPU schreibt sie in ein freies `offsets.zw`/Padding oder in `baseColor.w`-freie Lane, die Struktur ist 96 B bei `float4x4 + float4 + int4`) und vor der Matrix-Transformation einen Slab-Test im Weltraum machen; (c) mittelfristig ein TLAS-BVH über Instanzen (GiBvh.cpp bietet die Bausteine). Wirkt identisch auf die Parallelstellen: GL `gi_*.comp`, Vulkan, Metal `kGISWMSL`.
- **Erwarteter Effekt** (Schätzung): Bei Szenen mit Hunderten Instanzen deutlich weniger GPU-Zeit im GI-Shadow- und Probe-Pass. Bei wenigen Instanzen vernachlässigbar. Messen mit GPU-Pass-Timer des jeweiligen Backends.
- **Risiko / Verifikation**: Strukturlayout ist auf CPU-Seite (D3D11, D3D12, GL, Vulkan, Metal-SW `GISwInstanceCPU`, Header 1180) fix. Bei (b) alle fünf Parallelstellen und `GiBvh`-Tests anpassen. Bit-gleiches Ergebnis ist machbar, denn die Vorprüfung ist konservativ.

### REN-306 – Einstellungs-Structs werden in Einzelfelder zerlegt statt gespeichert
- **ID**: REN-306
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:264-269` (SSR), `339-347`, `360-361` (GI-Reflexion), `983-987` (Bloom), `1010-1013` (DoF), `1032-1035` (MotionBlur), `1114-1117` (SSAO), `1138-1145` (GI); Gegenbeispiel `647-648` (`ShadowSettings m_shadowSettings`)
- **Problem**: Für `SetBloomSettings`, `SetSSAOSettings`, `SetGISettings`, `SetSSRSettings`, `SetDepthOfFieldSettings`, … kopiert die Implementierung jedes Feld in ein eigenes Member mit teils abweichendem Namen/Typ (`m_ssrQuality` als `int`, `m_ssaoMethod` als `int`). Das sind rund 40 Felder mit doppelter Pflege: Kommt ein Settings-Feld hinzu, muss ein Member, ein Setter und jede Lesestelle angepasst werden. `m_shadowSettings` zeigt, dass es auch direkt als Struct geht.
- **Code-Auszug**:
```cpp
bool  m_ssrEnabled      = false;
float m_ssrIntensity    = 1.0f;
float m_ssrMaxRoughness = 0.6f;
float m_ssrMaxDistance  = 30.0f;
float m_ssrThickness    = 0.5f;
int   m_ssrQuality      = 1;      // 0 = 16 steps, 1 = 32, 2 = 64
```
- **Lösung**: `SSRSettings m_ssr;` (und analog) halten, Setter wird zu `m_ssr = s;` plus ggf. Dirty-Flag. Lesestellen `m_ssr.intensity`. Schritt für Schritt je Feature. Es hilft zugleich REN-301.
- **Erwarteter Effekt** (Schätzung): Etwa 40 Member und 8 Setter-Rümpfe weniger. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Niedrig, aber jede Lesestelle im 18k-Zeilen-`.mm` muss gefunden werden (Compiler hilft beim Umbenennen). Wo der Setter Seiteneffekte hat (Target-Neuanlage bei Qualitätswechsel), diese erhalten.

### REN-307 – Sieben handgeschriebene Offscreen-Ziele (Color/Depth/Größe) für Previews und Thumbnails
- **ID**: REN-307
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:701-706` (Material), `713-717` (Skeletal), `733-741` (`WorldPreviewTarget`), `747-750` (Partikel), `758-767` (Thumbnail + UI-Thumbnail); allgemeiner dieselbe Struktur bei HDR/LDR/Bloom/DoF/MB/UC/Cloud (`828-831`, `938-941`, `979-982`, `1004-1009`, `1028-1031`, `1048-1051`, `1062-1064`)
- **Problem**: Das Paar „Color-Textur + Depth-Textur + Größe + EnsureXxx/DestroyXxx" ist rund zehnmal von Hand angelegt, obwohl `WorldPreviewTarget` (733) das Muster schon als Struct zeigt. Jede Kopie hat ihre eigene Resize- und Release-Logik; sie driften bereits (z. B. teils `RetireTexture`, teils direkt freigegeben).
- **Code-Auszug**:
```cpp
void* m_previewColorTex = nullptr;  void* m_previewDepthTex = nullptr;  int m_previewSize = 0;
void* m_skelPreviewColorTex = nullptr; void* m_skelPreviewDepthTex = nullptr; int m_skelPreviewW = 0, m_skelPreviewH = 0;
void* m_particlePreviewColorTex = nullptr; void* m_particlePreviewDepthTex = nullptr; int m_particlePreviewSize = 0;
```
- **Lösung**: Ein `struct OffscreenTarget { void* color; void* depth; int w, h; bool ensure(device, fmt, w, h); void release(retire); }` im `.mm`. Alle Previews und Thumbnails nutzen es.
- **Erwarteter Effekt** (Schätzung): Etwa 150 bis 250 Zeilen `.mm` weniger. Einheitliches Retire-Verhalten.
- **Risiko / Verifikation**: Niedrig. Formate unterscheiden sich (RGBA16F vs. BGRA8, 765), also als Parameter. Smoke-Test über die Preview-/Thumbnail-Tests im Editor (`HE_UI_DUMP_DIR`).

### REN-308 – TAA-Transform-Historie als zwei `unordered_map` mit Per-Frame-Einträgen
- **ID**: REN-308
- **Kategorie**: C / D
- **Schweregrad**: Mittel (nur bei aktivem TAA, objektzahlabhängig)
- **Typ**: Performance
- **Ort**: `MetalRenderer.h:898-899`; Nutzung `MetalRenderer.mm:12973-13029`
- **Problem**: Jedes Frame wird `m_taaCurTransforms.clear()` aufgerufen und danach pro Objekt `m_taaCurTransforms[entityId] = transform` eingefügt. Danach folgt `swap`. `std::unordered_map` allokiert pro Eintrag einen Knoten (`malloc` pro Entity pro Frame) und der Lookup in der Vorgänger-Map ist ein weiterer Hash-Zugriff. Bei tausenden Objekten sind das tausende Allokationen pro Frame im Velocity-Pass.
- **Code-Auszug**:
```cpp
std::unordered_map<uint32_t, glm::mat4> m_taaPrevTransforms;
std::unordered_map<uint32_t, glm::mat4> m_taaCurTransforms;
// .mm
const auto it = m_taaPrevTransforms.find(obj.entityId);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Zwei `std::vector<std::pair<uint32_t,glm::mat4>>` (oder ein Flat-Hash-Map), die pro Frame mit `clear()` (Kapazität bleibt) gefüllt und nach `entityId` sortiert werden. Alternativ dichter Vektor mit Slot-Index pro Entity, falls die IDs dicht sind. Der Lookup ist dann ein Binary Search oder Index. Das `swap` bleibt.
- **Erwarteter Effekt** (Schätzung): Null Heap-Allokationen im eingeschwungenen Zustand statt einer pro Objekt und Frame; Velocity-Pass-CPU-Zeit bei großen Szenen messbar kleiner. Messen.
- **Risiko / Verifikation**: Niedrig. Verhalten für „erstmals gesehen → keine Bewegung" (12994-13006) muss bleiben. Test: zwei Frames mit bewegtem/neuem Objekt, Velocity-Ausgabe vergleichen.

### REN-309 – Veraltete Kommentare bei GI und Header-Größenangaben
- **ID**: REN-309
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:1130-1134`, `1284-1298`; `MetalRenderer.mm:2686-2690` vs. `3645-3647`; `HlslSources.h:3-5`
- **Problem**: (a) Header 1130-1134: „This checkpoint only builds the structures; the ray-traced shadow pass and DDGI probe pass that consume them land in later checkpoints." Beides ist längst vorhanden (`EncodeGIShadowRays`, `EncodeGIProbeUpdate`, Zeilen 1282, 1318). (b) Header 1292-1297: „no secondary shadow ray at the hit point (hit surfaces are treated as fully sun-lit)", aber der SW-Kernel hat sie (`.mm:3645-3647` „hit surfaces are no longer assumed fully sun-lit"), ebenso die HLSL-Variante (`HlslSources.h:1234-1237`). Der HW-Kernel-Kommentar `.mm:2686-2690` behauptet weiterhin das Gegenteil. Entweder ist der HW-Kernel wirklich zurück (dann ist es eine Verhaltensabweichung zwischen HW und SW-Pfad) oder der Kommentar ist falsch. Unsicher, im Kernel prüfen. (c) `HlslSources.h:3-4` spricht von „~750 lines" bei 1352 Zeilen.
- **Code-Auszug**:
```cpp
// no secondary
// shadow ray at the hit point (hit surfaces are treated as fully sun-lit) ...
```
- **Lösung**: Kommentare auf den Ist-Stand bringen. Für (b) zuerst den HW-Kernel (`.mm` um 2680-2700) gegen den SW-Kernel lesen. Fehlt der Sekundärstrahl dort, ist das ein Funktionsfund für den `.mm`-Slice (Probe-Beleuchtung weicht zwischen HW und SW ab).
- **Erwarteter Effekt**: Weniger Fehlinformation für die nächste Person; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Keines (nur Kommentare), außer (b) deckt eine echte Abweichung auf.

### REN-310 – Zwei identische Retire-Queues
- **ID**: REN-310
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:1223-1227` (`RetiredGIObject`, `RetireGIObject`, `AgeRetiredGIObjects`, `DrainRetiredGIObjects`) und `1352-1356` (`RetiredTexture`, `RetireTexture`, `AgeRetiredTextures`, `DrainRetiredTextures`)
- **Problem**: Beide haben dieselbe Form (`{void*, int framesLeft}`, Vektor, Retire/Age/Drain). Der Kommentar bei 1219-1222 sagt selbst „same lifetime problem and fix as m_retiredTextures/RetireTexture, generalised to any retained Objective-C object". Dann reicht die generalisierte Variante.
- **Code-Auszug**:
```cpp
struct RetiredGIObject { void* object; int framesLeft; };
struct RetiredTexture  { void* texture; int framesLeft; };
```
- **Lösung**: `RetireTexture(t)` durch `RetireObject(t)` ersetzen, eine Queue, ein `Age`, ein `Drain`.
- **Erwarteter Effekt** (Schätzung): Etwa 30 Zeilen weniger, eine Stelle statt zwei beim Anpassen der Verzögerung.
- **Risiko / Verifikation**: Niedrig; die Verzögerungsanzahl (`framesLeft`-Startwert) muss in beiden Fällen gleich sein oder bleibt parametrisiert.

### REN-311 – `ResolveGraphTexture` baut einen `std::string`-Schlüssel pro Aufruf
- **ID**: REN-311
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: Deklaration `MetalRenderer.h:509`, `1330-1331`; Implementierung `MetalRenderer.mm:9511-9517`; Aufrufer u. a. `.mm:7179` (Ribbons, pro Frame), `13567`, `14457` (nur im Cache-Miss-Block, dort unkritisch)
- **Problem**: Der Schlüssel entsteht jedes Mal aus zwei `std::to_string` und einer String-Verkettung (plus `#arr`); danach ein String-Hash. Das ist pro Textur-Slot und Aufruf eine Heap-Allokation (Schlüssel länger als die SSO-Grenze). Wo der Aufruf pro Frame läuft (Ribbon-Kollektion, 7179), summiert es sich.
- **Code-Auszug**:
```cpp
std::string key = texId != HE::UUID{}
    ? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
...
if (array) key += "#arr";
```
- **Lösung**: Schlüsseltyp `struct GraphTexKey { HE::UUID id; uint8_t array; }` (Hash über `hi/lo`) plus getrennte Map für pfadbasierte Texturen. Dann entfällt jede Stringbildung für UUID-Texturen.
- **Erwarteter Effekt** (Schätzung): Pro Aufruf eine Allokation und mehrere hundert Nanosekunden weniger. Praktisch nur relevant bei vielen Ribbon-/Material-Batches pro Frame. Messen.
- **Risiko / Verifikation**: Niedrig. `m_uiTexCache` teilt die Schlüssel-Konvention (.mm 9531), beide gemeinsam umstellen. `.mm:16543-16550` (Invalidate) iteriert über die Maps und muss mitziehen.

### REN-312 – HLSL-Quelltext wiederholt dieselben Deklarationen in jeder Konstante
- **ID**: REN-312
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HlslSources.h:116-122`, `705-711`, `1301-1307` (identischer `PerObject`-Cbuffer), `135`, `1317` (identisches `VSOut`), `In`/`FsIn`-Struct in `205`, `369`, `407`, `450`, `515`, `624`, `678`, `989`, `1071`
- **Problem**: Der `PerObject`-Cbuffer und das Skinned-`VSOut` stehen dreimal bzw. zweimal wortgleich da. Das Fullscreen-Eingangs-Struct `{float4 pos : SV_POSITION; float2 uv : TEXCOORD0;}` steht neunmal. Layout-Änderungen (z. B. ein weiteres Feld in `PerObject`) müssen an drei Stellen und zusätzlich in `kSceneHLSL` beider Backends (nicht in diesem Slice) nachgezogen werden. Die Kommentare (z. B. 703 „Reuses the PerObject cbuffer layout") zeigen, dass das Kopplung ist, die nur im Kopf der Autoren gesichert wird.
- **Code-Auszug**:
```hlsl
cbuffer PerObject : register(b0)
{ float4x4 uMVP; float4x4 uModel; float4 uColor; float4 uPBR; };
```
- **Lösung**: Kleine Prelude-Konstanten (`kPerObjectCBHLSL`, `kFsInHLSL`), die die Backends wie `kSkyFuncHLSL` schon per `std::string(...) + ...` voranstellen (z. B. `D3D11Renderer.cpp:4145`, `D3D12Renderer.cpp:2844`). Alternativ ein `#include`-Handler im `D3DCompile`-Aufruf.
- **Erwarteter Effekt**: Rund 40 Zeilen weniger, eine Quelle für das Cbuffer-Layout. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig. Byte-Identität der kompilierten Shader prüfen (Hash des Bytecodes vorher/nachher). Der Registerbezug (`b0`) bleibt.

### REN-313 – GI-Denoiser: teure Pro-Tap-Mathematik und inkonsistente `Sample`/`SampleLevel`
- **ID**: REN-313
- **Kategorie**: D
- **Schweregrad**: Niedrig (unsicher, messen)
- **Typ**: Performance
- **Ort**: `HlslSources.h:1100-1112` (A-Trous, 25 Taps × zwei Iterationen), `1034-1048` (Temporal, 5×5-Fenster), `1005-1008`/`1093-1096` (Footprint-Berechnung doppelt)
- **Problem**: (a) A-Trous: pro Tap werden `exp`, `pow(x, 32)`, ein zweites `exp`, ein `normalize` und zwei Texturzugriffe auf `uGNorm`/`uGPos` ausgeführt. `pow(max(dot,0), 32)` lässt sich durch fünf Quadrierungen ersetzen. Die gespeicherte Normale ist bereits normiert (`GiGBufPS`, Zeile 741), das erneute `normalize` kostet nur dann etwas Genauigkeit, wenn das Zielformat quantisiert. (b) Temporal: 25 Fetches und 9 Summen über je 9 Werte (81 Additionen); mit Spaltensummen (5 Spalten × 3-Fenster) ginge es mit etwa 15 Additionen. (c) Der Footprint (4 Nachbar-Taps plus `min`/`max`) wird in Temporal und A-Trous identisch berechnet (Kommentar „same constants (SYNC)"), eine Zwischenspeicherung im G-Buffer wäre denkbar. (d) `kGiTemporalHLSL` nutzt `Sample` hinter divergenten frühen `return`s (996-1000); der A-Trous-Kommentar 1058-1059 nennt genau das als Grund für `SampleLevel`. Bei Point-Sampler/Single-Mip ist das Ergebnis gleich, aber FXC kann dafür Warnungen oder Gradient-Anomalien erzeugen.
- **Code-Auszug**:
```hlsl
w *= exp(-abs(dot(n, q.xyz - pv.xyz)) / (1.0 * fp));
w *= pow(max(dot(n, normalize(uGNorm.SampleLevel(uPointSamp, uv, 0).xyz)), 0.0), 32.0);
w *= exp(-abs(v - c) / (2.0 * sig + 1e-3));
```
- **Lösung**: `pow` durch Quadrierungskette ersetzen (Ergebnis nicht bit-gleich, aber innerhalb Rundungsfehler; deshalb per GL/Metal-Parallelstelle gleichzeitig ändern, sonst driften die Backends); `Sample` → `SampleLevel(…, 0)` in `kGiTemporalHLSL`; `1.0 * fp` bereinigen (reines Rauschen).
- **Erwarteter Effekt** (Schätzung): Gering (halbaufgelöster Pass); nur bei GI-lastigen Szenen relevant. Ohne GPU-Timer keine Zusage. Unsicher / messen.
- **Risiko / Verifikation**: Visuelle Änderung möglich. Parallelstellen: GL `gi_atrous.frag`, `gi_temporal.frag`, Metal-Pendants. GI-Bildvergleich.

### REN-314 – BLAS-Traversal ohne Kind-Reihenfolge (Closest-Hit der Probes)
- **ID**: REN-314
- **Kategorie**: D
- **Schweregrad**: Niedrig (unsicher, messen)
- **Typ**: Performance
- **Ort**: `HlslSources.h:784-825` (`giBlasHit`), Aufrufer `843-859`, `1220`
- **Problem**: Beim Absteigen werden beide Kinder auf den Stack gelegt (`stack[sp++] = ...left; stack[sp++] = ...left+1`), und das rechte wird zuerst abgearbeitet, unabhängig davon, welches näher am Strahlursprung liegt. Für Any-Hit (Schatten) ist das egal. Für Closest-Hit (Probe-Update, `giSceneClosestHit`) wird dadurch `best` später verkleinert, und mehr Knoten werden besucht. Zusätzlich liegt ein `int stack[64]` pro Thread im Registerdruck-Bereich (GPU-Occupancy).
- **Code-Auszug**:
```hlsl
else if (sp + 2 <= 64)
{
    stack[sp++] = nodeOfs + leftFirst;
    stack[sp++] = nodeOfs + leftFirst + 1;
}
```
- **Lösung**: Beide Kind-AABBs testen und das nähere zuletzt pushen (zuerst abgearbeitet) bzw. nur bei `anyHit == false`. Teurer pro Knoten, aber weniger Knoten. Nur mit Messdaten durchführen.
- **Erwarteter Effekt**: Unbekannt; hängt von BVH-Qualität und Szene ab. Unsicher / messen.
- **Risiko / Verifikation**: Ergebnis des Closest-Hit bleibt gleich (nur Besuchsreihenfolge). Parallelstellen GL/Vulkan/Metal gleichzeitig ändern, damit Rauschmuster/Zeitverhalten nicht driften.

### REN-315 – SSAO leitet die Texelgröße aus dem Noise-Scale ab (versteckte Kopplung)
- **ID**: REN-315
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `HlslSources.h:222`, Konstanten `198-204`
- **Problem**: Die Nachbar-Offsets für die Normalenrekonstruktion nutzen `rcp(uSSAONoiseScale.xy * 4.0f)`. Das funktioniert nur, solange `uSSAONoiseScale = Auflösung / 4` gilt (4×4-Noise-Kachel). Wird die Noise-Kachelgröße oder die Berechnung des Scale geändert, ändert sich still der Rekonstruktionsradius. Zudem benutzen HBAO (236-279) und GTAO (280-334) fast identischen Slice-Setup-Code (`V`, `jitter`, `depthScale`, `omegaUV`, `projN`, `axis`).
- **Code-Auszug**:
```hlsl
float2 texel = rcp(float2(uSSAONoiseScale.xy * 4.0f));
```
- **Lösung**: Texelgröße explizit im Cbuffer übergeben (ein Platz in `uSSAOParams` oder `uSSAONoiseScale.zw` ist zu klären; die CPU-Seite in D3D11/D3D12, GL und Metal-Spiegelung ändern). Die gemeinsamen Teile von HBAO/GTAO in eine Hilfsfunktion `ssaoSliceSetup()`.
- **Erwarteter Effekt**: Lesbarkeit; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Erfordert Cbuffer-Layout-Änderung auf der CPU-Seite in zwei Backends; bildgleich testbar (AO-Screenshots für die drei Methoden).

### REN-316 – `MetalRenderer.h` zieht 14 schwere Header für jeden Includer
- **ID**: REN-316
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalRenderer.h:2-24`
- **Problem**: Der Header bindet unter anderem `RenderExtractor.h`, `OcclusionCuller.h`, `RenderGraph.h`, `CommandBuffer.h`, `GiBvh.h`, `GIProbeGrid.h`, `MaterialShaderLibrary.h` und `<mutex>`/`<atomic>` ein, nur weil Member-Typen by value im Header stehen. `RendererFactory.cpp` braucht nur den Klassennamen und den Konstruktor; die Editor-TUs nur die Handle-Zugriffe. `<limits>` wird nur für ein `quiet_NaN` (1086), `<mutex>`/`<atomic>` nur für `MetalGpuTimerShared` (32-40) gebraucht.
- **Code-Auszug**:
```cpp
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/OcclusionCuller.h>
#include <HorizonRendering/GiBvh.h>
#include <material/MaterialShaderLibrary.h>
```
- **Lösung**: Folgt aus REN-301 (Pimpl). Zwischenlösung: Vorwärtsdeklarationen und `std::unique_ptr<…>` für schwere Member sowie `MetalGpuTimerShared` ins `.mm` verschieben.
- **Erwarteter Effekt** (Schätzung): Kürzere Compile-Zeit der drei Includer; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Link-/Compile-Check auf macOS.

### REN-317 – `D3D12Renderer` besitzt `m_impl` per rohem `new/delete` ohne Kopierschutz
- **ID**: REN-317
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D12Renderer.h:119` (`D3D12RendererImpl* m_impl = nullptr;`), `D3D12Renderer.cpp:9807` (`~D3D12Renderer() { delete m_impl; }`); Parallelstelle `D3D11Renderer.h:95`
- **Problem**: Ein roher besitzender Zeiger mit implizit erzeugtem Kopierkonstruktor/-zuweisung: Eine versehentliche Kopie führte zu Doppel-Delete. Ob `IRenderer` die Kopie schon verbietet, habe ich nicht geprüft (unsicher).
- **Code-Auszug**:
```cpp
D3D12RendererImpl* m_impl = nullptr;
```
- **Lösung**: `std::unique_ptr<D3D12RendererImpl> m_impl;` (Destruktor ist bereits out-of-line deklariert, `D3D12Renderer.h:21`, also ist ein unvollständiger Typ unproblematisch). Gleiches für D3D11.
- **Erwarteter Effekt**: Sicherheit, 1 Zeile weniger.
- **Risiko / Verifikation**: Praktisch keines; Windows-Build.

## Funde (Slice 5 – Metal-Shader-Manager)

### REN-351 – `MetalShaderManager` (und die vier Schwestern) haben keine Produktionsaufrufer für `load`/`compile`/`createProgram`
- **ID**: REN-351
- **Kategorie**: F / A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/Metal/MetalShaderManager.h:1-35`, `src/Backends/Metal/MetalShaderManager.mm:1-138`; Nutzung `MetalRenderer.mm:6659` (`setDevice`), `6697` (`cleanup`); Schwestern `D3D12ShaderManager.{h,cpp}`, `D3D11ShaderManager`, `OpenGLShaderManager`, `VulkanShaderManager`
- **Problem**: In `MetalRenderer.mm` wird nur `setDevice` und `cleanup` aufgerufen. Alle echten Pipelines baut der Renderer selbst (`newLibraryWithSource` im Renderer, `MaterialShaderLibrary`). Das Grep über `src/` nach den Managern liefert außer den Renderern, den Manager-Dateien und `tests/test_d3d_shader_manager.cpp` nur Kommentare. Die D3D12-Header-Notiz bestätigt es: „D3D12Renderer does not route its shaders through this class" (`D3D12ShaderManager.h:283`). OpenGL/Vulkan weisen den Manager nur zu (`OpenGLRenderer.cpp:3372`, `VulkanRenderer.cpp:216`). Die Klassen sind damit kompilierter, getesteter, aber ungenutzter Code mit fünf Implementierungen und einem Interface (`ShaderManager.h`).
- **Code-Auszug**:
```cpp
// MetalRenderer.mm:6659 / 6697
m_shaderManager.setDevice(m_device);
m_shaderManager.cleanup();
```
- **Lösung**: Entscheiden: (a) entfernen samt Tests und Interface, oder (b) tatsächlich verwenden (z. B. für die bei `MetalRenderer` handgepflegten Spiegel-Shader, damit Fehler einheitlich geloggt werden). Mindestens im Header als „nur Tests / Legacy" kennzeichnen. Vor dem Entfernen öffentliche Verwendung durch externe Module (HE_Python, Plugins) prüfen: im `src/`-Baum habe ich keine gefunden, externe Verbraucher sind unsicher.
- **Erwarteter Effekt** (Schätzung): Etwa 500 bis 600 Zeilen weniger über alle Backends, kleinere Testfläche.
- **Risiko / Verifikation**: Niedrig, wenn Grep über das gesamte Repo (inkl. `docs/`, Tools) bestätigt, dass nichts den Manager nutzt. Build aller Backends.

### REN-352 – Dateilesecode fünffach kopiert und doppelt ineffizient
- **ID**: REN-352
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `MetalShaderManager.mm:371-389`; wortgleich `D3D12ShaderManager.cpp:147-166`, `D3D11ShaderManager.cpp:~35`, `OpenGLShaderManager.cpp:~24`, `VulkanShaderManager.cpp:~25`
- **Problem**: `fs::exists` + `std::ifstream` + `is_open` + `stringstream << rdbuf()` + `buffer.str()` ist in jedem Manager derselbe Block mit nur anderem Präfix im Fehlertext. Dabei ist die `exists`-Vorprüfung redundant zu `is_open` (zweiter Syscall plus Race), und der Weg über `stringstream` kopiert den Inhalt zweimal (Stream-Puffer, dann `str()`).
- **Code-Auszug**:
```cpp
if (!fs::exists(path)) { HE_LOG_ERROR(RHI, "%s", (std::string("MetalShaderManager: shader file not found: ") + path).c_str()); return { 0, false }; }
std::ifstream file(path);
...
std::stringstream buffer; buffer << file.rdbuf();
m_pendingSources[handle.id] = buffer.str();
```
- **Lösung**: Ein geschützter Helfer in `ShaderManager` (`static bool readSourceFile(const char* path, std::string& out, const char* who)`), der `ifstream` binär öffnet, mit `seekg`/`tellg` vorab dimensioniert und einmal liest. Fehlertext: „nicht gefunden" bei `errno==ENOENT` beibehalten.
- **Erwarteter Effekt**: Rund 60 Zeilen weniger, eine Kopie weniger. Laufzeit irrelevant (Startpfad).
- **Risiko / Verifikation**: Niedrig; Tests `test_d3d_shader_manager.cpp` prüfen die Fehlertexte ggf. wörtlich. Dann Texte unverändert lassen. Hinweis: `std::ifstream` im Textmodus konvertiert unter Windows CRLF, im Binärmodus nicht; für HLSL/MSL-Quelltext unkritisch, aber Verhalten erwähnt.

### REN-353 – `releaseProgram` hat auf Metal andere Besitzsemantik als in den anderen Managern
- **ID**: REN-353
- **Kategorie**: A (Vertrag)
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MetalShaderManager.mm:455-479` (`createProgram`/`releaseProgram`) gegen `D3D12ShaderManager.cpp:209-236`, `VulkanShaderManager.cpp:105-110`; Vertrag `ShaderManager.h` („Every resource tied to that program should be released with it.")
- **Problem**: Metal gibt in `releaseProgram` die Stage-Funktionen aller enthaltenen Shader frei (`for (shader : handle.shaders) release(shader)`). D3D12 und Vulkan lassen die Shader unberührt, D3D12 entfernt nur den Programmeintrag. Wird ein Shader in zwei Programmen verwendet oder nach `releaseProgram` weiterbenutzt, funktioniert es in D3D12 und scheitert in Metal („compile called on unknown handle" / „uncompiled shader"). Metal führt keine Programmliste (`createProgram` merkt sich nichts, `cleanup` kann daher keine Programme freigeben, was dort unkritisch ist, weil Programme keine GPU-Ressource sind). Ein doppeltes `releaseProgram` ist auf Metal harmlos, aber die Semantik ist implizit.
- **Code-Auszug**:
```cpp
void MetalShaderManager::releaseProgram(ShaderProgramHandle handle)
{
	for (const auto& shader : handle.shaders)
		release(shader);
}
```
- **Lösung**: Den Vertrag in `ShaderManager.h` explizit machen (gehören Shader dem Programm oder nicht) und alle fünf Manager angleichen. Wenn REN-351 (b) nicht umgesetzt wird, wird das Thema mit dem Entfernen hinfällig.
- **Erwarteter Effekt**: Konsistenz; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Verhaltensänderung für jeden Aufrufer. Tests pro Backend schreiben (Shader in zwei Programmen, `releaseProgram` dann weiterverwenden).

### REN-354 – D3D12-Manager behält Quelltext nach `compile()` und führt dreifache Buchführung
- **ID**: REN-354
- **Kategorie**: C / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `D3D12ShaderManager.cpp:164-165`, `201-205`; `D3D12ShaderManager.h:306-311` (Kommentar „consumed by compile()"); Gegenbeispiel `MetalShaderManager.mm:449` (`m_pendingSources.erase(srcIt)`)
- **Problem**: Der Header sagt, `m_sources` werde von `load()` gefüllt und von `compile()` „consumed", aber `compile()` löscht den Eintrag nie. Damit bleibt der volle HLSL-Text bis `release`/`cleanup` im Speicher (bei großen Shadern dutzende KB pro Handle). Außerdem werden pro Shader drei Container parallel gepflegt (`m_sources`, `m_blobs`, `m_loadedShaders`), und `compile` sucht in `m_loadedShaders` linear nach dem Handle (`for (auto& h : m_loadedShaders) if (h.id == handle.id)`), `release` per `remove_if`.
- **Code-Auszug**:
```cpp
m_blobs[handle.id] = blob;
handle.ready = true;
for (auto& h : m_loadedShaders)
    if (h.id == handle.id) h.ready = true;
```
- **Lösung**: Entweder den Quelltext nach erfolgreichem Compile freigeben (dann ist Recompile desselben Handles nicht mehr möglich, wie bei Metal; der Header-Kommentar stimmt dann) oder den Kommentar korrigieren und bewusst behalten (Hot-Reload). Die drei Container durch `unordered_map<id, Entry{source, blob, ready}>` ersetzen; `loadedShaderCount()` wird `map.size()`.
- **Erwarteter Effekt** (Schätzung): Etwas Speicher; weniger Zeilen. Praktisch ohne Laufzeitwirkung.
- **Risiko / Verifikation**: `test_d3d_shader_manager.cpp` prüft Zähler und Refcounts (laut Header-Kommentar 280-281). Das Verhalten „compile ein zweites Mal" ist dort evtl. abgedeckt, vorher lesen.

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm` (18158 Zeilen): Ergänzend zu REN-302 gibt es `newBufferWithBytes` auch bei `10509`, `10844` (Previews, unkritisch) und `10947`; dort Hot-Path-Relevanz prüfen. Zwölf `waitUntilCompleted`: unkritisch sind die Editor-Pfade (Previews/Thumbnails/Capture `9998`, `10078`, `10221`, `10600`, `10907`, `11135`, `11281`) und der Profiler-Pfad (`16636`, `17372`, laut Kommentar „capture is slow"); kritisch ist nur `8164` (REN-303).
- `.mm:7183-7191` und `m_ribbonBuffers` (Header 1372): Die Freigabe „on the NEXT frame's collect, by which time the command buffer … is long done" gilt nur, wenn höchstens ein Frame in Flight ist. Ob `WaitForFrame` (`.mm:17535`) das erzwingt, ist unsicher und im `.mm`-Slice zu prüfen (Dangling-GPU-Referenz, wenn mehrere Frames in Flight sind).
- `src/HE_Rendering/src/Backends/D3D12/D3D12Renderer.cpp`: 27 `D3DCompile`-Aufrufe (`D3D11Renderer.cpp`: 30) und kein persistenter Shader-/PSO-Cache (kein `ID3D12PipelineLibrary`/CachedPSO gefunden), obwohl Metal mit `MTLBinaryArchive` (Header 690-697) einen hat. Startzeit-Thema (Kategorie E), im jeweiligen Renderer-Slice bewerten.
- `D3D12Renderer.cpp:2844`, `2955`: Der Sky läuft bevorzugt über den cross-kompilierten GL-Sky (`skyFullModel`), die HLSL-Fassung `kSkyPSHLSL12` ist nur Fallback. Damit gibt es für den Sky weiterhin drei bis vier Handkopien (GL `kSkyFS`, `HlslSources.h` `kSkyFuncHLSL`, `kSkyPSHLSL12`, Metal `kSkyMSL`). `CLAUDE.md` nennt `kSkyFS` die „single source"; das gilt für die D3D-Fallback-Pfade nicht.
- `HlslSources.h` ist laut eigenem Kommentar mit GL/Metal handgespiegelt (kGi*, kTaa*, kSSAO*, kUnderwater*). Änderungen an den Algorithmen (REN-305, REN-313, REN-314, REN-315) müssen daher in `OpenGLRenderer.cpp`, den Vulkan-Shadern und `MetalRenderer.mm` (`kGISWMSL`, `kGIReflMSL`, `kTaaMSL`) gleichzeitig erfolgen.
- Gleiche Struktur wie REN-301 (Gottklasse im Header): `include/Backends/OpenGL/OpenGLRenderer.h` (1397 Zeilen) und `include/Backends/Vulkan/VulkanRenderer.h` (1566 Zeilen).

## Unsicher / Messung nötig

- REN-302: tatsächliche Zeit von `newBufferWithBytes` je skinned Objekt auf Apple-Silicon und Intel/AMD-Macs (Profiler-CPU-Zähler).
- REN-303: GI-Aktivierungs-Hitch in Szenen mit vielen Meshes (Frame-Zeit der ersten GI-Frames).
- REN-304: Anteil von Frustum-Cull und Sort an der CPU-Frame-Zeit bei großen Szenen.
- REN-305, REN-313, REN-314: GPU-Zeiten der GI-Pässe je Backend (GPU-Pass-Timer), erst danach optimieren.
- REN-309 (b): Ob der HW-Probe-Kernel (`MetalRenderer.mm` um 2680-2700) den Sekundärschattenstrahl wirklich nicht hat (dann HW/SW-Abweichung).
- REN-317: Ob `IRenderer` Kopieren verbietet.
- REN-351: externe Nutzer der `ShaderManager`-Klassen außerhalb von `src/`.
- Nicht aufgenommen, weil vermutlich vom Compiler zusammengefasst: doppeltes Laden von `uCurrent`/`uVelocity` im TAA-Resolve (`HlslSources.h:630-645`, `657-661`); im Shader-Disassembly prüfen, falls das Pass-Budget knapp ist.
- Der Kommentar in `kGiShadowCSHLSL` (`963-966`, `break`/`continue` hebelten `[unroll]` aus) passt nicht zum A-Trous-Shader, der `[unroll]` mit `continue` verwendet (`1100-1105`, kompiliert offenbar). Die tatsächliche Ursache von X3511/X3500 ist vermutlich das dynamisch indizierte Vektor-Lvalue, nicht `continue`. Der Kommentar ist ggf. ungenau (Verständnisfrage, kein Funktionsfehler).

## Gelesen

Ja, vollständig: `D3D12Renderer.h`, `D3D12ShaderManager.cpp`, `D3D12ShaderManager.h`, `HlslSources.h` (1-1352), `MetalRenderer.h` (1-1415) sowie Slice 5 `MetalShaderManager.h` und `MetalShaderManager.mm`. Zur Verifikation von Header-Annahmen wurden nur gezielte Ausschnitte von `MetalRenderer.mm`, `D3D12Renderer.cpp`, `ShaderManager.h` und den Schwester-Managern gelesen (nicht vollständig, sie gehören zu anderen Slices).
