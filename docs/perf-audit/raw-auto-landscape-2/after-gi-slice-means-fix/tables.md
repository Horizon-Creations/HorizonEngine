### Tabelle 3b: GPU-Zeit je Pass (Detailed-Capture, RenderScale 2), ms

| Zelle | Läufe | Shadow p50 (p10) | SSAO p50 (p10) | Scene p50 (p10) | Bloom p50 (p10) | Tonemap p50 (p10) | Present p50 (p10) | Scene p50 min..max | Δ Scene ms | GPU Σ Pässe p50 | Δ GPU ms | Cluster | Instanzen in Reichweite / gespeichert | tickWorld Frame 3 (einmalig) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s128-off | 3 | 0.18 (0.17) | 0.97 (0.89) | 12.95 (12.23) | 0.86 (0.79) | 0.36 (0.34) | 0.28 (0.25) | 12.25..13.00 | Basis | 15.61 | Basis | 0 | 0 / 0 | – |
| s128-x4 | 3 | 0.26 (0.23) | 0.99 (0.92) | 13.13 (12.31) | 0.87 (0.81) | 0.38 (0.37) | 0.28 (0.26) | 13.01..13.20 | +0.18 | 15.99 | +0.38 | 86 | 7695 / 25510 | – |
| s512-x4 | 3 | 0.46 (0.30) | 1.21 (1.20) | 16.22 (16.11) | 0.82 (0.82) | 0.40 (0.39) | 0.26 (0.25) | 16.18..16.41 | Basis | 19.41 | Basis | 203 | 9258 / 425343 | 132.7 |

