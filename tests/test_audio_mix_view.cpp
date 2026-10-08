#include "doctest.h"

#include "AudioMixView.h"

#include <imgui.h>

#include <cmath>
#include <string>

// ── The Audio Editor's bus dropdown and EQ, without a window or a project ────
// The arithmetic (frequency and dB axes, which bus the dropdown shows and
// whether it is missing) is checked directly; the graph is then driven through
// real ImGui mouse events, like the waveform's canvas: a drag on a band's
// handle must land the band at the frequency and gain under the pointer, and
// only the release is an undo point.

using namespace HE::Ed::AudioMix;

TEST_CASE("audio mix view: the EQ graph's axes map both ways")
{
	const float w = 800.0f, h = 200.0f;
	CHECK(xOfFreq(kMinHz, w) == doctest::Approx(0.0f));
	CHECK(xOfFreq(kMaxHz, w) == doctest::Approx(w));
	CHECK(xOfFreq(632.455532, w) == doctest::Approx(w * 0.5f).epsilon(1e-4));   // geometric middle
	CHECK(xOfFreq(5.0, w) == doctest::Approx(0.0f));                            // clamped
	for (const double f : { 31.5, 100.0, 1000.0, 4000.0, 16000.0 })
		CHECK(freqOfX(xOfFreq(f, w), w) == doctest::Approx(f).epsilon(1e-4));
	CHECK(yOfDb(kRangeDb, h)  == doctest::Approx(0.0f));
	CHECK(yOfDb(0.0, h)       == doctest::Approx(h * 0.5f));
	CHECK(yOfDb(-kRangeDb, h) == doctest::Approx(h));
	CHECK(yOfDb(40.0, h)      == doctest::Approx(0.0f));                        // clamped
	for (const double db : { -12.0, -3.5, 0.0, 6.0, 17.0 })
		CHECK(dbOfY(yOfDb(db, h), h) == doctest::Approx(db).epsilon(1e-4));
}

TEST_CASE("audio mix view: the bus dropdown names master, a project bus, or a missing one")
{
	HE::AudioBusConfig cfg;
	cfg.add("Music");
	cfg.add("SFX");

	BusChoice c = busChoice(&cfg, "");
	CHECK(c.preview == "Master");
	CHECK_FALSE(c.missing);
	CHECK(c.hint.empty());

	c = busChoice(&cfg, "SFX");
	CHECK(c.preview == "SFX");
	CHECK_FALSE(c.missing);

	// Removed in the mixer (or renamed — the asset cannot tell): shown as
	// missing, with the sentence that it plays on master.
	c = busChoice(&cfg, "Ambience");
	CHECK(c.missing);
	CHECK(c.preview == "Ambience (missing)");
	CHECK(c.hint.find("Master") != std::string::npos);

	// The same list object the mixer edits: a bus removed there is missing
	// here on the next call, one re-added is found again.
	cfg.remove("SFX");
	CHECK(busChoice(&cfg, "SFX").missing);
	cfg.add("SFX");
	CHECK_FALSE(busChoice(&cfg, "SFX").missing);

	// No project open: any named bus is missing, master is master.
	CHECK(busChoice(nullptr, "Music").missing);
	CHECK_FALSE(busChoice(nullptr, "").missing);
}

TEST_CASE("audio mix view: a new band is a flat bell where no band is yet")
{
	HE::AudioEq eq;
	HE::AudioEqBand b = newBand(eq);
	CHECK(b.type == HE::AudioEqBandType::Peak);
	CHECK(b.gainDb == 0.0f);
	CHECK(b.freqHz == 1000.0f);
	eq.bands.push_back(b);
	CHECK(eq.isNeutral());   // adding a band changes nothing yet
	b = newBand(eq);
	CHECK(b.freqHz == 120.0f);
	// A band near 120 Hz (within a third of an octave) takes that spot too.
	eq.bands.push_back(b);
	eq.bands[1].freqHz = 130.0f;
	CHECK(newBand(eq).freqHz == 4000.0f);
}

namespace
{
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
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f);
		}
		~ImGuiCtx() { ImGui::DestroyContext(); }
	};

	EqResult frame(HE::AudioEq& eq, EqView& v)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(1200.0f, 300.0f));
		ImGui::Begin("##eqtest", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
		const EqResult r = drawEq(eq, v, 48000.0, ImVec2(1200.0f, 300.0f), true);
		ImGui::End();
		ImGui::Render();
		return r;
	}
	void mouseAt(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
	void mouseButton(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); }
}

TEST_CASE("audio mix view: dragging a band's handle moves its frequency and gain, the release commits")
{
	ImGuiCtx ctx;
	HE::AudioEq eq;
	HE::AudioEqBand bell;
	bell.type = HE::AudioEqBandType::Peak; bell.freqHz = 1000.0f; bell.gainDb = 6.0f; bell.q = 1.0f;
	eq.bands = { bell };
	EqView v;

	mouseAt(5.0f, 5.0f);
	EqResult r = frame(eq, v);
	r = frame(eq, v);
	REQUIRE(r.graphSize.x > 300.0f);
	const ImVec2 o = r.graphMin, s = r.graphSize;
	auto at = [&](double f, double db) { return ImVec2(o.x + xOfFreq(f, s.x), o.y + yOfDb(db, s.y)); };

	// Press on the handle: it is selected, nothing changed yet.
	ImVec2 p = at(1000.0, 6.0);
	mouseAt(p.x, p.y); frame(eq, v);
	mouseButton(0, true); r = frame(eq, v);
	CHECK(v.selBand == 0);
	CHECK_FALSE(r.committed);

	// Drag to 4 kHz, -9 dB: live edits, no commit until the release.
	p = at(4000.0, -9.0);
	mouseAt(p.x, p.y); r = frame(eq, v);
	CHECK(r.edited);
	CHECK_FALSE(r.committed);
	CHECK(eq.bands[0].freqHz == doctest::Approx(4000.0f).epsilon(0.01));
	CHECK(eq.bands[0].gainDb == doctest::Approx(-9.0f).epsilon(0.02));
	mouseButton(0, false); r = frame(eq, v);
	CHECK(r.committed);

	// A click on the handle that moves nothing is no edit.
	mouseAt(p.x, p.y); frame(eq, v);
	mouseButton(0, true);  r = frame(eq, v);
	mouseButton(0, false); r = frame(eq, v);
	CHECK_FALSE(r.edited);
	CHECK_FALSE(r.committed);

	// Right-click on the handle removes the band.
	mouseButton(1, true);  r = frame(eq, v);
	mouseButton(1, false); frame(eq, v);
	CHECK(eq.bands.empty());
	CHECK(r.committed);

	// Double-click on the empty graph adds a bell there.
	p = at(250.0, 4.0);
	mouseAt(p.x, p.y); frame(eq, v);
	mouseButton(0, true);  frame(eq, v);
	mouseButton(0, false); frame(eq, v);
	mouseButton(0, true);  r = frame(eq, v);
	mouseButton(0, false); frame(eq, v);
	REQUIRE(eq.bands.size() == 1);
	CHECK(r.committed);
	CHECK(eq.bands[0].type == HE::AudioEqBandType::Peak);
	CHECK(eq.bands[0].freqHz == doctest::Approx(250.0f).epsilon(0.01));
	CHECK(eq.bands[0].gainDb == doctest::Approx(4.0f).epsilon(0.03));
}
