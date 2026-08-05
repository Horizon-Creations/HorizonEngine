#pragma once

// ── Who else is in the session, in the footer ────────────────────────────────
// A cluster of profile pictures pinned to the right of the editor's footer bar,
// and the participant menu that opens when the mouse rests on it.
//
// The footer is the right home for this: presence is ambient information — you
// want to know at a glance that two other people are in the scene with you,
// without a window open. The Collaboration panel remains the place you go to
// DO something about the session; this is the place you find out about it.
//
// Split in two on purpose. The cluster itself has to be drawn inside the footer
// window (which is rendered before the dockspace so docked panels cannot overlap
// it), while the menu has to be drawn last of all or it would appear underneath
// them. So DrawFooter() records where the cluster ended up and whether it is
// hovered, and DrawOverlay() — called from the editor's overlay pass — draws the
// menu and the ban confirmation on top of everything.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

struct AppContext;
class IRenderer;

namespace HE::Net { struct Participant; }

namespace CollabPresenceBar
{
	// How wide the cluster will be, in pixels; 0 when there is nothing to draw.
	// Separate from DrawFooter because the footer right-aligns its contents and
	// therefore has to know the width BEFORE it places the cursor.
	float FooterWidth(AppContext& ctx);

	// The avatar cluster. Call from inside the footer window, at the position
	// FooterWidth() was used to compute. Draws nothing when there is no session.
	void DrawFooter(AppContext& ctx);

	// The hover menu and the ban confirmation. Call from the editor's overlay
	// pass, after every panel, so nothing can cover it.
	void DrawOverlay(AppContext& ctx);

	// The local user's own picture, drawn `size` pixels square at the cursor —
	// their stored one, or the same coloured-initial fallback everyone else gets.
	// Used by the identity editor in the Collaboration panel, so what the user
	// sees while choosing is exactly what the others will see.
	void DrawLocalAvatar(AppContext& ctx, float size);

	// A participant's picture, same treatment. For the roster in the
	// Collaboration window, so it and the footer show the same faces.
	void DrawAvatar(AppContext& ctx, const HE::Net::Participant& participant, float size);

	// The participant list: pictures, names, what each of them has selected and
	// — for the host — the remove and block actions, followed by the session's
	// blocklist. Shared by the footer menu and the Collaboration window rather
	// than written twice, since two copies of a list with destructive buttons on
	// it is two places for the buttons to disagree about what they do.
	void DrawRoster(AppContext& ctx);

	// Raise the "block this participant?" confirmation. The dialog is drawn by
	// DrawOverlay, so there is one of it however many places offer the action —
	// a destructive confirmation that exists twice is a confirmation that will
	// eventually differ in wording between the two.
	void RequestBan(std::uint32_t participantId, const std::string& name);

	// Where a participant's marker belongs on screen.
	struct MarkerPlacement
	{
		float x = 0.0f, y = 0.0f;
		// True when they are in front of the camera AND inside the image. False
		// means the marker is pinned to the border and `arrowAngle` says which
		// way to turn to find them (radians, screen space, +x right, +y down).
		bool  onScreen   = false;
		float arrowAngle = 0.0f;
	};

	// Project a world position into the viewport image. Pure geometry, and
	// deliberately reachable without ImGui: the Y flip and the behind-the-camera
	// case are exactly the kind of thing that is silently wrong in one backend or
	// one direction, and neither can be spotted by looking at a screenshot of a
	// scene where everyone happens to be in front of you.
	MarkerPlacement PlaceMarker(const glm::mat4& view, const glm::mat4& proj,
	                            const glm::vec3& world,
	                            float rectMinX, float rectMinY,
	                            float rectMaxX, float rectMaxY, float inset);

	// Name tags for the other people in the session, drawn on top of the
	// rendered viewport image. Call from inside the viewport window, after the
	// ImGui::Image, with THIS frame's camera matrices and the rectangle the
	// image occupies on screen.
	//
	// This is the half of the presence marker that cannot be missed: it is drawn
	// over the frame rather than into it, so no amount of scene detail hides it,
	// and someone outside the view is pinned to the nearest edge with an arrow
	// instead of simply not being there. The depth-aware half — rings and a
	// direction arrow that the scene can occlude — is drawn as debug lines in
	// EditorApplication, and the two are deliberately different things.
	void DrawViewportMarkers(AppContext& ctx, const glm::mat4& view, const glm::mat4& proj,
	                         float rectMinX, float rectMinY,
	                         float rectMaxX, float rectMaxY);

	// Release the uploaded avatar textures while the renderer is still alive.
	// Same contract as AssetThumbnailCache::shutdown().
	void Shutdown(IRenderer* renderer);
}
