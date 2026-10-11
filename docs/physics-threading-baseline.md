# Physik-Multithreading: Bestandsaufnahme und Messbasis (Thema 178, Schritt 1)

Stand 11.10.2026, Zweig `claude/physik-multithreading-jolt-jobsystem-thread-aufteilung-zeitbudget`
(Basis `a4fa3a9d` = `release/0.7.0`). In diesem Schritt wurde **kein Engine-Code** geändert. Neu sind nur
Messhilfen unter `scripts/perf/` (Abschnitt 8) und dieses Dokument. Die Zahlen sind die Vorher-Werte, gegen die
Schritt 4 misst.

## 1. Kurzfassung

1. **Die Physik-Zeit steckt zur Hälfte nicht in Jolt.** Im echten Spielloop kostet ein Schritt bei 20 000 aktiven
   Körpern 18,7 ms, `JPH::PhysicsSystem::Update` allein (gleiche Parameter, ohne Engine) 8,5 ms. Eine Stichprobe des
   Hauptthreads (`sample`) schlüsselt `PhysicsWorld::step` auf: **Update 43 %**, `applyRegionHold` 19 %, Positions-/
   Rotations-/Bewegungstyp-Abfragen je Körper 23 %, `writeBackWorldPose` 7 %, Rest 7 %. Alles außer Update läuft
   seriell auf dem Hauptthread.
2. **Die Jolt-Worker sind im echten Loop zu einem Drittel ausgelastet.** Im Bench ohne Engine laufen die vier Worker
   zu etwa 80 %, im Spiel bei 20 000 Körpern 28 %, der Hauptthread 93 %. Der serielle Anteil bestimmt die Zeit.
3. **Der `TempAllocatorImpl` überläuft nicht, hat aber keinen Spielraum, und ein Überlauf beendet den Prozess**
   (`std::abort()`, Jolt 5.5 `TempAllocator.h`, kein Heap-Fallback). Der Spitzenverbrauch ist fast szenenunabhängig
   (8,6 bis 9,6 MB von 10 MB bei 1 000 bis 50 000 Körpern), weil er an `kMaxContactConstraints` hängt:
   **0,84 KB je zulässigem Kontakt-Constraint**. Wer die Kontakt-Obergrenze hebt, muss den Allocator mitheben.
4. **Die Obergrenzen schneiden bei dichten Haufen still ab.** Ab etwa 10 000 Körpern im Kontakt meldet
   `PhysicsSystem::Update` in **jedem Schritt** `ManifoldCacheFull | BodyPairCacheFull | ContactConstraintsFull`.
   `PhysicsWorld::step` wirft den Rückgabewert weg (`PhysicsWorld.cpp:2813`). Mit vierfachen Obergrenzen ist derselbe
   Haufen **doppelt so schnell** (20,4 statt 41,3 ms) und fehlerfrei, braucht aber 34 MB Temp.
5. **Die Workerzahl ist nicht der Hebel.** Im Bench bringen mehr als vier Worker auf 10 Kernen kaum etwas
   (20 000 Körper: 8,65 ms bei 4, 7,9 ms bei 9). Der Hebel ist die Konkurrenz: laufen zehn Threads des Engine-Pools
   gleichzeitig voll, steigt dieselbe Update-Zeit von 8,7 auf 24,5 ms (Spitze 38,6 ms).
6. **Charaktere und Raycasts sind billig, solange die Welt einfach ist:** 4,7 µs je `ExtendedUpdate` (1 000
   Charaktere 4,7 ms seriell), 1,2 µs je `CastRay` gegen 20 000 Körper. Mit Mesh-Collidern und Treppen wird das mehr;
   das ist hier nicht gemessen.

## 2. Bedingungen (ehrlich)

| | |
|---|---|
| Gerät | MacBook Air von Connor, 10 Kerne, 24 GB, Release-Build |
| Strom | **Akku** (44 bis 45 %, "Battery Power"), Stromsparmodus aus (`pmset lowpowermode 0`) |
| Bildschirm | **gesperrt** (`CGSSessionScreenIsLocked=Yes`). Das versteckte Fenster wird nicht komponiert, `Metal::NextDrawable` wartet (11 bis 24 ms je Frame). Das betrifft Frame-Zeit und FPS, nicht die CPU-Zeit der Physik; FPS-Zahlen stehen deshalb nirgends als Ergebnis |
| Fremdlast | vorher `pgrep` auf Editor/Game: keiner; GPU "Device Utilization" 3 %; Load Average vor den Läufen 1,2 bis 3,1 (Queen und andere Hive-Dienste laufen, kein anderer Arbeiter baute). Während der Sweeps steigt er durch den Bench selbst auf 3,5 bis 8,5 |
| Wiederholung | Bench: jede Zeile ein frischer Prozess. Rauschboden: 20 000 springende Körper, 4 Worker, fünf unabhängige Läufe 8,50 / 8,61 / 8,65 / 8,67 / 8,72 ms (p50), Spanne 2,6 %. **Echter Loop: ein Lauf je Zeile**, kein Rauschboden gemessen |
| Game-Binary | `claude-render-extractor-und-schatten-pas/out/deploy/Game`, gebaut 11.10. 03:31 aus `cc04acf9`; `git diff cc04acf9 a4fa3a9d -- src` ist leer, der Quelltext gleicht diesem Zweig. Kopie nach `/tmp/he178/gamebase` mit `@loader_path`-Rpaths (sonst lädt sie die Dylibs des Build-Baums) |
| Jolt im Bench | v5.5.0, `libJolt.a` aus `out/build/macos-release` des Hauptcheckouts, Defines wie im Engine-Release (`JPH_DEBUG_RENDERER JPH_OBJECT_STREAM JPH_PROFILE_ENABLED NDEBUG`) |
| Profiler | `HE_PROFILE_CAPTURE=600 HE_PROFILE_WARMUP=300 HE_PROFILE_COUNTERS=0 HE_HIDDEN_WINDOW=1` (das `--no-counters` von `he_perf_capture.py`), vsync aus |

