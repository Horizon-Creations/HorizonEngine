# GI / Landscape: Optimierungs-Backlog (27.10.2026)

Sammelstelle für alles, was nach dem Audit (`docs/landscape-streaming-lod-audit-2026-10-27.md`) offen ist oder bewusst weggelassen wurde.
Aufwände sind Schätzungen, nicht gemessen. Stand der Arbeit: Höhenfeld-Schatten in D3D11, Vulkan (Software-Kernel), OpenGL (Pixelbeleg: D3D11 0.16,
Vulkan-SW 0.13, OpenGL 0.295 mittlere Abweichung gegen 8.7 / 8.7 / 11.7 der Negativkontrollen); Caster-Auswahl nach Distanz; LOD-Hysterese;
Vulkan-Retire-Liste; eigene, ausgedünnte Schatten-Instanzliste auch bei GI-Reflexionen.

## Vor allem anderen: Messung auf der Maschine des Nutzers
Auf der RTX 4070 (NN-WS03) messe ich GIShadow ~1.2 ms; die 5.3 ms des Nutzers sind nicht reproduzierbar. Erst ein Profil-JSON von dessen Maschine
(`he_perf_capture.py ... --detailed`, GI an, Kamera bewegt) sagt, welcher GI-Pass dort dominiert. Danach die Punkte unten priorisieren.

## Backlog

| # | Punkt | Wirkt auf | Aufwand | Erwarteter Nutzen | Status |
|---|---|---|---|---|---|
| 1 | **Höhenfeld im Vulkan-Hardware-Pfad (Ray Query):** TLAS ohne Terrain-Chunks aufbauen, im Ray-Query-Shader nach der TLAS-Abfrage `hfAnyHit` aufrufen (wie `GI_OCCLUDED` im Software-Kernel). Vorher messen: GI-Zeit mit/ohne Chunk-Instanzen in der TLAS (Debug-Variable). | Vulkan auf RTX-Karten (Standardpfad) | 1-2 Tage, Messung 0.5 Tag | unbekannt, vermutlich klein (Hardware-BVH ist billig) | nicht begonnen, bewusst weggelassen |
| 2 | **Höhenfeld in D3D12 (DXR):** DXR-Kernel ist eine eigene Datei, gleiche Trennung (TLAS ohne Chunks + `hfAnyHit`). | D3D12 | 0.5-1 Tag | wie 1 | nicht begonnen |
| 3 | **Höhenfeld in Metal** (`MetalRenderer.mm`, handgepflegte Kopie von `kSkyFS`/GI-Kernel). Auf dem Mac bauen und mit dem Witness prüfen. | Metal | 1 Tag | Parität | Metal-Änderungen des Caster-Filters sind ungebaut |
| 4 | **Höhenfeld für Proben und Reflexionen:** Closest-Hit im Feld (Pyramide + Bisektion), Normale aus dem Gefälle, Material am Treffer (Landschaftstabelle `GILandGpu`, UV aus lokaler xz-Position) in `kGiProbeCSHLSL`, `kGiReflCSHLSL` und den GLSL/MSL-Kopien; danach Landschaft aus der Hauptliste, 500-m-Grenze entfällt auch bei Reflexionen. | GL, Vulkan-SW, Metal, D3D12 (D3D11 hat GI-Reflexionen abgeschaltet) | 3-5 Tage | Instanzzahl 257 -> wenige, Reflexionen zeigen Fernlandschaft ohne Kosten | nicht begonnen; Risiko: Raster-Mesh und Feld stimmen an LOD/Tess/Nähten nicht exakt überein |
| 5 | **Mehr als eine Landschaft:** Höhenfeld-Pfad greift nur bei `heightfields.size() == 1`; sonst Fallback auf Meshes. Mehrere Felder (Array/Atlas, Auswahl pro Strahl). | alle | 1-2 Tage | nur bei Szenen mit mehreren Landschaften | nicht begonnen |
| 6 | **Kamerazentrierte Proben-Kaskaden + GI-Schatten in halber Auflösung** (Schritt 2 des ursprünglichen Vorschlags). | alle | 3+ Tage | Messung zeigte keinen akuten Engpass; Risiko für das Bild | zurückgestellt |
| 7 | **Fern-HLOD:** Chunk-Raster ist auf 16x16 begrenzt (8000 m / res 4097 = 257 Objekte, 40-70 Draws, 4-5 ms CPU). Lohnt erst, wenn die Grenze fällt oder mehrere Landschaften dazukommen. | CPU/Draws | 3+ Tage | derzeit keiner | bewusst übersprungen |
| 8 | **Stitch-Cache** für die Nahtmeshes (gemessen ~0.1 Stitch/Frame, ~250 KB je). | CPU/Upload | 1 Tag | vernachlässigbar | übersprungen |
| 9 | **OpenGL-Schatten-Test mit zweiter Kamera** hat keinen Schatten gezeigt; die Distanz-Negativkontrolle (`HE_GI_CASTER_RANGE=5`) ist auf GL wirkungslos (Sonnenstreifen-Regel behält die Chunks). Nur Dokumentation, Mute-Kontrolle ersetzt sie. | Test | 0 | - | erledigt (Mute-Hook) |

## Hinweise für die Umsetzung
* Höhenfeld-Quelle: `TerrainComponent::builtField` (+ `builtFieldRes`, `builtFieldVersion`) -> `RenderExtractor` -> `RenderWorld::heightfields`; Pyramide: `GiHeightfield.h`.
* Kernel-Kopien: D3D11/D3D12 `HlslSources.h`, Vulkan `shaders/gi_shadow.comp`, OpenGL `kGiShadowCS`/`kGiHeightfieldGLSL`, Metal `MetalRenderer.mm`. Drift-Guard: `tests/test_culling.cpp`.
* Prüfen ohne Fenster: Witness `HE_DUMP_GIHFTEST`, `scripts/he_gi_hf_check.py --rhi <X>` (Vulkan mit `HE_GI_FORCE_SW=1`; OpenGL-Negativkontrolle `HE_GI_HEIGHTFIELD_MUTE=1`);
  Messung mit `scripts/he_perf_capture.py` (hidden window, `HE_PERF_CAM_FLY`). Shader-Quellen der D3D/GL-Backends werden zur Laufzeit kompiliert: extern validieren (Memory-Notiz).
* Schalter: `HE_GI_HEIGHTFIELD=0` (aus), `HE_GI_CASTER_RANGE=<m>` (0 = aus, Standard 500).

## Handoff
* Verifiziert: Tests culling/gi_bvh/lod/terrain_tessellation 120/120; Pixelvergleiche D3D11, Vulkan-SW, OpenGL; Messungen D3D11/Vulkan/OpenGL (siehe Audit-Dokument).
* Nicht verifiziert: Metal (ungebaut), D3D12 (keine Höhenfeld-Änderung), Pixelvergleich mit GI-Reflexionen an, Vulkan-Hardware-Pfad mit Höhenfeld.
* Entschieden: Höhenfeld nur für den Sonnen-/Lichtschatten; Proben und Spiegel behalten die vollen Chunk-Meshes. Nur vorgeschlagen: Punkte 1-8 oben.
* Nächste Schritte in Reihenfolge: Messung vom Nutzer holen; Punkt 1 messen (Chunks aus TLAS); dann je nach Ergebnis 1/2/3 oder 4.
