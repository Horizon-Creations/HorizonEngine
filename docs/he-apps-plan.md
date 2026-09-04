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

**Geklärt am 27.08.2026:** gemeint war eine Forward-*Implementierung*, um sich die
Feature-Last des vollen Renderers in der Binärgröße zu sparen, nicht GL statt Metal. Metal auf
macOS bleibt. Was die Messung dazu sagt, steht in A3b: der Feature-Kram kostet 0,6 MB, die
Abhängigkeit zum Shader-Übersetzer kostet 5, also wird die gekappt statt ein Renderer
geschrieben.

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

**A3 Schlanker Runtime.** *Korrektur: die erste Fassung dieser Tabelle nannte 123 MB. Die Zahlen
stammten aus dem Deploy-Baum im Worktree, der kein Release-Build ist. Gemessen am
Release-Stand (`cmake-build-release`, deckungsgleich mit dem Deploy im Hauptcheckout):*

| Bestandteil | Größe | Braucht eine App das? |
|---|---:|---|
| Python (`libpython3.14` 13,6 + `python314.zip` 10,1 + `lib-dynload` 28) | **51,7 MB** | nur bei Python-Projekten, **schon gegated** (`settings.bundlePython`) |
| `libHorizonRendering` | 6,8 MB | **davon ~5 MB glslang + SPIRV-Cross** (7652 Symbole), Renderercode nur 1,4 |
| `libHorizonScene` | 6,5 MB | nein: Jolt (3,7 statisch), Recast/Detour, Terrain, Animation |
| `libcrypto.3` | 4,6 MB | nur bei verschlüsselter Pak |
| `libHorizonCore` | 3,1 MB | ja, inklusive Lua (0,4) und der Textur-Encoder |
| `libSDL3` | 2,4 MB | ja |
| Net, zstd, exe, lz4, HorizonPython | 2,0 MB | teils |
| **Summe** | **77 MB**, ohne Python **≈ 25 MB** | |

Eine Todo-App in HorizonCode wiegt heute also schon rund **25 MB**, nicht 70. Die alte Zielmarke
„unter 25 MB" war damit bereits erfüllt und taugt nicht. Neue Marke: **höchstens 15 MB**,
erreichbar über shaderc raus (~4,5), crypto optional (4,6), Jolt und Recast aus Scene heraus
(~4), Net (0,6). Die Zahl gehört als Schwelle in einen ctest oder CI-Schritt, **gemessen am
Release-Artefakt** — dass ein falscher Baum die erste Tabelle ruiniert hat, ist genau der
Grund, warum die Messstelle mit in die Schwelle gehört.

**A3a Diät zur Linkzeit, billig.** *Diese Liste stand zuerst optimistischer da. Am Code
nachgemessen (`otool -L` am Deploy-Baum) hält die Hälfte nicht, und das ist wichtiger als eine
schöne Aufzählung:*
- **Python: erledigt.** Ein Nicht-Python-Projekt trägt die 54,7 MB nicht. Das ging nur, weil
  vorher die strukturelle Arbeit gemacht wurde — der Interpreter war eine **Ladezeit**-Kante von
  HorizonScene und ist jetzt ein zur Laufzeit geladenes Plugin. Der Exporter überspringt ihn
  danach namensbasiert.
- **Backends: strukturell wahr, praktisch noch nicht.** Die Renderer sind eigene statische
  Bibliotheken, aber `HorizonRendering` linkt sie **alle** und die Factory nennt sie alle. „Was
  eine App nicht anzielt, wird nicht gelinkt" ist damit erst wahr, wenn der Advanced-Schalter
  entscheidet, welcher einzige gelinkt wird — und das ist **A3b**, nicht A3a.
- **`libcrypto`: über mbedTLS erledigt, nicht übers Weglassen.** Es ist eine Ladezeit-Kante von
  `libHorizonCore` (`otool -L` zeigt sie), ein Überspringen beim Kopieren ergäbe also ein Spiel,
  das vor `main` im dyld stirbt. Der richtige Weg war schon gebaut: `HE_PREFER_MBEDTLS=ON` linkt
  mbedcrypto **statisch** hinein, dann gibt es die 4,8 MB gar nicht erst. Linux und Windows
  fuhren das in der CI längst, macOS zog mit `fb108c5f` (30.08.2026) nach — **damit ist dieser
  Punkt zu.** Ein lokaler Build ohne den Schalter zeigt weiterhin die Homebrew-`libcrypto`, und
  das ist Absicht (`CMakeLists.txt`: der Standard nimmt das schnellere System-OpenSSL); gemessen
  wird das Release-Artefakt, und das fährt den Schalter.
- **`libHorizonNet`: geht so nicht.** Ladezeit-Kante von der Spiel-Exe, von HorizonScene **und**
  von HorizonRendering. 0,7 MB, und der Weg dahin wäre dieselbe Plugin-Behandlung wie bei
  Python. Das gehört zu A3b und steht dort.