Was der Bench **nicht** nachbaut: die Entity-Rückschreibung und die Welt/Lokal-Umrechnung von `PhysicsWorld::step`
(die stehen im Echt-Loop), die Objekt-Layer-Matrix von HE (hier zwei Layer), den Engine-Pool daneben. Der Bench ist
Jolt mit den Parametern der Engine, kein Ersatz für den Spielloop.

## 3. Der Aufbau heute (gelesen in `PhysicsWorld.cpp`)

| Teil | Wert | Stelle |
|---|---|---|
| Jolt-Worker | `clamp(hardware_concurrency / 2 - 1, 0, 4)`, also **4 auf 10 Kernen**; der aufrufende Thread hilft mit. Nicht konfigurierbar | `Impl::Impl`, Zeile 632 |
| Job-System | `JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, n)`, keine Priorität, kein QoS | Zeile 633 |
| `TempAllocatorImpl` | fest **10 MB**, ein Allocator für `Update` und alle `ExtendedUpdate` | Zeile 410 |
| Grenzen | `kMaxBodies` 65 536, `kMaxBodyPairs` 65 536, `kMaxContactConstraints` 10 240 | Zeilen 621 bis 623 |
| Schritt | `Update(dt, 1, ...)`: ein Collision-Step, `dt` fest 1/60 s | Zeile 2813 |
| Rückgabewert von `Update` | **ignoriert** (`EPhysicsUpdateError`) | Zeile 2813 |
| Danach | `breakOverloadedJoints`, Rückschreiben aller Körper (Map-Lookup, `GetMotionType`, `GetPosition`, `GetRotation`, `writeBackWorldPose`), dann jeder Charakter: `ExtendedUpdate` seriell | Zeilen 2819 bis 3049 |
| Vorher | `applyRegionHold()`: im Spiel ist `holdTest` gesetzt, der Aufruf geht bei **jedem** Schritt über alle dynamischen Körper (Map-Lookups, gesperrtes `GetPosition`, Callback) | Zeilen 2811, 3082 |
| Kontakt-Listener | ein `std::mutex` in `OnContactValidate`, `OnContactAdded`, `OnContactRemoved`, dazu eine `unordered_map` der aktiven Kontakte | Zeilen 192 bis 400 |
| Fester Schritt im Spiel | `HE::advanceFixedSteps`: höchstens **5** Schritte je Frame, der Rest der Zeit wird verworfen | `FixedStep.h`, `GameApplication.cpp:3178` |
| Engine-Pool | `globalPool()`: `hardware_concurrency` Threads (10), keine Priorität. Streaming (`CellStreamer`, `ContentManager`) und Render-Extract/Cull laufen darauf | `JobSystem.cpp:731` |
| Jolt-Profiler | `JPH_PROFILE_ENABLED` ist im Release-Build des Hauptcheckouts an, aber an keinen Haken angeschlossen; Jolt-Threads heißen auf macOS nicht (`SetThreadName` nur Windows/Linux) | CMake, Jolt |

`PhysicsStep` im Engine-Profiler (`GameApplication.cpp:3178`) umfasst **alle** festen Schritte eines Frames und
`CollisionDispatch` (verschachtelt). Die Dump-Datei enthält die Zahl der Schritte nicht; `physics_load_run.py` rechnet
sie aus `deltaMs` nach (`advanceFixedSteps` nachgespielt) und normiert damit auf einen Schritt. Das ist eine
Rückrechnung; die Zeilen "Frames mit genau einem Schritt" sind davon unabhängig.

## 4. Jolt-Bench: Update-Zeit (4 Worker = Engine-Standard, HE-Listener)

Szenen: **bounce** = Kugeln und Würfel auf einem 3-m-Raster, aus 4 bis 24 m fallen gelassen, Restitution 0,9, nie
schlafend (viele aktive Körper, kaum Kontakte); **pile** = Türme aus zehn Würfeln im Abstand 1,05 m, Schlafen aus
(dichter Kontakt). Zeit je Schritt in ms, p50 / p95 / max über 240 Schritte nach 90 Warm-up-Schritten.

| Szene | Körper | Update ms | Kerne im Mittel | Temp-Spitze MB | Jolt-Fehler |
|---|---:|---|---:|---:|---|
| bounce | 1 000 | 0,44 / 0,49 / 0,51 | 4,0 | 8,63 | keine |
| bounce | 5 000 | 1,95 / 2,26 / 3,15 | 4,2 | 8,71 | keine |
| bounce | 20 000 | 8,50 / 9,86 / 13,22 | 4,1 | 9,01 | keine |
| bounce | 50 000 | 24,81 / 29,53 / 47,99 | 4,0 | 9,61 | keine |
| pile | 1 000 | 1,88 / 2,12 / 4,38 | 4,6 | 8,63 | keine |
| pile | 2 000 | 3,78 / 4,30 / 5,38 | 4,6 | 8,65 | keine |
| pile | 5 000 | 9,65 / 11,18 / 12,29 | 4,7 | 8,72 | keine |
| pile | 10 000 | 41,28 / 48,57 / 61,75 | 4,6 | 8,94 | **240/240 Schritte**: ManifoldCacheFull, BodyPairCacheFull, ContactConstraintsFull |
| pile | 20 000 | 51,19 / 57,37 / 65,66 | 4,6 | 9,17 | **240/240 Schritte**: dieselben drei |
| pile, Schlafen erlaubt | 5 000 | 10,85 / 12,44 / 14,60 | 4,7 | 8,75 | keine (nach 19 s noch 4 480 von 5 000 wach) |

