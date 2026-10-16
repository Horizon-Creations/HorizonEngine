# Rendering Slice 6 – src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp (Zeilen 1–13589)

## Zusammenfassung

`OpenGLRenderer.cpp` ist funktional sehr vollständig und durchgehend gut kommentiert, aber in drei Punkten teuer: (1) der Pro-Draw-Call-Pfad für Graph-Materialien arbeitet ohne Memoisierung (GLSL-Kopie + Hash, Lighting-Struct-Fill, String-Schlüssel-Lookups pro Draw), obwohl für Mesh/Material-Parameter bereits ein `lastMatId`-Memo existiert; (2) UI-Quads und Schattenebenen werden ohne Batching bzw. ohne Caching jedes Frame komplett neu aufgebaut; (3) die Datei enthält sehr viel Copy-Paste (≈38 `glLinkProgram`-Blöcke, ≈34 `glGenFramebuffers`-Blöcke, dreifach geführte Uniform-Location-Listen, GLSL-Zwillinge). Zusätzlich gibt es tote Pfade (deaktiviertes Reflection-Temporal inkl. VRAM) und Lücken in `Shutdown()`.
Funde: **5 Hoch, 20 Mittel, 4 Niedrig** (29 gesamt). Alle Laufzeit-Aussagen sind aus dem Code abgeleitet, nicht gemessen; wo die Größenordnung unklar ist, steht sie unter „Unsicher / Messung nötig“.

---

## Funde

### REN-601 – `fillMatLight` füllt das komplette Lighting-UBO-Struct bei jedem Material-Draw, obwohl nur einmal pro Frame hochgeladen wird
- **ID**: REN-601
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12114-12126` (Aufrufstelle), `11598-11690` (Lambda `fillMatLight`)
- **Problem**: Im Forward-Pfad läuft für *jeden* Draw-Call mit Graph-Material (`matProg != 0`) `Lighting lit{}` (Null-Init, > 2 KB: `localShadowVP[16][16]` allein 1 KB) und `fillMatLight(lit)` (dominantDirectionalLight, `FillMaterialLightWindow`, bis zu 16 `zRemap * localViewProj`-Matrixprodukte + memcpy, Cluster-/GI-/Wind-/Weather-/Water-Fill). Das Ergebnis wird nur beim ersten Draw des Frames hochgeladen (`m_matLightUploadedThisFrame`); in allen weiteren Draws wird der Fill komplett verworfen. Der Transparenz-Pfad (12770-12778) macht es richtig, der Opaque-Pfad nicht.
- **Code-Auszug**:
```cpp
glUseProgram(matProg);
HE::MaterialShaderLibrary::Lighting lit{};
fillMatLight(lit); // shared heLitP ABI fill (see the lambda above)
if (!m_matLightUploadedThisFrame) { ... glBufferSubData(..., &lit); m_matLightUploadedThisFrame = true; }
```
- **Lösung**: Fill in den `if` verschieben (wie bei 12770):
```cpp
if (!m_matLightUploadedThisFrame) {
    HE::MaterialShaderLibrary::Lighting lit{};
    fillMatLight(lit);
    glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    m_matLightUploadedThisFrame = true;
}
```
- **Erwarteter Effekt**: (Schätzung) pro Graph-Material-Draw ~1–3 µs CPU gespart (Struct-Init + Fill); bei 1–5k Material-Draws ein spürbarer Teil des CPU-Frames.
- **Risiko / Verifikation**: Verhalten identisch (Fill ist innerhalb eines Frames deterministisch; `lit` wird sonst nirgends gelesen). Prüfen, dass kein Draw `m_matLightUploadedThisFrame=false` zwischen Draws setzt (UI/Preview setzen es, laufen aber nicht innerhalb der Schleife). Test: Headless-Frame-Dump vorher/nachher bytegleich.

### REN-602 – `resolveMaterialShader` kopiert und hasht die komplette GLSL-Quelle pro Draw-Call und Frame, ohne Memo
- **ID**: REN-602
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12066-12079` (Forward), `12006-12034` (transluzent), `11754-11788` (Deferred, zusätzlich `resolveMaterialShaderGB`), `12675-12709` (Ribbons), `4000-4011` (UI pro Quad), `8956-8957`; Ursache `MaterialShaderLibrary.cpp:1000-1008` (`resolveFragment`)
- **Problem**: `resolveMaterialShader()` → `resolveFragment()` macht `glslOut = mat->customShaderFragGlsl;` (Kopie mehrerer KB) und `std::hash<std::string>{}(glslOut)` (O(Länge)) — für *jeden* DrawCall, jeden Frame, nur um danach einen `unordered_map<uint64_t>`-Treffer zu bekommen (`GetOrBuildMaterialProgram`). Zusätzlich werden pro DC `std::string shFrag, shVert` konstruiert, `getMaterial()` 2–3× aufgerufen, die `precompiledShaders`-Liste linear durchsucht und `HE::matGlslTextureArrayMask` + `std::vector<float> mParams`-Kopie (Deferred, 11774) erzeugt. Mesh und Material-Parameter sind dagegen per `lastMatId`/`lastMeshId` memoisiert (11961-11973) — die Custom-Material-Auflösung nicht.
- **Code-Auszug**:
```cpp
uint64_t shKey; std::string shFrag, shVert;
if (resolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert)) { ...
    matProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
```
```cpp
glslOut = mat->customShaderFragGlsl;
hashOut = std::hash<std::string>{}(glslOut);   // MaterialShaderLibrary.cpp:1006-1007
```
- **Lösung**: (1) Im Renderer die Auflösung an das vorhandene `lastMatId`-Memo hängen: pro Material-ID einmal pro Frame `{ matProg, gbProg, precompiled*, arrMask }` ermitteln (kleine `unordered_map<UUID, ResolvedMat>` oder Einzel-Memo wie `cMesh`), pro Draw nur noch lesen. (2) In `MaterialShaderLibrary` Hash einmal beim Laden/Editieren in `MaterialAsset` ablegen (`customShaderHash`) und die Quelle nur bei Cache-Miss kopieren (`resolveShaders` mit `const std::string*`-Out oder Key-only-Variante). (3) Invalidierung über das vorhandene `InvalidateMaterial`.
- **Erwarteter Effekt**: (Schätzung) pro Custom-Material-Draw Wegfall von 1 String-Kopie + 1 O(n)-Hash (≈ 10–50 µs bei 10–40 KB GLSL) → bei einigen hundert Draws ein sichtbarer CPU-Block; im Deferred-Pfad doppelt (Forward + GB-Variante).
- **Risiko / Verifikation**: Cache muss bei Material-Edit (Graph-Neucompile → neue `customShaderFragGlsl`) invalidiert werden (`m_pendingMaterialInvalidations` existiert bereits). Tests: Material-Hot-Reload im Editor, `tests/` zu MaterialShaderLibrary; Bildvergleich Forward/Deferred.

