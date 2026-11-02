#pragma once

// ─── The world-scale landscape's shaders (Thema 326, step 2, part A) ─────────
// One 65 x 65 grid (HE::LandscapePatchMesh) is drawn once per patch of the quadtree (HE::LandscapeFrame::Patch); the vertex
// shader turns a grid position into a world position and takes the height from the tile atlas or, for the coarse levels, from
// the coarse world. GLSL 4.10 with loose uniforms, like kSkyFS: a Vulkan build gets a prelude that maps the uniform names
// onto a block (part B), the body stays as it is, so it must stay free of layout qualifiers and GL-only built-ins. Metal and D3D11/D3D12 have
// hand ports below (kLandscapeMSL, kLandscapeHLSL): a change to the GLSL is a change in all three.
//
// What the contract says (HE::LandscapeGpuLayout.h has the image layouts, HE::LandscapeFrame the numbers):
//   * Heights are read with texelFetch at integer lattice positions, never filtered: a vertex of a level-L patch sits on the
//     global lattice of cell size cell0 * 2^L, so its cell coordinates are integers (the patch's corner cell plus the grid index).
//   * The MORPH distance is horizontal distance to the vertex plus uCameraRel.w, the camera's distance to the height slab; it
//     does NOT include the vertex's own height. The selection on the CPU (landscapeMorphDistance) uses the same metric, which is
//     what makes the seams between levels exact. A shader that morphs by the true 3D distance opens cracks.
//   * A lattice point on a tile border is shared by two tiles; the lookup tries the tile it lies in and then the neighbours that
//     share it, and falls back to the coarse world's finest grid when no tile has it (still streaming): at the edge of the
//     loaded disc the last fine patch keeps its far edge, and a tile that is not there yet shows coarser ground, not a hole.
//   * An odd vertex morphs toward the middle of its neighbours on the SAME level (landscapeMorphSource): 1 or 2 more fetches.
//   * A fragment beyond the world's absolute extent is discarded: the top node of the quadtree is bigger than the world, and its
//     vertices past the edge read the last row of heights.
//   * Normals: derivatives of the interpolated world position in the fragment shader. Flat shaded at the patch's resolution, no
//     normal texture; a smooth normal is part B.
//   * Weights: the fragment's own tile, nearest texel, three RGBA8 pages, flat layer colours; a tile that is not in the atlas reads the
//     coarse world's weights (128 m texels) the same way. The material graph / auto-landscape material (which assumes one UV over the
//     whole landscape, uvTiling) needs world-space UVs at this size and comes after the draw paths.
namespace HE::glsl
{

inline constexpr const char* kLandscapeVS = R"GLSL(
#version 410 core
in vec2 aGrid;                       // (i, j) of the shared grid, 0..64

uniform mat4  uViewProj;             // origin-relative world to clip
uniform vec4  uCameraRel;            // xyz = camera, origin-relative; w = distance of the camera to the height slab
uniform vec4  uWorld;                // x = world max x, y = world max z (absolute), z = tile size, w = metres of a height of 65535
uniform vec4  uHeightInfo;           // x = metres of height 0, y = tiles in x, z = tiles in z, w = slot table window (power of two)
uniform vec3  uOrigin;               // floating origin: absolute = relative + origin
uniform ivec4 uTile;                 // x = height cells of a tile at level 0, y = weight texels, z = tile levels
uniform ivec4 uHeightRect[8];        // per tile mip: xy = origin inside the layer image, zw = size
uniform ivec4 uCoarseGrid[16];       // per coarse grid: x = width, y = height, z = row offset in the stacked image
uniform ivec4 uPatch;                // x = level, y/z = global cell of the node's corner (node index * 64), w = 1 coarse / 0 tiles
uniform vec4  uPatchPos;             // x/y = corner, origin-relative, z = node size, w = coarse grid index
uniform vec4  uMorph;                // x = morph start distance, y = morph end distance
uniform usampler2DArray uTileHeights;   // R16UI, a layer per atlas slot
uniform usampler2D      uSlotTable;     // R16UI, window x window, toroidal; 65535 = tile not in the atlas
uniform usampler2D      uCoarseHeights; // R16UI, the coarse grids stacked

out vec3 vWorldRel;                  // origin-relative position, height in absolute metres minus the origin's y

float heightOfCoarseGrid(ivec2 g, int grid)
{
    ivec4 gr = uCoarseGrid[grid];
    ivec2 c = clamp(g, ivec2(0), gr.xy - ivec2(1));
    uint v = texelFetch(uCoarseHeights, ivec2(c.x, gr.z + c.y), 0).r;
    return uHeightInfo.x + float(v) * (uWorld.w / 65535.0);
}

float heightOfTile(ivec2 g, int level)
{
    int cpt = uTile.x >> level;      // cells of a tile at this mip
    ivec2 maxTile = ivec2(int(uHeightInfo.y), int(uHeightInfo.z)) - ivec2(1);
    ivec2 t = min(g / cpt, maxTile);
    ivec2 l = min(g - t * cpt, ivec2(cpt));   // the world's last row/column: the tile's last sample
    int mask = int(uHeightInfo.w) - 1;
    // A lattice point on a tile border is the last sample of the tile before it and the first of the tile it lies in (the
    // edge samples are shared). The edge of the loaded disc is where one of the two is missing: try the tile it lies in, then
    // the neighbours that share the point, so the last fine patch keeps its far edge instead of dropping to the floor.
    for (int k = 0; k < 4; ++k)
    {
        ivec2 step = ivec2(k & 1, k >> 1);
        if ((step.x != 0 && l.x != 0) || (step.y != 0 && l.y != 0)) continue;     // only a border point is shared
        ivec2 tt = t - step;
        if (tt.x < 0 || tt.y < 0) continue;
        uint slot = texelFetch(uSlotTable, ivec2(tt.x & mask, tt.y & mask), 0).r;
        if (slot == 65535u) continue;
        ivec2 ll = l + step * cpt;
        ivec4 r = uHeightRect[level];
        uint v = texelFetch(uTileHeights, ivec3(r.x + ll.x, r.y + ll.y, int(slot)), 0).r;
        return uHeightInfo.x + float(v) * (uWorld.w / 65535.0);
    }
    // No tile has it (still streaming): the coarse world's finest grid, so the ground is there, only coarser, not a hole.
    return heightOfCoarseGrid(g >> (uTile.z + 1 - level), 0);
}

float heightAt(ivec2 g)
{
    return uPatch.w == 0 ? heightOfTile(g, uPatch.x) : heightOfCoarseGrid(g, int(uPatchPos.w));
}

void main()
{
    ivec2 ij = ivec2(aGrid + vec2(0.5));
    float cell = uPatchPos.z / 64.0;
    vec2 xz = uPatchPos.xy + vec2(ij) * cell;
    ivec2 g = uPatch.yz + ij;
    float h = heightAt(g);

    // CDLOD morph. The distance is horizontal plus the slab term, NOT the vertex's own height.
    vec2 d2 = xz - uCameraRel.xz;
    float d = sqrt(dot(d2, d2) + uCameraRel.w * uCameraRel.w);
    float m = clamp((d - uMorph.x) / max(uMorph.y - uMorph.x, 1e-3), 0.0, 1.0);
    bool oi = (ij.x & 1) == 1;
    bool oj = (ij.y & 1) == 1;
    if (m > 0.0 && (oi || oj))
    {
        ivec2 a, b;
        if (oi && !oj)      { a = g + ivec2(-1, 0);  b = g + ivec2(1, 0); }
        else if (!oi && oj) { a = g + ivec2(0, -1);  b = g + ivec2(0, 1); }
        else                { a = g + ivec2(-1, -1); b = g + ivec2(1, 1); }   // on the triangulation's diagonal
        h = mix(h, 0.5 * (heightAt(a) + heightAt(b)), m);
    }
    vWorldRel = vec3(xz.x, h - uOrigin.y, xz.y);
    gl_Position = uViewProj * vec4(vWorldRel, 1.0);
}
)GLSL";

