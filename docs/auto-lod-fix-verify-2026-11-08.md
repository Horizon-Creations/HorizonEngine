# Auto-LOD: Fix aus Schritt 8 auf der echten Poly-Haven-Szene (Thema 331, Schritt 9)

Stand 08.11.2026, Metal auf dem M5 (MacBook Air), Netzbetrieb, Bildschirm gesperrt. **Kein Engine-Code geaendert**, nur Messung und Bericht.
Prueft den Fix `9e7a2f05b` (`MaterialGraph.cpp`: Masked-Discard vor BaseColor/Normal/ORM) mit der Methodik aus
`docs/auto-lod-abnahme-2026-11-08.md` (Schritt 7). Rohdaten: `docs/perf-audit/auto-lod-step12-raw-2026-11-08/`
(32 `*.summary.json`, `table.txt`, `conditions.txt`, die Skripte `all.sh`/`cap.sh`/`tab.py`).

## 1. Ergebnis in drei Saetzen

* **Der Fix aendert auf der echten Szene nichts Messbares.** Scene-p50 vorher/nachher liegt in allen vier Szenen innerhalb des Rauschens
  (Endkonfiguration 23,5 / 23,8 ms vorher gegen 25,2 / 23,5 ms nachher), obwohl die Pflanzen genau der Zieltyp sind (`alphaMode: MASK` mit Diffuse + Normal + ARM, 2k).
  Die -82 bis -96 % des Windows/D3D12-Proxys aus Schritt 8 uebertragen sich auf Metal und die Poly-Haven-Pflanzen **nicht**.
* **60 fps werden nicht konsistent erreicht.** Nur die Endkonfiguration (Kette + Impostor) liegt bei Standard-RenderScale im Median unter 16,7 ms (11,0 bis 15,4 ms), aber mit
  Ausreissern (ein Lauf 44 fps im Mittel, 1-%-Low 6,6 bis 15,2 fps). v0 (12 bis 14 fps), Kette (20 bis 21 fps) und v5 (23 bis 49 fps) sind weit entfernt.
* RS2 bleibt wie vereinbart ausser Scope; die Pass-Zeit dort (Endkonfiguration 27 bis 29 ms) ist unveraendert.

## 2. Bedingungen

* **Genau ein Unterschied zwischen vorher und nachher: `libHorizonCore.dylib`.** "Vorher" = der unveraenderte Klon `/private/tmp/auto-lod-abnahme/Editor` aus Schritt 7
  (gebaut 08.11. 12:25, vor dem Fix-Commit 13:52). "Nachher" = `out/deploy/Editor` dieses Worktrees nach `cmake --build out/build/release --target HorizonEditor`
  (MaterialGraph.cpp neu uebersetzt, 13:57), per `cp -cR` geklont und mit `scripts/perf/selfcontain_deploy_copy.sh` auf `@loader_path` gebracht. `shasum` aller `libHorizon*.dylib`: nur Core verschieden,
  `HorizonEditor` bitgleich. (Der Build-Aufruf endet wie in Schritt 7 rot im Schritt `HorizonSDKRelease`, "dev MISMATCH"; der Editor ist davon unabhaengig aktuell.)
* Projektklone von Schritt 7 weiterverwendet (`proj` fuer v0/v5, `projchain`, `projbake`), jeder Lauf mit `--scene`; gleiche Kamera `--cam 0,2.5,-24,3.14159,-0.15`, `HE_SKY_TIME=40000`,
  `--warmup 300 --frames 600`, `caffeinate -d -u -i`; Pass-Lauf `--detailed --set RenderScale=2`, FPS-Lauf `--no-counters` bei Standard-RenderScale.
* **Vorher/Nachher abwechselnd** je Szene und Modus, zwei Runden (32 Laeufe, alle `rc=0`). Netzbetrieb, `CGSSessionScreenIsLocked=1` in jedem Lauf, Load Average 2,1 bis 14,0 (`conditions.txt`; andere Bees liefen mit).
  Zwei Laeufe pro Zelle, keine dritte Runde: **die Streuung durch Fremdlast ist gross (siehe 4), Aussagen unter ca. 5 bis 10 % sind nicht belegbar.**

