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

## Material-Pin t34 (Folge-Commit am selben Tag)

`D3D12MaterialRootSignature.h`: `kSrvPerDraw` 17 → 18, neuer Slot `kSlotFogVolume` = 17 (t34,
Texture3D, kein Sampler), `kRangeCount` 7 → 8 (Range t34). Die Resolve-Signatur zieht
`kResolveSlotGB0` usw. über `kSrvPerDraw` automatisch mit. `D3D12Renderer.cpp`: Template-Block
bekommt für Slot 17 eine Null-Ansicht der Dimension Texture3D; der Forward-Draw-Block schreibt das
integrierte Volumen über den Slot, solange `m_fogFrameActive`; `fillMatLight` hebt `fogVol.w` auf 1;
`Initialize` ruft `setHlslFogVolume(fogOk)` vor dem ersten Material-Compile.

Verifiziert (RTX 4070, Debug-Layer): Wasser-Szene (`HE_DUMP_WATERTEST=floor`, transluzentes
Graph-Material über `heApplyFogT`) + Nebel Modus 1: D3D12 vs D3D11 14 von 3 686 454 Bytes
(Modus 0: 6); Schattenstrahlen-Szene `HE_DUMP_FOGTEST=slits` 15 Bytes (Sonnen-CSM-Schatten im
Volumen stimmen); Deferred-Pfad mit Wasser 14 Bytes. Keine Fehler, kein neuer Debug-Layer-Eintrag
durch den Nebel. `he_tests`: test_fog_volume 16/16, test_engine_materials 15/15 und
test_material_graph 123/123 (enthalten die D3D12-Root-Signatur-PSO-Tests auf WARP).
Hinweis: die D3D12-Läufe mit der Wasser-Szene haben eine zusätzliche Debug-Layer-Warnung
("Pixel Shader expects a Render Target View bound to slot 3") bei Nebel Modus 0 UND 1; ob sie vor
dieser Änderung schon da war, ist nicht gegengeprüft (kein Basislauf mit der Wasser-Szene).

## Nicht gemacht / offen

1. **Punktlicht im Nebel weicht ab:** mit `HE_DUMP_FOGTEST=point` (Modus 1) unterscheiden sich D3D12
   und D3D11 in ~2,4 Mio Bytes (mittlere Helligkeit ~2 Stufen, max. 18,7; D3D12 heller, stärkerer
   Lichthof um die Lampe). Ohne Nebel (Modus 0) sind beide Bilder gleich (1 Byte), Sonne allein
   (`slits`, `1`) ebenfalls — es ist also der Punktlicht-Term des Inject-Kernels, nicht t34 (tritt
   auch ohne Wasser auf). Ein temporäres Abschalten des lokalen Schatten-Atlas im Nebel-Pass änderte
   das D3D12-Bild nur um max. 2,7 Stufen: der Atlas erklärt den Unterschied nicht. Nicht geklärt,
   welches der beiden Backends näher an Metal/Vulkan liegt (kein Referenzlauf); nächster Schritt:
   `docs/volumetric-fog-metal-2026-11-02.md` Zeuge c (Punktlicht-Halo) als Referenz, dann
   Inject-Samplerzuordnung (s0..s5) gegen den erzeugten HLSL prüfen.
2. **Keine Dichte-/Boden-Bank-Zeugen** und kein `HE_FOG_PARITY`-Lauf, keine Leistungsmessung.
3. **Ohne Schatten** (`shadows == false`): CSM-/Lokal-Array-Slot ist eine Null-Ansicht (liest 0, nicht 1);
   nur geprüft, dass der Frame läuft, nicht, dass das Bild dann stimmt. D3D11 bindet dort eine weiße Textur.
4. **Wolkenschatten (t3)** ist die weiße R8-Textur, kein echtes Wolkenschatten-Bild (wie D3D11).
5. **LDR-/Swapchain-Pfad** bekommt keinen volumetrischen Nebel (analytischer Nebel bleibt).

## Handoff

Nächste Schritte: (a) die Punktlicht-Abweichung D3D12 vs D3D11 klären (Punkt 1 oben), (b) Dichte-/
Boden-Bank-Zeugen und `HE_FOG_PARITY`-Lauf, (c) Leistungsmessung D3D11/D3D12.
