# SSR: Hardware-Verifikation D3D11 / D3D12 / Vulkan (NN-WS03)

Stand 30.09.2026, Thema 109 Schritt 1, Branch `claude/ssr-d3d-vulkan-verify`
(Basis `3bd2c153`). Letzter offener Punkt aus `docs/ssr-cross-backend-plan.md`
§8 Punkt 5: „Nutzer-Verify auf echter Hardware wird pro Backend als offen
gemeldet“.

## Ergebnis in drei Zeilen

| Backend | SSR aktiv? | Befund |
|---|---|---|
| **Vulkan** | ja | Spiegelungen richtig (Richtung, Lage, Rand, Qualitätsstufen, Bewegung). SSR bringt keinen neuen Validierungsfehler. **Sauber.** |
| **D3D11** | **nein** | `ssrTracePS` scheitert zur Laufzeit in `D3DCompile` (**X3511**). SSR bleibt für die ganze Sitzung still aus. **Bug.** |
| **D3D12** | **nein** | Derselbe Shader, derselbe Fehler, dasselbe Bild. **Bug.** |

OpenGL lief als Referenz auf derselben Maschine mit und spiegelt korrekt.
**Die Roadmap kann deshalb noch nicht auf 100 %.** D3D11/D3D12 brauchen erst
den Fix-Schritt unten.

## Umgebung

- NN-WS03, Windows 11, **NVIDIA GeForce RTX 4070, Treiber 610.88**. Alle drei
  Backends liefen nachweislich auf der RTX: `HorizonEditor` erscheint während
  jedes Laufs in `nvidia-smi`. Die AMD-iGPU war nicht beteiligt.
- Release-Vollbau nach `C:/hwSSR` mit `-DDEPLOY_DIR=C:/hwSSR/deploy`. Der
  Editor-Deploy des Menschen blieb unberührt. Alle 40 `.spv` im Deploy sind
  hash-gleich mit dem Build.
- Aufnahme headless über `HE_DUMP_*`, jeweils mit eigenem `APPDATA`. Grundeinstellung:
  `HE_DUMP_SKYTEST=1 HE_DUMP_SSRTEST=1 HE_DUMP_RENDERPATH=0 HE_DUMP_TOD=0.5
  HE_DUMP_COVERAGE=0.2 HE_DUMP_FRAMES=24`.
  - Boden: zusätzlich `PITCH=-8 CAMY=2.5 CAMZ=2`.
  - Wand: zusätzlich `SSRTESTWALL=1 PITCH=-4 CAMY=3 CAMZ=2`.
  - A/B jeweils `HE_DUMP_SSR=0` gegen `HE_DUMP_SSR=1`.
- `RENDERPATH=0` ist Pflicht: Außerhalb von Metal gibt es SSR nur im
  Forward-Pfad (Plan §2.2).

## Messungen

Diff jeweils SSR aus gegen an, nur im Bodenbereich (Zeilen 360–719 von 720).
„Geändert“ heißt: Summe der Kanaldifferenzen > 12. Die Wolken oben laufen auf der
Uhr und sind deshalb ausgeklammert.

| Szene | OpenGL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| Boden, geänderte Pixel | 11 793 | **0** | **0** | 10 957 |
| Boden, Diff-Box | (586,466)–(693,577) | — | — | (587,470)–(692,577) |
| Wand, geänderte Pixel | 118 930 | **0** | **0** | 117 288 |
| Log | — | `SSR ssrTracePS compile failed` | `SSR ssrTracePS compile failed` | `SSR pipelines created (forward, half-res trace)` |

Kontrollen auf Vulkan (GL in Klammern):

- **Rauschboden:** Derselbe Lauf zweimal ergibt 0 geänderte Pixel im Bodenbereich,
  bei Qualität 1 und bei Qualität 2 (GL: ebenso 0). Jede Zahl oben ist damit
  echtes Signal.
