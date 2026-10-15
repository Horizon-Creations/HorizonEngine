# Material-Graph: Tessellation/Displacement als Pin am Output-Knoten — Entwurf

Thema 207, Schritt 1 von 4 (nur Doku, kein Code). Ziel: ein `Displacement`-Pin am
Output-Knoten des Material-Graphen, der auf D3D11, D3D12, Vulkan und (seit der Entscheidung
in Abschnitt 6) OpenGL per GPU-Tessellation neue Geometrie erzeugt und entlang der Normale
verschiebt; Metal und der Software-Renderer bekommen einen sauberen Fallback ohne Pin-Wirkung.

Kennzeichnung: **[geprüft]** = im Code/Quelltext nachgelesen mit Zeilenangabe,
**[nicht geprüft]** = Annahme oder Spezifikationswissen, auf diesem Gerät nicht ausgeführt.

---

## 1. Ausgangslage

- **Vorlage ist World Position Offset (Pin 8).** Der Pin erzeugt einen eigenen
  Vertex-Body in Scope `"vs"` (`MaterialGraph.cpp:1619-1631`), der als
  `MatShaderGen::vertexBody` → `MaterialAsset::customShaderVertGlsl` reist
  (`MaterialGraph.h:507-510`, `Assets.h:293-297`) und pro Backend von
  `MaterialShaderLibrary::customVertex` in die Vertex-Vorlage gespleißt wird
  (`MaterialShaderLibrary.cpp:57-128`, `:917-939`). Der Body wird in den Pipeline-Schlüssel
  gefaltet (`:949-950` forward, `:995-996` G-Buffer). Aufrufer: Metal `:12700`,
  D3D11 `:4730`, Vulkan `:3175`, D3D12 `:9119`, GL `:3794`. [geprüft]
- **Die Vorlage hat kein viewProj.** Der U-Block ist `mvp, model, color, flags, pbr`
  (`MaterialShaderLibrary.cpp:75-77`); WPO rechnet den Welt-Offset per Transpose-Trick in
  Objektraum zurück (`:121-124`, exakt für starre Transformationen mit uniformer Skalierung). [geprüft]
- **Keine GPU-Tessellation im Repo.** Kein Backend hat Hull/Domain/tesc/tese. Die
  „Terrain-Tessellation" (`TerrainComponent::tessellationFactor`, `TerrainComponent.h:43-56`)
  ist reine CPU-Verfeinerung der Chunk-Meshes (`tests/test_terrain_tessellation.cpp`). [geprüft]
- **`he::shaderc` kennt nur Vertex/Fragment/Compute** (`ShaderCompiler.h:19-24`), HLSL-Ziel
  ist SM 5.0 (`ShaderCompiler.cpp:143`, `:272`). [geprüft]
- **SPIRV-Cross (gepinnt `vulkan-sdk-1.4.313.0`, `CMakeLists.txt:712-718`) gibt für HLSL
  keine Tessellationsstufen aus:** `CompilerHLSL::get_inner_entry_point_name` wirft
  „Unsupported execution model." für alles außer Vertex/Fragment/Compute/Mesh/Task
  (`_deps/spirv_cross-src/spirv_hlsl.cpp:2925-2948`). MSL und GLSL können beide Stufen
  (`spirv_msl.hpp:312`, `:426`). [geprüft im gefetchten Quelltext]
- **Beide D3D-Backends kompilieren mit FXC** (`D3DCompile`, `D3D12Renderer.cpp:2116`, Profile
  `vs_5_0`/`ps_5_0`), `hs_5_0`/`ds_5_0` stehen also zur Verfügung. D3D11 ruft heute kein
  einziges `DSSet*`/`HSSet*` (grep: 0 Treffer). D3D12-Root-Signaturen setzen keine
  `DENY_HULL/DOMAIN`-Flags, aber mehrere Parameter sind `SHADER_VISIBILITY_VERTEX`
  (`D3D12Renderer.cpp:2343`, `:2986`, `:3226`, `:5059`, `:5430`). [geprüft]
- **Vulkan aktiviert `tessellationShader` nicht.** Bei der Geräteerzeugung werden nur
  AccelerationStructure/RayQuery/BufferDeviceAddress eingeschaltet (`VulkanRenderer.cpp:1456-1496`);
  Set-Layouts haben `VERTEX | FRAGMENT` (`:3674`, `:6272`). [geprüft]
