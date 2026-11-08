# Auto-LOD: GPU-Capture und Stutter-Analyse der Endkonfiguration (Thema 331, Schritt 10)

Stand 08.11.2026, Metal auf dem M5 (MacBook Air), Netzbetrieb, Bildschirm gesperrt, Standard-RenderScale (Fenster 2840x1528).
**Kein Engine-Code geaendert**, nur Messung und Bericht. Baut auf `docs/auto-lod-fix-verify-2026-11-08.md` (Schritt 9).
Rohdaten und Skripte: `docs/perf-audit/auto-lod-step13-raw-2026-11-08/`. Editor-Binary: Klon aus Schritt 9 (HEAD mit Fix `9e7a2f05b`),
Projekt `projbake` (Kette + Impostor), gleiche Kamera `--cam 0,2.5,-24,3.14159,-0.15`, `HE_SKY_TIME=40000`.

## 1. Ergebnis in vier Saetzen

* **Dominanter Kostenblock sind die 3D-Wolken, nicht die Pflanzen:** der Wolken-Pre-Pass (ein Fullscreen-Dreieck, Viertel-Aufloesung) kostet **6,5 ms von 14,7 ms GPU-Zeit (44 %)**, der Haupt-Scene-Encoder mit Boden + Impostoren **5,6 ms (38 %)**, davon die Impostoren ca. 5 ms (Boden allein 0,5 ms). Schatten, SSAO, Bloom, Tonemap, Present zusammen ca. 2,4 ms.
* **Das "Stottern" (1-%-Low 6 bis 25 fps) ist in der Hauptsache ein Prasentations-/Pacing-Artefakt, kein Szenen-Ausreisser:** die Frame-Abstaende wechseln zwischen ca. 0,75 ms und ca. 35 ms (zwei Drawables werden gemeinsam freigegeben), und das passiert **genauso in einer Szene mit 6 ms GPU-Zeit** (Boden ohne Wolken: 198 bis 210 von 590 Frames ueber 25 ms, Endkonfiguration 200 bis 229 von 590). Ueber 4 Frames gemittelt liegt das p99 bei 21 bis 26 ms, nicht bei 60 ms.
* **Die extremen Ausreisser aus Schritt 9 (65 bis 170 ms) sind sehr wahrscheinlich Fremdlast**: in den neuen FPS-Laeufen ohne Vsync (Load Average 2,7 bis 5) gibt es keinen Frame ueber 55 ms (Schritt 9: Load 2 bis 14, Frames bis 169 ms, einer mit 73 ms CPU im Schatten-Vorbereitungs-Scope). In den Vsync-Laeufen kommen vereinzelte Spitzen (52 bis 496 ms, je 1 bis 3 Frames) **auch in den leichten Szenen** vor, also unabhaengig von der Szenenlast; belegt ist die Fremdlast als Ursache nicht, nur dass die Szene es nicht ist.
* **60 fps sind auf diesem Geraet knapp, aber nicht sicher erreichbar:** Durchsatz mit Vsync 53 fps (Endkonfiguration), 57 fps mit `cloudQuality 0` (+ `cloudShadows` aus: gleich), 65 fps in der leichten Referenz. Der Weg zu 60 fps laeuft ueber die Wolken, nicht ueber weitere Pflanzen-LOD-Arbeit.

## 2. Methode und Grenzen

