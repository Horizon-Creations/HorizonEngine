#include "EditorHelp.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace HE::Ed::Help
{
namespace
{
	// ── The table ────────────────────────────────────────────────────────────
	// Ordered by where the control lives, not alphabetically: the entries for
	// one component belong next to each other, because that is how they are
	// written and how a gap in them is spotted.
	//
	// Keys ending in a component name and a label ("Light/Range") are found by
	// the Details panel automatically — the component section pushes its name as
	// the scope and EditorWidgets::Row looks the row's label up. Dotted keys
	// ("viewport.play") are for hand-drawn controls that ask for their help by
	// name.
	//
	// `title` is empty wherever the label on screen already is the heading.
	constexpr Entry kEntries[] = {

	// ── Details: what each component IS ──────────────────────────────────────
	// Keyed under "Component/" because a component's NAME is also a row label in
	// places ("Material" is both a component and an asset slot).
	{ "Component/Transform", "Transform",
	  "Where the entity is, how it is turned and how big it is. Children inherit "
	  "it, so moving a parent moves everything under it.",
	  "", "scenes#components" },
	{ "Component/Transform 2D", "Transform 2D",
	  "The flat counterpart of Transform, for sprites and 2D physics: a position "
	  "in the XY plane, one rotation angle and a 2D scale.",
	  "", "scenes#components" },
	{ "Component/Mesh", "Mesh",
	  "Draws a static mesh asset at the entity's transform. Pair it with a "
	  "Material component to say what it looks like.",
	  "", "scenes#components" },
	{ "Component/Skeletal Mesh", "Skeletal Mesh",
	  "A mesh deformed by a skeleton. An Animator component on the same entity "
	  "drives the bones; without one the mesh stands in its bind pose.",
	  "", "systems#animation" },
	{ "Component/Material", "Material",
	  "Which material asset shades this entity's mesh, plus per-entity overrides "
	  "of its parameters — one material asset, differently tuned per entity.",
	  "", "materials#parameters" },
	{ "Component/Light", "Light",
	  "Makes the entity a light source. Directional lights light the whole scene "
	  "from a direction; point and spot lights have a position and a range.",
	  "", "rendering#lighting" },
	{ "Component/Decal", "Decal",
	  "Projects a texture onto whatever geometry is underneath — bullet holes, "
	  "puddles, painted markings — without touching the surface's own material.",
	  "", "rendering#postfx" },
	{ "Component/Rigid Body", "Rigid Body",
	  "Hands the entity to the physics solver. Without a Collider it uses a box "
	  "derived from the transform.",
	  "", "systems#physics" },
	{ "Component/Collider", "Collider",
	  "The shape physics actually collides with, which does not have to match the "
	  "mesh. Axis-aligned in the body's local frame; a capsule stands along Y.",
	  "", "systems#physics" },
	{ "Component/Character Controller", "Character Controller",
	  "Movement for something that walks: it climbs steps, slides off steep "
	  "slopes and stays upright, instead of tumbling the way a rigid body would.",
	  "", "systems#physics" },
	{ "Component/Movement", "Movement",
	  "What a character is doing, in the form an animator wants to read: how fast "
	  "it may go and what it was told to do this frame.",
	  "", "systems#animation" },
	{ "Component/Camera", "Camera",
	  "A viewpoint the game can render from. Exactly one camera per scene may be "
	  "the main one; that is the one play mode looks through.",
	  "", "rendering#cameras" },
	{ "Component/Camera Rig", "Camera Rig",
	  "Puts this camera on a target entity — in its head (first person) or on a "
	  "boom behind it (third person). First person is simply an arm length of 0.",
	  "", "rendering#cameras" },
	{ "Component/Script", "Script",
	  "Runs a Lua or Python script on this entity: onStart once, onUpdate every "
	  "frame. Properties the script exposes appear below it.",
	  "", "scripting#attach" },
	{ "Component/Terrain", "Terrain",
	  "A generated landscape: a heightfield the size of the entity, split into "
	  "chunks with their own level of detail, sculptable and paintable.",
	  "", "scenes#terrain" },
	{ "Component/Foliage", "Foliage",
	  "Scatters a mesh across this entity's terrain — grass, rocks, trees. The "
	  "instances are generated, not saved, so a seed reproduces the same layout.",
	  "", "scenes#foliage" },
	{ "Component/Nav Mesh", "Nav Mesh",
	  "The walkable surface, baked from the scene's static geometry. Agents can "
	  "only path where a nav mesh has been baked.",
	  "", "systems#navigation" },
	{ "Component/Nav Agent", "Nav Agent",
	  "Walks the entity along a path across the baked nav mesh — what makes an "
	  "enemy come after the player or a villager keep a patrol. Scripts drive it "
	  "with the Nav calls; give it a Character Controller as well and it walks "
	  "through the world the way the player does, colliding instead of gliding.",
	  "", "systems#navigation" },
	{ "Component/Audio Source", "Audio Source",
	  "Plays an audio asset from this entity. Switch on Spatial and it is heard "
	  "from where it stands, quieter with distance.",
	  "", "systems#audio" },
	{ "Component/Audio Listener", "Audio Listener",
	  "The ears of the scene — where spatial audio is heard from. One per scene, "
	  "normally on the camera or the player.",
	  "", "systems#audio" },
	{ "Component/Animator", "Animator",
	  "Plays one animation clip on this entity's skeleton.",
	  "", "systems#animation" },
	{ "Component/Animator Blend", "Animator Blend",
	  "Cross-fades two clips by weight — walk into run, aim into idle.",
	  "", "systems#animation" },
	{ "Component/Animator State Machine", "Animator State Machine",
	  "Runs an animator state machine asset: states with clips, and transitions "
	  "that fire on parameters. The graph decides which clip plays.",
	  "", "systems#animation" },
	{ "Component/Property Animator", "Property Animator",
	  "Animates component properties over time from a property clip — a moving "
	  "platform, a fading light — with no skeleton involved.",
	  "", "systems#animation" },
	{ "Component/Particle System", "Particle System",
	  "Emits particles from this entity. The emitter's shape, rate and look come "
	  "from a particle system asset, edited in its own tab.",
	  "", "systems#particles" },
	{ "Component/Save State", "Save State",
	  "Marks what a savegame keeps for this entity, so a saved game restores it "
	  "as it was rather than as the scene file has it.",
	  "", "scenes#scene-files" },
	{ "Component/LOD", "LOD",
	  "Swaps this entity's mesh for cheaper versions as the camera gets further "
	  "away.",
	  "", "rendering#performance" },
	{ "Component/Environment", "Environment (Sky)",
	  "The sky, sun, moon, clouds, stars and fog of this scene. It lives on an "
	  "ordinary Sky entity you can add or remove from the Environment window.",
	  "", "rendering#sky" },
	{ "Component/Weather", "Weather",
	  "Drives the sky toward a weather state — clouds, fog, wind, rain or snow — "
	  "and takes over those Environment values while it is present.",
	  "", "rendering#weather" },
	{ "Component/UI Canvas", "UI Canvas",
	  "A surface for UI elements. On an entity in the world it is a screen you "
	  "can walk up to; the elements below it are laid out inside it.",
	  "", "ui#entity-ui" },
	{ "Component/UI Element", "UI Element",
	  "Position, size and anchoring of one widget inside its canvas.",
	  "", "ui#elements" },
	{ "Component/UI Text", "UI Text", "A line of text inside a UI canvas.",
	  "", "ui#elements" },
	{ "Component/UI Image", "UI Image", "A textured rectangle inside a UI canvas.",
	  "", "ui#elements" },
	{ "Component/UI Button", "UI Button",
	  "A clickable region with a colour per state and an action to fire.",
	  "", "ui#elements" },

	// ── Transform ────────────────────────────────────────────────────────────
	{ "Transform/Position", "",
	  "Where the entity sits, in metres. Relative to the parent when it has one.",
	  "", "scenes#components" },
	{ "Transform/Rotation", "",
	  "Rotation in degrees around X, Y and Z. Y is the one that turns a standing "
	  "character to face elsewhere.",
	  "", "scenes#components" },
	{ "Transform/Scale", "",
	  "Size multiplier per axis. 1 is the mesh's authored size; non-uniform scale "
	  "on a physics body is approximated by the collider shape.",
	  "", "scenes#components" },

	// ── Mesh ─────────────────────────────────────────────────────────────────
	{ "Mesh/LOD Bias", "",
	  "Nudges level-of-detail selection: 0 lets the engine choose, higher values "
	  "force a coarser mesh sooner. For meshes that can afford to be cheap.",
	  "", "rendering#performance" },
	{ "Mesh/Visible", "",
	  "Off hides the mesh from rendering entirely — no draw call, no shadow. The "
	  "entity and its physics are untouched.",
	  "", "scenes#components" },
	{ "Mesh/Casts Shadow", "",
	  "Whether this mesh appears in the shadow maps. Turning it off for small "
	  "clutter is one of the cheapest performance wins there is.",
	  "", "rendering#shadows" },
	{ "Mesh/Receives Shadow", "",
	  "Whether shadows from other objects darken this mesh. Off is for surfaces "
	  "that should stay evenly lit, such as an emissive panel.",
	  "", "rendering#shadows" },

	// ── Material ─────────────────────────────────────────────────────────────
	{ "Material/Shader", "",
	  "The material asset that shades this mesh. Drop one from the Content "
	  "Browser, or click to pick from every material in the project.",
	  "", "materials#concept" },
	{ "Material/Base Color", "",
	  "The surface's colour under white light. For metals this is the reflection "
	  "tint rather than a paint colour.",
	  "", "rendering#lighting" },
	{ "Material/Metallic", "",
	  "0 for non-metals, 1 for bare metal. The values in between are for a "
	  "transition — dust on chrome — not for a general dial.",
	  "", "rendering#lighting" },
	{ "Material/Roughness", "",
	  "How rough the surface is: 0 is a mirror, 1 is chalk. This is the value "
	  "that decides whether something reads as wet, polished or worn.",
	  "", "rendering#lighting" },
	{ "Material/Opacity", "",
	  "1 is solid. Below 1 the surface is see-through, which also moves it into "
	  "the transparent pass — so it no longer writes depth.",
	  "", "rendering#lighting" },

	// ── Light ────────────────────────────────────────────────────────────────
	{ "Light/Type", "",
	  "Directional lights the whole scene from a direction, like the sun. Point "
	  "shines in every direction from where it stands; Spot shines in a cone.",
	  "", "rendering#lighting" },
	{ "Light/Color", "", "The light's colour. Warm for lamps and fire, cool for sky bounce.",
	  "", "rendering#lighting" },
	{ "Light/Intensity", "",
	  "How bright the light is. Values above a few hundred are normal for a small "
	  "point light that has to carry a whole room.",
	  "", "rendering#lighting" },
	{ "Light/Range", "",
	  "How far a point or spot light reaches, in metres. Beyond it the light "
	  "contributes nothing — keep it tight, it is also what makes lights cheap.",
	  "", "rendering#lighting" },
	{ "Light/Spot Angle", "",
	  "The width of a spot light's cone in degrees, edge to edge.",
	  "", "rendering#lighting" },
	{ "Light/Cull Distance", "",
	  "Switch this light off entirely once the camera is further away than this, "
	  "in metres. 0 keeps it on always.",
	  "", "rendering#performance" },
	{ "Light/Casts Shadow", "",
	  "Whether this light casts shadows. Each shadow-casting point or spot light "
	  "costs an extra render of the scene, so spend them where they show.",
	  "", "rendering#shadows" },
	{ "Light/Visible", "",
	  "Off switches the light off without deleting it — the way a zone hides what "
	  "is not loaded.",
	  "", "rendering#lighting" },

	// ── Rigid Body / Collider / Character ────────────────────────────────────
	{ "Rigid Body/Body Type", "",
	  "Static never moves (walls, ground). Dynamic is pushed around by forces and "
	  "gravity. Kinematic moves only where you put it, and shoves dynamic bodies "
	  "aside as it goes.",
	  "", "systems#physics" },
	{ "Rigid Body/Mass", "",
	  "How heavy the body is, in kilograms. Only Dynamic bodies use it — for the "
	  "other two the solver treats the body as immovable.",
	  "", "systems#physics" },
	{ "Rigid Body/Friction", "",
	  "How much the surface resists sliding: 0 is ice, 1 is rubber. The value used "
	  "for a contact is combined from both bodies.",
	  "", "systems#physics" },
	{ "Rigid Body/Restitution", "",
	  "Bounciness. 0 lands and stays, 1 bounces back to the height it fell from.",
	  "", "systems#physics" },
	{ "Rigid Body/2D Physics", "",
	  "Solve this body in the 2D solver — motion stays in the XY plane. For "
	  "sprite games; pair it with a Transform 2D.",
	  "", "systems#physics" },
	{ "Collider/Shape", "",
	  "Box, Sphere or Capsule for a shape from the numbers below; Mesh, Convex "
	  "Hull or Height Field to take the geometry from the entity itself. A "
	  "capsule stands along Y and is what a character usually wants; the sides "
	  "let it slide past corners instead of catching. Mesh and Height Field are "
	  "static-only, so a moving body wants Convex Hull.",
	  "", "systems#physics" },
	{ "Collider/Half Extents", "",
	  "Half the box's size on each axis — so 0.5 is a one-metre cube.",
	  "", "systems#physics" },
	{ "Collider/Radius", "", "Radius of the sphere, or of the capsule's cylinder and caps.",
	  "", "systems#physics" },
	{ "Collider/Total Height", "",
	  "The capsule's full height including both rounded ends, so it cannot be "
	  "shorter than twice the radius.",
	  "", "systems#physics" },
	{ "Collider/Is Trigger", "",
	  "A trigger reports overlaps but stops nothing — doorways, pickup volumes, "
	  "kill zones. Things pass straight through it.",
	  "", "systems#physics" },
	{ "Character Controller/Slope Limit (deg)", "",
	  "The steepest ground the character can still walk up. Anything steeper is "
	  "slid off rather than climbed.",
	  "", "systems#physics" },
	{ "Character Controller/Step Height (m)", "",
	  "How tall a step the character walks over without jumping. Too small and it "
	  "catches on kerbs; too large and it climbs walls.",
	  "", "systems#physics" },
	{ "Character Controller/Skin Width (m)", "",
	  "A thin padding kept between the character and the world, so it never ends "
	  "up exactly touching geometry and jittering against it.",
	  "", "systems#physics" },
	{ "Character Controller/Mass (kg)", "",
	  "How heavy the character is when it pushes dynamic bodies around.",
	  "", "systems#physics" },
	{ "Character Controller/Gravity (m/s²)", "",
	  "Downward acceleration for this character alone. Lower it for a floaty jump, "
	  "raise it for a heavy one.",
	  "", "systems#physics" },
	{ "Character Controller/Jump Speed (m/s)", "",
	  "How fast the character is thrown upward by a jump. It is a speed, not a "
	  "height: how high that carries depends on Gravity above, so raising one "
	  "means revisiting the other. Jump uses this; Jump With ignores it and takes "
	  "the speed it is given.",
	  "", "systems#physics" },
	{ "Character Controller/Velocity", "",
	  "The character's current speed, written by the physics step every tick. "
	  "Shown read-only, because a value typed here would be overwritten before it "
	  "could do anything — gameplay code is what sets it.",
	  "", "systems#physics" },
	{ "Character Controller/Air Time (s)", "",
	  "How long the character has been off the ground, written by the physics "
	  "step. It doubles as the grace a late jump spends: a jump asked for within "
	  "the first tenth of a second after walking off a ledge is still granted, so "
	  "a player who presses a frame too late gets the jump they meant. Landing "
	  "puts it back to zero.",
	  "", "systems#physics" },
	{ "Character Controller/Is Grounded", "",
	  "Whether the controller is standing on something right now. Read-only "
	  "state, shown so a jump that never fires can be diagnosed: Jump refuses "
	  "once this has been off for longer than the brief grace after leaving the "
	  "ground, and Air Time below is that clock.",
	  "", "systems#physics" },

	// ── Movement / Camera / Camera Rig ───────────────────────────────────────
	{ "Movement/Max Speed", "",
	  "Top speed in metres per second at full input.",
	  "", "systems#animation" },
	{ "Movement/Turn Rate", "",
	  "How fast the character turns to face where it is going, in degrees per "
	  "second. Only used with Orient To Movement.",
	  "", "systems#animation" },
	{ "Movement/Move Direction Is", "",
	  "Which way is forward when something calls Move. World takes the direction "
	  "as given, which is what a fixed or top-down camera wants. Camera turns it "
	  "by the main camera rig's yaw first, so pushing forward walks where the "
	  "player is looking — that is what a third- or first-person game means by "
	  "the controls working, and a player character ships set to it. Not the same "
	  "as Orient To Movement below: this decides where you GO, that one decides "
	  "which way you FACE while going there, and they are usually both on.",
	  "", "rendering#cameras" },
	{ "Movement/Orient To Movement", "",
	  "Turn the character to face the direction it walks. Switch it off when "
	  "something else owns the facing — a camera rig with coupled rotation. It "
	  "does NOT change which way forward is; Move Direction Is does that.",
	  "", "rendering#cameras" },
	{ "Camera/FOV", "",
	  "Vertical field of view in degrees. 60 is a normal game view; higher feels "
	  "faster and shows more, and distorts the edges.",
	  "", "rendering#cameras" },
	{ "Camera/Near Plane", "",
	  "Nothing closer than this is drawn. Keep it as large as the game allows — a "
	  "tiny near plane is what causes flickering surfaces in the distance.",
	  "", "rendering#cameras" },
	{ "Camera/Far Plane", "",
	  "How far the camera sees, in metres. Everything beyond is clipped away.",
	  "", "rendering#cameras" },
	{ "Camera/Main Camera", "",
	  "Makes this the camera the game renders through. Only one per scene can be "
	  "the main one; setting it here clears the others.",
	  "", "rendering#cameras" },
	{ "Camera/Orthographic", "",
	  "Drops perspective: parallel lines stay parallel and distance no longer "
	  "shrinks things. For 2D games and technical views.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Mode", "",
	  "First person puts the camera at the pivot; third person swings it back "
	  "along a boom. Same calculation, different arm length.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Target", "",
	  "The entity to follow. Left empty it follows whichever character the player "
	  "possesses, which is what a normal project wants.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Pivot Offset", "",
	  "Eye or shoulder height above the target's origin, in world axes. This is "
	  "the point the camera looks from and orbits around.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Arm Offset", "",
	  "Third person: how far the boom sits off to the side, in camera space. A "
	  "small X is the over-the-shoulder look.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Arm Length", "",
	  "How far behind the target the camera sits. 0 is first person.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Camera Radius", "",
	  "The boom sweeps a sphere this big and stops at the first solid thing, so "
	  "the camera does not end up inside a wall.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Target Rotation", "",
	  "Free lets the character face where it walks while the camera looks "
	  "elsewhere. Follow turns the character with the camera, which is what makes "
	  "strafing and walking backwards work.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Sensitivity", "",
	  "Mouse look, in degrees per pixel of motion.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Stick Sensitivity", "",
	  "Gamepad look, in degrees per second at full stick deflection — a rate, "
	  "unlike the mouse, which is per pixel moved. 180 is half a turn per second.",
	  "", "systems#input" },
	{ "Camera Rig/Invert Stick Y", "",
	  "Pushing the stick down looks up. The preference half of all players have.",
	  "", "systems#input" },
	{ "Camera Rig/Yaw", "",
	  "Which way the rig is looking, in degrees. Saved with the scene, so a "
	  "reloaded level starts facing where you left it.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Pitch", "", "How far up or down the rig is looking, in degrees.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Pitch Min", "",
	  "How far down the camera may look before it stops.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Pitch Max", "",
	  "How far up the camera may look before it stops. Keeping it under 90 avoids "
	  "the moment the view flips over.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Hide Target Mesh", "",
	  "Hide the followed character while this rig is active — what first person "
	  "needs, so the camera is not inside a head.",
	  "", "rendering#cameras" },
	{ "Camera Rig/Collide With World", "",
	  "Pull the camera in when something solid is between it and the target, "
	  "instead of letting the wall fill the screen.",
	  "", "rendering#cameras" },

	// ── Script ───────────────────────────────────────────────────────────────
	{ "Script/Class", "",
	  "Which HorizonCode class this entity runs, picked from the project's "
	  "classes. The class's own variables then appear underneath, so an entity "
	  "can be set up differently from every other instance of the same class.",
	  "", "horizoncode#hosts" },
	{ "Script/Script Name", "",
	  "The Lua or Python file this entity runs. Its exposed properties appear "
	  "below, and each entity can override them.",
	  "", "scripting#attach" },

	// ── Terrain ──────────────────────────────────────────────────────────────
	{ "Terrain/Width (X)", "", "How wide the landscape is, in metres.",
	  "", "scenes#terrain" },
	{ "Terrain/Depth (Z)", "", "How deep the landscape is, in metres.",
	  "", "scenes#terrain" },
	{ "Terrain/Resolution", "",
	  "Vertices per side. Detail and cost both scale with the square of this, so "
	  "raise it only where the ground is actually seen up close.",
	  "", "scenes#terrain" },
	{ "Terrain/Height Scale", "",
	  "How tall the generated hills are, in metres from lowest to highest.",
	  "", "scenes#terrain" },
	{ "Terrain/Texture Tiling", "",
	  "How many times the material repeats across the whole landscape. At 1 one "
	  "texture is stretched over every metre of it.",
	  "", "scenes#terrain" },
	{ "Terrain/LOD Distance", "",
	  "How far from the camera full detail is kept: 2 keeps it twice as far as the "
	  "default. Near ground is always full resolution.",
	  "", "rendering#performance" },
	{ "Terrain/Seed", "",
	  "Which random landscape is generated. 0 is flat ground; any other number is "
	  "a different set of hills, reproduced exactly on every load.",
	  "", "scenes#terrain" },
	{ "Terrain/Octaves", "",
	  "How many layers of noise are stacked. More layers add finer detail on top "
	  "of the big shapes, and cost more to generate.",
	  "", "scenes#terrain" },
	{ "Terrain/Frequency", "",
	  "How close together the hills are. Higher packs more of them into the same "
	  "ground.",
	  "", "scenes#terrain" },
	{ "Terrain/Lacunarity", "",
	  "How much finer each noise layer is than the one below. 2 is the usual "
	  "doubling.",
	  "", "scenes#terrain" },
	{ "Terrain/Gain", "",
	  "How much each finer layer contributes. Under 0.5 gives smooth hills, above "
	  "it rough, noisy ground.",
	  "", "scenes#terrain" },

	// ── Foliage ──────────────────────────────────────────────────────────────
	{ "Foliage/Density", "",
	  "Instances per square metre. Small changes matter — this multiplies across "
	  "the whole landscape.",
	  "", "scenes#foliage" },
	{ "Foliage/Draw Distance", "",
	  "Beyond this distance instances are not drawn at all, in metres.",
	  "", "scenes#foliage" },
	{ "Foliage/Min Scale", "", "Smallest random size an instance can be scattered at.",
	  "", "scenes#foliage" },
	{ "Foliage/Max Scale", "",
	  "Largest random size. Keeping a gap to Min Scale is what stops a field of "
	  "identical clones.",
	  "", "scenes#foliage" },
	{ "Foliage/Seed", "",
	  "Which scatter pattern is generated. The same seed always produces the same "
	  "layout, so nothing is saved per instance.",
	  "", "scenes#foliage" },

	// ── Navigation ───────────────────────────────────────────────────────────
	{ "Nav Mesh/Cell Size", "",
	  "How finely the bake samples the ground horizontally, in metres. Smaller "
	  "captures narrower ledges and takes longer to bake.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Cell Height", "",
	  "The vertical resolution of the bake. Smaller separates floors that are "
	  "close above each other.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Walk Height", "",
	  "How much headroom an agent needs. Anything lower is not walkable — set it "
	  "to your character's height.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Walk Climb", "",
	  "The tallest step an agent walks up without a ramp.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Walk Radius", "",
	  "How far the walkable surface is pulled back from walls — an agent's own "
	  "radius, so it does not clip corners.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Max Slope", "",
	  "The steepest ground still counted as walkable, in degrees.",
	  "", "systems#navigation" },
	{ "Nav Mesh/Show NavMesh", "",
	  "Draw the baked walkable surface in the viewport. Editor only — it never "
	  "appears in the game.",
	  "", "systems#navigation" },
	{ "Nav Agent/Target", "",
	  "Where the agent walks to. Moving it drops the path it was on and searches a "
	  "new one from where the agent stands, so a pursuer can simply keep writing "
	  "the player's position here. Setting it does not start the walk on its own: "
	  "press Go below, or call Move To from a script.",
	  "", "systems#navigation" },
	{ "Nav Agent/Speed", "",
	  "How fast the agent follows its path, in metres per second. Scripts change "
	  "it with Set Speed — the same guard strolling a patrol and then charging.",
	  "", "systems#navigation" },
	{ "Nav Agent/Stop Dist", "",
	  "How close to the target counts as arrived. Too small and the agent circles "
	  "its destination forever.",
	  "", "systems#navigation" },
	{ "Nav Agent/Auto Start", "",
	  "Walk to the target as soon as the game starts, without a script saying so. "
	  "This is what an agent in a packaged game needs: the Go button below belongs "
	  "to the editor and does not exist there, so an agent that is never sent off "
	  "by a script and does not start itself simply stands still. It fires once "
	  "per play session, so stopping the agent afterwards does not restart it.",
	  "", "systems#navigation" },

	// ── Audio ────────────────────────────────────────────────────────────────
	{ "Audio Source/Asset ID", "", "The sound this source plays.", "", "systems#audio" },
	{ "Audio Source/Bus", "",
	  "Which mixer bus the sound goes through — music, sfx, voice — so a whole "
	  "group can be turned down at once. Empty is the master bus.",
	  "", "systems#audio" },
	{ "Audio Source/Volume", "", "Playback volume. 1 is the file as recorded.",
	  "", "systems#audio" },
	{ "Audio Source/Pitch", "",
	  "Playback speed and pitch together: 2 plays twice as fast an octave up.",
	  "", "systems#audio" },
	{ "Audio Source/Inner Range", "",
	  "Within this distance the sound plays at full volume; attenuation only "
	  "starts outside it.",
	  "", "systems#audio" },
	{ "Audio Source/Range", "",
	  "How far the sound carries, in metres. Beyond it nothing is heard.",
	  "", "systems#audio" },
	{ "Audio Source/Rolloff Factor", "",
	  "How quickly the sound fades with distance between the inner range and the "
	  "range.",
	  "", "systems#audio" },
	{ "Audio Source/Spatial", "",
	  "Hear the sound from where the entity stands, quieter with distance. Off "
	  "plays it flat, everywhere at once — which is what music wants.",
	  "", "systems#audio" },
	{ "Audio Source/Loop", "", "Start over when the sound reaches its end.",
	  "", "systems#audio" },
	{ "Audio Source/Play on Start", "",
	  "Start playing as soon as the scene runs, with no script involved.",
	  "", "systems#audio" },
	{ "Audio Listener/Master Volume", "",
	  "The volume of everything the scene plays.", "", "systems#audio" },

	// ── Animation ────────────────────────────────────────────────────────────
	{ "Animator/Speed", "",
	  "Playback rate: 1 is the clip as authored, negative plays it backwards.",
	  "", "systems#animation" },
	{ "Animator/Time", "",
	  "Where the clip is now, in seconds. Drag it to scrub the pose.",
	  "", "systems#animation" },
	{ "Animator Blend/Blend", "",
	  "0 is entirely the first clip, 1 entirely the second. In between the two "
	  "poses are mixed — the walk-to-run dial.",
	  "", "systems#animation" },
	{ "Animator State Machine/Current", "",
	  "The state that is playing right now.", "", "systems#animation" },
	{ "Animator State Machine/Transitioning To", "",
	  "The state being blended into, while a transition is running.",
	  "", "systems#animation" },

	// ── Environment (Sky) ────────────────────────────────────────────────────
	{ "Environment/Time of Day", "",
	  "The clock the whole sky follows: 0 and 1 are midnight, 0.25 sunrise, 0.5 "
	  "noon. It moves the sun and the moon, and with them every shadow.",
	  "", "rendering#sky" },
	{ "Environment/Day-Night Cycle", "",
	  "Let time of day advance on its own while the scene runs.",
	  "", "rendering#sky" },
	{ "Environment/Day Length", "",
	  "How long a full day takes when the cycle runs, in seconds.",
	  "", "rendering#sky" },
	{ "Environment/Sun Color", "",
	  "Tint of the sunlight. The sky reddens the sun near the horizon on its own — "
	  "this is on top of that.",
	  "", "rendering#sky" },
	{ "Environment/Sun Brightness", "",
	  "How strong the sun is, and with it the whole daylit scene.",
	  "", "rendering#sky" },
	{ "Environment/Moon Color", "", "Tint of the moonlight at night.", "", "rendering#sky" },
	{ "Environment/Moon Brightness", "",
	  "How strong the moonlight is. This is the difference between a night you can "
	  "see in and a black screen.",
	  "", "rendering#sky" },
	{ "Environment/Phase", "",
	  "The moon's phase: 0 is new, 0.5 full. A full moon lights the ground; a new "
	  "one is only a dark disc against the stars.",
	  "", "rendering#sky" },
	{ "Environment/Auto Lunar Cycle", "",
	  "Advance the moon phase with the days instead of holding it where it is.",
	  "", "rendering#sky" },
	{ "Environment/Lunar Cycle Length", "",
	  "How many days a full new-to-full-to-new cycle takes.",
	  "", "rendering#sky" },
	{ "Environment/Coverage", "",
	  "How much of the sky the cloud layer fills: 0 is clear, 1 overcast. Most "
	  "other cloud settings only show at all with coverage above zero.",
	  "", "rendering#sky" },
	{ "Environment/Render Mode", "",
	  "Dome clouds sit painted on the sky — cheap, but they never come closer. "
	  "Volumetric clouds are real 3D shapes the camera can fly into.",
	  "", "rendering#sky" },
	{ "Environment/Cloud Altitude", "",
	  "The world height of the cloud deck's base, in metres. Set it above your "
	  "highest terrain, or the camera ends up above the weather.",
	  "", "rendering#sky" },
	{ "Environment/Cloud Style", "",
	  "Classic is the original flat, drifting layer. Realistic builds cauliflower "
	  "shapes that tower and dissolve, at a higher cost.",
	  "", "rendering#sky" },
	{ "Environment/Evolution", "",
	  "How fast clouds change shape as they drift. 0 freezes the formation and "
	  "only moves it.",
	  "", "rendering#sky" },
	{ "Environment/Quality", "",
	  "How many steps the cloud raymarch takes. The most expensive sky setting "
	  "there is, and the one to lower first when the sky costs too much.",
	  "", "rendering#performance" },
	{ "Environment/Density", "",
	  "How solid the clouds are: low is wispy and lets light through, high is "
	  "thick and dark underneath.",
	  "", "rendering#sky" },
	{ "Environment/Fluffiness", "",
	  "How broken up the cloud edges are — 0 is a smooth sheet, 1 is billowy and "
	  "full of holes.",
	  "", "rendering#sky" },
	{ "Environment/Cloud Tint", "",
	  "Multiplied into the cloud colour. A slightly warm tint at sunset, cool for "
	  "a storm.",
	  "", "rendering#sky" },
	{ "Environment/Wind Direction", "", "Which way the clouds drift, in degrees.",
	  "", "rendering#weather" },
	{ "Environment/Wind Speed", "",
	  "How fast the clouds drift. Also what weather changes drive when a Weather "
	  "component is present.",
	  "", "rendering#weather" },
	{ "Environment/Cast Cloud Shadows", "",
	  "Let the cloud layer darken the ground under it, moving as the clouds move. "
	  "It is what makes an overcast day read as overcast.",
	  "", "rendering#shadows" },
	{ "Environment/Shadow Strength", "",
	  "How dark the cloud shadows get. Full strength still keeps the sky light, so "
	  "shadowed ground reads as under a cloud rather than as night.",
	  "", "rendering#shadows" },
	{ "Environment/Clouds Shade Each Other", "",
	  "Let clouds cast shadows within their own body, so a tall tower darkens what "
	  "is behind it. Costs a second light march.",
	  "", "rendering#sky" },
	{ "Environment/Low-res clouds (quarter-res pass)", "",
	  "Raymarch the clouds at quarter resolution and upscale. Much cheaper on a "
	  "sky-filled view, slightly softer edges.",
	  "", "rendering#performance" },
	{ "Environment/Contrails", "",
	  "Vapour trails high in the sky. They fill an otherwise empty blue day, and "
	  "are unrelated to the cloud layer.",
	  "", "rendering#sky" },
	{ "Environment/Cirrus", "",
	  "Thin fibrous streaks high above the main clouds.", "", "rendering#sky" },
	{ "Environment/Cirrus Seed", "",
	  "Which cirrus pattern is drawn. Change it for a different sky, not a "
	  "different amount.",
	  "", "rendering#sky" },
	{ "Environment/God Rays", "",
	  "Shafts of light through gaps in the cloud toward the sun. Needs broken "
	  "cover to have gaps to shine through.",
	  "", "rendering#postfx" },
	{ "Environment/Lens Flare", "",
	  "The camera artefact when the sun is in shot. Fades out on its own once the "
	  "sun is hidden or off screen.",
	  "", "rendering#postfx" },
	{ "Environment/Density##fog", "Fog Density",
	  "How thick the atmospheric haze is. Even a very small amount gives distance "
	  "to a landscape; 0 switches fog off.",
	  "", "rendering#sky" },
	{ "Environment/Ground Hugging", "",
	  "How much the fog pools near the ground instead of filling the air evenly. "
	  "High values give valley mist.",
	  "", "rendering#sky" },
	{ "Environment/Star Brightness", "",
	  "How bright the star field is. Stars fade out by themselves as the sky "
	  "lightens.",
	  "", "rendering#sky" },
	{ "Environment/Star Color", "",
	  "Tints the whole field. The warm and cool variation between individual stars "
	  "survives it.",
	  "", "rendering#sky" },
	{ "Environment/Star Amount", "", "How many stars there are.", "", "rendering#sky" },
	{ "Environment/Star Size", "", "How large stars are drawn.", "", "rendering#sky" },
	{ "Environment/Size Variation", "",
	  "How much star sizes differ. 0 makes every star the same, which reads as "
	  "artificial.",
	  "", "rendering#sky" },
	{ "Environment/Star Glow", "",
	  "The halo around each star: 0 is crisp points, higher is a softer sky.",
	  "", "rendering#sky" },
	{ "Environment/Twinkle", "", "How much the stars flicker.", "", "rendering#sky" },
	{ "Environment/Milky Way", "",
	  "The bright band of the galaxy across the night sky.", "", "rendering#sky" },
	{ "Environment/Shooting Stars", "",
	  "How often meteors streak across the sky. 0 is none.", "", "rendering#sky" },
	{ "Environment/Intensity##neb", "Nebula Intensity",
	  "How visible the deep-space nebula is behind the stars.", "", "rendering#sky" },
	{ "Environment/Coverage##neb", "Nebula Coverage",
	  "How much of the sky the nebula spans, from a single band to nearly all of "
	  "it.",
	  "", "rendering#sky" },
	{ "Environment/Fidelity", "",
	  "Detail level of the nebula. The expensive setting of the night sky, the way "
	  "cloud quality is of the day one.",
	  "", "rendering#performance" },
	{ "Environment/Seed##neb", "Nebula Seed",
	  "Which nebula shape is generated.", "", "rendering#sky" },
	{ "Environment/Nebula Color 1", "",
	  "The cool interior veil of the nebula.", "", "rendering#sky" },
	{ "Environment/Nebula Color 2", "",
	  "The warm filament regions.", "", "rendering#sky" },
	{ "Environment/Nebula Color 3", "",
	  "The deep red filament regions.", "", "rendering#sky" },
	{ "Environment/Intensity##aur", "Aurora Intensity",
	  "How strong the aurora ribbons are. 0 switches them off.", "", "rendering#sky" },
	{ "Environment/Color (base)", "",
	  "Colour of the aurora at its lower edge, where it is brightest.",
	  "", "rendering#sky" },
	{ "Environment/Color (top)", "",
	  "Colour the aurora fades into at the top of the ribbon.", "", "rendering#sky" },
	{ "Environment/Height", "",
	  "How tall the aurora ribbons stand above the horizon.", "", "rendering#sky" },
	{ "Environment/Fragmentation", "",
	  "How broken the ribbons are — a smooth curtain against ragged, separate "
	  "streaks.",
	  "", "rendering#sky" },

	// ── Weather ──────────────────────────────────────────────────────────────
	{ "Weather/Preset", "",
	  "The weather to move toward. The change is blended in over the transition "
	  "time rather than switching on the spot.",
	  "", "rendering#weather" },
	{ "Weather/Intensity", "",
	  "How far toward the preset the weather goes: 0.5 is half a storm.",
	  "", "rendering#weather" },
	{ "Weather/Transition", "",
	  "How long a full weather change takes, in seconds.", "", "rendering#weather" },
	{ "Weather/Auto-Cycle", "",
	  "Wander between weather states on their own, so the sky keeps changing "
	  "without anything driving it.",
	  "", "rendering#weather" },
	{ "Weather/Cycle Time", "",
	  "Roughly how long one weather state lasts before the cycle moves on.",
	  "", "rendering#weather" },
	{ "Environment/Rain", "",
	  "Rain strength for the state being shown, 0 to 1.", "", "rendering#weather" },
	{ "Environment/Snow", "",
	  "Snow strength for the state being shown, 0 to 1.", "", "rendering#weather" },
	{ "Environment/Wetness", "",
	  "How wet surfaces look once it has been raining.", "", "rendering#weather" },
	{ "Weather/Max Rain Particles", "",
	  "Hard ceiling on live raindrops. Emission throttles itself to stay under it, "
	  "so this is the dial that trades downpour against frame rate.",
	  "", "rendering#weather" },
	{ "Weather/Max Snow Particles", "",
	  "The same ceiling for snowflakes.", "", "rendering#weather" },
	{ "Weather/Ground Y", "",
	  "The height where drops and flakes die when nothing else stops them. In play "
	  "mode collisions override it where they can.",
	  "", "rendering#weather" },

	// ── UI ───────────────────────────────────────────────────────────────────
	{ "UI Canvas/Width", "", "The canvas's width in UI units.", "", "ui#designer" },
	{ "UI Canvas/Height", "", "The canvas's height in UI units.", "", "ui#designer" },
	{ "UI Element/Position", "",
	  "Where the element sits inside its canvas, measured from its anchor.",
	  "", "ui#elements" },
	{ "UI Element/Size", "", "The element's width and height.", "", "ui#elements" },
	{ "UI Element/Pivot", "",
	  "The point of the element its position and rotation refer to: 0.5, 0.5 is "
	  "its middle, 0, 0 its top-left corner.",
	  "", "ui#elements" },
	{ "UI Element/Rotation", "", "Rotation around the pivot, in degrees.",
	  "", "ui#elements" },
	{ "UI Element/Anchor", "",
	  "Which part of the canvas the element sticks to as the canvas resizes — a "
	  "corner, an edge, or a stretch across it.",
	  "", "ui#elements" },
	{ "UI Element/Layer", "",
	  "Draw order within the canvas: higher numbers are drawn on top.",
	  "", "ui#elements" },
	{ "UI Element/Active", "",
	  "Off hides the element and everything inside it, and it stops receiving "
	  "clicks.",
	  "", "ui#elements" },
	{ "UI Text/Text", "",
	  "What the label says. Widget logic can replace it at runtime, so this is the "
	  "text the designer sees rather than necessarily what the player will.",
	  "", "ui#elements" },
	{ "UI Text/Font Size", "",
	  "Text height in UI units — the canvas's own coordinates, so it scales with "
	  "the canvas rather than with the screen.",
	  "", "ui#elements" },
	{ "UI Text/Color", "",
	  "Text colour, alpha included. Fading a whole panel is usually better done on "
	  "the panel's opacity than on each label.",
	  "", "ui#elements" },
	{ "UI Image/Tint", "",
	  "Multiplied into the image. White leaves the texture alone; the alpha "
	  "channel fades it.",
	  "", "ui#elements" },
	{ "UI Button/Normal", "", "The button's colour at rest.", "", "ui#elements" },
	{ "UI Button/Hovered", "", "Its colour while the pointer is over it.",
	  "", "ui#elements" },
	{ "UI Button/Pressed", "", "Its colour while it is being held down.",
	  "", "ui#elements" },
	{ "UI Button/OnClick", "",
	  "What happens when the button is clicked — a graph event, or a script "
	  "function by name.",
	  "", "ui#graph" },

	// ── The rows the passes above left uncovered ─────────────────────────────
	// Found by walking the panels' labels against these keys rather than by
	// re-reading the table: a component whose rows carry an "##id" suffix looks
	// covered from here and is not, because the lookup tries the suffixed
	// spelling first. Every one of these is a row somebody can hover today.
	{ "Transform 2D/Position", "",
	  "Where the entity sits in the XY plane. Z is not a position here — draw "
	  "order is the UI layer or the sprite's own sorting.",
	  "", "scenes#components" },
	{ "Transform 2D/Rotation", "",
	  "One angle, in degrees, about the axis pointing at the viewer.",
	  "", "scenes#components" },
	{ "Transform 2D/Scale", "", "Size multiplier in X and Y.", "", "scenes#components" },
	{ "Skeletal Mesh/Visible", "",
	  "Off hides the character entirely — no draw call, no shadow. Its animation "
	  "keeps running.",
	  "", "systems#animation" },
	{ "Skeletal Mesh/Casts Shadow", "",
	  "Whether the animated mesh appears in the shadow maps. It is skinned again "
	  "for the shadow pass, so this one is not free.",
	  "", "rendering#shadows" },
	{ "Skeletal Mesh/Receives Shadow", "",
	  "Whether shadows from other objects darken this character.",
	  "", "rendering#shadows" },
	{ "Animator/Playing", "",
	  "Runs the clip. Off freezes the pose where it stands, which is what makes "
	  "the Time row worth dragging.",
	  "", "systems#animation" },
	{ "Animator/Looping", "",
	  "Start the clip over when it ends, instead of holding the last pose.",
	  "", "systems#animation" },
	{ "Animator Blend/Speed", "",
	  "Playback rate for both clips at once, so the blend stays in step.",
	  "", "systems#animation" },
	{ "Animator Blend/Time", "",
	  "Where both clips are, in seconds. Drag it to scrub the blended pose.",
	  "", "systems#animation" },
	{ "Animator Blend/Playing", "",
	  "Runs both clips. Off freezes the blended pose.", "", "systems#animation" },
	{ "Animator Blend/Looping", "",
	  "Start over at the end. Both clips loop together, on the longer one's clock.",
	  "", "systems#animation" },
	{ "Animator State Machine/Speed", "",
	  "Playback rate for whichever state is running.", "", "systems#animation" },
	{ "Property Animator/Speed", "",
	  "Playback rate of the property clip: 1 is as authored, negative runs it "
	  "backwards.",
	  "", "systems#animation" },
	{ "Property Animator/Time", "",
	  "Where the clip is, in seconds. Drag it to scrub the animated properties.",
	  "", "systems#animation" },
	{ "Property Animator/Playing", "",
	  "Runs the clip. Off leaves the properties wherever it last put them.",
	  "", "systems#animation" },
	{ "Property Animator/Looping", "",
	  "Start over at the end — what a moving platform or a pulsing light wants.",
	  "", "systems#animation" },
	{ "Particle System/Playing", "",
	  "Emits in the editor, so an effect can be judged without entering play "
	  "mode.",
	  "", "systems#particles" },
	{ "Particle System/Destroy When Finished", "",
	  "The entity goes away with the effect: once a one-shot emitter has made its "
	  "particles and the last one has died, it deletes itself. This is what makes "
	  "a hit effect spawnable — a graph creates one at the impact point and never "
	  "has to think about it again. Leave it off for anything that belongs to the "
	  "scene, like a torch, and note that a looping emitter never finishes and so "
	  "is never cleaned up unless something stops it.",
	  "", "systems#particles" },
	{ "Decal/Color", "",
	  "Tints the projected texture; the alpha channel fades the whole decal.",
	  "", "rendering#postfx" },
	{ "Script/Enabled", "",
	  "Off leaves the script attached but never calls it — neither onStart nor "
	  "onUpdate. The way to take one out of the picture without losing its "
	  "property values.",
	  "", "scripting#attach" },
	{ "Save State/Enabled", "",
	  "Include this entity in savegames. Off, it is whatever the scene file says "
	  "every time the game is loaded.",
	  "", "scenes#scene-files" },
	{ "Save State/Transform", "",
	  "Remember where this entity ended up. For anything the player moves or "
	  "that moves itself.",
	  "", "scenes#scene-files" },
	{ "Save State/Visibility", "",
	  "Remember whether it was visible — a door that was opened, a pickup that "
	  "was taken.",
	  "", "scenes#scene-files" },
	{ "UI Canvas/Active", "",
	  "Off hides the whole canvas and everything on it, and it stops receiving "
	  "clicks.",
	  "", "ui#entity-ui" },
	{ "Environment/Auto-Advance", "",
	  "Let the moon phase move with the days on its own.", "", "rendering#sky" },
	// ── Details: the actions inside a component ──────────────────────────────
	// Buttons rather than values, and the earlier pass counted only values —
	// which is why these were missing while every row above them was covered.
	{ "Component/Remove Component", "Remove Component",
	  "Takes this component off the entity. What it drove stops: a Mesh removed "
	  "leaves the entity in the scene with nothing to draw.",
	  "", "editor#details" },
	{ "Nav Mesh/Bake", "",
	  "Walks the scene's static geometry and builds the walkable surface from "
	  "it. Nothing can path until this has run, and it has to run again after "
	  "the level's shape changes.",
	  "", "systems#navigation" },
	{ "Nav Agent/Go", "",
	  "Sends the agent to the target position now, from the editor — the way to "
	  "check a nav mesh without entering play mode. It is a test button and "
	  "nothing more: the packaged game has no Details panel, so what sends an "
	  "agent off there is Auto Start above, or Move To from a script.",
	  "", "systems#navigation" },
	{ "Nav Agent/Stop", "",
	  "Drops the current path and leaves the agent where it is.",
	  "", "systems#navigation" },
	{ "Material/+ Texture Slot", "",
	  "Adds a texture slot to this entity's material override. The material's "
	  "graph decides what a slot is used for; this only fills one in per entity.",
	  "", "materials#parameters" },
	{ "Material/Reset to material default", "",
	  "Drops this entity's override of the parameter and goes back to what the "
	  "material asset itself says.",
	  "", "materials#parameters" },
	{ "Terrain/Set for 4 m tiles", "",
	  "Sets the texture tiling so one repeat covers four metres of ground, "
	  "computed from this terrain's size — the usual starting point, instead of "
	  "one texture stretched over the whole landscape.",
	  "", "scenes#terrain" },
	{ "Foliage/Regenerate", "",
	  "Scatters the instances again from the current seed and density. Needed "
	  "after the terrain under them was sculpted.",
	  "", "scenes#foliage" },
	{ "LOD/+ Level", "",
	  "Adds a detail level: a cheaper mesh and the distance from which it takes "
	  "over.",
	  "", "rendering#performance" },
	{ "UI Canvas/Screen Space", "",
	  "The canvas is drawn flat on the screen, at the same size wherever the "
	  "camera is — a HUD, a menu.",
	  "", "ui#runtime" },
	{ "UI Canvas/World Space", "",
	  "The canvas stands in the world as a surface you can walk up to and view "
	  "from an angle — a terminal, a sign.",
	  "", "ui#entity-ui" },

	// ── The menu bar ─────────────────────────────────────────────────────────
	// Each menu pushes its own name as the scope, so these are keyed by the
	// label the row carries. A menu is the densest place in the editor where a
	// verb does more than the word says — "Save All" is the example everyone
	// meets first.
	{ "File/New Project", "",
	  "Creates a project folder with its content tree, settings and a first "
	  "scene, from a template. The editor then opens it — the current project is "
	  "closed, after asking about anything unsaved.",
	  "Ctrl+N", "editor#project-hub" },
	{ "File/Open Project", "",
	  "Opens a .heproj from disk. Everything in the editor belongs to a project: "
	  "the content tree, the settings, the layout.",
	  "Ctrl+O", "editor#project-hub" },
	{ "File/Close Project", "",
	  "Returns to the Project Hub. Tabs, scene and undo history end with the "
	  "project — the next one starts clean.",
	  "Ctrl+W", "editor#project-hub" },
	{ "File/New Scene", "",
	  "Replaces what is open with an empty scene. The old one is not deleted, "
	  "only closed, and you are asked about unsaved changes first.",
	  "", "scenes#scene-files" },
	{ "File/Open Scene...", "",
	  "Loads a scene from the project, replacing the open one.",
	  "", "scenes#scene-files" },
	{ "File/Add Scene Additive...", "",
	  "Loads a second scene ALONGSIDE the open one, at an offset — the editor "
	  "side of zone streaming. Both are live at once; the additive one keeps its "
	  "own file.",
	  "", "scenes#streaming" },
	{ "File/Save All", "",
	  "Writes every unsaved asset — open tabs and closed ones that still hold "
	  "edits — and then the scene. Saving the scene alone leaves a changed "
	  "material or graph on disk as it was.",
	  "Ctrl+Shift+S", "editor#menus" },
	{ "File/Save Scene As...", "",
	  "Writes the scene to a new file and continues in that one. The original "
	  "stays on disk as it was.",
	  "Ctrl+Alt+S", "scenes#scene-files" },
	{ "File/Exit", "",
	  "Closes the editor, asking about anything unsaved first — including asset "
	  "tabs, which the scene's own saved state says nothing about.",
	  "", "editor#menus" },
	{ "Edit/Duplicate", "",
	  "Copies the selected entity with all its components and children, next to "
	  "the original.",
	  "Ctrl+D", "editor#outliner" },
	{ "Edit/Preferences", "",
	  "Opens the settings as an editor tab: renderer, viewport, collaboration, "
	  "tools. They belong to the editor, not to the project.",
	  "Ctrl+,", "editor#preferences" },
	{ "View/Toggle Fullscreen", "",
	  "Fills the screen with the editor window. On a Mac the View menu's own "
	  "system entry is the reliable one — the key is usually claimed before the "
	  "editor sees it.",
	  "F11", "editor#layout" },
	{ "View/Reset Layout", "",
	  "Puts every panel back where it started. The escape hatch for a layout "
	  "that ended up with a panel dragged somewhere it cannot be reached.",
	  "", "editor#layout" },
	{ "View/Performance Profiler", "",
	  "Where the frame time goes: a live CPU and GPU readout, and captures that "
	  "break one frame down pass by pass.",
	  "", "editor#profiler" },
	{ "View/Collaboration", "",
	  "Host or join a live editing session — several people in one scene, with "
	  "locks so two of you cannot edit the same thing.",
	  "", "collaboration#overview" },
	{ "View/Source Control", "",
	  "The repository: what changed, what to commit, what the others have "
	  "pushed. Large assets included.",
	  "", "editor#layout" },
	{ "View/Console", "",
	  "Everything the engine logged this session. The first place to look when "
	  "something did not happen.",
	  "Ctrl+`", "advanced#diagnostics" },
	{ "View/Ground Grid", "",
	  "The reference grid on the ground plane. Hidden while the scene plays "
	  "either way.",
	  "", "editor#viewport" },
	{ "View/Level Script", "",
	  "The HorizonCode graph belonging to THIS scene — where its own events and "
	  "logic live. Opens as a tab.",
	  "", "horizoncode#hosts" },
	{ "View/Game Instance", "",
	  "The graph that outlives every scene: the app-wide state a level change "
	  "must not reset.",
	  "", "horizoncode#hosts" },
	{ "Assets/Import Asset...", "",
	  "Brings a file from outside into the project — meshes, textures, audio, "
	  "fonts — converting it to the engine's own format on the way in.",
	  "", "editor#content-browser" },
	{ "Assets/Refresh Assets", "",
	  "Re-walks the content tree. What makes a file dropped in from the Finder "
	  "appear without restarting the editor.",
	  "", "editor#content-browser" },
	{ "Assets/Publish Engine Content to Server...", "",
	  "Uploads the engine-wide content library to the shared server. Only for "
	  "maintainers of that library — a project's own assets are not touched.",
	  "", "editor#engine-content" },
	{ "Assets/Rebuild Manifest from Server...", "",
	  "Rebuilds the engine library's index from what is actually on the server. "
	  "The repair path for a manifest that no longer matches.",
	  "", "editor#engine-content" },
	{ "Build/Export Project...", "",
	  "Packages the project as a standalone game: cooked assets, the runtime, "
	  "and a config beside it. The export profile decides platform and packing.",
	  "", "export#overview" },
	{ "Help/Documentation", "",
	  "This manual, inside the editor. The reference half is generated from the "
	  "editor itself, so it describes the build you are running.",
	  "F1", "editor#menus" },
	{ "Help/Search the Documentation...", "",
	  "Opens the manual with the search box focused — the fastest route when you "
	  "know a word but not a page.",
	  "Ctrl+F1", "editor#menus" },
	{ "Help/Documentation (Website)", "",
	  "The same manual in a browser: shareable, always current, and the only "
	  "version with the pictures at full size.",
	  "", "editor#menus" },
	{ "Help/Interactive Tutorial", "",
	  "A guided tour through the editor in a sandbox project it creates for you. "
	  "It watches what you do rather than telling you to press Next.",
	  "", "getting-started#first-project" },
	{ "Help/Report Issue...", "",
	  "Opens a pre-filled issue on the project's tracker, with the version, the "
	  "platform and the recent log already in it.",
	  "", "editor#notifications" },
	{ "Help/About", "",
	  "Which version this is — the number to quote in a bug report.",
	  "", "editor#menus" },
	{ "Help/Website", "",
	  "horizoncreations.dev: releases, the roadmap and the devlog.",
	  "", "editor#menus" },

	// ── The viewport's options popup ─────────────────────────────────────────
	{ "Viewport Options/Snap to grid", "",
	  "Constrain dragging to fixed steps, so pieces line up exactly instead of "
	  "nearly. The three steps below are separate because a metre, a degree and "
	  "a factor are not the same number.",
	  "", "editor#viewport" },
	{ "Viewport Options/Move (m)", "",
	  "How far one snapped step moves, in metres.", "", "editor#viewport" },
	{ "Viewport Options/Rotate (\xc2\xb0)", "",
	  "How far one snapped step turns, in degrees. 15 gives you the eight "
	  "compass directions.",
	  "", "editor#viewport" },
	{ "Viewport Options/Scale (\xc3\x97)", "",
	  "How much one snapped step scales by.", "", "editor#viewport" },
	{ "Viewport Options/Screen-space rotation ring", "",
	  "The rotate gizmo's outer ring, which turns the object about the axis you "
	  "are looking along. Off by default: it behaves relative to the camera, "
	  "which surprises people mid-drag.",
	  "", "editor#viewport" },
	{ "Viewport Options/Speed", "",
	  "The editor fly camera's speed in units per second. Hold Shift while "
	  "flying for three times this.",
	  "", "editor#viewport" },
	{ "Viewport Options/Ground grid", "",
	  "The reference grid under the scene. Off while playing either way.",
	  "", "editor#viewport" },

	// ── World Outliner ───────────────────────────────────────────────────────
	{ "World Outliner/Create Child", "",
	  "Creates an entity parented to this one. A child follows its parent's "
	  "transform, which is how a hierarchy is built.",
	  "", "editor#outliner" },
	{ "World Outliner/Duplicate", "",
	  "Copies this entity, its components and its children, beside the original.",
	  "Ctrl+D", "editor#outliner" },
	{ "World Outliner/Save as Prefab", "",
	  "Saves this entity and everything under it as a reusable asset, so the "
	  "same thing can be dropped into any scene.",
	  "", "scenes#prefabs" },

	// ── Content Browser ──────────────────────────────────────────────────────
	{ "Content Browser/Create Asset", "",
	  "Makes a new asset in this folder. What kinds there are is the list below "
	  "it — everything the editor can author itself, as opposed to import.",
	  "", "editor#content-browser" },
	{ "Content Browser/Import", "",
	  "Brings this file into the project as an engine asset. The original is not "
	  "moved; a .hasset beside it records where it came from.",
	  "", "editor#content-browser" },
	{ "Content Browser/Reimport", "",
	  "Reads the source file again and rebuilds the asset from it — after the "
	  "model was changed in the program it came from.",
	  "", "editor#content-browser" },
	{ "Content Browser/Create Material Instance", "",
	  "A new material that inherits this one and overrides only what you change. "
	  "The way to get twenty variants without twenty graphs.",
	  "", "materials#parameters" },
	{ "Content Browser/Add to Scene", "",
	  "Puts this asset into the open scene as an entity, at the origin, already "
	  "carrying the components it needs.",
	  "", "editor#content-browser" },
	{ "Content Browser/Find References", "",
	  "Everything in the project that points at this asset. What to check before "
	  "deleting or renaming one.",
	  "", "editor#content-browser" },
	{ "Content Browser/Revert to Default", "",
	  "Throws away this project's copy of an engine asset and goes back to the "
	  "one the editor ships.",
	  "", "editor#engine-content" },
	{ "Content Browser/Remove Local Copy", "",
	  "Deletes the downloaded copy of an engine-library asset. It stays "
	  "available and is fetched again when something needs it.",
	  "", "editor#engine-content" },
	{ "Content Browser/Download", "",
	  "Fetches this engine-library asset from the server so it is available "
	  "offline and can be opened.",
	  "", "editor#engine-content" },
	{ "Content Browser/Ask to Edit", "",
	  "Asks the session host to hand you the lock on this asset. In a "
	  "collaboration session only one person may edit a thing at a time.",
	  "", "collaboration#locks" },
	{ "Content Browser/Number them", "",
	  "Renaming several files at once: appends a running number instead of "
	  "giving them all the same name.",
	  "", "editor#content-browser" },
	{ "Content Browser/starting at", "",
	  "The first number of that run.", "", "editor#content-browser" },

	// ── What the Create Asset menu offers ────────────────────────────────────
	{ "New Asset/Scene", "",
	  "A level: entities, their components, and the sky and weather that belong "
	  "to it.",
	  "", "scenes#scene-files" },
	{ "New Asset/UI Widget", "",
	  "A screen — HUD, menu, dialog — laid out in the widget designer, with its "
	  "own graph for what its buttons do.",
	  "", "ui#designer" },
	{ "New Asset/Theme", "",
	  "What the whole application looks like: nine colour ROLES — background, "
	  "surface, border, text, muted text, accent, warning, error, success — each "
	  "in a light and a dark value, plus the size steps and the shadows. Elements "
	  "point at a role instead of carrying a colour, so one edit here changes "
	  "every one of them, and light/dark becomes a switch instead of a second set "
	  "of widgets. A new theme starts as a copy of the shipped default.",
	  "", "ui#elements" },
	{ "New Asset/HorizonCode Class", "",
	  "A visual-scripting class: variables, functions and events, instantiated "
	  "at run time or put on an entity.",
	  "", "horizoncode#hosts" },
	{ "New Asset/Entity", "",
	  "A HorizonCode class that lives on an entity in the world — the usual "
	  "shape for a pickup, a door, an enemy.",
	  "", "horizoncode#hosts" },
	{ "New Asset/Player Controller", "",
	  "The class that receives the player's input and decides what to possess. "
	  "It outlives the character it drives.",
	  "", "horizoncode#players" },
	{ "New Asset/Player Character", "",
	  "The class for the thing the player moves: a character controller, a "
	  "camera rig and the logic between them.",
	  "", "horizoncode#players" },
	{ "New Asset/C++ Class", "",
	  "A native gameplay class, scaffolded into the project's Source folder and "
	  "compiled into the game module.",
	  "", "scripting#cpp" },
	{ "New Asset/Input Action", "",
	  "One named thing the player can do — Jump, Fire, Move. Bound to keys, "
	  "buttons and axes by a mapping context, never to a device directly.",
	  "", "systems#input" },
	{ "New Asset/Input Mapping Context", "",
	  "A set of bindings that can be switched as a whole: on foot, in a vehicle, "
	  "in a menu.",
	  "", "systems#input" },
	{ "New Asset/Material Function", "",
	  "A piece of material graph saved for reuse, with its own inputs and "
	  "outputs — a function, in a graph made of nodes.",
	  "", "materials#functions" },
	{ "New Asset/Struct", "",
	  "A named group of fields you can pass around as one value in graphs, "
	  "scripts and savegames.",
	  "", "horizoncode#functions" },
	{ "New Asset/Enum", "",
	  "A fixed list of named states — Idle, Walking, Dead — instead of numbers "
	  "nobody can read at the call site.",
	  "", "horizoncode#functions" },
	{ "New Asset/SaveGame Template", "",
	  "Declares what a savegame holds: the fields, their types and their "
	  "defaults. Every save is validated against it.",
	  "", "scenes#scene-files" },
	{ "New Asset/Folder", "",
	  "A folder in the content tree. Only that — the engine finds assets by "
	  "their id, so moving one breaks nothing.",
	  "", "editor#content-browser" },

	{ "Collaboration/Ask to edit", "",
	  "Asks the session host for the lock on this asset. Until it is granted the "
	  "tab is read-only: in a session exactly one person may edit a thing, and "
	  "this is how it changes hands.",
	  "", "collaboration#locks" },
	{ "Source Root/C++ Class", "",
	  "Scaffolds a native class into this project's Source folder — header, "
	  "source and its registration — and it is compiled into the game module on "
	  "the next build.",
	  "", "scripting#cpp" },
	{ "New Asset/Gameplay", "",
	  "The classes that run logic: HorizonCode classes, the player controller "
	  "and character, and native C++ classes.",
	  "", "horizoncode#hosts" },
	{ "New Asset/Input", "",
	  "Actions and the contexts that bind them — what the player can do, and "
	  "which keys mean it right now.",
	  "", "systems#input" },
	{ "New Asset/Rendering", "",
	  "What things look like: materials and the graph pieces they are built "
	  "from.",
	  "", "materials#concept" },
	{ "New Asset/Data", "",
	  "Types you define yourself: structs, enums, and the template a savegame is "
	  "validated against.",
	  "", "horizoncode#functions" },

	// ── Console and notifications ────────────────────────────────────────────
	{ "Console/Clear", "",
	  "Empties the view. The log file next to the executable keeps everything.",
	  "", "advanced#diagnostics" },
	{ "Console/Auto-scroll", "",
	  "Follow the newest line. Switch it off to read something while the log "
	  "keeps growing.",
	  "", "advanced#diagnostics" },
	{ "Console/Copy Line", "",
	  "Copies the selected line — the one to paste into a bug report.",
	  "", "advanced#diagnostics" },
	{ "Console/Copy All Shown", "",
	  "Copies everything the current filter leaves visible, not the whole log.",
	  "", "advanced#diagnostics" },
	{ "Notifications/Mark all as seen", "",
	  "Clears the bell without discarding the entries — they stay readable in "
	  "the list.",
	  "", "editor#notifications" },
	{ "Notifications/Clear", "",
	  "Discards every notification. Anything that still needs doing is not "
	  "undone by this — only the reminder is gone.",
	  "", "editor#notifications" },
	{ "Play Report/Show warnings", "",
	  "Include the warnings the session logged, not just the errors. Off keeps "
	  "the report to what actually went wrong.",
	  "", "editor#play-mode" },
	{ "Play Report/Copy All", "",
	  "Copies the whole report as text — what to paste into a bug report after a "
	  "session went wrong.",
	  "", "editor#play-mode" },

	// ── Project Hub and the reader itself ────────────────────────────────────
	{ "Project Hub/Advanced Shader Effects", "",
	  "Whether this application may author MATERIALS — shader graphs of its own. "
	  "Off, the editor offers none and the packaged build ships the small renderer: "
	  "widgets are still styled with rounded corners, borders, gradients and "
	  "shadows, which is what an interface actually uses. On, everything a game "
	  "has is available and the build carries a GPU renderer. Offered for "
	  "applications only, and it cannot be changed afterwards.",
	  "", "editor#project-hub" },
	{ "Project Hub/Remove from list", "",
	  "Takes the project off this list. The project itself is untouched on disk "
	  "— this is the list of what you have opened, not of what exists.",
	  "", "editor#project-hub" },
	{ "Project Hub/Browse .heproj...", "",
	  "Opens a project the list does not know yet. It is added to the list "
	  "afterwards.",
	  "", "editor#project-hub" },
	{ "Project Hub/Start the tutorial", "",
	  "Creates a sandbox project and starts the guided tour in it. Nothing you "
	  "build there is lost — it is an ordinary project.",
	  "", "getting-started#first-project" },
	{ "Project Hub/Not now", "",
	  "Puts the offer away. It comes back through Help ▸ Interactive Tutorial "
	  "whenever you want it.",
	  "", "getting-started#first-project" },
	{ "Documentation/Start", "",
	  "Back to the first page of the manual.", "", "editor#menus" },
	{ "Documentation/Online", "",
	  "Opens the page you are reading on the website — the version to send "
	  "somebody a link to.",
	  "", "editor#menus" },
	{ "Documentation/Open the manual online", "",
	  "The offline copy could not be read — this opens the same manual on the "
	  "website instead. The copy ships beside the editor as Docs/he-docs.json, "
	  "so a missing one usually means an incomplete install.",
	  "", "editor#menus" },
	{ "Documentation/Show me", "",
	  "Opens the panel this entry is about and outlines it, so \"where is that\" "
	  "is answered by pointing rather than by describing.",
	  "", "editor#layout" },
	{ "Tutorial/Start over", "",
	  "Begins the guided tour again from its first step. What you built while "
	  "following it stays where it is.",
	  "", "getting-started#first-project" },

	// ── Creating an entity ───────────────────────────────────────────────────
	// The Outliner's create menu offers recipes, not components — which is the
	// knowledge the menu used to hide rather than teach.
	{ "New Entity/Empty", "",
	  "An entity with a name and a transform and nothing else. Everything it "
	  "does comes from components you add in the Details panel.",
	  "", "editor#outliner" },
	{ "New Entity/Cube", "",
	  "A visible box: the engine's default cube mesh on a fresh entity. What to "
	  "reach for when you need something to stand on or aim at.",
	  "", "scenes#components" },
	{ "New Entity/Third Person", "",
	  "A camera on a boom behind the player, already following whoever the "
	  "player possesses — the whole setup, not just a camera.",
	  "", "rendering#cameras" },
	{ "New Entity/First Person", "",
	  "A camera in the player's head, with the character turning to face where "
	  "it looks. Same rig as third person with the boom at zero.",
	  "", "rendering#cameras" },
	{ "New Entity/Plain (no rig)", "",
	  "A camera that stays exactly where you put it — for a fixed shot, a "
	  "security monitor, a cutscene.",
	  "", "rendering#cameras" },
	{ "New Entity/Directional", "",
	  "A light with a direction and no position, like the sun: it lights the "
	  "whole scene from one angle.",
	  "", "rendering#lighting" },
	{ "New Entity/Point", "",
	  "A light shining in every direction from where it stands, out to its "
	  "range — a lamp, a fire, a torch.",
	  "", "rendering#lighting" },
	{ "New Entity/Spot", "",
	  "A light shining in a cone — a torch, a headlight, a stage light.",
	  "", "rendering#lighting" },

	// ── The viewport and its toolbar ─────────────────────────────────────────
	{ "viewport.play", "Play",
	  "Runs the scene in the viewport: scripts start, physics ticks and the game's "
	  "own camera takes over. Stopping restores the scene exactly as it was — "
	  "changes made while playing are not kept.",
	  "", "editor#play-mode" },
	{ "viewport.restartPreview", "Restart Live Preview",
	  "An application has no play mode: its interface is already running in the "
	  "panel. This builds it again from the saved assets — the same thing a save "
	  "does automatically, for when the preview has been driven into a state you "
	  "want out of. Whatever it was holding (typed text, scroll positions) is gone.",
	  "", "editor#play-mode" },
	{ "viewport.pause", "Pause",
	  "Freezes a running session without ending it. Everything stays where it is, "
	  "so the frame can be inspected in the Details panel.",
	  "", "editor#play-mode" },
	{ "viewport.step", "Step",
	  "Advances a paused session by exactly one frame — the way to watch a bug "
	  "happen instead of catching it afterwards.",
	  "", "editor#play-mode" },
	{ "viewport.translate", "Move",
	  "Drag the arrows to move the selection along an axis, or a square to slide "
	  "it in a plane.",
	  "W", "editor#viewport" },
	{ "viewport.rotate", "Rotate",
	  "Drag a ring to turn the selection around that axis.",
	  "E", "editor#viewport" },
	{ "viewport.scale", "Scale",
	  "Drag a handle to resize the selection along an axis, or the middle to "
	  "resize it evenly.",
	  "R", "editor#viewport" },
	{ "viewport.space", "Local / World",
	  "Whether the gizmo's axes follow the object's own orientation or the world's. "
	  "World is what you want to line things up with the ground.",
	  "", "editor#viewport" },
	{ "viewport.snap", "Snap",
	  "Constrain dragging to fixed increments — a metre, fifteen degrees — so "
	  "pieces line up exactly instead of nearly.",
	  "", "editor#viewport" },
	{ "viewport.camera-speed", "Camera Speed",
	  "How fast the editor's fly camera moves, in metres per second. Hold Shift "
	  "while flying for three times this.",
	  "", "editor#viewport" },
	{ "viewport.mode", "Viewport Mode",
	  "Scene is normal editing. Landscape turns the viewport into the terrain "
	  "sculpting and painting tool, with its brushes in Quick Settings.",
	  "", "editor#landscape-mode" },
	{ "viewport.grid", "Ground Grid",
	  "The reference grid on the ground plane. It is hidden while the scene plays "
	  "either way.",
	  "", "editor#viewport" },
	{ "viewport.frame", "Frame Selected",
	  "Moves the editor camera so the selected entity fills the view — the fastest "
	  "way back to something you have lost.",
	  "F", "editor#viewport" },

	// ── World Outliner ───────────────────────────────────────────────────────
	{ "outliner.duplicate", "Duplicate",
	  "Copies the selected entity with all its components, alongside the original.",
	  "Ctrl+D", "editor#outliner" },
	{ "outliner.delete", "Delete",
	  "Removes the selected entity and everything parented under it.",
	  "Delete", "editor#outliner" },
	{ "outliner.prefab", "Save as Prefab",
	  "Saves this entity and its children as a reusable asset, so the same thing "
	  "can be dropped into any scene.",
	  "", "scenes#prefabs" },

	// ── Details panel ────────────────────────────────────────────────────────
	{ "details.add-component", "Add Component",
	  "Gives the entity a new capability — a mesh to draw, a body for physics, a "
	  "script to run. An entity is only ever the sum of its components.",
	  "", "editor#details" },
	{ "details.name", "Name",
	  "What this entity is called in the Outliner and to scripts that look it up "
	  "by name.",
	  "", "editor#details" },

	// ── Content Browser ──────────────────────────────────────────────────────
	{ "content.import", "Import Asset",
	  "Brings a file from outside into the project — meshes, textures, audio, "
	  "fonts. It is converted to the engine's own format on the way in.",
	  "", "editor#content-browser" },
	{ "content.create", "Create Asset",
	  "Makes a new asset in this folder: a material, a particle system, a widget, "
	  "a HorizonCode class.",
	  "", "editor#content-browser" },
	{ "content.roots", "Content · Engine · Source",
	  "Content is your project's assets. Engine is the library that ships with the "
	  "editor, shared by every project and read-only. Source is your C++ code.",
	  "", "editor#engine-content" },
	{ "content.rename", "Rename",
	  "Renames the asset and repoints everything that referenced it.",
	  "F2", "editor#content-browser" },
	{ "content.delete", "Delete",
	  "Deletes the asset after showing what still references it — so nothing "
	  "vanishes from under a scene by accident.",
	  "Delete", "editor#content-browser" },

	// ── Preferences ──────────────────────────────────────────────────────────
	// Scoped "Preferences" and keyed by the label, like the Details rows — the
	// settings catalog draws through the same Row helpers, so these need no call
	// site either. The labels are the panel's, exactly.
	{ "Preferences/Display/Backend", "Backend",
	  "Which graphics API the editor and the game render through. Changing it "
	  "takes effect on the next start, and the list holds only what this build "
	  "and this machine actually support — some features (deferred shading, "
	  "ray-traced GI) exist on some backends and not on others.",
	  "", "rendering#backends" },
	{ "Preferences/Display/Render Path", "",
	  "Forward shades each object as it is drawn. Deferred shades the whole screen "
	  "at once, which is what lifts the light count and turns on SSAO and SSR.",
	  "", "rendering#pipeline" },
	{ "Preferences/Display/VSync", "",
	  "Waits for the display before showing a frame: no tearing, and the frame "
	  "rate is capped to the monitor's. Off is for measuring performance.",
	  "", "rendering#performance" },
	{ "Preferences/Display/Max FPS (VSync off)", "",
	  "Upper limit on frames per second once VSync is off. A cap keeps a laptop "
	  "quiet without giving up responsiveness. 0 is unlimited.",
	  "", "rendering#performance" },
	{ "Preferences/Post-Processing/Anti-Aliasing", "",
	  "How jagged edges are smoothed. SMAA is one cheap pass; TAA is steadier in "
	  "motion but needs the deferred path.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/AA Sharpness", "",
	  "How much detail is pulled back after the anti-aliasing pass softened it. "
	  "Too much re-introduces the edges it just removed.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Render Scale", "",
	  "Renders the 3D view at a fraction of the window size and upscales it. The "
	  "single most effective performance dial there is; 0.75 is often invisible.",
	  "", "rendering#performance" },
	{ "Preferences/Post-Processing/Specular AA", "",
	  "Tames the sparkle that fine, shiny detail produces in motion, by widening "
	  "the highlight where the surface curves too fast for the pixel.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Specular AA Strength", "",
	  "How aggressively that is done. Too high and polished surfaces go dull.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Bloom", "",
	  "Lets bright areas bleed light into what surrounds them, the way a camera "
	  "does.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Bloom Threshold", "",
	  "How bright a pixel has to be before it blooms. Low values make the whole "
	  "image glow, which is rarely what is wanted.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Bloom Intensity", "",
	  "How much of the bloom is added back on top of the image.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/AO", "",
	  "Ambient occlusion: darkens the creases and contact points that ambient "
	  "light cannot reach. It is what stops objects looking as if they float.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/AO Method", "",
	  "SSAO is the cheapest. HBAO follows the surface horizon and is more "
	  "accurate; GTAO is the most accurate and the most expensive.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/AO Radius", "",
	  "How far, in world units, a surface looks for neighbours that might be "
	  "shadowing it. Large radii darken whole surfaces rather than their creases.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/AO Intensity", "",
	  "How dark the occlusion gets. Past 1 it stops reading as shadow and starts "
	  "reading as dirt.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/Screen-Space Reflections", "",
	  "Reflects what is already on screen in wet and polished surfaces. What is "
	  "off screen cannot be reflected — that is the method's limit, not a bug.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/SSR Intensity", "",
	  "How strongly the screen-space reflection is blended in.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/SSR Max Roughness", "",
	  "The roughest surface still worth reflecting into. Rough surfaces scatter "
	  "so much that the reflection costs more than it shows.",
	  "", "rendering#postfx" },
	{ "Preferences/Post-Processing/SSR Quality", "",
	  "How many steps each reflection ray takes. More steps reach further before "
	  "the reflection gives up and fades.",
	  "", "rendering#performance" },
	{ "Preferences/Global Illumination/Global Illumination (ray-traced, Metal)", "",
	  "Ray-traced bounce light: colour carried from lit surfaces into the shadows "
	  "around them. Needs a ray-tracing capable GPU.",
	  "", "rendering#lighting" },
	{ "Preferences/Global Illumination/GI Indirect Intensity", "",
	  "How much of that bounced light is applied. Above 1 rooms glow from their "
	  "own walls.",
	  "", "rendering#lighting" },
	{ "Preferences/Global Illumination/GI Light Radius (deg)", "",
	  "How large the light source is treated as being, in degrees. Wider means "
	  "softer, more diffuse indirect shadows.",
	  "", "rendering#lighting" },
	{ "Preferences/Global Illumination/GI Reflections (ray-traced)", "",
	  "Traced reflections instead of screen-space ones: they can show what is "
	  "behind the camera, at the cost of tracing the scene.",
	  "", "rendering#postfx" },
	{ "Preferences/Global Illumination/GI Refl Intensity", "",
	  "How strongly the traced reflection is blended in.", "", "rendering#postfx" },
	{ "Preferences/Global Illumination/GI Refl Max Roughness", "",
	  "The roughest surface still traced. Above it the cheaper approximation "
	  "takes over.",
	  "", "rendering#postfx" },
	{ "Preferences/Global Illumination/GI Refl Quality", "",
	  "Rays per pixel for the traced reflections — the dial between noise and "
	  "frame time.",
	  "", "rendering#performance" },
	{ "Preferences/Global Illumination/GI Refl Bounces (Metal)", "",
	  "How many times a reflection ray may bounce on. Two is enough for a mirror "
	  "facing a mirror to look right.",
	  "", "rendering#performance" },
	{ "Preferences/Global Illumination/GI Refl Blur", "",
	  "Smooths the traced reflections before they are composited, which is what "
	  "hides the noise a low ray count leaves.",
	  "", "rendering#postfx" },
	{ "Preferences/Effects/GPU Weather Particles", "",
	  "Simulate rain and snow on the GPU. Far more drops for the same frame time; "
	  "it needs a backend that can do it, and falls back quietly where it cannot.",
	  "", "rendering#weather" },
	{ "Preferences/Collaboration/Find Sessions on the Local Network", "",
	  "Announce and discover collaboration sessions on this network, so joining "
	  "needs no address. Off, a session is still reachable by its id and code.",
	  "", "collaboration#discovery" },
	{ "Preferences/Collaboration/Sync Large Assets (Meshes, Textures, Audio)", "",
	  "Send the big files to peers as well as the small ones. Off, everybody needs "
	  "their own copy of the meshes and textures — but joining is instant.",
	  "", "collaboration#bigassets" },
	{ "Preferences/Collaboration/Largest Asset to Transfer (MB)", "",
	  "The ceiling on a single transferred file. Anything above it is skipped and "
	  "reported rather than holding up the session.",
	  "", "collaboration#bigassets" },
	{ "Preferences/Viewport/Camera Speed", "",
	  "How fast the editor's fly camera moves, in units per second. The viewport "
	  "toolbar's speed field is the same value.",
	  "", "editor#viewport" },
	{ "Preferences/Viewport/Pointer Device", "",
	  "Whether the preview panes expect a mouse or a trackpad. On a trackpad a "
	  "two-finger swipe steers and zoom moves behind the modifier key.",
	  "", "editor#preferences" },
	{ "Preferences/Input/Stick Deadzone", "",
	  "How far a gamepad stick must be pushed before it counts as moved. It is "
	  "what stops a worn stick from drifting the camera on its own.",
	  "", "systems#input" },
	{ "Preferences/Input/Trigger Deadzone", "",
	  "The same for the analogue triggers.", "", "systems#input" },
	{ "Preferences/Appearance/UI Font Scale", "",
	  "Scales the editor's entire interface. For a high-resolution display, or "
	  "simply for reading comfort.",
	  "", "editor#preferences" },
	{ "Preferences/Content Browser/Keep CPU Asset Cache", "",
	  "Keeps mesh and texture data in main memory after it has been uploaded to "
	  "the GPU. It costs RAM and saves a re-read.",
	  "", "advanced#assets" },
	{ "Preferences/Content Browser/Refresh Interval (s)", "",
	  "How often the Content Browser re-checks the project folder for files "
	  "changed outside the editor.",
	  "", "editor#content-browser" },
	{ "Graph Appearance/Detailed", "",
	  "How a variable is drawn in a HorizonCode graph's list: name and type on "
	  "two lines, with the type written out and coloured. The default.",
	  "", "horizoncode#graphs" },
	{ "Graph Appearance/Compact", "",
	  "Name and type on one line instead. Half the height per variable, so a long "
	  "list stays readable without scrolling.",
	  "", "horizoncode#graphs" },
	{ "Preferences/Restore Defaults", "Restore Defaults",
	  "Puts the settings in the category you are looking at back the way they "
	  "shipped. Only this category, and only the ones the engine owns — your "
	  "pinned settings and the project itself are untouched.",
	  "", "editor#preferences" },

	// ── Preferences » Editor » Source Control ────────────────────────────────
	// The one-time setup page: is git usable on this machine, and is this
	// project in a repository. Its own scope rather than "Preferences" because
	// its labels are about a repository, not about the editor — and because the
	// three entries this page used to have were keyed "Preferences/…" and never
	// resolved: no scope is open on that path at all.
	{ "Source Control/Recheck##git", "Recheck",
	  "Looks for git and git-lfs again. Press this after installing one of them, "
	  "rather than restarting the editor.",
	  "", "editor#preferences" },
	{ "Source Control/Save Identity", "Save Identity",
	  "Writes your name and email into git's global configuration. git records "
	  "who made each change and refuses to commit without them, so this is asked "
	  "once and then holds for every project on this machine.",
	  "", "editor#preferences" },
	{ "Source Control/Initialize Git repository", "Initialize Git repository",
	  "Turns the project folder into a git repository. It also writes a "
	  ".gitignore, so the engine's own output stays out of it, and a "
	  ".gitattributes, so meshes, textures and audio go through Git LFS instead "
	  "of being stored whole in every commit.",
	  "", "editor#preferences" },
	{ "Source Control/Name##gh", "Name",
	  "What the repository will be called on GitHub. It defaults to the project "
	  "folder's name, which is nearly always the answer.",
	  "", "editor#preferences" },
	{ "Source Control/Private", "",
	  "Create the new repository as private. It can be made public later on the "
	  "hosting side; the other direction is the awkward one.",
	  "", "editor#preferences" },
	{ "Source Control/Create & push", "Create & push",
	  "Creates the repository on GitHub with the token above, points this project "
	  "at it and pushes what is committed. The token is handed to git's "
	  "credential helper and wiped from the field, never written to a project "
	  "file.",
	  "", "editor#preferences" },
	{ "Source Control/Set##remote", "Set",
	  "Points the project at a repository that already exists — GitHub, GitLab, "
	  "Azure DevOps, anything git can push to. Use this instead of Create & push "
	  "when somebody else made the repository.",
	  "", "editor#preferences" },
	{ "Source Control/Save token", "Save token",
	  "Stores an access token for pushing and pulling. It goes straight into the "
	  "system keychain through git's credential helper; the engine keeps no copy "
	  "and no project or engine file ever contains it.",
	  "", "editor#preferences" },
	{ "Source Control/Push automatically after each commit", "",
	  "Send every commit to the remote as it is made. Convenient alone, and a "
	  "way to publish half-finished work when several people share the branch.",
	  "", "editor#preferences" },
	{ "Source Control/Check the remote for new commits periodically", "",
	  "Poll for what the others have pushed, so the footer's status is about the "
	  "repository rather than about the last time you looked.",
	  "", "editor#preferences" },

	// ── Preferences » Project ────────────────────────────────────────────────
	// The pages on this tab that edit the PROJECT. Everything else here follows
	// the editor from project to project; these travel with the project and into
	// the application it exports.
	{ "Permissions/Files outside the project", "Files outside the project",
	  "Off, a script reads and writes only inside the project's Saved folder — an "
	  "absolute path is simply refused. On, it may name any path on the machine.\n\n"
	  "This is about what a SCRIPT may name on its own, never about what a PERSON "
	  "may choose: a file somebody picks in a file dialog is allowed either way, "
	  "because choosing it IS the permission. Most applications never need this "
	  "switch — they need the dialog.",
	  "", "editor#preferences" },
	{ "Permissions/Run other programs", "Run other programs",
	  "Whether Run Program and Open URL work. Off, they do nothing and say so in "
	  "the log.\n\n"
	  "Find Program is deliberately outside this: asking whether something is "
	  "installed runs nothing, so a script can still say \"this needs git\" — "
	  "which is the message somebody needs in order to decide whether to grant "
	  "the permission at all.",
	  "", "editor#preferences" },
	{ "Permissions/Network access", "Network access",
	  "Reserved. Nothing reads it yet — the `http` group is a later wave. It is "
	  "here so that a project which has already thought about what it may reach "
	  "does not have to be asked a second time when that group arrives.",
	  "", "editor#preferences" },
	{ "Fonts/Regular", "Regular",
	  "Ordinary body text, and the setting a new project starts with. It is also "
	  "what makes <b> in rich text visible: a bold run is drawn in the same "
	  "family's Bold, which only looks like anything if the text around it is "
	  "not already bold.",
	  "", "editor#preferences" },
	{ "Fonts/Bold", "Bold",
	  "What the engine has always drawn, so a project made before this setting "
	  "existed keeps it and nothing moves.\n\n"
	  "With Bold as the base weight there is nothing bolder to reach for, so <b> "
	  "draws the same letters. That is deliberate: the alternative is a second "
	  "atlas holding the same glyphs and a tag that pretends.",
	  "", "editor#preferences" },
	{ "Fonts/Greek", "Greek",
	  "Adds the Greek alphabet to the font atlas. Off by default, like Cyrillic: "
	  "the base set (Latin, its umlauts and accents, the punctuation and the Euro "
	  "sign) is always there, and everything past it costs room in a texture "
	  "every project pays for.\n\n"
	  "A character the atlas does not carry draws nothing at all — no box, no "
	  "gap in the width — so a Greek label in a project that never ticked this "
	  "is simply blank.",
	  "", "editor#preferences" },
	{ "Fonts/Cyrillic", "Cyrillic",
	  "Adds the Cyrillic alphabet to the font atlas. See Greek above for what the "
	  "base set already covers.\n\n"
	  "Both take effect when the atlas is baked, which happens once per run. The "
	  "exported application bakes on its own start and gets this immediately; "
	  "this editor session has to be restarted, and the page says so when that "
	  "is the case.",
	  "", "editor#preferences" },

	// ── Preferences » Editor » Tool Status ───────────────────────────────────
	// One table of every external thing the editor leans on. The rows are text;
	// only these two are controls.
	{ "Tool Status/Fix", "Fix",
	  "Goes where this row can be put right — the page that installs the tool, "
	  "or the dialog that does it for you. It only appears on rows that are not "
	  "already in order.",
	  "", "editor#preferences" },
	{ "Tool Status/Recheck all", "Recheck all",
	  "Runs all three probes again: git, the C++ toolchain, and the router. The "
	  "results are cached from startup, so this is what to press after installing "
	  "something or changing the network.",
	  "", "editor#preferences" },

	// ── The build-tools dialog ───────────────────────────────────────────────
	// Shown at startup when cmake or a C++ compiler is missing. Its own scope
	// because its "Recheck" and the settings page's are the same word for the
	// same probe in two very different situations.
	{ "Build Tools/Install Automatically", "Install Automatically",
	  "Installs cmake and the compiler through this system's package manager — "
	  "Homebrew and the Xcode Command Line Tools on macOS, winget on Windows, "
	  "apt, dnf or pacman on Linux. It can be a several hundred MB download, and "
	  "the log below says what it is doing.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Copy 'brew install cmake'", "Copy the brew command",
	  "Puts the install command on the clipboard, for running it in your own "
	  "terminal instead of letting the editor do it.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Copy winget Command", "Copy the winget command",
	  "Puts the install command for CMake and the Visual Studio C++ Build Tools "
	  "on the clipboard, for running it in your own shell.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Copy 'sudo apt install build-essential cmake'", "Copy the apt command",
	  "Puts the install command on the clipboard. On a distribution without apt, "
	  "the package names are the same for dnf and pacman.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/cmake.org", "cmake.org",
	  "Opens the CMake download page in a browser, for installing it by hand.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Download Page", "Download Page",
	  "Opens Microsoft's page for the Visual Studio C++ Build Tools, which is "
	  "where the compiler comes from on Windows.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Don't show this again", "",
	  "Stops this dialog appearing at startup. The tools stay missing, and "
	  "Preferences » Editor » Tool Status still says so — this only silences the "
	  "interruption.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Recheck", "Recheck",
	  "Probes for cmake and a compiler again. A clean result closes this dialog "
	  "on its own, so this is what to press after an install finishes elsewhere.",
	  "", "horizoncode#compiler" },

	// ── The material editor ──────────────────────────────────────────────────
	// The graph's nodes are drawn small, so their fields carry the shortest
	// labels anywhere in the editor: "Pow", "Off", "Tile", "Clip". A three-letter
	// label on a node is a control with no explanation at all — which is exactly
	// the case these entries exist for.
	{ "Material Node/Pow", "Power",
	  "How sharply the Fresnel effect falls off towards the middle of a surface. "
	  "1 is a soft, wide rim; higher numbers pull the brightness into a thin edge, "
	  "the way a glancing reflection actually behaves.",
	  "", "materials#nodes" },
	{ "Material Node/Tile", "Tiling",
	  "How often the texture repeats across the mesh's UVs. 1 is one copy over "
	  "the whole unwrap; 4 is sixteen copies, four in each direction.",
	  "", "materials#nodes" },
	{ "Material Node/Off", "Offset",
	  "Slides the texture across the surface. In whole tiles: 0.5 shifts it by "
	  "half a repeat. Animating this is what makes a texture scroll.",
	  "", "materials#nodes" },
	{ "Material Node/Strength", "Normal Strength",
	  "How far the normal map is allowed to tilt the surface. 1 is the map as it "
	  "was authored, 0 flattens it back to the geometry, and past 1 the bumps "
	  "start lighting themselves in ways no real surface does.",
	  "", "materials#nodes" },
	{ "Material Node/Scale", "Noise Scale",
	  "How fine the procedural noise is. Bigger numbers mean smaller speckle: the "
	  "value is how many noise cells fit across one UV unit.",
	  "", "materials#nodes" },
	{ "Material Node/Clear", "Clear",
	  "Drops the picked texture and puts the node back on the material's own "
	  "base texture — whatever the mesh brought with it.",
	  "", "materials#nodes" },
	{ "Material Node/+ Layer", "Add Layer",
	  "Adds a paint layer to this blend node, which also adds its pin. The list "
	  "here IS the set of layers the Landscape tool offers, in this order — one "
	  "RGBA weightmap holds four of them, which is the limit.",
	  "", "materials#nodes" },
	{ "Material Node/Lit", "",
	  "Whether the scene's lights reach this material. Off makes it emissive-flat: "
	  "the graph's colour is the pixel, useful for skies, holograms and anything "
	  "that should not take a shadow.",
	  "", "materials#concept" },
	{ "Material Node/True", "",
	  "The value this constant hands out. It is baked into the shader, so the two "
	  "settings are two different shaders, not one shader with a branch.",
	  "", "materials#nodes" },
	{ "Material Node/Default", "",
	  "What this boolean parameter is before anything overrides it. It stays "
	  "changeable at run time, unlike the static switch.",
	  "", "materials#parameters" },
	{ "Material Node/On (default)", "",
	  "Which branch the static switch takes when nothing overrides it. Static "
	  "means the choice is made while the shader is compiled: the other branch is "
	  "not in the shader at all, so it costs nothing, and flipping this rebuilds "
	  "the shader rather than setting a value.",
	  "", "materials#nodes" },
	{ "Material Node/Set Texture", "Set Texture",
	  "Picks the texture this node samples, from the project's textures. Dragging "
	  "one onto the node from the Content Browser does the same thing.",
	  "", "materials#nodes" },
	{ "Material Node/(mesh texture)", "(mesh texture)",
	  "Leaves the slot empty, which means the node samples whatever texture the "
	  "mesh's material already carries. That is what makes one graph work for many "
	  "different meshes.",
	  "", "materials#nodes" },
	{ "Material Node/Open Function", "Open Function",
	  "Opens the material function this node calls, in its own tab. "
	  "Double-clicking the node does the same.",
	  "", "materials#functions" },
	{ "Material Node/Preview This Node", "Preview This Node",
	  "Puts THIS node's output on the preview ball instead of the finished "
	  "material, unlit. The way to see what a branch of the graph actually "
	  "produces before it is mixed into everything else.",
	  "", "materials#editor-usage" },
	{ "Material Node/Delete Node", "Delete Node",
	  "Removes the node and every link that ran through it. The Output node "
	  "cannot be deleted — a graph without one has nothing to compile.",
	  "", "materials#editor-usage" },
	{ "Material Graph/Delete Comment", "Delete Comment",
	  "Removes the comment frame. The nodes inside it stay where they are; a "
	  "comment groups them visually and owns none of them.",
	  "", "materials#editor-usage" },

	{ "Material Parameter/..", "Parameter metadata",
	  "The range, group and hint for this parameter — everything about how it is "
	  "PRESENTED to whoever sets it later, on a material instance or from a "
	  "script.",
	  "", "materials#parameters" },
	{ "Material Parameter/Min", "",
	  "Lower end of the slider this parameter is edited with. Leave min and max "
	  "equal and it stays a free drag with no bounds at all.",
	  "", "materials#parameters" },
	{ "Material Parameter/Max", "",
	  "Upper end of that slider. It bounds the editing widget, not the shader — "
	  "a script can still write past it.",
	  "", "materials#parameters" },
	{ "Material Parameter/Group", "",
	  "A heading to file this parameter under. Parameters sharing a group are "
	  "shown together, which is what keeps a material with twenty of them "
	  "readable.",
	  "", "materials#parameters" },
	{ "Material Parameter/Tooltip", "",
	  "A sentence for whoever uses this material later. It becomes the hint on "
	  "the parameter's own row, so it is worth writing in the same voice as the "
	  "rest of the editor.",
	  "", "materials#parameters" },

	{ "Material Settings/Clip", "Clip Threshold",
	  "Where a masked material cuts: pixels whose opacity falls below this are "
	  "discarded entirely, leaving a hard edge. It is what makes leaves and "
	  "chain-link fences out of one quad.",
	  "", "materials#concept" },
	{ "Material Preview/Show Material", "Show Material",
	  "Puts the finished material back on the preview ball, after Preview This "
	  "Node put a single node's output there.",
	  "", "materials#editor-usage" },
	{ "Material Preview/Load", "Load",
	  "Loads this mesh for the preview anyway. It is asked because the file is "
	  "large: the whole mesh stays in memory and goes to the GPU. The read runs "
	  "in the background, so the editor keeps working while it happens.",
	  "", "materials#editor-usage" },

	// ── The UI designer ──────────────────────────────────────────────────────
	// The layout fields here are the one part of the editor whose labels CHANGE
	// with the state of another control: anchor an element across a whole side
	// and "Position X" becomes "Left/Right", because on a stretched axis the
	// element has no position, it has two margins. Each shape gets its own
	// entry, and each says why it is the shape it is.
	{ "UI Hierarchy/Canvas", "Canvas",
	  "The root of the widget tree. Selecting it selects nothing in particular, "
	  "which is what puts the canvas settings in the details panel; dropping a "
	  "widget on it moves that widget out to the top level.",
	  "", "ui#designer" },
	{ "UI Hierarchy/Duplicate", "Duplicate",
	  "Copies this widget and everything under it, as a sibling. The copy keeps "
	  "the properties and the layout, and gets its own name.",
	  "", "ui#designer" },

	{ "Canvas/Width", "Canvas Width",
	  "The width this layout is DESIGNED for, in pixels. It is a reference, not a "
	  "promise: what the canvas becomes on a real screen is the Scale mode's "
	  "business.",
	  "", "ui#designer" },
	{ "Canvas/Height", "Canvas Height",
	  "The height the layout is designed for. Together with the width it fixes "
	  "the aspect the anchors were placed against.",
	  "", "ui#designer" },
	{ "Canvas/Scale", "Canvas Scale",
	  "How this canvas meets a screen that is not exactly the size above. Stretch "
	  "fits each axis separately: the canvas always covers the screen exactly, "
	  "and everything on it is distorted the moment the aspect differs. Every "
	  "other mode scales both axes by one factor, so nothing is distorted, and "
	  "treats the size above as a reference for how big things appear — the "
	  "canvas is then as large as the screen really is, so an element anchored to "
	  "an edge stays on that edge.",
	  "", "ui#designer" },

	{ "UI Widget/Name", "",
	  "What this widget is called. The graph finds it by this name, so renaming "
	  "one that the logic already refers to is worth doing deliberately.",
	  "", "ui#designer" },
	// ── The theme's styles ───────────────────────────────────────────────────
	// A role is one colour; a style is a whole kind of element, and the only
	// place a hover or a pressed colour can be themed at all.
	{ "Theme Styles/Add Variant", "",
	  "A named KIND of this type — \"success\", \"danger\", \"ghost\" — that an "
	  "element opts into with its Tag. It layers ON TOP of the plain type's style: "
	  "a variant that names one colour keeps the rounding, the border and "
	  "everything else the base decided, which is the whole reason to write a "
	  "variant instead of a second style. This is CSS's Button.success, with the "
	  "same cascade and none of its combinators.",
	  "", "ui#elements" },
	{ "Theme Styles/Add Named Style", "",
	  "A look that is not tied to one element type. An element points at it by "
	  "name, and it layers over whatever its type already said — so \"Card\" can "
	  "round a Panel and a Button the same way without either of them stopping "
	  "being what it is.",
	  "", "ui#elements" },
	{ "Theme Styles/Add Value", "",
	  "Which further property this VARIANT decides. A variant holds only what is "
	  "different about it — everything it does not name comes from the plain "
	  "type's style above, which already holds every value that type has. The "
	  "list is the element's own: whatever a type exposes, a style can answer "
	  "for, which is why hover and pressed are here and no role vocabulary has "
	  "them.",
	  "", "ui#elements" },
	{ "Theme Styles/Remove Style", "",
	  "Drops this variant or named style. Elements that followed it keep the "
	  "values they last got "
	  "rather than turning white — the same rule a renamed role follows, because "
	  "visible and fixable beats vanished.",
	  "", "ui#elements" },
	{ "Theme Editor/Start from", "",
	  "Replaces every value below with one of the palettes the engine ships: the "
	  "neutral Default, or Amber, which is the editor's own. They are a starting "
	  "point and not a link — once taken, the theme is yours and editing it "
	  "changes nothing else. The asset keeps its own name.",
	  "", "ui#elements" },
	{ "Theme Editor/Use for this Project", "",
	  "Makes this the theme the project's interface resolves against, and the one "
	  "an exported application boots with. It is stored in the project, not in "
	  "this asset — a project has one theme, a folder may hold several.",
	  "", "ui#elements" },
	{ "Theme Editor/Starts in", "",
	  "Which of the two the application opens in. \"System\" follows the desktop "
	  "and keeps following it while the application runs, which is why what is "
	  "stored is the QUESTION and not today's answer — writing down \"Dark\" "
	  "because the machine that built it was dark would ship as dark for "
	  "everybody. A script can still change it later with Set Theme Mode.",
	  "", "ui#elements" },
	{ "Theme Editor/Blur", "",
	  "How far this shadow fades out past the shape, in canvas pixels. Raised is "
	  "a card lying on the page, Overlay is a dialog floating well above it.",
	  "", "ui#elements" },
	{ "Theme Editor/Offset", "",
	  "How far the shadow is pushed from the shape, x then y. It is where the "
	  "light is, and one offset for the whole application is what makes a screen "
	  "look lit rather than assembled.",
	  "", "ui#elements" },
	{ "UI Widget/Style", "",
	  "Which of the theme's styles dresses this element — one decision for its "
	  "whole look, instead of binding value after value below. The entry named "
	  "after the element's TYPE is the theme's answer for every element of that "
	  "type, and it is what a newly placed element follows. A name of your own "
	  "(\"Card\", \"Danger\") is one you point at deliberately. \"None\" means the "
	  "theme's styles leave this element alone, which is where every widget "
	  "authored before styles existed starts.",
	  "", "ui#elements" },
	{ "UI Widget/Tag", "",
	  "Which KIND of its type this element is: a Button with the tag \"success\" "
	  "takes the theme's Button style and then Button.success on top of it, value "
	  "by value. That is CSS's class, and the layering is the point — the variant "
	  "only says what is different, everything else still comes from the plain "
	  "type. The list is what the theme in the toolbar defines for this type, so a "
	  "tag is picked from what exists rather than typed from memory; a tag the "
	  "theme does not define leaves the element an ordinary one of its type.",
	  "", "ui#elements" },
	{ "UI Widget/None (this element decides for itself)", "",
	  "No style. The values below are this element's own and nothing but a role "
	  "binding will change them.",
	  "", "ui#elements" },
	{ "UI Widget/Locked (keep this value)", "",
	  "Holds this one value against the theme: no role and no style may write it. "
	  "Different from \"Literal\", which only means nothing is bound — a style "
	  "answers for properties nobody bound, and this is what keeps it out. It is "
	  "what a component parameter sets when a page tells a component what colour "
	  "to be.",
	  "", "ui#elements" },
	{ "UI Widget/Literal (type it here)", "",
	  "Where this colour comes from. \"Literal\" means you typed it and it is "
	  "yours; picking one of the nine theme ROLES instead means the theme decides "
	  "it, every element with that role changes together, and light/dark switches "
	  "it for you. A bound colour's swatch is read-only, because editing it would "
	  "be overwritten the next time the theme or the mode changes.",
	  "", "ui#elements" },
	{ "UI Widget/Cell (col, row)", "",
	  "Which cell of the Grid this element sits in, counted from 0 at the top "
	  "left. -1 on either number means \"the next free cell\", which is what almost "
	  "everything wants: fill a form from the top and never type a coordinate. "
	  "Naming a cell PINS the element there, and pinned ones are placed first — so "
	  "an element that asked for a cell cannot have it taken by one that did not "
	  "care but happened to come earlier.",
	  "", "ui#elements" },
	{ "UI Widget/Span (cols, rows)", "",
	  "How many cells this element covers, at least one of each. A span takes the "
	  "gaps it crosses with it, so two columns and the space between them are one "
	  "wide cell — that is how a heading sits over a whole form. Cells under a span "
	  "are taken: everything placed automatically steps around it.",
	  "", "ui#elements" },
	{ "UI Widget/Column Sizes", "",
	  "One entry per column, in order. \"120\" is a fixed width, \"*\" is one share of "
	  "whatever is left over, \"2*\" is two shares, and \"auto\" is as wide as the "
	  "widest thing in that column. A form is usually \"auto\" for the labels and "
	  "\"*\" for the fields, which is exactly what two stacked boxes cannot do. "
	  "Anything unreadable is treated as \"*\" — a track you can see and fix beats "
	  "one that collapsed to nothing and hid the typo.",
	  "", "ui#elements" },
	{ "UI Widget/Row Sizes", "",
	  "The same, downwards. If there are more children than declared rows, the LAST "
	  "entry is repeated for as many rows as they need — a settings page with twenty "
	  "rows does not have to declare twenty.",
	  "", "ui#elements" },
	{ "UI Widget/Row Spacing", "",
	  "The gap between two ROWS of a Grid. Spacing is the gap between two columns; "
	  "two numbers because a form almost always wants its rows further apart than "
	  "its columns.",
	  "", "ui#elements" },
	// ── The palette: what each element type IS ───────────────────────────────
	// One line per widget type, because a palette of nineteen bare nouns is a
	// list you have to place things from to find out what they are.
	//
	// These are keyed by the type's own NAME (uiWidgetTypeName), which is why
	// the coverage audit cannot see them: the palette draws ImGui::Button with a
	// computed label, and the scan only reads literals. A runtime test in
	// test_editor_help.cpp asks the registry instead — every registered type must
	// have an entry here, and a new widget type fails that test until it does.
	{ "UI Palette/Panel", "",
	  "A rectangle of one colour, and the plainest container there is. What you "
	  "reach for to give a group of things a background, a border or a rounding.",
	  "", "ui#elements" },
	{ "UI Palette/Image", "",
	  "A picture. Point it at a texture; nine-slice settings let a small source "
	  "stretch into a big frame without the corners smearing.",
	  "", "ui#elements" },
	{ "UI Palette/Text", "",
	  "A run of text. Alignment in nine positions, optional word wrap, and Auto "
	  "Size to make the element fit what it says.",
	  "", "ui#elements" },
	{ "UI Palette/Button", "",
	  "A surface that reacts to the pointer and fires OnClicked. What is written "
	  "ON it is a child — a Text, an Image, or both — which is what lets a button "
	  "carry an icon beside its caption.",
	  "", "ui#elements" },
	{ "UI Palette/CheckBox", "",
	  "A box that is ticked or not, with a label beside it. Fires OnValueChanged "
	  "when it flips.",
	  "", "ui#elements" },
	{ "UI Palette/Slider", "",
	  "A value between a minimum and a maximum, dragged with the pointer.",
	  "", "ui#elements" },
	{ "UI Palette/ProgressBar", "",
	  "A fill between 0 and 1 that shows how far something got. Read-only: it is "
	  "told, it does not ask.",
	  "", "ui#elements" },
	{ "UI Palette/TextInput", "",
	  "An editable field. Placeholder, maximum length, password dots, an input "
	  "filter for numbers-only — and Multiline for a box that holds paragraphs.",
	  "", "ui#elements" },
	{ "UI Palette/ComboBox", "",
	  "A list to pick one entry from, shown as a dropdown. The entries are the "
	  "Options list; Selected Index is which one it shows.",
	  "", "ui#elements" },
	{ "UI Palette/VerticalBox", "",
	  "Stacks its children top to bottom, spaced and padded. A child in a box "
	  "does not place itself — the box decides — and Slot Fill lets one of them "
	  "take the space left over.",
	  "", "ui#elements" },
	{ "UI Palette/HorizontalBox", "",
	  "The same, left to right. A row of buttons with one Spacer set to fill is "
	  "how you push the last of them to the far end.",
	  "", "ui#elements" },
	{ "UI Palette/ScrollBox", "",
	  "A vertical box whose content may be taller than it is. Scrolls with the "
	  "wheel and clips what hangs out.",
	  "", "ui#elements" },
	{ "UI Palette/Spacer", "",
	  "Nothing, with a size. It draws no pixel and takes no click; all it does is "
	  "occupy its slot, which in a box is what pushes everything after it along.",
	  "", "ui#elements" },
	{ "UI Palette/ListView", "",
	  "A list of any length built from ONE authored row. It holds a COUNT and a "
	  "row widget, not the items — so ten thousand entries cost the rows the "
	  "window can show, and you answer OnRowBind for the ones on screen.",
	  "", "ui#elements" },
	{ "UI Palette/WrapBox", "",
	  "A row until it cannot be one: children run left to right and break onto a "
	  "new line when the next will not fit. Tags, chips, a toolbar that has to "
	  "survive a narrow window.",
	  "", "ui#elements" },
	{ "UI Palette/Grid", "",
	  "Rows and columns, with spans. What a FORM is made of: a label column that "
	  "fits its labels beside a field column that takes the rest — which two "
	  "stacked boxes can only fake, and only by hand-matching sizes.",
	  "", "ui#elements" },
	{ "UI Palette/TabBox", "",
	  "Pages behind a strip of tabs, one showing at a time. Its CHILDREN are the "
	  "pages and each child's NAME is its tab label, so there is no second list "
	  "to keep in step.",
	  "", "ui#elements" },
	{ "UI Palette/Splitter", "",
	  "Two panes and a divider you can drag. Exactly two: a three-pane layout is "
	  "a splitter inside a splitter, which says the same thing with the "
	  "arithmetic already solved.",
	  "", "ui#elements" },
	{ "UI Palette/WidgetRef", "",
	  "Another widget, used here as one element. Not offered in the palette as a "
	  "bare type — you pick the widget itself from Components or User Defined, "
	  "which is the same thing without an empty slot to point somewhere first.",
	  "", "ui#elements" },

	// ── Looking at the widget under another theme ────────────────────────────
	// The toolbar cell and the six entries of its popup. All of it is a way of
	// LOOKING: nothing here is stored in the widget, and nothing reaches the
	// running preview.
	{ "ui.theme-preview", "Theme",
	  "Which theme the canvas resolves bound colours against, and in which mode. "
	  "Only the picture changes — the widget keeps its bindings, the preview keeps "
	  "its theme, and nothing is written to the asset. What it is for is the "
	  "question every themed widget raises: does this still read on somebody "
	  "else's palette?",
	  "", "ui#designer" },
	{ "UI Theme Preview/From the preview", "",
	  "Draw with the theme the running preview uses, which is what the widget "
	  "will actually look like when the application runs. The default, and where "
	  "you go back to.",
	  "", "ui#designer" },
	{ "UI Theme Preview/Default", "",
	  "The engine's built-in theme — the one a project has before it sets one. "
	  "Worth a look because it is what an element falls back to when a binding "
	  "no longer resolves.",
	  "", "ui#designer" },
	{ "UI Theme Preview/Amber", "",
	  "The editor's own palette, shipped as a theme. A second real palette to "
	  "check against without authoring one first.",
	  "", "ui#designer" },
	{ "UI Theme Preview/Follow the preview", "",
	  "Light or dark as the preview resolved it — which, when the project follows "
	  "the desktop, is whatever this machine is set to.",
	  "", "ui#designer" },
	{ "UI Theme Preview/Light", "",
	  "Force the light half of every role, whatever the preview is in. Each role "
	  "carries both values, so this is the same theme read the other way, not a "
	  "second one.",
	  "", "ui#designer" },
	{ "UI Theme Preview/Dark", "",
	  "Force the dark half. The pair with Light is the cheap way to find the "
	  "colour somebody typed as a literal instead of binding to a role: it is the "
	  "one that does not change.",
	  "", "ui#designer" },

	{ "Canvas/Theme", "",
	  "The theme THIS widget resolves against, overriding the project's. Empty is "
	  "the normal answer — a project has one theme and everything follows it — and "
	  "this is for the widget that must not: a launcher, an overlay, a screen that "
	  "has to look like somebody else's product. It covers everything embedded in "
	  "this widget too; one widget, one theme, no cascade of themes down the tree. "
	  "The designer draws with it as soon as it is set, since that is what the "
	  "running application would do.",
	  "", "ui#elements" },
	{ "Canvas/Follow the Theme", "",
	  "Switches every element of this widget over to the theme's styles at once. "
	  "An element read from a file that predates styles follows none — that is "
	  "what keeps opening an old widget from repainting it — and this is the one "
	  "button that undoes that decision for the whole widget. Each element still "
	  "follows the style named after its own TYPE unless you point it somewhere "
	  "else, and anything locked or bound to a role stays where it is.",
	  "", "ui#elements" },

	// ── The timeline: animations a widget owns ───────────────────────────────
	// A track is one property of one element, a key is what that property is at
	// a moment, and the easing sits on the key. The same three words every
	// animation editor uses, in the vocabulary this one already had.
	{ "UI Timeline/Animation", "",
	  "Which of this widget's animations the timeline below is showing. They "
	  "belong to the widget and travel with it, so a component brings its own — "
	  "and a page that embeds it can play them by name.",
	  "", "ui#elements" },
	{ "UI Timeline/New", "",
	  "Makes an animation and names it. The name is the identity: a graph plays "
	  "it by name. Renaming it later is safe — \"Rename\" carries this widget's "
	  "own Play and Stop nodes with it and asks about the rest of the project.",
	  "", "ui#elements" },
	{ "UI Timeline/Rename", "",
	  "Renames this animation and retargets what plays it. The Play and Stop "
	  "nodes in this widget's own graph follow immediately; every other asset "
	  "that names it is listed first and only rewritten once you say so. A node "
	  "that names it but whose target cannot be proved — a Play on a widget this "
	  "graph was handed rather than one it made — is reported and left alone, "
	  "because renaming it on the strength of the name would break graphs that "
	  "were correct.",
	  "", "ui#elements" },
	{ "UI Timeline/Length", "",
	  "How much time you have room to work in, which is not the same as how "
	  "long the animation runs: playing stops at the LAST KEY, and the lane "
	  "greys out everything after it. Keys past the end still exist and still "
	  "hold their value — shortening the clip hides them rather than throwing "
	  "them away, so lengthening it again brings them back.",
	  "", "ui#elements" },
	{ "UI Timeline/Loop", "",
	  "Runs it again from the start instead of ending. A looping animation never "
	  "reports finished — the only way it ends is somebody stopping it — which is "
	  "what a spinner or a pulse wants.",
	  "", "ui#elements" },
	{ "ui.timeline-play", "Play",
	  "Plays the animation in the DESIGNER — the running application is not "
	  "involved — and stops at the last key, exactly where the running one "
	  "would. Drag the ruler to scrub by hand; either way the canvas shows the "
	  "clip at the playhead. To see the widget's own values again, pick "
	  "\"(none)\" as the animation.",
	  "", "ui#elements" },
	{ "UI Timeline/Zoom In", "",
	  "Spreads the time axis out around the playhead, so keys milliseconds "
	  "apart become separate things you can grab. The ruler follows: its labels "
	  "turn from seconds into milliseconds by themselves. The wheel over the "
	  "lane does the same around the pointer, and Shift+wheel slides along.",
	  "", "ui#elements" },
	{ "UI Timeline/Zoom Out", "",
	  "Back towards the whole clip in one lane. Zoom stops at fit — there is "
	  "nothing outside an animation to look at.",
	  "", "ui#elements" },
	{ "UI Timeline/Fit", "",
	  "The whole clip across the lane again, from the start. The way back when "
	  "zooming has left you somewhere in the middle of a long animation.",
	  "", "ui#elements" },
	{ "UI Timeline/Time", "",
	  "When the selected key happens. The playhead follows while you drag it, "
	  "so the canvas keeps showing the key you are moving — the same thing "
	  "dragging its diamond does, with a number for when you know the number.",
	  "", "ui#elements" },
	{ "UI Timeline/Value", "",
	  "What the animated property IS at this key. This is where an animation is "
	  "edited: the Details panel writes the widget's own values, which is what "
	  "the widget looks like with no animation playing.",
	  "", "ui#elements" },
	{ "UI Timeline/Ease", "",
	  "The curve leading INTO this key — how the value arrives from the one "
	  "before it. On the first key it means nothing; there is nothing to come "
	  "from. Out curves land softly, In curves leave softly, and Out Back "
	  "overshoots and settles, which is what makes a dialog land.",
	  "", "ui#elements" },
	{ "UI Timeline/Add Track", "",
	  "Adds a row for one property of the SELECTED element. Only what can be "
	  "interpolated is offered — a number, a colour, a point — because a string "
	  "has no halfway and a track that snapped at the end would be a duration "
	  "that means nothing. The new track starts with a key holding the value the "
	  "element has right now.",
	  "", "ui#elements" },
	{ "UI Timeline/Key", "",
	  "Adds a key at the playhead, holding whatever the animation already shows "
	  "there, and replaces one that was already at that moment. Adding it "
	  "changes nothing on its own — that is the point: you place the moment "
	  "first, then type what should happen at it in the row below.",
	  "", "ui#elements" },
	{ "UI Timeline/Delete Key", "",
	  "Removes this key. The track keeps the others, and a track with one key "
	  "left is simply a constant.",
	  "", "ui#elements" },

	// ── Components: what a widget offers whoever embeds it ───────────────────
	{ "Canvas/Parameter Name", "",
	  "What a page that embeds this widget calls this knob — \"Label\", \"Help Text\", "
	  "\"Icon\". This name is what the page STORES, not the element and property "
	  "underneath it, which is why you can rebuild the inside of this widget and "
	  "every page that uses it keeps working. Rename it and the pages that set it "
	  "fall back to the default, so rename early or not at all.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "Canvas/Element", "",
	  "Which element inside this widget the parameter writes. Listed by the same "
	  "names the hierarchy shows, so what you pick here is findable up there — "
	  "which is a reason to name the element before exposing it.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "Canvas/Property", "",
	  "Which of that element's properties the parameter sets. Only the ones the "
	  "element really has are offered, and picking a different element clears this "
	  "again: a parameter naming a property that is not there would write nothing "
	  "at all, quietly.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "Canvas/Help", "",
	  "One sentence, shown to whoever sets this parameter on a page. Worth writing "
	  "for anything whose name does not already say it — the person setting it is "
	  "looking at your component from the outside and cannot see what it does.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "Canvas/Add Parameter", "",
	  "Adds a knob to this widget, pointed at whatever is selected. A widget with "
	  "no parameters is a page: it can be embedded, but every copy of it looks "
	  "exactly the same. One with parameters is a component — the same form row "
	  "used twenty times with twenty different labels.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "UI Widget/Default", "",
	  "Stops setting this parameter on this copy, so it shows whatever the widget "
	  "itself was authored with. Not the same as clearing the field: an empty text "
	  "box is a label that says nothing, this is a label that says what its "
	  "component says.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "UI Widget/Parameters", "",
	  "What the embedded widget lets this page set. Each row is a knob that widget "
	  "declared for itself; what you see before touching one is the default it was "
	  "authored with, and \"Default\" gives that back. The values are written in when "
	  "the widget is created and not again — this is a starting state, not a live "
	  "link, so anything that has to change while the app runs changes through a "
	  "script.",
	  // The handbook has no "components" section yet (the docs live in the
	  // Website repository). Pointed at the one that exists rather than at one
	  // that would 404 — a dead link teaches people the button does nothing.
	  "", "ui#widgets" },
	{ "UI Widget/Line Spacing", "",
	  "The gap between two LINES of a Wrap Box. Its own number beside Spacing, "
	  "which is the gap between two items on the same line — a row of chips almost "
	  "always wants to sit tighter sideways than it does downwards, and one number "
	  "for both would force a compromise nobody wants.",
	  "", "ui#elements" },
	{ "UI Widget/Tooltip", "",
	  "What this element says about itself when the pointer rests on it for half a "
	  "second. Empty means nothing to say, which is most elements. The DELAY is the "
	  "point: a hint that appears the instant you cross a button is a hint in the "
	  "way of using it. The nearest element upwards that has one wins, so putting "
	  "it on a button covers the caption sitting on that button too.",
	  "", "ui#elements" },
	{ "UI Widget/Row Widget", "",
	  "The widget one row of this list is made of — authored once, in this same "
	  "designer, and repeated. The list keeps no items: the running application "
	  "says how many there are (Set List Count) and the list asks On Row Bind to "
	  "fill in each row it actually puts on screen. That is why ten thousand items "
	  "cost the ten rows you can see instead of ten thousand elements. Without a "
	  "row widget a list can never show anything, which the designer says on the "
	  "list itself.",
	  "", "ui#elements" },
	{ "UI Widget/Selection", "",
	  "What clicking a row does. \"None\" is a list you only read. \"Single\" picks "
	  "one and drops the one before it. \"Multiple\" toggles each row you click, so "
	  "a second one adds rather than replaces — no key held down. Either way the "
	  "list fires On Selection Changed with the item's index, and Get List Selected "
	  "answers it later.",
	  "", "ui#elements" },
	{ "UI Widget/Corner Radius", "",
	  "How far the surface's corners are rounded off, in canvas pixels. It is one "
	  "number for all four; at half the shorter side the shape becomes a capsule, "
	  "and no further. Text and the layout boxes do not have it, because they have "
	  "no surface to round.",
	  "", "ui#elements" },
	{ "UI Widget/Per corner", "",
	  "Rounds each corner on its own: the four fields are laid out where the "
	  "corners are, top row first. A tab, a chat bubble and the head of a card are "
	  "all one rounded and one square pair. Switching back off keeps the top-left "
	  "value and gives it to the other three.",
	  "", "ui#elements" },
	{ "UI Widget/Border Width", "",
	  "An outline drawn INSIDE the surface, following its rounding. Inside rather "
	  "than around it, so a thicker line never overlaps the neighbours: the "
	  "element's rectangle is what the layout gave it and does not grow.",
	  "", "ui#elements" },
	{ "UI Widget/Border Color", "",
	  "The outline's colour. Its own alpha, so a faint hairline over a dark "
	  "surface needs no second element.",
	  "", "ui#elements" },
	{ "UI Widget/Gradient", "",
	  "Fades the surface from its own colour to a second one instead of filling it "
	  "flat. It is a property, not a shader: it costs no material, shows in the "
	  "designer straight away, and works in every backend.",
	  "", "ui#elements" },
	{ "UI Widget/Gradient Color", "",
	  "The colour the surface fades TO. It starts at the element's own colour, so "
	  "a gradient is two colours and an angle and nothing else.",
	  "", "ui#elements" },
	{ "UI Widget/Gradient Shape", "",
	  "Linear fades along an angle, across the whole surface. Radial fades out of "
	  "the middle to the farthest corner — a glow behind a dialog, a spotlit card, "
	  "the soft centre of a button. A radial fade has no direction, so the angle "
	  "is not offered while it is picked.",
	  "", "ui#elements" },
	{ "UI Widget/Gradient Angle", "",
	  "Which way the fade runs, clockwise from straight down: 0 fades top to "
	  "bottom, 90 left to right. Down at zero because a vertical fade is what a "
	  "button or a header almost always wants, and the common case should need no "
	  "number typed into it.",
	  "", "ui#elements" },
	{ "UI Widget/Shadow", "",
	  "A drop shadow: the element's own shape drawn again underneath it, in one "
	  "colour, offset and softened. It is not a blur pass — it is one more "
	  "rectangle, which is why it costs nothing and works everywhere.",
	  "", "ui#elements" },
	{ "UI Widget/Shadow Color", "",
	  "The shadow's colour, alpha included. Real shadows are not black: a shadow "
	  "tinted towards the surface under it looks like light instead of like dirt.",
	  "", "ui#elements" },
	{ "UI Widget/Shadow Blur", "",
	  "How far the shadow fades out past the shape, in canvas pixels. Small and "
	  "tight reads as a card lying on the page, wide and faint as something "
	  "floating well above it.",
	  "", "ui#elements" },
	{ "UI Widget/Shadow Offset", "",
	  "How far the shadow is pushed from the shape, x then y, positive being right "
	  "and down. It is where the light is: one offset for everything on a screen "
	  "looks lit, a different one per element looks broken.",
	  "", "ui#elements" },
	{ "UI Widget/Inner Shadow", "",
	  "The same falloff cast INWARDS from the element's own edge, drawn on its "
	  "surface: a pressed key, a well, a field sunk into the page. Independent of "
	  "the drop shadow — an element may have both, one, or neither.",
	  "", "ui#elements" },
	{ "UI Widget/Inner Shadow Color", "",
	  "The colour darkening the inside of the rim, alpha included.",
	  "", "ui#elements" },
	{ "UI Widget/Inner Shadow Blur", "",
	  "How deep into the element the rim shading reaches, in canvas pixels.",
	  "", "ui#elements" },
	{ "UI Widget/Text Align", "",
	  "Where the text sits inside its own box: the nine cells are left/centre/right "
	  "across and top/middle/bottom down. This is not the anchor — the anchor places "
	  "the element in its parent, this places the letters in the element. A caption "
	  "stretched across a button needs this one.",
	  "", "ui#designer" },
	{ "UI Widget/Slot Fill", "",
	  "0 keeps the widget's own size along the box's axis. Above 0 it takes a "
	  "share of the space left over instead, split between the filling children "
	  "in proportion — two at 1 each take half.",
	  "", "ui#widgets" },
	{ "UI Widget/Width", "",
	  "How wide the widget is, in canvas pixels. It is gone from this panel on an "
	  "axis the widget is stretched across: there it has margins, not a width.",
	  "", "ui#designer" },
	{ "UI Widget/Height", "",
	  "How tall the widget is. Same rule as the width — a stretched axis has "
	  "margins instead.",
	  "", "ui#designer" },
	{ "UI Widget/Position", "",
	  "Where the widget's pivot sits, measured from its anchor. Not from the "
	  "corner of the screen: that is the whole point of the anchor, and it is why "
	  "these numbers stay small and stay right on a different screen.",
	  "", "ui#designer" },
	{ "UI Widget/Position X", "",
	  "The horizontal half of that, shown alone because the vertical axis is "
	  "stretched and has margins instead.",
	  "", "ui#designer" },
	{ "UI Widget/Position Y", "",
	  "The vertical half, shown alone because the horizontal axis is stretched.",
	  "", "ui#designer" },
	{ "UI Widget/Size", "",
	  "Width and height together. A container set to size itself to its content "
	  "owns these two numbers, so there they are greyed out.",
	  "", "ui#designer" },
	{ "UI Widget/Pivot", "",
	  "The point of the widget that its position refers to, and the point it "
	  "rotates about. 0,0 is the top left corner, 0.5,0.5 the middle, 1,1 the "
	  "bottom right.",
	  "", "ui#designer" },
	{ "UI Widget/Left/Right", "",
	  "The margins from the parent's left and right edges. They replace position "
	  "and width because this widget is anchored across the whole horizontal "
	  "axis: it has no width of its own there, it has two distances.",
	  "", "ui#designer" },
	{ "UI Widget/Top/Bottom", "",
	  "The same for the vertical axis: distance from the top edge, distance from "
	  "the bottom.",
	  "", "ui#designer" },
	{ "UI Widget/Offset TL", "",
	  "Distance from the anchored top and left edges of the parent. Stretched on "
	  "both axes, a widget is four margins and nothing else — all four at 0 means "
	  "exactly the anchored area.",
	  "", "ui#designer" },
	{ "UI Widget/Offset BR", "",
	  "Distance from the anchored right and bottom edges. Positive numbers pull "
	  "the edge inwards, whichever edge it is.",
	  "", "ui#designer" },
	{ "UI Widget/Layer", "",
	  "Who is drawn on top when two widgets overlap. Higher wins; ties fall back "
	  "to the order in the hierarchy.",
	  "", "ui#designer" },
	{ "UI Widget/Visible", "",
	  "Off hides this widget and everything inside it. It still exists, the graph "
	  "can still reach it, and it takes no clicks while hidden.",
	  "", "ui#widgets" },
	{ "UI Widget/Enabled", "",
	  "Off greys the widget out and makes it inert — this one and everything "
	  "inside it. Visible but not usable, which is the state a form control wants "
	  "while it is not applicable.",
	  "", "ui#widgets" },
	{ "UI Widget/Rotation", "",
	  "Turns this widget AND its children about the pivot. Layout is computed "
	  "unrotated, so a tilted widget does not shove its neighbours around.",
	  "", "ui#designer" },
	{ "UI Widget/Opacity", "",
	  "Fades this widget and everything inside it. One value on a root panel "
	  "fades a whole menu, which is what a menu fade-in is made of.",
	  "", "ui#widgets" },
	{ "UI Widget/Hover cursor", "",
	  "Which mouse cursor is shown while the pointer is over this widget. It "
	  "needs the widget to be hit-testable, since a widget the pointer passes "
	  "through is never hovered.",
	  "", "ui#runtime" },
	{ "UI Widget/Hit-testable", "",
	  "Off makes the widget transparent to the pointer: clicks pass straight "
	  "through it to whatever is behind. For decoration that must not swallow "
	  "input.",
	  "", "ui#runtime" },
	{ "UI Widget/Clip children", "",
	  "Cuts everything inside this widget off at its own edge. Clipped pixels are "
	  "neither drawn nor clickable — it is what makes a list longer than its box "
	  "look like a list in a box instead of spilling across the screen.",
	  "", "ui#widgets" },
	{ "UI Widget/Focus frame", "",
	  "While anything INSIDE this element has the keyboard focus, the focus ring "
	  "is drawn around this element instead of around whatever holds it. A "
	  "search field is a rounded frame with an icon and a text field inset "
	  "between them: the field takes the keys, but the frame is what a person "
	  "sees as the control, and a ring around the field alone is a small "
	  "rectangle floating inside a pill. Set it on the frame.",
	  "", "ui#widgets" },
	{ "UI Widget/Accepts drop", "",
	  "Makes this element a drop zone: a file dragged in from the desktop and let "
	  "go over it fires On File Dropped, once per file, with the file's path. The "
	  "drop travels UP from whatever the pointer met to the first element that "
	  "accepts one, so set it on the panel and not on the label inside it. Where "
	  "nothing accepts, the window took it and the Game Instance hears it instead.",
	  "", "ui#widgets" },
	{ "UI Widget/RichText", "",
	  "Reads the Text as MARKUP instead of as plain words. What it understands: "
	  "<color=#ff8800>, <size=1.5>, <b> and <link=id>, each closed by </>, plus "
	  "<icon=home> for one of the built-in icons and << for a literal '<'. An "
	  "icon inserts a single character and inherits the colour, size and link it "
	  "stands in. <b> needs the project's text weight to be Regular, since "
	  "nothing is bolder than bold (Preferences > Project > Fonts). A link makes "
	  "that stretch of words "
	  "clickable — the pointer turns into a hand over it and On Link Clicked "
	  "fires with the id — while the rest of the label stays as inert as any "
	  "other text. Anything the parser does not fully understand is left as "
	  "text, so a stray '<' shows up as one rather than eating the sentence. "
	  "Off by default: a label written before this may hold a '<' that means a "
	  "'<'. Sizes are a MULTIPLE of Font Size, not pixels.",
	  "", "ui#widgets" },
	{ "UI Widget/Draggable", "",
	  "Lets this element be picked up and carried onto a drop zone. The drag "
	  "begins at a DISTANCE, not at the press, so a draggable element can still "
	  "be clicked. It fires On Drag Started when it lifts and On Drag Ended when "
	  "it lands, with a bool saying whether anything took it; the zone it was "
	  "dropped on gets On Drop. Like Accepts drop, this bubbles: pressing the "
	  "label of a draggable card carries the card.",
	  "", "ui#widgets" },
	{ "UI Widget/Drag payload", "",
	  "What this element calls itself when it is dropped somewhere: the string "
	  "the target receives with On Drop. A list row writes its index in here, a "
	  "tool button its tool. Left empty it falls back to the element's name, "
	  "which is enough for dragging one named thing onto another without a line "
	  "of logic.",
	  "", "ui#widgets" },

	{ "UI Graph/Event Graph", "Event Graph",
	  "The widget's main graph, where the event handlers live. The functions "
	  "below it are sub-graphs of their own; this is the one that runs by itself.",
	  "", "ui#graph" },
	{ "UI Graph/Get", "Get",
	  "Adds a node that READS this property or variable. Dropping something from "
	  "the hierarchy or the variables list onto the canvas is what offers this.",
	  "", "ui#graph" },
	{ "UI Graph/Set", "Set",
	  "Adds a node that WRITES it. A function-local variable can only be used "
	  "inside its own function, which is why this is sometimes greyed out.",
	  "", "ui#graph" },
	{ "UI Graph/Get Ref", "Get Ref",
	  "The same read, but through a REFERENCE: the node takes a widget and the "
	  "element's NAME instead of being tied to this one element. Leave its "
	  "Target unwired and it means this widget, so it behaves like the plain "
	  "Get — wire one in and the same node reads the same element of a widget "
	  "somewhere else. That is what lets one function serve every card on a "
	  "page instead of one node per card.",
	  "", "ui#graph" },
	{ "UI Graph/Set Ref", "Set Ref",
	  "The same write through a reference. Needs the element to have a name, "
	  "because a name is what a reference can carry — an id belongs to the asset "
	  "that made it and means nothing in another widget.",
	  "", "ui#graph" },
	{ "UI Graph/Get Widget Ref", "Get Widget Ref",
	  "A reference to the COMPONENT sitting in this slot. A component is a class "
	  "like any other, so from here you can call its functions, bind its events "
	  "and read its public variables — and tell one of three identical cards "
	  "apart. Offered only on a slot, because only a slot holds an instance; "
	  "everything else on this menu is a property.",
	  "", "ui#graph" },
	{ "UI Graph/Show the node that failed", "Show the node that failed",
	  "Jumps to the node the compile check stopped at, opening its sub-graph and "
	  "selecting it. The message says what went wrong; this says where.",
	  "", "horizoncode#compiler" },

	{ "UI Variable/Name", "",
	  "What this variable is called. The rename carries: every Get and Set node "
	  "using it is renamed with it, so the wiring survives.",
	  "", "ui#graph" },
	{ "UI Variable/Access", "",
	  "Public can be read and written from a script through "
	  "horizon.callWidgetFunction and friends. Private is the widget's own "
	  "business.",
	  "", "ui#graph" },
	{ "UI Variable/Position##vdef", "Default Position",
	  "The position this transform variable starts at, when the widget is "
	  "created. It is a starting value, not a binding to anything.",
	  "", "ui#graph" },
	{ "UI Variable/Rotation##vdef", "Default Rotation",
	  "The rotation it starts at, in degrees per axis.", "", "ui#graph" },
	{ "UI Variable/Scale##vdef", "Default Scale",
	  "The scale it starts at. 1,1,1 is unscaled.", "", "ui#graph" },
	{ "UI Variable/Delete Variable", "Delete Variable",
	  "Removes the variable. Get and Set nodes that used it are left where they "
	  "are and read a default from then on, so nothing breaks silently — but "
	  "nothing is repaired either.",
	  "", "ui#graph" },

	{ "UI Graph Node/Name", "",
	  "What this function is called. Calls resolve by name, so renaming it "
	  "renames the Call and Return nodes that named it and the wiring survives.",
	  "", "ui#graph" },
	{ "UI Graph Node/Access", "",
	  "Public makes the function callable from a script through "
	  "horizon.callWidgetFunction(). Private keeps it inside the widget.",
	  "", "ui#graph" },
	{ "UI Graph Node/Event", "",
	  "Which event this handler answers. Picked from the bound widget's own "
	  "events, or typed freely when the node is bound to no particular widget — "
	  "Construct, Tick and Destruct are fired by every widget there is.",
	  "", "ui#graph" },
	{ "UI Graph Node/(Any)", "(Any)",
	  "Bind this node to no particular widget. It then answers for the widget as "
	  "a whole, which is what the lifecycle events need.",
	  "", "ui#graph" },
	{ "UI Graph Node/Property", "",
	  "Which property of the bound widget this node reads or writes — its own "
	  "type-specific ones plus the shared set every widget has. Picking one "
	  "re-types the node's value pin, and a wire that no longer typechecks is "
	  "dropped.",
	  "", "ui#graph" },
	{ "UI Graph Node/Function", "",
	  "Which of this widget's functions the call runs. Picking one re-syncs the "
	  "call's pins with that function's inputs and results.",
	  "", "ui#graph" },
	{ "UI Graph Node/Delete Node", "Delete Node",
	  "Removes the selected node and the links that ran through it.",
	  "", "ui#graph" },

	// ── Input actions ────────────────────────────────────────────────────────
	// An input asset is a list of actions and, under each, the physical things
	// that fire it. The panel's buttons are short verbs whose difference is the
	// whole point: Bind REPLACES one row, Auto Detect ADDS a row.
	{ "Input Action/Button", "",
	  "The action is on or off — a key, a mouse button, a pad button. It fires a "
	  "pressed and a released event and carries no number.",
	  "", "systems#input" },
	{ "Input Action/Axis", "",
	  "The action is one number, from -1 to 1. A stick or a trigger drives it "
	  "directly; a pair of keys drives it as the + and the - half.",
	  "", "systems#input" },
	{ "Input Action/Axis 2D", "",
	  "The action is two numbers, a direction rather than an amount — a stick, or "
	  "four keys. Movement wants this rather than two separate axes, because "
	  "diagonal is then one movement and not two.",
	  "", "systems#input" },
	{ "Input Action/Bind", "Bind",
	  "Arms this row and waits for the input it should read — a key, a gamepad "
	  "button, a mouse button, a stick or a trigger, whichever the row is for. "
	  "What you press REPLACES what the row had. Click again or press Esc to "
	  "cancel; a mouse row takes Esc only, because the click would be the "
	  "binding.",
	  "", "systems#input" },
	{ "Input Action/+ Add Binding", "Add Binding",
	  "Opens a searchable list of everything this action could be bound to, "
	  "narrowed to what its type can use. The way in when you know the name of "
	  "the key or button you want.",
	  "", "systems#input" },
	{ "Input Action/Auto Detect", "Auto Detect",
	  "Listens on the keyboard, the mouse and the gamepad at once, and ADDS "
	  "whatever you press first as a new binding. To replace one that is already "
	  "there, use the Bind button in its own row instead.",
	  "", "systems#input" },
	{ "Input Action/Press any input\xE2\x80\xA6", "Press any input",
	  "Auto Detect is listening. Press a key, click a mouse button or press a "
	  "gamepad button and it is added. On an axis action a key or pad button "
	  "becomes the + half of the pair, and moving a stick or pulling a trigger "
	  "binds that instead. Esc, or this button, cancels.",
	  "", "systems#input" },
	{ "Input Action/+ Add Action Entry", "Add Action Entry",
	  "Adds another action to this asset — one more named thing the game can "
	  "ask about, with its own bindings.",
	  "", "systems#input" },
	{ "Input Action/Fires while the game is paused", "",
	  "A pause (Set Time Scale 0) silences every action by default, or the "
	  "player would keep shooting through the pause menu. Switch this on for the "
	  "few that have to get through anyway: opening and closing the menu, "
	  "navigating it, confirming. Presses that arrive while a silenced action is "
	  "paused are dropped, not queued for the moment the game resumes.",
	  "", "systems#input" },

	// ── Landscape: the creation form ─────────────────────────────────────────
	// Nine numbers that decide what the ground looks like before there is any
	// ground to look at. Octaves, Lacunarity and Gain are the noise vocabulary,
	// and nobody should have to know it to make a hill.
	{ "New Landscape/Width (X)", "",
	  "How wide the new landscape will be along X, in metres. The green preview "
	  "grid in the viewport is this size.",
	  "", "scenes#terrain" },
	{ "New Landscape/Depth (Z)", "",
	  "How deep it will be along Z, in metres. With the width it sets the "
	  "footprint the preview grid shows.",
	  "", "scenes#terrain" },
	{ "New Landscape/Resolution", "",
	  "How many vertices the landscape has per side, from 2 to 512. Detail and "
	  "cost both grow with the square of this number, so a wide landscape does "
	  "not need a high one unless it is seen from close up.",
	  "", "scenes#terrain" },
	{ "New Landscape/Height Scale", "",
	  "How tall the generated hills are, in metres from the lowest point to the "
	  "highest. It scales the noise, so it does nothing at all while the seed is "
	  "0 and the ground is flat.",
	  "", "scenes#terrain" },
	{ "New Landscape/Seed", "",
	  "Which random landscape is generated. 0 makes flat ground; any other "
	  "number is a different set of hills, reproduced exactly from the same "
	  "number. The shape is baked into editable heights the moment the landscape "
	  "is created, so the seed is a starting point and not a live setting.",
	  "", "scenes#terrain" },
	{ "New Landscape/Octaves", "",
	  "How many layers of noise are stacked into the shape, from 1 to 8. Each "
	  "further layer adds finer detail on top of the big landforms, and costs a "
	  "little more to generate.",
	  "", "scenes#terrain" },
	{ "New Landscape/Frequency", "",
	  "How many hills the noise packs across the landscape. The noise runs from "
	  "0 to 1 over the whole terrain, so raising this makes the same ground "
	  "bumpier rather than bigger.",
	  "", "scenes#terrain" },
	{ "New Landscape/Lacunarity", "",
	  "How much finer each noise layer is than the one below it. 2, the usual "
	  "value, means every layer has twice the detail of the last.",
	  "", "scenes#terrain" },
	{ "New Landscape/Gain", "",
	  "How much each finer noise layer contributes compared with the one below, "
	  "between 0 and 1. Under 0.5 gives smooth rolling ground, above it rough "
	  "and noisy ground.",
	  "", "scenes#terrain" },
	{ "New Landscape/Create Landscape", "",
	  "Creates the landscape from the settings above and selects it: one Terrain "
	  "entity with a transform, the Terrain component and the engine's default "
	  "landscape material. With a seed other than 0 the generated surface is "
	  "baked into editable per-vertex heights straight away, so the sculpt "
	  "brushes start from those hills rather than from flat ground.",
	  "", "scenes#terrain" },

	// ── Landscape: the tools, once there is ground ───────────────────────────
	{ "Landscape/Material", "Landscape Material",
	  "The material the landscape is shaded with — drag one onto it from the "
	  "Content Browser. It is also what makes painting possible: the layers you "
	  "can paint are the ones named by the material's Landscape Layer Blend "
	  "node, and with no such node the Paint tool stays greyed out.",
	  "", "scenes#terrain" },
	{ "Landscape/Reset to Engine Default", "",
	  "Puts the built-in landscape material back on the terrain. It is only "
	  "offered while a material of your own is assigned.",
	  "", "scenes#terrain" },
	{ "Landscape/Sculpt", "Sculpt",
	  "Shapes the ground itself: the six brushes below raise, lower, smooth, "
	  "flatten, ramp and roughen the heights under the cursor.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Paint", "Paint",
	  "Paints the material's layers onto the ground instead of reshaping it. It "
	  "needs a material whose graph has a Landscape Layer Blend node, because "
	  "that node's list IS the set of layers there are to paint — without one "
	  "this stays greyed out.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Layer", "Paint Layer",
	  "Which of the material's layers the brush paints. The names come from the "
	  "material's Landscape Layer Blend node, in weightmap-channel order, and "
	  "one weightmap holds four of them.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Radius", "Brush Radius",
	  "The inner, full-strength part of the brush, in metres — the tight circle "
	  "drawn on the ground. The same value is used for sculpting and for "
	  "painting, so changing it in one mode changes it in the other.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Falloff", "Brush Falloff",
	  "How far past the radius the brush keeps working, in metres — the faint "
	  "outer circle. Strength falls off linearly from full at the radius to "
	  "nothing at the outer edge, so 0 gives a hard-edged brush and a large "
	  "value a very soft one. Shared with painting.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Strength", "Brush Strength",
	  "How fast the brush works while the left button is held. Raise and Lower "
	  "move the ground by roughly this many metres a second under the "
	  "full-strength part of the brush; Smooth, Flatten and Ramp use it as a "
	  "rate of blending towards their target instead.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Weightmap", "Weightmap Resolution",
	  "How many texels per side the layer weightmap has, from 32 to 2048, "
	  "stretched over the whole landscape — a large terrain needs more of them "
	  "before a painted edge stops looking blocky. It is locked as soon as "
	  "anything has been painted, because changing it would throw the existing "
	  "paint away; Clear Paint unlocks it again.",
	  "", "scenes#terrain" },
	{ "Landscape/Raise", "Raise",
	  "Pulls the ground up under the brush while you drag, at the brush's "
	  "strength per second.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Lower", "Lower",
	  "Pushes the ground down, the same way round.", "", "editor#landscape-mode" },
	{ "Landscape/Smooth", "Smooth",
	  "Averages each height with its neighbours, which takes the sharpness out "
	  "of whatever the other brushes left behind.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Flatten", "Flatten",
	  "Levels the ground towards one height: the height under the cursor when "
	  "the drag BEGAN. That is what makes it a plateau tool rather than a "
	  "smoother — where you start the drag decides the level.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Ramp", "Ramp",
	  "Blends between two heights along the drag: the ground at the start of the "
	  "stroke and the ground at the end. A road up a hillside is one drag.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Roughen", "Roughen",
	  "Adds fixed noise bumps under the brush, for ground that came out too "
	  "clean to read as earth.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Clear Paint", "",
	  "Throws away every painted layer weight, putting the whole landscape back "
	  "on the first layer. It is one undo step, but it removes the entire "
	  "weightmap rather than the last stroke — and it is what unlocks the "
	  "weightmap resolution again.",
	  "", "editor#landscape-mode" },
	{ "Landscape/Reset Sculpting", "",
	  "Discards every sculpted height, so the landscape falls back to the shape "
	  "its Seed and noise settings generate — flat ground if the seed is 0. It "
	  "is one undo step, but it drops the whole sculpt, not the last stroke. "
	  "Painted layers are left alone.",
	  "", "editor#landscape-mode" },

	// ── The Environment window ───────────────────────────────────────────────
	{ "Environment Window/Select", "Select the entity",
	  "Selects the Sky or Weather entity, which is what puts its settings into "
	  "the Details panel. This window only adds and removes them; everything "
	  "there is to tune lives on the entity itself.",
	  "", "editor#environment-window" },
	{ "Environment Window/Add Sky", "",
	  "Creates the scene's Sky and selects it: an entity carrying the "
	  "Environment component, with the built-in sun and moon lights attached "
	  "underneath it. Without a sky the background is flat and there is no "
	  "sunlight.",
	  "", "scenes#sky-weather-entities" },
	{ "Environment Window/Add Weather", "",
	  "Creates the scene's Weather entity and selects it; its preset, intensity "
	  "and automatic cycle are then edited in the Details panel. Weather works "
	  "by writing cloud coverage, fog and wind into the Sky, so with no Sky in "
	  "the scene there is nothing for it to change.",
	  "", "scenes#sky-weather-entities" },
	{ "Environment Window/Remove##sky", "Remove Sky",
	  "Deletes the Sky entity and everything under it, the built-in sun and moon "
	  "lights included, leaving the scene with a flat background and no "
	  "sunlight. It is one undo step.",
	  "", "scenes#sky-weather-entities" },
	{ "Environment Window/Remove##weather", "Remove Weather",
	  "Deletes the Weather entity, so nothing drives the sky's clouds and fog "
	  "any more. The Sky itself is left untouched. It is one undo step.",
	  "", "scenes#sky-weather-entities" },

	// ── The export dialog ────────────────────────────────────────────────────
	// Every option here changes what ships, and several of them are irreversible
	// once a build is in someone's hands. Four of these sentences used to live in
	// a hand-written tooltip behind a "(?)" mark beside the checkbox; they are
	// entries now, so the whole checkbox is the hover target and F1 works.
	{ "Export/Save Profile", "",
	  "Writes the fields below back into the profile chosen in the dropdown and "
	  "makes it the project's active one. The profile lives in the .heproj "
	  "manifest, so it is saved with the project. It overwrites the selected "
	  "profile in place — use Save As to keep the old settings under a different "
	  "name.",
	  "", "export#profiles" },
	{ "Export/Save As", "",
	  "Saves the fields below under the name typed in the box to its left. A name "
	  "that already exists overwrites that profile rather than adding a second "
	  "one; otherwise a new profile is appended. Either way the result becomes "
	  "the selected and active profile.",
	  "", "export#profiles" },
	{ "Export/(currently open scene)", "Currently Open Scene",
	  "Boots the packaged game into whichever scene the editor has open at the "
	  "moment you export, instead of a fixed one. The scene is read from its file "
	  "on disk and not from the live editor world, so unsaved edits are not in "
	  "the build — save the scene first.",
	  "", "export#overview" },
	{ "Export/(platform default)", "Platform Default",
	  "Ships no graphics-backend choice at all, so the packaged game picks its "
	  "own default for whatever machine it starts on. That is a different answer "
	  "from naming a backend: an absent key lets the runtime decide, a named one "
	  "the target does not have only falls back after trying.",
	  "", "export#shipped-files" },
	{ "Export/VSync", "",
	  "Whether the shipped game waits for the display's refresh before presenting "
	  "a frame. This rides to the player in config.json beside the executable "
	  "rather than inside the archive, so it stays editable after the export.",
	  "", "export#shipped-files" },
	{ "Export/Compress assets", "",
	  "Compresses every packed asset on the way into the archive — zstd where "
	  "this build has it, LZ4 otherwise. Off stores the bytes as they are: a much "
	  "larger file that needs no decompression at load.",
	  "", "export#hpak" },
	{ "Export/Encrypt assets", "",
	  "Encrypts the packed archive so the shipped assets cannot simply be "
	  "unpacked. The key travels inside the build, which makes this a barrier "
	  "against casual copying rather than against a determined attacker — "
	  "managing anything stronger is the project's own job.",
	  "", "export#encryption" },
	{ "Export/Enable mod support", "",
	  "The shipped game scans a Mods/ folder next to its executable and mounts "
	  "every .hpak it finds as an overlay on the base archive. An entry with the "
	  "same UUID replaces the original and a new UUID is added, the packed "
	  "startup scene included.",
	  "", "export#mods" },
	{ "Export/Incremental packing", "",
	  "Reuses the stored bytes of unchanged assets from the previous export at "
	  "the same output directory instead of compressing them again, matched by a "
	  "hash kept in a sidecar file. It falls back to a full pack on its own "
	  "whenever that pak or sidecar is missing, does not match, or the packing "
	  "settings changed, so it cannot quietly ship a stale archive.",
	  "", "export#incremental" },
	{ "Export/Compile HorizonCode", "",
	  "Translates HorizonCode graphs — classes, widgets, level scripts and the "
	  "GameInstance — into C++ and ships them as a compiled library. It needs "
	  "cmake and a C++ toolchain on this machine and only runs for a Host target. "
	  "The graphs ship as well either way, so by default anything that fails to "
	  "translate simply runs interpreted.",
	  "", "horizoncode#compiler" },
	{ "Export/Interpret on failure", "",
	  "A graph that cannot be translated ships interpreted. The build always "
	  "succeeds, and compiled and interpreted objects work together.",
	  "", "horizoncode#compiler" },
	{ "Export/Stop on failure", "",
	  "A graph that cannot be translated FAILS the export instead, naming every "
	  "offending graph and node. Use this when the packaged build has to be fully "
	  "native.",
	  "", "horizoncode#compiler" },
	{ "Export/macOS .app bundle", "",
	  "Emits a signed .app instead of a flat folder: the executable and libraries "
	  "in Contents/MacOS, the pak and config in Contents/Resources, a generated "
	  "Info.plist, and an ad-hoc codesign of the whole bundle. It appears only "
	  "when this editor runs on macOS and the target is macOS or Host, because "
	  "building it needs codesign.",
	  "", "export#platforms" },
	{ "Export/Metal", "Precompile for Metal",
	  "Cross-compiles every node-graph material into the pak as Metal shaders, so "
	  "the shipped game never cross-compiles at load. Tick only the backends the "
	  "target actually runs; with none ticked the game compiles shaders on first "
	  "use instead.",
	  "", "materials#pipeline" },
	{ "Export/OpenGL", "Precompile for OpenGL",
	  "The same for OpenGL. Every backend ticked adds its own copy of every "
	  "material to the archive.",
	  "", "materials#pipeline" },
	{ "Export/Vulkan", "Precompile for Vulkan",
	  "The same for Vulkan, on the targets that run it.",
	  "", "materials#pipeline" },
	{ "Export/D3D11", "Precompile for Direct3D 11",
	  "The same for Direct3D 11, which is the Windows fallback path.",
	  "", "materials#pipeline" },
	{ "Export/D3D12", "Precompile for Direct3D 12",
	  "The same for Direct3D 12, the newer of the two Windows backends.",
	  "", "materials#pipeline" },
	{ "Export/Export", "",
	  "Starts the export with the settings above and hands over to the Build "
	  "window, which shows each step, its own progress and its log. The packing "
	  "runs on a worker thread; the Build window stays up for the whole run so "
	  "nothing changes the project's content underneath it. Disabled until an "
	  "output directory is set, and while a run is going.",
	  "", "export#overview" },

	// ── The Build window ─────────────────────────────────────────────────────
	// Not "Build": that scope is the menu bar's Build menu.
	{ "Build Window/Start Game", "",
	  "Launches the executable this export just produced. It runs as a separate "
	  "process and keeps running when you close the editor. Available only after "
	  "an export that succeeded and shipped a runtime this machine can run: an "
	  "export aimed at another platform, or one that shipped no game runtime at "
	  "all, leaves it greyed out.",
	  "", "export#shipped-files" },
	{ "Build Window/Build Again", "",
	  "Runs the same export once more with the settings the last run used, "
	  "reporting into this window. Unlike the other two buttons it leaves the "
	  "window open, because the next run reports here anyway.",
	  "", "export#overview" },
	{ "Build Window/Build Settings", "",
	  "Closes this window and reopens the export dialog on the same profile, so "
	  "the target, packing options or output directory can be changed before the "
	  "next run. The next export reports back into this window.",
	  "", "export#profiles" },

	// ── The profiler ─────────────────────────────────────────────────────────
	{ "Profiler/Target", "Target Frame Rate",
	  "The frame rate everything on this tab is judged against. It fixes the "
	  "budget in milliseconds — 60 FPS is 16.67 ms — which colours the FPS tile, "
	  "draws the budget line across the frame-time graph and scales the CPU and "
	  "GPU bars. It changes how the numbers are presented, never how the engine "
	  "runs.",
	  "", "editor#profiler" },
	{ "Profiler/GPU time graph", "",
	  "Adds a second graph under the frame-time one plotting GPU milliseconds per "
	  "frame, so a frame that is slow on the GPU can be told apart from one that "
	  "is slow on the CPU. It appears only when the frames on screen carry GPU "
	  "times at all.",
	  "", "editor#profiler" },
	{ "Profiler/Fit", "",
	  "Resets the timeline's zoom and pan so the whole capture fits the view "
	  "again. The way back after wheel-zooming into one span.",
	  "", "editor#profiler" },
	{ "Profiler/Back to live", "",
	  "Leaves the recorded capture you loaded and points all the tabs back at "
	  "this editor session. It also clears the frame picked for Frame Detail and "
	  "the timeline's zoom, because neither means anything against a different "
	  "set of frames.",
	  "", "editor#profiler" },
	{ "Profiler/Start Benchmark Capture  (F9)", "Start Benchmark Capture",
	  "Records every frame until you stop it, with vsync forced off so the frame "
	  "times show what a frame actually costs rather than what the display "
	  "allows. Only the newest frames are kept, so a capture left running cannot "
	  "grow without limit. Any capture file you had loaded is closed first.",
	  "F9", "editor#profiler" },
	{ "Profiler/Stop & Dump  (F9)", "Stop & Dump",
	  "Ends the running benchmark capture, writes it to a file in the dumps "
	  "folder, and puts back the vsync setting the capture switched off.",
	  "F9", "editor#profiler" },
	{ "Profiler/Capture Single Frame", "",
	  "Records exactly one frame in full — every CPU scope, the counters and the "
	  "per-pass GPU times — and shows it in the Frame Detail tab. No file is "
	  "written. It forces detailed GPU timing for that frame, which makes the "
	  "frame itself slow; that is the price of an exclusive per-pass breakdown.",
	  "", "editor#profiler" },
	// The label carries a real em dash, and the key has to carry the same
	// character rather than an escape: the coverage scan reads this file as text.
	{ "Profiler/Detailed GPU pass timing (serializes GPU — capture only)",
	  "Detailed GPU Pass Timing",
	  "Makes the backends that support it submit each render pass in its own "
	  "command buffer, so each pass is measured on its own and the per-pass "
	  "numbers add up. Those per-pass costs are a reliable ranking and an upper "
	  "bound rather than the real cost. It serialises the GPU while it is on, so "
	  "the frame times measured during such a capture are not numbers to quote "
	  "for a shipping build.",
	  "", "editor#profiler" },
	{ "Profiler/Per-thread timeline (worker lanes)", "Per-thread Timeline",
	  "Records scopes on every thread rather than the main one alone. This is "
	  "what fills the Timeline tab and what shows whether the job pool is "
	  "actually being fed — gaps on a worker lane are idle cores. It costs memory "
	  "during a capture.",
	  "", "editor#profiler" },
	{ "Profiler/Debug: shadow cascades (cascade-index tint)", "Shadow Cascade Debug",
	  "Tints lit surfaces by which shadow cascade they sample, to check where the "
	  "cascade splits land — cascade 0 should hug the camera. It is a rendering "
	  "debug view, not a measurement, and only some backends honour it. It is not "
	  "saved, so it is off again after a restart.",
	  "", "rendering#shadows" },
	{ "Profiler/Dump Now", "",
	  "Writes whatever has been recorded so far to a file in the dumps folder "
	  "without stopping a capture that is running. It does nothing when nothing "
	  "has been recorded yet. The path it wrote is logged to the Console.",
	  "", "editor#profiler" },
	{ "Profiler/Open Dumps Folder", "",
	  "Opens the folder the capture files are written to in the system file "
	  "browser. The path itself is printed on the line below the button; it sits "
	  "next to the engine log, and the engine creates it if it is not there yet.",
	  "", "advanced#diagnostics" },
	{ "Profiler/Open Capture...", "",
	  "Reads a dump written by an earlier run — this machine's or someone "
	  "else's — and shows it through these same tabs. A banner appears above them "
	  "naming the file, the backend and the resolution it was recorded at, so a "
	  "recording is never mistaken for the live session.",
	  "", "editor#profiler" },
	{ "Profiler/clear", "Clear Frame Selection",
	  "Drops the frame you picked — from the hitch list, or by clicking a spike "
	  "in the frame-time graph of a loaded capture. Frame Detail then falls back "
	  "to the last single-frame capture, or failing that to the last frame of the "
	  "benchmark.",
	  "", "editor#profiler" },

	// ── The animator state machine ───────────────────────────────────────────
	// A transition row is six abbreviations in a narrow column: From, To, Op,
	// Param, Thresh, Duration. Together they are the whole condition under which
	// a character changes what it is doing, which is more weight than six short
	// words can carry on their own.
	{ "State Machine/Add State", "",
	  "Adds a state at the point on the canvas where you opened the menu. It "
	  "starts with no clip: drop an Animation Clip from the Content Browser onto "
	  "the node's slot to give it one. With Start State left empty, the first "
	  "state in the list is the one an entity enters.",
	  "", "systems#animation" },
	{ "State Machine/Loop", "",
	  "Play this state's clip over and over instead of stopping at its end. Off, "
	  "the clip runs once and the pose holds on the last frame. Either way the "
	  "state does not leave by itself when the clip ends; only a transition moves "
	  "the machine on.",
	  "", "systems#animation" },
	{ "State Machine Transitions/From", "From State",
	  "The state this transition leaves, written as that state's name. Names, not "
	  "ids, are how a transition points at its endpoints, so a name matching no "
	  "state means the transition is never considered and no link is drawn for it "
	  "on the canvas. Renaming a state in the canvas rewrites this field for you.",
	  "", "systems#animation" },
	{ "State Machine Transitions/To", "To State",
	  "The state this transition enters, written as that state's name. A name "
	  "that matches no state still lets the transition fire: the crossfade blends "
	  "towards an empty pose, logs one error saying so, and leaves the machine "
	  "sitting in a state that does not exist — at which point the character "
	  "stops animating altogether.",
	  "", "systems#animation" },
	{ "State Machine Transitions/Op", "Comparison",
	  "How the parameter is held against the threshold: greater than, less than, "
	  "or equal to. Equal is an exact floating-point comparison, so it fits a "
	  "parameter something sets to that value outright and not one that arrives "
	  "there by arithmetic.",
	  "", "systems#animation" },
	{ "State Machine Transitions/Param", "Parameter",
	  "Which of the machine's parameters this transition watches, by name. It has "
	  "to be one the machine actually carries, either declared under Default "
	  "Params or written at run time. A transition whose parameter has no value "
	  "never fires.",
	  "", "systems#animation" },
	{ "State Machine Transitions/Thresh", "Threshold",
	  "The number the parameter is compared against. With the comparison it is "
	  "the whole condition — nothing about the clip or its length is consulted. "
	  "The machine still only looks at transitions leaving the state it is in, "
	  "and only when it is not already mid-crossfade.",
	  "", "systems#animation" },
	{ "State Machine Transitions/Duration", "Crossfade Duration",
	  "How long the blend into the new state lasts, in seconds. Both clips are "
	  "sampled and mixed for that long, which is what smooths a hard change of "
	  "pose; zero makes the swap immediate. While a crossfade is running the "
	  "machine considers no further transition.",
	  "", "systems#animation" },
	{ "State Machine Transitions/+ Transition", "Add Transition",
	  "Appends an empty transition to the list. It stays inert until you name a "
	  "state at each end and give it a parameter to watch — a row with no "
	  "parameter never fires. Dragging from one state's Out pin to another's In "
	  "pin on the canvas produces the same row with both ends already filled in.",
	  "", "systems#animation" },
	{ "State Machine Parameters/+ Param", "Add Parameter",
	  "Declares the parameter named in the field beside it and gives it a "
	  "starting value of 0. Every entity running this machine is seeded with "
	  "these defaults, which is what lets a transition read a parameter before "
	  "any script has written to it.",
	  "", "systems#animation" },
	{ "Sync Graph/Show node", "",
	  "Selects the node the compile check blamed and brings the canvas to it. It "
	  "appears only after a check that failed, beside the line saying this graph "
	  "will ship interpreted instead of translated to C++.",
	  "", "horizoncode#compiler" },

	// ── The audio and mesh tabs ──────────────────────────────────────────────
	{ "Audio Editor/Volume", "Preview Volume",
	  "How loud this tab plays the clip, from silent to twice the recorded level. "
	  "It moves a preview that is already running. Preview only: an Audio Source "
	  "component in the scene carries its own volume.",
	  "", "systems#audio" },
	{ "Audio Editor/Pitch", "Preview Pitch",
	  "Playback rate for the preview, from a quarter speed to double. Speed and "
	  "pitch move together, so raising it both shortens the clip and lifts it. "
	  "Preview only, like the volume above it.",
	  "", "systems#audio" },
	{ "Audio Editor/Import as Audio Asset", "",
	  "Turns the source .wav open in this tab into an asset the project can "
	  "reference, at the path printed under the button. It only appears for a raw "
	  ".wav, not for a clip that is already an asset. Engine content is read-only "
	  "unless the editor is in engine-content dev mode, so a .wav from the engine "
	  "library normally lands in the project's own content instead.",
	  "", "editor#asset-editors" },
	{ "Mesh Viewer/Sky", "Sky lighting",
	  "Lights the preview with the sky at a chosen hour, so the mesh can be "
	  "judged in the light it will actually stand in. The time slider below picks "
	  "the hour.",
	  "", "editor#asset-editors" },
	{ "Mesh Viewer/Studio", "Studio lighting",
	  "Lights the preview with one fixed key light and no colour cast — the shape "
	  "alone, without the sky's opinion about it. It is the same light every "
	  "asset thumbnail is rendered under.",
	  "", "editor#asset-editors" },
	{ "Mesh Viewer/Ground grid", "",
	  "Draws the ground plane, its grid and the origin marker under the mesh, "
	  "which is what tells the eye how big the model is and where its origin "
	  "sits. A view aid in this tab only; nothing about the asset changes.",
	  "", "editor#asset-editors" },
	{ "Mesh Viewer/Clip:", "Preview Clip",
	  "An animation clip to pose this skeleton with, dropped from the Content "
	  "Browser. Empty leaves it in its bind pose. It is preview state on this "
	  "tab, so it is not saved with the asset and pushes no undo step.",
	  "", "systems#animation" },

	// ── HorizonCode: the panels around the graph ─────────────────────────────
	// The node reference covers what each NODE does. This covers the panels that
	// surround the canvas — the graph list, the declared events, the variables,
	// the selected node's rows — and the two shared files every host draws
	// through, which is why some of these scopes are pushed in HcGraphHost and
	// HcEditorUtil rather than in a panel: four editors share those rows, and a
	// sentence under those scopes has to be true in all four.
	{ "Script Graph/Event Graph", "Event Graph",
	  "The graph that holds the event handlers. The functions listed under it are "
	  "sub-graphs of their own; this is the one that runs by itself when the host "
	  "raises an event.",
	  "", "horizoncode#graphs" },
	{ "Script Graph/Show node", "Show node",
	  "Jumps to the node the compile check stopped at: it opens that node's "
	  "sub-graph, selects it and scrolls it into view. The strip above says what "
	  "went wrong; this says where.",
	  "", "horizoncode#compiler" },
	{ "Script Graph/Get", "Get",
	  "Adds a node that READS the variable you dropped on the canvas, already "
	  "typed like the variable. Greyed out when that variable is local to another "
	  "function, because a function's locals do not exist outside it.",
	  "", "horizoncode#graphs" },
	{ "Script Graph/Set", "Set",
	  "Adds a node that WRITES it. Same rule as Get: a variable belonging to "
	  "another function cannot be reached from here.",
	  "", "horizoncode#graphs" },

	{ "HorizonCode Event/Name", "",
	  "What this declared event is called. Renaming it rewrites every Event, Emit "
	  "Event and Bind Event node that used the old name, so the two halves of a "
	  "binding cannot drift apart by a typo. A name another event already has, or "
	  "one the engine declares, is refused and the box snaps back.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Event/Carries a value", "",
	  "Gives the event one argument. Every Event and Emit Event node of this name "
	  "takes the declaration's shape, so an Emit that sent a number and a handler "
	  "that read text can no longer disagree. Changing that shape drops the wire "
	  "on the value pin.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Event/Delete Event", "Delete Event",
	  "Removes the declaration. Nodes that used the name are left exactly where "
	  "they are: this says the event is no longer part of the class's interface, "
	  "not that the graph should be rewritten.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Event/Event", "",
	  "Which declared event this node binds to or raises. Picking one copies the "
	  "declaration's argument shape onto the node, so its value pin always "
	  "matches. The list holds the events this graph declares; the box under it "
	  "declares a new one.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Event/Declare", "Declare",
	  "Declares the name typed beside it as a new event on this graph and points "
	  "the node at it. Greyed out while the box is empty, while the name is "
	  "already declared, or while it names an engine event — every class can "
	  "handle those and none declares them.",
	  "", "horizoncode#communication" },

	{ "Script Variable/Name", "",
	  "What this variable is called. The rename carries: every Get Variable and "
	  "Set Variable node using it is renamed with it, so the wiring survives. A "
	  "name this graph or a base class already uses is refused, private ones "
	  "included — an instance has one variable store, so the same name would be "
	  "that variable rather than a new one.",
	  "", "horizoncode#functions" },
	{ "Script Variable/Access", "",
	  "Public lets another graph reach the variable through a Get (Ref) or Set "
	  "(Ref) node on a reference to this object. Private keeps it to this graph. "
	  "A function-local has no access at all, which is why a \"Local to\" line "
	  "stands here instead for those.",
	  "", "horizoncode#functions" },
	{ "Script Variable/Position##vdef", "Default Position",
	  "The position this Transform variable starts at. It is a starting value, "
	  "not a binding to anything.",
	  "", "horizoncode#functions" },
	{ "Script Variable/Rotation##vdef", "Default Rotation",
	  "The rotation it starts at, in degrees per axis.",
	  "", "horizoncode#functions" },
	{ "Script Variable/Scale##vdef", "Default Scale",
	  "The scale it starts at. 1, 1, 1 is unscaled.",
	  "", "horizoncode#functions" },
	{ "Script Variable/Delete Variable", "Delete Variable",
	  "Removes the variable from this graph. Get Variable and Set Variable nodes "
	  "that used it are left where they are, still naming something no longer "
	  "declared — nothing is repaired for you.",
	  "", "horizoncode#functions" },

	{ "Script Node/Overridable", "",
	  "Lets a class derived from this one replace this event or function. It then "
	  "appears in the derived class's add menu as an override, and only the "
	  "derived version runs. The row shows only on a class asset — a level script "
	  "and the Game Instance are nobody's base class.",
	  "", "horizoncode#functions" },
	{ "Script Node/Event", "",
	  "Which event this handler answers. Where the graph has a fixed catalogue — "
	  "the level script's world events, the Game Instance's app events — it is "
	  "picked from the list; a class names its own instead. Two Event nodes may "
	  "not share a name, so one that is already handled is greyed out.",
	  "", "horizoncode#communication" },
	{ "Script Node/Name", "",
	  "What this function is called. Renaming it renames the Call and Return "
	  "nodes that named it, so the wiring survives. Calls resolve by name and "
	  "the first match wins, so a second function with the same name is dead "
	  "code that still looks live.",
	  "", "horizoncode#functions" },
	// The dialog raised after a rename that reaches beyond its own graph. Both
	// buttons are looked up under a fixed key: the first one's text counts the
	// hits, so it is built at run time and has no stable label to key on.
	{ "Rename Across Project/Rename in place", "Rename everywhere",
	  "Rewrites and SAVES the listed assets, so every call, read and handler goes "
	  "on naming the same member. Undo only reaches the graph you renamed it in.",
	  "", "horizoncode#functions" },
	{ "Rename Across Project/Leave the others alone", "Rename here only",
	  "Keeps the rename inside the graph you are editing. Everything listed goes "
	  "on naming the old one, which for a call means it now names nothing.",
	  "", "horizoncode#functions" },
	{ "Script Node/Access", "",
	  "Public functions can be called from outside the graph: from Lua and "
	  "Python, and through a Call Function (Ref) node on a reference to this "
	  "object. Private keeps the function inside the graph.",
	  "", "horizoncode#functions" },
	{ "Script Node/Function", "",
	  "Which of this graph's own functions the call runs. Only named functions "
	  "are offered — an unnamed one has no label to click. Picking one re-syncs "
	  "the call's pins with that function's inputs and results.",
	  "", "horizoncode#functions" },

	{ "HorizonCode Node/Value", "",
	  "The constant this literal node hands out. Nothing feeds it: it is typed "
	  "here and read by whatever its output pin is wired to. On an enum literal "
	  "the list holds the entries of the definition picked above it.",
	  "", "horizoncode#graphs" },
	{ "HorizonCode Node/Variable", "",
	  "Which variable the node reads or writes. Get Variable and Set Variable "
	  "choose from this graph's own variables plus the public ones a base class "
	  "brings, and a function-local only when the node itself sits in that "
	  "function's sub-graph; the node then takes that variable's type, and a wire "
	  "the pin can no longer carry is dropped. Get (Ref) and Set (Ref) name a "
	  "public variable on the object wired to Target instead, picked from the "
	  "Target Class above once that is known.",
	  "", "horizoncode#functions" },
	{ "HorizonCode Node/Function", "",
	  "Which public function to call on the object wired to Target. Once the "
	  "Target Class above is known the list holds that class's own functions, and "
	  "picking one mirrors its parameters and results onto the node. A name that "
	  "class does not have is shown as such rather than hidden, because that is "
	  "what a rename elsewhere leaves behind.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Node/Target Class", "",
	  "Which class the object wired to Target is expected to be. Left on \"From "
	  "the wire\" it is read off whatever produces the reference — a Create "
	  "Object, a Cast, a typed object variable, Get Self — which is one place to "
	  "change instead of two. Naming it here is for the cases the wire cannot "
	  "answer, and it is also what lets renaming a member of that class find this "
	  "node instead of leaving it pointing at a name nothing has.",
	  "", "horizoncode#communication" },
	{ "HorizonCode Node/Struct", "",
	  "Which struct definition this node is bound to. Rebinding re-mirrors the "
	  "node's pins to the new definition's fields and carries the links over "
	  "where it can, because the shape of the node IS the definition.",
	  "", "horizoncode#graphs" },
	{ "HorizonCode Node/Enum", "",
	  "Which enum definition this node is bound to. The value list below it comes "
	  "from that definition, so picking a different one re-offers a different set "
	  "of entries.",
	  "", "horizoncode#graphs" },
	{ "HorizonCode Node/Field", "",
	  "Which member of the struct above this node reads or writes. The node's "
	  "value pin takes that member's type.",
	  "", "horizoncode#graphs" },
	{ "HorizonCode Node/Widget", "",
	  "Which UI Widget asset to instantiate. The node outputs the new widget's "
	  "id, which is what the rest of the graph then talks to it by.",
	  "", "ui#runtime" },
	{ "HorizonCode Node/Class", "",
	  "Which HorizonCode class to instantiate as a live object. The node outputs "
	  "a reference to it. Wire Location and Rotation to place it; left unwired it "
	  "keeps the placement the class was authored with.",
	  "", "horizoncode#hosts" },
	{ "HorizonCode Node/Cast to", "",
	  "What to try to read the incoming reference as — one of the engine's own "
	  "kinds, or one of the project's HorizonCode classes. The cast succeeds only "
	  "when the object really is that thing or derives from it, and the output is "
	  "valid on the Success branch alone.",
	  "", "horizoncode#hosts" },
	{ "Function Return/Function", "Return of",
	  "Which of this graph's functions this Return node belongs to. Picking one "
	  "mirrors that function's results onto the node, so its input pins are the "
	  "values the function promises to hand back.",
	  "", "horizoncode#functions" },
	{ "Node Parameter/Field", "Save Field",
	  "Which field of the project's savegame template this call reads or writes. "
	  "The list holds only the fields whose type this particular call can carry.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Scene", "Scene",
	  "Which scene this call loads, picked from the project rather than typed, so "
	  "a renamed or moved scene cannot leave a path behind that no longer points "
	  "anywhere.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Animation", "Animation",
	  "Which of the widget's animations this call plays or asks about, picked "
	  "from the ones it actually has — its own and the ones its embedded "
	  "components brought with them. A typed name that does not match plays "
	  "nothing and says nothing, which is the whole reason this is a list.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Direction", "Direction",
	  "Which way the animation runs: forwards, backwards, or out and back. "
	  "Backwards reads the same animation from its end, so \"the way it came in, "
	  "in reverse\" is a setting rather than a second animation to maintain. "
	  "Ping Pong takes twice as long, and with Loop it is the shape every "
	  "breathing animation has.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Easing", "Easing",
	  "The curve the value travels along. Out curves land softly, In curves "
	  "leave softly, In Out does both, and Out Back overshoots and settles, "
	  "which is what makes a dialog land instead of arrive.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Element", "Element",
	  "Which element of this widget the call acts on, by the name it carries in "
	  "the Designer. Only named elements are offered: a name is what a graph can "
	  "hold on to, and an element without one is the asset's private business.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Property", "Property",
	  "Which property of the element named beside it. The list follows that "
	  "element, so picking another one changes what is on offer — and on an "
	  "Animate call it holds only what can be moved: a number, a colour, a "
	  "point.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/List", "List",
	  "Which list this call drives. Only ListView elements are offered, because "
	  "a Set List Count pointed at a button is a call that cannot work.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Parent", "Parent",
	  "Which element the new child is placed under, by name.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Function", "Function",
	  "Which function to call. The list holds this widget's own public "
	  "functions; a function on a widget somebody else made cannot be listed "
	  "from here, so that one is typed.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Mode", "Mode",
	  "Light, Dark, or System. The third is a rule and not a colour: an "
	  "application set to System follows the desktop, and follows it again when "
	  "the desktop changes while it is running.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Button", "Button",
	  "Which gamepad button, in the names the platform layer uses. Picked "
	  "rather than typed, because a name that does not match is a button that "
	  "is simply never down.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Axis", "Axis",
	  "Which gamepad axis, in the names the platform layer uses. A name that "
	  "does not match reads zero for ever, and says nothing.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Class", "Class",
	  "Which HorizonCode class to spawn, from the ones in the project.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/Asset", "Asset",
	  "Which asset this call uses, from the ones in the project. Picked rather "
	  "than typed: a moved or renamed asset would otherwise leave a path behind "
	  "that points nowhere, and fails silently.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/WidgetAsset", "Widget Asset",
	  "Which widget to instantiate, from the ones in the project.",
	  "", "horizoncode#palette" },
	{ "Node Parameter/ThemeAsset", "Theme Asset",
	  "Which theme to switch to, from the ones in the project.",
	  "", "horizoncode#palette" },
	{ "HorizonCode Node/Type", "",
	  "The type of the external variable this node reads or writes, so the node's "
	  "value pin matches the variable on the other side. Changing it drops the "
	  "wire on that pin.",
	  "", "horizoncode#communication" },

	{ "HorizonCode Default Value/Pos", "Position",
	  "The position part of this Transform default.",
	  "", "horizoncode#functions" },
	{ "HorizonCode Default Value/Rot", "Rotation",
	  "The rotation part, in degrees per axis.",
	  "", "horizoncode#functions" },
	{ "HorizonCode Default Value/Scl", "Scale",
	  "The scale part. 1, 1, 1 is unscaled.",
	  "", "horizoncode#functions" },
	{ "HorizonCode Default Value/Reset", "Reset",
	  "Drops the value set here and follows the struct's own default for this "
	  "field again. It appears only while the field is overridden.",
	  "", "horizoncode#functions" },
	{ "HorizonCode Default Value/+ Add Slot", "Add Slot",
	  "Appends one more element to the list that seeds this container when it is "
	  "created. A new slot starts at the element type's zero, which for a "
	  "Transform means scale 1. The length is not fixed: Array Append, Insert and "
	  "Remove grow and shrink it the same way at run time.",
	  "", "horizoncode#functions" },

	{ "Class Components/Add Child", "Add Child",
	  "Adds a child entity under the selected one, with a Transform of its own. A "
	  "class is a real subtree rather than a flat list — a character carrying a "
	  "camera has it as a child — so what is selected decides where the new "
	  "entity lands.",
	  "", "scenes#components" },

	// The struct, enum and savegame-template editor. No topic: the manual has no
	// chapter about project types yet, and a link to a near-miss is worse than
	// none.
	{ "Type Editor/+ Add Entry", "Add Entry",
	  "Adds one named constant to this enum. It takes the next free value — one "
	  "past the highest already in the list, never below 0 — and both the name "
	  "and the number can be changed afterwards. Two entries sharing a name are "
	  "flagged in red, because the generated constants would be ambiguous.",
	  "", "" },
	{ "Type Editor/+ Add Field", "Add Field",
	  "Adds one field to this struct or savegame template. Give it a type, "
	  "optionally a container, and a default. A duplicate field name is flagged "
	  "in red, and a struct whose fields lead back to itself refuses to save — "
	  "that cycle would never finish.",
	  "", "" },
	{ "Type Editor/Set as Project Default", "Set as Project Default",
	  "Makes this savegame template the one a script gets when it creates a save "
	  "without naming a template. It is stored in the project, so it applies "
	  "project-wide; the template that already holds the role shows a note here "
	  "instead of the button.",
	  "", "" },

	// ── The collaboration window ─────────────────────────────────────────────
	// "Collaboration Session" rather than "Collaboration": the shorter name is
	// the viewport's lock banner and already owns keys.
	{ "Collaboration Session/Display name", "",
	  "The name everyone else in the session sees, stored with your picture so it "
	  "is set once instead of typed on every join. The identity is read when a "
	  "session starts, so changing it during one takes effect at the next join.",
	  "", "collaboration#presence" },
	{ "Collaboration Session/Auto", "Automatic colour",
	  "Lets the host give you a colour that is still free, instead of picking one "
	  "yourself. It is the only choice that can never collide, and the colour is "
	  "what marks your camera, your selection and your locks for the others.",
	  "", "collaboration#presence" },
	{ "Collaboration Session/Custom", "Custom colour",
	  "Picks a colour of your own. Nothing here is a promise: the host settles "
	  "collisions, so if somebody already had the colour you asked for you are "
	  "given a free one and the panel says so.",
	  "", "collaboration#presence" },
	{ "Collaboration Session/Port", "",
	  "The port guests connect to when you host. 0 lets the system pick a free "
	  "one. This is the session itself; the announcements that put it in other "
	  "people's lists go over a separate socket of their own.",
	  "", "collaboration#discovery" },
	{ "Collaboration Session/Open session", "",
	  "Starts hosting on the port above. The editor opens the port, generates a "
	  "session ID and a join code, and publishes where it can be reached, so a "
	  "guest needs those two values and never an address. Your open scene becomes "
	  "the session's scene.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Announce this session on the local network", "",
	  "Announces that a session exists here, so people on the same network find "
	  "it in their list and join without an address. The join code is never "
	  "announced, so it still has to be given out. It is one switch for both "
	  "halves of discovery: the same setting also listens for other people's "
	  "sessions, whether or not you are hosting.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Look for sessions on this network", "",
	  "Listens for hosts announcing themselves nearby and lists what it hears: "
	  "who is hosting, which project, and how many people are in it. Picking a "
	  "row fills in the session ID for you; the join code is still typed, because "
	  "it is the only thing keeping strangers on this network out.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Join code", "",
	  "The code the host gives you, and the only thing keeping strangers out of a "
	  "session they can otherwise see. It is never announced on the network and "
	  "is not kept between runs, so it is typed for each join.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Session ID", "",
	  "The host's session ID. It is looked up in the session directory, which "
	  "turns it into an address, so no IP address and no port are needed.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Join this session", "",
	  "Joins the session picked above, at the address it announced rather than "
	  "through the directory — a router usually refuses to let a machine reach "
	  "its own network by its public address. Your open scene is replaced by the "
	  "host's, so save anything you want to keep first.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Join", "",
	  "Connects to the session named by the ID and the code above; both are "
	  "required. Your open scene is replaced by the host's, so save anything you "
	  "want to keep first.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Enable and join", "",
	  "Agrees to this session's larger assets — meshes, textures and audio — and "
	  "dials the same host again. If there is no join left to retry the setting "
	  "is turned on anyway and the panel says to join again by hand. It is "
	  "remembered and applies to every session from then on; it can be turned off "
	  "again in Preferences while you are not in a session, and it can use "
	  "considerably more data than an ordinary session.",
	  "", "collaboration#bigassets" },
	{ "Collaboration Session/Approve", "",
	  "Lets this delete or rename go through, for everyone including you. Nothing "
	  "happens to the file until you say so. A row that says \"Delete folder\" "
	  "removes everything underneath it, with no trash and no undo.",
	  "", "collaboration#assets" },
	{ "Collaboration Session/Approve all", "",
	  "Answers a whole bundle at once: the files somebody selected and deleted or "
	  "renamed in one action, which arrive as one decision rather than twenty. "
	  "Open the row first to answer them one by one — \"all but that one\" is a "
	  "real answer.",
	  "", "collaboration#assets" },
	{ "Collaboration Session/Hand over", "",
	  "Gives the asset to whoever asked for it. Your lock is released and theirs "
	  "granted in one step, so no third person can take it in between. You keep "
	  "what you have already done; what you have open on it simply stops "
	  "accepting edits.",
	  "", "collaboration#locks" },
	{ "Collaboration Session/Leave session", "",
	  "Disconnects you and puts the panel back to the host-or-join form. As the "
	  "host it ends the session for everyone: the announcements stop, and the "
	  "port forward and the directory entry are given back rather than left "
	  "standing for people to click on.",
	  "", "collaboration#starting" },
	{ "Collaboration Session/Dismiss", "",
	  "Clears the failure above and puts the panel back to the host-or-join form. "
	  "It retries nothing: the attempt it is reporting has already ended.",
	  "", "collaboration#starting" },

	{ "Session Participants/Block", "",
	  "Removes this person from the session and refuses them if they try to "
	  "rejoin. It asks first. The block lasts as long as this session — it does "
	  "not affect a session you open later, and it can be lifted again in the "
	  "blocked list below.",
	  "", "collaboration#overview" },
	{ "Session Participants/Allow", "",
	  "Lifts the block on someone you removed, so this session will have them "
	  "back. It does not reconnect them: they have to join again the way they did "
	  "the first time.",
	  "", "collaboration#overview" },
	{ "Block Participant/Block", "Block participant",
	  "Confirms it: the person is removed from the session, and a further attempt "
	  "to join is refused without you being asked again. The block lasts until "
	  "you close this session and can be lifted again from the participant list.",
	  "", "collaboration#overview" },

	// ── The Source Control window ────────────────────────────────────────────
	// "Source Control Panel", because "Source Control" is the Preferences page
	// that installs and configures git. Several of these carry no topic: the
	// shipped manual has no source-control chapter, and a link to a near-miss is
	// worse than none.
	{ "Source Control Panel/New branch…", "New branch",
	  "Opens the branch dialog, starting from the current state of the project "
	  "rather than from a commit in the history. It is unavailable while a git "
	  "command is running, before the first commit exists, and for a guest in a "
	  "collaboration session, where the host manages source control.",
	  "", "" },
	{ "Source Control Panel/Tree view", "",
	  "Shows the changed files as a folder tree, with single-child folders folded "
	  "into one row. The choice is remembered between sessions.",
	  "", "" },
	{ "Source Control Panel/Flat list", "",
	  "Shows every changed file as one row carrying its full path from the "
	  "project root, instead of as a folder tree. The choice is remembered "
	  "between sessions.",
	  "", "" },
	{ "Source Control Panel/Open source-control settings…", "Open source-control settings",
	  "Opens the Preferences page that owns the setup: initialising the "
	  "repository, the remote, the access token, auto-push and the fetch "
	  "schedule. This window only commits, pushes, pulls and shows the history.",
	  "", "editor#preferences" },
	{ "Source Control Panel/Set up in Preferences…", "Set up in Preferences",
	  "Opens the Preferences page where a project folder is turned into a git "
	  "repository and a remote is added. Until it is one, this window has nothing "
	  "to show.",
	  "", "editor#preferences" },
	{ "Source Control Panel/Create branch from this commit…", "Create branch from this commit",
	  "Opens the branch dialog with this commit as the starting point rather than "
	  "the current state. Creating a branch only writes a reference, so it stays "
	  "available with uncommitted changes; it is SWITCHING to the new branch that "
	  "needs a clean project.",
	  "", "" },
	{ "Source Control Panel/Restore project to this commit…", "Restore project to this commit",
	  "Puts every file in the project folder back to how it was at this commit: "
	  "files added since are removed, changed files are reverted, deleted files "
	  "come back. Your history is kept, because the restore is recorded as a new "
	  "commit — so it can be undone by restoring to a later one. It needs a clean "
	  "project and is refused while anything is uncommitted.",
	  "", "" },
	{ "Source Control Panel/Switch to it right away", "",
	  "Checks the new branch out as soon as it is created. It needs a clean "
	  "project: with uncommitted changes the branch is still created, but this is "
	  "switched off and you move to it once you have committed.",
	  "", "" },
	{ "sc.commit", "Commit",
	  "Records every changed file in one commit with the message above; the "
	  "button says how many that is. It stays unavailable without a message, with "
	  "nothing changed, and while a conflict is unresolved — a commit that keeps "
	  "conflict markers preserves the mess for good. With auto-push on, the "
	  "commit goes to the remote as it is made.",
	  "", "" },

	// ── The startup dialog for a missing git ─────────────────────────────────
	// Under the Preferences page's scope on purpose: the dialog and that page
	// state the same facts about the same machine, and its remedy buttons are
	// drawn by helpers the page calls too. "Save Identity" is already an entry
	// there and is deliberately not repeated here.
	{ "Source Control/Don't show this again", "",
	  "Stops this dialog appearing at startup. Nothing is installed or configured "
	  "by dismissing it: source control stays unavailable, and the Tools page in "
	  "Preferences still reports git, Git LFS and your git identity as missing.",
	  "", "editor#preferences" },
	{ "Source Control/Recheck", "",
	  "Looks for git and Git LFS again, without restarting the editor. A clean "
	  "result closes this dialog by itself, so this is what to press once an "
	  "install has finished elsewhere.",
	  "", "editor#preferences" },
	{ "Source Control/Copy 'xcode-select --install'", "Copy the Xcode tools command",
	  "Puts the command on the clipboard, to run in your own terminal. On macOS "
	  "git arrives with the Xcode Command Line Tools, so that one command is the "
	  "whole install.",
	  "", "editor#preferences" },
	{ "Source Control/git-scm.com", "git-scm.com",
	  "Opens git's own download page in a browser, for installing it by hand "
	  "instead of through a package manager.",
	  "", "editor#preferences" },
	{ "Source Control/Copy winget Command", "Copy the winget command",
	  "Puts a winget install command on the clipboard, to run in your own shell. "
	  "It is the command for whichever piece this bullet is about — git itself, "
	  "or Git LFS. After installing Git for Windows, restart the editor so it "
	  "picks up the new PATH.",
	  "", "editor#preferences" },
	{ "Source Control/Download Page", "Download Page",
	  "Opens the Git for Windows download page, which is where git comes from on "
	  "Windows.",
	  "", "editor#preferences" },
	{ "Source Control/Copy 'sudo apt install git'", "Copy the apt command",
	  "Puts the command on the clipboard, to run in your own terminal. The editor "
	  "does not install git for you on Linux; it only says what is missing.",
	  "", "editor#preferences" },
	{ "Source Control/Copy 'brew install git-lfs'", "Copy the brew command",
	  "Puts the command on the clipboard, to run in your own terminal. Press "
	  "Recheck afterwards rather than restarting.",
	  "", "editor#preferences" },
	{ "Source Control/git-lfs.com", "git-lfs.com",
	  "Opens the Git LFS home page, where it can be downloaded and installed by "
	  "hand.",
	  "", "editor#preferences" },
	{ "Source Control/Copy 'sudo apt install git-lfs'", "Copy the apt command for Git LFS",
	  "Puts the command on the clipboard, to run in your own terminal. Git LFS is "
	  "what keeps meshes, textures and audio out of the repository's own history, "
	  "so a clone does not drag down every version of every asset.",
	  "", "editor#preferences" },

	// ── The issue reporter ───────────────────────────────────────────────────
	{ "Report Issue/Attach the engine log", "",
	  "Adds recent log lines to the report. They travel inside the issue text, "
	  "and filing directly also uploads the complete log file as a secret gist "
	  "and links it. A browser link cannot carry a file, so on that route the "
	  "rest has to be dragged in by hand.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Warnings & errors", "",
	  "Attaches only the lines logged as a warning or an error. Few lines survive "
	  "the length limit of a browser link, so which ones they are matters more "
	  "than how many. It is the default whenever anything was logged as a problem "
	  "this run.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Everything", "",
	  "Attaches the tail of the whole log rather than the problems alone. It is "
	  "the right choice for a bug that never logged a word — wrong pixels, a "
	  "frozen gizmo — where a filtered log would be empty.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Show Log File", "",
	  "Opens the folder the engine log sits in, so the file can be dragged into "
	  "an issue in the browser. It is the complete log; what the report carries "
	  "is only the tail of it.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Copy Log Path", "",
	  "Puts the full path of the log file on the clipboard, for opening it in "
	  "something else or pasting it into a message.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Create a token", "",
	  "Opens GitHub's personal access token page. A token with issues and gist "
	  "access is what lets the editor file the report and upload the whole log "
	  "for you; it is used once and never stored.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/File on GitHub", "",
	  "Files the issue from here, under your own GitHub account. With the log "
	  "attached it also uploads the log file as a secret gist and links it. It "
	  "needs a title, because GitHub rejects an issue without one, and either an "
	  "account the credential helper already holds or a token in the field above.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Open in Browser", "",
	  "Opens the pre-filled issue form on GitHub instead of filing it from here, "
	  "so nothing is sent until you submit it there. It needs no account. A link "
	  "can only carry so much: if the report does not fit, the full text is put "
	  "on your clipboard to paste over the form.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Copy Report", "",
	  "Puts the whole report on the clipboard — the title, what you wrote, your "
	  "engine version and this machine, and the log lines you chose — for sending "
	  "it somewhere other than GitHub.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Open Issue", "",
	  "Opens the issue that has just been filed in your browser. Its address is "
	  "shown above and stays there until this dialog is closed.",
	  "", "advanced#diagnostics" },
	{ "Report Issue/Copy Link", "",
	  "Puts the address of the issue that has just been filed on the clipboard. "
	  "The report itself is already on GitHub; this is only the way back to it.",
	  "", "advanced#diagnostics" },

	// ── Windows and panels ───────────────────────────────────────────────────
	{ "panel.console", "Console",
	  "Everything the engine logged this session — warnings, errors, script "
	  "output. The first place to look when something did not happen.",
	  "Ctrl+`", "advanced#diagnostics" },
	{ "panel.profiler", "Performance Profiler",
	  "Where the frame time goes: a live CPU and GPU readout, and captures that "
	  "break a single frame down pass by pass.",
	  "F9", "editor#profiler" },
	{ "panel.environment", "Environment",
	  "Adds or removes the scene's Sky and Weather entities. Their settings then "
	  "live in the Details panel like any other component.",
	  "", "editor#environment-window" },
	{ "panel.collab", "Collaboration",
	  "Host or join a live editing session: several people in one scene, with "
	  "locks so two of you cannot edit the same thing at once.",
	  "", "collaboration#overview" },
	{ "panel.source-control", "Source Control",
	  "Git for the whole project, large assets included: what has changed, what to "
	  "commit, and what the rest of the team has pushed.",
	  "", "editor#layout" },
	{ "panel.notifications", "Notifications",
	  "Things that happened without you doing them — a failed sync, a peer's "
	  "change that could not be applied. It keeps them until you have read them.",
	  "", "editor#notifications" },
	{ "panel.quick-settings", "Quick Settings",
	  "The engine settings you pinned, at hand. In Landscape mode this becomes the "
	  "terrain brush instead.",
	  "", "editor#layout" },
	{ "panel.play-report", "Play Session Report",
	  "What happened during the play session that just ended: how long it ran, "
	  "what it logged, what went wrong.",
	  "", "editor#play-mode" },

	// ── Build and export ─────────────────────────────────────────────────────
	{ "export.profile", "Export Profile",
	  "A saved set of export settings — platform, packing, encryption — so a "
	  "release build is one click and always the same one.",
	  "", "export#profiles" },
	{ "export.platform", "Target Platform",
	  "Which system the packaged game is built for. Cross-building needs that "
	  "platform's toolchain installed.",
	  "", "export#platforms" },
	{ "export.encryption", "Encryption",
	  "Encrypts the packed archive so shipped assets cannot simply be unpacked.",
	  "", "export#encryption" },
	{ "export.incremental", "Incremental Packing",
	  "Repacks only what changed since the last export, which turns a coffee break "
	  "back into a few seconds.",
	  "", "export#incremental" },
	};

	// ── Topics that are about a PANEL ────────────────────────────────────────
	// Only real windows: a topic about a concept has nowhere to point, and a
	// "Show me" that opens the wrong thing is worse than none. The window names
	// are the ImGui ones — the stable id after "###" where a panel has one.
	constexpr PanelTopic kPanelTopics[] = {
		{ "editor#viewport",           "Scene",               "" },
		{ "editor#play-mode",          "Scene",               "" },
		{ "editor#landscape-mode",     "Quick Settings",      "" },
		{ "editor#outliner",           "World Outliner",      "" },
		{ "editor#details",            "Details",             "" },
		{ "editor#content-browser",    "Content Browser",     "" },
		{ "editor#engine-content",     "Content Browser",     "" },
		{ "editor#profiler",           "Performance Profiler","View » Performance Profiler" },
		{ "editor#environment-window", "Environment",         "View » Environment" },
		{ "editor#layout",             "Scene",               "" },
		{ "advanced#diagnostics",      "Console",             "View » Console" },
		{ "collaboration#overview",    "Collaboration",       "View » Collaboration" },
		{ "collaboration#starting",    "Collaboration",       "View » Collaboration" },
		{ "scenes#terrain",            "Quick Settings",      "" },
		{ "scenes#components",         "Details",             "" },
		{ "systems#physics",           "Details",             "" },
	};

	// ── Which page of the reference an entry lands on ────────────────────────
	// Longest prefix wins. The component scopes all fold into one page — a
	// reader looking up "Mass" is in the Details panel whichever component it
	// belongs to, and the component's own name becomes the group inside the
	// page, which is exactly how the panel is arranged.
	constexpr Area kAreas[] = {
		// ── Interface ────────────────────────────────────────────────────────
		{ "viewport.", "editor-interface", "Editor Interface", "Scene viewport" },
		{ "outliner.", "editor-interface", "Editor Interface", "World Outliner" },
		{ "content.",  "editor-interface", "Editor Interface", "Content Browser" },
		{ "details.",  "editor-interface", "Editor Interface", "Details panel" },
		{ "panel.",    "editor-interface", "Editor Interface", "Panels and windows" },
		{ "hub.",      "editor-interface", "Editor Interface", "Project Hub" },
		// The menu bar, one rule per menu — the scope a menu pushes IS its name,
		// so an entry is keyed "File/Save All" and needs nothing at the call
		// site but the wrapper.
		{ "File/",   "editor-interface", "Editor Interface", "File menu" },
		{ "Edit/",   "editor-interface", "Editor Interface", "Edit menu" },
		{ "View/",   "editor-interface", "Editor Interface", "View menu" },
		{ "Assets/", "editor-interface", "Editor Interface", "Assets menu" },
		{ "Build/",  "editor-interface", "Editor Interface", "Build menu" },
		{ "Help/",   "editor-interface", "Editor Interface", "Help menu" },
		// The panels whose controls are looked up by label within the panel.
		{ "World Outliner/",   "editor-interface", "Editor Interface", "World Outliner" },
		{ "Content Browser/",  "editor-interface", "Editor Interface", "Content Browser" },
		{ "New Asset/",        "editor-interface", "Editor Interface", "Creating assets" },
		{ "Console/",          "editor-interface", "Editor Interface", "Console" },
		{ "Notifications/",    "editor-interface", "Editor Interface", "Notifications" },
		{ "Play Report/",      "editor-interface", "Editor Interface", "Play Session Report" },
		{ "Project Hub/",      "editor-interface", "Editor Interface", "Project Hub" },
		{ "Documentation/",    "editor-interface", "Editor Interface", "Documentation reader" },
		{ "Tutorial/",         "editor-interface", "Editor Interface", "Interactive tutorial" },
		{ "Collaboration/",    "editor-interface", "Editor Interface", "Collaboration" },
		{ "Source Root/",      "editor-interface", "Editor Interface", "Source root" },
		{ "New Entity/",       "editor-interface", "Editor Interface", "Creating entities" },
		{ "Viewport Options/", "editor-interface", "Editor Interface", "Viewport options" },
		// ── The Details panel's components ───────────────────────────────────
		{ "Component/", "editor-components", "Component Reference", "The components" },
		// ── Settings ─────────────────────────────────────────────────────────
		// One rule per category of the Preferences window, so the reference page
		// is split the way the window is: the group heading a reader sees here is
		// the SeparatorText they see there. The plain "Preferences/" rule stays
		// underneath as the fallback — longest prefix wins, so a categorised key
		// takes its category's rule and anything else (the footer's Restore
		// Defaults) still lands on the page.
		{ "Preferences/Display/",             "editor-settings", "Settings Reference", "Display" },
		{ "Preferences/Post-Processing/",     "editor-settings", "Settings Reference", "Post-Processing" },
		{ "Preferences/Global Illumination/", "editor-settings", "Settings Reference", "Global Illumination" },
		{ "Preferences/Effects/",             "editor-settings", "Settings Reference", "Effects" },
		{ "Preferences/Collaboration/",       "editor-settings", "Settings Reference", "Collaboration" },
		{ "Preferences/Viewport/",            "editor-settings", "Settings Reference", "Viewport" },
		{ "Preferences/Input/",               "editor-settings", "Settings Reference", "Input" },
		{ "Preferences/Appearance/",          "editor-settings", "Settings Reference", "Appearance" },
		{ "Preferences/Content Browser/",     "editor-settings", "Settings Reference", "Content Browser" },
		{ "Preferences/",    "editor-settings", "Settings Reference", "Preferences" },
		{ "settings.",       "editor-settings", "Settings Reference", "Preferences" },
		{ "Source Control/", "editor-settings", "Settings Reference", "Source control setup" },
		{ "Tool Status/",    "editor-settings", "Settings Reference", "Tool status" },
		// The palette's element types get their own section of the UI chapter:
		// "what can I put on a page" is the first question somebody has, and it
		// deserves a list rather than being scattered through the designer's
		// controls.
		{ "UI Palette/",     "editor-ui", "UI Designer", "The elements" },
		// The pages on the Preferences tab that edit the PROJECT rather than the
		// editor, which is why they get their own sections rather than sitting
		// under "Preferences".
		{ "Permissions/",    "editor-settings", "Settings Reference", "Project permissions" },
		{ "Fonts/",          "editor-settings", "Settings Reference", "Project fonts" },
		{ "Build Tools/",     "editor-settings", "Settings Reference", "Build tools" },
		{ "Graph Appearance/", "editor-settings", "Settings Reference", "Graph appearance" },
		// ── The asset editors ────────────────────────────────────────────────
		{ "material.",           "editor-materials", "Material Editor", "Material graph" },
		{ "Material Node/",      "editor-materials", "Material Editor", "Values on a node" },
		{ "Material Graph/",     "editor-materials", "Material Editor", "The graph canvas" },
		{ "Material Parameter/", "editor-materials", "Material Editor", "Parameters" },
		{ "Material Settings/",  "editor-materials", "Material Editor", "Material settings" },
		{ "Material Preview/",   "editor-materials", "Material Editor", "The preview" },
		{ "ui.",             "editor-ui", "UI Designer", "Widget designer" },
		{ "UI Hierarchy/",   "editor-ui", "UI Designer", "The hierarchy" },
		{ "Canvas/",         "editor-ui", "UI Designer", "The canvas" },
		{ "UI Widget/",      "editor-ui", "UI Designer", "Widget properties" },
		{ "UI Graph/",       "editor-ui", "UI Designer", "Widget logic" },
		{ "UI Theme Preview/", "editor-ui", "UI Designer", "Previewing a theme" },
		{ "UI Timeline/",    "editor-ui", "UI Designer", "The timeline" },
		{ "UI Graph Node/",  "editor-ui", "UI Designer", "Nodes in the graph" },
		{ "UI Variable/",    "editor-ui", "UI Designer", "Graph variables" },
		{ "input.",         "editor-input", "Input Reference", "Input assets" },
		{ "Input Action/",  "editor-input", "Input Reference", "Actions and bindings" },
		{ "hc.",                         "editor-horizoncode", "HorizonCode Editor", "Graph editing" },
		{ "Script Graph/",               "editor-horizoncode", "HorizonCode Editor", "Script graphs" },
		{ "Script Variable/",            "editor-horizoncode", "HorizonCode Editor", "Graph variables" },
		{ "Script Node/",                "editor-horizoncode", "HorizonCode Editor", "Nodes in a script graph" },
		{ "HorizonCode Event/",          "editor-horizoncode", "HorizonCode Editor", "Declared events" },
		{ "HorizonCode Node/",           "editor-horizoncode", "HorizonCode Editor", "Nodes in any graph" },
		{ "HorizonCode Default Value/",  "editor-horizoncode", "HorizonCode Editor", "Default values" },
		{ "Function Return/",            "editor-horizoncode", "HorizonCode Editor", "Nodes in any graph" },
		{ "Rename Across Project/",      "editor-horizoncode", "HorizonCode Editor", "Renaming a member" },
		{ "Node Parameter/",             "editor-horizoncode", "HorizonCode Editor", "Nodes in any graph" },
		{ "Class Components/",           "editor-horizoncode", "HorizonCode Editor", "Class components" },
		{ "Type Editor/",                "editor-horizoncode", "HorizonCode Editor", "Struct, enum and savegame types" },
		{ "Theme Editor/",               "editor-ui", "UI Designer", "Theme" },
		{ "Theme Styles/",               "editor-ui", "UI Designer", "Theme styles" },
		{ "terrain.",             "editor-landscape", "Landscape Tools", "Terrain brush" },
		{ "env.",                 "editor-landscape", "Landscape Tools", "Environment window" },
		{ "New Landscape/",       "editor-landscape", "Landscape Tools", "Creating a landscape" },
		{ "Landscape/",           "editor-landscape", "Landscape Tools", "Sculpting and painting" },
		{ "Environment Window/",  "editor-landscape", "Landscape Tools", "Environment window" },
		{ "anim.",                      "editor-animation", "Animation Editors", "Animator" },
		{ "State Machine/",             "editor-animation", "Animation Editors", "State machine" },
		{ "State Machine Transitions/", "editor-animation", "Animation Editors", "Transitions" },
		{ "State Machine Parameters/",  "editor-animation", "Animation Editors", "Parameters" },
		{ "Sync Graph/",                "editor-animation", "Animation Editors", "Sync graph" },
		{ "Audio Editor/",              "editor-animation", "Animation Editors", "Audio editor" },
		{ "Mesh Viewer/",               "editor-animation", "Animation Editors", "Mesh viewer" },
		// ── Build, diagnose, collaborate ─────────────────────────────────────
		{ "export.",       "editor-export", "Export & Diagnostics", "Export" },
		{ "profiler.",     "editor-export", "Export & Diagnostics", "Profiler" },
		{ "Export/",       "editor-export", "Export & Diagnostics", "Export dialog" },
		{ "Build Window/", "editor-export", "Export & Diagnostics", "Build window" },
		{ "Profiler/",     "editor-export", "Export & Diagnostics", "Profiler" },
		{ "collab.",   "editor-collab",    "Collaboration & Source Control", "Collaboration" },
		{ "sc.",                    "editor-collab", "Collaboration & Source Control", "Source control" },
		{ "Collaboration Session/", "editor-collab", "Collaboration & Source Control", "Collaboration window" },
		{ "Session Participants/",  "editor-collab", "Collaboration & Source Control", "Participants" },
		{ "Block Participant/",     "editor-collab", "Collaboration & Source Control", "Participants" },
		{ "Source Control Panel/",  "editor-collab", "Collaboration & Source Control", "Source control" },
		// The bug reporter is neither collaboration nor source control: it
		// attaches the engine log and files an issue, which is diagnostics.
		{ "Report Issue/",          "editor-export", "Export & Diagnostics", "Report an issue" },
	};

	// Every component scope maps to the component page, with the component as
	// the group. Listed rather than pattern-matched: a scope is a display name,
	// and "does this string contain a slash" is not a statement about the
	// editor.
	constexpr const char* kComponentScopes[] = {
		"Transform", "Transform 2D", "Mesh", "Skeletal Mesh", "Material", "Light",
		"Decal", "Rigid Body", "Collider", "Character Controller", "Movement",
		"Camera", "Camera Rig", "Script", "Terrain", "Foliage", "Nav Mesh",
		"Nav Agent", "Audio Source", "Audio Listener", "Animator", "Animator Blend",
		"Animator State Machine", "Property Animator", "Particle System",
		"Save State", "LOD", "Environment", "Weather", "UI Canvas", "UI Element",
		"UI Text", "UI Image", "UI Button",
	};

	// The current section, as a stack. A vector rather than one string because
	// panels nest (a component inside the Details panel inside a tab) and an
	// unwound scope has to restore what was there, not clear it.
	std::vector<const char*>& scopes()
	{
		static std::vector<const char*> s;
		return s;
	}

	// The visible part of an ImGui label: everything before "##".
	std::string_view visible(std::string_view label)
	{
		const std::size_t hash = label.find("##");
		return hash == std::string_view::npos ? label : label.substr(0, hash);
	}
} // namespace

const Entry* findKey(std::string_view key)
{
	if (key.empty()) return nullptr;
	for (const Entry& e : kEntries)
		if (key == e.key) return &e;
	return nullptr;
}

const Entry* find(std::string_view label)
{
	if (label.empty()) return nullptr;

	// Most specific first: the label EXACTLY as written, "##" suffix included.
	// That suffix is the only thing telling two rows apart when a component has
	// a "Density" for its clouds and another for its fog.
	const std::vector<const char*>& stack = scopes();
	if (!stack.empty() && stack.back() && stack.back()[0])
	{
		std::string scoped = std::string(stack.back()) + "/";
		if (const Entry* e = findKey(scoped + std::string(label))) return e;
		if (const Entry* e = findKey(scoped + std::string(visible(label)))) return e;
	}
	if (const Entry* e = findKey(label)) return e;
	return findKey(visible(label));
}

Scope::Scope(const char* name) { scopes().push_back(name ? name : ""); }
Scope::~Scope() { if (!scopes().empty()) scopes().pop_back(); }

void setScope(const char* name)
{
	// Silently ignored with no Scope open. That is deliberate: the Details panel
	// also runs this path in its two SILENT modes (collect the component names,
	// remove one) where nothing is drawn and no scope was pushed, and neither
	// wants a special case at the call site.
	if (!scopes().empty()) scopes().back() = name ? name : "";
}

const char* currentScope()
{
	const std::vector<const char*>& s = scopes();
	return (s.empty() || !s.back()) ? "" : s.back();
}

std::string shortcutLabel(std::string_view shortcut)
{
	std::string out(shortcut);
#ifdef __APPLE__
	// The editor accepts Ctrl and Cmd interchangeably everywhere (KeyCtrl ||
	// KeySuper), so this is not a different binding — it is the key a Mac
	// keyboard actually has under the user's thumb.
	for (std::size_t p = out.find("Ctrl"); p != std::string::npos;
	     p = out.find("Ctrl", p + 3))
		out.replace(p, 4, "Cmd");
#endif
	return out;
}

const PanelTopic* panelForTopic(std::string_view topic)
{
	if (topic.empty()) return nullptr;
	for (const PanelTopic& p : kPanelTopics)
		if (topic == p.topic) return &p;

	// A section with no mapping of its own falls back to its page's, so a topic
	// somewhere in the editor manual still points at the editor.
	const std::size_t hash = topic.find('#');
	if (hash == std::string_view::npos) return nullptr;
	const std::string_view page = topic.substr(0, hash);
	for (const PanelTopic& p : kPanelTopics)
	{
		const std::string_view t(p.topic);
		const std::size_t h = t.find('#');
		if (h != std::string_view::npos && t.substr(0, h) == page && t.substr(h + 1) == page)
			return &p;
	}
	return nullptr;
}

const Area* areaOf(std::string_view key)
{
	// Longest matching prefix, so a specific rule can sit in front of a general
	// one without depending on the order they were written in.
	const Area* best = nullptr;
	std::size_t bestLen = 0;
	for (const Area& a : kAreas)
	{
		const std::string_view p(a.prefix);
		if (key.size() >= p.size() && key.substr(0, p.size()) == p && p.size() > bestLen)
		{
			best = &a;
			bestLen = p.size();
		}
	}
	if (best) return best;

	// A component's own rows ("Rigid Body/Mass"): the scope names the component,
	// which is both the page's group and how the Details panel is arranged.
	//
	// The rows are built ONCE, in full, and never appended to afterwards — the
	// first version grew the vector per component and handed out pointers into
	// it, so the next component to be looked up reallocated the storage and
	// every pointer returned so far became a dangling one. It crashed in the
	// test that walks all of them, which is the only place that looks up enough
	// of them to reallocate.
	static const std::vector<Area> s_components = [] {
		std::vector<Area> v;
		v.reserve(std::size(kComponentScopes));
		for (const char* c : kComponentScopes)
			v.push_back({ c, "editor-components", "Component Reference", c });
		return v;
	}();

	const std::size_t slash = key.find('/');
	if (slash != std::string_view::npos)
	{
		const std::string_view scope = key.substr(0, slash);
		for (const Area& a : s_components)
			if (scope == a.group) return &a;
	}
	return nullptr;
}

std::string referenceTopic(std::string_view key)
{
	const Area* a = areaOf(key);
	if (!a) return {};
	// The key IS the anchor: it is already unique, already stable, and already
	// what the call site names. Only the slash has to go — it separates page
	// from section in a topic.
	std::string anchor(key);
	std::replace(anchor.begin(), anchor.end(), '/', '.');
	return std::string(a->page) + "#" + anchor;
}

int          entryCount()          { return static_cast<int>(std::size(kEntries)); }
const Entry& entryAt(int i)        { return kEntries[i]; }
int          areaCount()           { return static_cast<int>(std::size(kAreas)); }
const Area&  areaAt(int i)         { return kAreas[i]; }
int          panelTopicCount()     { return static_cast<int>(std::size(kPanelTopics)); }
const PanelTopic& panelTopicAt(int i) { return kPanelTopics[i]; }

} // namespace HE::Ed::Help
