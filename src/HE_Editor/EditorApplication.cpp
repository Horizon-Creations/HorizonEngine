#include "EditorApplication.h"
#include <cstring>
#include "AssetThumbnailCache.h" // renderer-owned Content-Browser tiles (freed on shutdown)
#include "EditorUI.h"
#include "HorizonVersion.h"
#include <Diagnostics/Profiler.h>
#include <Platform/Process.h>       // git config for the identity fix
#include <Diagnostics/Log.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h> // HE_DUMP_MATPRECOMPILE witness
#include <glm/gtc/quaternion.hpp>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/WeatherSystem.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/CollisionSystem.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EnvironmentPush.h>      // makeEnvironmentSettings (shared with the game runtime)
#include <HorizonScene/FlyCameraController.h>  // free-fly PIE camera (shared with the game runtime)
#include <HorizonScene/Components/ScriptComponent.h>
#include <ContentManager/Assets.h>
#include <Renderer/RendererFactory.h>
#include <DebugDraw/DebugDraw.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <string>
#include <array>
#include <future>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

// stb_image — declaration only (implementation in stb_image_impl.cpp)
#include "vendor/stb_image.h"

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
// Backend renderer headers — included only in this translation unit
#include <Backends/D3D11/D3D11Renderer.h>
#include <Backends/D3D12/D3D12Renderer.h>
// Modern folder/file picker (IFileOpenDialog)
#include <shobjidl.h>

// ─── Simple free-list SRV descriptor heap allocator for D3D12 ImGui ──────────
// Matches the pattern from the official ImGui DX12 example.
// Must be kept alive for the entire ImGui lifetime.
struct D3D12DescriptorHeapAllocator
{
	ID3D12DescriptorHeap*       Heap            = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu    = {};
	D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu    = {};
	UINT                        Increment       = 0;
	ImVector<int>               FreeIndices;

	void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
	{
		Heap = heap;
		D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
		HeapStartCpu  = heap->GetCPUDescriptorHandleForHeapStart();
		HeapStartGpu  = heap->GetGPUDescriptorHandleForHeapStart();
		Increment     = device->GetDescriptorHandleIncrementSize(desc.Type);
		FreeIndices.reserve((int)desc.NumDescriptors);
		for (int n = (int)desc.NumDescriptors - 1; n >= 0; --n)
			FreeIndices.push_back(n);
	}
	void Destroy() { Heap = nullptr; FreeIndices.clear(); }

	void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
	{
		IM_ASSERT(FreeIndices.Size > 0);
		int idx   = FreeIndices.back(); FreeIndices.pop_back();
		out_cpu->ptr = HeapStartCpu.ptr + (SIZE_T)(idx * Increment);
		out_gpu->ptr = HeapStartGpu.ptr + (UINT64)(idx * Increment);
	}
	void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
	{
		int idx = (int)((cpu.ptr - HeapStartCpu.ptr) / Increment);
		FreeIndices.push_back(idx);
		(void)gpu;
	}
};
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#include <imgui_impl_vulkan.h>
#include <Backends/Vulkan/VulkanRenderer.h>
#endif
#ifdef HE_IMGUI_METAL_ENABLED
#include "ImGuiMetalBridge.h"
#include <Backends/Metal/MetalRenderer.h>

// File-local alias. It used to arrive transitively from the public
// HorizonRendering/ShaderManager.h, which declared it at global scope and so
// leaked `fs` into every consumer of that header.
namespace fs = std::filesystem;

#endif
#endif // HE_IMGUI_ENABLED

std::string getRHIName(HE::RendererBackend backend)
{
	switch (backend)
	{
	case HE::RendererBackend::OpenGL: return "OpenGL";
	case HE::RendererBackend::D3D11: return "D3D11";
	case HE::RendererBackend::D3D12: return "D3D12";
	case HE::RendererBackend::Vulkan: return "Vulkan";
	case HE::RendererBackend::Metal: return "Metal";
	default: return "Unknown";
	}
}

EditorApplication::~EditorApplication() = default;

HE::ApplicationConfig EditorApplication::GetConfig() const
{
	HE::ApplicationConfig cfg;
	cfg.windowprops.title  = "Horizon Engine Editor  " HE_VERSION_FULL;
	cfg.windowprops.width  = 1600;
	cfg.windowprops.height = 900;
	cfg.windowprops.vsync  = true;
	cfg.windowprops.mode   = HE::WindowMode::Windowed;
	cfg.backend = m_globalState->getSelectedRHI();
	// Headless-dump backend override (HE_DUMP_RHI=Metal|OpenGL|Vulkan|D3D11|D3D12):
	// lets a verification screenshot force the user's ACTUAL backend (e.g. Metal on
	// macOS) instead of whatever RHI happens to be persisted in the config.
	if (const char* rhi = std::getenv("HE_DUMP_RHI"); rhi && *rhi)
	{
		const std::string s = rhi;
		if      (s == "Metal")               cfg.backend = HE::RendererBackend::Metal;
		else if (s == "OpenGL" || s == "GL") cfg.backend = HE::RendererBackend::OpenGL;
		else if (s == "Vulkan")              cfg.backend = HE::RendererBackend::Vulkan;
		else if (s == "D3D11")               cfg.backend = HE::RendererBackend::D3D11;
		else if (s == "D3D12")               cfg.backend = HE::RendererBackend::D3D12;
	}
	return cfg;
}

void ApplyHorizonDarkTheme()
{
#ifdef HE_IMGUI_ENABLED
	ImGui::StyleColorsDark();

	ImGuiStyle& s = ImGui::GetStyle();

	s.WindowRounding = 0.0f;
	s.Colors[ImGuiCol_WindowBg].w = 1.0f;
	s.ChildRounding = 0.0f;
	s.FrameRounding = 0.0f;
	s.PopupRounding = 0.0f;
	s.ScrollbarRounding = 0.0f;
	s.GrabRounding = 0.0f;
	s.TabRounding = 0.0f;
	s.WindowBorderSize = 1.0f;
	s.FrameBorderSize = 0.0f;
	s.PopupBorderSize = 1.0f;
	s.FramePadding = ImVec2(6, 4);
	s.ItemSpacing = ImVec2(8, 5);
	s.IndentSpacing = 16.0f;
	s.ScrollbarSize = 12.0f;

	ImVec4* c = s.Colors;

	// Text
	c[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
	c[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);

	// Backgrounds
	c[ImGuiCol_WindowBg]  = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
	c[ImGuiCol_ChildBg]   = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
	c[ImGuiCol_PopupBg]   = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);

	// Borders
	c[ImGuiCol_Border]       = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
	c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	// Frame
	c[ImGuiCol_FrameBg]        = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
	c[ImGuiCol_FrameBgActive]  = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);

	// Title
	c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
	c[ImGuiCol_TitleBgActive]    = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
	c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);

	// Menubar
	c[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

	// Scrollbar
	c[ImGuiCol_ScrollbarBg]          = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
	c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
	c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
	c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);

	// Checkmark + Slider
	c[ImGuiCol_CheckMark]       = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
	c[ImGuiCol_SliderGrab]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
	c[ImGuiCol_SliderGrabActive]= ImVec4(0.40f, 0.40f, 0.40f, 1.00f);

	// Buttons
	c[ImGuiCol_Button]        = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
	c[ImGuiCol_ButtonActive]  = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);

	// Header (Selectable, TreeNode, CollapsingHeader)
	c[ImGuiCol_Header]        = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
	c[ImGuiCol_HeaderActive]  = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);

	// Separator
	c[ImGuiCol_Separator]        = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
	c[ImGuiCol_SeparatorHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
	c[ImGuiCol_SeparatorActive]  = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);

	// Resize grip
	c[ImGuiCol_ResizeGrip]        = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
	c[ImGuiCol_ResizeGripHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
	c[ImGuiCol_ResizeGripActive]  = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);

	// Tabs
	c[ImGuiCol_Tab]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
	c[ImGuiCol_TabHovered]        = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
	c[ImGuiCol_TabActive]         = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
	c[ImGuiCol_TabUnfocused]      = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
	c[ImGuiCol_TabUnfocusedActive]= ImVec4(0.13f, 0.13f, 0.13f, 1.00f);

	// Docking
	c[ImGuiCol_DockingPreview] = ImVec4(0.35f, 0.35f, 0.35f, 0.50f);
	c[ImGuiCol_DockingEmptyBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);

	// Misc
	c[ImGuiCol_PlotLines]            = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
	c[ImGuiCol_PlotHistogram]        = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
	c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
	c[ImGuiCol_TableHeaderBg]        = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
	c[ImGuiCol_TableBorderStrong]    = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
	c[ImGuiCol_TableBorderLight]     = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
	c[ImGuiCol_TextSelectedBg]       = ImVec4(0.25f, 0.25f, 0.25f, 0.60f);
	c[ImGuiCol_NavHighlight]         = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
	c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
#endif // HE_IMGUI_ENABLED
}

std::unique_ptr<IRenderer> EditorApplication::CreateRenderer()
{
	m_backend = GetConfig().backend;
	HE_LOG_INFO(Editor, "%s", "EditorApplication: creating renderer");
	return RendererFactory::Create(m_backend);
}

void EditorApplication::OnInit()
{
	// A received collaboration snapshot replaces the whole world. Selection and
	// undo history still hold entity handles from the world that just went away,
	// so acting on them afterwards would touch freed storage.
	// ── Structural replication ───────────────────────────────────────────────
	// The editor creates and deletes entities from many places (outliner menus,
	// drag & drop, prefab drops, terrain tools). Rather than hooking every one of
	// them — and inevitably missing some — the change is detected by diffing the
	// entity set, which is complete by construction.
	m_collab.onRemoteCreate([this](std::uint32_t parentHandle,
	                               const std::vector<std::uint8_t>& blob) -> std::uint32_t {
		if (!m_editorWorld) return 0;
		const Entity parent = parentHandle
			? static_cast<Entity>(static_cast<entt::id_type>(parentHandle))
			: entt::null;
		SceneSerializer serializer;
		const Entity created = serializer.instantiatePrefab(*m_editorWorld, blob, parent);
		if (created == entt::null) return 0;
		// The fresh handle is recorded by CollabController against the sender's
		// network id — which is what makes handle-stable instantiation
		// unnecessary in the first place.
		m_structureKnown.insert(created);
		return static_cast<std::uint32_t>(entt::to_integral(created));
	});

	m_collab.onRemoteComponents([this](std::uint32_t handle,
	                                   const std::vector<std::uint8_t>& blob) {
		if (!m_editorWorld) return;
		const auto e = static_cast<Entity>(static_cast<entt::id_type>(handle));
		if (!m_editorWorld->registry().valid(e)) return;
		SceneSerializer serializer;
		serializer.applyEntityComponents(*m_editorWorld, e, blob);
		// Mark the transform dirty so the renderer rebuilds its matrix; other
		// components are read fresh each frame.
		if (auto* tc = m_editorWorld->registry().try_get<TransformComponent>(e))
			tc->dirty = true;
	});

	m_collab.onRemoteDestroy([this](std::uint32_t handle) {
		if (!m_editorWorld) return;
		const auto e = static_cast<Entity>(static_cast<entt::id_type>(handle));
		if (!m_editorWorld->registry().valid(e)) return;
		if (m_selectedEntity == e) m_selectedEntity = entt::null;
		m_structureKnown.erase(e);
		m_editorWorld->destroyEntity(e);
	});

	m_collab.onRemoteReparent([this](std::uint32_t handle, std::uint32_t parentHandle) {
		if (!m_editorWorld) return;
		const auto e = static_cast<Entity>(static_cast<entt::id_type>(handle));
		if (!m_editorWorld->registry().valid(e)) return;
		const Entity parent = parentHandle
			? static_cast<Entity>(static_cast<entt::id_type>(parentHandle))
			: m_editorWorld->rootEntity();
		m_editorWorld->reparentEntity(e, parent);
	});

	// ── Per-user undo/redo for the session ───────────────────────────────────
	// Undo becomes an ordinary edit that is republished, not a snapshot
	// restore — see CollabUndo.h.
	m_collabUndo.setHandlers(
		[this](std::uint64_t subject, const float v[9]) {
			if (!m_editorWorld) return;
			auto& reg = m_editorWorld->registry();
			const auto e = static_cast<entt::entity>(static_cast<entt::id_type>(subject));
			if (!reg.valid(e)) return;
			if (auto* tc = reg.try_get<TransformComponent>(e))
			{
				tc->position = glm::vec3(v[0], v[1], v[2]);
				tc->rotation = glm::vec3(v[3], v[4], v[5]);
				tc->scale    = glm::vec3(v[6], v[7], v[8]);
				tc->dirty    = true;
				// Publishing it is what makes the undo visible to everyone —
				// it travels as a normal change, not as a rewind.
				const float p[3] = { v[0], v[1], v[2] };
				const float r[3] = { v[3], v[4], v[5] };
				const float s[3] = { v[6], v[7], v[8] };
				m_collab.publishTransform(subject, p, r, s, 0);
			}
		},
		[this](const std::string& path, const std::vector<std::uint8_t>& bytes) {
			applyAssetBytes(path, bytes);
			m_collab.publishAsset(path, contentManager().resolveSavePath(path));
		},
		[this](std::uint64_t subject) { return m_collab.ownsLock(subject); });

	// ── Authored-asset sync ──────────────────────────────────────────────────
	// Every asset type funnels through ContentManager::saveAsset, so one hook
	// covers HorizonCode graphs, materials, UI widgets, particle and animator
	// graphs and scenes alike.
	contentManager().setOnAssetSaved(
		[this](const std::string& relPath, const std::string& fullPath) {
			m_collab.publishAsset(relPath, fullPath);
		});

	m_collab.onRemoteAsset([this](const std::string& relPath,
	                              const std::vector<std::uint8_t>& bytes) {
		applyAssetBytes(relPath, bytes);
		Logger::Log(Logger::LogLevel::Info,
		            ("Collab: applied remote change to " + relPath).c_str());
	});

	// A remote peer moved something. Applying it here (rather than inside
	// CollabController) keeps the network layer out of the ECS.
	m_collab.onRemoteTransform([this](std::uint64_t subject, const float pos[3],
	                                  const float rotEuler[3], const float scale[3]) {
		if (!m_editorWorld) return;
		auto& reg = m_editorWorld->registry();
		const auto e = static_cast<entt::entity>(static_cast<entt::id_type>(subject));
		if (!reg.valid(e)) return;   // not in our world — ignore rather than guess

		if (auto* tc = reg.try_get<TransformComponent>(e))
		{
			tc->position = glm::vec3(pos[0], pos[1], pos[2]);
			tc->rotation = glm::vec3(rotEuler[0], rotEuler[1], rotEuler[2]);
			tc->scale    = glm::vec3(scale[0], scale[1], scale[2]);
			tc->dirty    = true;   // or the renderer keeps the stale matrix
			// Deliberately NOT pushed through the undo system: this is someone
			// else's edit, and putting it on our stack would let us "undo" their
			// work — see the undo section in docs/networking-layer-design.md.
		}
	});

	m_collab.onWorldReplaced([this] {
		// Both peers just deserialized the same bytes, so handle == network id
		// here — seed the map from the world before anything else runs.
		std::vector<std::uint32_t> handles;
		m_structureKnown.clear();
		if (m_editorWorld)
		{
			m_editorWorld->registry().view<entt::entity>().each([&](auto e) {
				handles.push_back(static_cast<std::uint32_t>(entt::to_integral(e)));
				m_structureKnown.insert(e);
			});
		}
		m_collab.seedNetIds(handles);

		m_selectedEntity = entt::null;
		// Every snapshot in the undo stack belongs to the replaced world, so
		// undoing into one would restore a scene the session no longer shares.
		m_undo.clearHistory();
		// The received scene has never been saved locally: leave the dirty flag
		// set by making the saved revision unreachable.
		m_savedRevision = static_cast<std::uint64_t>(-1);
	});

	// ── Headless frame-dump hook (validation / CI screenshots) ──────────────
	if (const char* p = std::getenv("HE_DUMP_PATH"); p && *p)
	{
		m_dumpPath = p;
		if (const char* q = std::getenv("HE_DUMP_QUIT"); q && *q)
			m_dumpQuit = (std::atoi(q) != 0);
		HE_LOG_INFO(Editor, "%s",
			("EditorApplication: frame dump armed → " + m_dumpPath).c_str());
	}
#ifdef HE_IMGUI_ENABLED
	HE_LOG_INFO(Editor, "%s", "EditorApplication::OnInit — initialising ImGui");
	m_vsync = GetConfig().windowprops.vsync;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ApplyHorizonDarkTheme();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	// ImGui windows that end up in their own OS window (a dialog that does not fit
	// inside the editor, a panel dragged out of it) are top-level and unparented
	// by default, so the window manager orders them independently of the editor:
	// alt-tabbing away and back, or clicking the editor, buries them behind the
	// docked layout while they stay open — and an open modal keeps eating input.
	// Declaring the main viewport as their parent makes the OS keep them in front
	// of the editor window. EditorWidgets::raiseDetachedModals() covers macOS,
	// where the SDL3 backend skips parenting (multi-monitor quirk upstream).
	io.ConfigViewportsNoDefaultParent = false;

	// ── Load editor fonts ─────────────────────────────────────────────────────
	// Font file is deployed alongside the executable via the CMake post-build step.
	{
		const char* basePath = SDL_GetBasePath();
		std::string fontPath = std::string(basePath ? basePath : "") + "Fonts/Roboto_Condensed-Bold.ttf";

		ImFontConfig cfg;
		cfg.OversampleH = 2;
		cfg.OversampleV = 2;
		cfg.PixelSnapH  = false;

		// Font sizes are in logical points. Since ImGui 1.92 the (1.92+) renderer
		// backend rasterises glyphs at the viewport's framebuffer scale
		// automatically, so HiDPI crispness comes from the high-pixel-density
		// drawable — NOT from pre-scaling the size here (the old DisplayFramebuffer
		// Scale/FontGlobalScale trick no longer maps to the new font system).
		const float sizeBody       = 13.0f;
		const float sizeSubheading = 16.0f;
		const float sizeHeading    = 19.0f;

		if (std::filesystem::exists(fontPath))
		{
			m_fontBody       = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), sizeBody,       &cfg);
			m_fontSubheading = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), sizeSubheading, &cfg);
			m_fontHeading    = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), sizeHeading,    &cfg);
			HE_LOG_INFO(Editor, "%s", ("EditorApplication: fonts loaded from " + fontPath).c_str());
		}
		else
		{
			HE_LOG_WARN(Editor, "%s", ("EditorApplication: font not found at " + fontPath + " — using ImGui default").c_str());
			m_fontBody       = io.Fonts->AddFontDefault();
			m_fontSubheading = io.Fonts->AddFontDefault();
			m_fontHeading    = io.Fonts->AddFontDefault();
		}

		// Monospace font for the script code editor — ImGui's built-in ProggyClean is
		// monospace, so the columns/line-numbers align without shipping a new TTF.
		m_fontMono = io.Fonts->AddFontDefault();

		// No io.Fonts->Build() here — since ImGui 1.92 the renderer backends
		// own the font atlas (ImGuiBackendFlags_RendererHasTextures) and build
		// it lazily; calling Build() before backend init raises errors.

		// Body font is the ImGui default — push it as the global default
		io.FontDefault = m_fontBody;
	}

	// ── Engine-wide default content (EditorDeps/EngineContent) ────────────────
	// Deployed next to the editor executable exactly like Fonts/Images (see the
	// copy_directory of EditorDeps in HE_Editor's CMakeLists.txt) — NOT part of
	// any project's Content/ folder, so it's resolved once here, independent of
	// which project gets loaded afterwards. Content Browser shows it as the
	// "Engine" root, sibling to "Content" (see EditorUI.cpp).
	{
		const char* basePath = SDL_GetBasePath();
		std::string engineContentPath = std::string(basePath ? basePath : "") + "EngineContent";
		contentManager().setEngineContentRoot(engineContentPath);
		m_globalState->refreshEngineFolder(engineContentPath);
	}

	switch (m_backend)
	{
	// ── OpenGL ────────────────────────────────────────────────────────────────
	case HE::RendererBackend::OpenGL:
		ImGui_ImplSDL3_InitForOpenGL(
			window()->GetNativeWindow(),
			window()->GetGLContext());
#ifdef __APPLE__
		// macOS is capped at OpenGL 4.1 / GLSL 410
		ImGui_ImplOpenGL3_Init("#version 410");
#else
		ImGui_ImplOpenGL3_Init("#version 460");
#endif
		m_imguiReady = true;
		HE_LOG_INFO(Editor, "%s", "Initialized ImGui OpenGL backend");
		break;

#ifdef HE_IMGUI_METAL_ENABLED
	// ── Metal ─────────────────────────────────────────────────────────────────
	case HE::RendererBackend::Metal:
	{
		auto* mtl = static_cast<MetalRenderer*>(renderer());
		if (mtl && mtl->GetDevice())
		{
			ImGui_ImplSDL3_InitForMetal(window()->GetNativeWindow());
			if (ImGuiMetalBridge::Init(mtl->GetDevice()))
			{
				m_imguiReady = true;
				HE_LOG_INFO(Editor, "%s", "Initialized ImGui Metal backend");
			}
		}
		break;
	}
#endif

#ifdef _WIN32
	// ── D3D11 ─────────────────────────────────────────────────────────────────
	case HE::RendererBackend::D3D11:
	{
		HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
			SDL_GetWindowProperties(window()->GetNativeWindow()),
			SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		auto* dx11 = static_cast<D3D11Renderer*>(renderer());
		if (hwnd && dx11)
		{
			ImGui_ImplSDL3_InitForOther(window()->GetNativeWindow());
			ImGui_ImplDX11_Init(
				static_cast<ID3D11Device*>(dx11->GetDevice()),
				static_cast<ID3D11DeviceContext*>(dx11->GetContext()));
			m_imguiReady = true;
			HE_LOG_INFO(Editor, "%s", "Initialized ImGui D3D11 backend");
		}
		break;
	}

	// ── D3D12 ─────────────────────────────────────────────────────────────────
	case HE::RendererBackend::D3D12:
	{
		HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
			SDL_GetWindowProperties(window()->GetNativeWindow()),
			SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		auto* dx12 = static_cast<D3D12Renderer*>(renderer());
		if (hwnd && dx12)
		{
			auto* device   = static_cast<ID3D12Device*>(dx12->GetDevice());
			auto* cmdQueue = static_cast<ID3D12CommandQueue*>(dx12->GetCommandQueue());

			// 64-slot shader-visible SRV heap — enough for font atlas + textures
			// used by extra viewport windows (multi-viewport).
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.NumDescriptors = 64;
			srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			ID3D12DescriptorHeap* srvHeap = nullptr;
			if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap))))
				break;

			m_d3d12SrvHeap = srvHeap;

			// Build the free-list allocator so ImGui can allocate/free individual SRV slots.
			auto* alloc = new D3D12DescriptorHeapAllocator();
			alloc->Create(device, srvHeap);
			m_d3d12SrvAllocator = alloc;

			ImGui_ImplSDL3_InitForOther(window()->GetNativeWindow());

			ImGui_ImplDX12_InitInfo dx12Info{};
			dx12Info.Device            = device;
			dx12Info.CommandQueue      = cmdQueue;
			dx12Info.NumFramesInFlight = 2;
			dx12Info.RTVFormat         = DXGI_FORMAT_R8G8B8A8_UNORM;
			dx12Info.DSVFormat         = DXGI_FORMAT_UNKNOWN;
			dx12Info.SrvDescriptorHeap = srvHeap;
			dx12Info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
				D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
			{
				static_cast<D3D12DescriptorHeapAllocator*>(info->UserData)->Alloc(out_cpu, out_gpu);
			};
			dx12Info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info,
				D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
			{
				static_cast<D3D12DescriptorHeapAllocator*>(info->UserData)->Free(cpu, gpu);
			};
			dx12Info.UserData = alloc;

			ImGui_ImplDX12_Init(&dx12Info);
			m_imguiReady = true;
			HE_LOG_INFO(Editor, "%s", "Initialized ImGui D3D12 backend");
		}
		break;
	}
#endif // _WIN32

