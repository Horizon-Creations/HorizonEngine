# Graph-Material Sky-Cube + AO auf D3D11/D3D12/Vulkan: Abnahme (Thema 149)

Stand: 05.10.2026, Zweig `claude/graph-material-ssr-skyenv-ao-auf-vulkan-d3d11-d3d12-visuelle`.
Schliesst die zwei Punkte, die PR #88 (Thema 126) unter „Nicht belegt“ offen liess. Gemessen auf
NN-WS03 mit RTX 4070. Mit `nvidia-smi` belegt: D3D11, D3D12 und Vulkan liefen dort als `C+G`-Prozess.

## 0. Kurzfassung

- **Orientierung der Cube-Faces auf D3D: gemessen, stimmt.** Zwei Belege:
  - Ein WARP-Test liest jedes Texel eines 4×4-Cubes entlang der Bake-Richtung zurück, 96 von 96.
    Gespiegelte Uploads als Gegenprobe verfehlen 96 von 96.
  - Auf Hardware gleicht eine Chromkugel auf D3D11, D3D12 und Vulkan dem GL-Bild bis auf 1,6–1,7/255.
    Eine absichtlich gespiegelte Cube weicht um 22,1/255 ab.
- **AO-Bild: stimmt.** SSAO macht die Graph-Kugel auf allen vier Backends nur dunkler, nie heller,
  und zwar am Kontakt mit dem Boden (bis 20–38/255). Im WARP-Test folgt das Pixel genau dem
  AO-Puffer auf t16 und nicht dem SSR-Ergebnis, das die eingebaute Pass dort hält.
- **WARP-Test für `BindSkyEnvAndAO` / `RestoreBuiltinSkyEnvAOSlots`: ergänzt** (`test_material_graph.cpp`).
- **Nebenbefund, kein Fehler dieses Themas:** Graph-Materialien haben auf D3D11 und D3D12 kein
  Forward-SSR. Die untere Hälfte der Chromkugel zeigt dort den Boden-Ton der Cube statt der
  Spiegelung. Das ist bekannt und so gewollt (`D3D11Renderer.cpp`, Kommentar an `lit.ssr`):
  `heSSRFwd` liegt auf t31/s31, ausserhalb der 16 Sampler von D3D11. Offen in
  `docs/ssr-cross-backend-plan.md` §2.3 (C5).

## 1. WARP (he_tests, `test_material_graph.cpp`)

**„D3D11: a graph material draw reads the sky cube on t15/s15 and the AO on t16, and gives t16 back
to the SSR result (WARP)“**

Der Lit-Graph ist so gewählt, dass im Pixel genau ein Term steht:
- Basis weiss, metallic 1, roughness 1.
- Damit liefert `heLitP` `0,4 · heSkyEnv(N) · ssao`.
- Die sechs Cube-Faces tragen sechs verschiedene Farben mit Wert 2,0.

| Fall | erwartet | gemessen |
|---|---|---|
| sechs Normalen ±X/±Y/±Z, AO weiss | je die eigene Face, 204 je Kanal | alle sechs |
| +Z, AO 128 auf t16 | (0,0,102) | (0,0,102) |
| +Z, t16 bleibt auf dem SSR-Puffer (64) | (0,0,51) | (0,0,51) |
| +Z, t15 leer, fog.z 1 | schwarz | (0,0,0) |
| +Z, fog.w 0 (AO gebunden, ignoriert) | (0,0,204) | (0,0,204) |
| +Z, fog.z 0 | schwarz | (0,0,0) |
| nach Restore | t15 leer, t16 = SSR | ja |

Die D3D11-Debug-Schicht war an und meldete im Draw nichts.

**„D3D11: every sky-cube texel samples back along the direction the bake gave it (WARP)“**

- Jedes Texel eines 4×4-Cubes trägt (face, s, t).
- Der Upload läuft über `HE::d3d11mat::UploadSkyEnvCube`, also über dieselbe Funktion wie im Renderer.
- Ein Punkt-Sample entlang `SkyEnvFaceDirection(face, u, v)` an jeder Texelmitte muss genau dieses
  Texel liefern: 96 von 96.
- Gegenproben: Zeilen umgekehrt und Spalten umgekehrt verfehlen je 96 von 96.

D3D12 lädt den Bake im selben Speicherlayout hoch, ebenfalls ohne Spiegelung:
Slice f, Zeile t, eng gepackt (`D3D12Renderer.cpp`, `updateSkyEnvCube`). D3D12 hat aber keinen
eigenen WARP-Test. Dort ist der Hardware-Lauf unten der Beleg.

## 2. Hardware (deployter Editor, `HE_DUMP_*`)

**Zeugenszene:** `HE_DUMP_SSRTEST` mit eingebautem Spiegelboden und rotem Würfel, dazu
`HE_DUMP_MATERIALTEST=chrome|matte` (neu). Die Graph-Kugel mit r = 2,5 liegt per
`HE_DUMP_MATTESTPOS=-3.5,2.5,-8` (neu) auf dem Boden.