### REN-603 – UI-Pass: ein Draw pro Quad/Glyph mit 15 `glUniform*`-Aufrufen, kein Batching, kein Uniform-Cache
- **ID**: REN-603
- **Kategorie**: B/D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:7873-8027` (insb. `8006-8025`), Shader `3117-3253`
- **Problem**: Jedes `UIRenderObject` (auch jedes einzelne Glyphen-Quad, `type == 2`) setzt unbedingt Rect, Color, UVRect, Rotation, Mode, CornerRadius, BorderWidth, BorderColor, Gradient, GradientColor, GradientAngle, GradientShape, Blur, InnerBlur, InnerColor (15 Uniform-Calls) und zeichnet ein `GL_TRIANGLE_STRIP` mit 4 Vertices. Ein Text mit 100 Zeichen = 100 Draws + 1500 Uniform-Calls pro Frame. Die allermeisten Quads haben Default-Werte für Border/Gradient/Blur/Inner, werden aber trotzdem jedes Mal neu gesetzt.
- **Code-Auszug**:
```cpp
glUniform4f(m_uUIRect,  obj.position.x, obj.position.y, obj.size.x, obj.size.y);
glUniform4f(m_uUIColor, ...); ... glUniform4f(m_uUIInnerColor, ...);
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```
- **Lösung**: Stufe 1 (exakt, minimal): Letzte gesetzte Werte für die selten wechselnden Uniforms (`CornerRadius`, `Border*`, `Gradient*`, `Blur`, `InnerBlur`, `InnerColor`, `Rotation`) in einer kleinen Struct cachen und nur bei Änderung setzen. Stufe 2: Läufe aufeinanderfolgender Quads mit gleichem Programm/Atlas/Clip in einen Instanz-Puffer (pro Quad ein `struct{vec4 rect, color, uv, ...}`) schreiben und mit `glDrawArraysInstanced(GL_TRIANGLE_STRIP,0,4,n)` zeichnen (Painter-Reihenfolge bleibt erhalten, da nur aufeinanderfolgende Quads zusammengefasst werden). Stufe 2 verlangt Shader-Änderung `kUIVS/kUIFS` und Spiegelung im Metal-`kUIMSL`.
- **Erwarteter Effekt**: (Schätzung) Stufe 1: 70–90 % der UI-Uniform-Calls entfallen; Stufe 2: Draw-Calls der UI von O(Quads) auf O(Zustandswechsel).
- **Risiko / Verifikation**: Stufe 1 risikolos. Stufe 2 betrifft Rundung/Gradient/Rotation-Pfade im Shader → Pixel-Vergleich mit den UI-Widget-Tests (`tests/test_ui_widgets.cpp`, Software-Raster-Dump) und den GL-Thumbnails (`RenderWidgetThumbnail`).

### REN-604 – UI-Material-Quads: pro Quad Auflösung, UBO-Uploads und (bei „Frosted“) komplette Framebuffer-Kopie + Mipmaps
- **ID**: REN-604
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:7846-7871` (`snapshotBackdrop`), `7873-7975` (Material-Zweig), `3997-4011` (`GetOrBuildUIMaterialProgram`)
- **Problem**: (a) `backdropStale` wird nach *jedem* Quad wieder `true` (7973, 8026); jedes folgende Quad, dessen Fragment `heBackdrop` enthält, löst `glCopyTexSubImage2D` über das ganze Ziel + `glGenerateMipmap` aus → N Frosted-Elemente = N Vollbild-Kopien mit Mip-Kette pro Frame. (b) Pro Material-Quad: `GetOrBuildUIMaterialProgram` → `resolveMaterialShader` (GLSL-Kopie + Hash, siehe REN-602) + `fragGlsl.find("heBackdrop")` über die gesamte Quelle (Zeile 4008, vor dem Memo-Lookup!) + `EnsureMaterialUBOs`; danach `getMaterial`, `padded[64]`-Memcpy, 3 UBO-`glBufferSubData` (U, HeUI, HeParams), pro Textur ein `std::string gp`-Copy + `ResolveGraphTexture` mit String-Schlüssel (REN-607), `matGlslTextureArrayMask`. (c) Die Lighting-UBO wird dagegen korrekt einmal pro Pass hochgeladen (7901).
- **Code-Auszug**:
```cpp
if (usesBackdrop) *usesBackdrop = fragGlsl.find("heBackdrop") != std::string::npos;  // vor m_uiMaterialPrograms.find
...
glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);
glGenerateMipmap(GL_TEXTURE_2D);
```
- **Lösung**: (a) Snapshot-Bedarf nur neu nehmen, wenn seit dem letzten Snapshot tatsächlich etwas *unter* dem Rect gezeichnet wurde (Dirty-Rect-Schnitt) bzw. mehrere Frosted-Quads ohne dazwischenliegende Zeichnung teilen sich den Snapshot; Mips nur bis zur nötigen Blur-Stufe. (b) `ResolvedUIMaterial {program, usesBackdrop, arrMask}` in `m_uiMaterialPrograms` ablegen (statt nur `unsigned int`), Auflösung nur bei Miss; `usesBackdrop` beim Bau berechnen. (c) `paramData` + Textur-IDs pro Material-ID einmal pro Pass vorbereiten.
- **Erwarteter Effekt**: (Schätzung) (a) bei 5 Frosted-Panels 5 × (Vollbild-Copy + Mip-Chain) → 1–2; (b) pro Material-Quad ~10–50 µs CPU.
- **Risiko / Verifikation**: (a) ändert das Aussehen nur, wenn „stale“ bewusst Überlagerungen abbilden soll (Kommentar: Dialog über Panel blurt das Panel) → Dirty-Rect-Logik muss genau diese Reihenfolge erhalten. Visueller Test mit Dialog-über-Panel-Szene.

### REN-605 – Schattenpass: pro Cascade und pro lokaler Ebene vollständiger Cull + Sort, keine Wiederverwendung statischer Ebenen
- **ID**: REN-605
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:11023-11083` (`renderDepthLayer`), `11085-11098`
- **Problem**: `renderDepthLayer` ruft pro Layer `m_culler.cull(...)`, `m_sorter.sort(...)` und `batchDepthCasters(...)` über die gesamte `RenderWorld` auf: 3 Cascades + bis zu 16 lokale Ebenen (Punktlicht = 6 Ebenen) = bis zu 19 Cull+Sort-Durchläufe plus 19 Depth-Renderings pro Frame — auch wenn Licht und alle Caster in der Ebene unverändert sind (typisch: statische Lampen in statischer Geometrie). Dazu kommt das Hochladen der Instanz-Matrizen je Batch/Ebene (siehe REN-608).
- **Code-Auszug**:
```cpp
glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, 0, layer);
glClear(GL_DEPTH_BUFFER_BIT);
m_culler.cull(m_renderWorld, vp, m_shadowVisible);
m_sorter.sort(m_renderWorld, m_shadowVisible, m_shadowSorted);
```
- **Lösung**: (1) Einmal pro Frame nach Mesh-ID vorsortierte Caster-Liste (nur `castsShadow`) erzeugen; pro Layer nur noch frustum-filtern (stabiler Filter ⇒ Reihenfolge bleibt sortiert, kein Re-Sort). (2) Für lokale Ebenen einen Hash aus (Layer-ViewProj, Transform-Version/Hash der Caster im Frustum) halten; unveränderte Ebenen nicht neu rendern (Atlas-Layer bleibt erhalten, solange kein anderer Pass ihn überschreibt). (3) Cascades mit unverändertem Light-VP (Licht und Kamera statisch) analog überspringen.
- **Erwarteter Effekt**: (Schätzung) bei Szenen mit vielen statischen Punktlichtern kann der Schattenpass um >50 % schrumpfen; Cull/Sort-CPU-Anteil sinkt auf 1× pro Frame.
- **Risiko / Verifikation**: Cache-Invalidierung muss jede Caster-Änderung (Transform, Mesh-Wechsel, Sichtbarkeit, Skinned-Posen) und Atlas-Resize (`m_shadowSizeDirty`) erfassen; Fehler zeigen sich als „eingefrorene“ Schatten. Messen (siehe unten) vor Umsetzung; Test: bewegter Caster im statischen Licht, Licht bewegt, Licht an/aus.

### REN-606 – Transparenz-Sammlung: `std::vector<float>`-Kopie pro Instanz, Sort über große Structs, kein `reserve`
- **ID**: REN-606
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:11569-11589` (`TPDraw`), `11794-11811`, `11854-11869`, `12039-12056`, `12733-12734`, `12715-12722`
- **Problem**: `TPDraw` (≈250 Byte: 2 × mat4 + Farbe + Skalare + Pointer + `std::vector<float> params` + `gtex[4]`) wird für *jede Instanz* eines transparenten Batches (z. B. Partikel/Foliage) erzeugt, und jedes Mal wird `tp.params = mParams` kopiert (Heap-Alloc pro Instanz). Die Vektoren `transparent`/`deferredForward` werden pro Frame neu angelegt, ohne `reserve`. Danach sortiert `std::sort` die Structs *by value* (Moves von 250 B + vector-Move).
- **Code-Auszug**:
```cpp
auto pushTP = [&](const glm::mat4& t) {
    TPDraw tp{ viewProj * t, t, baseColor, ... backToFrontKey(t, camPos) };
    tp.params  = tpParams;        // Heap-Kopie pro Instanz
    transparent.push_back(std::move(tp)); };
...
std::sort(transparent.begin(), transparent.end(), [](const TPDraw& a, const TPDraw& b){ return a.distSq > b.distSq; });
```
- **Lösung**: `params` pro DC einmal in einen frame-lokalen Arena-Vektor (`std::vector<float> paramPool`) schreiben und im `TPDraw` nur `uint32_t paramOffset, paramCount` halten. `transparent`/`deferredForward` als Member (Kapazität bleibt) mit `clear()` pro Frame. Sortierung über `std::vector<uint32_t>` Indizes + Key-Array (`{distSq, index}`), anschließend in Reihenfolge abarbeiten.
- **Erwarteter Effekt**: (Schätzung) bei 10k transparenten Instanzen: 10k Heap-Allokationen/Frame entfallen, Sort verschiebt 16 B statt 250 B pro Swap.
- **Risiko / Verifikation**: Sortierstabilität: `std::sort` ist nicht stabil – bei gleichem `distSq` kann die Reihenfolge variieren; mit Index-Tie-Break wird sie deterministisch (leichte, harmlose Änderung). Bildvergleich mit Partikel-Szene.

