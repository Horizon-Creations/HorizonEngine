# Job-System-Umbau: Übergabe an einen Mac

Stand 31.10.2026. Vorgänger: `docs/jobsystem-audit-world-streaming-2026-10-31.md` (Audit, §5 Umsetzungsstand,
dort auch der ausführliche Handoff). Dieses Doc ist nur die Mac-Checkliste: was noch nie auf macOS/Metal gelaufen ist.

## Handoff

- **Topic / step:** Job-System-Audit (World-Streaming), Mac-Verifikation und Metal-Teil, Schritt 2 von n.
- **Branch / commit:** `claude/jobsystem-audit-world-streaming` (PR #215, base `release/0.8.0`), Stand `e8193039` + Doc-Commit dieser Übergabe; gepusht: ja.
- **Done (Linux, g++ 13):** Schritte 1-7 des Audits (Wartezeit-Stats, `promote`, `order`, `JobKind::Io`, Shutdown-Cancel,
  Backlog-Grenze, Pak als Read+Decode, `CellStreamer`-Schlüssel, `SkyNoise3D` auf den Pool). Einzelheiten und Dateien: Audit §5.
- **Verified:** nur Linux. `test_jobsystem` 38/38 (+TSan sauber), `test_async_streaming`/`test_hasset`/`test_hpak`/`test_profiler` 116 Fälle grün,
  `editor_help_audit.py --check` ok. **Nichts davon auf macOS, Apple Clang oder Metal.**
- **Not verified (das ist die Aufgabe auf dem Mac):**
  1. Die **volle** `he_tests`-Suite (hier nur 5 Dateien von Hand gelinkt) und CI-Leg macOS: CI-Lauf des PR ansehen.
  2. Apple Clang: `JobSystem.cpp` nutzt `std::atomic<JobPriority>` und einen Heap aus `std::vector<Entry>` mit `PoolTask` (move-only Anteile),
     `HpakReader::decodeStored` ist `static` mit `StoredEntry&&`. Sollte kompilieren, ist aber nur mit g++ gebaut.
  3. Der Streaming-Tab (*Performance Profiler ▸ Streaming*): neue Spalten *Wait avg/Wait max* und die Zeile „Decoded, waiting for the main thread“. Kein UI-Test, nur Syntax + Help-Audit.
  4. **Kein Nutzen gemessen.** Weder Wartezeit noch Ladezeit noch Frame-Zeit vorher/nachher.
- **Decisions taken:** keine neuen Lanes (Audit §5); Shutdown-Cancel nur für geplante Low-Jobs; Backlog fix 256 MB / 10 s; Io-Limit `clamp(threads/2, 2, 4)`.
- **Decisions suggested, not taken:** siehe unten „Metal-Arbeit“; Io-Limit aus den Projekteinstellungen; QoS-Gruppen.
- **Pending (Reihenfolge):**
  1. **Bauen und testen** (CLAUDE.md §2): `cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release`, dann
     `cmake --build cmake-build-release --target he_tests -j 8` und `ctest --test-dir cmake-build-release -R "test_jobsystem|test_async_streaming|test_hpak|test_hasset|test_streaming_view|test_profiler" --output-on-failure`.
     Danach die ganze Suite, ohne `--no-skip` (der Schalter lässt `test_auto_landscape_variants` abstürzen und kürzt den Lauf still ab, siehe `cpu-threading-implementation-2026-10-27.md`).
     `test_hpak` hat einen bekannten Fehler in `scanContentDirectory`; mit `HOME=<leerer Ordner>` läuft er durch.
  2. **Streaming-Tab ansehen:** Editor mit einer Zellen-Szene (`scripts/perf/make_cell_game_dir.py`, `HE_HIDDEN_WINDOW=1` siehe `docs/headless-runs.md`) starten,
     Tab öffnen, Kamera bewegen. Erwartung: *Wait avg* der Low-Lane steigt unter Last, fällt danach; *Wait max* springt je Halbsekunden-Fenster zurück.
     Screenshot per `HE_UI_DUMP_DIR` + `scripts/he_uishot.py`, falls ein UI-Test dafür gewünscht ist (`tests/test_streaming_view.cpp`).
  3. **Vorher/Nachher messen:** `origin/release/0.8.0` gegen den PR-Kopf, gleiche Szene, mehrere Läufe (Median). Werkzeuge: `scripts/perf/thread_timeline_report.py`,
     `scripts/perf/dump_scope_p50.py`, Profiler-Dump (`HE_PROFILE_WARMUP`/`HE_PROFILE_CAPTURE`, siehe `cpu-threading-audit-2026-10-22.md` §2). Zu messen: Zeit „angefragt → registriert“
     (neu: Queue-Wartezeit), `lastPollMs`, Main-Thread-Spitzen in `ParseAndRegisterAsset`, Speicher-Peak bei einem Welt-Sprung (Backlog-Grenze).
  4. **Metal-Arbeit (Audit B2/B3/C3):** `MetalRenderer.mm:9911` `WarmupMaterials` baut Pipeline + Blend-Zwilling synchron (Kommentar: 0,4-0,7 s je Material auf einer E-Core).
     Plan: Cross-Compile und `newLibraryWithSource` auf dem Pool (`schedule`, `cancel` = Material-Interesse, `onCancelled` gibt den Key frei), `newRenderPipelineStateWithDescriptor`
     mit Completion-Handler, im Encoder-Loop „noch nicht fertig“ = nicht zeichnen. Vorlage fürs Abholen: `D3DShaderPrefetch.h` (`take`).
     Vorher mit Profiler-Scopes belegen, dass dort wirklich die Frame-Spitzen liegen (Audit-Lehre: erst benennen, dann ändern). Dann `waitUntilCompleted` in `MetalRenderer.mm:10321…11604` prüfen,
     welche im Streaming-Pfad liegen (vermutlich Thumbnails/Capture). QoS: `pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0)` je Low-Job im Worker (vor/nach `runClaimed`); dass sich die QoS danach wieder auf den Ausgangswert setzen lässt, ist Annahme aus der Apple-Doku und auf dem Mac zu prüfen (unter Linux geht das Zurücknehmen einer gesenkten Priorität ohne Rechte nicht, deshalb dort nicht gebaut).
- **Traps:**
  - `MetalRenderer.mm`, `EditorApplication.cpp`, `ContentManager.cpp` nie ganz lesen (CLAUDE.md §3); mit `grep -n` und ~100 Zeilen Umfeld arbeiten.
  - `timeout` gibt es auf macOS nicht (Kill-Schleife benutzen); vor jedem Messlauf `pgrep HorizonEditor`, ein vergessener Editor verfälscht die Zahlen und frisst GPU-Zeit.
  - `ContentManager::LoadInterest` hält seine Jobs, die Jobs halten das `LoadInterest` (Zyklus endet beim Fertigwerden, Kommentar im Header). Nicht „aufräumen“, ohne das zu prüfen.
  - `ThreadPoolStats::Lane::queued` zählt tote Einträge; `promote()` legt absichtlich einen zweiten an.
  - Gesperrter Bildschirm macht `NextDrawable` zu 20-40-ms-Warten (siehe `cpu-threading-audit-2026-10-22.md`): Messungen mit entsperrtem Bildschirm.
  - Linux-Build-Hinweis, falls ein Container gebraucht wird: `apt-get install libgl-dev libglx-dev libopengl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev`.
- **Next steps:** 1. PR-CI und lokale Voll-Suite grün machen (Befunde als Commits auf denselben Zweig). 2. Streaming-Tab und Messung (Punkte 2-3). 3. Metal-PSO-Aufbau im Pool (Punkt 4) als eigener PR.
