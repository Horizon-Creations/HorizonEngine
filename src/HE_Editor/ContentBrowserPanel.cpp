#include "ContentBrowserPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include <algorithm>
#include <cstdint>
#include <mutex>
#include "EditorApplication.h"           // AppContext, GlobalState folders, ProjectManager
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include "ScriptEditorPanel.h"
#include "CppClassEditorPanel.h"
#include "MaterialEditorPanel.h"
#include "UIEditorPanel.h"
#include "HorizonCodeClassPanel.h"
#include "InputAssetPanel.h"
#include "TypeAssetPanel.h"
#include "SkeletalMeshEditorPanel.h"
#include "StaticMeshEditorPanel.h"
#include "ParticleGraphEditorPanel.h"
#include "AnimatorStateMachineEditorPanel.h"
#include "AudioEditorPanel.h"
#include "EditorAssetTypeCache.h"
#include "GitController.h"        // per-file source-control status for the tile badge
#include "AssetThumbnailCache.h"         // rendered mesh/material tiles for the grid
#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h> // remote-only asset download queue
#endif
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <UIWidget/UIWidgetTree.h>       // starter tree for freshly created UI widgets
#include <HorizonCode/HorizonCode.h>     // starter graph for freshly created HorizonCode classes
#include <Types/Enums.h>
#include "MeshImporter.h"
#include "SkeletalMeshImporter.h"
#include "AnimationClipImporter.h"
#include "TextureImporter.h"
#include "MaterialImporter.h"
#include "AudioImporter.h"
#include "FontImporter.h"
#include "ImporterCommon.h"   // Importer::gltfHasSkin — static vs. skeletal routing

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

// File-local alias, as in EditorUI.cpp.
namespace fs = std::filesystem;

namespace ContentBrowserPanel
{

static bool s_quietContentRefresh = false;

#ifdef HE_HAVE_LIBSSH2
// The inverse of mergeManifestInto's fullPath synthesis (GlobalState.cpp):
// a remote-only File's fullPath is always engineContentCacheDir()/relativePath,
// so this recovers the SFTP-side relative path (== the manifest's own "path"
// field, and — per the deployment's remote layout — the same string EngineContent
// sync downloads from) straight back out of it.
static std::string engineRelativePath(const std::string& fullPath)
{
	std::error_code ec;
	const fs::path rel = fs::relative(fullPath, GlobalState::engineContentCacheDir(), ec);
	return ec ? std::string() : rel.generic_string();
}
#endif

// Which root the tree/grid is showing: 0=Content, 1=Engine, 2=Source. At
// namespace scope rather than inside render() only so browsedRootKind() can read
// it — everything that writes it still lives in render().
static int s_selectedRootKind = 0;

bool quietRefreshRequested()  { return s_quietContentRefresh; }
void clearQuietRefreshRequest() { s_quietContentRefresh = false; }
int  browsedRootKind()          { return s_selectedRootKind; }

// Starter template for a freshly created script, by language (0 = Lua, 1 = Python).
static const char* scriptStarterTemplate(int lang)
{
	static const char* kLua =
		"local M = {}\n\n"
		"function M.onStart(self)\nend\n\n"
		"function M.onUpdate(self, dt)\nend\n\n"
		"return M\n";
	static const char* kPy =
		"import horizon\n\n"
		"class NewScript(horizon.Behavior):\n"
		"    def on_start(self):\n        pass\n\n"
		"    def on_update(self, dt):\n        pass\n";
	return (lang == 1) ? kPy : kLua;
}

// An item just moved on disk: hand the old/new ABSOLUTE paths to the content
// manager as the content-relative ones references are stored in, so everything
// that points at the item follows it. Silently a no-op for anything outside the
// content/engine roots (the C++ Source tree, which holds no asset references).
static void retargetReferences(AppContext& ctx, const std::string& oldAbs,
                               const std::string& newAbs, bool folder)
{
	if (!ctx.contentManager) return;
	const std::string oldRel = ctx.contentManager->toContentRelativePath(oldAbs);
	const std::string newRel = ctx.contentManager->toContentRelativePath(newAbs);
	if (oldRel.empty() || newRel.empty()) return;
	ctx.contentManager->retargetAssetReferences(oldRel, newRel, folder);
}

// Is `path` inside `folder`? Both are absolute disk paths, but they are not
// built the same way everywhere in the panel (the folder scan uses the native
// separator, the create handlers append '/'), so the boundary character is
// accepted in either form rather than compared against one separator.
static bool isUnderFolder(const std::string& path, const std::string& folder)
{
	if (folder.empty() || path.size() <= folder.size()) return false;
	if (path.compare(0, folder.size(), folder) != 0)    return false;
	const char sep = path[folder.size()];
	return sep == '/' || sep == '\\';
}

// Every file below `folderAbs`, as paths relative to it — what a folder delete
// would take with it, for the confirmation dialog. Dotfiles/dotfolders are
// skipped for the same reason GlobalState's content scan hides them: they are
// VCS/OS bookkeeping (.gitkeep, .DS_Store), not content the user put there. The
// deletion itself removes them regardless; this list only decides what the
// warning shows and whether a folder counts as empty enough to go without one.
static std::vector<std::string> collectFolderContents(const std::string& folderAbs)
{
	std::vector<std::string> out;
	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(
		folderAbs, std::filesystem::directory_options::skip_permission_denied, ec);
	if (ec) return out;

	const std::filesystem::recursive_directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break; // iteration broke down — warn with what was readable
		const std::filesystem::path p = it->path();
		if (p.filename().string().rfind('.', 0) == 0)
		{
			std::error_code dirEc;
			if (it->is_directory(dirEc)) it.disable_recursion_pending();
			continue;
		}
		std::error_code fileEc;
		if (!it->is_regular_file(fileEc)) continue;
		std::error_code relEc;
		const std::filesystem::path rel = std::filesystem::relative(p, folderAbs, relEc);
		out.push_back(relEc ? p.filename().string() : rel.generic_string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

void render(AppContext& ctx, int& tabSelectRequest,
            const std::function<void(const std::string&)>& openSceneGuarded)
{
#ifdef HE_IMGUI_ENABLED
    //Content Browser
	auto [contentFolder, contentLock] = ctx.globalState->lockContentFolder();
	auto [engineFolder, engineLock]   = ctx.globalState->lockEngineFolder();
	auto [sourceFolder, sourceLock]   = ctx.globalState->lockSourceFolder();
	// Only C++ projects author gameplay as native source, so only they show the
	// Source root (its .h/.cpp classes + the h/cpp viewer).
	const bool cbShowSource = ctx.projectManager &&
		ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Cpp;
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ctx.fontHeading) ImGui::PopFont();

    {
        const float totalWidth    = ImGui::GetContentRegionAvail().x;
        const float totalHeight   = ImGui::GetContentRegionAvail().y;
        const float splitterW     = 6.0f;
        const float minPaneWidth  = 60.0f;

        if (ctx.cbTreeWidth < 0.0f)
            ctx.cbTreeWidth = totalWidth * 0.25f;

        ctx.cbTreeWidth = std::clamp(ctx.cbTreeWidth, minPaneWidth, totalWidth - minPaneWidth - splitterW);

		ImGui::BeginChild("##cb_tree", ImVec2(ctx.cbTreeWidth, totalHeight), false);

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Content");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();

		// ── Tree: single-click = expand/collapse, double-click = navigate ──
		static const HE::Folder* s_selectedTreeFolder = nullptr;
		// s_selectedRootKind (namespace scope, above) says which root
		// s_selectedTreeFolder/s_gridFolder belongs to. Needed because nullptr used
		// to unambiguously mean "the Content root" — three roots now exist, so
		// nullptr is ambiguous and every place that treats it as a root sentinel
		// must also check this tag.
		// The Folder backing each root kind (structured-binding refs captured above).
		auto cbRootFolder = [&](int kind) -> const HE::Folder&
		{ return kind == 1 ? engineFolder : kind == 2 ? sourceFolder : contentFolder; };
		// If the Source root is hidden (non-C++ project) but was last selected,
		// fall back to Content so the grid never shows a stale/empty Source view.
		if (s_selectedRootKind == 2 && !cbShowSource) { s_selectedRootKind = 0; s_selectedTreeFolder = nullptr; }

		// Drag-to-move: an asset dragged from the grid ("HE_ASSET_PATH") and
		// dropped onto a folder — a grid folder item or a tree node — records
		// the request here. It executes AFTER the draw loops (below the file
		// grid), so the folder tree is never mutated mid-iteration and the
		// Folder* pointers stay valid for the whole frame.
		static std::string s_pendingMoveSrc; // absolute path of the dragged asset
		static std::string s_pendingMoveDst; // absolute path of the target folder
		auto folderDropTarget = [&](const std::string& folderAbs)
		{
			if (!ImGui::BeginDragDropTarget()) return;
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
			{
				s_pendingMoveSrc.assign(static_cast<const char*>(p->Data));
				s_pendingMoveDst = folderAbs;
			}
			ImGui::EndDragDropTarget();
		};

		std::function<void(const HE::Folder*, int, int)> renderTree = [&](const HE::Folder* folder, int depth, int rootKind)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
									 | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (folder->subfolders.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;

			// ID by full path: sibling subtrees may repeat folder names, and the
			// drop target below must land on THIS node, not a same-named twin.
			ImGui::PushID(folder->fullPath.c_str());
			bool open = ImGui::TreeNodeEx(folder->name.c_str(), flags);
			// Same rollup the grid tiles use: one hash lookup against the
			// precomputed dirty-folder set, so the tree costs nothing extra.
			if (ctx.git && ctx.git->isRepo() && ctx.git->folderHasChanges(folder->fullPath))
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 0.95f), "•");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Contains uncommitted changes");
			}
			folderDropTarget(folder->fullPath); // move dragged assets into this folder

			// Double-click anywhere on the item → navigate grid to this folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = folder;
				s_selectedRootKind   = rootKind;
			}