#ifdef HE_IMGUI_VULKAN_ENABLED
	// ── Vulkan ────────────────────────────────────────────────────────────────
	case HE::RendererBackend::Vulkan:
	{
		auto* vk = static_cast<VulkanRenderer*>(renderer());
		if (vk)
		{
			ImGui_ImplVulkan_InitInfo vkInfo{};
			vkInfo.ApiVersion        = VK_API_VERSION_1_2;
			vkInfo.Instance          = static_cast<VkInstance>(vk->GetInstance());
			vkInfo.PhysicalDevice    = static_cast<VkPhysicalDevice>(vk->GetPhysicalDevice());
			vkInfo.Device            = static_cast<VkDevice>(vk->GetDevice());
			vkInfo.QueueFamily       = vk->GetQueueFamily();
			vkInfo.Queue             = static_cast<VkQueue>(vk->GetQueue());
			// ImGui creates one fixed-size internal pool of this many SAMPLED_IMAGE
			// descriptors; each ImGui_ImplVulkan_AddTexture() consumes one and they
			// are not freed (DestroyImGuiTexture is a no-op). Sized to cover the
			// font atlas + viewport + logo + content-browser icons with headroom
			// (mirrors the 64-slot D3D12 ImGui SRV heap). Was 8 — too small once the
			// editor registers ~15 icon/logo textures, which silently exhausted the
			// pool and left those textures blank.
			vkInfo.DescriptorPoolSize = 64;
			vkInfo.MinImageCount     = 2;
			vkInfo.ImageCount        = vk->GetImageCount();
			const uint64_t rpRaw = vk->GetRenderPass();
			VkRenderPass rp{};
			static_assert(sizeof(rp) == sizeof(rpRaw), "VkRenderPass size mismatch");
			std::memcpy(&rp, &rpRaw, sizeof(rp));
			vkInfo.PipelineInfoMain.RenderPass = rp;
			ImGui_ImplSDL3_InitForVulkan(window()->GetNativeWindow());
			ImGui_ImplVulkan_Init(&vkInfo);
			m_imguiReady = true;
			HE_LOG_INFO(Editor, "%s", "Initialized ImGui Vulkan backend");
		}
		break;
	}
#endif

	default:
		break;
	}

	// Initialize default tabs (Viewport is always present and not closable)
	if (m_tabs.empty())
		m_tabs.push_back({ "Viewport", "", false, true });

	// Register the per-frame overlay injection callback with the active renderer
	if (m_imguiReady)
	{
		HE_LOG_INFO(Editor, "%s", "EditorApplication::OnInit — ImGui backend ready");
		renderer()->SetOverlayCallback([this](void* nativeContext)
		{
			ImDrawData* drawData = ImGui::GetDrawData();
			if (!drawData) return;

			switch (m_backend)
			{
			case HE::RendererBackend::OpenGL:
				if (drawData->TotalVtxCount > 0)
					ImGui_ImplOpenGL3_RenderDrawData(drawData);
				break;
#ifdef _WIN32
			case HE::RendererBackend::D3D11:
				if (drawData->TotalVtxCount > 0)
					ImGui_ImplDX11_RenderDrawData(drawData);
				break;
			case HE::RendererBackend::D3D12:
			{
				if (!nativeContext) break;
				auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(nativeContext);
				auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(m_d3d12SrvHeap);
				if (!srvHeap) break;

				// D3D12 requires the SRV heap to be explicitly bound before ImGui draw calls.
				ID3D12DescriptorHeap* heaps[] = { srvHeap };
				cmdList->SetDescriptorHeaps(1, heaps);

				// ImGui needs a viewport and scissor — set them to the full display size.
				D3D12_VIEWPORT vp{};
				vp.Width    = drawData->DisplaySize.x;
				vp.Height   = drawData->DisplaySize.y;
				vp.MaxDepth = 1.0f;
				cmdList->RSSetViewports(1, &vp);

				D3D12_RECT scissor{};
				scissor.right  = static_cast<LONG>(drawData->DisplaySize.x);
				scissor.bottom = static_cast<LONG>(drawData->DisplaySize.y);
				cmdList->RSSetScissorRects(1, &scissor);

				ImGui_ImplDX12_RenderDrawData(drawData, cmdList);
				break;
			}
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
			case HE::RendererBackend::Vulkan:
				if (nativeContext && drawData->TotalVtxCount > 0)
					ImGui_ImplVulkan_RenderDrawData(drawData,
						static_cast<VkCommandBuffer>(nativeContext));
				break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
			case HE::RendererBackend::Metal:
			{
				if (!nativeContext || drawData->TotalVtxCount <= 0) break;
				auto* mtlCtx = static_cast<MetalOverlayContext*>(nativeContext);
				ImGuiMetalBridge::RenderDrawData(drawData,
					mtlCtx->commandBuffer, mtlCtx->renderEncoder);
				break;
			}
#endif
			default:
				break;
			}
		});

		// ── ImGui texture registrar (D3D12 / Vulkan) ────────────────────────────
		// The renderer DLL does not link ImGui, so for these backends the editor
		// must turn an uploaded GPU texture into an ImGui ImTextureID. Installed
		// here (inside the m_imguiReady block, before the logo/icons are loaded
		// below) so the renderer's CreateImGuiTexture can call back into ImGui's
		// descriptor heap.
#ifdef _WIN32
		if (m_backend == HE::RendererBackend::D3D12)
		{
			renderer()->SetImGuiTextureRegistrar(
				[this](void* res, void* /*unused*/) -> void*
			{
				auto* dx12   = static_cast<D3D12Renderer*>(renderer());
				auto* device = dx12 ? static_cast<ID3D12Device*>(dx12->GetDevice()) : nullptr;
				auto* alloc  = static_cast<D3D12DescriptorHeapAllocator*>(m_d3d12SrvAllocator);
				if (!device || !alloc || !res) return nullptr;

				// Allocate an ImGui-heap SRV slot and create the texture's SRV.
				D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
				D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
				alloc->Alloc(&cpu, &gpu);

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
				srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Texture2D.MipLevels     = 1;
				device->CreateShaderResourceView(
					static_cast<ID3D12Resource*>(res), &srvDesc, cpu);

				// The GPU descriptor handle is the ImGui texture ID.
				return reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.ptr));
			});
		}
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
		if (m_backend == HE::RendererBackend::Vulkan)
		{
			renderer()->SetImGuiTextureRegistrar(
				[](void* view, void* sampler) -> void*
			{
				return reinterpret_cast<void*>(ImGui_ImplVulkan_AddTexture(
					reinterpret_cast<VkSampler>(sampler),
					reinterpret_cast<VkImageView>(view),
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
			});
		}
#endif
	}
#endif // HE_IMGUI_ENABLED
	m_backend      = m_globalState->getSelectedRHI();
	m_backend_name = getRHIName(m_backend);

	GlobalState& globalstate = GlobalState::getInstance();
	m_editorConfig.ContentBrowserRefreshRate   = globalstate.getCustomConfigInt("ContentBrowserRefreshRate",   m_editorConfig.ContentBrowserRefreshRate);
	m_editorConfig.KeepCPUAssets               = globalstate.getCustomConfigBool("KeepCPUAssets",               m_editorConfig.KeepCPUAssets);
	// The persisted key was misspelled "KeepCPUAssetsInfoAcknoleged" until the
	// 2026-07 rework. Read the old spelling FIRST and hand it in as the default for
	// the corrected key, so a user who already dismissed the info box keeps that
	// state instead of silently getting it back. The save side only ever writes the
	// corrected key, so the stale entry stops mattering after one clean shutdown.
	// Do not delete this fallback — every config.json written before that rework
	// still carries only the old spelling.
	const bool legacyKeepCPUAssetsAck =
		globalstate.getCustomConfigBool("KeepCPUAssetsInfoAcknoleged", m_editorConfig.KeepCPUAssetsInfoAcknowledged);
	m_editorConfig.KeepCPUAssetsInfoAcknowledged =
		globalstate.getCustomConfigBool("KeepCPUAssetsInfoAcknowledged", legacyKeepCPUAssetsAck);
	m_editorConfig.CbTreeWidth                 = globalstate.getCustomConfigFloat("CbTreeWidth", m_editorConfig.CbTreeWidth);
	m_editorConfig.UiFontScale                 = globalstate.getCustomConfigFloat("UiFontScale",       m_editorConfig.UiFontScale);
	m_editorConfig.EditorCameraSpeed           = globalstate.getCustomConfigFloat("EditorCameraSpeed", m_editorConfig.EditorCameraSpeed);
	m_editorConfig.MaxFps                      = globalstate.getCustomConfigFloat("MaxFps",            m_editorConfig.MaxFps);
	m_editorConfig.BloomEnabled                = globalstate.getCustomConfigBool("BloomEnabled",        m_editorConfig.BloomEnabled);
	m_editorConfig.BloomThreshold              = globalstate.getCustomConfigFloat("BloomThreshold",     m_editorConfig.BloomThreshold);
	m_editorConfig.BloomIntensity              = globalstate.getCustomConfigFloat("BloomIntensity",     m_editorConfig.BloomIntensity);
	m_editorConfig.SSAOEnabled                 = globalstate.getCustomConfigBool("SSAOEnabled",         m_editorConfig.SSAOEnabled);
	m_editorConfig.SSAORadius                  = globalstate.getCustomConfigFloat("SSAORadius",         m_editorConfig.SSAORadius);
	m_editorConfig.SSAOIntensity               = globalstate.getCustomConfigFloat("SSAOIntensity",      m_editorConfig.SSAOIntensity);
	m_editorConfig.SSAOMethod                  = globalstate.getCustomConfigInt("SSAOMethod",           m_editorConfig.SSAOMethod);
	m_editorConfig.GpuParticles                = globalstate.getCustomConfigBool("GpuParticles",        m_editorConfig.GpuParticles);
	m_editorConfig.GlobalIlluminationEnabled   = globalstate.getCustomConfigBool("GlobalIlluminationEnabled", m_editorConfig.GlobalIlluminationEnabled);
	m_editorConfig.GIIndirectIntensity         = globalstate.getCustomConfigFloat("GIIndirectIntensity",      m_editorConfig.GIIndirectIntensity);
	m_editorConfig.GILightRadius               = globalstate.getCustomConfigFloat("GILightRadius",            m_editorConfig.GILightRadius);
	m_editorConfig.GIReflectionsEnabled        = globalstate.getCustomConfigBool("GIReflectionsEnabled",      m_editorConfig.GIReflectionsEnabled);
	m_editorConfig.GIReflIntensity             = globalstate.getCustomConfigFloat("GIReflIntensity",          m_editorConfig.GIReflIntensity);
	m_editorConfig.GIReflMaxRoughness          = globalstate.getCustomConfigFloat("GIReflMaxRoughness",       m_editorConfig.GIReflMaxRoughness);
	m_editorConfig.GIReflQuality               = globalstate.getCustomConfigInt("GIReflQuality",              m_editorConfig.GIReflQuality);
	m_editorConfig.RenderPath                  = globalstate.getCustomConfigInt("RenderPath",           m_editorConfig.RenderPath);
	m_editorConfig.SSREnabled                  = globalstate.getCustomConfigBool("SSREnabled",          m_editorConfig.SSREnabled);
	m_editorConfig.SSRIntensity                = globalstate.getCustomConfigFloat("SSRIntensity",       m_editorConfig.SSRIntensity);
	m_editorConfig.SSRQuality                  = globalstate.getCustomConfigInt("SSRQuality",           m_editorConfig.SSRQuality);
	m_editorConfig.SSRMaxRoughness             = globalstate.getCustomConfigFloat("SSRMaxRoughness",    m_editorConfig.SSRMaxRoughness);
	m_editorConfig.QuickSettingsFavorites      = globalstate.getCustomConfigString("QuickSettingsFavorites", m_editorConfig.QuickSettingsFavorites);
	m_editorCamera.setFlySpeed(m_editorConfig.EditorCameraSpeed);
	// Restore the last editor camera view (saved on exit). Skipped on first run (no
	// saved view yet) so the default 3/4 framing of the world origin is used instead.
	if (globalstate.getCustomConfigBool("EditorCamValid", false))
	{
		const glm::vec3 camPos(
			globalstate.getCustomConfigFloat("EditorCamPosX", m_editorCamera.position().x),
			globalstate.getCustomConfigFloat("EditorCamPosY", m_editorCamera.position().y),
			globalstate.getCustomConfigFloat("EditorCamPosZ", m_editorCamera.position().z));
		m_editorCamera.restoreView(
			camPos,
			globalstate.getCustomConfigFloat("EditorCamYaw",   m_editorCamera.yaw()),
			globalstate.getCustomConfigFloat("EditorCamPitch", m_editorCamera.pitch()),
			globalstate.getCustomConfigFloat("EditorCamPivot", m_editorCamera.pivotDistance()));
	}
	setMaxFps(m_editorConfig.MaxFps);   // VSync-off frame cap (0 = unlimited)

#ifdef HE_IMGUI_ENABLED
	// ── Load HC_Logo ──────────────────────────────────────────────────────────
	{
		const char* basePath = SDL_GetBasePath();
		std::string logoPath = std::string(basePath ? basePath : "") + "Images/HC_Logo.png";

		int w = 0, h = 0, ch = 0;
		unsigned char* pixels = stbi_load(logoPath.c_str(), &w, &h, &ch, 4);
		if (pixels)
		{
#ifndef __APPLE__
			// ── Window icon via SDL ───────────────────────────────────────────
			// On macOS the Dock/app icon comes from the .app bundle's icns
			// (CFBundleIconFile, set by scripts/package_macos.sh). Overriding it
			// at runtime would replace the polished squircle with the bare logo,
			// so we skip it here and let the bundle icon stand.
			SDL_Surface* iconSurface = SDL_CreateSurfaceFrom(
				w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
			if (iconSurface && window() && window()->GetNativeWindow())
			{
				SDL_SetWindowIcon(window()->GetNativeWindow(), iconSurface);
				SDL_DestroySurface(iconSurface);
			}
#endif

			// ── ImGui texture (via abstract renderer API) ────────────────────
			if (void* handle = renderer()->CreateImGuiTexture(pixels, w, h))
			{
				m_logoTexture = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle));
				m_logoW = w;
				m_logoH = h;
			}
			stbi_image_free(pixels);
			HE_LOG_INFO(Editor, "%s", ("EditorApplication: logo loaded from " + logoPath).c_str());
		}
		else
		{
			HE_LOG_WARN(Editor, "%s", ("EditorApplication: logo not found at " + logoPath).c_str());
		}
	}

	// ── Load Content Browser Icons ────────────────────────────────────────────
	{
		const char* basePath = SDL_GetBasePath();
		std::string imgDir   = std::string(basePath ? basePath : "") + "Images/";

		struct IconEntry { const char* file; ImTextureID* target; };
		IconEntry icons[] = {
			{ "Folder.png",   &m_iconFolder   },
			{ "Material.png", &m_iconMaterial },
			{ "Model2D.png",  &m_iconModel2d  },
			{ "Model3D.png",  &m_iconModel3d  },
			{ "Script.png",   &m_iconScript   },
			{ "Sound.png",    &m_iconSound    },
			{ "Texture.png",  &m_iconTexture  },
			{ "Scene.png",    &m_iconScene    },
			{ "Play.tga",     &m_iconPlay     },
			{ "Stop.tga",     &m_iconStop     },
			{ "undo.png",     &m_iconUndo     },
			{ "redo.png",     &m_iconRedo     },
			// Per-asset-type glyphs (scripts/make_asset_icons.py). File names are
			// the HE::AssetType spelling so the mapping stays obvious.
			{ "MaterialFunction.png",     &m_iconMaterialFunction     },
			{ "Shader.png",               &m_iconShader               },
			{ "Prefab.png",               &m_iconPrefab               },
			{ "AnimationClip.png",        &m_iconAnimationClip        },
			{ "PropertyAnimClip.png",     &m_iconPropertyAnimClip     },
			{ "Widget.png",               &m_iconWidget               },
			{ "HorizonCodeClass.png",     &m_iconHorizonCodeClass     },
			{ "InputAction.png",          &m_iconInputAction          },
			{ "InputMappingContext.png",  &m_iconInputMappingContext  },
			{ "ParticleSystem.png",       &m_iconParticleSystem       },
			{ "AnimatorStateMachine.png", &m_iconAnimatorStateMachine },
			{ "Font.png",                 &m_iconFont                 },
		};
		for (auto& entry : icons)
		{
			std::string path = imgDir + entry.file;
			int w = 0, h = 0, ch = 0;
			unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
			if (pixels)
			{
				if (void* handle = renderer()->CreateImGuiTexture(pixels, w, h))
					*entry.target = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle));
				stbi_image_free(pixels);
				HE_LOG_INFO(Editor, "%s", ("EditorApplication: icon loaded — " + path).c_str());
			}
			else
			{
				HE_LOG_WARN(Editor, "%s", ("EditorApplication: icon not found — " + path).c_str());
			}
		}
	}
#endif // HE_IMGUI_ENABLED

	// ── Load the night-sky moon texture ───────────────────────────────────────
	// Pushed to the renderer (not ImGui), so it loads in headless builds too —
	// before the validation dump below renders.
	if (renderer())
	{
		const char* basePath = SDL_GetBasePath();
		std::string moonPath = std::string(basePath ? basePath : "") + "Images/moon.png";

		int w = 0, h = 0, ch = 0;
		unsigned char* pixels = stbi_load(moonPath.c_str(), &w, &h, &ch, 4);
		if (pixels)
		{
			renderer()->SetMoonTexture(pixels, w, h);
			stbi_image_free(pixels);
			HE_LOG_INFO(Editor, "%s", ("EditorApplication: moon texture loaded from " + moonPath).c_str());
		}
		else
		{
			HE_LOG_WARN(Editor, "%s", ("EditorApplication: moon texture not found at " + moonPath).c_str());
		}
	}

	// Create the editor world and register it with the base Application
	m_editorWorld = std::make_unique<HorizonWorld>();
	// Route the world's HorizonCode through the app-wide runtime so widgets, the
	// level script and the GameInstance share one interpreter (and the
	// GameInstance survives scene switches).
	m_editorWorld->setScriptRuntime(&m_gameInstance.runtime());
	// Widget + object nodes route to the editor world's WidgetManager and the
	// app runtime (+ ContentManager to load assets).
	{
		HorizonCode::Runtime::Services svc;
		svc.createWidget  = [this](const std::string& p){ return m_editorWorld ? m_editorWorld->widgets().createWidget(contentManager(), p) : 0; };
		svc.showWidget    = [this](int id){ if (m_editorWorld) m_editorWorld->widgets().showWidget(id); };
		svc.hideWidget    = [this](int id){ if (m_editorWorld) m_editorWorld->widgets().hideWidget(id); };
		svc.destroyWidget = [this](int id){ if (m_editorWorld) m_editorWorld->widgets().destroyWidget(id); };
		svc.createObject  = [this](const std::string& p) -> uint32_t {
			const HE::UUID id = contentManager().loadAsset(p);
			const HorizonCodeClassAsset* a = contentManager().getHorizonCodeClass(id);
			if (!a) return 0u;
			HorizonCode::Graph g;
			if (!a->graphJson.empty()) HorizonCode::fromJson(a->graphJson, g);
			const HorizonCode::InstanceId inst = m_gameInstance.runtime().add(std::move(g));
			m_gameInstance.runtime().fireEvent(inst, "Construct", 0); // let the object init
			return inst;
		};
		svc.destroyObject = [this](uint32_t ref){
			if (ref != 0 && ref != m_gameInstance.runtime().gameInstance())
				m_gameInstance.runtime().destroy(ref); // fires "Destruct"
		};
		// EngineCall nodes dispatch through the HE::api registry against the editor
		// world (+ content). Physics is null here (no PIE physics threaded yet) →
		// physics nodes no-op (null-Ctx tolerance).
		svc.callApi = [this](const std::string& id, const std::vector<HorizonCode::Value>& args)
			-> std::vector<HorizonCode::Value> {
			const HE::api::ApiFn* fn = HE::api::find(id);
			if (!fn) return {};
			// fs/save sandbox: the project's Saved/ directory (follows the loaded
			// project; setSandboxRoot is a cheap string assign).
			const std::string& projPath = m_projectManager.currentProject().path;
			if (!projPath.empty())
				HE::api::fs::setSandboxRoot(
					(std::filesystem::path(projPath).parent_path() / "Saved").string());
			HE::api::Ctx c{ m_editorWorld.get(), nullptr, &contentManager(), &m_audioEngine };
			return fn->invoke(c, args);
		};
		m_gameInstance.runtime().setServices(std::move(svc));
	}
	setWorld(m_editorWorld.get());
	m_propScriptEngine = std::make_unique<ScriptEngine>();
	m_undo.setWorld(m_editorWorld.get());

	if (!m_audioEngine.init())
		HE_LOG_WARN(Editor, "%s", "EditorApplication: audio engine init failed (no audio playback)");

	HE_LOG_INFO(Editor, "%s", "EditorApplication: HorizonWorld created and registered");

	// Register the project-loaded callback BEFORE the first loadProject call so
	// the startup scene is already loaded when OnInit returns.
	m_projectManager.setOnProjectLoaded([this](const std::string& sceneAbsPath)
	{
		setWorld(m_editorWorld.get());

		// Point the ContentManager at this project's content folder so the
		// renderer and the content browser can resolve asset references.
		{
			std::filesystem::path projectPath = m_projectManager.currentProject().path;
			if (std::filesystem::is_regular_file(projectPath))
				projectPath = projectPath.parent_path();
			contentManager().setContentRoot((projectPath / "Content").string());
			// Source control follows the project. Repository discovery runs on the
			// worker, so this returns immediately and the panel shows "checking"
			// until the first answer lands.
			m_git.setCollab(&m_collab);
			m_git.openProject(projectPath);
			// Re-merge the Engine tree against THIS project's overrides (a
			// different project may have different Content/Engine/... files).
			if (m_globalState)
			{
				m_globalState->refreshEngineFolder(contentManager().engineContentRoot(),
				                                    contentManager().contentRoot());
				m_globalState->refreshSourceFolder(); // C++ Source/ tree (empty for non-C++)
			}
			// Index every .hasset's (UUID → path) so scene component references
			// (mesh/material UUIDs) resolve after a reload without a bulk preload.
			const size_t indexed = contentManager().scanContentDirectory();
			HE_LOG_INFO(Editor, "%s",
				("EditorApplication: indexed " + std::to_string(indexed) + " content assets").c_str());
		}

		// Load this project's app-wide GameInstance script (referenceable from
		// any scene via Get Game Instance; OnInit fires when play mode starts).
		loadGameInstanceGraph();

		// Restore the editor tabs that were open the last time this project was used.
		restoreOpenTabs();

		m_currentScenePath.clear();
		if (!sceneAbsPath.empty())
		{
			SceneSerializer serializer;
			bool ok = serializer.load(*m_editorWorld, sceneAbsPath, SerializeFormat::JSON);
			if (ok)
			{
				m_currentScenePath = sceneAbsPath;
				SceneSystems::preloadAssetRefs(*m_editorWorld, contentManager());
				warmupWorldMaterials(); // build custom-material pipelines before the first draw
				HE_LOG_INFO(Editor, "%s",
					("EditorApplication: startup scene loaded from " + sceneAbsPath).c_str());
			}
			else
				HE_LOG_WARN(Editor, "%s",
					("EditorApplication: failed to load startup scene from " + sceneAbsPath).c_str());
		}
		else
		{
			HE_LOG_INFO(Editor, "%s", "EditorApplication: no startup scene defined for this project");
		}

		m_editorWorld->markHierarchyDirty();
		m_undo.clearHistory();
		m_savedRevision = m_undo.revision();
	});

	// If a project was previously opened, load it now (triggers the callback above)
	if (!m_globalState->getLastProjectPath().empty())
	{
		if (m_projectManager.loadProject(m_globalState->getLastProjectPath()))
		{
			m_projectLoaded         = true;
			m_contentRefreshPending = true;
		}
	}

	// Headless validation screenshot: render + capture now, before the paced
	// main loop (which throttles when the window is occluded), then quit.
	if (!m_dumpPath.empty())
		dumpFrameHeadless();

	// Startup toolchain check (cmake + a working C++ compiler) — needed for
	// HorizonCode C++ export codegen and C++-language projects. Skipped for
	// the headless dump path, which never reaches the UI that shows it.
	if (m_dumpPath.empty())
		startToolchainProbe();
		startGitProbe();
}

