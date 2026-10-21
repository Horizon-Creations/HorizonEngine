# Pfützen-Flackern — Zeit-Teilausdruck eingegrenzt (Thema 234, Schritt 3)

## Kurzfassung

Schritt 2 zeigte: das Flackern ist zeitgetrieben (Δ 7.65→0.097 bei komplett
eingefrorener `Time`-Node), aber nicht der Chop. Dieser Schritt grenzt weiter ein:
**Ursache ist die gewöhnliche Wellen-Phase `drift = speed × time` in `waveTrain()`
(`src/HE_Tools/src/MatGen/main.cpp:387`), die in die Sinus/Cosinus-Phase der drei
Hex-Zellen (Zeilen 412–424) für alle sechs Aufrufe (Wellen A/B/C + ihre drei
Companions, Zeilen 774–779) einfließt.** Die Gust-Modulation (Zeilen 750–757) trägt
nichts bei. Der FBM-Warp (Zeilen 396–401), der densselben `drift`-Wert nutzt, trägt
nur einen kleinen Teil bei — der Großteil des Effekts ist die reine Phasen-Bewegung,
nicht die Rauschfeld-Verzerrung.

**Wichtiger Befund für den Fix-Schritt**: Die gemessenen Phasenänderungen pro Frame
(66 ms) sind mit den echten Projekt-Parametern (`kWaveA` 14 m/1.0 m/s, `kWaveB` 5 m/
0.8 m/s, `kWaveC` 1.6 m/0.5 m/s, Physics/WaterWaves.h:54-56) klein (0.03–0.15 rad,
≈1.7°–8.6°) — zu klein für ein offensichtliches Aliasing (>π pro Frame) oder einen
Float32-Präzisionsverlust (die `Time`-Quelle ist `SDL_GetTicks()/1000.0f` seit
Prozessstart, bei den hier getesteten ~30 s ist der ULP vernachlässigbar, siehe
D3D11Renderer.cpp:6717-6722). Die Hypothesen "große absolute Zeit" und "periodisches
Aliasing" aus der Schritt-3-Vorgabe sind damit für DIESEN Repro (frischer Export,
30 s Laufzeit) **nicht bestätigt**. Was bleibt: eine korrekt, glatt fortschreitende
Wellen-Phase, die über Fresnel/Spiegelung die Sternreflexionen jeden Frame leicht
verschiebt — die offene Frage aus Schritt 2 ("ist das ein Bug oder beabsichtigtes
Glitzern?") bleibt daher weiterhin unbeantwortet und wiegt nach diesem Befund eher
in Richtung "beabsichtigtes/inhärentes Verhalten einer bewegten Normalen", nicht
"numerischer Defekt". Kein Fix in diesem Schritt.

## Methodik

Gleicher Aufbau wie Schritt 2: `C:\hw234\HEProject`, privater Build `C:/hw234b`
(nur Target `mat_gen`), private Editor-Kopie `C:\hw234\deploycopy\Editor` (eigenes
APPDATA je Variante, `EngineContentCache` jeweils leer), Export über die
MCP-Bridge (`project_package`, Profil Shipping) nach `C:\hw234\game_<variante>`,
D3D11, 24-Frame-Burst (`PrintWindow`, 66 ms, **25 s Settle** — der Skript-Default
von 9 s fängt noch den Splash ein, siehe Pitfalls), maskiertes Δ auf denselben
3892 Seepixeln aus Schritt 1 (`masked_diff.py`).

Drei Experimente, je ein lokal eingefügter, frischer Konstanten-Pin
(`m.k(0.0f, ...)`) anstelle von `time` an genau einer Stelle, danach `main.cpp`
sofort auf den committeten Stand zurückgesetzt (`git diff` nach jedem Experiment
leer):

- **gustfrozen**: `time` in der Gust-UV (Zeilen 751–752) durch 0 ersetzt. Alles
  andere (Wellen-Drift, Chop/Ripple) unverändert zeitgetrieben.
- **driftfrozen**: `time`-Argument aller sechs `waveTrain()`-Aufrufe (774–779)
  durch 0 ersetzt. Gust und Chop/Ripple bleiben zeitgetrieben.
- **warpfrozen**: nur der `drift`, der in `travel0` (Zeile 396, der FBM-Warp)
  einfließt, durch 0 ersetzt; der `drift` in der Pro-Hex-Phase (Zeile 416) bleibt
  live. Isoliert Warp von der reinen Phase.

Jede Variante: Knotenzahl/Chain-Länge geprüft (sollten bis auf die absichtlich
eingefügten Konstanten unverändert sein), Export-Hash (`md5sum` des `.hpak`) gegen
Referenz/Vorläufer verglichen (immer unterschiedlich → kein Stale-Asset).

## Messungen (D3D11, maskiert auf den See, 23 Frame-Paare je Lauf)

| Variante | mean \|Δ\| (maskiert) | Bereich | geänderte Px (>10, von 3892) | Knoten |
|---|---|---|---|---|
| Referenz (frisch nachgemessen, `control_refresh`) | 7.54 | 5.3–9.6 | 719 | 1316 |
| **gustfrozen** (nur Gust-Zeit = 0) | 7.65–8.28 | 3.1–9.7¹ | 724–789 | 1317 |
| **warpfrozen** (nur FBM-Warp-Zeit = 0, Phase live) | 6.43 | 3.1–8.4 | 566 | 1322 |
| **driftfrozen** (ALLE sechs `waveTrain()`-Zeit = 0) | **0.20** | 0.15–0.26 | **9.4** | 1317 |
| (Referenz Schritt 2: Time-Node komplett = 0) | 0.097 | 0.08–0.12 | 4.6 | — |

¹ zwei driftfrozen-Läufe (`gustfrozen`/`gustfrozen2`) zur Kontrolle, beide im
Referenzband.

## Interpretation

- **gustfrozen ≈ Referenz** → die Gust-Modulation (Zeilen 750–757) ist NICHT die
  Ursache, exakt wie die Hypothese aus Schritt 2 vermutete, aber jetzt widerlegt
  (analog zur Chop-Widerlegung in Schritt 2).
- **driftfrozen ≈ Schritt-2-Referenz (Time komplett 0)** → die Wellen-Drift in
  `waveTrain()` ALLEIN erklärt praktisch den gesamten Effekt; mit ihr eingefroren
  bleibt kein messbarer Unterschied zum vollständig eingefrorenen Material übrig.
- **warpfrozen bleibt bei ~84 % der Referenz** → der FBM-Warp (der denselben
  `drift`-Wert nutzt) trägt nur einen kleinen Teil (~16 %) bei; der Großteil
  kommt aus der reinen Sinus/Cosinus-Phase der drei Hex-Zellen (Zeile 417:
  `phase = kWave × travel`, `travel = alongOf(dir) − drift`).
- **Warum das weder offensichtliches Aliasing noch ein Präzisionsproblem ist**:
  mit den realen Parametern (`kWaveA/B/C`, `Physics/WaterWaves.h:54-56`) beträgt
  die Phasenänderung pro 66-ms-Frame für die Primär-Trainings 0.03–0.13 rad
  (A: 2π/14×1.0×0.066≈0.030; B: 2π/5×0.8×0.066≈0.066; C: 2π/1.6×0.5×0.066≈0.130),
  für die Companions ähnlich (lengthScale/√lengthScale skaliert beides
  gleichermaßen, z. B. Companion C: 2π/1.136×0.421×0.066≈0.154). Alle deutlich
  unter π — kein Phasenwrap zwischen zwei Captures. Die `Time`-Quelle selbst ist
  `SDL_GetTicks()/1000.0f` (Sekunden seit Prozessstart, D3D11Renderer.cpp:6717-6722,
  identisches Muster auf OpenGL/D3D12/Vulkan/Metal) — bei den hier gemessenen
  ~30 s seit Start ist der Float32-ULP (~3.6e-6 s) komplett vernachlässigbar.
  Das gemessene Δ ist vermutlich einfach die korrekt funktionierende, glatt
  fortschreitende Wellenanimation, deren Normalenänderung über Fresnel die
  Sternreflexionen jeden Frame leicht verschiebt — keine numerische Instabilität,
  die hier nachgewiesen wurde.

## Verifiziert vs. nicht verifiziert

- **Verifiziert**: Gust ist nicht die Ursache. Die Wellen-Drift in `waveTrain()`
  (Zeile 387, angewendet über die sechs Aufrufe 774–779) erklärt praktisch den
  gesamten gemessenen Effekt. Innerhalb dieser Drift ist die reine Phase
  (nicht der FBM-Warp) der Haupttreiber. Phasenänderung pro Frame ist bei den
  Projekt-Parametern klein; Float32-Präzision bei ~30 s Laufzeit unauffällig.
- **Nicht verifiziert**: Verhalten bei SEHR langer Laufzeit (Stunden, wo
  `SDL_GetTicks()/1000.0f` als Float32 tatsächlich an Auflösung verliert — bei
  ~2.7 h ULP≈1.2 ms, bei ~28 h ULP≈12 ms — nicht getestet, da dieser Repro immer
  frisch startet). Ob das gemessene Δ (mean ~7–8 von 765 möglichen, ~1 %) das ist,
  was der Mensch als störendes "Flackern" gemeldet hat, oder beabsichtigtes
  Glitzern (Kommentar `main.cpp:787` "sparse glints") — weiterhin offen, jetzt mit
  härteren Zahlen, aber Hive-Request #147 noch unbeantwortet. Tagszene
  (Sonnenglitzer statt Sternfunkeln) nicht getestet. D3D12/Vulkan/OpenGL/Metal
  nicht erneut gemessen (kein Fix vorhanden). Keine weitere Aufschlüsselung
  innerhalb der Pro-Hex-Phase (z. B. welcher der drei Hex-Beiträge dominiert).

## Pitfalls für den nächsten Schritt

- `burst_capture2.ps1`s Default `-SettleMs 9000` ist zu kurz (fängt den
  Horizon-Creations-Splash als Frame 0 ein, Δ fällt künstlich auf ~0.2) — IMMER
  `-SettleMs 25000` übergeben. Neu: `-GameDirOverride` hinzugefügt (vorher auf
  `$Root\game` hartkodiert), damit `game_<variante>`-Exporte direkt nutzbar sind.
- Pfade mit Backslash im Bash-Tool: `-Root C:\hw234` wird zu `C:hw234`
  (Backslash wird von sh als Escape gefressen) → IMMER Forward-Slashes
  (`C:/hw234`) in Bash-Aufrufen von PowerShell-Skripten.
- `cmd.exe /c "... && ..."` aus dem Bash-Tool führt NICHTS aus (nur Banner,
  kein Fehler) — aus dem PowerShell-Tool funktioniert es. MSVC-Env laden:
  `cmd.exe /c '"...\vcvars64.bat" >nul 2>&1 && set'` parsen und per
  `Set-Item env:` übernehmen, dann `cmake --build` im selben PowerShell-Aufruf
  (Env überlebt nicht zwischen PowerShell-Tool-Aufrufen).
- `mat_gen.exe <outdir>` nimmt den ersten Nicht-Flag-Arg als Ausgabeverzeichnis
  — `--help` wird als Pfad "--help" interpretiert und legt dort ein Water.hasset
  an, kein echter Hilfetext.
- Fund-Infrastruktur aus Schritt 2 (`C:\hw234b`, `C:\hw234\deploycopy\Editor`,
  `drive_time0.py`-Muster) überlebt zwischen Hive-Runs auf demselben Gerät
  (NN-WS03) — vor dem Neuaufsetzen erst prüfen, ob sie noch da ist.

## Artefakte

`C:\hw234\variants\{gustfrozen,driftfrozen,warpfrozen}\Water.hasset`,
`C:\hw234\game_{gustfrozen,driftfrozen,warpfrozen}` (Exporte),
`C:\hw234\shots\{gustfrozen,gustfrozen2,driftfrozen,warpfrozen,control_refresh}`,
`C:\hw234\drive_variant.py` (generischer MCP-Treiber, appdata-Pfad als Argument),
`C:\hw234\burst_capture2.ps1` (jetzt mit `-GameDirOverride`). Alles unter
`C:\hw234*`, scratch, nicht im Repo. Keine Code-Änderung im Repo (`main.cpp` für
alle drei Experimente ediert, gebaut, gemessen, vor jedem nächsten Experiment und
vor diesem Commit zurückgesetzt — `git diff` ist leer).

## Nächste Schritte (nicht Teil dieses Schritts)

1. **Entscheiden, ob und wie gefixt wird.** Da keine numerische Instabilität
   (Aliasing/Präzision) nachgewiesen wurde, ist ein "Fix" hier keine
   Bug-Korrektur im klassischen Sinn, sondern eine Entscheidung: die
   Phasen-Geschwindigkeit der Hauptwellen dämpfen/filtern (z. B. ein Temporal-
   Filter auf den Spiegel-Term, oder die Distanz-Fades `fadeB`/`fadeC` enger
   fassen), ODER den Effekt als beabsichtigt einstufen und nichts ändern.
   Braucht die ausstehende Antwort auf Hive-Request #147.
2. Tagszene testen (Sonnenglitzer statt Sternfunkeln — deutlicher sichtbar,
   würde auch klären, ob es wirklich derselbe Effekt ist, den der Mensch meint).
3. Lang laufender Test (Stunden), um die Float32-`SDL_GetTicks()`-Hypothese für
   SPÄTE Spielzeiten doch noch zu prüfen, falls der Mensch von Flackern NACH
   längerem Spielen berichtet (anders als dieser Repro, der immer frisch startet).
