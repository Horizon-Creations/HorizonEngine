# Rendering Slice 1 – D3D11-Backend (D3D11Renderer/ShaderManager/MaterialBindings) + D3D12MaterialRootSignature.h

Dateien (alle unter `src/HE_Rendering/`): `include/Backends/D3D11/D3D11MaterialBindings.h`, `src/Backends/D3D11/D3D11Renderer.cpp` (8601 Z.), `include/Backends/D3D11/D3D11Renderer.h`, `src/Backends/D3D11/D3D11ShaderManager.cpp`, `include/Backends/D3D11/D3D11ShaderManager.h`, `include/Backends/D3D12/D3D12MaterialRootSignature.h`.

## Zusammenfassung

Der Code ist gründlich kommentiert, funktional sauber und für D3D11 sinnvoll aufgebaut (Instancing für Depth-/Szenen-Pässe, Dynamic-CBs mit `MAP_WRITE_DISCARD`, GPU-Timer-Ring ohne Stall). Die größten Hebel liegen im Hot Path der Draw-Schleife (`drawDC`, Graph-Material-Zweig): dort werden pro Draw GLSL-Strings kopiert und gehasht, Textur-Schlüssel als `std::string` gebaut und rund 25 Binds + Restores abgesetzt. Danach folgen per-Frame-Doppelarbeit (Cluster-Build/Upload zweimal, GI-Instanzbuffer wird jedes Frame neu angelegt, volle Probe-Atlas-Kopie), eine ca. 1500 Zeilen lange `DrawScene`-Funktion und ein 4700-Zeilen-`Impl`-Struct mit vielfach kopierten Lambdas (`makeRT` 6x, `makeCB` 6x). Insgesamt 23 Funde: 3 Hoch, 10 Mittel, 10 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten (siehe "Unsicher / Messung nötig").

---

