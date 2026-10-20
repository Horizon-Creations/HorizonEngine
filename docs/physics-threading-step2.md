# Physik-Multithreading: Konfiguration und Abstimmung (Thema 178, Schritt 2)

Stand 11.10.2026, Zweig `claude/physik-multithreading-jolt-jobsystem-thread-aufteilung-zeitbudget`. Aufbauend auf
`docs/physics-threading-baseline.md` (Schritt 1). Dieses Dokument beschreibt, was geändert wurde, welche Standardwerte
gelten und warum, was getestet ist und, in Abschnitt 8, was gemessen wurde und **was nicht**.

## 1. Kurzfassung

| Thema | Vorher | Jetzt |
|---|---|---|
| Worker-Zahl | fest `clamp(hw/2 - 1, 0, 4)` im Konstruktor, nicht änderbar | Projekteinstellung `physics.threads`, Umgebung `HE_PHYSICS_THREADS`, API `PhysicsWorld::setThreadCount`; Standard (Auto) bleibt die gemessene Regel |
| Jolt-Threads | auf macOS unbenannt | `Jolt-0` bis `Jolt-n` (auch im Engine-Log und in Profilern) |
| `TempAllocator` | Jolt `TempAllocatorImpl`, fest 10 MB, bricht bei Überlauf mit `std::abort()` ab | `TempAllocatorImplWithMallocFallback`, Größe aus den Obergrenzen berechnet (`PhysicsTuning::tempBytesFor`); ein Überlauf kostet ein `malloc`, nie den Prozess |
| Kontakt-Obergrenzen | 65 536 Paare, 10 240 Constraints, still abgeschnitten | 262 144 Paare, 40 960 Constraints (`HE::PhysicsLimits`, im Konstruktor wählbar) |
| Rückgabewert von `Update` | weggeworfen | gezählt (`PhysicsWorld::stepStats()`), gedrosselt geloggt |
| Collision-Steps | immer 1 | `round(dt * 60)`, mindestens 1, höchstens 8; 1 bei 45 Hz und darüber, also im Standard |
| Region-Hold | bei jedem Schritt Pass über alle dynamischen Körper | optionale Sonde `mayHold`; Pass entfällt, solange sie "nein" sagt und nichts gehalten wird |
| Rückschreiben | 3 gesperrte Jolt-Aufrufe und ein Map-Lookup je Körper | ein Zugriff über `BodyLockInterfaceNoLock::TryGetBody` |

Nicht angefasst: Gameplay-API, Netzwerkschicht, Kontakt-Listener (Abschnitt 6), Determinismus.

## 2. Thread-Anzahl

**Bedeutung.** "Threads" zählt alle, die an einem Schritt teilnehmen, den aufrufenden eingeschlossen: 1 ist single-threaded,
5 ist der Aufrufer plus vier Jolt-Worker. 0 heißt automatisch.

**Wo man sie einstellt** (Vorrang von oben nach unten):

1. `HE_PHYSICS_THREADS=<n>` in der Umgebung. Gewinnt immer, auch gegen die Projektdatei; gedacht für Benchmarks und
   Fehlerberichte. `0` heißt automatisch, ungültige Werte werden ignoriert, Obergrenze 32.
2. `Config/ProjectSettings.json`, `"physics": { "threads": n }` (0 = automatisch). Im Editor unter Project Settings, Physics,
   "Physics threads". Editor-Play und exportiertes Spiel lesen denselben Wert; eine Änderung während Play greift sofort
   (`applyPhysicsSettings`).
3. API: `PhysicsWorld::setThreadCount(n)`, `threadCount()`. Startet die Worker nur neu, wenn sich die Zahl ändert; nur zwischen
   Schritten aufrufen (beide Anwendungen tun es vor `initialize()`).

**Auto-Regel** (`PhysicsTuning::autoWorkers`, rein und getestet): `clamp(hardware_threads / 2 - 1, 0, 4)` Worker plus der Aufrufer.

