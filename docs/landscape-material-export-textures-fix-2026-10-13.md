# Landscape-Material-Export: Fix (Thema 189, Schritt 2)

Ursache laut Befund (`docs/landscape-material-export-textures-befund-2026-10-13.md`): Der Editor liest die
drei Landschafts-Arrays aus dem EngineContent-Download-Cache, der dritten `Engine/`-Stufe von
`ContentManager::resolveAbsolutePath`. Der Exporter packte nur Projekt-`Content` und `<exe>/EngineContent`,
und `rewriteRefsForPack` backte die drei Pfad-Refs von M_AutoLandscape still zu Null-UUIDs.

## 1. Was geändert ist

| Stelle | Änderung |
|---|---|
| `ProjectExporter.h` `ExportSettings::engineContentCacheDir` | neu: der Cache als dritte Wurzel, nur wenn `engineContentDir` gesetzt ist (dasselbe Gate wie im Resolver) |
| `ProjectExporter.cpp` `packContent` | Wurzeln jetzt `Content` → `engineContentDir` (`Engine/`) → Cache (`Engine/`, `fallbackOnly`), die Reihenfolge des Resolvers |
| `HpakWriter.h` `SourceRoot::fallbackOnly` | eine Datei dieser Wurzel füllt nur einen adressierten Pfad, den keine frühere Wurzel hat. Ohne das würde eine veraltete Cache-Kopie mit anderer UUID den Pfad im Bake übernehmen, während der Editor den ausgelieferten Default zeigt |
| `HpakWriter.cpp` `addDirectories` | Warnungen: (a) Ref ohne Ziel in irgendeiner Wurzel → „baked as a null UUID“, (b) UUID schon von einer Datei mit **anderem** Pfad belegt → „not packed“ (gleicher Pfad = Override, bleibt still). Auch bei inkrementell wiederverwendeten Einträgen (aus deren `deps`) |
| `HpakFormat.h` `PackSettings::onWarning`, `ExportSettings::onWarning`, `HpakWriter::warnings()` | Durchreichung der Warnungen |
| `ExportDialogPanel.cpp` ~1601 / ~1645 | setzt `engineContentCacheDir = GlobalState::engineContentCacheDir()`; Warnungen gehen als Severity 1 ins Build-Fenster. MCP `project_package` läuft durch denselben Code |

Das Auto-Landscape-Material, die Texturdaten und der Runtime-Loader sind unverändert.

## 2. Nachweis (NN-WS03, 2026-10-13)

* `he_tests` (Release, `C:/hw189b`, `DEPLOY_DIR=C:/hw189b/deploy`, Scratch-APPDATA): `test_project_exporter.cpp`
  34/34; alle zehn Testdateien, die `HpakWriter`/`exportProject` benutzen (`grep -l`), 623/623. Vier neue Fälle:
  Cache-only-Arrays werden gepackt und gebacken (auch per Pfad über `__asset_index__` auflösbar);
  Negativkontrolle ohne Cache → Refs `0:0` + zwei Warnungen; **inkrementeller Re-Export über das kaputte Pak**
  (`incremental` ist Default `true`, der Fall des Menschen): das Material wird neu gebacken statt mit `0:0`
  übernommen (`assetsReused == 0`, Kontrolle davor: unverändert = 1 reused); ausgelieferter Default verdeckt eine
  veraltete Cache-Kopie (andere UUID), Kollision unter anderem Pfad ergibt genau eine Warnung.
* Negativkontrolle des Schattenwurfs: `fallbackOnly`-Prüfung abgeschaltet → der dritte Fall scheitert
  (4 statt 3 Einträge, alte UUID gepackt und gebacken). Danach zurückgesetzt.
