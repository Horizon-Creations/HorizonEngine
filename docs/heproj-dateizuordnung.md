# Dateityp .heproj: Doppelklick öffnet den Editor (Thema 161)

Stand 2026-10-10, Schritt 5. Dieses Dokument sagt, was beim Doppelklick auf eine
`.heproj` je Plattform passiert, wie der Dateityp registriert wird (ab Schritt 5
macht das auf Windows und Linux der Editor selbst), wo die Teile im Repo liegen,
was geprüft wurde und was als Abnahme auf echter Hardware offen bleibt.

## Was beim Doppelklick passiert

Voraussetzung ist, dass das Betriebssystem `.heproj` dem Editor zuordnet. Der
Normalfall braucht dafür kein Zutun des Anwenders: macOS liest die Zuordnung aus
der Info.plist des Bundles (LaunchServices beim ersten Start), Windows und Linux
bekommen sie vom Editor selbst bei jedem Start (Abschnitt „Selbstregistrierung
beim Start“). Die Skripte im Paketordner `FileTypes/` sind der Rückfall und der
Weg, einen Handler zu übernehmen, den der Editor nicht anfasst.

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
| Windows | Registry unter HKCU (ProgID, Symbol, Befehl zum Öffnen), kein Administrator. Normalfall: der Editor schreibt sie bei jedem Start selbst, wenn sie fehlt oder abweicht. Rückfall: `HorizonEditor.exe --register-file-types` oder das Skript | `scripts/windows_assets/register_heproj.cmd`, `register_heproj.ps1`, `unregister_heproj.cmd`, `heproj.ico`, Prüfung `check_heproj_registration.ps1` |
| Linux | Desktopdatei, MIME XML und Symbole im eigenen XDG Datenordner. Normalfall: der Editor schreibt sie bei jedem Start selbst, wenn sie fehlen oder abweichen. Rückfall: `HorizonEditor --register-file-types` oder das Skript | `scripts/linux_assets/install_file_types.sh`, `horizon-editor.desktop`, `horizon-editor.xml`, `application-x-heproj.png`, `horizon-editor.png` |
| Editor (Windows und Linux) | Selbstregistrierung beim Start, Schalter `RegisterHeprojFileType`, Kommandozeile `--register-file-types` | `src/HE_Editor/HeprojRegistration.h` und `.cpp` (Entscheidung, Windows Registry, Linux Dateien, Kommandozeile), Aufruf in `EditorApplication.cpp` (`registerProjectFileTypeAtStartup`) und `main.cpp`, Schalter in `EditorSettingsPanel.cpp`, Hilfetext in `EditorHelp.cpp`, Tests `tests/test_heproj_registration.cpp` und `tests/test_heproj_file_types.py` |

Windows und Linux haben keinen Installer (ZIP und tar.gz aus `package/Editor`).
Deshalb reist die Registrierung als Ordner `FileTypes/` im Paket mit, kopiert per
POST_BUILD in `src/HE_Editor/CMakeLists.txt` (nicht auf macOS). Typname,
ProgID und MIME bleiben auf allen drei Plattformen gleich:
`dev.horizoncreations.heproj` bzw. `application/x-heproj`. Den Ordner braucht auch
der Editor: aus ihm nimmt er das Symbol (Windows) bzw. die Vorlage der Desktopdatei,
das MIME XML und die beiden Symbole (Linux). Ohne `FileTypes/` neben dem Editor,
etwa in einem Build Ordner, registriert er nichts.

## Selbstregistrierung beim Start

Bis Schritt 4 musste man nach dem Entpacken einmal `FileTypes\register_heproj.cmd`
(Windows) bzw. `FileTypes/install_file_types.sh` (Linux) ausführen. Seit Schritt 5
macht der Editor das bei jedem Start selbst, pro Benutzer, ohne Administratorrechte
und wiederholbar. Die Skripte bleiben als Rückfall. macOS tut nichts: die Info.plist
des Bundles genügt, LaunchServices registriert sie beim ersten Start.

