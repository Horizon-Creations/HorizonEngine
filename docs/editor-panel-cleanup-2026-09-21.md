# Editor-Menüführung: Panels und Inspector (Thema 73, Schritt 3/5, 21.09.2026)

Fortsetzung von `docs/editor-menu-restructure-2026-09-21.md` (Schritt 2, die
Menüleiste) auf der Panel-Ebene: World Outliner, Content Browser, Details.
Dieses Dokument ist die **Karte alt → neu** für die Panels und sagt, was
bewusst so bleibt, wie es ist.

Maßstab wie in Schritt 2: kürzere Wege zu dem, was man ständig tut, gleiche
Muster in allen Panels, keine neuen Panels, keine neuen Funktionen, nichts an
Rendering, Szenen- oder Dateiformat.

---

## 1. Befund nach dem Durchgang

Die Panels sind in besserem Zustand als die Menüleiste war. Outliner und
Content Browser haben dieselbe Kopfzeile (Suchfeld · Typfilter · ×), der
Details-Bereich zeichnet jede Komponente als eigenen Header mit demselben
Rechtsklick-Menü (Copy / Paste Values / Reset / Remove), Preferences und
Project Settings sind sauber in Kategorien geteilt (Audit §6, „keine Befunde").
Was auf Panel-Ebene wirklich hakte, sind drei Stellen, alle aus der
Schwachstellenliste des Audits (§8):

| # | Befund | Panel |
|---|---|---|
| 4 | Add Component: 28 Einträge flach, ungruppiert, ohne Suchfeld | Details |
| 3 | Kein Import-, kein Add-Knopf in der Kopfzeile; Anlegen nur per Rechtsklick auf Leerfläche | Content Browser |
| 2 (Rest) | Entity anlegen nur per Rechtsklick auf Leerfläche, die ein voller Outliner nicht hat | World Outliner |

---

## 2. Karte alt → neu

### Details ▸ Add Component (`InspectorPanel.cpp`, `addComponentMenu`)

| alt | neu |
|---|---|
| Popup mit 28 Zeilen in Einfügereihenfolge (Nav Mesh zwischen Skeletal Mesh und Material, Save State zwischen Joint und Script) | **Suchfeld** oben, darunter **sieben Gruppen als Untermenüs**: Transform (2) · Rendering (10) · Physics (3) · Animation (4) · Gameplay (6) · Navigation (2) · Audio (2) |
| — | Tippen ersetzt die Gruppen durch die **flache Trefferliste** über alle Gruppen, Gruppenname rechts neben dem Treffer; **Enter fügt den ersten Treffer** hinzu |
| Paste Component (X) erste Zeile | = (erste Zeile unter dem Suchfeld, nur ohne Filter) |
| Die vier Skelett-Komponenten (Animator State Machine, Root Motion, Animation Layers, Inverse Kinematics) fehlten ohne Skeletal Mesh **stillschweigend** | Gruppe **Animation ausgegraut** ohne Skeletal Mesh; der Tooltip sagt, was fehlt und wo es steht (Rendering ▸ Skeletal Mesh) |
| Camera Rig immer angeboten (überschrieb ein vorhandenes Rig) | wie alle anderen: nicht angeboten, wenn schon vorhanden; bringt weiterhin eine Camera mit |
| Bereits vorhandene Komponenten werden nicht angeboten | = ; eine Gruppe, deren Zeilen alle vorhanden sind, entfällt |

Der Katalog ist jetzt eine Tabelle (`kAddGroups`), die beide Ansichten
(Untermenüs, Trefferliste) ablaufen. Die Klassen-Tab-Ansicht des
HorizonCode-Editors (`LevelScriptPanel.cpp`, Rechtsklick auf einen
Komponentenbaum) nutzt dieselbe Funktion und bekommt dieselbe Gliederung.

Zuordnung der 28 Einträge:

| Gruppe | Einträge |
|---|---|
| Transform | Transform, Transform 2D |
| Rendering | Mesh, Skeletal Mesh, Material, Light, Decal, Particle System, Foliage, LOD, Rope, Trail |
| Physics | Rigid Body, Collider, Joint |
| Animation *(braucht Skeletal Mesh)* | Animator State Machine, Root Motion, Animation Layers, Inverse Kinematics |
| Gameplay | Camera, Camera Rig, Movement, Script, Save State, Network |
| Navigation | Nav Mesh, Nav Agent |
| Audio | Audio Source, Audio Listener |

### Content Browser ▸ Kopfzeile (`ContentBrowserPanel.cpp`, Breadcrumb-Leiste)

| alt | neu |
|---|---|
| Breadcrumb (Root · Ordner … ) allein in der Leiste | Breadcrumb links, **rechts drei Zellen: Add (+), Import (↓), Refresh (⟳)** in derselben `EditorToolbar::Bar` |
| Asset anlegen: Rechtsklick auf Leerfläche oder Assets ▸ Create Asset… | **+** öffnet dasselbe Create-Popup am angezeigten Ordner (`requestCreateMenu`-Pfad); grau auf dem Engine-Root (read-only, Tooltip sagt es) |
| Import: File/Assets ▸ Import Asset… oder Rechtsklick auf eine Quelldatei | **↓** läuft über denselben Handler wie das Menü (`takeImportRequest` → `triggerImportAsset`), landet also im angezeigten Ordner mit denselben Filtern; grau außerhalb des Content-Roots |
| Refresh: Assets ▸ Refresh Assets | **⟳** setzt dasselbe `contentRefreshPending` |
| Lange Ordnerketten liefen unter den rechten Rand | Kette verliert bei Platzmangel ihre **ältesten** Glieder, Root bleibt, „…" steht für das Weggelassene; der aktuelle Ordner ist immer sichtbar |

Tooltips der drei Zellen kommen aus dem Handbuch (`content.create`,
`content.import`, `Assets/Refresh Assets`), im ausgegrauten Zustand aus dem
Grund fürs Grau.

### World Outliner ▸ Kopfzeile (`OutlinerPanel.cpp`)

| alt | neu |
|---|---|
| Suchfeld · Typfilter · × | **+** · Suchfeld · Typfilter · × |
| Entity anlegen: Rechtsklick auf Leerfläche, Entity ▸ Create (Schritt 2) | **+** öffnet dasselbe Create-Menü (`drawCreateEntityMenu`) mit fester Adresse im Panel; grau während Play, wie Entity ▸ Create |

Die Kontextmenüs (Hintergrund, Entity-Zeile, Create Child) sind unverändert.

### Unverändert, und warum

- **Panel-Fenstertitel** („Quick Settings", „Watch", „Environment",
  „Scene 2/3/4"; Audit #10). Der Titel ist die Identität des Fensters: Hash in
  `imgui.ini`, `s_panelPrefs`-Config-Keys, `docsPanelOpener`, `kPanelTopics`,
  Tutorial-Spotlight, `SetWindowFocus`-Aufrufe, `DockBuilderDockWindow`. Ein
  Umbenennen über das `###`-Suffix ändert den Hash **nicht** nur dann, wenn
  auch alle Aufrufer den neuen String benutzen (`ImHashStr` setzt den Hash bei
  `###` zurück), also bräuchte jeder Aufrufer den vollen Titel. Das ist eine
  eigene, riskante Änderung an gespeicherten Layouts, nicht Aufräumen. Die
  Menüeinträge unter Window und die Handbucheinträge erklären die Namen.
- **Fußleiste**. Der Status-Cluster rechts ist handverkettet (Audit §4), aber
  bedienerisch in Ordnung: alles dort ist Anzeige, nichts davon ist ein Weg
  zu einer Funktion. „Ready" bleibt.
- **Details-Komponentenabschnitte**. Reihenfolge, Header, Rechtsklick-Menü,
  „(changed here)"-Badge: konsistent, keine Befunde.
- **Preferences / Project Settings**: sauber kategorisiert (Audit §6).
- **Environment-Fenster, Watch, Undo History, Audio Mixer**: kleine Fenster
  mit je einem Zweck; nichts zu entzerren.

---

## 3. Docking und Breite

Alle drei Umbauten sitzen in Fenstern, die der Nutzer schmal ziehen oder
undocken kann:

- Outliner-Kopfzeile: das Suchfeld gibt die Breite ab (`avail − + − Typfilter
  − ×`, Untergrenze 60 px wie bisher); die drei Knöpfe behalten ihre Größe.
- Content-Browser-Leiste: die rechte Gruppe hat feste Breite
  (`iconGroupWidth(3)`) und wird **vor** dem Breadcrumb deklariert, damit
  `remaining()` die Kette begrenzt; die Kette kürzt sich von links.
- Add-Component-Popup: ein Popup, kein Dock; Suchfeld 220 px, Untermenüs
  öffnen seitlich wie jedes ImGui-Menü.

---

## 4. Was aus der Schwachstellenliste (Audit §8) jetzt steht

| # | Befund | Schritt 2 | Schritt 3 |
|---|---|---|---|
| 2 | Entity anlegen nur per Rechtsklick | Entity ▸ Create | **+ im Outliner-Kopf** |
| 3 | Asset anlegen/importieren nur per Rechtsklick/Menü | Assets ▸ Create Asset… | **Add / Import / Refresh in der Kopfzeile** |
| 4 | Add Component flach, ohne Suche | — | **erledigt** |
| 10 | Unklare Panel-Beschriftungen | — | bewusst offen (Identität, §2) |
| 15 | Kein Tastatur-Weg für Tabs, keine Palette | offen | offen (neues Verb) |

---

## 5. Technische Spuren

- `InspectorPanel.cpp`: `AddRow`/`AddGroup`/`kAddGroups` (anonymer Namespace),
  `addComponentMenu` mit `Help::Scope("Add Component")`, Filter-Static
  `s_filter` (wird bei `IsWindowAppearing` geleert und fokussiert).
- `ContentBrowserPanel.cpp/.h`: `takeImportRequest()` (Flag `s_importRequested`),
  rechte Zellengruppe, Breadcrumb-Kürzung über `Bar::remaining()`.
- `EditorUI.cpp`: `if (ContentBrowserPanel::takeImportRequest()) triggerImportAsset();`
  direkt hinter der Lambda-Definition.
- `OutlinerPanel.cpp`: „+"-Knopf + Popup `##outliner_add_menu` vor dem Suchfeld.
- `EditorHelp.cpp`: `details.add-component-search`, `Add Component/<Gruppe>` ×7
  (Area-Regel `Add Component/` → Details panel), `outliner.create`.
- `scripts/editor_help_audit.py --check`: 953/953 gedeckt, keine Änderung nötig.
