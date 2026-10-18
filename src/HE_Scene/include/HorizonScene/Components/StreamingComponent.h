#pragma once
#include <cstdint>

// ─── Where an entity lives when its scene is split into cells ────────────────
// The Streaming tab splits a big scene into cells that load around the camera
// (CellSplit.h). Which entities go into a cell is the splitter's call, by what
// they carry (its class table: placed, static things move; a camera, a dynamic
// body, an unknown component stay in the base). This is the author's say on top:
//
//   Auto      the splitter decides, as if the component were not there.
//   Resident  stays in the base scene, always loaded — and with it the whole
//             top-level subtree it is in, since a subtree is split as one. For
//             what the base's logic needs at all times: a manager script, a
//             waypoint a level script looks up by name, a quest giver whose
//             state the level asks about.
//
// Resident is the default because adding the component is how an author says
// "keep this"; Auto exists so a prefab placement can take back what its prefab
// asked for. Read by the splitter only: the runtime never sees it.
struct StreamingComponent {
	enum class Mode : uint8_t
	{
		Auto     = 0,
		Resident = 1,
	};
	Mode mode = Mode::Resident;
};
