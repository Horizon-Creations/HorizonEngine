# Editor-Feinschliff: Gamification, Sound-Cues, visuelles Feedback, HC-Drag-and-Drop-Sounds

Thema 140, Schritt 1 (Bestand + Umfang). Stand: Zweig
`claude/editor-feinschliff-gamification-sound-cues-visuelles-hc-dnd-sounds` auf
main `469fa9f6`. Noch kein Feature gebaut; dieses Dokument legt fest, was die
Folgeschritte bauen und was ausdrücklich nicht.

## 0. Was es schon gibt

### Gamification und Töne (Themen 75 und 95)
Alles liegt in `src/HE_Editor/EditorRewards.{h,cpp}`. Der Kopfkommentar in
`EditorRewards.h` ist die Spezifikation, und die Leitplanken dort gelten auch hier:
**keine Punkte, keine Level, keine Bestenlisten, keine Achievement-Popups,
nichts Modales, nichts, was den Fokus nimmt oder die Aktion verzögert.**

- **Drei Momente** über `Rewards::fire()` (das einzige Tor auf `RewardsEnabled`):
  `Saved` (EditorUI.cpp: doSaveScene, Save-As, doSaveActiveTab, doSaveAll,
  Guard-Save), `BuildSucceeded` (Flankendetektor `pollBuild` über
  `BuildProgressDialog::outcome()`), `AssetsImported` (Import-Batch,
  TextureColourSpaceDialog, Content-Browser-Import, TextureViewer).
- **Feed** (ImGui-frei, getestet mit eigener Uhr): ein Moment pro Frame, gleiche
  Art verschmilzt, Rang Build > Import > Save, 2 s Abstand zwischen zwei Tönen,
  20 s zwischen zwei Speicher-Tönen, Ton nur wenn `toneWanted` (Schalter, Lautstärke,
  nicht während Play, Build-Töne nur ohne Editor-Fokus).
- **Vier Töne plus einer**, synthetisiert als mono PCM16 (kein Asset, keine Lizenz):
  `saveTickPcm16`, `chimePcm16` (Build), `buildFailedPcm16` (kein Moment, nur Ton),
  `importPopPcm16`. Regeln: ≤ 0,45 s, Spitze deutlich unter Vollaussteuerung,
  ≥ 4 ms Einblenden, Ende exakt 0, nichts unter ~600 Hz, A-Dur-Pentatonik.
- **Eigene UI-Audio-Engine** (`AppContext::uiAudioEngine`, `EditorApplication::m_uiAudio`),
  wird lazy in `pollBuild` geöffnet, Projekt-Mixer, Mute und `stopAll()` berühren sie
  nicht. `EditorSoundsMuted` schaltet genau diese Engine stumm.
- **Visuell**: Die Fußzeilenzeile („Saved“, „Build succeeded“, „Imported N assets“)
  mit schrumpfender Unterlinie, V1 gezeichnetes Häkchen, V2b Lichtkante, V3
  Zähler-Tick, V4 Tab-Häkchen beim Speichern, V5 Rahmen um frisch importierte
  Kacheln. Dazu Reduced Motion (folgt dem System über `EditorSystemMotion.*`).
- **Fortschritt**: „Ready · 3 builds today · 5 days in a row“ und ein Tooltip mit
  den letzten 7 Tagen. Gespeichert in GlobalState (`RewardsDay`,
  `RewardsBuildsToday`, `RewardsStreakDays`, `RewardsRecent`), bewusst nicht in
  EditorConfig.
- **Schalter** (Preferences ▸ Feedback, Katalog-Zeilen-Id `"rewards"`): 17 Felder,
  von `RewardsEnabled` bis `RewardsStreakTooltip`, jedes an den sechs Stellen
  verdrahtet (EditorConfig.h, Laden/Speichern in EditorApplication.cpp,
  EditorSettingsCatalog.cpp, EditorSettingsPanel.cpp inkl. Restore Defaults,
  EditorHelp.cpp).
- **Tests**: `tests/test_editor_rewards.cpp` (1346 Zeilen), Live-Prüfung
  `scripts/he_rewards_live.py`.

**Lücken gegenüber dem Auftrag:** Kompilieren hat weder Moment noch Ton. Fehler
(außer dem fehlgeschlagenen Build) haben keinen Ton. Git-Commit/Push und das
Ende des Tutorials sind keine Momente. Das HC-Drag-and-Drop ist komplett stumm.

