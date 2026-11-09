# Wasser-Tool: Strömungen und Wasserfälle an Steigungen, Bestandsaufnahme und Entwurf (Thema 232, Schritt 1)

> **ÜBERHOLT (Schritt 14, 2026-11-09, auf Wunsch des Menschen):** Terrassen, Wasserfall-Bänder (`WaterFall.cpp`) und die Stufen-Regler sind
> ausgebaut. Ein Strich mit *Follow Slope* malt jetzt einen **Stream**: eine glatte Fläche auf dem Boden. Die Abschnitte oben bis Schritt 13
> beschreiben den alten Weg und gelten nur noch als Geschichte; maßgeblich ist der Abschnitt **Schritt 14** am Ende.

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
  **Vollsuite nachgeholt:** `ctest --test-dir cmake-build-release -j4` 365/365 grün (Release, Binary vom Commit 900d24b93).
  Die Renderings mit tiefer Sonne (`DAYNIGHT=1 TOD=0.3|0.72`, nur Boden, dig an/aus) bringen nichts über die Differenzbilder hinaus
  (die Rinne liegt im Nahbild hinter dem vorderen Rücken, abends liegt der Hang im Schatten); sie sind nicht abgelegt.
- **Entscheidung V-Kanal (eigene Einschätzung, keine Architekturfrage):** Slope-Standard bleibt **0,25**. Das Wasser im Kanal ist
  `2·Lippe/Slope` breit (0,3 m Lippe: 2,4 m in einer 6 m breiten Rinne), das passt zu einem Bach auf einem Hügel dieser Größe, und
  Slope ist dieselbe Einstellung wie beim Teich: ein höherer Standard nur für Terrassen bräuchte einen zweiten, serialisierten
  Wert (`brushBed` gilt je Körper) für wenig Gewinn. Wer einen schmaleren, tieferen Bach will, stellt Slope hoch (0,5: 1,2 m Wasser
  in 3 m Rinne) und Max Depth nach; der Dig-Bed-Hilfetext nennt beides. Falls Anwender das trotzdem als Ärgernis melden: Dig Bed
  mit Terraces bekommt einen eigenen Slope-Regler.
- **Folgeschritt (nicht hier umgesetzt): bodenfolgendes Band.** `buildFalls` (`WaterFall.cpp`) hängt je Kantenstück ein gerades
  Viereck von der Lippe zum Fuß (`footOf` liefert nur den Endpunkt). Ersatz: Polylinie entlang des Bergab-Wegs, die Höhe je Stützpunkt
  aus `terrainHeightAt` + `kFallLift`, damit das Band auf der Wellung und im frisch gegrabenen Kanal nicht schwebt oder einsinkt;
  Stützabstand ~ eine Wasserzelle, Hash und Watertightness an der Kante wie heute (`feet`-Schlüssel). Test: Band-Höhe nie über
  Boden + Toleranz, nie darunter.
- **Nicht verifiziert:** andere Backends; Editor-Mausbedienung; grobe Wassergitter (3,9-m-Zellen); Close Bed auf einem teilweise
  radierten Bach (nur ganz radiert getestet).
- **Offen / nächste Schritte:** (1) das Band ist eine gerade Sehne von Lippe zu Fuß, kein Bodenfolger: bei 0,5 m unsichtbar, bei langen
  Stufen auf der Wellung schwebend; (2) ein zweiter Strich, der den Bach **verbreitert**, gräbt nur unter seinem eigenen Rand (Umriss =
  seine Körper, nicht die fremden Stufen daneben); (3) 46 Körper + 45 Bänder je 43-m-Strich sind je ein Entity + Mesh, auf einer echten
  Landschaft zu zählen; (4) im V-Kanal (Slope 0,25) ist das Wasser `2·Lippe/Slope` breit (2,4 m in einem 6-m-Graben), ein U-Profil gibt
  es mit höherem Slope; (5) Material: streifend gesehene Becken bleiben türkis (Schritt 11).

### Schritt 13 (Kamerawinkel + die kantige Ecke oben rechts), erledigt

- **Bilder** (Hügel + Dig Bed, Metal, Release, sauberes HOME, `HE_SKY_TIME=30`, `WATERLAKE=hill WATERSLOPE=0.8 WATERDIG=1 TOD=0.5
  DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`; Kamera = `CAMX/CAMY/CAMZ PITCH YAW`, Landschaft auf y=300):
  `docs/water-flow-hill-top-step13-2026-11-09.png` **Draufsicht** (25/352/1, −89,5, 0: Mäander und Stufen, Fußteich rechts);
  `-side-step13-` **Seitenprofil quer zum Hang** (24/332/38, −25, 0: von +Z, bergab nach rechts, der Bach im Kanal die Flanke hinab);
  `-close-step13-` **Nahaufnahme von oben** auf die oberen Becken (18/325/−6, −32, 206,6: kein Becken von unten);
  `-down-step13-` den Bach hinab vom Gipfel (−8/333/1, −28, 90); `-up-step13-before-`/`-up-step13-` vom Fuß hinauf (58/310/−3, 6, −90),
  vor/nach dem Fix; `-close-step13-fix-` die alte Nahaufnahme (20/316/−14, −18, 160) nach dem Fix (vorher: `-close-step12-dig-`).
- **Ursache der Ecke (gemessen):** Material, nicht kaputte Geometrie. Die Ecke sind die Körper 12–17 (Pegel 16,8–18,8 m), die Kamera
  steht auf 16 m, also 0,8–2,8 m **unter ihrer Ebene**. Dort schaltet das Wassermaterial auf die Unterseite (`undersideTerms`,
  Totalreflexion = deckender Teal-Spiegel), weil `below` nur „Kamera unter der Ebene“ prüft, nicht „Kamera im Wasser“. Beleg: mit
  `below` fest 0 (Probe, nicht eingecheckt) verschwand das Teal in beiden Ansichten ganz; der Spitzenfaktor `depth/(n·v+0.05)` der
  Oberseite (Schritt-11-Vermutung) war es nicht. Im Bild vom Fuß beginnt das Teal genau auf Augenhöhe. Geometrie je Körper
  (Diagnose-Testfall, nicht eingecheckt): Blattrand 0,33–0,49 m über Boden (Lippe 0,3 + halbe Zelle Gefälle), jedes Band eine Stufe,
  keine schwebende Platte, kein Rest-Körper. Das Sägezahn-Profil ist die echte Terrassenform (0,5-m-Zellkontur + gerade Bänder), die
  erst durch den deckenden Spiegel hart wurde.
