# Entity-Cooking Schritt 2: Kochbarkeits-Analyse und Mobility

Stand 14.11.2026, Thema 343, Branch `claude/entity-cooking-statische-entities-auf-daten-reduzieren`.
Setzt `docs/entity-cooking-survey-2026-11-13.md` §5 um. Nichts liest die Analyse oder das Flag zur Laufzeit.

## Was gebaut ist

- `src/HE_Scene/include/HorizonScene/Components/MobilityComponent.h`: `MobilityComponent { Mode mode = Movable; }`,
  `Mode::{Movable, Static}` (eigener Name, nicht `RigidBodyType::Static`).
- `src/HE_Scene/include/HorizonScene/CookAnalysis.h` + `src/CookAnalysis.cpp`: `HE::analyzeCookability(reg, e)` (eine Entity,
  läuft die Eltern hoch) und `HE::analyzeCookability(reg)` (alle Entities mit `TransformComponent`, Ahnen-Cache, loggt
  `Static`-aber-blockiert als Warnung). Ergebnis `CookVerdict::blockers`, ein Bit je Grund (`CookBlocker`).
- Serialisierung: Schlüssel `"mobility": "static"|"movable"` nur wenn die Komponente da ist (`SceneSerializer.cpp`,
  Save neben `"inactive"`, Load daneben). Kein Schlüssel = "abgeleitet": alte Szenen laden und speichern byteweise wie vorher.
  Schema-Version unverändert (additiver optionaler Schlüssel).
- Registrierung: X-Makro `HE_SCENE_COMPONENT_KEYS`, `isKnownComponentKey`, `kComponentScopes` (`EditorHelp.cpp`, "Mobility"),
  `CellSplit.cpp` (`StreamClass::Static`; ohne Eintrag bliebe die Entity als Resident in der Basis).
  `resetComponentForKey` setzt auf Movable zurück (kein Sonderfall nötig). Der Sonderfall `prefab`/`inactive` gilt nicht.

## Regeln (Survey-Nummern)

1 Script · 2 Animator/Layer/Blend/StateMachine/RootMotion/IK · 3 PropertyAnimator/SequencePlayer · 4 RigidBody != Static ·
5 CharacterController/NavAgent/Movement/ParticleSystem · 6 `NetworkComponent::replicates` oder `ReplicatedVars` mit Einträgen ·
7 SaveState · 8 LOD mit >1 Level oder `refinedMeshId` · 9 Trail/Rope · 10 Ahne mit eigenem Blocker (inkl. Movable) ·
11 `StreamingComponent::Mode::Auto` · 12 `InactiveComponent` oder `MeshComponent::visible == false` · zusätzlich
`MarkedMovable` (explizit Movable). Hinweis: der Auftragstext nennt "Regel 6" für Joint/Rope-Ziele; im Survey ist das §6/§10 Schritt 3
(Rückindex), Regel 6 ist dort Netzwerk.

## Entscheidungen

- Mobility ist Daten, kein Tag; Abwesenheit = abgeleitet (Default aus der Analyse). Komponenten-Default `Movable`.
- Regeln sind hartes Veto: `Movable` schließt aus, `Static` hebt keine Regel auf, Widerspruch wird einmal pro Analyse-Lauf geloggt.
- Regel 11 ist nur teilweise entscheidbar: eine geladene Welt trägt keine Zell-Zugehörigkeit. Nur explizites `Auto` wird gesehen.
  Eine Entity ohne `StreamingComponent` in einer gesplitteten Szene ist für Schritt 3/5 offen (Block muss dem Zell-Entladen folgen).
- Joint-/Rope-Ziele (Survey §6, Rückindex) bewusst NICHT hier: gehört zum Einfrieren.

## Geprüft

Build `he_tests` (Release, Ninja, voller Neubau) grün. `he_tests --source-file=*test_cook_analysis.cpp` 6 Fälle/105 Assertions,
`*test_scene_serializer.cpp` 46/1835, `*test_cell_split.cpp` 13/410 (neuer Key "mobility" in der Verschiebe-Liste),
`test_inspector_prefab_keys`, `test_editor_help*`, `test_scene_entity_ids` grün (46 Fälle). `scripts/editor_help_audit.py --check` ok.
Nicht geprüft: Editor-Build, volle `ctest`-Läufe, Windows/Linux. Kein Formatter vorhanden (kein `.clang-format`, kein `clang-format` auf dem Gerät).
Perf: nicht gemessen (Analyse läuft nur bei Load/Export).

## Nächste Schritte

1. Schritt 3: flache Blöcke; Rückindex für Joint/Rope-Ziele (§6) vor dem Einfrieren; `analyzeCookability(reg)` einmal beim Load/Export aufrufen.
2. Optional: Inspector-Feld für Mobility (braucht `EditorHelp.cpp`-Einträge für jedes beschriftete Control; "Mobility" steht schon in `kComponentScopes`).
3. Schritt 4: Entkoch-Hooks (Survey §8), Reparent, Zell-Entladen.
