# Editor-Menüführung: Bestandsaufnahme (Ist-Zustand, 21.09.2026)

Thema 73 im Hive: „Editor der Horizon Engine: Menüführung und Bedienbarkeit
überholen", Schritt 1. Dieses Dokument ändert nichts, es hält fest, **was es gibt,
wo es steht und wo die Wege zu lang sind**. Es ist zugleich die Paritäts-Checkliste
für die späteren Schritte: „jede bisherige Funktion bleibt erreichbar" lässt sich
nur gegen ein vollständiges Inventar prüfen, deshalb sind die Tabellen zeilengenau.

Alle Pfade relativ zu `src/HE_Editor/`. Zeilennummern Stand Commit `54bf9032`
(Zweig `claude/editor-menuefuehrung-ueberholen`, identisch mit `main`).

---

## 1. Wo die Oberfläche lebt

| Teil | Datei | Bemerkung |
|---|---|---|
| Hauptmenüleiste (ImGui, Windows/Linux) | `EditorUI.cpp:1480-1697` | `BeginMainMenuBar`, sechs Menüs |
| Hauptmenüleiste (nativ, macOS) | `MacMenuBar.mm:101-305`, Enum `MacMenuBar.h:20-` | eigener `Cmd`-Katalog, Dispatch `EditorUI.cpp:1405-1476` |
| Dock-Layout (Standard) | `EditorUI.cpp:384-411` `BuildDefaultDockLayout` | Quick Settings links 18 %, Outliner+Details rechts 26 % (50/50), Content Browser+Console unten 33 %, Scene Mitte |
| Fußleiste | `EditorUI.cpp:2389-2601` `##EditorFooter` | Undo/Redo, Source Control, „Ready", rechts der Status-Cluster |
| Tab-Streifen (Asset-Editoren) | `EditorUI.cpp:2603-2785` `##EditorTabBar` | Scene-Tab + ein Tab je geöffnetem Asset; bei aktivem Asset-Tab verschwindet das ganze Szenen-Layout (`sceneTabActive`, `EditorUI.cpp:2795`) |
| Tab-Dispatch | `EditorUI.cpp:3003-3074` | 4 virtuelle Tabs + 16 Asset-Editor-Panels für 17 Asset-Typen (Material und Material Function teilen sich eines, siehe §7) |
| Viewport-Toolbar | `ViewportToolbar.cpp` | im Scene-Fenster, mit vier Popups |
| Kontextmenüs | `OutlinerPanel.cpp:717-905, 1065`, `ViewportPanel.cpp:640-710`, `ContentBrowserPanel.cpp:2605-3080, 4312-4360`, `InspectorPanel.cpp:3278-3479` | siehe §5 |
| Shortcut-Registry | `EditorShortcuts.cpp` | 25 Einträge, alle in Preferences ▸ Shortcuts umbelegbar |
| Hilfe-Registry | `EditorHelp.cpp`, `Help::Scope` | Schlüssel `"<Bereich>/<Label>"`, 92 benannte Scopes (Anhang A) |
| Startbildschirm | `ProjectHubPanel.cpp` | Recent Projects nur hier (`:426`) |

Toolkit: Dear ImGui (Docking-Zweig) + ImGuizmo, eigene Widget-Schicht `EditorWidgets`
(`menuItem`, `button`, `checkbox` … tragen die Hilfe-Anbindung mit).

---

## 2. Hauptmenüleiste: ImGui und macOS nebeneinander

Spalte „Mac" = `MacMenuBar.mm`-Zeile; „—" = auf macOS nicht vorhanden.
„Gate" = wann der Eintrag sichtbar/aktiv ist (`P` = Projekt geladen, `!App` = kein
Anwendungsprojekt, `Dev` = EngineContent-Dev-Modus + libssh2).

### 2.1 File (`EditorUI.cpp:1482-1518`)

