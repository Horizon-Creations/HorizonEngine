#include "Backends/OpenGL/OpenGLRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cstring>
#include <Diagnostics/Logger.h>
#include <glm/gtc/type_ptr.hpp>

// ─── Embedded unlit shader ────────────────────────────────────────────────────
// GLSL 410: the macOS Core Profile ceiling — works everywhere we run.
static const char* kUnlitVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;
void main()
{
	vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
	vNormal     = mat3(uModel) * aNormal;
	vUV         = aUV;
	gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

// Blinn-Phong over up to 8 scene lights. Scenes without lights fall back to
// the fixed "headlight" so nothing renders black.
static const char* kUnlitFS = R"GLSL(
#version 410 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;

const int MAX_LIGHTS = 8;
uniform int  uLightCount;
uniform vec4 uLightPos[MAX_LIGHTS];    // xyz = position,  w = type (0 dir / 1 point / 2 spot)
uniform vec4 uLightDir[MAX_LIGHTS];    // xyz = direction, w = cos(spot half angle)
uniform vec4 uLightColor[MAX_LIGHTS];  // rgb = color,     w = intensity
uniform vec4 uLightParams[MAX_LIGHTS]; // x = range
uniform vec3 uCameraPos;

uniform vec3      uColor;       // base-color tint (material baseColor, or flat fallback)
uniform bool      uHasTexture;
uniform sampler2D uTexture;
uniform float     uMetallic;    // 0 dielectric … 1 metal
uniform float     uRoughness;   // 0 mirror … 1 fully rough
uniform vec3      uSunDir;      // direction toward the sun (for image-based ambient)
uniform vec3      uAmbient;     // flat ambient fill (never-black floor + overcast)

// shared skyColor() is injected at the marker below (CreateUnlitPipeline)
//#SKYFUNC#

// Directional-light shadow map
uniform mat4      uLightVP;
uniform sampler2D uShadowMap;
uniform int       uShadowEnabled;

out vec4 FragColor;

float computeShadow(vec3 worldPos, vec3 N, vec3 L)
{
	if (uShadowEnabled == 0) return 1.0;
	vec4 lp = uLightVP * vec4(worldPos, 1.0);
	vec3 p  = lp.xyz / lp.w;
	p = p * 0.5 + 0.5;                       // NDC [-1,1] → [0,1]
	if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
		return 1.0;                          // outside the map → lit
	// Slope-scaled bias: grows toward grazing sun angles (low sun / day-night
	// sunsets) to stop shadow acne, clamped so high sun keeps crisp contact.
	float ndl     = clamp(dot(N, L), 0.0, 1.0);
	float bias    = clamp(0.0016 * tan(acos(ndl)), 0.0005, 0.02);
	// 3×3 PCF: averaging neighbouring texels softens the edge and hides the
	// per-texel flicker the hard test produced as the day-night light rotates.
	vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float c = texture(uShadowMap, p.xy + vec2(x, y) * texel).r;
			vis += (p.z - bias > c) ? 0.0 : 1.0;
		}
	vis /= 9.0;
	return mix(0.35, 1.0, vis);
}

void main()
{
	vec3 albedo = uHasTexture ? texture(uTexture, vUV).rgb * uColor : uColor;
	vec3 N      = normalize(vNormal);

	if (uLightCount == 0)
	{
		vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
		float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		FragColor  = vec4(albedo * diff, 1.0);
		return;
	}

	// Metallic-roughness split: metals lose diffuse and tint the specular F0;
	// roughness widens + dims the Blinn-Phong highlight (cheap PBR stand-in).
	vec3  diffuseColor = albedo * (1.0 - uMetallic);
	vec3  specColor    = mix(vec3(0.04), albedo, uMetallic);
	float shininess    = mix(128.0, 8.0, uRoughness);
	float specScale    = mix(0.5, 0.03, uRoughness);

	vec3 V = normalize(uCameraPos - vWorldPos);

	// Image-based ambient from the procedural sky (replaces the flat floor):
	// diffuse from the surface normal, specular from the reflection vector
	// (bent toward the normal as roughness grows = crude prefilter).
	vec3 Rrough  = normalize(mix(reflect(-V, N), N, uRoughness));
	vec3 ambDiff = skyColor(N, uSunDir)      * diffuseColor;
	vec3 ambSpec = skyColor(Rrough, uSunDir) * specColor;
	vec3 result  = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * uRoughness);
	// Flat ambient fill (never-black floor + overcast replacement for the
	// switched-off sun/moon light), applied to the diffuse albedo.
	result += uAmbient * diffuseColor;

	for (int i = 0; i < uLightCount; ++i)
	{
		int   type  = int(uLightPos[i].w);
		vec3  L;
		float atten = 1.0;

		if (type == 0) // directional
			L = normalize(-uLightDir[i].xyz);
		else
		{
			vec3  d    = uLightPos[i].xyz - vWorldPos;
			float dist = max(length(d), 1e-4);
			L = d / dist;
			float range = max(uLightParams[i].x, 1e-4);
			atten = clamp(1.0 - dist / range, 0.0, 1.0);
			atten *= atten;
			if (type == 2) // spot cone
			{
				float c = dot(-L, normalize(uLightDir[i].xyz));
				float cosCone = uLightDir[i].w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
		}

		// Only the (first) directional light casts shadows.
		float sh = (type == 0) ? computeShadow(vWorldPos, N, L) : 1.0;

		float diff = max(dot(N, L), 0.0);
		vec3  H    = normalize(L + V);
		float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;
		result += (diffuseColor * diff + specColor * spec)
		        * uLightColor[i].rgb * uLightColor[i].w * atten * sh;
	}
	FragColor = vec4(result, 1.0);
}
)GLSL";

// ─── Procedural skybox (drawn into the HDR target behind the scene) ─────────
// Fullscreen triangle at the far plane; reconstructs a world-space ray per
// pixel from the inverse view-projection and evaluates the same skyColor() the
// scene shader uses for ambient, so background and reflections match.
static const char* kSkyVS = R"GLSL(
#version 410 core
out vec2 vNDC;
void main()
{
	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0,
	              float((gl_VertexID & 2) << 1) - 1.0);
	vNDC = p;
	gl_Position = vec4(p, 1.0, 1.0); // z = far plane
}
)GLSL";

static const char* kSkyFS = R"GLSL(
#version 410 core
in vec2 vNDC;
uniform mat4 uInvViewProj;
uniform vec3 uSunDir;
uniform sampler2D uMoonTex;
uniform bool      uHasMoonTex;
uniform float     uTimeOfDay;   // cloud scroll phase (0..1)
uniform float     uCloudCoverage; // cloud amount (0 = clear … 1 = full overcast)
out vec4 FragColor;
//#SKYFUNC#

