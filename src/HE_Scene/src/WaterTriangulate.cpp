#include "HorizonScene/WaterMesh.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

// Ear clipping with hole bridging, for the water surface (WaterMesh.h).
//
// This is the algorithm and the staging of mapbox/earcut (ISC license,
// Copyright (c) 2016, Mapbox), re-written for this engine: doubles throughout,
// (x, z) as the plane, rings as std::vector<glm::dvec2>, the working ring as an
// index-linked circular list in a deque (stable addresses). The stages are the
// same — Eberly's ear clipping, holes merged into the outer ring through a bridge
// to a vertex they can see, a z-order hash that makes "is another vertex inside
// this ear" local for big rings, and the three escalations when nothing clips
// (filter degenerate points, cure local self-intersections, split the ring).
//
// ISC license: Permission to use, copy, modify, and/or distribute this software
// for any purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies. The software
// is provided "as is" and the author disclaims all warranties.
//
// ORIENTATION. Internally the outer ring runs counter-clockwise and holes
// clockwise, in the plane with x right and z up. `area(p, q, r)` is NEGATIVE for a
// convex corner of such a ring (it is minus the usual cross product), which is
// what every comparison below is written against.
namespace HE::water
{
namespace
{
    struct Node
    {
        uint32_t i = 0;                      // index into the rings laid end to end
        double   x = 0.0, z = 0.0;
        Node*    prev = nullptr;
        Node*    next = nullptr;
        int32_t  zOrder = 0;                 // position on the z-order curve (hashing only)
        Node*    prevZ = nullptr;
        Node*    nextZ = nullptr;
        bool     steiner = false;            // a lone point of a degenerate hole
    };

    // Minus the cross product of (q − p) and (r − q): negative at a convex corner
    // of a counter-clockwise ring, positive at a reflex one, zero when collinear.
    double area(const Node* p, const Node* q, const Node* r)
    {
        return (q->z - p->z) * (r->x - q->x) - (q->x - p->x) * (r->z - q->z);
    }

    bool equals(const Node* a, const Node* b) { return a->x == b->x && a->z == b->z; }

    // p inside (or on the boundary of) the counter-clockwise triangle abc.
    bool pointInTriangle(double ax, double az, double bx, double bz,
                         double cx, double cz, double px, double pz)
    {
        return (cx - px) * (az - pz) >= (ax - px) * (cz - pz) &&
               (ax - px) * (bz - pz) >= (bx - px) * (az - pz) &&
               (bx - px) * (cz - pz) >= (cx - px) * (bz - pz);
    }

    int sign(double v) { return (v > 0.0) - (v < 0.0); }

    bool onSegment(const Node* p, const Node* q, const Node* r)
    {
        return q->x <= std::max(p->x, r->x) && q->x >= std::min(p->x, r->x) &&
               q->z <= std::max(p->z, r->z) && q->z >= std::min(p->z, r->z);
    }

    // Do segments p1q1 and p2q2 intersect (touching counts)?
    bool intersects(const Node* p1, const Node* q1, const Node* p2, const Node* q2)
    {
        const int o1 = sign(area(p1, q1, p2));
        const int o2 = sign(area(p1, q1, q2));
        const int o3 = sign(area(p2, q2, p1));
        const int o4 = sign(area(p2, q2, q1));
        if (o1 != o2 && o3 != o4) return true;
        if (o1 == 0 && onSegment(p1, p2, q1)) return true;
        if (o2 == 0 && onSegment(p1, q2, q1)) return true;
        if (o3 == 0 && onSegment(p2, p1, q2)) return true;
        if (o4 == 0 && onSegment(p2, q1, q2)) return true;
        return false;
    }

    class Earcut
    {
    public:
        std::vector<uint32_t> run(const std::vector<Ring>& rings);

    private:
        std::deque<Node>      m_pool;
        std::vector<uint32_t> m_tris;
        bool   m_hashing = false;
        double m_minX = 0.0, m_minZ = 0.0, m_invSize = 0.0;

        Node* make(uint32_t i, double x, double z)
        {
            m_pool.emplace_back();
            Node& n = m_pool.back();
            n.i = i; n.x = x; n.z = z;
            return &n;
        }

