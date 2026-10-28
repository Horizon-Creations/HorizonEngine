# Game Slice 1 – src/HE_Game/src (GameApplication, main, AppMacMenu, AppNotify, EmbeddedPakKey)

## Zusammenfassung

Der Slice ist fast vollständig `GameApplication.cpp` (3800 Zeilen); die übrigen Dateien (main, EmbeddedPakKey, AppNotify, AppMacMenu) sind klein und unauffällig. Der Per-Frame-Pfad (`OnRender`, Z. 2997–3628) ist funktional sauber gegliedert (Zeitbudget fürs Asset-Streaming, Fixed-Step-Physik, App-Mode-Gates), enthält aber pro Frame eine große Menge wiederholter, unveränderter Arbeit: rund 30 `GlobalState`-Config-Lookups mit String-Keys, ein Vollvergleich des Zell-Manifest-JSON, mehrfache Kamera-View-Iterationen und ein unbedingtes Neusetzen aller Renderer-Settings. Beim Start läuft alles synchron im Main-Thread in einer 890 Zeilen langen `OnInit`. Funde: 0 Hoch, 5 Mittel, 8 Niedrig. Alle Performance-Aussagen sind ohne Messdaten Schätzungen; die Per-Frame-Funde sind einzeln klein (Mikrosekunden), summieren sich aber in jedem ausgelieferten Frame.

## Funde

### GAM-101 – Renderer-Settings werden jeden Frame aus der Config gelesen und neu gepusht
- **ID**: GAM-101
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: src/HE_Game/src/GameApplication.cpp:3422–3601 (insb. 3475–3574)
- **Problem**: Jeder Frame führt ca. 35 `GlobalState::getInstance().getCustomConfigX("Key", default)`-Aufrufe aus. Jeder konstruiert einen `std::string` aus dem Literal (Keys wie `"GlobalIlluminationEnabled"`, `"SpecularAAStrength"`, `"GIReflMaxRoughness"` überschreiten SSO, also Heap-Allokation) und macht `contains` + `operator[]` auf einem `nlohmann::json`-Objekt (je zwei Map-Lookups, siehe `GlobalState.cpp:893–905`). Die Werte ändern sich im ausgelieferten Spiel nie (Config wird nur beim Start gelesen, vgl. Kommentar Z. 505 "the file does not change mid-session"); danach folgen ~13 Setter-Aufrufe am Renderer, davon mehrere mit Capability-Abfragen. Ausnahme-Kandidat: `makeEnvironmentSettings` (muss laut Kommentar Z. 3580 jeden Frame laufen, weil es den Tag/Nacht-Zyklus fortschreibt) und `SetShadowSettings`, die aus `m_projectSettings` kommen.
- **Code-Auszug**:
```cpp
r->SetBloomSettings(IRenderer::BloomSettings{
    GlobalState::getInstance().getCustomConfigBool("BloomEnabled", true),
    static_cast<float>(GlobalState::getInstance().getCustomConfigFloat("BloomThreshold", 1.0f)), ...
r->SetGIReflectionSettings(gr);   // ... bis r->SetRenderPath(...)  (Z. 3475–3574)
```
- **Lösung**: Die statischen Settings (Bloom, SSAO, DoF, MotionBlur, GI, SSR, Occlusion, GI-Refl, AA, RenderPath, SwapchainPostProcessing, Shadow) einmal in einer Funktion `pushStaticRendererSettings(IRenderer&)` bauen, nach dem Renderer-Create / in `OnInit` aufrufen und nur bei Bedarf wiederholen (Szenenwechsel, Settings-Änderung über `HE::api::settings`, Backend-Reset). Im Frame bleiben nur Environment und Ripples. Dasselbe gilt für den App-Mode-Zweig (3438–3463), der jeden Frame 9 "Off"-Setter schickt. Das bündelt außerdem die ~100 Zeilen Einzelaufrufe zu einer Funktion (Vereinfachung von `OnRender`).
- **Erwarteter Effekt** (Schätzung): einige Mikrosekunden CPU pro Frame und ca. 10+ Heap-Allokationen/Frame weniger; vor allem aber eine kürzere, lesbare `OnRender`. Ob die Backends die Setter intern mit Änderungserkennung behandeln oder pro Frame Pipelines/Targets invalidieren, ist unbekannt (siehe Unsicher).
- **Risiko / Verifikation**: Verhalten bleibt gleich, solange der Push nach jeder Quelländerung erneut passiert. Risiken: (a) Renderer wird neu erzeugt/zurückgesetzt, (b) ein Backend verlässt sich auf den Push pro Frame (Kommentar Z. 3469 "Every frame, like everything here — the call only stores a flag"), (c) Capability-Werte stehen erst nach Renderer-Init fest. Absichern: Headless-Lauf mit `HE_HIDDEN_WINDOW=1`, Vergleich eines Frames vor/nach (Software-/OpenGL-Backend), `tests/` für Config→Settings-Mapping ergänzen, falls extrahiert.