void EditorApplication::startToolchainProbe()
{
	// Prefer a cmake bundled next to the editor (<app>/cmake) over a system cmake, so a
	// user only needs a C++ compiler. Set before probing (and any later export build).
	if (const char* base = SDL_GetBasePath())
		HE::hccg::setBundledCmakeDir(std::filesystem::path(base) / "cmake");

	if (m_toolchainThread.joinable()) m_toolchainThread.join();
	m_toolchainChecked.store(false, std::memory_order_release);
	m_toolchainThread = std::thread([this]
	{
		HE::hccg::ToolchainProbe probe = HE::hccg::probeToolchain();
		m_toolchainProbe = std::move(probe);
		m_toolchainChecked.store(true, std::memory_order_release);
	});
}

void EditorApplication::startGitProbe()
{
	if (m_gitThread.joinable()) m_gitThread.join();
	m_gitChecked.store(false, std::memory_order_release);
	m_gitThread = std::thread([this]
	{
		HE::Sc::GitProbe probe = HE::Sc::probeGit();
		m_gitProbe = std::move(probe);
		m_gitChecked.store(true, std::memory_order_release);
	});
}

// Writes the identity to the user's global git config, then re-probes so the
// dialog closes itself on success rather than making the user press Recheck.
void EditorApplication::applyGitIdentity(std::string name, std::string email)
{
	if (m_gitIdentityApplying.load(std::memory_order_acquire)) return;
	if (m_gitIdentityThread.joinable()) m_gitIdentityThread.join();
	m_gitIdentityApplying.store(true, std::memory_order_release);

	m_gitIdentityThread = std::thread([this, name = std::move(name), email = std::move(email)]
	{
		auto config = [](const char* key, const std::string& value)
		{
			HE::Proc::Options o;
			o.exe       = "git";
			o.args      = { "config", "--global", key, value };
			o.timeoutMs = 5000;
			// A probe must never stop to ask something it cannot display.
			o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
			return HE::Proc::run(o).ok();
		};
		const bool okName  = config("user.name",  name);
		const bool okEmail = config("user.email", email);
		if (!okName || !okEmail)
		{
			HE_LOG_WARN(SourceControl, "Could not write the git identity to the global config");
		}

		// Re-probe inline on this thread rather than calling startGitProbe(),
		// which would try to join the thread it is called from.
		HE::Sc::GitProbe probe = HE::Sc::probeGit();
		m_gitProbe = std::move(probe);
		m_gitChecked.store(true, std::memory_order_release);
		m_gitIdentityApplying.store(false, std::memory_order_release);
	});
}

// Kick off a best-effort auto-install of the missing toolchain pieces on a worker
// thread (see HcCodegen::installToolchain). Installer output streams into m_installLog
// (guarded by m_installLogMutex); the "Toolchain Missing" dialog shows it live and,
// on completion, re-runs the probe. No-op if an install is already running.
void EditorApplication::startToolchainInstall(bool needCmake, bool needCompiler)
{
	if (m_installRunning.load(std::memory_order_acquire))
		return;
	if (m_installThread.joinable()) m_installThread.join();
	{
		std::lock_guard<std::mutex> lk(m_installLogMutex);
		m_installLog.clear();
	}
	m_installFinished.store(false, std::memory_order_release);
	m_installAttempted.store(false, std::memory_order_release);
	m_installExit.store(0, std::memory_order_release);
	m_installRunning.store(true, std::memory_order_release);
	m_installThread = std::thread([this, needCmake, needCompiler]
	{
		const auto onLine = [this](const std::string& line)
		{
			std::lock_guard<std::mutex> lk(m_installLogMutex);
			m_installLog += line;
			m_installLog += '\n';
		};
		const HE::hccg::ToolchainInstall res =
			HE::hccg::installToolchain(needCmake, needCompiler, onLine);
		m_installAttempted.store(res.attempted, std::memory_order_release);
		m_installExit.store(res.exitCode, std::memory_order_release);
		m_installRunning.store(false, std::memory_order_release);
		m_installFinished.store(true, std::memory_order_release);
	});
}

// Logger sink: capture play-session warnings/errors for the post-PIE report.
// May run on ANY thread (streaming/export workers log too) — appendPlayLog locks.
static void hePlayLogSink(HE::LogLevel level, const char* message, void* user)
{
	if (level != HE::LogLevel::Warning && level != HE::LogLevel::Error &&
	    level != HE::LogLevel::Critical) return;
	static_cast<EditorApplication*>(user)->appendPlayLog(level, message);
}

void EditorApplication::appendPlayLog(HE::LogLevel level, const char* message)
{
	const std::string msg = message ? message : "";
	std::lock_guard<std::mutex> lk(m_playLogMutex);
	// Collapse a repeated error (e.g. a null-reference in a Tick that fires every
	// frame) into a single entry with a repeat count, so it doesn't drown the
	// report or hit the cap — the user still sees it happened, and how often.
	if (!m_playLog.empty() && m_playLog.back().level == level && m_playLog.back().message == msg)
	{
		++m_playLog.back().count;
		return;
	}
	if (m_playLog.size() >= 2000) return; // cap a runaway error loop of DISTINCT messages
	m_playLog.push_back({ level, msg, HE::api::time::elapsed(), 1 });
}

void EditorApplication::OnRender(float dt)
{
	// During play-in-editor, feed the engine clock + input snapshot so time.*/input.*
	// nodes and scripts read fresh per-frame values (edit mode leaves them untouched).
	if (m_isPlaying)
	{
		HE::api::time::advance(dt);
		HE::api::input::pushSdlSnapshot();
		// Zone requests (additive load / unload / show / hide / move) run in PIE
		// against the editor world — leaving play mode restores the pre-play
		// snapshot, which drops zone entities again. Only the FULL level switch
		// and activate stay game-runtime-only (the play snapshot belongs to THIS
		// scene), consumed loudly so a graph author sees why nothing happened.
		using Kind = HE::api::scene::RequestKind;
		for (const auto& r : HE::api::scene::takeRequests())
		{
			HE::api::Ctx c{ m_editorWorld.get(), nullptr, &contentManager(), &m_audioEngine };
			if (r.kindOf() == Kind::Additive && m_editorWorld) // additive zone
			{
				const std::filesystem::path projRoot =
					std::filesystem::path(m_projectManager.currentProject().path).parent_path();
				const auto scenePath = projRoot / r.path;
				SceneSerializer ser;
				std::vector<entt::entity> created;
				std::error_code ec;
				if (!std::filesystem::exists(scenePath, ec) ||
				    !ser.loadAdditive(*m_editorWorld, scenePath, SerializeFormat::JSON, &created))
				{
					HE_LOG_WARN(Editor, "%s",
						("PIE: scene.loadAdditive failed — '" + r.path + "' not found in the project").c_str());
					continue;
				}
				HE::api::scene::ZoneInfo info;
				info.path = r.path;
				info.entities.reserve(created.size());
				for (entt::entity e : created) info.entities.push_back((uint32_t)e);
				for (entt::entity e : created)
				{
					const auto* h = m_editorWorld->registry().try_get<HierarchyComponent>(e);
					if (h && h->parent == m_editorWorld->rootEntity()) { info.root = (uint32_t)e; break; }
				}
				if (info.root == 0 && !created.empty()) info.root = (uint32_t)created.front();
				HE::api::scene::noteZoneLoaded(r.zone, std::move(info));
				if (r.pos != glm::vec3(0.0f)) HE::api::scene::setZonePosition(c, r.zone, r.pos);
				if (r.hidden)                 HE::api::scene::setZoneVisible(c, r.zone, false);
				SceneSystems::preloadAssetRefs(*m_editorWorld, contentManager());
				HE_LOG_INFO(Editor, "%s",
					("PIE: zone " + std::to_string(r.zone) + " loaded ('" + r.path + "', "
					 + std::to_string(created.size()) + " entities"
					 + (r.hidden ? ", hidden" : "") + ")").c_str());
			}
			else if (r.kindOf() == Kind::UnloadZone && m_editorWorld) // unload zone
			{
				if (const auto* z = HE::api::scene::zoneInfo(r.zone))
				{
					auto& reg = m_editorWorld->registry();
					for (uint32_t id : z->entities)
						if (reg.valid((entt::entity)id))
							ScriptApi::destroy(*m_editorWorld, id);
					HE::api::scene::noteZoneUnloaded(r.zone);
				}
			}
			else if (r.kindOf() == Kind::ZoneVisible)  HE::api::scene::setZoneVisible(c, r.zone, r.flag);
			else if (r.kindOf() == Kind::ZonePosition) HE::api::scene::setZonePosition(c, r.zone, r.pos);
			else
				HE_LOG_WARN(Editor, "%s",
					("scene." + std::string(r.kindOf() == Kind::Switch ? "load" : "activate")
					 + (r.path.empty() ? "" : " ('" + r.path + "')")
					 + " runs in the packaged game — play-in-editor keeps the current scene.").c_str());
		}
	}

	// ── Window title ─────────────────────────────────────────────────────
	{
		const std::string& projName = m_projectManager.currentProject().name;
		const bool dirty = m_undo.revision() != m_savedRevision;
		// Only rebuild the title (string concats + filesystem path-stem parse + the
		// SDL_SetWindowTitle syscall) when an input actually changed — this whole block
		// ran every editor frame otherwise.
		static std::string s_lastProj, s_lastScene;
		static int         s_lastDirty = -1;
		if (projName != s_lastProj || m_currentScenePath != s_lastScene ||
		    static_cast<int>(dirty) != s_lastDirty)
		{
			s_lastProj  = projName;
			s_lastScene = m_currentScenePath;
			s_lastDirty = static_cast<int>(dirty);

			std::string title = projName.empty()
				? "Horizon Engine"
				: "Horizon Engine — " + projName;
			const std::string sceneName = m_currentScenePath.empty()
				? "Untitled"
				: std::filesystem::path(m_currentScenePath).stem().string();
			title += " — " + sceneName + (dirty ? " *" : "");
			window()->SetTitle(title);
		}
	}
	// ── Automatischer asynchroner Content-Refresh ─────────────────────────────
	if (m_projectLoaded && m_editorConfig.ContentBrowserRefreshRate > 0)
	{
		m_contentRefreshTimer += dt;
		if (m_contentRefreshTimer >= static_cast<float>(m_editorConfig.ContentBrowserRefreshRate))
		{
			m_contentRefreshTimer = 0.0f;
			// Nur starten wenn kein Refresh bereits läuft
			if (!m_contentRefreshFuture.valid() ||
				m_contentRefreshFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				GlobalState* gs = m_globalState;
				std::string engineContentPath = contentManager().engineContentRoot();
				std::string projectContentRoot = contentManager().contentRoot();
				m_contentRefreshFuture = std::async(std::launch::async, [gs, engineContentPath, projectContentRoot]()
				{
					gs->refreshContentFolder();
					gs->refreshSourceFolder();
					if (!engineContentPath.empty()) gs->refreshEngineFolder(engineContentPath, projectContentRoot);
				});
			}
		}
	}

	// ── Hot-reload: poll disk assets every ~1.5 s ────────────────────────────
	if (m_projectLoaded && renderer())
	{
		m_hotReloadTimer += dt;
		if (m_hotReloadTimer >= 1.5f)
		{
			m_hotReloadTimer = 0.0f;
			auto changed = contentManager().pollHotReload();
			for (const HE::UUID& id : changed)
			{
				switch (contentManager().assetType(id))
				{
				case HE::AssetType::StaticMesh:
				case HE::AssetType::SkeletalMesh:
					renderer()->InvalidateMesh(id);
					break;
				case HE::AssetType::Material:
					renderer()->InvalidateMaterial(id);
					break;
				case HE::AssetType::Texture:
					// Texture GPU caches are keyed by material UUID, not texture UUID.
					// Flush all material caches so re-uploads pick up the new texel data.
					for (const HE::UUID& matId : contentManager().enumerateIds(HE::AssetType::Material))
						renderer()->InvalidateMaterial(matId);
					break;
				default:
					break;
				}
			}
		}
	}

	// Push post-process (engine prefs) + the scene's environment (World entity).
	if (renderer())
	{
		renderer()->SetBloomSettings(IRenderer::BloomSettings{
			m_editorConfig.BloomEnabled,
			m_editorConfig.BloomThreshold,
			m_editorConfig.BloomIntensity});
		renderer()->SetSSAOSettings(IRenderer::SSAOSettings{
			m_editorConfig.SSAOEnabled,
			m_editorConfig.SSAORadius,
			m_editorConfig.SSAOIntensity,
			m_editorConfig.SSAOMethod});
		renderer()->SetGISettings(IRenderer::GISettings{
			m_editorConfig.GlobalIlluminationEnabled,
			m_editorConfig.GIIndirectIntensity,
			m_editorConfig.GILightRadius});
		{
			IRenderer::SSRSettings ssr;
			ssr.enabled      = m_editorConfig.SSREnabled;
			ssr.intensity    = m_editorConfig.SSRIntensity;
			ssr.maxRoughness = m_editorConfig.SSRMaxRoughness;
			ssr.quality      = m_editorConfig.SSRQuality;
			renderer()->SetSSRSettings(ssr);
		}
		{
			IRenderer::GIReflectionSettings gr;
			gr.enabled      = m_editorConfig.GIReflectionsEnabled;
			gr.intensity    = m_editorConfig.GIReflIntensity;
			gr.maxRoughness = m_editorConfig.GIReflMaxRoughness;
			gr.quality      = m_editorConfig.GIReflQuality;
			renderer()->SetGIReflectionSettings(gr);
		}
		// Render path (Forward | Deferred) — gated on the backend capability so an
		// unsupported backend simply stays forward. HE_DUMP_RENDERPATH must win
		// HERE too (not only in the one-shot dump block): this push runs every
		// frame and would otherwise flip a headless capture back to the persisted
		// config value between the dump setup and the captured frame.
		{
			int rpath = m_editorConfig.RenderPath;
			static const char* s_rpOv = std::getenv("HE_DUMP_RENDERPATH");
			if (s_rpOv && *s_rpOv)
				rpath = (std::string(s_rpOv) == "1" || std::string(s_rpOv) == "deferred") ? 1 : 0;
			renderer()->SetRenderPath(
				(rpath == 1 && renderer()->GetCapabilities().supportsDeferredRendering)
					? HE::RenderPath::Deferred : HE::RenderPath::Forward);
		}
		// Regenerate terrain meshes for any entity whose TerrainComponent is dirty
		// (newly created, parameter-edited in the inspector, or just loaded/restored).
		if (m_editorWorld)
		{
			// Shared with the standalone game runtime (GameApplication) so weather,
			// animation, particles, terrain, foliage, nav & LOD behave identically.
			// Pass the physics world in play mode so precipitation collides with the scene.
			// Drive the free-fly PIE camera first so LOD/particles follow the new pose.
			updatePlayCameraController(dt);
			const bool gpuParticles = m_editorConfig.GpuParticles &&
			                          renderer()->GetCapabilities().supportsGpuParticles;
			HE_PROFILE_SCOPE_N("SceneSystemsTick");
			SceneSystems::tick(*m_editorWorld, contentManager(), renderer(),
			                   m_editorCamera.position(), dt,
			                   (m_isPlaying && m_physicsWorld) ? m_physicsWorld.get() : nullptr,
			                   gpuParticles);
		}

		// Step physics at a fixed rate during play mode
		if (m_isPlaying && m_physicsWorld && m_editorWorld)
		{
			HE_PROFILE_SCOPE_N("PhysicsStep");
			m_physicsAccum += dt;
			while (m_physicsAccum >= kPhysicsFixedDt)
			{
				m_physicsWorld->step(*m_editorWorld, kPhysicsFixedDt);
				m_physicsAccum -= kPhysicsFixedDt;
			}
		}

		// Keep spatial audio sources and listener in sync each play-mode frame
		if (m_isPlaying && m_editorWorld)
		{
			HE_PROFILE_SCOPE_N("AudioSpatial");
			AudioSystem::updateSpatial(*m_editorWorld, m_audioEngine);
		}

		// Thunder: when a lightning strike fired this frame, play the configured sound
		// (graceful no-op if no thunderSound asset is set on the WeatherComponent).
		if (m_isPlaying && m_editorWorld)
		{
			for (auto [e, wx] : m_editorWorld->registry().view<WeatherComponent>().each())
			{
				if (wx.flashTriggered && wx.thunderSound != HE::UUID{})
					if (const auto* a = contentManager().getAudio(wx.thunderSound))
						m_audioEngine.play(a->audioData, a->sampleRate, a->channels);
				break;
			}
		}

		// Per-frame script update
		if (m_isPlaying && m_scriptContext)
		{
			HE_PROFILE_SCOPE_N("ScriptUpdate");
			for (auto& [entityId, instId] : m_scriptInstances)
				m_scriptContext->callOnUpdate(instId, dt);
		}

		// Dispatch collision events to scripts (after physics has stepped this frame)
		if (m_isPlaying && m_physicsWorld && m_scriptContext)
		{
			HE_PROFILE_SCOPE_N("CollisionDispatch");
			CollisionSystem::dispatch(*m_physicsWorld, *m_scriptContext, m_scriptInstances);
		}

		// Live widgets: per-frame logic tick (EventTick).
		if (m_isPlaying && m_editorWorld)
		{
			m_editorWorld->widgets().tick(dt);
			// Latent HorizonCode flow (Delay nodes) — PIE only, like the tick.
			m_editorWorld->scripts().update(dt);
			// Player instances: Tick + Input.<Action>.* events.
			m_playerHost.tick(input(), dt);

			// Toggle SDL text-input to match widget text-field focus, so a focused
			// PIE text field receives SDL_EVENT_TEXT_INPUT. Only touched on a focus
			// transition, so it doesn't fight ImGui's own text-input management.
			if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
			{
				const bool want = m_editorWorld->widgets().hasFocusedTextField();
				if (want != m_widgetTextInputActive)
				{
					if (want) SDL_StartTextInput(w); else SDL_StopTextInput(w);
					m_widgetTextInputActive = want;
				}
			}
		}

		// In-game UI pointer input (hover/click) + script event dispatch. The
		// viewport panel feeds the pointer (reportPlayUIPointer); while the PIE
		// mouse capture is engaged there is no cursor, so the pointer is invalid.
		if (m_isPlaying && m_editorWorld && m_uiViewportW > 0.0f && m_uiViewportH > 0.0f)
		{
			// Widget pointer input first — widgets draw on top of entity UI.
			m_editorWorld->widgets().processPointer(
				m_uiViewportW, m_uiViewportH, m_uiPointerX, m_uiPointerY,
				m_uiPointerDown, m_uiPointerValid && !m_playMouseCaptured);

			// Reflect the hovered element's cursor in the PIE viewport. ImGui owns
			// the cursor in the editor, so route through ImGui::SetMouseCursor.
			if (m_uiPointerValid && !m_playMouseCaptured)
			{
				ImGuiMouseCursor mc = ImGuiMouseCursor_Arrow;
				switch (m_editorWorld->widgets().hoverCursor())
				{
					case HE::UICursor::Hand:      mc = ImGuiMouseCursor_Hand;      break;
					case HE::UICursor::Text:      mc = ImGuiMouseCursor_TextInput; break;
					case HE::UICursor::ResizeWE:  mc = ImGuiMouseCursor_ResizeEW;  break;
					case HE::UICursor::ResizeNS:  mc = ImGuiMouseCursor_ResizeNS;  break;
					case HE::UICursor::Move:      mc = ImGuiMouseCursor_ResizeAll; break;
					case HE::UICursor::No:        mc = ImGuiMouseCursor_NotAllowed;break;
					default:                      mc = ImGuiMouseCursor_Arrow;     break;
				}
				ImGui::SetMouseCursor(mc);
			}

			std::vector<UIInputSystem::PointerEvent> uiEvents;
			UIInputSystem::update(*m_editorWorld, m_uiInputState,
			                      m_uiViewportW, m_uiViewportH,
			                      m_uiPointerX, m_uiPointerY,
			                      m_uiPointerDown,
			                      m_uiPointerValid && !m_playMouseCaptured,
			                      uiEvents);
			if (m_scriptContext)
				for (const auto& ev : uiEvents)
				{
					auto it = m_scriptInstances.find(ev.entity);
					if (it == m_scriptInstances.end()) continue;
					const UIScriptEvent se =
						ev.type == UIInputSystem::PointerEvent::Type::Click ? UIScriptEvent::Click :
						ev.type == UIInputSystem::PointerEvent::Type::HoverEnter ? UIScriptEvent::HoverEnter
						                                                         : UIScriptEvent::HoverExit;
					m_scriptContext->callOnUIEvent(it->second, se);
				}
		}

		{
			HE_PROFILE_SCOPE_N("EnvironmentPush");
			pushEnvironment(dt); // auto-advances + pushes the World env component
		}

		// ── Debug draw overlay (selected-entity marker + colliders) ──────────
		if (m_projectLoaded && m_editorWorld)
		{
			DebugDrawBuffer dbg;

			// Selected-entity marker: unit AABB centered on transform position
			if (m_selectedEntity != entt::null && m_editorWorld->registry().valid(m_selectedEntity))
			{
				auto* tc = m_editorWorld->registry().try_get<TransformComponent>(m_selectedEntity);
				if (tc)
				{
					const glm::vec3 p = tc->position;
					dbg.aabb(p - glm::vec3(0.5f), p + glm::vec3(0.5f),
					         glm::vec3(1.0f, 0.8f, 0.0f));
				}
			}

			// Collider wireframes: cyan for solid, magenta for triggers
			{
				auto& reg = m_editorWorld->registry();
				for (auto [entity, col, transform] :
				     reg.view<ColliderComponent, TransformComponent>().each())
				{
					const glm::vec3 color = col.isTrigger
					    ? glm::vec3(1.0f, 0.0f, 1.0f)   // trigger: magenta
					    : glm::vec3(0.0f, 1.0f, 1.0f);  // solid:   cyan
					const glm::vec3 pos = transform.position;
					switch (col.shape)
					{
					case ColliderShape::Box:
						dbg.aabb(pos - col.halfExtents, pos + col.halfExtents, color);
						break;
					case ColliderShape::Sphere:
						dbg.sphere(pos, col.radius, color);
						break;
					case ColliderShape::Capsule:
						dbg.capsule(pos, col.radius, col.height, color);
						break;
					}
				}
			}

			// NavMesh wireframe(s): baked polygons, per-component toggle
			{
				auto& reg = m_editorWorld->registry();
				for (auto [entity, nmc] : reg.view<NavMeshComponent>().each())
					if (nmc.showDebugMesh)
						NavigationSystem::extractNavMeshWireframe(nmc, dbg);
			}

			// New-landscape grid preview: while the Landscape creation form is
			// open (no terrain yet) draw a green wireframe of the terrain-to-be.
			// Emitting it as debug lines means it's depth-tested by the backend —
			// closer objects occlude it per-pixel, like any 3D mesh.
			if (m_editorConfig.mode == EditorMode::Landscape && !m_isPlaying &&
			    m_editorWorld->registry().view<TerrainComponent>().empty())
			{
				TerrainComponent preview;
				preview.sizeX       = m_editorConfig.newTerrain.sizeX;
				preview.sizeZ       = m_editorConfig.newTerrain.sizeZ;
				preview.resolution  = static_cast<uint32_t>(
					std::clamp(m_editorConfig.newTerrain.resolution, 2, 1024));
				preview.heightScale = m_editorConfig.newTerrain.heightScale;
				preview.seed        = m_editorConfig.newTerrain.seed;
				preview.octaves     = m_editorConfig.newTerrain.octaves;
				preview.frequency   = m_editorConfig.newTerrain.frequency;
				preview.lacunarity  = m_editorConfig.newTerrain.lacunarity;
				preview.gain        = m_editorConfig.newTerrain.gain;

				// Coarse, readable grid (not the full sculpt resolution).
				const int   gridN = std::clamp(static_cast<int>(preview.resolution), 2, 33);
				const float halfX = preview.sizeX * 0.5f;
				const float halfZ = preview.sizeZ * 0.5f;
				const float stepX = preview.sizeX / static_cast<float>(gridN - 1);
				const float stepZ = preview.sizeZ / static_cast<float>(gridN - 1);

				std::vector<float> hpre(static_cast<size_t>(gridN) * gridN);
				for (int zi = 0; zi < gridN; ++zi)
					for (int xi = 0; xi < gridN; ++xi)
						hpre[zi * gridN + xi] = terrainHeightAt(
							preview, -halfX + xi * stepX, -halfZ + zi * stepZ);

				const glm::vec3 colMid (0.30f, 0.85f, 0.45f);
				const glm::vec3 colEdge(0.50f, 1.00f, 0.65f); // boundary, brighter
				auto vert = [&](int xi, int zi) {
					return glm::vec3(-halfX + xi * stepX, hpre[zi * gridN + xi],
					                 -halfZ + zi * stepZ);
				};
				for (int zi = 0; zi < gridN; ++zi)
					for (int xi = 0; xi < gridN; ++xi)
					{
						if (xi + 1 < gridN)
							dbg.line(vert(xi, zi), vert(xi + 1, zi),
							         (zi == 0 || zi == gridN - 1) ? colEdge : colMid);
						if (zi + 1 < gridN)
							dbg.line(vert(xi, zi), vert(xi, zi + 1),
							         (xi == 0 || xi == gridN - 1) ? colEdge : colMid);
					}
			}

			// ── Collaboration presence ───────────────────────────────────────
			// Where everyone else is looking, and what they have selected. Drawn
			// as overlay lines only — presence never touches scene state, which
			// is what makes it the low-risk half of collaboration.
			if (m_collab.inSession())
			{
				const auto localId = m_collab.localParticipant();
				for (const auto& p : m_collab.participants())
				{
					if (p.id == localId) continue;   // no gizmo for our own camera

					const HE::Net::PresenceState* pres = m_collab.presenceOf(p.id);
					if (!pres || !pres->valid) continue;

					float rgb[3];
					CollabController::participantColor(p.id, rgb);
					const glm::vec3 color(rgb[0], rgb[1], rgb[2]);

					const glm::vec3 eye(pres->cameraPos[0], pres->cameraPos[1],
					                    pres->cameraPos[2]);
					const glm::quat rot(pres->cameraRot[3], pres->cameraRot[0],
					                    pres->cameraRot[1], pres->cameraRot[2]);

					// A small frustum: four corner rays out to a fixed depth,
					// closed by the far rectangle. Fixed size on purpose — this
					// marks a viewpoint, it does not reproduce their FOV.
					constexpr float kDepth = 1.6f;
					constexpr float kHalfW = 0.55f;
					constexpr float kHalfH = 0.32f;

					const glm::vec3 fwd = rot * glm::vec3(0.0f, 0.0f, -1.0f);
					const glm::vec3 rgt = rot * glm::vec3(1.0f, 0.0f,  0.0f);
					const glm::vec3 up  = rot * glm::vec3(0.0f, 1.0f,  0.0f);
					const glm::vec3 c   = eye + fwd * kDepth;

					const glm::vec3 corners[4] = {
						c + rgt * kHalfW + up * kHalfH,
						c - rgt * kHalfW + up * kHalfH,
						c - rgt * kHalfW - up * kHalfH,
						c + rgt * kHalfW - up * kHalfH,
					};
					for (int i = 0; i < 4; ++i)
					{
						dbg.line(eye, corners[i], color);
						dbg.line(corners[i], corners[(i + 1) % 4], color);
					}
					// Short stub marking "up", so the gizmo's roll is readable.
					dbg.line(c, c + up * (kHalfH * 1.6f), color);

					// Their selection, in the same colour — this is what makes
					// "don't both grab that object" visible before it happens.
					auto& reg = m_editorWorld->registry();
					for (const std::uint64_t raw : pres->selection)
					{
						const auto e = static_cast<entt::entity>(
							static_cast<entt::id_type>(raw));
						if (!reg.valid(e)) continue;   // not present in our world
						if (auto* tc = reg.try_get<TransformComponent>(e))
						{
							dbg.aabb(tc->position - glm::vec3(0.6f),
							         tc->position + glm::vec3(0.6f), color);
						}
					}
				}
			}

			// Timed debug primitives from HC/script debug.* calls ride along with
			// the editor's own gizmo lines (they age with real dt in play mode,
			// and stay frozen while paused/editing).
			std::vector<DebugLine> merged = dbg.lines();
			HE::api::debug::collect(m_isPlaying ? dt : 0.0f, merged);
			renderer()->SetDebugLines(merged);
		}
		else
		{
			std::vector<DebugLine> apiDbg;
			HE::api::debug::collect(m_isPlaying ? dt : 0.0f, apiDbg);
			renderer()->SetDebugLines(apiDbg);
		}
	}

	// ── Viewport texture registration (D3D12 / Vulkan) ──────────────────────
	// The renderer creates the offscreen RT inside Render() (previous frame).
	// When the RT is (re)created HasViewportResourceChanged() fires; we
	// allocate an SRV / descriptor set in the editor-side ImGui heap here,
	// then hand the opaque handle back to the renderer so GetViewportTexture()
	// returns it for use in ImGui::Image().
#ifdef _WIN32
	if (m_backend == HE::RendererBackend::D3D12)
	{
		auto* dx12 = static_cast<D3D12Renderer*>(renderer());
		if (dx12 && dx12->HasViewportResourceChanged())
		{
			auto* device = static_cast<ID3D12Device*>(dx12->GetDevice());
			auto* alloc  = static_cast<D3D12DescriptorHeapAllocator*>(m_d3d12SrvAllocator);
			if (device && alloc)
			{
				// Release the previous slot if we already had one.
				if (m_d3d12ViewportSrvAllocated)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE cpu{}; cpu.ptr = static_cast<SIZE_T>(m_d3d12ViewportSrvCpuPtr);
					D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; gpu.ptr = static_cast<UINT64>(m_d3d12ViewportSrvGpuPtr);
					alloc->Free(cpu, gpu);
					m_d3d12ViewportSrvAllocated = false;
				}
				auto* rt = static_cast<ID3D12Resource*>(dx12->GetViewportD3DResource());
				if (rt)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
					D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
					alloc->Alloc(&cpu, &gpu);
					m_d3d12ViewportSrvCpuPtr    = static_cast<uint64_t>(cpu.ptr);
					m_d3d12ViewportSrvGpuPtr    = static_cast<uint64_t>(gpu.ptr);
					m_d3d12ViewportSrvAllocated = true;

					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
					srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
					srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srvDesc.Texture2D.MipLevels     = 1;
					device->CreateShaderResourceView(rt, &srvDesc, cpu);

					// Pass the GPU handle as the ImGui texture ID.
					dx12->SetViewportImGuiHandle(reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.ptr)));
				}
				dx12->ClearViewportResourceChanged();
			}
		}
	}
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
	if (m_backend == HE::RendererBackend::Vulkan)
	{
		auto* vk = static_cast<VulkanRenderer*>(renderer());
		if (vk && vk->HasViewportResourceChanged())
		{
			// Remove the old descriptor set if present.
			if (m_vkViewportDescSet)
			{
				ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(m_vkViewportDescSet));
				m_vkViewportDescSet = nullptr;
			}
			auto sampler = reinterpret_cast<VkSampler>(vk->GetViewportVkSampler());
			auto view    = reinterpret_cast<VkImageView>(vk->GetViewportVkImageView());
			if (sampler && view)
			{
				VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
					sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				m_vkViewportDescSet = reinterpret_cast<void*>(ds);
				vk->SetViewportImGuiHandle(reinterpret_cast<void*>(ds));
			}
			vk->ClearViewportResourceChanged();
		}
	}