| Hardware-Threads | 2 | 4 | 6 | 8 | 10 | 12 und mehr |
|---|---|---|---|---|---|---|
| Threads im Schritt (Auto) | 1 | 2 | 3 | 4 | 5 | 5 |

**Warum die Regel so bleibt.** Die Baseline zeigt das Knie bei drei bis vier Workern (20 000 springende Körper: 27 ms ohne
Worker, 8,65 ms mit vier, 7,9 ms mit neun auf zehn Kernen), und die Hälfte der Kerne ist, was neben einem Schritt für die
anderen übrig bleibt. Mehr Worker sind nicht der Hebel; sie wären es auf einem Rechner mit sehr vielen Kernen und sehr
großen Szenen, aber das ist hier nicht gemessen, also ist es ein Wert in der Einstellung und kein neuer Standard.

**Die Simulation ist unabhängig von der Zahl.** Jolt verspricht Determinismus unabhängig von der Thread-Anzahl; der Test
`PhysicsWorld: the result does not depend on the number of threads` prüft es für diese Engine bitweise (192 Kisten, 150
Schritte, Pose als rohe Float-Bits, Kontakt-Ereignisse in Reihenfolge) mit 1, 4 und 8 Threads und mit einem Neustart der
Worker mitten in der Simulation. Dazu läuft die gesamte Physik-Teilmenge der Tests (Abschnitt 7) zusätzlich mit
`HE_PHYSICS_THREADS=1`, `2` und `8` grün.

## 3. Abstimmung mit dem Engine-Pool (`globalPool()`)

Was neben `PhysicsSystem::Update` tatsächlich läuft:

- **Streaming** (`CellStreamer`, `ContentManager`) läuft mit Priorität `Low`; der Pool begrenzt `Low` bereits auf
  `threads - 2` gleichzeitige Worker (`ThreadPool`-Konstruktor), damit zwei Kerne für den Frame frei bleiben.
- **Render-Extraktion und Culling** laufen als `parallel_for` mit Priorität `High`. Im Aufrufbaum des Spielframes
  (`GameApplication::OnRender`, per `sample` gesehen) kommen sie nach dem Schritt auf demselben Hauptthread, also nicht
  gleichzeitig mit `Update`; ein eigener Renderthread wurde nicht gefunden, aber auch nicht gesondert ausgeschlossen.
- **Audio** ist ein Gerätethread.

Gleichzeitig mit einem Schritt laufen also nur Streaming-Jobs (höchstens `threads - 2` Pool-Worker) und der Audio-Thread.
Die Auto-Regel hält die Physik bei höchstens der Hälfte der Kerne (Worker plus Aufrufer) und lässt der Pool-Last damit
mindestens die andere Hälfte. Die Baseline hat den schlechtesten Fall gemessen (zehn voll drehende Threads: Update
8,7 auf 24,5 ms); im echten Loop waren die Pool-Threads ohne Streaming zu 1 % ausgelastet.

**Bewusst nicht gemacht**, mit Begründung:

- *Die `Low`-Grenze um die Physik-Worker senken.* Das würde Streaming dauerhaft verlangsamen, um eine Überlappung zu
  entschärfen, die nur während der wenigen Millisekunden eines Schritts besteht, und es ist ohne Szene mit Streaming
  und Physik im selben Frame (Thema 164) nicht zu bewerten.
- *Die Jolt-Jobs auf `globalPool()` legen* (eigener `JobSystem` für Jolt). Jolt braucht prompte Wake-ups für Barrieren;
  ob `High` das zuverlässig liefert, während Streaming-Jobs laufen, ist nicht gemessen. Das Risiko (ein hängender
  Schritt) steht in keinem Verhältnis zu einem ungemessenen Gewinn.
- *Thread-QoS/Priorität.* Auf macOS erben alle Threads die QoS des Hauptthreads; es gäbe keinen Unterschied zwischen
  Jolt-Workern und Pool, solange man nicht den Pool absenkt.

`HE_PHYSICS_THREADS` ist der Schalter, mit dem Schritt 4 (oder Thema 164) eine Streaming-plus-Physik-Szene mit 1, 2, 4 Threads
fahren kann, ohne neu zu bauen.

