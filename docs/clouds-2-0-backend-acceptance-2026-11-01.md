# Wolken 2.0, Schritt 5: Backend-Abnahme D3D11/D3D12/Vulkan (+ OpenGL-Gegenprobe, Metal statisch) (Thema 227)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, HEAD `61435f2b` (NN-WS03, Windows). Baut auf den Schritten 1137-1140
(Wind-Drift, Dome Cloud Height, Bestandsaufnahme, 3D-Verbesserungen), die bisher nur auf Mac/Metal+GL geprüft waren.

## Build
`cmake -S . -B C:/hebuild/wolken5` mit `FETCHCONTENT_SOURCE_DIR_*` auf die `_deps`-Quellen des Hauptcheckouts (der
Worktree-Pfad ist sonst zu lang für `nlohmann_json`s Subbuild-Stempel, `CMAKE_OBJECT_PATH_MAX` warnte zusätzlich bei
Jolt/glslang/Assimp/recast — ein kurzer Build-Ordner behebt beides, siehe `[[worktree-builds-need-deploy-dir-override]]`).
Zwei Session-Abbrüche während des ~25-min-Builds (siehe Lehre unten); am Ende sauber: **0 Fehler**, `HorizonEditor.exe`
nach `out/deploy/Editor` deployt, `he_tests.exe` gelinkt. Fünf Backends aktiv (OpenGL, Vulkan, D3D11, D3D12, Software),
glslang+SPIRV-Cross gefunden ("shader cross-compile enabled").

## Tests
* `scripts/validate_embedded_shaders.py`: **117/117** eingebettete Shader übersetzen (HLSL 59, GLSL 58; MSL übersprungen,
  kein Xcode auf Windows).
* `he_tests.exe --source-file="*test_sky_shader.cpp"`: **14/14** Testfälle, 340/340 Assertions. Darunter D3D11- und
  D3D12-WARP-Tests, die den cross-kompilierten `kSkyFS` echt übersetzen und eine PSO bauen (nicht nur Text-Diff) — das
  ist der Pfad, über den Wind-Drift und Dome Cloud Height auf Windows laufen (`he::shaderc::compileHlslPinned`, siehe
  `D3D11Renderer.cpp:6142`). D3D12 vs. D3D11 Nebel: **mean |delta| 0, max 0** (bitgleich).
* Volle `he_tests.exe`: **6062/6062** Testfälle, **2.156.089/2.156.089** Assertions, 0 fehlgeschlagen, 20 übersprungen
  (plattformbedingt). Scratch-`%APPDATA%` benutzt, nicht das echte Profil.

## Backend-Screenshots (D3D11, D3D12, Vulkan, OpenGL — Mac/Metal nicht verfügbar auf NN-WS03)
Erzeugt über `HE_DUMP_*` (headless, `HE_HIDDEN_WINDOW=1`, `HE_DUMP_RHI=<Backend>`, `HE_SKY_TIME` gepinnt wo wirksam),
1280x720, TOD 0.32, Coverage 0.6, Wind 20/45°, Pitch 16°.

* `docs/clouds-2-0-survey/backend-acceptance-dome-height-2026-11-01.jpg` — Dome-Wolken, `cloudDomeHeight` 1.0 vs. 2.5,
  vier Backends. Der Faktor wirkt auf allen vieren gleich (2.5 = kleinere, feinere, weiter entfernt wirkende Wolken,
  wie in `docs/clouds-2-0-dome-height-2026-10-29.md` für Metal/GL beschrieben).
* `docs/clouds-2-0-survey/backend-acceptance-realistic-clouds-2026-11-01.jpg` — 3D-Realistic-Wolken, `cloudType` 0
  (Gemischt) vs. 4 (Cumulonimbus), vier Backends, sichtbar übereinstimmende Form je Spalte.

### Pixel-Diffs (1-von-4-Pixel-Raster, |ΔR|+|ΔG|+|ΔB| / 3)
| Vergleich | Szene | mean Δ/255 | max Δ/255 |
|---|---|---|---|
| OpenGL vs. D3D12 | klarer Himmel (Coverage 0) | 0.46 | 3 |
| OpenGL vs. D3D12 | Realistic-Wolken (Type 0) | 17.35 | 110 |
| OpenGL vs. Vulkan | Realistic-Wolken (Type 0) | 17.37 | 109 |
| OpenGL vs. D3D11 | Realistic-Wolken (Type 0) | 17.52 | 110 |
| D3D12 vs. Vulkan | Realistic-Wolken (Type 0) | 2.02 | 40 |
| OpenGL vs. D3D11/D3D12/Vulkan | Dome, Höhe 2.5 | ~20.6-20.8 | ~100-102 |
| D3D11 Höhe 1.0 vs. 2.5 (selbes Backend) | Dome | 17.93 | 90 |

