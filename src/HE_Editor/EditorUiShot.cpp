#include "EditorUiShot.h"

#include <Diagnostics/Logger.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
// The test harness's CPU rasteriser (linked into HorizonEditor for these witnesses).
#include "../../tests/ImGuiSoftwareRaster.h"
#endif

namespace HE::Ed::UiShot
{

#ifdef HE_IMGUI_ENABLED
namespace
{
	// The rasteriser is fed a CLONE of the draw data. The live draw data carries
	// the GPU backend's texture ids and texture requests; handed those, the
	// rasteriser would claim the font atlas from the real backend. The clone's
	// commands point at CPU copies of ImGui's own textures instead, and anything
	// that is not one of them (the Scene viewport's GPU image) is drawn flat
	// grey — there is no picture of it on this side.
	//
	// A CPU copy of the ImGui texture behind `id`, RGBA8 (the atlas may be
	// Alpha8), registered with the rasteriser. `grey` when `id` is not one of
	// ImGui's own textures.
	ImTextureID cpuTexture(ImTextureID id, ImTextureID grey,
	                       std::vector<std::pair<ImTextureID, ImTextureID>>& made)
	{
		for (const auto& [from, to] : made)
			if (from == id) return to;
		ImTextureID to = grey;
		for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
		{
			if (!t || t->GetTexID() != id || !t->Pixels || t->Width <= 0 || t->Height <= 0)
				continue;
			const size_t n = static_cast<size_t>(t->Width) * static_cast<size_t>(t->Height);
			std::vector<std::uint8_t> rgba(n * 4);
			for (size_t i = 0; i < n; ++i)
			{
				if (t->BytesPerPixel == 1)
				{
					rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
					rgba[i * 4 + 3] = t->Pixels[i];
				}
				else
					std::memcpy(&rgba[i * 4], &t->Pixels[i * 4], 4);
			}
			to = he_ui::registerTexture(rgba.data(), t->Width, t->Height);
			break;
		}
		made.emplace_back(id, to);
		return to;
	}

	struct Burst
	{
		std::string name;
		int         total   = 0;   // frames asked for
		int         written = 0;
		bool        armed   = false;
	};
}
#endif

bool captureDrawData(const std::string& path)
{
#ifdef HE_IMGUI_ENABLED
	const ImDrawData* dd = ImGui::GetDrawData();
	if (!dd || !dd->Valid || dd->DisplaySize.x < 1.0f || dd->DisplaySize.y < 1.0f)
		return false;

	const std::uint8_t greyPx[4] = { 58, 54, 50, 255 };
	const ImTextureID grey = he_ui::registerTexture(greyPx, 1, 1);
	std::vector<std::pair<ImTextureID, ImTextureID>> made;
	std::vector<ImDrawList*> clones;

	ImDrawData copy;
	copy.Valid            = true;
	copy.DisplayPos       = dd->DisplayPos;
	copy.DisplaySize      = dd->DisplaySize;
	copy.FramebufferScale = ImVec2(1.0f, 1.0f);   // one pixel per point is plenty
	copy.Textures         = nullptr;              // no texture requests: not ours to answer
	for (const ImDrawList* list : dd->CmdLists)
	{
		ImDrawList* c = list->CloneOutput();
		for (ImDrawCmd& cmd : c->CmdBuffer)
			if (!cmd.UserCallback)
				cmd.TexRef = ImTextureRef(cpuTexture(cmd.GetTexID(), grey, made));
		clones.push_back(c);
		// Straight into the list, not AddDrawList(): that asserts on the
		// write cursor, which CloneOutput() leaves unset.
		copy.CmdLists.push_back(c);
		copy.CmdListsCount += 1;
		copy.TotalVtxCount += c->VtxBuffer.Size;
		copy.TotalIdxCount += c->IdxBuffer.Size;
	}

	const he_ui::Image img = he_ui::rasterize(&copy, static_cast<int>(dd->DisplaySize.x),
	                                          static_cast<int>(dd->DisplaySize.y));
	const bool ok = img.valid() && he_ui::writeBmp(img, path);

	for (ImDrawList* c : clones) IM_DELETE(c);
	for (const auto& [from, to] : made)
		if (to != grey) he_ui::unregisterTexture(to);
	he_ui::unregisterTexture(grey);
	return ok;
#else
	(void)path;
	return false;
#endif
}

void liveAfterRender()
{
#ifdef HE_IMGUI_ENABLED
	static const char* s_dir     = std::getenv("HE_DUMP_UI");
	static const char* s_trigger = std::getenv("HE_DUMP_UI_TRIGGER");
	if (!s_dir || !*s_dir || !s_trigger || !*s_trigger) return;
	static Burst s_burst;
	std::error_code ec;

	if (!s_burst.armed)
	{
		if (!std::filesystem::exists(s_trigger, ec)) return;
		std::ifstream in(s_trigger);
		std::string line;
		std::getline(in, line);
		std::istringstream words(line);
		Burst b;
		float x = 0.0f, y = 0.0f;
		if (!(words >> b.name)) b.name = "ui";
		if (!(words >> b.total) || b.total < 1) b.total = 1;
		if (words >> x >> y)
		{
			// Queued for the next NewFrame, which is the burst's first frame. A
			// negative position takes the mouse away again.
			ImGuiIO& io = ImGui::GetIO();
			if (x < 0.0f || y < 0.0f) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
			else                      io.AddMousePosEvent(x, y);
		}
		b.armed = true;
		s_burst = std::move(b);
		std::filesystem::create_directories(s_dir, ec);
		// The burst starts with the next frame: the one that has seen the mouse.
		return;
	}

	char index[16];
	std::snprintf(index, sizeof(index), "-%03d.bmp", s_burst.written);
	const std::string path = std::string(s_dir) + "/" + s_burst.name + index;
	const bool ok = captureDrawData(path);
	HE_LOG_INFO(Editor, "UI shot %s: frame %d, t=%.3f s%s", path.c_str(), ImGui::GetFrameCount(),
	            ImGui::GetTime(), ok ? "" : " (capture FAILED)");
	if (++s_burst.written >= s_burst.total)
	{
		s_burst.armed = false;
		std::filesystem::remove(s_trigger, ec);
	}
#endif
}

} // namespace HE::Ed::UiShot
