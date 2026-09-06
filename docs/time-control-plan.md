# Time Control: Zeitskala, Pause, Zeitlupe, Hit-Stop

Umbauplan für eine zentrale Zeitskala durch Gameplay-Systeme und Physik-Accumulator.

---

## Stand

Das Fundament steht bereits. `HE::api::time` trägt seit längerem eine skalierte
Uhr, beide Anwendungsschleifen führen sie, und die Trennung „skalierte Spielzeit
gegen rohe Frame-Zeit" ist an jeder Aufrufstelle schon bewusst getroffen und
auskommentiert. Was fehlt, ist nicht die Skala, sondern **wem sie gehört**: es
gibt genau einen Float und keinen Besitzer, also kann jede Wirkung jede andere
überschreiben.

Dieses Dokument kartiert den Ist-Zustand, benennt die Lücken und entwirft die
Ergänzung. Es ändert keinen Code.

---

## 1. Ausgangslage: wo dt heute entsteht

Genau eine Stelle misst die Zeit. `Application::Run` nimmt `SDL_GetTicksNS`,
bildet `dt` und reicht es an `OnRender(dt)` weiter
(`src/HE_Core/src/Application/Application.cpp:216-292`). Die beiden
Anwendungen — `GameApplication` und `EditorApplication` — machen daraus je eine
skalierte und eine rohe Zeit und verteilen sie.

```
SDL_GetTicksNS  →  dt (roh, ungeklammert)
                     ├─→ HE::api::time::advance(dt)      →  gameDt = deltaTime()  (= dt * scale)
                     ├─→ Widgets, debug::collect, Real-Time-Delays, Editor-Kamera
                     └─→ GameLoop::tick(world, logic, dt)  ← eigener Accumulator, UNSKALIERT
```

### 1.1 Die Uhr selbst

`src/HE_Scene/src/EngineApi.cpp:2004-2027`, Namespace `HE::api::time`. Ein
prozessglobaler `Clock` mit `delta`, `unscaledDelta`, `elapsed`, `frame`,
`scale`. `advance(dt)` multipliziert einmal, addiert das skalierte Delta auf
`elapsed` und zählt `frame` hoch.

| Funktion | Bedeutung |
|---|---|
| `advance(dt)` | App-Haken, einmal pro gerendertem Frame, ROHES dt |
| `reset()` | auf Null, Skala zurück auf 1 |
| `deltaTime()` | letztes Delta, **skaliert** |
| `unscaledDeltaTime()` | letztes Delta, wie die App es gemessen hat |
| `elapsed()` | Summe der skalierten Deltas seit `reset()` |
| `frameCount()` | Frames seit `reset()` |
| `setTimeScale(s)` / `timeScale()` | auf `[0, kMaxTimeScale]` geklammert, `kMaxTimeScale = 5` |
| `isPaused()` | `timeScale() <= 0`, ohne Registry-Zeile |

### 1.2 Wer welche Uhr bekommt

| Verbraucher | Ort | Uhr heute | Absicht |
|---|---|---|---|
| `time::advance` | `GameApplication.cpp:1339` | roh, jeden Frame | richtig |
| `time::advance` | `EditorApplication.cpp:1891` | `simulating ? dt : 0` | richtig, Editor-Pause |
| Skripte (`updateScripts`) | `GameApplication.cpp:1416` | `gameDt` | Spielzeit |
| Physik-Accumulator | `GameApplication.cpp:1424-1438` | `gameDt` | Spielzeit |
| Physik-Accumulator (PIE) | `EditorApplication.cpp:2618-2623` | `gameDt` | Spielzeit |
| Kamera-Rig | `GameApplication.cpp:1459`, `EditorApplication.cpp:2630` | `gameDt` | Spielzeit |
| `PlayerHost::tick` | `GameApplication.cpp:1483` | `gameDt` | Spielzeit |
| `EntityHost::tick` | `GameApplication.cpp:1485` | `gameDt` | Spielzeit |
| `SceneSystems::tickWorld` | `GameApplication.cpp:1520`, `EditorApplication.cpp:2608` | `gameDt` | Wetter, Partikel, LOD, Navigation |
| `SceneSystems::tickAnimation` | `GameApplication.cpp:1524` | `gameDt` | Spielzeit |
| Tag-Nacht-Zyklus | `GameApplication.cpp:1615` (`makeEnvironmentSettings`) | `gameDt` | Weltzustand, friert mit ein |
| `WidgetManager::tick` | `GameApplication.cpp:1468` | **roh** | bewusst: ein eingefrorenes Pausenmenü könnte sich nie selbst aufheben |
| `debug::collect` | `GameApplication.cpp:1396` | **roh** | bewusst: befristete Debug-Primitiven müssten sonst nie ablaufen |
| HorizonCode-Latent-Flow | `GameApplication.cpp:1493` (`runtime().update(gameDt, deltaTime)`) | **beide** | Delay zählt Spielsekunden, mit Real-Time-Pin echte |
| Editor-Kamera (Edit-Modus) | `EditorApplication.cpp:2045` (`gameDt = dt`, wenn nicht `m_isPlaying`) | **roh** | bewusst Echtzeit |
| `GameLoop::tick` → `IGameLogic::onUpdate` | `Application.cpp:292` | **roh, eigener Accumulator** | **Lücke, siehe 3.f** |

