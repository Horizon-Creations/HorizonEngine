#include "HcRenameSweep.h"
#include "HcEditorUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonCode/HorizonCode.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace HcRenameSweep
{
namespace
{
	bool contains(const std::vector<std::string>& v, const std::string& s)
	{
		return std::find(v.begin(), v.end(), s) != v.end();
	}

	// A scene keeps its level script as a plain JSON section. Reading and writing
	// just that section beats round-tripping the whole scene through a world: the
	// rest of the file stays byte-for-byte what it was, which matters for a file
	// under version control that the user did not ask to have rewritten.
	bool readSceneGraph(const std::string& absPath, HorizonCode::Graph& out, nlohmann::json& scene)
	{
		std::ifstream f(absPath);
		if (!f) return false;
		std::stringstream ss;
		ss << f.rdbuf();
		scene = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
		if (scene.is_discarded() || !scene.contains("levelScript")) return false;
		return HorizonCode::fromJson(scene["levelScript"].dump(), out);
	}

	bool writeSceneGraph(const std::string& absPath, const HorizonCode::Graph& g, nlohmann::json& scene)
	{
		nlohmann::json ls = nlohmann::json::parse(HorizonCode::toJson(g), nullptr, false);
		if (ls.is_discarded()) return false;
		scene["levelScript"] = std::move(ls);
		std::ofstream f(absPath, std::ios::trunc);
		if (!f) return false;
		f << scene.dump(2);
		return f.good();
	}

	// Which of the two roles a graph plays for this rename. Only a class asset can
	// override, and only when it really derives from the renamed class.
	HcRename::Role roleOf(Kind kind, const std::string& key,
	                      const std::vector<std::string>& targetKeys, const std::string& classKey)
	{
		if (kind == Kind::Class && key != classKey && contains(targetKeys, key))
			return HcRename::Role::Overrides;
		return HcRename::Role::Other;
	}

	// The Game Instance is addressed by nothing a stored graph can name — a
	// Get Game Instance node carries no key at all — so the sweep resolves it to
	// the same string the editor's own tab uses.
	const char* kGameInstanceKey = "Game Instance";
} // namespace

int  Report::assetsToWrite() const
{
	int n = 0;
	for (const Entry& e : entries) if (e.writable()) ++n;
	return n;
}
int Report::renameCount() const
{
	int n = 0;
	for (const Entry& e : entries) if (e.writable()) n += (int)e.plan.rename.size();
	return n;
}
int Report::unsureCount() const
{
	int n = 0;
	for (const Entry& e : entries) n += (int)e.plan.unsure.size();
	return n;
}
bool Report::anything() const
{
	for (const Entry& e : entries) if (e.plan.anything()) return true;
	return false;
}

std::vector<std::string> classAndDescendants(ContentManager& cm, const std::string& classKey)
{
	std::vector<std::string> keys{ classKey };
	if (classKey.empty()) return keys;

	// Every class asset's ancestry, resolved the way the runtime resolves it, so
	// a chain three deep counts as much as a direct child.
	for (const HcEditorUtil::ClassRef& c : HcEditorUtil::listHorizonCodeClasses(&cm))
	{
		if (c.path == classKey) continue;
		const HorizonCode::ResolvedClass rc = HorizonCode::resolveClassAsset(cm, c.path);
		if (rc.ok && contains(rc.chain, classKey)) keys.push_back(c.path);
	}
	return keys;
}

Report scan(ContentManager& cm, const HcRename::Target& t,
            const std::vector<std::string>& targetKeys,
            const std::vector<std::string>& handledInMemory,
            const std::function<std::string(const std::string&)>& lockedOrDirty)
{
	Report r;
	if (t.oldName.empty() || t.newName.empty() || t.oldName == t.newName) return r;

	auto consider = [&](Kind kind, const std::string& display, const std::string& path,
	                    HE::UUID id, const HorizonCode::Graph& g)
	{
		if (contains(handledInMemory, display) || contains(handledInMemory, path)) return;
		HcRename::Plan plan = HcRename::planGraph(
			g, roleOf(kind, display, targetKeys, t.classKey), targetKeys, display,
			kGameInstanceKey, t);
		if (!plan.anything()) return;
		Entry e;
		e.kind = kind; e.display = display; e.path = path; e.id = id;
		e.plan = std::move(plan);
		if (lockedOrDirty && e.plan.touches()) e.skipWhy = lockedOrDirty(display);
		r.entries.push_back(std::move(e));
	};

	for (const HcEditorUtil::ClassRef& c : HcEditorUtil::listHorizonCodeClasses(&cm))
	{
		const HE::UUID id = cm.loadAsset(c.path);
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(id);
		if (!a || a->graphJson.empty()) continue;
		HorizonCode::Graph g;
		if (HorizonCode::fromJson(a->graphJson, g)) consider(Kind::Class, c.path, c.path, id, g);
	}

	for (const HcEditorUtil::ClassRef& w : HcEditorUtil::listAssets(&cm, HE::AssetType::Widget))
	{
		const HE::UUID id = cm.loadAsset(w.path);
		const UIWidgetAsset* a = cm.getWidget(id);
		if (!a || a->graphJson.empty()) continue;
		HorizonCode::Graph g;
		if (HorizonCode::fromJson(a->graphJson, g)) consider(Kind::Widget, w.path, w.path, id, g);
	}

	// An animator's sync graph is a HorizonCode graph like any other, and being
	// the one that runs per animated entity per frame it is a likely caller.
	for (const HcEditorUtil::ClassRef& m : HcEditorUtil::listAssets(&cm, HE::AssetType::AnimatorStateMachine))
	{
		const HE::UUID id = cm.loadAsset(m.path);
		const AnimatorStateMachineAsset* a = cm.getAnimatorStateMachine(id);
		if (!a || a->syncGraphJson.empty()) continue;
		HorizonCode::Graph g;
		if (HorizonCode::fromJson(a->syncGraphJson, g)) consider(Kind::AnimatorSync, m.path, m.path, id, g);
	}

	// Scenes are files, not assets: their level script is a section in the
	// .hescene. listScenes yields the project-relative path the exporter uses;
	// the absolute one is what gets read and written.
	const std::filesystem::path projectRoot =
		std::filesystem::path(cm.contentRoot()).parent_path();
	for (const HcEditorUtil::ClassRef& s : HcEditorUtil::listScenes(&cm))
	{
		const std::string abs = (projectRoot / s.path).string();
		HorizonCode::Graph g;
		nlohmann::json scene;
		if (readSceneGraph(abs, g, scene)) consider(Kind::Scene, s.path, abs, HE::UUID{}, g);
	}

	return r;
}

int apply(ContentManager& cm, const Report& r, const HcRename::Target& t)
{
	int written = 0;
	for (const Entry& e : r.entries)
	{
		if (!e.writable()) continue;

		switch (e.kind)
		{
			case Kind::Class:
			{
				HorizonCodeClassAsset* a = cm.getHorizonCodeClassMutable(e.id);
				if (!a) break;
				HorizonCode::Graph g;
				if (!HorizonCode::fromJson(a->graphJson, g)) break;
				if (!HcRename::apply(g, e.plan, t)) break;
				a->graphJson = HorizonCode::toJson(g);
				if (cm.saveAsset(*a)) ++written;
				break;
			}
			case Kind::Widget:
			{
				UIWidgetAsset* a = cm.getWidgetMutable(e.id);
				if (!a) break;
				HorizonCode::Graph g;
				if (!HorizonCode::fromJson(a->graphJson, g)) break;
				if (!HcRename::apply(g, e.plan, t)) break;
				a->graphJson = HorizonCode::toJson(g);
				if (cm.saveAsset(*a)) ++written;
				break;
			}
			case Kind::AnimatorSync:
			{
				AnimatorStateMachineAsset* a = cm.getAnimatorStateMachineMutable(e.id);
				if (!a) break;
				HorizonCode::Graph g;
				if (!HorizonCode::fromJson(a->syncGraphJson, g)) break;
				if (!HcRename::apply(g, e.plan, t)) break;
				a->syncGraphJson = HorizonCode::toJson(g);
				if (cm.saveAsset(*a)) ++written;
				break;
			}
			case Kind::Scene:
			{
				// Re-read rather than carry the graph from the scan: the dialog
				// stood open in between, and the file is the truth.
				HorizonCode::Graph g;
				nlohmann::json scene;
				if (!readSceneGraph(e.path, g, scene)) break;
				if (!HcRename::apply(g, e.plan, t)) break;
				if (writeSceneGraph(e.path, g, scene)) ++written;
				break;
			}
		}
	}
	if (written)
		HE_LOG_INFO(Editor, "Renamed %s \"%s\" to \"%s\" in %d other asset(s).",
		            t.member == HcRename::Member::Function ? "function"
		            : t.member == HcRename::Member::Variable ? "variable"
		            : t.member == HcRename::Member::Animation ? "animation" : "event",
		            t.oldName.c_str(), t.newName.c_str(), written);
	return written;
}

} // namespace HcRenameSweep
