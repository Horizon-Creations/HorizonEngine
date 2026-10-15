#include "doctest.h"
#include "TestFsUtil.h"
#include "ProjectLaunchOpen.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// A .heproj handed to the editor from outside — a double-click in the file
// manager arrives as a launch argument (Windows/Linux) or an open event (macOS).
// What a raw string means, whether it may be opened, and what happens to it
// given the project that is open now; the editor only adds the log line and the
// call into the Hub's or the editor's own Open.

namespace fs = std::filesystem;
namespace PLO = ProjectLaunchOpen;
using PLO::PathError;

namespace
{
	struct Sandbox
	{
		fs::path root;
		Sandbox(const char* tag)
		{
			root = fs::temp_directory_path() / ("he_test_launch_open_" + std::string(tag));
			he_test::removeAllQuiet(root);
			fs::create_directories(root);
		}
		~Sandbox() { he_test::removeAllQuiet(root); }
		fs::path touch(const fs::path& rel) const
		{
			const fs::path p = root / rel;
			fs::create_directories(p.parent_path());
			std::ofstream(p) << "{}";
			return p;
		}
	};

	std::string norm(const fs::path& p) { return p.lexically_normal().string(); }

	// The slot is process-wide; a case that leaves a request behind must not
	// hand it to the next one.
	void drain() { std::string s; while (PLO::take(s)) {} }
}

TEST_CASE("ProjectLaunchOpen::parse accepts a plain absolute path")
{
	Sandbox sb("plain");
	const fs::path proj = sb.touch("Game/Game.heproj");
	const PLO::ParsedPath r = PLO::parse(proj.string(), "/somewhere/else");
	CHECK(r.ok());
	CHECK(r.path == norm(proj));
}

TEST_CASE("ProjectLaunchOpen::parse strips surrounding whitespace and one pair of quotes")
{
	Sandbox sb("quotes");
	const fs::path proj = sb.touch("My Game/My Game.heproj");   // spaces in folder AND name

	SUBCASE("double quotes")
	{
		const auto r = PLO::parse("\"" + proj.string() + "\"", sb.root);
		CHECK(r.ok());
		CHECK(r.path == norm(proj));
	}
	SUBCASE("single quotes")
	{
		const auto r = PLO::parse("'" + proj.string() + "'", sb.root);
		CHECK(r.ok());
		CHECK(r.path == norm(proj));
	}
	SUBCASE("whitespace outside the quotes, as a registry line leaves it")
	{
		const auto r = PLO::parse("  \"" + proj.string() + "\"\r\n", sb.root);
		CHECK(r.ok());
		CHECK(r.path == norm(proj));
	}
	SUBCASE("unquoted with spaces is one path, not three")
	{
		const auto r = PLO::parse(proj.string(), sb.root);
		CHECK(r.ok());
		CHECK(r.path == norm(proj));
	}
	SUBCASE("mismatched quotes are not stripped")
	{
		const auto r = PLO::parse("\"" + proj.string() + "'", sb.root);
		CHECK_FALSE(r.ok());
	}
	SUBCASE("nothing but quotes and blanks")
	{
		CHECK(PLO::parse("   ", sb.root).error == PathError::Empty);
		CHECK(PLO::parse("\"\"", sb.root).error == PathError::Empty);
		CHECK(PLO::parse("", sb.root).error == PathError::Empty);
	}
}

TEST_CASE("ProjectLaunchOpen::parse resolves a relative path against the start directory")
{
	Sandbox sb("relative");
	const fs::path proj = sb.touch("Projects/Demo/Demo.heproj");
	const fs::path cwd  = sb.root / "Projects";
	fs::create_directories(cwd / "Other");

	CHECK(PLO::parse("Demo/Demo.heproj", cwd).path == norm(proj));
	CHECK(PLO::parse("./Demo/Demo.heproj", cwd).ok());
	// ".." is folded away, so the path the editor remembers is the plain one.
	const auto up = PLO::parse("Other/../Demo/Demo.heproj", cwd);
	CHECK(up.ok());
	CHECK(up.path == norm(proj));
	// Relative to the start directory, NOT to wherever the test happens to run.
	CHECK(PLO::parse("Demo/Demo.heproj", sb.root).error == PathError::NotFound);
}

TEST_CASE("ProjectLaunchOpen::parse turns a file:// URL back into a path")
{
	Sandbox sb("url");
	const fs::path proj = sb.touch("With Space/P.heproj");
	std::string url = "file://" + proj.generic_string();
	// Percent-escape the space the way a file manager hands it over.
	for (size_t i = url.find(' '); i != std::string::npos; i = url.find(' ', i))
		url.replace(i, 1, "%20");
	const auto r = PLO::parse(url, sb.root);
	CHECK(r.ok());
	CHECK(PLO::samePath(r.path, proj.string()));
}