| Eintrag | Shortcut-Label | wirklich gebunden? | Gate | Mac |
|---|---|---|---|---|
| New Project | „Ctrl+N" (hartcodiert) | **nein** (ImGui); ⌘N nativ | | `:148` |
| Open Project | `file.openProject` Ctrl+O | ja | | `:149` |
| Close Project | „Ctrl+W" (hartcodiert) | **nein** (ImGui); ⌘W nativ | | `:150` |
| New Scene | | | !App | `:153` |
| Open Scene… | | | !App | `:155` |
| Add Scene Additive… | | | !App | `:157` |
| Save | `file.save` Ctrl+S | ja | | `:161` |
| Save All | `file.saveAll` Ctrl+Shift+S | ja | | `:162` |
| Save Scene As… | `file.saveSceneAs` Ctrl+Alt+S | ja | !App | `:165` |
| Exit | „Alt+F4" (hartcodiert) | OS | | App-Menü „Quit" `:138` |

Fehlt: Recent Projects (nur im Project Hub), Import (steht unter Assets).

### 2.2 Edit (`EditorUI.cpp:1519-1575`)

| Eintrag | Shortcut | Gate | Mac |
|---|---|---|---|
| Undo / Redo (im Collab-Modus mit Beschriftung der Aktion) | `edit.undo` / `edit.redo` / `edit.redoAlt` | canUndo/canRedo | `:184-185` (ohne Key-Equivalent, bewusst) |
| Cut / Copy / Paste / Duplicate / Delete (**Entities**, nicht Text) | `entity.*` | Auswahl | **—** |
| Project Settings | | P | `:190` |
| Preferences | `edit.preferences` Ctrl+, | | App-Menü `:115` |

