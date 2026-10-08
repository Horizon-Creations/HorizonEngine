#include "AudioEditorPanel.h"
#include "EditorToolbar.h"        // shared toolbar strip
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"     // shared per-tab state map + lazy asset open
#include "EditorInput.h"          // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "EditorHelp.h"           // "Audio Editor/<label>" scope for the tooltips
#include "EditorWidgets.h"        // WrapText
#include "AudioImporter.h"        // raw .wav/.ogg decode + the Import button
#include "AudioWaveformView.h"    // the canvas: peaks, zoom/scroll, playhead, selection
#include "AudioMixView.h"         // the clip's bus dropdown and its EQ
#include "ProjectManager.h"       // the project's bus list — the one the Audio Mixer edits
#include "ImporterCommon.h"       // readAssetChunk — the edit chunk as the file has it
#include <Audio/AudioEdit.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Diagnostics/Logger.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace AudioEditorPanel
{

namespace
{

// What the analysis pane reports. Everything is derived in one pass over the PCM
// (analyze() below) and cached for the tab's lifetime.
struct Analysis
{
	size_t frames        = 0;
	double durationSec   = 0.0;

	float  peakDb        = -144.0f;  // loudest single sample, dBFS
	float  rmsDb         = -144.0f;  // whole-clip RMS, dBFS
	size_t clipped       = 0;        // samples pinned at full scale
	float  dcOffset      = 0.0f;     // mean sample as a fraction of full scale

	double leadSilenceSec = 0.0;     // silence padding the head …
	double tailSilenceSec = 0.0;     // … and the tail (below kSilenceDb)

	// Loop-seam check: what happens at the wrap point when the clip loops.
	// `seamStepDb` is the instantaneous jump from the last frame to the first —
	// the click you hear. `seamLevelDb` is how far the first 250 ms sit from the
	// last 250 ms in RMS — a match in level, which is what stops a loop from
	// "breathing" even when there is no click.
	float  seamStepDb    = -144.0f;
	float  seamLevelDb   = 0.0f;
};

// Anything quieter than this counts as silence for the head/tail padding report.
constexpr float kSilenceDb = -60.0f;

struct State
{
	bool        loaded = false;
	std::string relPath;
	std::string name;

	// Exactly one of these two holds the clip: an imported .hasset lives in the
	// ContentManager (`clipId`), a raw .wav/.ogg is loaded into the panel (`raw`).
	HE::UUID    clipId;
	AudioAsset  raw;
	bool        isRawFile    = false;
	bool        decodeFailed = false;

	// A Vorbis clip (either source) decoded to int16 PCM, once, on first use —
	// everything below reads samples. `pcmTried` stops a clip the decoder rejects
	// from being retried every frame; `compressedBytes` is what the asset really
	// costs, for the Format readout (the PCM size would be the wrong number).
	AudioAsset  pcm;
	bool        pcmTried        = false;
	size_t      compressedBytes = 0;

	// The view's peak pyramid and the analysis: built once from the PCM above
	// and kept for the tab's lifetime, like the decoded copy they come from.
	HE::Ed::AudioWave::Peaks peaks;     bool peaksDone    = false;
	Analysis                 analysis;  bool analysisDone = false;

	// ── Transport ────────────────────────────────────────────────────────────
	// The engine outlives every tab (EditorApplication owns it), so holding the
	// pointer here is what lets forget() — which gets no AppContext — silence a
	// tab that is being closed mid-playback.
	AudioEngine* audio  = nullptr;
	uint64_t     handle = 0;
	bool         paused = false;      // handle alive but stopped; NOT finished
	bool         loop   = true;       // ambience is the reason this tab exists
	float        volume = 1.0f;
	float        pitch  = 1.0f;
	// What the running voice was handed, in frames of the clip: the whole clip,
	// or a copy of just the selection when one was marked at Play. The engine
	// counts its cursor from the start of THAT buffer, so the playhead is
	// voiceBegin + cursor, and a voice that ran off its end parks the playhead
	// at voiceEnd — the end of the selection, not of the clip.
	size_t       voiceBegin = 0;
	size_t       voiceEnd   = 0;

	// ── Waveform view ────────────────────────────────────────────────────────
	// Playhead, selection, zoom and scroll (AudioWaveformView.h). The playhead
	// lives in there because the canvas moves it; it survives Stop and decides
	// where Play starts.
	HE::Ed::AudioWave::View view;
	float  canvasW    = 0.0f;   // last laid-out canvas width, for the toolbar's zoom buttons
	double hoverFrame = -1.0;   // pointer over the canvas last frame, for the readout

	// ── Edits ────────────────────────────────────────────────────────────────
	// The asset's AudioEdit (trim, and what the later tools add to it) is edited
	// on the ContentManager's loaded copy (getAudioMutable): the loaded asset IS
	// the edit buffer, as in the Sequencer, so a voice started from it in play
	// mode plays the trim this tab shows. Only an imported .hasset has one; a raw
	// .wav/.ogg has nowhere to keep an edit and offers Extract only.
	bool dirty = false;
	// The tab's own undo over the edit, as whole snapshots in the chunk's own
	// form (AudioEdit::toChunkText): a few hundred bytes each, and every field a
	// later tool adds to the edit is covered without touching this. -1 = no
	// baseline yet.
	std::vector<std::string> undo;
	int         undoPos    = -1;
	bool        editsStale = false;   // reloadFromDisk: re-read the edit chunk next frame
	std::string lastSaveError;

	// Extract runs AFTER the tab's window has ended (render, at the bottom): the
	// button only asks for it, so nothing that writes files runs while `clip` —
	// a pointer into the ContentManager — is held.
	bool        extractRequested = false;
	std::string extractStatus;    // what was written, or why nothing was
	bool        extractFailed    = false;
	// Extract multiplies the volume curve into the new clip's samples instead of
	// handing it over as an edit (AudioImporter::extractRange, bakeCurve).
	bool        bakeCurve        = false;

	// The EQ pane under the waveform (toolbar "EQ"): open on the first frame
	// when the clip already has bands, so an EQ'd clip says so when it opens.
	bool                     showEq     = false;
	bool                     showEqInit = false;
	HE::Ed::AudioMix::EqView eqView;
};

AssetPanelState<State> s_states;

// ── Clip access ──────────────────────────────────────────────────────────────

// The clip as int16 PCM: the asset itself when it is PCM, the tab's decoded
// copy when it is Vorbis (decoded on first call, kept for the tab's lifetime —
// peaks and analysis are computed once from it and never invalidated either).
const AudioAsset* clipOf(AppContext& ctx, State& st)
{
	const AudioAsset* src = nullptr;
	if (st.isRawFile)              src = st.raw.audioData.empty() ? nullptr : &st.raw;
	else if (ctx.contentManager)   src = ctx.contentManager->getAudio(st.clipId);
	if (!src) return nullptr;
	if (src->encoding == AudioEncoding::PCM16) return src;

	if (!st.pcmTried)
	{
		st.pcmTried        = true;
		st.compressedBytes = src->audioData.size();
		st.pcm.type        = src->type;
		st.pcm.name        = src->name;
		st.pcm.sampleRate  = src->sampleRate;
		st.pcm.channels    = src->channels;
		st.pcm.encoding    = AudioEncoding::PCM16;
		if (!AudioEngine::decodeToPcm16(*src, st.pcm.audioData))
		{
			st.pcm.audioData.clear();
			st.decodeFailed = true;
		}
	}
	return st.pcm.audioData.empty() ? nullptr : &st.pcm;
}

const int16_t* samplesOf(const AudioAsset& a)
{
	return reinterpret_cast<const int16_t*>(a.audioData.data());
}

size_t frameCountOf(const AudioAsset& a)
{
	if (a.channels <= 0) return 0;
	return a.audioData.size() / (sizeof(int16_t) * static_cast<size_t>(a.channels));
}

// ── Analysis ─────────────────────────────────────────────────────────────────

float toDb(double linear)
{
	// -144 dB is the int16 noise floor; anything at or below it reads as silence
	// rather than as -inf, which formats badly and means the same thing here.
	return linear <= 1.0e-7 ? -144.0f : static_cast<float>(20.0 * std::log10(linear));
}

// RMS of a frame range, as a fraction of full scale. Used for the whole clip and
// for the two 250 ms windows the seam check compares.
double rmsOf(const AudioAsset& a, size_t f0, size_t f1)
{
	const int ch = a.channels;
	if (ch <= 0 || f1 <= f0) return 0.0;
	const int16_t* s = samplesOf(a);
	double sum = 0.0;
	for (size_t f = f0; f < f1; ++f)
		for (int c = 0; c < ch; ++c)
		{
			const double v = static_cast<double>(s[f * ch + c]) / 32768.0;
			sum += v * v;
		}
	return std::sqrt(sum / (static_cast<double>(f1 - f0) * ch));
}

Analysis analyze(const AudioAsset& a)
{
	Analysis an;
	const int ch = a.channels;
	an.frames = frameCountOf(a);
	if (ch <= 0 || an.frames == 0 || a.sampleRate <= 0) return an;

	const int16_t* s    = samplesOf(a);
	const double   rate = static_cast<double>(a.sampleRate);
	an.durationSec = static_cast<double>(an.frames) / rate;

	// One pass for peak, RMS, clipping and DC — they all want every sample, and at
	// tens of millions of samples per clip a second pass is a visible stall.
	// Integer accumulators: a squared sample tops out at 2^30, so int64 covers
	// ~8.6 billion samples (≈25 hours of 48 kHz stereo) before it could overflow.
	int      peak  = 0;
	int64_t  sum   = 0;
	uint64_t sumSq = 0;
	const size_t total = an.frames * static_cast<size_t>(ch);
	for (size_t i = 0; i < total; ++i)
	{
		const int v  = s[i];
		const int av = v < 0 ? -v : v;
		if (av > peak) peak = av;
		if (av >= 32767) ++an.clipped;
		sum   += v;
		sumSq += static_cast<uint64_t>(static_cast<int64_t>(v) * v);
	}
	const double totalD = static_cast<double>(total);
	an.peakDb   = toDb(static_cast<double>(peak) / 32768.0);
	an.rmsDb    = toDb(std::sqrt(static_cast<double>(sumSq) / totalD) / 32768.0);
	an.dcOffset = static_cast<float>(static_cast<double>(sum) / totalD / 32768.0);

	// Head/tail silence: walk in from both ends while every channel of the frame
	// stays under the threshold.
	const int silenceAmp = static_cast<int>(32768.0 * std::pow(10.0, kSilenceDb / 20.0));
	auto frameIsSilent = [&](size_t f)
	{
		for (int c = 0; c < ch; ++c)
		{
			const int v  = s[f * ch + c];
			const int av = v < 0 ? -v : v;
			if (av > silenceAmp) return false;
		}
		return true;
	};
	size_t lead = 0;
	while (lead < an.frames && frameIsSilent(lead)) ++lead;
	an.leadSilenceSec = static_cast<double>(lead) / rate;
	if (lead < an.frames)   // an all-silent clip is not "silence at both ends"
	{
		size_t tail = 0;
		while (tail < an.frames - lead && frameIsSilent(an.frames - 1 - tail)) ++tail;
		an.tailSilenceSec = static_cast<double>(tail) / rate;
	}

	// Loop seam. The step is the worst per-channel jump across the wrap; the
	// level delta compares a quarter-second at each end.
	int step = 0;
	for (int c = 0; c < ch; ++c)
	{
		const int d = std::abs(static_cast<int>(s[(an.frames - 1) * ch + c]) -
		                       static_cast<int>(s[c]));
		if (d > step) step = d;
	}
	an.seamStepDb = toDb(static_cast<double>(step) / 32768.0);

	const size_t win = std::min<size_t>(an.frames / 2, static_cast<size_t>(rate * 0.25));
	if (win > 0)
	{
		const double head = rmsOf(a, 0, win);
		const double tail = rmsOf(a, an.frames - win, an.frames);
		an.seamLevelDb = toDb(head) - toDb(tail);
	}
	return an;
}

// ── Formatting ───────────────────────────────────────────────────────────────

void formatBytes(size_t bytes, char* buf, size_t n)
{
	const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
	if (mb >= 1.0) std::snprintf(buf, n, "%.1f MB", mb);
	else           std::snprintf(buf, n, "%.0f KB", static_cast<double>(bytes) / 1024.0);
}

// ── Transport ────────────────────────────────────────────────────────────────

void stopPreview(State& st)
{
	if (st.audio && st.handle) st.audio->stop(st.handle);
	st.handle = 0;
	st.paused = false;
}

// The clip as the canvas reads it. clipOf() only ever hands back int16 PCM.
HE::Ed::AudioWave::Clip waveClipOf(const AudioAsset& a)
{
	HE::Ed::AudioWave::Clip c;
	c.samples    = samplesOf(a);
	c.frames     = frameCountOf(a);
	c.channels   = a.channels;
	c.sampleRate = a.sampleRate;
	return c;
}

// Start a voice on what Play means right now: the selection if one is marked,
// else the whole clip, from the playhead when it sits inside that range.
//
// The engine has no loop region, so a selection is played as a copy of just
// its frames, and the voice's own loop flag loops exactly the selection. The
// engine copies whatever play() is handed anyway (AudioEngine.h), so the slice
// can die at the end of this function; for the whole clip there is no slice
// at all — the asset's buffer goes straight in, as before.
void startPreview(const AudioAsset& clip, State& st, const HE::AudioEdit* edit)
{
	if (!st.audio || !st.audio->isInitialized()) return;
	stopPreview(st);

	namespace AW = HE::Ed::AudioWave;
	const size_t frames = frameCountOf(clip);
	const AW::PlayRange r = AW::playRange(st.view, frames);
	if (r.end <= r.begin) return;

	// An asset's voice carries its volume curve, offset to where the played
	// range sits in the clip, so the preview hears it where it is drawn — and,
	// because the curve stage is in even for an empty curve, hears every edit of
	// it while it plays (setSoundEnvelope after each change). The same stage
	// runs its EQ (setSoundEq), and the voice goes through the bus the game
	// would route the clip to, so the mixer's fader, mute and solo — and its
	// voice count — apply to the preview too. A raw file has none of these and
	// plays as it is, on master.
	const std::string bus = edit ? st.audio->routeFor({}, edit->bus) : std::string();
	auto playBytes = [&](const std::vector<uint8_t>& bytes)
	{
		return edit ? st.audio->play(bytes, clip.sampleRate, clip.channels, edit->envelope, r.begin,
		                             st.volume, st.pitch, st.loop, bus, &edit->eq)
		            : st.audio->play(bytes, clip.sampleRate, clip.channels,
		                             st.volume, st.pitch, st.loop, {});
	};
	if (r.begin == 0 && r.end == frames)
		st.handle = playBytes(clip.audioData);
	else
	{
		const size_t bytesPerFrame = sizeof(int16_t) * static_cast<size_t>(clip.channels);
		const std::vector<uint8_t> slice(clip.audioData.begin() + std::ptrdiff_t(r.begin * bytesPerFrame),
		                                 clip.audioData.begin() + std::ptrdiff_t(r.end   * bytesPerFrame));
		st.handle = playBytes(slice);
	}
	if (!st.handle)
	{
		HE_LOG_ERROR(Editor, "%s", ("Audio preview failed to start for " + st.name).c_str());
		return;
	}
	st.voiceBegin    = r.begin;
	st.voiceEnd      = r.end;
	st.view.playhead = r.start;
	if (r.start > r.begin) st.audio->seekSound(st.handle, r.start - r.begin);
}

// ── Edits ────────────────────────────────────────────────────────────────────

// The asset whose `edit` this tab changes: the ContentManager's own loaded copy.
// Fetched per use and never held — any load can move it (ContentManager.h).
AudioAsset* editableOf(AppContext& ctx, const State& st)
{
	if (st.isRawFile || !ctx.contentManager) return nullptr;
	return ctx.contentManager->getAudioMutable(st.clipId);
}

// The edit as it is now becomes the newest undo point. The same as the one
// already on top is no step at all, so a click that changed nothing costs none.
void pushUndo(State& st, const HE::AudioEdit& edit)
{
	std::string snap = edit.toChunkText();
	if (st.undoPos >= 0 && st.undoPos < static_cast<int>(st.undo.size()) && st.undo[st.undoPos] == snap)
		return;
	st.undo.resize(static_cast<size_t>(st.undoPos + 1));
	st.undo.push_back(std::move(snap));
	if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
	st.undoPos = static_cast<int>(st.undo.size()) - 1;
}

bool restoreSnapshot(AppContext& ctx, State& st, int pos)
{
	if (pos < 0 || pos >= static_cast<int>(st.undo.size())) return false;
	AudioAsset* a = editableOf(ctx, st);
	if (!a) return false;
	a->edit.fromChunkText(st.undo[pos]);
	st.undoPos = pos;
	st.dirty   = true;
	return true;
}

// A finished edit: dirty, and an undo point.
void commitEdit(State& st, const HE::AudioEdit& edit)
{
	st.dirty = true;
	pushUndo(st, edit);
}

bool saveState(AppContext& ctx, State& st)
{
	st.lastSaveError.clear();
	AudioAsset* a = editableOf(ctx, st);
	if (!a)
	{
		st.lastSaveError = "Not saved: this clip is no longer loaded.";
		return false;
	}
	// The samples go back into the file exactly as they were loaded — the same
	// bytes in the same chunk — so the edit chunk is the only thing that changes.
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	return true;
}

// Edits thrown away (reloadFromDisk): the edit chunk as the file has it now goes
// back onto the loaded asset. The samples are not re-read — nothing here ever
// changes them.
void reloadEdits(AppContext& ctx, State& st, const std::string& assetPath)
{
	if (AudioAsset* a = editableOf(ctx, st))
	{
		std::vector<uint8_t> bytes;
		if (Importer::readAssetChunk(assetPath, HAsset::CHUNK_AUED, bytes))
			a->edit.fromChunkText(std::string(bytes.begin(), bytes.end()));
		else
			a->edit = HE::AudioEdit{};
	}
	st.undo.clear();
	st.undoPos = -1;
	st.dirty   = false;
}

// The trim as the canvas draws it and Play plays it, read off the asset every
// frame, so an undo, a reload or a save shows at once. A trim that resolves to
// the whole clip is drawn as none.
void syncTrimView(State& st, const AudioAsset* editable, size_t frames)
{
	st.view.trimBegin = st.view.trimEnd = 0;
	if (!editable || editable->edit.trim.isDefault()) return;
	const HE::AudioTrim::Range r = editable->edit.trim.resolve(frames);
	if (r.begin == 0 && r.end == frames) return;
	st.view.trimBegin = static_cast<size_t>(r.begin);
	st.view.trimEnd   = static_cast<size_t>(r.end);
}

void applyTrim(State& st, AudioAsset& a, const HE::AudioTrim& t)
{
	if (a.edit.trim.startFrame == t.startFrame && a.edit.trim.endFrame == t.endFrame) return;
	// What Play means just changed under a running voice; start over rather
	// than keep playing a range that is no longer the clip.
	stopPreview(st);
	a.edit.trim = t;
	commitEdit(st, a.edit);
}

// The curve changed (a drag in progress, a toolbar button, a delete): the
// edit is unsaved, and a voice that is playing hears the new curve at once.
// `commit` makes it an undo point — a drag commits once, when it lets go.
void curveChanged(State& st, AudioAsset& a, bool commit)
{
	st.dirty = true;
	if (st.handle && st.audio) st.audio->setSoundEnvelope(st.handle, a.edit.envelope);
	if (commit) pushUndo(st, a.edit);
}

// The same for the EQ: heard at once in a running preview, an undo point when
// the gesture is finished.
void eqChanged(State& st, AudioAsset& a, bool commit)
{
	st.dirty = true;
	if (st.handle && st.audio) st.audio->setSoundEq(st.handle, a.edit.eq);
	if (commit) pushUndo(st, a.edit);
}

// The project's bus list — THE one the Audio Mixer edits, read every frame, so
// a bus added or removed there is in (or missing from) the dropdown at once.
// Null when no project is open.
const HE::AudioBusConfig* projectBuses(AppContext& ctx)
{
	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty()) return nullptr;
	return &ctx.projectManager->currentProject().audioBuses;
}

// Where a file this tab writes lands: next to the clip, or — for a clip in the
// engine library, which is read-only outside engine-content dev mode — in the
// project's own Content/Audio, which is somewhere the project can actually
// reference. Import and Extract share it. An empty root = no project open.
struct WriteTarget
{
	std::filesystem::path root, relDir;
	bool                  engineLocked = false;
};

WriteTarget writeTargetFor(AppContext& ctx, const std::string& assetPath)
{
	WriteTarget t;
	if (!ctx.contentManager) return t;
	const std::filesystem::path src(assetPath);
	t.engineLocked = ctx.contentManager->isEngineDefaultPath(assetPath) &&
	                 !ContentManager::isEngineContentDevMode();
	if (t.engineLocked)
	{
		t.root   = ctx.contentManager->contentRoot();
		t.relDir = "Audio";
	}
	else
	{
		t.root = ctx.contentManager->isEngineDefaultPath(assetPath)
			? std::filesystem::path(ctx.contentManager->engineContentRoot())
			: std::filesystem::path(ctx.contentManager->contentRoot());
		std::error_code ec;
		t.relDir = std::filesystem::relative(src.parent_path(), t.root, ec);
		if (ec || t.relDir == ".") t.relDir.clear();
	}
	return t;
}

// Extract Selection, run after the window has ended. The clip is fetched again
// (no pointer from the frame lives this long), what the new asset carries is
// copied out of the original's edit, and the original's file is never opened
// for writing: the importer writes a NEW file through a ContentManager of its
// own, and the editor's picks it up with the content refresh asked for here.
void runExtract(AppContext& ctx, State& st, const std::string& assetPath)
{
	st.extractFailed = true;
	const AudioAsset* clip   = clipOf(ctx, st);
	const size_t      frames = clip ? frameCountOf(*clip) : 0;
	if (!clip || clip->sampleRate <= 0 || !st.view.hasSelection() || st.view.selEnd > frames)
	{
		st.extractStatus = "Nothing extracted: select a range first.";
		return;
	}
	const WriteTarget t = writeTargetFor(ctx, assetPath);
	if (t.root.empty())
	{
		st.extractStatus = "Nothing extracted: open a project first.";
		return;
	}
	const double  rate = static_cast<double>(clip->sampleRate);
	HE::AudioEdit carried;
	if (const AudioAsset* a = editableOf(ctx, st))
		carried = a->edit.forRange(static_cast<double>(st.view.selBegin) / rate,
		                           static_cast<double>(st.view.selEnd)   / rate);
	const std::string stem = std::filesystem::path(assetPath).stem().string() + "_extract";

	// Baking multiplies the original's curve into the samples — against the
	// original's frame numbers, the gain its own voice applies there — and the
	// extract then carries no curve of its own (extractRange clears it).
	const AudioAsset*        owner = editableOf(ctx, st);
	const HE::AudioEnvelope* bake  = (st.bakeCurve && owner && !owner->edit.envelope.empty())
		? &owner->edit.envelope : nullptr;

	AudioImporter::ExtractResult r;
	if (!AudioImporter::extractRange(*clip, st.view.selBegin, st.view.selEnd,
	                                 t.root, t.relDir, stem, carried, r, bake))
	{
		st.extractStatus = "Nothing extracted: the new asset could not be written (the log says why).";
		return;
	}
	st.extractFailed = false;
	st.extractStatus = "Extracted " + HE::Ed::AudioWave::formatFrames(static_cast<size_t>(r.frames)) +
	                   " frames to " + r.path + (bake ? ", volume curve baked in" : "");
	if (r.clampedSamples > 0)
		st.extractStatus += " (" + std::to_string(r.clampedSamples) +
		                    " samples clipped at full scale: the curve lifts it past 0 dBFS)";
	ctx.contentRefreshPending = true;
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool isAudioAsset(const std::string& path)
{
	if (AudioImporter::isSupportedSource(path)) return true;
	return EditorAssetTypeCache::is(path, HE::AssetType::Audio);
}

void forget(const std::string& assetPath)
{
	if (State* st = s_states.find(assetPath)) stopPreview(*st);
	s_states.forget(assetPath);
}

bool isDirty(const std::string& assetPath) { return s_states.dirty(assetPath); }

void appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }

void appendSnapshots(AppContext& ctx, std::vector<HE::Ed::AssetSnapshotSource>& out)
{
	ContentManager* cm = ctx.contentManager;
	if (!cm) return;
	s_states.forEach([&](const std::string&, State& st) {
		if (!st.dirty || st.isRawFile || st.relPath.empty()) return;
		out.push_back({ cm->resolveSavePath(st.relPath), [cm, &st](const std::string& dest) {
			// The loaded clip IS the edit buffer (saveState writes it as it is).
			const AudioAsset* a = cm->getAudio(st.clipId);
			if (!a) return false;
			AudioAsset copy = *a;
			return cm->writeAssetTo(copy, dest);
		} });
	});
}

bool save(AppContext& ctx, const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	if (!st || !st->dirty) return true;   // "not mine" reads as success
	return saveState(ctx, *st);
}

bool reloadFromDisk(const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	if (!st || st->isRawFile) return false;
	// The next render puts the file's edit back onto the loaded asset; that
	// needs the ContentManager, which only render() is handed.
	st->editsStale = true;
	st->dirty      = false;
	return true;
}

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = s_states[assetPath];
	st.audio  = ctx.audioEngine;
	HE::Ed::Help::Scope helpScope("Audio Editor");

	if (!st.loaded)
	{
		st.isRawFile = AudioImporter::isSupportedSource(assetPath);

		if (st.isRawFile)
		{
			// Loaded into the panel, never registered: a source file is not an
			// asset, and the ContentManager addresses assets by UUID.
			st.name    = std::filesystem::path(assetPath).filename().string();
			st.relPath = ctx.contentManager
				? ctx.contentManager->toContentRelativePath(assetPath) : assetPath;
			st.decodeFailed = !AudioImporter::decode(assetPath, st.raw);
		}
		else if (ctx.contentManager)
		{
			st.clipId = openPanelAsset(ctx, assetPath, st.name, st.relPath);
		}
		st.loaded = true;
	}

	// A REAL host window pinned to the tab area, not a bare BeginChild: with no
	// window open, every ImGui call lands in the implicit "Debug" window — which
	// has a title bar and is user-movable, so the whole tab appeared inside a
	// draggable floating window. Same setup as ScriptEditorPanel.
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
	ImGui::Begin("##AudioEditor", nullptr,
		ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoDocking);
	ImGui::PopStyleVar(2);

	const AudioAsset* clip = clipOf(ctx, st);
	// A zero sample rate is as unusable as no samples at all — every time readout
	// below divides by it — and the importer always writes a real one, so a clip
	// without one is a broken asset rather than a case to render around.
	if (!clip || clip->channels <= 0 || clip->sampleRate <= 0 || frameCountOf(*clip) == 0)
	{
		// The whole content of the panel in this state is one sentence, and it is
		// the sentence that says WHY there is nothing to look at — the parenthesis
		// naming the formats we cannot decode is at the very end of it, which is
		// precisely the part that runs off the right edge of a docked tab and gets
		// clipped. The scope closes before the End() below: a wrap position still
		// pushed at that point would be popped off a window that has already been
		// ended, which is an assertion in a build nobody runs with assertions on.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled(st.decodeFailed
				? "Could not decode '%s'. WAV and Ogg Vorbis are supported "
				  "(mp3/flac have no decoder linked in)."
				: "Could not load '%s' as an audio clip.", st.name.c_str());
		}
		ImGui::End();
		return;
	}

	namespace AW = HE::Ed::AudioWave;
	const AW::Clip wclip = waveClipOf(*clip);
	if (!st.peaksDone)    { st.peaks    = AW::buildPeaks(wclip); st.peaksDone    = true; }
	if (!st.analysisDone) { st.analysis = analyze(*clip);        st.analysisDone = true; }

	const Analysis& an     = st.analysis;
	const size_t    frames = an.frames;
	const double    rate   = static_cast<double>(clip->sampleRate);
	const bool      audioReady = st.audio && st.audio->isInitialized();

	// ── The edit ─────────────────────────────────────────────────────────────
	// Engine content is read-only outside engine-content dev mode: its edits
	// could not be saved, so they cannot be made either. Extract still works —
	// it writes into the project (writeTargetFor).
	if (st.editsStale) { st.editsStale = false; reloadEdits(ctx, st, assetPath); }
	AudioAsset* editable    = editableOf(ctx, st);
	const bool  editsLocked = ctx.contentManager && ctx.contentManager->isEngineDefaultPath(assetPath) &&
	                          !ContentManager::isEngineContentDevMode();
	const bool  canEdit     = editable != nullptr && !editsLocked;
	// The baseline undo goes back to, taken on the first frame the asset is there.
	if (editable && st.undoPos < 0) pushUndo(st, editable->edit);
	if (editable && !st.showEqInit)
	{
		st.showEqInit = true;
		st.showEq     = !editable->edit.eq.bands.empty();
	}
	syncTrimView(st, editable, frames);

	// ── Follow the running voice ─────────────────────────────────────────────
	// A paused voice is alive but not playing, so it must be excluded from the
	// finished-voice reaping below — otherwise Pause would free the clip and the
	// next Resume would have nothing to resume.
	if (st.handle && audioReady && !st.paused)
	{
		if (st.audio->isPlaying(st.handle))
		{
			// A scrub in progress owns the playhead; the voice follows it.
			if (st.view.drag != AW::View::Drag::Scrub)
			{
				// The view follows a playhead that walks off its right edge, page
				// by page — but only one that was on screen: somebody who scrolled
				// away during playback to look at another part of the clip is not
				// yanked back, and nor is anybody in the middle of a drag.
				const double span = st.view.framesPerPx * static_cast<double>(st.canvasW);
				const double was  = static_cast<double>(st.view.playhead);
				const bool   following = st.canvasW > 0.0f &&
					was >= st.view.viewStart && was <= st.view.viewStart + span;
				st.view.playhead = std::min(st.voiceEnd,
					st.voiceBegin + static_cast<size_t>(st.audio->getSoundCursorFrames(st.handle)));
				if (following && st.view.drag == AW::View::Drag::None)
					AW::reveal(st.view, frames, st.canvasW, st.view.playhead);
			}
		}
		else
		{
			// Ran off the end (a non-looping voice). Park the playhead at the end
			// of what it played — the selection's end when it played one — and
			// give the PCM copy back: these clips are tens of megabytes.
			st.view.playhead = st.voiceEnd;
			stopPreview(st);
		}
	}

	// ── Toolbar ──────────────────────────────────────────────────────────────
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, assetPath, st.dirty);
		// Folder, then Save, then the transport: what you do to LISTEN comes first
		// on the left. The tools that CHANGE the clip (trim, curve, EQ) and the
		// ruler unit sit together at the right edge.
		if (T::saveButton(bar, st.dirty && editable != nullptr, /*atLeft=*/true)) saveState(ctx, st);

		bar.group();
		const bool playing = st.handle != 0 && !st.paused;
		if (bar.item("##audioplay", playing ? T::iconPause : T::iconPlay, nullptr,
		             playing, audioReady,
		             audioReady ? "Play / Pause — the selection if there is one, else the (trimmed) clip"
		                        : "No audio device — the editor's audio engine failed to start",
		             "Audio Editor/Play"))
		{
			if (!st.handle)          startPreview(*clip, st, editable ? &editable->edit : nullptr);
			else if (st.paused)      { st.audio->resumeSound(st.handle); st.paused = false; }
			else                     { st.audio->pauseSound(st.handle);  st.paused = true;  }
		}
		if (bar.item("##audiostop", T::iconStop, nullptr, false, st.handle != 0, "Stop and rewind",
		             "Audio Editor/Stop"))
		{
			stopPreview(st);
			st.view.playhead = st.view.hasSelection() ? st.view.selBegin
			                 : st.view.hasTrim()      ? st.view.trimBegin : 0;
		}
		if (bar.item("##audioloop", T::iconRefresh, nullptr, st.loop, true,
		             "Loop the selection, or the clip — the seam check says whether the clip's loop clicks",
		             "Audio Editor/Loop"))
		{
			st.loop = !st.loop;
			if (st.handle) st.audio->setSoundLooping(st.handle, st.loop);
		}
		if (bar.item("##audiozoomsel", T::iconSearch, nullptr, false,
		             st.view.hasSelection() && st.canvasW > 0.0f,
		             "Zoom to the selection", "Audio Editor/Zoom to Selection"))
			AW::zoomToRange(st.view, frames, st.canvasW, st.view.selBegin, st.view.selEnd);
		if (bar.item("##audiofit", T::iconFit, nullptr, false, true, "Fit the whole clip",
		             "Audio Editor/Fit"))
			st.view.framesPerPx = 0.0;   // refitted on the next draw, which knows the width
		bar.endGroup();

		if (st.isRawFile)
		{
			bar.group();
			bar.readout(nullptr, "source file — not imported", T::kFgDim);
			bar.endGroup();
		}

		// Right-hand wells stack leftwards, so they are declared rightmost first:
		// ruler unit, EQ, volume curve, cutting. The cutting well is the one that
		// reads and changes the selection, and nothing below depends on the order
		// they run in — syncTrimView after them draws this frame's.
		bar.rightGroup(bar.labelGroupWidth({ "Samples" }));
		if (bar.item("##audiosamples", nullptr, "Samples", st.view.rulerInSamples, true,
		             "Label the ruler in frames instead of time", "Audio Editor/Samples"))
			st.view.rulerInSamples = !st.view.rulerInSamples;
		bar.endGroup();


		// The EQ pane under the waveform. (The bus is a dropdown in the left
		// column: a list of names does not fit a toolbar cell.)
		bar.rightGroup(bar.labelGroupWidth({ "EQ" }));
		if (bar.item("##audioeq", nullptr, "EQ", st.showEq && editable != nullptr, editable != nullptr,
		             editable ? "Show the clip's EQ under the waveform"
		                      : "A source file has no asset to keep an EQ in — import it first",
		             "Audio Editor/EQ"))
			st.showEq = !st.showEq;
		bar.endGroup();
		// Volume curve. "Curve" switches the lanes from selecting to editing
		// points; Linear/Smooth shape the segment that starts at the selected
		// point (the left point owns its segment, AudioEdit.h).
		bar.rightGroup(bar.labelGroupWidth({ "Curve", "Linear", "Smooth", "Clear Curve" }));
		if (bar.item("##audiocurve", nullptr, "Curve", st.view.curveMode, canEdit,
		             !canEdit ? (st.isRawFile ? "A source file has no asset to keep a curve in — import it first"
		                                      : "Engine content is read-only")
		                      : "Edit the volume curve: click to add a point, drag to move it, "
		                        "right-click or double-click to delete it",
		             "Audio Editor/Curve"))
		{
			st.view.curveMode = !st.view.curveMode;
			st.view.curveSel  = -1;
		}
		const int  sel      = st.view.curveSel;
		const bool hasPoint = canEdit && st.view.curveMode && sel >= 0 &&
		                      sel < static_cast<int>(editable->edit.envelope.points.size());
		const HE::AudioCurveInterp selInterp = hasPoint
			? editable->edit.envelope.points[static_cast<size_t>(sel)].interp : HE::AudioCurveInterp::Linear;
		if (bar.item("##audiocurvelinear", nullptr, "Linear", hasPoint && selInterp == HE::AudioCurveInterp::Linear,
		             hasPoint, hasPoint ? "A straight line from the selected point to the next one"
		                                : "Select a curve point first",
		             "Audio Editor/Linear") && hasPoint)
		{
			editable->edit.envelope.points[static_cast<size_t>(sel)].interp = HE::AudioCurveInterp::Linear;
			curveChanged(st, *editable, true);
		}
		if (bar.item("##audiocurvesmooth", nullptr, "Smooth", hasPoint && selInterp == HE::AudioCurveInterp::Smooth,
		             hasPoint, hasPoint ? "An eased curve from the selected point to the next one — no corner at either end"
		                                : "Select a curve point first",
		             "Audio Editor/Smooth") && hasPoint)
		{
			editable->edit.envelope.points[static_cast<size_t>(sel)].interp = HE::AudioCurveInterp::Smooth;
			curveChanged(st, *editable, true);
		}
		if (bar.item("##audiocurveclear", nullptr, "Clear Curve", false,
		             canEdit && !editable->edit.envelope.empty(),
		             "Remove every point: the clip plays at its own level again", "Audio Editor/Clear Curve"))
		{
			editable->edit.envelope.points.clear();
			st.view.curveSel = -1;
			curveChanged(st, *editable, true);
		}
		bar.endGroup();

		// Cutting. Trim is an edit of THIS asset — nothing is deleted, it is
		// undoable and saved with the asset. Extract writes a NEW asset made of
		// the selected frames and leaves this one as it is.
		const bool hasSel = st.view.hasSelection();
		bar.rightGroup(bar.labelGroupWidth({ "Trim", "Clear Trim", "Extract" }) + 6.0f);
		if (bar.item("##audiotrim", nullptr, "Trim", false, canEdit && hasSel,
		             !canEdit ? (st.isRawFile ? "A source file has no asset to keep a trim in — import it first"
		                                      : "Engine content is read-only — Extract the range instead")
		                      : hasSel ? "Play only the selection from now on — nothing is deleted"
		                               : "Select a range to trim the clip to",
		             "Audio Editor/Trim"))
		{
			applyTrim(st, *editable, HE::AudioTrim::fromRange(st.view.selBegin, st.view.selEnd, frames));
			st.view.playhead = st.view.selBegin;
			AW::clearSelection(st.view);
		}
		if (bar.item("##audiountrim", nullptr, "Clear Trim", false, canEdit && st.view.hasTrim(),
		             "Play the whole clip again", "Audio Editor/Clear Trim"))
			applyTrim(st, *editable, HE::AudioTrim{});
		bar.divider();
		if (bar.item("##audioextract", nullptr, "Extract", false, hasSel && ctx.contentManager != nullptr,
		             hasSel ? "Write the selection as a new audio asset beside this one"
		                    : "Select a range to extract",
		             "Audio Editor/Extract"))
			st.extractRequested = true;
		bar.endGroup();

		// The buttons above may have changed the trim; the canvas below and the
		// transport draw this frame's.
		syncTrimView(st, editable, frames);
	}

	// ── Left: format, levels, loop ───────────────────────────────────────────
	ImGui::BeginChild("##audioInfo", ImVec2(290.0f, 0.0f), true);
	{
		// This column is pinned to 290 px, which makes it the narrowest thing in
		// the tab, and nearly everything in it is a sentence about the clip: what
		// a DC offset will do to the mix, where an import will write, which
		// formats have no decoder linked in. Unwrapped, each of those runs under
		// the waveform pane and is clipped there — the reader is told "The asset
		// stores decoded int16 PCM (4.1 MB) and ships with" and nothing on screen
		// admits a word went missing. Held for the whole child rather than per
		// line so the next readout somebody adds here inherits it, and safely so:
		// nothing in this column is laid out from the width of the text it prints
		// (SeparatorText measures its own label, the sliders and the button are
		// sized by ImGui), so a wrap column has nothing here to disturb.
		EditorWidgets::WrapText wrap;
		char buf[64];

		ImGui::SeparatorText("Format");
		ImGui::Text("Duration    %s", AW::formatTime(an.durationSec).c_str());
		ImGui::Text("Sample rate %d Hz", clip->sampleRate);
		ImGui::Text("Channels    %d%s", clip->channels,
		            clip->channels == 1 ? " (mono)" : clip->channels == 2 ? " (stereo)" : "");
		ImGui::Text("Frames      %zu", frames);
		if (st.compressedBytes > 0)
		{
			// The clip is Vorbis: what it costs is the compressed size, which is
			// also what a playing voice holds — the PCM only exists in this tab.
			formatBytes(st.compressedBytes, buf, sizeof(buf));
			ImGui::Text("Ogg Vorbis  %s (streamed)", buf);
			formatBytes(clip->audioData.size(), buf, sizeof(buf));
			ImGui::Text("Decoded     %s (editor only)", buf);
		}
		else
		{
			formatBytes(clip->audioData.size(), buf, sizeof(buf));
			ImGui::Text("PCM in RAM  %s (int16)", buf);
		}

		ImGui::SeparatorText("Levels");
		ImGui::Text("Peak        %.1f dBFS", an.peakDb);
		ImGui::Text("RMS         %.1f dBFS", an.rmsDb);
		if (an.clipped > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%zu clipped samples", an.clipped);
		else
			ImGui::TextDisabled("No clipping.");
		if (std::fabs(an.dcOffset) > 0.005f)
		{
			ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f), "DC offset %.3f", an.dcOffset);
			ImGui::TextWrapped("The waveform sits off centre. It wastes headroom and can thump "
			                   "when the sound starts or stops.");
		}
		if (an.peakDb < -12.0f)
			ImGui::TextWrapped("Quiet master — %.1f dB of headroom is left unused.", -an.peakDb);

		ImGui::SeparatorText("Silence");
		if (an.leadSilenceSec > 0.01 || an.tailSilenceSec > 0.01)
		{
			ImGui::Text("Head        %.2f s", an.leadSilenceSec);
			ImGui::Text("Tail        %.2f s", an.tailSilenceSec);
			ImGui::TextWrapped("Padding below %.0f dBFS. Harmless for a one-shot, but it is a "
			                   "gap in a loop.", kSilenceDb);
		}
		else
			ImGui::TextDisabled("None at either end.");

		ImGui::SeparatorText("Loop seam");
		// What matters for the ambience beds this tab was built for: what the wrap
		// from the last frame back to the first actually sounds like.
		if (an.seamStepDb > -30.0f)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Step  %.1f dBFS — audible click", an.seamStepDb);
		else if (an.seamStepDb > -50.0f)
			ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f), "Step  %.1f dBFS — faint tick", an.seamStepDb);
		else
			ImGui::Text("Step  %.1f dBFS — clean", an.seamStepDb);
		ImGui::Text("Level %+.1f dB head vs tail", an.seamLevelDb);
		if (std::fabs(an.seamLevelDb) > 3.0f)
			ImGui::TextWrapped("The two ends sit at different loudness — the loop will breathe "
			                   "even without a click. A crossfade fixes both.");

		ImGui::SeparatorText("Cut");
		if (st.view.hasTrim())
		{
			ImGui::Text("Plays %s of %s",
			            AW::formatTime(static_cast<double>(st.view.trimEnd - st.view.trimBegin) / rate).c_str(),
			            AW::formatTime(an.durationSec).c_str());
			ImGui::TextDisabled("Frames %s to %s. Nothing is deleted; Clear Trim brings the rest back.",
			                    AW::formatFrames(st.view.trimBegin).c_str(),
			                    AW::formatFrames(st.view.trimEnd).c_str());
		}
		else if (st.isRawFile)
			ImGui::TextDisabled("A source file cannot be trimmed: import it first. Extract works on it as it is.");
		else if (editsLocked)
			ImGui::TextDisabled("Engine content is read-only, so it cannot be trimmed here. Extract "
			                    "writes the selection into the project instead.");
		else
			ImGui::TextDisabled("Untrimmed. Select a range and press Trim to play only that.");
		if (editable && !editable->edit.envelope.empty())
		{
			ImGui::Checkbox("Bake Curve into Extract", &st.bakeCurve);
			EditorWidgets::helpForLabel("Bake Curve into Extract");
		}
		if (!st.extractStatus.empty())
			ImGui::TextColored(st.extractFailed ? ImVec4(1.0f, 0.6f, 0.4f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
			                   "%s", st.extractStatus.c_str());

		ImGui::SeparatorText("Volume curve");
		if (editable && !editable->edit.envelope.empty())
		{
			const HE::AudioEnvelope& env = editable->edit.envelope;
			float lo = env.points.front().gain, hi = lo;
			for (const HE::AudioEnvelopePoint& p : env.points) { lo = std::min(lo, p.gain); hi = std::max(hi, p.gain); }
			ImGui::Text("%zu point%s", env.points.size(), env.points.size() == 1 ? "" : "s");
			ImGui::Text("Low   %s", AW::formatGain(lo).c_str());
			ImGui::Text("High  %s", AW::formatGain(hi).c_str());
			ImGui::TextDisabled("Heard in this preview and wherever the game plays the clip.");
		}
		else if (canEdit)
			ImGui::TextDisabled("No curve: the clip plays at its own level. Press Curve and click "
			                    "the waveform to add points.");
		else
			ImGui::TextDisabled("Only an imported, editable asset can carry a volume curve.");

		// ── Mixer bus ────────────────────────────────────────────────────────
		// Which Audio Mixer bus the clip plays through wherever the game plays
		// it, unless an Audio Source names one of its own. The list is the
		// project's (the mixer's), read every frame.
		ImGui::SeparatorText("Mixer bus");
		if (editable)
		{
			const HE::AudioBusConfig* buses = projectBuses(ctx);
			std::string bus = editable->edit.bus;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (HE::Ed::AudioMix::drawBusCombo(buses, bus, canEdit))
			{
				editable->edit.bus = bus;
				commitEdit(st, editable->edit);
				// A voice cannot change buses while it plays: start it again
				// on the new one, from where it is.
				if (st.handle && !st.paused) startPreview(*clip, st, &editable->edit);
				else                         stopPreview(st);
			}
			const HE::Ed::AudioMix::BusChoice bc = HE::Ed::AudioMix::busChoice(buses, editable->edit.bus);
			if (bc.missing)
				ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.38f, 1.0f), "%s", bc.hint.c_str());
			else if (!buses)
				ImGui::TextDisabled("Open a project to choose one of its mixer buses.");
			else if (buses->buses.empty())
				ImGui::TextDisabled("The project has no buses yet: add them in Window > Audio Mixer.");
			else
				ImGui::TextDisabled("An Audio Source with a Bus of its own still overrides this.");
		}
		else
			ImGui::TextDisabled("Only an imported asset can be assigned to a bus.");

		if (!st.lastSaveError.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", st.lastSaveError.c_str());

		ImGui::SeparatorText("Preview");
		if (!audioReady)
			ImGui::TextDisabled("No audio device.");
		if (ImGui::SliderFloat("Volume", &st.volume, 0.0f, 2.0f, "%.2f") && st.handle)
			st.audio->setSoundVolume(st.handle, st.volume);
		EditorWidgets::helpForLabel("Volume");
		if (ImGui::SliderFloat("Pitch", &st.pitch, 0.25f, 2.0f, "%.2f") && st.handle)
			st.audio->setSoundPitch(st.handle, st.pitch);
		EditorWidgets::helpForLabel("Pitch");
		ImGui::TextDisabled("Preview only — an Audio Source component\ncarries its own volume and pitch.");

		// ── Import, for a raw .wav/.ogg ──────────────────────────────────────
		if (st.isRawFile && ctx.contentManager)
		{
			ImGui::SeparatorText("Import");
			const std::filesystem::path src(assetPath);
			// Where the .hasset lands: next to the source, or the project's own
			// Content/Audio for a locked engine file (writeTargetFor).
			const WriteTarget             wt           = writeTargetFor(ctx, assetPath);
			const bool                    engineLocked = wt.engineLocked;
			const std::filesystem::path&  root         = wt.root;
			const std::filesystem::path&  relDir       = wt.relDir;

			const std::string target =
				(relDir.empty() ? src.stem().string() : (relDir / src.stem()).string()) + ".hasset";

			if (root.empty())
				ImGui::TextDisabled("Open a project to import.");
			else
			{
				if (EditorWidgets::button("Import as Audio Asset", ImVec2(-FLT_MIN, 0.0f)))
				{
					if (AudioImporter::import(src, root, relDir))
						ctx.contentRefreshPending = true;
					else
						HE_LOG_ERROR(Editor, "%s", ("Editor: audio import failed for " + assetPath).c_str());
				}
				ImGui::TextWrapped("Writes %s", target.c_str());
				if (engineLocked)
					ImGui::TextWrapped("Engine content is read-only, so this goes to the project "
					                   "instead. Set HE_ENGINE_CONTENT_EDITABLE=1 to import into "
					                   "the engine library itself.");
				if (st.compressedBytes > 0)
				{
					formatBytes(st.compressedBytes, buf, sizeof(buf));
					ImGui::TextDisabled("The asset keeps the Ogg Vorbis stream (%s), decoded "
					                    "while it plays, and ships with every packaged build.", buf);
				}
				else
				{
					formatBytes(clip->audioData.size(), buf, sizeof(buf));
					ImGui::TextDisabled("The asset stores decoded int16 PCM (%s) and ships with every "
					                    "packaged build.", buf);
				}
			}
		}
	}
	ImGui::EndChild();

	// ── Right: waveform + position readout ───────────────────────────────────
	ImGui::SameLine();
	ImGui::BeginChild("##audioWave", ImVec2(0.0f, 0.0f), true);
	{
		const ImVec2 avail   = ImGui::GetContentRegionAvail();
		const float  canvasW = avail.x;

		// The footer is composed up here, above the canvas it sits under, because
		// the canvas can only be sized once the footer's height is known — and the
		// only height that is ever right is the one measured from the very strings
		// that will be drawn. The state they read (playhead, selection, zoom,
		// pointer) is the state before AW::draw consumes this frame's input, so
		// during a drag the numbers are one frame behind; that is invisible,
		// whereas a footer whose height was guessed is not.
		//
		// Two rows. The first says where things are, each as time AND frame
		// number — the frame is what a cut in the next step will be made at. The
		// second is the pointer grammar, which is the first thing to go over the
		// right edge of a narrow tab, so both rows wrap instead of clipping.
		const std::string where = AW::readout(st.view, wclip, st.hoverFrame,
		                                      editable ? &editable->edit.envelope : nullptr);

		// framesPerPx is still zero the first time a clip is opened — AW::draw
		// fits the whole clip to the canvas below. Anticipate that fit rather than
		// flashing "0 s visible" for a frame.
		const double fpp = st.view.framesPerPx > 0.0
			? st.view.framesPerPx
			: static_cast<double>(frames) / static_cast<double>(std::max(64.0f, canvasW));
		char hint[320];
		if (st.view.curveMode && canEdit)
			std::snprintf(hint, sizeof(hint),
				"%s long  |  %.3g s visible  |  curve: click to add a point, drag to move it "
				"(Shift: gain only), right-click, double-click or Delete to remove it, "
				"Linear/Smooth shape the segment after the selected point",
				AW::formatTime(an.durationSec).c_str(), fpp * canvasW / rate);
		else
		std::snprintf(hint, sizeof(hint), EditorInput::trackpadPointer(ctx)
			? "%s long  |  %.3g s visible  |  drag the waveform to select, click to place the "
			  "playhead, shift-click to extend, drag the ruler to scrub, swipe to pan, "
			  "Cmd/Ctrl+scroll to zoom"
			: "%s long  |  %.3g s visible  |  drag the waveform to select, click to place the "
			  "playhead, shift-click to extend, drag the ruler to scrub, wheel to zoom, "
			  "middle-drag or the strip below to pan",
			AW::formatTime(an.durationSec).c_str(), fpp * canvasW / rate);

		const float wrapW   = std::max(1.0f, canvasW);
		const float whereH  = ImGui::CalcTextSize(where.c_str(), nullptr, false, wrapW).y;
		const float hintH   = ImGui::CalcTextSize(hint, nullptr, false, wrapW).y;
		const float footerH = whereH + hintH + ImGui::GetStyle().ItemSpacing.y * 2.0f;
		// The EQ pane, when it is open, takes the bottom of the column.
		const float eqH = st.showEq && editable ? std::clamp(avail.y * 0.42f, 200.0f, 280.0f) : 0.0f;

		const AW::Result res = AW::draw(wclip, st.peaks, st.view,
			ImVec2(canvasW, std::max(80.0f + AW::metrics().overviewH, avail.y - footerH - eqH)),
			EditorInput::trackpadPointer(ctx),
			editable ? &editable->edit.envelope : nullptr, canEdit);
		st.canvasW    = res.canvasW;
		st.hoverFrame = res.hoverFrame;
		if (editable && (res.curveEdited || res.curveCommitted))
			curveChanged(st, *editable, res.curveCommitted);

		// What the canvas did, applied to the voice. A new selection (or one
		// cleared by a click) changes what Play means, so a running voice is
		// restarted on it; a paused one is dropped, and the next Play starts on
		// the new range. A seek inside the range the voice holds just moves it.
		if (st.handle && audioReady)
		{
			if (res.selectionChanged)
			{
				if (st.paused) stopPreview(st);
				else           startPreview(*clip, st, editable ? &editable->edit : nullptr);
			}
			else if (res.seek)
			{
				const size_t f = std::clamp(st.view.playhead, st.voiceBegin,
				                            st.voiceEnd > st.voiceBegin ? st.voiceEnd - 1 : st.voiceBegin);
				st.audio->seekSound(st.handle, f - st.voiceBegin);
			}
		}

		{
			// The scope is closed before EndChild(): the wrap must come off this
			// child's stack, not off whatever window follows it.
			EditorWidgets::WrapText wrap;
			ImGui::TextUnformatted(where.c_str());
			ImGui::TextDisabled("%s", hint);
		}

		if (eqH > 0.0f)
		{
			ImGui::SeparatorText("EQ");
			const HE::Ed::AudioMix::EqResult er = HE::Ed::AudioMix::drawEq(
				editable->edit.eq, st.eqView, rate,
				ImVec2(canvasW, std::max(80.0f, ImGui::GetContentRegionAvail().y)), canEdit);
			if (er.edited || er.committed) eqChanged(st, *editable, er.committed);
		}
	}
	ImGui::EndChild();

	// ── Keyboard shortcuts (skip while typing in a field) ────────────────────
	// The tab's own undo, like the Sequencer's: Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z or
	// Ctrl/Cmd+Y, Ctrl/Cmd+S. An undo changes the trim under a running voice, so
	// it stops the voice the way the Trim button does.
	const bool typing = ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
	if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
	{
		const ImGuiIO& io   = ImGui::GetIO();
		const bool     ctrl = io.KeyCtrl || io.KeySuper;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S) && st.dirty) saveState(ctx, st);
		// Delete / Backspace removes the selected curve point.
		AudioAsset* ed = canEdit ? editableOf(ctx, st) : nullptr;
		if (ed && st.view.curveMode && st.view.curveSel >= 0 &&
		    st.view.curveSel < static_cast<int>(ed->edit.envelope.points.size()) &&
		    (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
		{
			HE::Ed::AudioWave::curveErase(ed->edit.envelope, st.view.curveSel);
			st.view.curveSel = -1;
			curveChanged(st, *ed, true);
		}
		if (ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z) &&
		    restoreSnapshot(ctx, st, st.undoPos - 1))
			stopPreview(st);
		if (((ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) ||
		     (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))) &&
		    restoreSnapshot(ctx, st, st.undoPos + 1))
			stopPreview(st);
	}

	ImGui::End();

	// Deferred to here, see State::extractRequested. `clip` and `editable` are
	// not used past this point.
	if (st.extractRequested)
	{
		st.extractRequested = false;
		runExtract(ctx, st, assetPath);
	}
}

} // namespace AudioEditorPanel