- **Fix:** Nur ein Körper, dessen Wasser die Kamera erreichen kann, zeigt die Unterseite. `SurfaceMesh::deepest` (`WaterMesh.h:239`,
  gefüllt in `buildSurface`, `WaterMesh.cpp:696`, an den Wasser-Ecken des Gitters; Band 0, `WaterFall.cpp`) -> `WaterSurfaceComponent::
  deepest` -> `Flow.z`, `Flow.w = 1` (`applyFlowParams`, `WaterSurface.cpp:216`, auch beim Neubau eines Blatts, `:301`). Material
  (`MatGen/main.cpp:1184`): Kamera tiefer unter der Ebene als `Flow.z + 0,25 m` -> Oberseite (von unten streifend = Himmels-Fresnel,
  wie die Becken unter der Kamera); über die 0,25 m blendet die Unterseite vorher aus. Alles hinter dem Static Switch `Flow`.
  `Water.hasset` neu erzeugt (mat_gen, deterministisch: unverändert neu erzeugt = md5-gleich mit dem alten) und nach `EditorDeps` kopiert.
- **Verifiziert (Mac, Release, Metal):** `ctest -R "test_water|test_engine_materials"` 11/11; neuer Fall `test_water_fall.cpp:498`
  (Hügel + Dig: jedes Becken `Flow.z` = `deepest`, ±0,25 m zur Zellmitten-Messung, `w` = 1; jedes Band `z` = 0; stiller Teich ohne
  Knopf; zweiter Tick gleich). Pixel: `up` vs `up-after` 0,97 % Pixel > 8 (bbox nur der obere Bach), `close-ref` vs `-after` 0,56 %
  (bbox nur die Ecke); nachher vs Probe (`below` = 0) 0,01/0,02 %. **Sonst unverändert, md5-gleich altes vs neues Hasset (gleiches
  Binary):** stiller See `l` von oben, stiller Bowl-See aus 1 m unter Wasser, fließender Bowl-See (`WATERFLOW=2,0`, `WaterFlow`)
  aus 1 m unter Wasser (Taucher sieht Snell-Fenster und Netz wie vorher). Ansichten von oben (side/close/down) vor/nach md5-gleich,
  top 0 Pixel > 8 (Mittel 0,000). **OpenGL:** `up` nach dem Fix mit `RHI=OpenGL` gerendert: 0 Link-/Compile-Fehler im Log, alle 89
  Entities `WaterFlow, shader ready`, Bild wie Metal (kein Teal). Vollsuite `ctest -j4` 365/365.
- **Kompatibel in beide Richtungen:** alter Editor + neues Hasset: Knopf hat z = w = 0 -> kein Tor, altes Aussehen; neuer Editor + altes
  Hasset: das Material liest z/w nicht -> altes Aussehen. (Anders als `kShoreFadeDepth` muss hier nichts zusammen ausgeliefert werden.)
- **Entscheidung:** Tor je KÖRPER (exakt: feuert nie für eine Kamera, die im Körper sein kann), nicht je Fragment über `shoreD` (das
  hätte einem Schnorchler im flachen Uferwasser die Totalreflexion zum Ufer hin genommen).
- **Nicht verifiziert:** D3D11/D3D12/Vulkan (Material-Codegen, kein neuer Knotentyp); Editor-Mausbedienung.
- **Offen:** (1) **stilles** Wasser (Terraces ohne Flow From Slope, Standard aus) hat keinen `Flow`-Knopf: seine Becken zeigen von unten
  weiter Teal; ein Knopf für alle Varianten sprengt bei Flow+Tessellation die 16 Slots (heute genau 16). (2) Körper 6, 8, 14: Band
  fällt zwei Stufen (`lo` −0,98), `landsIn` nimmt bei einem Becken schmaler als eine Zelle den nächsttieferen; nicht die Ecke.
  (3) Bänder tragen eine +Y-Normale (e im Kipp): von vorn gesehen „streifend“; echte Flächennormale bräche die e-Kodierung.
- **Nächste Schritte:** (1) Offen (1) schließen: den `Flow`-ParamVec4 vor den Static Switch ziehen (die Versatz-Knoten bleiben
  geschaltet): plain 15 / Tess 16 / Flow 15 / Flow+Tess 16 Slots, passt; dann `deepest` an jedes Blatt (w = 1), still oder nicht.
  (2) bodenfolgendes Band (Abschnitt Schritt 12); (3) Backend-Abnahme der Flow-Variante (D3D/Vulkan).

### Schritt 14 (Redo: Stream statt Terrassen, auf Wunsch des Menschen)

Der Mensch wollte keine Terrassen, sondern eine natürliche, glatte Wasserfläche, die dem Hang folgt, sichtbar fließt, wie Wasser aussieht (nicht wie
Minecraft-Wasser), in einer ausgegrabenen Rinne liegt, nicht übersteht und sich ohne Naht mit anderem Wasser verbindet; dazu D3D/Vulkan.

- **Datenmodell:** `Body::stream` + `Body::streamDepth` (`WaterField.h`, Standard aus, nur bei `true` gespeichert, `bodyEq`, `sanitize`, `setStream`). Die Höhe ist
  nie gespeichert: `streamSurfaceAt` = Boden + Tiefe. `Body::level` bleibt nur die Bezugshöhe des Entity.
- **Mesh (`WaterMesh.cpp`):** ShoreClip/SurfaceOptions `streamDepth`; Lattice ohne Boden-Clipping, `g` = scheinbare Tiefe (`kStreamVisualDepth` 0,9 m) mit weichem Rand;
  `refineShore(..., maxLen)` schneidet das Dreiecksnetz auf die Terrain-Rasterweite (0,4..3 m); Vertex-Y = Boden + `kStreamLift` + Tiefe · taper − level; echte AABB; die Normale
  bleibt +Y (das Material zieht die Wellennormale von der Mesh-Normale ab, eine echte Hangnormale ginge verloren). **Pro Vertex in der UV: die Bodensteigung in Weltachsen**
  (Landschaftsdrehung `streamTurn`, Skalierung `shoreTiltScale`), Gradient über `kStreamGradientStep` (2,5 m), damit der Quergefälle der Rinne wegfällt.
- **Rinne:** ein Stream gräbt immer (`dab()`): Tiefe `kStreamChannelFactor` (1,25) · Wassertiefe, Böschung `kStreamBankSlope` 0,8, Dig Bed macht sie tiefer/flacher. Das Wasser
  steht damit höchstens auf dem alten Boden. `digChannel` aus Schritt 12 bleibt, nur für den einen Stream-Körper.
- **Physik (`WaterQuery.cpp`):** Höhe und Strömung für Streams aus dem Boden am Punkt (`streamFlowAt`), keine Wellen, `regions()` aus dem Bodenbereich unter dem Stream.
- **Pinsel (`WaterBrush.*`):** `Params::followSlope` ersetzt `cascade/terraceStep/terraceSlope`; ein Strich auf trockenem Boden ist ein Stream **den ganzen Weg** (auf ebenem Boden steht
  er still, ein Körper, keine Naht); ein Strich auf vorhandenem Wasser lässt es, wie es ist. Min Slope und Step Height gibt es nicht mehr. Für einen Teich Follow Slope ausschalten.