* **Werkzeug:** `xcrun xctrace record --template 'Metal System Trace' --attach <pid>` auf einen normal gestarteten Editor (`--launch` startete stattdessen `/Applications/HorizonEditor.app` und haengte; Falle, siehe Handoff), 6 s, Export der Tabelle `metal-gpu-intervals` (je Encoder Vertex- und Fragment-Dauer). Das ist die echte GPU-Zeit je Encoder ohne die Engine-Zaehler (die per-Encoder-Zaehler der Engine ueberlappen auf der TBDR-GPU und nehmen Takt-Effekte auf).
* **Eine Xcode-GUI-Capture (`.gputrace`, Shader-Profiler je Draw) ist headless nicht auswertbar** und wurde nicht gemacht; die Encoder-Intervalle des System Trace sind die feinste Aufloesung, die sich skriptbar auswerten laesst. Pro Draw im Scene-Encoder ist damit nicht getrennt (Impostor vs. Boden nur per Subtraktion mit der Boden-Szene).
* Die Encoder haben **keine Labels** (`Render Command N`). Die Zuordnung ist abgeleitet: Encoder 7 = Wolken-Pre-Pass (Vertex 0,003 ms = 3 Eckpunkte; Zeit sinkt mit `cloudQuality 0` von 6,5 auf 4,4 ms, waehrend Encoder 8 gleich bleibt; Reihenfolge laut `EncodeFrame`: Wolken-Pre-Pass vor dem Scene-Encoder), Encoder 3 = Wolkenschatten-Karte (512x512, verschwindet mit `cloudShadows=false`), Encoder 8 = Scene (Boden + Impostoren + Himmels-Komposit), Encoder 22 = Tonemap/Present. SSAO/Schatten-Kaskaden sind die kleinen Encoder 0 bis 5 (nicht einzeln belegt).
* Der Trace verlangsamt den Lauf (26 ms je Frame, GPU nur zu 50 % aktiv): **gut fuer die Arbeitsaufteilung, nicht fuer das Pacing.** Pacing kommt aus den Laeufen ohne Trace.
* `he_perf_capture.py` mit `--detailed` zeigt die Wolken falsch verteilt (Tabelle 3.2): die Zaehler schreiben die Wolken-Zeit dem Scene-Pass zu, und die Wirkung von `cloudQuality 0` ist dort kaum sichtbar (-0,5 ms statt -2,0 ms im Trace). **Fuer Wolken-Entscheidungen gilt der Trace.**

## 3. GPU-Zeit nach Pass (Chain + Impostor, Standard-RS)

### 3.1 Metal System Trace (p50 ueber ca. 260 Frames, ms)

| Block | Endkonfig. | `cloudQuality 0` | Wolken-Regler alle billig* | Boden + Wolken (ohne Pflanzen) |
|---|---|---|---|---|
| Wolken-Pre-Pass (Enc 7) | **6,46** | 4,40 | 4,40 | 7,52 |
| Scene-Encoder (Boden, Impostoren, Himmel) | **5,76** (V 0,20 + F 5,55) | 5,95 | 6,23 | 0,73 |
| Wolkenschatten-Karte (Enc 3) | 0,64 | 0,64 | entfaellt | 0,87 |
| Tonemap/Present (Enc 22) | 0,26 | 0,26 | 0,26 | 0,31 |
| Rest (Schatten, SSAO, Bloom) | ca. 1,6 | ca. 1,8 | ca. 1,4 | ca. 1,6 |
| **Summe Vertex + Fragment** | **14,70** | 13,03 | **12,25** | 11,03 |

\* `cloudQuality 0`, `cloudInterShadows` aus, `cloudShadows` aus. Impostor-Anteil = Scene-Encoder Endkonfiguration minus Scene-Encoder der Boden-Szene = ca. 5,0 ms (Fragment).
Der Wolken-Pre-Pass ist **konstant** (p10 5,4 / p50 6,45 / p90 6,8 ms, ein Ausreisser 13,6 ms im Trace), der Scene-Encoder ebenso (5,6 / 5,75 / 6,4): kein einzelner Pass hat eine Streuung, die ein Stottern erklaert.

### 3.2 Engine-Zaehler (`--detailed`, Pass-p50, Mittel 2 Laeufe) und warum sie hier irrefuehren

