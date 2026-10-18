# Editor Slice 19 – ViewportPick.cpp/.h, ViewportToolbar.cpp/.h, ViewportViewMode.h (+ src/HE_Editor/CMakeLists.txt)

## Zusammenfassung
Der Slice ist insgesamt sauber und gut kommentiert. Der einzige echte Hot Path ist `ViewportToolbar::render` (läuft jeden Frame). Die teure Hilfe-Suche pro Zelle ist bereits per Hash-Index gelöst (`EditorHelp.cpp:9634`, Perf-Audit 3.2), sodass keine schweren Per-Frame-Kosten übrig sind. Die Funde betreffen vor allem eine 460-Zeilen-Funktion mit doppelt gepflegter Layout-Arithmetik, wiederholte Textmessung pro Frame, eine handgepflegte Duplikatliste der Show-Flags und schwere Header. `ViewportPick` läuft nur bei Klick (nicht pro Frame) und ist dort korrekt; ein möglicher Randfall (ungültige Entity im Snapshot) ist als "unsicher" markiert. Funde: 0 Hoch, 3 Mittel, 9 Niedrig (EDT-1901 bis EDT-1912), dazu vier Niedrig-Befunde zur CMakeLists im Abschnitt "Zusatzprüfung".

## Funde

### EDT-1901 – `render()` ist eine 460-Zeilen-Funktion mit doppelt geführter Breitenarithmetik
- **ID**: EDT-1901
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:639-1096 (Fit 680-743, Left-Zone 745-862, Right-Zone 966-1092)
- **Problem**: Jede Zonenbreite wird zweimal berechnet und von Hand synchron gehalten: einmal in den Lambdas `leftWidth`/`rightWidth` (705-733) und ein zweites Mal beim tatsächlichen Zeichnen (z. B. `wView/wLand/wSpl` in 752-755 und 771, Tools in 786 und 802, Snap in 825 und 861, Kamera in 974, View in 1001, Mode in 1034, Show in 1059). Ändert jemand eine Zelle, muss er 2-3 Stellen anfassen; eine Abweichung verschiebt unbemerkt die Mitten-Zentrierung (`wLeft`/`wRight`, 742-743, 868-872). Dazu kommt `x += ...` / `rx += ...` als manuell mitgeführter Cursor.
- **Code-Auszug**:
```cpp
auto leftWidth = [&](bool labels, bool snap) { float w = kWellPad * 2.0f + cellWidth(m, labels ? "View" : nullptr) + ...
...
well(m, x, kWellPad * 2.0f + wView + kSegGap + wLand + kSegGap + wSpl);
...
x += kWellPad * 2.0f + wView + kSegGap + wLand + kSegGap + wSpl + kGroupGap;
```
- **Lösung**: Jede Gruppe als kleine Struktur `Group { float width(bool labels, bool snap) const; void draw(float x, ...); }` bzw. als Tabelle `{label, icon, width}`. Die Breite einmal pro Gruppe berechnen und für Messung, `well()` und den Cursor-Vorschub wiederverwenden (`const float gw = group.width(...)`). Alternativ ein Zwei-Pass-Layout mit einem `Layout`-Objekt (`advance(w)`), das beim Messdurchlauf nur summiert und beim Zeichendurchlauf zeichnet. `render()` in `drawLeftZone/drawTransport/drawRightZone` zerlegen.
- **Erwarteter Effekt**: Schätzung: ca. 80-120 Zeilen weniger, deutlich geringere Fehleranfälligkeit; Laufzeit unverändert.
- **Risiko / Verifikation**: Pixel-genaues Layout muss erhalten bleiben. Absicherung über das UI-Test-Muster (`tests/test_*_ui.cpp`, `HE_UI_DUMP_DIR`-BMP-Vergleich) bei drei Balkenbreiten (voll, ohne Labels, ohne Snap) und mit/ohne Play-Modus.

