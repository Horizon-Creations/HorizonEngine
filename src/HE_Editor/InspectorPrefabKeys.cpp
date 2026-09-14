#include "InspectorPanel.h"
#include <cstring>

// ── Details-panel section label → scene-format component key ─────────────────
// The one place a Details label is tied to the key the prefab sync speaks. A
// placed prefab's override list is the serializer's diff ("light" /
// "intensity"), and the Details panel names its sections for people ("Light"):
// the "(changed here)" marker on a component header is where the two meet, and
// this table is how. Only components the sync can propagate need an entry; a
// label without one simply gets no marker.
//
// Its own translation unit, and deliberately free of ImGui: the test that holds
// every key here against SceneSerializer::isKnownComponentKey links this file
// alone, and a renamed scene key then fails a test rather than leaving a marker
// that silently points at nothing.
namespace InspectorPanel
{
namespace
{
	struct ComponentKeyEntry { const char* label; const char* key; };
	constexpr ComponentKeyEntry kComponentKeys[] = {
		{ "Transform",              "transform" },
		{ "Transform 2D",           "transform2d" },
		{ "Mesh",                   "mesh" },
		{ "Skeletal Mesh",          "skeletalmesh" },
		{ "Material",               "material" },
		{ "Light",                  "light" },
		{ "Decal",                  "decal" },
		{ "Rope",                   "rope" },
		{ "Trail",                  "trail" },
		{ "Rigid Body",             "rigidbody" },
		{ "Collider",               "collider" },
		{ "Joint",                  "joint" },
		{ "Character Controller",   "characterController" },
		{ "Movement",               "movement" },
		{ "Camera",                 "camera" },
		{ "Camera Rig",             "cameraRig" },
		{ "Script",                 "script" },
		{ "Terrain",                "terrain" },
		{ "Foliage",                "foliage" },
		{ "Nav Mesh",               "navmesh" },
		{ "Nav Agent",              "navagent" },
		{ "Audio Source",           "audiosource" },
		{ "Audio Listener",         "audiolistener" },
		{ "Animator",               "animator" },
		{ "Animator Blend",         "animatorblend" },
		{ "Animator State Machine", "animstatemachine" },
		{ "Root Motion",            "rootmotion" },
		{ "Animation Layers",       "animationlayers" },
		{ "Inverse Kinematics",     "ik" },
		{ "Property Animator",      "propertyanimator" },
		{ "Particle System",        "particlesystem" },
		{ "Save State",             "saveState" },
		{ "LOD",                    "lod" },
		{ "Environment",            "environment" },
		{ "Weather",                "weather" },
		{ "UI Canvas",              "uicanvas" },
		{ "UI Element",             "uielement" },
		{ "UI Text",                "uitext" },
		{ "UI Image",               "uiimage" },
		{ "UI Button",              "uibutton" },
	};
}

const char* componentKeyForLabel(const char* label)
{
	if (!label) return nullptr;
	for (const auto& e : kComponentKeys)
		if (std::strcmp(e.label, label) == 0) return e.key;
	return nullptr;
}

size_t componentKeyCount() { return sizeof(kComponentKeys) / sizeof(kComponentKeys[0]); }

const char* componentLabelAt(size_t i)
{
	return i < componentKeyCount() ? kComponentKeys[i].label : nullptr;
}

} // namespace InspectorPanel
