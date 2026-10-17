# Rendering Slice 7 – OpenGLRenderer.h, OpenGLShaderManager, Software-Backend, VulkanMaterialLayout.h

## Zusammenfassung
Der Slice ist klein (2640 Zeilen) und besteht fast nur aus Headern bzw. dem schmalen Software-Backend. Der Code ist durchgehend gut kommentiert; die Software-Rasterung ist sauber vom Fenster-Shell getrennt und hat bereits Dirty-Rects. Die größten Hebel: (1) `OpenGLRenderer.h` trägt rund 600 Zeilen handgepflegter `int m_u…`-Uniform-Locations in drei fast identischen Sätzen (unlit/skinned/instanced), obwohl `SceneLightingLocs` und `GISceneLocs` schon den besseren Weg zeigen; (2) der Software-Renderer macht pro Dirty-Rect und Objekt unnötige Arbeit (Texture-Lookup, sin/cos vor dem Reject) und konvertiert jedes Pixel über `SDL_MapRGBA`; (3) `OpenGLShaderManager` ist faktisch toter Code. Funde: 1 Hoch, 3 Mittel, 5 Niedrig. Keine Messdaten vorhanden; Performance-Schätzungen sind als solche markiert.

## Funde

### REN-701 – Drei parallele Uniform-Location-Sätze (unlit / skinned / instanced) im OpenGLRenderer
- **ID**: REN-701
- **Kategorie**: A
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Rendering/include/Backends/OpenGL/OpenGLRenderer.h:470-514`, `:516-556`, `:558-601`; Verbraucher in `src/Backends/OpenGL/OpenGLRenderer.cpp:11501`, `:11545`, `:12547`
- **Problem**: Die drei Programme werden laut Header-Kommentar (`:206-210`) aus demselben `kUnlitFS` gelinkt und deklarieren „byte-identische“ Licht-/Schatten-Uniforms. Trotzdem existieren drei benannte Sätze à ca. 40 `int`-Member (`m_uLightCount` / `m_uSkinnedLightCount` / `m_uInstLightCount`, usw.), die an je einer Stelle mit `glGetUniformLocation` gefüllt und an je einer Stelle wieder zu einem `SceneLightingLocs` zusammengesetzt werden (`BindSceneLighting({ m_uLightCount, m_uLightPos, … })`). Zusätzlich gibt es mit `GISceneLocs m_giLocsUnlit/Skinned/Instanced` (`:1302`) bereits das richtige Muster (struct + `Fetch…(program)`). Jede neue Beleuchtungs-Uniform (Wetness, CloudShadow, SpecAA wurden offenbar so nachgezogen: `m_uSpecAA` vs. `m_uInstSpecAA`, aber kein Skinned-Gegenstück) muss in 3 Member-Listen, 3 Fetch-Blöcken und 3 Zusammenbau-Stellen gepflegt werden; Auslassungen sind bereits sichtbar (`m_uSkinnedWetness/Snow` fehlen, `m_uInstShadowEnabled` steht isoliert).
- **Code-Auszug**:
```cpp
int m_uLightCount = -1; ... int m_uCascadeVP = -1; ...      // :479-
int m_uSkinnedLightCount = -1; ... int m_uSkinnedCascadeVP = -1;   // :528-
int m_uInstLightCount = -1; ... int m_uInstCascadeVP = -1;         // :571-
```
- **Lösung**: (1) `struct ScenePassLocs { SceneLightingLocs light; GISceneLocs gi; int ao, viewport, ssaoEnabled, wetness, snow, cloudShadowMap/A/B, fogDensity, fogHeightFalloff, sunDir, ambient, skyEnv, color, hasTex, tex, metallic, roughness, opacity, noShadow; };` (2) `ScenePassLocs FetchScenePassLocs(unsigned prog) const;` – eine Funktion, die die gemeinsamen Namen einmal abfragt (fehlende → -1, wie bisher). (3) Die drei Programme behalten nur ihre Sonder-Uniforms (`mvp/model` bzw. `viewProj` bzw. `bones`) als kleine Zusatzmember. (4) `m_locsUnlit/m_locsSkinned/m_locsInstanced` ersetzen die ~120 Member; `BindSceneLighting(m_locsX.light, …)`. Schrittweise: erst Skinned (kleinster Satz), dann Instanced, dann Unlit; je Schritt bytegleiche Screenshots (`HE_UI_DUMP_DIR`/Headless-Dump) vergleichen.
- **Erwarteter Effekt**: Header −~110 Zeilen und −~120 Member, ca. −300 Zeilen in der .cpp (Schätzung); weniger Drift-Risiko. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Mittel: Reihenfolge/Namen der Uniforms müssen exakt gleich bleiben, `-1`-Semantik (Programm ohne `HE_CLUSTERED`) beibehalten. Absicherung: GL-Render-Vergleich vor/nach (Szene mit Skinned, Instanced, Schatten, Cluster), da `tests/` kein GL-Gerät nutzt.

### REN-702 – Software-Renderer: Texture-Lookup und sin/cos vor dem Reject, O(Rects × Objekte)
- **ID**: REN-702
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRaster.cpp:254-302`, aufgerufen aus `SoftwareRenderer.cpp:205-210`
- **Problem**: `draw()` wird pro Dirty-Rect (bis 32, `SoftwareRaster.cpp:213`) mit der **gesamten** Objektliste aufgerufen. Pro Objekt passiert vor dem Reject-Test (`px1 <= px0`, `:302`): `texResolve(...)` (`:276`, bei `SoftwareRenderer.cpp:27-49` ein `cm->getTexture(id)`-Lookup), `std::cos/std::sin` (`:294`, auch bei `rot == 0`) und `quadBounds(o)` (bei Rotation nochmals cos/sin). Für Objekte, die das Dirty-Rect gar nicht berühren (der Normalfall bei Teilupdates), ist das reine Verschwendung: 32 Rects × N Quads Lookups/Trigonometrie pro Frame.
- **Code-Auszug**:
```cpp
if (o.type == 0 && texResolve && o.textureAssetId != HE::UUID{})
    tv = texResolve(o.textureAssetId, texUser);          // :276 – vor dem Reject
...
const float cs = std::cos(rot), sn = std::sin(rot);        // :294 – auch bei rot == 0
const glm::vec4 bounds = quadBounds(o);
...
if (px1 <= px0 || py1 <= py0) continue;                    // :302
```
- **Lösung**: (1) `bounds = quadBounds(o)` und die px/py-Schnittmenge **zuerst** berechnen und `continue`n; (2) `texResolve` erst danach aufrufen; (3) `cs/sn` nur `if (rot != 0)` (oder `cs=1,sn=0`). Ergebnis ist identisch, nur die Reihenfolge ändert sich. Optional später: `quadBounds` einmal pro Objekt und Frame vorberechnen statt pro Rect.
- **Erwarteter Effekt**: Bei vielen Quads (Listen, Inventare) und mehreren Dirty-Rects spürbar weniger CPU; Größenordnung linear in (Rects×Quads) – **messen** (unsicher, wie viele Quads reale Apps haben).
- **Risiko / Verifikation**: Niedrig, rein umgeordnet. Bestehende Raster-Tests (`tests/test_ui_widgets.cpp` nutzt `HE::sw::draw`) müssen pixelgleich bleiben.

