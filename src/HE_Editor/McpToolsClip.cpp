#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring what an animation ANNOUNCES, from outside the editor ──────────
// Why a clip has tools at all even though it is an imported asset, and why the
// address of a notify is an index: McpToolRegistry.h, beside McpClipHooks. What
// is worth stating HERE is what the handlers promise.
//
//   • THE QUESTION IS ANSWERED FROM THE FILE, THE EDIT GOES THROUGH THE LOADED
//     CLIP. Reading walks CHUNK_ANIM's first two fields and CHUNK_ANOT, so
//     asking about a clip never loads its keyframes — a 300-channel walk cycle
//     is megabytes of samples nobody asked for. Writing has no such choice: the
//     notify list lives in the clip asset, `saveAsset` writes the whole file
//     from it, and that is the route SkeletalMeshEditorPanel's own Save takes.
//
//   • A TIME OUTSIDE THE CLIP IS REFUSED, NOT CLAMPED. A notify past the end
//     never fires (AnimationNotify.h's firing rule), so a tool that silently
//     moved one inside would be inventing a timestamp, and one that silently
//     left it outside would be writing a dead event and reporting success. The
//     refusal names the clip's duration, which is the number the client lacked.
//
//   • THE ANSWER CARRIES THE WHOLE TIMELINE. Every result — including the two
//     removes — reports the clip's duration, its root-motion switch and all of
//     its notifies with their CURRENT indices. A client that just removed index
//     1 needs to know that the old index 2 is now 1, and asking it to call
//     `clip_info` to find out would be asking it to guess in between.
//
//   • A REFUSAL IS A NO-OP. Nothing is written and nothing is loaded.
//
// What is deliberately NOT here: the keyframes. Those are the import
// (AssetStubWriter refuses to stub an AnimationClip for exactly this reason),
// and a tool that edited a channel would be re-authoring animation that a DCC
// tool owns. PropertyAnimClip is a different asset with a different chunk and is
// not covered either.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── What the file says, without loading it ───────────────────────────────────
// The two chunks a clip's authored half lives in, read in the same order and
// with the same tolerance as ContentManager's loader: a truncated chunk gives
// what it could prove rather than a guess, because the clip still plays.
struct ClipFile
{
	float                        duration      = 0.0f;
	int                          channelCount  = 0;
	bool                         hasRootMotion = false;
	std::vector<AnimationNotify> notifies;
};

bool readClipFile(const std::string& abs, ClipFile& out)
{
	HAsset::Reader r;
	if (!r.open(abs)) return false;

	if (const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_ANIM))
	{
		// Only the first two fields. The channels follow, and walking them is the
		// very cost this read exists to avoid.
		std::size_t o = 0;
		HAsset::Reader::readPOD(c->data, o, out.duration);
		std::uint32_t channels = 0;
		HAsset::Reader::readPOD(c->data, o, channels);
		out.channelCount = static_cast<int>(channels);
	}
	if (const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_ANOT))
	{
		std::size_t   o    = 0;
		std::uint8_t  flag = 0;
		if (HAsset::Reader::readPOD(c->data, o, flag)) out.hasRootMotion = (flag != 0);
		std::uint32_t count = 0;
		if (HAsset::Reader::readPOD(c->data, o, count))
		{
			for (std::uint32_t i = 0; i < count; ++i)
			{
				AnimationNotify n;
				if (!HAsset::Reader::readString(c->data, o, n.name))  break;
				if (!HAsset::Reader::readPOD(c->data, o, n.time))     break;
				if (!HAsset::Reader::readPOD(c->data, o, n.duration)) break;
				out.notifies.push_back(std::move(n));
			}
		}
	}
	// An absent ANOT chunk is not a failure: a clip imported before notifies
	// existed simply carries none, which is also the default.
	return true;
}

// `duration > 0` is the entire difference between the two forms (Assets.h), so
// the form is reported rather than left for a client to rediscover.
json notifyJson(const AnimationNotify& n, int index)
{
	return json{
		{ "index",    index },
		{ "name",     n.name },
		{ "time",     n.time },
		{ "duration", n.duration },
		{ "kind",     n.duration > 0.0f ? "notifyState" : "notify" },
	};
}

json notifiesJson(const std::vector<AnimationNotify>& list)
{
	json out = json::array();
	for (std::size_t i = 0; i < list.size(); ++i)
		out.push_back(notifyJson(list[i], static_cast<int>(i)));
	return out;
}

