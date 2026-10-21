# Pfützen-Flackern im Spiel — Untersuchung (Thema 234, Schritt 1)

## Kurzfassung

Das Flackern lässt sich reproduzieren: eine gemalte Wasserfläche (WaterSurface/WaterMesh,
ein kleiner Teich — visuell genau das, was man als "Pfütze" bezeichnen würde) zeigt bei
VOLLSTÄNDIG STATISCHER Kamera eine messbare Pixel-Änderung von Frame zu Frame, auf allen
vier getesteten Windows-Backends (D3D11, D3D12, Vulkan, OpenGL) in praktisch identischer
Stärke. **GlobalIlluminationEnabled an/aus ändert daran nichts** (getestet auf D3D12 und
OpenGL). **Thema 191 (GI-Reflektionen) ist auf diesem Branch gar nicht vorhanden** — siehe
unten — kommt also als Ursache nicht in Frage. Das wetterabhängige Pfützen-Overlay auf
trockenem Boden (AutoLandscapeMaterial, Thema 180 Schritt 8) zeigt in denselben Aufnahmen
KEIN messbares Flackern. Verdächtigster Mechanismus: die zeitanimierte Chop/Wellen-Normale
des Wassermaterials (Thema 206), die Spiegelungen von Punktlichtquellen (hier: Sterne bei
Nacht) jeden Frame anders bricht — ein klassisches "Glitzer"-Artefakt, keine GI-Sache.

## Vorab-Korrektur der Rahmung

1. **Thema 191 ist nicht in release/0.7.0 gemerged.** PR #183
   (`claude/global-illumination-reflections` → `release/0.7.0`) ist offen (state OPEN,
   mergedAt null, mergeStateStatus CLEAN). Auf diesem Branch setzen NUR Metal
   (`MetalRenderer.mm:18072`, fest `true`) und OpenGL (`OpenGLRenderer.cpp:13564`,
   `m_giSupported`) `Capabilities::supportsGIReflections`. D3D11/D3D12/Vulkan setzen das
   Feld in diesem Branch überhaupt nicht (bleibt `false`); D3D11 wird in Thema 191 Schritt 4
   zusätzlich hart deaktiviert (Perf-Regression, +22ms/Frame). Falls der Mensch einen
   release/0.7.0-Export auf einem D3D-Backend oder Vulkan gespielt hat, kann Thema 191
   nicht die Ursache sein — der Codepfad existiert dort nicht.
2. **"Pfützen" ist im Code zweierlei:** das wetterabhängige Overlay des
   Auto-Landscape-Materials (`kAutoLandscapeParamPuddleAmount`, Weather-Panel "Puddles",
   `AutoLandscapeMaterial.cpp`/`GiAutoLandscape.cpp`, von Thema 180 Schritt 8 verändert) UND
   das gemalte `WaterSurface`/`WaterMesh`-System (Seen/Teiche, von Thema 206 verändert). Das
   reale Testprojekt des Menschen (`C:\Users\conno\Downloads\HEProject`) hat BEIDES
   gleichzeitig auf derselben Landschaft: `puddleAmount: 0.76` (Weather-Komponente) UND drei
   gemalte Wasserkörper (`terrain.waterBodies`, ids 1/3/4) in einem gemeinsamen
   `WaterSurface`-Mesh. Nur Letzteres flackert in meinen Messungen.

## Repro-Aufbau

- Projekt: Kopie von `C:\Users\conno\Downloads\HEProject` (das tatsächliche, in
  `%APPDATA%\HorizonEngine\config.json` als `LastProjectPath` hinterlegte Projekt des
  Menschen) nach `C:\hw234\HEProject`. Einzige Szene: `Content\StartupScene.hescene`.
  `timeOfDay: 0.0` (Mitternacht), `wetness: 0.0` (Environment-Komponente, getrennt von
  Weather-`puddleAmount`).
- Die Szene hatte **keine CameraComponent**. Über die MCP-Bridge der deployten
  `HorizonEditor.exe` (private APPDATA `C:\hw234\appdata`, `HE_MCP=1`,
  `scripts/he_mcp.py`/`he_mcp.py` aus dem Deploy) eine `ReproCamera`-Entity angelegt:
  Position `[187.9966, 22.0, 628.0]`, Rotation `[-15, 0, 0]`, `camera.isMain=true`,
  60° FOV — blickt auf das `WaterSurface`-Mesh bei Weltposition `[187.9966, 0.0,
  568.3793]` (per `entity_get` auf die `WaterSurface`-Entity ermittelt). Szene gespeichert
  (`scene_save`).
