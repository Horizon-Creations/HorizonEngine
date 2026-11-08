#pragma once
#include <string>
#include <vector>

// The editor's Hardware page (Preferences » Preview » Hardware, Thema 337): what machine this is, as
// the engine detected it - the same values the hardware.* script rows hand a game for its own settings
// screen. Read-only; the one control copies the summary for a bug report or a forum post.
//
// A value the machine could not tell shows as "-", never as a made-up number. Nothing here is sent
// anywhere.
namespace HardwarePage
{
    struct Row
    {
        std::string group;   // "Processor", "Memory", "Graphics", "Display", "System"
        std::string label;   // "Name", "Cores", "Video memory", ...
        std::string value;   // already formatted, "-" when unknown
    };

    // Every row in display order, formatted. Live values (free memory, battery) are read now.
    std::vector<Row> rows();

    // Draws the table and the copy button into the current ImGui window.
    void draw();
}
