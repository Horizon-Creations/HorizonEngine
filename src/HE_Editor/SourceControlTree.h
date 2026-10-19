#pragma once
// The Source Control panel's "Tree view" as a flat list of lines (EDT-1503).
//
// The tree used to be rebuilt per frame and section (a std::map node and a substr
// per path level) and drawn as nested TreeNodeEx calls, every row of it: a fresh
// import with thousands of new files cost thousands of rows a frame, while the
// flat view next to it was clipped. Built here once per status snapshot, as lines
// in drawing order, the tree can go through the same ImGuiListClipper.
//
// Header-only and free of ImGui, so the shape of the tree can be tested without
// the panel (which the test binary does not build).

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace HE::Ed::ChangeTree
{
	struct Line
	{
		int         depth = 0;    // indent level; a file sits one level under its folder
		int         row   = -1;   // the file's index into the rows; -1: a folder line
		int         end   = 0;    // folder: the index one past its last line (its subtree)
		std::string label;        // folder: the name, single-child chains folded ("Content/Meshes")
		std::string key;          // folder: its full path, what its fold state is kept under
	};

	namespace detail
	{
		struct Node
		{
			std::map<std::string, Node> dirs;   // by name: folders in name order
			std::vector<int>            files;  // row indices, in row order
		};

		inline void emit(const std::string& name, const std::string& path, const Node& node,
		                 int depth, std::vector<Line>& out)
		{
			// Single-child chains fold into one line ("Content/Meshes/Props"), the
			// way VS Code does it.
			const Node* n = &node;
			std::string label = name, key = path;
			while (n->files.empty() && n->dirs.size() == 1)
			{
				label += "/" + n->dirs.begin()->first;
				key   += "/" + n->dirs.begin()->first;
				n = &n->dirs.begin()->second;
			}
			const std::size_t at = out.size();
			Line folder;
			folder.depth = depth;
			folder.label = std::move(label);
			folder.key   = key;
			out.push_back(std::move(folder));
			for (const auto& [sub, child] : n->dirs) emit(sub, key + "/" + sub, child, depth + 1, out);
			for (int r : n->files) { Line f; f.depth = depth + 1; f.row = r; out.push_back(f); }
			out[at].end = static_cast<int>(out.size());
		}
	}

	// The lines for `count` rows whose repo-relative paths `pathOf(i)` gives,
	// folders before files at every level, files in row order. The same order and
	// folding the nested TreeNodeEx calls had.
	template <class PathOf>
	std::vector<Line> build(int count, PathOf&& pathOf)
	{
		detail::Node root;
		for (int i = 0; i < count; ++i)
		{
			const std::string& p = pathOf(i);
			detail::Node* node = &root;
			std::size_t start = 0;
			while (true)
			{
				const std::size_t slash = p.find('/', start);
				if (slash == std::string::npos) { node->files.push_back(i); break; }
				node = &node->dirs[p.substr(start, slash - start)];
				start = slash + 1;
			}
		}
		std::vector<Line> out;
		for (const auto& [name, sub] : root.dirs) detail::emit(name, name, sub, 0, out);
		for (int r : root.files) { Line f; f.row = r; out.push_back(f); }
		return out;
	}

	// The lines on screen: everything except what a folded folder holds. Into a
	// caller-owned vector, so a frame reuses its capacity.
	inline void visible(const std::vector<Line>& lines, const std::unordered_set<std::string>& folded,
	                    std::vector<int>& out)
	{
		out.clear();
		for (int i = 0; i < static_cast<int>(lines.size()); )
		{
			out.push_back(i);
			const Line& l = lines[static_cast<std::size_t>(i)];
			i = (l.row < 0 && folded.count(l.key)) ? l.end : i + 1;
		}
	}
}
