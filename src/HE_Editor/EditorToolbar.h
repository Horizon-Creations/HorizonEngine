#pragma once

// ── Toolbar look, shared ─────────────────────────────────────────────────────
// The palette and the drawing primitives behind the editor's toolbars: a darker
// band with a hairline, related controls collected in rounded "wells", gaps
// between groups so a row parses as four things instead of eight, and cells that
// are icon-first with a tooltip.
//
// This started as the private top of ViewportToolbar.cpp. It moved here the
// moment a second bar wanted the same look, because the alternative — copying
// nine colour constants and four metrics into another file — is a guarantee that
// the two drift apart the first time anyone restyles either one.
//
// What stays with each bar is its own icons and its own layout. What lives here
// is only what "looking like an editor toolbar" means.
//
// Everything is drawn by hand rather than assembled from ImGui widgets: the bars
// have to look right at any UI font scale and on HiDPI, and they need states
// (armed, dimmed, running) that ImGui's button has no vocabulary for.

struct AppContext;

#ifdef HE_IMGUI_ENABLED

#include <imgui.h>

namespace EditorToolbar
{

// ── Palette ──────────────────────────────────────────────────────────────────
// The editor theme is deliberately greyscale, so the bars stay greyscale too and
// spend their one colour on state. Nothing decorative is coloured.
inline constexpr ImU32 kBarBg     = IM_COL32( 21,  21,  21, 255);
inline constexpr ImU32 kBarLine   = IM_COL32( 44,  44,  44, 255);
inline constexpr ImU32 kWellBg    = IM_COL32( 33,  33,  33, 255);
inline constexpr ImU32 kHoverBg   = IM_COL32(255, 255, 255,  20);
inline constexpr ImU32 kDownBg    = IM_COL32(255, 255, 255,  38);
inline constexpr ImU32 kOnBg      = IM_COL32( 56, 108, 178, 255);
inline constexpr ImU32 kOnBgHover = IM_COL32( 72, 130, 205, 255);
inline constexpr ImU32 kFg        = IM_COL32(198, 198, 198, 255);
inline constexpr ImU32 kFgOn      = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kFgDim     = IM_COL32( 96,  96,  96, 255);

// State colours, shared so "something needs attention" is the same amber
// everywhere and "this went wrong" the same red.
inline constexpr ImU32 kGood      = IM_COL32( 96, 196, 124, 255);
inline constexpr ImU32 kWarn      = IM_COL32(230, 176,  86, 255);
inline constexpr ImU32 kBad       = IM_COL32(222,  92,  92, 255);
inline constexpr ImU32 kGoodWell  = IM_COL32( 96, 196, 124,  28);
inline constexpr ImU32 kBadWell   = IM_COL32(222,  92,  92,  38);

// ── Geometry ─────────────────────────────────────────────────────────────────
inline constexpr float kWellRound = 7.0f;
inline constexpr float kCellRound = 5.0f;
inline constexpr float kWellPad   = 3.0f;   // well border → cell
inline constexpr float kSegGap    = 2.0f;   // cell → cell inside a well
inline constexpr float kGroupGap  = 9.0f;   // well → well
inline constexpr float kEdgeGap   = 8.0f;   // bar edge → first/last well
inline constexpr float kLabelGap  = 6.0f;   // icon → label
inline constexpr float kCellPadX  = 9.0f;   // side padding of a labelled cell

// A glyph drawn into the box `s` centred on `c`. Vectors, not textures: a 16 px
// PNG is wrong at every scale but one.
using IconFn = void (*)(ImDrawList*, const ImVec2&, float, ImU32);

// Stroke width for an icon of size `s`, with a floor so it survives at small
// font scales.
float stroke(float s);

// Everything a bar needs to place things, derived once per frame from the font.
struct Metrics
{
	float bar   = 0.0f;   // strip height
	float cell  = 0.0f;   // interactive cell height (= square icon cell width)
	float icon  = 0.0f;   // glyph box inside a cell
	float wellH = 0.0f;   // cell + padding
	float y     = 0.0f;   // well top, screen space
	float cy    = 0.0f;   // vertical centre of the strip, screen space
};

// Height of a strip in ImGui units. Scales with the font, so a bar keeps its
// proportions under Preferences ▸ UI Font Scale.
float   height();
Metrics metrics(float originY);

// Width of one cell: square when icon-only, icon + label otherwise.
float cellWidth(const Metrics& m, const char* label);

// One interactive cell inside a well. Returns true when clicked. `on` paints the
// armed state, `enabled == false` dims it and swallows the click. A null `label`
// makes it a square icon cell; a null `icon` a text-only one.
bool cell(const Metrics& m, float x, float w, const char* id, IconFn icon,
          const char* label, bool on, bool enabled, const char* tooltip);

// Same, but with the foreground colour forced — for a cell whose job is to
// report a state (ahead/behind, conflicts) rather than to arm a tool.
bool cellTinted(const Metrics& m, float x, float w, const char* id, IconFn icon,
                const char* label, ImU32 fg, bool enabled, const char* tooltip);

// The rounded background a group of cells sits on.
void well(const Metrics& m, float x, float w, ImU32 col = kWellBg);

// The strip itself: darker band plus a hairline along its bottom edge. Draw
// before any well.
void bar(const ImVec2& origin, float width, const Metrics& m,
         ImU32 tint = 0, ImU32 lineCol = kBarLine, float lineThickness = 1.0f);

// ── Icons shared by more than one bar ────────────────────────────────────────
void iconGear(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconRefresh(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconArrowUp(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconArrowDown(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconBranch(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconCloud(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconCheck(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconWarning(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconList(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconTree(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconHistory(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconPlus(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);

} // namespace EditorToolbar

#endif // HE_IMGUI_ENABLED