### REN-607 – Textur-Caches mit `std::string`-Schlüsseln und `to_string`-Bau im Draw-Pfad
- **ID**: REN-607
- **Kategorie**: D/C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:8824-8844` (`ResolveGraphTexture`), `8860-8870` (`ResolveUITexture`), `8848-8851` (`BindGraphTexture`); Aufrufer u. a. `11783`, `11792`, `11852`, `11909`, `12029`, `12037`, `12172`, `12199`, `5998`, `7960`, `7998`, `8294`; Typen `OpenGLRenderer.h:640-644`
- **Problem**: Bei jedem Aufruf (pro Draw: bis zu 4 Graph-Texturen + 1 Weightmap auf Unit 13; pro UI-Quad mit Textur; pro GI-Landscape/Frame) wird `std::to_string(id.hi) + ":" + std::to_string(id.lo)` (+ `"#arr"`) gebaut – Strings > 15 Zeichen ⇒ Heap-Alloc – und in `unordered_map<std::string,…>` nachgeschlagen. `BindGraphTexture` macht zusätzlich einen `unordered_set<unsigned>::count` pro Bind. Das ist reiner Overhead, da die IDs (`HE::UUID`) bereits als Hash-Key taugen (`m_meshCache` nutzt sie).
- **Code-Auszug**:
```cpp
std::string key = id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path;
if (array) key += "#arr";
if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end()) ...
```
- **Lösung**: Zwei Maps: `unordered_map<UUID, GlTex>` (+ zweite für Array-Variante oder Schlüssel `{UUID, bool}`) für ID-Zugriffe, `unordered_map<std::string, GlTex>` nur für Pfad-Fallback (ID leer). `GlTex = {GLuint id; GLenum target;}` – dann entfällt `m_glArrayTex`. Invalidierung (`m_pendingTexInvalidations`, 10826-10837) wird dadurch ebenfalls einfacher (kein String-Bau).
- **Erwarteter Effekt**: (Schätzung) pro Graph-Material-Draw bis zu 5 Heap-Allokationen + 5 String-Hashes weniger; in UI-Szenen pro Image-Quad 1.
- **Risiko / Verifikation**: Invalidierungslogik anpassen (zwei Caches, `#arr`); Test: Landscape-Weightmap-Paint (re-upload), Texture-Array-Material.

### REN-608 – Instanz-Matrizen werden pro Pass und Batch wiederholt in denselben `m_instanceVBO` hochgeladen
- **ID**: REN-608
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:6500-6506` (GI-Prepass), `7317-7323` (SSAO), `11046-11051` (Schatten, je Layer/Batch), `11822-11826` (G-Buffer), `12085-12089` (Szene)
- **Problem**: Dieselben `dc.instanceTransforms` eines GeometryPass-Batches werden pro Frame bis zu 4–5× per `glBufferData(…GL_STREAM_DRAW)` in einen *einzigen* gemeinsamen VBO geschrieben (GI-Prepass, SSAO-/Reflection-Prepass, Szene; Schatten zusätzlich je Cascade und lokalem Layer). Jeder Upload orphaned den Puffer; auf manchen Treibern bedeutet das Allokation + Kopie + ggf. Sync auf vorherige Draws.
- **Code-Auszug**:
```cpp
glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
glBufferData(GL_ARRAY_BUFFER, dc.instanceTransforms.size() * sizeof(glm::mat4), dc.instanceTransforms.data(), GL_STREAM_DRAW);
```
- **Lösung**: Einmal pro Frame alle Batch-Transformationen der `CommandBuffer` in einen großen Puffer packen (ein `glBufferData`/`glBufferSubData`, Offsets pro DC merken) und pro Draw nur die vier `glVertexAttribPointer(4..7, …, offset)` auf den DC-Offset setzen (GL 4.1-kompatibel; `BaseInstance` erst ab 4.2). Schatten-Batches analog mit eigener Offsets-Tabelle.
- **Erwarteter Effekt**: (Schätzung) 3–4 Orphan-Uploads weniger pro Batch und Frame; Treiberabhängig, daher messen.
- **Risiko / Verifikation**: Alle VAOs haben die Attribute auf `m_instanceVBO` gebunden (8637-8649) – `glVertexAttribPointer` pro Draw ändert VAO-State; Test mit GeometryPass-Batches (Foliage-Szene), Bildvergleich in Schatten/GI/SSAO.

### REN-609 – Skinned-Draws laden pro Draw immer 128 Bone-Matrizen (8 KB) und füllen vorher den Scratch neu mit Identität
- **ID**: REN-609
- **Kategorie**: C/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12576-12594`
- **Problem**: Pro Skinned-DrawCall: `std::fill` von 128 `mat4` (8 KB), `copy_n` der echten Matrizen und `glUniformMatrix4fv(m_uSkinnedBones, 128, …)`. Ein typischer Charakter hat 20–80 Joints; bei 50 Charakteren sind das 400 KB Uniform-Upload und 400 KB Fill pro Frame. Skinned-Draws werden außerdem nie instanziert/gebatcht.
- **Code-Auszug**:
```cpp
std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
... std::copy_n(dc.boneMatrices.begin(), boneCount, boneScratch.begin());
glUniformMatrix4fv(m_uSkinnedBones, kMaxBones, GL_FALSE, glm::value_ptr(boneScratch[0]));
```
- **Lösung**: Nur `boneCount` Matrizen direkt aus `dc.boneMatrices` hochladen (`glUniformMatrix4fv(loc, boneCount, …, dc.boneMatrices.data())`), Identität einmalig beim Programmbau in die übrigen Slots schreiben (Uniform-Werte persistieren pro Programm) – dann ist weder Fill noch Scratch nötig. Mittelfristig: Bone-Matrizen in UBO/Textur.
- **Erwarteter Effekt**: (Schätzung) Upload-Volumen proportional zu echten Joints statt 8 KB; Wegfall des 8-KB-Fill pro Draw.
- **Risiko / Verifikation**: Slots ≥ `boneCount` enthalten dann Werte eines früheren Draws, falls ein Vertex Bone-IDs ≥ `boneCount` referenziert (der Scratch deckte das bisher mit Identität ab, Kommentar 12578). Vorher sicherstellen (Importer/Test), dass `max(boneID) < boneCount`; sonst Slots bis `maxBoneId+1` mit Identität auffüllen. Test: Skeletal-Animations-/Bone-Overlay-Tests.

### REN-610 – `UpdateGIAccel`: pro Frame `glm::inverse` je Objekt, Surface-Lookup je Objekt und komplette SSBO-Neuladung (Instanzen + Landscapes)
- **ID**: REN-610
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:5959-5983`, `5989-6020`, `6040-6043`
- **Problem**: Bei aktivem GI/GI-Reflections: für jedes Objekt jeden Frame `glm::inverse(obj.transform)` (volle 4×4-Inverse), `HE::giInstanceSurface(obj, m_contentManager)` (Material-Lookup), anschließend `glBufferData` des *gesamten* Instanz-SSBO, auch wenn sich nichts bewegt hat. Der Landscape-SSBO wird jeden Frame neu allokiert und befüllt (`std::vector<GILandGpu> lands` pro Frame, `ResolveGraphTexture` je Landscape → String-Key, siehe REN-607), obwohl sich Landscapes selten ändern.
- **Code-Auszug**:
```cpp
inst.invTransform = glm::inverse(obj.transform);
const HE::GiInstanceSurface surf = HE::giInstanceSurface(obj, m_contentManager);
...
glBufferData(GL_SHADER_STORAGE_BUFFER, m_giInstancesCpu.size() * sizeof(GIInstanceGpu), m_giInstancesCpu.data(), GL_DYNAMIC_DRAW);
```
- **Lösung**: (1) Affine Inverse (Rotation/Skalierung transponieren/invertieren) statt `glm::inverse`, oder Inverse nur neu berechnen, wenn sich `obj.transform` gegenüber dem Vorframe geändert hat (Cache `entityId → {transform, inv}`). (2) Surface-Daten pro `(meshId, materialId)` cachen. (3) Instanz-SSBO nur hochladen, wenn sich Hash der Instanzliste geändert hat; sonst nur geänderte Bereiche per `glBufferSubData`. (4) Landscape-SSBO nur bei Änderung der `landscapes`-Liste/Texturen.
- **Erwarteter Effekt**: (Schätzung) bei 5k Objekten ~0.3–1 ms CPU/Frame und der SSBO-Upload (5k × 112 B = 560 KB) in statischen Szenen eliminiert.
- **Risiko / Verifikation**: Cache-Invalidierung bei Material-/Mesh-Änderung. GI-Verhalten muss bytegleich bleiben → vorhandener `tests/test_gi_bvh.cpp` deckt nur Traversal; zusätzlicher Vergleichstest der Instanzliste (alt vs neu) empfohlen.

### REN-611 – BLAS-Aufbau synchron auf dem Renderthread, komplette Neu-Upload-Kaskade, keine Eviction
- **ID**: REN-611
- **Kategorie**: E/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:5911-5939` (`BuildGIBlas`), `5952-5958`, `6028-6039`, `10811-10817`
- **Problem**: Die erste Verwendung eines Meshes im GI-Modus baut den BVH komplett auf dem Renderthread (`HE::buildGiBvh`) – bei großen Meshes ein Frame-Hitch. Jede neu angehängte BLAS setzt `m_giBlasDirty` und `glBufferData` lädt anschließend *alle* Knoten und Dreiecke aller Meshes erneut hoch (O(Gesamtgröße) pro neu gesehenem Mesh). `m_giBlasCache` wird nie bereinigt, wenn Meshes entladen werden (nur bei `InvalidateMesh` wird *alles* gelöscht und alle BVHs werden neu gebaut, 10811-10817).
- **Code-Auszug**:
```cpp
if (m_giBlasCache.count(id)) { m_giBlasCache.clear(); m_giNodesCpu.clear(); m_giTrisCpu.clear(); m_giBlasDirty = true; }
```
- **Lösung**: BVH-Aufbau in den Job-System-Worker verlagern (vorhandenes `JobSystem`), bis zur Fertigstellung das Objekt aus der GI-Instanzliste ausnehmen (Cube-Fallback existiert bereits). SSBO mit `glBufferSubData` am Ende erweitern (Kapazitätsverdopplung) statt Neu-Upload. Pro Mesh invalidieren (`free list`/Kompaktierung) statt alles zu verwerfen.
- **Erwarteter Effekt**: (Schätzung) Hitch beim Laden GI-aktiver Szenen von Frames-Spitzen (10–100 ms bei Millionen Dreiecken) auf Hintergrundarbeit.
- **Risiko / Verifikation**: Asynchronität ändert die erste Sichtbarkeit (Schatten/Reflexionen erscheinen verzögert). Messen; Tests: `tests/test_gi_bvh.cpp` + Szene mit nachgeladenen Meshes.