## 3. Vorher / Nachher (Pass-p50 in ms bei RenderScale 2; Lauf 1 / Lauf 2)

| Szene | Scene vorher | Scene nachher | GPU gesamt vorher | GPU gesamt nachher | Tris Hauptpass |
|---|---|---|---|---|---|
| v0 Baseline | 68,2 / 73,6 | 68,9 / 69,8 | 113,4 / 125,5 | 114,4 / 116,0 | 47,75 M |
| v5 handgemacht | 53,9 / 53,9 | 56,9 / 52,2 | 57,8 / 57,8 | 60,8 / 56,3 | 6,28 M |
| Kette | 61,9 / 65,2 | 62,1 / 61,3 | 86,7 / 93,7 | 86,8 / 86,0 | 23,40 M |
| **Endkonfiguration (Kette + Impostor)** | 23,5 / 23,8 | 25,2 / 23,5 | 26,9 / 27,3 | 28,9 / 27,0 | 0,14 M |

Schatten und SSAO sind ebenfalls gleich (Schatten v0 31,8 bis 36,4 ms, Kette 16,4 bis 19,0, Endkonfiguration 0,8; SSAO 11,5 bis 13,2 / 6,5 bis 7,6 / 1,2 bis 1,3).
Die zwei groessten Abweichungen (v0 Lauf 2 vorher 125,5 ms, Kette Lauf 2 vorher 93,7 ms) liegen auf der **Vorher**-Seite, also in der Richtung, die ein Fix-Gewinn haette, sind aber
Fremdlast-Ausreisser (Load 8 bis 14, jeweils nur ein Lauf, die zugehoerigen Nachher-Laeufe derselben Runde liegen bei 116,0 und 86,0 ms): in der ruhigen Runde 1 sind vorher und nachher gleich
(v0 113,4 gegen 114,4 ms; Kette 86,7 gegen 86,8 ms; Endkonfiguration 26,9 gegen 28,9 ms; Lauf 2 der Endkonfiguration 27,3 gegen 27,0 ms). Mit zwei Laeufen je Zelle ist ein kleiner Gewinn (wenige Prozent) nicht auszuschliessen, ein grosser (Proxy: -84 %) schon.

## 4. Standard-RenderScale, Zaehler aus (fps Mittel / p50-Frametime ms / 1-%-Low fps; Lauf 1 / Lauf 2)

| Szene | vorher | nachher |
|---|---|---|
| v0 | 13,8 / 72,4 / 4,9 und 11,6 / 59,1 / 2,3 | 13,1 / 58,0 / 2,5 und 12,1 / 78,8 / 2,6 |
| v5 | 49,0 / 14,9 / 13,3 und 23,0 / 21,0 / 2,9 | 48,3 / 15,1 / 10,7 und 32,1 / 15,8 / 3,4 |
| Kette | 21,4 / 46,8 / 14,1 und 20,9 / 47,1 / 5,9 | 21,4 / 46,8 / 15,5 und 20,4 / 48,7 / 7,1 |
| **Endkonfiguration** | 44,1 / 15,4 / 6,6 und 63,0 / 11,6 / 14,4 | 61,0 / 11,5 / 13,7 und 64,3 / 11,0 / 15,2 |

* Die fps-Laeufe streuen mit der Fremdlast mehr als jeder Fix-Effekt (Endkonfiguration 44 bis 64 fps, v5 23 bis 49 fps bei gleichem Binary). **Vorher gegen nachher ist hier kein Befund**, der "Gewinn" der Endkonfiguration
  (44,1 -> 61,0 fps) liegt im Lauf-zu-Lauf-Rauschen (vorher Lauf 2 hatte 63,0 fps).
