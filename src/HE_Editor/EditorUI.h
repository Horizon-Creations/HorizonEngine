#pragma once

struct AppContext;

class EditorUI
{
public:
	static void render(AppContext& ctx, float dt);

	// Blocks until a project export running on the worker thread has finished.
	// Must be called on editor shutdown — destroying a joinable std::thread
	// terminates the process.
	static void joinPendingExport();

private:
	static void RenderEditor(AppContext& ctx, float dt);

	};