### EDT-1902 – Textbreiten werden pro Frame bis zu dreifach neu gemessen
- **ID**: EDT-1902
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:685-743, 752-754, 1001/1034 (zusätzlich `cellImpl` in EditorToolbar.cpp:79)
- **Problem**: `fits()` ruft bis zu dreimal `leftWidth()` und `rightWidth()` auf. `leftWidth` ruft bei Labels dreimal `cellWidth()` (je `CalcTextSize`), danach misst der Zeichenblock dieselben drei Labels erneut (752-754). Dazu `cellWidth` für "World"/"Local" (686), "Perspective"/"Bottom"/"Ortho" (717), "Wireframe"/"G-Buffer"/"Unlit" (723) und sechs weitere `CalcTextSize` (687-696). Die Ergebnisse hängen nur von Font und UI-Skalierung ab, nicht vom Frame. Zusammen sind es grob 40-60 `CalcTextSize`-Aufrufe pro Frame nur für die Leiste (unsicher / messen; der Perf-Audit hat die Toolbar schon einmal als 12 % der Main-Thread-CPU gemessen, ein Teil davon war die Hilfe-Suche, die inzwischen gehasht ist).
- **Code-Auszug**:
```cpp
const float orientW  = std::max(cellWidth(m, "World"), cellWidth(m, "Local"));
const float snapValW = std::floor(ImGui::CalcTextSize("88.88\xc2\xb0").x + kCellPadX * 2.0f);
...
const float viewW    = std::max({ cellWidth(m, "Perspective"), cellWidth(m, "Bottom"), cellWidth(m, "Ortho") });
```
- **Lösung**: Alle label-abhängigen Breiten in eine `struct Widths` packen, die nur neu berechnet wird, wenn sich `ImGui::GetFontSize()`/`m.cell`/Font-Pointer ändern (Schlüssel: `FontSize`, `FontScale`). `static Widths s_w; static float s_key;` im anonymen Namespace. Zusätzlich `leftWidth`/`rightWidth` nur einmal je (labels, snap)-Kombination auswerten (Ergebnis des ersten `fits()` weiterverwenden).
- **Erwarteter Effekt**: Schätzung: weniger als 0,05 ms pro Frame; nur lohnend, wenn die Messung die Leiste noch relevant zeigt. Zusammen mit EDT-1901 fast kostenlos.
- **Risiko / Verifikation**: Cache-Schlüssel muss UI-Font-Scale-Wechsel (Preferences) und DPI-Wechsel erfassen. Messen mit dem Profiler-Panel vor/nach.

### EDT-1903 – Show-Flag-Liste doppelt gepflegt (Popup vs. `showFlagFields`)
- **ID**: EDT-1903
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportToolbar.cpp:458-494 gegen ViewportPanel.cpp:72-90 (`showFlagFields`)
- **Problem**: Das Popup listet jede Flag von Hand mit eigenem Label (464-482), während `showFlagFields` bereits Tabelle mit `member` und `label` ist. "Hide All" und die "anyOff"-Anzeige (1061-1069) nutzen die Tabelle, das Popup nicht. Folge: `streamingCells` steht in der Tabelle (ViewportPanel.cpp:84), fehlt aber im Popup (bewusst? Er hat einen eigenen Schalter im Streaming-Debug-Panel, StreamingDebugView.cpp:442). "Hide All" setzt ihn trotzdem auf false, und "Show All" stellt ihn über die Defaults zurück. Eine neue Flag muss an drei Stellen eingetragen werden; vergisst man das Popup, leuchtet die Zelle "anyOff" ohne dass der Nutzer sehen kann warum.
- **Code-Auszug**:
```cpp
EditorWidgets::checkbox("Ground Grid",    &f.groundGrid);
EditorWidgets::checkbox("Editor Icons",   &f.editorIcons);
...
if (EditorWidgets::menuItem("Hide All Overlays")) { ... f.*(fields[i].member) = false; }
```
- **Lösung**: Gruppenüberschrift als weiteres Feld in die Tabelle (`group` Index), das Popup iteriert `showFlagFields`, `EditorWidgets::checkbox(fields[i].label, &(f.*fields[i].member))`; Flags ohne Popup-Eintrag (`streamingCells`) per `hidden`-Bit markieren. Die Help-Einträge hängen am Label ("Viewport Show/<Label>") und bleiben gültig, solange die Label-Strings gleich bleiben.
- **Erwarteter Effekt**: Schätzung: ca. 15 Zeilen weniger, eine Pflegeliste statt drei.
- **Risiko / Verifikation**: `python3 scripts/editor_help_audit.py --check` (der Audit erkennt Literal-Labels; nach dem Umbau sind sie Tabellen-Literale, also prüfen, ob der Audit sie noch findet) und Sichtprüfung der Gruppenreihenfolge.

