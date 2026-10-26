# Material-Graph: Displacement-Pin auf Vulkan, D3D11, D3D12 (Schritt 4)

Thema 207, Schritt 4. Vorgänger: `docs/material-graph-tessellation-umsetzung-2026-10-17.md`
(Schritt 2: Stufen in `MaterialShaderLibrary`, nur GL verdrahtet).

## Was steht

Das Schema von GL auf den drei anderen tessellierenden Backends: `foldDisplacement` faltet den
Displacement-Body nach jedem `resolveShaders`/`resolveGBufferShaders` (Draw + Warm-up) in den
Shader-Hash, nur wenn Tessellation an ist; `GetOrBuild*` findet ihn unter dem Hash und baut die
vierstufige Variante. Fehlschlag → Variante ohne Stufen (Fallback = Pin unverbunden).
`HE_TESSELLATION=0` schaltet überall ab (A/B auf demselben Binary). `SupportsTessellation()` ist
auf allen drei überschrieben, die Material-Leiste (Schritt 3) zeigt den Hinweis dadurch nicht mehr.

- **Vulkan** (`VulkanRenderer.cpp`): Feature `tessellationShader` abgefragt und aktiviert (`:1504`,
  `pEnabledFeatures` war null); die Set-0-Zeilen, die die Stufen lesen (0,1,2,3,4–7,14), tragen
  die Tess-Bits (`VulkanMaterialLayout.h`), ohne Feature maskiert (`:2847`); `PATCH_LIST` +
  `patchControlPoints 3`, bei Fehler ohne Stufen neu (`:3375`). Body mit `heLandscapeWeights` auf
  einem Gerät ohne Binding 14 → untesselliert.
- **Winding:** die SpirV-Eval-Stufe sagt `cw` (Vulkans Domain-Ursprung oben links) statt eine
  LOWER_LEFT-Domain-Origin einzuhängen (bräuchte Vulkan 1.1, der Renderer kann eine 1.0-Instanz
  haben). **Nicht zusätzlich LOWER_LEFT einhängen**, sonst dreht es doppelt. Alle drei Backends
  zeichnen Materialien mit Cull NONE; Winding wirkt nur über `gl_FrontFacing`, das kein
  Material liest.
- **D3D11** (`D3D11Renderer.cpp`): FL-11_0-Gate (`:6143`); `MatShaders` mit `hs/ds`
  (`buildTessStages` `:4663`, FXC `hs_5_0`/`ds_5_0`); der Draw (`:7247`) bindet HS/DS, Patchlist,
  und gibt dem DS die Register der PS (b0/b1/b3, t2/t4–t7/t14, s0/s2/s4–s7); danach HS/DS null +
  TRIANGLELIST (`:7325`).
- **D3D12** (`D3D12Renderer.cpp`): `MatBytecode` mit `hs/ds`, PSO mit PATCH (`:9275`), bei Fehler
  ohne Stufen; `m_tessPSOs` wählt die Patch-Topologie am Draw (`:11224`). Die Material-Root-
  Signatur zeigt SRV-Tabelle + statische Sampler jetzt **ALL** statt PIXEL
  (`D3D12MaterialRootSignature.h`), die Resolve-Signatur bleibt PIXEL.

## Prüfung

Hardware: NN-WS03, RTX 4070, Release-Baum `C:/hw207` (eigenes Deploy), 19.10.2026. Witness
`HE_DUMP_MATERIALTEST=displace` (Kugel 8 m vor der Kamera, Wertrauschen × 0,4 m, maxFactor 16),
Skript `scripts/tess-hw/cap207.ps1` + `ana207.py`, frische APPDATA je Lauf.

| Was | Ergebnis |
|---|---|
| CI 39147211589 (Windows, Linux, Linux·Vulkan/lavapipe, macOS) | grün; Windows: `test_material_tessellation` passed |
| lokal `he_tests --source-file=*test_material_tessellation.cpp` | 15 Fälle, 402 Assertions grün |
| lokal `test_material_graph` + `test_engine_materials` | 127 Fälle, 14 136 Assertions grün |
| HS/DS-PSO mit TRIANGLE-Topologie auf WARP (nur WARN im Test) | vom Gerät abgelehnt, die Kontrolle greift |
| Log: „built a tessellated material …" | D3D11, D3D12, Vulkan je einmal |
| ref (Pin unverbunden) zweimal | 0 px Unterschied auf allen vier Backends |
| `HE_TESSELLATION=0`, Höhe 0,4 gegen ref (Fallback-Vertrag) | **0 px** auf D3D11, D3D12, Vulkan, GL |
| Höhe 0,4 gegen ref | D3D11 137 599, D3D12 137 990, Vulkan 138 069, GL 137 609 px, Silhouette verformt (`docs/images/material-tessellation-d3d-vulkan-height04.png`: ref, D3D11, D3D12, Vulkan) |
| Höhe 0, maxFactor 1 gegen ref (D3D11, D3D12) | **0 px**: auf D3D gilt Entwurf §3.2 bitgleich (anders als Apple-GL) |
| D3D12 mit `HE_GPU_DEBUG=1` (Debug-Layer + DRED) | PSO gebaut, einzige Meldung: ClearRenderTargetView-Clear-Wert (nicht Tessellation) |
| Vulkan-Validation (an) | 0 `[ERROR]` in ref- und disp-Lauf |
| konstante Höhe 0,3 (`HE_DUMP_DISPCONST`, neu), Faktor 1 und 16, Backends untereinander | D3D11↔D3D12 8 px (max 1), ↔Vulkan max 4, ↔GL max 9 (0–1 px > 8): Verdrahtung gleich |

