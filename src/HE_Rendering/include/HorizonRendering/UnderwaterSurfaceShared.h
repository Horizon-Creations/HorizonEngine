#pragma once
#include <Physics/WaterWaves.h>
#include <cstdio>
#include <cstring>
#include <string>

// The height of the TESSELLATED water surface, in the post passes' own shaders (underwater fog and
// caustics). The passes used to treat the surface as a flat plane at one height; with a landscape's
// water tessellation on the real surface is the three wave trains of Physics/WaterWaves.h
// (HE::water::tessellatedHeight is the CPU twin of the function below, same formula, same clock), so the
// pass asks the surface's height AT each pixel instead, and the waterline on the screen follows the waves.
//
//   float heSurfaceH(V2 xz, float groundDepth, V4 wv, V4 sp)
//     wv = (turn radians, length factor, speed factor, steepness factor)   the wind (water::windTrain)
//     sp = (height, shoreWidth, wave clock seconds, flat level)            the body (TessellationParams)
//   returns the metres the surface stands above sp.w at xz, 0 when sp.x (height) is 0: a flat plane
//   exactly as before, which is what every backend that cannot tessellate sends.
//
// One text for every language: only the vector types differ (@V2@, @V4@). The three base trains are spliced
// in from kWaveA/B/C, like causticsWaveTrainsList does for the caustic net; the offline-compiled Vulkan
// shader carries a copy of the same text (a test reads it back and compares).
namespace HE
{
    enum class SurfaceLang { GLSL, HLSL, MSL };

    inline std::string underwaterSurfaceFunctions(SurfaceLang lang)
    {
        auto lit = [](float v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
            if (!std::strpbrk(buf, ".eE")) std::strncat(buf, ".0", sizeof(buf) - std::strlen(buf) - 1);
            return std::string(buf);
        };
        std::string s =
            "float heTrain(@V2@ xz, float dirDeg, float speed, float len, float steep, @V4@ wv, @V4@ sp)\n"
            "{\n"
            "    float L = len * wv.y;\n"
            "    float k = 6.28318531 / L;\n"
            "    float rad = dirDeg * 0.0174532925 + wv.x;\n"
            "    float ph = k * (cos(rad) * xz.x + sin(rad) * xz.y - speed * wv.z * sp.z);\n"
            "    return steep * wv.w / k * sin(ph);\n"
            "}\n"
            "float heSurfaceH(@V2@ xz, float groundDepth, @V4@ wv, @V4@ sp)\n"
            "{\n"
            "    if (sp.x <= 0.0) return 0.0;\n"
            "    float u = clamp(groundDepth / clamp(sp.y, 0.1, 3.0), 0.0, 1.0);\n"
            "    float shore = u * u * (3.0 - 2.0 * u);\n"
            "    float h = @A@ + @B@ + @C@;\n"
            "    return sp.x * shore * h;\n"
            "}\n";
        auto train = [&](const water::WaveTrain& t) {
            return "heTrain(xz, " + lit(t.directionDeg) + ", " + lit(t.speed) + ", " + lit(t.length) + ", " +
                   lit(t.steepness) + ", wv, sp)";
        };
        auto replace = [&](const std::string& from, const std::string& to) {
            for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
                s.replace(p, from.size(), to);
        };
        replace("@A@", train(water::kWaveA));
        replace("@B@", train(water::kWaveB));
        replace("@C@", train(water::kWaveC));
        replace("@V2@", lang == SurfaceLang::GLSL ? "vec2" : "float2");
        replace("@V4@", lang == SurfaceLang::GLSL ? "vec4" : "float4");
        return s;
    }
}
