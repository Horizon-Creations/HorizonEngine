# Diagnose im Dev-Game-Build (Hive-Thema 236), 26.10.2026

Frage: Wie kommt man an Profiler-Daten, wenn ein Problem nur im Spiel auftritt (Wasser-Perf, Pfützen-Flackern), nicht
im Editor?

## 1. Was es schon gab

| Werkzeug | Wo | Spiel? |
|---|---|---|
| F9: Benchmark-Aufnahme, JSON-Dump nach `<exe>/dumps/profile_*.json` | `Application.cpp:311` | ja, in Dev-Builds |
| `HE_PROFILE_CAPTURE/WARMUP/DETAILED/COUNTERS`: dieselbe Aufnahme per Umgebung | `Application.cpp:504` | ja |
| `HE_CAPTURE_FRAME/PATH`: ein Bild als PPM | `Application::Run` | ja |
| F10: Statistik-Karte (FPS, GPU-Frame, Draws, Dreiecke, Objekte, VRAM, GPU-Pässe) | `DevStatsTile.cpp`, `GameApplication.cpp:2943` | ja, wenn Dev-Hotkeys an sind |

Dev-Hotkeys sind an in Debug/RelWithDebInfo, mit `-DHE_GAME_DEV_HOTKEYS=ON` oder im Export mit „Development hotkeys"
(`project.hcfg`, Flag 4096). Ein Release-Profil-Build (SDK-Export) hat nichts davon.

## 2. Was fehlte und jetzt dazukommt

- **Die GPU-Pässe der Karte waren im Spiel leer.** Seit der Profiler die Einzelbild-Aufnahme nicht mehr erzwungen detailliert macht,
  hatte die Karte auf Metal nie eine Pass-Liste.
  - `EngineProfiler::requestSingleFrameCapture(bool detailed)` lässt den Aufrufer den detaillierten Modus für das eine Bild verlangen.
  - Metal liefert die Liste dieses Bildes asynchron ein bis zwei Bilder später. Die Karte nimmt sie darum vom Renderer selbst,
    je Anfrage die vollständigste.
- **CPU-Seite.** Die Karte zeigt die teuersten CPU-Scopes des aufgenommenen Bildes (oberste Ebene und eine darunter, nach Namen summiert).
  - Der Backend-Präfix fällt weg, weil das Backend oben steht.
  - Weil ein detailliertes Bild nach jedem Pass auf die GPU wartet, wechseln sich die Aufnahmen ab: ein normales Bild für die CPU-Zahlen, ein detailliertes für die GPU-Pässe. Jede Art bleibt bis zur nächsten stehen.
- **Frame-Zeit-Streuung.** `Frame p50 / p99 / max` über die letzten 240 Bilder. Ein Hänger zeigt sich dort, wo der FPS-Schnitt ihn versteckt.
  Die Bilder direkt nach einer detaillierten Aufnahme zählen nicht mit.
- **Ohne Taste.**
  - `HE_GAME_STATS=1` zeigt die Karte ab dem ersten Bild.
  - `HE_GAME_STATS_LOG=<Sekunden>` schreibt ihren Text zusätzlich ins Log, in Blöcken unter der 512-Byte-Grenze einer Logzeile.
  - Damit bringt ein Lauf auf einem anderen Rechner seine Zahlen ohne Screenshot zurück.
- **Kompakter.** Schrift 13 statt 15, auf der Karte höchstens 4 CPU-Scopes und 6 GPU-Pässe, 0-ms-Pässe (etwa GI aus) weggelassen.
- **Nebenbei gefixt:** Eine versteckte Karte zieht ihre offene Einzelbild-Anfrage zurück (`cancelSingleFrameCapture`).
  Sonst startete die nächste Aufnahme für niemanden, und `test_profiler` war nach `test_dev_stats_tile` rot (die zwei Fälle, die in vollen Läufen fielen).

Beispiel, Metal, 50k-Referenzwelt, Spiel, verstecktes Fenster (Log mit `HE_GAME_STATS_LOG`):

```
114 FPS   8.78 ms
GPU        5.97 ms
...
CPU        8.48 ms
Frame p50 8.7  p99 9.5  max 9.8

CPU scopes
 Render                  8.36 ms
 ExtractFrame            2.78 ms
 EncodeShadowMap         2.34 ms
 EncodeScene             1.37 ms

GPU passes  (detailed)
 Scene            5.48 ms
 Shadow           2.13 ms
 SSAO             1.06 ms
 ...
```

## 3. Benutzen

```
HE_GAME_STATS=1 HE_GAME_STATS_LOG=5 ./HorizonGame          # Karte an, alle 5 s ins Log
HE_PROFILE_CAPTURE=600 HE_PROFILE_WARMUP=300 ./HorizonGame  # 600 Bilder als JSON-Dump (dumps/)
```

Im Spiel selbst: F10 Karte, F9 Aufnahme (Dev-Hotkeys an).

## Handoff

- **Topic / step:** Hive 236, Schritte 1 (Entwurf, dieses Dokument) und 2 (Umsetzung). Zweig `claude/game-dev-diagnostics` ab `release/0.7.0` (b128c97b).
- **Files touched:**
  - `src/HE_Core/{include,src}/Diagnostics/DevStatsTile.*`
  - `EngineProfiler.h/.cpp` (`requestSingleFrameCapture(bool)`, `cancelSingleFrameCapture`)
  - `src/HE_Game/src/GameApplication.{h,cpp}` (`HE_GAME_STATS`)
  - `tests/test_dev_stats_tile.cpp`
- **Verified:**
  - Build Release (Worktree, ccache) rc 0.
  - `test_dev_stats_tile` + `test_profiler` grün, auch zusammen und in zufälliger Reihenfolge.
  - HorizonGame auf Metal und OpenGL mit der 50k-Welt: Karte im Bild (PPM-Aufnahme angesehen) und im Log, Metal mit allen GPU-Pässen, OpenGL meldet ehrlich keine.
- **Not verified:** Vulkan, D3D11, D3D12. Die Karte ist backend-unabhängig (UI-Widgets); ob deren Pass-Liste kommt, hängt an ihren Timestamp-Queries.
- **Next steps:** Auf Windows einmal F10 mit Vulkan/D3D12 ansehen (Pass-Liste vorhanden?).
