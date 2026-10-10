# Foliage: Messzeug und Basismessung auf Metal (Thema 163, Teil 2c)

Stand 10.10.2026, Zweig `claude/instanced-foliage-und-lod-impostor-fuer-b-ume-gras-felsen`, Basis
`202e14f2` (= `origin/release/0.7.0`). Bezug: `docs/foliage-instancing-lod-plan-2026-10-10.md`
Abschnitte 8 (Teil 2c) und 9. Im Renderer und im Scatter wurde nichts umgebaut. Neu sind ein
Profiler-Scope, ein Editor-Zeuge, eine Generator-Option und zwei Skripte. Die Zahlen unten sind die
**Vorher-Werte**, gegen die Teil 2a und die Verifikation (Schritt 5) messen.

## 1. Was dazugekommen ist

| Was | Wo | Wozu |
|---|---|---|
| Profiler-Scope `ExtractFoliage` | `RenderExtractor.cpp`, Kopf von `extractFoliage` | Die Walk-Kosten über alle `cachedInstances` standen bisher nur im Summenscope `RenderExtractor::extract` |
| Zeuge `HE_DUMP_FOLIAGETEST=<N>` | `EditorApplication.cpp`, `dumpFrameHeadless` | Flaches Terrain mit **einer** Foliage-Schicht aus N Instanzen des eingebauten Meshes. Loggt, wie viele wirklich gestreut wurden, und die Streuzeit. Dazu `HE_DUMP_FOLIAGESIZE` (Terrainseite in m, 400), `HE_DUMP_FOLIAGEDIST` (`drawDistance`, 1e6 = alles in Reichweite), `HE_DUMP_FOLIAGEMESH=cube\|sphere`, `HE_DUMP_FOLIAGESCALE=min,max` (0.5,1.0), `HE_DUMP_FOLIAGESEED` (42) |
| `gen_reference_world.py --foliage N` | `scripts/perf/` | Dieselbe Szene als `.hescene` für den echten Editor-Loop. `--foliage-extent`, `--foliage-distance`, `--foliage-mesh`, `--foliage-scale`, `--foliage-seed`; `--count` darf jetzt 0 sein. Das Terrain der Vorlage wird flach und quadratisch, die Dichte ergibt sich aus N und der Fläche, ohne Entity je Pflanze |
| `foliage_ladder.sh` | `scripts/perf/` | Eine `he_perf_capture`-Messung je N, schreibt je Lauf `<label>.conditions.txt` (Last, Bildschirmsperre, Stromsparmodus, GPU-Auslastung, Netzteil/Akku, lauteste Prozesse, vor und nach dem Lauf) |
| `foliage_ladder_table.py` | `scripts/perf/` | Tabellen aus den `*.profile.json` (Zähler eines mittleren Frames, CPU/GPU p50/p90, Scope-Summen je Frame p50, RSS). `--list` zeigt alle Scopes nach p50 |

Der Zeuge wurde auf Metal bei 10 000, 100 000, 500 000 und 1 000 000 Instanzen gelaufen: gestreut
werden genau N Instanzen (Dichte `(N + 0,5) / Fläche`, damit Rundung keine kostet). Streuzeit
einmalig: 1,1 ms (10k), 11,9 ms (100k), 54,7 ms (500k), 90,1 ms (1 Mio.). Gezeichnet in der
Zeuge-Kamera (`CAMY=40 CAMZ=-120 PITCH=-20`): `draws=3`, sichtbar 4 087 von 100 004 Objekten
bei 100k.

## 2. Messbedingungen (bitte lesen, bevor jemand Zahlen vergleicht)