inline constexpr const char* kLandscapeFS = R"GLSL(
#version 410 core
in vec3 vWorldRel;
out vec4 fragColor;

uniform vec4  uWorld;                // x = world max x, y = world max z (absolute), z = tile size
uniform vec4  uHeightInfo;           // y = tiles in x, z = tiles in z, w = slot table window
uniform vec3  uOrigin;
uniform ivec4 uTile;                 // y = weight texels of a tile at level 0
uniform vec4  uLayerColor[12];       // flat colour of each of the twelve layers (the material graph replaces this in part B)
uniform vec3  uSunDir;               // towards the sun
uniform vec3  uSunColor;
uniform vec3  uAmbient;
uniform vec4  uTex;                  // x = 1 / metres of one texture tile, y = 1 when uAlbedoArray is the group's albedo array, zw = fract(origin.xz * x)
uniform usampler2D      uSlotTable;
uniform sampler2DArray  uTileWeights0;   // RGBA8, layers 0-3 of every tile's twelve
uniform sampler2DArray  uTileWeights1;
uniform sampler2DArray  uTileWeights2;
uniform sampler2D       uCoarseWeights0; // RGBA8, the finest weight grid of the coarse world (one texel per 128 m), pages as above
uniform sampler2D       uCoarseWeights1;
uniform sampler2D       uCoarseWeights2;
uniform sampler2DArray  uAlbedoArray;    // the texture group's albedo array: slices 0-3 are layers 0-3, repeated in world coordinates

