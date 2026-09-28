# B2 Lastmessung: parallel_for mit Mindest-Körnung, vorher/nachher (Thema 102, Schritt 2)

Stand 2026-09-28. Misst den Umbau aus Commit a848b253 (`parallel_for` mit `minGrain`, Aufrufer holt sich Blöcke
selbst statt in `fut.get()` zu schlafen) gegen den Stand davor (898d4988), unter künstlicher Rechnerlast wie in
Audit Schritt 3 (`step3-cpu-memory-deep-dive-2026-09-27.md`, Lauf N1).

## Ergebnis

**Unter Build-Last (die Bedingung aus dem Audit) fällt die Zeit, die der Hauptthread in extract+cull verbringt, von
p50 5 bis 10 ms auf 0,07 bis 0,08 ms, p90 von 23 bis 35 ms auf 0,1 bis 0,56 ms.** Das ist der Posten, den das Audit
mit 8,72 ms p50 (N1) gemessen hat. Die CPU-Zeit pro Frame ohne `NextDrawable` sinkt im Median der drei Paare von 21,4
auf 6,3 ms. Pro Frame gehen keine Jobs mehr in den Pool (27 → 0): Die Landscape-Szene hat 4 Objekte, das liegt unter
`2 × minGrain`, also laufen extract und cull inline.

Unter gleichmäßiger Volllast (`yes`) tritt der große Stall gar nicht auf. Dort ist der Unterschied klein (0,39 → 0,06
ms p50). Der Stall braucht wechselnde Last, bei der Worker parken und wieder geweckt werden müssen, wie sie echte
Compiler erzeugen.

## Serie 2: Compile-Last (10 Schleifen, je eine Engine-TU mit -O3), 3 ABAB-Runden

| Lauf | Load 1 min | Jobs/Frame | extract+cull Hauptthread p50/p90 ms | CPU ohne ND p50/p90 ms | Frames > 5 ms |
|---|---|---|---|---|---|
| L1-before-load | 18.57 | 27 | **4.98** / 23.00 | 14.34 / 47.88 | 515/600 |
| L1-after-load | 21.03 | 0 | **0.073** / 0.560 | 7.67 / 26.18 | 422/600 |
| L2-after-load | 21.62 | 0 | **0.069** / 0.097 | 2.93 / 8.88 | 88/600 |
| L2-before-load | 22.01 | 27 | **10.49** / 35.28 | 33.66 / 129.64 | 559/600 |
| L3-before-load | 21.40 | 27 | **6.05** / 32.81 | 21.36 / 86.21 | 509/600 |
| L3-after-load | 19.91 | 0 | **0.082** / 0.532 | 6.30 / 36.92 | 326/600 |
| Q1-before (Last abgeklungen) | 15.23 | 27 | 0.41 / 21.67 | 3.44 / 26.49 | 181/600 |
| Q1-after (Last abgeklungen) | 12.14 | 0 | 0.051 / 0.094 | 2.44 / 2.89 | 5/600 |

Pro Frame (p50, L2-Paar) fallen auch die Scopes, die extract/cull verschachtelt aufrufen: `Metal::EncodeShadowMap`
5,82 → 0,20 ms (extrahiert für die Kaskaden neu, `MetalRenderer.mm:7064`), `OnRender` 11,02 → 1,43 ms,
`RenderExtractor::extract` 6,75 → nicht mehr unter den Top-Scopes.

## Serie 1: `yes`-Last (10 Busy-Loops), 3 ABAB-Runden

| Lauf | Load 1 min | Jobs/Frame | extract+cull Hauptthread p50/p90 ms | CPU ohne ND p50/p90 ms | Frames > 5 ms |
|---|---|---|---|---|---|
| L1-before-load | 17.03 | 27 | 0.390 / 0.945 | 3.35 / 7.33 | 87/600 |
| L1-after-load | 22.35 | 0 | 0.058 / 0.095 | 2.92 / 6.78 | 89/600 |
| L2-after-load | 17.38 | 0 | 0.066 / 0.128 | 3.79 / 20.75 | 242/600 |
| L2-before-load | 22.38 | 27 | 0.390 / 0.594 | 3.34 / 5.83 | 79/600 |
| L3-before-load | 21.82 | 27 | 0.379 / 0.635 | 3.29 / 5.43 | 74/600 |
| L3-after-load | 18.49 | 0 | 0.053 / 0.083 | 2.85 / 4.92 | 54/600 |
| Q1-before (ohne yes, fremder Build lief) | 7.80 | 27 | **5.67** / 31.25 | 14.04 / 41.82 | 402/600 |
| Q1-after (ohne yes, fremder Build lief) | 8.47 | 0 | **0.081** / 0.110 | 5.22 / 7.88 | 343/600 |

