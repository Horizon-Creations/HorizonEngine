#include "StaticMeshEditorPanel.h"
#include <cstdint>
#include "EditorApplication.h" // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h" // shared per-tab state map + lazy asset open
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace StaticMeshEditorPanel
{

namespace
{

struct UvStats
{
	bool   haveUVs      = false;
	bool   allZero      = true;   // no real unwrap (every UV at the origin)
	size_t outsideTile  = 0;      // verts outside the 0..1 tile → the texture repeats
	float  minU = 0, maxU = 0, minV = 0, maxV = 0;
};

struct State
{
	bool        loaded = false;
	std::string relPath;
	std::string name;
	HE::UUID    meshId;

	// UV view: pan/zoom over the 0..1 tile. 1.0 = the tile exactly fills the
	// square canvas.
	float   uvZoom = 1.0f;
	ImVec2  uvPan{ 0.0f, 0.0f };
	bool    showTileGrid = true;
	bool    flipV        = false;   // preview the other V convention at a glance

	UvStats stats;
	bool    statsDone = false;
};

AssetPanelState<State> g_states;

// Interleaved (cooked) assets keep uv in floats 6/7 of each 8-float vertex; loose
// assets keep a separate uvs array. Returns false when the mesh has no UVs.
bool readUV(const StaticMeshAsset& m, size_t vertex, float& u, float& v)
{
	if (m.cooked)
	{
		const size_t base = vertex * 8;
		if (base + 7 >= m.interleaved.size()) return false;
		u = m.interleaved[base + 6];
		v = m.interleaved[base + 7];
		return true;
	}
	if (vertex * 2 + 1 >= m.uvs.size()) return false;
	u = m.uvs[vertex * 2 + 0];
	v = m.uvs[vertex * 2 + 1];
	return true;
}

size_t vertexCount(const StaticMeshAsset& m)
{
	return m.cooked ? static_cast<size_t>(m.vertexCount) : m.vertices.size() / 3;
}

UvStats computeStats(const StaticMeshAsset& m)
{
	UvStats s;
	const size_t n = vertexCount(m);
	bool first = true;
	for (size_t i = 0; i < n; ++i)
	{
		float u, v;
		if (!readUV(m, i, u, v)) continue;
		s.haveUVs = true;
		if (u != 0.0f || v != 0.0f) s.allZero = false;
		if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) ++s.outsideTile;
		if (first) { s.minU = s.maxU = u; s.minV = s.maxV = v; first = false; }
		else
		{
			s.minU = std::min(s.minU, u); s.maxU = std::max(s.maxU, u);
			s.minV = std::min(s.minV, v); s.maxV = std::max(s.maxV, v);
		}
	}
	if (!s.haveUVs) s.allZero = false;
	return s;
}

// Line segments the UV view will draw before it starts dropping triangles. A
// dense mesh can hold hundreds of thousands of edges and ImGui pushes every one
// through the CPU — the cap keeps the tab responsive, and the caller SAYS when
// it truncated rather than quietly showing a partial unwrap.
constexpr size_t kMaxUvSegments = 120000;

void drawUvView(const StaticMeshAsset& mesh, State& st, const ImVec2& size)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  side   = std::max(64.0f, std::min(size.x, size.y));
	const ImVec2 canvasSize(side, side);

	ImGui::InvisibleButton("##uvcanvas", canvasSize,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();

	// Pan with a left/middle drag, zoom on the wheel around the cursor.
	if (ImGui::IsItemActive() &&
	    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
	{
		const ImVec2 d = ImGui::GetIO().MouseDelta;
		st.uvPan.x += d.x; st.uvPan.y += d.y;
	}
	if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
	{
		const float prev = st.uvZoom;
		st.uvZoom = std::clamp(st.uvZoom * (1.0f + ImGui::GetIO().MouseWheel * 0.12f), 0.15f, 40.0f);
		// Keep the point under the cursor put.
		const ImVec2 m = ImGui::GetMousePos();
		const float k = st.uvZoom / prev;
		st.uvPan.x = m.x - origin.x - (m.x - origin.x - st.uvPan.x) * k;
		st.uvPan.y = m.y - origin.y - (m.y - origin.y - st.uvPan.y) * k;
	}

	const float tile = side * st.uvZoom;
	auto toScreen = [&](float u, float v) {
		// V grows UPWARD in the engine's GL-style convention, so the tile's v=0
		// edge is drawn at the BOTTOM — otherwise every unwrap would look
		// vertically mirrored against the actual texture.
		const float vv = st.flipV ? v : (1.0f - v);
		return ImVec2(origin.x + st.uvPan.x + u  * tile,
		              origin.y + st.uvPan.y + vv * tile);
	};

	dl->PushClipRect(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y), true);
	dl->AddRectFilled(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y),
	                  IM_COL32(24, 24, 28, 255));

	// The 0..1 tile + a 4x4 sub-grid, so overflow past the tile is obvious.
	if (st.showTileGrid)
	{
		const ImVec2 tl = toScreen(0.0f, 1.0f), br = toScreen(1.0f, 0.0f);
		dl->AddRectFilled(tl, br, IM_COL32(255, 255, 255, 10));
		for (int i = 1; i < 4; ++i)
		{
			const float f = static_cast<float>(i) / 4.0f;
			dl->AddLine(toScreen(f, 0.0f), toScreen(f, 1.0f), IM_COL32(255, 255, 255, 22));
			dl->AddLine(toScreen(0.0f, f), toScreen(1.0f, f), IM_COL32(255, 255, 255, 22));
		}
		dl->AddRect(tl, br, IM_COL32(255, 210, 110, 160));
	}

	// UV wireframe: one line per triangle edge.
	size_t drawn = 0;
	bool   truncated = false;
	const ImU32 edge = IM_COL32(120, 200, 255, 190);
	for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
	{
		if (drawn + 3 > kMaxUvSegments) { truncated = true; break; }
		float u[3], v[3];
		bool ok = true;
		for (int k = 0; k < 3; ++k)
			if (!readUV(mesh, mesh.indices[t + k], u[k], v[k])) { ok = false; break; }
		if (!ok) continue;
		const ImVec2 p0 = toScreen(u[0], v[0]);
		const ImVec2 p1 = toScreen(u[1], v[1]);
		const ImVec2 p2 = toScreen(u[2], v[2]);
		dl->AddLine(p0, p1, edge);
		dl->AddLine(p1, p2, edge);
		dl->AddLine(p2, p0, edge);
		drawn += 3;
	}
	dl->PopClipRect();

	if (truncated)
		ImGui::TextDisabled("Showing the first %zu edges of %zu — zoom is unaffected, "
		                    "the rest are omitted for responsiveness.",
		                    drawn, mesh.indices.size());
}

} // namespace

