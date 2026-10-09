#include "AudioMixView.h"
#include "EditorWidgets.h"   // helpForKey — tooltips queued to the frame's end

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace HE::Ed::AudioMix
{

// ── Bus ──────────────────────────────────────────────────────────────────────

BusChoice busChoice(const HE::AudioBusConfig* buses, const std::string& assetBus)
{
	BusChoice c;
	if (assetBus.empty())
	{
		c.preview = "Master";
		return c;
	}
	if (buses && buses->find(assetBus))
	{
		c.preview = assetBus;
		return c;
	}
	c.missing = true;
	c.preview = assetBus + " (missing)";
	c.hint    = "The mixer has no bus called '" + assetBus + "' any more: it was removed or "
	            "renamed. The clip plays on Master until you pick a bus here, or a bus "
	            "with that name is added again.";
	return c;
}

bool drawBusCombo(const HE::AudioBusConfig* buses, std::string& assetBus, bool enabled)
{
	const BusChoice choice = busChoice(buses, assetBus);
	bool changed = false;
	ImGui::BeginDisabled(!enabled);
	if (choice.missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.38f, 1.0f));
	const bool open = ImGui::BeginCombo("##audiobus", choice.preview.c_str());
	if (choice.missing) ImGui::PopStyleColor();
	EditorWidgets::helpForKey("Audio Editor/Bus");
	if (open)
	{
		auto item = [&](const char* label, const std::string& name) {
			const bool sel = name == assetBus;
			if (ImGui::Selectable(label, sel) && !sel)
			{
				assetBus = name;
				changed  = true;
			}
			if (sel) ImGui::SetItemDefaultFocus();
		};
		item("Master", std::string());
		if (buses)
			for (const HE::AudioBusDef& b : buses->buses)
				item(b.name.c_str(), b.name);
		// The name the asset still carries, so keeping it is a choice too.
		if (choice.missing)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.38f, 1.0f));
			item(choice.preview.c_str(), assetBus);
			ImGui::PopStyleColor();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	return changed;
}

// ── EQ graph mapping ─────────────────────────────────────────────────────────

float xOfFreq(double hz, float width)
{
	const double t = std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
	return static_cast<float>(t) * width;
}

