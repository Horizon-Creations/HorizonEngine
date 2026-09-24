# Script-API-Doku: Abdeckung je Registry-Id (generiert)

Erzeugt von `scripts/script_api_docs/coverage.py` aus `scripts/script_api_docs/registry.json` (Dump von `HE::api::registry()`) und der lokalen Website-Doku. Nicht von Hand pflegen, neu erzeugen. Einordnung und Folgeschritte: `docs/script-api-docs-gap-audit-2026-09-24.md`.

Status: **ref** = Signatur+Rückgabe dokumentiert (nur über den flachen Zwilling), **named** = Id wörtlich auf einer Doku-Seite, **catalog** = nur der Name im HorizonCode-Knotenkatalog, **missing** = nirgends.

**Gesamt: 582 Registry-Ids in 42 Gruppen** — ref 13, named 39, catalog 96, missing 434.

| Gruppe | Kategorie | Ids | Lua/Py `horizon.<gruppe>.*` | ref | named | catalog | missing |
|---|---|---:|:---:|---:|---:|---:|---:|
| `env` | Environment | 116 | ja | 0 | 3 | 7 | 106 |
| `net` | Multiplayer | 44 | ja | 0 | 0 | 0 | 44 |
| `physics` | Physics | 33 | ja | 3 | 3 | 0 | 27 |
| `math` | Math | 31 | ja | 0 | 1 | 11 | 19 |
| `widget` | Widget | 28 | ja | 0 | 0 | 3 | 25 |
| `camera` | Camera | 26 | ja | 0 | 0 | 6 | 20 |
| `app` | App | 24 | ja | 0 | 0 | 0 | 24 |
| `audio` | Audio | 18 | ja | 0 | 4 | 3 | 11 |
| `entity` | Entity | 18 | ja | 3 | 1 | 9 | 5 |
| `input` | Input | 17 | ja | 0 | 8 | 0 | 9 |
| `save` | Save | 17 | ja | 0 | 6 | 5 | 6 |
| `anticheat` | AntiCheat | 15 | ja | 0 | 0 | 0 | 15 |
| `fs` | File | 13 | ja | 0 | 0 | 5 | 8 |
| `time` | Time | 13 | ja | 0 | 3 | 3 | 7 |
| `scene` | Scene | 12 | ja | 0 | 3 | 9 | 0 |
| `string` | String | 12 | ja | 0 | 0 | 11 | 1 |
| `ui` | UI | 12 | ja | 0 | 0 | 11 | 1 |
| `datetime` | DateTime | 9 | ja | 0 | 0 | 0 | 9 |
| `http` | HTTP | 9 | ja | 0 | 0 | 0 | 9 |
| `prefs` | Prefs | 9 | ja | 0 | 0 | 0 | 9 |
| `animator` | Animator | 8 | ja | 0 | 2 | 0 | 6 |
| `json` | JSON | 8 | ja | 0 | 0 | 0 | 8 |
| `transform` | Transform | 8 | **nein** | 6 | 0 | 0 | 2 |
| `db` | Database | 7 | ja | 0 | 0 | 0 | 7 |
| `locomotion` | Locomotion | 6 | ja | 0 | 0 | 0 | 6 |
| `movement` | Movement | 6 | ja | 0 | 0 | 0 | 6 |
| `nav` | Navigation | 6 | ja | 0 | 0 | 0 | 6 |
| `player` | Player | 6 | ja | 0 | 0 | 6 | 0 |
| `theme` | Theme | 6 | ja | 0 | 0 | 0 | 6 |
| `dialog` | Dialog | 5 | ja | 0 | 0 | 0 | 5 |
| `random` | Random | 5 | ja | 0 | 0 | 5 | 0 |
| `timer` | Timer | 5 | ja | 0 | 0 | 0 | 5 |
| `window` | Window | 5 | ja | 0 | 0 | 0 | 5 |
| `content` | Content | 4 | ja | 0 | 0 | 0 | 4 |
| `debug` | Debug | 4 | ja | 0 | 4 | 0 | 0 |
| `particle` | Particles | 4 | ja | 0 | 0 | 0 | 4 |
| `clipboard` | Clipboard | 3 | ja | 0 | 0 | 0 | 3 |
| `print` | Print | 3 | ja | 0 | 0 | 0 | 3 |
| `process` | Process | 3 | ja | 0 | 0 | 0 | 3 |
| `material` | Material | 2 | **nein** | 0 | 1 | 1 | 0 |
| `cursor` | Cursor | 1 | **nein** | 0 | 0 | 1 | 0 |
| `log` | Debug | 1 | **nein** | 1 | 0 | 0 | 0 |

## Flache `horizon.*`-Funktionen (35, nicht in der Registry)

Hand geschriebene Shims, identisch in Lua (`ScriptContext.cpp`) und Python (`PyScriptBackend.cpp`).

- dokumentiert mit Signatur (13): `log`, `getName`, `getPosition`, `setPosition`, `getRotation`, `setRotation`, `getScale`, `setScale`, `spawn`, `destroy`, `raycast`, `setVelocity`, `isGrounded`
- **ohne Signatur** (22): `setMaterialParam`, `getMaterialParam`, `setUIText`, `getUIText`, `setUIColor`, `getUIColor`, `setUIVisible`, `isUIVisible`, `setUIPosition`, `getUIPosition`, `setUISize`, `getUISize`, `setUIMaterialParam`, `createWidget`, `destroyWidget`, `showWidget`, `hideWidget`, `setWidgetZOrder`, `isWidgetVisible`, `callWidgetFunction`, `showCursor`, `hideCursor`

## Lifecycle-Callbacks

Aus dem Code gelesen (Lua: `HE_SCRIPT_CALL`-Namen, Python: `on_*`-Strings). Dokumentiert = Name kommt auf einer Doku-Seite vor.

