#include "AppMetadataRows.h"
#include "EditorApplication.h"           // AppContext, ProjectManager
#include "EditorWidgets.h"               // Row:: label-above widgets
#include <Application/AppIcon.h>         // the generated icon, the PNG loader, the resampler
#include <Renderer/UIFont.h>             // uiParseRichColor
#include <Renderer/IRenderer.h>          // CreateImGuiTexture
#include <SDL3/SDL_dialog.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace AppMetadataRows
{

namespace fs = std::filesystem;

std::filesystem::path resolveProjectFile(const AppContext& ctx, const std::string& stored)
{
	if (stored.empty()) return {};
	const fs::path p(stored);
	if (p.is_absolute() || !ctx.projectManager) return p;
	return fs::path(ctx.projectManager->projectRoot()) / p;
}

#ifdef HE_IMGUI_ENABLED

namespace
{
	// The one PNG picker both rows share. Which row asked is remembered in
	// `slot`, so a pick for the splash never lands in the icon field. The
	// callback runs on SDL's thread; the draw code polls the flag.
	enum class Slot { None, Icon, Splash };
	Slot              s_pickSlot = Slot::None;
	std::string       s_pickPath;
	std::atomic<bool> s_pickReady{false};

	void requestPng(const AppContext& ctx, Slot slot)
	{
		s_pickSlot = slot;
		static const SDL_DialogFileFilter kFilters[] = {
			{ "PNG image", "png" },
			{ "All files", "*" },
		};
		SDL_ShowOpenFileDialog(
			[](void* /*userdata*/, const char* const* filelist, int /*filter*/)
			{
				// A cancelled dialog hands over an empty list; the poll below
				// then finds an empty path and does nothing.
				s_pickPath = (filelist && filelist[0]) ? filelist[0] : "";
				s_pickReady.store(true, std::memory_order_release);
			},
			nullptr,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			kFilters, 2, nullptr, false);
	}

	// Takes the picked path for `slot` if one arrived: project-relative when
	// the file lies inside the project, absolute otherwise. False = nothing.
	bool takePng(const AppContext& ctx, Slot slot, std::string& out)
	{
		if (s_pickSlot != slot || !s_pickReady.load(std::memory_order_acquire)) return false;
		s_pickReady.store(false, std::memory_order_relaxed);
		s_pickSlot = Slot::None;
		if (s_pickPath.empty()) return false;
		fs::path picked(s_pickPath);
		if (ctx.projectManager)
		{
			std::error_code ec;
			const fs::path root = fs::path(ctx.projectManager->projectRoot());
			const fs::path rel  = fs::relative(picked, root, ec);
			// generic_string, not native(): on Windows native() is a wstring
			// and has no rfind(const char*).
			if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0)
				picked = rel;
		}
		out = picked.generic_string();
		return true;
	}

	bool outsideProject(const std::string& stored)
	{
		return !stored.empty() && fs::path(stored).is_absolute();
	}

	// One cached preview texture PER SIZE, keyed on everything that changes the
	// picture. Per size because the two callers can be on screen at once —
	// Settings > Application at 128 px and the Export dialog's section at 48 —
	// and one shared slot would then rebuild the texture every frame.
	struct Preview
	{
		int         px     = 0;
		std::string key;
		void*       handle = nullptr;
		ImTextureID tex    = 0;
	};
	Preview s_previews[2];
	Preview& previewSlot(int px)
	{
		for (Preview& p : s_previews) if (p.px == px) return p;
		for (Preview& p : s_previews) if (p.px == 0)  return p;
		return s_previews[0];   // a third size evicts the first — never happens today
	}
}

bool drawIconFileRow(AppContext& ctx, ProjectData& p)
{
	bool commit = false;
	EditorWidgets::Row::inputText("Icon file##appiconfile", &p.appIconFile);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Icon file");
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Browse...##appiconfile")) requestPng(ctx, Slot::Icon);
	if (takePng(ctx, Slot::Icon, p.appIconFile)) commit = true;
	if (!p.appIconFile.empty())
	{
		std::error_code ec;
		if (!fs::is_regular_file(resolveProjectFile(ctx, p.appIconFile), ec))
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "Not found — the export falls back to the generated icon.");
		else if (outsideProject(p.appIconFile))
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "Outside the project: it will not travel with it. "
			                   "Copy it into Content/ to be safe.");
	}
	return commit;
}

