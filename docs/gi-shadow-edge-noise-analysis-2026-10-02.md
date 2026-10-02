# GI-Schatten: fleckige, wandernde Kanten — Repro und Ursache

Stand 02.10.2026, Zweig `claude/gi-schatten-zerrissen-und-instabil-am-rand-auf-d3d11-d3d12-v`
(Basis `9ed6f816`). Schritt 1 des Themas 131 „GI-Schatten zerrissen und instabil am Rand auf
D3D11/D3D12/Vulkan/OpenGL (nicht Metal)". Analyse mit Hardware-Repro, **kein Engine-Code geändert**.
Gemessen auf NN-WS03 (RTX 4070), Release-Build in einem privaten Baum (`C:/hw131`).

## Kurzfassung

Bei aktivem GI kommt die Sonnen-Schattenkante **nicht** aus der Shadow-Map und **nicht** aus den
DDGI-Probes, sondern aus einer eigenen Kette: ray-getracte Sonnenmaske, 1 Strahl pro Pixel,
halbe Auflösung, dann Temporal-Akkumulation, dann 3×3-Blur. Diese Maske **ersetzt** die CSM
komplett. Die Kette hat zwei Defekte:

1. **Hauptursache für das gemeldete Bild (fleckig, ausgefranst, wechselt jeden Frame):**
   `gi_temporal` clampt die History auf Min/Max der 3×3-Nachbarschaft des **rohen
   1-spp-Signals**. Das Signal ist binär (0/1). Im Halbschatten ist die Nachbarschaft
   regelmäßig zufällig einheitlich (alle 0 oder alle 1). Dann wird die History auf 0 oder 1
   gezogen, und die Akkumulation fängt von vorn an. Dazu kommen der Blend von nur 0.9
   (≈10 effektive Samples), weißes Rauschen aus einem `sin`-Hash und ein Blur von nur 3×3 auf
   halber Auflösung. Zusammen ergibt das genau das Muster aus der Meldung.
   Auf der Hardware belegt: Wird nur der Clamp entfernt, sinkt das p99-Frame-zu-Frame-Flackern
   an der Kante von **8.0 auf 2.9** Graustufen, und das Hochfrequenz-Rauschen geht von 1.10 auf
   0.84 zurück.
2. **Zweiter, laufzeitabhängiger Defekt:** Der Jitter-Seed ist ein unbeschränkt wachsender
   `float` (+1 pro GI-Frame). Er läuft ungebremst in `fract(sin(dot(gid + seed·13.37, …))·43758)`.
   Ab Seed ≈ 1e5 kollabiert der Hash auf der GPU. Bei 144 fps sind das ≈ 11.6 min Editor-Laufzeit,
   bei 60 fps ≈ 28 min. Der Kegel-Jitter steht dann still: Der Halbschatten verschwindet, der
   Schatten wird hart, verschoben und um ~10 Graustufen dunkler. Belegt auf **Vulkan und D3D12**
   (beide HW-RT, Shader-Tausch ohne Rebuild). Schon bei Seed 2e4 (≈ 2.3 min @144 fps) ist der
   Hash messbar degradiert: weniger zeitliche Variation, mehr HF-Struktur.

**Die gesamte Kette ist auf allen fünf Backends textgleich, Metal eingeschlossen.** Eine
Metal-Abweichung, die erklärt, warum Metal „nicht betroffen" sein soll, steht nicht im Code
(siehe §5). Auf diesem Rechner läuft kein Metal, der Metal-Vergleich ist deshalb reine
Code-Lektüre.

## 1. Die Kette, pro Backend

