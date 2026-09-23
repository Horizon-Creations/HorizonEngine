# Bestandsaufnahme: welche Test- und Verifikationsläufe ein sichtbares Fenster öffnen

Thema 87, Schritt 1. Stand `cec6b3b3` (Zweig `claude/headless-test-runs-no-window`).
Nichts am Code geändert.

**Wie belastbar das ist:** alles hier ist aus der Code-Lektüre abgeleitet (Engine
und die vendorte SDL3-Quelle unter `cmake-build-release/_deps/sdl3-src`), plus
die schon vorhandenen Laufberichte aus Thema 73. Keiner der Fälle wurde in diesem
Schritt nachgestellt und am Bildschirm beobachtet, gerade weil das genau das
Aufpoppen ist, um das es geht.

## Kurzfassung

Es gibt **keinen** Headless-Mechanismus für das Fenster selbst. Was „headless"
heißt (`HE_DUMP_PATH`, `HE_EXIT_AFTER_FRAMES`, `HE_CAPTURE_*`), steuert Lebensdauer
und Bildausgabe, nie die Sichtbarkeit. Das einzige Stück Code, das ein Fenster
versteckt erzeugen kann (`WindowProps::startHidden`), gehört dem Splash, und genau
der Dump-Pfad schaltet den Splash ab. Dazu kommt auf macOS, dass SDL3 die App
beim `SDL_Init(VIDEO)` aktiv in den Vordergrund holt, egal ob ein Fenster
sichtbar ist. Ein `SDL_WINDOW_HIDDEN` allein würde den Fokusraub also nicht
beheben.

## Die Bausteine, auf die sich alle Fälle zurückführen

