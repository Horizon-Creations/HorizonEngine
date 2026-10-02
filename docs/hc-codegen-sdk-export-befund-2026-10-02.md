# Befund: Export findet die HorizonCode-Codegen-SDK nicht (Thema 132, Schritt 1, 2026-10-02)

Symptom (Windows, Shipping-Export aus dem deployten Editor):
`HorizonCode: compile failed, shipped interpreted (no codegen SDK found (HE_HCGEN_SDK, <editor>/SDK, he_sdk_config.json))`

## Ursache

**Ein deployter Editor hat keine der drei SDK-Quellen. Ausgeliefert wird nur der Dev-Fallback, und zwar
nicht in den Deploy.** Die Pfaderkennung selbst arbeitet korrekt: Sie sucht an den richtigen Stellen,
dort liegt aber nichts.

`HE::hccg::resolveSdk(editorBaseDir)` (`src/HE_Scene/src/HcCodegen.cpp:3963-3996`) prüft in dieser Reihenfolge:

1. die Env-Variable `HE_HCGEN_SDK` mit dem Layout `<root>/include` und `<root>/lib`,
2. `<editorBaseDir>/SDK/include` und `SDK/lib`,
3. `<editorBaseDir>/he_sdk_config.json` mit `includeDirs[]` und `libDir`.

`editorBaseDir` ist `SDL_GetBasePath()`, also das Verzeichnis der laufenden `HorizonEditor.exe`
(`src/HE_Editor/ExportDialogPanel.cpp:1449`, an den Worker als `hcBase` übergeben, `:1693`, `:1823`).
Findet keine Quelle etwas, gibt `buildDylib` die obige Meldung zurück (`HcCodegen.cpp:4683-4686`).

Am realen Deploy auf NN-WS03 (`C:/Users/conno/source/repos/HorizonEngineBuild/Editor`) geprüft:

| Quelle | Zustand |
|---|---|
| `HE_HCGEN_SDK` | weder im Prozess noch im User- oder Machine-Environment gesetzt |
| `Editor/SDK/` | existiert nicht |
| `Editor/he_sdk_config.json` | existiert nicht |
| Log | `HorizonEngine.1.log:98` (13:08:50): `HorizonCode codegen: no codegen SDK found (…)` von `ExportDialogPanel.cpp:1853` |

Warum keine der Quellen da ist:

- **`SDK/` wird von keinem Build-, Deploy-, Paket- oder CI-Schritt gestagt.** Plan §8.3 Punkt 2
  (`docs/horizoncode-cpp-codegen-implementation-plan.md`) sagt: „The engine's deploy step gains a target
  that stages include/ … + glm … lib/ … (Windows: the corresponding .lib import libraries, added to the
  deploy)“. Dieses Target wurde nie gebaut. `git log -G"he_sdk_config|/SDK"` über alle CMakeLists, `*.cmake`,
  `scripts/` und `.github/` findet nur `5b4b6b31` (WP0–WP5), und der dort eingeführte Code ist der
  Dev-Fallback. Auch `scripts/package_macos.sh` stagt kein SDK. Ein Rückschritt ist das also nicht:
  Die Lücke besteht seit der Einführung und betrifft jeden deployten oder paketierten Editor, nicht nur
  Windows.
- **`he_sdk_config.json` landet nur im Build-Baum.** `src/HE_Editor/CMakeLists.txt:541-543` erzeugt sie per
  `file(GENERATE OUTPUT "$<TARGET_FILE_DIR:HorizonEditor>/he_sdk_config.json")`, also nach
  `out/build/<preset>/src/HE_Editor/`. Dort liegt sie auch (x64-debug und x64-release). Die POST_BUILD-Kopien
  nach `DEPLOY_EDITOR` (DLLs, `Game/`, `Shaders/`, cmake) nehmen sie nicht mit. Wer, wie der Mensch, den
  Editor aus `DEPLOY_DIR/Editor` startet, sieht sie nie.
