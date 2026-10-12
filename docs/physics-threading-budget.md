# Physik-Multithreading: Zeitbudget für die Arbeit um Jolt (Thema 178, Schritt 3)

Stand 11.10.2026, Zweig `claude/physik-multithreading-jolt-jobsystem-thread-aufteilung-zeitbudget`. Aufbauend auf
`docs/physics-threading-baseline.md` (Schritt 1) und `docs/physics-threading-step2.md` (Schritt 2). Dieses Dokument
beschreibt, was das Budget begrenzt, was es **nicht** begrenzt, die Standardwerte, die Tests und, in Abschnitt 8, was
gemessen wurde und was nicht.

## 1. Kurzfassung

| Arbeit um `Update()` | Was das Budget tut | Wo |
|---|---|---|
| Charaktere (`ExtendedUpdate`) | Reicht das Budget nicht, wartet der Rest bis zum nächsten Schritt und wird dann mit der **verpassten Zeit** gesteppt (`dt + Schuld`). Wer am längsten wartet, kommt zuerst dran. Die ersten 32 werden immer gesteppt | `PhysicsWorld::step` |
| Szenenabfragen (`raycast`, `sphereCast`, `boxCast`, `capsuleCast`, `overlap*`, `raycastAll`) | **Gezählt und gebucht**, nicht aufgeschoben (sie sind synchron: ein Skript will die Antwort in derselben Zeile). Die gebuchte Zeit geht im nächsten Schritt vom Anteil der Charaktere ab | `QueryCharge` |
| Körper anlegen/entfernen beim Zellen-Streaming (`addEntity`, `addEntities`, `removeEntityImpl`, `removeEntityTree`) | Ebenfalls **gebucht**. Der Streamer hat sein eigenes Slice-Budget (`CellStreamer::update(..., budgetMs)`, in `GameApplication` 4 ms) und sein `bodiesFit`-Tor; das wurde nicht angefasst | `BodyWorkCharge` |
| Kontakt-Events | Höchstens `eventsPerPoll` je Liste und Abruf (Standard 4096). Der Rest bleibt **in Reihenfolge** vorn in der Warteschlange und geht beim nächsten Abruf zuerst hinaus. Ein Exit geht nie vor dem Enter desselben Kontakts hinaus | `HEContactListener::take` |
| Wasser (Thema 179): Auftrieb, Dämpfung, Strömung für Körper im Wasser | **Gezählt, begrenzt, gebucht** (Abschnitt 10): höchstens `waterBodiesPerStep` (256, 0 = alle) Körper je Schritt; die übrigen kommen im nächsten Schritt mit der **verpassten Zeit** dran (`steppedDt`, wie ein aufgeschobener Charakter), am längsten Wartende zuerst. Die Zeit des Schritts geht vom Anteil der Charaktere ab | `PhysicsWorld::applyWater` |

Nicht im Budget: `PhysicsSystem::Update()` selbst. Dafür sind der Schritt-Deckel von `advanceFixedSteps` (5 je Frame), die
Thread-Anzahl (Schritt 2) und die Obergrenzen da. Das Budget begrenzt, was mit der Zahl der **Entities** wächst und
bisher ungeteilt in dem Schritt lief, der es gerade verlangte.

## 2. Einstellung

| | |
|---|---|
| Projekt | `Config/ProjectSettings.json`, `"physics": { "budgetMs": 4.0, "eventsPerPoll": 4096 }`. Im Editor unter Project Settings, Physics, "Time budget": Feld "Physics budget" (0 = Off) mit Hilfe-Eintrag. `eventsPerPoll` nur in der Datei |
| Umgebung | `HE_PHYSICS_BUDGET_MS=<ms>` gewinnt immer (wie `HE_PHYSICS_THREADS`); `0` schaltet das Budget aus; ungültige Werte werden ignoriert |
| API | `PhysicsWorld::setBudget({frameMs, eventsPerPoll})`, `budget()`, `budgetStats()`, `PhysicsWorld::lastBudgetStats()` (der neueste Schritt irgendeiner Welt, für das Profiler-Fenster) |
| Wann | Zwischen Schritten. Beide Anwendungen setzen es vor `initialize()`; eine Änderung während Play greift sofort (`applyPhysicsSettings`) |

