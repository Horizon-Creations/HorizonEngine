# HE Apps: Verschmelzungs-Analyse für den Merge nach main

**Stand:** 06.09.2026, Branch `claude/he-apps-ui-framework-83b5f7` (HEAD `6902a5c5`), Merge-Base `e38eabfa`, main `af05e59b`.
**Zweck:** Entscheidungsgrundlage für den Menschen. Kein Code, keine Änderung, nur Analyse. Grundlage sind `docs/he-apps-plan.md`, der vollständige Diff `main...HEAD` (178 Commits, 194 Dateien, +73 495 / −3 678 Zeilen) und die 80 Beiträge im Hive-Thema.
**Lesehinweis:** Abschnitt 1 ist die Kurzfassung. Wer nur eine Seite liest, liest die. Abschnitt 6 (Grauzone) ist der Teil, an dem die eigentliche Entscheidung hängt.

---

## 1. Kurzfassung und Empfehlung

**Der Branch ist mergefähig, aber nicht als „App-Feature", sondern als Engine-Umbau mit einem App-Schalter obendrauf.** Etwa neun Zehntel des Diffs liegen in HE_Core und sind für alle Nutzer wirksam, Spiele eingeschlossen. Das ist kein Unfall, sondern die Bauweise, die der Plan von Anfang an vorgab: „kein neues Teilsystem, sondern ein zweiter Betriebsmodus für Dinge, die größtenteils schon existieren". Der App-Modus selbst ist klein: ein Bit in `project.hcfg`, ein Dutzend `if (m_appMode)`-Verzweigungen in `GameApplication`, ein CMake-Schalter für die Runtime-Ausprägung.

**Drei Dinge sind geprüft und in Ordnung:**