| # | Stelle | Was dort passiert |
|---|---|---|
| B1 | `src/HE_Core/src/Window/Window.cpp:82` | `SDL_WINDOW_HIDDEN` nur wenn `props.startHidden`. |
| B2 | `src/HE_Core/src/Application/Application.cpp:127` | **Einzige** Stelle, die `startHidden` setzt: `wp.startHidden \|\| m_splash.isOpen()`. Kein Env-Var, kein Config-Feld. |
| B3 | `src/HE_Core/src/Application/Application.cpp:223` | Nach `OnInit()`: `if (wp.startHidden) m_window->Show();`. Jedes versteckt erzeugte Fenster wird also vor dem ersten Loop-Frame sichtbar gemacht. `startHidden` ist ein Splash-Mechanismus („erst zeigen, wenn fertig"), kein Headless-Mechanismus. |
| B4 | `src/HE_Editor/EditorApplication.cpp:327` | Bei `HE_DUMP_PATH` wird der Editor-Splash abgeschaltet. Folge mit B2: genau im Dump-Modus bleibt `startHidden` false, das Fenster ist ab `SDL_CreateWindow` sichtbar. |
| B5 | `src/HE_Core/src/Application/Application.cpp:272-330` | `HE_EXIT_AFTER_FRAMES` (Frame-Budget, dann `m_running=false`) und `HE_CAPTURE_FRAME`/`HE_CAPTURE_PATH` (PPM aus `CaptureViewport`). Gelten im Basis-Loop, also für Game **und** Editor. Kein Einfluss auf das Fenster. `CaptureViewport` liest das Offscreen-Target, braucht selbst kein sichtbares Fenster. |
| B6 | `_deps/sdl3-src/src/video/cocoa/SDL_cocoavideo.m:70` → `SDL_cocoaevents.m:517` und `:322-329` | `Cocoa_RegisterApp()` läuft schon bei `SDL_Init(VIDEO)` (Video-Device-Erzeugung): setzt `NSApplicationActivationPolicyRegular` (Dock-Icon), und in `applicationDidFinishLaunching` aktiviert SDL erst das Dock, wartet 300 ms und ruft dann `[NSApp activateIgnoringOtherApps:YES]`. Beides nur, solange `SDL_HINT_MAC_BACKGROUND_APP` nicht gesetzt ist. Der Hint wird in `src/` **nirgends** gesetzt (auch keine Activation-Policy, kein `SDL_VIDEO_DRIVER`/offscreen-Treiber). Engine-Seite: `SDL_Init(VIDEO)` in `Window.cpp:60`, beim Splash früher in `SplashScreen.cpp:85`. |
| B7 | `src/HE_Editor/vendor/imgui/backends/imgui_impl_sdl3.cpp:1100/1122/1177` | ImGui-Multi-Viewport (`ImGuiConfigFlags_ViewportsEnable`, `EditorApplication.cpp:771`) erzeugt losgelöste Panels/Popups/Tooltips als eigene SDL-Fenster: versteckt angelegt, dann sofort `SDL_ShowWindow`. |
| B8 | `src/HE_Core/src/Application/Application.cpp:213` und `:440` | `SDL_ShowSimpleMessageBox` bei Renderer-Init- bzw. Render-Fehler: in einem unbeaufsichtigten Lauf ein Modal, das bis zum Timeout blockiert. |
| B9 | `src/HE_Core/src/Application/SplashScreen.cpp:96` | Der Splash ist ein **eigenes** sichtbares, always-on-top-Fenster. Editor: immer an außer bei `HE_DUMP_PATH` (B4). Game: nur wenn `ProjectSettings::game.splashEnabled` (Default `false`, `ProjectSettings.h:76`); `HE_EXIT_AFTER_FRAMES` schaltet ihn nicht ab. |

## Die Fälle

### F1: ctest `test_app_todo`, Fall „Both app flavours actually start"

- **Wo:** `tests/test_app_todo.cpp:739-796` (`bootOnce`, POSIX-Zweig), aufgerufen `:902`.
  Registriert wie jede Testdatei über `add_test(NAME ${_stem} COMMAND he_tests --source-file=*${_name})`
  (`tests/CMakeLists.txt:927`), läuft also bei **jedem** vollen `ctest` mit,
  sobald eine Game-Runtime deployt ist (`HE_TEST_GAME_RUNTIME_DIR`).
- **Was:** exportiert die Todo-App zweimal (Software-Flavour und Advanced =
  Metal auf dem Mac) und startet die echte `HorizonGame` per `popen` mit
  `HE_EXIT_AFTER_FRAMES=30 HE_CAPTURE_FRAME=26 HE_CAPTURE_PATH=boot.ppm`.
  Zwei sichtbare Fenster nacheinander; im Debug-Build je bis zu ~176 s
  (Deadline 420 s), in Thema 73 lief der Test 486 s.
- **Ursache:** B2/B5. Es gibt schlicht keinen Schalter, der der Game-Runtime
  „bleib unsichtbar" sagt; der Test setzt nur Lebensdauer und Capture. Auf dem
  Mac zusätzlich B6 (Dock-Icon + Fokusraub, zweimal).
- **Warum die Headless-Infra nicht greift:** `he_shot`/`HE_DUMP_*` sind reine
  Editor-Mechanismen (`EditorApplication`), die Game-Runtime kennt sie nicht.
  Und der Test will ausdrücklich den **echten Start** (Fenster, Renderer, Pak,
  OnInit) prüfen, also ist ein Offscreen-Dump vor dem Loop kein Ersatz.
- **Nebenbefund:** der Test-Kommentar (`:851-872`) behandelt „kann ein Fenster
  öffnen" als Voraussetzung und skippt auf Linux ohne `DISPLAY`. Die CI fährt
  `ctest` auch auf `macos-latest` (`.github/workflows/ci.yml:19/188`), dort
  läuft F1 also ebenfalls mit Fenster (stört dort niemanden, zeigt aber, dass
  ein Hidden-Modus auf CI mitgetestet würde).

### F2: `scripts/he_shot.py` (und jeder andere `HE_DUMP_PATH`-Lauf des Editors)

- **Wo:** `scripts/he_shot.py:50-57` setzt `HE_DUMP_PATH`, `HE_DUMP_QUIT=1`,
  `HE_DUMP_SKYTEST=1`, `HE_DUMP_RHI=Metal` und startet
  `out/deploy/Editor/HorizonEditor`. Der Dump selbst läuft in `OnInit`
  (`EditorApplication.cpp:1787` → `dumpFrameHeadless()` ab `:4523`), rendert
  offscreen, schreibt die BMP und pusht `SDL_EVENT_QUIT` (`:6966-6973`).
- **Ursache:** B4 + B2. Weil der Dump den Splash abschaltet, wird das Fenster
  ohne `SDL_WINDOW_HIDDEN` erzeugt und steht vom `SDL_CreateWindow` bis zum
  Teardown auf dem Bildschirm: bei Release Sekunden, bei Debug die ~160 s
  Metal-Init (so steht es in `he_shot.py` selbst). Auf dem Mac dazu B6.
- **Warum die Headless-Infra nicht greift:** sie **ist** die Headless-Infra.
  „Headless" bezieht sich dort nur aufs Rendern (Offscreen-Target statt
  Swapchain, kein ImGui), nicht aufs Fenster. Die Ironie: der Kommentar an B4
  sagt „a headless dump run has nobody to show it to", schaltet aber nur den
  Splash ab, der als einziger das Fenster versteckt hätte.
