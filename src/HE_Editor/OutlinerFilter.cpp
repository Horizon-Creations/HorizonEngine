#include "OutlinerFilter.h"
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/AudioListenerComponent.h>
#include <HorizonScene/Components/DecalComponent.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
#include <algorithm>
#include <cctype>

namespace OutlinerFilter
{

namespace
{
	template <typename... C>
	bool anyOf(const entt::registry& r, Entity e) { return r.any_of<C...>(e); }

	// Not in the list, on purpose: Transform, Name, Hierarchy, EntityId, Save
	// State and Network are on nearly everything or on nothing the user placed,
	// Material and LOD ride on a mesh and are found through it, and a Terrain
	// Chunk is generated and never shown. A type nobody can tell apart by is
	// not a type worth a dropdown entry.
	const Kind kKinds[] = {
		{ "All types",            [](const entt::registry&, Entity) { return true; } },
		{ "Mesh",                 anyOf<MeshComponent> },
		{ "Skeletal Mesh",        anyOf<SkeletalMeshComponent> },
		// A camera rig is a camera to the user — "Camera (Third Person)" in the
		// Create menu is one — so both components count.
		{ "Camera",               anyOf<CameraComponent, CameraRigComponent> },
		{ "Light",                anyOf<LightComponent> },
		{ "Rigid Body",           anyOf<RigidBodyComponent> },
		{ "Collider",             anyOf<ColliderComponent> },
		{ "Character Controller", anyOf<CharacterControllerComponent> },
		{ "Script",               anyOf<ScriptComponent> },
		{ "Particle System",      anyOf<ParticleSystemComponent> },
		{ "Audio",                anyOf<AudioSourceComponent, AudioListenerComponent> },
		{ "Decal",                anyOf<DecalComponent> },
		{ "Foliage",              anyOf<FoliageComponent> },
		{ "Terrain",              anyOf<TerrainComponent> },
		{ "Rope",                 anyOf<RopeComponent> },
		{ "Trail",                anyOf<TrailComponent> },
		{ "Animator",             anyOf<AnimatorComponent, AnimatorStateMachineComponent> },
		{ "Navigation",           anyOf<NavMeshComponent, NavAgentComponent> },
		// Every in-world UI element carries UIElementComponent; the canvas that
		// roots them carries UICanvasComponent and nothing else.
		{ "UI",                   anyOf<UICanvasComponent, UIElementComponent> },
		{ "Weather",              anyOf<WeatherComponent> },
		{ "Prefab Instance",      anyOf<PrefabInstanceComponent> },
	};

	// One entry per kKinds entry, same order. The colours are spread over the
	// hue wheel so neighbouring rows tell apart; letters are the initial where
	// one is free.
	struct Look { const char* glyph; unsigned rgb; };
	const Look kLooks[] = {
		{ "All", 0x9A9488 },   // All types
		{ "M",   0x5B9BD5 },   // Mesh
		{ "Sk",  0x7C8CF8 },   // Skeletal Mesh
		{ "C",   0xE8A04C },   // Camera
		{ "L",   0xF2D45C },   // Light
		{ "B",   0xC678DD },   // Rigid Body
		{ "X",   0x56B6C2 },   // Collider
		{ "H",   0x4EC9A0 },   // Character Controller
		{ "{}",  0x98C379 },   // Script
		{ "P",   0xE06C9F },   // Particle System
		{ "A",   0xD19A66 },   // Audio
		{ "D",   0xB48EAD },   // Decal
		{ "F",   0x6FBF73 },   // Foliage
		{ "T",   0xA3875B },   // Terrain
		{ "R",   0xE5C07B },   // Rope
		{ "~",   0x61AFEF },   // Trail
		{ "An",  0xFF9E64 },   // Animator
		{ "N",   0x2AC3DE },   // Navigation
		{ "UI",  0xBB9AF7 },   // UI
		{ "W",   0x7DCFFF },   // Weather
		{ "Pf",  0xE8A04C },   // Prefab Instance
	};
	static_assert(sizeof(kLooks) / sizeof(kLooks[0]) == sizeof(kKinds) / sizeof(kKinds[0]),
	              "every kind needs a look");

	// What an entity is called first, most telling first. A component that only
	// decorates another (a Collider on a Mesh) comes after the thing it decorates.
	// Prefab Instance is not here: it has a chip of its own on the row.
	const int kPriority[] = {
		3,   // Camera
		4,   // Light
		13,  // Terrain
		12,  // Foliage
		19,  // Weather
		18,  // UI
		2,   // Skeletal Mesh
		1,   // Mesh
		9,   // Particle System
		10,  // Audio
		11,  // Decal
		14,  // Rope
		15,  // Trail
		7,   // Character Controller
		5,   // Rigid Body
		17,  // Navigation
		16,  // Animator
		6,   // Collider
		8,   // Script
	};
}

int kindCount()
{
	return static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));
}

int primaryKind(const entt::registry& reg, Entity e)
{
	for (const int k : kPriority)
		if (kKinds[k].test(reg, e)) return k;
	return kAllKinds;
}

const char* kindGlyph(int i)
{
	if (i < 0 || i >= kindCount()) i = kAllKinds;
	return kLooks[i].glyph;
}

unsigned kindColor(int i)
{
	if (i < 0 || i >= kindCount()) i = kAllKinds;
	return kLooks[i].rgb;
}

void tally(const entt::registry& reg, Entity e, int* counts)
{
	++counts[kAllKinds];
	for (int i = 1; i < kindCount(); ++i)
		if (kKinds[i].test(reg, e)) ++counts[i];
}

const Kind& kindAt(int i)
{
	if (i < 0 || i >= kindCount()) i = kAllKinds;
	return kKinds[i];
}

bool nameMatches(std::string_view name, std::string_view needle)
{
	if (needle.empty()) return true;
	if (needle.size() > name.size()) return false;
	const auto lower = [](char c)
	{ return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
	const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(),
	                            [&](char a, char b) { return lower(a) == lower(b); });
	return it != name.end();
}

Span matchSpan(std::string_view name, std::string_view needle)
{
	if (needle.empty() || needle.size() > name.size()) return {};
	const auto lower = [](char c)
	{ return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
	const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(),
	                            [&](char a, char b) { return lower(a) == lower(b); });
	if (it == name.end()) return {};
	return { static_cast<size_t>(it - name.begin()), needle.size() };
}

std::vector<Shown> apply(const std::vector<Row>& rows)
{
	std::vector<Shown> out(rows.size());
	// The chain of ancestors of the row being looked at, as indices into
	// `rows`, one per depth. Depth-first order means the ancestor at depth d
	// is simply the last row seen at depth d — so a hit marks its path by
	// walking this stack, without the rows knowing their parents.
	std::vector<size_t> path;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		// Back up to this row's parent. Only ever shrinks: a row that skips a
		// depth (which a well-formed walk never produces) is treated as the
		// child of the deepest ancestor there is rather than of a phantom.
		const auto depth = static_cast<size_t>(std::max(0, rows[i].depth));
		if (path.size() > depth) path.resize(depth);
		if (rows[i].matches)
		{
			out[i].show = Show::Hit;
			// Every ancestor now has a shown row directly under it — the next
			// link of this path. One that is itself a hit stays a hit; the
			// dimming is for rows that are only there for the path.
			for (const size_t a : path)
			{
				out[a].childShown = true;
				if (out[a].show == Show::Hidden) out[a].show = Show::Context;
			}
		}
		path.push_back(i);
	}
	return out;
}

} // namespace OutlinerFilter
