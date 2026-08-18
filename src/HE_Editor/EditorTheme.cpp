#include "EditorTheme.h"

// ── The editor's ImGui style, applied once at startup ────────────────────────
// Split out of EditorApplication.cpp so it can be BUILT AND TESTED without the
// world, the renderer and the collaboration session behind it. That is not
// tidiness: this file has a failure mode that only a test catches — see the
// note on StyleColorsDark below, and tests/test_editor_theme.cpp.

#if __has_include(<imgui.h>)

namespace HE::Ed
{

void applyHorizonDarkTheme()
{
	// The starting point, and the trap. StyleColorsDark does not just fill in
	// defaults — it DERIVES several colours from its own palette, e.g.
	//     CheckboxSelectedBg  = lerp(FrameBg, FrameBgActive, 0.65)
	//     TabSelectedOverline = HeaderActive
	//     TextLink            = HeaderActive
	// and ImGui's HeaderActive/FrameBgActive are its signature blue. So every
	// slot this function does not name explicitly keeps a blue derived from a
	// palette we have already replaced — which is exactly how a themed editor
	// ended up with a blue tick behind its amber checkmark and a blue overline
	// above every docked tab, months after "the theme" was considered done.
	//
	// Hence: every one of the 67 slots is assigned below, and
	// tests/test_editor_theme.cpp fails the build if a future ImGui adds one
	// that nobody assigned.
	ImGui::StyleColorsDark();

	ImGuiStyle& s = ImGui::GetStyle();

	// ── Shape ────────────────────────────────────────────────────────────────
	// The old theme zeroed every rounding, which is what made each context menu
	// and combo read as a decade older than the toolbars next to it. The radii
	// follow EditorToolbar's (wells 7, cells 5) so chrome and widgets agree.
	// WindowRounding only shows on floating windows — ImGui flattens docked ones
	// and platform-owned viewports on its own.
	s.WindowRounding    = 6.0f;
	s.ChildRounding     = 6.0f;
	s.FrameRounding     = 5.0f;
	s.PopupRounding     = 7.0f;
	s.ScrollbarRounding = 8.0f;
	s.GrabRounding      = 5.0f;
	s.TabRounding       = 5.0f;

	s.WindowBorderSize = 1.0f;
	s.ChildBorderSize  = 1.0f;
	s.PopupBorderSize  = 1.0f;
	s.FrameBorderSize  = 0.0f;
	s.TabBarBorderSize = 1.0f;

	// Roomier than ImGui's defaults: cramped padding is most of what "looks old"
	// actually is. FramePadding.y also drives every row height, so this is the
	// single biggest lever the theme has.
	s.WindowPadding    = ImVec2(10.0f, 8.0f);
	s.FramePadding     = ImVec2(8.0f, 4.0f);
	s.CellPadding      = ImVec2(6.0f, 4.0f);
	s.ItemSpacing      = ImVec2(8.0f, 6.0f);
	s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
	s.IndentSpacing    = 16.0f;
	s.ScrollbarSize    = 11.0f;
	s.GrabMinSize      = 10.0f;

	// SeparatorText is the section header of every properties panel (Details,
	// material properties, the input-asset pages, Preferences). Left-aligned
	// with a heavier rule reads as a heading; centred with hairlines reads as
	// 1990s group boxes — which is exactly the complaint.
	s.SeparatorTextBorderSize = 2.0f;
	s.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);
	s.SeparatorTextPadding    = ImVec2(16.0f, 8.0f);

	ImVec4* c = s.Colors;

	// One accent, the logo's amber, spent only on state: selection, checks,
	// grabs, the focused tab. Everything decorative stays neutral.
	//
	// Amber and not gold, deliberately: gold sits 0.08 away from
	// EditorToolbar::kWarn in RGB and amber sits 0.30 away, so this is the
	// choice that leaves "needs attention" a colour of its own. The gold is
	// still here — as the BRIGHT tier, for the marks that answer a question
	// (a checkmark, a drop target) rather than tint a surface.
	using namespace HE::Ed::Theme;
	const ImVec4 accent       = Accent;
	const ImVec4 accentHi     = AccentHi;
	const ImVec4 accentBright = AccentBright;

	// Text. Warm off-white rather than the ivory of the logo: ivory is right
	// for a wordmark and reads yellowed as 13 px body copy across a panel.
	c[ImGuiCol_Text]         = Text;
	c[ImGuiCol_TextDisabled] = TextDim;

	// Backgrounds. Popups sit slightly ABOVE the window shade — elevation is
	// what makes a context menu look like it belongs to this decade.
	//
	// Every neutral here is warm(v) of the grey it used to be: same lightness,
	// blue pulled out, a little red in. Going all the way to the splash's
	// panel colour would read brown across a full-screen editor; the identity
	// is carried by the warm off-white text and the amber, not by brown walls.
	c[ImGuiCol_WindowBg] = warm(0.105f);
	c[ImGuiCol_ChildBg]  = warm(0.085f);
	c[ImGuiCol_PopupBg]  = warm(0.13f);

	c[ImGuiCol_Border]       = warm(0.25f);
	c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	// Frames (inputs, drags, combos, checkboxes). The ACTIVE one used to be a
	// blue-tinted grey picked by hand; it is now the same base blended toward
	// the accent, so it cannot drift away from the rest.
	c[ImGuiCol_FrameBg]        = warm(0.16f);
	c[ImGuiCol_FrameBgHovered] = warm(0.21f);
	c[ImGuiCol_FrameBgActive]  = mix(warm(0.16f), accentHi, 0.18f);