### GAM-102 – Zell-Manifest-JSON wird jeden Frame per Vollvergleich geprüft
- **ID**: GAM-102
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Game/src/GameApplication.cpp:1518–1528 (Aufruf Z. 3290); HorizonWorld.h:106–107, 313
- **Problem**: `updateCellStreaming` läuft jeden Frame und vergleicht `manifestJson != m_cellManifestJson`. Bei einem Cell-Streaming-Szenario ist das Manifest ein JSON-String beliebiger Größe (potenziell hunderte KB bei vielen Zellen). Sind beide gleich (der Normalfall, praktisch jeder Frame), muss `std::string::operator!=` die gesamte Länge vergleichen (memcmp bis zum Ende). Zusätzlich hält `m_cellManifestJson` eine zweite Kopie des Manifests nur für diesen Vergleich.
- **Code-Auszug**:
```cpp
const std::string& manifestJson = m_world->cellManifestJson();
if (m_cellWorld != m_world.get() || manifestJson != m_cellManifestJson)
```
- **Lösung**: Im `HorizonWorld` einen Revisionszähler führen (`setCellManifestJson` erhöht `m_cellManifestRevision`), im Game nur `uint64_t m_cellManifestRev` vergleichen. Die String-Kopie `m_cellManifestJson` entfällt. `HorizonWorld::clear()` (HorizonWorld.cpp:523) muss ebenfalls hochzählen.
- **Erwarteter Effekt** (Schätzung): ohne Manifest nichts (beide leer, Länge 0); mit großem Manifest ca. Größe/10 GB/s pro Frame, z. B. 500 KB ≈ 50 µs; Speicher −1 Manifest-Kopie.
- **Risiko / Verifikation**: Gering; Revision muss an jeder Stelle steigen, die das Manifest ändert (Serializer, CellSplit). Test: Szene mit Manifest laden, Manifest ändern, prüfen, dass der Streamer neu startet (vgl. vorhandene Cell-Tests in `tests/`).

### GAM-103 – `OnInit` (890 Zeilen) und `OnRender` (630 Zeilen) sind monolithisch; Host-Lambdas inline
- **ID**: GAM-103
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: GameApplication.cpp:592–1484 (OnInit), 2997–3628 (OnRender), 2602–2984 (OnEvent, 380 Zeilen), 973–1294 (Services/`g_host`-Lambdas inline in `OnInit`)
- **Problem**: Drei Funktionen mit je mehreren hundert Zeilen und bis zu 6 Ebenen Verschachtelung (z. B. Z. 1177–1213 `createObject`: if/if/if im Lambda in einem Block in `OnInit`). Startreihenfolge-Abhängigkeiten ("BEFORE fireInit", "AFTER the player spawns") stehen nur in Kommentaren und lassen sich bei dieser Länge kaum prüfen. `OnRender` mischt Event-Zustellung (Tray, HTTP, Dateiwatcher, Timer), Input-Snapshot, Physik, Szenensysteme, Renderer-Push und Netz.
- **Code-Auszug**:
```cpp
void GameApplication::OnInit() { ... }   // Z. 592–1484
void GameApplication::OnRender(float deltaTime) { ... }   // Z. 2997–3628
```
- **Lösung**: Rein mechanisches Extract-Method, Reihenfolge unverändert: `OnInit` → `initWindowAndMouse()`, `mountContent()`, `loadTypesAndCompiledClasses()`, `initTheme()`, `bindHostServices()` (die `g_host`-Lambdas), `loadStartupScene()`, `startHosts()`, `initAudio()`, `loadGameLogic()`. `OnRender` → `deliverHostEvents()` (Z. 3040–3113), `pushInputSnapshot()` (3137–3181), `streamAssets()` (3212–3231), `simulate(gameDt)` (3237–3290), `tickWorldSystems()` (3358–3412), `pushRendererSettings(gameDt)`. Die Textvorkommen von `OnEvent` (zwei fast identische Text-Edit-Switches, siehe GAM-110) in eine Hilfsfunktion.
- **Erwarteter Effekt**: Lesbarkeit/Wartbarkeit; keine Laufzeitänderung (Schätzung: neutral, ggf. Inlining).
- **Risiko / Verifikation**: Gering bei reinem Verschieben; Reihenfolge-Fehler wären sichtbar nur im Spiel. Absichern: vorhandene Game-/Headless-Smoke-Läufe (`docs/headless-runs.md`), Log-Reihenfolge von `OnInit` vorher/nachher diffen.

