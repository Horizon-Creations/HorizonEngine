#pragma once
#include <glm/glm.hpp>
#include <string>

class EditorCamera;

// ── Camera bookmarks ────────────────────────────────────────────────────────
// Ten remembered views of the scene camera, on the digit keys the way Unreal
// has them: Ctrl+<digit> stores where the camera is, <digit> jumps back there.
// "The corner of the map I keep flying to" is a thing every level has three
// of, and flying there by hand each time is the cost this removes.
//
// A bookmark is the camera's whole pose — position, heading, orbit distance
// and whether it is orthographic — so recalling an axis view brings the lens
// state with it, not just the place. Pure data, no ImGui: the viewport reads
// the keys and the toolbar draws the rows, this only remembers.
//
// Shared by every viewport (the Scene window and the secondary ones), because
// a bookmark is a place in the level, not a property of the pane that set it.
//
// Persisted with the editor config as one string (`encode` / `decode`), next
// to the last camera view — global like that view is, not per project. A
// project-scoped editor state would be the better home once one exists.
namespace CameraBookmarks
{
	constexpr int kSlots = 10;   // the digits 0..9

	struct Bookmark
	{
		bool      set           = false;
		glm::vec3 position{ 0.0f };
		float     yaw           = 0.0f;   // radians
		float     pitch         = 0.0f;   // radians
		float     pivotDistance = 0.0f;
		bool      orthographic  = false;
	};

	struct Set
	{
		Bookmark slots[kSlots];

		// Remember the camera's current pose in `slot` (0..9; anything else is
		// ignored). Overwrites whatever was there.
		void store(int slot, const EditorCamera& cam);
		// Put the camera where `slot` remembers. False, and the camera untouched,
		// when the slot is empty or out of range.
		bool recall(int slot, EditorCamera& cam) const;
		void clear(int slot);
		void clearAll();
		bool any() const;
		bool isSet(int slot) const;

		// One line for the config file: only the set slots, as
		//   "<slot>:<x>,<y>,<z>,<yaw>,<pitch>,<pivot>,<ortho>;" …
		// decode() tolerates an empty or damaged string (it yields whatever
		// parsed cleanly), so a config from before bookmarks existed is simply
		// "no bookmarks".
		std::string encode() const;
		static Set  decode(const std::string& text);
	};

	// The editor's one set — the viewports and the toolbar all read this.
	Set& editorSet();
}
