# Pfützen-Flackern — Chop-Hypothese widerlegt (Thema 234, Schritt 2)

## Kurzfassung

Die in Schritt 1 (`docs/puddle-flicker-repro-2026-10-21.md`) vermutete Ursache — die
Chop-Schichten des Wassermaterials (`kChopBase`/`kChopMid`/`kChopFine`/`kChopFiner`,
`src/HE_Tools/src/MatGen/main.cpp:784-838`) — ist **widerlegt**. Ein Lauf mit
`kChopBase = 0.0f` (zieht alle vier Chop-Amplituden auf exakt 0, geprüft über
Knotenzahl/Chain-Länge des generierten Graphen: unverändert, nur der Konstantenwert
ändert sich) zeigt auf dem See **keine** Reduktion des Frame-zu-Frame-Δ gegenüber dem
Referenzstand. Ein zweiter, schärferer Test — die `Time`-Node des gesamten
Wassermaterials durch eine Konstante `0.0f` ersetzt, friert also JEDE Zeit-Animation
des Materials ein, nicht nur den Chop — drückt das See-Δ auf das Rauschniveau der
statischen Kamera. **Das Flackern sitzt im Wassermaterial und ist zeitgetrieben, aber
nicht im Chop-Code — vermutlich in den drei Haupt-Wellenzügen (A/B/C,
`waveTrain()`/`fadeB`/`fadeC`) oder der Gust-Modulation, die über die Sichtweite der
Pfütze hinweg kaum fadet.** Kein Fix wurde umgesetzt; dieser Schritt endet mit einem
belastbaren Negativbefund plus Diagnose-Infrastruktur für den nächsten.

## Methodik

- Gleicher Repro-Aufbau wie Schritt 1 (`C:\hw234\HEProject`, `ReproCamera` bei
  `[187.9966, 22.0, 628.0]`, Pitch -15°, Blick auf das `WaterSurface`-Mesh bei
  `[187.9966, 0.0, 568.3793]`), Export über `project_package` (Profil `Shipping`),
  24-Frame-Burst (`PrintWindow`, 66 ms Abstand, 25 s Settle), D3D11.
- **Eigener, privater Build-Baum** `C:/hw234b` (`cmake --preset x64-release -B
  C:/hw234b -DDEPLOY_DIR=C:/hw234b/deploy -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
  -ULibssh2_DIR`), nur das Ziel `mat_gen` gebaut — **nicht** der geteilte
  `HorizonEngineBuild`-Editor, den andere Hive-Instanzen/der Mensch evtl. gerade
  benutzen. `mat_gen.exe` braucht `HorizonCore.dll` im selben Ordner (POST_BUILD
  kopiert sie nur ins Deploy, nicht neben `mat_gen.exe` selbst — von Hand kopiert).
- **Eigene private Kopie des Editor-Deploys** (`C:\hw234\deploycopy\Editor`,
  `robocopy`-frei per `cp -r`, 497 MB, ~2 s) statt den geteilten
  `C:\Users\conno\source\repos\HorizonEngineBuild\Editor` zu verändern. Jede Variante
  des `Water.hasset` wurde dort hinein kopiert, die Editor-Kopie mit `HE_MCP=1`,
  `HE_HIDDEN_WINDOW=1` und eigenem `APPDATA` (`C:\hw234\appdata_<variante>`, von Schritt
  1s `appdata/HorizonEngine/config.json` geseedet, aber `EngineContentCache` geleert,
  um jeden Zweifel an einem gecachten alten Asset auszuschließen) gestartet,
  `project_package` per eigenem `he_mcp.py`-Treiber aufgerufen, Export mit
  `project_build_status` gepollt. Nach jedem Export per `md5sum` auf `HEProject.hpak`
  geprüft, dass der Export tatsächlich neu gepackt wurde (nicht nur der Dateiname).
- **Maskierte Messung statt Vollbild-Diff.** `diff_frames.py` (Schritt 1) misst das
  GANZE Bild inkl. Himmel — dort funkeln die Sterne unabhängig vom Wassermaterial und
  verdünnen jede Wasser-spezifische Veränderung. Maske aus Schritt 1s
  `diff_accum.png` (D3D11, Referenzlauf `d3d11_gi0b`) abgeleitet: alle Pixel mit
  Δ > 3 in den Bildzeilen 325–560 (See-Fleck, vom Sternenhimmel-Band y<165 klar
  getrennt — dazwischen liegt bei y 165–330 durchgehend Δ=0). 3 892 von 810 000
  Pixeln. Dieselbe Maske auf jede Variante angewendet (`masked_diff.py`).
