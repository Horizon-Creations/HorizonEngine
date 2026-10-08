#include "EditorTabs.h"
#ifdef HE_IMGUI_ENABLED
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace EditorTabs
{
bool isVirtual(const std::string& assetPath)
{
	return !assetPath.empty() && assetPath[0] == ':';
}

std::string key(const std::string& assetPath)
{
	if (assetPath.empty() || isVirtual(assetPath)) return assetPath;
	return fs::path(assetPath).lexically_normal().generic_string();
}

bool sameAsset(const std::string& a, const std::string& b, const ContentManager* cm)
{
	if (a.empty() || b.empty()) return false;
	if (a == b) return true;
	// A tab with no file behind it is only ever itself.
	if (isVirtual(a) || isVirtual(b)) return false;
	if (key(a) == key(b)) return true;

	std::error_code ec;
	if (fs::exists(a, ec) && fs::exists(b, ec))
	{
		ec.clear();
		if (fs::equivalent(a, b, ec) && !ec) return true;
	}

	// The same content-relative path is the same asset. The file names are
	// compared first: toContentRelativePath touches the disk, and two paths with
	// different names can never agree on a relative path.
	if (!cm || fs::path(a).filename() != fs::path(b).filename()) return false;
	const std::string ra = cm->toContentRelativePath(a);
	return !ra.empty() && ra == cm->toContentRelativePath(b);
}

int find(const std::vector<AppContext::EditorTab>& tabs, const std::string& assetPath,
         const ContentManager* cm)
{
	if (assetPath.empty()) return -1;
	// An exact spelling wins outright, so a tab that is plainly the one is never
	// preempted by an earlier tab that merely resolves to the same asset.
	for (size_t i = 0; i < tabs.size(); ++i)
		if (tabs[i].assetPath == assetPath) return static_cast<int>(i);
	for (size_t i = 0; i < tabs.size(); ++i)
		if (sameAsset(tabs[i].assetPath, assetPath, cm)) return static_cast<int>(i);
	return -1;
}

int openOrFocus(std::vector<AppContext::EditorTab>& tabs, int& activeTab, int& selectRequest,
                const std::string& assetPath, const std::string& label, const ContentManager* cm)
{
	if (assetPath.empty()) return -1;
	int idx = find(tabs, assetPath, cm);
	if (idx < 0)
	{
		const std::string shown = !label.empty() ? label
			: isVirtual(assetPath) ? assetPath
			: fs::path(assetPath).stem().string();
		tabs.push_back({ shown, assetPath, true, true });
		idx = static_cast<int>(tabs.size()) - 1;
	}
	else
	{
		// A close that is still pending would be carried out by the tab bar's next
		// pass, taking the tab out from under the selection set below.
		tabs[idx].open = true;
	}
	activeTab     = idx;
	selectRequest = idx;
	return idx;
}

bool focusSceneTab(const std::vector<AppContext::EditorTab>& tabs, int& activeTab,
                   int& selectRequest)
{
	const auto it = std::find_if(tabs.begin(), tabs.end(),
		[](const AppContext::EditorTab& t) { return t.assetPath.empty(); });
	if (it == tabs.end()) return false;
	activeTab     = static_cast<int>(it - tabs.begin());
	selectRequest = activeTab;
	return true;
}

int retarget(std::vector<AppContext::EditorTab>& tabs, const std::string& oldPath,
             const std::string& newPath, bool folder)
{
	if (oldPath.empty() || newPath.empty() || isVirtual(oldPath)) return 0;
	const std::string oldKey = key(oldPath);
	if (oldKey.empty()) return 0;
	const std::string prefix = oldKey.back() == '/' ? oldKey : oldKey + "/";
	int moved = 0;
	for (AppContext::EditorTab& t : tabs)
	{
		if (t.assetPath.empty() || isVirtual(t.assetPath)) continue;
		const std::string tabKey = key(t.assetPath);
		if (t.assetPath == oldPath || tabKey == oldKey)
		{
			t.assetPath = newPath;
		}
		else if (folder && tabKey.size() > prefix.size() &&
		         tabKey.compare(0, prefix.size(), prefix) == 0)
		{
			// Everything under the folder keeps its place beneath it.
			t.assetPath = (fs::path(newPath) / fs::path(tabKey.substr(prefix.size())))
			                  .make_preferred().string();
		}
		else
		{
			continue;
		}
		t.label = fs::path(t.assetPath).stem().string();
		++moved;
	}
	return moved;
}

std::vector<int> dedupe(std::vector<AppContext::EditorTab>& tabs, const ContentManager* cm)
{
	std::vector<int> where(tabs.size(), 0);
	std::vector<AppContext::EditorTab> kept;
	kept.reserve(tabs.size());
	for (size_t i = 0; i < tabs.size(); ++i)
	{
		const int hit = find(kept, tabs[i].assetPath, cm);
		if (hit >= 0) { where[i] = hit; continue; }
		kept.push_back(std::move(tabs[i]));
		where[i] = static_cast<int>(kept.size()) - 1;
	}
	tabs = std::move(kept);
	return where;
}
} // namespace EditorTabs
#endif // HE_IMGUI_ENABLED
