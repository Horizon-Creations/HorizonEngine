# Foliage: Cluster-Pfad gegen die Basis (Thema 163, Teil 2a)

Stand 10.10.2026, Zweig `claude/instanced-foliage-und-lod-impostor-fuer-b-ume-gras-felsen`. Die **Vorher-Werte** stehen in
`foliage-baseline-2026-10-10.md` (Schritt 2c), der Bauplan in `../foliage-instancing-lod-plan-2026-10-10.md`, die
Einordnung des Cluster-Pfads in dessen Abschnitt 11. Hier steht die **Messung vorher gegen nachher auf demselben Gerät, abwechselnd**.

## 1. Wie gemessen wurde, und was man davon nicht lesen darf

- **Vorher-Binary:** ein selbstständiger Klon (`cp -cR` + `scripts/perf/selfcontain_deploy_copy.sh`) des Deploys vom Stand `08a8938b`,
  dem Merge von `origin/release/0.7.0` vor dieser Arbeit. **Nachher-Binary:** der Deploy dieses Zweigs.
- **Ablauf:** `scripts/perf/foliage_ladder.sh` (der Ablauf aus 2c: `he_perf_capture`, `--no-counters`, Szene mit EINER Foliage-Schicht
  auf flachem 400-m-Feld, eingebauter Würfel, Skala 0,5 bis 1,0, Kamera mitten im Feld, Warmup 40, 120 Frames; 500k alle in Reichweite:
  30 und 60), je Fall **abwechselnd** Vorher, Nachher, Vorher, Nachher. `EDITOR=` wird je Lauf gesetzt; `EXTRA_ENV="HE_FOLIAGE_STATS=1"` für die Zähler.
  Pass 1 (zwei Wiederholungen, alle Fälle) lief vor einer kleinen Änderung der Randbucket-Schleife ("Position zuerst, Matrix nur für Treffer"),
  Pass 2 (drei Wiederholungen: 100k alle, 100k und 500k mit 20 % in Reichweite) mit dem endgültigen Binary. Die Fälle "alle in Reichweite" haben
  keinen Randbucket, die Änderung betrifft sie nicht; ihre Zahlen aus Pass 1 gelten für das endgültige Binary.
- **Bedingungen waren schlecht und nicht die der Basismessung.** Basismessung: Bildschirm gesperrt, Stromsparmodus an, Load 2 bis 3. Hier: Akkubetrieb,
  Stromsparmodus **aus**, Bildschirm in Pass 1 zur Hälfte der Läufe entsperrt und zur Hälfte gesperrt, in Pass 2 fast durchgehend gesperrt (je Lauf in `*.conditions.txt`),
  **Load average 7 bis 14,5** (andere Hive-Instanzen übersetzten mit acht Compilern), GPU-Auslastung vor und nach den Läufen **0 bis 88 %**, dazu ein geöffneter Editor des Haupt-Checkouts.
  Gleiches Binary, gleiche Szene, zwei Durchgänge: die absoluten Zeiten liegen um den Faktor 2 auseinander (100k, 20 %: vorher 23 bis 28 ms in Pass 1,
  56 bis 63 ms in Pass 2). **Absolute Millisekunden sind deshalb weder mit der Basismessung noch zwischen den Durchgängen vergleichbar.**
  Belastbar sind die Verhältnisse innerhalb der abwechselnden Paare, und die liegen in beiden Durchgängen in derselben Größenordnung.
- Verglichen wird, wie in der Basismessung, `Render` (Summe der CPU-Scope-Zeit je Frame), `RenderExtractor::extract`, `ExtractFoliage`, `draws`, RSS;
  nicht FPS und nicht `CPU p50`. **`objects` und `visible` zählen jetzt Cluster**, nicht Pflanzen.

## 2. Ergebnis

`Render` p50 in ms je Frame, die Läufe in der Reihenfolge ihrer Wiederholung (r1, r2, r3):