void main()
{
    vec2 abs2 = vWorldRel.xz + uOrigin.xz;
    if (abs2.x > uWorld.x || abs2.y > uWorld.y || abs2.x < 0.0 || abs2.y < 0.0) discard;

    ivec2 tile = ivec2(floor(abs2 / uWorld.z));
    tile = clamp(tile, ivec2(0), ivec2(int(uHeightInfo.y), int(uHeightInfo.z)) - ivec2(1));
    int mask = int(uHeightInfo.w) - 1;
    uint slot = texelFetch(uSlotTable, ivec2(tile.x & mask, tile.y & mask), 0).r;

    vec4 w0, w1, w2;
    if (slot == 65535u)
    {
        // A tile that is not in the atlas (the coarse levels, or one still streaming): the coarse world's weights, one texel per 128 m.
        ivec2 cs = textureSize(uCoarseWeights0, 0);
        ivec2 c = clamp(ivec2(floor(abs2 / uWorld.xy * vec2(cs))), ivec2(0), cs - ivec2(1));
        w0 = texelFetch(uCoarseWeights0, c, 0);
        w1 = texelFetch(uCoarseWeights1, c, 0);
        w2 = texelFetch(uCoarseWeights2, c, 0);
    }
    else
    {
        float texel = uWorld.z / float(uTile.y);
        ivec2 t = clamp(ivec2(floor((abs2 - vec2(tile) * uWorld.z) / texel)), ivec2(0), ivec2(uTile.y - 1));
        ivec3 at = ivec3(t, int(slot));
        w0 = texelFetch(uTileWeights0, at, 0);
        w1 = texelFetch(uTileWeights1, at, 0);
        w2 = texelFetch(uTileWeights2, at, 0);
    }
    // Layers 0-3 are the texture group's grass / dirt / rock / snow slices in world coordinates; the others (and all of them where the
    // group's array is not here) keep their flat colour. The branch is on a uniform, so the derivatives of texture() stay defined.
    vec3 c0 = uLayerColor[0].rgb, c1 = uLayerColor[1].rgb, c2 = uLayerColor[2].rgb, c3 = uLayerColor[3].rgb;
    if (uTex.y > 0.5)
    {
        vec2 uv = vWorldRel.xz * uTex.x + uTex.zw;
        c0 = texture(uAlbedoArray, vec3(uv, 0.0)).rgb;
        c1 = texture(uAlbedoArray, vec3(uv, 1.0)).rgb;
        c2 = texture(uAlbedoArray, vec3(uv, 2.0)).rgb;
        c3 = texture(uAlbedoArray, vec3(uv, 3.0)).rgb;
    }
    vec3 albedo = w0.x * c0 + w0.y * c1 + w0.z * c2 + w0.w * c3
                + w1.x * uLayerColor[4].rgb + w1.y * uLayerColor[5].rgb + w1.z * uLayerColor[6].rgb + w1.w * uLayerColor[7].rgb
                + w2.x * uLayerColor[8].rgb + w2.y * uLayerColor[9].rgb + w2.z * uLayerColor[10].rgb + w2.w * uLayerColor[11].rgb;
    vec3 n = normalize(cross(dFdx(vWorldRel), dFdy(vWorldRel)));
    if (n.y < 0.0) n = -n;
    float ndl = max(dot(n, normalize(uSunDir)), 0.0);
    fragColor = vec4(albedo * (uAmbient + uSunColor * ndl), 1.0);
}
)GLSL";

