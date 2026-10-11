# Physik-Multithreading, Thema 178 Schritt 4: Rohdaten

Ausgaben von `scripts/perf/physics_load_run.py` (je Lauf ein frischer Prozess, 300 Warm-up- und 600 Mess-Frames,
`HE_PROFILE_COUNTERS=0`, verstecktes Fenster, Bildschirm gesperrt, Netzteil), 11.10.2026 19:52 bis 20:06.
`<szene>_old_r1.txt` ist das Vorher-Binary (Schritt 1, Quelle `a4fa3a9d`), `<szene>_new_r1.txt` der Kopf `af4f2868`.
`*.physics-log.txt` fasst das Spiel-Log zusammen (Initialisierung, Zahl der Warnungen "PhysicsWorld::step took > 8 ms",
Zahl der Warnungen "Jolt dropped contacts"). Auswertung und Bedingungen: `docs/physics-threading-baseline.md`, Abschnitt 10.