### REN-101 – Graph-Material-Draw kopiert und hasht pro Draw den kompletten GLSL-Fragment-String
- **ID**: REN-101
- **Kategorie**: D (wiederholtes Hashing/Kopieren im Hot Path), C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7006-7041` (Aufrufer); `src/material/MaterialShaderLibrary.cpp:941-952, 1000-1009` (Slice 10, Ursache); `D3D11Renderer.cpp:7021-7022`
- **Problem**: Für jeden Draw eines Graph-Materials ruft `drawDC` `resolveShaders()` auf. Das kopiert `mat->customShaderFragGlsl` (typisch mehrere KB, Heap-Allokation) nach `matFrag`, berechnet `std::hash<std::string>` über den ganzen Text, kopiert zusätzlich `customShaderVertGlsl` und hasht es erneut. Erst danach folgt der `m_materialShaders`-Lookup per Hash, der in der Regel ein Cache-Treffer ist, sodass Kopie und Hash umsonst waren. Zusätzlich wird `precompiledFor(getMaterial(...))` (Z.7021) berechnet, bevor man weiß, dass es gebraucht wird (nur bei Cache-Miss nötig), und `getMaterial(dc.materialAssetId)` wird im selben Draw noch dreimal gelöst (Z.7022, 7077, 7114) plus `resolveMaterialOverride` (Hash-Lookup). Alle fünf Backends (GL, Vulkan, D3D12, Metal, D3D11) rufen dieselbe `resolveShaders`.
- **Code-Auszug**:
```cpp
uint64_t matHash = 0; std::string matFrag, matVertBody;
if (p.m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId, matHash, matFrag, matVertBody))
{ ...
    const MaterialShaderVariant* matPre = HE::MaterialShaderLibrary::precompiledFor(
        m_contentManager->getMaterial(dc.materialAssetId), HE::RendererBackend::D3D11);
// MaterialShaderLibrary.cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);
```
- **Lösung**: (1) Pro Frame (oder bis `InvalidateMaterial`) ein kleiner Cache `materialAssetId -> {hash, MatShaders*, matPre, const MaterialAsset*}` im Renderer; die String-Kopie und der Hash laufen nur beim ersten Auftreten bzw. nach Invalidierung. (2) Alternativ `resolveShaders` zerlegen: `bool resolveShaderKey(cm, id, uint64_t& hash)` mit gecachtem Hash am `MaterialAsset` (Generationszähler), und Strings nur bei Cache-Miss holen (`const std::string&` statt Kopie). (3) `precompiledFor` und `frag/vertBody` nur im Miss-Pfad von `GetOrBuildMaterialShaders` auswerten. Skizze:
```cpp
struct MatDrawKey { uint64_t hash; D3D11RendererImpl::MatShaders* sh; const MaterialAsset* ma; };
auto it = p.matKeyCache.find(dc.materialAssetId);          // geleert in processPendingInvalidations()
if (it == end) it = p.matKeyCache.emplace(id, buildKey(...)).first;   // einmalig resolveShaders + GetOrBuild
```
- **Erwarteter Effekt** (Schätzung): Bei einigen hundert Graph-Material-Draws pro Frame fallen pro Draw eine String-Allokation/-Kopie von mehreren KB und ein Hash über denselben Text weg; geschätzt 5-20 µs CPU pro Draw, bei 500 Draws also 2-10 ms CPU pro Frame. Zu messen.
- **Risiko / Verifikation**: Cache muss bei `InvalidateMaterial`, Material-Hot-Reload und `Shutdown` geleert werden (`processPendingInvalidations`, Z.5271ff.). `tests/test_material_graph.cpp` (WARP-Fälle) deckt den D3D11-Pfad ab; zusätzlich ein Test, dass nach `InvalidateMaterial` mit geändertem GLSL ein neues Shaderpaar entsteht. Dieselbe Änderung in den vier anderen Backends (Parallelstellen unten in Cross-Modul).

---

### REN-102 – Pro Draw werden Textur-Cache-Schlüssel als `std::string` gebaut und Pfade kopiert
- **ID**: REN-102
- **Kategorie**: C, D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7069-7111`, `4947-4961`, `1453-1456`
- **Problem**: Im Graph-Material-Zweig wird pro Draw ein `std::string gPaths[4]` angelegt und aus `graphTexturePaths` kopiert (Heap bei Pfaden > 15 Zeichen). Danach baut jeder `resolveGraphTexture`-Aufruf den Cache-Schlüssel neu: `graphTexKey()` macht zwei `std::to_string` plus Konkatenation, bei Array-Slots kommt `key += "#arr"` dazu. Das geschieht für bis zu 4 Textur-Slots und zusätzlich für die Weightmap (bei Nicht-Landscape-Draws jedes Mal für `kDefaultLayer0WeightTextureId`). Ergebnis ist pro Draw ein Dutzend kleiner Heap-Operationen für ein Ergebnis, das sich zwischen zwei Frames nicht ändert.
- **Code-Auszug**:
```cpp
static std::string graphTexKey(const HE::UUID& id, const std::string& path)
{ return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path; }
...
if (!heWeights)
    heWeights = p.resolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {}, m_contentManager);
```
- **Lösung**: (1) UUID-keyed Lookups direkt über `unordered_map<HE::UUID, ...>` statt String-Key (Pfad-Keys in eine zweite Map `unordered_map<std::string,...>` nur für lose Editor-Assets); `#arr` als zweiter UUID-Map-Eintrag oder Schlüssel `{UUID, bool}`. (2) Das aufgelöste Slot-Array (`heTexP[4]`, `heWeights`, `heTex0`) pro Material (nicht pro Draw) in dem Cache aus REN-101 halten und bei `InvalidateTexture/Material/Mesh` verwerfen. (3) Die Default-Weightmap einmal als Member (`ID3D11ShaderResourceView* defaultWeights`) auflösen.
- **Erwarteter Effekt** (Schätzung): Pro Draw 8-15 Heap-Allokationen und mehrere Hash-Berechnungen über Strings weniger; zusammen mit REN-101 der größte CPU-Hebel des Graph-Material-Pfads.
- **Risiko / Verifikation**: Das Verhalten "Null wird gecacht (kein Retry pro Frame)" und "Pfad-Schlüssel wird nicht hot-reloaded" (Kommentar Z.1443-1447) muss erhalten bleiben. `resolveTextureRef` kann synchron laden und Pointer verschieben (Kommentar Z.4940-4942): gecachte `MaterialAsset*` nicht über einen Resolve hinweg halten. Test: `tests/test_material_graph.cpp` (Texture Sample / Texture Array Sample).

---

### REN-103 – Jeder Graph-Material-Draw bindet ~25 Ressourcen und stellt den Built-in-Zustand komplett wieder her; HeParams wird pro Instanz neu hochgeladen
- **ID**: REN-103
- **Kategorie**: B, D (Render-State-Wechsel, Doppel-Uploads)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:7119-7248` (Bind + Restore), `7163-7199` (`drawMatInstance`), `D3D11MaterialBindings.h:67-133, 204-222`
- **Problem**: Pro Draw: VS/PS/IL, 2 CB-Binds, Tex0, 4x TexP, 2 Sampler-Calls, `BindLandscapeWeights` (2), `BindDDGIAtlases` (4), `BindSkyEnvAndAO` (3), VB/IB. Direkt danach Restore: `RestoreAfterMaterialDraw` (2), `RestoreBuiltinSkyEnvAOSlots` (2), `RestoreBuiltinGISlots` (4), VS/PS/IL, 2 CBs, t2, `PSSetShaderResources(4,4)`, Sampler s2. Das sind über 45 Context-Calls pro Draw, auch wenn der nächste Draw dasselbe Material nutzt und der Restore sofort wieder überschrieben wird. Die Bindings, die nur von Frame-Zustand abhängen (DDGI-Atlanten, Sky-Cube, AO, Cluster-Listen, Sampler s0/s1/s3/s15), ändern sich zwischen Draws überhaupt nicht. Außerdem steht der HeParams-Upload (`float padded[64]`, `Map/Unmap`, memcpy) im pro-Instanz-Lambda, obwohl `params` für alle Instanzen eines DrawCalls identisch ist; bei `instanceTransforms` mit N Einträgen werden U **und** HeParams N-mal gemappt.
- **Code-Auszug**:
```cpp
auto drawMatInstance = [&](const glm::mat4& model) {
    ... Map(p.m_matObjCB ...)
    float padded[64] = { 0.0f };
    if (params) std::memcpy(padded, params->data(), ...);
    ... Map(p.m_matParamCB ...)   // identisch für jede Instanz
```
- **Lösung**: (1) HeParams-Upload vor `if (!dc.instanceTransforms.empty())` einmal pro DrawCall ausführen (Verhalten identisch, Risiko minimal). (2) Frame-konstante Bindings (Atlanten, Sky-Cube, AO, Sampler s0/s1/s3/s15) einmal beim Eintritt in eine Folge von Graph-Material-Draws binden und erst beim Wechsel zurück zu einem Built-in-Draw bzw. am Pass-Ende wiederherstellen (kleine Zustandsmaschine `enum { Builtin, Material }` in `drawDC`). (3) Optional: Sortierung des opaken Passes nach Material-Hash (falls `RenderSorter` das nicht schon tut) und Binds nur bei Wechsel von `sh`, `heTex0`, `heTexP`.
- **Erwarteter Effekt** (Schätzung): D3D11 ist treiber-CPU-lastig; 30-40 eingesparte Calls pro Draw dürften bei 500-1000 Material-Draws 1-3 ms/Frame CPU bringen. HeParams-Hoisting spart bei instanzierten Draws 1 Map/Unmap pro Instanz.
- **Risiko / Verifikation**: Die Reihenfolge-Fallen sind im Code dokumentiert (t16/SSR, s0 Albedo-Sampler, DSV/SRV-Konflikte); das WARP-Test-Setup in `tests/test_material_graph.cpp` (verwendet `D3D11MaterialBindings.h`) sollte um "zwei Material-Draws hintereinander, danach Built-in-Draw" erweitert werden, damit ein vergessener Restore auffällt. Mit PIX/RenderDoc gegenprüfen, dass die Bildausgabe bit-gleich bleibt.

---

### REN-104 – Cluster-Licht-Build und -Upload laufen zweimal pro Frame; der Kommentar behauptet "einmal"
- **ID**: REN-104
- **Kategorie**: D (Doppelarbeit), C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6317-6368` (`uploadClusters`), `6373-6389` (`fillPerFrame`), `6439` und `6869` (zwei Aufrufe)
- **Problem**: `uploadClusters` ruft `HE::BuildClusterLights` (CPU-Scatter über alle Lichter/Zellen) und lädt danach 3 strukturierte plus (bei `matClustered`) 3 rohe Buffer per `Map(DISCARD)` hoch. `fillPerFrame` wird einmal vor den Pässen (Z.6439) und einmal nach GI/SSAO (Z.6869) aufgerufen und ruft jedes Mal `uploadClusters`. Der Kommentar darüber sagt "Built once per frame"; tatsächlich läuft alles doppelt, obwohl sich nur die GI-Maskenkanal-Lane (`giActive && giLocalMaskSRV`) unterscheiden kann. Zusätzlich wird `BuildDirectionalLightWindow` und `FillMaterialLightWindow` (fillMatLight, Z.6569 und 6871) doppelt ausgeführt.
- **Code-Auszug**:
```cpp
// "Built once per frame — the light set and camera do not change between the two fillPerFrame calls below"
auto uploadClusters = [&](bool giActive) {
    frameClusters = HE::BuildClusterLights(p.m_renderWorld, localShadows, giActive && p.giLocalMaskSRV);
```
- **Lösung**: Den ersten `fillPerFrame/fillMatLight`-Aufruf (Z.6439/6569) nur ausführen, wenn zwischen ihm und Z.6869 etwas den Per-Frame-CB liest (laut Code liest keiner der Prepässe `perFrameCB`/`m_matLightCB`: Schatten, SSAO, GI-G-Buffer, SSR haben eigene CBs; vor dem Verlassen von `if (!kBackbufferTarget)` wird nicht gezeichnet). Falls doch nötig: `BuildClusterLights` nur beim zweiten Mal neu ausführen, wenn sich `giActive` geändert hat (`giActive` ist im ersten Aufruf `false`), sonst Ergebnis wiederverwenden.
- **Erwarteter Effekt** (Schätzung): Halbiert die CPU-Kosten des Cluster-Builds (je nach Lichtanzahl 0.05-0.5 ms) und spart 6 Map/Unmap + CopyBuffer-Bandbreite; bei vielen Lichtern spürbar.
- **Risiko / Verifikation**: Prüfen, dass der Entfall des ersten Fills kein Frame-1-Artefakt (z. B. Sky-Pass, `drawDebugLines`) erzeugt: `drawSky` liest den PerFrame-CB nicht. Test: `tests/test_culling.cpp` (Cross-Copy-Drift), Bildvergleich mit vielen Point-Lights, `HE_FORWARD_CLUSTER=0/1`.

---

### REN-105 – GI-Instanzbuffer wird jedes Frame als IMMUTABLE-Buffer neu erzeugt; `glm::inverse` für jedes Objekt jedes Frame
- **ID**: REN-105
- **Kategorie**: C, D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3152-3232` (`updateGiAccel`), konkret `3183-3188` und `3199-3230`
- **Problem**: Bei aktivem GI wird pro Frame über **alle** Casting-Objekte iteriert, je Objekt `glm::inverse(obj.transform)` (4x4-Inversion) berechnet und danach `makeSB` aufgerufen. `makeSB` setzt `buf.Reset()`, legt einen neuen `D3D11_USAGE_IMMUTABLE`-Buffer plus SRV an (`CreateBuffer`, `CreateShaderResourceView`) und lässt den alten vom Treiber deferred freigeben. Der Kommentar "counts are small → IMMUTABLE re-create is fine" ist für Szenen mit tausenden Casters nicht belegt; Ressourcen-Erzeugung pro Frame ist Treiber-Allokation und kann Hitches verursachen.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
...
makeSB(giInstancesCpu.data(), static_cast<UINT>(giInstancesCpu.size()),
       sizeof(GIInstanceGpu), giInstanceSB, giInstanceSRV);   // CreateBuffer + CreateSRV pro Frame
```
- **Lösung**: (1) Instanzbuffer als `D3D11_USAGE_DYNAMIC` structured buffer mit Kapazität (Wachstum in Zweierpotenzen, wie `uploadRibbon`/`debugVB`) anlegen und per `Map(WRITE_DISCARD)` füllen; SRV nur bei Kapazitätswechsel neu. (2) `invTransform` pro `entityId` cachen und nur bei geänderter `obj.transform` neu berechnen (oder Affin-Inverse über Transponierte/Skalierung statt generischem `inverse`). (3) Optional: Instanzliste nur neu aufbauen, wenn sich Transform-Hash/Objektanzahl geändert hat (Dirty-Flag aus dem Extractor).
- **Erwarteter Effekt** (Schätzung): Bei 1000 Casters entfallen pro Frame ~1000 Inversionen (~50-100 µs) und eine Buffer-/SRV-Neuanlage (Treiberabhängig 20-200 µs, mit Hitch-Potenzial).
- **Risiko / Verifikation**: Die Struktur `GIInstanceGpu` und HLSL `GiInst` bleiben unverändert. `tests/test_gi_bvh.cpp` deckt nur die BVH ab; einen WARP-/Headless-Lauf mit GI an (`HE_GI_LOG_INSTANCES`) zum Vergleich der Probe-Ergebnisse nutzen.

---

### REN-106 – Probe-Update kopiert pro Frame beide kompletten DDGI-Atlanten, obwohl nur `budget` Probes geschrieben werden
- **ID**: REN-106
- **Kategorie**: D (Bandbreite, unnötige GPU-Kopie)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3727-3734` (`dispatchGiProbeUpdate`)
- **Problem**: Für das Lesen der Vorwerte werden `giIrrTex` (RGBA16F) und `giVisTex` (RG16F) jedes Frame vollständig per `CopyResource` in die Prev-Texturen kopiert. Der Kernel schreibt aber nur `budget` (Standard 256, max 4096) aufeinanderfolgende Probes (`giProbeCursor` rotiert). Bei z. B. 20x10x20 = 4000 Probes à 8x8 Texel ist der Atlas 256x1024 Texel (2 MB + 1 MB); davon werden nur ~6 % wirklich geändert.
- **Code-Auszug**:
```cpp
ctx->CopyResource(giIrrPrevTex.Get(), giIrrTex.Get());
ctx->CopyResource(giVisPrevTex.Get(), giVisTex.Get());
```
- **Lösung**: Nur die Region der aktuellen Probe-Batch kopieren: Probes liegen zeilenweise (`giProbesPerRow = min(count, 32)`), die Batch `[cursor, cursor+budget)` belegt wenige Tile-Zeilen. `CopySubresourceRegion` mit Box der betroffenen Zeilen (bei Wrap-around zwei Boxen). Die Prev-Textur ist an allen anderen Stellen laut Kernel-Kommentar nicht gelesen ("texels outside this batch keep their values since the kernel never writes them"); Prev muss dort aber bei Leseversatz identisch zu Canon sein, daher Regionen, die der Kernel liest (nur die eigenen Tiles), genügen.
- **Erwarteter Effekt** (Schätzung): GPU-Bandbreite -90 % für diesen Schritt (einige MB pro Frame), vernachlässigbar auf großen GPUs, relevant auf iGPUs/Laptops.
- **Risiko / Verifikation**: Hängt davon ab, dass `GiProbeCS` ausschließlich Texel seiner eigenen Probe-Tiles als Vorwert liest (HLSL `kGiProbeCSHLSL` in `D3D_Shared/HlslSources.h` prüfen; ich habe sie nicht gelesen). Unsicher bis dahin; erst messen (GPU-Zeit des Schritts), dann umbauen. Test: gleiche Atlas-Bytes nach N Frames mit alter und neuer Variante (WARP, Compute 5.0).

---

### REN-107 – `DrawScene` ist eine 1466-Zeilen-Funktion mit 350-Zeilen-`drawDC`-Lambda, Flag-Variablen und tiefer Verschachtelung
- **ID**: REN-107
- **Kategorie**: A (zu lange Funktionen, Flag-Parameter, Verschachtelung)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6178-7643` (`DrawScene`), darin `6981-7337` (`drawDC`), `6597-7642` (Render-Graph-Callback)
- **Problem**: Eine Funktion erledigt Extraktion, Cull, Sort, Cluster, Shadow-Pass, GI, SSAO, SSR, Deferred, Skinned, Decals, Transparenz. Zustand wird über Lambda-Captures per Referenz und mutable Flags (`gbufferPass`, `allowInstancing`, `builtinPS`, `deferredForwardDCs`) zwischen den Phasen gereicht. Die Verschachtelung im Graph-Material-Zweig von `drawDC` erreicht 8-9 Ebenen (`if matReady` → `if resolveShaders` → `if (sh && ...)` → Lambda → `if (params)`). Rekursives Verständnis und Test einzelner Phasen sind kaum möglich; fast jede Änderung (REN-101/103) berührt dieselbe Funktion.
- **Code-Auszug**:
```cpp
bool gbufferPass = false;
ID3D11PixelShader* builtinPS = p.ps.Get();
std::vector<const DrawCall*> deferredForwardDCs;
auto drawDC = [&](const DrawCall& dc) { ... 350 Zeilen ... };
```
- **Lösung**: In Member-Funktionen des Impl (oder eine `FrameContext`-Struct mit den Frame-Konstanten: `viewProj`, `aoSRV`, `ssrSRV`, `giShadingActive`, `localShadowSrv_`) zerlegen: `renderShadowPass`, `runPrepasses`, `drawOpaque`, `drawDeferred`, `drawSkinned`, `drawTransparent`. `drawDC` in `drawBuiltinDC` und `drawMaterialDC` teilen, das Flag `gbufferPass` zu einem `enum class PassKind { Forward, GBuffer }`-Parameter machen. Rein mechanisches Extract-Function, kein Verhaltenswechsel.
- **Erwarteter Effekt**: Keine Laufzeitänderung; deutlich geringere Fehleranfälligkeit und kürzere Compile-Zeit je Änderung (Schätzung).
- **Risiko / Verifikation**: Nur Refactoring, aber Draw-Reihenfolge und State-Restore sind subtil (Kommentare Z.6824ff.); als Absicherung Bildvergleich Forward/Deferred/SSR/TAA mit `HE_DUMP_GBUFFER` und den vorhandenen WARP-Tests. Am besten gemeinsam mit REN-103 umbauen, damit nicht zweimal angefasst wird.

---

### REN-108 – `D3D11RendererImpl` ist ein 4700-Zeilen-God-Object, und 1135 Zeilen HLSL stecken im selben `.cpp`
- **ID**: REN-108
- **Kategorie**: A, F (Struktur, schwere Übersetzungseinheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:1317-6037` (Impl), `86-1220` (HLSL-Literale: `kSkyPSHLSL`, `kSceneHLSL`, `kAABlitHLSL`, `kSmaaHLSL`, `kFxaaHLSL`, `kBloom*HLSL`, `kUIHLSL`)
- **Problem**: Der Impl-Struct enthält ca. 450 Member (SSAO, SSR, GI, TAA, Decals, Deferred, Sky, UI, Preview, GPU-Timer) und deren Pipeline-/Target-/Pass-Funktionen. Jedes Teilsystem hat die Form `xxxReady/xxxFailed` + Shader + CB + Targets + `ensureXxx()/destroyXxx()`, aber ohne Kapselung. Dazu kommen ~1135 Zeilen Shader-Quelltext, die bei jeder C++-Änderung mit-kompiliert werden und beim Lesen von Logik stören. Das D3D12-Backend hat dasselbe Muster (`kFxaaHLSL12`, `kSmaaHLSL12`, `kUIHLSL12`, `D3D12Renderer.cpp:945-1200`), `D3D_Shared/HlslSources.h` hat bereits ~750 Zeilen herausgelöst.
- **Code-Auszug**:
```cpp
struct D3D11RendererImpl {
    ... // 4700 Zeilen
    bool ssrReady = false; bool ssrFailed = false; bool ssrEnabled = false; ...
    bool decalReady = false; bool decalFailed = false; ...
```
- **Lösung**: Pro Teilsystem eine eigene Struct/Datei (`D3D11Ssao.cpp`, `D3D11Ssr.cpp`, `D3D11Gi.cpp`, `D3D11Taa.cpp`, `D3D11Decals.cpp`, `D3D11Deferred.cpp`, `D3D11Ui.cpp`) mit `Init(device)`, `Resize(w,h)`, `Record(ctx, FrameInputs)`; die State-Flags (`ready/failed`) in ein kleines `LazyPipeline`-Hilfsobjekt. Embedded HLSL für Post-Passes (FXAA/SMAA/Bloom/AABlit/UI) in `D3D11Hlsl.cpp` bzw. besser nach `D3D_Shared/HlslSources.h` mit einem `#define`/Prefix für den `_dummy`-Unterschied von D3D12 (siehe Kommentar im Header) auflösen.
- **Erwarteter Effekt**: Schnellere inkrementelle Builds (nur das geänderte Teilsystem), bessere Testbarkeit; Laufzeit unverändert.
- **Risiko / Verifikation**: Größerer, rein mechanischer Umbau. Der Test `tests/test_culling.cpp` greift den Text der Szenen-HLSL ab (Kommentar Z.750); beim Verschieben des Shader-Texts muss dieser Pfad angepasst werden. Schrittweise (je Teilsystem ein PR) mit WARP-Tests.

---

### REN-109 – Dieselben Helfer-Lambdas sind im Impl 6-7x kopiert (`makeRT`, `makeCB`, `compile`)
- **ID**: REN-109
- **Kategorie**: A (Redundanz)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `makeRT`: Z.1808, 2207, 2367, 2408, 2827 (+ `colour` Z.5737, `makeTex` Z.3337); `makeCB`: Z.2776, 3273, 4307, 4563, 5206; `compile`: Z.3259, 3790, 3887, 5157, 5396, 5706; manuelle `D3D11_BUFFER_DESC`-Blöcke an Dutzenden Stellen (z. B. Z.2276, 2303, 2309, 3844-3868, 4193-4200, 5800-5805)
- **Problem**: `makeRT` (Texture2D + RTV + SRV mit `BIND_RENDER_TARGET|SHADER_RESOURCE`) existiert in fünf praktisch gleichen Varianten, `makeCB` (Dynamic-CB auf 16 B aufgerundet) fünfmal, `compile` (D3DCompile + Logging) sechsmal mit leicht verschiedenem Fehlertext. Teilweise gibt `makeRT` keinen Fehler zurück (`ok = makeRT & makeRT`, Z.2425 und Z.2844 mit `&` bzw. `&=` statt `&&`: absichtlich ohne Kurzschluss, aber inkonsistent zu Z.1823 mit `&&`). Fehler beim Erzeugen von RTV/SRV werden nur über `rtv && srv` abgefangen.
- **Code-Auszug**:
```cpp
auto makeRT = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& rtv,
                  ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
    D3D11_TEXTURE2D_DESC td{}; ... // 5x kopiert
```
- **Lösung**: Drei Member-Helfer im Impl: `bool createColorTarget(fmt, w, h, Target& t)` mit `struct Target { Tex, RTV, SRV, UAV }` plus `reset()`; `ComPtr<ID3D11Buffer> createDynamicCB(UINT bytes)`; `ComPtr<ID3DBlob> compileHlsl(src, entry, profile, tag, flags)` (einziger Ort für `D3DCOMPILE_DEBUG|SKIP_OPTIMIZATION`, Fehlertext-Kürzung wie in `makePS` Z.2757-2765). Damit entfallen ~300 Zeilen und die `Reset()`-Orgien (Z.2358-2365, 2809-2815, 3511-3524) werden zu `target.reset()`.
- **Erwarteter Effekt**: -300 Zeilen, einheitliche Fehlerbehandlung.
- **Risiko / Verifikation**: Gering; Formate/Flags je Aufrufer als Parameter durchreichen (UAV-Varianten bei GI, Depth-Typeless bei Viewport). Smoke-Run mit `HE_HIDDEN_WINDOW=1`.

---

### REN-110 – Alle ~30 Shader werden bei jedem Start zur Laufzeit mit `D3DCompile` kompiliert, teilweise dieselbe Quelle mehrfach, ohne Disk-Cache
- **ID**: REN-110
- **Kategorie**: D (wiederholtes Parsen/Kompilieren), E (blockierend beim Start)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:4145-4189` (`createPipeline`: `sceneSource` 5x für `VSMain/PSMain/VSDepth/VSDepthInstanced/VSMainInstanced`), `2253-2271` (SSAO-Pos 3x), `3285-3301` (`kGiGBufHLSL` 3x), `3805-3816`, `5171-5182`; 29 `D3DCompile(`-Aufrufe in der Datei
- **Problem**: FXC parst und optimiert jedes Mal den kompletten Quelltext. Die Szenen-Quelle (Sky-Funktion + `kSceneHLSL`, ca. 700 Zeilen mit entrollten PCF-Schleifen und DDGI-Schleife) wird allein in `createPipeline` fünfmal kompiliert, danach nochmals in `createDeferredPipelines` (Z.5171) für `GBufPS`; `sceneSource` wird dort per `std::string(kSkyFuncHLSL) + kSceneHLSL` ein zweites Mal gebaut. Ein Compile-Cache (z. B. Hash der Quelle + Flags → `ID3DBlob` auf Platte) fehlt; die Material-Shader-Pfade besitzen immerhin einen In-Memory-Cache (`m_materialShaders`) und gebackene Pak-Varianten.
- **Code-Auszug**:
```cpp
const std::string sceneSource = std::string(kSkyFuncHLSL) + kSceneHLSL;
D3DCompile(sceneSource.c_str(), ..., "VSMain", "vs_5_0", ...);
D3DCompile(sceneSource.c_str(), ..., "PSMain", "ps_5_0", ...);
D3DCompile(sceneSource.c_str(), ..., "VSDepth", ...);        // + VSDepthInstanced, + VSMainInstanced
```
- **Lösung**: (1) `sceneSource` einmal als `static const std::string` bauen und an `createDeferredPipelines` weiterreichen. (2) Kleiner Shader-Blob-Cache auf Platte (`%LOCALAPPDATA%/Horizon/shadercache/<hash>.bin`, Schlüssel = Quelle + Entry + Profil + Flags + FXC-Version) hinter `compileHlsl` (REN-109). (3) Alternativ vor dem Ablegen im Pak (`asset_compiler`) vorkompilieren, analog zu `MaterialShaderVariant`.
- **Erwarteter Effekt** (Schätzung): Start auf der Windows-Plattform um einige hundert ms bis Sekunden schneller; Shader-Hitches bei Ressourcen-Neuanlage (z. B. Shadow-Resize) bleiben unberührt.
- **Risiko / Verifikation**: Cache muss bei Änderung der Quelle/Flags invalidieren (Hash reicht). Unsicher: tatsächliche Compile-Zeit je Shader; Start einmal mit Stoppuhr um `createPipeline()` messen (`HE_LOG_INFO` mit `SDL_GetTicks`) vor jeder Arbeit.

---

### REN-111 – UI-Pass: pro Quad ein `Map(DISCARD)` + `Draw`, `RSGetState` pro Pass, kein Batching
- **ID**: REN-111
- **Kategorie**: D, B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:5901-5945` (`renderUIPass`), Header `D3D11Renderer.cpp:2026` (Kommentar zu `uiCB`)
- **Problem**: Jedes `UIRenderObject` (Panel, Glyph, Border…) füllt einen 176-Byte-`UICBData`, mappt `uiCB` mit DISCARD (Treiber benennt den Buffer um) und zeichnet 4 Vertices. Eine Textzeile mit 40 Glyphen sind 40 Map + 40 Draws; ein komplexes In-Game-HUD kommt schnell auf einige Tausend Draws pro Frame. Die Vertexposition wird im VS aus `SV_VertexID` und der CB berechnet; das schließt Batching über einen strukturierten Instanzbuffer (wie `instanceSB` für Meshes) nicht aus.
- **Code-Auszug**:
```cpp
for (const UIRenderObject& obj : m_renderWorld.uiObjects) {
    ... UICBData cb; ... 
    if (SUCCEEDED(ctx->Map(uiCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) { memcpy; Unmap; }
    ctx->Draw(4, 0);
}
```
- **Lösung**: `UICBData` je Objekt in einen strukturierten `StructuredBuffer<UICBData>` (Dynamic, Kapazität wachsend) schreiben, einmal mappen, danach Runs gleicher State (Clip-Rect, Atlas/Textur, Blend) mit `DrawInstanced(4, runLength, 0, firstInstance)` zeichnen; VS liest `uUI[iid]`. Reihenfolge bleibt erhalten (Painter-Algorithmus bleibt dank Run-Bildung nur bei identischem State gebündelt).
- **Erwarteter Effekt** (Schätzung): Bei 1000-3000 UI-Quads Verringerung der CPU-Kosten um 50-80 % im UI-Pass; zu messen mit `FrameGpuStats::drawCalls`.
- **Risiko / Verifikation**: HLSL `kUIHLSL` (Z.1116-1220) und `UICBData` ändern; Parallelstellen: `kUIHLSL12` in `D3D12Renderer.cpp:945`, GL `kUIVS/kUIFS`. Test: `tests/test_ui_widgets.cpp`-Pfad (Render-Vergleich) und Screenshot-Vergleich der Widget-Beispiele.

---

### REN-112 – TAA-Velocity-Pass: pro Objekt `Map/Unmap` und zwei `unordered_map<entityId, mat4>` pro Frame neu gefüllt
- **ID**: REN-112
- **Kategorie**: D, C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:1847-1912` (`renderTaaVelocity`), Member `1782`
- **Problem**: Pro sichtbarem Objekt: ein Hash-Lookup in `taaPrevTransforms`, ein Insert in `taaCurTransforms` (Knoten-Allokation je Eintrag, `clear()` gibt Knoten wieder frei und `swap` tauscht die Maps), danach 3 Matrixmultiplikationen, `Map/Unmap` des `taaVelocityCB` und ein eigener `DrawIndexed`. Instancing wie bei Schatten/SSAO fehlt (der Pass nimmt `m_sortedIndices` und zeichnet Objekt für Objekt, auch gleiche Meshes).
- **Code-Auszug**:
```cpp
const auto it = taaPrevTransforms.find(obj.entityId);
const glm::mat4 prevModel = (it != taaPrevTransforms.end()) ? it->second : obj.transform;
taaCurTransforms[obj.entityId] = obj.transform;
...
ctx->Map(taaVelocityCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m) ... DrawIndexed(...)
```
- **Lösung**: (1) `unordered_map` durch `std::vector<glm::mat4>` indiziert nach dem stabilen Objektindex bzw. durch eine flache Map (z. B. sortiertes Vektorpaar `{entityId, mat4}` mit Merge-Join gegen die letzte Frame-Liste) ersetzen; Kapazität behalten. (2) Per Objekt nur `mvpJitter/mvpNow/mvpPrev` in einen strukturierten Instanzbuffer schreiben und je Mesh-Run per `DrawIndexedInstanced` zeichnen (analog `drawDepthInstanced`). (3) Alternativ Velocity im G-Buffer/Szenenpass als zweites Render-Target mitschreiben.
- **Erwarteter Effekt** (Schätzung): Nur bei TAA relevant; bei 2000 Objekten ca. 0.2-0.5 ms CPU/Frame.
- **Risiko / Verifikation**: Reihenfolge `vpJit * transform` beibehalten (Kommentar Z.1843-1846, LESS_EQUAL-Test hängt davon ab). Hängt mit REN-103/107 zusammen. Test: Bildvergleich mit TAA, bewegte und statische Objekte (kein Ghosting).

---

### REN-113 – Schatten: alle Kaskaden und bis zu 16 lokale Layer werden jedes Frame neu geculled, sortiert und gerendert
- **ID**: REN-113
- **Kategorie**: D (fehlende Dirty-Flags)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6645-6698` (`renderDepthLayer`-Lambda und beide Schleifen)
- **Problem**: Pro Layer: `m_culler.cull(...)`, `m_sorter.sort(...)` über alle Objekte, `batchDepthCasters`, Viewport/DSV-Wechsel, Clear. Mit 3 Kaskaden und 16 lokalen Layern (Punktlicht = 6 Faces) sind das bis zu 19 vollständige Cull+Sort+Render-Läufe pro Frame, auch wenn Lichter und Caster in diesem Bereich statisch sind. Ein Cache für statische lokale Lichter (Layer nur neu rendern, wenn Licht oder ein Caster im Light-Frustum seine Transform geändert hat) fehlt; der Sort dient laut Kommentar nur dem Gruppieren nach Mesh.
- **Code-Auszug**:
```cpp
p.m_culler.cull(p.m_renderWorld, viewProj, p.shadowVisible);
p.m_sorter.sort(p.m_renderWorld, p.shadowVisible, p.shadowSorted);
RenderSorter::batchDepthCasters(p.m_renderWorld, p.shadowSorted, skipEntity, p.shadowBatches);
```
- **Lösung**: (1) Für lokale Layer einen Signatur-Hash pro Layer (Light-VP, Owner-Entity, Hash der Caster-Transforms im Frustum, Mesh-Invalidierungsgeneration) halten; bei unveränderter Signatur Layer überspringen (Atlas-Inhalt bleibt, `ClearDepthStencilView` und Render entfallen). (2) Für die Sortierung ein direktes Gruppieren nach Mesh-UUID ohne Tiefensortierung (Depth-only braucht keine Tiefenordnung) statt `m_sorter.sort`.
- **Erwarteter Effekt** (Schätzung): Bei Szenen mit vielen statischen Punktlichtern GPU- und CPU-Zeit des Shadow-Passes -50 % bis -90 %; sonst keine Wirkung.
- **Risiko / Verifikation**: Hoch genug, dass Messdaten nötig sind: Caster-Transform-Änderungen, Skinned-Caster (werden in dieser Schleife nicht behandelt), Mesh-Hot-Reload und `shadowSize`-Resize müssen die Signatur ändern. Ohne vorherige Messung ("wie viele lokale Layer sind real aktiv?") nicht umsetzen.

---

### REN-114 – Per-Frame-Heap-Allokationen in Render-Pfaden (Vektoren, Maps, Kernel, Bones, Debug-Lines)
- **ID**: REN-114
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6720` (`opaqueDCs_/transparentDCs_`), `6913-6915` (`ribbonDCs`, `ribbonMeshes`, `ribbonMeshByDC`), `6922` (`rebased`), `6980` (`deferredForwardDCs`), `7544` (`boneScratch(128)`), `2624` (`BuildSSAOKernel` pro Frame), `5607-5612` (`drawDebugLines` baut `verts` neu), `2478/3532` (`std::function`-Parameter), `8381`
- **Problem**: Alle genannten Container werden pro Frame neu angelegt und wieder freigegeben. `runSSAO` ruft pro Frame `HE::BuildSSAOKernel(kSsaoKernelSize)` (RNG mit festem Seed `0x9E3779B9u`, ergibt jedes Mal das identische Ergebnis, plus `vector`-Allokation und `normalize` je Sample) und befüllt daraus `cb.kernel[]`. Die `std::function`-Parameter `resolveMeshFn` werden pro Draw (virtueller Aufruf, ggf. Heap bei großen Captures) benutzt, obwohl es immer derselbe Lambda `p.resolveMesh` ist. Skinned-Pass füllt 128 Matrizen (8 KB) pro Draw mit Identität und kopiert immer 8 KB in den CB, auch bei 20 Bones.
- **Code-Auszug**:
```cpp
std::vector<glm::vec3> kernel = HE::BuildSSAOKernel(HE::kSsaoKernelSize);   // Z.2624, jedes Frame
std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));              // Z.7544
const std::function<const GpuMesh*(HE::UUID)>& resolveMeshFn                 // Z.2478
```
- **Lösung**: Container als Member des Impl (`clear()` statt neu anlegen, `reserve` behalten); SSAO-Kernel einmal beim Anlegen von `ssaoCB` in ein `static const std::array<glm::vec4,32>` bzw. direkt in den CB-Zwischenspeicher; `resolveMeshFn` als Template-Parameter oder als `GpuMesh* (D3D11RendererImpl::*)(...)`; Bone-Upload auf `n * sizeof(mat4)` begrenzen und nur den Rest bei Bedarf mit Identität füllen (der Shader indiziert nur gültige Bones).
- **Erwarteter Effekt** (Schätzung): Einige µs bis ~0.1 ms pro Frame, vor allem weniger Allokator-Druck; kein Muss.
- **Risiko / Verifikation**: Kleinteilig, bit-identisch. Parallelstellen für `BuildSSAOKernel` pro Frame: D3D12Renderer.cpp:5792, OpenGLRenderer.cpp:5784, VulkanRenderer.cpp:12172, MetalRenderer.mm:12550 (Cross-Modul-Hinweis).

---

### REN-115 – Mesh-Lookups über `unordered_map<UUID>`: pro Objekt dreimal pro Frame
- **ID**: REN-115
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6245-6249` (`refineWorldBounds`-Schleife über alle Objekte), `6981-6991` (`drawDC`), `6659` (Shadow-Batches), `1879` (Velocity), `3180` (GI)
- **Problem**: `resolveMesh()` macht pro Aufruf einen `meshCache.find()`. Pro Objekt geschieht das in der Bounds-Schleife, im Shadow-Pass (pro Batch), im SSAO-/GI-Prepass (pro DrawCall), im Hauptpass und im Velocity-Pass. Bei 5000 Objekten sind das einige 10 000 Hash-Lookups pro Frame; die UUID-Hash-Funktion ist nicht eingesehen (`HE::UUID` in `Types/UUID.h`, außerhalb des Slices).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : p.m_renderWorld.objects)
    if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager))
        obj.refineWorldBounds(mesh->localBounds);
