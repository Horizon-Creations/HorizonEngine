#pragma once

// ── The editor's colour identity, in one place ───────────────────────────────
// Horizon Creations' mark is a sun over a horizon in three colours: ivory, gold
// and amber, on a near-black that leans warm. The startup splash was the first
// thing in the product to actually look like that, and the editor behind it did
// not — it was neutral grey with a borrowed blue accent, which is what every
// other tool looks like.
//
// So the brand values live here, and everything that draws chrome derives from
// them: the ImGui theme, the toolbars, panel headings, primary buttons. One
// header, because the alternative is twenty hand-picked shades that disagree
// the first time anyone touches one of them.
//
// What does NOT come from here, deliberately:
//   • node and pin type colours (graph editors), file-status colours (source
//     control) and notification severities — there hue IS the data, and a
//     palette that paints them all amber destroys the encoding;
//   • viewport overlays (terrain brush, mesh edges, waveforms) — those are cyan
//     because they are drawn OVER warm, sunlit scene content and have to fight
//     it for contrast, which is the one job amber cannot do.
//
// The tiers matter. `Accent` is the logo's amber and reads distinctly ORANGE;
// `AccentBright` is the gold. Keeping the two apart is what leaves room for
// EditorToolbar's `kWarn` (230,176,86) to still mean "needs attention" instead
// of dissolving into the accent.

// Gated on the HEADER, not on HE_IMGUI_ENABLED: the widget test target compiles
// EditorRowWidgets.cpp against ImGui's core alone, without the editor's define,
// and a palette of plain ImVec4 constants has no reason to disappear there.
#if __has_include(<imgui.h>)

#include <imgui.h>

namespace HE::Ed::Theme
{
	// ── Brand ────────────────────────────────────────────────────────────────
	// Sampled from HC_Logo.png — the same three values the installer artwork
	// (scripts/dmg_assets/gen_assets.py) and the splash panel use.
	inline constexpr ImVec4 Ivory { 0.941f, 0.902f, 0.808f, 1.0f };  // 240,230,206
	inline constexpr ImVec4 Gold  { 0.918f, 0.761f, 0.306f, 1.0f };  // 234,194, 78
	inline constexpr ImVec4 Amber { 0.808f, 0.486f, 0.141f, 1.0f };  // 206,124, 36

	// ── Neutrals ─────────────────────────────────────────────────────────────
	// One formula rather than twenty picked shades: take the grey the theme used
	// to have, keep its lightness, pull the blue out and put a little red in.
	// Everything neutral in the editor is `warm(v)` of its old value, so the
	// whole surface shifts together and nothing drifts.
	constexpr ImVec4 warm(float v, float a = 1.0f)
	{
		return ImVec4(v * 1.10f, v * 1.01f, v * 0.92f, a);
	}

	// Linear blend, for the tints that used to be blue-tinted greys (selected
	// rows, active frames, hovered tabs). Derived the same way from the same
	// accent instead of eyeballed one by one.
	constexpr ImVec4 mix(const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f)
	{
		return ImVec4(a.x + (b.x - a.x) * t,
		              a.y + (b.y - a.y) * t,
		              a.z + (b.z - a.z) * t,
		              alpha);
	}

	constexpr ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

	// ── Accent tiers ─────────────────────────────────────────────────────────
	// Spent only on state: selection, checks, grabs, the focused tab, the armed
	// toolbar cell. Nothing decorative is coloured.
	inline constexpr ImVec4 Accent       = Amber;
	inline constexpr ImVec4 AccentHi     = mix(Amber, Gold, 0.45f);
	inline constexpr ImVec4 AccentBright = Gold;

	// ── Text ─────────────────────────────────────────────────────────────────
	// Body text is warm off-white, NOT the full ivory: ivory is right for a
	// 40-point wordmark and reads yellowed at 13 px across a whole panel.
	inline constexpr ImVec4 Text        { 0.925f, 0.910f, 0.875f, 1.0f };
	inline constexpr ImVec4 TextDim     { 0.460f, 0.440f, 0.410f, 1.0f };
	// Section headings and "this is the thing you are looking at" labels — the
	// places that used to be light blue. Pale gold rather than the accent
	// itself, and measured against EditorToolbar's kWarn (230,176,86) rather
	// than picked by eye: a heading that is the same colour as a warning is a
	// heading that teaches people to ignore warnings.
	inline constexpr ImVec4 TextHeading { 0.950f, 0.880f, 0.680f, 1.0f };

	// ── ImU32 mirrors, for the hand-drawn chrome (toolbars, list markers) ─────
	inline ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
}

#endif // __has_include(<imgui.h>)
