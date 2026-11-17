# Entity-Cooking Schritt 8: LOD-Entities kochen

Stand 17.11.2026, Thema 343. Setzt auf `docs/entity-cooking-cooked-static-2026-11-16.md` (Block, Fast-Pfad) und
`docs/entity-cooking-uncook-2026-11-16.md` (Entkochen) auf.

## Was gebaut ist

- **Regel 8 enger** (`src/HE_Scene/src/CookAnalysis.cpp:64-68`): nur `LODComponent::refinedMeshId != {}` blockiert noch
  (`CookBlocker::Lod`). Mehrere Level kochen. `refinedMeshId` bleibt Blocker: `TerrainSystem` setzt/löscht es zur Laufzeit
  per Raw-Write (Tessellation), das kann keine Zeile halten (Terrain-Tessellation ist laut Schritt außerhalb des Umfangs).
- **Gekochte Zeile je Level** (`CookedStatic.h`: `lodFirst/lodCount` je Zeile, `levelMeshId/levelMaterialId/levelBounds`
  flach): das, was der Walk für JEDES Level bauen würde (Mesh, Ganz-Zeichnen-Material, Weltbounds). Weltmatrix, `lod`,
  Schatten-Flags bleiben in den normalen Zeilenfeldern.
- **Settle** (`RenderExtractor.cpp` `settleLodRow`, Phase 3, seriell): alle Level resident und einsektionig → `Ready`; ein
  Level nicht resident → `Pending` (nächster Frame neu); mehrsektionig / Mesh-Id leer / gezeichnetes Mesh ist keines der
  Level / refined oder stitched gesetzt → `Walk`. Je Level über `resolveEntitySlots` (dieselbe Funktion wie der Walk,
  kein zweiter Regelsatz); `localBoundsOf` ist neu herausgezogen und vom Walk mitbenutzt.
- **Fast-Pfad** (Phase 2): `lod.current` live lesen, `levelMeshId[first+current]` gegen `MeshComponent::meshAssetId`
  prüfen, Level-Daten kopieren. Passt das Mesh nicht (Raw-Swap), läuft die Entity im Walk und wird neu gesettelt.

## Entscheidung gegen den Wortlaut (Queen informiert, hive_request 181)

Der Fast-Pfad **rechnet die Distanzauswahl nicht selbst nach**. `LODSystem::update` hat Zustand (Hysterese auf
`LODComponent::current`, `LODSystem.cpp:106-111`) und schreibt `MeshComponent`/`current` für andere Leser; ein zweiter
Wähler im Extract würde im 5-%-Band abweichen und könnte mit einer anderen Kamera rechnen als der Tick. Stattdessen indexiert
die Zeile nach LODSystems Ergebnis: bitgleich per Konstruktion, `LODSystem` unverändert, Entity bleibt lesbar/frisch. Preis:
der Update-Pfad (`LODSystem::update`) wird nicht billiger; der Gewinn liegt im Extract.

## Geprüft

- `he_tests` (Release, Metal-Mac): `test_cooked_static` 22 Fälle (drei neue LOD-Fälle: Kamerafahrt durch beide Grenzen und
  das Hysterese-Band, vor/zurück, jedes Frame Feld für Feld gegen einen Extractor ohne Cooking, alle 3 Level gesehen, 12/12
  Zeilen `Ready` nach der Fahrt; Level nicht resident → `Pending`; mehrsektionig → `Walk`; refined → keine Zeile;
  Transform-Schreibzugriff und Raw-Mesh-Swap zeigen sich im nächsten Extract), `test_cook_analysis` (Regel 8 angepasst),
  `test_lod`, `test_culling`, `test_world_scale`, `test_layer_mesh_instancing`, `test_terrain_sync` grün.
- **Flaky-Test behoben:** `CookedStatic cells: a streamed cell...` (Schritt 7) schlug unter Last ~50 % fehl (`c.size() ==
  rowsLoaded`, Werte 10/15/5): er zählte Zeilen, solange Nachbarzellen noch auf dem Pool luden. Jetzt wartet er auf
  `stats().inFlight+ready+building == 0` (10/10 grün bei Last 14). Mit LOD hat das nichts zu tun.
- Messung (temporärer Bench, nicht eingecheckt): 20 000 LOD-Entities mit 3 einsektionigen Leveln, 40 Extracts je Lauf,
  Median, he_tests Release, Last 14 (andere Bees bauen, also nur das Verhältnis zählt). `extract` ms/Frame aus gegen an,
  3 Runden: 31,1 / 14,1; 21,5 / 10,1; 32,2 / 9,9: **ca. 2 bis 3,3x schneller** gegenüber dem unkochten LOD-Pfad.
  Nicht gemessen: Referenzwelt/F9-Capture mit LOD-Entities (die Referenzwelt hat keine), `LODSystem`-Scope (unverändert).

## Nicht geprüft

- **Pixel-A/B** auf einem Editor-Bild: nicht gelaufen (kein Editor-Build in dieser Sitzung). Ersatz: die Tests vergleichen
  jedes `RenderObject`-Feld bitgleich; Bilder aus gleichen Objekten sind gleich, aber ein echtes A/B steht aus.
- Vulkan/D3D, Windows/Linux (Extractor ist backend-unabhängig, nur Metal-Mac gebaut), Play-Mode, Auto-LOD-Szene.

## Handoff

- **Topic / step:** Thema 343, Schritt 8. Code: `CookedStatic.h/.cpp`, `RenderExtractor.cpp` (`settleLodRow`, Phase 2/3),
  `CookAnalysis.cpp`; Tests `test_cooked_static.cpp`, `test_cook_analysis.cpp`.
- **Pending:** (1) Pixel-A/B mit `HE_COOK_STATIC=1` auf einer Szene mit Auto-LOD-Meshes. (2) LOD-Rows mit mehrsektionigen
  Leveln bleiben Walk (sehr üblich bei echten Auto-LOD-Ketten?): wenn ja, braucht die Zeile ein Sektionsarray je Level.
  (3) Nach einem Entkochen (Transform-Write) setzt `LODSystem` bei seinem alten `lod.current` fort: das ist ohnehin so, kein
  neuer Effekt. (4) `LODSystem::update` komponiert die Parent-Kette je Entity weiter; für gekochte Zeilen könnte es
  `world[row][3]` lesen (Update-Gewinn), bitgleich nur nach Prüfung gegen `worldPositionOf`.
- **Traps:** `refinedMeshId` bleibt Blocker. Ein LOD-Row-Settle verlangt, dass das heute gezeichnete Mesh eines der Level
  ist, sonst `Walk` bis zum nächsten Epochenwechsel. Level-Tabelle wird bei gleichem Level-Anzahl in place neu geschrieben.