### REN-612 – `glGetUniformLocation` mit String-Literal pro Frame in GI-, Partikel- und Graph-Pfaden
- **ID**: REN-612
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:6471-6476`, `6563-6635` (GI-Shadow, ≈14 Lookups), `6724-6786` (`loc`-Lambda ≈ 25 Lookups + 4 × `"uLandWeights[" + to_string(i) + "]"`), `6947-6970` (≈ 12), `13273-13276` (4 pro Partikel-Batch), `13213`; insgesamt 298 `glGetUniformLocation(..., "…")` in der Datei
- **Problem**: Der Programm-Locations-Cache existiert für Szene/Sky/Post (Member `m_u…`), aber nicht für die GI-Compute/Temporal-/Atrous-/Refl-Programme und die ParticleGraph-Programme: dort wird pro Frame (bzw. pro Emitter) per Namen nachgeschlagen (Treiber hasht den String; `std::string`-Konstruktion für die Landscape-Samplernamen).
- **Code-Auszug**:
```cpp
auto loc = [&](const char* n) { return glGetUniformLocation(m_giReflCSProgram, n); };
glUniform4f(loc("uCamPos"), ...); ... 
const std::string nm = "uLandWeights[" + std::to_string(i) + "]";
```
- **Lösung**: Je Programm eine kleine Struct (`GiReflLocs`, `GiShadowLocs`, `GiProbeLocs`, `GiTemporalLocs`, …) in `CreateGIPipelines()` einmal füllen (wie bereits `FetchGISceneLocs`); für Partikel-Programme die Locations im Programm-Cache (`m_particlePrograms`) mitführen (`struct {GLuint prog; GLint viewProj, camRight, camUp, hasTex;}`).
- **Erwarteter Effekt**: (Schätzung) ≈ 60 String-Lookups pro Frame bei aktivem GI entfallen (je 0,2–1 µs, treiberabhängig).
- **Risiko / Verifikation**: Rein mechanisch; Locations ändern sich nach Relink nicht. Kein Verhaltensunterschied.

### REN-613 – `DrawSkyFullscreen` setzt ≈ 60 Uniforms + `getenv` je Aufruf und wird für die Reflection-Cube 6× pro Frame aufgerufen
- **ID**: REN-613
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:10665-10775`, Aufrufer `6692-6701` (`RenderSkyReflCube`), `10649`, `9734`; `getenv` in `10711` und `5076`
- **Problem**: Bei aktivem GI-Reflection-Pfad rendert `RenderSkyReflCube` jedes Frame 6 Cube-Faces (je 128², volle Wolken-/Sky-Shader) und jede Face ruft `DrawSkyFullscreen` mit allen ≈ 60 `glUniform*` neu auf (360 Uniform-Calls). Nur `uInvViewProj` unterscheidet die Faces. Zusätzlich liest `DrawSkyFullscreen` (und `RenderCloudShadowMap`) jedes Frame `std::getenv("HE_SKY_TIME")` + `atof`. Der Bake hat keine Änderungserkennung (Sonne/Wolken/Kamera).
- **Code-Auszug**:
```cpp
float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
```
- **Lösung**: `DrawSkyFullscreen` in `SetSkyUniforms(env, sunDir, camPos, clock)` + `DrawSkyPass(invViewProj)` teilen; die Cube-Faces rufen `SetSkyUniforms` einmal und dann 6× nur `glUniformMatrix4fv`. `HE_SKY_TIME` einmal in einer `static const` auswerten (wie `s_timeOv` bei 5659/11608 bereits). Optional: Cube nur neu rendern, wenn sich Sonnenrichtung/Wolkenzeit/Kameraposition über Schwelle geändert haben.
- **Erwarteter Effekt**: (Schätzung) ≈ 300 Uniform-Calls + 6 `getenv` weniger pro Frame (nur bei GI-Reflections); Cube-Bake optional 1/N der Frames.
- **Risiko / Verifikation**: Nur die Teilung ist verhaltensgleich. Das Überspringen des Bakes ändert Latenz der Wolkenspiegelung → bewusst entscheiden (Visueller Vergleich).

### REN-614 – Totes deaktiviertes Reflection-Temporal (`if (false && …)`) hält Shader, Programm und 4 RGBA16F-Texturen vor
- **ID**: REN-614
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:6803-6837`, `6296-6313`, `2728-2779` (`kGiReflTemporalFS`), `6163-6164`, `6838`
- **Problem**: Der Temporal-Zweig ist mit `if (false && …)` und `const float blend = 0.0f; (void)camMoved;` deaktiviert („one constant to undo“). Trotzdem werden `kGiReflTemporalFS` bei jedem GI-Start kompiliert/gelinkt, `m_giReflHistTex[2]` + `m_giReflHistPosTex[2]` (4 × RGBA16F im Prepass-Format, halbe Auflösung) + 2 MRT-FBOs alloziert und per Clear initialisiert, und `m_giReflPrevViewProj` wird pro Frame geschrieben. Der Code enthält außerdem eine nie erreichte `camMoved`-Berechnung.
- **Code-Auszug**:
```cpp
if (false && m_giReflQuality >= 2 && m_giReflTemporalProgram && m_giReflHistFBO[0]) { ...
    const bool camMoved = (viewProj != m_giReflPrevViewProj);
    const float blend = 0.0f;
    (void)camMoved;
```
- **Lösung**: Entweder entfernen (Programm, Shader-String, History-Texturen, FBOs, `m_giReflHistIdx/Valid/PrevViewProj`, Zweig) und den Rückbau-Hinweis in `docs/gi-reflections-plan.md` verlinken; oder hinter eine `constexpr bool kGiReflTemporal = false;` stellen und Ressourcen nur dann anlegen. 
- **Erwarteter Effekt**: ≈ 4 × (w/2 × h/2 × 8 B) VRAM gespart (bei 1080p ≈ 16 MB), ≈ 60 Zeilen Code und 1 Shader-Kompilierung weniger.
- **Risiko / Verifikation**: Keine Bildänderung (Zweig nie aktiv). Kompilieren + GI-Reflection-Headless-Dump vorher/nachher.

### REN-615 – Wetter-Partikel: Simulation und Draw laufen immer über den vollen Pool, auch wenn nur `coverage` × Pool aktiv ist
- **ID**: REN-615
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12969-12995` (`alive = step(iSeed, coverage)`), `13147-13181` (`SimulateGpuParticles`), `13183-13206` (`DrawGpuParticles`, `glDrawArraysInstanced(…, count)`); Seed: `WeatherParticleSeed.h` (`seed = (i + 0.5f) / count`)
- **Problem**: Slots mit `seed > coverage` werden nur „geparkt“ (`pos.y = -100000`, `life = -1`), aber trotzdem jedes Frame per Transform Feedback simuliert (`count` Punkte) und als `count` Instanzen × 4 Vertices gezeichnet; der Vertex-Shader verwirft sie erst (`gl_Position = vec4(2,…)`). Bei Pool bis 1 000 000 und leichtem Regen (coverage 0.1) laufen 90 % der Arbeit für nichts. Da `seed` monoton in `i` steigt, sind aktive Slots exakt die ersten `ceil(coverage × count)`.
- **Code-Auszug**:
```glsl
float alive = step(iSeed, coverage);
...
else { life = -1.0; pos = camPos + vec3(0.0, -100000.0, 0.0); vel = vec3(0.0); }
```
- **Lösung**: `activeCount = min(count, ceil(coverage * count))` berechnen; `glDrawArrays(GL_POINTS, 0, activeCount)` in der Simulation und `glDrawArraysInstanced(…, activeCount)` im Draw verwenden. Beim Erhöhen von `coverage` müssen neu aktivierte Slots entweder ihr Parken-Reset behalten (Slot wurde beim letzten Aktivsein in unbekanntem Zustand eingefroren) → beim Wechsel der `activeCount`-Obergrenze einmal `life <= 0` für die Differenz setzen (Respawn) oder Pool nur neu seeden.
- **Erwarteter Effekt**: (Schätzung) bei coverage 0.1–0.3: 70–90 % weniger Vertex-Arbeit im Wetter-Pass.
- **Risiko / Verifikation**: Verhalten bleibt gleich, solange eingefrorene Slots beim Reaktivieren respawnen. Das Verhalten beim Hochfahren der Dichte (Plop-Effekt) muss verglichen werden; Metal-Zwilling `HE::SeedWeatherParticles`-Layout identisch halten. Prüfen: `GpuParticleParams.count` enthält coverage evtl. schon (siehe „Unsicher“).

### REN-616 – Sky-Environment-Cube: 1,5-MB-Vector pro Neubau, 6×128²×RGBA32F, Thread-Pool-Stall auf dem Renderthread
- **ID**: REN-616
- **Kategorie**: C/E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:64-81` (`BuildSkyEnvCube`), `3437`, `4226-4244` (`UpdateSkyEnvCube`), `3546-3558`
- **Problem**: Bei jeder Sonnenbewegung > 2e-3 (unter Tag/Nacht-Automatik ≈ jedes zweite Frame) wird auf dem Renderthread ein neuer `std::vector<float>` mit 6 × 128 × 128 × 4 Floats (1,5 MB) alloziert, per `parallel_for` befüllt (laut Kommentar „a few ms“, der Renderthread wartet auf den Pool) und als RGBA32F-Cube hochgeladen. IBL-Ambient ist niederfrequent (Kommentar 4228-4231), 128² RGBA32F (6 × 256 KB = 1,5 MB Upload) ist dafür deutlich überdimensioniert.
- **Code-Auszug**:
```cpp
const std::vector<float> px = BuildSkyEnvCube(kSkyEnvFace, sunDir);
for (int f = 0; f < 6; ++f) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, kSkyEnvFace, kSkyEnvFace, GL_RGBA, GL_FLOAT, px.data() + f * faceFloats);
```
- **Lösung**: (1) Puffer als Member wiederverwenden (kein Alloc). (2) Bake asynchron als Job starten und im nächsten Frame hochladen (1 Frame Latenz bei langsamer Sonnenbewegung unsichtbar). (3) RGBA16F statt RGBA32F (halbe Bandbreite) und/oder 64² (4× weniger Texel); Auswirkung auf Ambient-Qualität vergleichen. Metal bäckt pro Face on-demand (Zeile 61-63), GL könnte pro Frame nur 1–2 Faces rotierend aktualisieren.
- **Erwarteter Effekt**: (Schätzung) 1,5 MB Alloc + Wartezeit des Renderthreads (ms) pro zweitem Frame entfallen.
- **Risiko / Verifikation**: Auflösung/Format ändern minimal das Ambient; für exakte Bildgleichheit nur (1) und (2). Vergleich per Headless-Dump bei mehreren Sonnenständen.

### REN-617 – Mehrere unabhängige O(N)-Durchläufe über alle Objekte pro Frame
- **ID**: REN-617
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:10918-10920` (`ResolveMesh` + `refineWorldBounds` für jedes Objekt), `5033` (Szenen-AABB nur für Cloud-Shadows), `6366-6371` (`GIProbeSceneSignature` / Refine), `5959-5981` (GI-Instanzen), `4747-4788` (Velocity: `unordered_map`-Einfügen pro Entity und Frame)
- **Problem**: Pro Frame läuft mindestens ein Durchlauf über `m_renderWorld.objects` für: Bounds-Refinement (Hash-Lookup `m_meshCache` je Objekt), Cloud-Shadow-Szenenbox (wenn Cloud-Shadows an), GI-Signatur (wenn GI an), GI-Instanzen (wenn GI an), TAA-Velocity-History (`m_taaCurTransforms[obj.entityId] = …` ⇒ Node-Allokation pro Entity/Frame, `swap` mit Vorframe). Die Refinement-Schleife kann außerdem das *synchrone Hochladen neuer Meshes* auslösen (REN-625).
- **Code-Auszug**:
```cpp
for (RenderObject& obj : m_renderWorld.objects)
    if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId)) obj.refineWorldBounds(mesh->localBounds);
...
for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
m_taaCurTransforms[obj.entityId] = obj.transform;
```
- **Lösung**: `localBounds` einmal pro Mesh-Asset in `RenderObject`/Extractor beim Erzeugen setzen (Extractor kennt `ContentManager`) und die Refine-Schleife auf neu geladene Meshes beschränken; Szenenbox einmal pro Frame im Extractor berechnen und von Cloud-Shadows/GI-Grid wiederverwenden; Velocity-History als `std::vector<glm::mat4>` indiziert über einen stabilen Slot (Entity-Index) oder ein flat-hash-Map (keine Node-Allokation).
- **Erwarteter Effekt**: (Schätzung) bei 10k Objekten 3–5 Hash-Lookups/Objekt/Frame und 10k Node-Allokationen (TAA) entfallen.
- **Risiko / Verifikation**: `refineWorldBounds` hängt von der Auflösung des GPU-Meshs ab (Kommentar 6358); Extractor kennt die Bounds evtl. erst nach Upload → Refine bei Mesh-Auflösung mit Version/Flag. Test: Culling-Tests, TAA-Velocity bei Objekt-Spawn.

