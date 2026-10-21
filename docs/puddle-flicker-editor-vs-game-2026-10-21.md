# Pfützen-Flackern — Editor-Play vs. Spiel (Thema 234, Schritt 5)

## Kurzfassung

Der Editor-vs-Spiel-Vergleich aus Schritt 1 wurde nachgeholt: dieselbe Wasserszene
(`ReproCamera`, `C:\hw234\HEProject`), derselbe Backend (D3D11, GI aus), dieselbe
24-Frame-Burst-Methodik (`PrintWindow`, 66 ms Abstand) wie Schritt 1-3 — zweimal im
Editor-**Play**-Modus, zweimal im Shipping-Export, ein Negativ-Kontrolllauf
(`driftfrozen`) zur Validierung der Messmethode, und ein gezielter Test, ob das Wasser
auch im normalen **Edit**-Modus (nie Play gedrückt) animiert.

**Befund: Das Wassermaterial selbst animiert immer gleich, unabhängig von
Editor/Spiel und von Play/Edit — ob das auch beim NORMALEN Hinsehen im
Editor-Viewport ankommt, ist offen (siehe Punkt 2).**

1. **Editor-Play vs. Spiel, gleiche Größenordnung.** Mit einer je Lauf selbst
   abgeleiteten Wassermaske (kein Resize, keine Masken-Wiederverwendung über
   Auflösungen hinweg) liegt das mittlere See-Δ im Editor-Play bei 1.08-1.12, im
   Spiel bei 0.95-0.97 (je zwei unabhängige Läufe). Ein eingefrorener
   Negativ-Kontrolllauf (`driftfrozen`, Schritt 3) liegt bei 0.22 — 4-5× darunter —
   und bestätigt, dass diese Metrik echtes Wellen-Signal von Rauschen trennt. Editor
   und Spiel liegen nah beieinander (Faktor ~1.1-1.2×, in Richtung Editor); angesichts
   eines unten dokumentierten Boden-Unterschieds unter dem Wasser lässt sich diese
   kleine Differenz NICHT sicher als "Editor flackert mehr" lesen (siehe
   "Geprüfte Rahmenbedingungen", Punkt 5) — die robuste Aussage ist: **keine
   Größenordnung Unterschied, kein 4-7×-Gefälle.**
2. **Auch im reinen Edit-Modus (nie Play gedrückt) animiert das Wasser.** Eine erste
   Fassung dieses Reports behauptete das Gegenteil, gestützt auf den Code-Fund, dass
   `EditorApplication.cpp:3243-3250` `HE::api::time::advance()` nur innerhalb von
   `if (m_isPlaying)` aufruft. **Das war ein Fehlschluss** — widerlegt per Gegenprobe
   (Abschnitt "Edit-Modus-Test" unten): zwei `scene_screenshot`-Aufnahmen derselben
   Szene, 1.5 s auseinander, NIE im Play-Modus, zeigen ein deutliches See-Δ (mean
   6.88 über eine 150 091-Px-Maske), während ein gleich großer Terrain-Streifen in
   denselben Bildzeilen exakt 0 Δ zeigt (schließt allgemeines Render-Rauschen aus).
   Eine zweite Gegenprobe (derselbe 1.52-s-Abstand in `driftfrozen`, wo nur die
   Wellen eingefroren sind, der Himmel aber normal läuft: Δ=1.20) schließt auch aus,
   dass das gemessene Δ allein vom gespiegelten Sternenhimmel kommt — 6.88 liegt
   nahe bei "alles läuft normal" (`control_refresh`, Δ=7.23 über dieselbe Spanne),
   weit über "nur Himmel" (1.20). Der tatsächliche Grund: das Wassermaterial bezieht
   seine Zeit NICHT aus `HE::api::time::elapsed()` (das IST `m_isPlaying`-gegated),
   sondern aus `SDL_GetTicks()` — direkt im Quelltext als *"Engine seconds for the
   node graph's Time input"* kommentiert (`D3D11Renderer.cpp:6717-6722`), mit
   demselben Fallback-Muster in allen vier geprüften Backends — eine reale Wanduhr,
   die läuft, sobald der Prozess läuft, unabhängig von Play/Edit.