- Betrifft ebenso alle Doku-Rezepte, die `HE_DUMP_*` direkt setzen
  (headless-visual-verification, headless-dump-log-and-worktree-configure).

### F3: Manueller Editor-Start als Verifikation (Thema 73 Schritt 4)

- **Wo:** `docs/editor-menu-verification-2026-09-21.md` (auf main), Tabelle
  Nr. 2 und 3: `out/deploy/Editor/HorizonEditor` mit umgebogenem `HOME`, normal
  gestartet, 429 s / 2683 Frames bzw. 794 s / 12 596 Frames, per SIGTERM beendet.
- **Ursache:** es gibt keinen Modus „volle Editor-UI für N Frames, ohne
  sichtbares Fenster". Der Dump-Pfad taugt dafür nicht, er beendet **vor** dem
  ersten UI-Frame (so steht es auch im Dokument selbst, Zeile 69). Also blieb
  nur der normale Start, mit Splash (B9), Hauptfenster, Dock-Icon und
  Fokusraub (B6).
- **Was schon ginge, aber unbekannt war:** `HE_EXIT_AFTER_FRAMES` (B5) gilt im
  Basis-Loop auch für den Editor und hätte den SIGTERM-Lauf sauber auf eine
  Frame-Zahl begrenzt, mit „leaving cleanly" im Log als Exit-Beleg. Das Fenster
  wäre trotzdem sichtbar gewesen.

### F4: Chefchens Editor-Smoke-Test mit `HE_HEADLESS_DUMP` / `HE_HEADLESS_DUMP_FRAMES`

- **Ursache:** diese beiden Variablen gibt es nicht. Grep über `src/`,
  `tests/`, `scripts/`: null Treffer für `HE_HEADLESS`. Der Editor hat sie
  ignoriert und ist normal gestartet, mit Splash und Fenster. Die echten Namen
  sind `HE_DUMP_PATH` (Pflicht, schaltet alles andere erst scharf),
  `HE_DUMP_QUIT`, `HE_DUMP_FRAMES` (Settle-Frames vor dem Capture, 1-240,
  `EditorApplication.cpp:6709`).
- **Auch mit den richtigen Namen** wäre das Fenster sichtbar gewesen (F2), und
  für einen „läuft die UI"-Smoke wäre der Dump der falsche Werkzeugtyp (F3).

### F5: `scripts/he_mcp_multiclient.py`

- **Wo:** `:139-152`, startet den vollen Editor mit `HE_MCP=1`, `HE_MCP_PORT`,
  `HE_DUMP_LIVE`, `HE_DUMP_LIVE_TRIGGER` per `Popen` (Rezept aus Memory:
  ~6,5 min bis zum Endpunkt).