Die Trennlinie ist also bereits gezogen und stimmt: alles, was *das Spiel ist*,
läuft auf `gameDt`; UI, Debug-Primitiven und die Editor-Kamera laufen bewusst in
Echtzeit. Der Kommentar in `GameApplication.cpp:1379-1387` spricht das explizit
aus und bleibt gültig.

### 1.3 Die Physik-Accumulatoren

Zwei Kopien, eine pro Anwendung, beide auf `PhysicsWorld::kFixedDt` (1/60), das
absichtlich in `PhysicsWorld` wohnt und nicht je Anwendung
(`PhysicsWorld.h:321-325`). Der Editor aliasiert es als
`EditorApplication.h:597 kPhysicsFixedDt = PhysicsWorld::kFixedDt` — kein Drift.

Unterschied: die Spiel-Schleife deckelt die Schrittzahl und lässt den Deckel
**mit der Zeitskala mitwachsen** (`maxSteps = 5 * ceil(timeScale())`,
`GameApplication.cpp:1431`), damit Vorlauf bei Skala 5 nicht still zu Zeitlupe
verkommt. Die Editor-Schleife hat gar keinen Deckel
(`EditorApplication.cpp:2619`).

### 1.4 Editor-Pause

Der Editor pausiert über den Weg *„advance mit Null füttern"*
(`EditorApplication.cpp:1885-1891`) und schreibt bewusst **nie**
`time::setTimeScale`. Der Kommentar an `EditorApplication.h:712` sagt warum:
diese Stellschraube gehört dem Spiel, ein Editor-Knopf, der sie mitbenutzt,
würde mit einer Zeitlupe des Spiels streiten. Ein Einzelschritt-Frame
(`m_stepFrame`) lässt genau einen Tick durch. Das bleibt so.

---

## 2. Was schon steht und nicht angefasst wird

* Die sechs Getter/Setter oben, in **allen vier Frontends** verdrahtet: C++,
  Registry-Zeilen `time.deltaTime`, `time.unscaledDeltaTime`, `time.elapsed`,
  `time.frameCount`, `time.setTimeScale`, `time.timeScale`
  (`EngineApi.cpp:2529`), Lua/Python über `ScriptContext`, HorizonCode-Knoten
  mit Handbuch-Einträgen (`HcNodeDocs.cpp:466-482`).
* Die Klammerung auf `[0, 5]` liegt in `setTimeScale` selbst, also erbt jedes
  Frontend dieselben Grenzen.
* Der Physik-Deckel, der mit der Skala mitwächst.
* `PlayerHost` schweigt Eingaben während einer Pause, lässt aber Aktionen mit
  `runWhilePaused` durch und tickt das Mapping weiter, damit eine über die
  Pause gehaltene Taste beim Fortsetzen nicht wie ein frischer Druck aussieht
  (`PlayerHost.cpp:195-225`).

**Diese Semantik ist durch Tests festgenagelt** und der Umbau muss sie
erhalten: `tests/test_engine_api.cpp:938-966` (Skala nach `reset()` wieder 1,
`setTimeScale(0)` → `timeScale() == 0`, Klammerung bei 99 → 5,
`unscaledDeltaTime` bleibt roh) und `tests/test_scripting_binding.cpp:211-233`
(dasselbe über Lua).

---

## 3. Lücken