**Die Zahl ist jetzt eine Schwelle, kein Absatz.** `scripts/runtime_size.py` läuft als ctest
`runtime_size` über den Deploy-Baum, gruppiert nach **Namensmustern** (nie nach einer festen
Liste — was kein Muster trifft, landet sichtbar in „unaccounted" und zählt trotzdem mit) und
prüft zwei Grenzen: mit Python und ohne. Die Messstelle wird bei jedem Lauf gedruckt und in
jeder Fehlermeldung genannt, weil genau ihr Fehlen die erste Tabelle ruiniert hat. Fehlt der
Baum, ist das ein **SKIP** (Exit 2) und kein roter Lauf.

Die Grenzen stehen bewusst auf dem heute **gemessenen** Stand plus Luft (90 MB / 32 MB), nicht
auf den angestrebten 15: die sind das Ziel **nach** A3b, und eine Schwelle, die am Tag ihrer
Entstehung reißt, lernt jeder zu ignorieren. Sie ist da, um eine Regression zu fangen, und um
mit jedem Stück A3b eine Stufe zu sinken.

**Und sie misst heute genau eine Ausprägung**, den Spiel-Runtime, weil es nur die gibt. Die
beiden App-Ausprägungen aus der Tabelle weiter unten bekommen ihre eigenen Grenzen, wenn A3b
sie erzeugt.

**A3b Der strukturelle Teil, die eigentliche Arbeit.** Der User hat den Punkt gemacht: wer nur
UI zeichnet, soll nicht den ganzen GL- oder Metal-Renderer mitschleppen. Richtig, und der Weg
dahin ist **nicht**, die 11.000-Zeilen-Renderer mit `#ifdef` zu zerschneiden. Das erzeugt
genau die zweite Konfiguration, die laut Risikoliste still verrottet.

Stattdessen entscheidet der Advanced-Schalter aus A0, **welcher einzige Renderer gelinkt
wird**. Advanced aus: `RendererSoftware`, gar kein GPU-Stack. Advanced an: der Forward-Renderer
der Plattform, allein. Beides geht, weil `IRenderer` nur vier rein virtuelle Methoden hat und
die anderen 37 Standard-Rümpfe haben, und weil die Renderer schon eigene statische
Bibliotheken sind.

**Für den Advanced-an-Fall sagt die Messung etwas anderes, als die Intuition erwartet.** Der
Wunsch war eine eigene Forward-Implementierung, „um sich den unnötigen Feature-Kram von
OpenGL in der Binärgröße zu sparen". Nachgemessen an den Objektdateien:

| Artefakt | Größe |
|---|---:|
| `libRendererOpenGL.a` (der **ganze** Renderer, deferred, GI, SSR, CSM, clustered) | **0,6 MB** |
| `libRendererMetal.a` | 0,8 MB |
| `libglad.a` | 0,2 MB |
| glslang + SPIRV-Cross im fertigen `libHorizonRendering.dylib` | **~5 MB** |

Der Feature-Kram im Renderer kostet also **eine halbe Megabyte**. Was der volle Renderer
wirklich kostet, ist nicht sein Code, sondern seine **Abhängigkeitskante** zum
Shader-Übersetzer. Und die kappt man ohne einen dritten Renderer: mit den vorkompilierten
Varianten (beide Hälften weiter unten) und `HE_ENABLE_SHADERC=OFF`.

> **Nachtrag, ausprobiert statt gezählt (27.08.2026):** die Behauptung „ein shaderc-freier
> Build ist vorgesehen und nicht neu" stammte daher, dass `HE_HAVE_SHADERC` in
> `OpenGLRenderer.cpp` 15 mal und in `MetalRenderer.mm` 18 mal vorkommt. Ein
> `-DHE_ENABLE_SHADERC=OFF`-Build **bricht trotzdem ab**: `MetalRenderer.mm` benutzt an
> mindestens zehn Stellen (7917, 7923, 7929, 7956, 8219, 10996, 12028, 12117 …)
> `ResolveMaterialShader`, `GetOrBuildMaterialPipeline` und `matLight` **außerhalb** ihrer
> Wächter. Die Wächter zu zählen war die falsche Prüfung; die richtige war, es zu bauen.
> Das ist echte A3b-Arbeit und keine Konfigurationszeile.

**Empfehlung deshalb: für Advanced-an den vorhandenen Forward-Pfad nehmen, ohne
Shader-Übersetzer gebaut, und keinen zweiten Renderer schreiben.** Was von der Intuition
gültig bleibt, ist nicht die Binärgröße, sondern die **Laufzeit**: ob der volle Renderer beim
Start Schattenatlas, G-Buffer, SSAO- und GI-Ziele anlegt, die eine App nie benutzt, und wie
viele der eingebauten Shader er beim Start übersetzt. Die SSAO-Ziele werden faul angelegt
(`EnsureSSAOTargets` aus dem Pass heraus), der Rest ist zur Laufzeit nachzumessen. Fällt das
ins Gewicht, ist die Antwort „der App-Modus legt diese Ziele nicht an", nicht „ein neuer
Renderer".

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

**A6 Menüleiste.** **Fertig** (Abschnitte weiter unten): gezeichnet am 03.09.2026, nativ auf
macOS am 04.09.2026. Auf macOS die echte (der Editor hat mit `MacMenuBar` schon einen
Präzedenzfall), auf Windows/Linux eine gezeichnete im Fenster. Definiert als Laufzeit-API statt
als Asset, damit HorizonCode sie befüllen kann — die Begründung steht beim Abschnitt. Offen
bleiben nur Tastenkürzel und `enabled`/`checked`.

**A7 Systemintegration.** App-Icon, Bundle-Identifier, Dateitypen-Zuordnung, „Öffnen mit",
Autostart, Tray-Icon. Alles klein einzeln, zusammen der Unterschied zwischen „App" und
„Fenster mit Zeug drin". **Erledigt am 03.09.2026** (zwei Abschnitte weiter unten).

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

**E2/E3/E4 zusammengelegt: die Live-Vorschau.** Entscheidung des Users vom 27.08.2026, und sie
ersetzt drei getrennte Punkte durch einen. Ein App-Projekt zeigt **keinen Viewport**. An
derselben Stelle steht die laufende App als 2D-Bild, bedienbar, mit Hot-Reload, und **ohne
dedizierten Play-Modus**: was du an einem UI- oder HorizonCode-Asset änderst, ist sofort
sichtbar und anklickbar, und die Vorschau **behält ihren Zustand über den Reload hinweg**.

Dahinter liegt ein **Root-Widget** und eine **GameInstance**, deren `OnInit` genau dieses
Widget erzeugt. Das ist auch die Struktur, die der gepackte Build fährt, also übt die Vorschau
nicht etwas anderes ein als das Produkt.

*Warum das billiger ist als es klingt:* fast alles existiert und ist bereits verdrahtet. Der
Editor hat einen vollständigen Widget-Runtime (`m_editorWorld->widgets()`), der Zeiger- und
Tastaturweg ist schon über Viewport-Größe und Zeigerposition parametrisiert, und
`RenderExtractor::extractUI` ruft `widgets().extract` **ungegated** — die Widgets zeichnen
also bereits in die Viewport-Textur, ganz ohne Play-Modus. Was am Play-Modus hängt, sind nur
`widgets().tick`, `processPointer`/`processWheel`, die Tastaturzeilen und der
HorizonCode-Runtime-Tick.

**Stufe 1, mit vorhandener Mechanik (billig).** In einem App-Projekt: keine Welt in die
Viewport-Textur rendern, Widget-Tick und Eingabe **ohne** `simulating` laufen lassen, das
Panel „Live Preview" nennen, Fly-Kamera, Gizmo und Entity-UI-Interaktion abschalten. Dazu der
Startinhalt (Root-Widget + GameInstance), ohne den es nichts vorzuschauen gibt.

**Stufe 2, echte Arbeit, aber begrenzt.** Hot-Reload beim Speichern: ein geändertes
Widget-Asset erzeugt die Instanzen aus dem neuen Baum neu, ein geänderter Graph lädt neu. Für
den Graphen gibt es schon Maschinerie (`HcGraphHost`, `reloadFromDisk`) — die zuerst lesen,
nicht neu bauen.

**Stufe 3, der tiefe Teil: den Zustand behalten.** Hier liegen die Kanten, deshalb ist der
Umfang von v1 ausdrücklich klein:
- **Pro Element, über die Element-Id zugeordnet:** Text und Cursor eines TextInput, der Haken
  einer CheckBox, der Wert eines Sliders, der Versatz einer ScrollBox, die Auswahl einer
  ComboBox, der Fokus.
- **Pro HorizonCode-Instanz, über Name und Typ zugeordnet:** die Variablen.
- Alles, was sich nicht zuordnen lässt, bekommt den neuen Standardwert.

Zwei Vorbilder im Haus, die vorher zu lesen sind: `LinkRemapSnapshot` (die Falle mit dem
fromJson-Snapshot steht in den HC-Review-Funden) und die typisierte Feldzuordnung des
Save-Systems.

**Was ausdrücklich NICHT überlebt**, und das gehört in die Oberfläche, nicht nur hierher:
laufende Delay-Fortsetzungen, und der Zustand von allem, was **umbenannt** wurde. Ein
umbenanntes Element oder eine umbenannte Variable ist für die Zuordnung ein neues Ding, und
stillschweigend verlorener Zustand ist genau die Sorte Verhalten, über die man eine Stunde
rätselt.

**Eine Entscheidung, die der User noch treffen muss:** wenn die Vorschau immer läuft, sind
Gestalten und Benutzen dann dieselbe Fläche oder zwei? Mein Vorschlag: **zwei Ansichten auf
dieselben Assets.** Der Designer-Tab ist zum Bauen, die Live-Vorschau zum Bedienen. Sonst ist
jeder Klick beim Gestalten auch ein Klick in der App, und das Verschieben eines Knopfes drückt
ihn gleichzeitig.

*E3 ist damit erledigt, bevor es gebaut wurde:* die Live-Vorschau IST das echte
Engine-Rendering von Widgets, nur in einem anderen Panel. Für Spielprojekte bleibt die
ImGui-Nachbildung im Designer, bis jemand sie ablöst.

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
>
> Und die Begründung ist ausdrücklich **nicht** die Binärgröße: die Messung in A3 zeigt, dass
> ein GPU-Renderer selbst nur 0,6 bis 0,8 MB wiegt. Der Grund ist, dass eine App ohne Effekte
> dann **gar keine GPU und keinen Treiber** mehr voraussetzt.

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

### Umsetzungsstand (27.08.2026, Branch `claude/he-apps-ui-framework-83b5f7`)

**Gebaut und getestet (1971/1971 Tests grün, HorizonGame und HorizonEditor bauen sauber):**
- **A0 Versionsleiter.** `Window.cpp`: `kGlVersions` 4.6 → 4.5 → 4.3 → 4.1 (macOS nur 4.1,
  forward-compatible), `setGlAttributes` einmal vor der Fenstererzeugung und einmal pro
  Versuch, jede Stufe außer der letzten warnt statt zu sterben.
- **A1 Weltloser Modus.** `ProjectConfig::appMode` + `advancedShaderEffects` in den
  vorhandenen Flags-Wörtern (kein Formatsprung; `advancedShaderEffects` **negiert**
  gespeichert, damit jede ältere Datei als „Spiel mit Materialien" zurückliest, mit Test).
  `GameApplication` überspringt Szenenladen, Physik, Entity-/Player-/Animator-Hosts,
  Asset-Streaming, Kamerasteuerung und den ECS-Systemtick; Maus-Capture wird zurückgenommen.
  Welt und Standardkamera bleiben, weil die Widget-API über `HorizonWorld::widgets` läuft und
  der Renderpfad eine Kamera braucht.
- **A2 Ereignisgetriebenes Zeichnen.** `Application::setEventDriven` / `requestRedraw` /
  `setIdleHeartbeatMs` (Standard 100 ms), `Window::WaitForEvent` und `EventsLastPoll`. Ohne
  Ereignis, ohne Redraw-Wunsch und vor dem Herzschlag entfallen `OnRender`, `Render` und der
  Swap komplett. Für Spiele aus.
- **A4 `app`-Gruppe:** `setTitle`, `setSize`, `size`, `requestRedraw` (dazu das vorhandene
  `quit`), über neue `Ctx`-Callbacks, im Spiel-Runtime ans Fenster gebunden.
- **`clipboard`** (`getText`, `setText`, `hasText`) und **`dialog`** (`message`, `confirm`)
  über SDL3, blockierend und nativ.
- **E1b Advanced-Schalter:** `ProjectData::appProject` / `advancedShaderEffects`, im `.heproj`
  persistiert, Häkchen in der Anlegen-Maske, im Content Browser die Ersteller „Material",
  „Material Function" und „Create Material Instance" gesperrt, der Material-Editor zeigt
  stattdessen den Grund. Der Exporter schreibt beide Flags nach `project.hcfg`.

**Zweiter Durchgang, ebenfalls gebaut und getestet (1978/1978):**
- **E1 App-Projekttyp.** `ProjectPreset::Application` (angehängt, der Wert steht als int im
  `.heproj`), legt `Content/UI`, `Textures` und `Fonts` an, schreibt **keine** Startszene und
  setzt `appProject`. Die Preset-Wahl und das Flag können nicht mehr auseinanderlaufen: der
  Manager verodert beides zu einem `isApp`.
- **E5 Export.** Für ein App-Projekt blendet der Dialog das Startszenen-Feld aus, leert die
  Auswahl und packt **keine** Szene — sonst wäre die im Editor offene Szene mitgereist und der
  Runtime hätte sie im App-Modus abgelehnt.
- **`json`** (getString/getNumber/getBool/has/count/setString/setNumber/setBool) mit dotted
  Pfaden und `[i]`-Indizes, **`prefs`** (typisiert, als eine JSON-Datei in derselben Sandbox
  wie `fs`, Schreiben bei jeder Änderung) und **`datetime`** (now/format plus sieben Felder,
  lokale Zeit, `localtime_r`/`localtime_s` statt des geteilten Puffers).
- **B1-Rest, bis auf IME und Eingabefilter:** Wortsprünge (Ctrl **und** Alt, für beide
  Plattform-Konventionen), Ctrl+Backspace löscht ein Wort, **Ziehen wählt aus** (eigener
  `draggingText`-Zustand, der Anker bleibt am Druckpunkt), Doppelklick nimmt das Wort,
  Dreifachklick alles, **waagerechtes Scrollen** unter dem Cursor mit passendem Rückweg im
  Klick-Treffer, und der **I-Beam** über einem Textfeld, ohne dass jemand ihn setzt.
  Ein Textfeld **klippt jetzt seine eigenen Glyphen** (in `uiElementClipRect`), sonst liefe
  der herausgescrollte Text über seine Nachbarn.

**Noch nicht verifiziert:** A1 und A2 sind nie gelaufen. Es gibt noch kein lauffähiges
App-Projekt zum Starten, und der Leerlaufverbrauch ist damit gemessen worden: gar nicht. Genau
dafür steht der headless-Starttest in der Risikoliste.

**Dritter Durchgang (1980 Tests):**
- **E2 Stufe 1, die Live-Vorschau.** `uiLive = simulating || appProject`; Play-Modus wird in
  `setPlayMode` selbst abgelehnt (nicht nur versteckt, sonst reißt ein Kürzel die Widgets mit
  der Welt ab); Viewport-Leiste und Details-Panel weg; der Outliner zeigt die
  **Widget-Hierarchie** (`WidgetManager::liveIds`) und sagt bei leerer Liste, was das bedeutet;
  kein Mausfang; Designer speichert automatisch (zurückgehalten bis Maus los und kein Feld
  aktiv, sonst speichert jeder Frame eines Ziehens); Neustart der Vorschau von Hand über den
  Transportknopf und automatisch beim Speichern von Widget-, HC-Klassen- und Script-Assets.
- **Startinhalt** in der App-Vorlage: `RootWidget.hasset` plus `GameInstance.hcode` mit
  OnInit → Create Widget, als `HorizonCode::Graph` gebaut statt als JSON-Literal.
- **Feinkörnige Invalidierung.** `WidgetManager::consumeVisualDirty()` wird von allem gesetzt,
  was das Bild ändert (Property-Setter aus Skripten, Texteingabe, Fokus, Hover-/Press-Wechsel,
  Scrollen, Sichtbarkeit, Lebenszyklus), und **nicht** von bloßer Zeigerbewegung — das ist der
  Teil, den der Test festnagelt. Dazu wurde die Schleife in **zwei** Entscheidungen geteilt:
  *laufen* (Herzschlag, damit die Uhr nicht stehenbleibt) und *zeigen* (`WantsPresent()` nach
  `OnRender`, weil erst danach feststeht, ob sich etwas geändert hat). Ein Herzschlag ohne
  Änderung tickt die App also weiter, ohne die GPU anzufassen und ohne Swap.

**Vierter Durchgang (1983 Tests): Welle 1 ist inhaltlich fertig.**
- **Eingabefilter** am Textfeld: `Input Filter` (alles / ganze Zahlen / Dezimalzahlen / eigene
  Liste) plus `Allowed Characters`, gefiltert dort wo Text hereinkommt, also auch beim
  Einfügen. Ein `-` nur vorn und einmal, ein `.` nur einmal, beides gegen den wachsenden Text
  geprüft; ein eingefügtes `-12-34` behält `-1234` statt ganz abgelehnt zu werden.
- **IME**: `SDL_EVENT_TEXT_EDITING` in beiden Anwendungen, Preedit-Text am Cursor mit
  Unterstreichung gezeichnet und **nicht** Teil des Feldwerts, IME-Cursor innerhalb der
  Komposition, Commit beendet sie (sonst doppelt gezeichnet), `SDL_SetTextInputArea` pro Frame
  aus `focusedFieldRect`, damit das Kandidatenfenster am Feld steht.
- **Create Widget zeigt nicht mehr.** Entscheidung des Users: Create Widget erzeugt eine
  Instanz der Widget-Klasse, Show Widget bringt sie auf den Bildschirm. 40 Testfälle mussten
  angepasst werden. **Migrationshinweis für bestehende Projekte:** ein Graph, der bisher nur
  Create Widget hatte, braucht jetzt ein Show Widget dahinter.
- **Editor im App-Modus weiter entkernt:** Add-Menü auf vier Einträge, Export-Dialog ohne
  Fenstergröße, Fenstermodus, Backend, Mod-Support und (ohne Materialien) ohne vorkompilierte
  Shader, Szenen-Zeilen aus beiden Menüleisten (nativ **versteckt**, nicht ausgegraut).
- **Leistungsfund:** der App-Runtime schaltete für die leere Welt jeden Frame Bloom, SSAO,
  Anti-Aliasing, GI, SSR und den Deferred-Pfad ein — die volle Nachbearbeitungskette über
  nichts. Im App-Modus jetzt ausdrücklich aus (nicht weggelassen: der Renderer behält sonst
  seine eigenen Standards, und Bloom und SSAO sind bei ihm an).

**Nicht gebaut, mit Begründung:** die `timer`-Gruppe. Widgets bekommen ein Tick-Event,
Lua/Python ein `onUpdate`, und ein Timer, der einen Callback feuert, bräuchte genau die
Runtime-Arbeit, die diese beiden schon leisten. Was übrig bliebe, kann `datetime`.

**Was von Welle 1 offen bleibt, ist keine Zeile Code, sondern der Nachweis:** die Abnahme
(„Todo-App, exportiert, unter 2 % CPU im Leerlauf") steht aus, solange die Live-Vorschau beim
User nichts zeigt. IME ist zudem **nicht real geprüft** — dafür braucht es eine echte
Eingabemethode, headless testbar ist nur die Zustandsmaschine.

**Zwischenfall bei der Projekterzeugung (behoben):** „Application" war die sechste Vorlage in
einer Liste, die mit `height_in_items = 5` gezeichnet wurde — sie stand unterhalb des
sichtbaren Bereichs, also legte jeder ein Empty-Projekt an und bekam folgerichtig keinen
Startinhalt, kein kurzes Add-Menü und keine Vorschau. Beide Formulare zeigen jetzt alle
Vorlagen, und `ProjectPreset::COUNT` plus ein `static_assert` im Header machen aus dem
nächsten Auseinanderdriften einen Compile-Fehler statt einer stillen Fehlbedienung. Der
Advanced-Haken erscheint außerdem nur noch beim Application-Template.

---

### Welle 2, angefangen

**D5 Schicht 0, Teil 1: Rahmen.** `UIRenderObject` trägt `borderWidth` und `borderColor`,
`UIElement` die authored Eigenschaften (auf der Basis, angeboten nur wo es eine Fläche gibt).
Der WidgetManager **stempelt** sie nach `render()` auf die Fläche — das erste Quad des
Elements, und nur wenn es dessen ganzes Rechteck bedeckt. Genau dieser Test unterscheidet
einen Hintergrund von etwas, das darauf liegt: die Füllung eines Fortschrittsbalkens bekommt
so keinen eigenen Rahmen, ohne dass irgendein Widget-Typ von Rahmen wissen muss.

Die Mathematik ist in beiden Sprachen dieselbe: `d` ist eine vorzeichenbehaftete Distanz in
Pixeln, die Innenkante also `d + width`, und ein `mix` zwischen Rahmen- und Füllfarbe ist auf
beiden Seiten kantengeglättet. MSL mit `xcrun metal` und GLSL mit `glslangValidator` offline
übersetzt; **optisch nicht verifiziert**.

**D5 Schicht 0, Teil 2: Verläufe.** `gradient` / `gradientColor` / `gradientAngleDeg` am
Quad, drei gestaltete Eigenschaften am Element. Der Winkel läuft im Uhrzeigersinn von „unten",
also blendet 0 von oben nach unten und 90 von links nach rechts — die senkrechte Blende ist
das, was ein Knopf oder eine Kopfzeile fast immer will, und der Normalfall soll der sein, für
den man nichts eintippen muss. Berechnet im 0..1-Raum des Quads, damit die Blende der Box
folgt und nicht dem Bildschirm. Der Verlauf gehört der **Füllung**, nicht dem Rahmen.

**Wichtig für den Editor-Umbau:** der Designer hat einen **generischen** Property-Editor über
`UIPropDesc`. Jedes Attribut, das sich in vorhandene `UIPropType`-Kinder zerlegen lässt,
bekommt sein Bedienelement damit umsonst — deshalb sind „Gradient" (Bool), „Gradient Color"
(Color) und „Gradient Angle" (Float) drei Eigenschaften statt eines Verlaufs-Objekts. Wo das
nicht geht, ist die Handarbeit fällig, die der User angemahnt hat.

**D5 Schicht 0, Teil 3: Eckenradius als gestaltete Eigenschaft.** Vorher buk ihn jeder Typ in
seinen `render()` ein — der Button 6, ComboBox und ProgressBar 4, das Panel gar keinen. Jetzt
liegt er auf der Basis, wird wie Rahmen und Verlauf gestempelt, und die Typen setzen ihren
alten Wert im Konstruktor als Standard, damit nichts anders aussieht. Zwei Fallen dabei: der
`render()` darf ihn **nicht mehr mitgeben** (sonst doppelt, einmal unskaliert), und der
Schreiber legt ihn nur ab, wenn er vom Typ-Standard **abweicht** — sonst bekäme jeder je
gespeicherte Button ein `cornerRadius: 6` für einen Wert, der ohnehin der Standard ist.

**Der Button ist eine Fläche geworden (BREAKING, mit Migration).** Er zeichnet seine drei
Zustände und sonst nichts; was auf ihm steht, sind **Kinder**, die in seinem Rechteck ankern
wie die Kinder jedes anderen Elternteils. Das ist es, was einen Icon-Button oder eine nach
links gerückte Beschriftung überhaupt möglich macht — eine eingebaute zentrierte Zeichenkette
konnte immer nur ein Layout.

Seine Eigenschaften sind damit **Normal/Hovered/Pressed Color**, dazu die geteilten
Flächen-Eigenschaften (Eckenradius, Rahmen, Verlauf) und die Basis-Eigenschaften. Text,
FontSize und Text Color sind weg.

**Migration:** `uiWidgetTreeFromJson` verwandelt die alte Beschriftung in ein Text-Kind
(zentriert, über die ganze Fläche gespannt, `hitTestable` aus, damit es den Klick nicht
klaut). Ausgelöst wird sie vom **alten JSON-Schlüssel**, nicht von einem Feld — nach einmal
Speichern ist der Schlüssel fort und die Migration wirkungslos, was das doppelte Anlegen
verhindert. Ein Test fährt genau diesen Weg zweimal.

**Zur Erinnerung des Users an die Property-Editoren:** für diese Runde war keine Handarbeit
nötig, der Editor ist generisch über `UIPropDesc`. Sechs Testfälle mussten angepasst werden,
weil sie auf der alten Button-Beschriftung standen — genau dafür ist die angepinnte
Eigenschaftsliste da.

**Nachtrag: drei Dinge, die der Umbau erst sichtbar gemacht hat.**

**1. Der Designer ließ nur Panels Kinder aufnehmen.** Ein hartes
`type() == UIWidgetType::Panel` an sieben Stellen (Hierarchie-Drop, Palette-Drop auf die
Leinwand, Klick in der Palette, jeweils für neue Elemente und für WidgetRefs). Der Baum selbst
hat Verschachtelung nie eingeschränkt, das war reine Designer-Regel — und sie hat genau das
verhindert, was der Button-Umbau möglich machen sollte. Jetzt entscheidet
`UIElement::acceptsChildren()`: Panel, **Button** und die drei Layout-Boxen. Die Boxen konnten
vorher übrigens auch keinen Drop annehmen, obwohl sie für nichts anderes existieren.

**2. Der Zeiger nahm nicht das oberste Widget.** `processPointer` verwarf jedes Element, das
nicht *reagiert* (`!isInteractive && hoverCursor == Default`) — ein Panel über einem Button war
also ein Panel, durch das man hindurchklickt, obwohl `hitTestable` seit jeher „undurchlässig für
den Zeiger" behauptet. Die Regel ist jetzt die, die der Kommentar immer beschrieb: **oberstes
`hitTestable`-Element gewinnt**, und von dort **blubbert das Ereignis nach oben** zum ersten
Vorfahren, der reagiert. Beides zusammen ist nötig — ohne das Blubbern wäre eine Beschriftung
auf einem Button ein Loch im Button, ohne das Blockieren bliebe der Durchklick. Zwei Tests
halten die beiden Hälften fest.

**3. Zwei identische 3×3-Raster in einem Panel.** Das Anker-Raster (4×4, bernstein) und das
neue Text-Ausrichtungsraster (3×3, bernstein) sahen gleich aus, und im Testprojekt des Users
hatte das Label prompt einen Anker statt einer Ausrichtung bekommen (nachgerechnet an der
gespeicherten `.hasset`: `pos [90,0] size [180,48]` ist exakt das Ergebnis von
`uiReanchorKeepingRect` auf Mitte-links). Das Ausrichtungsraster heißt jetzt **„Text Align"**
und zeichnet zwei gestapelte Textzeilen in kühlem Grau statt Punkte und Balken in Bernstein.

**Nachtrag 2: der Spacer.** Vierzehnter Elementtyp, und der erste, der nichts zeichnet und
nichts anfasst. Sein ganzer Zweck ist sein Rechteck: in einer Vertical Box ist das seine Höhe,
in einer Horizontal Box seine Breite, und mit `Slot Fill` über 0 frisst er stattdessen den Rest
und schiebt damit alles Folgende ans andere Ende. Eigene Eigenschaften hat er **keine** — die
Größe auf der Achse und Slot Fill stehen längst in der Basis, und das Details-Panel benennt sie
für Box-Kinder schon achsenrichtig („Height" bzw. „Width"). Gezeichnet wird er nur im Designer
(gestricheltes Rechteck plus Doppelpfeil), weil eine Lücke, die man nicht sieht, eine Lücke ist,
die man nicht anfassen kann.

**Und ein Fehler, der nur die Vorschau betraf:** `propFloatOr` ist typgeprüft und gibt für ein
**Int** den Standardwert zurück. „Align H"/„Align V" sind Int, also hat die Designer-Vorschau
jede Beschriftung links-mittig gezeichnet, egal welche der neun Zellen gewählt war — während die
Engine es richtig machte. Jetzt gibt es `propIntOr` daneben. Die Lehre: **jeder typgeprüfte
Leser braucht seine Variante, sonst ist ein Typfehler kein Fehler, sondern ein Standardwert.**

### Schicht 0 ist fertig (27.08.2026)

Das Vokabular aus D5 steht vollständig, in Metal **und** GL, mit Editor-Zeilen, Designer-Vorschau,
Serialisierung und Tests. Vier Scheiben, jede für sich grün und committet:

**1. Eckenradius pro Ecke.** `cornerRadius` ist ein `glm::vec4` in CSS-Reihenfolge (TL, TR, BR, BL),
auf `UIElement` wie auf `UIRenderObject`. Die SDF wählt den Radius nach dem Quadranten, in dem der
Punkt liegt (`heRoundedBoxSDF`, wortgleich in MSL und GLSL); bei vier gleichen Werten ist es exakt
die alte Formel, es ändert sich also kein Pixel an irgendetwas Bestehendem. **Der Schreiber hat drei
Stufen:** unverändert schreibt nichts, ein Wert für alle vier behält den **alten** Schlüssel
`cornerRadius`, und nur wirklich verschiedene Ecken kosten das Array `cornerRadii`. Skripte behalten
`"Corner Radius"` (schreibt alle vier, liest die obere linke) und bekommen `"Corner TL/TR/BR/BL"`
dazu.

**2. Radialer Verlauf.** `gradientShape` 0/1. Radial läuft von der Mitte bis zur **entferntesten
Ecke** — die CSS-Voreinstellung und die einzige Normierung, unter der die zweite Farbe jeden Teil
der Fläche erreicht. Der Winkel bedeutet dann nichts, also bietet der Editor ihn nicht mehr an.

**3. Schlagschatten.** Ein `blur` an `UIRenderObject`: über 0 wird die Deckung über `blur` Pixel
beiderseits der Kante weich, statt der gewohnten 1-Pixel-Kante. Damit ist der Schatten **ein
gewöhnliches Quad** und kein Blur-Pass — der WidgetManager stellt es **vor** `render()` in die
Liste (Malerreihenfolge, kein Einfügen), versetzt, und um den Blur auf jeder Seite **vergrößert**,
weil der Abfall so weit über die Form hinausreicht. Der Shader misst die Form deshalb gegen eine um
genau so viel eingerückte Box.

**4. Innenschatten.** Kein zweites Quad: er muss von der Form selbst beschnitten werden, also
reitet er wie Rahmen und Verlauf auf dem Flächen-Quad. `d` ist innen negativ, `-d` ist die Tiefe,
derselbe Abfall andersherum gelesen dunkelt den Rand.

**Verifiziert, nicht nur behauptet:** neu ist `HE_DUMP_UITEST=1`, das ein Musterblatt aus zwölf
Kacheln über den ordentlichen Weg (Widget-Asset → WidgetManager → Extractor → UI-Shader) auf den
Bildschirm stellt, sodass `scripts/he_shot.py OUT.png UITEST=1` es headless fotografiert. Auf Metal
angeschaut und richtig: Tab (oben rund, unten eckig), Blatt (diagonal), Rahmen, linearer Verlauf
nach unten und nach rechts, radialer Verlauf, Schlagschatten, Innenschatten, Kapsel. **GL ist
weiterhin nur offline validiert** — die Sandbox hat kein Display.

**Nebenbefund, dieselbe Lehre wie beim Int-Leser:** die Designer-Vorschau hat Rundung, Rahmen und
Verlauf **überhaupt nicht** gezeigt. Sechs Typen hatten je eine feste Rundung von 3 oder 4
einkodiert und die Flächen-Eigenschaften standen nur in `allProperties()`, also nur für Skripte. Es
gibt jetzt ein `drawSurfacePreview`, das die Fläche einmal für alle sechs zeichnet, und einen
Abschnitt „Surface" im Details-Panel, in dem die Eigenschaften überhaupt zum ersten Mal bedienbar
sind. Vier Radien kann `AddRectFilled` nicht, also läuft das über einen eigenen Pfad aus vier
`PathArcTo`.

### Block G ist gebaut (27.08.2026)

**Die Entscheidung, die den Block trägt: der Rasterizer ist von der Backend-Schale getrennt.**
`HE::sw` (`SoftwareRaster.h/.cpp`) ist Quads rein, Pixel raus — kein SDL, kein Fenster, kein
`IRenderer`. Damit ist Schicht 0 **zum ersten Mal in ctest überprüfbar**: der Metal-Zeuge brauchte
eine Maschine mit Display, der CPU-Zeuge ist ein Unittest, der dieselben zwölf Kacheln zeichnet und
hinterher Pixel abfragt. `SoftwareRenderer` ist die dünne Hülle darum (Extractor fragen, blitten).

**Das Fragment-Modell ist wörtlich portiert**, nicht nachempfunden: dieselben Zweige in derselben
Reihenfolge wie `uiFragment` und `kUIFS`, inklusive `roundedBoxSDF` mit Quadrantenwahl, `blur` per
`smoothstep`, Innenschatten, Rahmenring, Source-over. Die SDF hat einen eigenen Test gegen genau
die Zahlen aus der Offline-Simulation — das ist das Band, das die **drei** Kopien zusammenhält.

**Dirty Rectangles.** `HE::sw::dirtyRects` vergleicht die Quad-Liste **positionsweise** mit der des
Vorframes (UI-Quads kommen in Malerreihenfolge, „Quad 7 ist anders als letztes Quad 7" ist also
genau die Frage) und liefert die Vereinigung aus **alter und neuer** Hülle je Unterschied — nur das
neue Rechteck zu übermalen ließe das alte stehen. `quadBounds` ist bewusst weiter als das Rechteck
des Quads: gedrehte Ecken schwingen aus, ein Schatten reicht über die Form hinaus, ein Scissor
schneidet zu. Überlappende Rechtecke werden verschmolzen, weil zweimal „over" nicht dasselbe Bild
ist wie einmal. Über der halben Fensterfläche oder über 32 Rechtecken gibt die Funktion auf und
sagt „male alles" — das ist billiger als die Buchführung. Der wichtigste Test ist der, der prüft,
dass ein Teil-Neuzeichnen **pixelgleich** zum Vollbild ist.

Drei Gründe erzwingen trotzdem ein Vollbild, und alle drei heißen „wir können dem nicht trauen, was
schon da steht": Größenwechsel, erster Frame, und **eine andere Surface von SDL** — dann hält deren
unberührter Teil ein anderes Bild und ein Teilupdate zeigte zwei Frames auf einmal.

**Ändert sich gar nichts, wird keine einzige Zeile geschrieben** und die Surface nicht angefasst.
Das ist der Fall, den A2 erreichen soll.

**Zwei Grenzen, ausgesprochen statt versteckt:** ein Quad mit **Material** zeichnet Schicht 0 plus
Farbton (der D5-Rückfallvertrag), und **blockkomprimierte Texturen** werden nicht dekodiert, sondern
einmal gemeldet — das Export-Profil backt UI-Texturen als RGBA8.

**Verdrahtet:** der Export-Dialog schreibt für ein App-Projekt mit Advanced **aus** jetzt
`GameBackend = "Software"` in die ausgelieferte `config.json`. Erst das macht den Haken beim
Anlegen zu dem, was A0 verspricht.

**Verifiziert:** `scripts/he_shot.py OUT.png UITEST=1 RHI=Software` fährt den echten
`SoftwareRenderer` im echten Editor-Prozess und liefert dasselbe Musterblatt wie Metal. Zwei
Beobachtungen dabei: `SDL_GetWindowSurface` gibt auf macOS die **logische** Größe (das Fenster wird
ohne `HIGH_PIXEL_DENSITY` erzeugt, absichtlich — viermal so viele Pixel für einen Blit, der wieder
heruntergerechnet wird, wäre reine Arbeit), und die Aufnahme hat deshalb Fenstergröße statt der
1280×720, die der GPU-Pfad in ein Offscreen-Ziel rendert.

**Offen aus diesem Block:** die Bibliothek wird heute neben den anderen gelinkt und per
Konfiguration gewählt. Das stand hier als A3a; die Nachmessung oben hat es **A3b** zugeschlagen,
und da gehört es hin: „was eine App nicht anzielt, wird nicht gelinkt" wird erst wahr, wenn der
Advanced-Schalter entscheidet, welcher einzige Renderer gelinkt wird. **A3a selbst ist damit
abgeschlossen** — Python ist ein Laufzeit-Plugin, `libcrypto` ist über mbedTLS weg, und die
beiden verbliebenen Punkte (Backends, `libHorizonNet`) sind beide A3b, also Welle 3.

### Welle 1 ist abgenommen (27.08.2026)

**Erst fehlte eine Fähigkeit.** Beim Prüfen des Vokabulars gegen eine Todo-App fiel auf: das
Widget-System konnte zur **Laufzeit keine Elemente erzeugen**. `CreateWidget` macht eine
eigenständige Instanz, aber es gab keinen Weg, einer Vertical Box ein Kind hinzuzufügen — eine
Liste unbekannter Länge war damit nicht ausdrückbar, und das ist die gewöhnlichste Sache, die
eine Anwendung zeigt. Der User hat entschieden, das zuerst zu bauen.

`widget.addChild(widget, parentName, widgetAsset) → child` greift dafür auf die **vorhandene**
Graft-Maschinerie zurück, die ein WidgetRef benutzt: eine Zeile ist ein eigenes Widget-Asset,
einmal gezeichnet, mit **eigener Logik als eigene Skript-Instanz**. Der Rückgabewert ist eine
Ref, also ein Objekt wie jedes andere — Set External schreibt seine öffentlichen Variablen, Call
External ruft seine Funktionen, Bind Event hört auf das, was es sendet. Dazu `removeChild` (eine
Zeile wieder heraus, mit ihren Elementen und ihrer Logik) und `clearChildren`. Adressiert wird
über den **Namen** des Elternteils, weil das der Designer anzeigt und der Autor tippen kann.

**Dann die App.** `tests/test_app_todo.cpp` baut sie so, wie ein Autor sie bauen würde — ein
Asset für die Seite, eines für die Zeile, HorizonCode-Graphen für beide — tippt echten Text,
klickt echte Knöpfe und fragt, was auf dem Bildschirm steht: zwei Zeilen mit **je ihrem eigenen**
Text, im Kasten gestapelt, und eine Zeile, die sich durch ihren eigenen Löschknopf selbst aus der
Liste nimmt. Der erste Lauf fand gleich einen Fehler in der App: ohne Leeren des Eingabefelds
liest das nächste Hinzufügen, was noch dasteht.

**Und exportiert.** Der zweite Testfall exportiert dieselbe Sache über `ProjectExporter` und
prüft die ausgelieferte `config.json` auf `GameBackend = Software` und den Baum auf die Abwesenheit
von Python. Das gestartete Ergebnis:

| | |
|---|---|
| Start | `backend=Software`, „application mode — no world, no physics, no scene" |
| Größe | **26,6 MB** (ohne Python), unter der 32-MB-Schwelle |
| **CPU im Leerlauf** | **0,1–0,2 %** — die Abnahmemarke ist 2 % |
| Speicher | 104 MB RSS |

**Zwei Funde, beide echt.** Der erste: `appProject`/`advancedShaderEffects` reisen in
`project.hcfg`, **nicht** in `config.json` — ohne sie startete die exportierte App Jolt und stellte
eine Fly-Kamera in eine Welt, die sie nicht hat. Der zweite: die exportierte App warnte alle zwei
Sekunden „Fixed update is falling behind", mit einer Zahl, die **wuchs** (1332 ms nach 15 s). Eine
ereignisgetriebene Anwendung reißt die Fixed-Step-Deckelung **bauartbedingt**: ein Frame im
Leerlauf ist ein Zehntel Sekunde lang, also sechs Schritte bei einer Deckelung von fünf, jeden
Frame, für immer. Zwei Zeilen in `GameLoop::tick`: der Rückstand wird **verworfen** statt
mitgeschleppt (die Todesspirale, und die richtige Antwort auch für Spiele), und ohne
Game-Logic-Modul läuft die Fixed-Schleife gar nicht erst — es gibt nichts, dem man hinterherhinken
könnte. Danach: null Warnungen.

**Was die Abnahme nicht abdeckt, ausdrücklich:** die App wurde als flaches Verzeichnis exportiert,
nicht als signiertes `.app`; das Klicken in der laufenden exportierten App ist ungetestet (der
Testfall oben treibt den WidgetManager im Prozess); Persistenz über einen Neustart ist nicht
gefahren; und dies ist die Ausprägung **Advanced aus**. Die Ausprägung Advanced **an** — GPU-Pfad,
Material-Graphen — hat weiterhin keinen Abnahmelauf, und das bleibt der offene Posten aus der
Risikoliste („der headless-Starttest muss beide App-Ausprägungen fahren").

### D1 Theme-System, erste Fassung (27.08.2026)

**Zwei Entscheidungen tragen den ganzen Block.**

*Wie eine Rolle am Element hängt:* eine generische Abbildung `Eigenschaftsname → Rollenname` auf
der **Basis**, kein Begleitfeld pro Farbe. Die Flächenfarbe heißt bei jedem Typ anders („Color",
„Tint", „Normal Color", „Back Color"); ein Feld pro Farbe hieße, alle sechs Typen anzufassen und
den siebten trotzdem zu verpassen. Serialisiert wird sie als **ein** optionales JSON-Objekt —
ist nichts gebunden, steht nichts in der Datei, und ein Widget von vorgestern speichert
byte-identisch.

*Wo aufgelöst wird:* **beim Zuweisen, nicht pro Frame.** `uiApplyTheme` schreibt die Farbe der
Rolle in die gewöhnliche Eigenschaft — beim Erzeugen eines Widgets, beim Theme-Wechsel, beim
Moduswechsel und für jede zur Laufzeit eingehängte Zeile. Danach sehen Laufzeit,
**Designer-Vorschau**, Thumbnails und **Software-Renderer** die Theme-Farbe durch dieselben
Feldzugriffe, die sie ohnehin machen, und keiner von ihnen muss wissen, dass es Themes gibt. Im
Extractor aufzulösen hätte die Laufzeit eingefärbt und den Designer etwas anderes zeigen lassen —
genau die Spaltung, die dieses Panel schon zweimal gebissen hat (fest einkodierte Rundungen, der
Int-durch-Float-Leser). Ein Preis, der dokumentiert gehört: ein Skript, das ein Literal in eine
gebundene Eigenschaft schreibt, wird beim nächsten Wechsel überschrieben.

**Was steht:** neun Farbrollen × hell/dunkel, drei Größenstufen für Radius und Abstand, fünf
Textgrößen, zwei Schattenstufen; JSON-Round-Trip, bei dem eine **fehlende Rolle** auf den
Standard zurückfällt statt auf Schwarz; zwei mitgelieferte Themes (Default und Amber);
`AssetType::Theme` mit Chunk, Editor-Anlegen (auch im App-Modus, der Themes am nötigsten hat) und
einem Theme-Editor-Tab, der Hell und Dunkel **nebeneinander** zeigt — ein Editor, der einen Modus
zur Zeit zeigt, ist einer, in dem der andere vergessen wird. Am Widget hat jede Farbzeile einen
Knopf „Literal / Rolle"; ist sie gebunden, wird das Farbfeld schreibgeschützt, weil ein Wert, der
beim nächsten Moduswechsel überschrieben wird, schlimmer ist als einer, den man nicht tippen
kann. Skripte bekommen `theme.set`, `theme.setMode`, `theme.getMode`.

**Optisch belegt:** derselbe Baum, zweimal gerendert, hell gegen dunkel (im CPU-Rasterizer, als
ctest) — ein Theme, das nur über Feld-Asserts geprüft ist, könnte in ein Widget auflösen, das
niemand zeichnet.

**Ein Fund am Werkzeug:** `scripts/editor_help_audit.py` hat eine **feste Dateiliste**. Ein neues
Panel ist für den Audit unsichtbar, bis es dort steht — die Deckungsgarantie hätte still ein Loch
bekommen. `ThemeAssetPanel.cpp` ist eingetragen.

**Zweiter Durchgang: „folge dem System" und die Projektwahl.**

`UIThemePreference` ist ein eigener Aufzählungstyp neben `UIThemeMode`, und das ist der Kern:
gespeichert wird die **Frage**, nicht die Antwort. „System" ist eine Regel, keine Farbe — würde
man den aufgelösten Wert ablegen, ginge ein Projekt, das auf einem dunklen Rechner gebaut wurde,
für alle als dunkel raus. Der WidgetManager kennt deshalb beides: die Präferenz und die letzte
Meldung des Schreibtischs (`setSystemThemeMode`), und `themeMode()` ist die Ableitung. SDL wird
nicht dort gefragt — die Klasse linkt kein SDL und soll es nicht lernen —, sondern im Host:
`SDL_GetSystemTheme()` beim Start und `SDL_EVENT_SYSTEM_THEME_CHANGED` im laufenden Betrieb, beides
in SDL 3.2.14 vorhanden (nachgesehen, nicht angenommen). Ändert eine Meldung nichts am Ergebnis,
weil die Präferenz sie überstimmt, wird auch **nicht neu gezeichnet** — eine ereignisgetriebene
App darf dafür nicht aufwachen.

Die Projektwahl steht im Theme-Editor selbst („Use for this Project" plus „Starts in"), aus
demselben Grund, aus dem „als Projekt-Standard setzen" bei der Savegame-Vorlage steht: die
Antwort gehört zu dem, was man gerade ansieht, und eine Einstellung drei Menüs weiter findet
niemand. Sie reist über `.heproj` → `ExportSettings` → `project.hcfg` v4, und **jedes Anhängsel
wird nur geschrieben, wenn es etwas trägt** — ein Projekt ohne Theme gibt weiterhin die v2 aus,
die jeder ältere Runtime lesen kann. Der Game-Runtime lädt beides **vor** OnInit, weil dort das
erste Widget entsteht und ein eine Frame später umgefärbtes Widget ein Aufblitzen der falschen
Farben ist.

**Dritter Durchgang: D1 ist fertig.**

**Größen und Textstufen binden wie Farben.** Die Textgrößen im Theme las bis dahin niemand — tote
Daten. Jetzt trägt **dieselbe** Abbildung alle drei Arten, und **die Eigenschaft entscheidet, was
eine Bindung bedeutet, nie der Name**: eine Color-Eigenschaft liest eine Farbrolle, `FontSize`
liest die Typografie-Stufen, jede andere Float-Eigenschaft — Eckenradius, `Padding`, `Spacing` —
liest die Größenstufen. Das ist keine Feinheit: die beiden Zahlen-Vokabulare enthalten **beide**
ein „Small", und nur das, worauf die Bindung sitzt, sagt welches gemeint ist. Ein Name aus dem
falschen Vokabular löst in keinem auf und lässt den Wert stehen — dieselbe „sichtbar und
reparierbar"-Regel wie bei einer umbenannten Farbrolle. Ein Test fährt genau diese Kollision.

**Die mitgelieferten Paletten bleiben C++**, und das ist eine Entscheidung, keine Auslassung:
`EditorContent/EngineContent` holt seinen echten Inhalt über SFTP, im Repository liegt nur die
Ordnerstruktur (`Materials/` enthält ein `.gitkeep` und sonst nichts). Ein eingechecktes
Theme-Asset wäre dort das einzige, das niemand neu erzeugen könnte. Stattdessen bietet der
Theme-Editor „Start from" an: Default oder Amber übernehmen, den Namen des Assets behalten. Das
ist ohnehin besser als eine Datei — man bekommt eine Palette **und** ein Asset, das man ändern
darf, in einem Klick.

**Was ausdrücklich nicht in D1 kommt:** eine zweite Schriftart. „Mono" ist hier eine Größenstufe
und keine Schriftfamilie; eine mitgelieferte Monospace-Schrift ist eigene Arbeit und gehört zu D2.

**Als Nächstes:** der Rest von D1, dann D2 (Komponentenbibliothek) oder A3b.

### Der Starttest, und was er sofort gefunden hat (27.08.2026)

Die Risikoliste fordert seit Tag eins einen Test, der eine App-Konfiguration hochfährt, ab Welle 2
**beide** Ausprägungen. Es gab ihn nicht, und die Lücke war nicht theoretisch: eine Theme-Änderung
setzte eine Null-Dereferenzierung in `OnInit` (`m_world->widgets()`, bevor es die Welt gibt), alle
Unittests blieben grün, der Build war sauber, und **beide** exportierten Anwendungen stürzten vor
dem ersten Frame ab. Nichts in dieser Suite fasst das **Starten** an, weil alles davon passiert,
bevor es etwas zum Prüfen gibt.

Zwei Schalter machen es prüfbar: `HE_EXIT_AFTER_FRAMES` lässt die Anwendung n Frames zeichnen und
sauber gehen — „startet sie" wird zu einem Exit-Code —, und `HE_CAPTURE_FRAME`/`HE_CAPTURE_PATH`
schreibt einen Frame als PPM heraus. Der Test exportiert beide Ausprägungen durch **dieselbe**
Funktion (zwei Kopien wären genau, wie eine verrottet), startet jede mit Deadline und prüft
Exit-Code **und** Pixel.

**Der Fund, den er sofort erbrachte.** Der Zweig, der im App-Modus Bloom, SSAO, GI, SSR, AA, den
Forward-Pfad und den Himmel abschaltet, stand **innerhalb** von `if (m_world && !m_appMode)` — eine
Bedingung, verschachtelt in ihrer eigenen Verneinung. Kein Compiler warnt, kein Test schlug fehl,
und die Wirkung war: eine Anwendung sagte dem Renderer **nie etwas**, behielt dessen Vorgaben und
zeichnete Himmel, Wolken und Boden hinter ihrer Oberfläche — samt der vollen Nachbearbeitungskette
über eine leere Welt, also genau dem, was vor Tagen als „exportierte App lagt" gemeldet und
angeblich behoben wurde. Behoben war nur der Text.

**Und eine Lehre über das Prüfen selbst.** Die erste Aufnahme zeigte vor und nach dem Fix
dasselbe Bild — weil die Testseite jeden Pixel bedeckte und damit über das, was dahinter liegt,
nichts sagen konnte. Der Starttest exportiert deshalb eine Variante, deren Hintergrund einen Rand
frei lässt: **„ist da etwas hinter der Oberfläche" ist von einem Screenshot, der alles bedeckt,
nicht zu beantworten.** Gegengeprüft, beide Male: Fix raus → Eckpixel 114,127,152, Test rot; Fix
rein → schwarz, Test grün.

**Und Schicht 1, zum ersten Mal end-to-end.** Die Advanced-Seite trägt jetzt ein Image mit einem
UI-**Material**: ein Graph, der flach grün malt, auf einem **roten** Farbton. Das ist der
Unterscheider und nicht Dekoration — ein Material, das nicht übersetzt, nicht gefunden oder nicht
gebunden wird, fällt laut D5-Vertrag auf „Schicht 0 plus Farbton" zurück, und der Fleck käme
**rot** heraus. Gemessen: innen exakt `(0,255,0)`, außen der Theme-Hintergrund. Gegengeprüft mit
einem absichtlich falschen Materialpfad: `swatch pixel 255,0,0`, Test rot mit „showing its RED
tint, which is exactly the material fallback".

Damit ist der Weg Graph → Codegen → Cross-Compile → Pipeline → Pixel in einer **ausgelieferten**
Anwendung belegt. Vorher prüfte das niemand: ein Build hätte jedes Material als seinen Farbton
zeichnen können und die ganze Suite wäre grün geblieben.

**Die Risikoliste ist damit abgearbeitet** — beide App-Ausprägungen starten, zeichnen und werden
gemessen, jede mit dem, was nur sie kann.

**Frühere Notiz (erledigt):** Schatten, dann Eckenradius pro Ecke (das erste Attribut, das
sich nicht sauber in vorhandene `UIPropType`-Kinder zerlegen lässt — vier Zahlen sind vier
Zeilen, ein „vier Radien"-Feld wäre Handarbeit). Danach Block G (`RendererSoftware`), der
Schicht 0 ebenfalls tragen muss.

---

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
- E2 Live-Vorschau Stufe 1 (kein Viewport, ungegateter Widget-Tick, Startinhalt)
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
- E2/E3/E4 Live-Vorschau, Stufe 2 (Hot-Reload) und Stufe 3 (Zustand über den Reload)

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

### B2 ListView (31.08.2026)

Fünfzehnter Elementtyp, und der erste, dessen ganze Behauptung eine **Verneinung** ist: zehntausend
Zeilen sind nicht zehntausend Elemente. Bisher war eine Liste entweder N im Designer vorgebaute
Zeilen mit einer harten Decke, oder ab `addChild` N echte Elemente — und bei zehntausend steht das
Programm.

**Sie funktioniert, weil sie keine Daten hält.** Sie hält eine Zeilenvorlage (ein ganz normales
UI-Widget-Asset, im selben Designer gebaut), eine **Anzahl**, die der Besitzer setzt, und eine
Zeilenhöhe. Daraus rechnet sie aus, welche Einträge das Sichtfenster überhaupt zeigen kann,
realisiert nur die, und lässt sich jede einzelne vom Besitzer füllen (`OnRowBind` mit dem
**Eintrags**-Index). Scrollen richtet dieselbe Handvoll Zeilen auf andere Einträge, statt neue zu
bauen: die Kosten sind die Größe des Fensters, nicht die der Liste.

**Was daraus folgt, und wofür ich mich entschieden habe, es so zu lassen:** eine Liste, die keine
Einträge hat, kann sie auch nicht **sortieren**. Der Plan nennt Sortierung und Spaltenbreiten in
einem Atemzug mit der ListView; ich habe stattdessen `refreshList` gebaut. Sortieren ist eine
Operation auf dem Array, das der Besitzer ohnehin schon hat (HorizonCode kann das), und danach
fragt ein Aufruf alle sichtbaren Zeilen neu. Die Alternative wäre, die Daten ein zweites Mal in der
Liste zu halten, und zwei Quellen für dieselbe Wahrheit sind teurer als der eine Aufruf.
**Spaltenbreiten** gehören zur Tabelle mit Kopfzeile, und die ist ein eigener Typ (B2b, offen) —
in der ListView sind „Spalten" das, was die Zeilenvorlage nebeneinander legt.

**Vier Dinge, die dabei nicht so laufen konnten wie beim Rest:**

**1. Die Platzierung ist nicht die der Layout-Boxen.** `boxSlotRect` findet den Platz eines Kindes,
indem es alles davor aufaddiert — genau das, was eine virtualisierte Liste nicht kann, weil die
Zeilen davor nicht existieren. Eine Zeile steht deshalb bei `Index × Schritt − Offset`, in
`listSlotRect`. Der Kommentar in `boxSlotRect` behauptete, ein später hinzukommender Container werde
„von diesem Code ohne Änderung gelegt": für das Lesen von Padding/Spacing per Namen stimmt das, für
den Achsenlauf nicht, und für ein Grid wird es auch nicht stimmen.

**2. Scrollen ist jetzt eine Frage, die jedes Element beantwortet.** `uiScrollBy`, der Clamp und das
Mausrad nannten alle drei den `ScrollBox`-Typ. Statt jeder Stelle eine zweite Zeile zu geben, gibt
es `UIElement::scrollOffsetPtr()` und `maxScrollAmount()`: nicht null heißt „ich scrolle". Der
dritte scrollende Container kostet danach keine dieser Stellen mehr.

**3. Zwei Zahlen pro Zeile, nicht eine.** `rowIndex` sagt, **wo** die Zeile steht, `rowBound`, worauf
sie zuletzt **gefüllt** wurde. Genau diese Trennung ist das Recycling: beim Scrollen um eine Zeile
ändert sich ein `rowIndex`, neun bleiben, und nur wo die beiden auseinanderlaufen wird `OnRowBind`
gefeuert. Ein einziger Wert hätte entweder jeden Frame neu gefüllt (und damit jeden Frame neu
gezeichnet) oder gar nicht mehr.

**4. Mehrfachauswahl schaltet beim einfachen Klick um.** `processPointer` bekommt die Modifiertasten
nicht, und der Modus wurde vom Autor gerade deshalb gewählt, weil mehr als eine Zeile gemeint ist.
Eine Liste, in der die zweite Zeile nur mit Strg erreichbar wäre, ist schlechter als eine ohne.

**5. `OnRowBind` läuft fremden Code mitten im Lauf.** Das Naheliegendste, was ein Graph darin tut,
ist die Liste ändern: ein Filter macht sie kürzer, „die letzte Zeile ist dran, also lade fünfzig
mehr" macht sie länger. Beides landet wieder in `setListCount`, das synchronisiert — **während** die
Synchronisation, die den Bind ausgelöst hat, noch über ihre Zeilen läuft. Zwei getrennte Vorkehrungen,
weil es zwei getrennte Fehler sind:

- Die Bind-Schleife hält **keine Zeiger** mehr über den Aufruf hinweg, sondern Element-IDs, und sucht
  vor jedem Feuern neu. Ein Handler darf die Zeile entfernen, die er gerade bekommen hat — das
  löscht Elemente, und ein vorher genommener Zeiger wäre für den Rest der Schleife tot.
- Ein **Riegel** (`m_syncingLists`) schließt die Tür für die Dauer des Laufs. Ohne ihn ist
  `OnRowBind → refreshList` eine Endlosschleife: jeder Refresh bindet, jeder Bind refresht. Gemessen
  2211 Aufrufe statt elf, bevor die Notbremse im Test greift.

Der Preis ist ein Frame Verzögerung, und der ist bei ereignisgetriebenem Zeichnen genau der Preis,
den man dafür bezahlen will.

**Die Tests sind entsprechend Zählungen von Dingen, die nicht passieren dürfen**, und alle drei
tragenden habe ich gegengeprüft, indem ich den Mechanismus absichtlich kaputtgemacht habe: ohne den
`rowBound`-Vergleich wird „eine Liste, die sich nicht bewegt, zeichnet nicht neu" rot, wenn die
Zeilen jedes Mal neu gegraftet werden, wird „Scrollen richtet die vorhandenen Zeilen neu aus" rot,
und ohne den Riegel wird „ein Bind, der die Liste ändert, frisst nicht die Zeilen, über die er
läuft" rot. Der dritte hat mich zusätzlich etwas gelehrt: mein **erster** Versuch dieses Tests
(`OnRowBind → setListCount`) blieb auch ohne Riegel grün, weil der Schnappschuss ihn schon trägt.
Ein Gegenbeweis, der nicht rot wird, prüft nicht das, was man glaubt.

**Der Eckfall daneben:** „Item Count" ist eine Eigenschaft **und** ein API-Aufruf, also gab es zwei
Schreibwege mit unterschiedlich viel Aufräumen dahinter. Beide landen jetzt auf
`UIListView::setItemCount` — sonst hinterlässt ein `Set Property`-Knoten eine Auswahl, die hinter das
Ende zeigt, und Enter feuert `OnRowActivated` für einen Eintrag, den es nicht gibt.

**Und eines, das der Bauart innewohnt:** die Auswahl ist **indexbasiert**. Nach einem Sortieren zeigt
sie auf das, was jetzt an diesen Positionen steht, nicht auf dieselben Einträge. Das steht in der
Node-Dokumentation von `Refresh List`, weil es der Preis dafür ist, dass die Liste die Daten nicht
kennt.

**Zwei Funde nebenbei, beide von diesem Durchgang aufgedeckt und beide behoben:**

- **`runtime_size` urteilte über Debug-Bäume.** Die Schwellen (90/32 MB) sind an einem
  **Release**-Baum gemessen; ein Debug-Deploy wiegt 132 MB und ging folgerichtig rot, obwohl an der
  ausgelieferten Größe nichts dran ist. Der Build-Typ wandert jetzt aus CMake in das Skript, und
  alles außer Release berichtet nur. Der Docstring sagte die Lehre schon („der Messpunkt ist Teil
  der Behauptung"), die Prüfung kannte ihren eigenen Messpunkt nur nicht.
- **Der Starttest hatte eine Release-Frist.** 40 Sekunden für 30 Frames; ein Debug-Build braucht
  ~176 s, weil das Backend beim ersten Lauf jede Pipeline übersetzt (auf macOS 27 ohne
  `MTLBinaryArchive`, siehe Memory). Er meldete „hat nie 30 Frames geschafft" — wahr, und über die
  falsche Sache. Die Frist hängt jetzt an `NDEBUG`.

**Offen an dieser Ecke:** die Tabelle mit Kopfzeile und ziehbaren Spalten (B2b), variable
Zeilenhöhen (v1 ist bewusst fest — dort explodiert die Komplexität), und der Doppelklick auf eine
Zeile ist nur im Spiel-Runtime verdrahtet, in der Live-Vorschau des Editors noch nicht.

---

### B4 Dialoge, Popups, Menüs (31.08.2026)

Vier Dinge im Plan, **ein** Begriff im Code: der **Grab-Stapel**. Ein modaler Dialog, ein
Kontextmenü und eine offene Auswahlliste unterscheiden sich nur darin, ob der Hintergrund
abgedunkelt wird und was sie wieder schließt. Was sie gemeinsam haben, ist der ganze Punkt:
*solange einer oben liegt, gehört die Eingabe ihm und nichts darunter hört mit.*

Sie stapeln sich, weil eine Rückfrage über einem Einstellungsdialog der Normalfall ist und ein
einzelner Platz still einen davon verschluckt hätte.

**Die eigentliche Arbeit war nicht der Stapel, sondern die Liste der Eingänge.** Es gibt neun
Wege in den WidgetManager hinein, und der eine, den man vergisst, ist das Loch im Dialog. Sie
fragen jetzt alle dieselbe Funktion: `takesInput(widgetId)`. Dieselbe Lehre wie bei
`scrollOffsetPtr()` in B2, an einer Stelle, wo sie mehr kostet.

**Fünf Entscheidungen, die vorher fallen mussten statt hinterher aufzufallen:**

**1. `showModal` hebt den Z-Order.** Ein Dialog, der die Eingabe blockiert und dabei *hinter*
etwas zeichnet, ist das Schlechteste aus beidem, und genau das passiert, wenn jemand einmal
einen Z-Order gesetzt und ihn vergessen hat.

**2. `pointerOverUI` muss bei einem Modal für den ganzen Bildschirm wahr sein.** Die Abdunklung
wird vom Manager gezeichnet und ist kein Element, also findet der Trefferlauf dort nichts —
und ohne diese eine Zeile feuert ein Klick neben den Pausendialog ins Spiel dahinter. Man
sieht das erst, wenn jemand einen Pausendialog ausliefert.

**3. Der Fokus wird gemerkt und zurückgegeben.** Sonst kommt die Seite dahinter ohne Auswahl
wieder und der nächste Pfeiltastendruck fängt von vorn an.

**4. Ein Popup wird beim Öffnen normalisiert, nicht per Konvention platziert.** „Verankere
deine Wurzel oben links" ist eine Konvention, die der erste Nutzer bricht. `openPopupAt`
setzt die Anker der Wurzelelemente selbst, rechnet den Bildschirmpunkt in Canvas-Einheiten um
und klemmt gegen die Größe — ein Menü unten rechts klappt nach oben auf, statt aus dem Bild zu
laufen. Der Preis ist, dass ein Popup-Asset seine Anker verliert; das ist bei etwas, dessen
Position von außen kommt, ohnehin die richtige Antwort.

**5. Die ComboBox öffnet jetzt eine Liste, statt durchzuschalten.** Das war mit drei Einträgen
benutzbar und mit zwanzig nicht mehr (fünfzehn Klicks bis zum fünfzehnten Eintrag, kein Weg
zurück), und es ist nirgends sonst, was ein Dropdown bedeutet. Die offene Liste zeichnet der
**Manager**, nicht `render()`: sie hängt außerhalb des Element-Rechtecks, und jedes Rechteck in
diesem System — Treffer, Clip, Zeichenreihenfolge — ist das Element selbst. Was aus dem eigenen
Rahmen ragt, muss der Schicht gehören, der der Bildschirm gehört.

**Der Tooltip ist eine Zeichenkette auf der Basis, und die Verzögerung ist das Feature.** Ein
Hinweis, der beim Überfahren sofort erscheint, steht im Weg. Der Zähler läuft in `tick()` — und
**dort muss er selbst das Dirty-Flag setzen**: eine ereignisgetriebene Anwendung zeichnet bei
*Änderung*, die Änderung ist hier vergehende Zeit, und ohne diese Zeile erschiene der Tooltip
erst bei der nächsten Mausbewegung, also genau dann, wenn er nicht mehr gewollt ist. Das ist
die „vergessenes Dirty-Flag"-Falle aus der Risikoliste, im Konkreten.

**Zwei neue Ereignisse:** `OnRightClicked` (am Element, blubbert nach oben zum ersten, der
zuhört) und `OnDismissed` (am Widget selbst). Ein Kontextmenü ist damit zwei Knoten:
On Right Clicked → Open Popup At Pointer.

**Escape ist in BEIDEN Anwendungen verdrahtet**, plus Gamepad-East, und zwar *vor* der
bisherigen Bedeutung — ein Pausendialog, den Escape nicht schließen kann, weil Escape schon
vergeben ist, ist ein Dialog, aus dem niemand herauskommt. Bei der Gelegenheit ist auch der
Doppelklick aus B2 in der Live-Vorschau des Editors nachgezogen: die Vorschau einer Anwendung,
die man nicht vollständig bedienen kann, ist keine.

**Was der Gegenbeweis diesmal gelehrt hat, ist dasselbe wie bei B2 und es hat wieder zugebissen.**
Alle sechs Testfälle waren beim ersten Lauf grün. Der Modal-Test bestand allerdings **aus dem
falschen Grund**: mein Dialog deckte den ganzen Bildschirm, also nahm er jeden Klick schon
deshalb, weil er obenauf lag — die Eingabesperre wurde nie gefragt. Erst mit einem *kleinen*
Dialog in der Ecke, einem Klick daneben und einer Scroll-Box unter dem Rad wird der Test rot,
wenn man `takesInput` entfernt. Ebenso der Tooltip: ohne das Dirty-Flag in `tick` fällt genau
die eine Zeile um, die es prüft.

**Nicht gebaut, mit Begründung:** ein eingebautes **Menü-Widget**. „Dropdown-Menü" aus dem Plan
ist jetzt *baubar* — ein Popup mit einer ListView oder ein paar Knöpfen darin — statt ein
eigener Typ zu sein; das eingebaute, das repariert wurde, ist die ComboBox. Ein Menü mit
Untermenüs, Trennern und Tastenkürzeln gehört zu D2, wo es als Komponente aus vorhandenen
Teilen entsteht.

**Nachtrag: zwei Löcher, die beim Gegenlesen auffielen, beide vom ersten Dialog erreichbar.**

**1. Verstecken war ein Deadlock.** Der OK-Knopf, den jeder schreibt, ist
`OnClicked → Hide Widget (Get Self)`. `hideWidget` setzte nur `visible = false` — der Grab
überlebte das Bild: `takesInput` nannte weiter ein Widget, das der Trefferlauf als unsichtbar
überspringt, also lehnte **jeder** Weg hinein alles ab, ohne dass auf dem Bildschirm etwas zu
sehen war, dem man die Schuld geben könnte. Nur Escape hätte noch herausgeführt. Verstecken und
Schließen sind dasselbe Ereignis von zwei Seiten und dürfen sich nicht darin unterscheiden, was
sie hinterlassen: `hideWidget` und `HideSelf` geben jetzt die Ebene frei, inklusive
`OnDismissed`.

**2. Das Escape im Editor war genau dann tot, wenn ein Dialog offen ist.** Ich hatte es in den
Block gehängt, der mit `!hasFocusedTextField()` beginnt — und `hasFocusedTextField()` heißt
„irgendetwas hat die Tastatur", was `showModal` per Konstruktion herstellt. Der Fall, für den
die Verdrahtung gedacht war, war der einzige, in dem sie nicht lief. Jetzt steht Back **vor**
diesem Tor, in beiden Anwendungen — und funktioniert damit auch, während man in ein Feld des
Dialogs tippt, was Escape überall bedeutet.

**Dazu drei kleinere, alle aus derselben Durchsicht:** ein Popup misst seine Wurzel jetzt,
**bevor** es sie umankert (auf einer gedehnten Achse ist `sizeX` nicht die Größe, sondern die
Differenz zur Spanne — bei einer füllenden Wurzel negativ, und ein Popup mit Breite −100 ist
kein Popup); die offene Liste schließt auch auf Rechtsklick; und ihr früher Ausstieg quittiert
jetzt **beide** Tastenflanken, sonst zählt eine über das Schließen gehaltene rechte Taste als
frischer Druck und öffnet ein Kontextmenü, das niemand wollte.

**Offen an dieser Ecke:** die Tastaturbedienung der offenen Liste (Pfeile durch die Optionen),
Untermenüs, und der Rechtsklick ist im Editor-Viewport nur in der Live-Vorschau nutzbar (im
Spielmodus gehört er der Kamera).

### Ein Byte, eine halbe Höhe (31.08.2026)

Meldung aus einem echten Projekt: zwei Knöpfe in einer Vertical Box, jeder mit einem Text und
einem Bild als Kind, im Designer beide richtig — im Viewport klebt der Text des **zweiten**
oben im Knopf, obwohl der Anker mitte-links steht.

Ich habe die gespeicherte `AppTest/RootWidget.hasset` ausgelesen und die beiden Label
verglichen. Sie sind **identisch**: gleicher Anker `[0, 0.5]–[1, 0.5]`, gleiches `pos [5,0]`,
gleiches `size [-10,48]`, gleiches `alignV: 1`. Der einzige Unterschied ist der Text selbst:
`"Option 1"` gegen `"Option 2\n"`.

Eine leere letzte Zeile ist die **halbe Höhe** eines zweizeiligen Blocks. Zentriert man den,
landet die sichtbare Zeile in der oberen Hälfte — gemessen: Oberkante bei 2,5 statt bei 14 in
einem 48 Einheiten hohen Knopf. Genau das, was gemeldet wurde, und die Engine hat dabei nichts
falsch gemacht: sie hat die Daten gezeichnet, die sie bekam.

**Wie man an diese Daten kommt:** die Text-Eigenschaft ist mehrzeilig (für absichtliche
Zweizeiler), und Enter heißt dort „neue Zeile", während man es als „fertig" tippt.

**Warum der Designer trotzdem richtig aussah, und warum das der eigentliche Fehler ist:** ImGuis
`CalcTextSizeA` zählt einen abschließenden Umbruch **nicht** als Zeile, `layoutUITextLines` schon.
Zwei Textmaße, die sich um eine Zeile unterscheiden — und ein Designer, der etwas anderes zeigt
als die Engine zeichnet, ist schlimmer, als wenn einer von beiden falsch wäre. Jetzt lassen beide
genau **einen** abschließenden Umbruch fallen: `"a\n\n"` behält seine gewollte Leerzeile, ein
leerer Text bleibt eine (leere) Zeile, und ein versehentliches Enter kostet nichts mehr.

Die Lehre ist dieselbe wie beim Align-Vorfall davor: **wenn Designer und Viewport sich
widersprechen, ist die Frage nicht, wer recht hat, sondern warum es zwei Antworten gibt.**

**Und dieselbe Frage nochmal, am Dropdown-Pfeil.** Die Engine zeichnete dort den **Buchstaben
„v"** aus der UI-Schrift — was man ihm ansah —, der Designer ein richtiges Dreieck, an einer
leicht anderen Stelle, in einer fest eingetippten grauen Farbe. Jetzt kommt die Geometrie aus
**einer** Funktion (`UIComboBox::arrowIn`), beide zeichnen daraus, und die Farbe ist die
Textfarbe des Elements. Der Pfeil dreht sich um, solange die Liste unten ist — der einzige
Zustand, den eine ComboBox hat und den ihr eigenes Rechteck nicht zeigen kann.

Gebaut ist er aus **Zeilen von Kapseln**, nicht aus zwei gedrehten Balken (dem modernen
Chevron), und das ist keine Geschmacksfrage: `extract` faltet die Drehkette auf **jedes** Quad,
das ein Element ausgibt, und überschreibt dabei, was das Element selbst gesetzt hat — in einem
gedrehten Panel bekämen beide Chevron-Balken denselben Winkel und lägen parallel. Ein Dreieck
aus aufrechten Zeilen hat keinen Winkel zu verlieren. Die Rundung pro Zeile kostet nichts (es
ist dieselbe SDF, durch die ohnehin jedes Quad geht) und macht die Diagonalen kantengeglättet.

**Nachgesehen statt behauptet:** der Software-Rasterizer aus Block G rendert die Sache in ein
Bild, das man anschauen kann — geschlossen und offen, in drei Größen. Genau dafür ist er da.

**Und derselbe Blick auf die offene Liste, mit einer stark gerundeten ComboBox.** Gemeldet aus
einer laufenden App: die Liste sah bei einem runden Knopf falsch aus und der Optionstext lief
links über die Kurve hinaus. Zwei Symptome, **eine** Annahme — die Liste hat die Rundung des
Knopfes wörtlich übernommen, und der Textabstand war eine feste Zahl.

Drei Regeln stehen jetzt an einer Stelle:

- **Ein Rechteck gibt seine Ecke um den Radius zurück**, also muss der Inhalt um den Radius
  hereinrücken statt um eine feste 6 (`contentInset`). Sonst hängen die ersten Buchstaben bei
  jeder größeren Rundung außerhalb der Form — auf dem geschlossenen Knopf genauso wie in der
  Liste.
- **Eine Ecke, die tiefer ist als eine halbe Zeile, ist eine Ecke, die eine Zeile frisst**
  (`listRadius`). Die Liste ist deshalb nie runder als das, egal was der Knopf verlangt; ein
  Radius aus einer Theme-Stufe kann das leicht überschreiten.
- **Die Liste ist eine eigene Karte mit Abstand, kein angeklebter Deckel.** Der Knopf darf eine
  Kapsel sein, und ein Rechteck unter einer Kapsel lässt links und rechts zwei Kerben stehen, wo
  ihre unteren Ecken wegkurven. Eine Karte, die absteht, stimmt bei jeder Rundung.

Dazu nehmen **die erste und die letzte Zeile die Ecken der Karte** — jede auf ihrer Seite. Genau
dafür sind die vier getrennten Radien da: eine gleichmäßig gerundete Zeile ragt oben und unten
aus der Karte heraus, eine ungerundete lässt eine eckige Schulter in einer runden Form stehen.

**Und die Auswahl fällt beim Loslassen, nicht beim Drücken.** Gemeldet als „fühlt sich komisch
an", und das ist genau richtig: Drücken **zielt**, Loslassen **entscheidet**, und bis man
loslässt darf man es sich durch Bewegen noch anders überlegen. Eine Liste, die die Zeile nimmt,
sobald die Taste unten ist, nimmt die Entscheidung eine halbe Geste zu früh ab. Dasselbe gilt für
das Schließen ohne Auswahl — es ist dieselbe Entscheidung, andersherum getroffen, und passiert
deshalb im selben Moment.

Ganz nebenbei fällt damit **Drücken-Ziehen-Loslassen** heraus: Taste halten, die Liste
hinunterfahren, auf dem gewünschten Eintrag loslassen. So funktionieren Menüs, seit es Menüs
gibt, und es ist kein zweiter Codepfad, sondern die Folge davon, dass die Entscheidung an der
Loslass-Flanke hängt.

**Der alte Test hat den Unterschied nicht sehen können**, weil sein Klick-Helfer Drücken und
Loslassen in einem Rutsch schickte — beides ergibt dieselbe Auswahl. Er fährt die beiden Schritte
jetzt einzeln und prüft dazwischen, dass noch nichts entschieden ist, plus das Umentscheiden
durch Bewegen. Zurückgedreht auf die Druck-Flanke wird er rot.

---

### B3, erste Hälfte: die WrapBox (31.08.2026)

Sechzehnter Elementtyp und der einfachere der beiden Container aus B3: eine waagerechte Box,
die umbricht, wenn der Platz ausgeht. Schlagworte, Chips, eine Knopfleiste, die ein schmales
Fenster überleben muss — alles, was eine Zeile ist, **bis es keine mehr sein kann**.

Der Lauf ist bewusst genauso zustandslos wie der der Boxen (`wrapSlotRect`): keine
zwischengespeicherte Zeilentabelle, die ungültig werden kann, und die Listen sind kurz. Die
Gesamthöhe fällt als Nebenprodukt desselben Laufs ab, damit „an Inhalt anpassen" nicht eine
zweite, driftende Kopie derselben Arithmetik braucht.

**Drei Entscheidungen, die vorher fallen mussten:**

**1. Zwei Abstände, nicht einer.** `Spacing` ist die Lücke zwischen zwei Elementen **einer**
Zeile, `Line Spacing` die zwischen zwei Zeilen. Eine Reihe Chips will seitlich fast immer
enger sitzen als untereinander, und eine Zahl für beides erzwingt einen Kompromiss, den
niemand will.

**2. `Slot Fill` wird ignoriert.** Ein Kind, das den Rest frisst, nähme die ganze erste Zeile,
und es gäbe nie eine zweite — Füllen und Umbrechen widersprechen sich. Das Details-Panel zeigt
einem WrapBox-Kind deshalb **beide** Größen und kein Slot Fill, also genau umgekehrt zu einem
Box-Kind.

**3. `Size To Content` misst nur die HÖHE.** Die Breite ist das, wogegen umgebrochen wird — sie
aus den Kindern zu messen wäre die Frage, die sich selbst beantwortet. Die Höhe, also wie viele
Zeilen sie gebraucht haben, ist die nützliche Hälfte.

Dazu die Regel, die eine Zeile hoch macht: **so hoch wie ihr eigenes höchstes Kind**, nicht wie
das höchste im ganzen Kasten. Ein einzelnes großes Element darf nicht alle anderen Zeilen
mit auseinanderschieben.

**Beim ersten Testlauf lag ich falsch, nicht die Engine:** ich hatte drei Kinder à 100 mit
10 Lücke in 300 Breite gerechnet und drei erwartet — es sind 320. Die Erwartung war die
Rechnung, nicht das Verhalten. Gegengeprüft ist die Weiche selbst: nimmt man die
`UIWrapBox`-Zeile aus `uiElementRect` heraus, fallen die Kinder auf den Box-Lauf zurück und
elf Zusicherungen werden rot.

### B3, zweite Hälfte: das Grid (31.08.2026)

Siebzehnter Elementtyp und der Container, an dem D2 hängt: ein **Formular** ist eine
Beschriftungsspalte, die zu ihren Beschriftungen passt, neben einer Feldspalte, die den Rest
nimmt. Zwei gestapelte Boxen können das nur nachmachen, indem jemand von Hand jede Breite
abgleicht, und sie fallen auseinander, sobald eine Beschriftung wächst.

**Die Spurbezeichner sind ein On-Disk-Format** und stehen deshalb als Kasten im Header:
`120` fest, `*` ein Anteil, `2*` zwei Anteile, `auto` so breit wie das Breiteste darin.
Alles Unlesbare wird zu `*` — **absichtlich**: eine Spur, die man sieht und reparieren kann,
ist besser als eine, die auf null zusammenfällt und den Tippfehler versteckt. Dieselbe Regel
wie bei einer Theme-Rolle, die nicht mehr auflöst.

**Vier Entscheidungen, die vorher fallen mussten:**

**1. Eine Zelle IST der Slot.** Anker und Position des Kindes werden nicht gelesen, genauso wie
in einer Box. Wer einen kleinen Knopf in einer großen Zelle will, setzt ein Panel hinein — eine
Platzierungsregel für alle Container, sonst muss ein Autor lernen, welcher Container welche
Felder liest.

**2. Angeheftete Kinder werden ZUERST platziert**, egal wo sie im Baum stehen. Ein Element, das
seine Zelle nennt, muss sie bekommen; ein automatisches, das zufällig früher kommt, darf sie ihm
nicht wegnehmen.

**3. Mehr Kinder als erklärte Zeilen lassen die letzte Zeile sich wiederholen.** Ein
Einstellungsformular mit zwanzig Zeilen soll nicht zwanzig erklären müssen.

**4. Geparst wird beim SCHREIBEN, nicht beim Fragen.** Ein Treffertest läuft über jedes Element;
würde `uiElementRect` die Wörter neu zerlegen, geschähe das hundertfach pro Bild. Die Property
ist deshalb ein `uiprop::custom`-Slot, der beim Setzen neu parst — ein `Set Property` aus einem
Graphen darf nicht die Wörter ändern und das Layout auf den alten laufen lassen.

`Size To Content` zählt feste und `auto`-Spuren zusammen; gewichtete zählen als **nichts**, weil
ein Anteil am Rest genau das ist, was diese Messung erst herstellen soll — dieselbe Konvention,
die `UIBoxBase` für füllende Kinder schon nennt.

**Der Designer zeichnet die Spurlinien**, und zwar aus **demselben Löser**, mit dem der Runtime
legt (`uiGridTracks`, dafür exportiert). Ein leeres Grid, das nur seinen Umriss zeigt, ist ein
Rechteck, in das man nicht zielen kann — und ein gleichmäßig geteiltes Raster hätte dem Autor ein
Layout gezeigt, das es nicht gibt. Ich hatte es zuerst gleichmäßig geteilt und den Kommentar
daneben geschrieben, der das Gegenteil behauptete; das ist die Sorte Zeile, die später jemand
glaubt.

**Und der Hilfe-Audit hat meine zwei neuen Bedienelemente nicht gesehen.** Sein Muster kennt
`ImGui::DragFloat\d?`, aber `ImGui::DragInt` **ohne** Ziffer — jedes `DragInt2` im ganzen Editor
war für ihn unsichtbar. Nach dem Fix meldete er sofort zwei fehlende Einträge. Zweite Lücke
derselben Art nach der festen Dateiliste: **ein Deckungswerkzeug, das etwas nicht kennt, meldet
keine Lücke, sondern Deckung.**

### D2, erste Hälfte: der Bauteil-Vertrag (01.09.2026)

Eine Komponentenbibliothek scheitert vor der ersten Komponente an einer Frage: **wie sagt man
einer eingebetteten Kopie, was sie sagen soll?** Ein `WidgetRef` pfropfte den fremden Baum bisher
exakt so ein, wie er verfasst wurde. Eine Formularzeile mit eingebackener Beschriftung ist
deshalb *eine* Formularzeile, keine Formularzeile, und eine Bibliothek daraus ist ein Katalog
von Bildschirmfotos.

**Ein Parameter benennt eine Eigenschaft eines Elements und gibt dem Paar einen eigenen Namen.**
Die Seite speichert „Label", nicht „die Text-Eigenschaft von Element 7". Diese Zwischenschicht
IST das Merkmal: der Autor der Komponente darf das innere Element umbenennen, die Eigenschaft
auf ein anderes verschieben oder die Zeile ganz neu bauen, und jede Seite, die sie benutzt,
läuft weiter. Adressierte die Seite Element und Eigenschaft direkt, wäre jedes Innendetail Teil
des Vertrags.

**Drei Entscheidungen, die vorher fallen mussten:**

**1. Parameter sind Element-Eigenschaften, keine Skript-Variablen.** Das war die eine echte
Weggabelung, und sie entscheidet sich am Designer: **Graphen laufen dort nicht.** Eine Seite mit
fünf Formularzeilen zeigte fünf identische Beschriftungen im Designer und fünf verschiedene zur
Laufzeit — und der Designer löge genau über das, wofür es ihn gibt. Eigenschaften werden auf die
Kopie geschrieben, die die Vorschau ohnehin schon anlegt, also stimmt beides. Nebenbei umgeht es
die Frage, ob eine *kompilierte* Instanz überhaupt vorbelegte Variablen annimmt.

**2. Ein nicht gesetzter Parameter ist kein leerer Wert**, sondern das, was der Autor der
Komponente in der Eigenschaft stehen ließ. Der Standard wohnt damit an genau einer Stelle, und
es gibt keine zweite, die ihm widersprechen könnte.

**3. Ein Wert, den niemand mehr deklariert, wird verworfen, nicht geraten.** Der Autor hat den
Knopf umbenannt oder entfernt; ihn still in das zu schreiben, was jetzt an der Stelle steht,
wäre schlimmer als eine Beschriftung, die bleibt, wie sie verfasst wurde. Fällt *jeder* Wert
weg, sagt es das im Log — Schweigen sähe aus wie eine Komponente, die ignoriert, was man ihr
sagt, und das ist das am schwersten zu erklärende Nichts.

**Einer Komponente eine Farbe zu nennen, löst diese Farbe vom Theme.** Ohne diese eine Zeile
kämpfen die beiden: der Parameter schreibt beim Pfropfen, der Theme-Durchgang überschreibt ihn
eine Zeile später, und zwar nur im Runtime — der Designer fährt gar keinen Theme-Durchgang.

**Und die Basis-Eigenschaften brauchten endlich eine aufzählbare Liste.** `getBaseProp` ist eine
if-Kette; nichts musste sie je durchgehen, weil das Details-Panel jede Zeile von Hand an ihren
Platz zeichnet. Ein Parameter muss es: „die Hilfszeile nur bei den Zeilen zeigen, die Hilfe
haben" ist das Gewöhnlichste, was eine Komponente will, und das ist `Visible`. `uiBaseProperties()`
ist dieselbe Liste ein zweites Mal, und ein **Test hält sie in Deckung** statt Disziplin —
ein Name, der nur in der if-Kette steht, würde schlicht nie angeboten, und das sieht aus wie
„die Eigenschaft gibt es nicht".

Gegengeprüft am Runtime-Pfad: nimmt man das Anwenden aus `embedWidgetRefs` heraus, sagen beide
Kopien wieder „Default" — sieben Glyphen statt einer und dreier. Geprüft wird am **Bild**, nicht
am Baum: der lebende Baum gehört dem WidgetManager, und die Frage „steht da, was ich gesetzt
habe" muss ohnehin nur dort wahr sein, wo man sie sieht.

### D2, zweite Hälfte: die ersten zwölf Komponenten (01.09.2026)

`EditorDeps/EngineContent/Widgets`, erzeugt von **`widget_gen`** nach dem Vorbild von
`mesh_gen`: deterministisch, feste UUIDs, Ausgaben **eingecheckt**, nicht Teil des normalen
Build-Graphen. Der Grund für einen Generator statt zwölf von Hand gespeicherter Assets ist
einfach: ein `.hasset` ist ein Binärcontainer, und zwölf davon im Repository wären zwölf Klumpen,
die niemand prüfen oder reparieren kann, ohne den Editor zu öffnen. Genau die Bibliothek muss
aber prüfbar bleiben, weil jedes Projekt sie erbt. Hier ist eine Komponente eine Funktion.

**Die zwölf**: `FormRowText`, `FormRowToggle`, `FormRowChoice`, `SectionHeader`, `Card`,
`ListRow`, `TitleBar`, `Toolbar`, `StatusBar`, `SearchField`, `EmptyState`, `DialogFrame`.

**Eine Komponente ist ein ganzes Ding, kein Rahmen mit einem Loch.** Ein `WidgetRef` pfropft
einen fertigen Baum; eine Seite kann keine eigenen Kinder hineinsetzen. Deshalb gibt es keine
„Formularzeile, in die man sein Bedienelement stellt", sondern eine Text-, eine Schalter- und
eine Auswahlzeile, jede vollständig. **Diese Grenze ist es wert, benannt zu werden**, statt um
sie herum zu bauen: die Alternative ist ein Slot-Mechanismus, und diese Bibliothek ist genau
das, woran sich zeigt, ob man ihn braucht.

**Farben und Größen kommen aus dem Theme, und das Literal daneben ist die Vorschau.** Jede Farbe
bindet eine Rolle, jede Textgröße eine Stufe; `bake()` schreibt anschließend die Werte des
Standard-Themes in die gewöhnlichen Eigenschaften, damit der **Designer** — der gar keinen
Theme-Durchgang fährt — die Komponente so zeigt, wie sie aussehen wird. Das Literal ist die
Vorschau, die Rolle ist die Wahrheit.

**Der Dialog ist die einzige Komponente mit eigener Logik, und sie brauchte keinen neuen
Mechanismus.** Er wird mit `Create Widget` → `Show Modal` geöffnet, die Seite hält also schon
eine Referenz auf ihn — er sendet `Confirmed` beziehungsweise `Cancelled` und versteckt sich
selbst, die Seite bindet sich mit `Bind Event` daran. Ein Dialog, der nach OK stehen bliebe,
machte jede Seite dafür zuständig, etwas zu schließen, das sie nicht geöffnet hat.

**Drei Prüfungen halten die Bibliothek ehrlich**, und alle drei sind Tests, keine Disziplin:
- **Jeder Parameter schreibt wirklich.** Der Test setzt jeden auf einen *anderen* Wert und liest
  ihn zurück. Ein Parameter, der eine Eigenschaft nennt, die es an diesem Element nicht gibt,
  schreibt nichts und sagt nichts — gegengeprüft, indem ich einen auf „Caption" umbog: der Test
  nannte sofort Datei und Parameternamen.
- **Die Farben gehören dem Theme.** Ein Moduswechsel muss an jeder Komponente etwas bewegen.
  Eine, die ihre Farben selbst entschied, sieht richtig aus, bis jemand auf Dunkel schaltet.
- **Eine Seite, die eine Komponente einbettet, übersteht das Paketieren.** Der gefährlichste
  Pfad: ein ausgeliefertes Spiel hat kein `EditorDeps`, die Komponente muss im `.hpak` liegen
  und der `Engine/…`-Pfad aus dessen Index auflösen. Genau so präsentierten sich der
  `GameInstance.hcode`- und der Pfadindex-Fehler, und keiner der beiden zeigte sich im Editor.
  Gegengeprüft, indem ich `engineContentDir` leerte.

**Und ein Kontaktabzug**, gerendert durch den Software-Rasterizer nach
`$TMPDIR/he_components.ppm`: „zeichnet überhaupt etwas" kann ein Test sagen, „sieht aus wie eine
Formularzeile" ist eine Frage an ein Bild. Beim ersten Abzug las die halbe Bibliothek als kaputt
— weißer Text auf hellem Grund. **Der Fehler war der Abzug, nicht die Bibliothek**: `createWidget`
löst die Rollen gegen den Modus auf, der gerade gilt, und ich hatte den Hintergrund des *anderen*
gemalt. Zweite Fassung, zweiter Fund: gleich hohe Fächer streckten eine 62 Pixel hohe
Formularzeile auf zweihundert und zeigten eine Beschriftung im Leeren. Jedes Fach ist jetzt so
hoch, wie die Komponente verfasst wurde — sonst ist der Abzug ein Bild der eigenen Rechnung.

**Offen geblieben:** das Handbuch hat noch keinen Abschnitt über Komponenten (die Docs liegen im
Website-Repository), die neuen Hilfe-Einträge zeigen deshalb auf `ui#widgets` statt auf einen
Anker, der 404 gäbe.

### Block C: das Berechtigungsmodell, `process` und `fs` entsperrt (01.09.2026)

Die Fallen-Liste unten sagt: **`fs` zu entsperren ist eine Einbahnstraße, das Berechtigungsmodell
vorher festlegen.** Also zuerst das Modell, und es ist ein Satz:

> **Die Berechtigung sagt, was ein SKRIPT von sich aus benennen darf. Sie sagt nie, was ein
> MENSCH auswählen darf.** Ein Pfad, den jemand in einem Dateidialog gewählt hat, ist danach
> frei — das Auswählen IST die Erlaubnis. `allowFiles` ist die Decke darüber, für den Fall, dass
> ein Skript einen absoluten Pfad nennt, ohne dass jemand ihn gewählt hat.

Drei Türen im `.heproj` (`allowFiles`, `allowProcesses`, `allowNetwork`), alle **zu**, solange
ein Projekt nichts anderes sagt. Sie reisen in den `project.hcfg` des Exports und stehen im
Editor unter *Preferences ▸ Project ▸ Permissions*.

**Der Editor hängt an denselben drei Türen wie die ausgelieferte App**, absichtlich. Eine
Vorschau, die fremde Verzeichnisse löschen darf, während der Export es nicht darf, ist der
schlechtere der beiden Fehler: der Schaden passiert auf der Maschine des Autors, bevor irgendwas
ausgeliefert werden konnte.

**`abwesend` heißt hier `zu`**, und nur hier unter den Projekt-Schaltern. `advancedShaderEffects`
liest ein fehlendes Feld als *an*, weil jedes ältere Projekt Materialien hatte; ein fehlendes
`allowFiles` als *an* zu lesen, würde jedem Projekt rückwirkend jede Tür öffnen. Im `hcfg`
stehen die drei deshalb **gerade** und nicht negiert wie das Shader-Bit.

**`resolved()` ist die eine Stelle**, an der aus der Zeichenkette eines Skripts ein echter Pfad
wird — relativ immer, absolut nur mit Decke oder Erteilung. Genau deshalb konnten die sechs neuen
Zeilen (`isDir`, `size`, `modified`, `list`, `rename`, `copy`) dazukommen, ohne dass eine davon
die Prüfung vergessen kann.

**Ein Präfix auf Pfadteilen, nicht auf Zeichen.** Erteilt jemand `/tmp/out`, darf das nicht
`/tmp/out_private` erteilen, und ein `..` muss **vor** dem Vergleich verrechnet sein, nicht
danach. Gegengeprüft: ersetzt man den Vergleich durch das naheliegende `rfind(base, 0) == 0`,
werden beide Zusicherungen rot — die Nachbardatei ist lesbar und der Umweg über `..` auch.

**`process` läuft auf `HE::Proc`**, das es schon gab und das aus gutem Grund kein `popen` ist
(argv-Vektor statt Shell-Zeile, stdout und stderr getrennt, ehrliche Exit-Codes, Timeout).
`run` gibt **vier** Werte zurück: wer nur wissen will, ob es geklappt hat, liest `ok`; wer einem
Menschen erklären muss, warum nicht, braucht Exit-Code und `err`. Ein Exit-Code ungleich null ist
eine **Antwort**, kein Fehler.

**`which` ist bewusst NICHT gesperrt.** Zu fragen, ob ein Programm installiert ist, führt nichts
aus — und „dafür brauchst du git" ist genau die Nachricht, die jemand braucht, um zu entscheiden,
ob er die Berechtigung überhaupt erteilt. Eine Sperre dort hätte den Nutzer im Dunkeln gelassen.

**`fs.list("")` ist die Wurzel**, und nur diese Zeile darf das. `validRel` weist den leeren
String für jede andere Zeile zu Recht ab (dort benennte er das Wurzel-VERZEICHNIS als Datei);
„was liegt oben in meinem Sandkasten" ist aber eine gewöhnliche Frage ohne zweite Schreibweise.

**`copy` überschreibt nicht.** Es ist die einzige Zeile hier, die eine Datei zerstören kann, die
der Aufrufer nicht genannt hat, und ein verlorenes Dokument kostet mehr als ein `false`.

**Und eine Falle für die Testdatei selbst:** die Berechtigungen sind prozessweite Statics. Ein
Test, der Dateizugriff anlässt, würde die Ausbruchs-Zusicherungen im Sandkasten-Test daneben
still lizenzieren. Jeder neue Fall stellt den zugesperrten Standard über einen Destruktor wieder
her.

**Die drei Dateidialoge sind der Erteilungsmechanismus** und deshalb Teil derselben Welle, nicht
eine spätere Bequemlichkeit: ohne sie gibt es keinen Weg, einem Skript einen Pfad zu geben, ohne
dass jemand eine Berechtigung tippt. `dialog.openFile/saveFile/pickFolder` reichen ihr Ergebnis
an `fs::grantPath` weiter, und das ist deren **einziger** Aufrufer — eine Zeile, die ihr eigenes
Argument erteilt, wäre ein Modell, das alles erlaubt.

**Sie sind synchron, obwohl SDLs es nicht sind.** Ein Graph-Pin kann keine Fortsetzung halten,
also wird gewartet: `SDL_PumpEvents` in einer Schleife, **pumpen und nie abholen**. Pumpen ist
das, was die Portal-Picker unter Linux brauchen, und es lässt jedes Ereignis in der Warteschlange
für die eigene Schleife der Anwendung — abholen hieße, Skriptcode innerhalb eines Skriptaufrufs
laufen zu lassen, und genau das darf ein Modal nicht. Der Rückruf kommt laut SDL womöglich aus
einem anderen Thread, also eine kleine gesperrte Box plus ein atomares Flag.

**Der Wartepfad war ein echter Absturz, bevor er einer wurde.** Die Box lag zuerst auf dem
**Stack**: läuft die Frist ab, kehrt der Aufrufer zurück, sein Rahmen verschwindet — und SDL hält
den Zeiger weiter, denn der Dialog steht ja noch offen. Ein Klick nach einem langen Telefonat
schreibt dann in toten Stack, und die Filter-Strings hingen genauso dran (SDLs eigener Header
sagt, sie müssen den Rückruf überleben). Jetzt liegt die Box auf dem Heap und gehört dem, der
zuletzt fertig wird: ein Zustand mit drei Werten, jeder Übergang ein CAS, und **wer sein CAS
verliert, gibt frei**. Auf der Frist wird die Box also übergeben, nicht weggeworfen.

**Kein `title`-Pin.** SDLs Picker nehmen keinen, die Plattform stellt ihren eigenen — und ein
toter Pin ist hier schlimmer als anderswo, weil ein gespeicherter Graph Pin-INDIZES ablegt.
Später entfernt hätte er jeden Graphen dahinter umverdrahtet; jetzt kostet er nichts.

**Was hier ehrlich offen bleibt:**
- Die drei Dialoge selbst sind **nicht getestet**. Sie brauchen einen Desktop, und die Sandbox
  hier hat keinen. Getestet ist, woran sie hängen — dass `grantPath` die Tür öffnet, dass ein
  Präfix auf Pfadteilen vergleicht und dass ein `..` vorher verrechnet wird. Der erste echte
  Klick auf „Datei öffnen" ist die Probe, die noch aussteht.
- **Der Datei-Watcher aus der `fs`-Zeile ist NICHT gebaut**, und das ist eine bewusste
  Verschiebung, keine Auslassung. Ein Watcher muss ein Ereignis an ein Skript liefern, und den
  Weg vom Host in einen laufenden Graphen gibt es für so etwas noch nicht — das ist ein eigenes
  Stück Arbeit und gehört zu dem, was auch `http` braucht. Bis dahin ist der Behelf `fs.modified`
  plus eine Delay-Schleife, und genau so macht es der Editor selbst mit seinem 1,5-Sekunden-Takt.
- **`fs.modified` fährt auf einem 32-Bit-Float-Pin**, wie `datetime.now` auch. Auf Epoch-Größe
  sind das rund zwei Minuten Auflösung. Für „wie alt ist diese Datei" reicht das; wer eine
  Änderungserkennung darauf baut, baut auf Sand und sollte die Größe vergleichen.

### E4 Stufe 3: der Zustand über den Reload (01.09.2026)

**Stufe 2 gab es schon**: ein gespeichertes Asset setzt `m_appPreviewRestartPending`, und
`restartAppPreview` baut die Vorschau neu. Richtig für „neu starten", falsch für „ich habe eine
Beschriftung geändert" — und das ist der meiste Speichervorgang. Ein halb ausgefülltes Formular
ging dabei verloren.

**Zwei Hälften, zwei verschiedene Schlüssel, weil es zwei verschiedene Fragen sind:**

**Elemente über (Asset-Pfad, welche Kopie, Element-Id).** Eine Id wird einmal vergeben und nie
neu nummeriert, überlebt also jede Änderung außer dem Löschen — **auch das Umbenennen**. Ein
umbenanntes Feld verliert nicht, was darin stand. (Der Plan sagt oben pauschal „Umbenanntes
überlebt nicht"; für Elemente stimmt das nicht, für Variablen schon, und die Zeile daneben sagt
es genau so.) „Welche Kopie" muss dabei sein: zwei Kopien eines Widgets auf einer Seite sind
gewöhnlich, und der Pfad allein gäbe beiden den Zustand der ersten.

**Variablen über den Namen auf der Skript-Instanz.** Ein Graph hat keine stabile Id für eine
Variable, der Name IST ihre Identität — eine umbenannte Variable ist von einer gelöschten plus
einer neuen nicht zu unterscheiden, und ihr Wert ist weg.

**Was erhalten wird, ist kurz**, und das ist Absicht: Text und Cursor eines Feldes, der Haken,
der Wert eines Reglers, die Auswahl einer ComboBox oder Liste, der Versatz beim Scrollen, der
Fokus. **Eine Beschriftung, die ein Skript geschrieben hat, ist kein Zustand, sondern Ausgabe** —
sie wiederherzustellen hieße, die Antwort von gestern über eine frisch gerechnete zu kleben. Die
Tabelle `statePropsOf` ist die einzige Stelle, die das entscheidet; ein neuer Elementtyp trägt
dort eine Zeile ein oder wird eben nicht übertragen, und das ist die sichere Richtung.

**Nicht passt heißt fallen lassen, nicht raten.** Die Id muss da sein UND die Eigenschaft muss
noch denselben Typ haben. Gegengeprüft, indem ich die Typprüfung ausgehängt habe: dann landet
der Text eines TextInput in der Auswahlnummer einer ComboBox, und der Test wird rot.

**Und der Editor sagt es, wenn nichts gepasst hat.** Eine Neuerstellung, die die Widgets
umgebaut vorfindet, hat weggeworfen, was drin war, und „mein Formular hat sich selbst geleert"
ist genau die Sorte Sache, über die jemand eine Stunde rätselt. Nur wenn es überhaupt etwas zu
verlieren gab.

**Dabei hat mich mein eigener Zähler erwischt:** `landed` zählte „nichts war fokussiert" als
erhaltenen Zustand mit. Damit stand die Zahl bei jedem Widget über null, und die Warnung wäre
nie erschienen — die Prüfziffer hätte genau das kaputtgemacht, wofür es sie gibt. Der Knopf
„Restart Live Preview" ist der **einzige** Weg, der den Zustand absichtlich fallen lässt; er
existiert ja, um aus einem Zustand herauszukommen.

**Und bei den Variablen war der Wächter zuerst der falsche.** Ich hatte auf den Typ geprüft:
„gibt es die Variable noch und passt sie". Nur antwortet `getVariable` auf einen Namen, den der
Graph nicht mehr kennt, mit einem default-konstruierten Wert — und dessen Typ ist **Float**. Eine
umbenannte oder gelöschte **Float**-Variable passte damit auf ihren eigenen Unfall und wurde im
Instanz-Speicher neu angelegt: ein Wert, den niemand liest, niemand aufräumt und mit dem eine
spätere Umbenennung kollidieren kann. Richtig ist die **Mitgliedschaft** in dem, was die frisch
gebaute Instanz deklariert.

**Meine erste Gegenprobe dazu blieb grün**, und das war der eigentliche Fund: ich hatte die
umbenannte Variable als String angelegt, und ein String passt nicht auf einen Float-Unfall. Erst
mit einer **Float**-Variablen wurde der Test rot. Zum wiederholten Mal dieselbe Lehre — **eine
Gegenprobe, die nicht rot wird, prüft nicht, was man denkt.**

### B1b: mehr als eine Zeile (01.09.2026)

Erster Punkt aus Welle 3, und der einzige, den die vorhandene kopflose Prüfung ganz abdeckt —
eine echte Menüleiste auf macOS könnte ich hier gar nicht anfassen.

**Der Kern ist ein Zeilenmodell aus BYTE-BEREICHEN**, weil ein Cursor ein Byte-Versatz ist.
`layoutUITextLines` kann das nicht liefern, und zwar aus zwei guten Gründen, die für ein Label
richtig und für einen Editor tödlich sind: der Umbruch **schneidet Leerzeichen weg** und jedes
`\r` wird verschluckt. Für eine Beschriftung ist beides unsichtbar; in einem Feld, in das jemand
tippt, ist ein Byte, das das Zeilenmodell nicht benennen kann, ein Byte, das der Cursor nicht
erreicht. Also `uiTextLineRanges` daneben: harte Umbrüche, jedes Byte verbucht.

**Und es behält die leere letzte Zeile** — genau die, die der Label-Splitter absichtlich
wegwirft. Das war seinerzeit ein echter Fix (eine Beschriftung mit versehentlichem Zeilenumbruch
wurde eine halbe Zeile zu hoch gezeichnet). Dieselbe Regel im Editor hieße: Enter am Ende setzt
den Cursor auf eine Zeile, die es nicht gibt. **Beide Regeln sind richtig für das, was sie
bedienen, und deshalb sind es zwei Funktionen und kein Schalter.**

**Enter bindet nicht mehr ab.** In einem mehrzeiligen Feld fügt Return einen Umbruch ein, und es
gibt danach keine Taste mehr, die „fertig" heißen könnte — so ein Feld meldet `OnTextCommitted`
beim Fokusverlust. Entschieden in `inputSubmit`, der einen Stelle, an der Return ankommt, statt
jeden Host den Unterschied lernen zu lassen. Der Eingabefilter gilt weiter: ein Feld, das nur
Ziffern nimmt, darf keinen Umbruch durch die eine Taste bekommen, die die Prüfung überspringt.

**Home und End sind jetzt pro ZEILE**, ohne `multiline`-Abfrage — in einem einzeiligen Feld IST
die eine Zeile das ganze Feld, also antworten sie dort unverändert.

**Die Zielspalte ist der Teil, den man am leichtesten weglässt.** Ohne sie verliert der Cursor
beim Abwärtsgehen durch eine kurze Zeile seine Spalte und klebt am Zeilenende: dreimal Runter
und er steht links. Gegengeprüft, indem ich `preferredCaretX` ausgehängt habe — der Cursor landet
auf 18 statt auf 26.

**Y ist ein echtes Argument, kein voreingestelltes.** `caretAtPoint`, `setCaretFromPointer`,
`dragCaretFromPointer` und `selectWordAtPointer` haben es bekommen, und der Compiler hat die
Aufrufer gesucht. Ein Vorgabewert 0 hätte still „Zeile eins" geheißen, an jeder Stelle, die es
nicht gelernt hat.

**Das Feld ist beim Scrollen ein Container.** `scrollOffsetPtr`/`maxScrollAmount` melden für ein
mehrzeiliges Feld seinen senkrechten Versatz, für ein einzeiliges `nullptr` — damit fahren das
Mausrad (`uiScrollBy`) und die Zustandssicherung der Live-Vorschau (E4) ohne eine einzige neue
Verdrahtung darüber, und ein Rad über einem gewöhnlichen Feld scrollt weiter die Seite.

**Der Zeichenpfad ist ein eigener Zweig**, kein Dutzend `multiline`-Abfragen im bestehenden: ein
mehrzeiliges Feld scrollt in die andere Richtung, richtet oben aus und malt seine Auswahl als
mehrere Rechtecke. Jede Zeile wird in ihr EIGENES Ein-Zeilen-Rechteck ausgegeben, statt die
ganze Zeichenkette der Textschicht zu geben — die würde neu umbrechen und neu ausrichten, und
dann gäbe es zwei Antworten darauf, wo Zeile sieben steht.

**Nicht gebaut, und der Plan sagt es statt es zu verschweigen: Wortumbruch IM Eingabefeld.**
Der Umbruch-Splitter verwirft Bytes (siehe oben), ein Editor braucht aber einen, der jedes
behält. Das ist ein eigener Splitter, und ihn in den Label-Pfad zu mischen würde jede
Beschriftung im Baum verändern. Bis dahin bricht ein mehrzeiliges Feld dort um, wo jemand Enter
gedrückt hat.

### B5: Reiter und Splitter (01.09.2026)

Achtzehnter und neunzehnter Elementtyp, und die beiden Hälften desselben Satzes: **ein Tab Box
sagt WELCHE Seite, ein Splitter sagt WIE VIEL Platz.** Zusammen sind sie das, was eine App mit
Seitenleiste ausmacht.

**Die Kinder des Tab Box SIND die Seiten, und der Name eines Kindes ist seine Beschriftung.**
Keine zweite Liste. Ein Array von Titeln neben einer Liste von Kindern sind zwei Dinge, die von
Hand in Deckung gehalten werden müssen — und beim ersten Umsortieren gehört jede Beschriftung
zur falschen Seite.

**`hidesChild` ist eine Frage an den ELTERNTEIL**, und `uiElementEffectiveVisible` ist die eine
Stelle, die sie stellt. Das ist der ganze Punkt: Bild und Zeiger kommen beide dort durch, also
kann ein Knopf auf einer versteckten Seite keinen Klick an seinen eigenen Koordinaten
beantworten. Genau diese Sorte Auseinanderdriften steht schon in der Fallen-Liste („Widget-Extract
und Pointer-Test müssen durch dieselbe Auflösung"), und der Test dazu ist als **Eingabe**
formuliert und nicht als Geometrie: zwei Seiten mit deckungsgleichen Knöpfen, und nur der auf der
aktiven Seite darf gedrückt werden. Gegengeprüft — Tor ausgehängt, fünf Zusicherungen rot.

**Es fragt den BAUM statt einen Index zu speichern.** „Das wievielte Kind bin ich" ist Wissen des
Baumes; eine Zahl auf dem Element wäre eine, die veralten kann, und zwar auf genau dem Pfad, wo
veraltet „ein Klick erreicht etwas Unsichtbares" heißt. Nur die *Beschriftungen* für den
Streifen werden zwischengespeichert (`uiApplyAutoSize` füllt sie), und das Schlimmste, was ein
alter Eintrag dort kostet, ist ein Wort, das ein Bild zu spät stimmt.

**`tabLayout` ist eine Arithmetik mit drei Verbrauchern** — Zeichnen, Treffertest, Designer.
Dieselbe Lehre wie bei der ComboBox, wo zwei Kopien in dem Moment auseinanderliefen, in dem die
Ecken rund wurden. Der Treffertest liest die Beschriftungen dabei **aus dem Baum**, nicht aus dem
Zeichen-Cache: was ein Klick trifft, darf nie davon abhängen, ob schon ein Bild gezeichnet wurde.

**Ein Tab ist so breit wie das, was er sagt.** Gleich breite Tabs sehen aufgeräumt aus, bis eine
Seite „Einstellungen" heißt und die nächste „A". Überlauf wird **abgeschnitten**, ausdrücklich
und nicht aus Versehen; ein scrollender Streifen gehört zu B9.

**Der Splitter nimmt GENAU zwei Kinder.** Drei Bereiche hätten zwei Trenner und ein Verhältnis,
das keine Zahl mehr ist — und ein Splitter im Splitter sagt dasselbe mit bereits gelöster
Arithmetik. Dafür gibt es einen eigenen Test: die Bereiche des inneren teilen die **rechte Hälfte
des äußeren**, nicht die Leinwand, und das ist der Fehler, den ein Rechteck-Raum-Ausrutscher
macht.

**Die Mindestbreiten greifen im LAYOUT, nicht nur beim Ziehen.** Ein verfasstes Verhältnis von
0,01 mit 100 Pixeln Minimum muss geklemmt liegen, sonst widersprechen sich Designer und Runtime
in dem Moment, in dem die Datei geladen wird. Das Verhältnis selbst wird roh gespeichert und an
genau einer Stelle geklemmt (`clampedRatio`) — gegen ein Minimum zu ziehen und loszulassen darf
nicht einen Wert hinterlassen, den das Layout danach verschiebt.

**Nur der TRENNER startet ein Ziehen**, nicht der ganze Container. Beide neuen Typen sind
`interactive()` und damit die einzigen Layout-Container, die nicht zeigerdurchlässig sind — die
Bereiche liegen tiefer und decken alles außer dem Trenner ab, also erreicht ein Druck den
Splitter genau dort, wo er soll. Das hat mich einen Testlauf gekostet: ohne `interactive()`
liefert der Manager den Druck gar nicht erst aus.

**Beide gehören zum Vorschau-Zustand** (E4): welcher Reiter offen war und wohin der Trenner
gezogen wurde, hat ein Mensch dorthin getan. Zwei Zeilen in `statePropsOf`, und es komponiert
sich von selbst.

**Nicht gebaut: das Akkordeon**, das der Plan in derselben Zeile nennt. Es ist kein Reiter mit
anderem Aussehen, sondern ein Stapel aus aufklappbaren Abschnitten — näher an einer Liste mit
Kopfzeilen als an einem Tab Box, und es wird billig, sobald es „aufklappbar" als eigenes Stück
gibt.

---

### B7, erste Hälfte: was von außen hereinkommt (02.09.2026)

Eine Datei aufs Fenster ziehen, der Teil von B7, den man einer App zuerst ansieht. Die zweite
Hälfte steht direkt darunter.

**Ein Drop ist keine Maus.** Er kommt als eigene Ereignisfolge, mit einer Position im Fenster,
und während er in der Luft ist bewegt sich der Zeiger nicht. Deshalb sind es zwei Aufrufe,
`dropHover` und `processDrop`, und nicht ein Flag am Zeiger.

**Angenommen wird ausdrücklich, geerbt wird nach oben.** `acceptsDrop` ist ein Feld am
Basiselement und kein eigener Typ, weil „hier kann man etwas ablegen" etwas ist, das eine
Fläche, eine Liste, ein Bild und ein Textfeld alle sein wollen. Der Drop wandert vom
getroffenen Element nach oben zum ersten, das zusagt, genau wie ein Klick: eine Datei, die man
auf die Beschriftung einer Karte zieht, zieht man auf die Karte.

**Ein Ereignis pro Datei.** Drei Dateien sind drei `OnFileDropped` mit je einem Pfad. Eine
Liste als Nutzlast würde den einfachen Fall das Gewicht des schweren tragen lassen, und die
Ereignis-Argumente kennen ohnehin nur Exec, String, Float, Bool und Int.

**Was kein Element annimmt, hat das Fenster genommen.** Dann hört es die GameInstance, mit
elem 0. Eine Anwendung, die öffnet was man ihr gibt, will genau eine Stelle dafür und nicht
eine pro Panel. Beides zugleich feuert nie, sonst öffnete jede App jede Datei doppelt.

**Der Treffertest ist jetzt eine Funktion mit zwei Aufrufern.** `topmostHit` war der
eingerückte Doppelblock in `processPointer`; ihn stehen zu lassen und für den Drop
nachzubauen wäre die Falle aus der Liste unten gewesen, wörtlich: Bild und Zeiger müssen
durch dieselbe Auflösung. Der Drop gehört zum Zeiger. Aus demselben Grund ist auch der Ring
nur einmal geschrieben (`emitRing`), die beiden Aufrufer unterscheiden sich in Farbe und in
der Frage, an welchem Element er hängt, in sonst nichts.

**Der Rahmen sagt, wo es landet, nicht wo die Tasten hingehen.** Zwei Ringe in zwei Farben,
und das ist kein Schmuck: wer eine Datei zieht, muss auf einen Blick sehen können, welche der
beiden Zusagen gerade gemeint ist.

**Nebenbefund, mitgefixt:** `SDL_EVENT_SYSTEM_THEME_CHANGED` lag im
`isEditingText()`-Zweig von `GameApplication::OnEvent`. „Dem System folgen" funktionierte
damit nur, solange jemand gerade in einem Textfeld tippte. Der Zweig war der falsche Ort,
und das fällt erst auf, wenn man daneben etwas Neues einhängt.

**Die Live-Vorschau nimmt sie auch.** Der Editor hatte bisher gar keinen Drop-Pfad. Das
Viewport-Panel meldet jetzt neben dem Zeiger auch sein Rechteck, in Fensterpunkten und
gegen die eigene Plattform-Ansicht gerechnet, damit ein herausgezogenes Panel nicht um die
Fensterposition daneben liegt; die SDL-Fenster-Id kommt mit, weil ein Drop auf ein anderes
Fenster kein Drop auf dieses ist.

---

### B7, zweite Hälfte: etwas in der Anwendung tragen (02.09.2026)

**Ein Zug beginnt an einer Strecke, nie am Druck.** Das ist der ganze Unterschied zwischen
einem ziehbaren Ding, das man noch anklicken kann, und einem, das man nicht mehr anklicken
kann: ein Druck ist auch der Anfang eines Klicks. Also gibt es zwei Zustände, gespannt und
aktiv, und dazwischen liegen vier Pixel. Gemessen wird ab dem Druck und nicht ab dem letzten
Bild, sonst ist ein langsames Ziehen keines.

**Was daraus folgt, ist die Zeile, die man vergisst:** ein Druck, der zum Zug geworden ist,
darf beim Loslassen kein Klick mehr sein. Ohne sie feuert jeder ziehbare Knopf genau dann,
wenn man ihn wieder hinlegt.

**Die Nutzlast ist ein String, den die Quelle über sich selbst sagt.** Eine Listenzeile
schreibt ihren Index hinein, ein Werkzeugknopf sein Werkzeug. Leer heißt: der Name des
Elements, und damit funktioniert „ziehe dieses benannte Ding auf jenes" ohne eine Zeile
Logik. Mehr als ein String passt ohnehin nicht in ein Ereignis-Argument, und ein
Nutzlast-System nebenher wäre eine zweite Wertewelt neben HorizonCode.

**Drei Ereignisse, weil es drei Momente sind:** `OnDragStarted` an der Quelle, `OnDrop` am
Ziel mit der Nutzlast, `OnDragEnded` wieder an der Quelle mit einem Bool. Das Bool ist das,
woran eine Quelle merkt, dass sie sich zurücklegen muss.

**Aufheben ist Zeigerinteraktion.** `draggable` musste in `isInteractive`, sonst blast der
Druck an einem ziehbaren Panel ohne Klick-Bindung nach oben vorbei und es spannt nie. Ein
Flag, das sagt „hier tut der Zeiger etwas", muss eine der Antworten auf „tut der Zeiger hier
etwas" sein.

**Auf sich selbst kann man nichts fallen lassen**, und das ist kein Sonderfall, sondern der
Normalfall: das getragene Ding ist per Definition unter dem Zeiger. Quelle und alles darin
fallen deshalb aus der Zielsuche heraus, im Bild wie beim Loslassen, sonst verspricht der
Rahmen etwas, das das Loslassen nicht halten kann.

**Escape gehört dem Zug, vor jedem Dialog.** Er wurde zuletzt begonnen, und „mach die Geste
rückgängig, in der ich gerade stecke" ist, was Escape dort heißt. Ein Zug, dessen Zeiger
ungültig wird (eingefangen, aus dem Viewport), geht aus demselben Grund zurück.

**Die Quelle wird halb durchsichtig, das Ziel bekommt den Rahmen aus der ersten Hälfte.** Ein
Zug, dessen Quelle an ihrem alten Platz stehen bleibt, liest sich als Kopie.

**Das Beenden liegt an einer Stelle** (`finishDrag`), außerhalb der Widget-Schleife: ein Drop
betrifft zwei Elemente, die in zwei verschiedenen Widgets liegen können, und pro Widget würde
er zweimal oder von der falschen Seite enden.

**Was das `isInteractive` gekostet hat, und wo B2 und B7 sich treffen:** die Auswahl in einer
Liste hing an `hot`, also an dem, was den Druck GENOMMEN hat. Solange in einer Zeile nichts
reagierte, war das dieselbe Antwort wie „worauf wurde gedrückt". Eine ziehbare Zeile reagiert,
und damit war eine umsortierbare Liste eine, in der man nichts mehr auswählen konnte. Die
Auswahl läuft jetzt vom ROHEN Treffer nach oben zur umschließenden Liste, so wie die
Hover-Hervorhebung es längst tut. Gefunden, weil ein Test dafür geschrieben wurde, und der
erste Anlauf dieses Tests war selbst nicht unterscheidend: die Zeilenbeschriftung war
auto-sized und leer, also schrumpfte sie auf nichts und der Druck segelte an genau dem Ding
vorbei, um das es ging.

---

### Eine Animation umbenennen ist Retargeting, kein Textfeld (02.09.2026)

Der Hinweis am „New"-Knopf sagte: der Name ist die Identität, wer später umbenennt, muss jede
Play-Node selbst suchen. Der User: **das ist eine Aufgabe von automatischem Retargeting.**
Richtig, und beim Nachsehen war es schlimmer als der Hinweis behauptete: es gab **überhaupt
keine Umbenennung** für einen Clip. Der Hinweis warnte vor Kosten, die man über die
Oberfläche gar nicht auslösen konnte.

**Es ist derselbe Vorgang wie bei Funktion, Variable und Ereignis**, also gehört es in
dieselbe Maschinerie (`HcRename`) und nicht daneben. Damit kommen der projektweite Durchlauf
und der Dialog geschenkt. Nur die Form ist anders, und das ist der ganze Unterschied: die
anderen drei stehen als `Node::s` auf dem Knoten, ein Clipname steht **als Wert in einem Pin**
(dem `animation`-Parameter einer Engine-Zeile), entweder inline oder in einem Const String,
und seine Deklaration liegt gar nicht in einem Graphen, sondern in `UIWidgetTree::animations`.

**Welcher Parameter eine Animation benennt, fragt die Registry**, nicht eine Liste von vier
Zeilen-Ids hier. Dieselbe Regel wie bei `selfDefault`: die fünfte Zeile, die eine Animation
nimmt, wird sonst still vergessen.

**Ein Const String wird gezählt, nicht beurteilt.** Es gibt nur EINEN String: hängt derselbe
Literal an einer Animation und an einem Set Text, wäre das Umschreiben eine Umbenennung, die
Wörter auf dem Bildschirm ändert. Also wird umbenannt, wenn jede Leitung daraus auf einen
bewiesenen Animationspin geht, sonst gemeldet.

**Beweisbar fremd wird schweigend übersprungen, unbeweisbar wird gemeldet.** Genau der
Vertrag, den die Datei schon hatte, hier nur auf den Ziel-Pin angewandt: jedes Widget im
Projekt darf ein „Fade" haben.

**Bekannte Grenze, ausdrücklich nicht geraten:** ein `OnClipFinished`-Handler, der die
Nutzlast gegen ein Literal vergleicht, ist nicht retargetbar. Und ein Play auf einem Widget,
das der Graph gereicht bekam statt es selbst erzeugt zu haben, landet in der Melde-Liste
statt in der Umbenennung.

**Der Fehler dabei, und der ist es wert:** `ApiParam::name` ist ein `const char*`, mein
Vergleich war `p.name == "animation"`, also ein **Zeigervergleich**. Er compiliert
stillschweigend und ist genau so lange wahr, wie der Linker beide Literale auf dieselbe
Adresse legt. Ein Test war grün, drei waren rot, und die Erklärung war nicht die Logik,
sondern die Adresse. Deshalb steht dafür jetzt eine eigene Zusicherung im Test.

---

### B6: ein Label mit mehr als einer Stimme (02.09.2026)

Rich Text, also der Teil von B6, der übrig war: Umbruch und Ausrichtung gab es schon.

**Markup-String, kein strukturiertes Run-Modell.** Ein Skript erzeugt Text mit Set Property,
also muss alles, was ein Label sein kann, als String ausdrückbar sein, sonst wird
formatierter Text eine zweite API, die nur C++ erreicht. UMG, TextMeshPro und Slate sind aus
demselben Grund dort gelandet.

**Eine Regel für alles Kaputte: ein Tag, das nicht vollständig verstanden wird, IST Text.**
Unbekannter Name, kaputtes Hex, fehlender Wert, herrenloses `</>` — derselbe Fall. Die
Alternative ist eine Tabelle von Sonderfällen, die sich niemand merkt, und ein Label, das
still Zeichen verliert, die jemand sehen wollte. Das ist ein Dateiformat, also stehen die
Regeln als Tabelle im Test.

**`size` ist ein Skalar, keine Pixel.** Die Schriftgröße ist schon von Canvas und Auto-Size
skaliert; absolute Pixel würden sich mit beiden streiten.

**Ein Flag am Text, kein zwanzigster Elementtyp.** Ein Text ist rich oder nicht, und ein Flag
erspart Löschen-und-neu-Anlegen, wenn nachträglich ein Wort bunt werden soll. Aber:
**`interactive()` fragt das MARKUP, nicht das Flag.** Ein rich Label, das nur eine Farbe
enthält, darf die Klicks nicht schlucken, die dem Ding dahinter galten.

**Das Layout ist eine Funktion mit drei Verbrauchern**, zeichnen, messen, treffen. Dieselbe
Lehre wie bei `tabLayout`. Zwei Dinge, die der Klartextpfad nicht kennt: eine Zeile ist so
hoch wie ihr höchster Run, und alle Stücke einer Zeile teilen **eine** Grundlinie, sonst
schwebt ein großes Wort über seinen Nachbarn.

**Die Zusicherung, die den Rest trägt:** einfarbiger Text landet durch den neuen Weg
glyphengenau dort, wo der alte ihn hinlegt. Ohne sie verschiebt das Einschalten von Rich Text
jedes Label ein wenig, und das ist die Sorte Änderung, die später niemand mehr zuordnet.

**Der Zeiger sagt es, bevor der Klick es tut**, und zwar pro Link und nicht pro Element: die
Wörter neben einem Link sind nicht anklickbar und dürfen das nicht behaupten. Die Hand hängt
deshalb am Treffertest und nicht am Elementtyp, gleich neben der I-Beam-Regel des Textfelds.

**Zwei Grenzen, gemeldet statt umgangen:** der Atlas kennt ASCII 32..127, also sind
**Icon-Schriften aus B6 blockiert** (eigene Baustelle, sie leben in der Private Use Area weit
jenseits von 127), und es gibt keine zweite Schnittbreite im Atlas, also **kein `<b>`**, das
still nichts täte. Theme-Rollen als Farbe (`<color=accent>`) sind die offensichtliche v2,
deshalb ist der Farbslot im Parser ein String und kein aufgelöster Wert.

**Nicht gebaut:** auswählbarer statischer Text, der dritte Rest von B6. Das ist eine eigene
Scheibe, näher am Textfeld als am Label.

---

### Der Atlas kannte 96 Zeichen (03.09.2026)

Aus einer Nachfrage entstanden: was zu tun ist, um die oben gemeldeten Grenzen aufzuheben.
Beim Nachmessen war die Grenze größer als gemeldet.

**Die Icon-Grenze war die Umlaut-Grenze.** Alle vier Byte-Schleifen der Textpipeline
übersprangen Bytes ab 128. „Größe" wurde als „Gre" gezeichnet **und so gemessen**, also saß
auch alles daneben, was sich am Text ausrichtet. Und ein Passwortfeld zeichnete gar nichts:
sein Punkt ist U+2022, drei Bytes, keines davon ASCII, also null Glyphen, Breite null und ein
Cursor, der sich nicht bewegen kann. Beides gemessen, bevor es behauptet wurde, und beides
ohne Zusicherung, die es festhielt.

**Drei Fallen, an denen es beinahe gescheitert wäre.**

`stbtt_PackFontRanges` skaliert bei negativer Größe (`STBTT_POINT_SIZE`) über
`ScaleForMappingEmToPixels`, bei positiver über `ScaleForPixelHeight` — und letzteres ist,
was `stbtt_BakeFontBitmap` vorher benutzte. Die Zusicherung dahinter ist die gleiche wie bei
Rich Text: bestehende Labels dürfen sich nicht bewegen. Nachgewiesen, indem beide Fassungen
gebaut und die 95 ASCII-Metriken Zeichen für Zeichen verglichen wurden.

Der **Rückgabewert** von `PackFontRanges` taugt mit `skip_missing` nicht als „hat gepasst":
er meldet Fehlschlag für jedes Zeichen, das die *Schrift* nicht hat. „Roboto kennt kein
Kyrillisch" sah damit aus wie „der Atlas ist zu klein", und genau das ist zuerst passiert,
still, mit Rückfall auf ASCII. Deshalb Gather/Pack/Render von Hand: die Rechtecke
beantworten die einzige Frage, die zählt.

Ein Zeichen ohne Glyphe zeichnet **nichts**, statt das Kästchen der Schrift zu bekommen. Ein
Kästchen behauptet, das Zeichen sei da gewesen.

**Die Bereiche sind eine Projekteinstellung, keine Konstante.** Basis ist immer Latein, wie
es geschrieben wird: ASCII, Latin-1, Latin Extended-A, die Satzzeichen samt Punkt und
Gedankenstrich, das Eurozeichen. Griechisch und Kyrillisch kosten Fläche, also werden sie
gefragt (Preferences ▸ Project ▸ Fonts) und reisen im `.heproj` und in der `project.hcfg` in
die exportierte Anwendung, die ihren Atlas auf einer Maschine backt, die den Editor nie
gesehen hat. Zwei freie Bits im vorhandenen Flagwort, also bewegt sich kein Dateiformat.
Alles zusammen passt in dieselben 1024², in denen vorher 96 Zeichen lagen.

**Der Atlas wird einmal gebacken, und die Backends laden ihn einmal hoch.** Deshalb wird die
Maske beim Projektöffnen gesetzt und danach nicht mehr: `uiSetFontScripts` lehnt eine spätere
Änderung ab, statt sie halb anzuwenden, und die Seite macht aus diesem Nein einen Satz über
den Neustart. Eine Textur, deren Glyphen sich verschoben haben, während sechs Backends noch
die alte halten, wäre der teurere Weg zur selben Einstellung.

**Die UTF-8-Schritte wohnen jetzt neben den Glyphenschleifen** (`Renderer/UIFont.h`) und
nicht mehr im Element: der Cursor und die Glyphen müssen sich einig sein, wo ein Zeichen
anfängt, und zwei Kopien dieser Regel werden es irgendwann nicht mehr sein.

**Immer noch nicht gebaut:** Icon-Schriften (Private Use Area plus ein `<icon=name>`-Tag,
denn niemand tippt einen PUA-Codepoint in ein Textfeld) und `<b>` (die Standardschrift **ist**
Roboto Condensed Bold, es gibt nichts Fetteres; der Weg dahin ist ein `<font=asset>`-Tag pro
Run, der Icon-Schriften gleich mitbedient).

Nebenbei aufgeräumt: Collaboration, Source Control und Tools hatten im Einstellungs-Rail je
eine Gruppe mit genau einem Eintrag. Sie stehen jetzt unter **Editor**, und die
Beschriftungen erben, was die Überschrift getragen hat.

---

### Ein Schnitt und ein Icon sind dasselbe Problem (03.09.2026)

Die beiden übrigen Grenzen aus B6, `<b>` und Icon-Schriften. Die Frage des Users war, ob er
Schriften anschaffen muss: nein. **Roboto Condensed Regular** (SIL OFL 1.1) und **Material
Icons** (Apache 2.0), beide frei mitlieferbar, beide eingebettet, die Lizenztexte liegen
neben den Dateien. `scripts/embed_font.py` und `scripts/embed_icon_names.py` machen daraus
Header, damit die nächste Schrift ein Befehl ist und kein Nachmittag.

**Ein Face pro Run, nicht zwei Mechanismen.** `<b>fett</>` und `<icon=home>` heißen beide
„diese Zeichen kommen aus einer anderen Datei". Also trägt `UITextRun` ein Face, `UIRichPiece`
das aufgelöste, aufgelöst wird an EINER Stelle (`resolveFace`, von Messung UND Zeichnen
benutzt), und gestempelt wird pro Quad — was `UIRenderObject` immer schon konnte.

**Eine Zeile hat eine Grundlinie, auch mit drei Schriften darauf.** Sie rechnet sich weiter
aus dem Ascent der BASIS. Pro Run gerechnet säße ein fettes Wort einen Pixel neben seinen
Nachbarn, und das liest sich als Wackeln, nicht als Betonung.

**Fett ist nicht breiter.** In einer Condensed ist die fette Vorbreite von `H` kleiner als
die magere (33.52 gegen 33.87), obwohl die Zeile als Ganzes breiter wird. Die erste
Zusicherung maß die Vorbreite und war damit falsch. Gewicht misst man als Tinte (Deckung im
Atlas), und was das Layout beweisen muss, ist etwas anderes: dass ein Stück in SEINEM Face
gemessen wurde.

**`<b>` bleibt wirkungslos, solange die Basis Bold ist**, und die war es immer. Deshalb wählt
Preferences ▸ Project ▸ Fonts das Textgewicht, mit der Asymmetrie von `themeStyled`: fehlt
heißt Bold (kein bestehendes Projekt bewegt sich), ein NEUES Projekt wird mit Regular
angelegt. Ein Tag, das still nichts tut, wäre das gewesen, was hier eine Scheibe vorher
abgelehnt wurde.

**Icons haben ihren eigenen Atlas, und zwar faul.** 2234 Namen, 2188 Umrisse (46 Namen sind
Aliase), gebacken beim ERSTEN `<icon=…>` und dann ganz, bei 40 px in 2048² — 48 px passen
nicht, und ein Icon wird bei 16 bis 32 gezeichnet, also ist es ein Verkleinern statt eines
Verwischens. Ein Projekt ohne Icons zahlt nichts, und der Atlas wächst nach dem ersten Upload
nie, also gilt die Sechs-Backends-Regel weiter.

**`<icon=…>` ist das einzige Tag, das ein Zeichen EINFÜGT**, statt die folgenden zu färben.
Es erbt Farbe, Größe und Link von der Stelle, an der es steht, denn die Alternative wäre eine
Regel darüber, welche Eigenschaften ein Icon erreichen und welche nicht, und die merkt sich
niemand. Ein Name wird gegen die SCHRIFT geprüft, nicht nur gegen die Liste: sonst wäre ein
Tippfehler ein unsichtbares Zeichen statt des Textes, den jemand geschrieben hat.

**Das Gewicht gilt pro Prozess**, weil der Atlas einmal gebacken wird. Der Test dafür hat
deshalb eine eigene Datei, damit ctest ihm einen eigenen Prozess gibt. Erster Versuch mit
einem statischen Initialisierer hat die Bold-Tests der Nachbardatei umgelegt: der läuft auch
dann, wenn doctests Filter die Datei ausschließt.

**Damit ist B6 zu.** Offen bleibt aus dem Kapitel nur auswählbarer statischer Text, und der
ist näher am Textfeld als am Label.

---

### A7, erster Teil: die Anwendung bekommt ein Gesicht (03.09.2026)

Aus B6 fällt A7 billiger heraus, als es im Plan steht: **das App-Icon wird erzeugt, nicht
verlangt.** Ein Name aus der eingebauten Icon-Schrift plus eine Plattenfarbe, und der Export
schreibt daraus die drei Container, auf denen drei Systeme bestehen. Niemand zeichnet dasselbe
Bild dreimal, und ein Projekt hat ein Icon an dem Tag, an dem es angelegt wird — neue Projekte
bekommen `widgets` beziehungsweise `sports_esports` in die `.heproj` geschrieben.

**Ein PNG-Encoder, nicht drei Container-Encoder.** `.icns` und `.ico` tragen beide PNG-Nutzlast,
also gibt es genau eine Stelle, die Pixel in Bytes verwandelt, und zwei kurze Funktionen, die
Verzeichnisse drumherum bauen. Der Encoder ist `stb_image_write.h` (vendort, Public Domain):
die erste Fassung schrieb PNG mit STORED-Deflate von Hand, war korrekt, und produzierte 1,4 MB
statt 25 KB pro `.icns`. Ein Icon, das vierzigmal zu groß ist, reist in jede ausgelieferte
Anwendung mit.

**Die Falle beim zweiten Leser einer vendorten Bibliothek.** Der PNG-DECODER lag mit
`STB_IMAGE_STATIC` in `SplashScreen.cpp`, also dateilokal — mit dem Kommentar „nichts sonst in
HorizonCore hat eine Meinung zu stb_image". Jetzt hat etwas: das Fenster-Icon wird zur Laufzeit
aus derselben PNG gelesen. Statt einer zweiten Kopie des Decoders ist das `static` gefallen; die
Symbole bleiben innerhalb der Bibliothek und werden nicht exportiert.

**Und die Falle, die C++ still stellt:** ein `struct SDL_Window;` INNERHALB von `namespace HE`
deklariert `HE::SDL_Window` und damit einen anderen Typ als den, den SDL meint. Die
Vorwärtsdeklaration gehört auf globale Ebene, sonst findet der Linker die Funktion nicht, die
direkt darüber steht.

**Was das System sonst noch wissen muss**, ist jetzt auch einstellbar statt abgeleitet:
Bundle-Identifier (leer = `com.horizonengine.<projekt>` wie bisher) und Version wandern in die
`Info.plist`, `CFBundleIconFile` wird nur gesetzt, wenn die Datei wirklich daneben liegt — ein
Bundle, das auf ein fehlendes Icon zeigt, bekommt das generische UND lässt es cachen.

---

### A7, der Rest: Dateitypen, Öffnen mit, Tray, Autostart (03.09.2026)

**Anmelden und Empfangen gehören zusammen.** Einen Dateityp anzumelden, den man nicht
empfangen kann, ist eine tote Funktion; empfangen ohne anzumelden kann man nicht ausprobieren.
Deshalb ein Commit für beides.

Ein Dateityp ist Endung, Anzeigename, Icon. Die Namen, die die Systeme dafür wollen, sind
**abgeleitet und nie gefragt**: UTI, MIME-Typ und ProgId kommen alle aus Bundle-Id plus
Endung. Drei Namen, die sich widersprechen können, sind genau der Weg, auf dem eine Datei in
der falschen Anwendung aufgeht. Erlaubt sind nur Buchstaben und Ziffern, also das, was eine
UTI tragen kann, und abgelehnt wird dort, wo es getippt wird.

Beide Hälften der Plist müssen sein: nur `CFBundleDocumentTypes` wäre ein Verweis auf einen
Typ, von dem das System nie gehört hat. Die Gegenprobe ohne `UTExportedTypeDeclarations` ist
rot. Geprüft mit Apples eigenem `plutil`, das die erzeugte Datei liest und UTI und
Anzeigename zurückgibt, samt maskiertem `&`.

**„Öffnen mit" kommt durch dieselbe Tür wie ein Drop.** `OnFileDropped` am GameInstance, mit
derselben Freigabe des Pfades. Ein zweites Ereignis hätte denselben Handler zweimal
geschrieben, und ob ein Dokument per Doppelklick oder per Ziehen ankam, ist Sache des Systems.
Unter macOS kostet das nichts, dort macht SDL aus dem Open-Event schon ein Drop-Event.
Zugestellt wird im ERSTEN BILD, nicht in OnInit: vorher gibt es die GameInstance nicht.

**Tray.** `Show Tray Icon`, `Add Tray Item`, `Clear Tray Menu`, `Hide Tray Icon`, dazu
`OnTrayItem`. Ein Eintrag hat eine **Id und eine Beschriftung**, zurück kommt die Id: wer nach
der Beschriftung feuert, dessen Menü hört auf zu funktionieren, sobald es übersetzt wird. Ein
Klick kommt aus SDLs Ereignisschleife, also wartet er in einer Schlange und das Bild stellt
ihn zu, statt den Interpreter mitten im Bild erneut zu betreten. Und das Tray liegt neben
`g_host` in der .cpp, weil SDL dem Rückruf einen nackten `void*` gibt: eine `std::list` von
Ids bewegt sich nie, ein Vector schon.

**Autostart** liegt hinter „Run other programs". Nicht weil es jetzt ein Programm startet,
sondern weil es das SYSTEM bittet, bei jedem Login eines zu starten, und das ist mehr als
eines jetzt zu starten, nicht weniger. Lesen braucht keine Berechtigung, sonst lügt jede
Checkbox, die daran hängt. Auf macOS ein LaunchAgent, auf Linux ein `.desktop` unter
`~/.config/autostart`, auf Windows die Registry unter HKCU (blind geschrieben). Die
Zusicherung zeigt `HOME` auf ein Wegwerfverzeichnis und schaut nach, was dort landet.

Dafür trägt die `project.hcfg` jetzt die **Bundle-Id** (Format v5, Anhang wie die anderen):
die Laufzeit muss wissen, wie die Anwendung heißt, und ein Login-Eintrag unter einem anderen
Namen als dem des Bundles ist einer, den später niemand findet. Aufgelöst wird beim Export,
damit die Laufzeit nicht ein zweites Mal ableitet und zu einer anderen Antwort kommt.

**Damit ist A7 zu.** Was blind geschrieben ist, steht oben: die Windows- und Linux-Pfade für
Dateitypen und Autostart, wie seinerzeit der GL-Port.

---

### A6: die Menüleiste, gezeichnet (03.09.2026)

**Abweichung vom Plan, mit Grund:** der Plan wollte die Leiste als ASSET. Gebaut ist sie als
Laufzeit-API (`Add Menu`, `Add Menu Item`, `Add Menu Separator`, `Clear Menu Bar`), weil das
Ziel dahinter war, dass HorizonCode sie füllen kann statt sie fest zu verdrahten — und das tut
eine API direkt, während ein neuer Asset-Typ zuerst eine Editor-Oberfläche bräuchte, nach der
niemand gefragt hat. `HE::AppMenu` ist die Struktur, in die so ein Asset später laden würde.

**Und die zweite Abweichung:** gebaut ist zuerst die GEZEICHNETE Leiste, nicht die native.
Der Plan nennt macOS zuerst, aber die native ist auf dieser Maschine nicht zu sehen (eine
Anwendung mit Fenster lässt sich hier nicht anschauen), während die gezeichnete vollständig
headless prüfbar ist. Eine Funktion, die auf einer Plattform läuft, die ich nicht sehen kann,
und auf zwei anderen gar nicht, ist die schlechtere erste Hälfte. Die native ist danach eine
Aufwertung mit derselben API und derselben Datenstruktur.

**Der Grab-Stapel trägt sie mit.** Ein offenes Menü schiebt einen Grab mit `widget = 0` — und
weil `takesInput` gegen die Spitze des Stapels vergleicht und keine Widget-Id 0 ist, ist damit
alles darunter automatisch taub. Escape schließt es durch dieselbe Tür wie einen Dialog, ohne
eine Zeile extra.

**Das eine, was eine Leiste von einer Reihe Dropdowns unterscheidet:** mit gedrückter Taste
auf den nächsten Titel zu wandern schaltet das Menü um, ohne loszulassen, und es bleibt EIN
Grab. Genau dafür gibt es eine eigene Zusicherung, und ihre Gegenprobe ist rot.

**Sie überlagert die Leinwand, sie schrumpft sie nicht.** Schrumpfen hieße, `UIWidgetCanvas`
einen Ursprung zu geben, den jede Rechteck-Rechnung in HE_Core liest — eine größere Änderung
als die Leiste selbst, und eine, die man absichtlich macht und nicht nebenbei. Bis dahin frisst
der Streifen den Zeiger in seinem eigenen Band (eine Leiste, durch die man die Seite darunter
anklicken kann, wäre keine), und `menuBarHeight()` sagt, wie viel Platz eine Seite lassen muss.

**Ein Trennstrich ist kein Eintrag.** Loslassen darauf wählt nichts und schließt nichts: er hat
keine Id, und ein um drei Pixel danebengegangener Zielversuch soll das Menü nicht zumachen.

**Offen:** Tastenkürzel und `enabled`/`checked` an Einträgen. Die native macOS-Leiste ist der
Abschnitt direkt darunter.

---

### A6: dieselbe Leiste, nativ auf macOS (04.09.2026)

`HE_Game/src/AppMacMenu.mm`, ObjC++ und **nicht** in HE_Core: dessen Cocoa-Abhängigkeit zöge
AppKit in he_tests, hc_codegen, widget_gen und die Windows-CI. Der Editor hält seine
`MacMenuBar` aus demselben Grund in HE_Editor.

**Sie fügt ein, sie ersetzt nicht.** SDL baut auf macOS selbst eine Menüleiste (App-Menü mit
Über/Dienste/Ausblenden/Beenden, dazu Fenster mit Schließen/Minimieren/Zoom/Vollbild) und hängt
sie an `NSApp.mainMenu`. Ein `NSApp.mainMenu = eigenes` hätte das alles weggeworfen, und zwar
auch in jedem ausgelieferten **Spiel**, nicht nur in einer Anwendung, die eine Leiste bestellt
hat. Die Menüs der Anwendung gehen deshalb zwischen App-Menü und Fenster, wo die eigenen Menüs
eines Mac-Programms hingehören. Wer nie `Add Menu` aufruft, bekommt exakt die Leiste, die SDL
gebaut hat.

**Zwei Leisten wären nicht die doppelte Funktion.** Deshalb `setMenuBarNative(true)`: der
Streifen im Fenster wird nicht gezeichnet, `menuBarHeight()` ist 0, und das Band gehört wieder
der Seite. Die Daten bleiben dieselben — beide Leisten lesen denselben Vektor, die Anwendung
sagt ihre Menüs genau einmal.

**Ob es eine Systemleiste gibt, wird pro Frame gefragt und nicht beim Start.** Das ist SDLs
Antwort und sie stimmt erst, wenn SDL die Anwendung registriert hat; einmal zu früh gefragt,
zeichnet das Fenster den Streifen für den Rest des Laufs.

**Die Id reist auf dem Eintrag** (`representedObject`), nicht in einer Tabelle neben dem Menü:
eine Seitentabelle müsste im Gleichschritt mit jedem Neubau gepflegt werden, und was das
kostet, hat der Tray schon gezeigt. Der Klick kommt aus AppKits eigener Schleife und wartet
darum wie ein Tray-Klick auf den Frame, statt den Interpreter mittendrin zu betreten.

**Neu gebaut wird einmal pro Frame, nicht pro Aufruf.** Ein Graph, der ein Menü mit sechs
Einträgen aufbaut, fasst die Leiste siebenmal an; sechs davon wären NSMenu-Neubauten auf dem
Weg zum selben Ergebnis.

**Kein Tastenkürzel, mit Grund:** ein natives schluckt den Anschlag, bevor irgendetwas im
Fenster ihn sieht. Genau daran ist im Editor das ⌘Z des Material-Graphen gestorben, und
Kürzel sind ohnehin ein eigenes Stück Arbeit.

**Was hier NICHT verifiziert ist:** wie die Leiste aussieht. Diese Maschine kann kein Fenster
zeigen, geprüft ist also der lokale macOS-Build (die `.mm` kompiliert keine CI) plus der
headless Test über den Schalter, der entscheidet, ob das Fenster die Leiste auch noch zeichnet.

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