```
- **Lösung**: Die in der Bounds-Schleife aufgelöste `GpuMesh*` in einem per-Frame-Vektor `std::vector<const GpuMesh*> objMesh` (Index = Objektindex) ablegen und in den späteren Pässen nutzen; `meshCache` ist `unordered_map` (stabile Knoten), die Zeiger bleiben bis `processPendingInvalidations` (Frame-Anfang) gültig.
- **Erwarteter Effekt** (Schätzung): -1 bis -3 Lookups pro Objekt und Frame (~20-50 ns je), also ~0.1-0.5 ms bei 5000 Objekten; nur mit Messung angehen.
- **Risiko / Verifikation**: `resolveMesh` kann beim ersten Aufruf Buffer erzeugen (Upload auf Render-Thread). Pointer-Stabilität prüfen (keine `meshCache`-Rehash-Invalidierung bei `emplace`: für `unordered_map` bleiben Elementzeiger gültig).

---

### REN-116 – `updateSkyEnvCube` allokiert bei jedem Re-Bake 2.3 MB und packt die Halbfloats single-threaded
- **ID**: REN-116
- **Kategorie**: C, B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:4615-4637`
- **Problem**: Bei jeder Sonnenbewegung (Tag-Nacht-Automatik, Totband 2e-3) legt die Funktion `std::vector<float> px(128*128*6*4)` (1.5 MB) und `std::vector<uint16_t> half(...)` (0.8 MB) neu an. Der Bake selbst läuft über `parallel_for`, die anschließende Konvertierung `packHalf1x16` über 393 216 Werte läuft seriell auf dem Render-Thread. Der Bake blockiert den Render-Thread bis zum Ende (`parallel_for` wartet).
- **Code-Auszug**:
```cpp
std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
parallel_for(...BuildSkyEnvFaceRow...);
std::vector<uint16_t> half(px.size());
for (size_t i = 0; i < px.size(); ++i) half[i] = glm::packHalf1x16(px[i]);
```
- **Lösung**: `px`/`half` als wiederverwendete Member; Halbfloat-Packen in den `parallel_for`-Körper pro (face,row) verlegen (jede Zeile schreibt direkt `uint16_t`-Output). Dasselbe gilt für die anderen Backends (Parallelstellen unten).
- **Erwarteter Effekt** (Schätzung): ~0.3-1 ms weniger bei jedem Re-Bake, kein Allokator-Hitch.
- **Risiko / Verifikation**: Ergebnis-Bytes identisch; der WARP-Orientierungstest in `tests/test_material_graph.cpp` ruft `UploadSkyEnvCube` über dieselbe Funktion und deckt den Upload ab.