### Drag and Drop im HorizonCode-Graph-Editor
Die Leinwand ist `GraphEditor::draw` (`src/HE_Editor/GraphEditor.cpp`). Sie wird
geteilt von **HorizonCode** (Level Script, Game Instance, HC-Class, UI-Widget-Graph,
alle über `HcGraphHost::buildModel`) sowie Material-, Partikel- und
Animator-Editor. Das Drag and Drop ist kein ImGui-DnD, sondern eigener Zustand
in `GraphEditor::State`:

| # | Geste | Stelle | Ende heute |
|---|-------|--------|------------|
| D1 | Kabel aus einem Pin ziehen | GraphEditor.cpp:533–557 (`linkSrcNode`) | — |
| D2 | Verbundenes Eingangs-Pin greifen = Kabel lösen | :543–554 (`linkGrab = true`) | — |
| D3 | Loslassen auf einem Pin | :1022–1034 → `model.connect` (HC: `Graph::connect`, sonst `connectWithConversion`) | true = verbunden, false = abgelehnt |
| D4 | Loslassen im Leeren (kein `linkGrab`) | :1035–1044 → öffnet `##ge_pindrag` | Menü: Auswahl erzeugt + verkabelt (:1285–1291) oder Menü geschlossen |
| D5 | Loslassen im Leeren nach `linkGrab` | :1035, Zweig fällt durch | Kabel bleibt gelöscht |
| D6 | Quick-Spawn-Taste während des Ziehens | :997–1020 | Knoten erzeugt + verkabelt, oder `created == 0` |
| D7 | Loslassen auf einem Pin desselben Knotens | :1027 `tn != linkSrcNode` | nichts passiert |
| D8 | Variable/Element aus der Seitenliste auf die Leinwand (ImGui-Payload) | Quelle LevelScriptPanel.cpp:340–347, :396; UIEditorPanel.cpp:5570, :5609. Ziel GraphEditor.cpp:208–216 | `onDrop` → Get/Set-Popup; ohne Ziel losgelassen = verfällt |
| D9 | Knoten verschieben | :885, :948–981 | kein Ziel |

**Wichtig:** Während des Kabelziehens wird `pinAt(mouse)` nur beim Loslassen
abgefragt (:1025). Ein Hover-Zustand „über gültigem/ungültigem Pin“ existiert nicht,
und HorizonCode hat keine Prüfung ohne Seiteneffekt: `Graph::connect` verändert
den Graphen. Es gibt nur die Bausteine `pinRanges`, `dataPinDescOf`,
`canConvertPinType`, `conversionNodeFor` (HorizonCode.h:620–945).

### Handbuch und Tooltips
- Tooltips **und** die im Editor erzeugte „Settings Reference“ kommen aus
  `EditorHelp.cpp` (Einträge `"Preferences/Feedback/…"`, Seitenregel
  `{ "Preferences/Feedback/", "editor-settings", …, "Feedback" }`). Der
  Abdeckungstest liegt in `tests/test_editor_help.cpp`.
- Das Fließtext-Handbuch (`EditorDeps/Docs/he-docs.json`) wird von
  `scripts/build_docs_bundle.py` aus dem **zweiten Repository**
  `../Website/HorizonEngineDocs/*.html` erzeugt. Dort erwähnt der Abschnitt
  „Preferences“ (`editor.html#preferences`) das Feedback heute gar nicht, und
  `horizoncode.html:131` beschreibt das Kabelziehen ohne Töne.

## 1. Richtung 1: neue Auslöser und Belohnungen

Neue Momente laufen durch dieselbe `fire()`/Feed-Mechanik und bekommen damit die
Fußzeilenzeile, V1/V2b, die Merge-, Rang- und Ton-Regeln geschenkt.

