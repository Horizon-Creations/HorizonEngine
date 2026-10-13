# GI: schwarze Linien an Landscape-Kachelgrenzen, Fix (Thema 190, Schritt 2)

Stand: 13.10.2026, Zweig `claude/gi-landscape-tile-seams`. Ursache siehe
`docs/gi-landscape-tile-seams-ursache-2026-10-13.md`: ungenähte Naht zwischen Chunks
verschiedener Detailstufe, die Stufe blockiert den GI-Sonnenstrahl (Ursprung 5 cm über der
Oberfläche) und wird zur gestrichelten schwarzen Linie.

## Was der Fix tut

Die feinere Seite jeder gemischten Naht wird auf die gröbere gelegt (Geo-Mipmapping-Vernähen),
rein auf der CPU. Damit gibt es keine Stufe mehr, für GI, CSM und alle fünf Backends gleich:
kein Shader, kein Backend-Code geändert.

- `stitchTerrainChunkEdges` (`src/HE_Scene/src/TerrainMeshGenerator.cpp:499`, Doku im Header):
  Kopie einer Chunk-Stufe; Kantenvertices zur gröberen Nachbarseite bekommen Höhe und Normale der
  groben Eckpunkte aus dem **eigenen LOD0-Mesh** (gleiches `sampleField` an denselben dyadischen
  UVs wie jede gröbere Stufe, also bitgleich mit dem Nachbarn), dazwischen linear. Skirt wandert
  mit. Ecken optional festpinnbar (nur für die verschobene, verfeinerte Stufe gegen einen
  gröberen Diagonal-Nachbarn).
- `TerrainSystem::stitchSeams` (`TerrainSystem.cpp:867`), läuft direkt nach `LODSystem::update`
  (`SceneSystems.cpp:154`): Gitter je Terrain mit der Zellzahl der gewählten Stufe; feinere Seite
  zeichnet ihre genähte Kopie. Ein Mesh je Chunk (einmal registriert, danach `replaceStaticMesh` +
  `InvalidateMesh`), neu gebaut nur, wenn sich `stitchKey` (Stufe, Nachbar-Zellzahlen, Ecken)
  ändert; zurückgegeben, wenn der Chunk verschwindet (`g_stitchMeshes`, wie `g_tessMeshes`).
- `LODComponent::stitchedMeshId/stitchedLevel` (Laufzeit, nicht serialisiert): `LODSystem.cpp:56`
  zeichnet die Kopie statt der Stufe, solange `stitchedLevel == chosen`, also kein Hin und Her.
- `buildChunk`/`emptyTessLevel` erkennen eine gezeichnete genähte Refined-Stufe über
  `current == kRefined` (vorher Vergleich auf `tessMeshId`).
- Dump: `HE_DUMP_TERRAINTICK` näht mit (`EditorApplication.cpp:7899`), `HE_DUMP_NOSTITCH=1` für
  die Gegenprobe; die Logzeile zählt `stitched=`.

## Belege

| Zeuge | Ergebnis |
|---|---|
| `he_tests --source-file=*test_terrain_tessellation.cpp` (4 neue Fälle ab Zeile 616) | 18/18 grün; Gegenproben ohne Naht: Stufe > 1 cm (LOD), > 1 mm (refined) |
| `test_lod`, `test_terrain`; voller `he_tests` ohne `test_material_graph` (macOS Release) | grün, 5050/5050 Fälle |
| CPU-Probe `scripts/gi-seam-repro/seam_probe.cpp` | genäht LOD0\|LOD1/2/3 und tess×4\|LOD0: 0 Stufen-Strahlen, 0 Skirt-Treffer (vorher 15 %/35 %) |
| Metal HW-GI, Berg `MTROUGH=1 LODSCALE=0.1`, Log `LOD1=1 LOD2=11 LOD3=4 stitched=5` | Strichpixel im Nahtfenster 260 (ungenäht) → 44 (genäht; LOD0-Bild 128 = Relief); Bild im Thema `thema190_s2_stitch_ab.png` |
| Metal SW-GI (`HE_GI_FORCE_SW=1`, der Weg von D3D11/GL) | identisch 260 → 44 |

