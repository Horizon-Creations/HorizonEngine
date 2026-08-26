#include "EditorReference.h"

#include "DocsLibrary.h"
#include "EditorHelp.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace HE::Ed::EditorReference
{
namespace
{
	namespace Docs = HE::Ed::Docs;

	Docs::Run run(std::string text, Docs::Style style = Docs::Style::Body,
	              std::string href = {})
	{
		Docs::Run r;
		r.text  = std::move(text);
		r.style = style;
		r.href  = std::move(href);
		return r;
	}

	Docs::Block paragraph(std::string text)
	{
		Docs::Block b;
		b.kind = Docs::BlockKind::Paragraph;
		b.runs.push_back(run(std::move(text)));
		return b;
	}

	// The label a reader sees on the control, from the key. For a scoped key
	// ("Rigid Body/Mass") the part after the slash IS the label — that is what
	// the lookup matches on, so it is what the panel prints.
	std::string labelOf(const Help::Entry& e)
	{
		if (e.title && e.title[0]) return e.title;
		const std::string key(e.key);
		// The LAST slash, not the first: a key can carry more than one level of
		// scope ("Preferences/Post-Processing/Bloom Threshold"), and what the
		// section is called is the label at the end of it.
		const std::size_t slash = key.rfind('/');
		return slash == std::string::npos ? key : key.substr(slash + 1);
	}

	// Where the control sits, spelled for someone who has to find it. The group
	// is the panel or component; for a scoped key that is the component's own
	// name, which is what the Details panel labels the section with.
	std::string whereOf(const Help::Entry& e, const Help::Area& area)
	{
		return area.group;
	}
} // namespace

void install(Docs::Library& lib)
{
	// Collect the entries per page first: the table is written in the order the
	// editor is built, and a page has to come out in one piece.
	struct Row { const Help::Entry* entry; const Help::Area* area; };
	std::map<std::string, std::vector<Row>> byPage;
	std::map<std::string, std::string>      pageTitle;

	for (int i = 0; i < Help::entryCount(); ++i)
	{
		const Help::Entry& e = Help::entryAt(i);
		const Help::Area* a = Help::areaOf(e.key);
		if (!a) continue;   // no home: the coverage test is what reports this
		byPage[a->page].push_back({ &e, a });
		pageTitle[a->page] = a->title;
	}

	for (auto& [pageId, rows] : byPage)
	{
		Docs::Page page;
		page.id    = pageId;
		page.title = pageTitle[pageId];
		page.summary =
			"Every control in this part of the editor, with what it does. The same "
			"sentence appears when you rest the cursor on it; F1 there opens the "
			"entry.";

		// Grouped by where the control sits, so the page reads in the order
		// somebody walks the panel — and so the sidebar can fold it.
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
			const int g = std::string(a.area->group).compare(b.area->group);
			return g != 0 ? g < 0 : std::string(a.entry->key) < std::string(b.entry->key);
		});

		for (const Row& r : rows)
		{
			const Help::Entry& e = *r.entry;
			Docs::Section sec;
			std::string conceptLabel;
			// The anchor is the key with the slash flattened — the same one
			// Help::referenceTopic builds, which is what F1 hands the reader.
			sec.id = e.key;
			std::replace(sec.id.begin(), sec.id.end(), '/', '.');
			sec.title   = labelOf(e);
			sec.eyebrow = whereOf(e, *r.area);
			sec.blocks.push_back(paragraph(e.body));

			// The shortcut, where there is one — in the same words the menus use.
			if (e.shortcut && e.shortcut[0])
			{
				Docs::Block b;
				b.kind = Docs::BlockKind::Paragraph;
				b.runs.push_back(run("Shortcut: ", Docs::Style::Body));
				b.runs.push_back(run(Help::shortcutLabel(e.shortcut), Docs::Style::Code));
				sec.blocks.push_back(std::move(b));
			}

			// And the concept behind it, as a link rather than a second
			// explanation. Two descriptions of one idea drift; a link cannot.
			//
			// The link is named after where it GOES — resolved here, against the
			// library the page is being added to. "More about this: the chapter
			// it belongs to" is a link that tells the reader nothing about
			// whether it is worth following.
			if (e.topic && e.topic[0])
			{
				int page = -1, section = -1;
				std::string label;
				if (lib.resolve(e.topic, page, section))
				{
					const Docs::Page& p = lib.pages()[static_cast<std::size_t>(page)];
					label = section >= 0
					      ? p.sections[static_cast<std::size_t>(section)].title
					      : p.title;
				}
				if (!label.empty())
				{
					Docs::Block b;
					b.kind = Docs::BlockKind::Paragraph;
					b.runs.push_back(run("More about this: ", Docs::Style::Body));
					b.runs.push_back(run(label, Docs::Style::Link, e.topic));
					sec.blocks.push_back(std::move(b));
					// Kept for the search text below, which is assembled after
					// the blocks — appending to sec.text here would be writing
					// into a string that is about to be overwritten.
					conceptLabel = label;
				}
			}

			sec.text = sec.title + " " + sec.eyebrow + " " + e.body + " " + e.key
			         + " " + conceptLabel;
			page.sections.push_back(std::move(sec));
		}
		lib.appendPage(std::move(page));
	}
}

} // namespace HE::Ed::EditorReference
