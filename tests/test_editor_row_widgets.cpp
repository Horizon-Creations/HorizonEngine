#include "doctest.h"
#include <imgui.h>
#include <string>

#include "EditorWidgets.h"

// ── Panel-width layout, actually measured ────────────────────────────────────
// The bug these widgets fix is a layout one: with ImGui's default label
// placement the text sits to the RIGHT of the control, so in a narrow docked
// panel (Quick Settings, Details) it ran past the edge and was clipped away.
// "Looks right on my screen" is exactly the check that missed it, because the
// failure only shows below a certain panel width.
//
// ImGui needs no backend — a context, a display size and a window are enough —
// so the invariant can be asserted directly: whatever the panel width, no item
// may extend past its content edge, and a long hint must grow DOWNWARD (wrap)
// rather than sideways. The widgets live in their own ImGui-only translation
// unit precisely so this test can reach them.

namespace {

struct ImGuiCtx
{
	ImGuiCtx()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		// No renderer: claim texture support so ImGui never waits on a backend to
		// upload the font atlas (1.92's dynamic-font path).
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~ImGuiCtx() { ImGui::DestroyContext(); }
};

// What one frame of a `width`-wide panel produced.
struct Measured
{
	float maxItemRight = 0.0f;   // furthest right edge any item reached
	float contentRight = 0.0f;   // the panel's own content edge
	float height       = 0.0f;   // total vertical extent used
};

// Draws `body` in a panel of the given width and measures it. ImGui reports the
// last item's rect, so the body records after each widget via `note`.
template <typename F>
Measured measure(float width, F&& body)
{
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(width, 600.0f));
	ImGui::Begin("panel", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

	Measured m;
	m.contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
	const float top = ImGui::GetCursorScreenPos().y;

	auto note = [&m] { m.maxItemRight = ImGui::GetItemRectMax().x > m.maxItemRight
		                                 ? ImGui::GetItemRectMax().x : m.maxItemRight; };
	body(note);

	m.height = ImGui::GetCursorScreenPos().y - top;
	ImGui::End();
	ImGui::EndFrame();
	return m;
}

} // namespace

TEST_CASE("Row widgets stay inside the panel at any width")
{
	ImGuiCtx ctx;

	// 180 px is narrower than the Quick Settings dock's default; 900 is wider
	// than any of it. Both must hold — the point is that the layout does not
	// depend on the width at all.
	for (const float width : { 180.0f, 260.0f, 420.0f, 900.0f })
	{
		CAPTURE(width);
		float f = 0.5f;
		int   i = 3;
		float rgb[3] = { 1.0f, 0.5f, 0.25f };
		const char* items[] = { "Low", "Medium", "High" };

		// Two frames: ImGui settles a window's content region on the second.
		Measured m{};
		for (int frame = 0; frame < 2; ++frame)
		{
			m = measure(width, [&](auto note)
			{
				EditorWidgets::Row::sliderFloat("A Fairly Long Setting Label", &f, 0.0f, 1.0f); note();
				EditorWidgets::Row::sliderInt("Another Long Label Here", &i, 0, 10);           note();
				EditorWidgets::Row::combo("Quality Preset Selection", &i, items, 3);           note();
				EditorWidgets::Row::colorEdit3("Some Colour", rgb);                            note();
				EditorWidgets::Row::dragFloat("Dragged Value", &f, 0.01f);                     note();
			});
		}

		// The actual regression: nothing may reach past the panel's content edge.
		CHECK(m.maxItemRight <= m.contentRight + 0.5f);
	}
}

TEST_CASE("A long hint wraps downward instead of running off the side")
{
	ImGuiCtx ctx;

	const char* kLong =
		"Raymarch clouds at quarter resolution and upsample. A big win in open-sky "
		"views, and the kind of explanation that is worthless if the reader can only "
		"see the first half of it.";

	// Same text, two panel widths. Wrapping means: still inside the panel, and
	// visibly taller in the narrow one.
	auto run = [&](float width)
	{
		Measured m{};
		for (int frame = 0; frame < 2; ++frame)
			m = measure(width, [&](auto note) { EditorWidgets::hint("%s", kLong); note(); });
		return m;
	};

	const Measured narrow = run(200.0f);
	const Measured wide   = run(700.0f);

	CHECK(narrow.maxItemRight <= narrow.contentRight + 0.5f);
	CHECK(wide.maxItemRight   <= wide.contentRight   + 0.5f);
	// The same sentence needs more lines in a narrower panel — which is the
	// difference between wrapping and clipping.
	CHECK(narrow.height > wide.height);
}

TEST_CASE("Row labels drop the ## id suffix but keep the ids apart")
{
	ImGuiCtx ctx;

	// CollapsingHeader does not open an id scope, so the Details panel relies on
	// "Speed##an" / "Speed##ab" to keep four different Speed rows distinct. The
	// user must not see the suffix, and ImGui must still see two separate items.
	float a = 1.0f, b = 2.0f;
	ImGuiID idA = 0, idB = 0;

	for (int frame = 0; frame < 2; ++frame)
	{
		measure(400.0f, [&](auto note)
		{
			EditorWidgets::Row::dragFloat("Speed##an", &a, 0.01f); note();
			idA = ImGui::GetItemID();
			EditorWidgets::Row::dragFloat("Speed##ab", &b, 0.01f); note();
			idB = ImGui::GetItemID();
		});
	}

	CHECK(idA != 0);
	CHECK(idA != idB);   // same visible label, different items
}
