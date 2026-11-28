# Auto-LOD: Impostor (Kreuzkarte mit Bild) als letzte Stufe (Thema 331, Schritt 4)

Stand 08.11.2026, Metal und OpenGL auf dem M5. Baut auf `docs/auto-lod-chains-2026-10-29.md` (Schritt 3).

## 1. Was gebaut ist

* **Bake ohne Renderer.** `src/HE_Tools/src/MeshSimplify/Impostor.{h,cpp}` (HorizonImporters): rastert die Dreiecke der
  Pflanze orthografisch in zwei Ansichten (entlang -Z und entlang +X), Z-Buffer, Textur-Abtastung gegen die
  Basisfarb-Textur des Materials (Alpha-Test, Tint), 3x3 Supersampling. Ergebnis: ein Atlas 2N x N (N = 256/512/1024),
  links Ansicht A, rechts Ansicht B, **Albedo + Alpha**, Zeilen unten-oben (Konvention von `TextureAsset`), Farbe 8 Texel in
  den transparenten Rand geblutet.
  Warum kein GPU-Bild: `RenderSceneImage`/`CaptureViewport` liefern beleuchtetes, getonemapptes RGB ueber dem Himmel ohne Alpha,
  `rawOutput` gibt es nur auf Metal, `RenderAssetThumbnail` hat feste 3/4-Sicht und nimmt `mesh->texture` statt des Graph-Materials.
  Eine Karte ist zur Laufzeit ein normales beleuchtetes Mesh, ihre Textur muss Albedo sein. Der CPU-Bake ist auf jedem Backend
  gleich und laeuft in `he_tests` ohne GPU.
* **Dateien** neben dem Pflanzen-Mesh (feste Namen, ein zweiter Bake ersetzt sie an Ort und Stelle, UUIDs bleiben):
  `<stem>_Impostor.hasset` (Textur, sRGB), `<stem>_ImpostorMat.hasset` (Graph-Material, Masked, Cutoff 0,2, eine Textur),
  `<stem>_ImpostorCard.hasset` (Karte). Material ueber `Importer::writeCutoutMaterial` (`PbrMaterialImport.cpp`), also derselbe
  Graph wie ein glTF-Import mit alphaMode MASK; `importMaterial` ruft jetzt dieselbe `writeMaterial`.
* **Karte** (`Impostor::makeCard`; **seit Schritt 11 pro Ansicht auf die Alpha-Silhouette gestutzt, ca. 4 Vierecke je Ebene statt einem, siehe `docs/auto-lod-impostor-overdraw-2026-11-08.md`**; die Beschreibung hier gilt fuer die Rechteck-Form): zwei gekreuzte Quads, beide Seiten je Ebene (16 Vertices, 8 Dreiecke), Drehpunkt unten, wie
  `CrossCard`, aber **auf die Pflanze skaliert** (Halbbreite = groesster Abstand von der Achse, Hoehe = Pflanzenhoehe, je 1 %
  Rand), Ebene A mappt die linke, Ebene B die rechte Atlashaelfte. Normalen zum Himmel geneigt (0,45 seitlich, 0,89 hoch): die
  Karte wird wie ein Busch beleuchtet, nicht wie eine Wand.
* **Stufe.** `FoliageLod` traegt Mesh- und Material-UUID schon: `AutoLodApply::applyImpostorLevel` ersetzt eine letzte Stufe, die
  eine Kreuzkarte ist (Engine-Karte oder die der letzten Bake), sonst haengt es eine an. `FoliageOptions::impostorMesh/
  impostorMaterial/impostorHeight` machen `fillFoliageLayer` (Dialog "End on Cross Card") dasselbe. **Startabstand:** nie vor dem
  Abstand, in dem die Pflanze nur noch 128 px hoch ist (`impostorStartDistance`, 1080p, 60 Grad), sonst Schritt-3-Regel
  (1,8 x letzte Stufe). Im Szenenformat nichts Neues: die Stufe steht als normaler `lods`-Eintrag in der `.hescene`.
* **Editor.** Inspector, Foliage, Detail Levels: **Bake Impostor** (direkt, Groesse aus dem Dialog), Dialog Generate LODs:
  **Bake Impostor** (setzt End on Cross Card voraus) und **Impostor Size** (256/512/1024). Hilfe in `EditorHelp.cpp`.
  Undo nimmt die Stufe zurueck, die Dateien bleiben.
