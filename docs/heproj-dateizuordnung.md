# Dateityp .heproj: Doppelklick öffnet den Editor (Thema 161)

Stand 2026-10-09, Schritt 4. Dieses Dokument sagt, was beim Doppelklick auf eine
`.heproj` je Plattform passiert, wo die Teile im Repo liegen, was geprüft wurde
und was als Abnahme auf echter Hardware offen bleibt.

## Was beim Doppelklick passiert

Der Editor bekommt den Pfad auf zwei Wegen, die Plattform entscheidet:

* Windows und Linux starten den Editor mit der Datei als Argument (Registry
  `"%1"`, Eintrag in der Desktopdatei `%f`). Alle Argumente, die keine Optionen
  sind, liegen in `launchArguments()`.
* macOS startet ohne Argument und schickt ein Ereignis zum Öffnen des Dokuments,
  auch an einen schon laufenden Editor. SDL macht daraus `SDL_EVENT_DROP_FILE` ohne Fenster
  (windowID 0). Beim Start wird dieses Ereignis in `pickLaunchProject` nur
  angeschaut, nicht entnommen. Das erste Projekt ist dadurch das richtige. Das
  Ereignis erreicht danach `OnEvent`, findet sein Projekt offen und tut nichts.

Beide Wege münden in `ProjectLaunchOpen` (`src/HE_Editor/ProjectLaunchOpen.h`),
einen Speicher für genau eine wartende Anfrage. Der Projekt Hub nimmt den Pfad über sein eigenes Laden,
der Editor über `requestGuarded(GuardedAction::OpenProjectPath, ...)`, also mit der
Rückfrage bei ungespeicherten Änderungen. Wartet ein Dialog, wartet auch das
Projekt und das Log nennt den Dialog. Alles, was nicht geöffnet werden kann,
steht im Log. Fehlt eine beim Start genannte `.heproj` oder ist der Pfad ein Ordner
namens `*.heproj`, zeigt der Editor den Hub mit dem Grund statt des letzten
Projekts. Ein Argument mit anderer Endung wird nur im Log vermerkt, der Start
verläuft normal. Zur Laufzeit (macOS Ereignis) bleibt es bei der Logzeile.

## Wo die Teile liegen

| Plattform | Registrierung | Dateien |
|---|---|---|
| macOS | `CFBundleDocumentTypes` und `UTExportedTypeDeclarations` in der Info.plist des Bundles, geschrieben von `scripts/package_macos.sh` (Abschnitt 7, Kontrolle in 10b) | `scripts/dmg_assets/gen_assets.py` (Subkommando `docicon`, erzeugt `ProjectIcon.icns`) |
| Windows | Registry unter HKCU (ProgID, Symbol, Befehl zum Öffnen), kein Administrator, einmal nach dem Entpacken ausführen | `scripts/windows_assets/register_heproj.cmd`, `register_heproj.ps1`, `unregister_heproj.cmd`, `heproj.ico`, Prüfung `check_heproj_registration.ps1` |
| Linux | Desktopdatei, MIME XML und Symbole im eigenen XDG Datenordner, einmal nach dem Entpacken ausführen | `scripts/linux_assets/install_file_types.sh`, `horizon-editor.desktop`, `horizon-editor.xml`, `application-x-heproj.png`, `horizon-editor.png` |

Windows und Linux haben keinen Installer (ZIP und tar.gz aus `package/Editor`).
Deshalb reist die Registrierung als Ordner `FileTypes/` im Paket mit, kopiert per
POST_BUILD in `src/HE_Editor/CMakeLists.txt` (nicht auf macOS). Typname,
ProgID und MIME bleiben auf allen drei Plattformen gleich:
`dev.horizoncreations.heproj` bzw. `application/x-heproj`.

## Was geprüft wurde

### macOS, belegt (Debug Bundle aus diesem Zweig, 2026-10-09)

