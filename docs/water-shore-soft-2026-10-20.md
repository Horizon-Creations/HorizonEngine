# Weiches Ufer (Thema 206, Schritt 2)

Stand 20.10.2026, Mac M5, Release-Editor des Zweigs. Basis: `docs/water-realistic-baseline-2026-10-20.md`,
`docs/water-shader-plan.md` §6, §13.

## Befund: kommt ein Uferabstand im Material an?

Nein. Das Material-Fragment liest keine Szenentiefe (`heGBDepth` gibt es nur im Deferred-Resolve/SSR,
`MaterialShaderLibrary.cpp` ~1212/~1602), `VertexColor` ist ein Uniform (`vColor = u.color.rgb`, `:142`,
`:1149`), das Wassermesh trug nur Position, Normale +Y und Welt-UV (`WaterMesh.cpp`), und das Blatt ist das
Konturpolygon ohne Innenpunkte: jeder Eckpunkt liegt AM Ufer, eine Tiefe je Eckpunkt wäre überall gleich.
Die Queen hat deshalb Option A freigegeben (Tiefe ins Mesh backen, keine Renderer-Änderung).

## Was gebaut ist

1. **Tiefe je Eckpunkt** (`WaterMesh.cpp`, `SurfaceOptions::shoreFade`, in `WaterSurface.cpp` an). Aus dem
   Gitter `Lattice::g` (Wasser über Grund an den Gitterecken, bilinear: `shoreProximityAt`) wird
   `e = 1 - Tiefe / kShoreFadeDepth` (0..1, 1 an der Wasserlinie, 0 ab 1 m Wasser; Tiefe über dem GEZEICHNETEN
   Blatt, also `g - Überstand`). Ohne Beschneidung (`clipToGround` aus) und ohne `shoreFade`: Mesh und Hash
   bit-gleich wie vorher.
2. **Kanal = Eckpunkt-Normale.** `normal = (kShoreNormalGain * e, 1, 0)` mit `kShoreNormalGain = 1/32`
   (`Physics/WaterWaves.h`, damit `mat_gen` dieselben Konstanten sieht). Die einzigen Vertex-Kanäle sind
   Position/Normale/UV in allen fünf Backends; die UV tragen die Welt-Kachelung für fremde Materialien, die
   Normale war konstant. Maximal 1,8° Neigung am Ufer. Eine Plane hat `n.x = 0` = `e = 0` = offenes Wasser.
3. **Verfeinerung** (`refineShore`): Kantenmitten werden geteilt (Rot-Grün, Nachbarn nach Position der
   Endpunkte, also ohne T-Stücke, auch an Brücken zu Inseln), solange `e` in der Kantenmitte oder im
   Dreiecksschwerpunkt mehr als 0,03 (3 cm Wassertiefe) von der Geraden abweicht; nie feiner als eine halbe
   Gitterzelle, höchstens 400000 Eckpunkte, 24 Durchgänge. Eine ebene Böschung braucht nur die Teilungen, die
   einen Eckpunkt dorthin legen, wo `e` bei 0 sättigt. Beispiele: Mulde 607 -> 1898 Dreiecke (res 256),
   80 -> 1236 (res 33); See r=200 m mit sanfter Böschung 874 -> 1292, 22 ms Bauzeit.
4. **Material** (`MatGen/main.cpp`, Block "Soft shore" ~890): `e = n.x / n.y / gain`, Tiefe `(1-e) * 1 m`.
   Breite `w` und Schaum kommen aus den freien Komponenten von `Turbidity` (jetzt `ParamVec4`,
   Default `0.45, 4.0, 0.35, 0.7`; alle 16 Parameter-Slots sind belegt): z = Uferbreite in m (0 = hart, bis 0,7),
   w = Uferschaum 0..1.
   - Alpha: `alpha * smoothstep(0, w, Tiefe + wobble)`, `wobble` = Foam-Lace (±0,3 w), damit die Linie organisch
     wird, nicht die Kontur einer Gitterzelle.
   - Schaumsaum: Band bei 0,04..0,5 der Breite, vom Lace aufgebrochen, ist deckend (`lerp(alpha, 1, 0.85 * Schaum)`)
     und geht in `foamAmt` (Farbe, Rauheit).
   - Wellenneigung: das `n.x` des Meshes wird in `tilt` mit abgezogen, damit es nicht als Wellenhang wirkt.
   - Kontrolle: `gate = step(1e-4, e)` schließt den Zweig auf offenem Wasser; Rampe = 1, Schaum = 0.
5. **Zeugen/Skript:** `scripts/shore_edge_sharpness.py` (Kantenbreite je Bildzeile, 20..80 % Luma).

## Messung (Release-Editor, Rezept `scripts/water_baseline_shots.py`, 1280x720, Zeugen `docs/water-realistic-baseline/after-*.png`)

| Ansicht | Metal | OpenGL |
|---|---|---|
| `open`, `open-hi` (Kontrolle, `WATERTEST=floor`) | md5 **gleich** (ecdaf6f1, a17f53fb) | md5 **gleich** (bf28f2ec, 4f1039d8) |
| Kantenbreite `beach`, Zeile 300 / 400 | 2 -> 86 / 94 px | 2 -> 86 / 94 px |
| `beach-smooth`, Zeile 300 / 400 | 2 -> 91 / 85 px | 2 -> 84 / 85 px |
| abweichende Pixel `beach` / `beach33` / `beach-smooth` / `beach-sheet` | 28,7 / 29,3 / 28,8 / 28,8 % | 28,9 / 29,6 / 29,0 / 29,1 % |
| abweichende Pixel `shore` / `shore33` / `shoresheet33` | 2,7 / 3,0 / 2,9 % | 2,7 / 3,0 / 2,9 % |