- **Verbinden:** (a) Ein Stream reicht eine Zelle in angrenzendes Wasser (`ShoreClip::joinBody`, `joinReaches`: Stream reicht in flache Körper, nie umgekehrt) und liegt dort knapp
  unter deren Oberfläche; die Überlappung läuft zum fernen Rand aus. (b) Flache Körper gleicher Höhe (`kJoinLevelTolerance` 0,05 m), die sich berühren, sind EINE Fläche:
  `flatGroupLeads`, `WaterSurface.cpp` zeichnet die Gruppe mit der niedrigsten Id als ein Mesh, die anderen zeichnen nichts.
- **Material (`MatGen/main.cpp`):** Static Switch `Stream` (nur mit `Flow`): Strömung aus der UV-Steigung (`kStreamStillSlope/FallSlope/MaxSpeed` in `Physics/WaterWaves.h`),
  Zwei-Phasen-Flow-Map (1,4 s) für alle vier Chop-Schichten, die Steigung als Lean in die Wellennormale, Streifen (gestrecktes Fbm entlang der Strömung) als Schaum, **nie die
  Unterseite** (Kamera neben dem Bach). Neue Material-ID `kEngineWaterStreamMaterialId` {0x600,5}. Ohne den Switch ist der Shader der alte (stilles Wasser D3D12: alt gegen neu im Rauschen).
- **Entfernt:** `WaterFall.cpp/.h`, `test_water_fall.cpp`, `WaterSurfaceComponent::fall`, Terrassen-Tests.
- **Backends:** das Material ist ein Graph, es gibt keinen neuen Knotentyp und keinen Renderer-Code; D3D11, D3D12, Vulkan sind auf NN-WS03 (RTX 4070) gerendert (`HE_DUMP_WATERLAKE=hill|join`),
  Material `600,5 (WaterStream, shader ready)` im Log. Metal/OpenGL: nicht gerendert (Windows-Rechner).

## Handoff (Schritt 14)

- **Branch:** `claude/wasser-tool-str-mungen-und-automatische-wasserf-lle-an-steig`, Worktree `C:/hw232` (Build `C:/hw232/o`), **nicht committet, nicht gepusht**.
- **Verifiziert:** `he_tests` Wasser-Fälle (374) grün, Vollsuite siehe Ergebnis im Chat; D3D11/D3D12/Vulkan Hügel-Zeuge (`docs/img-water-stream/`); stilles Wasser alt = neu.
- **Nicht verifiziert:** Metal/OpenGL; Mausbedienung im Editor (nur Pinsel-Code und kompilierte UI); Perf der Zwei-Phasen-Chop-Schichten (nicht gemessen, 4 Schichten doppelt nur im Stream-Material);
  echte 1000-m-Landschaft (grobe Wasserzellen); ein dunkleres Band, wo ein Stream über ein See-Ende hinausläuft (Zeuge `join`).
- **Nächste Schritte:** 1. Perf messen (`scripts/perf/water_matrix.py`). 2. Band am Stream-See-Übergang entschärfen (Stream-Streifen im Überlappungsstreifen ausblenden). 3. Streams in der Wasser-Physik
  der Schwimmer (`WaterBuoyancy`) an einem Hang testen. 4. Handbuch.

### Schritt 15 (Bett, Spritzer, Richtungswechsel; auf Wunsch des Menschen)

- **Follow Slope auf vorhandenem Wasser:** ein Strich, der in einem See beginnt, malt jetzt einen **eigenen Stream** daneben (bisher setzte er den flachen See den Hang hinauf fort: ein
  ebener Pegel, den das Bett nicht füllen kann). Der Stream verbindet sich mit dem See (joinReaches), der See bleibt wie er ist. Stream-Film dichter (`kStreamVisualDepth` 1,4 m).
- **Bett bei verbundenem Wasser:** `digGroup`/`touchingBodies` in `WaterBrush.cpp`: gleich hohe flache Körper werden als EIN Gewässer ausgehoben (Lattice über die Gruppe), die Ufer-Distanz
  von Bach und See wird über die gemeinsame Kontur gemessen (kein Grat am Übergang), die Böschung schabt nichts unter Nachbarwasser.
- **Strömung nie bergauf:** `streamSlopeAt` (steilster Abstieg auf einem Kreis, nie bergauf) statt der breiten Differenz; Material und Physik lesen sie.
- **Unterseite:** jeder Landschafts-Körper trägt `deepest` in der UV (x, Marker y = -2); das Material zeigt die Unterseite nur einer Kamera, die in diesem Wasser sein kann.
- **Spritzer (`WaterSpray.h/.cpp`):** `findSprayFeet`: wo schnelles Wasser (>= 1,5 m/s) auf ruhiges trifft (flussab <= 35 % der Geschwindigkeit, Wasser dort) und mindestens 0,8 m gefallen ist
  (der Weg flussauf, solange es >= 1 m/s läuft); `sprayStrength(drop, speed)` 0..1, mit der Fallhöhe (bis 6 m) und der Geschwindigkeit. `WaterSurface.cpp`: ein Partikel-Emitter pro Fuß
  (generiertes Kind der Landschaft: `WaterSurfaceComponent::spray`, nie gespeichert), Konfiguration je Stärke (Rate, Höhe, Weite, Größe), Richtung = Ankunftsrichtung; nur neu berechnet, wenn
  sich `Field::revision` ändert. Keine neue Komponente, kein Renderer-Code.
- **Richtungswechsel im Material:** neue Knoten `Ddx`/`Ddy` (`MatNodeType`, angehängt, nie in einem If-Zweig); der Stream-Zweig misst aus den Ableitungen der Strömungsrichtung und der Weltposition,
  wie schnell sich die Richtung je Meter dreht (`turbulence`), und macht das Wasser dort unruhiger (Chop-Amplitude) und schäumt (Flecken im Streifenrauschen).
- **Nicht verifiziert:** Spritzer in Echtzeit (der Headless-Dump läuft mit großen Zeitschritten: die Wolke dort ist zu hoch); Perf; Metal/OpenGL.

### Schritt 16 (gefüllte Becken, Unterseite nur im Wasser, Wasserfall-Vorhang; auf Wunsch des Menschen, Referenzfotos von Wasserfällen)

- **Gefülltes Bett:** ein Stream liegt in der Rinne, die er gräbt (`Body::brushPatch` merkt, was jede Dab gesenkt hat); das Wasser steht jetzt bis `kStreamFreeboard` (5 cm) unter den ALTEN Boden
  (`streamFillAt`, `brushDugAt`, `streamSurfaceAt` in `WaterMesh.cpp`), also ein Becken, das mit Dig Bed 2 m tief gegraben ist, ist bis zur Wasserlinie am Rand voll (vorher ein Film auf dem Boden, der sich
  an den Wänden hochzog). Mesh, Physik (`WaterQuery`, Tiefe, Unterwasser-Kamera) und Spritzer lesen dieselbe Höhe; das Material bekommt die echte Tiefe (`deepest`).
