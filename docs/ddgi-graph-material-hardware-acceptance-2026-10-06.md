# DDGI im Graph-Material-Pfad: Hardware-Abnahme D3D11/D3D12/Vulkan (Thema 148)

Stand: 06.10.2026, NN-WS03 (NVIDIA RTX 4070, Vulkan 1.4.341 mit Validation, D3D12 mit DXR 1.1).
Zweig `claude/ddgi-graph-material-pfad-visuelle-hardware-abnahme-auf-d3d11` auf `9b00bfb1`
(= main vor #93/#94, die nur Kommentare und Widget-Undo ändern). Release-Build, privater Deploy
`C:\hw148`. Nachzug zu Thema 120 / PR #79 (`docs/gi-ddgi-material-path-analysis-2026-10-02.md`,
§8: „Nach dem Merge nicht erneut auf Hardware geprüft").

## 0. Kurzfassung

| Backend | Graph-Material liest die Probes (Gate-Gegenprobe) | Farbbounce auf Graph-Material | Graph-Kugel gegen GL | Urteil |
|---|---|---|---|---|
| D3D11 | ja (25,8 Stufen) | ja, 19,9 (GL 20,2) | 0,15 (GI an), 0,01 (GI aus) | **abgenommen** |
| D3D12 | ja (25,8 Stufen) | ja, 19,9 (GL 20,2) | 0,15 (GI an), 0,01 (GI aus) | **abgenommen** |
| Vulkan | ja (18,5 Stufen) | **nein, 0,00** | 12,4 (GI an), 0,01 (GI aus) | Material-Pfad abgenommen, **Probe-Feld fehlerhaft** (s. §3) |

- Der in PR #79 gebaute Material-Pfad (Gate `giProbe.y`, Atlanten auf t17/t18 bzw. Binding 17/18)
  funktioniert auf allen drei Backends auf echter Hardware. Mit hart abgeschaltetem Gate fällt der
  Probe-Anteil überall weg, mit GI aus ist alles bitgleich.
- Auf D3D11/D3D12 stimmt das Graph-Material mit GL praktisch überein (0,15 von 255 im Mittel).
- **Neuer Befund, nicht im Material-Pfad:** Auf Vulkan enthält das Probe-Feld keinen Farbbounce
  vom Boden. Der rote und der graue Boden ergeben bitgleiche Kugeln, beim Graph-Material **und** beim
  eingebauten Shader, mit HW- und mit SW-Strahlen. Die Graph-Materialien lesen das Feld korrekt.
  Das Feld selbst ist auf Vulkan falsch. Details und Repro in §3.

## 1. Aufbau

### 1.1 Zeugen

- **Bleed-Szene** (`HE_DUMP_MATERIALTEST=1` + `HE_DUMP_GIBLEED`, aus Thema 120):
  - Graph-Kugel (Lerp orange/blau über Fresnel, Metallic = sin(Time)) 8 m vor der Kamera.
  - `GIBLEED=3/4`: roter bzw. grauer Bodenblock unter der **Graph**-Kugel.
  - `GIBLEED=1/2`: derselbe Block unter einer **eingebauten** Kontrollkugel 6 m rechts davon.
  - Gemessen wird die untere Kugelhälfte, die dem Boden zugewandt ist.
- **Bemaltes Terrain** (`HE_DUMP_LANDSCAPELAYERS=1`, Draufsicht aus 392 m): ein Graph-Material
  (Landscape-Layer-Blend).
- Jeweils GI an/aus (`HE_DUMP_GI`).

### 1.2 Einstellungen

`scripts/ddgi-material-repro/cap148.ps1`:

- TOD 0,5 (Bleed) bzw. 0,4 (Terrain), keine Wolken, kein Wolkenschatten.
- Forward-Pfad, SSR, GI-Reflexionen, SSAO, AA, Bloom, DOF und Motion Blur aus. Sie hängen nicht
  am Probe-Feld und unterscheiden sich pro Backend aus anderen Gründen (z. B. fehlt Forward-SSR für
  Graph-Materialien auf D3D, s. Thema 149).
- `HE_SKY_TIME=6.2832` friert den Time-Knoten ein, sin(2π) ≈ 0. Die Kugel ist also ein Dielektrikum,
  und der diffuse Probe-Term ist voll sichtbar.
- 60 Settle-Frames, frische APPDATA pro Aufnahme, 1280×720.

`run148.ps1` fährt die Matrix, `ana148.py` misst. Die Masken stehen im Kopf von `ana148.py`.

### 1.3 Gegenprobe ohne zweiten Baum

`ctl` ist derselbe Build, nur mit `FillMaterialGIProbe` (`LightPacking.cpp`) als
`giProbe[1] = 0`. Das entspricht dem Stand vor PR #79 auf allen Backends. Lokal gepatcht,
inkrementell gebaut, Deploy weggelegt, Patch zurückgedreht, **nicht committet**. Zwischen
`post` und `ctl` unterscheidet sich nur `HorizonRendering.dll` (SHA-256 `2B527E7B…` gegen
`AC0AA9C6…`, `HorizonEditor.exe` gleich).

Rauschboden: eine zweite `post`-Serie (`_r2`). Alle Aufnahmen sind über die Masken bitgleich
(|Δ| = 0,00).

## 2. Messwerte

8-Bit-sRGB-Werte des Dumps. „Bleed" = Mittel über die Maske von (R−G) beim roten Boden minus
(R−G) beim grauen Boden.

### 2.1 Farbbounce auf der unteren Kugelhälfte

| Backend | Graph, GI an | Graph, GI an, Gate 0 | Graph, GI aus | eingebaut, GI an | eingebaut, GI an, `ctl` | eingebaut, GI aus | Graph/eingebaut |
|---|---|---|---|---|---|---|---|
| GL | 20,19 | 0,00 | 0,00 | 46,76 | 46,77 | 0,00 | 0,43 |
| D3D11 | 19,87 | 0,00 | 0,00 | 69,56 | 69,56 | 0,00 | 0,29 |
| D3D12 | 19,87 | 0,00 | 0,00 | 69,56 | 69,56 | 0,00 | 0,29 |
| Vulkan | **0,00** | 0,00 | 0,00 | **0,00** | 0,00 | 0,00 | — |
| D3D12, `HE_GI_FORCE_SW=1` | 19,87 | | | 69,56 | | | |
| Vulkan, `HE_GI_FORCE_SW=1` | **0,00** | | | **0,00** | | | |

- Das Gate wirkt nur auf Graph-Materialien. Die eingebaute Kugel ist in `post` und `ctl` gleich.
- Die eingebaute Kugel bounct auf D3D stärker als auf GL (69,6 gegen 46,8). Die Rot-minus-Grau-
  Metrik hebt den bekannten Grundabstand eingebaut-D3D gegen GL (~10,5 auch bei GI aus) auf. Das ist
  also ein echter Paritätsunterschied, aber im **eingebauten** Shader (dessen DDGI-Gewichtung),
  nicht in diesem Pfad. Die Graph-Kugel trifft GL fast genau. Nicht weiter untersucht.

### 2.2 Gate-Gegenprobe (`post` gegen `ctl`, Graph-Kugel, mittlere |Δ|)

| Backend | rot, GI an | grau, GI an | rot, GI aus | Rauschen (`post` gegen `post_r2`) |
|---|---|---|---|---|
| GL | 25,92 | 17,65 | 0,00 | 0,00 |
| D3D11 | 25,81 | 17,67 | 0,00 | 0,00 |
| D3D12 | 25,81 | 17,67 | 0,00 | 0,00 |
| Vulkan | 18,54 | 18,54 | 0,00 | 0,00 |

Auf allen vier Backends liest das Graph-Material die Probes nur bei GI an. Auf Vulkan ist der
Beitrag beim roten und beim grauen Boden gleich groß, das Feld trägt also keine Bodenfarbe (§3).

### 2.3 Gegen GL (`post`, mittlere |Δ| über die Maske)

| Backend | Graph rot, GI an | Graph rot, GI aus | Graph grau, GI an | eingebaut rot, GI an | eingebaut rot, GI aus |
|---|---|---|---|---|---|
| D3D11 | 0,15 | 0,01 | 0,10 | 19,15 | 10,46 |
| D3D12 | 0,15 | 0,01 | 0,10 | 19,15 | 10,46 |
| Vulkan | 12,39 | 0,01 | 9,26 | 44,02 | 10,46 |

D3D11 und D3D12 liefern über die Maske dieselben Werte, die Dateien sind aber nicht md5-gleich.

### 2.4 Bemaltes Terrain (Graph-Material, Draufsicht)

| Backend | Mittel GI aus | Mittel GI an | GI an: `post` gegen `ctl` | Pixel > 2 | GI aus: `post` gegen `ctl` | Anhebung `post`/`ctl` |
|---|---|---|---|---|---|---|
| GL | 181,20 | 157,89 | 11,20 | 96,1 % | 0,00 | +7,6 % |
| D3D11 | 181,20 | 153,43 | 13,13 | 96,2 % | 0,00 | +9,4 % |
| D3D12 | 175,46 | 148,29 | 13,06 | 96,2 % | 0,00 | +9,7 % |
| Vulkan | 175,46 | 148,30 | 13,07 | 96,2 % | 0,00 | +9,7 % |

- Das Probe-Licht hebt das Terrain auf allen Backends an. Bei GI aus ist der Pfad unsichtbar
  (bitgleich).
- D3D11 gegen GL: GI aus 0,00, GI an 25,6. Das kommt fast ganz von den bekannten waagrechten
  GI-Schattenmasken-Streifen, die pro Backend anders liegen (`ctl` gegen `ctl`: 33,3). Es kommt
  nicht vom Material-Pfad: Der reine Probe-Anteil (`post`−`ctl`) weicht nur um 7,8 ab.
- **D3D12 und Vulkan zeigen keine Bemalung, nur Layer 0** (`painted-terrain.png`). Das ist alt
  und hat mit DDGI nichts zu tun:
  - Vulkan: Binding 14 `heLandscapeWeights` fehlt im Material-Layout. Der Fix steht in PR #95
    (Thema 143) und ist noch nicht auf main.
  - D3D12: Slot 9 (t14) der Material-SRV-Vorlage bleibt eine Null-View
    (`D3D12Renderer.cpp:8394`). Bekannt aus `docs/terrain-vegetation-gap-audit-2026-09-26.md:68`,
    bisher ohne eigenes Thema.
  - D3D12 und Vulkan stimmen deshalb miteinander überein (0,00 bei GI aus, 0,01 bei GI an).

### 2.5 Bilder

- `docs/img/ddgi-material-hw-2026-10-06/graph-sphere-bleed.png`: untere Kugelhälfte, Zeilen
  GL/D3D11/D3D12/Vulkan, Spalten „GI an rot", „GI an grau", „GI an rot, Gate 0", „GI aus rot".
- `docs/img/ddgi-material-hw-2026-10-06/painted-terrain.png`: Terrain GI aus / GI an /
  GI an mit Gate 0.

### 2.6 Validation und Log

- **Vulkan:** nur die bekannten Meldungen von main: `vkCmdUpdateBuffer`/`vkCmdPipelineBarrier`
  im Render-Pass (Fix in #96) und `[SSAO blur]`-Layout (Fix in #97). Die Graph-Kugel meldet kein
  einziges Binding, das bemalte Terrain nur Binding 14 `heLandscapeWeights` (Fix in #95). Die
  PR-#79-Lücke (15–18/32/33) bleibt also zu.
- **D3D12:** Die Matrix lief ohne Debug-Layer, er ist im Release-Build nur mit `HE_GPU_DEBUG=1`
  an. Nachgeholt: Bleed `b3`, GI an, mit `HE_GPU_DEBUG=1` (Debug-Layer + DRED). Gemeldet wird nur
  das bekannte harmlose „Ignoring InitialState UNORDERED_ACCESS" für Buffer, und das Bild ist
  bitgleich zum Lauf ohne Layer (max |Δ| 0).
- **Refit mitten im Lauf** (`HE_DUMP_GIREFIT=1`, Terrain, GI an; Gitter 15×4×15 → 22×4×11,
  Atlanten neu). Das prüft den Lebensdauer-Pfad aus PR #79 (D3D12 `retireGiProbeAtlas` →
  Null-Views in den Slots 12/13, Vulkan-Writes 17/18):
  - D3D12 mit Debug-Layer: nur dieselbe InitialState-Meldung.
  - Vulkan: nur Binding 14.
  - D3D11: läuft durch, nur Bildbeleg.
- **D3D11:** hat keinen Debug-Layer, belegt ist dort nur das Bild.
- Alle vier Backends loggen dasselbe Gitter: `GI probe grid 6x5x6 (180 probes), spacing 4`.

## 3. Neuer Befund: Vulkan-Probe-Feld ohne Farbbounce

**Symptom:** Mit GI an sind auf Vulkan die Kugeln über rotem und über grauem Boden über die Maske
bitgleich (Bleed 0,00). Das gilt für das Graph-Material wie für den eingebauten Shader und mit
HW-Ray-Query wie mit erzwungenem SW-Pfad (`HE_GI_FORCE_SW=1`). Auf GL, D3D11 und D3D12 tönt der
rote Boden die Unterseite deutlich (20 bzw. 47–70 Stufen).

**Eingrenzung:**

- **Nicht der Material-Pfad.** Das Graph-Material liest das Feld (Gate-Gegenprobe 18,5), und die
  eingebaute Kugel zeigt dasselbe Nullsignal.
- **Die Instanzfarbe auf der CPU ist nur statisch geprüft, nicht zur Laufzeit.**
  `VulkanRenderer::updateGiAccel` füllt `GIInstanceGpu::baseColor` aus `obj.baseColor`, mit
  demselben Code wie D3D11 (`D3D11Renderer.cpp:3098`), und auf D3D11 bounct es. Aber
  `GiInstanceSurface.h` sagt ausdrücklich, dass `RenderObject::baseColor` nur die Meshfarbe ist
  und eine Materialfarbe nachgeschlagen werden muss (nur GL nutzt den Helfer). Ob der rote Wert
  auf Vulkan im Instanz-Buffer ankommt, ist nicht gemessen.
- **Die Probe-Kette an sich läuft auf Vulkan.** Beim Terrain liegen Vulkan und D3D12 mit GI an
  0,01 auseinander, und die Anhebung `post`/`ctl` ist gleich (13,07 gegen 13,06). Kernel,
  Dispatch und Atlas-Update liefern dort also dasselbe Feld wie D3D12. Default-Cubes stehen im
  Vulkan-GI-BVH: Ihre GI-Schatten sind in den Themen 134/142 auf Vulkan gemessen worden.
- **Shader-Quelltext:** `gi_probe.comp` und `gi_probe_hw.comp` multiplizieren
  `giInsts[hitInst].baseColor` in Sonnen-, Licht- und Multi-Bounce-Term.
- **Exakt 0,00 statt „schwächer"** heißt: Kein Probe-Strahl liefert vom Boden einen
  albedo-abhängigen Beitrag.
- **Hauptverdacht, nicht belegt:** Die Materialfarbe des Bodens (`MaterialComponent` →
  `baseColor`) erreicht die Vulkan-GI-Instanz nicht, oder die Instanz wird gebaut, bevor das
  Material aufgelöst ist, und nie nachgezogen. Weniger wahrscheinlich ist der Kernel.

**Repro:**

```
scripts/ddgi-material-repro/cap148.ps1 -Name v3 -Rhi Vulkan -Gi 1 -Scene bleed -Bleed 3
scripts/ddgi-material-repro/cap148.ps1 -Name v4 -Rhi Vulkan -Gi 1 -Scene bleed -Bleed 4
```

Auf Vulkan sind beide Aufnahmen auf der Kugel gleich, gegen GL ergibt `ana148.py` Tabelle A 20,19.

**Vorschlag:** ein eigenes Thema „Vulkan: DDGI-Probe-Feld ohne Farbbounce" mit diesem Zeugen als
Abnahme.

1. `baseColor` der Boden-Instanz in `updateGiAccel` auf Vulkan und D3D11 loggen bzw. vergleichen
   (rot muss (1, 0,05, 0,05) sein).
2. Erst danach am Kernel ansetzen: `gi_probe*.comp` mit `radiance = albedo` am Treffer und ohne
   Schattenstrahl. Die `.spv` lässt sich ohne Rebuild im Deploy tauschen
   (`Editor/Shaders/gi_probe*.spv`).

## 4. Was diese Abnahme nicht abdeckt

- **Metal** als zweite Referenz: Auf NN-WS03 nicht verfügbar. GL ist die Referenz.
- **Deferred-Pfad:** Gemessen wurde nur Forward (`RENDERPATH=0`). Der Deferred-Renderer läuft auf
  D3D/Vulkan noch nicht (Thema 150).
- **SSR, Sky-Cube und AO** für Graph-Materialien auf D3D/Vulkan: absichtlich aus, die gehören zu
  Thema 149 / PR #98.
- **Bemalung auf D3D12** (t14 Null-View): dokumentiert, nicht behoben, s. §2.4.