- **GameBackend D3D11 only** für diesen Verifikationslauf (wie in der Vorgabe
  "zuerst EIN Backend"); die Vier-Backend-Bestätigung ist erst nach einem echten Fix
  sinnvoll und entfällt hier, weil kein Fix vorliegt.

## Messungen (D3D11, maskiert auf den See, 23 Frame-Paare je Lauf)

| Variante | mean \|Δ\| (maskiert) | Bereich | geänderte Px (>10, maskiert, von 3892) |
|---|---|---|---|
| Referenz (Schritt 1, `d3d11_gi0b`, unverändert) | 7.65 | 5.1–9.6 | 718 (436–875) |
| **kChopBase = 0** (alle vier Chop-Schichten exakt 0) | 7.56 | 4.9–9.6 | 720 (426–867) |
| **Time-Node = 0** (gesamtes Material eingefroren) | **0.097** | 0.08–0.12 | **4.6** (1–10) |

Chop=0 liegt **innerhalb der Lauf-zu-Lauf-Streuung** der Referenz (±1 %, die Spannweite
*innerhalb* eines einzelnen Laufs ist mit 4.9–9.6 schon größer als der Unterschied
zwischen den beiden Läufen) — kein Effekt. Time=0 drückt den Wert auf ~1 % des
Referenzwerts, das Rauschniveau einer wirklich statischen Szene.

Das unmaskierte Vollbild-Δ bleibt bei Time=0 nahezu unverändert (mean 0.12–0.13,
~4 000–4 400 geänderte Px) — die Himmelssterne funkeln unabhängig weiter, wie erwartet,
und bestätigen, dass die Maske tatsächlich nur die wassereigene Animation misst, nicht
irgendein globales Capture-Rauschen.

## Warum das die Chop-Hypothese widerlegt, nicht nur abschwächt

- `kChopBase` multipliziert `drive` zu `ampBase`, und `amp1..amp4` sind alle
  `ampBase × (Konstante) × fade`. `kChopBase = 0` zieht **alle vier** Chop-Amplituden
  exakt auf 0, unabhängig von `fadeR`/`fadeF`/`fadeG` — nicht nur eine der vier
  Schichten. Der generierte Graph hat identische Knotenzahl (1316) und Chain-Länge
  (63) wie die unveränderte Referenz — nur der eine Konstantenwert unterscheidet sich,
  die Chop-Rechnung bleibt also im Graphen vorhanden, liefert aber überall 0 statt
  eines kleinen oder großen Ausschlags.
- Der Effekt bleibt bei exakt 0 Chop-Beitrag messtechnisch identisch zur Referenz. Wäre
  Chop auch nur ein Teil-Faktor, müsste der Wert spürbar fallen, selbst wenn andere
  Quellen mit beitragen.
- Die eigene Distanz-Analyse (siehe unten) zeigt zudem, warum: in der Sichtlinie der
  `ReproCamera` liegt der See zwischen ~32 m (Vordergrund, Bildunterkante der
  See-Fläche) und ~85 m (Hintergrund, Bildoberkante) Kameraentfernung — nicht bei den
  59.6 m des in Schritt 1 genannten Einzelpunkts (dem `WaterSurface`-Weltposition,
  nicht dem sichtbaren Flächenbereich). `fadeF`/`fadeG` (die beiden feinsten
  Chop-Schichten, 0.4 m / 0.17 m) sind über die GESAMTE sichtbare Fläche praktisch 0
  (fadeG immer, fadeF nur im Vordergrund schwach bei ~0.15); `fadeR` (r1/r2, 2.6 m /
  0.9 m) liegt nur im Vordergrund (<45 m) nennenswert über 0 (~0.76 bei 32 m), im
  Hintergrund exakt 0. Die Haupt-Wellenzüge A/B/C sind dagegen über fast die GESAMTE
  Sichtfläche kaum gefadet (`fadeB` ≈ 0.93–1.0, `fadeC` ≈ 0.21–0.98 im gesamten
  32–85 m-Bereich) — das sind KEINE Chop-Konstanten, sondern die eigenen,
  zeitanimierten Steigungen der drei Wellenzüge samt ihrer "Companion"-Trainings und
  der Gust-Modulation (Zeilen 744–782).

