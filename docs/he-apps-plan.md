# HE Apps: normale Anwendungen mit dem UI-Framework bauen

**Stand:** Branch `claude/he-apps-ui-framework`, Basis `main` 6b3b2539.
**Grundlage:** der Code, nicht der Plan. Alle „schon da"-Aussagen unten sind am Quelltext geprüft.

Neue Richtung, quer zu allem bisherigen: mit Horizon Engine soll man **normale
Desktop-Anwendungen** bauen können. Kein Spiel, keine 3D-Welt, keine Physik. Ein Fenster,
Widgets, Logik in HorizonCode, Lua oder Python, exportiert als eigenständige App.

Das ist kein neues Teilsystem, sondern ein **zweiter Betriebsmodus** für Dinge, die
größtenteils schon existieren. Der Rest dieses Dokuments trennt sauber: was trägt schon,
was fehlt, in welcher Reihenfolge.

---

## 1. Was eine „App" von einem „Spiel" unterscheidet

Sechs harte Unterschiede, aus denen sich fast jeder Punkt weiter unten ableitet:

1. **Es gibt keine Welt.** Keine Szene, keine Kamera, keine Entities, keine Physik.
   Die UI ist nicht *über* etwas, sie ist alles.
2. **Nicht jeder Frame ist neu.** Ein Spiel rendert 60 mal pro Sekunde. Ein Texteditor
   rendert, wenn sich etwas ändert. Eine App, die im Leerlauf die GPU auslastet und den
   Akku leert, ist disqualifiziert, egal wie hübsch sie ist.
3. **Das Fenster ist Teil der App.** Titel, Größe, Resize, Minimieren, mehrere Fenster,
   Menüleiste, „Willst du speichern?" beim Schließen.
4. **Die Eingabe ist Text, nicht WASD.** Auswählen, Cursor, Ctrl+C/V, Undo, Tab-Reihenfolge,
   Doppelklick auf ein Wort, IME.
5. **Daten kommen von außen.** Dateien an beliebigen Pfaden, Zwischenablage, HTTP,
   vielleicht eine Datenbank. Das Sandbox-`Saved/`-Gefängnis der Spiel-API ist genau falsch.
6. **Es soll nach Standard aussehen, ohne Aufwand.** Ein Spiel-HUD wird von Hand gestaltet.
   Ein Einstellungsdialog soll aus der Schachtel kommen und wie ein Einstellungsdialog aussehen.
   Das ist der „viel vordefiniert"-Teil aus dem Auftrag.

---

## 2. Was heute schon trägt

Mehr als erwartet. Der Kern ist da, verifiziert:

**Boot ohne Welt ist bereits die Reihenfolge.** `GameApplication::OnInit`
(`src/HE_Game/src/GameApplication.cpp:224`) startet die **GameInstance zuerst**, vor Welt und
Szene, und ihre Widgets leben in `m_widgets` am App-Level, nicht an der Welt. Ein
Szenenwechsel löscht sie nicht. Genau das ist schon die Struktur einer App: Logik-Objekt
startet, baut UI, UI überlebt alles. Die Welt danach ist heute nur nicht abschaltbar.

**Das Widget-System ist kein Prototyp mehr.** 13 Typen
(`src/HE_Core/include/UIWidget/UIElement.h:31`): Panel, Image, Text, Button, CheckBox,
Slider, ProgressBar, TextInput, ComboBox, VerticalBox, HorizontalBox, ScrollBox, WidgetRef.
Dazu Anchor-Rechtecke nach UMG-Modell, sechs Canvas-Skalierungsmodi, Clipping mit Scissor in
allen fünf Backends, Textur-Slots, 9-Slice, vererbte Deckkraft, Fokus plus räumliche
Navigation mit Fokusring, Rotation, und mit `WidgetRef` **echte Komponenten**: ein Widget-Asset
wird in ein anderes eingepfropft und führt seine Logik als eigene Skript-Instanz aus. Das ist
die Grundlage jeder Komponentenbibliothek.

