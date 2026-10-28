# Thema 174 Schritt 16: Handbuch-Eintrag + Gesamtverifikation — Befund

Stand 2026-10-12, NN-WS03, HEAD 95e7873a (Doku-Commit auf ee263101, Schritt 14 + CI-Fix).

## 1. Handbuch-Eintrag

Das in-editor Handbuch (`EditorDeps/Docs/he-docs.json`, erzeugt aus `Website/HorizonEngineDocs/
editor.html`, siehe `docs/in-engine-docs-design.md`) kannte Spline-Werkzeug, Wasser-Pinsel und
Lake-Werkzeug bisher gar nicht — nur die Engine-Water-**Material**-Seite existierte. Neuer
Abschnitt „Water & Lakes" direkt nach „Landscape Mode" (Pinsel, Lake-aus-Spline, Ufer-Böschung
inkl. der in Schritt 13 korrigierten sanften Abflachung, Lake-Preset aus Schritt 14), dazu der
Spline-Modus in der Viewport-Moduswahl ergänzt. Details und die Begründung für den gespleissten
statt vollständig neu generierten Bundle-Commit stehen in Commit 95e7873a.

Die Website-Quelle liegt in einem separaten Checkout (`HC-Website`, Branch
`docs/thema174-water-lake-handbuch`, Commit 1a869fa) — **lokal committet, nicht gepusht**: der
Checkout war unabhängig vom letzten Bundle-Regen weitergelaufen (project-hub/asset-editors/
preferences/shortcuts gekürzt, ein `github`-Abschnitt entfernt), eine Vollregeneration hätte
diese fremde Drift mitgezogen. Jemand mit Überblick über den aktuellen Stand von HC-Website
sollte vor einem Push prüfen, ob diese Drift beabsichtigt ist.

## 2. Vollbau

`cmake --build . -j16` auf dem bestehenden Ordner `C:/hebuild/pws9` (Release, Ninja/MSVC, von
Schritt 9/10) schlug zunächst mit `LNK2019` auf `__std_rotate`/`__std_max_element_d_`/
`__std_min_element_8u`/`__std_replace_copy_2` fehl — ein ABI-Mismatch zwischen alten .obj-Dateien
(älterer MSVC-Stand) und frisch kompilierten, nichts mit dieser Änderung oder dem Wasser-Code zu
tun (siehe `hive_lesson` #238). `--clean-first` behebt es: **1902/1903 Ziele, 0 Fehler.**

## 3. he_tests

Scratch `HE_CONFIG_DIR`, `HE_NET_LOOPBACK_ONLY=1`.

- Gefiltert (`*water*,*spline*,*terrain*,*lake*,*brush*`): **278/278 grün, 449850/449850
  Assertions.**
- Voller Lauf: **4980/4984 grün, 1287075/1287085 Assertions.** Die vier roten Fälle sind
  bekanntes, unabhängiges Rauschen, keiner davon berührt Wasser/Spline/Terrain-Code:
  - `test_inspector_ui.cpp` (zwei Fälle, Clipboard) und `test_shortcuts_ui.cpp` (zwei Fälle,
    `persisted()`) — bereits dokumentiertes `HE_CONFIG_DIR`-abhängiges Verhalten (siehe
    `vulkan-d3d-baseline-noise.md`, Handoff von Schritt 8).
  - `test_hpak.cpp` „scanContentDirectory resolves UUIDs from loose content" (15 statt 2
    Einträge) — reproduziert auch isoliert (`-tc=` nur dieser eine Fall); der Fixture-Pfad
    `%TEMP%\he_registry` ist nicht eindeutig benannt und kollidiert mit anderen Läufen auf
    demselben Gerät (siehe `hive_lesson` #239, neu dokumentiert).

## 4. Backend-Matrix (Regression-Check für Schritt 13/14 auf aktuellem HEAD)

Schritt 9 und 10 deckten D3D11/D3D12/Vulkan/OpenGL bereits strukturell und mit echtem Rendern ab
(Commits 8d1ca365/d625211b) — aber **vor** den Böschungs-Fixes aus Schritt 13 und dem
Lake-als-Actor aus Schritt 14. Da die Aushub-/Sync-Logik ausschließlich CPU-seitige, Backend-
unabhängige Geometrie erzeugt (Schritt 9: kein wasserspezifischer Renderer-Code), ist das
Regressionsrisiko backend-spezifisch gering; trotzdem erneut gemessen statt nur angenommen:
`HE_DUMP_LAKEEDIT=slope` (die Szene, die Schritt 13s Fix direkt zeigt — Böschung statt Klippe)
auf allen vier verfügbaren Hardware-Backends, mit `HE_DUMP_SKYTEST=1` für Himmel/Kamera.

| Backend | Log-Zeile (identisch) | [ERROR]-Zeilen | Mittel-RGB |
|---|---|---|---|
| D3D11  | lake made, level -4.41, 2564 cells, 4157 ground vertices dug, worst step 1.738 m | 0 | 157, 169, 180 |
| D3D12  | ident. | 0 | 157, 169, 180 |
| Vulkan | ident. | 0 | 156, 167, 177 |
| OpenGL | ident. | 0 | 165, 171, 178 |

D3D11/D3D12/Vulkan liegen je Kanal innerhalb von ~2 Einheiten; OpenGL weicht wie in Schritt 10
dokumentiert etwas mehr ab (vorbekannte Cross-Backend-Varianz, kein neuer Befund). Die
Log-Zahlen (Zellen, gegrabene Vertices, worst step) sind über alle vier Backends exakt
identisch — die Geometrie ist, wie architektonisch erwartet, backend-unabhängig.

**Metal**: auf NN-WS03 nicht verfügbar (kein Mac). Unverändert gegenüber Schritt 10: der Mensch
hatte dort bereits zwei Nahaufnahmen vom Wassermaterial mit Spline-Overlay geliefert (gleiche
Transparenz-/Reflexions-/Trübungscharakteristik), kein strikter Pixel-Vergleich.

## 5. CI

Commit ee263101 (Schritt 14 + CI-Fix): grün,
<https://github.com/Horizon-Creations/HorizonEngine/actions/runs/38245660619>. Commit 95e7873a
(dieser Schritt, Doku) noch nicht auf GitHub geprüft — lokal mit `he_tests -tc=*docs*,*help*`
(71/71 grün) vorab abgesichert.
