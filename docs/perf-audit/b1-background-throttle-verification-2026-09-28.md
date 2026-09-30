# B1: Hintergrund-Drossel, Verifikation (Thema 101, Schritt 2, 28.09.2026)

Prüft Commit `eee8f7c2` (verstecktes/verdecktes/minimiertes Fenster auf 15 FPS,
Bild-Läufe ausgenommen) auf dem M5: Vollbau, Testsuite, GPU-Last vorher/nachher
und die Bild-Läufe.

## Build und Tests

- Debug-Vollbau `cmake --build build -j8`: RC 0, keine Fehler.
- `ctest -j6` im selben Baum: **194 bestanden, 0 fehlgeschlagen, 3 übersprungen**
  (`runtime_size*`, brauchen ein Release-Deploy). Darunter `test_hidden_window`
  und `test_app_todo` (startet HorizonGame mit `HE_HIDDEN_WINDOW=1
  HE_EXIT_AFTER_FRAMES` + `HE_CAPTURE_FRAME`).
- Für die Messung ein eigener Release-Build (`out/build/rel`,
  `-DHE_BUILD_TESTS=OFF -DDEPLOY_DIR=out/deploy-rel`). Der Debug-Editor kam im
  Stromsparmodus in über 10 Minuten nicht über `BuildSkyNoise3D` hinaus
  (`sample`), der Release-Editor rendert nach ~30 s.

## GPU-Last des versteckten Editors

Aufbau wie der verwaiste Editor aus dem Audit (Befund B1): privates HOME, Kopie
von HorizonTutorial, Metal, `HE_SKY_TIME=30`, `HE_HIDDEN_WINDOW=1`, kein
Bild-Lauf. „aus" = `HE_BACKGROUND_FPS=0`, also das Verhalten vor `eee8f7c2`, im
selben Binary. 20 s Einschwingen, dann 20 s GPU-Zeit (IORegistry
`accumulatedGPUTime`) und CPU-Zeit dieses einen Prozesses.
Skript: `scripts/perf/hidden_window_gpu.py`.

| Lauf | Drossel | GPU | CPU | Log-Zeile |
|---|---|---|---|---|
| A3 | aus | 40,4 % | 14,7 % | Background throttle off (HE_BACKGROUND_FPS=0) |
| B3 | an | **8,3 %** | 2,3 % | Window in background — throttled to 15.0 FPS |
| A4 | aus | 42,6 % | 16,8 % | Background throttle off (HE_BACKGROUND_FPS=0) |
| B4 | an | **8,2 %** | 2,3 % | Window in background — throttled to 15.0 FPS |

Rechner ruhig (Load 1–2, GPU ohne Lauf: nur WindowServer 0,1 %), Akku,
Stromsparmodus an, Bildschirm nicht gesperrt.

- **GPU-Last −80 %** (≈41 % → ≈8 %), CPU −85 % (≈16 % → 2,3 %).
- Plausibel: 8,2 % bei 15 FPS sind ~5,5 ms GPU pro Bild. Mit derselben
  Bildzeit ergeben 41 % ungedrosselt ~75 FPS, genau die 72–77 FPS, die das
  Audit für ein verstecktes Fenster gemessen hat. Die „aus"-Werte treffen auch
  den Mittelwert des alten Orphans (~39 %).
- Eine erste Serie (A1/B1/A2/B2) lief versehentlich unter fremder CPU-Vollast
  (10× `yes` + weitere Editoren, Thema 102). Sie ergab dieselben Werte
  (39,2/41,0 % gegen 8,1/8,1 %).

**Nicht gemessen:** ein sichtbares, dann minimiertes oder verdecktes Fenster.
Das ginge nur per GUI (Fenster erscheint auf dem Bildschirm des Menschen,
osascript fragt nach Rechten). Der Codepfad ist derselbe
(`Window::IsInBackground` prüft HIDDEN | OCCLUDED | MINIMIZED), gemessen ist
aber nur HIDDEN, und auch die Rückkehr („Window back in view") ist nicht am
echten Fenster gesehen.

## Bild-Läufe bleiben unverändert

**he_shot (Dump in `OnInit`)**, Release-Editor, `HE_SKY_TIME=30 TOD=0.5
COVERAGE=0.6 AA=0 RENDERPATH=0 PITCH=10`:

| Lauf | md5 |
|---|---|
| Standard | `b2a651f8…` |
| Standard (Kontrolle, zweiter Lauf) | `b2a651f8…` |
| `HE_BACKGROUND_FPS=0` | `b2a651f8…` |
| COVERAGE=0 (muss abweichen) | `187df04d…` |

Bitgleich, Bild vollständig (Himmel mit Wolken, Mittelwert 182), das Orakel
reagiert auf eine echte Änderung.

**Hauptschleife mit Frame-Budget**: versteckter Release-Editor mit Projekt,
`HE_EXIT_AFTER_FRAMES=300 HE_CAPTURE_FRAME=296`:

| Lauf | Frames 1→300 | Aufnahme md5 | Log |
|---|---|---|---|
| Standard | 9,1 s | `839eb481…` | Background throttle off (HE_EXIT_AFTER_FRAMES) |
| `HE_BACKGROUND_FPS=0` | 8,9 s | `839eb481…` | Background throttle off (HE_EXIT_AFTER_FRAMES) |
| Standard | 9,1 s | `839eb481…` | Background throttle off (HE_EXIT_AFTER_FRAMES) |

Gedrosselt bräuchten 300 Frames mindestens 20 s. Keine Zeile „Window in
background", Aufnahme vollständig (Tutorial-Szene mit Würfel, Gitter, Himmel)
und in allen drei Läufen bitgleich, RC 0.
