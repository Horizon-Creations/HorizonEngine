#include "doctest.h"

#include "CppClassEditorPanel.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <string>

// ── The C++ class tab and a collaboration peer's edit (EDT-307) ──────────────
// What has to hold: when a peer's change lands in the .h or the .cpp of an
// open class, reloadFromDisk makes the tab show the file as it is now — not
// the text it read when the tab was first opened. Before the fix the reload
// dropped a flag nothing read, so the tab kept the old text for the session.

namespace fs = std::filesystem;

namespace
{
	struct ImGuiScope
	{
		ImGuiScope()  { ImGui::CreateContext(); }
		~ImGuiScope() { ImGui::DestroyContext(); }
	};

	void writeFile(const fs::path& p, const std::string& text)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out << text;
	}

	// A fresh folder per case: the panel's state is keyed by path and lives for
	// the session, so two cases sharing a path would read each other's buffers.
	fs::path freshDir(const char* name)
	{
		const fs::path dir = fs::temp_directory_path() / "he_cpp_class_editor" / name;
		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::create_directories(dir);
		return dir;
	}
}

TEST_CASE("cpp class editor: a reload shows what is on disk now")
{
	ImGuiScope imgui;
	const fs::path dir = freshDir("reload");
	const std::string h   = (dir / "Foo.h").string();
	const std::string cpp = (dir / "Foo.cpp").string();
	writeFile(h,   "struct Foo { int a; };");
	writeFile(cpp, "#include \"Foo.h\"");

	CHECK(CppClassEditorPanel::bufferText(h, true)  == "struct Foo { int a; };");
	CHECK(CppClassEditorPanel::bufferText(h, false) == "#include \"Foo.h\"");

	// A peer rewrites the header. Without a reload the tab keeps what it read —
	// that is the cache, not the bug.
	writeFile(h, "struct Foo { int a; int b; };");
	CHECK(CppClassEditorPanel::bufferText(h, true) == "struct Foo { int a; };");

	REQUIRE(CppClassEditorPanel::reloadFromDisk(h));
	CHECK(CppClassEditorPanel::bufferText(h, true) == "struct Foo { int a; int b; };");
	CHECK_FALSE(CppClassEditorPanel::isDirty(h));

	CppClassEditorPanel::forget(h);
}

TEST_CASE("cpp class editor: a peer's edit to the .cpp reaches a tab opened on the .h")
{
	// The tab is keyed by the canonical file (the header when there is one), but
	// the collaboration layer names the file that changed.
	ImGuiScope imgui;
	const fs::path dir = freshDir("sibling");
	const std::string h   = (dir / "Bar.h").string();
	const std::string cpp = (dir / "Bar.cpp").string();
	writeFile(h,   "struct Bar;");
	writeFile(cpp, "int bar() { return 1; }");

	CHECK(CppClassEditorPanel::bufferText(h, false) == "int bar() { return 1; }");

	writeFile(cpp, "int bar() { return 2; }");
	REQUIRE(CppClassEditorPanel::reloadFromDisk(cpp));
	CHECK(CppClassEditorPanel::bufferText(h, false) == "int bar() { return 2; }");
	CHECK(CppClassEditorPanel::bufferText(h, true)  == "struct Bar;");

	// A file this panel holds no tab for is not its business.
	CHECK_FALSE(CppClassEditorPanel::reloadFromDisk((dir / "Other.cpp").string()));

	CppClassEditorPanel::forget(h);
}
