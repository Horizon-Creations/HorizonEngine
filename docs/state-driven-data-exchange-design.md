# State Driven Data Exchange: Analyse und Design

Thema 127, Schritt 1 (Analyse und Design). Noch keine Umsetzung. Was hier steht, ist der Vorschlag für Schritt 2 ff. und wird vorher reviewt.

## 0. Auslegung des Themas (bitte im Review bestätigen)

Das Thema kam ohne Beschreibung, im Repo, auf der Roadmap und im Handbuch kommt der Begriff nicht vor. Dieser Entwurf liest es so:

> **Werte fließen zustandsgetrieben zwischen HorizonCode-Instanzen.** Ändert sich eine Variable, sorgt die Engine dafür, dass es die Interessierten erfahren bzw. dass gebundene Variablen anderer Klassen den neuen Wert bekommen, ohne dass jemand jedes Frame nachfragt.

Das ersetzt das heute einzige Muster für „Klasse B zeigt, was Klasse A hat“: **Event Tick → Get (Ref) → vergleichen/kopieren**, in jeder Konsumentenklasse von Hand und jedes Frame.

Nicht gewählt, aber als Ausbau in §9 notiert: ein eigenes geteiltes Zustands-Asset (Blackboard / „State Store“), an das sich Klassen hängen. Das wäre Austausch zwischen Klassen und einem Speicher, nicht zwischen HC-Klassen. Die Game Instance erfüllt diese Rolle heute schon und ist in diesem Entwurf eine vollwertige Quelle.

## 1. Ist-Stand: wie HC-Instanzen heute Daten austauschen

Abkürzungen: **HC** = `src/HE_Core/{include,src}/HorizonCode/HorizonCode.{h,cpp}`, **RT** = `…/HorizonCodeRuntime.{h,cpp}`, **CG** = `src/HE_Scene/src/HcCodegen.cpp`, **GS** = `src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h`.

Alles ist **explizit**: entweder Push (Set, Call, Emit) oder Pull (Get). Es gibt keinen Beobachter, kein OnChanged, keine Bindung, keine Interfaces, kein Get-All-Of-Class.

| Mechanismus | Richtung | Kern (Interpreter / Runtime / Codegen) | Wie das Ziel gefunden wird |
|---|---|---|---|
| Get/Set (Ref) — `GetExternal`/`SetExternal` | Pull / Push, nur `access==0 && scope==0` | HC.cpp:4480 / HC.cpp:3698 → `ctx.setExternal` RT.cpp:644-660; CG:1754-1795, CG:2389-2424 | Ref aus Variable, Get Self, Get Game Instance, Create Object, Cast, `entity.instance` u. a. |
| Call (Ref) — `CallExternal` | Push, synchron, mit Rückgabe | HC.cpp:3711 → `Runtime::callFunction(requirePublic)` RT.cpp:1257; CG:2439-2519 | Ref |
| Event-Dispatcher — `BindEvent`/`EmitEvent` | Push, Broadcast, ein `Value` | RT.cpp:1171-1188 (`dispatchToListeners`, Tiefe 32, Budget 256); CG:2425-2438 | Listener bindet sich an eine Ref; sein eigenes Event gleichen Namens feuert |
| Cast | Typprüfung, trägt keine Daten | HC.cpp:3624, `instanceIsA` RT.cpp:134; CG:2231 | — |
| Game Instance | de-facto globaler Speicher, GC-Wurzel | `ctx.getGameInstance` RT.cpp:670 | `GetGameInstance` |
| Replikation (RepNotify) | **einzige Änderungserkennung**, maschinenübergreifend | `PropertyReplicator::update` (`src/HE_Scene/src/Net/PropertyReplicator.cpp:359-448`) **pollt** und vergleicht mit Schattenkopie (`valuesEqual`, Z. 432); Client ruft `OnRep_<Var>(Old)` (`NetEvents.h:132-150`) | gleiche Entity auf anderer Maschine |
| Savegame | Pull, explizit | `Runtime::savedVariablesOf` RT.cpp:394, `entity.saveState/applySavedState` | Entity |
| AnimatorHost | pollt Sync-Graph-Variablen jedes Frame | `src/HE_Scene/src/AnimatorHost.cpp:127` | — |
| Lua/Python/C++ | erreichen HC-Variablen nur über `net.*Var`, OnRep, RPC, `widget.callFunction` | `EngineApi.cpp:3421/3435`, `ScriptApi.cpp:275` | — |

