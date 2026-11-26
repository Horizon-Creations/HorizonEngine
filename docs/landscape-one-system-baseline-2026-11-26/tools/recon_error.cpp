#include <cstdio>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

#include <HorizonRendering/ResolveReconstruct.h>

// The deferred resolve's world position with the camera far from the origin (topic 355, step 3; the black tiles of Catania view 5).
// The shader computes invViewProj * clip in float and divides by w. Done with the full inverse, the GPU has to cancel numbers of the
// size camera / near down to a position, so a ground pixel 500 m away came back metres too low at 70 km: the tile ground, which
// casts into the shadow cascades, then shadowed itself. HE::setResolveCameraRelative takes the camera's translation out of the matrix
// and hands it over as camOrigin. These are the shader's two lines, done in float on the CPU.
namespace
{
struct Hit { glm::vec3 truth; glm::vec3 clipNdc; };

// A ground pixel (y = 0) seen from `cam`: its true world position (double) and the clip-space point a rasteriser would have
// stored, depth rounded to the float the depth buffer holds.
Hit groundPixel(const glm::mat4& viewProj, const glm::dvec3& cam, const glm::dmat4& viewD, const glm::dmat4& projD, double ndcX, double ndcY)
{
	const glm::dmat4 inv = glm::inverse(projD * viewD);
	glm::dvec4 a = inv * glm::dvec4(ndcX, ndcY, -1.0, 1.0), b = inv * glm::dvec4(ndcX, ndcY, 1.0, 1.0);
	const glm::dvec3 p0 = glm::dvec3(a) / a.w, p1 = glm::dvec3(b) / b.w, d = glm::normalize(p1 - p0);
	const glm::dvec3 truth = cam + d * (-cam.y / d.y);
	const glm::dvec4 c = glm::dmat4(viewProj) * glm::dvec4(truth, 1.0);
	return { glm::vec3(truth), glm::vec3(float(c.x / c.w), float(c.y / c.w), float(c.z / c.w)) };
}

struct Cam
{
	glm::dvec3 pos;
	glm::dmat4 view, proj;
	glm::mat4  viewProj;   // float, as the renderer holds it
	explicit Cam(double x, double z)
	{
		pos = glm::dvec3(x, 300.0, z);
		view = glm::lookAt(pos, pos + glm::dvec3(-0.9, -0.1, 0.4), glm::dvec3(0, 1, 0));
		proj = glm::perspective(glm::radians(60.0), 2002.0 / 954.0, 0.1, 5000.0);
		viewProj = glm::mat4(proj) * glm::mat4(view);
	}
};

// The shader, in float: P = camOrigin + (invViewProj * clip).xyz / w
glm::vec3 reconstruct(const HE::MaterialShaderLibrary::ResolveUniforms& ru, const glm::vec3& clip)
{
	glm::mat4 inv;
	std::memcpy(&inv[0][0], ru.invViewProj, sizeof(ru.invViewProj));
	const glm::vec4 wp = inv * glm::vec4(clip, 1.0f);
	return glm::vec3(ru.camOrigin[0], ru.camOrigin[1], ru.camOrigin[2]) + glm::vec3(wp) / std::max(wp.w, 1e-8f);
}
} // namespace


int main(){
 for (double c : {1000.0, 30000.0, 60000.0, 70000.0, 75000.0, 90000.0}) {
  const Cam cam(c,c);
  for (double ndcY : {-0.5,-0.75,-0.95}) {
    const Hit h = groundPixel(cam.viewProj, cam.pos, cam.view, cam.proj, 0.0, ndcY);
    HE::MaterialShaderLibrary::ResolveUniforms a; const glm::mat4 inv = glm::inverse(cam.viewProj); std::memcpy(a.invViewProj,&inv[0][0],sizeof(a.invViewProj));
    HE::MaterialShaderLibrary::ResolveUniforms r; HE::setResolveCameraRelative(r, cam.viewProj, glm::vec3(cam.pos));
    glm::vec3 pa=reconstruct(a,h.clipNdc), pr=reconstruct(r,h.clipNdc);
    std::printf("cam %6.0f ndcY %+.2f  dist %6.1f  absolute: dy %+8.3f |err| %8.3f   camera-relative: dy %+7.3f |err| %7.3f\n", c, ndcY, glm::length(h.truth-glm::vec3(cam.pos)), pa.y-h.truth.y, glm::length(pa-h.truth), pr.y-h.truth.y, glm::length(pr-h.truth));
  }}
}