| Punkt | Wert |
|---|---|
| Gerät, Backend | Apple M5 (`Mac17,4`, 10 Kerne, 24 GB), Metal, Release (`cmake-build-release`, `HE_ENABLE_SHADERC=ON`), Editor-Loop über `scripts/he_perf_capture.py`, vsync aus, `--no-counters` (nur Ganz-Frame-GPU-Zeit) |
| Fenster | 2 840 × 1 528 px (Anzeige 1 440 × 932 @ 2x) |
| Szene | `gen_reference_world.py --count 0 --foliage N` auf `landscape_noclouds.hescene`, Terrain 400 × 400 m flach, eingebauter Würfel, Skala 0,5 bis 1,0, keine Lichter, kein Mesh-Entity |
| Kamera | `--cam 0,30,0,0,-0.2`: mitten im Feld, Blick entlang −Z |
| Fälle | **alles in Reichweite** (`drawDistance` 1e6) und **rund 20 % in Reichweite** (`drawDistance` 100 m, Kreisfläche 19,6 % des Feldes) |
| Frames | Warmup 40, Aufnahme 120 Frames; bei 500k (alles in Reichweite) Warmup 30, 60 Frames, weil ein Frame dort rund 1 s kostet. Jeder Fall **zweimal** (`-r1`, `-r2`) |
| Bildschirm | **gesperrt** (`CGSSessionScreenIsLocked=Yes`) in jedem Lauf. Das Fenster wird dann nicht komponiert; `Metal::NextDrawable` und `WaitForFrame` sind bei billigen Frames 7 bis 11 ms Leerlauf (Memory *perf-measurement-on-m5*). Darum ist **`Render` der Vergleichswert, nicht `CPU p50`** |
| Stromsparmodus | **an** (`lowpowermode 1`), Akkubetrieb (`Battery Power`). Die Arbeit läuft damit überwiegend auf Effizienzkernen, die absoluten ms sind eher zu hoch. Nicht umschaltbar ohne sudo; gilt für Vorher und muss für Nachher gleich sein |
| Fremdlast | Load average 2,0 bis 3,2 während der Läufe. Gleichzeitig liefen andere Hive-Instanzen, CLion (einmal 116 % CPU im Nachlauf), `claude`, Hive.app. Nichts davon war ein Build oder GPU-Lauf (alle Vollbau- und ctest-Läufe waren vorher beendet). GPU-Auslastung vor den Läufen 0 bis 5 %. Pro Lauf in `raw-foliage/*.conditions.txt` |
| Wiederholbarkeit | `Render` p50 der zwei Läufe weicht bei 100k und 500k (alles in Reichweite) um weniger als 1 % ab, bei 10k um 0,1 %, bei den 20-%-Fällen um bis 12 % bei 100k (23,4 gegen 26,2 ms) und unter 1 % bei 500k |

Die Rohzusammenfassungen (`*.summary.json`) und die Bedingungen liegen in
`docs/perf-audit/raw-foliage/`. Die `*.profile.json` (je rund 1 MB) sind nicht eingecheckt; die
Tabellen unten kommen aus ihnen.

## 3. Ergebnis

`Render` p50 in ms je Frame, Summe über alle Aufrufe des Scopes im Frame (Mittel der zwei Läufe):

| Instanzen | in Reichweite | Objekte im Extract | sichtbar (Frustum) | Draws | **Render** | Shadow-Encode | Scene-Encode | SSAO-Encode | Extract | davon `ExtractFoliage` | `reuse` | `FrustumCuller::cull` | RSS max |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (Kontrolle) | 1 | 5 | 2 | 2 | 0,9 | 0,15 | 0,10 | 0,11 | 0,03 | 0,00 | 0,00 | 0,00 | 325 MB |
| 10 000 | alle | 10 004 | 2 816 | 3 | 10,2 | 5,5 | 1,8 | 1,9 | 2,0 | 0,62 | 0,77 | 0,75 | 332 MB |
| 100 000 | alle | 100 004 | 27 944 | 3 | **153,8** | 103,3 | 25,9 | 25,7 | 22,3 | 6,2 | 9,2 | 12,6 | 470 MB |
| 500 000 | alle | 500 004 | 139 794 | **139 794** | **1 049** | 607 | 259 | 172 | 106,7 | 29,2 | 42,1 | 69,2 | 1 648 MB |
| 10 000 | rund 20 % | 1 977 | 474 | 3 | 2,9 | 1,5 | 0,43 | 0,44 | 0,47 | 0,17 | 0,15 | 0,25 | 329 MB |
| 100 000 | rund 20 % | 19 719 | 4 757 | 3 | 24,8 | 16,6 | 3,6 | 3,6 | 4,6 | **1,86** | 1,5 | 1,4 | 357 MB |
| 500 000 | rund 20 % | 98 267 | 23 558 | 3 | 197,9 | 152,8 | 23,6 | 23,1 | 25,1 | 9,4 | 8,9 | 13,3 | 555 MB |

Alle Zahlen für beide Läufe einzeln stehen in den Tabellen in Abschnitt 5.

## 4. Was die Zahlen sagen

1. **Die Kosten sind CPU-Kosten, nicht GPU-Kosten.** Die Ganz-Frame-GPU-Zeit (p50) liegt bei 100k
   (alles in Reichweite) bei 12 bis 14 ms, der `Render`-Scope auf der CPU bei 154 ms. Bei 500k stehen
   rund 50 ms GPU gegen 1 049 ms CPU. Das bestätigt den Ansatz des Plans (Abschnitt 0, 2.4): Extract,
   Culling, Sortieren und Befüllen der Instanzringe sind der Engpass, nicht das Zeichnen.
