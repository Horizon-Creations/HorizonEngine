# HTML-Altlasten im Repo: Bestandsaufnahme (Thema 156, Schritt 1)

Stand: 2026-10-06, Zweig `claude/html-dateien-aus-dem-repo-aufraeumen` auf 9b00bfb1.
Bisher ist nichts gelöscht. Hier steht nur, was im Repo liegt, wer darauf verweist und was mit jeder Gruppe passieren soll.

## Ergebnis

`git ls-files '*.html' '*.htm'` findet 1291 Dateien mit zusammen 22,13 MiB (Blobgröße im Arbeitsbaum).

| Gruppe | Dateien | Größe (Blobs in HEAD) | Verweise | Entscheidung |
|---|---|---|---|---|
| `src/HE_Rendering/glm/doc/api/` (Doxygen-Ausgabe) | 1247 HTML + 98 JS + 26 PNG + 3 CSS = 1374 | 22,59 MiB (davon HTML 21,75 MiB); im Pack ca. 2,1 MiB | keine (siehe unten) | **löschen**: das ganze Verzeichnis `doc/api/`, nicht nur `*.html` (Empfehlung, siehe „Offene Entscheidung“) |
| `scripts/script_api_docs/overlay/` (`groups/*.html`, `sections/conventions.html`) | 43 HTML (in `overlay/` liegen außerdem 3 JSON) | 0,12 MiB HTML | `gen_reference.py:49,530,710`, `coverage.py:123-126`, `docs/script-api-docs-gap-audit-2026-09-24.md:242` | **behalten**: handgeschriebene Quelle der Script-API-Referenz |
| `src/HE_Editor/vendor/imgui/examples/libs/emscripten/shell_minimal.html` | 1 | 2,4 KB | nur innerhalb der imgui-Beispiele: 7× `Makefile.emscripten`, 3× `example_*_wgpu/CMakeLists.txt` | **behalten**: kein messbarer Gewinn, und das Löschen würde die vendorte Kopie vom Upstream-Stand entfernen |

Durchsucht wurden mit `git grep` diese Pfade: `CMakeLists.txt`, `cmake/`, `scripts/`, `.github/workflows/`, `tests/` und `EditorDeps/` einschließlich `EditorDeps/Docs/he-docs.json`. Gesucht wurde nach `glm/doc`, `glm\doc`, `manual.pdf`, `man.doxy`, `doxygen`, `script_api_docs/overlay`, `overlay/groups|sections`, `shell_minimal` und `emscripten`. Die einzigen Treffer stehen in der Tabelle. Der einzige Treffer in `tests/` ist `__EMSCRIPTEN__` in `tests/vendor/doctest.h` und gehört nicht hierher.

## Gruppe 1: GLM-Doxygen-Doku

- **Die vendorte GLM baut nicht mit.** `CMakeLists.txt:72-79` holt GLM 1.0.1 per `FetchContent` von GitHub. Beide lokalen Build-Caches (`out/build/x64-debug`, `out/build/x64-release`) führen `glm_SOURCE_DIR = …/_deps/glm-src`. Kein CMake im Repo ruft `add_subdirectory` auf `src/HE_Rendering/glm` auf. Alle Targets binden `glm::glm` aus FetchContent. `scripts/script_api_docs/dump_engine_api.sh:27` und `scripts/build_runtimes.py:92` nutzen ebenfalls `_deps/glm-src`.
- **Auch die eigene CMake-Konfiguration von GLM bindet `doc` nicht ein.** `src/HE_Rendering/glm/CMakeLists.txt` kennt nur `add_subdirectory(glm)`, `add_subdirectory(test)` (wenn `GLM_BUILD_TESTS` gesetzt ist) und `install(DIRECTORY glm …)`. `doc/api/` ist die Ausgabe von Doxygen über `doc/man.doxy` (`INPUT = ../glm`, `OUTPUT_DIRECTORY = .`). Upstream hat sie eingecheckt, kein Target erzeugt oder liest sie.
- **Was wo hängt:** `glm/manual.md` bindet Bilder aus `./doc/manual/*.png|jpg` ein. Wer nur `doc/api/` löscht, lässt das vendorte Handbuch heil. `doc/manual/` (1,21 MiB), `doc/manual.pdf` (1,40 MiB), `doc/theme/` (0,04 MiB) und `doc/man.doxy` (0,11 MiB) enthalten keine HTML-Dateien. Sie bleiben in diesem Thema unangetastet.