- **Ursache:** `HE_DUMP_LIVE` (`EditorApplication.cpp:4488`) braucht den
  **laufenden** Loop, damit sich MCP-Clients verbinden können; `HE_DUMP_PATH`
  ist dafür ausgeschlossen. Also normaler Start: Splash (B9), Hauptfenster,
  B6, und mit echter UI auch B7.

## Was **kein** Fenster erzeugt (geprüft)

- **`he_tests` selbst:** in `tests/` ruft nur `test_input_gamepad.cpp:558`
  SDL auf, und zwar `SDL_InitSubSystem(SDL_INIT_GAMEPAD)`, kein Video. Kein
  Test instanziiert `Window`, `RendererFactory`, `GameApplication` oder
  `EditorApplication`-Loops. Die ImGui-UI-Tests (`test_*_ui.cpp`,
  `HE_UI_DUMP_DIR`) laufen über den CPU-Rasterizer ohne SDL-Video. Kein Test
  ruft `dialog.message`/`confirm` (`EngineApi.cpp:1672/1696`, echte
  SDL-Messagebox).
- **`scripts/he_uishot.py`:** startet nur `he_tests -tc=…` (`:123`), s. o.
- **Die Python-ctests** `editor_help_audit`, `he_mcp_shim` (Fake-TCP-Server,
  kein Editor), `runtime_size*`: keiner startet eine Engine-Binary.
- **Übrige Skripte** (`build_runtimes.py`, `validate_embedded_shaders.py`,
  `he_syntax.py`, `editor_menu_parity.py`): bauen oder lesen Quellen, starten
  keine App.

Im ganzen ctest ist F1 also die **einzige** Fensterquelle.

## Hebel für Schritt 2 (nur benannt, nicht gebaut)

1. **Ein echter Hidden-Zustand in `Application`**, getrennt von `startHidden`:
   z. B. Env `HE_HIDDEN_WINDOW=1` (oder automatisch bei `HE_DUMP_PATH` /
   `HE_EXIT_AFTER_FRAMES`), der `SDL_WINDOW_HIDDEN` setzt **und** das `Show()`
   an B3 unterdrückt. Nur das Flag umzubiegen reicht wegen B3 nicht.
2. **Auf macOS `SDL_HINT_MAC_BACKGROUND_APP=1` vor dem ersten
   `SDL_Init(VIDEO)`**, also vor `SplashScreen.cpp:85` und `Window.cpp:60`.
   Ohne das bleiben Dock-Icon und Fokusraub (B6), auch bei verstecktem Fenster.
   Zu prüfen: ob die Hint-Wirkung auch das native Menü (`MacMenuBar`) stört;
   im Hidden-Lauf wäre das egal.
3. **Splash im Hidden-Modus immer aus** (B9), auch in der Game-Runtime.
4. **ImGui-Viewports im Hidden-Modus aus** (B7), sonst poppen Tooltips und
   losgelöste Panels aus einer gespeicherten `imgui.ini` als eigene Fenster auf.
5. **Messageboxen im Hidden-Modus nur loggen** (B8), sonst hängt ein
   Fehlerlauf bis zum Timeout statt rot zu werden.
6. **Offene Risikofrage, erst empirisch klären:** läuft der Metal-Present-Pfad
   mit einem versteckten Fenster überhaupt normal weiter? Ein verdecktes/
   verstecktes Fenster auf macOS drosselt der Loop laut Kommentar an
   `EditorApplication.cpp:1784` („throttles when the window is occluded"), und
   `nextDrawable` kann für einen nicht sichtbaren Layer ausbremsen. Das betrifft
   F1 (30 Frames bis zum Exit) und F3/F5 direkt. Der Dump-Pfad (F2) rendert
   ohnehin offscreen vor dem Loop und ist davon nicht betroffen. Falls der
   Present hängt: Present im Hidden-Modus überspringen und nur ins
   Offscreen-Target rendern (das ist, was `CaptureViewport` ohnehin liest).
7. **Doku/Rezepte:** F3/F4 zeigen, dass die vorhandenen Env-Vars nicht bekannt
   sind. Eine Stelle mit „Editor/Game unbeaufsichtigt starten" (Namen, was sie
   tun, was nicht) gehört mit in den Umbau.