**Drei Sprachen sind schon verdrahtet.** Eine Registry (`HE::api`, `EngineApi.cpp`) speist
gleichzeitig HorizonCode-Nodes und `horizon.<gruppe>.<fn>` in Lua und Python. 19 Gruppen,
134 Funktionen. Wer eine Funktion einmal registriert, hat sie in allen Frontends.

**Projekt-Typen gibt es schon.** Struct- und Enum-Assets erzeugen echte Typen in HorizonCode,
Lua, Python und generiertem C++. Für Apps ist das das Datenmodell.

**Das Ausliefern funktioniert.** `ProjectExporter` packt alles in eine `.hpak`, schreibt
`project.hcfg`, und auf macOS fällt ein fertiges `.app`-Bundle heraus. Ein `__asset_index__`
im Pak löst Assets per Pfad auf, was Widgets brauchen, die zur Laufzeit erzeugt werden.

**Das Fenster kann mehr, als das Spiel nutzt.** `Window` (`src/HE_Core/include/Window/Window.h`)
kennt sekundäre Fenster (`isPrimary=false`), `SetTitle`, `SetSize`, `SetBorderless`, versteckten
Start, und `CancelClose()`, mit dem sich ein OS-Schließen abfangen lässt, bis der Nutzer
„Speichern?" beantwortet hat. Das ist exakt die App-Semantik, nur nie so benutzt.

**Speichern und Dateien gibt es, nur zu eng.** Das Save-System mit typisierten Vorlagen und
die `fs`-Gruppe existieren, letztere ist absichtlich auf `Saved/` eingesperrt.

**Ein „Tool"-Projektpreset existiert schon** (`ProjectPreset::Tool` in
`src/HE_Tools/src/FileOps/ProjectManager.h:9`), legt aber nur Ordner an.

---

## 3. Block A: App-Runtime

Der größte Brocken und der, ohne den nichts anderes zählt.

