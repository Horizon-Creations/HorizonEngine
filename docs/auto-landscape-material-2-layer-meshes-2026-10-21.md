# Auto-Landschaftsmaterial 2.0: Schicht-Meshes erzeugen und platzieren (Thema 180, Schritt 4)

Setzt §8.2 und §8.3 von `docs/auto-landscape-material-2-plan.md` um: die Meshes aus `mesh_gen`, die Platzierungsregeln als Felder an
`FoliageLayer`, ein Preset je Mesh-Sorte im Inspector. Kein neuer Streu- oder Zeichenpfad, kein Renderer angefasst (das Instancing auf den
fünf Backends ist Schritt 5).

## 1. Regeln an `FoliageLayer` (`FoliageComponent.h`, nach `lodHysteresis`)

| Feld | Vorgabe | Bedeutung |
|---|---|---|
| `slopeMin`, `slopeMax` | 0, 1 | Steilheit `1 − N.y` wie im Material (0 flach, 0,29 ≈ 45°); Kandidat bleibt bei `min ≤ s ≤ max` |
| `heightMin`, `heightMax` | ∓`kNoHeightLimit` (10 000 m) | Welt-Y des Bodens (Position der Terrain-Entity eingerechnet) |
| `weightLayer` | −1 | Malschicht 0..7 (`layerWeights`, `layerWeights2`); Behalten mit Wahrscheinlichkeit Gewicht/255. Unbemaltes Gelände zählt als Schicht 0 voll (wie der Shader) |

* `scatterLayer` (`FoliageSystem.cpp`): Zurückweisung **nach** der Dichtemaske. Höhe und Hang sind harte Grenzen ohne Würfeln; das Gewicht würfelt mit
  einem eigenen Hash (`keepSalt ^ 0x68E31DA4`), damit Maske und Gewicht nicht dieselbe Zahl ziehen. Die Überlebenden behalten Lage, Drehung und Maßstab
  des ungeregelten Layouts (Teilmenge, im Test geprüft). Ohne Regel läuft kein neuer Code.
* Steilheit: zentrale Differenzen von `terrainHeightAt` über eine Terrainzelle; am Rand verkürzt (sonst halbiert die Klemmung die Steilheit auf den
  letzten Metern: im ersten Testlauf gefunden).
* Ein Malstrich auf den Gewichten (`TerrainSystem`, Textur-Upload) markiert nur Schichten mit `weightLayer ≥ 0` als `dirty`
  (`markWeightGatedFoliage`); Rückgängig bis „unbemalt“ ebenso.
* Serializer: fünf Schlüssel, nur geschrieben wenn ≠ Vorgabe; beim Lesen geklemmt (`weightLayer` außerhalb 0..7 → −1). Eine unberührte Schicht speichert
  dieselben Schlüssel wie vorher (`test_foliage_layers.cpp:124` unverändert grün).
