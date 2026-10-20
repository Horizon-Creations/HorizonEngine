# Thema 162, Schritt 4: Schattenpass auf den Frame-Zustand (Rohdaten)

Metal, Editor-Loop, 50 568 Entities (`scripts/perf/gen_reference_world.py --count 50000`),
`scripts/he_perf_capture.py --warmup 240 --frames 120 --no-counters --cam 0,25,90,0,-0.25`, Fenster versteckt
(`HE_HIDDEN_WINDOW=1`), Scratch-Projekt kopiert aus dem Test-Projekt. Bedingungen: **Bildschirm gesperrt,
Akkubetrieb, Last 2 bis 4**, also nur alt gegen neu innerhalb dieser Sitzung vergleichbar (abwechselnd gefahren).

Die `t162s4-<lauf>.summary.json` sind die Zusammenfassungen der Läufe (`cpuScopePerFrame` hat p10/p50/p90 je Scope),
die Profile (1,9 MB je Lauf) liegen nicht im Repo. Die Tabelle unten ist aus den Profilen gebaut: p50 je Frame in ms,
in Klammern die Aufrufe je Frame. Auswertung und Deutung: `docs/render-extractor-shadow-pass-plan.md` Abschnitt 8.5.

| Lauf | Stand |
|---|---|
| old1 bis old5 | vor Schritt 4 (Commit `9a890ca6`, Merge `7cb9a18d` ohne Renderer-Änderung) |
| new1, new2 | nur der Frame-Walk (`Metal::ExtractFrame`), Verfeinern in jedem Pass |
| meas1 | wie new1, dazu ein Scope `Metal::RefineBounds` um die Schleife (misst sie: 3 Läufe je Frame) |
| n2a, n2b | Verfeinern einmal je Walk, im ersten Pass (Schatten) |
| n3a, n3b | der Commit dieses Schritts: Verfeinern einmal je Walk, im Frame-Schritt |

Reihenfolge der Läufe: old1, new1, old2, new2; meas1, meas2; old3, n2a, n2b, old4; n3a, old5, n3b.

| capture | frame CPU | RenderExtractor::extract | RenderExtractor::reuse | Metal::ExtractFrame | Metal::EncodeShadowMap | Metal::RefineBounds | Metal::EncodeSSAO | Metal::EncodeScene | FrustumCuller::cull | Render |
|---|---|---|---|---|---|---|---|---|---|---|
| old1 | 51.40 | 19.10 (3x) | 0.00 (2x) | - | 25.74 (1x) | - | 8.02 (1x) | 7.78 (1x) | 3.48 (5x) | 41.94 (1x) |
| old2 | 50.15 | 19.07 (3x) | 0.00 (2x) | - | 25.34 (1x) | - | 7.90 (1x) | 7.80 (1x) | 3.40 (5x) | 41.79 (1x) |
| old3 | 49.46 | 18.55 (3x) | 0.00 (2x) | - | 24.86 (1x) | - | 7.76 (1x) | 7.58 (1x) | 3.23 (5x) | 40.99 (1x) |
| old4 | 49.58 | 18.64 (3x) | 0.00 (2x) | - | 24.81 (1x) | - | 7.72 (1x) | 7.43 (1x) | 3.17 (5x) | 40.59 (1x) |
| old5 | 49.47 | 18.79 (3x) | 0.00 (2x) | - | 25.00 (1x) | - | 7.75 (1x) | 7.53 (1x) | 3.23 (5x) | 40.97 (1x) |
| new1 | 49.77 | 18.79 (4x) | 0.00 (3x) | 18.79 (1x) | 6.33 (1x) | - | 7.95 (1x) | 7.71 (1x) | 3.30 (5x) | 41.32 (1x) |
| new2 | 49.90 | 18.73 (4x) | 0.00 (3x) | 18.73 (1x) | 6.52 (1x) | - | 8.07 (1x) | 7.78 (1x) | 3.26 (5x) | 41.52 (1x) |
| meas1 | 49.39 | 18.48 (4x) | 0.00 (3x) | 18.48 (1x) | 6.23 (1x) | 10.56 (3x) | 7.68 (1x) | 7.54 (1x) | 3.15 (5x) | 40.82 (1x) |
| n2a | 41.74 | 18.37 (4x) | 0.00 (3x) | 18.37 (1x) | 6.24 (1x) | 3.48 (1x) | 4.05 (1x) | 4.07 (1x) | 3.15 (5x) | 33.45 (1x) |
| n2b | 42.36 | 18.66 (4x) | 0.00 (3x) | 18.66 (1x) | 6.21 (1x) | 3.48 (1x) | 4.09 (1x) | 4.04 (1x) | 3.11 (5x) | 34.04 (1x) |
| n3a | 43.15 | 18.87 (4x) | 0.00 (3x) | 22.25 (1x) | 2.85 (1x) | 3.48 (1x) | 4.11 (1x) | 4.21 (1x) | 3.27 (5x) | 34.32 (1x) |
| n3b | 41.66 | 18.56 (4x) | 0.00 (3x) | 22.06 (1x) | 2.69 (1x) | 3.54 (1x) | 4.06 (1x) | 4.10 (1x) | 3.12 (5x) | 33.45 (1x) |
