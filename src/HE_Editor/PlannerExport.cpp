#include "PlannerExport.h"

#include "TaskBoard.h"   // uuidToHex, taskTimeToIso

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>

namespace HE::Planner
{

namespace
{

void put16(std::string& out, std::uint32_t v)
{
	out += static_cast<char>(v & 0xFF);
	out += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& out, std::uint32_t v)
{
	put16(out, v & 0xFFFF);
	put16(out, v >> 16);
}

std::string xmlEscape(std::string_view s)
{
	std::string out;
	out.reserve(s.size());
	for (const char c : s)
		switch (c)
		{
		case '&':  out += "&amp;";  break;
		case '<':  out += "&lt;";   break;
		case '>':  out += "&gt;";   break;
		case '"':  out += "&quot;"; break;
		default:
			// Control characters other than tab are not allowed in XML 1.0.
			if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') out += c;
		}
	return out;
}

// ── .docx ────────────────────────────────────────────────────────────────────

std::string run(std::string_view text, bool code = false)
{
	std::string r = "<w:r>";
	if (code) r += "<w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/><w:sz w:val=\"18\"/></w:rPr>";
	r += "<w:t xml:space=\"preserve\">" + xmlEscape(text) + "</w:t></w:r>";
	return r;
}

struct DocxWriter
{
	const MarkdownContext& ctx;
	std::string body;

	void paragraph(const std::string& runs, int depth, const char* style = nullptr, const std::string& extraPPr = {})
	{
		body += "<w:p><w:pPr>";
		if (style) body += std::string("<w:pStyle w:val=\"") + style + "\"/>";
		body += extraPPr;
		if (depth > 0) body += "<w:ind w:left=\"" + std::to_string(360 * depth) + "\"/>";
		body += "</w:pPr>" + runs + "</w:p>";
	}

