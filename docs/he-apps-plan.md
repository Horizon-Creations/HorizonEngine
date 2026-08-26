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

**A0 Backend-Baseline, gesteuert von einem Schalter beim Anlegen des Projekts.** Zwei
Entscheidungen des Users, die zusammengehören.

*26.08.2026:* eine App braucht keine visuellen Fähigkeiten, für die sich fünf Backends
anstrengen müssten, also zielt sie nur auf **Metal (macOS) und OpenGL (Rest)**. D3D und Vulkan
fallen als App-Ziel weg (siehe F1, das damit aus diesem Plan ausscheidet). Beide
Baseline-Backends können heute schon alles, was UI braucht: Eckenradius, Texturen,
Material-Graphen.

*27.08.2026:* beim Anlegen eines Werkzeugprojekts gibt es ein Häkchen **„Advanced Shader
Effects"**, und es entscheidet, **welcher einzige Renderer** in den App-Runtime gelinkt wird:

| Häkchen | Renderer | Materialien | Effekte |
|---|---|---|---|
| **aus** | `RendererSoftware`, als eigene Rendering-Bibliothek, der einzige in HorizonRendering | keine, im Editor auch nicht erstellbar | Schicht 0 (Eckenradius, Rahmen, Verläufe, Schatten) |
| **an** | der Forward-Renderer, ebenfalls als einziger gelinkt | Material-Graphen, Schicht 1 | Schicht 0 + Schicht 1 |

Das ist die sauberere Version dessen, was in A3b als „dünner UIRenderer pro Backend" stand.
Eine App ohne Effekte linkt dann nämlich **gar keinen GPU-Stack**, statt einen dünn
geschnittenen. Und der Schalter fällt genau auf die Naht, die D5 ohnehin gezogen hat:
Schicht 0 kommt ohne Shader-Graph aus, Schicht 1 ist der Graph. „Advanced Shader Effects"
heißt also schlicht „Schicht 1", und alles hängt an einem Häkchen statt an drei.

Damit ist auch Block G rehabilitiert: ich hatte den Software-Renderer gestern auf „optional,
Welle 4" zurückgestuft, mit dem Argument, Mesa llvmpipe erledige „läuft überall" billiger.
Das Argument des Users ist ein anderes und besseres, nämlich **Paketgröße**, und darauf
antwortet llvmpipe überhaupt nicht: das ist eine zusätzliche DLL, keine kleinere. Für
Advanced-**an**-Apps auf treiberlosen Windows-Kisten bleibt llvmpipe eine Zeile wert, mehr
nicht.

**Offen und nachzufragen:** „der OpenGL-Forward-Renderer" schließt wörtlich Metal aus und
widerspräche der Entscheidung vom Vortag. Ich lese es als „der Forward-Pfad statt des
deferred", also Metal auf macOS und GL sonst. Die wörtliche Lesart hätte einen echten Vorteil:
**ein einziger Shader-Dialekt** (GLSL410) für die vorkompilierten UI-Material-Varianten statt
MSL und GLSL nebeneinander. Auf die Zahl der Binärsätze wirkt sich das nicht aus, jede
Plattform baut ohnehin ihre eigenen.

**Die 4.1 ist keine Anforderung, sie ist eine Untergrenze.** Erste Fassung dieses Abschnitts
wollte den Kontext außerhalb von macOS auf 4.1 herunterziehen. Das war unbegründet: die 4.1
stammt allein daher, dass macOS OpenGL dort deckelt, und auf macOS läuft eine App auf Metal.
Auf Windows und Linux gibt ein normaler Treiber 4.6, und es kostet nichts, das zu nehmen.

Was aber bleibt, und im Code nachgesehen ist: `Window::Init` fordert außerhalb von macOS
**hart 4.6 an und hat keinen Rückfall** (`Window.cpp:52`). Schlägt `SDL_GL_CreateContext`
fehl, wirft es und die Anwendung stirbt. Auf 4.6 fehlt es genau dort, wo der Rückfall aus
Block G greifen soll: Mesa llvmpipe deckelt unterhalb von 4.6, ältere Intel-iGPUs auf Windows
liefern 4.4 oder 4.5, VM-GL-Stacks oft nur 3.3 bis 4.1. Eine App, die dort gar nicht startet,
ist schlimmer als eine ohne Rundungen.

