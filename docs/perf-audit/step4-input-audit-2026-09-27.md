# Perf-Audit Schritt 4: Input-Audit (2026-09-27)

Thema 99, Zweig `claude/performance-audit-editor-runtime-auf-m5-macbook-landscape-hi`.
Frage: Trägt die Eingabeverarbeitung zur Frame-Zeit bei, und wie groß ist die Latenz zwischen Eingabe und
sichtbarer Reaktion?

## Kurzfassung

1. **Die Eingabe kostet keine Frame-Zeit.** `PollEvents` (SDL-Pump, Cocoa-Eventschleife, Dispatch an
   ImGui und `Input`) liegt bei **p50 0,08 ms / p90 0,13 ms** pro Frame. Mit 16 bzw. 64 eingespeisten
   Mausbewegungen pro Frame steigt das um **höchstens 0,015 ms**, `OnRender` bleibt unverändert
   (p50 1,0 ms). Eine Positivkontrolle bestätigt, dass die Events wirklich verarbeitet wurden. Input ist
   **keine** Ursache für < 60 FPS.
2. **Die Latenz ist hoch, aber das liegt an der Pipeline, nicht am Eingabesystem.** Die Eingabe wird
   einmal am Frame-Anfang abgefragt. Danach wird der ganze Frame kodiert, und erst dann blockiert der
   Hauptthread in `[CAMetalLayer nextDrawable]`. Der Command Buffer wird erst nach diesem Warten committet.
   Die Eingabe ist deshalb schon **p50 10 ms (GPU frei) bzw. 20 ms (am Bildschirm unter Fremdlast)** alt,
   bevor die GPU überhaupt anfängt, davon **8–19 ms reines Warten**. Hergeleitet (nicht gemessen) ergibt
   das insgesamt etwa **50 ms (GPU frei) bis 90 ms (L2-Bedingungen)** von der Eingabe bis zum Photon.
3. **Empfehlung:** Die Wartestelle vor die Eingabeabfrage ziehen (Frames-in-flight-Semaphore oder
   `CAMetalDisplayLink`). Das spart die 8–19 ms Eingabealter, ohne die FPS zu ändern. Details in Abschnitt 5.

**Gemessen vs. hergeleitet:** Die CPU-Kosten (Abschnitt 2) und der Abstand Poll→Commit (Abschnitt 3,
Zeile b) sind gemessen. Die Photonen-Latenz insgesamt ist hergeleitet: Der Bildschirm des M5 ist seit
15:19 gesperrt (`CGSSessionScreenIsLocked=Yes`), und es gibt im ganzen Audit keinen Lauf am Bildschirm ohne
Fremdlast. Deshalb wurde kein `addPresentedHandler`-Messhaken gebaut, er hätte nichts Belastbares geliefert.

## 1. Der Eingabepfad

| Stelle | Was passiert | Art |
|---|---|---|
| `Application::Run` → `Window::PollEvents` (Application.cpp, Window.cpp:286) | `while (SDL_PollEvent)`, jedes Event → Callback | **Event-basiert**, einmal pro Frame, am Anfang |
| Callback (Application.cpp:160) | `Input::ProcessMouseEvent`, `ProcessGamepadEvent`, dann `OnEvent` (Editor: `ImGui_ImplSDL3_ProcessEvent`), sonst `Input::ProcessEvent` | pro Event O(1) |
| `Input::PollGamepads` (Input.cpp:152) | ohne Pad sofortiger Ausstieg; mit Pad 6 Achsen + 26 Tasten über SDL-Getter (gecachter Zustand) | Polling, pro Frame |
| Editor/Scene: `SDL_GetKeyboardState`, `SDL_GetMouseState`, `SDL_GetRelativeMouseState` (EditorApplication.cpp:2295, EditorViewportNav.cpp, FlyCameraController.cpp:45) | Lesen von SDL-internem Zustand, der beim Pump aktualisiert wurde | Polling, Zeigerlesen |
| `Window::WaitForEvent` / `IdleWait` | nur im eventgesteuerten Modus; den nutzt nur der App-Modus des Spiels (GameApplication.cpp:653), der **Editor nicht** | im Audit nie aktiv |

ImGui: `io.ConfigInputTrickleEventQueue` steht auf dem Standard `true`. Ein Klick, dessen Down und Up im
selben Frame ankommen, wird auf zwei Frames verteilt (+1 Frame für das Up). Das ist Absicht und wirkt nur bei
sehr schnellen Klicks, reine Mausbewegungen werden in einem Frame verarbeitet.