## 4. Scratch-Speicher und Obergrenzen

Jolt reserviert seine Puffer pro Schritt nach der **Obergrenze**, nicht nach dem, was gerade in Kontakt ist (Baseline,
Abschnitt 6): etwa 830 Byte je zulässigem Kontakt-Constraint plus 20 Byte je Körper. Deshalb hängt der Block an den
Obergrenzen und nicht an der aktuellen Körperzahl; eine Anpassung zur Laufzeit würde nichts einsparen und einen Block,
der gerade verwendet wird, nicht vergrößern können.

`PhysicsTuning::tempBytesFor(maxBodies, maxContactConstraints)` = `(900 B * Constraints + 32 B * Körper) * 1,5`, auf volle MiB
aufgerundet, mindestens 1 MiB. Der Test hält das gegen die gemessenen Spitzen (9,9 MB bei 10 240, 34,06 MB bei 40 960,
67,96 MB bei 81 920 Constraints) mit mindestens 40 % Reserve.

| Obergrenzen | Block (virtuell) | gemessene Spitze |
|---|---:|---:|
| alt: 65 536 Paare, 10 240 Constraints | 17 MiB (vorher 10 MiB) | 9,9 MB |
| neu: 262 144 Paare, 40 960 Constraints | 56 MiB | 34,06 MB |

Der Block ist virtueller Speicher: nur beschriebene Seiten zählen. Gemessen im Bench (Haufen aus 10 000 Kisten): Spitzen-RSS
25,8 MB bei den alten Obergrenzen, 29,9 MB bei den vierfachen, obwohl die Temp-Spitze bei 34 MB liegt.

**Warum die Obergrenzen vervierfacht wurden.** Der Haufen mit 10 000 Kisten lief bei den alten Obergrenzen in 55 von 60
Schritten in `ContactConstraintsFull` (Jolt lässt Kontakte weg, Körper sinken ineinander), und die Baseline hat gemessen, dass derselbe
Haufen mit vierfachen Obergrenzen doppelt so schnell ist (20,4 gegen 41,3 ms Update), weil Jolt das Abgeschnittene
nicht jeden Schritt neu versucht. Mit den neuen Werten meldet der Bench für diesen Haufen 0 Fehler. Der Preis ist
Adressraum und ein etwas größeres Löschen der Cache-Tabellen je Schritt (rund 2,6 MB, in der Größenordnung 0,1 ms; im
Bench unter Last nicht von Rauschen zu trennen; im Spielloop bei 1 000 Körpern kein Mehraufwand sichtbar, Abschnitt 8). Szenen unterhalb der Grenzen rechnen unverändert.

`PhysicsWorld(const HE::PhysicsLimits&)` erlaubt andere Werte (der Test nutzt 64 Paare und 16 Constraints, um die
Sättigung billig zu erzeugen); der Block wird aus denselben Zahlen abgeleitet, beide können also nicht auseinanderlaufen.

## 5. Collision-Steps bei großem `dt`

Jolt will höchstens 1/60 s je Collision-Step: es integriert einmal je Step und löst danach die Kontakte. Die feste Rate eines
Projekts ist frei (10 bis 480 Hz); ein 30-Hz-Projekt rief bisher `Update(1/30, 1)` und lag damit beim Doppelten von Jolts
eigener Grenze, ein 10-Hz-Projekt beim Sechsfachen.

`PhysicsTuning::collisionStepsFor(dt) = clamp(round(dt * 60), 1, 8)`. Gerundet statt aufgerundet, damit 60 Hz, 50 Hz und 45 Hz
bei einem Step bleiben (der Standard und jede übliche Rate sind unverändert); 30 Hz bekommt zwei, 20 Hz drei, 10 Hz sechs. Der
Aufwand je simulierter Sekunde bleibt etwa gleich, weil das Projekt entsprechend seltener steppt. Die Anzahl steht in
`stepStats().collisionSteps`.

