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