- **Unterseite nur für eine Kamera IM Wasser:** das Material liest den Knopf `Flow` jetzt in JEDER Variante (15 Knöpfe, Flow+Tess bleibt bei 16); z/w setzt der Renderer je Frame und Fläche
  (`RenderExtractor.cpp waterCameraKnob`): die Kamera in genau diesem Körper (`UnderwaterCameraData::terrainEntity/bodyId`) darf die Unterseite sehen, jede andere bekommt z = -10 (immer Oberseite).
  So gibt es die teal Platte über einem Hang nicht mehr. Kamera im Wasser: Bild unverändert (alt = neu, D3D12). Ein Mesh, dem niemand den Knopf setzt (Ebene): wie vorher.
- **Wasserfall-Vorhang (`findFallLips`, `kFallSlope` 1,2 m/m):** wo der Boden so steil ist, dass das Wasser fällt, wirft die Oberkante jeder steilen Strecke einen Vorhang weißer Tropfen (Emitter,
  fällt `sqrt(2h/g)` lang), am Fuß übernimmt der Spritzer (`findSprayFeet`). Stärke mit der Höhe (voll ab 12 m). Sehr schnelles Wasser (>1,8 m/s) ist im Material viel weißer (mehr Streifen-Schaum).
- **Brush:** Follow Slope malt auch in einem See einen eigenen Stream daneben; auf Hang/steil/eben ergibt sich Film, Vorhang plus Spritzer, gefülltes Becken je nach Gelände, ohne Umschalten.
- **Verifiziert:** Vollsuite grün, D3D11/D3D12/Vulkan gerendert (Stream-Material `shader ready`, Bilder gleich).
- **Nicht verifiziert:** Partikel in Echtzeit (der Headless-Dump läuft mit großen Zeitschritten), Aussehen des Vorhangs gegen die Fotos, Perf, Metal/OpenGL.

### Schritt 17 (Pools, Follow Slope als Standard, Umschauen)

- **Ursache "Kante nicht erkannt":** Follow Slope war im Werkzeug standardmäßig aus (statische Einstellung, nach jedem Editorstart wieder aus): der Pinsel malte einen flachen See, der
  über die Kante gerade weiterlief. Jetzt standardmäßig AN (`TerrainTools.cpp s_waterFollow`); ausgeschaltet bleibt es der See mit festem Pegel.
- **Pools (`streamPoolOf`/`streamPoolAt`, `WaterMesh.cpp`):** eine Senke innerhalb des Streams füllt sich bis zu ihrem Überlauf (Priority-Flood über die Zellen des Körpers, vom Rand aus),
  als stille, ebene Fläche (keine Strömung, kein Lean, kein Spritzer daraus); am Hang ändert sich nichts. Mesh, Physik, Spritzer und Unterwasser-Tiefe lesen sie. Cache je Landschaft, Körper,
  Wasser-Revision und Boden-Version; eine Dab, die gräbt, erhöht die Wasser-Revision.
- **Umschauen (RMB):** das Editor-Log des Menschen zeigte Look/Capture/relMode = 1 und Fokus beim Hauptfenster; trotzdem robuster: Mouse-Grab für die ganze Dauer, und wenn der relative Modus
  nichts liefert, während sich der Zeiger auf dem Desktop bewegt, wird diese Bewegung genommen und der Zeiger zurückgesetzt (`EditorViewportNav.cpp`). Nicht interaktiv verifiziert.


### Schritt 18 (Partikel auf D3D11/D3D12/Vulkan, dichterer Vorhang, Formen statt Kugeln; auf Wunsch des Menschen)

- **Warum es fehlte:** Partikel-Emitter (`RenderWorld::particleBatches`) zeichneten nur OpenGL und Metal (instanzierte runde Sprites). D3D11/D3D12/Vulkan/Software zeichneten nichts.
- **Weg (`RenderExtractor.cpp extractParticleBatches`):** `ParticleEmitterConfig::billboard` (`ParticleGraph.h`, `ParticleBillboard` Sprite/Streak/Puff) entscheidet:
  Streak/Puff → ein **Partikel-Band** je Emitter, ein `RibbonBatch` (der Pfad der Trails) mit einem Welt-Quad je Partikel, hinten nach vorn sortiert: EIN Draw je Emitter auf allen fünf
  Backends, Graph-Material, Blended-Pass, nie Schatten/AO/GI. Das UV trägt das Partikel (`RenderWorld.h kParticleBandLevels`, `particleBandUv`): fract(uv) = Quad, floor(uv.y)/64 = Alpha,
  floor(uv.x)/64 = Seed. Sprite → wie bisher `particleBatches`, wo das Backend sie zeichnet (`setParticleSpritesCapable`, GL/Metal), sonst ein Billboard-`RenderObject` je Partikel (instanceTint).
- **Verworfen:** ein `RenderObject` je Streak-Partikel. D3D12 zeichnet ein transluzentes Graph-Material einmal JE INSTANZ (Ring 1024 je Frame) und die Distanz-Sortierung zerreißt Tint-Läufe:
  8127 Partikel wurden 4354 Draws, der Überlauf fiel auf Built-in-PBR (graue Riesendreiecke).
- **Spawn-Fläche:** `spawnHalfA/B` (Weltraum-Parallelogramm, `ParticleSystem.cpp spawnParticle`; Punkt-Emitter ziehen keine Zufallszahl mehr als vorher).
- **Material `Engine/Materials/WaterSpray.hasset`** (mat_gen Eintrag 3, `kEngineWaterSprayMaterialId` {0x602,1}, ReachFilter-Pfad + UUID): weiche Scheibe, durch fbm (Seed-versetzt, längs gestreckt)
  zerrissen, Stärke je Partikel 0,55..1,45, lit + etwas Eigenlicht. `ensureSprayMaterial` lädt per UUID, sonst per Pfad.
- **Emitter (`WaterSurface.cpp`):** Vorhang = Streaks über die ganze Kante (±0,85 m quer, Kanten 1,5 m auseinander → eine Fläche), 140..560/s, Alpha steigt 0,25 → 0,7 (glasig an der Kante, weiß unten);
  Fuß = kurze Streak-Tropfen 60..400/s; NEU Nebel = wachsende, gedrehte Puffs (0,7 → 4,5 m, dünn, steigt). Budget 30000 Partikel je Landschaft (alle Raten skaliert).
- **Zeuge:** `HE_DUMP_WATERLANDMAT=rock` (dunkles Konstant-Material) für den `fall`-Zeugen. Achtung: `shot.ps1 -Extra` setzt Env-Variablen der PowerShell-Sitzung DAUERHAFT;
  `HE_DUMP_PARTICLESECS=0` einer Kontrolle bleibt für die nächsten Läufe stehen → immer explizit setzen.
- **Verifiziert:** he_tests Vollsuite 6205/6205 Fälle grün (20 übersprungen); Help-Audit grün. Partikel laufen im Editor auch ohne Play (`gameDt = dt`); Kamera mitten im Fuß-Nebel: weicher Dunst. Fall-Zeuge auf D3D12/D3D11/Vulkan/OpenGL: Partikel an vs. aus ändert je
  ~115k von 921k Pixeln; D3D11/Vulkan gegen D3D12 547/1104 Pixel; OpenGL weicht nur so weit ab wie ohne Partikel (Himmel). Bilder `docs/img-water-stream/waterfall_*.png`. Deploy MD5-geprüft.
