# Auto-LOD: Abnahme und Gesamtbericht (Thema 331, Schritt 7)

Stand 08.11.2026, Metal auf dem M5 (MacBook Air). **Kein Engine-Code geaendert**, nur Messung und Bericht. Massstab ist
`docs/auto-lod-baseline-2026-10-29.md` (Schritt 1). Rohdaten: `docs/perf-audit/auto-lod-abnahme-raw-2026-11-08/`
(`*.summary.json`, Logs, `conditions.txt`, `table.txt`, die vier Laufskripte). Die Rohdaten der Baseline (`perf-raw-2026-10-29/`) sind unberuehrt.

## 1. Kurzfassung

| Ziel aus Schritt 1 | Ergebnis (Endkonfiguration: Kette + gebackener Impostor) | erreicht? |
|---|---|---|
| Hauptpass **unter ~6 Mio Dreiecke** | **0,14 Mio** (Kette allein: 23,40 Mio; handgemachtes v5: 6,28 Mio) | **ja** (nur die Endkonfiguration; v5 knapp nicht) |
| Schatten + SSAO **unter ~2 ms** | 0,84 + 1,26 = 2,10 / 0,78 + 1,17 = 1,95 / 0,79 + 1,17 = 1,96 ms | **ja, an der Grenze** (2 von 3 Laeufen knapp darunter, einer 0,1 darueber; Kette allein 23 bis 24 ms: nein) |
| **60 fps** | Standard-RenderScale, Zaehler aus: 41,1 / 64,1 / 50,0 fps (p50-Frametime 14,9 / 10,4 / 15,6 ms); Pass-p50 bei RS2 **27,1 bis 29,8 ms** (= 34 bis 37 fps GPU-seitig) | **nein, nicht verlaesslich** (1 von 3 Laeufen im Mittel ueber 60, die anderen nicht; RS2-GPU-Zeit klar ueber 16,7 ms) |

* GPU gesamt (Pass-p50, RS2): Baseline **121,0 ms** (alter Build) und **114,1 ms** (neuer Build, gleiche Szene v0) auf **27,1 bis 29,8 ms**
  in der Endkonfiguration: etwa **-74 bis -76 %** gegen v0 auf demselben Build. Der Boden ohne Foliage lag bei 20,9 ms (Schritt 1, alter Build).
* Der Gewinn kommt fast vollstaendig aus **Schatten + SSAO (43,6 -> 2,0 ms)** und der Pixel-/Dreieckszeit des Hauptpasses (68,5 -> 23,5 bis 25,7 ms).
  Die LOD-Kette allein (Schritt 3) bringt nur -27 ms; erst die Impostor-Karte (Schritt 4) bringt die Hauptpass-Zeit herunter (Ueberzeichnung, siehe Schritt 4).
* **Der Preis ist sichtbar** (Abschnitt 4): die Endkonfiguration ist deutlich lichter, flacher und dunkler als die Baseline. Das Budget ist erreicht, Bildgleichheit nicht.

## 2. Bedingungen und Abweichungen von Schritt 1

* **Binary:** Klon von `out/deploy/Editor` dieses Worktrees (HEAD `f66139a7c`, Release; Dylibs 08.11. 03:47 bis 12:07, `FoliageExtract.cpp` 11:14 -> `libHorizonRendering` 11:16),
  `selfcontain_deploy_copy.sh` angewandt (rpath `@loader_path`). `ninja -n` zeigt keine offene Uebersetzung. **Der Build-Aufruf selbst endet rot** im Schritt
  `HorizonSDKRelease` ("dev MISMATCH": das `lib/dev`-Set des Export-SDKs ist veraltet); das betrifft nur das Export-SDK, nicht den Editor. Die Editor-Binary wurde nicht neu gelinkt, sondern war schon aktuell.