- **Die WPO-Vertexstufe deklariert keine Texturen** (`wpoDeclarations` = Lighting-Block +
  HeParams + Noise, `MaterialShaderLibrary.cpp:901-903`), Texture Sample emittiert `texture()`
  mit impliziter LOD (`MaterialGraph.cpp:857-862`). Ein Texture Sample am WPO-Pin dürfte
  deshalb nicht kompilieren. [nicht geprüft: aus dem Code gelesen, nicht ausgeführt]
- **Schatten sehen WPO nicht.** CSM/Local-Shadow zeichnen mit fest eingebauten
  Depth-only-Vertexshadern (D3D11 `:589`, D3D12 `:622`, Metal `:415`, Vulkan
  `scene_shadow.vert`, `:2553`). Der Occlusion-Culler nimmt WPO-Materialien als Verdecker
  heraus (`OcclusionCuller.cpp:44`). [geprüft]

## 2. Datenformat

### 2.1 Pin 9 „Displacement"

- Neuer Eintrag **am Ende** der Output-Registry (`MaterialGraph.cpp:28-38`):
  `{ "Displacement", F::Float, 0.0f } // 9 — Domain-Stufe`, plus `kMatOutputDisplacementPin = 9`
  in `MaterialGraph.h:307-317`. Links liegen nach Pin-INDEX; angehängt verschiebt nichts →
  **kein `kMatGraphVersion`-Bump**, keine Remap-Tabelle (vgl. `MaterialGraph.cpp:2115-2130`).
