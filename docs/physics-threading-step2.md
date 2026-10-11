# Physik-Multithreading: Konfiguration und Abstimmung (Thema 178, Schritt 2)

Stand 11.10.2026, Zweig `claude/physik-multithreading-jolt-jobsystem-thread-aufteilung-zeitbudget`. Aufbauend auf
`docs/physics-threading-baseline.md` (Schritt 1). Dieses Dokument beschreibt, was geändert wurde, welche Standardwerte
gelten und warum, was getestet ist und, in Abschnitt 8, **was in diesem Schritt nicht gemessen werden konnte**.

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
Bench unter Last nicht von Rauschen zu trennen, Abschnitt 8). Szenen unterhalb der Grenzen rechnen unverändert.

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

**Ausgeführt (macOS, Release):** eine aus der Link-Zeile von `he_tests` abgeleitete Teilmenge `he_tests_phys` mit allen
Testdateien, die `PhysicsWorld` berühren: `test_physics`, `test_physics_threading`, `test_character`, `test_raycast`,
`test_collision_system`, `test_collider`, `test_project_settings`, `test_world_scale` (44 Fälle ohne Benchmarks),
`test_entity_host`, `test_movement`, `test_terrain`, `test_root_motion`, `test_camera_rig`, `test_third_person_template`,
`test_navigation`, `test_net_game_session`, `test_net_rpc`, `test_engine_api`, `test_particles`, `test_animation_ik`,
`test_anticheat`, `test_project_exporter`: **659 Fälle, 36 796 Assertions, 0 Fehler** (7 übersprungen, alle schon vorher mit
`skip`). Dieselbe Teilmenge ohne `test_world_scale` zusätzlich mit `HE_PHYSICS_THREADS` 1, 2 und 8: je 359 Fälle grün.
`EditorApplication.cpp` und `ProjectSettingsPanel.cpp` kompilieren (Editor-Binary nicht gelinkt).

**Nicht ausgeführt, abgeschnitten:**

- Der **volle `he_tests`-Bau und der volle `ctest`**. Der Worktree hatte keinen Build; ein Kaltbau hat 1364 Objekte, die
  letzten rund 340 sind schwere Test-TUs, und auf dem Gerät lief gleichzeitig der Kaltbau einer anderen Instanz (Load 12 bis 23
  auf 10 Kernen). Nach 24 Minuten standen 1005 von 1364; der Rest hätte bei dieser Rate mehr als 40 Minuten gebraucht. Die
  Matrix übernimmt die CI auf dem Zweig (Windows, Linux, macOS).
- Nicht in der Teilmenge: `test_python_scripting`, `test_viewport_overlays`, `test_gamelogic_services`,
  `test_project_settings_ui` (je mit eigenen Zusatz-Objekten), `editor_help_audit` lief als Skript (grün), `he_mcp_shim` nicht.
- **Sauberes Vorher/Nachher der Frame-Zeit.** Die Echt-Loop-Läufe (`physics_load_run.py`, Szene sparse, 10 000 Körper, 150
  Frames, Vorher-Binary `~/HorizonEnginePerf/he178_before`, Nachher-Binary dieser Stand) liefen unter fremder Last; die
  Zeit je Schritt ist dort Rauschen (p50 17,35 gegen 17,22 ms), weil die Jolt-Worker um Kerne konkurrieren. Was sich unter
  dieser Last trotzdem sagen lässt:
  - Hauptthread-CPU (Thread-Probe) für dieselbe Schrittzahl (rund 740): 7523 ms in 18,4 s vor, 6750 ms in 18,7 s nach, also
    rund 10,1 gegen 9,1 ms je Schritt (minus 10 %, ein Paar, kein Rauschboden).
  - `sample` auf den Hauptthread (rund 150 Proben je Lauf): `applyRegionHold` 42 von 124 Proben in `step` (34 %, davon 30 in
    `std::mutex::lock` der gesperrten `GetPosition`) vor, **0** nach. Der Anteil von `step` ohne `Update` an allen
    Hauptthread-Proben sinkt von rund 51 % (82 von 160) auf rund 26 % (37 von 144). Mit rund 150 Proben je Lauf sind das Größenordnungen,
    keine Prozentpunkte.
  - Die Jolt-Worker verbrauchten im Nachher-Lauf mehr CPU (4 x 2,1 s gegen 4 x 1,4 s). Der Bench ohne Engine zeigt bei gleicher
    Szene mit den alten und den neuen Obergrenzen 2594 und 2233 ms gegen 2718 und 2634 ms CPU für 150 Schritte, bei 16 %
    Spanne innerhalb derselben Einstellung. Ob die größeren Tabellen etwas kosten, ist offen.
  Schritt 4 muss die Zahlen auf einem ruhigen Gerät (kein fremder Bau, Netz, Bildschirm entsperrt) neu erheben, abwechselnd
  alt und neu, mehrere Läufe. Die Binaries liegen: Vorher `~/HorizonEnginePerf/he178_before`, Nachher wird aus dem Zweig gebaut.

## 9. Hinweise für Schritt 3 und 4

- `stepStats()` (Schritte, gesättigte Schritte, Fehlerflags, Collision-Steps) ist die Stelle, an der ein Zeitbudget
  und eine Anzeige im Profiler ansetzen können; `Impl` kennt jetzt `limits`, `threads` und `stats`.
- Profiler-Scopes in `PhysicsWorld::step` fehlen weiterhin (`Update`, Rückschreiben, Charaktere, Hold); mit ihnen wäre die
  `sample`-Aufschlüsselung dieses Dokuments im Profiler sichtbar und für Vorher/Nachher nicht auf `sample` angewiesen.
- Die 5-Schritte-Grenze von `advanceFixedSteps` (Simulation bei 20 000 Körpern 0,7-fach) ist unverändert.
- Website-Doku (HorizonEngineDocs, Project Settings, Physics) kennt das neue Feld "Physics threads" noch nicht; die Datei liegt
  außerhalb dieses Repos und wurde nicht angefasst.