Zwei Beobachtungen tragen den Entwurf:

1. **Änderungserkennung durch Abgleich (Poll + Schattenkopie) ist im Haus schon das bewährte Muster** (Replikator, AnimatorHost) und hat eine definierte Semantik (mehrere Schreibvorgänge in einem Frame = eine Meldung; Schreiben und Zurücksetzen im selben Frame = keine Meldung).
2. **Die Variablen-Häkchen sind die Deklaration.** `Replicated`, `Notify` und `Save Game` sind Flags an `Variable` (HC.h:409-478) ohne einen einzigen Knoten. Das Häkchen „Notify“ legt die Handler-Funktion `OnRep_<Var>(Old)` gleich selbst an (`src/HE_Editor/LevelScriptPanel.cpp:735-795`).

### 1.1 Wo heute geschrieben wird

Hier müsste ein Schreib-Haken sitzen, deshalb vollständig:

* **Interpreter, eigene Variable:** `T::SetVariable` HC.cpp:3653-3664 → `m_ctx.setVariable` → `Runtime::setVariable` RT.cpp:320-328.
* **Interpreter, fremde Variable:** `ctx.setExternal` RT.cpp:644-660 schreibt `i->vars[var]` bzw. `compiled->setVariable` **direkt**, nicht über `Runtime::setVariable`.
* **Codegen, eigene Variable:** CG:2306-2331 erzeugt eine nackte Member-Zuweisung `v_x = …;`.
* **Codegen, fremde Variable (Schnellpfad):** CG:2389-2424 erzeugt `o__->Acc() = v;`, und der Accessor liefert `T&` (CG:3225). Auch das ist roh.
* **Host-Seite:** Replikator-Apply auf dem Client (`Runtime::setVariable`), Savegame-Apply (`EngineApi.cpp:508`), Hot-Reload-Restore (`WidgetManager.cpp:5126`), `reseedVariables` RT.cpp:330.
* Container-Knoten (Array/Set/Map) sind rein und liefern eine neue Kopie (HC.cpp:4111 ff.). Ein Container ändert sich also auch nur über SetVariable/SetExternal.

### 1.2 Wer wann tickt

* **Spiel** (`src/HE_Game/src/GameApplication.cpp`): Timer (2831) … Collision (2987) … Widgets tick (3008) → Net-Session (3033) → PlayerHost (3044) → EntityHost (3049) → `runtime.update` (Delays, 3057) → `updateUIInput` (3060).
* **Editor-Play** (`src/HE_Editor/EditorApplication.cpp`): Collision (3392) → Widgets tick (uiLive) → `scripts().update` (Delays) → PlayerHost/EntityHost (nur `simulating`, 3435/3438) → `TimerSystem::dispatch` (3450) → `m_antiCheat.pump` (3455).
* **Game Instance, Level-Script und reine Create-Object-Instanzen tickt niemand.** Eine Lösung, die am Tick hängt, erreicht sie nicht.

## 2. Entscheidung

### 2.1 Zwei Bausteine, beide als Variablen-Häkchen

**A. „Notify on Change“** (`Variable::notifyChange`) auf jeder Instanzvariable:
Ändert sich der Wert, ruft die Engine
1. die eigene private Funktion **`OnChanged_<Var>(Old)`** auf, falls vorhanden (das Häkchen legt sie an, wie „Notify“ es für `OnRep_` tut), und
2. danach `emitEvent(self, "<Var>Changed", Neu)`. Jede andere Klasse, die per **Bind Event** an diese Instanz gebunden ist, bekommt ihr eigenes Event `<Var>Changed` mit dem neuen Wert. Das ist der vorhandene Dispatcher, nichts Neues.

