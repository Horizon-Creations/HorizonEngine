# Wolken 2.0, Schritt 1: Wind-Sprung-Bug, akkumulierter Wolken-Drift (Thema 227)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, Basis `a79c5150` (origin/release/0.8.0 nach PR 210).

## Das Problem
Die Wolken standen an ~16 Stellen auf `pos * scale + wind * time` (kSkyFS in `SkyShaderSource.h`, der
Metal-Spiegel in `MetalRenderer.mm`). Ändert man Windgeschwindigkeit oder -richtung, springt jede Wolke um
`(neu - alt) * time`, statt dass nur ihr Tempo ab jetzt wechselt.

## Was geändert ist
* `CloudDrift` (`SkyFrameParams.h`): integriert den Wind über die Uhr, `double`-Zustand. Erster Aufruf = `wind * time`
  (eine Szene mit konstantem Wind sieht aus wie vorher), gleiche oder frühere Uhr bewegt nichts (Sky-Pass,
  Wolkenschatten und Low-Res-Pre-Pass eines Frames lesen denselben Wert).
* `CloudDriftOffset(wind, time)` (`SkyFrameParams.cpp`): der prozessweite Akkumulator. Mit festgehaltener Uhr
  (`HE_SKY_TIME` oder `HE_DUMP_SKYTIME`) ist er die geschlossene Form `wind * time`, damit Headless-Bilder
  nicht davon abhängen, was vorher lief.
* Neuer Slot `SkyFrameParams::drift` (vec4, 336 -> 352 Byte), danach in: `kSkyVulkanPrelude` (Block +
  `#define uWindDrift`), kSkyFS-Uniform `uWindDrift`, MSL `SkyParams`, OpenGL (`m_uSkyWindDrift`, beide
  Push-Stellen: Wolkenschatten ~5354 und `DrawSkyFullscreen` ~11331). Metal/Vulkan/D3D kopieren die ganze Struktur.
* Shader: `wind * time` -> `uWindDrift` (GLSL, globales Uniform), im MSL-Spiegel als zusätzlicher Parameter
  `float3 drift` hinter `wind` durch alle Cloud-Funktionen gereicht. `wind` bleibt die Geschwindigkeit (die
  Windscherung in `cloudFieldDensity` braucht `|wind|`).
* Cirrus bekommt `driftXZ` statt `windXZ * time`.

## Geprüft (Mac, Release, Metal + OpenGL)
* A/B mit `scripts/he_shot.py`, 16 Bilder je Backend-Satz (Dome/Volumetrisch, t=30/600 s, Dämmerung, Cirrus, Godrays;
  `HE_SKY_TIME` fest, AA aus): alle 16 **bitgleich** zur Baseline vom Release-Editor `31d6253c` (Sky-Code zwischen
  `31d6253c` und `a79c5150` unverändert). Rauschboden: zwei Baseline-Läufe 16/16 identisch. Zeuge ist
  empfindlich (t=30 und t=600 liefern verschiedene Bilder). Das beweist, dass `uWindDrift` auf Metal und GL
  ankommt und gelesen wird.
* `ctest`: `test_sky_shader` (neu: 5 CloudDrift-Fälle, Feldreihenfolge mit `drift`, HLSL `heSky_drift : packoffset(c21)`,
  kein `wind * time` mehr im Shadertext), `test_culling`, `test_weather`, `test_engine_api*` grün.
* Unpinned Smoke-Bild (Wanduhr) rendert auf beiden Backends Himmel und Wolken.

## Nicht geprüft
* Das Verhalten beim Verschieben des Wind-Reglers im laufenden Editor **sichtbar**: nur der Akkumulator ist als
  Unit-Test belegt (Geschwindigkeits- und Richtungswechsel stetig), kein Live-Bild vor/nach dem Wechsel.
* D3D11, D3D12, Vulkan zur Laufzeit (Windows). Compile-Ebene ist über he::shaderc (SPIR-V, HLSL) im Test belegt.
* Die Fallback-Shader ohne shaderc (`HlslSources.h` kSkyFS/kSkyPSHLSL, `shaders/sky.frag`) positionieren die
  Wolken weiter mit `wind * time`; dort springt der Fall weiter. Reduzierte Blöcke, nicht angefasst.
* Mehrere Welten mit verschiedenem Wind im selben Prozess teilen einen Akkumulator (Editor-Szene und Spielvorschau
  haben normalerweise denselben Wind).

## Fallen
* he_tests/Editor im Worktree: Konfiguration mit `-DHE_SDK_RELEASE_SET=OFF`, `HE_ENABLE_STEAM_AUDIO=OFF`,
  `FETCHCONTENT_SOURCE_DIR_*` aus `cmake-build-release/_deps` des Hauptcheckouts und ccache; Editor in ~10 min,
  he_tests danach in ~12 min.
* Ein Edit während des laufenden Builds kann ein veraltetes Objekt hinterlassen: nach dem Build die geänderten
  Dateien `touch`-en und noch einmal bauen.

## Handoff
* Entscheidung getroffen: ein neuer vec4-Slot statt Umdeuten von `uTime` (`uTime` treibt auch Aurora, Evolution,
  Sternschnuppen; eine Richtungsänderung würde die Verschiebung ebenfalls springen lassen).
* Nächste Schritte (Hive 227): 1138 Dome-Wolken Höhe/Distanz (`domeDeckXZ`, ~485-490), 1139 Bestandsaufnahme und
  Plan der 3D-Volumetrik (Referenzbilder `~/Downloads/clouds-4215608_1920.jpg`, `images*.jpeg`), 1141 Abnahme.
* Offen für Windows: Laufzeit D3D11/D3D12/Vulkan, und die Fallback-Shader entscheiden (nachziehen oder lassen).