- **Nicht verifiziert:** Metal (Bänder laufen dort durch den Trail-Pfad, ungetestet); Echtzeit-Perf (CPU: Band-Bau ~8k Quads je Frame, nicht gemessen); Partikel-Kollision mit dem Becken
  (Tropfen sterben nach Lebenszeit, nicht an der Oberfläche).
- **Nächste Schritte:** 1. Perf im Editor messen (Profiler-Scope `RenderExtractor::extract`). 2. Soft-Particles (Tiefen-Fade an Wasser/Fels), heute harte Schnittkante. 3. Metal-Lauf auf dem Mac.

### Schritt 18b (Vorhang einheitlich, Oberkante bündig; Rückmeldung des Menschen mit Screenshot)

- **Versetzte Oberkante, Ursache:** eine Kante (`FallLip`) war die erste steile RASTER-Zelle, irgendwo bis zu einer Schrittweite die Wand hinunter (auf 2,5 m/m bis ~3,7 m tiefer), je Reihe
  anders; jede Kante hatte ihren eigenen Emitter mit Streuung ±0,12 m längs. **Fix (`WaterSpray.cpp`):** die Kante wird auf den Gelände-Knick bisektiert (Steigung über 0,5 m längs
  der Strömung kreuzt `kFallSlope`); `findFallLines` fasst Kanten zu EINER geraden Linie (`FallLine`) zusammen: Nachbarn bis `kFallLineGap` 3,2 m quer, bis `kFallLineReach` 10 m,
  wenn dazwischen durchgehend Wasser liegt (Union-Find, unabhängig von der Reihenfolge); eine Linie weiter unten in der Wand einer höheren wird verworfen (keine Stufe mehr).
  Zeuge `fall`: 3 Kanten bei x = 3,98 (± 0,01), eine Linie 7,4 m breit.
- **Pixelige Oberkante:** frisch geborene Partikel sind von vorn fast quadratisch (Geschwindigkeit zur Kamera). `ParticleEmitterConfig::fadeIn` (Anteil der Lebenszeit, Smoothstep, im Band-Extraktor):
  der Vorhang beginnt als weiche, gerade Kante, geboren GENAU auf der Linie (`spawnHalfB` = 0).
- **Streifen:** Vorhang jetzt breite, stark überlappende Streaks (0,28 → 0,8 m), fast gleich stark (Material: Stärke je Partikel 0,8..1,2 statt 0,55..1,45, Rauschen weniger längs gestreckt),
  Alpha 0,75 → 0,6, Rate je 1,5 m Kante (160..580/s). Ein Emitter je Linie (`curtainConfig`), Konfiguration wird bei jeder Wasser-Änderung neu gesetzt.
- **Verifiziert:** Vollsuite 6206/6206 grün (neu: „a wide fall is ONE line…“); Deploy MD5-geprüft; D3D12/D3D11/Vulkan/OpenGL gerendert (`waterfall_v2_*.png`), D3D11/Vulkan ~850 px von D3D12, an/aus 121k px.

### Schritt 19 (Hindernisse im Wasser: Schaum an den Mesh-Bounds bei Strömung; auf Wunsch des Menschen)

- **Komponente `WaterObstacleComponent`** (`Components/WaterObstacleComponent.h`: enabled, foam 0..4, margin m, wake m). Serialisiert als `"waterObstacle"` (SceneSerializer write/read/
  `HE_SCENE_COMPONENT_KEYS`/`isKnownComponentKey`, CellSplit Static), Inspector-Abschnitt + „Add Component ▸ Rendering ▸ Water Obstacle“, Prefab-Key, Hilfe (`Component/Water Obstacle`,
  `Water Obstacle/*`), Audit grün.
- **Schaum (`WaterObstacle.h/.cpp`):** `obstacleFootprint` = Mesh-Bounds unter der Welt-Matrix von oben (zwei flachste Halbachsen, Y-Bereich); `obstacleFoam` fragt den `WaterIndex`
  (Streams, Seen, ihre Strömung, in WELT-Koordinaten): Pegel und Strömung im Ring um den Footprint; kein Schaum, wenn trocken, Box ganz unter/über der Oberfläche, Wasser < 0,15 m/s, aus.
  Band = Superellipse (Exponent 4) um den Footprint, 48 × 4 Vertices, stromauf kurz (margin·0,7), seitlich margin, stromab + wake·Stärke (Schweif); jeder Vertex auf dem Wasserpegel + 4 cm,
  trockene Vertices ausgeblendet. UV: fract(u) Ring, floor(u)/16 Stärke; Normale geneigt = Strömung (`kObstacleFoamLevels/FlowGain` in `Physics/WaterWaves.h`, geteilt mit dem Material).
- **Zeichnen:** `RenderExtractor extractWaterObstacles` baut die Bänder JEDEN Frame (RibbonBatch, kein Asset, kein BVH; ein bewegtes Hindernis nimmt den Schaum mit). Material
  `Engine/Materials/WaterFoam.hasset` (mat_gen Eintrag 4, `kEngineWaterFoamMaterialId` {0x603,1}, ReachFilter): Fbm-Spitze, läuft mit der Strömung, dicht am Hindernis.
  Mesh ohne gespeicherte Bounds (Engine-Primitive): Bounds aus den Vertices.
- **Blend-Ebene (neu, alle 5 Backends):** `RibbonBatch::blendLayer`/`DrawCall::blendLayer`, `RenderSorter::blendedKey`: Ebene 1 (Schaum, Wasser-Gischt) wird nach JEDEM Ebene-0-Blended-Draw
  gezeichnet — sonst sortiert ein See, dessen Mitte näher an der Kamera liegt, über den Schaum am fernen Ufer. D3D11/D3D12/Vulkan über `sortBackToFront`, GL/Metal über ihren Ribbon-Schlüssel.
- **Bewusst NICHT:** das Wasser fließt nicht um das Hindernis (Feld, Meshes, Physik-Strömung, Gischt unverändert); keine Gischt an Hindernissen.
- **Zeuge:** `HE_DUMP_WATEROBSTACLE=x,z,size[;…]` stellt einen grauen Block (size × 2·size × size) mit der Komponente in den Wasserpegel. Bilder `docs/img-water-stream/obstacle_*.png`:
  See `bowl` still vs. `HE_DUMP_WATERFLOW=1.2,0` (still: kein Schaum, 201k px Unterschied), Hügel-Stream auf D3D12/D3D11/Vulkan/OpenGL (D3D11 1 px, Vulkan 966 px von D3D12).
- **Tests:** Vollsuite 6211/6211 grün; Deploy MD5-geprüft. `tests/test_water_obstacle.cpp` (Footprint, Schweif länger stromab, Negativkontrollen still/untergetaucht/darüber/trocken/aus, Extraktor-Band folgt dem Verschieben,
  Blend-Sortierung, Serializer-Rundreise); Serializer-„every component“ trägt einen Fels.
