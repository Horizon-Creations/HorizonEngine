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

#include <algorithm>
#include <cmath>

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
	// Secondary lines: hints, versions, "1 to install", every TextDisabled. It
	// used to be 0.46, which is 3.3:1 on a popup and 2.9:1 on an input field,
	// below the 4.5:1 body text needs, so the second line of every row was the
	// one people had to lean in for. 0.60 clears 4.5 on every surface up to
	// FrameBg and is still ~2.5:1 darker than Text, so it reads as secondary.
	inline constexpr ImVec4 TextDim     { 0.600f, 0.574f, 0.535f, 1.0f };
	// Section headings and "this is the thing you are looking at" labels — the
	// places that used to be light blue. Pale gold rather than the accent
	// itself, and measured against EditorToolbar's kWarn (230,176,86) rather
	// than picked by eye: a heading that is the same colour as a warning is a
	// heading that teaches people to ignore warnings.
	inline constexpr ImVec4 TextHeading { 0.950f, 0.880f, 0.680f, 1.0f };

	// ── Status ───────────────────────────────────────────────────────────────
	// What a line of feedback means, as a colour. Panels used to carry their own
	// greens and ambers (a dozen slightly different triples across the Collab
	// window and Preferences), so the same verdict looked different from page to
	// page. Warn is EditorToolbar's kWarn (230,176,86) for the reason given above
	// TextHeading; Ok and Bad are kept at a similar lightness so none of the
	// three shouts louder than the others on the dark surface.
	inline constexpr ImVec4 Ok   { 0.450f, 0.800f, 0.500f, 1.0f };
	inline constexpr ImVec4 Warn { 0.902f, 0.690f, 0.337f, 1.0f };
	inline constexpr ImVec4 Bad  { 0.950f, 0.460f, 0.400f, 1.0f };

	// ── ImU32 mirrors, for the hand-drawn chrome (toolbars, list markers) ─────
	inline ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

	// ── Text on a coloured ground ────────────────────────────────────────────
	// A status line is coloured so it is noticed; the ground under it is often
	// coloured for the same reason (a flashed row, a tinted error line, a lit
	// button). Picked one pair at a time, the two drifted onto the same hue: an
	// amber Warn line on an amber row, a red error on a red tint, white on an
	// amber button. Each looked fine to whoever picked it, on its own.
	//
	// So the rule is a number, WCAG's contrast ratio, and these helpers enforce
	// it where text meets a ground that is not the plain surface. 4.5:1 is the
	// floor for body text. It measures lightness only, so it cannot see that
	// orange on orange-brown is hard to read even at 4.7:1: same-hue pairs are
	// avoided by not drawing them (the Tool Setup flash moved its accent out
	// from under the text), not by a ratio. tests/test_editor_theme.cpp holds
	// the palette's own pairs to this floor.
	inline constexpr float kMinTextContrast = 4.5f;

	// Dark text for light, saturated grounds: the amber of a primary button,
	// where neither Text nor white reaches 4.5:1.
	inline constexpr ImVec4 Ink = warm(0.08f);

	// `top` composited over an opaque `under`, the way ImGui blends a fill.
	constexpr ImVec4 over(const ImVec4& top, const ImVec4& under)
	{
		const float a = top.w;
		return ImVec4(top.x * a + under.x * (1.0f - a),
		              top.y * a + under.y * (1.0f - a),
		              top.z * a + under.z * (1.0f - a), 1.0f);
	}

	// WCAG relative luminance, of the colour as it lands in an 8-bit target,
	// so the ratio is the one measured on the pixels.
	inline float luminance(const ImVec4& c)
	{
		const auto lin = [](float v) {
			v = std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f;
			return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
		};
		return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z);
	}

	// 1 (same) to 21 (black on white). Both colours opaque: composite a
	// translucent ground with over() first.
	inline float contrastRatio(const ImVec4& a, const ImVec4& b)
	{
		const float la = luminance(a), lb = luminance(b);
		return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
	}

	// The text colour for a filled ground: `light` (Text, or the white a
	// filled button uses) while it clears the floor, otherwise whichever of
	// `light` and Ink stands out more. A lit amber button gets Ink.
	inline ImVec4 textOn(const ImVec4& fill, const ImVec4& light = Text,
	                     float minRatio = kMinTextContrast)
	{
		const float l = contrastRatio(light, fill);
		if (l >= minRatio) return light;
		return contrastRatio(Ink, fill) > l ? Ink : light;
	}

	// `fg` as close to itself as it can stay while clearing `minRatio` on `bg`:
	// moved toward Text on a dark ground (toward Ink on a light one) only as
	// far as needed, so a Warn line stays recognisably amber and a dim line
	// recognisably dim. Already readable comes back unchanged.
	inline ImVec4 readableOn(const ImVec4& fg, const ImVec4& bg,
	                         float minRatio = kMinTextContrast)
	{
		if (contrastRatio(fg, bg) >= minRatio) return fg;
		const ImVec4 to = contrastRatio(Text, bg) >= contrastRatio(Ink, bg) ? Text : Ink;
		float lo = 0.0f, hi = 1.0f;
		for (int i = 0; i < 12; ++i)
		{
			const float t = 0.5f * (lo + hi);
			(contrastRatio(mix(fg, to, t), bg) >= minRatio ? hi : lo) = t;
		}
		return mix(fg, to, hi, fg.w);
	}
}

namespace HE::Ed
{
	// Applies the whole ImGui style — shape, spacing and all 67 colour slots.
	// Called once, before the first frame. Lives in EditorTheme.cpp so it can be
	// built and asserted on without the editor around it.
	void applyHorizonDarkTheme();
}

#endif // __has_include(<imgui.h>)
