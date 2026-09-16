#include "doctest.h"
#include "EditorShortcuts.h"
#include "EditorHelp.h"

#include <imgui.h>
#include <set>
#include <string>

// ── The shortcut table ───────────────────────────────────────────────────────
// What the Shortcuts page and every pressed("…") call rely on: the table is
// consistent, a rebind survives the trip through the config string, a clash
// is reported where two actions could really fire together, and a chord
// fires only with exactly its modifiers.

using namespace EditorShortcuts;

namespace
{
	// An ImGui context for the parts that read the keyboard. Every test
	// starts from the defaults — the overrides are process-global.
	struct Ctx
	{
		Ctx()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(400.0f, 300.0f);
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // no atlas to bake
			// The mac swap (Cmd ↔ Ctrl in AddKeyEvent) would make the key
			// events below land on the other flag; the rule under test is
			// platform-independent, so it is switched off here.
			io.ConfigMacOSXBehaviors = false;
			resetAll();
		}
		~Ctx() { resetAll(); ImGui::DestroyContext(); }

		// One frame with these keys down (edge = pressed this frame).
		void frame(std::initializer_list<ImGuiKey> down, bool typing = false)
		{
			ImGuiIO& io = ImGui::GetIO();
			for (const ImGuiKey k : down) io.AddKeyEvent(k, true);
			ImGui::NewFrame();
			io.WantTextInput = typing;
		}
		void endFrame(std::initializer_list<ImGuiKey> up)
		{
			ImGui::EndFrame();
			ImGuiIO& io = ImGui::GetIO();
			for (const ImGuiKey k : up) io.AddKeyEvent(k, false);
			ImGui::NewFrame();   // lets the releases land
			ImGui::EndFrame();
		}
	};
}

TEST_CASE("shortcuts: the table is consistent")
{
	Ctx ctx;
	std::set<std::string> ids;
	for (const Action& a : actions())
	{
		CHECK(ids.insert(a.id).second);              // unique ids
		CHECK(std::string(a.label).size() > 0);
		CHECK(std::string(a.category).size() > 0);
		CHECK(a.defaultChord != ImGuiKey_None);      // everything ships bound
		// Every default spells and reads back as itself.
		CHECK(chordFromText(chordToText(a.defaultChord)) == a.defaultChord);
		// …and no two actions that could fire together ship on one chord.
		CHECK_MESSAGE(conflicts(a.id).empty(), a.id);
	}
	CHECK(find("file.save") != nullptr);
	CHECK(find("no.such.action") == nullptr);
	CHECK(chord("no.such.action") == ImGuiKey_None);
}

TEST_CASE("shortcuts: chord text is portable and forgiving")
{
	Ctx ctx;
	CHECK(chordToText(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S) == "Ctrl+Shift+S");
	CHECK(chordToText(ImGuiKey_F11) == "F11");
	CHECK(chordToText(ImGuiMod_Alt | ImGuiKey_Keypad7) == "Alt+Keypad7");
	CHECK(chordToText(ImGuiKey_None).empty());
	CHECK(chordFromText("Ctrl+Shift+S") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S));
	CHECK(chordFromText("End") == ImGuiKey_End);
	CHECK(chordFromText("") == ImGuiKey_None);
	CHECK(chordFromText("Ctrl+") == ImGuiKey_None);
	CHECK(chordFromText("Hyper+S") == ImGuiKey_None);        // unknown modifier
	CHECK(chordFromText("NotAKey") == ImGuiKey_None);
	CHECK(chordFromText("MouseLeft") == ImGuiKey_None);      // never a chord key

	// What the menus print: Ctrl becomes Cmd on a Mac, Delete reads "Del".
	CHECK(chordLabel(ImGuiMod_Ctrl | ImGuiKey_S) == HE::Ed::Help::shortcutLabel("Ctrl+S"));
	CHECK(chordLabel(ImGuiKey_Delete) == "Del");
	CHECK(chordLabel(ImGuiMod_Ctrl | ImGuiKey_Comma) == HE::Ed::Help::shortcutLabel("Ctrl+,"));
	CHECK(chordLabel(ImGuiKey_None).empty());
}

TEST_CASE("shortcuts: overrides round-trip through the config string")
{
	Ctx ctx;
	CHECK(encode().empty());                     // nothing changed, nothing written
	CHECK(isDefault("file.save"));

	setChord("file.save", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_W);
	setChord("viewport.focus", ImGuiKey_None);   // unbound on purpose
	CHECK_FALSE(isDefault("file.save"));
	CHECK(label("viewport.focus").empty());
	const std::string text = encode();
	CHECK(text == "file.save=Ctrl+Alt+W;viewport.focus=");

	resetAll();
	CHECK(chord("file.save") == find("file.save")->defaultChord);
	decode(text);
	CHECK(chord("file.save") == (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_W));
	CHECK(chord("viewport.focus") == ImGuiKey_None);
	CHECK(chord("edit.undo") == find("edit.undo")->defaultChord);   // untouched

	// Setting the default again clears the override rather than storing it.
	setChord("file.save", find("file.save")->defaultChord);
	CHECK(isDefault("file.save"));
	reset("viewport.focus");
	CHECK(encode().empty());

	// An id from a build that no longer has it, and junk, are dropped quietly.
	decode("gone.action=Ctrl+Q;garbage;edit.redo=F5");
	CHECK(encode() == "edit.redo=F5");
}

