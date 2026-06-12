#include "Backends/OpenGL/OpenGLRenderer.h"
#include <Window/Window.h>
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <Diagnostics/Logger.h>

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
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initialized successfully");
}

void OpenGLRenderer::Shutdown()
{
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: shutdown");
	// Destroy secondary contexts (secondary windows' SDL_GLContexts are owned by us)
	for (auto& [sdlWin, ctx] : m_secondaryContexts)
		if (ctx) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(ctx));
	m_secondaryContexts.clear();
	m_glContext        = nullptr;
	m_primarySdlWindow = nullptr;
}

void OpenGLRenderer::Render()
{
	// Make sure we are rendering into the primary window each frame
	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	// TODO: submit draw calls
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
