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

## 6. Schritt 2: Fix (`6d2753f2`) und Regressionstest (`15341488`)

**Fix A, alle fünf Backends:** Das Positions-Target des GI-G-Buffers ist jetzt **RGBA32F**. Die
Normalen bleiben RGBA16F. Es bleibt bei Formatänderungen, Shader und Konsumenten sind unverändert.
Alle Leser holen die Position per `texelFetch`/`Load`/`read` oder über einen Point-Sampler, die
Filterbarkeit von 32F spielt also keine Rolle (geprüft für Schattenkernel SW und HW, Temporal und
À-trous auf allen fünf Backends).

Kamerarelativ in fp16 wurde verworfen: Der Fehler wüchse dann mit dem Abstand zur Kamera statt zum
Ursprung, und Terrain ab ~128 m vor der Kamera bekäme die Bänder zurück.

Formatstellen:

- Vulkan: `kGiGBufPosFormat`, Attachment 0 des G-Buffer-Render-Pass und das Image
- D3D11: die Textur; RTV und SRV folgen ihr per `nullptr`-Desc
- D3D12: eine Konstante `kGiGBufPosFormat` für Textur, `RTVFormats[0]` und alle sechs SRVs (Slots
  0, 8, 11 und `atrousBase+1`)
- GL: `GL_RGBA32F`
- Metal: `kGiGBufPosFormat` für Textur und Pipeline; der Normalen-Descriptor ist abgetrennt.
  **Metal ist nicht gebaut** (kein Mac).

**Fix B, nur Vulkan/D3D11/D3D12:** Der GI-Zweig des eingebauten Shaders addiert jetzt wie GL und
Metal `ambient * albedo * (1 - metallic)`. `RenderWorld::ambient` kommt dafür als neues, zuletzt
angehängtes vec4 in Frame-UBO bzw. PerFrame-cbuffer. Der Nicht-GI-Zweig ist unberührt.

Offen gelassen, weil es nicht zur Streifen-Ursache gehört: Der GI-Term selbst nutzt auf Vulkan/D3D
`base * kd` mit `kd = (1 - F0)(1 - met)`, auf GL/Metal `albedo * (1 - metallic)`. Das ist ein
Unterschied von rund 4 % (`F0` = 0,04).

### Messung

RTX 4070, Release `C:\hw159`, `cap159.ps1` wie in §1. Verglichen werden der Vorher-Deploy (Stand
`d1b8c36a`, mit LANDY-Zeugen, `C:\hw159\pre2`) und der Fix-Deploy.

Streifenenergie = mittlere |Δ Luminanz| zwischen vertikal benachbarten Pixeln, Box Terrain
(x 8–92 %, y 78–98 %) bzw. Kugel (x 37–63 %, y 17–47 %).

| Fall | GL vorher → nachher | Vulkan | D3D11 | D3D12 |
|---|---|---|---|---|
| Mountain y = 300, GI an, Kugel | 6,09 → 0,45 | 5,12 → 0,64 | 6,11 → 0,55 | 6,11 → 0,55 |
| Layers y = 300, GI an, Terrain | 4,34 → 0,15 | 5,22 → 0,15 | 5,22 → 0,15 | 5,22 → 0,15 |
| Layers y = 300, GI an, Kugel | 5,58 → 0,41 | 5,58 → 0,41 | 5,63 → 0,41 | 5,63 → 0,41 |
| Mountain y = 0, GI an, Kugel | 0,45 → 0,45 | 0,73 → 0,64 | 0,64 → 0,55 | 0,64 → 0,55 |

Vorher gegen nachher, mean |Δ| / max |Δ| (8-Bit-Stufen):

| Fall | GL | Vulkan | D3D11 | D3D12 |
|---|---|---|---|---|
| GI aus, y = 300 | **0 / 0** | **0 / 0** | **0 / 0** | **0 / 0** |
| GI aus, y = 0 | **0 / 0** | **0 / 0** | **0 / 0** | **0 / 0** |
| GI an, y = 0 | 0,019 / 103 (0,17 % px) | 5,78 / 65 | 7,78 / 82 | 7,78 / 82 |
| GI an, y = 300 | 52,0 / 143 | 35,5 / 130 | 48,8 / 143 | 48,8 / 143 |