| Moment | Zeile | Auslöser (nur Benutzeraktion, nur Erfolg) | Rang | Tally | Fokus-Regel |
|--------|-------|--------------------------------------------|------|-------|-------------|
| **M4 `CompiledClean`** | „Compiles clean“ | Compile-Knopf im HC-Graph: `runCompileCheck` LevelScriptPanel.cpp:189 (Zweig `compileOk = true`, :256) und UIEditorPanel.cpp:7105–7127 | über Save, unter Import | zählt als „benutzt“ (Serie), **nicht** als Build | **keine**: der Compile läuft synchron auf Klick, ein Fokus-Gate würde ihn praktisch immer stumm machen |
| **M5 `Committed`** | „Committed“ / „Committed and pushed“ / „Pushed“ | Source-Control-Panel: Commit (SourceControlPanel.cpp:742), Push (:532). Erkannt als Flanke `busy → idle` ohne `lastError` nach einer **im Panel** ausgelösten Anfrage (eigener Anfrage-Zähler in GitController, ImGui-frei testbar). Pull/Fetch sind kein Moment. | gleich Build | zählt als „benutzt“, neuer Tageszähler `commits` nur im 7-Tage-Tooltip | wie Build (Push kann dauern, man schaut weg) |
| **M6 `TourFinished`** | „Tutorial complete“ | TutorialPanel.cpp:767, der letzte Schritt ist erledigt (`tut::finished(tut::advance(s_cursor))`) und wird bestätigt. **Nicht** pro Schritt: der hat schon eigenes Feedback (`s_doneTimer`, Auto-Advance). Einmal pro Durchlauf. | gleich Build | zählt als „benutzt“ | keine |

Hinweis für M5: Die Flanke `busy → idle` hat dieselbe Falle, die der Kommentar zu
`m_cloneBusy` in GitController.h beschreibt. Nach `pump()` kann `busy()` schon
„idle“ sagen, während das Ergebnis noch in der Warteschlange steht. Deshalb
`busy()` **vor** dem Pumpen lesen, wie es der Clone-Pfad tut.

Neuer Ton ohne Moment (wie `buildFailedPcm16`):
- **`CompileFailed`**: Compile-Knopf mit Fallback/Fehler (LevelScriptPanel.cpp:239,
  UIEditorPanel analog). Ohne Fokus-Gate (siehe M4). Keine Zeile; der Graph springt
  heute schon zum fehlerhaften Knoten.
- **`Problem`**: eine neue `NoteLevel::Problem`-Meldung im `NotificationStore`
  (Flanke auf die Anzahl ungelesener Problem-Einträge, im UI-Thread). Eigener,
  **langer** Abstand (Vorschlag 30 s), weil `attachToEngineLog` jedes
  `HE_LOG_ERROR` dorthin weiterleitet. Klingt nur, wenn der Editor *nicht* im
  Vordergrund ist; im Vordergrund reicht der Puls an der Glocke (V8). Standard:
  Schalter an, steht aber unter `RewardsSound` (aus).
  `NotificationStore` kennt nur `unseenCount()` (alle Stufen) und `snapshot()`.
  Für die Problem-Flanke entweder einen kleinen Zähler je Stufe ergänzen oder den
  Snapshot mitbenutzen, den NotificationBar ohnehin pro Frame holt. Keinen zweiten
  Snapshot ziehen.

Rang im Feed (Regel 3), von oben nach unten: Build = Commit = Tutorial >
Import > Compile > Save. Bei gleichem Rang und anderer Art ersetzt der neuere
Moment die Zeile (wie heute ein höherer).

Belohnungen (also Anzeige, nicht Mechanik):
- Die neuen Momente erscheinen in der Fußzeile. Der 7-Tage-Tooltip zeigt zusätzlich
  Commits pro Tag (`RewardsRecent` bekommt ein optionales drittes Feld; alte Einträge
  bleiben lesbar, `parseRecent` testen).
- **Bewusst nicht:** Meilensteine („100. Build“), Abzeichen, Tagesziele,
  Fortschrittsbalken. Das ist die Grenze aus Thema 75/95 (siehe Kopf von
  EditorRewards.h) und bleibt sie.
- Nicht als Moment: Material-Recompile (läuft bei jeder Änderung von selbst),
  Play-Start/-Stop, Autosave, alles, was ein MCP-Client oder Skript auslöst.

Neue Schalter (sechs Stellen + EditorHelp-Eintrag je Label):
`RewardsMomentCompile` (an), `RewardsMomentCommit` (an), `RewardsMomentTutorial` (an).
Sie stehen unter dem Master und schalten **nur den Moment**; gezählt wird
weiterhin, solange der Master an ist (wie `RewardsShowProgress`).

## 2. Richtung 2: Sound-Cues

Alle neuen Töne folgen den Klangregeln aus Thema 95 (siehe §0) und hängen an der
vorhandenen UI-Audio-Engine, an `RewardsVolume` (quadratisch) und an
`EditorSoundsMuted`. Lautstärke und Abschalten bleiben in Preferences ▸ Feedback,
jeder Ton hat dort einen „Preview“-Knopf.

