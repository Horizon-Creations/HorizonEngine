# Editor-Grundausstattung: Bestandsaufnahme (Stand 24.09.2026)

Thema 82, Schritt 1. Geprüft wurden die vier Punkte, die das Projekt-Logbuch (Stand 25.08.) und
`gap-audit-2026-08-25.md` noch als offen führten: Mehrfachauswahl, Lichter und Kameras im Viewport,
Autosave, sRGB. Grundlage sind `git log`/`git merge-base` auf HEAD `7d49d44f` und das Lesen des
Codes. **Es gab keinen Editor- oder Build-Lauf.** Alles unten ist Commit- und Codelektüre, die mit
„plausibel" markierten Funde sind nicht zur Laufzeit belegt.

Kurzfassung: **Keiner der vier Punkte fehlt noch.** Alle vier sind zwischen dem 14. und 16.09.
gemergt worden, das Logbuch und die Liste „Weiterhin offen" im Lückenaudit waren veraltet. Offen
sind nur Restposten, und der schwerste davon liegt beim sRGB-Flag.

Auf allen vier Feature-Zweigen liegt kein unverschmolzener Commit mehr (`git rev-list --count
HEAD..origin/claude/<zweig>` = 0 für `mehrfachauswahl-im-editor`,
`lichter-und-kameras-im-viewport-sichtbar-und-anklickbar`,
`solo-autosave-unabhaengig-von-collaboration-session` und
`srgb-flag-durch-die-textur-pipeline-durchziehen`).

| Punkt | Urteil | Merge auf HEAD |
|---|---|---|
| Mehrfachauswahl | **da** | `f7747f7f` (Thema 31) |
| Lichter/Kameras im Viewport | **da**, Restposten | `809e46e0` (Thema 32) |
| Autosave | **da** (Szene), Asset-Tabs nicht | `764299ce` (Thema 46) |
| sRGB | **teilweise**: Pipeline fertig, aber nur der glTF-Import setzt das Flag | `1bba4e22`, `95f1069d` (Thema 43) |

---

## 1. Mehrfachauswahl: da

Commits `8e02d5d5`, `b702a990`, `d3708054`, `5ce56ed1`.

Vorhanden:
- `EditorSelection` (`src/HE_Editor/EditorSelection.{h,cpp}`) ist eine geordnete Menge mit
  primary und anchor und ersetzt das alte `m_selectedEntity`. Tests: `tests/test_editor_selection.cpp`.