* Projekt: drei APFS-Klone von `~/HorizonEngineTestAssets/foliage-test-project` (`proj` fuer v0/v5, `projchain` fuer die Kette, `projbake` fuer Kette + Impostor, weil
  die Szenen die Sidecar-Dateien des jeweiligen Projekts brauchen). Jeder Lauf nennt `--scene`. Die Szenen `chain.hescene` und `impostor.hescene` entstanden mit dem echten Editor
  (`HE_AUTOLOD_BAKE=...:chain|impostor`, Schritt 3/4) aus `v0_base`: **12 von 12 Layern ok** in beiden Backen. Szenen und Klone liegen ausserhalb des Repos.
* Wie in Schritt 1: `--cam 0,2.5,-24,3.14159,-0.15`, `HE_SKY_TIME=40000`, `--warmup 300 --frames 600`, Fenster 2840x1528, `caffeinate -d -u -i`,
  Pass-Lauf `--detailed --set RenderScale=2`, FPS-Lauf `--no-counters`. Je Zeile 2 Laeufe (v0 und Impostor: ein dritter Lauf, siehe unten).
* **Abweichungen:**
  1. **Netzbetrieb** statt Akku (Schritt 1: Akku).
  2. **Bildschirm gesperrt** (`CGSSessionScreenIsLocked=Yes` in jedem Lauf; Schritt 1: nicht gesperrt). Folge unbekannt; der FPS-Lauf rastet nicht mehr auf 16,7-ms-Stufen ein
    , anders als in Schritt 1. Die Pass-Zeiten werden davon nicht erkennbar beruehrt (v0 neu 114 ms in drei Laeufen, Tonemap-Zeuge 0,37 bis 0,43 ms gegen 0,38 bis 0,41 in Schritt 1).
  3. **Fremdlast durch andere Bees**: Load Average 2,4 bis 13,8 (`conditions.txt` je Lauf). Die GPU-Auslastung vor jedem Lauf stand bei 80 bis 99 %, war aber im Leerlauf spaeter 0 %: das ist der Nachlauf des Vorgaenger-Editors, kein fremder Verbraucher.
* **Die Pass-GPU-Zeiten streuen kaum** (v0 113,4 bis 114,6; Impostor 27,1 bis 29,8), **die fps im Pass-Lauf dagegen stark mit der CPU-Fremdlast**: v0 4,5 / 5,0 fps bei Load 10 bis 14, aber **8,3 fps bei Load 5**
  (Lauf 3) = die 8 fps der Baseline. Die 8 fps des Menschen reproduzieren sich also im Pass-Lauf nur bei ruhiger Maschine; die GPU-Zeit ist davon unabhaengig.
* **Dreiecke** = `counters_midframe.tris`, **nur Hauptpass**. Schatten-/SSAO-Dreiecke zaehlt die Engine nicht; die Spalte "Caster / AO" sind Batches der ersten Frames aus der Logzeile
  (`shadow pass instanced (first run: N casters)`, `SSAO pre-pass instanced (first run: N objects)`) und **kein Mass fuer die Passzeit** (Endkonfiguration: 144 / 144 Batches, aber Schatten 0,8 ms).

## 3. Tabelle (Pass-p50 in ms bei RenderScale 2; FPS bei Standard-RenderScale; alle Zeilen "neu" vom selben Binary)

