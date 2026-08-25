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
	  "Moves the entity along a path on the nav mesh toward a target position.",
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
	  "Box, Sphere or Capsule. A capsule stands along Y and is what a character "
	  "usually wants; the sides let it slide past corners instead of catching.",
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
	{ "Character Controller/Velocity", "",
	  "The character's current speed, written by the physics step. Editable here "
	  "for testing — normally gameplay code sets it.",
	  "", "systems#physics" },
	{ "Character Controller/Is Grounded", "",
	  "Whether the controller is standing on something right now. Read-only "
	  "state, shown so a jump that never fires can be diagnosed.",
	  "", "systems#physics" },

	// ── Movement / Camera / Camera Rig ───────────────────────────────────────
	{ "Movement/Max Speed", "",
	  "Top speed in metres per second at full input.",
	  "", "systems#animation" },
	{ "Movement/Turn Rate", "",
	  "How fast the character turns to face where it is going, in degrees per "
	  "second. Only used with Orient To Movement.",
	  "", "systems#animation" },
	{ "Movement/Orient To Movement", "",
	  "Turn the character to face the direction it walks. Switch it off when "
	  "something else owns the facing — a camera rig with coupled rotation.",
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
	  "Where the agent is walking to. Set it and a path is found on the next tick.",
	  "", "systems#navigation" },
	{ "Nav Agent/Speed", "", "How fast the agent follows its path, in metres per second.",
	  "", "systems#navigation" },
	{ "Nav Agent/Stop Dist", "",
	  "How close to the target counts as arrived. Too small and the agent circles "
	  "its destination forever.",
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
	{ "Weather/Rain", "",
	  "Rain strength for the state being shown, 0 to 1.", "", "rendering#weather" },
	{ "Weather/Snow", "",
	  "Snow strength for the state being shown, 0 to 1.", "", "rendering#weather" },
	{ "Weather/Wetness", "",
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

	// ── The viewport and its toolbar ─────────────────────────────────────────
	{ "viewport.play", "Play",
	  "Runs the scene in the viewport: scripts start, physics ticks and the game's "
	  "own camera takes over. Stopping restores the scene exactly as it was — "
	  "changes made while playing are not kept.",
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
	{ "outliner.add", "Add Entity",
	  "Creates an empty entity in the scene. Everything else — a mesh, a light, a "
	  "camera — is a component you then add to it in the Details panel.",
	  "", "editor#outliner" },
	{ "outliner.search", "Filter",
	  "Narrows the tree to entities whose name contains what you type. The "
	  "hierarchy around a match stays visible so you can see where it sits.",
	  "", "editor#outliner" },
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
	{ "settings.render-path", "Render Path",
	  "Forward shades each object as it is drawn. Deferred shades the whole screen "
	  "at once, which is what lifts the light count and enables SSAO and SSR.",
	  "", "rendering#pipeline" },
	{ "settings.backend", "Graphics Backend",
	  "Which graphics API the editor renders through. Changing it takes effect on "
	  "the next start.",
	  "", "rendering#backends" },
	{ "settings.vsync", "VSync",
	  "Waits for the display before showing a frame: no tearing, and the frame "
	  "rate is capped to the monitor. Off is for measuring performance.",
	  "", "rendering#performance" },
	{ "settings.aa", "Anti-Aliasing",
	  "How the jagged edges are smoothed. SMAA is a cheap single pass; TAA is "
	  "steadier in motion but needs the deferred path.",
	  "", "rendering#postfx" },
	{ "settings.render-scale", "Render Scale",
	  "Renders the 3D view at a fraction of the window size and upscales it. The "
	  "single most effective performance dial there is.",
	  "", "rendering#performance" },
	{ "settings.bloom", "Bloom",
	  "Lets bright areas bleed light into what is around them.",
	  "", "rendering#postfx" },
	{ "settings.ssao", "Ambient Occlusion",
	  "Darkens the creases and contact points that ambient light does not reach. "
	  "It is what stops objects looking as if they float.",
	  "", "rendering#postfx" },
	{ "settings.gi", "Global Illumination",
	  "Ray-traced bounce light: colour carried from lit surfaces into shadow. "
	  "Needs a ray-tracing capable GPU.",
	  "", "rendering#lighting" },
	{ "settings.ssr", "Screen-Space Reflections",
	  "Reflects what is already on screen in wet and polished surfaces. What is "
	  "off screen cannot be reflected — that is the method's limit.",
	  "", "rendering#postfx" },
	{ "settings.shadows", "Shadow Quality",
	  "The resolution of the shadow maps, and how far from the camera shadows are "
	  "still drawn.",
	  "", "rendering#shadows" },
	{ "settings.max-fps", "Frame Cap",
	  "Upper limit on frames per second with VSync off. A cap keeps the laptop "
	  "quiet without giving up responsiveness. 0 is unlimited.",
	  "", "rendering#performance" },
	{ "settings.camera-speed", "Editor Camera Speed",
	  "The starting speed of the editor's fly camera. The viewport toolbar's speed "
	  "field changes the same value.",
	  "", "editor#viewport" },
	{ "settings.pointer", "Pointer Input",
	  "Whether the preview panes expect a mouse or a trackpad. On a trackpad a "
	  "two-finger swipe steers and zoom moves to Cmd-scroll.",
	  "", "editor#preferences" },

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
		{ "editor#profiler",           "Performance Profiler","View ▸ Performance Profiler" },
		{ "editor#environment-window", "Environment",         "View ▸ Environment" },
		{ "editor#layout",             "Scene",               "" },
		{ "advanced#diagnostics",      "Console",             "View ▸ Console" },
		{ "collaboration#overview",    "Collaboration",       "View ▸ Collaboration" },
		{ "collaboration#starting",    "Collaboration",       "View ▸ Collaboration" },
		{ "scenes#terrain",            "Quick Settings",      "" },
		{ "scenes#components",         "Details",             "" },
		{ "systems#physics",           "Details",             "" },
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

int          entryCount()          { return static_cast<int>(std::size(kEntries)); }
const Entry& entryAt(int i)        { return kEntries[i]; }
int          panelTopicCount()     { return static_cast<int>(std::size(kPanelTopics)); }
const PanelTopic& panelTopicAt(int i) { return kPanelTopics[i]; }

} // namespace HE::Ed::Help