- Ein Trocken-Merge (`git merge-tree --write-tree main HEAD`) ist **konfliktfrei**. main ist seit dem Abzweig um genau einen Commit weiter (`af05e59b`, ContentManager plus `ci.yml`), der Branch hat main am 01.09. bereits einmal hereingeholt (`1e9e0965`).
- Es gibt **keinen alten und keinen neuen WidgetManager nebeneinander**. Die Klasse ist in `5c49fbc1` von HE_Scene nach HE_Core umgezogen (Datei gelöscht, Datei angelegt, sieben Include-Stellen umgeschrieben). Die Aufgabenbeschreibung, die einen Konflikt mit dem „alten HE_Scene::WidgetManager" vermutet, geht von einer Verdopplung aus, die es nicht gibt.
- Das ECS-UI (`UISystem`, `UIInputSystem`, `UIElementComponent`) ist **eingefroren, nicht entfernt**: neun geänderte Zeilen im ganzen Diff, alle wegen des Umzugs. Der Plan hat das in Abschnitt 9(a) so entschieden („der Widget-Baum gewinnt, das ECS-UI wird eingefroren").

**Fünf Dinge sind geprüft und brauchen vor dem Merge eine Entscheidung** (Details in 6.1; die ersten drei wiegen am schwersten):

1. **`project.hcfg` wird ab jetzt immer als v5 geschrieben, auch für Spiele.** Der Exporter füllt `bundleId` bedingungslos, der Schreiber wählt bei gefüllter Bundle-Id v5, und der Leser auf main akzeptiert nur v2 und v3. Ein vom Nutzer unter `GameRuntimes/<Plattform>` abgelegtes Runtime-Bundle vom Stand main bootet danach ohne Pak. Das bricht still einen dokumentierten Arbeitsablauf.
2. **`createWidget` erzeugt versteckt** (`WidgetManager.cpp:803`, Commit `4039f047`). Show Widget ist jetzt Pflicht. Die Projektvorlagen und die Tests wurden nachgezogen, eine automatische Migration bestehender Graphen gibt es nicht. Jedes bestehende Spielprojekt, das Create Widget ohne Show Widget benutzt, zeigt nach dem Merge nichts mehr.
3. **`GameLoop` verwirft den Fixed-Step-Rückstand am Step-Cap** statt ihn weiterzutragen, bedingungslos. Der Kommentar begründet es als die richtige Antwort auch für Spiele („Spiral of Death"). Die Änderung ist vertretbar, aber sie muss gewusst sein.
4. **Neue Spielprojekte bekommen regulären statt fetten UI-Text** (`fontWeightBold = false` für jedes neue Projekt), alte Projekte bleiben Bold. Eine Zeile, aber eine sichtbare.
5. **Bestehende Spiele bekommen beim nächsten Export ein generiertes „widgets"-Icon**, weil der Loader den fehlenden Icon-Namen mit einem Default füllt und der Exporter das Icon unabhängig vom Projekttyp erzeugt.

**Empfehlung:** Den Branch als Ganzes mergen, nicht in Teilen. Die Abhängigkeitskette (HE_Core-Widgets, HE_Tools-Projektfelder, HE_Scene-Registry, Editor) lässt sich nicht sauber trennen, und der Trocken-Merge zeigt, dass es technisch nichts zu trennen gibt. Vorher die drei Punkte oben abarbeiten (Punkt 1 ist eine Zeile im Exporter, Punkt 2 eine Entscheidung zwischen Migration und Release-Note, Punkt 3 eine Kenntnisnahme). Die Merge-Reihenfolge, die der Mensch am 05.09. festgelegt hat, bleibt: **dieser Branch geht als letzter**, nach jedem Engine-Thema, und zieht main vorher noch einmal nach.

---

## 2. Faktenlage

| Größe | Wert |
|---|---|
| Commits auf dem Branch (main..HEAD) | 178 |
| Geänderte Dateien | 194 (+73 495 / −3 678) |
| Davon Vendor-Header (Fonts, stb_image_write) | ca. 16 700 Zeilen |
| Davon Tests | ca. 14 000 Zeilen (`test_ui_widgets.cpp` allein +12 881) |
| main seit Merge-Base | 1 Commit (`af05e59b`) |
| Überlappende Dateien beider Seiten | 3 (`ci.yml`, `ContentManager.h`, `ContentManager.cpp`) |
| Trocken-Merge `git merge-tree` | konfliktfrei |
| ctest (letzter Stand im Thema, `6902a5c5`) | 120/120, `he_tests` 2393/2393 |
| Hilfe-Deckung Editor | 689/689 Bedienelemente, 972 Einträge |
| Schritte im Hive-Thema | 23 von 23 abgeschlossen |

**Merge-Reihenfolge (Anweisung vom Menschen, über den Leitstand am 05.09.):** Alle „normale Engine"-Themen (aktuell Thema 8, Decals) gehen vor diesem Branch nach main. Dieser Branch bleibt der letzte, zieht main nach jedem durchgegangenen Engine-Thema nach und geht selbst erst, wenn er fertig ist und alle Engine-Themen drin sind. Danach: voller ctest, dann PR statt direktem Push, damit die CI drüberläuft.

**Zu prüfen für den Menschen:** Der Leitstand hat gemeldet, dass `claude/decals-cross-backend` versehentlich von diesem Branch abgezweigt war. Der Nachfolger `claude/decals-cross-backend-v2` hat seine Merge-Base auf `af05e59b`, also sauber auf main. Der alte Decals-Branch hat seine Merge-Base auf `e38eabfa` und sollte nicht mehr verwendet werden.

**Bekannte Restbaustellen aus dem Thema** (nicht Teil der 23 Schritte, siehe Abschnitt 8): `app.minimize`, `app.maximize`, `isMaximized` fehlen (eine eigene Titelleiste hat damit nur einen von drei Knöpfen), auswählbarer statischer Text, Menü `enabled`/`checked`, `db` und `print` aus Block C, vier bewusst offene Kontrastbefunde, Lösch-Kreuz im Suchfeld.

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

**Zustellungs-Asymmetrie, geprüft:** `fireOnMenuItem`, `fireOnTrayItem` und `fireOnHttpResponse` werden nur in `GameApplication.cpp` aufgerufen (Zeilen 2191, 2201, 2241). In der Editor-Vorschau kommen Menü-, Tray- und HTTP-Ereignisse nicht an. Der Plan sagt das so; es ist eine bewusste Lücke, keine vergessene.

### 3.4 Die drei Runtime-Ausprägungen

`HE_RUNTIME_FLAVOR` (`game | app-advanced | app-basic`) ist ein Baum-Schalter, kein Ziel-Schalter: „an app runtime is its OWN build directory (scripts/build_runtimes.py drives them), never a second output of the game build". Default ist `game`, und der Kommentar verspricht Byte-Identität des Game-Builds mit dem Zustand vor dem Schalter. Die beiden App-Ausprägungen zwingen `HE_ENABLE_SHADERC=OFF`, bauen keinen Editor, und linken genau einen Renderer (`AppAdvanced`: Metal beziehungsweise GL; `AppBasic`: `RendererSoftware`). Der Exporter wählt die Ausprägung aus dem, was das Projekt ist (`runtimeFlavorFor(appMode, advancedShaderEffects)`, für Spiele immer `Game`) und fällt auf `Game` zurück, wenn das Bundle fehlt.

Gemessen (Release, macOS/arm64): app-advanced 22,2 MB ohne Python (Rendering 1,1 MB), app-basic 21,5 MB (Rendering 0,4 MB). Die CI (`runtime-flavors.yml`) misst seit dem 05.09. alle drei Ausprägungen auf allen drei Plattformen gegen Schwellen.

### 3.5 Das Berechtigungsmodell

Der Ein-Satz-Vertrag aus Block C, wörtlich aus dem Plan: „**Die Berechtigung sagt, was ein SKRIPT von sich aus benennen darf. Sie sagt nie, was ein MENSCH auswählen darf.** Ein Pfad, den jemand in einem Dateidialog gewählt hat, ist danach frei, das Auswählen IST die Erlaubnis." Drei Türen (`allowFiles`, `allowProcesses`, `allowNetwork`), alle zu, gespeichert „gerade" (fehlend heißt zu), im Gegensatz zu `advancedShaderEffects` und `fontWeightBold`, die negiert gespeichert werden, weil ihr ehrlicher Default „an" ist. Drop und „Öffnen mit" gehen durch dieselbe Tür wie der Dateidialog (`fs::grantPath` im Host). Die Flags stehen in jedem Projekt, auch in Spielen; ein Spiel, das `fs` außerhalb von `Saved/` will, kann sie setzen.

### 3.6 Wo die Cocoa-Dateien liegen

`AppMacMenu.mm` und `AppNotify.mm` liegen in HE_Game, nicht in HE_Core: „Cocoa dort zöge AppKit in he_tests, hc_codegen, widget_gen und die Windows-CI." Die Editor-Menüleiste (`HE_Editor/MacMenuBar.mm`) ist davon unabhängig und teilt keinen Code.

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
| 1 | `project.hcfg` wird immer v5 | Nutzer, die vorgebaute Runtime-Bundles vom Stand main unter `GameRuntimes/<Plattform>` ablegen (dokumentierter Weg, der Kommentar im Schreiber nennt ihn selbst) | `ProjectExporter.cpp:1074` setzt `bundleId` immer (leer wird zu `com.horizonengine.<name>`), `ProjectConfig.cpp:29` wählt v5 bei gefüllter Bundle-Id, main-Leser akzeptiert `version != 2 && version != k_version(3)` nicht, „bootet pak-less" | **Vor dem Merge fixen:** v5 nur schreiben, wenn `bundleId` vom ableitbaren Default abweicht (oder v5 nur bei `appMode`). Sonst Release-Note: Runtime-Bundles müssen neu gebaut werden. |
| 2 | `createWidget` erzeugt versteckt, Show Widget ist Pflicht | Jedes bestehende Spielprojekt mit Create Widget ohne Show Widget | `WidgetManager.cpp:803`; Commit `4039f047` zog Vorlagen und Tests nach, `HorizonCode.cpp` enthält keine Graph-Migration | **Entscheidung nötig:** Migration im HC-Loader (ein Show Widget hinter jedes Create Widget ohne folgendes Show) oder Release-Note plus Log-Warnung. Der Plan spricht von einem „Migrationshinweis", nicht von einer Migration. |
| 3 | `GameLoop::tick` verwirft den Fixed-Step-Rückstand am Cap und setzt den Akkumulator ohne Logik-Modul auf 0 | Alle Spiele bei Hitches | `GameLoop.cpp:20,45-53`, Kommentar: „carrying it is the spiral of death" | **Behalten,** in der Release-Note nennen. Es bleibt die Warnung im Log. |
| 4 | Fenstergröße wird mit `SDL_GetDisplayContentScale` multipliziert und auf die nutzbaren Bildschirmmaße geklemmt | Editor und jedes Spiel auf Windows/X11 mit Skalierung; auf macOS/Wayland ein No-op (Content Scale 1.0) | `Window.cpp:134-155`, nur `m_isPrimary` | **Behalten.** Der Plan sagt selbst: „ändert auch windowed Spiel-Fenster auf Windows/X11 mit Skalierung", nie auf echter Windows/Linux-Hardware geprüft. In die Verifikationsliste nach dem Merge. |
| 5 | Menü-, Tray-, HTTP-Zustellung nur im Spiel-Host | Editor-Vorschau von Apps | `GameApplication.cpp:2191,2201,2241`, sonst nirgends | Kenntnisnahme, ist so gewollt (3.3). |
| 6 | Der Shader-Stub-Block im Root-`CMakeLists.txt` (Zeile 628) greift auf `src/HE_Tools/` zu, bevor `add_subdirectory(src/HE_Tools)` (Zeile 783) läuft | Wer den glslang-Block auf main umbaut | eigenes Target mit absolutem Pfad, funktioniert | Kein Fehler, nur textuelle Nähe. |
| 7 | `createNewProject` schreibt `fontWeightBold = false` für **jedes** neue Projekt, Spiele eingeschlossen; ältere Projekte lesen „fehlt heißt Bold" | Neue Spielprojekte zeichnen Fließtext regular, alte bleiben Bold | `ProjectManager.cpp:1346` (schreibt `false`), `:1512` (liest Default `true`) | Entscheiden, ob Spiele weiter Bold bekommen sollen (`isApp ? false : true`, eine Zeile). |
| 8 | `appIconName` fehlt in alten `.heproj`, der Loader nimmt `"widgets"`; neue Spiele bekommen `"sports_esports"`; der Exporter erzeugt das Icon, sobald der Name nicht leer ist, unabhängig von `appProject` | Ein bestehendes Spiel exportiert nach dem Merge mit einem generierten „widgets"-Icon, wo es vorher keins hatte | `ProjectManager.cpp:1349,1516` | Beim Laden leer lassen und das Icon nur erzeugen, wenn der Name gesetzt ist. |

### 6.2 Aus den Berichten (nicht selbst nachgelesen)

| # | Was | Wen trifft es | Empfehlung |
|---|---|---|---|
| 9 | `saveProject` schreibt die neuen Schlüssel bedingungslos | Jede `.heproj` ändert sich beim ersten Speichern (Churn in Git/Collab) | Hinnehmen oder nur bei Nicht-Default schreiben, wie es die `.hasset`-Serialisierung durchgehend tut. |
| 10 | Jedes `.hpak` wächst um die 23 neuen Engine-Assets (14 Widgets, 9 Material-Functions), weil `addDirectories` den ganzen `Engine/`-Baum packt | Alle Exporte | Hinnehmen (klein) oder später nur referenzierte Engine-Assets packen. |
| 11 | `UIRenderObject`-ABI: `cornerRadius` ist `vec4`, plus rund 100 Byte neue Felder; `BakedUIFont::glyphs` ist ein Vektor mit `glyph(cp)`-Zugriff statt `std::array<96>` | D3D11, D3D12, Vulkan lesen die neuen Felder nicht (der UI-Pass steht deshalb jetzt als P6 im Paritätsplan, `396e3faa`); jeder `glyphs[c-32]`-Zugriff außerhalb des Branches kompiliert nicht mehr | Kompiliert es auf Windows-CI? Der Branch hat Windows-CI grün (`runtime-flavors.yml`, Lauf 33968204444), also ja. Optische Parität auf D3D/Vulkan bleibt offen. |
| 12 | Button-Beschriftung wird beim Laden in ein Text-Kind migriert (am JSON-Schlüssel `text`, nicht an einer Version), einseitig | Ein auf dem Branch gespeichertes Widget lädt auf main ohne Beschriftung | Nur relevant, wenn jemand nach dem Merge zurückgeht. |
| 13 | `HorizonCodeCompiled.h` bekommt rund 17 neue virtuelle Methoden; `Context` wächst um zwei `std::function` | Per `HcCompiledLoader` geladene Game-Logic-Module, die gegen den alten Header gebaut wurden, haben ein anderes vtable-Layout | Release-Note: Game-Logic-Module neu bauen. Das war bei jeder HC-Erweiterung so. |
| 14 | Shader-Varianten-Codec v2 (führende 0 als Versionsmarke); ein alter Runtime liest 0 Varianten und übersetzt zur Laufzeit | Alte Runtimes mit neuen Paks | Gleicher Fall wie #1, gleiche Lösung. |
| 15 | Rund 450 KB eingebettete Fonts (Roboto Regular, Material Icons) in jedem Runtime, keine CMake-Option zum Ausschließen | Alle Builds | Hinnehmen; das Baken ist faul, nur die Bytes sind da. |
| 16 | Native macOS-Menüleiste **fügt ein** statt zu ersetzen, damit SDLs Leiste in Spielen bleibt | Nur bei `setMenuBar`-Aufruf; ohne Aufruf unverändert | Kenntnisnahme. Der Thread-Beitrag warnt ausdrücklich, dass ein `NSApp.mainMenu = eigenes` „auch in jedem ausgelieferten Spiel" SDLs Leiste wegwerfen würde. |
| 17 | `strip -x` plus Re-Sign auf jeder kopierten Runtime-Binary; Re-Sign-Fehler bricht den Export ab | Alle macOS/Linux-Exporte | Behalten (spart Dutzende MB), Fehlerfall im Build-Log sichtbar. |
| 18 | Labels lassen genau einen abschließenden Zeilenumbruch fallen (Designer und Engine) | Ein Label „Text\n" war doppelt so hoch wie es aussah | Bugfix, behalten. |
| 19 | `SplashScreen.cpp` verliert `STB_IMAGE_STATIC`, `stbi_*` bekommt Default-Sichtbarkeit in `libHorizonCore`; `HE_Editor/stb_image_impl.cpp` definiert dieselben Symbole | Interposition-Risiko, falls die stb-Versionen auseinanderlaufen | Beobachten; heute identische Version. |
| 20 | `ConstantPixel`-Skalierungsmodus bedeutet jetzt „ein Canvas-Pixel ist ein geräteunabhängiger Pixel"; wirkt nur, wenn der Host `setDisplayScale` ruft | Spiele mit ConstantPixel, deren Host das ruft (`GameApplication` tut es) | Auf HiDPI größer als vorher. Behalten, ist die richtige Bedeutung. |
| 21 | `themeStyled` ist für konstruierte Elemente `true`, für aus Datei gelesene ohne Schlüssel `false` | Neue Elemente im Designer bekommen Theme-Stile, sobald ein Theme Stile hat; `uiDefaultTheme()` hat keine, also zunächst kein optischer Unterschied | Behalten. |
| 22 | `hasFocusedTextField()` antwortet nicht mehr „irgendetwas ist fokussiert", `setCaretFromPointer` bekommt `mouseY`, `TextEdit`-Enum ist verlängert (positional in Key-Tabellen benutzt) | C++-Hosts außerhalb des Repos | Keine bekannt. |
| 23 | `he_shadercompiler` linkt als Stub, wenn glslang fehlt | Ein Game-Build ohne glslang-Fetch linkt jetzt statt zu scheitern | Andere Fehlerart, eher besser; in die Doku. |
| 24 | `UIText::align` wird `alignH`/`alignV` (JSON-Schlüssel `align` bleibt, `alignV` default Middle); `UIBoxBase::minSize` wandert auf die Basis (JSON-Schlüssel bleibt) | C++-Code, der `align` liest | Keiner außerhalb des Branches. |
| 25 | `Equals` bleibt Float-Vergleich, `String Equals` ist neu | Bestehende Graphen, die `Equals` auf Strings benutzten, waren schon vorher kaputt (alles gleich) | Release-Note. |

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
| `.hasset` (Widget) | rund 30 neue Schlüssel, alle nur bei Nicht-Default geschrieben („byte-gleich" ist die durchgehende Regel); Button-Caption-Migration einseitig | ja, bis auf Button-Beschriftung (#12) |
| `.hasset` (Theme) | neuer Asset-Typ, `AssetType::Theme` angehängt, `CHUNK_THEM` | main kennt den Typ nicht |
| Material-Varianten-Blob | v2 mit `uiVertex` | alter Runtime liest 0 Varianten (#14) |
| Material-Graph | `FnInput` mit `p[1]` Default, `p[2]` Flag; sechs Knoten angehängt | alte Graphen behalten 0.5 |
| Enums | `RendererBackend::Software = 5`, `ProjectPreset::Application…COUNT`, `UICursor` +2, `AssetType::Theme`, `UIElementType` +8, `UIEase`, `UIThemeRole` | alles angehängt, gespeicherte Werte bleiben gültig |

### 7.4 Cross-Backend

Schicht 0, Texturen und UI-Materialien existieren im UI-Pass nur auf Metal und GL (plus Software). D3D11, D3D12 und Vulkan schieben pro Quad dieselben sechs Werte wie vorher, `cornerRadius` kommt dort null mal vor. Das ist in `docs/backend-parity-plan.md` als §1.5 und Phase P6 festgehalten, mit dem Vulkan-Push-Constant-Problem (96 zu ~184 Bytes bei 128 garantierten). Kein Merge-Blocker, weil der Plan Apps auf Metal und GL beschränkt, aber ein Spiel-Widget mit Rundung sieht auf D3D anders aus als auf Metal. Die Bindungen 8 (`HeUI`) und 9 (`heBackdrop`) des Material-Graphen müssen in jedem Backend mit dem UI-Pass abgestimmt sein; auf dem Branch ist das für Metal und GL geschehen.

### 7.5 Tests und Deckung

`editor_help_audit.py --check` hat keine feste Zahl, sondern Baseline 0 offene Bedienelemente je Bereich; `test_editor_help.cpp` verlangt einen `UI Palette/<Typ>`-Eintrag für jeden Typ aus `uiWidgetTypeRegistry()`. Wer nach dem Merge einen Widget-Typ anlegt, muss `EditorHelp.cpp` mitnehmen (die zwei Gesetze aus dem Gedächtnis gelten weiter). Die drei `runtime_size`-ctests überspringen bei Nicht-Release (Exit 2); auf dem Mac ist `ctest -R runtime_size` rot, wenn `out/deploy/Game` vom Debug-Baum geschrieben wurde (Thema-Beitrag vom 05.09.).

---

## 8. Offene Lücken (nicht Teil der 23 Schritte)

Aus der Abschlussbilanz und den Warnungen im Thema:

- `app.minimize`, `app.maximize`, `app.isMaximized` fehlen. Die eigene Titelleiste (F3) hat damit nur einen von drei Knöpfen (`app.quit`). Der Bearbeiter von Schritt 21 nannte es „die nächste Hälfte derselben Sache".
- Menü-Einträge ohne `enabled`/`checked`.
- Auswählbarer statischer Text (B6-Rest).
- `db` und `print` aus Block C, `timer` bewusst nicht gebaut.
- Vier Kontrastbefunde absichtlich offen: Text auf Akzentfläche (3.7 hell, 2.5 dunkel), weil dem Vokabular eine Theme-Rolle für Text auf Akzent fehlt.
- Lösch-Kreuz im Suchfeld („braucht Logik, die eine Komponente heute nicht trägt").
- Akkordeon (B5), Tabelle mit Kopfzeile (B2b), mehrere Fenster (A5), Datei-Watcher ist gebaut (`fs.watch`, `bfb3f28e`), aber die Zustellung läuft nur im Spiel-Host.
- Nie auf echter Hardware geprüft: Window-DIP-Skalierung auf Windows/X11, GL-Runtime der Schicht 0, Autostart auf Windows und Linux (blind gebaut), `notify` auf Windows (nur Warnung).
- `docs/ui-theme-css.md` ist eine Bewertung, „Nothing here is built."

---

## 9. Merge-Checkliste

Vor dem Merge:

1. Alle Engine-Themen (Decals v2 und was noch aufgemacht wird) sind auf main. Dieser Branch geht als letzter.
2. 6.1 #1 fixen: `project.hcfg` v5 nur bei nicht ableitbarer Bundle-Id oder nur bei `appMode`.
3. 6.1 #2 entscheiden: Graph-Migration für Create Widget oder Release-Note.
4. 6.1 #7 und #8 entscheiden: Schriftgewicht und Icon-Default für Spielprojekte.
5. main noch einmal hereinholen, voller ctest (Release, seriell wegen `runtime_size`), `he_tests` komplett.
6. Branch-Filter in `runtime-flavors.yml` bereinigen (braucht das gh-Token).
7. PR statt direktem Push, CI auf allen drei Plattformen abwarten (die Windows- und Linux-Fehler dieses Branches wurden alle nur dort gefunden).

Nach dem Merge:

8. Release-Note mit: Show Widget Pflicht, Fixed-Step-Verwurf, Game-Logic-Module neu bauen, Runtime-Bundles neu bauen, `String Equals`, Fenstergröße auf Windows/X11 mit Skalierung.
9. Verifikationsliste auf echter Hardware: Windows-Fenstergröße, GL-Schicht-0, D3D/Vulkan-UI-Parität (P6), Autostart Windows/Linux.
10. Entscheidung zum ECS-UI (abbilden oder entfernen) als eigenes Thema aufmachen.
11. EngineContent-Binärdateien: Repo oder SFTP.
12. Gedächtnis und Masterplan-Logbuch nachziehen (Kurzstand-Register Block A/E).
