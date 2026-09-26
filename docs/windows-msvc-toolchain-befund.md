# Windows: horizoncode findet MSVC nicht — Befund (Thema 96, Schritt 1)

Stand 2026-09-26, Zweig `claude/windows-msvc-toolchain-erkennung`, Basis `152659ff`.

**Was dieser Bericht ist:** Lokalisierung aus dem Code plus ein Blick in den
Windows-CI-Log. **Nicht** auf einer Windows-Maschine reproduziert — der Bericht
entstand auf einem Mac. Alles unten ist entweder *belegt* (Code-Zeile, CI-Log)
oder ausdrücklich als *Hypothese* markiert. Das Repro-Rezept für einen
Windows-Rechner (NN-WS03) steht am Ende.

## 1. Es gibt keine eigene MSVC-Erkennung

Im Engine-Code gibt es keinen einzigen Treffer für `vswhere`, `VCINSTALLDIR`,
`VSINSTALLDIR`, `vcvars` oder eine Suche nach `cl.exe`/`link.exe`
(`git grep`, ohne Vendor-Code). Die Annahme im Thema („verlässt sich auf PATH
oder VCINSTALLDIR“) trifft den Ort nicht: die **gesamte** Compiler-Erkennung ist
ein `cmake -S … -B …` mit dem **Default-Generator** von cmake.

Der Code-Pfad (alles `src/HE_Scene/src/HcCodegen.cpp`):

| Stelle | Zeilen | Was passiert |
|---|---|---|
| `shq()` | 3997–4000 | Windows: Argument einfach in `"…"` einpacken |
| `cmakeAnswers()` | 4050–4057 | `std::system("<cmake> --version >NUL 2>&1")` |
| `resolveCmake()` | 4064–4088 | 1. `<Editor>/cmake/bin/cmake.exe` (gequotet via `shq`), 2. blankes `cmake` vom PATH |
| `runStreaming()` | 4101–4105 | `_popen(cmd + " 2>&1")` → läuft als `cmd.exe /c <cmd>` |
| `probeToolchain()` | 4129–4197 | Wegwerf-Projekt `project(he_toolchain_probe CXX)`, `cmake -S -B` (4173f.); rc≠0 ⇒ `compilerFound=false`, `detail` = letzte 15 Zeilen |
| `buildDylib()` | 4435–4491 | Configure (4461–4468, ohne `-G`) + `cmake --build … --config Release` (4474) |
| `installToolchain()` (Win) | 4286–4307 | winget: Kitware.CMake + VS 2022 BuildTools (VCTools-Workload) |

Aufrufer:

- `src/HE_Editor/EditorApplication.cpp:1797–1819` — Start-Check, setzt `setBundledCmakeDir(<Editor>/cmake)`
- `src/HE_Editor/EditorSettingsPanel.cpp:1563–1575` — Tool-Status-Zeilen „cmake“ / „C++ compiler: not found“
- `src/HE_Editor/ToolchainDialog.cpp:133–141` — „Details“ zeigt `ToolchainProbe::detail` (die Log-Zeilen, die der Mensch kopieren kann), `:190–194` winget-Befehl
- `src/HE_Editor/ExportDialogPanel.cpp:1798` — Spiel-Export (HorizonCode → C++ → `HorizonCodeGen.dll`)
- `src/HE_Editor/GameLogicBuildPanel.cpp:131` — GameLogic-DLL
- Das generierte `CMakeLists.txt` (`generateCMakeLists`, HcCodegen.cpp:3923ff.) legt weder Generator noch Compiler fest.

Nebenschauplätze (kein Editor-Pfad):

- `configure_x64.bat:2` — hartkodiert `C:\Program Files\Microsoft Visual Studio\18\Community\...\vcvars64.bat`; scheitert bei Professional/Enterprise/BuildTools oder VS 2022. Nur Entwickler-Build.
- `scripts/build_runtimes.py:105–124` — nimmt Ninja, sobald `ninja` auf PATH liegt; Ninja braucht `cl.exe` + `INCLUDE`/`LIB` aus der Umgebung ⇒ ohne Developer Prompt `No CMAKE_CXX_COMPILER could be found`. Läuft in CI nur innerhalb `msvc-dev-cmd`.

