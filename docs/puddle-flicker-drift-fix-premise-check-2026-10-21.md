# Pfützen-Flackern — Prämissen-Check vor dem Drift-Fix (Thema 234, Schritt 4)

## Kurzfassung

Dieser Schritt sollte einen "Float-Präzisionsverlust bei großer absoluter Zeit"
in `drift = speed × time` (`src/HE_Tools/src/MatGen/main.cpp:387`) beheben.
**Die Prämisse ist falsch — es gibt keinen Präzisionsverlust zu fixen, und die
Done-when-Kriterien des Schritts widersprechen sich gegenseitig.** Kein
Code-Change vorgenommen; `git diff` ist leer.

## Warum die Prämisse nicht stimmt

Schritt 3 (`docs/puddle-flicker-time-diagnosis-2026-10-21.md`) hat bereits
durchgerechnet, dass die Pro-Frame-Phasenänderung bei den echten Parametern
(`kWaveA/B/C`, `Physics/WaterWaves.h:54-56`) klein ist: A≈0.030 rad,
B≈0.066 rad, C≈0.130 rad pro 66-ms-Frame. Dieser Schritt rechnet die
tatsächliche fp32-vs-fp64-Differenz nach (numpy, Wellenzug B, `kWave=2π/5`,
`speed=0.8`, `along≈600` — Weltkoordinaten-Größenordnung an der Repro-Position
`[187.99, 0, 568.38]`, `px`/`pz` kommen direkt aus `WorldPos`, main.cpp:722-724):

| Laufzeit | fp32-Phase | fp64-Phase | \|Fehler\| |
|---|---|---|---|
| 30 s (Repro-Laufzeit) | 723.822998 | 723.822947 | **5.07e-05 rad** |
| 28 h (Worst-Case aus Schritt 3) | -100581.234 | -100581.230 | **3.98e-03 rad** |

Pro-Frame-Phasenänderung (B): **0.0664 rad.**

Der fp32-Fehler ist bei 30 s über 1000× kleiner, bei 28 h immer noch 17×
kleiner als die reale, beabsichtigte Phasenbewegung pro Frame. Es gibt in
keinem getesteten oder zitierten Zeitfenster eine numerisch sichtbare
Ungenauigkeit — `along` (Weltposition, Größenordnung Hunderte Meter) dominiert
die Argumentgröße der Sinus/Cosinus-Aufrufe ohnehin viel stärker als `time`
jemals könnte, und selbst das bleibt im fp32-Rahmen unauffällig.

Skript: `/tmp/<inline, siehe hive-Transkript>` — numpy fp32/fp64-Vergleich,
reproduzierbar mit `kB=2π/5`, `speedB=0.8`, `along=600.0`.

## Der in der Vorgabe vorgeschlagene Fix wäre zusätzlich falsch

Die Vorgabe schlägt vor, `drift` per `fmod(drift, 2π)` zu wrappen. `drift` ist
aber in **Metern** (`speed [m/s] × time [s]`), nicht in Radiant — die Phase
entsteht erst später via `kWave × travel` (Zeile 417, `kWave = 2π/length`).
`fmod(drift, 2π)` würde also nicht "eine volle Phasenperiode" wrappen, sondern
alle `2π` **Meter** Drift-Weg — das sind bei Wellenzug B (`speed=0.8`) alle
`2π/0.8 ≈ 7.85 s`, bei C (`speed=0.5`) alle `2π/0.5 ≈ 12.57 s` ein künstlicher
Sprung. Das wäre ein **neues** Flackern, keine Behebung. Ein korrekter Wrap
müsste `drift` auf das jeweilige (ggf. lengthScale-skalierte) `length` wrappen,
da `kWave × length = 2π` — Wrap-Periode dann `length/speed` (B: 6.25 s, C:
3.2 s). Für den Rest des Ausdrucks (`travel0`/FBM-Warp, Zeile 396) gilt das
nicht: der Warp ist nicht periodisch, ein Wrap dort würde das Rauschfeld
springen lassen.

## Die Done-when-Kriterien widersprechen sich