**Wann.** `HeprojRegistration::startAtStartup` läuft am Ende von
`EditorApplication::OnInit`, nach dem Laden der Einstellungen und vor dem ersten
Frame. Die Prüfung ist billig (unter Windows ein paar Registry Werte, unter Linux ein
paar kleine Dateien). Nur wenn etwas fehlt oder abweicht, schreibt ein kurzer
Hintergrundthread. Der Start wartet nie darauf, `OnShutdown` wartet auf den Thread
(`HeprojRegistration::shutdown`, begrenzt durch die Zeitlimits der Hilfsprogramme).
Nicht aufgerufen wird es in Läufen ohne Bedienung (gesetzter Dump Pfad oder
verstecktes Fenster): Tests und Screenshots sollen die Zuordnung dessen, der sie
startet, nicht umbiegen.

**Was geschrieben wird, Windows.** Dieselben Werte wie `register_heproj.ps1`, alle
unter `HKCU\Software\Classes` (`windowsRegistryValues`): `.heproj` (Standardwert =
ProgID `dev.horizoncreations.heproj`, `Content Type` = `application/x-heproj`),
`.heproj\OpenWithProgids` (die ProgID als Wert ohne Daten), die ProgID selbst
(Typname „Horizon Engine Project“), `DefaultIcon` (`FileTypes\heproj.ico`) und
`shell\open\command` (`"<exe>" "%1"`). Danach `SHChangeNotify(SHCNE_ASSOCCHANGED)`,
damit der Explorer neu zeichnet. Als aktuell gilt die Registrierung, wenn die drei
Werte stimmen, die über das Ziel des Doppelklicks entscheiden (Standardwert von
`.heproj`, `DefaultIcon`, `shell\open\command`, ohne Unterschied zwischen Groß und
Kleinschreibung). Content Type, „Open with“ Eintrag und Typname werden mitgeschrieben,
aber nicht nachgeprüft, damit eine funktionierende Registrierung nicht wegen
Kosmetik umgeschrieben wird. Voraussetzung ist `FileTypes\heproj.ico` neben der Exe.

**Was geschrieben wird, Linux.** Dieselben Dateien wie `install_file_types.sh` in
`$XDG_DATA_HOME` (Standard `~/.local/share`, ein relativer Wert wird wie im Skript
ignoriert): `applications/horizon-editor.desktop` (die Vorlage aus `FileTypes/`, nur
`Exec=` und `TryExec=` zeigen auf die laufende Binary aus `/proc/self/exe`, ein `%`
im Pfad wird verdoppelt, ein Pfad mit `"`, `` ` ``, `$` oder `\` wird abgelehnt und
ergibt `failed`), `mime/packages/horizon-editor.xml` und zwei PNG unter
`icons/hicolor/256x256/` (`mimetypes/application-x-heproj.png`,
`apps/horizon-editor.png`). Jede Datei geht über eine Temp Datei und Umbenennen. MIME
XML und Symbole werden nur geschrieben, wenn ihr Inhalt abweicht. Danach, soweit
vorhanden: `update-mime-database`, `update-desktop-database`, `gtk-update-icon-cache`
und `xdg-mime default horizon-editor.desktop application/x-heproj` (je 8 Sekunden
Zeitlimit; ein fehlendes oder scheiterndes Programm steht nur im Log und macht die
Registrierung nicht ungültig). Als aktuell gilt sie, wenn die Desktopdatei
byteweise das ist, was jetzt geschrieben würde, und MIME XML und beide Symbole
byteweise den Vorlagen gleichen. Voraussetzung sind die vier Dateien in `FileTypes/`
und ein auffindbarer Datenordner.

**Entscheidung.** `HeprojRegistration::decide(Schalter, Dateien da, Zustand)` prüft in
dieser Reihenfolge: Schalter, fehlende Dateien, Zustand.

| Lage beim Start | Ergebnis | Was der Editor tut | Log und Kommandozeile |
|---|---|---|---|
| Nicht registriert (keine Zuordnung, auch keine fremde) | `Registered` | schreibt alles, auf dem Hintergrundthread | `registered`, Exit 0 |
| Schon registriert, auf diesen Pfad | `AlreadyRegistered` | schreibt nichts, auch nicht die Zeitstempel | `already-registered`, Exit 0 |
| Eigene Registrierung, aber anderer Pfad oder unvollständig (Editor verschoben oder aktualisiert, Schlüssel halb weg) | `Updated` | schreibt sie auf den neuen Pfad um | `updated`, Exit 0 |
| Fremder Handler (siehe unten) | `LeftAlone` | schreibt nichts, eine Logzeile, eine Benachrichtigung (Info) je Programm | `left-alone`, Exit 3 |
| Schalter aus | `Disabled` | schaut nicht einmal nach, schreibt nichts | Logzeile „switched off in Preferences“, die Kommandozeile ignoriert den Schalter |
| `FileTypes/` fehlt neben dem Editor (Build Ordner statt Paket; unter Linux auch: kein Datenordner auffindbar) | `MissingFiles` | schaut nicht nach, schreibt nichts | Logzeile „no FileTypes folder next to the editor“, Exit 4 (`missing-files`) |
| Schreiben scheitert | `Failed` | Warnung im Log, kein Error, der Start verläuft normal | `failed`, Exit 1 |
| macOS (kein Backend) | `Unsupported` | nichts | Logzeile „nothing to do here“, Exit 5 (`unsupported`) |
| Lauf ohne Bedienung (Dump Pfad, verstecktes Fenster) | kein Aufruf | nichts | keine Logzeile |

**Fremder Handler.** Der Editor nimmt `.heproj` keinem anderen Programm weg.

* Windows: Die Wahl des Anwenders im Explorer (Öffnen mit, Immer; Schlüssel
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.heproj\UserChoice`,
  Wert `ProgId`) nennt eine andere ProgID als die eigene oder `Applications\<Exe>`,
  oder die zusammengeführte Sicht `HKCR\.heproj` nennt eine andere ProgID (auch eine
  Zuordnung für alle Benutzer). Die Wahl dieses Editors zählt nicht als fremd.