			if (open)
			{
				for (auto* sub : folder->subfolders)
					renderTree(sub, depth + 1, rootKind);
				ImGui::TreePop();
			}
			ImGui::PopID();
		};

		// Root "Content" node
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_DefaultOpen
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Content", rootFlags);
			folderDropTarget(contentFolder.fullPath); // move to the content root
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to root
				s_selectedRootKind   = 0;
			}
			if (rootOpen)
			{
				for (auto* sub : contentFolder.subfolders)
					renderTree(sub, 1, 0);
				ImGui::TreePop();
			}
		}

		// Root "Engine" node — engine-wide default content (EditorDeps/EngineContent),
		// sibling of "Content". Same tree/drag-drop/create/rename/delete machinery;
		// only the backing Folder differs.
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Engine", rootFlags);
			folderDropTarget(engineFolder.fullPath); // move to the engine root
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to engine root
				s_selectedRootKind   = 1;
			}
			if (rootOpen)
			{
				for (auto* sub : engineFolder.subfolders)
					renderTree(sub, 1, 1);
				ImGui::TreePop();
			}
		}

		// Root "Source" node — the C++ project's native gameplay tree (<root>/Source),
		// sibling of Content. Only shown for C++ projects. Same tree machinery; the
		// grid groups each class's .h/.cpp into one item (see the file loop).
		if (cbShowSource)
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Source", rootFlags);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_selectedTreeFolder = nullptr; // back to source root
				s_selectedRootKind   = 2;
			}
			if (rootOpen)
			{
				for (auto* sub : sourceFolder.subfolders)
					renderTree(sub, 1, 2);
				ImGui::TreePop();
			}
		}

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

		ImGui::Button("##cb_splitter", ImVec2(splitterW, totalHeight));

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (ImGui::IsItemActive())
			ctx.cbTreeWidth += ImGui::GetIO().MouseDelta.x;

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		ImGui::SameLine();

		ImGui::BeginChild("##cb_content", ImVec2(0.0f, totalHeight), false);

		// ── Determine which folder's content to show ──────────────────────
		// s_selectedTreeFolder nullptr → root; otherwise the selected sub-folder
		static const HE::Folder* s_gridFolder = nullptr;

		// refreshContentFolder()/refreshEngineFolder() (asset create/rename/delete,
		// project load, periodic external-change poll) delete and rebuild every
		// Folder node of THEIR OWN tree, so the navigation statics above dangle
		// after each refresh — dereferencing them was a use-after-free crash when
		// creating an asset inside a sub-folder. Re-resolve the remembered path in
		// the fresh tree; if the folder no longer exists, fall back to the root.
		// Two independent version counters (Content/Engine refresh independently),
		// re-resolution triggers if EITHER changed; s_selectedIsEngine (set by the
		// tree above) says which tree to search — the two roots never share a
		// fullPath, so there is no ambiguity in picking one.
		static uint64_t    s_treeVersionSeen   = ~0ull;
		static uint64_t    s_engineVersionSeen = ~0ull;
		static uint64_t    s_sourceVersionSeen = ~0ull;
		static std::string s_gridFolderPath;
		const uint64_t treeVersion =
			ctx.globalState ? ctx.globalState->contentFolderVersion.load(std::memory_order_acquire) : 0;
		const uint64_t engineVersion =
			ctx.globalState ? ctx.globalState->engineFolderVersion.load(std::memory_order_acquire) : 0;
		const uint64_t sourceVersion =
			ctx.globalState ? ctx.globalState->sourceFolderVersion.load(std::memory_order_acquire) : 0;
		if (treeVersion != s_treeVersionSeen || engineVersion != s_engineVersionSeen ||
		    sourceVersion != s_sourceVersionSeen)
		{
			s_treeVersionSeen   = treeVersion;
			s_engineVersionSeen = engineVersion;
			s_sourceVersionSeen = sourceVersion;
			const HE::Folder* fresh = nullptr;
			if (!s_gridFolderPath.empty())
			{
				std::function<const HE::Folder*(const HE::Folder*)> findByPath =
					[&](const HE::Folder* cur) -> const HE::Folder*
				{
					if (cur->fullPath == s_gridFolderPath) return cur;
					for (const HE::Folder* sub : cur->subfolders)
						if (const HE::Folder* hit = findByPath(sub)) return hit;
					return nullptr;
				};
				const HE::Folder& searchRoot = cbRootFolder(s_selectedRootKind);
				fresh = findByPath(&searchRoot);
				if (fresh == &searchRoot) fresh = nullptr; // root is the null state
			}
			s_gridFolder         = fresh;
			s_selectedTreeFolder = fresh;
		}

		// Sync from tree double-click
		if (s_selectedTreeFolder != s_gridFolder)
			s_gridFolder = s_selectedTreeFolder;

		const HE::Folder* displayFolder = s_gridFolder ? s_gridFolder : &cbRootFolder(s_selectedRootKind);
		s_gridFolderPath = s_gridFolder ? s_gridFolder->fullPath : std::string{};

		// ── Toolbar: where you are ────────────────────────────────────────
		// A breadcrumb, not a row of tools — so it keeps its own semantics and
		// only borrows the editor's toolbar surface: the same band, the same
		// well, the same cells the Scene and Source Control bars use, so the
		// browser stops being the one panel with its own idea of a header.
		{
			namespace T = EditorToolbar;

			// Ancestor chain to the folder on screen, by DFS from the active root.
			std::vector<const HE::Folder*> crumbs;
			if (s_gridFolder)
			{
				std::function<bool(const HE::Folder*)> findPath = [&](const HE::Folder* cur) -> bool
				{
					if (cur == s_gridFolder) return true;
					for (auto* sub : cur->subfolders)
					{
						crumbs.push_back(sub);
						if (findPath(sub)) return true;
						crumbs.pop_back();
					}
					return false;
				};
				findPath(&cbRootFolder(s_selectedRootKind));
			}

			const char* rootLabel = s_selectedRootKind == 1 ? "Engine"
			                      : s_selectedRootKind == 2 ? "Source"
			                                                : "Content";

			T::Bar bar;
			bar.group();
			if (bar.item("##bc_root", T::iconFolder, rootLabel, crumbs.empty(), true,
			             "Back to the top of this root"))
			{
				s_gridFolder         = nullptr;
				s_selectedTreeFolder = nullptr;
			}
			for (int ci = 0; ci < static_cast<int>(crumbs.size()); ++ci)
			{
				const HE::Folder*  crumb  = crumbs[ci];
				const bool         isLast = (ci == static_cast<int>(crumbs.size()) - 1);
				const std::string  id     = "##bc_" + std::to_string(ci);
				// The folder you are IN is armed rather than disabled: it is where
				// you are, not something that failed to be available.
				if (bar.item(id.c_str(), nullptr, crumb->name.c_str(), isLast, !isLast,
				             isLast ? nullptr : "Go up to this folder"))
				{
					s_gridFolder         = crumb;
					s_selectedTreeFolder = crumb;
				}
			}
			bar.endGroup();
		}

		// ── Grid ──────────────────────────────────────────────────────────
		constexpr float k_cellSize    = 72.0f;
		constexpr float k_iconPad     = 6.0f;   // padding inside ImageButton
		constexpr float k_iconSize    = k_cellSize - k_iconPad * 2.0f;
		constexpr float k_padding     = 8.0f;

		// ── Icon + tint for one grid item ─────────────────────────────────
		// Keyed on what the file IS rather than what it is called. Engine assets
		// are all named ".hasset", so the old extension-only lookup matched none
		// of them and every one rendered as a blank button — the cached HAsset
		// header sniff is what actually identifies them. Loose source files
		// (.png/.obj/.lua/…) are not HAssets and fall through to the extension
		// map below, which now exists only for them.
		//
		// The tint travels WITH the icon instead of being recovered afterwards by
		// comparing texture handles: related types share a hue (meshes blue,
		// material-ish green, scripting yellow, animation pink, input steel) and
		// that grouping is only legible if the colour sits next to the type.
		struct AssetVisual { ImTextureID icon = 0; ImVec4 tint{ 0.75f, 0.85f, 1.0f, 1.0f }; };
		auto pickAssetVisual = [&](const HE::File* file) -> AssetVisual
		{
			const auto& I = ctx.cbIcons;
			switch (EditorAssetTypeCache::assetTypeOf(file->fullPath))
			{
				case HE::AssetType::StaticMesh:          return { I.model3d,              {0.70f, 0.80f, 1.00f, 1.0f} };
				case HE::AssetType::SkeletalMesh:        return { I.model3d,              {0.62f, 0.86f, 1.00f, 1.0f} };
				case HE::AssetType::Prefab:              return { I.prefab,               {0.58f, 0.78f, 1.00f, 1.0f} };
				case HE::AssetType::Material:            return { I.material,             {0.60f, 0.90f, 0.60f, 1.0f} };
				case HE::AssetType::MaterialFunction:    return { I.materialFunction,     {0.55f, 0.88f, 0.75f, 1.0f} };
				case HE::AssetType::Shader:              return { I.shader,               {0.50f, 0.88f, 0.82f, 1.0f} };
				case HE::AssetType::Texture:             return { I.texture,              {0.90f, 0.75f, 0.60f, 1.0f} };
				case HE::AssetType::ParticleSystem:      return { I.particleSystem,       {1.00f, 0.82f, 0.55f, 1.0f} };
				case HE::AssetType::Scene:               return { I.scene,                {0.75f, 0.65f, 1.00f, 1.0f} };
				case HE::AssetType::Widget:              return { I.widget,               {0.82f, 0.72f, 1.00f, 1.0f} };
				case HE::AssetType::Script:              return { I.script,               {0.90f, 0.90f, 0.50f, 1.0f} };
				case HE::AssetType::HorizonCodeClass:    return { I.horizonCodeClass,     {0.95f, 0.85f, 0.55f, 1.0f} };
				case HE::AssetType::AnimationClip:       return { I.animationClip,        {1.00f, 0.70f, 0.85f, 1.0f} };
				case HE::AssetType::PropertyAnimClip:    return { I.propertyAnimClip,     {0.96f, 0.76f, 0.94f, 1.0f} };
				case HE::AssetType::AnimatorStateMachine:return { I.animatorStateMachine, {0.98f, 0.72f, 0.78f, 1.0f} };
				case HE::AssetType::InputAction:         return { I.inputAction,          {0.70f, 0.88f, 0.96f, 1.0f} };
				case HE::AssetType::InputMappingContext: return { I.inputMappingContext,  {0.60f, 0.82f, 0.96f, 1.0f} };
				case HE::AssetType::Audio:               return { I.sound,                {0.60f, 0.90f, 0.90f, 1.0f} };
				case HE::AssetType::Font:                return { I.font,                 {0.92f, 0.88f, 0.80f, 1.0f} };
				// User-defined types share the HC-class glyph, tinted apart until
				// they earn their own .tga.
				case HE::AssetType::StructType:          return { I.horizonCodeClass,     {0.60f, 0.95f, 0.80f, 1.0f} };
				case HE::AssetType::EnumType:            return { I.horizonCodeClass,     {0.80f, 0.95f, 0.60f, 1.0f} };
				case HE::AssetType::SaveGameTemplate:    return { I.horizonCodeClass,     {0.95f, 0.85f, 0.95f, 1.0f} };
				case HE::AssetType::Unknown: break; // not an HAsset — try the extension
			}

			std::string e = file->extension;
			for (auto& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			if (e == ".mat")                                   return { I.material, {0.60f, 0.90f, 0.60f, 1.0f} };
			if (e == ".obj" || e == ".fbx" || e == ".gltf"
				|| e == ".glb" || e == ".dae")                 return { I.model3d,  {0.70f, 0.80f, 1.00f, 1.0f} };
			if (e == ".svg" || e == ".ai")                     return { I.model2d,  {0.80f, 0.70f, 1.00f, 1.0f} };
			if (e == ".cs"  || e == ".lua" || e == ".py"
				|| e == ".js")                                  return { I.script,   {0.90f, 0.90f, 0.50f, 1.0f} };
			if (e == ".h"   || e == ".hpp" || e == ".hh" || e == ".hxx"
				|| e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c")
				                                                return { I.script,   {0.90f, 0.90f, 0.50f, 1.0f} };
			if (e == ".wav" || e == ".mp3" || e == ".ogg"
				|| e == ".flac")                                return { I.sound,    {0.60f, 0.90f, 0.90f, 1.0f} };
			if (e == ".png" || e == ".jpg" || e == ".jpeg"
				|| e == ".bmp" || e == ".tga" || e == ".hdr")   return { I.texture,  {0.90f, 0.75f, 0.60f, 1.0f} };
			if (e == ".hescene")                               return { I.scene,    {0.75f, 0.65f, 1.00f, 1.0f} };
			if (e == ".ttf" || e == ".otf")                    return { I.font,     {0.92f, 0.88f, 0.80f, 1.0f} };
			return {};
		};

		// ── Rendered thumbnails for mesh/material assets ──────────────────
		// Point the cache at the loaded project (a no-op after the first frame)
		// and refill its per-frame budget, then let each tile ask for its image.
		// A miss just falls back to the extension icon below.
		if (ctx.projectManager)
		{
			const std::string& projFile = ctx.projectManager->currentProject().path;
			const std::string cacheDir = projFile.empty() ? std::string{}
				: (std::filesystem::path(projFile).parent_path() / "Saved" / "Thumbnails").string();
			AssetThumbnailCache::setContext(ctx.renderer, ctx.contentManager, cacheDir);
		}
		AssetThumbnailCache::beginFrame(ImGui::GetTime());

		// Column count from the HORIZONTAL stride only: an item is k_cellSize
		// wide (icon button = icon + 2×frame padding) with k_padding ItemSpacing
		// between columns; the label renders BELOW the icon and adds height, not
		// width. N items fit when N*cell + (N-1)*spacing <= avail.
		const float availW     = ImGui::GetContentRegionAvail().x;
		const int   columns    = (std::max)(1, static_cast<int>(
			(availW + k_padding) / (k_cellSize + k_padding)));

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(k_padding, k_padding));

		int col = 0;

		// ── Shared selection/rename/context state ────────────────────────
		static std::string s_selectedItem;
		static bool        s_selectedIsFolder = false;
		static std::string s_ctxMenuItem;
		static bool        s_ctxMenuIsFolder  = false;
		static std::string s_renameTarget;
		static bool        s_renameIsFolder   = false;
		static char        s_renameBuf[256]   = {};
		static bool        s_openRenamePopup  = false;
		static bool        s_renameIsCreate   = false; // naming a freshly created item
		// A created asset is announced to the session when its name is FINAL,
		// not when the file appears. tryCreate writes "NewMaterial.hasset" and
		// then opens the naming dialog; publishing at the write would put the
		// placeholder on every peer and, a second later, a rename — two events
		// for one user action, and the rename half needs the host's approval.
		// Holds the path from the write until the dialog resolves, whichever way
		// it resolves.
		static std::string s_pendingCreatePublish;
		// A folder travels a different way — it has no bytes — so the announcing
		// block has to know which of the two it is holding.
		static bool        s_pendingCreateIsFolder = false;
		static int         s_renameScriptLang = -1;    // creating a script: 0=Lua 1=Python; -1=not a script
		static bool        s_rightClickOnItem = false;
		// Delete-folder confirmation: the dialog outlives the frame its context
		// menu closed in, so the target and the list of files that go with it are
		// kept as STRINGS — a Folder* would dangle across the content refresh
		// (same use-after-free the grid navigation statics above guard against).
		static std::string              s_deleteFolderTarget;
		static std::vector<std::string> s_deleteFolderFiles;
		static bool                     s_openDeleteFolderPopup = false;
		// A single asset used to go on one click of a red menu item — no dialog,
		// no undo, and nothing on screen afterwards to say what had been there.
		// Same STRING-not-pointer rule as the folder case: this outlives the
		// frame the popup is opened on.
		static std::string              s_deleteAssetTarget;
		static bool                     s_deleteAssetIsSource = false;   // .h/.cpp pair
		static bool                     s_openDeleteAssetPopup = false;
#ifdef HE_HAVE_LIBSSH2
		// Remote-download confirmation: same "outlives the popup's opening frame,
		// so keep it as plain data, never a File*" reasoning as the delete-folder
		// state above — a content refresh (which the download itself triggers)
		// would dangle a pointer into the old tree.
		static std::string s_remoteDownloadRelativePath;
		static HE::UUID    s_remoteDownloadUuid;
		static std::string s_remoteDownloadTabLabel;
		static std::string s_remoteDownloadFullPath;
		static bool        s_openRemoteDownloadPopup = false;
		// A completed download's onComplete callback (see the confirmation popup
		// below) runs on a WORKER thread and must not touch ctx.tabs directly —
		// it drops the path here instead, and the drain right below (main thread,
		// once per frame) does the actual opening. Same shape as ContentManager's
		// AsyncSink: producer on a worker, consumer on the main thread, never both.
		static std::mutex               s_pendingOpenMutex;
		static std::vector<std::string> s_pendingOpenFullPaths;
#endif

		// Opens (or focuses) a tab for `fullPath`, exactly what a double-click on a
		// known asset type does. Factored out so a completed EngineContent download
		// can auto-open the tab the user originally asked for, without duplicating
		// the type dispatch below.
		auto openAssetTab = [&](const std::string& fullPath)
		{
			if (std::filesystem::path(fullPath).extension() == ".hescene")
			{
				openSceneGuarded(fullPath);
				return;
			}
			// Script assets open the code editor tab, material assets the node-graph
			// editor tab. Other asset types have no dedicated editor yet → no-op.
			// C++ source/header (Source root) opens the h/cpp class viewer; a raw
			// .wav opens the audio tab (auditioning a source file must not require
			// importing it first). Both predicates are raw extension checks, not
			// HAsset sniffs, so they must be tested explicitly here.
			if (!(CppClassEditorPanel::isCppSourceAsset(fullPath) ||
			      AudioEditorPanel::isAudioAsset(fullPath) ||
			      ScriptEditorPanel::isScriptAsset(fullPath) ||
			      MaterialEditorPanel::isMaterialAsset(fullPath) ||
			      MaterialEditorPanel::isMaterialFunctionAsset(fullPath) ||
			      UIEditorPanel::isWidgetAsset(fullPath) ||
			      HorizonCodeClassPanel::isClassAsset(fullPath) ||
			      InputAssetPanel::isInputAsset(fullPath) ||
			      TypeAssetPanel::isTypeAsset(fullPath) ||
			      SkeletalMeshEditorPanel::isSkeletalMeshAsset(fullPath) ||
			      StaticMeshEditorPanel::isStaticMeshAsset(fullPath) ||
			      ParticleGraphEditorPanel::isParticleAsset(fullPath) ||
			      AnimatorStateMachineEditorPanel::isAnimatorStateMachineAsset(fullPath)))
				return; // no dedicated editor for this type — same no-op the old inline dispatch had

			const std::string tabLabel = std::filesystem::path(fullPath).stem().string();
			auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
				[&](const AppContext::EditorTab& t){ return t.assetPath == fullPath; });
			if (it == ctx.tabs.end())
			{
				ctx.tabs.push_back({ tabLabel, fullPath, true, true });
				ctx.activeTab = static_cast<int>(ctx.tabs.size()) - 1;
			}
			else
			{
				ctx.activeTab = static_cast<int>(std::distance(ctx.tabs.begin(), it));
			}
			// Force the tab bar to select this tab next frame (else ImGui keeps the
			// Scene tab selected and the editor never opens).
			tabSelectRequest = ctx.activeTab;
		};