## 2. CPU-Kosten der Eingabe (gemessen)

### 2a. Ohne Eingabe: alle vorhandenen Captures

In allen Captures aus Schritt 1–3 (über 50 Läufe, Landscape/Sky, vsync an/aus, detailed) liegt
`PollEvents` bei p50 0,064–0,098 ms und p90 0,10–0,13 ms (Tabelle in `baseline-2026-09-27.md`, Zeile
„PollEvents (Input)“). Im Time Profiler aus Schritt 3 (T1) macht der ganze Pump-Pfad
`Window::PollEvents → SDL_PumpEventsInternal → Cocoa_PumpEvents → -[NSApplication nextEventMatchingMask:…]`
43 von 1150 Hauptthread-Samples aus (3,7 %). Das ist fast ausschließlich die Cocoa-Eventschleife
(Mach-Messages an den WindowServer), die auch ohne Events einmal pro Frame läuft.

### 2b. Unter Eingabelast: neuer Injektor `HE_PERF_INPUT_EVENTS=N`

Alle Captures laufen ohne jemanden an der Maus. Deshalb gibt es jetzt einen Messhaken in
`Application::Run` (direkt vor `PollEvents`). Er schiebt N `SDL_EVENT_MOUSE_MOTION` pro Frame in die
SDL-Queue und fährt dabei langsam zeilenweise über das ganze Fenster (eine Zeile pro 240 Frames, 40 pt
Abstand). So ändert sich ImGuis Hover-Zustand wirklich, auch über Toolbar, Panels und Viewport. N = 16
entspricht einer 1000-Hz-Maus bei 60 FPS, N = 64 ist ein Stresstest. Ohne Variable ist der Haken aus
(einmal gecachtes `getenv`).

**Positivkontrolle:** Das Log meldet für Frame 100 `dispatched 64 events` (N = 64), `16` (N = 16) bzw.
`0` (N = 0), und beim Start `keyboard focus none`. Der Kontroll-Log kam erst mit dem zweiten Build dazu.
Er steht deshalb in `I64-run4`/`I0-run4` und in den drei verworfenen Lastläufen, nicht in run1/run2. Der
Injektor-Code ist in beiden Builds derselbe. Das SDL3-Backend von ImGui ersetzt die eingespeiste
Position also nicht durch den echten Cursor (das täte es nur mit Tastaturfokus und ohne überfahrenes Fenster).

Landscape-Szene, Editor Edit-Modus, vsync aus, 300 Warmup + 600 Frames, Release, Metal. Bildschirm gesperrt,
die FPS sind also nicht die am Bildschirm, die CPU-Scopes aber belastbar.

| Lauf | N | PollEvents p10 / p50 / p90 (ms) | OnRender p50 / p90 (ms) | Render p50 (ms) | NextDrawable p50 (ms) | FPS | GPU p50 (ms) |
|---|---|---|---|---|---|---|---|
| I0-run1 | 0 | 0,047 / 0,078 / 0,126 | 0,99 / 1,31 | 10,59 | 9,06 | 39,6 | 14,46 |
| I0-run2 | 0 | 0,049 / 0,085 / 0,128 | 1,02 / 1,35 | 10,03 | 8,66 | 40,0 | 15,10 |
| I16-run1 | 16 | 0,055 / 0,088 / 0,134 | 1,03 / 1,36 | 9,64 | 8,52 | 38,9 | 14,83 |
| I16-run2 | 16 | 0,054 / 0,086 / 0,133 | 1,01 / 1,32 | 10,28 | 8,95 | 40,8 | 14,46 |
| I64-run1 | 64 | 0,059 / 0,089 / 0,137 | 0,99 / 1,31 | 9,29 | 7,94 | 41,0 | 14,41 |
| I64-run4 | 64 | 0,065 / 0,095 / 0,147 | 1,01 / 1,32 | 9,46 | 8,28 | 41,3 | 14,68 |
| I0-run4 | 0 | 0,053 / 0,083 / 0,132 | 1,02 / 1,34 | 10,83 | 9,34 | 41,0 | 15,37 |

(run4 ist ein Paar direkt hintereinander bei Load 2,2, mit Positivkontrolle im Log.)