Der Punkt ist also **nicht** „4.1 anfordern", sondern eine **Leiter**: 4.6 anfordern, bei
Fehlschlag 4.5, dann 4.3, dann 4.1, und die erreichte Version protokollieren. Ein paar Zeilen,
der Normalfall bleibt exakt wie heute, und „läuft überall" wird wahr statt behauptet. Für die
UI reicht jede Stufe, ihre Shader sind `#version 410 core`. Geprüft: das Absteigen ist
gefahrlos, denn der einzige `#version 430`-Shader und alle drei `glDispatchCompute`-Stellen
gehören zur GI und hängen an `m_giSupported` (`GLAD_GL_VERSION_4_3`), schalten sich unterhalb
von 4.3 also von selbst ab. Eine App ohne Welt berührt sie ohnehin nie, und ein Spiel verliert
auf so einer Maschine GI statt zu sterben.

**A1 Weltloser Modus.** Ein Schalter im Projekt (`.heproj` „appMode") und in `project.hcfg`.
Ist er an, erzeugt der Runtime keine `HorizonWorld`, keine `PhysicsWorld`, keine Default-Kamera,
kein Audio-System-Tick, keinen Szenen-Ladepfad. Der Renderer macht genau einen Pass: Clear plus
UI. Heute wird all das in `OnInit` bedingungslos aufgebaut.

**A2 Ereignisgetriebenes Zeichnen.** Der Loop in `Application::Run` läuft dauerhaft; es gibt
nur einen optionalen FPS-Deckel per `SDL_DelayNS`, und der greift nur ohne VSync. Für Apps
braucht es `SDL_WaitEvent` mit Timeout plus ein Dirty-Flag: neu zeichnen, wenn ein Event
ankam, ein Widget sich geändert hat, eine Animation läuft oder ein Timer fällig ist. Sonst
schlafen. Das ist der Unterschied zwischen 0,3 % und 40 % CPU im Leerlauf.

**A3 Schlanker Runtime.** Gemessen statt geschätzt, an `out/deploy/Game` (macOS, Release):

| Bestandteil | Größe | Braucht eine App das? |
|---|---:|---|
| Python (`libpython3.14` + `python314.zip` + `lib-dynload`) | **52,9 MB** | nur bei Python-Projekten, **schon gegated** (`settings.bundlePython`) |
| `libHorizonScene` | 25,4 MB | nein: enthält Jolt, Recast/Detour, Terrain, Animation, Partikel |
| `libHorizonRendering` | 22,7 MB | zum kleinen Teil: alle Backends + glslang/SPIRV-Cross |
| `libHorizonCore` | 12,0 MB | ja, aber inklusive Lua und der Textur-Encoder (Cook-Zeit) |
| `libcrypto.3` | 4,8 MB | nur bei verschlüsselter Pak |
| `libSDL3` | 4,3 MB | ja |
| `libHorizonNet` | 2,5 MB | nur mit Netzwerk |
| `HorizonGame` | 2,0 MB | ja |
| zstd + lz4 | 0,8 MB | ja |
| **Summe** | **123 MB**, ohne Python **≈ 70 MB** | |

Eine Todo-App in HorizonCode wiegt heute also rund 70 MB. **Ziel: unter 25 MB**, und die Zahl
gehört als Schwelle in einen ctest oder CI-Schritt, sonst kriecht sie zurück.

**A3a Diät zur Linkzeit, billig.** Nichts davon ist Umbau, nur Weglassen:
- Python: schon erledigt, ein Nicht-Python-Projekt trägt die 52,9 MB nicht.
- Backends: die Renderer sind bereits **eigene statische Bibliotheken** (`RendererOpenGL`,
  `RendererMetal`, `RendererVulkan`, `RendererD3D11/12`). Was eine App nicht anzielt, wird
  nicht gelinkt. Kein Code muss dafür angefasst werden.
- `libHorizonNet` und `libcrypto` nur, wenn Netzwerk beziehungsweise Pak-Verschlüsselung
  wirklich benutzt werden (`HE_PREFER_MBEDTLS` existiert als Schalter).

**A3b Der strukturelle Teil, die eigentliche Arbeit.** Der User hat den Punkt gemacht: wer nur
UI zeichnet, soll nicht den ganzen GL- oder Metal-Renderer mitschleppen. Richtig, und der Weg
dahin ist **nicht**, die 11.000-Zeilen-Renderer mit `#ifdef` zu zerschneiden. Das erzeugt
genau die zweite Konfiguration, die laut Risikoliste still verrottet.

Stattdessen entscheidet der Advanced-Schalter aus A0, **welcher einzige Renderer gelinkt
wird**. Advanced aus: `RendererSoftware`, gar kein GPU-Stack. Advanced an: der Forward-Renderer
der Plattform, allein. Beides geht, weil `IRenderer` nur vier rein virtuelle Methoden hat und
die anderen 37 Standard-Rümpfe haben, und weil die Renderer schon eigene statische
Bibliotheken sind.

Für den Advanced-an-Fall bleibt zu entscheiden, ob dort wirklich der volle Forward-Renderer
gelinkt wird oder ein **dünner `UIRenderer`**, der nur Clear, UI-Pass und Present kann. Der
volle ist billiger zu haben (existiert), der dünne ist kleiner. Vorschlag: erst den vollen
nehmen, messen, und den dünnen nur bauen, wenn die 25-MB-Marke sonst reißt.

Bedingung in jedem Fall: den UI-Pass **einmal** herausziehen und von jedem Renderer benutzen
lassen, sonst driften Kopien auseinander. Genau diese Extraktion ist auch das, was das
Software-Backend aus Block G überhaupt bezahlbar macht.

Dazu zwei konkrete Punkte, beide am Code geprüft:
- **`WidgetManager` gehört nicht in `HE_Scene`.** Heute muss eine App die Bibliothek linken,
  in der Jolt und Recast stecken, nur um Widgets zu bekommen. Der Header zieht ausschließlich
  Core-Sachen (`UIWidget/*`, `HorizonCode/*`, `Renderer/UIRenderObject.h`, `Types/UUID.h`) und
  deklariert `ContentManager` vorwärts; `HorizonWorld` hängt laut Kommentar nur an der
  Lebensdauer. Das ist ein Umzug, kein Umbau. Mitzunehmen wäre `sortKey` aus `UISystem`.
- **Der Shader-Übersetzer muss aus dem App-Runtime raus, aber ohne Schicht 1 zu killen.**
  `MaterialShaderVariant` sagt es selbst: „baked at export time **so the shipped game never
  cross-compiles**", und hält Backend-Quelltext (MSL, HLSL, Desktop-GLSL) oder SPIR-V. Der
  Exporter erzeugt die Varianten bereits (`shaderBackends`). Nur: der **UI**-Materialpfad
  benutzt sie nicht, `GetOrBuildUIMaterialProgram` geht direkt auf
  `m_matShaderLib.fragment(...)`, also auf glslang. Die Arbeit hat deshalb **zwei** Hälften,
  und die zweite ist die, die man übersieht:
  1. Der UI-Pfad liest `precompiledShaders`, so wie der Mesh-Pfad es vormacht.
  2. Dieser Ladepfad muss **außerhalb** des `HE_HAVE_SHADERC`-Gates liegen. Heute baut
     `he_materialshader` gar nicht erst, wenn `he_shadercompiler` fehlt (`if(TARGET …)` in
     der CMake). Ohne diese zweite Hälfte hat ein shaderc-freier Build **keinen**
     UI-Materialpfad statt eines abgespeckten, und „eigene Shader in kleinen Apps" wird von
     wahr zu falsch.

**Ein Kostenpunkt, der leicht untergeht:** der Exporter kopiert die Binaries aus
`gameRuntimeDir` wörtlich. Mit dem Schalter aus A0 heißt das **drei Runtime-Ausprägungen pro
Plattform**, die gebaut, mit dem Editor ausgeliefert und beim Export ausgewählt werden:

| Ausprägung | Gelinkter Renderer | Shader-Übersetzer | Wofür |
|---|---|---|---|
| Spiel | alle Backends der Plattform | ja | wie heute |
| App, Advanced an | Forward-Renderer, einer | nein (vorkompilierte Varianten) | Werkzeug mit Material-Graphen |
| App, Advanced aus | `RendererSoftware`, einer | nein | Werkzeug ohne Effekte |

Ohne diese Zeile setzt der Plan Dateien voraus, die es nicht gibt. Und die Risikoliste bekommt
einen Eintrag: der headless-Starttest muss **beide** App-Ausprägungen fahren, sonst verrottet
eine davon still.

**Einordnung:** Welle 1 liefert bewusst noch die fetten ~70 MB, sonst verschiebt sich die
erste lauffähige App hinter einen Umbau. A3a gehört in Welle 2, A3b in Welle 3.

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

**B1 TextInput, Restarbeiten.** Korrektur gegenüber der ersten Fassung dieses Dokuments: das
Feld ist längst ein richtiges Textfeld, nicht „Anhängen und Backspace". Verifiziert im Code
(`UIElements.h:224`, `WidgetManager.cpp:619–780`, `GameApplication.cpp:1114–1160`): Cursor als
Byte-Offset auf UTF-8-Grenzen mit Klickpositionierung über `caretAtX`, Auswahl mit Anker,
Shift+Pfeil, Home/End, Entf, Ctrl+A, Ctrl+C/X/V gegen die SDL-Zwischenablage (im Spiel **und**
im Editor verdrahtet), Tippen ersetzt die Auswahl, Zeichenlimit in Zeichen statt Bytes,
Passwortmodus, `editable` und `selectable` getrennt, Platzhalter, Auswahlrechteck hinter den
Glyphen, Cursor als Strich an der richtigen Stelle, dazu `OnTextChanged`, `OnTextCommitted`,
`OnFocused`, `OnUnfocused`.

Was wirklich noch fehlt, klein und abzählbar:
- **Ziehen zum Auswählen.** Der Druck setzt Cursor *und* Anker (`setCaretFromPointer`); einen
  Ziehpfad gibt es nur für den Slider (`draggingSlider`).
- **Doppelklick wählt Wort, Dreifachklick wählt Zeile.**
- **Wortsprünge.** `TextEdit` kennt nur `Left, Right, Home, End, Delete, SelectAll`; Ctrl+Pfeil
  und Ctrl+Backspace fehlen.
- **Undo/Redo im Feld** (keine Historie pro Feld).
- **Waagerechtes Scrollen bei Überlänge.** `render` zeichnet ab `tp.x` ohne Versatz, also
  läuft der Cursor bei langem Text aus dem Feld heraus.
- **IME.** `SDL_EVENT_TEXT_EDITING` kommt im ganzen Baum nicht vor, ebenso wenig
  `SDL_SetTextInputArea` für das Kandidatenfenster. Ohne das ist CJK-Eingabe nicht benutzbar.
- **I-Beam automatisch.** `UICursor::Text` existiert, `hoverCursor` ist aber eine gestaltete
  Eigenschaft pro Element mit Standard `Default`, kein Automatismus für TextInput.
- **Eingabefilter** (nur Zahlen, Muster, Live-Validierung).

**B1b Mehrzeiligkeit** ist der einzige große Rest davon und deshalb ein eigener Punkt:
Umbruch, Cursor hoch/runter, Auswahl über Zeilen hinweg, senkrechtes Scrollen. Der Rest oben
ist Feinarbeit an etwas Fertigem.

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

**D5 Effekte und Materialien: drei Schichten.** Der Spagat zwischen „viel vordefiniert" und
„eigene Shader per Material-Graph" löst sich nur, wenn beides **nicht dasselbe** ist.

*Schicht 0, „Stil": Eigenschaften am Element, kein Shader.* Eckenradius pro Ecke, Rahmen mit
Breite und Farbe, linearer und radialer Verlauf, Schlagschatten (ein zweites Quad mit
Abfall), Innenschatten. Umgesetzt als Feldern an `UIRenderObject` plus Feature-Bits im
**eingebauten** UI-Shader jedes Backends. Geschlossenes Vokabular, überall identisch, kein
Shader-Übersetzen, kein Kompilieren, und vom Software-Rasterizer (Block G) darstellbar.
**Hier lebt der Großteil dessen, was „vordefiniert" bedeutet**, weil es genau das ist, was
Apps jeden Tag brauchen.

*Schicht 1, „Material": der Graph als Ausweg.* `MatDomain::UserInterface` existiert und
erzwingt schon den unlit-Schwanz. Der `uiVertex` liefert dem Fragment bereits `vUV` (das
UV-Rechteck des Quads), `vColor` (den Farbton), `vWorldPos` (die Bildschirmposition in Pixeln)
und über den Time-Node die Zeit. Damit sind Verlaufsanimationen, Rauschen, Puls, Wellen,
Scanlines und Ähnliches schon heute baubar. Es fehlt:
- **Parameter pro Instanz** (siehe die Falle unten), sonst teilen sich alle Nutzer eines
  Materials einen Wert.
- **UI-Nodes**: `ElementSize` in Pixeln, `ElementUV`, `RoundedRectSDF`, `BorderDistance`,
  und **Element-Zustand** (hover / gedrückt / fokussiert / deaktiviert) als Eingang. Den
  Zustand kennt der WidgetManager beim Extrahieren ohnehin, und er ist der Grund, warum ein
  einziges mitgeliefertes „Button-Glow" ohne Verdrahtung pro Widget funktionieren kann.
- **Mitgelieferte Bausteine als `MaterialFunction`-Assets.** Der `FunctionCall`-Node und
  MaterialFunction-Assets existieren bereits, das ist der fertige Mechanismus für eine
  Effektbibliothek, die sich im Graph-Editor zusammenstecken lässt statt kopiert zu werden.

*Schicht 2, der Rückfall-Vertrag.* Der Schalter aus A0 gibt ihm seine endgültige Form, und
zwar eine ganz einfache: **Schicht 1 ist genau das, was „Advanced Shader Effects" ein- und
ausschaltet.** Daraus folgt der Vertrag:
- Advanced **an**: beide Baseline-Backends fahren Material-Graphen, Materialien sind kein
  degradiertes Feature.
- Advanced **aus**: es gibt keine Materialien, weil der Editor keine anlegen lässt. Der Vertrag
  greift nur für Altbestand, also für Projekte, in denen der Schalter nachträglich umgelegt
  wurde: solche Quads zeichnen **Schicht 0 plus Farbton**.

Genau deshalb ist Schicht 0 ein geschlossenes Vokabular und keine Sammlung von Graphen. Und
sie wäre auch dann richtig getrennt, wenn es den Schalter nicht gäbe: sie kostet keinen
Shader-Übersetzungslauf, ist im Designer sofort sichtbar, im Theme (D1) als Rolle
referenzierbar und ohne Graph-Wissen bedienbar. Ein Eckenradius soll ein Zahlenfeld sein,
kein Knoten.

**Zwei Funde, die dabei aufgefallen sind:**

1. **`ui.setMaterialParam` ist eine Falle, keine Lösung.** Es läuft über
   `ScriptApi::setUIMaterialParam` und damit erstens ausschließlich über die ECS-
   `UIImageComponent`, also über genau das UI-System, das Abschnitt 9 einfrieren will, und
   zweitens schreibt es `content->setMaterialParam(materialAssetId, …)`, verändert also das
   **geteilte Asset**: zwei Widgets mit demselben Material ändern sich beide. Parameter pro
   Instanz brauchen einen echten Mechanismus, entweder einen Parameterblock pro Quad am
   `UIRenderObject` oder eine Instanz-Tabelle, die erst beim Zeichnen aufgelöst wird.

2. **Ein Backdrop-Node ist kein Node, sondern ein Arbeitspaket.** Weichzeichnen hinter dem
   Element (Glas, Milchglas, moderne Dialoge) heißt, dass der UI-Pass lesen muss, was hinter
   ihm liegt: Texturkopie auf GL und D3D, Framebuffer-Fetch oder Tile-Tricks auf Metal. Das
   ist ein eigener Punkt mit ehrlichen Kosten, nicht ein Kästchen im Graph-Editor. Es speist
   danach sowohl einen Schicht-1-Node als auch jede Glas-Komponente aus D2.

**Eine Falle für den App-Modus:** der Time-Node liest `heLight.sunDir.w`. Ohne Welt füllt
niemand den Szenen-Lichtblock, also müsste der UI-Pass die Zeit ausdrücklich einspeisen,
sonst steht jedes animierte Material auf 0. Billig jetzt, ärgerlich später.

**D4 Layout-Hilfen.** Abstände aus der Theme-Skala statt Zahlen, Ausrichtungshilfen und
Einrasten im Designer, „an Inhalt anpassen" als Größenmodus, Mindest-/Maximalgrößen.

---

## 7. Block E: Editor- und Projektseite

**E1 App-Projekttyp.** Beim Anlegen „Anwendung" neben „Spiel". Setzt `appMode`, legt keine
Szene an, erzeugt eine GameInstance und ein Haupt-Widget, öffnet direkt den UI-Designer statt
des 3D-Viewports.

**E1b Der Advanced-Schalter im Editor.** Das Häkchen aus A0 ist ein Feld in `ProjectData`,
neben `scriptLanguage`, und folgt genau dessen Vorbild. Dessen Kommentar beschreibt das
Muster schon: „This is a HARD restriction: the editor only offers the matching logic-authoring
assets (the Content Browser hides the other languages' creators)". Dieselbe Mechanik, nur für
Materialien.

Zu schließende Stellen, wenn der Schalter aus ist:
- Die Ersteller im Content Browser: „Material" und „Material Function"
  (`ContentBrowserPanel.cpp:2300`, `:2301`) und „Create Material Instance" (`:2755`).
- Der Material-Editor als Panel, samt Doppelklick-Öffnen.
- Die Material-Slots, wo ein Widget eines wählt (`UIEditorPanel`, `InspectorPanel`).
- Die Asset-Auswahl und die Thumbnails für Material-Assets.

Im Editor referenzieren 31 Stellen in 14 Dateien `AssetType::Material`, aber die meisten
gehören zu Terrain, Skeletal Mesh und Partikeln, also zu Panels, die E2 im App-Modus ohnehin
ausblendet. Die eigentliche Liste ist die oben.

**Zwei Regeln dazu, die sonst wehtun:**
- *„Nicht auffindbar" heißt Ersteller, Auswahl und Panel weg, nicht die Dateien.* Eine Datei,
  die es gibt und die niemand sieht, verwirrt Source Control und die Referenzsuche. Vorhandene
  Materialien bleiben im Browser sichtbar, aber gekennzeichnet.
- *Der Schalter ist nachträglich umlegbar, aber das Ausschalten fragt nach.* Es listet die
  betroffenen Materialien auf, wofür `findReferrers` schon da ist, und die referenzierenden
  Widgets zeichnen danach Schicht 0 plus Farbton.

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

**F1 UI-Renderpfad-Parität auf D3D11/D3D12/Vulkan — gehört nicht mehr in diesen Plan.** Der
Fund bleibt, die Einordnung ändert sich durch A0. Gemessen: der UI-Pass dieser drei Backends
zeichnet **ausschließlich** einfarbige Quads, Glyphen, Clip und Rotation. Der
D3D11-Konstantenpuffer hat kein Feld für den Eckenradius, und `cornerRadius` kommt in D3D11,
D3D12 und Vulkan überhaupt nicht vor (Metal und GL: ja). Es fehlen dort also Eckenradius,
Texturen und Materialien zugleich, ein abgerundeter Knopf ist dort ein Rechteck.

Für Apps ist das seit A0 gleichgültig, sie zielen auf Metal und GL. Der Punkt wandert damit
in die **Spiel-Spur**, zur ohnehin offenen Backend-Parität, und blockiert hier nichts mehr.

**F2 HiDPI.** Die Pixel-Größe kennt das Fenster schon. Ob der Canvas-Skalierungsmodus die
Systemskalierung auf Windows und Linux korrekt aufnimmt, ist ungeprüft.

**F3 Randlose Fenster mit eigener Titelleiste**, inklusive Ziehen, Größenänderung an den
Rändern und Snap-Verhalten auf Windows.

**F4 Systemcursor** (Text-Cursor über Textfeldern, Größenänderungs-Cursor an Splittern).
`UICursor` existiert bereits als Enum, die Verdrahtung ist zu prüfen.

---

## 8b. Block G: Software-Renderer (der Renderer für „Advanced aus")

> Dieser Block hat zwei Rückstufungen und eine Rehabilitierung hinter sich. Stand jetzt: er
> ist **nicht optional**, sondern der Renderer, den jedes Werkzeugprojekt ohne Advanced
> Shader Effects benutzt, und damit der Normalfall statt der Ausnahme. Siehe A0.

Nicht „ein Software-Renderer für die Engine". Ein **Software-Backend, das nur UI zeichnet**.
Der Unterschied ist der ganze Punkt: 3D auf der CPU (PBR, Schatten, GI, Terrain) wäre ein
eigenes Projekt mit fragwürdigem Nutzen. Die UI dagegen ist eine winzige, geschlossene
Sprache, und für eine App ist die UI alles.

**Warum es billig ist, belegt am Code:**
- `IRenderer` hat genau **vier rein virtuelle Methoden**: `Initialize`, `Shutdown`, `Render`,
  `GetCapabilities`. Die anderen 37 haben Standard-Rümpfe. Ein Backend, das nichts von SSR,
  GI, Bloom oder Skelett-Vorschauen weiß, lässt sie einfach stehen.
- `UIRenderObject` ist abgeschlossen: Rechteck, Farbe, Eckenradius, Textur *oder*
  Glyphen-Kachel, Clip-Rechteck, Rotation, Ebene. Mehr Vokabular gibt es nicht.
- Die UI kommt fertig extrahiert an (`m_renderWorld.uiObjects`), derselbe Weg, den OpenGL
  und Metal gehen.
- Die Schrift liegt **schon auf der CPU**: `BakedUIFont::pixels`, ein einkanaliger
  1024×1024-Atlas.
- Texturen liegen **auch schon auf der CPU**: `TextureAsset::data` behält die Pixelbytes.
  Einschränkung: `TextureFormat` kennt neben `RGBA8` auch `BC7`, `BC3` und `ASTC_4x4`; die
  müsste das Backend entweder auf der CPU auspacken oder ablehnen.
- Der Präzedenzfall steht im Repo: `tests/ImGuiSoftwareRaster.h`, 63 Zeilen, rastert
  ImGui-Dreiecke auf der CPU und speist `scripts/he_uishot.py`. Unsere Quads sind einfacher
  als ImGui-Dreieckslisten.

**Was es kostet:** ein echter Quad-Rasterizer, keine achsenparallelen Füllungen, weil
`rotation` gedrehte Rechtecke erlaubt. Dazu Kantenglättung an Eckenradien, bilineares
Abtasten für Glyphen und Texturen, Source-over-Blending, Scissor. Realistisch 600 bis 1000
Zeilen, plus Präsentation ins Fenster.

**Die harte Grenze, ausdrücklich:** `materialAssetId` kann ein CPU-Backend nicht bedienen, ein
Material ist ein übersetzter Shader-Graph. Hier greift der Rückfall-Vertrag aus D5: ein
Material-Quad zeichnet **Schicht 0 plus Farbton**, also Eckenradius, Rahmen, Verlauf und
Schatten, nur ohne den Graph. Deshalb ist Schicht 0 als geschlossenes Vokabular gebaut und
nicht als Sammlung von Graphen. Das gehört dokumentiert, nicht stillschweigend gemacht.

Eine dritte Möglichkeit gäbe es: einen kleinen **CPU-Interpreter für UI-Material-Graphen**.
Der Graph ist ein DAG aus einfachen Operationen, und für App-große Quads wäre das machbar.
Ich empfehle ihn **nicht** für Welle 2. Der Rückfall-Vertrag macht ihn entbehrlich, und es ist
genau die Sorte Punkt, die im Umfang explodiert.

**Mechanik:**
- `RendererBackend::Software = 5` **angehängt**, nie eingefügt. Der Kommentar an `Metal = 4`
  („appended last") sagt, dass die Reihenfolge tragend ist.
- Ein Fall in `Window::Init`: der überspringt für D3D, Vulkan und Metal schon die
  GL-Kontexterzeugung, Software verhält sich genauso.
- Präsentation über `SDL_GetWindowSurface` plus `SDL_UpdateWindowSurface`, alternativ eine
  streamende SDL-Textur. Kein GPU-Kontext, kein Treiber, keine Shader-Übersetzung.
- Ein Fall in `RendererFactory::Create`, und wählbar über das Backend-Feld, das die
  ausgelieferte `config.json` bereits hat.

**Zwei harte Bedingungen, die aus der Beförderung folgen.** Als optionaler Rückfall durfte
das Backend gemütlich sein, als Normalfall nicht:

1. **A2 ist Voraussetzung, nicht Kür.** Ein CPU-Rasterizer, der 60 mal pro Sekunde ein
   ganzes Fenster neu malt, ist inakzeptabel. Ereignisgetriebenes Zeichnen muss vorher stehen.
2. **Dirty Rectangles sind Pflicht.** Ein Retina-Fenster in Vollbild sind 3840 × 2160, also
   8,3 Millionen Pixel pro Vollbild-Neuzeichnung. Im Leerlauf rettet A2 das, beim Ziehen eines
   Sliders oder beim Vergrößern des Fensters nicht. Nur die geänderten Rechtecke neu zu malen
   ist der Unterschied zwischen flüssig und zäh.

Dazu wächst der Umfang: das Backend trägt jetzt **ganz Schicht 0**, also Eckenradius mit
Kantenglättung, Rahmen, Verläufe und Schatten, nicht nur die Grundformen. Ehrlicher als meine
erste Schätzung sind **1500 bis 2500 Zeilen**.

**Texturen:** die BCn- und ASTC-Frage erledigt sich per Export-Einstellung statt per
CPU-Dekoder. Ein Advanced-aus-Profil kocht UI-Texturen als RGBA8. Blockkompression ist eine
Optimierung für 3D-Speicherbedarf, UI-Texturen sind klein, eine App braucht das nicht.

**llvmpipe bleibt trotzdem eine Zeile wert**, aber nur noch für Advanced-**an**-Apps auf
Windows-Kisten ohne Grafiktreiber: dort `opengl32.dll` daneben legen. Für Advanced-aus ist es
gegenstandslos, da läuft ohnehin kein GL.

**Einordnung:** Welle 2, zusammen mit dem schlanken Binärsatz. Welle 1 liefert den Schalter
und die Editor-Sperren, exportiert aber übergangsweise noch den GPU-Runtime, siehe
Reihenfolge. Der Editor bleibt in jedem Fall auf der GPU.

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
- A0 Baseline festschreiben, plus die GL-Versionsleiter 4.6 → 4.5 → 4.3 → 4.1 statt des
  harten 4.6 ohne Rückfall
- A1 weltloser Modus
- A2 ereignisgetriebenes Zeichnen
- A4 `app`-Gruppe (Titel, Größe, Beenden, Schließen-Veto)
- C: `dialog`, `json`, `prefs`, `timer` (`clipboard` ist im Textfeld schon verdrahtet, fehlt
  nur als Skript-Gruppe)
- B1 TextInput-Restarbeiten (Ziehen zum Auswählen, Wortsprünge, Scrollen, Undo, I-Beam)
- E1 App-Projekttyp plus eine Vorlage, **E1b Advanced-Schalter im `.heproj` + Editor-Sperren**
- E5 Export-Voreinstellung „App"
- Abnahme: Todo-App, exportiert als `.app`, speichert nach `~`, unter 2 % CPU im Leerlauf

> **Zum Schalter in Welle 1:** er wird hier schon gesetzt und sperrt schon, aber ein
> Advanced-aus-Projekt exportiert übergangsweise noch den GPU-Runtime. Funktional ist das
> identisch, es gibt ja keine Materialien zu zeichnen, nur eben noch fett. Der Software-Runtime
> kommt in Welle 2 dazu. So bleibt „erste lauffähige App in Welle 1" stehen, ohne dass der
> Schalter erst später etwas bedeutet.

**Welle 2, brauchbar**
- G `RendererSoftware` als eigene Bibliothek, plus Dirty Rectangles (setzt A2 voraus)
- D5 Schicht 0: Eckenradius pro Ecke, Rahmen, Verläufe, Schatten, in Metal, GL **und Software**
- A3a Diät zur Linkzeit, plus die drei Runtime-Ausprägungen im Editor
- D1 Theme-System
- D2 erste zwölf Komponenten
- B2 ListView, B3 Grid, B4 Dialoge/Kontextmenü/Tooltip
- C: `fs` entsperrt mit Dialog-Erteilung, `datetime`, `process`
- E3 echte Designer-Vorschau

**Welle 3, konkurrenzfähig**
- A6 Menüleiste, A7 Icon und Dateitypen
- B1b mehrzeiliges Textfeld, B5 Tabs/Splitter, B6 RichText, B7 Drag & Drop, B8 Animationen
- C: `http`, `notify`
- D3 alle App-Vorlagen
- D5 Schicht 1: Parameter pro Instanz, UI-Nodes, MaterialFunction-Effektbibliothek, Backdrop
- A3b WidgetManager-Umzug, UI-Materialien aus vorkompilierten Varianten (beide Hälften!),
  Advanced-an-Ausprägung ohne Shader-Übersetzer, Größenschwelle in der CI

**Welle 4, Kür**
- A5 mehrere Fenster
- B9 Feinschliff-Widgets
- B10 Zugänglichkeit und Lokalisierung
- `db`, Drucken, Auto-Update

---

## 11. Risiken und Fallen

- **Zwei Betriebsmodi bedeuten zwei Testpfade, mit dem Advanced-Schalter sind es drei.** Ein
  Schalter, der die halbe Initialisierung überspringt, verrottet still. Ab Tag eins ein Test,
  der eine App-Konfiguration headless hochfährt, und ab Welle 2 muss er **beide**
  App-Ausprägungen fahren, Advanced an und aus, sonst verrottet eine davon.
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