* **Zeugen.** `HE_AUTOLOD_BAKE=<out.hescene>:<chain|card|impostor>` (`EditorApplication.cpp`, nach `updateAssetAutosave`): fuehrt im
  echten Editor `AutoLodDialog::run` ueber alle Foliage-Layer der Projektszene aus, schreibt die Szene, beendet. Eingabe fuer
  `he_perf_capture.py --scene` und `HE_CAPTURE_FRAME`. `HE_DUMP_FOLIAGEIMPOSTOR=<ab>:<Karte>:<Material>` (Foliage-Zeuge, Dump):
  haengt die gebackene Stufe an die Witness-Schicht, Log nennt Karte/Material/Atlas "loaded".

## 2. Messung (Metal, M5, APFS-Klon, `--detailed --set RenderScale=2`, Kamera 0,2.5,-24 / Yaw pi / Pitch -0,15, `HE_SKY_TIME=40000`)

Je Zeile ein Lauf (Impostor: 4 Laeufe, 23,3 bis 25,0 ms Scene). Dreiecke = `counters_midframe.tris`, nur Hauptpass.

| Szene | Dreiecke Hauptpass | Draws | Scene p50 | Shadow p50 | SSAO p50 | GPU p50 gesamt | fps |
|---|---|---|---|---|---|---|---|
| Original (keine Stufen) | 47,75 Mio | 24 | 69,0 | 32,0 | 11,5 | 114,5 | 8,2 |
| nur LOD-Kette (Schritt 3) | 23,40 Mio | 24 | 62,5 | 16,6 | 6,5 | 87,5 | 10,7 |
| Kette + Engine-Karte (Layer-Material) | 0,14 Mio | 25 | 93,5 | 0,8 | 1,0 | 96,8 | 9,9 |
| Kette + **Impostor** | 0,14 Mio | 25 | **23,9 bis 25,0** | 0,8 | 1,2 | **27,4 bis 28,4** | **30,7 bis 31,9** |

Die Engine-Karte mit dem vollen Pflanzenmaterial (4 Texturen, 1 x 1 m gross, also grosser als die Pflanze) ist **langsamer** als die
Kette allein: Ueberzeichnung. Die Impostor-Karte (eine Textur, auf die Pflanze skaliert, Cutoff 0,2) nicht. Der Schattenpass faellt auf
0,8 ms, weil ferne Pflanzen jetzt Karten sind, die Zahl ist kein Dreieckszaehler (die Engine hat keinen fuer Schatten, siehe Baseline-Doc).

## 3. Gesehen

* `docs/perf-audit/img/auto-lod-impostor-fern-atlas.png`: der Farn-Atlas (links/rechts die zwei Ansichten, Schachbrett = Alpha 0).
  Das Farn-Mesh enthaelt zwei Pflanzen nebeneinander, beide Ansichten zeigen beide.
* `...-fern-forced-metal.png` gegen `...-fern-geometry-metal.png`: Farn-Feld, Impostor ab 1 m erzwungen gegen die echte Geometrie
  (gleiche Kamera). Form, Groesse und Farbe stimmen; die Karten sind etwas heller und flacher.
* `...-impostor-metal.png` / `...-impostor-gl.png`: derselbe Dump-Zeuge (`HE_DUMP_FOLIAGETEST=250 ... HE_DUMP_FOLIAGEIMPOSTOR=...`) auf
  Metal und OpenGL: zwei Wuerfel (Stufe 0) vorn, dahinter ab 4 m Farn-Karten mit Textur und Cutout, an denselben Stellen. Der weisse
  Boden im GL-Bild ist das GL-Terrain-Material des Dumps, nicht der Impostor.
* Gesamtszene (Baseline-Kamera): die Karten sind in Gras-Layern **dunkler und dichter-gruen als die Geometrie**, duenne Halme sind
  gestutzt (Alpha-Cutoff 0,5 liess ganze Layer verschwinden, deshalb 0,2). Nicht pixelgleich, nicht als solche gedacht.

## Handoff

- **Topic / step:** 331, step 4 of 7 (Impostor / Kreuzkarte)
- **Branch / commit:** claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un (Hash im Hive-Ergebnis, gepusht: siehe dort)
- **Done:** Editor-Aktion "Bake Impostor" backt Albedo+Alpha zweier Ansichten in einen Atlas, schreibt Karte, Material, Textur und
  setzt die Karte als letzte Stufe der Schicht. In der Foliage-Testszene zeigt Metal (Editor-Schleife, 12 Layer) und Metal+GL (Dump)
  die Karten in der Ferne. Perf siehe Tabelle: Scene 62,5 -> 24 ms gegen die reine Kette.
