#include "McpToolRegistry.h"

#include "CinematicTimeline.h"        // lastContentTime — the Cinematic tab's length rule
#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk
#include "SequencerTimeline.h"        // targetName — the label a property track wears

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <GraphCommon/GraphJson.h>
#include <Sequence/SequenceJson.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Authoring a cutscene from outside the editor ────────────────────────────
// Why the unit is the whole document, why it is translated and why doubt is a
// refusal: McpToolRegistry.h, beside McpSequenceHooks. What is worth stating
// HERE is what the handlers promise.
//
//   • READ AND WRITE SPEAK ONE FORM. `sequence_info` reports the document in
//     the tool vocabulary (entity uuids as entity_list prints them, assets as
//     paths, targets and curves as names), and `sequence_write` accepts exactly
//     that. The chunk's raw {"hi","lo"} form is accepted too, so a reference
//     whose file is gone can still be written back unchanged.
//
//   • THE QUESTION IS ANSWERED FROM THE FILE. Reading parses CHUNK_SEQU out of
//     the .hasset and never registers the asset; resolving a clip's path reads
//     file headers, never loads a clip.
//
//   • THE WRITE GOES THROUGH THE LOADED SEQUENCE. `saveAsset` writes from the
//     resident asset, which is also what a running player and an open tab read —
//     writing the file behind its back would leave both on the old cutscene.
//     That load is the only one on the write path, and nothing is loaded after
//     it, so the pointer cannot move under us (ContentManager.h: dense pools).
//
//   • A REFUSAL IS A NO-OP. Nothing is written, nothing is loaded and no tab is
//     told to re-read anything.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── The vocabulary ───────────────────────────────────────────────────────────

std::string uuidHex(const HE::UUID& id)
{
	char buf[33];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx",
	              static_cast<unsigned long long>(id.hi),
	              static_cast<unsigned long long>(id.lo));
	return std::string(buf);
}

// The 32-digit form uuidOf (EditorCommands.cpp) prints, and nothing looser: a
// uuid with a digit missing is a typo, not a shorter id.
bool uuidFromHex(const std::string& s, HE::UUID& out)
{
	if (s.size() != 32) return false;
	for (char c : s)
		if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
	out.hi = std::strtoull(s.substr(0, 16).c_str(), nullptr, 16);
	out.lo = std::strtoull(s.substr(16).c_str(), nullptr, 16);
	return true;
}

// A reference as the tool accepts it: the hex string, or the chunk's own object.
bool uuidFromAny(const json& j, HE::UUID& out)
{
	if (j.is_string()) return uuidFromHex(j.get<std::string>(), out);
	if (j.is_object() && j.contains("hi") && j.contains("lo"))
	{
		out = HE::graph::uuidFromJson(j);
		return true;
	}
	return false;
}

std::string allTargetNames()
{
	std::string s;
	for (int i = 0; i < Sequencer::kTargetCount; ++i)
		s += (s.empty() ? "" : ", ") + std::string(Sequencer::targetName(static_cast<PropTarget>(i)));
	return s;
}

int targetByName(const std::string& name)
{
	for (int i = 0; i < Sequencer::kTargetCount; ++i)
		if (name == Sequencer::targetName(static_cast<PropTarget>(i))) return i;
	return -1;
}

const char* curveName(int c)
{
	switch (c)
	{
	case static_cast<int>(SequenceBlendCurve::Linear):  return "linear";
	case static_cast<int>(SequenceBlendCurve::EaseOut): return "easeOut";
	default:                                            return "smoothStep";
	}
}

int curveByName(const std::string& name)
{
	if (name == "linear")     return static_cast<int>(SequenceBlendCurve::Linear);
	if (name == "smoothStep") return static_cast<int>(SequenceBlendCurve::SmoothStep);
	if (name == "easeOut")    return static_cast<int>(SequenceBlendCurve::EaseOut);
	return -1;
}

// ── What the file says, without loading it ───────────────────────────────────
// An absent chunk is not a failure: AssetStubWriter writes a new sequence
// without one, and the Cinematic tab shows it as empty.
bool readSequenceFile(const std::string& abs, SequenceAsset& out)
{
	HAsset::Reader r;
	if (!r.open(abs)) return false;
	const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_SEQU);
	if (!c) return true;
	const std::string text(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	return sequenceFromJson(text, out);
}

// uuid → content path for every clip and sound in the project, by header only.
// Built only when the sequence references an asset at all.
using PathIndex = std::unordered_map<HE::UUID, std::string>;