// Textured moon disk — drawn only in the sky pass (kept out of the shared
// skyColor() so the scene's image-based ambient needn't bind the texture).
// Smaller than the sun and shaded as a sphere so the grayscale map reads as
// craters on a lit body. Falls back to a plain disk when no texture is set.
vec3 moonDisk(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day   = smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	float night = 1.0 - day;
	if (night <= 0.0) return vec3(0.0);

	vec3 moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return vec3(0.0);

	// Local tangent frame so the disk gets 2D UVs for the texture.
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), moonDir));
	vec3 up    = cross(moonDir, right);
	const float kRadius = 0.030;                   // angular radius (< the sun disk)
	vec2  q = vec2(dot(dir, right), dot(dir, up)) / kRadius;
	float r = length(q);
	if (r > 1.0) return vec3(0.0);

	float tex  = uHasMoonTex ? texture(uMoonTex, q * 0.5 + 0.5).r : 1.0;
	float limb = sqrt(max(1.0 - r * r, 0.0));      // spherical brightness falloff
	float edge = smoothstep(1.0, 0.90, r);         // soft anti-aliased rim
	vec3  tint = vec3(0.92, 0.94, 1.00);
	return tint * tex * limb * edge * 3.0 * night;
}

// Procedural star field — drawn only in the sky pass (like the moon). Fades in
// at night, sits above the horizon and is occluded by clouds (applied before
// applyClouds()). Each view ray lands in one cell of a fixed grid on a large
// sphere shell (stable, pole-skew free); the rarest cells host a small round
// star at a hashed sub-cell position. Mirrors the Metal starField() exactly.
float starHash(vec3 p)
{
	p  = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}
vec3 starField(vec3 dir, vec3 sunDir, float timeOfDay)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

	vec3  p       = dir * 70.0;
	vec3  cell    = floor(p);
	float present = starHash(cell);
	if (present < 0.92) return vec3(0.0);          // keep only the rarest cells = stars

	vec3  sp   = vec3(starHash(cell + 1.7), starHash(cell + 4.3), starHash(cell + 8.9));
	float d    = length(fract(p) - sp);
	float core = smoothstep(0.25, 0.0, d);
	core *= core;                                  // tighten the core, keep a faint glow
	float mag  = 0.4 + 0.6 * smoothstep(0.92, 1.0, present);            // per-star brightness
	float tw   = 0.75 + 0.25 * sin(timeOfDay * 40.0 + present * 6.2831); // gentle twinkle
	float horizon = smoothstep(0.0, 0.15, dir.y);  // fade into the horizon haze
	vec3  tint = mix(vec3(0.80, 0.88, 1.0), vec3(1.0, 0.93, 0.82), starHash(cell + 12.1));
	return tint * (core * mag * tw * horizon * night * 1.6);
}

// Procedural clouds — drawn only in the sky pass (kept out of the shared
// skyColor() so the scene's image-based ambient stays cheap). A scrolling FBM
// over a flat cloud layer drifts with the time of day and is lit/tinted by the
// day-night cycle. Mirrors the Metal applyClouds() exactly.
float cloudHash(vec2 p)
{
	p  = fract(p * vec2(127.1, 311.7));
	p += dot(p, p + 34.56);
	return fract(p.x * p.y);
}
float cloudNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = cloudHash(i);
	float b = cloudHash(i + vec2(1.0, 0.0));
	float c = cloudHash(i + vec2(0.0, 1.0));
	float d = cloudHash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float cloudFbm(vec2 p)
{
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 5; ++i)
	{
		v += a * cloudNoise(p);
		p  = p * 2.02;
		a *= 0.5;
	}
	return v;
}
vec3 applyClouds(vec3 baseSky, vec3 dir, vec3 sunDir, float timeOfDay, float coverage)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

	// Project the ray onto a flat cloud layer (compresses toward the horizon).
	vec2 uv     = dir.xz / dir.y * 0.5;
	vec2 scroll = vec2(timeOfDay * 8.0, timeOfDay * 2.0); // drift across the day
	// Coverage slider lowers the density threshold: 0 = clear, 1 = full overcast.
	float lo    = mix(0.95, 0.05, clamp(coverage, 0.0, 1.0));
	float cover = smoothstep(lo, lo + 0.35, cloudFbm(uv + scroll));
	cover *= smoothstep(0.02, 0.22, dir.y);       // fade out near the horizon

	// Cheap shading: a sun-offset sample fakes bright tops / darker undersides.
	float lit  = smoothstep(0.45, 0.95, cloudFbm(uv + scroll + sunDir.xz * 0.20));
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	vec3 dayCol   = mix(vec3(0.55, 0.58, 0.66), vec3(1.00, 0.99, 0.96), lit);
	vec3 nightCol = mix(vec3(0.05, 0.06, 0.10), vec3(0.20, 0.23, 0.32), lit);
	vec3 cloudCol = mix(nightCol, dayCol, day);
	cloudCol = mix(cloudCol, vec3(1.0, 0.62, 0.40), dusk * lit * 0.6); // warm dusk tops

	return mix(baseSky, cloudCol, cover);
}

void main()
{
	vec4 wp1 = uInvViewProj * vec4(vNDC,  1.0, 1.0);
	vec4 wp0 = uInvViewProj * vec4(vNDC, -1.0, 1.0);
	vec3 dir = wp1.xyz / wp1.w - wp0.xyz / wp0.w;
	vec3 col = skyColor(dir, uSunDir);
	col += starField(dir, uSunDir, uTimeOfDay);
	col += moonDisk(dir, uSunDir);
	col = applyClouds(col, dir, uSunDir, uTimeOfDay, uCloudCoverage);
	FragColor = vec4(col, 1.0);
}
)GLSL";