- Export über `project_package` (Host, Shipping-Profil, `startupScene:
  Content/StartupScene.hescene`) nach `C:\hw234\game`. Executable heißt `HEProject.exe`
  (nicht `HorizonGame.exe` — das Export-Profil benennt nach dem Projekt).
- Burst-Capture-Skript `C:\hw234\burst_capture.ps1` (Ableitung von
  `docs/spielpfad-postfx-run-game.ps1`, DPI-fix via `SetProcessDpiAwarenessContext`,
  `PrintWindow` + `PW_RENDERFULLCONTENT`): setzt `GameBackend`/`GlobalIlluminationEnabled`
  in `data/config.json`, startet `HEProject.exe`, wartet auf "OnInit complete" im Log,
  25 s Settle (Shipping-Export zeigt trotzdem kurz das Horizon-Creations-Intro — mit
  9 s Settle war Frame 0 noch der Splash), dann 24 Frames im Abstand von 66 ms
  (`PrintWindow`-Screenshots, Kamera/Fenster bleiben die ganze Zeit unangetastet — kein
  Input, keine Bewegung). `C:\hw234\diff_frames.py` bildet absolute Pixel-Differenzen
  zwischen aufeinanderfolgenden Frames (mean/max/Anzahl Pixel mit |Δ|>10) und ein
  gemitteltes Diff-Heatmap-Bild.

## Messungen

| Backend | GI (`GlobalIlluminationEnabled`) | Frames | mean |Δ| | max |Δ| | geänderte Px (>10, von 810 000) |
|---|---|---|---|---|---|
| D3D11  | aus | 24 | 0.15–0.22 | 39–94 | ~3 900–5 900 |
| D3D12  | aus | 24 | 0.15–0.22 | 49–86 | ~3 800–5 700 |
| D3D12  | **an** | 24 | 0.19–0.22 | 49–82 | ~4 900–6 100 |
| Vulkan | aus | 24 | 0.16–0.20 | 39–77 | ~3 900–5 300 |
| OpenGL | aus | 24 | 0.18–0.23 | 45–74 | ~4 300–5 600 |
| OpenGL | **an** | 24 | 0.18–0.22 | 45–79 | ~4 250–5 600 |
| Metal  | — | — | — | — | **nicht getestet** (NN-WS03 ist Windows, kein Mac erreichbar) |