#endif

	// ── Collaboration ─────────────────────────────────────────────────────
	// CollabSession is entirely poll-driven, so without this call nothing at all
	// happens — no joins, no snapshots, no presence.
	{
		m_collab.setWorld(m_editorWorld.get());
		const auto nowMs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		m_collab.update(nowMs);
	// Not gated on a project being loaded: a close still has to be drained.
	m_git.update(nowMs);

		if (m_collab.inSession()) syncStructuralChanges();

		// Claim the selected entity, so everyone else sees it is being worked on
		// before they click it themselves.
		if (m_collab.inSession())
		{
			const std::uint64_t subject =
				m_selectedEntity == entt::null
					? 0ull
					: static_cast<std::uint64_t>(entt::to_integral(m_selectedEntity));
			m_collab.followSelection(subject);

			// Publish its transform while we hold it. Sending unconditionally is
			// fine — publishTransform drops unchanged values and rate-limits the
			// rest, so a still object costs nothing.
			if (subject != 0 && m_editorWorld &&
			    m_editorWorld->registry().valid(m_selectedEntity))
			{
				if (auto* tc = m_editorWorld->registry()
				                   .try_get<TransformComponent>(m_selectedEntity))
				{
					const float p[3] = { tc->position.x, tc->position.y, tc->position.z };
					const float r[3] = { tc->rotation.x, tc->rotation.y, tc->rotation.z };
					const float s[3] = { tc->scale.x, tc->scale.y, tc->scale.z };

					// Record the change for OUR undo stack before publishing it.
					// The "before" is whatever we last saw for this subject; the
					// first observation only establishes a baseline.
					const float now9[9] = { p[0], p[1], p[2], r[0], r[1], r[2],
					                        s[0], s[1], s[2] };
					if (m_undoBaselineSubject == subject)
					{
						bool changed = false;
						for (int i = 0; i < 9; ++i)
						{
							if (std::fabs(now9[i] - m_undoBaseline[i]) > 0.0005f)
							{ changed = true; break; }
						}
						if (changed)
						{
							m_collabUndo.recordTransform(subject, m_undoBaseline, now9);
							std::memcpy(m_undoBaseline, now9, sizeof(now9));
						}
					}
					else
					{
						m_undoBaselineSubject = subject;
						std::memcpy(m_undoBaseline, now9, sizeof(now9));
					}

					m_collab.publishTransform(subject, p, r, s, nowMs);
				}

				// Everything the transform delta does not cover — mesh, material,
				// light, collider, script, name… Serializing one entity is cheap,
				// and publishComponents hashes the result so an entity that is
				// merely selected rather than edited sends nothing.
				{
					SceneSerializer serializer;
					const std::vector<std::uint8_t> comps =
						serializer.serializeEntityComponents(*m_editorWorld, m_selectedEntity);
					if (!comps.empty())
					{
						m_collab.publishComponents(
							static_cast<std::uint32_t>(entt::to_integral(m_selectedEntity)),
							comps);
					}
				}
			}
		}

		// Feed the local camera + selection so remote peers can draw them.
		if (m_collab.active())
		{
			const glm::vec3 pos = m_editorCamera.position();
			// The view matrix maps world → camera; a remote peer draws the
			// frustum in world space, so send the inverse rotation.
			const glm::quat rot =
				glm::quat_cast(glm::transpose(glm::mat3(m_editorCamera.viewMatrix())));
			const float p[3] = { pos.x, pos.y, pos.z };
			const float r[4] = { rot.x, rot.y, rot.z, rot.w };

			std::vector<std::uint64_t> selection;
			if (m_selectedEntity != entt::null)
				selection.push_back(static_cast<std::uint64_t>(entt::to_integral(m_selectedEntity)));

			m_collab.setLocalPresence(p, r, selection);
		}
	}

	AppContext ctx = makeContext();
	EditorUI::render(ctx, dt);
	saveOpenTabs(); // persists only when the tab set/active index actually changed

	// ── FPS counter ───────────────────────────────────────────────────────
	if (dt > 0.0f)
	{
		m_fpsAccum      += 1.0f / dt;
		m_fpsAccumCount += 1;
		if (m_fpsAccumCount >= 20)
		{
			m_smoothFps     = m_fpsAccum / static_cast<float>(m_fpsAccumCount);
			m_fpsAccum      = 0.0f;
			m_fpsAccumCount = 0;
		}

		m_frametimeHistory[m_fpsHistoryOffset] = dt * 1000.0f; // ms
		m_fpsHistoryOffset = (m_fpsHistoryOffset + 1) % k_fpsHistorySize;
	}
}

// ─── Headless frame dump ──────────────────────────────────────────────────────
namespace
{
	// Minimal dependency-free 32-bit BGRA, top-down BMP writer. Input is
	// tightly-packed RGBA8, top row first. Used by the validation screenshot
	// path; convert to PNG with `sips` if needed.
	bool writeBMP(const std::string& path, const std::vector<uint8_t>& rgba,
	              uint32_t w, uint32_t h)
	{
		if (rgba.size() < static_cast<size_t>(w) * h * 4) return false;
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) return false;

		const uint32_t pixelBytes = w * h * 4;
		const uint32_t fileSize   = 54 + pixelBytes;
		auto u16 = [&](uint16_t v){ out.put(char(v & 0xFF)); out.put(char((v >> 8) & 0xFF)); };
		auto u32 = [&](uint32_t v){ for (int i = 0; i < 4; ++i) out.put(char((v >> (8 * i)) & 0xFF)); };
		auto i32 = [&](int32_t v){ u32(static_cast<uint32_t>(v)); };

		// BITMAPFILEHEADER
		out.put('B'); out.put('M');
		u32(fileSize); u16(0); u16(0); u32(54);
		// BITMAPINFOHEADER
		u32(40); i32(static_cast<int32_t>(w)); i32(-static_cast<int32_t>(h)); // negative = top-down
		u16(1); u16(32); u32(0); u32(pixelBytes); i32(2835); i32(2835); u32(0); u32(0);

		// Pixels: RGBA → BGRA
		for (uint32_t i = 0; i < w * h; ++i)
		{
			const uint8_t* px = &rgba[static_cast<size_t>(i) * 4];
			out.put(char(px[2])); out.put(char(px[1])); out.put(char(px[0])); out.put(char(px[3]));
		}
		return out.good();
	}
}

