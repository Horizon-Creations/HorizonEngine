# Wasser-Tool: Strömungen und Wasserfälle an Steigungen, Bestandsaufnahme und Entwurf (Thema 232, Schritt 1)

Nur Entwurf. Kein Shader, kein Mesh-Bake, kein Pinselverhalten ist geändert. Alle Zeilenangaben sind in diesem
Zweig gelesen (Stand `a8d6ce2d4`); was nicht gelesen wurde, steht unter "Nicht verifiziert".

## 1. Befund: was es gibt

* **Eine Strömung je Körper existiert schon, gespeichert und physikalisch wirksam.** `BodyPhysics::flow`
  (`WaterField.h:136`, terrain-lokal XZ in m/s, `kMaxFlow` = 50 `WaterField.h:111`), geklemmt `WaterField.cpp:333`,
  gespeichert nur bei Abweichung vom Standard (`SceneSerializer.cpp:825`, Laden `:1905`), gesetzt per
  `water.setFlow` (`EngineApi.cpp:1686`, Weltachsen rein, Landschaftsdrehung raus), im Editor `TerrainTools.cpp:2345`,
  in die Welt gedreht in `WaterQuery.cpp:208`. Sie zieht schwimmende Körper (Jolt-Widerstand) und schiebt Ringe
  (`docs/water-physics-plan.md:271-283`). Dort steht ausdrücklich: Wellen des Shaders folgen ihr nicht, "ortsabhängige
  Strömung ... als Textur" ist nicht Teil. **Dieses Thema erweitert genau das**, und es ersetzt die Zahl nicht.
* **Das Material liest keine Strömung.** `waterMaterial()` (`MatGen/main.cpp:659-1169`) baut alles aus
  `T::WorldPos` (`:749`), `T::Time` (`:752`) und `T::NormalWS` (`:753`); Wellen in Weltkoordinaten, nicht Mesh-UV
  (Kommentar `:255-262`). Verschiebung läuft über `T::Panner` mit Weltposition (Kaustik `:958-962`, Schaum
  `:981-984`). Die Zeit ist `lit.sunDir[3]` (Thema 234, Lektion im Kontext), **nicht** `HE::api::time::elapsed()`.
  Korrektur zum Auftrag: `MatGen/main.cpp:1211` ist die Kissen-Demo (`displacementDemoMaterial`, `:1199-1242`),
  nicht das Wasser; das einzige `T::UV` der Datei steht dort. Das Wasser-Material liest die Mesh-UV nirgends.
* **Das Mesh ist eine waagerechte Platte.** Eckpunkt `y = 0` (`WaterMesh.cpp:725-727`), die Entity steht auf
  `Body::level` (`WaterSurface.cpp:251`). Layout fest `pos3 + norm3 + uv2` (`Assets.h` `StaticMeshAsset`, `interleaved`
  "vertexCount*8"). Die Normale ist belegt: `(e·kShoreNormalGain, 1, 0)` (`WaterMesh.cpp:728-730`), vom Material als
  Betrag in XZ zurückgelesen (`MatGen/main.cpp:898-904`), damit eine gedrehte Landschaft das Ufer nicht zerlegt.
  Die UV (`:734-735`) ist Welt/`kUvMetersPerTile` und frei: Verbraucher sind nur `paramsKeyOf` (`WaterSurface.cpp:57-64`,
  hasht es) und `tests/test_water_surface.cpp:689-699`.