## Stand nach Schritt 2 (Hidden-Modus gebaut)

**Schalter:** `HE::hiddenWindowRequested()` (`Window.h`, HE_API), einmal pro
Prozess gelesen. `HE_HIDDEN_WINDOW` entscheidet, wenn gesetzt (`0` = sichtbar
erzwingen, alles andere = hidden). Sonst hidden bei `HE_EXIT_AFTER_FRAMES`≠0
oder `HE_DUMP_PATH`. Die Logzeile „Hidden mode on (<Auslöser>)" nennt den Grund.
Unit-Test: `tests/test_hidden_window.cpp`.

**Wirkung, je Baustein:**

- B1/B2/B3: jedes `Window` (auch Sekundärfenster) wird mit `SDL_WINDOW_HIDDEN`
  erzeugt, `Window::Show()` ist im Hidden-Modus ein No-op. Maximize, Fullscreen
  und Raise auf einem versteckten Fenster legt SDL3 nur in `pending_flags` ab
  (`SDL_video.c`), die machen es also nicht sichtbar.
- B6: `SDL_HINT_MAC_BACKGROUND_APP=1` (plus `WINDOW_ACTIVATE_WHEN_SHOWN/RAISED=0`)
  ganz oben in `Application::Run`, vor dem ersten `SDL_Init(VIDEO)`. Das
  Menü, das SDL baut, liegt in `Cocoa_RegisterApp` außerhalb des Hint-Gates und
  bleibt also. Es fallen nur die Activation-Policy Regular und
  `activateIgnoringOtherApps` weg. **Grenze:** eine gebündelte `.app` bekommt die
  Policy aus ihrem Info.plist, dort bliebe das Dock-Icon. Alle heutigen
  Test-/Skriptläufe starten unbundled Binaries (`HorizonGame` im Export,
  `out/deploy/Editor/HorizonEditor`).
- B9: `SplashScreen::open` steigt im Hidden-Modus aus (Editor und Game, auch bei
  `game.splashEnabled`).
- B7: `ImGuiConfigFlags_ViewportsEnable` im Hidden-Modus aus
  (`EditorUI.cpp` hängt `UpdatePlatformWindows` schon am Flag).
- B8: `Application`-Fehlerboxen und die Skript-Dialoge `dialog.message` /
  `dialog.confirm` (`EngineApi.cpp`) loggen nur; `confirm` antwortet „nein".
  Datei-Picker (`dialog.open*`/`save*`) sind **nicht** abgedeckt.
- Zeuge: „Primary window hidden|shown as the main loop starts" im Log.

**Hebel 6, gemessen (Release, M5, Metal, 23.09.2026):** der Present-Pfad läuft
mit verstecktem Fenster normal weiter. Editor ohne Projekt, eingeschwungen aus
der Differenz 900−300 Frames: hidden 21,1 ms/Frame, sichtbar 24,0 ms/Frame,
beide ohne einen einzigen Hitch. Kein Present-Skip nötig, daher nicht gebaut.
Die Todo-App ist dafür kein Orakel: sie läuft ereignisgetrieben (~190 ms/Frame
Heartbeat), hidden und sichtbar exakt gleich (90 Frames je 17,1 s).

