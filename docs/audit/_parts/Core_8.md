# Core Slice 8 – UIWidget/WidgetManager.h, Window/Window.cpp, Window/Window.h

## Zusammenfassung
Der Slice besteht aus dem Header der Widget-Verwaltung (1441 Zeilen, fast nur Deklarationen und Kommentare), der Window-Implementierung (521 Zeilen) und dem Window-Header. Der Code ist sauber und gut begründet kommentiert; die Window-Datei enthält keinen Hot-Path-Code außer `PollEvents`, `IsInBackground` und dem Hit-Test-Callback, alle drei sind unkritisch. Die relevanten Befunde betreffen (1) die Header-Abhängigkeiten von `WidgetManager.h` und `Window.h` (Compile-Zeit), (2) die Datenhaltung `std::vector<Instance>` mit linearer Suche und (3) duplizierte Env-Parsing-Logik in `Window.cpp`. Gesamt: 0 Hoch, 3 Mittel, 5 Niedrig. Alle Performance-Aussagen sind ohne Messdaten Schätzungen. Die Implementierung `WidgetManager.cpp` liegt nicht in diesem Slice; Aussagen dazu sind nur Stichproben (grep) und als solche markiert.

## Funde

### CORE-801 – `WidgetManager.h` zieht die komplette Widget-/HorizonCode-Welt in jeden Includer
- **ID**: CORE-801
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Performance (Build-Zeit) / Vereinfachung
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:2-16; Includer u.a. src/HE_Scene/include/HorizonScene/HorizonWorld.h:11, HorizonScene.h:57
- **Problem**: Der Header inkludiert sieben schwere Projekt-Header (`UIWidgetTree`, `UIElements`, `UIWidgetBinding`, `AppMenu`, `UIWindowFrame`, `HorizonCode`, `HorizonCodeRuntime`, `UIRenderObject`) und `<unordered_map>`. Er wird von `HorizonWorld.h` und `HorizonScene.h` eingebunden, also transitiv von praktisch jeder Datei in HE_Scene/Editor/Game. Die Klasse braucht die vollständigen Typen nur, weil `Instance`, `StateSnapshot`, `m_menuBar`, `m_theme`, `m_ownRuntime` (Wert-Member `HorizonCode::Runtime`, Z. 1317) und `m_catalog` inline im Header stehen. Das Kommentar-Muster aus `Window.h:13-17` („Declared, not included“) zeigt, dass das Problem im Haus bekannt ist.
- **Code-Auszug**:
```cpp
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
...
HorizonCode::Runtime  m_ownRuntime;        // fallback when none is injected
```
- **Lösung**: Pimpl-Light: `struct Instance`, `m_grabs`, `m_columnInfo`, Drag/Tooltip-Zustand und `m_ownRuntime` in `struct WidgetManager::Impl` (nur in der .cpp) verlagern; im Header nur öffentliche Typen behalten (`StateSnapshot`, `DesignTimeRun`, `TextEdit`, `NavDir`). `StateSnapshot` braucht `HorizonCode::Value` und `UIPropValue`; diese zwei Header bleiben, der Rest entfällt. Alternativ erst die einfache Stufe: in `HorizonWorld.h` nur `class WidgetManager;` vorwärts deklarieren und `std::unique_ptr<WidgetManager>` halten (nur sinnvoll, wenn `HorizonWorld` den Member nicht inline braucht; prüfen).
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkremental-Builds bei Änderungen an `UIWidgetTree.h`/`HorizonCode*.h` (derzeit rebuilden alle Scene/Editor-TUs). Konkrete Zahl nur per `-ftime-trace` / `ClangBuildAnalyzer` messbar.
- **Risiko / Verifikation**: Mittel – `HorizonWorld` und Tests verwenden `WidgetManager` per Wert; Pimpl ändert Move-/Copy-Verhalten (Kommentar Z. 1027 „stays movable“ beachten, d.h. Move-Ctor/Assign explizit in der .cpp definieren). Verifikation: voller Build aller Targets (Editor, Game, `he_tests`), `test_ui_widgets`, `test_horizoncode_*`. Vorher mit `-ftime-trace` messen, ob sich der Aufwand lohnt.