	// Title / menu
	c[ImGuiCol_TitleBg]          = warm(0.08f);
	c[ImGuiCol_TitleBgActive]    = warm(0.12f);
	c[ImGuiCol_TitleBgCollapsed] = warm(0.06f);
	c[ImGuiCol_MenuBarBg]        = warm(0.10f);

	// Scrollbars: no boxed track, just a grab that firms up under the mouse.
	c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.12f);
	c[ImGuiCol_ScrollbarGrab]        = warm(0.27f, 0.85f);
	c[ImGuiCol_ScrollbarGrabHovered] = warm(0.34f);
	c[ImGuiCol_ScrollbarGrabActive]  = warm(0.42f);

	// The one place the accent is loud on purpose: the mark that answers
	// "is this on". CheckboxSelectedBg is the box BEHIND that mark and has to
	// be named too — left to StyleColorsDark it is a lerp of ImGui's own blue
	// frame colours, which is what put a blue tile behind the gold tick.
	c[ImGuiCol_CheckMark]          = accentBright;
	c[ImGuiCol_CheckboxSelectedBg] = mix(warm(0.16f), accentHi, 0.22f);
	c[ImGuiCol_SliderGrab]       = ImVec4(accent.x, accent.y, accent.z, 0.90f);
	c[ImGuiCol_SliderGrabActive] = accentHi;

	c[ImGuiCol_Button]        = warm(0.19f);
	c[ImGuiCol_ButtonHovered] = warm(0.25f);
	c[ImGuiCol_ButtonActive]  = alpha(accent, 0.80f);

	// Header drives Selectable rows (outliner, lists, menus) AND CollapsingHeader
	// (every Details section). A tint, not the full accent: a selected row should
	// read selected, a section header should not shout on every component. Kept
	// deliberately weak — a warm tint turns muddy far sooner than the blue one
	// did, and a brown outliner is not what anyone asked for.
	c[ImGuiCol_Header]        = mix(warm(0.16f), accentHi, 0.22f, 0.65f);
	c[ImGuiCol_HeaderHovered] = mix(warm(0.16f), accentHi, 0.32f, 0.75f);
	c[ImGuiCol_HeaderActive]  = alpha(accent, 0.85f);

	c[ImGuiCol_Separator]        = warm(0.24f);
	c[ImGuiCol_SeparatorHovered] = alpha(accent, 0.70f);
	c[ImGuiCol_SeparatorActive]  = accentHi;

	c[ImGuiCol_ResizeGrip]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	c[ImGuiCol_ResizeGripHovered] = alpha(accent, 0.60f);
	c[ImGuiCol_ResizeGripActive]  = accentHi;

	// Tabs. Written with the CURRENT enum names: TabActive/TabUnfocused/
	// TabUnfocusedActive are 1.90.9 aliases of TabSelected/TabDimmed/
	// TabDimmedSelected, and using the old spellings hid the fact that the two
	// *Overline slots next to them were never assigned at all.
	c[ImGuiCol_Tab]                          = warm(0.11f);
	c[ImGuiCol_TabHovered]                   = mix(warm(0.11f), accentHi, 0.26f);
	c[ImGuiCol_TabSelected]                  = mix(warm(0.21f), accentHi, 0.14f);
	// The rule over the focused tab — the blue bar that survived the first pass.
	c[ImGuiCol_TabSelectedOverline]          = accent;
	c[ImGuiCol_TabDimmed]                    = warm(0.09f);
	c[ImGuiCol_TabDimmedSelected]            = warm(0.14f);
	// Same rule on an UNFOCUSED tab bar: present, so the selected tab is still
	// identifiable, but muted enough to say the bar does not have focus.
	c[ImGuiCol_TabDimmedSelectedOverline]    = alpha(accent, 0.40f);

	c[ImGuiCol_DockingPreview] = alpha(accent, 0.45f);
	c[ImGuiCol_DockingEmptyBg] = warm(0.08f);

	c[ImGuiCol_PlotLines]            = warm(0.56f);
	c[ImGuiCol_PlotLinesHovered]     = accentBright;
	c[ImGuiCol_PlotHistogram]        = alpha(accent, 0.85f);
	c[ImGuiCol_PlotHistogramHovered] = accentHi;
	c[ImGuiCol_TableHeaderBg]        = warm(0.14f);
	c[ImGuiCol_TableBorderStrong]    = warm(0.24f);
	c[ImGuiCol_TableBorderLight]     = warm(0.17f);
	c[ImGuiCol_TextSelectedBg]       = alpha(accent, 0.35f);
	c[ImGuiCol_DragDropTarget]       = alpha(accentBright, 0.90f);
	c[ImGuiCol_DragDropTargetBg]     = alpha(accent, 0.12f);
	c[ImGuiCol_NavCursor]            = accentHi;
	c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

	// ── The rest of the slots ────────────────────────────────────────────────
	// Not an afterthought: these are the ones StyleColorsDark derives from its
	// own blue, so leaving any of them out puts that blue back on screen.
	c[ImGuiCol_InputTextCursor]      = Text;
	c[ImGuiCol_TextLink]             = accentHi;
	c[ImGuiCol_TreeLines]            = warm(0.25f);
	c[ImGuiCol_UnsavedMarker]        = alpha(Text, 0.90f);
	// Table striping: a warm white at very low alpha, so it reads as a lift of
	// whatever is underneath rather than as a colour of its own.
	c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 0.96f, 0.90f, 0.045f);
	c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 0.97f, 0.92f, 0.70f);
	c[ImGuiCol_NavWindowingDimBg]    = warm(0.20f, 0.20f);
}

} // namespace HE::Ed

#endif // __has_include(<imgui.h>)