// The state block every result carries, so no client has to follow its own write
// with a read.
json clipStateJson(const std::string& rel, float duration, int channelCount,
                   bool hasRootMotion, const std::vector<AnimationNotify>& notifies)
{
	return json{
		{ "path",         rel },
		{ "duration",     duration },
		{ "channelCount", channelCount },
		{ "rootMotion",   hasRootMotion },
		{ "notifies",     notifiesJson(notifies) },
	};
}

// ── The addressed clip ───────────────────────────────────────────────────────
// The path check, the play-mode gate, the lock gate and the unsaved-tab gate,
// once — the same four questions and the same order as the particle and animator
// tools.
struct Clip
{
	std::string rel;
	std::string abs;
	ClipFile    file;
	bool        ok      = false;
	ToolResult  failure = ToolResult::ok(json::object());
};

Clip openClip(ContentManager& content, const McpClipHooks& h, const json& args, bool forWrite)
{
	Clip c;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { c.failure = p.failure; return c; }
	c.rel = p.rel;
	c.abs = p.abs;

	if (EditorAssetTypeCache::assetTypeOf(p.abs) != HE::AssetType::AnimationClip)
	{
		c.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not an Animation Clip asset. asset_resolve reports what a "
			"path holds, and clip_info without arguments lists every clip in the project. "
			"A clip cannot be created either — it arrives with a skeletal mesh import.");
		return c;
	}

	if (!readClipFile(p.abs, c.file))
	{
		c.failure = ToolResult::fail("failed",
			"'" + p.rel + "' could not be read. The editor log carries the reason.");
		return c;
	}

	if (forWrite)
	{
		if (p.engine) { c.failure = failEngineReadOnly(p.rel); return c; }
		if (h.isPlaying && h.isPlaying())
		{
			c.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and a notify that moves under a playhead fires "
				"or fails to fire depending on where that playhead happens to be. The edit "
				"is refused rather than landed mid-sweep. Ask the user to stop play mode.");
			return c;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			c.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return c;
		}
		if (h.isDirty && h.isDirty(p.rel))
		{
			c.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' has unsaved notify or root-motion edits in the Skeletal "
				"Mesh Editor — the clip is scrubbed in a mesh's tab and that tab's edits "
				"go straight into the loaded clip. Writing now would mix this edit into "
				"theirs under one Save. Ask the user to save or discard first, then call "
				"again.");
			return c;
		}
	}
	c.ok = true;
	return c;
}

// ── Writing one back ─────────────────────────────────────────────────────────
// The loaded clip IS the edit buffer (ContentManager::getAnimationClipMutable),
// which is why there is no reload hook in McpClipHooks: a tab scrubbing this
// clip reads the very list this writes, so it shows the edit on its next frame.
//
// The load is the ONLY one in this path and nothing is loaded after it, so the
// pointer taken here cannot be moved out from under us by a second asset
// registering (ContentManager.h: the pool is a dense vector).
AnimationClipAsset* loadForWrite(ContentManager& content, const std::string& rel)
{
	const HE::UUID id = content.loadAsset(rel);
	if (id == HE::UUID{}) return nullptr;
	return content.getAnimationClipMutable(id);
}

ToolResult failWrite(const std::string& rel)
{
	return ToolResult::fail("failed",
		"Could not write '" + rel + "'. A read-only file or a full disk is the usual "
		"cause; the editor log carries the reason.");
}

ToolResult saveAndReport(ContentManager& content, const std::string& rel,
                         AnimationClipAsset& clip, json extra)
{
	if (!content.saveAsset(clip)) return failWrite(rel);

	json out = clipStateJson(rel, clip.duration, static_cast<int>(clip.channels.size()),
	                         clip.hasRootMotion, clip.notifies);
	for (auto it = extra.begin(); it != extra.end(); ++it) out[it.key()] = it.value();
	return ToolResult::ok(std::move(out));
}

// A notify outside the clip never fires. Refused with the number the client was
// missing rather than clamped — see the promise at the top of this file.
bool timeInClip(float t, float duration, const std::string& rel, ToolResult& failure)
{
	if (t < 0.0f || (duration > 0.0f && t > duration))
	{
		failure = ToolResult::fail("invalid_args",
			"time " + std::to_string(t) + " is outside '" + rel + "', which is " +
			std::to_string(duration) + " seconds long. A notify past the end of a clip "
			"never fires, so it is refused rather than moved to where it would.");
		return false;
	}
	if (duration <= 0.0f && t != 0.0f)
	{
		failure = ToolResult::fail("invalid_args",
			"'" + rel + "' has no duration on record (no keyframe chunk), so the only "
			"time a notify can carry is 0. Re-import the clip if this is a clip that "
			"should have samples.");
		return false;
	}
	return true;
}