**a) Ein Float, kein Besitzer.** Die Zeitlupe eines Treffer-Effekts endet und
setzt die Skala auf 1 — und hebt damit stillschweigend das Pausenmenü auf, das
jemand anders gesetzt hatte. Zwei gleichzeitige Wirkungen können sich nicht
schichten, die letzte gewinnt. Das ist die eigentliche Lücke; alles andere ist
Beiwerk.

**b) Kein Hit-Stop.** Ein Treffer-Einfrieren ist ein *reales* Zeitfenster
(80–120 ms), das den Zustand ringsum unberührt lässt und danach genau
dahin zurückkehrt, wo es herkam — auch wenn ringsum Zeitlupe läuft. Über den
einen Float ist das nicht ausdrückbar.

**c) Hit-Stop darf nicht `isPaused()` sein.** `PlayerHost.cpp:211` liest
`isPaused()` und **verwirft** in diesem Frame Eingabe-Ereignisse. Für ein
Pausenmenü ist das genau richtig; für einen 100-ms-Hit-Stop wäre es falsch —
der Spieler würde seinen Tastendruck im Einfrieren verlieren. „Pausiert" und
„eingefroren" müssen also unterscheidbare Zustände sein, auch wenn beide die
Spielzeit auf Null bringen.

**d) Fokusverlust pausiert nichts.** `SDL_EVENT_WINDOW_FOCUS_LOST` erreicht heute
nur `GameInstanceHost::setWindowFocus`, das ein HorizonCode-Ereignis
`OnWindowFocusChanged` feuert (`GameApplication.cpp:1238`,
`GameInstanceHost.cpp:39-45`). Engine-seitig passiert nichts: wer nicht selbst
einen Graphen dafür baut, spielt beim Alt-Tab weiter.

**e) Kein `unscaledElapsed`.** `elapsed()` summiert die skalierten Deltas, ein
Pausenmenü kann also keine echte Sitzungszeit anzeigen. `unscaledDeltaTime()`
gibt es, das Integral dazu nicht.

**f) `GameLoop::tick` ignoriert die Zeitskala vollständig.**
`Application.cpp:292` reicht das **rohe** dt in einen zweiten, eigenen
Fixed-Step-Accumulator, der `IGameLogic::onUpdate` treibt — also das
hot-geladene C++-Spiellogik-Modul. Beide Anwendungen setzen `Application::m_world`
(`GameApplication.cpp:622`, `EditorApplication.cpp:1390`), diese Schleife läuft
daher immer. Folgen: C++-Spiellogik läuft in Zeitlupe und Pause ungebremst
weiter, und im Editor tickt sie sogar im **Edit-Modus**, weil hier kein
`m_isPlaying`-Gatter liegt. Von allen Lücken ist das die, die am ehesten wie ein
Fehler aussieht.

**g) Kein Klammer nach oben auf dem rohen dt.** `Application.cpp:218` nimmt die
gemessene Spanne, wie sie ist. Ein 30-Sekunden-Frame nach einem Alt-Tab oder
einem Haltepunkt geht ungebremst in `advance()` und damit in die Physik. Der
Physik-Deckel fängt die Folgeschäden ab, indem er den Rest verwirft
(`GameApplication.cpp:1438`) — der Rest der Welt bekommt den Sprung aber voll.
Das ist der billigste Teil der Fokusverlust-Pause: die erste Welle nach dem
Zurückkommen.

**h) Das gepackte Spiel ruft `time::reset()` nie.** Nur der Editor tut es beim
Play-Start (`EditorApplication.cpp:5988`). Im gepackten Spiel überleben Skala
und `elapsed` daher jeden Szenenwechsel. Für `elapsed` ist das vertretbar
(Sitzungszeit), für die **Skala** ist es eine Falle: wer in Zeitlupe eine neue
Szene lädt, landet in Zeitlupe. Zu entscheiden, nicht stillschweigend zu ändern
— siehe 4.5.

**i) `frameCount()` zählt durch die Editor-Pause weiter**, weil `advance(0)`
trotzdem `++frame` macht. Notiert als Eigenart, nicht als Fehler: `frameCount`
zählt gerenderte Frames, und das stimmt dann auch.

---

## 4. Entwurf

### 4.1 Drei Kanäle statt eines Floats

Die Zeitskala wird **komponiert**, nicht überschrieben. Der `Clock` bekommt drei
unabhängige Quellen, und `advance()` bildet daraus einmal das effektive Delta:

