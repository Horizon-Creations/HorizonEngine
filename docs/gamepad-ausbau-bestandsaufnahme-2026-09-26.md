# Gamepad-Ausbau: Bestandsaufnahme (Thema 85, Schritt 1)

Stand: 26.09.2026, Zweig `claude/gamepad-catchup` auf main 152659ff. Vorgänger-Plan: `docs/gamepad-support-plan.md` (E4). Dessen Abschnitt „Explizit NICHT in E4" nennt genau die drei Lücken dieses Themas: Rumble, Per-Player-Zuordnung, In-Game-Remapping. Dieses Dokument klärt, wo sie andocken und in welcher Reihenfolge sie kommen.

## 1. Was heute steht

Datenfluss (unverändert seit E4):

```
SDL-Events → Application → Input::ProcessGamepadEvent (nur Hot-Plug, öffnet/schließt SDL_Gamepad*)
           → Input::PollGamepads   (alle Pads → EIN GamepadFrame, mergeGamepadFrame)
           → PlayerHost::tick → InputMapping::tick(Input&, MouseFrame)
           → Input.<Action>.Pressed/Released/Axis/Axis2D an JEDEN Controller (+ dessen Pawn) + Lua/Python
           → HE::api::input::setActions (Polling-Zwilling), setGamepad (gefilterter Snapshot für Skripte)
```