| Szene | Scene | SSAO | Schatten | Bloom | Tonemap | GPU gesamt |
|---|---|---|---|---|---|---|
| Endkonfiguration | 13,27 | 0,42 | 0,90 | 0,29 | 0,16 | 15,6 |
| ohne Wolken | 12,05 | 0,78 | 0,61 | 0,54 | 0,29 | 15,2 |
| Dome statt 3D-Wolken | 11,69 | 0,63 | 0,68 | 0,41 | 0,23 | 14,4 |
| Boden + Wolken | 11,42 | 0,48 | 1,27 | 0,40 | 0,20 | 14,4 |
| Boden ohne Wolken | 1,97 | 1,18 | 0,97 | 0,93 | 0,49 | 6,7 |

**Taktzeuge:** die Tonemap-Zeit (gleiche Arbeit) schwankt von 0,15 bis 0,49 ms (Faktor 3): leichtere Szenen laufen auf niedrigerem GPU-Takt. Deshalb ist "Scene ohne Wolken 12,05 ms" in der Endkonfiguration **kein** Beleg, dass Wolken nichts kosten. Szenen mit unterschiedlicher Last sind mit diesen Zaehlern nicht ms-gegen-ms vergleichbar.

## 4. Ursache des Stotterns

Belegt (Laeufe ohne Trace, 600 Frames, Zaehler aus):

| Szene | GPU (Zaehler) | Frames > 25 ms, Vsync aus | Frames > 25 ms, Vsync keep | Mittel-dt (aus / keep) |
|---|---|---|---|---|
| Boden ohne Wolken | 6 ms | 198 (1 Lauf) | 203 / 210 | 14,6 / 15,1 bis 15,5 |
| Boden + Wolken | 15 ms | 217 / 228 | 186 / 216 | 15,2 bis 15,5 / 14,6 bis 15,8 |
| Endkonfiguration | 15 ms | 200 / 213 | 229 / 225 | 15,2 bis 15,7 / 18,4 bis 19,0 |

* Das Muster `x.x.x.` (kurz/lang im Wechsel) ist in jeder Szene gleich, auch in der mit 6 ms GPU. Es hat daher **nichts mit Impostoren, Pflanzen oder Wolken zu tun**: das ist, wie Frames auf diesem (gesperrten) Bildschirm an die Compositor-Drawables gebunden werden (`nextDrawable` ist die einzige Gegendruck-Stelle, `MetalRenderer.mm:18093`; drei Drawables). Das 1-%-Low pro Frame misst dieses Paarmuster (35 ms) und nicht die Szene.
* `--vsync keep` aendert das Muster nicht (gesperrter Bildschirm); die leichten Szenen zeigen dort 63 bis 68 fps wie ohne Vsync, nur die Endkonfiguration faellt auf 53 fps (Mittel-dt 18,4 bis 19,0 ms).
* **Reiner Durchsatz** (Mittel-dt) ist die belastbare Zahl: Endkonfiguration 15,2 bis 15,7 ms ohne Vsync (63 bis 66 fps), mit Vsync 18,5 bis 19,0 ms (53 fps).
* Schritt 9 hatte 1-%-Lows von 6,6 bis 15,2 fps und Einzel-Frames bis 169 ms bei Load 2 bis 14. Zu Beginn dieses Schritts lag der Load bei 10 und `MTLCompilerService` bei 95 % CPU (anderer Bee). Mit ruhigerer Maschine (Load 2,7 bis 5) sind die 1-%-Lows 22 bis 26 fps und verschwinden die Frames ueber 55 ms im vsync-aus-Lauf: **konsistent mit Fremdlast, aber nicht isoliert nachgewiesen** (kein Lauf mit absichtlich erzeugter Last).
* **Nicht belegt:** wie sich das Pacing auf einem **entsperrten** Bildschirm mit echtem Vsync verhaelt (der Schirm blieb gesperrt, ich kann ihn nicht entsperren), und ob die Paarung dort auch auftritt. Das ist die offene Frage fuer das menschliche "Ruckeln".

## 5. Massnahme

**Keine Engine-Aenderung**, weil keine risikoarme, lokale Code-Massnahme mit Beleg gefunden wurde. Was es gibt, sind Szenen-Einstellungen (keine Codeaenderung, jeder Nutzer hat sie im Environment-Inspector):