        Node* insertNode(uint32_t i, double x, double z, Node* last)
        {
            Node* p = make(i, x, z);
            if (!last) { p->prev = p; p->next = p; }
            else
            {
                p->next = last->next;
                p->prev = last;
                last->next->prev = p;
                last->next = p;
            }
            return p;
        }

        static void removeNode(Node* p)
        {
            p->next->prev = p->prev;
            p->prev->next = p->next;
            if (p->prevZ) p->prevZ->nextZ = p->nextZ;
            if (p->nextZ) p->nextZ->prevZ = p->prevZ;
        }

        // Circular list of a ring, forced to the wanted winding. The shoelace sum is
        // positive for a counter-clockwise ring.
        Node* linkedList(const Ring& ring, uint32_t base, bool wantCounterClockwise)
        {
            double sum = 0.0;
            const size_t n = ring.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++)
                sum += (ring[j].x - ring[i].x) * (ring[i].y + ring[j].y);
            const bool isCcw = sum > 0.0;

            Node* last = nullptr;
            if (wantCounterClockwise == isCcw)
                for (size_t i = 0; i < n; ++i)
                    last = insertNode(base + static_cast<uint32_t>(i), ring[i].x, ring[i].y, last);
            else
                for (size_t i = n; i-- > 0;)
                    last = insertNode(base + static_cast<uint32_t>(i), ring[i].x, ring[i].y, last);

            if (last && equals(last, last->next))
            {
                Node* dup = last;
                last = last->next;
                removeNode(dup);
            }
            return last;
        }

        // Drop repeated and collinear points. Returns a node still in the list.
        Node* filterPoints(Node* start, Node* end = nullptr)
        {
            if (!start) return start;
            if (!end) end = start;
            Node* p = start;
            bool again;
            do
            {
                again = false;
                if (!p->steiner && (equals(p, p->next) || area(p->prev, p, p->next) == 0.0))
                {
                    removeNode(p);
                    p = end = p->prev;
                    if (p == p->next) break;
                    again = true;
                }
                else
                    p = p->next;
            } while (again || p != end);
            return end;
        }

        // ── The main loop ───────────────────────────────────────────────────
        void earcutLinked(Node* ear, int pass = 0)
        {
            if (!ear) return;
            if (pass == 0 && m_hashing) indexCurve(ear);

            Node* stop = ear;
            while (ear->prev != ear->next)
            {
                Node* prev = ear->prev;
                Node* next = ear->next;
                if (m_hashing ? isEarHashed(ear) : isEar(ear))
                {
                    m_tris.push_back(prev->i);
                    m_tris.push_back(ear->i);
                    m_tris.push_back(next->i);
                    removeNode(ear);
                    // Skipping the next vertex leads to fewer sliver triangles.
                    ear = next->next;
                    stop = next->next;
                    continue;
                }
                ear = next;
                if (ear == stop)
                {
                    // Nothing clips any more. Escalate: filter degenerate points,
                    // then cut local self-intersections, then split in two.
                    if (pass == 0)      earcutLinked(filterPoints(ear), 1);
                    else if (pass == 1) { ear = cureLocalIntersections(filterPoints(ear)); earcutLinked(ear, 2); }
                    else if (pass == 2) splitEarcut(ear);
                    break;
                }
            }
        }

        bool isEar(const Node* ear) const
        {
            const Node* a = ear->prev;
            const Node* b = ear;
            const Node* c = ear->next;
            if (area(a, b, c) >= 0.0) return false;          // reflex or flat: not an ear
            const Node* p = ear->next->next;
            while (p != ear->prev)
            {
                if (pointInTriangle(a->x, a->z, b->x, b->z, c->x, c->z, p->x, p->z) &&
                    area(p->prev, p, p->next) >= 0.0)
                    return false;
                p = p->next;
            }
            return true;
        }

