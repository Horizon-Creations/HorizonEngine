#include "Backends/OpenGL/OpenGLLandscape.h"

#include <HorizonRendering/LandscapeShaderSource.h>

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace HE;

struct OpenGLLandscape::Impl
{
	GLuint program = 0;
	GLuint vao = 0, vbo = 0, ibo = 0;
	GLuint tileHeights = 0, slotTable = 0, coarseHeights = 0;
	GLuint weights[3] = { 0, 0, 0 };
	GLuint coarseWeights[3] = { 0, 0, 0 };
	GLuint noArray = 0;   // bound where there is no albedo array: the shader does not read it, but an unloadable texture on a sampler is a driver warning
	std::shared_ptr<const LandscapeRenderStatic> world;   // what the images were made for
	Stats stats;

	// uniform locations
	GLint uViewProj = -1, uCameraRel = -1, uWorld = -1, uHeightInfo = -1, uOrigin = -1, uTile = -1, uHeightRect = -1, uCoarseGrid = -1;
	GLint uPatch = -1, uPatchPos = -1, uMorph = -1, uLayerColor = -1, uSunDir = -1, uSunColor = -1, uAmbient = -1, uTex = -1, uTexMaps = -1, uAlbedoArray = -1, uNormalArray = -1, uMaskArray = -1;
	GLint uTileHeights = -1, uSlotTable = -1, uCoarseHeights = -1, uTileWeights[3] = { -1, -1, -1 }, uCoarseWeights[3] = { -1, -1, -1 };
	bool ok = false;

	void destroyImages()
	{
		const GLuint t[9] = { tileHeights, slotTable, coarseHeights, weights[0], weights[1], weights[2], coarseWeights[0], coarseWeights[1], coarseWeights[2] };
		for (GLuint id : t)
			if (id) glDeleteTextures(1, &id);
		tileHeights = slotTable = coarseHeights = 0;
		weights[0] = weights[1] = weights[2] = 0;
		coarseWeights[0] = coarseWeights[1] = coarseWeights[2] = 0;
		world.reset();
	}
};

namespace
{
constexpr int kUnits = 12;   // tile heights, slot table, coarse heights, three tile weight pages, three coarse weight pages, the albedo, normal and mask arrays (not ours)

GLuint compile(GLenum type, const char* src, std::string& error)
{
	const GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		error = std::string(type == GL_VERTEX_SHADER ? "landscape vertex shader: " : "landscape fragment shader: ") + log;
		glDeleteShader(s);
		return 0;
	}
	return s;
}

GLuint makeTexture(GLenum target)
{
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(target, t);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);   // incomplete without it: the default asks for mips
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

// Puts GL state back as it was found, whatever draw() returns through.
struct StateGuard
{
	GLint program = 0, vao = 0, activeTexture = 0, depthFunc = 0, unpack = 0, depthMask = 0;
	GLboolean depthTest, cull, blend;
	StateGuard()
	{
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
		glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMask);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
		depthTest = glIsEnabled(GL_DEPTH_TEST);
		cull = glIsEnabled(GL_CULL_FACE);
		blend = glIsEnabled(GL_BLEND);
	}
	~StateGuard()
	{
		glUseProgram(static_cast<GLuint>(program));
		glBindVertexArray(static_cast<GLuint>(vao));
		glActiveTexture(static_cast<GLenum>(activeTexture));
		glDepthFunc(static_cast<GLenum>(depthFunc));
		glDepthMask(static_cast<GLboolean>(depthMask));
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
		(depthTest ? glEnable : glDisable)(GL_DEPTH_TEST);
		(cull ? glEnable : glDisable)(GL_CULL_FACE);
		(blend ? glEnable : glDisable)(GL_BLEND);
	}
};
} // namespace

OpenGLLandscape::OpenGLLandscape() : m_impl(std::make_unique<Impl>()) {}
OpenGLLandscape::~OpenGLLandscape() = default;   // the GL objects go with shutdown(): a destructor may run without a context
bool OpenGLLandscape::ready() const { return m_impl->ok; }
const OpenGLLandscape::Stats& OpenGLLandscape::stats() const { return m_impl->stats; }

