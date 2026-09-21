# Editor-Menüführung: Verifikation (Thema 73, Schritt 4/5, 21.09.2026)

Abschluss zu Schritt 1 (`docs/editor-menu-audit-2026-09-21.md`, Inventar),
Schritt 2 (`docs/editor-menu-restructure-2026-09-21.md`, Menüleiste) und
Schritt 3 (`docs/editor-panel-cleanup-2026-09-21.md`, Panels). Dieses Dokument
sagt, **was geprüft wurde, womit, und was dabei herauskam**, und es enthält die
kurze Vorher/Nachher-Übersicht, die das Thema als Abnahmekriterium nennt.

Stand: Zweig `claude/editor-menuefuehrung-ueberholen`, Commit `ec5d1112` plus
dieser Schritt. Maschine: macOS 27 (arm64), Debug-Build im Worktree
(`build/`, Unix Makefiles).

---

## 1. Vorher / Nachher in einer Tabelle

### Menüleiste (ImGui auf Windows/Linux, nativ auf macOS)

| vorher (6 Menüs, 7 auf dem Mac) | nachher (9 Menüs, beide Plattformen gleich) |
|---|---|
| **File**: New/Open/Close Project, New/Open/Add Scene, Save/Save All/Save As, Exit. Ctrl+N/Ctrl+W nur beschriftet. Recent Projects nur im Hub. | **File**: dasselbe + **Recent Projects ▸**, **Import Asset…**; Ctrl+N/Ctrl+W wirklich gebunden (umbelegbar) |
| **Edit**: Undo/Redo, Cut/Copy/Paste/Duplicate/Delete (Mac: fehlten), Project Settings, Preferences | **Edit**: unverändert; Mac hat die fünf Entity-Befehle jetzt auch |
| — (Entity nur per Rechtsklick in Outliner/Viewport) | **Entity** *(neu)*: Create ▸ (Empty, Cube, Camera ▸ 3, Light ▸ 3, Rope, Trail), Focus, Snap to Ground, Hide/Isolate/Show All, Group/Ungroup, Lock/Unlock, Save as Prefab |
| **Assets**: Import, Refresh, (Dev: Publish/Rebuild) | **Assets**: **Create Asset…** + dasselbe |
| — (Play/Pause/Step nur in der Viewport-Toolbar, ohne Taste) | **Play** *(neu)*: Play/Stop (Ctrl+P), Pause/Resume/Continue (Ctrl+Shift+P), Step Frame (Ctrl+Alt+P), Step Node |
| **Build**: Export Project…, Build and Reload Game Logic | **Build**: gleiche zwei Zeilen, Build zuerst |
| **View** (16 Zeilen, vier Arten): Fullscreen, Reset Layout, 8 Panel-Toggles, Ground Grid, Scene 2/3/4, Level Script, Game Instance | **View** (nur Darstellung): View Mode ▸ (7), Show ▸ (10 Overlays + Show/Hide All), Camera ▸ (7 Presets, Orthographic), Toggle Fullscreen |
| Window nur auf dem Mac (Minimize, Zoom) | **Window** (beide): Console (Ctrl+`), Profiler, Environment, Collaboration, Source Control, Audio Mixer, Undo History, Watch, Scene 2/3/4, Level Script, Game Instance, **Landscape Tools**, Reset Layout (+ Minimize/Zoom auf dem Mac) |
| **Help**: 6 Zeilen | **Help**: unverändert |

### Panels

| Stelle | vorher | nachher |
|---|---|---|
| Details ▸ Add Component | 28 Zeilen flach, ohne Suche; Skelett-Komponenten fehlten stumm | Suchfeld + 7 Gruppen (Transform/Rendering/Physics/Animation/Gameplay/Navigation/Audio); Tippen → flache Trefferliste, Enter fügt den ersten Treffer hinzu; Animation ausgegraut mit Tooltip ohne Skeletal Mesh |
| Content Browser Kopfzeile | nur Breadcrumb; Anlegen/Import nur per Rechtsklick/Menü | Breadcrumb + **Add (+) · Import (↓) · Refresh (⟳)** rechts; Breadcrumb kürzt sich von links |
| World Outliner Kopfzeile | Suche · Filter · × ; Anlegen nur per Rechtsklick auf Leerfläche | **+** · Suche · Filter · × (dasselbe Create-Menü wie Entity ▸ Create) |
| Fensterttitel, Fußleiste, Preferences, Project Settings | — | bewusst unverändert (Schritt-3-Doku §2) |

Klickwege, die kürzer wurden: Play/Pause/Step (Toolbar-only → Menü + Taste),
Entity anlegen (Rechtsklick → Menü, Outliner-„+"), Asset anlegen/importieren
(Rechtsklick → Menü + zwei Knöpfe), Komponente hinzufügen (Scrollen in 28 →
Tippen + Enter), zuletzt geöffnetes Projekt (Projekt schließen → Hub → Liste
→ File ▸ Recent Projects), Landscape-Werkzeuge (Toolbar-Modus → Window-Menü).
Länger geworden ist nichts: jede alte Zeile hat genau einen neuen Ort
(Tabelle in §2, Skript in §3).

---

## 2. Was geprüft wurde

| # | Prüfung | Ergebnis |
|---|---|---|
| 1 | **Build im Vordergrund**: `make -j10 HorizonEditor he_tests` in `build/` | `MAKE_RC=0`; `HorizonEditor` und `he_tests` (21:44) jünger als der letzte Commit (21:42). Inkrementell: der isolierte Vollbuild lief davor durch Chefchen im selben Worktree. |
| 2 | **Editor gestartet, Fenster, kein Projekt** (`out/deploy/Editor/HorizonEditor`, `HOME` auf ein leeres Verzeichnis umgebogen, damit die Config des Menschen unberührt bleibt) | SDL/cocoa-Fenster 1600×900 auf dem Retina-Display, Metal, Project Hub. **429 s Laufzeit, 2683 Frames, 0 Fehler**, sauberer Shutdown per SIGTERM (Config + `imgui.ini` geschrieben). Sample zeigt `EditorUI::render → ProjectHubPanel::render`; damit ist `MacMenuBar::install()` mit dem neuen `Cmd`-Katalog in jedem Frame durchgelaufen. |
| 3 | **Editor gestartet, Fenster, mit Projekt** (Minimalprojekt `MenuVerify.heproj` + Root-Szene mit einem Cube, als `LastProjectPath` gesetzt) | Projekt und Szene geladen, **794 s, 12 596 Frames, 0 Fehler**, sauberer Shutdown. `imgui.ini` belegt, welche Fenster gezeichnet wurden: Scene, World Outliner, Details, Content Browser, Console, Quick Settings. Damit lief der neue Content-Browser-Kopf (Add/Import/Refresh) erstmals mit einem echten ContentManager, der Outliner-Kopf mit „+" und die Entity-/Play-Menüs mit geladener Welt, alles ohne Absturz. |
| 4 | **Paritätsabgleich alt → neu**, `scripts/editor_menu_parity.py` (neu, §3) | **47 alte Zeilen + 21 versprochene Zeilen: 0 Lücken**, sowohl in der ImGui-Leiste als auch im nativen Mac-Menü; Top-Level-Reihenfolge beider Leisten identisch; 0 unerklärte ImGui/Mac-Unterschiede. Negativkontrolle (zwei Zeilen künstlich entfernt) → rc=1, beide gemeldet. |
| 5 | **Shortcut-Registry** `EditorShortcuts.cpp` | `file.newProject` Ctrl+N, `file.closeProject` Ctrl+W, `play.toggle` Ctrl+P, `play.pause` Ctrl+Shift+P, `play.step` Ctrl+Alt+P, `view.console` unter „Window"; 30 Einträge, keine doppelte Standardbelegung. |
| 6 | **Handbuch-Deckung** `scripts/editor_help_audit.py --check` | 953/953 gedeckt, 1460 Hilfeeinträge. |
| 7 | **Testsuite** `ctest -j8 --timeout 600` in `build/` | **187/188 im Sammellauf**, 3 `runtime_size` geskippt (wie immer). `test_material_graph` lief in den von mir gesetzten 600-s-Timeout, während parallel zwei Debug-Editoren (Prüfung 2/3), `test_app_todo` (486 s) und ein fremder Vollbuild (Load 13) liefen. **Einzelwiederholung mit `--timeout 900`: bestanden in 720 s** (571 s reine CPU, der Test braucht also auch ohne Last rund 10 Minuten; 600 s waren zu knapp gewählt, kein Regress). Damit **188/188 grün**, wie bei Chefchens Lauf am 21.09. |

### Was NICHT geprüft wurde, und warum

- **Mit der Maus durchgeklickt: nein.** Aus dieser Shell gibt es keinen Weg
  in ein laufendes Fenster: `osascript`/System Events hängt am fehlenden
  Automation-Recht (ein Dialog „Terminal möchte System Events steuern" kann
  auf dem Bildschirm zurückgeblieben sein), die MCP-Bridge des Editors hat
  nur Daten-Werkzeuge (Entities, Assets, Materialien), keine UI-Steuerung,
  und der Headless-Dump (`HE_DUMP_PATH`) beendet den Prozess **vor** dem
  ersten UI-Frame, zeichnet also weder Menü noch Panels.
- **Die ImGui-Leiste ist auf dem Mac toter Code** (`MacMenuBar::available()`
  gewinnt). Selbst ein Mensch, der hier alles anklickt, sieht den
  Windows/Linux-Pfad nie. Der Paritätsabgleich (§3) ist die einzige Deckung,
  die dieser Pfad von diesem Rechner aus bekommt; auf echter Windows/Linux-HW
  steht der Klick noch aus.
- **Klickpfade der drei Panel-Umbauten**: Add-Component-Popup ist headless
  getestet (`test_inspector_ui`, Schritt 3); Outliner-„+" und die
  Content-Browser-Knöpfe wurden gezeichnet (Prüfung 3), aber nicht gedrückt.

---

## 3. Das Paritäts-Skript

`scripts/editor_menu_parity.py` liest beide neuen Leisten strukturell aus dem
Quelltext: `BeginMenu`/`EndMenu`-Verschachtelung und `menuItem`-Aufrufe im
`BeginMainMenuBar`-Block von `EditorUI.cpp` (klammerbewusst, damit Ternaries
wie `hcSuspended ? "Continue" : (playing && paused) ? "Resume" : "Pause"` als
eine Zeile mit drei Titeln gelten), `heAddSubmenu`/`heAddItem`/`heAddToggle`/
`initWithTitle:` in `MacMenuBar::install()`. Tabellengetriebene Untermenüs
(Entity-Presets `kPresetTable`, `viewModeName`, `showFlagFields`,
Kamera-Presets) werden aus denselben Tabellen expandiert, die der Editor
selbst liest; die drei per Frame umbenannten Mac-Zeilen kommen aus den
`setItemTitle`-Aufrufen im Dispatch.

Verglichen wird gegen das alte Inventar aus dem Audit (§2, wörtlich
übernommen) plus die Zeilen, die die Neugliederung versprochen hat. Erklärte
Plattformunterschiede (App-Menü, Quit/Exit, Toggle Full Screen, Minimize/Zoom,
Kamera-Bookmarks nur in der ImGui-/Toolbar-Variante) stehen in
`PLATFORM_ONLY` mit Begründung; alles andere ist eine Differenz und lässt das
Skript mit 1 enden.

Aufruf: `scripts/editor_menu_parity.py` (druckt beide Bäume und den Bericht),
`--quiet` nur den Bericht. Auszug des Berichts (21.09.2026):

```
== top-level order  ImGui: ['File', 'Edit', 'Entity', 'Assets', 'Play', 'Build', 'View', 'Window', 'Help']
                    Mac:   ['File', 'Edit', 'Entity', 'Assets', 'Play', 'Build', 'View', 'Window', 'Help']
== result: 0 gap(s) old->new, 0 unexplained ImGui/Mac difference(s), order identical
```

Ein Fund nebenbei, kein Fehler: das Mac-Untermenü View ▸ Camera hat keine
Bookmark-Zeilen (Bookmarks 1-9, Set/Clear), die ImGui-Variante schon, weil sie
das Toolbar-Popup wiederverwendet. Beide Plattformen erreichen die Bookmarks
weiterhin über das View-Popup der Toolbar; wer sie auch im Mac-Menü will,
braucht ein paar `heAddItem`-Zeilen mit `arg`.

---

## 4. Checkliste für den Menschen am Mac (5 Minuten)

Was hier nicht geklickt werden konnte, in der Reihenfolge des höchsten
Nutzens:

1. Projekt öffnen → **Play ▸ Play** (Ctrl+P), Menü-Titel wechselt auf „Stop";
   **Play ▸ Pause** (Ctrl+Shift+P) → „Resume"; Step Frame nur im Play aktiv.
2. **Entity ▸ Create ▸ Cube**: Cube erscheint am Wurzelknoten; dasselbe über
   **„+" im Outliner-Kopf**; beides grau während Play.
3. Cube auswählen → **Entity ▸ Focus Selected** (F), **Hide** (H), **Show All**
   (Alt+H), **Lock** → Titel „Unlock".
4. **File ▸ Recent Projects ▸** zeigt die Hub-Liste; nicht existierender Pfad
   grau; Wechsel fragt bei ungesicherter Szene.
5. **Assets ▸ Create Asset…** und **„+" im Content Browser** öffnen dasselbe
   Popup am angezeigten Ordner; „+" grau auf dem Engine-Root; **↓** öffnet den
   Import-Dialog; **⟳** zeigt kurz „Updating project data…".
6. **Details ▸ Add Component**: sieben Gruppen; „camera r" + Enter fügt Camera
   Rig samt Camera hinzu; ohne Skeletal Mesh ist Animation grau mit Tooltip.
7. **View ▸ View Mode ▸ Wireframe** hakt die Zeile ab und die Toolbar folgt;
   **View ▸ Show ▸ Ground Grid** aus/an; **Window ▸ Landscape Tools** schaltet
   den Toolbar-Modus und holt das Panel nach vorn.
8. **Window ▸ Console** (Ctrl+`) togglet; **Window ▸ Reset Layout** stellt das
   Standard-Dock wieder her.

Auf Windows/Linux zusätzlich: dieselbe Liste in der ImGui-Leiste, plus
**File ▸ Exit**, **Edit ▸ Preferences** (Ctrl+,), **Help ▸ About** (auf dem Mac
im App-Menü).