// ─── Metal ───────────────────────────────────────────────────────────────────
// The same two shaders in MSL, by hand (MetalRenderer's sky is hand-written MSL too). Same maths, line for line: a change to the GLSL
// above is a change here, and tests/test_landscape_shader.cpp compares the two sources' uniform sets. Differences that are not maths:
//   * no vertex buffer: the grid position comes from [[vertex_id]] (the index value of the indexed draw, base vertex 0): (id % 65, id / 65)
//   * the loose uniforms are one constant struct (LsGlobals, buffer 20, both stages) and the per-patch ones another (LsPatch, vertex buffer 21)
//   * images: vertex textures 20 tile heights, 21 slot table, 22 coarse heights; fragment textures 20 slot table, 21-23 tile weight pages,
//     24-26 coarse weight pages. High indices on purpose: MetalRenderer binds its own below 17 and does not rebind them after us.
//   * integer images are texture2d<ushort> / texture2d_array<ushort> read with read(), RGBA8 as float, nothing is sampled
//   * dFdy points the other way (the framebuffer's y is down), which the fragment's `if (n.y < 0) n = -n` already absorbs
// The C++ mirror of the structs is MetalLandscape.mm (a static_assert on their sizes lives there).
inline constexpr const char* kLandscapeMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct LsGlobals
{
    float4x4 viewProj;       // origin-relative world to clip, the GL-convention matrix of MetalRenderer's scene pass (depth -1..1)
    float4   cameraRel;      // xyz = camera, origin-relative; w = distance of the camera to the height slab
    float4   world;          // x = world max x, y = world max z (absolute), z = tile size, w = metres of a height of 65535
    float4   heightInfo;     // x = metres of height 0, y = tiles in x, z = tiles in z, w = slot table window
    float4   origin;         // xyz = floating origin
    int4     tile;           // x = height cells of a tile at level 0, y = weight texels, z = tile levels
    int4     heightRect[8];
    int4     coarseGrid[16];
    float4   layerColor[12];
    float4   sunDir;         // xyz = towards the sun
    float4   sunColor;
    float4   ambient;
    float4   tex;            // x = 1 / metres of one texture tile, y = 1 when the albedo array is bound, zw = fract(origin.xz * x)
};

struct LsPatch
{
    int4   patch;            // x = level, y/z = global cell of the node's corner, w = 1 coarse / 0 tiles
    float4 patchPos;         // x/y = corner, origin-relative, z = node size, w = coarse grid index
    float4 morph;            // x = morph start distance, y = morph end distance
};

struct LsVertexOut
{
    float4 position [[position]];
    float3 worldRel;
};

static float lsHeightOfCoarseGrid(int2 g, int grid, constant LsGlobals& G, texture2d<ushort, access::read> coarse)
{
    int4 gr = G.coarseGrid[grid];
    int2 c = clamp(g, int2(0), gr.xy - int2(1));
    uint v = coarse.read(uint2(uint(c.x), uint(gr.z + c.y))).r;
    return G.heightInfo.x + float(v) * (G.world.w / 65535.0);
}

static float lsHeightOfTile(int2 g, int level, constant LsGlobals& G, texture2d_array<ushort, access::read> tiles,
                            texture2d<ushort, access::read> slots, texture2d<ushort, access::read> coarse)
{
    int cpt = G.tile.x >> level;
    int2 maxTile = int2(int(G.heightInfo.y), int(G.heightInfo.z)) - int2(1);
    int2 t = min(g / cpt, maxTile);
    int2 l = min(g - t * cpt, int2(cpt));
    int mask = int(G.heightInfo.w) - 1;
    for (int k = 0; k < 4; ++k)
    {
        int2 step = int2(k & 1, k >> 1);
        if ((step.x != 0 && l.x != 0) || (step.y != 0 && l.y != 0)) continue;
        int2 tt = t - step;
        if (tt.x < 0 || tt.y < 0) continue;
        uint slot = slots.read(uint2(uint(tt.x & mask), uint(tt.y & mask))).r;
        if (slot == 65535u) continue;
        int2 ll = l + step * cpt;
        int4 r = G.heightRect[level];
        uint v = tiles.read(uint2(uint(r.x + ll.x), uint(r.y + ll.y)), slot).r;
        return G.heightInfo.x + float(v) * (G.world.w / 65535.0);
    }
    return lsHeightOfCoarseGrid(g >> (G.tile.z + 1 - level), 0, G, coarse);
}