---

### REN-117 – `Shutdown()` listet ~120 `Reset()`-Aufrufe von Hand, obwohl `~D3D11Renderer` das Impl löscht
- **ID**: REN-117
- **Kategorie**: A, F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6096-6176`, `6039-6040`
- **Problem**: Alle Member sind `ComPtr`/Container; der Destruktor `delete m_impl` räumt ohnehin alles ab. `Shutdown()` zählt trotzdem einen Teil der Member manuell auf (SSR, TAA, GI, Sky, Decals ...), aber nicht alle (z. B. `vs`, `ps`, `hdrTex`, `bloomTex[]`, `gbTex[]`, `shadowTex`, `localShadowTex`, `giGBuf*`, `worldPreview[]`, `uiFontAtlases` ist drin, `m_debugLines` nicht). Neue Member müssen an zwei Stellen gepflegt werden; vergessene bleiben nach `Shutdown()` am Leben und werden bei erneutem `Initialize()` still überschrieben.
- **Code-Auszug**:
```cpp
void D3D11Renderer::Shutdown() {
    m_impl->meshCache.clear(); ... m_impl->device.Reset();
}
```
- **Lösung**: `Shutdown()` auf `delete m_impl; m_impl = new D3D11RendererImpl{};` (nach `m_impl->context->ClearState(); Flush();`) reduzieren. Die Reihenfolge der Freigabe ist bei D3D11 (Refcounting) unkritisch; `swapchain` vor `device` ist über Member-Reihenfolge gegeben.
- **Erwarteter Effekt**: -80 Zeilen, keine vergessenen Ressourcen.
- **Risiko / Verifikation**: Prüfen, dass kein Aufrufer nach `Shutdown()` auf Impl-Zustand zugreift (`GetCapabilities`, `GetDevice`). Headless-Start/Stop-Test.

---

### REN-118 – `D3D11ShaderManager` wird vom Renderer nicht benutzt (nur Test); doppelte Compile-Logik, schwerer Header, lineare Scans
- **ID**: REN-118
- **Kategorie**: F (toter Code im Produktpfad), A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11ShaderManager.h:29-31` (Eingeständnis im Kommentar), `D3D11ShaderManager.h:1-14`, `D3D11ShaderManager.cpp:25-47, 56-59, 125-126, 150-156`; Nutzer: nur `tests/test_d3d_shader_manager.cpp`, `HE_Rendering/CMakeLists.txt`
- **Problem**: Der Renderer hält Shader selbst in ComPtr-Membern (Header-Kommentar). Die Klasse ist damit ein Test-/Zukunftsobjekt mit einer weiteren D3DCompile-Pipeline (anders als die sechs Compile-Lambdas im Renderer: ohne Debug-Flags, ohne Fehlertext-Kürzung). Weitere Kleinigkeiten: `load()` macht `fs::exists` und anschließend `ifstream` (doppelter Syscall, TOCTOU); Quelle wird über `stringstream` kopiert; `m_loadedShaders`/`m_loadedPrograms` werden per linearer Suche gepflegt (`release`, `compile`: `for (auto& h : m_loadedShaders)`), obwohl die IDs aufsteigend sind; der Header zieht `windows.h`, `d3d11.h` und `wrl/client.h` in jeden Includer.
- **Code-Auszug**:
```cpp
// Note: D3D11Renderer does not route its shaders through this class — it holds them itself
for (auto& h : m_loadedShaders) if (h.id == handle.id) h.ready = true;
```
- **Lösung**: Entweder den Renderer über diese Klasse leiten (und `compileHlsl` aus REN-109 hier hinein verlegen) oder die Klasse klar als Test-/Tool-Hilfe kennzeichnen. In jedem Fall: `fs::exists` entfernen (der `is_open()`-Check genügt), `m_loadedShaders` als `unordered_map<id, ShaderHandle>`, Header-Includes mit Forward-Deklarationen schlanker halten (`ID3D11Device` ist nur ein ComPtr-Member, dafür braucht der Header `d3d11.h`; ggf. `Impl`-Zeiger).
- **Erwarteter Effekt**: Weniger Code-Duplikation; keine Laufzeitwirkung.
- **Risiko / Verifikation**: `tests/test_d3d_shader_manager.cpp` bleibt grün lassen (ComPtr-Refcount-Test nach `cleanup()`).

