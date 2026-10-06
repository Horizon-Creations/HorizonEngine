# HTML-Altlasten im Repo: Bestandsaufnahme (Thema 156, Schritt 1)

Stand: 2026-10-06, Zweig `claude/html-dateien-aus-dem-repo-aufraeumen` auf 9b00bfb1.
Die Bestandsaufnahme beschreibt den Stand vor dem Löschen: was im Repo liegt, wer darauf verweist und was mit jeder Gruppe passieren soll. Was Schritt 2 tatsächlich entfernt hat, steht unter [„Schritt 2: entfernt“](#schritt-2-entfernt). Den Rückfallschutz und die gemessene Ersparnis beschreibt [„Schritt 3“](#schritt-3-rückfallschutz-und-gemessene-ersparnis).

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

## Schritt 2: entfernt

Der Chefchen hat Variante B entschieden. Mit `git rm -r src/HE_Rendering/glm/doc/api` ist das Verzeichnis aus dem Index und aus dem Arbeitsbaum verschwunden: 1374 Dateien, 22,59 MiB Blobgröße (1247 HTML, 98 JS, 26 PNG, 3 CSS).

- **Geblieben in `glm/doc/`:** `man.doxy`, `manual.pdf`, `manual/` und `theme/`. Sie enthalten kein HTML, und `glm/manual.md` braucht die Bilder aus `manual/`. Danach verweist im Repo nichts mehr auf `doc/api` (`git grep 'doc/api'` findet nur dieses Dokument).
- **Geblieben sind auch die übrigen 44 HTML-Dateien:** die 43 Overlay-Seiten der Script-API-Referenz und `shell_minimal.html` aus den imgui-Beispielen, beide begründet in der Tabelle oben. `git ls-files '*.html' '*.htm'` zählt jetzt 44 statt 1291 Dateien.
- **GLM-Quellcode** (`glm/glm`, `glm/test`, `glm/util`) bleibt unberührt.

### Warum die Doku zurückkommen kann: kein Vendoring-Verfahren, sondern ein versehentlicher Wiedereinzug

Ein beschriebenes Verfahren zum Vendoren von GLM gibt es im Repo nicht. Es gibt deshalb auch keine Stelle, an der man „`doc/api` nicht mitnehmen“ eintragen könnte. Die Historie zeigt aber, wie die Dateien hereingekommen sind:

1. `14d1c7d2` (Initial-Commit) hat die komplette GLM-Kopie samt `doc/api` eingecheckt.
2. `8a9a79f3` (2026-06-17, „Cleanup: remove vendored glm from HE_Rendering (MASTERPLAN 0.6)“) hat alle 2090 Dateien entfernt, laut `CopilotDocs/MASTERPLAN.md` (Forts. 28) mit `git rm -r --cached`. Die Dateien blieben also auf der Platte liegen, und ein Ignore-Eintrag kam nicht dazu.
3. `fd81c830` (2026-06-19, „Landscape: brush fixes, new sculpt tools, seed bake + 3D grid preview“) hat zusammen mit Editor-Änderungen alle 2090 Dateien unter `src/HE_Rendering/glm/` wieder eingecheckt, darunter alle 1374 aus `doc/api` (gezählt mit `git diff-tree -r --name-only --diff-filter=A`). Wahrscheinlich ist das durch `git add -A` oder `git add .` in einem Arbeitsbaum passiert, in dem die ungetrackten Dateien noch lagen.

Daraus folgt für dieses Thema:

- Schritt 2 löscht mit `git rm -r` **ohne** `--cached`. In diesem Worktree ist `doc/api` auch auf der Platte weg. Andere Checkouts (Hauptarbeitsbaum, weitere Worktrees) verlieren die Dateien, sobald sie diesen Stand auschecken oder mergen, weil Git getrackte Dateien beim Checkout mitlöscht.
- **Für Schritt 3 (Rückfallschutz):** Der wahrscheinliche Rückweg ist ein `git add -A` mit liegengebliebenen Dateien, kein Re-Vendoring. Ein Ignore-Eintrag muss außerdem zwei Ausgabeorte abdecken: `src/HE_Rendering/glm/doc/api/` (der eingecheckte Upstream-Stand) und `src/HE_Rendering/glm/doc/html/`. Denn `doc/man.doxy` setzt `OUTPUT_DIRECTORY = .` und `HTML_OUTPUT = html`, ein lokaler Doxygen-Lauf schreibt also nach `doc/html/`, nicht nach `doc/api/`.

## Schritt 3: Rückfallschutz und gemessene Ersparnis

### `.gitignore`

In `.gitignore` stehen jetzt zwei verankerte Verzeichnismuster, jeweils mit Begründung im Kommentar:

```
/src/HE_Rendering/glm/doc/api/
/src/HE_Rendering/glm/doc/html/
```

- **`doc/api/`** ist der früher eingecheckte Upstream-Stand. **`doc/html/`** ist der Ort, an den ein lokaler `doxygen man.doxy` schreibt. `man.doxy` erzeugt nur HTML (`GENERATE_HTML = YES`). LaTeX, RTF, man, XML und DocBook stehen auf `NO`, `GENERATE_TAGFILE` ist leer. Weitere Ausgabeorte gibt es also nicht.
- **Kein allgemeines `*.html`:** Ein solches Muster würde die 43 Overlay-Seiten und `shell_minimal.html` verstecken, die bewusst bleiben.
- **Geprüft** auf 9cdb009d + dieser Änderung:
  - `git check-ignore -v` trifft `doc/api/index.html` und `doc/html/index.html`.
  - Nicht getroffen werden `doc/manual.pdf`, `doc/man.doxy`, `doc/manual/*.png`, `overlay/sections/conventions.html` und `shell_minimal.html` (exit 1).
  - Den echten Rückweg aus `fd81c830` habe ich nachgestellt. Dazu lagen Dummy-Dateien in `doc/api/x.html` und `doc/html/x.html`, als Gegenprobe außerdem `doc/kontrolle.html`. `git add -A --dry-run` nahm nur die Gegenprobe auf, die beiden anderen nicht. Danach habe ich alle drei wieder gelöscht.
  - `git ls-files -ci --exclude-standard` listet keine Datei unter `glm/doc`. Die neuen Muster verstecken also nichts, was getrackt ist.
- **Grenze:** Ein Ignore-Eintrag hält `git add -A` und `git add .` auf. Gegen `git add -f` hilft er nicht.

### CI: keine Prüfung auf Repo-Größe oder Dateityp

Durchsucht habe ich `.github/workflows/{ci,claude,runtime-flavors}.yml`, `.gitattributes`, `core.hooksPath` und die Hooks des gemeinsamen `.git`. Ergebnis:

- **Keine Prüfung auf Repo-Ebene.** Es gibt keine Prüfung, die eingecheckte Dateien nach Größe oder Typ bewertet. Es gibt auch kein Git LFS, kein `pre-commit` und keinen gesetzten `core.hooksPath`. Im Hook-Ordner liegen nur die `*.sample`-Dateien.
- **Die einzige Größenprüfung ist `scripts/runtime_size.py`** (`ci.yml:270-280`, `runtime-flavors.yml:137-146`). Sie wiegt die gebauten Game- und App-Runtimes im Deploy-Verzeichnis, nicht das Repo.
- **Neu gebaut wurde keine CI-Prüfung.** Das war nicht Teil des Schritts. Eine mögliche spätere Absicherung wäre ein Job, der `git ls-files 'src/HE_Rendering/glm/doc/api/*' 'src/HE_Rendering/glm/doc/html/*'` auf leer prüft. Er würde auch `git add -f` fangen.

### Gemessene Ersparnis (`du -sk`)

Gemessen habe ich auf NN-WS03 (NTFS, 4-KB-Cluster) an je einem frisch ausgepackten `git archive` von `9cdb009d^` (vorher) und `9cdb009d` (nachher), also ohne `.git`, ohne `out/` und ohne ungetrackte Dateien. `core.autocrlf` ist auf diesem Rechner `true`, und `git archive` wendet die Zeilenende-Umwandlung an. Darum zeigt die Spalte „Inhalt“ (`du -sk --apparent-size`) für Textdateien CRLF-Größen.

| Messpunkt | vorher | nachher | Differenz |
|---|---|---|---|
| ganzer Checkout, belegt (`du -sk`) | 129 309 KiB | 102 332 KiB | **−26 977 KiB (−20,9 %)** |
| ganzer Checkout, Inhalt (`--apparent-size`) | 118 769 KiB | 95 363 KiB | −23 406 KiB |
| ganzer Checkout, Dateien | 4253 | 2879 | −1374 |
| `src/HE_Rendering/glm/doc`, belegt | 29 903 KiB | 2922 KiB | −26 981 KiB |
| `src/HE_Rendering/glm/doc`, Inhalt | 26 243 KiB | 2834 KiB | −23 410 KiB |

Die Zahlen passen zu den Angaben aus Schritt 1 und 2, sie messen nur jeweils etwas anderes:

- **22,59 MiB (23 130 KiB)** ist die Blobgröße in Git, mit LF-Zeilenenden.
- **23 410 KiB** ist dieselbe Datenmenge als Windows-Checkout mit CRLF. Die rund 280 KiB Unterschied sind eingefügte `\r`.
- **26 981 KiB** ist die Belegung auf der Platte. 1374 meist kleine Dateien runden je auf 4 KB auf. Daher kommen auch die „rund 29 MB“ im Thema-Titel (`doc/` vorher 29 903 KiB).

Die Differenz im ganzen Checkout (−26 977 KiB) und die in `glm/doc` (−26 981 KiB) stimmen bis auf 4 KiB überein: ein Cluster Rundung im Verzeichnis-Overhead. Außerhalb von `doc/api` hat sich also nichts geändert.

### Was mit `.git` passiert: nur flache und neue Checkouts profitieren

Die Historie bleibt unverändert (kein filter-repo, kein Force-Push). Darum gilt:

- **Ein bestehendes `.git` wird nicht kleiner.** Die 1374 Blobs bleiben in der Historie erreichbar, über `9cdb009d^` und jeden älteren Commit. Das Pack auf NN-WS03 bleibt bei 296 MiB (`git count-objects -vH`, `size-pack`).
- **Ein neuer voller Clone wird auch nicht kleiner.** Er holt dieselbe Historie mitsamt den rund 2,1 MiB komprimierten Blobs (siehe [„Was das Löschen wirklich spart“](#was-das-löschen-wirklich-spart)).
- **Was wirklich sinkt:** der Arbeitsbaum jedes neuen Checkouts oder Worktrees, um die oben gemessenen ~26,3 MiB Belegung und 1374 Dateien. Dazu sinkt bei flachen Clones (CI mit `fetch-depth: 1`) die Transfergröße um diese rund 2,1 MiB.
- **Bestehende Arbeitsbäume** verlieren `doc/api` erst, wenn sie einen Stand ab `9cdb009d` auschecken oder mergen. Danach greift der Ignore-Eintrag: Falls dort Dateien liegen bleiben, weil sie z. B. nie getrackt waren, checkt `git add -A` sie nicht wieder ein.

## Schritt 4: Verifikation (Build, Tests, Doku-Skripte)

Geprüft wurde Stand `1df3b075` auf NN-WS03 (Windows 11, VS 18 / MSVC 14.51, Ninja, CMake 4.4). Dieser Abschnitt ist danach die einzige Änderung, und er ändert keinen Code.

### Build: frisch, Release, grün

- **Konfiguration:** `cmake --preset x64-release -B C:/hw156/build -DDEPLOY_DIR=C:/hw156/deploy`, also ein leeres Build-Verzeichnis mit allen FetchContent-Abhängigkeiten neu. Exit 0 (91 s). Das Log nennt `GLM: Version 1.0.1`, und im Cache steht `glm_SOURCE_DIR = C:/hw156/build/_deps/glm-src`. Die vendorte Kopie unter `src/HE_Rendering/glm` kommt im Build nicht vor. Die CMake-Warnungen sind nur Deprecation-Hinweise aus `glm-src` und `mbedtls-src`. Keine Warnung nennt fehlende Dateien, `doc/` oder `.html`.
- **Build:** `cmake --build C:/hw156/build -j8`. Exit 0, 1780/1780 Schritte. Der letzte Schritt prüft die zur Laufzeit kompilierten Shader-Strings mit fxc und glslangValidator, Ergebnis „51 compiled, 0 failed“.
- **Editor-Handbuch:** `Editor/Docs/he-docs.json` liegt im Deploy und ist byte-gleich zu `EditorDeps/Docs/he-docs.json`. Im ganzen Deploy liegt keine `.html`-Datei. Gemessen ist nur der Stand nach der Löschung. Dass auch vorher kein `glm/doc` im Deploy lag, folgt aus dem fehlenden CMake-Verweis (Schritt 1), nicht aus einer Messung.
- Der gemeinsame Deploy `HorizonEngineBuild` wurde nicht berührt (`DEPLOY_DIR`-Override, `HorizonEditor.exe` dort unverändert vom 05.10.).

### Tests: `ctest` im Vordergrund, Exit 0

`ctest --test-dir C:/hw156/build -j8 --output-on-failure` mit eigenem `APPDATA` (he_tests überschreibt sonst die echte Editor-Config) und `HE_COLLAB_OFFLINE=1`:

- **Exit 0, „100% tests passed out of 237“**, 156 s. 235 Tests liefen und bestanden.
- **2 übersprungen:** `runtime_size_app_advanced` und `runtime_size_app_basic`. Das ist gewollt (`tests/CMakeLists.txt:1133`): die App-Runtimes entstehen nur über `scripts/build_runtimes.py`. Ohne sie meldet der Test Exit 2 = Skip, auch in CI. `runtime_size` (Game) lief und bestand.

### Doku-Skripte: Zweig und Merge-Base liefern dasselbe

`scripts/script_api_docs/` ist zwischen Merge-Base `9b00bfb1` und diesem Zweig unverändert. Geprüft habe ich trotzdem empirisch, als A/B: einmal auf diesem Arbeitsbaum, einmal auf einem `git archive` von `9b00bfb1`. Beide liefen gegen dieselbe Website, einen eigenen Worktree von `HC-Website` `origin/main` (`2178676`). Der Website-Hauptcheckout steht 82 Commits zurück, ihm fehlt `scripting.html`. Daran scheitert `gen_reference.py` mit `FileNotFoundError`, unabhängig von diesem Thema.

| Skript | Zweig | Merge-Base | Vergleich |
|---|---|---|---|
| `gen_reference.py --check` | Exit 1, stale: `scripting-reference.html`, `horizoncode-nodes.html`, `scripting.html` | gleich | Ausgabe byte-gleich |
| `coverage.py --out` | Exit 0, 617 Ids (582 ref, 4 named, 31 missing) | gleich | Bericht byte-gleich |
| `build_docs_bundle.py --out` | Exit 0, Bündel + 8 Abbildungen | gleich | `he-docs.json` byte-gleich |

Das „stale“ von `gen_reference.py` heißt: Die Website-Seiten sind älter als `registry.json`. Das ist auf beiden Seiten gleich, also kein Effekt dieses Themas. `gen_reference.py` und `coverage.py` lesen die 43 Overlay-Seiten und finden sie. `build_docs_bundle.py` liest nur die Website. Keins der drei liest `glm/doc`.

### Tatsächlich eingespart (aus Git gemessen, Merge-Base gegen HEAD)

| Messgröße | `9b00bfb1` | `1df3b075` | Differenz |
|---|---|---|---|
| versionierte Dateien | 4252 | 2879 | −1373 (−1374 gelöscht, +1 diese Datei) |
| davon HTML | 1291 | 44 | −1247 |
| Blobgröße (LF) | 120 023 696 B | 96 348 923 B | **−23 674 773 B (−22,58 MiB, −19,7 %)** |
| Pack für einen Depth-1-Checkout | 52 389 898 B | 50 155 482 B | **−2 234 416 B (−2,13 MiB, −4,3 %)** |

Das Pack habe ich mit `git rev-list --objects -n1 <commit> | git pack-objects --stdout` gemessen. Das entspricht grob dem, was ein flacher Clone (CI, `fetch-depth: 1`) überträgt. Doxygen-HTML komprimiert sehr gut. Darum sinkt der Arbeitsbaum um ~22,6 MiB Inhalt bzw. ~26,3 MiB Belegung (Schritt 3), der Transfer aber nur um ~2,1 MiB. Volle Clones und bestehende `.git` werden nicht kleiner, weil die Historie bleibt.

### Übersprungen

- **Debug-Build und andere Plattformen** (macOS/Metal, Linux) habe ich nicht gebaut. Die Änderung entfernt nur Dateien, die kein Build-System liest. CI baut die übrigen Plattformen beim PR.
- **`dump_engine_api.sh`** (erzeugt `registry.json` neu) habe ich nicht ausgeführt. Es liest `_deps/glm-src` und den Engine-Quellcode, kein `glm/doc`, und die Registry ist nicht Gegenstand des Themas.
- **Doxygen** (`man.doxy`) habe ich nicht laufen lassen. Kein Build-Schritt ruft es auf.

## Für ein späteres Thema notiert (nicht Teil von Thema 156)

- **Vendorte GLM:** Ganz `src/HE_Rendering/glm` (einschließlich `glm/glm` und `glm/test`) wird vom Build nicht benutzt, weil FetchContent GLM holt. Ob die Kopie insgesamt weg kann, gehört in ein eigenes Thema. Laut diesem Thema bleibt der GLM-Quellcode unangetastet.
- **imgui-Beispiele:** `src/HE_Editor/vendor/imgui/examples/` (164 Dateien, 1,54 MiB) baut die Engine nicht. `src/HE_Editor/CMakeLists.txt` nimmt nur einzelne `.cpp` aus `vendor/imgui` und `vendor/imgui/backends`, ebenso `tests/CMakeLists.txt:709-714`. Kommen die Beispiele weg, fällt `shell_minimal.html` mit ihnen weg.

## Hinweis zum Umfang

Das Thema nimmt `docs/*.md` aus. Gemeint sind die bestehenden Dokumente. Diese Datei ist neu und vom Schritt ausdrücklich verlangt.