**A1 Weltloser Modus.** Ein Schalter im Projekt (`.heproj` „appMode") und in `project.hcfg`.
Ist er an, erzeugt der Runtime keine `HorizonWorld`, keine `PhysicsWorld`, keine Default-Kamera,
kein Audio-System-Tick, keinen Szenen-Ladepfad. Der Renderer macht genau einen Pass: Clear plus
UI. Heute wird all das in `OnInit` bedingungslos aufgebaut.

**A2 Ereignisgetriebenes Zeichnen.** Der Loop in `Application::Run` läuft dauerhaft; es gibt
nur einen optionalen FPS-Deckel per `SDL_DelayNS`, und der greift nur ohne VSync. Für Apps
braucht es `SDL_WaitEvent` mit Timeout plus ein Dirty-Flag: neu zeichnen, wenn ein Event
ankam, ein Widget sich geändert hat, eine Animation läuft oder ein Timer fällig ist. Sonst
schlafen. Das ist der Unterschied zwischen 0,3 % und 40 % CPU im Leerlauf.

**A3 Schlanker Runtime.** Eine App, die 200 MB wiegt, weil Physik, Terrain, GI und der
Python-Interpreter mitfahren, verkauft sich schlecht. Build-Optionen: ohne Jolt, ohne
Terrain/Foliage/Weather, ohne die schweren Renderpfade. Ziel als Zahl formulieren, sonst
passiert es nie.

**A4 Fenster-API im Skript.** Neue `app`-Gruppe: `setTitle`, `getSize`/`setSize`,
`setMinSize`, `center`, `maximize`, `minimize`, `setResizable`, `setFullscreen`, `quit`,
`requestQuit` (mit Veto über `CancelClose`), plus die Events `onClose`, `onResize`,
`onFocus`/`onBlur`, `onFileDropped`.

**A5 Mehrere Fenster.** `Window` kann es, der Runtime nicht. Ein zweites Fenster mit eigenem
Widget-Baum, eigener Canvas, eigenem Skript-Kontext. Modale Dialoge als echtes Fenster statt
als Overlay. Das ist deutlich mehr Arbeit als A4 und gehört in eine spätere Welle.

**A6 Menüleiste.** Auf macOS die echte (der Editor hat mit `MacMenuBar` schon einen
Präzedenzfall), auf Windows/Linux eine gezeichnete im Fenster. Definiert als Asset, nicht als
Code, damit HorizonCode sie befüllen kann.

**A7 Systemintegration.** App-Icon, Bundle-Identifier, Dateitypen-Zuordnung, „Öffnen mit",
Autostart, Tray-Icon. Alles klein einzeln, zusammen der Unterschied zwischen „App" und
„Fenster mit Zeug drin".

---

## 4. Block B: Widget-Lücken, für Apps neu sortiert

Die offene Liste aus dem UI-Ausbau existiert schon, aber in Spiel-Reihenfolge. Für Apps sieht
die Rangfolge anders aus:

**B1 TextInput richtig.** Heute kann das Feld Anhängen und Backspace. Für eine App ist das
nichts. Gebraucht: Cursor mit Klickpositionierung, Auswahl per Ziehen und Shift+Pfeil,
Wort-Sprünge, Doppelklick wählt Wort, Dreifachklick wählt Zeile, Ctrl+A/C/X/V/Z,
Mehrzeiligkeit, Platzhaltertext, Zeichenlimit, Validierung, Passwortmodus, IME für CJK,
Scrollen bei Überlänge. Das ist der einzelne wichtigste Punkt im ganzen Dokument.

**B2 ListView und Tabelle.** Datengebunden, virtualisiert (10.000 Zeilen dürfen nicht 10.000
Elemente sein), Zeilenvorlage als WidgetRef, Auswahl (einzeln/mehrfach), Sortierung,
Spaltenbreiten. Ohne Liste gibt es keine Datei-App, keine Todo-App, keine Tabellen-App.

**B3 Container.** Grid mit Zeilen/Spalten und Spannen, plus ein WrapBox. VerticalBox und
HorizontalBox alleine erzwingen Verschachtelungstürme.

**B4 Dialoge und Popups.** Modale Overlays mit Abdunklung und Fokusfalle, Kontextmenü per
Rechtsklick, Dropdown-Menü, Tooltip mit Verzögerung.

**B5 Tabs und Splitter.** Reiter, Akkordeon, ziehbare Trenner zwischen Bereichen. Für jede
App mit Seitenleiste unverzichtbar.

**B6 Text, der mehr kann.** Zeilenumbruch, Ausrichtung, RichText mit Inline-Formaten und
anklickbaren Bereichen, auswählbarer statischer Text, Icon-Schriften.

**B7 Drag & Drop**, innerhalb der App und vom Betriebssystem herein (Datei aufs Fenster ziehen).

**B8 Animation.** Timeline pro Widget, Easing, Übergänge zwischen Zuständen. Für Apps kein
Luxus, weil ohne Übergänge alles billig wirkt.

**B9 Feinschliff.** Bildlauf mit Trägheit und sichtbarer Leiste, Fortschritts-Spinner,
Zahlenfeld mit Steppern, Datumswähler, Farbwähler, Suchfeld, Badge, Trennlinie, Umschalter.

**B10 Zugänglichkeit und Lokalisierung.** Tab-Reihenfolge (der Fokus existiert schon, die
Reihenfolge ist heute räumlich), Tastaturkürzel als Asset, Textkatalog mit Sprachumschaltung
zur Laufzeit, Schriftgrößenskalierung, Kontrastprüfung.

---

## 5. Block C: Skript-API-Lücken

Die 19 Gruppen sind Spielgruppen. Für Apps fehlen ganze Bereiche. Jede neue Gruppe kostet
einen Registry-Eintrag und leuchtet danach in HorizonCode, Lua und Python gleichzeitig auf,
das ist der billige Teil.

| Gruppe | Warum | Aufwand |
|---|---|---|
| `app` | Fenster, Beenden, Version, Pfade, Kommandozeile | klein |
| `clipboard` | Text und Bild rein/raus. SDL3 kann es fast geschenkt | winzig |
| `dialog` | Datei öffnen/speichern, Ordner wählen, Nachricht/Frage. SDL3 hat native Dialoge | klein |
| `fs` **entsperrt** | Apps müssen an beliebige Pfade. Modell: der Dialog **erteilt** den Pfad, dann ist er frei. Dazu Verzeichnisse listen, Metadaten, umbenennen, kopieren, Datei-Watcher | mittel |
| `http` | GET/POST/JSON, asynchron mit Callback. Ohne Netz ist eine App heute halb tot | mittel |
| `json` | Parsen und Schreiben in Skript-Werte. nlohmann liegt schon im Baum | klein |
| `datetime` | Jetzt, Formatieren, Parsen, Differenz, Zeitzone | klein |
| `prefs` | Kleine persistente Einstellungen, getrennt vom Save-System (das ist für Spielstände gedacht und an eine Vorlage gebunden) | klein |
| `timer` | Intervall und einmalig, mit Abbrechen. HorizonCode hat Delay, aber keinen wiederholenden Timer | klein |
| `process` | Externes Programm starten, Ausgabe lesen, URL im Browser öffnen. `HE::Proc` existiert schon | klein |
| `notify` | Systembenachrichtigung | klein |
| `db` | Optional, SQLite. Erst wenn es jemand braucht | groß |
| `print` | Drucken/PDF. Ehrlich gesagt: erstmal nicht | groß |

Sicherheitsseite: eine exportierte App darf all das, ein Skript im Editor sollte nicht
ungefragt fremde Verzeichnisse löschen. Ein Berechtigungsmodell im `.heproj`
(„diese App darf Netz/Dateien/Prozesse") ist billiger, wenn es von Anfang an dasteht.

---

## 6. Block D: „viel vordefiniert"

Das ist der Teil, der über Erfolg entscheidet. Ein UI-Framework mit 30 Primitiven ist eine
Zumutung, wenn man einen Einstellungsdialog will.

**D1 Theme-System.** Ein Theme-Asset: Farbrollen (Hintergrund, Fläche, Rahmen, Text, gedämpfter
Text, Akzent, Warnung, Fehler, Erfolg), Abstands-Skala, Radien, Schatten, Typografie-Stufen
(Titel/Überschrift/Fließtext/Klein/Mono). Widgets referenzieren **Rollen**, keine Literalfarben.
Hell/Dunkel als zwei Belegungen desselben Themes, umschaltbar zur Laufzeit, mit „folge dem
System" als Standard. Zwei mitgelieferte Themes, dazu das Amber-Theme des Editors als drittes.

**D2 Komponentenbibliothek als WidgetRef-Assets.** Genau dafür wurde `WidgetRef` gebaut,
es fehlt nur der Inhalt. In `EngineContent` ausliefern, versioniert, vom Nutzer kopierbar
und veränderbar:

- Formularzeile (Beschriftung links, Bedienelement rechts, Hilfetext, Fehlerzustand)
- Einstellungsseite mit Abschnitten
- Dialog (Titel, Inhalt, Knopfzeile, Abbrechen/OK, Escape und Enter verdrahtet)
- Bestätigungs-, Fehler- und Fortschrittsdialog
- Titelleiste mit Fensterknöpfen (für randlose Fenster)
- Seitenleiste mit Navigationspunkten
- Werkzeugleiste, Statusleiste
- Suchfeld, leerer Zustand, Ladezustand, Fehlerzustand
- Karte, Listenzeile, Kopfzeile mit Aktionen
- Umschalter, Segmentkontrolle, Stepper, Tag-Feld
- Assistent (mehrere Schritte, Zurück/Weiter)
- Über-Dialog, Einstellungen-Fenster

**D3 App-Vorlagen.** Beim Anlegen wählbar, jede ein lauffähiges Skelett:
*Leeres Fenster*, *Seitenleisten-App* (Navigation links, Inhalt rechts), *Assistent*,
*Dashboard*, *Formular/Editor*, *Werkzeug mit Werkzeugleiste*. Wer eine Vorlage nimmt und
F5 drückt, sieht sofort eine echte App.

**D4 Layout-Hilfen.** Abstände aus der Theme-Skala statt Zahlen, Ausrichtungshilfen und
Einrasten im Designer, „an Inhalt anpassen" als Größenmodus, Mindest-/Maximalgrößen.

---

## 7. Block E: Editor- und Projektseite

**E1 App-Projekttyp.** Beim Anlegen „Anwendung" neben „Spiel". Setzt `appMode`, legt keine
Szene an, erzeugt eine GameInstance und ein Haupt-Widget, öffnet direkt den UI-Designer statt
des 3D-Viewports.

**E2 App-Modus im Editor.** Viewport, Outliner, Details, Terrain, Landschaft, Physik und
Rendering-Einstellungen sind in einem App-Projekt Lärm. Panels ausblenden, Layout auf
Designer plus Logik plus Assets plus Vorschau umstellen.

**E3 Designer-Vorschau, die die Wahrheit zeigt.** Heute zeichnet der Designer eine
ImGui-Nachbildung (`drawElementPreview`). Bei einem Spiel-HUD verzeiht man die Abweichung,
bei einer App nicht. `RenderWidgetThumbnail` zeigt, dass echtes Engine-Rendering in ein Panel
möglich ist. Das ist die Entscheidung aus Abschnitt 9.

**E4 Vorschau starten ohne Vollstart.** Aktuelles Widget in einem echten Fenster laufen
lassen, mit Skript, ohne Export. Idealerweise mit Hot-Reload beim Speichern.

**E5 Export-Voreinstellung „App".** Kein Startszenen-Feld, dafür Icon, Version,
Bundle-Identifier, Copyright, Fenstergröße, Theme-Standard. Auf macOS ein `.app`, auf Windows
ein Ordner plus optional Installer, auf Linux ein `.tar.gz` oder AppImage.

**E6 Kein Spiel-Beiwerk in der App.** Kein Splash mit Engine-Logo, kein Standard-Vollbild,
kein „Escape beendet". Klingt trivial, ist der Unterschied zwischen professionell und Bastelei.

---

## 8. Block F: Plattform-Parität

**F1 Texturen auf D3D11/D3D12/Vulkan.** Widget-Bilder zeichnen dort heute nur den reinen
Farbton. Eine App ohne Bilder und Icons auf Windows ist kein Produkt. Das ist ein Blocker,
kein Feinschliff.

**F2 HiDPI.** Die Pixel-Größe kennt das Fenster schon. Ob der Canvas-Skalierungsmodus die
Systemskalierung auf Windows und Linux korrekt aufnimmt, ist ungeprüft.

**F3 Randlose Fenster mit eigener Titelleiste**, inklusive Ziehen, Größenänderung an den
Rändern und Snap-Verhalten auf Windows.

**F4 Systemcursor** (Text-Cursor über Textfeldern, Größenänderungs-Cursor an Splittern).
`UICursor` existiert bereits als Enum, die Verdrahtung ist zu prüfen.

---

## 9. Zwei Entscheidungen, die diese Richtung erzwingt

Beide standen schon offen; die App-Richtung entscheidet sie.

**(a) Zwei UI-Systeme nebeneinander.** Es gibt den Widget-Baum und die ECS-`UIElementComponent`
mit `UISystem`. Für Apps gibt es keine ECS. **Empfehlung: der Widget-Baum gewinnt**, das
ECS-UI wird eingefroren und später auf den Baum abgebildet oder entfernt. Alles andere
verdoppelt jede Zeile Arbeit in diesem Dokument.

**(b) Designer-Vorschau.** **Empfehlung: auf echtes Engine-Rendering umstellen** (E3). Bei
Apps ist die Vorschau das Produkt.

---

## 10. Reihenfolge

Leitgedanke: der erste Meilenstein ist eine **exportierbare Taschenrechner- oder Todo-App**,
die im Leerlauf nichts verbraucht. Alles, was dafür nicht nötig ist, kommt später.

**Welle 1, das Fundament**
- A1 weltloser Modus
- A2 ereignisgetriebenes Zeichnen
- A4 `app`-Gruppe (Titel, Größe, Beenden, Schließen-Veto)
- C: `clipboard`, `dialog`, `json`, `prefs`, `timer`
- B1 TextInput richtig
- E1 App-Projekttyp plus eine Vorlage
- E5 Export-Voreinstellung „App"
- Abnahme: Todo-App, exportiert als `.app`, speichert nach `~`, unter 2 % CPU im Leerlauf

**Welle 2, brauchbar**
- D1 Theme-System
- D2 erste zwölf Komponenten
- B2 ListView, B3 Grid, B4 Dialoge/Kontextmenü/Tooltip
- C: `fs` entsperrt mit Dialog-Erteilung, `datetime`, `process`
- E3 echte Designer-Vorschau
- F1 Texturen auf D3D/Vulkan

**Welle 3, konkurrenzfähig**
- A6 Menüleiste, A7 Icon und Dateitypen
- B5 Tabs/Splitter, B6 RichText, B7 Drag & Drop, B8 Animationen
- C: `http`, `notify`
- D3 alle App-Vorlagen
- A3 schlanker Runtime

**Welle 4, Kür**
- A5 mehrere Fenster
- B9 Feinschliff-Widgets
- B10 Zugänglichkeit und Lokalisierung
- `db`, Drucken, Auto-Update

---

## 11. Risiken und Fallen

- **Zwei Betriebsmodi bedeuten zwei Testpfade.** Ein Schalter, der die halbe Initialisierung
  überspringt, verrottet still. Ab Tag eins ein Test, der eine App-Konfiguration headless
  hochfährt.
- **Der Leerlauf-Modus ist der subtilste Punkt.** Ein vergessenes Dirty-Flag heißt: eine
  Änderung erscheint erst, wenn die Maus wackelt. Ein zu großzügiges Flag heißt: es war
  umsonst. Eine sichtbare Diagnosezeile („Frames pro Sekunde im Leerlauf") spart Stunden.
- **Widget-Extract und Pointer-Test müssen durch dieselbe Auflösung.** Steht schon in der
  bestehenden Fallen-Liste und gilt hier doppelt, weil Apps pixelgenau geklickt werden.
- **`fs` zu entsperren ist eine Einbahnstraße.** Das Berechtigungsmodell vorher festlegen.
- **Der Editor ist selbst eine ImGui-App.** Die Versuchung, App-Widgets „schnell in ImGui"
  zu bauen, ist groß und falsch: das Framework muss sich an der eigenen Bibliothek beweisen.
  Wenn die Komponenten gut sind, sollten mittelfristig Editor-Panels damit gebaut werden können.
- **Umfang.** Dieses Dokument beschreibt ohne Übertreibung mehrere Monate. Welle 1 alleine
  ist bereits ein vorzeigbares Produkt und der einzige Teil, der jetzt zählt.

---

## 12. Was ausdrücklich nicht gebaut wird

Web-Export, Mobile, ein Browser-Widget, ein Rich-Text-Editor auf Word-Niveau, ein
Diagramm-Framework, ein eigenes Fenstersystem statt SDL. Alles davon ist ein eigenes Projekt.
