# UI-Designer: Widgets an eine bestimmte Stelle der Hierarchie ziehen

Stand: 08.09.2026 — Schritt 1 (Orientierung + Entwurf). Noch kein Feature-Code.
Zweig `claude/outliner-widgets-per-drag-drop-an-eine-bestimmte-stelle-in-d`,
`origin/main` (93 Commits, u.a. das MCP-Thema) ist hineingemergt, damit dieser
Entwurf gegen den echten Stand und nicht gegen einen alten Abzug geschrieben ist.

Gemeint ist die **Hierarchie im UI-Designer** (`UIEditorPanel.cpp`), nicht der
Szenen-Outliner. Der Wunsch aus dem Brett — „einen Spacer in einer VerticalBox
zwischen zwei andere Kinder ziehen" — ist ein Widget-Baum-Wunsch. Was mit dem
Szenen-Outliner ist, steht unten unter „Ausdruecklich nicht in diesem Feature".

---

## 1. Der heutige Weg, kartiert

### 1.1 Wo die Reihenfolge ueberhaupt lebt

`HE::UIWidgetTree` (`src/HE_Core/include/UIWidget/UIWidgetTree.h:98`) haelt
**einen flachen Vektor** `std::vector<std::unique_ptr<UIElement>> elements`, und
jedes Element traegt seine `parentId`. Eine Kinderliste gibt es nicht:

```cpp
std::vector<int> UIWidgetTree::childrenOf(int parentId) const
{
    std::vector<int> out;
    for (const auto& e : elements) if (e->parentId == parentId) out.push_back(e->id);
    return out;
}
```
`src/HE_Core/src/UIWidget/UIWidgetTree.cpp:21`

**Geschwisterreihenfolge ist also die relative Position im flachen Vektor.**
Dasselbe gilt fuer `uiChildIndexOf` / `uiChildCountOf`
(`UIWidgetTree.cpp:1277`, `:1289`), fuer jeden Layout-Walk (die Container laufen
direkt ueber `tree.elements` und filtern auf `parentId`) und fuer den Kollab-
Adapter. Es gibt genau **eine** Ordnung, und sie ist global; „Position 2 unter
diesem Parent" ist immer nur eine Sicht darauf.

Der Vektor braucht dabei **keine** Eltern-vor-Kind-Invariante. Der Auto-Size-Pass
sortiert sich seine Container selbst nach Tiefe („innermost first",
`UIWidgetTree.cpp:1076`), die Fokusreihenfolge laeuft explizit ueber
`childrenOf` statt ueber den Vektor (`WidgetManager.cpp:5366`, mit Kommentar
warum), und der Zeichenpass sortiert nach `(layer, depth)`. Ein Element an eine
beliebige Vektorstelle zu schieben ist damit erlaubt.

### 1.2 Die beiden Reparent-Stellen

Im ganzen Designer wird `parentId` an genau vier Stellen geschrieben, davon zwei
beim Umhaengen:

| Stelle | Was |
|---|---|
| `UIEditorPanel.cpp:506` | `addElementAt` — neues Element, `tree.add` haengt hinten an |
| `UIEditorPanel.cpp:546` | `duplicateSubtree` — Kopie, ebenfalls hinten |
| `UIEditorPanel.cpp:669` | **Drop auf einen Hierarchie-Knoten** |
| `UIEditorPanel.cpp:5618` | **Drop auf die Canvas-Wurzel** (Parent 0) |

Die Drop-Stellen sehen beide so aus:

```cpp
if (dragged != nodeId && !st.tree.isDescendantOf(nodeId, dragged) &&
    n->acceptsChildren())
{
    if (UIElement* d = st.tree.find(dragged))
    {
        d->parentId = nodeId;
        structureEdit = true;
    }
}
```

### 1.3 Korrektur an der Aufgabenbeschreibung

Auf dem Brett steht „werden beim Reparenting immer ans Ende der Kinderliste
angehaengt". Das stimmt so nicht, und der Unterschied ist fuer den Entwurf
wichtig: die Drop-Stellen aendern **nur** `parentId` und lassen den Vektoreintrag
liegen, wo er ist. Das umgehaengte Element landet unter seinen neuen Geschwistern
also dort, wo es zufaellig schon im Vektor stand — bei einem frisch angelegten
Element ist das das Ende (weil `add` hinten anfuegt), bei einem alten Element,
das man aus einer spaeteren Box in eine fruehere zieht, kann es vorne landen.

