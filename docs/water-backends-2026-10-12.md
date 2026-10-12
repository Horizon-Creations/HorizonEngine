# D3D11, D3D12, Vulkan: Wasserfläche und Werkzeug-Darstellung (Thema 174 Schritt 9)

Stand 2026-10-12, NN-WS03 (RTX 4070). Ziel: sicherstellen, dass die Wasserflächen aus
Schritt 4/5 (Marching-Squares-Mesh + Ufer-Beschneidung) und das Engine-Wasser-Material
(Thema 152) auf D3D11, D3D12 und Vulkan gezeichnet werden, und dass die Editor-Overlays
(Spline-Linien, Punkt-Handles, Pinsel-Cursor) auf allen drei Backends erscheinen.

**Ergebnis vorweg: kein Code geändert.** Alle Pfade, die das Wasser dieses Themas
benutzt, liefen bereits über bestehende, backend-übergreifende Infrastruktur — keine davon
ist wasserspezifisch. Dieser Schritt ist eine Verifikation, kein Umbau.

## 1. Befund: was schon da war

Gelesen, nicht geraten — jede Zeile am Code nachgeschlagen:

- **Mesh-Upload.** `WaterSurface::update` (`src/HE_Scene/src/WaterSurface.cpp`) erzeugt
  pro Wasserkörper eine gewöhnliche `StaticMesh` (Vertices/Normalen/UVs) und reicht sie über
  `ContentManager::registerStaticMesh`/`replaceStaticMesh` + `IRenderer::InvalidateMesh` an
  den aktiven Renderer — denselben Pfad, den jedes andere Mesh im Spiel nimmt
  (`createMeshBuffers` in allen fünf Backends). Kein Wasser-Sonderpfad.
  `InvalidateMesh` ist auf allen fünf Backends implementiert, D3D12 und Vulkan geben
  GPU-Puffer über eine Retire-Queue frei (sicher bei Frames in Flight), dieselbe Regel wie
  bei jedem anderen Mesh-Rebuild (Sculpt, Terrain-LOD). Indexpuffer sind auf D3D12 und
  Vulkan durchgehend 32-Bit (kein `R16_UINT`-Treffer im Code) — eine Wasserfläche mit mehr
  als 65535 Vertices (Pinselteiche auf feinem Raster) ist also kein Risiko. Ein Körper, der
  auf 0 Dreiecke schrumpft, wird in `WaterSurface::update` vorher als leer erkannt
  (`Surface::empty()`) und die Surface-Entity entfernt — nie ein leerer Draw.