---

### REN-119 – Built-in-Draw bindet pro Draw Konstantenpuffer und Textur erneut (`uploadObject`), ohne Änderungsprüfung
- **ID**: REN-119
- **Kategorie**: B (redundante State-Wechsel)
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:6577-6594` (`uploadObject`), `7255-7262, 7329-7336`, `7583-7584`
- **Problem**: `uploadObject` mappt den PerObject-CB und ruft danach jedes Mal `VSSetConstantBuffers(0,...)` und `PSSetConstantBuffers(0,...)` mit demselben Buffer auf; der Buffer ändert sich zwischen den Draws nicht (nur sein Inhalt). Zusätzlich `PSSetShaderResources(0, ...)` und `IASetVertexBuffers` pro Draw auch bei gleichem Mesh/Textur wie beim Vorgänger. Der Hauptpass sortiert zudem nach Tiefe, sodass Folgedraws oft verschiedene Meshes haben; Textur und Mesh-VB sind trotzdem häufig identisch (Instanced-Runs sind schon abgedeckt).
- **Code-Auszug**:
```cpp
ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());   // pro Draw, derselbe Buffer
```
- **Lösung**: Die CB-Binds aus `uploadObject` entfernen und einmal nach den Material-Draws bzw. am Pass-Anfang setzen (sie werden nach jedem Graph-Material-Draw ohnehin wiederhergestellt, Z.7235-7236); `lastAlbedo`/`lastVB` im `drawDC` merken und nur bei Änderung binden.
- **Erwarteter Effekt** (Schätzung): 2-4 API-Calls pro Built-in-Draw, minimal bis ~0.3 ms bei 3000 Draws.
- **Risiko / Verifikation**: Schattenpass nutzt `uploadObject` ebenfalls (Z.6683), dort ist `VSSetConstantBuffers(0)` durch `VSSetShader(depthVS)` nicht gesichert; beim Entfernen sicherstellen, dass b0 vor der ersten Nutzung gebunden ist (`DrawScene`-Setup Z.6832-6835 tut das für den Hauptpass, nicht für den Schattenpass).

---

### REN-120 – Bloom: 10 Blur-Passes (je 9 Taps) mit 11 CB-Maps pro Frame
- **ID**: REN-120
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `D3D11Renderer.cpp:3934-3977` (`runBloom`), `3925-3930` (`updatePostFxCB`), `1090-1105` (`kBloomBlurHLSL`)
- **Problem**: Halbauflösende Ping-Pong-Kette mit 5 H- + 5 V-Passes mit 9 Taps, d. h. 90 Taps pro Pixel bei Halbauflösung, plus pro Pass `Map/Unmap` auf dem 16-Byte-`postFxCB`. Die Parameter wechseln nur zwischen zwei Werten (`horiz` an/aus).
- **Code-Auszug**:
```cpp
for (int p = 0; p < 10; ++p) { ... const float cb[4] = { tw, th, horiz ? 1.0f : 0.0f, 0.0f };
    updatePostFxCB(cb); ... ctx->Draw(3, 0); ... }