| Prüfung | Ergebnis |
|---|---|
| `plutil -lint` auf der Info.plist | OK |
| PlistBuddy, Dokumenttyp | Name „Horizon Engine Project“, Rolle Editor, Rang Owner, `LSItemContentTypes` = `dev.horizoncreations.heproj`, Symbol `ProjectIcon` |
| PlistBuddy, exportierter Typ | `dev.horizoncreations.heproj`, konform zu `public.json`, Endung `heproj`, MIME `application/x-heproj`, Symbol `ProjectIcon` |
| Das DMG | Info.plist byteidentisch zu der im Staging (`cmp`), beide Typen vorhanden, `ProjectIcon.icns` und `AppIcon.icns` im Bundle, `codesign --verify --deep --strict` in Staging und DMG OK |
| `lsregister -f` auf das Staging Bundle, dann `mdls` auf eine Test `.heproj` | `kMDItemContentType = dev.horizoncreations.heproj`, `kMDItemKind = Horizon Engine Project` |
| Standard Handler (`NSWorkspace.urlForApplication(toOpen:)`) | vorher Visual Studio Code, nach der Registrierung das Staging Bundle, nach `lsregister -u` wieder Visual Studio Code |
| Echter Start: `open -n -a <Bundle> <Datei>` mit privatem HOME | Log „opening … (handed over at launch)“, Projekt geladen |
| Laufender Editor, `open -a <Bundle> <dieselbe Datei>` | Log „… is already the open project“, kein Wechsel |
| Laufender Editor, `open -a <Bundle> <andere Datei>` | Log „opening … (handed over by the system)“, Welt geleert, anderes Projekt geladen |

Beobachtungen: `lsregister -dump` führt die UTI weiter als `untrusted`, während
`mdls` und `NSWorkspace` sie trotzdem auflösten. Möglicherweise, weil dasselbe
Bundle an diesem Pfad in Schritt 2 schon einmal gestartet wurde. Das wurde nicht
isoliert geprüft. Für eine frisch installierte App bleibt die Regel aus Schritt 2:
erst nach dem ersten Start ist sie sicher der Handler (steht im Handbuchtext unten).
Die Test Projektkopie war mit rund 202000 Entities schwer, das Laden dauerte etwa
12 Sekunden. Das ist die Eigenart der Kopie, nicht des Öffnens.

### Windows und Linux, auf CI belegt, nicht auf Hardware

CI Lauf 37855344542 auf `ac613e9f` war grün (Windows, Linux, Linux Vulkan lavapipe, macOS).

* `tests/test_heproj_file_types.py`: 23 Fälle, hier auf dem Mac 21 gelaufen, 2
  übersprungen (Auflösung über glib/gio gibt es nur unter Linux, `--package` nur
  mit Paketordner). Er hält Typname, ProgID, MIME, Endung und Glob in allen drei
  Plattformen gleich, validiert Desktop Eintrag (`desktop-file-validate`) und MIME
  XML und lässt den Linux Installer echt gegen einen Wegwerf `XDG_DATA_HOME`
  laufen, auch mit Leerzeichen und Klammern im Pfad.
* Linux Runner: derselbe Test mit `--package package/Editor` (das ausgelieferte
  Paket trägt die Dateien byteidentisch) und die gio Auflösung der Endung samt
  Start mit Pfad.
* Windows Runner: derselbe Test mit `--package`, dazu
  `check_heproj_registration.ps1`. Es schreibt die Schlüssel unter HKCU
  (vorher gesichert, danach wiederhergestellt), liest sie zurück, fragt die Shell,
  was `.heproj` auflöst, und entfernt sie wieder, auch aus einem Pfad mit
  Leerzeichen und Klammern (`cmd /S /C`).
* Die PowerShell Dateien wurden auf dem Mac nie ausgeführt, nur auf dem Windows
  Runner.

### Tests des Editors

`he_tests` über `test_project_launch_open.cpp` (9 Fälle, 62 Prüfungen: Anführungszeichen,
Leerraum, relative Pfade, `file://`, falsche Endung, erstes gültiges Argument,
Hub gegen geführten Wechsel gegen schon offen, ein Platz für genau eine wartende
Anfrage) und die Nachbarn (Asset und Szenen Autosave, Dock Zustand,
Benachrichtigungen, Report Issue, Projekt Einstellungen, Projekt Export): 115 Fälle,
860 Prüfungen, alle grün, auf dem Zweig vor dem Zusammenführen mit `release/0.7.0`.
Voller ctest ohne `test_material_graph` auf demselben Stand: 235 von 237 grün,
`test_culling` und `test_widget_designer_ui` liefen bei 240 s ins Zeitlimit (Debug,
Last 11 auf dem Rechner) und sind einzeln grün (309 s und 125 s).

