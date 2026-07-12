#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

// Skeletal Mesh Editor — a top-level tab opened by double-clicking a SkeletalMesh
// .hasset in the Content Browser. Shows the joint hierarchy as a tree (name +
// parent/child indentation, straight from SkeletalMeshAsset::skeleton) and a live
// preview (orbit camera, same interaction as MaterialEditorPanel) rendered via
// IRenderer::RenderSkeletalPreview. Optionally scrubs an AnimationClip against the
// mesh (AnimationPreview::evaluateClipPose, HE_Scene) — pure preview, does not
// touch any ECS entity/component.
namespace SkeletalMeshEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Whether the .hasset at `path` is a skeletal mesh asset (reads the HAsset
	// header type; cached per path — same convention as MaterialEditorPanel).
	bool isSkeletalMeshAsset(const std::string& path);

	// Drop cached editor state for `path` (content-browser rename/delete).
	void forget(const std::string& assetPath);
}