| Einstellung | Wirkung im Trace | Durchsatz Vsync (Mittel-dt, 3 Runden) |
|---|---|---|
| Standard (cloudQuality 1) | 14,7 ms GPU | 18,5 / 19,0 / 18,8 ms = 53 fps |
| `cloudQuality 0` | 13,0 ms (Wolken-Pre-Pass -2,05 ms) | 17,7 / 17,4 / 17,8 ms = 57 fps |
| + `cloudShadows` aus, `cloudInterShadows` aus | 12,25 ms (-0,64 ms Wolkenschatten) | 17,5 / 17,2 / 17,9 ms = 57 fps |

Die Reihenfolge ist in allen drei Runden gleich (jeweils +3 bis 4 fps), der Abstand zur leichten Referenz (65 fps) bleibt. Das Default von `cloudQuality` im Code zu aendern wuerde jede Szene optisch aendern: das ist eine Entscheidung fuer die Queen/den Menschen, kein Eingriff in dieser Rolle.

Nicht gemacht (waere Architektur oder optisch sichtbar, ausserhalb des Schritts): Wolken-Pre-Pass zeitlich verteilen (jeder zweite Frame / Schachbrett), Pre-Pass nur ueber den Himmels-Anteil des Bildes (Scissor), Pre-Pass auf 1/8-Aufloesung, Impostor-Ueberzeichnung senken (5 ms Fragment).

## 6. Finaler Stand: 60 fps und Entscheidungsgrundlage fuer den PR

| Frage | Antwort |
|---|---|
| Hat die Kette + Impostor 60 fps bei Standard-RS? | **Knapp: ja im Durchsatz ohne Vsync (63 bis 66 fps), nein mit Vsync (53 fps, 57 fps mit billigen Wolken).** Gemessen auf gesperrtem Bildschirm. |
| Was kostet die Endkonfiguration? | 14,7 ms GPU: 44 % Wolken, 38 % Scene (davon Impostoren ca. 5 ms), Rest 18 %. |
| Bringt noch mehr Pflanzen-LOD etwas? | Nicht fuer 60 fps: Pflanzen-Anteil (Impostoren) ist ca. 5 ms, die Wolken sind groesser. Der Beitrag der Pflanzen-Arbeit (47,75 M -> 0,14 M Dreiecke) ist fertig. |
| Ist das Stottern ein Pflanzen-/Impostor-Problem? | **Nein.** Gleiches Muster ohne Pflanzen und ohne Wolken; Ausreisser in Schritt 9 wahrscheinlich Fremdlast (nicht isoliert). |
| Was blockiert den PR? | Nichts aus diesem Schritt. Offen: Pacing auf entsperrtem Bildschirm, Windows/Linux, v0/Kette/v5 bleiben weit unter 60 fps (Pflanzen-Stufen, Schritt 9). |

## 7. Tests

* `ctest --test-dir out/build/release -R test_foliage`: 6 von 6 gruen (kein `src/` geaendert, nur Docs und Rohdaten).
* Kein voller `ctest`-Lauf (kein Code geaendert; Schritt 9: 350/351, `test_hpak` Disk registry = bekannte Mac-Umgebungsluecke).

## 8. Nicht verifiziert

* Entsperrter Bildschirm / echtes Vsync-Pacing, Akkubetrieb, GPU-Takt-Verlauf ueber die Zeit (die Tabelle `metal-gpu-state-intervals` ist exportiert, aber nicht gedeutet: im Trace war die GPU zu 50 % im Leerlauf, durch die Trace-Last verfaelscht).
* Per-Draw-Kosten im Scene-Encoder (Xcode-GUI-Capture, Shader-Profiler); der Impostor-Anteil ist nur Subtraktion.
* Wolken-Entscheidung "Qualitaet 0 als Default": nicht angefasst, nur die Wirkung gemessen.
* Windows/Linux, andere Backends, CI-Ergebnis.