```
- **Lösung**: Zwei konstante CBs (H/V) einmal pro Frame (oder statisch beim Resize) anlegen und pro Pass nur umbinden; optional Downsample-Kette (Mip-Pyramide, Dual-Kawase) statt 10 Full-Taps. Letzteres ändert das Bild und ist ausdrücklich keine verhaltensgleiche Änderung.
- **Erwarteter Effekt** (Schätzung): CB-Teil gering; die Algorithmusänderung könnte die Bloom-GPU-Zeit stark senken, braucht aber Bildfreigabe.
- **Risiko / Verifikation**: CB-Teil bit-identisch. Algorithmusänderung nur nach GPU-Messung (`FrameGpuStats` ist whole-frame; PIX nötig).

---

### REN-121 – Kommentar-/Doku-Abweichungen und Log-Rauschen
- **ID**: REN-121
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D11Renderer.cpp:6320-6322` ("Built once per frame", siehe REN-104), `2026` (`uiCB; // 64 bytes` vs. tatsächlich 176, Z.5801), `1269-1270` (`PerObjectCB::pbr` Kommentar nennt nicht `w`), `67` und `D3D11Renderer.h:51` (`[blind] added D3D11 sky+IBL+debuglines parity`), `4693-4698` (gesamte Material-HLSL als `HE_LOG_INFO`), `1396` ("shader hash" Kommentar korrekt), `D3D11Renderer.cpp:4595-4604`
- **Problem**: Einige Kommentare beschreiben veralteten oder falschen Stand (CB-Größe, einmal pro Frame). `[blind]` ist ein Entstehungs-Tag ohne Aussage. `GetOrBuildMaterialShaders` schreibt beim ersten Material den kompletten VS- und PS-Quelltext (mehrere 10 KB) als Info-Log, was Logdateien und die Editor-Konsole aufbläht.
- **Code-Auszug**:
```cpp
ComPtr<ID3D11Buffer> uiCB;       // 64 bytes: rect(16)+color(16)+uvRect(16)+viewport(8)+mode(4)+pad(4)
...
HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material VS HLSL:\n") + vsSrc).c_str());
```
- **Lösung**: Kommentare korrigieren, `[blind]`-Marker entfernen, den HLSL-Dump hinter eine Umgebungsvariable (`HE_DUMP_MATERIAL_HLSL=1`) oder `HE_LOG_DEBUG` legen.
- **Erwarteter Effekt**: Klarere Codebasis.
- **Risiko / Verifikation**: Trivial.

