# Widget-Animationstool: Visible und Enabled als Tracks (Thema 108, Schritt 1)

Konzept und Diagnose, kein Code. Stand: main `2bde99e3`. Die Anim-Quellen aus
Thema 107 (`UIWidgetAnim.*`, `UIEditorPanel.cpp`, `WidgetManager.cpp`) sind dort
schon drin; der 107-Zweig unterscheidet sich von main nur noch in Doku, Tests und
Fixtures.

## Kurzfassung

- **Kein neuer Property-Typ nötig.** `UIPropType::Bool` gibt es seit jeher,
  `UIAnimKey::value` ist ein `UIPropValue`, die Clip-Serialisierung geht durch
  `uiPropValueToJson` und kann Bool schon. Visible (`"Visible"`) und Enabled
  (`"Enabled"`) sind Basis-Properties **jedes** Elements (`uiBaseProperties()`).
- Laufzeit (`WidgetManager::tick`) und Designer-Vorschau (`ScrubPreview`)
  schreiben Samples generisch per `setPropAny` mit Typprüfung, und würden
  Bool-Samples ohne Änderung durchreichen.
- Es fehlen genau drei Dinge:
  1. **Step-Auswertung** in `between()` (heute falsch für Bool, siehe unten),
  2. der **Typ-Filter** im Add-Track-Popup lässt nur Float/Color/Vec2 durch,
  3. der **Key-Editor** hat für Bool keinen Zweig (zeigt „cannot be interpolated").
- Aufwand: klein bis mittel, etwa ein Arbeitstag inklusive Tests, in drei Schritten.

## 1. Wie ein Property-Track heute aufgebaut ist

| Stelle | Was |
|---|---|
| `src/HE_Core/include/UIWidget/UIWidgetAnim.h` | `UIAnimKey { time, UIPropValue value, UIEase ease }`, `UIAnimTrack { element, std::string prop, keys }`, `UIAnimClip { name, duration, loop, tracks }` |
| `src/HE_Core/src/UIWidget/UIWidgetAnim.cpp:146` | `between(a, b, k)`: Lerp für Float, Vec2, Color (Color geklemmt). **`default: return b.value;`** (Z. 163) |
| `UIWidgetAnim.cpp` `uiAnimEvaluate` | sucht `prev` (letzter Key ≤ t) und `next`; vor dem ersten Key gilt der erste, nach dem letzten der letzte; zwischen zwei Keys `between(prev, next, ease(next))` |
| `src/HE_Core/src/UIWidget/UIWidgetTree.cpp:2060ff / 2180ff` | Speichern/Laden: `{"elem","prop","keys":[{"t","v","ease"?}]}`, `v` über `uiPropValueToJson` (Typname steht im JSON) |
| `src/HE_Core/src/UIWidget/WidgetManager.cpp:2396` | Laufzeit: `uiAnimEvaluate` → `setPropAny`, nur wenn `getPropAny(prop).type == sample.type`; bei `restore` am Ende `restoreOne` (generisch, Z. 2231) |
| `src/HE_Editor/UIEditorPanel.cpp:4403` | `ScrubPreview`: gleiche generische Überlagerung im Designer |
| `UIEditorPanel.cpp:3205–3232` | **Add Track**: Liste = `sel->allProperties()`, gefiltert auf `Float/Color/Vec2` (Z. 3213). Kein festes Enum. Opacity, Position, Size, Rotation, Tint sind einfach die Float/Vec2/Color-Properties, die dabei übrig bleiben |
| `UIEditorPanel.cpp:2598` `drawKeyEditor` | Wert-Feld je Typ: DragFloat (mit Property-Range), ColorEdit4, DragFloat2; sonst Text „(this key's type cannot be interpolated)"; dazu Ease-Combo und Delete Key |
| `UIEditorPanel.cpp:~3120` | Rechtsklick auf Key: Ease-Auswahl |

Die „Property-Liste" ist also datengetrieben: jede Property, die ein Element über
`allProperties()` meldet und die den Typ-Filter besteht, ist animierbar.

## 2. Gibt es Infrastruktur für boolsche/diskrete Werte?

Teilweise. Vorhanden:

- Werttyp (`UIPropType::Bool`, `UIPropValue::ofBool`), JSON (`"type":"bool"`),
  `uiPropValueCoerce` nach Bool.
- Generischer Schreibpfad in Laufzeit und Designer, Restore-Mechanik
  (`originals` merkt sich den Vorher-Wert generisch).

Fehlt bzw. ist falsch:

- **`between()` springt zu früh.** Für jeden nicht interpolierbaren Typ liefert
  der `default`-Zweig `b.value`, also den Wert des *nächsten* Keys, sobald die
  Zeit auch nur ein Stück hinter dem vorigen Key liegt (sogar genau *auf* dem
  vorigen Key, weil `prev` per `<=` gefunden wird und `next` dann schon gesetzt
  ist). Beispiel Visible: Keys `0.0 = true`, `0.5 = false`. Heute: ab `t = 0.0`
  unsichtbar. Gewollt: bis `0.5` sichtbar, ab `0.5` unsichtbar. Das trifft heute
  niemanden, weil der Editor keine Bool-Tracks anlegen lässt; für das Feature
  muss es aber korrigiert werden.
- Kein Bool-Zweig im Key-Editor, Bool wird im Add-Track-Popup herausgefiltert.

## 3. Wie der Editor erkennt, ob ein Widget Enabled/Clickable kann

Es gibt **drei** verwandte Felder, die man auseinanderhalten muss:

| Feld / Property | Wirkung | Reichweite |
|---|---|---|
| `enabled` / `"Enabled"` | gedimmt gezeichnet (`kUIDisabledDim`) **und** inert: kein Hover, Klick, Drag, Fokus, Tastatur | **Teilbaum** (`uiElementEffectiveEnabled` läuft die Eltern hoch) |
| `hitTestable` / `"Hit Testable"` | für die Maus durchlässig, sonst keine Optik | **nur das Element selbst** (WidgetManager.cpp:2816) |
| `visible` / `"Visible"` | nicht gezeichnet, kein Hit, kein Fokus; **Layout-Boxen überspringen unsichtbare Kinder** (Geschwister rücken nach) | Teilbaum (`uiElementEffectiveVisible`) |

Alle drei sind Basis-Felder, jedes Element hat sie. Die Fähigkeit „reagiert
überhaupt auf Eingabe" meldet die virtuelle Methode `UIElement::interactive()`:
fest `true` bei Button, CheckBox, Slider, TextInput, ComboBox, ListView, TabBox,
Splitter, Accordion, DatePicker, ColorPicker, RadioButton, TreeView; bei `UIText`
**instanzabhängig** (`selectable` oder Rich-Text mit Link). Panel, Image,
ProgressBar usw. sind `false`.

Wichtig für den Filter: Weil `enabled` Teilbaum-Semantik hat, ist Enabled auch
auf **Containern** sinnvoll („ganzes Menü während des Einblendens sperren"). Ein
Filter nur über `interactive()` würde genau diesen Fall verbieten.

Nebenbefunde, die die Motivation schärfen:

- Render Opacity ≤ 0,001 blockiert den Hit-Test und den Fokus schon heute
  (WidgetManager.cpp:2822, 5289). „Ausfaden = nicht mehr klickbar" gibt es also
  bereits. Was Visible/Enabled als Track zusätzlich bringen: Visible nimmt das
  Element aus dem Layout und der Tab-Reihenfolge, Enabled sperrt ein **sichtbares**
  Element sichtbar gedimmt (z. B. Button erst nach dem Einblenden freigeben).
- Das Aktivieren eines fokussierten Elements prüft Enabled und Visible bei
  jedem Ereignis (WidgetManager.cpp:5423). Ob auch das laufende Tippen in ein
  schon fokussiertes TextInput das prüft, ist im Umsetzungsschritt nachzusehen
  (Risiko: per Clip deaktiviertes Feld nimmt weiter Zeichen an).

## 4. Vorschlag Datenmodell

**Empfehlung: Interpolationsart aus dem Werttyp ableiten, kein neues Feld.**

- Float, Vec2, Color: interpoliert wie bisher.
- Alles andere (Bool, und nebenbei Int/String aus handeditierten Dateien):
  **Step/Hold**. Zwischen zwei Keys gilt der Wert des vorigen Keys, genau auf
  einem Key gilt dieser Key.
- Umsetzung: in `between()` der `default`-Zweig `return a.value;` statt
  `b.value`. Das ist bewusst auch für Int/String so (Halten statt Vorspringen);
  heute legt der Editor solche Tracks nicht an, betroffen wären nur
  handeditierte Dateien. Der Typ-Mismatch-Zweig (`a.type != b.type`) kann
  `b.value` behalten oder ebenfalls auf `a.value` gehen; Vorschlag: auch `a`,
  dann ist die Regel „nicht interpolierbar = halten" ohne Ausnahme.
- Ease wird bei Step-Tracks ignoriert (bleibt `Linear` im File, wird also nicht
  geschrieben).
- Keine Formatänderung: Bool-Keys stehen schon heute korrekt im JSON.
- Backward/PingPong funktionieren automatisch, weil `uiAnimDirectedTime` nur die
  Clip-Zeit umrechnet und die Auswertung eine reine Funktion der Clip-Zeit ist.

**Zurückgestellte Alternative:** eine Interpolationsart pro Key („Constant" wie in
Unreal/Unity), womit auch Float-Tracks springen könnten. Wäre ein neues Feld in
`UIAnimKey` (On-Disk-Format, Name muss gepinnt werden), und für das gewünschte
Feature nicht nötig. Kann später additiv kommen.

## 5. Vorschlag UI

**Add-Track-Popup** (`UIEditorPanel.cpp:3205ff`):

- Filter erweitern: zusätzlich Bool-Properties aus einer **Whitelist**
  `Visible`, `Enabled` (und je nach Antwort auf Frage 1 `Hit Testable`). Alle
  anderen Bool-Properties (Checked, Switch, Password, WordWrap, Clip Children …)
  in v1 nicht, siehe Frage 2.
- Gruppierung: Bool-Einträge unter einem Trenner „Switches" o. ä., damit klar
  ist, dass sie springen statt gleiten.
- **Enabled**: angeboten, wenn `interactive() || acceptsChildren()`; sonst
  **ausgegraut** (nicht versteckt) mit Tooltip „Hat auf diesem Element keine
  Wirkung: nichts darin reagiert auf Eingabe". Ausgegraut statt versteckt, damit
  niemand sucht, warum es auf einem Image fehlt. `UIText` fällt je nach Instanz
  mal hinein, mal nicht; das ist korrekt, weil es genau dann wirkt.
- **Erster Key bei Bool-Tracks:** Add Track legt heute den ersten Key am Playhead
  an (Z. 3227), und vor dem ersten Key gilt dessen Wert. Für Visible ist das eine
  Falle: Track bei 0,8 s angelegt, auf `false` gestellt, Element ist ab 0 s
  unsichtbar. Vorschlag: bei Bool-Tracks, wenn der Playhead > 0 steht, zusätzlich
  einen Key bei 0 mit dem aktuellen Wert anlegen (zwei Keys: 0 = Ist-Wert,
  Playhead = Ist-Wert, der zweite ist selektiert). Alternativ nur bei Bool den
  ersten Key immer bei 0.

**Key-Editor** (`drawKeyEditor`):

- `case UIPropType::Bool`: Checkbox „Value" (Help über das vorhandene
  `helpForLabel("Value")` im Scope „UI Timeline", also keine neue Doku-Zeile).
- Ease-Combo für Bool-Tracks ausblenden, ebenso die Ease-Einträge im
  Rechtsklickmenü des Keys (Z. ~3120).

**Lane-Darstellung (nice to have):** Bool-Track als Balken zeichnen (gefüllt,
wo `true`), damit man ohne Klicken sieht, wann etwas an/aus ist. Nicht nötig für
die Funktion.

**Designer-Nebenwirkung:** Die Leinwand lässt unsichtbare Elemente beim Zeichnen
und Picken weg (Z. 4361, 4559). Steht der Playhead in einem „unsichtbar"-Bereich,
ist das Element auf der Leinwand nicht anklickbar, nur über die Hierarchie.
Vorschlag v1: so lassen und im Handbuch erwähnen; optional später ein
gestrichelter Umriss für Elemente, die *nur durch den offenen Clip* unsichtbar
sind (die `ScrubPreview` weiß, welche das sind).

## 6. Verhalten zur Laufzeit (Festlegungen)

- Solange ein Clip läuft, schreibt er seine Tracks **jeden Frame**. Ein Skript,
  das währenddessen `Set Visible` aufruft, wird im nächsten Frame überschrieben.
  Das gilt heute schon für Opacity/Position und bleibt so.
- Ohne `restore` bleibt nach dem Clip der Wert des letzten Keys stehen (gewollt:
  „einblenden und freigeben" soll danach freigegeben bleiben). Mit `restore`
  kommt der Vorher-Wert zurück, generisch über `restoreOne`.
- Visible-Keys auf Kindern von Layout-Boxen (VBox/HBox/Grid/Wrap) lassen die
  Geschwister springen, weil unsichtbare Kinder keinen Slot bekommen. Wer das
  nicht will, animiert Render Opacity (und ggf. Enabled) statt Visible. Gehört
  in den Help-Text.

## 7. Tests (für die Umsetzungsschritte)

- `test_ui_widgets.cpp`: Step-Auswertung Bool (vor dem ersten Key, genau auf
  einem Key, zwischen zwei Keys, nach dem letzten, zwei Keys gleiche Zeit),
  Backward und PingPong mit Bool, Int-Track hält statt vorzuspringen,
  JSON-Roundtrip eines Clips mit Bool-Key.
- Laufzeit: `playAnimation` mit Visible-Track setzt `visible` zur richtigen
  Zeit; `restore` bringt es zurück; Enabled-Track macht einen Button inert
  (Hit-Test liefert ihn nicht mehr).
- `test_widget_designer_ui.cpp`: Add-Track-Popup zeigt Visible bei Panel,
  Enabled bei Button und Panel, Enabled ausgegraut bei Image; Key-Editor zeigt
  Checkbox und keine Ease-Combo für Bool.
- Help-Abdeckung: neue Labels (Trenner, Tooltip des ausgegrauten Eintrags)
  brauchen Einträge, damit die Abdeckungsprüfung grün bleibt.

## 8. Aufwand

| Schritt | Inhalt | Schätzung |
|---|---|---|
| 2 | Core: `between()` Step, Tests Auswertung + JSON + Laufzeit | 1–2 h |
| 3 | Editor: Add-Track-Filter mit Whitelist und Enabled-Fähigkeit, erster Key bei 0, Key-Editor Checkbox, Ease ausblenden, Help-Einträge, Designer-UI-Tests | 3–4 h |
| 4 | Optional: Bool-Balken in der Lane, Umriss für clip-verdeckte Elemente, Handbuch | 2 h |
| 5 | Vollbau + volle Testsuite, Windows-CI | Laufzeit |

Risiko gering: kein Formatwechsel, Laufzeitpfad unverändert bis auf `between()`.

## 9. Offene Fragen an den Menschen

1. **„Clickable" = Enabled oder Hit Testable?** Enabled dimmt und sperrt den
   ganzen Teilbaum inkl. Tastatur; Hit Testable macht nur dieses eine Element
   mausdurchlässig, ohne Optik, Kinder bleiben klickbar. Vorschlag: Enabled
   anbieten, Hit Testable zusätzlich als eigenen Track (kostet nichts extra).
2. **Nur Visible/Enabled (Whitelist) oder alle Bool-Properties** (Checked,
   Switch, Password, WordWrap …)? Vorschlag: v1 Whitelist; alle Bools wären
   technisch sofort möglich, fluten aber das Popup.
3. **Enabled bei nicht-interaktiven Elementen:** ausgegraut (Vorschlag) oder
   ganz ausblenden? Container (Panel, Boxen) sollen Enabled bekommen, weil es auf
   ihren Inhalt wirkt, einverstanden?
4. **Erster Key bei Bool-Tracks:** automatisch zusätzlicher Key bei 0 mit dem
   Ist-Wert (Vorschlag), oder wie bei den anderen Tracks nur am Playhead?

## 10. Umsetzung (Schritt 2)

Umgesetzt wie oben, die offenen Fragen aus Abschnitt 9 waren beim Umsetzen noch
unbeantwortet und sind **nach dem Vorschlag** entschieden, jede einzeln umkehrbar:

1. „Clickable" = **Enabled**. Hit Testable ist in v1 **nicht** im Popup (ein
   Eintrag in der Tabelle in `uiAnimTrackOffer`, falls gewünscht).
2. **Whitelist** Visible + Enabled, alle anderen Bools bleiben draußen.
3. Enabled auf nicht fähigen Elementen **ausgegraut** mit Tooltip
   (`ui.timeline-track-no-effect`); Container (Panel, Boxen, alles mit
   `acceptsChildren()`) bekommen Enabled.
4. Bool-Track bei Playhead > 0: **zusätzlicher Key bei 0** mit dem Ist-Wert.

Wo es steht:

- `UIWidgetAnim.cpp` `between()`: nicht interpolierbar = halten (auch Typ-Mismatch).
  Neu `uiAnimTypeInterpolates()` und `uiAnimTrackOffer()` (Capability-Filter, im Kern
  und damit ohne Designer testbar).
- `UIEditorPanel.cpp`: Add-Track-Popup in zwei Gruppen („Switches" darunter),
  Key-Editor mit Checkbox, Ease-Combo und Ease-Einträge im Key-Rechtsklick nur
  für interpolierende Typen.
- `WidgetManager.cpp`: das Risiko aus Abschnitt 3 bestand. `focusedTextField` und
  `hasFocusedTextField` prüften weder Enabled noch Visible, ein per Clip
  gesperrtes, schon fokussiertes TextInput nahm weiter Zeichen an. Jetzt nimmt es
  nichts, bis es wieder frei ist; der Fokus bleibt stehen.
- Tests: `test_ui_widgets.cpp` („Clips: a Bool track steps…" und fünf weitere),
  `test_widget_designer_ui.cpp` („Thema 108: Add Track offers…").

Nicht gemacht (Schritt 4 im Plan): Bool-Balken in der Lane, Umriss für
clip-verdeckte Elemente, Handbuch auf der Website.
