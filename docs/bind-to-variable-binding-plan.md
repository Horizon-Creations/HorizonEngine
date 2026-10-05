# Bind To: Variablen laufend aneinander binden

Thema 137, Schritt 1 (Entwurf). Nur Doku, kein Feature-Code. Schritt 2 setzt die Bindung um (§3), Schritt 3 die Änderungsmeldung `OnChanged_<Var>` samt Brücke zu Lua/Python/C++ (§4), Schritt 4 prüft und legt den PR an.

**Herkunft.** Der erste Entwurf zu Thema 127 (Commits ab3afee8, fbb9908c, Datei `docs/state-driven-data-exchange-design.md` in diesem Stand) hat dieses Feature schon einmal beschrieben. Er wurde als Lesart von „State Driven Data Exchange“ verworfen (Antwort des Menschen auf Frage #16), als Idee aber zurückgestellt. Dieses Dokument nimmt ihn als Gerüst und zieht ihn auf den heutigen Stand von main nach. Seitdem sind Pull on Construct, Extract on Destruct, Expose on Spawn und der Wegfall des Listener-Dispatches für Lebenszyklus-Events auf main gelandet (PR #91, Merge b4a642ea). Mehrere Annahmen des Erstentwurfs stimmen deshalb nicht mehr. Was sich geändert hat, steht in §1.4.

**Zeilennummern** beziehen sich auf `origin/main` 2b0ea05f. Abkürzungen: **HC.h** = `src/HE_Core/include/HorizonCode/HorizonCode.h`, **HC** = `src/HE_Core/src/HorizonCode/HorizonCode.cpp`, **RT** = `src/HE_Core/src/HorizonCode/HorizonCodeRuntime.cpp`, **CC** = `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h`, **CG** = `src/HE_Scene/src/HcCodegen.cpp`, **GA** = `src/HE_Game/src/GameApplication.cpp`, **EA** = `src/HE_Editor/EditorApplication.cpp`.

> **Wichtig für Schritt 2:** Der Themen-Zweig steht auf 469fa9f6, also **vor** PR #91. Auf ihm fehlen `HcPull`, `pullShapesCompatible`, `m_creatorStack`, `m_pullWarned`, `dispatchDestroyed` und alles andere, worauf dieser Entwurf aufsetzt. Schritt 2 holt deshalb zuerst `origin/main` per Merge-Commit herein (kein Rebase, kein Force-Push), bevor er Code anfasst.

## 0. Was gemeint ist, und was ausdrücklich nicht

**Bind To:** Eine Instanzvariable einer Klasse wird deklarativ an eine öffentliche Variable einer Quelle gebunden. Ändert sich der Wert an der Quelle, zieht die Engine ihn am Ende des Frames nach, und zwar während der gesamten Lebensdauer der Instanz. Quellen sind die Game Instance, der Erzeuger und eine Ref-Variable der eigenen Klasse.

**OnChanged_\<Var\>:** Eine Variable meldet ihre eigene Änderung. Die Klasse bekommt ihre private Funktion `OnChanged_<Var>(Old)` aufgerufen, und andere Klassen, die per Bind Event zuhören, bekommen ein Event.

Zusammen ersetzt das das heutige Muster **Event Tick → Cast → Get (Ref) → vergleichen** in jeder Konsumentenklasse für den Fall „ich will immer den aktuellen Wert von X sehen“.

### 0.1 Abgrenzung zu Thema 127

| | Pull on Construct | **Bind To** (neu) | Extract on Destruct |
|---|---|---|---|
| Wann | einmal, bei der Registrierung, **vor** jedem Nutzercode | **laufend**, einmal pro Frame am Frame-Ende, **nach** dem Nutzercode | einmal, in `Runtime::destroy`, nach dem eigenen Destruct |
| Richtung | Quelle → neue Instanz | Quelle → lebende Instanz | sterbende Instanz → Listener |
| Erkennung | keine nötig (Schnappschuss) | Abgleich mit Schattenkopie | keine nötig (Ereignis) |
| Deklaration | `Variable::pull*` | dieselbe `pull*`-Angabe plus Schalter `bindTo` (§2, offene Frage 1) | `Graph::extract` |

Die drei teilen sich den Lebenszyklus, ohne sich zu überschneiden: Pull deckt die Geburt ab, Extract den Tod, Bind To die Zeit dazwischen. Daraus folgen zwei Sätze fürs Handbuch:

* **Eine Instanz, die mitten im Frame stirbt, meldet ihre letzten Änderungen nicht mehr.** Der Abgleich am Frame-Ende findet sie nicht mehr. Für „was hatte sie zuletzt“ ist Extract on Destruct da.
* **Bind To + Replicated bleibt gesperrt**, anders als Pull + Replicated. Die Begründung steht in der SDDE-Doku §2.6: Bei Pull schreiben keine zwei Quellen *dauerhaft* gegeneinander. Bei einer Dauerbindung täten sie es auf dem Client (Replikator von der Autorität, Bindung lokal).

**Warum das die Trennung des Menschen nicht unterläuft.** Antwort #16 hat festgelegt, dass State Driven Data Exchange die beiden Enden meint und nicht den laufenden Austausch. Dieser Entwurf lässt Pull on Construct unverändert ein Schnappschuss sein. Er nutzt nur denselben **Deklarationsplatz** (welche Quelle, welche Variable, welches Member) und schaltet per eigenem Häkchen einen **anderen Lebenszyklus** dazu. Wer nur Pull will, sieht und bekommt genau das, was heute auf main steht.

## 1. Ist-Stand auf main

### 1.1 Datenaustausch zwischen HC-Instanzen

Der Austausch ist weiter **explizit**: Get/Set (Ref), Call (Ref), Bind/Emit Event, Cast, Game Instance, dazu jetzt Pull on Construct und OnDestroyed. Es gibt keinen Beobachter, kein OnChanged und keine Dauerbindung. Die einzige laufende Änderungserkennung ist weiter der Replikator: `PropertyReplicator.cpp:432` pollt und vergleicht mit `valuesEqual` gegen eine Schattenkopie.

### 1.2 Wo geschrieben wird

Ein Schreib-Haken müsste an jeder dieser Stellen sitzen:

* **Interpreter, eigene Variable:** `ctx.setVariable` (RT:941) → `Runtime::setVariable` (RT:692).
* **Interpreter, fremde Variable:** `ctx.setExternal` (RT:1033) läuft heute über `Runtime::setPublicVariable` (RT:702). Das ist neu gegenüber dem Erstentwurf, der hier noch einen rohen Zugriff auf `i->vars` fand.
* **Codegen, eigene Variable:** CG:2393, eine nackte Zuweisung `iv.member = …;`.
* **Codegen, fremde Variable:** CG:2493 (`acc() = …` auf Self) und CG:2508 (`o__->acc() = …` im Compiled-zu-Compiled-Schnellpfad). Beides ist weiter **roh**.
* **Host-Seite:** Replikator-Apply (`PropertyReplicator.cpp:204/217` → `Runtime::setVariable`), Savegame (`EngineApi.cpp:508`), `reseedVariables` (RT:719), Pull on Construct (RT:284), Expose on Spawn (`setPublicVariable`), Widget-Hot-Reload (`restoreState`).

Die Codegen schreibt also nach wie vor an mehreren Stellen an jeder Runtime vorbei. **Das Argument des Erstentwurfs für einen Abgleich statt eines Schreib-Hakens gilt unverändert** (§3.3).

### 1.3 Frame-Ablauf und eine Runtime für alles

* **Eine Runtime:** Spiel und Editor hängen Welt, Widgets, Level-Script und Objekte an die Runtime der Game Instance (`setScriptRuntime(&m_gameInstance.runtime())`, GA:1263/1719, EA:1487). Ein Abgleich pro Frame erreicht damit jede Instanz.
* **Spiel** (`GameApplication::OnRender`, GA:2751-3310): Timer (:2832) … Widgets-Tick (:3009) → **Net-Session** (:3034, hier wendet der Client replizierte Werte an) → PlayerHost (:3045) → EntityHost (:3050) → `runtime().update` (:3058, Delays) → `updateUIInput` (:3061, Klicks) → Weltsysteme, Renderer-Einstellungen → **`dispatchNetEvents`** (:3302, hier laufen `OnRep_<Var>`) → `m_antiCheat.flush()` (:3310, „LAST in the frame“).
* **Editor:** Der `uiLive`-Block (EA:3415-3462) tickt Widgets, `scripts().update`, Player/Entities (nur `simulating`), Timer und `m_antiCheat.pump`. Das Frame-Ende ist `dispatchNetEvents` (EA:4456) → `m_antiCheat.flush()` (EA:4462). Unter der **Editor-Pause** steht der `uiLive`-Block still (Kommentar EA:3406).
* **Gezeichnet wird erst nach `OnRender`** (`Application.cpp:740` → `m_renderer->Render()` :753). Alles, was in `OnRender` bis zum Schluss geschrieben wird, ist im selben Bild sichtbar. Im Event-getriebenen Modus (Apps) entscheidet `WantsPresent()` (:747) danach, ob überhaupt gezeichnet wird.
* **Game Instance, Level-Script und reine Create-Object-Instanzen tickt niemand.** Ein Mechanismus, der am Tick hängt, erreicht sie nicht. Einer über die Runtime schon.

### 1.4 Was sich gegenüber dem Erstentwurf geändert hat

| Erstentwurf (fbb9908c) | Heute auf main | Folge für diesen Entwurf |
|---|---|---|
| Abgleich direkt nach `runtime().update`, „OnRep vor OnChanged“ | Der Client wendet replizierte Werte am Frame-**Anfang** an (:3034), `OnRep` läuft erst am Frame-**Ende** (:3302). Nach `runtime().update` käme `OnChanged` **vor** `OnRep` | Der Abgleich rückt ans Frame-Ende hinter `dispatchNetEvents` (§3.4). Damit stimmt die Reihenfolge wieder, und Klicks aus `updateUIInput` gehören noch zum selben Frame |
| Eigene Felder `bindSource`/`bindVar` | `Variable::pullSource/pullVar/pullMember/pullClass`, Typregel `pullShapesCompatible`/`pullValuesCompatible`/`pullConvert`, Fehlertexte `PullFailure`/`pullFailureText` (HC.h:490-575), Editor `HcPull`/`HcPullUi` mit Quellenwahl, Member-Combo, Statuszeile und „Add to Target“ | Bind To setzt auf die Pull-Angabe auf (§2), statt alles ein zweites Mal zu bauen |
| „Erstschub über die Bindung zählt als Änderung“, Construct sieht den Wert noch nicht | Pull on Construct legt den Wert **vor** Construct hin | Für Game Instance und Erzeuger ist der Anfangswert schon in PreConstruct/Construct da. Nur die Ref-Quelle braucht noch einen Erstschub (§3.2) |
| `<Var>Changed` über `emitEvent` → `dispatchToListeners` | `fireEvent` gibt beim Listener an **dessen** Listener weiter (RT:1576-1587). `dispatchDestroyed` (RT:489-571) zeigt, wie man das vermeidet und pro Listener die Signatur prüft | Eigener `dispatchChanged` nach diesem Vorbild (§4.3) |
| Lebenszyklus-Events gehen an Listener | Nur noch `OnDismissed` (RT:1349-1360), PreConstruct/Construct/Destruct/BeginPlay/OnInit … laufen nur in der eigenen Klasse | Nichts zu tun. Bestätigt die Regel „Listener-Events sind eigene, typisierte Events“ |
| `CompiledVarInfo` mit `nullptr`-Strings | Strings sind `""` und nie `nullptr` (CC:43-51) | Neue Felder genauso |
| `CompiledEventInfo` nur `{name, elem}` | `argType`/`typeName` gibt es (CC:53-64) | `dispatchChanged` kann kompilierte Listener prüfen, ohne Formatänderung |
| `valuesEqual` nach HE_Core ziehen | liegt weiter in `HE_Scene/Net/ValueWire` (ValueWire.h:67/73, .cpp:263/277) | Bleibt Aufgabe von Schritt 2 (§3.6) |

## 2. Datenmodell: Bind To als Modus der Pull-Angabe

### 2.1 Entscheidung (Empfehlung, offene Frage 1)

An `Variable` (HC.h:490-505), hinter `pullClass`:

```
// ── Bind To (docs/bind-to-variable-binding-plan.md) ───────────────────────
// Keeps the pull's source BOUND for the instance's whole life: at the end of
// every frame the runtime compares the source value with what it last saw and,
// when it moved (or the source is another instance now), writes it here. The
// construct-time pull above still runs first, so the value is there before
// PreConstruct. A local write stays until the SOURCE changes again.
// INSTANCE variables only; never together with `replicated` (the loader drops
// bindTo then).
bool        bindTo  = false;
// Source kPullFromRef only: the name of a Ref INSTANCE variable of this class;
// the source is whatever that reference holds at the moment of the compare.
std::string pullRef;
```

Neue Quellschreibweise neben `kPullFromGameInstance`/`kPullFromCreator`:

```
inline constexpr const char* kPullFromRef = "Ref";
```

* **Game Instance / Erzeuger + `bindTo`:** Pull bei der Registrierung wie heute, **danach** laufender Abgleich.
* **Ref + `bindTo`:** **kein** Pull bei der Registrierung, denn die Ref-Variable hat da nur ihren Default (null). Das ist genau der Grund, aus dem die SDDE-Doku §2.3 Ref-Quellen für Pull abgelehnt hat. Der erste Wert kommt mit dem ersten Abgleich, nachdem die Ref auf etwas zeigt (§3.2).
* **Ref ohne `bindTo`** ist kein gültiger Zustand. Der Loader verwirft die Angabe mit Warnung, der Editor bietet „Ref“ nur im Modus Bind To an. `pullOnConstruct` überspringt Ref-Quellen ohne Warnung. Sie sind nicht fehlgeschlagen, sie sind nicht seine Aufgabe.
* `pullVar`/`pullMember` bedeuten dasselbe wie bei Pull. Eine Bindung an ein Struct-Member (`LastRun.Score`) geht also ohne Extraarbeit.
* `pullClass` bleibt für den Erzeuger. Bei der Ref-Quelle kommt die Klasse aus `className` der Ref-Variable (Editor-Metadaten). Zur Laufzeit wird sie nicht geprüft, wie bei Get (Ref).

**JSON** (`variableToJsonObj` HC:1923 / `variableFromJsonObj` HC:2055): Im vorhandenen Objekt `"pull"` kommen `"bind": true` und `"ref": "<Name>"` dazu, beide nur geschrieben, wenn gesetzt. Verworfen wird beim Laden:
* `bind` bei `replicated` (mit Warnung, §0.1),
* `src == "Ref"` ohne `bind` oder mit leerem `ref`,
* `ref`, das keine Ref-Instanzvariable dieser Klasse ist. Das prüft der Loader nur innerhalb des Graphs. Geerbte Ref-Variablen prüft die Runtime und meldet einmal (`PullFailure::NoRefVariable`, §3.5).

**Ältere Engines:** Ein Projekt mit `src: "Ref"` wird dort laut verworfen (`isKnownPullSource`, HC:2061). Eine Bindung an die Game Instance fällt dort still auf einen einmaligen Pull zurück, weil `bind` unbekannt ist. Das ist die gutmütigste mögliche Rückstufung, und so gehört es auch in die Release-Notes.

### 2.2 Die Alternative und ihre Kosten

Eigene Felder `bindSource`/`bindVar`/`bindMember` wie im Erstentwurf, unabhängig von `pull*`. Dafür spricht die strikte Trennung der Begriffe in Daten und UI. Dagegen spricht:

* ein zweiter UI-Block mit eigener Quellen- und Member-Wahl, die mit `HcPullUi` auseinanderlaufen kann (die Detailansicht gibt es zweimal, LevelScriptPanel und UIEditorPanel),
* eine zweite Typprüfung, zweite Fehlertexte, ein zweites „Add to Target“ und ein zweiter Rename-Pfad in `HcRename` (HcRename.cpp:293-300 zieht heute `pullVar` nach),
* ein zweiter Satz Felder in `CompiledVarInfo` und Codegen,
* ein Zustand „Pull von A **und** Bind an B“ an derselben Variable, den niemand braucht und den man trotzdem definieren und testen muss.

Empfehlung: §2.1. Fällt das Review anders aus, ändern sich nur §2 und §5.1 (Editor). Laufzeit (§3) und Meldung (§4) bleiben gleich.

### 2.3 Codegen-Metadaten

* `CompiledVarInfo` (CC:25-52): `bool bindTo = false; const char* pullRef = "";` **hinten angehängt und vorbelegt**, Strings `""` statt `nullptr` (Begründung im Kommentar CC:43-47).
* `hc::VarSlot`/`hc::slot<>`/`varInfosOf` in `HorizonCodeGenSupport.h` um dieselben Felder erweitern.
* Emission CG:3384-3400: Das `trailing` wird um `, true/false, "<ref>"` verlängert, sobald `bindTo` gesetzt ist. Dabei gilt dieselbe Kette wie heute: Wer die hinteren Felder schreibt, muss die vorderen mitschreiben.
* **Kein Bind-Code.** Der Abgleich läuft für beide Wege in derselben Runtime-Funktion über `varInfos()`/`getVariable`/`setVariable`. Die Parität ist damit eingebaut, wie bei Pull (SDDE §2.8).

## 3. Schritt 2: Frame-Ende-Abgleich (die Bindung)

### 3.1 Liste der Bindungen

Die Runtime führt `m_bindings`, gebaut **bei der Registrierung** in `registerLevels`/`registerCompiled` direkt nach `pullOnConstruct`. Es gilt dieselbe Regel wie dort: Die abgeleitetste Deklaration gewinnt (RT:166-187, `report`-Lambda), nur `scope == 0`. Die Game Instance bindet nicht (Schalter `pull=false`, wie bei Pull, SDDE §2.3).

```
struct Binding {
    InstanceId  owner;
    std::string name;            // Zielvariable
    std::string src, var, member, cls, ref;
    InstanceId  boundTo = 0;     // Quelle beim letzten Abgleich (0 = keine)
    Value       sourceShadow;    // Quellwert beim letzten Abgleich (nach Member-Auswahl)
    bool        haveShadow = false;
};
```

* **Startwert:** Hat `pullOnConstruct` für diese Variable erfolgreich gezogen, setzt er `boundTo = src` und `sourceShadow = gezogener Wert`. Der erste Abgleich sieht dann „unverändert“ und schreibt nichts. Ein Spawn-Wert (Expose on Spawn), der nach dem Pull kommt, **bleibt** deshalb stehen, bis sich die Quelle ändert. Dieselbe Regel gilt für einen Hot-Reload-Wert aus `restoreState`. Das setzt SDDE §2.4 („Spawn-Werte schlagen Pull“) sinngemäß fort.
* **Fehlgeschlagener Pull** (Quelle fehlt, Typ falsch …): `haveShadow = false`. Der erste Abgleich, in dem die Quelle auflösbar ist, schiebt.
* `remove()` (RT:357) streicht alle Bindungen mit `owner == id`. Bindungen, deren Quelle `id` war, merken das beim nächsten Abgleich selbst (`alive` false).
* `Runtime::boundVariablesOf(id)` liefert für Tests und Werkzeuge pro Bindung Quelle, `boundTo` und den letzten Grund, nach dem Muster von `pulledVariablesOf` (RT:306).

### 3.2 Ablauf von `Runtime::exchangeState()`

```
int exchangeState():                         // Rückgabe: Zahl der Schreibvorgänge (für requestRedraw)
  writes = 0
  for round in 0 .. kMaxRounds-1 (8):
      changedAny = false
      for key in copy(Schlüssel von m_bindings):   // (owner, name) kopieren, nicht die Einträge
          b = find(m_bindings, key)          // pro Durchgang neu suchen: ein Handler kann
          if !b or !alive(b.owner): continue //   registrieren/entfernen, b ist der ECHTE Eintrag
          (src, why) = resolve(b)             // GameInstance | creatorOf(owner) | getVariable(owner, b.ref).ref
          if why != None: rest(b, why); continue
          val = getVariable(src, b.var)       // nur öffentlich, dieselbe Tür wie Pull/Get (Ref)
          if b.member: val = Feld per findField (folgt formerNames), sonst rest(NoSuchMember)
          if src == b.boundTo && b.haveShadow && valuesEqual(val, b.sourceShadow): continue
          b.boundTo = src; b.sourceShadow = val; b.haveShadow = true
          shape = getVariable(b.owner, b.name)
          if !pullValuesCompatible(val, shape): rest(b, TypeMismatch); continue
          [Schritt 3: Notify-Basis vor dem Schreiben sichern, §4.2]
          setVariable(b.owner, b.name, pullConvert(val, shape))
          changedAny = true; ++writes
      [Schritt 3: Phase 2, Änderungen melden, §4.2]
      if !changedAny: break
  if round == kMaxRounds: einmal pro Sitzung warnen ("Bind To: cycle? stopped after 8 rounds")
  return writes
```

* **Geschoben wird nur, wenn sich die Quelle ändert** (Wert oder Identität), nicht jedes Frame. Eine lokal beschriebene gebundene Variable behält ihren Wert, bis die Quelle sich wieder bewegt. Für das Muster „HUD zeigt, Spieler überschreibt kurz“ ist das richtig. Wer erzwingen will, dass der Quellwert gilt, liest per Get (Ref) wie heute.
* **Ketten** A → B → C schließen sich in einem Frame: Jede Runde zieht erneut, und `changedAny` hält den Abgleich am Laufen, auch ohne Notify. **Zyklen** (A an B, B an A) laufen höchstens `kMaxRounds` Runden. Der Rest steht im nächsten Frame an. Ein Zyklus aus zwei gleichen Werten kommt sofort zur Ruhe, weil der zweite Schub `valuesEqual` trifft.
* **Ruhen** (`rest`): Der Konsument behält seinen letzten Wert, `boundTo = 0`, `haveShadow = false`. Zeigt die Quelle wieder auf etwas, wird sofort geschoben. Gewarnt wird einmal pro (Klasse, Variable, Grund) und Sitzung, mit `pullFailureText` und dem Präfix „Bind To:“. Der Schlüssel liegt in `m_pullWarned` mit eigenem Präfix. **Ausnahme ohne Warnung: eine Ref-Quelle, die null ist.** Das ist ein normaler Zustand („noch nicht zugewiesen“) und kein Fehler.
* Ein **Struct als Quelle ohne Member** wird ganz verglichen (`valuesEqual` geht tief) und ganz geschoben.
* **Kosten:** pro Bindung und Frame ein `getVariable` (Kopie) und ein `valuesEqual`. Die Kosten wachsen also mit der Zahl der **Bindungen**, nicht mit der Zahl der Instanzen. Für große Container ist die Kopie der teure Teil. Schritt 2 misst einmal 1000 Bindungen auf Int und 10 Bindungen auf ein Array mit 10 000 Einträgen und schreibt die Zahlen in die Doku. Ein Dirty-Bit an `setVariable`/`setPublicVariable` hülfe nur dem Interpreter, die Codegen schreibt roh (§1.2). Es kommt erst, wenn die Messung es verlangt.

### 3.3 Warum Abgleich und kein Schreib-Haken

Unverändert aus dem Erstentwurf, mit den Fundstellen von heute:

* Die Codegen schreibt an drei Stellen roh (CG:2393, 2493, 2508). Ein Haken bräuchte dort Umbauten, und jede künftige Emissionsstelle wäre eine neue Gelegenheit, ihn zu vergessen. Der Abgleich braucht von der Codegen nur Metadaten (§2.3).
* Host-Schreibstellen (Replikator, Savegame, Reseed, Spawn, Hot Reload) sieht der Abgleich alle, ohne dass man sie aufzählt.
* Instanzen ohne Tick (Game Instance, Level-Script, Create Object) sind abgedeckt.
* **Semantik wie RepNotify:** Mehrfaches Schreiben in einem Frame ergibt eine Meldung. Schreiben und Zurücksetzen im selben Frame ergibt keine.

### 3.4 Wo im Frame

**Am Frame-Ende, hinter `dispatchNetEvents()` und vor `m_antiCheat.flush()`**, in beiden Hosts:

* **Spiel:** GA zwischen :3302 und :3310: `if (m_gameInstance.runtime().exchangeState() > 0) requestRedraw();`
* **Editor:** EA zwischen :4456 und :4462, mit derselben Bedingung, die den `uiLive`-Block schaltet (dort als Merker festhalten, z. B. `m_uiLiveThisFrame`). Unter der Editor-Pause steht also auch der Abgleich still, wie alles andere Skriptgeschehen. Ein Einzelschritt gleicht ab.

Begründung:

* **Nach allem Nutzercode des Frames:** Ticks, Delays, UI-Klicks (`updateUIInput` :3061), Timer und `OnRep` (:3302). Alles, was in Frame N geschrieben wird, ist am Ende von Frame N gebunden.
* **Vor dem Zeichnen:** Gezeichnet wird erst nach `OnRender` (§1.3). Ein gebundenes HUD zeigt den neuen Wert also **im selben Bild**, nicht erst im nächsten. Das ist besser als im Erstentwurf, der vor `updateUIInput` abglich.
* **OnRep vor OnChanged** auf dem Client (§4.4).
* **Vor dem Anti-Cheat-Flush**, denn der soll „nach jedem Skript“ laufen (Kommentar GA:3305-3308), und Handler aus dem Abgleich sind Skripte.
* **Event-getriebene Apps:** Ohne `requestRedraw()` bliebe ein Wert, den der Abgleich schreibt, unsichtbar, bis zufällig die nächste Eingabe kommt. Die Rückgabe von `exchangeState` ist genau dafür da.

**Spielpause (Zeitskala 0):** Der Abgleich läuft trotzdem, wie der Widget-Tick und die Timer (rohes dt). Ein Pausenmenü, das an `GameInstance.Volume` gebunden ist, muss nachziehen.

**Im Editor muss Schritt 2 prüfen,** ob das Spiel-Viewport-UI (EA:3577 ff.) seine Zeichenliste schon vor EA:4456 baut. Dann zeigt die Editor-Vorschau den gebundenen Wert ein Bild später als das Spiel. Das wäre hinnehmbar, gehört aber dann ins Handbuch.

### 3.5 Randfälle

| Fall | Verhalten |
|---|---|
| Quelle zerstört (Erzeuger stirbt, Ref zeigt auf Totes) | Bindung ruht, Wert bleibt, eine Warnung (`CreatorGone` bzw. neu `RefTargetGone`). Die Bindung hält die Quelle **nicht** am Leben, Erreichbarkeit kommt weiter nur aus Ref-Variablen |
| Ref umgehängt | Identität geändert → sofortiger Schub vom neuen Ziel, auch wenn der Wert gleich ist (`boundTo` wechselt) |
| Ref-Variable selbst gebunden | erlaubt, die Kette löst sich über die Runden auf |
| `pullRef` ist keine Ref-Instanzvariable (geerbt, umbenannt, Typ geändert) | Bindung ruht, neue `PullFailure::NoRefVariable`, eine Warnung |
| Game Instance `reseedVariables` (Play-Start, GA/EA `fireInit`) | Die GI-Werte springen auf Defaults zurück. Gebundene Konsumenten ziehen beim nächsten Abgleich nach. Das ist gewollt |
| Szenenwechsel (`retainOnlyReachableFrom`, RT:615) | eingesammelte Instanzen verlieren ihre Bindungen über `remove` |
| Hot Reload Widget | wie Expose on Spawn: `restoreState` gewinnt, bis die Quelle sich bewegt (§3.1) |
| Typ passt nicht (Quelle umgetypt) | ruht mit `TypeMismatch`, eine Warnung. Der Editor zeigt es rot, mit demselben Text |
| Bind + Save Game | erlaubt. `applySavedState` schreibt lokal, die Bindung überschreibt erst bei der nächsten Quellenänderung |
| Bind + Replicated | gesperrt (§0.1): Loader verwirft `bind`, Editor graut aus |
| Bind + Extract on Destruct | unabhängig, der zuletzt gebundene Wert wird extrahiert |
| Funktions-lokale Variable | keine Option (Loader verwirft, wie `pull`) |
| Netz | rein lokal, jede Maschine bindet für sich |

### 3.6 Checkliste Schritt 2

Vorbereitung:
1. `origin/main` per Merge-Commit in den Themen-Zweig holen (siehe Hinweis oben).

Datenmodell und Persistenz:
2. `Variable::bindTo`, `Variable::pullRef`, `kPullFromRef`, `isKnownPullSource` und `pullSourceLabel` („Reference ‹Name›“). Neue `PullFailure`-Werte `NoRefVariable` und `RefTargetGone` mit Text in `pullFailureText`.
3. JSON `pull.bind`/`pull.ref`, Verwerfen nach §2.1.

Runtime (HE_Core):
4. `valuesEqual` (und `valueTypesMatch`) als `HorizonCode::valuesEqual` nach HE_Core ziehen. `ValueWire` leitet weiter, der Replikator bleibt unverändert.
5. `m_bindings`, Aufbau in `registerLevels`/`registerCompiled` nach `pullOnConstruct`, Startwert aus dem Pull-Ergebnis (§3.1), Abbau in `remove`, `clear`.
6. `pullOnConstruct` überspringt `kPullFromRef`.
7. `Runtime::exchangeState()` nach §3.2, `boundVariablesOf(id)`.

Codegen:
8. `CompiledVarInfo` + GenSupport + Emission nach §2.3.

Frame-Schleifen:
9. GA :3302/:3310 und EA :4456/:4462 nach §3.4, `requestRedraw()` bei Rückgabe > 0. Danach mit `grep exchangeState src/HE_Game src/HE_Editor` belegen, dass **beide** echten Hosts verdrahtet sind, nicht nur das Test-Rig (Memory „Test-Rig verdeckt fehlende App-Verdrahtung“).

Editor (§5.1) und Doku:
10. `HcPull`/`HcPullUi` erweitern, Kennzeichen in der Variablenliste, `HcRename` zieht `pullRef` beim Umbenennen der eigenen Ref-Variable mit.
11. EditorHelp-Einträge in beiden Scopes („Script Variable/…“, „UI Variable/…“), `scripts/editor_help_audit.py --check` BASELINE im selben Commit. `docs/horizoncode-reference.md`, In-Engine-Handbuch.
12. **SDDE-Doku nachziehen** (`docs/state-driven-data-exchange-design.md`, liegt nach dem Merge auf dem Zweig): §2.6 „Einmal … wer laufend nachziehen will, macht das wie heute selbst“ → Verweis auf Bind To. §7 „Laufendes Nachziehen nach Construct. Das war der verworfene erste Entwurf.“ → „siehe docs/bind-to-variable-binding-plan.md“. §0 „wird hier nicht weitergeführt“ → „wird in Thema 137 weitergeführt“.

### 3.7 Umsetzungsstand Schritt 2

Umgesetzt nach §2.1 (Modus der Pull-Angabe; Frage 1 war beim Start unbeantwortet, die Empfehlung gilt, bis das Review anders entscheidet).

* **Datenmodell/JSON:** `Variable::bindTo`, `pullRef`, `kPullFromRef`, `PullFailure::NoRefVariable/RefTargetGone`, `pullSourceLabel(src, ref)`, `pullFailureText(…, ref)`. Der Loader verwirft `bind` bei Replicated (Pull bleibt), eine Ref-Quelle ohne `bind`, ohne `ref` oder auf einer Replicated-Variable ganz. **Ob `ref` eine Ref-Instanzvariable ist, prüft nur die Runtime** (`NoRefVariable`): eine abgeleitete Klasse darf über eine geerbte Referenz binden, die der Loader eines einzelnen Graphs nicht sieht.
* **`valuesEqual`/`valueTypesMatch`** liegen jetzt in HE_Core (HorizonCode.h). `ValueWire.h` holt sie per `using`-Deklaration herein. Eigene Weiterleitungsfunktionen gleicher Signatur gingen nicht: jeder unqualifizierte Aufruf mit einem `HorizonCode::Value` findet das Original per ADL und wäre mehrdeutig.
* **Runtime:** Die Bindungen entstehen in `pullOnConstruct`, im selben Durchgang wie der Pull, mit dessen Ergebnis als Startwert. Lese- und Prüfweg der Quelle teilen sich Pull und Bind (`readPullSource`). Ein fehlgeschlagener Pull einer gebundenen Variable meldet nicht ein zweites Mal als „Bind To“. Die Liste ist ein flacher Vektor in Registrierungsreihenfolge; der Abgleich geht pro Runde über den Index, damit Schritt 3 Handler dazwischen rufen kann.
* **Hosts:** GA hinter `dispatchNetEvents`, EA ebenso auf `uiLive`. Ob die Editor-Vorschau die Zeichenliste schon vorher baut, ist **nicht gemessen**; Kommentar und Handbuch sagen „vielleicht ein Bild später“.
* **Editor:** Modus-Combo „Source Mode“, Quelle „Reference“ nur bei Bind To, Liste der Ref-Variablen nur aus der **eigenen** Ebene (eine geerbte Referenz bleibt eingetragen, ist aber nicht wählbar). Bind To bei Replicated nicht wählbar; ein Graph mit beidem fällt im Block auf den Pull zurück. Das Replicated-Häkchen selbst wird **nicht** ausgegraut (es sitzt in LevelScriptPanel/UIEditorPanel, die auf einem anderen Zweig belegt waren). `HcRename` beweist eine Bindung über den `className` der Referenz und zieht `pullRef` beim Umbenennen der eigenen Ref-Variable nach, nicht aber in abgeleiteten Klassen, die über eine geerbte Referenz binden (die warnen dann zur Laufzeit `NoRefVariable`).
* **Offen:** die Kostenmessung aus §3.2 (1000 Bindungen, 10 × 10 000-Array); der Headless-Lauf des echten Spiels mit gebundenem Widget (§6); die Website-Doku.

## 4. Schritt 3: OnChanged_\<Var\> und die Meldung nach außen

### 4.1 Deklaration

`Variable::notifyChange` (bool, `scope == 0`), JSON `"notifyChange": true` nur wenn gesetzt, `CompiledVarInfo::notifyChange = false` hinten angehängt. Das Häkchen **„Notify on Change“** in der Variablen-Detailansicht legt die private Funktion `OnChanged_<Var>(Old)` an, mit dem Code, den „Notify“ heute für `OnRep_` hat (`LevelScriptPanel.cpp:765-800`: FunctionEntry, `access = 1`, ein Parameter `Old` mit der **ganzen** Form der Variable). Dieser Code wird dafür in einen Helfer `ensureVarHandler(graph, var, prefix)` gezogen, den beide Häkchen benutzen.

Für Ref-Variablen ist das Häkchen erlaubt (lokal ist eine Instanz-Id ein Wert), anders als bei Replicated/Save Game.

### 4.2 Phase 2 im Abgleich

```
// in exchangeState(), pro Runde nach Phase 1:
for key in copy(Schlüssel von m_watched):    // notifyChange-Variablen + Script-Abos (§4.5)
    w = find(m_watched, key)                  // echter Eintrag, pro Durchgang neu gesucht
    if !w or !alive(w.owner): continue
    now = getVariable(w.owner, w.name)
    if !w.baselined: w.shadow = now; w.baselined = true; continue
    if valuesEqual(now, w.shadow): continue
    old = w.shadow; w.shadow = now; changedAny = true
    if w.declared:                            // nur bei Häkchen Notify on Change
        callFunction(w.owner, "OnChanged_" + w.name, requirePublic=false, {old})   // fehlt sie: nichts
        dispatchChanged(w.owner, w.name, now)  // §4.3
    if w.scriptWatchers > 0:                  // Deklaration ODER Abo, jeder Eintrag
        notifyHost(w.owner, w.name, old, now)  // §4.5
    if ++fires > kMaxFires (256): einmal warnen, abbrechen (Rest im nächsten Frame)
```

* **Ein Script-Abo ändert nichts an der HC-Seite.** Ein Eintrag, der nur durch `hc.watch` entstanden ist (`declared == false`), ruft weder `OnChanged_<Var>` noch `dispatchChanged`. Ob eine HC-Klasse `<Var>Changed` bekommt, hängt nur am Häkchen und nicht daran, ob gerade ein Lua-Script zuschaut. Umgekehrt geht an den Host nur, was ein Script abonniert hat. Deklaration und Abo teilen sich nur Schattenkopie und Vergleich.
* **Der eigene Anfangszustand meldet nicht.** Die Basis entsteht im ersten Abgleich, in dem die Instanz vorkommt. Defaults, gezogene Pull-Werte, Spawn-Werte und alles, was Construct/BeginPlay im selben Frame setzt, sind Ausgangszustand.
* **Ein Schub über eine Bindung meldet dagegen immer**, auch der erste: Phase 1 legt die Basis an, **bevor** sie schreibt, falls noch keine da ist. So meldet die Ref-Quelle ihren ersten Wert (null → Wert), und eine Bindung an die Game Instance meldet jede spätere Änderung. Eine Bindung an die Game Instance mit erfolgreichem Pull meldet ihren Startwert nicht, denn der ist schon in Construct da.
* Handler dürfen schreiben, zerstören, erzeugen und latente Knoten benutzen. Die Listen werden pro Runde kopiert, jede Instanz wird vor dem Zugriff mit `alive()` geprüft.

### 4.3 Was ein Listener bekommt (Entscheidung, offene Frage 3)

* **Besitzer:** `OnChanged_<Var>(Old)`. Den neuen Wert liest er per Get. Das ist das Muster von `OnRep_<Var>(Old)`.
* **Listener** (per Bind Event an den Besitzer gebunden): ein Event **`<Var>Changed`** mit dem **neuen** Wert als einzigem Argument. Mit `OnChanged_<Var>` darf es nicht gleich heißen: Ein Listener kann selbst eine Variable gleichen Namens mit eigener `OnChanged_`-Funktion haben, und „mein Score hat sich geändert“ ist etwas anderes als „der Score von dem, an den ich gebunden bin“.
* **Verteilt wird über einen eigenen `dispatchChanged`** nach dem Vorbild von `dispatchDestroyed` (RT:489-571) und nicht über `emitEvent`:
  * **direkt** an den Handler des Listeners, ohne die Weiterleitung, die `fireEvent` anhängt (RT:1586). Sonst käme „X von A hat sich geändert“ bei den Listenern von L als „X von L hat sich geändert“ an,
  * **pro Listener typgeprüft:** Der Handler hat kein Argument → feuern ohne Wert. Hat er eins, wird mit `pullValuesCompatible` gegen dessen Form geprüft (interpretiert: Event-Knoten `hasArg/propType/typeName`, kompiliert: `CompiledEventInfo::argType/typeName`). Passt es nicht → nicht feuern, einmal warnen. Der Typ wird geprüft und nicht still umgewandelt, wie bei OnDestroyed,
  * Tiefe 32, Budget 256 wie der Dispatcher, Besitzer selbst nie.
* **Grenze, wie bei OnDestroyed:** `<Var>Changed` trägt keinen Absender. Ein Listener an zwei Quellen mit gleichem Variablennamen kann sie nicht unterscheiden. Der Dispatcher hat einen `Value`, und `EventDecl` hat höchstens ein Argument (HC.h, EventDecl). Ausweg im ersten Wurf: eine gebundene Ref-Variable pro Quelle und `OnChanged_` an der eigenen Kopie. Die Bindung **ist** dann die Zuordnung.
* **Kein Engine-Event-Tabelleneintrag**, aus demselben Grund wie OnDestroyed (SDDE §3.3): Der Typ hängt von der Quellvariable ab.
* **Bestehende Projekte**, die selbst ein Event `<Var>Changed` per Emit Event schicken: eine Lade-Warnung wie bei OnDestroyed (HC:2262), kein Umbenennen.

### 4.4 Netz

`OnRep_<Var>` heißt „kam übers Netz, nur Client“, `OnChanged_<Var>` heißt „hat sich geändert, egal wodurch, überall“. Mit dem Abgleich hinter `dispatchNetEvents` (§3.4) ist die Reihenfolge auf dem Client **OnRep vor OnChanged**. Notify on Change + Replicated ist erlaubt (nur **Bind** + Replicated ist gesperrt).

### 4.5 Brücke zu Lua, Python und C++

Heute erreichen diese Frontends HC-Variablen fast gar nicht. Sie haben eigene Netz-Variablen (`net.declareVar*`) mit `onRep_<name>` (`ScriptContext::callOnRep`, ScriptContext.cpp:1733-1751, geroutet über `NetEvents::dispatchRep`, NetEvents.h:132-150). Der Schnitt für Schritt 3 folgt genau diesem Vorbild:

1. **Runtime → Host:** ein Beobachter-Haken an der Runtime, `std::function<void(InstanceId owner, const std::string& var, const Value& old, const Value& now)> onVariableChanged`. Es gibt genau einen, wie die Debug-Hooks. Phase 2 ruft ihn pro gemeldeter Änderung (`notifyHost`). HE_Core kennt dabei weder ScriptContext noch IGameLogic.
2. **Abos:** Ein Script meldet sich an einer **öffentlichen** Variable einer HC-Instanz an. Die Runtime nimmt sie dann als dynamischen Eintrag in `m_watched` auf, auch ohne Häkchen an der Variable. Kosten entstehen also nur, solange jemand zuschaut. `Runtime::watch(owner, var, token)` / `unwatch(token)`. Das Token gehört dem Host, damit „Script-Instanz stirbt“ alle ihre Abos abräumt.
3. **Host → Script:** Die Hosts (GA, EA) setzen `onVariableChanged` und routen über eine Abo-Tabelle in `ScriptContext` an die Script-Instanz. Lua bekommt `onChanged_<Var>(source, old)` per `callInstanceMethod` mit `luaPushFieldValue`, Python dasselbe über das Backend, gespiegelt an `callOnRep`. Der Name wird wörtlich übernommen, wie bei `onRep_`.
4. **API-Zeile** im Engine-API-Register: `hc.watch(target, "Var")` und `hc.unwatch(target, "Var")`. `target` ist ein Entity (dessen HC-Klasse über `EntityHost::instanceOf`) oder 0 für die Game Instance. Pflichtstellen einer neuen Zeile: Display-Name-Map, `HcNodeDocs`, `isScriptGroup`, Parity-Fixture in `HCGEN_CLASSES` und für die C-ABI-Gruppe Diensttabelle, Wrapper und Probe (Memory „Neue Registry-Row: drei (vier) Stellen“).
5. **C++ (Spiel-dylib):** `IGameLogic` bekommt **hinten angehängt und vorbelegt** `virtual void onHcVariableChanged(uint32_t entity, const char* var)`. Ohne `Value` über die Modulgrenze, mit genau der Begründung von `onRep` (IGameLogic.h:68-78). Den neuen Wert liest das Modul über die Dienste nach. **In Schritt 3 prüfen:** ob die Diensttabelle heute schon einen HC-Variablen-Leser hat. Wenn nicht, gehört er zu dieser Zeile.

Die API-Form (Abo auf jede öffentliche Variable oder nur auf solche mit Notify on Change, Ziel per Entity oder per Ref) ist offene Frage 4.

### 4.6 Checkliste Schritt 3

1. `Variable::notifyChange`, JSON, `CompiledVarInfo::notifyChange` (+ GenSupport, Emission).
2. `m_watched` aus Deklarationen bei der Registrierung, Phase 2 nach §4.2, Basis-Regel bei Bind-Schub.
3. `dispatchChanged` nach §4.3, Lade-Warnung für selbst emittierte `<Var>Changed`.
4. Editor: Häkchen „Notify on Change“ mit `ensureVarHandler`. Bind Event bietet `<Var>Changed (Typ)` an, wenn das Ziel eine bekannte Klasse hat, und ein Knopf „Create \<Var\>Changed Event“ legt das Event typisiert an. Vorbild ist „Create OnDestroyed Event“ (`HcExtract::createOnDestroyedEvent`, HcExtract.h:104, UI in `HcExtractUiBind.cpp`).
5. Beobachter-Haken, `watch`/`unwatch`, ScriptContext-Abo-Tabelle, Lua/Python-Aufruf, `IGameLogic::onHcVariableChanged`, API-Zeilen mit allen Pflichtstellen.
6. EditorHelp, Audit-BASELINE, Handbuch, `docs/horizoncode-reference.md`, Lua/Python-API-Doku.

### 4.7 Umsetzungsstand Schritt 3

Umgesetzt (Commit 29732494 ff.):

* **Datenmodell/JSON/Codegen:** `Variable::notifyChange` (JSON `notifyChange`, der Loader verwirft es bei Funktions-lokalen), `CompiledVarInfo::notifyChange`, `VarSlot::notifyChange`, `slot<>` hinten angehängt. Die Codegen gibt leere Pull-/Bind-Argumente aus, wenn nur `notifyChange` gesetzt ist.
* **Runtime:** `m_watched` entsteht in `registerLevels`/`registerCompiled` (`watchDeclared`), **nicht** in `pullOnConstruct`, sonst fehlte die Game Instance (sie registriert ohne Pull). Phase 2 (`reportChanges`) läuft in jeder Runde von `exchangeState` nach Phase 1; eine Meldung hält die Runden am Laufen, damit ein Handler, der eine Bind-Quelle schreibt, im selben Aufruf durchschlägt. Budget 256 Meldungen pro Aufruf, eine Warnung. Ein Bind-Schub legt die Basis vor dem Schreiben an und meldet deshalb immer. `exchangeState` zählt eine Meldung als Schreibvorgang (der Host zeichnet neu).
* **`dispatchChanged`** nach dem Vorbild von `dispatchDestroyed`: direkt, ohne Weiterleitung, pro Listener mit `pullShapesCompatible` gegen das Event-Argument geprüft (numerisch wird wie beim Pull umgewandelt), sonst übersprungen mit einer Warnung pro (Listener-Klasse, Event).
* **Script-Abo-Naht:** `Runtime::watch(owner, var, token)` nur auf öffentliche Instanzvariablen, `unwatch(token)` / `unwatch(owner, var, token)`, gemeldet über den einen Haken `onVariableChanged(owner, var, old, now, tokens)`. Ein reines Abo ruft weder `OnChanged_` noch `dispatchChanged`.
* **Editor:** Häkchen „Notify on Change“ in LevelScriptPanel, `ensureVarHandler(prefix)` teilt die Funktionsanlage mit „Notify“ (OnRep_). Hilfe-Eintrag „Script Variable/Notify on Change“.
* **Tests:** `tests/test_hc_on_changed.cpp`.

**Offen** (für den nächsten Lauf, Nummern nach §4.5/§4.6):

* §4.5 Punkt 3–5: Hosts (GA/EA) setzen `onVariableChanged` noch nicht; keine Abo-Tabelle in `ScriptContext`, kein `onChanged_<Var>(source, old)` in Lua/Python, kein `IGameLogic::onHcVariableChanged`, keine API-Zeilen `hc.watch`/`hc.unwatch` (mit allen Pflichtstellen der Registry).
* §4.6 Punkt 3: Lade-Warnung für selbst per Emit Event geschickte `<Var>Changed`.
* §4.6 Punkt 4: Bind Event bietet `<Var>Changed (Typ)` noch nicht an, kein Knopf „Create \<Var\>Changed Event“; das Häkchen fehlt im Widget-Editor (UIEditorPanel).
* Kompilierte Seite: kein Paritäts-Fixture `notify_change` in der Codegen-Suite.
* Doku: `docs/horizoncode-reference.md`, In-Engine-Handbuch.

## 5. Editor

### 5.1 Variablen-Detailansicht (Schritt 2)

Der Block aus `HcPullUi::drawSection` (LevelScriptPanel.cpp:828, UIEditorPanel.cpp:5826, **eine** Zeichnung für beide Kopien) bekommt oben einen Modus statt des bisherigen Häkchens:

```
Source mode  ( Off | Pull on Construct | Bind To )
    Source     [ Game Instance | Creator | Ref: Target  ▾ ]   ← Ref nur bei Bind To
    Variable   [ Score   Int                            ▾ ]
    Member     [ …                                      ▾ ]   ← nur bei Struct
    ✓ Bound to Game Instance › Score (Int), kept in sync
Fallback / Initial value   [ 0 ]
```

* **„Bind To“ setzt `bindTo` und lässt die Pull-Felder stehen.** Ein Wechsel zwischen Pull und Bind verliert nichts.
* **Quelle „Ref: ‹Name›“:** Die Combo listet die Ref-Instanzvariablen der Klasse (auch geerbte, mit Elternname). Die Variable-Combo listet die öffentlichen Variablen von deren `className`, über denselben Weg, den der Erzeuger mit `pullClass` heute geht (HcPullUi.cpp:59, Klassen-Asset laden → `HcPull::publicVariables`). Eine untypisierte Ref erlaubt Freitext mit dem gelben Hinweis „class unknown, checked at run time“, wie der Erzeuger ohne Klasse.
* **„Add to Target“** funktioniert unverändert, bei der Ref-Quelle schreibt es in das `className`-Asset.
* **Ausgegraut mit Grund:** Bind To bei `replicated` (und umgekehrt das Replicated-Häkchen bei `bindTo`), im GI-Graph, bei Funktions-locals.
* Das Default-Feld heißt bei Pull „Fallback“ (`defaultSectionLabel`), bei Bind To mit GI/Erzeuger ebenfalls „Fallback“ und bei Bind To mit Ref „Initial value“. Bei der Ref-Quelle ist der Default der Wert bis zum ersten Schub, kein Rückfallwert.
* **Variablenliste:** `HcPullUi::listNote` zeigt „bound“ statt „pulled“, der Tooltip „Bound to Game Instance › Score“.
* **Hilfe:** „…/Source mode“, „…/Bind To“, „…/Source/Ref“ in beiden Scopes.

### 5.2 Notify on Change (Schritt 3)

Häkchen direkt unter „Save Game“, sichtbar bei `scope == 0`. Dazu das Kennzeichen in der Variablenliste und die Bind-Event-Ergänzung aus §4.6 Punkt 4.

## 6. Tests (he_tests, Interpreter **und** kompiliert)

Fixture-Paare in `HCGEN_CLASSES`, damit die Paritäts-Suite beide Wege vergleicht.

**Schritt 2 (Bindung):**
* GI `Score = 7`, Konsument mit Bind To GI.Score: in Construct schon 7 (Construct-Pull). GI setzt 9 → nach `exchangeState` 9. **Negativkontrolle:** reiner Pull bleibt bei 7.
* Kein Schub ohne Quellenänderung: Konsument setzt lokal 3, GI unverändert → bleibt 3. GI ändert → Quellwert gilt.
* Spawn-Wert bleibt, bis die Quelle sich bewegt. Hot-Reload-Wert ebenso.
* Ref-Quelle: kein Pull bei Construct, keine Warnung bei null. Ref zuweisen → Schub im selben Frame. Ref umhängen auf ein Ziel mit **gleichem** Wert → trotzdem Schub (Identität). Ziel zerstört → ruht, Wert bleibt, genau eine Warnung.
* Erzeuger-Quelle mit `pullClass`, Erzeuger stirbt → ruht.
* Struct-Member-Bindung, Member umbenannt mit `formerNames` → trifft weiter.
* Kette A → B → C **ohne** Notify schließt sich in einem `exchangeState`. Zyklus A ↔ B mit wechselnden Werten → Abbruch nach 8 Runden, eine Warnung, kein Hänger.
* Typ falsch / Variable privat / `pullRef` keine Ref → ruht, **eine** Warnung für zehn Instanzen derselben Klasse.
* Container: Element ändern wird erkannt (prüft das verlagerte `valuesEqual`), die gebundene Kopie ist tief (Quelle danach ändern → Konsument unverändert bis zum nächsten Abgleich).
* JSON-Rundreise. Loader verwirft `bind` bei `replicated`, `Ref` ohne `bind`, bei Funktions-locals. Alte `CompiledVarInfo`-Aggregate kompilieren weiter.
* `exchangeState` liefert die Zahl der Schreibvorgänge (0 im Ruhezustand → kein `requestRedraw`).
* **Echte Hosts:** ein Headless-Lauf des Spiels (`HE_EXIT_AFTER_FRAMES`) mit einem gebundenen Widget, das den Wert ins Log schreibt. Das belegt die Verdrahtung in GA, nicht nur im Test-Rig.

**Schritt 3 (Meldung):**
* Wert ändern → genau ein `OnChanged_X(Old)` mit richtigem Old. Zweimal im Frame → einmal. Schreiben und Zurücksetzen → keinmal. Defaults/Construct/Pull/Spawn → keinmal.
* Bind-Schub meldet, auch der erste über eine Ref-Quelle.
* Listener mit `XChanged(Int)` bekommt den neuen Wert. Listener ohne Argument feuert. Listener mit falschem Typ → kein Aufruf, eine Warnung. Interpretiert und kompiliert gleich (prüft `CompiledEventInfo`).
* **Keine Weiterleitung:** L hört auf A, M hört auf L → M bekommt nichts, wenn A sich ändert.
* Handler zerstört Besitzer oder Quelle → kein Absturz (ASan/gmalloc-Lauf wie bei den Net-Session-Tests).
* Script-Abo: Lua-Instanz `hc.watch(0, "Score")` → `onChanged_Score(source, old)` kommt an. Script-Instanz zerstört → Abo weg, kein Aufruf ins Leere. Python gleich. C++-Probe für `onHcVariableChanged`.
* Replicated + Notify auf dem Client: Reihenfolge OnRep vor OnChanged (Net-Session-Testrig).

## 7. Bewusst nicht in diesem Thema

* **Zweiseitige Bindung.** Mit „Quelle gewinnt bei Änderung“ wäre sie machbar, bringt aber die Zyklusfrage in jeden Normalfall. Erst auf Nachfrage.
* **Element-Property-Bindung im Widget** (Text = Variable, wie UMG-Bindings). Mit Notify on Change ist das ein Einzeiler im Handler. Eine deklarative Bindung an Element-Eigenschaften wäre ein eigenes Thema, das auf `exchangeState` aufsetzen kann.
* **Absender in `<Var>Changed`** (zweites Dispatcher-Argument), siehe §4.3.
* **Dirty-Bit-Optimierung**, siehe §3.2.
* **Bind + Replicated** (§0.1).

## 8. Offene Fragen an das Review

1. **Datenmodell:** Bind To als Modus der Pull-Angabe (`bindTo` + `pullRef`, §2.1, empfohlen) oder eigene `bind*`-Felder (§2.2)? Diese Frage **blockiert Schritt 2**.
2. **Platz im Frame:** am Frame-Ende hinter `dispatchNetEvents` (§3.4, empfohlen: OnRep vor OnChanged, UI-Klicks im selben Frame, Bild aktuell) oder wie im Erstentwurf direkt nach `runtime().update`?
3. **Listener-Event:** `<Var>Changed` mit dem neuen Wert, Old nur beim Besitzer (§4.3, empfohlen)? Oder ein anderer Name?
4. **Script-API:** `hc.watch(target, var)` auf jede öffentliche Variable (empfohlen, Kosten nur bei Abo) oder nur auf Variablen mit Notify on Change? Ziel per Entity/0 = GI oder zusätzlich per Widget-Id?
5. **Editor-Pause:** Abgleich steht mit dem `uiLive`-Block still (§3.4, empfohlen), oder soll er im pausierten Editor weiterlaufen, damit gebundene Werte im Inspektor nachziehen?
6. **Lokales Überschreiben:** Bleibt ein lokal geschriebener Wert stehen, bis die Quelle sich ändert (§3.2, empfohlen), oder soll die Bindung jedes Frame durchsetzen („read-only gebunden“, dann wäre Set auf die Variable eine Warnung)?