Ein Haufen schläft in der Engine also nicht von allein ein; "gestapelte Kisten" sind kein Leerlauf-Fall.

**Obergrenzen gehoben (pile, 10 000 Körper):** mit `kMaxBodyPairs` 262 144 und `kMaxContactConstraints` 40 960
(vierfach) meldet Jolt keinen Fehler mehr und rechnet **20,4 / 23,2 / 24,8 ms statt 41,3 / 48,6 / 61,8 ms**. Die
abgeschnittene Szene kostet also doppelt so viel, weil Jolt sie jeden Schritt erneut versucht. Der Preis steht in
Abschnitt 6.

## 5. Jolt-Bench: Worker, Listener, Konkurrenz

**Worker-Sweep** (Jolt-Worker neben dem aufrufenden Thread; Engine-Standard 4):

| Szene | Körper | 0 | 1 | 2 | 3 | 4 | 6 | 9 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bounce, p50 ms | 20 000 | 27,21 | 15,78 | 11,87 | 9,81 | **8,65** | 7,98 | 7,93 |
| bounce, max ms | 20 000 | 36,93 | 29,76 | 15,31 | 13,00 | 10,59 | 12,35 | 10,63 |
| pile, p50 ms | 5 000 | 41,97 | 11,86 | 8,34 | 9,82 | **9,18** | 8,68 | 8,40 |
| pile, max ms | 5 000 | 51,56 | 29,75 | 19,70 | 13,50 | 13,96 | 14,45 | 12,15 |

Die Skalierung flacht ab drei bis vier Workern ab; im Pile-Fall ist das Optimum bei zwei bis drei Workern nicht
von vier zu trennen. Der Sprung von 0 auf 1 Worker im Pile-Fall (42 auf 12 ms) ist größer als die Kernzahl erklärt;
das ist hier nicht untersucht, es steht als Messwert da. Der Standard von vier Workern liegt nahe am Knie.

**Kontakt-Listener** (HE-Listener mit Mutex gegen keinen Listener, 4 Worker): bounce 20 000 8,72 gegen 8,43 ms
(+3 %), pile 5 000 9,51 gegen 9,49 ms (gleich). Der Mutex ist bei vier Workern nicht der Engpass; bounce 20 000 setzt
dabei 39 706 `OnContactAdded` und 39 379 `OnContactRemoved` in 240 Schritten ab.

**Konkurrenz um die Kerne** (bounce 20 000, K Threads drehen im Leerlauf voll, ersetzt einen Engine-Pool, dessen
Jobs gleichzeitig laufen; auf 10 Kernen sind 4 Worker + Hauptthread + K Threads aktiv):

| Jolt-Worker | K = 0 | K = 2 | K = 5 | K = 10 |
|---|---|---|---|---|
| 4 | 8,67 / 9,98 / 11,30 | 9,48 / 11,64 / 15,47 | 13,59 / 17,07 / 26,57 | **24,47 / 30,28 / 38,56** |
| 2 | 11,84 / 13,48 / 14,40 | 12,02 / 14,26 / 32,50 | 17,22 / 23,59 / 53,42 | 28,77 / 35,71 / 43,34 |

(Update ms, p50 / p95 / max.) Bei K = 10, der Größe von `globalPool()`, ist der Schritt fast dreimal so lang und die
Spitze 38,6 ms. Das ist der Worst Case (alle Pool-Threads gleichzeitig voll). Im echten Loop waren die Pool-Threads in
keinem Lauf mehr als 1 % ausgelastet, weil nichts gestreamt wurde (siehe Abschnitt 7); wie oft Streaming und Physik
im selben Frame zusammentreffen, ist **nicht gemessen**, das braucht eine Szene mit Zellstreaming und Physik
(Thema 164).

**Charaktere** (`CharacterVirtual::ExtendedUpdate`, seriell nach `Update`, 5 000 springende Körper in der Welt, die
Charaktere laufen im Kreis auf ebenem Boden):

| Charaktere | ExtendedUpdate ms p50 / p95 / max | µs je Charakter |
|---:|---|---:|
| 10 | 0,05 / 0,07 / 0,10 | 4,8 |
| 100 | 0,47 / 0,54 / 0,63 | 4,7 |
| 500 | 2,35 / 2,58 / 3,25 | 4,7 |
| 1 000 | 4,72 / 5,18 / 7,85 | 4,7 |

**Raycasts** (`NarrowPhaseQuery::CastRay`, nächster Treffer, vom aufrufenden Thread, 20 000 Körper in der Welt):
100 Strahlen 0,13 ms, 1 000 Strahlen 1,22 ms, 5 000 Strahlen 6,03 ms; **1,2 µs je Strahl**, linear. Szenenabfragen
mit Formen (`CastShape`, `CollideShape`) und Mesh-Collider sind nicht gemessen.

## 6. Der TempAllocator

Der Allocator ist ein fester Block. In Jolt 5.5:

```cpp
if (new_top > mSize) { Trace("TempAllocator: Out of memory trying to allocate %u bytes", inSize); std::abort(); }
```

Ein Überlauf ist also kein Leistungsproblem, sondern ein Absturz. Gemessen mit einem mitzählenden Wrapper (512 MB,
Spitze = größter gleichzeitiger Verbrauch, `Update` und `ExtendedUpdate` zusammen):

