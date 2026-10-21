# Pfützen-Flackern — Zeit-Teilausdruck eingegrenzt (Thema 234, Schritt 3)

## Kurzfassung

Schritt 2 zeigte: das Flackern ist zeitgetrieben (Δ 7.65→0.097 bei komplett
eingefrorener `Time`-Node), aber nicht der Chop. Dieser Schritt grenzt weiter ein:
**Ursache ist die gewöhnliche Wellen-Phase `drift = speed × time` in `waveTrain()`
(`src/HE_Tools/src/MatGen/main.cpp:387`), angewendet auf die Sinus/Cosinus-Phase
der drei Hex-Zellen (Zeilen 412–424).** Von den drei Wellenzügen trägt **B
(`kWaveB`, 5 m/0.8 m/s) allein ~81 % des Effekts**, **C (1.6 m/0.5 m/s, fine
ripples) ~58 %**, **A (14 m/1.0 m/s, swell) praktisch nichts** bei (Details unten;
die drei Anteile überlappen, Summe >100 % ist bei einem additiven Sinus-Feld
normal). Die Gust-Modulation (Zeilen 750–757) trägt nichts bei. Der FBM-Warp
(Zeilen 396–401), der denselben `drift`-Wert nutzt, trägt nur einen kleinen Teil
bei (~16 %) — der Großteil ist die reine Phasen-Bewegung, nicht die
Rauschfeld-Verzerrung.

**Ist das überhaupt die Art Instabilität, nach der die Vorgabe gesucht hat?**
Nein, nicht im engen Sinn. Phasenänderung pro Frame (66 ms) ist mit den echten
Projekt-Parametern klein (0.03–0.15 rad, ≈1.7°–8.6°, siehe Rechnung unten) — zu
klein für einen Phasenwrap (>π) oder Float32-Präzisionsverlust (die `Time`-Quelle
ist `SDL_GetTicks()/1000.0f` seit Prozessstart; bei ~30 s ULP≈3.6 µs,
D3D11Renderer.cpp:6717-6722). Eine zusätzliche Lag-Autokorrelation auf den
bereits vorhandenen Referenz-Frames (`control_refresh`, kein neuer Capture)
bestätigt das unabhängig: das maskierte Δ wächst von Lag 1 (7.5) über Lag 3
(19.8) bis Lag 12 (32.2) und sättigt erst dort — ein glattes, über ~12 Frames
dekorrelierendes Signal, NICHT das Flachbild-ab-Lag-1-Muster, das echtes
Pro-Frame-Flackern/Specular-Aliasing zeigen würde. Beide Belege zusammen sprechen
dafür, dass hier die korrekt funktionierende, glatt fortschreitende Wellenanimation
(v. a. Wellenzug B) gemessen wird, nicht ein numerischer Defekt. Die aus Schritt 2
offene Frage ("ist das ein Bug oder beabsichtigtes Glitzern?", Hive-Request #147
unbeantwortet) bleibt damit offen — wiegt nach diesem Befund aber eher Richtung
"kein Defekt, sondern zu wenig gedämpfter sichtbarer Glint von Wellenzug B". **Kein
Fix in diesem Schritt** (laut Auftrag Limits: Fix ist der nächste Schritt).

## Methodik

Gleicher Aufbau wie Schritt 2: `C:\hw234\HEProject`, privater Build `C:/hw234b`
(nur Target `mat_gen`), private Editor-Kopie `C:\hw234\deploycopy\Editor` (eigenes
APPDATA je Variante, `EngineContentCache` jeweils leer), Export über die
MCP-Bridge (`project_package`, Profil Shipping) nach `C:\hw234\game_<variante>`,
D3D11, 24-Frame-Burst (`PrintWindow`, 66 ms, **25 s Settle** — der Skript-Default
von 9 s fängt noch den Splash ein, siehe Pitfalls), maskiertes Δ auf denselben
3892 Seepixeln aus Schritt 1 (`masked_diff.py`).

Sechs Experimente, je ein lokal eingefügter, frischer Konstanten-Pin
(`m.k(0.0f, ...)`) anstelle von `time` an genau einer Stelle, danach `main.cpp`
sofort auf den committeten Stand zurückgesetzt (`git diff` nach jedem Experiment
leer, auch am Ende dieses Schritts):