- **GI aus ist auf allen vier Backends bytegleich.**
- **GL bei y = 0, GI an:** Abweichung nur auf der 1-Pixel-Penumbrakante des Kugelschattens. Der
  Strahlursprung verschiebt sich um Bruchteile eines Zentimeters. Rauschboden derselben Aufnahme
  zweimal: max |Δ| 1, vorher wie nachher. Die Abweichung ist deterministisch und optisch nicht zu
  sehen. Das ist die erwartete Wirkung von fp32, kein Fehler.
- **Vulkan/D3D bei y = 0, GI an:** Die Aufhellung kommt aus Fix B, das Terrain geht von 177/165 auf
  186/177 (GL 220). Sie ist gewollt.
- **Mountain y = 300 gegen y = 0 nach dem Fix:** Die mittlere Terrain-Luminanz ist auf jedem Backend
  gleich (GL 219,6, Vulkan 186,2, D3D 176,5). Die Höhe spielt also keine Rolle mehr.
- **D3D-Masken-Tausch (offen aus §5) indirekt beantwortet:** Die Layers-Szene läuft ganz über den
  Graph-Pfad (`heLitP`), den Fix B nicht berührt. Ihre Streifen verschwinden auch auf D3D11/D3D12.
  Auf D3D genügt also Fix A, und die Ursache ist dieselbe wie auf Vulkan/GL.
- **Validation:**
  - D3D12 mit `HE_GPU_DEBUG=1`: 0 Fehler, nur die bekannte Warnung „Ignoring InitialState“ der Buffer.
  - Vulkan: vorher und nachher dieselben Meldungen (`gi_layout`, `mat_ubo`), keine neue.

### Regressionstest

`scripts/he_vk_imagetests.py`, Fall `gi_stripes`:

- Szene: Layers-Terrain + Graph-Kugel auf y = 300, GI aus/an
- neues Urteil `stripes`: Streifenenergie je Box ≤ `STRIPE_MAX` = 2,0

Positivkontrolle auf der RTX (`--require-device RTX`):

| Deploy | gi_on Terrain / Kugel | gi_off | Urteil |
|---|---|---|---|
| vor dem Fix | 5,273 / 5,632 | 0,24 / 0,419 | **FAIL** |
| mit Fix | 0,149 / 0,412 | 0,24 / 0,419 | ok |

Auf lavapipe in CI ist der Fall noch nicht gelaufen. Die Rundung float→half dort kann vom Treiber
abhängen. Bei Rundung zur nächsten Zahl ist die halbe ULP bei y = 300 aber 0,125 m und damit immer
noch größer als 5 cm.

`he_tests` (Release, scratch APPDATA): 4234/4236. Die zwei Fehlschläge sind die bekannten
`inspector_ui`-Clipboard-Fälle auf NN-WS03.

## 7. Schritt 3: Bildvergleich vorher/nachher und Abnahme

Stand `4e2e4d90`, NN-WS03 (RTX 4070), 07.10.2026. Alle Zahlen sind mit
`scripts/gi-stripes-repro/ana159.py` aus den Aufnahmen gerechnet und damit nachrechenbar.
Aufgenommen wurde mit `scripts/gi-stripes-repro/run159.ps1`, das `cap159.ps1` mit denselben
Einstellungen wie §1 aufruft.

### Aufbau

