# Plan: Sprach-Umschalter fuer die praktischen Beispiele im In-Engine-Handbuch

Handoff aus Schritt 1 (Orientierung + Design) fuer Thema 23,
Branch `claude/inengine-docs-example-switcher`.
Kein Feature-Code, dieses Dokument ist die Vorlage fuer Schritt 2 ff.

Ziel laut Brett: das **In-Engine-Handbuch** (Help ▸ Documentation, `DocsPanel`)
soll die praktischen Beispiele je Scripting-Sprache mit einem Umschalter zeigen,
so wie es die Website seit Thema 20 tut. Die UI-Umsetzung ist zwangslaeufig eine
andere, ImGui statt HTML/CSS/JS.

---

## 0. Die eine Entscheidung, die Konsistenz gratis macht

**In der Engine wird kein einziges Beispiel geschrieben.**

Das In-Engine-Handbuch ist kein zweiter Text, sondern die *konvertierte*
Website: `scripts/build_docs_bundle.py` liest `Website/HorizonEngineDocs/*.html`
und schreibt `EditorDeps/Docs/he-docs.json`, das committet ausgeliefert wird
(`DocsLibrary.h`, Kopfkommentar: „The bundle is DATA, not code"). Die fuenf
Beispiele aus Thema 20 stehen also bereits im Bundle, sie werden nur falsch
dargestellt.

Damit ist die vom Brett gewuenschte Konsistenz keine Aufgabe, sondern eine
Eigenschaft: dieselben fuenf Aufgaben, derselbe Code, dieselben Kommentare, weil
es physisch derselbe Text ist. **Wer hier ein Beispiel in C++ hartcodiert, hat
die Architektur gebrochen** — die einzige Ausnahme im ganzen Reader ist die
generierte Node-Referenz (`HcNodeReference.cpp`), und die kommt aus den
Engine-Registries, nicht aus der Hand.

Die Arbeit besteht deshalb aus genau zwei Teilen:

1. **Konverter**: `.docs-langs` als *einen* Block erkennen statt ihn zu zerlegen.
2. **Reader**: diesen Block als Umschalter zeichnen.

---

## 1. Ist-Stand: wie das Handbuch heute gebaut ist

### 1.1 Die drei Teile

| Teil | Datei | Rolle |
|---|---|---|
| Modell | `src/HE_Editor/DocsLibrary.h/.cpp` (785 Z.) | Bundle laden, Topics aufloesen, suchen. Kennt kein ImGui, testbar ohne Fenster (`tests/test_docs_library.cpp`) |
| Ansicht | `src/HE_Editor/DocsPanel.h/.cpp` (1409 Z.) | Der Leser. Nimmt `Host{4 Fonts, IRenderer*}`, **keinen** `AppContext` — genau damit ist er headless zeichenbar (`tests/test_ui_shot.cpp`) |
| Tooltips/F1 | `src/HE_Editor/EditorHelp.h/.cpp` (4675 Z.) | Tabelle Label → Erklaerung + Topic; F1 oeffnet `DocsPanel::openTopic` |

Dazu der Konverter `scripts/build_docs_bundle.py` (737 Z.) und die
Deckungspruefung `scripts/editor_help_audit.py` (ctest `editor_help_audit`,
`tests/CMakeLists.txt:506`).

### 1.2 Blockmodell

`DocsLibrary.h` kennt heute die Arten: `Paragraph, Lead, Heading, Bullets,
Numbers, Table, Code, Callout, Flow, Figure, Tile, NodePreview, Unknown`.
Ein `Code`-Block traegt `text` plus optional `title` (der Dateiname in der
Kopfleiste). `DocsPanel::drawCode` (`DocsPanel.cpp:455`) zeichnet Titel dim,
dann ein `BeginChild` mit Codefont, auf 340 px gedeckelt, ohne Umbruch, mit
horizontalem Scrollbalken und einem `Copy`-Knopf darunter.

Eine **Sektion** (= `<section id>` der Website) ist die Einheit, auf die ein
Topic zeigt (`scripting#examples`), die Sucheinheit und das Sprungziel von F1.
`<h3>` erzeugt nur einen `Heading`-Block **innerhalb** der Sektion.

### 1.3 Was der Konverter aus `.docs-langs` heute macht

Nachgemessen, nicht vermutet (Bundle testweise nach `/tmp` neu gebaut, Sektion
`scripting#examples`, 56 Bloecke):

```
 3 h3     'Find an entity'
 4 p      'Every later example needs a handle …'
 5 p      'Lua'          ← Text des <button>
 6 p      'Python'       ← Text des <button>
 7 p      'C++'          ← Text des <button>
 8 p      'HorizonCode'  ← Text des <button>
 9 code   'finder.lua'
10 code   'finder.py'
11 code   'Game.cpp'
12 code   'Finder (graph)'
```

Der Grund steht in `build_docs_bundle.py:325-350`: der `tag == "div"`-Zweig
kennt `callout`, `docs-code`, `pipeline-*`, `docs-pager` und faellt sonst auf
`return blocks_of(k, images)` durch, „Unknown wrapper: keep the contents".
`.docs-langs` ist so ein unbekannter Wrapper, `.docs-langs-bar` ebenfalls, und
`<button>` hat keinen eigenen Zweig, also landet der Knopftext als Absatz.

**Das ist der Befund**: im Handbuch stehen heute vier Sprachnamen als lose
Absaetze und darunter vier Code-Bloecke gestapelt, fuenfmal hintereinander.
Der Leser scrollt durch 20 Listings, um 5 Aufgaben zu lesen.

### 1.4 Das Bundle ist stale

`python3 scripts/build_docs_bundle.py --check` beendet sich mit **rc=1**:
`EditorDeps/Docs/he-docs.json` ist aelter als die Website, die Beispiele aus
Thema 20 sind im ausgelieferten Bundle noch gar nicht drin.

Das ist **kein** roter ctest — `--check` ist nirgends in CMake verdrahtet
(`grep build_docs_bundle tests/CMakeLists.txt` → leer). Aber es ist eine Falle
fuer Schritt 2:

> **Das Bundle erst im selben Commit wie die Parser-Aenderung neu bauen.**
> Wer vorher `build_docs_bundle.py` laufen laesst, liefert genau die kaputte
> Darstellung aus 1.3 aus — vier Absaetze und vier gestapelte Listings, fuenfmal.

---

## 2. Gibt es schon ein Umschalter-Muster im Editor?

Ja, zwei, und die Wahl zwischen ihnen ist nicht Geschmack.

### 2.1 `ImGui::BeginTabBar` — **nicht nehmen**

Benutzt in `ProfilerPanel.cpp:767` und `EditorUI.cpp:2486`. Ein Tab-Bar haelt
seine Auswahl **selbst**, pro Bar. Der Umschalter soll aber *eine* Wahl fuer den
ganzen Reader sein (siehe 4.1), also fuenf Bars, die alle einer Variablen folgen
muessen. Genau davor warnt der Kommentar in `EditorUI.cpp:2515`: „setting
`s_activeTab` here every frame is wrong: `BeginTabItem` mutates `s_activeTab`".
Das ist ein Kampf, den man nicht anfangen muss.

### 2.2 `EditorToolbar` — **das ist das Vorbild**

`src/HE_Editor/EditorToolbar.h`: Palette (`kWellBg`, `kOnBg`, `kFg`, `kFgDim`),
Geometrie (`kWellRound`, `kSegGap`, `kCellPadX`) und die „Wells": runde
Gruppen, in denen zusammengehoerende Zellen sitzen. Der Zustand gehoert dem
Aufrufer, die Zelle bekommt ihn als `bool on` herein — genau die Form, die hier
gebraucht wird. Das Muster fuer eine *entweder-oder*-Wahl steht in
`UIEditorPanel.cpp:5305`:

```
// Designer | Graph, die UMG-Teilung. Zwei Radio-Knoepfe wurden ein segmentiertes
// Paar in einem Well: sie sind eine Wahl, und das Well ist, was das sagt.
bar.group();
if (bar.item("##uidesigner", T::iconWidget, "Designer", st.viewMode == 0, true, "…"))
    st.viewMode = 0;
…
bar.endGroup();
```

**Zu pruefen in Schritt 2**: ob `EditorToolbar::Bar` inline im Fliesstext eines
Panels gezeichnet werden kann, ohne die Bar-Flaeche `kBarBg` und die Haarlinie
mitzumalen — eine Toolbar-Bande mitten in einem Absatz waere falsch. Wenn nicht:
**nicht** `Bar` benutzen, sondern die Primitiven (Well-Rechteck, Zellfarben,
`kCellRound`, `kSegGap`) direkt, so wie ViewportToolbar es urspruenglich tat.
Die Optik wird uebernommen, das Bauteil nicht erzwungen.

Damit sieht der Umschalter im Handbuch aus wie jede andere segmentierte Wahl im
Editor, und er sieht dem Website-Pendant (`.docs-lang-btn`, Pillen mit
Gold-Aktiv) so aehnlich, wie zwei verschiedene UI-Systeme sich aehnlich sehen
koennen.

---

## 3. Die fuenf Beispiele

Sie sind bereits gewaehlt und geschrieben, in `HorizonEngineDocs/scripting.html`,
Sektion `examples`. **Unveraendert uebernehmen** — das ist der Punkt aus
Abschnitt 0.

| Anker | Aufgabe | Dateititel Lua / Python / C++ / HC |
|---|---|---|
| `ex-find` | Eine Entity finden | `finder.lua` · `finder.py` · `Game.cpp` · `Finder (graph)` |
| `ex-move` | Jeden Frame etwas bewegen (Plattform auf +Z) | `platform.*` · `Game.cpp` · `Platform (graph)` |
| `ex-jump` | Springen auf Tastendruck (Space + Bodenkontakt) | `jump.*` · `Game.cpp` · `Jump (graph)` |
| `ex-beam` | Raycast und Treffer verwerten (Sprungfeld) | `launchpad.*` · `Game.cpp` · `LaunchPad (graph)` |
| `ex-save` | Fortschritt im Savegame merken | `progress.*` · `Game.cpp` · `Progress (graph)` |

Das deckt sich mit der Liste vom Brett (Entity finden, Plattform bewegen,
Sprung, Raycast+Impuls, Speichern). Die Begruendung, warum es genau diese fuenf
sind und warum „Entity spawnen" und „Spinner" ausscheiden mussten (C++ hat
weder `spawn` noch `setRotation` in den fuenf Service-Tabellen), steht im
Schwesterplan `Website/docs/scripting-example-switcher-plan.md`, Abschnitt 2.
Sie gilt hier unveraendert weiter.

---

## 4. Entwurf

### 4.1 Eine Wahl fuer den ganzen Reader

Die Website haelt die Sprache seitenweit in `localStorage` unter
`he-docs-lang` (`docs-nav.js:168-180`: „One choice for the whole page: every
`.docs-langs` group switches together"). Der Reader macht dasselbe: **ein
file-static `s_lang` in `DocsPanel.cpp`**, alle Umschalter folgen ihm.

Das ist auch die Konvention des Panels: `s_page` und `s_section`
(`DocsPanel.cpp:76-77`) sind ebenfalls file-statics und ueberleben den
Neustart des Editors *nicht*. Persistenz ueber Sitzungen hinaus waere ein
eigener Schritt (der Reader hat heute gar keine) und gehoert **nicht** in
diesen. Als Notiz fuer spaeter festhalten, nicht bauen.

Startwert: die erste Sprache, die der Block anbietet. Kein hartcodiertes „Lua".

### 4.2 Der neue Block: `LangTabs`

**Bundle-Schema** (`build_docs_bundle.py`), additiv:

```json
{ "k": "langs",
  "vars": [
    { "lang": "lua",         "label": "Lua",         "title": "finder.lua",     "text": "…" },
    { "lang": "python",      "label": "Python",      "title": "finder.py",      "text": "…" },
    { "lang": "cpp",         "label": "C++",         "title": "Game.cpp",       "text": "…" },
    { "lang": "horizoncode", "label": "HorizonCode", "title": "Finder (graph)", "text": "…" }
  ] }
```

`label` **muss** aus dem Knopftext der Website kommen, nicht aus einer Tabelle
`{"cpp": "C++"}` im Reader. `DocsLibrary.h` verbietet ausdruecklich, dass C++
Namen aus dem Bundle kennt („nothing here may hard-code a page id, a section id
or a heading"); eine Sprachnamen-Tabelle im Panel waere dieselbe Sorte Drift.

**C++-Seite**: `BlockKind::LangTabs` in `DocsLibrary.h`, dazu

```cpp
struct Variant { std::string lang, label, title, text; };
std::vector<Variant> vars;   // LangTabs
```

im `Block`, und `"langs"` im `kindFromString`-Schalter (`DocsLibrary.cpp:54`).

### 4.3 Schema-Version: **nicht** bumpen

`DocsLibrary.cpp:16` sagt, wofuer `kSchemaVersion` da ist: bumpen, wenn der
Leser das Bundle **verweigern statt missverstehen** soll. Eine additive Blockart
ist beides nicht: ein unbekanntes `k` wird zu `BlockKind::Unknown` und in
`drawBlock` (`DocsPanel.cpp:836`) stillschweigend uebersprungen.

Der Unterschied ist der Schadensfall. Mit Bump verweigert ein aelterer Editor
**das ganze Handbuch** wegen fuenf Code-Bloecken. Ohne Bump fehlen ihm die fuenf
Beispiele, alles andere liest er. Also: **`SCHEMA_VERSION = 1` bleibt.**

Schritt 2 prueft einmal nach, dass der `Unknown`-Pfad wirklich nur ueberspringt
und nicht ueber `vars` stolpert (er liest die Felder gar nicht, sollte also
halten) — wenn doch, gilt die Ueberlegung neu.

### 4.4 Konverter

In `block_of`, **vor** dem generischen `return blocks_of(k, images)` am Ende des
`tag == "div"`-Zweiges (`build_docs_bundle.py:325-350`):

* `k.has_class("docs-langs")` →
  * Knopftexte aus `.docs-langs-bar` einsammeln: `data-lang` → `label`.
  * Panels: jedes `.docs-lang-panel` traegt **auch** `docs-code`, der vorhandene
    Zweig liefert also `title` + `text` fertig. Wiederverwenden, nicht kopieren.
  * Knopf und Panel ueber `data-lang` paaren. Ein Panel ohne Knopf oder ein
    Knopf ohne Panel ist ein Fehler in der Seite → **laut scheitern**, nicht
    stillschweigend weglassen.
* `k.has_class("docs-langs-bar")` → `[]` (vom Elter verbraucht), analog zu
  `pipeline-node`.

`block_text` (`build_docs_bundle.py:508`) **muss** `langs` kennen und ueber die
Varianten flachziehen. Sonst faellt genau der Teil des Handbuchs aus der Suche,
der die meisten API-Namen enthaelt — heute steht der Code aller vier Sprachen im
Suchtext der Sektion, und das muss so bleiben. Vorschlag:

```python
if k == "langs":
    return " ".join((v.get("title","") + " " + v["text"]).strip() for v in b["vars"])
```

### 4.5 Reader

`drawLangTabs(ctx, b)`, aufgerufen aus `drawBlock`:

1. Eine Zeile Umschalter: pro Variante eine Zelle in einem Well, aktiv =
   `v.lang == s_lang`. Klick setzt `s_lang`.
2. Darunter **genau eine** Variante, gezeichnet vom vorhandenen `drawCode` —
   nicht nachbauen. Titel, Codefont, Deckel bei 340 px, kein Umbruch,
   `Copy`-Knopf: alles schon da und soll gleich bleiben.
3. Kennt der Block die aktuelle `s_lang` nicht (eine Aufgabe, die eine Sprache
   auslaesst), faellt er auf seine erste Variante zurueck und die Zelle der
   fehlenden Sprache wird gar nicht angeboten. Die Website macht dasselbe
   (`docs-nav.js:179`, Sprachen pro Gruppe aus den Panels abgeleitet).
4. ImGui-Ids: der `Copy`-Knopf in `drawCode` heisst hart `"Copy##code"` und die
   Kinder heissen `"##code"`. Fuenf Umschalter auf einer Seite zeichnen jeweils
   nur *eine* Variante, aber es bleiben fuenf gleichnamige Ids pro Sektion.
   Das ist heute schon so (20 Code-Bloecke, alle `##code`) und ImGui verzeiht es
   bei Kindfenstern nicht immer — **`PushID` um jeden Block** ist der billige
   Schutz und gehoert in Schritt 2 mit hinein.

### 4.6 HorizonCode

Die Website stellt HorizonCode als **ASCII-Graph in einem Code-Block** dar
(`Event  BeginPlay` / `└▸ Set Variable  Player ◂ Engine Call Find By Name ("Player")`),
mit einem Callout darueber, das die Zeichen erklaert. Der Reader uebernimmt das
unveraendert — siehe Abschnitt 0.

Die naheliegende „Verbesserung" — `NodePreview`-Bloecke — **taugt hier nicht**:
`NodePreview` zeichnet *einen* Knoten mit seinen Pins (`DocsPanel.cpp:595`), es
gibt kein Bauteil fuer eine verdrahtete Kette. Ein Beispiel aus fuenf
NodePreviews waere fuenf Bilder ohne die Draehte dazwischen, also weniger
Information als der ASCII-Graph. Nicht bauen.

Was billig und richtig waere (optional, am Ende von Schritt 2 oder spaeter):
unter dem HorizonCode-Panel eine Zeile „Die Knoten im Einzelnen" mit Link auf
die generierte Node-Referenz (`horizoncode-nodes`). Der Reader kann das, Links
sind ein `Run` mit `Style::Link` und einem Topic als `href`.

### 4.7 Glyphen: das echte Risiko, nachgemessen

Der ASCII-Graph benutzt `└`, `▸`, `◂`, `·`, `→`. Der Konverter hat dafuer eine
Ersetzungstabelle, `FONT_SUBSTITUTIONS` (`build_docs_bundle.py:188-201`), weil
die Editorschrift keine Pfeile hat. Sie deckt `→ ← ↔ ▸ ▶ ✅ ✓ ✔ ⌘ ⇧ ⌃ ⌥` ab.
**`└` und `◂` stehen nicht drin.**

Nachgemessen an den ausgelieferten Schriften:

| Zeichen | Roboto Condensed (Fliesstext) | ProggyClean (Codefont) |
|---|---|---|
| `·` U+00B7 | ja | ja (Latin-1) |
| `»` U+00BB | ja | ja (Latin-1) |
| `└` U+2514 | **nein** | **nein** |
| `◂` U+25C2 | **nein** | **nein** |
| `▸` U+25B8 | nein → wird zu `»` | nein → wird zu `»` |

Der Codefont ist ImGui's eingebautes **ProggyClean**
(`EditorApplication.cpp:789`, `AddFontDefault()`), das nur Basic Latin und
Latin-1 kann — also noch enger als Roboto Condensed.

Folge: **die HorizonCode-Panels rendern heute Kaestchen**, mit oder ohne
Umschalter. Der Umschalter macht es nur sichtbar, weil er die HC-Variante von
einem von zwanzig gestapelten Listings zu einer von vier gleichrangigen
Ansichten befoerdert.

Fuer Schritt 2 heisst das: `FONT_SUBSTITUTIONS` erweitern, mit einem Satz
Ersatzzeichen aus Latin-1, die die Struktur erhalten. Vorschlag zum Diskutieren,
nicht als Beschluss:

```
"└": "\\",   "▸": ">",   "◂": "<",   "─": "-",   "├": "|",   "│": "|"
```

Achtung: `▸` steht heute auf `»` und wird auch fuer **Menuepfade** benutzt
(„View ▸ Console") — das darf nicht kaputtgehen. Entweder `»` behalten und
akzeptieren, dass die Graphen `└» Set Variable` schreiben, oder die Ersetzung
kontextabhaengig machen, was der Tabelle ihre Einfachheit nimmt. **Empfehlung:
`▸`→`»` unangetastet lassen, nur `└` und `◂` ergaenzen.** Dann liest der Graph
`\» Set Variable  Player < Engine Call …`, was haesslich, aber lesbar ist, und
das erklaerende Callout der Website muss in Schritt 2 einmal gegengelesen
werden, weil es die Zeichen beim Namen nennt.

**Diese Frage ist am Ende eine Sichtprobe**, kein Argument: `he_uishot` auf
`scripting#examples` (siehe 6.2) entscheidet sie.

---

## 5. Deckung, F1 und der Audit

### 5.1 `editor_help_audit` faellt sonst rot

`scripts/editor_help_audit.py` hat fuer **alle** Bereiche `BASELINE = 0`,
`DocsPanel.cpp` gehoert zum Bereich `interface`. Vier neue beschriftete Knoepfe
darin heben `interface` auf 4, der ctest `editor_help_audit` scheitert. Das ist
kein Unfall, sondern der Zweck des Tests.

**Gewaehlte Loesung** (Variante a, weil sie dem bestehenden Praezedenzfall
folgt):

1. In `EditorHelp.cpp` vier Eintraege unter dem Scope, den der Reader ohnehin
   oeffnet (`Help::Scope helpScope("Documentation")`, `DocsPanel.cpp:1285`):
   `Documentation/Lua`, `Documentation/Python`, `Documentation/C++`,
   `Documentation/HorizonCode`. Text im Sinne der Hausregel — was der Knopf
   *tut*, nicht wie er heisst: „Zeigt dieselbe Aufgabe als Lua-Skript. Die Wahl
   gilt fuer alle Beispiele im Handbuch."
2. In `editor_help_audit.py` die vier Paare zu `IGNORE` hinzufuegen, mit
   **derselben Begruendung**, die dort schon fuer `(None, "Show me")` steht:
   der Reader oeffnet seinen Scope in `draw()` am Dateiende, waehrend die
   Knoepfe von Helfern darueber gezeichnet werden; ein Scan, der die Datei von
   oben nach unten liest, kann das nicht sehen. Zur Laufzeit ist der Scope
   offen — `drawBlock` laeuft ueber `drawPage` aus `draw()` heraus, **nach**
   Zeile 1285. Nachgesehen, stimmt.
3. In `tests/test_editor_help.cpp` die vier Keys per Laufzeit-Lookup zusichern,
   genau wie es der Kommentar in `IGNORE` fuer die anderen Reader-Knoepfe
   verlangt. Ohne diesen Schritt ist der `IGNORE`-Eintrag ein Versteck.
4. `BASELINE` bleibt bei 0. **Niemals hochsetzen, um den Check gruen zu
   bekommen** — das steht so im Kopf des Skripts.

Verworfene Variante (b): eigene Keys `docs.lang.lua` + eine neue `Area`-Regel
mit Praefix `docs.`. Funktioniert auch, kostet aber eine Regel in `areaOf` und
eine neue Sektion in der generierten Referenzseite fuer vier Knoepfe, die
zusammen eine einzige Wahl sind. Nicht verhaeltnismaessig.

Die zwei Gesetze aus `[[in-engine-docs-and-tooltips]]` sind beide beruehrt und
beide erfuellt: der Scope ist offen, wenn die Eintraege nachgeschlagen werden,
und ein geteilter Helfer (`drawLangTabs`) scopet sich nicht selbst — er liegt im
Scope des Readers, der genau einer ist.

### 5.2 F1 zeigt auf die Sektion, nicht auf das Beispiel

`resolve("scripting#ex-find")` findet die **Seite**, aber nicht die Sektion:
`ex-find` ist ein `<h3>`, und Sektionen im Bundle sind `<section id>`
(`build_docs_bundle.py:557`). `DocsLibrary.h` beschreibt genau dieses Verhalten
als beabsichtigt („a renamed anchor should land the reader on the right page,
not nowhere").

Praktisch: **F1 und Tooltip-Topics koennen nur `scripting#examples` treffen**,
nicht ein einzelnes Beispiel. Auf der Website funktionieren die Tiefenlinks.

Das ist eine bekannte Grenze, **kein Auftrag fuer diesen Schritt**. Sie zu
beheben hiesse, `<h3 id>` zu Untersektionen zu machen, und das aendert die
Topic-Granularitaet des gesamten Handbuchs — ein eigenes Thema mit eigenem
Risiko fuer die 600 gedeckten Bedienelemente.

---

## 6. Schrittfolge und Abnahme

### 6.1 Reihenfolge fuer Schritt 2 ff.

1. `build_docs_bundle.py`: `docs-langs` erkennen, `docs-langs-bar` verbrauchen,
   `block_text` erweitern, `FONT_SUBSTITUTIONS` um `└` und `◂` ergaenzen.
   Probelauf nach `/tmp`, Sektion `examples` im JSON gegenlesen: **1 `h3` +
   1 `p` + 1 `langs` je Beispiel**, keine losen `p` mit Sprachnamen mehr.
2. `DocsLibrary.h/.cpp`: `BlockKind::LangTabs`, `Variant`, `"langs"` im
   Schalter. `tests/test_docs_library.cpp` um einen Fall erweitern, der einen
   `langs`-Block aus JSON parst und ihn im Suchtext wiederfindet.
3. `DocsPanel.cpp`: `drawLangTabs`, `s_lang`, `PushID` je Block.
4. `EditorHelp.cpp` + `editor_help_audit.py` + `tests/test_editor_help.cpp`
   nach 5.1.
5. **Bundle neu bauen und committen** — erst hier, siehe 1.4.
6. `he_uishot`, Sichtprobe, Glyphen-Entscheidung final ziehen.

Schritte 1–2 und 3 sind trennbar; 5 gehoert an das Ende, nicht an den Anfang.

### 6.2 Abnahme

* `python3 scripts/build_docs_bundle.py --check` → rc 0.
* `ctest` gruen, insbesondere `editor_help_audit`, `test_docs_library`,
  `test_editor_help`.
* **Sichtprobe**: `scripts/he_uishot.py` zeichnet den Reader headless
  (`tests/test_ui_shot.cpp:416-467` macht das heute schon fuer
  `editor#layout`). Ein Shot von `scripting#examples` ist die Abnahme, und er
  ist zugleich die einzige Instanz, die die Glyphenfrage aus 4.7 beantwortet.
  Ein neuer Fall in `test_ui_shot.cpp` daneben waere die dauerhafte Fassung.
* Gegenprobe von Hand: Sprache umschalten, in eine andere Sektion navigieren,
  zurueck — die Wahl steht noch. Suche nach `isGrounded` findet
  `scripting#examples`.

---

## 7. Was dieser Schritt bewusst nicht entscheidet

* **Ob `EditorToolbar::Bar` inline taugt** oder ob nur seine Primitiven benutzt
  werden. Das entscheidet der erste Blick auf die gezeichnete Bande (2.2).
* **Die endgueltigen Ersatzzeichen** fuer `└` und `◂` (4.7) — Sichtprobe.
* **Persistenz der Sprachwahl** ueber den Editor-Neustart. Der Reader hat heute
  gar keine; das waere ein eigener kleiner Schritt (4.1).
* **Tiefenlinks auf einzelne Beispiele** (`scripting#ex-jump`). Bekannte Grenze,
  eigenes Thema (5.2).
* **Link vom HorizonCode-Panel in die Node-Referenz** (4.6). Nice-to-have.

---

## 8. Offene Frage an den Chefchen

Die Website nennt in ihrem Callout die Zeichen beim Namen („`└▸` is the next
node on it, `◂` means this pin is fed by"). Wenn der Reader `└` und `◂` durch
`\` und `<` ersetzt, stimmt dieser Satz im Handbuch nicht mehr, waehrend er auf
der Website stimmt — und Text darf hier nicht auseinanderlaufen (Abschnitt 0).

Zwei Wege:

* **A**: Der Website-Text wird so umformuliert, dass er die Zeichen *nicht*
  beim Namen nennt („die Einrueckung zeigt die Kette, ein Pfeil vor einem Pin
  heisst: gespeist von"). Dann stimmt er in beiden Systemen. Kostet eine
  kleine Aenderung an `scripting.html` und einen Deploy, also einen Ausflug in
  das andere Repo.
* **B**: Der Konverter ersetzt die Zeichen *auch im Callout-Text* konsistent
  mit, dann stimmt der Satz in beiden Faellen, sagt aber je nach System
  verschiedene Zeichen. Kostet nichts, ist aber der Punkt, an dem die beiden
  Handbuecher zum ersten Mal wirklich unterschiedliche Saetze sagen.

**Empfehlung: A.** Ein Handbuch, das Zeichen beschreibt, die auf dem Bildschirm
anders aussehen, ist genau die Sorte Drift, gegen die dieses ganze System gebaut
ist. Aber es ist eine Aenderung an der Website und damit ausserhalb dieses
Themas — deshalb hier als Frage und nicht als Beschluss.