- Lua: `onAnimationNotify` **fehlt**, `onAnimationNotifyBegin` **fehlt**, `onAnimationNotifyEnd` **fehlt**, `onBeginOverlap` **fehlt**, `onCheatDetected` **fehlt**, `onCollisionEnter`, `onCollisionExit`, `onEndOverlap` **fehlt**, `onInputAxis` **fehlt**, `onInputAxis2D` **fehlt**, `onInputPressed` **fehlt**, `onInputReleased` **fehlt**, `onNetEvent` **fehlt**, `onRep` **fehlt**, `onStart`, `onTimer` **fehlt**, `onUIEvent` **fehlt**, `onUpdate`
- Python: `on_animation_notify` **fehlt**, `on_animation_notify_begin` **fehlt**, `on_animation_notify_end` **fehlt**, `on_begin_overlap` **fehlt**, `on_cheat_detected` **fehlt**, `on_click`, `on_collision_enter`, `on_collision_exit`, `on_connected` **fehlt**, `on_disconnected` **fehlt**, `on_end_overlap` **fehlt**, `on_hover_enter`, `on_hover_exit`, `on_input_axis` **fehlt**, `on_input_pressed` **fehlt**, `on_input_released` **fehlt**, `on_player_joined` **fehlt**, `on_player_left` **fehlt**, `on_rep_` **fehlt**, `on_session_ended` **fehlt**, `on_session_started` **fehlt**, `on_start`, `on_timer` **fehlt**, `on_update`

## Je Gruppe

### `env` — Environment (116)

58 Felder, je get+set. Kandidat für eine generierte Tabelle statt Einzeleinträgen.

| Feld | Typ | Status |
|---|---|---|
| TimeOfDay | Float | catalog |
| CycleSeconds | Float | missing |
| SunIntensity | Float | missing |
| MoonIntensity | Float | missing |
| MoonPhase | Float | missing |
| MoonCycleDays | Float | missing |
| CloudCoverage | Float | catalog/named |
| WindDirection | Float | catalog |
| WindSpeed | Float | catalog/named |
| CloudHeight | Float | missing |
| CloudShadowStrength | Float | missing |
| CloudEvolution | Float | missing |
| CloudDensity | Float | missing |
| CloudFluffiness | Float | missing |
| ContrailAmount | Float | missing |
| CirrusAmount | Float | missing |
| CirrusSeed | Float | missing |
| GodRays | Float | missing |
| ShootingStars | Float | missing |
| LensFlare | Float | missing |
| FogDensity | Float | catalog/named |
| FogHeightFalloff | Float | missing |
| RainAmount | Float | missing |
| SnowAmount | Float | missing |
| Wetness | Float | missing |
| Flash | Float | missing |
| AuroraIntensity | Float | missing |
| MilkyWayIntensity | Float | missing |
| NebulaIntensity | Float | missing |
| NebulaSeed | Float | missing |
| NebulaCoverage | Float | missing |
| AuroraHeight | Float | missing |
| AuroraFragmentation | Float | missing |
| StarBrightness | Float | missing |
| StarSize | Float | missing |
| StarSizeVariation | Float | missing |
| StarGlow | Float | missing |
| StarTwinkle | Float | missing |
| StarDensity | Float | missing |
| DayNightCycle | Bool | missing |
| AutoAdvance | Bool | missing |
| MoonPhaseAuto | Bool | missing |
| CloudShadows | Bool | missing |
| CloudInterShadows | Bool | missing |
| LowResClouds | Bool | missing |
| CloudMode | Int | missing |
| CloudQuality | Int | missing |
| CloudStyle | Int | missing |
| NebulaQuality | Int | missing |
| SunColor | Color | missing |
| MoonColor | Color | missing |
| CloudTint | Color | missing |
| NebulaColor | Color | missing |
| NebulaColor2 | Color | missing |
| NebulaColor3 | Color | missing |
| AuroraColor | Color | missing |
| AuroraColorTop | Color | missing |
| StarColor | Color | missing |

### `net` — Multiplayer (44)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `net.host(port: Int, displayName: String)` → ok: Bool | exec | missing |  |
| `net.joinDirect(address: String, port: Int, code: String, displayName: String)` → ok: Bool | exec | missing |  |
| `net.joinLan(index: Int, code: String, displayName: String)` → ok: Bool | exec | missing |  |
| `net.leave()` | exec | missing |  |
| `net.status()` → status: Int | pure | missing |  |
| `net.lastError()` → error: String | pure | missing |  |
| `net.sessionId()` → sessionId: String | pure | missing |  |
| `net.joinCode()` → code: String | pure | missing |  |
| `net.refreshLan()` | exec | missing |  |
| `net.lanSessionCount()` → count: Int | pure | missing |  |
| `net.lanSessionName(index: Int)` → name: String | pure | missing |  |
| `net.lanSessionPlayers(index: Int)` → players: Int | pure | missing |  |
| `net.isAuthority()` → isAuthority: Bool | pure | missing |  |
| `net.isClient()` → isClient: Bool | pure | missing |  |
| `net.localPlayer()` → player: Int | pure | missing |  |
| `net.playerCount()` → count: Int | pure | missing |  |
| `net.playerAt(index: Int)` → player: Int | pure | missing |  |
| `net.playerName(player: Int)` → name: String | pure | missing |  |
| `net.ping(player: Int)` → ms: Float | pure | missing |  |
| `net.kick(player: Int)` | exec | missing |  |
| `net.ownerOf(entity=self: Int)` → player: Int | pure | missing |  |
| `net.isLocallyControlled(entity=self: Int)` → local: Bool | pure | missing |  |
| `net.localCharacter()` → entity: Int | pure | missing |  |
| `net.declareVarBool(entity=self: Int, name: String, initial: Bool, notify: Bool)` → ok: Bool | exec | missing |  |
| `net.declareVarInt(entity=self: Int, name: String, initial: Int, notify: Bool)` → ok: Bool | exec | missing |  |
| `net.declareVarFloat(entity=self: Int, name: String, initial: Float, notify: Bool)` → ok: Bool | exec | missing |  |
| `net.declareVarString(entity=self: Int, name: String, initial: String, notify: Bool)` → ok: Bool | exec | missing |  |
| `net.declareVarVec3(entity=self: Int, name: String, initial: Vec3, notify: Bool)` → ok: Bool | exec | missing |  |
| `net.setVarBool(entity=self: Int, name: String, value: Bool)` → ok: Bool | exec | missing |  |
| `net.setVarInt(entity=self: Int, name: String, value: Int)` → ok: Bool | exec | missing |  |
| `net.setVarFloat(entity=self: Int, name: String, value: Float)` → ok: Bool | exec | missing |  |
| `net.setVarString(entity=self: Int, name: String, value: String)` → ok: Bool | exec | missing |  |
| `net.setVarVec3(entity=self: Int, name: String, value: Vec3)` → ok: Bool | exec | missing |  |
| `net.getVarBool(entity=self: Int, name: String)` → value: Bool | pure | missing |  |
| `net.getVarInt(entity=self: Int, name: String)` → value: Int | pure | missing |  |
| `net.getVarFloat(entity=self: Int, name: String)` → value: Float | pure | missing |  |
| `net.getVarString(entity=self: Int, name: String)` → value: String | pure | missing |  |
| `net.getVarVec3(entity=self: Int, name: String)` → value: Vec3 | pure | missing |  |
| `net.callServer(entity=self: Int, function: String)` → ok: Bool | exec | missing |  |
| `net.callClient(player: Int, entity: Int, function: String)` → ok: Bool | exec | missing |  |
| `net.callAllClients(entity=self: Int, function: String)` → ok: Bool | exec | missing |  |
| `net.allowAnyClient(entity=self: Int, function: String)` → ok: Bool | exec | missing |  |
| `net.rpcSender()` → player: Int | pure | missing |  |
| `net.hasVar(entity=self: Int, name: String)` → declared: Bool | pure | missing |  |