* End-to-End, Variante A aus dem Befund §3 mit dem **neuen** Editor (`C:\hw189b\deploy\Editor`,
  `EngineContent` ohne `Textures/`, Arrays nur in `appdata_C\…\EngineContentCache`), Export per MCP:
  Pak 355,5 MB, 48 Einträge, 0x412-Refs `1039:1 / 1041:1 / 1040:1` (= 0x40F/0x411/0x410), vorher `0:0`.
  Spiel D3D11: texturiert, Boden-Mittelwert RGB **68 63 46** = Kontrolle B (A ohne Fix: 156 167 190).
  Bild `C:\hw189\shots\gC_d3d11\fresh_1600x900.png`. Skript: `C:\hw189\run_editor_new.ps1`
  (= `scripts/landscape-export-repro/run_editor.ps1` mit Editor-Pfad `C:\hw189b\deploy\Editor`).

## 3. Entscheidungen

* **Ganzer Cache statt „nur Referenziertes“.** Tier 2 wird auch ganz gepackt; eine Pfad-Hülle aus den
  Material-Refs hätte Script-`loadAsset("Engine/…")` und UUID-Refs aus Szenen verfehlt und denselben Fehler
  später wiederholt. Preis beim Menschen: alle 13 Cache-`.hasset` (≈ 510 MB roh: Arrays 320 MB +
  10 lose Layer-Texturen ≈ 190 MB). Wer das nicht will, schließt sie mit `excludePatterns` aus.
* Nur was **dieser Rechner** heruntergeladen hat, kann mit. Eine Engine-Datei, die nie geladen wurde,
  fehlt weiter, jetzt aber mit Warnung im Build-Fenster.
* Die 0x400-Kollision (Water-Material vs. `T_Landscape_Grass_Albedo`) wird nicht gelöst, nur gemeldet:
  Mit dem Cache des Menschen erscheint pro Export eine Warnung dazu. Water gewinnt wie bisher.

## Handoff

- **Topic / step:** Thema 189, Schritt 2 von 3 (Ursache beheben)
- **Branch / commit:** `claude/landscape-material-export-textures`, Fix `3aa588df`, Doku `1064ebe2`, Re-Export-Test + Doku-Nachtrag im Commit danach (gepusht)
- **Done:** Exporter packt den EngineContent-Cache als dritte, verdeckte `Engine/`-Wurzel; stille Null-Refs und
  UUID-Kollisionen werden gemeldet. Tests + E2E auf D3D11 grün (§2).
- **Verified:** §2. **Not verified:** Vulkan- und D3D12-Spiel mit dem neuen Pak (Schritt 3; das Pak ist
  backend-unabhängig, Kontrolle B lief im Befund auf D3D11 und Vulkan). Volle `he_tests`-Suite nicht gelaufen,
  nur die Pak/Export-Fälle. macOS/Linux nur über CI.
- **Scratch:** `C:\hw189b` (Release-Build + Deploy dieses Branches), `C:\hw189\{out_C,g_C_d3d11,appdata_C}`.
- **Traps:** `C:\hw189\commitmsg.txt` lag noch vom Schritt 1 da; ein fehlgeschlagenes Write ließ `git commit -F`
  die alte Nachricht nehmen (per amend vor dem Push korrigiert). Pakete mit Arrays sind ≈ 355 MB unkomprimiert.
- **Nicht angefasst, aber gesehen:** Ein Projekt-Override mit **anderer** UUID als der Default unter demselben
  Pfad: beide werden gepackt und `pathToUuid[rel]` zeigt danach auf den Default (Tier 2), nicht auf den Override.
  Vorbestehend, betrifft den Cache nicht (dort greift `fallbackOnly`).
- **Next steps (Schritt 3):** 1. `C:\hw189\g_C_d3d11` kopieren und mit `-Set GameBackend=Vulkan` bzw. `D3D12`
  laufen lassen (`docs/spielpfad-postfx-run-game.ps1 -Mode fresh -SettleMs 20000 -Root C:\hw189`), mit dem
  Editor-Bild vergleichen. 2. Optional mit dem vollen Cache des Menschen (13 Dateien) exportieren und die
  0x400-Kollisionswarnung im Build-Fenster sehen. 3. CI-Ergebnis des Branches abwarten (Hive meldet es).