* Linux: `mimeapps.list` (zuerst in `$XDG_CONFIG_HOME`, Standard `~/.config`, dann in
  `$XDG_DATA_HOME/applications` wie ältere `xdg-mime`) nennt für `application/x-heproj`
  als ersten Eintrag unter `[Default Applications]` eine andere Desktopdatei als
  `horizon-editor.desktop`. Andere Typen in der Datei zählen nicht.

Dann schreibt der Editor nichts, die Logzeile nennt das Programm, und eine
Benachrichtigung (Info) sagt, wie man umschaltet: „Öffnen mit“ im Dateimanager, oder
das Skript aus `FileTypes/`, das überschreibt. Welches Programm gemeldet wurde, merkt
sich der Editor in der `config.json` (Schlüssel `HeprojForeignHandlerNoticed`). Die
Meldung kommt also einmal je Programm und nicht bei jedem Start. Unter Windows
übersteuert eine Wahl über „Öffnen mit, Immer“ jede Registrierung, auch die des
Skripts. Das Skript sagt es selbst am Ende seiner Ausgabe (Öffnen mit, Andere App
auswählen).

**Schalter.** `Edit > Preferences > Editor > Tool Status`, am Seitenende der Abschnitt
„File types“ mit der Checkbox „Register .heproj with this editor“ (nicht auf macOS).
Standard an, gespeichert in der `config.json` unter `RegisterHeprojFileType`. Aus: der
Editor fasst die Zuordnung nie an, auch nicht, um sie zu entfernen. Beim Einschalten
läuft die Registrierung sofort, ohne Neustart (bei fremdem Handler mit derselben
Benachrichtigung). Entfernt man die Zuordnung mit `unregister_heproj.cmd` oder
`install_file_types.sh --uninstall`, legt der nächste Start sie wieder an, solange der
Schalter an ist. Wer sie loswerden will, schaltet zuerst den Schalter aus.

**Log.** Je Start eine Zeile mit dem Präfix `HeprojRegistration:` (Info), bei einem
Fehler eine Warnung, nie ein Error (ein Error ginge an die Glocke der
Benachrichtigungen). Wird geschrieben, kommt die Zeile vom Hintergrundthread, sonst
sofort.