- **Build:** Release, `C:\hw159`, `HE_BUILD_TESTS=ON`, alle Targets
  (`cmake --build C:\hw159 -j8` ohne `--target`), EXIT 0. Release ist auch der Build-Typ des
  Windows-CI-Jobs (`ci.yml`: `-DCMAKE_BUILD_TYPE=Release`). Die 108 eingebetteten HLSL/GLSL-Shader
  kompilieren. Danach wurden HorizonGame und HorizonEditor neu gelinkt, sonst bleiben die
  Spiel-Kopien alt. Der Folge-Build hatte nichts mehr zu tun.
  - md5 von `HorizonRendering.dll`, `scene.frag.spv`, `gi_gbuf.frag.spv` und `gi_shadow.comp.spv`
    ist an allen vier Stellen gleich: Build-Baum, `deploy/Editor`, `deploy/Game` und
    `deploy/Editor/Game`. **Der exportierte Spiel-Deploy enthält den Fix also**, gemessen ist
    das aber nur über die Hashes, nicht im Bild.
- **Drei eingefrorene Deploys:**

  | Name | Inhalt | Ort |
  |---|---|---|
  | pre | `d1b8c36a`, vor dem Fix | `C:\hw159\pre2`; Aufnahmen aus S2, ergänzt um `*_s3` |
  | fixA | HEAD, aber Fix B per temporärem Patch entfernt (die drei `+ ambient …`-Zeilen in `scene.frag`/D3D11/D3D12, UBO-Feld bleibt); nicht committet | `C:\hw159\s3fixA` |
  | post | HEAD `4e2e4d90` | `C:\hw159\s3post` |

  Mit fixA lassen sich die Wirkungen von Fix A und Fix B getrennt messen.
- **Szenen:** Mountain (eingebautes Terrain-Material) + Graph-Kugel auf y = 300 und y = 0, und
  Layers (Graph-Terrain) + Graph-Kugel auf y = 300, jeweils mit GI an und aus. Die Backends sind
  OpenGL, Vulkan, D3D11 und D3D12.
- **Rauschboden:** Die bekannte Schwelle 0,25 mittlere |Δ| ist das Rauschen zwischen zwei GI-Läufen
  auf Hardware (`ci-software-vulkan-icd-analyse-2026-10-03.md`, Zeile GI). Gemessen wird deshalb
  dieselbe Aufnahme zweimal (`post` gegen `post_r2`).

### Ergebnis

Diffs als mittlere |Δ| / max |Δ| (8-Bit-Stufen) / Anteil geänderter Pixel:

| Vergleich | OpenGL | Vulkan | D3D11 | D3D12 |
|---|---|---|---|---|
| **GI aus** y = 300, pre → post | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** |
| **GI aus** y = 0, pre → post | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** |
| **GI aus** Layers y = 300, pre → post | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** | **0 / 0 / 0 %** |
| Rauschen GI an y = 0, post → post_r2 | 0,000 / 1 / 0,08 % | 0,002 / 1 / 0,51 % | 0,000 / 1 / 0 % | 0,000 / 1 / 0 % |
| Rauschen GI an y = 300, post → post_r2 | 0,000 / 1 / 0,05 % | 0,001 / 1 / 0,23 % | 0,000 / 1 / 0 % | 0,000 / 1 / 0 % |
| Rauschen GI an Layers, post → post_r2 | 0,001 / 1 / 0,30 % | 0,000 / 1 / 0,01 % | 0 / 0 / 0 % | 0 / 0 / 0 % |
| pre (S2) → pre (S3), GI an y = 0 | 0,000 / 1 / 0,10 % | 0,001 / 1 / 0,39 % | 0 / 0 / 0 % | 0,000 / 3 / 0,00 % |
| **Fix A allein**, GI an y = 0 (pre → fixA) | 0,019 / 103 / 0,45 % | 0,013 / 52 / 0,53 % | 0,017 / 66 / 0,34 % | 0,017 / 66 / 0,34 % |
| **Fix B allein**, GI an y = 0 (fixA → post) | 0,000 / 1 / 0,03 % | 5,771 / 30 / 64,8 % | 7,762 / 37 / 64,8 % | 7,762 / 37 / 64,8 % |
| Fix B allein, GI an Layers (Graph-Pfad) | 0,001 / 1 / 0,21 % | 0,000 / 1 / 0,01 % | 0 / 0 / 0 % | 0 / 0 / 0 % |
| A+B, GI an y = 300, pre → post | 51,96 / 143 / 52,4 % | 35,54 / 130 / 69,5 % | 48,78 / 143 / 69,5 % | 48,78 / 143 / 69,5 % |
| A+B, GI an Layers y = 300, pre → post | 18,56 / 147 / 46,9 % | 15,68 / 147 / 44,1 % | 15,75 / 147 / 44,1 % | 15,75 / 147 / 44,1 % |
| Höhe egal? GI an, **pre** y = 300 → y = 0 | 51,96 / 143 / 52,8 % | 29,78 / 130 / 54,6 % | 41,02 / 143 / 52,8 % | 41,02 / 143 / 52,8 % |
| Höhe egal? GI an, **post** y = 300 → y = 0 | 0,028 / 149 / 0,79 % | 0,024 / 117 / 3,0 % | 0,021 / 103 / 0,83 % | 0,021 / 103 / 0,83 % |

