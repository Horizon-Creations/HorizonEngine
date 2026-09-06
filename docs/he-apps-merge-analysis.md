# HE Apps: Verschmelzungs-Analyse für den Merge nach main

**Stand:** 06.09.2026, Branch `claude/he-apps-ui-framework-83b5f7` (HEAD `6902a5c5`), Merge-Base `e38eabfa`, main `af05e59b`.
**Zweck:** Entscheidungsgrundlage für den Menschen. Kein Code, keine Änderung, nur Analyse. Grundlage sind `docs/he-apps-plan.md`, der vollständige Diff `main...HEAD` (178 Commits, 194 Dateien, +73 495 / −3 678 Zeilen) und die 80 Beiträge im Hive-Thema.
**Lesehinweis:** Abschnitt 1 ist die Kurzfassung. Wer nur eine Seite liest, liest die. Abschnitt 6 (Grauzone) ist der Teil, an dem die eigentliche Entscheidung hängt.

---

## 1. Kurzfassung und Empfehlung

**Der Branch ist mergefähig, aber nicht als „App-Feature", sondern als Engine-Umbau mit einem App-Schalter obendrauf.** Knapp die Hälfte der neuen Zeilen liegt in HE_Core (davon fast die Hälfte eingebettete Fonts), ein Fünftel sind Tests, ein Zehntel Editor. Was in HE_Core liegt, ist fast ausnahmslos ungegated und für alle Nutzer wirksam, Spiele eingeschlossen. Das ist kein Unfall, sondern die Bauweise, die der Plan von Anfang an vorgab: „kein neues Teilsystem, sondern ein zweiter Betriebsmodus für Dinge, die größtenteils schon existieren". Der App-Modus selbst ist klein: ein Bit in `project.hcfg`, ein Dutzend `if (m_appMode)`-Verzweigungen in `GameApplication`, ein CMake-Schalter für die Runtime-Ausprägung.

**Drei Dinge sind geprüft und in Ordnung:**