// Shared analytic sky, injected (via the //#SKYFUNC# marker) into both the
// skybox FS and the scene FS so background and image-based ambient match. The
// sky's mood is driven by the sun's elevation (sunDir.y): a daytime blue sky
// warms and reddens at the horizon as the sun sets and dims into night.
static const char* kSkyFuncGLSL = R"GLSL(
vec3 skyColor(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);                 // 0 night → 1 day
	float dusk = smoothstep(-0.06, 0.05, sunY)
	           * (1.0 - smoothstep(0.05, 0.28, sunY));          // peaks near horizon

	vec3 zenithDay  = vec3(0.08, 0.28, 0.72);
	vec3 horizDay   = vec3(0.42, 0.62, 0.88);
	vec3 zenithNite = vec3(0.012, 0.016, 0.05);
	vec3 horizNite  = vec3(0.03, 0.04, 0.10);
	vec3 zenith  = mix(zenithNite, zenithDay, day);
	vec3 horizon = mix(horizNite,  horizDay,  day);
	horizon = mix(horizon, vec3(0.95, 0.45, 0.22), dusk);       // warm sunset band

	float h    = clamp(dir.y, 0.0, 1.0);
	float grad = pow(1.0 - h, 2.5);                             // horizon-weighted
	vec3 sky = mix(zenith, horizon, grad);

	// Below the horizon: ease into a soft ground haze over a wide band so the
	// sky stays atmospheric just under the horizon line.
	vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));

	// Sun disk + glow, tinted warm near the horizon, white when high.
	vec3  sunTint = mix(vec3(1.0, 0.42, 0.20), vec3(1.0, 0.96, 0.88),
	                    smoothstep(0.0, 0.25, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	sky += sunTint * (pow(s, 1800.0) * 14.0) * day;             // crisp disk (blooms)
	sky += sunTint * (pow(s, 7.0)    * 0.18) * max(day, dusk);  // soft halo

	// Moon: opposite the sun, fading in at night. The lit disk itself is drawn
	// (textured) in the sky pass; here we keep only the soft halo and a faint
	// fill so the night ambient/reflections aren't pitch black.
	// Opposite the sun in azimuth + elevation, but kept on the same hemisphere
	// (z sign) so it rises into the visible sky rather than behind the viewer.
	float night   = 1.0 - day;
	vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	float m       = max(dot(dir, moonDir), 0.0);
	vec3  moonTint= vec3(0.80, 0.86, 1.00);
	sky += moonTint * (pow(m, 60.0)   * 0.05) * night;          // soft halo
	sky += vec3(0.04, 0.05, 0.08) * night;                      // faint moonlit fill
	return sky;
}
)GLSL";

// Depth-only shader for the shadow pass — transforms by the light's view-proj.
static const char* kDepthVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uDepthMVP;
void main() { gl_Position = uDepthMVP * vec4(aPos, 1.0); }
)GLSL";

static const char* kDepthFS = R"GLSL(
#version 410 core
void main() {}
)GLSL";

// ─── HDR tonemap (PostProcessPass) ──────────────────────────────────────────
// Fullscreen triangle generated from gl_VertexID — no vertex buffer needed.
static const char* kTonemapVS = R"GLSL(
#version 410 core
out vec2 vUV;
void main()
{
	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0,
	              float((gl_VertexID & 2) << 1) - 1.0);
	vUV = p * 0.5 + 0.5;
	gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Samples the RGBA16F scene color, adds the blurred bloom, applies exposure, the
// ACES filmic curve and sRGB gamma, then writes LDR. This is where HDR highlights
// stop clipping and where bloom glow is composited back in.
static const char* kTonemapFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uHDR;
uniform sampler2D uBloom;
uniform float     uExposure;
uniform float     uBloomStrength;
out vec4 FragColor;
vec3 aces(vec3 x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main()
{
	vec3 hdr    = texture(uHDR, vUV).rgb;
	hdr        += texture(uBloom, vUV).rgb * uBloomStrength;
	hdr        *= uExposure;
	vec3 mapped = aces(hdr);
	mapped      = pow(mapped, vec3(1.0 / 2.2));
	FragColor   = vec4(mapped, 1.0);
}
)GLSL";

// Bloom bright-pass: keep only the part of each pixel above a soft-knee
// threshold (Call-of-Duty-style curve), preserving hue. Feeds the blur chain.
static const char* kBloomBrightFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uHDR;
uniform float     uThreshold;
uniform float     uKnee;
out vec4 FragColor;
void main()
{
	vec3  c  = texture(uHDR, vUV).rgb;
	float br = max(c.r, max(c.g, c.b));
	float soft = clamp(br - uThreshold + uKnee, 0.0, 2.0 * uKnee);
	soft = (soft * soft) / (4.0 * uKnee + 1e-4);
	float contrib = max(soft, br - uThreshold) / max(br, 1e-4);
	FragColor = vec4(c * contrib, 1.0);
}
)GLSL";

// Separable 9-tap Gaussian blur. uHorizontal picks the axis; run as ping-pong
// horizontal/vertical pairs to approximate a 2D blur.
static const char* kBloomBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uImage;
uniform vec2      uTexel;       // 1 / textureSize
uniform int       uHorizontal;
out vec4 FragColor;
void main()
{
	float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
	vec2 dir = (uHorizontal == 1) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
	vec3 result = texture(uImage, vUV).rgb * w[0];
	for (int i = 1; i < 5; ++i)
	{
		result += texture(uImage, vUV + dir * float(i)).rgb * w[i];
		result += texture(uImage, vUV - dir * float(i)).rgb * w[i];
	}
	FragColor = vec4(result, 1.0);
}
)GLSL";

static GLuint CompileStage(GLenum stage, const char* src)
{
	GLuint shader = glCreateShader(stage);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		glDeleteShader(shader);
		throw std::runtime_error(std::string("OpenGLRenderer: shader compile failed: ") + log);
	}
	return shader;
}

// Replaces the //#SKYFUNC# marker with the shared skyColor() so the skybox and
// scene shaders stay in sync from one source.
static std::string injectSkyFunc(const char* src)
{
	std::string s = src;
	const std::string marker = "//#SKYFUNC#";
	if (size_t pos = s.find(marker); pos != std::string::npos)
		s.replace(pos, marker.size(), kSkyFuncGLSL);
	return s;
}

OpenGLRenderer::OpenGLRenderer()  = default;
OpenGLRenderer::~OpenGLRenderer() = default;

void OpenGLRenderer::Initialize(HE::Window* window)
{
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();
	m_glContext        = window->GetGLContext();
	if (!m_glContext)
		throw std::runtime_error("OpenGLRenderer: no GL context on window");

	if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)))
		throw std::runtime_error("OpenGLRenderer: gladLoadGLLoader failed");

	m_shaderManager = OpenGLShaderManager();

	glEnable(GL_DEPTH_TEST);
	CreateUnlitPipeline();
	CreateShadowResources();
	CreateSkyPipeline();
	CreateTonemapPipeline();
	CreateBloomPipeline();
	CreateCubeMesh();
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initialized successfully");
}

void OpenGLRenderer::CreateUnlitPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kUnlitVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());

	m_unlitProgram = glCreateProgram();
	glAttachShader(m_unlitProgram, vs);
	glAttachShader(m_unlitProgram, fs);
	glLinkProgram(m_unlitProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_unlitProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_unlitProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: program link failed: ") + log);
	}

	m_uMVP         = glGetUniformLocation(m_unlitProgram, "uMVP");
	m_uModel       = glGetUniformLocation(m_unlitProgram, "uModel");
	m_uColor       = glGetUniformLocation(m_unlitProgram, "uColor");
	m_uHasTexture  = glGetUniformLocation(m_unlitProgram, "uHasTexture");
	m_uTexture     = glGetUniformLocation(m_unlitProgram, "uTexture");
	m_uMetallic    = glGetUniformLocation(m_unlitProgram, "uMetallic");
	m_uRoughness   = glGetUniformLocation(m_unlitProgram, "uRoughness");
	m_uLightCount  = glGetUniformLocation(m_unlitProgram, "uLightCount");
	m_uLightPos    = glGetUniformLocation(m_unlitProgram, "uLightPos");
	m_uLightDir    = glGetUniformLocation(m_unlitProgram, "uLightDir");
	m_uLightColor  = glGetUniformLocation(m_unlitProgram, "uLightColor");
	m_uLightParams = glGetUniformLocation(m_unlitProgram, "uLightParams");
	m_uCameraPos   = glGetUniformLocation(m_unlitProgram, "uCameraPos");
	m_uSunDir      = glGetUniformLocation(m_unlitProgram, "uSunDir");
	m_uAmbient     = glGetUniformLocation(m_unlitProgram, "uAmbient");
	m_uLightVP       = glGetUniformLocation(m_unlitProgram, "uLightVP");
	m_uShadowMap     = glGetUniformLocation(m_unlitProgram, "uShadowMap");
	m_uShadowEnabled = glGetUniformLocation(m_unlitProgram, "uShadowEnabled");
}