**B. „Bind To“** (`Variable::bindSource` + `Variable::bindVar`) auf einer Instanzvariable des **Konsumenten**:
Die Variable bekommt den Wert einer **öffentlichen** Variable einer Quelle, sobald er sich dort ändert. Die Quelle ist eines von:
* `@GameInstance`: die Game Instance,
* der Name einer **Ref-Variable dieser Klasse** (die Quelle ist dann, worauf diese Ref gerade zeigt).

Die Quelle muss nichts tun und nichts angehakt haben. Bindungen sind **einseitig** (Quelle → Konsument).

Ein Konsument, der zusätzlich auf die Ankunft reagieren will, hakt an derselben Variable „Notify on Change“ an und bekommt sein `OnChanged_<Var>`. A und B setzen also aufeinander auf, statt zwei Wege zum selben Ziel zu sein.

Typisches Beispiel: Ein HUD-Widget hat `Score : Int`, gebunden an `@GameInstance.Score`, mit Notify on Change, und `OnChanged_Score` setzt den Text. Kein Tick, kein Cast, kein Get.

### 2.2 Erkennung: Abgleich am Frame-Ende, kein Schreib-Haken

Die Engine gleicht **einmal pro Frame** alle beobachteten Variablen mit einer Schattenkopie ab (`Runtime::exchangeState()`), so wie der Replikator.

Begründung, warum kein Haken an den Schreibstellen:

* **Codegen schreibt roh** (§1.1): drei Emissionsstellen plus die Accessor-Form `T&` müssten umgebaut werden. Danach wäre jede künftige Schreibstelle eine neue Gelegenheit, den Haken zu vergessen. Der Abgleich braucht vom Codegen nur Metadaten.
* **Host-Schreibstellen** (Replikator, Savegame, Hot Reload, Reseed) sieht der Abgleich alle, ohne dass man sie aufzählen muss.
* **Instanzen ohne Tick** (Game Instance, Level-Script, Create Object) sind abgedeckt, weil der Abgleich über die Runtime läuft und nicht über einen Tick.
* **Semantik wie RepNotify:** mehrere Schreibvorgänge in einem Frame ergeben eine Meldung mit `Old` = Stand des letzten Abgleichs. Schreiben und Zurücksetzen im selben Frame ergibt keine Meldung. Ein Haken müsste dafür selbst puffern und würde dann doch zu einem Abgleich.
* **Kosten** wachsen nur mit der Zahl der **beobachteten** Variablen (Opt-in-Häkchen), nicht mit der Zahl der Instanzen. Die Liste wird beim Registrieren gebaut, nicht jedes Frame durch `m_insts` gesucht (§3.1).

Später möglich, nicht Schritt 2: ein Dirty-Bit pro beobachteter Variable, das `Runtime::setVariable`/`setExternal` setzen, damit der Abgleich unveränderte Container überspringt. Erst wenn eine Messung es verlangt.

### 2.3 Wo im Frame

`Runtime::exchangeState()` läuft **nach aller Spiellogik des Frames, vor UI-Eingabe und Render**:

* **Spiel:** direkt nach `m_gameInstance.runtime().update(gameDt, deltaTime)` (GameApplication.cpp:3057), vor `updateUIInput()` (3060).
* **Editor-Play:** am Ende des `uiLive`-Blocks nach `m_antiCheat.pump()` (EditorApplication.cpp:3455). Bewusst unter `uiLive` und nicht unter `simulating`: in App-Projekten laufen Game Instance und Widgets ohne Play, wie die Timer dort.

Pause: Der Abgleich läuft auch im pausierten Spiel. Eine Pause friert die Spielzeit ein, aber ein Pausenmenü, das an `@GameInstance.Volume` gebunden ist, muss trotzdem nachziehen. Das ist die Regel, die Widgets-Tick und Timer schon haben (rohes dt).