### CORE-802 – Widget-Instanzen als `std::vector<Instance>` mit Wert-Semantik und linearer ID-Suche
- **ID**: CORE-802
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:863-1021 (`Instance`), 1023-1025, 1312; Gegenstück src/HE_Core/src/UIWidget/WidgetManager.cpp:32-47, 948-958 (Stichprobe, nicht Teil des Slice)
- **Problem**: `Instance` ist ein sehr großer Wert-Typ (eigener `UIWidgetTree`, vier `unordered_map`, drei `vector`, mehrere `std::string`) und liegt direkt in `std::vector<Instance> m_instances`. Jeder `push_back` mit Reallokation und jedes `erase` (destroyWidget, destroyWidgetsOfWindow) verschiebt/kopiert ganze Bäume und macht alle `Instance*` ungültig – der Header muss dafür an mehreren Stellen Latch-Konstrukte begründen (`m_syncingLists`, Z. 1120-1132, Use-after-free-Szenario). `find(int id)` und `findByScript()` sind lineare Scans; `grep` zeigt 169 `find(`-Treffer in `WidgetManager.cpp`, darunter `takesInput` → `topGrabOf` → `find` pro Grab pro Eingabe-Eintrittspunkt (`topGrabOf` liest `find()` per `const_cast` im Schleifenkörper). Bei den realistischen Instanzzahlen (einige bis ~dutzend; List-Rows sind aber eigene Embeds/Skript-Instanzen, keine Instanzen) ist der Scan billig – das ist ein Robustheits- und Skalierungsproblem, kein bewiesener Engpass.
- **Code-Auszug**:
```cpp
std::vector<Instance> m_instances;          // Z. 1312
Instance* find(int id);                     // Z. 1023, linear in .cpp:34
```
- **Lösung**: `std::vector<std::unique_ptr<Instance>>` (oder `std::deque<Instance>`): stabile Adressen, `erase` verschiebt nur Zeiger, die Re-Entrancy-Gefahr bei `createWidget` aus einem Graph-Callback sinkt. `find` bleibt linear (Instanzen sind sortiert nach Erzeugung und `id` wird monoton vergeben – dann reicht `std::lower_bound` auf `id`, falls IDs nie wiederverwendet werden; verifizieren). Zusätzlich `const_cast` in `topGrabOf` durch eine const-Überladung/`findConst` ersetzen (Vereinfachung).
- **Erwarteter Effekt**: Schätzung: geringe Frame-Zeit-Änderung; Hauptgewinn ist Wegfall von Dangling-Pointer-Klassen (Instance* über Skript-Callbacks hinweg). Ohne Messung keine Zeitangabe.
- **Risiko / Verifikation**: Mittel. Code, der `Instance*` über mögliche `push_back`s hält, kann sich bisher implizit auf „kein Reallokieren“ verlassen oder wird heute schon durch Latches geschützt; Umstellung ändert Reihenfolge nicht. Absicherung: bestehende `test_ui_widgets` (nur gezielt lesen), ASAN-Lauf mit List-/Modal-Tests, Profiling mit 50+ Widgets vor dem Umbau.

### CORE-803 – Env-Auswertung in `Window.cpp` ist fünffach dupliziert, inklusive nachträglicher „Ursachen-Rekonstruktion“ für das Log
- **ID**: CORE-803
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:50-145 (bes. 59-61, 78-80, 95-101, 130-134)
- **Problem**: Die Regel „Zahl ≠ 0“ (`*s && std::strtoull(s,nullptr,10) != 0`) steht an fünf Stellen. Die beiden Lambdas in `hiddenWindowRequested()` und `backgroundFrameInterval()` rechnen dieselbe Entscheidung für den Log-Text ein zweites Mal aus (verschachtelte `?:`, Z. 78-80 und 130-134). Entscheidungsfunktion und Log-Begründung können auseinanderlaufen, sobald eine Bedingung ergänzt wird. Zudem ist `hiddenWindowFromEnv` mit drei positionellen `const char*`-Parametern gleichen Typs fehleranfällig (Vertauschung kompiliert).
- **Code-Auszug**:
```cpp
(hw && *hw) ? "HE_HIDDEN_WINDOW"
: (ex && *ex && std::strtoull(ex, nullptr, 10) != 0)
    ? "HE_EXIT_AFTER_FRAMES" : "HE_DUMP_PATH");
```
- **Lösung**: Kleine anonyme Hilfe `bool nonZeroNumber(const char*)` und die Entscheidungsfunktionen mit einem optionalen `const char** why`-Out-Parameter bzw. einem Rückgabe-Struct `{bool value; const char* reason;}` versehen; die Lambdas loggen nur noch `result.reason`. Die öffentlichen Signaturen (`HE_API`, im Test verwendet) bleiben als dünne Wrapper unverändert.
- **Erwarteter Effekt**: Weniger Code (~15-20 Zeilen), keine doppelte Wahrheit. Laufzeit irrelevant (einmaliger Start).
- **Risiko / Verifikation**: Niedrig. Verhalten identisch; Tests, die `hiddenWindowFromEnv`/`backgroundFrameIntervalFromEnv` aufrufen (grep in `tests/`), müssen unverändert grün bleiben.