Nicht gemessen: der Durchtunnelungs-Gegenversuch (schneller Körper gegen dünnen Boden bei 30 Hz mit einem gegen zwei Steps).
Die Regel folgt Jolts dokumentierter Empfehlung; der Test pinnt die Rechenregel, nicht den Effekt.

## 6. Kontakt-Listener: die Garantie

Unverändert und erhalten: `OnContactValidate`, `OnContactAdded` und `OnContactRemoved` laufen unter einem `std::mutex`; die
Ereignisse werden nach Entity-Paar sortiert herausgegeben (Thema 153), und `setPairCollides` wird nur zwischen Schritten vom
Spielthread gerufen, was den Mutex im Betrieb unumstritten hält. `setThreadCount` ist ebenfalls nur zwischen Schritten legal
und wird nur dort gerufen.

Belegt wird es durch den Determinismus-Test (Abschnitt 2): dieselben Kontakt-Ereignisse in derselben Reihenfolge bei 1, 4 und 8
Threads und nach einem Neustart der Worker, bei einer Szene mit mehr als 192 Ereignissen (also nicht vakuös).

## 7. Serieller Anteil

Die Baseline zerlegt `PhysicsWorld::step` bei 20 000 Körpern: `Update` 43 %, `applyRegionHold` 19 %, gesperrte Abfragen je
Körper 23 %, `writeBackWorldPose` 7 %. Zwei Posten wurden ohne Verhaltensänderung angegangen:

1. **`applyRegionHold`.** `GameApplication` setzt die Prüf-Funktion immer, sie antwortet aber `false`, solange der Streamer
   keine Zellen hat (`CellStreamer::holdsAt` beginnt mit `if (!active()) return false`). Neu: `setRegionHold(test, mayHold)`
   mit einer billigen Sonde (`m_cellStreamer.active()`). Sagt sie "nein" und ist nichts gehalten, entfällt der Durchlauf; sagt
   sie "ja" oder ist etwas gehalten (und müsste freigegeben werden), läuft er wie vorher. Der Vertrag geht nur in eine Richtung:
   die Sonde darf "ja" sagen, wo der Test "nein" sagen würde, nie umgekehrt. Ohne Sonde (alle Tests, jeder andere Aufrufer) ändert
   sich nichts. Auch der Aufruf im Charakter-Loop folgt der Sonde. Der Durchlauf selbst (wenn er läuft) geht jetzt über eine
   Tabelle `Entity -> BodyID` statt über eine Menge mit zwei Zusatzsuchen je Körper und liest die Position ohne Sperre.
2. **Rückschreiben.** Je dynamischem Körper waren es drei gesperrte Jolt-Aufrufe (`GetMotionType`, `GetPosition`,
   `GetRotation`, jeder mit Mutex) und ein zweiter Map-Lookup. Jetzt ein Zugriff über `BodyLockInterfaceNoLock::TryGetBody`
   (prüft die Sequenznummer der Id, ein inzwischen entfernter Körper fällt wie vorher durch) und direkte Lesezugriffe. Das ist
   zulässig, weil `Update` zurückgekehrt ist (kein Worker in einem Körper) und alles, was Körper schreibt, Aufrufe des
   Spielthreads sind. Gelesen werden dieselben Felder wie von den gesperrten Aufrufen; die Physik-Tests (Abschnitt 8)
   laufen darüber unverändert grün.

Beides ändert keine Ausgabe. Was bleibt (Hinweise für Schritt 3): der Schnappschuss der Körper-Map je Schritt, die
Reap-Prüfung aller (auch statischer) Körper je Schritt, `glm::eulerAngles` je Körper, `ExtendedUpdate` je Charakter, und
der Hold-Durchlauf, sobald ein Streamer aktiv ist (O(dynamische Körper) je Schritt mit einem `std::function`-Aufruf).

## 8. Tests und Messungen (ehrlich)