Heute ist die Reihenfolge nach einem Umhaengen also nicht „hinten", sondern
**unbestimmt**. Damit ist die neue Grundoperation nicht nur ein Zusatzfeature:
sie muss den Vektoreintrag *immer* bewegen, auch im schlichten „rein damit"-Fall
(dann ausdruecklich ans Ende), damit das Ergebnis ueberhaupt vorhersagbar wird.

### 1.4 Undo

Der Designer fuehrt kein Kommandoprotokoll, sondern **Ganzbaum-Schnappschuesse**:
`makeSnapshot` = `uiWidgetTreeToJson(tree) + '\x1f' + HC::toJson(graph)`
(`UIEditorPanel.cpp:409`), `commitEdit` (`:492`) legt einen ab, setzt `dirty` und
spiegelt in das Live-Asset. Die Hierarchie ruft `commitEdit` einmal pro Frame,
wenn irgendein Knoten `structureEdit` gesetzt hat (`:5639`).

**Fuer Undo ist damit nichts zu bauen.** Ein Index-Move ist automatisch
rueckgaengig zu machen — vorausgesetzt die JSON-Runde erhaelt die Reihenfolge.
Tut sie: geschrieben wird in Vektorreihenfolge (`UIWidgetTree.cpp:1835`),
gelesen mit `push_back` in Array-Reihenfolge (`:1968`, `:1996`), ohne Sortierung
nach Id.

### 1.5 Kollaboration

`CollabDocSync` ist bereits auf Umsortieren vorbereitet, und zwar wegen genau
dieser Eigenschaft des Designers:

* `IDocAdapter::reorder` existiert (`CollabDocSync.h:100`) mit dem Kommentar
  „Sibling order in the UI designer IS draw order, so a pure reorder is a real
  edit that no per-item payload would carry".
* `DocMirror` haelt `order[kind]` mit (`CollabDocSync.h:120`).
* Der Diff sendet `kOpOrder`, wenn die Sequenz sich bewegt hat und das nicht nur
  Anfuegen/Loeschen war (`CollabDocSync.cpp:170-194`).
* `UITreeAdapter::reorder` wendet es an (`CollabDocSync.cpp:785`), `applyOrder`
  ist der geteilte Helfer (`:287`).
* Angewendet wird die Ordnung **zuletzt** in einem Batch (`:238`), damit die
  Sequenz auch Elemente nennen darf, die die Upserts gerade erst angelegt haben.

**Fuer Kollab ist damit ebenfalls nichts zu bauen.** Ein Move erzeugt einen
Upsert (die `parentId` im Payload) plus ein `kOpOrder` (die Vektorsequenz), und
beides ist schon verdrahtet.

### 1.6 Command-Gateway / MCP

`HE::Ed::EditorCommands` (auf main seit dem MCP-Thema) ist die Tuer in die
**Szene**: `CommandKind::Reparent` nimmt `Entity target` und `Entity parent`,
arbeitet auf `entt`-Handles, publiziert ueber `subjectFor` und kennt **keinen
Index** (`src/HE_Editor/EditorCommands.h:88-125`). Der Widget-Baum kommt darin
nicht vor.

Die MCP-Werkzeuge auf main (`McpToolsEntity.cpp`, `McpToolsHc.cpp`,
`McpToolsApi.cpp`) decken Entities und HorizonCode-Graphen ab; **Werkzeuge fuer
den Widget-Baum gibt es nicht**. `Origin::External` ist fuer dieses Feature heute
also gegenstandslos. Wenn spaeter `widget_*`-Tools kommen, rufen sie dieselbe
Kernoperation aus Abschnitt 3 auf — das ist der Grund, sie in HE_Core zu legen
und nicht ins Panel.

---

## 2. Wo eine Geschwisterreihenfolge etwas bewirkt

Pro Container-Typ, aus dem Code gelesen. „Ordnung" heisst immer: relative
Position im `elements`-Vektor unter demselben Parent.