| Ton | Funktion (neu) | Klang | Schalter (unter `RewardsSound`) | Standard |
|-----|----------------|-------|----------------------------------|----------|
| Kompiliert | `compileOkPcm16` | C#6 → E6, kurz (≈ 0,18 s), leiser als der Build-Chime | `RewardsSoundCompile` | an |
| Kompilieren fehlgeschlagen | `compileFailedPcm16` | E6 → C#6 abwärts, weicher Anschlag, kein Summer | `RewardsSoundCompileFailed` | an |
| Commit/Push | `commitPcm16` | A5 → C#6 → E6 Arpeggio, 0,3 s | `RewardsSoundCommit` | an |
| Tutorial fertig | `tourDonePcm16` | Build-Chime + F#6 als dritte Note | `RewardsSoundTutorial` | an |
| Problem | `problemPcm16` | B5 zweimal kurz, gedämpft | `RewardsSoundProblem` | an |
| Speichern, Build, Build fehlgeschlagen, Import | vorhanden | unverändert | vorhanden | vorhanden |

`RewardsSound` selbst bleibt **aus**: Eine frische Installation hört weiterhin nichts.
Die Momenttöne (Compile, Commit, Tutorial) laufen durch den Feed (2-s-Abstand,
Rang). Compile-Failed und Problem laufen wie Build-Failed direkt über `takeTone()`,
Problem zusätzlich mit eigenem 30-s-Abstand.

## 3. Richtung 4: Drag-and-Drop-Sounds im HC-Graph

### Grundsatz
Das Verhalten des Drag and Drop ändert sich nicht. Die Cues hängen sich an die
vorhandenen Stellen. Dazu kommen **ein optionaler, rein beobachtender Callback**
im Modell und eine Hover-Abfrage, die nur liest:

```cpp
// GraphEditor::Model, optional. Unset (Material/Partikel/Animator) = kostet nichts.
enum class DragCue { Pickup, OverValid, OverInvalid, Drop, Cancel };
std::function<void(DragCue)> onDragCue;
// optional: darf dieses Kabel verbinden? Nur lesen, nie den Graphen ändern.
std::function<bool(int outNode, int outPin, int inNode, int inPin)> canConnect;
```

Gesetzt werden beide nur in `HcGraphHost::buildModel`. Während ein Kabel in der
Hand ist, ruft `GraphEditor::draw` zusätzlich zum Loslassen **jeden Frame**
`pinAt(mouse)` auf und meldet nur die **Flanke** (anderes Pin betreten), nicht jeden
Frame. Die Abfrage ist beobachtend; `connect` wird weiterhin nur beim Loslassen
gerufen.

### Gültigkeit ohne Seiteneffekt
`canConnect` im HC-Host ist ein **reines Prädikat** aus den vorhandenen Bausteinen:
verschiedene Knoten, Ausgang → Eingang, Exec ↔ Exec, gleicher Typ samt
`typeName`, sonst `canConvertPinType`, sonst `conversionNodeFor` (dann würde
`connectWithConversion` greifen, also gültig), plus die Sonderfälle ForEach
(generisch, bis verkabelt) und Reroute (nimmt jede Form an). Die
Hidden-Node-Liste (`h.menus->addExcluded`) gilt auch hier.
**Kein Probe-Connect auf einer Graph-Kopie zur Laufzeit.** Die Kopie ist das
**Test-Orakel**: Fixture-Graph, alle Pin-Paare aufzählen, prüfen
`canConnect(a, b) == m.connect(a, b)` auf einer Kopie. So kann das Prädikat nicht
unbemerkt von `connect` wegdriften.

### Zuordnung Geste → Cue

