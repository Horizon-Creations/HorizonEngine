# Script-API-Doku: Abdeckung je Registry-Id (generiert)

Erzeugt von `scripts/script_api_docs/coverage.py` aus `scripts/script_api_docs/registry.json` (Dump von `HE::api::registry()`) und der lokalen Website-Doku. Nicht von Hand pflegen, neu erzeugen. Einordnung und Folgeschritte: `docs/script-api-docs-gap-audit-2026-09-24.md`.

Status: **ref** = Signatur+Rückgabe dokumentiert (Zeile in der generierten Referenz `scripting-reference.html` oder flacher Zwilling in `scripting-api.html#api`), **named** = Id wörtlich auf einer anderen Doku-Seite, **catalog** = nur der Name im HorizonCode-Knotenkatalog, **missing** = nirgends. **hand** = es gibt Handinhalt aus `overlay/` (Gruppen-Einleitung oder zur Id eine Notiz, ein Beispiel, ein Recht, eine Skript-Signatur): das, was der Generator nicht schreiben kann, und die Zahl, die die Gruppen-Schritte bewegen.

**Gesamt: 582 Registry-Ids in 42 Gruppen** — ref 582, named 0, catalog 0, missing 0; hand 104.

| Gruppe | Kategorie | Ids | Lua/Py `horizon.<gruppe>.*` | ref | named | catalog | missing | hand |
|---|---|---:|:---:|---:|---:|---:|---:|---:|
| `env` | Environment | 116 | ja | 116 | 0 | 0 | 0 | 0 |
| `net` | Multiplayer | 44 | ja | 44 | 0 | 0 | 0 | 3 |
| `physics` | Physics | 33 | ja | 33 | 0 | 0 | 0 | 0 |
| `math` | Math | 31 | ja | 31 | 0 | 0 | 0 | 31 |
| `widget` | Widget | 28 | ja | 28 | 0 | 0 | 0 | 0 |
| `camera` | Camera | 26 | ja | 26 | 0 | 0 | 0 | 0 |
| `app` | App | 24 | ja | 24 | 0 | 0 | 0 | 1 |
| `audio` | Audio | 18 | ja | 18 | 0 | 0 | 0 | 0 |
| `entity` | Entity | 18 | ja | 18 | 0 | 0 | 0 | 0 |
| `input` | Input | 17 | ja | 17 | 0 | 0 | 0 | 0 |
| `save` | Save | 17 | ja | 17 | 0 | 0 | 0 | 0 |
| `anticheat` | AntiCheat | 15 | ja | 15 | 0 | 0 | 0 | 0 |
| `fs` | File | 13 | ja | 13 | 0 | 0 | 0 | 13 |
| `time` | Time | 13 | ja | 13 | 0 | 0 | 0 | 13 |
| `scene` | Scene | 12 | ja | 12 | 0 | 0 | 0 | 0 |
| `string` | String | 12 | ja | 12 | 0 | 0 | 0 | 12 |
| `ui` | UI | 12 | ja | 12 | 0 | 0 | 0 | 0 |
| `datetime` | DateTime | 9 | ja | 9 | 0 | 0 | 0 | 9 |
| `http` | HTTP | 9 | ja | 9 | 0 | 0 | 0 | 2 |
| `prefs` | Prefs | 9 | ja | 9 | 0 | 0 | 0 | 0 |
| `animator` | Animator | 8 | ja | 8 | 0 | 0 | 0 | 0 |
| `json` | JSON | 8 | ja | 8 | 0 | 0 | 0 | 0 |
| `transform` | Transform | 8 | **nein** | 8 | 0 | 0 | 0 | 0 |
| `db` | Database | 7 | ja | 7 | 0 | 0 | 0 | 1 |
| `locomotion` | Locomotion | 6 | ja | 6 | 0 | 0 | 0 | 0 |
| `movement` | Movement | 6 | ja | 6 | 0 | 0 | 0 | 0 |
| `nav` | Navigation | 6 | ja | 6 | 0 | 0 | 0 | 0 |
| `player` | Player | 6 | ja | 6 | 0 | 0 | 0 | 0 |
| `theme` | Theme | 6 | ja | 6 | 0 | 0 | 0 | 0 |
| `dialog` | Dialog | 5 | ja | 5 | 0 | 0 | 0 | 0 |
| `random` | Random | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `timer` | Timer | 5 | ja | 5 | 0 | 0 | 0 | 5 |
| `window` | Window | 5 | ja | 5 | 0 | 0 | 0 | 0 |
| `content` | Content | 4 | ja | 4 | 0 | 0 | 0 | 0 |
| `debug` | Debug | 4 | ja | 4 | 0 | 0 | 0 | 4 |
| `particle` | Particles | 4 | ja | 4 | 0 | 0 | 0 | 0 |
| `clipboard` | Clipboard | 3 | ja | 3 | 0 | 0 | 0 | 0 |
| `print` | Print | 3 | ja | 3 | 0 | 0 | 0 | 2 |
| `process` | Process | 3 | ja | 3 | 0 | 0 | 0 | 2 |
| `material` | Material | 2 | **nein** | 2 | 0 | 0 | 0 | 0 |
| `cursor` | Cursor | 1 | **nein** | 1 | 0 | 0 | 0 | 0 |
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
| `net.host(port: Int, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-host |
| `net.joinDirect(address: String, port: Int, code: String, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-joinDirect |
| `net.joinLan(index: Int, code: String, displayName: String)` → ok: Bool | exec | ref | scripting-reference.html#net-joinLan |
| `net.leave()` | exec | ref | scripting-reference.html#net-leave |
| `net.status()` → status: Int | pure | ref | scripting-reference.html#net-status |
| `net.lastError()` → error: String | pure | ref | scripting-reference.html#net-lastError |
| `net.sessionId()` → sessionId: String | pure | ref | scripting-reference.html#net-sessionId |
| `net.joinCode()` → code: String | pure | ref | scripting-reference.html#net-joinCode |
| `net.refreshLan()` | exec | ref | scripting-reference.html#net-refreshLan |
| `net.lanSessionCount()` → count: Int | pure | ref | scripting-reference.html#net-lanSessionCount |
| `net.lanSessionName(index: Int)` → name: String | pure | ref | scripting-reference.html#net-lanSessionName |
| `net.lanSessionPlayers(index: Int)` → players: Int | pure | ref | scripting-reference.html#net-lanSessionPlayers |
| `net.isAuthority()` → isAuthority: Bool | pure | ref | scripting-reference.html#net-isAuthority |
| `net.isClient()` → isClient: Bool | pure | ref | scripting-reference.html#net-isClient |
| `net.localPlayer()` → player: Int | pure | ref | scripting-reference.html#net-localPlayer |
| `net.playerCount()` → count: Int | pure | ref | scripting-reference.html#net-playerCount |
| `net.playerAt(index: Int)` → player: Int | pure | ref | scripting-reference.html#net-playerAt |
| `net.playerName(player: Int)` → name: String | pure | ref | scripting-reference.html#net-playerName |
| `net.ping(player: Int)` → ms: Float | pure | ref | scripting-reference.html#net-ping |
| `net.kick(player: Int)` | exec | ref | scripting-reference.html#net-kick |
| `net.ownerOf(entity=self: Int)` → player: Int | pure | ref | scripting-reference.html#net-ownerOf |
| `net.isLocallyControlled(entity=self: Int)` → local: Bool | pure | ref | scripting-reference.html#net-isLocallyControlled |
| `net.localCharacter()` → entity: Int | pure | ref | scripting-reference.html#net-localCharacter |
| `net.declareVarBool(entity=self: Int, name: String, initial: Bool, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarBool |
| `net.declareVarInt(entity=self: Int, name: String, initial: Int, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarInt |
| `net.declareVarFloat(entity=self: Int, name: String, initial: Float, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarFloat |
| `net.declareVarString(entity=self: Int, name: String, initial: String, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarString |
| `net.declareVarVec3(entity=self: Int, name: String, initial: Vec3, notify: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-declareVarVec3 |
| `net.setVarBool(entity=self: Int, name: String, value: Bool)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarBool |
| `net.setVarInt(entity=self: Int, name: String, value: Int)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarInt |
| `net.setVarFloat(entity=self: Int, name: String, value: Float)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarFloat |
| `net.setVarString(entity=self: Int, name: String, value: String)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarString |
| `net.setVarVec3(entity=self: Int, name: String, value: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#net-setVarVec3 |
| `net.getVarBool(entity=self: Int, name: String)` → value: Bool | pure | ref | scripting-reference.html#net-getVarBool |
| `net.getVarInt(entity=self: Int, name: String)` → value: Int | pure | ref | scripting-reference.html#net-getVarInt |
| `net.getVarFloat(entity=self: Int, name: String)` → value: Float | pure | ref | scripting-reference.html#net-getVarFloat |
| `net.getVarString(entity=self: Int, name: String)` → value: String | pure | ref | scripting-reference.html#net-getVarString |
| `net.getVarVec3(entity=self: Int, name: String)` → value: Vec3 | pure | ref | scripting-reference.html#net-getVarVec3 |
| `net.callServer(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callServer · hand |
| `net.callClient(player: Int, entity: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callClient · hand |
| `net.callAllClients(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-callAllClients · hand |
| `net.allowAnyClient(entity=self: Int, function: String)` → ok: Bool | exec | ref | scripting-reference.html#net-allowAnyClient |
| `net.rpcSender()` → player: Int | pure | ref | scripting-reference.html#net-rpcSender |
| `net.hasVar(entity=self: Int, name: String)` → declared: Bool | pure | ref | scripting-reference.html#net-hasVar |

### `physics` — Physics (33)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `physics.raycast(origin: Vec3, direction: Vec3, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-raycast |
| `physics.setVelocity(entity=self: Int, velocity: Vec3)` | exec | ref | scripting-reference.html#physics-setVelocity |
| `physics.isGrounded(entity=self: Int)` → grounded: Bool | pure | ref | scripting-reference.html#physics-isGrounded |
| `physics.sphereCast(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-sphereCast |
| `physics.overlapSphere(center: Vec3, radius: Float)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapSphere |
| `physics.raycastLayers(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-raycastLayers |
| `physics.sphereCastLayers(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-sphereCastLayers |
| `physics.overlapSphereLayers(center: Vec3, radius: Float, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapSphereLayers |
| `physics.boxCast(origin: Vec3, halfExtents: Vec3, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-boxCast |
| `physics.capsuleCast(origin: Vec3, radius: Float, height: Float, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | scripting-reference.html#physics-capsuleCast |
| `physics.overlapBox(center: Vec3, halfExtents: Vec3, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapBox |
| `physics.overlapCapsule(center: Vec3, radius: Float, height: Float, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | ref | scripting-reference.html#physics-overlapCapsule |
| `physics.raycastAll(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → entities: Int[], points: Vec3[], normals: Vec3[], distances: Float[], layers: Int[] | pure | ref | scripting-reference.html#physics-raycastAll |
| `physics.addForce(entity=self: Int, force: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addForce |
| `physics.addImpulse(entity=self: Int, impulse: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addImpulse |
| `physics.addTorque(entity=self: Int, torque: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addTorque |
| `physics.addForceAtPosition(entity=self: Int, force: Vec3, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addForceAtPosition |
| `physics.addImpulseAtPosition(entity=self: Int, impulse: Vec3, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-addImpulseAtPosition |
| `physics.getVelocity(entity=self: Int)` → velocity: Vec3 | pure | ref | scripting-reference.html#physics-getVelocity |
| `physics.setAngularVelocity(entity=self: Int, angularVelocity: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setAngularVelocity |
| `physics.getAngularVelocity(entity=self: Int)` → angularVelocity: Vec3 | pure | ref | scripting-reference.html#physics-getAngularVelocity |
| `physics.setGravity(gravity: Vec3)` | exec | ref | scripting-reference.html#physics-setGravity |
| `physics.getGravity()` → gravity: Vec3 | pure | ref | scripting-reference.html#physics-getGravity |
| `physics.setPosition(entity=self: Int, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setPosition |
| `physics.setPositionAndReset(entity=self: Int, position: Vec3)` → ok: Bool | exec | ref | scripting-reference.html#physics-setPositionAndReset |
| `physics.hasPhysics(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#physics-hasPhysics |
| `physics.addJoint(entityA: Int, entityB: Int, type: Int, anchorA: Vec3, anchorB: Vec3, axis: Vec3, minLimit: Float, maxLimit: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-addJoint |
| `physics.removeJoint(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#physics-removeJoint |
| `physics.hasJoint(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#physics-hasJoint |
| `physics.setJointMotor(entity=self: Int, targetSpeed: Float, maxForce: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointMotor |
| `physics.setJointBreakForce(entity=self: Int, breakForce: Float)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointBreakForce |
| `physics.setJointCollideConnected(entity=self: Int, collide: Bool)` → ok: Bool | exec | ref | scripting-reference.html#physics-setJointCollideConnected |
| `physics.pollJointBroken()` → entitiesA: Int[], entitiesB: Int[] | exec | ref | scripting-reference.html#physics-pollJointBroken |

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
| `widget.setZOrder(widget: Ref, z: Int)` | exec | ref | scripting-reference.html#widget-setZOrder |
| `widget.isVisible(widget: Ref)` → visible: Bool | pure | ref | scripting-reference.html#widget-isVisible |
| `widget.callFunction(widget: Ref, function: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-callFunction |
| `widget.addChild(widget: Ref, parent: String, widgetAsset: String)` → child: Ref | exec | ref | scripting-reference.html#widget-addChild |
| `widget.removeChild(widget: Ref, child: Ref)` → ok: Bool | exec | ref | scripting-reference.html#widget-removeChild |
| `widget.clearChildren(widget: Ref, parent: String)` → removed: Int | exec | ref | scripting-reference.html#widget-clearChildren |
| `widget.setListCount(widget: Ref, list: String, count: Int)` → ok: Bool | exec | ref | scripting-reference.html#widget-setListCount |
| `widget.listCount(widget: Ref, list: String)` → count: Int | pure | ref | scripting-reference.html#widget-listCount |
| `widget.listRow(widget: Ref, list: String, index: Int)` → row: Ref | pure | ref | scripting-reference.html#widget-listRow |
| `widget.refreshList(widget: Ref, list: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-refreshList |
| `widget.setListSelected(widget: Ref, list: String, index: Int, selected: Bool)` → ok: Bool | exec | ref | scripting-reference.html#widget-setListSelected |
| `widget.listSelected(widget: Ref, list: String)` → index: Int | pure | ref | scripting-reference.html#widget-listSelected |
| `widget.scrollListToItem(widget: Ref, list: String, index: Int)` → ok: Bool | exec | ref | scripting-reference.html#widget-scrollListToItem |
| `widget.animate(widget=self: Ref, element: String, property: String, to: Float, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animate |
| `widget.animateColor(widget=self: Ref, element: String, property: String, to: Color, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animateColor |
| `widget.animateVec2(widget=self: Ref, element: String, property: String, to: Vec2, seconds: Float, easing: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-animateVec2 |
| `widget.stopAnimation(widget=self: Ref, element: String, property: String)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAnimation |
| `widget.playAnimation(widget=self: Ref, animation: String, restoreAfterCompleted: Bool, direction: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-playAnimation |
| `widget.playAnimationLooped(widget=self: Ref, animation: String, loop: Bool, direction: String)` → ok: Bool | exec | ref | scripting-reference.html#widget-playAnimationLooped |
| `widget.stopAnimationClip(widget=self: Ref, animation: String)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAnimationClip |
| `widget.isPlayingAnimation(widget=self: Ref, animation: String)` → playing: Bool | pure | ref | scripting-reference.html#widget-isPlayingAnimation |
| `widget.childRef(widget=self: Ref, element: String)` → child: Ref | pure | ref | scripting-reference.html#widget-childRef |
| `widget.stopAllAnimations(widget=self: Ref)` → stopped: Int | exec | ref | scripting-reference.html#widget-stopAllAnimations |
| `widget.restoreOriginalState(widget=self: Ref)` → restored: Int | exec | ref | scripting-reference.html#widget-restoreOriginalState |
| `widget.showModal(widget: Ref)` | exec | ref | scripting-reference.html#widget-showModal |
| `widget.openPopup(widget: Ref, x: Float, y: Float)` | exec | ref | scripting-reference.html#widget-openPopup |
| `widget.openPopupAtPointer(widget: Ref)` | exec | ref | scripting-reference.html#widget-openPopupAtPointer |
| `widget.closeTopLayer()` → closed: Bool | exec | ref | scripting-reference.html#widget-closeTopLayer |

### `camera` — Camera (26)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `camera.getPosition()` → position: Vec3 | pure | ref | scripting-reference.html#camera-getPosition |
| `camera.setPosition(position: Vec3)` | exec | ref | scripting-reference.html#camera-setPosition |
| `camera.getRotation()` → rotation: Vec3 | pure | ref | scripting-reference.html#camera-getRotation |
| `camera.setRotation(rotation: Vec3)` | exec | ref | scripting-reference.html#camera-setRotation |
| `camera.getFov()` → degrees: Float | pure | ref | scripting-reference.html#camera-getFov |
| `camera.setFov(degrees: Float)` | exec | ref | scripting-reference.html#camera-setFov |
| `camera.setRigMode(mode: Int)` | exec | ref | scripting-reference.html#camera-setRigMode |
| `camera.getRigMode()` → mode: Int | pure | ref | scripting-reference.html#camera-getRigMode |
| `camera.setRigTarget(entity=self: Int)` | exec | ref | scripting-reference.html#camera-setRigTarget |
| `camera.setArmLength(length: Float)` | exec | ref | scripting-reference.html#camera-setArmLength |
| `camera.getArmLength()` → length: Float | pure | ref | scripting-reference.html#camera-getArmLength |
| `camera.setTargetYawMode(mode: Int)` | exec | ref | scripting-reference.html#camera-setTargetYawMode |
| `camera.getTargetYawMode()` → mode: Int | pure | ref | scripting-reference.html#camera-getTargetYawMode |
| `camera.getRigYaw()` → degrees: Float | pure | ref | scripting-reference.html#camera-getRigYaw |
| `camera.getRigPitch()` → degrees: Float | pure | ref | scripting-reference.html#camera-getRigPitch |
| `camera.addYawPitch(deltaYaw: Float, deltaPitch: Float)` | exec | ref | scripting-reference.html#camera-addYawPitch |
| `camera.setLagEnabled(enabled: Bool)` | exec | ref | scripting-reference.html#camera-setLagEnabled |
| `camera.getLagEnabled()` → enabled: Bool | pure | ref | scripting-reference.html#camera-getLagEnabled |
| `camera.setLagSpeeds(position: Float, rotation: Float)` | exec | ref | scripting-reference.html#camera-setLagSpeeds |
| `camera.snapRig()` | exec | ref | scripting-reference.html#camera-snapRig |
| `camera.playShake(positionAmplitude: Float, rotationAmplitude: Float, frequency: Float, duration: Float)` → handle: Int | exec | ref | scripting-reference.html#camera-playShake |
| `camera.stopShake(handle: Int)` | exec | ref | scripting-reference.html#camera-stopShake |
| `camera.stopAllShakes()` | exec | ref | scripting-reference.html#camera-stopAllShakes |
| `camera.kickFov(degrees: Float, attack: Float, hold: Float, decay: Float)` | exec | ref | scripting-reference.html#camera-kickFov |
| `camera.blendTo(camera: Int, seconds: Float, curve: Int)` | exec | ref | scripting-reference.html#camera-blendTo |
| `camera.isBlending()` → blending: Bool | pure | ref | scripting-reference.html#camera-isBlending |

### `app` — App (24)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `app.quit()` | exec | ref | scripting-reference.html#app-quit |
| `app.setTitle(title: String)` | exec | ref | scripting-reference.html#app-setTitle |
| `app.setSize(width: Int, height: Int)` | exec | ref | scripting-reference.html#app-setSize |
| `app.size()` → size: Vec2 | pure | ref | scripting-reference.html#app-size |
| `app.requestRedraw()` | exec | ref | scripting-reference.html#app-requestRedraw |
| `app.minimize()` | exec | ref | scripting-reference.html#app-minimize |
| `app.maximize(maximized: Bool)` | exec | ref | scripting-reference.html#app-maximize |
| `app.isMaximized()` → maximized: Bool | pure | ref | scripting-reference.html#app-isMaximized |
| `app.showTray(tooltip: String)` | exec | ref | scripting-reference.html#app-showTray |
| `app.hideTray()` | exec | ref | scripting-reference.html#app-hideTray |
| `app.addTrayItem(id: String, label: String)` | exec | ref | scripting-reference.html#app-addTrayItem |
| `app.clearTrayMenu()` | exec | ref | scripting-reference.html#app-clearTrayMenu |
| `app.addMenu(id: String, label: String)` | exec | ref | scripting-reference.html#app-addMenu |
| `app.addMenuItem(menu: String, id: String, label: String, shortcut: String)` | exec | ref | scripting-reference.html#app-addMenuItem |
| `app.addMenuSeparator(menu: String)` | exec | ref | scripting-reference.html#app-addMenuSeparator |
| `app.clearMenuBar()` | exec | ref | scripting-reference.html#app-clearMenuBar |
| `app.setMenuItemEnabled(id: String, enabled: Bool)` | exec | ref | scripting-reference.html#app-setMenuItemEnabled |
| `app.setMenuItemChecked(id: String, checked: Bool)` | exec | ref | scripting-reference.html#app-setMenuItemChecked |
| `app.menuItemEnabled(id: String)` → enabled: Bool | pure | ref | scripting-reference.html#app-menuItemEnabled |
| `app.menuItemChecked(id: String)` → checked: Bool | pure | ref | scripting-reference.html#app-menuItemChecked |
| `app.notify(title: String, text: String)` → shown: Bool | exec | ref | scripting-reference.html#app-notify |
| `app.notifyAvailable()` → available: Bool | pure | ref | scripting-reference.html#app-notifyAvailable |
| `app.setAutostart(enabled: Bool)` | exec | ref | scripting-reference.html#app-setAutostart · hand |
| `app.autostart()` → enabled: Bool | pure | ref | scripting-reference.html#app-autostart |

### `audio` — Audio (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `audio.play(asset: String, volume: Float, pitch: Float, loop: Bool)` → handle: Int | exec | ref | scripting-reference.html#audio-play |
| `audio.playAt(asset: String, position: Vec3, volume: Float, pitch: Float, loop: Bool, minDist: Float, maxDist: Float)` → handle: Int | exec | ref | scripting-reference.html#audio-playAt |
| `audio.stop(handle: Int)` | exec | ref | scripting-reference.html#audio-stop |
| `audio.stopAll()` | exec | ref | scripting-reference.html#audio-stopAll |
| `audio.isPlaying(handle: Int)` → playing: Bool | pure | ref | scripting-reference.html#audio-isPlaying |
| `audio.setBusVolume(bus: String, volume: Float)` | exec | ref | scripting-reference.html#audio-setBusVolume |
| `audio.setSoundPosition(handle: Int, position: Vec3)` | exec | ref | scripting-reference.html#audio-setSoundPosition |
| `audio.pause(handle: Int)` | exec | ref | scripting-reference.html#audio-pause |
| `audio.resume(handle: Int)` | exec | ref | scripting-reference.html#audio-resume |
| `audio.isPaused(handle: Int)` → paused: Bool | pure | ref | scripting-reference.html#audio-isPaused |
| `audio.setVolume(handle: Int, volume: Float)` | exec | ref | scripting-reference.html#audio-setVolume |
| `audio.getVolume(handle: Int)` → volume: Float | pure | ref | scripting-reference.html#audio-getVolume |
| `audio.setPitch(handle: Int, pitch: Float)` | exec | ref | scripting-reference.html#audio-setPitch |
| `audio.getPitch(handle: Int)` → pitch: Float | pure | ref | scripting-reference.html#audio-getPitch |
| `audio.setLooping(handle: Int, loop: Bool)` | exec | ref | scripting-reference.html#audio-setLooping |
| `audio.seek(handle: Int, seconds: Float)` | exec | ref | scripting-reference.html#audio-seek |
| `audio.getTime(handle: Int)` → seconds: Float | pure | ref | scripting-reference.html#audio-getTime |
| `audio.getLength(handle: Int)` → seconds: Float | pure | ref | scripting-reference.html#audio-getLength |

### `entity` — Entity (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `entity.getName(entity=self: Int)` → name: String | pure | ref | scripting-reference.html#entity-getName |
| `entity.spawn(parent: Int, name: String)` → entity: Int | exec | ref | scripting-reference.html#entity-spawn |
| `entity.destroy(entity: Int)` | exec | ref | scripting-reference.html#entity-destroy |
| `entity.spawnClass(class: String, x: Float, y: Float, z: Float)` → entity: Int | exec | ref | scripting-reference.html#entity-spawnClass |
| `entity.spawnClassRotated(class: String, x: Float, y: Float, z: Float, rx: Float, ry: Float, rz: Float)` → entity: Int | exec | ref | scripting-reference.html#entity-spawnClassRotated |
| `entity.destroyObject(object: Ref)` | exec | ref | scripting-reference.html#entity-destroyObject |
| `entity.self()` → entity: Int | pure | ref | scripting-reference.html#entity-self |
| `entity.selfObject()` → object: Ref | pure | ref | scripting-reference.html#entity-selfObject |
| `entity.instance(entity=self: Int)` → object: Ref | pure | ref | scripting-reference.html#entity-instance |
| `entity.owned(object: Ref)` → entity: Int | pure | ref | scripting-reference.html#entity-owned |
| `entity.distance(a: Int, b: Int)` → distance: Float | pure | ref | scripting-reference.html#entity-distance |
| `entity.findByName(name: String)` → entity: Int | pure | ref | scripting-reference.html#entity-findByName |
| `entity.exists(entity: Int)` → exists: Bool | pure | ref | scripting-reference.html#entity-exists |
| `entity.setVisible(entity=self: Int, visible: Bool)` | exec | ref | scripting-reference.html#entity-setVisible |
| `entity.saveState(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#entity-saveState |
| `entity.hasSavedState(entity=self: Int)` → has: Bool | pure | ref | scripting-reference.html#entity-hasSavedState |
| `entity.applySavedState(entity=self: Int)` → ok: Bool | exec | ref | scripting-reference.html#entity-applySavedState |
| `entity.getVisible(entity=self: Int)` → visible: Bool | pure | ref | scripting-reference.html#entity-getVisible |

### `input` — Input (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `input.keyDown(key: String)` → down: Bool | pure | ref | scripting-reference.html#input-keyDown |
| `input.mouseButton(button: Int)` → down: Bool | pure | ref | scripting-reference.html#input-mouseButton |
| `input.mousePosition()` → position: Vec2 | pure | ref | scripting-reference.html#input-mousePosition |
| `input.mouseDelta()` → delta: Vec2 | pure | ref | scripting-reference.html#input-mouseDelta |
| `input.scrollDelta()` → scroll: Float | pure | ref | scripting-reference.html#input-scrollDelta |
| `input.gamepadConnected()` → connected: Bool | pure | ref | scripting-reference.html#input-gamepadConnected |
| `input.gamepadButton(button: String)` → down: Bool | pure | ref | scripting-reference.html#input-gamepadButton |
| `input.gamepadAxis(axis: String)` → value: Float | pure | ref | scripting-reference.html#input-gamepadAxis |
| `input.actionDown(action: String)` → down: Bool | pure | ref | scripting-reference.html#input-actionDown |
| `input.actionPressed(action: String)` → pressed: Bool | pure | ref | scripting-reference.html#input-actionPressed |
| `input.actionReleased(action: String)` → released: Bool | pure | ref | scripting-reference.html#input-actionReleased |
| `input.actionAxis(action: String)` → value: Float | pure | ref | scripting-reference.html#input-actionAxis |
| `input.actionAxis2D(action: String)` → value: Vec2 | pure | ref | scripting-reference.html#input-actionAxis2D |
| `input.setModeGameOnly()` | exec | ref | scripting-reference.html#input-setModeGameOnly |
| `input.setModeGameAndUI()` | exec | ref | scripting-reference.html#input-setModeGameAndUI |
| `input.setModeUIOnly()` | exec | ref | scripting-reference.html#input-setModeUIOnly |
| `input.mode()` → mode: String | pure | ref | scripting-reference.html#input-mode |

### `save` — Save (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `save.create(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-create |
| `save.load(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-load |
| `save.write()` → ok: Bool | exec | ref | scripting-reference.html#save-write |
| `save.close()` | exec | ref | scripting-reference.html#save-close |
| `save.activeId()` → id: String | pure | ref | scripting-reference.html#save-activeId |
| `save.list()` → ids: String[] | pure | ref | scripting-reference.html#save-list |
| `save.exists(id: String)` → exists: Bool | pure | ref | scripting-reference.html#save-exists |
| `save.delete(id: String)` → ok: Bool | exec | ref | scripting-reference.html#save-delete |
| `save.fields()` → names: String[] | pure | ref | scripting-reference.html#save-fields |
| `save.setNumber(field: String, value: Float)` → ok: Bool | exec | ref | scripting-reference.html#save-setNumber |
| `save.getNumber(field: String, default: Float)` → value: Float | pure | ref | scripting-reference.html#save-getNumber |
| `save.setString(field: String, value: String)` → ok: Bool | exec | ref | scripting-reference.html#save-setString |
| `save.getString(field: String, default: String)` → value: String | pure | ref | scripting-reference.html#save-getString |
| `save.setBool(field: String, value: Bool)` → ok: Bool | exec | ref | scripting-reference.html#save-setBool |
| `save.getBool(field: String, default: Bool)` → value: Bool | pure | ref | scripting-reference.html#save-getBool |
| `save.setStruct(field: String, value: Struct)` → ok: Bool | exec | ref | scripting-reference.html#save-setStruct |
| `save.getStruct(field: String)` → value: Struct | pure | ref | scripting-reference.html#save-getStruct |

### `anticheat` — AntiCheat (15)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `anticheat.check(rule: String, value: Float, player: Int)` → ok: Bool | exec | ref | scripting-reference.html#anticheat-check |
| `anticheat.expectDisplacement(entity=self: Int, maxDistance: Float)` | exec | ref | scripting-reference.html#anticheat-expectDisplacement |
| `anticheat.report(player: Int, rule: String, weight: Float, detail: String)` | exec | ref | scripting-reference.html#anticheat-report |
| `anticheat.setPlayerLabel(player: Int, label: String)` | exec | ref | scripting-reference.html#anticheat-setPlayerLabel |
| `anticheat.respond(reportId: Int, response: Int)` | exec | ref | scripting-reference.html#anticheat-respond |
| `anticheat.kick(player: Int, reasonCode: Int)` | exec | ref | scripting-reference.html#anticheat-kick |
| `anticheat.reportLevel(reportId: Int)` → level: Int | pure | ref | scripting-reference.html#anticheat-reportLevel |
| `anticheat.reportRule(reportId: Int)` → rule: String | pure | ref | scripting-reference.html#anticheat-reportRule |
| `anticheat.reportPlayer(reportId: Int)` → player: Int | pure | ref | scripting-reference.html#anticheat-reportPlayer |
| `anticheat.reportEntity(reportId: Int)` → entity: Int | pure | ref | scripting-reference.html#anticheat-reportEntity |
| `anticheat.reportScore(reportId: Int)` → score: Float | pure | ref | scripting-reference.html#anticheat-reportScore |
| `anticheat.reportDetail(reportId: Int)` → detail: String | pure | ref | scripting-reference.html#anticheat-reportDetail |
| `anticheat.reportReason(reportId: Int)` → reasonCode: Int | pure | ref | scripting-reference.html#anticheat-reportReason |
| `anticheat.playerScore(player: Int)` → score: Float | pure | ref | scripting-reference.html#anticheat-playerScore |
| `anticheat.isEnabled()` → enabled: Bool | pure | ref | scripting-reference.html#anticheat-isEnabled |

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
| `scene.load(scene: String, hidden: Bool)` | exec | ref | scripting-reference.html#scene-load |
| `scene.loadAdditive(scene: String, hidden: Bool, position: Color)` → zone: Int | exec | ref | scripting-reference.html#scene-loadAdditive |
| `scene.unloadZone(zone: Int)` | exec | ref | scripting-reference.html#scene-unloadZone |
| `scene.activate()` | exec | ref | scripting-reference.html#scene-activate |
| `scene.hasPendingLevel()` → pending: Bool | pure | ref | scripting-reference.html#scene-hasPendingLevel |
| `scene.showZone(zone: Int)` | exec | ref | scripting-reference.html#scene-showZone |
| `scene.hideZone(zone: Int)` | exec | ref | scripting-reference.html#scene-hideZone |
| `scene.zonePosition(zone: Int)` → position: Vec3 | pure | ref | scripting-reference.html#scene-zonePosition |
| `scene.setZonePosition(zone: Int, position: Vec3)` | exec | ref | scripting-reference.html#scene-setZonePosition |
| `scene.zoneScene(zone: Int)` → scene: String | pure | ref | scripting-reference.html#scene-zoneScene |
| `scene.loadedZones()` → zones: Int[] | pure | ref | scripting-reference.html#scene-loadedZones |
| `scene.available()` → scenes: String[] | pure | ref | scripting-reference.html#scene-available |

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
| `ui.getText(entity=self: Int)` → text: String | pure | ref | scripting-reference.html#ui-getText |
| `ui.setText(entity=self: Int, text: String)` | exec | ref | scripting-reference.html#ui-setText |
| `ui.getColor(entity=self: Int)` → color: Color | pure | ref | scripting-reference.html#ui-getColor |
| `ui.setColor(entity=self: Int, color: Color)` | exec | ref | scripting-reference.html#ui-setColor |
| `ui.getVisible(entity=self: Int)` → visible: Bool | pure | ref | scripting-reference.html#ui-getVisible |
| `ui.setVisible(entity=self: Int, visible: Bool)` | exec | ref | scripting-reference.html#ui-setVisible |
| `ui.getPosition(entity=self: Int)` → position: Vec2 | pure | ref | scripting-reference.html#ui-getPosition |
| `ui.setPosition(entity=self: Int, position: Vec2)` | exec | ref | scripting-reference.html#ui-setPosition |
| `ui.getSize(entity=self: Int)` → size: Vec2 | pure | ref | scripting-reference.html#ui-getSize |
| `ui.setSize(entity=self: Int, size: Vec2)` | exec | ref | scripting-reference.html#ui-setSize |
| `ui.setMaterialParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | ref | scripting-reference.html#ui-setMaterialParam |
| `ui.pointerOverUI()` → over: Bool | pure | ref | scripting-reference.html#ui-pointerOverUI |

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
| `http.done(ticket: Int)` → done: Bool | pure | ref | scripting-reference.html#http-done |
| `http.ok(ticket: Int)` → ok: Bool | pure | ref | scripting-reference.html#http-ok |
| `http.status(ticket: Int)` → status: Int | pure | ref | scripting-reference.html#http-status |
| `http.body(ticket: Int)` → body: String | pure | ref | scripting-reference.html#http-body |
| `http.error(ticket: Int)` → error: String | pure | ref | scripting-reference.html#http-error |
| `http.forget(ticket: Int)` | exec | ref | scripting-reference.html#http-forget |
| `http.available()` → available: Bool | pure | ref | scripting-reference.html#http-available |

### `prefs` — Prefs (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `prefs.getString(key: String, fallback: String)` → value: String | pure | ref | scripting-reference.html#prefs-getString |
| `prefs.getNumber(key: String, fallback: Float)` → value: Float | pure | ref | scripting-reference.html#prefs-getNumber |
| `prefs.getBool(key: String, fallback: Bool)` → value: Bool | pure | ref | scripting-reference.html#prefs-getBool |
| `prefs.setString(key: String, value: String)` | exec | ref | scripting-reference.html#prefs-setString |
| `prefs.setNumber(key: String, value: Float)` | exec | ref | scripting-reference.html#prefs-setNumber |
| `prefs.setBool(key: String, value: Bool)` | exec | ref | scripting-reference.html#prefs-setBool |
| `prefs.has(key: String)` → present: Bool | pure | ref | scripting-reference.html#prefs-has |
| `prefs.remove(key: String)` → removed: Bool | exec | ref | scripting-reference.html#prefs-remove |
| `prefs.clear()` | exec | ref | scripting-reference.html#prefs-clear |

### `animator` — Animator (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `animator.setParam(entity=self: Int, name: String, value: Float)` | exec | ref | scripting-reference.html#animator-setParam |
| `animator.getParam(entity=self: Int, name: String)` → value: Float | pure | ref | scripting-reference.html#animator-getParam |
| `animator.getState(entity=self: Int)` → state: String | pure | ref | scripting-reference.html#animator-getState |
| `animator.notifiesOf(clipPath: String)` → names: String[] | pure | ref | scripting-reference.html#animator-notifiesOf |
| `animator.setLayerWeight(entity=self: Int, layer: String, weight: Float)` | exec | ref | scripting-reference.html#animator-setLayerWeight |
| `animator.getLayerWeight(entity=self: Int, layer: String)` → weight: Float | pure | ref | scripting-reference.html#animator-getLayerWeight |
| `animator.playLayer(entity=self: Int, layer: String)` | exec | ref | scripting-reference.html#animator-playLayer |
| `animator.layerNames(entity=self: Int)` → names: String[] | pure | ref | scripting-reference.html#animator-layerNames |

### `json` — JSON (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `json.getString(text: String, path: String, fallback: String)` → value: String | pure | ref | scripting-reference.html#json-getString |
| `json.getNumber(text: String, path: String, fallback: Float)` → value: Float | pure | ref | scripting-reference.html#json-getNumber |
| `json.getBool(text: String, path: String, fallback: Bool)` → value: Bool | pure | ref | scripting-reference.html#json-getBool |
| `json.has(text: String, path: String)` → present: Bool | pure | ref | scripting-reference.html#json-has |
| `json.count(text: String, path: String)` → count: Int | pure | ref | scripting-reference.html#json-count |
| `json.setString(text: String, path: String, value: String)` → result: String | pure | ref | scripting-reference.html#json-setString |
| `json.setNumber(text: String, path: String, value: Float)` → result: String | pure | ref | scripting-reference.html#json-setNumber |
| `json.setBool(text: String, path: String, value: Bool)` → result: String | pure | ref | scripting-reference.html#json-setBool |

### `transform` — Transform (8)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.transform.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `transform.getPosition(entity=self: Int)` → position: Vec3 | pure | ref | scripting-reference.html#transform-getPosition |
| `transform.setPosition(entity=self: Int, position: Vec3)` | exec | ref | scripting-reference.html#transform-setPosition |
| `transform.getRotation(entity=self: Int)` → rotation: Vec3 | pure | ref | scripting-reference.html#transform-getRotation |
| `transform.setRotation(entity=self: Int, rotation: Vec3)` | exec | ref | scripting-reference.html#transform-setRotation |
| `transform.getScale(entity=self: Int)` → scale: Vec3 | pure | ref | scripting-reference.html#transform-getScale |
| `transform.setScale(entity=self: Int, scale: Vec3)` | exec | ref | scripting-reference.html#transform-setScale |
| `transform.getWorldPosition(entity=self: Int)` → position: Vec3 | pure | ref | scripting-reference.html#transform-getWorldPosition |
| `transform.setWorldPosition(entity=self: Int, position: Vec3)` | exec | ref | scripting-reference.html#transform-setWorldPosition |

### `db` — Database (7)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `db.open(path: String)` → handle: Int | exec | ref | scripting-reference.html#db-open · hand |
| `db.close(handle: Int)` | exec | ref | scripting-reference.html#db-close |
| `db.exec(handle: Int, sql: String, params: String)` → ok: Bool | exec | ref | scripting-reference.html#db-exec |
| `db.query(handle: Int, sql: String, params: String)` → rows: String | exec | ref | scripting-reference.html#db-query |
| `db.changes(handle: Int)` → rows: Int | pure | ref | scripting-reference.html#db-changes |
| `db.lastInsertId(handle: Int)` → id: Int | pure | ref | scripting-reference.html#db-lastInsertId |
| `db.lastError(handle: Int)` → error: String | pure | ref | scripting-reference.html#db-lastError |

### `locomotion` — Locomotion (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `locomotion.move(entity=self: Int, direction: Vec3)` | exec | ref | scripting-reference.html#locomotion-move |
| `locomotion.look(entity=self: Int, yaw: Float, pitch: Float)` | exec | ref | scripting-reference.html#locomotion-look |
| `locomotion.setMaxSpeed(entity=self: Int, speed: Float)` | exec | ref | scripting-reference.html#locomotion-setMaxSpeed |
| `locomotion.setOrientToMovement(entity=self: Int, on: Bool)` | exec | ref | scripting-reference.html#locomotion-setOrientToMovement |
| `locomotion.jump(entity=self: Int)` → jumped: Bool | exec | ref | scripting-reference.html#locomotion-jump |
| `locomotion.jumpWith(entity=self: Int, speed: Float)` → jumped: Bool | exec | ref | scripting-reference.html#locomotion-jumpWith |

### `movement` — Movement (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `movement.speed(entity=self: Int)` → speed: Float | pure | ref | scripting-reference.html#movement-speed |
| `movement.verticalSpeed(entity=self: Int)` → speed: Float | pure | ref | scripting-reference.html#movement-verticalSpeed |
| `movement.isGrounded(entity=self: Int)` → grounded: Bool | pure | ref | scripting-reference.html#movement-isGrounded |
| `movement.velocity(entity=self: Int)` → velocity: Vec3 | pure | ref | scripting-reference.html#movement-velocity |
| `movement.forwardAmount(entity=self: Int)` → amount: Float | pure | ref | scripting-reference.html#movement-forwardAmount |
| `movement.rightAmount(entity=self: Int)` → amount: Float | pure | ref | scripting-reference.html#movement-rightAmount |

### `nav` — Navigation (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `nav.moveTo(entity=self: Int, x: Float, y: Float, z: Float)` → started: Bool | exec | ref | scripting-reference.html#nav-moveTo |
| `nav.stop(entity=self: Int)` | exec | ref | scripting-reference.html#nav-stop |
| `nav.isMoving(entity=self: Int)` → moving: Bool | pure | ref | scripting-reference.html#nav-isMoving |
| `nav.hasPath(entity=self: Int)` → hasPath: Bool | pure | ref | scripting-reference.html#nav-hasPath |
| `nav.remainingDistance(entity=self: Int)` → distance: Float | pure | ref | scripting-reference.html#nav-remainingDistance |
| `nav.setSpeed(entity=self: Int, speed: Float)` | exec | ref | scripting-reference.html#nav-setSpeed |

### `player` — Player (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `player.possess(controller: Ref, character: Ref)` | exec | ref | scripting-reference.html#player-possess |
| `player.unpossess(controller: Ref)` | exec | ref | scripting-reference.html#player-unpossess |
| `player.possessed(controller: Ref)` → character: Ref | pure | ref | scripting-reference.html#player-possessed |
| `player.controllerOf(character: Ref)` → controller: Ref | pure | ref | scripting-reference.html#player-controllerOf |
| `player.controller()` → controller: Ref | pure | ref | scripting-reference.html#player-controller |
| `player.character()` → character: Ref | pure | ref | scripting-reference.html#player-character |

### `theme` — Theme (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `theme.set(themeAsset: String)` → ok: Bool | exec | ref | scripting-reference.html#theme-set |
| `theme.setMode(mode: String)` | exec | ref | scripting-reference.html#theme-setMode |
| `theme.getMode()` → mode: String | pure | ref | scripting-reference.html#theme-getMode |
| `theme.getPreference()` → preference: String | pure | ref | scripting-reference.html#theme-getPreference |
| `theme.setFontScale(scale: Float)` | exec | ref | scripting-reference.html#theme-setFontScale |
| `theme.getFontScale()` → scale: Float | pure | ref | scripting-reference.html#theme-getFontScale |

### `dialog` — Dialog (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `dialog.message(title: String, text: String, kind: Int)` | exec | ref | scripting-reference.html#dialog-message |
| `dialog.confirm(title: String, text: String, affirmative: String, negative: String)` → confirmed: Bool | exec | ref | scripting-reference.html#dialog-confirm |
| `dialog.openFile(filter: String)` → path: String | exec | ref | scripting-reference.html#dialog-openFile |
| `dialog.saveFile(filter: String)` → path: String | exec | ref | scripting-reference.html#dialog-saveFile |
| `dialog.pickFolder()` → path: String | exec | ref | scripting-reference.html#dialog-pickFolder |

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
| `window.open(title: String, width: Int, height: Int)` → window: Int | exec | ref | scripting-reference.html#window-open |
| `window.close(window: Int)` | exec | ref | scripting-reference.html#window-close |
| `window.setTitle(window: Int, title: String)` | exec | ref | scripting-reference.html#window-setTitle |
| `window.setSize(window: Int, width: Int, height: Int)` | exec | ref | scripting-reference.html#window-setSize |
| `window.show(window: Int, widget: Ref)` | exec | ref | scripting-reference.html#window-show |

### `content` — Content (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `content.load(asset: String)` → loaded: Bool | exec | ref | scripting-reference.html#content-load |
| `content.unload(asset: String)` → unloaded: Bool | exec | ref | scripting-reference.html#content-unload |
| `content.isLoaded(asset: String)` → loaded: Bool | pure | ref | scripting-reference.html#content-isLoaded |
| `content.typeName(asset: String)` → type: String | pure | ref | scripting-reference.html#content-typeName |

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
| `particle.play(entity=self: Int)` | exec | ref | scripting-reference.html#particle-play |
| `particle.stop(entity=self: Int)` | exec | ref | scripting-reference.html#particle-stop |
| `particle.burst(entity=self: Int, count: Int)` → emitted: Int | exec | ref | scripting-reference.html#particle-burst |
| `particle.isPlaying(entity=self: Int)` → playing: Bool | pure | ref | scripting-reference.html#particle-isPlaying |

### `clipboard` — Clipboard (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `clipboard.getText()` → text: String | pure | ref | scripting-reference.html#clipboard-getText |
| `clipboard.setText(text: String)` | exec | ref | scripting-reference.html#clipboard-setText |
| `clipboard.hasText()` → has: Bool | pure | ref | scripting-reference.html#clipboard-hasText |

### `print` — Print (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `print.toPdf(path: String, text: String, title: String)` → ok: Bool | exec | ref | scripting-reference.html#print-toPdf · hand |
| `print.file(path: String)` → ok: Bool | exec | ref | scripting-reference.html#print-file · hand |
| `print.available()` → available: Bool | pure | ref | scripting-reference.html#print-available |

### `process` — Process (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `process.run(exe: String, args: String[], timeoutSeconds: Float)` → ok: Bool, exitCode: Int, out: String, err: String | exec | ref | scripting-reference.html#process-run · hand |
| `process.openUrl(url: String)` → ok: Bool | exec | ref | scripting-reference.html#process-openUrl · hand |
| `process.which(exe: String)` → path: String | pure | ref | scripting-reference.html#process-which |

### `material` — Material (2)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.material.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `material.getParam(entity=self: Int, name: String)` → value: Color | pure | ref | scripting-reference.html#material-getParam |
| `material.setParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | ref | scripting-reference.html#material-setParam |

### `cursor` — Cursor (1)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.cursor.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `cursor.setVisible(show: Bool)` | exec | ref | scripting-reference.html#cursor-setVisible |

### `log` — Debug (1)

> Id ohne Gruppe: in Lua/Python als flache Funktion `horizon.log` da.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `log(message: String)` | exec | ref | scripting-reference.html#fn-log · hand |