bool OpenGLLandscape::init(std::string* error)
{
	Impl& m = *m_impl;
	if (m.ok) return true;
	std::string err;
	GLint units = 0;
	glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
	if (units < kFirstUnit + kUnits)
	{
		if (error) *error = "landscape: the context has too few texture units (" + std::to_string(units) + ")";
		return false;
	}
	const GLuint vs = compile(GL_VERTEX_SHADER, glsl::kLandscapeVS, err);
	const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, glsl::kLandscapeFS, err) : 0;
	if (!vs || !fs)
	{
		if (vs) glDeleteShader(vs);
		if (error) *error = err;
		return false;
	}
	m.program = glCreateProgram();
	glAttachShader(m.program, vs);
	glAttachShader(m.program, fs);
	glBindAttribLocation(m.program, 0, "aGrid");     // no layout qualifier in the source
	glLinkProgram(m.program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint linked = 0;
	glGetProgramiv(m.program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char log[2048];
		glGetProgramInfoLog(m.program, sizeof(log), nullptr, log);
		if (error) *error = std::string("landscape link: ") + log;
		glDeleteProgram(m.program);
		m.program = 0;
		return false;
	}
	const auto loc = [&](const char* n) { return glGetUniformLocation(m.program, n); };
	m.uViewProj = loc("uViewProj");   m.uCameraRel = loc("uCameraRel"); m.uWorld = loc("uWorld");         m.uHeightInfo = loc("uHeightInfo");
	m.uOrigin = loc("uOrigin");       m.uTile = loc("uTile");           m.uHeightRect = loc("uHeightRect"); m.uCoarseGrid = loc("uCoarseGrid");
	m.uPatch = loc("uPatch");         m.uPatchPos = loc("uPatchPos");   m.uMorph = loc("uMorph");           m.uLayerColor = loc("uLayerColor");
	m.uSunDir = loc("uSunDir");       m.uSunColor = loc("uSunColor");   m.uAmbient = loc("uAmbient");
	m.uTex = loc("uTex");             m.uTexMaps = loc("uTexMaps");     m.uAlbedoArray = loc("uAlbedoArray");
	m.uNormalArray = loc("uNormalArray"); m.uMaskArray = loc("uMaskArray");
	m.uTileHeights = loc("uTileHeights"); m.uSlotTable = loc("uSlotTable"); m.uCoarseHeights = loc("uCoarseHeights");
	m.uTileWeights[0] = loc("uTileWeights0"); m.uTileWeights[1] = loc("uTileWeights1"); m.uTileWeights[2] = loc("uTileWeights2");
	m.uCoarseWeights[0] = loc("uCoarseWeights0"); m.uCoarseWeights[1] = loc("uCoarseWeights1"); m.uCoarseWeights[2] = loc("uCoarseWeights2");

	{
		const uint8_t white[4] = { 255, 255, 255, 255 };
		glGenTextures(1, &m.noArray);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m.noArray);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	glGenVertexArrays(1, &m.vao);
	glGenBuffers(1, &m.vbo);
	glGenBuffers(1, &m.ibo);
	m.ok = true;
	return true;
}

void OpenGLLandscape::shutdown()
{
	Impl& m = *m_impl;
	m.destroyImages();
	if (m.noArray) glDeleteTextures(1, &m.noArray);
	m.noArray = 0;
	if (m.vao) glDeleteVertexArrays(1, &m.vao);
	if (m.vbo) glDeleteBuffers(1, &m.vbo);
	if (m.ibo) glDeleteBuffers(1, &m.ibo);
	if (m.program) glDeleteProgram(m.program);
	m.vao = m.vbo = m.ibo = m.program = 0;
	m.ok = false;
}