static float lsHeightAt(int2 g, constant LsGlobals& G, constant LsPatch& P, texture2d_array<ushort, access::read> tiles,
                        texture2d<ushort, access::read> slots, texture2d<ushort, access::read> coarse)
{
    return P.patch.w == 0 ? lsHeightOfTile(g, P.patch.x, G, tiles, slots, coarse) : lsHeightOfCoarseGrid(g, int(P.patchPos.w), G, coarse);
}

vertex LsVertexOut vertexMain(uint vid [[vertex_id]],
                              constant LsGlobals& G [[buffer(20)]],
                              constant LsPatch& P [[buffer(21)]],
                              texture2d_array<ushort, access::read> tiles [[texture(20)]],
                              texture2d<ushort, access::read> slots [[texture(21)]],
                              texture2d<ushort, access::read> coarse [[texture(22)]])
{
    int2 ij = int2(int(vid % 65u), int(vid / 65u));
    float cell = P.patchPos.z / 64.0;
    float2 xz = P.patchPos.xy + float2(ij) * cell;
    int2 g = P.patch.yz + ij;
    float h = lsHeightAt(g, G, P, tiles, slots, coarse);

    // CDLOD morph. The distance is horizontal plus the slab term, NOT the vertex's own height.
    float2 d2 = xz - G.cameraRel.xz;
    float d = sqrt(dot(d2, d2) + G.cameraRel.w * G.cameraRel.w);
    float m = clamp((d - P.morph.x) / max(P.morph.y - P.morph.x, 1e-3), 0.0, 1.0);
    bool oi = (ij.x & 1) == 1;
    bool oj = (ij.y & 1) == 1;
    if (m > 0.0 && (oi || oj))
    {
        int2 a, b;
        if (oi && !oj)      { a = g + int2(-1, 0);  b = g + int2(1, 0); }
        else if (!oi && oj) { a = g + int2(0, -1);  b = g + int2(0, 1); }
        else                { a = g + int2(-1, -1); b = g + int2(1, 1); }   // on the triangulation's diagonal
        h = mix(h, 0.5 * (lsHeightAt(a, G, P, tiles, slots, coarse) + lsHeightAt(b, G, P, tiles, slots, coarse)), m);
    }
    LsVertexOut o;
    o.worldRel = float3(xz.x, h - G.origin.y, xz.y);
    o.position = G.viewProj * float4(o.worldRel, 1.0);
    return o;
}

