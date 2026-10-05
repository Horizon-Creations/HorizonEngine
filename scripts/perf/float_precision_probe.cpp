// Float-precision probe for the world-streaming baseline (Thema 153).
//
// The engine keeps positions in glm::vec3 (float), composes mvp = viewProj *
// model on the CPU in float and has no camera-relative rendering or floating
// origin. This probe replays exactly that math with the engine's own glm at
// growing distances from the origin and compares it against a double reference,
// so "how large can a world be" gets a number instead of a guess.
//
//   clang++ -std=c++17 -O2 -I src/HE_Rendering/glm \
//       scripts/perf/float_precision_probe.cpp -o /tmp/float_precision_probe
//   /tmp/float_precision_probe
//
// Columns:
//   ulp_mm      spacing of float at that coordinate (smallest representable step)
//   step_err%   error of one 60 Hz walking step (1.4 m/s => 23.3 mm) added to a
//               float position, i.e. how badly `pos += v * dt` quantises motion
//   jitter_px   worst screen-space error (pixels, 1920x1080, 60 deg fov) of the
//               8 corners of a 1 m cube 5 m in front of the camera, float vs double
//   depth_ok    whether the cube's float NDC depth still orders front/back faces
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
double ulpAt(double x)
{
    const float f = static_cast<float>(x);
    return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity())) - f;
}

template<typename T>
glm::mat<4, 4, T> mvpAt(glm::vec<3, T> cam, glm::vec<3, T> obj, T yawDeg)
{
    using V3 = glm::vec<3, T>;
    using M4 = glm::mat<4, 4, T>;
    const V3 fwd(std::sin(glm::radians(yawDeg)), T(-0.1), -std::cos(glm::radians(yawDeg)));
    const M4 view  = glm::lookAt(cam, cam + fwd, V3(0, 1, 0));
    const M4 proj  = glm::perspective(glm::radians(T(60)), T(1920) / T(1080), T(0.1), T(1000));
    M4 model = glm::translate(M4(T(1)), obj);
    model    = glm::rotate(model, glm::radians(T(33)), V3(0, 1, 0));
    return proj * view * model;   // same order as the renderer: viewProj * transform
}

void row(double d)
{
    // Camera and object both sit at distance d along a diagonal, so x and z
    // carry the large component (the worst case for a ground-plane world).
    const double s = d / std::sqrt(2.0);

    // One placement says little — rounding error depends on where inside a
    // float step the value lands — so take the worst of 64 sub-step offsets.
    double maxPx = 0.0;
    bool depthOk = true;
    for (int k = 0; k < 64; ++k)
    {
        const double f = k / 64.0;
        const glm::dvec3 camD(s + 0.37 + f, 1.8, s + 0.61 + 0.7 * f);
        const glm::dvec3 objD = camD + glm::dvec3(0.013 * k, -0.5, -5.0);
        const double yaw = 3.0;

        const glm::dmat4 mD = mvpAt<double>(camD, objD, yaw);
        const glm::mat4  mF = mvpAt<float>(glm::vec3(camD), glm::vec3(objD), static_cast<float>(yaw));

        double zFace[2] = { 0, 0 };
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec4 p((c & 1) ? 0.5 : -0.5, (c & 2) ? 0.5 : -0.5, (c & 4) ? 0.5 : -0.5, 1.0);
            const glm::dvec4 cd = mD * p;
            const glm::vec4  cf = mF * glm::vec4(p);
            const double xd = cd.x / cd.w * 960.0, yd = cd.y / cd.w * 540.0;
            const double xf = double(cf.x) / cf.w * 960.0, yf = double(cf.y) / cf.w * 540.0;
            maxPx = std::fmax(maxPx, std::hypot(xf - xd, yf - yd));
            // corner 0 (z=-0.5) lies on the far face, corner 4 (z=+0.5) on the near face
            if (c == 0) zFace[0] = double(cf.z) / cf.w;
            if (c == 4) zFace[1] = double(cf.z) / cf.w;
        }
        depthOk = depthOk && zFace[1] < zFace[0];
    }

    // Same worst-of-64 placement as above for the walking step.
    const double step = 1.4 / 60.0;
    double stepErr = 0.0;
    for (int k = 0; k < 64; ++k)
    {
        const float  pf    = static_cast<float>(s + k / 64.0);
        const double moved = double(pf + static_cast<float>(step)) - double(pf);
        stepErr = std::fmax(stepErr, std::fabs(moved - step) / step * 100.0);
    }

    std::printf("%10.0f  %10.3f  %9.1f  %10.3f  %s\n", d, ulpAt(s) * 1000.0, stepErr, maxPx,
                depthOk ? "yes" : "NO");
}
} // namespace

int main()
{
    std::printf("%10s  %10s  %9s  %10s  %s\n", "dist_m", "ulp_mm", "step_err%", "jitter_px", "depth_ok");
    for (double d : { 100.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0, 32000.0, 65000.0,
                      100000.0, 250000.0, 1000000.0, 10000000.0 })
        row(d);
    return 0;
}