| Stufe | Vulkan | D3D11/D3D12 | OpenGL 4.3 | Metal |
|---|---|---|---|---|
| Strahl-Kernel SW (CPU-BVH) | `shaders/gi_shadow.comp` | `D3D_Shared/HlslSources.h` `kGiShadowCSHLSL` (Z. 714 ff.) | `OpenGLRenderer.cpp` `kGiShadowCS` (Z. 1907 ff.) | `MetalRenderer.mm` `kGISWMSL` |
| Strahl-Kernel HW | `shaders/gi_shadow_hw.comp` (ray_query) | D3D12: `shaders/gi_shadow_hw.hlsl` (DXR 1.1) | — | `MetalRenderer.mm` `kGIShadowMSL` |
| Hash `giHash2` | `gi_shadow.comp:110`, `gi_shadow_hw.comp:74` | `HlslSources.h:727`, `gi_shadow_hw.hlsl:70` | `OpenGLRenderer.cpp:1917` | `MetalRenderer.mm:2117`, `:3052` |
| Seed `+= 1.0f` (nie zurückgesetzt) | `VulkanRenderer.cpp:9486` | `D3D11Renderer.cpp:3426`, `D3D12Renderer.cpp:7252` | `OpenGLRenderer.cpp:5971` | `MetalRenderer.mm:8258` |
| Temporal + 3×3-Clamp | `shaders/gi_temporal.frag:36-48` | `HlslSources.h:835-846` (`kGiTemporalHLSL`) | `OpenGLRenderer.cpp:2015-2027` | `MetalRenderer.mm:2065-2081` |
| History-Gewicht 0.9 | `VulkanRenderer.cpp:9506` | `D3D11Renderer.cpp:3477`, `D3D12Renderer.cpp:7326` | `OpenGLRenderer.cpp:6008` | `MetalRenderer.mm:8315` |
| 3×3-Blur | `shaders/gi_blur.frag` | `HlslSources.h` `kGiBlurHLSL` | `OpenGLRenderer.cpp` | `MetalRenderer.mm:2087` |
| Halbe Auflösung | `VulkanRenderer.cpp:9468` | `D3D11Renderer.cpp:6052`, `D3D12Renderer.cpp:9628` | `OpenGLRenderer.cpp:10471` | `MetalRenderer.mm:15859` |
| Maske ersetzt CSM, lineares Upsample | `scene.frag:502` | `D3D11Renderer.cpp:796`, `D3D12Renderer.cpp:815` | `OpenGLRenderer.cpp:552` | `MetalRenderer.mm:819` |

Konstanten, Formate und Sampler sind überall gleich: Kegelradius = `GILightRadius` (Default 0.5°),
Bias 0.05, tMin 0.02, History RGBA16F, Maske R16F, Temporal mit Point-Sampler, Upsample im
Scene-Pass linear (D3D11 `giLinearClamp`, D3D12 `uGISampler` s3, Vulkan `m_ssaoSampler` LINEAR,
GL `GL_LINEAR`, Metal `m_linearSampler`). Der SYNC-Kommentar oben in `gi_shadow_hw.comp` führt
alle sieben Kernel-Kopien auf.

## 2. Repro

Witness: `HE_DUMP_SHADOWINSTTEST=1` (Boden + 7 Würfel), `HE_DUMP_GI=1`, `SKYTEST` mit
`TOD=0.35`, Kamera `CAMX=-3 CAMY=3 CAMZ=-5 PITCH=-40` (schaut auf die Schattenreihe), Wolken aus,
1280×720 → Maske 640×360. Skripte liegen in `scripts/gi-shadow-repro/`:
`cap.ps1` (ein Capture, frisches APPDATA, optional Config-Vorlage), `ana.py` (Metriken),
`hashsim.py` (float32-Simulation der Kette).

* **Frame N gegen N+1** = zwei Läufe mit `HE_DUMP_FRAMES=60` und `=61`. Seed und Kette sind
  deterministisch, der Rauschboden (zweimal f60) ist auf Vulkan und D3D11 **exakt 0**. Jede
  f60/f61-Differenz ist also echtes Frame-zu-Frame-Flackern.
* **`GILightRadius`**: Beim Default 0.5° ist der Halbschatten in dieser Einstellung nur
  ~1–2 Masken-Pixel breit. Die Kante sieht weich aus, das Rauschen geht im Blur unter
  (Flackern 0.28). Bei **6°** (Config-Vorlage mit `"GILightRadius": 6.0`) tritt das gemeldete
  Bild deutlich hervor: weich, aber fleckig, ausgefranst und dither-artig.
* Metriken (`ana.py`) im Kantenband, d. h. in den Bodenpixeln mit Schattengradient
  (~47 000 px): `flicker` = mittlere |f61−f60| (8-bit-Luminanz) mit p99; `hf` = mittlere
  |Bild − 5×5-Box|, also Hochfrequenz-Rauschen; `L` = mittlere Luminanz.

### 2.1 Alle vier Windows-Backends zeigen es (Stock, 6°)

| Backend (Pfad) | flicker | p99 | hf | L |
|---|---|---|---|---|
| D3D11 (SW-BVH, Compute) | 1.264 | 7.9 | 1.126 | 167.6 |
| D3D12 (DXR 1.1 RayQuery) | 1.283 | 8.0 | 1.104 | 167.3 |
| Vulkan (VK_KHR_ray_query) | 1.287 | 8.0 | 1.102 | 167.3 |
| OpenGL 4.3 (SW, Compute) | 1.755 | 12.0 | 1.404 | 179.4 |

D3D11, D3D12 und Vulkan liegen praktisch gleichauf, und zwar SW- wie HW-Pfad. Das passt zu
textgleichem Code. GL ist am stärksten betroffen und insgesamt heller. Das GL-Delta ist hier
nicht weiter untersucht. Achtung: Ein warmer GL-Program-Binary-Cache in `%APPDATA%` lässt GL
Programme anders bauen; die Captures hier liefen mit frischem APPDATA.

