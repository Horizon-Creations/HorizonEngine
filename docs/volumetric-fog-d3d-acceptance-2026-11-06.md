# Volumetrischer Nebel — D3D11/D3D12-Abnahme (Thema 225, Schritt 6)

Stand 06.11.2026, NN-WS03 (RTX 4070), Zweig `claude/echtes-volumetrisches-nebel-statt-hintergrund-blend`.

## Scope-Entscheidung

Der Plan (`docs/volumetric-fog-plan.md:406`) nennt Schritt 6 "Hardware-Abnahme", aber kein früherer
Schritt hat D3D11/D3D12 angefasst (Null Treffer für `FogVolume`/`fogVol`/`heApplyFogT`/Binding 34 in
`src/HE_Rendering/src/Backends/D3D11` oder `D3D12` vor diesem Schritt) — anders als Metal/GL/Vulkan, die
je einen eigenen Implementierungsschritt hatten. Mit der Queen abgestimmt (Hive-Request 165, Antwort von
chefchen): dieser Lauf macht (a) die Hardware-Regressionsabnahme, (b) den schriftlichen Implementierungsplan,
(c) so viel D3D11/D3D12-Machbarkeit wie sicher noch passt — ohne die eigentliche Renderer-Verdrahtung
(PSO/Root-Signature/Ressourcen/Apply-Spleiß) zu erzwingen, die echte Hardware-Iteration braucht.

## (a) Hardware-Regressionsabnahme — bestanden

- `cmake --build --target HorizonEditor` (VS18, `-j8`) baut clean, 0 Fehler. (Traf unterwegs die bekannte
  `HorizonSDKRelease`-"dev MISMATCH"-Falle nach Header-Edits — Workaround `-DHE_SDK_RELEASE_SET=OFF`,
  siehe Gedächtnis.)
- `he_tests.exe` komplett (nicht gefiltert, mit Scratch-`APPDATA`): **6159/6159 Testfälle, 2 162 553/2 162 553
  Assertions, 0 Fehler** — inklusive des neuen HLSL-Falls unten. Keine der bekannten Altlasten (z. B.
  `test_contentmanager`/APPDATA-Scanzahl) ist aufgetreten, weil mit Scratch-APPDATA gelaufen wurde.
- Witness-Capture `HE_DUMP_FOGTEST` auf echter Hardware, je 1 Frame, `HE_HIDDEN_WINDOW=1`:
  D3D11 Modus 0, D3D11 Modus 1, D3D12 Modus 0, D3D12 Modus 1 — alle vier Läufe `EXIT=0`, 0 Warnungen/Fehler
  (D3D11 Modus 0 hatte 1 harmlose Warnung, sonst 0/0).
- **Fallback-Beleg, zweifach:**
  1. **Code:** `IRenderer::Capabilities::supportsVolumetricFog` defaultet `false`
     (`src/HE_Core/include/Renderer/IRenderer.h:297`); weder `D3D11Renderer.cpp` noch `D3D12Renderer.cpp`
     setzen es (Metal/GL/Vulkan tun es). `HE::MakeFogVolumeData` kann `enabled` damit auf D3D nie `true`
     setzen, unabhängig von `fogMode` — und `fogMode`/`FogVolumeData` werden in beiden D3D-Backends nirgendwo
     gelesen (nur das alte `uFog`/`fogDensity`/`fogHeightFalloff` existiert dort).
  2. **Pixel:** Modus-0- vs. Modus-1-Capture pixelweise verglichen (RGB24, 960×960×3 Byte). Oberes Bildfeld
     (Himmel) unterscheidet sich (444 383 / 1 382 400 Bytes bei D3D11, 614 907 / 1 382 400 bei D3D12, max
     Abweichung 70–74) — das ist die bekannte, von `HE_SKY_TIME` nicht erfasste Wanduhr-Drift der
     D3D11-Himmelswolken (Gedächtnis "D3D11 main sky runs on m_wallTime"), nicht der Nebel. **Unteres
     Bildfeld (Boden, wo Nebel sichtbar wäre): 0 / 1 382 400 Byte-Unterschiede, max 0** auf beiden Backends.
     Modus 1 ändert auf D3D also exakt nichts gegenüber Modus 0 — der neue Szenen-Default (`fogMode = 1`,
     Schritt 7) ist für D3D unsichtbar, wie E6 es verlangt.