PathIndex assetPaths(ContentManager& content)
{
	PathIndex out;
	bool truncated = false;
	for (const ContentAsset& a : walkContentAssets(
	         content, { HE::AssetType::AnimationClip, HE::AssetType::Audio }, 5000, truncated))
	{
		const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);
		if (id != HE::UUID{}) out[id] = a.rel;
	}
	return out;
}

// ── Chunk form → tool form ───────────────────────────────────────────────────
// Each reference that becomes a path keeps its uuid when no file carries it, so
// the document still writes back unchanged; `unresolved` says which.
json toToolForm(const SequenceAsset& s, ContentManager& content, json& unresolved)
{
	json doc = json::parse(sequenceToJson(s));
	unresolved = json::array();

	std::vector<HE::UUID> refs;
	sequenceAssetRefs(s, refs);
	const PathIndex paths = refs.empty() ? PathIndex{} : assetPaths(content);
	auto refToTool = [&](json& obj, const char* rawKey, const char* pathKey) {
		const auto it = obj.find(rawKey);
		if (it == obj.end()) return;
		const HE::UUID id = HE::graph::uuidFromJson(*it);
		obj.erase(rawKey);
		const auto p = paths.find(id);
		if (p != paths.end()) obj[pathKey] = p->second;
		else
		{
			obj[rawKey] = uuidHex(id);
			unresolved.push_back(uuidHex(id));
		}
	};

	for (json& b : doc["bindings"])
	{
		b["entity"] = uuidHex(HE::graph::uuidFromJson(b["entityId"]));
		b.erase("entityId");
	}
	for (json& t : doc["tracks"])
	{
		const std::string kind = t.value("kind", std::string());
		if (kind == "property")
			t["target"] = Sequencer::targetName(static_cast<PropTarget>(t.value("target", 0)));
		else if (kind == "skeletal")
			for (json& sec : t["sections"]) refToTool(sec, "clipId", "clip");
		else if (kind == "audio")
			for (json& sec : t["sections"]) refToTool(sec, "assetId", "sound");
		else if (kind == "cameraCut")
			for (json& c : t["cuts"]) c["curve"] = curveName(c.value("curve", 1));
	}
	return doc;
}

// ── Tool form → chunk form ───────────────────────────────────────────────────
// Only the references and names are translated; every other field goes through
// to the loader untouched, so its tolerance rules stay the only ones.
struct Translation
{
	json        chunk;
	std::string error;   // empty = fine
};

// A clip or sound named by path: inside the project, of the right type, and
// carrying a uuid. Reads a header; loads nothing.
bool assetRefFromPath(ContentManager& content, const std::string& raw, HE::AssetType want,
                      const char* what, HE::UUID& out, std::string& error)
{
	const PathCheck p = checkPath(content, raw, /*mustExist=*/true, what);
	if (!p.ok)
	{
		error = "'" + raw + "' (" + what + ") is not a file in the project. clip_info lists "
		        "the animation clips, asset_list the sounds.";
		return false;
	}
	if (EditorAssetTypeCache::assetTypeOf(p.abs) != want)
	{
		error = "'" + p.rel + "' (" + what + ") is not " +
		        (want == HE::AssetType::AnimationClip ? "an Animation Clip" : "an Audio asset") + ".";
		return false;
	}
	bool unreadable = false;
	out = HE::AssetRefs::assetUuidOfFile(p.abs, &unreadable);
	if (out == HE::UUID{})
	{
		error = "'" + p.rel + "' carries no asset id" +
		        (unreadable ? std::string(" (the file could not be read)") : std::string()) +
		        ", so a sequence could not find it again.";
		return false;
	}
	return true;
}

