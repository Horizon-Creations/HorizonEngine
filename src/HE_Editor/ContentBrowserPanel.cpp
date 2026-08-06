#include "ContentBrowserPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include <algorithm>
#include <cstdint>
#include "EditorApplication.h"           // AppContext, GlobalState folders, ProjectManager
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include "ScriptEditorPanel.h"
#include "CppClassEditorPanel.h"
#include "MaterialEditorPanel.h"
#include "UIEditorPanel.h"
#include "HorizonCodeClassPanel.h"
#include "InputAssetPanel.h"
#include "SkeletalMeshEditorPanel.h"
#include "StaticMeshEditorPanel.h"
#include "ParticleGraphEditorPanel.h"
#include "AnimatorStateMachineEditorPanel.h"
#include "AudioEditorPanel.h"
#include "EditorAssetTypeCache.h"
#include "GitController.h"        // per-file source-control status for the tile badge
#include "AssetThumbnailCache.h"         // rendered mesh/material tiles for the grid
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
		static int         s_renameScriptLang = -1;    // creating a script: 0=Lua 1=Python; -1=not a script
		static bool        s_rightClickOnItem = false;
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
			// Double-click → open tab
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (std::filesystem::path(file->fullPath).extension() == ".hescene")
				{
					openSceneGuarded(file->fullPath);
				}
				// Script assets open the code editor tab, material assets the node-graph
				// editor tab. Other asset types have no dedicated editor yet → no-op.
				// C++ source/header (Source root) opens the h/cpp class viewer; a raw
				// .wav opens the audio tab (auditioning a source file must not require
				// importing it first). Both predicates are raw extension checks, not
				// HAsset sniffs, so they must be tested explicitly here.
				else if (CppClassEditorPanel::isCppSourceAsset(file->fullPath) ||
				         AudioEditorPanel::isAudioAsset(file->fullPath) ||
				         ScriptEditorPanel::isScriptAsset(file->fullPath) ||
				         MaterialEditorPanel::isMaterialAsset(file->fullPath) ||
				         MaterialEditorPanel::isMaterialFunctionAsset(file->fullPath) ||
				         UIEditorPanel::isWidgetAsset(file->fullPath) ||
				         HorizonCodeClassPanel::isClassAsset(file->fullPath) ||
				         InputAssetPanel::isInputAsset(file->fullPath) ||
				         SkeletalMeshEditorPanel::isSkeletalMeshAsset(file->fullPath) ||
				         StaticMeshEditorPanel::isStaticMeshAsset(file->fullPath) ||
				         ParticleGraphEditorPanel::isParticleAsset(file->fullPath) ||
				         AnimatorStateMachineEditorPanel::isAnimatorStateMachineAsset(file->fullPath))
				{
				const std::string tabLabel = std::filesystem::path(file->name).stem().string();
				auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
					[&](const AppContext::EditorTab& t){ return t.assetPath == file->fullPath; });
				if (it == ctx.tabs.end())
				{
					ctx.tabs.push_back({ tabLabel, file->fullPath, true, true });
					ctx.activeTab = static_cast<int>(ctx.tabs.size()) - 1;
				}
				else
				{
					ctx.activeTab = static_cast<int>(std::distance(ctx.tabs.begin(), it));
				}
					// Force the tab bar to select this tab next frame (else ImGui keeps the
					// Scene tab selected and the editor never opens).
					tabSelectRequest = ctx.activeTab;
				}
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
			if (!engineLocked && !s_ctxMenuIsFolder && EditorWidgets::dangerMenuItem("Delete"))
			{
				std::error_code ec;
				std::filesystem::remove(s_ctxMenuItem, ec);
				// The path is free again: a NEW asset of a different type may be created
				// there next, and the panels' cached header sniff would still name the
				// deleted asset's type (→ double-click opens the wrong editor).
				EditorAssetTypeCache::invalidate(s_ctxMenuItem);
				AssetThumbnailCache::invalidate(s_ctxMenuItem); // + its cached tile
				// In the Source root, delete BOTH halves of the class's .h/.cpp pair.
				if (s_selectedRootKind == 2)
				{
					const std::filesystem::path p(s_ctxMenuItem);
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
				if (s_selectedItem == s_ctxMenuItem)
					s_selectedItem.clear();
				s_ctxMenuItem.clear();
				ctx.contentRefreshPending = true;
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
					std::error_code ec;
					std::filesystem::rename(oldPath, newPath, ec);
					if (!ec)
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
					HE_LOG_INFO(Editor, "%s",
						("Editor: created C++ class at " + created).c_str());
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