| Fall (Instanzen, Reichweite) | Pass | Vorher | Nachher | Verhältnis der Mediane |
|---|---|---|---|---|
| 10 000, alle | 1 | 6,7 / 12,6 | 3,2 / 2,6 | 3,3 |
| 100 000, alle | 2 | 174 / 311 / 363 | 24,2 / 18,9 / 25,1 | 12,9 |
| 100 000, alle | 1 | 291 / 202 | 11,2 / 12,8 | 20,5 |
| 100 000, rund 20 % | 2 | 61,3 / 63,1 / 56,3 | 9,4 / 9,3 / 9,1 | 6,6 |
| 100 000, rund 20 % | 1 | 23,1 / 27,6 | 3,1 / 4,4 | 6,8 |
| 500 000, rund 20 % | 2 | 213 / 414 / 447 | 17,4 / 27,9 / 28,0 | 14,8 |
| 500 000, rund 20 % | 1 | 177 / 404 | 11,1 / 19,6 | 18,9 |
| 500 000, alle | 1 | 3 122 / 3 257 | 89 / 122 | 30,3 |

Der Anteil der Foliage am Extract (`ExtractFoliage`, in ms je Frame) und der ganze Extract (`RenderExtractor::extract`):

| Fall | Pass | `ExtractFoliage` vorher | nachher | Verh. | `extract` vorher | nachher |
|---|---|---|---|---|---|---|
| 10 000, alle | 1 | 0,26 / 0,62 | 0,04 / 0,06 | 8,8 | 1,07 / 2,19 | 0,09 / 0,14 |
| 100 000, alle | 2 | 6,35 / 9,77 / 10,07 | 0,08 / 0,05 / 0,08 | 122 | 24,3 / 39,9 / 50,2 | 0,18 / 0,12 / 0,19 |
| 100 000, rund 20 % | 2 | 3,51 / 3,16 / 3,07 | 0,35 / 0,35 / 0,36 | 9,0 | 9,8 / 9,4 / 8,6 | 0,43 / 0,42 / 0,47 |
| 500 000, rund 20 % | 2 | 9,6 / 15,6 / 17,5 | 1,5 / 2,7 / 1,6 | 9,6 | 26,1 / 47,3 / 59,6 | 1,6 / 2,8 / 1,8 |
| 500 000, alle | 1 | 102,7 / 111,0 | 0,07 / 0,11 | 1 187 | 631 / 615 | 0,17 / 0,22 |

Zähler eines Frames (Metal, Profiler und `HE_FOLIAGE_STATS=1`), vorher gegen nachher:

| Fall | `objects` | `visible` | `draws` | `triangles` | RSS max (Median, MB) |
|---|---|---|---|---|---|
| 100 000, alle | 100 004 gegen **173** | 27 944 gegen 59 | 3 gegen 3 | 352 712 gegen 448 316 | 461 gegen 396 (Pass 2) |
| 100 000, rund 20 % | 19 719 gegen **47** | 4 757 gegen 18 | 3 gegen 3 | 74 468 gegen 107 768 | 355 gegen 358 |
| 500 000, rund 20 % | 98 267 gegen **47** | 23 558 gegen 18 | 3 gegen 3 | 300 080 gegen 466 760 | 543 gegen 489 |
| 500 000, alle | 500 004 gegen **173** | 139 794 gegen 59 | **139 794 gegen 5** | 1 694 912 gegen 2 167 076 | 1 316 gegen 603 |

Cluster und Randtests aus `HE_FOLIAGE_STATS=1`: bei 100k mit 100 m sind es 43 Cluster für 19 715 von 100 000 Instanzen, davon 24 Randbuckets mit 15 529 einzeln
getesteten Instanzen; bei 500k mit 100 m 43 Cluster für 98 263 von 500 000, 24 Randbuckets, 76 781 Einzeltests; alle in Reichweite: 169 Cluster, kein Randbucket.

Der Pfad je Instanz im **neuen** Binary (`HE_FOLIAGE_CLUSTERS=0`, Pass 2): 100k alle `Render` 272 ms, 100k mit 100 m 57,6 ms. Das liegt im Bereich des alten Binarys
(174 bis 363 ms und 56 bis 63 ms): der Rückweg kostet nicht mehr als vorher.

## 3. Was die Zahlen sagen

1. **Die Kosten, die mit der Pflanzenzahl wuchsen, sind weg.** Bei 100k alle in Reichweite fällt `ExtractFoliage` von 6 bis 10 ms auf unter 0,1 ms, der ganze Extract von 24 bis 50 ms
   auf unter 0,2 ms, und es kommt dort dasselbe heraus, ob 100k oder 500k Pflanzen stehen (0,05 bis 0,08 ms gegen 0,07 bis 0,11 ms: dieselben 169 Buckets).
   Schatten-Encode (`Metal::EncodeShadowMap`) fällt bei 100k alle von 116 bis 223 ms auf 10 bis 13 ms, weil jede Kaskade Cluster statt Pflanzen culled und sortiert.
