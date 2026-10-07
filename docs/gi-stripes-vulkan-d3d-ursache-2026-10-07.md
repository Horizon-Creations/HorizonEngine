# GI an: schwarze Streifen, Repro und Ursache (Thema 159, Schritt 1)

Stand: 07.10.2026, Zweig `claude/gi-an-schwarze-streifen-auf-vulkan-und-d3d11-d3d12-beheben` auf
main `6866923d` (plus Zeugen-Commit `710f4960`). Gemessen auf NN-WS03 (RTX 4070), Release-Build
`C:\hw159`, Aufnahmen in `C:\hw159\cap` (nicht eingecheckt). Noch kein Fix.

## Kurzfassung

1. **Die Streifen kommen aus der Sonnenschattenmaske der GI, nicht aus dem Probe-Feld.** Bei GI an
   ersetzt die raygetracte Maske die CSM. Der Strahlursprung jedes Maskenpixels ist die Weltposition
   aus dem GI-G-Buffer plus 5 cm Normal-Offset. Dieser G-Buffer speichert die **absolute**
   Weltposition als **Half-Float (RGBA16F)**. Ab |Koordinate| ≈ 100 m ist der Rundungsfehler größer
   als 5 cm. Der Ursprung liegt dann unter der eigenen Oberfläche, und der Strahl trifft die eigene
   Fläche. Das ergibt falsche Schatten entlang der Höhenlinien: Bänder auf dem Terrain und waagrechte
   Streifen auf Kugeln.
2. **Die Prämisse „nur Vulkan/D3D" stimmt für diesen Mechanismus nicht.** OpenGL zeigt in derselben
   Szene dieselben Bänder. Metal hat dasselbe Format und denselben Offset, ist aber nicht gemessen
   (kein Mac). Spezifisch für Vulkan/D3D ist nur, **wie dunkel** die falschen Schattenbänder werden.
   Der GI-Zweig des eingebauten Shaders hat dort keinen Ambient-Boden (`+ ambient * diffuse`), GL und
   Metal haben ihn. Auf D3D ist ein Schattenband deshalb rund 30 Stufen dunkler als auf GL. Das
   erklärt „schwarz" auf D3D gegen „grau" auf GL.
