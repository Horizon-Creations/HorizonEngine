# Unterseite der Wasserflaeche (Thema 205, Schritt 3)

Stand 15.10.2026. Vorgabe des Menschen: die Unterseite "deutlich geiler mit caustics und durchscheinendem licht und
lichtbrechung, durchsichtig wenn man nah dran ist, ansonsten der blaue nebel". Audit (Schritt 1, Abschnitt 3): kein
Post-Pass erreicht die Unterseite, sie war die Oberseite mit nach oben zeigender Normale (graue Platte mit Flecken
der Wolkenspiegelung).

## Was sich geaendert hat

Die Unterseite lebt jetzt im Wassermaterial selbst (`mat_gen`, `undersideTerms` in `src/HE_Tools/src/MatGen/main.cpp`),
also in einem Graphen, den der Codegen fuer alle fuenf Backends uebersetzt. Keine Renderer-Aenderung.

- **Welche Seite:** `dot(Mesh-Normale, ViewDir) < 0`, die Mesh-Normale, nicht die Wellennormale (eine steile Welle
  soll kein Pixel umschalten). Jeder Ausgang waehlt per `If` zwischen Ober- und Unterseite. `If` ist
  `mix(oben, unten, 0|1)`, von oben also exakt der alte Ausdruck, solange der Unterseiten-Zweig endlich bleibt
  (Saettigung vor jedem `pow`, Epsilon vor der Wurzel, Alpha mit Untergrenze 0,04 vor der Division).
- **Snell-Fenster und Totalreflexion:** Fresnel von innen mit dem Winkel auf der LUFTseite:
  `k = 1 - 1,333^2 (1 - cos_i^2)`, `cos_t = sqrt(max(k,0))`, Schlick auf `cos_t` (0,02 senkrecht, 1 am Rand und
  darueber, stetig). `cos_i` kommt von der Wellennormale, deshalb kraeuselt sich der Fensterrand mit den Wellen.
  Durchsichtig = Alpha ≈ R: hinter der Flaeche steht schon der Himmel im Ziel.
- **Spiegel jenseits des Rands:** Wasserfarbe (`lerp(Shallow, Deep, 0,3)`), kurz hinter dem Rand heller (der
  gespiegelte Strahl zeigt steil auf den beleuchteten Boden), streifend dunkler; die Wellennormale verschiebt diese
  Grenze und gibt dem Spiegel Struktur.
- **Licht durch die Flaeche:** neuer Graph-Knoten **Sun** (Direction, Color = `heLight.sunDir.xyz` /
  `sunColor.rgb`, die alle fuenf Backends an den Material-Stellen schon fuellen). Der gebrochene Strahl
  `t = (cos_t - eta cos_i) N + eta * up` gegen die Sonne gibt Funken (pow 250) und ein breites Gluehen (pow 6), mal
  Transmission. Das gebrochene Sonnenbild sitzt sichtbar versetzt neben der Himmelssonne (`zenith`-Bild).
- **Netz an der Unterseite:** die beiden driftenden FBM-Felder der Oberseiten-Kaustik, 2,5x feiner gelesen, als
  duenne Linien (`saturate(1 - 14|f1-f2|)^3`), mal `Caustics.x` (0 schaltet es mit aus) und mit `Caustics.w`
  ueber die Entfernung ausgeblendet (kein Flimmern am Horizont). Im Fenster heller zur Sonne hin, im Spiegel halb so
  hell (die gespiegelte Bodenkaustik).
- **heLitP ausgeschaltet:** von unten steht die Sonne hinter der Normale. Basisfarbe 0, Specular 0, Roughness 1 und
  die umgedrehte Wellennormale machen den Fresnel-Himmelsterm zu 0; alles liegt in Emissive. Blend ist
  SrcAlpha/OneMinusSrcAlpha, deshalb ist Emissive durch Alpha geteilt, sonst kaeme das Licht im fast
  durchsichtigen Fenster nur zu 2 bis 10 % an.
- **Nebel:** unveraendert der Post-Pass aus Thema 185. Er schneidet den Sichtstrahl analytisch an der Ebene, die
  Unterseite wird also nach ihrer Entfernung eingefaerbt: nah klar, fern blau (`deep_up`-Bild).

Kosten: `If` ist verzweigungsfrei, jedes Wasserpixel rechnet beide Seiten. Neu sind zwei FBM und eine Handvoll pow.
Graph 1056 -> 1177 Knoten, laengste Leitung 53 -> 61 (Obergrenze 90), weiter 16 Parameter (kein neuer Knopf).

