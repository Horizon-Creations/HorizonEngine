# Wolken-Pre-Pass auf Metal/M5: Messung der Tier-Demotion (Thema 338, Schritt 2)

Stand 08.11.2026, M5 (MacBook Air), Netzbetrieb, Bildschirm gesperrt, Standard-RenderScale.
Schritt 1 (6b4129650) senkt den Pre-Pass-Raymarch um eine `cloudQuality`-Stufe (Med -> Low,
nur beim Fuellen des Viertel-Aufloesungs-Puffers). Dieser Schritt prueft den Metal-Mirror
und misst die Wirkung auf Metal.

## 1. MSL-Syntax

* `scripts/validate_embedded_shaders.py --msl-only`: **uebersprungen**, `xcrun metal` meldet
  "missing Metal Toolchain" (Xcode-Komponente nicht installiert, siehe Memory zu macos-ld-sdk-mismatch).
  Das Skript prueft MSL nur in CI.
* Ersatz-Zeuge: der Editor compiliert `kSkyMSL` zur Laufzeit per `newLibraryWithSource:`.
  Der Release-Editor mit dem Mirror lief 8 mal im Metal-Pfad (Pre-Pass-Encoder 7 vorhanden,
  `Session summary ... 0 error(s)`), und die Encoder-Zeit aendert sich gegen den Vorgaenger
  (Abschnitt 2): der neue Shader wurde also kompiliert und ausgefuehrt.
* `ctest -R test_sky`: 2/2 gruen; voller `ctest -j4` Release: **364/364 gruen**.

## 2. Metal System Trace, Pre-Pass (Encoder 7), p50 je Lauf, ms

Methode wie Thema 331 Schritt 10: `xctrace record --template 'Metal System Trace' --attach <pid>`,
6 s ab "capture started", Export `metal-gpu-intervals`, nur Prozess HorizonEditor,
je Command Buffer die Encoder, Median ueber ca. 200 bis 500 Frames.
Szene `imp` (Chain + Impostor, Realistic-3D-Wolken, `cloudQuality 1`), Kamera
`0,2.5,-24,3.14159,-0.15`, `HE_SKY_TIME=40000`, abwechselnd Vorher/Nachher.

| Variante | Laeufe (Encoder 7, ms) | Mittel |
|---|---|---|
| Vorher (Med im Pre-Pass) | 5.69, 6.01, 6.09, 6.71 | 6.12 (Median 6.05) |
| Nachher (Low im Pre-Pass) | 5.02, 4.95, 5.06, 5.02 | 5.01 |
| Kontrolle `cloudQuality 0` Vorher (Low, unberuehrt) | 5.01 | |
| Kontrolle `cloudQuality 0` Nachher | 4.97, 5.07 | 5.02 |

* **Nachher == Low-Boden (5.0 ms)**: die Demotion greift genau wie gedacht, Low bleibt Low.
* **Senkung auf Metal: ca. 1.0 ms = 12 bis 17 %** (Median-gegen-Median -17 %, bester Vorher-Lauf -12 %),
  **nicht** die 41 bis 45 % von OpenGL/RTX 4070 aus Schritt 1. Der Unterschied Med -> Low ist auf
  dem M5 nur ca. 1 ms (Thema 331 hatte 2.05 ms, in einer anderen Taktlage gemessen).
* Die absoluten Zahlen sind kleiner als die 6.46 ms aus Thema 331 (dort 6.46 Vorher, hier 5.7 bis 6.7):
  GPU-Takt / Fremdlast (Systemlast 6 bis 12, GPU-Auslastung vor dem Lauf 67 bis 87 %, Aerial-Wallpaper
  des gesperrten Bildschirms) streuen die Vorher-Laeufe um +-0.5 ms, der Nachher-Wert war stabil.
* Summe aller Encoder (p50): Vorher 13.7 bis 14.8 ms, Nachher 13.0 bis 13.6 ms.

**Der Low-Boden von 5.0 ms ist die eigentliche Grenze:** weiter senkt das nur noch Amortisierung
(jeder zweite Frame / Schachbrett, Scissor auf den Himmel, 1/8-Aufloesung), also die Hebel, die
dieser Schritt ausdruecklich nicht baut. Das Ziel "-41 bis 45 %" ist mit der Demotion auf Metal nicht
erreichbar; erreichbar sind ca. -1 ms.

## Fallen dieses Laufs

* **Kopierter Deploy laedt die dylibs des Build-Baums** (absolute LC_RPATH): die ersten beiden
  Serien maßen zweimal denselben Code. `scripts/perf/selfcontain_deploy_copy.sh` vor jedem A/B.
* `xctrace record --attach` hing 2 von 6 + 3 von 8 Laeufen beim "ending recording" (0 % CPU),
  auch ohne sichtbaren Grund; ein Timeout mit `kill -9` und Wiederholung hilft.
  Anhaengen erst nach "Profiler: capture started" (Editor-stdout ist bei Umleitung blockgepuffert:
  die `HorizonEngine.log` im Deploy-Ordner pollen, nicht stdout).
* `encsum.py` aus Thema 331 muss nach Prozess filtern (WindowServer-Zeilen im selben Export) und
  den Kanal `Compute` kennen.

## Handoff

* **Done / verified:** MSL-Mirror laeuft zur Laufzeit auf dem M5 (kein Shader-Fehler), `test_sky` 2/2,
  voller ctest 364/364, Trace-A/B mit selbstenthaltenen Kopien, Kontrolle Low-Boden.
* **Not verified:** `validate_embedded_shaders.py --msl-only` (Toolchain fehlt), Pixel-A/B auf Metal
  (nur GL in Schritt 1), Wirkung bei `cloudQuality 2` (High -> Med) auf Metal, andere Szenen.
* **Decision (not taken):** Ziel -41 bis 45 % gilt nur fuer GL/RTX. Auf Metal braucht es Amortisierung,
  wenn die Wolken unter ~5 ms sollen.
* **Next steps:** 1. Queen entscheidet ueber Amortisierung (Schachbrett / jeder 2. Frame, Scissor
  Himmel, 1/8-Aufloesung) als eigenen Schritt, optisch pruefen. 2. Optional Pixel-A/B auf Metal
  (`he_shot.py`, `HE_SKY_TIME=40000`, Realistic + Classic). 3. CI-Lauf abwarten.
* **Raw:** `/tmp/wk2` (Traces, `xt.sh`, `encsum.py`), Zusammenfassung angehaengt.