## 3. Ablauf von `exchangeState()` (verbindlich für Schritt 2)

### 3.1 Beobachtungsliste

Die Runtime führt `m_watched`: pro Instanz die beobachteten Einträge, gebaut in `add`/`addLevels`/`addCompiled` aus den Deklarationen (Interpreter: `graphAt(level).variables`, Codegen: `varInfos()`), nach derselben Regel wie `replicatedVariablesOf`: die Deklaration der abgeleitetsten Ebene gewinnt, nur `scope == 0`.

```
struct WatchedVar {
    std::string name;
    bool        notify;            // Notify on Change
    std::string bindSource;        // "" | "@GameInstance" | Name einer Ref-Variable
    std::string bindVar;           // öffentliche Variable der Quelle
    Value       shadow;            // eigener Wert beim letzten Abgleich (für notify)
    bool        baselined = false;
    InstanceId  boundTo   = 0;     // Quelle beim letzten Abgleich (für bind)
    Value       sourceShadow;      // Quellwert beim letzten Abgleich
    bool        warned    = false; // „Quelle nicht auflösbar“ nur einmal loggen
};
```

`remove()` streicht die Instanz aus `m_watched`, so wie es die Listener schon abräumt.

### 3.2 Eine Runde

```
exchangeState():
  for round in 0 .. kMaxRounds-1 (8):
      changedAny = false
      // Phase 1: Bindungen ziehen (Quelle → Konsument)
      for each (inst, w) mit w.bindSource:
          src = resolve(inst, w.bindSource)          // GameInstance oder getVariable(inst, refVar).ref
          if !alive(src) || !istÖffentlich(src, w.bindVar):
              w.boundTo = 0; einmal warnen; continue  // Konsument behält seinen Wert
          now = getVariable(src, w.bindVar)
          if src != w.boundTo || !valuesEqual(now, w.sourceShadow):
              w.boundTo = src; w.sourceShadow = now
              if w.notify && !w.baselined:            // Erstschub zählt als Änderung (§4)
                  w.shadow = getVariable(inst, w.name); w.baselined = true
              setVariable(inst, w.name, coerce(now, Typ von w))
              changedAny = true                       // Ketten ohne Notify laufen weiter
      // Phase 2: Änderungen melden
      for each (inst, w) mit w.notify:
          now = getVariable(inst, w.name)
          if !w.baselined: w.shadow = now; w.baselined = true; continue
          if valuesEqual(now, w.shadow): continue
          old = w.shadow; w.shadow = now; changedAny = true
          callFunction(inst, "OnChanged_" + w.name, requirePublic=false, {old})  // fehlt sie: nichts
          emitEvent(inst, w.name + "Changed", now)
          if ++fires > kMaxFires (256): warnen, abbrechen (Rest kommt nächsten Frame)
      if !changedAny: break
```

* **Ende einer Runde:** Der Abgleich hört auf, wenn eine Runde **nichts geschoben und nichts gemeldet** hat. Deshalb setzt auch Phase 1 `changedAny`, sonst bräche eine Kette aus gebundenen Variablen ohne Notify nach der ersten Runde ab.
* **Ketten** A → B → C (B bindet an A, C an B) schließen sich in einem Frame, weil jede Runde erneut zieht und meldet. Die Reihenfolge innerhalb von Phase 1 spielt dafür keine Rolle: Liest C den alten Wert von B, holt die nächste Runde das nach. **Zyklen** (A bindet an B, B an A, beide ändern sich gegenseitig) laufen höchstens `kMaxRounds` Runden. Danach steht der Rest im nächsten Frame an, mit Warnung. Gleicher Stil wie `dispatchToListeners` (Tiefe 32, Budget 256).
* Ein Handler darf schreiben, zerstören, erzeugen. Die Liste wird pro Runde als Kopie der Ids durchlaufen, und jede Instanz wird vor dem Zugriff mit `alive()` geprüft (zerstörte liegen bis zum Frame-Ende in `m_doomed`).
* `valuesEqual` zieht dafür aus `HE_Scene/Net/ValueWire` nach `HE_Core` (`HorizonCode::valuesEqual`), denn es ist eine reine Funktion ohne Netzbezug (ValueWire.cpp:277). `ValueWire::valuesEqual` leitet weiter, damit der Replikator unverändert bleibt.