Streifenenergie, Terrain-Box / Kugel-Box. Definition und Boxen sind dieselben wie bei
`he_vk_imagetests.py`, Grenze 2,0:

| Fall | Backend | pre | fixA | post | post_r2 |
|---|---|---|---|---|---|
| Mountain y = 300, GI an | GL | 0,053 / **6,094** | 0,002 / 0,452 | 0,002 / 0,452 | 0,002 / 0,452 |
| | Vulkan | 0,105 / **5,123** | 0,035 / 0,735 | 0,027 / 0,638 | 0,027 / 0,638 |
| | D3D11 = D3D12 | 0,056 / **6,114** | 0,013 / 0,640 | 0,010 / 0,553 | 0,010 / 0,553 |
| Layers y = 300, GI an | GL | **4,340 / 5,576** | 0,149 / 0,413 | 0,149 / 0,413 | 0,149 / 0,413 |
| | Vulkan | **5,216 / 5,579** | 0,148 / 0,410 | 0,148 / 0,410 | 0,148 / 0,410 |
| | D3D11 = D3D12 | **5,219 / 5,626** | 0,148 / 0,411 | 0,148 / 0,411 | 0,148 / 0,411 |
| GI aus, alle Fälle | alle | pre = post (Terrain 0,000–0,240, Kugel 0,419–0,575) | | | |

Mittlerer Wert der Terrain-Box (0–255), Mountain, GI an:

| | y = 0 pre / fixA / post | y = 300 pre / fixA / post |
|---|---|---|
| GL | 219,6 / 219,6 / 219,6 | **112,5** / 219,6 / 219,6 |
| Vulkan | 176,8 / 176,8 / 186,2 | **113,9** / 176,8 / 186,2 |
| D3D11 = D3D12 | 164,6 / 164,6 / 176,5 | **80,7** / 164,6 / 176,5 |

**Achtung beim Lesen der Streifenenergie:** Auf dem Mountain-Terrain sind die falschen Schatten
breite Höhenbänder, keine Zeilen. Die Streifenenergie misst nur Nachbarzeilen und sieht diese
Bänder kaum (pre 0,05–0,10). Für das Mountain-Terrain sind deshalb der mittlere Wert
(GL 112,5 → 219,6) und „y = 300 gegen y = 0" die Zeugen. Die Kugel und das flache Layers-Terrain
zeigen Zeilenstreifen, dort greift die Streifenenergie.

Per Auge geprüft, Kontaktbogen `C:\hw159\s3_sheet.png` (Spalten GL, Vulkan, D3D11, D3D12):

- pre, Mountain y = 300: auf allen vier Backends Höhenbänder auf dem Terrain und waagrechte
  Streifen auf der Kugel
- post: sauber
- Layers: pre mit Zeilenstreifen auf Terrain und Kugel, post sauber

### Abnahme gegen das Kriterium

