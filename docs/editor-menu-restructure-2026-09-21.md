# Editor-Menüführung: Neugliederung der Menüs (Thema 73, Schritt 2/5, 21.09.2026)

Umsetzung zu `docs/editor-menu-audit-2026-09-21.md` (Bestandsaufnahme). Dieses
Dokument ist die **Karte alt → neu** für die Hauptmenüleiste (ImGui auf
Windows/Linux, nativ auf macOS) und zugleich die Paritäts-Checkliste für
Schritt 4: jede Zeile der alten Menüs hat hier einen neuen Ort.

Nicht Teil dieses Schritts (Schritt 3): Panels, Add-Component-Popup,
Content-Browser-Kopfzeile, Outliner-Kopf. Die Viewport-Toolbar selbst ist
unverändert; ihre Popups sind jetzt zusätzlich als Untermenüs im View-Menü
erreichbar.

---

## 1. Die Regel hinter der Gliederung

| Menü | Enthält | Regel |
|---|---|---|
| **File** | Projekte, Szenen, Speichern, Import, Beenden | Dateien rein und raus |
| **Edit** | Undo/Redo, Cut/Copy/Paste/Duplicate/Delete (Entities), Project Settings, Preferences | Bearbeiten und Einstellen |
| **Entity** *(neu, nur Spielprojekte)* | Create ▸, Focus, Snap to Ground, Hide/Isolate/Show All, Group/Ungroup, Lock, Save as Prefab | Verben der beiden Kontextmenüs mit fester Adresse |
| **Assets** | Create Asset…, Import Asset…, Refresh Assets, (Dev: Publish/Rebuild) | Der Content-Baum |
| **Play** *(neu)* | Play/Stop, Pause/Resume/Continue, Step Frame, Step Node | Die Transport-Zelle der Toolbar, mit Tasten |
| **Build** | Build and Reload Game Logic, Export Project… | Übersetzen und Paketieren |
| **View** | View Mode ▸, Show ▸, Camera ▸, Toggle Fullscreen | Wie das Scene-Fenster zeichnet; **öffnet nichts** |
| **Window** *(neu auf Windows/Linux, erweitert auf macOS)* | Panels, Scene 2/3/4, Level Script, Game Instance, Landscape Tools, Reset Layout | Alles, was ein Fenster oder einen Tab öffnet |
| **Help** | unverändert | |

Sieben Menüs wurden neun; dafür hat das alte View-Menü (16 Zeilen, vier
Arten von Einträgen) keinen Mischbetrieb mehr, und die beiden häufigsten
Handlungen (Play, Entity anlegen) haben eine Adresse in der Leiste.

---

## 2. Karte alt → neu (jede alte Zeile)

„=" bedeutet unverändert am selben Ort.

### File