### GAM-104 – Startpfad: alles synchron im Main-Thread, `project.hcfg` doppelt geladen, Typ-Assets einzeln synchron geladen
- **ID**: GAM-104
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: GameApplication.cpp:462–483 (Peek im Konstruktor) und 656 (zweites `ProjectConfigLoader::load`), 728 (`scanContentDirectory`), 805–824 (Typ-Index), 832–839 (`compiledClasses().load`), 884–907 (GameInstance-Graph), 1321 (`loadFromMemory` Startszene)
- **Problem**: Time-to-first-frame ist die Summe sequenzieller Main-Thread-Arbeit: (1) `project.hcfg` wird im Konstruktor (`peek`) und erneut in `OnInit` komplett gelesen und geparst. (2) `scanContentDirectory()` läuft auch im Pak-Build; laut Kommentar Z. 726 ein No-op, wenn `Content/` fehlt, aber bei Loose-/WIP-Builds ein rekursiver Verzeichnislauf vor dem ersten Frame. (3) Der Typ-Index lädt jede Struct/Enum-Definition per `loadAsset` synchron in einer Schleife (815). (4) Die HorizonCodeGen-Bibliothek wird per `dlopen`/`LoadLibrary` geladen (833), die GameLogic-Bibliothek ebenfalls (1428, inkl. `std::filesystem::exists`). (5) Die Startszene wird vollständig deserialisiert, bevor das erste Frame erscheint (Splash ist optional, Z. 561). Es gibt in diesem Slice keine Stelle, die nebenläufig lädt außer dem Integritäts-Hash (794) und dem Asset-Streaming.
- **Code-Auszug**:
```cpp
if (ProjectConfigLoader::load(fs::path(baseRaw), peek) && peek.appMode) ...   // Z. 465
const bool haveConfig = ProjectConfigLoader::load(exeDir, m_config);          // Z. 656
for (const auto& e : idx) if (e.is_string() && !(contentManager().loadAsset(e.get<std::string>()) == HE::UUID{})) ++types;
```
- **Lösung**: (a) Den Peek-Wert in einen Member `ProjectConfig m_config` laden und in `OnInit` wiederverwenden statt neu zu laden (der Kommentar Z. 457 begründet nur, dass `m_appMode` früh bekannt sein muss, nicht das zweite Laden). (b) Zuerst messen (siehe Unsicher), dann ggf. Typ-Assets parallel über `loadAssetAsync` anstoßen und nur auf die Registry warten, bevor Skripte starten. (c) `scanContentDirectory` nur, wenn `mountPak` fehlschlägt oder Overlay-Content erwartet wird. Nichts davon ändert das Ergebnis, solange die Reihenfolge "Typen vor Skript-/Graph-Bootstrap" gilt (Kommentar Z. 799).
- **Erwarteter Effekt** (Schätzung, ohne Messdaten): (a) klein (ein hcfg-Parse, <1 ms); (b)/(c) abhängig von Typ-Anzahl und Content-Größe, evtl. zweistelliger ms-Bereich bei Loose-Builds.
- **Risiko / Verifikation**: (a) gering. (b) mittel – `TypeRegistry` ist process-global, Thread-Sicherheit prüfen (Main-Thread-Registrierung über `pollAsyncResults` erhalten). Messen mit `HE_PROFILE_SCOPE_N` um OnInit-Blöcke, Startlog-Zeitstempel (`HE_LOG_INFO`) vor/nach.

