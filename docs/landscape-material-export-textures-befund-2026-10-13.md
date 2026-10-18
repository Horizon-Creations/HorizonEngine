# Landscape-Material-Export: Befund (Thema 189, Schritt 1)

Im Editor ist das Auto-Landschaftsmaterial texturiert, im exportierten Spiel ist die Landschaft
weiß-grau mit Sechseck-Muster. Ursache: **Die drei Landschafts-Arrays kommen nie ins Pak.** Der Editor
liest sie aus dem SFTP-Cache, der Exporter packt diesen Cache nicht, und der Packer backt die
unauflösbaren Pfade still zu einer Null-UUID. Noch nichts gefixt.

Das ist die Lücke, die `docs/auto-landscape-material-textures.md` §20.4 Punkt 2 schon als
„nicht umgesetzt“ führt („Exporter muss den Cache zusätzlich packen“, §18.4). Es ist also keine
Regression, sondern ein fehlendes Stück, das seit Thema 177 Schritt 7 trifft: Seitdem sind die
Platzhalter aus git entfernt und die echten Texturen liegen nur noch auf dem Server.

**Antwort auf die Frage des Themas:** Es liegt weder an den komprimierten Texturdaten noch am
Runtime-Loader. Dieselben Dateien rendern, einmal gepackt, auf D3D11 und Vulkan wie im Editor
(Kontrolle B, §3). Der Fehler sitzt davor: Der Export packt unvollständig. Man sieht das an den fehlenden
Manifest-/TOC-Einträgen 0x40F–0x411 und an den Material-Refs, die zu `0:0` gebacken sind.

## 1. Symptom

| Lauf | Landschaft | Boden-Mittelwert RGB (y 600–900) |
|---|---|---|
| Editor (GL), Arrays nur im Cache | texturiert (Gras/Erde) | – (Screenshot) |
| Spiel des Menschen (`HEProject.hpak` vom 12.10.), D3D11 | weiß-grau, Sechsecke | 166 176 198 |
| dasselbe Pak, Vulkan | identisch | 166 176 198 |
| dasselbe Pak, GL (Backend des Menschen laut Log) | weiß-grau, Sechsecke | 143 154 179 |
| Kontrolle B (Arrays im Pak), D3D11 | texturiert, wie Editor | 68 63 46 |
| Kontrolle B, Vulkan | texturiert, wie Editor | 68 63 46 |

* Die Texturen **fehlen**, statt vertauscht zu sein. Jeder Renderer findet für die drei Graph-Texturen
  weder UUID noch Pfad (`graphTexKey` ist leer) und bindet die weiße Dummy-Textur
  (`D3D11Renderer.cpp:4950-4951`, `:7079-7093`). Das Sechseck-Muster ist die Hex-Kachelung des
  Materials auf der weißen Albedo, und die Dummy-Normale (1,1,1) wird je Zelle anders gedreht.
* „Vertauscht“ wurde nicht reproduziert. Kandidat dafür ist nur die UUID-Kollision in §4.
* Im Spiel-Log steht **kein Fehler und keine Warnung**, auf keinem Backend.

## 2. Weg der Texturreferenz und die Stelle, an der Editor und Export auseinanderlaufen

1. **Material:** `Engine/Materials/M_AutoLandscape.hasset` (0x412:1) nennt seine drei Arrays nur als
   Pfad (`graphTexturePaths`, `graphTextureIds` leer): `Engine/Textures/Landscape/T_Landscape_{Albedo,Normal,Mask}_Array.hasset`
   = 0x40F / 0x410 / 0x411 (`:1`, gelesen aus dem META-Chunk der Cache-Dateien).
2. **Editor:** `ContentManager::resolveAbsolutePath` (`ContentManager.cpp:982-1000`) prüft der Reihe nach
   Projekt-Override, ausgelieferten Default `<exe>/EngineContent` und als dritte Stufe
   `GlobalState::engineContentCacheDir()` = `%APPDATA%\HorizonEngine\EngineContentCache`. Dort liegen die
   Arrays (SFTP-Download, je 106,7 MiB). `scanContentDirectory` (`:3334`) trägt sie auch in die Registry
   ein. Beim Menschen ist `<deploy>\Editor\EngineContent\Textures\Landscape` **leer** (geprüft:
   `C:\Users\conno\source\repos\HorizonEngineBuild\Editor`).