| Geste | Cue |
|-------|-----|
| D1 Kabel aus Pin aufnehmen | **Pickup** |
| D2 verbundenes Kabel greifen (lösen) | **Pickup** |
| Kabel in der Hand, ein Pin betreten, `canConnect` true | **OverValid** |
| …`canConnect` false, oder Pin desselben Knotens, oder Richtung falsch | **OverInvalid** |
| Pin verlassen, leere Leinwand | kein Ton |
| D3 Loslassen auf Pin, `connect` true | **Drop** |
| D3 Loslassen auf Pin, `connect` false (abgelehnt, auch nach Konversion) | **Cancel** |
| D7 Loslassen auf Pin desselben Knotens | **Cancel** |
| D4 Loslassen im Leeren → Menü öffnet | kein Ton (die Geste läuft weiter) |
| D4 Menü: Eintrag gewählt (`created != 0`) | **Drop** |
| D4 Menü geschlossen ohne Auswahl (Flanke `IsPopupOpen("##ge_pindrag")` → zu, ohne `created`) | **Cancel** |
| D5 gelöstes Kabel im Leeren losgelassen (= Kabel entfernt) | **Cancel**: das Kabel verschwindet, der Abbruch-Klang passt zum Bild. Bewusst kein eigener „Löschen“-Ton. |
| D6 Quick-Spawn-Taste, `created != 0` | **Drop** |
| D6 Quick-Spawn-Taste, `created == 0` | **Cancel** |
| D8 Variable/Element aus der Liste aufnehmen (erster Frame von `BeginDragDropSource`, LevelScriptPanel/UIEditorPanel) | **Pickup** |
| D8 Payload über der Leinwand, Typ in `dropPayloads` | **OverValid** (Flanke, im Ziel-Block :208 per `GetDragDropPayload()->IsDataType`) |
| D8 Payload über der Leinwand, fremder Typ (z. B. `HE_ASSET_PATH`, `HE_ENTITY`) | **OverInvalid** |
| D8 `onDrop` gefeuert | **Drop** (das Get/Set-Popup danach ist eine eigene Auswahl) |
| D8 Payload-Ziehen endet ohne Zustellung | **Cancel** (Quelle war im letzten Frame aktiv, jetzt nicht, kein `onDrop`) |
| D9 Knoten verschieben | **kein Ton**: Verschieben hat kein Ziel, also gibt es kein gültig/ungültig, und jeder Knoten-Zug würde klicken. |

### Klänge (`dragPickupPcm16` … `dragCancelPcm16`)
Alle **hörbar leiser und kürzer als der Save-Tick** (Spitze ≤ 0,6 × Tick, ≤ 60 ms),
gleiche Regeln (≥ 600 Hz, A-Dur-Pentatonik, Ende auf 0):
- Pickup: kurzes Aufwärts-Blip E6 → A6, 35 ms
- OverValid: heller Tick C#7, 20 ms, sehr leise
- OverInvalid: gedämpfter Tick B5 mit etwas Rauschen, 25 ms (kein Summer)
- Drop: „Snap“ E6 mit Klick-Transient, 40 ms
- Cancel: Abwärts-Blip A6 → E6, 45 ms, weicher Anschlag

### Regeln für die DnD-Cues
- **Keine Momente**: keine Fußzeile, nichts gezählt, nicht durch den Feed (dessen
  2-s-Abstand würde Pickup → Hover → Drop verschlucken).
- Eigener kleiner Taktgeber `DragCues` (ImGui-frei, neue Datei
  `src/HE_Editor/EditorDragCues.{h,cpp}`): Hover-Cues nur auf die Flanke, Untergrenze
  50 ms zwischen zwei Hover-Cues (Zickzack über eine Pinreihe), Drop/Cancel immer.
  Ein Reward-Ton aus dem Feed hat Vorrang: Spielt im selben Frame ein Momentton,
  entfällt der DnD-Cue.
- Tor: Master, `RewardsSound`, `RewardsSoundDragDrop` (neu, an), Lautstärke > 0,
  nicht stummgeschaltet, **nicht während Play-in-Editor** (Regel 6). Kein Fokus-Gate,
  man zieht ja gerade.
- Gespielt über `uiAudioEngine`. Das Gerät wird wie gehabt in `pollBuild`
  vorgeöffnet; `uiSoundPossible` deckt den neuen Schalter mit ab.

## 4. Richtung 3: visuelles Feedback

Alles nur mit `ImDrawList` im Marken-Theme (`EditorTheme.h`, Grün aus
`Rewards`/`T::kGood`, Warnfarbe `kWarn`), kein Glyph, kein Icon-Font, keine
Layoutverschiebung, kein Fenster, **nichts in `src/HE_Editor/vendor`**. Reduced
Motion gilt überall: Bewegungen entfallen, Farbwechsel und Ausblenden bleiben.

