#pragma once

struct AppContext;

// ── Audio Mixer (View ▸ Audio Mixer) ─────────────────────────────────────────
// One strip per bus, plus the master: a fader, mute, solo, and how many voices
// are on it right now. The bus LIST and every fader position belong to the
// project (ProjectData::audioBuses, saved into the .heproj and carried into
// the packaged build) — mute and solo do not. They are what you do while
// listening, and a game shipped with "SFX" muted because somebody was checking
// the music yesterday is a bug nobody would find.
//
// Edits reach the editor's own AudioEngine at once, so a fader moved during
// play is heard in the same frame; the file is written when the drag ends,
// not per pixel (the same split the Collision Layers page makes).
namespace AudioMixerPanel
{
	// `open` is the View-menu toggle; the window clears it when closed. Drawn
	// from the editor's overlay pass like the other tool windows.
	void DrawAudioMixerWindow(AppContext& ctx, bool& open);
}