| Baustein | Ort | Befund |
|---|---|---|
| Pad-Handles | `Input.h:149` `unordered_map<SDL_JoystickID, SDL_Gamepad*> m_pads` (privat) | Alle offenen Pads sind da, aber nicht nach außen sichtbar |
| Merge | `Input.h` `mergeGamepadFrame`, `PollGamepads` | Hart „alle Pads = ein Spieler" (größte Magnitude pro Achse, Buttons ODER) |
| Deadzone | `Input.h:109` `stickDeadzone`/`triggerDeadzone` | Nur der **Editor** schreibt sie (`EditorApplication.cpp:1202`, aus EditorConfig). Das Spiel läuft immer auf den Defaults 0.15/0.05 |
| Stick-Look | `CameraRigComponent::stickSensitivity/stickInvertY` | **Szenendaten**, im Inspector einstellbar, kein Spieler-Override, keine Skript-API |
| Bindings | `InputMapping` (`mapAction/mapAxis/mapAxis2D`) | Eine Instanz pro `PlayerHost`. Keine Contexts zur Laufzeit, keine Prioritäten, kein Aktivieren/Deaktivieren |
| Asset-Laden | `PlayerHost.cpp:51` `applyInputMappingContext` für jeden Context | Siehe Befund B2 |
| Skript-Gamepad | `EngineApi.h:2423ff` `input.gamepadConnected/Button/Axis`, `HorizonGameServices.h:186-192` | Nur lesend, nur der gemergte Frame |
| Input-Modi | `input.setModeGameOnly/GameAndUI/UIOnly`, Action-Flag `runWhilePaused` | Grundlage für Menüs existiert |
| Persistenz | `EngineApi.h:2014` `prefs.*` (JSON im fs-Sandbox, schreibt bei jeder Änderung) | Passender Speicher für Spieler-Einstellungen |
| Widget-Menüs | `UIWidgetType`: Button, CheckBox, Slider, ComboBox, ScrollBox, ListView, WidgetRef, … | Genug für ein Settings-Menü |
| Pad-Menü-Navigation | `GameApplication.cpp:~2210` (Editor-PIE `EditorApplication.cpp:~3509`) | D-Pad → `WidgetManager::navigate`, South → `activateFocused` ist **verdrahtet**. Fehlt: Stick-Navigation, East = Zurück |
| Rumble | nirgends | Kein `SDL_RumbleGamepad` im Code, kein API |
| Editor-Capture | `InputAssetPanel.cpp:67ff` (`s_capture`, „Press it to bind it") | ImGui-gebunden, im Spiel **nicht** wiederverwendbar |
| Runtime-Settings | nichts | Kein engine-geliefertes Settings-/Pausenmenü, keine Starter-Widgets dafür (ProjectManager legt nur Root-Widget + Move/Look/Jump an). VSync nur als Dev-Hotkey (`GameApplication.cpp:2652`), keine Grafik-Settings-API. Nur `audio.setBusVolume` ist per Skript erreichbar |

SDL-Version im Build: **3.2.14**. Vorhanden: `SDL_RumbleGamepad`, `SDL_RumbleGamepadTriggers` (kein Haptic-Subsystem nötig, läuft über `SDL_INIT_GAMEPAD`, das `Window.cpp:114` schon initialisiert), `SDL_Get/SetGamepadPlayerIndex` (Spieler-LED), virtueller Joystick mit `Rumble`-Callback in `SDL_VirtualJoystickDesc` (damit headless testbar, wie der bestehende E2E-Test in `tests/test_input_gamepad.cpp`).

## 2. Befunde (Fehler, die nebenbei aufgefallen sind)

**B1: Spiel-Template bindet den Stick nicht (Fehler, ungetestet).**
`src/HE_Tools/src/FileOps/ProjectManager.cpp:1022` (`kMappings`, seit f1be4389 vom 01.09.) schreibt `{"source":"GamepadAxis","axis":"leftx"}` und `{"source":"MouseDeltaX"}`. Der Loader (`InputAssets.cpp` `axisSourceFromName`) kennt aber nur `GamepadLeftX/LeftY/RightX/RightY/LeftTrigger/RightTrigger` und `MouseX/MouseY/MouseWheel`. Unbekannt fällt auf `Key` zurück, und eine Key-Zeile ohne Tasten wird in `readAxes` verworfen. Folge: In jedem neu angelegten Spielprojekt bewegt der linke Stick die Figur **nicht**. Nur `Jump` (`"gamepadButtons":["a"]`) geht am Pad, und Look ist komplett leer (vom Template-Graphen allerdings ohnehin nicht benutzt). Fix: vier Strings anpassen (`GamepadLeftX`, `GamepadLeftY`, `GamepadRightX`, `GamepadRightY`, `MouseX`, `MouseY`) plus ein Test, der das Template durch `applyInputMappingContext` schickt und die Bindungszahl prüft. Klein, sollte vor oder mit Schritt 2 erledigt werden.

**B2: „Union aller Mapping-Contexts" ist in Wahrheit Last-Writer-Wins mit nicht festgelegter Reihenfolge.**
Der Kommentar in `PlayerHost.cpp` sagt „union of every mapping context". `InputMapping::mapAction/mapAxis/mapAxis2D` **ersetzen** aber die Bindings pro Aktionsname. Taucht eine Aktion in zwei Contexts auf, gewinnt der zuletzt angewendete, und die Reihenfolge kommt aus `ContentManager::discoverAssets`: zuerst `m_assetTypeIndex` (eine `unordered_map`), danach `recursive_directory_iterator`. Beides ist nicht festgelegt, das Ergebnis kann also je nach Plattform bzw. Dateisystem anders ausfallen. Für das Rebinding ist das wichtig, weil eine Override-Schicht genau diese Ersetzungs-Semantik nutzen will, aber auf einer **festen** Basis. Vorschlag: Contexts beim Laden nach Pfad sortieren und im selben Zug entscheiden, ob mehrere Contexts pro Aktion vereinigt (append) oder ersetzt werden. Vereinigen passt zum Kommentar und zur Erwartung der Autoren.

*Gelöst in Schritt 3:* `PlayerHost::begin` wendet die Contexts nach Pfad sortiert an (`PlayerHost::mappingContexts()` nennt die Reihenfolge). `applyInputMappingContext` vereinigt standardmäßig (`HE::MappingMerge::Union` über `InputMapping::addAction/addAxis/addAxis2D`, doppelte Bindings zählen einmal). `MappingMerge::Replace` (die alten `map*`-Aufrufe) bleibt für die Override-Schicht aus Schritt 3 der Reihenfolge unten. Bei einem Formkonflikt (1D in einem Context, 2D im anderen) gewinnt der im Pfad spätere Context. Tests: `test_player_host.cpp` („mapping contexts are a union applied in path order"), `test_inputmapping.cpp` (Union/Replace, X und Y aus zwei Contexts, Formkonflikt).

## 3. Andockpunkte

### Rumble
- `Input` (HE_Core, `HE_API`): `bool rumble(float low, float high, uint32_t ms)` über **alle** Handles in `m_pads`. Das ist konsistent mit der Merge-Politik „alle Pads = ein Spieler". Dazu `rumbleTriggers(...)` (optional, nur DualSense/Xbox One), `stopRumble()`. Werte 0..1 → `Uint16` skalieren.
- Weg zur Skript-API: `Input` lebt in HE_Core, `EngineApi` in HE_Scene und kennt `Input` nicht. Muster wie `setGamepad`, nur umgekehrt: Die App registriert einen Callback/Hook (`HE::api::input::setRumbleSink(...)`) oder `EngineApi` legt eine Anfrage-Queue an, die die App pro Frame abarbeitet. Der Hook ist einfacher und hat keine Frame-Verzögerung.
- Gate: Im Editor nur bei `m_isPlaying` (dasselbe Gate wie die Snapshot-Speisung, `EditorApplication.cpp:2474ff`). Beim Stop von PIE und bei Pause **alle Pads stoppen**, sonst brummt ein Dauer-Rumble (`duration <= 0`-Fall) weiter. Die Frage, ob Rumble bei `time.isPaused()` pausiert, gehört ins Design von Schritt 2.
- Kopplung an die Kamera: `camera.playShake` (`EngineApi.h:1199ff`) kennt schon das Dauer-Handle-Muster. Rumble sollte dasselbe Handle-Muster bekommen (`play → handle`, `stop(handle)`), eine automatische Kopplung Shake→Rumble aber **nicht** (bleibt Entscheidung des Spiels).
- Checkliste neue API-Zeilen (vier Stellen, nicht drei): (1) `EngineApi.h/.cpp` Funktion + Registry-Row, (2) Display-Name-Map, (3) `HcNodeDocs.cpp` (`isScriptGroup("input")` gilt schon, Lua/Python kommen umsonst), (4) `HorizonGameServices.h` Funktionszeiger in der Input-Services-Tabelle + Inline-Wrapper `he::input::rumble`, sonst fehlt es der C++-Gamelogic-dylib. Dazu die Parity-Fixture (`HCGEN_CLASSES`) und `tests/fixtures/test_gamelogic_services.cpp`.
- Test: `SDL_VirtualJoystickDesc::Rumble`-Callback zeichnet `low/high` auf und bestätigt, dass `Input::rumble` ankommt; überspringt wie der bestehende E2E-Test, wenn das Subsystem fehlt.

### Per-Player-Pad-Zuweisung
- Input-Schicht: aus `m_pads` eine Slot-Tabelle machen (`SDL_JoystickID → Slot 0..N-1`, Vergabe beim Hot-Plug, Slot bleibt beim Abziehen reserviert, damit ein wieder eingestecktes Pad denselben Spieler bekommt). `GamepadFrame gamepad(int slot)` zusätzlich zum gemergten `gamepad()`. Der gemergte Frame bleibt Default, damit alle Single-Player-Projekte unverändert laufen. `SDL_SetGamepadPlayerIndex` für die Pad-LED.
- Mapping-Schicht: `InputMapping::tick` liest heute `input.isGamepadButtonDown` bzw. `gamepadAxisFiltered`, also den gemergten Frame. Es braucht eine Quelle pro Spieler (entweder `tick(const GamepadFrame&, …)` oder eine „Gerätesicht" als Parameter). Tastatur/Maus gehören dann einem Spieler (Default Spieler 0).
- PlayerHost: heute **ein** `InputMapping` und `fireInputEvent` an **alle** Controller. Für lokalen Multiplayer: ein Mapping-Zustand pro Controller, Controller ↔ Slot-Zuordnung, Events nur an den eigenen Controller. `HE::api::player::controller()` antwortet heute „der erste", das braucht eine Index-Variante.
- **Außerhalb des Themas** (sonst wächst es unkontrolliert): Splitscreen-Rendering (mehrere Viewports/Kamera-Rigs), die Annahme „eine Sitzung hat einen lokalen Spieler" in der Net-Schicht (`GameApplication.cpp:1508-1530`, `net.localPlayer`). Per-Player-Input ist ohne Splitscreen trotzdem nützlich (Couch-Coop auf einem Bildschirm, Party-Spiele), darum trennbar.

### Rebinding
- Override-Schicht in `PlayerHost::begin` **nach** der Context-Schleife: Spieler-Overrides im **selben Entries-JSON-Format** wie ein Mapping-Context und ebenfalls durch `applyInputMappingContext` geschickt. So gibt es keinen zweiten Parser, und die Ersetzungs-Semantik von `mapAction` ist hier genau richtig (Override ersetzt die Aktion). Voraussetzung: B2 gelöst, damit die Basis fest ist.
- Persistenz: `prefs`-Datei (ein Schlüssel `input.overrides` mit dem JSON-String) oder eine eigene Datei im selben Sandbox-Pfad. `prefs` reicht für v1.
- Neue API (Skript + C++), grob: `input.rebindBegin(action, slot, device)`, dazu ein Capture-Zustand, der über Kantenerkennung in `Input` läuft (Muster `m_uiNavPrev` in `GameApplication`, alle Scancodes, Pad-Buttons, Achsen über Schwelle), sowie `input.rebindCancel()`, `input.bindingName(action, slot)` für die Anzeige (`gamepadButtonDisplayName` existiert), `input.resetBindings()`, `input.saveBindings()`. Die Capture muss die eigene Bestätigungs- und Abbruchtaste (South/Enter bzw. Esc/East) unterscheiden und Doppelbelegungen melden.
- Wichtig beim Capture: Im UIOnly-Modus ist Gameplay-Input stumm, die Capture muss also **vor** dem Mapping direkt auf `Input` lesen, nicht über Actions.
- Die Editor-Capture (`InputAssetPanel.cpp`) ist nur als Vorbild für die Logik brauchbar (Priming, Esc bricht ab), nicht als Code.

*Umgesetzt in Schritt 4* (`Application/InputRebind.h`, `PlayerHost`, Tests `test_input_rebind.cpp` und `test_player_host.cpp`):
- **Override pro Aktion und Geräteklasse** (Tastatur+Maus als eine Klasse, Gamepad als die andere), nicht der ganze Binding-Satz. Beim Anwenden: Basis-Zeilen dieser Klasse raus, Override rein, die andere Klasse bleibt. Damit sieht ein Spieler, der nur die Tastatur umgelegt hat, eine später im Projekt ergänzte Pad-Belegung trotzdem. Gespeichert im Entries-Format, gelesen über `applyInputMappingContext` in eine Scratch-Mapping (kein zweiter Parser).
- **Capture = Kantenerkennung gegen einen Schnappschuss**, nicht „warten bis nichts gedrückt ist": Was beim Scharfschalten gedrückt ist (der Bestätigungsdruck, eine hängende Taste), zählt nie, und eine hängende Taste blockiert nichts. Nach dem Fang bleibt die Capture „beschäftigt", bis genau diese Taste losgelassen ist. Erst dann wird die Mapping neu gebaut, plus ein Vor-Tick gegen den laufenden Frame, damit Gehaltenes nicht als neuer Druck gilt.
- **Abbruch: Esc und Pad-Start** (beide Geräte, unabhängig vom gebundenen Gerät). Beide sind darum per Capture nicht belegbar. East blieb frei, weil es UI-Zurück und oft Gameplay ist. Da beide Apps Esc in `OnEvent` verbrauchen, bevor `Input` es sieht, brechen sie selbst ab (`isRebinding()` → `rebindCancel()`), vor allen anderen Esc-Bedeutungen.
- Während die Capture läuft: **alle** Aktionen stumm (auch `runWhilePaused`), UI-Navigation (Back/Tab/D-Pad/South) und Zeiger-Klicks in Spiel und Editor-PIE gesperrt. Die Vorzustände laufen weiter mit, damit danach keine Phantom-Kante entsteht. Maustasten erreichen die Capture auch im UIOnly-Menü (Spiel) bzw. mit freiem Cursor über der PIE-Ansicht (Editor, nur Tasten, keine Bewegung).
- Zeilen (vier Stellen + C-ABI v3): `rebindBegin(action, device)`, `rebindCancel`, `isRebinding`, `rebindConflict`, `bindingName(action, device)`, `resetBindings`, `saveBindings`. `isRebinding` und `rebindConflict` sind zusätzlich zur Aufgabenliste nötig, sonst weiß ein Menü nicht, wann es die Beschriftung erneuern und warnen soll. `bindingName` nimmt das **Gerät**, nicht den Slot. Der Slot steckt nur im Prefs-Schlüssel `input.overrides.0`.
- Persistenz: `saveBindings` schreibt den Schlüssel (leere Schicht entfernt ihn), `resetBindings` wirft nur im Speicher weg, `begin()` lädt. Ungespeicherte Rebinds enden mit der Sitzung.
- **Offen:** Achsen (WASD-Move, Stick-Zuordnung) brauchen einen Richtungsparameter, v1 lehnt sie ab (`rebindBegin` → false + Log). Der Prefs-Speicher ist prozessweit statisch (im Editor über Projektwechsel hinweg, bekanntes Muster). Im Editor-PIE erreichen Tastendrücke `Input` nur, wenn ImGui die Tastatur nicht will. Das gilt schon für die Menü-Navigation und ist nicht neu. Real-HW und Windows-CI sind ungeprüft.

### Runtime-Settings-Menü
- Die Engine hat **alle Bausteine**, aber kein fertiges Menü und keine Settings-API. Was ein Spieler heute nicht einstellen kann: Deadzone, Stick-Sensitivity, Invert-Y (nur Szenendaten), VSync, Fenster/Vollbild, Bindings. Lautstärke geht per `audio.setBusVolume`.
- Empfehlung: zuerst die **API** (`settings.*` oder Erweiterung der vorhandenen Gruppen: `input.setStickDeadzone`, `camera.setStickSensitivity` als Spieler-Override über der Rig-Komponente, `app.setVSync`, …, alles über `prefs` persistiert und beim Start angewendet), danach als Komfort ein **Starter-Widget** „Settings" im Projekt-Template bzw. EngineContent, das nur diese API aufruft. So hängt das Menü an der API, nicht umgekehrt.

*Umgesetzt in Schritt 5* (`HE_Scene/src/PlayerSettings.cpp`, Template in `ProjectManager.cpp`, Tests `test_player_settings.cpp`, `test_third_person_template.cpp`, `test_camera_rig.cpp`):
- **Store `HE::api::settings`**: jeder Spielerwert ist **optional**, „nie gewählt" heißt Projektwert (bzw. App-Wert). Ändert ein Projekt später seine Defaults, erreicht das jeden Spieler, der nichts überschrieben hat, wie bei den Binding-Overrides. Persistenz: **ein** prefs-Schlüssel `settings` mit einem JSON-Objekt. `settings.save` schreibt, ein leerer Satz entfernt den Schlüssel. Beschädigte Einzelwerte werden übersprungen und Ausreißer geklemmt, NaN wird abgewiesen. Änderungen wirken sofort und halten die Sitzung über, gespeichert wird erst mit `save` (derselbe Vertrag wie `input.saveBindings`, ein Slider-Zug schreibt nicht bei jedem Schritt auf die Platte).
- **Zeilen** (vier Stellen + Parity-Fixture `player_settings`): `input.setStickDeadzone/stickDeadzone` (auch C-ABI `HeInputServices` v4), `camera.setStickSensitivityScale/stickSensitivityScale`, `camera.setStickInvertY/stickInvertY`, `app.setVSync/vsync/setFullscreen/isFullscreen`, `settings.setVolume/volume/save/resetToDefaults` (neue Skriptgruppe `settings`).
- **Abweichung vom Brett**: Die Stick-Sensitivity ist ein **Faktor** auf die Rig-Geschwindigkeit, kein Ersatz. Rigs behalten so ihren entworfenen Unterschied (Fahrzeug langsam, zu Fuß schnell). Invert **ersetzt** das Rig-Flag, sobald der Spieler gewählt hat. Die Rig-Komponente bleibt unangetastet, und `CameraRigController` liest beides pro Frame.
- **Apps**: Das Spiel lädt direkt nach dem Sandbox-Root (vor `fireInit`) und installiert die Hooks nach Audio-Init und Bus-Config. „Vollbild aus" kehrt zum **konfigurierten** Modus zurück, ein Borderless-Export bleibt also Borderless. Die Anzeige-Hooks vergleichen vor dem Anfassen. Lautstärke geht pro Bus (`Master` = Master), `nullopt` = Projektwert, und nur geänderte Busse werden angefasst, damit ein per Skript geduckter Bus bleibt. Editor-PIE: Laden beim Start, Deadzone und Lautstärke live, VSync und Vollbild nur gemerkt (es ist das Editorfenster). Beim Stop kommen Editor-Deadzone und Projekt-Mixer zurück.
- **Starter-Menü** im Third-Person-Template: `UI/SettingsMenu.hasset` (Deadzone, Look-Sensitivity, Invert, VSync, Vollbild, Master-Lautstärke, Jump auf Tastatur und Gamepad neu belegen, Reset, Back). Der Graph ruft **nur** die Zeilen oben auf und wird über die Graph-API gebaut, nicht als JSON-Literal. Neue Aktion `Menu` (Taste `O`, Pad `start`; nicht Esc, das die Apps vorher verbrauchen). Der Controller legt das Menü nach Possess versteckt an. `Menu` zeigt es, ruft `Refresh`, schaltet UIOnly und gibt den Cursor frei. Back speichert Settings **und** Bindings, schaltet GameOnly und versteckt das Menü. Jede Engine-Call-Zeile im Template wird im Test gegen die Registry geprüft (Parameter von Hand, weil HE_Tools die Registry nicht linkt).
- **Offen:** Beim Start mit gespeichertem „Vollbild aus" öffnet das Fenster zuerst im Exportmodus und wechselt dann in `OnInit` (Prefs liegen erst ab dem Sandbox-Root vor). Bei offenem Audio-Mixer im Editor überschreibt dessen `applyBusConfig` pro Frame die Spieler-Lautstärke, wie schon `audio.setBusVolume`. Maus-Sensitivity ist keine Spielereinstellung. Kamera-, App- und Audio-Zeilen haben keine C-ABI-Tabelle (nur die Deadzone). Das Menü nennt Achsen „noch nicht belegbar" (wie Schritt 4). Real-HW, sichtbarer UI-Screenshot und Windows-CI sind ungeprüft.

## 4. Reihenfolge (Entscheidung, weicht vom Brett ab)

Das Brett schlägt vor: Rumble → Multi-Pad → Settings-Menü → Rebinding-UI, mit „Rebinding hängt am Settings-Menü". Die Bestandsaufnahme sagt: Rebinding hängt an einer **API** (Override-Schicht + Capture + Persistenz), nicht an einem engine-gelieferten Menü. Widgets, Pad-Navigation, `prefs` und Input-Modi sind da, Pausenmenüs bauen Spiele schon heute selbst. Multi-Pad hat die meisten versteckten Abhängigkeiten (Mapping pro Spieler, PlayerHost-Routing, `player.controller()`, Net-Annahme) und bringt ohne Splitscreen am wenigsten.

```
B1 Template-Fix ─┐
B2 Context-Reihenfolge ─┬─> (3) Rebinding-API ──> (4) Settings-API + Starter-Settings-/Rebinding-Widget
(1) Rumble (unabhängig) │
                        └─> (5) Per-Player-Pad-Zuweisung (Input-Slots → Mapping pro Spieler → PlayerHost-Routing)
```

Konkret:
1. **Rumble** (klein, unabhängig, headless testbar). B1 im selben Zug mitnehmen, weil es ein Ein-Zeilen-Fix mit Test ist und jedes neue Projekt betrifft.
2. **B2**: Context-Reihenfolge festlegen (sortieren, Union oder Ersetzen entscheiden, Kommentar angleichen). Klein, aber Voraussetzung für 3.
3. **Rebinding-API** (Override-Schicht, Capture, Persistenz, Skript-Zeilen an vier Stellen).
4. **Settings-API + Starter-Menü-Widget** (Deadzone, Stick-Sensitivity/Invert, VSync/Vollbild, Lautstärke, Rebinding-Liste). Das ist die „In-Game-Rebinding-UI" aus dem Thema.
5. **Per-Player-Pad-Zuweisung** zuletzt: Input-Slots, Mapping pro Spieler, PlayerHost-Routing, Pad-LED. Splitscreen-Rendering und Net-Mehrspieler-lokal ausdrücklich als eigenes Thema.

Warum Multi-Pad nach hinten rückt: Schritt 3 legt die Override-Schicht in `PlayerHost` an. Wird zuerst auf Mapping pro Spieler umgebaut, muss das Rebinding von Anfang an „pro Spieler" können, ohne dass es dafür schon einen Nutzer gibt. Andersherum lässt sich die Override-Schicht in Schritt 5 einfach pro Spieler vervielfachen (Slot im Override-Schlüssel ist ab Schritt 3 vorzusehen: `input.overrides.<slot>`, v1 nur Slot 0).

## 5. Offen / nicht verifiziert

- Real-HW bleibt offen wie in E4: Rumble-Stärke und -Gefühl, Trigger-Rumble nur auf DualSense/Xbox One, LED per Player-Index. Headless lässt sich nur der API-Durchstich prüfen.
- B1 ist durch Lesen des Loaders belegt, nicht durch einen Lauf. Der Test, der in Schritt 2 dazu kommt, muss rot anfangen.