- **Nicht verifiziert:** Metal (Ribbon-Sortschlüssel geändert, ungetestet); Perf bei vielen Hindernissen (je ~200 Wasser-Abfragen pro Frame; kein Cache); Editor interaktiv.
- **Nächste Schritte:** 1. Stream-UV um Hindernisse biegen (Streifen fließen herum) über `SurfaceOptions` + Hindernis-Hash im Mesh-Key. 2. Gischt an Hindernissen in sehr schnellem Wasser.
  3. Cache der Bänder je (Entity, Matrix, Wasser-Revision), falls Profiler es zeigt.

### Schritt 20 (Rückmeldung: Schaum dort, wo das Wasser auftrifft; Strömung wie die Referenzfotos; keine Sonderregeln für Engine-Assets)

Plan in Phasen (jede baut und testet): 1. Sortier-Priorität als Material-Einstellung + Schaum-Material als Referenz ✔ — 2. Polster stromauf, Streifen ✔ —
3. Material-Eingangsknoten (Wasser-/Partikel-/Banddaten) statt versteckter UV/Normal-Kodierungen — 4. Wasser-Material als Landschafts-Referenz, Wind/Unterseite über Vertrag statt IDs —
5. Gischt über Partikel-Assets + Emitter-Modifikatoren — 6. ReachFilter-Liste weg (Referenzen tragen).
- **Phase 1:** `MaterialAsset::sortPriority` (-8..8; Output-Knoten `sortPriority`, JSON nur wenn ≠ 0, MTRL-Tail angehängt, alte Dateien = 0), Material-Editor ▸ Blend ▸ Translucent ▸ „Sort Priority“
  (+ Hilfe). `RenderSorter::blendedBefore/materialSortPriority`; `sortBackToFront(…, cm)` in D3D11/D3D12/Vulkan, GL/Metal-TPDraw `prio` (Metal ungetestet). `blendLayer` und die ID-Abfrage
  der Gischt sind WEG; Schaum/Gischt-Materialien setzen Priorität 1 im Graphen. `WaterObstacleComponent::foamMaterial` (Default Engine-Schaum, immer serialisiert, `collectAssetRefs`,
  Inspector-Slot, Hilfe); `ensureFoamMaterial` gelöscht, der Extraktor macht die referenzierte UUID resident wie jede Referenz.
- **Phase 2:** Band 48 × 5: Polster stromauf (Breite margin·(0,55 + (1,1 + 0,7·sf)·up^0,8)), Schultern, Kehrwasser + Schweif; uv.y = Facing (1 stromauf), Normale = Strömung AM Vertex
  (am Stauflächenmittel 0, außen frei, `pow(s, 0.35)`). Material WaterFoam neu (Polster/Kehrwasser/Kragen/Streifen). Bach: Streifen in DREI festen Achsen, nach |cos|^8 gemischt —
  die frühere Projektion der absoluten Weltposition auf die örtliche Richtung zeichnete Höhenlinien (30 m vom Ursprung: 1° = halbe Streifenbreite); breite + feine Skala; Belüftung nach
  Tempo. `kStreamTurnGain = 0`: die Bildschirm-Ableitung der interpolierten Richtung ist pro Dreieck konstant → Facetten (Gegenprobe: aus = glatt).
- **Zeuge:** lädt `Engine/Materials/WaterFoam.hasset` per Pfad (Dump rendert vor dem Engine-Scan; im Editor löst die UUID). Bilder `obstacle_v2_*.png`; See still vs. strömend 230k px.
- **Verifiziert:** Vollsuite 6212/6212 grün, Help-Audit grün, Deploy MD5-geprüft; D3D11 0–2 px, Vulkan ≤ 10k px von D3D12; OpenGL weicht nur in der Geländebeleuchtung ab.
- **Offen:** Phasen 3–6; Verwirbelung als glatter Vertex-Wert (kommt mit dem Wasser-Knoten); Bach-Streifen biegen nicht um Hindernisse (nur der Schaum tut es); Metal.

### Schritt 21 (Phasen 3–6: Engine-Assets sind nur noch Assets; auf Wunsch des Menschen)

- **Phase 3 — Material-Knoten** (`MatNodeType` angehängt: `WaterSurfaceData` „Water Surface“ {Shore, Depth, Slope}, `ParticleData` „Particle“ {Quad UV, Alpha, Seed},
  `FoamBandData` „Foam Band“ {Ring, Strength, Facing, Flow}; branchPure, MCP-Namen). Die Dekodierung (Normal-Neigung, UV-floor/fract) steht EINMAL im Codegen
  (`MaterialGraph.cpp`), mit den Konstanten aus `Physics/WaterWaves.h` und `ParticleGraph.h kParticleBandLevels` (von RenderWorld.h dorthin gezogen). Water, WaterSpray, WaterFoam lesen nur
  noch die Knoten; mat_gen kennt keine Kodierung mehr. Bild vorher/nachher: See 0 px, Bach Mittel 0,4/255 (Rundung).
- **Phase 4 — Wasser-Material pro Landschaft:** `Field::material` (Default Engine-Water; immer serialisiert `waterMaterial`, `sameContent`, `pristine`, `collectAssetRefs` bei Wasser,
  Inspector „Water Material“ + Hilfe). `waterMaterialFor(cm, base, …)`: Varianten (Tessellation/Flow/Stream) als Instanzen von GENAU diesem Material, UUID = base.lo + 1..4 (für die
  Engine-Water die bekannten `kEngineWater*MaterialId`); `ensureWaterMaterial(cm, id)` nur per UUID — der Pfad-Fallback ist weg. Wind: Vertrag statt IDs (`windDrivesWaves`: Material mit
  WaveA/B/C und ohne Displacement); Unterseite war schon Vertrag (Knopf „Flow“ auf WaterSurfaceComponent-Entities). `WaterSurface::kMaterialPath` entfernt.
- **Phase 5 — Gischt als Partikel-Assets:** Partikel-Graph-Pins 17–21 angehängt (Billboard, Stretch, Fade In, Spawn Half A/B). `ParticleEmitterModifiers` an `ParticleSystemComponent`
  (Rate/Lebenszeit/Größe/Alpha/Streuung/Geschwindigkeit je Achse/Spawn-Breite/Heading; runtime, nicht gespeichert; `applyEmitterModifiers` in `resolveConfig`, das jetzt Asset und Material
  per UUID resident macht). Neues Werkzeug `fx_gen` (HE_Tools) → `EngineContent/Particles/WaterCurtain|WaterDrops|WaterMist.hasset` ({0x700..0x702,1}). `Field::fallCurtain/fallDrops/
  fallMist` (Defaults = diese; serialisiert, Refs, Inspector + Hilfe; null = keine). `WaterSurface` setzt nur noch Asset + Modifikatoren (`footMods/mistMods/curtainMods`), Budget über
  den Pool der Graphen; `sprayLook/ensureSprayMaterial/kSprayMaterialPath` entfernt. Bild: Wasserfall vorher/nachher Mittel 2,6/255 (Partikel-Zufall).