Fazit (a): D3D11/D3D12 sind durch den gesamten Themenstand (Schritte 1–5 + 7, append-only `HeLighting`
v3.6, neue `EnvironmentComponent`-Felder, Wetter-Hooks, neuer Szenen-Default) unverändert und unbeschädigt.

## (c) HLSL-Machbarkeit — die Plan-§10-Risikofrage ist jetzt beantwortet

Der Entwurf (`docs/volumetric-fog-plan.md:376`) hatte als offenes Risiko stehen: *"der Pin-Mechanismus
kennt heute nur Vertex/Fragment; Compute-Stufe über SPIRV-Cross ist in Schritt 2 als Erstes zu prüfen."*
Schritt 2 hat das nur für MSL geprüft (Metal). Für HLSL war es bis jetzt offen.

Ergebnis: **`he::shaderc::compileHlslPinned` trägt `Stage::Compute` ohne Sonderfall** (`ShaderCompiler.cpp`
übersetzt jede Stage generisch zu `EShLangCompute`/`ExecutionModelGLCompute`). Die drei kanonischen GLSL-450-
Kernel aus `FogVolumeShaderSource.h` (dieselben, die Metal und Vulkan produktiv nutzen) kompilieren
unverändert zu HLSL SM 5.0 — über denselben Pin-Mechanismus, den `kDecalHlslPins`/`kSSRHlslPins` für die
anderen D3D-only-Pässe schon benutzen.

**Geänderte Dateien:**
- `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`: je ein `else if (backend == Backend::HLSL)`-
  Zweig in `fogInject`/`fogIntegrate`/`fogApply`, mit frischen Register-Zuweisungen (siehe unten — diese
  Dispatches binden nichts anderes gleichzeitig, es gibt also keinen bestehenden Bereich zu vermeiden, anders
  als bei Decal/SSR).
- `tests/test_fog_volume.cpp`: die alten `CHECK_FALSE(lib.fog…(B::HLSL).ok)`-Zeilen (drei Stellen) entfernt
  und durch einen neuen, positiven Testfall `"Fog kernels (D3D11/D3D12 compute): …cross-compile for HLSL SM
  5.0 (Thema 225 step 6)"` ersetzt, der echte Compile-Ergebnisse prüft (nicht nur "compiliert", sondern
  `register(bN)`/`register(tN)`/`register(u0)`/`numthreads` an den erwarteten Stellen).
- `docs/volumetric-fog-plan.md`: unverändert (dieser Bericht ergänzt, ersetzt nichts).

**Register-Zuordnung** (neu, nicht an Renderer-Code gebunden — der nächste Schritt muss sie beim Dispatch
genau so binden):

| Kernel | Ressource | GLSL-Binding | HLSL-Register |
|---|---|---|---|
| Inject (Compute) | HeLighting | 0 | `b0` |
| Inject | HeFogVol | 36 | `b1` |
| Inject | CSM-Array | 12 | `t0` |
| Inject | Local-Shadow-Atlas | 13 | `t1` |
| Inject | Sky-Env-Cube | 15 | `t2` |
| Inject | Cloud-Shadow | 33 | `t3` |
| Inject | SkyNoise3D | 35 | `t4` |
| Inject | History (voriges Inject-Volumen) | 37 | `t5` |
| Inject | Output-Volumen (RWTexture3D) | 38 | `u0` |
| Integrate (Compute) | HeFogVol | 36 | `b0` |
| Integrate | Injiziertes Volumen (nur `texelFetch`) | 37 | `t0` |
| Integrate | Output-Volumen (RWTexture3D) | 38 | `u0` |
| Apply (Fragment) | HeFogVol | 36 | `b0` |
| Apply | Integriertes Volumen | 34 | `t0` |
| Apply | Sky-Env-Cube | 15 | `t1` |
| Apply | Szenentiefe | 39 | `t2` |

**Verifiziert:** `he_tests.exe --source-file=*test_fog_volume.cpp` (16/16 Testfälle, 600/600 Assertions) und
der volle Lauf oben (6159/6159) — beide grün, auf echter Hardware gebaut (nicht nur Syntax-Check).
**Nicht verifiziert:** keine GPU führt diese HLSL-Kernel je aus — es gibt noch keinen Dispatch-Code. Dieser
Schritt beweist nur, dass der Shader-Text ankommt, nicht dass er auf der RTX 4070 läuft.

## Nicht gemacht: die Renderer-Verdrahtung (nächster Schritt)

