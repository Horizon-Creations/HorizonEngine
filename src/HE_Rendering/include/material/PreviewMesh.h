#pragma once
// Shared procedural preview primitives for IRenderer::RenderMaterialPreview.
// Interleaved pos3/normal3/uv2 (8 floats — the material vertex layout both the GL
// and Metal preview paths bind), uint32 indices. Header-only so both backends share
// one implementation and can never drift apart.
//
// shape: 0 = unit sphere, 1 = cube, 2 = plane (double-sided quad, so it shades
// correctly from both sides while the orbit camera circles it).

#include <cmath>
#include <cstdint>
#include <vector>

namespace HE
{
inline void buildPreviewMesh(int shape, std::vector<float>& verts, std::vector<uint32_t>& idx)
{
    verts.clear();
    idx.clear();
    switch (shape)
    {
        default: // 0 — unit sphere
        {
            const int segU = 48, segV = 32;
            for (int y = 0; y <= segV; ++y)
            {
                const float v = (float)y / segV, phi = v * 3.14159265f;
                for (int x = 0; x <= segU; ++x)
                {
                    const float u = (float)x / segU, th = u * 6.2831853f;
                    const float nx = std::sin(phi) * std::cos(th);
                    const float ny = std::cos(phi);
                    const float nz = std::sin(phi) * std::sin(th);
                    verts.insert(verts.end(), { nx, ny, nz, nx, ny, nz, u, v });
                }
            }
            for (int y = 0; y < segV; ++y)
                for (int x = 0; x < segU; ++x)
                {
                    const uint32_t a = y * (segU + 1) + x, b = a + segU + 1;
                    idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            break;
        }
        case 1: // cube (24 verts, per-face normals + a clean [0,1] unwrap per face)
        {
            const float h = 0.82f; // slightly smaller than the unit sphere so it frames alike
            const float fn[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
            const float fq[6][4][3] = {
                { {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} }, // +Z
                { { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} }, // -Z
                { { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h} }, // +X
                { {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h} }, // -X
                { {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h} }, // +Y
                { {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h} }, // -Y
            };
            const float uvq[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };
            for (int f = 0; f < 6; ++f)
            {
                const uint32_t base = (uint32_t)(verts.size() / 8);
                for (int k = 0; k < 4; ++k)
                    verts.insert(verts.end(), { fq[f][k][0], fq[f][k][1], fq[f][k][2],
                                                fn[f][0], fn[f][1], fn[f][2],
                                                uvq[k][0], uvq[k][1] });
                idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            }
            break;
        }
        case 2: // plane — an XY quad with an explicit back side (opposed normals/winding)
        {
            const float h = 0.78f; // frames like the unit sphere at the default orbit distance
            // Front (+Z)
            verts.insert(verts.end(), { -h,-h,0,  0,0,1,  0,1,
                                         h,-h,0,  0,0,1,  1,1,
                                         h, h,0,  0,0,1,  1,0,
                                        -h, h,0,  0,0,1,  0,0 });
            idx.insert(idx.end(), { 0, 1, 2, 0, 2, 3 });
            // Back (-Z) — mirrored winding so it faces the camera from behind too.
            verts.insert(verts.end(), { -h,-h,0,  0,0,-1, 1,1,
                                         h,-h,0,  0,0,-1, 0,1,
                                         h, h,0,  0,0,-1, 0,0,
                                        -h, h,0,  0,0,-1, 1,0 });
            idx.insert(idx.end(), { 4, 6, 5, 4, 7, 6 });
            break;
        }
    }
}
} // namespace HE