**Neue Tests** (`tests/test_physics_threading.cpp`, 12 Fälle, 97 Assertions; dazu ein Fall in `test_project_settings.cpp` und
das Rundlauf-Feld `threads`): Auto-Regel, Auflösung mit Umgebung, Parsen von `HE_PHYSICS_THREADS`, Collision-Step-Regel,
Block-Größe gegen die gemessenen Spitzen, `setThreadCount`, bitweise gleiches Ergebnis bei 1/4/8 Threads und Neustart,
gezählte Sättigung bei winzigen Obergrenzen ohne Absturz (und keine bei den Standardwerten), Collision-Steps im echten
`step()`, drei Fälle zur Hold-Sonde (nicht gefragt solange "nein", ohne Sonde wie bisher, ein gehaltener Körper wird auch bei
"nein" freigegeben). `scripts/editor_help_audit.py --check`: 1129 von 1129 Steuerelementen abgedeckt (Hilfe-Eintrag für das neue Feld).

**Ausgeführt (macOS, Release, shaderc ON, Stand `aa92c54a` = dieser Zweig mit eingemergtem `origin/release/0.7.0`):**

- **Voller Bau:** `he_tests` (361 Schritte nach dem Merge) und danach `all` (638 Schritte, mit dem Link von `HorizonEditor`):
  fehlerfrei.
- **Voller `ctest -j4 --timeout 1500`, 260 Tests: 100 % bestanden** (erster Lauf nach dem `he_tests`-Bau 242 s, zweiter Lauf nach
  `all` 283 s). Übersprungen sind `runtime_size_app_advanced` und `runtime_size_app_basic`: sie messen `out/deploy/AppAdvanced`
  und `AppBasic`, die nur beim Verpacken der App-Varianten entstehen ("nothing to measure", `SKIP_RETURN_CODE 2`), das ist bei jedem
  Standardbau so. Kein roter Test, also auch kein Gegenlauf auf `origin/release/0.7.0` nötig.
- **CI-Lauf 38140744172** (Kopf `8acc03af`, vor dem Merge): Linux, Linux · Vulkan (lavapipe), Windows und macOS alle **grün**.
- Davor, als der Kaltbau nicht passte: eine aus der Link-Zeile von `he_tests` abgeleitete Teilmenge mit allen Testdateien, die
  `PhysicsWorld` berühren, 659 Fälle, 36 796 Assertions, 0 Fehler; dieselbe Teilmenge mit `HE_PHYSICS_THREADS` 1, 2 und 8 grün.
  Der volle Lauf ersetzt sie.
- `scripts/editor_help_audit.py --check`: 1129 von 1129 Steuerelementen abgedeckt (läuft auch im `ctest`).

**Vorher/Nachher im echten Spielloop** (`scripts/perf/physics_load_run.py`, Standard: 300 Warm-up- und 600 Mess-Frames,
`HE_PROFILE_COUNTERS=0`, verstecktes Fenster, abwechselnd Vorher und Nachher).

Bedingungen: MacBook Air, 10 Kerne, Release; Netzteil, Akku 90 %, Stromsparmodus aus; **Bildschirm gesperrt**, wie in der
Baseline (`Metal::NextDrawable` wartet, das betrifft die Frame-Zeit, nicht die CPU-Zeit der Physik); Load Average 1,5 bis 4,4,
kein fremder Bau, kein anderes Spiel oder Editor. Vorher-Binary `~/HorizonEnginePerf/he178_before` (Quelle `a4fa3a9d`, Schritt 1),
Nachher-Binary dieser Stand (`out/deploy/Game`, mit `selfcontain_deploy_copy.sh` kopiert; der Merge von `release/0.7.0` bringt
dort nur Audio und Editor-UI). Die Vorher-Werte decken sich mit der Baseline-Tabelle (20 000: 18,74 dort, 18,75 und 18,91 hier;
5 000: 4,46 und 4,45; 1 000: 0,94 und 0,95; dense 10 000: 12,01 und 12,18), die Bedingungen sind also vergleichbar.

Zeit je Schritt in ms, `PhysicsStep` geteilt durch die nachgerechnete Schrittzahl (p50 / p95 / max):