**Der Standard (4 ms je Schritt, 4096 Events)** ist ein Viertel eines 60-Hz-Frames. Gemessen (Baseline, Abschnitt 5 und 7):
500 stehende Charaktere kosten 1,2 ms, 1 000 laufende 4,7 ms. Eine gewöhnliche Szene erreicht das Budget nie, eine Menge
von einigen tausend Charakteren wird auf mehrere Schritte verteilt.

**Bedeutung von "je Schritt".** Das Budget gilt für jeden `step()`-Aufruf, nicht für den ganzen Frame. Ein Frame mit fünf
nachgeholten festen Schritten bekommt also bis zu fünf Budgets. Der Deckel von fünf Schritten je Frame bleibt die Grenze
für die Spirale; ein eigener Frame-Zähler wäre ein Eingriff in `GameApplication` und `EditorApplication` gewesen, den dieser
Schritt nicht gemacht hat.

## 3. Charaktere: Überlauf und Nachholen

Vor der Schleife über die Charaktere wird gerechnet: Was Abfragen und Körperarbeit seit dem vorigen Schritt gebucht haben,
geht vom Budget ab; die Charaktere bekommen den Rest. Vor jedem Charakter (außer den ersten 32) wird gefragt, ob
`gebucht + bisher verbraucht >= Budget`.

* **Aufschub.** Ein aufgeschobener Charakter wird in diesem Schritt nicht angefasst (kein `ExtendedUpdate`, kein Rückschreiben,
  kein `MoveKinematic`) und schuldet eine Schrittlänge mehr (`characterOwed`).
* **Nachholen.** Beim nächsten Mal steppt er mit `dt + Schuld` (`PhysicsBudgetRules::steppedDt`): Schwerkraft, Bewegung,
  `airTime` und der Kamera-Proxy (`MoveKinematic`) rechnen mit derselben Zeit. Er legt dieselbe Strecke zurück und fällt gleich weit,
  nur später.
* **Reihenfolge.** Die mit der größten Schuld kommen zuerst, bei gleicher Schuld die kleinere Entity-Id (nie die Hash-Reihenfolge).
  Daraus folgt eine Reihumbedienung: bei 96 Charakteren und 32 je Schritt wird jeder alle drei Schritte gesteppt.
* **Obergrenze der Schuld.** `kMaxOwedSteps = 4`: höchstens fünf Schrittlängen auf einmal (83 ms bei 60 Hz). `ExtendedUpdate`
  mit sehr großem `dt` tunnelt und verliert den Boden; eine Menge, die das Budget nie einholt, wird langsamer und springt nicht.
* **Garantie.** Die ersten `kGuaranteedCharacters = 32` je Schritt werden immer gesteppt (rund 150 µs). Ein Spiel mit ein paar
  Charakteren und jeder Test sieht nie einen Aufschub, auch auf einer langsamen oder ausgelasteten Maschine; ein Budget unter
  einem `ExtendedUpdate` bleibt trotzdem lebendig.
* **Entfernen.** `removeEntityImpl` löscht die Schuld; ein Charakter, dessen Entity gelöscht wurde, fällt wie vorher durch den
  Reap.
* **Region-Hold** (Charakter auf einer noch nicht gebauten Zelle) kommt vor der Budgetfrage und bleibt, wie er war.

Ohne Budget (`budgetMs = 0`) läuft nichts davon: dieselbe Schleife, dasselbe `dt`, bitgleich zu vorher.

## 4. Szenenabfragen und Körperarbeit: gebucht, nicht aufgeschoben

Ein `raycast` liefert seine Antwort in der Zeile, in der das Skript ihn aufruft. Ihn "auf einen Folgeframe zu verschieben"
hieße, eine asynchrone Abfrage-API zu erfinden (Handle, Ergebnis später) und jedes Skript umzuschreiben, und das ist ein neues
Physik-Feature, das dieses Thema ausschließt. Stattdessen:

* jede Abfrage misst ihre Dauer (`steady_clock`, etwa 50 ns Aufwand gegen 1,2 µs je Strahl) und zählt sich, aus beliebigem Thread
  (atomar); verschachtelte Abfragen buchen einmal;
* der nächste Schritt entnimmt die Summe und zieht sie vom Anteil der Charaktere ab. Ein Skript, das in einem Frame 20 ms
  Strahlen schießt, lässt dem nächsten Schritt von den Charakteren nur die garantierten 32 übrig;