Translation fromToolForm(const json& doc, ContentManager& content)
{
	Translation tr;
	if (!doc.is_object())
	{
		tr.error = "'sequence' has to be a JSON object: the document sequence_info reports.";
		return tr;
	}
	json out = doc;

	if (out.contains("bindings"))
	{
		if (!out["bindings"].is_array()) { tr.error = "'bindings' has to be an array."; return tr; }
		int i = 0;
		for (json& b : out["bindings"])
		{
			const std::string at = "bindings[" + std::to_string(i++) + "]";
			if (!b.is_object()) { tr.error = at + " is not an object."; return tr; }
			const json* ref = b.contains("entity") ? &b["entity"]
			                : b.contains("entityId") ? &b["entityId"] : nullptr;
			HE::UUID id;
			if (!ref || !uuidFromAny(*ref, id) || id == HE::UUID{})
			{
				tr.error = at + " needs 'entity': the 32-digit uuid entity_list reports for the "
				           "actor. A binding is resolved by that uuid alone, never by its name.";
				return tr;
			}
			b.erase("entity");
			b["entityId"] = HE::graph::uuidToJson(id);
			if (!b.contains("slot") || !b["slot"].is_number_integer())
			{
				tr.error = at + " needs an integer 'slot': the number tracks and cuts use to "
				           "name this actor.";
				return tr;
			}
		}
	}

	if (out.contains("tracks"))
	{
		if (!out["tracks"].is_array()) { tr.error = "'tracks' has to be an array."; return tr; }
		int i = 0;
		for (json& t : out["tracks"])
		{
			const std::string at = "tracks[" + std::to_string(i++) + "]";
			if (!t.is_object()) { tr.error = at + " is not an object."; return tr; }
			const std::string kind = t.value("kind", std::string());
			if (kind == "property")
			{
				if (t.contains("target") && t["target"].is_string())
				{
					const int target = targetByName(t["target"].get<std::string>());
					if (target < 0)
					{
						tr.error = at + ": unknown target '" + t["target"].get<std::string>() +
						           "'. The targets are: " + allTargetNames() + ".";
						return tr;
					}
					t["target"] = target;
				}
			}
			else if (kind == "skeletal" || kind == "audio")
			{
				const bool  skel    = kind == "skeletal";
				const char* pathKey = skel ? "clip" : "sound";
				const char* rawKey  = skel ? "clipId" : "assetId";
				if (!t.contains("sections")) continue;
				if (!t["sections"].is_array()) { tr.error = at + ".sections has to be an array."; return tr; }
				int k = 0;
				for (json& sec : t["sections"])
				{
					const std::string sat = at + ".sections[" + std::to_string(k++) + "]";
					if (!sec.is_object()) { tr.error = sat + " is not an object."; return tr; }
					HE::UUID id;
					if (sec.contains(pathKey) && sec[pathKey].is_string())
					{
						std::string err;
						if (!assetRefFromPath(content, sec[pathKey].get<std::string>(),
						                      skel ? HE::AssetType::AnimationClip : HE::AssetType::Audio,
						                      pathKey, id, err))
						{
							tr.error = sat + ": " + err;
							return tr;
						}
						sec.erase(pathKey);
					}
					else if (!sec.contains(rawKey) || !uuidFromAny(sec[rawKey], id) || id == HE::UUID{})
					{
						tr.error = sat + " needs '" + pathKey + "': the content-relative path of the " +
						           (skel ? "clip it plays." : "sound it starts.");
						return tr;
					}
					sec[rawKey] = HE::graph::uuidToJson(id);
				}
			}
			else if (kind == "cameraCut")
			{
				if (!t.contains("cuts")) continue;
				if (!t["cuts"].is_array()) { tr.error = at + ".cuts has to be an array."; return tr; }
				for (json& c : t["cuts"])
				{
					if (!c.is_object() || !c.contains("curve") || !c["curve"].is_string()) continue;
					const int curve = curveByName(c["curve"].get<std::string>());
					if (curve < 0)
					{
						tr.error = at + ": unknown curve '" + c["curve"].get<std::string>() +
						           "'. The curves are: linear, smoothStep, easeOut.";
						return tr;
					}
					c["curve"] = curve;
				}
			}
			// Any other kind goes to the loader as it is: it drops what it does not
			// know, and the dropped count turns that into a refusal below.
		}
	}
	tr.chunk = std::move(out);
	return tr;
}