* Keine neuen Komponentenschlüssel: `"foliage"` steht in `isKnownComponentKey`, der X-Liste und `kComponentScopes` (`"Foliage"`) schon.
* Collab: die Felder sind Skalare im Komponentenblob (`TerrainSync::serializeWithoutGrids` legt nur die Raster beiseite), reisen also mit. Ein neues
  Raster gibt es nicht. **Eine Stelle brauchte Code:** `restoreFoliage` (`TerrainSync.cpp:~318`) behält beim Empfänger die alte Streuung, wenn die
  Streu-Eingaben gleich sind (`sameScatter`); die fünf Regelfelder fehlten dort, ein Peer, der eine Regel ändert, hätte beim Empfänger die alte
  Streuung stehen lassen. Aufgenommen, Test in `test_terrain_sync.cpp` („a peer's change to how a layer is drawn sticks, and the scatter stays“).

## 2. Die Meshes (`mesh_gen`, UUIDs ab `0x109`)

Zur Werkzeugzeit erzeugt, deterministisch (Hash-Rauschen, kein Zufallsstrom; zweiter Lauf byte-gleich), je Sorte Detailstufen mit **gleichem Rauschfeld
dünner abgetastet** (Silhouette bleibt): `Tuft` 9/5/3 Klingen, `Pebble` 320/80 Dreiecke, `RockA/B/C` je 1280/320/80, `SnowDrift` 1280/320.
Konstanten in `DefaultAssets.h` (`kEngineTuftMeshId`, … je Stufe 0; Stufe n = `hi + n`), im Generator per Laufzeitprüfung gegen die Tabellenposition
abgesichert. 16 neue `.hasset` in `EditorDeps/EngineContent/Meshes` (344 KB). Die neun alten Dateien wurden **nicht** neu geschrieben (ein
Neulauf von `mesh_gen` erzeugt sie anders als eingecheckt, ab Byte 37; `CrossCard` ist identisch; ungeklärt, ob Versionsfeld).

## 3. Presets (`LayerMeshPresets.h`, Inspector „Mesh Preset“ + „Apply Preset“)

`applyLayerMeshPreset(layer, kind)` setzt Mesh, Stufen, Dichte, Maßstab, Schatten/AO und die Regeln passend zu den Material-Vorgaben (Fels ab 0,12;
Schnee ab 60 m): Büschel `slopeMax 0,12`; Kiesel `slopeMax 0,25`; Felsen `slopeMin 0,15`, werfen Schatten; Schneewehe `heightMin 66`, `slopeMax 0,35`.
Seed und Maske der Schicht bleiben. Ein Mesh je Schicht; eine zweite Felsform ist eine zweite Schicht (`addLayer()` vergibt eigene Seeds).
Der Knopf lädt die Mesh-Dateien (Engine-Dateien sind nicht von allein resident).

## 4. Tests (`tests/test_foliage_rules.cpp`, 11 Fälle)

`Foliage rules: a layer without rules scatters as it always did` · `…: slope - Slope Min keeps a rock off the flat, Slope Max keeps grass off the hill`
· `…: height - the limits are in world metres, the entity's own height included` · `…: paint weight - the layer grows where its paint layer is, and
only there` · `…: a weight-gated layer re-scatters after a paint stroke, the others do not` · **`…: the same seed gives the same instances, another seed
does not`** (zwei Streuungen bitgleich `first[i] == second[i]`, Reihenfolge inklusive; anderer Seed → andere Posen; Regeln gelten auch dort) ·
`…: two layers with one mesh and different seeds do not grow in the same spots` · `…: a scatter creates instance data, never an entity per instance`
(`in_use()` vor/nach gleich, Store hat weniger Buckets als Instanzen) · `…: the keys are saved only when set, and a scene from before them loads as no
rule` · `Layer meshes: every kind and detail level is a committed engine file with the id the code names` · `Layer meshes: a preset fills the layer and
its rules put it on the ground that suits it`.

## Handoff

- **Topic / step:** 180, step 4 of 10
- **Branch / commit:** `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit` (see `git log`; pushed: see the result post)
- **Done:** a layer can be tied to slope, height and a paint layer; 16 procedural layer meshes ship as engine files; the inspector has the rules (with help) and
  a preset picker. Scatter stays instance data in the bucket store, deterministic per layer seed.
- **Files touched:** `FoliageComponent.h:~90` (fields), `FoliageSystem.cpp` (`steepnessAt`, `layerWeightAt`, rules block in `scatterLayer`), `TerrainSystem.cpp`
  (`markWeightGatedFoliage`), `SceneSerializer.cpp:~181,~222`, `LayerMeshPresets.h` (new), `DefaultAssets.h` (ids), `MeshGen/main.cpp` (generators),
  `InspectorPanel.cpp` (Placement + Preset rows), `EditorHelp.cpp` (8 entries), `tests/test_foliage_rules.cpp`.
- **Verified:** `he_tests --source-file='*test_foliage_rules.cpp'` 11/11 (macOS release); `editor_help_audit.py --check` 1212/1212; `mesh_gen` run twice, byte-equal;
  full `he_tests` run: see the result post.
- **Not verified:** the meshes were never drawn (no image: step 5 draws them instanced and shows them); the inspector rows were compiled, not looked at
  (no UI test added); the editor target `HorizonEditor` was not built (its sources compile in `he_tests`); a window/GPU run.
- **Decisions taken:** rules only (slope, height, paint weight); `surface` enum and its CPU noise twin (`heHashI2`/`heFbmI`, plan §8.3) and the „Match Auto
  Material“ button left for later; one mesh = one layer (plan D7), no mesh sub-list; mesh levels are separate files with consecutive ids, not LODs inside a
  file; rules are scatter inputs (set `dirty`), not part of `settingsKeyOf`.
- **Decisions suggested, not taken:** a surface material for the meshes (plan §8.5 built-in PBR colour per layer; presets leave `materialAssetId` empty, so
  the meshes draw with the mesh default material); a dirt-noise twin so pebbles stay in the dirt patches instead of all flat ground.
- **Pending:** step 5 (instancing wiring + draw counts + images on all five backends), material for the meshes, look of the meshes (UV is a planar
  projection, rocks untextured).
- **Traps:** a header edit in `DefaultAssets.h` or `FoliageComponent.h` rebuilds most of the tree (done once here, ~15 min with ccache); `mesh_gen` rewrites the
  old primitives differently from the committed files, copy only the new ones; `in_use()` of the entt storage is deprecated but used elsewhere in the tests.
- **Next steps:** 1. in the editor add a Foliage component on an Auto Landscape terrain, apply `Rock A`/`Grass Tuft`, check `HE_FOLIAGE_STATS` draw counts;
  2. step 5 backends; 3. material for the layer meshes (plan §8.5).