### GAM-105 – Mehrfache Kamera-Suche per ECS-View pro Frame
- **ID**: GAM-105
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: GameApplication.cpp:3282–3288, 3361–3370, 1604–1609 (und `ensureDefaultCamera` 1491)
- **Problem**: Pro Frame wird `view<TransformComponent, CameraComponent>()` bis zu dreimal getrennt iteriert, um die "erste" Kamera zu finden: Floating Origin (3282), Camera-Position für `tickWorld` (3361), Zell-Streaming (1604). Jedes Mal folgt `worldPositionOf` (Parent-Kette). Bei vielen Kamera-Entities (Cutscenes, Render-Targets) wird dabei die Kamera der Extraktion (`isMain`) nicht einmal bevorzugt – es wird die erste in View-Reihenfolge genommen, was nicht notwendig dieselbe wie die gerenderte ist (Verhaltensfrage, nicht ändern ohne Test).
- **Code-Auszug**:
```cpp
for (auto e : m_world->registry().view<TransformComponent, CameraComponent>())
{ camPos = HE::worldPositionOf(*m_world, e); break; }
```
- **Lösung**: Hilfsfunktion `Entity firstCamera() const` einmal pro Frame aufrufen (Ergebnis lokal halten; nach Floating-Origin-Shift neu auflesen, da sich die Position ändert), `worldPositionOf` nur einmal berechnen und an `updateFloatingOrigin`, `updateCellStreaming`, `tickWorld` weiterreichen. Verhalten identisch (gleiche View-Reihenfolge).
- **Erwarteter Effekt** (Schätzung): <10 µs/Frame bei wenigen Kameras; vor allem Duplikat-Code (3 Kopien derselben Schleife).
- **Risiko / Verifikation**: Gering; Position muss nach `updateFloatingOrigin` neu berechnet werden. Test: Szene mit Floating Origin, Kamera weit entfernt, Position vor/nach vergleichen.

### GAM-106 – `HostCtxParts`/`apiCtx`: ca. 40 `std::function`-Felder werden bei jeder `callApi` kopiert
- **ID**: GAM-106
- **Kategorie**: B
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: GameApplication.cpp:102–157 (Struct), 322–368 (`apiCtx`), 1275–1292 (`svc.callApi`), 1799, 2008, 2058, 2071
- **Problem**: `apiCtx()` baut bei jedem Aufruf ein vollständiges `HE::api::Ctx` und kopiert dabei ~35 `std::function`-Objekte aus `g_host` (jede Kopie kann bei Lambdas mit Captures eine Heap-Allokation bedeuten; `createObject`, `addMenuItem` u. a. haben `this`-Captures, die klein genug für SBO sind, aber das Kopieren bleibt je ein Funktionszeiger-Aufruf der Type-Erasure-Schicht) plus zwei frische Lambdas für `notify`. `svc.callApi` ist der Pfad für **jeden EngineCall-Knoten eines HorizonCode-Graphs**, also potenziell hunderte Male pro Frame. Dazu kommen `HE::api::find(id)` mit einem String-Key pro Aufruf (Z. 1278) und der Rückgabe-`std::vector<Value>`.
- **Code-Auszug**:
```cpp
svc.callApi = [this](HorizonCode::InstanceId self, const std::string& id, const std::vector<HorizonCode::Value>& args) {
    const HE::api::ApiFn* fn = HE::api::find(id);
    ...
    HE::api::Ctx c = apiCtx(m_world.get(), m_physicsWorld.get(), &contentManager(), self);
    return fn->invoke(c, args);
```
- **Lösung**: Ein persistentes `HE::api::Ctx m_ctx` Member, dessen Host-Teil einmal in `OnInit` gefüllt wird; pro Aufruf nur die Szenen-Handles (`world`, `physics`, `content`, `self`) setzen (vier Zuweisungen) – oder `Ctx` hält `const HostCtxParts*` statt Kopien. Das erhält den Grundgedanken ("exactly ONE place builds a Ctx") und ändert das Verhalten nicht, solange `world/physics` pro Aufruf frisch gesetzt werden. Die Änderung berührt `HE_Scene` (`EngineApi.h`, `Ctx`-Definition), also modulübergreifend.
- **Erwarteter Effekt** (Schätzung): pro `callApi` mehrere hundert Nanosekunden bis wenige µs gespart (35 std::function-Kopien); bei 100–1000 EngineCalls/Frame messbar. Ohne Profil als "unsicher" zu werten.
- **Risiko / Verifikation**: Wiedereintritt: `callApi` kann verschachtelt aufgerufen werden (Graph ruft Skript ruft Graph); ein gemeinsamer mutabler `Ctx` würde dann `self` überschreiben. Daher entweder `Ctx` mit Pointer auf Host-Teil oder `self` als Parameter. Test: HorizonCode-Graph mit verschachteltem EngineCall (vorhandene Tests in `tests/` für Engine-API ergänzen).

