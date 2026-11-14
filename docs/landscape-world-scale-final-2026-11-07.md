# Landscape im Weltmaßstab: Abschlussbericht (Schritt 7, 2026-11-07)

Branch `claude/landscape-weltmassstab-100km`, Head-Commit `5295c34fa` (Thema 326, Hive-Schritt 12).
Gerät: NN-WS03 (Ryzen 9 9950X, RTX 4070 12 GB, Windows 11, Release-Build `out/build/x64-release`).

Dies ist der zweite Versuch von Schritt 7 — der erste (Hive-Schritt 8) wurde durch die Deadline abgebrochen,
ohne Bericht. Dieser Bericht fasst zusammen, was auf diesem Gerät heute tatsächlich gelaufen ist, listet die
offenen Punkte aus den Handoffs der Schritte 9–11 auf (ohne sie neu zu untersuchen) und sagt ehrlich, was für
0.8.0 noch fehlt.

## 1. he_tests (komplette Suite)

`ctest --test-dir out/build/x64-release -j 4` (unter vcvars64, 32 Kerne, davon `-j 4` für ctest): **383/383
Tests bestanden**, Gesamtzeit 172 s (plus 15,65 s separat für `sdk_example_project`, s.u.).

Ein Lauf ohne vorher geladene MSVC-Umgebung (`vcvars64.bat`) zeigt 382/383: `sdk_example_project` startet
selbst eine CMake-Konfigurationsprobe, die ohne `INCLUDE`/`LIB` aus vcvars keinen Compiler findet
(`cl.exe - broken`). Mit vcvars geladen läuft er in 15,65 s durch. Das ist ein Artefakt der Aufruf-Umgebung,
keine Regression — Lektion für das nächste Mal: `ctest` auf diesem Gerät immer unter vcvars64 starten, sonst
meldet genau dieser eine Test fälschlich rot.

Die von früheren Mac-Handoffs bekannte Störung (`test_hpak` „Disk registry: scanContentDirectory resolves
UUIDs from loose content" 7 != 2) tritt auf Windows **nicht** auf — bestätigt plattformspezifisch.

## 2. landscape_gpu_check je Backend (separat, `--only`)

Build frisch (`cmake --build out/build/x64-release --target landscape_gpu_check -j 8`, unter vcvars64 — ohne
vcvars bricht die Kompilierung mit `cstdint`/`cmath` nicht gefunden ab, ebenfalls ein reines Umgebungsartefakt).
`Shaders/*.spv` liegt neben der exe (von CMake kopiert, Fix aus Schritt 9 weiterhin wirksam).

Jeder Lauf einzeln mit `--only <backend>`, Logs unter `out/gpucheck_step12/<backend>_run.log`:

| Backend | Offscreen (ramp/hills/world-uv) | Engine, forward | Engine, deferred | Ergebnis |
|---|---|---|---|---|
| OpenGL | OK (depth bad 3/80176 px, worst 1/255) | OK (100 % Boden) | OK | **grün** |
| Vulkan 1.4.357 | OK (depth bad 9/80176 px, worst 1/255) | OK (100 % Boden) | OK | **grün** |
| D3D11 | **3 FAIL** (Normal+Mask-Unterprüfung, s.u.) | OK (100 % Boden) | OK | gelb — bekannte Lücke |
| D3D12 | **3 FAIL** (identisch zu D3D11) | OK (100 % Boden) | OK | gelb — bekannte Lücke |
| Metal | — | — | — | **nicht verifiziert** (kein Mac in dieser Session erreichbar) |

**D3D11/D3D12-Befund (kein neuer Fund, bestätigt eine bereits gemeldete Lücke):** die Unterprüfung
„world uv, normal + mask" zeigt bei beiden D3D-Backends für alle sechs Varianten (keine Karten, flache
Normale, ±z, ±x, AO 1, AO 0,5) exakt dieselbe mittlere Luminanz 123,48 — das Normal+Mask-Array hat auf D3D
keinen messbaren Effekt. GL und Vulkan zeigen dieselbe Szene mit sauber variierender Luminanz (124,07 … 59,52
je Normalrichtung, 113,59 bei AO 0,5). Das deckt sich mit der Meldung von `landscape-fw-uv` (Thema 326, vor
5 Tagen): „der HLSL-Port der D3D-Session liest [uNormalArray/uMaskArray] noch NICHT". Dieser Schritt ändert
daran nichts (Auftrag: keine Code-Änderung) — siehe „Fehlt für 0.8.0" unten.

