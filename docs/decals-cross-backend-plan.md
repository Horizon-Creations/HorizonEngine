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

**Empfehlung:** Weg 1 für die Optik-Parität, aber **nicht blockierend** — d. h.
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

**C1 · D3D11** — Tiefe ist `DXGI_FORMAT_D24_UNORM_S8_UINT` mit `BindFlags =
D3D11_BIND_DEPTH_STENCIL` (`D3D11Renderer.cpp:2429..2433`), also **nicht**
sampelbar. Umbau auf `R24G8_TYPELESS` + DSV `D24_UNORM_S8_UINT` + SRV
`R24_UNORM_X8_TYPELESS` (oder gleich `R32_TYPELESS`/`D32_FLOAT`, was zu D3D12
und Metal passt). Beachten: eine Ressource kann nicht gleichzeitig als DSV
gebunden und gesampelt werden — der Decal-Pass muss den DSV lösen.

**C2 · D3D12** — Haupt-Tiefe ist `D32_FLOAT`, DSV-only (`:2548..2554`); das
Vorbild steht drei Zeilen tiefer: die Shadow-Tiefe ist bereits
`R32_TYPELESS` mit DSV **und** SRV (`:2556..2565`). Dasselbe Muster auf die
Haupt-Tiefe anwenden, plus die Barriere `DEPTH_WRITE` →
`PIXEL_SHADER_RESOURCE` und zurück.

**C3 · Vulkan** — `m_depthImage` hat nur
`VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` (`:6075`). Zwei Wege:
`SAMPLED_BIT` ergänzen und per Layout-Übergang lesen (passt zu C1/C2 und zur
Sampled-Variante), **oder** einen zweiten Subpass mit der Tiefe als
Input-Attachment — dann kompiliert `kDecalFS` mit seinem `subpassInput`
**unverändert** nach SPIR-V, und Vulkan bekommt als einziges Nicht-Apple-Backend
den originalen Metal-Shader. Der zweite Weg ist der elegantere und der
näher am Metal-Pfad; er kostet einen Render-Pass-Umbau.

---

## 6. Checkpoint D — Forward-Decal-Shader (nur nach Freigabe von Weg 2)

`kDecalFSForward`: Schritte 1–4 wie gehabt, danach Normale aus
`dFdx/dFdy` der rekonstruierten Weltposition, Lambert gegen `heLight.sunDir` +
Ambient, Blend ins Farbziel statt nach GB0. Ein Shader, drei Backends
(HLSL für D3D11/D3D12, SPIR-V für Vulkan) — die Bindungen kommen aus dem
jeweiligen Root-Signature-/Descriptor-Muster der Material-Pipelines
(`D3D12Renderer.cpp:5747`, `D3D11Renderer.cpp:2775`, `VulkanRenderer.cpp:2024`).
**Explizit vermerken:** kein Schatten, keine Punktlichter, kein GI auf dem
Decal — bewusste Abweichung von der Metal/GL-Optik.

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
   Neuer Testfall: `decalVertex` und `decalFragmentSampled` müssen für
   `Metal`, `GLSL410`, `HLSL` und `SpirV` kompilieren. **Heute gibt es keinen
   einzigen Decal-Test** (`grep -i decal tests/` ist leer) — dieser Test ist das
   erste automatische Netz unter dem Feature.
3. **Metal-Zwei-Pass real** (Checkpoint B): per `scripts/he_shot.py` headless
   sichtbar, mit `HE_RENDER_PATH=deferred` und ohne Tile-Modus.
4. **GL/VK/D3D: Nutzer-Verify auf echter Hardware.** Wird als offen gemeldet,
   nicht als erledigt.

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