| Kanal | Wer setzt ihn | Wirkung |
|---|---|---|
| **Authored Scale** | `setTimeScale()`, unverändert | Zeitlupe/Vorlauf, `[0, 5]` |
| **Pause-Gründe** | `pause(reason)` / `resume(reason)` | pausiert, solange irgendein Grund gesetzt ist |
| **Hit-Stop** | `hitStop(sekunden)` | eingefroren, bis der Echtzeit-Countdown abläuft |

```
effectiveScale = (anyPauseReason || hitStopRemaining > 0) ? 0 : authoredScale
delta          = unscaledDelta * effectiveScale
```

Der Hit-Stop-Countdown wird **aus `unscaledDelta` innerhalb von `advance()`**
heruntergezählt, bevor das Delta gebildet wird — sonst könnte er sich selbst
nicht mehr beenden, denn er hält die Spielzeit ja an. Aus derselben Zerlegung
folgt umsonst, dass ein Hit-Stop mitten in einer Zeitlupe danach wieder in
genau diese Zeitlupe zurückkehrt: er hat sie nie angefasst.

**Pause-Gründe** als kleiner benannter Satz (Bitset), nicht als Zähler, damit
doppeltes `pause(Menu)` idempotent bleibt und `resume` nicht die falsche Zahl
abzieht:

```
enum class PauseReason : uint32_t { Menu = 1<<0, FocusLost = 1<<1, Debug = 1<<2, Script = 1<<3 };
```

`Script` ist der Grund, den Skript-Frontends ohne Argument benutzen — damit ein
Skript-Pause den Fokusverlust-Grund nicht mit aufhebt.

### 4.2 Welche Frage welcher Getter beantwortet

Hier liegt die Entscheidung, und sie folgt aus 3.c:

| Getter | Antwort | Begründung |
|---|---|---|
| `timeScale()` | die **authored** Skala | Bestandsschutz: `tests/test_engine_api.cpp:941-966` prüft genau das, und ein Skript, das seine eigene Zeitlupe zurücknehmen will, muss sie ablesen können |
| `isPaused()` | „ein Pausengrund liegt an" | **nicht** `timeScale() <= 0` — sonst würde Hit-Stop hier auftauchen und `PlayerHost` würde Eingaben schlucken |
| `isFrozen()` | „Hit-Stop läuft" | neu |
| `effectiveScale()` | die tatsächlich wirksame Skala | neu, für HUDs und Diagnose |
| `deltaTime()` | `unscaledDelta * effectiveScale` | unverändert in der Bedeutung |

Damit ändert sich `isPaused()` von *abgeleitet* zu *eigenständig*. Der eine
Fall, in dem das sichtbar wird: `setTimeScale(0)` allein macht `isPaused()`
künftig **nicht** mehr wahr. Das ist gewollt — „Skala Null" ist eine
Zeitlupe bis zum Stillstand, „pausiert" ist eine Aussage über den Spielzustand
— aber es ist eine Verhaltensänderung, und der Test bei
`test_engine_api.cpp:960` muss daraufhin gelesen werden. Wenn Schritt 2 das
nicht sauber trennen kann, ist die Rückfallposition: `isPaused()` bleibt
`effectiveScale() <= 0 && !isFrozen()`.

### 4.3 `reset()`

Räumt alle drei Kanäle: Skala auf 1, Pausengründe leer, Hit-Stop auf 0. Ein
Play-Start beginnt nie pausiert, nie eingefroren, nie in Zeitlupe — so wie
heute schon.

### 4.4 Fokusverlust-Pause

Ein Projekt-Schalter, kein Automatismus, und **im Editor standardmäßig aus**:
das Editorfenster verliert den Fokus dauernd, und eine PIE-Sitzung, die dabei
jedes Mal einfriert, wäre unbenutzbar.

* Konfigurationsschlüssel in `config.json`, gleiches Muster wie `GpuParticles`
  (`GlobalState::getCustomConfigBool("PauseOnFocusLoss", true)`).
* `GameApplication::ProcessEvent` schiebt bei `SDL_EVENT_WINDOW_FOCUS_LOST`
  `pause(FocusLost)` und nimmt es bei `FOCUS_GAINED` zurück
  (die Stelle existiert schon: `GameApplication.cpp:1238-1239`).
* Das bestehende `OnWindowFocusChanged`-Ereignis bleibt **zusätzlich** bestehen;
  wer schon einen Graphen darauf gebaut hat, verliert nichts.