## 2. Warum CI davon nichts merkt (belegt)

- `.github/workflows/ci.yml:37` (und `runtime-flavors.yml:74`): `ilammy/msvc-dev-cmd@v1` — **jeder** Windows-Job läuft in einer Developer-Umgebung. Log von Lauf 36260400179 (Job 108454993111): „Found with vswhere: C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat“, MSVC 14.51.36231, `VCINSTALLDIR`/`INCLUDE`/`LIB` gesetzt.
- Engine-Configure `ci.yml:159` mit `-G Ninja` — funktioniert nur wegen dieser Umgebung.
- Gebündeltes cmake = das cmake des Runners, laut Log **4.4.3** (kennt „Visual Studio 18 2026“ und „17 2022“).
- `tests/test_gamelogic_build.cpp` ruft `buildDylib` **ohne** `setBundledCmakeDir` ⇒ `resolveCmake()` liefert das blanke Wort `cmake` (ungequotet). Der Pfad, den ein ausgelieferter Editor nimmt (gequoteter Pfad zum gebündelten cmake), wird von keinem Test und keinem CI-Job ausgeführt.

## 3. Wichtige Einordnung

cmakes **Visual-Studio-Generator** (Default auf Windows) findet MSVC selbst über
die VS-Setup-API (dasselbe, was vswhere benutzt) — er braucht **kein** vcvars,
kein `VCINSTALLDIR`, kein `cl.exe` auf PATH, und er kennt auch reine Build Tools.
Plain PowerShell + gebündeltes cmake 4.4.3 + VS 2022/2026 mit C++-Workload
*sollte* also gehen. Das Scheitern braucht einen konkreten Mechanismus. Kandidaten,
nach Wahrscheinlichkeit:

### H1 (Hauptverdacht, Hypothese): `cmd /c` frisst Anführungszeichen

`_popen` und `std::system` lassen `cmd.exe /c <Zeile>` laufen (ohne `/S`).
Regel aus `cmd /?`: Anführungszeichen bleiben nur erhalten, wenn die Zeile
**genau zwei** davon enthält (plus weitere Bedingungen). Sonst wird, wenn das
erste Zeichen ein `"` ist, **das erste und das letzte `"` der Zeile entfernt**.

- `cmakeAnswers`: `"C:\…\cmake\bin\cmake.exe" --version >NUL 2>&1` — genau zwei
  Quotes ⇒ überlebt ⇒ Tool-Status „cmake: 4.4.3“ grün.
- Probe/Configure: `"C:\…\cmake.exe" -S "C:\…\Temp\he_toolchain_probe_1234" -B "C:\…\build" 2>&1`
  — sechs Quotes, beginnt mit `"` ⇒ cmd macht daraus
  `C:\…\cmake.exe" -S "C:\…\he_toolchain_probe_1234" -B "C:\…\build 2>&1`
  ⇒ zerlegter Befehl; `2>&1` steht zudem in einem offenen Quote-Bereich.
- Ergebnis: rc≠0 ⇒ `compilerFound=false` ⇒ **„C++ compiler: not found“**,
  obwohl VS korrekt installiert ist. Genau das Bild aus dem Thema.
- Trifft **jeden** CI-gepackten Editor (der Pfad zum gebündelten cmake ist
  immer gequotet); Dev-Builds (`HE_BUNDLE_CMAKE` aus, blankes `cmake`) und die
  Tests treffen es nie. `buildDylib` (Export, GameLogic) hätte dasselbe Problem.
- Zu erwartender Text in „Details“ etwa:
  `'C:\…\cmake.exe" -S "C:\…' is not recognized as an internal or external command`
  oder `The filename, directory name, or volume label syntax is incorrect.`
  — also *kein* cmake-Compilerfehler. **Genauen Wortlaut auf Windows prüfen.**