| Szene | Vorher | Nachher | p50 |
|---|---|---|---:|
| sparse 20 000, Lauf 1 | 18,75 / 21,27 / 27,99 | 11,42 / 13,49 / 17,07 | -39 % |
| sparse 20 000, Lauf 2 | 18,91 / 21,99 / 28,33 | 10,96 / 13,05 / 29,38 | -42 % |
| sparse 5 000 | 4,45 / 5,32 / 8,85 | 2,52 / 3,08 / 3,89 | -43 % |
| sparse 1 000 | 0,95 / 1,17 / 2,43 | 0,58 / 0,81 / 1,53 | -39 % |
| dense 10 000 | 12,18 / 13,99 / 16,25 | 8,64 / 10,19 / 21,22 | -29 % |
| sparse 5 000 + 500 Charaktere | 5,89 / 7,12 / 9,15 | 3,92 / 4,81 / 6,70 | -33 % |

Bei 20 000 Körpern sinkt der Frame (p50) von 118,8 auf 74,2 ms und von 119,1 auf 66,8 ms; die Simulation läuft nicht mehr an der
5-Schritte-Grenze (nachgerechnete Schritte je Frame 4,90 und 5,00 vorher, 4,46 und 3,65 nachher). Die Jolt-Worker sind
nachher höher ausgelastet (39 % statt 28 %, derselbe Jolt-Anteil in kürzerem Schritt). Die vierfachen Obergrenzen kosten bei
1 000 Körpern nichts Messbares (dort wäre ein fester Aufwand je Schritt am deutlichsten); die Sorge aus der Messung unter fremder
Last (mehr Worker-CPU) hat sich nicht bestätigt.

Grenzen der Zahlen: ein Lauf je Zeile (außer 20 000), kein Rauschboden; die Schrittzahl je Frame ist aus `deltaMs`
zurückgerechnet (`physics_load_run.py`), die Zeilen mit genau einem Schritt (`one-step frames`) liegen bei ähnlichen Werten. Welcher
Anteil auf die Hold-Sonde und welcher auf das Rückschreiben ohne Sperren entfällt, ist nicht getrennt gemessen (es gibt keine
Profiler-Scopes in `step`); die Baseline-Aufschlüsselung (Hold 19 %, gesperrte Abfragen 23 % eines 20 000-Schritts) und eine
`sample`-Probe (`applyRegionHold` 34 % von `step` vor, 0 nach) erklären die Größenordnung. Jolt selbst (`Update`) läuft
unverändert.

**Nicht gemessen:** der Durchtunnelungs-Gegenversuch für die Collision-Steps (30 Hz, ein gegen zwei Steps); Streaming plus Physik
im selben Frame; Mesh-Collider, Treppen, laufende Charaktere; Windows und Linux (dort läuft nur die CI, nicht die Messung).
Nicht in der lokalen Durchführung ersetzbar: `he_mcp_shim` lief im `ctest` mit, `runtime_size_app_*` blieben übersprungen.

## 9. Hinweise für Schritt 3 und 4

- `stepStats()` (Schritte, gesättigte Schritte, Fehlerflags, Collision-Steps) ist die Stelle, an der ein Zeitbudget
  und eine Anzeige im Profiler ansetzen können; `Impl` kennt jetzt `limits`, `threads` und `stats`.
- Profiler-Scopes in `PhysicsWorld::step` fehlen weiterhin (`Update`, Rückschreiben, Charaktere, Hold); mit ihnen wäre die
  `sample`-Aufschlüsselung dieses Dokuments im Profiler sichtbar und für Vorher/Nachher nicht auf `sample` angewiesen.
- Die 5-Schritte-Grenze von `advanceFixedSteps` ist unverändert. Bei 20 000 Körpern lief jeder Frame an ihr (Simulation 0,7-fach, Baseline);
  jetzt liegt der Schnitt bei 3,7 bis 4,5 Schritten je Frame (Abschnitt 8), die Grenze bleibt aber die Stelle, die Schritt 3 beim
  Zeitbudget im Blick haben muss.
- Website-Doku (HorizonEngineDocs, Project Settings, Physics) kennt das neue Feld "Physics threads" noch nicht; die Datei liegt
  außerhalb dieses Repos und wurde nicht angefasst.