**Ergebnis:** 16 Events pro Frame kosten etwa **+0,005 ms** (p50), 64 Events **+0,007 bis +0,012 ms**
(p90 +0,015 ms), also rund 0,1–0,2 µs pro Event. Die Hover-Folgekosten in `OnRender` liegen unter der Streuung zwischen zwei
Läufen ohne Eingabe (0,99 vs. 1,02 ms). Beides zusammen ist unter **0,1 % des 16,7-ms-Budgets**.

**Verworfen:** Die Läufe `I64-run2`, `I0-run3` und `I16-run3` (liegen in `raw-step4/`) liefen, während
eine andere Instanz mit etwa acht clang-Prozessen kompilierte (Load 11). Dort springt `PollEvents` auf
p90 bis 5 ms und `OnRender` auf p90 bis 25 ms, **auch ohne Eingabe** (I0-run3). Das ist Verdrängung des
Hauptthreads (Schritt 3: E-Core-only im Stromsparmodus), keine Eingabekosten. Die Läufe sind nur als
Beleg dafür aufgehoben.

Die absoluten Zahlen (40 statt 69 FPS in Schritt 3, GPU p50 14,5 statt 11,3 ms) zeigen, dass der Rechner
auch während der sauberen Läufe nicht ganz ruhig war. Der Vergleich innerhalb der Reihe ist trotzdem
gültig: Alle sieben Läufe liegen in derselben Lastlage (FPS 38,9–41,3, GPU 14,4–15,4 ms).

**Nicht gemessen:** Tastatur-Events (gleicher Dispatch-Pfad, gleiche Größenordnung zu erwarten), ein
angeschlossener Gamepad (`PollGamepads` fällt ohne Pad sofort heraus), der Play-Modus (Spielkamera über
`SDL_GetRelativeMouseState`, ebenfalls reines Lesen).

## 3. Latenz Eingabe → sichtbare Reaktion

### Struktur

```
 Event kommt an ──► wartet in der SDL/Cocoa-Queue bis zum nächsten PollEvents      (a)
 PollEvents  ──► OnRender (ImGui, Szene-Tick, Kamera) ──► Render: Encode Szene …   (b)
             … ──► Metal::NextDrawable  ◄── blockiert, Eingabe ist schon verbaut
             … ──► Present-Pass ──► presentDrawable ──► commit
 GPU führt den Frame aus                                                           (c)
 nächster VBlank + WindowServer-Komposition ──► Photon                             (d)
```

Maßgeblich ist MetalRenderer.mm:16034/16079. Die ganze Szene (Shadow, Scene, Bloom, Tonemap, FXAA, UI)
steht in einem Command Buffer, der **erst nach** `[layer nextDrawable]` committet wird. Es gibt keine
Frames-in-flight-Semaphore, `nextDrawable` ist der einzige Gegendruck. Dieses Warten
(`maximumDrawableCount` 3, Schritt 2) fällt deshalb genau zwischen die Eingabeabfrage und den GPU-Start.
Solange der Hauptthread dort blockiert, pumpt er auch keine Events, neue Eingaben warten in der Queue (a).

### Werte

| Anteil | C2 (Schritt 3: GPU frei, Bildschirm gesperrt, vsync an, 69 FPS) | L2 (Schritt 1: am Bildschirm, vsync an, Fremdlast pid 72986, 44 FPS) | Quelle |
|---|---|---|---|
| (a) Warten bis zum Poll, Mittel ½ Frame | ≈ 5 ms (Frame p50 10,2–10,4) | ≈ 10 ms (Frame p50 19,2–21,2) | hergeleitet |
| (b) Poll → Commit = OnRender + Render | **≈ 10,3 ms** p50 (1,05 + 9,2–9,4), p90 ≈ 34 | **≈ 20 ms** p50 (1,0 + 18,3–20,2), p90 ≈ 52 | **gemessen** |
|   davon `Metal::NextDrawable` | 7,8–8,3 ms p50, p90 31–32 | 16,9–18,8 ms p50, p90 50–51 | **gemessen** |
| (c) GPU-Ausführung | 11,5–12,4 ms p50 | 32–33 ms p50 (durch Fremdlast überhöht) | gemessen (GPU-Zeit) |
| (d) VBlank (Mittel ½ Refresh) + 1 Kompositions-Frame | ≈ 8 + 17 = 25 ms | ≈ 25 ms | angenommen (typisch macOS, Fenster) |
| **Summe p50, Größenordnung** | **≈ 50 ms** | **≈ 90 ms** | hergeleitet |