**Fazit:** Dieser Schritt findet keine Ursache auf Material-/Zeit-Ebene — die
Wellen-Uhr läuft immer und überall gleich. Ob der Mensch das im normalen
Edit-Modus-Viewport überhaupt zu sehen bekommt (Redraw-Takt, Punkt 2), ist NICHT
geklärt und der naheliegendste offene Kandidat für "nur im Spiel, nicht im
Editor" — siehe "Nächster Schritt".

## Verworfene Zwischenstände dieses Reports

Drei aufeinanderfolgende Fassungen dieses Dokuments kamen zu unterschiedlichen,
jeweils falschen Schlüssen, bevor der obige Stand sich als haltbar erwies. Alle drei
sind dokumentiert (nicht gelöscht), damit künftige Läufe dieselben Fallen nicht
wiederholen:

1. *"Editor 4.4× schwächer als Spiel."* Eine Maske aus Schritt 2 (3892 Px, aus dem
   Spiel abgeleitet) wurde auf den Editor-Crop angewendet, nachdem dieser
   nicht-uniform von 1208×645 auf 1200×675 skaliert wurde. Fehler: eine ortsfeste,
   aus einem Lauf gewonnene Maske trifft nach einer verzerrenden Größenänderung
   andere, ruhigere Pixel.
2. *"Beide ~8.0, identisch."* Eine je Lauf selbst abgeleitete, aber auf
   `diff_accum > 3` geschwellte ("Hotspot") Maske. Fehler: diese Metrik liefert für
   JEDEN Lauf ungefähr denselben Wert — auch für den komplett eingefrorenen
   Negativ-Kontrolllauf (`driftfrozen`, dort mean 8.78 trotz eines tatsächlichen
   Δ von 0.22 bei der validen Metrik). Die Schwelle wählt die "lautesten" Pixel
   JEDES Laufs aus und mittelt praktisch immer in dieselbe Gegend, unabhängig vom
   realen Signal.
3. *"Edit-Modus ist statisch, Play/Spiel sind gleich — das erklärt alles."* Der
   `m_isPlaying`-Gate um `HE::api::time::advance()` wurde korrekt gelesen, aber
   nie empirisch geprüft, ob das Wassermaterial diese Quelle überhaupt konsumiert.
   Tut es nicht — siehe Kurzfassung Punkt 2 und "Edit-Modus-Test" unten.

## Methodik — Burst-Vergleich (Editor-Play vs. Spiel)

- Gleicher Repro-Aufbau wie Schritt 1-4: `C:\hw234\HEProject`, `ReproCamera` vor dem
  gemalten `WaterSurface`-See, D3D11, `GlobalIlluminationEnabled=false`, VSync an
  (beide Seiten geprüft, siehe unten).
- **Spiel:** `C:\hw234\shots\control_refresh` (Schritt 2-4) und
  `C:\hw234\shots\d3d11_gi0b` (Schritt 1), je 1200×675, 24 Frames. **Negativ-
  Kontrolle:** `C:\hw234\shots\driftfrozen` (Schritt 3, Zeit auf 0 eingefroren).
- **Editor-Play:** neues Skript `C:\hw234\burst_capture_editor.ps1`, zweimal
  ausgeführt (`editor_play`, `editor_play2`, unabhängige Prozess-Starts). Startet die
  private Editor-Kopie (`C:\hw234\deploycopy\Editor\HorizonEditor.exe`, `HE_MCP=1`,
  eigenes `APPDATA`), wartet auf den MCP-Endpoint als Lade-Signal, klickt den
  Play-Knopf der Toolbar per `SendInput` (Koordinaten aus einem Screenshot vermessen
  — der Tastatur-Shortcut Ctrl+P erreichte trotz funktionierendem Fokus-Workaround
  nie `ImGui::IsKeyPressed`, siehe "Automatisierungs-Fallstrick"), bestätigt den
  Play-Modus über die Log-Zeile "EditorApplication: entering play mode"
  (`EditorApplication.cpp:13519`), wartet 25 s Settle, dann 24 Frames × 66 ms
  PrintWindow. Fenster 2000×1125 (volles Editor-Fenster); für die Messung auf den
  reinen Viewport-Inhalt zugeschnitten (`(363,95)-(1571,740)` → 1208×645, deckt sich
  mit der im Statusbalken angezeigten Viewport-Auflösung "1211×650").
