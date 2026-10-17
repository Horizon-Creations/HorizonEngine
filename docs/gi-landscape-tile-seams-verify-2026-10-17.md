# GI: schwarze Linien an Landscape-Kachelgrenzen, Verifikation (Thema 190, Schritt 6)

Stand: 17.10.2026, Zweig `claude/gi-landscape-tile-seams`, NN-WS03 (RTX 4070), Release-Baum
`C:\hw190` (eigener Deploy). Fix siehe `docs/gi-landscape-tile-seams-fix-2026-10-13.md`.

## Ergebnis in drei Sätzen

Das Vernähen (`TerrainSystem::stitchSeams`) hält unter LOD-Wechseln, Neubauten und
nachlaufenden Refined-Stufen in **jedem** Tick, und es entfernt die gestrichelte Linie an
Nähten gemischter Detailstufe auf D3D11, D3D12 und Vulkan. Auf **D3D11/D3D12** bleiben aber
ab etwa 100 m dunkle Striche auf den Chunk-Nähten, auch bei gleicher LOD und auch im Projekt
des Menschen: das ist Defekt B aus Thema 204 (GI-G-Buffer-Tiefe `D16_UNORM`, Skirts gewinnen
den Tiefentest), nicht diese Ursache, und laut Queen-Entscheid (#101) wird er in Thema 204
behoben, nicht hier. Die Szene des Menschen ist flach (y = 0), dort erzeugt gemischte LOD keine
Stufe; ihre Linien sind auf D3D11 (ihr aktuelles RHI) also sehr wahrscheinlich Defekt B.

## Belege

| Zeuge | Ergebnis |
|---|---|
| `test_terrain_tessellation.cpp`, neu: Kameraflug über 5 Landscapes (8 m … 1 km, 2×2 … 8×8 Chunks, eines verschoben, eines tess ×2), Sculpt-Neubau nach 1/3, jeder Tick in tickWorld-Reihenfolge geprüft | größte vernähte Stufe **0 m** in allen 160 Ticks; derselbe Flug ohne Vernähen 4,7 mm … 9 cm (Gegenprobe); 52–84 Ticks mit gemischten Nähten |
| ebenda, Kamera zittert ±0,5 m über eine LOD-Grenze | 40 Flips → 40 Neubauten der genähten Kopie, gleiche Asset-ID (kein Leck), Nähte zu; Stillstand: 0 Neubauten |
| `he_tests` voll (scratch APPDATA) | 5160/5162; die 2 Fehler sind `test_inspector_ui`-Zwischenablage (bekannt, Sitzung) |
| Dump Berg `MTROUGH=1 LODSCALE=0.1`, Kamera (15, 360, 45), Log `LOD1=1 LOD2=11 LOD3=4 stitched=5` auf allen drei | Strichpixel im Nahtfenster x 360..460 y 280..700: ungenäht **334 / 334 / 330** → genäht **55 / 55 / 33** (D3D11/D3D12/Vulkan; alles-LOD0-Bild 164 = Relief) |
| Dump fern (0, 360, 320), Pitch −12, alle Chunks LOD3 (nichts zu nähen) | D3D11/D3D12: dunkle Linien auf den Nähten in allen drei Varianten, Vulkan sauber (`thema190_s6_far_d16.png`) |
| Live-Editor, Kopie des Projekts des Menschen (`live190.py`, Bodengitter aus), Editor-Kamera nah (alles LOD0) bzw. 900 m weg (gemischt) | D3D-Pixel > 25 dunkler als Vulkan: nah 390 → 60 m 1 200–1 500 → 200 m 4 200–4 700 → Bergnaht fern 7 300–11 400; D3D11 mit GI aus = Vulkan-Niveau; nah/fern-Editorkamera gleich. Vulkan auch an gemischten Bergnähten sauber |
| Gepacktes Spiel (Export über MCP, Kamera ergänzt, GI an) | D3D11/D3D12 haben gegenüber Vulkan gestrichelte dunklere Linien (Differenzmaske); schwächerer Beleg, Landscape-Arrays fehlen im Export (weißes Ersatzmaterial) |

## Fallen, die diesen Schritt Zeit gekostet haben

- **Editor-Bodengitter** (`ViewportGroundGrid`) liegt bei y = 0 und z-fightet mit einer bei
  y = 0 flachen Landschaft: gestrichelte dunkle Linien alle 10 m, auf allen Backends, mit und
  ohne GI. Sieht aus wie der Defekt, ist es nicht. `live190.py` schaltet es ab; im Play ist es aus.
- `sculptHeights` ersetzt das fBm ganz: die Szene des Menschen ist bis auf den Bergblock exakt
  flach, gemischte LOD gibt dort keine Stufe.
- Die Szene steht auf timeOfDay 0 (Nacht); die Kopie in `C:\hw190\proj` ist auf 0,4 gesetzt.
- Ein Export ohne `EngineContentCache` im APPDATA des exportierenden Editors packt die
  Landscape-Arrays nicht; das Kopieren des Caches (`EXPORT190_CACHE`) half hier auch nicht.
- Development-Exporte zeigen ~10 s ein Logo-Intro: Spiel-Aufnahme erst nach ≥ 25 s.

## Handoff

- **Topic / step:** 190, Schritt 6 (Streaming/LOD-Verifikation, Build, Tests, D3D/Vulkan)
- **Branch / commits:** `claude/gi-landscape-tile-seams`, `c151049c` (Tests + Skripte), Doku-Commit danach
- **Done:** Sweep-/Flip-Flop-Tests; Release-Build Editor+Spiel+Tests; voller he_tests;
  Dump-A/B D3D11/D3D12/Vulkan nah/fern; Live-Editor auf dem Projekt des Menschen (Kopie);
  gepacktes Spiel D3D11/D3D12/Vulkan.
- **Verified:** wie Tabelle oben, mit Gegenproben (NOSTITCH, GI aus, Vulkan als Referenz).
- **Not verified:** Tessellation *mit* Displacement im Renderer; gepacktes Spiel mit echten
  Landscape-Texturen; GL; ob die Linien des Menschen vom 12.10. auf D3D11 entstanden (Config
  heute `RHI: 2` = D3D11, Lektion aus Thema 204 sagte Vulkan).
- **Decisions taken:** keine D16→D32-Änderung hier (Queen, Anfrage #101: bleibt bei Thema 204).
  Kein Hysterese-/Cache-Umbau fürs Flip-Flop (1 Neubau je Flip, gemessen, nicht verlangt).
- **Open for the topic:** Die Fertig-Bedingung „keine Linien auf D3D“ ist erst mit Thema 204s
  D16-Fix erfüllt. Danach dieselben Läufe wiederholen: `cap190.ps1 -Name far ... -CamX 0 -CamY 360
  -CamZ 320 -Pitch -12` und `live190.py ... 2 far` gegen `... 1 far`, Kennzahl D3D-dunkler-als-Vulkan.
- **Next steps:** 1. Thema 204 Schritt 2 (D16→D32 im D3D11/D3D12-GI-G-Buffer) umsetzen lassen.
  2. Danach D3D-Abnahme mit den Skripten hier wiederholen, Kennzahlen gegen Vulkan. 3. Zweig auf
  release/0.7.0 bringen (162 Commits zurück), dann mergen.
