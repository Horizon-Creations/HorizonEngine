# Script-API-Doku: Abdeckung je Registry-Id (generiert)

Erzeugt von `scripts/script_api_docs/coverage.py` aus `scripts/script_api_docs/registry.json` (Dump von `HE::api::registry()`) und der lokalen Website-Doku. Nicht von Hand pflegen, neu erzeugen. Einordnung und Folgeschritte: `docs/script-api-docs-gap-audit-2026-09-24.md`.

Status: **ref** = Signatur+Rückgabe dokumentiert (Zeile in der generierten Referenz `scripting-reference.html` oder flacher Zwilling in `scripting-api.html#api`), **named** = Id wörtlich auf einer anderen Doku-Seite, **catalog** = nur der Name im HorizonCode-Knotenkatalog, **missing** = nirgends. **hand** = es gibt Handinhalt aus `overlay/` (Gruppen-Einleitung oder zur Id eine Notiz, ein Beispiel, ein Recht, eine Skript-Signatur): das, was der Generator nicht schreiben kann, und die Zahl, die die Gruppen-Schritte bewegen.

**Gesamt: 582 Registry-Ids in 42 Gruppen** — ref 582, named 0, catalog 0, missing 0; hand 582.

| Gruppe | Kategorie | Ids | Lua/Py `horizon.<gruppe>.*` | ref | named | catalog | missing | hand |
|---|---|---:|:---:|---:|---:|---:|---:|---:|
| `env` | Environment | 116 | ja | 116 | 0 | 0 | 0 | 116 |
| `net` | Multiplayer | 44 | ja | 44 | 0 | 0 | 0 | 44 |
| `physics` | Physics | 33 | ja | 33 | 0 | 0 | 0 | 33 |
| `math` | Math | 31 | ja | 31 | 0 | 0 | 0 | 31 |
| `widget` | Widget | 28 | ja | 28 | 0 | 0 | 0 | 28 |
| `camera` | Camera | 26 | ja | 26 | 0 | 0 | 0 | 26 |
| `app` | App | 24 | ja | 24 | 0 | 0 | 0 | 24 |
| `audio` | Audio | 18 | ja | 18 | 0 | 0 | 0 | 18 |
| `entity` | Entity | 18 | ja | 18 | 0 | 0 | 0 | 18 |
| `input` | Input | 17 | ja | 17 | 0 | 0 | 0 | 17 |
| `save` | Save | 17 | ja | 17 | 0 | 0 | 0 | 17 |
| `anticheat` | AntiCheat | 15 | ja | 15 | 0 | 0 | 0 | 15 |
| `fs` | File | 13 | ja | 13 | 0 | 0 | 0 | 13 |
| `time` | Time | 13 | ja | 13 | 0 | 0 | 0 | 13 |
| `scene` | Scene | 12 | ja | 12 | 0 | 0 | 0 | 12 |
| `string` | String | 12 | ja | 12 | 0 | 0 | 0 | 12 |
| `ui` | UI | 12 | ja | 12 | 0 | 0 | 0 | 12 |
| `datetime` | DateTime | 9 | ja | 9 | 0 | 0 | 0 | 9 |
| `http` | HTTP | 9 | ja | 9 | 0 | 0 | 0 | 9 |
| `prefs` | Prefs | 9 | ja | 9 | 0 | 0 | 0 | 9 |
| `animator` | Animator | 8 | ja | 8 | 0 | 0 | 0 | 8 |
| `json` | JSON | 8 | ja | 8 | 0 | 0 | 0 | 8 |
| `transform` | Transform | 8 | **nein** | 8 | 0 | 0 | 0 | 8 |
| `db` | Database | 7 | ja | 7 | 0 | 0 | 0 | 7 |
| `locomotion` | Locomotion | 6 | ja | 6 | 0 | 0 | 0 | 6 |
| `movement` | Movement | 6 | ja | 6 | 0 | 0 | 0 | 6 |
| `nav` | Navigation | 6 | ja | 6 | 0 | 0 | 0 | 6 |
| `player` | Player | 6 | ja | 6 | 0 | 0 | 0 | 6 |
| `theme` | Theme | 6 | ja | 6 | 0 | 0 | 0 | 6 |
| `dialog` | Dialog | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `random` | Random | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `timer` | Timer | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `window` | Window | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `content` | Content | 4 | ja | 4 | 0 | 0 | 0 | 4 |
| `debug` | Debug | 4 | ja | 4 | 0 | 0 | 0 | 4 |
| `particle` | Particles | 4 | ja | 4 | 0 | 0 | 0 | 4 |
| `clipboard` | Clipboard | 3 | ja | 3 | 0 | 0 | 0 | 3 |
| `print` | Print | 3 | ja | 3 | 0 | 0 | 0 | 3 |
| `process` | Process | 3 | ja | 3 | 0 | 0 | 0 | 3 |
| `material` | Material | 2 | **nein** | 2 | 0 | 0 | 0 | 2 |
| `cursor` | Cursor | 1 | **nein** | 1 | 0 | 0 | 0 | 1 |
| `log` | Debug | 1 | **nein** | 1 | 0 | 0 | 0 | 1 |

## Flache `horizon.*`-Funktionen (35, nicht in der Registry)

Hand geschriebene Shims, identisch in Lua (`ScriptContext.cpp`) und Python (`PyScriptBackend.cpp`).

- dokumentiert mit Signatur (35): `log`, `getName`, `getPosition`, `setPosition`, `getRotation`, `setRotation`, `getScale`, `setScale`, `spawn`, `destroy`, `raycast`, `setVelocity`, `isGrounded`, `setMaterialParam`, `getMaterialParam`, `setUIText`, `getUIText`, `setUIColor`, `getUIColor`, `setUIVisible`, `isUIVisible`, `setUIPosition`, `getUIPosition`, `setUISize`, `getUISize`, `setUIMaterialParam`, `createWidget`, `destroyWidget`, `showWidget`, `hideWidget`, `setWidgetZOrder`, `isWidgetVisible`, `callWidgetFunction`, `showCursor`, `hideCursor`
- **ohne Signatur** (0): 

## Lifecycle-Callbacks

Aus dem Code gelesen, dort wo die Engine die Methode sucht (Lua: `ScriptEngine.cpp`, Python: `PyScriptBackend.cpp`; `onRep_`/`on_rep_` = Präfix vor dem Variablennamen). Dokumentiert = Name kommt auf einer Doku-Seite vor.

- Lua (25): `onAnimationNotify`, `onAnimationNotifyBegin`, `onAnimationNotifyEnd`, `onBeginOverlap`, `onCheatDetected`, `onClick`, `onCollisionEnter`, `onCollisionExit`, `onConnected`, `onDisconnected`, `onEndOverlap`, `onHoverEnter`, `onHoverExit`, `onInputAxis`, `onInputAxis2D`, `onInputPressed`, `onInputReleased`, `onPlayerJoined`, `onPlayerLeft`, `onRep_`, `onSessionEnded`, `onSessionStarted`, `onStart`, `onTimer`, `onUpdate`
- Python (25): `on_animation_notify`, `on_animation_notify_begin`, `on_animation_notify_end`, `on_begin_overlap`, `on_cheat_detected`, `on_click`, `on_collision_enter`, `on_collision_exit`, `on_connected`, `on_disconnected`, `on_end_overlap`, `on_hover_enter`, `on_hover_exit`, `on_input_axis`, `on_input_axis2d`, `on_input_pressed`, `on_input_released`, `on_player_joined`, `on_player_left`, `on_rep_`, `on_session_ended`, `on_session_started`, `on_start`, `on_timer`, `on_update`

## Je Gruppe

### `env` — Environment (116)

58 Felder, je get+set. Kandidat für eine generierte Tabelle statt Einzeleinträgen.