- **Maske — "whole-water":** `C:\hw234\masked_diff_self.py`. Pro Verzeichnis: eine
  Wasser-Farbmaske (`b > r+15 and b > g+5 and b < 200`, unterhalb der Horizontzeile —
  170 für 1200×675-Spiel-Crops, 190 für den 1208×645-Editor-Crop, je an der eigenen
  Auflösung bestimmt) grenzt den gesamten sichtbaren See vom Himmel ab; gemessen wird
  das Δ über die GANZE Seefläche, kein zusätzlicher Schwellenwert, kein Resize.

## Messung — Burst-Vergleich (whole-water, D3D11, GI aus, 23 Frame-Paare je Lauf)

| Lauf | water_mask_px | mean \|Δ\| | Bereich | geänderte Px (>10) | Hot-Anteil |
|---|---|---|---|---|---|
| Spiel, `control_refresh` (frisch, dieser Schritt) | 110 577 | 0.953 | 0.768–1.105 | 904.7 | 0.82 % |
| Spiel, `d3d11_gi0b` (Schritt 1) | 110 571 | 0.972 | 0.776–1.148 | 910.7 | 0.82 % |
| Editor-Play, Lauf 1 | 123 019 | 1.120 | 0.621–2.677 | 1457.1 | 1.18 % |
| Editor-Play, Lauf 2 | 122 990 | 1.079 | 0.509–2.722 | 1417.0 | 1.15 % |
| Negativ-Kontrolle `driftfrozen` (Zeit=0, Schritt 3) | 159 835 | 0.219 | 0.183–0.237 | 400.2 | 0.25 % |

Spiel: 0.95–0.97. Editor-Play: 1.08–1.12. Eingefroren: 0.22. **Keine Größenordnung
Unterschied zwischen Editor-Play und Spiel**, beide klar über dem Rauschniveau der
Negativ-Kontrolle.

Obere Spannweite der Editor-Läufe (2.68/2.72) kommt von einem einmaligen,
Vollbild-weiten Sprung je Lauf (siehe "Zusatzbefund"); ohne dieses eine Frame-Paar
sinkt der Editor-Mittelwert auf 1.06-1.08, ändert die Einordnung nicht.

## Edit-Modus-Test (die eigentliche Antwort auf "nur im Spiel, nicht im Editor")

**Aufbau:** `C:\hw234\launch_editor_hidden.ps1` startet die private Editor-Kopie
komplett unsichtbar (`HE_HIDDEN_WINDOW=1`, `HE_MCP=1`) und rührt NIE den Play-Knopf
an. `C:\hw234\edit_mode_check.py` ruft über die MCP-Brücke (`he_mcp.py`) zweimal das
Werkzeug `scene_screenshot` auf (1.5 s Abstand), mit `position`/`look_at` exakt wie
`ReproCamera` (`[187.9966, 22.0, 628.0]` → `[187.9966, 0.0, 568.3793]`). Wichtig:
`scene_screenshot` rendert laut Quelltext-Kommentar
(`McpToolsScreenshot.cpp:26-34`) von einer eigenen Client-Kamera, unabhängig vom
Viewport und unabhängig vom Play/Edit-Zustand — genau das macht den Test möglich,
ohne die (als unzuverlässig erkannte) Tastatur/Maus-Automatisierung zu brauchen.

