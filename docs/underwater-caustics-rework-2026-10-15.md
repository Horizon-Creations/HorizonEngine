# Unterwasser-Kaustik ueberarbeitet (Thema 205, Schritt 2)

Stand 15.10.2026, Zweig `claude/unterwasser-postfx-wirkt-billig-caustics-wasseroberfl-chen-u`. Vorgabe: "caustics
sollten sichtbarer am rand sein da wo das gewaesser flacher ist und langsam verschwinden bis zu einer bestimmten
tiefe". Ausgangslage und Ursachen: `docs/underwater-look-audit-2026-10-15.md` (Schritt 1).

## Was sich geaendert hat

Dieselbe Formel in allen vier Kopien: GL `kUnderwaterCausticsFS` (OpenGLRenderer.cpp, traegt die Begruendung),
MSL `kUnderwaterCausticsMSL` (MetalRenderer.mm), HLSL `kUnderwaterCausticsHLSL` (HlslSources.h, D3D11 + D3D12),
Vulkan `src/HE_Rendering/shaders/postfx_underwatercaustics.frag`. Kein neuer Pass, keine neue Pipeline, kein neues
Push-Constant/CBuffer-Feld.

1. **Netz statt Linsen.** Zu den 3 + 3 Wellenzuegen der Engine kommen 10 kurze Kraeusel-Zuege (1,2 m bis 0,4 m,
   geometrisch gestuft, je um den goldenen Winkel gedreht, Tiefwasser-Geschwindigkeit), in der Schleife hergeleitet,
   also keine zweite Tabelle, die driften kann (`causticsWaveTrainsList()` und der Vulkan-Sync-Test unveraendert).
2. **Feste Fokaldistanz** `kFocal = 6 m` statt `0,6 * Tiefe + 2`: die Faltlinien von `det` sind in 10 cm Wasser so
   scharf wie in 5 m. Damit ist die Tiefenabhaengigkeit nicht mehr verkehrt herum.
3. **Tiefen-Fade**: `1 - smoothstep(0, depth, Bodentiefe)` mal eine 15-cm-Rampe an der Wasserlinie (statt
   `exp(-d/14) * smoothstep(0, 0,4, d)`). Die Kappe auf `1/|det|` geht mit der Tiefe von 5 auf 2: oben duenne
   helle Linien, tiefer breitere, mattere.
4. **Licht umverteilen**: Ausgabe `rgb` = Licht der Linien (`kGain * (J - 1)^1,5`), `a` = Anteil, den die Zellen
   dazwischen verlieren (`kDark * (1 - J)`, mit `min(Intensitaet, 1)` skaliert). D3D/Vulkan blenden den Pass schon
   immer mit ONE/INV_SRC_ALPHA (Blend des Nebels), `dst' = rgb + dst * (1 - a)` faellt also ohne Zustandsaenderung
   heraus; GL/Metal (gefalteter Pass) rechnen dasselbe als `scene * (1 - a) + rgb`.
5. **Anti-Aliasing**: `dDet` = exakter Gradient von `det` ueber S (ein `cos` je Zug) mal Bildschirmschritt von S,
   nach unten begrenzt durch `10/m * Schritt` (an Extremwerten ist der Gradient 0). Linien bleiben mindestens 2 px
   breit; wo das nicht geht (fern, streifend) blendet das Netz aus. Verworfen: Footprint allein (gepunktete
   1-px-Linien), `fwidth(det)` (2x2-Quad-Koernung). Fuer die Ableitungen von S laeuft der Shader nach dem einen
   uniformen Early-out (Sonne unter dem Horizont) fuer jedes Pixel; Himmelspixel werden auf die Kamera geparkt und
   am Ende weggewaehlt.
6. **Knopf `depth`** heisst weiter "Fade Depth", bedeutet jetzt "Tiefe, ab der das Netz weg ist" (vorher 1/e-Tiefe).
   Standard 14 m -> 10 m (`water::kDefaultCausticsDepth`, `RenderWorld.h`). Der Serializer schreibt nur
   Nicht-Standardwerte: Szenen auf Standard bekommen 10 m, eine explizit gespeicherte 14 bleibt 14 (jetzt
   Endtiefe). Hilfe-Texte (`EditorHelp.cpp`, `##wcau`) angepasst.

Unveraendert: nur bei nasser Kamera (`queryUnderwaterCamera`), Einblendung ueber die ersten 0,5 m Kameratiefe
(`causticsFadeIn`), `ndl`-Gewicht, Wellenuhr/Speed/Scale, die Unterseite (Schritt 3).

## Zahlen

`python3 scripts/underwater_caustics_coverage.py` (CPU-Nachbau beider Formeln, Standardwerte, flacher Boden):

| Bodentiefe | alt: Anteil Netz | alt: Mittel | neu: Anteil Licht > 0,05 Sonne | neu: Mittel Licht | neu: Mittel Abdunklung |
|---|---|---|---|---|---|
| 0,1 m | 0 % | 0 | 40,8 % | 0,31 | 0,057 |
| 0,5 m | 0 % | 0 | 43,0 % | 0,42 | 0,076 |
| 1 m | 0 % | 0 | 42,6 % | 0,39 | 0,074 |
| 2 m | 1,0 % | 0,0005 | 42,0 % | 0,33 | 0,069 |
| 5 m | 12,1 % | 0,018 | 38,2 % | 0,13 | 0,039 |
| 8 m | 22,6 % | 0,048 | 0 % | 0,017 | 0,008 |
| 14 m | 38,7 % | 0,082 | 0 % | 0 | 0 |