### REN-618 – Graph-Material-Draws binden pro Draw Frame-konstante Texturen neu und laden Objekt-UBO pro Instanz
- **ID**: REN-618
- **Kategorie**: B/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:12127-12201`, `12204-12221`, `11879-11934`, `12801-12809`
- **Problem**: Im Material-Zweig werden pro Draw die Units 14 (Sky-Cube), 15 (AO), 16/17 (DDGI) neu gebunden (`glActiveTexture`/`glBindTexture` ×8), obwohl sie pro Frame konstant sind und „only ever used by these material programs“ (Kommentar 12176-12179). Außerdem Unit 13 (Weightmap) per Draw mit String-Lookup (REN-607), `glBindBufferBase` ×3 und – bei Instanz-Batches – pro Instanz ein eigener `glBufferSubData` des 176-B-Objekt-UBO + ein `glDrawElements` (die Instanzierung wird für Graph-Materialien bewusst ausgelassen, 12081).
- **Code-Auszug**:
```cpp
glActiveTexture(GL_TEXTURE14); glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
glActiveTexture(GL_TEXTURE15); glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
...
glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj); glDrawElements(...);   // pro Instanz
```
- **Lösung**: Units 14–17 (und die Lighting-/Param-`glBindBufferBase`) einmal vor der Draw-Schleife binden (und nach der Transparenz-Sammlung nur wenn eine andere Stelle sie überschreibt – laut Kommentar verwenden nur Material-Programme diese Units). Für Instanz-Läufe eines Graph-Materials den UBO als Array/SSBO mit `gl_InstanceID`-Variante der Standard-Vertexshader (`standardVertex`) anbieten – oder wenigstens alle Instanzen eines Batches in ein UBO-Array hochladen und mit `glDrawElementsInstanced` zeichnen (WPO/Wind braucht pro Instanz das `model`).
- **Erwarteter Effekt**: (Schätzung) ≈ 10 GL-Calls pro Material-Draw entfallen; Instanz-Läufe (Foliage mit Wind-Material) von N Draws auf 1.
- **Risiko / Verifikation**: Reihenfolge: Frame-Binds müssen nach `RenderUIPass`/Previews (die Units 5 benutzen) erneuert werden. Instanzierung erfordert Shader-Variante und Metal-Parität; zuerst nur die Bind-Hoisting-Maßnahme.

### REN-619 – GLSL-Zwillinge (DDGI-Sampling, Cone-Hash, Oct-Encode, SpecAA, Temporal-Kernel) mehrfach im selben File
- **ID**: REN-619
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:195-200`/`712-717`/`2268-2273`/`2462-2467` (octEncode ×4), `208-264` vs `2469-2525` (`sampleDDGIIrradiance`), `2018-2039` vs `2535-2556` (`giPcg3d/giHash2/giConeSample`), `150-162` vs `722-732` (`heSpecAARoughness`), `2103-2175` vs `2728-2779` (Temporal)
- **Problem**: Mehrfache wortgleiche GLSL-Funktionen, die nur über „SYNC“-Kommentare zusammengehalten werden (z. B. 2458-2461, 2527-2528, 148-149). Zusätzlich existieren Spiegel in `MetalRenderer.mm`, `D3D_Shared/HlslSources.h` und `MaterialShaderLibrary.cpp` (Preamble `heSpecAARoughness`). Der Code zeigt selbst, dass Drift die reale Fehlerquelle ist (Kommentar 2533-2534: gelöster Hash-Bug musste in jeder Kopie nachgezogen werden).
- **Code-Auszug**:
```cpp
// SYNC: byte-for-byte the scene shader's giOctEncode / sampleDDGIIrradiance (kUnlitFS above)
```
- **Lösung**: Wie bei `kSkyFuncGLSL` / `injectSkyFunc` gemeinsame Snippet-Strings (`kGiOctGLSL`, `kDdgiSampleGLSL`, `kGiConeHashGLSL`, `kSpecAAGLSL`) in einem Header (`HorizonRendering/*ShaderSource.h`) halten und per Marker/String-Konkatenation in `kUnlitFS`, `kGBufFS`, `kGiReflCS`, `kGiShadowCS` einsetzen (GLSL hat kein `#include`, `injectSkyFunc` ist das vorhandene Muster). Für Cross-Backend (Vulkan/D3D über `kSkyFS`) dasselbe Snippet-Header nutzen.
- **Erwarteter Effekt**: ≈ 150 Zeilen GLSL weniger, Wegfall der „SYNC“-Pflege im GL-File; keine Laufzeitänderung.
- **Risiko / Verifikation**: `scripts/validate_embedded_shaders.py` (Kommentar 3288-3290) muss die zusammengesetzten Texte weiterhin bauen; Shader-Compile auf GL 4.10 (Mac) und 4.30 prüfen.