Mac-Lücke: die fünf Entity-Befehle fehlen im nativen Edit-Menü komplett
(Kommentar `MacMenuBar.mm:168-180` erklärt nur, warum keine Key-Equivalents; die
Zeilen selbst fehlen). Das Gap-Audit vom 25.08. (§ „Auf macOS gibt es gar kein
Edit-Menü") ist damit nur halb abgearbeitet.

### 2.3 View (`EditorUI.cpp:1576-1627`)

| Eintrag | Art | Shortcut | Gate | Mac |
|---|---|---|---|---|
| Toggle Fullscreen | Aktion | `view.fullscreen` F11 | | nativ `:197` |
| Reset Layout | Aktion | | | `:200` |
| Performance Profiler | Panel-Toggle (floating) | | | `:204` |
| Environment | Panel-Toggle (floating) | | | `:206` |
| Collaboration | Panel-Toggle | | | `:208` |
| Source Control | Panel-Toggle | | | `:210` |
| Console | Panel-Toggle | `view.console` Ctrl+` | | `:212` |
| Audio Mixer | Panel-Toggle | | | `:214` |
| Undo History | Panel-Toggle | | | `:216` |
| Watch | Panel-Toggle | | | `:218` |
| Ground Grid | Viewport-Flag (auch in Toolbar ▸ Show) | | P, !App | `:223` |
| Scene 2 / 3 / 4 | Panel-Toggle | | P, !App | `:231-233` |
| Level Script | Tab-Öffner | | P, !App | `:244` |
| Game Instance | Tab-Öffner | | P | `:247` |

Vier Arten von Einträgen in einem Menü: Aktionen, Fenster-Toggles, Tab-Öffner,
ein Render-Flag. Keine Trennung, keine Untermenüs.

### 2.4 Assets (`EditorUI.cpp:1628-1661`)

| Eintrag | Gate | Mac |
|---|---|---|
| Import Asset… | P | `:253` |
| Refresh Assets | P | `:254` |
| Publish Engine Content to Server… | Dev | `:262` |
| Rebuild Manifest from Server… | Dev | `:264` |

Für normale Nutzer zwei Einträge. **Kein** „Create Asset" hier, obwohl der Content
Browser ein 20-teiliges Erzeugungsmenü hat (§5.3).

### 2.5 Build (`EditorUI.cpp:1662-1675`, ganzes Menü nur bei P)

| Eintrag | Gate | Mac |
|---|---|---|
| Export Project… | | `:269` |
| Build and Reload Game Logic | C++-Projekt (sonst grau) | `:273` |

**Play / Pause / Step stehen nirgends im Menü** (siehe §8.1).

### 2.6 Help (`EditorUI.cpp:1676-1697`)

| Eintrag | Shortcut | Mac |
|---|---|---|
| Documentation | F1 | `:297` |
| Search the Documentation… | Ctrl+F1 (gebunden, `EditorUI.cpp:706`) | `:298` |
| Documentation (Website) | | `:299` |
| Interactive Tutorial | | `:302` |
| Report Issue… | | `:304` |
| About | | App-Menü `:108` |

### 2.7 Nur macOS

App-Menü (About, Preferences, Hide, Quit), **Window**-Menü (Minimize, Zoom;
`MacMenuBar.mm:278-284`). Auf Windows/Linux gibt es kein Window-Menü.

---

## 3. Viewport-Toolbar (`ViewportToolbar.cpp`)

Eine Zeile oben im Scene-Fenster, drei Gruppen (links/Mitte/rechts), schrumpft in
drei Stufen (Labels → Kamera → Snap, `:686-690`).

| Zelle | Position | Verhalten | Hilfe-Key |
|---|---|---|---|
| View / Landscape (Modus) | links | schaltet `EditorMode`; Landscape benennt das Quick-Settings-Panel in „Landscape" um (`EditorUI.cpp:3161`) | `viewport.mode` |
| Move / Rotate / Scale | links | W / E / R (`viewport.*`) | `viewport.translate/rotate/scale` |
| World / Local | links | Gizmo-Raum | `viewport.space` |
| Snap-Schalter + Wert (Popup mit Presets, `:340-382`) | links | | `viewport.snap` |
| Play/Stop, Pause, Step, Step Node, Uhr (Zeitskala) | Mitte | **keine Taste, kein Menüeintrag**, nur hier (`:823-852`) | `viewport.play/pause/step/step-node/time-scale` |
| Kamera-Geschwindigkeit | rechts | | `viewport.camera-speed` |
| View-Preset (Popup) | rechts | Perspective/Top/Bottom/Front/Back/Right/Left (Num 5/7/Ctrl+7/1/Ctrl+1/3/Ctrl+3), Orthographic, Bookmarks 1-9 (Ctrl+Ziffer setzen) | `Viewport View/*` |
| View-Mode (Popup) | rechts | Lit Alt+4, Unlit Alt+3, Wireframe Alt+2, G-Buffer: Base Color/Normal/Rough-Spec-Metal/Emissive (nur Deferred) | `Viewport View Mode/*` |
| Show (Popup, `:441-475`) | rechts | Ground Grid, Editor Icons, Selection · Colliders, Joints, NavMesh · Guides, Script Debug, Collaborators · Stats · Show All / Hide All | `Viewport Show/*` |
| Options (Popup, `:385-437`) | rechts | Snapping (Modus, Move/Rotate/Scale-Werte, Rest on surface, Vertex radius), Gizmo (Screen-space ring), Camera Speed, Render-target-Anzeige | `Viewport Options/*` |

Die Toolbar ist der jüngste und stimmigste Teil der Oberfläche; die Probleme liegen
nicht in ihr, sondern darin, dass Dinge **nur** in ihr erreichbar sind.

---

## 4. Fußleiste und Tab-Streifen

Fußleiste (`EditorUI.cpp:2389-2601`), von links: Undo/Redo-Knöpfe (zweite Tür auf
denselben Stack wie Edit), Source-Control-Status (Klick öffnet Panel), Mitte „Ready"
(statischer Text, `:2594`), rechts handverketteter Cluster: Remote-Control (MCP),
Collab-Aktivität, EngineContent-Download, Präsenz, Glocke (Notifications),
Auflösung + FPS. Die Kette ist laut Kommentar `:2470-2475` handgepflegt: jedes neue
Widget erfordert Änderung aller Blöcke links davon.

Tab-Streifen (`EditorUI.cpp:2603-2785`): Tab 0 „Scene" (nicht schließbar), danach
je ein Tab pro Asset/virtuellem Tab; Reorderable, Scroll-Fitting, kein
Mittelklick-Schließen, **kein Close-Tab-Befehl** (nur das × am Tab), kein
Tab-Wechsel per Tastatur.

---

## 5. Kontextmenüs

### 5.1 World Outliner

Hintergrund-Rechtsklick (`OutlinerPanel.cpp:1065`) und „Create Child" teilen sich
`drawCreateMenu` (`:126-155`): Empty, Cube · Camera ▸ Third Person / First Person /
Plain · Light ▸ Directional / Point / Spot · Rope, Trail.
**Das ist die einzige Stelle, an der man ein Entity anlegt.** Kein Hauptmenü, kein
Knopf im Outliner-Kopf, nicht im Viewport-Kontextmenü.

Entity-Rechtsklick (`:717-905`): Create Child ▸, Rename · Move Up, Move Down, Sort
Children by Name, Lock/Unlock · Duplicate, Copy, Cut, Paste · Save as Prefab ·
Delete. Kein Focus/Hide/Isolate (die stehen nur im Viewport-Kontextmenü).

Outliner-Kopf: keine Suche, kein Filter, kein „+"-Knopf (`:305-`; im App-Modus
zeigt er stattdessen den Widget-Baum).

### 5.2 Viewport (`ViewportPanel.cpp:640-710`, Rechtsklick ohne Drag oder Menü-Taste/Shift+F10)

Focus Selected F, Snap to Ground End · Hide Selected H, Isolate Shift+H, Show All
Alt+H · Group Ctrl+G, Ungroup Shift+G · Lock/Unlock · Duplicate, Copy, Cut, Paste ·
Delete. Kein „Create", kein Rename.

### 5.3 Content Browser

Kopfzeile (`ContentBrowserPanel.cpp:855-990`): Breadcrumb (Root-Wechsel Project /
Engine / Source), Suchfeld, Filter-Löschen. **Kein Import-Knopf, kein Add/Create-Knopf.**

Hintergrund-Rechtsklick (`:4312-4360`, `drawCreateAssetItems` `:2199-2366`):
- Spielprojekt: Scene, UI Widget · Gameplay ▸ (HorizonCode Class | Script | C++
  Class je nach Projektsprache, Entity, Player Controller, Player Character ·
  Animator State Machine, Bone Mask, Blend Space, Property Animation Clip) ·
  Input ▸ (Input Action, Input Mapping Context) · Rendering ▸ (Material, Material
  Function [Gate Advanced Shader Effects], Particle System) · Data ▸ (Struct, Enum,
  SaveGame Template, Theme) · Folder · Hinweistext „Meshes, textures, audio and
  fonts arrive via Assets ▸ Import Asset."
- App-Projekt: flach UI Widget, Theme, Klasse/Script, Material(-Function), Folder.
- Engine-Root: nur Hinweis „read-only"; Source-Root: nur C++ Class.

Item-Rechtsklick (`:2605-3080`): Revert to Default, Remove Local Copy (Engine-Cache)
· Import / Reimport (Quelldateien) · Create Material Instance · Add to Scene · Ask
to Edit (Collab-Lock) · Find References · Rename · Delete · Create Asset ▸ (auf
Ordnern).

### 5.4 Details ▸ Add Component (`InspectorPanel.cpp:3278-3479`)

Zentrierter Knopf unter der Komponentenliste, Popup mit Paste Component + **flacher
Liste von 28 Einträgen ohne Gruppierung und ohne Suchfeld**: Transform, Mesh,
Skeletal Mesh, Nav Mesh, Nav Agent, Material, Movement, Network, Camera, Camera
Rig, Light, Decal, Rope, Trail, Rigid Body, Collider, Joint, Save State, Script,
Audio Source, Audio Listener, Particle System, LOD, Foliage, Animator State
Machine, Root Motion, Animation Layers, Inverse Kinematics (die letzten vier nur
mit Skelett-Mesh).

---

## 6. Panels und virtuelle Tabs

Immer gedockt (`EditorUI.cpp:249`): Scene, World Outliner, Details, Content Browser,
Quick Settings. Per View-Menü (Status wird in der Config gemerkt, `s_panelPrefs`
`:341-380`): Performance Profiler, Environment, Collaboration, Source Control,
Console, Audio Mixer, Undo History, Watch, Scene 2/3/4. Von selbst öffnend:
Watch bei HC-Breakpoint, Console beim ersten Play-Fehler, Play Session Report nach
Stop, Tutorial, Documentation (F1).

Virtuelle Tabs (`*::kTabPath`): Preferences (Edit), Project Settings (Edit), Level
Script (View), Game Instance (View). Zwei Einstellungsdialoge unter Edit, zwei
Skript-Graphen unter View.

Preferences (`EditorSettingsPanel.cpp:1863-1896`): General ▸ Appearance, Viewport,
Content Browser, Autosave · Editor ▸ HorizonCode, Shortcuts, Collaboration, Remote
Control, Source Control, Tool Status · Rendering ▸ Display, Post-Processing, Global
Illumination, Effects. Project Settings (`ProjectSettingsPanel.cpp:1133-1150`):
Game ▸ General, Application, Permissions, Fonts, Anti-Cheat · Rendering ▸ Defaults,
Shadows · Physics ▸ Simulation, Collision Layers · Audio ▸ Buses. Beide sauber
gegliedert, keine Befunde.

„Quick Settings" = in Preferences angepinnte Engine-Einstellungen
(`EditorSettingsPanel::DrawEngineSettings(QuickSettings)`, `EditorUI.cpp:3171`);
im Landscape-Modus wird dasselbe Fenster zum Landscape-Werkzeugpanel mit „Create
Landscape" (`TerrainTools.cpp:535-562`).

---

## 7. Asset-Editoren als Tabs (`EditorUI.cpp:3003-3074`)

Material, Material Function, UI Widget, HorizonCode Class, Input Action/Mapping,
Theme, Bone Mask, Blend Space, Sequencer (Property Animation), Struct/Enum/SaveGame
(Type), Skeletal Mesh, Static Mesh, Particle System, Animator State Machine,
Audio, C++ Class, Script (Lua/Python). Öffnen nur per Doppelklick im Content
Browser oder Querverweis (Konsole „go to node", Material-Function-Node). Bei aktivem
Asset-Tab sind Outliner, Details, Content Browser und Viewport weg; nur die
floating Fenster aus §6 bleiben (`renderOverlays`, `EditorUI.cpp:3190-3309`).

---

## 8. Schwachstellen (priorisiert, nur Befund, keine Lösung)

Reihenfolge nach Häufigkeit der betroffenen Handlung.

1. **Play/Pause/Step nur über den Toolbar-Knopf.** Kein Shortcut in
   `EditorShortcuts.cpp` (0 Treffer für `viewport.play`), kein Menüeintrag, kein
   Mac-`Cmd`. `ctx.setPlayMode` wird nur aus `ViewportToolbar.cpp:852` ausgelöst.
   Die häufigste Handlung im Editor hat genau eine Tür.
2. **Entity anlegen nur per Rechtsklick auf Outliner-Leerfläche** (§5.1). Nicht
   im Hauptmenü, nicht im Viewport-Kontextmenü, kein Knopf. Wer die Leerfläche
   nicht trifft (voller Outliner), bekommt das Entity-Menü statt des Create-Menüs.
3. **Asset anlegen nur per Rechtsklick im Content Browser**, Import nur über
   Assets ▸ Import Asset oder Rechtsklick auf eine Quelldatei; die
   Content-Browser-Kopfzeile hat keinen Import- und keinen Add-Knopf (§5.3).
4. **Add Component: 28 Einträge flach, ungruppiert, ohne Suchfeld** (§5.4).
5. **View-Menü mischt vier Arten von Einträgen** (Aktion, Fenster-Toggle,
   Tab-Öffner, Render-Flag) in einer 16-zeiligen Liste ohne Untermenüs (§2.3).
6. **Die vier virtuellen Tabs sind auf Edit und View verstreut**; Level Script
   und Game Instance (Gameplay-Graphen) stehen unter View neben „Audio Mixer".
7. **Mac-Edit-Menü ohne Cut/Copy/Paste/Duplicate/Delete** (§2.2); Windows/Linux
   haben sie. Parität wird laut `MacMenuBar.mm`-Kommentaren von Hand gehalten.
8. **Hartcodierte Shortcut-Labels ohne Bindung**: „Ctrl+N" (New Project) und
   „Ctrl+W" (Close Project) stehen im ImGui-Menü, sind aber auf Windows/Linux
   nicht gebunden (kein `ImGuiKey_N/W` außerhalb der Viewport-Navigation, keine
   `SDL_SCANCODE`-Behandlung). Auf macOS wirken sie nativ. Dazu: Ctrl+W wäre in
   jedem anderen Editor „Tab schließen"; einen Close-Tab-Befehl gibt es nicht.
9. **Landscape-Erzeugung versteckt**: Toolbar-Modus „Landscape" → das
   Quick-Settings-Panel wird stillschweigend zum Landscape-Panel → dort „Create
   Landscape". Kein Menüeintrag, kein Hinweis, wo das Panel ist, wenn es zugeklappt
   oder floating ist.
10. **Unklare Beschriftungen**: „Watch" (HC-Breakpoint-Variablen), „Quick
    Settings" (angepinnte Engine-Settings), „Environment" (nur Sky/Weather
    anlegen/entfernen, die Werte selbst stehen in Details), „Scene 2/3/4"
    (zusätzliche Viewports), „Game Instance"/„Level Script" (Graphen).
11. **Assets- und Build-Menü fast leer** (2 bzw. 2 Einträge), während das
    Create-Menü mit 20 Einträgen im Rechtsklick steckt.
12. **Recent Projects nur im Project Hub**, nicht unter File.
13. **Focus/Hide/Isolate/Group** nur im Viewport-Kontextmenü, Rename/Move
    Up/Down/Sort/Save as Prefab nur im Outliner-Kontextmenü: zwei
    Kontextmenüs für dasselbe Objekt mit unterschiedlichen Verben.
14. **Kein Window-Menü auf Windows/Linux**, dafür Panel-Toggles unter View; auf
    macOS existiert ein natives Window-Menü nur mit Minimize/Zoom.
15. **Kein Tastatur-Weg für Tabs** (nächster/vorheriger/schließen) und keine
    Befehlspalette; F1 öffnet Docs, Ctrl+F1 die Doc-Suche, mehr nicht.

Beobachtungen außerhalb des Themas (nicht Teil der Fix-Liste, nur notiert):
- Asset-Tab-Modus blendet das komplette Szenen-Layout aus (§7); ein Umbau wäre
  Architektur, nicht Menüführung.
- Fußleisten-Cluster ist handverkettet (§4); technisch, nicht bedienerisch.
- Gap-Audit 25.08. §3.3 (Lichter/Kameras im Viewport unsichtbar) bleibt offen.

---

## 9. Randbedingungen für den Umbau

Was die nächsten Schritte einhalten müssen, damit Build, Tests und gespeicherte
Zustände heil bleiben:

- **Hilfe-Schlüssel hängen am Menüpfad.** `Help::Scope("File")` +
  `menuItem("Save")` → Schlüssel `"File/Save"` in `EditorHelp.cpp`. Zwei
  Wächter in ctest: `editor_help_audit` (`scripts/editor_help_audit.py --check`,
  `tests/CMakeLists.txt:919`) schlägt bei jedem neuen ungedeckten Bedienelement
  fehl, und `test_editor_help.cpp:148-` schlägt literale Scope/Label-Paare nach
  („File"/„Save All", „World Outliner"/„Save as Prefab", „New Entity"/„Cube" …).
  Jeder verschobene oder umbenannte Eintrag braucht also einen mitgezogenen
  Hilfe-Eintrag **und** ggf. eine angepasste Testtabelle. Tooltip-Texte nennen
  Pfade wörtlich („Assets ▸ Import Asset", `ContentBrowserPanel.cpp:2365`;
  „View ▸ Reset Layout", `EditorUI.cpp:3119`).
- **Zwei Menü-Implementierungen.** ImGui-Leiste und `MacMenuBar.mm` (`Cmd`-Enum +
  Dispatch `EditorUI.cpp:1405-1476`) müssen von Hand synchron gehalten werden;
  Kommentar bei Save (`EditorUI.cpp:1509`) sagt es ausdrücklich.
- **Fenstertitel sind Identität.** `DockBuilderDockWindow("Quick Settings")`,
  `imgui.ini`, `s_panelPrefs`-Config-Keys (`EditorUI.cpp:341-380`),
  `docsPanelOpener`-Liste (`:221-256`), Tutorial-Spotlight (`PanelSpotlight.h`) und
  Docs-„Show me" adressieren Panels über den Titel. Umbenennen = gespeicherte
  Layouts und Prefs brechen; Muster dafür ist das `###`-Suffix beim
  Landscape/Quick-Settings-Panel (`EditorUI.cpp:3161`).
- **Tutorial und Docs nennen Pfade.** `TutorialSteps.cpp` (z. B. `:432-447`
  „Create Landscape") und die Website-Docs beschreiben Klickwege wörtlich.
- **Shortcut-Labels kommen aus `EditorShortcuts::label()`**; die drei
  hartcodierten („Ctrl+N", „Ctrl+W", „Alt+F4") laufen daneben her. Neue
  Shortcuts gehören in die Registry (dann sind sie auch in Preferences ▸ Shortcuts
  umbelegbar).
- **App-Projekt-Gates.** Viele Einträge sind bei `appProject` versteckt statt
  ausgegraut (`docs/he-apps-plan.md` E2); eine neue Gliederung muss die Gates
  mitnehmen, sonst tauchen Szenen-Befehle in App-Projekten wieder auf.
- **Kein Umbau von Rendering, Szenenformat, Datenmodell, MCP** (Thema-Grenze).

---

## Anhang A: Hilfe-Scopes als Karte der UI-Regionen (92 benannte)

`grep -ho 'helpScope("[^"]*")' src/HE_Editor/*.cpp src/HE_Editor/Guides/*.cpp | sort -u`,
ohne den einen leeren Scope. Die Details-Komponenten laufen daneben per
`helpForKey("details.*")`.

Anti-Cheat, Application, Assets, Audio Buses, Audio Editor, Audio Mixer, Blend
Space Editor, Block Participant, Bone Mask Editor, Build, Build Tools, Build
Window, Canvas, Class Components, Collaboration Session, Collision Layers,
Console, Content Browser, Documentation, Edit, Environment Window, Export, File,
Fonts, Function Return, Graph Appearance, Help, HorizonCode Default Value,
HorizonCode Event, HorizonCode Graph, HorizonCode Node, Input Action, Landscape,
Material Graph, Material Node, Material Parameter, Material Preview, Material
Settings, Mesh Viewer, New Asset, New Entity, New Landscape, Node Parameter,
Notifications, Permissions, Physics, Play Report, Preferences, Profiler, Project
General, Project Hub, Rename Across Project, Render Defaults, Report Issue,
Scene Recovery, Script Graph, Script Node, Script Variable, Secondary Viewport,
Sequencer, Session Participants, Shadows, Shortcuts, Source Control, Source
Control Panel, Source Root, State Machine, State Machine Parameters, State
Machine Transitions, Sync Graph, Theme Editor, Theme Styles, Tool Status,
Tutorial, Type Editor, UI Align, UI Graph, UI Graph Node, UI Hierarchy, UI Theme
Preview, UI Timeline, UI Variable, UI Widget, Undo History, View, Viewport Menu,
Viewport Options, Viewport Show, Viewport View, Viewport View Mode, Watch, World
Outliner

## Anhang B: Shortcut-Registry (`EditorShortcuts.cpp`, 25)

File: save, saveAll, saveSceneAs, openProject · Edit: undo, redo, redoAlt,
preferences · Entities: duplicate, copy, cut, paste, delete · View: console,
fullscreen · Viewport: move, rotate, scale, focus, snapToGround, hide, isolate,
showAll, group, ungroup. Nicht in der Registry, aber fest verdrahtet: F1 Docs /
Ctrl+F1 Doc-Suche (`EditorUI.cpp:701-706`), Num-Block View-Presets, Alt+2/3/4 View-Modes, Ctrl+1..9
Kamera-Bookmarks, Menü-Taste/Shift+F10 Viewport-Kontextmenü.