### GAM-107 – Menüleiste: Copy-Modify-Set der gesamten Menüstruktur pro Aufruf
- **ID**: GAM-107
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: GameApplication.cpp:1088–1120
- **Problem**: `addMenu`, `addMenuItem`, `addMenuSeparator` kopieren das komplette `std::vector<HE::AppMenu>` (inkl. aller Items/Strings) aus dem WidgetManager, ändern eine Stelle und übergeben es per `setMenuBar`. Ein Menü mit N Einträgen kostet O(N²) Kopien beim Aufbau, außerdem schließt `setMenuBar` ein geöffnetes Menü (Kommentar Z. 1121). Läuft nur beim Aufbau durch den Graph, nicht pro Frame.
- **Code-Auszug**:
```cpp
std::vector<HE::AppMenu> menus = m_widgets.menuBar();
for (HE::AppMenu& m : menus) if (m.id == menuId) { m.items.push_back({ id, label, false, shortcut }); m_widgets.setMenuBar(std::move(menus)); ...
```
- **Lösung**: `WidgetManager::addMenuItem(menuId, item)` / `addMenu(menu)` direkt anbieten (wie schon `setMenuItemEnabled`), die ohne Kopie in-place anfügen; die vier Lambdas reduzieren sich auf je eine Zeile plus `g_menuDirty = true`.
- **Erwarteter Effekt**: vernachlässigbar (einmalig); Vereinfachung der vier Lambdas.
- **Risiko / Verifikation**: Gering; `setMenuBar` schließt offene Menüs – `addMenuItem` müsste dies erhalten oder explizit nicht tun (Verhaltensentscheidung). Test: `tests/` für Menu-API.

### GAM-108 – `updateScripts`: Iteration über `unordered_map` ruft jede Instanz über zwei virtuelle Ebenen auf, auch ohne `onUpdate`
- **ID**: GAM-108
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: GameApplication.cpp:2220–2226; ScriptContext.cpp:1677–1681
- **Problem**: Pro Frame wird jede registrierte Skriptinstanz über `ScriptContext::callOnUpdate` → `backendForId` → virtuelles `IScriptBackend::callOnUpdate` angesprochen, unabhängig davon, ob das Skript eine `onUpdate`-Funktion definiert. Ob die Backends das billig verwerfen, ist in diesem Slice nicht sichtbar. Iteration über `unordered_map` ist zudem cache-unfreundlich und hat eine nichtdeterministische Reihenfolge (ein Verhaltensrisiko, kein Performanceproblem; nicht ändern ohne Test).
- **Code-Auszug**:
```cpp
for (auto& [entityId, instId] : m_scriptInstances)
    m_scriptContext->callOnUpdate(instId, dt);
```
- **Lösung**: Beim Start (`startScripts`/`startScriptsFor`) einmal eine dichte Liste "Instanzen mit `onUpdate`" aufbauen (`std::vector<InstanceId>`), Pflege bei Zone-Unload (Z. 2096). Reihenfolge dann deterministisch (Einfügereihenfolge) – Abweichung zu heute dokumentieren.
- **Erwarteter Effekt** (Schätzung): proportional zu Skripten ohne `onUpdate`; nur relevant bei hunderten Skriptinstanzen. Messen.
- **Risiko / Verifikation**: Ändert Reihenfolge der onUpdate-Aufrufe (heute hash-bedingt unbestimmt); Test, der Reihenfolge-Unabhängigkeit der Skripte sicherstellt, vor der Änderung.