fragment float4 fragmentMain(LsVertexOut in [[stage_in]],
                             constant LsGlobals& G [[buffer(20)]],
                             texture2d<ushort, access::read> slots [[texture(20)]],
                             texture2d_array<float, access::read> w0t [[texture(21)]],
                             texture2d_array<float, access::read> w1t [[texture(22)]],
                             texture2d_array<float, access::read> w2t [[texture(23)]],
                             texture2d<float, access::read> c0t [[texture(24)]],
                             texture2d<float, access::read> c1t [[texture(25)]],
                             texture2d<float, access::read> c2t [[texture(26)]],
                             texture2d_array<float> albedoArr [[texture(27)]])
{
    float2 abs2 = in.worldRel.xz + G.origin.xz;
    if (abs2.x > G.world.x || abs2.y > G.world.y || abs2.x < 0.0 || abs2.y < 0.0) discard_fragment();

    int2 tile = int2(floor(abs2 / G.world.z));
    tile = clamp(tile, int2(0), int2(int(G.heightInfo.y), int(G.heightInfo.z)) - int2(1));
    int mask = int(G.heightInfo.w) - 1;
    uint slot = slots.read(uint2(uint(tile.x & mask), uint(tile.y & mask))).r;

    float4 w0, w1, w2;
    if (slot == 65535u)
    {
        // A tile that is not in the atlas (the coarse levels, or one still streaming): the coarse world's weights, one texel per 128 m.
        int2 cs = int2(int(c0t.get_width()), int(c0t.get_height()));
        int2 c = clamp(int2(floor(abs2 / G.world.xy * float2(cs))), int2(0), cs - int2(1));
        w0 = c0t.read(uint2(c));
        w1 = c1t.read(uint2(c));
        w2 = c2t.read(uint2(c));
    }
    else
    {
        float texel = G.world.z / float(G.tile.y);
        int2 t = clamp(int2(floor((abs2 - float2(tile) * G.world.z) / texel)), int2(0), int2(G.tile.y - 1));
        w0 = w0t.read(uint2(t), slot);
        w1 = w1t.read(uint2(t), slot);
        w2 = w2t.read(uint2(t), slot);
    }
    // Layers 0-3 are the texture group's grass / dirt / rock / snow slices in world coordinates (see the GLSL).
    float3 c0 = G.layerColor[0].rgb, c1 = G.layerColor[1].rgb, c2 = G.layerColor[2].rgb, c3 = G.layerColor[3].rgb;
    if (G.tex.y > 0.5)
    {
        constexpr sampler smp(coord::normalized, address::repeat, filter::linear, mip_filter::linear);
        float2 uv = in.worldRel.xz * G.tex.x + G.tex.zw;
        c0 = albedoArr.sample(smp, uv, 0).rgb;
        c1 = albedoArr.sample(smp, uv, 1).rgb;
        c2 = albedoArr.sample(smp, uv, 2).rgb;
        c3 = albedoArr.sample(smp, uv, 3).rgb;
    }
    float3 albedo = w0.x * c0 + w0.y * c1 + w0.z * c2 + w0.w * c3
                  + w1.x * G.layerColor[4].rgb + w1.y * G.layerColor[5].rgb + w1.z * G.layerColor[6].rgb + w1.w * G.layerColor[7].rgb
                  + w2.x * G.layerColor[8].rgb + w2.y * G.layerColor[9].rgb + w2.z * G.layerColor[10].rgb + w2.w * G.layerColor[11].rgb;
    float3 n = normalize(cross(dfdx(in.worldRel), dfdy(in.worldRel)));
    if (n.y < 0.0) n = -n;
    float ndl = max(dot(n, normalize(G.sunDir.xyz)), 0.0);
    return float4(albedo * (G.ambient.xyz + G.sunColor.xyz * ndl), 1.0);
}
)MSL";

// ─── D3D11 / D3D12 ───────────────────────────────────────────────────────────
// The same two shaders in HLSL (SM 5.0, fxc), by hand, one source for both D3D backends (the D3D sky is hand-written HLSL too).
// Same maths, line for line, as the MSL above; what differs is not maths:
//   * no vertex buffer and no input layout: the grid position comes from SV_VertexID, which in an indexed draw (base vertex 0) is the
//     index value: (id % 65, id / 65)
//   * the loose uniforms are two constant buffers: LsGlobals (b12, both stages) and LsPatch (b13, vertex stage, 48 bytes, one per patch).
//     D3D11 only has b0-b13 and the renderer's own passes use the low ones; D3D12 maps the same two registers in a root signature of its own
//   * images: t32 tile heights, t33 slot table, t34 coarse heights, t35-t37 tile weight pages, t38-t40 coarse weight pages, all
//     read with Load(); t41 is the texture group's albedo array of the world-UV ground, the one image that is sampled (s13: linear, repeat,
//     mips; a D3D12 static sampler, a D3D11 sampler state). The array is sampled unconditionally, before the discard and outside any
//     branch (a gradient needs uniform flow), and `tex.y` picks between it and the flat colours. SRVs above t31 on purpose: the D3D11
//     renderer and the graph materials use t0-t26 and do not rebind them after the landscape draw
//   * `patch` is a keyword in HLSL, the per-patch block's member is lsPatch
//   * D3D clip depth is 0..1 but the main scene pass rasterises the plain GL view-projection (kD3DClipFix is NOT applied there, like
//     Metal): the landscape takes the same matrix, so its depth is the GL ndc z, 0..1 for the half of the frustum that is not clipped
// The C++ mirror of the two blocks is D3D_Shared/LandscapeD3D.h (a static_assert on their sizes lives there).
inline constexpr const char* kLandscapeHLSL = R"HLSL(
cbuffer LsGlobals : register(b12)
{
    float4x4 viewProj;       // origin-relative world to clip, the GL-convention matrix of the scene pass (depth -1..1)
    float4   cameraRel;      // xyz = camera, origin-relative; w = distance of the camera to the height slab
    float4   world;          // x = world max x, y = world max z (absolute), z = tile size, w = metres of a height of 65535
    float4   heightInfo;     // x = metres of height 0, y = tiles in x, z = tiles in z, w = slot table window
    float4   origin;         // xyz = floating origin
    int4     tile;           // x = height cells of a tile at level 0, y = weight texels, z = tile levels
    int4     heightRect[8];
    int4     coarseGrid[16];
    float4   layerColor[12];
    float4   sunDir;         // xyz = towards the sun
    float4   sunColor;
    float4   ambient;
    float4   tex;            // x = 1 / metres of one texture tile, y = 1 when the albedo array is bound, zw = fract(origin.xz * x)
};