- **Richtung:** Die Spiegelung liegt senkrecht unter dem Würfel, ist vertikal
  gespiegelt und deckt sich in x mit dem Würfel (Würfel ≈ x 590–690). Vulkan und
  GL weichen um höchstens 4 px ab. In der Wand-Szene spiegeln sich Wand, Würfel
  und der seitliche Würfel mit derselben Diff-Geometrie wie auf GL.
- **Bildrand** (`HE_DUMP_YAW=-42`, Würfel vom rechten Rand angeschnitten): Die
  Spiegelung blendet zum Rand weich aus, ohne Verschmieren und ohne Umbruch.
  Das ist die SSR-typische Grenze. Diff-Box Vulkan (1078,414)–(1237,673),
  GL (1078,408)–(1247,679).
- **Qualitätsstufen:**
  - Q0 (roh) zeigt die erwarteten Stufenkanten ohne Blur.
  - Q2 (temporal) ist sauber.
  - Beim Stufenwechsel ändert sich auf Vulkan zusätzlich ein schmaler Streifen am
    fernen Bodenrand (y ≈ 335–375, mittlere Abweichung 0,44/255). Dort treffen
    Strahlen streifend den Horizont. Auf GL ist er schwächer vorhanden. Im Bild ist
    nichts zu sehen, das zählt nicht als Befund.
- **Flackern / temporale Stabilität:** Nur ein Stellvertreter-Test, denn ein
  headless Dump zeigt kein Bild-zu-Bild-Flackern.
  - Zwei identische Q2-Läufe sind pixelgleich.
  - `HE_DUMP_MBYAWSTEP=0.3` (eine Bildbewegung auf der temporalen Stufe) ändert
    nur 580 Pixel (GL 594), direkt an der Spiegelung. Kein Geisterbild, kein
    Nachziehen.
- **Rauigkeit** (`SSRTESTROUGH=0.35`, Q2): Die Spiegelung bleibt auf Vulkan wie
  auf GL scharf. Das ist der dokumentierte Stand des Forward-Pfads: kein RoughMix,
  der Vorpass schreibt `roughness = 0` (Plan §4, Punkt 2). Auf Vulkan liegt dabei
  unter dem Würfel ein trapezförmiger, dunklerer Fleck. Er ist **mit SSR aus
  pixelgleich**, gehört also zur Beleuchtung bzw. zum Schatten und nicht zu SSR.
  Er fällt unter den bekannten Boden-Unterschied VK/D3D gegen GL.
- **Validierung:** Vulkan meldet mit SSR an und aus exakt dieselben 10 Meldungen,
  alle aus der bekannten Baseline:
  - `vkCmdUpdateBuffer` im Renderpass
  - Barriere in Subpass 0
  - Leaks bei `vkDestroyDevice`

  SSR fügt nichts hinzu.
- **D3D-Exitcodes** 0xC0000374 / 0xC0000005 nach dem Dump: vorbestehend, siehe
  Baseline, unabhängig von SSR.

Screenshots (halbe Größe, jeweils links aus / rechts an bzw. GL | D3D11 | D3D12 |
Vulkan) in `docs/img/ssr-d3d-vulkan-2026-09-30/`:

- Übersicht: `alle-backends-boden-ssr-an.png`, `alle-backends-wand-ssr-an.png`
- Je Backend: `{opengl,d3d11,d3d12,vulkan}-{boden,wand}-ssr-aus-an.png`
- `vulkan-qualitaet0-qualitaet2-bewegung-rand.png`
- `experiment-d3d-mit-textureLod-wand.png` (siehe unten, nicht committeter Code)

## Der D3D-Bug

**Symptom.** Mit SSR an erscheint im Log genau einmal:

```
[WARN] D3D11Renderer: SSR ssrTracePS compile failed: …ssrTracePS(202,36-75): warning X3570:
gradient instruction used in a loop with varying iteration, attempting to unroll the loop
```

(D3D12 gleichlautend.) `EnsureSSRPipelines` setzt `ssrFailed = true`, und der
Schalter tut für den Rest der Sitzung nichts. Das Bild mit SSR an ist pixelgleich
mit dem ohne. Nutzer sehen „SSR an, keine Spiegelung“ und bekommen keinen Fehler
angezeigt.

