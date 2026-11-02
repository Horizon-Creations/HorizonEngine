# D3D11, D3D12, Vulkan (plus OpenGL als Referenz): Abnahme Wasser-Realismus (Thema 206 Schritt 5)

Stand 2026-10-20, NN-WS03. Ziel: die vier Verbesserungen aus den Schritten 2–4 und 6
(weicher Ufer-Übergang, feineres unregelmässiges Chop, klare Untiefen mit Tiefen-
Überblendung, Surface/ShallowWater als Vec4-Knöpfe) auf echter D3D11-, D3D12- und
Vulkan-Hardware prüfen — kein Mac auf diesem Gerät verfügbar, deshalb OpenGL als
Windows-seitige Referenz statt Metal.

## 0. Hardware, Treiber, Build

| | |
|---|---|
| Rechner | NN-WS03, Windows 11 Pro (10.0 build 26300) |
| CPU | AMD Ryzen 9 9950X, 32 Hardware-Threads |
| GPU | NVIDIA GeForce RTX 4070, Treiber 32.0.16.1088 (610.88) |
| Build | Release, Ninja/MSVC (vcvars64, VS 18 Community — nicht VS2022), `C:/hw206`, `-DDEPLOY_DIR` isoliert vom laufenden Editor-Deploy |
| Commit | 4e6e5eb4 (alle sechs Wasser-Schritte), plus dieser Schritt: `tests/test_inspector_ui.cpp` |
| he_tests | gebaut, rc 0 |

Die vorangehenden zwei Läufe (1080, 1081) dieses Schritts sind ohne Ergebnis
verschwunden, weil `cmake --build` direkt aus der Bash-Shell ohne geladenes
MSVC-Environment lief (`cstdint`/`stdarg.h` nicht gefunden — kein Code-Fehler). Lehre
dazu gepostet (`hive_lesson` 483). Dieser Lauf baut über PowerShell mit
`vcvars64.bat` und pollt aktiv auf `BUILD_RC=` im Log statt auf eine Benachrichtigung
zu warten.

## 1. Fund und Fix: Inspector-UI-Test war nicht auf 14 Parameter nachgezogen

Commit 4e6e5eb4 (Schritt 6) hat `tests/test_engine_materials.cpp` auf die neue
Parameterzahl angepasst, aber `tests/test_inspector_ui.cpp` übersehen: der Test
"inspector ui: the engine water material shows all sixteen parameters…" erwartete
weiterhin `names.size() == 16` benannte Graph-Parameter. Seit Schritt 6 sind es 14
(Surface und ShallowWater bündeln je vier Floats unter einem Namen; 16 Float-Slots,
14 benannte Parameter). `he_tests --test-case="*water*"` zeigte das als einzigen
Fehlschlag:

```
test_inspector_ui.cpp(929): FATAL ERROR: REQUIRE( names.size() == 16 ) is NOT correct!
  values: REQUIRE( 14 == 16 )
```

Fix: `REQUIRE(names.size() == 16)` → `14`, Testname und die zwei Kommentare, die noch
"sixteen" sagten, auf vierzehn/vierzehn benannte Parameter korrigiert (kein
Verhaltens-Code geändert, nur die Erwartung). Einzel-Datei-Rebuild (ein `.obj`, kein
Header angefasst) bestätigt den Fix: **270/270 Testfälle, 404064/404064 Assertions,
0 Fehlschläge** auf `*water*`.

