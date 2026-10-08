# CI: Windows-Job der Themen-Zweige auf NN-WS03

Stand 08.10.2026. Der Windows-Job braucht auf GitHubs Runner rund 50 Minuten (46 davon
Build, vier Kerne, jedes Mal von null), NN-WS03 baut die Engine in etwa fünf. Deshalb läuft
der Windows-Job von **Themen-Zweigen** (Pull Requests und `gh workflow run CI --ref …`) auf
NN-WS03, solange der Hive den Rechner frei meldet.

## Wie es zusammenspielt

- **GitHub-Runner auf NN-WS03** (self-hosted, Label `nn-ws03`): ein Dienst von GitHub, der
  sich Jobs selbst abholt. Nur ausgehende Verbindungen, nichts wird von außen geöffnet.
- **Der Hive entscheidet** (`claude_hive_mcp/ci.py`, `route_windows`): Er setzt die
  Repo-Variable `WINDOWS_RUNNER` auf `nn-ws03`, solange
  - der Runner bei GitHub online ist,
  - NN-WS03 im Hive da ist,
  - dort keine Bienen arbeiten,
  - und die CPU nicht über 60 % liegt (der eigene Build des Runners zählt nicht).

  Sonst setzt er sie auf `github`. Geschrieben wird nur bei einem Wechsel, jeder Wechsel
  steht als CI-Ereignis im Verlauf.
- **`ci.yml` liest die Variable** und schickt den Windows-Job dann an `["self-hosted", "nn-ws03"]`.
  Nie dorthin:
  - Pushes auf `main` und `release/**`: Daraus kommen die Download-Pakete, die entstehen auf
    GitHubs sauberen Maschinen.
  - Pull Requests aus Forks: Das Repo ist öffentlich, fremder Code läuft nicht auf einem PC zu Hause.
- **Auf NN-WS03 bleibt der Build-Ordner liegen:** checkout ohne `clean`, aufgeräumt wird alles
  außer `build/` und `package/`. Ninja übersetzt nur, was sich geändert hat, mit allen Kernen.
  Es gibt dort keinen Actions-Cache und kein Paket-Upload, und die Tests bleiben auf Loopback
  (keine Firewall-Dialoge).
- **Runner weg:** Warten Jobs auf ihn, wenn er offline geht, bricht der Hive diese Läufe ab
  und startet sie neu, dann bei GitHub.

## Einrichten (einmal, auf NN-WS03)

### 1. Voraussetzungen

Meist schon da:

- Visual Studio 2022 oder die Build Tools mit „Desktopentwicklung mit C++“
  (inkl. „C++-CMake-Tools für Windows“).
- Git für Windows.
- Python 3.
- **Vulkan SDK** (wie in CI 1.4.313.0). Der Installer setzt `VULKAN_SDK` systemweit; der Job
  prüft das und bricht sonst mit einer klaren Meldung ab.
- **PowerShell 7** (`pwsh`), die Schritte laufen damit: `winget install Microsoft.PowerShell`.

### 2. Runner anlegen

1. Auf GitHub: Repo → **Settings → Actions → Runners → New self-hosted runner** → Windows, x64.
2. GitHub zeigt Befehle mit einem Einmal-Token. In einer PowerShell als Administrator ausführen,
   den Ordner aber **kurz** wählen, z. B. `C:\ar` statt `C:\actions-runner`. Die Pfade unter
   `_work\HorizonEngine\HorizonEngine\build\_deps\…` werden sonst schnell zu lang für Windows.
3. Bei `config.cmd`:
   - Runner-Gruppe: Standard.
   - Name: `NN-WS03`. Der Hive findet das Gerät über den Namen.
   - **Zusätzliche Labels: `nn-ws03`**. Ohne dieses Label landet kein Job dort.
   - Arbeitsordner: Standard (`_work`).
   - **Als Dienst ausführen: ja**. Das Standard-Konto (Netzwerkdienst) reicht und hat keine
     Admin-Rechte.
4. Dienst neu starten, falls `VULKAN_SDK` erst danach gesetzt wurde: Der Dienst liest die
   Umgebung nur beim Start.

### 3. Absicherung in GitHub

Repo → **Settings → Actions → General → „Fork pull request workflows from outside
collaborators“ → „Require approval for all outside collaborators“**. `ci.yml` schickt
Fork-PRs ohnehin nie auf NN-WS03; das hier ist die zweite Sicherung.

### 4. Fertig

Innerhalb von ein, zwei Minuten sieht der Hive (auf dem Rechner mit `gh`, also dem Mac) den
Runner online und NN-WS03 frei und setzt `WINDOWS_RUNNER=nn-ws03`. Der nächste PR-Lauf baut
Windows dann dort; im Lauf steht beim Windows-Job der Runner-Name `NN-WS03`.

## Was es nicht kann

- **Ein Lauf nach dem anderen:** Der Runner baut immer nur einen Job. Mehrere Themen-Läufe
  warten aufeinander, bei etwa fünf Minuten pro Lauf ist das immer noch schneller als GitHub.
- **Bienen auf NN-WS03 haben Vorrang:** Solange dort welche arbeiten, gehen *neue* Jobs zu
  GitHub. Ein Job, der schon an NN-WS03 vergeben ist, läuft trotzdem dort.
- **Zweige mit altem Stand:** Der Workflow kommt bei `workflow_dispatch` aus dem Zweig selbst.
  Ein Themen-Zweig ohne diese `ci.yml` baut weiter bei GitHub, bis er `main` bzw. seinen
  Release-Zweig hereinholt. Pull Requests nehmen die `ci.yml` des Merge-Stands.
