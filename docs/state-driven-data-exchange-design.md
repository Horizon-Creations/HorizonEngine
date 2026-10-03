# State Driven Data Exchange: Pull on Construct und Extract on Destruct

Thema 127, Schritt 2 (Entwurf korrigiert). Nur Doku, keine Umsetzung. Schritt 3 setzt (A) um, Schritt 4 setzt (B) um.

## 0. Auslegung und was verworfen ist

**Gemeint ist** (Antwort des Menschen auf Frage #16, Beiträge 864 und 865 im Thema):

1. **Pull on Construct:** Eine Variable einer Klasse wird beim Initialisieren automatisch von anderswo gezogen. Die Engine sorgt dafür, dass der Wert bei Construct schon da ist. Ist die Quelle nicht verfügbar, gilt ein vorher festgelegter Default. Im Editor legt man pro Variable fest, **welche** Daten gewünscht sind und **woher** sie kommen. Ob dafür Asset-Index oder Registry erweitert werden müssen, prüft §2.10.
2. **Extract on Destruct:** Beim Zerstören einer Klasse werden Daten automatisch in eine Struct extrahiert und über ein bindbares Event **„On Destroyed“** verfügbar gemacht. Die Klasse hat dafür eine hinterlegte Struct und eine Zuordnung Feld → Struct-Member, die auch die C++-Codegen nutzt. Für diese Zuordnung gibt es eine möglichst intuitive Editor-UI.

**Verworfen** ist der erste Entwurf aus Schritt 1 (Commits ab3afee8, fbb9908c): laufende Änderungsmeldung `OnChanged_<Var>` und Dauerbindung „Bind To“ mit Abgleich am Frame-Ende. Das ist ein anderes Feature (fortlaufender Austausch statt Austausch an den beiden Enden des Lebenszyklus). Sein Inhalt steht in der Git-Historie und wird hier nicht weitergeführt. Wiederverwendet werden nur die Ist-Befunde, die für beide Fassungen gelten (§1).

## 1. Ist-Stand

Abkürzungen: **HC** = `src/HE_Core/src/HorizonCode/HorizonCode.cpp` (Header `…/include/HorizonCode/HorizonCode.h` = **HC.h**), **RT** = `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp` (**RT.h**), **CG** = `src/HE_Scene/src/HcCodegen.cpp`, **GS** = `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h`, **CC** = `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h`. Zeilennummern beziehen sich auf den Zweigkopf fbb9908c (= main 9a1cc950 plus Doku).

### 1.1 Lebenszyklus pro Instanzart

Registrieren heißt `Runtime::add` → `addLevels` (RT:58/65, Interpreter) oder `addCompiled` (RT:108, kompiliert). `addLevels` setzt die Defaults wurzelseitig zuerst (RT:77-79, `variableDefaultValue`, nur `scope == 0`). Bei `addCompiled` stehen die Defaults in den generierten Member-Initialisierern (CG:3193-3195, `memberDefault` CG:2702). Danach folgt `bindContext` (RT:131). **Beide feuern nichts.** Jeder Host feuert danach selbst:

| Art | Registriert in | Danach | Events in Reihenfolge |
|---|---|---|---|
| Entity-Klasse | `EntityHost::bind` (`EntityHost.cpp:114/116`) | `setOwnedEntity` (:119) | Construct (:124), BeginPlay (:125) |
| Widget + Embeds | `WidgetManager::createWidget`: Embeds zuerst (`WidgetManager.cpp:327/329`), Theme/Text/Material (:465-474), Host (:493/495) | Ids herauskopieren (:507-512) | PreConstruct Host, Embeds (:525/527), dann Construct Host, Embeds (:528/532). Kein BeginPlay |
| Widget-Zeilen (`addChild`) | `embedWidgetRefs` | — | PreConstruct (:719), Construct (:720) |
| Create Object (keine Entity) | Spiel `GameApplication.cpp:1173-1177` (kompiliert) bzw. :1192-1194, Editor `EditorApplication.cpp:1622-1624` | — | Construct. Kein BeginPlay |
| Create Object (Entity-Klasse) | über `EntityHost::spawn` (`GameApplication.cpp:1158`) | wie Entity | wie Entity |
| Player Controller | `PlayerHost.cpp:111/113` | alle Controller registriert | Construct (:181), BeginPlay (:182) |
| Animator-Sync-Graph | `AnimatorHost.cpp:61/72` | — | Construct (:82) |
| Game Instance | `Runtime::setGameInstance` (RT:225) bzw. `…Compiled` (RT:233) | `GameInstanceHost::fireInit`: `reseedVariables` (`GameInstanceHost.cpp:24`) | OnInit (:26). **Nie Construct**, am Ende OnShutdown statt Destruct |
| Level-Script | `HorizonWorld::fireLevelLoaded` (`HorizonWorld.cpp:681/687`) | — | OnLevelLoaded (:684/690). **Nie Construct** |

**Startreihenfolge** (wichtig für die Frage, welche Quelle bei Construct schon steht):

* Spiel: Game Instance (`GameApplication.cpp:871`) → OnInit (:1253) → Szene laden (:1259 ff.) → Entities (:1316) → Player (:1319) → Animatoren → `startScripts` (:1415) → Level-Script mit OnLevelLoaded (:1419).
* Editor-Play (`setPlayMode` ab `EditorApplication.cpp:9621`): OnInit (:9701) → Entities (:9820) → Player (:9823) → Animatoren (:9830) → Level-Script (:9868). Der GI-Graph ist schon beim Projektladen installiert (:10149/10165), die GI-Instanz existiert also auch außerhalb von Play.

Daraus folgt: **Die Game Instance existiert und hat OnInit hinter sich, bevor irgendeine andere Instanz Construct sieht.** Das Level-Script entsteht dagegen erst **nach** den platzierten Entities.

### 1.2 Kein gemeinsamer Punkt zwischen „Defaults gesetzt“ und „Construct“

Den gibt es nicht. Gemeinsam sind nur:

* die beiden Registrierfunktionen `addLevels`/`addCompiled`. Hier ist alles schon geseedet, und noch läuft kein Nutzercode. Das ist der früheste Punkt, und **jede** Instanzart läuft hindurch, auch Game Instance und Level-Script.
* das Makro `HE_HC_PLAIN_EVENT` (RT:923-931), das Construct, PreConstruct, BeginPlay, OnInit usw. feuert. Es feuert aber mehrere verschiedene Events, und Game Instance und Level-Script rufen nie `fireConstruct`.

**PreConstruct (Thema 119) ist kein Vorbild für den Einhakpunkt, sondern ein Nachbar.** PreConstruct ist eine Phase mit **Nutzercode** und nur für Widgets (`docs/widget-pre-construct-design.md` §2.4: bewusst nicht in der `Object`-Taxonomie, HC:2666 nennt nur Construct/Destruct). Pull on Construct ist eine **Engine-Phase ohne Nutzercode** und gilt für alle Klassenarten. Sie muss deshalb vor PreConstruct laufen, damit PreConstruct die gezogenen Werte schon sieht.

### 1.3 Expose on Spawn liegt nur auf einem Zweig

Thema 119 Schritt 5 („Expose on Spawn“: Create Widget setzt freigegebene Variablen vor PreConstruct, `Variable::exposeOnSpawn`, `Runtime::setPublicVariable`) steht in 515c5786. Der Commit liegt nur auf `origin/claude/widgets-neues-event-pre-construct-fuer-code-vor-erzeugung-re`, **nicht auf main und nicht auf diesem Zweig** (`git merge-base --is-ancestor` geprüft). Dort setzt `createWidget` die Spawn-Werte nach dem Registrieren und vor dem ersten PreConstruct. §2.4 legt die Reihenfolge gegenüber Pull fest. Wer von beiden Zweigen als Zweiter nach main geht, muss sie einhalten.

### 1.4 Zerstörung

Es gibt zwei Ebenen (RT:173-206):

* `Runtime::destroy(id)`: Re-Entrancy-Wächter `m_destructing`, dann **`fireDestruct(id)`**, dann **`remove(id)`**.
* `Runtime::remove(id)`: `m_doomed.insert` (ab hier findet `find()` die Instanz nicht mehr), Listener-Tabellen in beide Richtungen, Delays und angehaltene Läufe weg. Der Speicher wird erst in `purgeDoomed` freigegeben (Anfang von `Runtime::update`, RT:854).

**Die Variablen sind genau bis zum Ende von `fireDestruct` lesbar.** Zwischen `fireDestruct` und `remove` liegt also das Fenster, in dem die Klasse ihr eigenes Destruct schon hinter sich hat und trotzdem noch vollständig da ist.

| Pfad | Stelle | Läuft über `destroy`? |
|---|---|---|
| Destroy Object (Spiel/Editor) | `GameApplication.cpp:1197-1216`, `EditorApplication.cpp:1627-1650` | ja, sofort. Entity noch gültig |
| Entity anders gelöscht (`entity.destroy`, Outliner, `ScriptApi.cpp:102`) | `EntityHost.cpp:238-251` → `unbind` :266-274 | ja, **einen Tick später**. Entity schon ungültig, Variablen lesbar |
| `EntityHost::end` (PIE-Stopp, Shutdown) | `EntityHost.cpp:276-290` | ja |
| Szenenwechsel im Spiel | `GameApplication.cpp:1716/1735` | ja für Entities, aber **nach** dem Freigeben der alten Welt. Create-Object-Instanzen werden dabei gar nicht eingesammelt (kein `clear()`, kein GC) |
| `HorizonWorld::clear` (PIE-Stopp, Szene öffnen im Editor) | `HorizonWorld.cpp:465-511` | Level-Script **nein** (`remove`, :502). Unerreichbare Instanzen ja (`retainOnlyReachableFrom` → `destroy`, RT:243-290) |
| Destroy Widget, `removeChild`, Widget-Clear | `WidgetManager.cpp:1940-1962`, :745-786, :1982-1996 | ja. Bei `removeChild` sind die Elemente vor dem Destruct schon weg |
| Player, Animator | `PlayerHost.cpp:433`, `AnimatorHost.cpp:102/133` | ja |
| Game Instance ersetzen | RT:227/236 | nein (`remove`) |
| `Runtime::clear()` | RT:208 | nein, nichts feuert |

### 1.5 Event-Dispatcher, und warum es kein „On Destroyed“ gibt

* **Bind Event** (RT:604-616, 1177-1183): Der Aufrufer bindet sich als Listener an ein Ziel und einen Eventnamen. `dispatchToListeners` (RT:1188-1218) feuert beim Listener **dessen eigenes Event gleichen Namens** (RT:1214), mit genau **einem** `Value`, Tiefe 32, Budget 256 pro Kaskade. Es gibt keinen Unbind-Knoten. `remove` räumt die Bindungen in beide Richtungen ab.
* Ein `Value` kann eine Struct tragen (`type == Struct`, Felder in `items`, `typeName`; HC.h:93-157).
* Ein `EventDecl` hat **höchstens ein Argument** mit **einem** `typeName` (HC.h:642-649).
* **Ein bindbares „On Destroyed“ gibt es nicht.** Eine Suche nach OnDestroyed, EndPlay, OnDestroy, BeginDestroy findet in HorizonCode nichts. Was es gibt, ist das Selbst-Event **`Destruct`** (HC:2484, CC:145, RT:935). Das Makro verteilt es zwar auch an Listener (`dispatchToListeners(id, "Destruct", {})`, RT:931). Beim Listener läuft dann aber **dessen eigenes Destruct**, also dessen Abbau-Logik, ohne Nutzlast und ohne Hinweis, wer gestorben ist. Als Benachrichtigung ist das unbrauchbar (Nebenbefund §8.1).
* Lua/Python/C++ können nur zerstören (`entity.destroyObject`, `EngineApi.cpp:316`), aber sich nicht benachrichtigen lassen.

### 1.6 Structs

* Asset `.hasset` mit `AssetType::StructType`, Chunk `STDF` (`HAsset.h:153`). C++: `HE::StructDef` / `StructField` (`src/HE_Core/include/Types/TypeRegistry.h:75-119`). Felder haben `name`, `type`, `isArray`, `typeName`, `defaultValue`, Container-Angaben und `formerNames`.
* `HE::TypeRegistry` (TypeRegistry.h:132-197) ist nach Projektpfad geschlüsselt, mit `getStruct`, `makeDefaultValue` und `findField`. `findField` löst erst den aktuellen Namen auf, dann die Aliase aus `formerNames`. Die Umbenennungs-Aliase (Thema 81) sind auf main (6d333f41, f2ad530d geprüft). Im Spiel kommen die Definitionen über den Pak-Eintrag `__type_index__`.
* Eine Variable nennt ihre Struct über `Variable::typeName` (HC.h:441-445). Feld-Defaults pro Graph stehen in `structDefaults` (:446-452).
* Codegen: Jede Struct wird ein natives `struct S_<Name>` mit `toValue`/`fromValue_` (CG:753 ff., 782-790). Welche Structs emittiert werden, sammelt `buildTypeTable` (CG:587-678).

### 1.7 Klassen-Registry, Asset-Index, Referenzen

* **Asset-Index im Pak** (`ProjectExporter.h:297-336`): `__asset_index__` (Pfad → UUID), `__asset_types__` (UUID → Typ), `__type_index__`. **Keine Metadaten pro Klasse.**
* **Editor-Klassenregistry** (`src/HE_Editor/HcEditorUtil.h:103-127`, `.cpp:457-502`): `listClasses` liest alle HC-Klassen- und Widget-Assets und nimmt optional Level- und GI-Graph dazu. Für jede liefert es ein `ClassInfo` mit öffentlichen Funktionen und öffentlichen Instanzvariablen (`access == 0 && scope == 0`), **ohne zu instanziieren**. Lücken:
  * `MemberVar` hat nur `{name, type, className}`. Es fehlen `isArray`, `container`, `keyType` und vor allem **`typeName`** (welche Struct bzw. welches Enum).
  * `classInfoForPath` liest das rohe `graphJson`, also **ohne geerbte Variablen**. `HC::resolveClassAsset` (`HcClassResolve.h:32-67`) könnte flach machen, das nutzt das Kontextmenü von Get/Set (Ref) (`HcGraphHost.cpp:447-466`).
  * Einziger Aufrufer ist der Typ-Picker (`HcEditorUtil.cpp:576-582`), und der rechnet bei offenem Combo jedes Frame neu.
* **Data Tables / Data Assets** gibt es nicht. DataTable, DataAsset und `.hdata` finden nichts.
* **Referenzen:** HC-Graphen referenzieren Assets per inhaltsrelativem Pfad (`className`, `typeName`). Steht ein Pfad im HCGR-JSON, zieht ihn `AssetRefs::retargetBlob` beim Verschieben automatisch nach (`AssetRefRetarget.cpp:32-37`, HCGR steht in `isJsonChunk`). STDF/ENDF/SGTP fehlen dort (dokumentiert in `AssetRefScan.cpp:25-36`): verschachtelte Struct-Pfade **in** einer Struct-Definition werden nicht nachgezogen (Nebenbefund §8.4). Member-Namen haben keine Ids. Umbenennen läuft über `HcRename` bzw. `formerNames`.

## 2. (A) Pull on Construct

### 2.1 Was der Nutzer sieht

Ein HUD-Widget hat `Score : Int` mit Default 0. In der Variablen-Detailansicht hakt man **Pull on Construct** an und wählt *Quelle* „Game Instance“, *Variable* `Score`. Wird das Widget erzeugt, hat `Score` schon in PreConstruct und Construct den Wert aus der Game Instance. Fehlt die Game Instance oder hat sie keine öffentliche Variable `Score` (mehr), bleibt `Score` auf 0, und im Log steht einmal eine Warnung.

Mit Struct-Member: Die Game Instance hält `LastRun : RunStats` (Struct mit `Score`, `Kills`). Das Widget wählt *Variable* `LastRun`, *Member* `Score`.

### 2.2 Datenmodell

An `Variable` (HC.h:409-481), hinter `saveGame`, mit Kommentarblock wie bei Replicated:

```
// Pull on Construct (Thema 127): at registration, before any of the instance's
// own code runs, the runtime copies a value from `pullSource` over the default.
// The default stays the fallback when the source cannot be resolved.
std::string pullSource;   // "" = off | "GameInstance"  (more kinds: §2.3)
std::string pullVar;      // public instance variable of the source
std::string pullMember;   // optional: field of pullVar when that is a Struct
```

* **Ein Feld für „an/aus“ gibt es nicht.** `pullSource` leer heißt aus. So kann es keinen halben Zustand geben (angehakt, aber ohne Quelle).
* **JSON** (`variableToJsonObj` HC:1857 / `variableFromJsonObj` HC:1991): Schlüssel `"pull": {"src": "...", "var": "...", "member": "..."}`, nur geschrieben, wenn gesetzt. Der Loader verwirft ihn bei `scope != 0` (Funktions-locals, wie `repNotify` HC:2005-2006), bei unbekanntem `src` (mit Warnung: ein neueres Projekt in einer älteren Engine) und bei leerem `var`.
* **Der Fallback ist der Default der Variable** (inklusive `structDefaults`). Ein zweiter Wert wäre überflüssig: Gelingt der Pull, sieht niemand den Default, denn vor dem Pull läuft kein Nutzercode. Misslingt er, ist der Default genau der festgelegte Rückfallwert. Im Editor heißt das Default-Feld bei angehaktem Pull deshalb „Fallback“ (§2.9).

### 2.3 Quellen: was heute bei Construct verlässlich da ist

| Quelle | Erster Wurf? | Begründung |
|---|---|---|
| **Game Instance**, öffentliche Instanzvariable, optional ein Struct-Member davon | **ja** | Existiert vor jeder anderen Instanz und hat OnInit hinter sich (§1.1). Im Editor gibt es sie auch außerhalb von Play |
| Level-Script | nein | Entsteht **nach** den platzierten Entities (§1.1). Es wäre für die meisten Ziele nicht da und damit eine Quelle, die meistens den Fallback liefert |
| Ref-Variable der eigenen Klasse | nein | Bei der Registrierung noch null, denn sie hat nur ihren Default. Anders als beim verworfenen „Bind To“ gibt es keinen späteren Abgleich |
| Erzeuger (wer Create Object / Create Widget / Spawn aufgerufen hat) | Ausbau | Fachlich sinnvoll. Die Erzeuger-Id müsste aber durch `ctx.createObject`, `createWidget`, `EntityHost::spawn` und die `Services` gereicht werden, und platzierte Entities haben gar keinen Erzeuger. Expose on Spawn (§1.3) deckt den Fall „der Erzeuger gibt etwas mit“ schon ab, nur als Push |
| Savegame | Ausbau | Heute nur für Entities und nur auf ausdrücklichen Aufruf (`applySavedState`, `EngineApi.cpp:549-601`, nie automatisch). Ein Pull daraus bräuchte einen Schlüssel pro Instanz, den platzierte Entities, Widgets und Objekte nicht haben |
| Data Table / Data Asset | entfällt | Gibt es nicht (§1.7). Wenn sie kommen, sind sie die natürlichste weitere Quelle (statisch, im Editor vollständig bekannt) |
| Datei, HTTP | nein | Asynchron. „Bei Construct vorhanden“ ist damit nicht einzuhalten (gleiche Grenze wie PreConstruct, `widget-pre-construct-design.md` §2.2) |

`pullSource` ist absichtlich ein String und kein Bool: Weitere Quellen kommen ohne Formatwechsel dazu, ältere Engines verwerfen sie laut (§2.2).

**Die Game Instance kann selbst kein Ziel sein.** Sie wäre ihre eigene Quelle, und `fireInit` setzt ihre Variablen ohnehin per `reseedVariables` zurück. Der Editor zeigt die Option im GI-Graph nicht an, die Runtime überspringt Pull für `m_gameInstance`.

### 2.4 Einhakpunkt und Reihenfolge

**Neu: `void Runtime::pullOnConstruct(InstanceId id)`, aufgerufen als letzte Zeile von `addLevels` und `addCompiled`**, also nach dem Seeden bzw. nach `bindContext` und bevor ein Host irgendein Event feuert.

Begründung:

* **Erreicht jede Instanzart**, ohne dass ein Host daran denken muss: Entities, Widgets samt Embeds und Zeilen, Create Object, Player, Animatoren, Level-Script. Ein neuer Host in der Zukunft bekommt Pull automatisch.
* **Vor jedem Nutzercode:** vor PreConstruct, Construct, BeginPlay und OnLevelLoaded. „Bei Construct vorhanden“ gilt damit auch für PreConstruct.
* Nicht im Fire-Makro: Das feuert mehrere Events, bräuchte einen „schon gezogen“-Merker pro Instanz, und Game Instance und Level-Script rufen `fireConstruct` nie.
* Nicht in den Hosts: sieben Stellen (Tabelle §1.1), und jede künftige wäre eine neue Gelegenheit, den Aufruf zu vergessen.

Verbindliche Reihenfolge für eine Instanz:

```
Defaults (addLevels-Seeding bzw. Member-Initialisierer)
→ pullOnConstruct            ← neu, Engine, kein Nutzercode
→ Expose-on-Spawn-Werte      ← nur Widgets, nur Zweig Thema 119 (§1.3)
→ PreConstruct (Widgets) → Construct → BeginPlay (Entities, Player)
```

**Spawn-Werte schlagen Pull.** Was der Erzeuger ausdrücklich an einen Pin hängt, ist die genauere Absicht als die allgemeine Regel der Klasse. Das ergibt sich von selbst, weil `setPublicVariable` auf dem Zweig von Thema 119 nach dem Registrieren läuft, also nach `pullOnConstruct`. Wer den Merge macht, muss nur darauf achten, dass Spawn-Werte nicht vor die Registrierung wandern.

### 2.5 Ablauf von `pullOnConstruct`

```
pullOnConstruct(id):
  if id == m_gameInstance: return
  for each Variable-Deklaration v der Instanz mit v.pullSource != ""
      (Interpreter: flache Ebenen, die abgeleitetste Deklaration gewinnt,
       wie replicatedVariablesOf; kompiliert: varInfos())
      src = resolve(v.pullSource)               // heute nur: m_gameInstance
      if src == 0 or !alive(src): fallback(v, "no Game Instance"); continue
      if !isPublicInstanceVar(src, v.pullVar): fallback(v, "no public variable"); continue
      val = getVariable(src, v.pullVar)
      if v.pullMember != "":
          if val.type != Struct: fallback(v, "not a struct"); continue
          f = TypeRegistry.getStruct(val.typeName)->findField(v.pullMember)  // folgt formerNames
          if !f: fallback(v, "no such member"); continue
          val = val.items[index of f]
      if !shapeMatches(val, v): fallback(v, "type mismatch"); continue
      setVariable(id, v.name, coerce(val, v))   // kompiliert: compiled->setVariable
      mark pulled(id, v.name)
```

* `fallback` schreibt **nichts** (der Default steht schon) und warnt **einmal pro (Klasse, Variable, Grund)** und Sitzung, nicht pro Instanz. Ein HUD, das pro Gegner erzeugt wird, flutet das Log sonst. Kategorie `HorizonCode`, Wortlaut z. B. `Pull on Construct: 'HUD.Score' — Game Instance has no public variable 'Score'; default used`.
* `shapeMatches`: gleicher `PinType`, gleiche Container-Art bzw. `isArray`, bei Struct/Enum gleicher `typeName`, bei Ref verträglicher `className`. Int ↔ Float ist erlaubt (`coerce`). Alles andere ist „type mismatch“. Den Vergleich gibt es im Replikator schon in ähnlicher Form (`valueTypesMatch`, `src/HE_Scene/include/HorizonScene/Net/ValueWire.h:73`). Er liegt aber in HE_Scene, die Runtime in HE_Core. Schritt 3 zieht ihn nach HE_Core (ValueWire leitet weiter) oder schreibt `shapeMatches` dort neu.
* **Kopie, keine Referenz:** `Value` kopiert Container und Structs tief. Ein gezogenes Array ändert die Game Instance nicht mit. Eine gezogene Ref zeigt auf dasselbe Objekt wie in der Quelle, und das ist gewollt.
* `Runtime::pulledVariablesOf(id)` (für Tests und Werkzeuge) liefert, welche Variablen wirklich gezogen und welche auf den Fallback gefallen sind.

### 2.6 Garantien (gehören ins Handbuch)

* **Vor jedem eigenen Code.** PreConstruct, Construct, BeginPlay und OnLevelLoaded sehen den gezogenen Wert bzw. den Fallback, nie einen Zwischenstand.
* **Einmal.** Pull ist ein Schnappschuss beim Erzeugen. Ändert sich die Quelle später, zieht die Variable nicht nach. Wer laufend nachziehen will, macht das wie heute selbst (Get (Ref) auf die Game Instance).
* **Quelle nicht verfügbar → Default**, und zwar genau der Default, der in der Variable steht. Eine Warnung pro Klasse und Variable.
* **Spawn-Werte gewinnen** (§2.4).
* **Nur lokal.** Auf einem Netz-Client zieht die Variable aus der Game Instance **dieses** Rechners. Ist die Variable zusätzlich Replicated, überschreibt der Replikator den Wert später, wie jeden anderen lokalen Wert auch. Die Kombination ist erlaubt. Anders als bei der verworfenen Dauerbindung schreiben hier keine zwei Quellen dauerhaft gegeneinander.

### 2.7 Randfälle

| Fall | Verhalten |
|---|---|
| Widget wird **in OnInit der Game Instance** erzeugt | Pull läuft gegen eine Game Instance, deren OnInit noch nicht fertig ist. Werte, die OnInit vor dem Create Widget setzt, kommen an, spätere nicht. Handbuchsatz |
| Widget-Thumbnails, Designer, App-Vorschau | Erzeugen über `createWidget`. Im Editor existiert die GI-Instanz mit ihren Defaults (§1.1), gezogen wird also deren Default. Das ist harmlos und sogar anschaulich |
| Embeds | werden vor dem Host registriert (§1.1). Sie ziehen ebenfalls aus der Game Instance und brauchen dafür keinen Host |
| `reseedVariables` der Game Instance (Play-Start) | betrifft nur die GI selbst, nicht schon gezogene Werte anderer. Alles, was nach dem Play-Start entsteht, zieht aus den frisch geseedeten Werten |
| Hot Reload | Läuft er als `remove` + `add`, wird neu gezogen. Einen Pfad, der Instanzen an Ort und Stelle ersetzt, prüft Schritt 3. Dort wird **nicht** neu gezogen, der laufende Wert bleibt |
| Save Game an derselben Variable | erlaubt. `applySavedState` läuft nur auf ausdrücklichen Aufruf und damit nach Construct, der gespeicherte Wert gewinnt |
| Ref-Variablen als Ziel | erlaubt (lokal ist eine Instanz-Id ein gültiger Wert) |
| Vererbung | die abgeleitetste Deklaration gewinnt, wie bei Replicated |
| GI-Variable umbenannt oder privat gemacht | zur Laufzeit Fallback + Warnung. Im Editor zeigt die Detailansicht die Quelle rot an (§2.9). Ob `HcRename` die `pullVar` in anderen Assets mitzieht, klärt Schritt 3. Get (Ref)-Knoten auf GI-Variablen haben dasselbe Problem, also dieselbe Lösung |
| Struct-Member der Quelle umbenannt | `findField` folgt `formerNames` (§1.6), der Pull trifft weiter. Der Editor schreibt beim nächsten Speichern den neuen Namen |

### 2.8 Codegen (Parität)

**Die Codegen erzeugt keinen Pull-Code, nur Metadaten.** Der Pull läuft für beide Wege in derselben Runtime-Funktion. Damit ist die Parität eingebaut und muss nicht nachgetestet werden.

* `CompiledVarInfo` (CC:25-43): `const char* pullSource = nullptr; const char* pullVar = nullptr; const char* pullMember = nullptr;` **hinten angehängt und vorbelegt**, damit älter generierte Tabellen weiter kompilieren (gleiche Regel wie bei `saveGame`).
* `hc::VarSlot` / `hc::slot<>` (GS:742-802) und `varInfosOf` (GS:889) um dieselben Felder erweitern.
* Emission der Slot-Tabelle (CG:3295) um die drei Strings erweitern.
* Geschrieben wird über `compiled->setVariable(name, Value)`, das es schon gibt. Ein Mal pro Variable und Construct eine `Value`-Umwandlung kostet nichts Messbares.

Verworfen: nativer Pull-Code (`v_score = gi->v_score;`). Er spart die Umwandlung, bräuchte aber die Quellklasse zur Übersetzungszeit, bricht bei interpretierter Game Instance und verdoppelt die Fallback-Logik.

### 2.9 Editor-UI

Variablen-Detailansicht `LevelScriptPanel::drawVariableDetails` (`src/HE_Editor/LevelScriptPanel.cpp:621-899`), direkt nach „Save Game“ (:798-810). **Dieselbe Änderung in der bewusst doppelten Kopie im Widget-Editor** (`UIEditorPanel.cpp:5432-5442`, Hilfe-Scope „UI Variable“).

```
[x] Pull on Construct
    Source     [ Game Instance          ▾ ]
    Variable   [ LastRun   RunStats     ▾ ]     ← alle öffentlichen GI-Variablen
    Member     [ Score     Int          ▾ ]     ← nur wenn Variable eine Struct ist
    ✓ Game Instance › LastRun › Score (Int)      ← Statuszeile
Fallback   [ 0 ]                               ← das bisherige Default-Feld, umbenannt
```

* **Nur bei `scope == 0`** sichtbar, nicht im GI-Graph (§2.3).
* **Variable-Combo:** listet **alle** öffentlichen Instanzvariablen der Quelle. Unverträgliche stehen ausgegraut mit Grund im Tooltip („Float, needs Int“, „Struct RunStats: pick a member“) statt zu fehlen. Wer eine Variable sucht, sieht dann, warum sie nicht geht. Structs, die ein passendes Member haben, sind wählbar und öffnen das Member-Combo, das nur verträgliche Felder anbietet.
* **Statuszeile:** grün mit Pfad und Typ, oder rot mit dem Grund aus §2.5, z. B. „Game Instance has no public variable ‚Score‘ (renamed?)“. Das ist derselbe Text wie die Laufzeitwarnung, also derselbe Helfer.
* **Knopf „Add to Game Instance“**, wenn die Quelle keine passende Variable hat: Er legt im GI-Graph eine öffentliche Variable mit Name, Typ und Default dieser Variable an und wählt sie aus. Damit ist der häufigste Fall („ich will das global haben“) ein Klick. Ob das Bearbeiten des GI-Graphs aus einem fremden Panel sauber in Undo und Dirty-Markierung passt, prüft Schritt 3. Sonst entfällt der Knopf im ersten Wurf.
* **Kennzeichen in der Variablenliste** wie bei Replicated, mit einem Glyph, den die Editor-Schrift hat (keine Pfeile, siehe Memory „Headless-ImGui-Screenshots“). Tooltip: „Pulled from Game Instance › LastRun › Score“.
* **Hilfe:** Einträge in `src/HE_Editor/EditorHelp.cpp` für „Script Variable/Pull on Construct“, „…/Source“, „…/Variable“, „…/Member“, „…/Fallback“ (Nachbarn :6915-6970) und dieselben unter „UI Variable/“ (:5865-5885). `scripts/editor_help_audit.py --check` läuft in ctest, die BASELINE wird im selben Commit angehoben.

### 2.10 Registry und Asset-Index: was es gibt, was fehlt

**Zur Laufzeit braucht Pull keine Registry und keinen Index.** Die Quelle wird über die lebende GI-Instanz aufgelöst, Variablen nach Namen über `getVariable`/`varInfos`, Struct-Member über `TypeRegistry`. Das gibt es alles. Der Pak-Index (§1.7) bleibt unverändert, denn eine Pull-Angabe ist kein Asset-Verweis, sondern ein Name in der Quellklasse.

**Im Editor fehlt etwas**, und zwar in der Klassenregistry `HcEditorUtil`, nicht im Asset-Index:

1. `MemberVar` um `isArray`, `container`, `keyType`, **`typeName`** erweitern und `classInfoFromGraph` diese Felder füllen lassen. Ohne `typeName` kann das Variable-Combo eine Struct nicht von einer anderen unterscheiden und kein Member-Combo öffnen.
2. Geerbte Variablen: Für die Game Instance ist das heute egal (der GI-Graph kommt direkt als `giGraph` in `listClasses`). Sobald weitere Quellklassen dazukommen (Erzeuger, §2.3), muss `classInfoForPath` über `HC::resolveClassAsset` flach machen. Erst dann, nicht in Schritt 3.
3. Eine kleine **Zwischenablage pro Frame bzw. pro Graph-Version** für die GI-ClassInfo, damit die Detailansicht nicht jedes Frame den GI-Graph durchgeht. Der Typ-Picker hat dasselbe Problem schon (§1.7), die Lösung kann er mitbenutzen.

Für Extract on Destruct (§3) gilt dasselbe: Der Struct-Pfad steht im HCGR-JSON und wird beim Verschieben des Struct-Assets automatisch nachgezogen. Neue Index-Einträge sind nicht nötig.

## 3. (B) Extract on Destruct

### 3.1 Was der Nutzer sieht

Die Klasse `Enemy` hat `Health`, `Kills`, `LootTier`. In ihrem Klassen-Panel gibt es einen Abschnitt **Extract on Destruct**. Dort wählt man die Struct `EnemyReport` (Felder `Kills : Int`, `Tier : Int`, `Who : Ref`) und ordnet zu: `Kills ← Kills`, `Tier ← LootTier`, `Who ← Self`. Stirbt ein Gegner, baut die Engine nach seinem eigenen Destruct eine `EnemyReport` und schickt sie über **OnDestroyed** an alle, die sich per Bind Event an diesen Gegner gebunden haben. Der Spawner hat in seinem Graph ein Event `OnDestroyed (EnemyReport)` und zählt dort die Kills zusammen.

### 3.2 Datenmodell

Am **Graph** (Klassenebene, nicht an einer Variable), gespeichert im HCGR-JSON:

```
struct ExtractMapEntry { std::string member; std::string var; };   // var: Variablenname | "@Self"
struct ExtractSpec     { std::string structPath; std::vector<ExtractMapEntry> map; };
// Graph::extract — leer = die Klasse extrahiert nichts
```

JSON: `"extract": {"struct": "Types/EnemyReport.hasset", "map": [{"member": "Kills", "var": "Kills"}, {"member": "Who", "var": "@Self"}]}`, nur geschrieben, wenn `struct` gesetzt ist.

* **„Markierte Felder“ sind genau die Einträge der Zuordnung.** Es gibt kein zusätzliches Häkchen an der Variable, das mit der Tabelle auseinanderlaufen könnte. Die Variablen-Detailansicht zeigt die Zuordnung nur an (§3.8).
* **Quellen eines Eintrags:** jede Instanzvariable der Klasse (`scope == 0`, auch private, auch geerbte, denn die Klasse gibt ihre eigenen Daten heraus). Dazu die Pseudo-Quelle **`@Self`** (Ref auf die sterbende Instanz). Mit ihr kann ein Listener, der an mehreren Gegnern hängt, sie unterscheiden (§1.5: das Event trägt sonst keinen Absender). Die Id zeigt nach dem Tod ins Leere, `alive()` sagt dann false. Als Schlüssel taugt sie, als Zugriff nicht.
* **Nicht zugeordnete Member** bekommen den Default der Struct (`TypeRegistry::makeDefaultValue`).
* **Vererbung:** Die abgeleitetste Ebene mit gesetztem `extract` gewinnt **ganz**, es wird nicht gemischt. Eine Kindklasse, die eine andere Struct will, ersetzt die Angabe. Ist sie bei der Kindklasse leer, gilt die der Elternklasse. Das ist dieselbe Regel wie bei Variablen, nur auf Klassenebene.
* **Member-Namen werden über `findField` aufgelöst**, Umbenennungen in der Struct (`formerNames`) treffen also weiter. Der Editor schreibt beim nächsten Speichern den aktuellen Namen.

### 3.3 Das bindbare Event „OnDestroyed“ (neu)

* **Name `OnDestroyed`, nur an Listener verteilt, nie beim Besitzer selbst gefeuert.** Damit ist ein `OnDestroyed`-Event im Graph eindeutig „ein Objekt, an das ich gebunden bin, ist gestorben“. Für „ich sterbe“ gibt es weiter `Destruct`.
* **Nutzlast:** die extrahierte Struct, falls die Klasse eine hat, sonst ein leeres `Value`. **OnDestroyed geht für jede über `destroy` zerstörte Instanz raus**, auch ohne Extract. Ein reines „ist tot“ ist für sich schon nützlich (Spawner zählt Lebende), und das Event „für Objekte“ ist so vom Menschen gewünscht.
* **Bindung:** mit dem vorhandenen Knoten Bind Event (Ziel = das Objekt, Event = `OnDestroyed`). Neue Knoten gibt es keine.
* **Kein Engine-Event-Tabelleneintrag.** Die Tabelle (HC:2472 ff.) legt pro Event einen festen Pin-Typ fest, die Struct ist hier aber je nach Quellklasse eine andere. `OnDestroyed` ist deshalb beim Listener ein **gewöhnliches Event mit einem Struct-Argument**, dessen `typeName` der Listener festlegt (EventDecl HC.h:642-649 trägt genau das). Der Editor legt es per Knopf richtig typisiert an (§3.8).
* **Typprüfung beim Verteilen:** Neuer Helfer `dispatchDestroyed(owner, payload)` statt des allgemeinen `dispatchToListeners`. Pro Listener schaut er dessen `EventDecl` für `OnDestroyed` an:
  * ohne Argument → feuern, Nutzlast fällt weg,
  * Struct mit gleichem `typeName` → feuern,
  * anderer Typ → **nicht** feuern, einmal warnen pro (Listener-Klasse, Struct). Ein stilles `coerce` einer fremden Struct ergäbe Unsinnswerte.
* Tiefe und Budget wie beim Dispatcher (32 / 256). Ein Listener darf in `OnDestroyed` erzeugen, zerstören und binden. Die Listener-Liste wird vorher kopiert, wie heute (RT:1197).

**Bestehende Nutzerprojekte** mit einem eigenen Custom Event `OnDestroyed`, das sie selbst per Emit Event schicken, bekommen ab dann zusätzlich das Engine-Event. Wie bei PreConstruct (`widget-pre-construct-design.md` §2.5): eine Warnung beim Laden, wenn ein Graph `OnDestroyed` per `EmitEvent` aussendet, kein automatisches Umbenennen.

### 3.4 Einhakpunkt

In `Runtime::destroy` (RT:195-206), zwischen den beiden vorhandenen Zeilen:

```
destroy(id):
  if !find(id): return
  if !m_destructing.insert(id).second: return
  fireDestruct(id)                    // eigenes Destruct: letzte Werte setzen
  if !find(id): (m_destructing.erase(id); return)   // Destruct hat sich schon über remove erledigt
  payload = buildExtract(id)          // neu: Struct oder leeres Value
  dispatchDestroyed(id, payload)      // neu: an Listener, Objekt noch registriert
  remove(id)
  m_destructing.erase(id)
```

* **Nach Destruct**, damit die Klasse in ihrem eigenen Destruct noch Endwerte ausrechnen kann (Spielzeit, Endpunktestand), die dann in der Struct landen.
* **Vor `remove`**, damit die Variablen lesbar sind und Listener in `OnDestroyed` per Get (Ref) über `@Self` noch zusätzlich am sterbenden Objekt lesen können. Das ist das Fenster aus §1.4.
* **Der vorhandene Destruct-Dispatch an Listener** (RT:931) bleibt in den Schritten 3/4 unverändert, um niemandem Verhalten wegzunehmen. Siehe §8.1 und offene Frage 3.

`buildExtract`:
* Interpreter: `TypeRegistry::makeDefaultValue(structPath)`, dann pro Eintrag `findField(member)` → `items[i] = coerce(getVariable(id, var), feldtyp)` bzw. `Value::ofRef(id)` für `@Self`. Ein unpassender Eintrag (Variable weg, Typ geändert) wird übersprungen, das Member behält seinen Default, und es gibt eine Warnung einmal pro (Klasse, Member).
* Kompiliert: neuer virtueller Hook in `CompiledInstance` (CC:137-149, neben `onDestruct`): `virtual bool extractOnDestruct(Value& out) const { return false; }`. Die Codegen überschreibt ihn (§3.7).
* Struct nicht registriert (Asset gelöscht): leeres `Value`, Warnung einmal pro Klasse. OnDestroyed geht trotzdem raus.

### 3.5 Welche Zerstörungen extrahieren

Alles, was über `Runtime::destroy` läuft (Tabelle §1.4): Destroy Object, Destroy Widget, `removeChild`, Widget-Clear, Entity-Löschen (einen Tick später), `EntityHost::end`, Player, Animator, GC beim Szenenwechsel im Editor.

**Nicht** extrahiert wird, was nur über `remove` oder `clear` geht. Das ist eine bewusste Grenze, sie gehört so ins Handbuch:

* **`Runtime::clear()`** (Programmende, Runtime-Reset): Es feuert schon heute kein Destruct, also auch kein Extract. Am Programmende gibt es niemanden mehr, der zuhört.
* **Level-Script** (`HorizonWorld::clear` ruft `remove`, `HorizonWorld.cpp:502`): Es hat kein Destruct, sondern OnLevelUnloaded. Extract on Destruct ist für Level-Scripts im Editor ausgegraut.
* **Game Instance:** hat OnShutdown statt Destruct und ist die Wurzel aller Daten. Ebenfalls ausgegraut.
* **Create-Object-Instanzen beim Szenenwechsel im Spiel** werden heute gar nicht eingesammelt (§1.4, Nebenbefund §8.2). Wird das behoben, extrahieren sie automatisch mit.

### 3.6 Grenze: ein Listener, mehrere Struct-Typen

Ein Listener-Graph hat genau **ein** Event `OnDestroyed` mit **einem** `typeName` (§1.5). Bindet sich ein Spawner an Gegner (`EnemyReport`) **und** an Kisten (`CrateReport`), kann er nicht beides typisiert empfangen. Die nicht passende Sorte wird nach §3.3 mit Warnung übersprungen.

Für den ersten Wurf gilt: **Wer verschiedene Klassen beobachtet, gibt ihnen dieselbe Struct**, oder er hängt sein `OnDestroyed` ohne Argument an und liest per `@Self` nach (dann ohne Nutzlast). Zwei echte Auswege stehen als offene Frage 2 da. Beide ändern den Dispatcher und sind deshalb nicht Schritt 4.

### 3.7 Codegen

* `buildTypeTable` (CG:587-678) nimmt `graph.extract.structPath` in die zu emittierenden Structs auf. Heute sammelt sie nur aus Knoten und Variablen.
* Pro Klasse mit `extract` emittiert die Codegen:

```cpp
bool extractOnDestruct(hc::Value& out) const override
{
    S_EnemyReport s{};                 // Struct-Defaults aus den Member-Initialisierern
    s.Kills = v_Kills;
    s.Tier  = static_cast<int32_t>(v_LootTier);   // nur wo coerce es auch täte
    s.Who   = hc::self(m_ctx);         // @Self
    out = toValue(s);
    return true;
}
```

  Die Zuordnung ist zur Übersetzungszeit vollständig bekannt. Daher kommt sie nativ und typsicher heraus, wie vom Menschen gewünscht („für die C++-Codegen“). Ein ungültiger Eintrag wird **beim Generieren** übersprungen, mit derselben Warnung wie im Interpreter, statt nicht kompilierbaren Code zu erzeugen.
* `@Self` wird zu demselben Ausdruck, den die Codegen heute für den Knoten Get Self erzeugt (`hc::self(m_ctx)`, CG:1805).
* Die Paritäts-Suite vergleicht `buildExtract` (Interpreter) und `extractOnDestruct` (kompiliert) feldweise mit `valuesEqual` (§6).

### 3.8 Editor-UI für die Zuordnung

**Ort:** ein neuer Abschnitt in der linken Klassenleiste von `LevelScriptPanel` (heute „Variables“ :351, „Events“ :443, „Functions“ :479, alle über `EditorWidgets::sectionHeaderAdd`). Neu darunter **„Extract on Destruct“** mit genau einem Eintrag. Ein Klick öffnet rechts die Detailansicht, so wie ein Klick auf eine Variable deren Details öffnet. Gleiches im Widget-Editor (`UIEditorPanel`). Im Level-Script- und GI-Graph ist der Abschnitt ausgegraut, mit Begründung im Tooltip (§3.5).

```
Extract on Destruct
  Struct  [ EnemyReport ▾ ]   [ New Struct from Variables… ]

  Member        Type     From
  ─────────────────────────────────────────────
  Kills         Int      [ Kills          ▾ ]   ✓
  Tier          Int      [ LootTier       ▾ ]   ✓
  Who           Ref      [ Self           ▾ ]   ✓
  Bonus         Float    [ — default 0.0  ▾ ]         ← grau: Struct-Default
  ─────────────────────────────────────────────
  [ Auto-Map by Name ]  [ Clear ]

  Listeners receive this as  OnDestroyed (EnemyReport)
```

* **Zeilen kommen aus der Struct**, nicht aus der Klasse. Die Struct bestimmt, was herauskommt. Pro Member gibt es eine Zeile, und das ist auch die Form, die die Codegen braucht. Vorlage: `drawStructDefaultEditor` (`HcEditorUtil.cpp:1414 ff.`, eine Zeile pro Feld, nach Feldname geschlüsselt). Eine fertige Zwei-Spalten-Zuordnung gibt es im Editor noch nicht.
* **„From“-Combo** pro Zeile: zuerst „— default ‹Wert›“, dann „Self“ (nur bei Ref-Membern), dann alle Instanzvariablen der Klasse, geerbte mit Elternname. Unverträgliche stehen ausgegraut mit Grund im Tooltip, wie in §2.9.
* **Struct wählen füllt sofort vor:** Gleicher Name und verträglicher Typ werden zugeordnet (dasselbe wie „Auto-Map by Name“). Der Nutzer korrigiert nur noch Abweichungen wie `Tier ← LootTier`, statt jede Zeile von Hand zu setzen. Das Vorbefüllen ist sichtbar und rückgängig zu machen (ein Undo-Schritt), nicht heimlich.
* **Ziehen und Ablegen:** Eine Variable aus der Variablenliste auf eine Zeile ziehen ordnet sie zu. Das ist dieselbe Geste, mit der man Variablen heute in den Graph zieht.
* **„New Struct from Variables…“** für den Fall „ich habe noch keine Struct“: ein kleines Popup mit einer Häkchenliste aller Instanzvariablen der Klasse (**das** sind die „markierten Felder“ aus der Aufgabe), einem Namen und einem Ordner. Es legt das Struct-Asset an (Feldname, Typ und Default aus der Variable, über den Weg von `ContentBrowserPanel.cpp:2439` / `AssetStubWriter.cpp:107-110`), registriert es und füllt die Zuordnung 1:1. Damit ist der häufigste Einstieg zwei Klicks und kein Wechsel in den Struct-Editor.
* **Kaputte Zeilen** sind rot mit Grund: Variable gelöscht, Typ geändert, Member in der Struct entfernt. Ein Member, das nur noch in der Zuordnung steht, aber nicht mehr in der Struct, steht unten als rote Zeile „removed from struct“ mit Löschknopf, damit nichts still verschwindet.
* **Rückmeldung in die andere Richtung:**
  * In der Variablenliste bekommt jede zugeordnete Variable ein Kennzeichen (Glyph aus der vorhandenen Schrift).
  * Die Variablen-Detailansicht zeigt eine Zeile „Extracted to EnemyReport.Kills“ mit Sprung zur Tabelle.
  * Wird eine Variable umbenannt, zieht die Zuordnung im selben Graph mit (dieselbe Stelle wie die übrigen Umbenennungen über `HcRename`, `HcRename.h:7-30`).
* **Listener-Seite:** Wählt man am Knoten Bind Event ein Ziel, dessen Klasse bekannt ist, bietet das Event-Combo `OnDestroyed (EnemyReport)` an. Ein Knopf **„Create OnDestroyed Event“** legt im Listener-Graph das Event mit genau diesem Struct-Argument an. Das ist das Muster, mit dem „Notify“ heute `OnRep_<Var>` anlegt (`LevelScriptPanel.cpp:763-793`). Hat der Listener schon ein `OnDestroyed` mit anderem Typ, sagt der Knopf das, statt ein zweites anzulegen (§3.6).
* **Hilfe:** EditorHelp-Einträge für „Extract on Destruct“, „Struct“, „From“, „Auto-Map by Name“, „New Struct from Variables…“, „Create OnDestroyed Event“, BASELINE im selben Commit.

### 3.9 Randfälle

| Fall | Verhalten |
|---|---|
| Destruct-Handler ist latent (Delay) | Der synchrone Teil läuft vor dem Extract, der geparkte Rest stirbt mit `remove` (RT:188), wie heute |
| Destruct zerstört die Instanz selbst noch einmal | Re-Entrancy-Wächter wie heute, ein Extract, ein OnDestroyed |
| Listener zerstört in OnDestroyed weitere Objekte | eigene `destroy`-Aufrufe mit eigenem Extract. Tiefe/Budget begrenzen Kaskaden |
| Entity gelöscht ohne Destroy Object | Extract einen Tick später, Variablen sind dann noch richtig. Was am Entity hängt (Transform, Komponenten), ist nicht zuordenbar und wäre ohnehin ungültig |
| Netz | rein lokal wie Destruct. Jede Maschine extrahiert für ihre eigene Instanz. Wer es übers Netz braucht, nimmt RPC/Replicated |
| Struct mit Containern oder verschachtelten Structs | Member werden als ganze Werte kopiert, tief. Verschachtelt zuordnen (`Stats.Kills ← Kills`) gibt es im ersten Wurf nicht |
| Widget per `removeChild` | Extract sieht die Variablen, aber die Elemente sind schon weg (§1.4). Zuordnen lassen sich nur Variablen, also kein Problem |

## 4. Zusammenspiel: Daten über den Tod hinaus

Der naheliegende Gebrauch beider Hälften zusammen ist „Spieler stirbt, der nächste Spieler fängt mit seinen Werten an“. Mit dem, was oben steht, geht das **ohne weiteres Feature** über die Game Instance:

1. `Player` extrahiert `RunStats` (`Score ← Score`, `Kills ← Kills`).
2. Die Game Instance hat `LastRun : RunStats` und ein Event `OnDestroyed (RunStats)`, das `LastRun` setzt. Sie bindet sich an jeden neuen Player. Dafür ruft der Player in BeginPlay eine öffentliche GI-Funktion `RegisterPlayer(Self)`, die ihrerseits Bind Event ausführt. Bind Event bindet immer den Aufrufer.
3. Der nächste `Player` zieht `Score` per Pull on Construct aus `Game Instance › LastRun › Score`.

Schritt 2 ist Kleberei, die jeder Nutzer selbst schreiben muss. Ob es dafür eine deklarative Abkürzung geben soll, ist offene Frage 1. Im ersten Wurf ist sie absichtlich nicht drin.

## 5. Umsetzungs-Checklisten

### 5.1 Schritt 3: Pull on Construct

Datenmodell und Persistenz:
1. `Variable` (HC.h:409-481): `pullSource`, `pullVar`, `pullMember` mit Kommentar (§2.2).
2. JSON speichern/laden (HC:1857 ff. / HC:1991 ff.): Schlüssel `pull`, Verwerfen nach §2.2.

Runtime (HE_Core):
3. `Runtime::pullOnConstruct(id)` nach §2.5, aufgerufen als letzte Zeile von `addLevels` (RT:65 ff.) und `addCompiled` (RT:108 ff.). Überspringt `m_gameInstance`.
4. Warnung einmal pro (Klasse, Variable, Grund), Helfer für den Grundtext, den auch der Editor benutzt.
5. `shapeMatches` (ggf. mit dem Replikator teilen), `Runtime::pulledVariablesOf(id)`.

Codegen:
6. `CompiledVarInfo` (CC:25-43) drei Felder hinten angehängt und vorbelegt, `hc::slot<>`/`VarSlot`/`varInfosOf` (GS:742-802, 889), Emission CG:3295.

Editor und Doku:
7. `HcEditorUtil`: `MemberVar` um `isArray`/`container`/`keyType`/`typeName` erweitern, Zwischenablage für die GI-ClassInfo (§2.10).
8. `drawVariableDetails` (LevelScriptPanel.cpp nach :810) und die Kopie in `UIEditorPanel.cpp:5432 ff.`: UI nach §2.9, Kennzeichen in der Variablenliste.
9. EditorHelp-Einträge in beiden Scopes, Audit-BASELINE, In-Engine-Handbuch (Quelle `Website/HorizonEngineDocs/*.html`, danach `scripts/build_docs_bundle.py`, `he-docs.json` committen, Website-Deploy nur nach Rückfrage), `docs/horizoncode-reference.md`.
10. Wenn der Zweig von Thema 119 (Expose on Spawn) vorher auf main landet: Reihenfolge §2.4 im Test festhalten.

### 5.2 Schritt 4: Extract on Destruct

Datenmodell und Persistenz:
1. `ExtractSpec`/`ExtractMapEntry` an `Graph`, JSON `extract` (nur wenn gesetzt), Laden mit `findField`-Auflösung der Member.
2. Lade-Warnung bei Graphen, die selbst ein `OnDestroyed` per `EmitEvent` schicken (§3.3).

Runtime:
3. `Runtime::destroy` nach §3.4: `buildExtract`, `dispatchDestroyed` mit Typprüfung nach §3.3.
4. `CompiledInstance::extractOnDestruct(Value&)` (CC:137-149), Standard `false`.
5. Flache Auflösung der `extract`-Angabe über die Ebenen (die abgeleitetste gewinnt ganz).

Codegen:
6. `buildTypeTable` nimmt die Extract-Struct auf, Emission von `extractOnDestruct` nach §3.7, ungültige Einträge beim Generieren mit Warnung überspringen.

Editor und Doku:
7. Abschnitt „Extract on Destruct“ in `LevelScriptPanel` und `UIEditorPanel` mit Tabelle nach §3.8, Vorbefüllen, Ziehen, „New Struct from Variables…“, rote Zeilen.
8. Bind Event: `OnDestroyed (Struct)` im Event-Combo, Knopf „Create OnDestroyed Event“.
9. Variablenliste: Kennzeichen, Detailzeile „Extracted to …“, Umbenennen zieht die Zuordnung mit.
10. EditorHelp, Audit-BASELINE, Handbuch, `docs/horizoncode-reference.md` (Garantien §3.3-§3.5 sinngemäß, Grenze §3.6).

## 6. Tests (he_tests, Interpreter **und** kompiliert)

Fixtures für beide Hälften in `HCGEN_CLASSES` aufnehmen, damit die Paritäts-Suite beide Wege vergleicht.

Pull on Construct:
* GI hat `Score = 7` → neue Instanz mit Pull hat in Construct 7 (Probe in Construct schreibt den Wert in eine zweite Variable). **Negativkontrolle:** ohne Pull-Angabe 0.
* Widget: PreConstruct sieht schon den gezogenen Wert.
* Struct-Member-Pull (`LastRun.Score`), Member umbenannt mit `formerNames` → trifft weiter.
* Fallback: keine GI, GI-Variable privat, fehlt, Typ falsch → Default, **genau eine** Warnung für zehn Instanzen derselben Klasse.
* Int ↔ Float wird umgewandelt, Array gegen Skalar fällt zurück.
* Kopie: Array ziehen, danach GI-Array ändern → gezogene Kopie unverändert.
* Level-Script und Entity, Player, Create Object ziehen alle (jede Host-Art einmal, weil der Einhakpunkt in der Runtime sitzt, reicht je ein kurzer Fall).
* GI-Graph mit Pull-Angabe → wird ignoriert, kein Absturz.
* JSON-Rundreise, Loader verwirft bei Funktions-locals und unbekannter Quelle.
* Codegen: alte `CompiledVarInfo`-Aggregate kompilieren weiter. Kompilierte Klasse zieht genauso wie interpretierte.
* Falls Thema 119 Schritt 5 schon gemergt ist: Spawn-Pin schlägt Pull.

Extract on Destruct:
* Destroy Object → Listener bekommt `OnDestroyed (S)` mit richtigen Feldern, auch dem Wert, den das eigene Destruct **zuletzt** gesetzt hat. **Negativkontrolle:** Extract vor Destruct ergäbe den alten Wert.
* `@Self` trägt die Id der sterbenden Instanz. Im Handler ist sie noch lesbar (Get (Ref)), nach dem Frame `alive() == false`.
* Nicht zugeordnete Member = Struct-Default. Variable gelöscht → Member Default + eine Warnung.
* Ohne Extract: `OnDestroyed` ohne Nutzlast kommt trotzdem an.
* Listener mit falschem Struct-Typ → kein Aufruf, eine Warnung. Listener ohne Argument → Aufruf.
* Besitzer selbst bekommt kein `OnDestroyed`.
* Pfade: Destroy Widget, `removeChild`, Entity-Löschen mit Verzögerung um einen Tick, GC beim Editor-Szenenwechsel → je ein Extract. `Runtime::clear()` → keins.
* Re-Entrancy: Destruct zerstört sich selbst → ein Extract. Listener zerstört in OnDestroyed die Quelle erneut → kein zweiter.
* Interpreter gegen kompiliert: `buildExtract` und `extractOnDestruct` feldweise gleich.
* Vererbung: Kindklasse ohne eigene Angabe nimmt die der Eltern, mit eigener ersetzt sie sie ganz.
* JSON-Rundreise, Member-Umbenennung über `formerNames`.
* ASan- bzw. gmalloc-Lauf für die Zerstörungs-Kaskaden wie bei den Net-Session-Tests.

## 7. Bewusst nicht in den Schritten 3/4

* Weitere Pull-Quellen (Erzeuger, Savegame, Data Tables), §2.3.
* Laufendes Nachziehen nach Construct. Das war der verworfene erste Entwurf.
* Verschachtelte Zuordnung (`Stats.Kills ← Kills`) und Konstanten als Quelle in der Extract-Tabelle.
* Extract für Level-Script und Game Instance (§3.5).
* Lua/Python/C++-Zugriff auf OnDestroyed bzw. Pull-Angaben. Diese Frontends erreichen HC-Variablen heute kaum (erster Entwurf §1), das ist ein eigenes Loch.
* Ein Speicher „letzter Extrakt pro Klasse“ in der Runtime (offene Frage 1).

## 8. Nebenbefunde (nicht Teil dieses Features)

1. **Destruct wird an Listener verteilt und lässt dort deren eigenes Destruct laufen** (RT:931 im Makro, RT:1214). Wer Bind Event auf `Destruct` eines anderen Objekts setzt, bekommt keine Benachrichtigung, sondern führt seine eigene Abbau-Logik aus. Dasselbe Makro verteilt auch Construct, BeginPlay, OnInit usw. Mit `OnDestroyed` gibt es dafür das richtige Werkzeug. Ob der Listener-Dispatch der Lebenszyklus-Events wegfallen oder warnen soll, ist offene Frage 3.
2. **Szenenwechsel im Spiel sammelt Create-Object-Instanzen nicht ein** (`swapToWorld`, `GameApplication.cpp:1716/1735`, kein `clear()`/GC). Im Editor passiert es über `HorizonWorld::clear` (`HorizonWorld.cpp:508`). Zudem fällt das Destruct der Entities dort erst nach dem Freigeben der alten Welt.
3. **Der GC markiert keine Refs innerhalb von Structs** (GS:853). Eine Instanz, die nur über ein Struct-Feld erreichbar ist, wird beim Editor-Szenenwechsel eingesammelt.
4. **Retarget lässt STDF/ENDF/SGTP aus** (§1.7). Verschiebt man eine Struct, die in einer anderen Struct als Feldtyp steht, bleibt der alte Pfad in der äußeren stehen.
5. **`MemberVar` ohne `typeName`** (§1.7) betrifft heute schon das Get/Set-(Ref)-Umfeld. Das Kontextmenü setzt am GetExternal-Knoten nur `propType`, nicht `typeName` (`HcGraphHost.cpp:1320-1352`).

## 9. Offene Fragen an das Review

1. **Wohin mit extrahierten Daten, wenn gerade niemand zuhört?** Im Entwurf gehen sie nur an gebundene Listener, der Weg über die Game Instance (§4) braucht Kleberei. Variante a: deklarativ „Extract also into Game Instance variable X“ an der Extract-Angabe. Variante b: ein Runtime-Speicher „letzter Extrakt der Klasse K“, der zugleich eine weitere Pull-Quelle wäre. Variante c: nichts, §4 reicht.
2. **Mehrere Struct-Typen an einem Listener** (§3.6): (a) Dispatcher-Event zusätzlich unter dem Namen `OnDestroyed_<Struct>` verteilen, sodass ein Listener mehrere typisierte Events haben kann, oder (b) Bind Event bekommt einen Zielnamen für das Event. Beides ändert den Dispatcher. Oder (c) die Grenze stehen lassen.
3. **Lebenszyklus-Events an Listener** (§8.1): abschaffen, warnen, oder lassen?
4. **„Add to Game Instance“-Knopf** (§2.9) im ersten Wurf, oder erst, wenn das Bearbeiten des GI-Graphs aus fremden Panels sauber geklärt ist?
5. **Quelle Erzeuger** (§2.3) gleich mit, oder reicht Game Instance plus Expose on Spawn für den ersten Wurf?