void EditorApplication::dumpFrameHeadless()
{
	if (m_dumpPath.empty() || m_dumpDone) return;
	IRenderer* r = renderer();
	if (!r)
	{
		HE_LOG_ERROR(Editor, "%s", "EditorApplication: no renderer for frame dump");
		m_dumpDone = true;
		return;
	}

	// Render the scene into a fixed offscreen target a few times, bypassing the
	// ImGui overlay and the window swap. Doing this here (before the paced main
	// loop) means it works even when the window is occluded / App-Napped on
	// macOS — where the normal loop throttles to a near-frozen frame rate and a
	// loop-driven capture never fires.
	r->SetOverlayCallback(nullptr);
	r->SetBloomSettings(IRenderer::BloomSettings{
		m_editorConfig.BloomEnabled, m_editorConfig.BloomThreshold, m_editorConfig.BloomIntensity});
	r->SetSSAOSettings(IRenderer::SSAOSettings{
		m_editorConfig.SSAOEnabled, m_editorConfig.SSAORadius, m_editorConfig.SSAOIntensity,
		m_editorConfig.SSAOMethod});
	{
		// HE_DUMP_GI: override the persisted GI toggle for this capture only (does
		// not touch config.json), so he_shot.py can compare GI-on vs GI-off without
		// the editor's Preferences state leaking between captures.
		const bool dumpGI = [&]{
			const char* v = std::getenv("HE_DUMP_GI");
			return v && *v ? std::atof(v) > 0.5 : m_editorConfig.GlobalIlluminationEnabled;
		}();
		r->SetGISettings(IRenderer::GISettings{
			dumpGI, m_editorConfig.GIIndirectIntensity, m_editorConfig.GILightRadius});
	}
	{
		// HE_DUMP_SSR: override the persisted SSR toggle for this capture only.
		// HE_DUMP_SSRQUALITY: override the quality tier (0 = raw trace without
		// the P4 blur, 1/2 = blurred) for headless A/B of the blur passes.
		const bool dumpSSR = [&]{
			const char* v = std::getenv("HE_DUMP_SSR");
			return v && *v ? std::atof(v) > 0.5 : m_editorConfig.SSREnabled;
		}();
		IRenderer::SSRSettings ssr{
			dumpSSR, m_editorConfig.SSRIntensity, m_editorConfig.SSRMaxRoughness};
		ssr.quality = m_editorConfig.SSRQuality;
		if (const char* q = std::getenv("HE_DUMP_SSRQUALITY"); q && *q)
			ssr.quality = std::atoi(q);
		r->SetSSRSettings(ssr);
	}
	{
		// HE_DUMP_GIREFL: override the persisted GI-reflections toggle for this
		// capture only (ray-traced-reflections A/B without touching config.json).
		// HE_DUMP_GIREFLQUALITY: override the tier (0 raw / 1 blur / 2 glossy).
		const bool dumpGR = [&]{
			const char* v = std::getenv("HE_DUMP_GIREFL");
			return v && *v ? std::atof(v) > 0.5 : m_editorConfig.GIReflectionsEnabled;
		}();
		IRenderer::GIReflectionSettings gr;
		gr.enabled      = dumpGR;
		gr.intensity    = m_editorConfig.GIReflIntensity;
		gr.maxRoughness = m_editorConfig.GIReflMaxRoughness;
		gr.quality      = m_editorConfig.GIReflQuality;
		if (const char* q = std::getenv("HE_DUMP_GIREFLQUALITY"); q && *q)
			gr.quality = std::atoi(q);
		r->SetGIReflectionSettings(gr);
	}
	{
		// HE_DUMP_RENDERPATH: override the persisted render path for this capture
		// only (he_shot Forward/Deferred A/B without touching config.json).
		int path = m_editorConfig.RenderPath;
		if (const char* v = std::getenv("HE_DUMP_RENDERPATH"); v && *v)
			path = (std::string(v) == "1" || std::string(v) == "deferred") ? 1 : 0;
		r->SetRenderPath((path == 1 && r->GetCapabilities().supportsDeferredRendering)
			? HE::RenderPath::Deferred : HE::RenderPath::Forward);
	}

	// ── Sky-test capture (HE_DUMP_SKYTEST): aim the camera up at the sky and override
	// the scene environment so a headless dump exercises the sky features (stars /
	// nebula / 3D clouds / contrails) that the default down-looking editor camera and
	// daytime env never show. Env knobs are read from optional vars so several scenes
	// can be captured without rebuilding. No-op unless HE_DUMP_SKYTEST is set.
	if (const char* st = std::getenv("HE_DUMP_SKYTEST"); st && *st && m_editorWorld)
	{
		auto envF = [](const char* k, float d){ const char* v = std::getenv(k); return v && *v ? std::atof(v) : d; };
		// HE_DUMP_NOSKY: exercise the "no Sky entity" path (flat background, no sky pass).
		if (envF("HE_DUMP_NOSKY", 0.0f) > 0.5f)
			m_editorWorld->removeSky();
		// Otherwise make sure there IS a Sky entity to configure — the world/scene may
		// have started empty (only the Game/Simulation templates seed a sky).
		else if (m_editorWorld->environmentEntity() == entt::null)
			m_editorWorld->addSky();
		Entity dumpSky = m_editorWorld->environmentEntity();
		if (auto* e = dumpSky == entt::null ? nullptr
		            : m_editorWorld->registry().try_get<EnvironmentComponent>(dumpSky))
		{
			e->dayNightCycle  = true;
			e->timeOfDay      = static_cast<float>(envF("HE_DUMP_TOD", 0.0f));        // 0 = midnight
			e->cloudMode      = static_cast<int>(envF("HE_DUMP_CLOUDMODE", 1.0f));
			e->cloudCoverage  = static_cast<float>(envF("HE_DUMP_COVERAGE", 0.5f));
			e->lowResClouds   = envF("HE_DUMP_LOWRESCLOUDS", 0.0f) > 0.5f;  // diag: exercise the quarter-res cloud reprojection path
			e->contrailAmount = static_cast<float>(envF("HE_DUMP_CONTRAILS", 0.0f));
			e->cirrusAmount   = static_cast<float>(envF("HE_DUMP_CIRRUS", 0.0f));
			e->cirrusSeed     = static_cast<float>(envF("HE_DUMP_CIRRUSSEED", 0.0f));
			e->cloudHeight    = static_cast<float>(envF("HE_DUMP_CLOUDHEIGHT", 200.0f));
			e->nebulaIntensity   = static_cast<float>(envF("HE_DUMP_NEBULA",   e->nebulaIntensity));
			e->nebulaSeed        = static_cast<float>(envF("HE_DUMP_NEBSEED",  e->nebulaSeed));
			e->nebulaCoverage    = static_cast<float>(envF("HE_DUMP_NEBCOVER", e->nebulaCoverage));
			e->nebulaQuality     = static_cast<int>(envF("HE_DUMP_NEBQUALITY", e->nebulaQuality));
			// Nebula colours as "r,g,b" (0..1). The loaded scene carries SERIALIZED colours,
			// so new component defaults never show up in a dump without these overrides.
			auto envV3 = [](const char* k, glm::vec3 d){
				const char* v = std::getenv(k); glm::vec3 c;
				if (v && *v && std::sscanf(v, "%f,%f,%f", &c.x, &c.y, &c.z) == 3) return c;
				return d;
			};
			e->nebulaColor  = envV3("HE_DUMP_NEBCOL1", e->nebulaColor);
			e->nebulaColor2 = envV3("HE_DUMP_NEBCOL2", e->nebulaColor2);
			e->nebulaColor3 = envV3("HE_DUMP_NEBCOL3", e->nebulaColor3);
			e->moonPhase         = static_cast<float>(envF("HE_DUMP_MOONPHASE", e->moonPhase));
			e->milkyWayIntensity = static_cast<float>(envF("HE_DUMP_MILKYWAY", e->milkyWayIntensity));
			e->starSizeVariation = static_cast<float>(envF("HE_DUMP_STARVAR",  e->starSizeVariation));
			e->starDensity       = static_cast<float>(envF("HE_DUMP_STARDENS", e->starDensity));
			e->starSize          = static_cast<float>(envF("HE_DUMP_STARSIZE", e->starSize));
			e->starGlow          = static_cast<float>(envF("HE_DUMP_STARGLOW", e->starGlow));
			e->starTwinkle       = static_cast<float>(envF("HE_DUMP_STARTWINKLE", e->starTwinkle));
			e->auroraIntensity   = static_cast<float>(envF("HE_DUMP_AURORA", e->auroraIntensity));
			e->auroraHeight        = static_cast<float>(envF("HE_DUMP_AURHEIGHT", e->auroraHeight));
			e->auroraFragmentation = static_cast<float>(envF("HE_DUMP_AURFRAG",   e->auroraFragmentation));
			e->rainAmount        = static_cast<float>(envF("HE_DUMP_RAIN",     e->rainAmount));
			e->godRays           = static_cast<float>(envF("HE_DUMP_GODRAYS", e->godRays));
			e->shootingStars     = static_cast<float>(envF("HE_DUMP_METEORS", e->shootingStars));
			e->lensFlare         = static_cast<float>(envF("HE_DUMP_LENSFLARE", e->lensFlare));
		}
		// Look slightly up toward the sky from a low vantage. HE_DUMP_YAW rotates the
		// heading (0 = toward -Z, 180 = toward +Z) so e.g. the aurora band can be framed.
		const float pitch = glm::radians(static_cast<float>(envF("HE_DUMP_PITCH", 22.0f)));
		const float yaw   = glm::radians(static_cast<float>(envF("HE_DUMP_YAW", 0.0f)));
		const glm::vec3 fwd(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
		                    -std::cos(yaw) * std::cos(pitch));
		const glm::vec3 camPos(static_cast<float>(envF("HE_DUMP_CAMX", 0.0f)),
		                       static_cast<float>(envF("HE_DUMP_CAMY", 2.0f)),
		                       static_cast<float>(envF("HE_DUMP_CAMZ", 0.0f)));
		m_editorCamera.setOrientation(camPos, fwd);
		r->SetEditorCamera(m_editorCamera.makeOverride());
	}

	// ── Material asset→pixel proof (HE_DUMP_MATERIALTEST): put a real entity in the
	// scene whose MaterialAsset carries a custom shader, so the NORMAL render path
	// (extractor → RenderObject.materialAssetId → ResolveMaterialShader → cross-compiled
	// pipeline) draws it — witnessing the full asset→pixel path, not just an inline demo.
	HE::UUID s_matTestId{}; // material-test id, reused by the preview-path witness below
	if (const char* mt = std::getenv("HE_DUMP_MATERIALTEST"); mt && *mt && m_editorWorld)
	{
		MaterialAsset mat;
		mat.type = HE::AssetType::Material;
		mat.name = "MatTest";
		mat.baseColor[0] = 0.2f; mat.baseColor[1] = 0.8f; mat.baseColor[2] = 0.3f;
		// M3 witness: author the material as a NODE GRAPH (the same authoring model the
		// editor tab edits), then generate the shader from it — so the screenshot proves
		// graph → codegen → cross-compile → pixels, not a hand-written fragment.
		// Graph: lerp(orange, blue, fresnel) → lit BaseColor; sin(time) → Metallic.
		{
			HE::MaterialGraph g;
			if (std::string(mt) == "noisecube")
			{
				// Auto-UV witness: the user's exact graph — UV → FBM → (colour ×) → BaseColor
				// on a cube WITHOUT texture coords. Without generated UVs this is solid black
				// (vUV = 0 → heFbm(0) = 0); with box-projection UVs it mottles per face.
				const int out = g.addNode(HE::MatNodeType::Output);
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.85f; g.findNode(col)->p[1] = 0.30f; g.findNode(col)->p[2] = 0.20f;
				const int uv  = g.addNode(HE::MatNodeType::UV);
				const int fbm = g.addNode(HE::MatNodeType::Fbm);
				g.findNode(fbm)->p[0] = 8.0f;
				const int mul = g.addNode(HE::MatNodeType::Multiply);
				g.connect(uv,  0, fbm, 0);
				g.connect(col, 0, mul, 0);
				g.connect(fbm, 0, mul, 1);
				g.connect(mul, 0, out, 0);
			}
			else if (std::string(mt) == "translucent")
			{
				// Translucent blend witness: constant 0.45 opacity → the sphere must be
				// see-through (sorted blend pass with the material's OWN pipeline).
				const int out = g.addNode(HE::MatNodeType::Output);
				g.findNode(out)->p[1] = 2.0f; // Translucent
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.9f; g.findNode(col)->p[1] = 0.5f; g.findNode(col)->p[2] = 0.1f;
				const int op  = g.addNode(HE::MatNodeType::ConstFloat);
				g.findNode(op)->p[0] = 0.45f;
				g.connect(col, 0, out, 0);
				g.connect(op,  0, out, 4); // Opacity
			}
			else if (std::string(mt) == "masked")
			{
				// Masked blend witness: a checkerboard mask discards half the sphere's
				// fragments — hard-edged holes, still in the opaque pass.
				const int out = g.addNode(HE::MatNodeType::Output);
				g.findNode(out)->p[1] = 1.0f; // Masked
				g.findNode(out)->p[2] = 0.5f; // cutoff
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.85f; g.findNode(col)->p[1] = 0.25f; g.findNode(col)->p[2] = 0.2f;
				const int chk = g.addNode(HE::MatNodeType::Checker); // UV falls back to vUV
				g.connect(col, 0, out, 0);
				g.connect(chk, 0, out, 4); // OpacityMask
			}
			else if (std::string(mt) == "wpo")
			{
				// WPO witness: sin(worldPos.y * 8) * 0.35 offsets X → a wavy sphere.
				// The offset happens in the VERTEX stage (graph-generated custom VS).
				const int out = g.addNode(HE::MatNodeType::Output);
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.3f; g.findNode(col)->p[1] = 0.75f; g.findNode(col)->p[2] = 0.35f;
				g.connect(col, 0, out, 0);
				const int wp   = g.addNode(HE::MatNodeType::WorldPos);
				const int spl  = g.addNode(HE::MatNodeType::SplitRGBA);
				const int freq = g.addNode(HE::MatNodeType::ConstFloat);
				g.findNode(freq)->p[0] = 8.0f;
				const int m1   = g.addNode(HE::MatNodeType::Multiply);
				const int sn   = g.addNode(HE::MatNodeType::Sine);
				const int amp  = g.addNode(HE::MatNodeType::ConstFloat);
				g.findNode(amp)->p[0] = 0.35f;
				const int m2   = g.addNode(HE::MatNodeType::Multiply);
				const int cmb  = g.addNode(HE::MatNodeType::Combine3);
				g.connect(wp,   0, spl, 0); // worldPos → split (G = y)
				g.connect(spl,  1, m1,  0);
				g.connect(freq, 0, m1,  1);
				g.connect(m1,   0, sn,  0);
				g.connect(sn,   0, m2,  0);
				g.connect(amp,  0, m2,  1);
				g.connect(m2,   0, cmb, 0); // offset in X
				g.connect(cmb,  0, out, 6); // WPO
			}
			else if (std::string(mt) == "switchon" || std::string(mt) == "switchoff")
			{
				// Static-switch permutation witness: same graph, two baked permutations.
				// ON → red branch only, OFF → blue branch only (the other is culled).
				const int out = g.addNode(HE::MatNodeType::Output);
				const int sw  = g.addNode(HE::MatNodeType::StaticSwitch);
				g.findNode(sw)->s = "UseRed";
				g.findNode(sw)->p[0] = std::string(mt) == "switchon" ? 1.0f : 0.0f;
				const int red = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(red)->p[0] = 0.9f; g.findNode(red)->p[1] = 0.1f; g.findNode(red)->p[2] = 0.1f;
				const int blu = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(blu)->p[0] = 0.1f; g.findNode(blu)->p[1] = 0.2f; g.findNode(blu)->p[2] = 0.9f;
				g.connect(red, 0, sw, 0);
				g.connect(blu, 0, sw, 1);
				g.connect(sw,  0, out, 0);
			}
			else if (std::string(mt) == "noise")
			{
				// v6 witness: colour × Noise Texture → mottled ("fleckig") BaseColor.
				const int out = g.addNode(HE::MatNodeType::Output);
				const int col = g.addNode(HE::MatNodeType::ConstColor);
				g.findNode(col)->p[0] = 0.85f; g.findNode(col)->p[1] = 0.30f; g.findNode(col)->p[2] = 0.20f;
				const int tex = g.addNode(HE::MatNodeType::NoiseTexture);
				g.findNode(tex)->p[0] = 10.0f; // Scale — fine speckle, obvious in a capture
				const int mul = g.addNode(HE::MatNodeType::Multiply);
				g.connect(col, 0, mul, 0);
				g.connect(tex, 0, mul, 1);
				g.connect(mul, 0, out, 0); // BaseColor
			}
			else
			{
			const int out  = g.addNode(HE::MatNodeType::Output);
			// Base color as a NAMED PARAM → exercises the HeParams uniform path (the value
			// reaches the shader through the UBO upload, not as a baked constant).
			const int a    = g.addNode(HE::MatNodeType::ParamColor);
			g.findNode(a)->s = "BaseTint";
			g.findNode(a)->p[0] = 0.95f; g.findNode(a)->p[1] = 0.42f; g.findNode(a)->p[2] = 0.18f;
			const int b    = g.addNode(HE::MatNodeType::ConstColor);
			g.findNode(b)->p[0] = 0.10f; g.findNode(b)->p[1] = 0.35f; g.findNode(b)->p[2] = 0.85f;
			const int fres = g.addNode(HE::MatNodeType::Fresnel);
			g.findNode(fres)->p[0] = 1.2f; // wide rim so the effect is obvious in captures
			const int lerp = g.addNode(HE::MatNodeType::Lerp);
			const int time = g.addNode(HE::MatNodeType::Time);
			const int sine = g.addNode(HE::MatNodeType::Sine);
			g.connect(a,    0, lerp, 0);
			g.connect(b,    0, lerp, 1);
			g.connect(fres, 0, lerp, 2);
			g.connect(lerp, 0, out,  0); // BaseColor
			g.connect(time, 0, sine, 0);
			g.connect(sine, 0, out,  1); // Metallic
			}
			mat.nodeGraphJson = HE::materialGraphToJson(g);
			const HE::MatShaderGen gen = HE::generateFragment(g);
			mat.customShaderFragGlsl = gen.glsl;
			mat.customShaderGBufGlsl = gen.glslGBuffer;
			mat.customShaderVertGlsl = gen.vertexBody; // WPO vertex body (if the graph uses it)
			mat.blendMode            = gen.blendMode;
			for (const auto& slot : gen.params)
			{
				mat.shaderParamData.insert(mat.shaderParamData.end(),
				                           slot.value, slot.value + 4);
				mat.graphParamNames.push_back(slot.name); // runtime setMaterialParam by name
				mat.graphParamTypes.push_back(static_cast<uint8_t>(slot.kind));
			}

			// Witness the PRECOMPILED path (HE_DUMP_MATPRECOMPILE): bake per-backend
			// shader variants into the material NOW, exactly as the exporter would, so
			// the renderer takes the getOrBuild*(precompiled) branch instead of cross-
			// compiling at draw time. A capture matching the non-baked run proves the
			// baked path renders identically.
			if (const char* pc = std::getenv("HE_DUMP_MATPRECOMPILE"); pc && *pc)
			{
				using LB = HE::MaterialShaderLibrary::Backend;
				HE::MaterialShaderLibrary lib;
				const uint64_t h = std::hash<std::string>{}(gen.glsl);
				auto bake = [&](HE::RendererBackend rb, LB lb) {
					const auto& v = gen.vertexBody.empty()
						? lib.standardVertex(lb)
						: lib.customVertex(std::hash<std::string>{}(gen.vertexBody),
						                   gen.vertexBody, lb);
					const auto& f = lib.fragment(h, gen.glsl, lb);
					if (v.ok && f.ok) {
						MaterialShaderVariant var;
						var.backend  = static_cast<uint8_t>(rb);
						var.vertex   = v.source;
						var.fragment = f.source;
						mat.precompiledShaders.push_back(std::move(var));
					}
				};
				bake(HE::RendererBackend::OpenGL, LB::GLSL410);
				bake(HE::RendererBackend::Metal,  LB::Metal);
				HE_LOG_INFO(Editor, "%s",
					"EditorApplication: HE_DUMP_MATPRECOMPILE baked precompiled shader variants");
			}
		}
		const HE::UUID matId = contentManager().registerMaterial(std::move(mat));
		s_matTestId = matId;

		// Witness the runtime scripting param path (HE_DUMP_SETPARAM="Name,r,g,b"):
		// set a named graph parameter BY NAME exactly as a script's
		// horizon.setMaterialParam would, so the capture reflects the override
		// (the harness graph exposes ParamColor "BaseTint").
		if (const char* sp = std::getenv("HE_DUMP_SETPARAM"); sp && *sp)
		{
			std::string s(sp); std::string name; float rgb[3] = { 0, 0, 0 };
			const size_t c0 = s.find(',');
			if (c0 != std::string::npos)
			{
				name = s.substr(0, c0);
				std::sscanf(s.c_str() + c0 + 1, "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]);
				const float v[4] = { rgb[0], rgb[1], rgb[2], 0.0f };
				const bool ok = contentManager().setMaterialParam(matId, name, v, 4);
				HE_LOG_INFO(Editor, "%s", ok
					? ("EditorApplication: HE_DUMP_SETPARAM set '" + name + "' at runtime").c_str()
					: ("EditorApplication: HE_DUMP_SETPARAM param '" + name + "' not found").c_str());
			}
		}

		// Test mesh (SoA loose asset). Default: a procedural UV sphere (curved surface shows
		// per-normal shading). MATERIALTEST=noisecube: a UNIT CUBE with UVs all (0,0) — i.e.
		// a mesh WITHOUT real UVs — to reproduce the "Noise Texture on a cube is black" case
		// (UV noise collapses at vUV=0; world-space noise must still mottle it).
		StaticMeshAsset sphere;
		sphere.type = HE::AssetType::StaticMesh;
		const bool cubeMesh = (std::string(mt) == "noisecube");
		sphere.name = cubeMesh ? "MatTestCube" : "MatTestSphere";
		if (cubeMesh)
		{
			const float h = 2.0f;
			const glm::vec3 fn[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
			const glm::vec3 fq[6][4] = {
				{{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}}, // +Z
				{{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}}, // -Z
				{{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}}, // +X
				{{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}}, // -X
				{{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}}, // +Y
				{{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}}, // -Y
			};
			for (int f = 0; f < 6; ++f)
			{
				const uint32_t base = (uint32_t)(sphere.vertices.size() / 3);
				for (int k = 0; k < 4; ++k)
				{
					sphere.vertices.insert(sphere.vertices.end(), { fq[f][k].x, fq[f][k].y, fq[f][k].z });
					sphere.normals.insert(sphere.normals.end(),   { fn[f].x, fn[f].y, fn[f].z });
					// NO uvs on purpose → exercises ContentManager's box-projection UV
					// fallback (the real default cube also ships without texture coords).
				}
				sphere.indices.insert(sphere.indices.end(),
					{ base, base + 1, base + 2, base, base + 2, base + 3 });
			}
		}
		else
		{
			const int segU = 48, segV = 24; const float radius = 2.5f;
			const float kPi = glm::pi<float>();
			for (int y = 0; y <= segV; ++y)
			{
				const float v = (float)y / segV, phi = v * kPi;
				for (int x = 0; x <= segU; ++x)
				{
					const float uu = (float)x / segU, th = uu * 2.0f * kPi;
					const glm::vec3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
					const glm::vec3 p = n * radius;
					sphere.vertices.insert(sphere.vertices.end(), { p.x, p.y, p.z });
					sphere.normals.insert(sphere.normals.end(),   { n.x, n.y, n.z });
					sphere.uvs.insert(sphere.uvs.end(),           { uu, v });
				}
			}
			for (int y = 0; y < segV; ++y)
				for (int x = 0; x < segU; ++x)
				{
					const uint32_t a = y * (segU + 1) + x, b = a + segU + 1;
					sphere.indices.insert(sphere.indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
				}
		}
		const HE::UUID meshId = contentManager().registerStaticMesh(std::move(sphere));

		auto& reg = m_editorWorld->registry();
		auto  e   = m_editorWorld->createEntity("MatTestSphere");
		// Camera forward from public yaw/pitch (EditorCamera::forward is private) — same
		// convention: yaw=0,pitch=0 looks down -Z; +pitch up; +yaw right.
		const float cp = std::cos(m_editorCamera.pitch()), sp = std::sin(m_editorCamera.pitch());
		const float cy = std::cos(m_editorCamera.yaw()),   sy = std::sin(m_editorCamera.yaw());
		const glm::vec3 camFwd(cp * sy, sp, -cp * cy);
		TransformComponent tc;
		tc.position = m_editorCamera.position() + camFwd * 8.0f;
		reg.emplace<TransformComponent>(e, tc);
		reg.emplace<MeshComponent>(e, MeshComponent{ meshId });
		MaterialComponent mc{ matId };
		// Witness the PER-ENTITY override path (HE_DUMP_ENTITYPARAM="Name,r,g,b"): the
		// shared material is untouched — the value rides on this entity's component and
		// is merged by the extractor into the DrawCall's HeParams block.
		if (const char* ep = std::getenv("HE_DUMP_ENTITYPARAM"); ep && *ep)
		{
			std::string s(ep); const size_t c0 = s.find(',');
			if (c0 != std::string::npos)
			{
				MaterialParamOverride ov; ov.name = s.substr(0, c0);
				std::sscanf(s.c_str() + c0 + 1, "%f,%f,%f", &ov.value[0], &ov.value[1], &ov.value[2]);
				mc.paramOverrides.push_back(ov);
				HE_LOG_INFO(Editor, "%s",
					("EditorApplication: HE_DUMP_ENTITYPARAM override '" + ov.name + "' on entity").c_str());
			}
		}
		reg.emplace<MaterialComponent>(e, mc);
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_MATERIALTEST sphere with custom-shader material added");

		// Shadow-reception witness (HE_DUMP_MATOCCLUDER=1): park a flat default-cube
		// slab directly ABOVE the test sphere. With a high sun, the sphere — shaded
		// by its GRAPH material (heLitP) — must visibly darken versus a run without
		// this flag, proving graph materials receive occlusion (GI mask when GI is
		// on, CSM fallback when it is off). Same A/B idea as the sky knobs.
		if (const char* oc = std::getenv("HE_DUMP_MATOCCLUDER"); oc && *oc)
		{
			auto occ = m_editorWorld->createEntity("MatTestOccluder");
			TransformComponent otc;
			otc.position = tc.position + glm::vec3(3.0f, 6.0f, 0.0f);
			otc.scale    = glm::vec3(12.0f, 0.25f, 12.0f);
			reg.emplace<TransformComponent>(occ, otc);
			reg.emplace<MeshComponent>(occ, MeshComponent{ HE::kDefaultCubeMeshId });
			// Control receiver: an identical sphere WITHOUT the graph material —
			// the built-in PBR path. If this one darkens under the slab and the
			// graph sphere does not, the defect is in the material preamble; if
			// neither darkens, the GI mask / CSM itself is wrong in this scene.
			auto ctl = m_editorWorld->createEntity("MatTestBuiltinSphere");
			TransformComponent btc = tc;
			btc.position = tc.position + glm::vec3(6.0f, 0.0f, 0.0f);
			reg.emplace<TransformComponent>(ctl, btc);
			reg.emplace<MeshComponent>(ctl, MeshComponent{ meshId });
			HE_LOG_INFO(Editor, "%s",
				"EditorApplication: HE_DUMP_MATOCCLUDER slab + built-in control sphere added");
		}

		// Colour-bleed witness (HE_DUMP_GIBLEED=1|2): a tall cube RIGHT next to
		// the test sphere — saturated red (1) vs neutral grey (2). With GI on,
		// the multi-bounce probe feedback must tint the sphere's cube-facing
		// side red in the "1" shot; the "2" shot is the geometry-identical
		// control, so the A/B diff isolates pure bounce colour.
		if (const char* bl = std::getenv("HE_DUMP_GIBLEED"); bl && *bl)
		{
			MaterialAsset wallMat;
			wallMat.type = HE::AssetType::Material;
			wallMat.name = "GIBleedWall";
			// "1"/"2": red/grey floor under the BUILT-IN receiver sphere.
			// "3"/"4": the same pair, but the floor sits under the GRAPH-material
			// sphere — the only way to measure whether heLitP itself consumes the
			// probe field (it does since the DDGI port; before that the graph
			// sphere was deliberately excluded from this witness).
			const std::string blMode(bl);
			const bool red      = (blMode != "2" && blMode != "4");
			const bool underMat = (blMode == "3" || blMode == "4");
			const float bleedX  = underMat ? 0.0f : 6.0f;
			wallMat.baseColor[0] = red ? 1.0f : 0.5f;
			wallMat.baseColor[1] = red ? 0.05f : 0.5f;
			wallMat.baseColor[2] = red ? 0.05f : 0.5f;
			const HE::UUID wallMatId = contentManager().registerMaterial(std::move(wallMat));
			// A FLOOR slab, not a wall: at the noon sun of the standard capture a
			// vertical wall's sphere-facing side has ndl≈0 — nothing to reflect.
			// A fully sunlit red floor bounces onto the sphere's underside.
			auto wall = m_editorWorld->createEntity("GIBleedWall");
			TransformComponent wtc;
			wtc.position = tc.position + glm::vec3(bleedX, -4.5f, 0.0f);
			wtc.scale    = glm::vec3(12.0f, 0.5f, 12.0f);
			reg.emplace<TransformComponent>(wall, wtc);
			reg.emplace<MeshComponent>(wall, MeshComponent{ HE::kDefaultCubeMeshId });
			reg.emplace<MaterialComponent>(wall, MaterialComponent{ wallMatId });
			// Built-in receiver sphere next to the wall — kept as the reference
			// surface for the "1"/"2" pair. In the "3"/"4" pair the floor is under
			// the GRAPH sphere instead and the receiver is omitted, so nothing
			// else can contribute bounce to the measured region.
			if (!underMat)
			{
				auto rcv = m_editorWorld->createEntity("GIBleedReceiver");
				TransformComponent rtc = tc;
				rtc.position = tc.position + glm::vec3(6.0f, 0.0f, 0.0f);
				reg.emplace<TransformComponent>(rcv, rtc);
				reg.emplace<MeshComponent>(rcv, MeshComponent{ meshId });
			}
			HE_LOG_INFO(Editor, "%s", red
				? "EditorApplication: HE_DUMP_GIBLEED red wall added"
				: "EditorApplication: HE_DUMP_GIBLEED grey control wall added");
		}
	}

	// ── SSR witness (HE_DUMP_SSRTEST=1): a mirror floor (metallic 1, roughness
	// 0.05) with a red cube standing on it. With SSR on (deferred tile path)
	// the floor must show the cube's reflection; the SSR=0 control shows only
	// the sky cubemap — the A/B diff isolates exactly the reflected pixels.
	if (const char* sw = std::getenv("HE_DUMP_SSRTEST"); sw && *sw && m_editorWorld)
	{
		auto& reg = m_editorWorld->registry();
		MaterialAsset mirror;
		mirror.type = HE::AssetType::Material;
		mirror.name = "SSRMirrorFloor";
		mirror.baseColor[0] = 0.9f; mirror.baseColor[1] = 0.9f; mirror.baseColor[2] = 0.9f;
		mirror.metallic  = 1.0f;
		mirror.roughness = 0.05f;
		// HE_DUMP_SSRTESTROUGH: override the floor roughness (0..1) so the High
		// tier's glossy lerp (sharp vs wide-blur) is visible in a headless A/B.
		if (const char* fr = std::getenv("HE_DUMP_SSRTESTROUGH"); fr && *fr)
			mirror.roughness = std::clamp(static_cast<float>(std::atof(fr)), 0.0f, 1.0f);
		auto floorE = m_editorWorld->createEntity("SSRFloor");
		TransformComponent ftc;
		ftc.position = glm::vec3(0.0f, -0.1f, -8.0f);
		ftc.scale    = glm::vec3(30.0f, 0.2f, 30.0f);
		reg.emplace<TransformComponent>(floorE, ftc);
		reg.emplace<MeshComponent>(floorE, MeshComponent{ HE::kDefaultCubeMeshId });
		reg.emplace<MaterialComponent>(floorE,
			MaterialComponent{ contentManager().registerMaterial(std::move(mirror)) });

		MaterialAsset red;
		red.type = HE::AssetType::Material;
		red.name = "SSRRedCube";
		red.baseColor[0] = 1.0f; red.baseColor[1] = 0.1f; red.baseColor[2] = 0.1f;
		red.roughness = 0.6f;
		auto cubeE = m_editorWorld->createEntity("SSRCube");
		TransformComponent ctc;
		ctc.position = glm::vec3(0.0f, 1.5f, -8.0f);
		ctc.scale    = glm::vec3(1.5f);
		reg.emplace<TransformComponent>(cubeE, ctc);
		reg.emplace<MeshComponent>(cubeE, MeshComponent{ HE::kDefaultCubeMeshId });
		reg.emplace<MaterialComponent>(cubeE,
			MaterialComponent{ contentManager().registerMaterial(std::move(red)) });
		// HE_DUMP_SSRTESTWALL=1: additionally a camera-FACING mirror wall with a
		// bright cube standing beside it. Its reflection rays run lateral /
		// mildly back toward the camera — the case the old forward-only facing
		// gate silently killed (mirror walls never reflected their neighbours).
		if (const char* ww = std::getenv("HE_DUMP_SSRTESTWALL"); ww && *ww)
		{
			MaterialAsset wall;
			wall.type = HE::AssetType::Material;
			wall.name = "SSRMirrorWall";
			wall.baseColor[0] = 0.9f; wall.baseColor[1] = 0.9f; wall.baseColor[2] = 0.9f;
			wall.metallic  = 1.0f;
			wall.roughness = 0.05f;
			auto wallE = m_editorWorld->createEntity("SSRWall");
			TransformComponent wtc;
			wtc.position = glm::vec3(-1.5f, 3.0f, -12.0f);
			wtc.scale    = glm::vec3(8.0f, 6.0f, 0.4f);
			reg.emplace<TransformComponent>(wallE, wtc);
			reg.emplace<MeshComponent>(wallE, MeshComponent{ HE::kDefaultCubeMeshId });
			reg.emplace<MaterialComponent>(wallE,
				MaterialComponent{ contentManager().registerMaterial(std::move(wall)) });

			MaterialAsset side;
			side.type = HE::AssetType::Material;
			side.name = "SSRWallSideCube";
			side.baseColor[0] = 1.0f; side.baseColor[1] = 0.15f; side.baseColor[2] = 0.1f;
			side.roughness = 0.5f;
			auto sideE = m_editorWorld->createEntity("SSRWallSideCube");
			TransformComponent stc;
			stc.position = glm::vec3(4.5f, 1.5f, -7.0f); // beside + in front of the wall
			stc.scale    = glm::vec3(2.0f);
			reg.emplace<TransformComponent>(sideE, stc);
			reg.emplace<MeshComponent>(sideE, MeshComponent{ HE::kDefaultCubeMeshId });
			reg.emplace<MaterialComponent>(sideE,
				MaterialComponent{ contentManager().registerMaterial(std::move(side)) });
		}
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_SSRTEST witness scene added");
	}

	// ── GI-reflections witness (HE_DUMP_GIREFLTEST=1): a mirror floor with a
	// GRAPH-material cube (ConstColor → BaseColor) and an emissive graph cube
	// (ConstColor → Emissive) standing on it. The ray-traced reflection must
	// show the green cube GREEN and the emissive cube glowing red — exactly the
	// two hit-shading terms giInstanceShading approximates via
	// matGraphApproxSurface. The SSR witness's PLAIN materials never exercised
	// them (their colour lives in MaterialAsset::baseColor, not in a graph).
	if (const char* gw = std::getenv("HE_DUMP_GIREFLTEST"); gw && *gw && m_editorWorld)
	{
		auto& reg = m_editorWorld->registry();
		auto addCube = [&](const char* name, HE::UUID matId,
		                   glm::vec3 pos, glm::vec3 scale)
		{
			auto e = m_editorWorld->createEntity(name);
			TransformComponent tc;
			tc.position = pos;
			tc.scale    = scale;
			reg.emplace<TransformComponent>(e, tc);
			reg.emplace<MeshComponent>(e, MeshComponent{ HE::kDefaultCubeMeshId });
			reg.emplace<MaterialComponent>(e, MaterialComponent{ matId });
		};

		MaterialAsset mirror;
		mirror.type = HE::AssetType::Material;
		mirror.name = "GIReflMirrorFloor";
		mirror.baseColor[0] = 0.9f; mirror.baseColor[1] = 0.9f; mirror.baseColor[2] = 0.9f;
		mirror.metallic  = 1.0f;
		mirror.roughness = 0.05f;
		addCube("GIReflFloor", contentManager().registerMaterial(std::move(mirror)),
		        glm::vec3(0.0f, -0.1f, -8.0f), glm::vec3(30.0f, 0.2f, 30.0f));

		MaterialAsset green;
		green.type = HE::AssetType::Material;
		green.name = "GIReflGraphGreen";
		{
			HE::MaterialGraph g = HE::MaterialGraph::makeDefault(); // Output + ConstColor→BaseColor
			for (auto& n : g.nodes)
				if (n.type == HE::MatNodeType::ConstColor)
				{ n.p[0] = 0.05f; n.p[1] = 0.85f; n.p[2] = 0.1f; }
			green.nodeGraphJson = HE::materialGraphToJson(g);
		}
		const HE::UUID greenId = contentManager().registerMaterial(std::move(green));
		contentManager().regenerateMaterialFromGraph(greenId); // codegen + approx fold
		addCube("GIReflGraphCube", greenId,
		        glm::vec3(-2.5f, 1.5f, -8.0f), glm::vec3(1.5f));

		MaterialAsset glow;
		glow.type = HE::AssetType::Material;
		glow.name = "GIReflEmissive";
		{
			HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
			for (auto& n : g.nodes)               // near-black base: the glow must
				if (n.type == HE::MatNodeType::ConstColor) // come from the emissive term
				{ n.p[0] = 0.02f; n.p[1] = 0.02f; n.p[2] = 0.02f; }
			int outId = 0;
			for (auto& n : g.nodes)
				if (n.type == HE::MatNodeType::Output) { outId = n.id; break; }
			const int em = g.addNode(HE::MatNodeType::ConstColor, 0.0f, 200.0f);
			if (HE::MatGraphNode* n = g.findNode(em))
			{ n->p[0] = 3.0f; n->p[1] = 0.25f; n->p[2] = 0.25f; } // HDR red
			g.connect(em, 0, outId, HE::kMatOutputEmissivePin);
			glow.nodeGraphJson = HE::materialGraphToJson(g);
		}
		const HE::UUID glowId = contentManager().registerMaterial(std::move(glow));
		contentManager().regenerateMaterialFromGraph(glowId);
		addCube("GIReflEmissiveCube", glowId,
		        glm::vec3(2.5f, 1.5f, -8.0f), glm::vec3(1.5f));
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_GIREFLTEST witness scene added");
	}

	// ── Decal witness (HE_DUMP_DECALTEST=1): a grey floor slab with a red decal
	// projector box over its centre. In the deferred (tile) path the floor must
	// show a red patch exactly under the box; forward ignores decals (v1) — the
	// A/B diff isolates the projected pixels.
	if (const char* dt = std::getenv("HE_DUMP_DECALTEST"); dt && *dt && m_editorWorld)
	{
		auto& reg = m_editorWorld->registry();
		auto floorE = m_editorWorld->createEntity("DecalFloor");
		TransformComponent ftc;
		ftc.position = glm::vec3(0.0f, -0.1f, -8.0f);
		ftc.scale    = glm::vec3(30.0f, 0.2f, 30.0f);
		reg.emplace<TransformComponent>(floorE, ftc);
		reg.emplace<MeshComponent>(floorE, MeshComponent{ HE::kDefaultCubeMeshId });

		auto decalE = m_editorWorld->createEntity("DecalProjector");
		TransformComponent dtc;
		dtc.position = glm::vec3(0.0f, 0.0f, -8.0f);
		dtc.scale    = glm::vec3(5.0f, 2.0f, 5.0f);
		reg.emplace<TransformComponent>(decalE, dtc);
		DecalComponent dc;
		dc.color = glm::vec4(1.0f, 0.1f, 0.1f, 0.85f);
		reg.emplace<DecalComponent>(decalE, dc);
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_DECALTEST witness scene added");
	}

	// ── Local-light shadow witness (HE_DUMP_LOCALSHADOW=point|spot): a floor
	// slab + caster cube + ONE shadow-casting local light. Shot at midnight
	// (TOD=0) the local light dominates: the cube must throw a visible shadow
	// onto the floor away from the light. Before the local shadow maps this
	// floor stayed uniformly lit (local lights shone through geometry).
	if (const char* ls = std::getenv("HE_DUMP_LOCALSHADOW"); ls && *ls && m_editorWorld)
	{
		auto& reg = m_editorWorld->registry();
		// Isolate the witness: pre-existing scene lights lose their shadow flag so
		// the atlas holds ONLY the witness light's layers (deterministic layer 0).
		for (auto [e, plc] : reg.view<LightComponent>().each())
			plc.castsShadow = false;
		auto floorE = m_editorWorld->createEntity("LocalShadowFloor");
		TransformComponent ftc;
		ftc.position = glm::vec3(0.0f, 199.0f, -8.0f); // high above any loaded scene content
		ftc.scale    = glm::vec3(24.0f, 0.25f, 24.0f);
		reg.emplace<TransformComponent>(floorE, ftc);
		reg.emplace<MeshComponent>(floorE, MeshComponent{ HE::kDefaultCubeMeshId });
		// "...mat" variants (pointmat/spotmat/+off): the floor gets a NODE-GRAPH
		// material (custom shader → heLitP path), witnessing that CUSTOM materials
		// receive local-light shadows too — not just the built-in PBR pipeline.
		if (std::string(ls).find("mat") != std::string::npos)
		{
			MaterialAsset fm;
			fm.type = HE::AssetType::Material;
			fm.name = "LocalShadowFloorMat";
			HE::MaterialGraph g;
			const int out = g.addNode(HE::MatNodeType::Output);
			const int col = g.addNode(HE::MatNodeType::ConstColor);
			g.findNode(col)->p[0] = 0.8f; g.findNode(col)->p[1] = 0.8f; g.findNode(col)->p[2] = 0.8f;
			g.connect(col, 0, out, 0); // BaseColor → lit output (heLitP)
			fm.nodeGraphJson = HE::materialGraphToJson(g);
			const HE::MatShaderGen gen = HE::generateFragment(g);
			fm.customShaderFragGlsl = gen.glsl;
			fm.customShaderGBufGlsl = gen.glslGBuffer;
			fm.customShaderVertGlsl = gen.vertexBody;
			fm.blendMode            = gen.blendMode;
			reg.emplace<MaterialComponent>(floorE,
				MaterialComponent{ contentManager().registerMaterial(std::move(fm)) });
		}

		auto caster = m_editorWorld->createEntity("LocalShadowCaster");
		TransformComponent ctc;
		ctc.position = glm::vec3(0.0f, 200.25f, -8.0f);
		reg.emplace<TransformComponent>(caster, ctc);
		reg.emplace<MeshComponent>(caster, MeshComponent{ HE::kDefaultCubeMeshId });

		auto lightE = m_editorWorld->createEntity("LocalShadowLight");
		TransformComponent ltc;
		LightComponent lc;
		// "...off" control variant (pointoff/spotoff): identical scene WITHOUT the
		// shadow flag — the A/B image diff isolates exactly the shadowed pixels.
		std::string lsMode(ls);
		lc.castsShadow = lsMode.find("off") == std::string::npos;
		lc.intensity   = 8.0f;
		lc.range       = 40.0f;
		if (lsMode.rfind("spot", 0) == 0)
		{
			lc.type      = HE::LightType::Spot;
			lc.spotAngle = 70.0f;
			// Off to the side so the cast shadow lands NEXT to the cube in screen
			// space (on-axis it hides exactly behind the caster from this camera).
			ltc.position = glm::vec3(-3.0f, 206.0f, -3.0f);
			ltc.rotation = glm::vec3(-55.0f, 0.0f, 0.0f); // aim down-forward
		}
		else
		{
			lc.type      = HE::LightType::Point;
			ltc.position = glm::vec3(2.5f, 202.0f, -5.0f);
		}
		reg.emplace<TransformComponent>(lightE, ltc);
		reg.emplace<LightComponent>(lightE, lc);
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_LOCALSHADOW witness scene added");
	}

	// ── Landscape layer-blend witness (HE_DUMP_LANDSCAPELAYERS=1) ────────────
	// A flat landscape with a THREE-LAYER material (red / green / blue) and a
	// painted weightmap: a green disc in the middle of a red field, with a blue
	// stripe. Proves the whole chain — layer-blend codegen → per-draw weightmap
	// binding → painted weights — lands on pixels, and the three colours make a
	// wrong channel obvious at a glance.
	if (const char* ll = std::getenv("HE_DUMP_LANDSCAPELAYERS"); ll && *ll && m_editorWorld)
	{
		auto& reg = m_editorWorld->registry();

		MaterialAsset lm;
		lm.type = HE::AssetType::Material;
		lm.name = "LayerBlendWitness";
		HE::MaterialGraph g;
		const int out = g.addNode(HE::MatNodeType::Output);
		const int lb  = g.addNode(HE::MatNodeType::LandscapeLayerBlend);
		g.findNode(lb)->s = "Red\nGreen\nBlue";
		const float rgb[3][3] = { { 0.90f, 0.10f, 0.10f },
		                          { 0.10f, 0.85f, 0.15f },
		                          { 0.15f, 0.25f, 0.95f } };
		for (int i = 0; i < 3; ++i)
		{
			const int c = g.addNode(HE::MatNodeType::ConstColor);
			g.findNode(c)->p[0] = rgb[i][0];
			g.findNode(c)->p[1] = rgb[i][1];
			g.findNode(c)->p[2] = rgb[i][2];
			g.connect(c, 0, lb, i);
		}
		g.connect(lb, 0, out, HE::kMatOutputBaseColorPin);
		lm.nodeGraphJson = HE::materialGraphToJson(g);
		const HE::MatShaderGen gen = HE::generateFragment(g);
		lm.customShaderFragGlsl = gen.glsl;
		lm.customShaderGBufGlsl = gen.glslGBuffer;
		lm.customShaderVertGlsl = gen.vertexBody;
		lm.blendMode            = gen.blendMode;
		lm.graphLayerNames      = gen.layerNames;
		const HE::UUID lmId = contentManager().registerMaterial(std::move(lm));

		auto land = m_editorWorld->createEntity("LayerLandscape");
		TransformComponent ltf;
		ltf.position = glm::vec3(0.0f, 300.0f, 0.0f); // clear of any loaded scene
		reg.emplace<TransformComponent>(land, ltf);
		TerrainComponent ltc;
		ltc.sizeX = ltc.sizeZ = 100.0f;
		ltc.resolution = 33;      // already 2ⁿ+1 → no resample
		ltc.heightScale = 0.0f;   // flat: the colours are the whole point
		ltc.seed = 0;
		ltc.weightRes = 128;
		ltc.dirty = true;
		TerrainPaint::ensureWeightmap(ltc);
		TerrainPaint::paint(ltc,   0.0f,  0.0f, /*Green*/1, 22.0f, 6.0f, 1.0f);
		TerrainPaint::paint(ltc, -34.0f, 20.0f, /*Blue*/ 2, 12.0f, 4.0f, 1.0f);
		reg.emplace<TerrainComponent>(land, ltc);
		reg.emplace<MaterialComponent>(land, MaterialComponent{ lmId });
		// The headless dump renders from OnInit, BEFORE the main loop's
		// SceneSystems::tick — without this the terrain has no chunk entities yet
		// and there is simply nothing to draw.
		TerrainSystem::updateTerrains(*m_editorWorld, contentManager(), r);
		HE_LOG_INFO(Editor, "%s",
			"EditorApplication: HE_DUMP_LANDSCAPELAYERS witness landscape added");
	}

	pushEnvironment(0.0f); // scene environment from the World entity (no auto-advance)
	r->SetViewportSize(1280, 720);
	// Warm the material pipelines before rendering (mirrors openScene) so the dump
	// exercises the warmed path — the draw loop then hits the cache instead of
	// cross-compiling mid-encoder.
	if (m_editorWorld) r->WarmupMaterials(SceneSystems::collectAssetRefs(*m_editorWorld));

	// Witness the material-preview offscreen path (HE_DUMP_PREVIEW + HE_PREVIEW_DUMP):
	// render the test material's preview sphere and let the backend dump it.
	if (const char* pv = std::getenv("HE_DUMP_PREVIEW"); pv && *pv && s_matTestId != HE::UUID{})
	{
		// HE_DUMP_PREVIEW=1 → sphere (default); =2 cube, =3 plane (the editor's shape combo).
		const int shape = std::clamp(std::atoi(pv) - 1, 0, 2);
		r->RenderMaterialPreview(contentManager(), s_matTestId, 512, 0.6f, 0.35f, 3.1f, shape);
		// Stress the property-change→re-preview path (repro for the side-panel crash):
		// mutate the material's shader source + params like an editor edit would, then
		// re-preview. HE_DUMP_PREVIEW_STRESS=N repeats N times.
		if (const char* sp = std::getenv("HE_DUMP_PREVIEW_STRESS"); sp && *sp)
		{
			const int reps = std::max(1, std::atoi(sp));
			for (int k = 0; k < reps; ++k)
			{
				if (MaterialAsset* m = contentManager().getMaterialMutable(s_matTestId))
				{
					// Rebuild the graph's shader with a changed constant → new source hash
					// (forces a program/pipeline rebuild), and resize the preview target.
					HE::MaterialGraph g;
					if (!m->nodeGraphJson.empty()) HE::materialGraphFromJson(m->nodeGraphJson, g);
					const HE::MatShaderGen gen = HE::generateFragment(g);
					m->customShaderFragGlsl = gen.glsl + "\n// v" + std::to_string(k);
					m->shaderParamData.clear();
					for (const auto& slot : gen.params)
						m->shaderParamData.insert(m->shaderParamData.end(), slot.value, slot.value + 4);
				}
				r->InvalidateMaterial(s_matTestId);
				r->RenderMaterialPreview(contentManager(), s_matTestId, 200 + k * 16, 0.6f + k * 0.1f, 0.35f, 3.1f);
			}
			HE_LOG_INFO(Editor, "%s", "EditorApplication: preview stress loop done");
		}
	}
	// Witness the Content-Browser thumbnail path (HE_DUMP_THUMB=<dir>): render the
	// material and static-mesh thumbnails through IRenderer::RenderAssetThumbnail —
	// the same call the asset grid makes — and write each as a PPM. Written from the
	// returned PIXELS, not from a GPU handle, so it proves the whole render →
	// readback → "what the tile will contain" chain, alpha included: the transparent
	// background is composited over a checkerboard, so an opaque-background
	// regression is visible rather than invisible.
	if (const char* tb = std::getenv("HE_DUMP_THUMB"); tb && *tb)
	{
		const std::filesystem::path dir(tb);
		std::error_code tec;
		std::filesystem::create_directories(dir, tec);
		auto dumpThumb = [&](const char* name, ThumbnailKind kind, const HE::UUID& id)
		{
			if (id == HE::UUID{}) return;
			std::vector<uint8_t> px;
			if (!r->RenderAssetThumbnail(contentManager(), kind, id, 128, px))
			{
				HE_LOG_WARN(Editor, "%s",
					(std::string("EditorApplication: thumbnail witness '") + name + "' produced nothing").c_str());
				return;
			}
			const int S = 128;
			std::ofstream f((dir / (std::string(name) + ".ppm")).string(), std::ios::binary);
			if (!f) return;
			f << "P6\n" << S << " " << S << "\n255\n";
			for (int y = 0; y < S; ++y)
				for (int x = 0; x < S; ++x)
				{
					const uint8_t* p = &px[(static_cast<size_t>(y) * S + x) * 4];
					const float a = p[3] / 255.0f;
					const uint8_t bg = ((x / 16 + y / 16) & 1) ? 90 : 150; // checkerboard
					for (int c = 0; c < 3; ++c)
					{
						const uint8_t v = static_cast<uint8_t>(p[c] * a + bg * (1.0f - a));
						f.write(reinterpret_cast<const char*>(&v), 1);
					}
				}
			HE_LOG_INFO(Editor, "%s",
				(std::string("EditorApplication: thumbnail witness wrote ") + name + ".ppm").c_str());
		};
		dumpThumb("material", ThumbnailKind::Material,   s_matTestId);
		dumpThumb("mesh",     ThumbnailKind::StaticMesh, HE::kDefaultCubeMeshId);
		// A texture tile is produced on the CPU, so it bypasses dumpThumb's
		// renderer call entirely. Uses the editor's own logo because it has real
		// alpha AND a non-square aspect — the two things the tile has to handle
		// (checkerboard behind transparency, letterbox instead of squash).
		{
			const char* bp = SDL_GetBasePath();
			const std::string logo = std::string(bp ? bp : "") + "Images/HC_Logo.png";
			int tw = 0, th = 0, tch = 0;
			if (unsigned char* px = stbi_load(logo.c_str(), &tw, &th, &tch, 4))
			{
				TextureAsset ta;
				ta.type = HE::AssetType::Texture;
				ta.name = "__thumbWitnessTex";
				ta.width = (uint32_t)tw; ta.height = (uint32_t)th; ta.channels = 4;
				ta.data.assign(px, px + (size_t)tw * th * 4);
				stbi_image_free(px);
				const HE::UUID texId = contentManager().registerTexture(std::move(ta));
				AssetThumbnailCache::setContext(r, &contentManager(), dir.string());
				std::vector<uint8_t> tp;
				if (AssetThumbnailCache::textureThumbnail(texId, tp))
				{
					const int TS = (int)AssetThumbnailCache::thumbnailSize();
					if (std::ofstream f((dir / "texture.ppm").string(), std::ios::binary); f)
					{
						f << "P6\n" << TS << " " << TS << "\n255\n";
						for (int i = 0; i < TS * TS; ++i)
							f.write(reinterpret_cast<const char*>(&tp[(size_t)i * 4]), 3);
					}
					HE_LOG_INFO(Editor, "%s",
						"EditorApplication: thumbnail witness wrote texture.ppm");
				}
				AssetThumbnailCache::setContext(nullptr, nullptr, "");
			}
		}
		// A widget tile lays the tree out and draws it through the UI pass. A
		// freshly created widget is an EMPTY tree, so the witness authors a small
		// one — a panel with a caption — otherwise there would be nothing to see
		// and "no quads" would look the same as "broken".
		{
			HE::UIWidgetTree tree;
			tree.canvasWidth = tree.canvasHeight = 512.0f;
			const int panelId = tree.add(HE::UIWidgetType::Panel);
			if (HE::UIElement* pe = tree.find(panelId))
			{
				pe->posX = 256.0f; pe->posY = 256.0f;
				pe->sizeX = 400.0f; pe->sizeY = 260.0f;
				pe->anchor = 0;
			}
			const int textId = tree.add(HE::UIWidgetType::Text);
			if (HE::UIElement* te = tree.find(textId))
			{
				te->parentId = panelId;
				te->posX = 200.0f; te->posY = 130.0f;
				te->sizeX = 340.0f; te->sizeY = 60.0f;
			}
			UIWidgetAsset wa;
			wa.type = HE::AssetType::Widget;
			wa.name = "__thumbWitnessWidget";
			wa.path = "__thumbWitnessWidget.hasset";
			wa.treeJson = HE::uiWidgetTreeToJson(tree);
			if (contentManager().saveAsset(wa))
			{
				AssetThumbnailCache::setContext(r, &contentManager(), dir.string());
				std::vector<uint8_t> wp;
				if (AssetThumbnailCache::widgetThumbnail(wa.path, wp))
				{
					const int TS = (int)AssetThumbnailCache::thumbnailSize();
					if (std::ofstream f((dir / "widget.ppm").string(), std::ios::binary); f)
					{
						f << "P6\n" << TS << " " << TS << "\n255\n";
						for (int i = 0; i < TS * TS; ++i)
						{
							const float a = wp[(size_t)i * 4 + 3] / 255.0f;
							const uint8_t bg = (((i % TS) / 16 + (i / TS) / 16) & 1) ? 90 : 150;
							for (int c = 0; c < 3; ++c)
							{
								const uint8_t v = (uint8_t)(wp[(size_t)i * 4 + c] * a + bg * (1.0f - a));
								f.write(reinterpret_cast<const char*>(&v), 1);
							}
						}
					}
					HE_LOG_INFO(Editor, "%s",
						"EditorApplication: thumbnail witness wrote widget.ppm");
				}
				else
					HE_LOG_WARN(Editor, "%s",
						"EditorApplication: thumbnail witness produced no widget tile");
				AssetThumbnailCache::setContext(nullptr, nullptr, "");
				std::error_code wrc;
				std::filesystem::remove(
					std::filesystem::path(contentManager().contentRoot()) / wa.path, wrc);
			}
		}
		// A particle tile steps a real pool and renders it through the dedicated
		// particle thumbnail path — worth witnessing because "did the simulation
		// actually produce particles" cannot be seen from the code.
		{
			ParticleGraphAsset pg;
			pg.type = HE::AssetType::ParticleSystem;
			pg.name = "__thumbWitnessParticles";
			pg.path = "__thumbWitnessParticles.hasset";
			pg.nodeGraphJson = HE::particleGraphToJson(HE::ParticleGraph::makeDefault());
			HE::UUID pid{};
			if (contentManager().saveAsset(pg)) pid = contentManager().loadAsset(pg.path);
			AssetThumbnailCache::setContext(r, &contentManager(), dir.string());
			std::vector<uint8_t> pp;
			if (pid != HE::UUID{} && AssetThumbnailCache::particleThumbnail(pid, pp))
			{
				const int TS = (int)AssetThumbnailCache::thumbnailSize();
				if (std::ofstream f((dir / "particles.ppm").string(), std::ios::binary); f)
				{
					f << "P6\n" << TS << " " << TS << "\n255\n";
					for (int i = 0; i < TS * TS; ++i)
					{
						const float a = pp[(size_t)i * 4 + 3] / 255.0f;
						const uint8_t bg = (((i % TS) / 16 + (i / TS) / 16) & 1) ? 90 : 150;
						for (int c = 0; c < 3; ++c)
						{
							const uint8_t v = (uint8_t)(pp[(size_t)i * 4 + c] * a + bg * (1.0f - a));
							f.write(reinterpret_cast<const char*>(&v), 1);
						}
					}
				}
				HE_LOG_INFO(Editor, "%s",
					"EditorApplication: thumbnail witness wrote particles.ppm");
			}
			else
				HE_LOG_WARN(Editor, "%s",
					"EditorApplication: thumbnail witness produced no particle tile");
			AssetThumbnailCache::setContext(nullptr, nullptr, "");
			std::error_code prc;
			std::filesystem::remove(
				std::filesystem::path(contentManager().contentRoot()) / pg.path, prc);
		}
		// A font tile is baked through UIFontCache, so it too skips the renderer.
		{
			const char* bp2 = SDL_GetBasePath();
			const std::string ttf = std::string(bp2 ? bp2 : "") + "Fonts/Roboto_Condensed-Bold.ttf";
			if (std::ifstream tf(ttf, std::ios::binary); tf)
			{
				FontAsset fo;
				fo.type = HE::AssetType::Font;
				fo.name = "__thumbWitnessFont";
				fo.path = "__thumbWitnessFont.hasset";
				fo.fontData.assign(std::istreambuf_iterator<char>(tf), std::istreambuf_iterator<char>());
				HE::UUID fid{};
				if (contentManager().saveAsset(fo)) fid = contentManager().loadAsset(fo.path);
				AssetThumbnailCache::setContext(r, &contentManager(), dir.string());
				std::vector<uint8_t> fp;
				if (AssetThumbnailCache::fontThumbnail(fid, fp))
				{
					const int TS = (int)AssetThumbnailCache::thumbnailSize();
					if (std::ofstream f((dir / "font.ppm").string(), std::ios::binary); f)
					{
						f << "P6\n" << TS << " " << TS << "\n255\n";
						for (int i = 0; i < TS * TS; ++i)
						{
							// Composite the coverage over grey — the glyphs are white on
							// transparent, which a plain RGB dump would render invisible.
							const float a = fp[(size_t)i * 4 + 3] / 255.0f;
							const uint8_t v = (uint8_t)(255 * a + 60 * (1.0f - a));
							for (int c = 0; c < 3; ++c) f.write(reinterpret_cast<const char*>(&v), 1);
						}
					}
					HE_LOG_INFO(Editor, "%s",
						"EditorApplication: thumbnail witness wrote font.ppm");
				}
				AssetThumbnailCache::setContext(nullptr, nullptr, "");
				std::error_code frc;
				std::filesystem::remove(
					std::filesystem::path(contentManager().contentRoot()) / fo.path, frc);
			}
		}
		// A material with NO node graph exercises the OTHER material branch — the
		// built-in-PBR fallback, which the interactive preview never reaches (it
		// returns "no preview" there) and which is therefore only visible here.
		{
			MaterialAsset flat;
			flat.type = HE::AssetType::Material;
			flat.name = "__thumbWitnessFlat";
			flat.baseColor[0] = 0.85f; flat.baseColor[1] = 0.35f; flat.baseColor[2] = 0.15f;
			flat.metallic = 0.1f; flat.roughness = 0.35f;
			dumpThumb("material_pbr", ThumbnailKind::Material,
			          contentManager().registerMaterial(std::move(flat)));
		}
		// And a material FUNCTION, which is drawn through the scratch material the
		// thumbnail cache wraps it in. Unit tests cover the wrapping; only a real
		// render shows whether the generated shader actually produces a picture.
		{
			MaterialFunctionAsset fn;
			fn.type = HE::AssetType::MaterialFunction;
			fn.name = "__thumbWitnessFn";
			fn.path = "__thumbWitnessFn.hasset";
			fn.nodeGraphJson = HE::materialGraphToJson(HE::MaterialGraph::makeDefaultFunction());
			if (contentManager().saveAsset(fn))
			{
				AssetThumbnailCache::setContext(r, &contentManager(), dir.string());
				dumpThumb("material_function", ThumbnailKind::Material,
				          AssetThumbnailCache::materialFunctionScratch(fn.path));
				AssetThumbnailCache::setContext(nullptr, nullptr, "");
				std::error_code rc;
				std::filesystem::remove(
					std::filesystem::path(contentManager().contentRoot()) / fn.path, rc);
			}
			else
				HE_LOG_WARN(Editor, "%s",
					"EditorApplication: thumbnail witness could not write the function asset");
		}
	}

	// DIAGNOSTIC (HE_DUMP_GIROTATE): sweep the camera through an orbit before the
	// final static settle below, to directly exercise GI's temporal reprojection
	// under camera rotation (disocclusion). The plain 3x static-render capture
	// this harness otherwise does can NEVER trigger this — every he_shot.py call
	// is a fresh process (no accumulated history) and the camera never moves
	// within a single dump. "<deg>" (one value) sweeps start==end, i.e. a STATIC
	// control at that angle for every frame; "<start>,<end>" sweeps between them
	// — comparing the two final captures isolates whether an in-flight rotation
	// (not just the final angle) changes the result.
	if (const char* rot = std::getenv("HE_DUMP_GIROTATE"); rot && *rot && m_editorWorld)
	{
		float startDeg = 0.0f, endDeg = 0.0f;
		if (std::sscanf(rot, "%f,%f", &startDeg, &endDeg) != 2)
		{ endDeg = static_cast<float>(std::atof(rot)); startDeg = endDeg; }
		const int steps = 12;
		const glm::vec3 pivot(0.0f, 1.0f, 0.0f);
		const float dist = 14.0f;
		const float pitchRad = glm::radians(-20.0f);
		const float cp = std::cos(pitchRad), sp = std::sin(pitchRad);
		for (int i = 0; i <= steps; ++i)
		{
			const float t      = static_cast<float>(i) / static_cast<float>(steps);
			const float yawRad = glm::radians(startDeg + (endDeg - startDeg) * t);
			const glm::vec3 camPos = pivot + glm::vec3(std::sin(yawRad) * cp, sp, std::cos(yawRad) * cp) * dist;
			m_editorCamera.setOrientation(camPos, glm::normalize(pivot - camPos));
			r->SetEditorCamera(m_editorCamera.makeOverride());
			r->Render();
		}
		HE_LOG_INFO(Editor, "%s",
			("EditorApplication: GI rotate diagnostic swept yaw " + std::to_string(startDeg)
			 + "° -> " + std::to_string(endDeg) + "°").c_str());
	}

	// HE_DUMP_FRAMES: settle frames before the capture (default 3). Temporal
	// features (GI-reflection glossy accumulation, probe convergence) need more
	// frames to settle than the default — headless A/Bs raise this.
	int settleFrames = 3;
	if (const char* sf = std::getenv("HE_DUMP_FRAMES"); sf && *sf)
		settleFrames = std::clamp(std::atoi(sf), 1, 240);
	for (int i = 0; i < settleFrames; ++i)
		r->Render();

	std::vector<uint8_t> rgba;
	uint32_t w = 0, h = 0;
	if (r->CaptureViewport(rgba, w, h) && w > 0 && h > 0 && writeBMP(m_dumpPath, rgba, w, h))
		HE_LOG_INFO(Editor, "%s",
			("EditorApplication: frame dumped (" + std::to_string(w) + "x" +
			 std::to_string(h) + ") → " + m_dumpPath).c_str());
	else
		HE_LOG_ERROR(Editor, "%s",
			("EditorApplication: frame dump failed → " + m_dumpPath).c_str());

	m_dumpDone = true;
	if (m_dumpQuit)
	{
		// Ask the main loop to exit on its first iteration (before any swap).
		SDL_Event q;
		q.type = SDL_EVENT_QUIT;
		SDL_PushEvent(&q);
	}
}

void EditorApplication::syncStructuralChanges()
{
	if (!m_editorWorld) return;
	auto& reg = m_editorWorld->registry();

	// Entities the engine generates per machine — terrain chunks regenerate from
	// the TerrainComponent, environment lights belong to the Sky entity — must
	// NOT replicate: every peer makes its own, and sending them would duplicate
	// them on arrival. This is also precisely why network ids exist instead of
	// raw handles: these local entities consume handles independently on each
	// peer, so the allocators are never in lockstep.
	const auto isLocalOnly = [&reg](Entity e) {
		return reg.all_of<EnvironmentLightComponent>(e) ||
		       reg.all_of<TerrainChunkComponent>(e);
	};

	std::unordered_set<Entity> current;
	reg.view<entt::entity>().each([&](auto e) {
		if (!isLocalOnly(e)) current.insert(e);
	});

	// ── Created ──
	for (const Entity e : current)
	{
		if (m_structureKnown.count(e)) continue;

		SceneSerializer serializer;
		const std::vector<std::uint8_t> blob = serializer.serializeSubtree(*m_editorWorld, e);
		if (!blob.empty())
		{
			std::uint32_t parentHandle = 0;
			if (auto* hier = reg.try_get<HierarchyComponent>(e);
			    hier && reg.valid(hier->parent))
			{
				parentHandle = static_cast<std::uint32_t>(entt::to_integral(hier->parent));
			}
			m_collab.publishCreate(
				static_cast<std::uint32_t>(entt::to_integral(e)), parentHandle, blob);
		}
		m_structureKnown.insert(e);
	}

	// ── Destroyed ──
	// Collected first: erasing from the set while iterating it would invalidate
	// the iterator.
	std::vector<Entity> gone;
	for (const Entity e : m_structureKnown)
	{
		if (!current.count(e)) gone.push_back(e);
	}
	for (const Entity e : gone)
	{
		m_collab.publishDestroy(static_cast<std::uint32_t>(entt::to_integral(e)));
		m_structureKnown.erase(e);
	}
}

void EditorApplication::applyAssetBytes(const std::string& relativePath,
                                        const std::vector<std::uint8_t>& bytes)
{
	// Write the bytes exactly as they arrived, then reload so open panels pick
	// the new content up. Writing the file rather than deserializing per asset
	// type is what keeps this uniform across HorizonCode, materials, UI widgets,
	// particle and animator graphs and scenes alike.
	const std::string full = contentManager().resolveSavePath(relativePath);
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(full).parent_path(), ec);
		std::ofstream out(full, std::ios::binary | std::ios::trunc);
		if (!out) return;
		out.write(reinterpret_cast<const char*>(bytes.data()),
		          static_cast<std::streamsize>(bytes.size()));
		if (!out) return;
	}
	contentManager().loadAsset(relativePath);
}

AppContext EditorApplication::makeContext()
{
	m_sdlDialogBridge.pendingDirResult  = &m_pendingDirResult;
	m_sdlDialogBridge.pendingDirReady   = &m_pendingDirReady;
	m_sdlDialogBridge.pendingFileResult = &m_pendingFileResult;
	m_sdlDialogBridge.pendingFileReady  = &m_pendingFileReady;

	return AppContext{
		.imguiReady          = m_imguiReady,
		.quit                = [this]{ Quit(); },
		.toggleProfilerCapture = [this]{ toggleProfilerCapture(); },
		.setVSync              = [this](bool v){ setVSync(v); m_vsync = v; },
		.setMaxFps             = [this](float f){ setMaxFps(f); m_editorConfig.MaxFps = f; },
		.editorConfig        = m_editorConfig,
		.vsync               = m_vsync,
		.backendName         = m_backend_name,
		.backend             = m_backend,
		.globalState         = m_globalState,
		.projectManager      = &m_projectManager,
		.renderer            = renderer(),
		.window              = window(),
		.world               = world(),
		.contentManager      = &contentManager(),
		.gameInstanceGraph   = &m_gameInstanceGraph,
		.commitGameInstance  = [this]{
			m_gameInstance.setGraph(HorizonCode::toJson(m_gameInstanceGraph));
			saveGameInstanceGraph();
		},
		.propScriptEngine    = m_propScriptEngine.get(),
		.editorCamera        = &m_editorCamera,
		.selectedEntity      = m_selectedEntity,
		.isPlaying           = m_isPlaying,
		.playLog             = &m_playLog,
		.playLogMutex        = &m_playLogMutex,
		.playReportOpen      = &m_playReportOpen,
		.setPlayMode         = [this](bool play){ setPlayMode(play); },
		.reportPlayUIPointer = [this](float mx, float my, float vpW, float vpH,
		                              bool down, bool valid)
		{
			m_uiPointerX = mx; m_uiPointerY = my;
			m_uiViewportW = vpW; m_uiViewportH = vpH;
			m_uiPointerDown = down; m_uiPointerValid = valid;
		},
		.currentScenePath    = m_currentScenePath,
		.sceneDirty          = m_undo.revision() != m_savedRevision,
		.exitRequested       = m_exitRequested,
		.saveSceneToPath     = [this](const std::string& p){ saveSceneToPath(p); },
		.openScene           = [this](const std::string& p){ openScene(p); },
		.openSceneAdditive   = [this](const std::string& p){ openSceneAdditive(p); },
		.newScene            = [this]{ newScene(); },
		.undoSys             = &m_undo,
		.undo                = [this]{ if (m_undo.undo()) m_selectedEntity = entt::null; },
		.redo                = [this]{ if (m_undo.redo()) m_selectedEntity = entt::null; },
		.projectLoaded       = m_projectLoaded,
		.contentRefreshPending = m_contentRefreshPending,
		.contentRefreshDone  = m_contentRefreshDone,
		.toolchainProbe      = m_toolchainChecked.load(std::memory_order_acquire)
		                           ? &m_toolchainProbe : nullptr,
		.recheckToolchain    = [this]{ startToolchainProbe(); },
		.startToolchainInstall = [this](bool needCmake, bool needCompiler){ startToolchainInstall(needCmake, needCompiler); },
		.toolchainInstallLog = [this]{ std::lock_guard<std::mutex> lk(m_installLogMutex); return m_installLog; },
		.toolchainInstalling = m_installRunning.load(std::memory_order_acquire),
		.toolchainInstallDone = m_installFinished.load(std::memory_order_acquire),
		.toolchainInstallOk  = m_installFinished.load(std::memory_order_acquire)
		                           && m_installAttempted.load(std::memory_order_acquire)
		                           && m_installExit.load(std::memory_order_acquire) == 0,
		.gitProbe            = m_gitChecked.load(std::memory_order_acquire)
		                           ? &m_gitProbe : nullptr,
		.recheckGit          = [this]{ startGitProbe(); },
		.setGitIdentity      = [this](std::string n, std::string e)
		                           { applyGitIdentity(std::move(n), std::move(e)); },
		.gitIdentityApplying = m_gitIdentityApplying.load(std::memory_order_acquire),
		.git                 = &m_git,
		.frametimeHistory    = m_frametimeHistory,
		.fpsHistorySize      = k_fpsHistorySize,
		.fpsHistoryOffset    = m_fpsHistoryOffset,
		.fpsAccum            = m_fpsAccum,
		.fpsAccumCount       = m_fpsAccumCount,
		.smoothFps           = m_smoothFps,
#ifdef HE_IMGUI_ENABLED
		.tabs                = m_tabs,
		.activeTab           = m_activeTab,
		.fontBody            = m_fontBody,
		.fontSubheading      = m_fontSubheading,
		.fontHeading         = m_fontHeading,
		.codeFont            = m_fontMono,
		.logoTexture         = m_logoTexture,
		.logoW               = m_logoW,
		.logoH               = m_logoH,
		.cbIcons             = {
			m_iconFolder,
			m_iconMaterial,
			m_iconModel2d,
			m_iconModel3d,
			m_iconScript,
			m_iconSound,
			m_iconTexture,
			m_iconScene,
			m_iconMaterialFunction,
			m_iconShader,
			m_iconPrefab,
			m_iconAnimationClip,
			m_iconPropertyAnimClip,
			m_iconWidget,
			m_iconHorizonCodeClass,
			m_iconInputAction,
			m_iconInputMappingContext,
			m_iconParticleSystem,
			m_iconAnimatorStateMachine,
			m_iconFont,
		},
		.toolbarIcons        = {
			m_iconPlay,
			m_iconStop,
			m_iconUndo,
			m_iconRedo,
		},
		.cbTreeWidth         = m_editorConfig.CbTreeWidth,
		.hubSelectedPreset   = m_hubSelectedPreset,
		.hubSelectedLang     = m_hubSelectedLang,
		.hubProjectName      = m_hubProjectName,
		.hubProjectNameSize  = (int)sizeof(m_hubProjectName),
		.hubProjectDir       = m_hubProjectDir,
		.hubProjectDirSize   = (int)sizeof(m_hubProjectDir),
		.hubCreateError      = m_hubCreateError,
		.hubOpenError        = m_hubOpenError,
		.hubRemoveIndex      = m_hubRemoveIndex,
		.hubRemoveRequested  = m_hubRemoveRequested,
		.pendingDirResult    = m_pendingDirResult,
		.pendingDirReady     = m_pendingDirReady,
		.pendingFileResult   = m_pendingFileResult,
		.pendingFileReady    = m_pendingFileReady,
		.dialogBridge        = &m_sdlDialogBridge,
#endif
		.collab              = &m_collab,
		.collabUndo          = &m_collabUndo,
	};
}

// ─── Play-in-editor ───────────────────────────────────────────────────────────
// Play: snapshot the editor world to a temp file (binary). Stop: wipe the
// world and restore the snapshot — any changes made by game systems while
// playing are discarded.
void EditorApplication::setPlayMouseCaptured(bool captured)
{
	const bool wasCaptured = m_playMouseCaptured;
	m_playMouseCaptured = captured;
	// SDL engages relative mode only while the *flagged* window holds keyboard focus
	// (SDL_UpdateRelativeMouseMode). With multi-viewport panels the focused window can
	// be a floating panel's OS window rather than the main one — so flag whichever
	// window actually has focus (updatePlayCameraController re-asserts this per frame
	// in case focus moves while captured).
	SDL_Window* const mainWin = window() ? window()->GetNativeWindow() : nullptr;
	SDL_Window* const focusWin = SDL_GetKeyboardFocus();
	if (captured)
	{
		if (SDL_Window* w = focusWin ? focusWin : mainWin)
		{
			SDL_SetWindowRelativeMouseMode(w, true);
			SDL_HideCursor();
			SDL_GetRelativeMouseState(nullptr, nullptr); // flush stale delta
		}
	}
	else if (wasCaptured) // skip the release work (esp. the warp) if nothing was captured
	{
		// Clear the flag from every window — focus may have wandered across several
		// viewport windows while captured, flagging each via the per-frame re-assert.
		int winCount = 0;
		if (SDL_Window** wins = SDL_GetWindows(&winCount))
		{
			for (int i = 0; i < winCount; ++i)
				SDL_SetWindowRelativeMouseMode(wins[i], false);
			SDL_free(wins);
		}
		SDL_ShowCursor();
		if (SDL_Window* w = focusWin ? focusWin : mainWin)
		{
			// Reappear mid-window instead of wherever the cursor last drifted.
			int ww = 0, wh = 0;
			SDL_GetWindowSize(w, &ww, &wh);
			SDL_WarpMouseInWindow(w, ww * 0.5f, wh * 0.5f);
		}
	}
	// While the game owns the input, stop ImGui from reacting: NoMouse blocks hover/
	// click, NoKeyboard blocks keyboard-nav (Space would otherwise activate the nav-
	// focused Play button), NoMouseCursorChange stops the SDL3 backend from re-showing
	// the hidden cursor every frame (imgui_impl_sdl3 UpdateMouseCursor). Esc restores
	// normal editor input so the UI is clickable again.
	if (m_imguiReady)
	{
		constexpr ImGuiConfigFlags kPlayFlags = ImGuiConfigFlags_NoMouse |
			ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
		ImGuiIO& io = ImGui::GetIO();
		if (captured) io.ConfigFlags |= kPlayFlags;
		else          io.ConfigFlags &= ~kPlayFlags;
	}
}

// Free-fly camera while playing in the editor — mirrors GameApplication so PIE is
// navigable like the packaged game. Drives the scene's main camera (isMain, else the
// first) from raw mouse motion (look) + WASD/QE/Space/Ctrl (move). No-op unless
// playing AND the mouse is captured.
void EditorApplication::updatePlayCameraController(float dt)
{
	if (!m_isPlaying || !m_playMouseCaptured || !m_editorWorld || dt <= 0.0f) return;

	// The focused window is both the one whose relative mode must be re-asserted and
	// the one the cursor is warped back into (see FlyCameraController) — with
	// multi-viewport panels that may be a floating panel's OS window, not the main one.
	SDL_Window* const focusWin = SDL_GetKeyboardFocus();

	HE::FlyCameraController::Config cfg;
	cfg.reassertCapture  = true;   // focus can move between OS windows mid-play
	cfg.runWithoutCamera = true;   // keep feeding the self-diagnostic below
	const auto frame =
		HE::FlyCameraController::update(m_editorWorld->registry(), input(), dt, focusWin, cfg);

	// ── Self-diagnostic (throttled ~once/sec) ──────────────────────────────────────
	// One PIE test should be conclusive: this reports whether a camera is being driven
	// and whether mouse motion is actually reaching us, so a "still doesn't move" report
	// tells us which cause (no camera / input not arriving / extractor) to chase.
	static int   s_diagFrames = 0;
	static float s_diagMotion = 0.0f;
	s_diagMotion += std::abs(frame.dx) + std::abs(frame.dy);
	if (++s_diagFrames >= 60)
	{
		HE_LOG_INFO(Editor, "%s",
			(std::string("PIE camera controller: ")
			+ (frame.camera == entt::null ? "NO camera to drive" : "driving a scene camera")
			+ ", mouse motion (60 frames) = " + std::to_string(s_diagMotion)
			+ (input().IsKeyDown(SDL_SCANCODE_W) ? " [W held]" : "")
			+ (focusWin && SDL_GetWindowRelativeMouseMode(focusWin) ? "" : " [rel-mode OFF]")
			+ (SDL_CursorVisible() ? " [cursor visible]" : "")).c_str());
		s_diagFrames  = 0;
		s_diagMotion  = 0.0f;
	}
}

void EditorApplication::setPlayMode(bool play)
{
	if (play == m_isPlaying || !m_editorWorld)
		return;

	const std::filesystem::path snapshot =
		std::filesystem::temp_directory_path() / "he_play_snapshot.hescene_bin";
	SceneSerializer serializer;

	if (play)
	{
		if (!serializer.save(*m_editorWorld, snapshot, SerializeFormat::Binary))
		{
			HE_LOG_ERROR(Editor, "%s",
				"EditorApplication: play-mode snapshot failed — staying in edit mode");
			return;
		}
		m_isPlaying = true;
		HE::api::time::reset(); // play-relative clock (elapsed/frameCount start at 0)
		// Capture warnings/errors for the post-PIE report.
		{
			std::lock_guard<std::mutex> lk(m_playLogMutex);
			m_playLog.clear();
		}
		m_playReportOpen = false;
		Logger::setSink(&hePlayLogSink, this);
		m_undo.clearHistory(); // edits made while playing are not undoable

		// GameInstance OnInit fires first — before scripts, the level and any
		// widgets — mirroring the packaged game's "before anything loads".
		m_gameInstance.fireInit();

		// PIE needs a camera to *drive* and to render through. Edit-mode scenes are
		// navigated with the editor camera, so many have no CameraComponent at all — the
		// packaged game handles that by adding a default one (GameApplication::OnInit), and
		// PIE must mirror it or the view sits on a fixed fallback and mouse-look/WASD appear
		// to do nothing. Added *after* the snapshot save above, so leaving play mode (clear +
		// restore from snapshot) drops it again. Seeded at the editor camera's current pose so
		// PIE opens on the view you were looking at. Mapping editor yaw/pitch → TransformComponent
		// euler: editor forward = (cp·sy, sp, -cp·cy); TransformComponent forward = quat(radians(rot))·(0,0,-1),
		// which yields rot.x = +pitch, rot.y = -yaw.
		{
			auto& reg = m_editorWorld->registry();
			bool hasCamera = false;
			for (auto e : reg.view<CameraComponent>()) { (void)e; hasCamera = true; break; }
			if (!hasCamera)
			{
				auto camE = m_editorWorld->createEntity("PlayCamera");
				TransformComponent tc;
				tc.position   =  m_editorCamera.position();
				tc.rotation.x =  glm::degrees(m_editorCamera.pitch());
				tc.rotation.y = -glm::degrees(m_editorCamera.yaw());
				reg.emplace<TransformComponent>(camE, tc);
				CameraComponent cc; cc.isMain = true;
				reg.emplace<CameraComponent>(camE, cc);
				HE_LOG_INFO(Editor, "%s",
					"EditorApplication: PIE added a default main camera at the editor view (scene had none)");
			}
		}

		// Initialise physics from the current world state
		m_physicsWorld = std::make_unique<PhysicsWorld>();
		m_physicsWorld->initialize(*m_editorWorld);
		m_physicsAccum = 0.0f;

		// Start audio for sources marked playOnStart
		AudioSystem::playOnStart(*m_editorWorld, m_audioEngine, &contentManager());

		// Initialise script context and start all enabled scripts
		m_scriptContext = std::make_unique<ScriptContext>(*m_editorWorld);
		m_scriptContext->setPhysicsWorld(m_physicsWorld.get());
		m_scriptContext->setContentManager(&contentManager()); // horizon.setMaterialParam
		// Same call the packaged game's startScripts() makes, so PIE and a shipped
		// game bring scripts up identically.
		m_scriptContext->startWorldScripts(contentManager(), m_scriptInstances);

		// The level script's "OnLevelLoaded" fires once, after per-entity
		// scripts have started. Leaving play mode routes through clear(), which
		// fires the matching "OnLevelUnloaded".
		m_editorWorld->fireLevelLoaded();

		// Player controller/character classes + input events, mirroring the
		// packaged game: spawn after the level is up (Construct + BeginPlay),
		// pump Tick/Input.* per frame while playing.
		m_playerHost.begin(m_gameInstance.runtime(), contentManager());

		// horizon.showCursor()/hideCursor(): scripts release/re-grab the PIE
		// mouse capture (visible cursor = UI interaction mode).
		ScriptApi::setCursorHook([this](bool show){ setPlayMouseCaptured(!show); });

		// Capture the mouse so PIE plays like the packaged game (Esc toggles it).
		setPlayMouseCaptured(true);

		HE_LOG_INFO(Editor, "%s", "EditorApplication: entering play mode");
	}
	else
	{
		// Player instances go down first (their Destruct may still reference the
		// GameInstance), then the GameInstance fires OnShutdown while the app
		// runtime is still intact (it lives outside the world, so clear() below
		// doesn't touch it).
		m_playerHost.end();
		m_gameInstance.fireShutdown();

		setPlayMouseCaptured(false); // release the mouse when leaving play mode
		m_editorWorld->clear();
		if (!serializer.load(*m_editorWorld, snapshot, SerializeFormat::Binary))
			HE_LOG_ERROR(Editor, "%s",
				"EditorApplication: play-mode restore failed — world may be empty");
		m_selectedEntity = entt::null;
		m_editorWorld->markHierarchyDirty();
		m_isPlaying = false;
		m_undo.clearHistory();
		// Stop capturing; anything collected pops the post-PIE report window.
		Logger::setSink(nullptr, nullptr);
		{
			std::lock_guard<std::mutex> lk(m_playLogMutex);
			m_playReportOpen = !m_playLog.empty();
		}
		// PIE-loaded zones die with the snapshot restore below — drop the table
		// so stale zone ids don't survive into the next play session.
		HE::api::scene::clearZones();

		// Tear down physics
		m_physicsWorld.reset();
		m_physicsAccum = 0.0f;

		// Tear down scripts
		m_scriptContext.reset();
		m_scriptInstances.clear();
		m_uiInputState = {};
		ScriptApi::setCursorHook(nullptr);
		if (m_widgetTextInputActive)
		{
			if (SDL_Window* w = window() ? window()->GetNativeWindow() : nullptr)
				SDL_StopTextInput(w);
			m_widgetTextInputActive = false;
		}

		// Stop all audio when exiting play mode
		m_audioEngine.stopAll();

		HE_LOG_INFO(Editor, "%s", "EditorApplication: returned to edit mode");
	}
}

// ─── Game Instance (app-wide HorizonCode script) ────────────────────────────────
std::string EditorApplication::gameInstancePath()
{
	std::filesystem::path p = m_projectManager.currentProject().path;
	if (p.empty()) return {};
	if (std::filesystem::is_regular_file(p)) p = p.parent_path();
	return (p / "GameInstance.hcode").string();
}

void EditorApplication::loadGameInstanceGraph()
{
	m_gameInstanceGraph = HorizonCode::Graph{};
	const std::string path = gameInstancePath();
	if (!path.empty())
	{
		std::ifstream f(path);
		if (f)
		{
			const std::string content((std::istreambuf_iterator<char>(f)),
			                          std::istreambuf_iterator<char>());
			HorizonCode::fromJson(content, m_gameInstanceGraph); // broken/absent → empty
		}
	}
	// Register with the app runtime so Get Game Instance resolves and it can run
	// (empty graph → an empty but referenceable GameInstance).
	m_gameInstance.setGraph(HorizonCode::toJson(m_gameInstanceGraph));
}

void EditorApplication::saveGameInstanceGraph()
{
	const std::string path = gameInstancePath();
	if (path.empty()) return;
	std::ofstream f(path, std::ios::trunc);
	if (f) f << HorizonCode::toJson(m_gameInstanceGraph);
}

// ─── Per-project open-tab persistence ───────────────────────────────────────────
void EditorApplication::saveOpenTabs()
{
	if (!m_projectLoaded) return;
	// A cheap signature (paths + active index) so we only write on real changes.
	std::string sig;
	for (const auto& t : m_tabs) sig += t.assetPath + "|";
	sig += std::to_string(m_activeTab);
	if (sig == m_lastTabSig) return;
	m_lastTabSig = sig;

	nlohmann::json arr = nlohmann::json::array();
	for (const auto& t : m_tabs)
		if (!t.assetPath.empty()) // skip the built-in Viewport tab
			arr.push_back({ { "label", t.label }, { "path", t.assetPath } });
	const nlohmann::json state = { { "tabs", arr }, { "active", m_activeTab } };

	if (m_globalState)
	{
		// "replace" handler: dump() throws on invalid UTF-8 in a tab label/path,
		// which would abort the editor; substitute U+FFFD rather than crash.
		m_globalState->setCustomConfigEntry("openTabs:" + m_projectManager.currentProject().path,
			state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
		m_globalState->writeConfig();
	}
}

void EditorApplication::restoreOpenTabs()
{
	if (!m_globalState) return;
	const std::string raw = m_globalState->getCustomConfigString(
		"openTabs:" + m_projectManager.currentProject().path, "");
	if (raw.empty()) return;
	nlohmann::json state = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
	if (state.is_discarded() || !state.is_object()) return;

	// Keep the Viewport tab (empty path); replace any prior asset tabs.
	if (m_tabs.empty()) m_tabs.push_back({ "Viewport", "", false, true });
	m_tabs.erase(std::remove_if(m_tabs.begin(), m_tabs.end(),
		[](const AppContext::EditorTab& t){ return !t.assetPath.empty(); }), m_tabs.end());

	for (const auto& t : state.value("tabs", nlohmann::json::array()))
	{
		const std::string path = t.value("path", std::string());
		// Restore virtual tabs (":…") and assets that still exist on disk.
		if (path.empty()) continue;
		if (path[0] != ':' && !std::filesystem::exists(path)) continue;
		m_tabs.push_back({ t.value("label", std::string()), path, true, true });
	}
	const int active = state.value("active", 0);
	m_activeTab = (active >= 0 && active < (int)m_tabs.size()) ? active : 0;
}

// ─── Scene file management ──────────────────────────────────────────────────────
void EditorApplication::saveSceneToPath(const std::string& path)
{
	if (!m_editorWorld || path.empty()) return;

	SceneSerializer serializer;
	if (serializer.save(*m_editorWorld, path, SerializeFormat::JSON))
	{
		m_currentScenePath = path;
		m_savedRevision    = m_undo.revision(); // scene is now clean
		HE_LOG_INFO(Editor, "%s", ("EditorApplication: scene saved to " + path).c_str());
	}
	else
	{
		HE_LOG_ERROR(Editor, "%s", ("EditorApplication: failed to save scene to " + path).c_str());
	}
}

void EditorApplication::pushEnvironment(float dt)
{
	if (!renderer() || !m_editorWorld) return;
	// The Sky (EnvironmentComponent) is a scene entity now, not a fixed root component.
	// No Sky entity → tell the renderer to skip the sky pass (flat background).
	const Entity envEntity = m_editorWorld->environmentEntity();
	auto* env = (envEntity == entt::null)
		? nullptr : m_editorWorld->registry().try_get<EnvironmentComponent>(envEntity);
	if (!env)
	{
		renderer()->SetEnvironmentSettings(IRenderer::EnvironmentSettings{ .skyEnabled = false });
		return;
	}

	// Shared with the packaged game runtime so the viewport and a shipped build push
	// the SAME fields. It also AUTO-ADVANCES the day-night cycle by `dt` — which is
	// why this function is called exactly once per frame (dt = 0 from the headless
	// dump path, where nothing may advance).
	renderer()->SetEnvironmentSettings(HE::makeEnvironmentSettings(*env, dt));
}

void EditorApplication::warmupWorldMaterials()
{
	if (!m_editorWorld || !renderer()) return;
	renderer()->WarmupMaterials(SceneSystems::collectAssetRefs(*m_editorWorld));
}

void EditorApplication::openScene(const std::string& path)
{
	if (!m_editorWorld || path.empty()) return;

	if (m_isPlaying) setPlayMode(false); // leave play mode before switching scenes

	SceneSerializer serializer;
	m_editorWorld->clear();
	if (serializer.load(*m_editorWorld, path, SerializeFormat::JSON))
	{
		m_currentScenePath = path;
		SceneSystems::preloadAssetRefs(*m_editorWorld, contentManager());
		warmupWorldMaterials(); // build custom-material pipelines before the first draw
		HE_LOG_INFO(Editor, "%s", ("EditorApplication: scene opened from " + path).c_str());
	}
	else
	{
		m_currentScenePath.clear();
		HE_LOG_ERROR(Editor, "%s", ("EditorApplication: failed to open scene from " + path).c_str());
	}

	m_selectedEntity = entt::null;
	m_editorWorld->markHierarchyDirty();
	m_undo.clearHistory();
	m_savedRevision = m_undo.revision();
}

void EditorApplication::openSceneAdditive(const std::string& path)
{
	if (!m_editorWorld || path.empty()) return;

	SceneSerializer serializer;
	if (serializer.loadAdditive(*m_editorWorld, path, SerializeFormat::JSON))
	{
		m_undo.snapshotNow();
		SceneSystems::preloadAssetRefs(*m_editorWorld, contentManager());
		HE_LOG_INFO(Editor, "%s", ("EditorApplication: scene merged from " + path).c_str());
	}
	else
	{
		HE_LOG_ERROR(Editor, "%s", ("EditorApplication: failed to merge scene from " + path).c_str());
	}
	m_editorWorld->markHierarchyDirty();
}

void EditorApplication::newScene()
{
	if (!m_editorWorld) return;

	if (m_isPlaying) setPlayMode(false);

	m_editorWorld->clear(); // keeps the root entity, drops all children
	m_currentScenePath.clear();
	m_selectedEntity = entt::null;
	m_editorWorld->markHierarchyDirty();
	m_undo.clearHistory();
	m_savedRevision = m_undo.revision();
	HE_LOG_INFO(Editor, "%s", "EditorApplication: new empty scene");
}

void EditorApplication::OnShutdown()
{
	// A project export may still be packing on its worker thread — wait for it
	// (destroying a joinable std::thread would terminate the process).
	EditorUI::joinPendingExport();

	// Same rule for the toolchain probe (destroying a joinable std::thread
	// terminates the process).
	if (m_toolchainThread.joinable()) m_toolchainThread.join();
	if (m_gitThread.joinable()) m_gitThread.join();
	// Bounded by the probe's own per-call timeouts, so this cannot hang shutdown.
	if (m_gitIdentityThread.joinable()) m_gitIdentityThread.join();
	// The auto-install worker can outlive the dialog (installs take minutes); join
	// it too so we never destroy it joinable.
	if (m_installThread.joinable()) m_installThread.join();

#ifdef HE_IMGUI_ENABLED
	if (!m_imguiReady) return;
	HE_LOG_INFO(Editor, "%s", "EditorApplication::OnShutdown — shutting down ImGui");

	if (renderer()) renderer()->SetOverlayCallback(nullptr);

	switch (m_backend)
	{
	case HE::RendererBackend::OpenGL:
		// ImGui_ImplOpenGL3_Shutdown() re-inits the GL3W loader inside
		// DestroyDeviceObjects(); its parse_version() then reads glGetString/
		// glGetIntegerv. If no GL context is current on this thread, those return
		// null and parse_version dereferences it -> SIGSEGV during shutdown. The
		// render loop leaves the context current on the happy path, but nothing
		// guarantees it here (e.g. after an error or a secondary-window teardown),
		// so re-assert it explicitly before tearing ImGui down.
		if (window() && window()->GetNativeWindow() && window()->GetGLContext())
			SDL_GL_MakeCurrent(window()->GetNativeWindow(),
				static_cast<SDL_GLContext>(window()->GetGLContext()));
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
#ifdef _WIN32
	case HE::RendererBackend::D3D11:
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
	case HE::RendererBackend::D3D12:
		ImGui_ImplDX12_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		if (m_d3d12SrvAllocator)
		{
			auto* alloc = static_cast<D3D12DescriptorHeapAllocator*>(m_d3d12SrvAllocator);
			alloc->Destroy();
			delete alloc;
			m_d3d12SrvAllocator = nullptr;
		}
		if (m_d3d12SrvHeap)
		{
			static_cast<ID3D12DescriptorHeap*>(m_d3d12SrvHeap)->Release();
			m_d3d12SrvHeap = nullptr;
		}
		break;
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
	case HE::RendererBackend::Vulkan:
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
	case HE::RendererBackend::Metal:
		ImGuiMetalBridge::Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
#endif
	default:
		break;
	}

	ImGui::DestroyContext();
	m_imguiReady = false;
#endif // HE_IMGUI_ENABLED

	// Release logo GPU texture
	if (m_logoTexture)
	{
		renderer()->DestroyImGuiTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_logoTexture)));
		m_logoTexture = 0;
	}
	// Same for the Content Browser's rendered asset tiles — they are renderer-owned
	// textures, so they have to go while the renderer is still alive.
	AssetThumbnailCache::shutdown();

	m_audioEngine.shutdown();

	GlobalState& globalstate = GlobalState::getInstance();
	globalstate.setCustomConfigEntry("KeepCPUAssets",               m_editorConfig.KeepCPUAssets);
	globalstate.setCustomConfigEntry("KeepCPUAssetsInfoAcknowledged", m_editorConfig.KeepCPUAssetsInfoAcknowledged);
	globalstate.setCustomConfigEntry("ContentBrowserRefreshRate",   m_editorConfig.ContentBrowserRefreshRate);
	globalstate.setCustomConfigEntry("CbTreeWidth",                 m_editorConfig.CbTreeWidth);
	globalstate.setCustomConfigEntry("UiFontScale",                m_editorConfig.UiFontScale);
	globalstate.setCustomConfigEntry("EditorCameraSpeed",          m_editorConfig.EditorCameraSpeed);
	// Persist the last editor camera view so it is restored next launch. Only when the
	// camera was actually used this session (otherwise keep any previously-saved view).
	if (m_editorCamera.initialised())
	{
		globalstate.setCustomConfigEntry("EditorCamPosX",  m_editorCamera.position().x);
		globalstate.setCustomConfigEntry("EditorCamPosY",  m_editorCamera.position().y);
		globalstate.setCustomConfigEntry("EditorCamPosZ",  m_editorCamera.position().z);
		globalstate.setCustomConfigEntry("EditorCamYaw",   m_editorCamera.yaw());
		globalstate.setCustomConfigEntry("EditorCamPitch", m_editorCamera.pitch());
		globalstate.setCustomConfigEntry("EditorCamPivot", m_editorCamera.pivotDistance());
		globalstate.setCustomConfigEntry("EditorCamValid", true);
	}
	globalstate.setCustomConfigEntry("MaxFps",                     m_editorConfig.MaxFps);
	globalstate.setCustomConfigEntry("BloomEnabled",               m_editorConfig.BloomEnabled);
	globalstate.setCustomConfigEntry("BloomThreshold",             m_editorConfig.BloomThreshold);
	globalstate.setCustomConfigEntry("BloomIntensity",             m_editorConfig.BloomIntensity);
	globalstate.setCustomConfigEntry("SSAOEnabled",                m_editorConfig.SSAOEnabled);
	globalstate.setCustomConfigEntry("SSAORadius",                 m_editorConfig.SSAORadius);
	globalstate.setCustomConfigEntry("SSAOIntensity",              m_editorConfig.SSAOIntensity);
	globalstate.setCustomConfigEntry("SSAOMethod",                 m_editorConfig.SSAOMethod);
	globalstate.setCustomConfigEntry("GpuParticles",              m_editorConfig.GpuParticles);
	globalstate.setCustomConfigEntry("GlobalIlluminationEnabled", m_editorConfig.GlobalIlluminationEnabled);
	globalstate.setCustomConfigEntry("GIIndirectIntensity",       m_editorConfig.GIIndirectIntensity);
	globalstate.setCustomConfigEntry("GILightRadius",             m_editorConfig.GILightRadius);
	globalstate.setCustomConfigEntry("GIReflectionsEnabled",      m_editorConfig.GIReflectionsEnabled);
	globalstate.setCustomConfigEntry("GIReflIntensity",           m_editorConfig.GIReflIntensity);
	globalstate.setCustomConfigEntry("GIReflMaxRoughness",        m_editorConfig.GIReflMaxRoughness);
	globalstate.setCustomConfigEntry("GIReflQuality",             m_editorConfig.GIReflQuality);
	globalstate.setCustomConfigEntry("RenderPath",                m_editorConfig.RenderPath);
	globalstate.setCustomConfigEntry("SSREnabled",                m_editorConfig.SSREnabled);
	globalstate.setCustomConfigEntry("SSRIntensity",              m_editorConfig.SSRIntensity);
	globalstate.setCustomConfigEntry("SSRQuality",                m_editorConfig.SSRQuality);
	globalstate.setCustomConfigEntry("SSRMaxRoughness",           m_editorConfig.SSRMaxRoughness);
	globalstate.setCustomConfigEntry("QuickSettingsFavorites",     m_editorConfig.QuickSettingsFavorites);
	globalstate.writeConfig();
}