Im Bild (Metal, `scripts/underwater_look_shots.py`, Netz an gegen `HE_DUMP_UWCAUSTICS=0`, Anteil Pixel > 2/255):

| Ansicht | Schritt 1 | jetzt |
|---|---|---|
| shore (Boden 0 bis 2,8 m), ohne Nebel | 0,14 % | 69,1 % |
| shore, mit Nebel | - | 64,2 % |
| floor (Boden 4,9 m), ohne Nebel | 5,7 % | 75,2 % |
| floor, mit Nebel | - | 55,8 % |
| deep_to_shore (neu, Boden 7 bis 2 m), ohne Nebel | - | 30,5 % |
| up (Unterseite) | byte-gleich | byte-gleich (md5 c15d0851, Schritt 3) |

Metal gegen OpenGL (gleiche Ansichten): mittlere Abweichung 0,01 bis 0,59 von 255, hoechstens 0,51 % der Pixel
ueber 8/255. Bilder: `docs/images/underwater-caustics-rework-{floor,shore,shore_nofog,deep_to_shore_nofog}-metal.jpg`,
`...-floor-gl.jpg`.

## Handoff

- **Topic / step:** Thema 205, Schritt 2 von 4
- **Branch / commit:** siehe Commit "Underwater caustics rework (Thema 205 step 2)" auf dem Themenzweig (gepusht)
- **Done:** Kaustik ist ein feines Netz, im Flachen am staerksten, weich weg bis zur einstellbaren Tiefe (10 m),
  verteilt Licht um statt nur aufzuhellen, ohne Moire in der Ferne; vier Backends gleiche Formel.
- **Files touched:** OpenGLRenderer.cpp `kUnderwaterCausticsFS` (~5451-5640, Begruendung); MetalRenderer.mm
  `kUnderwaterCausticsMSL` (~1704-1830); HlslSources.h `kUnderwaterCausticsHLSL` (~489-610);
  `shaders/postfx_underwatercaustics.frag`; WaterField.h:196-235 (Default + Bedeutung `depth`); RenderWorld.h:53;
  EditorHelp.cpp `##wcau`; Blend-Kommentare in D3D11/D3D12/VulkanRenderer.cpp; `scripts/underwater_caustics_coverage.py`
  (alt/neu), `scripts/underwater_look_shots.py` (Ansicht `deep_to_shore`).
- **Verified:** Release-Build HorizonEditor + he_tests gruen (Mac). `he_tests` underwater_camera 20/20 (inkl.
  Vulkan-Wellenzug-Sync), water_query 27/27, engine_materials 11/11; volles `ctest -j4` 288/289, einziger Fehler
  `test_hpak` ist Umgebung (eigenes HOME des Mac, mit `HOME=<leer>` 54/54; Lesson 345). `.frag`: glslc + spirv-val;
  HLSL: `glslangValidator -D -V -e main -S frag` + spirv-val; GL: `validate_embedded_shaders.py` 58/58; MSL: zur
  Laufzeit im Dump kompiliert (kein "compile failed" im Log). Metal- und GL-Bilder wie oben, `editor_help_audit --check`.
- **Not verified:** D3D11/D3D12/Vulkan zur Laufzeit (kein Windows/Vulkan-Geraet hier; nur Offline-Compile). FXC selbst
  nicht gelaufen (pow/ternary auf Vektoren sind SM5-ueblich). Bewegung (Flimmern) nur an Standbildern beurteilt.
- **Decisions taken:** nur bei nasser Kamera wie bisher (Tests pinnen "net stays wet-only"); Feld `depth` nicht
  umbenannt (Serialisierung), nur umgedeutet; Kraeusel-Zuege als Formel statt Tabelle; Default-Intensitaet 0,55 belassen.
- **Decisions suggested, not taken:** Kaustik auch bei trockener Kamera (vom Ufer/Boot ins Flache, Audit Abschnitt 5):
  braucht Pass-Lauf bei trockener Kamera und eine Antwort, wie er mit der Brechung des Wassermaterials zusammengeht.
  Kaustik kennt keine Schatten (Post-Pass ohne Shadow-Map), Licht+Abdunklung landen auch im Schatten.
- **Pending:** Schritt 3 (Unterseite im Material), Schritt 4 (D3D11/D3D12/Vulkan-Abnahme, Screenshots).
- **Traps:** auf dem hellgrauen Witness-Boden ohne Nebel saettigen die Linien (kein Albedo im Post-Pass); mit Nebel und
  echter Bodentextur unauffaellig. `underwater-accept-*` aus Thema 185 sind damit veraltet. `HE_DUMP_UWCAUSTICS=0` als
  Umgebungsvariable vor `underwater_look_shots.py` liefert die Aus-Kontrolle.
- **Next steps:** 1. Schritt 3 starten (Unterseite, `up`-Ansicht ist der Zeuge). 2. In Schritt 4 auf D3D/Vulkan
  dieselben Ansichten schiessen und gegen `underwater-caustics-rework-*` vergleichen; bei FXC-Warnungen
  `kUnderwaterCausticsHLSL` pruefen.