### REN-703 – Pixelweiser `SDL_MapRGBA`-Aufruf beim Blit in die Fensterfläche
- **ID**: REN-703
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRenderer.cpp:231-239`
- **Problem**: Jeder Pixel des Dirty-Bereichs geht durch `SDL_MapRGBA(fmt, nullptr, r,g,b,255)`, eine nicht inlinebare Bibliotheksfunktion mit generischem Format-Pfad. Bei einem vollen 4K-Repaint (die Datei selbst nennt 8,3 Mio. Pixel, `SoftwareRaster.h:100`) sind das 8,3 Mio. Aufrufe pro Frame – bei Resize/Drag genau der Fall, für den der Blit gebaut wurde.
- **Code-Auszug**:
```cpp
for (int x = x0; x < x1; ++x, src += 4)
    dst[x] = SDL_MapRGBA(fmt, nullptr, src[0], src[1], src[2], 255);
```
- **Lösung**: Vor der Schleife einmal prüfen: `surf->format` ist `SDL_PIXELFORMAT_XRGB8888`/`ARGB8888` → `dst[x] = 0xFF000000u | (src[0]<<16) | (src[1]<<8) | src[2];` (bei `ABGR8888`/`XBGR8888` Kanäle vertauschen); sonst bestehender generischer Pfad als Fallback. Alternativ `SDL_ConvertPixels` zeilenweise.
- **Erwarteter Effekt**: Schätzung 3-10x schnellerer Blit bei vollem Repaint; **messen**.
- **Risiko / Verifikation**: Farbkanal-Vertauschung („it looked blue“, Kommentar `:214-217`) ist das Risiko. Test: Pixel-Roundtrip über eine Offscreen-`SDL_Surface` je Format (XRGB/ARGB/ABGR) gegen den `SDL_MapRGBA`-Pfad vergleichen.

### REN-704 – TAA-Velocity: zwei `unordered_map<uint32_t, mat4>` mit Knoten-Allokation pro Objekt und Frame
- **ID**: REN-704
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: Deklaration `OpenGLRenderer.h:877-878`; Nutzung `src/Backends/OpenGL/OpenGLRenderer.cpp:4746-4788`
- **Problem**: Pro Frame wird `m_taaCurTransforms.clear()` (gibt alle Knoten frei) aufgerufen und danach pro sichtbarem Objekt `m_taaCurTransforms[obj.entityId] = …` (je ein Heap-Knoten, 64 B + Overhead) sowie ein `find` in der Vorgänger-Map ausgeführt; am Ende `swap`. Bei 10k sichtbaren Objekten sind das 10k Allokationen/Frees plus 20k Hash-Operationen pro Frame, nur solange TAA aktiv ist.
- **Code-Auszug**:
```cpp
m_taaCurTransforms.clear();
...
const auto it = m_taaPrevTransforms.find(obj.entityId);
...
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: Flache Struktur: `std::vector<std::pair<uint32_t, glm::mat4>>` für Cur (reserve, kein Free), nach `entityId` sortiert (oder dichte Tabelle `entityId → slot` falls Ids dicht sind) und `Prev` per `lower_bound` suchen; alternativ `robin_hood`/Flat-Map, falls im Projekt vorhanden. Pro Frame wäre dann nur `clear()` ohne Freigabe nötig.
- **Erwarteter Effekt**: Entfällt 1 Alloc/Objekt/Frame; Schätzung −0,2…0,5 ms bei 10k Objekten, nur mit TAA – **messen**.
- **Risiko / Verifikation**: Niedrig; Verhalten (neues Objekt → keine Bewegung) beibehalten. Prüfen, ob `entityId` bei Instanz-Ketten mehrfach vorkommt (letzter Eintrag gewinnt in der Map; Sortier-/Duplikatsemantik gleich halten).