**Ergebnis** (1200×675, derselbe Seeausschnitt wie oben, aber eine andere
Farb-/Belichtungs-Pipeline als der normale Viewport — sichtbar an der kühleren,
bläulicheren Gesamtfarbe des Bilds, Ursache nicht untersucht):

| Bereich | Δ zwischen zwei Aufnahmen, 1.5 s auseinander |
|---|---|
| Terrain-Streifen, gleiche Bildzeilen wie der See, außerhalb der Seefläche (47 700 Px) | **mean 0.0, max 0, 0 geänderte Px** — bit-identisch |
| See (Farb- + Box-Maske, 150 091 Px) | **mean 6.88, max 253, 33 093 geänderte Px (22 %)** |

Das Terrain ist zwischen den zwei Aufnahmen exakt bit-identisch — das schließt
allgemeines Render-Rauschen (Dithering, TAA-Jitter o. Ä.) als Erklärung aus. Der See
verändert sich deutlich. **Das Wassermaterial animiert also auch dann, wenn nie auf
Play gedrückt wurde.**

**Gegenprobe — Spiegel-Konfundierung ausgeschlossen.** Schritt 1/3 zeigten, dass der
See Sterne spiegelt ("sparse glints"), und Sternenfunkeln + Wolkendrift laufen über
eine ANDERE Uhr (siehe unten) und sind nicht an die Wellen selbst gebunden. Ein Δ von
6.88 über 1.5 s könnte also allein vom bewegten Sternenhimmel-Spiegelbild kommen, bei
stillstehenden Wellen. Gegenprobe mit vorhandenen Daten: `f000` gegen `f023` (23
Frames × 66 ms ≈ 1.52 s, exakt die Lücke des Edit-Modus-Tests) in zwei Spiel-Läufen,
mit derselben whole-water-Maske:

| Lauf | mean \|Δ\| über 1.52 s |
|---|---|
| `driftfrozen` (Wellen bei Schritt 3 eingefroren, Himmel läuft normal weiter) | 1.20 |
| `control_refresh` (beides läuft) | 7.23 |

Reiner Spiegel-Effekt (Wellen eingefroren) liegt bei 1.20 — der Edit-Modus-Wert (6.88)
liegt nahe bei "beides läuft" (7.23) und weit über "nur Himmel" (1.20). Das
Edit-Modus-Δ kommt also überwiegend von den Wellen selbst, nicht vom
Sternenhimmel-Spiegelbild allein (die unterschiedliche Rendering-Pipeline von
`scene_screenshot`, siehe oben, erlaubt nur einen Verhältnis-, keinen Zahl-Vergleich
— aber das Verhältnis ist eindeutig).

**Warum:** `HE::api::time::advance()` wird tatsächlich nur bei `m_isPlaying`
aufgerufen (`EditorApplication.cpp:3243-3250`, Kommentar an `:14559-14562` bestätigt
das als Absicht) — aber das Wassermaterial liest seine Zeit nicht von dort. Direkt im
Quelltext gefunden, nicht nur vermutet: `D3D11Renderer.cpp:6717-6722`,
`lit.sunDir[3]`, mit dem Kommentar *"Engine seconds for the node graph's Time
input"* — also exakt der Wert, den die Material-Graph-Knoten (einschließlich
`waveTrain()`s `time`-Pin) als Zeit sehen:

```cpp
// D3D11Renderer.cpp:6717-6722
// Engine seconds for the node graph's Time input (HE_SKY_TIME pins it for
// deterministic headless captures, mirroring the sky clock + GL/D3D12/Vulkan exactly).
static const char* s_timeOv = std::getenv("HE_SKY_TIME");
lit.sunDir[3] = (s_timeOv && *s_timeOv)
    ? static_cast<float>(std::atof(s_timeOv))
    : static_cast<float>(SDL_GetTicks()) / 1000.0f;
```