#ifdef HE_HAVE_LIBSSH2
		// Drain downloads that finished since the last frame — once per frame,
		// before the grid loop, so it can never re-enter mid-iteration.
		{
			std::vector<std::string> ready;
			{
				std::lock_guard<std::mutex> lock(s_pendingOpenMutex);
				ready.swap(s_pendingOpenFullPaths);
			}
			for (const std::string& p : ready) openAssetTab(p);
		}
#endif
		// Name-a-C++-class popup (C++ projects only): the create menu stages a
		// class name here, then this popup writes Source/<Name>.{h,cpp}.
		static bool        s_openCppClassPopup = false;
		static char        s_cppClassName[128] = "GameplayClass";

		// ── Folders first ─────────────────────────────────────────────────
		for (auto* sub : displayFolder->subfolders)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == sub->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(sub->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			if (ctx.cbIcons.folder)
			{
				ImGui::ImageButton("##icon", ctx.cbIcons.folder,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0),
					ImVec4(0.96f, 0.78f, 0.26f, 1.0f));
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}
			// A folder with changes somewhere beneath it gets a small dot, so a
			// user can find modified assets without opening every folder. One hash
			// lookup: the rollup is precomputed on the worker rather than walked
			// per tile per frame.
			if (ctx.git && ctx.git->isRepo() && ctx.git->folderHasChanges(sub->fullPath))
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 4.5f;
				const ImVec2 c(mn.x + r + 4.0f, mx.y - r - 4.0f);
				dl->AddCircleFilled(c, r, IM_COL32(255, 200, 90, 235));
				dl->AddCircle(c, r, IM_COL32(20, 20, 22, 220), 0, 1.0f);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("Contains uncommitted changes");
			}

			folderDropTarget(sub->fullPath); // move dragged assets into this folder

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
			}
			// Double-click → navigate into folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_gridFolder         = sub;
				s_selectedTreeFolder = sub;
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
				s_ctxMenuItem      = sub->fullPath;
				s_ctxMenuIsFolder  = true;
				s_rightClickOnItem = true;
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
			ImGui::PopID();

			// Centered label (truncated)
			const float labelW = k_cellSize;
			std::string label  = sub->name;
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		// ── Files ─────────────────────────────────────────────────────────
		// In the Source root a class's header and .cpp are ONE item: drop a .cpp
		// when a same-stem header sits in the same folder, so the header stands in
		// for the pair (double-click opens both in the C++ viewer). Lone .cpp files
		// (e.g. GameLogic.cpp) and non-C++ files pass through unchanged.
		auto cbLowerExt = [](const std::string& e){ std::string s=e; for(auto&c:s) c=(char)::tolower((unsigned char)c); return s; };
		auto cbIsHeaderExt = [&](const std::string& e){ std::string s=cbLowerExt(e); return s==".h"||s==".hpp"||s==".hh"||s==".hxx"; };
		auto cbIsSourceExt = [&](const std::string& e){ std::string s=cbLowerExt(e); return s==".cpp"||s==".cc"||s==".cxx"||s==".c"; };
		std::vector<const HE::File*> gridFiles;
		gridFiles.reserve(displayFolder->files.size());
		for (auto* f : displayFolder->files)
		{
			if (s_selectedRootKind == 2 && cbIsSourceExt(f->extension))
			{
				const std::string stem = std::filesystem::path(f->name).stem().string();
				bool headerSibling = false;
				for (auto* g : displayFolder->files)
					if (cbIsHeaderExt(g->extension) &&
					    std::filesystem::path(g->name).stem().string() == stem)
					{ headerSibling = true; break; }
				if (headerSibling) continue; // collapsed into its header item
			}
			gridFiles.push_back(f);
		}

		for (auto* file : gridFiles)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == file->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(file->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			// A rendered thumbnail wins over the generic icon and is drawn untinted,
			// since its colours ARE the information. Types without one — and assets
			// whose tile has not been rendered yet — keep the per-type glyph.
			ImTextureID thumb = reinterpret_cast<ImTextureID>(
				AssetThumbnailCache::get(file->fullPath));
			const AssetVisual vis = thumb ? AssetVisual{ thumb, ImVec4(1, 1, 1, 1) }
			                              : pickAssetVisual(file);
			if (vis.icon)
			{
				ImGui::ImageButton("##icon", vis.icon,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0), vis.tint);
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			// A material FUNCTION renders as the sphere its own editor tab shows —
			// which is exactly a material's tile. The corner badge is what tells
			// the two apart at a glance; drawn over the tile rather than baked into
			// the image so the cached thumbnail stays a plain picture of the shader.
			if (thumb && EditorAssetTypeCache::is(file->fullPath, HE::AssetType::MaterialFunction))
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 9.0f;
				const ImVec2 c(mx.x - r - 3.0f, mn.y + r + 3.0f);
				dl->AddCircleFilled(c, r, IM_COL32(24, 26, 24, 225));
				dl->AddCircle(c, r, IM_COL32(150, 225, 190, 235), 0, 1.5f);
				const ImVec2 ts = ImGui::CalcTextSize("f");
				dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
				            IM_COL32(180, 240, 210, 255), "f");
			}

			// EngineContent SFTP remote-only badge, TOP-left (the material-function
			// badge owns the top-right, the git-status badge owns the bottom-left —
			// see below). A plain downward-triangle-in-a-circle rather than a text
			// glyph, so it never depends on the loaded font's glyph ranges.
			if (file->isRemoteOnly)
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const float r = 9.0f;
				const ImVec2 c(mn.x + r + 3.0f, mn.y + r + 3.0f);
				dl->AddCircleFilled(c, r, IM_COL32(24, 26, 30, 225));
				dl->AddCircle(c, r, IM_COL32(120, 170, 230, 235), 0, 1.5f);
				const float tr = 3.5f;
				dl->AddTriangleFilled(
					ImVec2(c.x - tr, c.y - tr * 0.6f),
					ImVec2(c.x + tr, c.y - tr * 0.6f),
					ImVec2(c.x,      c.y + tr * 0.9f),
					IM_COL32(150, 195, 240, 255));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					ImGui::SetTooltip("On the EngineContent server — double-click to download");
			}

			// Source-control status, drawn in the BOTTOM-left so it cannot collide
			// with the material-function badge in the top-right. Same technique:
			// over the tile via the item rect, so the cached thumbnail stays a
			// plain picture.
			//
			// Looked up by absolute path rather than by caching a pointer into the
			// status map — the Content Browser rebuilds its File* nodes wholesale
			// on every refresh, so anything held across a frame dangles.
			if (ctx.git && ctx.git->isRepo())
			{
				if (const HE::Sc::FileEntry* e = ctx.git->entryForAbsolutePath(file->fullPath))
				{
					if (e->dirty())
					{
						// The worktree state is what the user is looking at; the
						// staged state matters in the panel, not on a tile.
						const HE::Sc::FileState st =
							(e->worktree != HE::Sc::FileState::Unmodified) ? e->worktree : e->index;
						const char* glyph = "?";
						ImU32 colour = IM_COL32(170, 170, 170, 255);
						switch (st)
						{
						case HE::Sc::FileState::Added:      glyph = "+"; colour = IM_COL32(140, 215, 140, 255); break;
						case HE::Sc::FileState::Modified:   glyph = "M"; colour = IM_COL32(255, 200, 90,  255); break;
						case HE::Sc::FileState::Deleted:    glyph = "-"; colour = IM_COL32(240, 130, 115, 255); break;
						case HE::Sc::FileState::Renamed:
						case HE::Sc::FileState::Copied:     glyph = "R"; colour = IM_COL32(150, 190, 255, 255); break;
						case HE::Sc::FileState::Conflicted: glyph = "!"; colour = IM_COL32(255, 100, 100, 255); break;
						case HE::Sc::FileState::Untracked:  glyph = "?"; colour = IM_COL32(170, 170, 170, 255); break;
						default: break;
						}
						const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
						ImDrawList* dl = ImGui::GetWindowDrawList();
						const float r = 8.0f;
						const ImVec2 c(mn.x + r + 3.0f, mx.y - r - 3.0f);
						dl->AddCircleFilled(c, r, IM_COL32(20, 20, 22, 230));
						dl->AddCircle(c, r, colour, 0, 1.5f);
						const ImVec2 ts = ImGui::CalcTextSize(glyph);
						dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), colour, glyph);

						// Words on hover, not just a letter: the glyph alphabet
						// is git's, and not everyone reads it.
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
						{
							const char* what = "Changed";
							switch (st)
							{
							case HE::Sc::FileState::Added:      what = "Added — staged for the next commit"; break;
							case HE::Sc::FileState::Modified:   what = "Modified since the last commit"; break;
							case HE::Sc::FileState::Deleted:    what = "Deleted"; break;
							case HE::Sc::FileState::Renamed:
							case HE::Sc::FileState::Copied:     what = "Renamed"; break;
							case HE::Sc::FileState::Conflicted: what = "Merge conflict — resolve before committing"; break;
							case HE::Sc::FileState::Untracked:  what = "Not in source control yet"; break;
							default: break;
							}
							ImGui::SetTooltip("%s", what);
						}
					}
				}
			}

			// ── Somebody is editing this asset right now ──────────────────
			// The lock table already knows; until now it only showed up once
			// you had opened the asset and found the tab read-only. On the tile
			// it is worth something else entirely: you see it BEFORE you invest
			// in opening it, and you can go and ask them instead.
			//
			// Drawn in the holder's own colour — the same one their cursor,
			// their presence dot and the outliner use, so "orange means Anna"
			// holds everywhere rather than being learned per panel.
			if (ctx.collab && ctx.collab->inSession() && ctx.contentManager && file)
			{
				const std::string rel =
					ctx.contentManager->toContentRelativePath(file->fullPath);
				const HE::Net::LockInfo* lock =
					rel.empty() ? nullptr : ctx.collab->assetLockInfo(rel);
				if (lock && lock->owner != ctx.collab->localParticipant())
				{
					float rgb[3] = { 1.0f, 0.75f, 0.3f };
					ctx.collab->colorFor(lock->owner, rgb);
					const ImU32 col = IM_COL32(int(rgb[0] * 255), int(rgb[1] * 255),
					                           int(rgb[2] * 255), 255);
					const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
					ImDrawList* dl = ImGui::GetWindowDrawList();
					// Top-right, clear of the git dot at the bottom-left and the
					// download badge at the top-left: three different facts about
					// one file, three corners, none of them hiding another.
					const float r = 8.0f;
					const ImVec2 c(mx.x - r - 3.0f, mn.y + r + 3.0f);
					dl->AddCircleFilled(c, r, IM_COL32(20, 20, 22, 230));
					dl->AddCircle(c, r, col, 0, 1.5f);
					// A padlock as two shapes rather than a glyph: the font's
					// glyph ranges are not guaranteed to carry one, and a missing
					// glyph would draw an empty box that means nothing.
					dl->AddRectFilled(ImVec2(c.x - 3.5f, c.y - 0.5f),
					                  ImVec2(c.x + 3.5f, c.y + 4.0f), col, 1.0f);
					constexpr float kPi = 3.14159265f;   // IM_PI is imgui_internal
					dl->PathArcTo(ImVec2(c.x, c.y - 0.5f), 2.6f, kPi, kPi * 2.0f, 12);
					dl->PathStroke(col, 0, 1.4f);

					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					{
						ImGui::SetTooltip("%s is editing this — it is read-only for you.",
							lock->ownerName.empty() ? "Someone" : lock->ownerName.c_str());
					}
				}
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);

			// Drag source — carries the asset's absolute path so drop targets
			// (e.g. the Material slot in the inspector) can load it. Disabled in the
			// Source root: a class item stands for a .h/.cpp pair, and moving just
			// the one file would orphan its sibling.
			if (s_selectedRootKind != 2 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				ImGui::SetDragDropPayload("HE_ASSET_PATH",
					file->fullPath.c_str(), file->fullPath.size() + 1);
				ImGui::TextUnformatted(
					std::filesystem::path(file->name).stem().string().c_str());
				ImGui::EndDragDropSource();
			}

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
			}
			// Double-click → open tab (or, for an EngineContent asset that only
			// exists on the SFTP server so far, ask before downloading it — see
			// the "##cb_remote_download_popup" modal below).
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
#ifdef HE_HAVE_LIBSSH2
				if (file->isRemoteOnly)
				{
					s_remoteDownloadRelativePath = engineRelativePath(file->fullPath);
					s_remoteDownloadUuid         = file->remoteUuid;
					s_remoteDownloadTabLabel     = std::filesystem::path(file->name).stem().string();
					s_remoteDownloadFullPath     = file->fullPath;
					s_openRemoteDownloadPopup    = true;
				}
				else