**Kommandozeile.** `HorizonEditor --register-file-types` (`HorizonEditor.exe` unter
Windows) registriert sofort, ohne Fenster, Logdatei und Konfiguration: `main()` ruft
`HeprojRegistration::runCommandLine` noch vor dem Crash Handler. Der Schalter
spielt dabei keine Rolle, es ist ja eine Anforderung. Ein fremder Handler bleibt wie
beim Start unberührt. Ausgabe ist eine Zeile `<Schlüsselwort>: <Satz>` auf stdout. Exit
Code: 0 für `registered`, `updated` und `already-registered`, 1 für `failed`, 3 für
`left-alone`, 4 für `missing-files`, 5 für `unsupported` (macOS). Das führt die CI
gegen das Paket aus, und es ist ein Einzeiler als Alternative zu den Skripten.

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
Das ist der Stand von Schritt 3 und 4: geprüft wurden die Skripte. Was Schritt 5 daran
ändert, steht im Unterabschnitt „Schritt 5“ weiter unten.

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

### Schritt 5, Selbstregistrierung

Neu ist der Editor selbst als Registrierer. Die Tabelle trennt nach dem Ort der
Prüfung: was sich auf dem Mac belegen lässt, was nur die CI belegt (echte Registry,
gepacktes Programm) und was echte Hardware braucht. Jede Zeile gehört in genau eine
Spalte. Die Läufe stehen unter der Tabelle.

| Belegt auf dem Mac | Nur durch die CI belegt | Offen, braucht Hardware |
|---|---|---|
| **Entscheidung.** `test_heproj_registration` in `he_tests`, mit einem Fake Backend: Schalter aus schaut nicht einmal nach, fehlende Dateien schlagen den Zustand, nicht registriert wird einmal geschrieben, schon registriert gar nicht, anderer Pfad wird umgeschrieben, fremder Handler bleibt unberührt und wird benannt, ein scheiterndes Schreiben meldet seinen Grund | | |
| **Vertrag der Kommandozeile.** Schlüsselwörter, Exit Codes (0, 1, 3, 4, 5) und Sätze; ohne das Argument tut `runCommandLine` nichts | | |
| **Windows gegen eine Map als Fake Registry.** Die Werte sind genau die von `register_heproj.ps1`; frisch wird einmal geschrieben; ein verschobener Editor wird umgeschrieben, ein falscher Symbolpfad repariert, der Content Type nicht nachgeprüft; fremd sind eine Registrierung eines anderen Programms, eine Zuordnung über HKCR und die Wahl im Explorer (UserChoice), nicht fremd ist die Wahl dieses Editors; ohne Symbol neben dem Editor keine Registrierung; ein scheiternder Schreibzugriff nennt den Schlüssel | | |
| **Linux gegen einen temporären XDG Ordner** (die Hilfsprogramme sind aus, nichts erreicht die echte Desktopdatenbank): ein frischer Ordner bekommt die Dateien des Skripts; was stimmt, wird nicht angefasst, auch nicht die Zeitstempel; ein verschobener Editor wird umgezeigt und zurück; fehlende oder geänderte Dateien werden zurückgeschrieben; ein fremder Standard in `mimeapps.list` lässt alles in Ruhe und legt nichts an; `%` im Pfad wird verdoppelt, ein Pfad, den eine Exec Zeile nicht tragen kann, abgelehnt; ohne `FileTypes/` oder Datenordner passiert nichts; `resolveXdgHome` und `defaultHandlerIn` | | |
| **Namen gleich.** `tests/test_heproj_file_types.py` liest den Quelltext der Selbstregistrierung und hält ProgID, MIME und Typname gleich zu Skripten, UTI und MIME XML | | |
| | **Windows Runner.** `check_heproj_registration.ps1` führt jetzt in einer dritten Runde `HorizonEditor.exe --register-file-types` aus dem Paket gegen die echte HKCU aus (vorher gesichert, danach wiederhergestellt): aus dem Nichts, noch einmal (eine aktuelle Registrierung wird nicht umgeschrieben, auch kein kosmetischer Wert), nach dem Verschieben des Ordners, nach verschwundenem Symbol und mit einer `.heproj` Zuordnung eines anderen Programms (Exit Code 3, nichts geschrieben). Das Urteil sind Exit Code und Registry, die Ausgabezeile nur, wenn sie ankommt. Die Skriptrunden 1 und 2 laufen weiter. Hat das Konto des Runners schon eine fremde Zuordnung, sagt die Runde das und hört auf | |
| | **Linux Runner.** `tests/test_heproj_file_types.py --package` führt die gepackte Binary als `HorizonEditor --register-file-types` gegen ein Wegwerf HOME und Wegwerf XDG Ordner aus (Klasse `LinuxSelfRegistration`): frisch kommen die vier Dateien und die Zeile `registered`; die Desktopdatei besteht `desktop-file-validate`; die MIME Datenbank bildet die Endung ab; die Dateien sind byteweise die des Skripts; die Desktopumgebung löst ein Projekt auf und startet den Editor damit; der zweite Lauf sagt `already-registered` und ändert nichts; ein Paketordner an anderer Stelle wird auf den neuen Pfad aktualisiert; fehlende oder veraltete Dateien werden repariert; ein fremder Standard bleibt unberührt; die Rückfälle für `XDG_DATA_HOME` und `XDG_CONFIG_HOME` | |
| | | **Der echte Doppelklick nach der Selbstregistrierung.** Frisch entpacktes Paket, ein Start, dann Doppelklick im Windows Explorer und in einem Linux Dateimanager; im Finder mit einem installierten Release DMG |
| | | **Der Aufruf beim Start in einer echten GUI Sitzung** unter Windows und Linux. Entscheidung, Schreiben und Kommandozeile sind mit Fakes, Wegwerf Ordnern und dem Paket geprüft, die Verdrahtung in `OnInit` samt Logzeile, Benachrichtigung und Start ohne Verzögerung nicht |
| | | **Explorer UserChoice.** Wie sich eine echte Wahl über „Öffnen mit, Immer“ zur Registrierung verhält, ob die Benachrichtigung dann erscheint und ob das Skript eine solche Wahl überhaupt ersetzen kann |
| | | **Der Schalter im Fenster.** Die Checkbox in `Edit > Preferences > Editor > Tool Status` wurde nicht als Bild gesehen |