Dasselbe `lit.sunDir[3]`/`matLight.sunDir[3]`-Feld mit demselben
`SDL_GetTicks()/1000.0f`-Fallback (wenn `HE_SKY_TIME` nicht gesetzt ist) taucht in
`D3D12Renderer.cpp:10739`, `OpenGLRenderer.cpp:8023` und `VulkanRenderer.cpp:8686`
auf — konsistent über vier der fünf Backends. `MetalRenderer.mm:13508` setzt
dasselbe Feld ebenfalls aus `SDL_GetTicks()`, und `MetalRenderer.mm:14929` trägt
exakt denselben Kommentar wie D3D11 ("engine seconds — the node graph's Time
input") an einer zweiten Stelle (`skyClock`, selbst aus `SDL_GetTicks()`
gespeist) — Metal ist also ebenfalls konsistent, nur über eine zusätzliche
Zwischenvariable.
`SDL_GetTicks()` ist die Millisekunden seit Prozessstart, läuft unabhängig von
`HE::api::time`, von `m_isPlaying`, vom GameLoop — und (nicht in diesem Schritt
verifiziert, aber naheliegend) vermutlich genau das, was Schritt 3 mit
"`SDL_GetTicks()/1000.0f` als float32 verliert bei großer Laufzeit an Präzision"
meinte, als über Langzeit-Verhalten spekuliert wurde.

**Nicht verifiziert:** der exakte Pfad von `lit.sunDir[3]` bis zum konkreten
`time`-Pin in `waveTrain()` (`src/HE_Tools/src/MatGen/main.cpp:387`, über die
Material-Konstantenpuffer-Bindung) wurde nicht Zeile für Zeile verfolgt — der
Kommentar an der Quelle ("the node graph's Time input") plus die empirische
Gegenprobe (Terrain bit-identisch, See bewegt sich stark, kein Play-Klick, Verhältnis
schließt reine Spiegel-Konfundierung aus) sind zusammen aber ein belastbarer Beleg,
dass die Materialzeit ungegated läuft.

**Offene Frage, NICHT beantwortet: redrawt das sichtbare Viewport-Fenster im
Edit-Modus überhaupt kontinuierlich?** `scene_screenshot` erzwingt pro Aufruf einen
frischen Render — das sagt nichts darüber, ob der NORMALE Viewport (was ein Mensch
tatsächlich vor sich hat) genauso oft neu zeichnet oder nur bei Eingaben
(Mauslook, Tastatur) bzw. in einem reduzierten Leerlauf-Takt. Ein Versuch, das mit
PrintWindow-Bursts auf dem echten Viewport zu prüfen (`-Play 0` in
`burst_capture_editor.ps1`, Kamera per `EditorCamPosX/Y/Z`/`EditorCamYaw`/
`EditorCamPitch`-Config-Werte auf die `ReproCamera`-Pose geparkt), zeigte eine
Fehlkonfiguration: `m_editorCamera.restoreView(pos, yaw, pitch, pivotDistance)`
(`EditorApplication.cpp:1721-1725`) nimmt zusätzlich eine `EditorCamPivot`-Distanz
entgegen, die nicht gesetzt wurde — das Ergebnis war eine fast senkrechte
Vogelperspektive auf das Terrain, der See nicht im Bild (per Screenshot
verifiziert). Die auf dieser fehlerhaften Kamera gemessenen "0 Δ"-Zahlen sind damit
NICHT aussagekräftig (kein See im Bild heißt: kein Flackern zu sehen, unabhängig
vom Redraw-Takt) und wurden deshalb aus diesem Report entfernt. Diese Frage bleibt
offen — siehe "Nächster Schritt".

## Zusatzbefund: ein periodischer Vollbild-Sprung, nur im Editor-Play