TEST_CASE("ProjectLaunchOpen::parse refuses what is not an openable project")
{
	Sandbox sb("refuse");
	const fs::path scene = sb.touch("Level.hescene");
	fs::create_directories(sb.root / "Folder.heproj");

	CHECK(PLO::parse(scene.string(), sb.root).error == PathError::WrongExtension);
	CHECK(PLO::parse("readme.txt", sb.root).error == PathError::WrongExtension);
	CHECK(PLO::parse("Game.heproj.bak", sb.root).error == PathError::WrongExtension);
	CHECK(PLO::parse("heproj", sb.root).error == PathError::WrongExtension);
	CHECK(PLO::parse("Missing.heproj", sb.root).error == PathError::NotFound);
	CHECK(PLO::parse((sb.root / "Folder.heproj").string(), sb.root).error == PathError::NotAFile);
	// Every reason has a sentence for the log.
	for (PathError e : { PathError::Empty, PathError::WrongExtension,
	                     PathError::NotFound, PathError::NotAFile })
		CHECK(std::string(PLO::describe(e)).size() > 0);
}

TEST_CASE("ProjectLaunchOpen::parse takes the extension in any case")
{
	Sandbox sb("case");
	const fs::path proj = sb.touch("Upper.HEPROJ");
	CHECK(PLO::parse(proj.string(), sb.root).ok());
}

TEST_CASE("ProjectLaunchOpen::pickFromArguments opens the first project and reports the rest")
{
	Sandbox sb("pick");
	const fs::path a = sb.touch("A/A.heproj");
	const fs::path b = sb.touch("B/B.heproj");

	SUBCASE("no arguments")
	{
		const auto pick = PLO::pickFromArguments({}, sb.root);
		CHECK(pick.project.empty());
		CHECK(pick.rejected.empty());
		CHECK(pick.ignored.empty());
	}
	SUBCASE("junk before the project does not stop it")
	{
		const auto pick = PLO::pickFromArguments({ "notes.txt", "Nope.heproj", "A/A.heproj" }, sb.root);
		CHECK(pick.project == norm(a));
		REQUIRE(pick.rejected.size() == 2);
		CHECK(pick.rejected[0].first == "notes.txt");
		CHECK(pick.rejected[0].second == PathError::WrongExtension);
		CHECK(pick.rejected[1].second == PathError::NotFound);
	}
	SUBCASE("a second project is ignored, not opened")
	{
		const auto pick = PLO::pickFromArguments({ a.string(), b.string() }, sb.root);
		CHECK(pick.project == norm(a));
		REQUIRE(pick.ignored.size() == 1);
		CHECK(pick.ignored[0] == norm(b));
	}
	SUBCASE("the same project twice is one project")
	{
		const auto pick = PLO::pickFromArguments({ a.string(), "A/./A.heproj" }, sb.root);
		CHECK(pick.project == norm(a));
		CHECK(pick.ignored.empty());
	}
}

TEST_CASE("ProjectLaunchOpen::decide follows the Hub, the guarded switch, or does nothing")
{
	Sandbox sb("decide");
	const fs::path a = sb.touch("A/A.heproj");
	const fs::path b = sb.touch("B/B.heproj");

	// Nothing open: the Hub's direct load.
	CHECK(PLO::decide(false, "", a.string()) == PLO::Action::OpenDirect);
	// A stale "current" path with no project loaded is still the Hub.
	CHECK(PLO::decide(false, a.string(), a.string()) == PLO::Action::OpenDirect);
	// Another project open: the unsaved-work prompt and the session switch.
	CHECK(PLO::decide(true, a.string(), b.string()) == PLO::Action::OpenGuarded);
	// The open project again — a second double-click, or macOS's launch event
	// arriving after the start already opened it — is nothing to do, also when
	// it is spelled differently.
	CHECK(PLO::decide(true, a.string(), a.string()) == PLO::Action::AlreadyOpen);
	CHECK(PLO::decide(true, (sb.root / "A" / ".." / "A" / "A.heproj").string(), a.string())
	      == PLO::Action::AlreadyOpen);
}

TEST_CASE("ProjectLaunchOpen request slot holds one request until it is taken")
{
	drain();
	std::string got;
	CHECK_FALSE(PLO::pending());
	CHECK_FALSE(PLO::take(got));

	CHECK_FALSE(PLO::post(""));             // nothing to open is not a request
	CHECK_FALSE(PLO::pending());

	CHECK(PLO::post("/p/First.heproj"));
	CHECK(PLO::pending());
	// Several files double-clicked at once: the first wins, the rest are refused.
	CHECK_FALSE(PLO::post("/p/Second.heproj"));

	CHECK(PLO::take(got));
	CHECK(got == "/p/First.heproj");
	CHECK_FALSE(PLO::pending());
	CHECK_FALSE(PLO::take(got));            // taken once, not twice

	CHECK(PLO::post("/p/Third.heproj"));    // the slot is free again
	drain();
}
