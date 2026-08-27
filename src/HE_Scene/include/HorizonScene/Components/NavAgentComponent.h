#pragma once
#include <Math/Math.h>
#include <vector>

struct NavAgentComponent {
    glm::vec3 targetPos      = glm::vec3(0.0f);
    float     speed          = 3.5f;
    float     stoppingDist   = 0.1f;

    // Walk to targetPos as soon as the simulation starts, with nothing having to
    // say so. Authored and serialized, unlike `moving` below — a packaged game
    // has no Inspector "Go" button, so an agent whose only possible starter was
    // that button could never move once the editor was gone.
    bool      autoStart      = false;

    // ── Runtime state — rebuilt every session, never serialized ──────────────
    std::vector<glm::vec3> path;      // world-space waypoints
    size_t                 pathIdx  = 0;
    bool                   hasPath  = false;
    bool                   moving   = false;

    // The goal the current path was planned for. NavigationSystem compares
    // targetPos against it and re-plans once they differ by more than a step,
    // which is what the "reset when targetPos changes" this header used to
    // claim, and never did.
    glm::vec3 pathTarget    = glm::vec3(0.0f);

    // autoStart is a one-shot: latched the first time the agent is ticked with a
    // running simulation, so stopping the agent does not immediately restart it.
    bool      autoStarted   = false;

    // True while this system is the one writing the character's velocity. The
    // frame it goes false, that velocity has to be written back to zero: Jolt
    // keeps the last value it was given, so an agent that simply stopped being
    // steered would glide on at its walking speed forever.
    bool      drivingCharacter = false;
};