## Handoff

- **Topic / step:** 331 Auto-LOD, Schritt 10 (Plan-Nr. 13). Branch `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`.
- **Done:** Metal-System-Trace der Endkonfiguration (Encoder-Intervalle, Standard-RS), Varianten (Boden, ohne Wolken, Dome, cloudQuality 0, alle Wolken-Regler billig), Pass-Zaehler-Laeufe, rund 30 Vsync-/FPS-Laeufe ohne Zaehler (insgesamt 48 `he_perf_capture`-Laeufe, alle `rc=0`), Pacing-Analyse der F9-Frame-Zeitreihen aus Schritt 9 und der neuen Laeufe.
- **Result in one line:** Wolken-Pre-Pass 6,5 ms (44 %) ist der dominante Block (Impostoren ca. 5 ms), das "Stottern" ist ein szenenunabhaengiges Pacing-Muster (auch bei 6 ms GPU), die extremen Ausreisser aus Schritt 9 wahrscheinlich Fremdlast; `cloudQuality 0` bringt +3 bis 4 fps, 60 fps mit Vsync bleiben knapp ausser Reichweite (57).
- **Files:** `docs/auto-lod-gpu-capture-stutter-2026-11-08.md`, `docs/perf-audit/auto-lod-step13-raw-2026-11-08/` (Summaries, Skripte `mkvariants.py`/`run*.sh`/`xt2.sh`/`encsum.py`, Encoder-Tabellen). Kein `src/`. Die `.trace`-Dateien liegen nur in `/private/tmp/auto-lod-step13/` (enthalten die Prozess-Umgebung samt Tokens, nicht committen).
- **Verified:** Encoder-Aufteilung 4 Traces (Endkonfig., Boden+Wolken, cq0, billig); Wirkung von cq0 doppelt (Trace -2,05 ms, Durchsatz +3 bis 4 fps in 3 von 3 Runden); Paarmuster in leichter Szene (6 ms GPU) und schwerer gleich; `ctest -R test_foliage` 6/6.
- **Not verified:** entsperrter Bildschirm / echtes Vsync; Per-Draw-Kosten; GPU-Takt (DVFS) als Ursache der Zaehler-Abweichung; Windows/Linux; CI.
- **Decisions taken:** keine Engine-Aenderung (keine risikoarme Massnahme mit Beleg); Trace statt Engine-Zaehler als Autoritaet fuer Wolken; Traces nicht committet.
- **Decisions suggested, not taken:** Queen/Mensch entscheidet, ob `cloudQuality` 0 als Default oder ein Auto-Wolken-Budget (Wolken-Pre-Pass nur jeden 2. Frame, Scissor auf den Himmel) gebaut werden soll; fuer das menschliche Ruckeln zuerst auf entsperrtem Schirm gegenpruefen, bevor am Pacing gebaut wird.
- **Traps:** `xctrace record --launch -- <binary>` startete `/Applications/HorizonEditor.app` statt des Klons und haengte (T-Zustand): immer `--attach <pid>` auf einen selbst gestarteten Prozess, danach auf `XTDONE`/Export warten, nicht nach 5 min killen (Nachbearbeitung rechnet). Der Editor-Klon schreibt sein Log ins Klon-Verzeichnis. `he_perf_capture.py --detailed` ist bei Wolken irrefuehrend (Takt + Encoder-Ueberlappung): Tonemap-Zeit als Taktzeuge mitlesen. Gesperrter Bildschirm: Vsync-aus und Vsync-an zeigen dasselbe Paarmuster.
- **Next steps:** 1. Queen: PR-Entscheidung mit Abschnitt 6, Wolken-Default klaeren. 2. Optional: Pacing auf entsperrtem Schirm (Mensch). 3. Optional: Wolken-Pre-Pass amortisieren (eigener Schritt, optisch pruefen). 4. CI-Ergebnis abwarten.