* Die geschaetzte GPU-Zeit im FPS-Modus ist ebenfalls gleich (Endkonfiguration 23,3 / 30,3 gegen 31,6 / 25,8 ms).

## 5. 60-fps-Verdict

**Nein, nicht konsistent ueber die gemessenen Szenen.**

| Szene | Standard-RS p50-Frametime | 60 fps? |
|---|---|---|
| v0 Baseline | 58 bis 79 ms (12 bis 14 fps) | nein, Faktor ~5 |
| Kette | 47 ms (20 bis 21 fps) | nein |
| v5 handgemacht | 15 bis 21 ms (23 bis 49 fps) | nein, Mittel unter 60 und 1-%-Low 3 bis 13 fps |
| Endkonfiguration | 11,0 bis 15,4 ms | **Median ja** (4 von 4 Laeufen unter 16,7 ms), **Mittel nur in 3 von 4 ueber 60**, 1-%-Low 6,6 bis 15,2 fps (Ruckler) |

Begruendung: Der Fix hat in dieser Szene keine Wirkung, es hat sich gegen Schritt 7 nichts bewegt (Schritt 7: 41 / 64 / 50 fps). Was die Endkonfiguration traegt, ist die Impostor-Karte (0,14 M Dreiecke,
Hauptpass 23 bis 25 ms bei RS2), nicht der Discard. Alle Szenen mit echten Pflanzen-Stufen (v0, Kette, v5) bleiben weit unter 60 fps. Die
Ruckler (1-%-Low unter 16 fps in jedem Lauf, auch mit Impostor) sind nicht untersucht (Fremdlast oder Engine-Hitch, nicht getrennt).

## 6. Warum der Proxy-Gewinn ausbleibt (nicht belegt, nur Hypothesen)

Der Fix ist im Binary (nur Core-dylib verschieden, `MaterialGraph.cpp` neu uebersetzt) und die Pflanzen sind `MASK` mit vollem PBR-Satz. Nicht gemessen, also nur Kandidaten:
1. Der Metal-Shader-Compiler (MSL -> AIR/GPU-Code) sortiert Texturzugriffe hinter dem `discard` ohnehin um oder maskiert sie mit Helper-Lanes; der D3D12/NVIDIA-Treiber im Proxy tat das offenbar nicht (Proxy: Vorher-Tail 4602 ms).
2. Auf der Apple-GPU (TBDR) ist die Pixelkoste anders verteilt (Tile-Verwaltung, Blending/Overdraw des Tile-Speichers, nicht nur ALU/Texturen), und die Szene ist in der Endkonfiguration von der 0,14-M-Karte
   bestimmt (Impostor-Material hat nur eine Textur, nichts vor dem Discard zu sparen).
3. In v0/Kette/v5 dominiert die Geometrie- und Schattenzeit (Schatten 16 bis 36 ms), nicht der Shading-Anteil, den der Fix trifft; in der Endkonfiguration der Boden (Schritt 1: v6 17,5 ms Scene bei RS2).
Ein Beleg waere ein Per-Draw- oder Xcode-GPU-Capture des Hauptpasses mit/ohne Fix oder ein Proxy-Lauf auf Metal; beides nicht gemacht.

## 7. Tests

* `ctest --test-dir out/build/release -R test_foliage`: 6 von 6 gruen.
* Voller `ctest -j6` (351 Tests, Release, HEAD mit Fix): 350 gruen, **1 rot: `test_hpak`**, Fall "Disk registry: scanContentDirectory resolves UUIDs from loose content"
  (`tests/test_hpak.cpp:1188`, `scanContentDirectory() == 2` nicht erfuellt). Das ist die bekannte Mac-Umgebungsluecke aus Schritt 7 und 8, kein Bezug zu diesem Schritt (kein `src/` geaendert).

## 8. Nicht verifiziert

