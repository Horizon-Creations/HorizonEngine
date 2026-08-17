# Gamepad-Support (Block E4) — Umbauplan

Stand der Analyse: 17.08.2026, main. Ziel: Controller-Support (Xbox/PlayStation/Switch-Layouts via SDL3-Gamepad-API) durch alle Schichten — Rohgeräte, Action/Axis-Mapping, Assets + Editor-UI, Skript-API (Lua/Python/HorizonCode/C++), Kamera-Rig, Game- und PIE-Runtime.

## Ausgangslage

Die gute Nachricht: **Die Mapping-Schicht wurde bereits als Erweiterungspunkt für Gamepads entworfen.** `InputMapping.h:18-19` sagt es wörtlich („gamepad sticks and triggers belong in this list, they are just not built yet"), der Log-Kanal `Input` erwähnt Gamepads (`Log.h:49`). Es fehlt ausschließlich die Gerätestufe: `SDL_INIT_GAMEPAD` wird nie übergeben, kein einziges `SDL_Gamepad*`-Symbol existiert außerhalb von vendortem ImGui (Grep über `src/` ohne `vendor/`: null Treffer).

Der Datenfluss heute (Gamepad muss sich exakt einreihen):

```
SDL_Event → Window::PollEvents (Window.cpp:136) → Application-Callback (Application.cpp:87-117)
  → Input (m_keys / MouseFrame)                                  [Rohzustand]
  → InputMapping::tick(Input&, MouseFrame&)                      [Actions/Axes aus Assets]
  → PlayerHost::tick → "Input.<Name>.Pressed/Axis/Axis2D"-Events [HorizonCode]
  → Graph schreibt MovementComponent → MovementSystem → CharacterController
```

Parallel dazu der Immediate-Pfad für Skripte: `HE::api::input::pushSdlSnapshot()` (EngineApi.cpp:1482) pollt SDL direkt und speist `input.keyDown` & Co. — automatisch in Lua **und** Python sichtbar, weil `"input"` in `isScriptGroup` (EngineApi.cpp:2162) steht.

**Konsequenz:** Wer die Gerätestufe bis in `InputMapping` durchreicht, bekommt Character-Movement, HorizonCode-Graphen und MovementSystem **gratis** — dort wird nur Intent konsumiert, keine Geräte.

## Architektur-Entscheidungen (festgelegt, nicht TBD)

**1. `GamepadFrame` lebt in `Input`, kein neuer Parameter.**
`PlayerHost::tick` und `InputMapping::tick` nehmen schon `const Input&` — steckt der Gamepad-Zustand dort drin (analog `m_keys`/`m_mouse`), propagiert er ohne jede Signaturänderung durch Editor und Game. Struktur:

```cpp
struct GamepadFrame {
    bool  connected = false;
    float axes[SDL_GAMEPAD_AXIS_COUNT] = {};   // Sticks [-1,1], Trigger [0,1], deadzone-roh
    bool  buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
};
```

**2. Stick-Look ist eine Rate, Maus-Look ein Displacement — nicht vermischen.**
Maus: Grad **pro Pixel**, nie dt-skaliert (`CameraRigComponent::sensitivity = 0.12f`). Rechter Stick: Grad **pro Sekunde**, dt-skaliert. Stick-Werte durch `MouseFrame` zu schleusen wäre falsch (framerate-abhängige Drehgeschwindigkeit). Daher bekommt `CameraRigController::update` (CameraRigController.h:55) einen erweiterten Look-Input:

```cpp
struct LookInput {
    MouseFrame mouse;            // Displacement, Grad/Pixel
    float stickX = 0, stickY = 0; // Rate [-1,1], Grad/Sekunde * stickSensitivity * dt
    float dt = 0;
};
```

`CameraRigComponent` bekommt ein separates `float stickSensitivity` (Grad/Sek bei Vollausschlag, Default ~180). Die dt-Skalierung macht **der Rig-Controller**, nicht der Aufrufer.

**3. PlayerHost-Axis-Events bleiben un-dt-skaliert — Sticks werden engine-seitig NICHT vorskaliert.**
`PlayerHost.cpp:143-147` dokumentiert bewusst: Axis-Werte sind Rohintensität, Graphen skalieren selbst (Movement via MovementSystem ist ohnehin geschwindigkeitsbasiert). Ein Stick liefert also wie eine Taste einen Zustandswert in [-1,1] — konsistent. Nur Look-Graphen, die direkt Rotation schreiben, müssen dt selbst anwenden; das gilt heute schon für Maus-Deltas und wird in der Doku vermerkt.

**4. Deadzone: radial, in der Rohstufe konfiguriert, in der Mapping-Stufe angewendet.**
- 2D-Sticks: **radiale** Deadzone (Länge des Vektors, nicht pro Achse — sonst „kreuzförmige" tote Zonen und Diagonal-Snapping), danach Rescale auf [0,1] ab Deadzone-Rand.
- Trigger: skalare Deadzone.
- Defaults: Stick 0.15, Trigger 0.05; als Konstanten in `Input`, später konfigurierbar.
- **Warum zwingend:** `PlayerHost::tick` feuert `Axis`/`Axis2D` **jeden Frame bedingungslos** (PlayerHost.cpp:142-157). Ein ungefilterter Stick-Drift heißt: permanente Nonzero-Events in jeden Graphen. `Input` liefert deshalb beides: `gamepad().axes[...]` roh (für Skript-Queries/Kalibrier-UI) und `gamepadAxisFiltered(...)` mit Deadzone (was `InputMapping` konsumiert).

**5. Multi-Controller: alle Pads verschmelzen, Single-Player.**
E4-Scope ist ein Spieler. Alle verbundenen Pads werden in einen `GamepadFrame` gemerged (letzter Nonzero gewinnt pro Achse, Buttons ODER-verknüpft) — so funktioniert „Pad auf dem Schoß wechseln" ohne Auswahl-UI. Per-Player-Zuordnung (Splitscreen, N4a-Replikation) ist explizit **nicht** Teil von E4; die interne Struktur (SDL-Instance-ID → Slot-Map) wird aber so gebaut, dass sie später aufgeteilt werden kann.

**6. Asset-Schema: additiv, alte JSONs parsen unverändert.**
`positive`/`negative` in Axis-Zeilen sind Scancode-*Namen* (`SDL_GetScancodeFromName`) — Gamepad-Buttons dürfen diesen Namensraum nicht teilen („A" ist die Tastatur-Taste). Schema-Delta:
- Action-Bindings: neben `keys[]` neu `gamepadButtons[]` (SDL-Button-Namen via `SDL_GetGamepadStringForButton`, z. B. `"south"`, `"left_shoulder"`).
- Axis-Zeilen: `source` bekommt neue Werte `"GamepadLeftX" | "GamepadLeftY" | "GamepadRightX" | "GamepadRightY" | "GamepadLeftTrigger" | "GamepadRightTrigger"`; zusätzlich optional `positiveButton`/`negativeButton` für Button-als-Achse (D-Pad-Movement).
- Unbekannte `source`-Strings fallen wie bisher auf `Key` zurück (`axisSourceFromName`, InputAssets.cpp:47-53) — alte Engine liest neue Assets ohne Crash, neue Engine liest alte Assets identisch.

**7. `axisSourceIsDelta` = false für alle Gamepad-Quellen.**
Sticks/Trigger sind Haltezustände wie Tasten (clampen, nicht akkumulieren) — die Clamp-Regel in InputMapping.cpp:73 gilt für sie. Das ist der eine leicht zu übersehende Semantik-Punkt im bestehenden Code.

**8. Snapshot-Falle aus dem E4-Audit vermeiden.**
`input::pushSdlSnapshot()` nullt heute das Maus-Delta zugunsten des Freiflug-Akkumulators (Ownership-Konflikt, bekannt aus dem Masterplan-Audit). Die Gamepad-Erweiterung des Snapshots pollt **nicht selbst SDL**, sondern bekommt den fertigen (gemergten, rohen) `GamepadFrame` von der App gepusht — eine Quelle der Wahrheit, kein zweiter Ownership-Konflikt.

**9. ImGui: `NavEnableGamepad` bleibt AUS.**
Weder gesetzt noch geplant (EditorApplication.cpp:712-714 setzt nur Keyboard/Docking/Viewports). Dadurch konkurriert ImGui nie um Gamepad-Events — PIE bekommt sie exklusiv. Wird Editor-Navigation per Pad je gewünscht, braucht es denselben `m_isPlaying`-Bypass wie die Tastatur (EditorApplication.cpp:5565); bis dahin: bewusst aus, Kommentar an die Stelle.

**10. Event-Routing: Gamepad-Events gehen den „Maus-Weg" (ungated).**
`Application.cpp:112` verarbeitet Mausbewegung VOR dem `OnEvent`-Consume-Gate; Tastatur dahinter. Gamepad-Events gehören auf den ungegateten Pfad (`ProcessGamepadEvent` neben `ProcessMouseEvent`) — `io.WantCaptureKeyboard` darf keinen Stick besitzen. Die Play-Gates sitzen weiter hinten (CP5): der Editor speist Gamepad nur bei `m_isPlaying` in PlayerHost/Snapshot ein, das Game immer.

## Checkpoints

### CP0 — Rohgeräte-Schicht (HE_Core)
- `Window.cpp:20`: `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)`.
- `Input.h/.cpp`: `GamepadFrame` + `m_gamepad`; neue `HE_API`-Queries `gamepad()`, `gamepadAxisFiltered(axis)`, `isGamepadButtonDown(button)` (Windows-CI-Regel: HE_Core braucht explizites `HE_API`).
- Neue Datei `HE_Core/src/Application/GamepadState.cpp` (oder in `Input.cpp`): Verwaltung `SDL_JoystickID → SDL_Gamepad*`, Öffnen bei `SDL_EVENT_GAMEPAD_ADDED`, Schließen bei `_REMOVED`, Merge aller Pads in den Frame. Zustand per `SDL_GetGamepadAxis`/`SDL_GetGamepadButton` einmal pro Frame gepollt (in `Input::EndFrame`-Nähe bzw. eigenem `PollGamepads()` aus `Application`), nicht event-akkumuliert — SDL hält den Zustand, wir lesen ihn; Events nur für Hot-Plug.
- Hot-Plug-Logging über den existierenden `Input`-Log-Kanal (`HE_LOG_INFO(Input, "Gamepad connected: %s", SDL_GetGamepadName(...))`).
- Routing in `Application.cpp:87-117`: `SDL_EVENT_GAMEPAD_ADDED/_REMOVED` ungated behandeln (Punkt 10).
- Tests: `GamepadFrame`-Merge-Logik + radiale Deadzone als reine Funktionen testbar (`test_input_gamepad.cpp`).

### CP1 — Mapping-Schicht (HE_Core)
- `AxisSource` erweitern: `GamepadLeftX/LeftY/RightX/RightY/LeftTrigger/RightTrigger`; `axisSourceIsDelta` → false für alle (Punkt 7).
- `ActionBinding`: von `SDL_Scancode key` zu tagged Binding (`enum class BindingDevice { Key, GamepadButton }` + Wert) — kleiner Struct, alle Brace-Init-Stellen prüfen.
- `AxisBinding`: optionale `SDL_GamepadButton positiveButton/negativeButton` (Default `SDL_GAMEPAD_BUTTON_INVALID`), angehängt hinter `source` (bestehende Brace-Inits bleiben gültig — dieselbe Rückwärts-Regel wie beim `source`-Feld selbst, InputMapping.h:41-43).
- `InputMapping::tick` (InputMapping.cpp:35-74): Action-Schleife prüft zusätzlich Gamepad-Buttons; `component`-Lambda bekommt die sechs neuen `AxisSource`-Cases (lesen `input.gamepadAxisFiltered(...)`; Trigger auf [0,1] belassen, `scale` wirkt wie gehabt).
- Tests in `test_inputmapping.cpp` erweitern: synthetische `GamepadFrame`s injizieren (Stick auf Achse, Deadzone-Kante, Trigger, Button-als-Achse, Key+Stick gemischt auf derselben Achse → Clamp).

### CP2 — Assets + Editor-Binding-UI
- `InputAssets.cpp`: `axisSourceName`/`FromName` um die sechs Strings erweitern; Parser (`applyInputMappingContext`, :55-129) liest `gamepadButtons[]` und `positiveButton`/`negativeButton` (Button-Namen via `SDL_GetGamepadButtonFromString`; unbekannt → ignorieren, wie bei Keys).
- `InputAssetPanel.cpp`: 
  - Source-Combo (:553) um Gamepad-Quellen erweitern; bei Gamepad-Stick/Trigger-Quellen Key-Felder ausblenden (bestehender `keyed`-Guard :567 wird dreiwertig: Key-Felder / nichts / Button-Picker).
  - Action-Tabelle: neben `+ Key` ein `+ Gamepad-Button` mit Dropdown (SDL-Button-Namen) **und** Live-Capture („Taste am Pad drücken") — Capture liest `Input::gamepad()` direkt, funktioniert also nur mit angeschlossenem Pad, UI zeigt sonst „kein Gamepad verbunden".
- Doku-Seite in den Engine-Docs ergänzen (Binding-Referenz: Button-/Achsennamen-Tabelle).

### CP3 — Skript-API (HE_Scene, EngineApi)
- Snapshot-Struct (EngineApi.cpp:1469) um Gamepad-Zustand erweitern; neuer App-Hook `input::pushGamepadSnapshot(const float* axes, const bool* buttons, bool connected)` — von der App gespeist, kein SDL-Poll im Snapshot (Punkt 8).
- Neue Registry-Rows (EngineApi.cpp:1773ff): `input.gamepadConnected()`, `input.gamepadButton(name)`, `input.gamepadAxis(name)`, Display-Namen bei :2089ff. Durch `isScriptGroup` sofort in Lua (`horizon.input.gamepadAxis("leftx")`) und Python verfügbar — null Binding-Code. **Umgesetzt: gefilterte Werte, nicht rohe** — Skripte implementieren Gameplay, und ein ruhender Stick, der im Skript nie 0.0 liest, wäre die Drift-Falle aus Punkt 4 durch die Hintertür; rohe Werte bleiben in C++ über `Input::gamepad()` abfragbar (Kalibrier-UI). Namen sind SDLs Mapping-Strings (`"a"`, `"leftx"` — **nicht** `"south"`: SDL3s Tabelle nutzt die Legacy-Mapping-Namen im Xbox-Layout).
- HorizonCode: dieselben Rows erscheinen automatisch als Nodes; zusätzlich prüfen, dass die `Input.<Action>.*`-Events (der eigentliche Empfehlungsweg) unverändert funktionieren — Gamepad kommt dort via CP1 gratis an.
- `docs/horizoncode-reference.md` um die neuen Nodes ergänzen.

### CP4 — Kamera-Rig + PlayerHost
- `CameraRigController::update` auf `LookInput` umstellen (Punkt 2); beide Call-Sites (GameApplication.cpp:832-849, EditorApplication.cpp:4785) anpassen. `CameraRigComponent`: `stickSensitivity` (+ Serialisierung + Details-Panel + optional Invert-Y für Stick).
- `PlayerHost::tick`: keine Signaturänderung nötig (liest `Input&`) — nur sicherstellen, dass Axis2D-Kombination Key- und Stick-Quellen sauber mischt (Clamp) und die Deadzone verhindert, dass Ruhe-Drift Events „belebt". Ein Guard ist NICHT nötig (Axis feuert designgemäß auch bei 0), aber der Wert muss bei losgelassenem Stick exakt 0.0 sein.
- Fly-Cam bleibt Maus/Tastatur-only (Editor-Werkzeug, kein Spielpfad).

### CP5 — Runtime-Verdrahtung + Settings
- `GameApplication`: pro Frame `input().PollGamepads()`-Ergebnis ungated nutzen; `pushGamepadSnapshot` **bedingungslos** (analog :913-914); Look-Input aus rechtem Stick an `updateCameraController`.
- `EditorApplication` (PIE): Snapshot/PlayerHost-Speisung **nur bei `m_isPlaying`** — anders als Maus nicht an `m_playMouseCaptured` gebunden (ein Pad hat keinen Cursor-Konflikt mit ImGui; wer mit Pad spielt, soll nicht erst Esc/Capture togglen müssen). Kommentar an die Stelle, warum die Gates verschieden sind.
- `EditorSettingsPanel`: neue Zeile im `Viewport`- oder eigenem `Input`-Abschnitt nach dem Muster der `pointerinput`-Row (:425-440): verbundene Pads anzeigen (Name via SDL), Deadzone-Slider (Stick/Trigger), Test-Anzeige (Live-Achsenwerte). Persistenz via `setCustomConfigEntry`.
- Roadmap-Eintrag „Gamepad Support" auf der Website erst **nach** Fertigstellung auf `done` heben (+ Devlog), nicht jetzt.

## Verifikation (ohne physisches Pad in der Sandbox)

- **Primär: synthetische Injektion.** `GamepadFrame` ist ein POD — Unit-Tests befüllen ihn direkt und treiben `InputMapping::tick`/`PlayerHost::tick` (Muster von `test_inputmapping.cpp`/`test_editor_input.cpp`). Deckt Mapping, Deadzone, Events, Asset-Parsing vollständig ab.
- **End-to-End: SDL3 Virtual Gamepad — GEKLÄRT, funktioniert headless** (auf macOS verifiziert). Der Test lebt in `test_input_gamepad.cpp` („End-to-end: virtual pad …"): Attach → Hot-Plug-Event → `Input` öffnet das Pad → `PollGamepads` → `InputMapping`, plus Unplug-nullt-den-Frame; skippt sauber, wo das Subsystem fehlt (CI ohne Gerätesupport).
- **Real-HW-Verify bleibt offen** (wie GL-Runtime): Xbox-/DualSense-Pad am Mac, PIE + gepacktes Spiel, Stick-Look-Gefühl (`stickSensitivity`-Default), Trigger, Hot-Plug im laufenden Spiel. Explizit als offener Punkt führen, nicht versprechen.

## Explizit NICHT in E4

- Rumble/Haptik, Gyro, Touchpad, LED (SDL kann es; eigener Folgeschritt).
- Per-Player-Pad-Zuordnung / Splitscreen (wartet auf Gameplay-Replikation N4a).
- ImGui-Editor-Navigation per Pad.
- In-Game-Remapping-UI für Spieler — es gibt noch kein Runtime-Settings-Menü (GameApplication.cpp:882-885 sagt das selbst); Remapping bleibt Editor-Sache (Assets), Spieler-Remapping kommt mit einem künftigen Game-Settings-System.
- D-Pad-als-Cursor für Widgets (UI-Interaktion bleibt Maus/Touch).

## Reihenfolge & Aufwand

CP0 → CP1 → CP2 → CP3 → CP4 → CP5; CP2 und CP3 sind nach CP1 unabhängig voneinander. Jeder Checkpoint baut und testet für sich (Standing Order: pro fertigem Checkpoint commit + push). Kern-Risiko ist nicht Code-Menge, sondern Gefühl (Sensitivity/Deadzone-Defaults) — genau das ist der Real-HW-Teil.