| Parent | `acceptsChildren` | `laysOutChildren` | Was die Ordnung bewirkt | Einsortieren sinnvoll? |
|---|---|---|---|---|
| **VerticalBox / HorizontalBox** | ja | ja | Stapelreihenfolge entlang der Achse (`boxSlotRect`, `UIWidgetTree.cpp:315`) | **Ja — der Kernfall** |
| **ScrollBox** | ja | ja | dasselbe, nur verschoben (`:312`) | **Ja** |
| **WrapBox** | ja | ja | Reihenfolge auf der Zeile und wo umgebrochen wird | **Ja** |
| **Grid** | ja | ja | „naechste freie Zelle" in Lesereihenfolge — **nur fuer ungepinnte Kinder**. Wer `Grid Row`/`Grid Column` >= 0 gesetzt hat, wird vorab platziert und ignoriert die Ordnung komplett (`UIWidgetTree.cpp:420-434`) | **Ja, aber teilweise irrefuehrend** |
| **TabBox** | ja | ja | Kind-Index = Seitennummer; `tabLabels` ist der Kindname in derselben Reihenfolge (`UIWidgetTree.cpp:1028-1037`) | **Ja, mit Umschreibung — s. 3.2** |
| **Accordion** | ja | ja | Kind-Index = Abschnittsnummer, Kindname = Ueberschrift | **Ja, mit Umschreibung — s. 3.2** |
| **Splitter** | ja | ja | Index 0 = erste Pane, 1 = zweite, ab 2 wird versteckt (`UIElement.cpp:3020`, `splitSlotRect`) | **Ja — „Panes tauschen" ist genau das** |
| **Panel** | ja | nein | Kinder liegen frei per Anchor/Rect. Ordnung entscheidet nur die **Zeichenreihenfolge**: sortiert wird nach `(layer, depth)` mit `std::stable_sort` (`WidgetManager.cpp:5620`), und stabil heisst: bei gleichem Schluessel gewinnt die Vektorreihenfolge. Der Hit-Test benutzt denselben Schluessel (`:2672`) | **Wirkt — aber nicht raeumlich. Siehe Entscheidung 3.4** |
| **Button** | ja | nein | wie Panel (Caption und Icon sind Kinder) | dito |
| **Canvas-Wurzel (parentId 0)** | — | — | wie Panel | dito |
| **ListView** | **nein** | ja | Zeilen kommen aus der Vorlage, ein Drop wuerde ein Element erzeugen, das der naechste Frame wegwirft (`UIElements.h:1274`) | **Nein — kein Ziel** |
| **WidgetRef** | nein | — | Unterbaum wird zur Laufzeit eingepfropft | **Nein** |

Zwei Faelle noch, die man nicht im Panel sieht:

* **Fokus-/Tab-Reihenfolge.** `WidgetManager::focusNext` laeuft tiefensuchend ueber
  `childrenOf` (`:5366`) — mit dem ausdruecklichen Kommentar, dass eben *nicht*
  ueber den Vektor gelaufen wird, weil „re-parenting a row would leave the tab
  order where the row used to be". Ein Move aendert also auch die Tastaturreise.
  Das ist erwuenscht und braucht nichts.
* **Unsichtbare Kinder.** `uiChildIndexOf` zaehlt **alle** Kinder, `boxSlotRect`
  ueberspringt unsichtbare. Eine Einfuegemarke „zwischen dem 2. und 3. sichtbaren"
  muss also auf die Voll-Kinderliste zurueckgerechnet werden. Die Hierarchie
  zeigt ohnehin alle Kinder, also faellt das weg, solange die Marke aus der
  Baumzeile kommt und nicht aus dem Canvas.

---

## 3. Entwurf

### 3.1 Eine Kernoperation, in HE_Core

```cpp
// UIWidgetTree.h
// Haengt `id` unter `newParentId` und setzt es dort DIREKT VOR `beforeSiblingId`.
// beforeSiblingId == 0 heisst "ans Ende". Der ganze Unterbaum kommt mit (er
// haengt an parentId, nicht an der Vektorstelle) — bewegt wird nur der Eintrag
// von `id` selbst, das genuegt, weil Geschwisterordnung immer nur relativ
// zwischen Kindern desselben Parents gelesen wird.
// Falsch (Zyklus, unbekannte Id, Parent nimmt keine Kinder): false, Baum
// unveraendert.
bool moveElement(int id, int newParentId, int beforeSiblingId = 0);
```

Warum eine **Geschwister-Id** und kein Index: ein Index ist in dem Moment
veraltet, in dem man das bewegte Element aus dem Vektor loescht — „an Index 2"
bedeutet vor und nach dem Herausnehmen etwas anderes, und genau daran gehen
Reorder-Implementierungen ueblicherweise kaputt. „Vor S" ist dagegen unter jeder
Zwischenzustandsordnung dasselbe. „Nach S" ist „vor S's naechstem Geschwister,
sonst ans Ende" und wird an der Aufrufstelle in diese Form gebracht.

