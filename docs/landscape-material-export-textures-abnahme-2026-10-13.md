# Landscape-Material-Export: Abnahme auf D3D und Vulkan (Thema 189, Schritt 3)

Ergebnis: Mit dem Fix aus Schritt 2 (`3aa588df`) sieht die exportierte Testkarte auf D3D11, D3D12 und
Vulkan aus wie im Editor, auch als inkrementeller Re-Export über das kaputte Pak. Voller Build und
ctest sind grün.

**Ursache (Befund, Schritt 1):** Der Editor liest die drei Landschafts-Arrays
`T_Landscape_{Albedo,Normal,Mask}_Array` (0x40F/0x410/0x411) aus dem EngineContent-Download-Cache
(`%APPDATA%\HorizonEngine\EngineContentCache`, dritte `Engine/`-Stufe von
`ContentManager::resolveAbsolutePath`). Der Exporter packte nur Projekt-`Content` und `<exe>/EngineContent`,
und `rewriteRefsForPack` backte die drei Pfad-Refs von `M_AutoLandscape` (0x412) **still** zu `0:0`.
Das Spiel band deshalb die weiße Dummy-Textur, daher die weiß-grauen Sechsecke. Texturdaten, BCn und der
Runtime-Loader waren in Ordnung. **Fix (Schritt 2):** Der Cache wird als dritte, nur auffüllende Wurzel
mitgepackt, und nicht auflösbare Refs sowie UUID-Kollisionen erscheinen als Warnung im Build-Fenster.

## 1. Build und Tests (NN-WS03, Release, `C:\hw189b`, HEAD `4244eb69`)

* `cmake --build C:/hw189b -j 8` (alle Ziele, VS18): 0 Fehler, alle 113 eingebetteten Shader kompilieren.
* `ctest -j 8` mit Scratch-`APPDATA`: **280/282 bestanden, 0 fehlgeschlagen**. 2 übersprungen
  (`runtime_size_app_{basic,advanced}`): Das Deploy hat keine `AppBasic`/`AppAdvanced`, die Ursache liegt in der Umgebung.
* `editor_help_audit.py --check`: 1141/1141.

## 2. Exporte mit dem neuen Editor (`C:\hw189b\deploy\Editor`, `EngineContent` ohne `Textures/`)

| Lauf | Editor-Backend | Cache | Export | Ergebnis |
|---|---|---|---|---|
| D | D3D12 | 3 Arrays | **inkrementell über das kaputte Pak A** (19,1 MB, Refs `0:0`) | nur `M_AutoLandscape` neu gebacken, 48 Einträge, 355,5 MB, Refs `1039:1/1041:1/1040:1`, keine Warnung |
| E | Vulkan | 3 Arrays | frisch | 48 gepackt, keine Warnung; alle 48 `inHash`/`srcHash` = D |
| F | D3D12 | **voller Cache des Menschen** (13 `.hasset`) | frisch | 57 Einträge, 405,9 MB, Refs wie D, genau 1 Warnung: 0x400-Kollision Water vs. `T_Landscape_Grass_Albedo` (Nebenbefund Schritt 1, eigenes Thema) |

Editor-Ansicht auf D3D12 und Vulkan texturiert (Gras/Erde mit Wolkenschatten), gleich wie auf GL
(Schritt 1/2). Bilder: `C:\hw189\shots\editor_{D,E,F}\editor.png`.

## 3. Spiel, Pixelvergleich gegen die Referenz

Referenz B = Arrays als Projekt-Override (Schritt 1, rendert wie der Editor). Alle Spielordner tragen
dieselbe neue Runtime (`out_D`), nur Pak, Manifest und `project.hcfg` sind getauscht.
`docs/spielpfad-postfx-run-game.ps1 -Mode fresh -SettleMs 20000`, 1600×900, Vergleich unterhalb des oberen
Viertels (`docs/spielpfad-postfx-cmp.ps1`). Die Wolkenschatten auf dem Boden sind animiert, deshalb gilt
nicht 0 px, sondern das Rauschen B gegen B2.

| Backend | B gegen B2 (Rauschen) | D gegen B / B2 | A (kaputt) gegen B | Luma B / D / A | `[ERROR]` |
|---|---|---|---|---|---|
| D3D11 | 0,46 | 0,11 / 0,51 | – (Schritt 1: weiß) | 59,8 / 59,8 / – | 0 |
| D3D12 | 0,59 | 0,48 / 0,24 | **74,80** | 59,8 / 59,8 / 128,5 | 0 |
| Vulkan | 0,03 | 0,04 / 0,02 | **74,28** | 59,6 / 59,6 / 128,3 | 0 |
| D3D12, F | – | 0,38 / 0,34 | – | 59,8 | 0 |

Die Abweichung von D liegt auf jedem Backend im Rauschen. Die Negativkontrolle A ist auf demselben Weg
weit davon entfernt, die Messung sieht den Fehler also. In den Spiel-Logs stehen nur Frame-Hitch-, Tick-Budget- und
PlayerController-Warnungen, und die gleich auch in A und B. Bilder: `C:\hw189\shots\g3{A,B,B2,D,F}_{d3d11,d3d12,vk}`.

## Handoff

- **Topic / step:** Thema 189, Schritt 3 von 3 (Abnahme D3D + Vulkan, Build, Tests)
- **Branch / commit:** `claude/landscape-material-export-textures`, dieses Dokument + `scripts/landscape-export-repro/{run_editor_step3.ps1,pkg.py}` (gepusht)
- **Done:** Abnahme bestanden: D3D11, D3D12 und Vulkan im Spiel gleich der Referenz, Editor auf D3D12 und Vulkan
  texturiert. Der Fall des Menschen (inkrementeller Re-Export, voller Cache) ist geprüft. Build und ctest sind grün.
- **Verified:** §1–§3 auf NN-WS03 (RTX 4070). **Not verified:** OpenGL-Spiel mit dem neuen Pak (Schritt 1 maß GL nur
  für das kaputte Pak), macOS/Linux nur über CI. CI-Lauf 38451815670 lief beim Schreiben noch.
- **Traps:** `project.hcfg` trägt die Startszenen-UUID des jeweiligen Exports. Wer ein Pak in einen anderen
  Exportordner kopiert, muss `project.hcfg` mitnehmen, sonst steht im Log „failed to load packed startup scene“ und
  das Bild ist schwarz. Die Läufe in §3 mit Wolkenschatten sind nicht pixelgleich, daher immer gegen das Rauschen B/B2 messen.
- **Code nicht geändert** in diesem Schritt. Offen, aber nicht Teil des Themas: die 0x400-UUID-Kollision.
- **Next steps:** 1. Queen: Thema abschließen und in `release/0.7.0` mergen, sobald CI grün ist.
  2. Eigenes Thema für die 0x400-Kollision (`kEngineWaterMaterialId` vs. `T_Landscape_Grass_Albedo`).
- **Scratch:** `C:\hw189` (Exporte `out_{D,E,F}`, Spielordner `g3_*`, ≈ 3 GB), `C:\hw189b` (Build).
