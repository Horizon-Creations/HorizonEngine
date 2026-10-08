#pragma once
#include <Audio/AudioBusConfig.h>
#include <Audio/AudioEdit.h>
#include <string>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// ── The Audio Editor's mix controls: the clip's bus and its EQ ──────────────
// Two things the Audio Editor sets on an asset that are about WHERE and HOW it
// sits in the mix rather than about its samples: the mixer bus it plays through
// (AudioEdit::bus) and its EQ (AudioEdit::eq). Both are edits like the trim and
// the curve — undoable, saved in the asset's edit chunk, honoured by every voice
// the engine starts from it (AudioEngine::play routes and filters by them).
//
// Kept free of AppContext and the AudioEngine like AudioWaveformView: the tab
// hands in the project's bus list and the edit, and gets back what changed. The
// bus list handed in is THE list the Audio Mixer edits
// (ProjectData::audioBuses), never a copy — a bus added in the mixer is in the
// dropdown on the next frame, one removed there shows up here as missing.
namespace HE::Ed::AudioMix
{
	// ── Bus ──────────────────────────────────────────────────────────────────
	// What the dropdown shows for an asset naming `assetBus` in a project with
	// `buses`. A name the project does not have (the bus was removed, or renamed
	// — the mixer cannot tell the two apart, and neither can the asset) is shown
	// as missing, and the clip plays on master until it is pointed at a bus that
	// exists or the bus comes back. The asset keeps the name: the edit is
	// non-destructive, and a bus re-added under the same name picks it up again.
	struct BusChoice
	{
		std::string preview;   // the combo's closed text: "Master", "Music", "Ambience (missing)"
		bool        missing = false;
		std::string hint;      // the sentence under it; empty when nothing needs saying
	};
	BusChoice busChoice(const HE::AudioBusConfig* buses, const std::string& assetBus);

	// The dropdown: Master first, then the project's buses in mixer order, then
	// — only while it is missing — the asset's own name, so it can be seen and
	// kept. Returns true when a different bus was picked; `assetBus` is then the
	// new name ("" = Master). `buses` null = no project open (Master only).
	bool drawBusCombo(const HE::AudioBusConfig* buses, std::string& assetBus, bool enabled);

	// ── EQ graph ─────────────────────────────────────────────────────────────
	// Frequency on a log axis from kMinHz to kMaxHz, gain linear in dB over
	// ±kRangeDb. The mapping is public so the tests can find a band on screen.
	constexpr double kMinHz   = 20.0;
	constexpr double kMaxHz   = 20000.0;
	constexpr double kRangeDb = 18.0;

	float  xOfFreq(double hz, float width);     // 0 … width
	double freqOfX(float x, float width);
	float  yOfDb(double db, float height);      // 0 (top, +kRangeDb) … height (bottom, −kRangeDb)
	double dbOfY(float y, float height);

	// A new band the "Add Band" button puts in: a bell at a frequency the EQ
	// does not use yet (spread over the decades), 0 dB, so adding one changes
	// nothing until it is dragged.
	HE::AudioEqBand newBand(const HE::AudioEq& eq);

	// Per-tab view state: which band is selected (its handle drawn bright, its
	// row highlighted) and which one is being dragged.
	struct EqView
	{
		int  selBand   = -1;
		int  dragBand  = -1;
		bool dragMoved = false;   // the drag in hand changed something: its release is an edit
	};

	struct EqResult
	{
		bool edited    = false;   // the EQ changed this frame (a drag in progress too)
		bool committed = false;   // …and that change is finished: an undo point
		// Where the graph landed on screen, for the tests: a band at frequency f
		// and gain g sits at graphMin + (xOfFreq(f, w), yOfDb(g, h)).
		ImVec2 graphMin  = ImVec2(0.0f, 0.0f);
		ImVec2 graphSize = ImVec2(0.0f, 0.0f);
	};

	// The whole EQ block: bypass and Add Band on top, the response graph
	// (summed curve, one handle per band — drag to move frequency and gain,
	// wheel over it for Q, double-click the empty graph to add a bell there),
	// and one row per band with its type, frequency, gain, Q, on/off and
	// remove. `sampleRate` is the rate the filter runs at (the clip's — see
	// AudioEdit.h): the curve is computed for it, and the part of the axis
	// above its Nyquist is shaded, because nothing can be shaped there.
	// `enabled` false draws it read-only.
	EqResult drawEq(HE::AudioEq& eq, EqView& view, double sampleRate, const ImVec2& size, bool enabled);
}
