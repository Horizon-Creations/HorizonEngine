#pragma once
#include <Renderer/IRenderer.h>
#include "OpenGLShaderManager.h"
#include <unordered_map>

struct SDL_Window;

class OpenGLRenderer : public IRenderer
{
public:
	OpenGLRenderer();
	~OpenGLRenderer();
	void Initialize(HE::Window* window) override;
	void Shutdown()                      override;
	void Render()                        override;
	Capabilities GetCapabilities() const override;

	void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
	void  DestroyImGuiTexture(void* handle) override;

	// Multi-window support
	void AttachWindow(HE::Window* window) override;
	void DetachWindow(HE::Window* window) override;
	void RenderWindow(HE::Window* window) override;

private:
	SDL_Window* m_primarySdlWindow = nullptr;   // needed to restore current context
	void*       m_glContext        = nullptr;   // borrowed — owned by primary HE::Window
	// Secondary windows: SDL_Window* → shared SDL_GLContext (owned here)
	std::unordered_map<SDL_Window*, void*> m_secondaryContexts;
	OpenGLShaderManager m_shaderManager;
};