- Ein Trocken-Merge (`git merge-tree --write-tree main HEAD`) ist **konfliktfrei**. main ist seit dem Abzweig um genau einen Commit weiter (`af05e59b`, ContentManager plus `ci.yml`), der Branch hat main am 01.09. bereits einmal hereingeholt (`1e9e0965`).
- Es gibt **keinen alten und keinen neuen WidgetManager nebeneinander**. Die Klasse ist in `5c49fbc1` von HE_Scene nach HE_Core umgezogen (Datei gelöscht, Datei angelegt, sieben Include-Stellen umgeschrieben). Die Aufgabenbeschreibung, die einen Konflikt mit dem „alten HE_Scene::WidgetManager" vermutet, geht von einer Verdopplung aus, die es nicht gibt.
- Das ECS-UI (`UISystem`, `UIInputSystem`, `UIElementComponent`) ist **eingefroren, nicht entfernt**: neun geänderte Zeilen im ganzen Diff, alle wegen des Umzugs. Der Plan hat das in Abschnitt 9(a) so entschieden („der Widget-Baum gewinnt, das ECS-UI wird eingefroren").

**Fünf Dinge sind geprüft und brauchen vor dem Merge eine Entscheidung** (Details in 6.1; die ersten drei wiegen am schwersten). **NACHTRAG 06.09.2026: alle fünf sind entschieden und umgesetzt**, dazu der Paritätsbefund weiter unten — siehe die Merge-Checkliste in Abschnitt 9 und die Zeilen in 6.1, jede mit ihrem Commit. Die Absätze hier stehen unverändert, weil sie beschreiben, wovon die Entscheidung handelte:

1. **`project.hcfg` wird ab jetzt immer als v5 geschrieben, auch für Spiele.** Der Exporter füllt `bundleId` bedingungslos, der Schreiber wählt bei gefüllter Bundle-Id v5, und der Leser auf main akzeptiert nur v2 und v3. Ein vom Nutzer unter `GameRuntimes/<Plattform>` abgelegtes Runtime-Bundle vom Stand main bootet danach ohne Pak. Das bricht still einen dokumentierten Arbeitsablauf.
2. **`createWidget` erzeugt versteckt** (`WidgetManager.cpp:803`, Commit `4039f047`). Show Widget ist jetzt Pflicht. Die Projektvorlagen und die Tests wurden nachgezogen, eine automatische Migration bestehender Graphen gibt es nicht. Jedes bestehende Spielprojekt, das Create Widget ohne Show Widget benutzt, zeigt nach dem Merge nichts mehr.
3. **`GameLoop` verwirft den Fixed-Step-Rückstand am Step-Cap** statt ihn weiterzutragen, bedingungslos. Der Kommentar begründet es als die richtige Antwort auch für Spiele („Spiral of Death"). Die Änderung ist vertretbar, aber sie muss gewusst sein.
4. **Neue Spielprojekte bekommen regulären statt fetten UI-Text** (`fontWeightBold = false` für jedes neue Projekt), alte Projekte bleiben Bold. Eine Zeile, aber eine sichtbare.
5. **Bestehende Spiele bekommen beim nächsten Export ein generiertes „widgets"-Icon**, weil der Loader den fehlenden Icon-Namen mit einem Default füllt und der Exporter das Icon unabhängig vom Projekttyp erzeugt.

**Und ein Befund gegen den Plan:** Der Plan verspricht, jede Registry-Gruppe „leuchtet danach in HorizonCode, Lua und Python gleichzeitig auf". Das stimmt für `app`, `http`, `string.equals` und `fs`, aber **nicht für `widget`, `theme`, `dialog`, `clipboard`, `process`, `json`, `prefs`, `datetime`**: `isScriptGroup` in `EngineApi.cpp` kennt sie nicht, und Lua wie Python spiegeln nur die Gruppen dieser Liste. Listen, Dialoge, Ebenen, Animationen und Themes sind heute HorizonCode-only. Kein Merge-Blocker, aber eine Lücke in der Parität, die das Handbuch und die HorizonCode-Vision anders darstellen (Details 6.1 #9).

**Empfehlung:** Den Branch als Ganzes mergen, nicht in Teilen. Die Abhängigkeitskette (HE_Core-Widgets, HE_Tools-Projektfelder, HE_Scene-Registry, Editor) lässt sich nicht sauber trennen, und der Trocken-Merge zeigt, dass es technisch nichts zu trennen gibt. Vorher die drei Punkte oben abarbeiten (Punkt 1 ist eine Zeile im Exporter, Punkt 2 eine Entscheidung zwischen Migration und Release-Note, Punkt 3 eine Kenntnisnahme). Die Merge-Reihenfolge, die der Mensch am 05.09. festgelegt hat, bleibt: **dieser Branch geht als letzter**, nach jedem Engine-Thema, und zieht main vorher noch einmal nach.

---

## 2. Faktenlage

| Größe | Wert |
|---|---|
| Commits auf dem Branch (main..HEAD) | 178 |
| Geänderte Dateien | 194 (+73 495 / −3 678) |
| Davon HE_Core | 34 395 Zeilen, davon 16 740 Vendor-Header (Fonts, stb_image_write) |
| Davon Tests | 16 409 Zeilen (`test_ui_widgets.cpp` allein +12 881) |
| Davon HE_Editor / HE_Scene / HE_Tools / HE_Rendering / HE_Game | 7 120 / 2 834 / 2 351 / 1 546 / 1 535 Zeilen |
| main seit Merge-Base | 1 Commit (`af05e59b`) |
| Überlappende Dateien beider Seiten | 3 (`ci.yml`, `ContentManager.h`, `ContentManager.cpp`) |
| Trocken-Merge `git merge-tree` | konfliktfrei |
| ctest (letzter Stand im Thema, `6902a5c5`) | 120/120, `he_tests` 2393/2393 |
| Hilfe-Deckung Editor | 689/689 Bedienelemente, 972 Einträge |
| Schritte im Hive-Thema | 23 von 23 abgeschlossen |

**Merge-Reihenfolge (Anweisung vom Menschen, über den Leitstand am 05.09.):** Alle „normale Engine"-Themen (aktuell Thema 8, Decals) gehen vor diesem Branch nach main. Dieser Branch bleibt der letzte, zieht main nach jedem durchgegangenen Engine-Thema nach und geht selbst erst, wenn er fertig ist und alle Engine-Themen drin sind. Danach: voller ctest, dann PR statt direktem Push, damit die CI drüberläuft.

**Zu prüfen für den Menschen:** Der Leitstand hat gemeldet, dass `claude/decals-cross-backend` versehentlich von diesem Branch abgezweigt war. Geprüft: seine Merge-Base mit diesem Branch ist `6a8ca1f8`, ein Commit dieses Branches, seine Merge-Base mit main nur `e38eabfa`. Der Nachfolger `claude/decals-cross-backend-v2` hat seine Merge-Base auf `af05e59b`, also sauber auf main. Der alte Decals-Branch sollte nicht mehr verwendet werden.

**Bekannte Restbaustellen aus dem Thema** (nicht Teil der 23 Schritte, siehe Abschnitt 8): auswählbarer statischer Text, `db` und `print` aus Block C. Vier Punkte von hier sind seit dem 06.09.2026 erledigt (Abschnitt 8): die Fensterzeilen, Menü `enabled`/`checked`, die Kontrastbefunde und das Lösch-Kreuz.

---

## 3. Wie das neue Widget-System mit Spiel-UI und Editor zusammenhängt

### 3.1 Der eine WidgetManager

Auf main lebte `WidgetManager` in `src/HE_Scene/{include/HorizonScene,src}/WidgetManager.{h,cpp}` (243 + 1216 Zeilen). Auf dem Branch liegt er in `src/HE_Core/{include,src}/UIWidget/WidgetManager.{h,cpp}` (1193 + 5462 Zeilen). Der Umzug (`5c49fbc1`) hatte einen einzigen Grund: „ein App-Runtime kann Widgets künftig ohne `libHorizonScene` und damit ohne Jolt und Recast bekommen". Vorher hatte ein früherer Schritt festgestellt: „libHorizonScene kann NICHT wegbleiben, der ganze Widget-Baum und die Widget-Laufzeit liegen darin und HorizonGame linkt es direkt."

Was sich durch den Umzug geändert hat:

- Die `.cpp` nannte aus HE_Scene nur `UISystem::sortKey`. Das ist jetzt `HE::uiSortKey(layer, depth)` in `Renderer/UIRenderObject.h`, mit der Regel: „Both UI producers, the entity path (UISystem::extract) and the widget path (WidgetManager::extract), sort by this, and they have to agree." Das ist der eine geteilte Vertrag zwischen ECS-UI und Widget-Baum.
- Die Klasse trägt jetzt `HE_API`, weil HorizonCore anders als HorizonScene nicht mit `WINDOWS_EXPORT_ALL_SYMBOLS` gebaut wird. Das ist die Windows-Portabilitätsregel aus dem Gedächtnis, hier zum ersten Mal an einer großen Klasse angewendet.
- Sieben Include-Stellen wurden umgeschrieben: `HorizonScene.h`, `HorizonWorld.h`, `AssetThumbnailCache.cpp`, `OutlinerPanel.cpp` und drei Tests. Jede Datei, die auf main neu den alten Pfad `HorizonScene/WidgetManager.h` einführt, bricht nach dem Merge. Aktuell gibt es keine.
- `WidgetManager.cpp` enthält **keine** `appMode`-, Berechtigungs- oder Flavour-Abfragen und keine SDL-Includes. Alle OS-Anbindung (Cursor, Zwischenablage, Dateidialoge, Menüleiste, Tray, Fenster-Treffertest) bleibt beim Host.

**Der Zugriffsweg im Spiel ist unverändert `HorizonWorld::widgets()`.** Genau deshalb behält der App-Modus Welt und Standardkamera (Plan A1): „Welt und Standardkamera bleiben, weil die Widget-API über `HorizonWorld::widgets` läuft und der Renderpfad eine Kamera braucht." Ein weltloser Modus im Wortsinn existiert also nicht. Es gibt eine leere Welt mit einer Standkamera, deren Systeme nicht ticken.

### 3.2 Die zwei UI-Pfade

| | ECS-UI (`UISystem`) | Widget-Baum (`WidgetManager`) |
|---|---|---|
| Ort | HE_Scene, `UIElementComponent`, `UIImageComponent` | HE_Core |
| Stand auf dem Branch | eingefroren, 9 geänderte Zeilen | alles Neue |
| Geteilt | `uiSortKey`, `UIRenderObject`, `m_renderWorld.uiObjects`, der UI-Pass jedes Backends | |
| Bekannte Falle | `ui.setMaterialParam` läuft weiter über `UIImageComponent` und verändert das geteilte Asset (Plan D5) | |

Der Plan sagt zur Zukunft des ECS-UI „später auf den Baum abgebildet oder entfernt". Das ist eine offene Entscheidung, die dieser Branch nicht trifft. Für den Merge heißt das: nichts am ECS-UI ist kaputt, aber niemand pflegt es weiter.

### 3.3 Der Host: GameApplication und Editor

Der App-Modus ist in `GameApplication` ein einziges Bit, das an zwei Stellen gelesen wird: einmal im Konstruktor (`peek.appMode`, damit das Fenster einer Anwendung nicht im Vollbild öffnet und der Mausgriff gar nicht erst stattfindet, E6) und einmal in `OnInit` aus der geladenen Config. Danach hängen daran: kein Szenenladen, keine Physik, keine Entity-/Player-/Animator-Hosts, kein Asset-Streaming, keine Kamerasteuerung, kein ECS-Systemtick, kein Mausgriff, Escape nicht geschluckt, `setEventDriven(true)`, und der Fenster-Treffertest nur bei `appMode + Borderless`. Rund 15 Verzweigungen, alle mit `m_appMode`.

Der Editor kennt denselben Schalter als Projektattribut (`ProjectData::appProject`, gesetzt über das Preset beim Anlegen, nicht nachträglich änderbar) und liest ihn an einer Stelle (`AppContext::appLivePreview`). Im App-Projekt gibt es keinen Play-Modus: die UI läuft permanent, der Viewport ist die App, der Play-Knopf heißt „Restart Live Preview", Details-Panel und Viewport-Toolbar entfallen, der Outliner zeigt Widget-Instanzen, die Szenenmenüs sind weg (ImGui und macOS-Leiste in Gleichschritt). Regel aus dem Code: „The editor is gated by the SAME block as the shipped app on purpose", die drei Berechtigungen binden also auch den Editor.

**Zustellungs-Asymmetrie, geprüft:** `fireOnMenuItem`, `fireOnTrayItem` und `fireOnHttpResponse` werden nur in `GameApplication.cpp` aufgerufen (Zeilen 2191, 2201, 2241). In der Editor-Vorschau kommen Menü-, Tray- und HTTP-Ereignisse nicht an. Der Plan sagt das so; es ist eine bewusste Lücke, keine vergessene. Der Grund steht im Code: „a previewed graph must not put an icon in the menu bar of the machine somebody is working on."

**Was der Spiel-Host für alle bindet:** Die neuen Host-Callbacks am `Ctx` (Fenster, Tray, Autostart, Menüleiste, Notify) bindet `GameApplication` unabhängig von `m_appMode`. Regel für ungebundene Zeilen: „Unset = the row logs once and does nothing." Ein Spiel-Graph kann also eine Menüleiste zeichnen oder ein Tray-Icon setzen; nur `app.requestRedraw` ist im Spiel stumm, „because a game already draws every frame". Ob das ein Feature oder ein Versehen ist, steht in 6.2 #34.

Ebenfalls für alle Spiele aktiv sind die neuen Eingabepfade in `OnEvent` und `updateUIInput`: Datei-Drop, Systemthema-Wechsel, IME (`SDL_EVENT_TEXT_EDITING`), Wort-Navigation, Undo/Redo, Doppel- und Dreifachklick, rechte Maustaste, Tab, Gamepad-East, Menü-Kürzel. Ohne Menüleiste und Ebenen sind das Leerläufe, aber sie stehen vor der bisherigen Logik (6.1 #10, 6.2 #31 bis #33).

### 3.4 Die drei Runtime-Ausprägungen

`HE_RUNTIME_FLAVOR` (`game | app-advanced | app-basic`) ist ein Baum-Schalter, kein Ziel-Schalter: „an app runtime is its OWN build directory (scripts/build_runtimes.py drives them), never a second output of the game build". Default ist `game`, und der Kommentar verspricht Byte-Identität des Game-Builds mit dem Zustand vor dem Schalter. Die beiden App-Ausprägungen zwingen `HE_ENABLE_SHADERC=OFF`, bauen keinen Editor, und linken genau einen Renderer (`AppAdvanced`: Metal beziehungsweise GL; `AppBasic`: `RendererSoftware`). Der Exporter wählt die Ausprägung aus dem, was das Projekt ist (`runtimeFlavorFor(appMode, advancedShaderEffects)`, für Spiele immer `Game`) und fällt auf `Game` zurück, wenn das Bundle fehlt.

Gemessen (Release, macOS/arm64): app-advanced 22,2 MB ohne Python (Rendering 1,1 MB), app-basic 21,5 MB (Rendering 0,4 MB). Die CI (`runtime-flavors.yml`) misst seit dem 05.09. alle drei Ausprägungen auf allen drei Plattformen gegen Schwellen.

### 3.5 Das Berechtigungsmodell

Der Ein-Satz-Vertrag aus Block C, wörtlich aus dem Plan: „**Die Berechtigung sagt, was ein SKRIPT von sich aus benennen darf. Sie sagt nie, was ein MENSCH auswählen darf.** Ein Pfad, den jemand in einem Dateidialog gewählt hat, ist danach frei, das Auswählen IST die Erlaubnis." Drei Türen (`allowFiles`, `allowProcesses`, `allowNetwork`), alle zu, gespeichert „gerade" (fehlend heißt zu), im Gegensatz zu `advancedShaderEffects` und `fontWeightBold`, die negiert gespeichert werden, weil ihr ehrlicher Default „an" ist. Drop und „Öffnen mit" gehen durch dieselbe Tür wie der Dateidialog (`fs::grantPath` im Host). Die Flags stehen in jedem Projekt, auch in Spielen; ein Spiel, das `fs` außerhalb von `Saved/` will, kann sie setzen.

Was gegated ist und was nicht, jeweils mit Begründung im Code: `process.run`, `process.openUrl` und `app.setAutostart` über `processes` („it asks the SYSTEM to run a program at every login, which is a bigger thing than running one now"), `http.get`/`http.post` über `network`, jeder absolute Pfad in `fs` über `files` oder eine Laufzeit-Freigabe. Nicht gegated: `dialog.*` („The choosing IS the permission"), `clipboard`, `json`, `prefs`, `datetime`, `app.notify` („A notification cannot read anything, reach anywhere or start anything"), `process.which` („asking whether a tool is installed runs nothing"). Die Fenster-, Tray- und Menüzeilen haben keine Berechtigung; ihr Gate ist, ob der Host sie gebunden hat. Zwei Gates also, `perm::allowed` in der Registry und „ist der Callback gebunden" im Host.

**Lua und Python** sehen von alldem nur die Gruppen aus `isScriptGroup`: `app`, `http`, `fs`, `string`, `ui` ja, `widget`, `theme`, `dialog`, `clipboard`, `process`, `json`, `prefs`, `datetime` nein (6.1 #9).

### 3.6 Wo die Cocoa-Dateien liegen

`AppMacMenu.mm` und `AppNotify.mm` liegen in HE_Game, nicht in HE_Core: „Cocoa dort zöge AppKit in he_tests, hc_codegen, widget_gen und die Windows-CI." Die Editor-Menüleiste (`HE_Editor/MacMenuBar.mm`) ist davon unabhängig und teilt keinen Code. Der Preis: jedes macOS-Spiel linkt Cocoa und UserNotifications (6.1 #11).

### 3.7 Der UI-Pass in den Backends

`UIRenderObject` ist das geteilte Vokabular zwischen Widget-Baum, ECS-UI und allen sechs Backends. Metal und GL implementieren die Schicht-0-Felder mit derselben `heRoundedBoxSDF` („one rule, two languages"), dazu pro Quad einen `HeUI`-Block (GL UBO-Bindung 8, Metal Fragment-Buffer 3) und den `heBackdrop`-Schnappschuss für Milchglas (GL `glCopyTexSubImage2D`, Metal teilt den Pass, `EncodeUIPass` gibt jetzt den Encoder zurück). D3D11, D3D12 und Vulkan sind unverändert; sie kompilieren, weil sie `cornerRadius` nie gelesen haben, und ignorieren Rundungen, Ränder, Verläufe und Schatten (7.4).

Der Software-Renderer ist zweigeteilt: `SoftwareRaster` (Quads rein, Pixel raus, kein SDL, testbar) und `SoftwareRenderer` (IRenderer-Hülle, blittet per `SDL_GetWindowSurface`, Dirty-Rectangles). `GetCapabilities()` gibt alles `false` zurück, „and that is the honest answer rather than a limitation". Er kennt keine Materialien auf Quads (Schicht 0 plus Farbton), keine Gammakorrektur, keine komprimierten Texturen und nichts Dreidimensionales. Er sitzt trotzdem in der geteilten Factory und wird auch in der `game`-Ausprägung gelinkt (6.1 #13).

Alle `HE_HAVE_SHADERC`-Guards in Metal und GL sind entfernt; der Materialpfad ist immer kompiliert, `he_materialshader` wird immer gebaut, und die UI-Pipelines lesen zuerst die vorkompilierten Varianten (`uiVertex`). Deferred, SSR, Decals und GI-Resolve bleiben bewusst hinter `HE_HAVE_SHADERC`.

---

## 4. Engine-weit übernehmen (auch für Spiele nutzbar)

Alles hier liegt ungegated in HE_Core, HE_Scene oder HE_Rendering und wirkt für jeden Nutzer. Pro Gruppe der Grund, warum ein Spiel davon etwas hat.

**Widget-Typen und Layout.** Acht neue Typen angehängt (Spacer, ListView, WrapBox, Grid, TabBox, Splitter, DatePicker, ColorPicker), Min/Max-Größe auf der Basis, `AutoSize` für CheckBox und ComboBox, Einrasten und Hilfslinien (Geometrie in HE_Core, der Editor zeichnet nur). Ein Inventar ist eine ListView, ein Optionsmenü ist ein Grid mit Tabs.

**Schicht 0 (Stil-Felder an `UIRenderObject`).** Vier Eckenradien in CSS-Reihenfolge, Rahmen, linearer und radialer Verlauf, Schlagschatten, Innenschatten, alles im eingebauten UI-Shader jedes Backends (Metal und GL vollständig, D3D/Vulkan siehe 7.4). Der Button ist jetzt eine Fläche mit Text-Kind statt eines Labels mit Rahmen. Kein Spiel-HUD musste bisher für eine abgerundete Ecke ein Material anlegen; jetzt muss es das nicht mehr.

**Themes.** `AssetType::Theme`, neun Farbrollen mal hell/dunkel, Radius-/Abstands-/Textstufen, Stile pro Elementtyp mit CSS-Spezifität (`Button < Card < Button.success`), Auflösung beim Zuweisen statt pro Frame (`uiApplyTheme`), Theme-Editor als Formular. Rangfolge an einer Stelle: „Rolle > Sperre > Kaskade" in `uiThemeValueFor`. Ein Spiel bekommt damit hell/dunkel als Schalter statt als zweite Garnitur Widgets.

**Textkatalog, Schriftskalierung, Tab-Reihenfolge, Kontrastprüfung.** `UITextCatalog` mit `setLanguage`, `WidgetManager::setFontScale`, `Tab Index`, WCAG-2.1-Kontrast als ctest über die Bibliothek. Lokalisierung und Barrierefreiheit sind für Spiele dieselbe Arbeit wie für Apps.

**Animation.** Tweens mit acht Kurven, Clips mit Spuren und Keys am Baum (`UIAnimClip`), Zeitleiste im Designer, Zustandsübergänge (`Transition` in Sekunden). Hover-Blenden auf einem Menüknopf sind kein App-Feature.

**Ebenen: Dialoge, Popups, Kontextmenüs, Tooltips.** Der Grab-Stapel (`showModal`, `openPopupAt`, `closeTopLayer`), `takesInput` an allen neun Eingängen, Escape vor der bisherigen Bedeutung. Der Plan nennt den Pausendialog als Spielbeispiel.

**Text.** Rich-Text-Markup (`<color>`, `<size>`, `<b>`, `<link>`, `<icon>`, Regel: „ein Tag, das nicht vollständig verstanden wird, IST Text"), UTF-8 in allen vier Byte-Schleifen von `UIFont.cpp` (Umlaute waren stumm), Atlas mit Latin-1, Latin Extended-A, Griechisch, Kyrillisch, eingebettete Roboto Condensed Regular und Material Icons, Mehrzeiligkeit mit Wortumbruch, IME, Undo/Redo, Wortsprünge, Ziehen zum Auswählen, Stepper am Zahlenfeld. Der Umlaut-Fix allein ist ein Bugfix für jedes deutsche Spiel.

**Drag & Drop.** Von außen (`dropHover`, `processDrop`, `OnFileDropped`) und innerhalb der Anwendung (`draggable`, `dragPayload`, Vier-Pixel-Schwelle, „ein Zug beginnt an einer Strecke, nie am Druck").

**Registry-Zeilen in `HE::api`.** `widget.*` (Listen, Ebenen, Animationen, `addChild`/`removeChild`), `theme.*`, `app.*` (Titel, Größe, Redraw, Quit, Menüleiste, Tray, Autostart), `clipboard`, `dialog`, `json`, `prefs`, `datetime`, `fs` erweitert, `process`, `http`, `notify`, `string.equals`. Jede Zeile ist gleichzeitig in HorizonCode, Lua, Python und Codegen. `string.equals` ist eine eigene Zeile, weil `Equals` Strings als 0 verglich und „jede Id war jeder gleich"; das traf alle Ereignisse mit Id-Argument. Die Berechtigungsprüfung (`perm::get()`) sitzt an der einzelnen Zeile, nicht an der Gruppe.

**HorizonCode.** `Get Property (Ref)` / `Set Property (Ref)` als Knoten, 17 neue Engine-Ereignisse, String-Pins mit Liste werden im Editor Dropdowns (`engineParamChoices`; trifft auch Level-Skript und Klassen-Editor).

**Material-Graph.** Sechs UI-Knoten (`ElementSize`, `ElementUV`, `RoundedRectSDF`, `BorderDistance`, `ElementState`, `Backdrop`), außerhalb der UI-Domäne Kompilierzeit-Konstanten, „ein Milchglas auf einem Mesh ist ausdrücklich erlaubt". `Function Input` mit Vorgabewert. Neun Effekte als `MaterialFunction`-Assets in EngineContent (UUID-Block 0x300, „Reihenfolge = Identität, nur anhängen").

**Renderer und Fenster.** `RendererFactory::Available/Default/RuntimeFlavor` statt Plattform-Ifdefs („that is how a config.json written for another build would kill the process before its window opens"), GL-Versionsleiter 4.6 → 4.5 → 4.3 → 4.1 (ein Spiel verliert auf schwacher Hardware GI statt zu sterben), `Window::SetHitTest`, `Window::WaitForEvent`, Systemcursor aus dem Widget-Baum, `RendererBackend::Software` als sechstes Backend (per `config.json` auch für Spiele wählbar, zeichnet nur UI; der Editor bleibt immer auf der GPU). Software-Renderer heißt für die Engine: Schicht 0 ist „zum ersten Mal in ctest überprüfbar".

**Shader-Übersetzer.** `he_shadercompiler` existiert immer, ohne glslang als Stub: „`HE_HAVE_SHADERC` heißt jetzt nur noch: darf zur Laufzeit übersetzen, nicht: keine Materialien". Vorkompilierte UI-Varianten (`MaterialShaderVariant::uiVertex`), damit ein Widget-Material beim Laden kein glslang braucht. Für Spiele: ein Build, dessen glslang-Fetch scheitert, linkt jetzt statt zu scheitern.

**Export.** `strip -x` plus Re-Sign auf jede kopierte Runtime-Binary (die gemeldeten 27 MB waren die lokale Symboltabelle), generiertes App-Icon (`.icns`, `.ico`, PNG aus einem PNG-Encoder, „the engine's only PNG writer"), Dateitypen und „Öffnen mit", Autostart. Nichts davon ist an `appProject` gebunden; ein Spiel kann sich dasselbe Icon generieren lassen.

**Editor-Fixes ohne App-Bezug** (könnten notfalls einzeln gecherry-pickt werden): GraphEditor-Inline-Editor-Spalte (`96f927bb`, abgeschnittene Dropdowns), `applyEventSignature` aus `engineEvents` statt zwei Handketten (`bfb3f28e`), `Row::inputText(std::string*)` (`7fe198c8`), Preferences-Rail (`cb3d3ac8`), Project-Hub-`static_assert` gegen die stille sechste Vorlage (`f8549031`), UI-Designer: verzögerter Pick plus Vier-Pixel-Schwelle (`3a43f982`, `a80631ed`, jeder Klick schob die Auswahl um ein Pixel und erzeugte einen Undo-Schritt), Clip-Rect im Treffertest (Klicks landeten auf dem unsichtbaren Teil), Hilfe-Audit kannte `DragInt` ohne Ziffer nicht („ein Deckungswerkzeug, das etwas nicht kennt, meldet keine Lücke, sondern Deckung"), Palette-Knöpfe ohne Hilfeeintrag, mbedTLS auf macOS in `ci.yml` (`fb108c5f`).

---

## 5. Bewusst App-only, und warum

| Was | Gate | Warum nicht für Spiele |
|---|---|---|
| Ereignisgetriebene Hauptschleife (`setEventDriven`, `WaitForEvent`, `WantsPresent`) | `GameApplication` setzt es nur bei `m_appMode`; API-Default aus: „a game must not accidentally inherit it" | Ein Spiel rendert jeden Frame. Die API steht bereit, ein Spiel könnte sie im Pausenmenü nutzen, tut es aber nicht von selbst. |
| Kein Szenenladen, keine Physik, kein ECS-Tick, keine Kamerasteuerung, kein Mausgriff | `m_appMode` in `GameApplication` | Das ist die Definition einer App (Plan Abschnitt 1). |
| E6: kein Vollbild-Default, kein Splash, kein „Escape beendet", kein Mausgriff beim Start | Exporter schreibt `Fullscreen` → `Windowed` nur bei `settings.appProject` (Borderless geht bewusst durch); Splash war nie an (`SplashConfig::enabled = false`, weil HorizonCore in jedem Wirt steckt); Escape schaltete nur den Mausgriff um, und der entfällt | Ein Spiel darf im Vollbild starten und die Maus greifen. |
| Runtime-Ausprägungen `app-advanced` / `app-basic` | `runtimeFlavorFor` liefert für `!appMode` immer `Game`; `HE_RUNTIME_FLAVOR` default `game` | Ein Spiel braucht alle Backends und den Übersetzer. |
| Randloses Fenster mit eigener Titelleiste (`windowHitAt`, `UIWindowResizer`) | Treffertest nur bei `appMode + Borderless` installiert | Ein Spiel im Borderless-Modus will keinen ziehbaren Rand. Die Geometrie liegt trotzdem in HE_Core und ist testbar. |
| Native macOS-Menüleiste, Tray, Benachrichtigungen (`AppMacMenu.mm`, `AppNotify.mm`) | Nur, wenn ein Skript `setMenuBar`/`Show Tray Icon`/`notify` ruft; Dateien in HE_Game | Kein Gate im engen Sinn, aber ein Spiel ruft es nicht. Achtung 6.2: die Einfüge-Regel in `NSApp.mainMenu` ist so gebaut, dass SDLs Leiste in Spielen erhalten bleibt. |
| Berechtigungsbits `allowFiles/Processes/Network`, `bundleId`-Autostart | Alle drei Türen zu, Autostart hinter `processes` | In jedem Projekt vorhanden, aber ein Spiel setzt sie nicht. `fs` bleibt für Spiele in `Saved/`. |
| Editor-App-Modus (Live-Vorschau, kein Play, keine Szenenmenüs, Widget-Outliner) | `appProject` / `appLivePreview` | Für Spielprojekte bleibt die ImGui-Nachbildung im Designer; E3 (echtes Engine-Rendering in der Vorschau) gilt nur im App-Projekt. |
| App-Projektvorlagen (Application, Sidebar, Wizard, Dashboard, Form, Tool) | `isAppPreset` | Angehängte Enum-Werte; die Spielvorlagen bleiben. |
| Zustellung von Menü-, Tray- und HTTP-Ereignissen | nur `GameApplication` | In der Editor-Vorschau bewusst nicht (3.3). |

Was der Plan ausdrücklich nicht baut (Abschnitt 12): „Web-Export, Mobile, ein Browser-Widget, ein Rich-Text-Editor auf Word-Niveau, ein Diagramm-Framework, ein eigenes Fenstersystem statt SDL." Und die Risiko-Liste des Plans hält fest: „Der Editor ist selbst eine ImGui-App. Die Versuchung, App-Widgets „schnell in ImGui" zu bauen, ist groß und falsch." Der Branch hat sich daran gehalten: kein einziges App-Widget ist in ImGui gebaut, der Theme-Editor ist ImGui, weil er Editor ist.

---

## 6. Grauzone: bedingungslose Verhaltensänderungen, die ein Spiel erbt

Das ist der Abschnitt, an dem die Entscheidung hängt. Eine Zeile pro Punkt: was, wen trifft es, Empfehlung. **Selbst geprüft** heißt: im Code nachgelesen. **Bericht** heißt: aus der Diff-Analyse übernommen, nicht selbst nachgelesen.

### 6.1 Selbst geprüft

| # | Was | Wen trifft es | Beleg | Empfehlung |
|---|---|---|---|---|
| 1 | `project.hcfg` wird immer v5 | Nutzer, die vorgebaute Runtime-Bundles vom Stand main unter `GameRuntimes/<Plattform>` ablegen (dokumentierter Weg, der Kommentar im Schreiber nennt ihn selbst) | `ProjectExporter.cpp:1074` setzt `bundleId` immer (leer wird zu `com.horizonengine.<name>`), `ProjectConfig.cpp:29` wählt v5 bei gefüllter Bundle-Id, main-Leser akzeptiert `version != 2 && version != k_version(3)` nicht, „bootet pak-less" | **ERLEDIGT** (`1db0f15e`): beides zusammen — v5 nur, wenn die Bundle-Id vom abgeleiteten Default abweicht ODER `appProject` gesetzt ist. Ein Spiel schreibt wieder v2. Release-Note für den, der eine eigene Id gesetzt hat. |
| 2 | `createWidget` erzeugt versteckt, Show Widget ist Pflicht | Jedes bestehende Spielprojekt mit Create Widget ohne Show Widget | `WidgetManager.cpp:803`; Commit `4039f047` zog Vorlagen und Tests nach, `HorizonCode.cpp` enthält keine Graph-Migration | **ERLEDIGT** (`7b13fc49`): keine Migration, Log-Warnung plus Release-Note. `widgetCreatorsWithoutShow` + `Runtime::addLevels`, einmal pro Klasse, mit Graph- und Widget-Namen; stumm, sobald die Referenz die lesbare Hälfte des Graphen verlässt. |
| 3 | `GameLoop::tick` verwirft den Fixed-Step-Rückstand am Cap und setzt den Akkumulator ohne Logik-Modul auf 0 | Alle Spiele bei Hitches | `GameLoop.cpp:20,45-53`, Kommentar: „carrying it is the spiral of death" | **ERLEDIGT** (Entscheidung: unverändert lassen): kein Code angefasst, in `docs/he-apps-release-notes.md` §1 als eigener Absatz dokumentiert. Es bleibt die Warnung im Log. |
| 4 | Fenstergröße wird mit `SDL_GetDisplayContentScale` multipliziert und auf die nutzbaren Bildschirmmaße geklemmt | Editor und jedes Spiel auf Windows/X11 mit Skalierung; auf macOS/Wayland ein No-op (Content Scale 1.0) | `Window.cpp:134-155`, nur `m_isPrimary` | **Behalten.** Der Plan sagt selbst: „ändert auch windowed Spiel-Fenster auf Windows/X11 mit Skalierung", nie auf echter Windows/Linux-Hardware geprüft. In die Verifikationsliste nach dem Merge. |
| 5 | Menü-, Tray-, HTTP-Zustellung nur im Spiel-Host | Editor-Vorschau von Apps | `GameApplication.cpp:2191,2201,2241`, sonst nirgends | Kenntnisnahme, ist so gewollt (3.3). |
| 6 | Der Shader-Stub-Block im Root-`CMakeLists.txt` (Zeile 628) greift auf `src/HE_Tools/` zu, bevor `add_subdirectory(src/HE_Tools)` (Zeile 783) läuft | Wer den glslang-Block auf main umbaut | eigenes Target mit absolutem Pfad, funktioniert | Kein Fehler, nur textuelle Nähe. |
| 7 | `createNewProject` schreibt `fontWeightBold = false` für **jedes** neue Projekt, Spiele eingeschlossen; ältere Projekte lesen „fehlt heißt Bold" | Neue Spielprojekte zeichnen Fließtext regular, alte bleiben Bold | `ProjectManager.cpp:1346` (schreibt `false`), `:1512` (liest Default `true`) | **ERLEDIGT** (`fbf77814`): `isApp ? false : true`. Neue Spiele bleiben fett, neue Anwendungen sind regulär. |
| 8 | `appIconName` fehlt in alten `.heproj`, der Loader nimmt `"widgets"`; neue Spiele bekommen `"sports_esports"`; der Exporter erzeugt das Icon, sobald der Name nicht leer ist, unabhängig von `appProject` | Ein bestehendes Spiel exportiert nach dem Merge mit einem generierten „widgets"-Icon, wo es vorher keins hatte | `ProjectManager.cpp:1349,1516` | **ERLEDIGT** (`fbf77814`): Loader-Default und Feld-Default sind beide leer; `writeAppIcons` sprang bei leerem Namen schon vorher ab. |
| 9 | Lua und Python erreichen `widget`, `theme`, `dialog`, `clipboard`, `process`, `json`, `prefs`, `datetime` nicht; die Registry wird nur für Gruppen aus `isScriptGroup` gespiegelt (auf HEAD: math, random, time, input, string, camera, env, entity, audio, debug, fs, save, scene, player, animator, movement, locomotion, particle, nav, physics, http, ui, app) | Jedes Lua-/Python-Projekt, Spiele und Apps, das Listen, Dialoge, Ebenen, Animationen oder Themes aus Textskripten steuern will | `EngineApi.cpp:5046-5083`, `ScriptContext.cpp:842`, `PyScriptBackend.cpp:326`; in `ScriptApi.cpp` gibt es zwar Durchreichungen (`setListCount`, `showModalWidget`), aber kein Lua-/Python-Binding ruft sie | **ERLEDIGT** (`5c27b192`): aufgenommen, je mit Kommentar. Drei Tests halten es: das Prädikat, eine Funktion pro Gruppe in Lua, dieselben acht in Python. |
| 10 | Drop-Ereignisse und `SDL_EVENT_SYSTEM_THEME_CHANGED` werden in `OnEvent` verschluckt (`return true`), unabhängig vom Modus; jeder gedroppte Pfad wird per `grantPath` für Skripte freigegeben | Alle Spiele; was in `OnEvent` danach käme, sieht die Events nicht mehr | `GameApplication.cpp:1919-1932` (Thema, `return true`), `:1934-1977` (Drop, `return true` am Blockende) | Nur verschlucken, wenn ein Widget den Drop genommen hat; sonst Kenntnisnahme. |
| 11 | Cocoa und UserNotifications werden in **jedes** macOS-Spiel gelinkt (`AppMacMenu.mm`, `AppNotify.mm` in HE_Game) | Jedes macOS-Spiel bekommt zwei Framework-Abhängigkeiten, die es nie ruft | `HE_Game/CMakeLists.txt:23-33` | Klein, hinnehmen; der Ort ist richtig (Cocoa darf nicht in HorizonCore). |
| 12 | `layer.framebufferOnly = NO` für jedes Metal-Fenster, damit Milchglas den UI-Pass lesen kann | Jedes Metal-Spiel; der Treiber darf das kompakteste Swapchain-Layout nicht mehr wählen | `MetalRenderer.mm:9468` | Messen, oder an „Backdrop-Material im Pak" knüpfen. |
| 13 | `RendererSoftware` wird auch in der `game`-Ausprägung gebaut und gelinkt, und `GameBackend = "Software"` in `config.json` ist für Spiele gültig | Ein Spiel mit dieser Einstellung lädt und tickt seine Welt, zeichnet aber nur die UI | `HE_Rendering/CMakeLists.txt:64`, `GameApplication.cpp:339` | Im Spiel-Host `Software` nur bei `appMode` zulassen, oder als Feature erklären (ein reines UI-Spiel ohne GPU ist denkbar). |

### 6.2 Aus den Berichten (nicht selbst nachgelesen)

| # | Was | Wen trifft es | Empfehlung |
|---|---|---|---|
| 14 | `saveProject` schreibt die neuen Schlüssel bedingungslos | Jede `.heproj` ändert sich beim ersten Speichern (Churn in Git/Collab) | Hinnehmen oder nur bei Nicht-Default schreiben, wie es die `.hasset`-Serialisierung durchgehend tut. |
| 15 | Jedes `.hpak` wächst um die 23 neuen Engine-Assets (14 Widgets, 9 Material-Functions), weil `addDirectories` den ganzen `Engine/`-Baum packt | Alle Exporte | Hinnehmen (klein) oder später nur referenzierte Engine-Assets packen. Dazu 6.1 #11 und #12 zu Cocoa-Link und `framebufferOnly`. |
| 16 | `UIRenderObject`-ABI: `cornerRadius` ist `vec4`, plus rund 100 Byte neue Felder; `BakedUIFont::glyphs` ist ein Vektor mit `glyph(cp)`-Zugriff statt `std::array<96>` | D3D11, D3D12, Vulkan lesen die neuen Felder nicht (der UI-Pass steht deshalb jetzt als P6 im Paritätsplan, `396e3faa`); jeder `glyphs[c-32]`-Zugriff außerhalb des Branches kompiliert nicht mehr | Kompiliert es auf Windows-CI? Der Branch hat Windows-CI grün (`runtime-flavors.yml`, Lauf 33968204444), also ja. Optische Parität auf D3D/Vulkan bleibt offen. |
| 17 | Button-Beschriftung wird beim Laden in ein Text-Kind migriert (am JSON-Schlüssel `text`, nicht an einer Version), einseitig | Ein auf dem Branch gespeichertes Widget lädt auf main ohne Beschriftung | Nur relevant, wenn jemand nach dem Merge zurückgeht. |
| 18 | `HorizonCodeCompiled.h` bekommt rund 17 neue virtuelle Methoden; `Context` wächst um zwei `std::function` | Per `HcCompiledLoader` geladene Game-Logic-Module, die gegen den alten Header gebaut wurden, haben ein anderes vtable-Layout | Release-Note: Game-Logic-Module neu bauen. Das war bei jeder HC-Erweiterung so. |
| 19 | Shader-Varianten-Codec v2 (führende 0 als Versionsmarke); ein alter Runtime liest 0 Varianten und übersetzt zur Laufzeit | Alte Runtimes mit neuen Paks | Gleicher Fall wie #1, gleiche Lösung. |
| 20 | Rund 450 KB eingebettete Fonts (Roboto Regular, Material Icons) in jedem Runtime, keine CMake-Option zum Ausschließen | Alle Builds | Hinnehmen; das Baken ist faul, nur die Bytes sind da. |
| 21 | Native macOS-Menüleiste **fügt ein** statt zu ersetzen, damit SDLs Leiste in Spielen bleibt | Nur bei `setMenuBar`-Aufruf; ohne Aufruf unverändert | Kenntnisnahme. Der Thread-Beitrag warnt ausdrücklich, dass ein `NSApp.mainMenu = eigenes` „auch in jedem ausgelieferten Spiel" SDLs Leiste wegwerfen würde. |
| 22 | `strip -x` plus Re-Sign auf jeder kopierten Runtime-Binary; Re-Sign-Fehler bricht den Export ab | Alle macOS/Linux-Exporte | Behalten (spart Dutzende MB), Fehlerfall im Build-Log sichtbar. |
| 23 | Labels lassen genau einen abschließenden Zeilenumbruch fallen (Designer und Engine) | Ein Label „Text\n" war doppelt so hoch wie es aussah | Bugfix, behalten. |
| 24 | `SplashScreen.cpp` verliert `STB_IMAGE_STATIC`, `stbi_*` bekommt Default-Sichtbarkeit in `libHorizonCore`; `HE_Editor/stb_image_impl.cpp` definiert dieselben Symbole | Interposition-Risiko, falls die stb-Versionen auseinanderlaufen | Beobachten; heute identische Version. |
| 25 | `ConstantPixel`-Skalierungsmodus bedeutet jetzt „ein Canvas-Pixel ist ein geräteunabhängiger Pixel"; wirkt nur, wenn der Host `setDisplayScale` ruft | Spiele mit ConstantPixel, deren Host das ruft (`GameApplication` tut es) | Auf HiDPI größer als vorher. Behalten, ist die richtige Bedeutung. |
| 26 | `themeStyled` ist für konstruierte Elemente `true`, für aus Datei gelesene ohne Schlüssel `false` | Neue Elemente im Designer bekommen Theme-Stile, sobald ein Theme Stile hat; `uiDefaultTheme()` hat keine, also zunächst kein optischer Unterschied | Behalten. |
| 27 | `hasFocusedTextField()` antwortet nicht mehr „irgendetwas ist fokussiert", `setCaretFromPointer` bekommt `mouseY`, `TextEdit`-Enum ist verlängert (positional in Key-Tabellen benutzt) | C++-Hosts außerhalb des Repos | Keine bekannt. |
| 28 | `he_shadercompiler` linkt als Stub, wenn glslang fehlt | Ein Game-Build ohne glslang-Fetch linkt jetzt statt zu scheitern | Andere Fehlerart, eher besser; in die Doku. |
| 29 | `UIText::align` wird `alignH`/`alignV` (JSON-Schlüssel `align` bleibt, `alignV` default Middle); `UIBoxBase::minSize` wandert auf die Basis (JSON-Schlüssel bleibt) | C++-Code, der `align` liest | Keiner außerhalb des Branches. |
| 30 | `Equals` bleibt Float-Vergleich, `String Equals` ist neu | Bestehende Graphen, die `Equals` auf Strings benutzten, waren schon vorher kaputt (alles gleich) | Release-Note. |
| 31 | Fokus und Tippen sind zwei Zustände: `setFocus(feld)` startet kein Tippen mehr, erst Enter, Leertaste oder Klick (`isEditingText`); Pfeiltasten-Navigation läuft jetzt auch bei offener Liste | Ein Spiel, das ein Namensfeld programmatisch fokussiert, braucht `activateFocused` | Release-Note. Die alte Antwort „ein Button hat die Tastatur" war falsch. |
| 32 | Positionale Startargumente werden im ersten Frame als `OnFileDropped` zugestellt und per `grantPath` freigegeben | Spiele, die mit Argumenten gestartet werden | An `appMode` binden oder als Feature erklären. |
| 33 | Tab → `focusNext`, Gamepad-East → `closeTopLayer`, Escape-Reihenfolge (Feld verlassen, Ebene schließen, dann Maus), `menuShortcutFromKey` als Erstes in jedem Key-Down | Alle Spiele; ohne Menüleiste und Ebenen Leerläufe, aber vor der bisherigen Logik | Akzeptabel, dokumentieren. |
| 34 | `app.*` Menü-, Tray- und Fensterzeilen sind im Spiel-Host gebunden (nur `requestRedraw` ist stumm); ein Spiel-Graph kann eine Menüleiste zeichnen, ein Tray-Icon setzen, das Fenster umbenennen. Der Editor bindet Tray, Menü, Notify bewusst nicht | Spiele | Entweder an `appMode` binden oder als Feature für Spiele erklären. Autostart ist gegated, der Rest nicht. |
| 35 | `dialog.*` blockiert bis zu fünf Minuten und pumpt SDL-Events, ohne sie zu dispatchen; im Spiel friert das die Welt ein | Spiele, die Dateidialoge rufen | Gewollt für modale Dialoge; ein Spiel hatte sie bisher nicht. |
| 36 | Veraltete Kommentare: `EngineApi.h:1007` und `ProjectConfig.h:74` nennen `network` „reserved, nothing reads it yet", `http::start` liest es | Leser | Zwei Kommentare korrigieren. |
| 37 | `heBackdrop` (GLSL-Bindung 9) und die GI-Schattenmaske (Bindung 10) sind auf Metal beide auf Textur 5 gepinnt (`MaterialShaderLibrary.cpp:1855-1863`); Slot-Wiederverwendung im Sinne der Tabelle, kritisch nur, wenn ein Shader beide referenziert | Metal, UI-Materialien | Der Milchglas-Commit lief auf Hardware; einen Test mit beiden Referenzen in einem Shader nachziehen. |
| 38 | `HcCodegen` kennt nur `Get/Set Property (Ref)` neu; ob die übrigen neuen Registry-Zeilen über `cppCall` automatisch im Codegen landen, hat kein Bericht geprüft | Packaged Builds mit kompiliertem HorizonCode | Paritäts-Harness (Interpreter gegen Codegen) für die neuen Zeilen laufen lassen. |
| 39 | `UICursorSDL.h` merkt sich den zuletzt gesetzten Cursor in einer statischen Variable; eine andere Stelle, die `SDL_SetCursor` direkt ruft, wird nicht gesehen | Hosts, die den Cursor selbst setzen | Kenntnisnahme. |

---

## 7. Konfliktpunkte

### 7.1 Textuell

Keine. Der Trocken-Merge ist konfliktfrei. Die drei überlappenden Dateien (`ci.yml`, `ContentManager.h/.cpp`) hat main mit `af05e59b` an anderen Stellen berührt. Die Regel aus dem Thema für `EngineApi.cpp` gilt weiter: „Gruppierung nach Kategorie schlägt Reihenfolge in der Datei", angrenzende Blöcke ohne gemeinsamen Text merged git allein (so lief `1e9e0965`).

### 7.2 Strukturell

- **Der WidgetManager-Umzug** ist die größte Verschiebung. Beim Merge mit Rename-Erkennung arbeiten (`git merge` erkennt es, `-M` beim Diff), damit main-Änderungen am alten Pfad (derzeit keine) mitwandern.
- **`.github/workflows/runtime-flavors.yml`** triggert auf `claude/he-apps-ui-framework-**` und `main`. Nach dem Merge den Branch-Filter entfernen. Der Push braucht das gh-Token mit `workflow`-Scope (Gedächtnis: `git -c credential.helper= -c credential.helper='!gh auth git-credential' push`).
- **EngineContent-Binärdateien im Repo** (14 Widgets, 9 Material-Functions): EngineContent kommt sonst per SFTP, das Repo hält nur die Struktur. Beide Generatoren (`widget_gen`, `matfn_gen`) sind deterministisch mit festen UUIDs (Blöcke 0x200, 0x300, „nur anhängen, nie umsortieren"). Zu klären: bleiben die Dateien im Repo oder gehen sie auf den Server. Der Theme-Editor-Kommentar begründet, warum die zwei Theme-Presets bewusst C++ sind: ein eingechecktes Theme-Asset wäre nach einem SFTP-Sync unregenerierbar.
- **Doku-Drift:** Header, CMake-Kommentar und Plan sagen „zwölf Bauteile", der Generator emittiert vierzehn. Harmlos wegen der Anhänge-Regel.
- **`HcEditorUtil`-API:** `drawSceneParamPicker`/`drawSaveFieldParamPicker` sind entfernt, `drawPinDefaultEditor` hat eine neue Signatur. Kein Aufrufer außerhalb von `HcGraphHost` bekannt.
- **Preferences-Rail** umsortiert (Collaboration, Source Control, Tools werden Unterseiten von Editor; neue Gruppe Project). Künftige main-Änderungen an `EditorSettingsPanel.cpp` kollidieren semantisch, nicht textuell.

### 7.3 Dateiformate

| Format | Änderung | Rückwärts lesbar auf main? |
|---|---|---|
| `project.hcfg` | v4 (Theme), v5 (Bundle-Id), neue Flag-Bits (appMode, ¬advancedShaderEffects, drei Berechtigungen, zwei Font-Bits, ¬fontWeightBold) | v2/v3 ja, v4/v5 **nein** (siehe 6.1 #1). Flag-Bits ignoriert ein alter Runtime. |
| `.heproj` | rund 14 neue Schlüssel, unbedingt geschrieben | ja, unbekannte Schlüssel werden ignoriert |
| `.hasset` (Widget) | rund 30 neue Schlüssel, alle nur bei Nicht-Default geschrieben („byte-gleich" ist die durchgehende Regel); Button-Caption-Migration einseitig | ja, bis auf Button-Beschriftung (6.2 #17) |
| `.hasset` (Theme) | neuer Asset-Typ, `AssetType::Theme` angehängt, `CHUNK_THEM` | main kennt den Typ nicht |
| Material-Varianten-Blob | v2 mit `uiVertex` | alter Runtime liest 0 Varianten (6.2 #19) |
| Material-Graph | `FnInput` mit `p[1]` Default, `p[2]` Flag; sechs Knoten angehängt | alte Graphen behalten 0.5 |
| Enums | `RendererBackend::Software = 5`, `ProjectPreset::Application…COUNT`, `UICursor` +2, `AssetType::Theme`, `UIElementType` +8, `UIEase`, `UIThemeRole` | alles angehängt, gespeicherte Werte bleiben gültig |

### 7.4 Cross-Backend

Schicht 0, Texturen und UI-Materialien existieren im UI-Pass nur auf Metal und GL (plus Software). D3D11, D3D12 und Vulkan schieben pro Quad dieselben sechs Werte wie vorher, `cornerRadius` kommt dort null mal vor. Das ist in `docs/backend-parity-plan.md` als §1.5 und Phase P6 festgehalten, mit dem Vulkan-Push-Constant-Problem (96 zu ~184 Bytes bei 128 garantierten). Kein Merge-Blocker, weil der Plan Apps auf Metal und GL beschränkt, aber ein Spiel-Widget mit Rundung sieht auf D3D anders aus als auf Metal. Die Bindungen 8 (`HeUI`) und 9 (`heBackdrop`) des Material-Graphen müssen in jedem Backend mit dem UI-Pass abgestimmt sein; auf dem Branch ist das für Metal und GL geschehen.

### 7.5 Tests und Deckung

`editor_help_audit.py --check` hat keine feste Zahl, sondern Baseline 0 offene Bedienelemente je Bereich; `test_editor_help.cpp` verlangt einen `UI Palette/<Typ>`-Eintrag für jeden Typ aus `uiWidgetTypeRegistry()`. Wer nach dem Merge einen Widget-Typ anlegt, muss `EditorHelp.cpp` mitnehmen (die zwei Gesetze aus dem Gedächtnis gelten weiter). Die drei `runtime_size`-ctests überspringen bei Nicht-Release (Exit 2); auf dem Mac ist `ctest -R runtime_size` rot, wenn `out/deploy/Game` vom Debug-Baum geschrieben wurde (Thema-Beitrag vom 05.09.).

---

## 8. Offene Lücken (nicht Teil der 23 Schritte)

Aus der Abschlussbilanz und den Warnungen im Thema:

- ~~`app.minimize`, `app.maximize`, `app.isMaximized` fehlen~~ **erledigt am 06.09.2026** (`4c1a46b6`): drei Registry-Zeilen, `Window::Minimize/Maximize/Restore/IsMaximized`, gebunden im ausgelieferten Wirt und bewusst nicht im Editor. `maximize` nimmt einen Wahrheitswert und deckt damit auch das Zurücksetzen ab.
- ~~Menü-Einträge ohne `enabled`/`checked`~~ **erledigt am 06.09.2026** (`bf03b40b`): zwei Felder an `AppMenuItem`, gezeichnete Leiste und macOS-Systemleiste, vier Skriptzeilen. Gesperrt heißt auch: der Akkord feuert nicht mehr, wird aber geschluckt.
- Auswählbarer statischer Text (B6-Rest).
- `db` und `print` aus Block C, `timer` bewusst nicht gebaut.
- ~~Vier Kontrastbefunde absichtlich offen~~ **erledigt am 06.09.2026** (`729a19f5`): `UIThemeRole::AccentText` als zehnte Rolle, in beiden mitgelieferten Themes belegt, OK-Beschriftung und Badge daran gebunden, Ausnahme im ctest entfernt. Die Zahl im Plan war falsch: Weiß auf dem hellen Akzent ist 4,75, nicht 4,4.
- ~~Lösch-Kreuz im Suchfeld~~ **erledigt am 06.09.2026**: `UITextInput::clearButton` — die „Logik, die eine Komponente nicht trägt", gehört dem Feld, nicht einem Element daneben.
- Akkordeon (B5), Tabelle mit Kopfzeile (B2b), mehrere Fenster (A5), Datei-Watcher ist gebaut (`fs.watch`, `bfb3f28e`), aber die Zustellung läuft nur im Spiel-Host.
- Nie auf echter Hardware geprüft: Window-DIP-Skalierung auf Windows/X11, GL-Runtime der Schicht 0, Autostart auf Windows und Linux (blind gebaut), `notify` auf Windows (nur Warnung).
- `docs/ui-theme-css.md` ist eine Bewertung, „Nothing here is built."

---

## 9. Merge-Checkliste

Vor dem Merge:

1. Alle Engine-Themen (Decals v2 und was noch aufgemacht wird) sind auf main. Dieser Branch geht als letzter.
2. ~~6.1 #1 fixen: `project.hcfg` v5 nur bei nicht ableitbarer Bundle-Id oder nur bei `appMode`.~~ **ERLEDIGT** (`1db0f15e`): beides, die Vereinigung der zwei Bedingungen. `ProjectExporter.cpp` lässt `cfg.bundleId` leer, wenn sie gleich der aus dem Namen abgeleiteten ist und das Projekt kein `appProject` ist; ein Spiel schreibt damit wieder v2. Test: `test_export_profiles.cpp`, „Export: a game keeps its project.hcfg readable by an older runtime bundle" (Versionswort nach dem Magic, vier Fälle).
3. ~~6.1 #2 entscheiden: Graph-Migration für Create Widget oder Release-Note.~~ **ENTSCHIEDEN und ERLEDIGT** (`7b13fc49`): keine Migration. `HorizonCode::widgetCreatorsWithoutShow` plus eine Warnung in `Runtime::addLevels`, einmal pro Klasse, mit Graph- und Widget-Namen. Release-Note-Entwurf in `docs/he-apps-release-notes.md`.
4. ~~6.1 #7 und #8 entscheiden: Schriftgewicht und Icon-Default für Spielprojekte.~~ **ERLEDIGT** (`fbf77814`): `fontWeightBold` ist `isApp ? false : true`, `appIconName` bleibt beim Laden leer. Nebenbefund mitbehoben: `createNewProject` spiegelte beide nie in das Projekt, das der Editor hält.
5. 6.1 #13 und 6.2 #34 entscheiden: Software-Backend und `app.*`-Menü/Tray im Spiel-Host an `appMode` binden oder als Feature erklären.
6. ~~6.1 #9 entscheiden: acht Gruppen in `isScriptGroup` aufnehmen oder als HorizonCode-only dokumentieren.~~ **ERLEDIGT** (`5c27b192`): aufgenommen. Drei Tests — das Prädikat, eine Funktion pro Gruppe in Lua, dieselben acht in Python.
7. Zwei veraltete `network`-Kommentare korrigieren (6.2 #36).
8. main noch einmal hereinholen, voller ctest (Release, seriell wegen `runtime_size`), `he_tests` komplett, Paritäts-Harness für die neuen Registry-Zeilen (6.2 #38).
9. Branch-Filter in `runtime-flavors.yml` bereinigen (braucht das gh-Token).
10. PR statt direktem Push, CI auf allen drei Plattformen abwarten (die Windows- und Linux-Fehler dieses Branches wurden alle nur dort gefunden).

Nach dem Merge:

11. Release-Note mit: Show Widget Pflicht, Fokus ist nicht Tippen, Fixed-Step-Verwurf, Game-Logic-Module neu bauen, Runtime-Bundles neu bauen, `String Equals`, Fenstergröße auf Windows/X11 mit Skalierung, Startargumente als `OnFileDropped`, Drop-Events werden verschluckt. **Entwurf steht** in `docs/he-apps-release-notes.md` (`7b13fc49`) — er deckt die ersten fünf plus die drei Vorgaben aus Punkt 2/4 oben; Fenstergröße, Startargumente und Drop-Events fehlen darin noch.
12. Verifikationsliste auf echter Hardware: Windows-Fenstergröße, GL-Schicht-0, D3D/Vulkan-UI-Parität (P6), Autostart Windows/Linux, `framebufferOnly`-Kosten auf Metal.
13. Entscheidung zum ECS-UI (abbilden oder entfernen) als eigenes Thema aufmachen.
14. EngineContent-Binärdateien: Repo oder SFTP.
15. Gedächtnis und Masterplan-Logbuch nachziehen (Kurzstand-Register Block A/E).
