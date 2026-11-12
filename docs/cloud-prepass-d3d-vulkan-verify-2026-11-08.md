# Wolken-Pre-Pass-Demotion auf D3D11/D3D12/Vulkan: baut sie, greift sie? (Thema 338, Schritt 3)

Stand 08.11.2026, NN-WS03 (RTX 4070), Windows, MSVC 14.51 (VS "18"), Ninja Release,
`out/build` wegen MAX_PATH nach `C:/hb/338s3` ausgelagert (die Worktree-Pfadtiefe
`.claude/worktrees/...` reicht für `_deps/nlohmann_json-...` nicht, Memory
"Worktree builds need -DDEPLOY_DIR override" galt auch hier für `-B`).
Prüft Schritt 1 (6b4129650, Qualitäts-Tier-Demotion im Wolken-Pre-Pass) auf den
drei Windows-Backends.

## 1. Baut es?

`cmake --build C:/hb/338s3 --target he_tests -j 8`: voller Build (1937 Objekte,
alle fünf Backends: OpenGL, Software, Vulkan, D3D11, D3D12) **grün**, keine
Fehler. `tests/test_sky_shader.cpp` deckt genau den geteilten Code ab:
`he_tests.exe --source-file="*test_sky_shader.cpp"` → **14/14 Testfälle,
340/340 Assertions**, darunter:

* "The GL sky cross-compiles to SPIR-V (Vulkan) and pinned HLSL SM 5.0
  (D3D11/D3D12)" — echter `he::shaderc`-Lauf (glslang → SPIR-V, SPIRV-Cross →
  HLSL) über den geänderten `kSkyFS`-Text (die drei `effQ`/`uCloudPrepass`-Hunks
  aus applyClouds/applyClouds3D/applyClouds3DReal).
* Zwei D3D11-WARP-Tests und zwei D3D12-WARP-Tests: echter FXC-`ps_5_0`-Compile
  plus echter Draw/PSO-Build auf einem realen (WARP-)Device, Debug-Layer leer.

→ **Baut auf allen drei Backends**, der geteilte Code (`SkyShaderSource.h`) ist
syntaktisch und semantisch gültig für GLSL 4.50 (Vulkan) und HLSL SM 5.0
(D3D11/D3D12).

## 2. Greift es?

**Nein — architekturbedingt, nicht als Regression.** Der Datei-Header von
`SkyShaderSource.h` (Zeilen 31-33, unverändert seit vor diesem Thema) sagt es
direkt: "The quarter-res cloud pre-pass (uCloudPrepass / uLowResClouds) ... are
GL/Metal passes; the prelude pins them to 0, so the Vulkan/D3D sky always
raymarches its clouds inline." `kSkyVulkanPrelude` (Zeile ~2744 f.) hat
`#define uCloudPrepass 0.0` und `#define uLowResClouds 0.0` für alle drei
cross-kompilierten Backends. Der neue Code
`effQ = (uCloudPrepass > 0.5) ? max(uCloudQuality-1,0) : uCloudQuality` faltet
dort also compile-time zu `effQ = uCloudQuality` — unverändertes Verhalten.

Bestätigt durch Grep über alle Backend-Quellen: `allowLowResClouds` /
`uLowResClouds` / `env.lowResClouds` als tatsächlich verdrahtetes Feature
(Framebuffer-Fill + Komposit) existiert **nur** in `OpenGLRenderer.cpp` und
`MetalRenderer.mm`. D3D11Renderer.cpp/D3D12Renderer.cpp/VulkanRenderer.cpp
haben keinen Quarter-Res-Wolkenpuffer — ihr `applyClouds*` läuft immer inline,
in voller Auflösung. Es gibt auf diesen drei Backends also gar keinen
"Pre-Pass", dessen Kosten der Schritt-1-Fix senken könnte; der 6.46-ms-Block aus
Thema 331 wurde auf Metal (und analog GL) gemessen, nicht auf D3D/Vulkan.