Warum in HE_Core und nicht im Panel: die Umschreibungen aus 3.2 muessen fuer
jeden Aufrufer gelten, und die kommenden `widget_*`-MCP-Werkzeuge sollen nicht
ihre eigene Variante schreiben. Ausserdem ist es damit **ohne ImGui testbar**,
in `tests/test_ui_widgets.cpp`.

Wachen in `moveElement`, alle schon vorhanden oder trivial:
* `find(id)` und (fuer `newParentId != 0`) `find(newParentId)` muessen greifen.
* `isDescendantOf(newParentId, id)` == false — kein Element in sich selbst.
* Zielparent muss `acceptsChildren()` sagen (Wurzel 0 immer ja).
* `beforeSiblingId`, falls gesetzt, muss ein Kind von `newParentId` sein; sonst
  wie 0 behandeln (ans Ende), nicht scheitern.

### 3.2 Die Fallgrube: indexgestuetzter Zustand

Drei Container speichern **Indizes** auf ihre Kinder. Wird umsortiert, zeigen die
Indizes danach auf etwas anderes — die aktive Seite springt, die aufgeklappten
Abschnitte wandern:

* `UITabBox::activeTab` — `hidesChild` vergleicht mit `uiChildIndexOf`
  (`UIElement.cpp:2896`).
* `UIAccordion::expanded` — eine **Bitmaske ueber Kindindizes**
  (`UIElement.cpp:3168`).
* `UISplitter` — braucht nichts, die Pane *ist* der Index; Tauschen ist die
  gewuenschte Wirkung.

`moveElement` muss das mitschreiben: vor dem Move fuer alten und neuen Parent
(falls TabBox/Accordion) die betroffenen **Element-Ids** festhalten — welche Id
ist die aktive Seite, welche Ids sind offen — und danach aus den neuen Indizes
zurueckrechnen. Faellt die aktive Seite aus dem TabBox heraus, klemmt `activeTab`
auf einen gueltigen Wert (`hidesChild` faengt Ausreisser ohnehin auf Seite 0 ab,
aber ein stiller Sprung ist schlechter als ein bewusster Clamp).

Netter Nebeneffekt: weil dieser Fix die **Nutzlast des Containers** aendert, sieht
der Kollab-Diff die TabBox/Accordion als geaendert und schickt einen Upsert
neben dem `kOpOrder` — die Peers bleiben ohne weiteres Zutun kohaerent.

### 3.3 Die Geste in der Hierarchie

Drei Zonen pro Baumzeile, aus der Maus-Y gegen `GetItemRectMin/Max`:

```
┌──────────────────────────┐  obere 30 %  → davor einsortieren
│  ▸ VerticalBox           │  mittlere 40 % → hinein (ans Ende)
└──────────────────────────┘  untere 30 % → dahinter einsortieren
```

* Zeile ist ein **Container** (`acceptsChildren()`): alle drei Zonen.
* Zeile ist ein **Blatt**: nur davor/dahinter (die Mitte faellt an die naehere
  Kante) — heute macht ein Drop auf ein Blatt gar nichts, das ist bereits eine
  Verbesserung.
* **Canvas-Wurzel** (`UIEditorPanel.cpp:5611`): bleibt „ans Ende der Wurzel",
  `moveElement(dragged, 0, 0)`.