Wenn H1 stimmt, ist Schritt 2 im Kern kein vswhere-Port, sondern: Prozesse ohne
cmd starten. `HE::Proc::run` (`src/HE_Core/include/Platform/Process.h:95`,
`CreateProcessW` + korrektes argv-Quoting in `Process.cpp:143ff.`) existiert
bereits und wird von HcCodegen nicht benutzt. (Alternative Minimal-Lösung: die
ganze Zeile in ein zusätzliches Paar `"…"` hüllen.)

### H2: `CMAKE_GENERATOR` in der Benutzerumgebung

VS Code CMake Tools, vcpkg-Setups oder CLion setzen gern `CMAKE_GENERATOR=Ninja`
(oder `NMake Makefiles`) global. Dann nimmt cmake Ninja/NMake statt des
VS-Generators ⇒ braucht `cl.exe` auf PATH ⇒
`No CMAKE_CXX_COMPILER could be found.` bzw.
`CMAKE_CXX_COMPILER not set, after EnableLanguage`. Das wäre die Meldung, die
wörtlich zu „findet cl.exe nicht“ passt.

### H3: Veralteter Build-Ordner

`buildDylib` konfiguriert in einen bestehenden `-B`-Ordner
(`<Projekt>/Source/build`, `<genDir>/build`). Hat dort vorher jemand aus
Developer Prompt/CLion mit Ninja konfiguriert, bleibt `CMAKE_GENERATOR` im
Cache ⇒ wie H2, oder `cl.exe` gefunden, aber `cannot open include file
'cstddef'` / `LNK1104 kernel32.lib`, weil `INCLUDE`/`LIB` fehlen.
Der Editor wählt den Generator nie neu.

### H4: Kein gebündeltes cmake ⇒ cmake vom PATH

Editor aus eigenem Build (`HE_BUNDLE_CMAKE=OFF`, z. B. `configure_x64.bat`):
`resolveCmake` fällt auf `cmake` vom PATH zurück. Das cmake, das VS mitbringt
(`Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`), liegt nur in der
Developer Prompt auf PATH ⇒ in PowerShell/Doppelklick Zeile „cmake“ rot. Liegt
ein älteres Kitware-cmake auf PATH, das VS 18 nicht kennt (VS 2026 erst ab
cmake 4.2), fällt es auf NMake zurück ⇒
`Running 'nmake' '-?' failed` + `CMAKE_CXX_COMPILER not set`.

### H5: VS ohne C++-Komponente

VS installiert, aber ohne „Desktopentwicklung mit C++“
(`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`): cmake überspringt die
Instanz ⇒ gleicher NMake-Rückfall wie H4.

## 4. Repro-Rezept (Windows, normale PowerShell, NICHT Developer Prompt)

Auf NN-WS03 mit dem CI-Paket `HorizonEditor-windows-x64.zip`:

1. Umgebung festhalten:
   `where.exe cmake; where.exe cl; $env:CMAKE_GENERATOR; $env:VCINSTALLDIR`
   und `& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -all -products * -format text`
2. Editor einmal nach `C:\HE\Editor` (ohne Leerzeichen) und einmal nach
   `C:\HE Test\Editor` (mit) entpacken, per Doppelklick starten,
   Preferences → Tool Status: beide Zeilen + „Details“-Text wörtlich kopieren.