### `physics` — Physics (33)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `physics.raycast(origin: Vec3, direction: Vec3, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | ref | flach `raycast` (scripting-api.html#api) |
| `physics.setVelocity(entity=self: Int, velocity: Vec3)` | exec | ref | flach `setVelocity` (scripting-api.html#api) |
| `physics.isGrounded(entity=self: Int)` → grounded: Bool | pure | ref | flach `isGrounded` (scripting-api.html#api) |
| `physics.sphereCast(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | missing |  |
| `physics.overlapSphere(center: Vec3, radius: Float)` → entities: Int[] | pure | missing |  |
| `physics.raycastLayers(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | missing |  |
| `physics.sphereCastLayers(origin: Vec3, direction: Vec3, radius: Float, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | missing |  |
| `physics.overlapSphereLayers(center: Vec3, radius: Float, layerMask: Int)` → entities: Int[] | pure | missing |  |
| `physics.boxCast(origin: Vec3, halfExtents: Vec3, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | missing |  |
| `physics.capsuleCast(origin: Vec3, radius: Float, height: Float, rotation: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → hit: Bool, entity: Int, point: Vec3, normal: Vec3, distance: Float, layer: Int | pure | missing |  |
| `physics.overlapBox(center: Vec3, halfExtents: Vec3, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | missing |  |
| `physics.overlapCapsule(center: Vec3, radius: Float, height: Float, rotation: Vec3, layerMask: Int)` → entities: Int[] | pure | missing |  |
| `physics.raycastAll(origin: Vec3, direction: Vec3, maxDistance: Float, layerMask: Int)` → entities: Int[], points: Vec3[], normals: Vec3[], distances: Float[], layers: Int[] | pure | missing |  |
| `physics.addForce(entity=self: Int, force: Vec3)` → ok: Bool | exec | missing |  |
| `physics.addImpulse(entity=self: Int, impulse: Vec3)` → ok: Bool | exec | named | scripting.html |
| `physics.addTorque(entity=self: Int, torque: Vec3)` → ok: Bool | exec | missing |  |
| `physics.addForceAtPosition(entity=self: Int, force: Vec3, position: Vec3)` → ok: Bool | exec | missing |  |
| `physics.addImpulseAtPosition(entity=self: Int, impulse: Vec3, position: Vec3)` → ok: Bool | exec | missing |  |
| `physics.getVelocity(entity=self: Int)` → velocity: Vec3 | pure | named | scripting.html |
| `physics.setAngularVelocity(entity=self: Int, angularVelocity: Vec3)` → ok: Bool | exec | missing |  |
| `physics.getAngularVelocity(entity=self: Int)` → angularVelocity: Vec3 | pure | missing |  |
| `physics.setGravity(gravity: Vec3)` | exec | missing |  |
| `physics.getGravity()` → gravity: Vec3 | pure | missing |  |
| `physics.setPosition(entity=self: Int, position: Vec3)` → ok: Bool | exec | named | scripting.html |
| `physics.setPositionAndReset(entity=self: Int, position: Vec3)` → ok: Bool | exec | missing |  |
| `physics.hasPhysics(entity=self: Int)` → has: Bool | pure | missing |  |
| `physics.addJoint(entityA: Int, entityB: Int, type: Int, anchorA: Vec3, anchorB: Vec3, axis: Vec3, minLimit: Float, maxLimit: Float)` → ok: Bool | exec | missing |  |
| `physics.removeJoint(entity=self: Int)` → ok: Bool | exec | missing |  |
| `physics.hasJoint(entity=self: Int)` → has: Bool | pure | missing |  |
| `physics.setJointMotor(entity=self: Int, targetSpeed: Float, maxForce: Float)` → ok: Bool | exec | missing |  |
| `physics.setJointBreakForce(entity=self: Int, breakForce: Float)` → ok: Bool | exec | missing |  |
| `physics.setJointCollideConnected(entity=self: Int, collide: Bool)` → ok: Bool | exec | missing |  |
| `physics.pollJointBroken()` → entitiesA: Int[], entitiesB: Int[] | exec | missing |  |

### `math` — Math (31)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `math.sin(x: Float)` → result: Float | pure | named | scripting-api.html |
| `math.cos(x: Float)` → result: Float | pure | missing |  |
| `math.tan(x: Float)` → result: Float | pure | missing |  |
| `math.sqrt(x: Float)` → result: Float | pure | missing |  |
| `math.abs(x: Float)` → result: Float | pure | missing |  |
| `math.floor(x: Float)` → result: Float | pure | missing |  |
| `math.ceil(x: Float)` → result: Float | pure | missing |  |
| `math.round(x: Float)` → result: Float | pure | missing |  |
| `math.sign(x: Float)` → result: Float | pure | missing |  |
| `math.radians(x: Float)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.degrees(x: Float)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.pow(base: Float, exp: Float)` → result: Float | pure | missing |  |
| `math.mod(a: Float, b: Float)` → result: Float | pure | missing |  |
| `math.bitAnd(a: Int, b: Int)` → result: Int | pure | missing |  |
| `math.bitOr(a: Int, b: Int)` → result: Int | pure | missing |  |
| `math.bitXor(a: Int, b: Int)` → result: Int | pure | missing |  |
| `math.bitNot(x: Int)` → result: Int | pure | missing |  |
| `math.shiftLeft(x: Int, count: Int)` → result: Int | pure | missing |  |
| `math.shiftRight(x: Int, count: Int)` → result: Int | pure | missing |  |
| `math.atan2(y: Float, x: Float)` → result: Float | pure | missing |  |
| `math.min(a: Float, b: Float)` → result: Float | pure | missing |  |
| `math.max(a: Float, b: Float)` → result: Float | pure | missing |  |
| `math.clamp(x: Float, lo: Float, hi: Float)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.lerp(a: Float, b: Float, t: Float)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.length(v: Vec2)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.distance(a: Vec2, b: Vec2)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.length3(v: Vec3)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.distance3(a: Vec3, b: Vec3)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.normalize3(v: Vec3)` → result: Vec3 | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.dot3(a: Vec3, b: Vec3)` → result: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `math.cross(a: Vec3, b: Vec3)` → result: Vec3 | pure | catalog | horizoncode-nodes.html#engine-call |

### `widget` — Widget (28)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `widget.setZOrder(widget: Ref, z: Int)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `widget.isVisible(widget: Ref)` → visible: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `widget.callFunction(widget: Ref, function: String)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `widget.addChild(widget: Ref, parent: String, widgetAsset: String)` → child: Ref | exec | missing |  |
| `widget.removeChild(widget: Ref, child: Ref)` → ok: Bool | exec | missing |  |
| `widget.clearChildren(widget: Ref, parent: String)` → removed: Int | exec | missing |  |
| `widget.setListCount(widget: Ref, list: String, count: Int)` → ok: Bool | exec | missing |  |
| `widget.listCount(widget: Ref, list: String)` → count: Int | pure | missing |  |
| `widget.listRow(widget: Ref, list: String, index: Int)` → row: Ref | pure | missing |  |
| `widget.refreshList(widget: Ref, list: String)` → ok: Bool | exec | missing |  |
| `widget.setListSelected(widget: Ref, list: String, index: Int, selected: Bool)` → ok: Bool | exec | missing |  |
| `widget.listSelected(widget: Ref, list: String)` → index: Int | pure | missing |  |
| `widget.scrollListToItem(widget: Ref, list: String, index: Int)` → ok: Bool | exec | missing |  |
| `widget.animate(widget=self: Ref, element: String, property: String, to: Float, seconds: Float, easing: String)` → ok: Bool | exec | missing |  |
| `widget.animateColor(widget=self: Ref, element: String, property: String, to: Color, seconds: Float, easing: String)` → ok: Bool | exec | missing |  |
| `widget.animateVec2(widget=self: Ref, element: String, property: String, to: Vec2, seconds: Float, easing: String)` → ok: Bool | exec | missing |  |
| `widget.stopAnimation(widget=self: Ref, element: String, property: String)` → stopped: Int | exec | missing |  |
| `widget.playAnimation(widget=self: Ref, animation: String, restoreAfterCompleted: Bool, direction: String)` → ok: Bool | exec | missing |  |
| `widget.playAnimationLooped(widget=self: Ref, animation: String, loop: Bool, direction: String)` → ok: Bool | exec | missing |  |
| `widget.stopAnimationClip(widget=self: Ref, animation: String)` → stopped: Int | exec | missing |  |
| `widget.isPlayingAnimation(widget=self: Ref, animation: String)` → playing: Bool | pure | missing |  |
| `widget.childRef(widget=self: Ref, element: String)` → child: Ref | pure | missing |  |
| `widget.stopAllAnimations(widget=self: Ref)` → stopped: Int | exec | missing |  |
| `widget.restoreOriginalState(widget=self: Ref)` → restored: Int | exec | missing |  |
| `widget.showModal(widget: Ref)` | exec | missing |  |
| `widget.openPopup(widget: Ref, x: Float, y: Float)` | exec | missing |  |
| `widget.openPopupAtPointer(widget: Ref)` | exec | missing |  |
| `widget.closeTopLayer()` → closed: Bool | exec | missing |  |

### `camera` — Camera (26)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `camera.getPosition()` → position: Vec3 | pure | catalog | horizoncode-nodes.html#engine-call |
| `camera.setPosition(position: Vec3)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `camera.getRotation()` → rotation: Vec3 | pure | catalog | horizoncode-nodes.html#engine-call |
| `camera.setRotation(rotation: Vec3)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `camera.getFov()` → degrees: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `camera.setFov(degrees: Float)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `camera.setRigMode(mode: Int)` | exec | missing |  |
| `camera.getRigMode()` → mode: Int | pure | missing |  |
| `camera.setRigTarget(entity=self: Int)` | exec | missing |  |
| `camera.setArmLength(length: Float)` | exec | missing |  |
| `camera.getArmLength()` → length: Float | pure | missing |  |
| `camera.setTargetYawMode(mode: Int)` | exec | missing |  |
| `camera.getTargetYawMode()` → mode: Int | pure | missing |  |
| `camera.getRigYaw()` → degrees: Float | pure | missing |  |
| `camera.getRigPitch()` → degrees: Float | pure | missing |  |
| `camera.addYawPitch(deltaYaw: Float, deltaPitch: Float)` | exec | missing |  |
| `camera.setLagEnabled(enabled: Bool)` | exec | missing |  |
| `camera.getLagEnabled()` → enabled: Bool | pure | missing |  |
| `camera.setLagSpeeds(position: Float, rotation: Float)` | exec | missing |  |
| `camera.snapRig()` | exec | missing |  |
| `camera.playShake(positionAmplitude: Float, rotationAmplitude: Float, frequency: Float, duration: Float)` → handle: Int | exec | missing |  |
| `camera.stopShake(handle: Int)` | exec | missing |  |
| `camera.stopAllShakes()` | exec | missing |  |
| `camera.kickFov(degrees: Float, attack: Float, hold: Float, decay: Float)` | exec | missing |  |
| `camera.blendTo(camera: Int, seconds: Float, curve: Int)` | exec | missing |  |
| `camera.isBlending()` → blending: Bool | pure | missing |  |

### `app` — App (24)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `app.quit()` | exec | missing |  |
| `app.setTitle(title: String)` | exec | missing |  |
| `app.setSize(width: Int, height: Int)` | exec | missing |  |
| `app.size()` → size: Vec2 | pure | missing |  |
| `app.requestRedraw()` | exec | missing |  |
| `app.minimize()` | exec | missing |  |
| `app.maximize(maximized: Bool)` | exec | missing |  |
| `app.isMaximized()` → maximized: Bool | pure | missing |  |
| `app.showTray(tooltip: String)` | exec | missing |  |
| `app.hideTray()` | exec | missing |  |
| `app.addTrayItem(id: String, label: String)` | exec | missing |  |
| `app.clearTrayMenu()` | exec | missing |  |
| `app.addMenu(id: String, label: String)` | exec | missing |  |
| `app.addMenuItem(menu: String, id: String, label: String, shortcut: String)` | exec | missing |  |
| `app.addMenuSeparator(menu: String)` | exec | missing |  |
| `app.clearMenuBar()` | exec | missing |  |
| `app.setMenuItemEnabled(id: String, enabled: Bool)` | exec | missing |  |
| `app.setMenuItemChecked(id: String, checked: Bool)` | exec | missing |  |
| `app.menuItemEnabled(id: String)` → enabled: Bool | pure | missing |  |
| `app.menuItemChecked(id: String)` → checked: Bool | pure | missing |  |
| `app.notify(title: String, text: String)` → shown: Bool | exec | missing |  |
| `app.notifyAvailable()` → available: Bool | pure | missing |  |
| `app.setAutostart(enabled: Bool)` | exec | missing |  |
| `app.autostart()` → enabled: Bool | pure | missing |  |

### `audio` — Audio (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `audio.play(asset: String, volume: Float, pitch: Float, loop: Bool)` → handle: Int | exec | named | systems.html |
| `audio.playAt(asset: String, position: Vec3, volume: Float, pitch: Float, loop: Bool, minDist: Float, maxDist: Float)` → handle: Int | exec | named | systems.html |
| `audio.stop(handle: Int)` | exec | named | systems.html |
| `audio.stopAll()` | exec | catalog | horizoncode-nodes.html#engine-call |
| `audio.isPlaying(handle: Int)` → playing: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `audio.setBusVolume(bus: String, volume: Float)` | exec | named | systems.html |
| `audio.setSoundPosition(handle: Int, position: Vec3)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `audio.pause(handle: Int)` | exec | missing |  |
| `audio.resume(handle: Int)` | exec | missing |  |
| `audio.isPaused(handle: Int)` → paused: Bool | pure | missing |  |
| `audio.setVolume(handle: Int, volume: Float)` | exec | missing |  |
| `audio.getVolume(handle: Int)` → volume: Float | pure | missing |  |
| `audio.setPitch(handle: Int, pitch: Float)` | exec | missing |  |
| `audio.getPitch(handle: Int)` → pitch: Float | pure | missing |  |
| `audio.setLooping(handle: Int, loop: Bool)` | exec | missing |  |
| `audio.seek(handle: Int, seconds: Float)` | exec | missing |  |
| `audio.getTime(handle: Int)` → seconds: Float | pure | missing |  |
| `audio.getLength(handle: Int)` → seconds: Float | pure | missing |  |

### `entity` — Entity (18)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `entity.getName(entity=self: Int)` → name: String | pure | ref | flach `getName` (scripting-api.html#api) |
| `entity.spawn(parent: Int, name: String)` → entity: Int | exec | ref | flach `spawn` (scripting-api.html#api) |
| `entity.destroy(entity: Int)` | exec | ref | flach `destroy` (scripting-api.html#api) |
| `entity.spawnClass(class: String, x: Float, y: Float, z: Float)` → entity: Int | exec | missing |  |
| `entity.spawnClassRotated(class: String, x: Float, y: Float, z: Float, rx: Float, ry: Float, rz: Float)` → entity: Int | exec | missing |  |
| `entity.destroyObject(object: Ref)` | exec | missing |  |
| `entity.self()` → entity: Int | pure | catalog | horizoncode-nodes.html#engine-call |
| `entity.selfObject()` → object: Ref | pure | missing |  |
| `entity.instance(entity=self: Int)` → object: Ref | pure | missing |  |
| `entity.owned(object: Ref)` → entity: Int | pure | catalog | horizoncode-nodes.html#engine-call |
| `entity.distance(a: Int, b: Int)` → distance: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `entity.findByName(name: String)` → entity: Int | pure | named | scripting.html |
| `entity.exists(entity: Int)` → exists: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `entity.setVisible(entity=self: Int, visible: Bool)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `entity.saveState(entity=self: Int)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `entity.hasSavedState(entity=self: Int)` → has: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `entity.applySavedState(entity=self: Int)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `entity.getVisible(entity=self: Int)` → visible: Bool | pure | catalog | horizoncode-nodes.html#engine-call |

### `input` — Input (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `input.keyDown(key: String)` → down: Bool | pure | named | scripting.html, systems.html |
| `input.mouseButton(button: Int)` → down: Bool | pure | named | systems.html |
| `input.mousePosition()` → position: Vec2 | pure | named | systems.html |
| `input.mouseDelta()` → delta: Vec2 | pure | named | systems.html |
| `input.scrollDelta()` → scroll: Float | pure | named | systems.html |
| `input.gamepadConnected()` → connected: Bool | pure | named | systems.html |
| `input.gamepadButton(button: String)` → down: Bool | pure | named | systems.html |
| `input.gamepadAxis(axis: String)` → value: Float | pure | named | systems.html |
| `input.actionDown(action: String)` → down: Bool | pure | missing |  |
| `input.actionPressed(action: String)` → pressed: Bool | pure | missing |  |
| `input.actionReleased(action: String)` → released: Bool | pure | missing |  |
| `input.actionAxis(action: String)` → value: Float | pure | missing |  |
| `input.actionAxis2D(action: String)` → value: Vec2 | pure | missing |  |
| `input.setModeGameOnly()` | exec | missing |  |
| `input.setModeGameAndUI()` | exec | missing |  |
| `input.setModeUIOnly()` | exec | missing |  |
| `input.mode()` → mode: String | pure | missing |  |

### `save` — Save (17)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `save.create(id: String)` → ok: Bool | exec | named | scripting.html |
| `save.load(id: String)` → ok: Bool | exec | named | scripting.html |
| `save.write()` → ok: Bool | exec | named | scripting.html |
| `save.close()` | exec | catalog | horizoncode-nodes.html#engine-call |
| `save.activeId()` → id: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `save.list()` → ids: String[] | pure | catalog | horizoncode-nodes.html#engine-call |
| `save.exists(id: String)` → exists: Bool | pure | named | scripting.html |
| `save.delete(id: String)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `save.fields()` → names: String[] | pure | catalog | horizoncode-nodes.html#engine-call |
| `save.setNumber(field: String, value: Float)` → ok: Bool | exec | named | scripting.html |
| `save.getNumber(field: String, default: Float)` → value: Float | pure | named | scripting.html |
| `save.setString(field: String, value: String)` → ok: Bool | exec | missing |  |
| `save.getString(field: String, default: String)` → value: String | pure | missing |  |
| `save.setBool(field: String, value: Bool)` → ok: Bool | exec | missing |  |
| `save.getBool(field: String, default: Bool)` → value: Bool | pure | missing |  |
| `save.setStruct(field: String, value: Struct)` → ok: Bool | exec | missing |  |
| `save.getStruct(field: String)` → value: Struct | pure | missing |  |

### `anticheat` — AntiCheat (15)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `anticheat.check(rule: String, value: Float, player: Int)` → ok: Bool | exec | missing |  |
| `anticheat.expectDisplacement(entity=self: Int, maxDistance: Float)` | exec | missing |  |
| `anticheat.report(player: Int, rule: String, weight: Float, detail: String)` | exec | missing |  |
| `anticheat.setPlayerLabel(player: Int, label: String)` | exec | missing |  |
| `anticheat.respond(reportId: Int, response: Int)` | exec | missing |  |
| `anticheat.kick(player: Int, reasonCode: Int)` | exec | missing |  |
| `anticheat.reportLevel(reportId: Int)` → level: Int | pure | missing |  |
| `anticheat.reportRule(reportId: Int)` → rule: String | pure | missing |  |
| `anticheat.reportPlayer(reportId: Int)` → player: Int | pure | missing |  |
| `anticheat.reportEntity(reportId: Int)` → entity: Int | pure | missing |  |
| `anticheat.reportScore(reportId: Int)` → score: Float | pure | missing |  |
| `anticheat.reportDetail(reportId: Int)` → detail: String | pure | missing |  |
| `anticheat.reportReason(reportId: Int)` → reasonCode: Int | pure | missing |  |
| `anticheat.playerScore(player: Int)` → score: Float | pure | missing |  |
| `anticheat.isEnabled()` → enabled: Bool | pure | missing |  |

### `fs` — File (13)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `fs.writeText(path: String, text: String)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `fs.readText(path: String)` → text: String | exec | catalog | horizoncode-nodes.html#engine-call |
| `fs.exists(path: String)` → exists: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `fs.remove(path: String)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `fs.makeDir(path: String)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `fs.isDir(path: String)` → isDir: Bool | pure | missing |  |
| `fs.size(path: String)` → bytes: Float | pure | missing |  |
| `fs.modified(path: String)` → time: Float | pure | missing |  |
| `fs.list(dir: String)` → names: String[] | pure | missing |  |
| `fs.rename(from: String, to: String)` → ok: Bool | exec | missing |  |
| `fs.copy(from: String, to: String)` → ok: Bool | exec | missing |  |
| `fs.watch(path: String)` → handle: Int | exec | missing |  |
| `fs.unwatch(handle: Int)` | exec | missing |  |

### `time` — Time (13)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `time.deltaTime()` → dt: Float | pure | named | scripting-api.html |
| `time.elapsed()` → seconds: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `time.frameCount()` → frame: Int | pure | catalog | horizoncode-nodes.html#engine-call |
| `time.setTimeScale(scale: Float)` | exec | named | scripting-api.html, systems.html |
| `time.timeScale()` → scale: Float | pure | catalog | horizoncode-nodes.html#engine-call |
| `time.unscaledDeltaTime()` → dt: Float | pure | named | scripting-api.html |
| `time.unscaledElapsed()` → seconds: Float | pure | missing |  |
| `time.pause()` | exec | missing |  |
| `time.resume()` | exec | missing |  |
| `time.isPaused()` → paused: Bool | pure | missing |  |
| `time.hitStop(seconds: Float)` | exec | missing |  |
| `time.isFrozen()` → frozen: Bool | pure | missing |  |
| `time.effectiveScale()` → scale: Float | pure | missing |  |

### `scene` — Scene (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `scene.load(scene: String, hidden: Bool)` | exec | named | scenes.html |
| `scene.loadAdditive(scene: String, hidden: Bool, position: Color)` → zone: Int | exec | named | scenes.html |
| `scene.unloadZone(zone: Int)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `scene.activate()` | exec | catalog | horizoncode-nodes.html#engine-call |
| `scene.hasPendingLevel()` → pending: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `scene.showZone(zone: Int)` | exec | named | scenes.html |
| `scene.hideZone(zone: Int)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `scene.zonePosition(zone: Int)` → position: Vec3 | pure | catalog | horizoncode-nodes.html#engine-call |
| `scene.setZonePosition(zone: Int, position: Vec3)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `scene.zoneScene(zone: Int)` → scene: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `scene.loadedZones()` → zones: Int[] | pure | catalog | horizoncode-nodes.html#engine-call |
| `scene.available()` → scenes: String[] | pure | catalog | horizoncode-nodes.html#engine-call |

### `string` — String (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `string.length(s: String)` → length: Int | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.equals(a: String, b: String)` → result: Bool | pure | missing |  |
| `string.substring(s: String, start: Int, count: Int)` → result: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.contains(s: String, needle: String)` → contains: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.find(s: String, needle: String)` → index: Int | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.replace(s: String, from: String, to: String)` → result: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.toUpper(s: String)` → result: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.toLower(s: String)` → result: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.trim(s: String)` → result: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.startsWith(s: String, prefix: String)` → result: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.endsWith(s: String, suffix: String)` → result: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `string.toNumber(s: String)` → number: Float | pure | catalog | horizoncode-nodes.html#engine-call |

### `ui` — UI (12)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `ui.getText(entity=self: Int)` → text: String | pure | catalog | horizoncode-nodes.html#engine-call |
| `ui.setText(entity=self: Int, text: String)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.getColor(entity=self: Int)` → color: Color | pure | catalog | horizoncode-nodes.html#engine-call |
| `ui.setColor(entity=self: Int, color: Color)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.getVisible(entity=self: Int)` → visible: Bool | pure | catalog | horizoncode-nodes.html#engine-call |
| `ui.setVisible(entity=self: Int, visible: Bool)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.getPosition(entity=self: Int)` → position: Vec2 | pure | catalog | horizoncode-nodes.html#engine-call |
| `ui.setPosition(entity=self: Int, position: Vec2)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.getSize(entity=self: Int)` → size: Vec2 | pure | catalog | horizoncode-nodes.html#engine-call |
| `ui.setSize(entity=self: Int, size: Vec2)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.setMaterialParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | catalog | horizoncode-nodes.html#engine-call |
| `ui.pointerOverUI()` → over: Bool | pure | missing |  |

### `datetime` — DateTime (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `datetime.now()` → epochSeconds: Float | pure | missing |  |
| `datetime.format(epochSeconds: Float, format: String)` → text: String | pure | missing |  |
| `datetime.year(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.month(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.day(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.hour(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.minute(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.second(epochSeconds: Float)` → value: Int | pure | missing |  |
| `datetime.weekday(epochSeconds: Float)` → value: Int | pure | missing |  |

### `http` — HTTP (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `http.get(url: String)` → ticket: Int | exec | missing |  |
| `http.post(url: String, contentType: String, body: String)` → ticket: Int | exec | missing |  |
| `http.done(ticket: Int)` → done: Bool | pure | missing |  |
| `http.ok(ticket: Int)` → ok: Bool | pure | missing |  |
| `http.status(ticket: Int)` → status: Int | pure | missing |  |
| `http.body(ticket: Int)` → body: String | pure | missing |  |
| `http.error(ticket: Int)` → error: String | pure | missing |  |
| `http.forget(ticket: Int)` | exec | missing |  |
| `http.available()` → available: Bool | pure | missing |  |

### `prefs` — Prefs (9)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `prefs.getString(key: String, fallback: String)` → value: String | pure | missing |  |
| `prefs.getNumber(key: String, fallback: Float)` → value: Float | pure | missing |  |
| `prefs.getBool(key: String, fallback: Bool)` → value: Bool | pure | missing |  |
| `prefs.setString(key: String, value: String)` | exec | missing |  |
| `prefs.setNumber(key: String, value: Float)` | exec | missing |  |
| `prefs.setBool(key: String, value: Bool)` | exec | missing |  |
| `prefs.has(key: String)` → present: Bool | pure | missing |  |
| `prefs.remove(key: String)` → removed: Bool | exec | missing |  |
| `prefs.clear()` | exec | missing |  |

### `animator` — Animator (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `animator.setParam(entity=self: Int, name: String, value: Float)` | exec | missing |  |
| `animator.getParam(entity=self: Int, name: String)` → value: Float | pure | missing |  |
| `animator.getState(entity=self: Int)` → state: String | pure | missing |  |
| `animator.notifiesOf(clipPath: String)` → names: String[] | pure | missing |  |
| `animator.setLayerWeight(entity=self: Int, layer: String, weight: Float)` | exec | named | systems.html |
| `animator.getLayerWeight(entity=self: Int, layer: String)` → weight: Float | pure | missing |  |
| `animator.playLayer(entity=self: Int, layer: String)` | exec | named | systems.html |
| `animator.layerNames(entity=self: Int)` → names: String[] | pure | missing |  |

### `json` — JSON (8)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `json.getString(text: String, path: String, fallback: String)` → value: String | pure | missing |  |
| `json.getNumber(text: String, path: String, fallback: Float)` → value: Float | pure | missing |  |
| `json.getBool(text: String, path: String, fallback: Bool)` → value: Bool | pure | missing |  |
| `json.has(text: String, path: String)` → present: Bool | pure | missing |  |
| `json.count(text: String, path: String)` → count: Int | pure | missing |  |
| `json.setString(text: String, path: String, value: String)` → result: String | pure | missing |  |
| `json.setNumber(text: String, path: String, value: Float)` → result: String | pure | missing |  |
| `json.setBool(text: String, path: String, value: Bool)` → result: String | pure | missing |  |

### `transform` — Transform (8)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.transform.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `transform.getPosition(entity=self: Int)` → position: Vec3 | pure | ref | flach `getPosition` (scripting-api.html#api) |
| `transform.setPosition(entity=self: Int, position: Vec3)` | exec | ref | flach `setPosition` (scripting-api.html#api) |
| `transform.getRotation(entity=self: Int)` → rotation: Vec3 | pure | ref | flach `getRotation` (scripting-api.html#api) |
| `transform.setRotation(entity=self: Int, rotation: Vec3)` | exec | ref | flach `setRotation` (scripting-api.html#api) |
| `transform.getScale(entity=self: Int)` → scale: Vec3 | pure | ref | flach `getScale` (scripting-api.html#api) |
| `transform.setScale(entity=self: Int, scale: Vec3)` | exec | ref | flach `setScale` (scripting-api.html#api) |
| `transform.getWorldPosition(entity=self: Int)` → position: Vec3 | pure | missing |  |
| `transform.setWorldPosition(entity=self: Int, position: Vec3)` | exec | missing |  |

### `db` — Database (7)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `db.open(path: String)` → handle: Int | exec | missing |  |
| `db.close(handle: Int)` | exec | missing |  |
| `db.exec(handle: Int, sql: String, params: String)` → ok: Bool | exec | missing |  |
| `db.query(handle: Int, sql: String, params: String)` → rows: String | exec | missing |  |
| `db.changes(handle: Int)` → rows: Int | pure | missing |  |
| `db.lastInsertId(handle: Int)` → id: Int | pure | missing |  |
| `db.lastError(handle: Int)` → error: String | pure | missing |  |

### `locomotion` — Locomotion (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `locomotion.move(entity=self: Int, direction: Vec3)` | exec | missing |  |
| `locomotion.look(entity=self: Int, yaw: Float, pitch: Float)` | exec | missing |  |
| `locomotion.setMaxSpeed(entity=self: Int, speed: Float)` | exec | missing |  |
| `locomotion.setOrientToMovement(entity=self: Int, on: Bool)` | exec | missing |  |
| `locomotion.jump(entity=self: Int)` → jumped: Bool | exec | missing |  |
| `locomotion.jumpWith(entity=self: Int, speed: Float)` → jumped: Bool | exec | missing |  |

### `movement` — Movement (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `movement.speed(entity=self: Int)` → speed: Float | pure | missing |  |
| `movement.verticalSpeed(entity=self: Int)` → speed: Float | pure | missing |  |
| `movement.isGrounded(entity=self: Int)` → grounded: Bool | pure | missing |  |
| `movement.velocity(entity=self: Int)` → velocity: Vec3 | pure | missing |  |
| `movement.forwardAmount(entity=self: Int)` → amount: Float | pure | missing |  |
| `movement.rightAmount(entity=self: Int)` → amount: Float | pure | missing |  |

### `nav` — Navigation (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `nav.moveTo(entity=self: Int, x: Float, y: Float, z: Float)` → started: Bool | exec | missing |  |
| `nav.stop(entity=self: Int)` | exec | missing |  |
| `nav.isMoving(entity=self: Int)` → moving: Bool | pure | missing |  |
| `nav.hasPath(entity=self: Int)` → hasPath: Bool | pure | missing |  |
| `nav.remainingDistance(entity=self: Int)` → distance: Float | pure | missing |  |
| `nav.setSpeed(entity=self: Int, speed: Float)` | exec | missing |  |

### `player` — Player (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `player.possess(controller: Ref, character: Ref)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `player.unpossess(controller: Ref)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `player.possessed(controller: Ref)` → character: Ref | pure | catalog | horizoncode-nodes.html#engine-call |
| `player.controllerOf(character: Ref)` → controller: Ref | pure | catalog | horizoncode-nodes.html#engine-call |
| `player.controller()` → controller: Ref | pure | catalog | horizoncode-nodes.html#engine-call |
| `player.character()` → character: Ref | pure | catalog | horizoncode-nodes.html#engine-call |

### `theme` — Theme (6)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `theme.set(themeAsset: String)` → ok: Bool | exec | missing |  |
| `theme.setMode(mode: String)` | exec | missing |  |
| `theme.getMode()` → mode: String | pure | missing |  |
| `theme.getPreference()` → preference: String | pure | missing |  |
| `theme.setFontScale(scale: Float)` | exec | missing |  |
| `theme.getFontScale()` → scale: Float | pure | missing |  |

### `dialog` — Dialog (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `dialog.message(title: String, text: String, kind: Int)` | exec | missing |  |
| `dialog.confirm(title: String, text: String, affirmative: String, negative: String)` → confirmed: Bool | exec | missing |  |
| `dialog.openFile(filter: String)` → path: String | exec | missing |  |
| `dialog.saveFile(filter: String)` → path: String | exec | missing |  |
| `dialog.pickFolder()` → path: String | exec | missing |  |

### `random` — Random (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `random.seed(seed: Int)` | exec | catalog | horizoncode-nodes.html#engine-call |
| `random.value()` → value: Float | exec | catalog | horizoncode-nodes.html#engine-call |
| `random.range(min: Float, max: Float)` → value: Float | exec | catalog | horizoncode-nodes.html#engine-call |
| `random.rangeInt(min: Int, max: Int)` → value: Int | exec | catalog | horizoncode-nodes.html#engine-call |
| `random.chance(p: Float)` → value: Bool | exec | catalog | horizoncode-nodes.html#engine-call |

### `timer` — Timer (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `timer.after(seconds: Float)` → handle: Int | exec | missing |  |
| `timer.every(seconds: Float)` → handle: Int | exec | missing |  |
| `timer.cancel(handle: Int)` → ok: Bool | exec | missing |  |
| `timer.active(handle: Int)` → active: Bool | pure | missing |  |
| `timer.cancelAll()` | exec | missing |  |

### `window` — Window (5)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `window.open(title: String, width: Int, height: Int)` → window: Int | exec | missing |  |
| `window.close(window: Int)` | exec | missing |  |
| `window.setTitle(window: Int, title: String)` | exec | missing |  |
| `window.setSize(window: Int, width: Int, height: Int)` | exec | missing |  |
| `window.show(window: Int, widget: Ref)` | exec | missing |  |

### `content` — Content (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `content.load(asset: String)` → loaded: Bool | exec | missing |  |
| `content.unload(asset: String)` → unloaded: Bool | exec | missing |  |
| `content.isLoaded(asset: String)` → loaded: Bool | pure | missing |  |
| `content.typeName(asset: String)` → type: String | pure | missing |  |

### `debug` — Debug (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `debug.line(from: Color, to: Color, color: Color, seconds: Float)` | exec | named | horizoncode-nodes.html |
| `debug.sphere(center: Color, radius: Float, color: Color, seconds: Float)` | exec | named | horizoncode-nodes.html |
| `debug.box(min: Color, max: Color, color: Color, seconds: Float)` | exec | named | horizoncode-nodes.html |
| `debug.clear()` | exec | named | horizoncode-nodes.html |

### `particle` — Particles (4)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `particle.play(entity=self: Int)` | exec | missing |  |
| `particle.stop(entity=self: Int)` | exec | missing |  |
| `particle.burst(entity=self: Int, count: Int)` → emitted: Int | exec | missing |  |
| `particle.isPlaying(entity=self: Int)` → playing: Bool | pure | missing |  |

### `clipboard` — Clipboard (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `clipboard.getText()` → text: String | pure | missing |  |
| `clipboard.setText(text: String)` | exec | missing |  |
| `clipboard.hasText()` → has: Bool | pure | missing |  |

### `print` — Print (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `print.toPdf(path: String, text: String, title: String)` → ok: Bool | exec | missing |  |
| `print.file(path: String)` → ok: Bool | exec | missing |  |
| `print.available()` → available: Bool | pure | missing |  |

### `process` — Process (3)

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `process.run(exe: String, args: String[], timeoutSeconds: Float)` → ok: Bool, exitCode: Int, out: String, err: String | exec | missing |  |
| `process.openUrl(url: String)` → ok: Bool | exec | missing |  |
| `process.which(exe: String)` → path: String | pure | missing |  |

### `material` — Material (2)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.material.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `material.getParam(entity=self: Int, name: String)` → value: Color | pure | catalog | horizoncode-nodes.html#engine-call |
| `material.setParam(entity=self: Int, name: String, value: Color)` → ok: Bool | exec | named | materials.html |

### `cursor` — Cursor (1)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.cursor.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `cursor.setVisible(show: Bool)` | exec | catalog | horizoncode-nodes.html#engine-call |

### `log` — Debug (1)

> Nicht in `isScriptGroup()`: in Lua/Python gibt es **kein** `horizon.log.*`; erreichbar nur über HorizonCode, C++ und ggf. einen flachen Zwilling.

| Id / Signatur | Art | Status | Beleg |
|---|---|---|---|
| `log(message: String)` | exec | ref | flach `log` (scripting-api.html#api) |