| Feld | Typ | Status |
|---|---|---|
| TimeOfDay | Float | ref |
| CycleSeconds | Float | ref |
| SunIntensity | Float | ref |
| MoonIntensity | Float | ref |
| MoonPhase | Float | ref |
| MoonCycleDays | Float | ref |
| CloudCoverage | Float | ref |
| WindDirection | Float | ref |
| WindSpeed | Float | ref |
| CloudHeight | Float | ref |
| CloudShadowStrength | Float | ref |
| CloudEvolution | Float | ref |
| CloudDensity | Float | ref |
| CloudFluffiness | Float | ref |
| ContrailAmount | Float | ref |
| CirrusAmount | Float | ref |
| CirrusSeed | Float | ref |
| GodRays | Float | ref |
| ShootingStars | Float | ref |
| LensFlare | Float | ref |
| FogDensity | Float | ref |
| FogHeightFalloff | Float | ref |
| RainAmount | Float | ref |
| SnowAmount | Float | ref |
| Wetness | Float | ref |
| Flash | Float | ref |
| AuroraIntensity | Float | ref |
| MilkyWayIntensity | Float | ref |
| NebulaIntensity | Float | ref |
| NebulaSeed | Float | ref |
| NebulaCoverage | Float | ref |
| AuroraHeight | Float | ref |
| AuroraFragmentation | Float | ref |
| StarBrightness | Float | ref |
| StarSize | Float | ref |
| StarSizeVariation | Float | ref |
| StarGlow | Float | ref |
| StarTwinkle | Float | ref |
| StarDensity | Float | ref |
| DayNightCycle | Bool | ref |
| AutoAdvance | Bool | ref |
| MoonPhaseAuto | Bool | ref |
| CloudShadows | Bool | ref |
| CloudInterShadows | Bool | ref |
| LowResClouds | Bool | ref |
| CloudMode | Int | ref |
| CloudQuality | Int | ref |
| CloudStyle | Int | ref |
| NebulaQuality | Int | ref |
| SunColor | Color | ref |
| MoonColor | Color | ref |
| CloudTint | Color | ref |
| NebulaColor | Color | ref |
| NebulaColor2 | Color | ref |
| NebulaColor3 | Color | ref |
| AuroraColor | Color | ref |
| AuroraColorTop | Color | ref |
| StarColor | Color | ref |

