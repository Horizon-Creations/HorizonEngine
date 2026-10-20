#include "doctest.h"

#include "SourceControlTree.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

// ─── The Source Control panel's "Tree view" as lines (EDT-1503) ──────────────
// The panel draws these through a list clipper; what has to stay is the shape the
// nested TreeNodeEx calls had: folders before files at every level, folders by
// name, files in row order, single-child chains folded into one line, and a folded
// folder hiding exactly its own subtree.

using HE::Ed::ChangeTree::Line;

namespace {

std::vector<Line> build(const std::vector<std::string>& paths)
{
	return HE::Ed::ChangeTree::build(static_cast<int>(paths.size()),
		[&](int i) -> const std::string& { return paths[static_cast<std::size_t>(i)]; });
}

// "label@depth" for folders, "#row@depth" for files: the tree in one string.
std::string shape(const std::vector<Line>& lines, const std::vector<int>* only = nullptr)
{
	std::string out;
	auto add = [&](const Line& l) {
		if (!out.empty()) out += ' ';
		out += (l.row < 0 ? l.label : "#" + std::to_string(l.row)) + "@" + std::to_string(l.depth);
	};
	if (only) for (int i : *only) add(lines[static_cast<std::size_t>(i)]);
	else      for (const Line& l : lines) add(l);
	return out;
}

} // namespace

TEST_CASE("Source control tree: order, chain folding and depth")
{
	// Sorted by path, as the panel's model sorts them.
	const std::vector<std::string> paths = {
		"Content/Meshes/Props/Crate.hasset",   // 0: Content/Meshes/Props is a chain
		"Content/Meshes/Props/Rock.hasset",    // 1
		"README.md",                           // 2: a root file comes after root folders
		"Source/Game.cpp",                     // 3
		"Source/Game.h",                       // 4
		"Source/sub/Util.h",                   // 5: a sub-folder comes before Source's files
	};
	const std::vector<Line> lines = build(paths);
	CHECK(shape(lines) ==
	      "Content/Meshes/Props@0 #0@1 #1@1 Source@0 sub@1 #5@2 #3@1 #4@1 #2@0");

	// The fold key is the full path, chain included; `end` spans the subtree.
	REQUIRE(lines.size() == 9);
	CHECK(lines[0].key == "Content/Meshes/Props");
	CHECK(lines[0].end == 3);
	CHECK(lines[3].key == "Source");
	CHECK(lines[3].end == 8);
	CHECK(lines[4].key == "Source/sub");
	CHECK(lines[4].end == 6);
}

TEST_CASE("Source control tree: a folded folder hides its subtree and nothing else")
{
	const std::vector<std::string> paths = {
		"A/x.txt", "A/B/y.txt", "A/B/z.txt", "C/w.txt", "top.txt",
	};
	const std::vector<Line> lines = build(paths);
	CHECK(shape(lines) == "A@0 B@1 #1@2 #2@2 #0@1 C@0 #3@1 #4@0");

	std::vector<int> vis;
	HE::Ed::ChangeTree::visible(lines, {}, vis);
	CHECK(vis.size() == lines.size());

	HE::Ed::ChangeTree::visible(lines, { "A/B" }, vis);
	CHECK(shape(lines, &vis) == "A@0 B@1 #0@1 C@0 #3@1 #4@0");

	// The outer fold wins; an inner fold under it changes nothing more.
	HE::Ed::ChangeTree::visible(lines, { "A", "A/B" }, vis);
	CHECK(shape(lines, &vis) == "A@0 C@0 #3@1 #4@0");

	HE::Ed::ChangeTree::visible(lines, { "C" }, vis);
	CHECK(shape(lines, &vis) == "A@0 B@1 #1@2 #2@2 #0@1 C@0 #4@0");
}

TEST_CASE("Source control tree: thousands of new files are one pass, not a frame's work")
{
	// The case the finding was about: a fresh import. Building is once per status
	// snapshot; with the big folder folded, the visible list is a handful of lines.
	std::vector<std::string> paths;
	for (int i = 0; i < 5000; ++i)
	{
		char name[64];
		std::snprintf(name, sizeof(name), "Content/Imported/Asset%05d.hasset", i);
		paths.emplace_back(name);
	}
	paths.emplace_back("project.heproj");
	const std::vector<Line> lines = build(paths);
	REQUIRE(lines.size() == 5002);
	CHECK(lines[0].label == "Content/Imported");
	CHECK(lines[0].end == 5001);
	CHECK(lines[1].row == 0);
	CHECK(lines[5000].row == 4999);
	CHECK(lines[5001].row == 5000);

	std::vector<int> vis;
	HE::Ed::ChangeTree::visible(lines, { "Content/Imported" }, vis);
	CHECK(vis == std::vector<int>{ 0, 5001 });
}