**Der eigentliche Fehler steht nicht im Log.** Die Meldung wird hinter der
dritten X3570-Warnung abgeschnitten. Die abschließende Fehlerzeile fehlt deshalb.
Offline nachgebaut:

1. `kSSRTraceFS` → `glslangValidator -V`
2. → `spirv-cross --hlsl --shader-model 50`
3. Register nach `kSSRHlslPins()` umgesetzt (b23→b12, t/s19→8, 20→9, 22→11,
   24→12, 25→13)
4. → `fxc /T ps_5_0 /E main`

Ergebnis:

```
error X3511: unable to unroll loop, loop does not appear to terminate in a timely
manner (115 iterations) or unrolled loop is too large, use the [unroll(n)] attribute …
compilation failed; no code produced
```

`ssrBlur` kompiliert auf demselben Weg fehlerfrei.

**Ursache.** Der Ray-March in `kSSRTraceFS`,
`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1402`
(`for (int i = 0; i < steps; ++i)`), hat zwei Eigenschaften:

- eine Iterationszahl aus dem Uniform `heSSR.cfg.w`,
- `break`-Ausgänge.

In dieser Schleife wird mit **implizitem LOD** gesampelt, also `texture(...)`:
`:1410` (`heGBDepth`), `:1435` (Binärsuche) sowie `:1461`/`:1494` gleich
dahinter. SPIRV-Cross macht daraus `Sample()`, eine Gradient-Instruktion. fxc
(SM 5.0) darf Gradienten in einer Schleife mit variabler Iterationszahl nicht
dynamisch ausführen und versucht deshalb auszurollen. Das scheitert.

- GL, Vulkan und Metal akzeptieren dasselbe. Deshalb fiel es nur auf D3D auf.
- Der Plan hat D3D11/D3D12 ohne Windows-Build und ohne fxc portiert (§6/§7:
  „Übersetzt wurde die `.cpp` nicht“).

**Warum kein Test anschlug:**

- Die SSR-Fälle in `tests/test_material_graph.cpp` prüfen nur, ob SPIRV-Cross
  HLSL-*Text* ausgibt (`trace.ok`) und welche Register darin stehen. Nach
  `D3DCompile`/fxc geht keiner.
- Der Build-Schritt „Validating runtime-compiled shader strings“ (hier:
  105 Shader, alle grün) deckt nur die eingebetteten Strings ab, nicht die
  Ausgabe des Cross-Compilers aus der `MaterialShaderLibrary`.

**Gegenprobe, dass es der einzige Blocker ist.** Lokal und **nicht committet**
wurde eine Zeile hinter `#version 450` in `kSSRTraceFS` eingefügt:

```glsl
#define texture(s_, u_) textureLod(s_, u_, 0.0)
```

Danach neu gebaut:

- Offline: fxc kompiliert den gepinnten Trace fehlerfrei.
- D3D11 und D3D12 melden `screen-space reflection pipeline created`.
- Die Boden-Diff-Box ist dieselbe wie auf Vulkan: (587,470)–(692,577),
  10 962 gegen 10 957 Pixel.
- Direkter Bildvergleich im Bodenbereich:
  - **Boden:** D3D11/D3D12 gegen Vulkan ergibt 1 Pixel über der Schwelle. Der
    SSR-Beitrag (an − aus) weicht im Mittel um 0,008/255 ab, höchstens um 3.
  - **Wand:** 116 701 Pixel, Box ab x=250 statt 248. Gegen Vulkan liegen
    4 055 Pixel über der Schwelle, der SSR-Beitrag weicht im Mittel um 0,125/255
    ab, höchstens um 15. Die Geometrie der Spiegelung ist also gleich, die Bilder
    sind aber nicht pixelgenau gleich.
  - D3D11 und D3D12 sind untereinander pixelgleich.
- Mit `HE_GPU_DEBUG=1` meldet der Debug-Layer auf D3D11 nichts. Auf D3D12 kommt
  nur „ClearRenderTargetView: clear values do not match“, und die kommt auch mit
  SSR aus, ist also vorbestehend.