### 2.2 Ursachen-Isolation per Shader-Tausch (Vulkan, 6°)

Deployte `.spv` gegen Varianten getauscht, kein Rebuild. Die Kontroll-Variante (unveränderte
Quelle, selbst kompiliert) ist pixelgleich zum Stock-Deploy. Danach wurden die Originale
zurückkopiert, die MD5 stimmt (`gi_shadow_hw.comp.spv` 7EEC01F9…, `gi_temporal.frag.spv` B4BD06DB…).

| Variante | flicker | p99 | hf | L | Bild |
|---|---|---|---|---|---|
| Stock / Kontrolle | 1.287 | 8.0 | 1.102 | 167.3 | weich, fleckig, ausgefranst, wechselt pro Frame |
| **ohne Neighbourhood-Clamp** (`mix(rawV, hist.a, w)`) | 0.776 | **2.9** | **0.835** | 169.8 | deutlich ruhiger und glatter |
| Seed + 2e4 | 0.914 | 6.1 | 1.363 | 163.2 | Hash beginnt zu degradieren |
| Seed + 1e5 | 0.003 | 0.0 | **2.157** | **157.8** | **hart**, Halbschatten weg, eingefroren |

Bei 0.5° (Default) zeigen sich dieselben Trends, nur kleiner: Stock 0.276 / p99 1.9; ohne Clamp
0.269 / 1.9; Seed + 1e5 → 0.005 / 0.1, hf 3.01 → 3.31.

**D3D12 (DXR, `gi_shadow_hw.cso` per dxc getauscht, Kontrolle byte-gleich zum Deploy):**
Seed + 1e5 → flicker 0.000, hf 2.168, L 157.8. Das ist derselbe Kollaps wie auf Vulkan.
Danach wurde das Original zurückgelegt, die MD5 stimmt (AB790A13…).

### 2.3 Simulation (`hashsim.py`)

float32-Emulation der Kette an einer geraden Kante. Der Clamp verdoppelt den RMS-Fehler gegen
die analytische Penumbra (0.062 gegen 0.028) und erhöht das Flackern um ~40 %, unabhängig vom
Seed. Mit exaktem `np.sin` setzt die Seed-Degradation erst bei ~4e5 sichtbar ein. Die
GPU-`sin` ist für große Argumente deutlich ungenauer, deshalb kollabiert der Hash dort schon bei
1e5 (§2.2). Die Simulation belegt also nur den Clamp-Effekt. Für den Seed gilt der
Hardware-Befund.

## 3. Ursachen im Detail

### A. Neighbourhood-Clamp auf binärem 1-spp-Signal (Hauptursache, alle Backends)