Gemeinsame Einstellungen:
- `SKYTEST`, Kamera (0, 3.5, 6), Pitch −12
- Forward (`RENDERPATH=0`)
- AA, DoF, Motion Blur, Bloom und Wolken aus
- 16 Frames, frisches APPDATA pro Lauf

**Rauschboden:** Zwei gleiche Läufe sind auf allen vier Backends bytegleich (|d| = 0,00).

**Sky-Cube auf der oberen Hälfte der Chromkugel, Abweichung von GL (mittleres |d| je Kanal):**

| | morgens (TOD 0,29) | abends (TOD 0,71) | Sonnenglanz morgens / abends |
|---|---|---|---|
| OpenGL | – | – | (534,257) / (400,253) |
| D3D11 | 1,68 | 1,73 | (534,257) / (400,254) |
| D3D12 | 1,68 | 1,73 | (534,257) / (400,254) |
| Vulkan | 1,58 | 1,61 | (534,257) / (400,254) |
| D3D11, Upload absichtlich zeilengespiegelt | **22,08** | | (533,259) |

Der Horizont auf der Kugel ist auf allen Backends waagrecht und durchgehend. Der Glanz wandert
morgens nach rechts und abends nach links. Nach dem Zurückbauen der Gegenprobe ist der D3D11-Lauf
wieder bytegleich mit dem ersten.

**AO, matte Graph-Kugel in der Dämmerung (TOD 0,26), SSAO an gegen aus, untere Kugelhälfte:**

| | mittlere Abdunklung | max | heller gewordene Pixel |
|---|---|---|---|
| OpenGL | 0,71 | 34 | 0 |
| D3D11 | 0,47 | 20 | 0 |
| D3D12 | 0,47 | 20 | 0 |
| Vulkan | 1,08 | 38 | 0 |

- Das Differenzbild zeigt eine Sichel genau am Bodenkontakt.
- Ein SSR-förmiges Muster wie beim Fehler vor Thema 126 gibt es nicht.
- Der Betrag ist klein, weil `heLitP` das Himmels-Ambient mit 0,35 gewichtet.
- Die Stärke unterscheidet sich je Backend so, wie sich deren eigene SSAO-Puffer unterscheiden.
  Am hellen Mittag fällt der Effekt auf 0,2–0,4.

**Boden-SSR neben dem Graph-Draw:** Der eingebaute Spiegelboden zeigt die Spiegelung des roten
Würfels auf allen vier Backends, SSR an gegen aus |d| ≈ 66–68. Der Restore von t16 lässt die
eingebauten Draws nach dem Graph-Material also intakt.

**Logs:**
- D3D11 und D3D12: keine Fehler. Die Debug-Schichten waren in diesen Läufen nicht an, ein Urteil
  „Debug-Schicht sauber“ gibt es daher nicht.
- Vulkan: nur die bekannten `vkCmdUpdateBuffer`/Barrier-im-Renderpass-Meldungen. PR #96 behebt
  sie, er ist in diesem Zweig noch nicht enthalten.

## 3. Nachmessen

```powershell
# private Deploy-Baumstruktur (-DDEPLOY_DIR), dann je Lauf in EINEM Aufruf:
$env:APPDATA="<frisch>"; $env:HE_COLLAB_OFFLINE="1"
$env:HE_DUMP_PATH="x.bmp"; $env:HE_DUMP_QUIT="1"; $env:HE_DUMP_RHI="D3D11"
$env:HE_DUMP_SKYTEST="1"; $env:HE_DUMP_SSRTEST="1"
$env:HE_DUMP_MATERIALTEST="chrome"; $env:HE_DUMP_MATTESTPOS="-3.5,2.5,-8"
$env:HE_DUMP_TOD="0.29"; $env:HE_DUMP_CAMY="3.5"; $env:HE_DUMP_CAMZ="6"; $env:HE_DUMP_PITCH="-12"
$env:HE_DUMP_RENDERPATH="0"; $env:HE_DUMP_SSR="1"; $env:HE_DUMP_SSAO="1"
$env:HE_DUMP_AA="0"; $env:HE_DUMP_DOF="0"; $env:HE_DUMP_MOTIONBLUR="0"; $env:HE_DUMP_BLOOM="0"
$env:HE_DUMP_CLOUDMODE="0"; $env:HE_DUMP_COVERAGE="0"; $env:HE_DUMP_FRAMES="16"
& <deploy>\Editor\HorizonEditor.exe
```

Die Messbereiche im 1280×720-Bild:
- Kugelmitte (478, 270), r ≈ 112
- obere Hälfte: y < 262
- Kontakt: y > 330
- Würfelspiegelung: x 610–672, y 415–485

Ohne die Overrides für DoF, Motion Blur, Bloom und AA war das erste Probebild weichgezeichnet.
Welcher davon das verursacht, ist nicht einzeln geprüft.