- **Semantik: Höhe in Metern entlang der interpolierten Welt-Normale** (Float, nicht Vec3).
  Begründung: Höhenkarte ist der Hauptfall; braucht keinen Tangentenrahmen; die Bounds-Grenze
  ist eine Zahl. Ein Vec3 am Pin wird nach der allgemeinen Regel auf `.x` gekürzt (wie bei
  jedem Float-Pin); der Editor soll das nicht stillschweigend tun, sondern am Link warnen.
  Vektor-Displacement wäre später ein eigener Pin 10 („World Displacement", Vec3) — wieder
  angehängt, ohne Bruch.
- `matOutputPins` (`MaterialGraph.cpp:341-360`): Zeile unabhängig vom Blend-Mode sichtbar;
  im Domain `UserInterface` ausgeblendet und von der Codegen ignoriert.
- Unverbunden = keine Tessellation, **byte-gleicher** Shadertext wie heute (alte Pipeline-Caches
  bleiben gültig).

### 2.2 Tessellation-Einstellungen am Output-Knoten

Alle vier `p[]` des Output-Knotens sind belegt (lit, Blend-Mode, Mask-Cutoff, Domain,
`MaterialGraph.cpp:38`). Neue Heimat: ein **optionales JSON-Objekt `"tess"` am Output-Knoten**,
im Speicher ein kleines Struct an `MatGraphNode` (nur am Output bedeutsam):

| Feld | JSON | Standard | Bedeutung |
|---|---|---|---|
| `maxFactor` | `max` | 8 | maximaler Tess-Faktor, geklemmt auf 1..64 (D3D11-Grenze, Vulkan garantiert ≥ 64) |
| `fadeNear` | `near` | 5 m | bis zu dieser Kameradistanz volle `maxFactor` |
| `fadeFar` | `far` | 40 m | ab hier Faktor 1 (nur noch Basis-Vertices werden verschoben) |
| `maxDisplacement` | `maxDisp` | 0.1 m | Bounds-Vertrag, siehe 4.3; die Höhe wird darauf geklemmt |

- **Am Knoten statt am Graphen**, weil `matNodeToJson` genau das ist, was CollabDocSync pro
  Element synchronisiert (`MaterialGraph.h:566-578`); ein Graph-Feld bräuchte einen eigenen
  Collab-Pfad.
- Geschrieben wird `"tess"` nur, wenn es vom Standard abweicht oder Pin 9 verbunden ist →
  alte Dateien und ihr JSON bleiben byte-gleich.
- Die Werte werden als Konstanten in den Hull-Text gebacken (kein neuer UBO); eine Änderung
  ergibt neuen Text und damit einen neuen Pipeline-Schlüssel. `maxDisplacement` braucht
  zusätzlich die CPU (Bounds) und wandert deshalb auch ins MaterialAsset.

### 2.3 Codegen-Ausgabe (`MatShaderGen`)

- `std::string displacementBody;` — kanonische GLSL-Anweisungen, enden in `float heDisp`,
  erzeugt in einem eigenen Scope **`"ds"`** (Funktionsaufrufe darin `"ds/<id>"`), analog
  `vertexBody`. Leer = Pin unverbunden.
- `MatTessSettings tess;` — die Werte aus 2.2.
- Texturen teilen die Slot-Liste der Fragmentstufe (heTexP0..3, Binding 4+k) — ein Sampler
  am Displacement-Pin verbraucht keinen zusätzlichen Slot, wenn dieselbe Textur auch im
  Fragment gelesen wird.

### 2.4 Asset und Paket

- **MaterialAsset:** neues `customShaderDispGlsl` + die vier Tess-Floats als **angehängter
  Tail** am MTRL-Chunk (Lesen `ContentManager.cpp:~318-345`, Schreiben `:~2005-2030`; Schritt 2
  muss das tatsächliche Tail-Ende finden). Laut `HAsset.h` (Kopfkommentar) kein
  Versions-Bump: alte Leser hören vorher auf, alte Dateien enden vorher. Beim Laden mit Graph
  regeneriert wie `customShaderVertGlsl` (`ContentManager.cpp:811`); der serialisierte Wert
  zählt nur für gepackte Materialien ohne Graph.
- **MaterialShaderVariant** (`Assets.h:15-44`): zwei neue Felder `tessControl`, `tessEval`
  (D3D: HLSL-Text; Vulkan: SPIR-V-Bytes), angehängt am Record-Tail mit derselben
  `requires`-Sonde wie `kHasClustered` (`ContentManager.cpp:2331`). Leer = altes Paket
  oder Bake fehlgeschlagen → zur Laufzeit cross-kompilieren, ohne shaderc → Pipeline ohne
  Tessellation (Fallback 5).
- **Pipeline-Schlüssel:** `displacementBody` und die Tess-Werte werden in `resolveShaders`
  und `resolveGBufferShaders` in `hashOut` gefaltet wie `vertBody` — **aber nur, wenn das
  aufrufende Backend tatsächlich tesselliert** (die beiden Funktionen bekommen dafür einen
  Schalter vom Backend). Auf Fallback-Backends bleibt der Schlüssel der des unverbundenen
  Pins (Abschnitt 5), sonst entstünde pro Displacement-Material eine doppelte Pipeline.

## 3. Shaderstufen

Reihenfolge: **VS (Standard oder WPO, unverändert) → HS → DS (Displacement) → FS (unverändert).**
WPO wirkt also vor dem Displacement, auf die Kontrollpunkte.

### 3.1 Hull (Engine-Code, kein Graph-Inhalt)

- Dreiecks-Patches, 3 Kontrollpunkte, Durchreichen der VS-Ausgaben.
- Partitionierung `fractional_odd` (D3D `partitioning("fractional_odd")`, Vulkan
  `fractional_odd_spacing`) — gleiche Semantik, weiche Übergänge.
- **Kantenfaktor symmetrisch aus den beiden Kanten-Endpunkten:** Distanz Kamera →
  Kantenmitte (Welt), `f = 1 + (max−1)·saturate((far − d)/(far − near))`. Zwei Dreiecke, die
  sich eine Kante teilen, rechnen denselben Wert → keine Risse durch Tess-Faktoren.
  Innenfaktor = Maximum der drei Kanten. Kamera aus `heLight.camPos` (Legacy-Präfix, im
  WPO-Vertex schon gebunden).
- Später möglich: Bildschirmraum-Faktor (Ziel-Kantenlänge in Pixeln) statt Distanz.

### 3.2 Domain (Engine-Prolog + Graph-Body)

Kanonisch, für beide Wege identisch:

```
// Prolog: baryzentrisch interpolieren, Varyings ZUERST schreiben (WPO-Präzedenz)
vWorldPos = interp(cp.vWorldPos); vNormal = normalize(interp(cp.vNormal));
vUV = interp(cp.vUV); vColor = interp(cp.vColor);
vec4 heClip = interp(cp.gl_Position);           // exakt: mvp ist linear, Interpolation vor der Division
<displacementBody>                              // → float heDisp
heDisp = clamp(heDisp, -maxDisp, maxDisp);      // Bounds-Vertrag
vWorldPos += vNormal * heDisp;
vec3 heObjDisp = (transpose(mat3(u.model)) * (vNormal * heDisp))
               / max(dot(u.model[0].xyz, u.model[0].xyz), 1e-8);
gl_Position = heClip + u.mvp * vec4(heObjDisp, 0.0);
```

Der Transpose-Trick wirkt **nur auf den Offset**, genau wie bei WPO (`:122-124`): Bei
`heDisp == 0` ist das Ergebnis bitgleich zum Draw ohne Tessellation, auch bei nicht-uniformer
Skalierung — der Fallback-Vertrag aus 5 hält also auch auf den unterstützten Backends. Kein
neues viewProj, keine Extra-Varyings, VS unverändert (der DS braucht `SV_Position`/`gl_Position`
der Kontrollpunkte, die der HS durchreicht); nur der Offset hat dieselbe Einschränkung wie WPO
(starr + uniform skaliert). Assembliert von **einer** Funktion mit zwei Prologen (wie
`assembleVertex` mit `VertexInput`):

- **Vulkan:** echter `tese`-Shader (`layout(triangles, fractional_odd_spacing, <winding>) in;`,
  Interpolation über `gl_TessCoord`), glslang → SPIR-V direkt. `tesc` ebenfalls kanonisch.
- **D3D11/D3D12:** derselbe Body als **Vertex-Stage-„Kernel"** mit Eingängen
  `heInWorldPos/heInNormal/heInUV/heInColor` (≠ Ausgangsnamen) durch die bestehende
  HLSL-Kette; `rename_entry_point("main", "heDispKernel", Vertex)` (`spirv_cross.hpp:348`) +
  `CompilerHLSL::Options::use_entry_point_name = true` (`spirv_hlsl.hpp:147`) liefern eine
  aufrufbare `heDispKernel_inner()` auf statischen Globals. Eine **handgeschriebene
  `ds_5_0`-Schale** (`[domain("tri")]`) interpoliert aus `OutputPatch<CP,3>` und
  `SV_DomainLocation`, setzt die statischen Eingänge, ruft `heDispKernel_inner()` und kopiert
  `vNormal..vWorldPos` + `gl_Position` in ihre Ausgabe (TEXCOORD0..3 + SV_Position, so wie
  SPIRV-Cross die Locations des VS benennt). Weil der Kernel `gl_Position` wie ein VS schreibt,
  gelten dieselben Clip-Space-Fixups wie für den normalen D3D-VS. Hull ebenfalls
  handgeschrieben (`hs_5_0`). [nicht geprüft: Mechanismus nur im Quelltext gelesen]
- `he::shaderc`: `Stage` bekommt `TessControl`, `TessEval` (glslang
  `EShLangTessControl/Evaluation`); HLSL-Ziel mit Tess-Stufe → klarer Fehler statt
  SPIRV-Cross-Exception. HLSL-Optionen um den Entry-Rename erweitern; die Pin-Tabellen
  (`HlslPin` usw.) brauchen die neuen Stage-Werte.

### 3.3 Regeln im Scope „ds"

- Texture Sample / Array Sample → `textureLod(s, uv, 0.0)` (keine Ableitungen im DS;
  Präzedenz `MaterialGraph.cpp:1067`). Mip 0 kann in der Ferne flimmern; dort ist der
  Tess-Faktor ohnehin 1. Später: LOD aus Faktor und UV-Spanne.
- Knoten mit `dFdx/dFdy/fwidth` oder impliziter LOD (Normal Map Sample, Cotangent-Frame
  `:867`; Bombing mit Gradienten) und szenenabhängige Knoten (Backdrop, Szenentiefe) liefern
  im DS **neutrale Werte** wie Water Ripple in `"vs"` (`:1397-1402`), nie einen Compile-Fehler.
  Stufentest wie dort: `sc.key == "ds" || sc.key.rfind("ds/", 0) == 0`.
- Lesbar: `vWorldPos/vNormal/vUV/vColor` (interpoliert), HeParams, `heLight.sunDir.w` (Zeit),
  Texturen. `heLandscapeWeights` (Binding 14, liest schon per `textureLod`) wäre für
  Terrain-Displacement je Schicht wertvoll — optional, muss dann auch im DS gebunden sein.

## 4. Laufzeitverhalten

### 4.1 Pässe

| Pass | Displacement | Grund |
|---|---|---|
| Forward opak / clustered, G-Buffer | ja | teilen die Vertex-Auswahl über `resolveShaders`/`resolveGBufferShaders` (D3D11 `:5318`/`:7009`, D3D12 `:4406`/`:10974`, Vulkan `:7669`/`:8254`) |
| Transluzent | ja, falls derselbe Weg [nicht geprüft] | Hive-Lektion „OpenGL forward Translucent": GL-Translucent weicht vom opaken Zweig ab; Schritt 2 prüft den Pipeline-Bau |
| Instanced (Foliage, `scene_instanced.vert`, Vulkan `:75`, `:2487`) und Skinned | **nein → Fallback** | eigene Vertexshader, erreichen die fünf `customVertex`-Stellen nie (Thema 182 wird fragen) |
| CSM, Local-Shadow-Atlas | **nein** (wie WPO heute) | fest eingebaute Depth-only-VS |
| GI-BVH/TLAS | **nein** | sieht das Basis-Mesh |

**Normale:** `vNormal` im Fragment ist die **unverschobene** interpolierte Normale. Der
Normal-Pin des Materials muss die zur Höhe passende Normal Map tragen, sonst wirkt die
Verformung im Licht flach (kein Bug, sondern so entworfen; Schritt-3-Vorschau entsprechend
beschriften). Später möglich: Normale im DS per finiten Differenzen aus zwei weiteren
Auswertungen des Bodys.

Folge: Schatten und GI sehen die flache Fläche. Positives Displacement (Fläche hebt sich)
ist unkritisch; **negatives** legt die sichtbare Fläche unter den Schattenwerfer und unter
den GI-Strahlstart (5 cm über der Fläche, Lektion Thema 190) → Eigenschatten/schwarze GI.
Empfehlung an Autoren und Hilfetext: Höhe ≥ 0 auslegen. Material-bewusster Schattenpass für
WPO+Displacement = eigenes Folgethema.

### 4.2 Risse

- Tess-Faktoren: rissfrei durch 3.1.
- **Harte Kanten/UV-Nähte** (duplizierte Vertices mit verschiedenen Normalen) reißen auf —
  konstruktionsbedingt; dokumentieren, später ggf. „Displacement-Normale" (gemittelt).
- **Terrain-Chunks unterschiedlicher LOD** haben T-Kreuzungen; Displacement vergrößert die
  linsenförmigen Stufen aus Thema 190. Gleiche LOD = gleiche Kantenpunkte = rissfrei.
- Hilfetext muss den Pin klar von `TerrainComponent::tessellationFactor` (CPU) abgrenzen.

### 4.3 Bounds und Culling

- Frustum-Bounds um `maxDisplacement × max(Skalierung)` aufblähen; die Klemme in 3.2 macht
  das zum Vertrag statt zur Hoffnung.
- `OcclusionCuller.cpp:44`: Displacement-Materialien wie WPO als Verdecker ausnehmen.

### 4.4 Bindungen pro Backend (Schritt 2)

- **D3D11:** HS/DS setzen; alles, was der DS liest (U, HeLighting, HeParams, heTexP + Sampler),
  zusätzlich per `DSSetConstantBuffers/ShaderResources/Samplers` an dieselben Register; HS braucht
  HeLighting. Topologie `D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST`.
- **D3D12:** PSO mit HS/DS, `PrimitiveTopologyType = PATCH`, IA-Topologie 3-CP-Patchlist;
  betroffene Root-Parameter von `VERTEX` auf `ALL`.
- **Vulkan:** `tessellationShader` abfragen und aktivieren; `PATCH_LIST` +
  `VkPipelineTessellationStateCreateInfo{patchControlPoints = 3}`; Set-Layout-`stageFlags` um
  `TESSELLATION_CONTROL/EVALUATION` erweitern.
- Winding (`outputtopology` / `cw/ccw`) muss zur Cull-Konvention jedes Backends passen — pro
  Backend nachlesen, nicht raten.

## 5. Fallback

**Ohne Unterstützung verhält sich das Material exakt so, als wäre Pin 9 unverbunden:** gleicher
Pipeline-Schlüssel, gleicher Shadertext, kein Magenta, kein ausgelassener Draw. Gilt für:

1. Metal, Software (siehe Tabelle); GL-Kontext ohne Tessellation (unter 4.0).
2. Vulkan-Gerät ohne `tessellationShader`.
3. HS/DS-Compile- oder PSO-Fehler → einmal loggen, Pipeline ohne Tessellation.
4. Paket ohne `tessControl/tessEval` und ohne shaderc.
5. D3D11/D3D12 auf Feature Level unter 11_0 (Hull/Domain sind ein 11_0-Feature).
6. Draw-Pfade mit eigenem Vertexshader (Instanced, Skinned, siehe 4.1).

Der Editor zeigt am Pin einen Hinweis „wirkt nur auf D3D11, D3D12, Vulkan, OpenGL". Bounds dürfen
auch im Fallback aufgebläht bleiben (konservativ, billig).

| Backend | API kann Tessellation | Toolchain | Plan |
|---|---|---|---|
| D3D11 | ja, FL 11_0 / SM 5.0 | FXC hs/ds_5_0 ja; SPIRV-Cross-HLSL nein → Schalen | **unterstützt** |
| D3D12 | ja | wie D3D11 | **unterstützt** |
| Vulkan | ja, optionales Feature `tessellationShader` (Desktop fast überall; lavapipe, MoltenVK) [nicht geprüft] | glslang tesc/tese direkt | **unterstützt** |
| OpenGL | ja, Core seit 4.0; macOS meldet 4.1 [nicht geprüft auf diesem Gerät] | SPIRV-Cross-GLSL kann beide Stufen | **unterstützt** (Entscheidung Abschnitt 6) |
| Metal | ja (Post-Tessellation-Vertex + Faktor-Puffer per Compute) | SPIRV-Cross-MSL kann beide Stufen | Fallback (Plan) |
| Software | nein | — | Fallback |

## 6. Offene Entscheidung: wo sieht man die Verformung?

Auf den Macs läuft der Editor auf Metal; Vulkan wird auf macOS nicht gebaut (nur Windows-MSVC
und der Linux-lavapipe-Job). **Unter dem Plan-Umfang kann auf keinem Mac jemand die Verformung
sehen** — Schritt 3 („Vorschau der Verformung") hängt damit an Windows.

- **A** Hinnehmen: Vorschau zeigt auf Mac nur den Hinweis; Sichtabnahme auf NN-WS03 + lavapipe-Bildtest.
- **B** GL als vierten Backend aufnehmen: billigster Zusatz (SPIRV-Cross-GLSL410 kann tesc/tese,
  `glPatchParameteri` + `GL_PATCHES`), und headless GL-Dumps (`he_shot.py`) machen die Verformung
  auf dem Mac prüfbar.
- **C** Metal aufnehmen (eigenes Folgethema): mehr Renderer-Arbeit (Faktor-Puffer, Compute-Vorpass,
  MSL-Pins).
- **D** Nur die Material-Editor-Vorschau verschiebt per Vertex (WPO-artig) auf dem dichten
  Vorschau-Mesh: weicht vom Spielbild ab, widerspricht „ohne Pin-Wirkung".

**Entschieden (Queen, Anfrage #100, 15.10.): B.** GL wird viertes unterstütztes Backend für
0.7.0 (SPIRV-Cross-GLSL410 tesc/tese, `glPatchParameteri(GL_PATCH_VERTICES, 3)`, Draw mit
`GL_PATCHES`, Programm mit vier Stufen; Uniform-Blöcke/Sampler, die der DS liest, binden per
Name wie heute). Damit zeigen headless GL-Dumps die Verformung auf dem Mac. Metal (C) ist als
separates Folgethema vorgemerkt, nicht jetzt. Ob macOS-GL 4.1 Tessellation tatsächlich ausführt,
ist der erste Test in Schritt 2 [nicht geprüft].

## 7. Prüfplan für Schritt 2–4

- **he_tests (`test_material_graph.cpp`):** Registry-Schleife deckt Pin 9; JSON-Rundlauf mit und
  ohne `"tess"`, alte Graphen byte-gleich; jeder Knotentyp im Scope „ds" kompiliert per glslang
  als TessEval **und** als Vertex-Kernel; Pin unverbunden → `displacementBody` leer, `glsl`
  unverändert. Offen zu prüfen: was ein **alter** Build mit einem Link auf Pin 9 macht
  (Collab zwischen Versionen) — `materialGraphFromJson` muss den Link verwerfen, nicht abstürzen.
- **HLSL offline:** `glslangValidator -D -V -e` für HS/DS-Schalen (Gedächtnis: hlsl-offline-check).
- **Vulkan:** lokal `clang -fsyntax-only` gegen MoltenVK-Header; Laufzeit im lavapipe-CI-Bildtest.
- **D3D11/D3D12:** nur auf NN-WS03; diese Abnahme bleibt offen, bis die Hardware lief (nie per CI „fertig").
- **Fallback-Vertrag als Pixel-A/B:** auf Metal und GL ist ein Material mit verbundenem Pin 9
  bitgleich zum selben Material mit unverbundenem Pin (`scripts/he_shot.py`, frame_pixel_ab).

## Handoff

- **Topic / step:** 207, Schritt 1 von 4 (Entwurf, nur Doku)
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ f235d3da (Entwurf) + Korrekturcommit direkt danach (`git log -- docs/material-graph-tessellation-displacement-entwurf-2026-10-15.md`), gepusht
- **Done:** Entwurf für Pin 9 „Displacement" (Float, Meter entlang Normale), Tess-Einstellungen als `"tess"` am Output-Knoten, Stufenaufbau VS→HS→DS→FS, D3D-Weg über HLSL-Schalen + SPIRV-Cross-Kernel, Fallback = Pin unverbunden.
- **Files touched:** nur diese Datei.
- **Verified:** alle [geprüft]-Stellen per grep/Lesen im Worktree und im gefetchten SPIRV-Cross (`cmake-build-dmg26/_deps/spirv_cross-src`); Zeilen oben.
- **Not verified:** kein Build, kein Shader kompiliert; WPO+Texture-Fehler nur gelesen; Entry-Rename-Mechanismus nur gelesen; Vulkan/GL/MoltenVK-Fähigkeiten aus Spezifikationswissen.
- **Decisions taken:** Float statt Vec3; Einstellungen am Knoten (Collab); Klemme auf maxDisplacement; Clip-Position der Kontrollpunkte interpolieren + nur den Offset per Transpose-Trick (wie WPO, Höhe 0 = bitgleich); Schlüssel nur auf tessellierenden Backends falten; Instanced/Skinned = Fallback; Schatten/GI ohne Displacement (wie WPO); ein Domain-Assembler mit zwei Prologen.
- **Decision by the queen (#100):** GL is the fourth supported backend (Option B); Metal is a separate follow-up topic.
- **Decisions suggested, not taken:** Displacement-Normale gegen Naht-Risse; Bildschirmraum-Faktor; material-bewusster Schattenpass.
- **Pending:** Schritt 2 Codegen + Backends, Schritt 3 Editor-UI, Schritt 4 Abnahme.
- **Traps:** SPIRV-Cross-HLSL wirft für tesc/tese; WPO-Vertex hat keine Texturen; D3D11 bindet pro Stufe; D3D12-Root-Parameter mit `VISIBILITY_VERTEX`; Vulkan-Feature nicht aktiviert; negatives Displacement → Schatten/GI-Artefakte.
- **Next steps:** 1. `MaterialGraph.h/.cpp`: Pin 9, `MatTessSettings`, `"tess"`-JSON, Scope „ds", `displacementBody` + Tests. 2. `ShaderCompiler.h/.cpp`: Stages TessControl/TessEval, HLSL-Entry-Rename. 3. `MaterialShaderLibrary`: `assembleDomain` (zwei Prologe), `tessControl/tessEval`, Hash-Faltung. 4. Asset-Tail + Variant-Felder. 5. GL zuerst (auf dem Mac prüfbar: headless Dump, Pixel-A/B Höhe 0 gegen unverbunden), dann Vulkan, D3D11, D3D12 verdrahten (4.4).