**Mehrere Backends in einem Prozess (ohne `--only`):** zur Gegenprüfung einmal ohne `--only` gestartet
(Exit-Code 125, Absturz während „Vulkan engine, forward"-Initialisierung, nach GL→D3D11→D3D12 in derselben
Prozessinstanz). Bestätigt exakt die Lektion aus Schritt 9 („landscape_gpu_check on Windows/4070"): ein
Renderer-seitiger Fund für Thema 1521+, nicht für diesen Schritt zu beheben. Mit `--only` pro Aufruf (wie oben)
läuft jedes Backend einzeln sauber durch.

## 3. CI

Run [42418411015](https://github.com/Horizon-Creations/HorizonEngine/actions/runs/42418411015) auf Head
`5295c34fa` (der Fix aus Schritt 11): zum Zeitpunkt dieses Berichts (07.11., kurz nach 23:30) **3 von 4 Jobs
grün** (Linux · Vulkan/lavapipe, Linux, macOS), **Windows-Job noch `in_progress`** (im Build-Schritt). Dies ist
exakt der Job, den der Fix aus Schritt 11 betrifft — bisher kein Hinweis auf einen neuen Fehler. Nicht
abgewartet (Vorgabe: CI nicht pollen); der nächste, der dieses Thema anfasst, oder die Queen sollte
`gh run view 42418411015` vor einem Merge noch einmal prüfen.

## 4. Verifiziert (Übersicht über alle Schritte dieses Themas)

| Schritt | Was | Backend/Gerät | Wann |
|---|---|---|---|
| 2 (Quadtree/Renderer) | Quadtree-Landschaft zeichnet, Atlas, Höhen-Textur | GL + Metal (Mac), D3D11 + D3D12 (NN-WS03), Vulkan (NN-WS03 RTX 4070, echte Hardware) + Vulkan (Mac via MoltenVK) | 01.–06.11. |
| 4 (GI/Schatten) | GI-Sonnenschatten sehen die Kachel-Landschaft (Höhen-Clipmap) | GL, Vulkan, D3D11 | Schritt 10 (02.11.) |
| 11 (CI-Fix) | `LandscapePackSource::open` mit `FILE_SHARE_DELETE` unter Windows | Windows CI | Schritt 11 (heute) |
| 12 (dieser Schritt) | he_tests komplett, landscape_gpu_check je Backend | Windows (D3D11/D3D12/Vulkan/OpenGL), NN-WS03 RTX 4070 | 07.11. |

## 5. Bewusst ausgeklammert (aus früheren Handoffs, hier nicht neu geprüft)

* **Metal** — kein Mac in dieser Session erreichbar; zuletzt in Schritt 2 (OpenGL+Metal, Mac) und für Vulkan
  separat (MoltenVK, Schritt „landscape-vulkan-mac") verifiziert, nicht aber mit dem heutigen Stand (Commit
  `5295c34fa`) erneut gegengeprüft.
* **Reflexionen gegen die Kachel-Landschaft** — laufen auf allen fünf Backends über BVH/BLAS-Trefferlisten,
  nie über das Höhenfeld; die Kachel-Landschaft hat auf KEINEM Backend eine BVH/BLAS-Integration. Die
  Höhenfeld-Schatten-Optimierung existiert nur auf GL/Vulkan/D3D11 — Metal und D3D12 haben dort nur BVH/DXR
  (Befund aus Schritt 10, `docs/landscape-world-scale-gi-2026-11-02.md`).
* **Sculpt/Paint-Rückschreiben auf Kacheln** — nur Entwurf (`docs/landscape-sculpt-paint-writeback-plan-*.md`,
  Commit `06a28d803`), kein Laufzeitcode. „Region bearbeiten" ebenfalls nicht gebaut (Schritt 6/7-Handoff,
  Run 1844).
* **Live-Editor-Durchlauf** — HorizonEditor wurde für diesen Schritt nicht gebaut (Auftrag: kein Editor-Build);
  ein echtes 100-km-Generieren/Migrieren über die UI, native Dateidialoge, wurde seit Schritt 6 nicht erneut
  im echten Editor gefahren.

## 6. Fehlt für 0.8.0 (konkrete, bekannte Lücken)

1. **D3D11/D3D12: Normal+Mask-Arrays im Landschafts-Pixel-Shader nicht angebunden** — siehe §2. GL/Vulkan/Metal
   lesen `uNormalArray`/`uMaskArray` (Welt-UV), der HLSL-Port (`kLandscapeHLSL`, von D3D11 und D3D12 geteilt)
   tut es nicht. Reiner Feature-Nachzug, keine Korrektheitsgefahr (die Landschaft zeichnet, nur ohne die
   zusätzliche Schattierung).
2. **Sculpt/Paint-Rückschreiben auf Tiles** — nur Entwurf, kein Code (s.o.). Ohne das ist eine 100-km-Welt im
   Editor nicht von Hand nachbearbeitbar, nur generierbar/importierbar/migrierbar.
3. **Projekt-relative Pack-Pfade** — `LandscapeWorldComponent`-Kommentar verspricht es, `LandscapeRuntime`
   öffnet weiterhin CWD-relativ/absolut (Lektion aus Schritt 6/7).
4. **Reflexionen gegen die Kachel-Landschaft** — auf keinem Backend implementiert (s.o., §5).
5. **Mehrere Backends in einem Windows-Prozess crashen** (`landscape_gpu_check` ohne `--only`) — ein
   Renderer-seitiger Fund (Thema 1521+), nicht blockierend für den Spielbetrieb (Spiel/Editor laufen immer nur
   mit einem Backend gleichzeitig), aber ein offenes Risiko für künftige Mehrbackend-Werkzeuge.
6. **Metal** auf dem aktuellen Stand nicht gegengeprüft (kein Mac erreichbar in dieser Session).

## 7. Fazit

Kernarchitektur (Schritte 0–6: Kachelspeicher, Quadtree-Renderer, Physik/Nav/Foliage/Wasser je Kachel,
Floating Origin, Regionen-Entladen, HLOD-Backen, Editor-Werkzeuge) ist auf allen vier Windows-Backends lauffähig
und die komplette Testsuite ist grün. Die einzige echte Lücke, die die Verifikation selbst aufgedeckt hat
(D3D-Normal+Mask), ist seit 5 Tagen bekannt und dokumentiert, nicht neu. Für 0.8.0 reicht der heutige Stand
als Basis; Punkte 1–4 in §6 sind Folgearbeit, kein Blocker für den Merge nach `release/0.8.0`, sofern die
Windows-CI (s. §3) grün durchläuft.

## Handoff

* **Done:** he_tests 383/383 grün (vcvars-Artefakt bei `sdk_example_project` identifiziert und durch
  Neulauf unter vcvars64 aufgelöst, keine echte Regression). `landscape_gpu_check` je Backend einzeln
  (`--only gl|d3d11|d3d12|vulkan`) auf NN-WS03/RTX 4070 gelaufen, Logs unter
  `out/gpucheck_step12/{gl,d3d11,d3d12,vulkan}_run.log`. CI-Run 42418411015 geprüft (3/4 grün, Windows noch
  am Bauen). Dieser Bericht geschrieben.
* **Pending:** Windows-CI-Job von Run 42418411015 noch nicht fertig — vor einem Merge erneut prüfen
  (`gh run view 42418411015`). Metal nicht gegengeprüft.
* **Decisions:** keine Code-Änderung vorgenommen (Auftrag explizit „keine Code-Änderungen"); D3D-Normal+Mask-
  Lücke als bekannt/nicht blockierend eingestuft, nicht behoben.
* **Fallen:** `ctest`/der Build von `landscape_gpu_check` brauchen auf diesem Gerät `vcvars64.bat` geladen,
  sonst meldet `sdk_example_project` falsch rot bzw. die Kompilierung bricht an `cstdint`/`cmath` ab — ein
  reines Umgebungsartefakt der Bash-Shell, kein Code-Fehler. `landscape_gpu_check` OHNE `--only` stürzt auf
  diesem Gerät weiterhin ab (Lektion aus Schritt 9, hier erneut reproduziert, nicht neu untersucht).
* **Next steps:** 1. Windows-CI-Ergebnis von Run 42418411015 abwarten/prüfen vor Merge. 2. D3D-Normal+Mask
  (§6.1) als eigener, kleiner Folgeschritt. 3. Sculpt/Paint-Rückschreiben (§6.2) und projekt-relative
  Pack-Pfade (§6.3) bleiben offene, größere Folgearbeit für 0.8.0, falls gewünscht. 4. Reflexionen gegen die
  Kachel-Landschaft (§6.4) ist der größte offene Posten, gehört eher zu einem eigenen Thema als zu 326.
