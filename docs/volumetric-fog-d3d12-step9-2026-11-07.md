# Volumetrischer Nebel — D3D12-Verdrahtung (Thema 225, Schritt 9)

Stand 07.11.2026, NN-WS03 (RTX 4070, Debug-Layer + DRED an), aufbauend auf Schritt 8 (D3D11,
`docs/volumetric-fog-d3d11-step8-2026-11-06.md`).

## Was gemacht wurde (`D3D12Renderer.cpp`)

- **Pipelines** (`EnsureFogVolumePipelines`, eager in `Initialize`, setzt
  `m_extractor.setVolumetricFogCapable` und `supportsVolumetricFog`): die drei HLSL-Kernel aus
  Schritt 6 per `D3DCompile` (cs_5_0 / cs_5_0 / ps_5_0) plus der Fullscreen-VS der Material-Bibliothek.
  Drei kleine Root-Signaturen nach der Registertabelle aus
  `docs/volumetric-fog-d3d-acceptance-2026-11-06.md`:
  - Inject: CBV b0 (HeLighting), CBV b1 (HeFogVol), SRV-Tabelle t0..t5, UAV-Tabelle u0,
    sechs statische Sampler s0..s5 in der Reihenfolge, in der D3D11 sie bindet
    (Punkt, Punkt, Linear, Punkt, Linear/Wrap, Linear).
  - Integrate: CBV b0, SRV t0, UAV u0.
  - Apply: CBV b0, SRV-Tabelle t0..t2, s0/s1 Linear/Clamp; Blend `ONE, SRC_ALPHA`,
    RTV `R16G16B16A16_FLOAT`.
- **Ressourcen:** zwei Ping-Pong-3D-Texturen (Inject) + integriertes Volumen, alle RGBA16F mit UAV;
  eigener shader-sichtbarer Descriptor-Heap mit `k_fogSlots`=16 Einträgen je Frame-Slot (CPU-seitig
  beim Aufzeichnen geschrieben); je Frame-Slot ein `HeFogVol`- und ein eigener `HeLighting`-Puffer
  (Inject braucht echte CSM/lokale Schatten unabhängig vom GI-Zustand).
- **Dispatch** (`RenderFogVolume`): direkt vor dem `fillPerFrame` des Scene-Pass, also nach
  Schatten-Pässen und Sky-Cube-Bake. Zustände der Quellen (CSM, lokaler Atlas, Sky-Cube, Noise,
  weiße R8) gehen für den Compute-Lesezugriff auf `PIXEL|NON_PIXEL` und danach zurück; die drei
  3D-Texturen führen ihren Zustand selbst (Inject-Ausgabe UAV → NPSR, integriert UAV → PSR).
- **Apply** (`RenderFogApply`): vor dem Transparent-Block, hinter Decals; Tiefe wie bei
  `EncodeDecals`/Unterwasser-Pass erst vom OM nehmen, dann als SRV lesen; danach wird der Scene-
  Root-Zustand wie nach den Decals neu aufgebaut.
- **Füllstellen:** `fillPerFrame` nullt die analytische Dichte, `fillMatLight` setzt `fogVol.x/y/z`,
  `fogVolGrid`, `fogViewProj`, solange `m_fogFrameActive`.
- Nur HDR-Viewport-Frames (die Apply-PSO ist RGBA16F); alle anderen Frames behalten den analytischen Nebel.

## Verifiziert (echte Hardware, RTX 4070, Debug-Layer an)

- `HorizonEditor` baut sauber (Debug-Baum `C:/hwfog`).
- `HE_DUMP_RHI=D3D12 HE_DUMP_FOGTEST=1`, Forward und `HE_RENDER_PATH=1` (Deferred):
  - Modus 0: "volumetric fog pipelines built", KEIN "volumetric fog frame", Exit 0.
  - Modus 1 (`HE_FOG_DENSITY=0.05`): "volumetric fog frame (grid 160x90x64, range 96 m, full)",
    Exit 0.
  - Debug-Layer: nur die drei Clear-Wert-Warnungen, die der Baseline-Lauf vor dieser Änderung
    ebenfalls hat; keine neue Warnung, kein Fehler.
- **Parität zu D3D11:** das Modus-1-Bild von D3D12 und das D3D11-Bild aus Schritt 8 unterscheiden
  sich in 17 von 3 686 454 Bytes (Modus 0: 4). Helligkeit Himmel-/Mitte-/Bodenband Modus 0 → 1:
  71.4→173.8 / 86.9→111.2 / 85.9→102.0, bei D3D11 und D3D12 identisch.
- Deferred-Modus-1-Bild ist byte-gleich zum Forward-Bild (0 Bytes Unterschied); die Test-Szene
  beleuchtet beide Pfade offenbar gleich, das beweist also den Apply-Platz, nicht mehr.

## Nicht gemacht / offen

1. **Material-Pin t34 für D3D12:** `HE_FOG_VOLUME` ist für D3D12-Graph-Materialien NICHT an
   (`setHlslFogVolume` wird für D3D12 nicht aufgerufen), `fogVol.w` bleibt 0. Transparente
   Graph-Materialien (Wasser, Glas) behalten daher `heApplyFogT`s analytischen Fallback, opake
   Materialien sind über `fogVol.x` ausgenommen (kein Doppelnebel). Für den Pin braucht
   `m_matRootSig` (D3D12MaterialRootSignature.h) einen SRV-Eintrag t34 plus Descriptor im
   Material-Block; fehlt eine Bindestelle, lehnt D3D12 die PSO komplett ab (Warnung aus dem
   Schritt-6-Doc) — deshalb bewusst nicht halb gemacht. Die Pin-Zeile in `kHlslMaterialPins` ist
   aus Schritt 8 schon da und wirkt erst, wenn `HE_FOG_VOLUME` für HLSL an ist.
2. **Keine echten Schatten-/Dichte-Zeugen** (Schattenstrahlen im Nebel, Lichthalo, Boden-Bank wie
   bei Metal/GL/Vulkan) — nur das Gesamtbild und die Parität zu D3D11.
3. **Ohne Schatten** (`shadows == false`): CSM-/Lokal-Array-Slot ist eine Null-Ansicht (liest 0, nicht 1);
   nur geprüft, dass der Frame läuft, nicht, dass das Bild dann stimmt. D3D11 bindet dort eine weiße Textur.
4. **Wolkenschatten (t3)** ist die weiße R8-Textur, kein echtes Wolkenschatten-Bild (wie D3D11).
5. **`HE_FOG_PARITY=1`-Pixeltoleranz** gegen Modus 0 und die Leistungsmessung nicht gelaufen.
6. **LDR-/Swapchain-Pfad** bekommt keinen volumetrischen Nebel (analytischer Nebel bleibt).

## Handoff

Nächste Schritte: (a) t34 für D3D12-Materialien (Root-Signatur + `setHlslFogVolume(true)` vor dem
ersten Material-Compile), (b) Schatten-/Dichte-Zeugen und Parität, (c) Leistungsmessung D3D11/D3D12.
