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

* ~~**Kein Lauf zur Entwurfszeit im Designer.**~~ Nachgezogen in Schritt 4, siehe Abschnitt 5.
* ~~**Kein Expose on Spawn.** Wer das Widget erzeugt, kann vor Construct nichts setzen, weil `createWidget` erst danach zurückkehrt. PreConstruct ändert daran nichts. Falls das gewünscht ist: Create Widget mit Eingangspins für öffentliche Variablen, gesetzt **zwischen** PreConstruct und Construct. Eigenes Thema.~~ Nachgezogen in Schritt 5, siehe Abschnitt 6. Die Werte landen dort **vor** PreConstruct, nicht dazwischen (Begründung in 6.2).
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

## 5. PreConstruct zur Entwurfszeit (Schritt 4, aus Frage #14, Möglichkeit 1)

In Unreal ist das der Hauptzweck von PreConstruct: der Designer zeigt, was der Code vor dem ersten Bild setzt (Beschriftung aus einer Variablen, Farbe nach Zustand). Hier lief bisher kein Graph im Designer, die Canvas zeichnet `st.tree` per ImDrawList. Abschnitt 2.4 hatte das wegen der Sandbox-Frage ausgeklammert. Diese Frage wird hier beantwortet.

### 5.1 Was läuft, und worauf

* **Nur PreConstruct.** Kein Construct, kein Tick, kein Destruct, keine Eingabe-Events. Construct ist der Ort für Code, der zur Laufzeit gehört (Daten holen, andere Widgets ansprechen). Das ist genau die Trennung, die UMG zwischen PreConstruct und Construct zieht.
* **Auf das Dokument, nicht auf das gespeicherte Asset.** Der Lauf nimmt `st.tree` und `st.graph`, wie sie im Editor stehen, mit ungespeicherten Änderungen. Eingebettete Widgets kommen aus dem ContentManager, also mit dem Stand, den auch `drawEmbeddedTree` zeichnet. Ihr PreConstruct läuft mit, die Familie ist dieselbe wie zur Laufzeit.
* **Immer interpretiert.** `compiledClasses()` wird übergangen, für den Host und alle Embeds. Eine kompilierte Klasse aus der Spiel-dylib wäre älter als der Graph, den man gerade bearbeitet.
* **In einem Wegwerf-WidgetManager mit eigener Runtime.** Neu `WidgetManager::runDesignTimePreConstruct(content, assetPath, tree, graph)`. Es baut die Instanz wie `createWidget` (gemeinsamer Helfer, damit beide nicht auseinanderlaufen), feuert nur PreConstruct und baut die Familie lautlos wieder ab. `Runtime::remove` statt `destroy`, also kein Destruct. Der Aufrufer stellt die Runtime, und damit die Sandbox.

### 5.2 Sandbox: was im Designer laufen darf

Die Runtime des Laufs bekommt **keine** `Services` außer einem gefilterten `callApi`:

| Weg | zur Entwurfszeit |
|---|---|
| Set/Get Property, (Ref)-Varianten, Variablen, Call Function auf sich selbst und auf Embeds, Emit/Bind Event innerhalb der Familie | erlaubt, wirkt nur auf den Wegwerf-Baum |
| Create/Destroy/Show/Hide Widget, Create/Destroy Object | aus (Services ungebunden), Ergebnis 0 bzw. nichts |
| Engine Call: Gruppen `math`, `string`, `json`, `datetime`, nur reine Zeilen (`isExec == false`) | erlaubt, berechnen nur aus ihren Argumenten (bzw. der Uhr) |
| Engine Call: `widget.isDesignTime` | erlaubt, liefert `true` |
| Engine Call: `widget.childRef` (Get Child Widget) | erlaubt, aufgelöst im Wegwerf-Manager, damit der Host seine Embeds erreicht |
| jede andere Engine-Call-Zeile (`fs`, `save`, `prefs`, `http`, `net`, `db`, `process`, `audio`, `entity`, `scene`, `ui`, `widget.*`, `random`, `print`, …) | abgelehnt: leere Ergebnisse (Ausgänge auf Vorgabe), Zeilen-Id im Ergebnis vermerkt |
| Delay, latente Knoten | parken für immer: der Wegwerf-Runtime tickt niemand |
| Endlosschleifen, Rekursion | der Interpreter bricht nach `kMaxSteps = 4096` Schritten bzw. Tiefe 64 ab |
| Multiplayer-Routing (`runOn`) | ungebunden, läuft lokal, d. h. im Wegwerf-Baum |