* Dazu die Klammer aus 3.g auf dem rohen dt (Größenordnung 0.25 s), damit auch
  der Frame *nach* dem Zurückkommen niemanden durch eine Wand schiebt.

### 4.5 Zu entscheidende Punkte

Diese zwei gehen nicht ohne eine Ansage durch, sie sind Schritt-2-Fragen:

1. **`time::reset()` beim Szenenwechsel im gepackten Spiel** (3.h) — Vorschlag:
   nur die **Skala und die Pausengründe** beim Laden einer Szene zurücksetzen,
   `elapsed`/`frameCount` weiterlaufen lassen. Beides zugleich wegzuwerfen wäre
   für eine Sitzungsuhr falsch.
2. **`GameLoop::tick` auf die Spielzeit umstellen** (3.f) — Vorschlag: das rohe
   dt bleibt der Takt der Schleife, aber der Accumulator wird mit
   `HE::api::time::deltaTime()` gefüttert und im Editor an `m_isPlaying`
   gekoppelt. Berührt `HE_Core`, also der einzige Teil des Entwurfs, der außerhalb
   von `HE_Scene`/den beiden Anwendungen liegt.

---

## 5. API-Oberfläche

Neu in `HE::api::time` (`EngineApi.h`), und damit in allen vier Frontends:

| Name | Signatur | Registry-Zeile |
|---|---|---|
| `pause` | `void pause(PauseReason = Script)` | `time.pause` |
| `resume` | `void resume(PauseReason = Script)` | `time.resume` |
| `isPaused` | `bool isPaused()` (bekommt eine Zeile, hatte bisher keine) | `time.isPaused` |
| `hitStop` | `void hitStop(float seconds)` | `time.hitStop` |
| `isFrozen` | `bool isFrozen()` | `time.isFrozen` |
| `effectiveScale` | `float effectiveScale()` | `time.effectiveScale` |
| `unscaledElapsed` | `float unscaledElapsed()` | `time.unscaledElapsed` |

Alle sind entweder reine Getter (pure Daten-Knoten in HorizonCode, konstant
innerhalb eines Frames) oder Zustandsänderungen (Exec-Knoten) — dieselbe
Einteilung, die `time.setTimeScale` heute schon hat.

**Pflicht, nicht Kür:** jede neue Registry-Zeile braucht einen Eintrag in
`HcNodeDocs.cpp` und einen Handbuch-Scope. Die Handbuch-Deckung ist ein ctest —
ein Implementierungsschritt ohne diese Einträge macht die CI rot, nicht bloß die
Doku lückenhaft.

---

## 6. Tests für die Schritte 2 und 3

* Hit-Stop während einer Zeitlupe: nach Ablauf steht wieder **genau** die
  Zeitlupe da, nicht Skala 1.
* Zwei Pausengründe: einer wird zurückgenommen, es bleibt pausiert.
* `pause(Menu)` doppelt, `resume(Menu)` einmal → nicht mehr pausiert
  (Idempotenz des Bitsets).
* Hit-Stop macht `isPaused()` **nicht** wahr — also schluckt `PlayerHost` keine
  Eingaben (der Test aus 3.c, direkt gegen `PlayerHost::tick`).
* `unscaledElapsed` läuft durch Pause und Hit-Stop weiter, `elapsed` nicht.
* Fokusverlust pausiert nur bei gesetztem Schalter.
* Editor-Accumulator: Deckel wie im Spiel, nach einem 5-Sekunden-Frame bleibt
  die PIE-Physik nicht in Aufholschritten hängen.
* Bestandsschutz: die bestehenden Fälle in `test_engine_api.cpp:938-966` und
  `test_scripting_binding.cpp:211-233` laufen unverändert weiter.

---

## 7. Bewusst außen vor

Damit niemand sie für vergessen hält:

* **Tonhöhe unter Zeitlupe.** Audio bei Skala 0.25 tiefer abzuspielen ist eine
  Frage an die Audio-Schicht, nicht an die Uhr.
* **`GameReplication` und die Zeitskala.** `GameReplication.cpp:78` klammert das
  Eingabe-Delta auf `kMaxInputDeltaTime`. Eine skalierte Uhr auf beiden Seiten
  einer Netzwerkverbindung ist ein eigenes Thema — Zeitlupe ist im
  Mehrspieler-Fall ohnehin kein lokaler Knopf.
