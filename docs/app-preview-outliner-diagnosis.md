# App-Modus: Widget-Hierarchie im Outliner — Diagnose

Stand 2026-09-08. Schritt 1 hat den Quelltext gelesen; Schritt 2 hat den
Vorschaupfad **wirklich laufen lassen**. Die Ursache steht fest und ist per
Probe bestaetigt. Der Fix ist hier NICHT eingebaut — das ist der naechste
Schritt.

Belege: `tests/test_app_preview_outliner.cpp`, gebaut und ausgefuehrt gegen
`build-tests/tests/he_tests`.

## Die Ursache

`ProjectManager.cpp:175`, in `shell()` — dem Helfer, aus dem **alle fuenf
geformten App-Templates** ihren Rahmen bauen:

```cpp
const int root = panel(t, -1, "Root", kBack);
```

Das Wurzel-Panel bekommt `parentId = -1`. Die Konvention der Engine ist aber
`parentId == 0` = direktes Kind der Canvas (`UIElement.h:368`), und daran
haengt jede Stelle, die Wurzeln aufzaehlt:

* `OutlinerPanel.cpp:225` — `drawElem(*tree, 0)`
* `UIEditorPanel.cpp:5637` — `st.tree.childrenOf(0)` (die Hierarchie-Liste des
  UI-Designers)
* `WidgetManager.cpp:951` — der Layout-Durchlauf ueber die Wurzeln
* `WidgetManager.cpp:1512` — der Spalten-Leser fuer Table-Row-Templates

Bei `-1` findet keine dieser Schleifen etwas. Der Outliner zeigt den Kopf
„Widget 1" **und darunter nichts**, obwohl die Vorschau 17 bis 48 Elemente
haelt.

### Warum es niemandem vorher auffiel

Gezeichnet wird trotzdem richtig: `parentRectOf`
(`UIWidgetTree.cpp:98-108`) macht `tree.find(-1)` → `nullptr` → faellt auf das
Canvas-Rechteck zurueck. Die App **sieht** also korrekt aus, nur jede Liste der
Hierarchie ist leer. Genau das ist „das Root-Widget zeigt die Hierarchie nicht
richtig an".

### Bestaetigt, nicht vermutet

`-1` → `0` in dieser einen Zeile, neu gebaut, Tests gelaufen:
**4 von 4 Testfaellen gruen, 114 von 114 Assertions**. Zurueckgedreht wieder
2 von 4 rot. Die Probe steht nicht im Commit.

## Was gemessen wurde (echter Lauf, kein Quelltext-Lesen)

Die Diagnosezeile von `EditorApplication.cpp:1994`, aus dem nachgebauten
Vorschaupfad, fuer **jedes** der sechs App-Presets identisch:

```
Application project: preview holds 1 widget(s) after OnInit (GameInstance graph: 3 node(s))
```

Damit sind zwei der drei im Thema genannten Ursachen erledigt:

| Vermutete Ursache | Befund |
| --- | --- |
| kein Graph | ausgeschlossen — 3 Nodes, geladen aus `GameInstance.hcode` |
| Graph erzeugt nichts | ausgeschlossen — `count() == 1`, `liveIds().size() == 1` |
| Widget laedt nicht | ausgeschlossen — `tree(id)` liefert 21–48 Elemente |

Die vierte, im Thema nicht genannte Ursache ist die richtige: **das Widget
laedt, und der Baum hat keinen Einstiegspunkt.**

Pro Preset, aus dem Testlauf:

| Preset | Elemente im Baum | Elemente mit `parentId == 0` | Outliner zeigt |
| --- | --- | --- | --- |
| Application | 2 | 1 | die Hierarchie, korrekt |
| AppSidebar | 20 | 0 | nichts |
| AppWizard | 17 | 0 | nichts |
| AppDashboard | 33 | 0 | nichts |
| AppForm | 21 | 0 | nichts |
| AppTool | 48 | 0 | nichts |

„Elemente mit `parentId == 0`" ist zugleich die Zahl der Elemente, die
`drawElem` ueberhaupt erreicht: bei fuenf von sechs Templates keines von allen.

`Application` ist heil, weil es nicht durch `shell()` geht, sondern durch
`rootWidgetTreeJson` (`ProjectManager.cpp:~380`) — dort bleibt `parentId` auf
seinem Default 0.

## Was nebenbei gruen war

* **Der Neustartpfad.** `restartAppPreview` nachgebaut (fireShutdown → clear →
  setGraph → fireInit) nach einer Bearbeitung ueber
  `ContentManager::getWidgetMutable` + `saveAsset`, so wie
  `UIEditorPanel.cpp:463` speichert: `elements before=21 after=22`, die neue
  Zeile ist nach dem Neustart da. **Kein Cache-Problem.** Die Sorge aus
  Schritt 1 ist damit erledigt.
* **Eindeutige Element-Ids.** Kein Duplikat in irgendeinem Template, auch nach
  `embedWidgetRefs`.
* **Das Manifest-Flag.** `createNewProject(..., appProject=false, ...)` mit
  einem App-Preset setzt `appProject` trotzdem auf true — das Gate, an dem
  `uiLive` haengt, ist heil.

## Der Randbefund, jetzt gemessen

`ProjectManager.cpp:1506` liest `appProject` und nichts sonst; `ProjectData`
hat kein `preset`-Feld. Ein von Hand geschriebenes `.heproj` mit App-Preset
aber ohne `appProject`-Schluessel laedt deshalb als **Spiel**:

```
hand-written manifest: loaded=true appProject=false
```

Kein OnInit, keine Diagnosezeile, eine leere Entity-Liste statt eines
Widget-Baums. Fuer neu angelegte Projekte kann das nicht passieren
(`ProjectManager.cpp:1312`). Im Test als `MESSAGE` + CHECK auf das heutige
Verhalten festgehalten, nicht als Fehler — wer das aendern will, aendert eine
Absicht, keinen Bug.

## Der Test

`tests/test_app_preview_outliner.cpp`, vier Faelle. Er baut den
Vorschaupfad ohne GUI nach: `createNewProject` → `setContentRoot`
(`EditorApplication.cpp:1487`) → dieselben vier Widget-Services
(`EditorApplication.cpp:1299`) → `GameInstance.hcode` per `fromJson`
(`loadGameInstanceGraph`) → `fireInit`.

Er prueft nicht „irgendein Element hat parentId 0", sondern **den echten
Vertrag von `drawElem`**: von parentId 0 aus absteigen und zaehlen, was dabei
erreicht wird. Was nicht erreicht wird, wird namentlich ausgegeben.

**Zwei der vier Faelle sind absichtlich rot** und werden mit dem Einzeiler in
`shell()` gruen. Wer den Fix baut, aendert am Test nichts.

Die ganze Suite dazu gelaufen: **2544 Faelle, 2542 gruen, 2 rot** — genau die
zwei hier. Sonst ist nichts angefasst.

## Grenze dieses Durchlaufs

Gemessen wurde der Pfad, den der Outliner LIEST. Der Editor selbst wurde nicht
gestartet und ImGui hat nichts gezeichnet — was das Panel malt, ist weiterhin
nur aus dem Quelltext abgeleitet. Fuer die Ursache spielt das keine Rolle: ein
Baum ohne Wurzel bei parentId 0 hat fuer `drawElem` keine erste Zeile,
unabhaengig davon, wer sie zeichnen wuerde.