- **gustfrozen**: `time` in der Gust-UV (Zeilen 751–752) durch 0 ersetzt.
- **driftfrozen**: `time`-Argument aller sechs `waveTrain()`-Aufrufe (774–779)
  durch 0 ersetzt (Gust, Chop/Ripple bleiben zeitgetrieben).
- **warpfrozen**: nur der `drift`, der in `travel0` (Zeile 396, FBM-Warp)
  einfließt, durch 0 ersetzt; der `drift` in der Pro-Hex-Phase (Zeile 416) bleibt
  live.
- **afrozen / bfrozen / cfrozen**: `time`-Argument nur der zwei Aufrufe eines
  Wellenzugs (A: 774–775, B: 776–777, C: 778–779) durch 0 ersetzt, die anderen
  vier Aufrufe bleiben live.

Jede Variante: Knotenzahl/Chain-Länge geprüft (nur +1 Konstante pro Variante,
Chain-Länge 63 unverändert), Export-Hash (`md5sum` des `.hpak`) gegen
Referenz/Vorläufer verglichen (immer unterschiedlich → kein Stale-Asset).

## Messungen (D3D11, maskiert auf den See, 23 Frame-Paare je Lauf)

| Variante | mean \|Δ\| (maskiert) | Bereich | geänderte Px (>10, von 3892) |
|---|---|---|---|
| Referenz (frisch nachgemessen, `control_refresh`) | 7.54 | 5.3–9.6 | 719 |
| **gustfrozen** (nur Gust-Zeit = 0, zwei Läufe) | 7.65 / 8.28 | 4.7–9.7 | 724 / 789 |
| **warpfrozen** (nur FBM-Warp-Zeit = 0, Phase live) | 6.43 | 3.1–8.4 | 566 |
| **afrozen** (nur Wellenzug A = 0) | 7.91 | 1.9–10.3 | 685 |
| **cfrozen** (nur Wellenzug C = 0) | 3.19 | 1.0–5.3 | 293 |
| **bfrozen** (nur Wellenzug B = 0) | **1.46** | 1.1–2.2 | **63** |
| **driftfrozen** (ALLE sechs `waveTrain()`-Zeit = 0) | 0.20 | 0.15–0.26 | 9.4 |
| (Schritt 2: Time-Node komplett = 0) | 0.097 | 0.08–0.12 | 4.6 |

Lag-Autokorrelation auf `control_refresh` (gleicher Lauf wie die Referenzzeile,
keine neue Aufnahme): mean |Δ| bei Lag 1/2/3/5/8/12/16/20 = 7.5 / 14.1 / 19.8 /
27.4 / 31.3 / 32.2 / 31.8 / 30.1 — Wachstum bis Lag ≈12, dann Sättigung.

## Interpretation

- **gustfrozen ≈ Referenz** → Gust-Modulation ist NICHT die Ursache (analog zur
  Chop-Widerlegung in Schritt 2).
- **driftfrozen ≈ Schritt-2-Referenz (Time komplett 0)** → die Wellen-Drift in
  `waveTrain()` ALLEIN erklärt praktisch den gesamten Effekt.
- **warpfrozen bleibt bei ~85 % der Referenz** → der FBM-Warp trägt nur einen
  kleinen Teil bei; der Großteil kommt aus der reinen Phase (Zeile 417:
  `phase = kWave × travel`, `travel = alongOf(dir) − drift`).
- **Pro-Wellenzug (afrozen/bfrozen/cfrozen)**: B ist der mit Abstand größte
  Einzelbeitrag (Einfrieren allein senkt Δ von 7.54 auf 1.46, −81 %), C trägt
  deutlich bei (−58 %), A fast nichts (−0 bis leicht negativ, innerhalb der
  Lauf-Streuung). Deckt sich mit Schritt 2s Distanz-Fade-Analyse: `fadeB` bleibt
  über die gesamte sichtbare Fläche (32–85 m) bei 0.93–1.0 (kaum gedämpft),
  während A dort gar keinen eigenen Fade hat, aber mit 14 m Wellenlänge/0.07
  Steilheit die langsamste und flachste der drei ist; C ist steiler (0.09) und
  schneller, aber über `fadeC` (0.21–0.98) teilweise gedämpft.