## 4. Garantien (gehören ins Handbuch)

* **Einen Frame später, wie `worldMatrix`.** Was in Frame N geschrieben wird, ist am Ende von Frame N gemeldet bzw. gebunden. Der Tick des Konsumenten sieht es in Frame N+1. Wer es sofort braucht, ruft wie bisher Call (Ref).
* **Zusammengefasst:** eine Meldung pro Frame und Variable, `Old` ist der Stand des letzten Abgleichs. Schreiben und Zurücksetzen im selben Frame wird nicht gemeldet.
* **Der eigene Anfangszustand meldet nicht.** Die Schattenkopie entsteht beim ersten Abgleich, in dem die Instanz vorkommt. Defaults und alles, was die Instanz in Construct/BeginPlay im selben Frame **selbst** setzt, sind der Ausgangszustand, kein „Changed“.
* **Ein Schub über eine Bindung zählt dagegen immer als Änderung**, auch der erste. Bindungen schieben einmal beim ersten Auflösen und bei jedem Quellwechsel, sonst bekäme ein Konsument, der nach dem Produzenten entsteht, den Wert nie. Weicht der geschobene Wert vom eigenen ab, folgt `OnChanged_<Var>` noch im selben Abgleich. So zeigt das HUD aus §2.1 den Punktestand sofort und nicht erst bei der ersten Änderung. Construct sieht ihn noch nicht, denn der erste Abgleich läuft am Ende des Frames, in dem die Instanz entsteht.
* **Quelle weg:** Ref null, Quelle zerstört, Variable nicht (mehr) öffentlich → die Bindung ruht, der Konsument behält den letzten Wert, eine Warnung pro Bindung (nicht pro Frame). Zeigt die Ref wieder auf etwas, wird sofort geschoben.
* **Kein Auf-/Abmelden:** Die Quelle wird jedes Frame über die Ref-Variable aufgelöst. Ref umhängen, Quelle zerstören, Szenenwechsel-GC (`retainOnlyReachableFrom` RT.cpp:243): alles heißt nur „diesen Frame eine andere bzw. keine Quelle“.
* **Bindungen halten nichts am Leben.** Erreichbarkeit für den GC kommt weiter nur aus Ref-Variablen. Die Ref, über die gebunden wird, ist ohnehin eine Ref-Variable.
* **Handler sind normaler Nutzercode** und dürfen latente Knoten (Delay) benutzen. Der Rest läuft dann über `Runtime::update` in späteren Frames.
* **Rein lokal.** Bindungen und OnChanged laufen auf jeder Maschine für sich und gehen nicht übers Netz. Wer einen Wert über die Leitung braucht, nimmt Replicated.

## 5. Randfälle