### REN-620 – 38 × `glCreateProgram…glLinkProgram`-Boilerplate in 8+ Varianten, teilweise ohne Fehlerprüfung
- **ID**: REN-620
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3465-3473`, `3893-3898`, `3964-3971`, `4250-4256`, `4329-4336`, `4426-4433`, `4451-4458`, `4473-4480`, `4495-4502`, `4519-4526`, `4591-4612`, `4846-4871` (Bloom: **kein** Link-Check), `5141-5160`, `5319-5338`, `5592-5599`, `5700-5781`, `6124-6155`, `7010-7028`, `7448-7472`, `8172-8188`, `8337-8355`, `9099-9105`, `9397-9422`, `9873-9879`, `10112-10126` (Debug-Lines: **weder Compile- noch Link-Check**), `13071-13109`
- **Problem**: Jede Pipeline-Funktion definiert ihr eigenes `link`-Lambda oder wiederholt die 8 Zeilen (create/attach/link/delete shaders/status/log). Fehlerbehandlung ist uneinheitlich: einige werfen (`throw std::runtime_error`), andere loggen und geben 0 zurück, Bloom (4846-4871) und Debug-Lines (10112-10126) prüfen gar nicht (Fehlschlag ⇒ stiller schwarzer Pass). Unterschiedliche `#version` (410 vs 330 beim Debug-Line-Shader).
- **Code-Auszug**:
```cpp
GLuint vs = CompileStage(GL_VERTEX_SHADER, kTonemapVS);
GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBrightFS);
m_bloomBrightProgram = glCreateProgram();
glAttachShader(...); glLinkProgram(m_bloomBrightProgram);   // kein GL_LINK_STATUS
```
- **Lösung**: Ein Datei-weiter Helfer `static GLuint LinkProgram(std::initializer_list<GLuint> stages, const char* what, bool throwOnFail)` (löscht Shader, prüft Status, loggt via `HE_LOG_ERROR(RHI, …)`), plus `LinkFullscreen(const char* fsSrc, const char* what)` für alle Programme mit `kTonemapVS`. Die lokalen Lambdas (4591, 5141, 5319, 6124, 7448, 8172) ersetzen.
- **Erwarteter Effekt**: ≈ 400 Zeilen weniger; einheitliche Fehlerbehandlung; Bloom/Debug-Line-Fehler werden sichtbar.
- **Risiko / Verifikation**: Wirf-/Log-Verhalten pro Aufrufer erhalten (Pflicht-Programme werfen, optionale geben 0). Initialisierung headless starten (`HE_HIDDEN_WINDOW=1`).

### REN-621 – Render-Target-/Textur-Anlage 17× handkopiert (156 × `glTexParameteri`, 34 × `glGenFramebuffers`)
- **ID**: REN-621
- **Kategorie**: A/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `4560-4576`, `4664-4679`, `4899-4911`, `4935-4944`, `4967-4977`, `5204-5219`, `5369-5384`, `5627-5636`, `7128-7140`, `7158-7172`, `7526-7537`, `7597-7609`, `8044-8076`, `8095-8107`, `9158-9178`, `9329-9348`, `9455-9475`, `9680-9714`, `10042-10061`, `10537-10550`
- **Problem**: Gleiches Muster `glGenFramebuffers → glGenTextures → glTexImage2D → 4 × glTexParameteri → glFramebufferTexture2D → Completeness-Check` mit marginal unterschiedlichen Parametern (Format, Filter, Wrap, Depth-RBO oder Textur). Preview-Ziele (Material, Skeletal, Particle, Thumbnail, World) sind fünfmal identisch (FBO + RGBA8 + D24-Renderbuffer, Delete+Recreate bei Größenwechsel). Auch die Destroy-Funktionen (`DestroyBloomTargets`, `DestroyDepthOfFieldTargets`, `DestroyGIShadowTargets`, …) sind Listen manuell geführter `if (x) glDelete…`.
- **Code-Auszug**:
```cpp
glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, type, nullptr);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); ... (4×)
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
```
- **Lösung**: Kleine RAII-Struct `GlTarget { GLuint fbo, tex, depthRbo; int w, h; bool ensure(w, h, TargetDesc); void destroy(); }` mit `TargetDesc{ internalFmt, fmt, type, filter, wrap, withDepth }`; alle `Ensure*Target`-Funktionen darauf abbilden. `glTexStorage2D` (4.2) wird bereits bei GI genutzt, für 4.1 weiter `glTexImage2D`.
- **Erwarteter Effekt**: ≈ 500–700 Zeilen weniger; Leaks bei vergessenen Deletes (siehe REN-624) strukturell ausgeschlossen.
- **Risiko / Verifikation**: Pro Target identische Parameter beibehalten (Filter NEAREST/LINEAR, Border, sRGB). Headless-Dump jeder Pass-Kette vergleichen; Preview-Panel-UI-Tests.

### REN-622 – Uniform-Location-Listen für Unlit/Skinned/Instanced dreifach geführt (≈ 140 Member, 3 × 45 Zeilen)
- **ID**: REN-622
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3527-3582`, `4125-4164`, `4183-4222`; `BindSceneLighting`-Aufrufe `11501-11505`, `11545-11551`, `12547-12553`; Per-Frame-Pushes `11425-11498`, `11526-11541`, `12541-12574`
- **Problem**: Die drei Programme teilen `kUnlitFS` (Kommentar 10589-10596), aber für jedes gibt es einen eigenen Satz Member (`m_uXxx`, `m_uSkinnedXxx`, `m_uInstXxx`), eine eigene `glGetUniformLocation`-Liste und einen eigenen Per-Frame-Pushblock mit Brace-Init-Listen aus 18 Locations. Neue Uniforms müssen an 6+ Stellen ergänzt werden (Drift bereits sichtbar: Skinned setzt `uWetness/uSnow/uSpecAA` nicht, Instanced schon).
- **Code-Auszug**:
```cpp
BindSceneLighting({ m_uInstLightCount, m_uInstLightPos, ..., m_uInstClusterParams, m_uInstClusterCamFwd }, shadowFrame, frameClusters);
```
- **Lösung**: `struct SceneProgramLocs { GLint mvp, model, color, ..., giLocs; SceneLightingLocs lighting; };` und `FetchSceneProgramLocs(prog)` (gleicher Locations-Namensliste für alle drei, `-1` bleibt No-Op); `PushSceneFrameUniforms(const SceneProgramLocs&, const FrameState&)` ersetzt die drei Per-Frame-Blöcke.
- **Erwarteter Effekt**: ≈ 150 Zeilen weniger, ein Ort für neue Uniforms.
- **Risiko / Verifikation**: Skinned fehlte `uWetness/uSnow/uSpecAA` bisher — Vereinheitlichung ändert das Aussehen von Skinned-Meshes bei Nässe/Schnee (vermutlich Bugfix, aber sichtbar). Entweder bewusst übernehmen oder Locations für Skinned absichtlich auslassen; Test mit Wetter-Szene.

### REN-623 – Vier Kopien derselben Draw-Call-Schleife (Mesh-Resolve + Instanz/Einzel-Zweig + Section-Range)
- **ID**: REN-623
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:6479-6541` (GI-Prepass), `7295-7377` (SSAO/Refl-Prepass), `11035-11079` (Schatten), `11720-11950` (G-Buffer), `11956-12239` (Forward), `4747-4782` (Velocity)
- **Problem**: Jede Schleife wiederholt: `ResolveMesh` mit „last id“-Memo, Fallback auf `kDefaultCubeMeshId`, `DrawIndexRange`, Instanzen-Zweig mit `glBufferData` + Programmwechsel-Tracking (`boundProgram`), Einzel-Zweig mit `pushDraw` und `static bool loggedOnce`. Änderungen am Instanz-Pfad (z. B. REN-608) müssen an 5 Stellen erfolgen.
- **Code-Auszug**:
```cpp
if (!valid || dc.meshAssetId != lastId) { cMesh = ResolveMesh(dc.meshAssetId); lastId = dc.meshAssetId; valid = true; }
const GpuMesh* mesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
```
- **Lösung**: `template<class OnInstanced, class OnSingle> void ForEachGeometryDraw(const CommandBuffer&, Filter, OnInstanced, OnSingle)` bzw. kleine Klasse `DrawIterator`, die Mesh-Memo, Fallback-Cube, `DrawIndexRange` und die Instanz-Aufteilung kapselt; Pässe liefern nur ihre Programm-/Uniform-Logik.
- **Erwarteter Effekt**: ≈ 150 Zeilen weniger; ein Ort für Instanz-Upload-Optimierung (REN-608) und Material-Memo (REN-602).
- **Risiko / Verifikation**: Templates im Hot Path inlinen; per Bildvergleich je Pass (GI, SSAO, Schatten) absichern.