**Befund außerhalb dieses Schritts (vermutet):** mit dem Rausch-Witness weichen D3D12 und Vulkan
(untereinander gleich, 3 px) von D3D11 und GL (untereinander gleich) in einem Band an der linken
Silhouette ab (~1 250 px > 8), schon bei maxFactor 1, bei beiden Renderpfaden; D3D12/Vulkan zeigen
dort zwei kleine Kerben. Mit konstanter Höhe verschwindet es. Vermutung: `heHash21`
(`MaterialGraph.cpp:1672`) ist nicht stabil gegen FMA-Kontraktion des Treibercompilers, derselbe
Gitterpunkt bekommt an zwei Aufrufstellen verschiedene Werte → Sprung an der Zellgrenze. Eine
float32-Nachrechnung ohne/mit FMA fand keine Zelle — **nicht bestätigt**. Betrifft jedes
Wertrauschen; als Höhe wird der Sprung zum Riss.

Neue Tests: Vulkan-Reflexion der tesc/tese gegen die Set-0-Tabelle (+ `cw`-Execution-Mode,
Negativkontrolle ohne Tess-Bits); D3D12-WARP-PSO VS+HS+DS+PS (Negativkontrolle: PIXEL-Signatur
mit samplender Domain-Stufe); D3D11-WARP-Draw: konstante Höhe verschiebt ein Dreieck mit
+x-Normale um die Höhe in NDC, 0,3 auf `maxDisplacement` 0,1 geklemmt.

## Handoff

- **Topic / step:** 207, Schritt 4 (Vulkan/D3D11/D3D12-Anbindung)
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ 253ef8d2, 165ac7ba, d1b0fe0c + Abnahme-Commit (Witness `HE_DUMP_DISPCONST`, Skripte, Bild, diese Tabelle)
- **Done:** Verdrahtung auf allen drei Backends, Tests, `SupportsTessellation()`, Hardware-Abnahme auf RTX 4070.
- **Verified:** Tabelle oben (CI grün, lokale Tests, Pixel-A/B auf vier Backends, D3D12-Debug-Layer, Vulkan-Validation).
- **Not verified:** andere GPUs (AMD-iGPU, lavapipe nur über CI-Compile/Tests); Transluzent- und
  G-Buffer-Draw einzeln im Bild (Renderpfad-A/B nur bei Faktor 1, beide gleich); Winding-Bits
  (Cull NONE überall, unsichtbar); Rausch-Befund oben.
- **Decisions taken:** SpirV-`cw` statt Domain-Origin; Root-Signatur ALL statt eigener DS-Tabelle;
  Tess-Bits in der Layout-Tabelle, vom Renderer maskiert; Fallback bei jedem Stufen-Fehler.
- **Traps (Merge in release/0.7.0):** Thema 217 (REN-103) bindet D3D11-Materialzustand einmal pro
  Lauf (`bindMatRunState`/`endMatRun`): danach HS/DS/Topologie bei **jedem** Material-Draw setzen
  (null für Nicht-Tess), sonst zeichnet ein normales Material nach einem tessellierten als
  Patches. Thema 214 (REN-101): `foldDisplacement` steht jetzt in vier Backends (Warm-up, Draw,
  G-Buffer), gehört in den Resolve-Cache. Thema 215 (REN-808): `VkPipelineCache` auch in die zwei
  neu geschriebenen `vkCreateGraphicsPipelines`-Aufrufe.
- **Next steps:** 1. Schritt 5 (Abnahme, Beispielmaterial mit Höhenkarte; dafür eine Textur statt
  Wertrauschen nehmen, oder vorher den `heHash21`-Befund klären). 2. `cap207.ps1` taugt für die
  Abnahme (`-Rhis`, `-Only c1,c16,f1…`); nach dem Merge von Thema 217 Kapitel D3D11 erneut fahren.