2. **Der Schatten-Pass ist der größte Posten**, 52 bis 77 % von `Render` (bei 100k alle in
   Reichweite 103 von 154 ms, bei 500k 20 % in Reichweite 153 von 198 ms). Er culled, sortiert und
   batcht die Instanzen je Kaskade neu. Das ist die Mehrfach-Kostenkette aus Plan Abschnitt 1.2 Stufe 5
   und 2.4 in Zahlen. `castsShadow` bleibt für Foliage heute immer `true`.
3. **Die Kurve ist überlinear.** Kosten je Instanz (nach Abzug der Kontrolle): 0,9 µs bei
   10k, 1,5 µs bei 100k, 2,1 µs bei 500k. Zwischen 10k und 100k wächst `Render` um den Faktor 15,
   zwischen 100k und 500k um 6,8, bei 10 bzw. 5 facher Instanzzahl. Vermutung (nicht gemessen):
   Sortieren (n log n) und Cache-Verhalten der 280-Byte-`RenderObject`s, die bei 100k schon rund 28 MB je
   Extract belegen. Die Cap-Wand des Plans (Abschnitt 0 Punkt 2) greift im Hauptpass bei 100k noch
   nicht (27 944 sichtbare Objekte, `draws = 3`); ob ein Schatten-Batch sie erreicht, ist nicht
   gemessen und kann den Sprung zwischen 10k und 100k mit erklären.
4. **500k alles in Reichweite fällt über die Cap.** `draws = 139 794 = sichtbar`: bei mehr als
   65 536 Instanzen eines Batches fällt Metal in die Schleife mit einem Draw je Instanz zurück
   (Plan Abschnitt 2.2). Das Ergebnis ist 0,95 FPS. `draws = sichtbar` passt zu diesem Rückfall; die Gate-Stelle im Metal-Renderer und eine Logzeile dazu habe ich nicht gelesen bzw. gesehen.
5. **Der Extract-Walk läuft über alle Instanzen, nicht nur über die in Reichweite.** Gleiche Zahl
   in Reichweite (rund 100k), aber fünfmal so viele Instanzen insgesamt: `ExtractFoliage` kostet 6,2 ms
   bei 100k gesamt und 9,4 ms bei 500k gesamt. Die 3,2 ms Differenz sind der Walk über die 400k
   Instanzen außerhalb der Reichweite (rund 8 ns je Instanz, Plan Abschnitt 1.2 Stufe 4). Das Ziel aus
   Plan Abschnitt 9, die Foliage-Extract-Kosten bei 100k platziert und 20 % in Reichweite unter 0,5 ms
   zu halten, hat damit die Vorher-Marke **1,86 ms** (`ExtractFoliage`); der ganze Extract kostet
   in diesem Fall 4,6 ms.
6. **Der Rest des Extracts und die Kopien wachsen mit.** `RenderExtractor::extract` ist bei 100k
   alle in Reichweite 22 ms, davon `ExtractFoliage` 6,2 ms; `RenderExtractor::reuse` (die fertige
   `RenderWorld` wird kopiert statt neu extrahiert) kommt mit 9 ms dazu. `ExtractFoliage` läuft
   einmal je Frame; die weiteren Extracts des Frames sind diese Kopien.
7. **Speicher:** der Editor wächst von 325 MB bei einer Instanz auf 470 MB (100k) und 1,65 GB (500k),
   das sind 0,7 KB (10k) bis 2,6 KB (500k) je Instanz. Darin stecken unter anderem die 64 Byte in
   `cachedInstances` und die 280-Byte-`RenderObject`s in mehreren Kopien; die Aufteilung ist nicht
   gemessen. Das RSS ist ein Maximum über den Lauf.
8. **Gleiche Zahl Objekte, anderer Preis:** 100k alle in Reichweite (154 ms) gegen 500k mit 98k
   in Reichweite (198 ms) haben fast dieselbe Objektzahl im Extract und kosten nicht dasselbe. Der
   Unterschied steckt im Schatten-Pass (153 gegen 103 ms). Vermutung, nicht geprüft: der zweite Fall
   packt die Instanzen in einen Kreis von 100 m um die Kamera, dort liegen alle in den
   Schatten-Kaskaden, im ersten liegt ein Teil jenseits der Schattenreichweite. Ein Lauf mit
   ausgeschaltetem Schatten würde das klären.

## 5. Volle Tabellen (beide Läufe)

Erzeugt mit `python3 scripts/perf/foliage_ladder_table.py raw-foliage/base-*.profile.json`
(`--skip 2`: die ersten zwei Frames fallen weg).