### EDT-1904 – Drei handgebaute "Text-in-Zelle"-Items neben `cell()`
- **ID**: EDT-1904
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:831-847 (Snap-Wert), 878-902 (Play), 953-963 (Uhr)
- **Problem**: Snap-Wert, Spiel-Uhr und Play-Knopf wiederholen jeweils `SetCursorScreenPos` + `InvisibleButton` + Hover-Füllung + zentrierter `AddText`/Icon, was `cellImpl` (EditorToolbar.cpp:46-97) bereits kann (nur der Text-Zentrierung wegen mit `label` ohne `icon` und Breite `w` machbar: `contentW = labelW`, `left` zentriert). Zusätzlich trägt der Snap-Wert eigene BeginDisabled/Tooltip-Logik (835-839), die in `cellImpl` ebenfalls steht (Tooltip bei disabled unterdrückt).
- **Code-Auszug**:
```cpp
ImGui::SetCursorScreenPos(ImVec2(vx, m.y + kWellPad));
if (!snapUsable) ImGui::BeginDisabled();
const bool pressed = ImGui::InvisibleButton("##vpSnapValue", ImVec2(snapValW, m.cell));
...
dl->AddText(ImVec2(std::floor(vx + (snapValW - tw) * 0.5f), std::floor(m.cy - ImGui::GetFontSize() * 0.5f)), fg, buf);
```
- **Lösung**: `cell(m, vx, snapValW, "##vpSnapValue", nullptr, buf, false, snapUsable, "Snap increment...")` für den Snap-Wert; die Farbe `kFgOn` bei aktivem Snap über `cellTinted` (`fg`-Parameter existiert). Die Uhr: `cellTinted(..., clock.text, fg, true, nullptr)` plus `helpForKey`-Variante (cellImpl nimmt `helpKey`, `cellTinted` reicht ihn nicht durch; Signatur um `helpKey` erweitern). Hover-Füllung bei der Uhr bewusst nicht (Kommentar 949-951) -> ein `noHoverFill`-Flag wäre nötig, daher nur Snap-Wert sicher.
- **Erwarteter Effekt**: Schätzung: ca. 15-20 Zeilen weniger.
- **Risiko / Verifikation**: Optisch identisch halten (Text-Rundung `std::floor`); UI-Screenshot-Test.

### EDT-1905 – `ViewportToolbar.h` zieht ImGui, ImGuizmo und IRenderer in jeden Gizmo-/Panel-Include
- **ID**: EDT-1905
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides (Build-Zeit)
- **Ort**: src/HE_Editor/ViewportToolbar.h:38-43
- **Problem**: Der Header bindet `<imgui.h>`, `<ImGuizmo.h>` und `Renderer/IRenderer.h` ein, weil `State` `ImGuizmo::OPERATION/MODE` und `HE::ViewMode` als Werte hält. Er wird von EditorTransformGizmo.cpp, SplineTool.cpp, LevelScriptPanel.cpp und ViewportPanel.cpp gebraucht (Treffer in `grep ViewportToolbar::`), die alle nur `State` brauchen, nicht `render/viewPopup/...`. IRenderer.h ist ein schwerer Header.
- **Code-Auszug**:
```cpp
#include <imgui.h>     // ImGuizmo.h uses ImVec2/ImU32/ImDrawList without declaring them
#include <ImGuizmo.h>
#include "Renderer/IRenderer.h"   // HE::ViewMode
```
- **Lösung**: `State` in `ViewportToolbarState.h` auslagern (nur `ImGuizmo.h`-Enums bzw. eigene `enum class GizmoOp/GizmoSpace` und `HE::ViewMode` per Forward-Deklaration einer `enum class ViewMode : int;`, falls IRenderer.h sie so deklarieren lässt - Vorab prüfen). Die Funktionsdeklarationen bleiben in `ViewportToolbar.h`.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkrementalbuilds für 3-4 Übersetzungseinheiten; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Der Kommentar bei `activeSnap()` (104-105) erklärt, dass der Test den Gizmo, aber nicht die Leiste linkt; das Auslagern ändert daran nichts. Build aller Targets inkl. `he_tests` prüfen.