### CORE-804 – `Window.h` zieht `<SDL3/SDL_events.h>` und `<functional>` in jeden Application-Includer
- **ID**: CORE-804
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Performance (Build-Zeit)
- **Ort**: src/HE_Core/include/Window/Window.h:6-7, 191; Includer: Application/Application.h:8 (13 Dateien nutzen `Window/Window.h` direkt, Application.h verbreitet es weiter)
- **Problem**: `SDL_events.h` wird nur für den `EventCallback`-Typ `std::function<void(const SDL_Event&)>` benötigt. Eine Vorwärtsdeklaration reicht (`union SDL_Event;`), `std::function` mit unvollständigem Parametertyp als Referenz ist zulässig; die Definition des Aufrufs steht in der .cpp. Das Fenster-Header-Kommentar zu `UIWindowHit` (Z. 13-17) wendet genau diese Technik bereits an.
- **Code-Auszug**:
```cpp
#include <SDL3/SDL_events.h>
...
using EventCallback = std::function<void(const SDL_Event&)>;
```
- **Lösung**: `union SDL_Event;` vorwärts deklarieren, Include in `Window.cpp` verschieben; Aufrufer, die `SDL_Event` selbst benutzen (Application.cpp, Editor), inkludieren SDL selbst. `SetEventCallback` ist inline mit `std::move` – braucht `<utility>`.
- **Erwarteter Effekt**: Schätzung: kleiner, nur per Build-Trace messbar.
- **Risiko / Verifikation**: Niedrig, aber Folgefehler durch transitiv genutzte SDL-Typen in Aufrufern möglich (Compile-Fehler, kein Laufzeitrisiko). Voller Build prüft das.

### CORE-805 – `PollEvents` ruft pro Event bis zu dreimal `SDL_GetWindowID(m_window)` auf
- **ID**: CORE-805
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance / Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:345-380
- **Problem**: In der Event-Schleife (Hot Path Input, einmal pro Frame, bei Mausbewegung viele Events) wird die Fenster-ID bis zu zweimal pro Event neu abgefragt, und die beiden Blöcke prüfen getrennt `event.type`. Die ID ist über die Lebenszeit des Fensters konstant. Zusätzlich sind die zwei `if`-Ketten für QUIT/CLOSE_REQUESTED und die Größen-Events eine einzige `switch (event.type)`-Struktur.
- **Code-Auszug**:
```cpp
if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
    event.window.windowID == SDL_GetWindowID(m_window))
...
event.window.windowID == SDL_GetWindowID(m_window) &&
```
- **Lösung**: `const SDL_WindowID myId = SDL_GetWindowID(m_window);` einmal vor der Schleife (oder als Member in `Init`), dann `switch (event.type)` mit drei Fällen.
- **Erwarteter Effekt**: Schätzung: unmessbar klein (SDL_GetWindowID ist ein Zeiger-Lookup mit Parameter-Check); Nutzen ist Lesbarkeit.
- **Risiko / Verifikation**: Niedrig. Verhalten identisch.

### CORE-806 – Doppelter `SDL_GetDisplayForWindow`/`SDL_GetDisplayName`-Aufruf und mehrfache Primary-Display-Abfragen in `Init`
- **ID**: CORE-806
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/src/Window/Window.cpp:236-251, 296-297
- **Problem**: `SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))` wird zweimal in einem Ausdruck ausgewertet (Prüfung und Wert); `SDL_GetPrimaryDisplay()` wird zweimal aufgerufen. Einmaliger Start, daher nur Lesbarkeit.
- **Code-Auszug**:
```cpp
SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))
    ? SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window)) : "?");
```
- **Lösung**: `const char* dn = SDL_GetDisplayName(...); ... dn ? dn : "?"`; `const SDL_DisplayID primary = SDL_GetPrimaryDisplay();`.
- **Erwarteter Effekt**: Lesbarkeit; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: Niedrig.

### CORE-807 – `WidgetManager.h`: Die Kommentarmasse überwiegt den Code und enthält Verwaiste/falsch platzierte Kommentare
- **ID**: CORE-807
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Core/include/UIWidget/WidgetManager.h:1266-1275, 1325-1330, 1137-1141
- **Problem**: Mehrere Kommentare stehen an der falschen Deklaration: der Tooltip-Block (Z. 1266-1274) beschreibt `m_tooltip*`, steht aber über `drawOpenDropdown`; „Starts true: the first frame…“ (Z. 1325-1327) steht über `m_theme`, gehört zu `m_visualDirty` (Z. 1345); der Tooltip-Kommentar nennt die Member erst 30 Zeilen später. Das ist reines Wartungsrisiko (der Code-Stil „erkläre das Warum“ ist ansonsten gut), der Header ist 1441 Zeilen lang und damit kaum zu überblicken. `liveIds()` (Z. 333) liefert eine neue `std::vector<int>` pro Aufruf; der Outliner ruft sie laut Kommentar pro Frame – siehe Messhinweis unten.
- **Code-Auszug**:
```cpp
// Starts true: the first frame after anything is created has never been
// drawn, so "nothing changed since the last draw" is false by construction.
// What roles resolve to. A copy rather than a pointer: ...
HE::UITheme           m_theme = HE::uiDefaultTheme();
```
- **Lösung**: Kommentare zu den zugehörigen Membern verschieben; den Header entlang der vorhandenen Abschnitte (Menu-Bar, Drag, Tooltips, Listen) in private Teil-Header oder in die Impl (CORE-801) aufteilen.
- **Erwarteter Effekt**: Wartbarkeit.
- **Risiko / Verifikation**: Keines (nur Kommentare).

