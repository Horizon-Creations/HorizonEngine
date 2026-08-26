# Handbuch im Editor + nützliche Tooltips — Design

> Stand: 2026-08-25 · Ziel: Help ▸ Documentation öffnet das Handbuch **im Editor**, mit
> Suche und Querverweisen, und jedes Bedienelement, das Erklärung braucht, hat eine.
> Beides hängt an derselben Tabelle, damit Tooltip und Handbuch nicht auseinanderlaufen.

## Ausgangslage

Help ▸ Documentation war ein `SDL_OpenURL` auf die Website. Das versagt genau dann, wenn
man es braucht: offline, ohne Browser zur Hand, oder mitten in einer Geste im Editor.

Der Details-Panel hatte sechs Tooltips für rund 180 Regler. Namen wie „Fluffiness",
„Rolloff Factor" oder „Walk Climb" standen ohne ein Wort dazu da.

## Die drei Teile

### 1. Bündel: `scripts/build_docs_bundle.py` → `EditorDeps/Docs/he-docs.json`

Übersetzt jede Doku-Seite der Website (`Website/HorizonEngineDocs/*.html`, Geschwister-
Checkout) in **Blöcke**: Absatz, Lead, Überschrift, Liste, Tabelle, Listing, Callout,
Flow, Abbildung, Kachel. Inline-Text sind gestylte Läufe (`b`, `i`, `c`, `l` + `href`).

Entscheidungen, die sich gerächt hätten:

- **Blöcke statt Flachtext.** Das halbe Editor-Handbuch besteht aus Referenztabellen; als
  Textblob ist eine Tabelle unlesbar. Die Website-eigene `docs-index.json` macht genau
  das und taugt deshalb nur zum Suchen, nicht zum Lesen.
- **Das Ergebnis ist eingecheckt.** CI und ein frischer Clone dürfen den Website-Checkout
  nicht brauchen. `--check` meldet ein veraltetes Bündel, ohne zu schreiben.
- **Screenshots werden auf 1280 px skaliert und als JPEG neu kodiert** (4,6 MB → 1,0 MB).
  Volle Retina-Breite neben jedem Editor-Build ist der falsche Handel.