ImGui-seitig: `AcceptDragDropPayload("HE_UIWIDGET_NODE", ImGuiDragDropFlags_
AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)`, damit die
Zone schon **waehrend** des Ziehens bekannt ist; die Einfuegemarke selbst zeichnet
der Fensterzeichner als 2 px starker Strich an der Zeilenkante, ausgerueckt um
die Einzugstiefe des Zielparents (sonst sieht „vor dem ersten Kind" aus wie
„nach der Box"). Angewendet wird erst bei `payload->IsDelivery()`. Beide Flags
gibt es in der vendored ImGui (`imgui.h:1566`).

`HE_UIWIDGET_NEW` und `HE_UIWIDGET_REF` (Palette bzw. Asset-Browser) bleiben in
diesem Schritt auf „hinein, ans Ende" — siehe „nicht in diesem Feature".

### 3.4 Entscheidung: freie Container bekommen die Geste auch

Zur Frage vom Brett, wo ein Index-Insert „sinnlos oder irrefuehrend" waere.

* **Sinnlos**: nur `ListView` und `WidgetRef`, und die nehmen ohnehin keine
  Kinder — sie sind schon heute kein Drop-Ziel.
* **Irrefuehrend im engeren Sinn**: der **Grid mit gepinnten Kindern**. Ein Kind
  mit gesetzter Zeile/Spalte laesst sich einsortieren, und es passiert sichtbar
  nichts. Behandlung: keine Sperre, sondern der Hinweis dort, wo er ankommt — im
  Handbuch beim Grid und in der Hilfe zu `Grid Row`/`Grid Column`, die den Satz
  „Gesetzt heisst: die Reihenfolge in der Hierarchie zaehlt fuer dieses Kind
  nicht mehr" ohnehin brauchen.
* **Wirkt, aber anders als erwartet**: Panel, Button, Canvas-Wurzel. Dort ist die
  Ordnung die **Zeichenreihenfolge** (Abschnitt 2). Das ist eine echte, nuetzliche
  Operation — heute kommt man an sie nur ueber die Zahl `Layer` heran, und wer
  zwei ueberlappende Bilder tauschen will, muss raten. Sie zu sperren waere ein
  Verlust.

**Vorschlag (Entscheidung, kein Befund): die Geste ist ueberall aktiv, wo der
Parent Kinder nimmt.** Unterschieden wird nur, was der Drag sagt: bei einem
`laysOutChildren()`-Parent „vor 'Icon' einsortieren", sonst „vor 'Icon'
zeichnen". Ein Satz im Handbuch dazu. Die Alternative — die Marke nur bei
Layout-Containern anbieten — waere ehrlicher aussehend, aber sie versteckt eine
Faehigkeit hinter der Behauptung, es gaebe sie nicht.

### 3.5 Tests (`tests/test_ui_widgets.cpp`, ohne ImGui)

1. Verschieben innerhalb desselben Parents, vorwaerts und rueckwaerts —
   `childrenOf` ergibt die erwartete Sequenz.
2. Verschieben in einen anderen Parent, vor ein bestimmtes Geschwister.
3. `beforeSiblingId == 0` haengt ans Ende; `beforeSiblingId` gehoert nicht zum
   Zielparent → ebenfalls Ende, Rueckgabe true.
4. In einen leeren Container; auf die Wurzel zurueck.
5. Zyklus (Element unter seinen eigenen Nachfahren) wird abgelehnt, Baum
   unveraendert.
6. Ziel nimmt keine Kinder (ListView) → abgelehnt.
7. Unterbaum haengt nach dem Move vollstaendig mit; `childrenOf` des bewegten
   Elements unveraendert.
8. **TabBox**: dieselbe Seite ist nach einem Reorder noch aktiv.
9. **Accordion**: dieselben Abschnitte sind nach einem Reorder noch offen.
10. **Splitter**: Tauschen der ersten beiden Kinder tauscht die Panes.
11. Reihenfolge ueberlebt `uiWidgetTreeToJson` → `uiWidgetTreeFromJson`
    (die Undo-Runde).
12. Ein `boxSlotRect` eines VerticalBox-Kindes liegt nach dem Einsortieren an der
    erwarteten Y-Position (die Ordnung wirkt wirklich aufs Layout).

### 3.6 Handbuch / Hilfe-Deckung

Der Deckungs-ctest zaehlt **beschriftete Bedienelemente**. Eine Drop-Zone ist
keins, also entsteht dort keine Luecke. Kommt spaeter ein Kontextmenue-Eintrag
„Nach oben" / „Nach unten" fuer Tastaturbedienung dazu, braucht der einen Eintrag
im Scope `"UI Hierarchy"` (der Scope existiert bereits, `UIEditorPanel.cpp:695`),
sonst wird der Audit rot. Zusaetzlich: ein Absatz im Handbuchkapitel zum
UI-Designer, und der Grid-Satz aus 3.4.

**Umgesetzt.** Die Geste hat keine Beschriftung, an die sich eine Erklaerung
haengen liesse, und niemand liest einen Tooltip mit gedrueckter Maustaste. Sie
haengt deshalb als gepunkteter Schluessel `ui.hierarchy-drop` am `(?)` neben der
Ueberschrift „Hierarchy" (`EditorWidgets::helpMarker`, `UIEditorPanel.cpp:5659`);
gepunktete Schluessel werden ueber `ui.` dem Kapitel „UI Designer" zugeordnet und
vom Audit nicht als Bedienelement gezaehlt — die Deckung bleibt 800/800.
`UI Hierarchy/Canvas` sagt jetzt „ans Ende" statt nur „ans oberste Level", und
der Grid-Satz aus 3.4 steht in `UI Widget/Cell (col, row)`.

**Nicht umgesetzt, absichtlich:** der Absatz im *Handbuchkapitel*. Der Text im
Docs-Reader kommt aus `EditorDeps/Docs/he-docs.json`, gebaut von
`scripts/build_docs_bundle.py` aus `HorizonEngineDocs/` im Website-Repo
(`DocsLibrary.h`: „Der Bundle ist DATEN, nicht Code"). Aus diesem Repo ist er
nicht zu aendern. Der Satz, der dort in „UI Designer → The hierarchy" fehlt:
eine Zeile im Baum hat drei Zonen, Mitte hinein, Ober- und Unterkante davor und
dahinter, und die Geschwisterreihenfolge ist in Layout-Containern die
Layout-Reihenfolge, in Panel/Button/Wurzel die Zeichenreihenfolge.

---

## 4. Ausdruecklich nicht in diesem Feature

* **Szenen-Outliner.** `HierarchyComponent::children` ist zwar ein geordneter
  Vektor, aber die Ordnung ist dort rein kosmetisch (Anzeigereihenfolge), und der
  Weg dorthin ist ein anderer: `OutlinerPanel.cpp:395/564` ruft direkt
  `world->reparentEntity`, `EditorCommands::Reparent` hat keinen Index, und eine
  Ordnungsaenderung braeuchte ein eigenes Kollab-Subjekt. Eigenes Thema, falls
  gewuenscht.
* **Der Outliner-Zweig fuer Anwendungsprojekte** (`OutlinerPanel.cpp:166-226`)
  ist eine Diagnoseansicht auf die Live-Widgets, ohne Drag&Drop. Unberuehrt.
* **Neue Elemente an eine Position droppen** (`HE_UIWIDGET_NEW`, `_REF`). Braucht
  eine `insertAt`-Variante von `UIWidgetTree::add`; sinnvoller Folgeschritt, aber
  ein eigener.
* **Ziehen im Canvas-Viewport**, um damit umzuhaengen. Der Viewport-Drag
  verschiebt heute nur Position; das ist ein eigenes Interaktionsthema.
* **Mehrfachauswahl** im Designer gibt es nicht — ein Drag ist ein Element.

## 5. Reihenfolge der naechsten Schritte

1. **Erledigt.** `UIWidgetTree::moveElement` + `canMoveElement` + die
   Index-Umschreibungen aus 3.2, mit den zwoelf Tests aus 3.5. Reines HE_Core,
   kein Editor.
2. **Erledigt.** Die beiden Drop-Stellen (`:669`, `:5618`) laufen ueber
   `moveElement` — schon ohne neue Geste ist das der Fix fuer 1.3 („hinten"
   statt „unbestimmt").
3. **Erledigt.** Die Drei-Zonen-Geste plus Einfuegemarke in `drawHierarchyNode`.
   Wie in 3.4 entschieden ohne Sperre nach Container-Typ: angeboten wird sie
   ueberall, wo der Parent Kinder nimmt, und ein Blatt bekommt davor/dahinter
   (bisher tat ein Drop auf ein Blatt gar nichts).
4. **Erledigt.** Hilfe aus 3.6: `ui.hierarchy-drop` am `(?)` der Ueberschrift,
   Grid-Satz in `UI Widget/Cell (col, row)`, `UI Hierarchy/Canvas` nachgezogen.
   Volle Suite zweimal gruen (135/135, drei `runtime_size*` planmaessig
   uebersprungen), die zwoelf `moveElement`-Faelle einzeln 12/12 mit 66
   Zusicherungen, `editor_help_audit` 800/800 und weiterhin 0 offen.
   Offen bleibt allein der Absatz im Handbuchkapitel — anderes Repo, siehe 3.6.

## 6. Was die Tests nicht abdecken

Die Zonen-Arithmetik in `drawHierarchyNode` (0.30/0.70 beim Container, 0.5 beim
Blatt, „dahinter" → naechstes Geschwister, der Selbst-Drop-Riegel) hat keinen
Test. `drawHierarchyNode` nimmt `AppContext&`, damit ist sie auch fuer
`scripts/he_uishot.py` unerreichbar — der rendert nur Panels, die ohne
AppContext auskommen. Geprueft ist der Unterbau (`moveElement`/`canMoveElement`)
und dass der Editor damit baut und laeuft; die Geste selbst ist bisher nur
gelesen, nicht gefahren.