In BEIDEN Editor-Play-Läufen (nicht im Spiel) tritt nahe dem Ende des
1.58-s-Burst-Fensters ein einmaliger, das GESAMTE Viewport betreffender Sprung auf
(Lauf 1: Frame 21→22, mean=1.06 über den ganzen Crop, 57353 von 779160 Px = 7.4 %;
Lauf 2: Frame 22→23, mean=1.065, 57763 Px). Zwei Frames nebeneinander sehen für das
Auge nahezu identisch aus. Das Editor-Log zeigt dazu keine Zeile in diesem
Zeitfenster — Ursache nicht ermittelt, am ehesten ein periodischer UI-/Viewport-
Refresh-Tick (`ContentBrowserRefreshRate`, `EditorApplication.cpp:3437-3440`) oder
ein TAA/SSAO-Temporal-Reset. Klar vom See-Flackern getrennt (betrifft das ganze
Bild) und ändert die Einordnung oben nicht (mit/ohne diesen Frame). Außerhalb der
Limits dieses Schritts, als Spur für ein künftiges "warum rendert der Editor-
Viewport periodisch anders" vermerkt.

## Geprüfte Rahmenbedingungen

1. **Shipping- vs. Dev-Build-Defines (`HE_SHIPPING`).** Existiert nicht:
   `docs/export-sdk-compile-defs-2026-10-18.md` (Thema 211) — 0 Treffer für
   `HE_SHIPPING`/`HE_DEV`/`isShipping`. `HE_BUILD_PROFILE_DEV/RELEASE` gated laut
   Thema 211 Schritt 3 nur Dev-Hotkeys, Profiler, Test-Hooks, Env-Overrides — kein
   `#if HE_FEATURE_*` um `waveTrain()`, den `time`-Namespace oder die
   `SDL_GetTicks()`-Aufrufe oben gefunden.
2. **VSync/Frame-Pacing.** Beide Seiten VSync an (Editor-Quick-Settings-Checkbox
   angehakt, Spiel-`config.json` `GameVSync: true`).
3. **Post-Processing/AA.** Editor-Default (`EditorConfig.h:193-194`,
   `SpecularAA=true, SpecularAAStrength=1.0`) deckt sich mit dem Export-Wert.
   Bloom/SSAO-Werte identisch.
4. **Wassermaterial-Identität.** `md5sum` von
   `deploycopy\Editor\EngineContent\Materials\Water.hasset` (`934c2a7b...`) stimmt
   exakt mit `C:\hw234\variants\control\Water.hasset` überein.
5. **Landscape-Textur-Bett ist NICHT identisch — macht den kleinen Editor/Spiel-
   Unterschied in der Burst-Tabelle nicht auswertbar.** Das Spiel rendert ein
   auffällig flaches, helles Terrain (RGB-Mittel `(176,182,158)`, σ≈27 — Noise-
   Blotches statt erkennbarer Hex-Textur); der Editor zeigt ein erkennbar
   texturiertes, dunkleres Terrain (`(52,49,40)`, σ≈8). Das Spiel-Log zeigt dazu
   KEINE `Cannot load asset …Landscape…`-Fehler, anders als der Editor beim ersten
   Laden — das heißt aber nicht zwingend, dass das Spiel die echte Textur lädt
   (vgl. Lesson "Exported game captures", Thema 190 Schritt 6: ein gepacktes Spiel
   ohne die `Engine/Textures/Landscape`-Arrays fällt still auf einen Fallback
   zurück, ohne dieselbe Fehlermeldung wie der Editor zu loggen). Das Wasser ist
   transparent und bricht den sichtbaren Boden mit — ein dunkler, hochfrequenter
   Hex-Boden durch bewegte Refraktion kann allein schon mehr Δ erzeugen als ein
   glatter, heller Boden, unabhängig von der Wellen-Animation selbst. Die
   10-20-%-Differenz zwischen Editor- und Spiel-Burst (Punkt 1 oben) lässt sich
   deshalb NICHT als "Editor flackert mehr" lesen — nur als "keine Größenordnung
   Unterschied" ist die Zahl belastbar.

## Automatisierungs-Fallstrick (für künftige Editor-Automatisierung)