cbuffer LsPatch : register(b13)
{
    int4   lsPatch;          // x = level, y/z = global cell of the node's corner, w = 1 coarse / 0 tiles
    float4 patchPos;         // x/y = corner, origin-relative, z = node size, w = coarse grid index
    float4 morph;            // x = morph start distance, y = morph end distance
};

Texture2DArray<uint>   tileHeights   : register(t32);
Texture2D<uint>        slotTable     : register(t33);
Texture2D<uint>        coarseHeights : register(t34);
Texture2DArray<float4> tileW0        : register(t35);
Texture2DArray<float4> tileW1        : register(t36);
Texture2DArray<float4> tileW2        : register(t37);
Texture2D<float4>      coarseW0      : register(t38);
Texture2D<float4>      coarseW1      : register(t39);
Texture2D<float4>      coarseW2      : register(t40);
Texture2DArray<float4> albedoArr     : register(t41);
SamplerState           albedoSmp     : register(s13);

struct LsVertexOut
{
    float4 position : SV_Position;
    float3 worldRel : TEXCOORD0;
};

float lsHeightOfCoarseGrid(int2 g, int grid)
{
    int4 gr = coarseGrid[grid];
    int2 c = clamp(g, int2(0, 0), gr.xy - int2(1, 1));
    uint v = coarseHeights.Load(int3(c.x, gr.z + c.y, 0)).r;
    return heightInfo.x + float(v) * (world.w / 65535.0);
}

float lsHeightOfTile(int2 g, int level)
{
    int cpt = tile.x >> level;
    int2 maxTile = int2(int(heightInfo.y), int(heightInfo.z)) - int2(1, 1);
    int2 t = min(g / cpt, maxTile);
    int2 l = min(g - t * cpt, int2(cpt, cpt));
    int mask = int(heightInfo.w) - 1;
    [loop] for (int k = 0; k < 4; ++k)
    {
        int2 stp = int2(k & 1, k >> 1);
        if ((stp.x != 0 && l.x != 0) || (stp.y != 0 && l.y != 0)) continue;
        int2 tt = t - stp;
        if (tt.x < 0 || tt.y < 0) continue;
        uint slot = slotTable.Load(int3(tt.x & mask, tt.y & mask, 0)).r;
        if (slot == 65535u) continue;
        int2 ll = l + stp * cpt;
        int4 r = heightRect[level];
        uint v = tileHeights.Load(int4(r.x + ll.x, r.y + ll.y, int(slot), 0)).r;
        return heightInfo.x + float(v) * (world.w / 65535.0);
    }
    return lsHeightOfCoarseGrid(g >> (tile.z + 1 - level), 0);
}

float lsHeightAt(int2 g)
{
    return lsPatch.w == 0 ? lsHeightOfTile(g, lsPatch.x) : lsHeightOfCoarseGrid(g, int(patchPos.w));
}