void OpenGLRenderer::CreateShadowResources()
{
	// Depth-only program for the shadow pass.
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kDepthVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kDepthFS);
	m_depthProgram = glCreateProgram();
	glAttachShader(m_depthProgram, vs);
	glAttachShader(m_depthProgram, fs);
	glLinkProgram(m_depthProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);
	m_uDepthMVP = glGetUniformLocation(m_depthProgram, "uDepthMVP");

	// Depth texture + FBO. Border color 1.0 so samples outside the map read as
	// "fully lit" (depth 1).
	glGenTextures(1, &m_shadowDepthTex);
	glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize,
	             0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

	glGenFramebuffers(1, &m_shadowFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowDepthTex, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderer::CreateSkyPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSkyVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kSkyFS).c_str());
	m_skyProgram = glCreateProgram();
	glAttachShader(m_skyProgram, vs);
	glAttachShader(m_skyProgram, fs);
	glLinkProgram(m_skyProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_skyProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_skyProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: sky link failed: ") + log);
	}
	m_uSkyInvVP  = glGetUniformLocation(m_skyProgram, "uInvViewProj");
	m_uSkySunDir = glGetUniformLocation(m_skyProgram, "uSunDir");
	m_uSkyMoonTex = glGetUniformLocation(m_skyProgram, "uMoonTex");
	m_uSkyHasMoon = glGetUniformLocation(m_skyProgram, "uHasMoonTex");
	m_uSkyTime    = glGetUniformLocation(m_skyProgram, "uTimeOfDay");
	m_uSkyCoverage = glGetUniformLocation(m_skyProgram, "uCloudCoverage");
}

void OpenGLRenderer::CreateTonemapPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kTonemapFS);
	m_tonemapProgram = glCreateProgram();
	glAttachShader(m_tonemapProgram, vs);
	glAttachShader(m_tonemapProgram, fs);
	glLinkProgram(m_tonemapProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_tonemapProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_tonemapProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: tonemap link failed: ") + log);
	}
	m_uHDRTex        = glGetUniformLocation(m_tonemapProgram, "uHDR");
	m_uExposure      = glGetUniformLocation(m_tonemapProgram, "uExposure");
	m_uBloomTex      = glGetUniformLocation(m_tonemapProgram, "uBloom");
	m_uBloomStrength = glGetUniformLocation(m_tonemapProgram, "uBloomStrength");

	// Core profile needs a bound VAO for glDrawArrays even with no attributes.
	glGenVertexArrays(1, &m_fsVAO);
}

void OpenGLRenderer::CreateBloomPipeline()
{
	// Bright pass (reuses the fullscreen-triangle VS).
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBrightFS);
		m_bloomBrightProgram = glCreateProgram();
		glAttachShader(m_bloomBrightProgram, vs);
		glAttachShader(m_bloomBrightProgram, fs);
		glLinkProgram(m_bloomBrightProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		m_uBrightHDR       = glGetUniformLocation(m_bloomBrightProgram, "uHDR");
		m_uBrightThreshold = glGetUniformLocation(m_bloomBrightProgram, "uThreshold");
		m_uBrightKnee      = glGetUniformLocation(m_bloomBrightProgram, "uKnee");
	}
	// Separable blur.
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBlurFS);
		m_blurProgram = glCreateProgram();
		glAttachShader(m_blurProgram, vs);
		glAttachShader(m_blurProgram, fs);
		glLinkProgram(m_blurProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		m_uBlurImage      = glGetUniformLocation(m_blurProgram, "uImage");
		m_uBlurTexel      = glGetUniformLocation(m_blurProgram, "uTexel");
		m_uBlurHorizontal = glGetUniformLocation(m_blurProgram, "uHorizontal");
	}
}

void OpenGLRenderer::SetBloomSettings(const BloomSettings& s)
{
	m_bloomEnabled   = s.enabled;
	m_bloomThreshold = s.threshold;
	m_bloomStrength  = s.intensity;
}

void OpenGLRenderer::EnsureBloomTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_bloomFBO[0] && width == m_bloomW && height == m_bloomH)
		return;
	DestroyBloomTargets();

	glGenFramebuffers(2, m_bloomFBO);
	glGenTextures(2, m_bloomColor);
	for (int i = 0; i < 2; ++i)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[i]);
		glBindTexture(GL_TEXTURE_2D, m_bloomColor[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bloomColor[i], 0);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: bloom FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_bloomW = width;
	m_bloomH = height;
}

void OpenGLRenderer::DestroyBloomTargets()
{
	if (m_bloomFBO[0])   glDeleteFramebuffers(2, m_bloomFBO);
	if (m_bloomColor[0]) glDeleteTextures(2, m_bloomColor);
	m_bloomFBO[0] = m_bloomFBO[1] = 0;
	m_bloomColor[0] = m_bloomColor[1] = 0;
	m_bloomW = m_bloomH = 0;
}

// Bright-pass the HDR color, then ping-pong blur. Leaves the result in
// m_bloomColor[0] and returns its id. Assumes m_fsVAO is the active VAO and
// depth test is already disabled. Restores nothing (caller rebinds output).
unsigned int OpenGLRenderer::RenderBloom(int fullW, int fullH)
{
	EnsureBloomTargets(fullW / 2, fullH / 2);
	if (!m_bloomFBO[0]) return 0;

	glViewport(0, 0, m_bloomW, m_bloomH);

	// Bright pass: HDR scene color → m_bloomColor[0].
	glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]);
	glUseProgram(m_bloomBrightProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
	glUniform1i(m_uBrightHDR, 0);
	glUniform1f(m_uBrightThreshold, m_bloomThreshold);
	glUniform1f(m_uBrightKnee, m_bloomKnee);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Ping-pong Gaussian blur. Even pass count ends back in m_bloomColor[0].
	glUseProgram(m_blurProgram);
	glUniform1i(m_uBlurImage, 0);
	glUniform2f(m_uBlurTexel, 1.0f / static_cast<float>(m_bloomW),
	                          1.0f / static_cast<float>(m_bloomH));
	bool horizontal = true;
	constexpr int kBlurPasses = 10; // 5 horizontal + 5 vertical
	for (int i = 0; i < kBlurPasses; ++i)
	{
		const int dst = horizontal ? 1 : 0;
		const int src = horizontal ? 0 : 1;
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[dst]);
		glUniform1i(m_uBlurHorizontal, horizontal ? 1 : 0);
		glBindTexture(GL_TEXTURE_2D, m_bloomColor[src]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		horizontal = !horizontal;
	}
	return m_bloomColor[0];
}

