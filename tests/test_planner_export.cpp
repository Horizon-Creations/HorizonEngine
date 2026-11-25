// The Planner's exports: a page as a Word document (a stored zip of WordprocessingML)
// and dated rows as iCalendar. What is checked is the format, byte for byte where a
// reader depends on it: the zip's CRC and directory, the docx parts and styles, the
// .ics all-day dates (exclusive end), UIDs, escaping, CRLF and line folding.
#include "doctest.h"

#include "PlannerExport.h"

#include <cstring>
#include <string>

using namespace HE;
using namespace HE::Planner;

namespace
{
	std::uint32_t le32(const std::string& s, std::size_t at)
	{
		return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) |
		       static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8 |
		       static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16 |
		       static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24;
	}
	std::uint32_t le16(const std::string& s, std::size_t at) { return le32(s + std::string(2, '\0'), at) & 0xFFFF; }

	// The stored files of a zip, read back from its central directory.
	std::vector<std::pair<std::string, std::string>> unzip(const std::string& z)
	{
		std::vector<std::pair<std::string, std::string>> out;
		REQUIRE(z.size() >= 22);
		const std::size_t eocd = z.size() - 22;
		REQUIRE(le32(z, eocd) == 0x06054b50u);
		const std::uint32_t count = le16(z, eocd + 10);
		std::size_t at = le32(z, eocd + 16);
		for (std::uint32_t i = 0; i < count; ++i)
		{
			REQUIRE(le32(z, at) == 0x02014b50u);
			const std::uint32_t crc = le32(z, at + 16), size = le32(z, at + 20);
			const std::uint32_t nameLen = le16(z, at + 28), local = le32(z, at + 42);
			const std::string name = z.substr(at + 46, nameLen);
			REQUIRE(le32(z, local) == 0x04034b50u);
			const std::size_t data = local + 30 + le16(z, local + 26) + le16(z, local + 28);
			const std::string bytes = z.substr(data, size);
			CHECK(crc32(bytes) == crc);
			out.emplace_back(name, bytes);
			at += 46 + nameLen;
		}
		return out;
	}

	std::string part(const std::vector<std::pair<std::string, std::string>>& files, const std::string& name)
	{
		for (const auto& [n, b] : files)
			if (n == name) return b;
		return {};
	}
}

TEST_CASE("planner export: crc32 is zip's")
{
	CHECK(crc32("123456789") == 0xCBF43926u);
	CHECK(crc32("") == 0u);
}

TEST_CASE("planner export: a page as .docx has the parts Word needs, headings as Word's headings, code and lists")
{
	Page page;
	page.title = "Game & design";
	page.addBlock(BlockType::Heading1, "Pillars", 1);
	page.addBlock(BlockType::Bullet, "Fast <movement>", 1);
	page.addBlock(BlockType::Code, "int x = 1;\nreturn x;", 1)->language = "cpp";
	Block* todo = page.addBlock(BlockType::Todo, "Ship", 1);
	todo->checked = true;
	page.addBlock(BlockType::Divider, "", 1);

	const std::string docx = pageToDocx(page);
	REQUIRE(docx.compare(0, 4, "PK\x03\x04") == 0);
	const auto files = unzip(docx);
	CHECK(files.size() == 5);
	CHECK(files[0].first == "[Content_Types].xml");   // first, as readers expect
	CHECK(part(files, "_rels/.rels").find("word/document.xml") != std::string::npos);
	CHECK(part(files, "word/_rels/document.xml.rels").find("styles.xml") != std::string::npos);
	CHECK(part(files, "word/styles.xml").find("w:styleId=\"Heading1\"") != std::string::npos);
	// pPr children in the schema's order: Word refuses a file where they are not.
	CHECK(part(files, "word/styles.xml").find("<w:pPr><w:keepNext/><w:spacing") != std::string::npos);
	CHECK(part(files, "word/styles.xml").find("</w:pBdr><w:ind") != std::string::npos);

	const std::string doc = part(files, "word/document.xml");
	CHECK(doc.find("Game &amp; design") != std::string::npos);
	CHECK(doc.find("<w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t xml:space=\"preserve\">Pillars</w:t>") != std::string::npos);
	CHECK(doc.find("Fast &lt;movement&gt;") != std::string::npos);
	CHECK(doc.find("int x = 1;</w:t></w:r><w:r><w:br/></w:r>") != std::string::npos);
	CHECK(doc.find("Courier New") != std::string::npos);
	CHECK(doc.find("\xE2\x98\x91  Ship") != std::string::npos);
	CHECK(doc.find("<w:pBdr><w:bottom") != std::string::npos);
}

TEST_CASE("planner export: dated rows as .ics are all-day events with an exclusive end, a stable UID and escaped text")
{
	CalendarEvent one;
	one.id = UUID{ 0x1234, 0x5678 };
	one.title = "Alpha, beta; gamma";
	one.firstDay = 20746;   // 2026-10-20
	one.lastDay = 20748;    // 2026-10-22
	one.description = "Line one\nLine two";
	CalendarEvent day;
	day.id = UUID{ 1, 2 };
	day.title = std::string(100, 'x');
	day.firstDay = day.lastDay = 20746;

	const std::string ics = eventsToIcs({ one, day }, "Roadmap", 1'790'000'000'000LL);
	CHECK(ics.rfind("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n", 0) == 0);
	CHECK(ics.find("X-WR-CALNAME:Roadmap\r\n") != std::string::npos);
	CHECK(ics.find("DTSTART;VALUE=DATE:20261020\r\nDTEND;VALUE=DATE:20261023\r\n") != std::string::npos);
	CHECK(ics.find("SUMMARY:Alpha\\, beta\\; gamma\r\n") != std::string::npos);
	CHECK(ics.find("DESCRIPTION:Line one\\nLine two\r\n") != std::string::npos);
	CHECK(ics.find("DTSTART;VALUE=DATE:20261020\r\nDTEND;VALUE=DATE:20261021\r\n") != std::string::npos);   // one day
	CHECK(ics.find("@planner.horizonengine\r\n") != std::string::npos);
	CHECK(ics.find("DTSTAMP:") != std::string::npos);
	// Lines longer than 75 octets fold: CRLF and a space.
	CHECK(ics.find("\r\n x") != std::string::npos);
	for (std::size_t at = 0, nl; (nl = ics.find("\r\n", at)) != std::string::npos; at = nl + 2)
		CHECK(nl - at <= 75);
	CHECK(ics.size() >= 15);
	CHECK(ics.compare(ics.size() - 15, 15, "END:VCALENDAR\r\n") == 0);
}

TEST_CASE("planner export: file names under Export/Planner")
{
	CHECK(exportFileFor("/p", "Game: design?", "md", "Page") == std::filesystem::path("/p/Export/Planner/Game_ design_.md"));
	CHECK(exportFileFor("/p", "  ", "ics", "Calendar") == std::filesystem::path("/p/Export/Planner/Calendar.ics"));
}