LsVertexOut VSMain(uint vid : SV_VertexID)
{
    int2 ij = int2(int(vid % 65u), int(vid / 65u));
    float cell = patchPos.z / 64.0;
    float2 xz = patchPos.xy + float2(ij) * cell;
    int2 g = lsPatch.yz + ij;
    float h = lsHeightAt(g);

    // CDLOD morph. The distance is horizontal plus the slab term, NOT the vertex's own height.
    float2 d2 = xz - cameraRel.xz;
    float d = sqrt(dot(d2, d2) + cameraRel.w * cameraRel.w);
    float m = clamp((d - morph.x) / max(morph.y - morph.x, 1e-3), 0.0, 1.0);
    bool oi = (ij.x & 1) == 1;
    bool oj = (ij.y & 1) == 1;
    if (m > 0.0 && (oi || oj))
    {
        int2 a, b;
        if (oi && !oj)      { a = g + int2(-1, 0);  b = g + int2(1, 0); }
        else if (!oi && oj) { a = g + int2(0, -1);  b = g + int2(0, 1); }
        else                { a = g + int2(-1, -1); b = g + int2(1, 1); }   // on the triangulation's diagonal
        h = lerp(h, 0.5 * (lsHeightAt(a) + lsHeightAt(b)), m);
    }
    LsVertexOut o;
    o.worldRel = float3(xz.x, h - origin.y, xz.y);
    o.position = mul(viewProj, float4(o.worldRel, 1.0));
    return o;
}

float4 PSMain(LsVertexOut pin) : SV_Target
{
    // The derivatives first: nothing that follows is allowed to make them divergent.
    float3 dx = ddx(pin.worldRel);
    float3 dy = ddy(pin.worldRel);
    // Layers 0-3 are the texture group's grass / dirt / rock / snow slices in world coordinates (see the GLSL); sampled here, in uniform flow.
    float2 uv = pin.worldRel.xz * tex.x + tex.zw;
    float3 t0 = albedoArr.Sample(albedoSmp, float3(uv, 0.0)).rgb;
    float3 t1 = albedoArr.Sample(albedoSmp, float3(uv, 1.0)).rgb;
    float3 t2 = albedoArr.Sample(albedoSmp, float3(uv, 2.0)).rgb;
    float3 t3 = albedoArr.Sample(albedoSmp, float3(uv, 3.0)).rgb;
    float2 abs2 = pin.worldRel.xz + origin.xz;
    if (abs2.x > world.x || abs2.y > world.y || abs2.x < 0.0 || abs2.y < 0.0) discard;

    int2 tl = int2(floor(abs2 / world.z));
    tl = clamp(tl, int2(0, 0), int2(int(heightInfo.y), int(heightInfo.z)) - int2(1, 1));
    int mask = int(heightInfo.w) - 1;
    uint slot = slotTable.Load(int3(tl.x & mask, tl.y & mask, 0)).r;

    float4 w0, w1, w2;
    if (slot == 65535u)
    {
        // A tile that is not in the atlas (the coarse levels, or one still streaming): the coarse world's weights, one texel per 128 m.
        uint cw, ch;
        coarseW0.GetDimensions(cw, ch);
        int2 cs = int2(int(cw), int(ch));
        int2 c = clamp(int2(floor(abs2 / world.xy * float2(cs))), int2(0, 0), cs - int2(1, 1));
        w0 = coarseW0.Load(int3(c, 0));
        w1 = coarseW1.Load(int3(c, 0));
        w2 = coarseW2.Load(int3(c, 0));
    }
    else
    {
        float texel = world.z / float(tile.y);
        int2 t = clamp(int2(floor((abs2 - float2(tl) * world.z) / texel)), int2(0, 0), int2(tile.y - 1, tile.y - 1));
        int4 at = int4(t, int(slot), 0);
        w0 = tileW0.Load(at);
        w1 = tileW1.Load(at);
        w2 = tileW2.Load(at);
    }
    float3 c0 = layerColor[0].rgb, c1 = layerColor[1].rgb, c2 = layerColor[2].rgb, c3 = layerColor[3].rgb;
    if (tex.y > 0.5)
    {
        c0 = t0; c1 = t1; c2 = t2; c3 = t3;
    }
    float3 albedo = w0.x * c0 + w0.y * c1 + w0.z * c2 + w0.w * c3
                  + w1.x * layerColor[4].rgb + w1.y * layerColor[5].rgb + w1.z * layerColor[6].rgb + w1.w * layerColor[7].rgb
                  + w2.x * layerColor[8].rgb + w2.y * layerColor[9].rgb + w2.z * layerColor[10].rgb + w2.w * layerColor[11].rgb;
    float3 n = normalize(cross(dx, dy));
    if (n.y < 0.0) n = -n;
    float ndl = max(dot(n, normalize(sunDir.xyz)), 0.0);
    return float4(albedo * (ambient.xyz + sunColor.xyz * ndl), 1.0);
}
)HLSL";

} // namespace HE::glsl