- **Phase 6 — ReachFilter:** Wasser-/Gischt-/Schaum-Einträge aus `engineImplicitRoots` entfernt; der Export serialisiert jede Szene neu, die Referenzen stehen immer drin.
  Test „Export: an untouched landscape water and obstacle reach their engine assets by reference“ (6 erreicht, DisplacementDemo nicht).
- **Zeuge/Tests:** `HE_DUMP_WATERLAKE` ruft `scanContentDirectory()` (Dump lief vor dem Editor-Scan) statt Pfade vorzuladen; zwei Tests in test_water_surface scannen ebenso.
  Neu: Knoten-Codegen, eigenes Wasser-Material + Szene + Refs, Partikel-Pins, Modifikatoren, Spray je Asset/Modifikator (+ anderes Asset, null = keins), Reach.
  Bilder `docs/img-water-stream/assets_{fall,rapids,lake}_4backends.png` (D3D11 ≤ 1,2k px, Vulkan ≤ 13k px von D3D12; OpenGL wie immer nur die Geländebeleuchtung).
- **Übrige feste Engine-Referenzen (bewusst):** Default-Werte der Referenzen (WaterField.h, WaterObstacleComponent.h), Migration alter Szenen-IDs (SceneSerializer.cpp:165),
  Wasser-Ebene-Zeuge (EditorApplication.cpp ~6491), Trail-Clipmap-Textur im ReachFilter (anderes System).
- **Verifiziert:** Vollsuite 6217/6217 grün, Help-Audit grün, Deploy MD5-geprüft.
- **Nicht verifiziert:** Metal (TPDraw-Priorität, kein Mac); Editor interaktiv (Inspector-Slots, Partikel-Editor mit den neuen Pins); Perf.
- **Nächste Schritte:** 1. Aussehen mit dem Menschen abstimmen (Schaum, Bach). 2. Verwirbelung als glatter Wert der Water-Surface-Daten (Kanal fehlt). 3. Bach-Streifen um Hindernisse.

### Schritt 22 (Performance-Tests; auf Wunsch des Menschen)

- **CPU-Benchmarks** `tests/test_water_perf.cpp` (berichtet, nicht geprüft; Release, NN-WS03, Median): Wasserfall 64 m (7 Emitter, 7740 Partikel),
  Schaum 0/10/50/100 Blöcke in einem See mit 1,2 m/s, Blended-Sort 2000 Draws.

  | Messung | vorher | nachher |
  |---|---|---|
  | Bänder bauen (Extract mit − ohne Gischt) | 1,10 ms | 0,25 ms |
  | Schaum 100 Hindernisse, ruhend | 2,85 ms | 0,04 ms |
  | Schaum 100 Hindernisse, alle bewegt | 2,85 ms | 0,44 ms |
  | Partikel-Tick 7740 / Rebuild bei Wasseränderung / Sort 2000 | 0,016 / 0,40–0,50 / 0,037 ms | unverändert |

- **Bänder** (`RenderExtractor.cpp particleBand`): Distanzschlüssel einmal beim Sammeln, Radix-Sort (3 × 11 Bit) über (Schlüssel, Index) statt `std::sort` der 56-Byte-Structs
  (200 → 20 µs beim 5833er-Emitter), Quads parallel an feste Plätze (`parallel_for`), Bounds aus den Positionen + halbe Diagonale des größten Quads. Partikel auf der Kamera: Quad ohne Fläche.
- **Band-Speicher** (`BandMemory`, `recycleBands` vor `out.clear()`, `takeBand`): Vertex/Index-Puffer aller Bänder gehen pro Thread an den nächsten Walk (Größenklassen 2^c, ≤ 32 MB,
  Reste eines Walks werden freigegeben). Grund gemessen: frisch allozierte ~1 MB pro Frame = Page Faults (112 µs beim Wasserfall, 0,3 ms bei 100 Schaumbändern zu je 12 KB).
- **Schaum-Cache** (`HE::WaterFoamMemory`, `m_waterFoam`): Band pro Entity mit Schlüssel (Weltmatrix, Mesh-Bounds, foam/margin/wake, Wasser-Stempel = FNV über jede Landschaft mit
  Wasser: Entity, `Field::revision`, res, Grid-Zeiger, Body-Zahl, Weltmatrix — derselbe Schlüssel wie WaterIndex::ensureBounds). Erlaubt, weil das Band nur level/flow/wet liest, die
  nicht von `t` abhängen (Ausnahme: überlappende Landschaften, dort gewinnt bei der Abfrage die höhere Welle). Neubau parallel (Grain 2), Index nur bei Bedarf; Welt-/CM-Wechsel leert.
  Bodenänderungen bumpen `Field::revision` über `noteGroundChanged` (nur mit clipToGround; ohne folgt auch die Wasserfläche nicht).
- **Tests neu:** „extractor keeps a resting obstacle's band …“ (gecacht == frischer Extraktor und ≠ vorher nach Verschieben, margin, foam, `setPhysics`-Strömung, Boden anheben);
  „a big band is back to front over every quad …“ (3000 Partikel: Reihenfolge über alle Quads, Ecken in den Bounds, zweiter Walk aus recyceltem Speicher identisch).
- **Profiler-Scopes:** `Extract::particles`, `Extract::waterObstacles`, `WaterSurface::sprays`.
- **Echt-Loop pro Backend** (`scripts/he_perf_capture.py`, versteckter Editor aus `C:\hw232\deploy`, 2000×1125, vsync aus, 300 + 600 Frames, 3 Wiederholungen in wechselnder
  Reihenfolge; GEPAART: Differenz mit − ohne innerhalb jeder Wiederholung (direkt nacheinander gelaufen), Median [Spanne] der drei). Szenen aus dem Zeugen
  (`HE_DUMP_SAVESCENE`): Wasserfall (`HE_DUMP_WATERLAKE=fall`, Kamera 32,303,-1 / yaw −90° pitch 10°) mit vs. ohne Fall-Referenzen (null = keine Emitter); Bowl-See 1,2 m/s
  mit 25 Blöcken 1,5 m mit vs. ohne `WaterObstacleComponent` (Blöcke bleiben). Rohdaten/Skripte: `C:\hw232\_scr\perf` (nicht im Repo).

  | Backend | Gischt: Frame p50 / GPU p50 / CPU-Szenenpass | 25 Hindernisse ruhend: Frame / GPU / CPU-Szenenpass |
  |---|---|---|
  | D3D12 | +0,50 [0,46..0,53] / +0,42 / +0,17 ms | +0,00 [−0,10..+1,14] / +0,02 / +0,21 ms |
  | D3D11 | +0,62 [0,57..0,65] / – (keine Timestamps) / +0,22 ms | +0,07 / – / +0,16 ms |
  | Vulkan | +0,70 [−1,63..+0,80] / +0,76 / +0,45 ms | −0,34 / +0,56 [−0,94..+0,93] / +0,10 ms — Rauschen größer als der Effekt |
  | OpenGL | +1,24 [0,90..1,48] / +1,18 / – ms | +0,11 / +0,07 / – ms |

  Überall: `Extract::particles` 0,29–0,31 ms, `ParticleSystem` 0,02–0,03 ms, `Extract::waterObstacles` 0,02–0,03 ms. OpenGL: ein ~2-s-Hänger nur in Runde 1
  (vermutlich das Programm des Gischt-Materials beim ersten Sichtbarwerden, GL-Programm-Cache in der echten %APPDATA%; nicht im Log belegt) — Median unberührt. Die
  GL-Pass-Zuordnung ist verschoben (Summen gleich): nur die Gesamtzeit zählt. Vulkan streut zwischen Läufen um ±1,5 ms.
