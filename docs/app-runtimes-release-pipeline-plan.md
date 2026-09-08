# App-Runtimes in die Release-Pipeline einhaengen

Stand 08.09.2026, Zweig `claude/app-runtimes-release-pipeline`, Schritt 1 (Orientierung und
Entwurf, kein Feature-Code).

`scripts/build_runtimes.py` baut die drei Runtime-Auspraegungen, aber niemand ruft es im
normalen Weg auf. In `out/deploy/` landen `Editor` und `Game`, sonst nichts, und der
Exporter faellt beim App-Export deshalb auf das volle Game-Runtime zurueck. Dieses Dokument
haelt fest, wie der Build heute wirklich laeuft, wo die zwei fehlenden Bundles kuenftig
entstehen sollen und was dafuer im Einzelnen zu tun ist.

---

## 0. Der Blocker, der vor allem anderen kommt

**Die beiden App-Auspraegungen bauen auf main derzeit auf keiner der drei Plattformen.**

`.github/workflows/runtime-flavors.yml` ist seit dem 07.09.2026 rot, Lauf `34105706124`,
macOS, Windows und Linux gleichermassen. Die Ursache ist auf allen dreien dieselbe:

```
undefined reference to `he::shaderc::compileHlslPinned(std::string const&,
                        he::shaderc::Stage, std::vector<he::shaderc::HlslPin> const&)'
  referenced from HE::MaterialShaderLibrary::decalFragmentForward(...)