3. **Export, die Abweichung:** `ProjectExporter.cpp:824-829` packt genau zwei Wurzeln, die Projekt-`Content`
   und `settings.engineContentDir` (= `engineContentRoot()`, `ExportDialogPanel.cpp:1601`). Die dritte
   Stufe aus Schritt 2 (Cache) kennt der Exporter nicht.
4. **Packer:** `rewriteRefsForPack` (`HpakWriter.cpp:311-316`) löst jeden Pfad über die gepackten
   Einträge auf. Fehlt das Ziel, wird es `UUID{}`, ohne Log-Zeile. Die Pfade selbst werden verworfen
   (`:427-430`). Im `.hpak.manifest` des Menschen sieht man das direkt:
   `0x412 refs = [[…Albedo_Array…,0,0],[…Mask_Array…,0,0],[…Normal_Array…,0,0]]`, und 0x401–0x411 fehlen
   (0x400 im Pak ist das Water-Material). Das Pak ist 2,8 MB groß.
5. **Spiel:** Das gepackte Material trägt nur Null-UUIDs und leere Pfade. Selbst ein Cache auf dem
   Spielrechner würde nicht helfen, und das Spiel hat ohnehin keinen SFTP-Weg.

## 3. Nachweis (NN-WS03, RTX 4070, 2026-10-13)

Aufbau in `C:\hw189` (außerhalb des Repos), Skripte in `scripts/landscape-export-repro/` (Pfade fest auf `C:\hw189`):

* Editor = Kopie des Deploys des Menschen (`HorizonEngineBuild\Editor`, 13.10., `Textures/Landscape` leer).
  **Nicht** `out/build/x64-release/src/HE_Editor` nehmen: Dort liegen noch die 128-px-Platzhalter vom 09.10.
  (`copy_directory` löscht nie), die würden gepackt und verdecken den Fehler.
* Private `APPDATA` je Lauf, deren `EngineContentCache\Textures\Landscape` nur die drei Arrays aus dem Cache
  des Menschen enthält. Projekt = Kopie von `C:\Users\conno\Downloads\HEProject` ohne `Export`.
* `run_editor.ps1 -Name A|B`: Editor starten (HE_MCP=1), nach 40 s PrintWindow-Bild, dann `project_package`
  über die MCP-Brücke (`pkg.py`, `incremental:false`, ohne Kompression/Verschlüsselung).
* Spiel: `docs/spielpfad-postfx-run-game.ps1 -Mode fresh -SettleMs 20000 -Set GameBackend=D3D11|Vulkan`
  (die ersten ~15 s zeigt das Projekt ein Logo-Widget).

| | A: Arrays nur im Cache (= Mensch) | B: dieselben 3 Dateien als Projekt-Override `Content/Engine/Textures/Landscape` |
|---|---|---|
| Editor-Ansicht | texturiert | texturiert, gleich |
| Pak | 19,1 MB, 45 Einträge, 0x412-Refs `0:0` | 339,1 MB, 48 Einträge, 0x40F/0x410/0x411 drin, Refs `0x40F:1`, `0x411:1`, `0x410:1` |
| Spiel D3D11 | weiß-graue Sechsecke (157 167 191) | texturiert (68 63 46) |
| Spiel Vulkan | – (Pak des Menschen: 166 176 198) | texturiert (68 63 46) |

Es ändert sich nur eine Variable: ob die Dateien unter einer Wurzel liegen, die der Exporter packt.
Der Projekt-Override ist damit auch ein **Workaround für den Menschen**, solange es keinen Fix gibt.

## 4. Nebenbefund: UUID-Kollision 0x400:1