* **Motion Blur / Renderer.** Dieser Umbau fasst nichts am Rendering an,
  ausdrücklich.
* **Zeitskala pro Entity oder pro Welt.** Eine globale Uhr, wie heute. Lokale
  Zeitblasen wären ein anderer Entwurf.

---

## 8. Umgesetzt

Nachgetragen, als das Vorhaben durch war. Der Entwurf oben bleibt stehen, wie er
geschrieben wurde — was davon abweicht, steht hier.

### Schritt 2 (`9693e77b`) — die Uhr

Wie in §4.1 bis §4.3 entworfen, ohne Abweichung. Die Rückfallposition aus §4.2
(`isPaused()` bleibt abgeleitet) wurde **nicht** gebraucht: `isPaused()` ist eine
eigene Aussage über die Pausengründe, und `test_engine_api.cpp:938-966` läuft
unverändert weiter. Dazu, über den Entwurf hinaus: `HE::advanceFixedSteps` als
die eine Regel hinter beiden Physik-Accumulatoren (§1.3 hatte sie
auseinanderlaufen sehen), und `Application::GameLogicDeltaTime` als der Haken,
mit dem §4.5.2 gelöst wurde, ohne dass `HE_Core` die Uhr in `HE_Scene` sieht.

### Schritt 3 — der Weg hinein

* **Registry.** Die sieben Zeilen aus §5, dazu die Anzeigenamen und die Einträge
  in `HcNodeDocs.cpp`. Lua, Python und HorizonCode laufen alle drei über die
  Registry, es gab also nichts, was pro Frontend zu verdrahten gewesen wäre.
  Eine Abweichung von §5: **`time.pause` und `time.resume` nehmen keinen
  Grund.** `PauseReason` bleibt ein C++-Typ, jedes Frontend teilt sich den
  `Script`-Kanal. Sonst könnte ein Skript `FocusLost` benennen und damit ein
  Spiel entpausieren, dessen Fenster noch im Hintergrund liegt — der Schalter
  aus §4.4 wäre dann eine Empfehlung. Festgenagelt in
  `tests/test_time_control.cpp`, „a script's resume cannot lift the window's
  pause".
* **Editor-Anzeige.** Ein Lesefeld in der Viewport-Leiste, gleich neben Play:
  Skala, `Paused` oder `Freeze`, bernstein sobald die Uhr des **Spiels** nicht
  normal läuft. Bewusst getrennt von der Bandfärbung, die die Pause des
  **Editors** meldet — genau diese zwei Ursachen waren an einer stehenden
  Vorschau nicht auseinanderzuhalten. Die Zelle wird immer in ihrer breitesten
  Beschriftung vermessen, sonst rutscht der Play-Knopf zur Seite, wenn die Szene
  in Zeitlupe geht.
* **Fokusverlust-Pause** (§4.4) wie entworfen: `PauseOnFocusLoss` in der
  `config.json`, standardmäßig an, nur vom gepackten Spiel gelesen.
  `OnWindowFocusChanged` bleibt daneben bestehen. `FOCUS_GAINED` nimmt den Grund
  **bedingungslos** zurück, auch bei ausgeschaltetem Schalter — ein Grund, den
  niemand gesetzt hat, zurückzunehmen kostet nichts, und die Alternative wäre
  ein Spiel, das für immer pausiert bleibt, wenn der Schalter zur Laufzeit
  umfällt.
* **Klammer auf dem rohen dt** (§3.g): `Application::kMaxFrameSeconds`, 0.25 s,
  dieselbe Zahl wie die Hitch-Schwelle. Zwei Zahlen statt einer, das ist der
  Punkt: `measuredDt` ist, wie lange der Frame gedauert hat — davon leben der
  Hitch-Melder und der Profiler, und eine Klammer darauf würde genau die Stockung
  verstecken, die sie melden sollen. `dt` ist, wie weit die Welt bewegt wird.

### Offen

* Die Website-Doku (`scripting-api#api` im Geschwister-Repo `Website`) kennt die
  neuen Aufrufe noch nicht. `EditorDeps/Docs/he-docs.json` ist daraus generiert
  und wird nicht von Hand angefasst.
* Die Fokusverlust-Pause ist nur über den Konfigurationsschalter getestet, nicht
  über ein echtes SDL-Fokusereignis — das gepackte Spiel läuft in keinem Test.