### Offene Entscheidung für Schritt 2

| Variante | Umfang | Arbeitsbaum kleiner um | Folge |
|---|---|---|---|
| A: nur `*.html` | 1247 Dateien | 21,75 MiB | `doc/api/` behält 127 tote JS-, CSS- und PNG-Dateien (0,84 MiB, darunter `jquery.js` und der Suchindex). Sie sind ohne die HTML-Seiten nutzlos. |
| **B: `doc/api/` komplett (Empfehlung)** | 1374 Dateien | 22,59 MiB | Die Doxygen-Ausgabe verschwindet vollständig. `manual.md` und `doc/manual/` bleiben unberührt. |
| C: `doc/` komplett | 1426 Dateien | 25,35 MiB | Die Bilder in `glm/manual.md` brechen. Dazu gehen `manual.pdf` und `man.doxy` verloren. Das geht über ein HTML-Thema hinaus. |

## Was das Löschen wirklich spart

- **Der Thema-Titel nennt rund 29 MB.** Das ist die Belegung auf der Platte (`du -sh doc` zeigt 30M, `doc/api` allein 27M, bei 4-KB-Clustern und vielen kleinen Dateien). Die Inhaltsgröße von `doc/api/` beträgt 22,59 MiB.
- **Arbeitsbaum und Checkout:** Jeder Checkout und jeder neue Worktree wird um rund 22,6 MiB und 1374 Dateien kleiner. Bei vielen parallelen Worktrees auf NN-WS03 ist das der eigentliche Gewinn.
- **Voller Clone: praktisch kein Gewinn.** Die Historie wird nicht umgeschrieben (kein filter-repo, kein Force-Push), die Blobs bleiben also im Pack. Komprimiert machen sie ohnehin nur etwa 2,1 MiB aus (`objectsize:disk`), das gesamte Pack ist 296 MiB groß.
- **Flacher Clone:** Hier sinkt die Transfergröße um diese etwa 2,1 MiB. Das betrifft die CI: `actions/checkout@v4` steht in `ci.yml` und `runtime-flavors.yml` ohne `fetch-depth`, also mit der Vorgabe 1. In `claude.yml` ist `fetch-depth: 1` ausdrücklich gesetzt.

## Für ein späteres Thema notiert (nicht Teil von Thema 156)

- **Vendorte GLM:** Ganz `src/HE_Rendering/glm` (einschließlich `glm/glm` und `glm/test`) wird vom Build nicht benutzt, weil FetchContent GLM holt. Ob die Kopie insgesamt weg kann, gehört in ein eigenes Thema. Laut diesem Thema bleibt der GLM-Quellcode unangetastet.
- **imgui-Beispiele:** `src/HE_Editor/vendor/imgui/examples/` (164 Dateien, 1,54 MiB) baut die Engine nicht. `src/HE_Editor/CMakeLists.txt` nimmt nur einzelne `.cpp` aus `vendor/imgui` und `vendor/imgui/backends`, ebenso `tests/CMakeLists.txt:709-714`. Kommen die Beispiele weg, fällt `shell_minimal.html` mit ihnen weg.

## Hinweis zum Umfang

Das Thema nimmt `docs/*.md` aus. Gemeint sind die bestehenden Dokumente. Diese Datei ist neu und vom Schritt ausdrücklich verlangt.