- **Gefunden, nicht behoben — Bänder im CPU-Szenenpass:** D3D11/D3D12/Vulkan zahlen mit Gischt +0,17–0,45 ms, mit 25 Schaumbändern +0,10–0,21 ms (~4–8 µs fix pro Band →
  bei 100 Hindernissen ~0,4–0,8 ms, zehnmal der Extract). **Widerlegt:** die Zwischenkopie `rebaseRibbonVertices` → `rebased` → gemappter Puffer (RenderWorld.h:267). Ausprobiert
  (direkt in den gemappten Puffer verschieben): Bilder identisch, Szenenpass unverändert (D3D12 +0,17 → +0,17, D3D11 +0,22 → +0,19, Vulkan +0,45 → +0,49 ms; 2 gepaarte Runden) —
  zurückgenommen. Die Kosten liegen im Absetzen der Draws selbst (Material/Pipeline je Band); nächster Schritt dort mit dem detaillierten Profiler, nicht beim Upload.
  Ein D3D11-Lauf (u1 lake_noobst) lief in den 300-s-Timeout; das Skript beendete ihn, kein Editor blieb zurück.
- **Verifiziert:** Vollsuite 6219/6222 — die 3 roten sind `test_input_gamepad` End-to-end (echtes Xbox-Pad am NN-WS03, bekannt), alles Wasser/Partikel/Rendering grün;
  Deploy nach HorizonEngineBuild MD5-geprüft (Rendering/Scene/Core/Editor).
- **Nicht verifiziert:** Metal; Vulkan-GPU-Differenz beim Schaum (streut stärker als der Effekt).
- **Cache-Sicherheit geprüft:** jeder Schreiber von Pegel/Strömung/Bodies/Zellen geht über Setter mit `++revision` (WaterField.cpp, WaterBrush-Undo :931, TerrainSync.cpp:317);
  Laden erzeugt neue Entity-Versionen (Schlüssel enthält die Version).
- **Nächste Schritte:** 1. Kosten der Bänder im Szenenpass mit dem detaillierten Profiler (oben). 2. Aussehen mit dem Menschen abstimmen. 3. Wie Schritt 21.

### Schritt 23 (der Brush macht einen Wasserfall statt verstreuter Emitter; Rückmeldung des Menschen mit Screenshot)

- **Ursache (reproduziert, Zeuge `HE_DUMP_WATERLAKE=crest`, `HE_DUMP_CRESTLEDGE=1`):** Kante und Fuß wurden UNABHÄNGIG gesucht. (a) Lippen verlangten 1,5 m flussauf
  „nicht ≥ 0,72 m/m steil“: an einer abgerundeten Kante nie erfüllt → kein Vorhang; nur ein Absatz mitten an der Wand ergab einen kurzen. (b) `findSprayFeet` setzte Gischt überall,
  wo schnelles auf ruhiges Wasser traf, schmal (±0,7 m), Tropfen mit > 6 m/s hoch → verstreute Nebel/Tropfen. Alt: runde Kante 0 Vorhänge + 5 Füße; mit Absatz 1 Vorhang (3 m) + 7 Füße.
- **Neu (`WaterSpray.h/.cpp`):** ein Fall = eine FLÄCHE: Wasserproben (~0,75 m Raster, `FaceGrid`), steil ENTLANG der Strömung ≥ `kFallSlope` (`slopeAlong`, ±0,75 m; der
  Kreis-Gradient `streamSlopeAt` las die Rinnenränder und machte jede 45°-Rampe zum „Fall“), 8-Nachbarn verbunden. Lippen = je Bin quer zur Strömung die oberste Probe der Fläche mit
  Wasser flussauf, das NICHT zur Fläche gehört (Bachanfang auf der Wand ist kein Fall; See flussauf zählt), Kante per Bisektion wie bisher. Landung: flussab bis entlang < `kFallSlope`.
  Kante in gerade Stücke (Lücke `kFallLineGap`, Douglas-Peucker über Lage/Höhe ±0,5 m und Landung ±1,5 m, ≥ 3 Lippen je Seite); Stücke einer ebenen Kante (Streuung ≤ 1 m) bündig auf eine
  Höhe. `FallLine::run/landing/landingY/slope`: Landepunkt = Kante + dir · max(Flächenlauf, Wurfweite) — EINE Quelle für Vorhang (`curtainMods`: v = run / t) und Fuß (`Fall::foot`,
  gleiche Breite). Stück unter dem Vorhang eines höheren: gehört zu diesem. Auslauf-Füße (`findRunoutFeet`) nur noch ab 1,5 m Fall, je Linie quer gebündelt, nicht wo ein Fall landet.
- **Emitter (`WaterSurface.cpp`):** Vorhang + Drops + Nebel je `Fall` aus `findFalls`, dazu Auslauf-Füße. Füße so breit wie der Vorhang (`footPieces`), Drops ≤ ~1 m hoch.
  Vorhang-Dichte nach Freiheit des Falls: voll ab `kFallFreeSlope` 2,5 m/m (Klippe), an einer runden Wand 0,3–1 (der Bach-Film zeigt das anliegende Wasser).
- **Tests neu (test_water_stream):** runde Wand = Stücke lückenlos (≤ 0,5 m), ≥ 6 m, Lippen nur oben, je Fuß genau am Landepunkt, keine weiteren Füße; Absatz = Kaskade; Simulation:
  Vorhang-Tropfen am Ende ihrer Fallzeit über ihrem Fuß (162/162). Alte Fall/Spray-Tests UNVERÄNDERT grün. Neuaufbau bei Wasseränderung 0,43 ms (vorher 0,40–0,50).
- **Bilder:** `docs/img-water-stream/falls_crest_before_after.jpg`, `falls_cliff_before_after.jpg` (D3D12). Klippe sieht aus wie vorher, Gischt jetzt über die Breite.
- **Verhaltensänderung:** keine Gischt mehr bei < 1,5 m Fall ohne Kante; Gischt-Emitter pro Fall breiter (mehr Partikel, Budget 30000 greift).
- **Nicht verifiziert:** andere Backends für Schritt 23 (nur D3D12 gerendert); Aussehen im echten Editor beim Pinseln; Metal.
- **Nächste Schritte:** 1. Aussehen mit dem Menschen (Vorhang-Dichte an runden Wänden, harte Unterkante, wo der Vorhang die Wand trifft). 2. Wie oben.