- **Files touched:** `src/HE_Tools/src/MeshSimplify/Impostor.{h,cpp}` (Bake, Karte, Schreiben), `PbrMaterialImport.{h,cpp}`
  (`writeCutoutMaterial`, `writeMaterial` ausgelagert), `src/HE_Scene/include/HorizonScene/AutoLodApply.h` (`applyImpostorLevel`,
  `impostorStartDistance`, Optionen), `src/HE_Editor/AutoLodDialog.{h,cpp}` (`bakeImpostor`, Dialogzeilen), `InspectorPanel.cpp`
  (Knopf), `EditorHelp.cpp`, `EditorApplication.cpp` (`HE_AUTOLOD_BAKE`, `HE_DUMP_FOLIAGEIMPOSTOR`), `tests/test_impostor.cpp`.
- **Verified:** `he_tests` (Release, aus `out/build/release`): `*test_impostor*` 6 Faelle/144 Assertions, `*test_auto_lod*` 9, `*test_auto_lod_ui*` 5,
  `*test_foliage_lod*` 10, alle gruen; voller Lauf siehe Hive-Ergebnis. `editor_help_audit.py --check` rc 0. HorizonEditor gebaut
  und gelinkt (Schritt 3 hatte ihn nie gelinkt) und im Editor-Lauf benutzt: `HE_AUTOLOD_BAKE` 12 von 12 Layern ok (je 3 Dateien).
  Bilder oben angesehen (Metal, GL). Perf: je 600 Frames, Impostor 4x wiederholt, 23,3 bis 25,0 ms Scene.
- **Not verified:** D3D11/D3D12/Vulkan (nicht gebaut, nicht gelaufen; die Stufe ist normale Szenendaten, ein ordinaeres
  Masked-Graph-Material, nichts Backend-Spezifisches in den Dateien). GL nur im Dump (Editor-Schleife auf GL haengt auf diesem Mac im
  ersten Frame bei 0 % CPU; ob das schon vor dieser Aenderung so war, ist nicht geprueft; `HE_HIDDEN_WINDOW=1` half nicht). Kein Pixelvergleich Karte gegen Geometrie.
  Der Bake auf Mac-Mesh mit gepackter (komprimierter) Textur: faellt auf Flachfarbe zurueck (Meldung im Ergebnis), nicht getestet.
  CI nicht gestartet.
- **Decisions taken:** CPU-Bake statt GPU (siehe 1). Zwei Ansichten (je eine Ebene), nicht Oktaeder, keine 8x8. Karte pro Pflanze
  skaliert statt der festen 1x1-`CrossCard` (die Layer-Instanz hat nur die Instanzskalierung, keinen Karten-Massstab). Cutoff 0,2.
  Aufloesung 512 Standard (je Ansicht), 1024 x 512 Atlas, Winkel: 2. Startabstand nach Pixelhoehe 128. Normalen zum Himmel.
  Ausgabe neben dem Pflanzen-Mesh im Projekt, nicht unter `EditorDeps/EngineContent` (pro Pflanze, nicht Engine-Inhalt; das
  Testprojekt liegt ausserhalb des Repos, deshalb sind keine gebackenen Dateien eingecheckt).
- **Decisions suggested, not taken:** Dither-/Alpha-Blend zwischen Stufe und Karte (Schritt 5, braucht ein Instanzfeld); Karten-Schatten
  (der Schattenpass zeichnet die Karte mit dem Cutout-Material, nicht geprueft, ob mit Alpha); Alpha-zu-Coverage statt Cutoff;
  `doubleSided` wird von keinem Material gespeichert (kein MTRL-Feld), die Karte traegt deshalb beide Seiten selbst.
- **Pending:** Schritt 5 (Wahl je Instanz, Bildschirmgroesse), Schritt 6 (Cluster). Impostor-Qualitaet: Grasschichten dunkler,
  der Startabstand fuer die 12 Pflanzen liegt meist bei 5 bis 20 m. Mip-Kette der Karte nur die automatische (lose Textur).
- **Traps:** `/usr/bin/grep`; `he_perf_capture.py --scene` ueberschreibt die Startszene des Klons (APFS-Klon nutzen); GL-Editor auf
  dem Mac haengt (Dump-Zeuge nehmen, `HE_DUMP_RHI=OpenGL`); `near` als Variablenname in Tests (MSVC); `pkill` ist gesperrt (eigenen
  Task mit TaskStop beenden); `HE_CAPTURE_PATH` muss absolut sein (Editor wechselt das Verzeichnis); ein Hintergrundbuild, waehrend
  man Header editiert, scheitert im SDK-Schritt (`HorizonSDKRelease`, "dev MISMATCH"): nochmal bauen.
- **Next steps:** 1. Szene mit Impostor-Stufen in der Foliage-Testszene als Variante ablegen (`scripts/perf/foliage_variants.py`).
  2. Schritt 5: Stufenwahl nach Bildschirmgroesse, dort den 128-px-Startabstand ersetzen. 3. Schattenpass der Karte pruefen.