| alt | neu | Bemerkung |
|---|---|---|
| New Project (Label „Ctrl+N", ungebunden) | File ▸ New Project (**Ctrl+N gebunden**, `file.newProject`) | Registry-Eintrag, in Preferences ▸ Shortcuts umbelegbar |
| Open Project | = | |
| — | **File ▸ Recent Projects ▸** | die Hub-Liste; nicht existierende Pfade ausgegraut; öffnet über den Unsaved-Guard (`GuardedAction::OpenProjectPath`) |
| Close Project (Label „Ctrl+W", ungebunden) | File ▸ Close Project (**Ctrl+W gebunden**, `file.closeProject`) | bewusst Close *Project* (nicht Tab), wie ⌘W auf dem Mac seit jeher; ein Close-Tab-Befehl wäre ein neues Verb |
| New Scene / Open Scene… / Add Scene Additive… | = | jetzt auch in ImGui auf „Projekt geladen" gegated (Mac war das schon) |
| Save / Save All / Save Scene As… | = | |
| — | **File ▸ Import Asset…** | zweite Tür auf dieselbe Aktion wie Assets ▸ Import Asset… |
| Exit | = | |

### Edit

| alt | neu | Bemerkung |
|---|---|---|
| Undo / Redo | = | |
| Cut / Copy / Paste / Duplicate / Delete | = | **macOS hat sie jetzt auch** (ohne Key-Equivalents, wie Undo/Redo, damit ⌘C/⌘V bei Textfeldern bleiben); in App-Projekten ausgeblendet |
| Project Settings / Preferences | = | |

### View (alt) → View / Window (neu)

| alt | neu |
|---|---|
| Toggle Fullscreen | View ▸ Toggle Fullscreen |
| Reset Layout | Window ▸ Reset Layout |
| Performance Profiler, Environment, Collaboration, Source Control, Console, Audio Mixer, Undo History, Watch | Window ▸ *(gleiche Namen, gleiche Reihenfolge, Console zuerst mit Ctrl+`)* |
| Ground Grid | View ▸ Show ▸ Ground Grid (Zeile 1 des Show-Untermenüs) |
| Scene 2 / 3 / 4 | Window ▸ Scene 2 / 3 / 4 |
| Level Script, Game Instance | Window ▸ Level Script / Game Instance |
| — | View ▸ View Mode ▸ (Lit / Unlit / Wireframe / G-Buffer …) — dieselben Zeilen wie das Toolbar-Popup (`ViewportToolbar::viewModeRows`) |
| — | View ▸ Show ▸ (alle Overlay-Schalter, Show All / Hide All) — `ViewportToolbar::showRows` |
| — | View ▸ Camera ▸ (Perspective/Top/…, Orthographic, Bookmarks) — `ViewportToolbar::viewPopup` |
| — | Window ▸ Landscape Tools (Toolbar-Modus Landscape + Panel nach vorn; Haken bei aktivem Modus) |

### Assets

| alt | neu | Bemerkung |
|---|---|---|
| — | **Assets ▸ Create Asset…** | öffnet das 20-teilige Create-Popup des Content Browsers am angezeigten Ordner (`ContentBrowserPanel::requestCreateMenu`); nur bei sichtbarem Content Browser (Scene-Tab) |
| Import Asset… | = | |
| Refresh Assets | = | |
| Publish Engine Content… / Rebuild Manifest… | = (Dev) | |

### Build

| alt | neu |
|---|---|
| Export Project… | Build ▸ Export Project… (jetzt zweite Zeile) |
| Build and Reload Game Logic | Build ▸ Build and Reload Game Logic (erste Zeile) |

### Play (neu)

| Zeile | Shortcut | entspricht Toolbar |
|---|---|---|
| Play / Stop / Restart Preview (App) | **Ctrl+P** `play.toggle` | Play-Knopf |
| Pause / Resume / Continue (HC-Breakpoint) | **Ctrl+Shift+P** `play.pause` | Pause-Zelle |
| Step Frame | **Ctrl+Alt+P** `play.step` | Step |
| Step Node | — | Step Node |

Die Gates sind dieselben wie in `ViewportToolbar.cpp` (Pause aktiv bei
`playing || hcSuspended`, Step nur `playing`, Step Node nur `hcSuspended`).
Die Shortcuts sind global und feuern auch mit Asset-Tab davor; `canPlay()`
verlangt ein geladenes Projekt mit Welt (oder App-Live-Preview).

### Entity (neu, nur Spielprojekte)

| Zeile | Shortcut (Viewport-Scope) | Herkunft |
|---|---|---|
| Create ▸ Empty, Cube, Camera ▸ (Third Person / First Person / Plain), Light ▸ (Directional / Point / Spot), Rope, Trail | | Outliner-Hintergrundmenü (`OutlinerPanel::drawCreateEntityMenu`), legt am Wurzelknoten an |
| Focus Selected | F | Viewport-Kontextmenü |
| Snap to Ground | End | Viewport-Kontextmenü |
| Hide Selected / Isolate Selected / Show All | H / Shift+H / Alt+H | Viewport-Kontextmenü |
| Group / Ungroup | Ctrl+G / Shift+G | Viewport-Kontextmenü |
| Lock / Unlock | | beide Kontextmenüs |
| Save as Prefab | | Outliner-Kontextmenü (`OutlinerPanel::saveSelectionAsPrefab`) |

Alle Zeilen greifen auf den letzten Scene-Extract zu und sind ohne aktiven
Scene-Tab ausgegraut. Die Kontextmenüs selbst sind unverändert.

### Help

Unverändert.

### macOS-Besonderheiten

- App-Menü (About, Preferences, Hide, Quit) und View ▸ Toggle Full Screen
  (nativ, ⌃⌘F) bleiben.
- Window-Menü: Minimize/Zoom bleiben oben, darunter dieselben Zeilen wie im
  ImGui-Window-Menü.
- Recent Projects, Play-Titel (Play/Stop), Pause-Titel, Lock/Unlock und alle
  Häkchen (View Mode, Show, Camera, Panels, Landscape) werden pro Frame aus
  dem Editorzustand nachgeführt (`setItemTitle`, `setToggleState(cmd, arg, on)`,
  `setRecentProjects`).
- Zeilen mit Argument (View Mode, Show-Flag, Kamera-Preset, Recent, Create
  Entity) transportieren es in `representedObject`; `MacMenuBar::arg()` liest
  es nach `take()`.
- Play/Pause/Step haben auf dem Mac **keine** Key-Equivalents (⌘P & Co. gehen
  wie bisher durch SDL an den Editor und sind in Preferences umbelegbar).

---

## 3. Was aus der Schwachstellenliste (Audit §8) damit erledigt ist

| # | Befund | Stand |
|---|---|---|
| 1 | Play/Pause/Step nur über Toolbar | **erledigt**: Play-Menü + 3 Shortcuts + Mac |
| 2 | Entity anlegen nur per Rechtsklick | **erledigt**: Entity ▸ Create ▸ |
| 3 | Asset anlegen nur per Rechtsklick | **erledigt** (Menü-Tür): Assets ▸ Create Asset…; Kopfzeilen-Knöpfe Add/Import/Refresh seit Schritt 3 |
| 5 | View-Menü mischt vier Arten | **erledigt**: View = Zeichnen, Window = Öffnen |
| 6 | Virtuelle Tabs verstreut | **teilweise**: Level Script/Game Instance unter Window; Preferences/Project Settings bleiben bewusst unter Edit |
| 7 | Mac-Edit ohne Cut/Copy/Paste/Duplicate/Delete | **erledigt** |
| 8 | Ctrl+N/Ctrl+W nur Beschriftung | **erledigt**: Registry-Einträge, gebunden |
| 9 | Landscape-Erzeugung versteckt | **erledigt** (Tür): Window ▸ Landscape Tools |
| 11 | Assets/Build fast leer | Assets +1, Build unverändert (2 Zeilen sind ehrlich) |
| 12 | Recent Projects nur im Hub | **erledigt**: File ▸ Recent Projects ▸ (ImGui + Mac) |
| 13 | Zwei Kontextmenüs mit verschiedenen Verben | **erledigt** (Vereinigung im Entity-Menü); die Kontextmenüs selbst unverändert |
| 14 | Kein Window-Menü auf Windows/Linux | **erledigt** |
| 4 | Add Component flach, ohne Suche | **erledigt in Schritt 3** (`docs/editor-panel-cleanup-2026-09-21.md`): sieben Gruppen + Suchfeld |
| 10 | Unklare Panel-Beschriftungen | bewusst offen (Fenstertitel sind Identität, siehe Schritt-3-Doku §2) |
| 15 | Tab-Tastatur/Palette | offen (neues Verb) |

---

## 4. Bewusst offen gelassen

- **Rename** im Entity-Menü: der Umbenennen-Dialog hängt an render()-lokalen
  Statics des Outliners (`s_renameEntity`, `s_openEntityRename`); nur über
  den Outliner-Rechtsklick.
- **Ctrl+W** bleibt Close Project. Ein „Close Tab"-Befehl (+ Tab-Wechsel per
  Tastatur, Audit #15) wäre ein neues Verb.
- **Website-Docs** (HorizonEngineDocs, außerhalb dieses Repos) nennen die
  alten Pfade („View ▸ Console"); das In-Engine-Handbuch (`EditorHelp.cpp`,
  `TutorialSteps.cpp`) ist nachgezogen.
- Panel-Fenstertitel („Quick Settings", „Watch", „Environment") sind unangetastet
  (Config-Keys, `imgui.ini`, `docsPanelOpener`); Audit #10 ist Schritt 3.

---

## 5. Technische Spuren

- `EditorShortcuts.cpp`: +5 Einträge (`file.newProject`, `file.closeProject`,
  `play.toggle`, `play.pause`, `play.step`); `view.console` unter Kategorie
  „Window".
- `ViewportPanel.h`: `entityActionState`, `focusSelected(ctx)`,
  `snapSelectionToGround(ctx)`, `hideSelected`/`isolateSelected`/`showAll`/
  `groupSelected`/`ungroupSelected` (jetzt extern), `toggleLockSelected`,
  `viewMode`/`setViewMode`; `ShowFlagField::label`.
- `ViewportToolbar.h`: `showRows`, `viewModeRows`.
- `OutlinerPanel.h`: `drawCreateEntityMenu`, `entityPresetTable`,
  `createEntityPreset`, `saveSelectionAsPrefab` (Prefab-Save als Funktion
  `savePrefabOf` extrahiert, Logik unverändert).
- `ContentBrowserPanel.h`: `requestCreateMenu`.
- `EditorUI.cpp`: `GuardedAction::OpenProjectPath` (in `endsSession`),
  `openProjectAt` (aus dem Dialog-Callback herausgelöst, Fehlerpopup über
  Flag), gemeinsame Lambdas `playToggle/pauseToggle/stepFrame/stepNode/
  toggleLandscapeTools`, New-Project-Popup-Öffner hinter den Shortcut-Block.
- `EditorHelp.cpp`: Keys `View/<Panel>` → `Window/<Panel>`, neue Scopes
  `Entity/`, `Play/`, `Window/` in `kAreas`, `kPanelTopics` sagt „Window » …".
- `scripts/editor_help_audit.py`: `MENU_TITLES` um Entity/Play erweitert.
- `tests/test_editor_help.cpp`: Tabelle auf die neuen Scopes, ternäre Labels
  der Entity-/Play-Zeilen ergänzt.