void OpenGLRenderer::EnsureHDRTarget(int width, int height)
{
	if (m_hdrFBO && width == m_hdrW && height == m_hdrH)
		return;
	DestroyHDRTarget();

	glGenFramebuffers(1, &m_hdrFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);

	glGenTextures(1, &m_hdrColor);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_hdrColor, 0);

	glGenRenderbuffers(1, &m_hdrDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_hdrDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_hdrDepth);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: HDR FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_hdrW = width;
	m_hdrH = height;
}

void OpenGLRenderer::DestroyHDRTarget()
{
	if (m_hdrFBO)   { glDeleteFramebuffers(1, &m_hdrFBO);   m_hdrFBO = 0; }
	if (m_hdrColor) { glDeleteTextures(1, &m_hdrColor);     m_hdrColor = 0; }
	if (m_hdrDepth) { glDeleteRenderbuffers(1, &m_hdrDepth);m_hdrDepth = 0; }
	m_hdrW = m_hdrH = 0;
}

void OpenGLRenderer::CreateCubeMesh()
{
	// Unit cube, 24 vertices (position + normal per face)
	static const float verts[] = {
		// +X                          // -X
		 0.5f,-0.5f,-0.5f, 1,0,0,      -0.5f,-0.5f, 0.5f,-1,0,0,
		 0.5f, 0.5f,-0.5f, 1,0,0,      -0.5f, 0.5f, 0.5f,-1,0,0,
		 0.5f, 0.5f, 0.5f, 1,0,0,      -0.5f, 0.5f,-0.5f,-1,0,0,
		 0.5f,-0.5f, 0.5f, 1,0,0,      -0.5f,-0.5f,-0.5f,-1,0,0,
		// +Y                          // -Y
		-0.5f, 0.5f,-0.5f, 0,1,0,      -0.5f,-0.5f, 0.5f, 0,-1,0,
		-0.5f, 0.5f, 0.5f, 0,1,0,      -0.5f,-0.5f,-0.5f, 0,-1,0,
		 0.5f, 0.5f, 0.5f, 0,1,0,       0.5f,-0.5f,-0.5f, 0,-1,0,
		 0.5f, 0.5f,-0.5f, 0,1,0,       0.5f,-0.5f, 0.5f, 0,-1,0,
		// +Z                          // -Z
		-0.5f,-0.5f, 0.5f, 0,0,1,       0.5f,-0.5f,-0.5f, 0,0,-1,
		 0.5f,-0.5f, 0.5f, 0,0,1,      -0.5f,-0.5f,-0.5f, 0,0,-1,
		 0.5f, 0.5f, 0.5f, 0,0,1,      -0.5f, 0.5f,-0.5f, 0,0,-1,
		-0.5f, 0.5f, 0.5f, 0,0,1,       0.5f, 0.5f,-0.5f, 0,0,-1,
	};
	static const uint32_t indices[] = {
		 0, 2, 4,  0, 4, 6,    1, 3, 5,  1, 5, 7,   // +X -X
		 8,10,12,  8,12,14,    9,11,13,  9,13,15,   // +Y -Y
		16,18,20, 16,20,22,   17,19,21, 17,21,23,   // +Z -Z
	};
	m_cubeIndexCount = static_cast<int>(sizeof(indices) / sizeof(indices[0]));

	glGenVertexArrays(1, &m_cubeVAO);
	glGenBuffers(1, &m_cubeVBO);
	glGenBuffers(1, &m_cubeEBO);

	glBindVertexArray(m_cubeVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_cubeEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindVertexArray(0);
}

// ─── Asset mesh upload ────────────────────────────────────────────────────────
const OpenGLRenderer::GpuMesh* OpenGLRenderer::ResolveMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_meshCache.find(assetId); it != m_meshCache.end())
		return &it->second;

	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(assetId);
	if (!asset || asset->vertices.empty() || asset->indices.empty())
		return nullptr;

	// Interleave position + normal + uv (8 floats per vertex). Missing
	// normals/uvs are zero-filled so every mesh fits the unlit layout.
	const size_t vertexCount = asset->vertices.size() / 3;
	std::vector<float> interleaved;
	interleaved.reserve(vertexCount * 8);
	for (size_t v = 0; v < vertexCount; ++v)
	{
		interleaved.insert(interleaved.end(),
			{ asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
		if (v * 3 + 2 < asset->normals.size())
			interleaved.insert(interleaved.end(),
				{ asset->normals[v*3+0], asset->normals[v*3+1], asset->normals[v*3+2] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
		if (v * 2 + 1 < asset->uvs.size())
			interleaved.insert(interleaved.end(), { asset->uvs[v*2+0], asset->uvs[v*2+1] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
	}

	GpuMesh mesh;
	mesh.indexCount  = static_cast<int>(asset->indices.size());
	mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);

	glGenVertexArrays(1, &mesh.vao);
	glGenBuffers(1, &mesh.vbo);
	glGenBuffers(1, &mesh.ebo);

	glBindVertexArray(mesh.vao);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
	             interleaved.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(asset->indices.size() * sizeof(uint32_t)),
	             asset->indices.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
	glBindVertexArray(0);

	// Base color texture via the mesh's material (load on demand by path)
	if (!asset->materialPath.empty())
	{
		const HE::UUID matId = m_contentManager->loadAsset(asset->materialPath);
		if (const MaterialAsset* mat = m_contentManager->getMaterial(matId);
		    mat && !mat->texturePaths.empty())
		{
			const HE::UUID texId = m_contentManager->loadAsset(mat->texturePaths[0]);
			if (const TextureAsset* tex = m_contentManager->getTexture(texId);
			    tex && !tex->data.empty() && tex->channels == 4)
			{
				glGenTextures(1, &mesh.texture);
				glBindTexture(GL_TEXTURE_2D, mesh.texture);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				             static_cast<GLsizei>(tex->width), static_cast<GLsizei>(tex->height),
				             0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
				glGenerateMipmap(GL_TEXTURE_2D);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
	}

	Logger::Log(Logger::LogLevel::Info,
		("OpenGLRenderer: uploaded mesh '" + asset->name + "' ("
		 + std::to_string(vertexCount) + " verts"
		 + (mesh.texture ? ", textured" : "") + ")").c_str());

	return &m_meshCache.emplace(assetId, mesh).first->second;
}

// ─── Material override texture ──────────────────────────────────────────────
bool OpenGLRenderer::ResolveMaterialTexture(const HE::UUID& materialId, unsigned int& outTex)
{
	outTex = 0;
	if (materialId == HE::UUID{} || !m_contentManager)
		return false;

	if (auto it = m_materialTexCache.find(materialId); it != m_materialTexCache.end())
	{
		outTex = it->second;
		return true;
	}

	const MaterialAsset* mat = m_contentManager->getMaterial(materialId);
	if (!mat)
		return false; // not loaded yet — retry next frame without caching

	unsigned int tex = 0;
	if (!mat->texturePaths.empty())
	{
		const HE::UUID texId = m_contentManager->loadAsset(mat->texturePaths[0]);
		if (const TextureAsset* t = m_contentManager->getTexture(texId);
		    t && !t->data.empty() && t->channels == 4)
		{
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			             static_cast<GLsizei>(t->width), static_cast<GLsizei>(t->height),
			             0, GL_RGBA, GL_UNSIGNED_BYTE, t->data.data());
			glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	m_materialTexCache.emplace(materialId, tex);
	outTex = tex;
	return true;
}

bool OpenGLRenderer::ResolveMaterialParams(const HE::UUID& materialId,
	glm::vec3& outBaseColor, float& outMetallic, float& outRoughness)
{
	if (materialId == HE::UUID{} || !m_contentManager)
		return false;
	const MaterialAsset* mat = m_contentManager->getMaterial(materialId);
	if (!mat)
		return false; // not loaded yet — caller keeps defaults
	outBaseColor = glm::vec3(mat->baseColor[0], mat->baseColor[1], mat->baseColor[2]);
	outMetallic  = mat->metallic;
	outRoughness = mat->roughness;
	return true;
}

void OpenGLRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
	// Defer the actual glDelete to DrawScene, where the GL context is current.
	if (materialId != HE::UUID{})
		m_pendingMaterialInvalidations.push_back(materialId);
}

void OpenGLRenderer::Shutdown()
{
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: shutdown");

	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	for (auto& [id, mesh] : m_meshCache)
	{
		if (mesh.vao)     glDeleteVertexArrays(1, &mesh.vao);
		if (mesh.vbo)     glDeleteBuffers(1, &mesh.vbo);
		if (mesh.ebo)     glDeleteBuffers(1, &mesh.ebo);
		if (mesh.texture) glDeleteTextures(1, &mesh.texture);
	}
	m_meshCache.clear();
	for (auto& [id, tex] : m_materialTexCache)
		if (tex) glDeleteTextures(1, &tex);
	m_materialTexCache.clear();
	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyBloomTargets();
	for (auto& r : m_retiredTextures)
		glDeleteTextures(1, &r.texture);
	m_retiredTextures.clear();

	if (m_cubeVAO)      { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
	if (m_cubeVBO)      { glDeleteBuffers(1, &m_cubeVBO);      m_cubeVBO = 0; }
	if (m_cubeEBO)      { glDeleteBuffers(1, &m_cubeEBO);      m_cubeEBO = 0; }
	if (m_unlitProgram) { glDeleteProgram(m_unlitProgram);     m_unlitProgram = 0; }
	if (m_depthProgram) { glDeleteProgram(m_depthProgram);     m_depthProgram = 0; }
	if (m_skyProgram)   { glDeleteProgram(m_skyProgram);       m_skyProgram = 0; }
	if (m_tonemapProgram) { glDeleteProgram(m_tonemapProgram); m_tonemapProgram = 0; }
	if (m_bloomBrightProgram) { glDeleteProgram(m_bloomBrightProgram); m_bloomBrightProgram = 0; }
	if (m_blurProgram)    { glDeleteProgram(m_blurProgram);    m_blurProgram = 0; }
	if (m_fsVAO)          { glDeleteVertexArrays(1, &m_fsVAO);  m_fsVAO = 0; }
	if (m_shadowFBO)      { glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0; }
	if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex);  m_shadowDepthTex = 0; }
	if (m_moonTex)        { glDeleteTextures(1, &m_moonTex);         m_moonTex = 0; }

	// Destroy secondary contexts (secondary windows' SDL_GLContexts are owned by us)
	for (auto& [sdlWin, ctx] : m_secondaryContexts)
		if (ctx) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(ctx));
	m_secondaryContexts.clear();
	m_glContext        = nullptr;
	m_primarySdlWindow = nullptr;
}

// ─── Offscreen viewport target ────────────────────────────────────────────────

void OpenGLRenderer::SetViewportSize(uint32_t width, uint32_t height)
{
	m_viewportReqW = width;
	m_viewportReqH = height;
}

void* OpenGLRenderer::GetViewportTexture()
{
	return reinterpret_cast<void*>(static_cast<intptr_t>(m_viewportColor));
}

bool OpenGLRenderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
	if (!m_viewportFBO || m_viewportW <= 0 || m_viewportH <= 0)
		return false;

	const int w = m_viewportW;
	const int h = m_viewportH;
	rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

	glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// GL returns bottom-row-first; flip to top-row-first for the caller.
	const size_t rowBytes = static_cast<size_t>(w) * 4;
	std::vector<uint8_t> tmp(rowBytes);
	for (int y = 0; y < h / 2; ++y)
	{
		uint8_t* top = rgba.data() + static_cast<size_t>(y) * rowBytes;
		uint8_t* bot = rgba.data() + static_cast<size_t>(h - 1 - y) * rowBytes;
		std::memcpy(tmp.data(), top, rowBytes);
		std::memcpy(top, bot, rowBytes);
		std::memcpy(bot, tmp.data(), rowBytes);
	}

	width  = static_cast<uint32_t>(w);
	height = static_cast<uint32_t>(h);
	return true;
}

void OpenGLRenderer::EnsureViewportTarget()
{
	const int w = static_cast<int>(m_viewportReqW);
	const int h = static_cast<int>(m_viewportReqH);
	if (m_viewportFBO && w == m_viewportW && h == m_viewportH)
		return;

	DestroyViewportTarget();

	glGenFramebuffers(1, &m_viewportFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);

	glGenTextures(1, &m_viewportColor);
	glBindTexture(GL_TEXTURE_2D, m_viewportColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_viewportColor, 0);

	glGenRenderbuffers(1, &m_viewportDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_viewportDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_viewportDepth);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: viewport FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	m_viewportW = w;
	m_viewportH = h;
}

void OpenGLRenderer::AgeRetiredTextures()
{
	for (auto it = m_retiredTextures.begin(); it != m_retiredTextures.end(); )
	{
		if (--it->framesLeft <= 0)
		{
			glDeleteTextures(1, &it->texture);
			it = m_retiredTextures.erase(it);
		}
		else
			++it;
	}
}

void OpenGLRenderer::DestroyViewportTarget()
{
	if (m_viewportFBO)   { glDeleteFramebuffers(1, &m_viewportFBO);   m_viewportFBO = 0; }
	if (m_viewportColor)
	{
		// Deferred — this frame's ImGui draw list still references the id
		m_retiredTextures.push_back({ m_viewportColor, 3 });
		m_viewportColor = 0;
	}
	if (m_viewportDepth) { glDeleteRenderbuffers(1, &m_viewportDepth);m_viewportDepth = 0; }
	m_viewportW = m_viewportH = 0;
}

void OpenGLRenderer::DrawScene(int pw, int ph)
{
	if (!m_world) return;
	if (pw <= 0 || ph <= 0) return;
	glViewport(0, 0, pw, ph);

	// Drop GPU textures for materials edited/re-assigned since last frame so the
	// loop below re-resolves them (deferred here where the GL context is current).
	for (const HE::UUID& id : m_pendingMaterialInvalidations)
		if (auto it = m_materialTexCache.find(id); it != m_materialTexCache.end())
		{
			if (it->second) glDeleteTextures(1, &it->second);
			m_materialTexCache.erase(it);
		}
	m_pendingMaterialInvalidations.clear();

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(pw) / static_cast<float>(ph),
	                    &m_editorCamera);
	// NB: do NOT early-out when there are no (visible) objects — the skybox is the
	// background and must still be drawn, or the viewport falls back to a stale
	// gray clear when the camera looks away from the scene.

	const glm::mat4 viewProj    = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 invViewProj = glm::inverse(viewProj);

	// Direction toward the sun for the sky + image-based ambient — resolved by the
	// extractor (scene directional light, or the day-night cycle when enabled).
	const glm::vec3 sunDir = m_renderWorld.sunDirection;

	// ── Refine bounds with real mesh AABBs (also uploads new meshes) ────────
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// ── Cull → sort → submit ────────────────────────────────────────────────
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	// (no early-out on empty: the geometry pass still draws the skybox background
	// and the post-process still tonemaps it, even with zero visible objects.)

	// Snapshot the active target (window or editor-viewport FBO) so the shadow
	// pass can render into the shadow map and then restore it for the main pass.
	GLint prevFBO = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

	const bool      shadows = m_renderWorld.shadow.enabled && m_shadowFBO != 0;
	const glm::mat4 lightVP = m_renderWorld.shadow.viewProj;

	// ShadowPass → depth map; GeometryPass → HDR scene color; PostProcessPass
	// tonemaps that into the backbuffer/viewport.
	if (m_renderGraph.empty())
	{
		m_renderGraph.addPass(std::make_unique<ShadowPass>());
		m_renderGraph.addPass(std::make_unique<GeometryPass>());
		m_renderGraph.addPass(std::make_unique<PostProcessPass>());
	}

	EnsureHDRTarget(pw, ph);

	m_renderGraph.execute(m_renderWorld, m_sortedIndices,
		[&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
	{
		// ── Shadow pass: depth from the light's POV into the shadow map ──────
		if (io.output.id == kShadowMapTarget)
		{
			if (!shadows) return;
			glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
			glViewport(0, 0, m_shadowSize, m_shadowSize);
			glClear(GL_DEPTH_BUFFER_BIT);
			glUseProgram(m_depthProgram);
			for (const DrawCall& dc : cmds.drawCalls())
			{
				glUniformMatrix4fv(m_uDepthMVP, 1, GL_FALSE, glm::value_ptr(lightVP * dc.transform));
				const GpuMesh* mesh = ResolveMesh(dc.meshAssetId);
				glBindVertexArray(mesh ? mesh->vao : m_cubeVAO);
				glDrawElements(GL_TRIANGLES, mesh ? mesh->indexCount : m_cubeIndexCount,
				               GL_UNSIGNED_INT, nullptr);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
			glViewport(0, 0, pw, ph);
			return;
		}

		// ── PostProcess pass: bloom + tonemap the HDR scene color to output ─
		if (io.inputCount > 0 && io.inputs[0] == kSceneColorTarget)
		{
			glDisable(GL_DEPTH_TEST);
			glBindVertexArray(m_fsVAO);

			// Bright-pass + blur the HDR target into the half-res bloom buffer
			// (skipped when bloom is disabled → strength 0 below).
			const unsigned int bloomTex = m_bloomEnabled ? RenderBloom(pw, ph) : 0u;

			// Composite: tonemap HDR scene color + bloom into the output.
			glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
			glViewport(0, 0, pw, ph);
			glUseProgram(m_tonemapProgram);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_hdrColor);
			glUniform1i(m_uHDRTex, 0);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, bloomTex);
			glUniform1i(m_uBloomTex, 1);
			glUniform1f(m_uExposure, 1.0f);
			glUniform1f(m_uBloomStrength, bloomTex ? m_bloomStrength : 0.0f);
			glActiveTexture(GL_TEXTURE0);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glEnable(GL_DEPTH_TEST);
			return;
		}

		// ── Geometry pass: scene program + per-frame state, into HDR target ─
		// Set here (not before the graph) because the shadow pass switched the
		// active program.
		glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
		glViewport(0, 0, pw, ph);
		glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// ── Skybox: fill the background with the procedural sky (into the HDR
		// target so the sun blooms). Drawn first, no depth write, so the scene
		// draws over it. ──
		if (m_skyProgram)
		{
			glDepthMask(GL_FALSE);
			glDisable(GL_DEPTH_TEST);
			glUseProgram(m_skyProgram);
			glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(invViewProj));
			glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(sunDir));
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_moonTex);
			glUniform1i(m_uSkyMoonTex, 0);
			glUniform1i(m_uSkyHasMoon, m_moonTex ? 1 : 0);
			glUniform1f(m_uSkyTime, GetEnvironment().timeOfDay);
			glUniform1f(m_uSkyCoverage, GetEnvironment().cloudCoverage);
			glBindVertexArray(m_fsVAO);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glEnable(GL_DEPTH_TEST);
			glDepthMask(GL_TRUE);
		}

		glUseProgram(m_unlitProgram);
		glUniform1i(m_uTexture, 0); // base color tint (uColor) is set per draw below
		glUniform3fv(m_uSunDir, 1, glm::value_ptr(sunDir));
		glUniform3fv(m_uAmbient, 1, glm::value_ptr(m_renderWorld.ambient));

		// Lights (clamped to the shader's MAX_LIGHTS)
		{
			constexpr int kMaxLights = 8;
			const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), kMaxLights);
			glm::vec4 pos[kMaxLights], dir[kMaxLights], color[kMaxLights], params[kMaxLights];
			for (int i = 0; i < count; ++i)
			{
				const LightData& l = m_renderWorld.lights[i];
				pos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
				dir[i]    = glm::vec4(l.direction, l.spotAngleCos);
				color[i]  = glm::vec4(l.color,     l.intensity);
				params[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
			}
			glUniform1i(m_uLightCount, count);
			if (count > 0)
			{
				glUniform4fv(m_uLightPos,    count, glm::value_ptr(pos[0]));
				glUniform4fv(m_uLightDir,    count, glm::value_ptr(dir[0]));
				glUniform4fv(m_uLightColor,  count, glm::value_ptr(color[0]));
				glUniform4fv(m_uLightParams, count, glm::value_ptr(params[0]));
			}
			glUniform3fv(m_uCameraPos, 1, glm::value_ptr(m_renderWorld.camera.position));
		}

		// Shadow map bound on texture unit 1.
		glUniform1i(m_uShadowEnabled, shadows ? 1 : 0);
		glUniformMatrix4fv(m_uLightVP, 1, GL_FALSE, glm::value_ptr(lightVP));
		glUniform1i(m_uShadowMap, 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, shadows ? m_shadowDepthTex : 0);
		glActiveTexture(GL_TEXTURE0); // base color binds here in the loop

		for (const DrawCall& dc : cmds.drawCalls())
		{
			const glm::mat4 mvp = viewProj * dc.transform;
			glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(mvp));
			glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(dc.transform));

			// An explicit MaterialComponent override wins over the mesh's own
			// base-color texture; otherwise fall back to the mesh's (or none).
			unsigned int overrideTex = 0;
			const bool   hasOverride = ResolveMaterialTexture(dc.materialAssetId, overrideTex);

			// Resolve the asset; entities without one fall back to the built-in cube.
			const GpuMesh*     mesh = ResolveMesh(dc.meshAssetId);
			const unsigned int tex  = hasOverride ? overrideTex
			                                      : (mesh ? mesh->texture : 0u);

			// PBR scalars from the material override; defaults otherwise. The base
			// tint is the material baseColor if assigned, else white when textured
			// (so the texture is unchanged) or the flat fallback color when not.
			glm::vec3 baseColor(1.0f);
			float     metallic = 0.0f, roughness = 0.5f;
			const bool hasMat = ResolveMaterialParams(dc.materialAssetId, baseColor, metallic, roughness);
			if (!hasMat)
				baseColor = (tex != 0) ? glm::vec3(1.0f) : glm::vec3(0.85f, 0.55f, 0.25f);
			glUniform3fv(m_uColor, 1, glm::value_ptr(baseColor));
			glUniform1f(m_uMetallic,  metallic);
			glUniform1f(m_uRoughness, roughness);

			glBindVertexArray(mesh ? mesh->vao : m_cubeVAO);
			glUniform1i(m_uHasTexture, tex != 0);
			glBindTexture(GL_TEXTURE_2D, tex);
			glDrawElements(GL_TRIANGLES, mesh ? mesh->indexCount : m_cubeIndexCount,
			               GL_UNSIGNED_INT, nullptr);
		}
	});

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	// One-time sanity check — GL errors are silent otherwise and a broken
	// draw path would just render nothing.
	static bool s_checkedFirstFrame = false;
	if (!s_checkedFirstFrame)
	{
		s_checkedFirstFrame = true;
		const GLenum err = glGetError();
		Logger::Log(err == GL_NO_ERROR ? Logger::LogLevel::Info : Logger::LogLevel::Error,
			("OpenGLRenderer: first scene frame drew "
			 + std::to_string(m_renderWorld.objects.size()) + " object(s), glGetError=0x"
			 + std::to_string(err)).c_str());
	}
}