unsigned long long iconPreviewTexture(AppContext& ctx, const ProjectData& p, int px)
{
	if (!ctx.renderer) return 0;
	const fs::path file = resolveProjectFile(ctx, p.appIconFile);
	std::error_code ec;
	const auto mtime = file.empty() ? fs::file_time_type{} : fs::last_write_time(file, ec);
	const std::string key = p.appIconName + "|" + p.appIconColor + "|" + file.string() + "|"
	                      + std::to_string(static_cast<long long>(mtime.time_since_epoch().count())) + "|"
	                      + std::to_string(px);
	Preview& slot = previewSlot(px);
	if (slot.px == px && key == slot.key) return static_cast<unsigned long long>(slot.tex);

	slot.px  = px;
	slot.key = key;
	if (slot.handle) { ctx.renderer->DestroyImGuiTexture(slot.handle); slot.handle = nullptr; }
	slot.tex = 0;

	// The same order the export uses: the file first, the glyph as fallback.
	std::vector<std::uint8_t> rgba;
	if (!file.empty())
	{
		std::vector<std::uint8_t> src;
		int w = 0, h = 0;
		if (HE::heLoadPngRGBA(file, src, w, h))
		{
			std::vector<HE::AppIconImage> set = HE::heAppIconSetFromImage(src.data(), w, h, { px });
			if (!set.empty()) rgba = std::move(set[0].rgba);
		}
	}
	if (rgba.empty() && !p.appIconName.empty())
	{
		glm::vec4 bg(0.12f, 0.44f, 0.78f, 1.0f);
		HE::uiParseRichColor(p.appIconColor, bg);
		rgba = HE::heRenderAppIcon(p.appIconName, px, bg, HE::heAppIconForeground(bg));
	}
	if (!rgba.empty())
		if (void* h = ctx.renderer->CreateImGuiTexture(rgba.data(), px, px))
		{
			slot.handle = h;
			slot.tex    = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(h));
		}
	return static_cast<unsigned long long>(slot.tex);
}

bool drawSplashRows(AppContext& ctx, ProjectData& p, bool compact)
{
	bool commit = false;
	HE::ProjectGameSettings& g = p.settings.game;

	commit |= EditorWidgets::checkbox("Show a splash while starting", &g.splashEnabled);
	EditorWidgets::helpForLabel("Show a splash while starting");
	if (!compact)
		ImGui::TextDisabled("A small window with your picture and the title, up while the game\n"
		                    "loads. Without a picture no splash opens — the engine does not\n"
		                    "advertise itself inside your game.");
	if (g.splashEnabled)
	{
		EditorWidgets::Row::inputText("Splash image##splashimage", &g.splashImage);
		commit |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Splash image");
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Browse...##splashimage")) requestPng(ctx, Slot::Splash);
		if (takePng(ctx, Slot::Splash, g.splashImage)) commit = true;
		std::error_code ec;
		if (g.splashImage.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "No picture: the export copies none and the game opens no splash.");
		else if (!fs::is_regular_file(resolveProjectFile(ctx, g.splashImage), ec))
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "Not found — the export copies none and the game opens no splash.");
		else if (outsideProject(g.splashImage))
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "Outside the project: it will not travel with it.");

		EditorWidgets::Row::inputText("Subtitle##splashsubtitle", &g.splashSubtitle);
		commit |= ImGui::IsItemDeactivatedAfterEdit();
		EditorWidgets::helpForLabel("Subtitle");
		if (!compact)
			ImGui::TextDisabled("The small line under the title — a version, a studio, a tagline.");
	}
	return commit;
}

#else  // !HE_IMGUI_ENABLED

bool drawIconFileRow(AppContext&, ProjectData&) { return false; }
unsigned long long iconPreviewTexture(AppContext&, const ProjectData&, int) { return 0; }
bool drawSplashRows(AppContext&, ProjectData&, bool) { return false; }

#endif

} // namespace AppMetadataRows