| run | objects | visible | draws | triangles | CPU p50 | CPU p90 | GPU p50 | GPU p90 | RSS max MB |
|---|---|---|---|---|---|---|---|---|---|
| base-1-r1 | 5 | 2 | 2 | 17408 | 12.45 | 61.85 | 6.65 | 9.76 | 325 |
| base-1-r2 | 5 | 2 | 2 | 17408 | 11.32 | 61.31 | 6.89 | 10.38 | 325 |
| base-10000-d100-r1 | 1977 | 474 | 3 | 23072 | 10.20 | 65.78 | 5.39 | 8.80 | 329 |
| base-10000-d100-r2 | 1977 | 474 | 3 | 23072 | 11.55 | 65.58 | 5.93 | 8.32 | 329 |
| base-10000-r1 | 10004 | 2816 | 3 | 51176 | 11.33 | 12.29 | 4.89 | 6.80 | 332 |
| base-10000-r2 | 10004 | 2816 | 3 | 51176 | 11.34 | 12.34 | 5.04 | 6.83 | 332 |
| base-100000-d100-r1 | 19719 | 4757 | 3 | 74468 | 24.07 | 26.86 | 5.51 | 8.23 | 357 |
| base-100000-d100-r2 | 19719 | 4757 | 3 | 74468 | 27.42 | 29.08 | 5.81 | 8.30 | 356 |
| base-100000-r1 | 100004 | 27944 | 3 | 352712 | 154.88 | 170.99 | 14.45 | 18.24 | 465 |
| base-100000-r2 | 100004 | 27944 | 3 | 352712 | 154.48 | 170.54 | 11.83 | 14.69 | 475 |
| base-500000-d100-r1 | 98267 | 23558 | 3 | 300080 | 199.13 | 226.50 | 16.69 | 23.94 | 554 |
| base-500000-d100-r2 | 98267 | 23558 | 3 | 300080 | 198.21 | 224.40 | 16.70 | 24.76 | 556 |
| base-500000-r1 | 500004 | 139794 | 139794 | 1694912 | 1053.97 | 1099.49 | 51.50 | 69.12 | 1641 |
| base-500000-r2 | 500004 | 139794 | 139794 | 1694912 | 1046.31 | 1075.91 | 48.75 | 54.57 | 1655 |

| run | Render | extract | reuse | ExtractFoliage | FrustumCuller::cull | EncodeShadowMap | EncodeScene | EncodeSSAO | NextDrawable | ExtractFoliage Aufrufe/Frame |
|---|---|---|---|---|---|---|---|---|---|---|
| base-1-r1 | 0.84 | 0.02 | 0.00 | 0.00 | 0.00 | 0.13 | 0.09 | 0.10 | 11.06 | 1 |
| base-1-r2 | 0.98 | 0.03 | 0.00 | 0.00 | 0.00 | 0.17 | 0.11 | 0.12 | 9.42 | 1 |
| base-10000-d100-r1 | 2.96 | 0.48 | 0.15 | 0.17 | 0.25 | 1.46 | 0.43 | 0.44 | 7.03 | 1 |
| base-10000-d100-r2 | 2.86 | 0.45 | 0.15 | 0.17 | 0.24 | 1.49 | 0.42 | 0.44 | 7.97 | 1 |
| base-10000-r1 | 10.22 | 1.96 | 0.77 | 0.62 | 0.75 | 5.57 | 1.84 | 1.87 | 0.04 | 1 |
| base-10000-r2 | 10.21 | 2.01 | 0.77 | 0.62 | 0.74 | 5.49 | 1.82 | 1.90 | 0.04 | 1 |
| base-100000-d100-r1 | 23.35 | 4.42 | 1.47 | 1.84 | 1.44 | 15.78 | 3.50 | 3.55 | 0.04 | 1 |
| base-100000-d100-r2 | 26.21 | 4.85 | 1.56 | 1.87 | 1.42 | 17.44 | 3.67 | 3.71 | 0.05 | 1 |
| base-100000-r1 | 153.97 | 22.06 | 9.43 | 6.21 | 12.62 | 104.80 | 25.85 | 25.83 | 0.04 | 1 |
| base-100000-r2 | 153.71 | 22.63 | 9.04 | 6.21 | 12.60 | 101.80 | 25.95 | 25.53 | 0.04 | 1 |
| base-500000-d100-r1 | 198.37 | 25.17 | 8.90 | 9.38 | 13.34 | 151.90 | 23.65 | 22.86 | 0.05 | 1 |
| base-500000-d100-r2 | 197.36 | 25.00 | 8.82 | 9.44 | 13.32 | 153.65 | 23.47 | 23.34 | 0.05 | 1 |
| base-500000-r1 | 1052.94 | 106.92 | 42.36 | 29.81 | 70.07 | 605.45 | 262.42 | 172.76 | 0.07 | 1 |
| base-500000-r2 | 1045.29 | 106.46 | 41.74 | 28.63 | 68.23 | 607.59 | 255.60 | 170.86 | 0.07 | 1 |