Einordnung: Ein gut gepipelinetes 60-Hz-Fensterspiel auf macOS liegt bei etwa 30–50 ms. Der vermeidbare
Anteil hier ist (b) ohne die eigentliche Arbeit, also das `NextDrawable`-Warten mit schon verbauter Eingabe:
**8 ms (GPU frei) bis 19 ms (belastet) im Median, 31–51 ms im p90.** Die Frame-Zeit-Spitzen aus Schritt 3
(Frame p95 35 ms bei p50 10 ms) machen die Latenz zusätzlich ungleichmäßig. Das spürt man im Viewport eher
als Ruckeln der Kamera denn als konstante Verzögerung.

## 4. Nebenbefunde

- **Der Editor zeichnet dauerhaft**, auch ohne Eingabe und bei stehender Szene (`m_eventDriven` = false,
  nur der App-Modus des Spiels schaltet es ein). Für die FPS-Frage egal, aber auf dem lüfterlosen MacBook Air
  bedeutet das dauerhafte GPU-Last, also Wärme und Akku, und damit auf Dauer auch Drosselung. Kein
  Input-Defekt, gehört in die Synthese (Schritt 5).
- **Unter Rechnerlast streut sogar der leere Pump** bis 5 ms p90 (I0-run3). Das ist dieselbe Ursache wie
  die Job-Stalls aus Schritt 3 (Hauptthread verdrängt, E-Cores), kein Eingabeproblem.

## 5. Vorschläge (nur Befund, nichts davon umgesetzt)

| Prio | Vorschlag | Wirkung | Aufwand/Risiko |
|---|---|---|---|
| 1 | **Warten vor die Eingabe ziehen:** Frames-in-flight-Semaphore (`dispatch_semaphore`, Zählwert 2–3) am Frame-Anfang **vor** `PollEvents` warten und im `addCompletedHandler` freigeben. Alternativ `CAMetalDisplayLink` (macOS 14+) für die Taktung, dann liefert der Callback Drawable und Ziel-Präsentationszeit zusammen. | Eingabe wird erst abgefragt, wenn der Frame wirklich gebaut werden kann: −8 bis −19 ms Eingabealter (p50), FPS unverändert | mittel. `Application::Run` bräuchte einen Backend-Haken („warte auf Frame-Slot“), für alle Backends zu klären. Gehört zu Schritt 5. |
| 2 | Szenen-Arbeit **vor** `nextDrawable` committen (eigener Command Buffer für alles bis Tonemap/FXAA, nur der Present-Pass wartet aufs Drawable) | GPU startet früher, (c) überlappt mit dem Warten | mittel. Schon in Schritt 2 als Pipelining-Thema, hier nur die Latenzfolge. |
| 3 | `maximumDrawableCount` 2 als Option (z. B. „Low-Latency“-Schalter) | −1 Frame Warteschlange | klein, kostet aber Durchsatz, solange die GPU am Limit ist. Erst nach 1/2 sinnvoll. |
| – | Eingabesystem selbst: **nichts tun.** Event-Pump, Dispatch, Gamepad-Polling und Zustandslesen sind im µs-Bereich. | – | – |

## 6. Nachmessen

```sh
# Build: cmake-build-release, ninja -j8 HorizonEditor (CLion-ninja: /Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja)
R() { python3 scripts/he_perf_capture.py --project /tmp/pa1/proj/Test/Test.heproj --out docs/perf-audit/raw-step4 \
        --cam 0,25,90,0,-0.25 --scene docs/perf-audit/scenes/landscape.hescene "$@"; }
R --label I0-run1-landscape-vsyncoff  --env HE_PERF_INPUT_EVENTS=0
R --label I16-run1-landscape-vsyncoff --env HE_PERF_INPUT_EVENTS=16
R --label I64-run1-landscape-vsyncoff --env HE_PERF_INPUT_EVENTS=64
grep HE_PERF_INPUT_EVENTS out/deploy/Editor/HorizonEngine.log   # Positivkontrolle: "frame 100 dispatched N events"
```

Vorher Last prüfen (`uptime`, `ps -Ao pcpu,comm -r | head`): Kompiliert eine andere Instanz, ist der Lauf wertlos.
Für die Photonen-Latenz braucht es einen **entsperrten** Bildschirm ohne Fremdlast. Dann wären
`L2`-Läufe mit einem `addPresentedHandler`-Haken (`drawable.presentedTime` gegen den Poll-Zeitstempel,
beide auf `mach_absolute_time`) der nächste Schritt.

Rohdaten: `docs/perf-audit/raw-step4/*.summary.json` und `*.log`.
