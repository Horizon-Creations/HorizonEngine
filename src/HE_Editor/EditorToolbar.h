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

#include <initializer_list>

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

// ── Bar builder ──────────────────────────────────────────────────────────────
// The primitives above are enough to build a bar by hand, and the Scene and
// Source Control bars do exactly that because both need custom fitting rules.
// Every other panel wants the same three things — a band, a couple of wells, a
// Save on the right — and hand-rolling the width arithmetic for that eleven more
// times is eleven more chances to be off by a pixel in a way nobody notices
// until the font scale changes.
//
// So: a single-pass builder. A well has to be painted BEHIND its cells but its
// width is only known once they are all placed, which normally forces a measure
// pass; this splits the draw list into two channels instead and back-fills the
// well when the group closes. The caller writes what the bar contains, in order.
//
//   {
//       EditorToolbar::Bar bar;
//       bar.group();
//       if (bar.item("##play", iconPlay, nullptr, playing, true, "Play")) …
//       bar.item("##loop", iconRefresh, "Loop", looping, true, "Loop the clip");
//       bar.endGroup();
//       bar.rightGroup(bar.iconGroupWidth(1));
//       if (bar.item("##save", iconSave, nullptr, false, dirty, "Save")) …
//       bar.endGroup();
//   }   // destructor merges the channels and puts the cursor below the strip
//
// Right-hand groups take their width from the caller because a single pass
// cannot know it in advance; iconGroupWidth()/labelGroupWidth() do the sum.
class Bar
{
public:
	Bar();
	~Bar();

	Bar(const Bar&)            = delete;
	Bar& operator=(const Bar&) = delete;

	// Wash the band and recolour the hairline — for a bar that carries a state
	// worth seeing peripherally. Call before the first group.
	void tint(ImU32 wash, ImU32 line, float thickness = 2.0f);

	// Open a well at the current left-hand position, or flush against the right
	// edge at `width` pixels wide. Groups after a rightGroup keep stacking
	// leftwards from it.
	void group();
	void rightGroup(float width);
	void endGroup();
	// Extra space between two left-hand groups, for a bar that wants a wider
	// break than the usual well-to-well gap.
	void gap();

	// One cell. Same semantics as cell() above.
	bool item(const char* id, IconFn icon, const char* label, bool on, bool enabled,
	          const char* tooltip);
	// A cell whose foreground colour is forced — for a readout rather than a tool.
	bool itemTinted(const char* id, IconFn icon, const char* label, ImU32 fg,
	                bool enabled, const char* tooltip);
	// A cell that reports rather than acts: icon and label inside the current
	// well, with no hit box at all. An InvisibleButton that does nothing still
	// lights up on hover, which promises a click that never happens.
	void readout(IconFn icon, const char* label, ImU32 fg = kFg);
	// A hairline between two cells inside one well, for a group that holds two
	// unrelated things and is not worth splitting.
	void divider();
	// Plain text on the band, outside any well.
	void label(const char* text, ImU32 col = kFgDim);

	// Width of a group of `n` square icon cells, wells included.
	float iconGroupWidth(int n) const;
	// Width of a group of labelled cells.
	float labelGroupWidth(std::initializer_list<const char*> labels) const;

	// Room still free between the left cursor and the right-hand groups. Lets a
	// bar drop its labels when it runs out — the same shrink idea the Scene bar
	// applies by hand.
	float remaining() const;

	const Metrics& metrics() const { return m_m; }

private:
	void flushGroup();

	Metrics     m_m;
	ImDrawList* m_dl        = nullptr;
	ImVec2      m_origin    { 0.0f, 0.0f };
	float       m_width     = 0.0f;
	float       m_left      = 0.0f;   // next left-hand x
	float       m_right     = 0.0f;   // right-hand groups grow leftwards from here
	float       m_groupX    = 0.0f;
	// The running cell cursor and the group's width are two different numbers:
	// a right-hand group's width is fixed up front while its cells still fill in
	// from the left. Conflating them placed every right-group cell at the
	// group's END — half off the window, since the group hugs the edge.
	float       m_cursor    = 0.0f;   // next cell x, relative to m_groupX
	float       m_groupW    = 0.0f;   // fixed width of a right group; unused for left
	bool        m_inGroup   = false;
	bool        m_groupIsRight = false;
	bool        m_first     = true;   // no separating gap before the first cell
};

// ── Asset-editor header ──────────────────────────────────────────────────────
// Every asset tab — material, HorizonCode, particle, animator, UI widget, mesh,
// script — opens with the same question ("what am I looking at, and is it
// saved?") and ends with the same answer ("Save"). Written out per panel that is
// six copies of one row, each free to drift in wording and spacing.

// The left-hand group: kind icon, asset name, and an "unsaved" mark when there
// are pending edits.
void assetHeader(Bar& bar, const char* name, IconFn kindIcon, bool dirty);

// The right-hand Save. True when pressed. `enabled` is the panel's answer to
// "is there anything to write, and did the asset even load".
bool saveButton(Bar& bar, bool enabled);

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
void iconSave(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconPlay(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconPause(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconStop(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconGrid(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconCode(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconEye(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconLayers(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconHammer(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconFolder(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconSearch(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconFit(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconTrash(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconBone(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconBrush(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconFlip(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconSparkle(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconWidget(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);
void iconWave(ImDrawList* dl, const ImVec2& c, float s, ImU32 col);

} // namespace EditorToolbar

#endif // HE_IMGUI_ENABLED