TODO-LEAD: Testergebnisse Schritt 5 eintragen

Dorthin gehören: `he_tests` (Fälle und Prüfungen von `test_heproj_registration` und der Nachbarn), `ctest` für `heproj_file_types`, der CI Lauf je Windows und Linux, der Commit.

## Offen, braucht Hardware

* Ein echter Doppelklick im Finder mit einem installierten Release DMG dieses
  Zweigs, im Windows Explorer und in einem Linux Dateimanager. Gesehen wurde bisher
  nur der Weg über `open`, die Registry und gio, nicht der Klick. Für Windows und
  Linux zählt der Weg, den der Anwender geht: Paket entpacken, den Editor einmal
  starten (er registriert sich selbst), dann doppelklicken. Auch das ist nicht gesehen.
* Der Aufruf der Selbstregistrierung beim Start in einer echten GUI Sitzung unter
  Windows und Linux (Logzeile, Zuordnung nach dem ersten Start, kein spürbarer Aufschub
  des Starts). Geprüft sind die Entscheidung, das Schreiben und die Kommandozeile,
  nicht die Verdrahtung in `OnInit`.
* Windows, Explorer UserChoice: eine Wahl über „Öffnen mit, Immer“ übersteuert jede
  Registrierung. Der Editor erkennt sie und lässt sie in Ruhe, geprüft nur gegen eine
  Registry als Map. Nicht gesehen: das echte Verhalten des Explorers, die
  Benachrichtigung dazu, und ob das Skript eine solche Wahl überhaupt ersetzen kann
  (es verweist selbst auf „Öffnen mit“).
* Windows, die Ausgabezeile von `--register-file-types` an einer interaktiven Konsole:
  `HorizonEditor.exe` ist ein Programm mit Windows Subsystem (`WIN32` in
  `add_executable`) und hängt sich an keine Konsole an. Ob Zeile und Exit Code dort
  ohne Umleitung zu sehen sind, wurde nicht geprüft. Die CI wertet Exit Code und
  Registry aus und die Zeile nur, wenn sie ankommt.
