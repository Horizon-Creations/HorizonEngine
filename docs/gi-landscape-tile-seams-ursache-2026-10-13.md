# GI: schwarze Linien an Landscape-Kachelgrenzen, Ursache (Thema 190, Schritt 1)

Stand: 13.10.2026, Zweig `claude/gi-landscape-tile-seams` (auf `release/0.7.0` = `ff3091fd`).
Gemessen auf dem MacBook Air (Metal, HW-RT und erzwungener SW-Pfad), Release-Editor aus diesem
Worktree. D3D/Vulkan sind hier nicht messbar. Kein Fix, nur Diagnose plus Werkzeug.

## Kurzfassung

1. **Die Linie entsteht nur an einer Naht zwischen zwei Chunks verschiedener Detailstufe**
   (LOD0|LOD1, LOD1|LOD2 …, oder tessellierte Stufe mit Displacement neben LOD0). Die Chunks werden
   nicht vernäht: Die gröbere Kante läuft gerade von Eckpunkt zu Eckpunkt, die feinere folgt dem
   Höhenfeld. Pro grobem Segment entsteht eine linsenförmige Stufe (an den gemeinsamen Eckpunkten
   null, dazwischen bis `h'' · Δ² / 8`). Diese Stufe ist echte Geometrie und wirft einen echten,
   wenige Zentimeter breiten Schatten auf die tiefere Seite.
2. **Die GI macht daraus die gestrichelte schwarze Linie.** Der raygetracte Sonnenstrahl startet
   5 cm über der Oberfläche (`gi_shadow.comp:168` und alle Zwillinge, unten). Wo die Stufe höher
   ist als diese 5 cm, trifft er die Nachbarkachel, die Maske wird 0. Weil die Stufe pro grobem
   Segment an- und abschwillt, wird nur ein Teil jedes Segments getroffen: Striche in festem
   Weltabstand, genau wie im Screenshot. Die Halbauflösungs-Maske wird bilinear hochgezogen
   (`scene.frag:513`), jeder Strich wird 2–4 px breit. Die CSM ohne GI zeigt dieselbe Naht als
   dünne durchgehende Linie (kein 5-cm-Schwellwert, dafür weicher und meist unter dem Bias).
3. **Gleiche Detailstufe beidseits ist sauber.** Kanten-Vertices sind auf der CPU bitgleich
   (gleiche u-Werte, gleiches Feld, Normalen aus dem globalen Feld), es gibt keine Stufe.
4. **Ausgeschlossen:** Skirts (Pixel mit und ohne Skirts identisch, siehe Zeuge D), Half-Float-
   Position (seit Thema 159 RGBA32F auf allen Backends), veraltete GI-BLAS nach
   `replaceStaticMesh` (Vulkan räumt `m_giBlasCache` in der Mesh-Invalidierung,
   `VulkanRenderer.cpp:7614`), Cull-Mismatch (GI-G-Buffer `VK_CULL_MODE_NONE`, gleiches
   Aktuell-LOD-Mesh wie der Scene-Pass), Probe-Abdeckung (die Probe-Gewichte werden
   renormiert, die Dunkelheit kommt aus der Sonnenmaske).

## Warum die Naht beim Menschen nah an der Kamera liegt (Kandidaten, im Code belegt)

Mit Standardwerten wechselt ein Chunk erst bei 6 Chunkbreiten × `lodDistanceScale` die Stufe
(`TerrainSystem.cpp:316-325`), gemischte Nähte liegen also weit weg. Nah an die Kamera kommen sie so:

- **Spielen im Editor:** Der Viewport rendert durch die Spielkamera (`ViewportPanel.cpp:1020`), das
  LOD misst aber von der geparkten Editor-Kamera (`EditorApplication.cpp:3512`,
  `m_editorCamera.position()`; kein Setter synchronisiert sie im Play). Um den Spieler liegen dann
  beliebige LOD-Grenzen.
- **Gepacktes Spiel:** Das LOD nimmt die *erste* `CameraComponent` der View
  (`GameApplication.cpp:3308-3316`), der Renderer die aktive mit Vorrang für `isMain`
  (`RenderExtractor.cpp:97-104`). Bei mehr als einer Kamera in der Szene misst das LOD von der
  falschen.
- **Inspector:** kleines `lodDistanceScale`, oder Tessellation mit Displacement (die verfeinerte
  Stufe liegt per Definition um die Kamera, `tessellationDistance` 50 m). Tessellation *ohne*
  Displacement ist harmlos: Catmull-Rom weicht von linear nur um mm–cm ab (Zeuge: max 1/255).

Welcher davon beim Menschen greift, ist offen (Frage im Thema, Post 1306).

## Zeugen

**A. CPU-Probe** `scripts/gi-seam-repro/seam_probe.cpp` (Engine-`generateTerrainChunkMesh` +
`GiBvh`, der Strahl aus `gi_shadow.comp`, 36 Sonnenrichtungen, Punkte ≤ 35 cm von der Naht):

| Naht | Strahlen | blockiert (nur Oberfläche) | nur durch Skirts | Stufenpixel blockiert |
|---|---|---|---|---|
| LOD0 \| LOD0 | 389 814 | 31 639 | 0 | keine Stufe |
| LOD0 \| LOD1 | 389 814 | 37 290 (+1,5 %) | 130 | 14,8 % |
| LOD0 \| LOD2 | 389 814 | 59 439 (+7,1 %) | 688 | 34,8 % |
| tess ×2/×4 \| LOD0 (ohne Disp.) | 389 814 | ≈ 32 100 | 13–19 | 10–11 % |