TEST_CASE("shortcuts: a clash is per scope, and Global clashes with everything")
{
	Ctx ctx;
	// W is the viewport's Move; Focus rebound to W clashes with it (same scope)…
	setChord("viewport.focus", ImGuiKey_W);
	auto c = conflicts("viewport.focus");
	REQUIRE(c.size() == 1);
	CHECK(std::string(c[0]->id) == "viewport.move");
	CHECK(conflicts("viewport.move").size() == 1);   // …and the other way round.
	resetAll();

	// A Global action on W clashes with the viewport's, though the scopes differ.
	setChord("entity.copy", ImGuiKey_W);
	c = conflicts("entity.copy");
	REQUIRE(c.size() == 1);
	CHECK(std::string(c[0]->id) == "viewport.move");
	resetAll();

	// Unbound never clashes.
	setChord("viewport.focus", ImGuiKey_None);
	CHECK(conflicts("viewport.focus").empty());
}

TEST_CASE("shortcuts: a chord fires with exactly its modifiers, and not while typing")
{
	Ctx ctx;

	// Ctrl+S: Save, not Save All.
	ctx.frame({ ImGuiMod_Ctrl, ImGuiKey_S });
	CHECK(pressed("file.save"));
	CHECK_FALSE(pressed("file.saveAll"));
	CHECK_FALSE(pressed("file.saveSceneAs"));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiKey_S });

	// Ctrl+Shift+S: Save All, not Save.
	ctx.frame({ ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_S });
	CHECK(pressed("file.saveAll"));
	CHECK_FALSE(pressed("file.save"));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_S });

	// Cmd counts as Ctrl — the editor's rule on every platform.
	ctx.frame({ ImGuiMod_Super, ImGuiKey_S });
	CHECK(pressed("file.save"));
	ctx.endFrame({ ImGuiMod_Super, ImGuiKey_S });

	// A bare key does not fire a modified chord, and a modified key does not
	// fire a bare one (W with Ctrl is not Move).
	ctx.frame({ ImGuiKey_S });
	CHECK_FALSE(pressed("file.save"));
	ctx.endFrame({ ImGuiKey_S });
	ctx.frame({ ImGuiMod_Ctrl, ImGuiKey_W });
	CHECK_FALSE(pressed("viewport.move"));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiKey_W });
	ctx.frame({ ImGuiKey_W });
	CHECK(pressed("viewport.move"));
	ctx.endFrame({ ImGuiKey_W });

	// While a text field has the keyboard nothing typed fires — but a
	// function key still does.
	ctx.frame({ ImGuiMod_Ctrl, ImGuiKey_S }, /*typing=*/true);
	CHECK_FALSE(pressed("file.save"));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiKey_S });
	ctx.frame({ ImGuiKey_F11 }, /*typing=*/true);
	CHECK(pressed("view.fullscreen"));
	ctx.endFrame({ ImGuiKey_F11 });

	// A rebind is what fires afterwards; the old key is dead.
	setChord("file.save", ImGuiKey_F9);
	ctx.frame({ ImGuiMod_Ctrl, ImGuiKey_S });
	CHECK_FALSE(pressed("file.save"));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiKey_S });
	ctx.frame({ ImGuiKey_F9 });
	CHECK(pressed("file.save"));
	ctx.endFrame({ ImGuiKey_F9 });

	// An unbound action never fires.
	setChord("file.save", ImGuiKey_None);
	ctx.frame({ ImGuiKey_F9 });
	CHECK_FALSE(pressed("file.save"));
	ctx.endFrame({ ImGuiKey_F9 });
}

TEST_CASE("shortcuts: nothing fires while the page is capturing, and the hold expires on its own")
{
	Ctx ctx;
	ImGuiIO& io = ImGui::GetIO();
	// Frame 1: the page says it is capturing (it draws AFTER the editor's
	// global shortcut block, so the block always sees last frame's stamp).
	ImGui::NewFrame();
	noteCapturing();
	CHECK(capturingNow());
	ImGui::EndFrame();

	// Frame 2: Ctrl+S is the binding being chosen, not a Save.
	io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_S, true);
	ImGui::NewFrame();
	CHECK(capturingNow());
	CHECK_FALSE(pressed("file.save"));
	ImGui::EndFrame();
	io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_S, false);

	// Frame 3: the page is no longer drawn (tab switched) — the hold expires
	// on its own rather than leaving the keyboard dead.
	ImGui::NewFrame();
	CHECK_FALSE(capturingNow());
	ImGui::EndFrame();

	// Frame 4: Save works again.
	io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_S, true);
	ImGui::NewFrame();
	CHECK(pressed("file.save"));
	ImGui::EndFrame();
	io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_S, false);
}

TEST_CASE("shortcuts: the capture reads the key that went down with the held modifiers")
{
	Ctx ctx;
	ctx.frame({ ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K });
	CHECK(captureChord() == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K));
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K });

	// Modifiers alone are not a chord yet.
	ctx.frame({ ImGuiMod_Ctrl, ImGuiKey_LeftCtrl });
	CHECK(captureChord() == ImGuiKey_None);
	ctx.endFrame({ ImGuiMod_Ctrl, ImGuiKey_LeftCtrl });

	// Nothing down: nothing captured.
	ctx.frame({});
	CHECK(captureChord() == ImGuiKey_None);
	ctx.endFrame({});
}
