#include "EditorGraphics.h"

#include <cstring>

namespace EditorGraphics
{

void apply(const HE::ProjectGraphics& from, EditorConfig& to)
{
#define HE_X(T, name, def, editorName, lo, hi) to.editorName = from.name;
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
}

void capture(const EditorConfig& from, HE::ProjectGraphics& to)
{
#define HE_X(T, name, def, editorName, lo, hi) to.name = from.editorName;
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
}

void copyFields(const EditorConfig& from, EditorConfig& to)
{
#define HE_X(T, name, def, editorName, lo, hi) to.editorName = from.editorName;
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
}

bool same(const EditorConfig& a, const EditorConfig& b)
{
#define HE_X(T, name, def, editorName, lo, hi) if (a.editorName != b.editorName) return false;
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
	return true;
}

namespace
{
// The catalogue's row keys, grouped the way the project's pages group them. A
// row may edit several fields (the "aa" row is anti-aliasing, sharpness, render
// scale and specular AA together), which is why this is keyed by row and not by
// field.
constexpr const char* kQuality[]        = { "renderpath", "occlusion", "aa", "gpuparticles" };
constexpr const char* kPostProcessing[] = { "bloom", "ssao", "dof", "motionblur" };
constexpr const char* kReflectionsGi[]  = { "ssr", "gi", "girefl" };

template <size_t N>
RowSet set(const char* const (&keys)[N]) { return { keys, static_cast<int>(N) }; }
} // namespace

RowSet qualityRows()          { return set(kQuality); }
RowSet postProcessingRows()   { return set(kPostProcessing); }
RowSet reflectionsAndGiRows() { return set(kReflectionsGi); }

bool isGraphicsRow(const char* key)
{
	if (!key) return false;
	for (const RowSet& r : { qualityRows(), postProcessingRows(), reflectionsAndGiRows() })
		for (int i = 0; i < r.count; ++i)
			if (std::strcmp(r.keys[i], key) == 0) return true;
	return false;
}

} // namespace EditorGraphics