* das Anlegen und Entfernen von Körpern (die Zellen-Slices, `cellSliceBodies`, `cellUnloadBodies`) bucht genauso.

**Was damit nicht begrenzt ist:** eine einzelne sehr teure Abfragewolke oder ein sehr großer Zellen-Slice verlängert den Schritt, in
dem sie auftreten, weiterhin. Das Budget verteilt die *Folgeschritte* um, es bricht keinen laufenden Aufruf ab. Ein Aufschub von
Abfragen oder des Körper-Entfernens beim Entladen einer Zelle wäre der nächste Schritt (Abschnitt 9).

## 5. Kontakt-Events: Obergrenze und Reihenfolge

Der Listener sammelt Enter/Exit und Overlap-Enter/-Exit in vier Warteschlangen. Ein Abruf gibt sie sortiert nach Entity-Paar
heraus (Thema 153). Mit `eventsPerPoll = N`:

* ein Abruf gibt höchstens N Events der Liste heraus; der Rest bleibt vorn in der Schlange und ist beim nächsten Abruf als
  Erster dran (nur was seitdem dazukam, wird sortiert), nichts wird verworfen oder doppelt ausgegeben;
* **ein Exit geht nie vor einem noch wartenden Enter heraus**: solange die Enter-Schlange einen Rest hat, gibt der Exit-Abruf
  nichts zurück (ebenso Overlap). Ein Skript sieht also nie "Kontakt beendet" vor "Kontakt begonnen". Der Preis: Exits warten auf
  die Enters, solange ein Rückstau besteht;
* `removeEntity` entfernt wartende Events der Entity wie vorher und führt die Zählung der übernommenen Events mit;
* 0 bedeutet alles, dann ist der Abruf wie vorher ("swap und sortieren").

Zählen statt Zeit, weil das Puffern billig ist und das Verbrauchen teuer (ein Skript je Event) und weil eine Zahl auf jeder
Maschine gleich wirkt. Ein dauerhafter Überschuss (mehr als N Events je Frame, immer) lässt den Rückstau wachsen; er steht als
`eventsBacklog` im Profiler.

## 6. Determinismus

Das Budget ist **wandzeitabhängig**, solange es greift: welche Charaktere in welchem Schritt dran sind, hängt von der Maschine
ab. Es greift erst über 32 Charakteren und nur, wenn deren Update zusammen mit Abfragen und Körperarbeit das Budget
übersteigt; darunter und mit `budgetMs = 0` bleibt alles bitgleich zu vorher. Für einen Server, der Charakter-Bewegung
bitgenau braucht, ist `HE_PHYSICS_BUDGET_MS=0` (oder `"budgetMs": 0`) der Schalter. Die Event-Obergrenze ist eine Zahl und
selbst deterministisch. Die Simulation der Körper (`Update`) ist unberührt.

## 7. Profiler

Window, Performance Profiler, Tab Overview, Abschnitt "Physics budget" (nur live, ein geladener Capture trägt es nicht):

| Feld | Bedeutung |
|---|---|
| budget / step, used | das Limit; was der letzte Schritt an gebuchter Arbeit verbraucht hat (Abfragen + Körper + Charaktere) |
| characters (ms), queries + bodies | Aufteilung des Verbrauchs |
| characters, stepped, put off | Charaktere gesamt, gesteppt, aufgeschoben im letzten Schritt |
| longest wait (steps) | längste Wartezeit unter den Charakteren, in Schritten |
| queries | Abfragen seit dem vorigen Schritt |
| events queued | Events, die hinter der Obergrenze warten |
| steps over budget, characters put off (total) | kumuliert seit Start des Schritts mit Budget |

Die Zahlen kommen aus `PhysicsWorld::lastBudgetStats()`, einem Prozess-Schnappschuss des zuletzt gesteppten Spiel-/Editor-Welt (das
Panel hält keine Welt). Mehrere Welten im Prozess (nur Tests) überschreiben sich.

## 8. Tests und Messungen (ehrlich)

`tests/test_physics_budget.cpp` (siehe unten die Ergebnisse im Abschnitt "Ausgeführt"):