| Fall | Verhalten |
|---|---|
| **Replicated + Notify on Change, Client** | Replikator schreibt im Lauf der Net-Session und ruft `OnRep_<Var>`, jedenfalls vor dem Abgleich, der als Letztes in der Spiellogik läuft (genaue Stelle von `dispatchRep` in Schritt 2 nachprüfen, für die Reihenfolge genügt „vorher“). Der Abgleich sieht die Änderung und ruft `OnChanged_<Var>`. Reihenfolge also **OnRep vor OnChanged**. Faustregel fürs Handbuch: OnRep = „kam übers Netz, nur Client“, OnChanged = „hat sich geändert, egal wodurch, überall“. |
| **Hot Reload** (Variable umbenannt/umgetypt) | Mindestens: neue Basis ohne Meldung. Das gilt automatisch, wenn Reload als remove + add läuft, denn dann ist die Instanz neu. Die `Runtime`-Schnittstelle zeigt keinen eigenen Reload-Pfad. Findet Schritt 2 einen, der Instanzen an Ort und Stelle ersetzt, wird `m_watched` dort neu gebaut, und Einträge mit gleichem Namen **und** gleicher Wertform (`valueTypesMatch`) behalten ihre Schattenkopie. Gleiche Idee wie das Neu-Tabellieren im Replikator. |
| **Bind To + Replicated auf derselben Variable** | Gesperrt: Der Editor graut das jeweils andere aus, der Loader verwirft `bindSource`/`bindVar`, wenn `replicated` gesetzt ist. Sonst schrieben auf dem Client zwei Quellen (Replikator von der Autorität, Bindung lokal) gegeneinander, und die HE_Core-Runtime weiß nicht, ob sie Client ist. Wer einen gebundenen Wert replizieren will, bindet eine zweite, nicht replizierte Variable und kopiert im `OnChanged_`. Siehe offene Frage 6. |
| **`reseedVariables`** (Game Instance pro Play-Session) | Neue Basis ohne Meldung. Bindungen auf die Game Instance schieben beim nächsten Abgleich (Quellwert anders bzw. `sourceShadow` zurückgesetzt). |
| **Gebundene Variable wird auch lokal beschrieben** | Erlaubt. Die Bindung schiebt nur, wenn sich die **Quelle** ändert, sie überschreibt nicht jedes Frame. Bis dahin gilt der lokale Wert. |
| **Typen passen nicht** | Der Editor bietet nur Quellvariablen mit verträglichem Typ an. Zur Laufzeit wird mit `coerce` auf den Konsumententyp gebracht. Unpassende Form (Container gegen Skalar, anderes Struct) → Bindung ruht mit Warnung. |
| **Ref-Variablen** | Notify on Change und Bind To sind für Ref erlaubt (lokal ist eine Instanz-Id ein gültiger Wert). Anders als bei Replicated/Save Game gibt es keinen Grund, es zu sperren. |
| **Bind auf eine Ref-Variable, die selbst gebunden ist** | Erlaubt. Die Kette wird über die Runden aufgelöst (§3.2). |
| **Funktions-lokale Variablen** | Keine der beiden Optionen. Das Häkchen ist ausgegraut, der Loader verwirft es (wie `repNotify` HC.cpp:2006). |
| **Klasse ohne Handler** | Notify on Change ohne `OnChanged_<Var>` meldet nur das Event `<Var>Changed`. Das ist gültig, z. B. wenn nur andere Klassen zuhören. |
| **Listener unterscheidet Quellen nicht** | `<Var>Changed` trägt nur den neuen Wert, nicht den Absender. Bindet sich ein Listener an zwei Quellen mit gleichem Variablennamen, kann er sie nicht unterscheiden. Das ist eine bestehende Grenze des Dispatchers (ein `Value`), keine neue. Siehe offene Frage 5. |

## 6. Editor

In der Variablen-Detailansicht (`LevelScriptPanel.cpp`, Block ab Z. 724, nach „Save Game“):

* **Checkbox „Notify on Change“**, nur bei `scope == 0`. Anhaken legt `OnChanged_<Var>(Old)` als private Funktion an, mit genau dem Code, den „Notify“ für `OnRep_` hat (Z. 752-795). Am besten wird dieser Code in einen gemeinsamen Helfer `ensureVarHandler(graph, var, prefix)` gezogen.
* **„Bind To“**: Combo *Quelle* (`—`, `Game Instance`, jede Ref-Variable dieser Klasse mit gesetztem `className`) und Combo *Variable* (öffentliche Instanzvariablen der Quellklasse mit verträglichem Typ). Die Klassenliste kommt aus derselben Quelle wie das Kontextmenü für Get/Set (Ref) heute (`Variable::className`). Eine untypisierte Ref erlaubt Freitext.
* In der Variablenliste ein kleines Kennzeichen (z. B. „⇐“ für gebunden, „◆“ für Notify) an der Zeile, analog zu Replicated. Die Schrift hat keine Pfeile (Memory „Headless-ImGui-Screenshots“), also ein Glyph wählen, das der Font hat.
* `EditorWidgets::helpForLabel("Notify on Change")` / `("Bind To")` und die In-Engine-Doku-Einträge, damit die Tooltip-Abdeckung bei 100 % bleibt.