| Zeile | Lauf | Tris Hauptpass | Draws | Caster / AO (Log) | GPU ges. | Scene | Shadow | SSAO | fps Standard-RS (p50-Frametime) | Load |
|---|---|---|---|---|---|---|---|---|---|---|
| **v0 Baseline, alter Build (geerbt, 29.10.)** | 1 / 2 | 47,75 M | 24 | 144 / 144 | 121,9 / 120,1 | 74,6 / 72,7 | 33,4 / 33,3 | 12,3 / 12,3 | 13,62 / 13,68 (67,0 / 67,2) | 19 / 41 |
| **v0, neuer Build** | 1 / 2 / 3 | 47,75 M | 24 | 144 / 144 | 114,6 / 114,4 / 113,4 | 68,9 / 68,6 / 68,1 | 32,1 / 32,1 / 31,9 | 11,6 / 11,7 / 11,5 | 13,68 (59,7) / 13,86 (72,4) / 13,84 (72,5) | 10,5 / 12,3 / 5,0 |
| **v5 (handgemacht)** | 1 / 2 | 6,28 M | 28 | - / 91 | 55,6 / 54,4 | 52,1 / 51,2 | 0,76 / 0,73 | 0,94 / 0,93 | 43,7 (12,5) / 49,2 (15,2) | 10,3 / 9,6 |
| Kette (Schritt 3, ohne Karte) | 1 / 2 | 23,40 M | 24 | 72 / 72 | 87,7 / 86,5 | 62,3 / 61,6 | 17,1 / 16,5 | 6,6 / 6,5 | 21,2 (41,2) / 21,5 (46,7) | 12,0 / 4,0 |
| **Endkonfiguration: Kette + Impostor** | 1 / 2 / 3 | **0,14 M** | 25 | 144 / 144 | **29,8 / 27,1 / 27,3** | 25,7 / 23,6 / 23,5 | 0,84 / 0,78 / 0,79 | 1,26 / 1,17 / 1,17 | 41,1 (14,9) / 64,1 (10,4) / 50,0 (15,6) | 11,4 / 2,4 / 3,6 |

**Achtung, Spalte SSAO ist kein reines SSAO (Nachtrag 10.11., Schritt 22).** Der Integrationscheck (Thema 341) las in derselben Szene SSAO ~11 ms (Impostor) bzw. ~61 ms (v0)
statt 1,2 / 11,6 ms, bei gleicher oder niedrigerer Summe. Das ist **keine SSAO-Regression**, sondern die Zuordnung des Detailed-Captures:
* Die Metal-Passzeit gehoert zum Command Buffer, nicht zum Algorithmus: `flushPass("SSAO")` (`MetalRenderer.mm`, nach dem GIProbes-Update) bis `flushPass("Scene")` ist der Eimer "SSAO".
  Im **Forward-Pfad** liegt darin nur der SSAO-Vorpass (daher 1,2 ms hier). Im **Deferred-Tile-Pfad** liegen darin auch `EncodeGBuffer` + Decals + `EncodeDeferredResolveTile` (die eigentliche
  Szenen-Rasterung), Velocity, GI-Reflexionen/SSR und der Wasser-Pre-Pass; "Scene" ist dann nur noch der Forward-Rest (Transparente, Sky, Fog). Die Rasterzeit der Pflanzen (v0: 47 M Dreiecke) wandert so von Scene nach SSAO.
* Es hat **keinen Commit gegeben, der die Eimergrenzen seit diesem Bericht verschoben haette**: `git diff 88945637f HEAD` an `MetalRenderer.mm` enthaelt kein `flushPass`/`runSSAO`. Die Aufteilung Deferred/Forward
  existiert seit Deferred P6 (31.07.) und dem Wasser-Pre-Pass (52ffe4e39, 28.10.), beides vor diesem Bericht. Die Zeilen oben liefen mit `--set RenderScale=2` und frischem `cfgdir`
  ohne Render-Pfad-Angabe (Standard `Forward`, `IRenderer.h` `m_renderPath`); der Check lief sehr wahrscheinlich mit Deferred (persistierte Editor-Einstellung oder `HE_DUMP_RENDERPATH`). **Das ist aus den Dateien
  geschlossen, der Check-Lauf selbst wurde hier nicht wiederholt (nicht gemessen).**
* Vergleichbar sind deshalb nur **GPU ges.** und die Summe Scene + SSAO (bei gleichem Render-Pfad), nicht die Einzelspalten ueber Pfade hinweg. Wer Scene/SSAO/Shadow vergleicht, nennt den Render-Pfad dazu.
* Die Tabelle gilt unveraendert fuer den **Forward-Pfad**; der Gewinn "Schatten + SSAO 43,6 -> 2,0 ms" in Abschnitt 1 ist dort ein reiner Forward-Wert.

