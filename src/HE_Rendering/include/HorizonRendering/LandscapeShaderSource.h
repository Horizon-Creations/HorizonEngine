#pragma once

// ─── The world-scale landscape's shaders (Thema 326, step 2, part A) ─────────
// One 65 x 65 grid (HE::LandscapePatchMesh) is drawn once per patch of the quadtree (HE::LandscapeFrame::Patch); the vertex
// shader turns a grid position into a world position and takes the height from the tile atlas or, for the coarse levels, from
// the coarse world. GLSL 4.10 with loose uniforms, like kSkyFS: a Vulkan / D3D build gets a prelude that maps the uniform names
// onto a block (part B), the body stays as it is, so it must stay free of layout qualifiers and GL-only built-ins.
//
// What the contract says (HE::LandscapeGpuLayout.h has the image layouts, HE::LandscapeFrame the numbers):
//   * Heights are read with texelFetch at integer lattice positions, never filtered: a vertex of a level-L patch sits on the
//     global lattice of cell size cell0 * 2^L, so its cell coordinates are integers (the patch's corner cell plus the grid index).
//   * The MORPH distance is horizontal distance to the vertex plus uCameraRel.w, the camera's distance to the height slab; it
//     does NOT include the vertex's own height. The selection on the CPU (landscapeMorphDistance) uses the same metric, which is
//     what makes the seams between levels exact. A shader that morphs by the true 3D distance opens cracks.
//   * An odd vertex morphs toward the middle of its neighbours on the SAME level (landscapeMorphSource): 1 or 2 more fetches.
//   * A fragment beyond the world's absolute extent is discarded: the top node of the quadtree is bigger than the world, and its
//     vertices past the edge read the last row of heights.
//   * Normals: derivatives of the interpolated world position in the fragment shader. Flat shaded at the patch's resolution, no
//     normal texture; a smooth normal is part B.
//   * Weights: the fragment's own tile, nearest texel, three RGBA8 pages, flat layer colours. The material graph / auto-landscape
//     material (which assumes one UV over the whole landscape, uvTiling) is part B and needs world-space UVs at this size.
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

float heightOfTile(ivec2 g, int level)
{
    int cpt = uTile.x >> level;      // cells of a tile at this mip
    ivec2 maxTile = ivec2(int(uHeightInfo.y), int(uHeightInfo.z)) - ivec2(1);
    ivec2 t = min(g / cpt, maxTile);
    ivec2 l = min(g - t * cpt, ivec2(cpt));   // the world's last row/column: the tile's last sample
    int mask = int(uHeightInfo.w) - 1;
    uint slot = texelFetch(uSlotTable, ivec2(t.x & mask, t.y & mask), 0).r;
    if (slot == 65535u) return uHeightInfo.x;
    ivec4 r = uHeightRect[level];
    uint v = texelFetch(uTileHeights, ivec3(r.x + l.x, r.y + l.y, int(slot)), 0).r;
    return uHeightInfo.x + float(v) * (uWorld.w / 65535.0);
}

float heightOfCoarse(ivec2 g)
{
    ivec4 gr = uCoarseGrid[int(uPatchPos.w)];
    ivec2 c = clamp(g, ivec2(0), gr.xy - ivec2(1));
    uint v = texelFetch(uCoarseHeights, ivec2(c.x, gr.z + c.y), 0).r;
    return uHeightInfo.x + float(v) * (uWorld.w / 65535.0);
}

float heightAt(ivec2 g)
{
    return uPatch.w == 0 ? heightOfTile(g, uPatch.x) : heightOfCoarse(g);
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
uniform usampler2D      uSlotTable;
uniform sampler2DArray  uTileWeights0;   // RGBA8, layers 0-3 of every tile's twelve
uniform sampler2DArray  uTileWeights1;
uniform sampler2DArray  uTileWeights2;

void main()
{
    vec2 abs2 = vWorldRel.xz + uOrigin.xz;
    if (abs2.x > uWorld.x || abs2.y > uWorld.y || abs2.x < 0.0 || abs2.y < 0.0) discard;

    ivec2 tile = ivec2(floor(abs2 / uWorld.z));
    tile = clamp(tile, ivec2(0), ivec2(int(uHeightInfo.y), int(uHeightInfo.z)) - ivec2(1));
    int mask = int(uHeightInfo.w) - 1;
    uint slot = texelFetch(uSlotTable, ivec2(tile.x & mask, tile.y & mask), 0).r;

    vec3 albedo = vec3(0.45);                      // a tile that is not in the atlas (the coarse levels): neutral
    if (slot != 65535u)
    {
        float texel = uWorld.z / float(uTile.y);
        ivec2 t = clamp(ivec2(floor((abs2 - vec2(tile) * uWorld.z) / texel)), ivec2(0), ivec2(uTile.y - 1));
        ivec3 at = ivec3(t, int(slot));
        vec4 w0 = texelFetch(uTileWeights0, at, 0);
        vec4 w1 = texelFetch(uTileWeights1, at, 0);
        vec4 w2 = texelFetch(uTileWeights2, at, 0);
        albedo = w0.x * uLayerColor[0].rgb + w0.y * uLayerColor[1].rgb + w0.z * uLayerColor[2].rgb + w0.w * uLayerColor[3].rgb
               + w1.x * uLayerColor[4].rgb + w1.y * uLayerColor[5].rgb + w1.z * uLayerColor[6].rgb + w1.w * uLayerColor[7].rgb
               + w2.x * uLayerColor[8].rgb + w2.y * uLayerColor[9].rgb + w2.z * uLayerColor[10].rgb + w2.w * uLayerColor[11].rgb;
    }
    vec3 n = normalize(cross(dFdx(vWorldRel), dFdy(vWorldRel)));
    if (n.y < 0.0) n = -n;
    float ndl = max(dot(n, normalize(uSunDir)), 0.0);
    fragColor = vec4(albedo * (uAmbient + uSunColor * ndl), 1.0);
}
)GLSL";

} // namespace HE::glsl