Das ist der eigentlich große, hardware-iterative Teil, absichtlich nicht in diesem Lauf begonnen (Risiko
eines halb verdrahteten, ungetesteten Zwei-Backend-Rush — chefchens Vorgabe war ausdrücklich das Gegenteil).
Konkret fehlt, mit Referenz auf das bestehende GI-Compute-Muster (`D3D11Renderer.cpp:3225/3634/4043`,
`giShadowCS`/`giProbeCS`: `ComPtr<ID3D11ComputeShader>`, `CreateComputeShader`, `ctx->Dispatch`):

1. **D3D11:** `ID3D11ComputeShader` für Inject/Integrate aus den obigen HLSL-Quellen erzeugen (Cache,
   wie `giShadowCS`); drei `ID3D11Texture3D` + UAV + SRV für das Fog-Volumen (Inject-Ziel = Integrate-
   Quelle, History = voriges Inject, Integrate-Ziel); `HeFogVol`-Konstantenpuffer (288 B, `FogVolumeConstants`
   aus `FogVolume.h`) pro Frame befüllen und an `b1`/`b0` binden; `Dispatch` mit den Gittermaßen aus
   `HE::fogGridFor`.
2. **D3D11 Apply:** an der Naht Decals → Transparente (`D3D11Renderer.cpp` ~7607/7616 laut Plan §3-Tabelle)
   einen Vollbild-Fragment-Pass einfügen: DSV read-only setzen (wie beim Unterwasser-Pass), `heFogIntegrated`
   (t0), `heSkyEnv` (t1), Tiefentextur (t2) binden, Blend `ONE, SRC_ALPHA`.
3. **D3D11:** `lit.fog`-Füllstellen (`uFog` bei `:423`, Fill `:6420`/`:6554` laut Plan §1) um `fogVol`/
   `fogVolGrid`/`fogViewProj` (HeLighting v3.6) ergänzen — erst DANN darf `heApplyFogT` für D3D emittiert
   werden (`HE_FOG_VOLUME`-Define für die HLSL-Materialvarianten, analog zu Metal/GL/Vulkan); vorher bleibt
   `heApplyFogT` auf D3D unerreichbar (Test `"heApplyFogT: texture 17…"` prüft das für HLSL schon: keine
   `heFogVolume`-Referenz).
4. **D3D11:** `D3D11MaterialBindings.h` um einen Null-SRV an Register `t34`-Äquivalent ergänzen (Plan §9:
   "sonst lehnt D3D12 die PSO ab" — für D3D11 weniger strikt, aber für Parität trotzdem nötig).
5. **D3D12:** dieselben drei Schritte, aber mit Root-Signature-Änderung statt loser Bindings — hier ist die
   Plan-Warnung ernst zu nehmen: fehlt eine der Bindestellen, lehnt D3D12 die PSO komplett ab, nicht nur
   einen Draw. Decals → Transparente bei ~11662 (Plan §3); Übergang `DEPTH_WRITE → DEPTH_READ |
   PIXEL_SHADER_RESOURCE` für die DSV wie beim Unterwasser-Pass.
6. **Capability:** erst wenn 1–5 für ein Backend stehen, dort `c.supportsVolumetricFog = true;` setzen
   (Metal-Beispiel: `MetalRenderer.mm:19219`) — sonst bleibt der Fallback (korrekt) für immer aktiv.
7. **Zeugen:** `HE_DUMP_FOGTEST` mit `HE_FOG_PARITY=1` gegen Modus 0 (md5/Pixel-Parität, Plan §9) je Backend,
   dann die echten Schatten/Dichte-Zeugen wie bei Metal/GL/Vulkan.

Reihenfolge-Empfehlung: D3D11 zuerst fertig und hardwareverifiziert, dann D3D12 (die lose Bindung ist
weniger fehleranfällig als die Root-Signature und lässt sich schneller iterieren).

## Commits

- (dieser Lauf) `feat(fog 225 step 6): HLSL cross-compile for Inject/Integrate/Apply (SPIRV-Cross, SM 5.0);
  D3D11/D3D12 hardware regression + fallback witness on NN-WS03`

## Handoff

Siehe `hive_handoff` / Board-Post für Schritt 6. Nächster Schritt: Punkte 1–7 oben, auf einer Maschine mit
der RTX 4070 (NN-WS03), iterativ (nicht in einem Rush) — wahrscheinlich eigener Schritt 6b oder 8.