### REN-705 – Software-Renderer kopiert die komplette Quad-Liste jeden Frame (auch im „nichts geändert“-Fall)
- **ID**: REN-705
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Rendering/src/Backends/Software/SoftwareRenderer.cpp:189-197`, `:211`; `SoftwareRenderer.h:62`
- **Problem**: Genau der Pfad, der laut Kommentar „nichts kostet“ (`:191-193`), kopiert `m_renderWorld.uiObjects` nach `t.prevObjects` (`:195`). `UIRenderObject` enthält u. a. UUIDs, vec4s und (vermutlich) mehr; für N Quads ist das ein O(N)-Speichercopy pro Idle-Frame. Im Idle-Fall sind beide Listen nach `dirtyRects` gleich (außer evtl. unsichtbare Quads, die `dirtyRects` überspringt, `:207`).
- **Code-Auszug**:
```cpp
t.lastPixels = 0;
t.prevObjects = m_renderWorld.uiObjects;
return;
```
- **Lösung**: Im Idle-Fall die Kopie weglassen, **wenn** `prevObjects.size() == uiObjects.size()` (dann sind alle Quads per `sameQuad` gleich oder unsichtbar-gleich). Im Dirty-Fall `std::swap(t.prevObjects, m_renderWorld.uiObjects)` statt Kopie, sofern `extractUI` die Liste vor dem Befüllen leert (`RenderExtractor::extractUI` prüfen). 
- **Erwarteter Effekt**: Idle-Frame wird wirklich kostenlos; Gewinn klein (Schätzung <0,1 ms bei 1000 Quads) – **messen**.
- **Risiko / Verifikation**: Niedrig-Mittel: nach dem Swap ist `uiObjects` leer/alt; `GetFrameGpuStats` nutzt nur `lastQuads`, `CaptureViewport` nur `frame`. Test: zwei identische Frames, dann einer mit geändertem Quad, Dirty-Rect-Ergebnis unverändert.

### REN-706 – `OpenGLShaderManager` ist praktisch toter Code und hat Fehler im Lifecycle
- **ID**: REN-706
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/Backends/OpenGL/OpenGLShaderManager.cpp:14-104`, `include/Backends/OpenGL/OpenGLShaderManager.h:1-17`, `OpenGLRenderer.h:273`, `OpenGLRenderer.cpp:3372`
- **Problem**: (a) Der einzige Verwendungsort von `m_shaderManager` ist die Zuweisung `m_shaderManager = OpenGLShaderManager();` (grep über `src/HE_Rendering`: keine Aufrufe von `load/compile/createProgram`); der Renderer baut seine Programme selbst (`LinkSceneProgram`, `GetOrBuild…Program`). (b) `m_loadedShaders`/`m_loadedPrograms` werden nie befüllt (anders als D3D11/D3D12, `D3D11ShaderManager.cpp:45,146`), also ist `cleanup()` ein No-op. (c) `createProgram` gibt bei Link-Fehler die `program.id` nicht frei (`:76-84`, Leak); `load` legt bei Fehlern kein `glDeleteShader` an. (d) `fs::exists` + `ifstream` ist doppelte Dateisystemarbeit. (e) Der Header hat kein `#pragma once` und keinen `<vector>`-Include; er hängt vom Vorinclude von `ShaderManager.h` ab, und `OpenGLRenderer.h` zieht ihn in jeden Includer.
- **Code-Auszug**:
```cpp
m_shaderManager = OpenGLShaderManager();   // OpenGLRenderer.cpp:3372 – einzige Verwendung
...
void OpenGLShaderManager::cleanup()
{   for (const auto& program : m_loadedPrograms) releaseProgram(program); ... }  // Listen immer leer
```
- **Lösung**: Entweder entfernen (Member, Include, Zeile 3372, .cpp/.h, CMake-Einträge `CMakeLists.txt:43`) – oder, falls das `ShaderManager`-Interface backend-übergreifend gebraucht wird, die Listen füllen, `#pragma once` ergänzen, Leak in `createProgram` beheben. Vor dem Entfernen klären, ob `ShaderManager` in anderen Modulen (Editor-Shaderkompilierung) über die Basisklasse verwendet wird (Hinweise in `HE_Editor/EditorUI.cpp:173`).
- **Erwarteter Effekt**: −~120 Zeilen, ein Include weniger im schweren Header; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Niedrig; Build aller Targets (Windows/Linux GL), `grep` nach `ShaderManager` in `HE_Editor`/`HE_Game`.

