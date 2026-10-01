# Nebula-Parität GL → D3D11 / D3D12 / Vulkan / Metal: Analyse und Plan

Stand 01.10.2026, Zweig `claude/nebula-cloud-parity-backends` (Basis `0513f8d4`).
Schritt 1 des Themas „Nebula/Wolken-Himmelseffekt: Parität GL → D3D11, D3D12, Vulkan, Metal".
Reine Analyse, kein Code geändert.

## Kurzfassung

**Die Prämisse des Themas ist überholt. Ein Port ist nicht nötig, er ist schon passiert.**

* Der Himmel ist **eine** GLSL-Quelle: `kSkyFS` + `kSkyFuncGLSL` in
  `src/HE_Rendering/include/HorizonRendering/SkyShaderSource.h`. Die volle Nebula v3
  (Worley-Gasstücke, Crackle-Cage, Hälse, Beads, Staub) steht dort in `nebIso()` und
  `nebula()` (Z. 1201–1467), aufgerufen in Z. 2082.
* **Vulkan, D3D11, D3D12** übersetzen genau diesen Text seit Thema 78, Schritt 3
  (Commit `c2e2351d`, „D3D11/D3D12/Vulkan: GL-Himmel als einzige Quelle, per he::shaderc
  uebersetzt"): `BuildSkyFragmentGLSL450()` ersetzt den GL-Deklarationsblock durch
  `kSkyVulkanPrelude` (Uniform-Block = `HE::SkyFrameParams`, jede GL-Uniform per `#define`
  auf ihr Feld), dann glslang → SPIR-V (Vulkan) bzw. → SPIRV-Cross → HLSL SM 5.0
  (`compileHlslPinned`, D3D11/D3D12).
  Aufrufstellen: `VulkanRenderer.cpp:7314`, `D3D11Renderer.cpp:4765`, `D3D12Renderer.cpp:2465`.
* **Metal** hat eine eigene MSL-Übertragung (`MetalRenderer.mm` Z. 4689–4957,
  Aufruf Z. 5572). Sie ist nach Normalisierung **zeilengleich** mit GL (Nachweis unten).
* Die „Einfarb-Nebula" in D3D12 (`D3D12Renderer.cpp` Z. 115/124/355/408), der reduzierte
  D3D11-Himmel ohne Nebula (`kSkyPSHLSL`, Z. 74 ff.) und `shaders/sky.frag` (Vulkan, Nebula v1)
  sind **Fallbacks**. Sie laufen nur, wenn der Cross-Compile zur Laufzeit scheitert oder der
  Baum mit `HE_ENABLE_SHADERC=OFF` gebaut ist. Das Game-Flavour (Editor, CI, Spiele-Export)
  baut mit `HE_ENABLE_SHADERC=ON` (Default, `CMakeLists.txt:670`); die App-Flavours, die
  shaderc abschalten, linken gar kein D3D/Vulkan (`CMakeLists.txt:636 ff.`).
* Wahrscheinliche Quelle der falschen Prämisse:
  `CopilotDocs/windows-gpu-verification-checklist.md` Abschnitt A5 („Sky/Nebula v2–v3.4 …
  auf D3D/Vulkan — NOCH NICHT IMPLEMENTIERT", „`sky.frag` hat noch Nebula v1"). Das stimmt
  seit `c2e2351d` nicht mehr, der Abschnitt wurde nie nachgezogen.

**Was wirklich fehlt**, ist nicht Code, sondern **Nachweis**: Auf D3D11/D3D12/Vulkan ist die
Nebula übersetzt und versorgt, aber nie als Bild geprüft. Der vorhandene WARP-Bildtest
schaltet sie ausdrücklich ab. Und die Metal-Kopie hat keinen Drift-Wächter.

## Frage aus dem Auftrag: „einmal schreiben" oder „viermal übersetzen"?

| Backend | Quelle des Nebula-Codes | Pflege bei Änderung |
|---|---|---|
| OpenGL | `kSkyFS` + `kSkyFuncGLSL`, GLSL 4.10, lose Uniforms | Referenz |
| Vulkan | derselbe Text, `kSkyVulkanPrelude` + glslang → SPIR-V zur Laufzeit | automatisch |
| D3D11 | derselbe Text, → SPIR-V → SPIRV-Cross HLSL SM 5.0, gepinnte Register | automatisch |
| D3D12 | wie D3D11 | automatisch |
| Metal | `kSkyMSL` in `MetalRenderer.mm`, handgepflegte MSL-Kopie | **von Hand** |

Also: **dreimal automatisch, einmal von Hand.** Eine Nebula-Änderung braucht genau zwei
Stellen (GL-Text + Metal-Kopie). Das ist dieselbe Mechanik wie beim SSR-Fix aus Thema 103.

Editierregeln stehen im Kopf von `SkyShaderSource.h`: neue Uniform braucht Slot in
`SkyFrameParams`/`BuildSkyFrameParams`, `#define` im Prelude und `glUniform*` in
`OpenGLRenderer::DrawSkyFullscreen`; `tests/test_sky_shader.cpp` fällt bei einer
unmapped Uniform um. Alles unter `//#SKYFUNC#` muss in GLSL 4.10 **und** 4.50 gültig sein.

## Das GL-Modell im Detail (Referenz für spätere Änderungen)

`nebula(dir, cdir, sunDir, intensity, nebColor)`, `SkyShaderSource.h:1229`:

1. **Gates.** `intensity <= 0` oder `cover <= 0` → 0. Tiefe Nacht:
   `night = 1 − smoothstep(−0.22, −0.04, sunDir.y)` (tiefer als die Sterne, gegen
   „Türkis-Flut bei Sonnenuntergang"), nur obere Hemisphäre. Galaktisches Band
   `exp(−bd² · mix(8.2, 0.8, cover))` um `galN = (0.46, 0.52, −0.72)`.
2. **Koordinaten.** `P = cN·3.4 + seed·(13.1, 7.7, 19.3)` im rotierenden
   Himmelsrahmen (`celestialDir`, Rodrigues um geneigte Polachse).
3. **Fluss-Warp.** Q1 (3× `starFbm3`, 2 Oktaven), High/Max zusätzlich Q2 → `Pw`, `Pc`, `Pp`.
   `aaFine` aus `fwidth(Pw)` blendet feinste Lagen nahe Nyquist aus.
4. **Gasstücke.** Signierte Tiefe `pd = worleyNoise3(Pp·2.2) + Erosion − thr`, plus
   halbgroße Satelliten (`worleyNoise3(Pp·4.4)`), Cluster-Existenz-Gate über `starFbm3`.
   Daraus `core`, `depth`, `border` (Rand), `skirt`, `neck` (die automatischen
   Filament-Verbindungen: Worley-F1 steigt zum Sattel, die Säume naher Stücke treffen
   sich dort zuerst).
5. **Seide.** Iso-Bänder von `pd` (3, Max 4) mal geriffelte Wisps (1/2/3 je Stufe).
6. **Crackle-Cage.** Vereinigung unabhängiger Ein-Iso-Familien (`nebIso`) auf 3/4/5
   Skalen (Perf/High/Max), Crinkle-Warp ab High, zwei Längenbrecher, Grain, Streifen,
   Mottle; Alpha über `reach = core·0.95 + neck·0.8 + border·0.3`.
7. **Beads** (High/Max) an Worley-Ecken, **Back-Web** + **Neck-Fray** (Max).
8. **Staub.** Rötungsbahnen mit einseitigem Gegenlicht-Rand, seltene dicke Amber-Flecken.
9. **Komposition** hinten → vorn, Farben 1/2/3 über Regionsfeld, wellenlängenabhängige
   Extinktion `exp(−tau·(0.55, 1.05, 1.90))`, Band/Rift-Gating (`mwRift`), Intensität,
   luminanzerhaltendes Rolloff.

Hilfsfunktionen: `starHash`, `celestialDir`, `galacticBand`, `starNoise3`, `starFbm3`,
`mwRift` (Z. 173–230), `worleyNoise3` (Z. 431).

### Parameter- und Rauschvertrag (alle fünf Backends identisch)

| Shader-Name | `SkyFrameParams`-Slot | Quelle (`EnvironmentSettings`) |
|---|---|---|
| `uNebula` | `nebulaColor.w` | `nebulaIntensity` |
| `uNebulaColor` | `nebulaColor.xyz` | `nebulaColor` |
| `uNebulaColor2` | `nebulaColor2.xyz` | `nebulaColor2` |
| `uNebulaColor3` | `nebulaColor3.xyz` | `nebulaColor3` |
| `uNebulaSeed` | `cirrus.w` | `nebulaSeed` |
| `uNebulaHiFi` | `nebulaColor2.w` | `nebulaQuality` (0/1/2) |
| `uNebulaCover` | `neb2.x` | `nebulaCoverage` |

Befüllt in `SkyFrameParams.cpp:37–51` (`BuildSkyFrameParams`), genutzt von Vulkan
(`VulkanRenderer.cpp:7465`), D3D11 (`:4903`), D3D12 (`:2698`) und Metal (`:11047`).
GL lädt dieselben Werte als lose Uniforms (`OpenGLRenderer.cpp:9852–9858`).

Rauschvolumen: alle fünf Backends backen es mit `HE::BuildSkyNoise3D(kNoiseN)`
(`SkyNoise3D.cpp`), RG16: **R** = Value-Noise (`starNoise3`), **G** = Worley
(`worleyNoise3`), Kachelperiode 256 Welteinheiten, Wrap-Sampler.

## Beweiskette (nachfahrbar)

### 1. Aktueller Text übersetzt nach SPIR-V und HLSL SM 5.0

Der Haupt-Build (`build/tests/he_tests`, 24.09.) ist älter als die letzte Sky-Änderung
(`a37590de`, 30.09.), ein Testlauf daraus prüft also alten Text. Deshalb direkt:

```sh
mkdir -p /tmp/nebula_chk && cd /tmp/nebula_chk
cat > dump.cpp <<'EOF'
#include <cstdio>
#include "HorizonRendering/SkyShaderSource.h"
int main() { auto s = HE::glsl::BuildSkyFragmentGLSL450(); if (s.empty()) return 2; fputs(s.c_str(), stdout); }
EOF
clang++ -std=c++20 -I<worktree>/src/HE_Rendering/include dump.cpp -o dump && ./dump > sky450.frag
glslangValidator -V -S frag sky450.frag -o sky.spv && spirv-val sky.spv
spirv-cross sky.spv --hlsl --shader-model 50 --output sky.hlsl
glslangValidator -D -V -S frag -e main sky.hlsl -o /dev/null
```

Ergebnis 01.10.: 2297 Zeilen GLSL, SPIR-V gültig, 2528 Zeilen HLSL, HLSL validiert (rc 0);
`nebula`, `nebIso`, `worleyNoise3` sind im HLSL vorhanden (`float3 nebula(...)` Z. 1189,
Aufruf Z. 2385). Negativkontrolle (Parametertyp vertippt) scheitert wie erwartet.
Grenze: das ist glslangs HLSL-Frontend, nicht FXC. FXC + D3D11-WARP-Draw + D3D12-PSO
prüft `tests/test_sky_shader.cpp` auf der Windows-CI.

### 2. Metal ist zeilengleich mit GL

Normalisierter Diff (Kommentare, Leerraum, `float3→vec3`, Noise-Parameter, Uniform-Namen
→ Parameternamen weg):

```sh
S=src/HE_Rendering/include/HorizonRendering/SkyShaderSource.h; M=src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm
norm() { sed -E 's/, noiseTex, noiseSamp//g; s/float3/vec3/g; s/float2/vec2/g; s/uNebulaColor2/nebColor2/g; s/uNebulaColor3/nebColor3/g; s/uNebulaSeed/nebulaSeed/g; s/uNebulaHiFi/nebQuality/g; s/uNebulaCover/nebCover/g; s/\/\/.*$//; s/[[:space:]]+/ /g; s/^ //; s/ $//' | grep -v '^$'; }
diff <(sed -n 1201,1467p $S | norm) <(sed -n 4689,4957p $M | norm)
```

Einziger Unterschied: die Signatur (Metal reicht Uniforms + Textur als Parameter durch).
Ebenso gleich: `starHash`, `celestialDir`, `galacticBand`, `starFbm3`, `mwRift`;
`starNoise3`/`worleyNoise3` unterscheiden sich nur in `texture()` vs. `.sample()`.
Der Aufruf (Z. 5572) übergibt `p.cirrus.w` als Seed, `p.nebulaColor2.w` als Qualität,
`p.neb2.x` als Coverage, also dieselben Slots wie das Prelude.

## Verbleibende Lücken = Vorschlag für die nächsten Schritte

> **Nachtrag Schritt 2 (01.10.2026):** Punkt 1 und 4 sind erledigt. `tests/test_sky_shader.cpp` hat zwei
> neue Fälle: „D3D11: … draws the nebula at quality 0/1/2 and colour 1 tints it" (32×32, Blick ins
> galaktische Band, `timeOfDay` 0, Sonne y ≈ −0.81, Coverage 0.8) und „D3D12: … draws the nebula like D3D11
> does". Die Prüfungen sind differentiell, also ohne GL-Referenzwerte: die Nebula addiert Licht auf über 5 % der
> Pixel, die Qualitätsstufen 0/1/2 ergeben verschiedene Bilder, Farbe 1 blau ↔ rot verschiebt den
> Blau- bzw. Rotanteil der Nebula, mit Farbe 1 blau (2/3 fast schwarz) überwiegt Blau, und das D3D12-Bild
> ist gleich dem D3D11-Bild (mittlere Abweichung < 1e-3). Windows-CI-Lauf 36835142115 grün
> (`test_sky_shader` 16,6 s statt 14,1 s). Die gemessenen Werte stehen als MESSAGE im Test und sind nur bei
> `ctest -V` oder einem roten Lauf sichtbar. CopilotDocs-Checkliste A5 ist korrigiert.

1. **D3D11-WARP-Bildtest mit Nebula an** (wichtigster Schritt). `skyLookingUp()` in
   `tests/test_sky_shader.cpp` setzt `nebulaIntensity = 0` und prüft nur das Zenit.
   Neuer Fall: tiefe Nacht (Sonne y ≈ −0.5), Sterne/Milchstraße/Aurora aus, Nebula
   Intensität 1, Coverage ~0.8, Kamera ins galaktische Band, je Qualitätsstufe 0/1/2.
   Prüfen: Bild mit Nebula ≠ ohne Nebula (Mittelwert über Region > Schwelle), Farbtendenz
   folgt Farbe 1 (z. B. Farbe 1 rein blau → B dominiert im Inneren). Läuft nur auf
   Windows-CI; D3D12 dort gleich mit (PSO-Test existiert, Draw ergänzen).
   Optional: GL-Referenzwerte aus einem Headless-GL-Lauf mit denselben Eingaben für eine
   grobe Kreuzprüfung.
2. **Vulkan wird nirgends ausgeführt.** Windows-CI hat das Vulkan-SDK (glslang/
   SPIRV-Cross-Tags), aber keinen Software-ICD; lokal gibt es nur den Syntax-Check gegen
   MoltenVK. Optionen: lavapipe (Mesa) im Linux-CI-Job oder SwiftShader auf Windows, dann
   denselben Bildtest wie Punkt 1 gegen den SPIR-V-Pfad. Aufwand höher, eigener Schritt.
   Bis dahin ist Vulkan nur „übersetzt + spirv-val", nicht „gezeichnet".
3. **Drift-Wächter Metal ↔ GL.** Der normalisierte Diff oben als doctest: beide Dateien
   als Text lesen (`kSkyMSL` liegt in `MetalRenderer.mm`, nicht in einem Header; Quellpfad
   per Compile-Definition), `nebIso`/`nebula` und die Hilfsfunktionen ausschneiden,
   normalisieren, vergleichen. Billig, fängt jede einseitige Nebula-Änderung.
4. **Doku nachziehen.** `CopilotDocs/windows-gpu-verification-checklist.md` A5 auf
   „implementiert seit c2e2351d, HW-Sichtprüfung offen" setzen; Roadmap-/Thementext
   korrigieren. `HlslSources.h:19 f.` beschreibt die Fallback-Kopien bereits korrekt.
5. **Fallbacks nicht portieren.** `sky.frag`, D3D11-`kSkyPSHLSL` und die D3D12-Einfarb-
   Nebula sind Fehlerpfade. Die volle Nebula dort von Hand nachzubauen hieße drei
   zusätzliche Kopien pflegen, für einen Pfad, der im Game-Flavour nur bei einem
   kaputten Compile läuft. Höchstens sicherstellen, dass der Fallback laut loggt
   (Vulkan tut das: „falling back to the reduced sky.frag", `VulkanRenderer.cpp:7328`).
   Wenn ein shaderc-freier D3D/Vulkan-Build je gebraucht wird, ist der richtige Weg
   vorkompiliertes SPIR-V/HLSL zur Build-Zeit, nicht eine vierte Handkopie.
6. **Echte Hardware.** Sichtvergleich GL ↔ D3D11/D3D12/Vulkan auf einer Windows-GPU
   (Nebula-Struktur, Farben, Qualitätsstufen) bleibt Aufgabe für einen Menschen; mit
   `HE_DUMP_NEBULA`/`NEBQUALITY`/`NEBCOVER` (siehe `scripts/he_shot.py`) reproduzierbare
   Einstellungen.