- **Material.** `EditorDeps/EngineContent/Materials/Water.hasset` (Thema 152) läuft über
  die normale Graph-Shader-Pipeline; `docs/water-shader-plan.md` §3/§8 belegt Transparenz,
  Blend, Tiefe (LESS, kein Write), Sky-Cube-Reflexion und Zeit-Uniform auf allen fünf
  Backends als bereits gebaut und (§8.3) auf genau dieser RTX 4070 als Rauchtest gelaufen.
  Offen blieb dort nur: die **aktuelle** Fassung des Materials (nach Schritt 7/8 von Thema
  152: `BombCells`-Knoten, Variation-Parameter, 1056 Knoten) wurde auf D3D11/D3D12/Vulkan
  nur noch **kompiliert** geprüft (FXC/WARP/spirv-val in he_tests), nie wieder **im Bild**
  gesehen (§11.4, §12.4: „Nicht belegt: D3D11/D3D12/Vulkan im Bild"). Abschnitt 3 unten
  schließt das.
- **Editor-Overlays.** Spline-Linien und Punkt-Handles laufen über
  `HE::Ed::ViewportOverlays::appendSplineGuides` → `DebugDrawBuffer` →
  `IRenderer::SetDebugLines`. Alle fünf Backends überschreiben `SetDebugLines` und zeichnen
  es auch wirklich (D3D11/D3D12: `drawDebugLines` je Frame, wenn `m_debugLines` nicht leer
  ist; Vulkan hat eine eigene Pipeline/UBO/Vertexpuffer je Frame, `vkCmdDraw`) — das ist
  bestehende, wasserunabhängige Editor-Infrastruktur (Gizmos, Selektions-Outlines nutzen
  denselben Weg). Debug-Linien zeichnen laut Code NACH der Transparent-Schleife, liegen also
  sichtbar über dem Wasser, nicht darunter.
- **Pinsel-Cursor.** `TerrainTools.cpp:505ff` zeichnet den Kreis NICHT über `DebugDrawBuffer`,
  sondern über `ImGui::GetWindowDrawList()->AddLine` (2D-Overlay, Weltpunkt→Bildschirm per
  eigener `VP`-Matrix projiziert). ImGui selbst ist für DX11, DX12 und Vulkan initialisiert
  (`EditorApplication.cpp`, `imgui_impl_dx11/dx12/vulkan`) — dieselbe Infrastruktur, die
  jedes Editor-Panel auf allen drei Backends zeichnet. Kein Wasser-Sonderfall.
- **Tiefenkonvention.** Genau die im Schritt genannte Falle ist bereits mit einer
  dokumentierten Lösung versehen: `src/HE_Rendering/include/HorizonRendering/ClipSpace.h`.
  Der gemeinsame `RenderExtractor` baut jede Kamera-/Licht-Projektion in GL-Konvention
  (-1..1), D3D11/D3D12/Vulkan (die mit `GLM_FORCE_DEPTH_ZERO_TO_ONE` übersetzt werden,
  `HE_Rendering/CMakeLists.txt:120,237,260`) multiplizieren beim Verbrauch einen festen
  Fix (`kZeroToOneDepthClipFix`/`kVulkanClipFix`) vor. Der Editor (HE_Editor, kompiliert
  OHNE das Makro) rechnet mit genau derselben GL-Konvention weiter — `TerrainTools.cpp`s
  `projectPt`/`clip.z < -1 || clip.z > 1` und die Spline-Lineal-Vorschau nutzen dieselbe
  `sceneSnapshot.camera.projection`, backend-unabhängig korrekt by construction. Kein neuer
  Code hat diese Annahme verletzt.
- **Vulkan-UBO während des Render-Pass.** Der bekannte Fehler (Lehre #85: `vkCmdUpdateBuffer`
  je Mesh-Draw innerhalb des Render-Pass) ist über `pushSceneMaterial`/`m_sceneMatBuf`
  (PR #96, Thema 144) bereits vorher behoben und auf diesem Zweig vorhanden
  (`VulkanRenderer.cpp:7485`, `:8340`, `:8748`). Wasser-Entities laufen über denselben aus
  einem vorab gemappten Ringpuffer geschriebenen Pfad, nicht über ein In-Pass-Update.
- **FXC-Sampler-Limit 16/16.** Das Wassermaterial hat **keine** Textur (`docs/water-shader-plan.md`
  §4, bestätigt durch Abschnitt 3 unten: 0 Sampler-Warnungen, kein X4509/X4500).

## 2. Was lokal gebaut und getestet wurde

Build: Release, Ninja/MSVC (vcvars64), `-DDEPLOY_DIR` auf einen privaten Ordner
(`C:/hebuild/pws9`, Standard-DEPLOY_DIR ist ein FEST codierter, von jedem Worktree geteilter
Pfad — siehe `CMakeLists.txt:813` — eine Hausaufgabe aus früheren Themen, hier beachtet),
kurzer `-B`-Pfad gegen MAX_PATH. `HE_BUILD_TESTS`/`HE_ENABLE_SHADERC` Default ON. Alle fünf
Backends verlinkt (`RendererOpenGL;RendererSoftware;RendererVulkan;RendererD3D11;RendererD3D12`),
DXC für D3D12-DXR-GI gefunden, Vulkan SDK 1.4.341 gefunden.

- `cmake --build . -j8`: **rc 0**, 110 eingebettete Laufzeit-Shader kompilieren (HLSL 53/53,
  GLSL 57/57).
- `he_tests.exe --test-case="*water*,*Water*,*Spline*,*spline*,*Lake*,*lake*,*engine water
  material*"`: **190/190 Testfälle, 358138/358138 Assertions, 0 fehlgeschlagen.**
- `scripts/water_shader_offline_check.py --he-tests build/tests/he_tests.exe -v` — zum
  ersten Mal mit den ECHTEN Windows-Werkzeugen (vorher nur auf dem Mac gelaufen, dort
  übersprungen `fxc`/`xcrun metal`): **13 bestanden, 0 fehlgeschlagen, 1 übersprungen**
  (nur `xcrun metal`, kein Xcode auf Windows — erwartet, Metal-Pfad nicht angefasst).
  GL 4.1/G-Buffer/ES 3.0/GL 4.3 geclustert (glslangValidator), Vulkan vert/frag/geclustert
  (spirv-val vulkan1.2), D3D11/D3D12 vert/frag/geclustert (**echtes `fxc.exe ps_5_0`/`vs_5_0`**,
  nicht nur die WARP-Bytecode-Reflexion aus he_tests), MoltenVK-Übersetzung
  (spirv-cross --msl). Einzige Meldungen: die vorbekannten FXC-Warnungen X3556
  (Integer-Modulo) und X3570 (Gradient in Schleife) aus der gemeinsamen Lighting-Preamble
  (`docs/water-shader-plan.md` §8.2), nichts Neues.

## 3. Rauchtest auf echter Hardware (RTX 4070), kein Hardware-Abnahme-Ersatz

Zeuge `HE_DUMP_LAKEEDIT=drawn|converted` (Thema 174 Schritt 8: eine echte Spline→See- bzw.
Pinselteich→See-Szene, nicht abgekürzt) mit `HE_DUMP_RHI=D3D11|D3D12|Vulkan`,
`CAMY=375 CAMZ=48 PITCH=-60 TOD=0.4`, 1280×720, je ein frisches `HE_CONFIG_DIR`.

| Backend | Witness-Log | Bild |
|---|---|---|
| D3D11 | `lake made, level 0.00, 2564 cells, 4267 ground vertices dug, surfaces 1, triangles 221, guide lines 252` — 0 error(s) | [d3d11-lake.png](water-backends-2026-10-12/d3d11-lake.png) |
| D3D12 | identisch | [d3d12-lake.png](water-backends-2026-10-12/d3d12-lake.png) |
| Vulkan | identisch | [vulkan-lake.png](water-backends-2026-10-12/vulkan-lake.png) |
| Vulkan, `=converted` (Pinselteich, 2287→2275 Zellen, 20 Spline-Punkte) | 0 error(s) | [vulkan-brush-converted.png](water-backends-2026-10-12/vulkan-brush-converted.png) |

Alle drei Backends zeichnen dieselbe bohnenförmige Seefläche (Wellenstruktur, Trübung,
Ufer-Schattensaum) **und** die zwölf nummerierten Spline-Punkt-Handles als Overlay darüber,
sichtbar identisch (Mittelwert-RGB der drei Vollbilder innerhalb von 2 Einheiten je Kanal).
Die Pinsel→See-Konvertierung (eigene, unregelmäßige 20-Punkt-Kontur statt der handgebauten
9-Punkt-Bohne) rendert ebenso sauber.

**Mit aktivem GPU-Debug-Layer** (`HE_GPU_DEBUG=1`, D3D12 und Vulkan — D3D11 hat laut Code
keinen Debug-Layer-Pfad, unveränderte Vorlücke):

- D3D12: `GPU debug layer + DRED ENABLED`, einzige Meldung die vorbekannte
  `ClearRenderTargetView`-Clear-Value-Warnung (`docs/water-shader-plan.md` §8.3, nicht
  wasserbezogen), **0 [ERROR]**.
- Vulkan: `validation layer ENABLED (HE_GPU_DEBUG)`, **keine einzige** WARN/ERROR/VUID-Zeile
  im Log — insbesondere keine `vkCmdUpdateBuffer`-im-Render-Pass-Meldung (die bekannte Falle
  aus Lehre #85 bliebe hier sichtbar, wenn sie noch existierte).

Das ist ein Rauchtest (ein Frame, eine Kamera, eine Tageszeit je Backend), **keine
Hardware-Abnahme** — die ist Schritt 10 (See, Pinsel, Ufer, Undo, interaktiv bedient). Nicht
geprüft hier: der Pinsel-Cursor-Ring selbst im Bild (er braucht eine Maus-Hover-Simulation,
kein Szenen-Zeuge; ImGui-Pipeline strukturell auf allen drei Backends bestätigt, s.o.),
AMD/Intel-GPUs, das exportierte Spiel, D3D11 mit Debug-Layer.

## 4. Offene Punkte für Schritt 10/11

- Kein Vulkan-Lavapipe-CI-Bildfall für Wasser (`scripts/he_vk_imagetests.py`, `CASES` kennt
  `nebula`/`clustered`/`gi`/…, kein `water`). Ließe sich wie die anderen Fälle anlegen,
  braucht aber einen echten CI-Lauf zur Schwellenwert-Kalibrierung (lavapipe-Werte weichen
  von der RTX 4070 ab) — nicht in diesem Schritt begonnen, um keine unkalibrierten
  Schwellen zu committen.
- Pinsel-Cursor-Ring nicht im Bild bestätigt (s.o.), nur strukturell (ImGui-Pipeline).
- D3D11-Debug-Layer bleibt eine Lücke (vorbestehend, nicht wasserspezifisch).
- Schritt 10 (Hardware-Abnahme) und 11 (Handbuch/Gesamtverifikation) bleiben wie geplant offen.

## 5. Windows-CI

Keine Code-Änderung in diesem Schritt — nur Dokumentation und Zeugnisbilder. Windows-CI war
auf dem letzten Commit (adaad8c0) bereits grün
(https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38180329524); dieser
Commit ändert keine Quelldatei, die CI-Matrix sollte unverändert grün bleiben.