```

`src/HE_Tools/ShaderCompiler/ShaderCompiler.h:97` deklariert `compileHlslPinned`, die echte
Umsetzung steht in `ShaderCompiler.cpp:213`, und `MaterialShaderLibrary.cpp` ruft sie an
sechs Stellen auf (SSR und Decals). Der Ersatzbaustein
`src/HE_Tools/ShaderCompiler/ShaderCompilerStub.cpp`, den ein Baum mit
`HE_ENABLE_SHADERC=OFF` unter demselben Zielnamen linkt, kennt `compile`,
`compileMslPinned` und `compileMany`, aber `compileHlslPinned` nicht. Die Decal- und
SSR-Arbeit hat die Funktion nur in die eine Haelfte eingetragen.

Das ist genau die Falle, vor der der Kommentar im Stub warnt: der Materialpfad bleibt
absichtlich im Binary, also muss jede Funktion des Headers dort eine Antwort haben. Fehlt
eine, faellt es nicht beim Compilieren auf, sondern erst beim Linken einer Auspraegung,
die niemand taeglich baut.

**Aufwand: eine Funktion**, dieselben vier Zeilen wie `compileMslPinned` daneben, plus ein
Satz im Stub-Kommentar darueber, dass der Header und diese Datei zusammen vollstaendig sein
muessen. Es gehoert vor jeden anderen Schritt dieses Themas, weil sonst jede
Pipeline-Verdrahtung eine kaputte Sache verdrahtet.

**Zweiter, fremder Punkt:** `ci.yml` ist auf main ebenfalls rot, aber aus anderem Grund und
nur auf Windows (`test_mcp_bridge`, `tests/test_mcp_bridge.cpp:599`,
`CHECK_FALSE(fs::exists(file))`, Laeufe `34199839606`, `34173208071`, `34164589071`). Das
ist nicht Sache dieses Themas, aber Schritt 3 kann seine Aenderung an `ci.yml` nur gegen ein
gruenes main verifizieren. Als Abhaengigkeit vormerken, nicht mitfixen.

---

## 1. Wie `out/deploy/` heute entsteht

Kein Skript und kein CI-Schritt, sondern **CMake POST_BUILD**. Wer den Editor baut, baut und
verteilt das Game-Runtime mit.

| Was | Wo | Wodurch |
|---|---|---|
| `DEPLOY_DIR` | `out/deploy` (Windows: ein fester Entwicklerpfad) | `CMakeLists.txt:726`, Cache-Variable, CI ueberschreibt sie mit `-DDEPLOY_DIR=$workspace/package` |
| `HE_RUNTIME_DIR_NAME` | `Game` / `AppAdvanced` / `AppBasic` | `CMakeLists.txt:730-736`, aus `HE_RUNTIME_FLAVOR` |
| `<deploy>/Game` | Exe, Engine-Bibliotheken, SDL3, Python, CRT, Shaders | `src/HE_Game/CMakeLists.txt:59-151`, POST_BUILD von `HorizonGame` |
| `<deploy>/Editor` | Editor plus alles, was er braucht | POST_BUILD von `HorizonEditor` und `he_deploy_dll` |
| `<deploy>/Editor/Game` | woertliche Kopie von `<deploy>/Game` | `src/HE_Editor/CMakeLists.txt:354`, POST_BUILD |
| Reihenfolge | Editor haengt an Game | `CMakeLists.txt`, `add_dependencies(HorizonEditor HorizonGame)` |

Das Game-Runtime liegt also **zweimal** vor: einmal als `out/deploy/Game` fuer den Editor aus
dem Build-Baum, einmal als `out/deploy/Editor/Game` fuer den ausgelieferten Editor. Das ist
kein Versehen, sondern die Antwort auf die zwei Orte, an denen `findRuntimeBundle` sucht,
und die App-Auspraegungen werden es genauso brauchen.

**Auslieferung** in `.github/workflows/ci.yml`, `push` auf main und jeder PR gegen main:

* Windows: `Compress-Archive package/Editor/*` nach `HorizonEditor-windows-x64.zip`
* Linux: `tar czf ... -C package Editor` nach `HorizonEditor-linux-x64.tar.gz`
* macOS: `cmake --build build --target dmg`, das ist `scripts/package_macos.sh`. Es liest
  `out/deploy/Editor`, baut daraus `HorizonEditor.app` und kopiert dabei
  `out/deploy/Editor/Game` nach `Contents/Resources/Game` (Zeilen 197-205), weil
  `SDL_GetBasePath()` in einem Bundle auf `Resources` zeigt.

Von dort holt der Mensch die Artefakte und veroeffentlicht sie auf der Website. Ein
Release-Workflow existiert nicht.

**Der zweite Workflow**, `.github/workflows/runtime-flavors.yml`, baut alle drei
Auspraegungen auf allen drei Plattformen mit dem Rezept von `ci.yml`
(`HE_PORTABLE_BUILD=ON`, `HE_PREFER_MBEDTLS=ON`), meldet die Groessentabelle und prueft gegen
`scripts/runtime_size.py`. Er laeuft auf `push` gegen main und `claude/he-apps-ui-framework-**`
mit einem Pfadfilter, sowie auf `workflow_dispatch`. **Er wiegt nur.** Kein
`upload-artifact`, kein Bundle verlaesst den Runner. Die drei Auspraegungen sind auf jeder
Plattform schon einmal gebaut worden, sie sind bloss nie irgendwo angekommen.

---

## 2. Was der Exporter erwartet

`src/HE_Core/src/Hpak/ProjectExporter.cpp:94-140`, `findRuntimeBundle`:

* Gesucht wird auspraegungsweise, nicht verzeichnisweise: erst die gewuenschte, dann `Game`.
* Pro Auspraegung laeuft ein Aufstieg von `SDL_GetBasePath()` aus, hoechstens sieben Ebenen,
  und prueft auf jeder Ebene `<dir>/<Name>` **und** `<dir>/out/deploy/<Name>`.
* `Host` liegt flach (`Game`, `AppAdvanced`, `AppBasic`), Fremdplattformen unter
  `GameRuntimes/<Plattform>/<Name>`.
* Ein Verzeichnis zaehlt nur, wenn `HorizonGame[.exe]` darin liegt und keine `project.hcfg`.

Daraus folgt fuer die zwei neuen Bundles zwingend:

1. Fuer den Editor **aus dem Build-Baum** muessen sie `out/deploy/AppAdvanced` und
   `out/deploy/AppBasic` heissen, denn der Aufstieg prueft `<dir>/out/deploy/<Name>`, nicht
   `<dir>/out/deploy/Editor/<Name>`. Das ist die Vorgabe von `build_runtimes.py` ohnehin.
2. Fuer den **ausgelieferten** Editor muessen sie neben dem Editorbinary liegen, also
   `package/Editor/AppAdvanced` (Windows, Linux) und `Contents/Resources/AppAdvanced`
   (macOS). Genau wie `Game`, genau aus demselben Grund.

`tests/CMakeLists.txt:534-537` haelt schon drei `ctest`-Zeilen bereit, die
`${DEPLOY_DIR}/Game`, `${DEPLOY_DIR}/AppAdvanced` und `${DEPLOY_DIR}/AppBasic` wiegen; die
beiden App-Zeilen ueberspringen sich heute, weil dort nichts liegt.

Geprueft und leer: ausser `DEPLOY_GAME` und `DEPLOY_EDITOR` schreibt nichts in
`${DEPLOY_DIR}`. Ein Auspraegungsbaum kann also gefahrlos in denselben Deploy-Wurzel
zeigen, sein `DEPLOY_EDITOR` ist ohnehin nach `${CMAKE_BINARY_DIR}/deploy-editor-unused`
umgeleitet (`CMakeLists.txt:745`).

---

## 3. Die Entscheidung: wo die Bundles entstehen

**Zentral in CI, in `ci.yml`, und nur bei `push` auf main.** Die zwei App-Runtimes werden
nach Test und vor dem Verpacken gebaut und wandern in dasselbe Editorpaket, das heute schon
das Game-Runtime traegt.

Das ist die vom Chefchen empfohlene Richtung, und die beiden Alternativen scheiden aus
Gruenden aus, die man nachlesen und nicht neu abwaegen muss:

**Lokal beim Editor-Build mitbauen: nein.** Eine Auspraegung ist ein eigener Build-Baum, das
ist die Kernaussage von A3b und aendert sich nicht. Wer den Editor baut, baute damit drei
`HorizonGame`-Baeume statt einem. Auf dem Runner sind das gemessene fuenf bis neun Minuten
pro Auspraegung, auf einem Entwicklerrechner mit kaltem `_deps` deutlich mehr. Fuer eine
Sache, die genau beim Export einer App gebraucht wird, ist das die falsche Rechnung.

**In-Editor auf Zuruf: nein, und zwar unmoeglich.** Der als Vorbild genannte
`GameLogicBuildPanel` existiert im Baum nicht; es gibt `BuildProgressDialog.cpp`, einen
Fortschrittsdialog ohne eigenen Build. Und selbst mit Panel: ein heruntergeladener Editor
bringt ein gebuendeltes `cmake` und ein gestagtes SDK fuer die HorizonCode-nach-C++-Codegen
mit, aber nicht den Engine-Quelltext und nicht die FetchContent-Abhaengigkeiten. Ein
Runtime-Flavour laesst sich daraus nicht bauen. Der Weg endet, bevor er anfaengt.

**Warum in `ci.yml` und nicht in `runtime-flavors.yml`:** die Bundles muessen in *dasselbe*
Editorpaket, das `ci.yml` schnuert. Artefakte zwischen zwei Workflows durchzureichen hiesse,
den Editor-Build und die Auspraegungs-Builds ueber `workflow_run` zu koppeln und das Paket
danach ein zweites Mal zu schnueren. Zwei Wege zum selben Zip, von denen einer selten
laeuft, ist genau die Sorte Pipeline, die stillschweigend auseinanderlaeuft.

**Warum nur bei `push` auf main:** `ci.yml` laeuft auch bei jedem PR, und dort ist niemandem
mit einem vollstaendigen Editorpaket gedient. Ein `if: github.event_name == 'push'` an den
neuen Schritten haelt die PR-Laufzeit, wie sie ist, und liefert das vollstaendige Paket
genau dort, wo der Mensch es abholt.

---

## 4. Was `runtime-flavors.yml` danach ist

Er bleibt, aber ohne main. Sobald `ci.yml` die drei Auspraegungen auf main baut, ist der
Zweig `main` in seinem `on: push` doppelt gemoppelt. Was er dann noch kann und `ci.yml`
nicht: eine **Feature-Branch** hinter einem Pfadfilter wiegen, ohne dass ein gewoehnlicher
Commit eine Stunde Runnerzeit kostet. Genau das war seine erklaerte Absicht.

Also: `main` aus der Zweigliste streichen, `claude/**` statt des einen alten Zweigmusters,
`workflow_dispatch` bleibt. Der Groessenwaechter fuer main zieht nach `ci.yml` um.

---

## 5. Umsetzung in Schritten

### P0. Den Stub vervollstaendigen (Vorbedingung, siehe Abschnitt 0)

`ShaderCompilerStub.cpp` um `compileHlslPinned` ergaenzen. Lokal pruefen mit

```
scripts/build_runtimes.py --flavor app-advanced --flavor app-basic --build-type Release
```

Das ist der einzige Schritt, den man auf dem Mac vollstaendig selbst verifizieren kann, und
er sollte auch dort verifiziert werden, bevor er nach CI geht.

### P1. `build_runtimes.py` findet die Quellen des Editorbaums

`existing_sources()` (Zeilen ~100-125) sucht in `cmake-build-release`, `cmake-build-debug`
und in `--build-root`. Der Editorbaum von `ci.yml` heisst `build`, und der steht in keiner
der drei Listen. In `runtime-flavors.yml` faellt das nicht auf, weil dort die erste
Auspraegung des Laufs den beiden anderen als Quelle dient. In `ci.yml` wuerden **beide**
App-Auspraegungen SDL, Jolt, Recast und Lua kalt neu klonen.

Zwei Wege, der zweite ist der bessere:

* `"build"` an die Liste der durchsuchten Baeume anhaengen. Billig, aber ein Name, den das
  Skript raet.
* Ein `--source-tree DIR` (wiederholbar), das vorne in die Liste kommt. `ci.yml` uebergibt
  `--source-tree "$GITHUB_WORKSPACE/build"`, `runtime-flavors.yml` bleibt unveraendert.

Der Cache-Schluessel von `ci.yml` (`path: build/_deps`) deckt die geteilten Quellen damit
mit ab; `out/runtime-builds/*/_deps` braucht keinen eigenen Cache-Eintrag mehr, wenn die
Objekte ohnehin pro Auspraegung neu entstehen.

### P2. Die zwei Auspraegungen in `ci.yml` bauen

Neuer Schritt **nach** `Run tests`, **vor** den drei Verpackungsschritten, mit
`if: runner.os == '...'`-freiem gemeinsamem `shell: bash`:

```yaml
- name: Build the two app runtimes
  if: github.event_name == 'push'
  shell: bash
  run: |
    PY=$(command -v python3 || command -v python)
    "$PY" scripts/build_runtimes.py \
      --flavor app-advanced --flavor app-basic \
      --build-type Release --jobs 4 \
      --deploy-dir "<derselbe Deploy-Wurzel wie der Configure-Schritt>" \
      --source-tree "${{ github.workspace }}/build" \
      --define HE_PORTABLE_BUILD=ON --define HE_PREFER_MBEDTLS=ON