---

### REN-122 – `D3D12MaterialRootSignature.h`: Test-only-Parameter in der Produktfunktion, Registertabelle dreifach von Hand synchron gehalten
- **ID**: REN-122
- **Kategorie**: A (Flag-/Default-Parameter), F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `D3D12MaterialRootSignature.h:93-104, 116-122, 141-155, 185-203, 238-248, 258-293`; Gegenstück `D3D11MaterialBindings.h:45-49, 85-91, 145-147, 195-197`
- **Problem**: `DescribeMaterialRootSignature(out, rangeCount, samplerCount, paramCount)` hat drei Defaultparameter, die ausschließlich für Test-Negativkontrollen existieren (`kLegacyRangeCount`, `kLegacySamplerCount`, `kPreClusterParamCount`), aber in der Produktdeklaration stehen. Die Zuordnung "GLSL-Binding → HLSL-Register" steht an drei Stellen von Hand: `kHlslMaterialPins` in `MaterialShaderLibrary.cpp` (Slice 10), die Slot-Konstanten in `D3D11MaterialBindings.h` und `D3D12MaterialRootSignature.h` (`kSlot*`, `kSamplers[]`); beide Header sagen im Kommentar "Must match kHlslMaterialPins". `DescribeResolveRootSignature` baut zunächst die komplette Material-Signatur und kopiert sie anschließend. Zudem sind Sampler-Registerlisten als Literale (`kSamplers[]`, `kResolvePointSamplers`) statt aus den Pins abgeleitet.
- **Code-Auszug**:
```cpp
inline void DescribeMaterialRootSignature(MaterialRootSignature& out,
    UINT rangeCount = kRangeCount, UINT samplerCount = kSamplerCount, UINT paramCount = kParamCount)
```
- **Lösung**: Test-Varianten in einen kleinen `DescribeLegacyMaterialRootSignature`-Wrapper im Testcode (oder `#ifdef HE_TESTS`); Register-Konstanten aus einer einzigen `constexpr`-Tabelle in `MaterialShaderLibrary.h` ableiten, `static_assert`s zwischen D3D11/D3D12-Slots und den Pins (z. B. `static_assert(kWeightmapSrvSlot == MaterialShaderLibrary::kPinLandscapeWeights.srv)`), damit der "Must match"-Kommentar vom Compiler geprüft wird.
- **Erwarteter Effekt**: Verhindert Drift (das dokumentierte Fehlerbild "Thema 56/57").
- **Risiko / Verifikation**: Die Pins-Tabelle liegt außerhalb dieses Slices (`MaterialShaderLibrary.cpp`, nicht gelesen); Umsetzung dort abstimmen. Bestehende WARP-PSO-Tests in `tests/test_material_graph.cpp` als Absicherung.