void OpenGLLandscape::draw(const LandscapeRenderFrame& f, const glm::mat4& viewProj, const Shading& shading)
{
	Impl& m = *m_impl;
	m.stats.patches = m.stats.drawCalls = 0;
	if (!m.ok || !f.active || !f.world) return;
	const LandscapeRenderStatic& w = *f.world;
	StateGuard guard;

	// ── the images of a landscape that began (or the first frame) ─────────────
	if (m.world != f.world && (!m.world || m.world->generation != w.generation))
	{
		m.destroyImages();
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		m.tileHeights = makeTexture(GL_TEXTURE_2D_ARRAY);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R16UI, w.heightW, w.heightH, w.slotCapacity, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
		for (int p = 0; p < 3; ++p)
		{
			m.weights[p] = makeTexture(GL_TEXTURE_2D_ARRAY);
			// pages the tiles do not have stay zero (a landscape of fewer than 9 channels); the shader reads all three
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w.weightW, w.weightH, w.slotCapacity, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		}
		m.slotTable = makeTexture(GL_TEXTURE_2D);
		const std::vector<uint16_t> empty(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, w.tableWindow, w.tableWindow, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, empty.data());
		m.coarseHeights = makeTexture(GL_TEXTURE_2D);
		if (w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH))
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, w.coarseW, w.coarseH, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, w.coarseHeights.data());
		else
			++m.stats.rejected;

		for (int p = 0; p < 3; ++p)
		{
			m.coarseWeights[p] = makeTexture(GL_TEXTURE_2D);
			const bool have = static_cast<size_t>(p) < w.coarseWeights.size()
			                  && w.coarseWeights[static_cast<size_t>(p)].size() == static_cast<size_t>(w.coarseWeightW) * static_cast<size_t>(w.coarseWeightH) * 4;
			const std::vector<uint8_t> none(have ? 0 : static_cast<size_t>(std::max(w.coarseWeightW, 1)) * static_cast<size_t>(std::max(w.coarseWeightH, 1)) * 4, 0);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, std::max(w.coarseWeightW, 1), std::max(w.coarseWeightH, 1), 0, GL_RGBA, GL_UNSIGNED_BYTE,
			             have ? w.coarseWeights[static_cast<size_t>(p)].data() : none.data());   // a page the world does not have is zero
		}

		// the grid: (i, j) as two floats; the indices are the shared patch mesh
		const int n = w.patchCells + 1;
		std::vector<float> grid(static_cast<size_t>(n) * static_cast<size_t>(n) * 2);
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				grid[(static_cast<size_t>(j) * n + i) * 2 + 0] = static_cast<float>(i);
				grid[(static_cast<size_t>(j) * n + i) * 2 + 1] = static_cast<float>(j);
			}
		glBindVertexArray(m.vao);
		glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(grid.size() * sizeof(float)), grid.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(w.patchIndices.size() * sizeof(uint16_t)), w.patchIndices.data(), GL_STATIC_DRAW);
		m.world = f.world;
		++m.stats.generations;
	}

	// ── the frame's uploads, then its table changes (a slot's image is in before the table says so) ──
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (const LandscapeRenderUpload& u : f.uploads)
	{
		if (u.slot >= w.slotCapacity || u.heights.size() != static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH)) { ++m.stats.rejected; continue; }
		glBindTexture(GL_TEXTURE_2D_ARRAY, m.tileHeights);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, u.slot, w.heightW, w.heightH, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, u.heights.data());
		for (size_t p = 0; p < u.pages.size() && p < 3; ++p)
		{
			if (u.pages[p].size() != static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4) { ++m.stats.rejected; continue; }
			glBindTexture(GL_TEXTURE_2D_ARRAY, m.weights[p]);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, u.slot, w.weightW, w.weightH, 1, GL_RGBA, GL_UNSIGNED_BYTE, u.pages[p].data());
		}
		++m.stats.uploads;
	}
	glBindTexture(GL_TEXTURE_2D, m.slotTable);
	for (const LandscapeRenderTableUpdate& t : f.tableUpdates)
	{
		if (t.index >= static_cast<uint32_t>(w.tableWindow) * static_cast<uint32_t>(w.tableWindow)) { ++m.stats.rejected; continue; }
		const uint16_t v = t.slot;
		glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(t.index % static_cast<uint32_t>(w.tableWindow)),
		                static_cast<GLint>(t.index / static_cast<uint32_t>(w.tableWindow)), 1, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &v);
		++m.stats.tableUpdates;
	}
	if (f.patches.empty()) return;

	// ── draw ───────────────────────────────────────────────────────────────────
	const GLuint images[kUnits] = { m.tileHeights, m.slotTable, m.coarseHeights, m.weights[0], m.weights[1], m.weights[2], m.coarseWeights[0], m.coarseWeights[1], m.coarseWeights[2], shading.albedoArray ? shading.albedoArray : m.noArray,
	                          shading.normalArray ? shading.normalArray : m.noArray, shading.maskArray ? shading.maskArray : m.noArray };
	const GLenum targets[kUnits] = { GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_2D_ARRAY };
	for (int i = 0; i < kUnits; ++i)
	{
		glActiveTexture(GL_TEXTURE0 + kFirstUnit + i);
		glBindTexture(targets[i], images[i]);
	}
	glUseProgram(m.program);
	glUniform1i(m.uTileHeights, kFirstUnit + 0);
	glUniform1i(m.uSlotTable, kFirstUnit + 1);
	glUniform1i(m.uCoarseHeights, kFirstUnit + 2);
	for (int p = 0; p < 3; ++p) glUniform1i(m.uTileWeights[p], kFirstUnit + 3 + p);
	for (int p = 0; p < 3; ++p) glUniform1i(m.uCoarseWeights[p], kFirstUnit + 6 + p);
	glUniform1i(m.uAlbedoArray, kFirstUnit + 9);
	glUniform1i(m.uNormalArray, kFirstUnit + 10);
	glUniform1i(m.uMaskArray, kFirstUnit + 11);
	// World UVs: a texture tile every `textureMeters` of absolute x/z. The shader works origin-relative, so the origin's part of the
	// coordinate comes in as a fraction computed here in double (a float at 100 km would be 8 mm coarse, a visible shimmer).
	{
		const bool tex = shading.albedoArray != 0 && !w.albedoArrayPath.empty() && w.textureMeters > 0.0f;
		const double k = tex ? 1.0 / static_cast<double>(w.textureMeters) : 1.0;
		const double fx = f.origin.x * k - std::floor(f.origin.x * k), fz = f.origin.z * k - std::floor(f.origin.z * k);
		glUniform4f(m.uTex, static_cast<float>(k), tex ? 1.0f : 0.0f, static_cast<float>(fx), static_cast<float>(fz));
		glUniform4f(m.uTexMaps, tex && shading.normalArray && !w.normalArrayPath.empty() ? 1.0f : 0.0f, tex && shading.maskArray && !w.maskArrayPath.empty() ? 1.0f : 0.0f, 0.0f, 0.0f);
	}

	glUniformMatrix4fv(m.uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
	glUniform4f(m.uCameraRel, f.cameraRel.x, f.cameraRel.y, f.cameraRel.z, f.slabDistance);
	glUniform4f(m.uWorld, w.worldMaxX, w.worldMaxZ, w.tileM, w.heightScaleM);
	glUniform4f(m.uHeightInfo, w.heightMinM, static_cast<float>(w.tilesX), static_cast<float>(w.tilesZ), static_cast<float>(w.tableWindow));
	glUniform3f(m.uOrigin, static_cast<float>(f.origin.x), static_cast<float>(f.origin.y), static_cast<float>(f.origin.z));
	glUniform4i(m.uTile, w.hCells, w.wTexels, w.tileLevels, 0);
	GLint rect[8 * 4] = {};
	for (int l = 0; l < w.heightLevels && l < 8; ++l)
		for (int c = 0; c < 4; ++c) rect[l * 4 + c] = w.heightRect[l][c];
	glUniform4iv(m.uHeightRect, 8, rect);
	GLint grid[16 * 4] = {};
	for (int g = 0; g < w.coarseGridCount && g < 16; ++g)
		for (int c = 0; c < 4; ++c) grid[g * 4 + c] = w.coarseGrid[g][c];
	glUniform4iv(m.uCoarseGrid, 16, grid);
	glUniform4fv(m.uLayerColor, 12, &w.layerColor[0][0]);
	glUniform3fv(m.uSunDir, 1, glm::value_ptr(shading.sunDir));
	glUniform3fv(m.uSunColor, 1, glm::value_ptr(shading.sunColor));
	glUniform3fv(m.uAmbient, 1, glm::value_ptr(shading.ambient));

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glBindVertexArray(m.vao);

	const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
	for (const LandscapeRenderPatch& p : f.patches)
	{
		glUniform4i(m.uPatch, p.level, p.cornerX, p.cornerZ, p.coarse ? 1 : 0);
		glUniform4f(m.uPatchPos, p.x0, p.z0, p.size, static_cast<float>(p.gridIndex));
		glUniform4f(m.uMorph, p.morphStart, p.morphEnd, 0.0f, 0.0f);
		if ((p.quadrants & 15) == 15)
		{
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(total), GL_UNSIGNED_SHORT, nullptr);
			++m.stats.drawCalls;
		}
		else
		{
			for (int q = 0; q < 4; ++q)
			{
				if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
				glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(w.quadrantCount[q]), GL_UNSIGNED_SHORT,
				               reinterpret_cast<const void*>(static_cast<uintptr_t>(w.quadrantFirst[q]) * sizeof(uint16_t)));
				++m.stats.drawCalls;
			}
		}
		++m.stats.patches;
	}
}