Warum eine **Positivliste nach Gruppe** und nicht „alles mit `isExec == false`“: auch `fs`, `save`, `prefs` haben reine Getter, die die Platte lesen. Neue Gruppen sind damit zur Entwurfszeit erst einmal aus, bis jemand sie bewusst freigibt. `random` bleibt aus, weil seine Zeilen den prozessweiten Generator weiterdrehen (sie sind Exec-Zeilen). `print` bleibt aus, damit der Designer die Konsole nicht bei jeder Änderung füllt.

Die Politik steht in HE_Scene (`HE::api::designTimeAllows(id)` und `HE::api::designTimeCallApi(widgets, refused)`), damit Editor und Tests dieselbe Funktion benutzen.

### 5.3 Knoten „Is Design Time“

Eine reine Registry-Zeile `widget.isDesignTime` (Bool, ohne Argumente), Anzeigename **Is Design Time**, liest das neue Feld `HE::api::Ctx::designTime` (Vorgabe `false`). Nur der Sandbox-Dispatcher baut einen Ctx mit `true`. Im Spiel, im PIE und unter einem ungebundenen `callApi` ist die Antwort also `false`. Wie in 2.3 vorgesehen ist es kein Pin am Event, die Hook-Signatur `onPreConstruct()` bleibt. Kompilierte Klassen bekommen ihn über den generischen EngineCall-Weg (`hc::callApi`) ohne Codegen-Änderung.

Typischer Gebrauch: `PreConstruct → Branch(Is Design Time)`, auf `true` Platzhalterdaten setzen, auf `false` die echten laden.

### 5.4 Wie das Ergebnis auf die Canvas kommt

* **Vergleich im Wegwerf-Baum, nicht mit `st.tree`.** `runDesignTimePreConstruct` kopiert den Instanz-Baum direkt vor dem ersten `firePreConstruct` und vergleicht danach jede Property jedes Elements (`allProperties`, `samePropValue`). Gemeldet wird, was sich unterscheidet, mit Endwert. Ein Vergleich mit `st.tree` ginge nicht: `registerInstance` hat Theme und Sprache schon in den Baum geschrieben, und jede themengebundene Property sähe wie „von PreConstruct gesetzt“ aus. Die Kopie liegt nach Theme und Sprache, deshalb fällt das hier weg. (Umgesetzt statt eines Schreibprotokolls in `makeBindings`: der Vergleich fängt auch indirekte Schreibvorgänge, etwa eine Radio-Gruppe, die beim Setzen von `Checked` die anderen abschaltet.)
* **Elemente des Hosts** tragen im Wegwerf-Baum dieselben Ids wie in `st.tree`. Ein RAII-Guard in `drawCanvas` schreibt die Werte für die Dauer des Frames hinein und nimmt sie danach zurück, wie `ScrubPreview`. Er liegt **vor** `ScrubPreview`, die Animationsvorschau legt sich also darüber (wie zur Laufzeit: erst PreConstruct, dann spielt die Animation). Zurückgeschrieben wird nur, was noch den angewendeten Wert hat. Hat der Nutzer im selben Frame denselben Wert geändert (Drag, Details), gewinnt seine Änderung. Damit erreichen die Entwurfszeit-Werte nie Save, Undo oder den Collab-Spiegel.
* **Elemente der Embeds** werden in `drawEmbeddedTree` in die ohnehin schon kopierte, gelayoutete Fassung (`laid`) geschrieben. Dazu merkt sich das Ergebnis pro Embed `(Ref-Element, idOffset)`, auch für verschachtelte.
* **Commits innerhalb des Canvas-Frames.** Ein losgelassener Drag oder ein Drop aus der Palette ruft `commitEdit` noch in `drawCanvas` auf, während die Entwurfszeit-Werte im Baum stehen. `makeSnapshot`, `applyToAsset` und `saveState` halten deshalb ein `DesignValuesOut`: für die Dauer des Schreibens stehen die gestalteten Werte im Baum, danach wieder die gezeigten. Ohne das landeten Farbe und Text aus PreConstruct im Undo-Schnappschuss und im Live-Asset (nachgewiesen im Designer-Test, siehe 5.7).
* **Details-Panel und Hierarchie** zeigen weiter die gestalteten Werte. Gespeichert wird, was gestaltet ist, nicht was der Code daraus macht.

