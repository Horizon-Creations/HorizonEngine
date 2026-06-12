#pragma once

struct AppContext;

class EditorUI
{
public:
	static void render(AppContext& ctx, float dt);

private:
	static void RenderProjectHub(AppContext& ctx);
	static void RenderEditor(AppContext& ctx, float dt);

	};