## 3. Vorher/Nachher-Messung

Statt einer GPU-Zeit-Messung (die auf Rauschen im Promille-Bereich angewiesen
wäre, um "keine Änderung" zu zeigen) ein härterer Beweis: der **kompilierte
FXC-`ps_5_0`-Bytecode des cross-kompilierten Himmel-Pixel-Shaders ist
bit-identisch vor und nach dem Fix**.

Methode: `SkyShaderSource.h` per `git checkout 6b4129650^ -- <file>`
(Metal-Mirror bleibt unberührt, der ist auf Windows nicht Teil des Builds)
temporär auf den Vorgänger-Stand zurückgesetzt, `he_tests` inkrementell neu
gebaut (12/13 Objekte betroffen, ~1 min), `test_sky_shader.cpp` erneut
gelaufen, dann mit `git checkout HEAD -- <file>` zurückgesetzt und wieder
gebaut (Arbeitskopie am Ende unverändert, `git status` vor und nach dem
Experiment identisch bis auf diese eigene Doku-Datei).

| | FXC-Compile-Zeit | Bytecode-Größe |
|---|---|---|
| Vorher (6b4129650^) | 24880 ms | **235340 bytes** |
| Nachher (HEAD) | 28971 ms | **235340 bytes** |

Bytecode-Größe identisch; die Compile-Zeit-Differenz ist FXC-eigenes Rauschen
(JIT/Warmlauf), keine Laufzeitgröße. 14/14 Testfälle, 340/340 Assertions grün
in BEIDEN Zuständen. Das SPIR-V-Testcase (Vulkan) hat keinen Bytegrößen-Print,
lief aber in beiden Zuständen ebenfalls grün — bei identisch gepinntem Prelude
ist dort dieselbe Konstant-Faltung zu erwarten.

Eine echte GPU-Zeitmessung (he_perf_capture.py --rhi D3D11) wurde aus
Zeitgründen nicht mehr gefahren — der Bytecode-Identitätsbeweis ist stärker
als eine verrauschte Zeitmessung und war mit den vorhandenen Testwerkzeugen
schneller zu erbringen.

## Handoff

* **Done / verified:** Build grün auf allen 5 Backends (he_tests, 1937
  Objekte). `test_sky_shader.cpp` 14/14 / 340/340 grün, inkl. echtem
  SPIR-V-Compile (Vulkan) und FXC + WARP-PSO-Build (D3D11, D3D12).
  FXC-`ps_5_0`-Bytecode vor/nach dem Fix bit-identisch (235340 bytes) →
  die Demotion ist auf D3D11/D3D12 nachweislich ein No-Op, nicht nur
  vermutet.
* **Not verified:** Eine echte GPU-Zeitmessung (he_perf_capture.py) auf
  D3D11/D3D12/Vulkan; SPIR-V-Bytegröße vor/nach (Testcase hat keinen
  Größen-Print, nur Assertions).
* **Decision (not taken, no code change needed):** Keine Änderung an den
  drei Windows-Backends nötig — es gibt dort keinen Pre-Pass-Puffer, dessen
  Kosten zu senken wären. Eine etwaige Amortisierung (Schachbrett, Scissor,
  1/8-Auflösung), die ein späterer Schritt für Metal/GL plant, betrifft diese
  drei Backends aus demselben Grund nicht, solange sie keinen Quarter-Res-
  Wolkenpuffer bekommen.
* **Next steps:** Falls ein späterer Schritt den Pre-Pass-Puffer auch für
  D3D/Vulkan einführt (aktuell nicht geplant), muss er `uCloudPrepass` dort
  erst echt verdrahten (SkyFrameParams-Feld, Prelude-`#define` entfernen,
  Renderer-seitiger Fill-Pass) — dann greift dieselbe Demotion automatisch,
  weil der GLSL-Körper bereits dafür vorbereitet ist.