* Gegenprobe auf Metal mit dem Proxy-Aufbau aus Schritt 8; GPU-Capture; Windows-Szene mit Poly-Haven-Assets.
* Mehr als zwei Laeufe je Zelle, Akkubetrieb, entsperrter Schirm (Schritt 1 lief so). Unterschiede unter 5 bis 10 % sind nicht aufloesbar.
* Bewegung/Ruckler, GI an, D3D11/D3D12/Vulkan/OpenGL auf der echten Szene.
* CI-Ergebnis (nur gestartet, siehe Handoff).

## Handoff

- **Topic / step:** 331 Auto-LOD, Schritt 9 (Plan-Nr. 12). Branch `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`.
- **Done:** Fix `9e7a2f05b` auf der echten Poly-Haven-Szene (Metal/M5, Schritt-7-Methodik, Standard-RS fuer fps, RS2 nur fuer Pass-Zeiten) vor/nachher gemessen: 4 Szenen x Pass/FPS x 2 Runden, abwechselnd, ein Binary-Unterschied (Core-dylib).
- **Result in one line:** Der Fix bringt auf Metal mit den echten Pflanzen **keinen messbaren Gewinn** (Scene-p50 gleich in allen Szenen); 60 fps werden **nicht** konsistent erreicht (nur die Endkonfiguration liegt im Median darunter, mit Ruckeln).
- **Files:** `docs/auto-lod-fix-verify-2026-11-08.md`, `docs/perf-audit/auto-lod-step12-raw-2026-11-08/`. Kein `src/`.
- **Verified:** 32 Laeufe `rc=0`; nur `libHorizonCore.dylib` verschieden (shasum); `ctest -R test_foliage` 6/6; voller ctest 350/351 (`test_hpak` Disk registry = bekannte Mac-Luecke).
- **Not verified:** Ursache des fehlenden Gewinns (Abschnitt 6 sind nur Hypothesen); Metal-Proxy-Gegenprobe; GPU-Capture; Akku/entsperrter Schirm; mehr als 2 Laeufe je Zelle; Ruckler-Ursache; CI-Ergebnis.
- **Decisions taken:** Vorher = unveraenderter Schritt-7-Klon, Nachher = Worktree-Build; Aussagen nur wo ueber dem Rauschen (Pass-p50), fps nicht als Vorher/Nachher gewertet.
- **Decisions suggested, not taken:** Der D3D12-Proxy-Gewinn (Schritt 8) darf nicht als Gewinn der echten Szene berichtet werden. Der Fix ist harmlos (Tests gruen) und kann bleiben, ist aber kein Hebel fuer 60 fps. Hebel bleiben Boden/Schatten/Hauptpass-Pixel (Schritt 1: Boden allein 17,5 ms Scene bei RS2) und die Ruckler.
- **Traps:** `cmake --build ... HorizonEditor` endet rot im Schritt `HorizonSDKRelease` (Editor trotzdem aktuell); vorher/nachher nur mit `selfcontain_deploy_copy.sh`-Klonen vergleichbar; fps-Laeufe streuen 44 bis 64 fps bei gleichem Binary (Fremdlast), nur Pass-p50 zaehlt.
- **Next steps:** 1. Queen: 60-fps-Ziel neu bewerten (Verdict Abschnitt 5). 2. Optional Metal-GPU-Capture (Xcode) des Hauptpasses der Endkonfiguration, um Boden/Impostor/Schatten/Sky zu trennen. 3. Optional Ruckler-Analyse (1-%-Low 6 bis 15 fps) per F9-Capture. 4. CI-Ergebnis abwarten.
- **Update Schritt 10:** Punkte 2 und 3 sind erledigt in `docs/auto-lod-gpu-capture-stutter-2026-11-08.md` (Abschnitt 6 = finaler 60-fps-Stand): Metal-System-Trace der Endkonfiguration (Wolken ca. 4 ms und Impostoren ca. 3,7 ms netto von 14,7 ms), Ruckeln = 99,5 % Warten auf das Drawable, szenenunabhaengig.