### EDT-1906 – `ViewportPick.h`: schwerer Include und nicht-const Registry
- **ID**: EDT-1906
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportPick.h:2, 46, 52; ViewportPick.cpp:20-21
- **Problem**: (a) `pick`/`pickAtScreen` nehmen `entt::registry&`, nutzen aber nur `valid`, `all_of` und `HE::terrainOwnerOf(const entt::registry&, ...)` (TerrainChunkComponent.h:39), alles const. Die Signatur lügt über Mutation und verhindert Aufrufe mit const-Welt. (b) Der Header zieht `HorizonWorld.h` (ganze ECS-Welt) nur für `Entity`/`entt::registry`.
- **Code-Auszug**:
```cpp
Entity pick(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes, ...
```
- **Lösung**: `const entt::registry&` in beiden Funktionen. Optional `using Entity = entt::entity;` über `<entt/entity/fwd.hpp>` statt `HorizonWorld.h` (Aufrufer binden die Welt ohnehin ein). Der Aufruf in ViewportPanel.cpp:1670 übergibt `ctx.world->registry()`; `const` bindet dort ohne Änderung.
- **Erwarteter Effekt**: Schätzung: marginal (API-Ehrlichkeit, etwas Buildzeit).
- **Risiko / Verifikation**: Reiner Signaturwechsel; Tests, die `ViewportPick::pick` aufrufen (grep in `tests/`), kompilieren weiter.

### EDT-1907 – `pick`: pro Objekt `glm::inverse(mat4)` und `std::function`-Aufruf vor dem Billig-Reject
- **ID**: EDT-1907
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/ViewportPick.cpp:25-39
- **Problem**: Jedes Snapshot-Objekt bekommt eine volle 4x4-Inversion plus zwei Matrix-Vektor-Produkte und einen `std::function`-Aufruf (LRU-/Map-Lookup in `meshBounds`), bevor der Strahltest läuft. Bei Szenen mit zehntausenden Objekten (Terrain-Chunks, Foliage ist ggf. nicht im Snapshot; unsicher) kostet jeder Klick diese Schleife komplett. Läuft aber nur bei Mausklick/Kontextmenü (ViewportPanel.cpp:1759, 1783), nicht pro Frame, daher niedrige Schwere.
- **Code-Auszug**:
```cpp
const glm::mat4 invModel = glm::inverse(obj.transform);
const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
```
- **Lösung**: Nur wenn gemessen relevant: grober Vorfilter im Weltraum (Weltraum-AABB aus `box.transformed(obj.transform)` ist teurer als die Inversion, daher lieber Bounding-Sphere-Radius um `obj.transform[3]`; ein Strahl-Kugel-Test ist exakt konservativ und billig), dann erst die Inversion. Alternativ affine Inversion (`glm::affineInverse`), falls Transforms garantiert affin sind (RenderObject-Transform ist es; vor Umstellung bestätigen, bei Projektionsanteil wäre das Ergebnis falsch).
- **Erwarteter Effekt**: Schätzung: bei 10k Objekten wenige 100 µs pro Klick eingespart; für den Nutzer nicht spürbar.
- **Risiko / Verifikation**: Trefferergebnis muss bit-identisch bleiben (Distanz `t` in Einheiten von |rayDir|). Bestehende Pick-Tests (`grep ViewportPick tests/`) reichen; zusätzlich einen Test mit skaliertem/rotiertem Objekt. Messen vor Umsetzung.

### EDT-1908 – Lock-/Terrain-Block in `pick` verschachtelt und mit überflüssigen `valid`-Prüfungen
- **ID**: EDT-1908
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportPick.cpp:41-65
- **Problem**: Zwei `reg.valid`-Prüfungen (43 und 55), ein eigener Block-Scope nur für `owner`, und zwei getrennte Verzweigungen (58-65) über dieselbe Variable `terrainOwner`. Außerdem: Ist `e` ungültig (veraltete Entity im Snapshot), wird sie ohne Sperr-Check als Treffer zurückgegeben (siehe "Unsicher").
- **Code-Auszug**:
```cpp
if (reg.valid(e)) { terrainOwner = HE::terrainOwnerOf(reg, e); ... }
{
	const Entity owner = (terrainOwner != entt::null) ? terrainOwner : e;
	if (reg.valid(owner) && reg.all_of<EditorLockComponent>(owner)) continue;
}
```
- **Lösung**: Früh aussteigen und ein Treffer-Ziel bündeln:
```cpp
if (!reg.valid(e)) continue;                       // (Verhalten ändert sich, siehe Unsicher)
Entity owner = HE::terrainOwnerOf(reg, e);
const bool isTerrain = owner != entt::null || reg.all_of<TerrainComponent>(e);
if (owner == entt::null) owner = e;
if (reg.all_of<EditorLockComponent>(owner)) continue;
float& best = isTerrain ? terrainDist : meshDist;
Entity& hit = isTerrain ? terrainHit : meshHit;
if (t < best) { best = t; hit = owner; }
```
- **Erwarteter Effekt**: Schätzung: ca. 10 Zeilen weniger, eine Verzweigung weniger.
- **Risiko / Verifikation**: `continue` bei ungültiger Entity ändert beobachtbares Verhalten (heute: ungültige Entity kann gewinnen). Ohne diese Änderung bleibt der Rest gleich; absichern mit einem Pick-Test, der ein Snapshot-Objekt mit gelöschter Entity enthält.