### 5.5 Wann neu gerechnet wird

* Wenn sich das Dokument geändert hat: Schlüssel ist der letzte Undo-Schnappschuss (`commitEdit` legt nach jeder abgeschlossenen Änderung einen an). Während eines Drags läuft also nichts neu, erst beim Loslassen.
* Wenn sich ein eingebettetes Asset geändert hat (dessen Baum- oder Graph-JSON im ContentManager), höchstens zweimal pro Sekunde geprüft.
* Wenn der Schalter umgelegt wird. **Schalter** in der Designer-Toolbar, „Pre Construct“, Vorgabe an (wie in UMG), nur Sitzungszustand, nicht im Asset.
* Abgelehnte Engine-Calls stehen klein unter der Canvas („Pre Construct (design time): skipped http.get, fs.readText“). Damit ist sichtbar, warum ein Wert im Designer fehlt, und wohin der `Is Design Time`-Zweig gehört.

### 5.6 Was nicht Entwurfszeit ist

* Thumbnails (`makeWidgetThumbnail`) und `__uiStyleWitness` laufen weiter über `createWidget`, also mit PreConstruct **und** Construct, mit `Is Design Time = false`. Sie zeigen das Widget, wie es zur Laufzeit ankommt. Das war schon vor diesem Thema so.
* Die Live-Vorschau / PIE ist Laufzeit.
* Expose on Spawn ist Laufzeit (Abschnitt 6). Im Designer läuft PreConstruct mit den Vorgabewerten der Variablen, so wie ein Create Widget ohne verdrahtete Pins sie sähe.

### 5.7 Tests (Schritt 4)

1. `widget.isDesignTime`: `false` mit Standard-Ctx, `true` mit `designTime = true`. Die Zeile ist rein, Bool, in der Gruppe Widget.
2. Sandbox-Politik: `math.*`, `string.*`, `widget.isDesignTime`, `widget.childRef` erlaubt; `fs.*`, `save.*`, `http.*`, `random.*`, `print.*`, `widget.create` abgelehnt; unbekannte Id abgelehnt.
3. Lauf: PreConstruct setzt Text per Set Property und Farbe nur im `Is Design Time`-Zweig; Ergebnis enthält genau diese Schreibvorgänge mit den Endwerten; ein `fs.writeText` im selben Strang schreibt keine Datei und steht in `refused`; Create Widget liefert 0; Construct läuft nicht (Variable bleibt 0); das Eingangsdokument ist unverändert.
4. Theme-Schreibvorgänge erscheinen nicht im Ergebnis (gebundene Farbe, kein Graph-Schreibvorgang).
5. Embed: das PreConstruct eines eingebetteten Widgets landet unter seinem Ref-Element mit lokaler Id.
6. Ungespeicherter Graph: der Lauf nimmt das übergebene Dokument, nicht das registrierte Asset.
7. Canvas-Guard: nach dem Frame ist `st.tree` byteweise unverändert; ein im Frame geänderter Wert überlebt das Zurückschreiben.

Umgesetzt: 1–5 in `tests/test_widget_design_time.cpp` (6 Fälle; 6 steckt im Lauf-Fall: das registrierte Asset hat einen anderen Graphen). 7 in `tests/test_widget_designer_ui.cpp` („Pre Construct shows on the canvas, never in the document“) mit dem echten `UIEditorPanel::render`: grüne Canvas, gestalteter `liveTree`, Drag-Commit aus dem Frame, Live-Asset und Save bleiben gestaltet. Gegenproben: Construct im Lauf bzw. `fs.writeText` freigegeben macht die Lauf- und Sandbox-Fälle rot; ohne `DesignValuesOut` steht nach dem Drag Grün und „Placeholder“ im Asset.

Bekannte Grenzen: Der Schlüssel für eingebettete Assets betrachtet nur direkt eingebettete Widgets, eine Änderung zwei Ebenen tiefer wird erst mit der nächsten Dokumentänderung sichtbar. Ein Drag läuft während der Bewegung mit den Werten des letzten Laufs, neu gerechnet wird beim Loslassen. `drawCanvas` ruft `uiApplyAutoSize(st.tree)` mit den gezeigten Werten auf, die abgeleitete Größe eines Auto-Size-Elements im gespeicherten Asset entspricht also dem PreConstruct-Text. Das ist harmlos, weil die Laufzeit sie neu berechnet, und war bei der Animationsvorschau schon so. Get Child Widget mit unverdrahtetem Widget-Pin meint wie überall das eigene Widget (`widgetIdForScript` im Sandbox-Dispatcher, getestet mit beiden Verdrahtungen).