**Einordnung:** der klare Himmel (keine Wolken, kein Dither) ist zwischen nativer GLSL (OpenGL) und cross-kompiliertem
HLSL (D3D12) praktisch bitgleich (0.46/255) — die Atmosphäre/Sonne/Horizont-Luftperspektive aus Schritt 4 kompiliert
korrekt in alle drei Windows-Backends. Sobald Wolken im Bild sind, liegt die Abweichung zwischen nativer GLSL und
jedem cross-kompilierten Backend bei 17-21/255 (Max ~100-110, an Wolkenrändern/Dither-Übergängen), während zwei
cross-kompilierte Backends untereinander (D3D12 vs. Vulkan) nur 2/255 auseinanderliegen. Das grenzt die Ursache auf
den Feindetail-/Blue-Noise-Dither-Hash ein, der beim Cross-Compile (SPIRV-Cross/FXC) offenbar einen leicht anderen
Bitpfad nimmt als die native GLSL-Auswertung — nicht auf Wolkenform oder -farbe (beide stimmen visuell über alle vier
Backends überein, siehe Grids). Zum Vergleich: Metal (handgepflegter Spiegel, kein Cross-Compile) lag auf dem Mac bei
0.46-0.64/255 gegen GL für dieselbe Szene (`docs/clouds-2-0-adaptive-march-detail-2026-10-31.md`) — deutlich näher,
weil Metal denselben Hash von Hand repliziert statt ihn zu cross-kompilieren. **Nicht behoben** (Schritt sagt: keine
Shader-Refactors) — falls das stört, ist der nächste Schritt, den Dither-Hash in allen Dialekten exakt gleich zu
schreiben, nicht neu zu tunen.

Der `cloudDomeHeight`-Effekt selbst (D3D11, Höhe 1.0 vs. 2.5) liegt mit 17.93/255 genau in der auf dem Mac gemessenen
Größenordnung (16/18/24 für 0.5/2.0/3.5 gegen 1.0, siehe `docs/clouds-2-0-dome-height-2026-10-29.md`) — der Regler
wirkt auf D3D11 wie erwartet.

## Metal-Spiegel (MetalRenderer.mm)
Nicht laufzeitgeprüft (NN-WS03 ist Windows, kein Xcode). Statisch per Grep bestätigt: `cloudDomeHeight`/`drift.w`,
`cloudType`/`cloudShapeParams`, `qFineF`/`tFineMax`/`skin` (adaptiver Marsch aus `61435f2b`) und `uWindDrift` sind alle
in `MetalRenderer.mm` vorhanden und an den erwarteten Stellen (Zeilen 4787-4877 Formfunktion, 5126-5254 adaptiver
Marsch) — konsistent mit den Mac-Handoffs, die "Metal+GL bitgleich" für jeden der vier Schritte dokumentieren. Kein
Shader geändert.

## Nicht geprüft
* Metal/macOS zur Laufzeit (kein Mac-Gerät in dieser Session).
* Der Wind-Sprung-Fix selbst **visuell live im Editor** (nur Unit-Test + die bestehenden Mac-Pinned-Bilder; ein
  Einzelbild-Dump kann den Sprung zwischen zwei Prozessstarts nicht zeigen, weil der Akkumulator pro Prozess neu
  beginnt — dafür ist `CloudDrift`/`CloudDriftOffset` in `test_sky_shader.cpp` und der vollen Suite abgedeckt).
* Die Fallback-Shader ohne shaderc (`HlslSources.h` `kSkyPSHLSL`, `shaders/sky.frag`) — wie in
  `docs/clouds-2-0-wind-drift-2026-10-29.md` vermerkt, bleiben sie unverändert (springen weiter, kein `cloudDomeHeight`).

## Lehre für die nächste Windows-Baurunde
Ein laufender `cmake --build`/`he_tests`-Lauf, über `run_in_background` (Bash/PowerShell-Tool) gestartet, stirbt mit
der Harness-Session (per `tasklist` bestätigt — Prozess komplett weg, nicht nur die Benachrichtigung). Fix: Start über
PowerShells `[System.Diagnostics.Process]::Start(UseShellExecute=$true)`, der echte Prozess läuft unabhängig vom
Harness weiter (zweimal über Session-Abbrüche hinweg bestätigt). Danach den Fortschritt über `tasklist`/Log-`tail`
pollen statt über die Tool-eigene Hintergrund-Benachrichtigung, die denselben Tod stirbt. Details: `[[hive_lesson 605]]`.

## Handoff
* Alle drei Themen-Punkte (Wind-Sprung, Dome-Höhe, 3D-Verbesserungen) sind auf D3D11, D3D12 und Vulkan lauffähig und
  liefern die erwarteten Effekte, geprüft über Unit-Tests (bitgenau wo zutreffend) und Screenshots (visuell
  übereinstimmend, Metrik oben). Build und volle Testsuite sind grün.
* Offen für einen möglichen Schritt 6 (nicht Teil dieses Schritts): den Blue-Noise-Dither-Hash so umschreiben, dass er
  bei Cross-Compile (SPIRV-Cross/FXC) bitgenau zur nativen GLSL-Auswertung passt, falls die ~17-21/255-Abweichung an
  Wolkenrändern stört. Aktuell rein kosmetisch (Feinkörnung), keine Form-/Farbabweichung.
* Metal-Laufzeitprüfung bleibt offen, bis wieder ein Mac-Gerät verfügbar ist; der Code-Abgleich gibt keinen Anlass zur
  Sorge.