### REN-624 – `Shutdown()` lässt Programme/Targets aus und setzt „Tried“-Flags nicht zurück (stale Handles bei Re-Initialize)
- **ID**: REN-624
- **Kategorie**: C/F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:10220-10388`; fehlend gegenüber `5584-5643`, `6118-6189`, `9392-9435`, `9437-9592`, `9869-9905`, `10032-10089`
- **Problem**: `Shutdown()` löscht nicht: `m_ucProgram` (Underwater-Caustics-Programm; `m_ucFBO/m_ucColor` werden nur in `EnsureUnderwaterCausticsTarget` beim Resize gelöscht), `m_skelPreviewProgram`, `m_skelPreviewLineProgram`, `m_skelPreviewLineVAO/VBO`, `m_skelPreviewFBO/Color/Depth`, `m_particlePreviewProgram/VAO/InstVBO/FBO/Color/Depth`, `m_giReflCSProgram`, `m_giReflTemporalProgram`, `m_giReflBlurProgram`, `m_giReflMixProgram` (diese vier nur im Fehlerzweig von `CreateGIPipelines`). Der Kommentar bei 10323-10324 („their programs are the skeletal preview's, freed with those“) trifft nicht zu – dort wird nichts freigegeben. Die Flags `m_ucTried` und `m_giPipelinesBuilt` werden nicht zurückgesetzt, obwohl derselbe Block andere `…Tried`-Flags explizit zurücksetzt (10266-10270, „re-Initialize() gets a fresh build attempt“). Nach Shutdown→Initialize liefert `EnsureSkelPreviewPrograms()` die alte (tote) Programm-ID als „vorhanden“, `CreateGIPipelines()` baut nie neu (`m_giPipelinesBuilt == true`, GI bleibt still aus).
- **Code-Auszug**:
```cpp
if (m_skelPreviewProgram && m_skelPreviewLineProgram) return true;   // 9394, auch nach Shutdown wahr
if (m_giPipelinesBuilt) return;                                       // 6120, nie zurückgesetzt
```
- **Lösung**: Fehlende `glDelete*` ergänzen und Handles/Flags nullen (`m_ucTried = false; m_giPipelinesBuilt = false;`). Strukturell: Mit REN-621 (`GlTarget`) + einem `ProgramHandle`-RAII-Typ (Destruktor löscht) wird `Shutdown()` kürzer und vollständig.
- **Erwarteter Effekt**: Keine Leaks beim Context-Wechsel/Re-Init, keine stale IDs.
- **Risiko / Verifikation**: Nur relevant, wenn `Initialize` nach `Shutdown` erneut aufgerufen wird (Kommentar 10266-10270 sagt ja). Test: Renderer-Re-Init (Backend-Wechsel im Editor), danach GI/Skelett-Preview/Underwater aktivieren.

### REN-625 – Mesh-/Textur-Upload synchron beim ersten Draw (Refine-Schleife, Draw-Schleife, UI) ohne Budget oder PBO
- **ID**: REN-625
- **Kategorie**: E/D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:8566-8670` (`ResolveMesh`), `8673-8783`, `8446-8514` (`uploadTextureAssetGL`), `8523-8561`, `10918-10920`
- **Problem**: Der erste Zugriff auf ein Mesh/eine Textur lädt im selben Frame alles hoch: Interleave-Build (`vector::insert(initializer_list)` je Vertex, 8596-8609), `glBufferData`, Textur-Upload mit allen Mips bzw. `glGenerateMipmap` für Loose-Assets (8508) – ohne Frame-Budget, ohne asynchrones PBO. Beim Laden einer Szene werden alle Meshes in der Refine-Schleife (10918) im ersten Frame hochgeladen ⇒ Frame-Spike. 4K-RGBA8-Texturen sind 64 MB je Upload.
- **Code-Auszug**:
```cpp
built.insert(built.end(), { asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
...
glTexImage2D(GL_TEXTURE_2D, 0, rgbaFmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
glGenerateMipmap(GL_TEXTURE_2D);
```
- **Lösung**: Upload-Queue mit Zeit-/Byte-Budget pro Frame (z. B. 2 ms), bis dahin Platzhalter (Default-Cube / Weiß); Interleaving mit `resize` + direkter Indizierung statt `insert`; für große Texturen PBO + `glTexSubImage2D` pro Mip. Cooked-Assets laden ohnehin direkt (ein Memcpy-Pfad).
- **Erwarteter Effekt**: (Schätzung) Szenen-Load-Spike von 100+ ms auf mehrere Frames verteilt; Interleave-Build ≈ 2–3× schneller.
- **Risiko / Verifikation**: Pop-in sichtbar (Platzhalter). Messen (siehe unten). Editor-Thumbnail-/Preview-Pfade rufen `ResolveMesh` direkt auf und erwarten sofortiges Ergebnis → dort Budget umgehen.

### REN-626 – `GetOrBuildMaterialProgram` / `setupProgram` hinterlassen `glUseProgram(0)` mitten in der Draw-Schleife
- **ID**: REN-626
- **Kategorie**: B/F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `OpenGLRenderer.cpp:3822-3873` (`setupProgram`), `4070-4090`, `5785-5787`, `12009-12017`, `12226-12235`, `4047-4100` + `7978`
- **Problem**: `setupProgram` bindet die neue Programm-ID (`glUseProgram(prog)`) um die Sampler-Units zu setzen und endet mit `glUseProgram(0)`. Wird ein Programm erstmals *mitten* in der Opaque-Schleife gebaut (z. B. im Transluzent-Zweig, 12017, oder im UI-Pass), ist danach kein Programm gebunden; der folgende Built-in-Einzeldraw (12226-12235) setzt Uniforms und zeichnet ohne erneutes `glUseProgram(m_unlitProgram)` (GL_INVALID_OPERATION/kein Draw für diesen einen Draw). Analog im UI-Pass: nach dem Bauen eines (fehlgeschlagenen) Material-Programms folgt der Solid-Pfad, dessen `basicBound`-Flag noch `true` ist (7976).
- **Code-Auszug**:
```cpp
glUseProgram(prog);
if (GLint l = glGetUniformLocation(prog, "heTex0"); l >= 0) glUniform1i(l, 0);
...
glUseProgram(0);
```
- **Lösung**: `glProgramUniform1i(prog, loc, unit)` (GL 4.1, `ARB_separate_shader_objects`, in Core 4.1 enthalten) statt `glUseProgram`+`glUniform1i` – lässt den aktuellen Programmzustand unberührt. Alternativ aktuelles Programm per `glGetIntegerv(GL_CURRENT_PROGRAM)` sichern/restaurieren.
- **Erwarteter Effekt**: Verhindert einen einmaligen Draw-Aussetzer beim ersten Material-Build; macht Aufrufstellen robust (weniger implizite „Programm muss neu gebunden werden“-Regeln).
- **Risiko / Verifikation**: Praktisch nur sichtbar, wenn `WarmupMaterials` das Programm nicht schon gebaut hat (unsicher, ob die Situation in Releases auftritt). `glProgramUniform*` auf macOS 4.1 verfügbar. Test: Szene mit transluzentem Graph-Material ohne Warmup, erster Frame.