## 6. Expose on Spawn (Schritt 5, aus Frage #14)

Wer ein Widget erzeugt, soll ihm Werte mitgeben können, die schon beim ersten Code des Widgets da sind: Create Widget bekommt Eingangspins für die Variablen, die das Widget dafür freigibt. Vorbild ist UMG.

### 6.1 Was der Nutzer sieht

* In den Details einer Variablen ein Häkchen **Expose on Spawn**. Nur für öffentliche Instanzvariablen (`access == 0`, `scope == 0`), sonst ausgegraut. Gespeichert als `"spawn": true` an der Variablen, nur wenn gesetzt; ein Graph ohne das Häkchen ist byte-gleich zu vorher. Beim Laden wird es an privaten und lokalen Variablen verworfen.
* **Opt-in, nicht jede öffentliche Variable.** Öffentlich heißt hier auch „per Get/Set (Ref) erreichbar“, das sind oft Referenzen und Listen für die Verdrahtung. Jede davon als Pin an jedem Create Widget wäre Lärm. So hält es auch Unreal (Instance Editable + Expose on Spawn).
* Create Widget hat für jede solche Variable einen Dateneingang, in Deklarationsreihenfolge, benannt und getypt wie die Variable. Gespiegelt in `Node::params` wie bei Call Function. Der Ausgang `Widget` liegt dahinter. Ein Knoten ohne Pins (jeder Graph von vorher) hat dasselbe Layout wie bisher, der Ausgang bleibt Pin 2. Eine Migration wie bei Create Object ist nicht nötig.

### 6.2 Reihenfolge: vor PreConstruct

Der Text in 2.4 und auf dem Brett sagte „zwischen PreConstruct und Construct (analog Unreal)“. Das widerspricht sich, und hier gilt Unreal: dort setzt der Create-Widget-Knoten die Werte direkt nach dem Erzeugen, PreConstruct und Construct laufen erst danach (`UUserWidget::OnWidgetRebuilt`). Entscheidend ist der Kernfall dieses Themas: PreConstruct schreibt „Score: “ + `score` in einen Text. Kämen die Spawn-Werte erst nach PreConstruct, zeigte das erste Bild bei einem Pin `score = 99` trotzdem den Vorgabewert, und Designer (Schritt 4) und Laufzeit liefen mit demselben PreConstruct auseinander.

```
createWidget(content, path, spawn):
  registerInstance          ← Variablen mit ihren Vorgaben geseedet
  m_instances.push_back, Ids herauskopieren
  Spawn-Werte auf den Host  ← neu
  firePreConstruct(host, embeds…)
  fireConstruct(host, embeds…)
```

Ein PreConstruct, das dieselbe Variable selbst setzt, überschreibt den Spawn-Wert. Das ist wie in Unreal und gehört ins Handbuch.

### 6.3 Welcher Pin wirkt

Dieselbe Regel wie bei Make Struct (`HorizonCode.cpp`, `HcCodegen.cpp`): ein Pin wirkt, wenn er **verdrahtet** ist oder **am Knoten einen Wert trägt** (`pinDefaults`). Sonst behält das Widget seinen eigenen Vorgabewert.

Das Inline-Feld eines unverdrahteten Bool/Int/Float/Double/String-Pins legt beim Zeichnen einen Eintrag an (`drawPinDefaultEditor`). Damit ein solcher Pin nicht still 0 setzt, wird er beim Spiegeln mit dem **Vorgabewert der Widget-Variablen** vorbelegt: der Pin zeigt, was ankommt. Vec2, Color, Referenzen, Structs und Container haben kein Inline-Feld und wirken nur verdrahtet.

### 6.4 Laufzeit