void OpenGLRenderer::Render()
{
	// Make sure we are rendering into the primary window each frame
	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	AgeRetiredTextures();

	const bool offscreen = m_viewportReqW > 0 && m_viewportReqH > 0;

	if (offscreen)
	{
		// Scene → offscreen viewport target (shown by the editor as an image)
		EnsureViewportTarget();
		glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);
		glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		DrawScene(m_viewportW, m_viewportH);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	else if (m_viewportFBO)
		DestroyViewportTarget();

	glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (!offscreen)
	{
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(m_primarySdlWindow, &pw, &ph);
		DrawScene(pw, ph);
	}
	if (m_overlayCallback) m_overlayCallback(nullptr);
}

IRenderer::Capabilities OpenGLRenderer::GetCapabilities() const
{
	return { true, true, true };
}

// ─── Multi-window support ─────────────────────────────────────────────────────

void OpenGLRenderer::AttachWindow(HE::Window* window)
{
	SDL_Window* sdlWin = window->GetNativeWindow();
	if (m_secondaryContexts.count(sdlWin)) return; // already attached

	// Create a new GL context that shares display lists / textures with the primary
	SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	SDL_GLContext sharedCtx = SDL_GL_CreateContext(sdlWin);
	if (!sharedCtx)
		throw std::runtime_error(std::string("OpenGLRenderer: SDL_GL_CreateContext failed for secondary window: ")
								 + SDL_GetError());
	m_secondaryContexts[sdlWin] = static_cast<void*>(sharedCtx);

	// Restore primary context
	SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: secondary window attached");
}