### REN-627 – Startup: `kUnlitFS` wird 3× kompiliert, Built-in-Programme ohne Binary-Cache, Cache-I/O auf dem Renderthread
- **ID**: REN-627
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:3463-3512` (`LinkSceneProgram`, aufgerufen in `3516`, `4111`, `4169`), `3615-3688` (Cache), `3876-3917`
- **Problem**: Scene/Skinned/Instanced kompilieren und linken denselben großen Fragment-Shader (`injectSkyFunc(kUnlitFS)` + ggf. `SceneStageSource`) je einmal (3 volle Compiles), obwohl ein Shader-Objekt an mehrere Programme angehängt werden kann. Der vorhandene On-Disk-Programm-Cache (`glTryLoadCachedProgram`) wird nur für Graph-Materialien benutzt, nicht für Built-in-Programme (Scene, Sky, SSAO, GI, SMAA …). `glSaveCachedProgram` (Datei schreiben) und `glTryLoadCachedProgram` (Datei lesen) laufen synchron beim ersten Draw.
- **Code-Auszug**:
```cpp
GLuint vs = CompileStage(GL_VERTEX_SHADER,   vsSrc);
GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());   // je Programm erneut
```
- **Lösung**: Fragment-Shader einmal kompilieren (Member-lokal) und an alle drei Programme attachen; `LinkSceneProgram`-Varianten (clustered/plain) analog. Built-in-Programme über dieselbe Cache-Funktion laufen lassen (Schlüssel: Quellen-Hash).
- **Erwarteter Effekt**: (Schätzung) Startzeit: 2 große FS-Compiles weniger (je 20–200 ms treiberabhängig); beim zweiten Start deutlich weniger Link-Zeit.
- **Risiko / Verifikation**: Shader-Objekt-Sharing ist GL-Standard. Cache-Invalidierung existiert (Device-Signatur + Quell-Hash). Startzeit messen.

### REN-628 – Shader-/Pass-Mikro-Optimierungen (niedrige Priorität, erst messen)
- **ID**: REN-628
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `OpenGLRenderer.cpp:334`, `415` (`tan(acos(ndl))`), `2158-2172` (Temporal 25 Taps + 9 Mittelwerte), `1476-1495` + `5123-5134` (Bloom: 10 Blur-Passes à 9 Taps), `5474-5503` (Caustics: `radians/cos/sin` je Pixel und Train), `1337-1338`
- **Problem**: (a) `tan(acos(ndl))` pro Pixel/Licht/Cascade ist `sqrt(1−ndl²)/ndl`; (b) `kGiTemporalFS` berechnet 9 überlappende 3×3-Mittelwerte aus 25 Taps einzeln (81 Additionen) statt separabel; (c) Bloom blurt 5 × (H+V) mit 9 Taps auf Halb-Res, ohne die Bilinear-Tap-Technik (5 statt 9 Taps) und ohne zusätzliche Downsample-Stufen; (d) Caustics: `radians(kTrains…)`, `cos`, `sin` pro Pixel und Train konstant über Frame → in Uniforms vorberechenbar.
- **Code-Auszug**:
```glsl
float bias = clamp(uShadowBias.x * tan(acos(ndl)), uShadowBias.y, 0.02) * float(c + 1);
```
- **Lösung**: (a) `sqrt(max(1.0 - ndl*ndl, 0.0)) / max(ndl, 1e-4)`; (b) Box-Mittel über Summed-Area (separabel); (c) Bilinear-Gauss 5-tap + Mip-Chain-Downsample; (d) pro Frame `vec2 dir[6]`, `k[6]` etc. als Uniform-Arrays.
- **Erwarteter Effekt**: Gering (GPU-seitig, nur messen, wenn der jeweilige Pass im GPU-Profil (`GpuPassScope`) auffällt).
- **Risiko / Verifikation**: (a) Grenzfall `ndl→0`: `tan(acos(0))` ist ~1.6e16, `clamp(…, , 0.02)` ergibt identisch 0.02 → bei der Ersetzung durch den `max(ndl, 1e-4)`-Nenner muss nach dem Clamp dasselbe herauskommen; (b)-(d) verändern Bildqualität leicht (Bloom). Messen vor Änderung.

### REN-629 – Kleinkram: State-Queries, Voll-Inverse für Kamera-Forward, Kopien in Debug-Linien/Previews, Shadowing
- **ID**: REN-629
- **Kategorie**: C/D/F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `10942-10943` (`glGetIntegerv(GL_FRAMEBUFFER_BINDING)` jedes Frame), `6684-6686` (3 State-Queries pro Frame in `RenderSkyReflCube`), `10966-10967` + `7630-7631` (`glm::inverse(view)[2]` nur für die Kamera-Forward-Richtung), `10091-10094` + `10144-10156` (`m_debugLines = lines` Kopie + Repack je Frame), `9505`/`9798` (8-KB-`boneScratch` je Skinned-Preview-Objekt), `9623-9646` (`RenderExtractor` + `RenderWorld` je Aufruf von `RenderWorldPreview`), `9184-9194` (`glReadPixels` synchron + doppelte Kopie je Thumbnail), `4054`/`4095` (Variablen `ok`/`log` verdecken äußere), `7693-7698` (4 × `glTexParameteri` je Frame auf den SSR-History-Texturen), `7589-7619` (Vollbild-Blit der HDR-Farbe jedes Frame für SSR-History)
- **Problem**: Je für sich geringfügig; zusammen vermeidbarer Dauer-Overhead und Lesbarkeitsmängel: Treiber-State-Queries (`glGetIntegerv`) können bei Threaded-Treibern synchronisieren; die Forward-Richtung ergibt sich aus der Transponierten der oberen 3×3 des View (`-vec3(view[0][2], view[1][2], view[2][2])`) ohne Inverse; Debug-Linien werden kopiert und neu gepackt, obwohl sich `std::vector<DebugLine>` mit `std::move`/`swap` übernehmen lässt.
- **Code-Auszug**:
```cpp
const glm::vec3 camFwd = -glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
void OpenGLRenderer::SetDebugLines(const std::vector<DebugLine>& lines) { m_debugLines = lines; }
```
- **Lösung**: Framebuffer-Bindung als Member (`m_currentTarget`) mitführen statt abzufragen; Forward aus View-Zeilen berechnen (`-normalize(vec3(view[0][2], view[1][2], view[2][2]))`, für Rotation+Translation identisch); `SetDebugLines(std::vector<DebugLine>&&)`/Swap und Debug-Vertexpuffer als Member mit `reserve`; Sampler-Objekte (`glGenSamplers`) für SSR-History statt `glTexParameteri` pro Frame; Shadowing beseitigen; Preview-Extractor als Member.
- **Erwarteter Effekt**: Klein; jede Maßnahme < 0,1 ms.
- **Risiko / Verifikation**: Forward aus View-Zeilen gleich der Inversen nur bei orthonormaler Rotation; bei nicht-uniformer Skalierung in der Kamera-Matrix (unüblich) Abweichung → `normalize` fängt Skalierung ab. Sonst risikolos.

---

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1000-1008` – `resolveFragment` kopiert die GLSL-Quelle und hasht sie bei jedem Aufruf; Hash gehört in `MaterialAsset` (`src/HE_Core/include/ContentManager/…`) und wird bei Material-Änderung neu berechnet. Betrifft alle Backends, die `resolveShaders` pro Draw rufen (D3D11/D3D12/Vulkan/Metal prüfen: `grep resolveShaders src/HE_Rendering/src/Backends/*`).
- `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm`, `src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h`, `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp` – dieselben Zwillinge (`heSpecAARoughness`, DDGI-Sampling, SMAA/FXAA/Bloom/DoF/MotionBlur-Kernel) wie in REN-619; ein gemeinsamer Snippet-Header würde Drift (z. B. der PCG-Hash-Fix, Kommentar 2533) an einer Stelle beheben.
- `RenderSorter::batchDepthCasters`, `m_culler.cull` (`src/HE_Rendering/…/RenderCuller*`, `RenderSorter*`) – die Schattenpass-Mehrfach-Cull-/Sort-Schleife (REN-605) sollte als Backend-neutrale Funktion (ein sortierter Caster-Pool, mehrere Frustum-Filter, Dirty-Cache) in der gemeinsamen Render-Schicht liegen, nicht je Backend.
- `src/HE_Rendering/include/HorizonRendering/LightPacking.h` – `BuildPackedLightArray`, `BuildMaskedLocalLights`, `dominantDirectionalLight` werden pro Frame mehrfach berechnet (GI-Shadow, GI-Refl, Probe-Update, Cloud-Shadow, UI-Pass, Caustics, `fillMatLight` bis zu 3× im Deferred-Pfad); einmal pro Frame in `RenderWorld` cachen.
- Skinned Meshes tauchen im GL-Schatten-, GI- und Velocity-Pass nicht auf (`UpdateGIAccel`-Kommentar 5948-5950, `renderDepthLayer` nutzt nur `m_renderWorld.objects`); Parität zu Metal/Vulkan/D3D prüfen (`grep skinned src/HE_Rendering/src/Backends/*`).
- `src/HE_Rendering/include/Backends/OpenGL/OpenGLRenderer.h` (≈ 1300 Zeilen Member): die in REN-620/621/622 vorgeschlagenen Helfer (`GlTarget`, `SceneProgramLocs`, Programm-RAII) würden die Member-Anzahl deutlich reduzieren.

## Unsicher / Messung nötig

- REN-605: tatsächlicher Anteil von Cull+Sort am Schattenpass (CPU) und GPU-Kosten der lokalen Ebenen — `GpuPassScope "Shadow"` und CPU-Zeit in `renderDepthLayer` messen, bevor Caching gebaut wird.
- REN-603/604: wie viele UI-Quads pro Frame im realen Spiel/Editor-Viewport vorkommen (nur Zähler `m_counters.draws` zeigt die UI nicht gesondert); Frosted-Anteil.
- REN-608: ob Orphaning von `m_instanceVBO` auf Ziel-Treibern (NVIDIA/AMD/Intel/Mesa/macOS) real kostet.
- REN-612: Kosten von `glGetUniformLocation` sind treiberabhängig (≈ 0,1–2 µs).
- REN-615: Ob `GpuParticleParams.count` bereits mit `coverage` skaliert wird (Aufrufer in `HE_Scene`/`HE_Rendering` prüfen, `SetGpuParticleParams`-Caller); falls ja, entfällt der Gewinn.
- REN-616: Tatsächliche Dauer des `BuildSkyEnvCube` in Release (Kommentar: „few ms“) und Auswirkung einer 64²/RGBA16F-Reduktion auf das Ambient.
- REN-618: Ob Wind-Materialien mit Instanz-Läufen in realen Szenen vorkommen (dann N Draws + N UBO-Uploads).
- REN-625: Größe der Spitzen beim Szenenladen (Zeit in `ResolveMesh`/`uploadTextureAssetGL`, vermutlich dominiert von Texturen).
- REN-626: Ob `WarmupMaterials` in allen Lade-Pfaden (Editor, Game, Hot-Reload) greift; falls ja, ist der Fund rein theoretisch.
- REN-628: GPU-Zeiten der Kandidaten-Passes aus dem Profiler (`GpuPassScope`) vor jeder Shader-Änderung.
- Ob `GLWireScope`/`Wireframe`-Pfade und Deferred-Pfad dieselben Memo-Strukturen brauchen (Deferred ist per `HE_HAVE_SHADERC` gated; Messung dort separat).

## Gelesen

Ja – `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp` (Zeilen 1–13589) vollständig in Blöcken von 700–1500 Zeilen gelesen; zusätzlich gezielt (Grep/Auszug) `MaterialShaderLibrary.cpp:935-1010`, `MaterialGraph.cpp:1981-1990`, `OpenGLRenderer.h` (Member-Typen der Caches) und `WeatherParticleSeed.h:25-80` zur Verifikation der Funde.