* **Der Wasserstand ist ein Skalar je Körper**, im Pinsel pro Strich festgehalten ("dragging uphill does not tilt the
  water", `WaterBrush.h:25-26`; `begin()` `WaterBrush.cpp:555-565`, Pegel `:546`). Die Uferbeschneidung nimmt die Fläche
  dort weg, wo der Boden über dem Pegel steht (`cutCell` `WaterMesh.cpp:95-135`). Bergauf malen ergibt also nichts,
  bergab einen stehenden, nach unten immer tieferen Keil. **"Strömung statt stehender Fläche" ist damit eine Frage des
  Höhenmodells, nicht nur des Shaders.**
* `WaterSurfaceComponent` wird **nie serialisiert** (`WaterSurfaceComponent.h:8-9`). Der gespeicherte Träger ist
  `Body` in `TerrainComponent.water` (`WaterField.h:291-340`). Ein neues Feld dort ist die Serialisierungsänderung.
* Pro-Körper-Werte erreichen das Material über `applyTessParams` (`WaterSurface.cpp:150-170`, `paramOverrides`), eine
  Material-Variante per Instanz-Schalter über `waterMaterialFor` (`WaterSurface.cpp:116-143`, UUID
  `DefaultAssets.h:98/103`). Beides ist Vorbild; kein Renderer-Code nötig.

## 2. Entscheidung A: Höhenmodell = Terrassen + Wasserfall-Band (nicht geneigte Fläche)

| | Geneigte Fläche (Eckpunkt-Y folgt Boden + Tiefe) | **Terrassen + Fallband (gewählt)** |
|---|---|---|
| Berührt | `Lattice`/`cutCell`/`insideSheet` (`WaterMesh.cpp:95-135`), `WaterQuery` (Skalar), Auftrieb, Unterwasser-Kamera, Tessellierung | Pinsel (neuer Körper je Stufe), neues Fall-Mesh |
| Physik/Schwimmen | neu zu definieren | unverändert (jede Terrasse ist eben) |
| Aufwand | groß, viele Tests | klein je Schritt, Bestand bleibt bit-gleich |

Gewählt: auf einer Steigung beginnt der Pinsel einen **neuen, tieferen Körper**, sobald die Wassertiefe unter dem
Pinsel `kTerraceDepth` überschreitet (Kandidat 1,0 m, passt zu `digDepth`-Standard 1,0 `WaterBrush.h:88` und
`levelOffset` 0,3 `:85`). Jede Terrasse bleibt flach. Der Wasserfall ist die **eigene Geometrie** zwischen dem unteren
Rand der oberen und dem Pegel der unteren Terrasse. Die Invariante "ein Strich = ein Körper" (`WaterBrush.h:15`) fällt
dafür nur unter einer neuen Option (`Params::cascade`, Standard aus), alles andere bleibt.

## 3. Entscheidung B: Darstellung der Strömung

Gewählt in zwei Stufen, kleinste zuerst:

1. **Ein Vektor je Körper** (Terrassen sind eben und kurz, ein Vektor genügt): Richtung und Betrag aus dem Boden,
   gespeichert in `physics.flow` (existiert, gespeichert, wirkt auf Physik und Ringe). Neu ist nur eine **Richtlinie**
   `Body::flowAuto` (Standard aus), damit der Pinsel keine von Hand gesetzte Strömung überschreibt. Das ist die einzige
   neue serialisierte Größe (Schreiben bei Abweichung wie `SceneSerializer.cpp:825`, Lesen `:1905`, `sameContent`/`Delta`,
   `sanitize`; alte Dateien ohne Schlüssel laden unverändert, dafür ein Test, siehe `CLAUDE.md` Abschnitt 4).
   Im Material: ein Knopf `Flow` (Vec4: Richtung xz in **Weltachsen**, Betrag, Reserve) per `applyTessParams`-Muster,
   der die bestehenden Panner (Kaustik, Schaum, Chop-Normalen) um `-flow · time` verschiebt.
2. **Pro Eckpunkt (nur wenn Stufe 1 an den Terrassengrenzen sichtbar nähtet):** Strömung in der **UV** des Wasser-Meshes
   (2 freie Floats, kein Layout- und kein Backend-Eingriff; die Normale ist belegt und wird von der Landschaftsdrehung
   betroffen, die UV nicht). In Weltachsen eingebacken (Drehung + Skalierung der Landschaft in `paramsKeyOf` aufnehmen,
   wie `shoreTiltScale`). Kosten: `test_water_surface.cpp:689-699` prüft UV = Welt/Kachel und gilt dann nur ohne Strömungsfeld;
   `surfaceHash` bekommt die Strömung. Verworfen: Strömungstextur (neue Ressource, Streaming, Quelle der Wahrheit
   doppelt) und Normale (belegt).

Schaumige Verschiebung bei ortsabhängiger Strömung braucht Zwei-Phasen-Überblendung (Valve-Flow-Map), das verdoppelt
die Rauschabfragen, und das Wasser ist der teuerste Pass (`docs/perf-water-cost-2026-10-21.md`: 12,8 ms im Scene-Pass bei 120 m See, vier Chop-Schichten allein ~25 %).
Deshalb **eine Material-Variante** `WaterFlow` (Instanz-Schalter `Flow` wie `Tessellation`), nur Körper mit Strömung
tragen die Kosten; stehendes Wasser bleibt bit-gleich (A/B-Zeuge).

## 4. Quelle des Steigungssignals

`terrainHeightAt(tc, x, z)` (`TerrainMeshGenerator.h:16`), zentrale Differenzen im Zellabstand des Wasserfelds
(`sizeX/res`, `kDefaultRes` = 256 `WaterField.h:67`), über die **nassen Zellen des Körpers** beim Strichende
(`end()`, `WaterBrush.cpp:640-669`; Zellkasten `bodyCells`, `WaterMesh.h:125`). Dieselbe Funktion füllt `Lattice::g`.
Nicht `gx/gz` aus `TerrainMeshGenerator.h:143` (LOD-Normalen-Cache je Chunk). Richtung = −(Mittel von ∇h über die
nassen Zellen); ein symmetrisches Becken mittelt sich zu 0, ein Teich bleibt also still, ohne Sonderfall.
Zum Zeitpunkt des Malens, nicht je Frame, mit dem Körper gespeichert; Sculpting danach rechnet **nicht** neu (Vorgabe:
später eine Schaltfläche "Recompute Flow"). **Nicht verwechseln mit `bedSlope`** (`WaterField.h:273`, `Params::bedSlope`
`WaterBrush.h:89`): das ist das Soll-Profil des gegrabenen Betts (m Tiefe je m Abstand zum Ufer), kein Maß des
vorgefundenen Bodens.

Betrag: `speed = vMax · smoothstep(sStill, sFall, |∇h|)`.

## 5. Schwellen (alle KANDIDATEN, nichts gemessen)

| Stufe | Neigung (m/m) | Winkel | Darstellung |
|---|---|---|---|
| stehend | < `sStill` = 0,03 | < 1,7° | Strömung 0, Fläche wie heute |
| Bach | 0,03 bis 0,7 | bis 35° | Material verschiebt Chop/Schaum, Betrag bis `vMax` = 3 m/s |
| Wasserfall | ≥ `sFall` = 0,7 | ≥ 35° | Fall-Band + Schaum am Fuß + Spray |

Begründung: Höhenfeld-Gelände überschreitet 35° kaum ohne bewusstes Modellieren; ab da klebt Wasser nicht mehr als Film
am Boden. 0,03 trennt Rauschen im Höhenfeld (Zellabstand mehrere Meter) von echtem Gefälle. Ein Fall entsteht außerdem
nur, wenn der Pegelunterschied zweier Nachbar-Terrassen ≥ `kFallMinDrop` = 0,4 m ist (Kandidat). Die Zahlen werden in
Schritt 5 gegen den Kopf-Schuss kalibriert; bis dahin ungeprüft.

## 6. Plan der übrigen Schritte (kleinster beweisender zuerst)

2. **Strömung aus dem Boden, ohne Optik.** `Body::flowAuto` + Berechnung bei `end()`; schreibt `physics.flow`.
   Dateien: `WaterField.h:291` (Feld), `WaterField.cpp` (`sanitize`, `sameContent`, `Delta`), `SceneSerializer.cpp:825/1905`,
   `WaterBrush.h` (`Params::autoFlow`), `WaterBrush.cpp:640` (`end()`), `TerrainTools.cpp:2345` (Schalter),
   `EditorHelp.cpp` (Hilfeeintrag, `scripts/editor_help_audit.py --check`), Tests `tests/test_water_brush.cpp`,
   `tests/test_water_field.cpp` (inkl. Laden der alten Form). Beweis: Rampe gibt Vektor bergab, Becken gibt ~0.
3. **Material verschiebt entlang der Strömung.** Knopf `Flow` + Variante `WaterFlow`. Dateien: `MatGen/main.cpp`
   (Panner `:958-990`, Schalter wie `Tessellation`), `WaterSurface.cpp:116/150` (Variante wählen, Knopf setzen),
   `DefaultAssets.h:103` (neue UUID neben 0x600,2), `tests/test_engine_materials.cpp`; `Water.hasset` neu erzeugen und mit
   dem Editor zusammen ausliefern (Paarungsfalle `kShoreFadeDepth`, unverändert). Alte Datei: Rückfall auf die einfache
   Variante wie bei `failed` in `waterMaterialFor`. Beweis: Headless-Schuss, stehender Körper bit-gleich zu vorher.
   Metal: Der Graph wird für alle fünf Backends erzeugt; die handgepflegte Spiegelung in `MetalRenderer.mm` ist der
   **Himmel**, das Wasser-Material berührt sie nicht. Anders bei Gischt als GPU-Partikel (eigenes System, Standard aus).
4. **Terrassen im Pinsel.** `Params::cascade`, `kTerraceDepth`; neuer Körper je Stufe. `WaterBrush.cpp:535-600`
   (`targetAt`/`begin`/`dab`), `WaterField.h` (`createBody` `:412`), `TerrainTools.cpp`, Tests in `test_water_brush.cpp`.
5. **Wasserfall-Band.** Nachbar-Körper mit Pegelunterschied ≥ `kFallMinDrop` aus `owner`/`level` ableiten, senkrechter
   Streifen vom Pegel oben zum Pegel unten, UV.y läuft mit der Fallhöhe, Schaumband am Fuß; Kind-Entity der Landschaft,
   nie serialisiert, wie die Fläche (`WaterSurfaceComponent.h`, `isTerrainGenerated`
   `TerrainChunkComponent.h:45` erweitern, Fünf-Stellen-Regel für neue Komponenten). Dateien: neues `WaterFall.cpp/.h`
   neben `WaterMesh.cpp`, `WaterSurface.cpp` (Anlegen/Abräumen), `MatGen/main.cpp` (`WaterFall.hasset`), Test
   `tests/test_water_fall.cpp` (in `tests/CMakeLists.txt` eintragen). Gischt: ParticleGraph (nicht gelesen).
6. **Nur falls nötig:** Strömung pro Eckpunkt in der UV (Abschnitt 3, Stufe 2). `WaterMesh.cpp:731-736`, `WaterMesh.h`
   (`SurfaceOptions`), `WaterSurface.cpp:57-64/293-299`, `test_water_surface.cpp`.
7. **Backends und Abnahme:** D3D11/D3D12/Vulkan (nicht auf dem Mac prüfbar), Perf-Vergleich gegen
   `docs/perf-water-cost-2026-10-21.md`, Handbuch-Eintrag.

Offene Entscheidung für die Königin: Terrassen (hier gewählt) gegen geneigte Fläche. Wer die geneigte Fläche will, braucht
eigenes Thema (Pegel als Feld statt Skalar).

## Handoff

- **Topic / step:** 232 (Wasser-Tool: Strömungen und Wasserfälle), Schritt 1 von 7 (Entwurf)
- **Branch / commit:** `claude/wasser-tool-str-mungen-und-automatische-wasserf-lle-an-steig` (siehe Git-Log; Push im Ergebnis vermerkt)
- **Done:** Entwurf mit Darstellung (Vektor je Körper, später UV), Steigungsquelle, Kandidaten-Schwellen, Plan Schritt 2 bis 7.
- **Files touched:** nur `docs/water-flow-2026-11-09.md`.
- **Verified:** jeder zitierte Pfad/jede Zeile in diesem Zweig gelesen (Abschnitt 1); `MatGen` `T::UV` nur `:1211` in der Kissen-Demo,
  `waterMaterial()` `:659-1169` enthält keines (grep). Kein Bau: es gibt hier kein `cmake-build-*`/`he_tests`.
- **Not verified:** `he_tests -tc="Water*"` (nicht gebaut, Vollbau zu lang); D3D11/D3D12/Vulkan; Hardware-Optik; wie benachbarte
  Körper am Rand ihre Konturen schneiden (`bodyCells`/`extractContours` nicht gelesen) und ob Schritt 5 daraus eine Kante ableiten kann;
  `SceneSerializer` nur an den Zeilen 825/1905 gelesen; ParticleGraph ungelesen.
- **Decisions taken:** Terrassen statt geneigter Fläche; Vektor je Körper in `physics.flow` + Richtlinie `flowAuto`;
  Material-Variante statt Dauerkosten; UV (nicht Normale/Textur) als spätere Eckpunkt-Strömung.
- **Decisions suggested, not taken:** alle Zahlen in Abschnitt 5 (Kandidaten); `kTerraceDepth` 1,0 m.
- **Pending:** Schritte 2 bis 7 (Abschnitt 6).
- **Traps:** `MatGen:1211` ist die Kissen-Demo; `WaterSurfaceComponent` ist nicht serialisiert; `physics.flow` ist Nutzereingabe,
  Pinsel darf sie nur mit `flowAuto` überschreiben; Zeit nur `lit.sunDir[3]`; `kShoreFadeDepth`/`kShoreNormalGain` unverändert,
  `Water.hasset` und Editor zusammen ausliefern.
- **Next steps:** 1. Schritt 2 (Abschnitt 6) mit `Body::flowAuto` in `WaterField.h:291`.

### Schritt 2 (Body::flowAuto + Strömung im Pinsel), erledigt

- **Gebaut:** `Body::flowAuto` (`WaterField.h`, Standard aus; in `bodiesFullEq` also sameContent + Delta; `setFlowAuto` in
  `WaterField.cpp`). Serializer: Schlüssel `flowAuto` nur wenn true (`SceneSerializer.cpp` Write/Read), alte Form lädt als aus.
  `brush::slopeFlow(tc, body)` (`WaterBrush.cpp`) = −Mittel(∇h) über die nassen Zellen (Stride ab ~4096 Zellen), Betrag
  `kFlowMaxSpeed·smoothstep(kFlowStillSlope, kFlowFallSlope, |∇h|)` (Kandidaten 3 m/s, 0,03, 0,7). `end()` schreibt das per
  `setPhysics` in `physics.flow`, nur wenn der Körper `flowAuto` hat und der Strich kein Radierstrich ist. `Params::autoFlow`
  schaltet in `begin()` `flowAuto` am Körper EIN, nie aus. Editor: Häkchen "Flow From Slope" im Pinsel (`TerrainTools.cpp`)
  und im Water-Physics-Baum des Körpers; Hilfeeinträge in `EditorHelp.cpp`.
- **Verifiziert:** `ctest -R test_water` 9/9 grün (Release, he_tests frisch gebaut), neue Fälle einzeln gelaufen
  (Rampe ±X/Z, steil = 3 m/s, eben/Becken/flach = 0, handgesetzter Wert bleibt, Radierstrich, Roundtrip + alte Form + falscher
  Typ); `editor_help_audit.py --check` grün.
- **Nicht verifiziert:** Editor-UI nur kompiliert, nicht bedient. Kein Neuberechnen nach Sculpting (Schritt-1-Vorgabe).
- **Entscheidung:** Ein Körper mit `flowAuto` wird bei jedem Strich neu berechnet, auch ohne `Params::autoFlow`.

### Schritt 3 (Material-Variante WaterFlow), erledigt

- **Gebaut:** `MatGen/main.cpp` (`waterMaterial()`): Param `Flow` (Vec4, x/y = Strömung in WELT-Achsen, m/s), Static Switch
  `Flow` (Standard aus). An: Suchposition = `Weltpos - flow * time` für die vier Chop-Schichten (`rippleLayer`), das
  Kaustik-`cuv` und die zwei Schaum-Panner. Aus: der Offset ist die Konstante 0, das Material ist das alte. Die Wellenzüge
  und die Tessellation bleiben unberührt (CPU-Formel von Physik/Unterwassereffekt). Ohne Switch hat die Standardvariante
  keinen `Flow`-Slot (14 Slots wie vorher); Flow + Tessellation = 16 Slots, passt.
- **Varianten:** `DefaultAssets.h` `kEngineWaterFlowMaterialId` {0x600,3} und `kEngineWaterFlowTessellatedMaterialId` {0x600,4}.
  `WaterSurface.cpp`: `waterMaterialFor(cm, tess, flow)` je KÖRPER (nicht je Landschaft), Rückfall flow→einfacher, nie grau.
  `worldFlowOf` dreht `physics.flow` mit der Landschaftsdrehung in Weltachsen; unter `WaterSurface::kMinVisibleFlow` (0,02 m/s)
  ist der Körper stehendes Wasser (Standardmaterial, kein Knopf). `RenderExtractor.cpp`: Wind gilt auch für die Flow-Variante.
- **Verifiziert (Mac, Metal, Release):** `ctest -R "test_water|test_engine_materials"` 10/10; neue Fälle: Flow-Switch im Graph
  (an/aus/mit Tess, Slotbudget), Variante je Körper mit echtem Water.hasset, Weltachsen-Knopf bei gedrehter Landschaft.
  **Pixel-Zeuge** (sauberes `HOME`, `HE_DUMP_WATERLAKE=l`, Level 8, `HE_DUMP_SETTLEMS=1500 HE_DUMP_FRAMES=6`, `HE_SKY_TIME=3|8`):
  stehendes Wasser mit NEUEM Water.hasset == mit dem ALTEN (git HEAD) md5-gleich bei beiden Zeiten; `HE_DUMP_WATERFLOW=2,0` ändert
  16,7 % der Pixel (Chop-Streifen und Schaumpunkte versetzt, die großen Wellen stehen).
- **Nicht verifiziert:** Richtung per Bildregistrierung (nur per GLSL: Suchposition `p - f*t` = Muster wandert mit `+f`, und Unit-Test
  der Weltachsen); D3D11/D3D12/Vulkan/OpenGL (laut Auftrag letzter Schritt); Release-Pak-Spiel (die Variante braucht Graph +
  Shader-Compiler wie die Tessellation, sonst Rückfall aufs einfache Wasser).
- **Fallen:** (1) Ein Projekt mit eigenem `Content/Engine/Materials/Water.hasset` (z. B. `Catania`) behält SEINEN Graph ohne
  Flow-Switch: Variante sieht dort aus wie stehendes Wasser. Für Zeugen sauberes `HOME` nehmen. (2) Ohne `HE_DUMP_SETTLEMS`
  wird ein frisch registriertes Material im ersten Frame nicht gezeichnet (Pipeline pending): "A == B md5" kann dann
  "beide leer" heißen. (3) `DefaultAssets.h` geändert = HorizonCore-Neubau. (4) `Water.hasset` ist neu erzeugt (mat_gen) und
  ins Repo-EditorDeps kopiert; `DisplacementDemo.hasset` NICHT mitgenommen (die mat_gen-Ausgabe weicht von der eingecheckten Datei ab; Ursache nicht untersucht, nicht Teil des Schritts).
- **Entscheidung:** Kein eigener "Variante aus"-Schalter (wäre eine neue serialisierte Einstellung): Fluss 0 = aus.
- **Nächste Schritte:** Schritt 4 (Terrassen im Pinsel, Abschnitt 6), dann Wasserfall-Band, dann Backend-Abnahme.

### Schritt 4 (Terrassen im Pinsel), erledigt

- **Gebaut:** `Params::cascade` (Standard aus), `kTerraceDepth` 1,0 m, `kTerraceSlope` 0,7 m/m, `kTerraceMinDrop` 0,4 m, `brush::groundSlope`
  (`WaterBrush.h/.cpp`). `dab()` ruft `startTerrace`: Wasser unter dem Dab tiefer als `kTerraceDepth`, Boden dort >= `kTerraceSlope`
  und Stufe `Boden + levelOffset` mindestens `kTerraceMinDrop` unter dem aktuellen Pegel -> neuer Körper, Strich malt dort weiter
  (`Stroke::body/level` zeigen auf den letzten). `Stroke::bodies` (alle Körper des Strichs, der erste zuerst) und `terraces`;
  `Dab::terrace`. `end()` räumt leere Körper und rechnet `flowAuto`-Strömung über alle Körper des Strichs. `begin()` setzt
  `flowAuto` auf Terrassen mit `autoFlow`. Editor: Häkchen "Terraces on Slopes" + Hilfeeintrag. Keine Serialisierungsänderung.
- **Verifiziert (Mac, Release):** `ctest -R test_water` 9/9; 7 neue Fälle (`-tc=*terraces*`): 45°-Rampe gibt Körper je Meter mit
  Stufenabstand 1,0 m, aus = ein Körper, eben/flach (0,5)/bergauf = ein Körper, hoher Offset hält Mindestabstand, Strömung je
  Terrasse, Radierstrich, Speichern/Laden. `editor_help_audit.py --check` grün.
- **Nicht verifiziert:** Editor-UI nur kompiliert, nicht bedient. Optik der Terrassen ohne Wasserfall-Band (Schritt 5) sichtbar als
  Stufen ohne Verbindung. Der neue Körper nimmt dem alten die Zellen im Pinselkreis (addCircle), bei grobem Dab-Abstand und großem
  Pinsel kann ein alter Körper fast leer werden.
- **Entscheidung:** Bei `levelFromGround = false` gilt der feste Pegel nur für die erste Stufe, danach Boden + Offset.
- **Nächste Schritte:** Schritt 5 (Wasserfall-Band zwischen Nachbar-Körpern, `Stroke`-Reihenfolge = Stufenreihenfolge nutzbar).

### Schritt 5 (Wasserfall-Band), erledigt

- **Gebaut:** `WaterFall.h/.cpp` (HE_Scene). `fallUppers(tc)`: ein Rasterdurchlauf, Körper mit 4-Nachbar-Zelle (beide nass) eines Körpers, dessen Pegel
  um >= `kFallMinDrop` (= `brush::kTerraceMinDrop` 0,4 m) tiefer liegt. `buildFalls(...)`: Band je OBEREM Körper aus den Kantenstücken seines Blatts
  (`shoreSegments`), die an einen tieferen Körper grenzen; je Stück ein Streifen von der Blattkante (Höhe oben) bergab entlang des Bodens bis
  zum tieferen Pegel (max. `kFallMaxRun` 8 m; flaches Gelände/seitliche Kante = senkrechter Abfall) plus ein Schaum-Vorfeld (`kFallApron` 0,6 m,
  `kFallLift` 2 cm über dem unteren Blatt). Schaum = die Ufernähe `e` in der Vertex-Normale (Oberkante: Wert des oberen Blatts, Fuß 1, Vorfeldrand 0),
  die das Wassermaterial schon liest. Kein neues Material: das Band nutzt die Flow-Variante (`Flow` = mittlere Bergab-Richtung * `kFallSpeed` 3 m/s).
  `WaterSurface.cpp`: Band-Entity = `WaterSurfaceComponent` mit neuem Feld `fall` (`body` = oberer Körper), also ohne neue Komponente, nie serialisiert,
  von `isTerrainGenerated` erfasst; `Stats::falls/fallsRemoved`. Läuft nach den Blättern, nur wenn das Blatt-Steady-Check nicht greift; Hash gleich = Mesh bleibt.
- **Verifiziert (Mac, Release):** `ctest` komplett 365/365 grün; neue `tests/test_water_fall.cpp` (7 Fälle: Kaskade gibt Band unter jeder Terrasse außer der
  letzten, Schwelle 0,3 vs. >= 0,4, nicht berührende Körper, senkrechter Abfall auf Ebene, Band bleibt an der Kante, Entity-Lebenszyklus inkl. Wegfall bei
  Pegelangleichung/Entfernen, keine Bänder ohne Gefälle). **Bild-Zeuge:** `HE_DUMP_WATERLAKE=cascade HE_DUMP_WATERSLOPE=0.8` (neuer Modus in
  `EditorApplication.cpp`: Pinselstrich bergab mit Terraces + Flow From Slope), sauberes `HOME`, `TOD=0.5 DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`, Metal:
  `docs/water-fall-cascade-front-2026-11-09.png` (CAMX=-24 CAMY=292 CAMZ=0 PITCH=15 YAW=90), `docs/water-fall-cascade-close-2026-11-09.png`
  (CAMX=-9 CAMY=298 CAMZ=5 PITCH=-12 YAW=75): durchgehender Vorhang zwischen den Stufen, Streifen laufen mit der Strömung, Schaumfuß.
- **Nicht verifiziert:** D3D11/D3D12/Vulkan/OpenGL; Editor-UI; Release-Pak-Spiel (Flow-Variante braucht Graph + Shader-Compiler, sonst Rückfall aufs einfache
  Wasser: Band dann ohne Streifen). Der Schaum ist kaum als weißes Band zu sehen (Material liest `e` nur als Ufernähe; ein eigenes WaterFall-Material
  mit echter Gischt wäre der nächste Ausbau). Perf nicht gemessen (Bänder sind wenige hundert Dreiecke, ein Aufbau je Strich-Tick und oberem Körper).
- **Entscheidungen:** Band aus dem Feld abgeleitet, nicht aus `Stroke::bodies` (nichts gespeichert, gilt auch für von Hand gemalte Nachbarn);
  Streifen liegt am Boden entlang statt als Wand; `shoreSegments` orientiert Einzelkreuzungen NICHT (nur Sättel), die Seite kommt aus `insideSheet`.
- **Fallen:** (1) `shoreSegments` Richtung ist beliebig: `insideSheet` je Stück fragen. (2) Der Pinsel-Rand (falloff) lässt Zellen mit kleiner Deckung
  zwischen den Blättern: Nachbar = erste Zelle eines FREMDEN Körpers beim Hinausgehen (bis 8 Zellen), nicht erst ab `kWet`. (3) Tests mit 1-m-Zellen
  und 0,5-m-Pinsel geben pro Stufe ein Ein-Zellen-Blatt (leer): `res` 256 nehmen. (4) Ein nacktes `ContentManager{"."}` hat kein Wassergraph, die Flow-Variante
  fällt auf einfaches Wasser zurück; getestet wird der `Flow`-Knopf, nicht die Material-ID.
- **Nächste Schritte:** Schritt 6/7: Backend-Abnahme (D3D/Vulkan/GL), Perf-Vergleich, Handbuch-Eintrag; optional eigenes Fall-Material mit Gischt.

### Schritt 10 (Hügel-Zeuge mit modelliertem Gelände), erledigt

- **Gebaut:** nur Zeugen-Modus: `HE_DUMP_WATERLAKE=hill` in `EditorApplication.cpp` (neben `cascade`): gaußscher Hügel 24 m hoch mit Wellung statt Rampe,
  Strich vom Gipfel (x=3) die +X-Flanke hinab mit Mäander in z, durch `brush::begin/dab/end` mit Terraces + Flow From Slope (wie `cascade`).
  Kein Produktcode geändert. Ein echtes GUI-Malen war nicht möglich (MCP-Bridge des Editors nicht erreichbar, kein Display): es ist der Pinsel-Code, nicht die Mausbedienung.
- **Bilder** (Metal, sauberes HOME, `WATERSLOPE=0.8 TOD=0.5 DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`): `docs/water-flow-hill-side-2026-11-09.png`
  (CAMX=24 CAMY=326 CAMZ=-42 PITCH=-12 YAW=180): Terrassenfolge den Hügel hinab in ein Becken; `docs/water-flow-hill-close-2026-11-09.png`
  (CAMX=20 CAMY=316 CAMZ=-14 PITCH=-18 YAW=160): Stufen mit Wasserfall-Bändern; `...-over-...png` (Rückansicht, schwach).
- **Auffällig:** in der Nahaufnahme steht oben rechts eine türkise Platte (Band an der Mäanderkehre, vermutlich Rückseite/Unterwasser-Tönung); nicht untersucht.
- **Nicht verifiziert:** Mausbedienung im Editor, andere Backends für diese Szene.

### Schritt 11 (Überarbeitung: Stufen-Leiter statt schwebender Platten), erledigt

- **Ursache (gemessen, Pinsel-Code auf dem Hügel, heute `test_water_fall.cpp` "modelled hill"):** 15 Körper; die mittleren hielten nur 22-31 Zellen
  (1 m x 6 m Streifen) und standen bis 4,4 m über dem Boden, Körper 5 bis 7,2 m, der letzte 9 m. Grund: `addCircle` nimmt **nie** fremde Zellen
  (`WaterField.h:525`; die Schritt-4-Notiz "nimmt dem alten die Zellen" stimmte nicht). Der Pinselkreis (2,5 + 1 m) hatte 3,5 m voraus schon im
  alten Pegel benetzt; die neue Stufe (Pegel aus der Kreismitte) bekam nur den trockenen Rand dahinter, die alte behielt ihre tiefe Front: Platten
  in der Luft, dazwischen 0,7-m-Vorhänge = Minecraft-Treppe. Dazu sperrte `kTerraceSlope` 0,7 die Flanke, wo die Wellung sie unter 35° drückte
  (x 8,6..15): dort wurde nur tiefer, bis eine Stufe um 3,9 m fiel.
- **Türkise Platte = erklärt + behoben:** Band von Körper 5, 4,7 m hohe senkrechte Wand in der Luft. Das Material tönt mit `depth/(n·v+0.05)`
  (`MatGen/main.cpp:964`); die Band-Normale zeigt nach oben, von der Seite ist n·v ≈ 0 (Weg 20-fach), und die Oberkante erbt e = 0 (offenes
  Wasser) vom 7 m schwebenden Blatt: deckendes Tiefseetürkis. Die Wand gibt es nicht mehr. Bleibt: dasselbe Material zeigt jede Wasserfläche
  **streifend oder von unten** (n·v ≤ 0) türkis; in der neuen Nahaufnahme oben rechts sind das die oberen Becken, gesehen von 16 m bei Pegeln
  16-18 m (`near1`, nicht eingecheckt). Von oben sind dieselben Becken hell. Materialänderung nicht Teil des Schritts.
- **Gebaut (`WaterBrush.h/.cpp`, `paintLadder`):** Stufen = Leiter je `stepHeight()` unter dem Strichpegel. Wo der Boden unter dem Pinsel
  (Steigung über die Pinselweite, `brushSlope`, nicht eine Zelle) >= Min Slope ist, geht jede Zelle des Tupfers an die **höchste Stufe, die sie
  höchstens `lipDepth` tief hält** (sonst an die unterste); Zellen wandern nur zwischen Stufen dieses Strichs, fortgesetzte und fremde Körper behalten
  ihre. Neue Stufe, sobald die **vordere Pinselhälfte** (Laufrichtung, `Stroke::last`) Boden tiefer als `lipDepth` unter der untersten Stufe erreicht
  (erster Tupfer, bergauf, quer: keine Stufe). Ergebnis: flache Becken, an der Lippe höchstens so tief wie gemalt, dazwischen (unter dem Hang verborgen)
  Zellen der nächsten Stufe, über die das Band läuft. `WaterFall.cpp`: Band nur, wo der Fuß im Wasser landet (`landsIn`: der untere Körper oder ein noch
  tieferer; vorher liefen Bänder vom Gipfel bis 8 m seitlich auf trockenes Land); Vorfeld-Außenrand e = Ufernähe des unteren Blatts statt 0 (die 0 zog
  dunkle Linien über jedes Becken).
- **Anfrage 171 des Menschen (feiner + Tiefe am Hang, eigener Parameter), mit der Königin abgestimmt (172: Terrassen, keine geneigte Fläche):**
  `Params::terraceStep` = Regler **Step Height** (Standard 1,0 m, 0,4..10) und `Params::terraceSlope` = Regler **Min Slope** (Grad, Standard 16,7° = 0,3 m/m,
  3..63°) unter "Terraces on Slopes" (`TerrainTools.cpp`, Hilfe in `EditorHelp.cpp`); Lippe = **Above Ground** (`lipDepth`, mind. 0,1 m; feste Höhe: 0,5 m).
  Am flachen Hang wird die Stufe von selbst niedriger (`stepHeight`: Lippe + 6 m · Steigung), damit das Band die nächste Stufe erreicht. Zeuge:
  `HE_DUMP_WATERSTEP=<m>`, `HE_DUMP_WATERMINSLOPE=<Grad>`.
- **Schwellen (begründet):** `kTerraceDepth` 1,0 -> Lippe = Above Ground (0,3 Standard; `kTerraceDepth` 0,5 nur bei fester Höhe); neu `kTerraceDrop`
  **1,0** (erst 2,0: auf 0,8 m/m 1,9 m Band + 0,6 m Becken; auf Wunsch feiner, die Bänder hängen weiter, weil kein Becken mehr in der Luft steht);
  `kTerraceSlope` 0,7 -> **0,3** (17°, über die Pinselweite gemessen: die Hügelflanke 0,5-0,8 mit Wellung fiel sonst stellenweise heraus);
  `kTerraceMinDrop` 0,4 bleibt (Band-Schwelle, Untergrenze von Step Height).
- **Bilder** (wie Schritt 10): vorher `docs/water-flow-hill-side-2026-11-09.png`/`-close-` (Schritt 10, heute vor dem Bau neu gerendert, optisch gleich),
  nachher `docs/water-flow-hill-side-after-2026-11-09.png`, `-close-after-`, `-top-after-` (CAMX=22 CAMY=372 CAMZ=2 PITCH=-89 YAW=180) mit den
  Standards (24 Körper), `-side-min5-after-`/`-top-min5-after-` mit Min Slope 5° (auch der flache Fuß gestuft). Rampe (31 Körper):
  `docs/water-fall-cascade-front-after-2026-11-09.png` (vgl. `water-fall-cascade-front-2026-11-09.png`; die Stufen über der Kamerahöhe sind von
  unten gesehen türkis, die auf Augenhöhe streifend unsichtbar, die darunter hell).
- **Verifiziert (Mac, Release, Metal):** `ctest -R test_water` 10/10; `he_tests -tc="Water*"` 256/256; `editor_help_audit.py --check` grün. Neue Fälle:
  Hügel (Mitte <= Lippe, erste/letzte <= 2 m, jedes Band nicht leer und eine Stufe hoch), 45°-Rampe ohne Zelle über Lippe, `lipDepth`, `stepHeight`/Step
  Height, Min Slope (flacher Hang 0,08 gestuft, Stufen niedriger), Vorfeld-e, Band auf trockenes Land.
- **Nicht verifiziert:** andere Backends; Editor-Mausbedienung; grobe Wassergitter (echte 1000-m-Landschaft: 3,9-m-Zellen, ein Becken ist schmaler
  als eine Zelle); Graben (`dig`) zusammen mit Terrassen.
- **Offen:** endet ein Strich mitten am Hang, hält die letzte Stufe nur verborgene Zellen und ihr Band ist teilweise (Rampe: 5 von 12 Stücken);
  Teiche mit steilen Ufern werden mit dem Schalter gestuft (Hilfetext sagt es).

### Schritt 12 (glatter auf die Oberfläche + Bachbett-Aushebung am Hang), erledigt

- **Warum die Leiter aus Schritt 11 noch wie eine Treppe aussah (Geometrie, nicht gemessen):** je Stufe ist das sichtbare Becken
  `Lippe / Steigung` lang (0,3 m / 0,7 = 0,43 m, flach) und das Band `(Stufe − Lippe) / Steigung` (1 m Stufe: 1 m, geneigt). Die Lippe
  kann auf diesem Zeugen nicht kleiner: 0,5-m-Zellen auf 0,5..0,8 Steigung, ein Becken unter einer Zelle hat keine Uferkante, das
  Band darunter nichts zum Aufhängen (Kette reißt). Der Hebel ist die Stufenhöhe: Rendering bei 1,0 / 0,7 / 0,5 / 0,4 m
  (`HE_DUMP_WATERSTEP`, gleiche Kamera): ab 0,5 m liest sich der Strich als ein Band den Hang hinab, 0,4 m sieht gleich aus mit
  einem Viertel mehr Körpern (57 statt 46) und läge genau auf `kFallMinDrop` (fallUppers prüft `>=` in float).
- **Entscheidung Step Height = 0,5 m** (`kTerraceDrop`, `Params::terraceStep`; `static_assert` hält beide gleich; vorher 1,0).
  Kein eigener Lippen-Regler (Above Ground bleibt die Tiefe am Hang, Schritt 11).
- **Dig Bed + Terrassen: vier Stellen des Teich-Grabens waren für einen Bach falsch** (`dab()` grub nur für `s.body`): (1) `digBed`
  schneidet Boden über dem Pegel im unverschnittenen Fußabdruck auf den Pegel, also die verborgenen Zellen jeder Stufe: der Hang
  würde zu Platten; (2) `keepUnderSheet` gibt alles zurück, was nicht unter dem **verschnittenen** Blatt der Stufe liegt, also den
  Boden unter den Bändern: kein durchgehender Kanal; (3) `shapeBank` schabt den Hang über jedem Becken zur Wasserlinie; (4) die
  Tiefe vom eigenen Ufer der Stufe gemessen ist zentimeterklein. **Gebaut:** `digChannel` (`WaterBrush.cpp`): Fußabdruck = Vereinigung
  der Körper des Strichs (`buildLattice`-Überladung für mehrere Körper, `WaterMesh.h/.cpp`, unverschnitten), gleiche Formel wie der
  Teich, `min(maxDepth, slope·d)` zum Umriss des Bachs, aber relativ zum **Boden vor dem Pinsel** = Höhe jetzt − Σ `brushPatch` aller
  Körper an dem Vertex (`brushMovedAt`): fester Zielwert, gehaltener Tupfer und zweiter Strich graben nichts mehr. Nur innerhalb des
  Umrisses, nur tiefer, `amount`-gestuft, keine Böschung. Danach `settleLadder` (die Leiter-Regel aus `paintLadder` herausgelöst) über
  die Reichweite des Grabens: die Becken bleiben an der Lippe so tief wie gemalt, der Bach liegt im Kanal. **Schalter ist die
  Einstellung** (`p.cascade`), nicht „hat schon gestuft": ein Strich, der auf einer Stufe eines Bachs hält oder ihn ohne eigene Stufe
  verlängert, würde sonst mit dem Teich-Graben eine Platte schneiden (Test). `closeBedAt`: Boden unter **irgendeinem** Wasser behält
  sein Bett (der Kanal steht im Patch der gerade malenden Stufe, unter Zellen der Nachbarstufen).
- **Zeugen-Knöpfe** (`EditorApplication.cpp`, Modus `cascade`/`hill`): `HE_DUMP_WATERDIG=<m>` (Dig Bed an, Max Depth),
  `HE_DUMP_WATERDIGSLOPE=<m/m>`, `HE_DUMP_WATERHIDEWATER=1` (nur Boden). Die Logzeile nennt Körper, Terrassen, bewegte Vertices und den
  tiefsten Schnitt.
- **Bilder** (Metal, Release, sauberes HOME, `HE_SKY_TIME=30`, `WATERSLOPE=0.8 TOD=0.5 DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`):
  **vorher** = `docs/water-flow-hill-side-after-2026-11-09.png` (Schritt 11, heute mit dem Schritt-11-Binary neu gerendert, 24 Körper, gleich);
  **nachher** `docs/water-flow-hill-side-step12-2026-11-09.png` (Standards, 46 Körper, Kamera CAMX=24 CAMY=326 CAMZ=-42 PITCH=-12 YAW=180: ein Band
  statt Treppe), `-side-step12-dig-` (dazu `WATERDIG=1`: 45 Körper, 333 Vertices bewegt, tiefster Schnitt 0,86 m, der Bach liegt in einer Rinne,
  der Fußteich dunkler = tiefer), `-close-step12-`/`-close-step12-dig-` (CAMX=20 CAMY=316 CAMZ=-14 PITCH=-18 YAW=160), `-side-step04-`
  (Step Height 0,4: 57 Körper, optisch wie 0,5). **Graben-Beleg ohne Wasser:** `docs/water-flow-hill-close-ground-diff-2026-11-09.png` =
  |Boden mit Dig − Boden ohne Dig| (×6, `WATERHIDEWATER=1`, gleiche Kamera/Licht; 2,0 % der Pixel > 8/255, mittlere Differenz 0,49/255): die
  Rinne zieht als Band über die ganze Flanke; `docs/water-flow-hill-top-ground-diff-2026-11-09.png` dasselbe senkrecht von oben (CAMX=22 CAMY=372
  CAMZ=2 PITCH=-89; mit dem 1,0-m-Binary gerendert, der Strich ist derselbe): der Mäander des Strichs. Im reinen Boden-Bild selbst ist die Rinne bei
  Mittagslicht (DAYNIGHT=0) kaum zu sehen (V-Profil 0,25, 0,86 m tief, 6 m breit), daher die Differenzbilder; Renderings mit tiefer Sonne
  (`DAYNIGHT=1 TOD=0.3|0.72`, `/tmp/wf12/*-am|pm.png`) waren beim Stopp noch nicht ausgewertet.
- **Verifiziert (Mac, Release, Metal):** `ctest -R test_water` 10/10 (test_water_brush 44 Fälle, test_water_fall 11 Fälle); neue Fälle: Kanal
  (Max Depth entlang der Mitte, zum Umriss ansteigend, nichts außerhalb, nichts gehoben, Becken ≤ Lippe, Stufen 0,5 m), Konvergenz (gehaltener
  Tupfer auf der letzten und einer mittleren Stufe, zweiter Strich: bitgleich), Close Bed nach Save/Load gibt alles zurück, Hügel + Dig (Rinne,
  Bänder an jeder Stufe). `editor_help_audit.py --check` grün. Vorher/Nachher-Renderings wie oben (Pinsel-Code, keine Maus).
  **Nicht gelaufen:** die Vollsuite (`ctest` komplett) — Stopp durch die Königin, bevor sie startete.
- **Nicht verifiziert:** andere Backends; Editor-Mausbedienung; grobe Wassergitter (3,9-m-Zellen); Close Bed auf einem teilweise
  radierten Bach (nur ganz radiert getestet).
- **Offen / nächste Schritte:** (1) das Band ist eine gerade Sehne von Lippe zu Fuß, kein Bodenfolger: bei 0,5 m unsichtbar, bei langen
  Stufen auf der Wellung schwebend; (2) ein zweiter Strich, der den Bach **verbreitert**, gräbt nur unter seinem eigenen Rand (Umriss =
  seine Körper, nicht die fremden Stufen daneben); (3) 46 Körper + 45 Bänder je 43-m-Strich sind je ein Entity + Mesh, auf einer echten
  Landschaft zu zählen; (4) im V-Kanal (Slope 0,25) ist das Wasser `2·Lippe/Slope` breit (2,4 m in einem 6-m-Graben), ein U-Profil gibt
  es mit höherem Slope; (5) Material: streifend gesehene Becken bleiben türkis (Schritt 11).
