#include "DocsPanel.h"

#include "DocsLibrary.h"
#include "EditorHelp.h"          // topic → the panel it is about ("Show me")
#include "HcNodeReference.h"     // the node reference, generated from the engine
#include "EditorReference.h"     // the editor reference, generated from the tooltips
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "PanelSpotlight.h"

// Only the AppContext adapter at the bottom needs this, and it is the one part
// of the file that belongs to the editor build alone: AppContext's own layout
// depends on HE_IMGUI_ENABLED, so a translation unit that does not define it
// must not see the panel through that type.
#ifdef HE_IMGUI_ENABLED
#include "EditorApplication.h"
#endif

#include <Diagnostics/Log.h>
#include <Renderer/IRenderer.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Gated on the HEADER rather than on HE_IMGUI_ENABLED, like EditorTheme and
// EditorDockState: everything below is ImGui plus the docs library, so it
// compiles wherever ImGui does — which is what lets the test target render the
// reader and look at it (tests/test_ui_shot.cpp).
#if __has_include(<imgui.h>)
#define HE_DOCS_PANEL_IMPL 1
#include <imgui.h>
#include "vendor/stb_image.h"
#endif

namespace docs = HE::Ed::Docs;

namespace DocsPanel
{
namespace
{
#ifdef HE_DOCS_PANEL_IMPL

	// ── What the reader was handed, resolved ─────────────────────────────────
	// DocsPanel::Host keeps its font pointers opaque so the header stays
	// ImGui-free; this is the same thing with types, and it is what every
	// drawing function below takes.
	struct Ctx
	{
		ImFont*    body = nullptr;
		ImFont*    sub  = nullptr;
		ImFont*    head = nullptr;
		ImFont*    code = nullptr;
		IRenderer* renderer = nullptr;
	};

	// ── State ────────────────────────────────────────────────────────────────
	bool  s_open        = false;
	bool  s_loadTried   = false;
	int   s_page        = 0;
	int   s_section     = -1;
	int   s_scrollTo    = -1;    // section to scroll to on the next draw
	bool  s_focusSearch = false;
	char  s_query[128]  = "";
	// How many more frames to keep asking for the scroll. One is not enough: on
	// the frame a page opens, nothing above the target has been laid out at its
	// final height yet (fonts, wrapped paragraphs, tables), so the offset ImGui
	// computes lands near the section rather than on it.
	int   s_scrollFrames = 0;
	// Frames left in which the sidebar may still force its groups open — the
	// same "we just navigated" window as the scroll, kept separate because the
	// scroll stops as soon as it has landed and the sidebar has to survive the
	// frame the user clicks a group in.
	int   s_navFrames = 0;
	// The ImGui frame in which the reader was last opened. See
	// DocsPanel::openedThisFrame() — F1 has two handlers in one frame.
	int   s_openedFrame = -1;

	// Where the reader has been, so Back means what it does in a browser. Pairs
	// of (page, section); -1 for "the top of the page".
	std::vector<std::pair<int, int>> s_history;
	int s_historyPos = -1;

	PanelOpener s_panelOpener = nullptr;
	// The panel a "Show me" is currently pointing at, and until when.
	const char* s_spotlightWindow = nullptr;
	double      s_spotlightUntil  = 0.0;

	constexpr const char* kWindowTitle = "Documentation";
	// Every figure in the manual is a screenshot at 1280 px; drawn at its full
	// width the text in it is smaller than the text around it and the panel
	// scrolls sideways. This is the cap in editor pixels.
	constexpr float kFigureMaxWidth = 720.0f;

	// ── Figures ──────────────────────────────────────────────────────────────
	// Loaded the first time a page that uses one is drawn, never before: opening
	// the manual must not cost eight texture uploads for pictures on pages the
	// reader may not visit. Failures are cached as a null handle so a missing
	// file is not re-read every frame.
	struct Figure { ImTextureID tex = 0; int w = 0, h = 0; bool tried = false; };
	std::unordered_map<std::string, Figure> s_figures;

