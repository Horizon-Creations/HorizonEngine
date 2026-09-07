#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>
#include <vector>

// Skeletal Mesh Editor — a top-level tab opened by double-clicking a SkeletalMesh
// .hasset in the Content Browser. Shows the joint hierarchy as a tree (name +
// parent/child indentation, straight from SkeletalMeshAsset::skeleton) and a live
// preview (orbit camera, same interaction as MaterialEditorPanel) rendered via
// IRenderer::RenderSkeletalPreview. Optionally scrubs an AnimationClip against the
// mesh (AnimationPreview::evaluateClipPose, HE_Scene).
//
// The scrub half is no longer view-only: the clip's NOTIFIES are authored here, on
// a timeline under the scrub slider, and its per-clip Root Motion switch is set
// here too. Both live in the CLIP asset, not in the mesh — the tab shows a mesh
// because a notify placed without a visible pose is a guess, which is the whole
// reason the timeline sits in this tab rather than in an editor of its own.
//
// Still nothing here touches an ECS entity: the preview poses an asset.
namespace SkeletalMeshEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Whether the .hasset at `path` is a skeletal mesh asset (reads the HAsset
	// header type; cached per path — same convention as MaterialEditorPanel).
	bool isSkeletalMeshAsset(const std::string& path);

	// ── Unsaved clip edits ───────────────────────────────────────────────────
	// Keyed by the CLIP's path, not by the tab's: the tab shows a mesh and the
	// edits belong to the clip scrubbed in it. So a dirty clip is reported (and
	// saved) under its own name, which is also what makes two tabs scrubbing the
	// same clip agree — there is one entry, not one per tab.
	bool isDirty(const std::string& assetPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& assetPath);

	// The unsaved clip open in the tab at `tabPath`, or "". This is what Ctrl+S
	// needs: the shortcut saves the ACTIVE TAB, and this tab's own path is a mesh
	// nobody has edits for — so without the question the keystroke would land on
	// the mesh, find nothing, report success and leave the clip dirty.
	std::string dirtyClipForTab(const std::string& tabPath);

	// Drop cached editor state for `path` (content-browser rename/delete). Takes
	// a mesh path (the tab) or a clip path (its pending edits) — the asset that
	// went away is not necessarily the one the tab is named after.
	void forget(const std::string& assetPath);
}
