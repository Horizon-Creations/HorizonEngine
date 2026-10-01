# Widget-Event „Pre Construct“: Analyse und Design

Thema 119, Schritt 1 (Analyse und Design). Schritt 2 setzt um, was hier steht.

## 1. Was heute passiert (Ist-Lifecycle)

`WidgetManager::createWidget` (`src/HE_Core/src/UIWidget/WidgetManager.cpp:402ff`), in dieser Reihenfolge:

1. Asset laden, Baum aus JSON, Graph aus JSON.
2. `embedWidgetRefs`: alle WidgetRefs werden eingepflanzt; jedes Embed bekommt schon hier **seine eigene Script-Instanz** (`rt().add`/`addCompiled`, Z. 327/329).
3. Eigenes Theme laden, `uiApplyTheme`, `uiApplyTextCatalog`, `refreshElementAssets` für alle Elemente.
4. Script-Instanz des Hosts registrieren (Z. 493/495), Instanz nach `m_instances` schieben.
5. Ids herauskopieren (Construct darf Widgets erzeugen und zerstören, dabei kann `m_instances` umziehen).
6. `rt().fireConstruct(host)` (Z. 518), danach `fireConstruct` für jedes Embed (Z. 522).
   Der Kommentar dort sagt es: „an embed may only be spoken to once the widget holding it has run its own Construct“.
7. `m_visualDirty = true`, Rückgabe der Id.

Das Widget kommt **versteckt** zur Welt. Tick (`WidgetManager::tick`, Z. 2610, nur `w.visible`) und Rendern (`extract`) gibt es erst nach `showWidget`. Zweite Feuerstelle: `addChild` (Z. 704), Construct für die frisch eingepflanzten Zeilen-Embeds.

Daraus folgt:

* **Construct läuft heute schon vor dem ersten Tick und dem ersten Render**, und zwar synchron. Ein Event, das nur „früher als Construct“ läuft, kauft für sich allein nichts.
* Die echte Lücke ist die **Reihenfolge in der Widget-Familie**. Construct des Hosts läuft, bevor irgendein Embed sein Construct gesehen hat. Ruft eine Seite in ihrem Construct eine Funktion auf einem Embed auf (Liste befüllen, Wert setzen), trifft sie ein Embed, das sich noch nicht initialisiert hat.
* Variablen sind nirgends an Element-Properties gebunden, weder einmalig noch pro Frame. `UIWidgetBinding` ist nur die Typbrücke UIPropValue ↔ HorizonCode::Value. Es gibt also keinen Punkt „vor dem Anwenden der Bindings“, an dem ein früheres Event Werte einspeisen müsste.

## 2. Entscheidung: was Pre Construct ist

**PreConstruct ist eine eigene Init-Phase vor jedem Construct der ganzen Widget-Familie.** Jede Instanz (Host und alle Embeds) setzt dort ihre eigenen Werte und lädt ihre eigenen Daten. Erst wenn alle damit durch sind, läuft Construct, und Construct darf sich darauf verlassen, dass jedes Embed seinen PreConstruct-Teil hinter sich hat.

### 2.1 Reihenfolge (verbindlich für Schritt 2)

```
createWidget:
  … Schritte 1–5 wie heute (Baum, Embeds, Theme/Text/Material, rt().add, m_instances, Ids herauskopieren)
  firePreConstruct(host)
  firePreConstruct(embed_0 … embed_n)      ← gleiche Reihenfolge wie Construct
  fireConstruct(host)
  fireConstruct(embed_0 … embed_n)
  m_visualDirty = true
  … später: showWidget → Tick → Render
```

```
addChild (nur die neu hinzugekommenen Embeds [embedsBefore, size)):
  firePreConstruct(neue Embeds)
  fireConstruct(neue Embeds)
```

Begründungen:

* **Nach der Theme-, Text- und Material-Auflösung, nicht davor.** Was PreConstruct setzt (Text, Farbe, Material per Set Property), muss gewinnen. Liefe es vorher, würde `uiApplyTheme` eine gebundene Farbe gleich wieder überschreiben. Außerdem braucht der Graph seine Script-Instanz und den Eintrag in `m_instances` (die Host-Callbacks lösen über die scriptId auf), und beides entsteht erst in Schritt 4.
* **Host vor Embeds innerhalb jeder Phase**, wie bei Construct heute. In PreConstruct kümmert sich jede Instanz um sich selbst, die Reihenfolge innerhalb der Phase ist dort also nebensächlich. Gleich zu bleiben ist einfacher zu erklären und zu testen.
* **Vor erstem Tick und Render:** ist automatisch erfüllt, weil beide Phasen synchron in `createWidget` laufen und das Widget versteckt ist.

### 2.2 Garantien, die ins Handbuch gehören

