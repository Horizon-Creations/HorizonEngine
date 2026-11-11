# Wasser-Strömung und Wasserfall: Backend-Abnahme (Thema 232, Schritt 4 der Queen-Liste)

09.11.2026, MacBook Air M5, Release-Build von `8ce5c03e9`. Entwurf und Schritte 1 bis 5: `docs/water-flow-2026-11-09.md`.

**Umfang laut Queen:** NN-WS03 (Windows) ist offline. **D3D11, D3D12 und Vulkan sind NICHT verifiziert.** Geprüft sind nur Metal und OpenGL (macOS, GL 4.1 über Metal).

## Ergebnis

| Punkt | Metal | OpenGL | D3D11 / D3D12 / Vulkan |
|---|---|---|---|
| Build `HorizonEditor` + `he_tests` | grün (`cmake --build`, nichts mehr zu bauen, Binaries von 05:52) | (gleicher Build) | nicht verifiziert |
| `ctest` komplett | **365/365 grün** (198 s) | – | nicht verifiziert |
| Kaskaden-Zeuge (`HE_DUMP_WATERLAKE=cascade`) | Bild, 31 Körper, 30 Terrassen, Material `600,3` WaterFlow, Shader bereit | Bild, dieselbe Zählung, `glGetError=0` | nicht verifiziert |
| Perf Standwasser gegen Messung vom 21.10. | gleich (s. u.) | +2,1 ms gegen ohne Wasser, nicht schlechter als Metal | nicht verifiziert |
| Perf Flow-Variante gegen Standwasser | +0,0 bis +0,4 ms | 0 ms (Rauschen) | nicht verifiziert |

## Visueller Vergleich

`docs/water-flow-cascade-metal-2026-11-09.png` und `docs/water-flow-cascade-opengl-2026-11-09.png`: Befehl wie Schritt 5
(sauberes `HOME`, `HE_DUMP_WATERLAKE=cascade HE_DUMP_WATERSLOPE=0.8 SKYTEST=1 TOD=0.5 DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`,
`CAMX=-24 CAMY=292 CAMZ=0 PITCH=15 YAW=90`, `HE_DUMP_RHI=Metal|OpenGL`). Beide Backends zeigen dieselbe Geometrie: dreißig Stufen, durchgehender
Vorhang zwischen den Stufen, senkrechte Streifen mit der Strömung, Schaumvorfeld am Fuß. Unterschiede: GL ist etwas dunkler und der Himmel
weicht ab (kein GI, 8-Lichter-Fenster auf GL < 4.3, andere Wolkenberechnung); das sind die bekannten Backend-Unterschiede, nicht dieses Features.
Kein md5-Vergleich (Himmel und Beleuchtung unterscheiden sich je Backend, ein Gleichheits-Test wäre sinnlos).

## Performance gegen `docs/perf-water-cost-2026-10-21.md`

Methode wie dort: `scripts/perf/water_matrix.py live --plain --home <leer> --caffeinate --detailed`, 400 Frames, 2840x1528, Display wach, Bildschirm gesperrt
(`conditions.txt` je Lauf in `/tmp`, Rohdaten `docs/water-flow-backends-2026-11-09/*.summary.json`). Projekt `Test` selbst angelegt (das aus Thema 235 existiert nicht mehr).
Einzelne oder zwei Wiederholungen, keine drei: Richtwerte, keine Statistik.

| Zelle (Metal) | Ganzer Frame GPU p50 | `Scene`-Pass p50 | Bezug 21.10. |
|---|---|---|---|
| kein Wasser, near3 | 7,09 | 0,53 | 7,72 ganzer Frame |
| Standwasser, near3 (2 Läufe) | 16,17 / 16,50 | 10,52 / 10,74 | 17,84 ganzer Frame, Scene 11,4 |
| Flow-Variante, near3 | 16,36 | 10,57 | = Standwasser |
| kein Wasser, down3 | 8,05 | 0,21 | 8,15 |
| Standwasser, down3 | 15,93 | 11,26 | 17,18 |
| Flow-Variante, down3 (2 Läufe) | 16,28 / 16,15 | 11,67 / 11,52 | +0,3 ms `Scene` |