- `SetForegroundWindow` von einem Hintergrund-PowerShell-Prozess aus liefert `false`
  (Windows-Fokus-Diebstahl-Schutz). Workaround: `AttachThreadInput` an den
  Vordergrund-Thread UND den Ziel-Thread vor `SetForegroundWindow`, danach wieder
  lösen.
- Maus-Klicks per `SendInput` erreichen nach diesem Trick zuverlässig ImGui
  (verifiziert: ein Klick auf die Viewport-Mitte selektierte die Terrain-Entity).
  Tastatur-Kombinationen per `SendInput` (`Ctrl+P`, `play.toggle`,
  `EditorShortcuts.cpp:54`) erreichten trotz erfolgreichem `ForceForeground`
  **nicht** `ImGui::IsKeyPressed`, über mehrere Versuche und Timing-Varianten.
  `PostMessage` (ohne Fokus-Trick) kam für Tastatur UND Maus ebenfalls nicht an.
  **Funktionierender Workaround:** den Play-Knopf der Toolbar direkt per
  `SendInput`-Mausklick treffen (Koordinaten aus einem Screenshot vermessen,
  `(942, 77)` bei 2000×1125 Fensterclient, proportional skaliert).
- Play-Start zeigt denselben Development-Splash-Intro wie ein Development-Export —
  Settle-Zeit muss ~25 s betragen, nicht die im Spiel-Skript üblichen 9 s.
- Der zuerst gegriffene `MainWindowHandle` ist während der Startphase das
  520×268-Splash-Fenster — `GetClientRect` darauf liefert nach dessen Schließen
  0×0 (von `PrintWindow`/`Capture()` auf 1×1 geklemmt). Workaround:
  `MainWindowHandle` nach dem MCP-Endpoint-Signal erneut abfragen, bis die
  Client-Größe > 400×400 ist.
- **Für reine Zeit-/Render-Fragen (kein UI-Interaktionstest nötig) ist
  `scene_screenshot` über `HE_HIDDEN_WINDOW=1` + `HE_MCP=1` deutlich robuster** als
  die PrintWindow/SendInput-Route: kein Fokus-Trick, kein Play-Klick, kein
  sichtbares Fenster nötig — die Kamera ist laut `McpToolsScreenshot.cpp:26-34`
  ohnehin eine eigene Client-Kamera, unabhängig vom Viewport.
- **Eine Maske, die aus einem Lauf gewonnen wurde, nicht unverändert (schon gar
  nicht über eine Größenänderung) auf einen Lauf mit anderer nativer Auflösung
  anwenden**, UND **eine geschwellte "Hotspot"-Maske ist keine Signal-Metrik** —
  gegen eine bekannte Negativ-Kontrolle (eingefrorene Zeit) prüfen, bevor man ihr
  traut.
- **Ein Code-Fund wie "X wird nur bei Bedingung Y aufgerufen" beweist nicht, dass
  Y die einzige Quelle für das beobachtete Verhalten ist** — der Materialgraph las
  in diesem Fall eine ANDERE Uhr (`SDL_GetTicks()` im Renderer) als die, die der
  Code-Fund nahelegte (`HE::api::time`, `m_isPlaying`-gegated). Nur eine
  Gegenprobe (hier: `scene_screenshot` im reinen Edit-Modus) deckte das auf.

## Nächster Schritt (Empfehlung)

- **Priorität 1 — die offene Frage aus "Edit-Modus-Test" klären:** redrawt der
  sichtbare Editor-Viewport im Edit-Modus bei stehender Kamera kontinuierlich, oder
  nur bei Eingaben/in einem reduzierten Takt? `burst_capture_editor.ps1 -Play 0`
  ist startklar, braucht nur eine korrekt geparkte Kamera — `EditorCamPivot` (nicht
  nur Pos/Yaw/Pitch) mit setzen, oder stattdessen die `ReproCamera`-Entity im
  Outliner selektieren und "Frame Selected" (Taste F) per Klick auslösen, dann erst
  den Burst starten. Falls der Viewport dabei NICHT redrawt: das ist eine
  einfache, vollständige Erklärung für "nur im Spiel, nicht im Editor" (Material
  animiert, Bildschirm zeigt es nur nicht an, solange niemand die Kamera bewegt).
  Falls er doch redrawt: die Beobachtung bleibt offen, dann zurück zu Priorität 2.