* **Regeln** (rein): `resolveFrameMs` (Projekt, Umgebung gewinnt, 0 = aus, Unsinn = aus, Obergrenze), `frameMsFromEnv`,
  Schuld und Obergrenze der Schuld; Standardwerte der Einstellung gleich denen der Welt; JSON-Rundlauf, Klammerung,
  Gleichheit, ein altes Projekt behält 4 ms.
* **Charaktere, Negativkontrolle:** ohne Budget werden alle 64 in jedem Schritt gesteppt und legen alle dieselbe Strecke zurück.
* **Charaktere, Überlauf:** mit einem Budget, das schon der erste Charakter aufbraucht, werden genau 32 gesteppt und 32
  aufgeschoben (`charactersDeferred`, `deferredCharacterSteps`, `overBudgetSteps`); nach sechs Schritten hat sich jeder bewegt,
  keiner weiter als die Referenz ohne Budget, keiner mehr als einen Schritt dahinter, und einige liegen wirklich zurück (der Test
  ist also nicht leer).
* **Charaktere, Nachholen / Reihumbedienung:** 96 Charaktere, in jedem Schritt rücken genau 32 vor, in neun Schritten bleibt keiner
  stehen, die längste Wartezeit bleibt unter der Obergrenze.
* **Kleine Szene:** zehn Charaktere werden bei winzigem Budget nie aufgeschoben.
* **Entfernen** eines aufgeschobenen Charakters: der Rest läuft weiter.
* **Umgebung:** `HE_PHYSICS_BUDGET_MS` gewinnt, `0` schaltet aus.
* **Abfragen:** 102 Abfragen werden gezählt und gebucht, der nächste Schritt hat 0 (Negativkontrolle: ein Schritt ohne Abfrage meldet 0);
  80 ms Strahlen vor einem Schritt lassen von 64 Charakteren genau die 32 garantierten stehen, der Folgeschritt holt die übrigen
  nach; ohne Abfragen passen alle 64 ins Budget.
* **Events:** ohne Obergrenze geht alles auf einmal hinaus, nichts überholt (Kontrolle: Batch größer 16). Mit Obergrenze 16: nie mehr
  als 16 je Liste und Abruf, ein Rückstau tritt auf, dieselben Events wie ohne Obergrenze (nichts verloren, nichts doppelt), kein
  Exit vor dem Enter seines Kontakts. Entfernen einer Entity mit wartenden Events: keines nennt sie danach.

**Ausgeführt** (macOS, Release, shaderc ON, Stand dieses Commits mit eingemergtem `origin/release/0.7.0`, 7c64a632):

* Voller Bau: `he_tests` (396 Schritte) und danach `all` (673 Schritte, mit dem Link von `HorizonEditor`, also ProfilerPanel,
  ProjectSettingsPanel, EditorHelp und EditorApplication): fehlerfrei, kein `FAILED`/`error:` im Log.
* Die 15 neuen Testfälle (1 693 Assertions): grün.
* **Negativkontrolle der Tests (Mutation):** mit ausgebautem Exit-Rückhalt (`take(..., false)` für beide Exit-Listen) und ohne
  Schuld-`dt` (`cdt = dt`) wurden drei Fälle rot: "a cap queues the rest in order and loses none" (`exitOvertookEnter`), "a crowd past
  the budget is put off…" (Lag größer als ein Schritt, 160 Assertions) und "the ones that waited go first…". Danach wieder hergestellt
  und neu gebaut, die Fälle sind wieder grün.
* Voller `ctest -j4 --timeout 1500`, 261 Tests: **100 % bestanden** (296 s). Übersprungen nur `runtime_size_app_advanced` und
  `runtime_size_app_basic` (kein Deploy-Ordner `AppAdvanced`/`AppBasic`, wie in Schritt 2).
* `scripts/editor_help_audit.py --check` läuft im `ctest` mit (neues Feld "Physics budget" hat seinen Hilfe-Eintrag).

**Nicht gemessen:** die Wirkung auf die Frame-Zeit in einer Lastszene mit tausenden Charakteren (die Messskripte
`scripts/perf/physics_load_*.py` setzen Charaktere ohne Laufskript; das Budget greift bei ihnen erst ab 32, und das Ziel ist
Gleichmäßigkeit, nicht Gesamtzeit); die Wirkung von Streaming plus Physik im selben Frame; Windows und Linux (nur CI). Der Aufwand
des Budgets bei ausgeschaltetem Budget ist ein Atomic-Austausch und eine `steady_clock`-Ablesung je Abfrage und Körperaufruf plus eine
Ablesung je Schritt; er wurde nicht gegen Rauschen aufgelöst.

