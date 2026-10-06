# Interaktives Tutorial (Erststart-Rundgang) — Design

> Stand: 2026-07-31 · Ziel: Wer den Editor zum ersten Mal startet, bekommt **einmal** einen
> geführten Rundgang durch *alles*, was mitgeliefert wird — in einer Sandbox, die der
> Editor selbst erzeugt. Kein Video, kein externes Handbuch, kein Sondermodus: der
> Rundgang läuft im echten Editor, im echten Projekt.

## Designentscheidungen

1. **Der Rundgang folgt dem Nutzer, nicht umgekehrt.** Jeder Schritt nennt genau eine
   Aktion und *merkt selbst*, wenn sie ausgeführt wurde (Entity angelegt, Komponente
   hinzugefügt, Szene gespeichert, Play gedrückt …). Kein „Weiter"-Klicken durch
   Behauptungen — und weil die Erkennung auch bei geschlossenem Fenster läuft, gilt eine
   Aktion auch dann, wenn der Nutzer sie vor dem Lesen gemacht hat.
2. **Nichts wird ferngesteuert.** Der Rundgang öffnet keine Menüs, klickt nichts und
   verändert die Szene nicht. Er beschreibt und beobachtet. Ein Tutorial, das Panels
   selbst bedient, bringt niemandem die Bedienung bei.
3. **Curriculum ist Daten, keine UI.** `TutorialSteps.{h,cpp}` enthält die Kapitel-/
   Schritt-Tabellen *und* die Abschlussprädikate — ImGui-frei, damit beides in
   `tests/test_tutorial.cpp` ohne Fenster geprüft werden kann. `TutorialPanel.cpp` ist
   nur die Haut darüber.
4. **Fortschritt wird als Schritt-*ID* gespeichert**, nicht als Index. Ein später
   eingeschobener Schritt in Kapitel 2 würde sonst die gespeicherte Position aller
   Nutzer verschieben.
5. **Die Sandbox ist ein ganz normales Projekt.** Kein Sonderformat, keine Read-only-
   Flags: alles, was jemand während des Rundgangs baut, behält er.

## Bestandteile

### 1. `HE::tut` — Curriculum + Prädikate (`src/HE_Editor/TutorialSteps.{h,cpp}`)

```
struct Step    { id, title, body, action, focusWindow, Check check, arg }
struct Chapter { id, title, summary, const Step* steps, int stepCount }
struct Signals { entityCount, assetCount, playSessions, selectionSet, playing,
                 sceneUnsaved, landscapeMode, profilerOpen, environmentOpen,
                 exportOpen, componentMask, openTabs }
bool satisfied(const Step&, const Signals& base, const Signals& now);
```

`base` ist der Schnappschuss vom Moment, in dem der Schritt aufging, `now` der aktuelle.
Damit kann ein Check einen **Übergang** verlangen statt eines Zustands, der eventuell
schon gilt: `SceneSaved` feuert nur, wenn vorher etwas ungespeichert war (sonst hakt sich
der Schritt beim Aufschlagen selbst ab und wirkt wie ein Bug), `PlayCycled` erst nach dem
Stoppen, `EntityAdded`/`AssetAdded` nur bei echtem Zuwachs.

`Cursor{chapter, step}` ist die Position; `clamp()` bildet jede kaputte oder veraltete
Position auf eine gültige ab, sodass kein Aufrufer vor dem Indizieren prüfen muss. Ein
Cursor mit `chapter == chapterCount()` ist die eine kanonische „fertig"-Position.

**18 Kapitel, 41 Schritte:** Orientierung · Navigation · Entities · Komponenten · Assets ·
Materialien · Sky/Wetter/Licht · Landschaft · Physik · Partikel · Animation · Navigation ·
UI · Gameplay-Logik · Play-in-Editor · Einstellungen & Profiler · Packaging · Abschluss.

### 2. `TutorialPanel` (`src/HE_Editor/TutorialPanel.{h,cpp}`)

- **`renderWelcome(ctx)`** — die Erststart-Karte über dem Project Hub. Erscheint, solange
  `Tutorial.Offered` nicht in der Editor-Config steht, legt bei „Start the tutorial" die
  Sandbox selbst an (`ProjectPreset::Tutorial`, Sprache HorizonCode) und öffnet den
  Rundgang. „Not now" setzt dasselbe Flag — danach kommt die Karte nur noch über
  Help ▸ Interactive Tutorial zurück (`showWelcome()`).
