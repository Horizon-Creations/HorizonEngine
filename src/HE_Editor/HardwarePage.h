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

    // The graphics-card drop-down (Thema 337): "Default (system)" and every adapter of the machine, the choice that
    // is saved selected. A saved choice whose card is gone shows as its own entry ("... (not present)"): the game
    // falls back to the default card then, and the page says so instead of silently dropping the choice.
    struct GpuChoiceOption
    {
        std::string id;      // "" = the system's default adapter
        std::string label;
    };
    struct GpuChoice
    {
        std::vector<GpuChoiceOption> options;   // [0] is always the default
        int         selected  = 0;
        bool        canChoose = true;           // false when this run's graphics backend cannot pick a card (OpenGL)
        std::string note;                       // what is true about the choice right now, "" = nothing to add
    };
    GpuChoice gpuChoice();

    // Draws the table, the graphics-card choice and the copy button into the current ImGui window.
    void draw();
}