## Nächste Schritte (nicht Teil dieses Schritts)

1. **Isolieren, welcher Teil der Zeit-Animation genau verantwortlich ist**: Wellen
   A/B/C einzeln (nicht nur komplett) einfrieren bzw. deren `knobSpeed`/Gust-Faktor auf
   0 setzen und denselben maskierten Burst-Test wiederholen — analog zu diesem Schritt,
   nur mit anderem Eingriffspunkt. `waveTrain()` (ab Zeile ~380) und die
   `gust`/`wPrim`/`wComp`-Modulation (Zeilen 750-757) sind die wahrscheinlichsten
   Kandidaten, weil sie (anders als Chop) kaum mit der Distanz faden.
2. **Prüfen, ob das überhaupt ein Bug ist statt beabsichtigtes Glitzern.** Ein
   bewegtes Wassernormal, das Sternreflexionen jeden Frame verschiebt, ist ein
   klassischer, oft gewollter "Glint"-Effekt (vgl. Kommentar Zeile 787: "sparse
   glints"). Ob das, was der Mensch als störendes "Flackern" gemeldet hat, genau DAS
   ist (und z. B. nur bei bestimmten Sichtwinkeln/Auflösungen störend wirkt) oder ob
   es eine Aliasing-Verstärkung ist (z. B. fehlendes Temporal-Filtering/TAA auf dem
   Spezular-Term), bräuchte entweder die ausstehende Antwort auf Hive-Request #147
   oder eine Tagszenen-Messung (Sonnenglitzer statt Sternfunkeln, deutlicher sichtbar,
   siehe Schritt 1 "Nächste Schritte" Punkt 2).
3. Die in Schritt 1 offene Frage (Editor-vs-Spiel-Vergleich mit kontrolliertem
   Zeit-Delta, Metal-Test) bleibt offen und ist von diesem Befund unabhängig.

## Verifiziert vs. nicht verifiziert

- **Verifiziert**: Chop-Konstanten sind nicht die Ursache (reproduzierbar, zwei
  mat_gen-Läufe byte-identisch pro Variante, Export-Hash je Variante unterschiedlich
  — kein Stale-Asset). Zeit-Animation des Wassermaterials als Ganzes IST die Ursache
  des See-Flackerns (99 % Reduktion bei eingefrorener Zeit).
- **Nicht verifiziert**: welche der Nicht-Chop-Komponenten (Wellen A/B/C, Gust,
  Companion-Trainings) im Einzelnen verantwortlich ist; ob ein Fix dort möglich ist,
  ohne die Optik (Reference 29, Gischt-Kämme) zu beschädigen; D3D12/Vulkan/OpenGL/Metal
  (nur D3D11 getestet, da kein Fix vorliegt, den es zu verifizieren gälte); Tageslicht-
  Szenario.

## Artefakte

`C:\hw234b` (privater Build-Baum, nur `mat_gen`), `C:\hw234\variants\{control,chop0,
time0}\Water.hasset`, `C:\hw234\deploycopy\Editor` (private Editor-Kopie),
`C:\hw234\appdata_{chop0,time0}`, `C:\hw234\game_{chop0,time0}` (Exporte),
`C:\hw234{chop0,time0}root\shots\*_d3d11\` (Frames + Diffs), `C:\hw234\masked_diff.py`,
`C:\hw234\burst_capture2.ps1` (Variante von Schritt 1s Skript mit `[int]$GI` statt
`[bool]$GI` — die PowerShell-Tool-Grenze mangelt `$true`/`$false` beim Aufruf von
`powershell.exe -File`, siehe Lesson). Alles unter `C:\hw234*`, scratch, nicht im Repo.
Keine Code-Änderung im Repo (main.cpp wurde für die beiden Experimente editiert,
gebaut, gemessen und VOR dem Commit zurückgesetzt — `git diff` ist leer).