3. Probe von Hand, einmal direkt, einmal so, wie der Editor es tut:
   ```powershell
   mkdir $env:TEMP\probe; Set-Content $env:TEMP\probe\CMakeLists.txt "cmake_minimum_required(VERSION 3.20)`nproject(p CXX)"
   & "C:\HE\Editor\cmake\bin\cmake.exe" -S "$env:TEMP\probe" -B "$env:TEMP\probe\build"      # erwartet: geht (H1-Kontrolle)
   cmd /c "`"C:\HE\Editor\cmake\bin\cmake.exe`" -S `"$env:TEMP\probe`" -B `"$env:TEMP\probe\build2`" 2>&1"   # wie _popen: scheitert laut H1
   ```
4. Liegt schon ein `Source\build\CMakeCache.txt` im Projekt: Zeile
   `CMAKE_GENERATOR:INTERNAL=` notieren (H3).
5. Export- bzw. GameLogic-Build auslösen, `build.log` beilegen.

Zuordnung: Schritt 3 direkt grün + über `cmd /c` rot ⇒ H1. Beide rot mit
„No CMAKE_CXX_COMPILER“ ⇒ H2/H3/H5 (Generatorzeile im Log ansehen).
„cmake“-Zeile rot ⇒ H4.

## 4a. Reproduktion auf NN-WS03 (Schritt 2, 2026-09-26): H1 bestätigt

Normale PowerShell, kein Developer Prompt: `where cl` findet nichts, VCINSTALLDIR/
INCLUDE/CMAKE_GENERATOR leer. vswhere findet drei Instanzen, alle mit
`VC.Tools.x86.x64`: Community 2026 (18.10), BuildTools 2026 (18.9), Community 2022 (17.14).

- cmake direkt aus PowerShell → rc=0, „Building for: Visual Studio 18 2026“. cmake
  findet MSVC ohne jede Umgebung (H2–H5 treffen hier nicht zu).
- `cmd /c "…"` aus PowerShell → ebenfalls rc=0. Das beweist nichts, denn PowerShell 5.1
  quotet die Zeile für native Programme selbst um. Rezept §4.3 kann H1 deshalb nicht zeigen.
- Editor-Aufruf 1:1 in C++ nachgebaut (shq + `std::system` + `_popen`):
  `cmakeAnswers` rc=0 (Tool Status „cmake“ grün), Probe rc=1 mit
  `Die Syntax für den Dateinamen, Verzeichnisnamen oder die Datenträgerbezeichnung ist falsch.`
  (Pfad ohne Leerzeichen) bzw.
  `Der Befehl "C:\…\he_repro\Space" ist entweder falsch geschrieben oder konnte nicht gefunden werden.`
  (Pfad mit Leerzeichen).

Fix: `cmdLine()` in HcCodegen.cpp hüllt jede Zeile für `std::system`/`_popen` auf
Windows in ein zusätzliches Paar Anführungszeichen, und genau das entfernt cmd.
Test: `tests/test_toolchain_quoting.cpp` (Probe und `buildDylib` über das gebündelte,
also gequotete cmake; Leerzeichen in -S, -B und im Wert eines -D). Ohne Fix rot mit
`Der Befehl "C:\Program" ist entweder falsch geschrieben…`, mit Fix grün.

Nebenbefund: Bei drei Instanzen nahm cmakes Default-Generator „Visual Studio 18 2026“
die `cl.exe` aus **BuildTools 18.9**, nicht aus Community 18.10 (gleiches Toolset
14.51.36231). Wichtig nur, falls später „neueste Instanz gewinnt“ explizit gebaut wird.

## 5. Offen für Schritt 2

- Reproduktion auf Windows (Rezept oben) — entscheidet zwischen H1 und H2–H5.
- Unabhängig davon für das Thema nötig: explizite Erkennung (vswhere bzw.
  VS-Setup-API, `-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`,
  alle Editionen inkl. BuildTools, neueste Version gewinnt) und `-G "Visual Studio <N> <Jahr>"`
  explizit übergeben statt Default/`CMAKE_GENERATOR` zu erben — sonst schlagen H2–H5 weiter zu.
- Test: die Windows-Tests laufen in CI innerhalb `msvc-dev-cmd` und mit
  blankem `cmake`; ein Test für „mehrere/keine Installation“ muss die Auswahl
  von der echten Maschine trennen (vswhere-Ausgabe als Eingabe), und der gequotete
  Bundle-Pfad braucht einen eigenen Fall.