Bloom 0,8 bis 0,9 und Present 0,26 bis 0,28 ms in allen Zeilen gleich. fps im Pass-Lauf (Zaehler an, RS2), nur als Beleg, lastabhaengig:
v0 4,5 / 5,0 / 8,3; v5 7,6 / 16,9; Kette 5,5 / 10,9; Impostor 11,4 / 27,7 / 15,1.

**Welche Zahlen woher:**
* Zeile 1 ist **geerbt** aus Schritt 1 (Binary vom 29.10.; dieser Build existiert nicht mehr, kein Lauf mit ihm in diesem Schritt).
* Alle anderen Zeilen: **ein Binary, ein Tag, drei Projektklone**, in der Reihenfolge v0 -> v5 -> Kette -> Impostor, zweimal rundum (Lauf 1/2), Lauf 3 (v0, Impostor) hinterher bei ruhigerer Maschine.
* **Nicht verglichen werden darf** mit den Impostor-Zahlen aus Schritt 4 ("Original 114,5 / Kette 87,5 / Impostor 27,4 bis 28,4"): sie kamen aus einem eigenen Lauf-Satz; hier reproduziert (v0 114,1, Kette 87,1, Impostor 27,1 bis 29,8). Die Uebereinstimmung ist ein Beleg, dass Schritt 4 sauber gemessen hat.
* Gegen die 121,0 aus Schritt 1 ist der neue Build bei **identischer Szene v0 um ~6 ms (-5 %) schneller** (Netz statt Akku, gesperrter Schirm und andere Last sind moegliche Ursachen; **Ursache nicht bestimmt**). Deshalb steht die Endkonfiguration immer gegen v0 **auf dem neuen Build**, nicht gegen 121,0.
* Schritt 5 (Stufenwahl je Instanz nach Pflanzengroesse) ist **in diesem Binary enthalten**, wurde aber nie **einzeln** gemessen (kein Vorher-Build); v0 hat keine Stufen und laeuft unveraendert durch den alten Pfad. Hinweis aus dem Vergleich: v5 liefert hier 28 Draws / 91 AO-Objekte / 6,28 M gegen 26 / 84 / 6,30 M in Schritt 1, also leicht andere Stufenwahl bei gleicher Szene, **plausibel Schritt 5, nicht bewiesen**.

## 4. Gesehen (Frame 150, gleiche Kamera, `HE_CAPTURE_FRAME`)

Bilder: `docs/perf-audit/img/auto-lod-abnahme-v0.jpg` (Baseline), `-impostor.jpg` (Endkonfiguration), `-v5.jpg` (handgemacht). Angeschaut: alle drei.

* **Die Endkonfiguration ist als Wiese erkennbar, aber nicht gleichwertig.** Die Gras-Layer rechts sind **deutlich dunkler und weniger dicht** als in der Baseline, die Pflanzen links
  erscheinen als flache, helle Gelbgruen-Karten statt als Pflanzen mit Blattstruktur; vom Boden sieht man mehr. Ursachen sind bekannt und in Schritt 3/4 benannt: Ausduennen auf 50 % je Stufe, Impostor-Alpha-Cutoff 0,2, Normalen zum Himmel, Karte aus zwei Ansichten.
* Die Impostor-Karten starten in den kleinen Layern schon bei **5 bis 20 m**, das heisst fast das ganze Sichtfeld besteht aus Karten (0,14 M Dreiecke). Das ist der Grund fuer das Erreichen des Dreiecksziels und zugleich fuer den Qualitaetsverlust.
* Das handgemachte v5 sieht dichter und gruener aus als die Endkonfiguration (Kreuzkarten mit Layer-Material, hinten sichtbar falsches Atlas-Bild, vgl. Schritt 1), kostet aber 55 gegen 27 ms.
* **Nicht beurteilt:** Bewegung, Popping an den Stufengrenzen, Wind, Schatten der Karten, Nahansicht der Karten (Schritt 4 hat Farn-Nahaufnahmen). Kein Bildmass, nur Augenschein.

## 5. Tests (Release, `out/build/release/tests/he_tests`, HEAD `f66139a7c`)