bool EditorApplication::OnEvent(const SDL_Event& event)
{
#ifdef HE_IMGUI_ENABLED
	if (!m_imguiReady) return false;

	// Forward every SDL event to the active ImGui platform backend.
	// ImGui_ImplSDL3_ProcessEvent returns true when ImGui wants to own the event.
	bool consumed = false;

	switch (m_backend)
	{
	case HE::RendererBackend::OpenGL:
	case HE::RendererBackend::Vulkan:
	case HE::RendererBackend::Metal:
		// SDL3 platform backend handles keyboard, mouse, window, touch.
		consumed = ImGui_ImplSDL3_ProcessEvent(&event);
		break;
#ifdef _WIN32
	case HE::RendererBackend::D3D11:
	case HE::RendererBackend::D3D12:
		// Win32 platform backend only handles Win32 messages via WndProc.
		// We still forward SDL events for mouse/keyboard via SDL3 backend,
		// but for D3D+Win32 the ImGui_ImplWin32 path already intercepts
		// Win32 messages; ImGui_ImplSDL3_ProcessEvent handles the rest.
		consumed = ImGui_ImplSDL3_ProcessEvent(&event);
		break;
#endif
	default:
		break;
	}

	// Forward OS window focus changes to the GameInstance (fires
	// OnWindowFocusChanged only while play mode is running).
	if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)      m_gameInstance.setWindowFocus(true);
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)   m_gameInstance.setWindowFocus(false);

	// A focused in-game text field (PIE) owns the keyboard: route text + edit keys
	// to the widget. Checked before Esc so typing works, but Esc still releases.
	if (m_isPlaying && m_editorWorld && m_editorWorld->widgets().hasFocusedTextField())
	{
		if (event.type == SDL_EVENT_TEXT_INPUT)
		{
			m_editorWorld->widgets().inputText(event.text.text);
			return true;
		}
		if (event.type == SDL_EVENT_KEY_DOWN)
		{
			if (event.key.key == SDLK_BACKSPACE) { m_editorWorld->widgets().inputBackspace(); return true; }
			if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
				{ m_editorWorld->widgets().inputSubmit(); return true; }
			if (event.key.key != SDLK_ESCAPE) return true; // swallow other keys while typing
		}
	}

	// Esc toggles the play-mode mouse capture (like the packaged game): release it to
	// click the editor UI (e.g. Stop), press again to resume mouse-look. Only in PIE.
	if (m_isPlaying && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
	    && event.key.key == SDLK_ESCAPE)
	{
		setPlayMouseCaptured(!m_playMouseCaptured);
		return true;
	}

	// While PIE owns the mouse, the game owns the keyboard too. Without this,
	// NavEnableKeyboard keeps io.WantCaptureKeyboard true whenever any ImGui window
	// is focused, so the tail of this function would consume every key event before
	// it ever reaches the engine Input — WASD in play mode would be dead. Esc (above)
	// is the one key the editor keeps for itself. Key-UPs pass whenever playing (even
	// while released): a key held across the Esc toggle must still deliver its release,
	// or Input would keep it "down" forever and the camera would drift on re-capture.
	if (m_isPlaying &&
	    (event.type == SDL_EVENT_KEY_UP ||
	     (m_playMouseCaptured && (event.type == SDL_EVENT_KEY_DOWN ||
	                              event.type == SDL_EVENT_TEXT_INPUT))))
		return false;

	// ── Unsaved-changes guard for OS-level close (window X / Cmd+Q / app quit) ──
	// Window::PollEvents() has already flagged the window to close this frame; if
	// there is anything unsaved, veto that here and ask the UI to raise the
	// save-prompt instead (EditorUI turns m_exitRequested into a guarded Quit). The
	// prompt's "quit" path then exits cleanly through Application::Quit(). Skipped
	// in headless-dump mode, when no project is loaded, or when nothing is dirty
	// (let it close normally). For window-close events we only react to the *main*
	// window — ImGui's secondary viewport windows manage their own close.
	//
	// "Anything unsaved" is deliberately BOTH tests: an asset tab (material / UI
	// widget / particle / animator state machine …) keeps its edits in per-panel
	// state and does not bump the world undo revision, so the scene-revision test
	// alone let a clean-scene + dirty-graph session quit without any prompt — on
	// macOS ⌘Q is the only quit path, so those edits were simply gone.
	const bool osCloseRequest =
		event.type == SDL_EVENT_QUIT ||
		(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && window() &&
		 event.window.windowID == SDL_GetWindowID(window()->GetNativeWindow()));
	if (osCloseRequest && m_dumpPath.empty() && m_projectLoaded)
	{
		const bool sceneDirty = m_undo.revision() != m_savedRevision;
		// Panel-driven, not m_tabs-driven: a dirty tab the user CLOSED keeps its
		// panel state but is gone from m_tabs, so walking m_tabs would let those
		// edits be thrown away silently.
		const bool tabsDirty = !EditorUI::unsavedAssetPaths().empty();
		if (sceneDirty || tabsDirty)
		{
			if (window()) window()->CancelClose();
			m_exitRequested = true;
			return true; // consume — defer the quit until the user resolves the prompt
		}
	}

	// Only truly consume the event if ImGui wants *exclusive* input —
	// i.e. keyboard events when a text field is focused, or mouse events
	// when the cursor is over an ImGui window. This way engine hotkeys
	// still work when the mouse is over the viewport.
	const ImGuiIO& io = ImGui::GetIO();
	if (consumed)
	{
		if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
			event.type == SDL_EVENT_TEXT_INPUT)
			return io.WantCaptureKeyboard;

		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
			event.type == SDL_EVENT_MOUSE_MOTION        || event.type == SDL_EVENT_MOUSE_WHEEL)
			return io.WantCaptureMouse;
	}
#endif // HE_IMGUI_ENABLED
	return false;
}