Alle sechs Läufe: identisches Bild. Kein Backend sticht heraus, kein GI-Zustand sticht
heraus. Das gemittelte Diff-Heatmap-Bild (`diff_accum.png`, je Lauf unter
`C:\hw234\shots\<run>\`) zeigt die Änderung ausschließlich an zwei Stellen: (1) den
Himmelssternen (vermutlich beabsichtigtes Sternfunkeln) und (2) einem Fleck in exakter
Form und Lage der Wasserfläche darunter — die trockene, feuchte Umgebung (das
Auto-Landscape-Puddle-Overlay) bleibt in jedem Lauf bei Δ≈0 stehen.

Stichprobe (D3D11, GI aus), zwei Rohframes im Abstand von 5×66ms nebeneinander
(`C:\hw234\shots\d3d11_gi0b\crop_f000.png` vs. `crop_f005.png`): die Sternreflexionen auf
dem Wasser verschieben/verändern sich sichtbar zwischen den Frames, obwohl Kamera und
Sternenhimmel selbst (bis auf das eigene Funkeln) unverändert sind.

## Ursachen-Eingrenzung

- **Thema 191 (GI-Reflektionen) — ausgeschlossen.** Nicht gemerged (s.o.); und selbst der
  auf diesem Branch erreichbare `GlobalIlluminationEnabled`-Schalter (diffuses GI, inkl.
  des separaten `GiAutoLandscape.cpp`-`autoWet`-Terms) ändert die Flacker-Stärke nicht
  messbar (D3D12, OpenGL).
- **Thema 180 Schritt 8 (Pfützen-Steigungs-Fix, AutoLandscapeMaterial) — kein beobachtetes
  Flackern im trockenen Overlay.** Dieser Commit ist im Branch enthalten, betrifft aber ein
  anderes System (Terrain-Material, nicht `WaterSurface`); in allen sechs Diff-Heatmaps
  bleibt die Fläche außerhalb des Sees stabil.
- **Thema 206 (Wasser-Material, insb. Schritt 3 "finer irregular chop, four noise-slope
  layers") — plausibelster Verdächtiger, nicht abschließend verifiziert.** Die
  Wellen-Chop-Normale ist laut `src/HE_Tools/src/MatGen/main.cpp:784-838` eine
  zeitanimierte ("drifting noise") Überlagerung aus vier Schichten, unabhängig von
  Kamera oder GI. Eine sich pro Frame ändernde Normale verschiebt die Spiegelung von
  Punktlichtquellen (hier Sterne) unabhängig vom Backend — genau das Bild, das gemessen
  wurde. Nicht verifiziert: ob genau DIESE Chop-Logik (oder ein anderer, unveränderter Teil
  des Wassermaterials) für das Δ verantwortlich ist; dazu müsste der Chop versuchsweise auf
  0 gesetzt und erneut gemessen werden — laut Vorgabe für diesen Schritt nicht erlaubt
  ("nicht patchen, nur messen").

## Offene Punkte / nicht verifiziert

- **Editor-Vergleich ist nicht kontrolliert.** Ein Burst von sechs `scene_screenshot`-Rufen
  an dieselbe Kamerapose über die MCP-Bridge zeigte ebenfalls Δ (sogar etwas stärker:
  mean 0.37–0.40, ~9 000 geänderte Px) — aber die Aufrufe liegen wegen MCP-Overhead
  unregelmäßig und deutlich weiter als 66 ms auseinander, sodass die zeitanimierte
  Chop-Welle zwischen zwei Editor-Shots weiter voranschreitet als zwischen zwei
  Game-Frames. Das ist KEIN kontrollierter Vergleich und widerlegt die "nur im Spiel"-
  Beobachtung nicht, bestätigt sie aber auch nicht. Für eine saubere Aussage bräuchte es
  eine `HE_DUMP_*`-Zeugen-Sequenz mit exakt demselben Zeit-Delta wie die 66-ms-Game-Bursts,
  oder eine Live-Editor-Capture-Methode mit fester Framerate.
- **Nur Nachtszene (timeOfDay 0.0) getestet.** Das Sternfunkeln liefert die
  Punktlichtquellen für den gemessenen Effekt; bei Tag wäre die Sonne die vergleichbare
  helle Quelle ("sun glitter" auf bewegtem Wasser) — vermutlich noch auffälliger, aber
  nicht gemessen.
- **Metal nicht getestet** (kein Mac auf NN-WS03 erreichbar).
- **Welcher Build/welche Pfützen-Art der Mensch tatsächlich beobachtet hat, ist nicht
  geklärt** — Anfrage über die Queen an den Menschen weitergeleitet (Hive-Request #147),
  noch keine Antwort.
- Das Auto-Landscape-Puddle-Overlay (trockener Boden) selbst wurde nur mit
  `puddleAmount=0.76`, `currentKind=Clear` (keine aktive Regenanimation) getestet; ob es
  unter aktivem Regen (`targetKind=Rain`, Partikel/Intensität live) anders aussieht, wurde
  nicht geprüft.

## Artefakte

Alle Rohdaten liegen unter `C:\hw234` (scratch, nicht im Repo):
`HEProject\` (Projektkopie mit `ReproCamera`), `game\` (Export), `shots\<run>\f*.png` +
`diff_accum.png`/`diff_worst.png` je Lauf (`d3d11_gi0b`, `d3d12_gi0`, `d3d12_gi1`,
`vulkan_gi0`, `opengl_gi0`, `opengl_gi1`), `edshots\` (Editor-Burst), `burst_capture.ps1`,
`diff_frames.py`, `drive.py` (MCP-Treiber), `poll_build.py`.

## Nächste Schritte (Vorschlag, nicht Teil dieses Schritts)

1. Kontrollierten Editor-vs-Spiel-Vergleich mit identischem Zeit-Delta nachholen.
2. Tagszene (helle Sonne) in derselben Kameraposition testen — vermutlich deutlicher.
3. Falls Thema 206s Chop-Normale bestätigt werden soll: versuchsweise (auf einem
   Experimentier-Branch, nicht hier) `kChopBase`/`kChopMid`/`kChopFine`/`kChopFiner` auf 0
   setzen und denselben Burst-Test wiederholen — bleibt der See dann bei Δ≈0, ist der Chop
   die Ursache.
4. Metal-Test auf einem Mac nachholen (MacBook-Air-von-Connor ist im Hive online).
5. Menschliche Antwort auf Hive-Request #147 abwarten (welcher Build, welche Pfützen-Art).