- **Warum weder klassisches Aliasing noch ein Präzisionsproblem erkennbar ist**:
  Phasenänderung pro 66-ms-Frame mit den realen Parametern (`kWaveA/B/C`,
  `Physics/WaterWaves.h:54-56`): A 2π/14×1.0×0.066≈0.030 rad, B
  2π/5×0.8×0.066≈0.066 rad, C 2π/1.6×0.5×0.066≈0.130 rad; Companions ähnlich
  (lengthScale/√lengthScale skaliert beides, z. B. Companion C:
  2π/1.136×0.421×0.066≈0.154 rad). Alle deutlich unter π — kein Phasenwrap
  zwischen zwei Captures. Die `Time`-Quelle (`SDL_GetTicks()/1000.0f`, identisches
  Muster auf allen fünf Backends) hat bei ~30 s seit Prozessstart einen
  vernachlässigbaren Float32-ULP (~3.6 µs). Die Lag-Autokorrelation (siehe oben)
  zeigt ein glattes Wachstum über ~12 Frames statt eines sofortigen Plateaus ab
  Lag 1 — das Muster einer kontinuierlichen, korrelierten Bewegung, nicht von
  zufälligem Pro-Frame-Flackern oder Specular-Aliasing.

## Verifiziert vs. nicht verifiziert

- **Verifiziert**: Gust ist nicht die Ursache. Die Wellen-Drift in `waveTrain()`
  (Zeile 387) erklärt praktisch den gesamten Effekt, davon dominiert Wellenzug B,
  sekundär C, A trägt praktisch nichts bei. FBM-Warp ist nur ein kleiner
  Nebeneffekt. Phasenänderung pro Frame ist bei den Projekt-Parametern klein;
  Float32-Präzision bei ~30 s Laufzeit unauffällig; die Lag-Kurve zeigt glatte
  Dekorrelation statt eines Flacker-Musters.
- **Nicht verifiziert**: Verhalten bei SEHR langer Laufzeit (Stunden, wo
  `SDL_GetTicks()/1000.0f` als Float32 an Auflösung verliert — bei ~2.7 h
  ULP≈1.2 ms, bei ~28 h ULP≈12 ms — nicht getestet, da dieser Repro immer frisch
  startet). Ob das gemessene Δ (mean ~7–8 von 765 möglichen, ~1 %) das ist, was
  der Mensch als störendes "Flackern" gemeldet hat, oder beabsichtigtes Glitzern
  (Kommentar `main.cpp:787` "sparse glints") — weiterhin offen, Hive-Request #147
  unbeantwortet. Tagszene (Sonnenglitzer statt Sternfunkeln) nicht getestet.
  D3D12/Vulkan/OpenGL/Metal nicht erneut gemessen (kein Fix vorhanden). Warum
  genau B so viel stärker beiträgt als C trotz kleinerer Steilheit (0.10 vs. 0.09)
  nicht quantitativ zerlegt (Fade-Dämpfung vs. Phasenrate vs. Hex-Blending).

## Pitfalls für den nächsten Schritt

- `burst_capture2.ps1`s Default `-SettleMs 9000` ist zu kurz (fängt den
  Horizon-Creations-Splash als Frame 0 ein, Δ fällt künstlich auf ~0.2) — IMMER
  `-SettleMs 25000` übergeben. Neu: `-GameDirOverride` hinzugefügt (vorher auf
  `$Root\game` hartkodiert), damit `game_<variante>`-Exporte direkt nutzbar sind.
- Pfade mit Backslash im Bash-Tool: `-Root C:\hw234` wird zu `C:hw234`
  (Backslash wird von sh als Escape gefressen) → IMMER Forward-Slashes
  (`C:/hw234`) in Bash-Aufrufen von PowerShell-Skripten.
- `cmd.exe /c "... && ..."` aus dem Bash-Tool führt NICHTS aus (nur Banner,
  kein Fehler, kein Nonzero-Exit) — aus dem PowerShell-Tool funktioniert dieselbe
  Zeile. MSVC-Env laden: `cmd.exe /c '"...\vcvars64.bat" >nul 2>&1 && set'`
  parsen und per `Set-Item env:` übernehmen, dann `cmake --build` im SELBEN
  PowerShell-Aufruf (Env überlebt nicht zwischen PowerShell-Tool-Aufrufen).
- `mat_gen.exe <outdir>` nimmt den ersten Nicht-Flag-Arg als Ausgabeverzeichnis
  — `--help` wird als Pfad "--help" interpretiert und legt dort ein Water.hasset
  an, kein echter Hilfetext.