        bool isEarHashed(const Node* ear) const
        {
            const Node* a = ear->prev;
            const Node* b = ear;
            const Node* c = ear->next;
            if (area(a, b, c) >= 0.0) return false;

            const double minTX = std::min({ a->x, b->x, c->x }), minTZ = std::min({ a->z, b->z, c->z });
            const double maxTX = std::max({ a->x, b->x, c->x }), maxTZ = std::max({ a->z, b->z, c->z });
            const int32_t minZ = zOrderOf(minTX, minTZ);
            const int32_t maxZ = zOrderOf(maxTX, maxTZ);

            auto blocks = [&](const Node* p)
            {
                return p != ear->prev && p != ear->next &&
                       pointInTriangle(a->x, a->z, b->x, b->z, c->x, c->z, p->x, p->z) &&
                       area(p->prev, p, p->next) >= 0.0;
            };

            const Node* p = ear->prevZ;
            const Node* n = ear->nextZ;
            while (p && p->zOrder >= minZ && n && n->zOrder <= maxZ)
            {
                if (blocks(p)) return false;
                p = p->prevZ;
                if (blocks(n)) return false;
                n = n->nextZ;
            }
            while (p && p->zOrder >= minZ)
            {
                if (blocks(p)) return false;
                p = p->prevZ;
            }
            while (n && n->zOrder <= maxZ)
            {
                if (blocks(n)) return false;
                n = n->nextZ;
            }
            return true;
        }

        // Two edges that cross each other, one vertex apart: cut the corner off.
        Node* cureLocalIntersections(Node* start)
        {
            Node* p = start;
            do
            {
                Node* a = p->prev;
                Node* b = p->next->next;
                if (!equals(a, b) && intersects(a, p, p->next, b) &&
                    locallyInside(a, b) && locallyInside(b, a))
                {
                    m_tris.push_back(a->i);
                    m_tris.push_back(p->i);
                    m_tris.push_back(b->i);
                    removeNode(p);
                    removeNode(p->next);
                    p = start = b;
                }
                p = p->next;
            } while (p != start);
            return filterPoints(p);
        }

        // Last resort: find a diagonal that splits the ring in two and clip each.
        void splitEarcut(Node* start)
        {
            Node* a = start;
            do
            {
                Node* b = a->next->next;
                while (b != a->prev)
                {
                    if (a->i != b->i && isValidDiagonal(a, b))
                    {
                        Node* c = splitPolygon(a, b);
                        a = filterPoints(a, a->next);
                        c = filterPoints(c, c->next);
                        earcutLinked(a);
                        earcutLinked(c);
                        return;
                    }
                    b = b->next;
                }
                a = a->next;
            } while (a != start);
        }

        // ── Holes ───────────────────────────────────────────────────────────
        static Node* getLeftmost(Node* start)
        {
            Node* p = start;
            Node* leftmost = start;
            do
            {
                if (p->x < leftmost->x || (p->x == leftmost->x && p->z < leftmost->z)) leftmost = p;
                p = p->next;
            } while (p != start);
            return leftmost;
        }

        // Merge a hole into the outer ring through a bridge (two coincident edges).
        Node* eliminateHole(Node* hole, Node* outer)
        {
            Node* bridge = findHoleBridge(hole, outer);
            if (!bridge) return outer;
            Node* bridgeReverse = splitPolygon(bridge, hole);
            filterPoints(bridgeReverse, bridgeReverse->next);
            return filterPoints(bridge, bridge->next);
        }

        // A vertex of the outer ring that the hole's leftmost vertex sees: cast a
        // ray to the left, take the endpoint of the edge it hits, and if another
        // vertex stands in the triangle between, the one at the smallest angle.
        Node* findHoleBridge(Node* hole, Node* outer)
        {
            Node* p = outer;
            const double hx = hole->x, hz = hole->z;
            double qx = -std::numeric_limits<double>::infinity();
            Node* m = nullptr;
            do
            {
                if (hz <= p->z && hz >= p->next->z && p->next->z != p->z)
                {
                    const double x = p->x + (hz - p->z) * (p->next->x - p->x) / (p->next->z - p->z);
                    if (x <= hx && x > qx)
                    {
                        qx = x;
                        m = p->x < p->next->x ? p : p->next;
                        if (x == hx) return m;               // the hole touches the outer edge
                    }
                }
                p = p->next;
            } while (p != outer);
            if (!m) return nullptr;

            const Node* stop = m;
            double tanMin = std::numeric_limits<double>::infinity();
            const double mx = m->x, mz = m->z;
            p = m;
            do
            {
                if (hx >= p->x && p->x >= mx && hx != p->x &&
                    pointInTriangle(hz < mz ? hx : qx, hz, mx, mz, hz < mz ? qx : hx, hz, p->x, p->z))
                {
                    const double tanCur = std::abs(hz - p->z) / (hx - p->x);
                    if (locallyInside(p, hole) &&
                        (tanCur < tanMin ||
                         (tanCur == tanMin && (p->x > m->x || sectorContainsSector(m, p)))))
                    {
                        m = p;
                        tanMin = tanCur;
                    }
                }
                p = p->next;
            } while (p != stop);
            return m;
        }