`CPU p50` ist bei billigen Frames nicht vergleichbar: im Fall `base-1` stecken 11 ms davon in
`NextDrawable`/`WaitForFrame` (gesperrter Bildschirm), und `CPU p90` springt dort auf 62 ms. Die
Scopes nesten (`Render` enthält die Encoder, ein Encoder seine Culls); die Spalten sind einzelne
Scope-Summen und addieren sich nicht.

## 6. Wie "nachher" gemessen wird

```sh
# privates Projekt (he_perf_capture überschreibt die Startszene):
cp -R ~/HorizonEngineProjects/Test /tmp/fol_proj/Test
# Leiter, alles in Reichweite (zweimal; zweiter Aufruf mit TAG=r2):
WARMUP=40 FRAMES=120 TAG=r1 scripts/perf/foliage_ladder.sh /tmp/fol_proj/Test/Test.heproj \
    /tmp/fol_raw base 1 10000 100000
WARMUP=30 FRAMES=60  TAG=r1 scripts/perf/foliage_ladder.sh /tmp/fol_proj/Test/Test.heproj \
    /tmp/fol_raw base 500000
# rund 20 % in Reichweite:
DIST=100 WARMUP=40 FRAMES=120 TAG=d100-r1 scripts/perf/foliage_ladder.sh \
    /tmp/fol_proj/Test/Test.heproj /tmp/fol_raw base 10000 100000 500000
python3 scripts/perf/foliage_ladder_table.py /tmp/fol_raw/base-*.profile.json
```

Regeln für einen fairen Vergleich: gleiches Gerät, Bildschirm, Stromsparmodus und Netzteil/Akku wie
in Abschnitt 2 (in `*.conditions.txt` nachlesen); keine Builds, ctest- oder GPU-Läufe nebenher;
`Render`, `ExtractFoliage`, `draws` und RSS vergleichen, nicht FPS und nicht `CPU p50`. Wer
auf einem entsperrten Mac an der Steckdose misst, hat andere absolute Zahlen und muss den Vorher-Wert
dort neu messen; das Verhältnis ist das, was zählt. Bilder: `HE_DUMP_FOLIAGETEST=<N>` über
`scripts/he_shot.py FOLIAGETEST=<N> CAMX=0 CAMY=40 CAMZ=-120 PITCH=-20 TOD=0.5`.

## 7. Grenzen dieser Messung

- **Nachtrag (Schritt 3):** `HE_DUMP_FOLIAGEMESH=sphere` zeichnet keine Kugel. Der Zeuge nimmt dafür die Id `{257, 1}`, die kein ContentManager kennt; die Backends fallen auf den
  Würfel zurück. Gemessen wurde mit dem Würfel (`cube`), das ist hier ohnehin der Fall der Tabellen; der Schalter `sphere` ist ein zweiter Würfel-Lauf mit anderer Skala, kein anderes Mesh.

- Nur Metal. GL, D3D11, D3D12 und Vulkan sind nicht gemessen (Vulkan und D3D laufen hier nicht).
- Gesperrter Bildschirm, Stromsparmodus und Akku (Abschnitt 2). Die absoluten Zahlen sind
  vermutlich schlechter als an einem entsperrten Mac an der Steckdose. Die Verhältnisse zwischen den
  Fällen dürften halten, das ist nicht geprüft.
- Der Würfel ist das einfachste Mesh (12 Dreiecke) mit eingebautem PBR-Material: das ist der Fall,
  der heute schon über den Instanzpfad läuft. Graph-Materialien (Wind, Alpha) laufen auf Metal in die
  Schleife (Plan Abschnitt 3) und sind **nicht** gemessen.
- Eine flache Fläche ohne Hangfilter; die Instanzen sind gleichverteilt. Reale Wälder mit Lichtungen
  und Dichte-Masken füllen die Kaskaden anders (siehe 4.8).
- Kein Occlusion-Culling, keine Lichter, keine Wolken, kein GI (alles Default aus bzw. nicht in der Szene).
- Der Schatten-Pass-Anteil (4.2) und die Erklärung in 4.3 und 4.8 sind Lesarten der Scope-Zeiten, keine
  Messung der Innenkosten des Schatten-Encoders (kein Scope darin).