### GAM-109 – Per-Frame-Allokationen und Kopien kleiner Hilfscontainer
- **ID**: GAM-109
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: GameApplication.cpp:3198–3202 (Debug-Lines), 2498–2503 (`events`), 3046–3050 (Tray), 3220 (`justRegistered`), 1935 (`takeRequests`)
- **Problem**: (1) `std::vector<DebugLine> dbg` wird jeden Frame lokal neu angelegt und per `SetDebugLines` an den Renderer **kopiert** (`OpenGLRenderer.cpp:10091–10094`: `m_debugLines = lines;`), auch wenn immer leer. Bei leerem Vektor kostet das fast nichts, aber der Renderer-Aufruf läuft trotzdem jeden Frame, bei den anderen Backends vermutlich gleich. (2) `std::vector<UIInputSystem::PointerEvent> events` wird in `updateUIInput` jeden Frame lokal angelegt (Allokation erst beim ersten Event). (3) `pollAsyncResults` liefert pro Schleifendurchlauf einen neuen `std::vector<HE::UUID>` zurück, auch wenn nichts fertig ist (SBO gibt es bei `std::vector` nicht, aber leer = keine Allokation).
- **Code-Auszug**:
```cpp
std::vector<DebugLine> dbg;
HE::api::debug::collect(deltaTime, dbg);
if (r) r->SetDebugLines(dbg);
```
- **Lösung**: `dbg` und `events` als Member halten (`clear()` statt neu anlegen, erhält Kapazität); `SetDebugLines` nur aufrufen, wenn `dbg` nicht leer ist oder zuletzt nicht leer war (Flag "hatte Linien"). Mikrooptimierung – nur umsetzen, wenn ohnehin im Rahmen von GAM-103 extrahiert wird.
- **Erwarteter Effekt**: minimal (Schätzung: <1 µs/Frame im Normalfall); relevant nur bei Debug-Draw-Spitzen.
- **Risiko / Verifikation**: Gering; Tests mit `debug.*` Zeitablauf (Linien müssen nach Ablauf verschwinden, also auch das Leer-Setzen einmal pushen).

### GAM-110 – Doppelter Text-Editier-Switch in `OnEvent`
- **ID**: GAM-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: GameApplication.cpp:2785–2816 und 2834–2905
- **Problem**: Die Tastatur-Grammatik für ein fokussiertes Label (Pfeile, Home/End, Ctrl+A, Ctrl+C) ist im Textfeld-Block ein zweites Mal fast wortgleich ausgeschrieben (`Left/Right/Home/End/Up/Down/SelectAll/Copy`). Die Kopien weichen bereits leicht ab (Label-Block hat kein Backspace/Delete, Feld-Block beantwortet Shift+Ctrl-Varianten). Beide Blöcke berechnen `shift/ctrl/alt` identisch.
- **Code-Auszug**:
```cpp
const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
const bool ctrl  = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
const bool alt   = (event.key.mod & SDL_KMOD_ALT) != 0;
switch (event.key.key) { case SDLK_LEFT: ... }
```
- **Lösung**: Hilfsfunktion `bool handleCaretKey(WidgetManager&, const SDL_KeyboardEvent&)` für die gemeinsamen Tasten (Pfeile, Home, End, Ctrl+A, Ctrl+C), beide Blöcke rufen sie; nur die schreibenden Tasten (Backspace, Delete, Return, Z/Y/X/V) bleiben im Feld-Block.
- **Erwarteter Effekt**: ca. 40 Zeilen weniger, weniger Drift-Risiko. Keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering; `tests/test_ui_widgets.cpp` deckt Widget-Editing, aber nicht diesen SDL-Event-Pfad – kleiner Test mit synthetischen `SDL_Event`s ergänzen, falls möglich.