Neue Knoten gibt es keine. Das Häkchen ist die Deklaration, wie bei Replicated und Save Game.

## 7. Umsetzungs-Checkliste für Schritt 2

Datenmodell und Persistenz:
1. `Variable` (HC.h:409-478): `bool notifyChange = false; std::string bindSource, bindVar;` mit Kommentarblock wie bei Replicated.
2. JSON speichern/laden (HC.cpp:1867-1868 / 2000-2009): Schlüssel `notifyChange`, `bindSource`, `bindVar`, nur geschrieben wenn gesetzt. Beim Laden verwerfen, wenn `scope != 0`. `bindVar` ohne `bindSource` (und umgekehrt) → beides leer. Bei `replicated` → Bindung leer (§5).

Runtime (HE_Core):
3. `HorizonCode::valuesEqual` nach HE_Core ziehen, `ValueWire::valuesEqual` leitet weiter (ValueWire.h:67, ValueWire.cpp:277).
4. `Runtime::m_watched` + Aufbau in `add`/`addLevels`/`addCompiled`, Abbau in `remove`, Neuaufbau bei Hot Reload und `reseedVariables`.
5. `Runtime::exchangeState()` nach §3.2, mit `kMaxRounds = 8`, `kMaxFires = 256` und Einmal-Warnungen.
6. `Runtime::watchedVariablesOf(id)` (Tooling/Tests), gleiche Form wie `replicatedVariablesOf`.

Codegen (Parität):
7. `CompiledVarInfo` (HorizonCodeCompiled.h:24-43): `bool notifyChange = false; const char* bindSource = nullptr; const char* bindVar = nullptr;` **hinten angehängt und vorbelegt**, damit älter generierte Tabellen weiter kompilieren (gleiche Regel wie bei `saveGame`).
8. GenSupport-Slot (GS:759-802, 899) um dieselben Felder erweitern.
9. Emission der Variablentabelle (CG:3282-3294) um die neuen Felder erweitern. **Keine** Änderung an Set-Emission oder Accessoren (§2.2).

Frame-Schleifen:
10. `GameApplication.cpp` nach Z. 3057: `m_gameInstance.runtime().exchangeState();`
11. `EditorApplication.cpp` am Ende des `uiLive`-Blocks nach Z. 3455: dasselbe.
12. Weitere Schleifen gibt es nicht: `runtime().update` wird nur in `GameApplication.cpp` gerufen, und der weltlose App-Modus läuft durch dieselbe Funktion (alles dort ist `if (m_world)`-bewacht, kein Frühausstieg zwischen 2780 und 3057). Ein Test, der nur das Test-Rig verdrahtet, beweist das nicht. Deshalb gehört in Schritt 2 ein Headless-Lauf des echten Spiels bzw. Editors dazu.

Editor und Doku:
13. LevelScriptPanel-UI nach §6, `ensureVarHandler` herausziehen.
14. In-Engine-Doku + Tooltips („Notify on Change“, „Bind To“), Handbuchseite HorizonCode (Garantien §4 und Randfälle §5 sinngemäß), `docs/horizoncode-reference.md`.

## 8. Tests (he_tests, Interpreter **und** kompiliert)

Ein Fixture-Paar Produzent/Konsument, in `HCGEN_CLASSES` aufgenommen, damit die Paritäts-Suite beide Backends vergleicht:

* Notify: Wert ändern → genau ein `OnChanged_X(Old)` mit richtigem Old. Zweimal schreiben im Frame → einmal. Schreiben und Zurücksetzen → keinmal. Defaults/Construct → keinmal.
* Event: zweite Klasse mit Bind Event bekommt `XChanged(Neu)`.
* Erstschub über eine Bindung löst `OnChanged_X` aus (Bind + Notify an derselben Variable), der eigene Anfangszustand nicht.
* Kette A → B → C **ohne** Notify an B schließt sich in einem Frame (Phase 1 hält den Abgleich am Laufen).
* Bind an `@GameInstance` und an eine Ref-Variable: Erstschub beim Auflösen, Nachziehen bei Änderung, kein Überschreiben lokaler Werte ohne Quellenänderung.
* Ref umhängen → sofortiger Schub vom neuen Ziel. Ref null / Quelle zerstört → Bindung ruht, eine Warnung, Wert bleibt.
* Kette A → B → C in einem Frame. Zyklus A ↔ B → bricht nach `kMaxRounds` ab, Warnung, kein Hänger.
* Handler zerstört die eigene oder die Quellinstanz → kein Absturz (gmalloc/ASan-Lauf wie bei den Net-Session-Tests).
* Container und Struct: Änderung eines Elements wird erkannt (das prüft `valuesEqual` nach der Verlagerung).
* JSON-Rundreise der neuen Felder. Loader verwirft sie bei Funktions-locals.
* `reseedVariables` → keine Meldung. Hot Reload mit umbenannter Variable → neue Basis ohne Meldung.
* Codegen: alte `CompiledVarInfo`-Aggregate (4 bzw. 7 Initialisierer) kompilieren weiter.

## 9. Bewusst nicht in Schritt 2 (möglicher Ausbau)

* **Zweiseitige Bindung.** Mit „Quelle gewinnt bei Änderung“ wäre sie machbar, bringt aber die Zyklusfrage in jeden Normalfall. Erst auf Nachfrage.
* **Element-Property-Bindung im Widget** (Text = Variable, wie UMG-Bindings). Heute gibt es dort gar keine Bindung (Thema 119, `widget-pre-construct-design.md` §1). Mit Notify on Change ist es ein Einzeiler im Handler. Eine deklarative Bindung wäre ein eigenes Thema, das auf `exchangeState` aufsetzen kann.
* **Lua/Python/C++-Zugriff** (`horizon.hc.onChanged(ref, "Var", fn)` o. ä.). Diese Frontends erreichen HC-Variablen heute fast gar nicht (§1). Das ist ein eigenes Loch, nicht Teil dieses Features.
* **Geteiltes Zustands-Asset (Blackboard).** Siehe §0.
* **Dirty-Bit-Optimierung.** Siehe §2.2.

## 10. Offene Fragen an das Review

1. Stimmt die Auslegung (§0)? Oder war ein geteiltes Zustands-Asset bzw. etwas ganz anderes gemeint?
2. Reicht einseitige Bindung für den ersten Wurf?
3. Event-Name `<Var>Changed`: passt das, oder lieber `OnChanged_<Var>` auch für das Dispatcher-Event? Das hieße dann gleich wie die Funktion und könnte verwirren.
4. Soll der Abgleich auch im pausierten Editor-Play laufen (§2.3, heute: ja unter `uiLive`)?
5. Braucht `<Var>Changed` den Absender als zweites Argument? Das würde den Dispatcher von einem auf zwei `Value` erweitern und alle `emitEvent`-Stellen berühren, also eher ein eigenes Thema.
6. Bind To + Replicated sperren (§5) oder „auf dem Client ruht die Bindung, Replikation gewinnt“? Das Zweite braucht eine Autoritäts-Abfrage in der HE_Core-Runtime, die es heute nicht gibt.
7. Schnitt der Umsetzung: Vorschlag **2a** Runtime + Datenmodell + JSON + Frame-Verdrahtung + Interpreter-Tests, **2b** Codegen-Parität + HCGEN-Fixture, **2c** Editor-UI + Doku/Tooltips. 2b und 2c können parallel laufen, wenn 2a die Felder festgelegt hat.