* **Nur der synchrone Teil zählt.** Ein latenter Knoten (Delay, Warten auf Animation, HTTP mit `OnHttpResponse`) parkt den Rest des PreConstruct-Strangs. Construct läuft trotzdem sofort danach. „Daten laden“ vor Construct heißt also synchron laden (Save Game, Datei, Set Variable). HTTP ist danach nicht fertig.
* **Re-Entrancy:** PreConstruct ist Nutzercode wie Construct und darf Widgets erzeugen und zerstören. Die Ids werden deshalb **vor dem ersten `firePreConstruct` herauskopiert**, und die Klammer umfasst beide Phasen. Nach dem ersten Fire darf `m_instances.back()` nicht mehr angefasst werden.
* **Selbstzerstörung:** Zerstört sich ein Widget in PreConstruct (Destroy Widget(Get Self)), ist das folgende `fireConstruct` ein No-op, weil `Runtime::find(id)` im Fire-Makro nichts findet. Das ist gewollt und bekommt einen Test.
* **Thumbnails und Zeugen:** `makeWidgetThumbnail` (`AssetThumbnailCache.cpp:442`) und der `__uiStyleWitness` (`EditorApplication.cpp:6672`) erzeugen über `createWidget`. Dort laufen also PreConstruct und Construct mit Nutzercode. Das ist bei Construct heute schon so, neu ist es nicht.

### 2.3 Name und Signatur

* Intern **`PreConstruct`**, in der Prosa „Pre Construct“. Der Knotentitel zeigt `n.s` roh (`HcGraphHost.cpp:371`), und die Konvention ist CamelCase ohne Leerzeichen (`BeginPlay`, `OnClicked`).
* **Exec ohne Argument, ohne Element** (`elem = false`), genau wie Construct.
* Ein späteres „läuft im Designer?“ kommt **nicht** als Pin an das Event, sondern als eigener Abfrage-Knoten (z. B. `Is Design Time`). Ein Pin würde die Hook-Signatur `onPreConstruct()` ändern, und jede kompilierte Klasse bräche.

### 2.4 Bewusst nicht in diesem Thema

* **Kein Lauf zur Entwurfszeit im Designer.** Die Canvas des UI-Editors zeichnet den statischen Baum und führt keinen Graph aus. In Unreal ist das der Hauptzweck von PreConstruct. Hier hieße es, Nutzercode mit Nebenwirkungen (Widgets erzeugen, HTTP, Dateien) bei jeder Änderung im Designer auszuführen. Das ist ein eigenes Thema mit eigener Sandbox-Frage.
* **Kein Expose on Spawn.** Wer das Widget erzeugt, kann vor Construct nichts setzen, weil `createWidget` erst danach zurückkehrt. PreConstruct ändert daran nichts. Falls das gewünscht ist: Create Widget mit Eingangspins für öffentliche Variablen, gesetzt **zwischen** PreConstruct und Construct. Eigenes Thema.
* **Nicht in der `Object`-Taxonomie** (`HorizonCode.cpp:2655`). Nur Widgets feuern PreConstruct. Stünde es bei `Object`, böte der Klassen-Editor es Entities und HC-Klassen an, bei denen es nie feuert. Die Event-Liste des Widget-Editors ist ohnehin hart verdrahtet (`kLifecycle`) und kommt nicht aus der Taxonomie.

### 2.5 Namenskollision mit bestehenden Projekten

`inferEventDecls` (`HorizonCode.cpp:2789`) deklariert keinen Event, dessen Name ein Engine-Event ist, und `HcGraphHost.cpp:1663/1675` sperrt Custom Events mit Engine-Namen. Ein Nutzerprojekt (zum Beispiel jemand, der von Unreal kommt) mit einem **Custom Event „PreConstruct“** wird nach dem Einbau stillschweigend zum Engine-Event: es feuert dann beim Erzeugen von selbst, und ein Call Event darauf läuft ins Leere. Im Repo und in den Vorlagen gibt es keinen solchen Event (grep leer), aber Nutzerprojekte liegen nicht im Repo.

Entscheidung für Schritt 2: **eine Warnung im Log beim Laden** (`HE_LOG_WARN`), wenn ein Graph `PreConstruct` in `events` deklariert hat (also aus einer Zeit vor dem Engine-Event stammt), mit dem Hinweis, dass der Event jetzt automatisch vor Construct feuert. Kein automatisches Umbenennen: Wer ihn so genannt hat, wollte meist genau dieses Verhalten.

## 3. Checkliste für Schritt 2