- Die Spitze ist **fast unabhängig von der Szene**: 8,63 MB bei 1 000 Körpern, 9,01 MB bei 20 000, 9,61 MB bei
  50 000, 9,17 MB beim gesättigten Haufen. Der Block ist der feste Anteil.
- Der feste Anteil hängt an `kMaxContactConstraints`, nicht an `kMaxBodyPairs`:

| `kMaxBodyPairs` | `kMaxContactConstraints` | Temp-Spitze |
|---:|---:|---:|
| 65 536 | 10 240 (Engine) | 8,63 MB |
| 131 072 | 10 240 | 8,63 MB |
| 65 536 | 20 480 | 17,11 MB |
| 131 072 | 40 960 | 34,06 MB |
| 262 144 | 81 920 | 67,96 MB |

  Das sind **rund 0,84 KB je Kontakt-Constraint**. Die 10 MB reichen für etwa 12 000 Constraints; **jede Hebung der
  Obergrenze über etwa 11 900 ohne größeren Allocator beendet den Prozess.** Der Haufen mit vierfachen Obergrenzen
  (Abschnitt 4) bräuchte 34 MB.
- In den gemessenen Szenen bleibt der Verbrauch unter 10 MB, der Spielraum beträgt aber nur 0,4 bis 1,4 MB. Charaktere
  verbrauchen ihr Temp nach `Update` aus demselben Block und erhöhen die Spitze nicht (5 000 Körper mit 1 000
  Charakteren: 8,71 MB, ohne Charaktere ebenfalls 8,71 MB).

## 7. Echter Spielloop (`HorizonGame`, versteckt, Profiler-Capture)

Szenen aus `scripts/perf/physics_load_scene.py`: **sparse** wie bounce oben, **dense** ein Gitter im Abstand 1,2 m,
fünf Lagen, gemeinsam fallen gelassen (Restitution 0,9, bleibt in Bewegung). Charaktere stehen (kein Skript, das sie
laufen lässt), haben aber Controller, kinematischen Körper und Kapsel. 600 Frames nach 300 Warm-up-Frames, ein Lauf.

**Zeit je Schritt** (`PhysicsStep` geteilt durch die nachgerechnete Schrittzahl, ms) und Auslastung:

| Szene | Körper / Charaktere | Schritt p50 / p95 / max | Jolt-Bench Update p50 | Rest (Engine) | Hauptthread | je Jolt-Worker |
|---|---|---|---:|---:|---:|---:|
| sparse | 1 000 / 0 | 0,94 / 1,22 / 2,12 | 0,44 | 0,50 | 13 % | 2 % |
| sparse | 5 000 / 0 | 4,46 / 5,40 / 9,37 | 1,95 | 2,51 | 49 % | 9 % |
| sparse | 20 000 / 0 | **18,74 / 21,04 / 24,45** | 8,50 | 10,24 | **93 %** | **28 %** |
| dense | 5 000 / 0 | 4,94 / 6,11 / 12,29 | - | - | 49 % | 11 % |
| dense | 10 000 / 0 | 12,01 / 13,71 / 15,58 | - | - | 94 % | 34 % |
| sparse | 5 000 / 100 | 4,75 / 5,78 / 7,19 | 1,95 | +0,3 (Charaktere) | 50 % | 9 % |
| sparse | 5 000 / 500 | 5,73 / 7,60 / 13,07 | 1,95 | +1,2 (Charaktere) | 59 % | 10 % |

(Die Spalte "Jolt-Bench" gibt es nur für die sparse-Zeilen, deren Szene dem Bench-`bounce` entspricht; das dichte
Gitter im Spiel ist nicht das `pile` des Benchs. Ob es die Obergrenzen füllt, ist nicht zu sehen, weil die Engine die
Fehlerflags verwirft. Die Charaktere kosten 4,7 µs im Bench, im Spiel etwa 2,5 µs je stehendem Charakter.)

**Pro Frame** (was der Frame kostet): bei 20 000 Körpern 118 ms CPU je Frame (p50), davon `PhysicsStep` 94 ms (p50, max
122 ms). Die Frames laufen **jedes Mal an die 5-Schritte-Grenze** (600 von 600 Frames mit fünf Schritten), die Physik
holt die Echtzeit nie ein und läuft mit etwa 0,7-facher Geschwindigkeit (83 ms simuliert je 118 ms Wanduhr). Bei
dense 10 000 sind es 2,1 Schritte je Frame (Frame 35 ms, Physik 24 ms). Bei 1 000 bis 5 000 Körpern trägt der
gesperrte Bildschirm den Frame (`NextDrawable`), die Physik ist 1,0 bis 4,6 ms je Frame (p50) mit Spitzen bis 37 ms
(zwei bis fünf Schritte in einem Frame).

**Wo der Schritt bei 20 000 Körpern bleibt** (`sample` auf den Hauptthread, 149 Proben in `PhysicsWorld::step`, direkte
Aufrufe):

| Aufruf | Proben | Anteil |
|---|---:|---:|
| `JPH::PhysicsSystem::Update` (inklusive Warten auf die Worker) | 64 | 43 % |
| `PhysicsWorld::applyRegionHold` | 29 | 19 % |
| `BodyInterface::GetPosition` (Rückschreiben) | 18 | 12 % |
| `writeBackWorldPose` | 11 | 7 % |
| `BodyInterface::GetRotation` | 7 | 5 % |
| `BodyInterface::GetMotionType` | 7 | 5 % |
| `BodyLockInterfaceLocking::LockRead` | 3 | 2 % |
| eigener Code (inline) | 10 | 7 % |