### REN-707 – Per-Pixel-Konstanten und Byte-Schleifen im Software-Rasterizer
- **ID**: REN-707
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `SoftwareRaster.cpp:357-374`, `:72-76`, `:85-90`, `:419-430`
- **Problem**: (a) Der lineare Gradient berechnet `std::sin(a)` / `std::cos(a)` pro Pixel (`:370-371`), obwohl der Winkel pro Quad konstant ist. (b) Pro Pixel wird die gesamte Verzweigungskette (type, tv.rgba, gradient, maxR/border/blur) neu ausgewertet, und `over` ruft `clamp01` dreifach. Ein deckendes Quad ohne Rundung/Border/Textur (`src = fill`, `:379`) läuft trotzdem über Pixel-Source-Over mit Float-Division durch 255. (c) `Image::clear` schreibt byteweise statt `std::fill`/Wiederholung eines 32-Bit-Wertes; `clearRect` analog.
- **Code-Auszug**:
```cpp
const float a = o.gradientAngleDeg * 0.017453292f;
t = clamp01((lu - 0.5f) * std::sin(a) + (lv - 0.5f) * std::cos(a) + 0.5f);   // pro Pixel
```
- **Lösung**: `sin/cos` vor der Pixelschleife (neben `cs/sn`) berechnen. Optional: Schnellpfad für `fill.a >= 1` ohne Form/Textur: Span-Fill der Zeile mit einem `uint32`. `Image::clear` über ein `uint32_t` pro Pixel (`std::fill_n` auf `uint32_t*`, Alignment der `vector<uint8_t>` beachten bzw. `memcpy`).
- **Erwarteter Effekt**: (a) klein; (c) klein; Schnellpfad: für große Flächen deutlich (Schätzung 5-10x für Vollflächen) – nur nach Messung, bevor die Funktion „drei Kopien in Sync“ (`SoftwareRaster.h:22-24`) verkompliziert wird.
- **Risiko / Verifikation**: (a) trivial, bytegleich; Schnellpfad ändert Rundung (`+0.5f`) potenziell – mit Pixelvergleichs-Tests absichern.