### GAM-111 – Viele kleine `SDL_*`-Aufrufe und Fensterabfragen pro Frame in `updateUIInput`
- **ID**: GAM-111
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: GameApplication.cpp:2273–2326, 2357–2372
- **Problem**: Jeden Frame werden `SDL_GetMouseState`, `SDL_GetMouseFocus`, `SDL_GetWindowSize`, `SDL_GetWindowSizeInPixels`, `SDL_GetWindowDisplayScale` und bei fokussiertem Feld `SDL_SetTextInputArea` aufgerufen; bei Mehrfenster zusätzlich `getWindow(HE::WindowHandle{sid})`. Die Größen ändern sich nur bei Resize-/Scale-Events. Der Kommentar (Z. 2309) lehnt Event-basiertes Nachführen bewusst ab ("one cheap call"). Außerdem läuft `updateUIInput` auch im Spielbetrieb ohne jedes Widget und ohne UI-Elemente vollständig durch (`processPointer`, `UIInputSystem::update` über ein View aller UI-Entities).
- **Code-Auszug**:
```cpp
SDL_GetWindowSize(w, &ww, &wh);
SDL_GetWindowSizeInPixels(w, &pw, &ph);
m_widgets.setDisplayScale(uiWindow, w ? SDL_GetWindowDisplayScale(w) : 1.0f);
```
- **Lösung**: Nur messen, nicht blind ändern: In einem Frame-Profil prüfen, ob die SDL-Aufrufe (auf Cocoa/Windows teils nicht trivial, insbesondere `SDL_GetWindowDisplayScale`) merkbar sind. Falls ja, Größe/Scale pro Fenster cachen und bei `SDL_EVENT_WINDOW_RESIZED`/`PIXEL_SIZE_CHANGED`/`DISPLAY_SCALE_CHANGED` invalidieren.
- **Erwarteter Effekt**: unsicher / messen; vermutlich <5 µs/Frame.
- **Risiko / Verifikation**: Ein verpasstes Event würde falsche Klickkoordinaten liefern (sichtbar bei Monitorwechsel mit anderem Scale). Manuell auf zwei Monitoren mit unterschiedlicher Skalierung prüfen.

### GAM-112 – `#ifdef`-Streuung für Plattformpfade (macOS-Menü/Notify/Cursor)
- **ID**: GAM-112
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: GameApplication.cpp:12–18, 197–233, 243–248, 2415–2426, 3115–3135, 3738–3743
- **Problem**: Plattformcode ist an sieben Stellen mitten in Funktionen eingestreut (`__APPLE__`, `__linux__`, `_WIN32`). Besonders `OnRender` (3115–3135) und `OnShutdown` (3738–3743) tragen `#ifdef`-Blöcke, die jeweils ein Stück macOS-Menülogik (`g_menuDirty`, `AppMacMenu::set/take`) enthalten, während `g_menuDirty` selbst auf allen Plattformen existiert und gesetzt wird (Z. 179, 1094 usw.).
- **Code-Auszug**:
```cpp
#ifdef __APPLE__
    if (g_menuDirty) { g_menuDirty = false; m_widgets.setMenuBarNative(HE::AppMacMenu::available()); HE::AppMacMenu::set(m_widgets.menuBar()); }
```
- **Lösung**: Eine plattformneutrale Fassade `HE::NativeMenu` (`syncMenuBar`, `pollClicks`, `clear`) mit leerer Implementierung auf Nicht-Apple-Plattformen (analog zu `AppNotify`); `OnRender`/`OnShutdown` rufen sie ohne `#ifdef`. Dasselbe für `notifyAvailable/notifyShow`, die bereits drei Implementierungen in einer Datei halten, besser in `AppNotify_{mac,linux,win}.cpp`.
- **Erwarteter Effekt**: weniger Präprozessor-Rauschen; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; Build auf allen drei Plattformen nötig (CI), da nur macOS die Menü-Kopplung tatsächlich nutzt.

### GAM-113 – Kleinigkeiten: tote/redundante Konstrukte
- **ID**: GAM-113
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: GameApplication.cpp:376–379 (`static` in anonymem Namespace), 1016 (`(void)netId`), 2569–2575/2580 (Kamera), GameApplication.h:260–268 (Tray-Kommentar), GameApplication.h:14 und 7–10 (Includes)
- **Problem**: (1) `static HE::RendererBackend defaultBackend()` innerhalb des anonymen Namespace – `static` ist dort redundant. (2) Der Header enthält einen verwaisten Kommentarblock "The tray (plan A7)" (Z. 260–268), unter dem kein Member mehr steht (der Tray lebt laut eigenem Text in der .cpp); er hängt sachlich am nächsten Member `m_windowWidth`. (3) `GameApplication.h` zieht schwere Header ein (`HorizonScene/*Host.h`, `PhysicsWorld.h`, `EngineApi.h`, `NetGameSession.h`, `AntiCheatHost.h`, `HorizonGameServices.h`), obwohl `main.cpp` nur die Klasse konstruiert; die Services-Tabellen und Hosts sind Werttypen und deshalb nicht einfach vorwärts deklarierbar. (4) In der .cpp wird `<unordered_set>` (Z. 66) und `<cstring>` nur teilweise genutzt (`std::memcpy` ja, `unordered_set` im gelesenen Code nicht gefunden).
- **Code-Auszug**:
```cpp
static HE::RendererBackend defaultBackend() { return RendererFactory::Default(); }
```
- **Lösung**: `static` entfernen, Kommentarblock im Header an den richtigen Platz bzw. löschen, per `include-what-you-use` die Includes prüfen; Pimpl für die Hosts nur erwägen, wenn die Kompilierzeit von `main.cpp` messbar stört (unwahrscheinlich, da eine einzelne TU).
- **Erwarteter Effekt**: kosmetisch.
- **Risiko / Verifikation**: Keines; Kompilieren genügt.