* Die Symbolanzeige des Dateityps im Finder, im Explorer und im Linux Dateimanager.
* Die Rückfrage bei ungespeicherten Änderungen mit sichtbarem Dialog: Schritt 1 sah
  auf macOS die Popup Kennung im Log und den ausbleibenden Wechsel, den Dialog selbst
  nie als Bild. In Schritt 4 wurde sie nicht wiederholt (kein schmutziger Zustand
  ohne Bedienung des Editors herzustellen).
* Windows und Linux, Nicht ASCII Pfade und eine zweite Instanz: ein Doppelklick bei
  laufendem Editor startet dort einen weiteren Prozess, es gibt keine Übergabe an die
  laufende Instanz (außerhalb dieses Themas).
* Seit Schritt 5 muss der Anwender nichts mehr einmal ausführen. Das Handbuch
  beschreibt die Selbstregistrierung und den Rückfall (siehe unten). Ein Hinweis in
  `getting-started.html` (Schritt „Unpack &amp; run“) wurde nicht ergänzt und ist für
  den Normalfall auch nicht nötig. Ebenso fehlt der Schalter in der Aufzählung des
  Abschnitts „Preferences“ der Seite `editor` (anderer Abschnitt, nicht Teil dieses
  Schritts).

## Handbuch

Der Abschnitt „Opening a project from the file manager“ steht in der Website Quelle
`Website/HorizonEngineDocs/editor.html` unter `<section id="project-hub">`
(Website Commit `12805c4` auf main, gepusht, nicht deployt; Schritt 5 darauf: Website
Commit `49b8e61`, lokal auf main, nicht gepusht, nicht deployt) und im Editor Handbuch
(`EditorDeps/Docs/he-docs.json`, Seite `editor`, Abschnitt `project-hub`). Er hat
Fließtext zum Weg in den Editor, eine Tabelle je Plattform (wie der Dateityp
registriert wird und was man tut: Normalfall automatisch, `--register-file-types` und
die Skripte als Rückfall), einen Hinweis auf das Verhalten bei laufendem Editor und
seit Schritt 5 den Unterabschnitt „Registering the file type“ (wann der Editor
registriert, fremder Handler, Schalter, Logzeile, Exit Codes der Kommandozeile).

Achtung beim Neubauen des Bundles: `scripts/build_docs_bundle.py` liest den
Website Ordner so, wie er gerade ausgecheckt ist. Auf Website main fehlen derzeit
Abschnitte, die in anderen Website Zweigen stehen und im Bundle von `release/0.7.0`
schon enthalten sind (`editor/github`, `materials/water`, `scripting-reference/sequence`,
`scripting-reference/settings`, `systems/audio-bus-eq`, `audio-curve`, `audio-editor`,
`audio-trim`). Ein voller Neubau würde sie aus dem Handbuch entfernen, und `--check`
meldet den Stand deshalb schon vor diesem Thema als veraltet. In Schritt 4 wurde
darum nur der Abschnitt `editor/project-hub` aus der Ausgabe des Generators in das
committete Bundle übernommen (vorher war dieser Abschnitt eine exakte Vorstufe des
neuen, nur die vier neuen Blöcke und der Suchtext kamen hinzu). Schritt 5 ging
genauso vor: Der Generator schrieb in einen Wegwerf Ordner, der Abschnitt
`editor/project-hub` wurde von dort in das committete Bundle übernommen und danach
gegen die Generator Ausgabe geprüft (gleich). Kein anderer Abschnitt und kein Kopffeld
änderte sich, `generated` bleibt auf 2026-10-06. Der Suchtext des Abschnitts ist sein
Feld `text` im Bundle und kommt aus dem Generator. Der Suchindex der Website
(`docs-index.json`) wurde mit `build_docs_index.py` neu gebaut, geändert hat sich nur
der Eintrag `editor.html#project-hub`. Ein voller Neubau gehört hinter das
Zusammenführen der Website Zweige.