## Handoff

- **Topic / step:** 190, Schritt 2 von 4 (Fix)
- **Branch / commit:** `claude/gi-landscape-tile-seams`, Fix `061ae1bf` + Doku-Commit, gepusht
- **Done:** Nähte zwischen Chunks verschiedener Stufe (LOD-Mix, Tessellation mit/ohne
  Displacement) werden vernäht; gestrichelte GI-Linie im Metal-Zeugen weg, HW und SW.
- **Verified:** Unit-Tests (Generator + System, inkl. Sculpt-Rebuild, Freigabe, keine Neubauten
  ohne Änderung), CPU-Probe, Metal-Dumps HW/SW mit Gegenprobe.
- **Not verified:** D3D11/D3D12/Vulkan (kein Windows hier; da reine CPU-Geometrie, erwartet
  identisch), Tessellation *mit* Displacement im Renderer (nur Unit-Test), Editor-Play/gepacktes
  Spiel live, Streaming-Szenen.
- **Decisions taken:** Feinere Seite passt sich an (gröbere bleibt unverändert, keine Varianten-
  kombinatorik); fehlender Nachbar (Rand, nicht gebaut/gestreamt) = Kante bleibt; Kopie bleibt
  beim Zurückschalten liegen und wird bei gleichem Schlüssel wiederverwendet.
- **Scope assumption:** vernäht werden Chunks *eines* Landscape-Entities (die Diagnose aus
  Schritt 1). Kachelt die Szene des Menschen mehrere Landscape-Entities nebeneinander (Post 1306
  offen), läuft diese Naht durch kein `stitchSeams`-Gitter: anderer Fix.
- **Decisions suggested, not taken:** LOD-Kamera = Bildkamera (`EditorApplication.cpp:3512` im
  Play, `GameApplication.cpp:3308` `isMain`/aktiv) — nach dem Nähen nur noch kosmetisch.
  `LODSystem` hat keine Hysterese: ein Chunk genau auf `maxDistance` springt bei Kamerazittern
  hin und her, und jeder Sprung baut die genähte Kopie neu (→ `InvalidateMesh` → ganzer GI-BVH
  auf D3D11/GL/Metal-SW). Abhilfe bei Bedarf: kleiner Cache je Chunk (letzte 2 `stitchKey → UUID`)
  oder Hysterese für Terrain-LOD, wie `kTessBuildMargin/kTessReleaseMargin`.
- **Traps:** Jeder Neubau einer genähten Kopie ruft `InvalidateMesh`; D3D11/GL/Metal-SW verwerfen
  dann ihren ganzen GI-BVH (wie schon bei jedem Tess-Refill/Sculpt-Schritt) — nur bei
  LOD-Wechseln, nicht pro Frame. Die dunklen Keile unten rechts im Bergzeugen sind grobe
  LOD2-Facetten im Schatten (LODSCALE 0.1), keine Naht. doctest: `REQUIRE((e != entt::null))`.
- **Next steps:** 1. Schritt 3: Streaming/LOD-Wechsel live (Editor-Play, Kamera fliegen), Szene mit
  Displacement-Textur; an einer LOD-Grenze schweben mit `HE_GI_FORCE_SW=1` und die
  `TerrainSeams`-Zeit/Neubauten pro Sekunde im Profiler (F9) zählen (Flip-Flop oben). 2. Schritt 4: D3D11/D3D12/Vulkan auf NN-WS03 mit demselben Dump
  (`HE_DUMP_TERRAINTICK=1 HE_DUMP_LODSCALE=0.1 HE_DUMP_MTROUGH=1`, A/B `HE_DUMP_NOSTITCH=1`).
