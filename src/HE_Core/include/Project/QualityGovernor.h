#pragma once
#include <Types/Defines.h>

namespace HE {

// ─── Automatic quality ────────────────────────────────────────────────────────
// A controller that moves the OVERALL quality level (Low 0 … Ultra 3, the numbers of
// HE::QualityLevel) so the game keeps its target frame rate: down when it misses
// the target for a while, up again when it has been meeting it for a long while.
// No engine types, no clock of its own: the caller feeds it one wall-clock frame
// time per frame, so it is deterministic and testable with a list of numbers.
//
// What it is NOT: a per-frame LOD. A step is a whole level (shadow resolution, AA,
// reflections, material quality …), so steps are rare and slow on purpose — a quality
// that changes every second is worse than one that is a notch too low.
//
// The rules:
//  * The frame time is smoothed (≈0.5 s). A single long frame — a streaming hitch, a
//    scene load — must not cost quality: a frame longer than 0.5 s is ignored.
//  * MISSING = smoothed frame time above target × missMargin. After lowerAfterSec of
//    it, step down one level.
//  * MEETING = at or below target × hitMargin. After raiseAfterSec of it, step up one
//    level. "Meeting", not "well under": with VSync on, a game that has headroom still
//    shows exactly the target frame time, and would never be seen to have any.
//  * Between the two the clock for both stands still, so a game sitting on the edge
//    does not drift either way.
//  * After every step the new level is measured first (cooldownSec).
//  * A level that was left because it missed the target is not tried again for
//    failBlockSec. Without that, "meeting the target one level down" would send it
//    straight back up every raiseAfterSec, and the picture would pump.
struct HE_API QualityGovernorConfig
{
    float targetFps      = 60.0f;
    int   minLevel       = 0;       // Low
    int   maxLevel       = 3;       // Ultra
    float lowerAfterSec  = 2.0f;
    float raiseAfterSec  = 15.0f;
    float missMargin     = 1.12f;
    float hitMargin      = 1.05f;
    float cooldownSec    = 3.0f;
    float failBlockSec   = 60.0f;
};

class HE_API QualityGovernor
{
public:
    // Starts over at `startLevel` (clamped into [minLevel, maxLevel]).
    void configure(const QualityGovernorConfig& cfg, int startLevel);

    // One frame of `realDt` seconds of WALL time (not scaled by pause or slow motion).
    // True when the level changed this frame; level() is then the new one.
    bool update(float realDt);

    int   level() const { return m_level; }
    float smoothedFrameTime() const { return m_avg; }   // seconds, 0 before the first frame
    const QualityGovernorConfig& config() const { return m_cfg; }

    // A frame longer than this is a hitch, not a measurement.
    static constexpr float kHitchSec = 0.5f;

private:
    QualityGovernorConfig m_cfg;
    int    m_level   = 2;
    float  m_avg     = 0.0f;
    float  m_over    = 0.0f;   // seconds spent missing the target
    float  m_under   = 0.0f;   // seconds spent meeting it
    float  m_cool    = 0.0f;   // seconds left before the next step may happen
    double m_clock   = 0.0;    // seconds since configure()
    double m_blockedUntil[4] = { 0.0, 0.0, 0.0, 0.0 };   // by level that was LEFT
};

} // namespace HE