- Fund-Infrastruktur aus Schritt 2 (`C:\hw234b`, `C:\hw234\deploycopy\Editor`,
  `drive_time0.py`-Muster) überlebt zwischen Hive-Runs auf demselben Gerät
  (NN-WS03) — vor dem Neuaufsetzen erst prüfen, ob sie noch da ist.
- `powershell.exe -File burst_capture2.ps1 ...` liefert manchmal KEIN Stdout
  (leere Tool-Antwort), obwohl der Lauf erfolgreich war und Frames geschrieben
  wurden — immer `ls shots/<name> | wc -l` prüfen statt sich auf die Ausgabe zu
  verlassen; bei 0 Dateien einfach denselben Befehl erneut ausführen.

## Handoff

**Done:** Gust widerlegt (Δ unverändert). Wellen-Drift in `waveTrain()` (Zeile
387) als Haupttreiber bestätigt (Δ 7.54→0.20 bei Einfrieren, deckt sich mit
Schritt-2-Referenz 0.097). Pro-Wellenzug zerlegt: **B dominiert (−81 % allein),
C sekundär (−58 %), A irrelevant**. FBM-Warp nur Nebeneffekt (−15 %). Phasenraten
und Time-Quelle (`SDL_GetTicks()/1000.0f`) durchgerechnet: keine Evidenz für
Aliasing/Präzisionsverlust bei den hier getesteten ~30 s Laufzeit. Lag-Autokorrelation
auf vorhandenen Frames bestätigt glatte Dekorrelation statt Flacker-Musters.
Commits: `a9bf039e` (dieses Doc, erste Fassung) + diese Überarbeitung. Kein
Code-Change im Repo (main.cpp pro Experiment ediert/gebaut/gemessen/zurückgesetzt).

**Pending:** Entscheidung Bug-vs-beabsichtigt braucht Antwort auf Hive-Request
#147 oder eine Tagszenen-Messung (Sonnenglitzer statt Sternfunkeln). Fix noch
nicht umgesetzt (laut Auftrags-Limits für diesen Schritt nicht erlaubt).

**Decisions:** Fokus eines möglichen Fixes auf Wellenzug B (`kWaveB`,
`src/HE_Core/include/Physics/WaterWaves.h:55`) und sekundär C, NICHT auf Gust
oder FBM-Warp — die lohnen sich für einen Fix nicht, da sie kaum beitragen.
Kandidaten für den Fix-Schritt: `fadeB`-Schwellen (main.cpp:733, aktuell
60–220 m) enger fassen, damit B bei 32–85 m stärker gedämpft wird wie C es
schon ist; ODER ein Temporal-Filter auf den Spiegel-/Fresnel-Term; ODER (falls
die Antwort auf #147 sagt, das sei beabsichtigt) nichts ändern.

**Fallen:** Keine. Alle sechs main.cpp-Experimente wurden vor diesem Commit
zurückgesetzt (`git diff` leer, über Branch-Claim auf
`src/HE_Tools/src/MatGen/main.cpp` abgesichert).

**Next steps:** (1) Antwort auf #147 oder Tagszenen-Test abwarten/einholen.
(2) Falls Fix gewünscht: `fadeB`-Schwellen anpassen (oder Alternative), auf
D3D11 mit genau diesem Repro verifizieren (Δ sollte auf ~0.1–0.2 fallen wie
`driftfrozen`), danach D3D12/Vulkan/OpenGL (Metal: kein Mac auf NN-WS03
erreichbar, braucht ein anderes Gerät). (3) Infrastruktur (`C:\hw234*`) bleibt
lokal auf NN-WS03 nutzbar, bei Bedarf vor Wiederverwendung Frische prüfen.

## Artefakte

`C:\hw234\variants\{gustfrozen,driftfrozen,warpfrozen,afrozen,bfrozen,cfrozen}\
Water.hasset`, `C:\hw234\game_<variante>` (Exporte je Variante),
`C:\hw234\shots\<variante>` (Frames + control_refresh für die Lag-Kurve),
`C:\hw234\drive_variant.py` (generischer MCP-Treiber, APPDATA-Pfad als Argument),
`C:\hw234\burst_capture2.ps1` (jetzt mit `-GameDirOverride`). Alles unter
`C:\hw234*`, scratch, nicht im Repo. Keine Code-Änderung im Repo.