double freqOfX(float x, float width)
{
	const double t = width > 0.0f ? std::clamp(double(x) / double(width), 0.0, 1.0) : 0.0;
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float yOfDb(double db, float height)
{
	const double t = (kRangeDb - std::clamp(db, -kRangeDb, kRangeDb)) / (2.0 * kRangeDb);
	return static_cast<float>(t) * height;
}

double dbOfY(float y, float height)
{
	const double t = height > 0.0f ? std::clamp(double(y) / double(height), 0.0, 1.0) : 0.5;
	return kRangeDb - t * 2.0 * kRangeDb;
}

HE::AudioEqBand newBand(const HE::AudioEq& eq)
{
	// The decades a mix is usually shaped at, in the order a person would reach
	// for them; the first one no band sits within a third of an octave of.
	static constexpr float kSpots[] = { 1000.0f, 120.0f, 4000.0f, 400.0f, 10000.0f, 60.0f, 2000.0f, 250.0f };
	HE::AudioEqBand b;
	b.type = HE::AudioEqBandType::Peak;
	b.q    = 1.0f;
	b.freqHz = kSpots[0];
	for (const float f : kSpots)
	{
		bool taken = false;
		for (const HE::AudioEqBand& o : eq.bands)
			if (std::fabs(std::log2(double(o.freqHz) / double(f))) < 1.0 / 3.0) { taken = true; break; }
		if (!taken) { b.freqHz = f; break; }
	}
	return b;
}

// ── EQ block ─────────────────────────────────────────────────────────────────

namespace
{
	const ImU32 kBg       = IM_COL32(20, 21, 25, 255);
	const ImU32 kGrid     = IM_COL32(255, 255, 255, 16);
	const ImU32 kGridZero = IM_COL32(255, 255, 255, 46);
	const ImU32 kLabel    = IM_COL32(170, 175, 185, 255);
	const ImU32 kCurve    = IM_COL32(120, 222, 160, 240);
	const ImU32 kCurveOff = IM_COL32(120, 222, 160, 90);    // bypassed: what it WOULD do
	const ImU32 kBandSel  = IM_COL32(120, 222, 160, 70);    // the selected band's own share
	const ImU32 kFill     = IM_COL32(120, 222, 160, 28);
	const ImU32 kHandle   = IM_COL32(120, 222, 160, 255);
	const ImU32 kHandleHi = IM_COL32(235, 255, 240, 255);
	const ImU32 kHandleOff= IM_COL32(140, 140, 150, 200);
	const ImU32 kNyquist  = IM_COL32(0, 0, 0, 120);

	bool isGainBand(HE::AudioEqBandType t)
	{
		return t == HE::AudioEqBandType::Peak || t == HE::AudioEqBandType::LowShelf ||
		       t == HE::AudioEqBandType::HighShelf;
	}

	const char* const kTypeNames[] = { "Bell", "Low Shelf", "High Shelf", "Low Pass", "High Pass" };

	void freqLabel(char* out, size_t n, double hz)
	{
		if (hz >= 1000.0) std::snprintf(out, n, hz >= 10000.0 ? "%.0fk" : "%.3gk", hz / 1000.0);
		else              std::snprintf(out, n, "%.0f", hz);
	}

	// The graph. Returns what it did to the EQ.
	EqResult drawGraph(HE::AudioEq& eq, EqView& v, double rate, const ImVec2& size, bool enabled)
	{
		EqResult r;
		ImDrawList*  dl = ImGui::GetWindowDrawList();
		const ImVec2 o  = ImGui::GetCursorScreenPos();
		const float  w  = std::max(64.0f, size.x);
		const float  h  = std::max(48.0f, size.y);
		ImGui::InvisibleButton("##eqgraph", ImVec2(w, h),
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		EditorWidgets::helpForKey("Audio Editor/EQ Graph");
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 br(o.x + w, o.y + h);
		r.graphMin  = o;
		r.graphSize = ImVec2(w, h);

		dl->AddRectFilled(o, br, kBg);
		dl->PushClipRect(o, br, true);

		// Grid: 1-2-5 per decade, labelled at the round ones; dB every 6.
		for (double dec = 10.0; dec <= 10000.0; dec *= 10.0)
			for (const double m : { 1.0, 2.0, 5.0 })
			{
				const double f = dec * m;
				if (f < kMinHz || f > kMaxHz) continue;
				const float x = o.x + xOfFreq(f, w);
				dl->AddLine(ImVec2(x, o.y), ImVec2(x, br.y), kGrid);
				char lbl[16];
				freqLabel(lbl, sizeof(lbl), f);
				dl->AddText(ImVec2(x + 3.0f, br.y - ImGui::GetTextLineHeight() - 2.0f), kLabel, lbl);
			}
		for (int db = -12; db <= 12; db += 6)
		{
			const float y = o.y + yOfDb(db, h);
			dl->AddLine(ImVec2(o.x, y), ImVec2(br.x, y), db == 0 ? kGridZero : kGrid);
			char lbl[16];
			std::snprintf(lbl, sizeof(lbl), db == 0 ? "0 dB" : "%+d", db);
			dl->AddText(ImVec2(o.x + 4.0f, y - ImGui::GetTextLineHeight() - 1.0f), kLabel, lbl);
		}

		// Above the clip's Nyquist nothing can be shaped (biquadCoefficients clamps
		// there): shade it rather than draw a curve that promises otherwise.
		if (rate > 0.0 && 0.5 * rate < kMaxHz)
			dl->AddRectFilled(ImVec2(o.x + xOfFreq(0.5 * rate, w), o.y), br, kNyquist);

		// The summed response, one point per pixel column, from coefficients
		// computed once per frame — the same ones the engine runs.
		std::vector<HE::BiquadCoeffs> coeffs;
		coeffs.reserve(eq.bands.size());
		for (const HE::AudioEqBand& b : eq.bands) coeffs.push_back(HE::biquadCoefficients(b, rate));
		auto sumAt = [&](double f) {
			double db = 0.0;
			for (const HE::BiquadCoeffs& c : coeffs)
				if (!c.isIdentity()) db += c.magnitudeDb(f, rate);
			return db;
		};
		const int cols = static_cast<int>(w);
		std::vector<ImVec2> line;
		line.reserve(static_cast<size_t>(cols) + 1);
		const float zeroY = o.y + yOfDb(0.0, h);
		for (int i = 0; i <= cols; ++i)
		{
			const double f = freqOfX(float(i), w);
			line.emplace_back(o.x + float(i), o.y + yOfDb(sumAt(f), h));
		}
		// The area between the curve and 0 dB, one pixel column at a time. Plain
		// rects: they carry no anti-aliased fringe, so neighbouring columns do
		// not overlap into stripes the way thin quads did.
		if (eq.enabled)
			for (size_t i = 0; i + 1 < line.size(); ++i)
				dl->AddRectFilled(ImVec2(line[i].x, std::min(zeroY, line[i].y)),
				                  ImVec2(line[i].x + 1.0f, std::max(zeroY, line[i].y)), kFill);
		// The selected band's own share, so a band among several can be read.
		if (v.selBand >= 0 && v.selBand < static_cast<int>(coeffs.size()) && !coeffs[size_t(v.selBand)].isIdentity())
		{
			std::vector<ImVec2> own;
			own.reserve(line.size());
			for (int i = 0; i <= cols; ++i)
				own.emplace_back(o.x + float(i),
				                 o.y + yOfDb(coeffs[size_t(v.selBand)].magnitudeDb(freqOfX(float(i), w), rate), h));
			dl->AddPolyline(own.data(), static_cast<int>(own.size()), kBandSel, ImDrawFlags_None, 1.5f);
		}
		dl->AddPolyline(line.data(), static_cast<int>(line.size()), eq.enabled ? kCurve : kCurveOff,
		                ImDrawFlags_None, 2.0f);

		// Handles: at a band's frequency and gain; a pass filter has no gain, so
		// it sits on the 0 dB line and only moves sideways.
		auto handleAt = [&](const HE::AudioEqBand& b) {
			return ImVec2(o.x + xOfFreq(b.freqHz, w),
			              isGainBand(b.type) ? o.y + yOfDb(b.gainDb, h) : zeroY);
		};
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int under = -1;
		float best = 9.0f * 9.0f;
		for (size_t i = 0; i < eq.bands.size(); ++i)
		{
			const ImVec2 p = handleAt(eq.bands[i]);
			const float  d = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
			if (d < best) { best = d; under = static_cast<int>(i); }
		}
		for (size_t i = 0; i < eq.bands.size(); ++i)
		{
			const HE::AudioEqBand& b = eq.bands[i];
			const ImVec2 p   = handleAt(b);
			const bool   sel = static_cast<int>(i) == v.selBand;
			const ImU32  col = !b.enabled || !eq.enabled ? kHandleOff : sel ? kHandleHi : kHandle;
			dl->AddCircleFilled(p, sel ? 6.0f : 5.0f, col);
			if (sel) dl->AddCircle(p, 9.0f, kHandleHi, 0, 1.5f);
			char num[4];
			std::snprintf(num, sizeof(num), "%d", static_cast<int>(i) + 1);
			dl->AddText(ImVec2(p.x + 7.0f, p.y - ImGui::GetTextLineHeight() - 2.0f), col, num);
		}
		dl->PopClipRect();

		if (!enabled) return r;

		// Press on a handle: select it and drag it. Press on nothing: deselect.
		if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			v.selBand   = under;
			v.dragBand  = under;
			v.dragMoved = false;
		}
		if (v.dragBand >= 0 && v.dragBand < static_cast<int>(eq.bands.size()))
		{
			if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				HE::AudioEqBand& b = eq.bands[size_t(v.dragBand)];
				const float nf = static_cast<float>(std::clamp(freqOfX(mouse.x - o.x, w),
				                                               double(HE::AudioEqBand::kMinFreqHz),
				                                               double(HE::AudioEqBand::kMaxFreqHz)));
				const float ng = isGainBand(b.type)
					? static_cast<float>(std::round(dbOfY(mouse.y - o.y, h) * 10.0) / 10.0) : b.gainDb;
				if (nf != b.freqHz || ng != b.gainDb)
				{
					b.freqHz    = nf;
					b.gainDb    = ng;
					r.edited    = true;
					v.dragMoved = true;
				}
			}
			if (ImGui::IsItemDeactivated())
			{
				r.committed = v.dragMoved;   // a click that only selected is no edit
				v.dragBand  = -1;
				v.dragMoved = false;
			}
		}
		else if (ImGui::IsItemDeactivated())
			v.dragBand = -1;

		// Wheel over a handle: Q, a notch at a time. Each notch is a finished edit.
		if (hovered && under >= 0 && ImGui::GetIO().MouseWheel != 0.0f)
		{
			HE::AudioEqBand& b = eq.bands[size_t(under)];
			const float q = std::clamp(b.q * std::pow(1.15f, ImGui::GetIO().MouseWheel),
			                           HE::AudioEqBand::kMinQ, HE::AudioEqBand::kMaxQ);
			if (q != b.q) { b.q = q; r.edited = r.committed = true; }
			v.selBand = under;
		}
		// Double-click the empty graph: a bell exactly there.
		if (hovered && under < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
		    eq.bands.size() < HE::AudioEq::kMaxBands)
		{
			HE::AudioEqBand b;
			b.type   = HE::AudioEqBandType::Peak;
			b.q      = 1.0f;
			b.freqHz = static_cast<float>(freqOfX(mouse.x - o.x, w));
			b.gainDb = static_cast<float>(std::round(dbOfY(mouse.y - o.y, h) * 10.0) / 10.0);
			eq.bands.push_back(b);
			v.selBand  = static_cast<int>(eq.bands.size()) - 1;
			v.dragBand = -1;
			r.edited = r.committed = true;
		}
		// Right-click a handle: remove that band.
		if (hovered && under >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			eq.bands.erase(eq.bands.begin() + under);
			v.selBand = v.dragBand = -1;
			r.edited = r.committed = true;
		}
		return r;
	}

	// One row of the band list.
	EqResult drawBandRow(HE::AudioEq& eq, size_t i, EqView& v, bool& remove)
	{
		EqResult r;
		HE::AudioEqBand& b = eq.bands[i];
		ImGui::PushID(static_cast<int>(i));
		const bool sel = static_cast<int>(i) == v.selBand;
		char num[8];
		std::snprintf(num, sizeof(num), "%zu", i + 1);
		if (ImGui::Selectable(num, sel, ImGuiSelectableFlags_None, ImVec2(ImGui::GetFontSize() * 1.2f, 0.0f)))
			v.selBand = static_cast<int>(i);
		ImGui::SameLine();
		if (ImGui::Checkbox("##on", &b.enabled)) r.edited = r.committed = true;
		EditorWidgets::helpForKey("Audio Editor/Band On");
		ImGui::SameLine();

		const float fs = ImGui::GetFontSize();
		ImGui::SetNextItemWidth(fs * 6.0f);
		int type = static_cast<int>(b.type);
		if (ImGui::Combo("##type", &type, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
		{
			b.type = static_cast<HE::AudioEqBandType>(type);
			r.edited = r.committed = true;
		}
		EditorWidgets::helpForKey("Audio Editor/Band Type");
		auto drag = [&](const char* id, float* val, float speed, float lo, float hi, const char* fmt,
		                ImGuiSliderFlags flags, const char* helpKey, float width) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(width);
			if (ImGui::DragFloat(id, val, speed, lo, hi, fmt, flags | ImGuiSliderFlags_AlwaysClamp))
			{
				r.edited  = true;
				v.selBand = static_cast<int>(i);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) r.committed = true;
			EditorWidgets::helpForKey(helpKey);
		};
		drag("##freq", &b.freqHz, 2.0f, HE::AudioEqBand::kMinFreqHz, HE::AudioEqBand::kMaxFreqHz, "%.0f Hz",
		     ImGuiSliderFlags_Logarithmic, "Audio Editor/Band Frequency", fs * 5.0f);
		ImGui::BeginDisabled(!isGainBand(b.type));
		drag("##gain", &b.gainDb, 0.1f, HE::AudioEqBand::kMinGainDb, HE::AudioEqBand::kMaxGainDb, "%+.1f dB",
		     ImGuiSliderFlags_None, "Audio Editor/Band Gain", fs * 4.5f);
		ImGui::EndDisabled();
		drag("##q", &b.q, 0.01f, HE::AudioEqBand::kMinQ, HE::AudioEqBand::kMaxQ, "Q %.2f",
		     ImGuiSliderFlags_Logarithmic, "Audio Editor/Band Q", fs * 4.0f);
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove")) remove = true;
		EditorWidgets::helpForKey("Audio Editor/Remove Band");
		ImGui::PopID();
		return r;
	}
}

EqResult drawEq(HE::AudioEq& eq, EqView& v, double sampleRate, const ImVec2& size, bool enabled,
                bool forBus)
{
	EqResult r;
	auto merge = [&r](const EqResult& o) {
		r.edited |= o.edited; r.committed |= o.committed;
		if (o.graphSize.x > 0.0f) { r.graphMin = o.graphMin; r.graphSize = o.graphSize; }
	};
	if (v.selBand >= static_cast<int>(eq.bands.size())) v.selBand = -1;

	ImGui::BeginDisabled(!enabled);
	// The bypass: the bands keep their settings, nothing runs. "On" rather than
	// "Bypass" so the box is ticked when the EQ is heard, like a band's own.
	if (ImGui::Checkbox("EQ On", &eq.enabled)) r.edited = r.committed = true;
	EditorWidgets::helpForKey("Audio Editor/EQ On");
	ImGui::SameLine();
	ImGui::BeginDisabled(eq.bands.size() >= HE::AudioEq::kMaxBands);
	if (ImGui::Button("Add Band"))
	{
		eq.bands.push_back(newBand(eq));
		v.selBand = static_cast<int>(eq.bands.size()) - 1;
		r.edited = r.committed = true;
	}
	EditorWidgets::helpForKey("Audio Editor/Add Band");
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (eq.bands.empty())
		ImGui::TextDisabled(forBus ? "No bands: the bus plays unfiltered. Add Band, or double-click the graph."
		                           : "No bands: the clip plays unfiltered. Add Band, or double-click the graph.");
	else if (!eq.enabled)
		ImGui::TextDisabled("Bypassed: the bands are kept, nothing is filtered.");
	else if (eq.isNeutral())
		ImGui::TextDisabled("Every band is flat: nothing is filtered yet.");
	else
		ImGui::TextDisabled(forBus ? "On everything this bus plays, in the editor and in the game."
		                           : "Heard in this preview and wherever the game plays the clip.");
	ImGui::EndDisabled();

	// Graph beside the band list when there is room for both, above it when not.
	// One band row, measured from what drawBandRow puts in it: number, check
	// box, type, frequency, gain, Q, Remove, the spacing between them and room
	// for the child's scrollbar.
	const ImGuiStyle& style = ImGui::GetStyle();
	const float rowW = ImGui::GetFontSize() * (1.2f + 6.0f + 5.0f + 4.5f + 4.0f) + ImGui::GetFrameHeight() +
	                   ImGui::CalcTextSize("Remove").x + style.FramePadding.x * 2.0f +
	                   style.ItemSpacing.x * 7.0f + style.ScrollbarSize;
	const ImVec2 avail(size.x, std::max(48.0f, size.y - ImGui::GetFrameHeightWithSpacing()));
	const bool   beside = avail.x > rowW + 320.0f;
	const ImVec2 graph  = beside ? ImVec2(avail.x - rowW - ImGui::GetStyle().ItemSpacing.x, avail.y)
	                             : ImVec2(avail.x, std::max(48.0f, avail.y * 0.55f));
	merge(drawGraph(eq, v, sampleRate, graph, enabled));
	if (beside) ImGui::SameLine();

	ImGui::BeginDisabled(!enabled);
	ImGui::BeginChild("##eqbands", beside ? ImVec2(rowW, avail.y) : ImVec2(avail.x, avail.y - graph.y), false);
	int removeAt = -1;
	for (size_t i = 0; i < eq.bands.size(); ++i)
	{
		bool remove = false;
		merge(drawBandRow(eq, i, v, remove));
		if (remove) removeAt = static_cast<int>(i);
	}
	if (removeAt >= 0)
	{
		eq.bands.erase(eq.bands.begin() + removeAt);
		v.selBand = v.dragBand = -1;
		r.edited = r.committed = true;
	}
	if (sampleRate > 0.0 && 0.5 * sampleRate < kMaxHz)
		ImGui::TextDisabled("Shaded: above %.1f kHz, this clip's Nyquist — nothing to shape there.",
		                    0.5 * sampleRate / 1000.0);
	ImGui::EndChild();
	ImGui::EndDisabled();
	return r;
}

} // namespace HE::Ed::AudioMix
