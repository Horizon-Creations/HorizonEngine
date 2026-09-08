# App-Modus: Widget-Hierarchie im Outliner — Zwischenbefund (Schritt 1, abgebrochen)

Stand 2026-09-08. Schritt 1 (Reproduzieren + Diagnose) wurde vom Leitstand
abgebrochen, bevor der Repro-Test gebaut und ausgefuehrt werden konnte. Alles
hier unten ist am Quelltext GELESEN, nicht laufend gemessen. Kein Fix.

## Was der Code sagt (mit Belegen)

* Die Diagnose-Logzeile steht auf `src/HE_Editor/EditorApplication.cpp:1994`
  (nicht ~2145, wie im Thema notiert). Sie meldet
  `m_editorWorld->widgets().count()` und `m_gameInstanceGraph.nodes.size()`.
* `uiLive` (`EditorApplication.cpp:1972`) und `appLivePreview`
  (`EditorApplication.cpp:6087`) haengen BEIDE nur an
  `currentProject().appProject`. Kein zweites Kriterium.
* **Der Outliner liest denselben WidgetManager, den `fireInit` befuellt.**
  `AppContext.world` ist `world()` (`EditorApplication.cpp:6070`), also
  `Application::m_world` (`Application.h:140`), und `setWorld(m_editorWorld.get())`
  laeuft in `EditorApplication.cpp:1455`. `svc.createWidget` schreibt in
  `m_editorWorld->widgets()` (`EditorApplication.cpp:1299`).
  → Ursache "Panel liest einen anderen Manager" ist damit ausgeschlossen.
* `drawElem(*tree, 0)` (`OutlinerPanel.cpp:225`) passt zur Konvention
  `parentId == 0` = Kind der Canvas (`UIElement.h:368`, `UIWidgetTree.h:152`).
  → Der Einstiegspunkt ist richtig. ABER: `drawElem` zeigt still NICHTS von
  einem Element, dessen `parentId` weder 0 noch die Id eines anderen Elements
  im selben Baum ist. Ein Waisenkind faellt lautlos raus. Das ist der
  plausibelste Kandidat fuer "zeigt die Hierarchie nicht RICHTIG an"
  (im Unterschied zu "zeigt nichts") und ist noch nicht geprueft.

## Randbefund aus der Chefchen-Recherche: bestaetigt, und schaerfer

`ProjectManager.cpp:1506` liest `appProject` ohne ODER mit `isAppPreset(preset)`.
Das laesst sich dort auch gar nicht nachruesten: **`ProjectData` hat ueberhaupt
kein `preset`-Feld** (`ProjectManager.h:258-261`), und `loadProject` liest den
Schluessel `"preset"` nirgends. Geschrieben wird er (`ProjectManager.cpp:1331`),
gelesen nie.

Folge, praeziser als "Widget laedt nicht": Bei einem von Hand editierten oder
alten `.heproj` mit App-Preset aber fehlendem/false `appProject`-Schluessel ist
`appProject == false`. Dann ist `uiLive` false, `fireInit` laeuft NIE, die
Diagnosezeile 1994 wird NIE ausgegeben, und der Outliner nimmt den
Entity-Zweig — er zeigt also gar keine Widget-Hierarchie, sondern eine leere
Entity-Liste. Fuer neu angelegte Projekte kann das nicht auftreten
(`ProjectManager.cpp:1312` merged `appProject || isAppPreset(preset)` und 1333
schreibt das Ergebnis).

**Erste Frage an den Nutzer sollte deshalb sein: steht `"appProject": true` in
seiner `.heproj`?** Das kostet nichts und schliesst den ganzen Zweig aus.

## Noch nicht geprueft (fuer den naechsten Schritt)

1. **Waisen-Elemente**: `tpl::dashboard/form/tool`
   (`ProjectManager.cpp:280/338/415`) auf Id/ParentId-Konsistenz pruefen, dazu
   das Remapping in `WidgetManager::embedWidgetRefs` (Graft eingebetteter
   Widgets, `WidgetManager.cpp:394ff`).
2. **Der Restart-Pfad**: `restartAppPreview`
   (`EditorApplication.cpp:6775-6812`) laeuft nach jeder Asset-Aenderung
   (`UIEditorPanel.cpp:5817`). Zu pruefen: liest der ContentManager das
   `UI/RootWidget.hasset` wirklich neu oder liefert er den Cache (=> veraltete
   Hierarchie nach einer Bearbeitung), und was macht `restoreState`
   (`WidgetManager.cpp:4608ff`) mit einem Baum, dessen Struktur sich geaendert
   hat. "Ich habe im UI-Editor ein Kind hinzugefuegt und der Outliner zeigt es
   nicht richtig" ist die wahrscheinlichste Nutzergeschichte.
3. **Der Repro-Test selbst**, geplant als `tests/test_app_preview_outliner.cpp`
   nach dem Muster von `tests/test_app_todo.cpp` (TempDir, ContentManager,
   `HorizonCode::Runtime::Services` wie `EditorApplication.cpp:1299` gebunden,
   `fireInit`): pro App-Preset `createProject`, `GameInstance.hcode` per
   `fromJson` laden (3 Nodes, 3 Links erwartet), danach `count()==1`,
   `liveIds().size()==1`, Baum nicht leer, mindestens ein Element mit
   `parentId==0`, und — das ist der eigentliche Vertrag von `drawElem` —
   JEDER `parentId` ist 0 oder eine existierende Element-Id.

## Build

`build-tests/` ist in diesem Worktree bereits konfiguriert (Unix Makefiles,
Debug, clang) und greift ueber `FETCHCONTENT_SOURCE_DIR_*` auf die schon
heruntergeladenen Abhaengigkeiten in `<Haupt-Checkout>/build/_deps` zurueck, es
wird also nichts neu geladen. `cmake --build build-tests --target he_tests -j10`
ist noch nie gelaufen — der naechste Schritt muss mit einem kalten Build von
HE_Core + HE_Scene + HE_Tools rechnen.