Der Clamp soll Ghosting verhindern, wenn sich der **Verdecker** bewegt (laut Kommentar „Metal
lesson"). Bei 1 Strahl/Pixel ist `raw` aber 0 oder 1. Liegt ein Penumbra-Pixel mit
Sichtbarkeit p vor, sind alle 9 Nachbarn mit Wahrscheinlichkeit ≈ p⁹ + (1−p)⁹ gleich. Bei
p = 0.9 sind das 39 % pro Frame. Dann gilt `nMin == nMax`, die History wird auf 0 bzw. 1 gesetzt
und die Akkumulation beginnt neu. Im äußeren und inneren Halbschatten wird die History so alle
2–3 Frames verworfen. Übrig bleibt nahezu rohes 1-spp-Rauschen, das der 3×3-Blur auf halber
Auflösung nur zu Flecken verschmiert. Die Flecken sind 2 Bildpixel groß (Half-Res, lineares
Upsample), und ihr Muster wechselt jeden Frame (neuer Seed). Daher kommen „zerrissen" und
„wandert".

Verstärkt wird das durch (a) History-Gewicht 0.9 (nur ~10 Frames effektiv, selbst ohne Clamp
bleibt Restrauschen), (b) weißes Rauschen aus `sin`-Hash statt Blue Noise oder
Low-Discrepancy-Folge, (c) Blur nur 3×3 und nicht kantenerhaltend.

### B. Unbeschränkter `float`-Seed im `sin`-Hash (laufzeitabhängig, alle Backends)

`giHash2` rechnet `p = vec2(gid) + seed·13.37`, dann `fract(sin(dot(p, (12.9898, 78.233)))·43758.5453)`.
Das Argument von `sin` wächst linear mit der Laufzeit (Seed 1e5 → ~1.2e8). Ab 2²³ ist das
float32-Ulp ≥ 1 rad. Die GPU-`sin` reduziert solche Argumente nicht genau, und `·43758` verstärkt
den Fehler bis ins Ganzzahlige. Ergebnis auf NVIDIA (Vulkan und D3D12): `xi` ist praktisch
konstant über Frames und Pixel. Alle Strahlen zielen dann in dieselbe Richtung im Kegel, der
Halbschatten verschwindet, der Schatten verschiebt sich (L −10). Weil das langsam einsetzt
(2e4: degradiert, 1e5: kollabiert), sieht die Kante im laufenden Editor mit der Zeit anders aus
als direkt nach dem Start.

Derselbe Hash steckt mit demselben Seed-Muster auch in den **GI-Reflexions-Kernels**
(`OpenGLRenderer.cpp:2381`, `MetalRenderer.mm:3464`, Kopien analog). Ein Fix sollte sie
mitnehmen.

### C. Ausgeschlossen

* **DDGI-Probes:** Die Strahlrichtungen sind deterministisch (Oktaeder-Texel,
  `gi_probe.comp:204-205`), es gibt keine Rotation pro Frame. Die Hysterese ist überall 0.92
  (`D3D11Renderer.cpp:3541`, `D3D12Renderer.cpp:7406`, `VulkanRenderer.cpp:9520`,
  `MetalRenderer.mm:8513`, GL über `uRayParams`). Die Probes liefern nur das indirekte Licht.
  Die Sonnenkante kommt allein aus der Maske.
* **CSM-/PCF-Dithering:** Bei GI an wird die CSM gar nicht gelesen (`if GI … else shadowFactor`).
* **TAA:** Die GI-Kette nutzt `viewProjClean`, also ohne Jitter.
* **Sampler/Upsample, Auflösung, Formate, Blend, Seed-Inkrement:** überall identisch (§1).

## 4. Hinweise für den Fix (Schritt 2 ff., hier nicht umgesetzt)

* **Clamp entschärfen:** gegen eine Statistik, die das Rauschen schon enthält (z. B.
  Mittelwert ± k·σ über 5×5 des rohen Signals oder Min/Max des *geblurrten* Rohsignals), oder
  nur clampen, wenn die Verdeckerbewegung erkannt ist. Der reine Wegfall halbiert das Flackern
  schon (§2.2), bringt aber das Occluder-Ghosting zurück, gegen das der Clamp eingeführt wurde.
* **Seed beschränken und Hash tauschen:** ganzzahliger Frame-Index modulo Periode (z. B.
  `frame & 1023` als `uint`) in einen Integer-Hash (PCG/Wang), besser Blue Noise oder R2-Folge
  pro Pixel. Das beseitigt Defekt B und senkt nebenbei das Rauschen.
* **Alle sieben Kopien** des Kernels plus die Temporal-Kopien (GLSL-Datei, HLSL, GL-String,
  MSL) gleich ändern. Guard: `tests/test_culling.cpp` „GI kernels: the constants the hand-kept
  copies must share" (vergleicht nur die drei Datei-Kopien).
* **Messlatte:** `scripts/gi-shadow-repro/` bei `GILightRadius` 6°, f60/f61, Kantenband-Metriken
  wie oben. Den Seed-Kollaps prüft man über einen Seed-Offset (Shader-Tausch) oder einen
  künftigen Dump-Knopf für den Start-Seed.

## 5. Offene Frage: Warum soll Metal nicht betroffen sein?

Der Metal-Code ist an jeder Stelle der Kette gleich: Kernel, Hash, Seed, Temporal mit
demselben Clamp, Blend 0.9, Half-Res, lineares Upsample. Nach dem Code müsste Metal **Defekt A
genauso** zeigen. Belegen lässt sich keine Erklärung, möglich sind:

1. **Pro-Rechner-Config:** `GILightRadius` liegt in `%APPDATA%`/`~/Library/…` des jeweiligen
   Rechners. Beim Default 0.5° ist der Effekt klein (§2), bei großem Radius groß. Steht der Mac
   auf dem Default, sieht er glatt aus. Am schnellsten prüfbar: dieselbe Config auf beiden
   Rechnern.
2. **Retina-Dichte:** Die 2-Bildpixel-Flecken der Half-Res-Maske sind auf einem 2×-Display
   physisch halb so groß.
3. **Bildrate und Laufzeit (nur Defekt B):** Der Seed wächst pro Frame. 60 Hz auf dem Mac gegen
   144 Hz hier heißt bis zu 2.4× später ins Kollaps-Regime. Ob Apples `sin` für große Argumente
   genauer reduziert, ist ungeprüft.
4. **Spielpfad statt Editor:** Laut Thema 130 fehlen dem D3D/Vulkan-Swapchain-Pfad TAA und Co.
   Wurde Metal im Spiel und Windows im Editor (oder umgekehrt) verglichen, sind die
   Nachbearbeitungen verschieden.

Klären lässt sich das nur auf einem Mac: `scripts/he_shot.py` mit denselben `HE_DUMP_*`-Werten
und `GILightRadius` 6°, f60/f61, dann dieselben Metriken.
