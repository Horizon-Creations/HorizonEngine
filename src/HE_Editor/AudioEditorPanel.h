#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

// Audio clip editor — a top-level tab opened by double-clicking an Audio .hasset
// (or a raw .wav) in the Content Browser. Before it existed both fell through to
// the script editor, which rendered megabytes of PCM as text.
//
// Two things a sound library needs and nothing else in the editor gives: you can
// HEAR the clip without entering play mode (the editor's AudioEngine is alive the
// whole session), and you can SEE it — a zoomable min/max waveform with a time
// ruler and a scrubable playhead. The analysis pane answers the questions that
// decide whether a clip is usable: how loud it peaks, whether it clips, how much
// silence pads the ends, and — for the ambience loops this was built for —
// whether the loop seam will click.
//
// Raw .wav is deliberately openable. Engine-content .wav files cannot be imported
// at all without HE_ENGINE_CONTENT_EDITABLE, so requiring an import first would
// leave exactly those files unauditionable; this way the tab is also the natural
// place to preview a clip before deciding to import it.
namespace AudioEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Whether the file at `path` is something this panel can open: an Audio
	// .hasset (HAsset header sniff, cached per path — same convention as the other
	// asset panels) or a raw .wav the importer can decode. Only .wav: dr_wav is
	// the only decoder linked in, so mp3/ogg/flac are NOT claimed here even though
	// the Content Browser draws them with a speaker icon.
	bool isAudioAsset(const std::string& path);

	// Drop cached editor state for `path` (tab close, content-browser
	// rename/delete). Stops whatever this tab was auditioning — a closed tab has
	// no UI left to silence a looping clip with.
	void forget(const std::string& assetPath);
}