// ── What the runtime would silently get wrong ────────────────────────────────
// Every rule here is one the Cinematic tab keeps by construction and a
// hand-written document can break. Empty = fine.
std::string checkSequence(const SequenceAsset& s)
{
	auto num = [](float v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };

	if (!std::isfinite(s.duration) || s.duration < 0.0f) return "'duration' must be 0 or more seconds.";
	if (!(s.frameRate > 0.0f)) return "'frameRate' must be above 0.";

	std::vector<uint16_t> slots;
	for (const SequenceBinding& b : s.bindings)
	{
		if (b.slot == kSequenceNoBinding)
			return "slot " + std::to_string(kSequenceNoBinding) + " is reserved for \"no actor\".";
		for (uint16_t o : slots)
			if (o == b.slot)
				return "two bindings share slot " + std::to_string(b.slot) +
				       "; a slot names exactly one actor.";
		slots.push_back(b.slot);
	}
	auto known = [&](uint16_t slot) {
		if (slot == kSequenceNoBinding) return true;
		for (uint16_t o : slots) if (o == slot) return true;
		return false;
	};

	int cutTracks = 0;
	for (std::size_t i = 0; i < s.tracks.size(); ++i)
	{
		const SequenceTrack& t  = s.tracks[i];
		const std::string    at = "tracks[" + std::to_string(i) + "]";
		if (!known(t.binding))
			return at + " names slot " + std::to_string(t.binding) + ", which no binding has.";
		switch (t.kind)
		{
		case SequenceTrackKind::Property:
			if (t.binding == kSequenceNoBinding)
				return at + " is a property track without an actor ('binding'), which animates nothing.";
			if (t.channel.times.empty()) return at + " has no keys.";
			for (std::size_t k = 0; k < t.channel.times.size(); ++k)
			{
				if (!std::isfinite(t.channel.times[k]) || t.channel.times[k] < 0.0f ||
				    !std::isfinite(t.channel.values[k]))
					return at + " key " + std::to_string(k) + " is not a finite time/value.";
				if (k > 0 && t.channel.times[k] < t.channel.times[k - 1])
					return at + ": key times have to be in ascending order (key " +
					       std::to_string(k) + " at " + num(t.channel.times[k]) + " comes after " +
					       num(t.channel.times[k - 1]) + ").";
			}
			break;
		case SequenceTrackKind::Skeletal:
			if (t.binding == kSequenceNoBinding)
				return at + " is a skeletal track without an actor ('binding'), so no skeleton plays it.";
			for (std::size_t k = 0; k < t.sections.size(); ++k)
			{
				const SequenceSkeletalSection& sec = t.sections[k];
				if (!(sec.start >= 0.0f) || !(sec.end > sec.start))
					return at + " section " + std::to_string(k) + " must start at 0 or later and end after it starts.";
				if (!(sec.playRate > 0.0f))
					return at + " section " + std::to_string(k) + " needs a playRate above 0.";
			}
			break;
		case SequenceTrackKind::CameraCut:
			if (++cutTracks > 1)
				return "there are two cameraCut tracks, and the runtime only ever reads the first; "
				       "put every cut into one.";
			for (std::size_t k = 0; k < t.cuts.size(); ++k)
			{
				if (!(t.cuts[k].time >= 0.0f))
					return at + " cut " + std::to_string(k) + " is before 0.";
				if (!known(t.cuts[k].binding))
					return at + " cut " + std::to_string(k) + " names slot " +
					       std::to_string(t.cuts[k].binding) + ", which no binding has.";
			}
			break;
		case SequenceTrackKind::Event:
			for (std::size_t k = 0; k < t.events.size(); ++k)
			{
				if (t.events[k].name.empty())
					return at + " event " + std::to_string(k) + " has no name, and the name is all a listener gets.";
				if (!(t.events[k].time >= 0.0f) || !(t.events[k].duration >= 0.0f))
					return at + " event " + std::to_string(k) + " needs a time and duration of 0 or more.";
			}
			break;
		case SequenceTrackKind::Audio:
			for (std::size_t k = 0; k < t.audio.size(); ++k)
				if (!(t.audio[k].start >= 0.0f) || !(t.audio[k].volume >= 0.0f) || !(t.audio[k].pitch > 0.0f))
					return at + " sound " + std::to_string(k) + " needs start >= 0, volume >= 0 and pitch > 0.";
			break;
		}
	}

	const float last = Cinematic::lastContentTime(s);
	if (s.duration + 1e-4f < last)
		return "'duration' is " + num(s.duration) + " s but something happens at " + num(last) +
		       " s. A sequence ends when its duration says, so the rest would never play; "
		       "make the duration at least " + num(last) + ".";
	return {};
}