### EDT-1909 – View-Mode-Kürzel stehen als Text in der Leiste, die Tasten im Panel
- **ID**: EDT-1909
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:609-617 und 513-519; Gegenstück ViewportPanel.cpp:1283-1284, 806
- **Problem**: "Alt+4/3/2" und "Num 5/7/1/3" sind Literale in der Leiste, die tatsächlichen Tasten sind in ViewportPanel.cpp fest verdrahtet (`ImGuiKey_3`, `ImGuiKey_4`, `ImGuiKey_Keypad7`), nicht in der Shortcut-Tabelle `EditorShortcuts.cpp`. Ändert jemand die Tasten, zeigt das Menü Falsches. Die Kollision mit Bookmark-Tasten ("1".."0" bzw. Ctrl+Ziffer, 545-556) ist nur durch die Alt-Modifikation gelöst.
- **Code-Auszug**:
```cpp
{ VM::Lit, "Alt+4" }, { VM::Unlit, "Alt+3" }, { VM::Wireframe, "Alt+2" },
```
- **Lösung**: View-Mode-/Preset-Shortcuts als Einträge in `EditorShortcuts.cpp` (`viewport.viewmode.lit` usw.) anlegen und `shortcutLabel(...)` (EditorHelp.cpp:9703) für die Menüanzeige nutzen; ViewportPanel fragt dieselbe Tabelle.
- **Erwarteter Effekt**: Konsistenz, rebindbare Tasten; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Eingabe-Verhalten muss gleich bleiben (Alt+3 bleibt Unlit). `tests/` für Shortcuts prüfen, ob neue IDs Konflikt-Tests auslösen.

### EDT-1910 – Rohe `ImGui::`-Controls und Tooltips statt `EditorWidgets` im Options-Popup
- **ID**: EDT-1910
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: ViewportToolbar.cpp:415-427, 433-434, 440-442, 546, 557, 838
- **Problem**: CLAUDE.md Abschnitt 4 verlangt Editor-Controls über `EditorWidgets`. Hier stehen `ImGui::InputFloat`, `ImGui::SliderFloat`, `ImGui::MenuItem`, `ImGui::SetTooltip("The rotate gizmo's outer ring...")` und `SetItemTooltip` roh; der Tooltip bei 434 ist ein Literaltext statt eines Help-Eintrags (landet nicht in F1). Die Clamps (421-423, 427) laufen jeden offenen Popup-Frame auch ohne Edit.
- **Code-Auszug**:
```cpp
EditorWidgets::checkbox("Screen-space rotation ring", &st.rotateScreenRing);
if (ImGui::IsItemHovered())
	ImGui::SetTooltip("The rotate gizmo's outer ring, which rotates about the view axis");
```
- **Lösung**: Tooltip in einen `EditorHelp`-Eintrag ("Viewport Options/Screen-space rotation ring") verschieben (Scope ist bereits gesetzt, 409); Float-Eingaben über vorhandene EditorWidgets-Float-Variante, falls vorhanden (nicht geprüft), sonst belassen. Clamp in ein `sanitizeSnap(State&)` ziehen, das auch `snapPresets` (399) benutzt.
- **Erwarteter Effekt**: Konsistenz, F1-Hilfe; kein messbarer Laufzeiteffekt.
- **Risiko / Verifikation**: `python3 scripts/editor_help_audit.py --check`.

