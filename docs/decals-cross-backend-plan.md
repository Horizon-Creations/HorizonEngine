# Decals: Cross-Backend-Port (GL, Vulkan, D3D11, D3D12)

Stand 06.09.2026, Branch `claude/decals-cross-backend`. Schritt 1 des Themas:
Kartierung des bestehenden Metal-Pfads plus Portierungsplan. **Kein Feature-Code
in diesem Schritt.**

Bezug: `docs/deferred-renderer-plan.md` (P7-Folgefeature „Decals v1"),
`docs/backend-parity-plan.md` §4 (Decals hängen dort bewusst hinter dem
Deferred-Port P4), `docs/gap-audit-2026-08-25.md` Zeile 328 („Decals nur auf
Metal").

---

## 1. Der Metal-Pfad, wie er heute ist

### 1.1 Die Kette vom Component bis zum Pixel

| Stufe | Ort |
|---|---|
| Autorenseite | `src/HE_Scene/include/HorizonScene/Components/DecalComponent.h` |
| Serialisierung | `src/HE_Scene/src/SceneSerializer.cpp` |
| Editor-UI | `src/HE_Editor/InspectorPanel.cpp`, `EditorApplication.cpp`, `EditorHelp.cpp` |
| Extraktion | `src/HE_Rendering/src/RenderExtractor.cpp:447` (`extractDecals`), Aufruf `:756` |
| Frame-Daten | `src/HE_Rendering/include/HorizonRendering/RenderWorld.h:113` (`DecalData`), `:159` (`decals`) |
| Shader (GLSL 450, Quelle für alle Backends) | `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1714` (`kDecalVS`/`kDecalFS`), Einstiege `:1772`/`:1786` |
| Uniform-Layout | `src/HE_Rendering/include/material/MaterialShaderLibrary.h:247` (`DecalUniforms`) |
| Pipeline | `src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm:12688` (`EnsureDecalPipeline`) |
| Zeichnen | `MetalRenderer.mm:12749` (`EncodeDecals`) |
| Aufrufstelle | `MetalRenderer.mm:14396`, **innerhalb** des offenen G-Buffer-Encoders |

### 1.2 Datenmodell

`DecalComponent` = `color` (rgba, a = Deckkraft), `roughness` (reserviert, v1
fasst GB1 nicht an), `textureId` (optional). `extractDecals` kopiert
`t.worldMatrix` → `DecalData::transform`; die Welt-Matrix **ist** der Projektor:
ein Einheitswürfel `[-0.5, 0.5]³`, projiziert entlang der lokalen Y-Achse.

Kein Frustum-Cull, keine Sortierung, kein Limit — jedes Decal ist ein eigener
Draw. (`extractDecals` liest `t.worldMatrix` wie alle anderen Extraktoren auch
— das ist konsistent und kein Decal-Befund.)

### 1.3 Uniforms (`DecalUniforms`, std140, UBO-Binding 23, beide Stufen)

```
mat4 viewProj      // Szenen-ViewProj in Raster-Konvention
mat4 model         // Einheitswürfel → Welt
mat4 invModel
mat4 invViewProj
vec4 color         // rgba-Tint
vec4 params        // x hasTexture, y ndc-y-Vorzeichen, z Depth-Scale, w Depth-Bias
vec4 vp            // xy Viewport in Pixeln
```

304 Byte, vier `mat4` plus drei `vec4`. Metal füllt `params = (hasTex, -1, 1, 0)`
und bindet die Struktur per `setVertexBytes`/`setFragmentBytes` auf Slot 0.

### 1.4 Der Algorithmus

**Vertex** (`kDecalVS`): bufferloser 36-Vertex-Würfel aus `gl_VertexIndex` über
eine `const int idx[36]`-Tabelle, `gl_Position = viewProj * model * corner`.
Keine Vertex-Buffer, kein Index-Buffer.

**Fragment** (`kDecalFS`):
1. NDC-Tiefe lesen — auf Metal per `subpassInput heGBDepth`
   (`input_attachment_index = 3`, Binding 22), also **Framebuffer-Fetch aus
   Tile-Speicher**;
2. `d >= 1.0` → `discard` (Hintergrund);
3. Weltposition aus `gl_FragCoord`, `vp`, `params.y/z/w` und `invViewProj`
   rekonstruieren;
4. nach Box-Raum: `invModel * P`; `any(abs(lp) > 0.5)` → `discard`;
5. `color` (× `texture(heDecalTex, lp.xz + 0.5)`, Binding 19) nach `oGB0`,
   alpha-geblendet.

**Pipeline-Zustand:** volles 5-Attachment-Layout des Single-Pass-G-Buffers,
`writeMask` nur auf Attachment 0 und dort nur RGB (GB0.a trägt Metallic),
`SourceAlpha`/`OneMinusSourceAlpha`, **kein Depth-Test** (`m_noDepthState`),
`MTLCullModeFront` — Front-Faces weg, damit der Projektor auch zeichnet, wenn
die Kamera in der Box steht. Danach wird auf `CullModeNone` zurückgestellt, weil
der Fullscreen-Resolve darauf zählt.

### 1.5 Wo der Pfad heute endet

`EncodeDecals` läuft nur, wenn `m_deferredTileMode` gilt (Apple-GPU,
Framebuffer-Fetch) — und `EnsureDecalPipeline` gibt ohne `m_deferredTileMode`
sofort `false` zurück. **Auch der Metal-Zwei-Pass-Fallback hat also keine
Decals.** Alles ist zusätzlich hinter `HE_HAVE_SHADERC` gegated.

---

## 2. Der Befund, der den Plan bestimmt

Der Auftrag lautet „G-Buffer-Compositing auf die anderen vier Backends
portieren". Ein G-Buffer existiert dort aber nur einmal:

| Backend | Deferred-Pfad | Konsequenz |
|---|---|---|
| **OpenGL** | vollständig, Zwei-Pass (`EnsureGBufferTargets` `:7704`, `EnsureDeferredPipelines` `:7766`, Resolve ab `:11155`) | echter Port, GB0 existiert |
| **Vulkan** | **keiner** — nur Forward | kein GB0, in das man blenden könnte |
| **D3D11** | **keiner** — nur Forward | dito |
| **D3D12** | **keiner** — nur Forward | dito |

(Belegt: in `VulkanRenderer.cpp`, `D3D11Renderer.cpp`, `D3D12Renderer.cpp` gibt
es keine einzige G-Buffer-Fundstelle; die Treffer auf „deferred" sind
`VK_KHR_deferred_host_operations` und „deferred to the next frame"-Kommentare.
`docs/backend-parity-plan.md:245` führt „Deferred G-Buffer-Pass" für alle drei
als `--`.)

**Damit zerfällt die Aufgabe in zwei verschiedene Arbeiten:**

- **A — echter Port** (GL, plus als Beifang der Metal-Zwei-Pass-Fallback): der
  Algorithmus bleibt Zeile für Zeile derselbe, nur die Tiefenquelle wechselt von
  Framebuffer-Fetch auf eine gesampelte Textur.
- **B — Neuentwurf** (Vulkan, D3D11, D3D12): ohne G-Buffer gibt es kein
  Base-Color-Ziel vor der Beleuchtung. Entweder man baut dort erst den
  Deferred-Pfad (das ist P4 des Parity-Plans, ein eigenes Thema), oder man baut
  einen **Forward-Screen-Space-Decal**, der ins bereits beleuchtete Farbziel
  blendet.

### 2.1 Die Gabelung für Vulkan/D3D11/D3D12 — Entscheidung offen

**Weg 1 — warten auf den Deferred-Port (Parity-Plan P4).**
Optisch identisch zu Metal/GL, weil dieselbe Shading-Quelle greift. Preis: für
dieses Thema fallen drei von vier Backends aus; der Roadmap-Punkt kommt auf ~55 %
statt auf 100 %.

**Weg 2 — Forward-Screen-Space-Decal.**
Eigener Pass nach den Opaque-Draws: Szenentiefe sampeln, Weltposition
rekonstruieren, gegen die Box clippen — Schritte 2 bis 4 sind unverändert. Was
fehlt, ist die Beleuchtung: in Metal/GL landet die Decal-Farbe **vor** dem
Resolve als Base-Color und wird danach normal beleuchtet, im Forward-Pfad wäre
sie ein flacher Aufkleber auf bereits beleuchteten Pixeln.
Behebbar mit dem üblichen Kunstgriff: Normale aus den Ableitungen der
rekonstruierten Weltposition (`ddx`/`ddy`), dann ein billiges
Lambert + Ambient aus dem `MaterialShaderLibrary::Lighting`-Fill, den alle drei
Backends ohnehin schon jeden Frame hochladen (D3D11 `:3630`, D3D12 `:6233`).
Preis: eine zweite, abweichende Optik-Quelle — Schatten, GI und Punktlichter
fehlen dem Decal dort. Genau die Art „zweite Wahrheit", vor der der
Deferred-Plan warnt (eine Shading-Quelle, §4.2).

> **Entschieden am 06.09.2026 (Leitstand): Weg 2.** Kein Warten auf den
> Deferred-Port — der ist ein Vielfaches der Arbeit und würde die Decal-Aufgabe
> sprengen. Vulkan, D3D11 und D3D12 bekommen Forward-Screen-Space-Decals mit
> bewusster, hier dokumentierter Abweichung von der Metal/GL-Optik, genau wie
> Single-Map statt CSM bei den Schatten. Der Rest dieses Abschnitts steht als
> Begründung; die Empfehlung darunter ist überholt.

**Empfehlung (überholt, siehe Kasten):** Weg 1 für die Optik-Parität, aber **nicht blockierend** — d. h.
in diesem Thema A vollständig liefern (GL + Metal-Zwei-Pass), und für die drei
Forward-Backends die **Vorbedingung** aus §4 liefern (lesbare Tiefe), weil die
jeder der beiden Wege braucht und sie außerdem SSAO/SSR dort freischaltet. Der
Forward-Decal-Shader selbst ist dann ein kleiner, klar abgegrenzter Nachschlag,
den der Chefchen bewusst freigibt oder eben nicht.
**Das ist die Entscheidung, die dieser Schritt sichtbar machen soll — sie
gehört nicht in den Code, sondern vor ihn.**

---

## 3. Checkpoint A — OpenGL (der eigentliche Port)

### A1 · Sampled-Tiefe-Variante im Shader-Library

Neu neben `kDecalFS`: `kDecalFSSampled`, identisch bis auf Zeile 1:
`layout(set = 0, binding = 22) uniform sampler2D heGBDepth;` statt des
`subpassInput`, und `float d = texture(heGBDepth, uv).r;` statt `subpassLoad`.
Dazu `decalFragmentSampled(Backend)` in `MaterialShaderLibrary`; der
Cache-Schlüssel muss die Variante mitführen (heute `backend*2 + stage`, künftig
`backend*4 + …`, sonst kollidieren Fetch- und Sampled-Fragment).
`decalVertex` bleibt unverändert und cross-kompiliert für jedes Ziel.

### A2 · Konventionen — abschreiben, nicht herleiten

Der GL-Resolve rekonstruiert bereits aus derselben Tiefentextur; seine Werte
stehen in `OpenGLRenderer.cpp:11241`:

```
ResolveUniforms::depthParams = { +1.0, 2.0, -1.0, debugView }
                                 ^y-Vorzeichen  ^Scale ^Bias
```

Für GL also `params = (hasTex, +1.0, 2.0, -1.0)` gegen Metals
`(hasTex, -1.0, 1.0, 0.0)`. Genau diese drei Zahlen sind die ganze
Konventionsdifferenz — sie werden abgeschrieben, nicht neu abgeleitet.

### A3 · Ziel-FBO ohne Feedback-Loop

GB0 ist an dasselbe FBO gehängt wie `m_gbDepthTex`, und der Resolve-Kommentar
(`:11193`) sagt bereits, warum „Attachment und gesampelter Input eines FBO"
nicht geht. Also ein eigenes, schlankes FBO: **nur** `m_gbColor0` als
Color-Attachment, **kein** Depth-Attachment. Lebenszyklus an
`EnsureGBufferTargets`/`DestroyGBufferTargets` (`:7704`/`:7756`) hängen.

### A4 · Zeichnen

Zwischen dem Ende der G-Buffer-Schleife und dem Depth-Blit/Resolve
(`OpenGLRenderer.cpp:11155`), analog zu `MetalRenderer.mm:14396`:

- `glEnable(GL_FRAMEBUFFER_SRGB)` — GB0 ist `GL_SRGB8_ALPHA8` (`:7729`),
  Gegenstück zu Metals `RGBA8Unorm_sRGB`; ohne das blendet GL im falschen Raum;
- `glColorMask(TRUE, TRUE, TRUE, FALSE)` — Metallic in `.a` bleibt stehen;
- Blend `GL_SRC_ALPHA` / `GL_ONE_MINUS_SRC_ALPHA`;
- `glDisable(GL_DEPTH_TEST)`, `glDepthMask(GL_FALSE)`;
- `glBindVertexArray(m_fsVAO)` — der leere VAO, den GL für jeden bufferlosen
  Draw benutzt (`:5836`, `:6150`); Core-Profile braucht *irgendeinen* VAO;
- `glDrawArrays(GL_TRIANGLES, 0, 36)` pro Decal;
- danach jeden Zustand zurücksetzen, den der Resolve voraussetzt (der
  Metal-Pfad macht genau das mit `CullModeNone`).

### A5 · Die Falle: Cull-Seite

Metal ruft **nirgends** `setFrontFacingWinding` → Default
`MTLWindingClockwise`; GL-Default ist `GL_CCW`, und der GL-Renderer setzt
weder `glFrontFace` noch `glCullFace` irgendwo. Es muss **genau eine**
Dreiecksschicht durchkommen: beide → doppeltes Blenden (doppelte Deckkraft),
keine → das Decal verschwindet, sobald die Kamera in die Box fährt.
Die Cull-Seite ist deshalb ein eigener Prüfpunkt und **nicht** aus dem
Metal-Code abzuschreiben. Prüfmuster: ein Decal mit `a = 0.5` über einer
Ebene, einmal von außen, einmal mit der Kamera in der Box; Deckkraft muss in
beiden Fällen gleich aussehen.

### A6 · Texturbindung

`OpenGLRenderer::ResolveGraphTexture` (`:8287`) liefert die GL-Textur zur UUID —
dieselbe Rolle wie `MetalRenderer::ResolveGraphTexture`. Eine Unit wählen, die
im Deferred-Abschnitt frei ist (0..3 = G-Buffer, 9..12/14..19 belegt, siehe die
`smp(...)`-Liste `:7863`); der Decal-Pass bindet ohnehin sein eigenes Programm,
Unit 0 ist während seines Draws frei.

**Ergebnis A:** GL rendert Decals, `docs/backend-parity-plan.md` Zeile 253 wird
für GL zu `JA`.

### Stand nach Schritt 2 (Checkpoint A gebaut)

Gebaut, mit zwei Abweichungen vom Text oben:

- **A1** — die Variante ist kein zweiter Shader-Konstant, sondern ein Template:
  `makeDecalFS(bool sampled)` in `MaterialShaderLibrary.cpp` erzeugt beide
  Fassungen aus einem Rumpf. Der Unterschied sind exakt zwei eingesetzte
  Stellen (Deklaration + Leseausdruck), damit die 25 Zeilen dahinter nicht in
  zwei Kopien auseinanderlaufen können. `uv` steht dafür jetzt **vor** dem
  Tiefenlesen — die gesampelte Fassung braucht es als Koordinate, und es hängt
  nur von `gl_FragCoord` und dem UBO ab, ist also frei verschiebbar.
  Cache-Schlüssel ist `backend*4 + (0 vertex / 1 fetch / 2 sampled)`.
- **A5** — die Cull-Seite ist *keine* Differenz. GLs Vorgabe `GL_CCW` wählt
  dieselben Dreiecke wie Metals Vorgabe `MTLWindingClockwise`: die Windung wird
  in Framebuffer-Koordinaten ausgewertet, und Metals Ursprung liegt oben links
  wo GLs unten links liegt — die beiden Konventionen heben sich auf. (Dasselbe
  y, das der Shader über `params.y` mit -1 auf Metal und +1 auf GL wieder
  geradezieht.) GL bekommt daher `glCullFace(GL_FRONT)` bei unveränderter
  Front-Face-Vorgabe. Der Prüfpunkt aus A5 bleibt trotzdem der erste, den ein
  Nutzer auf echter Hardware anschauen sollte.

Nicht gebaut, weil nicht in diesem Schritt: **Checkpoint B**. Er bleibt aber die
einzige Stelle, an der sich `decalFragmentSampled` auf dieser Maschine gegen
echte Hardware beweisen lässt — der GL-Pfad ist bis dahin nur kompiliert, nie
rasterisiert.

---

## 4. Checkpoint B — Metal-Zwei-Pass-Fallback (Beifang, klein)

Derselbe `decalFragmentSampled(Backend::Metal)` gegen `m_gbDepth`, eine zweite
Pipeline mit dem Drei-Attachment-Layout des Zwei-Pass-Modus, und die
`m_deferredTileMode`-Bedingung in `EnsureDecalPipeline` (`:12695`) fällt.
Schließt die Lücke, die `docs/deferred-renderer-plan.md:31` als „SSR/Decals im
Two-Pass-Fallback" führt — und ist auf diesem Mac **real verifizierbar**, im
Gegensatz zu allem in §3 und §5. Deshalb sollte B *vor* A gebaut oder wenigstens
zusammen mit A gebaut werden: es ist die einzige Stelle, an der sich der
Sampled-Shader gegen echte Hardware beweisen lässt, bevor er blind nach GL geht.

---

## 5. Checkpoint C — Vorbedingungen der Forward-Backends

Jede dieser drei Änderungen steht für sich, ist unabhängig vom Decal und nützt
auch SSAO/SSR/Fog. Sie sind **vor** jedem Decal-Draw fällig.

**C1 · D3D11** — ~~Tiefe ist `DXGI_FORMAT_D24_UNORM_S8_UINT` mit `BindFlags =
D3D11_BIND_DEPTH_STENCIL`, also **nicht** sampelbar.~~ **Erledigt in Schritt 4**
(§6b): `R24G8_TYPELESS` + DSV `D24_UNORM_S8_UINT` + SRV `R24_UNORM_X8_TYPELESS`,
für die Swapchain-Tiefe **und** die Viewport-Tiefe. Der Decal-Pass löst den DSV
für die Dauer seiner Draws — das war der ganze Aufwand, den Vulkan mit einem
Vorpass bezahlen musste.

**C2 · D3D12** — ~~Haupt-Tiefe ist `D32_FLOAT`, DSV-only (`:2548..2554`); das
Vorbild steht drei Zeilen tiefer: die Shadow-Tiefe ist bereits
`R32_TYPELESS` mit DSV **und** SRV (`:2556..2565`). Dasselbe Muster auf die
Haupt-Tiefe anwenden, plus die Barriere `DEPTH_WRITE` →
`PIXEL_SHADER_RESOURCE` und zurück.~~ **Erledigt in Schritt 5** (§6c), genau so
wie hier vorgezeichnet — für die Swapchain-Tiefe **und** die Viewport-Tiefe.

**C3 · Vulkan** — ~~`m_depthImage` hat nur
`VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` (`:6075`). Zwei Wege:
`SAMPLED_BIT` ergänzen und per Layout-Übergang lesen (passt zu C1/C2 und zur
Sampled-Variante), **oder** einen zweiten Subpass mit der Tiefe als
Input-Attachment — dann kompiliert `kDecalFS` mit seinem `subpassInput`
**unverändert** nach SPIR-V, und Vulkan bekommt als einziges Nicht-Apple-Backend
den originalen Metal-Shader.~~

**Erledigt in Schritt 3, aber auf einem dritten Weg** — beide oben scheitern an
derselben Stelle, nämlich am **Swapchain-Pass**:

- *`SAMPLED_BIT` + Layout-Übergang* setzt voraus, dass der Decal-Draw **außerhalb**
  des Szenenpasses liegt (eine gebundene Tiefe darf man nicht sampeln). Im
  Viewport ginge das; im Swapchain-Pfad hängen aber **UI-Canvas und ImGui im
  selben Render-Pass** hinter `DrawScene`, und der Farb-Endzustand ist
  `PRESENT_SRC_KHR`. Den Pass dort aufzuschneiden heißt, das ImGui-Backend
  mitzuziehen (es hält `GetRenderPass()` und baut seine Pipelines dagegen).
- *Zweiter Subpass mit Input-Attachment* kehrt die Reihenfolge um: alles in
  Subpass 0 — auch UI und ImGui — läge dann **unter** den Decals.

Gebaut wurde deshalb ein **Kamera-Tiefen-Vorpass in ein eigenes Bild**. Damit ist
die gesampelte Tiefe nie die des aktiven Passes, der Decal-Draw darf **innerhalb**
des Szenenpasses sitzen, und alle drei Zielpfade (Swapchain, Viewport,
Viewport-HDR) brauchen keine Sonderbehandlung. Der Vorpass leiht sich
`m_shadowPass` (Depth-only, `m_depthFormat`, Endlayout `SHADER_READ_ONLY`) und
damit `m_shadowPipeline` — Framebuffer-Größen sind von ihrem Render-Pass
unabhängig, neu sind nur Bild und Framebuffer. C1/C2 (D3D11/D3D12) bleiben davon
unberührt: dort ist der zusätzliche Vorpass genauso eine Option wie das
sampelbare DSV.

---

## 6. Checkpoint D — Forward-Decal-Shader (freigegeben, gebaut in Schritt 3)

`decalFragmentForward(Backend)` in `MaterialShaderLibrary`: Schritte 1–4 wie
gehabt, danach Normale aus `dFdx/dFdy` der rekonstruierten Weltposition, Lambert
gegen die dominante Richtungsquelle + Ambient, Blend ins Farbziel statt nach GB0.
Ein Shader, drei Backends (HLSL für D3D11/D3D12, SPIR-V für Vulkan).

**Die bewusste Abweichung, explizit:** kein Schatten, keine Punkt-/Spotlichter,
kein GI auf dem Decal. Auf Metal/GL ist die Decal-Farbe *BaseColor* und wird vom
selben Resolve beleuchtet wie alles andere — eine Shading-Quelle. Hier ist sie
ein selbst beleuchteter Aufkleber auf bereits beleuchteten Pixeln, also eine
zweite, kleinere Wahrheit. Steht so auch in `docs/backend-parity-plan.md`.

### Wie er gebaut ist

- **Erzeugt aus demselben Template** wie die anderen zwei Fassungen
  (`makeDecalFS(sampled, forward)`), Cache-Schlüssel `backend*4 + 3`. Der Rumpf
  bis einschließlich Tiefenlesen ist buchstäblich derselbe Text.
- **Kein einziges `discard`.** Das ist keine Stilfrage: `dFdx`/`dFdy` sind in
  nicht-uniformem Kontrollfluss undefiniert, und im Original stehen beide
  `discard` **vor** der Position, aus der die Normale abgeleitet wird. Die
  Deckung wandert deshalb in die Alpha (`d >= 1` und außerhalb der Box → `a = 0`);
  bei `SrcAlpha/OneMinusSrcAlpha` lässt Alpha 0 das Ziel unangetastet. Ein ctest
  hält das fest: der Forward-Shader darf das Wort `discard` nicht enthalten, die
  gesampelte Fassung muss es enthalten.
- **NaN-Fallen entschärft:** ein entartetes Kreuzprodukt (`length ≈ 0`) und eine
  Sonnenrichtung der Länge 0 bekommen beide einen expliziten Zweig, weil ein NaN
  die Alpha-Multiplikation überlebt und ins Blending durchschlägt.
- **`DecalUniforms` ist um vier `vec4` gewachsen** (`sunDir`, `sunColor`,
  `ambient`, `camPos`, hinten angehängt). Metal und GL lassen sie auf null und
  lesen sie nie — der Block ist trotzdem geteilt, weil Vertex- und
  Fragment-Stufe **dasselbe** `HeDecal` deklarieren müssen (ein Member-Unterschied
  ist auf GL ein Linkfehler, den kein Test ohne Kontext findet; der Vergleich der
  beiden Blocktexte steht als ctest).

---

## 6a. Checkpoint E — Vulkan (Schritt 3, gebaut)

Der Vulkan-Pfad in einem Absatz: **Kamera-Tiefen-Vorpass** (§5 C3) vor dem
Szenenpass, dann ein Decal-Draw **innerhalb** des Szenenpasses, direkt nach den
opaken Zeichnungen und vor den transparenten — dieselbe Stelle, an der Metal und
GL ihre Decals in den G-Buffer legen.

| Baustein | Ort |
|---|---|
| Tiefen-Vorpass | `VulkanRenderer::EncodeDecalDepth`, Aufruf in `Render()` gleich nach `EncodeShadowMap` |
| Ziel des Vorpasses | `DecalDepth` (Bild + View + Framebuffer), zweimal: Swapchain-groß an `createDepthResources`, Viewport-groß an `createViewportResources` |
| Pipelines | `EnsureDecalPipelines`, lazy wie GLs `EnsureDecalProgram`; zwei Stück, `m_renderPass` (LDR) und `m_postFxSceneRP` (HDR), ausgewählt über `hdr` wie die Szenen-Pipelines |
| Zeichnen | `VulkanRenderer::EncodeDecals`, aufgerufen aus `DrawScene` nach der Opaque-Schleife |
| Uniforms | Ring `m_decalUBO[2]`, **512 B Slot-Stride** (`DecalUniforms` ist > 256 B, und mehr als 256 darf `minUniformBufferOffsetAlignment` nicht verlangen), ein Descriptor-Set pro Decal aus `m_decalPool[frame]` |
| Textur | `resolveDecalTexture(UUID)` — `DecalData::textureId` ist eine **Textur**, keine Material-UUID, der Albedo-Cache kann sie also nicht liefern |

**Die Zahlen, die Vulkan von Metal/GL unterscheiden:**
`params = (hasTex, +1, 1, 0)`. Vulkans NDC-y zeigt nach unten (`kVulkanClipFix`
kippt es), also muss `uv.y = 0` oben auf `clip.y = -1` fallen → Vorzeichen `+1`;
die Tiefe liegt in Textur **und** NDC schon in `0..1` → Skalierung 1, Bias 0.
(Metal: `-1, 1, 0`. GL: `+1, 2, -1`.)

**Die Cull-Seite** ist wie bei GL keine Differenz: `VK_FRONT_FACE_COUNTER_CLOCKWISE`
+ `VK_CULL_MODE_FRONT_BIT`. GL und Vulkan nennen dasselbe Dreieck vorderseitig,
sobald es **auf dem Schirm** gegen den Uhrzeigersinn liegt — Vulkans Flächenformel
trägt das führende Minus für seinen y-nach-unten-Framebuffer genau dafür —, und
`kVulkanClipFix` ist gerade das, was das Bild auf dem Schirm gleich macht.

**Grenzen, bewusst und zu melden:**

- **Der Vorpass zeichnet nur opake statische Meshes.** Transparentes schreibt im
  Szenenpass auch keine Tiefe, sonst käme das Decal auf der Glasscheibe an.
  Skinned Meshes, Partikel und WPO-verschobene Geometrie stehen nicht im Vorpass,
  also projizieren Decals nicht auf sie — dieselbe Lücke, die die Shadow-Map hat.
- **Der Vorpass ist ein zusätzlicher Geometriedurchlauf.** Er läuft nur in Frames,
  die überhaupt ein Decal haben: die Prüfung `m_renderWorld.decals.empty()` steht
  **vor** dem eigenen `extract`, weil `EncodeShadowMap` zwei Zeilen vorher schon
  eines gemacht hat — ein decal-freier Frame kostet damit gar nichts. (Ein Decal,
  das in einem Frame ohne Shadow-Extract auftaucht, erscheint einen Frame später.)
  Ohne `HE_HAVE_SHADERC` legt `createDecalDepth` das Bild erst gar nicht an.
- **Die Deckkraft aus dem Material zählt im Vorpass mit.** `DrawScene`
  überschreibt `obj.opacity` aus dem Material-Asset, bevor es opak von
  transparent trennt; ohne dieselbe Überschreibung im Vorpass wäre eine
  material-getriebene Glasscheibe hier opak und dort transparent — Tiefe, wo der
  Szenenpass keine schreibt, und das Decal klebt auf dem Glas.
- **Decal-Texturen werden nicht hot-reloaded.** `m_decalTexCache` hängt an der
  Textur-UUID, `InvalidateMaterial` an der Material-UUID — eine im Editor
  bearbeitete Decal-Textur bleibt bis zum Neustart alt.
- **Höchstens `k_maxDecals` (256) Decals pro Frame**, danach eine einmalige
  Warnung im Log.
- **`colorWriteMask` nur RGB** — im Viewport ist die Alpha des Bildes das, womit
  ImGui compositet.
- **Nie auf echter Hardware gelaufen.** Dieser Mac hat kein Vulkan-SDK, also baut
  `VulkanRenderer.cpp` hier nicht einmal mit. Geprüft wurde: `clang++
  -fsyntax-only` gegen die von SDL mitgelieferten Vulkan-Header (die ganze
  Übersetzungseinheit), plus der Cross-Compile-ctest für beide Shader-Stufen.
  Der Rest ist Nutzer-Verify (§7 Gate 4).

---

## 6b. Checkpoint F — D3D11 (Schritt 4, gebaut)

### Die erste Frage: ist der Vulkan-Weg 1:1 übertragbar?

Ja — und trotzdem falsch. Vulkan brauchte den Kamera-Tiefen-Vorpass, weil eine am
**aktiven Render-Pass** hängende Tiefe nicht gesampelt werden darf und der
Swapchain-Pass sich nicht aufschneiden ließ (UI-Canvas und ImGui hängen darin,
Endzustand `PRESENT_SRC_KHR`). **D3D11 hat gar keine Render-Pass-Objekte.** Es
verbietet nur, dieselbe Ressource gleichzeitig als DSV und als SRV gebunden zu
haben — und das Lösen des DSV ist hier ein einzelner `OMSetRenderTargets`-Aufruf
mitten im Frame, kein Umbau der Pass-Struktur.

Der Vorpass wäre hier also ein zusätzlicher Geometriedurchlauf für ein Problem,
das es nicht gibt. Er wäre außerdem **schlechter**: Vulkans Vorpass zeichnet nur
opake statische Meshes, also projizieren dort keine Decals auf Skinned Meshes,
Partikel oder WPO-verschobene Geometrie. D3D11 liest den echten Szenen-Tiefenpuffer
und hat diese Lücke nicht.

Gebaut ist deshalb **C1 aus §5**: die Szenentiefe wird sampelbar, der Decal-Pass
löst den DSV kurz.

### Die Bausteine

| Baustein | Ort |
|---|---|
| Sampelbare Tiefe | `D3D11RendererImpl::createDepth` + `createViewportRT`, beide über `createDepthViews` |
| Pipeline (lazy) | `D3D11RendererImpl::EnsureDecalPipeline` |
| Textur | `D3D11RendererImpl::resolveDecalTexture` (Cache nach Textur-UUID) |
| Zeichnen | `D3D11RendererImpl::EncodeDecals` |
| Aufrufstelle | `D3D11Renderer::DrawScene`, nach den opaken/skinned Draws, vor den transparenten |

**C1 konkret:** `DXGI_FORMAT_D24_UNORM_S8_UINT` → `R24G8_TYPELESS` mit
`BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE`, DSV `D24_UNORM_S8_UINT`, SRV
`R24_UNORM_X8_TYPELESS`. Bittiefe und Stencil bleiben, was sie waren. Vorbild ist
die Shadow-Map zwei Funktionen tiefer, die das seit jeher so macht. **Ein
typeless Resource lehnt einen Null-View-Desc ab** — beide Descs sind deshalb
explizit, die alten `nullptr`-Aufrufe mussten weg.

### Die Reihenfolge, an der alles hängt

```
OMSetRenderTargets(1, &rtv, nullptr)   // DSV ZUERST lösen
PSSetShaderResources(15, 1, &depthSRV) // dann erst die Tiefe lesen
… 36-Vertex-Draw pro Decal …
PSSetShaderResources(14, 2, nullptr×2) // SRVs weg
OMSetRenderTargets(1, &rtv, dsv)       // erst danach den DSV zurück
```

Andersherum löst der Runtime den Konflikt **still** auf, indem er eine der beiden
Bindungen fallen lässt; nur die Debug-Layer sagt etwas dazu. Der Pass zeichnete
dann nichts, ohne Fehler an irgendeiner Stelle. Welcher SRV der richtige ist,
entscheidet ein Zeigervergleich des gebundenen DSV gegen `dsv` / `viewportDSV` —
damit sind alle drei Pfade (Swapchain, Viewport, Viewport-HDR) abgedeckt, ohne
dass `Render()` etwas durchreichen muss.

### Die Register — der Vertrag, den D3D12 erbt

SPIRV-Cross macht aus `layout(binding = N)` schlicht `register(bN/tN/sN)`. Die
kanonischen Decal-Bindings sind 23 (HeDecal), 19 (heDecalTex) und 22 (heGBDepth) —
**b23 und s19/s22 liegen jenseits von D3D11s harten Grenzen** von 14
Constant-Buffer- und 16 Sampler-Slots pro Stufe; solche Register kann die API
überhaupt nicht bedienen. Neu ist deshalb `he::shaderc::compileHlslPinned`
(`HlslPin`, exakt nach dem Vorbild von `MslPin`/`compileMslPinned`), und
`decalVertex`/`decalFragmentSampled`/`decalFragmentForward` benutzen es für
`Backend::HLSL`:

| GLSL-Binding | HLSL-Register | Ressource |
|---|---|---|
| 23 | `b13` (VS + PS) | `HeDecal` |
| 19 | `t14` / `s14` | `heDecalTex` |
| 22 | `t15` / `s15` | `heGBDepth` |

Oben im legalen Bereich, weil dort nichts anderes im Renderer bindet (der
Szenenpfad sitzt auf b0/b1/b3/b8/b9, t0..t7, s0..s7). Das Aufräumen nach dem Pass
schrumpft damit auf „t14/t15 lösen". **D3D12 nimmt dieselben Zahlen** — es hätte
die Grenze nicht, aber zwei Backends mit einem Shader und einem Vertrag sind
weniger, was auseinanderlaufen kann.

### Die Zahlen, die D3D11 von den anderen unterscheidet

`params = (hasTex, -1, 1, 0)` — **identisch zu Metal.** `SV_Position.y` zählt von
oben, NDC-y zeigt nach oben, also fällt `uv.y = 0` auf `clip.y = +1` → Vorzeichen
-1; die Tiefe liegt in Textur und NDC bereits in `0..1` → Skalierung 1, Bias 0.
(Vulkan: `+1, 1, 0`. GL: `+1, 2, -1`.)

**Die Cull-Seite** ist wie bei GL und Vulkan keine Differenz, nur anders
buchstabiert: `FrontCounterClockwise = FALSE` (die Vorgabe) + `CULL_FRONT`. D3Ds
Fensterursprung liegt oben links wie Metals, seine im-Uhrzeigersinn-ist-vorne-
Vorgabe wählt also dieselben Dreiecke wie GLs `GL_CCW`. **`DepthClipEnable` muss
ausdrücklich gesetzt werden** — der nullinitialisierte `D3D11_RASTERIZER_DESC`
heißt „aus", das Gegenteil der GL/Vulkan-Vorgabe.

### Grenzen, bewusst und zu melden

- **Nie kompiliert, nirgends.** Vulkan ließ sich hier wenigstens per
  `clang++ -fsyntax-only` gegen die von SDL mitgelieferten Header prüfen. Für
  D3D11 gibt es auf diesem Mac **keine Header und keinen `fxc`/`dxc`** — der
  Renderer-Code ist bis zum ersten Windows-Build ungeprüft. Was hier wirklich
  läuft, ist der Register-ctest (er prüft den emittierten HLSL-Text, den
  SPIRV-Cross lokal erzeugt) und der Rest der Suite.
- **Die Kamera-Projektion trägt `kD3DClipFix` nicht.** `DrawScene` baut
  `viewProj = projection * view` ohne den 0..1-Tiefen-Fix, den der Schattenpfad
  drei Zeilen tiefer anwendet — und `RenderExtractor` (in `HorizonRendering`, ohne
  `GLM_FORCE_DEPTH_ZERO_TO_ONE`) liefert GL-Konventionen. **D3D12 macht es
  genauso.** Das ist ein bestehender Cross-Backend-Befund, kein Decal-Befund, und
  wird hier nicht angefasst. Der Decal-Pfad ist davon unabhängig richtig: er
  benutzt genau dieselbe `viewProj` wie die Geometrie und invertiert sie, und mit
  Skalierung 1 / Bias 0 rekonstruiert er in beiden Fällen dieselbe Position, die
  der Rasterizer geschrieben hat.
- **Decal-Texturen werden nicht hot-reloaded**, wie auf Vulkan: `decalTexCache`
  hängt an der Textur-UUID, `InvalidateMaterial` an der Material-UUID.
- **Kein Frustum-Cull, keine Sortierung, kein Limit.** Anders als Vulkan braucht
  D3D11 keine Obergrenze — es gibt keinen Descriptor-Pool, jedes Decal ist ein
  `Map(WRITE_DISCARD)` auf denselben Constant-Buffer.
- **Der Zeigervergleich gegen `dsv`/`viewportDSV` ist die Abbruchbedingung.** Ein
  künftiger vierter Zielpfad mit eigener Tiefe zeichnet keine Decals, bis er dort
  eingetragen ist — leise, aber nicht falsch.
- **Kein GI, keine Schatten, keine Punktlichter auf dem Decal** — die Abweichung
  aus §6, unverändert.

---

## 6c. Checkpoint G — D3D12 (Schritt 5, gebaut)

### Die erste Frage: D3D11-Weg oder Vulkan-Vorpass?

D3D12 hat explizite Render-Pass- und Barrieren-Semantik und liegt darin näher an
Vulkan als an D3D11. Trotzdem ist der **D3D11-Weg der richtige**, und zwar aus
demselben Grund wie dort: Vulkans Kamera-Tiefen-Vorpass löst ein Problem, das nur
Vulkan hat.

Vulkan brauchte ihn, weil eine am **aktiven Render-Pass** hängende Tiefe nicht
gesampelt werden darf und der Swapchain-Pass sich nicht aufschneiden ließ (UI-Canvas
und ImGui hängen darin, Endlayout `PRESENT_SRC_KHR`). **D3D12 benutzt gar keine
Render-Pass-Objekte** — `D3D12Renderer` bindet Ziele mit `OMSetRenderTargets`, nicht
mit `BeginRenderPass`. Der DSV zu lösen ist also auch hier ein einzelner Aufruf
mitten im Frame, kein Umbau der Pass-Struktur. Der Vorpass wäre ein zusätzlicher
Geometriedurchlauf und obendrein **schlechter**: Vulkan zeichnet darin nur opake
statische Meshes, D3D12 liest den echten Szenen-Tiefenpuffer und projiziert damit
auch auf Skinned Meshes, Partikel und WPO-verschobene Geometrie.

Der einzige Unterschied zu D3D11 ist die Lautstärke. D3D11 verbietet DSV und SRV
auf derselben Ressource und löst den Verstoß **still** auf, indem es eine Bindung
fallen lässt. D3D12 verlangt stattdessen eine explizite `ResourceBarrier` — dieselbe
Reihenfolge, nur dass ein Fehler hier ein Debug-Layer-Fehler ist und kein
schweigend leerer Pass.

### Die Bausteine

| Baustein | Ort |
|---|---|
| Sampelbare Tiefe (C2) | `D3D12RendererImpl::createDepth` + `createViewportRT`, beide über `createDepthSrv` |
| Root-Signature + PSOs (lazy) | `D3D12RendererImpl::EnsureDecalPipeline` |
| Textur | `D3D12RendererImpl::resolveDecalTexture` (Cache nach Textur-UUID) |
| Zeichnen | `D3D12RendererImpl::EncodeDecals` |
| Aufrufstelle | `D3D12Renderer::DrawScene`, nach den opaken/skinned Draws, vor den transparenten |

**C2 konkret:** `DXGI_FORMAT_D32_FLOAT` → `R32_TYPELESS` mit
`ALLOW_DEPTH_STENCIL`, DSV `D32_FLOAT`, SRV `R32_FLOAT`. Vorbild ist die Shadow-Map
drei Zeilen tiefer, die das seit jeher so macht. **Ein typeless Resource lehnt einen
Null-View-Desc ab** — beide DSV-Descs sind deshalb explizit, die alten
`nullptr`-Aufrufe mussten weg. Der `D3D12_CLEAR_VALUE` bleibt `D32_FLOAT`: er nennt
die *View*-Format-Sicht, nicht die der Ressource.

### Die Reihenfolge, an der alles hängt

```
OMSetRenderTargets(1, &rtv, FALSE, nullptr)          // DSV ZUERST lösen
barrier12(depth, DEPTH_WRITE → PIXEL_SHADER_RESOURCE) // dann lesbar machen
… 36-Vertex-Draw pro Decal …
barrier12(depth, PIXEL_SHADER_RESOURCE → DEPTH_WRITE) // wieder beschreibbar …
OMSetRenderTargets(1, &rtv, FALSE, &dsv)              // … erst danach zurück ans OM
```

Die Invariante ist „kein Draw, solange eine als DSV gebundene Ressource in
`PIXEL_SHADER_RESOURCE` steht". Beide Tiefen (`depthBuffer`, `viewportDepth`) liegen
sonst dauerhaft in `DEPTH_WRITE` — es gibt im ganzen Renderer keine andere Barriere
auf sie, der Vorher-Zustand ist also nicht geraten.

### Was D3D12 im Gegensatz zu D3D11 selbst mitbringen muss

**Es gibt kein `OMGetRenderTargets`.** Ein D3D12-Command-List hat keine Getter, der
Zeigervergleich, mit dem D3D11 seinen Tiefen-SRV wählt, ist hier unmöglich. Das
aktive Ziel wird deshalb aus **derselben Dreiteilung** rekonstruiert, die schon die
SSAO- und GI-Restore-Blöcke in `DrawScene` benutzen (`usingHDR && hdrRtvHeap` →
`viewportRtvHeap` → Swapchain). Das ist die einzige Wahrheit darüber, worein
`DrawScene` zeichnet; wer einen vierten Zielpfad einbaut, muss sie an allen drei
Stellen erweitern.

**Root-Signature statt freier Slots.** D3D11 bindet Ressourcen an Registernummern,
D3D12 an Root-Parameter. Der Decal-Pass hat deshalb eine **eigene Root-Signature**:
Root-CBV `b13` (beide Stufen), zwei Ein-Deskriptor-Tabellen `t14` und `t15`, zwei
statische Sampler `s14` (linear-wrap) und `s15` (point-clamp). Die Registernummern
sind unverändert die aus §6b — D3D12 hätte D3D11s Grenze von 14 CB- und 16
Sampler-Slots nicht, nimmt die Zahlen aber trotzdem, damit die beiden D3D-Backends
einen Vertrag teilen statt zweier.

**Beide Tabellen zeigen in `sceneSrvHeap`**, den Heap, den der Geometriepass
ohnehin gebunden hat. Die Tiefe bekommt dafür zwei Slots, die **hinten an den Heap
angehängt** sind (`k_decalSceneDepthSlot`, `k_decalViewportDepthSlot`) — nicht vorne
in die statische Region, weil dort jede Zahl von `[0..7]` in Kommentaren und
Range-Offsets steht. Die Decal-Textur liegt in der Mesh-Textur-Region, vergeben von
`allocAlbedoSlot`; eine untexturierte Decal zeigt auf `k_albedoNullSlot`, dessen
Null-View als (0,0,0,0) sampelt und den das `hasTexture`-Flag ohnehin ausblendet.

**Zwei PSOs statt einer Pipeline.** Das Farbziel ist `RGBA8` (Swapchain und
Viewport) oder `RGBA16F` (Viewport-HDR), und ein PSO nagelt sein RTV-Format fest —
dieselbe LDR/HDR-Paarung, die Sky, Debug-Linien und Graph-Materials hier schon
haben. `DSVFormat` bleibt `UNKNOWN` und `DepthEnable` `FALSE`: der Pass läuft mit
**ungebundener** Tiefe, und die Deckung entscheidet der Box-Clip, nicht der Z-Test.
Ein Vertex-Buffer wird nicht gelöst — das PSO trägt ein leeres Input-Layout, also
ignoriert es, was der Geometriepass hinterlassen hat (D3D11 musste, dort ist das
Input-Layout eigener Zustand, den die Runtime gegen die gebundenen Buffer prüft).

**Eine Root-Signature-Umschaltung löscht alle Root-Argumente.** Nach dem Pass wird
der Szenenzustand deshalb genau so wieder aufgebaut, wie der Skinned-Pass es zwei
Blöcke höher tut: Root-Signature, Topologie, Per-Frame-CBV, Deskriptor-Heap,
Tabelle 2, Szenen-PSO.

### Die Zahlen

`params = (hasTex, -1, 1, 0)` — **identisch zu D3D11 und Metal**, dieselbe
Begründung: `SV_Position.y` zählt von oben, NDC-y zeigt nach oben, also fällt
`uv.y = 0` auf `clip.y = +1` → Vorzeichen -1; die Tiefe liegt in Textur und NDC
bereits in `0..1` → Skalierung 1, Bias 0. (Vulkan: `+1, 1, 0`. GL: `+1, 2, -1`.)

**Die Cull-Seite** ist wie überall keine Differenz, nur anders buchstabiert:
`FrontCounterClockwise = FALSE` + `CULL_FRONT`. Anders als in D3D11 muss
`DepthClipEnable` hier nicht gegen eine Vorgabe verteidigt werden — es wird
trotzdem ausdrücklich gesetzt, damit die beiden D3D-Pfade Zeile für Zeile
vergleichbar bleiben.

### Grenzen, bewusst und zu melden

- **Nie kompiliert, nirgends.** Wie bei D3D11: auf diesem Mac gibt es keine
  D3D12-Header und kein `fxc`/`dxc`, `D3D12Renderer.cpp` wird hier nicht übersetzt.
  Was wirklich läuft, ist der Register-ctest aus Schritt 4 — er prüft den
  emittierten HLSL-Text und deckt D3D12 mit ab, weil beide Backends **denselben
  Shader mit denselben Pins** benutzen. Ein eigener D3D12-Testfall wäre eine Kopie
  ohne zusätzliche Aussage und wurde deshalb nicht geschrieben.
- **Ein Deckel bei 256 Decals pro Frame.** Anders als D3D11 (`Map(WRITE_DISCARD)`
  auf einen Constant-Buffer) hat D3D12 einen Upload-Ring fester Größe, dessen Slots
  innerhalb eines Frames nicht wiederverwendet werden dürfen. Alles ab dem 257.
  Decal fällt still weg. Vulkan hat aus demselben Grund eine Obergrenze.
- **Jede Decal-Textur belegt einen Slot der Mesh-Textur-Region** (1024 Slots, geteilt
  mit den Mesh-Basisfarben). Kein realistisches Projekt kommt dort hin, aber es ist
  derselbe Topf.
- **Decal-Texturen werden nicht hot-reloaded**, wie auf Vulkan und D3D11:
  `decalTexCache` hängt an der Textur-UUID, `InvalidateMaterial` an der
  Material-UUID.
- **Kein Frustum-Cull, keine Sortierung** — wie auf allen anderen Backends.
- **Die Kamera-Projektion trägt `kD3DClipFix` nicht**, genau wie auf D3D11 (§6b).
  Bestehender Cross-Backend-Befund, kein Decal-Befund, hier nicht angefasst; der
  Decal-Pfad ist davon unabhängig richtig, weil er dieselbe `viewProj` invertiert,
  die der Rasterizer benutzt hat.
- **Kein GI, keine Schatten, keine Punktlichter auf dem Decal** — die Abweichung
  aus §6, unverändert.

---

## 7. Prüfungen und ihre Grenzen

Dieser Mac kann GL, Vulkan und D3D **nicht** laufen lassen (kein Display für GL,
kein Vulkan/D3D überhaupt — siehe `gl-runtime-verification-constraints`). Die
Gates sind deshalb dieselben wie beim GI- und Deferred-Port:

1. **Offline-Shaderprüfung.** `glslangValidator` ohne `-G` für Desktop-GLSL;
   für MSL `xcrun metal`.
2. **Cross-Compile als ctest.** Vorbild existiert:
   `tests/test_material_graph.cpp:676` („Every standard node cross-compiles with
   all inputs wired") kompiliert Library-Shader je `Backend` und prüft `.ok`.
   Neuer Testfall: `decalVertex`, `decalFragmentSampled` und (seit Schritt 3)
   `decalFragmentForward` müssen für `Metal`, `GLSL410`, `HLSL` und `SpirV`
   kompilieren. **Vor Schritt 2 gab es keinen einzigen Decal-Test** — dieser Test
   ist das erste automatische Netz unter dem Feature. Er prüft außerdem zwei
   Dinge, die kein Compiler meldet: dass Vertex- und Fragment-Stufe denselben
   `HeDecal`-Block deklarieren (sonst GL-Linkfehler), und dass der Forward-Shader
   kein `discard` enthält (sonst undefinierte Ableitungen). Seit Schritt 4 kommt
   ein zweiter Fall dazu: **die HLSL-Register müssen in D3D11s bindbarem Bereich
   liegen** (`b13`, `t14/s14`, `t15/s15`, und die kanonischen `b23`/`s19`/`s22`
   dürfen nicht mehr auftauchen). Das ist die einzige automatische Prüfung, die
   den D3D11-Pfad überhaupt berührt.
3. **Metal-Zwei-Pass real** (Checkpoint B): per `scripts/he_shot.py` headless
   sichtbar, mit `HE_RENDER_PATH=deferred` und ohne Tile-Modus.
4. **GL/VK/D3D: Nutzer-Verify auf echter Hardware.** Wird als offen gemeldet,
   nicht als erledigt. Für **D3D11 und D3D12** gilt zusätzlich: es gibt hier
   **keinen Syntax-Check** — weder Header noch `fxc`/`dxc` liegen auf dieser
   Maschine, und `D3D11Renderer.cpp`/`D3D12Renderer.cpp` werden auf macOS nicht
   übersetzt. Der erste Windows-Build ist die erste Prüfung.

`ctest` bleibt in jedem Schritt grün; Schritt 1 ändert keinen Code, es gibt
hier also nichts zu brechen.

---

## 8. Vorgeschlagene Schnittfolge

| # | Inhalt | Verifizierbar hier |
|---|---|---|
| 1 | (dieser Schritt) Kartierung + Plan | — |
| 2 | A1 Sampled-Shader-Variante + Cross-Compile-ctest | ja |
| 3 | B Metal-Zwei-Pass-Decals | ja, visuell |
| 4 | A3–A6 GL-Decal-Pass | nur offline + ctest |
| 5 | C1–C3 lesbare Tiefe in D3D11/D3D12/Vulkan | nur Kompilat |
| 6 | D Forward-Decal — **nur nach Entscheidung zu §2.1** | nein |

Schritte 2 und 3 hängen zusammen und sollten in einer Hand bleiben: der
Sampled-Shader wird in 3 gegen echte Hardware bewiesen, bevor er in 4 blind
nach GL geht.

### Wie die Schnitte tatsächlich gefallen sind

| # | Inhalt | Stand |
|---|---|---|
| 1 | Kartierung + Plan (`00ad406b`) | fertig |
| 2 | A komplett: Sampled-Variante + GL-Decal-Pass + ctest (`ae579089`) | fertig |
| 3 | **E: Vulkan** (Tiefen-Vorpass + Forward-Decal-Pass) und **D: `decalFragmentForward`** | fertig |
| 4 | **F: D3D11** (C1 sampelbare Tiefe + Decal-Pass + HLSL-Register-Pins) | fertig |
| 5 | **G: D3D12** (C2 sampelbare Tiefe + Decal-Pass, Register aus Schritt 4 geerbt) | fertig |
| — | B Metal-Zwei-Pass-Fallback | offen, nie gebaut |

Schritt 3 hat B und D getauscht: die Entscheidung des Leitstands zu §2.1 (Weg 2)
kam vor Checkpoint B, und weil D für drei Backends derselbe Shader ist, gehörte er
zum ersten von ihnen. **Damit ist die Reihenfolge-Warnung aus §4 eingetreten**: der
Sampled-Pfad ist bis heute nirgends rasterisiert worden, weder auf GL noch auf
Vulkan. Checkpoint B bleibt die einzige Stelle, an der er sich auf dieser Maschine
gegen echte Hardware beweisen ließe, und er ist immer noch offen.