// Who each binding is in the open scene. `null` when there is no scene to ask.
json actorsJson(const SequenceAsset& s, const McpSequenceHooks& h)
{
	if (!h.findActor) return nullptr;
	std::vector<uint16_t> cutSlots;
	for (const SequenceTrack& t : s.tracks)
		if (t.kind == SequenceTrackKind::CameraCut)
			for (const SequenceCameraCut& c : t.cuts) cutSlots.push_back(c.binding);

	json out = json::array();
	for (const SequenceBinding& b : s.bindings)
	{
		std::string name;
		bool        isCamera = false;
		const bool  found    = h.findActor(b.entityId, name, isCamera);
		json a{ { "slot", b.slot }, { "entity", uuidHex(b.entityId) }, { "found", found } };
		if (found)
		{
			a["sceneName"] = name;
			a["isCamera"]  = isCamera;
			bool cutTo = false;
			for (uint16_t c : cutSlots) cutTo = cutTo || c == b.slot;
			if (cutTo && !isCamera)
				a["warning"] = "a cut goes to this actor, but it has no CameraComponent, so the "
				               "cut shows nothing.";
		}
		out.push_back(std::move(a));
	}
	return out;
}

json stateJson(const std::string& rel, const SequenceAsset& s, ContentManager& content,
               const McpSequenceHooks& h)
{
	json unresolved;
	json out{
		{ "path",     rel },
		{ "sequence", toToolForm(s, content, unresolved) },
	};
	const json actors = actorsJson(s, h);
	if (actors.is_null()) out["scene"] = "no scene is open, so the bindings were not looked up";
	else                  out["actors"] = actors;
	if (!unresolved.empty()) out["unresolvedAssets"] = unresolved;
	return out;
}

// ── The addressed sequence ───────────────────────────────────────────────────
// The path check, the play-mode gate, the lock gate and the unsaved-tab gate,
// once — the same four questions and order as the clip and particle tools.
struct Seq
{
	std::string   rel;
	std::string   abs;
	SequenceAsset file;
	bool          ok      = false;
	ToolResult    failure = ToolResult::ok(json::object());
};

Seq openSequence(ContentManager& content, const McpSequenceHooks& h, const json& args, bool forWrite)
{
	Seq s;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { s.failure = p.failure; return s; }
	s.rel = p.rel;
	s.abs = p.abs;

	if (EditorAssetTypeCache::assetTypeOf(p.abs) != HE::AssetType::Sequence)
	{
		s.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Sequence asset. sequence_info without arguments lists "
			"every sequence in the project; asset_create with type 'Sequence' makes a new one.");
		return s;
	}
	if (!readSequenceFile(p.abs, s.file))
	{
		s.failure = ToolResult::fail("failed",
			"'" + p.rel + "' could not be read. The editor log carries the reason.");
		return s;
	}

	if (forWrite)
	{
		if (p.engine) { s.failure = failEngineReadOnly(p.rel); return s; }
		if (h.isPlaying && h.isPlaying())
		{
			s.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and a sequence rewritten under a running player "
				"changes a cutscene mid-shot. Ask the user to stop play mode.");
			return s;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			s.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return s;
		}
		if (h.isDirty && h.isDirty(p.rel))
		{
			s.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' has unsaved edits in an open Cinematic tab. Writing now would "
				"throw them away or be thrown away by their Save. Ask the user to save or "
				"discard first, then call again.");
			return s;
		}
	}
	s.ok = true;
	return s;
}