### REN-708 – Schwerer Header `OpenGLRenderer.h` mit impliziten Abhängigkeiten
- **ID**: REN-708
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.h:1-25`, `:642`
- **Problem**: Der Header inkludiert RenderWorld, RenderExtractor, FrustumCuller, OcclusionCuller, RenderSorter, RenderGraph, CommandBuffer, GiBvh, GIProbeGrid, MaterialShaderLibrary und `OpenGLShaderManager.h`, weil alle Member by value gehalten werden. Gleichzeitig fehlen direkte Includes: `std::unordered_set` (`:642`) und `std::vector` kommen nur transitiv, `<glm/…>` wird nirgends direkt inkludiert. Es gibt sinnvolle `struct`-Vorwärtsdeklarationen (`:22-25`), aber der Header selbst bleibt der schwerste Teil jeder GL-Übersetzungseinheit.
- **Code-Auszug**:
```cpp
std::unordered_set<unsigned int> m_glArrayTex;   // :642 – kein #include <unordered_set>
```
- **Lösung**: Fehlende Std-Includes (`<unordered_set>`, `<vector>`, `<glm/glm.hpp>`) ergänzen (sofort, risikofrei). Mittelfristig GI/SSR/DoF/MotionBlur-Zustand (ca. Zeilen 980-1380) in Teilstrukturen (`struct GiState`, `struct SsrState`) in eigenen Headern auslagern und im Renderer per `std::unique_ptr` oder Wert halten, sodass nur `OpenGLRenderer_*.cpp` die Details sehen. Das ist die gleiche Auslagerung wie REN-701.
- **Erwarteter Effekt**: Kürzere Kompilierzeit der GL-Dateien (Schätzung gering, **messen** mit `-ftime-trace`); bessere Lesbarkeit.
- **Risiko / Verifikation**: Niedrig für Includes; Mittel für die Strukturaufteilung (Zugriffe in einer 12k+-Zeilen-.cpp).

### REN-709 – VulkanMaterialLayout: Tabellen-Redundanz und volle Set-Größe pro Draw
- **ID**: REN-709
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `include/Backends/Vulkan/VulkanMaterialLayout.h:59-109`, `:175-197`, `:140-144`; Verbraucher `src/Backends/Vulkan/VulkanRenderer.cpp:2805-2880`, `:8300`
- **Problem**: (a) Die Pool-Dimensionierung (`VulkanRenderer.cpp:2873-2878`) legt pro Draw ein Set mit **25 Deskriptoren** an (17 Sampler, 5 UBO, 3 SSBO), obwohl pro Draw typischerweise nur wenige (b0,b1,b2,b3) material-spezifisch sind; die übrigen 13+ Sampler (GI, AO, SkyEnv, CSM, …) sind frame-konstant. Das ist ein Descriptor-Set-Allocate + Write pro Material-Draw (`k_matMaxDraws`) – ein klassisches Ziel für „Set 0 = frame-konstant, Set 1 = pro Material/Draw“. (b) Die Resolve-Tabelle dupliziert 12 Zeilen der Material-Tabelle (10-13, 15-18, 31-33), manuell parallel gepflegt, nur durch `static_assert`-Zählungen abgesichert. (c) `landscapeWeightsFit` vergleicht zweimal `fragmentSamplerCount(true)`; `std::min`-Vergleich gegen beide Limits wäre kürzer. (d) `kPreClusterBindingCount = kBindingCount - 3` verdrahtet die Zahl 3 getrennt von den drei Cluster-Konstanten.
- **Code-Auszug**:
```cpp
constexpr uint32_t kPreClusterBindingCount = kBindingCount - 3;
...
return fragmentSamplerCount(true) <= maxPerStageSamplers
    && fragmentSamplerCount(true) <= maxPerStageSampledImages;