Danach wurde der Patch zurückgenommen, neu gebaut und bestätigt, dass D3D11 wieder
scheitert. Das Bild ist pixelgleich mit dem ersten Lauf.

**Vorschlag für den Fix-Schritt** (nicht hier umgesetzt, Auftrag: „dokumentieren
und melden“):

1. In `kSSRTraceFS` die Samples innerhalb der Schleifen explizit machen:
   `textureLod(..., 0.0)` (bzw. `texelFetch` für die Tiefe).
   - Die G-Buffer- und Farbziele haben keine Mip-Kette. Die Pixel auf GL,
     Vulkan und Metal sollten deshalb gleich bleiben.
   - Metal-A/B gegen den Referenzshot nach Plan §8 Punkt 3.
   - Der Drift-Fall über die sechs Kaskaden-Kopien betrifft den Trace nicht.
2. Einen Windows-ctest ergänzen, der die HLSL-Ausgabe von `ssrTrace`/`ssrBlur`
   (und sinnvollerweise aller anderen `MaterialShaderLibrary`-Shader, die
   D3D11/D3D12 zur Laufzeit übersetzen) mit `D3DCompile` gegen `ps_5_0`
   übersetzt. Nur so wird dieser Fehler beim Bauen sichtbar statt erst zur
   Laufzeit.
3. Optional: Die Warnmeldung in `EnsureSSRPipelines` (D3D11 `:2577`, D3D12
   `:5527`) sollte die **letzte** Zeile des Fehlerblobs zuerst loggen oder nicht
   kürzen. Sonst verdecken die Warnungen den Fehler.

## Nicht als Bug gewertet (bekannte, dokumentierte Grenzen)

- SSR nur im Editor-Viewport (HDR-Pfad), nicht im Swapchain-Pfad des Spiels
  (C6, Vulkan §5).
- Graph-Materialien bekommen auf D3D11/D3D12 kein SSR (§2.3 Kopf 1). Die
  Testszene nutzt Built-in-Materialien, das Ergebnis oben betrifft also genau den
  Teil, der gehen soll.
- Der Vorpass zeichnet keine Skinned Meshes und keine Partikel. Auf D3D nutzt er
  D16-Tiefe.
- Deferred-Pfad ohne SSR auf GL/VK/D3D (A6).

## Testsuite

`ctest --test-dir C:/hwSSR -j8` (Release, eigenes `APPDATA`): **224/224
bestanden**, 2 übersprungen (`runtime_size_app_*`), 111 s. Die echte
`%APPDATA%\HorizonEngine\config.json` war vorher und nachher hash-gleich. Dieser
Schritt ändert keinen Code, nur diesen Bericht und die Bilder.

## Schritt 2: Fix und Nachmessung (Thema 109)

**Ursache bestätigt, keine zweite.** Der Code bestätigt den Befund oben. In
`kSSRTraceFS` (`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`)
läuft der Ray-March `for (int i = 0; i < steps; ++i)` mit `steps =
int(heSSR.cfg.w)` und `break`-Ausgängen. Darin wird mit `texture()` gesampelt.
SPIRV-Cross macht daraus `Sample()`, und fxc bricht mit X3511 ab.
`EnsureSSRPipelines` (D3D11 und D3D12) setzt danach `ssrFailed = true`.

Bindings, Barriers, Pass-Reihenfolge und Gates wurden nicht getrennt
auditiert. Sie sind durch die Messung unten belegt: Mit dem kompilierenden
Shader spiegelt D3D pixelgenau wie Vulkan. Ein zweiter Fehler dahinter hätte
das verhindert.

**Warum der Fehler im Log fehlte.** `Log.h` begrenzt eine Logzeile auf
`kRingLineSize = 512` Byte. Der fxc-Fehlerblob ist über 53 KB lang, und
`error X3511` steht erst an Offset 53 480, hinter lauter X3570-Warnungen.

**Fix** (Commit auf `claude/ssr-d3d-vulkan-verify`):