3. Der Verdacht aus `gi-ddgi-material-path-analysis-2026-10-02.md` §7.2 („dunkle
   GI-Schattenstreifen im Probe-Kachelmuster") ist derselbe Effekt. Die Szene dort
   (`HE_DUMP_LANDSCAPELAYERS`) liegt fest auf y = 300. Der Material-Fix hat die Bänder nur mit
   Probe-Licht aufgehellt. Mit dem Probe-Raster haben die Streifen nichts zu tun: Sie folgen
   Höhenlinien, und auf der Kugel liegen sie waagrecht. Beides kann ein 8,7-m-Kachelmuster nicht
   erzeugen.

## 1. Repro

`scripts/gi-stripes-repro/cap159.ps1`, eine Aufnahme pro Aufruf:

- privater Deploy und frische APPDATA
- `HE_SKY_TIME=10`, TOD 0,35, Wolken aus
- AA, SSAO, SSR, Bloom, DOF und Motion Blur aus
- `HE_DUMP_RENDERPATH=0`, 60 Frames

Szenen:

- `-Scene mountain`: `HE_DUMP_MOUNTAINTEST=before`, 240 m welliges Terrain mit dem
  **eingebauten** Terrain-Material
- `-Scene layers`: `HE_DUMP_LANDSCAPELAYERS=1`, 100 m flaches Terrain mit einem
  **Graph-Material**
- dazu jeweils `HE_DUMP_MATERIALTEST=1`, eine Graph-Material-Kugel 8 m vor der Kamera

Neuer Zeugen-Schalter `HE_DUMP_LANDY` (`710f4960`): Er setzt die Höhe beider Dump-Landschaften, die
bisher fest auf 300 stand. Die Kamera kommt per `-Extra @{HE_DUMP_CAMY=<y+6>}` mit.

```powershell
scripts\gi-stripes-repro\cap159.ps1 -Name vulkan_y300_gi1 -Rhi Vulkan -Gi 1
scripts\gi-stripes-repro\cap159.ps1 -Name vulkan_y0_gi1 -Rhi Vulkan -Gi 1 -Extra @{HE_DUMP_LANDY='0'; HE_DUMP_CAMY='6'}
```

| Fall (Mountain-Szene + Kugel) | GL | Vulkan | D3D11 | D3D12 |
|---|---|---|---|---|
| y = 300, GI aus | sauber | sauber | sauber | sauber |
| y = 300, GI an | **Bänder + Kugelstreifen** | **dito** | **dito** | **dito** |
| y = 0, GI an | sauber | sauber | sauber | sauber |
| y = 0, GI aus | sauber | sauber | sauber | sauber |

Das Bild ist auf dem Stand `6866923d` und auf einer Kopie des Thema-157-Deploys gleich.

**Höhen-Dosis (Vulkan, Mountain + Kugel, GI an):**

| y | 0 | 50 | 100 | 150 | 200 | 300 |
|---|---|---|---|---|---|---|
| Bild | sauber | sauber | erste dünne Höhenlinien | Bänder | Bänder | Bänder, am stärksten |

Das passt zur Half-Float-Schrittweite gegen den 5-cm-Offset:

| \|y\| | Half-ULP | halbe ULP (Rundung zur nächsten) | Erwartung |
|---|---|---|---|
| [32, 64) | 0,031 m | 0,016 m | sauber |
| [64, 128) | 0,0625 m | 0,031 m | nur Kanten und Hänge (dazu x/z-Fehler) |
| [128, 256) | 0,125 m | 0,0625 m | Bänder |
| [256, 512) | 0,25 m | 0,125 m | Bänder |

Auch das **flache** Graph-Terrain auf exakt y = 300,0 zeigt dichte Zeilenstreifen (Vulkan und
D3D12). Das spricht dafür, dass die float→half-Wandlung beim Render-Target-Write auf dieser GPU zur
Null rundet: 299,99997 wird zu 299,75. Der Fehler reicht dann bis zu einer vollen ULP. Belegt ist
das nicht, es ist eine Vermutung. Bei GI an treten beide Pfade auf: das eingebaute Terrain
(Mountain) und die Graph-Materialien (Kugel, Layers-Terrain). Beide lesen dieselbe Maske.

**Helligkeit eines Schattenbands** (mittleres RGB, Box x 100–300, y 640–700, Mountain y = 300, GI an):

| | Schattenband | beleuchtet (x 1100–1250, y 500–540) | y = 0 GI an | y = 0 GI aus |
|---|---|---|---|---|
| GL | 100/110/128 | 218/221/221 | 218/220/221 | 215/218/219 |
| Vulkan | 107/115/127 | 180/184/187 | 174/178/181 | 145/161/179 |
| D3D11 = D3D12 | **70/80/95** | 163/168/172 | 160/165/169 | 145/161/179 |

## 2. Ursachen-Isolation

- **Masken-Tausch (nur Vulkan):** In `gi_shadow.comp` und `gi_shadow_hw.comp` wird
  `imageStore(uOut, …, vec4(sunVis))` durch `vec4(1.0)` ersetzt (glslc
  `--target-env=vulkan1.2`, nur die zwei `.spv` im Deploy-Kopie getauscht). Danach sind Terrain und
  Kugel streifenfrei, es bleibt nur der Sonnenschatten weg. Das Probe-Feld ist dabei unverändert.
  Die Streifen stammen also aus der Maske.
- **Höhe:** dieselbe Szene auf y = 0 ist auf allen vier Backends sauber.
- D3D11/D3D12 sind **nicht** per Shader-Tausch isoliert. Der Schluss für D3D stützt sich auf dasselbe
  Format, denselben Offset und dasselbe Verhalten (sauber bei y = 0, gestreift bei y = 300).

## 3. Ursache mit Datei und Zeile

**A. Half-Float-Weltposition als Strahlursprung (alle Backends)**

- G-Buffer-Format RGBA16F:
  - Vulkan `VulkanRenderer.cpp:9769`
  - GL `OpenGLRenderer.cpp:5918`
  - D3D11 `D3D11Renderer.cpp:3267`
  - D3D12 `D3D12Renderer.cpp:6995` (SRV `:7051`)
  - Metal `MetalRenderer.mm:8286`
- Geschrieben wird die absolute Weltposition:
  - `gi_gbuf.frag:8`
  - D3D `HlslSources.h:589`
  - GL `OpenGLRenderer.cpp:1770`
- Ursprung = Position + `N * 0.05`, tMin 0,02:
  - `gi_shadow.comp:168` (lokale Lichter `:189`)
  - `gi_shadow_hw.comp:130`/`:151`
  - `gi_shadow_hw.hlsl:126`/`:150`
  - D3D11 `HlslSources.h:798`/`:768`
  - GL `OpenGLRenderer.cpp:2064`/`:2087`
  - Metal `MetalRenderer.mm:2344`/`:2376`

**B. Kein Ambient-Boden im GI-Zweig des eingebauten Shaders (nur Vulkan/D3D, macht A „schwarz")**

- Vulkan `scene.frag:467-469`, D3D11 `D3D11Renderer.cpp:761-763`, D3D12
  `D3D12Renderer.cpp:786-788`: `irr * base * kd * giIntensity + ambSpec * …`, ohne Boden.
- GL `OpenGLRenderer.cpp:591-593` und Metal `MetalRenderer.mm:799-802` addieren
  `ambient * diffuseColor`. Laut Kommentar ist das die „never-black guarantee".
- Der Graph-Pfad (`heLitP`) hat den Boden auf allen Backends.

**Nicht die Ursache, geprüft oder ausgeschlossen:**

- Probe-Atlas-Lesen, Chebyshev und Kachel-UV sind in allen Kopien zeilengleich (Code-Vergleich).
- Der Masken-Tausch lässt das Probe-Feld unberührt, und die Streifen verschwinden trotzdem.
- Den Code-Verdacht „Albedo 1,0 → INF im Irradiance-Atlas" erklärt diese Szene nicht. Er ist nicht
  weiter verfolgt.
- Binding-Gates: Der Graph-Pfad zeigt das Bild bei y = 0 sauber, also wirkt das Gate dort.

## 4. Nebenbefunde (nicht Thema 159)

- **Vulkan-Resolve fehlt auf main:** `fa73986b` (Thema 154, Materialfarbe vor `updateGiAccel`) liegt
  nur auf `claude/vulkan-ddgi-probe-feld-ohne-farbbounce`. Auf main sind alle Vulkan-GI-Instanzen
  weiß. Daher ist das Vulkan-Schattenband (107) heller als D3D (70). Nach dem Merge von 154 verschiebt
  sich die Vulkan-Basis von Schritt 3.
- **GI aus:** Auf GL ist das Terrain viel heller (215) als auf Vulkan/D3D (145/161/179). Auch der
  Nicht-GI-Zweig hat auf Vulkan/D3D keinen `ambient`-Boden. „GI aus bitgleich" heißt für Schritt 2:
  diesen Zweig nicht anfassen.

## 5. Für Schritt 2 (Entscheidung nötig)

Ursache A sitzt in **allen fünf** Backends. Ein Fix nur für Vulkan/D3D lässt GL und Metal ab ~100 m
Höhe gestreift. Ein Fix für alle fünf verletzt die Abnahme „Metal/OpenGL unverändert".

Mögliche Fixes für A, jeweils pro Backend gleich:

- Position im G-Buffer kamerarelativ speichern und im Kernel die Kameraposition (fp32) wieder
  addieren
- oder RGBA32F für die Position
- oder den Offset mit der Größe der Koordinate skalieren (Notbehelf)

Fix für B: den Ambient-Boden im GI-Zweig von `scene.frag`/D3D11/D3D12 nachziehen, wie bei GL/Metal.
Das ändert nur GI an.

**Entscheidung der Königin (07.10.2026, Anfrage 17):**

- A kommt auf **allen fünf** Backends: kamerarelative Position im G-Buffer oder RGBA32F.
- B kommt nur auf Vulkan/D3D.
- Das Kriterium „Metal/OpenGL unverändert" gilt am Referenzpunkt nahe dem Ursprung (y = 0).
- **Für das Review:** Nach Fix A sehen GL und Metal bei GI an weit weg vom Ursprung (ab ~100 m)
  absichtlich anders aus, nämlich ohne die falschen Bänder. Das ist die Korrektur, keine Regression.

Ungeprüft:

- Metal (kein Mac)
- D3D-Masken-Tausch
- die Rundungsart float→half
- die Szene des Melders: Höhe und Koordinaten unbekannt; die Vermutung ist, dass sie bei |y| oder
  |x|, |z| ≳ 100 m liegt
