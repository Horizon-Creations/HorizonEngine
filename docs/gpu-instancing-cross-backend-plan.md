# GPU Instancing: Cross-Backend-Stand und Portierungsplan

Stand 06.09.2026, Branch `claude/gpu-instancing-cross-backend`. Schritt 1 des
Themas: Kartierung des bestehenden Instancing-Pfads plus Portierungsplan.
**Kein Feature-Code in diesem Schritt.**

Bezug: `docs/gap-audit-2026-08-25.md` Zeile 60 (A3), `docs/backend-parity-plan.md`,
`docs/deferred-renderer-plan.md` (G-Buffer-Pass „opak + masked + skinned +
Landscape + Instancing"), Roadmap-Eintrag `gpu-instancing` (Website).

---

## 0. Der Befund vorweg: die Aufgabenbeschreibung ist verdreht

Das Brett und der Roadmap-Text sagen: *„GL+Metal haben echtes Instancing,
D3D11/D3D12/Vulkan zeichnen noch per Draw-Call-Schleife."*

Der Code sagt das Gegenteil. **A3 ist auf D3D11, D3D12 und Vulkan gebaut und
liegt auf `main`.** Alle drei haben einen echten Instance-Buffer, eine eigene
instanzierte Pipeline und genau *einen* Draw pro Batch:

| Backend | Instance-Speicher | Instanzierte Pipeline | Der eine Draw |
|---|---|---|---|
| D3D11 | `instanceSB` (dynamic StructuredBuffer, `Map(WRITE_DISCARD)`), SRV auf VS `t3` — `D3D11Renderer.cpp:952` | `vsInstanced` — `:951`, HLSL `:498` | `DrawIndexedInstanced` — `:4361` |
| D3D12 | `perInstanceRing[f]` (Upload-Heap, `instCursor`) — `:3179`, `:6691` | `psoInstanced` / `hdrPsoInstanced`, Root-SRV Slot 4 | `DrawIndexedInstanced` — `:7078` |
| Vulkan | `m_instanceBuf[m_currentFrame]` (host-visible, gemappt), Binding 1, `VK_VERTEX_INPUT_RATE_INSTANCE` — `VulkanRenderer.h:229`, `.cpp:1635` | `m_sceneInstancedPipeline` / `…HDR` — `VulkanRenderer.h:225` | `vkCmdDrawIndexed(…, count, …)` — `:4443` |

Das deckt sich mit `docs/gap-audit-2026-08-25.md:60`: „A3 ‚echtes
GPU-Instancing offen' → erledigt auf D3D11 und D3D12", und mit `:319`: „Was
inzwischen überall läuft: GPU-Instancing".

**Metal ist das Backend, das noch schleift.** Jeder Szenen-Draw in
`MetalRenderer::EncodeScene` (`MetalRenderer.mm:11971`) und
`MetalRenderer::EncodeGBuffer` (`:13760`) ist ein
`drawIndexedPrimitives:…indexCount:…` **ohne** `instanceCount:` — die Lambda
`drawInstance` wird pro Eintrag von `dc.instanceTransforms` einmal aufgerufen.
Zum Vergleich: der Partikelpfad derselben Datei benutzt sehr wohl
`instanceCount:` (`:9174`, `:14848`, `:14901`), die Szenengeometrie nicht.

Die Folgeschritte dieses Themas („Port nach D3D11/D3D12/Vulkan") zielen damit
auf fertige Arbeit. **Der offene Port ist Metal.** Details unten in §4.

---

## 1. Die gemeinsame Vorderseite: wo Batches entstehen

`GeometryPass::execute` in `src/HE_Rendering/src/RenderPass.cpp:20–83` ist die
einzige Stelle, an der Instanz-Batches gebildet werden. Sie läuft über die
gecullten + sortierten Indizes und verlängert einen Lauf, solange die
folgenden Felder gleich bleiben:

| Abbruchkriterium | Zeile | Warum |
|---|---|---|
| `meshAssetId`, `materialAssetId` | `:35` | offensichtlich |
| `paramOverride` nicht leer (bei einem der beiden) | `:39` | per-Entity-HeParams macht Objekte optisch verschieden |
| `instanceTint` | `:45` | Partikel-Farbe/Alpha-über-Lebenszeit ist eine *geteilte* Draw-Uniform |
| `receivesShadow` | `:48` | eine Uniform-Lane pro Draw |
| `weightmapTextureId` | `:51` | zwei Landscapes teilen Material, malen aber verschieden |

Ergebnis: ein `DrawCall` mit `instanceCount = runLen` und `instanceTransforms`
(`CommandBuffer.h:40`). Bei `runLen == 1` bleibt `instanceTransforms` **leer** —
das ist der Vertrag, an dem alle Backends den Fall unterscheiden.

Zwei Dinge, die diese Vorderseite *nicht* tut:

* **`ShadowPass::execute` batcht überhaupt nicht.** `RenderPass.cpp:132` setzt
  `dc.instanceCount = 1` pro Objekt, `instanceTransforms` bleibt leer. Kein
  Backend *kann* Schattenwürfe instanzieren, selbst wenn es wollte. Das ist eine
  Engine-Lücke, kein Backend-Befund (siehe §6).
* **Skinned-Objekte werden nie gebatcht** (`:96`, `:132`) — jedes trägt seine
  eigenen Bone-Matrizen. Korrekt so.

---

## 2. Wie GL es macht (das älteste Muster)

`OpenGLRenderer`, zwei Stellen in `DrawScene`:

* **Forward-Pfad** `OpenGLRenderer.cpp:10887–10912`
* **G-Buffer-Pfad** `:10674–10697`

Beide identisch aufgebaut:

1. `glBufferData(m_instanceVBO, …, GL_STREAM_DRAW)` — die kompletten
   `instanceTransforms` als `mat4`-Array, **pro Batch neu hochgeladen**
   (`:10891`, `:10678`);
2. `glUseProgram(m_instancedProgram)` bzw. `m_gbufferInstancedProgram`;
3. `uViewProj` + die batch-konstanten Uniforms (Farbe, metallic, roughness,
   hasTexture, noShadow);
4. `glDrawElementsInstanced(GL_TRIANGLES, indexCount, …, count)`;
5. Programm zurückstellen (`m_unlitProgram` / `m_gbufferProgram`).

Der Shader ist `kInstancedVS` (`:827–848`): Attribut-Locations **4–7** tragen
die vier Spalten der Modellmatrix, `uViewProj` ist Uniform. Die
`glVertexAttribDivisor`-Bindung hängt am VAO und wird einmal beim Mesh-Upload
gesetzt (`:7999–8002`) — deshalb reicht später ein `glBindVertexArray`.

**Der Haken, der schon heute existiert:** GL routet instanzierte Batches
*immer* auf das eingebaute Programm. Kommentar `:10676`: „Instanced batches
always take the built-in instanced program — the same routing the forward loop
uses." Das heißt: **ein Batch aus Meshes mit Node-Graph-Material verliert sein
Material**, sobald zwei davon nebeneinander sortiert werden. Bei `runLen == 1`
greift der Graph-Pfad, bei `runLen == 2` nicht mehr — ein Optikwechsel, der von
der Sortierreihenfolge abhängt. Nicht in diesem Thema zu fixen, aber in §6
notiert, weil ein Metal-Port dieselbe Einschränkung erben wird.

---

## 3. Wie A3 es macht (der Vertrag, den Metal erben soll)

Alle drei A3-Backends teilen dieselbe Form. Das ist der Vertrag, nicht drei
Zufälle:

**Layout.** 128 Byte pro Instanz = zwei `mat4`, `{mvp, model}`, spaltenweise.
In allen drei per `static_assert(k_instStride == 2 * sizeof(glm::mat4))`
festgenagelt (`D3D11:4334`, `D3D12:7048`, `Vulkan:4420`). `mvp` wird auf der
CPU vorgerechnet (`viewProj * t`) statt im Shader multipliziert — 64 Byte mehr
pro Instanz gegen eine Matrixmultiplikation pro Vertex.

**Kapazität.** `k_maxInstances = 65536` in allen dreien
(`D3D11:954`, `D3D12:73`, `VulkanRenderer.h:231`) → 8 MB Ring.

**Sub-Allokation.** D3D12 und Vulkan führen einen `instCursor`, der pro Frame
bei 0 startet und pro Batch weiterwandert (`D3D12:6691`, `Vulkan:4196`) — mehrere
Batches teilen sich einen Puffer, der Draw bekommt den Offset. D3D11 hat keinen
Cursor: es mappt `WRITE_DISCARD` und beginnt jedes Mal bei 0, weil dort ohnehin
pro Draw neu gemappt wird.

**Das Gate.** `bool allowInstancing` wird vor dem transparenten Pass auf
`false` gesetzt (`D3D11:4463`, `D3D12:7202`, `Vulkan:4479`). Transparente
Objekte müssen einzeln nach Tiefe sortiert werden, deshalb bleibt dort die
Schleife — **das ist kein Rückstand, das ist die richtige Antwort.**

**Der Rückfall.** `fits = allowInstancing && <Pipeline vorhanden> && <Ring
vorhanden> && instCursor + count <= k_maxInstances`. Ist `fits` falsch, läuft
exakt dieselbe Schleife wie vorher. Kein Sonderfall, kein Absturz bei vollem
Ring.

**Die HDR/LDR-Falle.** Vulkan und D3D12 halten *zwei* instanzierte Pipelines
(`m_sceneInstancedPipelineHDR`, `hdrPsoInstanced`) und wählen die, die zum
aktiven Szenen-Target passt. Der Kommentar bei `Vulkan:4424` sagt warum:
„never bind an LDR pipeline to the HDR render pass". Ist die passende Variante
null → `fits = false` → Schleife, statt falsch zu binden.

**Nach dem Draw wird zurückgestellt.** `VSSetShader(p.vs)` + t3 auf null
(D3D11), `SetPipelineState(scenePso)` (D3D12), `vkCmdBindPipeline(scenePipeline)`
(Vulkan). Der nächste, nicht instanzierte Draw findet den Zustand vor, den er
erwartet.

**Zähler.** Ein instanzierter Batch zählt als **1 Draw**, die Dreiecke werden
mit `count` multipliziert. Genau so misst man den Gewinn.

---

## 4. Checkpoint M — Metal (die eigentliche Arbeit)

### M1 · Was gebaut wird

Zwei neue Pipelines und **eine** neue MSL-Vertex-Funktion. Der Grund, warum
eine reicht: Metals eingebaute G-Buffer-Pipeline benutzt *dieselbe*
Vertex-Funktion wie die Forward-Pipeline — `MetalRenderer.mm:9722` setzt
`desc.vertexFunction = [lib newFunctionWithName:@"vertexMain"]` und nur die
Fragment-Funktion unterscheidet sich (`gbufferMain` statt `fragmentMain`).

```
vertexMainInstanced  (kUnlitMSL, neben vertexMain bei :294)
  ├── m_sceneInstancedPipeline     ← fragmentMain,  kSceneColorFormat
  └── m_gbufferInstancedPipeline   ← gbufferMain,   kGBuf0/Attr/R32F (+ Tile: Slot 4)
```

### M2 · Der Shader

`vertexMain` liest heute `u.mvp` und `u.model` aus `Uniforms` (Buffer 1). Die
instanzierte Variante liest stattdessen aus einem Device-Array, indiziert mit
`[[instance_id]]`, und lässt alles andere in `Uniforms` stehen:

```metal
struct InstXform { float4x4 mvp; float4x4 model; };   // 128 B, SYNC mit A3

vertex VSOut vertexMainInstanced(uint vid [[vertex_id]],
                                 uint iid [[instance_id]],
                                 const device VertexIn*   verts [[buffer(0)]],
                                 constant Uniforms&       u     [[buffer(1)]],
                                 const device InstXform*  inst  [[buffer(5)]])
```

`color`, `flags`, `pbr` bleiben in `u` — sie sind laut §1 über den ganzen Batch
konstant, das ist genau die Bedingung, unter der `GeometryPass` überhaupt
batcht. Der Rest des Funktionskörpers ist `vertexMain` mit `inst[iid].model` /
`inst[iid].mvp` statt `u.model` / `u.mvp`. `VSOut` bleibt unverändert, also
laufen `fragmentMain` und `gbufferMain` unverändert weiter.

**Buffer-Slot 5 ist frei.** Belegt sind heute: 0 Vertices, 1 `Uniforms`, 2/3
`matLight`/`HeParams` (nur WPO-Materialien), 4 Bone-Matrizen (`:8730`, `:9070`,
`:11343`). Kein `setVertexBuffer`/`setVertexBytes` in der Datei benutzt Index
≥ 5.

### M3 · Der Instanz-Puffer

**Nicht `setVertexBytes`.** Das ist der Fehler, der hier naheliegt: Metals
`setVertexBytes` hat eine 4-KB-Grenze, das wären **31 Instanzen** bei 128 Byte
Stride. Ein Landscape-Chunk-Feld oder ein Foliage-Batch sprengt das sofort und
fiele stumm auf die Schleife zurück — Instancing, das genau dann nicht greift,
wenn es sich lohnt.

Aber auch **kein per-Frame-Ring wie bei Vulkan/D3D12** — jedenfalls nicht in
Checkpoint M. Der Grund liegt im Backend selbst: `MetalRenderer` hält für den
Szenenpfad **gar keinen Frame-in-Flight-Ring**. Kein `dispatch_semaphore`, kein
Frame-Zähler, keine Frame-Parität in `MetalRenderer.h` (die einzige Ping-Pong-
Struktur, `m_ssrHistPos[2]`, ist SSR-Historie, nicht Frame-Synchronisation).
Einen Ring einzuführen hieße, die Frame-Synchronisation *nebenbei* zu bauen —
und ein Puffer, der pro Frame ohne Wissen über den Fortschritt der GPU
zurückgesetzt wird, ist genau der Fehler, den ein Ring verhindern soll.

Der Weg, den dieselbe Datei bereits erfolgreich geht, ist der Puffer pro Batch:

```cpp
std::vector<glm::mat4> xf;               // {mvp, model} je Instanz
xf.reserve(count * 2);
for (const glm::mat4& t : dc.instanceTransforms) { xf.push_back(viewProj * t); xf.push_back(t); }
id<MTLBuffer> instBuf = [device newBufferWithBytes:xf.data()
    length:xf.size() * sizeof(glm::mat4) options:MTLResourceStorageModeShared];
[encoder setVertexBuffer:instBuf offset:0 atIndex:5];
```

Genau so macht es der Partikelpfad bei `:14887` (`MetalRenderer.h:665`
dokumentiert es als bewusste Wahl). Das ist sicher, weil ein Encoder jede
gebundene Ressource bis zum Abschluss des Command-Buffers hält — kein
Use-after-free, keine Handsynchronisation nötig. Es kostet eine Allokation pro
Batch; das ist immer noch weit weniger als die `runLen` Draw-Calls, die es
ersetzt.

Ein Ring bleibt der spätere Schritt, **wenn** ein Profiler-Capture die
Allokationen tatsächlich als Kosten zeigt — dann aber zusammen mit einer
richtigen Frame-Synchronisation, nicht als Beifang. `k_maxInstances = 65536`
und `k_instStride = 128` gelten trotzdem als Obergrenze bzw. Layout, damit der
Vertrag mit A3 auch dann noch stimmt (§3).

### M4 · Die Aufrufstellen

Zwei, beide identisch geformt. In `EncodeScene` (`:11971`) und `EncodeGBuffer`
(`:13760`) steht heute:

```cpp
if (dc.instanceTransforms.empty()) drawInstance(dc.transform);
else for (const glm::mat4& t : dc.instanceTransforms) drawInstance(t);
```

Daraus wird das A3-Muster: `fits` prüfen, bei Erfolg füllen + **ein**
`drawIndexedPrimitives:…instanceCount:count`, sonst die alte Schleife
unverändert. `fits` braucht auf Metal **eine Bedingung mehr als A3**:

```cpp
const bool fits = instPipeline
               && count <= k_maxInstances
               && cMaterialPipeline == nullptr        // ← Metal-spezifisch
               && dc.paramOverride.empty();
```

`cMaterialPipeline == nullptr` heißt: nur eingebautes PBR wird instanziert.
Genau die Einschränkung, die GL bei `:10676` schon hat — aber hier **explizit
als Bedingung**, nicht als stiller Umweg. Ein Batch aus Graph-Material-Meshes
fällt damit auf die Schleife zurück und behält sein Material, statt es wie GL
zu verlieren. Das ist bewusst die *strengere* Variante: Optik geht vor
Draw-Call-Ersparnis.

Der transparente Pfad wird **nicht** angefasst. Metal sammelt Transparentes in
`TPDraw`-Einträgen (`:5362`) und sortiert sie back-to-front — dieselbe
Begründung wie `allowInstancing = false` bei A3. Die vorhandene
Per-Instanz-Schleife dorthin bleibt richtig.

### M5 · Zustand zurückstellen

Nach dem instanzierten Draw wieder `setRenderPipelineState:` auf die Pipeline,
die die Schleife erwartet — in `EncodeScene` ist das `boundPipeline`, das die
Lambda selbst mitführt (`:11901`). Sauberer, als hinterher zu reparieren: nach
dem instanzierten Draw `boundPipeline` auf die instanzierte Pipeline setzen,
dann findet der nächste normale Draw von selbst, dass er umbinden muss.

### M6 · Zähler

`++m_counters.draws` einmal, `m_counters.tris += (indexCount / 3) * count` —
wie A3. Der Profiler (F9-Capture) zeigt dann direkt, ob der Batch gegriffen hat.

### M7 · Der Tile-Sonderfall

`m_deferredTileMode` (Apple-Silicon-Einzelpass) hängt dem G-Buffer-Descriptor
ein fünftes Color-Attachment an (`:9727–9732`). Die instanzierte G-Buffer-PSO
muss **denselben** Descriptor bekommen, sonst lehnt Metal das Binden im
laufenden Encoder ab. Am besten den vorhandenen Descriptor-Aufbau in
`EnsureDeferredPipelines` nur um eine zweite `vertexFunction`-Variante
erweitern, statt ihn zu duplizieren.

---

## 5. Prüfungen und ihre Grenzen

| Prüfung | Wie | Was sie belegt |
|---|---|---|
| MSL-Syntax | `xcrun metal -c` auf der eingebetteten Quelle, offline | dass `vertexMainInstanced` überhaupt übersetzt — die Falle, die sonst erst zur Laufzeit auffällt |
| Bau | lokal (CLion-ninja), diese Sitzung läuft auf macOS | Metal-Pfad ist hier der einzige, der wirklich prüfbar ist |
| Optik | `scripts/he_shot.py` mit einer Szene aus ≥ 2 gleichen Meshes, Metal erzwungen; Bild vor/nach vergleichen | dass die Instanzen an den *richtigen* Stellen stehen — der Fehler, der bei falscher Matrix-Spaltenordnung entsteht, sieht aus wie „alles an einem Punkt" |
| Zähler | Profiler-Capture, `draws` vor/nach | dass der Batch tatsächlich einen Draw macht und nicht stumm zurückfällt |
| Regression | `ctest` | dass `GeometryPass` (test_rendergraph.cpp) unangetastet bleibt |

**Was keine Prüfung hier belegt:** dass D3D11/D3D12/Vulkan-Instancing auf echter
Hardware läuft. Das ist Windows-Terrain, in dieser Sandbox nicht erreichbar,
und war es bei A3 auch nicht. Ebensowenig FPS — `he_shot.py` prüft Optik, nicht
Geschwindigkeit.

---

## 6. Restliste (bewusst nicht in diesem Thema)

1. **Schattenwürfe werden nirgends gebatcht.** `ShadowPass::execute`
   (`RenderPass.cpp:118–133`) gibt einen `DrawCall` pro Objekt aus, immer
   `instanceCount = 1`. Ein Wald aus 500 Bäumen kostet 500 Shadow-Draws in
   *jedem* Backend. Der Fix liegt in `RenderPass.cpp`, nicht in den Backends —
   dieselbe Lauf-Logik wie `GeometryPass`, aber ohne die Material-Kriterien
   (der Schattenpass zeichnet nur Tiefe, es zählt nur `meshAssetId` und LOD).
   Das ist der größte verbliebene Gewinn und ein eigenes Thema wert.
2. **SSAO-Vorpass und GI-G-Buffer-Vorpass schleifen in allen fünf Backends.**
   `GL:6743`/`:7215`, `D3D11:1536`/`:2097`, `D3D12:3746`/`:4822`. Diese Pässe
   sind tiefen-/positions-only, brauchen also nur `model` — ein 64-Byte-Stride
   statt 128 würde reichen. Kleinerer Gewinn als der Schattenpass, aber
   dieselbe Mechanik.
3. **Graph-Materialien werden nirgends instanziert.** GL verliert sie dabei
   still (§2), der vorgeschlagene Metal-Pfad schließt sie sauber aus (§M4).
   Ein instanzierter Codegen-Pfad (`MaterialShaderLibrary` müsste eine
   instanzierte Vertex-Variante erzeugen) ist ein eigenes cross-backend-Thema
   und gehört zur Material-System-Vision.
4. **Der GL-Befund aus §2 ist ein Bug, kein Feature.** Solange 3. offen ist,
   sollte GL die Metal-Regel übernehmen: hat der Batch ein Graph-Material,
   nicht instanzieren. Ein Zweizeiler, aber eine Optikänderung — gehört
   angesagt, nicht nebenbei mitgenommen.

---

## 7. Roadmap-Korrektur

`Website/HorizonEngine/roadmap.json`, Eintrag `gpu-instancing` (Index 9,
`progress: 80`), beschreibt heute:

> „Same-mesh + same-material runs batched into a single instanced draw call on
> GL and Metal … D3D11/D3D12/Vulkan currently issue one draw call per instance
> in a loop …"

Das ist sachlich vertauscht. Der Text muss lauten: GL, D3D11, D3D12 und Vulkan
haben den echten Instance-Buffer; Metal zeichnet noch per Schleife. **Nicht in
diesem Schritt ändern** — die Korrektur gehört zu dem Commit, der den
Metal-Port landet, und ein Deploy wird vorher bestätigt.

---

## 8. Vorgeschlagene Schnittfolge

| Schritt | Inhalt |
|---|---|
| 1 | *(dieses Dokument)* Kartierung + Plan |
| 2 | **Checkpoint M** — Metal: `vertexMainInstanced`, zwei PSOs, Instanz-Ring, beide Aufrufstellen (§4). MSL offline validiert, lokal gebaut, `he_shot.py`-Vergleich. |
| 3 | Roadmap-Text korrigiert, `progress` hoch, Website-Abgleich (§7) |

Die ursprünglich geplanten Schritte „Port D3D11", „Port D3D12", „Port Vulkan"
entfallen — diese Arbeit liegt seit A3 auf `main`. Punkt 1 der Restliste
(Schattenwürfe batchen) ist der naheliegende Nachfolger, aber ein eigenes
Thema: er ändert `RenderPass.cpp` und wirkt auf alle fünf Backends gleichzeitig.