- Outliner: Klick, Strg/Cmd-Klick, Umschalt-Bereich. Viewport: Strg/Cmd-Klick schaltet um.
- Rahmenziehen im Viewport (`EditorMarquee`, `ViewportPanel.cpp:1416ff`). Lichter, Kameras und
  Audioquellen werden über ihre Symbol-Quads mitgerahmt (Kommentar `ViewportPanel.cpp:1417-1422`;
  der ältere Commit-Text `b702a990` sagt noch „nie gerahmt", der Code gilt).
- Gruppen-Gizmo mit Zentroid-Pivot, ein Undo-Schritt pro Zug.
- Inspector: gemeinsame Komponenten, Edits gehen an alle Mitglieder (`EditorMultiEdit`), „Remove
  Component" für alle. Duplizieren, Kopieren, Ausschneiden, Einfügen und Löschen arbeiten auf der
  ganzen Auswahl.

Restposten:
- Kein „Select All" (Ctrl+A) für Entities: `EditorShortcuts.cpp` registriert keinen solchen
  Eintrag, und kein Menü bietet ihn an.
- Kein Esc zum Abwählen: `ImGuiKey_Escape` kommt in `ViewportPanel.cpp`, `OutlinerPanel.cpp` und
  `EditorApplication.cpp` nicht vor.
- Der Multi-Inspector zeigt die Werte des aktiven Entities und keine „gemischte Werte"-Anzeige,
  wenn die Mitglieder sich unterscheiden.
- Im Multi-Inspector gibt es kein „Add Component" für alle Mitglieder, nur „Remove".
- In einer Collab-Sitzung nimmt nur das gehaltene (aktive) Entity Edits an, weil es kein
  Mehrfach-Lock-Modell gibt (`InspectorPanel.cpp:110-112`).

## 2. Lichter, Kameras, Audioquellen im Viewport: da, mit Restposten

Commits `0e01d57b`, `0a225797`, `bbb9d29b`. Später dazugekommen ist der Show-Schalter
`editorIcons` (`ViewportPanel.h:131`, Commit `6fc3e276`).

Vorhanden:
- Der `RenderExtractor` schiebt unter aktiver Editor-Kamera ein kamerazugewandtes Symbol-Quad mit
  konstanter Bildschirmgröße in `out.objects` (`RenderExtractor.cpp:756ff`, `extractEditorIcons`).
  Fünf Symbole: Punkt-, Spot- und Richtungslicht, Kamera, Audio. Im Spiel und im gepackten Build
  gibt es keine.
- Anklickbar über `ViewportPick`, mitgerahmt beim Rahmenziehen, F rahmt das Entity und nicht das
  Quad. Sonne und Mond (`EnvironmentLightComponent`) bleiben bewusst ohne Symbol.
- Tests: `tests/test_editor_icons.cpp`, `test_viewport_pick`.

Restposten:
- Im Commit `0e01d57b` selbst als offen genannt, seitdem ohne Folge-Commit: Gizmo auf dem Symbol,
  Symbole nach dem Tonemapping zeichnen (auf hellem Boden verwäscht der dunkle Rand), Lichtsymbol in
  der Lichtfarbe.
- Es gibt keine Visualisierung von Punktlicht-Radius, Spot-Kegel oder dem Frustum der gewählten
  Szenenkamera. Frustums zeichnet nur `McpCameraGizmos` für die Kameras der MCP-Clients.
- **Plausibel, nicht zur Laufzeit belegt:** Die gelbe Auswahlbox (`EditorApplication.cpp:3583-3593`)
  sitzt auf `tc->position`, der LOKALEN Position, das Symbol dagegen auf `t.worldMatrix`
  (`RenderExtractor.cpp:806-816`). Bei einem Licht unter einem verschobenen Eltern-Entity liegen
  Symbol und Auswahlbox auseinander. Die Collider-Drahtgitter direkt darunter lesen ebenfalls
  `transform.position`. Der richtige Leser wäre `HE::worldPositionOf`.

## 3. Autosave: da (Szene)

Commits `30b30107`, `db9093c5`, `d6dada30`, `e08827e4`; Beschreibung im Lückenaudit 3.8.

Vorhanden:
- `SceneAutosave` (`src/HE_Editor/SceneAutosave.{h,cpp}`) schreibt ohne Collab-Session eine
  Wiederherstellungskopie nach `<Projekt>/Saved/Autosave/`, atomar, nur bei dirty Szene
  (`EditorApplication::updateAutosave`, `EditorApplication.cpp:9867`). Preferences ▸ General ▸
  Autosave (an/aus, 10–3600 s, Standard 60 s).
- Crash-Wiederherstellung beim nächsten Start: `SceneRecoveryDialog` mit Restore, Delete Snapshot
  und Keep for Later.
- Tests: `tests/test_scene_autosave.cpp`, `tests/test_scene_recovery_ui.cpp`.

Restposten:
- Dirty Asset-Tabs sind nicht abgedeckt: Script, C++-Klasse, Material, UI-Designer, HorizonCode,
  Input, Type, Theme und die übrigen Panels (`EditorUI.cpp:765ff`) haben keine Wiederherstellungskopie.
  Ein Absturz verliert dort alles seit dem letzten Speichern.
- Ein einziger Snapshot-Slot pro Projekt, keine Rotation älterer Stände.
- Plausibel: `updateAutosave` serialisiert die ganze Welt als JSON synchron auf dem Hauptthread.
  Bei großen Szenen (Terrain) könnte das im Intervall einen spürbaren Ruckler geben. Nicht gemessen.

## 4. sRGB: teilweise

Commits `46b917ef` (Pack-Cook, GL, Metal, mit Laufzeit-Beleg `HE_DUMP_SRGBTEST`) und `f0539d2e`
(D3D11, D3D12, Vulkan, laut Commit ohne Laufzeit-Beleg, Vulkan baut kein CI-Runner).

Vorhanden:
- `TextureAsset::srgb` übersteht den Pak-Cook (`HpakWriter.cpp:712-790`), und alle fünf Backends
  wählen bei gesetztem Flag das sRGB-Pixelformat.
- Der glTF/PBR-Import setzt das Flag für Base Color und Emissive (`PbrMaterialImport.cpp:190-268`).

Restposten, der erste ist der eigentliche Produktionsfehler:
- **Jeder manuelle Texturimport ist linear.** `ImporterCommon.cpp:582-583` ruft
  `TextureImporter::import(..., TextureImporter::ImportSettings{}, ...)` mit `srgb = false`
  (`TextureImporter.h:20`) auf. Das gilt für den Content-Browser-Import und den AssetCompiler
  (`AssetCompiler/main.cpp:141`). Im Editor gibt es nirgends einen sRGB-Schalter:
  `grep -i srgb src/HE_Editor` trifft nur den Test-Witness und einen Tutorial-Text. Eine per Hand
  importierte Albedo-PNG wird also weiterhin linear gesampelt und wirkt zu hell und kontrastarm.
  Genau das Fehlerbild aus der Themenbeschreibung.
- **Plausibel:** Der Reimport (`ImporterCommon.cpp:603ff`) läuft über denselben Default. Eine
  Textur, die per glTF als sRGB kam und eine eigene Quelldatei hat, würde beim Reimport auf linear
  zurückfallen.
- Keine Migration für den Altbestand: Alles, was vor dem Flag importiert wurde, bleibt linear.
- Farbwähler (Lichtfarbe, Material-Konstanten, z. B. `MaterialEditorPanel.cpp:497`,
  `ImGuiColorEditFlags_Float`) speichern den Rohwert als linearen Wert. Das ist eine Konvention und
  kein Fehler an sich, passt aber nicht mehr zu sRGB-dekodierten Texturen: Mittelgrau aus dem
  Wähler (0,5) und Mittelgrau aus einer sRGB-PNG (≈0,21 linear) sehen verschieden aus.
- Die Ausgabe kodiert mit `pow(c, 1/2.2)` statt der stückweisen sRGB-Kurve
  (`MetalRenderer.mm:961`, `OpenGLRenderer.cpp:3253`, `HlslSources.h:401`). Das ist Design, der
  Unterschied liegt in den Tiefen.
- Veralteter Kommentar: `TextureImporter.h:17` sagt noch „D3D11/D3D12/Vulkan still upload linear
  regardless", das stimmt seit `f0539d2e` nicht mehr.
- Nicht geprüft: ob ImGui-Vorschaubilder (Content-Browser-Thumbnails, Textur-Vorschau) und
  UI-Widget-Bilder mit einer sRGB-geflaggten Textur zu dunkel erscheinen. Sie würden dekodiert in
  einen Nicht-sRGB-Framebuffer geschrieben.

---

## Empfehlung für die Folgeschritte

Die Themenüberschrift „nachziehen" ist im Kern erledigt. Die Folgeschritte sollten auf die
Restposten zeigen, geordnet nach Nutzen:

1. **sRGB am Import und Reimport** (M): ein Import-/Asset-Schalter „sRGB (Color)" mit einer
   Namensheuristik als Vorschlag (`_n`/`_normal`/`_orm`/`_rough`/`_mask` → linear, sonst sRGB), der
   Reimport behält das Flag des vorhandenen Assets, dazu eine Stapel-Aktion für den Altbestand.
   Veralteten Kommentar in `TextureImporter.h` korrigieren.
2. **Viewport-Auswahlbox in Weltkoordinaten** (S): `worldPositionOf` statt `tc->position`, auch für
   die Collider-Drahtgitter. Vorher per `he_shot`/Dump belegen.
3. **Licht- und Kamera-Visualisierung bei Auswahl** (M): Punktlicht-Radius, Spot-Kegel,
   Kamera-Frustum (Linien-Code aus `McpCameraGizmos` wiederverwenden), Symbol in Lichtfarbe.
4. **Mehrfachauswahl-Politur** (S): Ctrl+A/Select All, Esc zum Abwählen, „gemischte Werte"-Anzeige,
   Add Component für alle.
5. **Autosave für dirty Asset-Tabs** (M bis L): braucht pro Panel einen Speichern-nach-Pfad.