- **Streifen mit GI an weg, auf Vulkan, D3D11 und D3D12 (und GL):**
  - Die Kugel-Streifenenergie fällt von 5,1–6,1 auf 0,41–0,64, das Layers-Terrain von 4,3–5,2 auf
    0,15. Das liegt auf dem Niveau der jeweiligen GI-aus-Aufnahme (Kugel 0,42–0,58, Terrain
    0,02–0,24).
  - Nach dem Fix spielt die Höhe keine Rolle mehr: y = 300 gegen y = 0 ergibt mittlere |Δ| 0,021–0,028
    statt 30–52.
  - Die Rest-Pixel, Diff-Maske GL in `C:\hw159\s3_heightdiff_gl.png`: Abweichungen über 8 Stufen
    betreffen 0,05–0,10 % der Pixel (GL 899, Vulkan 499, D3D11 676) und liegen fast nur auf der
    1-Pixel-Penumbrakante des Kugelschattens.
  - Alles andere sind verstreute Pixel mit ±1 Stufe im Himmel und auf der Kugel, wie sie bei
    anderer Kamerahöhe zu erwarten sind.
- **Rauschen im Rahmen von 0,25 mittlere |Δ|:**
  - Lauf-zu-Lauf mit GI an liegt bei ≤ 0,002 und max 1 Stufe, auf allen vier Backends.
  - Fix A allein ändert das Bild am Referenzpunkt y = 0 um 0,013–0,019 (0,34–0,53 % der Pixel),
    Diff-Maske Vulkan in `C:\hw159\s3_fixA_vk.png`:
    - Abweichungen über 8 Stufen gibt es nur auf der Penumbrakante des Kugelschattens (GL 601,
      Vulkan 361, D3D11 517 Pixel).
    - Dazu kommen 2–8 Stufen am Kantensaum und blasse Bögen mit ±1 Stufe auf dem Terrain.
      Vermutlich sitzen sie an den früheren fp16-Rundungsstufen von x/z, die bei y = 0 bis ~120 m
      Abstand noch Zentimeter ausmachen.
    - Der Strahlursprung rückt dort um Bruchteile eines Zentimeters. Das ist die erwartete
      Wirkung von fp32.
  - Beides liegt deutlich unter 0,25.
  - Die 5,77 (Vulkan) bzw. 7,76 (D3D) bei y = 0 sind **ganz Fix B**: fixA → post, gleich groß auf
    y = 300. Das ist eine flächige Aufhellung um +9,4 bzw. +11,9 Stufen auf dem Terrain, max 30/37.
    - Diff-Bild Vulkan in `C:\hw159\s3_fixB_vk.png`.
    - In der Terrain-Box: Mittel +9,34, Streuung 0,51, Streifenenergie des Diffs 0,049, also
      glatt.
    - Die Korrelation mit der Helligkeit vorher ist −0,89, dunkle Pixel gewinnen also mehr Stufen.
      Das passt zu einem konstanten linearen Zusatzterm hinter einem komprimierenden
      Tonemapper, und daher ist der Zuwachs im Kugelschatten am größten.
    - Die Graph-Kugel bleibt bei 0, weil Fix B nur den eingebauten Pfad betrifft.
    - Die Streifenenergie der Kugel-Box ändert sich dabei kaum (Vulkan 0,735 → 0,638). Die Box
      enthält in den Ecken Terrain.
  - Fix B ist gewollt und kein Rauschen: Ambient-Boden wie GL/Metal, Entscheidung der Königin §5.
  - Auf GL und auf dem reinen Graph-Pfad (Layers) ist Fix B nachweislich wirkungslos (≤ 0,001).
- **GI aus bitgleich:** pre gegen post ist auf allen vier Backends in allen drei Szenen 0 / 0 / 0 %.
  fixA gegen post bei GI aus ist ebenfalls 0.
- **OpenGL unverändert am Referenzpunkt:** pre → post bei y = 0, GI an: 0,019 mittlere |Δ| auf
  0,44 % der Pixel (Penumbra, Fix A), bei GI aus 0. Weit vom Ursprung (y = 300) ändert sich GL
  absichtlich: Die Bänder verschwinden (§5).