## Bilder (Metal, `scripts/underwater_look_shots.py`, neue Ansichten `zenith` und `deep_up`)

| Bild | Kamera |
|---|---|
| `docs/images/underwater-underside-up-metal.png` | 2 m tief, 35 Grad nach oben (Vorher: `underwater-look-up-metal.png`) |
| `docs/images/underwater-underside-up_nofog-metal.png` | dasselbe ohne Nebel |
| `docs/images/underwater-underside-zenith-metal.png` | 2 m tief, senkrecht nach oben: ganzes Fenster |
| `docs/images/underwater-underside-deep_up-metal.png` | 5 m tief, 50 Grad nach oben: Nebel deckt Rand und Spiegel |

## Handoff

- **Topic / step:** Thema 205, Schritt 3 von 4
- **Branch / commit:** siehe Commit "Underwater underside (Thema 205 step 3)" auf dem Themenzweig (gepusht)
- **Done:** Die Unterseite hat Snell-Fenster mit gekraeuseltem Rand, Totalreflexion, gebrochene Sonne, ein feines
  Lichtnetz; nah durchsichtig, fern im blauen Nebel. Oberseite byte-gleich.
- **Files touched:** `src/HE_Tools/src/MatGen/main.cpp` (`undersideTerms`, Ausgangs-`If`s am Ende von `waterMaterial`);
  `EditorDeps/EngineContent/Materials/Water.hasset` (neu erzeugt, md5 c81b8ae9); `MaterialGraph.h` (Enum `Sun`, v17),
  `MaterialGraph.cpp` (Registry-Zeile, Codegen-`case` neben `Wind`); `McpToolsMaterial.cpp` (Name);
  `tests/test_material_graph.cpp` (Schleifengrenze auf `Sun`, Testfall "Sun: ..."); `scripts/underwater_look_shots.py`.
- **Verified:** `mat_gen` zweimal byte-gleich. Release-Build HorizonEditor + he_tests (Mac). he_tests
  material_graph 103/103 (inkl. "Every standard node cross-compiles", shaderc an), engine_materials 11/11 (Water
  nach GLSL/MSL/HLSL/SPIR-V), underwater_camera 20/20, water_ripples 14/14; voller `ctest -j4` mit leerem HOME 289/289 (2 uebersprungen: runtime_size_app_*).
  Bilder Metal: `up` md5 f9be117e (= Audit) -> 73aa998c, `up_nofog` -> 035eb4f2; Oberseite `dry_shallow`
  vorher = nachher (c1c8dede). OpenGL `up`/`zenith`: mittlere Abweichung zu Metal 0,03 bzw. 0,13 von 255.
- **Not verified:** D3D11/D3D12/Vulkan zur Laufzeit (nur Cross-Compile in den Tests). Bewegung nur an Standbildern.
  GPU-Kosten nicht gemessen. Regen (`wet`) setzt in heLitP F0 auf 0,08 und laesst von unten etwas Spiegelung durch.
- **Decisions taken:** Unterseite im Material statt eigenem Draw (ein Graph, fuenf Backends, kein Renderer-Code);
  Nebeldichte 0,45/m NICHT geaendert: das Fenster bleibt in 2 bis 3 m klar lesbar, weil der Himmel im HDR hell ist;
  kein neuer Knopf (16/16 belegt), Netz haengt an `Caustics`.
- **Decisions suggested, not taken:** Netz der Unterseite aus der Wellenkruemmung wie der Boden-Pass (Schritt 2)
  statt aus FBM, dann bewegt es sich exakt mit den Wellen; Szenenfarbe hinter der Flaeche lesen, damit das Ufer im
  Fenster wirklich gebrochen wird (Thema 206 "Transparenz" beruehrt dasselbe).
- **Pending:** Schritt 4 (Backend-Abnahme, Screenshots gegen die Vorgabe-Bilder).
- **Traps:** `underwater_look_shots.py --editor` braucht einen ABSOLUTEN Pfad (das Skript startet mit cwd = Editor-
  Ordner). Der Editor laedt `Water.hasset` aus `out/deploy/Editor/EngineContent`: zum Abstimmen `mat_gen` laufen
  lassen und dorthin UND nach EditorDeps kopieren, kein Editor-Neubau noetig (nur Core, wenn Knoten neu sind).
- **Next steps:** 1. Schritt 4: dieselben Ansichten auf D3D11/D3D12/Vulkan (`--rhi`), gegen die Metal-Bilder hier.
  2. Bei Bedarf Netz-Staerke an der Unterseite (Faktor 3 in `undersideTerms`, Spiegel 0,5) am Menschen abstimmen.