| Stelle | Änderung |
|---|---|
| `src/HE_Core/src/HorizonCode/HorizonCode.cpp:2472` (`engineEvents`) | Zeile `{ "PreConstruct", "onPreConstruct", P::Exec, false }` vor `Construct`. Darüber laufen Codegen (`HcCodegen.cpp:963 engineEventFor`), die Custom-Event-Sperre und `inferEventDecls` automatisch mit. |
| `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h:142` | `virtual void onPreConstruct() { fireEvent("PreConstruct", 0, Value{}); }` |
| `src/HE_Core/include/HorizonCode/HorizonCodeRuntime.h:352` | `void firePreConstruct(InstanceId id);` |
| `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp:933` | `HE_HC_PLAIN_EVENT(firePreConstruct, "PreConstruct", onPreConstruct)` |
| `WidgetManager.cpp:518/522` (`createWidget`) | zwei Phasen wie in 2.1; Copy-Out umfasst beide |
| `WidgetManager.cpp:704` (`addChild`) | PreConstruct-Schleife vor der Construct-Schleife |
| `WidgetManager.h:37` | Kommentar „fires the Construct event“ auf beide Phasen erweitern |
| `src/HE_Editor/UIEditorPanel.cpp:5698` (Details, Widget-Scope) | `kLifecycle` um `PreConstruct` erweitern; Schleifengrenze `3` durch `IM_ARRAYSIZE` ersetzen; `hasArg` bleibt nur für Tick |
| `src/HE_Editor/UIEditorPanel.cpp:5965` (Add-Menü) | `kLifecycle` um `PreConstruct` erweitern, Kommentar anpassen |
| Lade-Warnung (2.5) | beim Graph-Laden bzw. in `inferEventDecls`-Nähe |
| `docs/horizoncode-reference.md:24` | Widget-Graph-Zeile: `PreConstruct`, `Construct`, `Destruct`; kurzer Absatz zu Reihenfolge und Latenz-Garantie |
| In-Engine-Handbuch | Quelle ist `Website/HorizonEngineDocs/*.html` (Geschwister-Checkout), danach `scripts/build_docs_bundle.py` und das erzeugte `EditorDeps/Docs/he-docs.json` committen. Website-Deploy erst nach Rückfrage beim Menschen. |
| Tooltips/HcNodeDocs | prüfen, ob Lifecycle-Events dort Einträge haben (Construct hat heute keinen eigenen); bei Bedarf gleichziehen |

### Tests (Schritt 2)

1. **Reihenfolge über beide Phasen:** Widget mit einem Embed; jeder Graph hängt bei PreConstruct und Construct einen Buchstaben an eine gemeinsame Spur (z. B. über eine öffentliche Variable des Hosts oder eine Probe-Funktion). Erwartet: `P(host) P(embed) C(host) C(embed)`. Negativkontrolle: ohne die neue Phase wäre es `C(host) C(embed)`.
2. **Construct sieht PreConstruct-Werte des Embeds:** Embed setzt in PreConstruct eine öffentliche Variable, Host liest sie in Construct über die Embed-Ref und schreibt sie in eine eigene Variable. Erwartet: Wert angekommen.
3. **Vor erstem Tick/Render:** nach `createWidget` hat Tick noch nicht gefeuert (Zähler 0), PreConstruct schon (1).
4. **Kompilierter Pfad:** `CompiledInstance` mit Override `onPreConstruct` (Muster `tests/test_horizoncode_compiled.cpp:51ff`), Hook wird aufgerufen.
5. **Selbstzerstörung in PreConstruct:** Construct läuft nicht, kein Absturz (Konstrukt aus 2.2).
6. **addChild:** neue Zeile bekommt PreConstruct vor Construct.
7. **Codegen:** ein Graph mit PreConstruct-Event erzeugt ein `void onPreConstruct() override` (Muster in `tests/test_horizoncode_codegen.cpp`).
8. **Beispielwidget** (Auftrag aus Schritt 2): PreConstruct setzt Text aus einer Variablen bzw. lädt einen Wert synchron; Headless-Schuss per `scripts/he_uishot.py` als Zeuge, dass das erste Bild den gesetzten Wert zeigt.

## 4. Nebenbefund zu Thema 118 (Aufblitzen bei Opacity-Animation in Construct)

Pre Construct löst 118 nicht. Die Ursache liegt woanders: `WidgetManager::playAnimation` (Z. 2183ff) merkt sich nur den Clip mit `t = 0` und schreibt **keinen** Wert. Das erste Sample entsteht erst im nächsten `tick` mit `dt > 0` (Z. 2360ff), und das bei `t = dt`. Wird zwischen `showWidget` und diesem Tick schon ein Bild gezogen, zeigt es die gestalteten Werte (z. B. Opacity 1), danach springt die Animation auf ihren Startwert. Naheliegender Fix für 118: in `playAnimation` den Clip sofort bei `t = 0` auswerten und anwenden. Das gehört in Thema 118, nicht hierher.