2. **Der Randring kostet weiter Pflanzen, aber nur seine.** Bei begrenzter Sichtweite wird jeder Bucket, den die Kreislinie schneidet, Pflanze für Pflanze getestet, damit die sichtbare
   Menge **exakt** die des alten Pfads bleibt (11.2 Punkt 7 im Plan). Das sind bei 100k 15 529 Tests (0,35 ms, rund 22 ns je Test unter dieser Last) und bei 500k 76 781 (1,5 bis 2,7 ms): der Aufwand folgt
   der Dichte im Ring, nicht der Zahl der Pflanzen im Rest des Geländes. Das Ziel aus Plan 9 ("100k platziert, 20 % sichtbar, Foliage-Extract unter 0,5 ms") ist mit **0,35 ms** (Pass 2) und 0,18 bis 0,32 ms (Pass 1) erreicht,
   bei 500k nicht (1,5 bis 2,7 ms). Wer den Randring billiger will, hat zwei Hebel: kleinere Buckets (16 m verkleinert den Ring etwa um die Hälfte) oder eine bucket-genaue Reichweite statt der exakten (ändert das Bild am Rand um bis zu einen Bucket).
3. **`draws` und die Instanz-Cap.** 500k alle in Reichweite: vorher 139 794 Draws (jedes Backend fällt über 65 536 Instanzen eines Batches still auf einen Draw je Instanz zurück), jetzt 5, die Läufe sind in Stücke von
   höchstens 65 536 geschnitten. `Render` fällt dort von rund 3,2 s auf 0,09 bis 0,12 s, RSS von 1,3 GB auf 0,6 GB.
4. **`triangles` steigt (+27 % bei 100k alle, +45 % bei 100k mit 20 %, +56 % bei 500k mit 20 %), die GPU-Zeit nicht:** GPU p50 ist bei 100k und 500k niedriger (14,5 gegen 9,4 ms, 6,5 gegen 5,2 ms, 16,7 gegen 10,5 ms in Pass 2), bei 10k
   im Rauschen (2,4 / 4,9 gegen 2,9 / 6,8 ms). Ein sichtbarer Bucket wird ganz eingereicht, auch seine Pflanzen außerhalb des Bildes; die Grafikkarte verwirft sie beim Clippen. Kleinere Buckets würden das senken und die Clusterzahl erhöhen.
5. **RSS** fällt bei den großen Fällen (500k alle: 1,3 GB auf 0,6 GB), weil die 280-Byte-Objekte je Pflanze (in mehreren Kopien der `RenderWorld`) entfallen; bei 100k mit 20 % bleibt er gleich. Der Store (64 Byte je Instanz zusätzlich zu
   `cachedInstances`) ist darin enthalten. Die genaue Aufteilung ist, wie in der Basismessung, nicht gemessen.

## 4. Grenzen

- Nur Metal. GL, D3D11, D3D12 und Vulkan sind nicht gemessen; die Bilder auf GL stehen in Plan 11.3.
- Die Bedingungen oben (Fremdlast, wechselnder Bildschirmzustand, Akku) machen jede absolute Zahl unsicher. Wer die Zahlen an einem ruhigen, gesperrten Mac im Stromsparmodus wiederholt, sollte die Basismessung
  mitlaufen lassen und die Verhältnisse vergleichen.
- Nicht gemessen: 1 Mio. Instanzen, Graph-Materialien (Wind, Alpha), Kontrolllauf ohne Schatten, die Kosten eines Frames mit Kameraflug (Buckets wechseln).
- Das Bild-A/B steht in `scripts/perf/foliage_pixel_ab.py` (Pfad je Instanz und Prüfmodus müssen bitgleich zum Vorher-Bild sein).
- Rohdaten (`*.summary.json`, `*.conditions.txt`) dieser Läufe liegen nicht im Repository; die Aufrufe stehen in Abschnitt 1, die Tabellen kommen aus
  `python3 scripts/perf/foliage_ladder_table.py <label>.profile.json`.