```
- **Lösung**: (a) Layout in zwei Sets aufteilen: Set 0 mit den frame-konstanten Samplern/UBO (einmal pro Frame geschrieben), Set 1 mit b0..b9 der Materialparameter; Descriptor-Writes pro Draw sinken auf ~4-6. Das ist ein Umbau in `VulkanRenderer.cpp`/Shader-Bindings der `MaterialShaderLibrary` (SPIR-V-Generierung!) und nur mit Messung (Draws × Writes) sinnvoll. (b)-(d) kosmetisch: gemeinsame `constexpr`-Teiltabelle bzw. benannte Konstante `kClusterBindingCount = 3`.
- **Erwarteter Effekt**: (a) CPU-Zeit im Vulkan-Material-Draw-Loop sinkt vermutlich merklich bei vielen Materialdraws; **unsicher / messen** (hängt davon ab, ob `VulkanRenderer.cpp:8559` pro Draw alle 25 oder nur geänderte Bindings schreibt – dort nicht gelesen, nicht Teil des Slice). (b)-(d) keine.
- **Risiko / Verifikation**: (a) Hoch: ändert Shader-Bindings, D3D12/Metal-Parallelen und den SPIR-V-Reflexionstest (`he_tests`, Layout-gegen-SPIR-V). (b)-(d) niedrig; die vorhandenen `static_assert`s decken sie.

## Cross-Modul-Hinweise
- `src/HE_Rendering/src/Backends/Vulkan/VulkanRenderer.cpp:2860-2880, 8290-8560`: Pro-Draw-Descriptor-Set für Graph-Materialien (siehe REN-709); `k_matMaxDraws` als harte Grenze mit `noteRingFull()` – Verhalten bei Überlauf: Draw verschwindet leise. Das gehört in den Vulkan-Slice.
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:11501/11545/12547` (BindSceneLighting-Aufrufer) und `:6063` (`FetchGISceneLocs`) sind die Ziel-Stellen von REN-701; Metal/D3D11/D3D12 haben parallele Uniform-/Binding-Sätze (`MetalRenderer.mm`, `D3D11Renderer.cpp`, `D3D12Renderer.cpp`) und sollten bei einer Vereinheitlichung geprüft werden.
- `src/HE_Rendering/src/Backends/D3D11/D3D11ShaderManager.cpp`, `D3D12ShaderManager.cpp`: Gegenstücke zu REN-706; dort werden die `m_loaded*`-Listen befüllt.
- `src/HE_Scene` / `RenderExtractor::extractUI`: REN-705 hängt davon ab, ob `extractUI` `uiObjects` leert und mit Kapazität wiederverwendet.
- `HE_Editor/EditorUI.cpp:173`, `EditorApplication.cpp:276`: Kommentare zum früher leckenden `fs`-Alias aus `ShaderManager.h` (REN-706, vor Entfernen prüfen).

## Unsicher / Messung nötig
- REN-702/703/705/707: tatsächliche CPU-Kosten des Software-Renderers bei realen Anwendungs-UIs (Quad-Zahl, Fenstergröße) messen; ohne Messung bleiben es Schätzungen.
- REN-704: Kosten der TAA-Velocity-Map pro Frame bei großen Szenen (10k+ Objekte) per Profiler (CPU-Zeit in `RenderVelocity`).
- REN-709: Anzahl der Deskriptor-Writes pro Material-Draw in `VulkanRenderer.cpp:8559` (nicht Teil dieses Slice).
- Ob `UIRenderObject` Heap-Member enthält (bestimmt die Kosten der Kopie in REN-705): `Renderer/UIRenderObject.h` prüfen.
- REN-701: Anzahl tatsächlich fehlender Uniforms (Skinned-Wetness/Snow) – ob absichtlich oder Versäumnis, nicht verifiziert.
- Per-Draw `glUniform*`-Aufrufe im Opaque-Loop (mutmaßlich viele; liegt in `OpenGLRenderer.cpp`, nicht Teil dieses Slice).

## Gelesen
Alle acht Dateien des Slice vollständig gelesen: ja (`OpenGLRenderer.h` 1397 Zeilen in zwei Blöcken, `OpenGLShaderManager.cpp/.h`, `SoftwareRaster.cpp/.h`, `SoftwareRenderer.cpp/.h`, `VulkanMaterialLayout.h`); `OpenGLRenderer.cpp`, `VulkanRenderer.cpp` und `D3D*ShaderManager.cpp` nur per gezieltem Grep/Ausschnitt zur Verifikation der Querverweise.