        static bool sectorContainsSector(const Node* m, const Node* p)
        {
            return area(m->prev, m, p->prev) < 0.0 && area(p->next, m, m->next) < 0.0;
        }

        // ── z-order hashing ─────────────────────────────────────────────────
        int32_t zOrderOf(double x, double z) const
        {
            int32_t xi = static_cast<int32_t>((x - m_minX) * m_invSize);
            int32_t zi = static_cast<int32_t>((z - m_minZ) * m_invSize);
            xi = (xi | (xi << 8)) & 0x00FF00FF;
            xi = (xi | (xi << 4)) & 0x0F0F0F0F;
            xi = (xi | (xi << 2)) & 0x33333333;
            xi = (xi | (xi << 1)) & 0x55555555;
            zi = (zi | (zi << 8)) & 0x00FF00FF;
            zi = (zi | (zi << 4)) & 0x0F0F0F0F;
            zi = (zi | (zi << 2)) & 0x33333333;
            zi = (zi | (zi << 1)) & 0x55555555;
            return xi | (zi << 1);
        }

        void indexCurve(Node* start)
        {
            Node* p = start;
            do
            {
                p->zOrder = zOrderOf(p->x, p->z);
                p->prevZ = p->prev;
                p->nextZ = p->next;
                p = p->next;
            } while (p != start);
            p->prevZ->nextZ = nullptr;
            p->prevZ = nullptr;
            sortLinked(p);
        }

        // Merge sort of the z-order list (Simon Tatham's linked-list version).
        static Node* sortLinked(Node* list)
        {
            int inSize = 1;
            int numMerges;
            do
            {
                Node* p = list;
                list = nullptr;
                Node* tail = nullptr;
                numMerges = 0;
                while (p)
                {
                    ++numMerges;
                    Node* q = p;
                    int pSize = 0;
                    for (int i = 0; i < inSize; ++i)
                    {
                        ++pSize;
                        q = q->nextZ;
                        if (!q) break;
                    }
                    int qSize = inSize;
                    while (pSize > 0 || (qSize > 0 && q))
                    {
                        Node* e;
                        if (pSize == 0)                         { e = q; q = q->nextZ; --qSize; }
                        else if (qSize == 0 || !q)              { e = p; p = p->nextZ; --pSize; }
                        else if (p->zOrder <= q->zOrder)        { e = p; p = p->nextZ; --pSize; }
                        else                                    { e = q; q = q->nextZ; --qSize; }
                        if (tail) tail->nextZ = e; else list = e;
                        e->prevZ = tail;
                        tail = e;
                    }
                    p = q;
                }
                tail->nextZ = nullptr;
                inSize *= 2;
            } while (numMerges > 1);
            return list;
        }

        // ── Diagonals ───────────────────────────────────────────────────────
        // Is the diagonal ab inside the polygon and clear of its edges?
        bool isValidDiagonal(const Node* a, const Node* b) const
        {
            return a->next->i != b->i && a->prev->i != b->i && !intersectsPolygon(a, b) &&
                   ((locallyInside(a, b) && locallyInside(b, a) && middleInside(a, b) &&
                     (area(a->prev, a, b->prev) != 0.0 || area(a, b->prev, b) != 0.0)) ||
                    (equals(a, b) && area(a->prev, a, a->next) > 0.0 && area(b->prev, b, b->next) > 0.0));
        }

        static bool intersectsPolygon(const Node* a, const Node* b)
        {
            const Node* p = a;
            do
            {
                if (p->i != a->i && p->next->i != a->i && p->i != b->i && p->next->i != b->i &&
                    intersects(p, p->next, a, b))
                    return true;
                p = p->next;
            } while (p != a);
            return false;
        }