| Filter | Faelle | Assertions | Ergebnis |
|---|---|---|---|
| `*test_auto_lod*` | 14 | 3612 | gruen |
| `*test_impostor*` | 6 | 144 | gruen |
| `*test_foliage_lod*` | 13 | 33 249 | gruen |
| `*test_meshlet_cull*` | 5 | 194 756 | gruen |

`editor_help_audit.py --check`: 2078 Eintraege, ok (dieser Schritt beruehrt keinen Editor-Knopf). Ein voller `he_tests`-Lauf wurde in diesem Schritt **nicht** gemacht (die Messlaeufe sollten nicht durch eine
Dauerlast verfaelscht werden); die Vorgaenger-Schritte melden 5993/5994, der eine rote Fall (`test_hpak` Disk registry, 7/10 != 2) ist die bekannte Mac-Umgebung.

## 6. Befunde (kein Code geaendert)

1. **`cmake --build ... --target HorizonEditor` endet rot** wegen `HorizonSDKRelease` ("dev MISMATCH", `scripts/build_engine_sdk.py`), auch bei Wiederholung; die Editor-Binary ist davon unabhaengig aktuell. Wer ein Skript auf `BUILD_RC=0` wartet, scheitert hier. Ursache nicht untersucht (veraltetes `EngineSDK/lib/dev` im Deploy-Ordner).
2. **Das Schatten-Batch-Log passt nicht zur Passzeit**: Endkonfiguration meldet 144 Caster/144 AO-Objekte (wie v0), der Schattenpass kostet aber 0,8 ms (= Boden). Entweder zaehlt die Zeile Batches vor der Schattenreichweiten-/Stufenauswahl, oder die Batches sind fast leer. Nicht untersucht; kein Fehler im Ergebnis, aber die Logzeile taugt nicht als Dreiecks-Ersatz.
3. **Die fps im Pass-Lauf sind lastabhaengig** (4,5 bis 8,3 fps fuer dieselbe Szene), die GPU-Zeit nicht. Die 8-fps-Aussage des Menschen ist ohne Angabe der Maschinenlast nicht reproduzierbar.

## 7. Nicht verifiziert

* Windows/D3D11/D3D12/Vulkan/OpenGL: **nicht gemessen** (nur Metal auf diesem Mac), keine Aussage.
* Schritt 5 einzeln (kein Vorher-Build, keine `FoliageStraddlers`-Messung, kein F9-Capture); Schritt 6 ist ein Werkzeug-/Testprototyp, im Renderer nicht vorhanden, nichts zu messen (Entscheidung: kein Meshlet-Culling fuer Foliage).
* GI an: nicht gemessen (optional, Zeit gespart).
* Akkubetrieb wie in Schritt 1 und entsperrter Schirm: nicht wiederholt; der Einfluss von Netz/Sperre auf die ~6 ms Differenz zu 121,0 ist nicht getrennt.
* Bildqualitaet nur nach Augenschein an einer Kamera und einem Frame; keine Bewegung, kein Pixelvergleich Karte gegen Geometrie.
* Mehr als drei Laeufe je Zeile; die fps-Zahlen streuen stark (Impostor 41 bis 64), nur die Pass-GPU-Zeit ist belastbar (+-3 ms).
* Voller `he_tests`-Lauf (siehe 5) und CI.

## 8. Nachtrag (Schritte 8 bis 11): finaler Stand der Endkonfiguration (Kette + Impostor, Metal/M5, Standard-RS)

| Stand | GPU-Summe aller Encoder (Trace) | davon Impostoren (netto) | Durchsatz ohne Vsync | GPU je Frame (Command Buffer) |
|---|---|---|---|---|
| Schritt 7 (diese Abnahme) | - (Zaehler: 27-30 ms bei RS2) | - | - | - |
| Schritt 10, `docs/auto-lod-gpu-capture-stutter-2026-11-08.md` | 14,7 ms | ca. 3,7 ms | 63-66 fps (Vsync 53) | ca. 24 ms |
| **Schritt 11, `docs/auto-lod-impostor-overdraw-2026-11-08.md`** (Karten auf die Alpha-Silhouette gestutzt) | **11,7 / 11,95 ms (-3 ms)** | **ca. 0,8 ms** | **ca. 60 fps (59-64), Drawable-gebunden** | **ca. 14 ms (-10 ms)** |