* **Das Standwasser hat sich nicht verschlechtert** (`Water.hasset` mit `Flow`-Switch AUS hat 14 Slots wie vorher, md5 `a2871829f267` im Lauf, Datei `4ab9b09f…`): `Scene` 10,5 bis 10,7 ms gegen 11,4 damals (Messung damals bei gesperrtem Display, heute wach).
* **Die Flow-Variante kostet höchstens +0,4 ms** im schlimmsten Fall (Wasser auf jedem Pixel, 4,3 MP). Sie ist eine Material-Variante je Körper, nur Körper mit `|flow| >= 0,02 m/s` zahlen sie.
* **Wasserfall-Bänder** sind wenige hundert Dreiecke je Band und ein Aufbau je Strich-Tick; hier **nicht gemessen** (kein Szenen-Zeuge mit Kaskade in der Perf-Harness: die Kaskade entsteht nur im Dump-Modus `HE_DUMP_WATERLAKE=cascade`, nicht in einer `.hescene`).
* **OpenGL** (`he_perf_capture.py --rhi OpenGL --no-counters`, near3, nur ganzer Frame): kein Wasser 191,5 FPS (5,2 ms), Standwasser 133,2 und 138,7 FPS (7,2 bis 7,5 ms), Flow 138,8 und 135,4 FPS (7,2 bis 7,4 ms). Wasser +2,1 ms (am 21.10.: +3,5 ms), Flow = Standwasser.

## Fallen dieses Laufs

1. **Der erste Lauf einer Zelle nach einem Szenenwechsel ist zu billig** (Standwasser near3 7,11 ms = ohne Wasser; Flow near3 r1 13,55 gegen 16,36 im zweiten Lauf; auf OpenGL lief der allererste Start in den 300-s-Timeout): Pipeline/Mesh noch nicht fertig, das Wasser wird teilweise gar nicht gezeichnet. Wiederholung nehmen und den `Scene`-Pass gegen "kein Wasser" prüfen, nie den ersten Lauf allein.
2. `water_matrix.py` braucht `$HE235_ROOT/proj/Test/Test.heproj` MIT Ordner `Content/` (`startupScene`); der Editor schreibt die `.heproj` beim ersten Lauf um.
3. Die Flow-Variante in der Perf-Harness: `physics.flow` per JSON-Patch auf den Körper der `plain_lake`-Szene (`"physics": {"flow": [2, 0]}`); die Wahl der Variante je Körper ist nur an `|flow|` gebunden.

## Handoff

- **Topic / step:** 232, Backend-Abnahme. Branch `claude/wasser-tool-str-mungen-und-automatische-wasserf-lle-an-steig`.
- **Done:** Build, `ctest` 365/365, Kaskaden-Bild Metal und OpenGL, Perf-Messung Metal (Standwasser, Flow-Variante) und OpenGL (ganzer Frame).
- **Verified:** alles in der Tabelle oben für Metal und OpenGL, mit eigenen Augen gesehen (Bilder gelesen, `ctest`-Ausgabe, `summary.json`).
- **Not verified:** **D3D11, D3D12, Vulkan (NN-WS03 offline)**; Perf der Wasserfall-Bänder; Editor-UI-Bedienung (Häkchen "Flow From Slope" / "Terraces on Slopes" nur kompiliert); Release-Pak-Spiel; Richtung der Strömung per Bildregistrierung; Wasserfall mit eigenem Gischt-Material (nicht gebaut).
- **Decisions:** keine neuen; Zahlen sind Richtwerte aus 1 bis 2 Läufen.
- **Next steps:** 1. Sobald NN-WS03 da ist: Schritt wiederholen für D3D11/D3D12/Vulkan (`HE_DUMP_RHI=...`, gleicher Befehl, `ctest`). 2. Handbuch-Eintrag zu Strömung/Terrassen/Wasserfall. 3. Optional Fall-Material mit Gischt.

## Windows-Abnahme (Thema 232, Schritt 14 der Hive-Liste, NN-WS03, echte RTX 4070)