```

Die Deploy-Wurzel ist **nicht** auf allen drei Plattformen dieselbe: Windows und Linux
konfigurieren mit `-DDEPLOY_DIR=$workspace/package`, macOS nicht und landet damit auf
`out/deploy`. Entweder man gibt sie je Plattform mit, oder man setzt `DEPLOY_DIR` auch auf
macOS explizit. Das Zweite ist sauberer, aber es beruehrt `package_macos.sh`, das
`$SOURCE_DIR/out/deploy/Editor` fest verdrahtet hat. **Empfehlung: erst einmal je Plattform
mitgeben**, den Pfad in einer Ausgabevariablen des Configure-Schritts halten, und
`package_macos.sh` nicht anfassen ausser fuer die Kopie in P3.

**Nach den Tests und nicht davor.** Damit blieben die zwei `runtime_size_app_*`-`ctest`-Zeilen
uebersprungen, was `docs/he-apps-plan.md` als offenen Punkt notiert. Sie echt zu machen
hiesse, den Auspraegungsbau vor `ctest` zu ziehen, und dann haelt ein Linkfehler in einer
Auspraegung die gesamte Testsuite an. Das ist der falsche Tausch: die Testsuite ist die
Aussage darueber, ob der Code gut ist. Die Groessenschwelle wird stattdessen als eigener
Schritt direkt geprueft, genau wie `runtime-flavors.yml` es tut, mit derselben Behandlung
von Rueckgabecode 2 (keine gemessene Schwelle ist gruen, nicht rot).

### P3. Die Bundles ins Editorpaket kopieren

Dreimal dieselbe Kopie, an drei Stellen, in der Form, die `Game` schon hat.

* **Windows und Linux**, neuer `bash`-Schritt vor dem Verpacken:
  `package/AppAdvanced` und `package/AppBasic` nach `package/Editor/` verschieben, danach
  `HorizonEngine.log` und `imgui.ini` daraus entfernen, so wie `package_macos.sh` es fuer
  `Game` tut. Das vorhandene `Compress-Archive` beziehungsweise `tar` nimmt sie dann von
  selbst mit, es packt `package/Editor` als Ganzes.
* **macOS**, in `scripts/package_macos.sh` neben dem `Game`-Block (Zeilen 194-206): eine
  Schleife ueber `Game AppAdvanced AppBasic`, Quelle `$SOURCE_DIR/out/deploy/<Name>` fuer
  die App-Auspraegungen und `$DEPLOY_DIR/Game` fuer das Game (dort liegt es wegen der
  POST_BUILD-Kopie), Ziel `Resources/<Name>`. Eine fehlende App-Auspraegung ist eine
  **Warnung, kein Abbruch**: ein Entwickler, der lokal `cmake --build build --target dmg`
  aufruft, hat sie nicht gebaut und soll trotzdem ein `.app` bekommen. Fuer `Game` bleibt es
  bei der bestehenden Warnung.
* **Signieren:** die Kopie muss vor dem Ad-hoc-Signieren des Bundles passieren, aus
  demselben Grund wie bei `Game`. Die Reihenfolge in `package_macos.sh` also beibehalten.

### P4. Verifikation

Was von hier aus (macOS, kein Windows, kein Linux) wirklich pruefbar ist:

1. `build_runtimes.py` fuer beide App-Auspraegungen lokal durchbauen: das prueft P0 und P1.
2. `scripts/runtime_size.py out/deploy/AppAdvanced --flavor=app-advanced` gegen die
   lokal gemessenen Schwellen: 85 / 26 MB und 84 / 25 MB.
3. `cmake --build build --target dmg` und danach nachsehen, dass
   `HorizonEditor.app/Contents/Resources/` alle drei Runtimes enthaelt.
4. Einen App-Export aus dem so gebauten Editor fahren und im Export-Log die Zeile
   `Runtime: AppAdvanced` beziehungsweise `AppBasic` lesen. **Das ist der eigentliche
   Beweis** und der einzige, der die Frage des Themas beantwortet: der Fallback-Satz
   verschwindet.

Windows und Linux koennen nur ueber einen CI-Lauf verifiziert werden. `workflow_dispatch` an
`runtime-flavors.yml` deckt P0 ab, ohne dass etwas nach main muss.

---

## 6. Was es kostet

Gemessene Laufzeiten, Wanduhr je Job:

| | `ci.yml` heute (Lauf 34147355928) | `runtime-flavors.yml`, drei Auspraegungen (Lauf 33989763804) |
|---|---|---|
| macOS | 16 min 24 s | 15 min 12 s |
| Linux | 10 min 59 s | 16 min 21 s |
| Windows | 27 min 00 s | 26 min 43 s |

Daraus grob fuenf Minuten je Auspraegung auf macOS und Linux, rund neun auf Windows. Zwei
zusaetzliche Auspraegungen in `ci.yml` also **plus etwa 10 Minuten auf macOS und Linux, plus
etwa 18 auf Windows**, und Windows liefe damit auf etwa 45 Minuten. Nur bei `push` auf main,
PRs bleiben wie sie sind.

Diese Schaetzung setzt voraus, dass P1 wirklich greift und die Quellen aus `build/_deps`
wiederverwendet werden. Sie ist **nicht gemessen** und beim ersten Lauf nachzutragen.

Paketgroesse, aus den CI-Messungen vom 05.09.2026:

| | `app-advanced` | `app-basic` | Zuwachs je Editordownload |
|---|---|---|---|
| macOS | 48,8 MB | 48,0 MB | rund 97 MB |
| Windows | 32,0 MB | 31,4 MB | rund 63 MB |
| Linux | 45,8 MB | 45,0 MB | rund 91 MB |

Das ist unkomprimiert; Zip und Tar.gz druecken das deutlich. Der groesste Einzelposten ist
in allen drei Runtimes dieselbe Python-Standardbibliothek, rund 31 MB auf macOS, 22,7 auf
Linux, 9,8 auf Windows, **byteweise dreimal dasselbe**. Sie zwischen den drei Runtimes zu
teilen waere die naheliegende spaetere Ersparnis und ist hier ausdruecklich **nicht** Teil
des Vorhabens: der Exporter kopiert ein Runtime-Verzeichnis woertlich, und ein geteilter
Python-Ordner waere ein anderer Vertrag als der.

---

## 7. Der lokale Entwicklerfall

Bleibt, wie er ist, und das ist eine Entscheidung und kein Rest. Wer den Editor lokal baut,
bekommt `Game` und exportiert eine App mit dem dokumentierten Fallback plus Log-Warnung. Das
ist der Zustand, den `findRuntimeBundle` ausdruecklich vorsieht.

Eine Bequemlichkeit dazu, mehr nicht: ein CMake-Ziel `app-runtimes`, nicht in `ALL`, das
`scripts/build_runtimes.py` mit dem `DEPLOY_DIR` des Baums aufruft. Drei Zeilen
`add_custom_target`. Es baut nichts von selbst und kostet niemanden etwas, der es nicht
aufruft, und es erspart das Nachschlagen der richtigen Argumente. Nur mitnehmen, wenn P0 bis
P3 stehen.

---

## 8. Zusammengefasst

| Nr. | Was | Beruehrt |
|---|---|---|
| P0 | `compileHlslPinned` im Stub ergaenzen | `src/HE_Tools/ShaderCompiler/ShaderCompilerStub.cpp` |
| P1 | `--source-tree` in `build_runtimes.py` | `scripts/build_runtimes.py` |
| P2 | zwei App-Auspraegungen bauen, `push`-only, nach `ctest`, plus Groessenpruefung | `.github/workflows/ci.yml` |
| P3 | Bundles ins Editorpaket kopieren | `.github/workflows/ci.yml`, `scripts/package_macos.sh` |
| P4 | lokal verifizieren, ein CI-Lauf fuer Windows und Linux | -- |
| P5 | `runtime-flavors.yml` auf Feature-Zweige verengen | `.github/workflows/runtime-flavors.yml` |
| P6 | optional `add_custom_target(app-runtimes)` | `CMakeLists.txt` |

Nicht Teil dieses Vorhabens: die geteilte Python-Standardbibliothek, die
`GameRuntimes/<Plattform>`-Bundles fuer Cross-Exporte, und der rote
`test_mcp_bridge` auf Windows.

---

## 9. Was davon umgesetzt ist

Stand 08.09.2026, Commits `7362b42e` (P0, P1) und `009c54c7` (P2, P3, P5).

| Nr. | Stand |
|---|---|
| P0 | umgesetzt. `compileHlslPinned` gibt im Stub `failed()` zurueck, wie die anderen drei. Der Dateikopf sagt jetzt ausdruecklich, dass Header und Stub zusammen vollstaendig bleiben muessen und warum das erst beim Linken auffaellt. |
| P1 | umgesetzt als `--source-tree DIR` (wiederholbar), vorne in der Liste von `existing_sources()`. |
| P2 | umgesetzt. Zwei Schritte in `ci.yml` nach `Run tests`, beide `if: github.event_name == 'push'`: der Auspraegungsbau und die Groessenpruefung. Die Deploy-Wurzel wird plattformweise in einer Bash-Zeile bestimmt und ueber `GITHUB_ENV` an die folgenden Schritte weitergereicht. |
| P3 | umgesetzt. Windows und Linux: ein `mv`-Schritt nach `package/Editor`. macOS: `package_macos.sh` hat statt des einen `Game`-Blocks eine Schleife ueber `Game AppAdvanced AppBasic`, an derselben Stelle, also weiterhin vor dem Signieren. |
| P4 | umgesetzt fuer macOS, siehe Abschnitt 10. Windows und Linux bleiben offen, und zwar bis zum Merge nach main. |
| P5 | umgesetzt. `runtime-flavors.yml` laeuft nur noch auf `claude/**`. |
| P6 | bewusst nicht gemacht: Bequemlichkeit, nicht Pipeline. |

**Eine Nebenwirkung, die niemand ueberraschen darf:** das Windows-Verpacken stand bisher
**vor** `Run tests` und lud den Editor deshalb auch dann hoch, wenn ctest rot war. Es steht
jetzt dahinter, wie macOS und Linux es seit jeher tun, weil die App-Runtimes zwischen Test
und Verpacken entstehen. Solange `test_mcp_bridge` auf Windows rot ist (Abschnitt 0,
fremder Punkt), faellt damit das Windows-Artefakt aus. Der Tausch ist der richtige — die
Alternative, den Auspraegungsbau `continue-on-error` zu setzen, lieferte stillschweigend
einen Editor ohne App-Runtimes und damit genau den Fehler zurueck, gegen den dieses Thema
angetreten ist —, aber er macht den Windows-Flake zur Vorbedingung fuer den naechsten
Windows-Download.

### Was verifiziert ist und was nicht

**Gemessen, macOS/arm64, 08.09.2026.** Beide Auspraegungen bauen und deployen wieder,
Rueckgabecode 0:

```
scripts/build_runtimes.py --flavor app-basic --flavor app-advanced --build-type Release
    --jobs 8 --define HE_PORTABLE_BUILD=ON --define HE_PREFER_MBEDTLS=ON
    --source-tree <Hauptcheckout>/cmake-build-release
```

| | total | ohne python | rendering | Schwelle |
|---|---|---|---|---|
| `AppAdvanced` | 74,5 MB | 19,8 MB | 1,2 MB | 85 / 26 MB, eingehalten |
| `AppBasic` | 73,7 MB | 19,0 MB | 0,4 MB | 84 / 25 MB, eingehalten |

Das ist zugleich der Beweis fuer P0: `app-basic` und `app-advanced` linken beide gegen den
Stub, und beide linken durch.

**P1 gemessen, nicht nur geschrieben.** In den zwei Auspraegungsbaeumen liegen unter
`_deps` nur `mbedtls-src` und `sqlite3-src`, also genau die zwei Pakete, die nicht in
`SHARED_SOURCES` stehen. SDL3, glm, Jolt, Recast, Lua, nlohmann_json und astcenc wurden
kein einziges Mal geklont; im Log steht keine einzige `Cloning into`-Zeile. Ohne
`--source-tree` liefert `existing_sources()` in einem frischen Worktree eine leere Liste,
mit ihm sieben Flags.

**P3 auf macOS gelaufen, nicht nur geschrieben.** `scripts/package_macos.sh` gegen den
Release-Editor-Deploy des Hauptcheckouts (175 MB, hereinkopiert), Rueckgabecode 0:

```
Game/ runtime        → Resources/Game
AppAdvanced/ runtime → Resources/AppAdvanced
AppBasic/ runtime    → Resources/AppBasic
```

In allen dreien liegt `HorizonGame`, `codesign --verify --deep --strict` gibt 0 zurueck,
die zwei neuen Verzeichnisse sind also von der Signatur gedeckt — die Kopie sitzt richtig
vor dem Signieren. Das Editorbinary in diesem `.app` stammt aus dem Hauptcheckout und nicht
aus diesem Zweig; getestet war der Packager, nicht der Editor.

**Nicht gefallen und hier auch nicht faelschbar:** Windows und Linux, die einen CI-Lauf
brauchen — der Push dieses Zweigs hat `runtime-flavors.yml` ausgeloest (Lauf
`34214918244`), der P0 auf allen drei Plattformen beantwortet, aber niemand hat ihn
abgewartet. Und der Satz `Runtime: AppAdvanced` im Export-Log, der die eigentliche Frage
des Themas beantwortet: dafuer braucht es einen Editor aus diesem Zweig und einen echten
App-Export.

---

## 10. P4: der Weg einmal ganz durchlaufen (08.09.2026, Schritt 5)

Der Satz, um den es dem ganzen Thema geht, steht jetzt gemessen da:

```
Runtime: AppAdvanced (…/out/deploy/AppAdvanced)
Runtime: AppBasic    (…/out/deploy/AppBasic)
```

Kein Rueckfall auf `Game`, keine Warnzeile. Was dahinter steckt und was
ausdruecklich **nicht** dahinter steckt, steht hier vollstaendig.

### 10.1 Der Editor wurde nicht neu gebaut, und warum das genuegt

`git diff --stat 50abd19b..HEAD -- src/` ist eine einzige Datei:
`ShaderCompilerStub.cpp`. Die uebersetzt ein Baum nur mit `HE_ENABLE_SHADERC=OFF`,
also in den zwei App-Auspraegungen und nie im Editor. Ein Editor aus diesem Zweig
waere derselbe Editor wie der aus main.

Geprueft statt angenommen: das vorhandene `out/deploy/Editor/HorizonEditor`
(08.09., 12:20) traegt den auspraegungsfaehigen Exporter — die Zeichenkette
`shipping the full game runtime` steht drin, `AppAdvanced` in seinem
`libHorizonCore.dylib`. Ein Editor **vor** A3b haette beide nicht gehabt und die
Verifikation waere wertlos gewesen.

**Also: „Editor gebaut" waere hier gelogen.** Was gebaut wurde, sind die zwei
App-Runtimes und das `.app`; der Editor ist der aus dem Hauptcheckout, Stand nach
PR #50, quelltextgleich mit diesem Zweig.

### 10.2 Der Export, ohne die ImGui-Maske

`ExportDialogPanel.cpp:1206-1264` macht vier Dinge: es liest die zwei
Projekteigenschaften, ruft `runtimeFlavorFor`, dann `findRuntimeBundle` und
danach `ProjectExporter::exportProject` mit `settings.gameRuntimeDir` auf das
Gefundene. Ein 50-Zeilen-Programm, gegen das `libHorizonCore.dylib` **aus dem
Auspraegungsbaum dieses Zweigs** gelinkt (`out/runtime-builds/app-advanced/src/
HE_Core/`), macht dieselben vier Aufrufe mit denselben Argumenten:

```
clang++ -std=c++20 -Isrc/HE_Core/include verify_export.cpp \
    out/runtime-builds/app-advanced/src/HE_Core/libHorizonCore.dylib
verify_export <editorBaseDir> <contentDir> <name> <outDir> <appProject> <advanced>
```

Drei echte Projekte aus `~/HorizonEngineProjects`, keine Attrappen, und der dritte
ist die Gegenprobe:

| Projekt | `appProject` / `advanced` | gewuenscht | **gefunden** | Binaerdateien |
|---|---|---|---|---|
| `AppSide` | true / true | AppAdvanced | **AppAdvanced** | 8 |
| `AppTest` | true / false | AppBasic | **AppBasic** | 8 |
| `Test` | false / true | Game | **Game** | 13 (bzw. 12 im `.app`) |

**Beide Fundorte, nicht nur einer.** Einmal mit `editorBaseDir =
out/deploy/Editor/` — das ist der Editor aus dem Build-Baum, der Aufstieg findet
`out/deploy/AppAdvanced` eine Ebene hoeher. Und einmal mit `editorBaseDir =
out/dmg_staging/HorizonEditor.app/Contents/Resources/` — das ist der
**ausgelieferte** Editor, und dort liegt die Auspraegung direkt daneben, weil
`package_macos.sh` sie hineinkopiert hat. Der zurueckgegebene Pfad liegt in beiden
Faellen dort, wo er hingehoert; im `.app`-Fall innerhalb des Bundles.

Der zweite Fall ist der, der die Frage des Themas beantwortet: **wer den Editor
herunterlaedt, exportiert die schlanke Auspraegung.**

### 10.3 Die Groessen, gegen den Plan gehalten

Runtime-Bundles, `scripts/runtime_size.py`, macOS/arm64:

| | total | ohne Python | rendering | Schwelle |
|---|---|---|---|---|
| `Game` | 85,4 MB | 30,7 MB | 7,2 MB | 90 / 32 MB, eingehalten |
| `AppAdvanced` | 74,5 MB | 19,8 MB | 1,2 MB | 85 / 26 MB, eingehalten |
| `AppBasic` | 73,7 MB | 19,0 MB | 0,4 MB | 84 / 25 MB, eingehalten |

Das sind dieselben Zahlen wie in Abschnitt 9, unabhaengig nachgemessen, und sie
decken sich mit den Plan-Schwellen.

Wichtiger ist aber, was beim **Export** herauskommt, denn das ist das, was ein
Nutzer bekommt. Diese drei Projekte sind keine Python-Projekte, also faehrt keine
Standardbibliothek mit, und die 74 MB des Bundles werden zu:

| Export | Groesse | `libHorizonRendering.dylib` | `libcrypto` |
|---|---|---|---|
| `AppSide` (AppAdvanced) | **16 MB** | 0,9 MB | -- |
| `AppTest` (AppBasic) | **15 MB** | 0,3 MB | -- |
| `Test` (Game) | **25 MB** | 5,8 MB | 4,8 MB |

**Rund 10 MB je App-Export**, also 40 Prozent, und der Unterschied ist genau der,
den A3b versprochen hat: der Renderer und der Shader-Uebersetzer, die eine App
nicht braucht.

### 10.4 Die exportierten Apps starten auch

Ein kopiertes Verzeichnis ist noch kein laufendes Programm, also wurden beide
gestartet, jeweils rund elf Sekunden, dann abgeschossen:

```
AppTest  : runtime flavour 'app-basic'    → SoftwareRenderer: CPU rasterizer, user interface only
           101 Frames, 0 errors, 0 critical
AppSide  : runtime flavour 'app-advanced' → MetalRenderer: initialized on Apple M5, scene pass forward
            35 Frames, 0 errors, 0 critical
```

Beide melden im Log die Auspraegung, mit der sie gebaut wurden, beide gehen in
den Anwendungsmodus (`application mode — no world, no physics, no scene`) und
beide fahren sauber herunter.

### 10.5 Was NICHT verifiziert ist

* **Die Export-Maske selbst wurde nicht bedient.** Geprueft ist der Weg, den sie
  geht, nicht ihre Knoepfe. Ein Fehler, der zwischen dem Klick und
  `runtimeFlavorFor` saesse, faende sich hier nicht — dafuer muesste jemand den
  Editor oeffnen und exportieren.
* Das Pruefprogramm reicht kein `gameInstanceJson` durch (der Dialog tut es), also
  warnt `AppTest` beim Start ueber die fehlende GameInstance. Das ist eine Luecke
  des Pruefstands, nicht des Exports.
* **Die neuen `ci.yml`-Schritte sind auf diesem Zweig nie gelaufen und koennen es
  nicht.** Sie haengen an `if: github.event_name == 'push'` und `ci.yml` laeuft nur
  auf main und auf PRs gegen main. Windows und Linux — sowohl der Auspraegungsbau
  als auch die `mv`-Kopie ins Editorpaket — sind bis zum Merge blind. Das ist eine
  Entscheidung fuer den Merge, kein Rest dieses Schritts.

### 10.6 P0 auf allen drei Plattformen, jetzt abgewartet

Lauf `34214918244` (`runtime-flavors.yml`, Commit `4d44bbd4`) ist durch und **gruen auf
Linux, macOS und Windows**, 28 min 53 s auf dem laengsten Job. Damit ist der Blocker aus
Abschnitt 0 erledigt und nicht nur lokal: `compileHlslPinned` im Stub linkt ueberall, alle
drei Auspraegungen bauen auf allen drei Plattformen, und jede haelt ihre Schwelle.

| | `game` gesamt / ohne Python | `app-advanced` | `app-basic` |
|---|---|---|---|
| Windows/x64 | 37,8 / 28,0 MB | 33,7 / 24,0 MB | 33,1 / 23,4 MB |
| Linux/x64 | 54,8 / 32,1 MB | 48,0 / 25,2 MB | 47,1 / 24,3 MB |
| macOS/arm64 | 56,6 / 25,6 MB | 50,6 / 19,5 MB | 49,8 / 18,8 MB |

Alle neun `OK: within the … thresholds`. Die Zahlen liegen ueber denen vom 05.09.2026
(Abschnitt 6), weil der Baum seither gewachsen ist, nicht weil sich der Schnitt verschoben
haette: der Abstand zwischen `game` und `app-advanced` ist auf jeder Plattform derselbe wie
vorher.

Dieselbe Schleife, mit der `ci.yml` die Schwellen jetzt selbst prueft, wurde lokal Zeile fuer
Zeile gefahren (`runtime_size.py --check` ueber `Game`, `AppAdvanced`, `AppBasic`, mit der
Behandlung von Rueckgabecode 2): dreimal `OK`, Gesamtergebnis 0.

**Was dieser Lauf NICHT beweist:** er ist `runtime-flavors.yml`, nicht `ci.yml`. Er
beantwortet P0 und die Groessen, aber kein Bundle verlaesst dort den Runner. Der Schritt
`Stage the app runtimes into the editor package` — `[ -d ]`, `mv`, `rm -f` auf einem Pfad wie
`D:\a\HorizonEngine\HorizonEngine/package/AppAdvanced` — ist auf Windows noch nie gelaufen.
Dass `build_runtimes.py` mit genau diesem Pfad umgehen kann, zeigt dieser Lauf; dass die
MSYS-Bash-Werkzeuge es auch tun, ist wahrscheinlich und ungeprueft.
