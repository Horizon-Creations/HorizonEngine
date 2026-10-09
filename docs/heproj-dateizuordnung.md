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
Projekt und das Log nennt den Dialog. Ein Pfad, der nicht existiert, ein Ordner
oder eine andere Endung wird im Log gemeldet. Wurde ein `.heproj` beim Start
genannt und lässt sich nicht öffnen, zeigt der Editor den Hub mit dem Grund statt
des letzten Projekts.

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
Hub gegen geführten Wechsel gegen schon offen, Ein Platz Speicher) und die
Nachbarn (Asset und Szenen Autosave, Dock Zustand, Benachrichtigungen, Report Issue,
Projekt Einstellungen, Projekt Export): 115 Fälle, 860 Prüfungen, alle grün. Voller
ctest ohne `test_material_graph`: 235 von 237 grün, `test_culling` und
`test_widget_designer_ui` liefen bei 240 s ins Zeitlimit (Debug, Last 11 auf dem
Rechner) und sind einzeln grün (309 s und 125 s). Die drei `runtime_size` Fälle
werden übersprungen, sie brauchen einen Release Build.

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
* Das Handbuch im Editor: siehe unten.

## Handbuchtext (noch nicht in die Website übernommen)

Die Doku im Editor (`EditorDeps/Docs/he-docs.json`) wird von
`scripts/build_docs_bundle.py` aus der Website gebaut (`Website/HorizonEngineDocs/*.html`,
Geschwisterordner). Der Abschnitt gehört in `editor.html` hinter den Hinweis am
Ende von `<section id="project-hub">`, davor `</section>`. Danach im Website Ordner
`python3 build_docs_index.py`, hier `python3 scripts/build_docs_bundle.py`, beides
committen. Der Schreibzugriff auf `editor.html` wurde in Schritt 4 vom Auto Mode
Klassifikator abgelehnt, deshalb liegt der Text hier.

```html
          <h3>Opening a project from the file manager</h3>
          <p>
            A project file (<code>.heproj</code>) opens in the editor with a
            double click in Finder, Explorer or your Linux file manager. The
            editor takes it the same way the Hub's own Open does: if another
            project is open and has unsaved changes, it asks first, and if that
            very project is already open, nothing happens. The path also works
            on the command line, as in
            <code>HorizonEditor MyGame.heproj</code>. A path that does not
            exist, is a folder or does not end in <code>.heproj</code> is
            reported in the log and the editor shows the Project Hub instead.
          </p>
          <div class="docs-table-wrap">
            <table class="docs-table">
              <thead>
                <tr><th>Platform</th><th>How the file type is registered</th><th>What you do</th></tr>
              </thead>
              <tbody>
                <tr><td><strong>macOS</strong></td>
                    <td>The app's <code>Info.plist</code> declares the type, so
                        the <code>.dmg</code> already carries it.</td>
                    <td>Drag the app into <code>Applications</code> and start it
                        once. Until the first start macOS does not trust the new
                        type, and a double click may open another program.</td></tr>
                <tr><td><strong>Windows</strong></td>
                    <td>Registry entries for the current user only, so no
                        administrator is needed.</td>
                    <td>After unpacking the <code>.zip</code>, run
                        <code>FileTypes\register_heproj.cmd</code> once. Run it
                        again if you move the folder, because the registry
                        holds the full path.
                        <code>FileTypes\unregister_heproj.cmd</code> removes it.</td></tr>
                <tr><td><strong>Linux</strong></td>
                    <td>A desktop entry and a MIME type in your own data
                        directory (<code>~/.local/share</code>), so no root is
                        needed.</td>
                    <td>After unpacking the archive, run
                        <code>FileTypes/install_file_types.sh</code> once, and
                        again after moving the folder.
                        <code>--uninstall</code> takes it away.</td></tr>
              </tbody>
            </table>
          </div>
          <div class="callout note">
            <span class="callout-icon">◆</span>
            <p>
              <strong>Note:</strong> on macOS an editor that is already
              running receives the file as an open document event and switches
              to it after the unsaved changes question. Windows and Linux start
              a new editor for every double click and do not pass the file to
              one that is already running. To change the project inside a
              running editor, open it from there.
            </p>
          </div>
```

Optional, ein Satz in `getting-started.html`, Schritt „Unpack &amp; run“, hinter
`<code>Applications</code>.`: „To open `.heproj` files with a double click, register
the file type once; the Project Hub guide (`editor.html#project-hub`) shows how for
each platform.“