- **D3D11 und D3D12** liefern in jeder Zeile identische Zahlen.
- **Validation:**
  - Vulkan (Layer aktiv laut Log): pre und post haben dieselben 30 `[ERROR]` je Mountain-Aufnahme.
    GI aus hat ebenfalls 30, die Fehler kommen also nicht vom GI-Pfad. Layers hat 0.
  - D3D12 mit `HE_GPU_DEBUG=1` (Debug-Layer + DRED laut Log) bei y = 300, GI an: 0 Fehler, nur die
    bekannte Warnung „Ignoring InitialState". Für D3D11 meldet das Log keinen Debug-Layer, dafür
    gibt es also keine Aussage.
- **he_tests** (Release, Vordergrund, scratch APPDATA, `HE_NET_LOOPBACK_ONLY=1`, 258 s):
  - 4234/4236 Fälle, 586 096/586 103 Assertions.
  - Die zwei Fehlschläge sind `inspector ui: a component header's right-click menu copies, resets
    and pastes the component` (`test_inspector_ui.cpp:325`) und `inspector ui: the Replication
    section …` (`:413`). Das sind die bekannten Clipboard-Fälle auf NN-WS03, nicht GI.
- **CI** `37619120681`, Stand `4e2e4d90`: grün auf Windows, Linux, lavapipe und macOS. Auf
  lavapipe ist `gi_stripes` gelaufen: gi_on Terrain 0,146, Kugel 0,411; gi_off 0,241 / 0,418. Alle
  6/6 Fälle bestanden.

### Was noch aussteht (ehrlich)

- **Metal:** nicht gemessen, auf NN-WS03 gibt es keinen Mac. Belegt ist nur, dass
  `MetalRenderer.mm` im macOS-CI-Job kompiliert. Fix A ändert dort nur das Format
  (`kGiGBufPosFormat`). Ob Metal vorher Bänder bei y = 300 hatte und nachher keine, und ob es bei
  y = 0 unverändert bleibt, prüft erst ein Lauf auf Apple-Hardware (`cap_metal.sh`-Muster aus
  `scripts/gi-shadow-repro`).
- **lavapipe-Negativkontrolle:** Der CI-Fall `gi_stripes` ist nach dem Fix grün. Ob er auf lavapipe
  **vor** dem Fix rot gewesen wäre (Rundungsart float→half im Treiber), ist ungeprüft. Dafür
  bräuchte es einen Wegwerf-Zweig mit zurückgenommenem Fix in CI.
- **Exportiertes Spiel (Swapchain-Pfad):** Nur die Hashes sind gleich (DLL und `.spv` in
  `deploy/Game` und `deploy/Editor/Game`), ein Bild aus dem exportierten Spiel ist nicht
  aufgenommen.
- **Hardware-Breite:** Gemessen ist nur auf einer NVIDIA RTX 4070 (Treiber von NN-WS03). AMD und
  Intel, besonders deren float→half-Rundung beim RT-Write, sind nicht gemessen. Mit fp32 entfällt
  die Rundung als Ursache. Eine falsche Formatbehandlung auf einem anderen Treiber wäre aber
  nur im Bild zu sehen.
- **Szene des Melders:** unbekannt. Die Abnahme stützt sich auf die Dump-Szenen auf y = 300.
- **Vulkan-Basis:** `fa73986b` (Thema 154) ist weiterhin nicht auf main (Stand `origin/main
  6866923d`). Nach dessen Merge ändern sich die absoluten Vulkan-GI-Werte, nicht aber der
  Höhen-Befund.
- **Nicht behoben, nur notiert:** die ~4-%-Drift im GI-Term (`base*kd` gegen `albedo*(1-met)`) und
  der fehlende Ambient-Boden im Nicht-GI-Zweig von Vulkan/D3D (§4).