        // Does the diagonal ab leave `a` into the interior of the polygon?
        static bool locallyInside(const Node* a, const Node* b)
        {
            return area(a->prev, a, a->next) < 0.0
                ? area(a, b, a->next) >= 0.0 && area(a, a->prev, b) >= 0.0
                : area(a, b, a->prev) < 0.0 || area(a, a->next, b) < 0.0;
        }

        static bool middleInside(const Node* a, const Node* b)
        {
            const Node* p = a;
            bool inside = false;
            const double px = (a->x + b->x) / 2.0, pz = (a->z + b->z) / 2.0;
            do
            {
                if (((p->z > pz) != (p->next->z > pz)) && p->next->z != p->z &&
                    (px < (p->next->x - p->x) * (pz - p->z) / (p->next->z - p->z) + p->x))
                    inside = !inside;
                p = p->next;
            } while (p != a);
            return inside;
        }

        // Cut the ring along ab: two rings, one starting at a, the other at the
        // returned node. Used for the hole bridge as well, where the second ring
        // is the same ring read the other way round (the bridge is walked twice).
        Node* splitPolygon(Node* a, Node* b)
        {
            Node* a2 = make(a->i, a->x, a->z);
            Node* b2 = make(b->i, b->x, b->z);
            Node* an = a->next;
            Node* bp = b->prev;

            a->next = b;   b->prev = a;
            a2->next = an; an->prev = a2;
            b2->next = a2; a2->prev = b2;
            bp->next = b2; b2->prev = bp;
            return b2;
        }
    };

    std::vector<uint32_t> Earcut::run(const std::vector<Ring>& rings)
    {
        m_pool.clear();
        m_tris.clear();
        m_hashing = false;

        if (rings.empty() || rings[0].size() < 3) return {};
        size_t total = 0;
        for (const Ring& r : rings)
        {
            for (const glm::dvec2& p : r)
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) return {};
            total += r.size();
        }

        Node* outer = linkedList(rings[0], 0, true);
        if (!outer || outer->next == outer->prev) return {};

        if (rings.size() > 1)
        {
            std::vector<Node*> queue;
            uint32_t base = static_cast<uint32_t>(rings[0].size());
            for (size_t h = 1; h < rings.size(); ++h)
            {
                const Ring& hr = rings[h];
                Node* list = hr.empty() ? nullptr : linkedList(hr, base, false);
                base += static_cast<uint32_t>(hr.size());
                if (!list) continue;
                if (list == list->next) list->steiner = true;
                queue.push_back(getLeftmost(list));
            }
            // Leftmost hole first, then ties by z: the order the bridges are cut in
            // decides the triangulation, so it must not depend on the sort's mood.
            std::sort(queue.begin(), queue.end(), [](const Node* a, const Node* b)
                      { return a->x != b->x ? a->x < b->x : a->z < b->z; });
            for (Node* hole : queue)
                outer = eliminateHole(hole, outer);
        }

        // Hash only when the ring is big enough for the local test to pay.
        if (total > 80)
        {
            double minX = outer->x, maxX = outer->x, minZ = outer->z, maxZ = outer->z;
            Node* p = outer->next;
            while (p != outer)
            {
                minX = std::min(minX, p->x); maxX = std::max(maxX, p->x);
                minZ = std::min(minZ, p->z); maxZ = std::max(maxZ, p->z);
                p = p->next;
            }
            m_minX = minX;
            m_minZ = minZ;
            const double span = std::max(maxX - minX, maxZ - minZ);
            m_invSize = span != 0.0 ? 32767.0 / span : 0.0;
            m_hashing = true;
        }

        earcutLinked(outer);
        return std::move(m_tris);
    }
}

std::vector<uint32_t> triangulate(const std::vector<Ring>& rings)
{
    Earcut e;
    return e.run(rings);
}

double signedArea(const Ring& ring)
{
    double a = 0.0;
    for (size_t i = 0, j = ring.size() ? ring.size() - 1 : 0; i < ring.size(); j = i++)
        a += ring[j].x * ring[i].y - ring[i].x * ring[j].y;
    return a * 0.5;
}

} // namespace HE::water
