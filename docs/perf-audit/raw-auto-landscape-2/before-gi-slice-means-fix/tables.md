### Tabelle 4: Texturformate der Arrays auf Metal (nach dem GI-Slice-Means-Fix)

| Format (V = 3, 3 Arrays) | Läufe | Arrays im Speicher MB | RSS-Spitze je Lauf, min..max MB (Median) | Scene-Pass p50 ms | Tonemap p50 ms (Takt-Zeuge) | Scene / Tonemap | GPU Σ Pässe p50 ms | CPU-Frame p50 ms | Slow-Frames 2-6 Σ ms (Start: Terrain, Upload, Warm-up) |
|---|---|---|---|---|---|---|---|---|---|
| rgba8 | 1 | 873 (3 x 290,8) | 754..754 (754) | 13.31 | 0.35 | 37.8 | 15.90 | 19.48 | 2674 |
| astc | 1 | 218 (3 x 72,7) | 817..817 (817) | 11.24 | 0.48 | 23.6 | 15.13 | 18.37 | 902 |
| bc7 | 1 | 218 (3 x 72,7) | 817..817 (817) | 11.57 | 0.58 | 20.0 | 15.84 | 19.81 | 946 |
| bc3 | 1 | 218 (3 x 72,7) | 1093..1093 (1093) | 16.52 | 0.35 | 46.6 | 23.15 | 489.56 | 3120 |