### `net` — Multiplayer (44)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `net.host(port: Int, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-host · hand |
| `net.joinDirect(address: String, port: Int, code: String, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-joinDirect · hand |
| `net.joinLan(index: Int, code: String, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-joinLan · hand |
| `net.leave()` | exec | ref | scripting-reference.html#net-leave · hand |
| `net.status()` → status: Int | pure | ref | scripting-reference.html#net-status · hand |
| `net.lastError()` → error: String | pure | ref | scripting-reference.html#net-lastError · hand |
| `net.sessionId()` → sessionId: String | pure | ref | scripting-reference.html#net-sessionId · hand |
| `net.joinCode()` → code: String | pure | ref | scripting-reference.html#net-joinCode · hand |
| `net.refreshLan()` | exec | ref | scripting-reference.html#net-refreshLan · hand |
| `net.lanSessionCount()` → count: Int | pure | ref | scripting-reference.html#net-lanSessionCount · hand |
| `net.lanSessionName(index: Int)` → name: String | pure | ref | scripting-reference.html#net-lanSessionName · hand |
| `net.lanSessionPlayers(index: Int)` → players: Int | pure | ref | scripting-reference.html#net-lanSessionPlayers · hand |
| `net.isAuthority()` → isAuthority: Bool | pure | ref | scripting-reference.html#net-isAuthority · hand |
| `net.isClient()` → isClient: Bool | pure | ref | scripting-reference.html#net-isClient · hand |
| `net.localPlayer()` → player: Int | pure | ref | scripting-reference.html#net-localPlayer · hand |
| `net.playerCount()` → count: Int | pure | ref | scripting-reference.html#net-playerCount · hand |
| `net.playerAt(index: Int)` → player: Int | pure | ref | scripting-reference.html#net-playerAt · hand |
| `net.playerName(player: Int)` → name: String | pure | ref | scripting-reference.html#net-playerName · hand |
| `net.ping(player: Int)` → ms: Float | pure | ref | scripting-reference.html#net-ping · hand |
| `net.kick(player: Int)` | exec | ref | scripting-reference.html#net-kick · hand |
| `net.ownerOf(entity=self: Int)` → player: Int | pure | ref | scripting-reference.html#net-ownerOf · hand |
| `net.isLocallyControlled(entity=self: Int)` → local: Bool | pure | ref | scripting-reference.html#net-isLocallyControlled · hand |
| `net.localCharacter()` → entity: Int | pure | ref | scripting-reference.html#net-localCharacter · hand |
| `net.declareVarBool(entity=self: Int, name: String, initial: Bool, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarBool · hand |
| `net.declareVarInt(entity=self: Int, name: String, initial: Int, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarInt · hand |
| `net.declareVarFloat(entity=self: Int, name: String, initial: Float, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarFloat · hand |
| `net.declareVarString(entity=self: Int, name: String, initial: String, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarString · hand |
| `net.declareVarVec3(entity=self: Int, name: String, initial: Vec3, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarVec3 · hand |
| `net.setVarBool(entity=self: Int, name: String, value: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarBool · hand |
| `net.setVarInt(entity=self: Int, name: String, value: Int)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarInt · hand |
| `net.setVarFloat(entity=self: Int, name: String, value: Float)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarFloat · hand |
| `net.setVarString(entity=self: Int, name: String, value: String)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarString · hand |
| `net.setVarVec3(entity=self: Int, name: String, value: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarVec3 · hand |
| `net.getVarBool(entity=self: Int, name: String)` → value: Bool | pure | ref | scripting-reference.html#net-getVarBool · hand |
| `net.getVarInt(entity=self: Int, name: String)` → value: Int | pure | ref | scripting-reference.html#net-getVarInt · hand |
| `net.getVarFloat(entity=self: Int, name: String)` → value: Float | pure | ref | scripting-reference.html#net-getVarFloat · hand |
| `net.getVarString(entity=self: Int, name: String)` → value: String | pure | ref | scripting-reference.html#net-getVarString · hand |
| `net.getVarVec3(entity=self: Int, name: String)` → value: Vec3 | pure | ref | scripting-reference.html#net-getVarVec3 · hand |
| `net.callServer(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callServer · hand |
| `net.callClient(player: Int, entity: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callClient · hand |
| `net.callAllClients(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callAllClients · hand |
| `net.allowAnyClient(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-allowAnyClient · hand |
| `net.rpcSender()` → player: Int | pure | ref | scripting-reference.html#net-rpcSender · hand |
| `net.hasVar(entity=self: Int, name: String)` → declared: Bool | pure | ref | scripting-reference.html#net-hasVar · hand |

### `physics` — Physics (33)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `physics.raycast(origin: Vec3, direction: Vec3, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-raycast · hand |
| `physics.setVelocity(entity=self: Int, velocity: Vec3)` | exec | ref | scripting-reference.html#physics-setVelocity · hand |
| `physics.isGrounded(entity=self: Int)` → grounded: Bool | pure | ref | scripting-reference.html#physics-isGrounded · hand |
| `physics.sphereCast(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-sphereCast · hand |
| `physics.overlapSphere(center: Vec3, radius: Float)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapSphere · hand |
| `physics.raycastLayers(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-raycastLayers · hand |
| `physics.sphereCastLayers(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-sphereCastLayers · hand |
| `physics.overlapSphereLayers(center: Vec3, radius: Float, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapSphereLayers · hand |
| `physics.boxCast(origin: Vec3, halfExtents: Vec3, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-boxCast · hand |
| `physics.capsuleCast(origin: Vec3, radius: Float, height: Float, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-capsuleCast · hand |
| `physics.overlapBox(center: Vec3, halfExtents: Vec3, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapBox · hand |
| `physics.overlapCapsule(center: Vec3, radius: Float, height: Float, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapCapsule · hand |
| `physics.raycastAll(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → entities: Int[], points: Vec3[], normals: Vec3[], distances: Float[], layers: Int[] | pure | ref | scripting-reference.html#physics-raycastAll · hand |
| `physics.addForce(entity=self: Int, force: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addForce · hand |
| `physics.addImpulse(entity=self: Int, impulse: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addImpulse · hand |
| `physics.addTorque(entity=self: Int, torque: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addTorque · hand |
| `physics.addForceAtPosition(entity=self: Int, force: Vec3, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addForceAtPosition · hand |
| `physics.addImpulseAtPosition(entity=self: Int, impulse: Vec3, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addImpulseAtPosition · hand |
| `physics.getVelocity(entity=self: Int)` → velocity: Vec3 | pure | ref | scripting-reference.html#physics-getVelocity · hand |
| `physics.setAngularVelocity(entity=self: Int, angularVelocity: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setAngularVelocity · hand |
| `physics.getAngularVelocity(entity=self: Int)` → angularVelocity: Vec3 | pure | ref | scripting-reference.html#physics-getAngularVelocity · hand |
| `physics.setGravity(gravity: Vec3)` | exec | ref | scripting-reference.html#physics-setGravity · hand |
| `physics.getGravity()` → gravity: Vec3 | pure | ref | scripting-reference.html#physics-getGravity · hand |
| `physics.setPosition(entity=self: Int, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setPosition · hand |
| `physics.setPositionAndReset(entity=self: Int, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setPositionAndReset · hand |
| `physics.hasPhysics(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#physics-hasPhysics · hand |
| `physics.addJoint(entityA: Int, entityB: Int, type: Int, anchorA: Vec3, anchorB: Vec3, axis: Vec3, minLimit: Float, maxLimit: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-addJoint · hand |
| `physics.removeJoint(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#physics-removeJoint · hand |
| `physics.hasJoint(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#physics-hasJoint · hand |
| `physics.setJointMotor(entity=self: Int, targetSpeed: Float, maxForce: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointMotor · hand |
| `physics.setJointBreakForce(entity=self: Int, breakForce: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointBreakForce · hand |
| `physics.setJointCollideConnected(entity=self: Int, collide: Bool)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointCollideConnected · hand |
| `physics.pollJointBroken()` → entitiesA: Int[], entitiesB: Int[] | exec | ref | scripting-reference.html#physics-pollJointBroken · hand |

### `math` — Math (31)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `math.sin(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-sin · hand |
| `math.cos(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-cos · hand |
| `math.tan(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-tan · hand |
| `math.sqrt(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-sqrt · hand |
| `math.abs(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-abs · hand |
| `math.floor(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-floor · hand |
| `math.ceil(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-ceil · hand |
| `math.round(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-round · hand |
| `math.sign(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-sign · hand |
| `math.radians(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-radians · hand |
| `math.degrees(x: Float)` → result: Float | pure | ref | scripting-reference.html#math-degrees · hand |
| `math.pow(base: Float, exp: Float)` → result: Float | pure | ref | scripting-reference.html#math-pow · hand |
| `math.mod(a: Float, b: Float)` → result: Float | pure | ref | scripting-reference.html#math-mod · hand |
| `math.bitAnd(a: Int, b: Int)` → result: Int | pure | ref | scripting-reference.html#math-bitAnd · hand |
| `math.bitOr(a: Int, b: Int)` → result: Int | pure | ref | scripting-reference.html#math-bitOr · hand |
| `math.bitXor(a: Int, b: Int)` → result: Int | pure | ref | scripting-reference.html#math-bitXor · hand |
| `math.bitNot(x: Int)` → result: Int | pure | ref | scripting-reference.html#math-bitNot · hand |
| `math.shiftLeft(x: Int, count: Int)` → result: Int | pure | ref | scripting-reference.html#math-shiftLeft · hand |
| `math.shiftRight(x: Int, count: Int)` → result: Int | pure | ref | scripting-reference.html#math-shiftRight · hand |
| `math.atan2(y: Float, x: Float)` → result: Float | pure | ref | scripting-reference.html#math-atan2 · hand |
| `math.min(a: Float, b: Float)` → result: Float | pure | ref | scripting-reference.html#math-min · hand |
| `math.max(a: Float, b: Float)` → result: Float | pure | ref | scripting-reference.html#math-max · hand |
| `math.clamp(x: Float, lo: Float, hi: Float)` → result: Float | pure | ref | scripting-reference.html#math-clamp · hand |
| `math.lerp(a: Float, b: Float, t: Float)` → result: Float | pure | ref | scripting-reference.html#math-lerp · hand |
| `math.length(v: Vec2)` → result: Float | pure | ref | scripting-reference.html#math-length · hand |
| `math.distance(a: Vec2, b: Vec2)` → result: Float | pure | ref | scripting-reference.html#math-distance · hand |
| `math.length3(v: Vec3)` → result: Float | pure | ref | scripting-reference.html#math-length3 · hand |
| `math.distance3(a: Vec3, b: Vec3)` → result: Float | pure | ref | scripting-reference.html#math-distance3 · hand |
| `math.normalize3(v: Vec3)` → result: Vec3 | pure | ref | scripting-reference.html#math-normalize3 · hand |
| `math.dot3(a: Vec3, b: Vec3)` → result: Float | pure | ref | scripting-reference.html#math-dot3 · hand |
| `math.cross(a: Vec3, b: Vec3)` → result: Vec3 | pure | ref | scripting-reference.html#math-cross · hand |

### `widget` — Widget (28)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `widget.setZOrder(widget: Ref, z: Int)` | exec | ref | scripting-reference.html#widget-setZOrder · hand |
| `widget.isVisible(widget: Ref)` → visible: Bool | pure | ref | scripting-reference.html#widget-isVisible · hand |
| `widget.callFunction(widget: Ref, function: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-callFunction · hand |
| `widget.addChild(widget: Ref, parent: String, widgetAsset: String)` → child: Ref | exec | ref | scripting-reference.html#widget-addChild · hand |
| `widget.removeChild(widget: Ref, child: Ref)` → ok: Bool | exec | ref | scripting-reference.html#widget-removeChild · hand |
| `widget.clearChildren(widget: Ref, parent: String)` → removed: Int | exec | ref | scripting-reference.html#widget-clearChildren · hand |
| `widget.setListCount(widget: Ref, list: String, count: Int)` → ok: Bool | exec | ref | scripting-reference.html#widget-setListCount · hand |
| `widget.listCount(widget: Ref, list: String)` → count: Int | pure | ref | scripting-reference.html#widget-listCount · hand |
| `widget.listRow(widget: Ref, list: String, index: Int)` → row: Ref | pure | ref | scripting-reference.html#widget-listRow · hand |
| `widget.refreshList(widget: Ref, list: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-refreshList · hand |
| `widget.setListSelected(widget: Ref, list: String, index: Int, selected: Bool)` → ok: Bool | exec | ref | scripting-reference.html#widget-setListSelected · hand |
| `widget.listSelected(widget: Ref, list: String)` → index: Int | pure | ref | scripting-reference.html#widget-listSelected · hand |
| `widget.scrollListToItem(widget: Ref, list: String, index: Int)` → ok: Bool | exec | ref | scripting-reference.html#widget-scrollListToItem · hand |
| `widget.animate(widget=self: Ref, element: String, property: String, to: Float, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animate · hand |
| `widget.animateColor(widget=self: Ref, element: String, property: String, to: Color, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animateColor · hand |
| `widget.animateVec2(widget=self: Ref, element: String, property: String, to: Vec2, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animateVec2 · hand |
| `widget.stopAnimation(widget=self: Ref, element: String, property: String)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAnimation · hand |
| `widget.playAnimation(widget=self: Ref, animation: String, restoreAfterCompleted: Bool, direction: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-playAnimation · hand |
| `widget.playAnimationLooped(widget=self: Ref, animation: String, loop: Bool, direction: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-playAnimationLooped · hand |
| `widget.stopAnimationClip(widget=self: Ref, animation: String)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAnimationClip · hand |
| `widget.isPlayingAnimation(widget=self: Ref, animation: String)` → playing: Bool | pure | ref | scripting-reference.html#widget-isPlayingAnimation · hand |
| `widget.childRef(widget=self: Ref, element: String)` → child: Ref | pure | ref | scripting-reference.html#widget-childRef · hand |
| `widget.stopAllAnimations(widget=self: Ref)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAllAnimations · hand |
| `widget.restoreOriginalState(widget=self: Ref)` → restored: Int | exec | ref | scripting-reference.html#widget-restoreOriginalState · hand |
| `widget.showModal(widget: Ref)` | exec | ref | scripting-reference.html#widget-showModal · hand |
| `widget.openPopup(widget: Ref, x: Float, y: Float)` | exec | ref | scripting-reference.html#widget-openPopup · hand |
| `widget.openPopupAtPointer(widget: Ref)` | exec | ref | scripting-reference.html#widget-openPopupAtPointer · hand |
| `widget.closeTopLayer()` → closed: Bool | exec | ref | scripting-reference.html#widget-closeTopLayer · hand |

### `camera` — Camera (26)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `camera.getPosition()` → position: Vec3 | pure | ref | scripting-reference.html#camera-getPosition · hand |
| `camera.setPosition(position: Vec3)` | exec | ref | scripting-reference.html#camera-setPosition · hand |
| `camera.getRotation()` → rotation: Vec3 | pure | ref | scripting-reference.html#camera-getRotation · hand |
| `camera.setRotation(rotation: Vec3)` | exec | ref | scripting-reference.html#camera-setRotation · hand |
| `camera.getFov()` → degrees: Float | pure | ref | scripting-reference.html#camera-getFov · hand |
| `camera.setFov(degrees: Float)` | exec | ref | scripting-reference.html#camera-setFov · hand |
| `camera.setRigMode(mode: Int)` | exec | ref | scripting-reference.html#camera-setRigMode · hand |
| `camera.getRigMode()` → mode: Int | pure | ref | scripting-reference.html#camera-getRigMode · hand |
| `camera.setRigTarget(entity=self: Int)` | exec | ref | scripting-reference.html#camera-setRigTarget · hand |
| `camera.setArmLength(length: Float)` | exec | ref | scripting-reference.html#camera-setArmLength · hand |
| `camera.getArmLength()` → length: Float | pure | ref | scripting-reference.html#camera-getArmLength · hand |
| `camera.setTargetYawMode(mode: Int)` | exec | ref | scripting-reference.html#camera-setTargetYawMode · hand |
| `camera.getTargetYawMode()` → mode: Int | pure | ref | scripting-reference.html#camera-getTargetYawMode · hand |
| `camera.getRigYaw()` → degrees: Float | pure | ref | scripting-reference.html#camera-getRigYaw · hand |
| `camera.getRigPitch()` → degrees: Float | pure | ref | scripting-reference.html#camera-getRigPitch · hand |
| `camera.addYawPitch(deltaYaw: Float, deltaPitch: Float)` | exec | ref | scripting-reference.html#camera-addYawPitch · hand |
| `camera.setLagEnabled(enabled: Bool)` | exec | ref | scripting-reference.html#camera-setLagEnabled · hand |
| `camera.getLagEnabled()` → enabled: Bool | pure | ref | scripting-reference.html#camera-getLagEnabled · hand |
| `camera.setLagSpeeds(position: Float, rotation: Float)` | exec | ref | scripting-reference.html#camera-setLagSpeeds · hand |
| `camera.snapRig()` | exec | ref | scripting-reference.html#camera-snapRig · hand |
| `camera.playShake(positionAmplitude: Float, rotationAmplitude: Float, frequency: Float, duration: Float)` → handle: Int | exec | ref | scripting-reference.html#camera-playShake · hand |
| `camera.stopShake(handle: Int)` | exec | ref | scripting-reference.html#camera-stopShake · hand |
| `camera.stopAllShakes()` | exec | ref | scripting-reference.html#camera-stopAllShakes · hand |
| `camera.kickFov(degrees: Float, attack: Float, hold: Float, decay: Float)` | exec | ref | scripting-reference.html#camera-kickFov · hand |
| `camera.blendTo(camera: Int, seconds: Float, curve: Int)` | exec | ref | scripting-reference.html#camera-blendTo · hand |
| `camera.isBlending()` → blending: Bool | pure | ref | scripting-reference.html#camera-isBlending · hand |

### `app` — App (24)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `app.quit()` | exec | ref | scripting-reference.html#app-quit · hand |
| `app.setTitle(title: String)` | exec | ref | scripting-reference.html#app-setTitle · hand |
| `app.setSize(width: Int, height: Int)` | exec | ref | scripting-reference.html#app-setSize · hand |
| `app.size()` → size: Vec2 | pure | ref | scripting-reference.html#app-size · hand |
| `app.requestRedraw()` | exec | ref | scripting-reference.html#app-requestRedraw · hand |
| `app.minimize()` | exec | ref | scripting-reference.html#app-minimize · hand |
| `app.maximize(maximized: Bool)` | exec | ref | scripting-reference.html#app-maximize · hand |
| `app.isMaximized()` → maximized: Bool | pure | ref | scripting-reference.html#app-isMaximized · hand |
| `app.showTray(tooltip: String)` | exec | ref | scripting-reference.html#app-showTray · hand |
| `app.hideTray()` | exec | ref | scripting-reference.html#app-hideTray · hand |
| `app.addTrayItem(id: String, label: String)` | exec | ref | scripting-reference.html#app-addTrayItem · hand |
| `app.clearTrayMenu()` | exec | ref | scripting-reference.html#app-clearTrayMenu · hand |
| `app.addMenu(id: String, label: String)` | exec | ref | scripting-reference.html#app-addMenu · hand |
| `app.addMenuItem(menu: String, id: String, label: String, shortcut: String)` | exec | ref | scripting-reference.html#app-addMenuItem · hand |
| `app.addMenuSeparator(menu: String)` | exec | ref | scripting-reference.html#app-addMenuSeparator · hand |
| `app.clearMenuBar()` | exec | ref | scripting-reference.html#app-clearMenuBar · hand |
| `app.setMenuItemEnabled(id: String, enabled: Bool)` | exec | ref | scripting-reference.html#app-setMenuItemEnabled · hand |
| `app.setMenuItemChecked(id: String, checked: Bool)` | exec | ref | scripting-reference.html#app-setMenuItemChecked · hand |
| `app.menuItemEnabled(id: String)` → enabled: Bool | pure | ref | scripting-reference.html#app-menuItemEnabled · hand |
| `app.menuItemChecked(id: String)` → checked: Bool | pure | ref | scripting-reference.html#app-menuItemChecked · hand |
| `app.notify(title: String, text: String)` → shown: Bool | exec | ref | scripting-reference.html#app-notify · hand |
| `app.notifyAvailable()` → available: Bool | pure | ref | scripting-reference.html#app-notifyAvailable · hand |
| `app.setAutostart(enabled: Bool)` | exec | ref | scripting-reference.html#app-setAutostart · hand |
| `app.autostart()` → enabled: Bool | pure | ref | scripting-reference.html#app-autostart · hand |

### `audio` — Audio (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `audio.play(asset: String, volume: Float, pitch: Float, loop: Bool)` → handle: Int | exec | ref | scripting-reference.html#audio-play · hand |
| `audio.playAt(asset: String, position: Vec3, volume: Float, pitch: Float, loop: Bool, minDist: Float, maxDist: Float)` → handle: Int | exec | ref | scripting-reference.html#audio-playAt · hand |
| `audio.stop(handle: Int)` | exec | ref | scripting-reference.html#audio-stop · hand |
| `audio.stopAll()` | exec | ref | scripting-reference.html#audio-stopAll · hand |
| `audio.isPlaying(handle: Int)` → playing: Bool | pure | ref | scripting-reference.html#audio-isPlaying · hand |
| `audio.setBusVolume(bus: String, volume: Float)` | exec | ref | scripting-reference.html#audio-setBusVolume · hand |
| `audio.setSoundPosition(handle: Int, position: Vec3)` | exec | ref | scripting-reference.html#audio-setSoundPosition · hand |
| `audio.pause(handle: Int)` | exec | ref | scripting-reference.html#audio-pause · hand |
| `audio.resume(handle: Int)` | exec | ref | scripting-reference.html#audio-resume · hand |
| `audio.isPaused(handle: Int)` → paused: Bool | pure | ref | scripting-reference.html#audio-isPaused · hand |
| `audio.setVolume(handle: Int, volume: Float)` | exec | ref | scripting-reference.html#audio-setVolume · hand |
| `audio.getVolume(handle: Int)` → volume: Float | pure | ref | scripting-reference.html#audio-getVolume · hand |
| `audio.setPitch(handle: Int, pitch: Float)` | exec | ref | scripting-reference.html#audio-setPitch · hand |
| `audio.getPitch(handle: Int)` → pitch: Float | pure | ref | scripting-reference.html#audio-getPitch · hand |
| `audio.setLooping(handle: Int, loop: Bool)` | exec | ref | scripting-reference.html#audio-setLooping · hand |
| `audio.seek(handle: Int, seconds: Float)` | exec | ref | scripting-reference.html#audio-seek · hand |
| `audio.getTime(handle: Int)` → seconds: Float | pure | ref | scripting-reference.html#audio-getTime · hand |
| `audio.getLength(handle: Int)` → seconds: Float | pure | ref | scripting-reference.html#audio-getLength · hand |

### `entity` — Entity (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `entity.getName(entity=self: Int)` → name: String | pure | ref | scripting-reference.html#entity-getName · hand |
| `entity.spawn(parent: Int, name: String)` → entity: Int | exec | ref | scripting-reference.html#entity-spawn · hand |
| `entity.destroy(entity: Int)` | exec | ref | scripting-reference.html#entity-destroy · hand |
| `entity.spawnClass(class: String, x: Float, y: Float, z: Float)` → entity: Int | exec | ref | scripting-reference.html#entity-spawnClass · hand |
| `entity.spawnClassRotated(class: String, x: Float, y: Float, z: Float, rx: Float, ry: Float, rz: Float)` → entity: Int | exec | ref | scripting-reference.html#entity-spawnClassRotated · hand |
| `entity.destroyObject(object: Ref)` | exec | ref | scripting-reference.html#entity-destroyObject · hand |
| `entity.self()` → entity: Int | pure | ref | scripting-reference.html#entity-self · hand |
| `entity.selfObject()` → object: Ref | pure | ref | scripting-reference.html#entity-selfObject · hand |
| `entity.instance(entity=self: Int)` → object: Ref | pure | ref | scripting-reference.html#entity-instance · hand |
| `entity.owned(object: Ref)` → entity: Int | pure | ref | scripting-reference.html#entity-owned · hand |
| `entity.distance(a: Int, b: Int)` → distance: Float | pure | ref | scripting-reference.html#entity-distance · hand |
| `entity.findByName(name: String)` → entity: Int | pure | ref | scripting-reference.html#entity-findByName · hand |
| `entity.exists(entity: Int)` → exists: Bool | pure | ref | scripting-reference.html#entity-exists · hand |
| `entity.setVisible(entity=self: Int, visible: Bool)` | exec | ref | scripting-reference.html#entity-setVisible · hand |
| `entity.saveState(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#entity-saveState · hand |
| `entity.hasSavedState(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#entity-hasSavedState · hand |
| `entity.applySavedState(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#entity-applySavedState · hand |
| `entity.getVisible(entity=self: Int)` → visible: Bool | pure | ref | scripting-reference.html#entity-getVisible · hand |

### `input` — Input (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `input.keyDown(key: String)` → down: Bool | pure | ref | scripting-reference.html#input-keyDown · hand |
| `input.mouseButton(button: Int)` → down: Bool | pure | ref | scripting-reference.html#input-mouseButton · hand |
| `input.mousePosition()` → position: Vec2 | pure | ref | scripting-reference.html#input-mousePosition · hand |
| `input.mouseDelta()` → delta: Vec2 | pure | ref | scripting-reference.html#input-mouseDelta · hand |
| `input.scrollDelta()` → scroll: Float | pure | ref | scripting-reference.html#input-scrollDelta · hand |
| `input.gamepadConnected()` → connected: Bool | pure | ref | scripting-reference.html#input-gamepadConnected · hand |
| `input.gamepadButton(button: String)` → down: Bool | pure | ref | scripting-reference.html#input-gamepadButton · hand |
| `input.gamepadAxis(axis: String)` → value: Float | pure | ref | scripting-reference.html#input-gamepadAxis · hand |
| `input.actionDown(action: String)` → down: Bool | pure | ref | scripting-reference.html#input-actionDown · hand |
| `input.actionPressed(action: String)` → pressed: Bool | pure | ref | scripting-reference.html#input-actionPressed · hand |
| `input.actionReleased(action: String)` → released: Bool | pure | ref | scripting-reference.html#input-actionReleased · hand |
| `input.actionAxis(action: String)` → value: Float | pure | ref | scripting-reference.html#input-actionAxis · hand |
| `input.actionAxis2D(action: String)` → value: Vec2 | pure | ref | scripting-reference.html#input-actionAxis2D · hand |
| `input.setModeGameOnly()` | exec | ref | scripting-reference.html#input-setModeGameOnly · hand |
| `input.setModeGameAndUI()` | exec | ref | scripting-reference.html#input-setModeGameAndUI · hand |
| `input.setModeUIOnly()` | exec | ref | scripting-reference.html#input-setModeUIOnly · hand |
| `input.mode()` → mode: String | pure | ref | scripting-reference.html#input-mode · hand |

### `save` — Save (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `save.create(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-create · hand |
| `save.load(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-load · hand |
| `save.write()` → ok: Bool | exec | ref | scripting-reference.html#save-write · hand |
| `save.close()` | exec | ref | scripting-reference.html#save-close · hand |
| `save.activeId()` → id: String | pure | ref | scripting-reference.html#save-activeId · hand |
| `save.list()` → ids: String[] | pure | ref | scripting-reference.html#save-list · hand |
| `save.exists(id: String)` → exists: Bool | pure | ref | scripting-reference.html#save-exists · hand |
| `save.delete(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-delete · hand |
| `save.fields()` → names: String[] | pure | ref | scripting-reference.html#save-fields · hand |
| `save.setNumber(field: String, value: Float)` → ok: Bool | exec | ref | scripting-reference.html#save-setNumber · hand |
| `save.getNumber(field: String, default: Float)` → value: Float | pure | ref | scripting-reference.html#save-getNumber · hand |
| `save.setString(field: String, value: String)` → ok: Bool | exec | ref | scripting-reference.html#save-setString · hand |
| `save.getString(field: String, default: String)` → value: String | pure | ref | scripting-reference.html#save-getString · hand |
| `save.setBool(field: String, value: Bool)` → ok: Bool | exec | ref | scripting-reference.html#save-setBool · hand |
| `save.getBool(field: String, default: Bool)` → value: Bool | pure | ref | scripting-reference.html#save-getBool · hand |
| `save.setStruct(field: String, value: Struct)` → ok: Bool | exec | ref | scripting-reference.html#save-setStruct · hand |
| `save.getStruct(field: String)` → value: Struct | pure | ref | scripting-reference.html#save-getStruct · hand |

### `anticheat` — AntiCheat (15)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `anticheat.check(rule: String, value: Float, player: Int)` → ok: Bool | exec | ref | scripting-reference.html#anticheat-check · hand |
| `anticheat.expectDisplacement(entity=self: Int, maxDistance: Float)` | exec | ref | scripting-reference.html#anticheat-expectDisplacement · hand |
| `anticheat.report(player: Int, rule: String, weight: Float, detail: String)` | exec | ref | scripting-reference.html#anticheat-report · hand |
| `anticheat.setPlayerLabel(player: Int, label: String)` | exec | ref | scripting-reference.html#anticheat-setPlayerLabel · hand |
| `anticheat.respond(reportId: Int, response: Int)` | exec | ref | scripting-reference.html#anticheat-respond · hand |
| `anticheat.kick(player: Int, reasonCode: Int)` | exec | ref | scripting-reference.html#anticheat-kick · hand |
| `anticheat.reportLevel(reportId: Int)` → level: Int | pure | ref | scripting-reference.html#anticheat-reportLevel · hand |
| `anticheat.reportRule(reportId: Int)` → rule: String | pure | ref | scripting-reference.html#anticheat-reportRule · hand |
| `anticheat.reportPlayer(reportId: Int)` → player: Int | pure | ref | scripting-reference.html#anticheat-reportPlayer · hand |
| `anticheat.reportEntity(reportId: Int)` → entity: Int | pure | ref | scripting-reference.html#anticheat-reportEntity · hand |
| `anticheat.reportScore(reportId: Int)` → score: Float | pure | ref | scripting-reference.html#anticheat-reportScore · hand |
| `anticheat.reportDetail(reportId: Int)` → detail: String | pure | ref | scripting-reference.html#anticheat-reportDetail · hand |
| `anticheat.reportReason(reportId: Int)` → reasonCode: Int | pure | ref | scripting-reference.html#anticheat-reportReason · hand |
| `anticheat.playerScore(player: Int)` → score: Float | pure | ref | scripting-reference.html#anticheat-playerScore · hand |
| `anticheat.isEnabled()` → enabled: Bool | pure | ref | scripting-reference.html#anticheat-isEnabled · hand |

### `fs` — File (13)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `fs.writeText(path: String, text: String)` → ok: Bool | exec | ref | scripting-reference.html#fs-writeText · hand |
| `fs.readText(path: String)` → text: String | exec | ref | scripting-reference.html#fs-readText · hand |
| `fs.exists(path: String)` → exists: Bool | pure | ref | scripting-reference.html#fs-exists · hand |
| `fs.remove(path: String)` → ok: Bool | exec | ref | scripting-reference.html#fs-remove · hand |
| `fs.makeDir(path: String)` → ok: Bool | exec | ref | scripting-reference.html#fs-makeDir · hand |
| `fs.isDir(path: String)` → isDir: Bool | pure | ref | scripting-reference.html#fs-isDir · hand |
| `fs.size(path: String)` → bytes: Float | pure | ref | scripting-reference.html#fs-size · hand |
| `fs.modified(path: String)` → time: Float | pure | ref | scripting-reference.html#fs-modified · hand |
| `fs.list(dir: String)` → names: String[] | pure | ref | scripting-reference.html#fs-list · hand |
| `fs.rename(from: String, to: String)` → ok: Bool | exec | ref | scripting-reference.html#fs-rename · hand |
| `fs.copy(from: String, to: String)` → ok: Bool | exec | ref | scripting-reference.html#fs-copy · hand |
| `fs.watch(path: String)` → handle: Int | exec | ref | scripting-reference.html#fs-watch · hand |
| `fs.unwatch(handle: Int)` | exec | ref | scripting-reference.html#fs-unwatch · hand |

### `time` — Time (13)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `time.deltaTime()` → dt: Float | pure | ref | scripting-reference.html#time-deltaTime · hand |
| `time.elapsed()` → seconds: Float | pure | ref | scripting-reference.html#time-elapsed · hand |
| `time.frameCount()` → frame: Int | pure | ref | scripting-reference.html#time-frameCount · hand |
| `time.setTimeScale(scale: Float)` | exec | ref | scripting-reference.html#time-setTimeScale · hand |
| `time.timeScale()` → scale: Float | pure | ref | scripting-reference.html#time-timeScale · hand |
| `time.unscaledDeltaTime()` → dt: Float | pure | ref | scripting-reference.html#time-unscaledDeltaTime · hand |
| `time.unscaledElapsed()` → seconds: Float | pure | ref | scripting-reference.html#time-unscaledElapsed · hand |
| `time.pause()` | exec | ref | scripting-reference.html#time-pause · hand |
| `time.resume()` | exec | ref | scripting-reference.html#time-resume · hand |
| `time.isPaused()` → paused: Bool | pure | ref | scripting-reference.html#time-isPaused · hand |
| `time.hitStop(seconds: Float)` | exec | ref | scripting-reference.html#time-hitStop · hand |
| `time.isFrozen()` → frozen: Bool | pure | ref | scripting-reference.html#time-isFrozen · hand |
| `time.effectiveScale()` → scale: Float | pure | ref | scripting-reference.html#time-effectiveScale · hand |

### `scene` — Scene (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `scene.load(scene: String, hidden: Bool)` | exec | ref | scripting-reference.html#scene-load · hand |
| `scene.loadAdditive(scene: String, hidden: Bool, position: Color)` → zone: Int | exec | ref | scripting-reference.html#scene-loadAdditive · hand |
| `scene.unloadZone(zone: Int)` | exec | ref | scripting-reference.html#scene-unloadZone · hand |
| `scene.activate()` | exec | ref | scripting-reference.html#scene-activate · hand |
| `scene.hasPendingLevel()` → pending: Bool | pure | ref | scripting-reference.html#scene-hasPendingLevel · hand |
| `scene.showZone(zone: Int)` | exec | ref | scripting-reference.html#scene-showZone · hand |
| `scene.hideZone(zone: Int)` | exec | ref | scripting-reference.html#scene-hideZone · hand |
| `scene.zonePosition(zone: Int)` → position: Vec3 | pure | ref | scripting-reference.html#scene-zonePosition · hand |
| `scene.setZonePosition(zone: Int, position: Vec3)` | exec | ref | scripting-reference.html#scene-setZonePosition · hand |
| `scene.zoneScene(zone: Int)` → scene: String | pure | ref | scripting-reference.html#scene-zoneScene · hand |
| `scene.loadedZones()` → zones: Int[] | pure | ref | scripting-reference.html#scene-loadedZones · hand |
| `scene.available()` → scenes: String[] | pure | ref | scripting-reference.html#scene-available · hand |

### `string` — String (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `string.length(s: String)` → length: Int | pure | ref | scripting-reference.html#string-length · hand |
| `string.equals(a: String, b: String)` → result: Bool | pure | ref | scripting-reference.html#string-equals · hand |
| `string.substring(s: String, start: Int, count: Int)` → result: String | pure | ref | scripting-reference.html#string-substring · hand |
| `string.contains(s: String, needle: String)` → contains: Bool | pure | ref | scripting-reference.html#string-contains · hand |
| `string.find(s: String, needle: String)` → index: Int | pure | ref | scripting-reference.html#string-find · hand |
| `string.replace(s: String, from: String, to: String)` → result: String | pure | ref | scripting-reference.html#string-replace · hand |
| `string.toUpper(s: String)` → result: String | pure | ref | scripting-reference.html#string-toUpper · hand |
| `string.toLower(s: String)` → result: String | pure | ref | scripting-reference.html#string-toLower · hand |
| `string.trim(s: String)` → result: String | pure | ref | scripting-reference.html#string-trim · hand |
| `string.startsWith(s: String, prefix: String)` → result: Bool | pure | ref | scripting-reference.html#string-startsWith · hand |
| `string.endsWith(s: String, suffix: String)` → result: Bool | pure | ref | scripting-reference.html#string-endsWith · hand |
| `string.toNumber(s: String)` → number: Float | pure | ref | scripting-reference.html#string-toNumber · hand |

### `ui` — UI (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `ui.getText(entity=self: Int)` → text: String | pure | ref | scripting-reference.html#ui-getText · hand |
| `ui.setText(entity=self: Int, text: String)` | exec | ref | scripting-reference.html#ui-setText · hand |
| `ui.getColor(entity=self: Int)` → color: Color | pure | ref | scripting-reference.html#ui-getColor · hand |
| `ui.setColor(entity=self: Int, color: Color)` | exec | ref | scripting-reference.html#ui-setColor · hand |
| `ui.getVisible(entity=self: Int)` → visible: Bool | pure | ref | scripting-reference.html#ui-getVisible · hand |
| `ui.setVisible(entity=self: Int, visible: Bool)` | exec | ref | scripting-reference.html#ui-setVisible · hand |
| `ui.getPosition(entity=self: Int)` → position: Vec2 | pure | ref | scripting-reference.html#ui-getPosition · hand |
| `ui.setPosition(entity=self: Int, position: Vec2)` | exec | ref | scripting-reference.html#ui-setPosition · hand |
| `ui.getSize(entity=self: Int)` → size: Vec2 | pure | ref | scripting-reference.html#ui-getSize · hand |
| `ui.setSize(entity=self: Int, size: Vec2)` | exec | ref | scripting-reference.html#ui-setSize · hand |
| `ui.setMaterialParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | ref | scripting-reference.html#ui-setMaterialParam · hand |
| `ui.pointerOverUI()` → over: Bool | pure | ref | scripting-reference.html#ui-pointerOverUI · hand |

### `datetime` — DateTime (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `datetime.now()` → epochSeconds: Float | pure | ref | scripting-reference.html#datetime-now · hand |
| `datetime.format(epochSeconds: Float, format: String)` → text: String | pure | ref | scripting-reference.html#datetime-format · hand |
| `datetime.year(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-year · hand |
| `datetime.month(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-month · hand |
| `datetime.day(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-day · hand |
| `datetime.hour(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-hour · hand |
| `datetime.minute(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-minute · hand |
| `datetime.second(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-second · hand |
| `datetime.weekday(epochSeconds: Float)` → value: Int | pure | ref | scripting-reference.html#datetime-weekday · hand |

### `http` — HTTP (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `http.get(url: String)` → ticket: Int | exec | ref | scripting-reference.html#http-get · hand |
| `http.post(url: String, contentType: String, body: String)` → ticket: Int | exec | ref | scripting-reference.html#http-post · hand |
| `http.done(ticket: Int)` → done: Bool | pure | ref | scripting-reference.html#http-done · hand |
| `http.ok(ticket: Int)` → ok: Bool | pure | ref | scripting-reference.html#http-ok · hand |
| `http.status(ticket: Int)` → status: Int | pure | ref | scripting-reference.html#http-status · hand |
| `http.body(ticket: Int)` → body: String | pure | ref | scripting-reference.html#http-body · hand |
| `http.error(ticket: Int)` → error: String | pure | ref | scripting-reference.html#http-error · hand |
| `http.forget(ticket: Int)` | exec | ref | scripting-reference.html#http-forget · hand |
| `http.available()` → available: Bool | pure | ref | scripting-reference.html#http-available · hand |

### `prefs` — Prefs (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `prefs.getString(key: String, fallback: String)` → value: String | pure | ref | scripting-reference.html#prefs-getString · hand |
| `prefs.getNumber(key: String, fallback: Float)` → value: Float | pure | ref | scripting-reference.html#prefs-getNumber · hand |
| `prefs.getBool(key: String, fallback: Bool)` → value: Bool | pure | ref | scripting-reference.html#prefs-getBool · hand |
| `prefs.setString(key: String, value: String)` | exec | ref | scripting-reference.html#prefs-setString · hand |
| `prefs.setNumber(key: String, value: Float)` | exec | ref | scripting-reference.html#prefs-setNumber · hand |
| `prefs.setBool(key: String, value: Bool)` | exec | ref | scripting-reference.html#prefs-setBool · hand |
| `prefs.has(key: String)` → present: Bool | pure | ref | scripting-reference.html#prefs-has · hand |
| `prefs.remove(key: String)` → removed: Bool | exec | ref | scripting-reference.html#prefs-remove · hand |
| `prefs.clear()` | exec | ref | scripting-reference.html#prefs-clear · hand |

### `animator` — Animator (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `animator.setParam(entity=self: Int, name: String, value: Float)` | exec | ref | scripting-reference.html#animator-setParam · hand |
| `animator.getParam(entity=self: Int, name: String)` → value: Float | pure | ref | scripting-reference.html#animator-getParam · hand |
| `animator.getState(entity=self: Int)` → state: String | pure | ref | scripting-reference.html#animator-getState · hand |
| `animator.notifiesOf(clipPath: String)` → names: String[] | pure | ref | scripting-reference.html#animator-notifiesOf · hand |
| `animator.setLayerWeight(entity=self: Int, layer: String, weight: Float)` | exec | ref | scripting-reference.html#animator-setLayerWeight · hand |
| `animator.getLayerWeight(entity=self: Int, layer: String)` → weight: Float | pure | ref | scripting-reference.html#animator-getLayerWeight · hand |
| `animator.playLayer(entity=self: Int, layer: String)` | exec | ref | scripting-reference.html#animator-playLayer · hand |
| `animator.layerNames(entity=self: Int)` → names: String[] | pure | ref | scripting-reference.html#animator-layerNames · hand |

### `json` — JSON (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `json.getString(text: String, path: String, fallback: String)` → value: String | pure | ref | scripting-reference.html#json-getString · hand |
| `json.getNumber(text: String, path: String, fallback: Float)` → value: Float | pure | ref | scripting-reference.html#json-getNumber · hand |
| `json.getBool(text: String, path: String, fallback: Bool)` → value: Bool | pure | ref | scripting-reference.html#json-getBool · hand |
| `json.has(text: String, path: String)` → present: Bool | pure | ref | scripting-reference.html#json-has · hand |
| `json.count(text: String, path: String)` → count: Int | pure | ref | scripting-reference.html#json-count · hand |
| `json.setString(text: String, path: String, value: String)` → result: String | pure | ref | scripting-reference.html#json-setString · hand |
| `json.setNumber(text: String, path: String, value: Float)` → result: String | pure | ref | scripting-reference.html#json-setNumber · hand |
| `json.setBool(text: String, path: String, value: Bool)` → result: String | pure | ref | scripting-reference.html#json-setBool · hand |

### `transform` — Transform (8)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.transform.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `transform.getPosition(entity=self: Int)` → position: Vec3 | pure | ref | scripting-reference.html#transform-getPosition · hand |
| `transform.setPosition(entity=self: Int, position: Vec3)` | exec | ref | scripting-reference.html#transform-setPosition · hand |
| `transform.getRotation(entity=self: Int)` → rotation: Vec3 | pure | ref | scripting-reference.html#transform-getRotation · hand |
| `transform.setRotation(entity=self: Int, rotation: Vec3)` | exec | ref | scripting-reference.html#transform-setRotation · hand |
| `transform.getScale(entity=self: Int)` → scale: Vec3 | pure | ref | scripting-reference.html#transform-getScale · hand |
| `transform.setScale(entity=self: Int, scale: Vec3)` | exec | ref | scripting-reference.html#transform-setScale · hand |
| `transform.getWorldPosition(entity=self: Int)` → position: Vec3 | pure | ref | scripting-reference.html#transform-getWorldPosition · hand |
| `transform.setWorldPosition(entity=self: Int, position: Vec3)` | exec | ref | scripting-reference.html#transform-setWorldPosition · hand |

### `db` — Database (7)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `db.open(path: String)` → handle: Int | exec | ref | scripting-reference.html#db-open · hand |
| `db.close(handle: Int)` | exec | ref | scripting-reference.html#db-close · hand |
| `db.exec(handle: Int, sql: String, params: String)` → ok: Bool | exec | ref | scripting-reference.html#db-exec · hand |
| `db.query(handle: Int, sql: String, params: String)` → rows: String | exec | ref | scripting-reference.html#db-query · hand |
| `db.changes(handle: Int)` → rows: Int | pure | ref | scripting-reference.html#db-changes · hand |
| `db.lastInsertId(handle: Int)` → id: Int | pure | ref | scripting-reference.html#db-lastInsertId · hand |
| `db.lastError(handle: Int)` → error: String | pure | ref | scripting-reference.html#db-lastError · hand |

### `locomotion` — Locomotion (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `locomotion.move(entity=self: Int, direction: Vec3)` | exec | ref | scripting-reference.html#locomotion-move · hand |
| `locomotion.look(entity=self: Int, yaw: Float, pitch: Float)` | exec | ref | scripting-reference.html#locomotion-look · hand |
| `locomotion.setMaxSpeed(entity=self: Int, speed: Float)` | exec | ref | scripting-reference.html#locomotion-setMaxSpeed · hand |
| `locomotion.setOrientToMovement(entity=self: Int, on: Bool)` | exec | ref | scripting-reference.html#locomotion-setOrientToMovement · hand |
| `locomotion.jump(entity=self: Int)` → jumped: Bool | exec | ref | scripting-reference.html#locomotion-jump · hand |
| `locomotion.jumpWith(entity=self: Int, speed: Float)` → jumped: Bool | exec | ref | scripting-reference.html#locomotion-jumpWith · hand |

### `movement` — Movement (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `movement.speed(entity=self: Int)` → speed: Float | pure | ref | scripting-reference.html#movement-speed · hand |
| `movement.verticalSpeed(entity=self: Int)` → speed: Float | pure | ref | scripting-reference.html#movement-verticalSpeed · hand |
| `movement.isGrounded(entity=self: Int)` → grounded: Bool | pure | ref | scripting-reference.html#movement-isGrounded · hand |
| `movement.velocity(entity=self: Int)` → velocity: Vec3 | pure | ref | scripting-reference.html#movement-velocity · hand |
| `movement.forwardAmount(entity=self: Int)` → amount: Float | pure | ref | scripting-reference.html#movement-forwardAmount · hand |
| `movement.rightAmount(entity=self: Int)` → amount: Float | pure | ref | scripting-reference.html#movement-rightAmount · hand |

### `nav` — Navigation (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `nav.moveTo(entity=self: Int, x: Float, y: Float, z: Float)` → started: Bool | exec | ref | scripting-reference.html#nav-moveTo · hand |
| `nav.stop(entity=self: Int)` | exec | ref | scripting-reference.html#nav-stop · hand |
| `nav.isMoving(entity=self: Int)` → moving: Bool | pure | ref | scripting-reference.html#nav-isMoving · hand |
| `nav.hasPath(entity=self: Int)` → hasPath: Bool | pure | ref | scripting-reference.html#nav-hasPath · hand |
| `nav.remainingDistance(entity=self: Int)` → distance: Float | pure | ref | scripting-reference.html#nav-remainingDistance · hand |
| `nav.setSpeed(entity=self: Int, speed: Float)` | exec | ref | scripting-reference.html#nav-setSpeed · hand |

### `player` — Player (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `player.possess(controller: Ref, character: Ref)` | exec | ref | scripting-reference.html#player-possess · hand |
| `player.unpossess(controller: Ref)` | exec | ref | scripting-reference.html#player-unpossess · hand |
| `player.possessed(controller: Ref)` → character: Ref | pure | ref | scripting-reference.html#player-possessed · hand |
| `player.controllerOf(character: Ref)` → controller: Ref | pure | ref | scripting-reference.html#player-controllerOf · hand |
| `player.controller()` → controller: Ref | pure | ref | scripting-reference.html#player-controller · hand |
| `player.character()` → character: Ref | pure | ref | scripting-reference.html#player-character · hand |

### `theme` — Theme (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `theme.set(themeAsset: String)` → ok: Bool | exec | ref | scripting-reference.html#theme-set · hand |
| `theme.setMode(mode: String)` | exec | ref | scripting-reference.html#theme-setMode · hand |
| `theme.getMode()` → mode: String | pure | ref | scripting-reference.html#theme-getMode · hand |
| `theme.getPreference()` → preference: String | pure | ref | scripting-reference.html#theme-getPreference · hand |
| `theme.setFontScale(scale: Float)` | exec | ref | scripting-reference.html#theme-setFontScale · hand |
| `theme.getFontScale()` → scale: Float | pure | ref | scripting-reference.html#theme-getFontScale · hand |

### `dialog` — Dialog (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `dialog.message(title: String, text: String, kind: Int)` | exec | ref | scripting-reference.html#dialog-message · hand |
| `dialog.confirm(title: String, text: String, affirmative: String, negative: String)` → confirmed: Bool | exec | ref | scripting-reference.html#dialog-confirm · hand |
| `dialog.openFile(filter: String)` → path: String | exec | ref | scripting-reference.html#dialog-openFile · hand |
| `dialog.saveFile(filter: String)` → path: String | exec | ref | scripting-reference.html#dialog-saveFile · hand |
| `dialog.pickFolder()` → path: String | exec | ref | scripting-reference.html#dialog-pickFolder · hand |

### `random` — Random (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `random.seed(seed: Int)` | exec | ref | scripting-reference.html#random-seed · hand |
| `random.value()` → value: Float | exec | ref | scripting-reference.html#random-value · hand |
| `random.range(min: Float, max: Float)` → value: Float | exec | ref | scripting-reference.html#random-range · hand |
| `random.rangeInt(min: Int, max: Int)` → value: Int | exec | ref | scripting-reference.html#random-rangeInt · hand |
| `random.chance(p: Float)` → value: Bool | exec | ref | scripting-reference.html#random-chance · hand |

### `timer` — Timer (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `timer.after(seconds: Float)` → handle: Int | exec | ref | scripting-reference.html#timer-after · hand |
| `timer.every(seconds: Float)` → handle: Int | exec | ref | scripting-reference.html#timer-every · hand |
| `timer.cancel(handle: Int)` → ok: Bool | exec | ref | scripting-reference.html#timer-cancel · hand |
| `timer.active(handle: Int)` → active: Bool | pure | ref | scripting-reference.html#timer-active · hand |
| `timer.cancelAll()` | exec | ref | scripting-reference.html#timer-cancelAll · hand |

### `window` — Window (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `window.open(title: String, width: Int, height: Int)` → window: Int | exec | ref | scripting-reference.html#window-open · hand |
| `window.close(window: Int)` | exec | ref | scripting-reference.html#window-close · hand |
| `window.setTitle(window: Int, title: String)` | exec | ref | scripting-reference.html#window-setTitle · hand |
| `window.setSize(window: Int, width: Int, height: Int)` | exec | ref | scripting-reference.html#window-setSize · hand |
| `window.show(window: Int, widget: Ref)` | exec | ref | scripting-reference.html#window-show · hand |

### `content` — Content (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `content.load(asset: String)` → loaded: Bool | exec | ref | scripting-reference.html#content-load · hand |
| `content.unload(asset: String)` → unloaded: Bool | exec | ref | scripting-reference.html#content-unload · hand |
| `content.isLoaded(asset: String)` → loaded: Bool | pure | ref | scripting-reference.html#content-isLoaded · hand |
| `content.typeName(asset: String)` → type: String | pure | ref | scripting-reference.html#content-typeName · hand |

### `debug` — Debug (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `debug.line(from: Color, to: Color, color: Color, seconds: Float)` | exec | ref | scripting-reference.html#debug-line · hand |
| `debug.sphere(center: Color, radius: Float, color: Color, seconds: Float)` | exec | ref | scripting-reference.html#debug-sphere · hand |
| `debug.box(min: Color, max: Color, color: Color, seconds: Float)` | exec | ref | scripting-reference.html#debug-box · hand |
| `debug.clear()` | exec | ref | scripting-reference.html#debug-clear · hand |

### `particle` — Particles (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `particle.play(entity=self: Int)` | exec | ref | scripting-reference.html#particle-play · hand |
| `particle.stop(entity=self: Int)` | exec | ref | scripting-reference.html#particle-stop · hand |
| `particle.burst(entity=self: Int, count: Int)` → emitted: Int | exec | ref | scripting-reference.html#particle-burst · hand |
| `particle.isPlaying(entity=self: Int)` → playing: Bool | pure | ref | scripting-reference.html#particle-isPlaying · hand |

### `clipboard` — Clipboard (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `clipboard.getText()` → text: String | pure | ref | scripting-reference.html#clipboard-getText · hand |
| `clipboard.setText(text: String)` | exec | ref | scripting-reference.html#clipboard-setText · hand |
| `clipboard.hasText()` → has: Bool | pure | ref | scripting-reference.html#clipboard-hasText · hand |

### `print` — Print (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `print.toPdf(path: String, text: String, title: String)` → ok: Bool | exec | ref | scripting-reference.html#print-toPdf · hand |
| `print.file(path: String)` → ok: Bool | exec | ref | scripting-reference.html#print-file · hand |
| `print.available()` → available: Bool | pure | ref | scripting-reference.html#print-available · hand |

### `process` — Process (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `process.run(exe: String, args: String[], timeoutSeconds: Float)` → ok: Bool, exitCode: Int, out: String, err: String | exec | ref | scripting-reference.html#process-run · hand |
| `process.openUrl(url: String)` → ok: Bool | exec | ref | scripting-reference.html#process-openUrl · hand |
| `process.which(exe: String)` → path: String | pure | ref | scripting-reference.html#process-which · hand |

### `material` — Material (2)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.material.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `material.getParam(entity=self: Int, name: String)` → value: Color | pure | ref | scripting-reference.html#material-getParam · hand |
| `material.setParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | ref | scripting-reference.html#material-setParam · hand |

### `cursor` — Cursor (1)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.cursor.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `cursor.setVisible(show: Bool)` | exec | ref | scripting-reference.html#cursor-setVisible · hand |

### `log` — Debug (1)

> Id ohne Gruppe: in Lua/Python als flache Funktion `horizon.log` da.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `log(message: String)` | exec | ref | scripting-reference.html#fn-log · hand |
