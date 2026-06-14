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

uniform vec3      uColor;
uniform bool      uHasTexture;
uniform sampler2D uTexture;

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
	float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
	float closest = texture(uShadowMap, p.xy).r;
	return (p.z - bias > closest) ? 0.35 : 1.0;
}

void main()
{
	vec3 base = uHasTexture ? texture(uTexture, vUV).rgb : uColor;
	vec3 N    = normalize(vNormal);

	if (uLightCount == 0)
	{
		vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
		float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		FragColor  = vec4(base * diff, 1.0);
		return;
	}

	vec3 V      = normalize(uCameraPos - vWorldPos);
	vec3 result = 0.08 * base; // ambient floor

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
		float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;
		result += (base * diff + vec3(spec))
		        * uLightColor[i].rgb * uLightColor[i].w * atten * sh;
	}
	FragColor = vec4(result, 1.0);
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

// Samples the RGBA16F scene color, applies exposure, the ACES filmic curve and
// sRGB gamma, then writes LDR. This is where HDR highlights stop clipping.
static const char* kTonemapFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uHDR;
uniform float     uExposure;
out vec4 FragColor;
vec3 aces(vec3 x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main()
{
	vec3 hdr    = texture(uHDR, vUV).rgb * uExposure;
	vec3 mapped = aces(hdr);
	mapped      = pow(mapped, vec3(1.0 / 2.2));
	FragColor   = vec4(mapped, 1.0);
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
	CreateTonemapPipeline();
	CreateCubeMesh();
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initialized successfully");
}

void OpenGLRenderer::CreateUnlitPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kUnlitVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kUnlitFS);

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
	m_uLightCount  = glGetUniformLocation(m_unlitProgram, "uLightCount");
	m_uLightPos    = glGetUniformLocation(m_unlitProgram, "uLightPos");
	m_uLightDir    = glGetUniformLocation(m_unlitProgram, "uLightDir");
	m_uLightColor  = glGetUniformLocation(m_unlitProgram, "uLightColor");
	m_uLightParams = glGetUniformLocation(m_unlitProgram, "uLightParams");
	m_uCameraPos   = glGetUniformLocation(m_unlitProgram, "uCameraPos");
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
	m_uHDRTex   = glGetUniformLocation(m_tonemapProgram, "uHDR");
	m_uExposure = glGetUniformLocation(m_tonemapProgram, "uExposure");

	// Core profile needs a bound VAO for glDrawArrays even with no attributes.
	glGenVertexArrays(1, &m_fsVAO);
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
	DestroyViewportTarget();
	DestroyHDRTarget();
	for (auto& r : m_retiredTextures)
		glDeleteTextures(1, &r.texture);
	m_retiredTextures.clear();

	if (m_cubeVAO)      { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
	if (m_cubeVBO)      { glDeleteBuffers(1, &m_cubeVBO);      m_cubeVBO = 0; }
	if (m_cubeEBO)      { glDeleteBuffers(1, &m_cubeEBO);      m_cubeEBO = 0; }
	if (m_unlitProgram) { glDeleteProgram(m_unlitProgram);     m_unlitProgram = 0; }
	if (m_depthProgram) { glDeleteProgram(m_depthProgram);     m_depthProgram = 0; }
	if (m_tonemapProgram) { glDeleteProgram(m_tonemapProgram); m_tonemapProgram = 0; }
	if (m_fsVAO)          { glDeleteVertexArrays(1, &m_fsVAO);  m_fsVAO = 0; }
	if (m_shadowFBO)      { glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0; }
	if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex);  m_shadowDepthTex = 0; }

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

	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(pw) / static_cast<float>(ph),
	                    &m_editorCamera);
	if (m_renderWorld.objects.empty()) return;

	const glm::mat4 viewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;

	// ── Refine bounds with real mesh AABBs (also uploads new meshes) ────────
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// ── Cull → sort → submit ────────────────────────────────────────────────
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;

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

		// ── PostProcess pass: tonemap the HDR scene color to the output ─────
		if (io.inputCount > 0 && io.inputs[0] == kSceneColorTarget)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
			glViewport(0, 0, pw, ph);
			glDisable(GL_DEPTH_TEST);
			glUseProgram(m_tonemapProgram);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_hdrColor);
			glUniform1i(m_uHDRTex, 0);
			glUniform1f(m_uExposure, 1.0f);
			glBindVertexArray(m_fsVAO);
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
		glUseProgram(m_unlitProgram);
		glUniform3f(m_uColor, 0.85f, 0.55f, 0.25f);
		glUniform1i(m_uTexture, 0);

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

			// Resolve the asset; entities without one fall back to the built-in cube.
			if (const GpuMesh* mesh = ResolveMesh(dc.meshAssetId))
			{
				glBindVertexArray(mesh->vao);
				glUniform1i(m_uHasTexture, mesh->texture != 0);
				glBindTexture(GL_TEXTURE_2D, mesh->texture);
				glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
			}
			else
			{
				glBindVertexArray(m_cubeVAO);
				glUniform1i(m_uHasTexture, 0);
				glBindTexture(GL_TEXTURE_2D, 0);
				glDrawElements(GL_TRIANGLES, m_cubeIndexCount, GL_UNSIGNED_INT, nullptr);
			}
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