// ── clip_info ────────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content,
             const std::shared_ptr<McpClipHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "clip_info";
	t.description =
		"Without arguments: every Animation Clip asset in the project with its length "
		"and how many events it carries. With 'path': that clip's authored half in full "
		"— its duration, how many channels the import gave it, its per-clip root-motion "
		"switch and every notify with its index, name, time and form.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of an Animation Clip asset, e.g. "
		                      "'Characters/Hero_Run.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Maximum number of assets in the catalogue (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!hasArg(args, "path"))
		{
			bool truncated = false;
			const std::vector<ContentAsset> found = walkContentAssets(
				*cm, { HE::AssetType::AnimationClip }, intArg(args, "limit", 200), truncated);
			json list = json::array();
			for (const ContentAsset& a : found)
			{
				ClipFile f;
				readClipFile(a.abs, f);
				list.push_back(json{
					{ "path",         a.rel },
					{ "duration",     f.duration },
					{ "channelCount", f.channelCount },
					{ "rootMotion",   f.hasRootMotion },
					{ "notifyCount",  static_cast<int>(f.notifies.size()) },
				});
			}
			json out{ { "clips", std::move(list) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		}

		Clip c = openClip(*cm, *h, args, /*forWrite=*/false);
		if (!c.ok) return c.failure;

		json out = clipStateJson(c.rel, c.file.duration, c.file.channelCount,
		                         c.file.hasRootMotion, c.file.notifies);
		// Said outright rather than left to be discovered by a failing edit: the
		// keyframes are the import, and nothing here can author them.
		out["editable"] = json::array({ "notifies", "rootMotion" });
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── clip_notify_set ──────────────────────────────────────────────────────────

void addNotifySet(McpToolRegistry& registry, ContentManager& content,
                  const std::shared_ptr<McpClipHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "clip_notify_set";
	t.description =
		"Add an event to a clip's timeline, or change one that is already there. With "
		"'index' the event at that position is updated — only the fields that are sent "
		"— and without it a new one is appended, which needs 'name' and 'time'. A "
		"'duration' above zero makes it a notify STATE: Begin when the playhead enters, "
		"End when it leaves.";
	t.inputSchema = objectSchema(json{
		{ "path",     stringProp("Content-relative path of an Animation Clip asset.") },
		{ "index",    numberProp("Which event to change, as reported by clip_info. Omit "
		                         "to append a new one. Notifies have no identity of their "
		                         "own and two may carry the same name, so the position in "
		                         "the list is the only address there is.") },
		{ "name",     stringProp("The event's name — the whole payload. The graph or "
		                         "script listening decides what it means; a name nothing "
		                         "listens for fires and is ignored.") },
		{ "time",     numberProp("Seconds on the clip's timeline, inside the clip.") },
		{ "duration", numberProp("0 (default for a new event) fires once. Above 0 makes "
		                         "it a notify state that is open for that long.") },
	}, { "path" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		Clip c = openClip(*cm, *h, args, /*forWrite=*/true);
		if (!c.ok) return c.failure;

		const bool appending = !hasArg(args, "index");
		const int  index     = intArg(args, "index", -1);

		if (appending)
		{
			if (strArg(args, "name").empty() || !hasArg(args, "time"))
				return ToolResult::fail("invalid_args",
					"Appending an event needs both 'name' and 'time'. An unnamed event "
					"fires nothing and an event without a time would land at 0, which is a "
					"place nobody chose. Pass 'index' instead to change one that exists.");
		}
		else if (index < 0 || index >= static_cast<int>(c.file.notifies.size()))
		{
			return ToolResult::fail("invalid_args",
				"'" + c.rel + "' has " + std::to_string(c.file.notifies.size()) +
				" event(s), so index " + std::to_string(index) + " addresses nothing. "
				"clip_info reports the current indices — they shift when an event is "
				"removed.");
		}

		AnimationClipAsset* clip = loadForWrite(*cm, c.rel);
		if (!clip) return failWrite(c.rel);

		// Re-checked against the LOADED clip rather than against the file read: it
		// is the list about to be written, and the two differ for a clip whose tab
		// reloaded it between the read and here.
		if (!appending && index >= static_cast<int>(clip->notifies.size()))
			return ToolResult::fail("invalid_args",
				"Index " + std::to_string(index) + " addresses nothing in '" + c.rel +
				"' any more — the clip changed between the read and the write. Call "
				"clip_info again.");

		AnimationNotify next = appending ? AnimationNotify{}
		                                 : clip->notifies[static_cast<std::size_t>(index)];
		if (hasArg(args, "name"))     next.name     = strArg(args, "name");
		if (hasArg(args, "time"))     next.time     = static_cast<float>(numArg(args, "time", 0.0));
		if (hasArg(args, "duration")) next.duration = static_cast<float>(numArg(args, "duration", 0.0));

		if (next.name.empty())
			return ToolResult::fail("invalid_args",
				"An event's name is its entire payload, so clearing it would leave an event "
				"that cannot be listened for. Remove it with clip_notify_remove instead.");
		if (next.duration < 0.0f)
			return ToolResult::fail("invalid_args",
				"A negative duration has no meaning: 0 fires once, above 0 is a notify "
				"state that stays open that long.");

		ToolResult failure = ToolResult::ok(json::object());
		if (!timeInClip(next.time, clip->duration, c.rel, failure)) return failure;

		int at = index;
		if (appending)
		{
			clip->notifies.push_back(std::move(next));
			at = static_cast<int>(clip->notifies.size()) - 1;
		}
		else
		{
			clip->notifies[static_cast<std::size_t>(index)] = std::move(next);
		}

		return saveAndReport(*cm, c.rel, *clip,
		                     json{ { "index", at }, { "added", appending } });
	};
	registry.add(std::move(t));
}

// ── clip_notify_remove ───────────────────────────────────────────────────────

void addNotifyRemove(McpToolRegistry& registry, ContentManager& content,
                     const std::shared_ptr<McpClipHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "clip_notify_remove";
	t.description =
		"Take one event off a clip's timeline, addressed by its index. The answer lists "
		"what is left WITH the new indices, because removing one shifts every event "
		"after it.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of an Animation Clip asset.") },
		{ "index", numberProp("Which event to remove, as reported by clip_info.") },
	}, { "path", "index" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		Clip c = openClip(*cm, *h, args, /*forWrite=*/true);
		if (!c.ok) return c.failure;

		const int index = intArg(args, "index", -1);
		if (index < 0 || index >= static_cast<int>(c.file.notifies.size()))
			return ToolResult::fail("invalid_args",
				"'" + c.rel + "' has " + std::to_string(c.file.notifies.size()) +
				" event(s), so index " + std::to_string(index) + " addresses nothing.");

		AnimationClipAsset* clip = loadForWrite(*cm, c.rel);
		if (!clip) return failWrite(c.rel);
		if (index >= static_cast<int>(clip->notifies.size()))
			return ToolResult::fail("invalid_args",
				"Index " + std::to_string(index) + " addresses nothing in '" + c.rel +
				"' any more — the clip changed between the read and the write. Call "
				"clip_info again.");

		const std::string removed = clip->notifies[static_cast<std::size_t>(index)].name;
		clip->notifies.erase(clip->notifies.begin() + index);
		return saveAndReport(*cm, c.rel, *clip, json{ { "removed", removed } });
	};
	registry.add(std::move(t));
}