### EDT-1911 – `ViewportViewMode.h`: ungenutztes `<cstring>`, Namensvergleichskette
- **ID**: EDT-1911
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/ViewportViewMode.h:22-24, 34-40
- **Problem**: `<cstring>` wird nicht benutzt (kein `str*`/`mem*`-Aufruf im Header). `viewModeFromName` besteht aus sieben if-Zeilen mit denselben Mustern wie `HE::viewModeName`; die Kurznamen ("basecolor", "roughspecmetal") sind eine eigene Tabelle, was eine Pflegestelle mehr ist als nötig. `viewModeOverrideFromEnv` hält zwei statische Zeiger mit Thread-Safe-Init-Guard, wird pro Frame aufgerufen (ViewportPanel.cpp:958) - der Guard-Check kostet ein Atomic-Load, unerheblich.
- **Code-Auszug**:
```cpp
#include <cstring>
...
if (s == "lit")            return HE::ViewMode::Lit;
```
- **Lösung**: `<cstring>` entfernen. Optional `constexpr std::pair<std::string_view, HE::ViewMode> kNames[]` + Schleife (spart 5 Zeilen, ermöglicht Test über die Tabelle). Kein Perf-Gewinn erwartet.
- **Erwarteter Effekt**: Schätzung: vernachlässigbar.
- **Risiko / Verifikation**: Bestehende Tests in tests/test_editorcamera.cpp:245ff decken `viewModeFromName`/`viewModeOverride` ab.

### EDT-1912 – Spielclock-Text und Icon-Trigonometrie werden jeden Frame neu gebaut
- **ID**: EDT-1912
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: ViewportToolbar.cpp:333-359, 135-163 (iconCube/iconGlobe), 106-120, 190-199
- **Problem**: `gameClockState` formatiert pro Frame mit `snprintf("%g×")` einen String, auch wenn sich der Wert nicht ändert; `iconGlobe` berechnet 24x `cos`/`sin`, `iconCube` 6x, `iconRotate`/`iconEye` weitere Bogenberechnungen, ebenfalls pro Frame. Summe: grob 100-150 Trigonometrie-Aufrufe und ein `snprintf` pro Frame. Das ist weit unter 0,05 ms (unsicher / messen); als Doku-Fund aufgenommen, weil die Leiste bereits einmal im Perf-Audit auffiel.
- **Code-Auszug**:
```cpp
for (int i = 0; i < 24; ++i)
{
	const float a = i * (2.0f * kPi / 24.0f);
	e[i] = ImVec2(c.x + std::cos(a) * r * 0.45f, c.y + std::sin(a) * r);
}
```
- **Lösung**: Nur wenn ein Profil es zeigt: `static const` Einheitskreistabelle (`constexpr`-Sinus/Cosinus-Tabelle für 24 Punkte) und Uhrtext nur bei geändertem `scale` neu formatieren (statisches Cache-Paar `lastScale/text`).
- **Erwarteter Effekt**: Schätzung: unter 0,01 ms; nicht umsetzen ohne Messung.
- **Risiko / Verifikation**: Pixelgleichheit der Icons (Rundung); sonst keines.

## Cross-Modul-Hinweise
- src/HE_Editor/EditorToolbar.cpp:46-97 (`cellImpl`): `cellTinted` (107-111) reicht `helpKey` nicht durch; dadurch zwingt es Aufrufer zu handgebauten Zellen (siehe EDT-1904).
- src/HE_Editor/ViewportPanel.cpp:72-90 (`showFlagFields`) ist die eigentliche Quelle der Show-Flag-Liste; ein Gruppenfeld dort würde EDT-1903 lösen.
- src/HE_Editor/ViewportPanel.cpp:958: `SetViewMode` wird jeden Frame gepusht; Backends sollten ein Setzen des gleichen Werts als No-Op behandeln (nicht geprüft, in `src/HE_Rendering/.../*Renderer.*` nachsehen).
- src/HE_Editor/CMakeLists.txt: siehe "Gelesen" und die Hinweise unten.

