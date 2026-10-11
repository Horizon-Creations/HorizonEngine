CPU je Frame, p50 (ms), Mittel der Läufe (kleinster bis größter):

| Entities | old (3 Läufe) | new (3 Läufe) | full (1 Läufe) | Δ new ms | Δ new % | Δ full ms | Δ full % |
|---|---|---|---|---|---|---|---|
| 1084 | 12.9 (11.2 bis 14.7) | 13.3 (13.0 bis 13.8) | 14.4 (14.4 bis 14.4) | +0.4 | +3 % | +1.5 | +12 % |
| 10174 | 14.5 (14.2 bis 14.8) | 9.6 (9.4 bis 9.9) | 12.2 (12.2 bis 12.2) | -4.9 | -34 % | -2.3 | -16 % |
| 50574 | 70.7 (70.5 bis 70.9) | 43.5 (43.1 bis 43.8) | 56.1 (56.1 bis 56.1) | -27.3 | -39 % | -14.6 | -21 % |
| 101074 | 142.5 (140.7 bis 143.9) | 89.2 (88.2 bis 90.5) | 111.8 (111.8 bis 111.8) | -53.2 | -37 % | -30.6 | -22 % |
| 202074 | 291.6 (289.0 bis 295.4) | 182.5 (178.5 bis 187.1) | 233.4 (233.4 bis 233.4) | -109.1 | -37 % | -58.2 | -20 % |

Scopes, p50 je Frame (ms), Mittel der Läufe:

| Entities | Scope | old | new | full |
|---|---|---|---|---|
| 1084 | RenderExtractor::extract (Summe je Frame) | 0.7 | 0.4 | 0.6 |
| 1084 | Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap) | 0.9 | 0.7 | 0.9 |
| 1084 | Metal::EncodeSSAO | 0.3 | 0.2 | 0.2 |
| 1084 | Metal::EncodeScene | 0.4 | 0.3 | 0.3 |
| 1084 | FrustumCuller::cull (Summe je Frame) | 0.2 | 0.2 | 0.2 |
| 1084 | OnRender | 0.9 | 0.9 | 0.9 |
| 10174 | RenderExtractor::extract (Summe je Frame) | 6.5 | 3.2 | 5.6 |
| 10174 | Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap) | 7.1 | 4.6 | 7.0 |
| 10174 | Metal::EncodeSSAO | 1.9 | 0.7 | 0.8 |
| 10174 | Metal::EncodeScene | 1.9 | 0.8 | 0.8 |
| 10174 | FrustumCuller::cull (Summe je Frame) | 0.6 | 0.6 | 0.6 |
| 10174 | OnRender | 2.6 | 2.6 | 2.6 |
| 50574 | RenderExtractor::extract (Summe je Frame) | 37.7 | 18.7 | 30.8 |
| 50574 | Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap) | 39.9 | 25.1 | 37.0 |
| 50574 | Metal::EncodeSSAO | 10.2 | 4.3 | 4.5 |
| 50574 | Metal::EncodeScene | 10.1 | 4.3 | 4.6 |
| 50574 | FrustumCuller::cull (Summe je Frame) | 3.3 | 3.2 | 3.2 |
| 50574 | OnRender | 8.3 | 8.8 | 8.6 |
| 101074 | RenderExtractor::extract (Summe je Frame) | 75.1 | 37.2 | 62.2 |
| 101074 | Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap) | 79.2 | 50.0 | 74.1 |
| 101074 | Metal::EncodeSSAO | 22.4 | 10.5 | 10.6 |
| 101074 | Metal::EncodeScene | 22.8 | 11.0 | 11.0 |
| 101074 | FrustumCuller::cull (Summe je Frame) | 7.9 | 7.4 | 7.3 |
| 101074 | OnRender | 17.3 | 16.7 | 16.3 |
| 202074 | RenderExtractor::extract (Summe je Frame) | 147.5 | 73.1 | 121.3 |
| 202074 | Schatten samt Walk (old: EncodeShadowMap; new: ExtractFrame + EncodeShadowMap) | 155.5 | 99.0 | 147.5 |
| 202074 | Metal::EncodeSSAO | 48.7 | 24.3 | 24.5 |
| 202074 | Metal::EncodeScene | 51.4 | 27.4 | 27.8 |
| 202074 | FrustumCuller::cull (Summe je Frame) | 19.4 | 18.5 | 18.9 |
| 202074 | OnRender | 35.1 | 33.5 | 37.4 |

Aufrufe je Frame (Median über die Frames, erster Lauf der Gruppe):

| Entities | Gruppe | RenderExtractor::extract | RenderExtractor::reuse | FrustumCuller::cull | Metal::RefineBounds | Metal::ExtractFrame | Transforms::scan | Transforms::propagate |
|---|---|---|---|---|---|---|---|---|
| 1084 | old | 3 | 2 | 5 | - | - | - | - |
| 1084 | new | 4 | 3 | 5 | 1 | 1 | 1 | 1 |
| 1084 | full | 4 | 3 | 5 | 1 | 1 | - | 1 |
| 10174 | old | 3 | 2 | 5 | - | - | - | - |
| 10174 | new | 4 | 3 | 5 | 1 | 1 | 1 | 1 |
| 10174 | full | 4 | 3 | 5 | 1 | 1 | - | 1 |
| 50574 | old | 3 | 2 | 5 | - | - | - | - |
| 50574 | new | 4 | 3 | 5 | 1 | 1 | 1 | 1 |
| 50574 | full | 4 | 3 | 5 | 1 | 1 | - | 1 |
| 101074 | old | 3 | 2 | 5 | - | - | - | - |
| 101074 | new | 4 | 3 | 5 | 1 | 1 | 1 | 1 |
| 101074 | full | 4 | 3 | 5 | 1 | 1 | - | 1 |
| 202074 | old | 3 | 2 | 5 | - | - | - | - |
| 202074 | new | 4 | 3 | 5 | 1 | 1 | 1 | 1 |
| 202074 | full | 4 | 3 | 5 | 1 | 1 | - | 1 |