// ── sequence_info ────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content,
             const std::shared_ptr<McpSequenceHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "sequence_info";
	t.description =
		"Without arguments: every Sequence (cutscene) asset in the project with its length and "
		"how many actors and tracks it has. With 'path': the whole sequence as a document — "
		"duration, frameRate, bindings (slot, name, entity uuid) and tracks (property keys, "
		"skeletal clip sections, camera cuts, events, sounds) — plus who each binding is in "
		"the open scene. The document is exactly what sequence_write takes.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of a Sequence asset, e.g. "
		                      "'Cinematics/Intro.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Maximum number of assets in the catalogue (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!hasArg(args, "path"))
		{
			bool truncated = false;
			const std::vector<ContentAsset> found = walkContentAssets(
				*cm, { HE::AssetType::Sequence }, intArg(args, "limit", 200), truncated);
			json list = json::array();
			for (const ContentAsset& a : found)
			{
				SequenceAsset s;
				const bool read = readSequenceFile(a.abs, s);
				json row{ { "path", a.rel } };
				if (!read) row["unreadable"] = true;
				else
				{
					row["duration"]     = s.duration;
					row["bindingCount"] = static_cast<int>(s.bindings.size());
					row["trackCount"]   = static_cast<int>(s.tracks.size());
				}
				list.push_back(std::move(row));
			}
			json out{ { "sequences", std::move(list) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		}

		Seq s = openSequence(*cm, *h, args, /*forWrite=*/false);
		if (!s.ok) return s.failure;
		json out = stateJson(s.rel, s.file, *cm, *h);
		// The two closed vocabularies, so a client never guesses a spelling.
		json targets = json::array();
		for (int i = 0; i < Sequencer::kTargetCount; ++i)
			targets.push_back(Sequencer::targetName(static_cast<PropTarget>(i)));
		out["propertyTargets"] = std::move(targets);
		out["blendCurves"]     = json::array({ "linear", "smoothStep", "easeOut" });
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── sequence_write ───────────────────────────────────────────────────────────

void addWrite(McpToolRegistry& registry, ContentManager& content,
              const std::shared_ptr<McpSequenceHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "sequence_write";
	t.description =
		"Replace a Sequence's whole content with 'sequence' — the document sequence_info "
		"reports, changed as needed. Actors are bound by 'entity' (the uuid entity_list "
		"reports), clips by 'clip' and sounds by 'sound' (content paths), property targets and "
		"cut curves by name. Anything the runtime would silently get wrong — a slot two actors "
		"share, a track naming a slot nobody has, keys out of order, a second camera-cut "
		"track, content past the duration — is refused with the reason, and then nothing is "
		"written. A clean open Cinematic tab re-reads the file; one with unsaved edits is "
		"refused.";
	t.inputSchema = objectSchema(json{
		{ "path",     stringProp("Content-relative path of a Sequence asset.") },
		{ "sequence", json{ { "type", "object" },
		                    { "description", "The whole document: duration, frameRate, "
		                                     "bindings[], tracks[] — as sequence_info reports it." } } },
	}, { "path", "sequence" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		Seq s = openSequence(*cm, *h, args, /*forWrite=*/true);
		if (!s.ok) return s.failure;

		const auto doc = args.find("sequence");
		if (doc == args.end())
			return ToolResult::fail("invalid_args",
				"'sequence' is the content to write; sequence_info reports the current one.");

		const Translation tr = fromToolForm(*doc, *cm);
		if (!tr.error.empty()) return ToolResult::fail("invalid_args", tr.error);

		SequenceAsset scratch;
		int dropped = 0;
		if (!sequenceFromJson(tr.chunk.dump(), scratch, &dropped))
			return ToolResult::fail("invalid_args",
				"The document does not parse as a sequence (a field of the wrong type, such as a "
				"string where a number belongs). Compare it with what sequence_info reports.");
		if (dropped > 0)
			return ToolResult::fail("invalid_args",
				std::to_string(dropped) + " entr" + (dropped == 1 ? "y" : "ies") + " would be "
				"dropped on load: a track kind this build does not know (property, skeletal, "
				"cameraCut, event, audio), a property target out of range, a property track whose "
				"'times' and 'values' differ in length, or a list entry that is not an object. "
				"Nothing was written.");
		const std::string problem = checkSequence(scratch);
		if (!problem.empty())
			return ToolResult::fail("invalid_args", problem + " Nothing was written.");

		// The one load on this path (see the promise at the top of the file).
		const HE::UUID id = cm->loadAsset(s.rel);
		SequenceAsset* live = id == HE::UUID{} ? nullptr : cm->getSequenceMutable(id);
		if (!live)
			return ToolResult::fail("failed",
				"'" + s.rel + "' could not be loaded to write it. The editor log carries the reason.");

		const SequenceAsset before = *live;
		live->duration  = scratch.duration;
		live->frameRate = scratch.frameRate;
		live->bindings  = std::move(scratch.bindings);
		live->tracks    = std::move(scratch.tracks);
		if (!cm->saveAsset(*live))
		{
			// Put the resident copy back: a tab or a player must not run a cutscene
			// that is not the one on disk.
			*live = before;
			return ToolResult::fail("failed",
				"Could not write '" + s.rel + "'. A read-only file or a full disk is the usual "
				"cause; the editor log carries the reason.");
		}

		const bool reloaded = h->reloadFromDisk && h->reloadFromDisk(s.rel);
		json out = stateJson(s.rel, *live, *cm, *h);
		out["written"]    = true;
		out["tabReloaded"] = reloaded;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

} // namespace

void registerSequenceTools(McpToolRegistry& registry, ContentManager& content,
                           McpSequenceHooks hooks)
{
	auto h = std::make_shared<McpSequenceHooks>(std::move(hooks));
	addInfo(registry, content, h);
	addWrite(registry, content, h);
}

} // namespace HE::Ed
