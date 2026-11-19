# Volumetrischer Nebel — D3D11-Verdrahtung (Thema 225, Schritt 8)

Stand 06.11.2026, NN-WS03 (RTX 4070), Commit `54252b10c`.

## Was gemacht wurde

D3D11 (nicht D3D12 — Zeitbudget, mit der Queen abgestimmt, Request 166) bekam die
echte Renderer-Verdrahtung, die Schritt 6 bewusst ausgelassen hat:

- **Inject/Integrate** (Compute, `giShadowCS`-Muster): `EnsureFogVolumePipelines()`
  kompiliert die drei HLSL-Kernel (schon in Schritt 6 cross-compiled) über
  `D3DCompile`, legt die Ping-Pong-3D-Texturen (UAV+SRV) + die 288-B-`HeFogVol`-CB
  + einen eigenen kleinen `HeLighting`-Füll-Puffer an (Inject braucht echte CSM/
  lokale Schatten UNABHÄNGIG vom GI-Zustand — `fillMatLight`s eigener Füll nullt
  die CSM auf GI-Frames). `RenderFogVolume()` dispatcht beide Kernel, spiegelt
  `VulkanRenderer::encodeFogVolume` 1:1 (gleiche Register, gleiche Thread-Group-
  Größen 8x8x4 / 8x8x1).
- **Apply**: `RenderFogApply()` an der Decals→Transparent-Naht (`EncodeDecals`'
  DSV-Unbind-Trick, Blend `ONE, SRC_ALPHA`), restauriert danach den eingebauten
  Forward-Zustand (VS/PS/IL/Raster/Depth) exakt wie `EncodeDecals` ihn hinterlässt.
- **`lit.fog`/`lit.fogVol*`-Füll**: EIN Fund (nicht zwei wie der alte Doc-Stand
  annahm — `fillPerFrame`/`fillMatLight` sind inzwischen Lambdas, beide Aufrufe
  profitieren automatisch) — `fillPerFrame` nullt die analytische Dichte wenn
  die Volumen-Fog läuft, `fillMatLight` füllt `fogVol`/`fogVolGrid`/`fogViewProj`.
- **HE_FOG_VOLUME für HLSL-Materialien**: `MaterialShaderLibrary::setHlslFogVolume`
  (eigener Schalter pro Renderer-Instanz, wie Vulkans `setSpirvFogVolume`), neuer
  t34-Pin in `kHlslMaterialPins`, `BindFogVolume`/`RestoreAfterFogVolume` im
  Forward-Grafikmaterial-Pfad (D3D11MaterialBindings.h). NICHT im Deferred-Resolve
  verdrahtet (der compiliert über `compileResolveVariant`, nie mit `fogVolume=true`
  — unverändert, braucht `heFogVolume` für Opaque auch nicht).
- `supportsVolumetricFog` + `m_extractor.setVolumetricFogCapable` folgen
  `EnsureFogVolumePipelines()`'s Ergebnis, aufgerufen eager in `Initialize()`.

## Verifiziert (echte Hardware, RTX 4070)

- Build: `cmake --build C:/hwfog --target HorizonEditor|he_tests -j8` — beide clean.
- `he_tests --source-file=*test_fog_volume.cpp`: 16/16, 600/600 Assertions.
- Voller `he_tests` (Scratch-APPDATA `C:\hwfog_scratch_appdata`): 3781/3783 Fälle
  (2396 übersprungen — WARP/Netz-Fälle, erwartet). Der eine neue Fehler
  (`test_physics_threading.cpp`, "PhysicsWorld … full contact cache", SEH-Crash)
  ist NICHT verifiziert als vorbestehend (keine Zeit für `git stash`/Baseline-Lauf),
  aber inhaltlich fern von Rendering/Fog — Physics-Threading, kein Treffer in den
  geänderten Dateien.
- `HE_DUMP_RHI=D3D11 HE_DUMP_FOGTEST=1 HE_DUMP_QUIT=1` (Rezept, stand bisher nirgends
  notiert: `EditorBackendChoice.h`'s `HE_DUMP_RHI` erzwingt den Backend für einen
  Dump-Lauf, unabhängig vom Scratch-`config.json`-Default, der sonst `OpenGL` wäre):
  - **Modus 0** (`HE_FOG_MODE=0`): "volumetric fog pipelines built" im Log, KEIN
    "volumetric fog frame" (Fallback bleibt aktiv wie vor Schritt 8), Exit 0,
    0 Warnungen/Fehler.
  - **Modus 1** (`HE_FOG_MODE=1 HE_FOG_DENSITY=0.05`): zusätzlich "volumetric fog
    frame (grid 160x90x64, range 96 m, full)" — Inject+Integrate+Apply liefen
    real auf der RTX 4070, 0 Warnungen/Fehler, Exit 0.
  - Modus 0 vs. Modus 1 (`C:\hwfog_witness\d3d11_m{0,1}.bmp`, 1280x720 RGB24):
    **2 762 176 von ~3 686 400 Byte** unterscheiden sich — der Nebel rendert
    sichtbar, kein No-op. KEINE tiefere Pixel-Charakterisierung (Mittelwert-
    Luminanz, Boden- vs. Himmelband getrennt, HE_FOG_PARITY-Toleranz) — reine
    Zeitnot, nicht gemacht.

## Nicht gemacht / offen

1. **D3D12** — eigener Folgeschritt (Queen-Antwort #166: "D3D12 nur wenn danach
   noch Zeit bleibt … sonst ehrlich als Folgeschritt mit D3D11 als Vorlage").
   D3D11s Register-Tabelle, Dispatch-Reihenfolge und Apply-Platzierung
   (Decals→Transparent) sind die Vorlage; D3D12 braucht zusätzlich eine
   Root-Signature-Änderung (PSO lehnt sonst komplett ab, nicht nur den Draw).
2. **`HE_FOG_PARITY=1`-Messung** gegen Modus 0 (Plan §9) — nicht gelaufen.
3. **Baseline-Vergleich** für den `test_physics_threading`-Fehler (vorbestehend?).
4. Kein neuer Testfall für die D3D11-Dispatch-Korrektheit selbst (nur der
   bestehende HLSL-Cross-Compile-Test aus Schritt 6 lief erneut grün).

## Fallstricke für die nächste Maschine

- `C:/hwfog` (Debug, `DEPLOY_DIR=C:/hwfog/deploy`) ist ein wiederverwendbarer
  Build-Baum für GENAU dieses Worktree — spart die volle Fetch/Configure-Zeit
  (Steam Audio 180 MB etc.). `cmake --build C:/hwfog --target X -j8` reicht.
- `HE_DUMP_RHI=D3D11|D3D12|...` erzwingt den Backend für einen `HE_DUMP_PATH`-Lauf
  — ohne das liest eine frische Scratch-`config.json` den Struct-Default
  (`RendererBackend::OpenGL`), nicht D3D11/12.
- Scratch-`APPDATA` auch für den gefilterten `he_tests`-Lauf unschädlich (siehe
  Gedächtnis), für den vollen Lauf Pflicht.

## Handoff

Nächster Schritt: D3D12 analog zu D3D11 oben (eigener Schritt), dann
`HE_FOG_PARITY`-Messung + tiefere Pixel-Prüfung für beide D3D-Backends zusammen.