	const Figure& figure(const Ctx& ctx, const std::string& file)
	{
		Figure& f = s_figures[file];
		if (f.tried) return f;
		f.tried = true;
		if (!ctx.renderer) return f;

		const char* base = SDL_GetBasePath();
		const std::string path = docs::imageDir(base ? base : "") + file;
		int w = 0, h = 0, ch = 0;
		if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4))
		{
			if (void* handle = ctx.renderer->CreateImGuiTexture(px, w, h))
			{
				f.tex = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle));
				f.w = w;
				f.h = h;
			}
			stbi_image_free(px);
		}
		else
		{
			HE_LOG_WARN(Editor, "%s", ("DocsPanel: figure not found — " + path).c_str());
		}
		return f;
	}

	// ── Navigation ───────────────────────────────────────────────────────────
	void go(int page, int section, bool record = true)
	{
		const docs::Library& lib = docs::library();
		if (page < 0 || page >= static_cast<int>(lib.pages().size())) return;
		if (record && (page != s_page || section != s_section))
		{
			// A new destination truncates the forward history, exactly as a
			// browser does — the path not taken is gone.
			if (s_historyPos >= 0 &&
			    s_historyPos + 1 < static_cast<int>(s_history.size()))
				s_history.resize(static_cast<std::size_t>(s_historyPos) + 1);
			s_history.emplace_back(page, section);
			s_historyPos = static_cast<int>(s_history.size()) - 1;
		}
		s_page        = page;
		s_section     = section;
		s_scrollTo    = section;
		s_scrollFrames = 3;
		s_navFrames    = 3;
	}

	void goTopic(const char* topic)
	{
		int page = -1, section = -1;
		if (docs::library().resolve(topic ? topic : "", page, section))
			go(page, section);
	}

	// A link inside the manual: another topic, or a URL for anything off-site.
	void followLink(const std::string& href)
	{
		if (href.empty()) return;
		if (href.rfind("http", 0) == 0 || href.rfind("mailto:", 0) == 0)
		{
			SDL_OpenURL(href.c_str());
			return;
		}
		int page = -1, section = -1;
		if (docs::library().resolve(href, page, section)) go(page, section);
	}

	void ensureLoaded()
	{
		// Already loaded by someone else — a test that pointed the library at the
		// bundle in the source tree, or a second reader — is not a reason to read
		// it again, and re-reading would REPLACE a good copy with whatever this
		// process can find next to its own executable.
		if (s_loadTried || docs::library().loaded()) { s_loadTried = true; return; }
		s_loadTried = true;
		const char* base = SDL_GetBasePath();
		docs::Library& lib = docs::library();
		if (!lib.load(docs::bundlePath(base ? base : "")))
			HE_LOG_WARN(Editor, "%s", ("DocsPanel: " + lib.error()).c_str());
		// The node reference is not written, it is built — from the engine's own
		// registries, so it can neither miss a call nor list one that is gone.
		// It takes the page id the website's hand-written version had, which is
		// what keeps every cross-link and F1 anchor pointing at it resolving.
		if (lib.loaded())
		{
			HE::Ed::NodeReference::install(lib);
			// And the editor's own controls, from the same table the hover
			// tooltips come from — so F1 on a control opens the control.
			HE::Ed::EditorReference::install(lib);
		}
	}

	// ── Inline text ──────────────────────────────────────────────────────────
	// ImGui wraps a STRING; a paragraph here is a sequence of differently styled
	// runs, and there is no call that wraps across them. So the runs are broken
	// into words once and placed by hand: draw a word, then ask whether the next
	// one still fits before putting it on the same line.
	//
	// Deciding AFTER the item rather than before is what makes this simple —
	// GetItemRectMax() is the true right edge of what was just drawn, including
	// the padding a code span adds, which a CalcTextSize before the fact is not.
	struct Word
	{
		std::string       text;
		docs::Style       style = docs::Style::Body;
		const std::string* href = nullptr;
		bool              spaceBefore = false;
		bool              breakBefore = false;   // a hard newline from the source
	};

	std::vector<Word> splitWords(const std::vector<docs::Run>& runs)
	{
		std::vector<Word> out;
		bool pendingSpace = false;
		bool pendingBreak = false;
		for (const docs::Run& r : runs)
		{
			std::size_t i = 0;
			while (i < r.text.size())
			{
				const char c = r.text[i];
				if (c == '\n') { pendingBreak = true; pendingSpace = false; ++i; continue; }
				if (c == ' ' || c == '\t') { pendingSpace = true; ++i; continue; }
				std::size_t end = i;
				while (end < r.text.size() && r.text[end] != ' ' &&
				       r.text[end] != '\t' && r.text[end] != '\n') ++end;
				Word w;
				w.text        = r.text.substr(i, end - i);
				w.style       = r.style;
				w.href        = r.href.empty() ? nullptr : &r.href;
				w.spaceBefore = pendingSpace && !out.empty();
				w.breakBefore = pendingBreak;
				pendingSpace = pendingBreak = false;
				out.push_back(std::move(w));
				i = end;
			}
		}
		return out;
	}

	ImVec4 colorFor(docs::Style s)
	{
		using namespace HE::Ed::Theme;
		switch (s)
		{
		// Bold and code both carry emphasis, and the editor's only font is
		// already a bold weight — so emphasis is COLOUR here, not another face.
		// Bold takes the heading gold; code the brighter gold plus a plate, so
		// an API name still reads as a token rather than as a stressed word.
		case docs::Style::Bold:   return TextHeading;
		case docs::Style::Italic: return mix(Text, TextDim, 0.35f);
		case docs::Style::Code:   return AccentBright;
		case docs::Style::Link:   return AccentHi;
		default:                  return Text;
		}
	}

	void drawRuns(const Ctx& ctx, const std::vector<docs::Run>& runs)
	{
		const std::vector<Word> words = splitWords(runs);
		if (words.empty()) { ImGui::NewLine(); return; }

		const float rightEdge =
			ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		const float spaceW = ImGui::CalcTextSize(" ").x;

		// Words are placed with SameLine(0, gap), so the style's item spacing must
		// not add a second gap of its own — and the VERTICAL half is what sets the
		// leading between the wrapped lines of a paragraph. Tight, because these
		// are lines of one paragraph rather than separate controls.
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
		                    ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.25f));
		ImDrawList* dl = ImGui::GetWindowDrawList();

		bool lineStart = true;
		for (std::size_t i = 0; i < words.size(); ++i)
		{
			const Word& w = words[i];
			// A hard break needs nothing done to it: an item that was NOT put on
			// the previous line with SameLine already starts a new one. Calling
			// NewLine() here would leave a blank line behind — which is exactly
			// what wrapping did until the first screenshot of this panel showed
			// paragraphs at double leading (tests/test_ui_shot.cpp).
			if (w.breakBefore) lineStart = true;

			const bool isCode = w.style == docs::Style::Code;
			if (isCode && ctx.code) ImGui::PushFont(ctx.code, 0.0f);

			const float gap = (lineStart || !w.spaceBefore) ? 0.0f : spaceW;
			const ImVec2 size = ImGui::CalcTextSize(w.text.c_str());
			const float  pad  = isCode ? 3.0f : 0.0f;
			// Keep the word on this line only if it still fits. Otherwise simply
			// do not call SameLine and let it fall to the next one — a single word
			// wider than the column then overflows rather than looping forever on
			// an impossible fit.
			if (!lineStart)
			{
				const float x = ImGui::GetItemRectMax().x + gap;
				if (x + size.x + pad * 2.0f > rightEdge) lineStart = true;
				else                                     ImGui::SameLine(0.0f, gap);
			}

			if (isCode)
			{
				const ImVec2 p = ImGui::GetCursorScreenPos();
				dl->AddRectFilled(ImVec2(p.x - pad, p.y - 1.0f),
				                  ImVec2(p.x + size.x + pad, p.y + size.y + 1.0f),
				                  ImGui::GetColorU32(HE::Ed::Theme::warm(0.19f)), 3.0f);
			}

			ImGui::PushStyleColor(ImGuiCol_Text, colorFor(w.style));
			ImGui::TextUnformatted(w.text.c_str());
			ImGui::PopStyleColor();
			if (isCode && ctx.code) ImGui::PopFont();
			lineStart = false;

			if (w.href)
			{
				const ImVec2 mn = ImGui::GetItemRectMin();
				const ImVec2 mx = ImGui::GetItemRectMax();
				const bool hovered = ImGui::IsItemHovered();
				dl->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f),
				            ImGui::GetColorU32(colorFor(docs::Style::Link)),
				            hovered ? 1.4f : 1.0f);
				if (hovered)
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						// Copied before navigating: `w.href` points into the runs
						// of the page being drawn, and following a link is the one
						// call here that can decide to load a different bundle.
						const std::string target = *w.href;
						ImGui::PopStyleVar();
						followLink(target);
						return;
					}
				}
			}
		}
		ImGui::PopStyleVar();
	}

	// ── Blocks ───────────────────────────────────────────────────────────────
	void drawBlock(const Ctx& ctx, const docs::Block& b);

	void drawCells(const Ctx& ctx, const docs::Cell& cell) { drawRuns(ctx, cell); }

	void drawTable(const Ctx& ctx, const docs::Block& b)
	{
		const int cols = static_cast<int>(
			!b.head.empty() ? b.head.size()
			                : (b.rows.empty() ? 0 : b.rows.front().size()));
		if (cols <= 0) return;

		ImGui::Spacing();
		// Borders and a striped body: these are reference tables read a row at a
		// time, and a row that cannot be followed across is the one thing worse
		// than no table.
		const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		                              ImGuiTableFlags_SizingStretchProp |
		                              ImGuiTableFlags_NoHostExtendX;
		if (!ImGui::BeginTable("##docsTable", cols, flags)) return;

		// ── Column widths, from what is actually in them ─────────────────────
		// A fixed "narrow first column, wide rest" split is right for the
		// two-column term/explanation tables and catastrophic for the others:
		// the backend support matrix is one term column and five columns of a
		// single bullet, and an even share squeezed the terms into one character
		// per line while the bullets sat in a hundred pixels of nothing.
		//
		// So each column is weighted by the longest thing in it, clamped at both
		// ends — under the floor a column of bullets would vanish, over the
		// ceiling one long sentence would starve everything else.
		{
			std::vector<float> weight(static_cast<std::size_t>(cols), 0.0f);
			auto measure = [&](const docs::Cells& row) {
				for (int c = 0; c < cols && c < static_cast<int>(row.size()); ++c)
				{
					std::size_t n = 0;
					for (const docs::Run& r : row[static_cast<std::size_t>(c)]) n += r.text.size();
					weight[static_cast<std::size_t>(c)] =
						std::max(weight[static_cast<std::size_t>(c)], static_cast<float>(n));
				}
			};
			if (!b.head.empty()) measure(b.head);
			for (const docs::Cells& row : b.rows) measure(row);
			for (float& w : weight) w = std::clamp(w, 6.0f, 44.0f);
			for (int c = 0; c < cols; ++c)
				ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthStretch,
				                        weight[static_cast<std::size_t>(c)]);
		}

		if (!b.head.empty())
		{
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			for (int c = 0; c < cols; ++c)
			{
				ImGui::TableSetColumnIndex(c);
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
				if (c < static_cast<int>(b.head.size())) drawCells(ctx, b.head[c]);
				ImGui::PopStyleColor();
			}
		}
		for (const docs::Cells& row : b.rows)
		{
			ImGui::TableNextRow();
			for (int c = 0; c < cols; ++c)
			{
				ImGui::TableSetColumnIndex(c);
				if (c < static_cast<int>(row.size())) drawCells(ctx, row[c]);
			}
		}
		ImGui::EndTable();
		ImGui::Spacing();
	}

	void drawCode(const Ctx& ctx, const docs::Block& b)
	{
		ImGui::Spacing();
		if (!b.title.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::TextUnformatted(b.title.c_str());
			ImGui::PopStyleColor();
		}

		// Sized to the listing, capped: a forty-line example must not push the
		// paragraph after it off the bottom of the panel.
		int lines = 1;
		for (char c : b.text) if (c == '\n') ++lines;
		if (ctx.code) ImGui::PushFont(ctx.code, 0.0f);
		const float h = std::min(ImGui::GetTextLineHeightWithSpacing() * (lines + 1) + 8.0f,
		                         340.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.075f));
		ImGui::BeginChild("##code", ImVec2(0.0f, h), ImGuiChildFlags_Borders,
		                  ImGuiWindowFlags_HorizontalScrollbar);
		{
			// No WrapText guard on purpose: code is the one thing that must NOT
			// wrap — a broken line changes what the example says. It scrolls
			// horizontally instead.
			ImGui::TextUnformatted(b.text.c_str());
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		if (ctx.code) ImGui::PopFont();

		if (ImGui::SmallButton("Copy##code")) ImGui::SetClipboardText(b.text.c_str());
		ImGui::Spacing();
	}

	void drawCallout(const Ctx& ctx, const docs::Block& b)
	{
		using namespace HE::Ed::Theme;
		const ImVec4 accent = b.tone == docs::Tone::Warning ? ImVec4(0.90f, 0.55f, 0.30f, 1.0f)
		                    : b.tone == docs::Tone::Tip     ? ImVec4(0.55f, 0.80f, 0.55f, 1.0f)
		                                                    : AccentHi;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, warm(0.135f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
		ImGui::BeginChild("##callout", ImVec2(0.0f, 0.0f),
		                  ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		{
			for (const docs::Block& inner : b.blocks) drawBlock(ctx, inner);
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		// The tone bar, painted over the child's left edge once its rect is
		// known — which is only after EndChild.
		const ImVec2 mn = ImGui::GetItemRectMin();
		const ImVec2 mx = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y),
		                                          ImGui::GetColorU32(accent), 2.0f);
		ImGui::Spacing();
	}

	void drawFlow(const Ctx& ctx, const docs::Block& b)
	{
		// The website draws these as a chain of boxes with arrows. Written out as
		// a numbered list — which is what this was at first — a pipeline stops
		// looking like a pipeline: the reader gets eight paragraphs and has to
		// reassemble the fact that each one feeds the next.
		//
		// So it stays a diagram, turned to run DOWN the column instead of across
		// it. A docked panel has height and no width; the arrows are the same
		// arrows either way.
		ImGui::Spacing();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float arrowH = ImGui::GetTextLineHeight() * 0.9f;

		for (std::size_t i = 0; i < b.steps.size(); ++i)
		{
			const docs::Block::Step& s = b.steps[i];
			if (i > 0)
			{
				// The connector: a short stem and a filled head, centred under
				// the box above. Drawn into the gap rather than as an item, so
				// the boxes stay a tight column.
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float cx = p.x + ImGui::GetContentRegionAvail().x * 0.5f;
				const ImU32 col = ImGui::GetColorU32(HE::Ed::Theme::TextDim);
				dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + arrowH * 0.55f), col, 1.5f);
				dl->AddTriangleFilled(ImVec2(cx - arrowH * 0.28f, p.y + arrowH * 0.5f),
				                      ImVec2(cx + arrowH * 0.28f, p.y + arrowH * 0.5f),
				                      ImVec2(cx, p.y + arrowH), col);
				ImGui::Dummy(ImVec2(0.0f, arrowH));
			}

			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, HE::Ed::Theme::warm(0.145f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));
			ImGui::BeginChild("##step", ImVec2(0.0f, 0.0f),
			                  ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
			{
				// The step number stays — in a chain of eight it is how someone
				// says "it goes wrong at four" — but as a quiet index, not as the
				// structure.
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
				ImGui::Text("%d", static_cast<int>(i) + 1);
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 10.0f);
				ImGui::BeginGroup();
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
				ImGui::TextUnformatted(s.label.c_str());
				ImGui::PopStyleColor();
				if (!s.sub.empty())
				{
					EditorWidgets::WrapText wrap;
					ImGui::TextUnformatted(s.sub.c_str());
				}
				ImGui::EndGroup();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
			// The accent edge, over the child's left border once its rect is
			// known — the same device the callouts use, so a diagram and a note
			// read as the same family of block.
			const ImVec2 mn = ImGui::GetItemRectMin();
			const ImVec2 mx = ImGui::GetItemRectMax();
			dl->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y),
			                  ImGui::GetColorU32(HE::Ed::Theme::AccentHi), 2.0f);
			ImGui::PopID();
		}
		ImGui::Spacing();
	}

	// ── A picture of the node ────────────────────────────────────────────────
	// The reference draws each entry's node the way the canvas draws it: a
	// coloured header with the name, inputs down the left with their pins on the
	// left edge, outputs down the right with theirs on the right. Not a
	// screenshot — a drawing from the same signature the real node has, so it
	// cannot show a node the engine has since changed, and costs no files.
	//
	// This is the answer to "what am I looking for in the palette": a name in a
	// list is a name, and this is the shape you will actually see.
	void drawNodePreview(const docs::Block& b)
	{
		const float line   = ImGui::GetTextLineHeight();
		const float pad    = 8.0f;
		const float rowH   = line + 4.0f;
		const ImU32 accent = b.accent ? b.accent
		                              : ImGui::GetColorU32(HE::Ed::Theme::warm(0.30f));

		int nIn = 0, nOut = 0;
		float wIn = 0.0f, wOut = 0.0f;
		for (const docs::PinRow& p : b.pins)
		{
			const std::string label = p.name.empty() ? p.type : p.name;
			const float w = ImGui::CalcTextSize(label.c_str()).x + line + 8.0f;
			if (p.isInput) { ++nIn;  wIn  = std::max(wIn,  w); }
			else           { ++nOut; wOut = std::max(wOut, w); }
		}

		const float titleW = ImGui::CalcTextSize(b.title.c_str()).x + pad * 2.0f;
		const float bodyW  = std::max({ titleW, wIn + wOut + pad * 3.0f, 160.0f });
		const float width  = std::min(bodyW, ImGui::GetContentRegionAvail().x);
		const float headH  = line + 8.0f;
		const float height = headH + std::max(nIn, nOut) * rowH + pad;

		ImGui::Spacing();
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const ImVec2 p1(p0.x + width, p0.y + height);
		ImDrawList* dl = ImGui::GetWindowDrawList();

		// Body, header, border — the node's own three parts.
		dl->AddRectFilled(p0, p1, ImGui::GetColorU32(HE::Ed::Theme::warm(0.115f)), 6.0f);
		dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + headH), accent, 6.0f,
		                  ImDrawFlags_RoundCornersTop);
		dl->AddRect(p0, p1, ImGui::GetColorU32(HE::Ed::Theme::warm(0.26f)), 6.0f, 0, 1.0f);
		dl->AddText(ImVec2(p0.x + pad, p0.y + 4.0f),
		            IM_COL32(240, 238, 232, 255), b.title.c_str());

		auto glyph = [&](ImVec2 c, const docs::PinRow& p) {
			const float r = line * 0.28f;
			if (p.isExec)
				dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r),
				                      ImVec2(c.x + r, c.y), p.color);
			else if (p.isContainer)
			{
				const float o = r * 0.55f, h = r * 0.42f;
				for (int gy = -1; gy <= 1; gy += 2)
					for (int gx = -1; gx <= 1; gx += 2)
						dl->AddRectFilled(ImVec2(c.x + gx * o - h, c.y + gy * o - h),
						                  ImVec2(c.x + gx * o + h, c.y + gy * o + h), p.color);
			}
			else
				dl->AddCircleFilled(c, r, p.color);
		};

		int iIn = 0, iOut = 0;
		for (const docs::PinRow& p : b.pins)
		{
			const std::string label = p.name.empty() ? p.type : p.name;
			const float y = p0.y + headH + (p.isInput ? iIn++ : iOut++) * rowH + rowH * 0.5f;
			const ImU32 text = ImGui::GetColorU32(HE::Ed::Theme::Text);
			if (p.isInput)
			{
				glyph(ImVec2(p0.x + pad, y), p);
				dl->AddText(ImVec2(p0.x + pad + line * 0.6f, y - line * 0.5f), text,
				            label.c_str());
			}
			else
			{
				const float tw = ImGui::CalcTextSize(label.c_str()).x;
				glyph(ImVec2(p1.x - pad, y), p);
				dl->AddText(ImVec2(p1.x - pad - line * 0.6f - tw, y - line * 0.5f), text,
				            label.c_str());
			}
		}

		// Reserve what was drawn, so the blocks after it start below.
		ImGui::Dummy(ImVec2(width, height));
		ImGui::Spacing();
	}

	// The same pins as a read-down list with their TYPES — the picture above
	// shows where a pin sits, this says what it carries.
	void drawPins(const docs::Block& b)
	{
		const float line = ImGui::GetTextLineHeight();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		auto glyph = [&](const docs::PinRow& p) {
			const float r  = line * 0.28f;
			const ImVec2 q = ImGui::GetCursorScreenPos();
			const ImVec2 c(q.x + line * 0.5f, q.y + line * 0.5f);
			if (p.isExec)
				dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r),
				                      ImVec2(c.x + r, c.y), p.color);
			else if (p.isContainer)
			{
				const float o = r * 0.55f, h = r * 0.42f;
				for (int gy = -1; gy <= 1; gy += 2)
					for (int gx = -1; gx <= 1; gx += 2)
						dl->AddRectFilled(ImVec2(c.x + gx * o - h, c.y + gy * o - h),
						                  ImVec2(c.x + gx * o + h, c.y + gy * o + h), p.color);
			}
			else
				dl->AddCircleFilled(c, r, p.color);
			ImGui::Dummy(ImVec2(line, line));
			ImGui::SameLine(0.0f, 6.0f);
		};
		auto column = [&](bool input) {
			bool any = false;
			for (const docs::PinRow& p : b.pins)
			{
				if (p.isInput != input) continue;
				if (!any)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
					ImGui::TextUnformatted(input ? "Inputs" : "Outputs");
					ImGui::PopStyleColor();
					any = true;
				}
				glyph(p);
				if (!p.name.empty()) { ImGui::TextUnformatted(p.name.c_str()); ImGui::SameLine(0.0f, 6.0f); }
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.color));
				ImGui::TextUnformatted(p.type.c_str());
				ImGui::PopStyleColor();
			}
			return any;
		};

		ImGui::Spacing();
		// Side by side while there is room for two columns; stacked in a narrow
		// reader, where a half-width column would wrap every type name.
		const bool wide = ImGui::GetContentRegionAvail().x > 420.0f;
		if (wide && ImGui::BeginTable("##pins", 2, ImGuiTableFlags_SizingStretchSame))
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); column(true);
			ImGui::TableSetColumnIndex(1); column(false);
			ImGui::EndTable();
		}
		else
		{
			column(true);
			column(false);
		}
		ImGui::Spacing();
	}

	void drawFigure(const Ctx& ctx, const docs::Block& b)
	{
		const Figure& f = figure(ctx, b.src);
		ImGui::Spacing();
		if (f.tex && f.w > 0)
		{
			const float avail = ImGui::GetContentRegionAvail().x;
			const float w = std::min({ avail, kFigureMaxWidth, static_cast<float>(f.w) });
			const float h = w * static_cast<float>(f.h) / static_cast<float>(f.w);
			ImGui::Image(f.tex, ImVec2(w, h));
		}
		if (!b.alt.empty())
		{
			EditorWidgets::WrapText wrap;
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::TextUnformatted(b.alt.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
	}

	void drawBlock(const Ctx& ctx, const docs::Block& b)
	{
		switch (b.kind)
		{
		case docs::BlockKind::Lead:
			// The opening sentence, in the heading tint rather than a larger
			// font: a second size in a narrow panel reads as a heading, and the
			// section already has one of those.
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			drawRuns(ctx, b.runs);
			ImGui::PopStyleColor();
			ImGui::Spacing();
			break;
		case docs::BlockKind::Paragraph:
			drawRuns(ctx, b.runs);
			ImGui::Spacing();
			break;
		case docs::BlockKind::Heading:
			ImGui::Spacing();
			if (ctx.sub) ImGui::PushFont(ctx.sub, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			drawRuns(ctx, b.runs);
			ImGui::PopStyleColor();
			if (ctx.sub) ImGui::PopFont();
			ImGui::Spacing();
			break;
		case docs::BlockKind::Bullets:
		case docs::BlockKind::Numbers:
		{
			int n = 1;
			for (const docs::Cell& item : b.items)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
				if (b.kind == docs::BlockKind::Numbers) ImGui::Text("%d.", n++);
				else                                    ImGui::TextUnformatted("\xe2\x80\xa2");
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 8.0f);
				ImGui::BeginGroup();
				drawRuns(ctx, item);
				ImGui::EndGroup();
			}
			ImGui::Spacing();
			break;
		}
		case docs::BlockKind::NodePreview:
			// The picture of the node, then the same pins with their types under
			// it. Two views of one thing on purpose: the drawing answers "which
			// one is it in the palette", the list answers "what does that pin
			// take".
			drawNodePreview(b);
			drawPins(b);
			break;
		case docs::BlockKind::Table:   drawTable(ctx, b);   break;
		case docs::BlockKind::Code:    drawCode(ctx, b);    break;
		case docs::BlockKind::Callout: drawCallout(ctx, b); break;
		case docs::BlockKind::Flow:    drawFlow(ctx, b);    break;
		case docs::BlockKind::Figure:  drawFigure(ctx, b);  break;
		case docs::BlockKind::Tile:
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			const bool clicked = ImGui::Selectable(b.title.c_str(), false, 0,
			                                       ImVec2(0.0f, 0.0f));
			ImGui::PopStyleColor();
			if (!b.sub.empty())
			{
				EditorWidgets::WrapText wrap;
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
				ImGui::TextUnformatted(b.sub.c_str());
				ImGui::PopStyleColor();
			}
			if (clicked) followLink(b.href);
			ImGui::Spacing();
			break;
		}
		case docs::BlockKind::Unknown: break;
		}
	}

	// ── The page body ────────────────────────────────────────────────────────
	void drawPage(const Ctx& ctx)
	{
		const docs::Library& lib = docs::library();
		const docs::Page& page = lib.pages()[static_cast<std::size_t>(s_page)];

		if (ctx.head) ImGui::PushFont(ctx.head, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextUnformatted(page.title.c_str());
		ImGui::PopStyleColor();
		if (ctx.head) ImGui::PopFont();
		if (!page.summary.empty())
		{
			EditorWidgets::WrapText wrap;
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::TextUnformatted(page.summary.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();

		for (int i = 0; i < static_cast<int>(page.sections.size()); ++i)
		{
			const docs::Section& sec = page.sections[static_cast<std::size_t>(i)];
			ImGui::PushID(i);
			ImGui::Separator();
			ImGui::Spacing();

			// The scroll lands here, on the section's first item — set while the
			// section is being submitted, which is the only moment ImGui can turn
			// "this one" into a scroll offset.
			if (s_scrollTo == i && s_scrollFrames > 0) ImGui::SetScrollHereY(0.0f);

			if (!sec.eyebrow.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
				ImGui::TextUnformatted(sec.eyebrow.c_str());
				ImGui::PopStyleColor();
			}
			if (ctx.sub) ImGui::PushFont(ctx.sub, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			ImGui::TextUnformatted(sec.title.c_str());
			ImGui::PopStyleColor();
			if (ctx.sub) ImGui::PopFont();

			// "Show me": this section is about a panel, and the reader can point
			// at it. Only where the mapping actually knows one — a button that
			// opens the wrong window would be worse than no button.
			const std::string topic = lib.topicOf(s_page, i);
			if (const HE::Ed::Help::PanelTopic* pt = HE::Ed::Help::panelForTopic(topic);
			    pt && s_panelOpener)
			{
				ImGui::SameLine();
				if (EditorWidgets::button("Show me"))
				{
					if (s_panelOpener(pt->window))
					{
						ImGui::SetWindowFocus(pt->window);
						s_spotlightWindow = pt->window;
						s_spotlightUntil  = ImGui::GetTime() + 4.0;
					}
				}
				if (ImGui::BeginItemTooltip())
				{
					ImGui::Text("Highlight the %s panel", pt->window);
					if (pt->menu[0]) ImGui::TextDisabled("%s", pt->menu);
					ImGui::EndTooltip();
				}
			}
			ImGui::Spacing();

			for (const docs::Block& b : sec.blocks) drawBlock(ctx, b);
			ImGui::Spacing();
			ImGui::PopID();
		}
		if (s_scrollFrames > 0 && --s_scrollFrames == 0) s_scrollTo = -1;
		if (s_navFrames > 0) --s_navFrames;
	}

	// ── Search results ───────────────────────────────────────────────────────
	void drawResults(const char* query)
	{
		const docs::Library& lib = docs::library();
		const std::vector<docs::Hit> hits = lib.search(query);

		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
		if (hits.empty()) ImGui::Text("Nothing in the manual matches \"%s\".", query);
		else              ImGui::Text("%d result%s for \"%s\"", static_cast<int>(hits.size()),
		                              hits.size() == 1 ? "" : "s", query);
		ImGui::PopStyleColor();
		ImGui::Spacing();

		// Enter opens the best hit, so a search can be finished without leaving
		// the keyboard — the whole point of a search box in a tool window.
		if (!hits.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
		{
			s_query[0] = '\0';
			go(hits[0].page, hits[0].section);
			return;
		}

		for (std::size_t i = 0; i < hits.size(); ++i)
		{
			const docs::Hit& h = hits[i];
			const docs::Page& p = lib.pages()[static_cast<std::size_t>(h.page)];
			const docs::Section& s = p.sections[static_cast<std::size_t>(h.section)];

			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			const bool clicked = ImGui::Selectable(s.title.c_str());
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			// The breadcrumb, so two similarly named sections on different pages
			// are told apart before either is opened.
			ImGui::Text("%s \xc2\xb7 %s", p.title.c_str(), s.eyebrow.empty()
			                                                  ? p.id.c_str()
			                                                  : s.eyebrow.c_str());
			ImGui::TextUnformatted(h.snippet.c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();
			ImGui::PopID();

			if (clicked)
			{
				s_query[0] = '\0';
				go(h.page, h.section);
				return;
			}
		}
	}

	// ── Sidebar: the sections of the open page ───────────────────────────────
	// A flat list works for a manual page with a dozen sections. The node
	// reference has three hundred — one per callable thing — and a flat list of
	// those is not navigation, it is the problem it was supposed to solve. Past
	// a threshold the list collapses into its eyebrows, which on that page are
	// the categories (Physics, Save, Transform …), and only the open one
	// unfolds.
	void drawSectionList(const docs::Page& page)
	{
		const int count = static_cast<int>(page.sections.size());
		ImGui::Indent(12.0f);

		auto row = [&](int si) {
			ImGui::PushID(si);
			ImGui::PushStyleColor(ImGuiCol_Text, si == s_section ? HE::Ed::Theme::TextHeading
			                                                     : HE::Ed::Theme::TextDim);
			if (ImGui::Selectable(page.sections[static_cast<std::size_t>(si)].title.c_str(),
			                      si == s_section))
				go(s_page, si);
			ImGui::PopStyleColor();
			ImGui::PopID();
		};

		if (count <= 30)
		{
			for (int si = 0; si < count; ++si) row(si);
		}
		else
		{
			// Walk the sections once, opening a group per new eyebrow. The
			// sections are already grouped in page order, so no sorting and no
			// second structure is needed — and the group holding the section
			// being read opens itself, which is what makes the sidebar follow
			// the reader instead of the other way round.
			for (int si = 0; si < count; )
			{
				const std::string& cat = page.sections[static_cast<std::size_t>(si)].eyebrow;
				int end = si;
				while (end < count &&
				       page.sections[static_cast<std::size_t>(end)].eyebrow == cat) ++end;

				const bool holdsCurrent = s_section >= si && s_section < end;
				ImGui::PushID(si);
				// Only FORCE the state right after a navigation — the group
				// holding what was just opened unfolds itself, and everything
				// else closes so the list does not grow with every visit.
				//
				// Every frame (which is what this was) is the same call and a
				// completely different control: the user's click flips the node,
				// the next frame sets it straight back, and the group cannot be
				// opened by hand at all.
				if (s_navFrames > 0)
					ImGui::SetNextItemOpen(holdsCurrent, ImGuiCond_Always);
				else
					ImGui::SetNextItemOpen(holdsCurrent, ImGuiCond_FirstUseEver);
				ImGui::PushStyleColor(ImGuiCol_Text, holdsCurrent ? HE::Ed::Theme::TextHeading
				                                                  : HE::Ed::Theme::Text);
				const bool open = ImGui::TreeNodeEx(
					"##cat", ImGuiTreeNodeFlags_SpanAvailWidth,
					"%s  (%d)", cat.empty() ? "Other" : cat.c_str(), end - si);
				ImGui::PopStyleColor();
				if (open)
				{
					for (int k = si; k < end; ++k) row(k);
					ImGui::TreePop();
				}
				ImGui::PopID();
				si = end;
			}
		}
		ImGui::Unindent(12.0f);
	}

	// ── Sidebar ──────────────────────────────────────────────────────────────
	void drawNav()
	{
		const docs::Library& lib = docs::library();
		for (const docs::Group& g : lib.groups())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::TextUnformatted(g.title.c_str());
			ImGui::PopStyleColor();
			for (int idx : g.pages)
			{
				const docs::Page& p = lib.pages()[static_cast<std::size_t>(idx)];
				ImGui::PushID(idx);
				if (ImGui::Selectable(p.title.c_str(), idx == s_page)) go(idx, -1);

				// The current page's sections, indented under it — the "on this
				// page" list, which is how a long reference page is navigated at
				// all. Only for the open page: all of them at once would be a
				// hundred and twelve rows.
				if (idx == s_page) drawSectionList(p);
				ImGui::PopID();
			}
			ImGui::Spacing();
		}
	}

	// ── Top bar ──────────────────────────────────────────────────────────────
	void drawToolbar(const Ctx& ctx)
	{
		const docs::Library& lib = docs::library();

		ImGui::BeginDisabled(s_historyPos <= 0);
		if (ImGui::ArrowButton("##back", ImGuiDir_Left) && s_historyPos > 0)
		{
			--s_historyPos;
			go(s_history[static_cast<std::size_t>(s_historyPos)].first,
			   s_history[static_cast<std::size_t>(s_historyPos)].second, false);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Back");

		ImGui::SameLine(0.0f, 4.0f);
		ImGui::BeginDisabled(s_historyPos < 0 ||
		                     s_historyPos + 1 >= static_cast<int>(s_history.size()));
		if (ImGui::ArrowButton("##fwd", ImGuiDir_Right) &&
		    s_historyPos + 1 < static_cast<int>(s_history.size()))
		{
			++s_historyPos;
			go(s_history[static_cast<std::size_t>(s_historyPos)].first,
			   s_history[static_cast<std::size_t>(s_historyPos)].second, false);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Forward");

		ImGui::SameLine(0.0f, 8.0f);
		if (EditorWidgets::button("Start")) go(0, -1);
		ImGui::SetItemTooltip("The first page of the manual");

		ImGui::SameLine(0.0f, 12.0f);
		if (s_focusSearch) { ImGui::SetKeyboardFocusHere(); s_focusSearch = false; }
		ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x - 190.0f));
		ImGui::InputTextWithHint("##docsSearch", "Search the manual...",
		                         s_query, sizeof(s_query));

		ImGui::SameLine(0.0f, 8.0f);
		if (EditorWidgets::button("Online"))
			SDL_OpenURL(lib.url(s_page, s_section).c_str());
		ImGui::SetItemTooltip("Open this page on horizoncreations.dev");
	}

#endif // HE_DOCS_PANEL_IMPL
} // namespace

void setPanelOpener(PanelOpener opener)
{
#ifdef HE_DOCS_PANEL_IMPL
	s_panelOpener = opener;
#else
	(void)opener;
#endif
}

void open()
{
#ifdef HE_DOCS_PANEL_IMPL
	ensureLoaded();
	s_open = true;
	s_openedFrame = ImGui::GetFrameCount();
	s_focusSearch = true;
	if (s_history.empty()) go(0, -1);
#endif
}

void openTopic(const char* topic)
{
#ifdef HE_DOCS_PANEL_IMPL
	ensureLoaded();
	s_open = true;
	s_openedFrame = ImGui::GetFrameCount();
	s_query[0] = '\0';
	goTopic(topic);
	// Even an unknown topic leaves the manual open at whatever was last read:
	// a keypress that appears to do nothing is worse than the wrong page.
	if (s_history.empty()) go(0, -1);
#else
	(void)topic;
#endif
}

void openSearch(const char* query)
{
#ifdef HE_DOCS_PANEL_IMPL
	ensureLoaded();
	s_open = true;
	s_openedFrame = ImGui::GetFrameCount();
	std::snprintf(s_query, sizeof(s_query), "%s", query ? query : "");
	s_focusSearch = true;
	if (s_history.empty()) go(0, -1);
#else
	(void)query;
#endif
}

void close()
{
#ifdef HE_DOCS_PANEL_IMPL
	s_open = false;
#endif
}

bool isOpen()
{
#ifdef HE_DOCS_PANEL_IMPL
	return s_open;
#else
	return false;
#endif
}

bool openedThisFrame()
{
#ifdef HE_DOCS_PANEL_IMPL
	return s_openedFrame == ImGui::GetFrameCount();
#else
	return false;
#endif
}

bool navigatingGroups()
{
#ifdef HE_DOCS_PANEL_IMPL
	return s_navFrames > 0;
#else
	return false;
#endif
}

void draw(const Host& host)
{
#ifdef HE_DOCS_PANEL_IMPL
	const Ctx ctx{
		static_cast<ImFont*>(host.fontBody),
		static_cast<ImFont*>(host.fontSubheading),
		static_cast<ImFont*>(host.fontHeading),
		static_cast<ImFont*>(host.fontCode),
		host.renderer,
	};

	// The spotlight outlives the click that started it, and has to keep drawing
	// even while the reader is scrolled elsewhere — so it is here, before the
	// early return for a closed window.
	if (s_spotlightWindow)
	{
		if (ImGui::GetTime() < s_spotlightUntil)
			HE::Ed::Spotlight::outline(s_spotlightWindow,
			                           static_cast<float>(ImGui::GetTime()), false);
		else
			s_spotlightWindow = nullptr;
	}

	if (!s_open) return;
	ensureLoaded();

	// The reader's own controls explain themselves too — and the scope covers the
	// whole window, not just its toolbar: "Show me" sits inside a page and the
	// "open it online" button only appears on the error screen.
	HE::Ed::Help::Scope helpScope("Documentation");

	ImGui::SetNextWindowSize(ImVec2(1000.0f, 680.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 320.0f), ImVec2(FLT_MAX, FLT_MAX));
	if (!ImGui::Begin(kWindowTitle, &s_open, ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::End();
		return;
	}
	// Kept inside the editor window: multi-viewport would otherwise hand a
	// dragged-out reader its own OS window, which the window manager can bury
	// behind the editor while it is still open (see EditorWidgets.h).
	EditorWidgets::clampCurrentWindowToEditorWindow();

	const docs::Library& lib = docs::library();
	if (!lib.loaded())
	{
		// The guard needs its own block: its destructor pops the wrap position
		// off the CURRENT window, and ImGui::End() has by then switched that to
		// the parent (see EditorWidgets.h — this is the case the rule exists for,
		// an End() at the same level as the guard).
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextUnformatted("The offline manual could not be loaded.");
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::TextUnformatted(lib.error().c_str());
			ImGui::TextUnformatted("It ships next to the editor as Docs/he-docs.json.");
			ImGui::PopStyleColor();
			ImGui::Spacing();
			if (EditorWidgets::button("Open the manual online"))
				SDL_OpenURL("https://horizoncreations.dev/HorizonEngineDocs/");
		}
		ImGui::End();
		return;
	}

	// Keyboard, while the reader has the focus. Ctrl/Cmd+F is where every reader
	// puts search; Escape backs out one step at a time — the results first, the
	// window only once there is nothing left to leave.
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
	{
		const ImGuiIO& io = ImGui::GetIO();
		if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_F, false))
			s_focusSearch = true;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput)
		{
			if (s_query[0]) s_query[0] = '\0';
			else            s_open = false;
		}
	}

	drawToolbar(ctx);
	ImGui::Separator();

	const float navWidth = 230.0f;
	// Measured before the sidebar is drawn, because the reading column's left
	// margin has to be part of the SameLine that places it — an Indent() after a
	// SameLine moves the NEXT line, not this one, which put the body on top of
	// the sidebar the first time this was written.
	const float bodyAvail = ImGui::GetContentRegionAvail().x - navWidth
	                      - ImGui::GetStyle().ItemSpacing.x;

	ImGui::BeginChild("##docsNav", ImVec2(navWidth, 0.0f), ImGuiChildFlags_Borders);
	{
		drawNav();
	}
	ImGui::EndChild();

	// ── The reading column ──────────────────────────────────────────────────
	// The body is capped at a reading width instead of filling the window. Long
	// measures are why a page of documentation reads as a wall: past roughly
	// ninety characters the eye loses the start of the next line, and a
	// maximised reader on a wide screen was well past that. The cap is in EM, so
	// it follows the UI font scale rather than fighting it.
	//
	// The cap is on the SCROLLING child itself rather than on a column inside
	// it. A child nested in it would be the one SetScrollHereY talks to, and
	// with AutoResizeY that child never scrolls — so every jump to a section
	// would silently do nothing.
	{
		const float cap = ImGui::GetFontSize() * 42.0f;
		const float pad = bodyAvail > cap ? (bodyAvail - cap) * 0.35f : 0.0f;
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x + pad);
		ImGui::BeginChild("##docsBody", ImVec2(std::min(bodyAvail, cap), 0.0f),
		                  ImGuiChildFlags_None);
		{
			// Everything in the body is prose in a narrow column, so the wrap
			// position is pushed once for the whole child (see EditorWidgets::
			// WrapText — this is the BeginChild case, where the guard's own block
			// ends before EndChild).
			EditorWidgets::WrapText wrap;
			if (s_query[0]) drawResults(s_query);
			else            drawPage(ctx);

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			ImGui::Text("Offline copy of the manual, generated %s.", lib.generated().c_str());
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
	}

	ImGui::End();
#else
	(void)host;
#endif
}

// The adapter the editor calls: pull the four fonts and the renderer out of the
// AppContext and hand the panel the little it actually needs.
#ifdef HE_IMGUI_ENABLED
void draw(AppContext& ctx)
{
	Host host;
	host.fontBody       = ctx.fontBody;
	host.fontSubheading = ctx.fontSubheading;
	host.fontHeading    = ctx.fontHeading;
	host.fontCode       = ctx.codeFont;
	host.renderer       = ctx.renderer;
	draw(host);
}
#endif

} // namespace DocsPanel
