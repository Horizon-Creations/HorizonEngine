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

| Was | Ergebnis |
|---|---|
| MSVC `cl /Zs` (Release-Flags aus `build.ninja`) auf allen 6 berührten TUs | sauber |
| Build, Link, `he_tests`, Hardware-Lauf auf NN-WS03 | **nicht gemacht**: C: 1,4 GB frei, Löschen alter Build-Bäume vom Session-Klassifizierer abgelehnt |
| Neue Tests (`test_material_tessellation.cpp:497/565/680`) | nur in CI (Windows: FXC + WARP) |

Neue Tests: Vulkan-Reflexion der tesc/tese gegen die Set-0-Tabelle (+ `cw`-Execution-Mode,
Negativkontrolle ohne Tess-Bits); D3D12-WARP-PSO VS+HS+DS+PS (Negativkontrolle: PIXEL-Signatur
mit samplender Domain-Stufe); D3D11-WARP-Draw: konstante Höhe verschiebt ein Dreieck mit
+x-Normale um die Höhe in NDC, 0,3 auf `maxDisplacement` 0,1 geklemmt.

## Handoff

- **Topic / step:** 207, Schritt 4 (Vulkan/D3D11/D3D12-Anbindung)
- **Branch / commit:** `claude/material-graph-shader-tessellation-displacement-als-pin-am-o` @ 253ef8d2, 165ac7ba + dieser Doku-Commit
- **Done:** Verdrahtung auf allen drei Backends, Tests, `SupportsTessellation()`.
- **Verified:** nur `/Zs` (MSVC, Windows-Release-Flags). Sonst nichts.
- **Not verified:** Linux/clang-Compile des Vulkan-TU, Link, jeder `he_tests`-Lauf, Hardware,
  Vulkan-Validation, D3D12-Debug-Layer, D3D11-Draw auf echter GPU. CI-Lauf siehe Hive-Post.
- **Decisions taken:** SpirV-`cw` statt Domain-Origin; Root-Signatur ALL statt eigener DS-Tabelle;
  Tess-Bits in der Layout-Tabelle, vom Renderer maskiert; Fallback bei jedem Stufen-Fehler.
- **Traps (Merge in release/0.7.0):** Thema 217 (REN-103) bindet D3D11-Materialzustand einmal pro
  Lauf (`bindMatRunState`/`endMatRun`): danach HS/DS/Topologie bei **jedem** Material-Draw setzen
  (null für Nicht-Tess), sonst zeichnet ein normales Material nach einem tessellierten als
  Patches. Thema 214 (REN-101): `foldDisplacement` steht jetzt in vier Backends (Warm-up, Draw,
  G-Buffer), gehört in den Resolve-Cache. Thema 215 (REN-808): `VkPipelineCache` auch in die zwei
  neu geschriebenen `vkCreateGraphicsPipelines`-Aufrufe.
- **Next steps:** 1. Platz auf NN-WS03 (Mensch). 2. Privater Baum (`-B C:/hw207 -DDEPLOY_DIR=C:/hw207/deploy`,
  scratch APPDATA), `he_tests --source-file=*test_material_tessellation.cpp`. 3. `HE_DUMP_MATERIALTEST=displace`
  auf D3D11/D3D12/Vulkan gegen `HE_DUMP_DISPPIN=0` und gegen `HE_TESSELLATION=0` (beide 0 px zur
  Referenz erwartet), Log nach „built a tessellated material …" greppen, D3D12 mit `HE_GPU_DEBUG=1`,
  Vulkan-Validation greppen. 4. Danach Schritt 5 (Abnahme).