### CORE-808 – `std::function` als Hit-Test-Callback, der laut Vertrag pro Mausbewegung im Event-Pump läuft
- **ID**: CORE-808
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Core/include/Window/Window.h:149-159; src/HE_Core/src/Window/Window.cpp:488-501; Verwendung src/HE_Game/src/GameApplication.cpp:716
- **Problem**: Der Hit-Test wird von der OS pro Mausbewegung aufgerufen; Pfad: C-Lambda → `std::function` → Lambda `[this]` → `frameHitAt`. Der Vertrag („must not allocate, log, or take a lock“) ist nur Kommentar. Der Mehraufwand eines `std::function`-Aufrufs ist im Vergleich zum Hit-Test selbst klein; das Ganze betrifft nur Borderless-Fenster. Der `(SDL_HitTestResult)(int)`-Cast ist durch die `static_assert`s (Z. 477-486) abgesichert. Kein echtes Problem, nur „unsicher / messen“.
- **Code-Auszug**:
```cpp
using HitTestCallback = std::function<UIWindowHit(int x, int y)>;
return (SDL_HitTestResult)(int)self->m_hitTest(area->x, area->y);
```
- **Lösung**: Nur falls ein Profil es zeigt: Funktionszeiger + `void* user` (wie SDL selbst) statt `std::function`. Sonst belassen.
- **Erwarteter Effekt**: Schätzung: nicht messbar.
- **Risiko / Verifikation**: Niedrig; ändert öffentliche API (`GameApplication.cpp:716` und evtl. Editor). Nicht empfohlen ohne Messdaten.

## Cross-Modul-Hinweise
- src/HE_Core/src/UIWidget/WidgetManager.cpp:32-47, 948-958 – lineare `find()`/`findByScript()` in `topGrabOf`/`takesInput`, die in jedem Input-Eintrittspunkt laufen (siehe CORE-802); `const_cast<WidgetManager*>(this)->find(...)` an Z. 954 und in `takesInput` (Z. 963) wegen fehlender Verwendung der const-Überladung (Z. 37 existiert bereits – die `const_cast` sind überflüssig).
- src/HE_Core/src/Application/Application.cpp:885-910 – Hintergrund-Throttle fragt `IsInBackground()` (SDL_GetWindowFlags) für Haupt- und alle Sekundärfenster jeden Frame; billig, aber `backgroundFrameInterval()` ist statisch gecacht, also korrekt. Schleife über `m_secondaryWindows` ruft `win->IsInBackground()` weiter, auch wenn `unseen` schon false ist (`unseen && ...` kurzschließt nur den Aufruf, nicht die Schleife) – vernachlässigbar.
- src/HE_Scene/include/HorizonScene/HorizonWorld.h:11 – bindet `WidgetManager.h` ein (siehe CORE-801).
- src/HE_Game/src/GameApplication.cpp:716 – Hit-Test-Lambda (CORE-808).

## Unsicher / Messung nötig
- Build-Zeit-Anteil von `WidgetManager.h` und `Window.h` (`-ftime-trace`, CORE-801/804).
- Tatsächliche Instanzzahlen und Aufrufhäufigkeit von `find()`/`topGrabOf` pro Frame in einem realistischen UI-Szenario (CORE-802); `WidgetManager.cpp` ist nicht Teil dieses Slice.
- Wie oft `liveIds()` (Outliner, `WidgetManager.h:333`) pro Frame gerufen wird und ob die Allokation relevant ist.
- Ob `Instance`-Zeiger heute über `createWidget`-Aufrufe aus Skript-Callbacks gehalten werden (Relevanz für CORE-802).
- Hit-Test-Overhead auf Borderless-Fenstern (CORE-808).

## Gelesen
Alle drei Dateien vollständig gelesen: ja (WidgetManager.h 1441 Zeilen, Window.cpp 521, Window.h 211); `WidgetManager.cpp` und `Application.cpp` nur per gezieltem Grep/Ausschnitt.