bool isStaticMeshAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::StaticMesh);
}

void forget(const std::string& assetPath) { g_states.forget(assetPath); }

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = g_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.meshId = openPanelAsset(ctx, assetPath, st.name, st.relPath);
		st.loaded = true;
	}

	ImGui::SetCursorScreenPos(pos);
	ImGui::BeginChild("##staticMeshEditorRoot", size, false);

	const StaticMeshAsset* mesh = ctx.contentManager
		? ctx.contentManager->getStaticMesh(st.meshId) : nullptr;
	if (!mesh)
	{
		ImGui::TextDisabled("Could not load '%s' as a static mesh.", st.name.c_str());
		ImGui::EndChild();
		return;
	}
	if (!st.statsDone) { st.stats = computeStats(*mesh); st.statsDone = true; }

	ImGui::TextUnformatted(mesh->name.empty() ? st.name.c_str() : mesh->name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s", st.relPath.c_str());
	ImGui::Separator();

	// ── Left: stats + UV health ──────────────────────────────────────────────
	ImGui::BeginChild("##smInfo", ImVec2(280.0f, 0.0f), true);
	const size_t verts = vertexCount(*mesh);
	ImGui::SeparatorText("Geometry");
	ImGui::Text("Vertices  %zu", verts);
	ImGui::Text("Triangles %zu", mesh->indices.size() / 3);
	ImGui::Text("Format    %s", mesh->cooked ? "cooked (interleaved)" : "loose (SoA)");
	ImGui::Text("Bounds X  %.3f .. %.3f", mesh->boundsMin[0], mesh->boundsMax[0]);
	ImGui::Text("       Y  %.3f .. %.3f", mesh->boundsMin[1], mesh->boundsMax[1]);
	ImGui::Text("       Z  %.3f .. %.3f", mesh->boundsMin[2], mesh->boundsMax[2]);
	if (!mesh->materialPath.empty())
		ImGui::TextWrapped("Material  %s", mesh->materialPath.c_str());

	ImGui::SeparatorText("UVs");
	if (!st.stats.haveUVs)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No texture coordinates.");
		ImGui::TextWrapped("Every texture sample collapses to one texel. Loose meshes get "
		                   "box-projected UVs on import; this one has none at all.");
	}
	else if (st.stats.allZero)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "All UVs are (0,0).");
		ImGui::TextWrapped("The mesh carries a UV set but it is unwrapped to a single point — "
		                   "textures will show one flat colour.");
	}
	else
	{
		ImGui::Text("Range U   %.3f .. %.3f", st.stats.minU, st.stats.maxU);
		ImGui::Text("Range V   %.3f .. %.3f", st.stats.minV, st.stats.maxV);
		if (st.stats.outsideTile > 0)
		{
			ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f),
			                   "%zu verts outside the 0..1 tile", st.stats.outsideTile);
			ImGui::TextWrapped("That is fine for a tiling texture — the pattern repeats — "
			                   "but not for an atlas.");
		}
		else
			ImGui::TextDisabled("Fully inside the 0..1 tile.");
	}

	ImGui::SeparatorText("View");
	ImGui::Checkbox("Tile grid", &st.showTileGrid);
	ImGui::Checkbox("Flip V", &st.flipV);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The engine uses a GL-style bottom-left UV origin.\n"
		                  "Flip to preview how the unwrap reads under the other convention.");
	ImGui::Text("Zoom %.2fx", st.uvZoom);
	if (ImGui::Button("Reset View")) { st.uvZoom = 1.0f; st.uvPan = ImVec2(0.0f, 0.0f); }
	ImGui::TextDisabled("Drag to pan, wheel to zoom.");
	ImGui::EndChild();

	// ── Right: the UV wireframe ──────────────────────────────────────────────
	ImGui::SameLine();
	ImGui::BeginChild("##smUv", ImVec2(0.0f, 0.0f), true);
	drawUvView(*mesh, st, ImGui::GetContentRegionAvail());
	ImGui::EndChild();

	ImGui::EndChild();
}

} // namespace StaticMeshEditorPanel