---

### REN-123 – Fullscreen-Pass-Setup und "Restore scene state"-Blöcke sind mehr als ein Dutzend Mal von Hand wiederholt
- **ID**: REN-123
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: z. B. `D3D11Renderer.cpp:2606-2612, 2894-2901, 3661-3667, 3939-3946, 5577-5597, 5631-5649, 7741-7752, 7790-7801, 7818-7825, 8417-8427`; Save/Restore per `OMGetRenderTargets` an 9 Stellen
- **Problem**: Acht Stellen setzen Fullscreen-VS (`fsVS`), `noDepthDSS`, `fsRastState`, `IASetInputLayout(nullptr)`, Topologie von Hand; Passes enden mit individuell zusammengestellten "restore scene state"-Listen (`drawSky` Z.5595-5597, `drawDebugLines` Z.5640-5649, `RenderWorldPreview` Z.8454-8466), die jeweils unterschiedlich vollständig sind (z. B. stellt `drawSky` Sampler 0 wieder her, setzt aber PS-Shader nicht zurück). Vergessene Restores sind der in den Kommentaren mehrfach beschriebene Fehlertyp (stilles Unbinding, falsche Sampler).
- **Code-Auszug**:
```cpp
ctx->IASetInputLayout(nullptr);
ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
ctx->VSSetShader(fsVS.Get(), nullptr, 0);
ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
ctx->RSSetState(fsRastState.Get());
```
- **Lösung**: Hilfsfunktionen `bindFullscreenState(ctx)` und `restoreSceneState(ctx)` (je eine Stelle, die vollständige Liste der Scene-Defaults) bzw. ein RAII-`ScenePassScope`, der beim Verlassen den definierten Basiszustand setzt. Bei Verwendung passt sich REN-103/107 an.
- **Erwarteter Effekt**: -100 Zeilen, geringeres Risiko vergessener Restores.
- **Risiko / Verifikation**: Mechanisch; Bildvergleich Forward/Deferred/Preview.

---

## Cross-Modul-Hinweise

- **`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:941-952, 1000-1009` (Slice 10)**: `resolveShaders/resolveFragment` kopieren GLSL und hashen es bei jedem Aufruf. Ursache von REN-101 in allen fünf Backends. Parallelstellen mit demselben Per-Draw-Aufruf: `D3D12Renderer.cpp:10974`, `VulkanRenderer.cpp:8254`, `OpenGLRenderer.cpp:3599`, `MetalRenderer.mm:12815`. Eine gemeinsame Lösung (Generationszähler/gecachter Hash am `MaterialAsset`, `resolveShaderKey`) wäre einmal in `MaterialShaderLibrary` + `ContentManager` zu bauen.
- **`BuildSSAOKernel` pro Frame**: `OpenGLRenderer.cpp:5784`, `VulkanRenderer.cpp:12172`, `D3D12Renderer.cpp:5792`, `MetalRenderer.mm:12550` (siehe REN-114). Besser: `HE::SsaoKernel()` liefert `const std::array<glm::vec4,32>&` (statisch, festes Seed) in `src/HE_Rendering/src/SsaoKernel.cpp` (Slice 10).
- **`BuildClusterLights` doppelt pro Frame**: Prüfen, ob die gleiche Doppel-Fill-Struktur (`fillPerFrame` vor + nach den Pässen) auch in `D3D12Renderer.cpp:10318`, `OpenGLRenderer.cpp:11416`, `VulkanRenderer.cpp:7875` auftritt.
- **Embedded HLSL-Duplikation D3D11/D3D12**: `kFxaaHLSL/kSmaaHLSL/kBloom*HLSL/kAABlitHLSL/kUIHLSL` in `D3D11Renderer.cpp:934-1220` und `kFxaaHLSL12/kSmaaHLSL12/kBloomBrightHLSL12/kBloomBlurHLSL12/kAABlitHLSL12/kUIHLSL12` in `D3D12Renderer.cpp:945-1210`; laut `D3D_Shared/HlslSources.h` nur durch `Texture2D _dummy : register(t1)` (und bei UI Glyph-UV im VS/PS) verschieden. Gemeinsame Quelle mit Präprozessor-Makro ist machbar (REN-108).
- **`UpdateSkyEnvCube` / sky env bake**: `OpenGLRenderer`, `Vulkan`, `Metal` haben dieselbe Bake-/Upload-Struktur (REN-116); gemeinsamer Helfer `HE::BakeSkyEnvHalf(...)` in `SkyEnvBake.h` könnte `px`+`half` zusammenführen.
- **`SkyNoise3D.cpp:10` (Slice 10)**: `BuildSkyNoise3D(256)` läuft synchron beim Renderer-Start für alle Backends (D3D11: `createSkyPipeline`, Z.5468); die Datei enthält laut Kommentar bereits eine Optimierung (Worley-Cache, vorher ~10.7 s). Kein `parallel_for` zu sehen; Start-Messung sinnvoll.
- **`RenderSorter::sort/batchDepthCasters` (nicht in diesem Slice)**: REN-113 ruft sie bis zu 19x pro Frame auf; lohnt Blick in `src/HE_Rendering/src/RenderSorter.cpp` (Slice 10), ob für Depth-only eine Gruppierung ohne Tiefensort reicht.

## Unsicher / Messung nötig

- REN-101/102/103: tatsächlicher CPU-Anteil pro Graph-Material-Draw (Zielwert 5-20 µs ist Schätzung). Messung: `EngineProfiler`-Scope um den Graph-Zweig, 500 Material-Draws.
- REN-106: ob der GI-Probe-Kernel ausschließlich eigene Tiles aus den Prev-Texturen liest (`kGiProbeCSHLSL` in `D3D_Shared/HlslSources.h` nicht gelesen); GPU-Kosten der Atlas-Kopie.
- REN-110: reale `D3DCompile`-Zeit der Szenen-Shader (Start-Messung).
- REN-113: durchschnittliche Zahl aktiver Schatten-Layer und Anteil statischer Lichter in typischen Szenen.
- REN-115/REN-119: Wirkung einzelner Lookups/Binds im Bereich unter 0.5 ms ist ohne Profil nicht belegbar; nur umsetzen, wenn REN-101-103 gemessen sind.
- REN-120: GPU-Zeit der Bloom-Kette (PIX/GPU-Timer nur whole-frame).
- Nicht verifiziert: Verhalten von `RenderSorter::partitionByOpacity/sort` (ob die Opaque-Liste bereits nach Material sortiert ist; relevant für REN-103 Punkt 3).

## Gelesen

Alle sechs Dateien vollständig gelesen: ja (`D3D11Renderer.cpp` Z.1-8601 in Blöcken, `D3D11Renderer.h`, `D3D11ShaderManager.cpp/.h`, `D3D11MaterialBindings.h`, `D3D12MaterialRootSignature.h`); zur Verifikation außerhalb des Slices nur `MaterialShaderLibrary.cpp:925-1010`, `MaterialGraph.cpp:1981-1990`, `SsaoKernel.cpp:17-30`, `SkyNoise3D.cpp:10-40`, `D3D_Shared/HlslSources.h:1-40` und Grep-Treffer in den Backend-Dateien.