| Id | Effekt | Ort | Schalter (neu) |
|----|--------|-----|----------------|
| V6 | **Pin-Hinweis beim Kabelziehen**: Pins, die `canConnect` als gültig meldet, bekommen einen dünnen grünen Ring. Das Pin unter dem Cursor bekommt einen vollen Ring, grün bei gültig, `kWarn` bei ungültig. Gleiches Prädikat wie der Ton. | GraphEditor (nur mit gesetztem `canConnect`) | `RewardsGraphDragHints` (an) |
| V7 | **Snap-Puls beim Ablegen**: Das neue Kabel leuchtet 0,4 s auf und blendet aus. Bei **Cancel** läuft das Geisterkabel 0,15 s zum Pin zurück, statt zu verschwinden (Reduced Motion: nur Ausblenden). | GraphEditor | `RewardsGraphDragHints` |
| V8 | **Glocken-Puls**: Bei einer neuen Problem-Meldung pulsiert die Footer-Glocke einmal (Ring, 0,6 s). Kein Blinken, kein Wiederholen. | NotificationBar.cpp | `RewardsBellPulse` (an) |
| V9 | **Compile-Readout**: Das vorhandene 6-s-Readout im HC-Graph-Header (LevelScriptPanel.cpp:1462, UIEditorPanel.cpp:7082) zeichnet bei Erfolg das V1-Häkchen (`drawCheckMark`) statt des statischen Icons. Bei Fehler pulsiert der Fehlerknoten (`host.errorNode`) einmal. | LevelScriptPanel, UIEditorPanel, HcGraphHost | `RewardsCheckMark` (vorhanden) bzw. `RewardsGraphDragHints` |
| — | Neue Momente M4–M6 nutzen die vorhandene Zeile + V1/V2b/V3 | Fußzeile | vorhanden |

## 5. Handbuch und Tooltips

- **EditorHelp.cpp**: ein Eintrag je neuem Label unter `"Preferences/Feedback/…"`.
  Das sind gleichzeitig der Tooltip, das F1-Ziel und die Zeile in der
  Settings Reference. `tests/test_editor_help.cpp` fällt sonst.
- **Website-Handbuch** (zweites Repository `../Website/HorizonEngineDocs`):
  - `editor.html#preferences`: ein Absatz „Feedback“ (Momente, Töne, Lautstärke,
    Stummschalten, Reduced Motion, Fortschritt). Das fehlt heute ganz.
  - `horizoncode.html` beim Kabelziehen (:131): ein Satz zu den DnD-Tönen und zu den
    Pin-Hinweisen (V6).
  - Danach `scripts/build_docs_bundle.py` laufen lassen und
    `EditorDeps/Docs/he-docs.json` in diesem Repo committen. Den **Deploy** der
    Website macht nur der Mensch (vorher bestätigen).
- Kopfkommentar in `EditorRewards.h` um M4–M6, die neuen Töne und den Verweis auf
  `EditorDragCues.h` erweitern. Jede neue Hook-Stelle bekommt die einzeilige
  „Reward moment (EditorRewards.h)“- bzw. „Drag cue (EditorDragCues.h)“-Notiz.

## 6. Tests

- `tests/test_editor_rewards.cpp`: neue Momente im Feed (Rang, Merge), Tally
  (Compile/Commit/Tour zählen als benutzt, nicht als Build), `parseRecent` mit und
  ohne Commit-Feld, Klangregeln für alle neuen PCM-Funktionen (Länge, Spitze,
  Einblenden, Ende 0), `toneWanted` für die neuen Töne, Problem-Abstand,
  Commit-Flanke (Anfrage-Zähler + busy → idle, Fehler = kein Moment).
- **neu** `tests/test_editor_drag_cues.cpp`: `DragCues`-Taktgeber mit Testuhr
  (Flanke, 50-ms-Untergrenze, Vorrang des Feed-Tons, stumm bei Play), DnD-Klänge
  leiser als der Tick, **Prädikat-Orakel** `canConnect == connect-auf-Kopie` über
  einen Fixture-Graph mit Exec, Daten, Enum, Array, ForEach und Reroute.
- `tests/test_graph_editor_keys.cpp` (treibt `GraphEditor::draw` headless):
  Cue-Folge je Geste D1–D8 aus der Tabelle in §3 **und** dass Links, Auswahl und
  Rückgabewert mit und ohne gesetzten Callback **identisch** sind. Das ist der
  Beleg, dass das DnD-Verhalten unverändert ist.
