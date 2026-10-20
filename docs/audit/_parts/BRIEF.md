# Brief für Slice-Auditoren (rein lesend)
Du auditierst EINEN Slice der Horizon Engine (C++20, CMake, Win/macOS/Linux, ImGui-Editor, entt-ECS, 6 Render-Backends). Lies `CLAUDE.md` (kurz) und danach ALLE Dateien deines Slice VOLLSTÄNDIG (Dateiliste: `docs/audit/_slices/<Modul>.txt`, Abschnitt `## slice N`). Große Dateien in Blöcken à ~1500 Zeilen lesen (Read mit offset/limit), nichts überspringen. Ändere KEINEN Quellcode; schreibe nur deine Ausgabedatei.

Prüfkategorien: A Unnötig komplizierte Logik (Verschachtelung, tote/redundante Bedingungen, Überabstraktion, Eigenbau statt Std, Makros/TMP statt constexpr, zu lange Funktionen, Flag-Parameter) · B Kontrollfluss (Branches/virtuelle Aufrufe/std::function/RTTI/Exceptions in heißen Schleifen, #ifdef bündeln) · C Speicher (Heap pro Frame/Entity/Schleife, vermeidbare Kopien, shared_ptr statt unique_ptr, fehlendes reserve, AoS/SoA, Leaks/Dangling nur wenn offensichtlich) · D Algorithmen/Datenstrukturen (O(n²), falsche Container, wiederholtes Sortieren/Parsen, String-Hashing/Logging im Hot Path, Render-State-Wechsel/Doppel-Uploads/CPU-GPU-Sync, ECS ohne Dirty-Flags) · E Nebenläufigkeit/I/O (Locks, Busy-Wait, blockierende I/O im Update/Render-Thread, Job-Overhead) · F Sonstiges (ungenutzte Includes/Parameter, auskommentierter Code, Kommentare die Code statt Absicht beschreiben, schwere Header).

Regeln: Jeder Fund mit Datei, Zeilenbereich und kurzem Code-Zitat BELEGEN (Zeilen vorher verifizieren). Keine Vermutungen; ohne Messdaten Unklares als "unsicher / messen" kennzeichnen. Vorschläge müssen das beobachtbare Verhalten exakt erhalten; sonst Risiko und absichernde Tests (siehe `tests/`) nennen. Einfachste Lösung bevorzugen, keine Mikrooptimierungen ohne messbaren Gewinn. Trenne "Vereinfachung" von "Performance". Alle Zielplattformen beachten. Qualität vor Quantität: lieber 10–25 belegte, relevante Funde als 60 Banalitäten; nach Schweregrad (Hoch zuerst) sortieren. Hot Paths (Frame-Loop, Entity-Verwaltung, Command-Submission, Physik, Asset-Streaming, Jobs, Input, Audio) priorisieren; Editor-only-Code, der nicht pro Frame läuft, hat niedrigere Schwere.

Fundformat (genau so, Markdown, pro Fund ein `### <ID> – <Titel>` Block mit diesen Feldern):
- **ID**: <Kürzel>-<Nr> (Kürzel in deiner Aufgabe vorgegeben, fortlaufend, 3-stellig)
- **Kategorie**: A–F
- **Schweregrad**: Hoch / Mittel / Niedrig
- **Typ**: Vereinfachung / Performance / Beides
- **Ort**: Datei:Zeilen
- **Problem**:
- **Code-Auszug**: (kurz, ```cpp)
- **Lösung**: (Schritte + kurze Code-Skizze)
- **Erwarteter Effekt**: (als Schätzung kennzeichnen)
- **Risiko / Verifikation**:

Datei-Aufbau: `# <Modul> Slice N – <Dateibereich>` · "Zusammenfassung" (3–6 Sätze: Zustand, Anzahl Funde je Schwere) · Funde · "Cross-Modul-Hinweise" (Beobachtungen, die andere Module betreffen, mit Pfad) · "Unsicher / Messung nötig" (Liste) · "Gelesen" (ein Satz: Dateien vollständig gelesen: ja/nein, welche nicht). Schreibe auf Deutsch. Antworte im Chat nur mit: Ausgabedatei, Anzahl Funde Hoch/Mittel/Niedrig, 3 wichtigste Funde in je einer Zeile.