Breiterer Filter `*material*,*inspector*` (Sicherheitsnetz für den Rest des
Wasser-Themas): 194/196 Testfälle, 2 Fehlschläge — beide `ImGui::GetClipboardText()
== nullptr` in `test_inspector_ui.cpp:444` und `:509` (Rechtsklick-Kopieren bei
Komponenten/Replication). Vorbekanntes, umgebungsbedingtes Rauschen (kein
OS-Zwischenablagezugriff im Headless-Lauf dieses Geräts, siehe Memory "Pre-existing
Vulkan/D3D/ctest noise" → "inspector_ui clipboard"), nicht wasserbezogen, nicht in
diesem Schritt behoben.

`scripts/editor_help_audit.py --check`: **1204/1204 Controls gedeckt, 0 offen.**

## 2. Matrix: 12 Ansichten × 4 Backends, Zeugnis-Dumps

`scripts/water_baseline_shots.py` (vom vorigen Lauf vorbereitet) fährt den
gebauten Editor selbst an (eigenes `HE_CONFIG_DIR`, BMP-Stabilitäts-Poll, Kill nach
Dump), 1280×720, dieselben zwölf Ansichten wie die Metal/GL-Baseline der Schritte
1–4 (open, open-hi, far, shore, shore33, shoresheet33, beach, beach33, beach-smooth,
beach-sheet, shallow, shallow-hi). **48/48 Läufe (D3D11, D3D12, Vulkan, OpenGL ×
12 Ansichten) liefern ein gültiges 1280×720-RGB-PNG, keine FAILED-Zeile, keine
leeren/schwarzen Bilder.** Volle Liste inkl. md5 je Bild: `scripts/water_baseline_shots.py
C:/hw206/shots --backends D3D11,D3D12,Vulkan,OpenGL`.

### Zahlenvergleich (scripts/water_backend_diff.py, mean|Δ| / max|Δ| / %px>2 über RGB)

| Ansicht | D3D11 vs D3D12 | D3D11 vs Vulkan | D3D11 vs OpenGL |
|---|---|---|---|
| open | 0.000 / 1 / 0.00% | 0.150 / 5 / 0.01% | 30.349 / 137 / 78.40% |
| open-hi | 0.000 / 1 / 0.00% | 0.248 / 5 / 0.10% | 31.167 / 92 / 71.43% |
| far | 0.600 / 52 / 7.64% | 4.563 / 88 / 25.69% | 25.535 / 161 / 84.34% |
| shore | 0.263 / 13 / 4.15% | 1.380 / 75 / 5.71% | 26.585 / 69 / 90.05% |
| beach | 0.000 / 1 / 0.00% | 0.394 / 55 / 0.17% | 15.247 / 102 / 100.00% |
| shallow | 0.000 / 1 / 0.00% | 0.257 / 36 / 0.12% | 12.890 / 125 / 92.60% |
| shallow-hi | 0.000 / 1 / 0.00% | 0.356 / 6 / 0.21% | 13.950 / 130 / 100.00% |

**D3D11 und D3D12 sind praktisch bitgleich** (mean|Δ| 0.0–0.6, nur `far`/`shore`
etwas höher — Sky-Zeit-Drift über `SDL_GetTicks`, nicht wasserbezogen, bekannt aus
Thema 185). **D3D11 und Vulkan sind sehr nah** (mean|Δ| 0.15–4.6; `far`/`shoresheet33`
etwas höher, gleiche Ursache). **D3D11 gegen OpenGL** liegt bei mean|Δ| 13–31 —
deutlich mehr als die ~14 Einheiten aus der Lake-Werkzeug-Abnahme vom 2026-10-12, aber
geprüft gegen die Historie **vorbekannt und nicht wasserspezifisch**: Thema 185
(`docs/underwater-acceptance-2026-10-12.md` §8.1) misst "D3D/Vulkan liegen gegen GL
bei etwa 25" sogar mit Unterwasser-Effekten AUS; `docs/underwater-windows-acceptance-
2026-10-17.md` §3 zeigt auf genau diesem Gerät mean|Δ| 12.71–25.01 je nach Ansicht,
mit dem Befund "die bekannte hellere D3D/Vulkan-Szene". Visuell bestätigt (unten):
gleiches Wellenbild, gleiche Glanzlichter, gleiche Komposition — nur ein globaler
Ton-/Helligkeitsunterschied, keine strukturelle Abweichung.

### Bilder (Auswahl, volle 48 unter `C:/hw206/shots` auf diesem Gerät)

| | |
|---|---|
| D3D11 open | [d3d11-open.png](water-hardware-acceptance-2026-10-20/d3d11-open.png) |
| D3D12 open (fast bitgleich zu D3D11) | [d3d12-open.png](water-hardware-acceptance-2026-10-20/d3d12-open.png) |
| Vulkan open | [vulkan-open.png](water-hardware-acceptance-2026-10-20/vulkan-open.png) |
| OpenGL open (Referenz, dunklerer Ton) | [opengl-open.png](water-hardware-acceptance-2026-10-20/opengl-open.png) |
| D3D11 open-hi | [d3d11-open-hi.png](water-hardware-acceptance-2026-10-20/d3d11-open-hi.png) |
| D3D11 far | [d3d11-far.png](water-hardware-acceptance-2026-10-20/d3d11-far.png) |
| D3D11 shore (weicher Ufer-Übergang) | [d3d11-shore.png](water-hardware-acceptance-2026-10-20/d3d11-shore.png) |
| Vulkan shore | [vulkan-shore.png](water-hardware-acceptance-2026-10-20/vulkan-shore.png) |
| D3D11 beach | [d3d11-beach.png](water-hardware-acceptance-2026-10-20/d3d11-beach.png) |
| D3D11 beach-smooth | [d3d11-beach-smooth.png](water-hardware-acceptance-2026-10-20/d3d11-beach-smooth.png) |
| D3D11 shallow (Durchsicht aufs Ufer) | [d3d11-shallow.png](water-hardware-acceptance-2026-10-20/d3d11-shallow.png) |
| Vulkan shallow | [vulkan-shallow.png](water-hardware-acceptance-2026-10-20/vulkan-shallow.png) |
| D3D11 shallow-hi | [d3d11-shallow-hi.png](water-hardware-acceptance-2026-10-20/d3d11-shallow-hi.png) |

## 3. Abgleich gegen die Metal/GL-"after"-Bilder der Schritte 2–4

Pixel-für-Pixel nicht möglich (andere Hardware, kein gemeinsamer md5), aber
Komposition und Struktur verglichen gegen `docs/water-realistic-baseline/after-
metal-*.png` bzw. `transp-after-metal-*.png`:

- **Ufer-Übergang (Schritt 2):** `d3d11-shore.png` gegen `after-metal-shore.png` —
  dieselbe weiche, organische Kante mit Schaumsaum, keine zackige Linie, dasselbe
  Wellenmuster und dieselben Glanzlicht-Positionen auf beiden Bildern. Der
  Ufer-Übergang ist also nicht Metal-spezifisch, sondern sitzt im Material-Graph und
  wirkt identisch auf D3D11/D3D12/Vulkan.
- **Durchsichtigkeit (Schritt 4):** `vulkan-shallow.png` gegen `transp-after-metal-
  shallow.png` — na­hezu deckungsgleiche Komposition: dieselbe diagonale
  Übergangslinie zwischen tiefem und klarem Wasser, derselbe graue Untergrund, der
  durchscheint, dieselben Schaumflecken an derselben Stelle. Vulkan zeigt die klare
  Untiefe also genauso wie Metal.
- **Chop/Wellen (Schritt 3):** `d3d11-open.png`/`d3d11-far.png` zeigen dasselbe
  feine, unregelmässige Kräuselmuster über mehrere Grössenskalen wie die Mac-
  Referenzbilder (`waves-after-metal-*.png`, Schritt-3-Doku) — keine sichtbare
  Backend-spezifische Glättung oder Aliasing-Differenz in den Standardansichten.
- **Menschliche Referenzfotos** (`.hive/dateien/28–31`): die offene Ansicht
  (`d3d11-far.png`) zeigt dasselbe Muster wie in den Schritten 1–3 schon festgestellt
  — kurze, unregelmässige Wellchen mit Glanzlichtern nur auf den Flanken, keine
  flächige Glätte mehr. Keine neue Abweichung zu den Referenzfotos gegenüber dem,
  was Schritt 3 bereits maß.

## 4. GPU-Debug-/Validation-Layer

`HE_GPU_DEBUG=1`, Ansicht `open` (`WATERTEST=floor`), D3D12 und Vulkan:

| Backend | [ERROR] | [ WARN] |
|---|---|---|
| D3D12 | **0** | 3 (dieselbe bekannte `ID3D12CommandList::ClearRenderTargetView`-Meldung aus Thema 174 Schritt 10 §3, nicht wasserbezogen) |
| Vulkan | **0** | 0 |

Positiv-Kontrolle (derselbe Kanal zeigt im D3D12-Fall zuverlässig 3 echte
Warnungen statt pauschal 0/0 zu melden) aus der Hardware-Abnahme vom 2026-10-12
übernommen, hier erneut bestätigt — der Kanal ist also nachweislich nicht stumm.
D3D11 hat weiterhin keinen Debug-Layer-Codepfad im Renderer (vorbestehende Lücke,
nicht wasserspezifisch, nicht in diesem Schritt behoben).

## 5. Nicht in diesem Schritt

- Metal: kein Mac auf diesem Gerät. Visueller Komposition-Abgleich gegen die
  vorhandenen Metal-"after"-Bilder (§3), kein Pixel-Vergleich.
- Pixel-genaue Cross-Backend-Toleranzschwelle für `far`/`shore` auf D3D12/Vulkan
  (Sky-Zeit-Drift) — bekannte, nicht wasserspezifische Quelle, nicht quantifiziert.
- Die zwei Clipboard-Testfehlschläge (§1) — vorbekanntes Headless-Rauschen, nicht
  behoben.
- D3D11-Debug-Layer (existiert nicht im Renderer).
- Fenster-Resize, Speichern/Laden über MCP, AMD/Intel-GPUs, das exportierte Spiel —
  außerhalb des Rahmens dieses Schritts (Fokus: die vier Wasser-Verbesserungen
  selbst, nicht der allgemeine Rendering-Stack, der in Thema 174 Schritt 10 schon
  geprüft wurde).

## 6. Ergebnis

D3D11, D3D12 und Vulkan zeigen alle vier Verbesserungen aus den Schritten 2, 3, 4
und 6 — weicher Ufer-Übergang, feineres Chop, klare Untiefen mit Tiefen-Blend, die
neuen Surface/ShallowWater-Vec4-Knöpfe — strukturell identisch zu den vorhandenen
Metal/GL-Referenzbildern: D3D11/D3D12 sind untereinander bitgleich (mean|Δ| ≤ 0.6),
D3D11/Vulkan sind sehr nah (mean|Δ| ≤ 4.6), D3D11/OpenGL liegen bei mean|Δ| 13–31 —
derselbe vorbekannte, nicht wasserspezifische Ton-Unterschied wie Thema 185. 0
Validation-Fehler auf D3D12 und Vulkan mit aktivem Debug-/Validation-Layer. Ein
Regressions-Test (`test_inspector_ui.cpp`), den Schritt 6 beim Umbau auf Vec4-Knöpfe
übersehen hatte, ist gefunden und gefixt: `*water*` läuft wieder grün (270/270),
der breitere `*material*,*inspector*`-Filter zeigt nur die zwei vorbekannten
Clipboard-Fehlschläge. Editor-Hilfe-Audit grün (1204/1204).
