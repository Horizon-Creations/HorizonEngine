#pragma once

// ─── What a newborn asset of a given type contains ───────────────────────────
// One function, and the reason it is a function at all: until now this lived as
// a lambda inside ContentBrowserPanel's create menu, which meant the answer to
// "what is inside a brand-new Input Action" was only reachable from an ImGui
// popup. The MCP asset tools have to create the same files, and a second copy
// would be a second theory — the one that quietly forgets CHUNK_SLNG and gives a
// Python project a Lua script, or writes an empty widget the editor then cannot
// open.
//
// So the panel and the tools call this, and the panel keeps what is genuinely
// its own: choosing a free name, moving the selection, opening the rename popup,
// announcing the create to a collaboration session.
//
// Deliberately free of ImGui and of AppContext, so the tools' test binary can
// create every asset type the editor offers and then load it back.

#include <Scripting/ScriptTypes.h>
#include <Types/Enums.h>

#include <string>

namespace HE::Ed
{

// Everything that varies between two assets of the SAME type. Both fields are
// ignored for types that have no use for them.
struct AssetStubSpec
{
	// Script only. The language byte (CHUNK_SLNG) is the single source of truth
	// for routing Lua vs Python, so it is written at birth.
	HE::ScriptLanguage scriptLanguage = HE::ScriptLanguage::Lua;

	// HorizonCode class only. The base class decides the event catalog (input
	// events on PlayerController/PlayerCharacter), so it is part of the asset's
	// identity from birth. Empty = a plain Object, the spelling every asset
	// predating the taxonomy already carries.
	std::string horizonCodeBaseClass;
};

// Write a minimal .hasset at `absolutePath`. `contentRelativePath` is what goes
// into the META chunk as the asset's own stored path (the packer builds its
// path→UUID map from it), `displayName` is the asset's name chunk.
//
// The UUID minted here is the asset's permanent identity. Returns false when the
// file could not be written; nothing is left behind in that case beyond whatever
// the writer managed, which is what the caller's error path reports.
//
// Does NOT touch EditorAssetTypeCache: a path that was probed while it was still
// free holds a stale entry, and dropping it is the CALLER's job — the panel does
// it for the path it chose, the tools for theirs.
bool writeAssetStub(const std::string& absolutePath,
                    const std::string& contentRelativePath,
                    const std::string& displayName,
                    HE::AssetType      type,
                    const AssetStubSpec& spec = {});

// Can `writeAssetStub` produce a usable asset of this type at all?
//
// False for everything that is IMPORTED rather than authored (a mesh, a texture,
// an audio file: a stub with no payload is a file the editor can only fail to
// open) and for Scene, whose file is not an .hasset at all. The create menu
// answers the same question by simply not offering a row; this is that list in a
// form a tool can check an argument against.
bool isCreatableAssetType(HE::AssetType type);

// ── The one newborn file that is not an .hasset ──────────────────────────────
// A scene is JSON at a .hescene path, so `writeAssetStub` cannot make one: it
// would write an HAsset container, and SceneSerializer::loadJSON refuses that as
// "not valid JSON" — a file the Content Browser shows, offers to open, and then
// cannot. (It did exactly that until the scene tools were written: the create
// menu's "Scene" row called writeAssetStub with AssetType::Scene.)
//
// What it writes is what an empty world serialises to, not a hand-written
// literal, so a scene created here and a scene saved by File > Save As go
// through the SAME code and cannot drift apart. Same principle as the stub
// writer above: one writer, two callers, no second theory of what a newborn
// file contains.
bool writeEmptySceneFile(const std::string& absolutePath);

// `HE::assetTypeName`'s inverse, for the boundaries that carry the type as a
// string — the MCP tools' `type` argument. Unknown for a name nothing matches.
// Case-sensitive on purpose: the names are the enumerator spellings, and a
// forgiving match here is how "material" and "Material" become two vocabularies.
HE::AssetType assetTypeFromName(const std::string& name);

} // namespace HE::Ed