(`sample` hält den Prozess je Probe an, taugt also nur für Anteile, nicht für Millisekunden; 187 Proben in 10 s auf
dem Hauptthread, 149 davon in `step`.) `applyRegionHold` ist neu seit Thema 164 Schritt 3a: `GameApplication.cpp:2134` setzt die Prüf-Funktion
immer (sie antwortet "nichts halten", solange kein Streamer aktiv ist), die Funktion geht aber trotzdem bei jedem
Schritt über alle dynamischen Körper und macht je Körper drei Map-Zugriffe (`entityToCharacter.count`, `entityToBody.find`,
`heldBodies.count`), ein gesperrtes `GetPosition` und den Callback. Das ist der größte Einzelposten außerhalb von Jolt und für
Schritt 2 oder 3 der erste Kandidat.

**Engine-Pool im Loop:** die zehn `Worker-N`-Threads (aus `HorizonCore`) liegen in jedem Lauf bei etwa 1 % je Thread;
ohne Zellstreaming und ohne Asset-Last sind sie im Leerlauf. Die vier Jolt-Worker stammen aus `libHorizonScene` (ein
fünfter Thread dort ist untätig). Zusammen sind es 4 Jolt-Worker, 10 Pool-Threads und der Hauptthread auf 10 Kernen.

## 8. Messhilfen (neu, kein Engine-Code)

| Datei | Zweck |
|---|---|
| `scripts/perf/physics_load_bench.cpp`, `physics_load_bench.sh` | Jolt wie `PhysicsWorld::Impl` getrieben: gleiche Obergrenzen, Worker, 10-MB-Allocator (als mitzählender Wrapper), HE-Listener, `ExtendedUpdate`, `CastRay`; Optionen `--scene bounce|pile --bodies --characters --rays --workers --listener he|none --contend K --max-pairs --max-contacts --sleeping`. Druckt Update-Zeit, Temp-Spitze, Jolt-Fehlerflags und CPU je Thread (Mach `thread_info`) |
| `scripts/perf/physics_load_sweep.py` | die Matrix aus den Abschnitten 4 und 5 als Markdown-Tabellen (`--only bodies|workers|listener|contend|chars|rays`) |
| `scripts/perf/physics_load_scene.py` | baut ein loses HorizonGame-Verzeichnis mit der Lastszene (Körper, Charaktere, Boden, Kamera, `project.hcfg` v2) |
| `scripts/perf/physics_load_run.py` | fährt das Spiel mit `HE_PROFILE_CAPTURE`, liest `PhysicsStep`, rechnet die Schritte nach und wertet die Thread-Probe aus; `--reanalyze` liest einen vorhandenen Lauf neu |
| `scripts/perf/thread_cpu_probe.c` | `DYLD_INSERT_LIBRARIES`-Probe: CPU je Thread alle 100 ms, jeder Thread mit dem Image getaggt, das ihn erzeugt hat (Jolt-Worker = `HorizonScene`, Pool = `HorizonCore`), weil Jolt seine Threads auf macOS nicht benennt |

Wiederholen:

```
JOLT_BUILD=<build-baum mit _deps/joltphysics-build/libJolt.a> scripts/perf/physics_load_bench.sh build
python3 scripts/perf/physics_load_sweep.py                      # Bench-Tabellen
clang -O2 -dynamiclib scripts/perf/thread_cpu_probe.c -o /tmp/he178/thread_cpu_probe.dylib
cp -cR <worktree>/out/deploy/Game /tmp/he178/gamebase
scripts/perf/selfcontain_deploy_copy.sh /tmp/he178/gamebase    # sonst lädt HorizonGame die Dylibs des Build-Baums
python3 scripts/perf/physics_load_run.py --layout sparse --bodies 20000
```

`JOLT_BUILD` hat als Vorgabe den Pfad des Hauptcheckouts auf Connors Mac. Das "Vorher"-Binary dieser Messung liegt
unverändert in `~/HorizonEnginePerf/he178_before` (Kopie von `/tmp/he178/gamebase`, Rpaths schon gesetzt); Schritt 4
nimmt es als `--runtime`.

## 9. Was Schritt 2 bis 4 daraus mitnehmen

- **Schritt 2 (Threads konfigurierbar):** Vier Worker liegen nahe am Knie, mehr bringt im Bench nichts. Die Zahl
  gehört trotzdem in eine Einstellung (Projekt/Umgebung), und sie sollte gegen die Pool-Größe abgestimmt werden: die
  Konkurrenz kostet bis zum Dreifachen (Abschnitt 5), die Worker-Anzahl nur wenige Prozent. Jolt-Threads auf macOS
  benennen oder per QoS einordnen hilft beim Messen und eventuell beim Scheduling.
- **Allocator und Obergrenzen gehören zusammen.** Eine Hebung von `kMaxContactConstraints` ohne größeren
  `TempAllocatorImpl` (0,84 KB je Constraint) ist ein Absturz. Der Rückgabewert von `Update` sollte mindestens
  gezählt und gedrosselt geloggt werden; das ist heute die einzige Warnung vor stillem Abschneiden.
- **Schritt 3 (Zeitbudget):** der größte Posten außerhalb von Jolt ist der serielle Teil des Schritts (57 % bei
  20 000 Körpern): `applyRegionHold` und das Rückschreiben. Profiler-Scopes in `PhysicsWorld::step` (Update,
  Rückschreiben, Charaktere, Region-Hold) fehlen und würden die `sample`-Aufschlüsselung dieses Dokuments im Profiler
  sichtbar machen; sie wurden hier bewusst nicht eingebaut. Die 5-Schritte-Grenze lässt bei 20 000 Körpern die
  Simulation auf 0,7-fache Geschwindigkeit fallen, ohne dass es jemand sieht.