- Kriterium 3 verlangt maskiertes Δ ≤ 0.2 auf allen vier Windows-Backends.
- Schritt 3s Messtabelle zeigt: Δ ≤ 0.2 (genauer: 0.20) wurde **ausschließlich**
  erreicht, indem **alle sechs** `waveTrain()`-Aufrufe die Zeit komplett
  eingefroren bekamen (`driftfrozen`) — keine Wellenbewegung mehr.
  Teil-Einfrieren (nur B: Δ 1.46, nur C: Δ 3.19) kommt dem Zielwert nicht
  nahe.
- Kriterium 4 verlangt sichtbar normal animierende Wellen, keine Regression.
- Da die gemessene Pro-Frame-Phasenbewegung real und von der Wellenanimation
  selbst stammt (nicht von numerischem Rauschen, siehe Lag-Autokorrelation in
  Schritt 3: glattes Wachstum über ~12 Frames statt Sofort-Plateau), bleibt
  **jeder** Fix, der die Wellen sichtbar weiterlaufen lässt, bei Δ≈7.5 —
  unabhängig von der verwendeten Präzisionstechnik. Kriterium 3 und Kriterium 4
  schließen sich damit gegenseitig aus; es existiert kein Drift-Präzisionsfix,
  der beide erfüllt.

## Status

Kein Code in `src/HE_Tools/src/MatGen/main.cpp` geändert (`git diff` leer).
Blocker an die Queen gemeldet (Hive-Request #149) mit drei Optionen:
(a) Schritt als failed werten, (b) Pivot zu Schritt 3s Decisions-Vorschlag
(`fadeB`-Schwellen enger fassen, main.cpp:733 — außerhalb der Limits dieses
Schritts), (c) auf Antwort zu Hive-Request #147 (Bug vs. beabsichtigtes
Glitzern) warten bzw. den noch offenen sauberen Editor-vs-Spiel-Vergleich
nachholen (würde auch erklären, warum der Mensch es nur im Spiel sieht, obwohl
die Drift-Animation identisch in Editor und Spiel läuft).

## Handoff

**Done:** Prämisse des Schritts rechnerisch widerlegt (fp32/fp64-Phasenfehler
um Größenordnungen zu klein, sowohl bei 30s als auch beim zitierten
28h-Worst-Case). Der konkret vorgeschlagene Fix (`fmod(drift, 2π)`) als
Einheitenfehler identifiziert, der ein neues Flackern einführen würde. Die
Widersprüchlichkeit der Done-when-Kriterien (Δ≤0.2 vs. keine Wellen-Regression)
belegt. Kein main.cpp-Change. Blocker an Queen gesendet (Request #149).

**Pending:** Antwort der Queen auf Request #149. Falls Option (c) gewählt
wird: sauberer Editor-vs-Spiel-Vergleich mit identischem Repro-Setup (Schritt 1
hatte nur einen ungleichmäßig getimten Editor-Burst, kein belastbarer Beleg)
wäre der nächste sinnvolle Schritt, da die Drift-Animation nachweislich
identisch in Editor und Spiel läuft — der vom Menschen gemeldete
Spiel-exklusive Charakter des Bugs muss also woanders herkommen (Export-Pfad,
Postprocessing, Frame-Pacing, o.ä.), nicht in `waveTrain()`.

**Decisions:** Keinen Fix implementiert, der nur die Metrik erfüllt, indem er
die Wellenanimation unterdrückt (das wäre eine Regression, kein Fix) — auch
wenn das rechnerisch den geforderten Δ-Wert träfe.

**Fallen:** Falls ein künftiger Schritt doch einen Longrunning-Precision-Wrap
umsetzen will: NICHT `fmod(drift, 2π)` (Einheitenfehler, Meter vs. Radiant,
siehe oben) — richtig wäre `fmod(drift, length)` je Wellenzug (inkl.
`lengthScale`-Skalierung aus Zeile 365-368), und nur auf der Phasen-Seite
(Zeile 416), nicht auf der Warp-Seite (Zeile 396, dort ist travel0 nicht
periodisch).

**Next steps:** Siehe Pending. Keine Backend-Messungen in diesem Schritt
nötig/sinnvoll (kein Code-Change, der etwas verändern würde).