11.11.2026, Release-Build von `2c7f1e8c2` (Build-Verzeichnis `o/`, Junction `cmake-build-release → o` angelegt, da der
Hive-Check-Pfad diesen Namen erwartet). **Wichtiger Umbenennungs-Hinweis:** die Terrassen/Dig-Bed-Commits aus Schritten
11/12 (`dcab26e52`, `900d24b93`, `756ca0539`), auf die dieser Schritt ursprünglich zielte, sind durch Commit `2c7f1e8c2`
(Schritte 14–23, bereits auf diesem Branch) ersetzt: siehe den "ÜBERHOLT"-Hinweis oben in `docs/water-flow-2026-11-09.md`.
*Follow Slope* malt seither einen durchgehenden **Stream** statt einer Terrassen-Leiter; der *Step-Height*-Regler ist weg.
**Dig Bed existiert weiter** (`WaterBrush.h:67`): es gräbt jetzt immer einen Kanal unter dem Stream, tiefer mit dem Regler an.
Mit der Königin abgestimmt (`hive_request` #174): diese Abnahme prüft die **aktuelle** Hügel-Zeugenszene (Stream + Dig Bed),
nicht den ausgebauten Code.

### Ergebnis

| Punkt | D3D11 | D3D12 | Vulkan | OpenGL |
|---|---|---|---|---|
| Build `HorizonEditor` + `he_tests` | grün, frisch gegen `2c7f1e8c2` gebaut (vorheriges `he_tests.exe` war 19 min älter als der Commit) | | | |
| `ctest -R test_water` | **12/12 grün** (4.1 s) | | | |
| `ctest` komplett | **369/369 grün** (177 s; ein erster Lauf ohne `vcvars64.bat` meldete `sdk_example_project` rot, weil `rc.exe` im PATH fehlte — mit vcvars 100 %, kein Produktfehler) | | | |
| Hügel-Zeuge (`HE_DUMP_WATERLAKE=hill WATERSLOPE=0.8 WATERDIG=1`), Seite + Nahaufnahme | Bild, 1 Körper, 333 Vertices bewegt, tiefster Schnitt 1,00 m, Material `600,5` WaterStream, 0 Fehler/Warnungen | (Referenz) | Bild, identische Zahlen | Bild, identische Zahlen |
| Pixel vs. D3D12 (Seite) | 16/921600 px > 8/255 (0,002 %), max 25 | – | 65/921600 (0,007 %), max 21 | 550990/921600 (59,8 %) — nur Himmel/Geländebeleuchtung, Bild geprüft |
| Pixel vs. D3D12 (Nahaufnahme) | 65/921600 (0,007 %), max 16 | – | 143/921600 (0,016 %), max 29 | 623158/921600 (67,6 %) — dito |

Die Geometrie-Kennzahlen (`HE_DUMP_WATERLAKE=cascade: 1 bodies, dig on: 333 vertices moved, deepest cut 1.00 m (3035 vertex
updates)`) sind über alle 4 Backends × 2 Kameras (8 Prozesse) **byte-identisch** im Log — die Strich-Geometrie kommt von der
CPU, das war also so zu erwarten, aber es ist ein echter A/B-Beleg, kein Bild-Zufall. `Session summary` zeigt überall
`0 warning(s), 0 error(s), 0 critical`. D3D11/Vulkan liegen im selben Rahmen wie die früheren Werte aus Schritt 21 (≤1,2k
bzw. ≤13k Pixel von D3D12); OpenGL weicht nur so weit ab wie immer (kein GI, andere Wolkenberechnung) — beide OpenGL-Bilder
gegengeprüft (Wasser, Strömungsstreifen und Dig-Bed-Kanal sehen gleich aus, nur der Himmel ist heller/wolkiger).

Screenshots: `_scr/shots/hill_side_{D3D11,D3D12,Vulkan,OpenGL}.bmp`, `hill_close_{...}.bmp` (nicht eingecheckt, Hive-Anhang
`hive_attach`).

### Performance (Hügel-Zeuge, Stream + Dig Bed, 1280×720, `he_perf_capture.py --no-counters`, vsync aus)

Methode wie `docs/water-flow-2026-11-09.md` Schritt 22: `HE_DUMP_SAVESCENE` schreibt die Hügel-Szene einmal mit sichtbarem
Wasser (`hill_water.hescene`) und einmal mit `HE_DUMP_WATERHIDEWATER=1` gebacken (`hill_nowater.hescene`, Wasser-Meshes
unsichtbar, sonst identische Szene) — der Vergleich zur alten `plain_lake`-Szene der Mac-Messung ist wegen der viel
kleineren Zeugen-Landschaft nicht eins-zu-eins, zeigt aber zum ersten Mal die Kosten der NEUEN Stream/Dig-Bed-Geometrie
selbst (vorher nie gemessen, siehe `docs/water-flow-2026-11-09.md` Schritt 12 "nicht gemessen"). 3 Wiederholungen,
wechselnde Reihenfolge (vorwärts/rückwärts/vorwärts), 300 Frames je Zelle, Kamera = Seitenansicht-Kamera des Zeugen.
Lauf 1 verworfen (erster Lauf einer Zelle ist zu billig, siehe Falle 1 unten); Median aus Lauf 2 und 3:

| Backend | GPU p50 Wasser | GPU p50 ohne Wasser | Delta GPU | CPU/Frame p50 Wasser | CPU/Frame p50 ohne Wasser | Delta CPU |
|---|---|---|---|---|---|---|
| D3D11 | 0,37 ms | 0,37 ms | **≈0** (keine Timestamps, CPU-Timer) | 0,40 ms | 0,40 ms | ≈0 |
| D3D12 | 1,21 ms | 0,51 ms | **+0,70 ms** | 1,08 ms | 0,64 ms | +0,45 ms |
| Vulkan | 0,57 ms | 0,59 ms | **≈0** (innerhalb der ±1,5-ms-Streuung, wie Schritt 22 notiert) | 0,78 ms | 0,76 ms | ≈0 |
| OpenGL | 0,47 ms | 0,46 ms | **≈0** | 0,70 ms | 0,71 ms | ≈0 |

D3D12 hat als einziges Backend echte GPU-Timestamp-Zahlen hier (D3D11 ohne Timestamps, Vulkan/OpenGL-Streuung hier größer
als der Effekt auf dieser winzigen Szene) und zeigt +0,70 ms für den Stream + den gegrabenen Kanal (1 Körper, kein
Wasserfall-Band, keine Hindernisse/Gischt in diesem Zeugen) — das passt zur Größenordnung aus der Mac-Messung (Flow-Variante
+0 bis +0,4 ms) plus etwas Dig-Bed-Kanal-Overhead. **Kein Hinweis auf eine Regression:** alle vier Backends liegen im
Sub-Millisekunden-Bereich für diese Szenengröße; die Wasserfall-Bänder (Partikel-Kosten) sind in diesem Zeugen nicht
enthalten (kein Fall auf einer reinen Hangrampe — die separat in Schritt 22 mit `HE_DUMP_WATERLAKE=fall` gemessenen
Partikel-Kosten gelten unverändert weiter).

**Fallen dieses Laufs:** (1) wie oben in Schritt 22: der erste Lauf jeder Zelle nach einem Szenenwechsel ist zu billig
(D3D11-Wasser Lauf 1 maß 0,04 ms GPU gegen 0,37 ms in Lauf 2/3) — verworfen, nie der einzige Beleg. (2)
`he_perf_capture.py`s Default-Editorpfad ist `out/deploy/Editor/...`; dieser Worktree deployt nach `C:\hw232\deploy`
(`-DDEPLOY_DIR`) — `--editor` explizit setzen. (3) `HE_DUMP_SAVESCENE` schreibt nur, wenn `HE_DUMP_QUIT` **und**
`HE_DUMP_PATH` gesetzt sind (Quit ist an `HE_DUMP_PATH` gekoppelt, `EditorApplication.cpp:1175-1179`); ohne
`HE_DUMP_PATH` bleibt der Editor headless, aber offen — zwei hängende `HorizonEditor.exe` waren die Folge, per `TaskStop`
auf die eigene Hintergrundaufgabe beendet (kein `Stop-Process` auf fremde Prozesse nötig/erlaubt).

### Handoff

- **Topic / step:** 232, Schritt 14 der Hive-Liste (D3D11/D3D12/Vulkan/OpenGL-Abnahme). Branch
  `claude/wasser-tool-str-mungen-und-automatische-wasserf-lle-an-steig`.
- **Done:** Rebuild gegen `2c7f1e8c2` (vorheriges `he_tests.exe`/`HorizonEditor.exe` war älter als der Commit); Junction
  `cmake-build-release → o`; `ctest -R test_water` 12/12; `ctest` komplett 369/369; Hügel-Zeuge (Stream + Dig Bed) auf allen
  vier Backends × 2 Kameras gerendert, Pixel-Diffs und Log-Kennzahlen verglichen; Perf-Matrix (Wasser/kein Wasser ×
  4 Backends × 3 Wiederholungen) auf der aktuellen Geometrie.
- **Verified:** alles in den Tabellen oben mit eigenen Augen/Zahlen (Bilder gelesen, `ctest`-Ausgabe, Log-Zeilen,
  JSON-Summaries).
- **Not verified:** Editor-Maus-Bedienung; Release-Pak-Spiel; Wasserfall-Bänder/Gischt in DIESEM Zeugen (kein Fall auf
  einer reinen Hangrampe — die Fall-Perf aus Schritt 22 gilt unverändert, nicht neu gemessen); Metal (lief bereits in
  früheren Schritten).
- **Decisions:** keine neuen; der Schritt wurde mit der Königin abgestimmt auf die aktuelle Stream/Dig-Bed-Geometrie
  umgelegt, statt gegen ausgebauten Terrassen-Code zu testen (`hive_request` #174, Antwort: "Ja, das ist der richtige
  Ersatz").
- **Next steps:** 1. Handbuch-Eintrag zu Strömung/Stream/Dig-Bed (noch offen aus Schritt 7 des ursprünglichen Plans).
  2. Falls gewünscht: Perf-Matrix auch für `fall`/`rapids`/`lake`-Zeugen auf der aktuellen Geometrie (Schritt 22 deckt das
  für die damalige Gischt/Hindernis-Arbeit ab, nicht für Dig-Bed-Kanäle). 3. Optional: D3D12-GPU-Counter-Aufschlüsselung
  (`--detailed`, ohne `--no-counters`) für den Stream-Pass, falls die +0,70 ms genauer lokalisiert werden sollen.