- **Schritt 4 (Verifikation):** der Echt-Loop braucht ein Game-Binary des geänderten Zweigs. Das aus Abschnitt 2 ist
  der "Vorher"-Stand (Quelltext gleich `a4fa3a9d`); bei Änderungen am Zweig `HE_PROFILE_COUNTERS=0`, gleiche Szenen
  (`--seed 1` ist fest), abwechselnd alt und neu, Bildschirm entsperrt, am Netz.
- **Offen (nicht gemessen):** Streaming plus Physik im selben Frame, Mesh-Collider, Treppen, `CastShape` und
  `CollideShape`, laufende Charaktere, Wiederholläufe des Echt-Loops, entsperrter Bildschirm mit vsync (dort gäbe es
  genau einen Schritt je Frame statt der nachgerechneten Mischung), Windows und Linux.

## 10. Ergebnis nach Schritt 2 und 3 (Schritt 4: Vorher-nachher, Voll-Build, Tests)

Stand 11.10.2026, Zweig auf `origin/release/0.7.0` (`af4f2868`, enthält Schritt 1 bis 3: Messbasis, Threads #131,
Zeitbudget #132). Dieser Abschnitt ändert keinen Produktcode; die Rohdaten stehen in
`docs/perf-audit/physics-threading-step4/`.

**Ziel des Themas ("eine Lastszene mit vielen Körpern braucht messbar weniger Physik-Zeit pro Frame"): erreicht.**
Gemessen im echten Spielloop, Zeit je Schritt (`PhysicsStep` geteilt durch die nachgerechnete Schrittzahl, ms), abwechselnd
Vorher (Schritt-1-Binary, Quelle `a4fa3a9d`) und Nachher (Kopf dieses Zweigs), gleiche Szenen (`--seed 1`),
`HE_PROFILE_COUNTERS=0`, 300 Warm-up- und 600 Mess-Frames:

| Szene | Vorher p50 / p95 / max | Nachher p50 / p95 / max | p50 | p95 |
|---|---|---|---:|---:|
| sparse 1 000 | 0,91 / 1,13 / 1,50 | 0,56 / 0,69 / 1,08 | -38 % | -39 % |
| sparse 5 000 | 4,26 / 4,86 / 6,27 | 2,47 / 2,99 / 5,25 | -42 % | -38 % |
| sparse 20 000 | 17,81 / 19,42 / 23,76 | 10,54 / 11,75 / 16,71 | -41 % | -40 % |
| dense 10 000 | 11,60 / 12,68 / 20,24 | 8,23 / 9,41 / 11,05 | -29 % | -26 % |
| sparse 5 000 + 500 Charaktere | 5,65 / 6,39 / 10,31 | 3,88 / 4,47 / 12,57 | -31 % | -30 % |

Der Median und das 95. Perzentil sinken um 26 bis 42 %. **Die Spitzen (max) sinken nicht verlässlich:** im ersten Lauf
liegen sie bei vier von fünf Zeilen tiefer (bei 20 000 um 30 %), bei 5 000 mit 500 Charakteren höher (12,57 statt 10,31 ms,
ein einzelner Schritt in 600 Frames). Im Wiederholungslauf (unten) hat sparse 20 000 nachher einen Ausreißer von 30,5 ms
(vorher 23,1 ms), das 95. Perzentil liegt dort mit 12,5 ms über dem des ersten Laufs (11,8 ms). Belegt ist die Senkung von
Median und 95. Perzentil; über die Spitzen (ein Schritt von 600 bis 3 000) sagt diese Messung nichts Sicheres.

**Wiederholung in umgekehrter Reihenfolge** (Nachher zuerst, dann Vorher; gleiche Szenen, frische Prozesse, Load 3,2 bis 4,2
durch den vorangegangenen `ctest`-Lauf; Rohdaten `*_r2.txt`). Schritt p50 / p95 / max in ms:

| Szene | Vorher Lauf 1 / Lauf 2 (p50) | Nachher Lauf 1 / Lauf 2 (p50) | Lauf 2 Vorher p95 / max | Lauf 2 Nachher p95 / max | p50 Lauf 2 |
|---|---|---|---|---|---:|
| sparse 20 000 | 17,81 / 17,88 | 10,54 / 10,41 | 19,34 / 23,10 | 12,54 / 30,48 | -42 % |
| sparse 5 000 | 4,26 / 4,32 | 2,47 / 2,45 | 5,05 / 6,05 | 2,90 / 3,75 | -43 % |

Der Median wiederholt sich auf 0,4 bis 1,4 % (Rauschboden für p50 in dieser Messanordnung, zwei Szenen); der Gewinn ist
rund 30-mal größer als diese Streuung. Auch im Wiederholungslauf bleiben die Jolt-Worker bei 20 000 Körpern bei 35 bis 36 % (vorher 29 %)
und der Hauptthread bei 90 % (vorher 94 %); 3,15 Schritte je Frame bei 54,2 ms Frame-Abstand (0,97 der Echtzeit, vorher 4,99
Schritte bei 112,1 ms, also 0,74).

**Pro Frame** (was der Frame kostet), sparse 20 000: `PhysicsStep` je Frame p50 88,8 auf 33,0 ms (p95 97,1 auf 46,1; max
118,8 auf 63,1), Frame-CPU p50 110,6 auf 56,2 ms. Vorher lief jeder Frame an die 5-Schritte-Grenze (4,96 Schritte je
Frame, die Simulation holte 0,75 der Echtzeit: 82,7 ms simuliert je 110,8 ms Wanduhr). Nachher sind es 3,38 Schritte je
Frame bei 56,2 ms Frame-Abstand, also 56,3 ms simuliert je 56,2 ms Wanduhr: **die Simulation läuft in Echtzeit statt mit
dreiviertel Geschwindigkeit, und die Grenze wird nicht mehr erreicht.** (Die Pro-Frame-Zahlen vergleichen unterschiedlich
viele Schritte je Frame, der faire Vergleich ist die Tabelle oben.)

**Auslastung** (CPU je Thread im Mess-Fenster, Sonde `thread_cpu_probe`; die Jolt-Worker tragen jetzt Namen `Jolt-0` bis
`Jolt-3`, vorher stand dort nur das Image):

| Szene | Hauptthread vorher / nachher | je Jolt-Worker vorher / nachher |
|---|---:|---:|
| sparse 1 000 | 12 % / 10 % | 2 % / 2 % |
| sparse 5 000 | 45 % / 33 % | 9 % / 9 % |
| sparse 20 000 | 94 % / 92 % | 28 bis 29 % / 37 % |
| dense 10 000 | 96 % / 89 % | 33 % / 34 % |
| sparse 5 000 + 500 Charaktere | 52 % / 44 % | 9 % / 10 % |

Bei 20 000 Körpern bleibt der Hauptthread nachher fast voll ausgelastet (92 %), weil der Frame kürzer wurde, nicht weil
der Schritt serieller wäre: der Schritt kostet 10,5 ms; nimmt man die 8,5 ms von `JPH::PhysicsSystem::Update` aus dem Bench
(Abschnitt 4, nicht im Loop gemessen) als Jolt-Anteil, bleiben für die Engine etwa 2 ms, vorher waren es 10,2 ms. Die
Jolt-Worker laufen länger (37 statt 28 %), weil sie in kürzerer Zeit dieselbe Arbeit tun. **`stepStats`:** keiner der Nachher-Läufe meldet verworfene Kontakte (kein
`Jolt dropped contacts` in den Logs, `ContactConstraintsFull` tritt in keiner Szene auf); die Zeilen unter
`docs/perf-audit/physics-threading-step4/*.runlog` belegen das. (`stepStats()` ist eine C++-Schnittstelle und steht nicht im
Profiler-Dump; der Beleg ist die gedrosselte Warnung, die beim ersten gesättigten Schritt kommen müsste.) Die
Logs enthalten bei den großen Szenen tausende Zeilen `PhysicsWorld::step took ... ms (budget 8.00 ms)`: das ist die
ältere Langsam-Schritt-Warnung der Engine (eine Zeile je Schritt über 8 ms, vorher wie nachher vorhanden), nicht das Zeitbudget
aus Schritt 3; die Zahlen stehen in den `*.physics-log.txt` (sparse 20 000: 4 341 vorher, 2 899 nachher, passend zu 4,96 gegen 3,38
Schritten je Frame).

**Bedingungen (ehrlich):**

| | |
|---|---|
| Gerät | MacBook Air von Connor, 10 Kerne, Release-Build, Metal |
| Strom | **Netzteil** (`AC Power`), Akku 90 %, Stromsparmodus aus (`pmset lowpowermode 0`) |
| Bildschirm | **gesperrt** (`CGSSessionScreenIsLocked=Yes`) wie in Schritt 1 und 2. Das versteckte Fenster wird nicht komponiert, `Metal::NextDrawable` wartet (24 ms je Frame bei den kleinen Szenen). Das bremst Frame-Zeit und FPS, nicht die CPU-Zeit der Physik; FPS-Zahlen stehen deshalb nirgends. Die vom Auftrag gewünschte Messung mit entsperrtem Bildschirm und vsync (genau ein Schritt je Frame) konnte ich nicht herstellen |
| Last | Load Average vor dem ersten Lauf 2,4, während der Läufe 1,7 bis 4,7 (der Spielprozess selbst, Hive-Dienste; kein Bau, kein Editor, kein anderes Spiel); der he_tests-Bau lief erst nach den Messungen |
| Vorher-Binary | `~/HorizonEnginePerf/he178_before` (Schritt 1, Quelle `a4fa3a9d`) |
| Nachher-Binary | `out/deploy/Game` dieses Zweigs, gebaut aus dem Kopf `af4f2868`, kopiert mit `selfcontain_deploy_copy.sh` nach `/tmp/he178_s4/gamenew` |
| Reihenfolge | je Szene Vorher, dann Nachher (abwechselnd), eigener frischer Prozess je Lauf, getrennte Arbeitsordner (`m_old`, `m_new`) |
| Einstellungen | Standardwerte (kein `HE_PHYSICS_THREADS`, kein `HE_PHYSICS_BUDGET_MS`): 4 Jolt-Worker, Zeitbudget 4 ms, vierfache Obergrenzen, Hold-Sonde bei "nichts halten" übersprungen |

**Grenzen der Zahlen:**

- **Ein Lauf je Zeile, außer sparse 20 000 und 5 000 (zwei Läufe).** Dense 10 000, sparse 1 000 und 5 000 mit 500 Charakteren
  stehen auf einem Lauf, ohne eigenen Rauschboden. Die Gegenprobe aus Schritt 2 (20 000: 18,75 und 18,91 vorher, 11,42 und
  10,96 nachher) und die Wiederholung oben (0,4 bis 1,4 %) sprechen für eine Streuung des Medians unter 5 %. Die Richtung ist in
  allen Zeilen gleich, die Größenordnung (-29 bis -42 %) deckt sich mit Schritt 2. Die Spitzen sind, wie oben, nicht belastbar.
- **Die Reihenfolge ist nur teilweise ausgeglichen:** Lauf 1 immer Vorher zuerst, Lauf 2 (nur zwei Szenen) Nachher zuerst. Der
  Load Average lag in Lauf 1 bei 1,7 bis 4,7, in Lauf 2 bei 3,2 bis 4,2 (bei sparse 20 000 trägt der Spielprozess selbst rund 2,3 Kerne davon: Hauptthread 0,9 plus vier Jolt-Worker zu je 0,36).
- **Schritt 3 steckt in den Zahlen, ist aber kaum sichtbar.** Das Zeitbudget für Charaktere, Abfragen und Events greift erst ab
  32 Charakteren und bei verbrauchtem Budget; die Lastszenen setzen stehende Charaktere (500 in einer Zeile) und keine
  Strahlen. Der Gewinn kommt aus Schritt 2 (Rückschreiben ohne Sperren, Hold-Sonde, Obergrenzen). Dass das Budget bei ausgeschalteter
  Wirkung nichts kostet, ist durch den Vergleich mit der Schritt-2-Messung (500 Charaktere: -33 % damals, -31 % jetzt) gestützt, aber nicht
  gegen Rauschen aufgelöst. Der von Schritt 3 gemeldete `stable_sort` über alle Charaktere in jedem Schritt (`PhysicsWorld.cpp:3300`,
  läuft solange das Budget an ist, also im Standard) steht unverändert im Code; die Zeile mit 500 Charakteren ist mit ihm
  gemessen, seine Kosten sind nicht isoliert. Bei einigen tausend Charakteren ist er der erste Kandidat für eine Nacharbeit.
- **dense 10 000** ist nicht rein Serienanteil: die vierfachen Obergrenzen aus Schritt 2 können Kontakte erhalten, die vorher
  stillschweigend abgeschnitten wurden. Im Vorher-Binary war das nicht zu sehen (das Flag wurde weggeworfen), im Nachher-Binary
  trat es nicht auf. Wie viel der -29 % darauf entfällt, ist nicht getrennt.
- **Die 5-Schritte-Grenze bleibt**, wenn die Physik für Echtzeit zu langsam ist. Bei 20 000 aktiven Körpern ist sie auf
  diesem Mac jetzt eingeholt; eine lineare Hochrechnung (10,5 ms je 20 000 Körper gegen 16,7 ms je Schritt) legt die Grenze bei
  etwa 30 000 aktiven Körpern nahe. Das ist **nicht gemessen**.
- **Nicht gemessen:** Streaming plus Physik im selben Frame, Mesh-Collider, Treppen, laufende Charaktere, `CastShape` und
  `CollideShape`, Strahlen unter Last, der Durchtunnelungs-Gegenversuch der Collision-Steps, Windows, Linux, und alles mit
  entsperrtem Bildschirm und vsync.

**Voll-Build und Tests** (macOS, Release, `HE_ENABLE_SHADERC=ON` laut `CMakeCache.txt`, Quelle gleich `origin/release/0.7.0`
`af4f2868`, alles im Vordergrund gebaut und gelaufen):

- **`HorizonGame`:** 35 Schritte, 1,5 Minuten, `BUILD_RC=0`.
- **`he_tests`** (mit allen Test-Bibliotheken): 396 Schritte, 12 Minuten, `BUILD_RC=0`, kein `error:` und kein `FAILED` im Log.
- **Voller `ctest -j4 --timeout 1500`: 261 Tests, 100 % bestanden** (`CTEST_RC=0`, 284,7 s). Übersprungen sind nur
  `runtime_size_app_advanced` und `runtime_size_app_basic` (kein Deploy-Ordner `AppAdvanced`/`AppBasic`, wie in Schritt 2 und 3).
  Kein roter Test, also auch kein Gegenlauf nötig. Die Tests aus den vom Auftrag genannten Bereichen sind alle grün:
  - Physik: `test_physics`, `test_physics_threading` (Schritt 2), `test_physics_budget` (Schritt 3), `test_collision_system`;
  - Charaktere und Bewegung: `test_character`, `test_player_host`, `test_player_settings`, `test_movement`;
  - Streaming: `test_async_streaming`, `test_cell_split`, `test_streaming_view`;
  - Projekt-Einstellungen mit dem neuen Feld: `test_project_settings`, `test_project_settings_ui`;
  - Netzwerk-Schicht, die auf die Physik aufsetzt: `test_game_replication`, `test_net_property_replication`, `test_net_game_session`,
    `test_anticheat`.
- **Nicht gebaut: `all`** (der Link von `HorizonEditor` mit `ProfilerPanel`, `ProjectSettingsPanel`, `EditorHelp`). Ich habe ihn
  gestartet (723 Schritte, nach den Erfahrungen aus Schritt 2 gut 19 Minuten) und für die Messwiederholung wieder abgebrochen,
  weil die Zeit bis zur Frist nicht für beides reichte. Das heißt: der Editor-Teil dieser Verifikation ist lokal **nicht** belegt
  (die Schritte 2 und 3 haben `all` gebaut, siehe `physics-threading-step2.md` und `physics-threading-budget.md`, jeweils
  Abschnitt 8; `git diff e3f5bff9 af4f2868 -- src tests` ist leer, der Quelltext gleicht also dem von Schritt 3).
  Zur CI: Der PR dieses Schritts ändert nur Doku, zum Zeitpunkt des Schreibens hatte er noch keinen Check. Der CI-Lauf 38158075083
  auf dem Kopf `4799cc01` (Quelltext gleich, `git diff 4799cc01 af4f2868` ist leer) war auf allen Plattformen grün; ob er den
  Editor-Link auf macOS einschließt, habe ich nicht nachgesehen.
- **Nicht gelaufen:** Windows und Linux lokal (nur CI), die zwei `runtime_size_app_*` (übersprungen).