- `tests/test_editor_help.cpp`: Abdeckung der neuen Labels (läuft automatisch).
- `scripts/he_rewards_live.py` um die neuen Schalter erweitern (Live-Prüfung im
  Release-Build).
- Hinweis: he_tests kompiliert einige Editor-TUs mit; neue Symbole aus Panels
  brauchen ggf. Einträge in `tests/CMakeLists.txt` oder in den Link-Stubs.

## 7. Nicht Teil des Themas

- Knoten-Verschieben (D9) ohne Ton (Begründung in §3).
- Material-, Partikel- und Animator-Leinwand: Der Callback bleibt dort ungesetzt.
  Später möglich, aber nicht hier.
- Feed-Regeln 1–6 bleiben, wie sie sind. Einzige Erweiterung: Regel 3 bekommt den
  Gleichrang-Fall aus §1 (heute hat jede Art einen eigenen Rang, den Fall gibt es
  also noch nicht). Die vorhandenen Töne und ihre Standardwerte bleiben
  unverändert.
- Keine Engine-Features, keine Renderer-Backends, kein HorizonCode-Laufzeitverhalten
  (`Graph::connect` & Co. werden nur gelesen, nie geändert).
- Keine Vendor-Dateien (ImGui, ImGuizmo; die `HE-PATCH`-Stellen bleiben).
- Keine Punkte, Level, Abzeichen, Meilensteine, Tagesziele.

## 8. Vorschlag für die Folgeschritte

1. **Momente + Töne**: M4–M6, Compile-Failed, Problem, neue PCM-Funktionen,
   Schalter an den sechs Stellen, EditorHelp, Tests in test_editor_rewards.
2. **HC-DnD-Cues**: `EditorDragCues.{h,cpp}`, Callback + Hover-Flanke in
   GraphEditor, `canConnect`-Prädikat im HC-Host, Payload-Hooks in
   LevelScriptPanel/UIEditorPanel, Tests inkl. Orakel und Verhaltensgleichheit.
3. **Visuell**: V6–V9, Reduced Motion, Tests der Uhr-Teile.
4. **Handbuch**: Website-HTML + Bundle neu erzeugen, EditorRewards.h-Kopf.
5. **Vollbau + volle Testsuite + Release-Live-Prüfung** (`he_rewards_live.py`), Befund.
6. Pull Request (nicht selbst mergen).

Schritte 1 und 2 sind unabhängig voneinander. Beide fassen `EditorConfig.h`,
`EditorSettingsCatalog.cpp`, `EditorSettingsPanel.cpp` und `EditorHelp.cpp` an,
deshalb besser nacheinander als parallel.

## 9. Offene Fragen an den Menschen (mit Empfehlung)

1. **Standardwerte**: Alle neuen Einzeltöne stehen auf „an“, `RewardsSound` bleibt
   **aus**. Wer Sound einschaltet, hört dann auch DnD und Compile. *Empfehlung: so.*
2. **Problem-Ton** nur mit Editor im Hintergrund? *Empfehlung: ja*, im Vordergrund
   reicht der Glocken-Puls.
3. **Gelöstes Kabel ins Leere** = Cancel-Ton (statt eines eigenen Lösch-Tons)?
   *Empfehlung: ja.*
4. **DnD-Cues auch für Material/Partikel/Animator** später? *Empfehlung: erst nach
   Rückmeldung zum HC-Klang.*

## 10. Stand nach Schritt 3 (Sound-Cues + visuelles Feedback)

Umgesetzt (EditorRewards.h ist die Spezifikation, Abschnitte „The tones“ und
„The visual cues“):

- **Töne**: `compileCleanPcm16`, `compileFailedPcm16`, `commitPcm16`,
  `tourDonePcm16`, `problemPcm16`; `hasTone` ist für alle sechs Momente wahr.
  Compile-Failed ohne Moment über `sound()` bzw. `postSound()` (LevelScriptPanel
  hat keinen AppContext). Problem über `ProblemWatch` auf dem Snapshot, den die
  Glocke ohnehin holt (neuestes `whenMs` einer Problem-Meldung), nur ohne Fokus,
  eigener Abstand `kProblemToneGapSec` = 30 s.
- **Schalter**: `RewardsSoundCompile`, `…CompileFailed`, `…Commit`,
  `…Tutorial`, `…Problem` (unter Success Sound, je mit Preview) und
  `RewardsProblemPulse`. Abweichung vom Plan: statt `RewardsBellPulse` ein
  Schalter **Problem Pulse** für V8 und den Fehlerknoten-Puls aus V9 (beides
  „schau hier hin“).