- Der Live-Check zu Thema 96 (`docs/windows-msvc-toolchain-befund.md`, „Verbleibende Lücken“) lief nur,
  weil die JSON **von Hand** neben die exe kopiert worden war. Ein Commit, der das automatisiert,
  existiert nicht. Den Export mit „Compile HorizonCode“ hat dieser Check selbst nicht gefahren.

## Was eine SDK für den generierten Code tatsächlich braucht

Die Angabe dazu in Plan §8.3 ist veraltet. Nachgeprüft am Generator:

- **Header:** Der generierte Code inkludiert nur `<HorizonCode/HorizonCodeGenSupport.h>` und
  `<HorizonCode/HorizonCodeCompiled.h>` (`HcCodegen.cpp:707/765/3042/3846`), dazu seine eigenen
  `hcgen_*.h`. Beide Header liegen in **`src/HE_Core/include`** und ziehen `Types/Defines.h` sowie
  `HorizonCode.h` nach. `HorizonCode.h` braucht **glm**. Die HE_Scene-Header aus dem Plan werden nicht
  gebraucht. Das deckt sich mit der Dev-Config: `includeDirs = [src/HE_Core/include, _deps/glm-src]`.
- **Link:** Die generierte CMakeLists (`HcCodegen.cpp:3951-3953`) linkt nur `HorizonCore` aus
  `HE_SDK_LIB_DIR`. Unter MSVC heißt das: Gebraucht wird **`HorizonCore.lib`**, die Import-Lib. Der
  Windows-Deploy enthält nur `HorizonCore.dll`, keine `.lib`. Ein `SDK/lib` muss deshalb die `.lib`
  mitbringen. macOS und Linux linken direkt gegen die dylib bzw. so.

## Auflagen für den Fix (Schritt 2+)

1. **SDK in den Deploy stagen:** `DEPLOY_EDITOR/SDK/include` bekommt die Kopie von `src/HE_Core/include`
   plus `glm/` aus `glm_SOURCE_DIR`, `SDK/lib` bekommt `$<TARGET_LINKER_FILE:HorizonCore>` (Windows:
   `.lib`). Als POST_BUILD wie die übrigen Deploy-Kopien. Der CI-Zip- bzw. Installer-Pfad und
   `package_macos.sh` müssen dasselbe `SDK/` mitnehmen.
2. **ABI:** Die `.lib` und die Header müssen aus demselben Build stammen wie die deployten DLLs. Die
   Dev-JSON einfach in den Deploy zu kopieren genügt nicht: Sie zeigt mit absoluten Pfaden in *einen*
   Build-Baum (z. B. x64-debug), während der Deploy vielleicht von x64-release stammt. Außerdem bricht sie
   auf jeder fremden Maschine.
3. **GameLogic-Build nicht kaputtmachen:** `engineRootFromSdk()` (`HcCodegen.cpp:4644-4656`) leitet die
   Engine-Wurzel aus einem Include-Pfad der Form `…/src/HE_Core/include` ab. `GameLogicBuildPanel.cpp:95-102`
   bricht ab, wenn dabei nichts herauskommt. Ein gestagtes `SDK/include` hat diese Form nicht. Stagt man
   die SDK naiv, liefert `resolveSdk` sie zuerst (Stufe 2 vor 3), und der „Build Game Logic“-Knopf meldet
   im Dev-Build „engine headers could not be located“. Abhilfe: `SDK/` z. B. als `SDK/src/HE_Core/include`
   stagen oder `engineRootFromSdk` bzw. das Scaffold-`HORIZON_ENGINE_DIR` um das SDK-Layout erweitern.
4. **Test:** ein doctest für `resolveSdk` mit temporärem `<base>/SDK/{include,lib}` (Env leer). Ein
   Live-Export mit „Compile HorizonCode“ gegen einen **privaten** Deploy (`-DDEPLOY_DIR`) prüft das Ergebnis:
   Erwartet wird `HorizonCodeGen.dll` im Export, und der Log darf kein „shipped interpreted“ enthalten.

Diagnoseweg, damit es im Deploy des Menschen sichtbar bleibt: Im echten Deploy wurde nichts angelegt
oder geändert.