Das Q1-Paar dieser Serie war der erste Hinweis: Nach dem Abschalten der `yes`-Last lief nur noch ein Release-Build
einer anderen Instanz, und genau dort kam der Stall des Vorher-Builds (5,67 ms p50) zurück.

## Was nicht am Job-System liegt

Die Spalte „Frames > 5 ms“ bleibt auch nachher unter Last hoch (88 bis 422 von 600) und schwankt stark zwischen
Läufen mit gleichem Build. Das ist die übrige Verdrängung des Hauptthreads (Render-/Encode-Arbeit, die auf einen
Kern warten muss) und nicht Gegenstand von B2. Die extract+cull-Spalte ist der direkte Job-System-Anteil.

## Bedingungen

| | |
|---|---|
| Rechner | M5, 10 Kerne (4 P + 6 E). **Stromsparmodus an** (`lowpowermode 1`, ohne sudo nicht abschaltbar), Bildschirm gesperrt (`CGSSessionScreenIsLocked=Yes`, in jeder Zeile von `series.log`). Absolutwerte sind deshalb nicht 1:1 mit dem Audit vergleichbar, das A/B schon |
| Fremdlast | Andere Hive-Instanzen bauten und testeten parallel (Load ohne eigene Last ~8 bis 13), dazu ein fremder `HorizonEditor`. ABAB mit wechselnder Reihenfolge pro Runde gleicht Drift aus |
| Builds | Release, `HE_PROFILING=ON`, ein Build-Baum (`out/build/b2-release`). Nachher = a848b253. Vorher = dieselben fünf Quelldateien auf 898d4988 zurückgesetzt (JobSystem.h/.cpp, OcclusionCuller.cpp, MetalRenderer.mm, OpenGLRenderer.cpp), neu gebaut, danach wiederhergestellt. `libHorizonCore`/`libHorizonRendering` unterscheiden sich, der Rest ist bitgleich |
| Harness | `scripts/he_perf_capture.py`, 300 Frames Warmup, 600 Aufnahme, vsync aus, Landscape-Szene und Kamera wie Audit Schritt 3, Projekt-Kopie `/tmp/b2proj` |
| Beleg pro Lauf | `series.log` enthält pro Lauf den per `vmmap` gelesenen Pfad der geladenen `libHorizonCore.dylib` (Serie 2; in Serie 1 per Hand geprüft, die lsof-Zeile war leer) |

**Falle, die das A/B sonst wertlos gemacht hätte:** Lokale Builds verlinken `HorizonEditor` und die
`libHorizon*.dylib` mit absoluten `LC_RPATH`s in den Build-Baum, ohne `@executable_path`. Eine `cp -R`-Kopie des
Deploy-Ordners lädt deshalb die dylibs, die gerade im Build-Baum liegen, und vorher und nachher messen denselben Code.
`scripts/perf/selfcontain_deploy_copy.sh` ersetzt die rpaths der Kopie durch `@loader_path` und signiert neu,
`b2_ab_load.sh` ruft es selbst auf.

## Nachmessen

```sh
# zwei Release-Deploys kopieren (vorher/nachher), dann:
B2_LOAD=compile B2_BUILD=out/build/b2-release \
  scripts/perf/b2_ab_load.sh /tmp/before/HorizonEditor /tmp/after/HorizonEditor /tmp/ab 3
python3 scripts/perf/b2_ab_table.py /tmp/ab
```

Rohdaten (Dumps gzip, Summaries, `series.log`) außerhalb von git unter
`/Users/connorjansen/VSCode/HorizonEngine/out/perf-audit/2026-09-28-b2/{yes,compile}/`.

## Tests

Debug-Vollbau grün. Volle `ctest -j4`-Suite: 215 von 216 laufenden Tests grün, 3 übersprungen (`runtime_size*`).
`test_mcp_tools_material` schlug einmal fehl. Wahrscheinliche Ursache: Der ctest einer anderen Instanz lief denselben
Test um 09:38 bis 09:39 mit demselben TMPDIR (`/var/folders/…/he_tests/test_mcp_tools_material`, die Pfade hängen nur
am Testnamen, nicht am Worktree). Das passt zeitlich, der eigene Zeitstempel ist aber nicht mehr belegt. Einzeln
3 von 3 grün, HE_Net/Material-Pfad nutzen `parallel_for` nicht.

Nicht im Editor gemessen: der Pfad „Aufrufer holt sich Blöcke“. Die Landscape-Szene liegt mit 4 Objekten unter
`2 × minGrain`, nachher läuft alles inline. Der gemessene Gewinn ist also die Mindest-Körnung. Das Aufteilen mit
mitarbeitendem Aufrufer ist nur durch `tests/test_jobsystem.cpp` abgedeckt, eine Szene mit mehr als 512 Objekten
wäre der Lasttest dafür. `test_profiler` und
`test_jobsystem` 5 Wiederholungen grün.
