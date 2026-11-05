#include "Project/QualityGovernor.h"

#include <algorithm>
#include <cmath>

namespace HE {

namespace
{
constexpr float kSmoothingSec    = 0.5f;
constexpr float kMaxFrameTargets = 4.0f;
}

void QualityGovernor::configure(const QualityGovernorConfig& cfg, int startLevel)
{
    m_cfg = cfg;
    m_cfg.targetFps = std::clamp(m_cfg.targetFps, 10.0f, 1000.0f);
    m_cfg.minLevel  = std::clamp(m_cfg.minLevel, 0, 3);
    m_cfg.maxLevel  = std::clamp(m_cfg.maxLevel, m_cfg.minLevel, 3);
    m_level  = std::clamp(startLevel, m_cfg.minLevel, m_cfg.maxLevel);
    m_avg    = 0.0f;
    m_over   = m_under = 0.0f;
    m_cool   = 0.0f;
    m_clock  = 0.0;
    for (double& b : m_blockedUntil) b = 0.0;
}

bool QualityGovernor::update(float realDt)
{
    if (!(realDt > 0.0f)) return false;           // also catches NaN
    m_clock += realDt;
    if (m_cool > 0.0f) m_cool -= realDt;
    if (realDt >= kHitchSec) return false;        // a load or a stall: not a measurement

    // The smoothed frame time: first frame seeds it, the rest blend in with a ~0.5 s
    // time constant whatever the frame rate is.
    // One frame counts at most kMaxFrameTargets targets: a single 0.3 s hitch would
    // otherwise keep the average above the limit for ~2 s while it decays, and that alone
    // is enough to lower the quality. A game that really runs at 15 fps still averages
    // well above the limit, only the first frame of a stall loses its weight.
    const float target = 1.0f / m_cfg.targetFps;
    const float sample = std::min(realDt, target * kMaxFrameTargets);
    if (m_avg <= 0.0f) m_avg = sample;
    else               m_avg += (sample - m_avg) * (1.0f - std::exp(-realDt / kSmoothingSec));

    // While a step is being measured nothing counts, in either direction.
    if (m_cool > 0.0f) { m_over = m_under = 0.0f; return false; }

    if (m_avg > target * m_cfg.missMargin)
    {
        m_over += realDt;
        m_under = 0.0f;
    }
    else if (m_avg <= target * m_cfg.hitMargin)
    {
        m_under += realDt;
        m_over = 0.0f;
    }
    else
    {
        m_over = m_under = 0.0f;                  // on the edge: neither clock runs
    }

    if (m_over >= m_cfg.lowerAfterSec && m_level > m_cfg.minLevel)
    {
        // The level we are leaving did not hold the target: keep away from it for a while.
        m_blockedUntil[m_level] = m_clock + m_cfg.failBlockSec;
        --m_level;
        m_over = m_under = 0.0f;
        m_cool = m_cfg.cooldownSec;
        return true;
    }
    if (m_under >= m_cfg.raiseAfterSec && m_level < m_cfg.maxLevel
        && m_clock >= m_blockedUntil[m_level + 1])
    {
        ++m_level;
        m_over = m_under = 0.0f;
        m_cool = m_cfg.cooldownSec;
        return true;
    }
    // Missing at the floor, or meeting with the next level blocked: nothing to do, and the
    // counters keep running so the step happens the moment it is allowed.
    if (m_over >= m_cfg.lowerAfterSec) m_over = m_cfg.lowerAfterSec;
    if (m_under >= m_cfg.raiseAfterSec) m_under = m_cfg.raiseAfterSec;
    return false;
}

} // namespace HE