**B. Renderer (Metal)** mit neuem Dump-Schalter `HE_DUMP_TERRAINTICK=1` (+ `HE_DUMP_LODSCALE`,
`HE_DUMP_TESS`): Ohne ihn tickt der Dump die Welt nie, alle Chunks bleiben LOD0, eine gemischte
Naht gibt es im Dump schlicht nicht. Szene `HE_DUMP_MOUNTAINTEST=after`, Kamera (15, 360, 45),
Pitch −40, TOD 0,4, `scripts/gi-shadow-repro/cap_metal.sh`:

- `LODSCALE=0.1` → Log `LOD1=1 LOD2=11 LOD3=4`: Naht x = 0 als **gestrichelte** Linie mit GI,
  als dünne durchgehende Linie ohne GI.
- `LODSCALE=1` → `LOD0=16`: an derselben Stelle keine Linie (GI an und aus).
- Mit `HE_DUMP_MTROUGH=1` (rauer) werden die Striche dichter und dunkler: Strichdichte folgt der
  Stufenhöhe. Bilder im Thema (Datei-Anhänge `thema190_seam_witness.png`, `thema190_rough.png`).
- **D. Skirt-A/B** (temporärer, nicht eingecheckter Schalter in `buildChunkMesh`): 289 statt
  353 Vertices je Chunk, Pixel an der Naht identisch. Bei den getesteten Stufenhöhen (Stufe
  unter einem Pixel) sind die dunklen Pixel der Schlagschatten der Stufe, nicht die Skirt-
  Füllung. Werden Stufen pixelgroß, kommen laut Probe die Skirt-Füllpixel dazu (Spalte rechts),
  dieselbe Wurzel.

## Fundstellen

- Kein Vernähen der Kanten: `src/HE_Scene/src/TerrainMeshGenerator.cpp` `buildChunkMesh`
  (Gitter ~299-330, Skirt 332-372); LOD-Kette `TerrainSystem.cpp:299-327`.
- Strahlursprung `P + N·0.05`: `shaders/gi_shadow.comp:168`, `gi_shadow_hw.comp:130`,
  `gi_shadow_hw.hlsl:126`, `D3D_Shared/HlslSources.h:949`, `OpenGLRenderer.cpp:2069`,
  `MetalRenderer.mm:2492` (HW) und `:3510` (SW).
- Maske bilinear auf voller Auflösung: `scene.frag:513` (D3D/Metal/GL gleichartig).
- LOD-Kamera ≠ Bildkamera: `EditorApplication.cpp:3512`, `GameApplication.cpp:3308`.

## Handoff

- **Topic / step:** 190, Schritt 1 von 4 (Ursache)
- **Branch / commit:** `claude/gi-landscape-tile-seams` (siehe `git log`, gepusht)
- **Done:** Ursache eingegrenzt und im Metal-Renderer reproduziert: ungenähte Naht zwischen
  verschiedenen Detailstufen, die GI mit 5-cm-Ursprung zeichnet die Stufe gestrichelt schwarz.
- **Files touched:** `src/HE_Editor/EditorApplication.cpp` (Block `HE_DUMP_TERRAINTICK` vor
  `pushEnvironment` im Dump), `scripts/gi-seam-repro/seam_probe.cpp`, dieses Dokument.
- **Verified:** CPU-Probe (Tabelle A); Metal-Dumps B/D (mixed vs same, GI an/aus, rau, ohne
  Skirts) mit Log-Zeile `HE_DUMP_TERRAINTICK chunks …`; Kontrolle Neubau vs. Altbild max 1/255.
- **Not verified:** D3D11/D3D12/Vulkan (kein Windows hier); die Szene des Menschen (Größe, LOD-
  Skala, Tessellation, Kameras unbekannt); Intensität wie im Screenshot nur qualitativ.
- **Decisions taken:** Kein Engine-Verhalten geändert, nur ein Dump-Schalter.
- **Decisions suggested, not taken:** Fix auf der Landscape-Seite: die feinere Kante auf die
  gröbere Nachbarkante herunterziehen (Kantenvertices auf die lineare Interpolation der
  Nachbarstufe setzen, je Nachbar-LOD, wie bei Geo-Mipmapping/CDLOD), damit verschwindet die
  Stufe auf allen Backends und für CSM wie GI. Ein GI-seitiger Patch (größerer Offset an Nähten)
  versteckt es nur auf einem Pfad. Unabhängig davon: LOD-Kamera = Bildkamera (Editor-Play, Spiel
  `isMain`/aktiv), das nimmt die Nähte aus der Nähe der Kamera.
- **Pending:** Antwort des Menschen (Post 1306); Schritte 2–4.
- **Traps:** Dumps ticken LOD/Tessellation nie (`HE_DUMP_TERRAINTICK` nehmen); Built-in-Terrain-
  und `plaingraph`-Material sättigen bei ~230, Nähte nur per Diff/Crop sichtbar; GI=1-Dumps auf
  Metal sind nicht bitgleich (≤1 LSB). Release-Build dieses Worktrees: `out/build/release`
  (Ninja aus CLion, `_deps` des Haupt-Checkouts), Deploy in `out/deploy/Editor`.
- **Next steps:** 1. Nähen in `buildChunkMesh`: Parameter „Nachbar-Zellzahl je Seite", Kanten-
   Höhen auf das grobe Raster interpolieren; `TerrainSystem` muss beim LOD-Wechsel eines Chunks
   die Nachbarn neu wählen lassen (Varianten je Nachbarkombination oder Kanten-Patch).
   2. LOD-Kamera an die Bildkamera binden (`EditorApplication.cpp:3512` im Play die Spielkamera,
   `GameApplication.cpp:3308` `isMain`/aktiv wie `RenderExtractor`). 3. Zeuge B als Abnahme:
   `LODSCALE=0.1 MTROUGH=1` muss mit GI so sauber sein wie `LODSCALE=1`.