- **Priorität 2 — falls Priorität 1 nichts klärt:** die ursprüngliche
  Themen-Frage ("wird das Flackern als Bug oder beabsichtigtes Glint-Verhalten
  eingestuft?", Hive-Request #147, seit Schritt 2 offen) und direkt den Menschen
  fragen, unter welchen Umständen genau er "nur im Spiel, nicht im Editor"
  beobachtet hat.
- Falls relevant: den genauen Pfad von `lit.sunDir[3]`/`SDL_GetTicks()` bis zum
  `time`-Pin in `MatGen/main.cpp:387` nachverfolgen (nicht in diesem Schritt
  gemacht, siehe "Edit-Modus-Test").
- Der einmalige Vollbild-Sprung im Editor-Play (Abschnitt "Zusatzbefund") ist eine
  Spur wert, falls künftig AUCH im Editor-Viewport-Rendering Flacker-Reports
  auftauchen — aber nicht Teil dieses Bugs.

## Handoff

- **Topic/Schritt:** Thema 234, Schritt 5 von 5 (Stand des Boards zu Beginn dieses
  Laufs).
- **Branch/Commit:** `claude/bug-pf-tzen-auf-landscape-flackern-im-spiel-bisher-nur-auf-w`,
  dieser Report. `C:\hw234\burst_capture_editor.ps1`,
  `C:\hw234\launch_editor_hidden.ps1`, `C:\hw234\edit_mode_check.py` und
  `C:\hw234\masked_diff_self.py` liegen außerhalb des Repos (Repro-Kit wie in den
  Vorschritten, nicht committed).
- **Verifiziert:** vier unabhängige Burst-Läufe (2× Spiel, 2× Editor-Play),
  whole-water-Metrik gegen eine bekannte Negativ-Kontrolle validiert
  (`driftfrozen`), der Edit-Modus-Test per MCP `scene_screenshot` (Terrain
  bit-identisch, See bewegt sich deutlich, kein Play-Klick) UND gegen eine zweite
  Negativ-Kontrolle abgesichert (`driftfrozen` vs. `control_refresh` über dieselbe
  1.52-s-Spanne, schließt reine Sternenspiegel-Konfundierung aus), `lit.sunDir[3]`
  = "the node graph's Time input" als Code-Fund in allen fünf Backends,
  Wassermaterial-Identität (md5), VSync/Post-FX-Parität.
- **Nicht verifiziert:** ob der sichtbare Editor-Viewport im Edit-Modus bei
  stehender Kamera kontinuierlich redrawt (der Versuch dazu scheiterte an einer
  falsch geparkten Kamera — fehlendes `EditorCamPivot`, siehe "Edit-Modus-Test",
  letzter Absatz — und wurde nicht wiederholt); der exakte Datenpfad von
  `lit.sunDir[3]`/`SDL_GetTicks()` bis zum Material-`time`-Pin; die Ursache des
  periodischen Vollbild-Sprungs im Editor-Play; ob das Spiel die echten
  Landscape-Texturen lädt oder einen stillen Fallback zeigt.
- **Korrektur-Historie:** drei frühere Zwischenstände dieses Reports sind
  zurückgezogen — siehe "Verworfene Zwischenstände dieses Reports". Nur Kurzfassung
  + die beiden Mess-Abschnitte oben gelten.
- **Pending:** Neuplanung durch die Queen basierend auf diesem Befund — kein Fix in
  diesem Schritt. Keine belastbare technische Ursache auf Material-/Zeit-Ebene für
  "nur im Spiel" gefunden; ob es am Editor-Viewport-Redraw-Takt liegt, ist die
  offene, nicht beantwortete Frage für den nächsten Schritt (Priorität 1 oben).
