# GI-Reflexionen: Himmels-Bake einmal je Frame, Landschaftsfarbe aus der RVT (Metal, 2026-11-20)

Referenz für den Windows-Nachzug (D3D11, D3D12, Vulkan). Bilder in `docs/gi-rvt-metal-2026-11-20/`.

## 1. Himmels-Cubemap der Reflexionen (Commit 16335996d)

**Befund (Catania, M5, 3200 x 1800, Detailmodus, GI-Reflexionen an, 2 Bounces):** Die Reflexions-Himmels-Cubemap
(`EncodeSkyReflCube`, voller Himmels-Shader mit Wolken) kostete ~9,5 von 11-12,6 ms GPU. Die Traces selbst:
0,24 ms (Szene) + 0,75 ms (Wasser). Sie wurde in **jedem** `EncodeGIReflections` gebacken, im Deferred-Frame mit
Wasser also zweimal, je 2 Flächen 512². Darum änderten Qualitätsstufe und Bounces die Kosten kaum.
Messung: `HE_GIREFL_SKY=0` (Bake aus) -> GPU 3,4 ms.

**Änderung:** höchstens einmal je Frame (`m_skyReflThisFrame`, im Frame-Start zurückgesetzt), eine Fläche 256²
je Frame (ganze Cube alle 6 Frames). `kSkyReflCubeSize` = 256, `kSkyReflFacesPerFrame` = 1 in Metal **und** OpenGL.

| Variante (Metal) | GPU ms | Reflections ms |
|---|---|---|
| vorher (2x je Frame, 512², 2 Flächen) | 11,0-12,6 | 5,1 + 5,4 Wasser |
| 1x, 512², 2 Flächen | 8,50 | 5,35 |
| 1x, 512², 1 Fläche | 6,33 | 2,66 |
| 1x, 256², 2 Flächen | 5,84 | 2,58 |
| **1x, 256², 1 Fläche (übernommen)** | **4,5-5,2** | **1,4** |

Bild: `1-skycube-vorher.png` / `2-skycube-nachher.png` (Spiegel-Zeuge, praktisch gleich).
Der Detailmodus trennt jetzt `GBuffer`, `Reflections`, `SSRComposite`, `WaterRefl` (vorher alles unter "SSAO").

## 2. Landschaftsfarbe im GI-Treffer aus der RVT

**Befund:** Ein GI-Reflexionsstrahl, der die Landschaft trifft, kennt nur Mittelfarben je Schicht (Auto: Gras, Erde,
Fels, Schnee nach Neigung/Höhe; gemalt: Layer-Mittel nach Gewichtung). Keine Textur, daher "flach / ohne Farbe"
im Spiegel, auf **allen** Backends. Ohne residentes Albedo-Array fällt es auf Weiß zurück.

**Änderung (Metal, HW- und SW-Kern):** Wird die Landschaft gerade aus der RVT gezeichnet (`m_rvtSwaps`), liest der
Treffer die Seiten: `giRvtAlbedo` (Seitenmathe aus `RvtLookup.h`, Mip aus dem Footprint
`(|Pw-cam| + dist) * 1.155 / texH + dist * rough²` (GGX-Alpha; ein Spiegel weitet nichts), Tabelle fällt selbst auf gröbere Seiten zurück, Bit 23 = kalter
ASTC-Pool). Daten: `GiLandscape::terrainEntity` (Extraktor), `GILandGpu::rvtA/rvtB` = `RvtVolumeA/B` des Consumers
(aus `graphParamNames`/`shaderParamData`), Bindung: Tabellen Textur 13..16, heißer Pool 17, kalter Pool 18.
Kein Treffer in der Volume oder keine Seite -> die alten Mittelfarben.

**Nachweis:** Zeuge `HE_DUMP_RVTDRIVER_MIRRORSHOT=<prefix>` (mit `HE_DUMP_AUTOLANDMIRROR=1 HE_DUMP_GIREFL=1`, RVT an):
`<prefix>.own.bmp` (Tausch pausiert = Mittelfarben) gegen `<prefix>.rvt.bmp` (Seiten). `5-gi-rvt-nachweis-magenta.png`:
mit `giRvtAlbedo` fest auf Magenta sind alle Bodentreffer in beiden Spiegeln und den Pfützen magenta (Pfad greift).
`3-gi-mittelfarben.png` / `4-gi-rvt-seiten.png`: in dieser kleinen, gleichmäßigen 128-m-Zeugenszene bei flachem Licht fast
gleich (Spiegel-Boden RGB 34,7/31,1/22,9 gegen 35,3/31,4/23,0); der Unterschied zeigt sich an strukturierter Landschaft.
Kosten in Catania: GPU 4,49 ms, Reflections 1,42 ms (wie ohne).

## Handoff

- **Topic / step:** GI-Optimierung + RVT-Landschaftsfarbe, Metal fertig; Windows-Backends offen (Hive, Welle 2 von 0.8).
- **Branch / commit:** `release/0.8.0` @ 16335996d (Sky-Bake), dieser Commit (RVT im GI-Treffer); gepusht.
- **Verified:** Metal: Messungen oben (`scripts/he_perf_capture.py --detailed`, Catania), Zeugenbilder, Magenta-Nachweis,
  `he_tests` culling/gi/rvt grün (Drift-Wächter `test_culling.cpp:4914` erweitert).
- **Not verified:** OpenGL (macOS-GL hat keine GI; nur Konstanten geändert), D3D11/D3D12/Vulkan (nicht angefasst), echte
  Catania-Bilder mit Spiegel.
- **Traps:** `RvtVolumeB.y` = border + 256 * volume (`fmod`); Tabellen-Eintrag Bit 23 = kalt (Metal ASTC, Windows BC7 mit
  `HE_RVT_COLD_FETCH` = texelFetch statt Sampler); D3D11 hat kein Sampler-Register frei (siehe `RvtLookup.h`).
- **Next steps (Windows):** 1. Sky-Bake: nur OpenGL hat eine Reflexions-Cubemap; prüfen, ob D3D/Vulkan eine haben, sonst
  entfällt. 2. GI-Reflexionsmessung pro Pass auf D3D12/Vulkan (RTX 4070), Wasser-Doppeltrace prüfen. 3. `giRvtAlbedo` in
  `kGiReflCSHLSL` (D3D11/D3D12), `gi_refl.comp` (Vulkan), `kGiReflCS` (GL) plus Bindung der Tabellen und Pools; Zeuge
  wie oben mit `HE_DUMP_RHI=<X>`. 4. Drift-Wächter auf alle Kopien ausweiten.