1. `kSSRTraceFS`: Alle neun Samples sind jetzt `textureLod(…, 0.0)`, nicht
   nur die in der Schleife. Das ist genau die Variante, die das Experiment in
   Schritt 1 geprüft hat.
   - Keines der SSR-Eingangsziele hat eine Mip-Kette. Die einzigen
     Mip-Erzeuger in den Backends sind die UI-Backdrop-Kopien von GL und Metal.
   - Deshalb ist LOD 0 genau das, was `texture()` vorher gelesen hat.
2. D3D11/D3D12 `EnsureSSRPipelines`: Das Compile-Log beginnt jetzt bei der
   ersten `error X`-Zeile. Das ist zur Laufzeit **nicht verifiziert**, weil es
   dafür einen absichtlich kaputten Shader im Deploy bräuchte.
3. Neuer Test `FXC: the SSR passes compile exactly as D3D11/D3D12 build them`
   (`tests/test_material_graph.cpp`, nur Windows).
   - Übersetzt jede Stufe, die D3D für SSR zur Laufzeit kompiliert, mit
     `D3DCompile`, einmal mit den Flags von Release und D3D11, einmal mit denen
     von D3D12-Debug. Das sind `ssrTrace`, `ssrBlur` und der Reflexions-Vorpass
     (VS und PS).
   - **Negativkontrolle:** Die 9 `SampleLevel(…, 0.0)` werden wieder zu
     `Sample()`, und fxc muss mit X3511 ablehnen. Das tut es, mit derselben
     Meldung „115 iterations“ wie oben.

**Nachmessung** auf NN-WS03 (RTX 4070), Release-Deploy `C:/hwS2`, gleiche
`HE_DUMP_*`-Einstellungen und gleiche Metrik wie oben:

| Szene | OpenGL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| Boden, geänderte Pixel | 11 793 | **10 962** | **10 962** | 10 957 |
| Boden, Diff-Box | (586,466)–(693,577) | (587,470)–(692,577) | (587,470)–(692,577) | (587,470)–(692,577) |
| Wand, geänderte Pixel | 118 931 | **116 701** | **116 701** | 117 288 |
| Log | — | `screen-space reflection pipeline created` | `screen-space reflection pipeline created` | `SSR pipelines created (forward, half-res trace)` |

- D3D11 und D3D12 sind untereinander pixelgleich (Boden und Wand 0 Pixel).
- **Boden** gegen Vulkan: 1 Pixel über der Schwelle.
- **Wand** gegen Vulkan: 4 055 Pixel. Das ist dieselbe Abweichung von höchstens
  15/255, die das Experiment in Schritt 1 gemessen hat. Sie gehört nicht zu
  diesem Fix und bleibt ein offener Punkt, falls jemand D3D gegen Vulkan
  pixelgenau haben will.
- **GL und Vulkan** sind die Kontrolle für die LOD-Änderung. Beide sind gegen
  Schritt 1 unverändert, GL mit ±1 Pixel Rauschen an der Wand.
- In keinem Log steht mehr `compile failed`.
- Bild: `docs/img/ssr-d3d-vulkan-2026-09-30/d3d-nach-fix-boden-wand-ssr-aus-an.png`.

**Tests:** `he_tests` (Release, eigenes `APPDATA`): **4093/4093** Fälle,
557 564 Assertions grün. Die echte `config.json` war vorher und nachher
hash-gleich.

**Offen:**

- **Metal** konnte auf dieser Maschine nicht gegengeprüft werden. Das A/B
  gegen den Referenzshot nach Plan §8 Punkt 3 steht noch aus. Erwartet wird
  pixelgleich, weil es keine Mips gibt.
- Vor diesem Schritt prüfte der FXC-Sweep in `test_material_graph.cpp` nur
  `standardVertex` und die Node-Fragmente. SSR-Trace, -Blur und der
  Reflexions-Vorpass liefen nie durch fxc, jetzt schon. Ob andere
  D3D-Laufzeitpfade Shader aus der `MaterialShaderLibrary` ohne FXC-Test
  übersetzen, zum Beispiel Decals, wurde hier nicht geprüft.