- **`render(ctx, dt, flags)`** — die schwebende Karte im Editor. Sampelt einmal pro Frame
  die `Signals` (Entity-Count über `view<NameComponent>`, Komponenten-Maske über
  `registry.view<T>().empty()`, Asset-Count durch **iteratives** Ablaufen des
  Content-Baums, offene Tabs als ein durchsuchbarer Blob), latcht den Abschluss und
  wechselt nach 1,4 s automatisch weiter. Umrandet das Panel des aktuellen Schritts mit
  einem pulsierenden Rahmen auf der Foreground-Drawlist.
- `UiFlags` reicht die Fenster-Toggles herein, die als File-Statics in `EditorUI.cpp`
  bzw. in `ExportDialogPanel` liegen (`ExportDialogPanel::isOpen()` wurde dafür ergänzt —
  ein Statusbit, das `render()` setzt, statt `ImGui::IsPopupOpen`, dessen ID-Hash vom
  aufrufenden Fenster abhängt).

Die Karte ist bewusst `NoDocking`: sie zeigt auf die angedockten Panels und muss darüber
schweben, statt selbst eins zu werden.

**Die Karte weicht dem Panel aus, auf das sie zeigt.** Mit dem Standard-Layout und ihrer
Standardecke unten rechts lag sie genau auf Details und auf dem Content Browser, also auf
den Panels, um die es in einem Drittel der Schritte geht. `HE::Ed::Spotlight::KeepClear`
(`PanelSpotlight.{h,cpp}`) prüft deshalb vor jedem `Begin("Tutorial")` die Rechtecke der
`focusWindow`-Panels gegen die Karte vom Vorframe. Es sind dieselben Rechtecke, die der
pulsierende Rahmen zeichnet (`Spotlight::panelRect`, also der Dock-Knoten samt Tab-Leiste).
Überlappen sie, gleitet die Karte zur nächstgelegenen freien Stelle; Kandidaten sind die
Ecken des Editorfensters und die vier Seiten jedes Panels. Ist keine Stelle frei (der
„besuche vier Panels"-Schritt deckt fast den ganzen Editor ab), nimmt sie die mit der
kleinsten Überdeckung, und bei einem schon besuchten Panel darf sie liegen. Geprüft wird
jeden Frame, weil ein Ziel auch spät erscheinen kann (der Profiler öffnet sich, wo er
zuletzt war). Damit die Karte nicht mit dem Nutzer kämpft, gilt: Hat er sie in diesem
Schritt selbst verschoben, bleibt sie bis zum nächsten Schritt dort (`reset()` aus
`gotoCursor`). Beim Verkleinern oder Anklicken der Karte wartet sie nur ab.

### 3. `ProjectPreset::Tutorial` (`src/HE_Tools/.../ProjectManager.cpp`)

Ans Ende des Enums angehängt (der Wert steht als int im `.heproj`). Erzeugt das
Game-Ordnerskelett, ein `TUTORIAL.md` und eine **möblierte** Startszene:

| Entity | Inhalt |
|---|---|
| Sky | EnvironmentComponent, `dayNightCycle true`, `timeOfDay 0.32`, `cloudMode 1`, `cloudCoverage 0.4` |
| Weather | WeatherComponent (Defaults) |
| Ground | 24 × 0.5 × 24 skalierter Cube, graues Terrain-Material |
| Cube | Einheits-Cube auf y = 1, Default-Material |
| Point Light | warmes Punktlicht, Intensität 4, Reichweite 12 |

Drei Details, die keine Geschmacksfragen sind:

- **Ground ist ein flachgedrückter Cube, keine Plane.** Er hat Dicke, damit ein Box-Collider
  im Physik-Kapitel sinnvoll sitzt und der fallende Würfel landet statt durchzufallen.
- **Ground trägt das Terrain-Material, nicht das Default-Material.** Beide sind sonst weiß —
  im ersten Screenshot war der Würfel praktisch unsichtbar auf dem Boden.
- **Der Sky bleibt nicht auf den Struct-Defaults.** Dort ist `dayNightCycle` **aus**, und
  dann ist `timeOfDay` schlicht wirkungslos — der Renderer nimmt die Sonne dann aus dem
  Directional Light der Szene (siehe Kommentar in `EnvironmentSettings.h`). Der Schritt
  „zieh am Time-of-Day-Regler" täte also nichts sichtbares. Headless nachgemessen: mit
  Defaults ist der Frame flach und schattenlos, mit `dayNightCycle: true` + `timeOfDay 0.32`
  steht die Sonne schräg und der Würfel wirft einen Schatten.

Die Szene referenziert ausschließlich die well-known-UUIDs aus `ContentManager/DefaultAssets.h`,
kommt also ohne einen einzigen mitgelieferten Asset-Binärblob aus.

Beide Create-Formulare (Hub-Panel und `File ▸ New Project`) hatten je eine eigene Kopie der
Template-Liste; sie teilen jetzt `ProjectHubPanel::kPresetNames/kPresetDescs`.

## Persistenz

Zwei Einträge in der globalen Editor-Config (`config.json`, `CustomConfig`):

| Key | Bedeutung |
|---|---|
| `Tutorial.Offered` | Die Willkommenskarte wurde einmal beantwortet (egal wie). |
| `Tutorial.Step` | Serialisierter Cursor: die Schritt-ID, oder `"done"`. |

Eine unbekannte ID (Schritt in einer neueren Version entfernt) wird als „fertig" gelesen,
nicht als Fehler — niemand soll auf einem Schritt stranden, der nicht mehr gerendert
werden kann.

## Einstiegspunkte

- Erststart: Willkommenskarte über dem Project Hub.
- `Help ▸ Interactive Tutorial` im Editor (ImGui-Menü) bzw. im macOS-System-Menü
  (`MacMenuBar::Cmd::OpenTutorial`, im Help-Menü ganz rechts). Ohne offenes Projekt führt
  derselbe Befehl auf die Sandbox-Karte.
- Project Hub ▸ Template **Tutorial Sandbox** legt die Sandbox jederzeit neu an.

## Was geprüft ist

`tests/test_tutorial.cpp` (15 Testfälle):

- Curriculum-Integrität: keine doppelten Schritt-/Kapitel-IDs, kein leerer Text, jeder
  `ComponentPresent`-Schritt nennt eine existierende Komponente, jeder `TabOpen` ein
  nicht-leeres Muster.
- Cursor-Arithmetik: `advance` besucht jeden Schritt genau einmal und terminiert,
  `retreat` ist die Umkehrung, `nextChapter` landet immer auf Schritt 0, `clamp` repariert
  Müll, `flatIndex`/`fromFlat` sind invers.
- Fortschritt: Round-Trip über die serialisierte Form für jeden Schritt, plus die beiden
  Sonderfälle (leer, unbekannte ID).
- Prädikate: jede `Check`-Variante, inklusive der Übergangs-Semantik von `SceneSaved` und
  `PlayCycled`.
- Sandbox: Ordner, Manifest-Preset, Szenen-JSON **und** ein Ladetest durch den echten
  `SceneSerializer` in eine `HorizonWorld` — hand-geschriebenes JSON, das die Engine
  anders liest als gedacht, wäre genau der erste Eindruck, den dieses Feature nicht
  vertragen kann. Dazu die Gegenprobe, dass Game/Empty unverändert bleiben.

Optisch verifiziert (Metal, `HE_DUMP_PATH`-Headless-Dump der **erzeugten** Sandbox — also
mit ihrer eigenen Umgebung, nicht der `HE_DUMP_SKYTEST`-Ersatzszene): Sky rendert, Boden
und Würfel stehen richtig, und mit `dayNightCycle: true` wirft der Würfel einen
gerichteten Schlagschatten, den derselbe Dump mit den Struct-Defaults *nicht* hatte.
`cloudMode: 1` ist dabei **nicht** einzeln nachgewiesen — die Kamera des Dumps kommt aus
den `EditorCam*`-Config-Werten und blickt leicht nach unten, es ist also kaum Himmel im
Bild.

Das Ausweichen der Karte prüft `tests/test_ui_shot.cpp` („the tutorial card steps off the
panel its step points at"). Der Test baut das echte Standard-Dock-Layout nach (die Teilungen
aus `BuildDefaultDockLayout`), nimmt das Zielpanel aus einem echten Schritt des Curriculums
und lässt dasselbe `KeepClear` laufen wie der Rundgang. Je ein Fall für links (Quick
Settings), rechts oben (World Outliner), rechts unten (Details), unten (Content Browser) und
oben (schwebender Performance Profiler). Ohne `KeepClear` überdeckt die Karte das Panel,
mit `KeepClear` keinen Pixel davon. Mit `HE_UI_DUMP_DIR` entsteht je Seite ein Vorher/Nachher-Bild.
Ein zweiter Fall zieht die Karte per simulierter Maus zurück aufs Panel: sie bleibt dort,
bis `reset()` den nächsten Schritt anzeigt.

Im **laufenden Editor** belegt es der Zeuge `HE_DUMP_TUTORIALUI=<ordner>` mit
`HE_DUMP_TUTORIALUI_STEPS=id,id,…` (in `TutorialPanel.cpp`). Er öffnet den Rundgang
nacheinander auf diesen Schritten, ohne die Position in die Config zu schreiben, und hält
jeden 120 Frames lang. Dann rastert er die ImGui-Zeichendaten dieses Frames auf der CPU
(`tests/ImGuiSoftwareRaster`, über einen Klon mit umgehängten Texturen, damit das
GPU-Backend seinen Font-Atlas behält) und schreibt `tutorial-ui-<id>.bmp`. Ins Log kommen
das Rechteck der Karte, die Rechtecke der Zielpanels und die überdeckte Fläche. Danach
beendet sich der Editor. Mit `HE_DUMP_TUTORIALUI_NOAVOID=1` weicht die Karte nicht aus;
das ist der Kontrolllauf. Das Bild der Szene selbst fehlt dabei (grau), weil es eine
GPU-Textur ist. Rezept: frisches `HE_CONFIG_DIR` mit `LastProjectPath` auf eine Kopie der
Sandbox und `Tutorial.Offered: true` (so entsteht das Standard-Layout aus einer leeren
imgui.ini), dazu `HE_EXIT_AFTER_FRAMES=20000` (verstecktes Fenster, keine Drosselung).

Messwerte vom 06.10.2026, Debug-Editor, Metal, Fenster 1600×900 pt:

| Schritt | Zielpanel | Überdeckt ohne Ausweichen | Überdeckt mit Ausweichen |
|---|---|---|---|
| `add-mesh` | Details (rechts unten) | 109 140 pt² | 0 |
| `create-asset` | Content Browser (unten) | 28 248 pt² | 0 |
| `sculpt` | Quick Settings (links) | – | 0 |
| `outliner` | World Outliner (rechts oben) | – | 0 |
| `sky-tuning` | World Outliner + Details | – | 0 |
| `fly` | Scene (Mitte) | – | 3 990 pt² |
| `layout` | Scene + Outliner + Details + Content Browser | – | 47 680 pt² (nur Scene) |

Dazu ein Lauf über **alle 27 Schritte**, deren Zielpanel Details oder der Content Browser
ist, in der Reihenfolge des Rundgangs (die Karte wandert also von Schritt zu Schritt mit,
wie beim Nutzer): von `add-mesh` bis `input-mapping` überall 0 pt² überdeckt.

Bei `fly` und `layout` passt die 430×340 große Karte in keine Lücke des Standard-Layouts.
Die linke Spalte ist 287 pt breit, die rechte 341 pt, der Content Browser 280 pt hoch.
Die Karte nimmt dann die Stelle, die am wenigsten überdeckt. Bilder:
[Details vorher/nachher](img/tutorial-card-2026-10-06/details-vorher-nachher.png),
[Content Browser vorher/nachher](img/tutorial-card-2026-10-06/content-browser-vorher-nachher.png),
[weitere Schritte](img/tutorial-card-2026-10-06/weitere-schritte.png).

## Offen

- Das Willkommensmodal ist **nicht** in der laufenden App optisch verifiziert (der
  Zeuge oben setzt erst im geöffneten Projekt ein).
- Der pulsierende Rahmen liegt auf der Foreground-Drawlist und damit **über** der Karte.
  Wo die Karte ein Zielpanel nicht ganz meiden kann (`layout`, Scene-Schritte), läuft die
  Rahmenlinie durch die Karte.
- Ohne offenes Projekt gibt es außerhalb von macOS kein Menü im Hub, also dort auch keinen
  Weg zurück zur Willkommenskarte, wenn sie einmal weggeklickt wurde. Der Hub hätte gern
  eine eigene kleine Menüzeile.
