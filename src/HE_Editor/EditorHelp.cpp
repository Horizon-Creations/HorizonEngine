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
	  "check a nav mesh without entering play mode.",
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
	{ "Preferences/Render Path", "",
	  "Forward shades each object as it is drawn. Deferred shades the whole screen "
	  "at once, which is what lifts the light count and turns on SSAO and SSR.",
	  "", "rendering#pipeline" },
	{ "Preferences/VSync", "",
	  "Waits for the display before showing a frame: no tearing, and the frame "
	  "rate is capped to the monitor's. Off is for measuring performance.",
	  "", "rendering#performance" },
	{ "Preferences/Max FPS (VSync off)", "",
	  "Upper limit on frames per second once VSync is off. A cap keeps a laptop "
	  "quiet without giving up responsiveness. 0 is unlimited.",
	  "", "rendering#performance" },
	{ "Preferences/Anti-Aliasing", "",
	  "How jagged edges are smoothed. SMAA is one cheap pass; TAA is steadier in "
	  "motion but needs the deferred path.",
	  "", "rendering#postfx" },
	{ "Preferences/AA Sharpness", "",
	  "How much detail is pulled back after the anti-aliasing pass softened it. "
	  "Too much re-introduces the edges it just removed.",
	  "", "rendering#postfx" },
	{ "Preferences/Render Scale", "",
	  "Renders the 3D view at a fraction of the window size and upscales it. The "
	  "single most effective performance dial there is; 0.75 is often invisible.",
	  "", "rendering#performance" },
	{ "Preferences/Specular AA", "",
	  "Tames the sparkle that fine, shiny detail produces in motion, by widening "
	  "the highlight where the surface curves too fast for the pixel.",
	  "", "rendering#postfx" },
	{ "Preferences/Specular AA Strength", "",
	  "How aggressively that is done. Too high and polished surfaces go dull.",
	  "", "rendering#postfx" },
	{ "Preferences/Bloom", "",
	  "Lets bright areas bleed light into what surrounds them, the way a camera "
	  "does.",
	  "", "rendering#postfx" },
	{ "Preferences/Bloom Threshold", "",
	  "How bright a pixel has to be before it blooms. Low values make the whole "
	  "image glow, which is rarely what is wanted.",
	  "", "rendering#postfx" },
	{ "Preferences/Bloom Intensity", "",
	  "How much of the bloom is added back on top of the image.",
	  "", "rendering#postfx" },
	{ "Preferences/AO", "",
	  "Ambient occlusion: darkens the creases and contact points that ambient "
	  "light cannot reach. It is what stops objects looking as if they float.",
	  "", "rendering#postfx" },
	{ "Preferences/AO Method", "",
	  "SSAO is the cheapest. HBAO follows the surface horizon and is more "
	  "accurate; GTAO is the most accurate and the most expensive.",
	  "", "rendering#postfx" },
	{ "Preferences/AO Radius", "",
	  "How far, in world units, a surface looks for neighbours that might be "
	  "shadowing it. Large radii darken whole surfaces rather than their creases.",
	  "", "rendering#postfx" },
	{ "Preferences/AO Intensity", "",
	  "How dark the occlusion gets. Past 1 it stops reading as shadow and starts "
	  "reading as dirt.",
	  "", "rendering#postfx" },
	{ "Preferences/Screen-Space Reflections", "",
	  "Reflects what is already on screen in wet and polished surfaces. What is "
	  "off screen cannot be reflected — that is the method's limit, not a bug.",
	  "", "rendering#postfx" },
	{ "Preferences/SSR Intensity", "",
	  "How strongly the screen-space reflection is blended in.",
	  "", "rendering#postfx" },
	{ "Preferences/SSR Max Roughness", "",
	  "The roughest surface still worth reflecting into. Rough surfaces scatter "
	  "so much that the reflection costs more than it shows.",
	  "", "rendering#postfx" },
	{ "Preferences/SSR Quality", "",
	  "How many steps each reflection ray takes. More steps reach further before "
	  "the reflection gives up and fades.",
	  "", "rendering#performance" },
	{ "Preferences/Global Illumination (ray-traced, Metal)", "",
	  "Ray-traced bounce light: colour carried from lit surfaces into the shadows "
	  "around them. Needs a ray-tracing capable GPU.",
	  "", "rendering#lighting" },
	{ "Preferences/GI Indirect Intensity", "",
	  "How much of that bounced light is applied. Above 1 rooms glow from their "
	  "own walls.",
	  "", "rendering#lighting" },
	{ "Preferences/GI Light Radius (deg)", "",
	  "How large the light source is treated as being, in degrees. Wider means "
	  "softer, more diffuse indirect shadows.",
	  "", "rendering#lighting" },
	{ "Preferences/GI Reflections (ray-traced)", "",
	  "Traced reflections instead of screen-space ones: they can show what is "
	  "behind the camera, at the cost of tracing the scene.",
	  "", "rendering#postfx" },
	{ "Preferences/GI Refl Intensity", "",
	  "How strongly the traced reflection is blended in.", "", "rendering#postfx" },
	{ "Preferences/GI Refl Max Roughness", "",
	  "The roughest surface still traced. Above it the cheaper approximation "
	  "takes over.",
	  "", "rendering#postfx" },
	{ "Preferences/GI Refl Quality", "",
	  "Rays per pixel for the traced reflections — the dial between noise and "
	  "frame time.",
	  "", "rendering#performance" },
	{ "Preferences/GI Refl Bounces (Metal)", "",
	  "How many times a reflection ray may bounce on. Two is enough for a mirror "
	  "facing a mirror to look right.",
	  "", "rendering#performance" },
	{ "Preferences/GI Refl Blur", "",
	  "Smooths the traced reflections before they are composited, which is what "
	  "hides the noise a low ray count leaves.",
	  "", "rendering#postfx" },
	{ "Preferences/GPU Weather Particles", "",
	  "Simulate rain and snow on the GPU. Far more drops for the same frame time; "
	  "it needs a backend that can do it, and falls back quietly where it cannot.",
	  "", "rendering#weather" },
	{ "Preferences/Find Sessions on the Local Network", "",
	  "Announce and discover collaboration sessions on this network, so joining "
	  "needs no address. Off, a session is still reachable by its id and code.",
	  "", "collaboration#discovery" },
	{ "Preferences/Sync Large Assets (Meshes, Textures, Audio)", "",
	  "Send the big files to peers as well as the small ones. Off, everybody needs "
	  "their own copy of the meshes and textures — but joining is instant.",
	  "", "collaboration#bigassets" },
	{ "Preferences/Largest Asset to Transfer (MB)", "",
	  "The ceiling on a single transferred file. Anything above it is skipped and "
	  "reported rather than holding up the session.",
	  "", "collaboration#bigassets" },
	{ "Preferences/Camera Speed", "",
	  "How fast the editor's fly camera moves, in units per second. The viewport "
	  "toolbar's speed field is the same value.",
	  "", "editor#viewport" },
	{ "Preferences/Pointer Device", "",
	  "Whether the preview panes expect a mouse or a trackpad. On a trackpad a "
	  "two-finger swipe steers and zoom moves behind the modifier key.",
	  "", "editor#preferences" },
	{ "Preferences/Stick Deadzone", "",
	  "How far a gamepad stick must be pushed before it counts as moved. It is "
	  "what stops a worn stick from drifting the camera on its own.",
	  "", "systems#input" },
	{ "Preferences/Trigger Deadzone", "",
	  "The same for the analogue triggers.", "", "systems#input" },
	{ "Preferences/UI Font Scale", "",
	  "Scales the editor's entire interface. For a high-resolution display, or "
	  "simply for reading comfort.",
	  "", "editor#preferences" },
	{ "Preferences/Keep CPU Asset Cache", "",
	  "Keeps mesh and texture data in main memory after it has been uploaded to "
	  "the GPU. It costs RAM and saves a re-read.",
	  "", "advanced#assets" },
	{ "Preferences/Refresh Interval (s)", "",
	  "How often the Content Browser re-checks the project folder for files "
	  "changed outside the editor.",
	  "", "editor#content-browser" },
	{ "Preferences/Restore Defaults", "Restore Defaults",
	  "Puts the settings in the category you are looking at back the way they "
	  "shipped. Only this category, and only the ones the engine owns — your "
	  "pinned settings and the project itself are untouched.",
	  "", "editor#preferences" },

	// ── Preferences » Source Control ─────────────────────────────────────────
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

	// ── Preferences » Tools ──────────────────────────────────────────────────
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
	  "Preferences » Tools still says so — this only silences the interruption.",
	  "", "horizoncode#compiler" },
	{ "Build Tools/Recheck", "Recheck",
	  "Probes for cmake and a compiler again. A clean result closes this dialog "
	  "on its own, so this is what to press after an install finishes elsewhere.",
	  "", "horizoncode#compiler" },

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
		{ "Preferences/",    "editor-settings", "Settings Reference", "Preferences" },
		{ "settings.",       "editor-settings", "Settings Reference", "Preferences" },
		{ "Source Control/", "editor-settings", "Settings Reference", "Source control setup" },
		{ "Tool Status/",    "editor-settings", "Settings Reference", "Tool status" },
		{ "Build Tools/",    "editor-settings", "Settings Reference", "Build tools" },
		// ── The asset editors ────────────────────────────────────────────────
		{ "material.", "editor-materials", "Material Editor",   "Material graph" },
		{ "ui.",       "editor-ui",        "UI Designer",       "Widget designer" },
		{ "input.",    "editor-input",     "Input Reference",   "Input assets" },
		{ "hc.",       "editor-horizoncode", "HorizonCode Editor", "Graph editing" },
		{ "terrain.",  "editor-landscape", "Landscape Tools",   "Terrain brush" },
		{ "env.",      "editor-landscape", "Landscape Tools",   "Environment window" },
		{ "anim.",     "editor-animation", "Animation Editors", "Animator" },
		// ── Build, diagnose, collaborate ─────────────────────────────────────
		{ "export.",   "editor-export",    "Export & Diagnostics", "Export" },
		{ "profiler.", "editor-export",    "Export & Diagnostics", "Profiler" },
		{ "collab.",   "editor-collab",    "Collaboration & Source Control", "Collaboration" },
		{ "sc.",       "editor-collab",    "Collaboration & Source Control", "Source control" },
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