## 9. Abgeschnitten und nächste Schritte

* **Kein Frame-Budget**, sondern Schritt-Budget (Abschnitt 2). Ein `beginFrame()` am Spielschleifen-Anfang würde die fünf
  nachgeholten Schritte eines Frames teilen.
* **Abfragen werden nicht aufgeschoben.** Das ginge nur mit einer asynchronen Abfrage-API; bis dahin sind sie gebucht und verkleinern
  den Anteil der Charaktere.
* **Das Entladen einer Zelle** (`cellUnloadBodies`) entfernt die Körper einer Zelle in einem Aufruf. Aufschieben ginge über den Reap in
  `step()` (Körper einer ungültigen Entity werden dort schon entfernt), müsste aber die Gelenke und `removeEntityTree`-Rückgabewerte
  mitnehmen; das wurde in der Zeit nicht gemacht. Das Anlegen ist schon in Slices geteilt (Streamer).
* **Rückschreiben und Reap** der Körper (der Hauptposten des seriellen Anteils nach Schritt 2) stehen nicht im Budget.
* **Kein Profiler-Scope in `step`** für Update/Rückschreiben/Charaktere; die neuen Zahlen ersetzen ihn nur für die Budgetarbeit.

## 10. Wasser (Thema 179, Schritt 2)

Auftrieb, Dämpfung und Strömung für Körper im Wasser (`PhysicsWorld::applyWater`, Konzept und Messung in `docs/water-physics-plan.md`
Abschnitt 13) ist neue Arbeit **vor** `Update()`, im selben Fenster wie `applyRegionHold`: auf dem Schritt-Thread, bevor Jolts Worker in einen
Körper greifen, durch Jolts eigene Sperren (`BodyLockWrite`). Außerhalb von `step()` ist sie nicht erreichbar (private Funktion).

* **Kosten wachsen mit den Körpern über der Landschaft unterhalb ihrer Wasserfläche, nicht mit der Szene.** Je Landschaft mit Wasser eine
  Broadphase-Abfrage (nur die bewegliche Ebene) über das Rechteck der nassen Zellen. Ohne Wasser in der Szene: eine Ansicht über die
  `TerrainComponent`s und `empty()`; die Zeit dafür wird **nicht** gebucht, das Budget rechnet wie vorher.
* **Obergrenze per Zähler:** `PhysicsBudgetConfig::waterBodiesPerStep` (Standard 256, 0 = alle). Wer drüber liegt, wartet und bekommt im nächsten
  Schritt `dt + Schuld` (`PhysicsBudgetRules::carryOwed/steppedDt`, höchstens `kMaxOwedSteps`): der Auftriebsimpuls ist linear in `dt`, ein
  Körper sinkt und schwimmt also gleich weit, nur später. Reihenfolge: größte Schuld zuerst, dann kleinste Entity-Id (nie Hash- oder
  Broadphase-Reihenfolge). Getestet mit 300 Körpern: bei Obergrenze 100 sind alle drei Schritte später einmal bedient.
* **Gebucht:** `PhysicsBudgetStats::buoyancyMs` zählt in `chargedMs` und kommt vom Anteil der Charaktere ab (die ersten 32 bleiben garantiert);
  `wetBodies` (bedient und eingetaucht) und `buoyancyDeferred` stehen daneben. Profiler-Zeilen und ein Projekt-JSON-Schlüssel dafür fehlen noch.
* **Gemessen:** 256 Quader 0,28 bis 0,38 ms je Schritt (1,1 bis 1,5 µs je Körper, Release, Mac); Konvexhüllen nicht.
* **Determinismus:** die Entscheidung, welche Körper bedient werden, hängt an einer Zahl und an Entity-Ids, nicht an Zeit oder Maschine. Der Auftrieb
  macht damit keinen Starrkörper wandzeitabhängig; die wandzeitabhängige Aufschubentscheidung der Charaktere bleibt, was sie war (`HE_PHYSICS_BUDGET_MS=0`).
  `HE_PHYSICS_WATER=0` schaltet das Wasser für Vergleichsläufe aus.