* Die Karten-Ueberzeichnung (80-98 % durchsichtige Flaeche) war der Impostor-Block; gestutzt faellt der Scene-Encoder von 5,7 auf 2,5 ms. Die fps selbst bleiben bei ca. 60, weil der gesperrte Bildschirm das Tempo ueber das Drawable-Warten vorgibt (Schritt 10); gewonnen ist GPU-Reserve. **Vsync-Durchsatz nach Schritt 11 nicht messbar** (Fremdlast, Laeufe verworfen), der Vsync-Stand 53 fps aus Schritt 10 ist nicht neu gemessen.
* Groesster Einzelblock jetzt: der Wolken-Pre-Pass (6,5 ms Encoder-Zeit), eigenes Thema.
* Bestehende Szenen mit gebackenem Impostor laden unveraendert; **die Verbesserung braucht ein erneutes "Bake Impostor"** (ersetzt die Karte an Ort und Stelle, UUIDs bleiben).

## Handoff

- **Topic / step:** 331 Auto-LOD, Schritt 7 von 7 (Abnahme). Branch `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`.
- **Done:** Messung der Baseline-Szene v0 auf dem neuen Build, v5, Kette, Kette + Impostor (je 2 bis 3 Pass- und FPS-Laeufe), Bilder, Tests, dieser Bericht.
- **Result in one line:** Dreiecksziel (<6 M) erreicht (0,14 M), Schatten+SSAO (<2 ms) an der Grenze erreicht (1,95 bis 2,10), 60 fps nicht erreicht (RS2 27 ms GPU; Standard-RS 41 bis 64 fps); Preis: sichtbar lichtere, flachere Wiese.
- **Files:** `docs/auto-lod-abnahme-2026-11-08.md`, `docs/perf-audit/auto-lod-abnahme-raw-2026-11-08/` (Rohdaten, Skripte), `docs/perf-audit/img/auto-lod-abnahme-{v0,v5,impostor}.jpg`. Kein `src/`.
- **Verified:** alle Zahlen aus `*.summary.json`; vier Test-Filter gruen; Editor-Binary aktuell (`ninja -n`, Zeitstempel); 12/12 Layer gebacken; alle drei Bilder angesehen.
- **Not verified:** Abschnitt 7 (D3D/Vulkan/GL, Schritt 5 einzeln, GI, Akku/Schirm, Bewegung, voller he_tests, CI).
- **Decisions taken:** Hauptzahl Pass-p50 bei RS2 gegen v0 auf demselben neuen Build; drei Projektklone (Sidecar-Dateien); dritter Lauf fuer v0/Impostor bei ruhiger Maschine.
- **Decisions suggested, not taken:** Qualitaet vor dem Merge nachziehen (Ausduennung 50 % nur fern, Dither/Blend Stufe -> Karte, Impostor-Beleuchtung); Karten spaeter starten lassen (128 px Pixelhoehe -> ggf. 192) und dafuer messen, wie viel der 27 ms (Boden 20,9) noch zu holen ist; 60 fps braucht Hauptpass-Pixelkosten, nicht Dreiecke.
- **Traps:** `--scene` ueberschreibt die Startszene (Klon!); jede gebackene Szene nur in ihrem Projektklon messen; `cmake --build` rot im SDK-Schritt, Editor trotzdem aktuell; fps im Pass-Lauf lastabhaengig; Schirmsperre aendert das fps-Raster; Tris-Zaehler nur Hauptpass.
- **Next steps:** 1. Queen: Merge-Entscheidung mit Abschnitt 4 (Qualitaet) vor Augen; 2. optional Schritt-5-A/B mit Vorher-Build und `FoliageStraddlers`; 3. optional GI-Lauf (ein Lauf, getrennt berichten); 4. Befund 1 und 2 pruefen.