## Cross-Modul-Hinweise

- `src/HE_Scene/src/EngineApi.cpp` / `HorizonScene/EngineApi.h`: `HE::api::Ctx` ist ein Bündel aus ~35 `std::function`-Feldern, das pro Aufruf kopiert wird (GAM-106). Eine Struktur mit Zeiger auf die einmal gefüllte Host-Tabelle wäre dort die Lösung.
- `src/HE_Core/src/Diagnostics/GlobalState.cpp:880–905`: `getCustomConfig*` nimmt `const std::string&` und führt `contains` + `operator[]` aus (zwei Lookups, String-Konstruktion beim Aufrufer). Ein typisierter Cache der Renderer-Settings (struct, einmal gelesen und bei `setCustomConfigEntry` invalidiert) würde Editor-Preferences und Game gemeinsam entlasten.
- `src/HE_Scene/src/EngineApi.cpp:5359`: `takeRequests` verschiebt den gesamten Vektor heraus und leert die Quelle; die Kapazität geht dabei verloren (Neuallokation bei der nächsten Anfrage). Unkritisch, da selten.
- `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:10091`: `SetDebugLines` kopiert den Vektor, ein `std::move`/Swap würde die Kopie sparen (andere Backends analog prüfen).
- `src/HE_Scene/include/HorizonScene/HorizonWorld.h:106–107`: Revisionszähler für `cellManifestJson` (GAM-102).

## Unsicher / Messung nötig

- Ob die Renderer-Backends `SetBloom/SSAO/…/SetRenderPath/SetSwapchainPostProcessing` mit Änderungserkennung behandeln oder bei jedem Aufruf Zustände invalidieren (GAM-101). Das entscheidet, ob der Fund Mittel (messbar) oder nur Aufräumarbeit ist.
- Tatsächliche Kosten der Config-Lookups pro Frame (35 Aufrufe) – per Profiler messen.
- Dauer der einzelnen `OnInit`-Blöcke (Typ-Index, Szenen-Deserialisierung, `compiledClasses().load`, `scanContentDirectory`) für GAM-104; derzeit gibt es dafür keine `HE_PROFILE_SCOPE_N`.
- Kosten von `callApi`/`apiCtx` bei realistischen HorizonCode-Graphen (GAM-106).
- Ob die Skript-Backends `onUpdate` ohne Definition billig verwerfen (GAM-108).
- Ob `UIInputSystem::update`/`processPointer` in einer Szene ohne UI zu vernachlässigen sind (GAM-111).
- Nicht belegt, aber prüfenswert: der Streaming-Loop (3218–3230) ruft pro Batch `WarmupMaterials` auf; bei einer synchronen Kreuz-Kompilierung im Backend kann das das 4-ms-Budget überschreiten – ohne Backendcode nicht beurteilbar.

## Gelesen

Ja, alle neun Dateien des Slice vollständig gelesen (`GameApplication.cpp` in vier Blöcken 1–3800, `GameApplication.h`, `main.cpp`, `EmbeddedPakKey.{h,cpp}`, `AppNotify.{h,mm}`, `AppMacMenu.{h,mm}`); die Headers `AppMacMenu.h`, `AppNotify.h` und `EmbeddedPakKey.h` wurden mit Kommentarzeilen gefiltert ausgegeben; fremde Dateien (GlobalState.cpp, EngineApi.cpp, ScriptContext.cpp, OpenGLRenderer.cpp, HorizonWorld.h) nur gezielt für Belege.