// ── clip_root_motion_set ─────────────────────────────────────────────────────

void addRootMotionSet(McpToolRegistry& registry, ContentManager& content,
                      const std::shared_ptr<McpClipHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "clip_root_motion_set";
	t.description =
		"Set whether this clip's root bone carries motion that belongs on the entity. "
		"Per clip, and the other half of what the Skeletal Mesh Editor authors: an idle "
		"with a pinned root should not be treated like a roll. An entity also needs a "
		"RootMotionComponent before anything happens with it.";
	t.inputSchema = objectSchema(json{
		{ "path",    stringProp("Content-relative path of an Animation Clip asset.") },
		{ "enabled", json{ { "type", "boolean" },
		                   { "description", "true = the root's movement drives the entity, "
		                                    "false = the root is treated as pinned." } } },
	}, { "path", "enabled" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		Clip c = openClip(*cm, *h, args, /*forWrite=*/true);
		if (!c.ok) return c.failure;
		if (!hasArg(args, "enabled"))
			return ToolResult::fail("invalid_args",
				"'enabled' is the whole point of this call, so it is not optional — "
				"clip_info reports the current setting.");

		AnimationClipAsset* clip = loadForWrite(*cm, c.rel);
		if (!clip) return failWrite(c.rel);

		clip->hasRootMotion = boolArg(args, "enabled");
		return saveAndReport(*cm, c.rel, *clip, json::object());
	};
	registry.add(std::move(t));
}

} // namespace

void registerClipTools(McpToolRegistry& registry, ContentManager& content, McpClipHooks hooks)
{
	// Shared rather than copied into four lambdas: the hooks hold std::function
	// objects and every handler asks the same four questions of them.
	auto h = std::make_shared<McpClipHooks>(std::move(hooks));
	addInfo(registry, content, h);
	addNotifySet(registry, content, h);
	addNotifyRemove(registry, content, h);
	addRootMotionSet(registry, content, h);
}

} // namespace HE::Ed