void OpenGLRenderer::DetachWindow(HE::Window* window)
{
	auto it = m_secondaryContexts.find(window->GetNativeWindow());
	if (it == m_secondaryContexts.end()) return;
	SDL_GL_DestroyContext(static_cast<SDL_GLContext>(it->second));
	m_secondaryContexts.erase(it);
	// Restore primary context so the next Render() call works
	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: secondary window detached");
}

void OpenGLRenderer::RenderWindow(HE::Window* window)
{
	auto it = m_secondaryContexts.find(window->GetNativeWindow());
	if (it == m_secondaryContexts.end()) return;

	SDL_GL_MakeCurrent(window->GetNativeWindow(), static_cast<SDL_GLContext>(it->second));
	glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	// TODO: secondary-window draw calls
	// SwapBuffers is called by Application::Run after this method returns
}

void* OpenGLRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	GLuint texId = 0;
	glGenTextures(1, &texId);
	glBindTexture(GL_TEXTURE_2D, texId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8Pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return reinterpret_cast<void*>(static_cast<uintptr_t>(texId));
}

void OpenGLRenderer::DestroyImGuiTexture(void* handle)
{
	if (!handle) return;
	GLuint texId = static_cast<GLuint>(reinterpret_cast<uintptr_t>(handle));
	glDeleteTextures(1, &texId);
}

void OpenGLRenderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
	if (!rgba8Pixels || width <= 0 || height <= 0) return;
	if (m_moonTex) { glDeleteTextures(1, &m_moonTex); m_moonTex = 0; }
	glGenTextures(1, &m_moonTex);
	glBindTexture(GL_TEXTURE_2D, m_moonTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8Pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
}