- **Zeichen, die die Editor-Schrift nicht hat, werden ersetzt.** Roboto Condensed Bold hat
  keine Pfeile, keine Haken und keine Mac-Tastensymbole; im Browser findet sich dafür
  irgendeine Schrift, im Editor kommt ein leerer Kasten. `FONT_SUBSTITUTIONS` bildet sie
  einmal zentral ab (`→` → `->`, `▸` → `»`, `⌘` → `Cmd`, …). Die Typografie, die die
  Schrift hat (— … · × ² „ ") bleibt unangetastet.

### 2. Modell + Suche: `HE::Ed::Docs` (`DocsLibrary.{h,cpp}`)

Ohne ImGui, SDL und AppContext — wie `TutorialSteps`, und aus demselben Grund: so ist der
Inhalt ohne Fenster testbar und dem Panel bleibt nur das Zeichnen.

Die Suche ist der Teil, an dem das Ganze steht oder fällt:

- Überschrift wiegt schwerer als Fließtext, **mit Deckel**, wie oft eine Fundstelle im
  Text weiterzählt. Sonst gewinnt die Referenztabelle, die ein Wort vierzig Mal nennt,
  gegen den Abschnitt, der so heißt.
- **Alle Treffer sitzen am Wortanfang.** Mit reiner Teilstringsuche findet „ui" auch
  „build" und „guide", „light" auch „highlight". Das Wortende bleibt offen, damit
  „shader" noch „shaders" trifft und „profil" schon „profiler".
- **Eine Morphologie-Regel:** Singular und Plural finden einander. Man tippt die Form, in
  der man denkt, nicht die aus der Überschrift („shortcuts" → „Shortcut Reference").
- Mehrere Wörter werden UND-verknüpft, ein Treffer ist ein **Abschnitt**.

### 3. Reader: `DocsPanel.{h,cpp}`

Seitenbaum links, Suche oben, Blöcke rechts, Vor/Zurück wie im Browser, „Online" öffnet
dieselbe Stelle auf der Website.

- **Gezeichnet vor der Verzweigung in `EditorUI::render`**, also auch im Project Hub. „Wie
  fange ich ein Projekt an" ist die Frage, die man hat, BEVOR es eins gibt.
- **Inline-Umbruch von Hand.** ImGui bricht einen *String* um; ein Absatz hier ist eine
  Folge verschieden gesetzter Läufe, und dafür gibt es keinen Aufruf. Die Entscheidung
  fällt NACH dem Wort statt davor: `GetItemRectMax()` kennt auch das Polster, das ein
  Code-Span dazurechnet. **Beim Umbruch wird kein `NewLine()` gerufen** — nach einem
  gezeichneten Wort steht der Cursor schon in der nächsten Zeile, das gäbe doppelten
  Zeilenabstand (genau so gesehen im ersten Screenshot, siehe unten).
- **Betonung ist Farbe, nicht Schnitt.** Die einzige Schrift des Editors ist bereits fett;
  `**fett**` nimmt das Überschriften-Gold, Code das hellere Gold plus Plättchen.
- **Lesebreite statt Fensterbreite** (42 em). Ab etwa neunzig Zeichen verliert das Auge den
  Anfang der nächsten Zeile — das ist der Unterschied zwischen einer Seite und einer Wand.
  Die Begrenzung sitzt am **scrollenden** Child: ein Child darin wäre das, mit dem
  `SetScrollHereY` spricht, und mit `AutoResizeY` scrollt das nie. Und ein `Indent()` nach
  einem `SameLine()` verschiebt die NÄCHSTE Zeile, nicht diese.
- **Die Seitenleiste klappt lange Seiten in ihre Kategorien** (ab 30 Abschnitten), die
  Gruppe des gelesenen Abschnitts öffnet sich selbst. Dafür sortiert die Node-Referenz ihre
  Abschnitte nach Kategorie: die Enum-Reihenfolge ist die, in der Nodes zur Engine kamen.
- **Abbildungen, die zeigen wo etwas ist**, zeichnet der Editor selbst (`he_uishot.py
  --docs` → `EditorDeps/Docs/img`, platziert über die `FIGURES`-Tabelle des Generators):
  eine beschriftete Karte des Layouts mit dem gesuchten Panel in Amber. Ein Screenshot
  würde zugleich jemandes Projekt zeigen und veralten.
- **`draw()` nimmt einen schmalen `Host`** (vier Fonts, ein Renderer), nicht den ganzen
  `AppContext`. Sonst könnte ihn nur etwas zeichnen, das ein Projekt, eine Welt und eine
  GPU hinter sich hat — und genau das verhinderte, ihn je anzusehen.
- **„Show me"** öffnet das Panel, um das ein Abschnitt geht, und lässt den Umriss pulsen
  (`PanelSpotlight`, aus dem Rundgang herausgelöst). Nur wo die Zuordnung wirklich eins
  kennt und nur mit geladenem Projekt.

## Die Node-Referenz: gebaut, nicht geschrieben

`HcNodeReference.cpp` baut aus **den Registern selbst** eine Seite: einen Abschnitt pro
aufrufbarer Sache (311 Engine-Calls + alle Built-in-Nodes), mit den Pins aus
`signatureOf` und den Beschreibungen aus `HcNodeDocs.cpp`.

- **Pro Funktion, nicht pro Kategorie.** Dort muss F1 aus dem Node-Tooltip landen; ein
  Anker pro Kategorie setzt den Leser sechzehn Einträge über der Frage ab.
- **Sie behält die Seiten-Id der Website-Seite** (`horizoncode-nodes`), damit Querverweise
  und Anker weiter auflösen — der Generator überspringt die HTML-Seite jetzt. Weil sie
  eine gewöhnliche Seite ist, brauchen Suche, Navigation, Verlauf und Themen **keinen
  einzigen Sonderfall**. `appendPage` ersetzt nach Id, ist also idempotent.
- **Die Beschreibungen sagen, was die Signatur nicht kann:** Einheiten, Verhalten bei
  falscher Eingabe, Vorbedingungen, Konsequenzen von pure/exec. Die 116 Sky-Zeilen
  entstehen aus derselben X-Liste wie das Register, mit einer Beschreibung pro FELD.
- Der Coverage-Test läuft über das **lebende** Register: 16 Mathe-Funktionen werden über
  einen Helfer registriert und sind beim Lesen der Quelldatei unsichtbar.

**Der Tooltip wird gezeichnet, nicht zusammengesetzt** (`HcEditorUtil::drawNodeDoc`), damit
die Pins die Sprache des Canvas sprechen: Dreieck für Exec, gefüllter Kreis für Daten,
2×2-Raster für Container, jeweils in der Farbe des Typs. `GraphEditor::Model::nodeTooltip`
(String) wurde dafür zu `drawNodeTooltip` — die Komponente entscheidet WANN, der Host WIE.

## Tooltips: `HE::Ed::Help` (`EditorHelp.{h,cpp}`)

Eine Tabelle, keine Aufrufe. Drei Eigenschaften machen sie tragfähig:

1. **Sie ist Daten.** Ein am Aufrufort geschriebener Tooltip ist unsichtbar für Review,
   nicht testbar und läuft vom Handbuch weg. Über die Tabelle läuft ein Test, der prüft,
   dass jedes `topic` im ausgelieferten Bündel noch auflöst.
2. **Schlüssel ist das Label, das der Nutzer ohnehin liest**, im Rahmen seines Abschnitts:
   `"Rigid Body/Mass"`. Ein `Help::Scope` oben in der Komponente und ein Lookup in
   `EditorWidgets::Row` genügen für ein ganzes Panel — keine geänderte Aufrufstelle. Ein
   `##suffix` im Label ist Teil des Schlüssels, wo ein Label doppelt vorkommt
   (`Density##fog`).
3. **Jeder Eintrag kann ein `topic` tragen.** F1 über einem Regler öffnet das Handbuch an
   der Stelle, die ihn erklärt. Ein Tooltip ist ein Satz, manche Fragen brauchen eine
   Seite.

### Die Falle: der Tooltip wird am FRAMEENDE gezeichnet

`ImGui::Begin` (durch das `BeginTooltip` läuft) überschreibt `g.LastItemData`, und der
Details-Panel bucht seine Undo-Schritte genau daran (`IsItemDeactivatedAfterEdit`, direkt
hinter dem Control). Ein an Ort und Stelle gezeichneter Tooltip hätte **jedes Undo im
Panel stillgelegt**. Also melden die Controls nur an (`helpForLabel`/`helpForKey`), und
`EditorWidgets::drawQueuedHelp()` zeichnet einmal, ganz am Ende von `EditorUI::render`.
Nebenbei klärt die eine Warteschlange die Rangfolge: wer zuletzt angemeldet hat, ist der
unter der Maus.

`ImGuiHoveredFlags_ForTooltip` bringt ImGuis eigene Manieren mit — Verzögerung und
*Stationary*, der Tooltip feuert also nicht, während der Zeiger nur über das Panel
hinwegfährt. `AllowWhenDisabled` steckt darin, und das ist hier genau richtig: „warum kann
ich das nicht drücken" ist die Frage, die ein graues Bedienelement aufwirft.

## Headless ansehen: `tests/ImGuiSoftwareRaster.{h,cpp}` + `test_ui_shot.cpp`

ImGui **fahren** ließ sich headless schon (Kontext + Display-Größe genügen), sehen nicht.
Der Rasterizer schließt das: ImGuis Ausgabe ist eine Dreiecksliste mit Textur und Farbe
pro Vertex, das auf der CPU zu zeichnen ist ein kleiner Rasterizer, keine Grafikportierung.

    scripts/he_uishot.py [OUTDIR]     # → PNGs pro Szene

Was er **nicht** ist: pixelgleich mit den GPU-Backends. Kein Gamma, kein Multisampling,
gerades Source-over. ImGuis eigenes Anti-Aliasing kommt durch (es steckt im Vertex-Alpha),
Text und runde Ecken sehen also richtig aus; ob Metal und OpenGL sich beim Blend einig
sind, beantwortet das nicht.

Die Zusicherungen sind bewusst grob („das Panel hat etwas gezeichnet", „der Tooltip hat
Tinte hinzugefügt, wo keine war"). Eine pixelgenaue Erwartung würde beim nächsten
Schriftwechsel brechen und allen beibringen, sie ungelesen neu zu erzeugen. Gefunden hat
der erste Blick trotzdem zwei Dinge, für die es keinen anderen Zeugen gab: den doppelten
Zeilenabstand und die fehlenden Glyphen.

## Was offen ist

- Der Reader hat **keine Volltext-Sprungmarken innerhalb eines Abschnitts** — die Suche
  scrollt auf den Abschnitt, nicht auf die Zeile.
- **Die Node-Referenz-Seite der Website steht noch da**, sie wird nur nicht mehr ins Bündel
  übernommen. Sie zu löschen ist ein Deploy und braucht eine Bestätigung.
- **Kein Command-Palette-Ersatz.** „Wo ist X" beantwortet heute die Suche plus „Show me";
  eine Palette über Menübefehle und Einstellungen wäre der nächste Schritt.
- Die Hilfe-Tabelle deckt **alle 252 beschrifteten Zeilen** von Details-Panel und
  Preferences ab (gegengeprüft, indem die Labels der Panels gegen die Schlüssel gelaufen
  sind — Zeilen mit `##id` sehen von der Tabelle aus gedeckt aus und sind es nicht),
  dazu Viewport-Leiste, Outliner und Content Browser. **Die Asset-Editoren (Material-Graph,
  Widget-Designer, Partikel) haben noch keine Einträge.**
- Der Reader wurde **nie in der echten GUI bedient** — verifiziert ist er über die
  Software-Renderings und die Tests.