### Zusatzprüfung CMake / nicht gelisteter Dateien in src/HE_Editor
- Nicht in einer Slice-Liste von `Editor.txt` stehen (außer `vendor/`, laut CLAUDE.md übersprungen): nur `src/HE_Editor/CMakeLists.txt` (732 Zeilen). Keine `*.mm`, `Guides/` oder weiteren Dateitypen fehlen (`Guides/*.cpp`, `*.mm` und `EditorSystemMotion.*` sind in der Liste). `vendor/` (imgui, imguizmo, ImGuiColorTextEdit, stb_image.h) wurde absichtlich nicht gelesen.
- Befund CMake (Niedrig, Typ Vereinfachung, Kategorie F): `add_executable(HorizonEditor ...)` listet in Zeile 399 eine zweite Block-Liste von ca. 55 Headern (`"EditorUI.h" "EditorUI.cpp" "ScriptEditorPanel.h" ...`), die eine unvollständige, historisch gewachsene Header-Spiegelung ist (nur für IDE-Projektbäume). `EditorUI.cpp` steht nur dort, nicht bei den übrigen `.cpp`; ebenso `EditorUI.h` an `imgui_editor` (Zeile 67), obwohl `imgui_editor` ausschließlich Vendor-Quellen baut. Vorschlag: `EditorUI.cpp` in die normale Quellenliste verschieben, die Headerliste entfernen (CMake braucht Header nicht) oder per `file(GLOB ... CONFIGURE_DEPENDS *.h)` ersetzen; `"EditorUI.h"` aus `imgui_editor` entfernen. Risiko: IDE-Projektstruktur ändert sich; Build-Verifikation aller Plattformen nötig.
- Befund CMake (Niedrig, Performance Build, Kategorie D): `copy_directory "${EDITOR_DEPS_DIR}"` läuft bei jedem Link doppelt (Zeilen 519-524) und der gesamte `${DEPLOY_GAME}`-Baum (609-612) sowie die SDK-Header (632-645, inklusive `rm -rf` und vollständigem Neukopieren von `src/HE_Core/include` und `glm`) bei jedem Editor-Relink; `copy_directory` kopiert ohne Vergleich. Das verlängert jeden inkrementellen Editor-Link (unsicher / messen, Größe von `EditorDeps` und `DEPLOY_GAME` nicht geprüft). Vorschlag: `copy_directory_if_different` (CMake >= 3.26) für die Verzeichnisse und das `rm -rf` des SDK nur bei geänderten Headern; eigenes Custom-Target `editor_deploy`, das nicht am Link-Schritt hängt.
- Befund CMake (Niedrig, Kategorie F): `target_compile_definitions(HorizonCore PRIVATE HE_EDITOR)` (Zeile 489) setzt ein Flag auf ein Fremd-Target aus dem Editor-Verzeichnis; die Reihenfolge der `add_subdirectory`-Aufrufe wird dadurch implizit zur Voraussetzung. Besser in `src/HE_Core/CMakeLists.txt` hinter einer Option.
- Befund CMake (Niedrig, Kategorie F): Zeilen 36-43 und 87-101 pflegen die Vulkan-Auffindung (`find_package(Vulkan REQUIRED)`) in zwei Verzeichnissen parallel (Kommentar verweist auf `src/HE_Rendering/CMakeLists.txt`); ein gemeinsames `cmake/FindHeVulkan.cmake`-Include verhindert Drift.

## Unsicher / Messung nötig
- Tatsächliche Kosten von `ViewportToolbar::render` pro Frame (Anzahl `CalcTextSize`-Aufrufe, EDT-1902/1912) mit dem Profiler-Panel messen, bevor gecacht wird.
- Ob `pick` mit ungültigen Entities im Snapshot (gelöscht, aber noch im zuletzt extrahierten `RenderWorld`) vorkommt: Dann gibt `pick` (ViewportPick.cpp:41-67) eine ungültige Entity als Treffer zurück, weil der Lock-Check nur bei `reg.valid(owner)` läuft. Aufrufer ViewportPanel.cpp:1759/1783 auf Nachprüfung lesen (nicht vertieft).
- Ob `RenderObject::transform` immer affin ist (für `glm::affineInverse`, EDT-1907).
- Ob `streamingCells` absichtlich im Show-Popup fehlt (EDT-1903).
- Ob `SetViewMode` pro Frame im Backend billig ist (ViewportPanel.cpp:958).
- Größe von `EditorDeps` und `DEPLOY_GAME` (CMake-Post-Build-Kosten).

## Gelesen
Vollständig gelesen: ViewportPick.cpp, ViewportPick.h, ViewportToolbar.cpp (1-1100), ViewportToolbar.h, ViewportViewMode.h sowie src/HE_Editor/CMakeLists.txt (1-732); für Querverweise nur gezielt Ausschnitte von EditorToolbar.cpp, ViewportPanel.cpp, EditorHelp.cpp (`findKey`) und AABB.h gelesen; `src/HE_Editor/vendor/**` absichtlich nicht gelesen (Fremdcode laut CLAUDE.md).
