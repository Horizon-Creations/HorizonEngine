#pragma once
#include <Renderer/IRenderer.h>

class UIManager
{
    public:
    UIManager(IRenderer* renderer);
    ~UIManager();
    void Initialize();
    void Shutdown();
    bool registerUIElement(/* parameters for UI element */);
    bool unregisterUIElement(/* parameters for UI element */);

    private:
    IRenderer* m_renderer;
};