Danach wurde `release/0.7.0` (265 Commits weiter) in den Zweig gemerged, zwei
additive Konflikte (Quellenliste in `src/HE_Editor/CMakeLists.txt`, Include Liste in
`EditorUI.cpp`, jeweils `GitHubSignIn` neben `ProjectLaunchOpen`, beide behalten).
Auf diesem Stand: Build aller Ziele `-j8` ohne Fehler, gezielte Tests grün
(`he_tests` 129 Fälle, 8180 Prüfungen, darunter `test_project_launch_open` und
`test_docs_library`, dazu `heproj_file_types`, `test_editor_help`,
`test_github_oauth`) und voller ctest ohne `test_material_graph`: 251 Fälle, 248
gelaufen und alle grün, keine Zeitüberschreitung (Last war niedriger). Die drei
`runtime_size` Fälle werden übersprungen, sie brauchen einen Release Build. `test_material_graph` wurde nicht
gelaufen (im Debug Build bekannt langsam, berührt dieses Thema nicht).

## Offen, braucht Hardware

* Ein echter Doppelklick im Finder mit einem installierten Release DMG dieses
  Zweigs, im Windows Explorer und in einem Linux Dateimanager. Gesehen wurde bisher
  nur der Weg über `open`, die Registry und gio, nicht der Klick.
* Die Symbolanzeige des Dateityps im Finder, im Explorer und im Linux Dateimanager.
* Die Rückfrage bei ungespeicherten Änderungen mit sichtbarem Dialog: Schritt 1 sah
  auf macOS die Popup Kennung im Log und den ausbleibenden Wechsel, den Dialog selbst
  nie als Bild. In Schritt 4 wurde sie nicht wiederholt (kein schmutziger Zustand
  ohne Bedienung des Editors herzustellen).
* Windows und Linux, Nicht ASCII Pfade und eine zweite Instanz: ein Doppelklick bei
  laufendem Editor startet dort einen weiteren Prozess, es gibt keine Übergabe an die
  laufende Instanz (außerhalb dieses Themas).
* Der Anwenderhinweis zum einmaligen Registrieren steht im Handbuch (siehe unten),
  ein Hinweis in `getting-started.html` (Schritt „Unpack &amp; run“) wurde nicht
  ergänzt.

## Handbuch

Der Abschnitt „Opening a project from the file manager“ steht in der Website Quelle
`Website/HorizonEngineDocs/editor.html` unter `<section id="project-hub">`
(Website Commit `12805c4` auf main, gepusht, nicht deployt) und im Editor Handbuch
(`EditorDeps/Docs/he-docs.json`, Seite `editor`, Abschnitt `project-hub`). Er hat
Fließtext zum Weg in den Editor, eine Tabelle je Plattform (was einmal zu tun ist)
und einen Hinweis auf das Verhalten bei laufendem Editor.

Achtung beim Neubauen des Bundles: `scripts/build_docs_bundle.py` liest den
Website Ordner so, wie er gerade ausgecheckt ist. Auf Website main fehlen derzeit
Abschnitte, die in anderen Website Zweigen stehen und im Bundle von `release/0.7.0`
schon enthalten sind (`editor/github`, `materials/water`, `scripting-reference/sequence`,
`scripting-reference/settings`, `systems/audio-bus-eq`, `audio-curve`, `audio-editor`,
`audio-trim`). Ein voller Neubau würde sie aus dem Handbuch entfernen, und `--check`
meldet den Stand deshalb schon vor diesem Thema als veraltet. In Schritt 4 wurde
darum nur der Abschnitt `editor/project-hub` aus der Ausgabe des Generators in das
committete Bundle übernommen (vorher war dieser Abschnitt eine exakte Vorstufe des
neuen, nur die vier neuen Blöcke und der Suchtext kamen hinzu). Ein voller Neubau
gehört hinter das Zusammenführen der Website Zweige.
