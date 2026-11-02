#include "GpuCheck.h"

#include <Backends/OpenGL/OpenGLLandscape.h>

#include <chrono>

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <algorithm>

namespace gpucheck
{
namespace
{
class GlBackend final : public Backend
{
public:
	~GlBackend() override
	{
		if (m_ctx)
		{
			SDL_GL_MakeCurrent(m_window, m_ctx);
			m_landscape.shutdown();
			if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
			if (m_color) glDeleteTextures(1, &m_color);
			if (m_depth) glDeleteTextures(1, &m_depth);
			SDL_GL_DestroyContext(m_ctx);
		}
		if (m_window) SDL_DestroyWindow(m_window);
	}
	const char* name() const override { return "OpenGL"; }

	bool init(std::string& error) override
	{
		if (!SDL_Init(SDL_INIT_VIDEO)) { error = std::string("SDL_Init: ") + SDL_GetError(); return false; }
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		m_window = SDL_CreateWindow("landscape_gpu_check", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
		if (!m_window) { error = std::string("SDL_CreateWindow: ") + SDL_GetError(); return false; }
		m_ctx = SDL_GL_CreateContext(m_window);
		if (!m_ctx) { error = std::string("SDL_GL_CreateContext: ") + SDL_GetError(); return false; }
		SDL_GL_MakeCurrent(m_window, m_ctx);
		if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) { error = "gladLoadGLLoader failed"; return false; }
		m_info = std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION))) + " / " + reinterpret_cast<const char*>(glGetString(GL_RENDERER));
		return m_landscape.init(&error);
	}

	bool render(const RenderIn& in, RenderOut& out) override
	{
		SDL_GL_MakeCurrent(m_window, m_ctx);
		if (in.width != m_w || in.height != m_h) makeTarget(in.width, in.height);
		glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
		glViewport(0, 0, m_w, m_h);
		glClearColor(in.clear.r, in.clear.g, in.clear.b, in.clear.a);
		glClearDepth(1.0);
		glDepthMask(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		const auto t0 = std::chrono::steady_clock::now();
		HE::LandscapeRenderFrame frame;
		in.provider(glm::dmat4(in.viewProj), false, frame);
		const auto t1 = std::chrono::steady_clock::now();
		OpenGLLandscape::Shading sh;
		sh.sunDir = in.sunDir;
		sh.sunColor = in.sunColor;
		sh.ambient = in.ambient;
		const auto upload = [](const TexArray* t) -> GLuint {
			if (!t || t->size <= 0 || t->layers <= 0) return 0;
			GLuint id = 0;
			glGenTextures(1, &id);
			glBindTexture(GL_TEXTURE_2D_ARRAY, id);
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, t->size, t->size, t->layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba.data());
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
			return id;
		};
		const GLuint array = upload(in.albedo), narray = upload(in.normal), marray = upload(in.mask);
		sh.albedoArray = array;
		sh.normalArray = narray;
		sh.maskArray = marray;
		m_landscape.draw(frame, in.viewProj, sh);
		const auto t2 = std::chrono::steady_clock::now();
		for (GLuint id : { array, narray, marray }) if (id) glDeleteTextures(1, &id);
		glFinish();
		const auto t3 = std::chrono::steady_clock::now();
		const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
		out.providerMs = ms(t0, t1);
		out.encodeMs = ms(t1, t2);
		out.gpuMs = ms(t1, t3);
		if (in.timing)
		{
			const auto& st = m_landscape.stats();
			out.uploads = st.uploads; out.patches = st.patches; out.drawCalls = st.drawCalls; out.rejected = st.rejected;
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return true;
		}

		out.width = m_w;
		out.height = m_h;
		std::vector<uint8_t> px(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4);
		std::vector<float> dp(static_cast<size_t>(m_w) * static_cast<size_t>(m_h));
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, m_w, m_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		glReadPixels(0, 0, m_w, m_h, GL_DEPTH_COMPONENT, GL_FLOAT, dp.data());
		out.rgba.resize(px.size());
		out.depth.resize(dp.size());
		for (int y = 0; y < m_h; ++y)   // GL's row 0 is the bottom
		{
			std::copy_n(&px[static_cast<size_t>(m_h - 1 - y) * static_cast<size_t>(m_w) * 4], static_cast<size_t>(m_w) * 4, &out.rgba[static_cast<size_t>(y) * static_cast<size_t>(m_w) * 4]);
			std::copy_n(&dp[static_cast<size_t>(m_h - 1 - y) * static_cast<size_t>(m_w)], static_cast<size_t>(m_w), &out.depth[static_cast<size_t>(y) * static_cast<size_t>(m_w)]);
		}
		const auto& st = m_landscape.stats();
		out.uploads = st.uploads;
		out.patches = st.patches;
		out.drawCalls = st.drawCalls;
		out.rejected = st.rejected;
		if (glGetError() != GL_NO_ERROR) out.error = "GL error";
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return true;
	}

	const std::string& info() const { return m_info; }

private:
	void makeTarget(int w, int h)
	{
		if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
		if (m_color) glDeleteTextures(1, &m_color);
		if (m_depth) glDeleteTextures(1, &m_depth);
		m_w = w;
		m_h = h;
		glGenTextures(1, &m_color);
		glBindTexture(GL_TEXTURE_2D, m_color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenTextures(1, &m_depth);
		glBindTexture(GL_TEXTURE_2D, m_depth);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &m_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depth, 0);
	}

	SDL_Window*   m_window = nullptr;
	SDL_GLContext m_ctx = nullptr;
	GLuint        m_fbo = 0, m_color = 0, m_depth = 0;
	int           m_w = 0, m_h = 0;
	std::string   m_info;
	OpenGLLandscape m_landscape;
};
} // namespace

std::unique_ptr<Backend> makeGlBackend() { return std::make_unique<GlBackend>(); }
} // namespace gpucheck