#endif
					openAssetTab(file->fullPath);
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
				s_ctxMenuItem      = file->fullPath;
				s_ctxMenuIsFolder  = false;
				s_rightClickOnItem = true;
			}

			ImGui::PopID();

			// Centered label (stem only, truncated)
			const float labelW = k_cellSize;
			std::string label  = std::filesystem::path(file->name).stem().string();
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		ImGui::PopStyleVar();

		// ── Execute a drag-to-move recorded by a folder drop target ───────
		// Same operation as the rename popup's OK handler (a rename IS a move
		// within one folder): plain disk rename + selection/tab fixups + a
		// quiet content refresh. Skipped when the drop lands on the asset's
		// own folder or the name already exists in the target.
		if (!s_pendingMoveSrc.empty() && !s_pendingMoveDst.empty())
		{
			const std::filesystem::path src(s_pendingMoveSrc);
			const std::filesystem::path dst =
				std::filesystem::path(s_pendingMoveDst) / src.filename();
			std::error_code ec;
			const bool sameFolder =
				std::filesystem::equivalent(src.parent_path(), s_pendingMoveDst, ec);
			// Engine defaults/overrides don't move via drag in normal mode —
			// same "read-only ground" rule as create/rename/delete above.
			const bool engineLocked = ctx.contentManager && !ContentManager::isEngineContentDevMode() &&
				(ctx.contentManager->isEngineDefaultPath(s_pendingMoveSrc)  ||
				 ctx.contentManager->isEngineOverridePath(s_pendingMoveSrc) ||
				 ctx.contentManager->isEngineDefaultPath(s_pendingMoveDst)  ||
				 ctx.contentManager->isEngineOverridePath(s_pendingMoveDst));
			if (!engineLocked && !sameFolder && std::filesystem::exists(src) &&
			    !std::filesystem::exists(dst))
			{
				ec.clear();
				std::filesystem::rename(src, dst, ec);
				if (!ec)
				{
					// Everything that pointed at the old path — other assets' stored
					// references and the asset's own embedded path — follows it here.
					retargetReferences(ctx, s_pendingMoveSrc, dst.string(), /*folder=*/false);
					// Same reasoning as the rename popup: the asset left one path and
					// landed on another, so both cached type sniffs are now lies.
					EditorAssetTypeCache::invalidate(s_pendingMoveSrc);
					EditorAssetTypeCache::invalidate(dst.string());
					// The thumbnail is keyed by the asset's path, so the old key would
					// keep a .hthumb nobody can reach again.
					AssetThumbnailCache::invalidate(s_pendingMoveSrc);
					AssetThumbnailCache::invalidate(dst.string());
					if (s_selectedItem == s_pendingMoveSrc)
						s_selectedItem = dst.string();
					for (auto& t : ctx.tabs)
						if (t.assetPath == s_pendingMoveSrc)
							t.assetPath = dst.string();
					s_quietContentRefresh = true;
				}
			}
			s_pendingMoveSrc.clear();
			s_pendingMoveDst.clear();
		}

		// ── Open item context menu after loops ────────────────────────────
		// ── Shared "Create Asset" menu body ─────────────────────────────────
		// Used by BOTH the background right-click popup and the item context
		// menu's Create submenu, so creating an asset works wherever the user
		// right-clicks in the Content Browser.
		auto drawCreateAssetItems = [&](const std::string& targetFolder)
		{
			auto tryCreate = [&](const char* defaultName, const char* ext, HE::AssetType type,
			                     HE::ScriptLanguage scriptLang = HE::ScriptLanguage::Lua,
			                     const char* hcBaseClass = nullptr)
			{
				// Build a path that does not yet exist
				std::string base = targetFolder + "/" + defaultName;
				std::string path = base + ext;
				int counter = 1;
				while (std::filesystem::exists(path))
					path = base + std::to_string(counter++) + ext;

				// Create an empty asset file via ContentManager. Root-aware (Content
				// vs. reserved "Engine/" namespace) so an asset created inside the
				// Engine tree gets a path other panels' pickers can resolve back.
				std::string relative = ctx.contentManager->toContentRelativePath(path);

				// Write a minimal binary asset stub so the file exists on disk.
				// The UUID minted here is the asset's permanent identity.
				{
					const HE::UUID assetId = HE::UUID::generate();
					HAsset::Writer w;
					std::vector<uint8_t> meta;
					HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
					HAsset::Writer::appendPOD(meta, assetId.hi);
					HAsset::Writer::appendPOD(meta, assetId.lo);
					HAsset::Writer::appendString(meta, defaultName);
					HAsset::Writer::appendString(meta, relative);
					w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
					// Scripts are born with a language and a starter template. The
					// language byte (CHUNK_SLNG) is the single source of truth for
					// routing Lua vs Python, so it must be written here at birth —
					// this stub bypasses the ContentManager save path.
					if (type == HE::AssetType::Script)
					{
						const int lang = static_cast<int>(scriptLang);
						const char* starter = scriptStarterTemplate(lang);
						w.addChunk(HAsset::CHUNK_SRC, starter, std::char_traits<char>::length(starter));
						const uint8_t lb = static_cast<uint8_t>(lang);
						w.addChunk(HAsset::CHUNK_SLNG, &lb, 1);
					}
					// UI widgets are born with an empty 1920×1080 tree so the widget
					// editor has valid JSON to open straight away.
					if (type == HE::AssetType::Widget)
					{
						const std::string tree = HE::uiWidgetTreeToJson(HE::UIWidgetTree{});
						w.addChunk(HAsset::CHUNK_UIWT, tree.data(), tree.size());
					}
					if (type == HE::AssetType::HorizonCodeClass)
					{
						const std::string graph = HorizonCode::toJson(HorizonCode::Graph{});
						w.addChunk(HAsset::CHUNK_HCGR, graph.data(), graph.size());
						// The base class decides the event catalog (e.g. input events on
						// PlayerController/PlayerCharacter), so it is part of the asset's
						// identity from birth. Absent chunk = plain Object.
						if (hcBaseClass && *hcBaseClass)
							w.addChunk(HAsset::CHUNK_HCBC, hcBaseClass, std::strlen(hcBaseClass));
					}
					// Input assets are born with valid minimal JSON so their editors and
					// the runtime parser never see an empty payload.
					if (type == HE::AssetType::InputAction)
					{
						const char* json = "{\"valueType\":\"Button\"}";
						w.addChunk(HAsset::CHUNK_IACT, json, std::strlen(json));
					}
					if (type == HE::AssetType::InputMappingContext)
					{
						const char* json = "{\"entries\":[]}";
						w.addChunk(HAsset::CHUNK_IMAP, json, std::strlen(json));
					}
					// Type-definition assets are born with valid empty JSON so the
					// TypeAssetPanel and the TypeRegistry never see an empty payload.
					if (type == HE::AssetType::StructType)
					{
						const char* json = "{\"fields\":[]}";
						w.addChunk(HAsset::CHUNK_STDF, json, std::strlen(json));
					}
					if (type == HE::AssetType::EnumType)
					{
						const char* json = "{\"entries\":[]}";
						w.addChunk(HAsset::CHUNK_ENDF, json, std::strlen(json));
					}
					if (type == HE::AssetType::SaveGameTemplate)
					{
						const char* json = "{\"fields\":[]}";
						w.addChunk(HAsset::CHUNK_SGTP, json, std::strlen(json));
					}
					w.write(path, static_cast<uint16_t>(type));
				}
				// A path that was probed while it was still free (or held a deleted
				// asset) has a stale entry in the shared type cache — this asset is
				// the one that decides its type now.
				EditorAssetTypeCache::invalidate(path);

				// Show it now (don't wait for the next auto-refresh) and let the
				// user name it straight away via the rename/name dialog.
				s_selectedItem    = path;
				s_renameTarget    = path;
				s_renameIsFolder  = false;
				s_renameIsCreate  = true;
				// Announced once the dialog below settles on the final name.
				s_pendingCreatePublish  = path;
				s_pendingCreateIsFolder = false;
				// A script is born in the project's fixed language (Lua or Python) —
				// no per-asset language picker, so this stays -1 (combo hidden).
				s_renameScriptLang = -1;
				std::strncpy(s_renameBuf, defaultName, sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			};

			if (ImGui::MenuItem("Scene"))        tryCreate("NewScene",    ".hescene", HE::AssetType::Scene);
			if (ImGui::MenuItem("Material"))     tryCreate("NewMaterial", ".hasset",  HE::AssetType::Material);
			if (ImGui::MenuItem("Material Function")) tryCreate("NewMaterialFunction", ".hasset", HE::AssetType::MaterialFunction);
			if (ImGui::MenuItem("Particle System")) tryCreate("NewParticleSystem", ".hasset", HE::AssetType::ParticleSystem);
			if (ImGui::MenuItem("Animator State Machine")) tryCreate("NewStateMachine", ".hasset", HE::AssetType::AnimatorStateMachine);
			if (ImGui::MenuItem("UI Widget"))    tryCreate("NewWidget",   ".hasset",  HE::AssetType::Widget);
			if (ImGui::MenuItem("Input Action"))          tryCreate("NewInputAction",  ".hasset", HE::AssetType::InputAction);
			if (ImGui::MenuItem("Input Mapping Context")) tryCreate("NewInputMapping", ".hasset", HE::AssetType::InputMappingContext);
			// Type definitions are language-neutral (savegame templates, script
			// constants, C++ codegen all consume them) — offered in every project.
			if (ImGui::MenuItem("Struct"))       tryCreate("NewStruct",    ".hasset",  HE::AssetType::StructType);
			if (ImGui::MenuItem("Enum"))         tryCreate("NewEnum",      ".hasset",  HE::AssetType::EnumType);
			if (ImGui::MenuItem("SaveGame Template")) tryCreate("NewSaveTemplate", ".hasset", HE::AssetType::SaveGameTemplate);
			if (ImGui::MenuItem("Texture"))      tryCreate("NewTexture",  ".hasset",  HE::AssetType::Texture);
			if (ImGui::MenuItem("Static Mesh"))  tryCreate("NewMesh",     ".hasset",  HE::AssetType::StaticMesh);
			if (ImGui::MenuItem("Skeletal Mesh"))tryCreate("NewSkelMesh", ".hasset",  HE::AssetType::SkeletalMesh);

			// ── Gameplay logic — restricted to the project's chosen language ──────
			// The project's scriptLanguage (picked in the New Project wizard) is
			// authoritative: only the matching logic creator appears, so a project
			// stays single-language. HorizonCode → class/player graph assets; Lua/
			// Python → a Script asset born in that language (no per-asset picker);
			// C++ → a native source class under Source/ (see the C++ Class popup).
			const ProjectScriptLanguage projLang = ctx.projectManager
				? ctx.projectManager->currentProject().scriptLanguage
				: ProjectScriptLanguage::HorizonCode;
			switch (projLang)
			{
			case ProjectScriptLanguage::HorizonCode:
				if (ImGui::MenuItem("HorizonCode Class")) tryCreate("NewClass", ".hasset", HE::AssetType::HorizonCodeClass);
				if (ImGui::BeginMenu("HorizonCode Player"))
				{
					if (ImGui::MenuItem("Player Controller"))
						tryCreate("NewPlayerController", ".hasset", HE::AssetType::HorizonCodeClass, HE::ScriptLanguage::Lua, "PlayerController");
					if (ImGui::MenuItem("Player Character"))
						tryCreate("NewPlayerCharacter", ".hasset", HE::AssetType::HorizonCodeClass, HE::ScriptLanguage::Lua, "PlayerCharacter");
					ImGui::EndMenu();
				}
				break;
			case ProjectScriptLanguage::Lua:
				if (ImGui::MenuItem("Script")) tryCreate("NewScript", ".hasset", HE::AssetType::Script, HE::ScriptLanguage::Lua);
				break;
			case ProjectScriptLanguage::Python:
				if (ImGui::MenuItem("Script")) tryCreate("NewScript", ".hasset", HE::AssetType::Script, HE::ScriptLanguage::Python);
				break;
			case ProjectScriptLanguage::Cpp:
				if (ImGui::MenuItem("C++ Class"))
				{
					std::strncpy(s_cppClassName, "GameplayClass", sizeof(s_cppClassName) - 1);
					s_cppClassName[sizeof(s_cppClassName) - 1] = '\0';
					s_openCppClassPopup = true;
					ImGui::CloseCurrentPopup();
				}
				break;
			}

			if (ImGui::MenuItem("Shader"))       tryCreate("NewShader",   ".hasset",  HE::AssetType::Shader);
			if (ImGui::MenuItem("Audio"))        tryCreate("NewAudio",    ".hasset",  HE::AssetType::Audio);
			if (ImGui::MenuItem("Font"))         tryCreate("NewFont",     ".hasset",  HE::AssetType::Font);
		};

		// ── Delete one asset ──────────────────────────────────────────────
		// Split out from the menu item so the confirmation dialog and the actual
		// deletion are separate things — the menu item now only asks.
		auto deleteAssetNow = [&](const std::string& assetAbs, bool isSourceClass)
		{
			std::error_code ec;
			std::filesystem::remove(assetAbs, ec);
			if (ec)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not delete '" + assetAbs + "': " + ec.message()).c_str());
				return;
			}
			// The path is free again: a NEW asset of a different type may be created
			// there next, and the panels' cached header sniff would still name the
			// deleted asset's type (→ double-click opens the wrong editor).
			EditorAssetTypeCache::invalidate(assetAbs);
			AssetThumbnailCache::invalidate(assetAbs); // + its cached tile
			// In the Source root, delete BOTH halves of the class's .h/.cpp pair.
			if (isSourceClass)
			{
				const std::filesystem::path p(assetAbs);
				const std::string stem = p.stem().string();
				const std::filesystem::path dir = p.parent_path();
				for (const char* e : { ".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".c" })
				{
					std::filesystem::path sib = dir / (stem + e);
					if (sib != p)
					{
						std::error_code e2;
						std::filesystem::remove(sib, e2);
						EditorAssetTypeCache::invalidate(sib.string());
					}
				}
			}
			if (s_selectedItem == assetAbs) s_selectedItem.clear();
			ctx.contentRefreshPending = true;
		};

		// ── Delete a folder and everything under it ───────────────────────
		// Shared by the two ways in: a folder with nothing in it goes straight
		// away, one holding assets only gets here after the confirmation dialog.
		auto deleteFolderNow = [&](const std::string& folderAbs)
		{
			std::error_code ec;
			std::filesystem::remove_all(folderAbs, ec);
			if (ec)
			{
				HE_LOG_ERROR(Editor, "%s",
					("Editor: could not delete folder '" + folderAbs + "': " + ec.message()).c_str());
				return;
			}
			// Both caches are keyed by path and every asset below the folder just
			// lost its path — same call the folder RENAME makes, which drops the
			// whole map rather than walking the subtree it no longer owns.
			EditorAssetTypeCache::invalidateAll();
			AssetThumbnailCache::clear();

			// An editor tab still open on one of those assets would write the file
			// back into the folder that was just deleted on its next Save, so close
			// them here; EditorUI erases them (and forgets their state) next frame.
			for (auto& t : ctx.tabs)
				if (t.closable && !t.assetPath.empty() && isUnderFolder(t.assetPath, folderAbs))
					t.open = false;

			if (s_selectedItem == folderAbs || isUnderFolder(s_selectedItem, folderAbs))
				s_selectedItem.clear();
			s_ctxMenuItem.clear();
			// The grid may be standing inside the folder that just went away: the
			// refresh bumps the folder version, the re-resolution at the top of the
			// panel fails to find the remembered path and falls back to the root.
			ctx.contentRefreshPending = true;
			HE_LOG_INFO(Editor, "%s", ("Editor: deleted folder " + folderAbs).c_str());
		};

		if (s_rightClickOnItem)
		{
			ImGui::OpenPopup("##cb_item_ctx");
			s_rightClickOnItem = false;
		}

		// ── Item context menu (folder + file) ─────────────────────────────
		if (ImGui::BeginPopup("##cb_item_ctx"))
		{
			std::string displayName = s_ctxMenuIsFolder
				? std::filesystem::path(s_ctxMenuItem).filename().string()
				: std::filesystem::path(s_ctxMenuItem).stem().string();
			ImGui::TextDisabled("%s", displayName.c_str());

			// Engine default assets are read-only from a project's point of view
			// (HE_ENGINE_CONTENT_EDITABLE=1 lifts this for engine-authoring
			// builds): no create/rename/delete on the shared default or its
			// project override — "Revert to Default" is the only mutation an
			// override gets, and editing+Saving one (in its own graph/asset
			// editor tab) is what CREATES an override in the first place, via
			// ContentManager::saveAsset's redirect, not through this menu.
			const bool isEngineOverride = ctx.contentManager && ctx.contentManager->isEngineOverridePath(s_ctxMenuItem);
			const bool isEngineDefault  = ctx.contentManager && ctx.contentManager->isEngineDefaultPath(s_ctxMenuItem);
			const bool engineLocked     = (isEngineOverride || isEngineDefault) && !ContentManager::isEngineContentDevMode();
			if (engineLocked)
				ImGui::TextDisabled(isEngineOverride ? "project override of an engine default" : "engine default asset (read-only)");
			ImGui::Separator();

			if (isEngineOverride && !ContentManager::isEngineContentDevMode() && !s_ctxMenuIsFolder &&
			    ImGui::MenuItem("Revert to Default"))
			{
				const std::string relPath = ctx.contentManager->toContentRelativePath(s_ctxMenuItem);
				if (!relPath.empty())
				{
					const HE::UUID id = ctx.contentManager->loadAsset(relPath); // current (override) id
					std::error_code ec;
					std::filesystem::remove(s_ctxMenuItem, ec);
					if (!ec)
					{
						// Nothing lives at that path any more — drop the cached type sniff
						// and the rendered tile of the override that just went away.
						EditorAssetTypeCache::invalidate(s_ctxMenuItem);
						AssetThumbnailCache::invalidate(s_ctxMenuItem);
						if (id != HE::UUID{}) ctx.contentManager->unloadAsset(id);
						if (s_selectedItem == s_ctxMenuItem) s_selectedItem.clear();
						s_ctxMenuItem.clear();
						ctx.contentRefreshPending = true;
						HE_LOG_INFO(Editor, "%s", ("Editor: reverted '" + relPath + "' to its engine default").c_str());
					}
					else
						HE_LOG_ERROR(Editor, "%s", ("Editor: could not remove override for '" + relPath + "'").c_str());
				}
				ImGui::CloseCurrentPopup();
			}

			// ── Import source file → .hasset ─────────────────────────────
			if (!s_ctxMenuIsFolder)
			{
				const std::filesystem::path srcPath(s_ctxMenuItem);
				std::string ext = srcPath.extension().string();
				for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

				const bool isMeshSrc    = (ext == ".gltf" || ext == ".glb");
				const bool isTextureSrc = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
				                           ext == ".tga" || ext == ".bmp" || ext == ".hdr");
				const bool isAudioSrc   = (ext == ".wav");
				const bool isMatSrc     = (ext == ".hmat");
				const bool isFontSrc    = (ext == ".ttf" || ext == ".otf");

				if (!engineLocked && (isMeshSrc || isTextureSrc || isAudioSrc || isMatSrc || isFontSrc) &&
				    ImGui::MenuItem("Import"))
				{
					// The item being imported lives in whichever root is currently
					// browsed (s_selectedRootKind) — NOT always Content.
					const std::filesystem::path root = cbRootFolder(s_selectedRootKind).fullPath;
					std::error_code ec;
					std::filesystem::path relDir =
						std::filesystem::relative(srcPath.parent_path(), root, ec);
					if (ec || relDir == ".") relDir.clear();

					bool ok = false;
					if (isMeshSrc && Importer::gltfHasSkin(srcPath))
					{
						// Same routing as File ▸ Import Asset: a skinned glTF must not
						// go through MeshImporter, which discards the skeleton and the
						// JOINTS_0/WEIGHTS_0 attributes.
						ok = SkeletalMeshImporter::import(srcPath, root, relDir) != nullptr;
						if (ok) AnimationClipImporter::importAndWrite(srcPath, root, relDir);
					}
					else if (isMeshSrc)    ok = MeshImporter::import(srcPath, root, relDir)     != nullptr;
					else if (isTextureSrc) ok = TextureImporter::import(srcPath, root, relDir)  != nullptr;
					else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root, relDir)    != nullptr;
					else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root, relDir) != nullptr;
					else if (isFontSrc)    ok = FontImporter::import(srcPath, root, relDir)     != nullptr;

					if (!ok)
						HE_LOG_ERROR(Editor, "%s",
							("Editor: import failed for " + srcPath.string()).c_str());
					ctx.contentRefreshPending = true;
					ImGui::CloseCurrentPopup();
				}

				// ── Material → create a child INSTANCE (params/switches only) ──
				// Allowed even for an engine-default material: it creates a NEW,
				// separate asset (a plain project one, not "Engine/..."-namespaced)
				// rather than mutating the default.
				if (ext == ".hasset" && ctx.contentManager &&
				    MaterialEditorPanel::isMaterialAsset(s_ctxMenuItem) &&
				    ImGui::MenuItem("Create Material Instance"))
				{
					// registerMaterial() below stores inst.path directly (no later
					// loadAsset() to correct it), so it MUST carry the "Engine/"
					// prefix already when the parent lives under the engine root —
					// toContentRelativePath(), not a manual root-relative fs::relative.
					const std::string parentRel = ctx.contentManager->toContentRelativePath(srcPath.string());
					if (!parentRel.empty())
					{
						// Unique sibling: <stem>_Inst[.N].hasset
						std::filesystem::path dst =
							srcPath.parent_path() / (srcPath.stem().string() + "_Inst.hasset");
						for (int k = 2; std::filesystem::exists(dst) && k < 100; ++k)
							dst = srcPath.parent_path() /
								(srcPath.stem().string() + "_Inst" + std::to_string(k) + ".hasset");
						MaterialAsset inst;
						inst.type = HE::AssetType::Material;
						inst.name = dst.stem().string();
						inst.path = ctx.contentManager->toContentRelativePath(dst.string());
						inst.parentMaterialPath = parentRel;
						const HE::UUID iid = ctx.contentManager->registerMaterial(std::move(inst));
						ctx.contentManager->syncMaterialInstance(iid); // derive shader/params
						if (MaterialAsset* mi = ctx.contentManager->getMaterialMutable(iid))
							ctx.contentManager->saveAsset(*mi);
						ctx.contentRefreshPending = true;
						HE_LOG_INFO(Editor, "%s",
							("Editor: created material instance of '" + parentRel + "'").c_str());
					}
					ImGui::CloseCurrentPopup();
				}

				// ── Add a StaticMesh .hasset to the scene ─────────────────
				if (ext == ".hasset" && ctx.world && ctx.contentManager &&
				    ImGui::MenuItem("Add to Scene"))
				{
					std::string rel = ctx.contentManager->toContentRelativePath(srcPath.string());
					if (!rel.empty())
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (const StaticMeshAsset* mesh = ctx.contentManager->getStaticMesh(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							Entity e = ctx.world->createEntity(mesh->name);
							ctx.world->addComponent(e, TransformComponent{});
							ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
							ctx.world->markHierarchyDirty();
							HE_LOG_INFO(Editor, "%s",
								("Editor: added '" + mesh->name + "' to scene").c_str());
						}
						else
							HE_LOG_WARN(Editor, "%s",
								("Editor: " + rel + " is not a loadable static mesh").c_str());
					}
					ImGui::CloseCurrentPopup();
				}
			}

			// In the Source root a C++ class is a .h/.cpp pair; renaming it means
			// renaming both files AND rewriting the class name/registration inside —
			// a refactor best left to the user's C++ toolchain, so Rename is hidden.
			if (!engineLocked && s_selectedRootKind != 2 && ImGui::MenuItem("Rename"))
			{
				s_renameTarget   = s_ctxMenuItem;
				s_renameIsFolder = s_ctxMenuIsFolder;
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				std::strncpy(s_renameBuf, displayName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				ImGui::CloseCurrentPopup();
			}
			// Deleting an asset asks first. It used to happen on this click, and
			// a red menu item is not a confirmation: the pointer is already over
			// Delete when the menu opens under it, the file has no undo, and
			// afterwards nothing on screen says which one is gone. The dialog
			// names it, which is the whole point.
			if (!engineLocked && !s_ctxMenuIsFolder && EditorWidgets::dangerMenuItem("Delete"))
			{
				s_deleteAssetTarget   = s_ctxMenuItem;
				s_deleteAssetIsSource = s_selectedRootKind == 2;
				s_openDeleteAssetPopup = true;
				s_ctxMenuItem.clear();
				ImGui::CloseCurrentPopup();
			}

			// A folder deletes as a unit — the whole subtree goes with it. That is
			// the one browser operation that destroys work the user cannot see from
			// where they clicked, so anything but an empty folder has to pass a
			// confirmation that names every asset going with it.
			if (!engineLocked && s_ctxMenuIsFolder && EditorWidgets::dangerMenuItem("Delete"))
			{
				s_deleteFolderTarget = s_ctxMenuItem;
				s_deleteFolderFiles  = collectFolderContents(s_ctxMenuItem);
				if (s_deleteFolderFiles.empty())
				{
					// Nothing to lose (an empty folder, or one holding only empty
					// sub-folders) — no dialog worth the interruption. It still
					// has to REPLICATE though: this shortcut skipped the dialog
					// and with it the whole session path, so an empty folder was
					// the one deletion that only ever happened locally.
					const std::string rel = ctx.contentManager
						? ctx.contentManager->toContentRelativePath(s_deleteFolderTarget)
						: std::string();
					if (!(ctx.collab && !rel.empty() &&
					      ctx.collab->requestAssetDelete(rel, /*folder=*/true)))
					{
						deleteFolderNow(s_deleteFolderTarget);
					}
					s_deleteFolderTarget.clear();
				}
				else
					s_openDeleteFolderPopup = true;
				ImGui::CloseCurrentPopup();
			}

			// The asset-create submenu makes no sense in the Source root (it would
			// write .hasset files there) — the background right-click there offers
			// "C++ Class" instead.
			if (!engineLocked && s_selectedRootKind != 2)
			{
				ImGui::Separator();
				if (ImGui::BeginMenu("Create Asset"))
				{
					const std::string createDir = s_ctxMenuIsFolder
						? s_ctxMenuItem
						: std::filesystem::path(s_ctxMenuItem).parent_path().string();
					drawCreateAssetItems(createDir);
					ImGui::EndMenu();
				}
			}

			ImGui::EndPopup();
		}

		// Trigger rename popup outside item context popup. Gated on the content
		// refresh having settled so it never competes with the ##ContentRefresh
		// modal (e.g. right after a create, which requests both).
		if (s_openRenamePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_rename_popup");
			s_openRenamePopup = false;
		}

		// ── Rename / name-on-create popup ─────────────────────────────────
		ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			const char* verb = s_renameIsCreate ? "Name" : "Rename";
			ImGui::Text("%s %s", verb, s_renameIsFolder ? "Folder" : "Asset");
			ImGui::Separator();
			ImGui::Spacing();

			// Focus the field as the dialog opens so the user can type at once.
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1.0f);
			bool confirm = ImGui::InputText("##rename_input", s_renameBuf, sizeof(s_renameBuf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::Spacing();

			// (A script's language is fixed by the project, chosen in the New
			// Project wizard — there is no per-asset language picker here.)

			if (EditorWidgets::primaryButton("OK", ImVec2(140, 0)) || confirm)
			{
				std::string newName(s_renameBuf);
				if (!newName.empty() && !s_renameTarget.empty())
				{
					std::filesystem::path oldPath(s_renameTarget);
					std::filesystem::path newPath;
					if (s_renameIsFolder)
						newPath = oldPath.parent_path() / newName;
					else
						newPath = oldPath.parent_path() / (newName + oldPath.extension().string());

					// Renaming an existing asset in a session is a REQUEST, the
					// same as deleting one: it breaks every reference to the old
					// name, which is as much somebody else's problem as ours.
					// Naming a freshly created asset is not — nothing refers to
					// it yet, and it is announced under its final name anyway.
					//
					// NOT an early return, however tempting: this popup sits
					// inside the panel's own Begin/End, and leaving from here
					// would skip the End and unbalance the ImGui window stack —
					// which nothing but a runtime assertion would ever tell us.
					// NOT gated on being a client. requestAssetRename answers the
					// host by broadcasting it — that branch is the whole reason
					// it exists — and excluding the host here meant a host's
					// rename went to its own disk and nowhere else: peers kept
					// the old path, and the next scene save handed them a
					// reference to a file they did not have. The delete paths
					// call unconditionally for exactly this reason; this one now
					// matches them.
					bool requested = false;
					if (!s_renameIsCreate && ctx.collab &&
					    ctx.contentManager && ctx.collab->inSession())
					{
						const std::string relOld =
							ctx.contentManager->toContentRelativePath(oldPath.string());
						const std::string relNew =
							ctx.contentManager->toContentRelativePath(newPath.string());
						// Folders included: renaming one moves every asset under
						// it, so it breaks references on a far larger scale than
						// renaming a single file does.
						requested = !relOld.empty() && !relNew.empty() &&
						            ctx.collab->requestAssetRename(relOld, relNew,
						                                          s_renameIsFolder);
					}

					// Nothing moves locally when it was asked for. It moves when
					// the host says so, on every machine at once — renaming here
					// first would leave us out of step until the answer, and out
					// of step for good if the answer is no.
					std::error_code ec;
					if (!requested) std::filesystem::rename(oldPath, newPath, ec);
					if (!requested && !ec)
					{
						// Carry every stored reference to the old path (or, for a
						// folder, to anything under it) over to the new one — an
						// asset renamed out from under its referrers is exactly how
						// a project silently loses its materials/textures. Skipped
						// on create: a brand-new asset has no referrers yet.
						if (!s_renameIsCreate)
							retargetReferences(ctx, s_renameTarget, newPath.string(), s_renameIsFolder);
						// Both paths now hold something else than the type cache believes:
						// the old one nothing, the new one possibly a stale negative entry
						// from before it existed. A FOLDER rename moves every asset below
						// it, so there the whole map has to go.
						if (s_renameIsFolder)
						{
							EditorAssetTypeCache::invalidateAll();
							// Same for the tiles: every asset under the folder is now
							// addressed by a path that no longer exists. Their .hthumb
							// files are left behind as orphans rather than walking the
							// whole subtree to delete them — they are never looked up
							// again, and the cache directory is disposable by design.
							AssetThumbnailCache::clear();
						}
						else
						{
							EditorAssetTypeCache::invalidate(s_renameTarget);
							EditorAssetTypeCache::invalidate(newPath.string());
							AssetThumbnailCache::invalidate(s_renameTarget);
							AssetThumbnailCache::invalidate(newPath.string());
						}
						if (s_selectedItem == s_renameTarget)
							s_selectedItem = newPath.string();
						// The name the session will hear about. Only meaningful on
						// create; on a rename this is empty and stays so.
						if (s_renameIsCreate && !s_pendingCreatePublish.empty())
							s_pendingCreatePublish = newPath.string();
						if (!s_renameIsFolder)
						{
							for (auto& t : ctx.tabs)
								if (t.assetPath == s_renameTarget)
								{
									t.assetPath = newPath.string();
									t.label     = newName;
								}
						}
						s_quietContentRefresh = true;

						// In a C++ project, a freshly created scene gets a matching
						// native level script (Source/<Scene>LevelScript.{h,cpp}) with
						// event stubs — the C++ mirror of the HorizonCode Level Script
						// graph. Uses the scene's final name.
						if (s_renameIsCreate && ctx.projectManager &&
						    ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Cpp &&
						    newPath.extension() == ".hescene")
						{
							const std::filesystem::path projRoot =
								std::filesystem::path(ctx.projectManager->currentProject().path).parent_path();
							if (writeCppLevelScript(projRoot.string(), newName))
								HE_LOG_INFO(Editor, "%s",
									("Editor: generated C++ level script for scene '" + newName + "'").c_str());
						}
					}
				}
				s_renameTarget.clear();
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(140, 0)))
			{
				// On create, Cancel just keeps the default name — the file already
				// exists on disk; nothing to undo.
				s_renameTarget.clear();
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
		// ── The created asset is announced HERE ───────────────────────────
		// After the popup block, so it runs however the dialog ended: OK with a
		// new name, Cancel keeping the default, or Escape — which closes a modal
		// without running either branch and used to leave s_renameTarget and
		// s_renameIsCreate standing. Waiting for it to be closed AND not merely
		// waiting to open (the content-refresh gate can hold it back a frame) is
		// what keeps this from firing on the frame between the two.
		else if (!s_pendingCreatePublish.empty() && !s_openRenamePopup)
		{
			if (s_renameIsCreate)   // Escape: neither branch cleared this
			{
				s_renameTarget.clear();
				s_renameIsCreate   = false;
				s_renameScriptLang = -1;
			}
			// A new asset has no referrers and no lock, so there is nothing to
			// hold and nothing to retarget — the host decides whether the name
			// is free and hands it on.
			if (ctx.collab && ctx.contentManager)
			{
				const std::string rel =
					ctx.contentManager->toContentRelativePath(s_pendingCreatePublish);
				if (s_pendingCreateIsFolder) ctx.collab->publishFolderCreate(rel);
				else ctx.collab->publishAssetCreate(rel, s_pendingCreatePublish);
			}
			s_pendingCreatePublish.clear();
			s_pendingCreateIsFolder = false;
		}

		// ── Delete-asset confirmation ─────────────────────────────────────
		// Opened here rather than from the context menu for the same reason as
		// the two below: a modal cannot be opened from inside the context
		// popup's ID stack.
		if (s_openDeleteAssetPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_delete_asset_popup");
			s_openDeleteAssetPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_delete_asset_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			const std::string assetName =
				std::filesystem::path(s_deleteAssetTarget).filename().string();

			ImGui::Text("Delete \"%s\"", assetName.c_str());
			ImGui::Separator();
			ImGui::Spacing();
			// Two facts the user cannot get back afterwards, so they belong here
			// rather than in a log line nobody reads.
			ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f),
				"This deletes the file. It does not go to the trash and cannot be undone.");
			ImGui::TextDisabled("Anything still referencing it keeps a broken reference.");
			if (s_deleteAssetIsSource)
				ImGui::TextDisabled("Both halves of the class (.h and .cpp) are deleted.");
			ImGui::Spacing();

			// In a session this is a REQUEST, not a deletion — for everyone but
			// the host, who has just answered the only question a request asks.
			const bool needsApproval = ctx.collab && ctx.collab->inSession() &&
			                           !ctx.collab->isHost();
			if (needsApproval)
			{
				ImGui::Spacing();
				ImGui::TextDisabled("The host has to approve this before it happens.");
				ImGui::Spacing();
			}
			if (EditorWidgets::dangerButton(needsApproval ? "Ask the host" : "Delete",
			                                ImVec2(210, 0)))
			{
				const std::string rel = ctx.contentManager
					? ctx.contentManager->toContentRelativePath(s_deleteAssetTarget)
					: std::string();
				// requestOrPerformAssetOp answers false when there is no session,
				// which is the ordinary case and means: just delete it.
				if (!(ctx.collab && !rel.empty() &&
				      ctx.collab->requestAssetDelete(rel)))
				{
					deleteAssetNow(s_deleteAssetTarget, s_deleteAssetIsSource);
				}
				s_deleteAssetTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_deleteAssetTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		// Escape closes a modal without running either branch, and a target left
		// behind would be deleted by the NEXT confirmation. Same stale-state trap
		// the create/rename popup has (see the note there).
		else if (!s_deleteAssetTarget.empty() && !s_openDeleteAssetPopup)
			s_deleteAssetTarget.clear();

		// ── Delete-folder confirmation ────────────────────────────────────
		// Raised from the item context menu, opened here for the same reason the
		// rename dialog is: a modal cannot be opened from inside the context
		// popup's ID stack, and the content-refresh gate keeps it from competing
		// with the "##ContentRefresh" modal (which a delete requests as well).
		if (s_openDeleteFolderPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_delete_folder_popup");
			s_openDeleteFolderPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_delete_folder_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			const std::string folderName =
				std::filesystem::path(s_deleteFolderTarget).filename().string();
			const int fileCount = static_cast<int>(s_deleteFolderFiles.size());

			ImGui::Text("Delete Folder \"%s\"", folderName.c_str());
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.45f, 1.0f),
				"This deletes the folder and the %d asset%s inside it.",
				fileCount, fileCount == 1 ? "" : "s");
			ImGui::TextDisabled("Anything still referencing them keeps a broken reference.");
			ImGui::Spacing();

			// The list is what makes the warning worth reading, so it is shown in
			// full up to a point — a folder with thousands of assets would cost a
			// line of text per entry per frame, and the tail says nothing the count
			// above has not already said.
			constexpr int k_maxListed = 200;
			const int listed = (std::min)(fileCount, k_maxListed);
			const float lineH  = ImGui::GetTextLineHeightWithSpacing();
			const float listH  = std::clamp(lineH * static_cast<float>(listed) + 8.0f, lineH, 220.0f);
			ImGui::BeginChild("##cb_delete_folder_list", ImVec2(-1.0f, listH), true,
				ImGuiWindowFlags_HorizontalScrollbar);
			for (int i = 0; i < listed; ++i)
				ImGui::TextUnformatted(s_deleteFolderFiles[static_cast<size_t>(i)].c_str());
			if (fileCount > listed)
				ImGui::TextDisabled("... and %d more", fileCount - listed);
			ImGui::EndChild();
			ImGui::Spacing();

			// A folder is the biggest yes in this panel — everything under it
			// goes — so in a session it is asked for like any other deletion.
			const bool folderNeedsApproval =
				ctx.collab && ctx.collab->inSession() && !ctx.collab->isHost();
			if (folderNeedsApproval)
			{
				ImGui::TextDisabled("The host has to approve this before it happens.");
				ImGui::Spacing();
			}
			if (EditorWidgets::dangerButton(folderNeedsApproval ? "Ask the host" : "Delete",
			                                ImVec2(210, 0)))
			{
				const std::string rel = ctx.contentManager
					? ctx.contentManager->toContentRelativePath(s_deleteFolderTarget)
					: std::string();
				if (!(ctx.collab && !rel.empty() &&
				      ctx.collab->requestAssetDelete(rel, /*folder=*/true)))
				{
					deleteFolderNow(s_deleteFolderTarget);
				}
				s_deleteFolderTarget.clear();
				s_deleteFolderFiles.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(210, 0)))
			{
				s_deleteFolderTarget.clear();
				s_deleteFolderFiles.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

#ifdef HE_HAVE_LIBSSH2
		// ── EngineContent remote-download confirmation ─────────────────────
		// Raised from the file-grid double-click above. Same "cannot open a modal
		// from inside another ID stack" reason the delete-folder dialog is opened
		// out here instead of at the click site.
		if (s_openRemoteDownloadPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_remote_download_popup");
			s_openRemoteDownloadPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_remote_download_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::Text("Download \"%s\"?", s_remoteDownloadTabLabel.c_str());
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextWrapped(
				"This EngineContent asset is not on this machine yet — it lives on the "
				"EngineContent server. Downloading it saves a copy in the shared "
				"EngineContent cache, so every project (not just this one) can use it "
				"from now on without downloading it again.");
			ImGui::Spacing();

			if (EditorWidgets::primaryButton("Download", ImVec2(200, 0)))
			{
				const std::string relPath           = s_remoteDownloadRelativePath;
				const std::string fullPath          = s_remoteDownloadFullPath;
				GlobalState*      gs                = ctx.globalState;
				const std::string engineContentPath = ctx.contentManager->engineContentRoot();
				const std::string projectContentRoot = ctx.contentManager->contentRoot();
				HE::Cs::EngineContentSync::instance().enqueueDownload(
					relPath, s_remoteDownloadUuid, HE::Cs::DownloadTrigger::Explicit,
					[fullPath, gs, engineContentPath, projectContentRoot](bool success)
					{
						// Worker thread — must not touch ctx/ImGui. GlobalState's own
						// refresh is safe here (mutex-guarded, filesystem-only, same
						// fact startSftpProbe's manifest refresh already relies on).
						// The re-merge is what clears this asset's isRemoteOnly flag:
						// mergeManifestInto now sees the freshly-downloaded file in the
						// cache and emits a normal node instead of a remote placeholder.
						// The tab-open request, which DOES need the main thread, goes
						// through s_pendingOpenFullPaths (static storage, so this
						// reference stays valid regardless of when the callback fires).
						if (!success) return;
						if (gs && !engineContentPath.empty())
							gs->refreshEngineFolder(engineContentPath, projectContentRoot);
						std::lock_guard<std::mutex> lock(s_pendingOpenMutex);
						s_pendingOpenFullPaths.push_back(fullPath);
					});
				s_remoteDownloadRelativePath.clear();
				s_remoteDownloadTabLabel.clear();
				s_remoteDownloadFullPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(200, 0)))
			{
				s_remoteDownloadRelativePath.clear();
				s_remoteDownloadTabLabel.clear();
				s_remoteDownloadFullPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
#endif

		// ── Name-a-C++-class popup (C++ projects only) ────────────────────
		// The Create menu's "C++ Class" item stages a default name and raises
		// this; on confirm it writes Source/<Name>.{h,cpp}. Those files live
		// outside Content/, so they don't appear in the browser — the user edits
		// them in their own C++ toolchain (which is the C++ workflow).
		if (s_openCppClassPopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_cpp_class_popup");
			s_openCppClassPopup = false;
		}
		ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
		EditorWidgets::pinDialogToEditorWindow();
		if (ImGui::BeginPopupModal("##cb_cpp_class_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::TextUnformatted("New C++ Class");
			ImGui::Separator();
			ImGui::Spacing();
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1.0f);
			bool confirm = ImGui::InputText("##cpp_class_input", s_cppClassName, sizeof(s_cppClassName),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::TextDisabled("Creates Source/<Name>.h and .cpp");
			ImGui::Spacing();
			const bool canCreate = s_cppClassName[0] != '\0' && ctx.projectManager &&
				!ctx.projectManager->currentProject().path.empty();
			if ((EditorWidgets::primaryButton("Create", ImVec2(150, 0)) || confirm) && canCreate)
			{
				const std::filesystem::path projRoot =
					std::filesystem::path(ctx.projectManager->currentProject().path).parent_path();
				std::string created;
				if (writeCppClass(projRoot.string(), s_cppClassName, &created))
				{
					HE_LOG_INFO(Editor, "%s",
						("Editor: created C++ class at " + created).c_str());
					// BOTH halves. A class is a .h and a .cpp, and a peer that
					// received only the header would have a declaration with
					// nothing behind it — which compiles right up until it does
					// not. They travel under the reserved ::Source:: key, since
					// they live beside Content rather than inside it.
					if (ctx.collab && ctx.collab->inSession())
					{
						const std::string srcRoot = projRoot.string() + "/Source";
						const std::filesystem::path h(created);
						for (const char* ext : { ".h", ".cpp" })
						{
							const std::filesystem::path f =
								h.parent_path() / (h.stem().string() + ext);
							if (!std::filesystem::exists(f)) continue;
							const std::string rel =
								CollabController::projectRelativeAssetPath(f.string(), srcRoot);
							if (rel.empty()) continue;
							ctx.collab->publishAssetCreate("::Source::" + rel, f.string());
						}
					}
				}
				else
					HE_LOG_ERROR(Editor, "%s", "Editor: failed to create C++ class");
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel", ImVec2(150, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		// ── Background left-click → clear selection ───────────────────────
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsAnyItemHovered())
		{
			s_selectedItem.clear();
		}

		// ── Background right-click context menu
		// Only open when clicking on the panel background (not on any item)
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			!ImGui::IsAnyItemHovered())
		{
			ImGui::OpenPopup("##cb_create_ctx");
		}

		if (ImGui::BeginPopup("##cb_create_ctx"))
		{
			// Normal mode: the Engine tree is read-only ground — no creating
			// new "default" content from a project. (HE_ENGINE_CONTENT_EDITABLE=1
			// lifts this for engine-authoring builds.) Editing an existing
			// default and hitting Save is what creates a project override —
			// see ContentManager::saveAsset's redirect, not this menu.
			if (s_selectedRootKind == 1 && !ContentManager::isEngineContentDevMode())
			{
				ImGui::TextDisabled("Engine default assets are read-only here.");
				ImGui::TextDisabled("Open one in its editor tab and Save to");
				ImGui::TextDisabled("create a project-local override instead.");
			}
			else if (s_selectedRootKind == 2)
			{
				// Source root holds native C++ files, not engine assets — offer only
				// "C++ Class" (writes to this project's Source/). The .hasset list
				// would create engine assets under Source/, which doesn't belong here.
				ImGui::TextDisabled("Create C++");
				ImGui::Separator();
				if (ImGui::MenuItem("C++ Class"))
				{
					std::strncpy(s_cppClassName, "GameplayClass", sizeof(s_cppClassName) - 1);
					s_cppClassName[sizeof(s_cppClassName) - 1] = '\0';
					s_openCppClassPopup = true;
					ImGui::CloseCurrentPopup();
				}
			}
			else
			{
				ImGui::TextDisabled("Create Asset");
				ImGui::Separator();

				const std::string targetFolder = displayFolder ? displayFolder->fullPath
															   : contentFolder.fullPath;
				drawCreateAssetItems(targetFolder);
				if (ImGui::MenuItem("Folder"))
				{
					std::string base = targetFolder + "/NewFolder";
					std::string dir  = base;
					int counter = 1;
					while (std::filesystem::exists(dir))
						dir = base + std::to_string(counter++);
					std::filesystem::create_directory(dir);

					// Show it now and let the user name it straight away.
					const std::string folderName = std::filesystem::path(dir).filename().string();
					s_selectedItem    = dir;
					s_renameTarget    = dir;
					// Same choke point as an asset: announced when the name is
					// final, not when the directory appears.
					s_pendingCreatePublish  = dir;
					s_pendingCreateIsFolder = true;
					s_renameIsFolder  = true;
					s_renameIsCreate  = true;
					std::strncpy(s_renameBuf, folderName.c_str(), sizeof(s_renameBuf) - 1);
					s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
					s_openRenamePopup = true;
					s_quietContentRefresh = true;
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::EndPopup();
		}

		ImGui::EndChild();
    }

    ImGui::End();
#else
	(void)ctx; (void)tabSelectRequest; (void)openSceneGuarded;
#endif // HE_IMGUI_ENABLED
}

} // namespace ContentBrowserPanel