Gesehen (Metal und GL gleich): der Zickzack von `beach` ist weg, die Kante läuft über eine Rampe (8,75 m
Breite bei Neigung 0,04 und 0,35 m Tiefe) in einen unregelmäßigen Schaumsaum aus. Die steile Mulde (`shore`)
hat eine schmale weiche Kante. Metal und GL liegen zueinander wie vorher (mittlere Abweichung Metal/GL `beach` 0,36 -> 0,34,
`shore33` 0,57 -> 0,55 von 255, also kein neuer Backend-Unterschied). Zwei Läufe je Ansicht: Kontrolle und `*-sheet*` md5-gleich, `shore`,
`shore33` (GL), `beach-smooth` (GL) weichen um wenige Pixel ab (der Rauschboden aus Schritt 1).

Tests: `he_tests --source-file='*test_water_surface.cpp'` (51 Fälle, drei neue: Wasser ohne Boden, ebene Böschung
mit Tiefe linear/ohne T-Stücke, Insel mit Brücke, großer See), `*test_engine_materials.cpp` (11), alle
`*water*`-Dateien (248), `*test_terrain*`, `*test_material_graph.cpp`, `*test_mcp_tools_material.cpp`,
`*test_underwater_camera.cpp` grün; `editor_help_audit.py --check` 1204/1204.

## Handoff

- **Topic / step:** 206, Schritt 2 von 5
- **Branch / commit:** claude/realistischerer-wasser-shader-wellen-normals-transparenz-ufe (Commit siehe `git log`)
- **Done:** Wasserkante zeichnet eine weiche, ausgefranste Rampe mit Schaumsaum statt des Zickzacks (Metal + GL).
- **Files touched:** `src/HE_Scene/src/WaterMesh.cpp:166,210,624,707` (Tiefe, Verfeinerung), `WaterMesh.h`,
  `WaterSurface.cpp:212`, `src/HE_Core/include/Physics/WaterWaves.h` (Konstanten), `src/HE_Tools/src/MatGen/main.cpp:~890`,
  `EditorDeps/EngineContent/Materials/Water.hasset`, `tests/test_water_surface.cpp`, `tests/test_engine_materials.cpp`,
  `EditorHelp.cpp` (Tooltip), `he-docs.json` (Engine Water), `scripts/shore_edge_sharpness.py`.
- **Verified:** siehe Messung oben (Bild, md5, Tests). `Water.hasset` zweimal `mat_gen` byte-gleich, EditorDeps = Deploy.
- **Not verified:** Insel/Brücke im gerenderten Bild (nur der Test `bridged hole` deckt `refineShore` über eine
  Ohrenschnitt-Brücke; kein Baseline-View hat eine Insel), Landschaft mit Y-Drehung oder ungleicher Skalierung (der Kanal
  reist in der WELT-Normalen `mat3(model) * n`: eine Drehung um Y schiebt die Neigung von x nach z, bei 90° ist das Ufer wieder
  hart; ungleiche Skalierung ändert das x/y-Verhältnis und damit `e`; alle Zeugen und Tests laufen ohne Drehung/Skalierung),
  D3D11/D3D12/Vulkan (Schritt 5; dieselbe Graph-/Vertex-Quelle, aber nie gerendert), echte
  1-km-Landschaft mit Terrain-LOD (Witness: 1-m-Höhenfeld, LOD0), unsichtbarer Schatten/GI mit der Normalen-Neigung,
  GPU-Zeit des Mehr-Dreiecks-Meshes, Sculpt-Neubau im Editor interaktiv.
- **Decisions taken:** Tiefe aus dem Gitter-`g` (rein aus Gitter + Optionen, Hash trägt sie) statt aus dem Terrain
  an beliebigen Punkten (sonst wäre das Mesh keine Funktion des Hashes und ein Sculpt zwischen Gitterecken
  baute nichts neu); Kanal = Normale (UV gehören der Welt-Kachelung); Knöpfe in `Turbidity.zw` (alle 16 Slots voll);
  Nachbar-Kante nach Endpunktposition (Brücken).
- **Decisions suggested, not taken:** `Turbidity` bekommt in Schritt 4 vielleicht einen eigenen Namen/Slot, wenn
  die Transparenz Slots braucht; Wasseroberfläche mit gedrehter/gekippter Entity wird als Ufer gelesen
  (`n.x != 0`), falls das vorkommt: Marker in `n.z` ergänzen.
- **Pending:** Schritt 3 (Wellen), 4 (Transparenz: kann auf `e` aufbauen, Tiefe = `(1-e)*kShoreFadeDepth`,
  also nur bis 1 m), 5 (D3D/Vulkan).
- **Traps:** gespeicherte Overrides/Skripte, die `Turbidity` nur mit 2 Werten setzen, haben z = w = 0: Kante wieder
  hart, kein Schaum (kein Fehler, nur das alte Bild). `kShoreFadeDepth` ist auch die Obergrenze der Tiefe, die das
  Material kennt. Fremde Materialien auf dem Wasser sehen am Ufer bis 1,8° gekippte Normalen. Der Test `hasTJunction` braucht 1e-5 m
  Abstand (ein Float hält 25 m auf 3 µm): an der Brücke liegen zwei Eckpunkte ~4e-5 m auseinander (Kontur-Eckpunkt und eine
  Kantenmitte), das ist kein T-Stück, nicht noch einmal diagnostizieren. Die Degenerierten
  (Fläche 0) Dreiecke des Ohrenschnitts bleiben im Mesh, Tests filtern sie (`solidIndices`).
- **Next steps:** 1. Schritt 3. 2. Schritt 4 liest `e` für Transparenz (Boden sichtbar). 3. Schritt 5 rendert
  `beach`/`shore` auf D3D11/D3D12/Vulkan gegen `after-*.png`.
