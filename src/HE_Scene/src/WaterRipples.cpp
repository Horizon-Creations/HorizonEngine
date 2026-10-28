#include <HorizonScene/WaterRipples.h>
#include <HorizonScene/PhysicsWorld.h>
#include <Renderer/IRenderer.h>
#include <algorithm>
#include <cmath>

namespace HE::water
{
    float RippleField::effectiveStrength(const Ripple& r)
    {
        return r.strength * std::exp(-r.age / kDecayTime);
    }

    void RippleField::add(const Ripple& in)
    {
        // Not a number, or nothing to show: ignore it rather than carry a NaN into the shader.
        if (!std::isfinite(in.pos.x) || !std::isfinite(in.pos.y) || !std::isfinite(in.radius) ||
            !std::isfinite(in.strength) || !std::isfinite(in.age) || !std::isfinite(in.drift.x) ||
            !std::isfinite(in.drift.y))
            return;
        Ripple r   = in;
        r.strength = std::clamp(r.strength, 0.0f, 1.0f);
        r.radius   = std::clamp(r.radius, 0.05f, 6.0f);
        r.age      = std::max(r.age, 0.0f);
        if (effectiveStrength(r) < kMinStrength)
            return;

        // A young ring of the same kind right here: refresh it. The stronger of the two
        // wins (the old one is already part-faded), and it starts over from the new
        // radius and place, so two crates landing together make one ring, not two. Not a
        // trail: a body moving through the water leaves a line of rings behind it, one every
        // kTrailInterval, and merging them would pin one ring to the body instead (their
        // rate is limited per body, and the list is bounded, so they cannot flood it).
        for (int i = 0; i < m_count && r.kind != WaterRippleTrail; ++i)
        {
            Ripple& o = m_rings[i];
            if (o.kind != r.kind || o.age > kMergeAge)
                continue;
            const glm::vec2 d = o.pos - r.pos;
            if (d.x * d.x + d.y * d.y > kMergeDistance * kMergeDistance)
                continue;
            // Strength is stored unfaded and the merged ring takes r.age, so the old ring's
            // strength now is carried back out to that age; the newcomer's own is already there.
            const float oldNow = effectiveStrength(o);
            o.strength = oldNow > effectiveStrength(r) ? oldNow * std::exp(r.age / kDecayTime) : r.strength;
            o.radius   = std::max(o.radius, r.radius);
            o.pos      = r.pos;
            o.drift    = r.drift;
            o.age      = r.age;
            return;
        }

        if (m_count < kMax)
        {
            m_rings[m_count++] = r;
            return;
        }

        // Full: the weakest goes (the oldest of equally weak ones, then the first in the
        // list: never an order that depends on anything but the list), unless the newcomer is
        // weaker than all of them.
        int   weakest = 0;
        float weakestS = effectiveStrength(m_rings[0]);
        for (int i = 1; i < m_count; ++i)
        {
            const float s = effectiveStrength(m_rings[i]);
            if (s < weakestS || (s == weakestS && m_rings[i].age > m_rings[weakest].age))
            {
                weakest  = i;
                weakestS = s;
            }
        }
        if (effectiveStrength(r) >= weakestS)
            m_rings[weakest] = r;
    }

    void RippleField::advance(float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt) || m_count == 0)
            return;
        int kept = 0;
        for (int i = 0; i < m_count; ++i)
        {
            Ripple r = m_rings[i];
            r.age += dt;
            r.pos += r.drift * dt;
            if (effectiveStrength(r) < kMinStrength)
                continue;
            m_rings[kept++] = r;
        }
        m_count = kept;
    }

    void RippleField::pack(WaterRippleBlock& out) const
    {
        out = WaterRippleBlock{};
        out.head[0] = static_cast<float>(m_count);
        out.head[1] = kRingSpeed;
        out.head[2] = kDecayTime;
        for (int i = 0; i < m_count; ++i)
        {
            const Ripple& r = m_rings[i];
            out.a[i][0] = r.pos.x;
            out.a[i][1] = r.pos.y;
            out.a[i][2] = r.radius;
            out.a[i][3] = r.strength;
            out.b[i][0] = r.age;
            out.b[i][1] = static_cast<float>(r.kind);
        }
    }

    void pushRipples(IRenderer* renderer, const PhysicsWorld* physics)
    {
        if (!renderer)
            return;
        WaterRippleBlock block;   // empty: no rings
        if (physics)
            physics->waterRipples().pack(block);
        renderer->SetWaterRipples(block);
    }
}