**Verifiziert:** `test_app_todo` grün (25 s im ctest, beide Flavours, Log
„Hidden mode on (HE_EXIT_AFTER_FRAMES)"). Editor 60 Frames hidden gegen
`HE_HIDDEN_WINDOW=0`: „Splash: off in hidden mode" + „hidden" gegen Splash über
Metal + „shown". Dump-Lauf (`HE_DUMP_PATH`, wie `he_shot.py`) ist automatisch
hidden und schreibt trotzdem das Bild (639 verschiedene Farben in der
Stichprobe). Voller ctest: 195/196 grün, `runtime_size` rot (Game-Runtime ohne
Python 32,6 > 32,0 MB). Das liegt nicht an diesem Schritt: gegen den
Release-Build auf main vom 20.09. (vor dem Merge von Thema 77, `cec6b3b3`) ist
`libHorizonNet` um 90 KB gewachsen, obwohl hier nichts in HE_Net geändert
wurde, `libHorizonScene` um 538 KB, `libHorizonCore` um 42 KB. Zusammen sind
das die ~0,6 MB. Die Schwelle in `scripts/runtime_size.py` ist seit dem 05.09.
unverändert.

**Nicht verifiziert:** dass am Bildschirm wirklich nichts erscheint. Während der
Läufe war die Sitzung gesperrt (Vordergrund `loginwindow`), und dann meldet
`CGWindowListCopyWindowInfo` auch für den sichtbaren Kontrolllauf kein Fenster.
Das Orakel konnte also nicht trennen. Belegt ist nur SDLs eigener Fensterzustand
(Zeuge oben), nicht der des Window-Servers. Dasselbe gilt für die Hebel-6-Zahlen:
auch der „sichtbare" Kontrolllauf lief auf der gesperrten Sitzung. Belegt ist
also „hidden hängt nicht und liefert Frames im normalen Takt", nicht „so schnell
wie ein wirklich sichtbares Fenster".

Nachholen am entsperrten Bildschirm (30 s):
`HOME=$(mktemp -d) HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=600 out/deploy/Editor/HorizonEditor`.
Erwartung: kein Fenster, kein Splash, kein Dock-Icon, der Fokus bleibt, wo er
war. Negativkontrolle: dasselbe mit `HE_HIDDEN_WINDOW=0`.

**Offen für Schritt 3:** `he_mcp_multiclient.py` (F5, `HE_DUMP_LIVE` ohne
Frame-Budget) muss `HE_HIDDEN_WINDOW=1` selbst setzen. Doku/Rezepte (Hebel 7)
und die Datei-Picker stehen auch noch aus.

## Stand nach Schritt 3 (F1 explizit hidden, Rezept)

- **F1:** `bootOnce` setzt `HE_HIDDEN_WINDOW=1` ausdrücklich, statt sich auf
  die Ableitung aus `HE_EXIT_AFTER_FRAMES` zu verlassen, und der Testfall
  prüft nach sauberem Exit die Zeile „Primary window hidden as the main loop
  starts" in `boot.log`. „Kein Fenster" ist damit eine Assertion, keine
  Beobachtung. Negativkontrolle: mit `HE_HIDDEN_WINDOW=0` wird der Test an
  genau dieser Stelle rot (beide Flavours).
- **F5:** `he_mcp_multiclient.py` setzt `HE_HIDDEN_WINDOW=1` per `setdefault`.
  Nicht laufen lassen (6,5-min-Live-Rezept), nur die Env-Zeile geändert.
- **F2** braucht nichts (`HE_DUMP_PATH` schaltet selbst), **F3/F4** sind
  Rezepte: **`docs/headless-runs.md`** („Editor/Game unbeaufsichtigt starten",
  Hebel 7) nennt alle Schalter, die Zeugen-Zeilen, den Smoke-Test-Aufruf und
  die Fallen.
- **Gemessen (Release, M5, 23.09.2026, Sitzung gesperrt):** `test_app_todo`
  allein 32 s (vorher 176-486 s mit Fenster), im vollen `ctest -j8` 69 s. Die
  Differenz ist nicht Drosselung: die 30 Loop-Frames brauchen im Parallellauf
  6,3/7,1 s gegen 5,6/6,0 s allein, die Mehrzeit liegt vor „OnInit complete"
  im Metal-Flavour (46,7 s gegen 11,7 s), also Pipeline-Kompilieren unter
  CPU-Last. Voller ctest 195/196, rot nur `runtime_size` (32,6 > 32,0 MB,
  Thema 88).
- **Weiter nicht verifiziert:** dass der Window-Server nichts zeigt (gesperrte
  Sitzung, siehe oben). Datei-Picker im Hidden-Modus bleiben offen.