`kEngineWaterMaterialId` (`DefaultAssets.h:93`, mat_gen) und `T_Landscape_Grass_Albedo` (landscape_tex_gen,
`LandscapeTexGen/main.cpp:81`, `:345`) haben beide `0x400:1`, beide Generatoren beanspruchen den 0x400-Block.
`HpakWriter.cpp:944-990` lässt die erste Wurzel gewinnen und verwirft die zweite still. Das trifft
M_AutoLandscape nicht (es liest nur die Arrays), aber jedes eigene Material mit der losen Gras-Albedo. Ein Fix,
der den ganzen Cache nach `engineContentRoot` packt, würde Grass_Albedo still verlieren. Wie der Editor die
Kollision auflöst (`m_diskRegistry`, Scan-Reihenfolge), ist **nicht geprüft**. Das ist ein eigenes Thema und
wird hier nicht weiterverfolgt.

## Handoff

- **Topic / step:** Thema 189, Schritt 1 von 3 (Ursache eingrenzen, nichts fixen)
- **Branch / commit:** `claude/landscape-material-export-textures`, Commit mit diesem Dokument (gepusht, siehe Hive-Post)
- **Done:** Bug reproduziert und Ursache benannt: Der Exporter packt die dritte Engine-Content-Stufe (SFTP-Cache)
  nicht, und der Packer macht fehlende Refs still zu Null-UUIDs.
- **Files touched:** dieses Dokument, `scripts/landscape-export-repro/{run_editor.ps1,pkg.py}`. Kein Code geändert.
- **Verified:** A/B aus §3 mit Bildern in `C:\hw189\shots\{editor_A,editor_B,gA_d3d11,gB_d3d11,gB_vk,d3d11b,vk,gl}`,
  Pak-Manifeste `C:\hw189\out_{A,B}\HEProject.hpak.manifest`, META-UUIDs der Cache-Dateien.
- **Not verified:** D3D12-Spiel. Der Editor wurde nur auf GL aufgenommen, der Mensch nutzt D3D12; die Auflösung
  ist backend-unabhängig (`ContentManager`). Das Format der Arrays wurde nicht ausgelesen, für die Ursache
  ist es unerheblich (B rendert richtig).
- **Scratch:** `C:\hw189` (~2,5 GB: Editor-Kopie, appdata_A/B, out_A/B, Spielkopien) bleibt für Schritt 3 stehen.
- **Decisions suggested, not taken (für Schritt 2 / Queen):**
  1. Exporter nimmt den Cache als dritte Wurzel (`{engineContentCacheDir(), "Engine/"}`) **nach** `engineContentDir`,
     aber besser nur die vom Projekt referenzierten Dateien (sonst 15 × 16 MiB lose Texturen mehr, und die 0x400-Kollision).
  2. Fehlt eine referenzierte Engine-Datei auch im Cache, muss der Export-Dialog sie vorher herunterladen
     (HE_ContentSync, nicht aus HE_Core heraus) oder den Export mit Fehler abbrechen.
  3. `rewriteRefsForPack` soll einen nicht auflösbaren Ref als Warnung ins Export-Log schreiben. Jetzt ist er stumm.
     Das ist unabhängig von 1. und 2. sinnvoll.
- **Pending:** Schritt 2 (Fix), Schritt 3 (D3D + Vulkan abnehmen, Build, Tests).
- **Traps:** stale Platzhalter in `out/build/x64-release/src/HE_Editor/EngineContent` (s. §3). Private APPDATA
  verschiebt auch den Cache, also die Arrays dort hineinlegen. Das erste MainWindowHandle des Editors ist der
  Splash (1×1-Bild), erst nach dem Warten neu holen. Das Pak mit Arrays ist 339 MB unkomprimiert.
- **Next steps:** 1. Fix in `ProjectExporter.cpp:824-829` (+ Liste der referenzierten Engine-Pfade, z. B. aus
  `rewriteRefsForPack`-Deps) und Warnung in `HpakWriter.cpp:311-316`. 2. Test in `tests/test_project_exporter.cpp`:
  Material mit Pfad-Ref auf eine Datei nur im Cache → Pak enthält sie, Ref gebacken. 3. A/B aus §3 mit dem neuen
  Deploy wiederholen (Variante A muss texturiert sein).