	void blocks(const std::vector<Block>& list, int depth)
	{
		int number = 0;
		for (const Block& b : list)
		{
			number = b.type == BlockType::Numbered ? number + 1 : 0;
			switch (b.type)
			{
			case BlockType::Heading1: paragraph(run(b.text), depth, "Heading1"); break;
			case BlockType::Heading2: paragraph(run(b.text), depth, "Heading2"); break;
			case BlockType::Heading3: paragraph(run(b.text), depth, "Heading3"); break;
			case BlockType::Bullet:   paragraph(run("\xE2\x80\xA2  " + b.text), depth + 1); break;
			case BlockType::Numbered: paragraph(run(std::to_string(number) + ".  " + b.text), depth + 1); break;
			case BlockType::Todo:     paragraph(run((b.checked ? "\xE2\x98\x91  " : "\xE2\x98\x90  ") + b.text), depth + 1); break;
			case BlockType::Quote:    paragraph(run(b.text), depth, "Quote"); break;
			case BlockType::Callout:  paragraph(run(b.text), depth, "Callout"); break;
			case BlockType::Divider:
				paragraph({}, depth, nullptr, "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"999999\"/></w:pBdr>");
				break;
			case BlockType::Code:
			{
				// One paragraph, the lines joined by breaks, so the shading is one box.
				std::string runs;
				std::size_t at = 0;
				while (true)
				{
					const std::size_t nl = b.text.find('\n', at);
					runs += run(std::string_view(b.text).substr(at, nl == std::string::npos ? std::string::npos : nl - at), true);
					if (nl == std::string::npos) break;
					runs += "<w:r><w:br/></w:r>";
					at = nl + 1;
				}
				paragraph(runs, depth, "Code");
				break;
			}
			case BlockType::EngineLink:
				paragraph(run("[" + (b.link.label.empty() ? std::string("Engine object") : b.link.label) + "]"), depth);
				break;
			case BlockType::PageLink:
			{
				const std::string title = ctx.pageTitle ? ctx.pageTitle(b.page) : std::string{};
				paragraph(run("\xE2\x86\x92 " + (title.empty() ? std::string("Page") : title)), depth);
				break;
			}
			case BlockType::DatabaseView:
			{
				const std::string name = ctx.databaseName ? ctx.databaseName(b.database) : std::string{};
				paragraph(run("[" + (name.empty() ? std::string("Table") : name) + "]"), depth);
				break;
			}
			default: paragraph(run(b.text), depth); break;
			}
			blocks(b.children, depth + 1);
		}
	}
};

const char* const kDocxContentTypes =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
	"<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
	"<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
	"<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
	"<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
	"</Types>";

const char* const kDocxRels =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
	"</Relationships>";

const char* const kDocxDocumentRels =
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	"<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
	"</Relationships>";

std::string styleXml(const char* id, const char* name, const char* pPr, const char* rPr, bool heading, int level = 0)
{
	// pPr children in the schema's order (CT_PPrBase): keepNext ... pBdr, shd, spacing, ind ... outlineLvl.
	std::string s = std::string("<w:style w:type=\"paragraph\" w:styleId=\"") + id + "\"><w:name w:val=\"" + name + "\"/>"
	              "<w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr>";
	if (heading) s += "<w:keepNext/>";
	s += pPr;
	if (heading) s += "<w:outlineLvl w:val=\"" + std::to_string(level) + "\"/>";
	s += std::string("</w:pPr><w:rPr>") + rPr + "</w:rPr></w:style>";
	return s;
}

std::string docxStyles()
{
	std::string s =
		"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		"<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
		"<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:cs=\"Calibri\"/><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault>"
		"<w:pPrDefault><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:pPrDefault></w:docDefaults>"
		"<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>";
	s += styleXml("Title", "Title", "<w:spacing w:after=\"240\"/>", "<w:b/><w:sz w:val=\"48\"/>", false);
	s += styleXml("Heading1", "heading 1", "<w:spacing w:before=\"360\" w:after=\"120\"/>", "<w:b/><w:sz w:val=\"36\"/>", true, 0);
	s += styleXml("Heading2", "heading 2", "<w:spacing w:before=\"240\" w:after=\"80\"/>", "<w:b/><w:sz w:val=\"30\"/>", true, 1);
	s += styleXml("Heading3", "heading 3", "<w:spacing w:before=\"200\" w:after=\"60\"/>", "<w:b/><w:sz w:val=\"26\"/>", true, 2);
	s += styleXml("Quote", "Quote", "<w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"E8A33D\"/></w:pBdr><w:ind w:left=\"360\"/>",
	              "<w:i/><w:color w:val=\"555555\"/>", false);
	s += styleXml("Callout", "Callout", "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F4EEDF\"/><w:ind w:left=\"120\" w:right=\"120\"/>", "", false);
	s += styleXml("Code", "Code", "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/><w:spacing w:after=\"160\"/>",
	              "<w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/>", false);
	s += "</w:styles>";
	return s;
}

// ── .ics ─────────────────────────────────────────────────────────────────────

std::string icsDate(std::int64_t day)
{
	const std::string iso = taskTimeToIso(day * 86'400'000LL);   // "2026-10-20T00:00:00.000Z"
	return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2);
}

std::string icsStamp(std::int64_t ms)
{
	const std::string iso = taskTimeToIso(ms);   // "2026-10-20T09:12:03.120Z" -> "20261020T091203Z"
	return iso.substr(0, 4) + iso.substr(5, 2) + iso.substr(8, 2) + "T" + iso.substr(11, 2) + iso.substr(14, 2) + iso.substr(17, 2) + "Z";
}

// RFC 5545 text: backslash, semicolon and comma escaped, new lines as \n.
std::string icsText(std::string_view s)
{
	std::string out;
	for (const char c : s)
	{
		if (c == '\\' || c == ';' || c == ',') { out += '\\'; out += c; }
		else if (c == '\n')                    out += "\\n";
		else if (c != '\r')                    out += c;
	}
	return out;
}

// A content line, folded at 75 octets (never inside a UTF-8 sequence), CRLF ended.
void icsLine(std::string& out, const std::string& line)
{
	std::size_t at = 0, width = 75;
	while (line.size() - at > width)
	{
		std::size_t cut = at + width;
		while (cut > at && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
		out += line.substr(at, cut - at) + "\r\n ";
		at = cut;
		width = 74;   // the leading space of a continuation counts
	}
	out += line.substr(at) + "\r\n";
}

} // namespace

std::uint32_t crc32(std::string_view data)
{
	static const std::array<std::uint32_t, 256> table = [] {
		std::array<std::uint32_t, 256> t{};
		for (std::uint32_t i = 0; i < 256; ++i)
		{
			std::uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			t[i] = c;
		}
		return t;
	}();
	std::uint32_t c = 0xFFFFFFFFu;
	for (const char ch : data) c = table[(c ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

std::string zipStored(const std::vector<std::pair<std::string, std::string>>& files)
{
	// DOS time 1980-01-01 00:00: the content, not the moment, decides the bytes.
	constexpr std::uint32_t kDosTime = 0, kDosDate = (0 << 9) | (1 << 5) | 1;
	std::string out, central;
	for (const auto& [name, data] : files)
	{
		const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
		const std::uint32_t crc = crc32(data), size = static_cast<std::uint32_t>(data.size());
		auto header = [&](std::string& h, bool isCentral) {
			put32(h, isCentral ? 0x02014b50u : 0x04034b50u);
			if (isCentral) put16(h, 20);   // made by
			put16(h, 20);                  // needed to extract
			put16(h, 0x0800);              // UTF-8 names
			put16(h, 0);                   // stored
			put16(h, kDosTime);
			put16(h, kDosDate);
			put32(h, crc);
			put32(h, size);
			put32(h, size);
			put16(h, static_cast<std::uint32_t>(name.size()));
			put16(h, 0);                   // extra
			if (isCentral)
			{
				put16(h, 0);               // comment
				put16(h, 0);               // disk
				put16(h, 0);               // internal attributes
				put32(h, 0);               // external attributes
				put32(h, offset);
			}
			h += name;
		};
		header(out, false);
		out += data;
		header(central, true);
	}
	const std::uint32_t centralAt = static_cast<std::uint32_t>(out.size());
	out += central;
	put32(out, 0x06054b50u);
	put16(out, 0);
	put16(out, 0);
	put16(out, static_cast<std::uint32_t>(files.size()));
	put16(out, static_cast<std::uint32_t>(files.size()));
	put32(out, static_cast<std::uint32_t>(central.size()));
	put32(out, centralAt);
	put16(out, 0);
	return out;
}

std::string pageToDocx(const Page& page, const MarkdownContext& ctx)
{
	DocxWriter w{ ctx, {} };
	w.paragraph(run(page.title.empty() ? std::string("Untitled") : page.title), 0, "Title");
	w.blocks(page.blocks, 0);
	const std::string document =
		"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		"<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" + w.body +
		"<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" "
		"w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";
	return zipStored({ { "[Content_Types].xml", kDocxContentTypes },
	                   { "_rels/.rels", kDocxRels },
	                   { "word/_rels/document.xml.rels", kDocxDocumentRels },
	                   { "word/document.xml", document },
	                   { "word/styles.xml", docxStyles() } });
}

std::string eventsToIcs(const std::vector<CalendarEvent>& events, std::string_view calendarName, std::int64_t nowMs)
{
	std::string out;
	icsLine(out, "BEGIN:VCALENDAR");
	icsLine(out, "VERSION:2.0");
	icsLine(out, "PRODID:-//Horizon Engine//Planner//EN");
	icsLine(out, "CALSCALE:GREGORIAN");
	if (!calendarName.empty()) icsLine(out, "X-WR-CALNAME:" + icsText(calendarName));
	for (const CalendarEvent& e : events)
	{
		icsLine(out, "BEGIN:VEVENT");
		icsLine(out, "UID:" + uuidToHex(e.id) + "@planner.horizonengine");
		icsLine(out, "DTSTAMP:" + icsStamp(nowMs));
		// All-day: DTEND is the day after the last one (it is exclusive).
		icsLine(out, "DTSTART;VALUE=DATE:" + icsDate(e.firstDay));
		icsLine(out, "DTEND;VALUE=DATE:" + icsDate(std::max(e.firstDay, e.lastDay) + 1));
		icsLine(out, "SUMMARY:" + icsText(e.title.empty() ? std::string("Untitled") : e.title));
		if (!e.description.empty()) icsLine(out, "DESCRIPTION:" + icsText(e.description));
		icsLine(out, "END:VEVENT");
	}
	icsLine(out, "END:VCALENDAR");
	return out;
}

std::filesystem::path exportFileFor(const std::filesystem::path& projectRoot, const std::string& title,
                                    const char* ext, const char* fallback)
{
	std::string name;
	for (const char c : title)
		name += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || (static_cast<unsigned char>(c) & 0x80)) ? c : '_';
	while (!name.empty() && name.back() == ' ') name.pop_back();
	if (name.empty()) name = fallback;
	return projectRoot / "Export" / "Planner" / (name + "." + ext);
}

bool writeExport(const std::filesystem::path& file, const std::string& bytes, std::string* error)
{
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	std::ofstream f(file, std::ios::binary | std::ios::trunc);
	if (f) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if (!f)
	{
		if (error) *error = "could not write " + file.string();
		return false;
	}
	return true;
}

} // namespace HE::Planner