- **V8**: Ring um die Footer-Glocke bei einer neuen Problem-Meldung
  (`ringAt`, `drawProblemRing`), Reduced Motion: nur Ausblenden.
- **V9**: Compile-Readout schreibt das V1-Häkchen (`compileCheck`,
  `Bar::readout` gibt die Icon-Mitte zurück); der rote Halo des
  Fehlerknotens hellt einmal auf (`errorPulse`, `HcGraphHost::Host::errorPulse`).
- **Nicht hier, sondern Schritt 4**: V6 (Pin-Ringe beim Kabelziehen) und V7
  (Snap-Puls/Geisterkabel) brauchen `canConnect` und die Hover-Flanke aus dem
  DnD-Schritt, ebenso die `drag*Pcm16`-Töne.
- **Handbuch**: Absatz „Feedback“ / „Feedback sounds“ in
  `Website/HorizonEngineDocs/editor.html#preferences` (dort lokal committet,
  nicht gepusht, kein Deploy). Ins Editor-Bundle `he-docs.json` wurde **nur
  dieser Abschnitt** übernommen: das Bundle ist neuer als der Website-Stand
  (Abschnitte aus anderen Zweigen), ein komplettes Neuerzeugen hätte Inhalt
  zurückgedreht.

Bildbelege (`scripts/he_uishot.py`, Szenen „ui shot: …“ in
`tests/test_editor_rewards.cpp`):

| Effekt | Bild |
|--------|------|
| V8 Ring um die Glocke | `img/editor-feinschliff/rewards_bell_problem_ring.png` |
| V9 Häkchen wird geschrieben / fertig | `img/editor-feinschliff/rewards_compile_readout_writing.png`, `…_written.png` |
| V9 Fehlerknoten auf dem Puls / in Ruhe | `img/editor-feinschliff/rewards_error_node_pulse.png`, `…_rest.png` |

## 11. Stand nach Schritt 4 (Drag-and-Drop-Töne)

Umgesetzt wie in §3, die Spezifikation steht im Kopf von
`src/HE_Editor/EditorDragCues.h`:

- **GraphEditor**: `Model::onDragCue` und `Model::canConnect`, beide optional;
  Flankenzustand in `State::cue*`. Die Gesten D1–D8 melden die Cues aus der
  Tabelle in §3, D9 (Knoten verschieben) bleibt stumm. Die D8-Pickup/Cancel
  sieht die Leinwand selbst über `GetDragDropPayload()`; LevelScriptPanel und
  UIEditorPanel bleiben dafür unberührt.
- **HcGraphHost**: setzt beide. **Abweichung vom Plan:** `canConnect` ist kein
  eigenes Prädikat, sondern derselbe `hostConnect` auf einer Graph-Kopie,
  gefragt nur beim Pin-Wechsel, nicht pro Frame. So kann „passt“ nicht von dem
  abweichen, was der Drop wirklich tut, und das Orakel aus §6 erübrigt sich.
- **EditorRewards**: `postDragCue` (Warteschlange, gespielt in `pollBuild`),
  `dragCueWanted`, `DragCues::Gate` (Hover 50 ms, gleicher Cue 50 ms),
  `dragCuePcm16` (≤ 60 ms, Spitze ≤ 0,6 × Tick), ein Ton aus dem Feed im selben
  Frame hat Vorrang. Testschnittstelle `setDragCueProbe`.
- **Schalter** `RewardsSoundDragDrop` („Drag and Drop Sound“, an, unter Success
  Sound) mit Preview der fünf Cues in Folge.
- **Tests**: `test_graph_editor_keys.cpp` (Cue-Folge je Geste und gleiches
  Verhalten mit und ohne Callback), `test_editor_rewards.cpp` (Klangregeln,
  Gate, Schalter, Probe bis zum UI-Audio-Engine-Ausgang).
- **Handbuch**: `horizoncode.html#graphs` (Website, lokal committet, nicht
  gepusht, kein Deploy), ins Bundle nur dieser Abschnitt übernommen.
- **Offen:** V6/V7 (Pin-Ringe, Snap-Puls) sind optisch und gehören nicht zu
  Schritt 4. Die Bausteine dafür (`canConnect`, `State::cueNode`) liegen jetzt bereit.