* `HorizonCode::SpawnValue { name, value }`, `SpawnValues` = Vektor davon.
* `Context::createWidget` und `Runtime::Services::createWidget` nehmen `(path, const SpawnValues&)`. `hc::createWidget(ctx, path)` bleibt, dazu ein Überladung mit Werten für den generierten Code.
* `WidgetManager::createWidget(content, path, const SpawnValues* spawn = nullptr)`. Gesetzt wird nur auf den Host, nicht in `registerInstance` (sonst liefe es im Entwurfszeit-Lauf mit).
* Gesetzt über `Runtime::setPublicVariable(id, name, value)`, dieselbe Prüfung wie Set (Ref): nur öffentliche Instanzvariablen, für interpretierte wie kompilierte Klassen. Eine unbekannte oder private Variable (das Widget hat sie seit dem letzten Speichern des Aufrufers umbenannt oder versteckt) wird mit Warnung übersprungen. Auf „Expose on Spawn“ prüft die Laufzeit nicht: das Häkchen steuert, welche Pins der Editor anbietet, und kompilierte Klassen brauchen so kein neues Reflexionsfeld.
* Lua, Python und die Registry-Zeile `widget.create` bleiben ohne Werte.

### 6.5 Pins aktuell halten

* `spawnPinsOf(widgetGraph)` liefert die freigegebenen Variablen mit Typ und Vorgabewert. `syncSpawnPins(graph, nodeId, now, before)` spiegelt sie in den Knoten: Leitungen folgen ihrem Pin über den Namen (`captureLinkRemapSnapshot`/`remapLinksFromSnapshot`), ein entfernter Pin verliert seine Leitung sichtbar. `pinDefaults` sind nach Index geschlüsselt, der Snapshot verschiebt sie nicht, deshalb werden sie hier nach Namen umgeschlüsselt. Neue oder umgetypte Pins bekommen den Vorgabewert der Variablen. Ein Pin, der noch den **vorherigen** Vorgabewert trägt (`before`), folgt dem neuen.
* Umbenennen einer Variablen läuft über das projektweite Umbenennen (`HcRename`): es benennt den Pin an jedem Create Widget dieses Widgets mit um. Dem Spiegel allein überlassen sähe der umbenannte Pin neu aus und verlöre seinen getippten Wert (die Leitung bliebe über den Index-Rückfall des Snapshots).
* Der Editor spiegelt beim Wählen des Assets in den Details und laufend in `buildModel`: pro Asset höchstens zweimal pro Sekunde, Schlüssel ist der Hash des Graph-JSON im ContentManager. Ändert sich ein Knoten dadurch, meldet der Host `onEdit(false)`: das Dokument ist geändert (es speichert die neuen Pins), aber es entsteht kein Undo-Punkt.

### 6.6 Codegen

Ohne wirkende Pins wird exakt die bisherige Zeile erzeugt (byte-gleicher Paritätstext). Sonst ein Block, der die Werte in `hc::SpawnValues` packt und die Überladung aufruft.

### 6.7 Tests (Schritt 5)

1. Reihenfolge: ein Spawn-Wert ist in PreConstruct sichtbar (und damit im ersten Bild), und in Construct. Negativkontrolle: die Werte hinter PreConstruct gesetzt macht den Fall rot.
2. Unverdrahteter Pin ohne Wert lässt den Vorgabewert stehen; ein Pin mit Wert am Knoten setzt ihn.
3. Private oder unbekannte Variable wird übersprungen, kein Absturz, das Widget entsteht trotzdem.
4. Spiegeln: Pins in Deklarationsreihenfolge, nur Häkchen + öffentlich; Leitungen folgen einer Umsortierung, eine entfernte Variable verliert ihre Leitung, `pinDefaults` wandern mit, neue Pins sind vorbelegt, ein unveränderter Pin folgt einem neuen Vorgabewert.
5. Alter Graph ohne Pins: Ausgang `Widget` auf Pin 2, Leitungen unverändert.
6. JSON: `spawn` rundet, an privaten Variablen verworfen.
7. Codegen: ohne Pins identische Zeile; mit verdrahtetem Pin baut das Fixture und ist paritätisch zum Interpreter.

### 6.8 Grenzen

* Der gespiegelte Stand am Knoten ist der vom letzten Öffnen im Editor. Ändert jemand das Widget und exportiert, ohne den Aufrufer zu öffnen, nehmen Interpreter und Codegen die gespeicherten Pins; was es nicht mehr gibt, überspringt die Laufzeit mit Warnung (6.4).
* Nur Create Widget. Eingebettete Widgets (WidgetRef) und Listenzeilen (Add Child) bekommen keine Werte pro Instanz; das wäre das Gegenstück zu Unreals „Instance Editable“ im Designer, eigenes Thema.
* Das Folgen eines geänderten Vorgabewerts (6.5) kennt nur den Stand dieser Editor-Sitzung. Nach einem Neustart bleibt ein vorbelegter Pin bei seinem Wert.
