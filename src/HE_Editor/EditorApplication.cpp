#include "EditorApplication.h"
#include "EditorUI.h"
#include "HorizonVersion.h"
#include <Diagnostics/Profiler.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h> // HE_DUMP_MATPRECOMPILE witness
#include <glm/gtc/quaternion.hpp>
#include <HorizonScene/TerrainSystem.h>
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
		if      (s == "Metal")               cfg.backend = HE::GraphicsAPI::Metal;
		else if (s == "OpenGL" || s == "GL") cfg.backend = HE::GraphicsAPI::OpenGL;
		else if (s == "Vulkan")              cfg.backend = HE::GraphicsAPI::Vulkan;
		else if (s == "D3D11")               cfg.backend = HE::GraphicsAPI::D3D11;
		else if (s == "D3D12")               cfg.backend = HE::GraphicsAPI::D3D12;
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
	Logger::Log(Logger::LogLevel::Info, "EditorApplication: creating renderer");
	return RendererFactory::Create(m_backend);
}

void EditorApplication::OnInit()
{
	// ── Headless frame-dump hook (validation / CI screenshots) ──────────────
	if (const char* p = std::getenv("HE_DUMP_PATH"); p && *p)
	{
		m_dumpPath = p;
		if (const char* q = std::getenv("HE_DUMP_QUIT"); q && *q)
			m_dumpQuit = (std::atoi(q) != 0);
		Logger::Log(Logger::LogLevel::Info,
			("EditorApplication: frame dump armed → " + m_dumpPath).c_str());
	}
#ifdef HE_IMGUI_ENABLED
	Logger::Log(Logger::LogLevel::Info, "EditorApplication::OnInit — initialising ImGui");
	m_vsync = GetConfig().windowprops.vsync;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ApplyHorizonDarkTheme();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

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
			Logger::Log(Logger::LogLevel::Info, ("EditorApplication: fonts loaded from " + fontPath).c_str());
		}
		else
		{
			Logger::Log(Logger::LogLevel::Warning, ("EditorApplication: font not found at " + fontPath + " — using ImGui default").c_str());
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

	switch (m_backend)
	{
	// ── OpenGL ────────────────────────────────────────────────────────────────
	case RendererFactory::Backend::OpenGL:
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
		Logger::Log(Logger::LogLevel::Info, "Initialized ImGui OpenGL backend");
		break;

#ifdef HE_IMGUI_METAL_ENABLED
	// ── Metal ─────────────────────────────────────────────────────────────────
	case RendererFactory::Backend::Metal:
	{
		auto* mtl = static_cast<MetalRenderer*>(renderer());
		if (mtl && mtl->GetDevice())
		{
			ImGui_ImplSDL3_InitForMetal(window()->GetNativeWindow());
			if (ImGuiMetalBridge::Init(mtl->GetDevice()))
			{
				m_imguiReady = true;
				Logger::Log(Logger::LogLevel::Info, "Initialized ImGui Metal backend");
			}
		}
		break;
	}
#endif

#ifdef _WIN32
	// ── D3D11 ─────────────────────────────────────────────────────────────────
	case RendererFactory::Backend::D3D11:
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
			Logger::Log(Logger::LogLevel::Info, "Initialized ImGui D3D11 backend");
		}
		break;
	}

	// ── D3D12 ─────────────────────────────────────────────────────────────────
	case RendererFactory::Backend::D3D12:
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
			Logger::Log(Logger::LogLevel::Info, "Initialized ImGui D3D12 backend");
		}
		break;
	}
#endif // _WIN32

#ifdef HE_IMGUI_VULKAN_ENABLED
	// ── Vulkan ────────────────────────────────────────────────────────────────
	case RendererFactory::Backend::Vulkan:
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
			Logger::Log(Logger::LogLevel::Info, "Initialized ImGui Vulkan backend");
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
		Logger::Log(Logger::LogLevel::Info, "EditorApplication::OnInit — ImGui backend ready");
		renderer()->SetOverlayCallback([this](void* nativeContext)
		{
			ImDrawData* drawData = ImGui::GetDrawData();
			if (!drawData) return;

			switch (m_backend)
			{
			case RendererFactory::Backend::OpenGL:
				if (drawData->TotalVtxCount > 0)
					ImGui_ImplOpenGL3_RenderDrawData(drawData);
				break;
#ifdef _WIN32
			case RendererFactory::Backend::D3D11:
				if (drawData->TotalVtxCount > 0)
					ImGui_ImplDX11_RenderDrawData(drawData);
				break;
			case RendererFactory::Backend::D3D12:
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
			case RendererFactory::Backend::Vulkan:
				if (nativeContext && drawData->TotalVtxCount > 0)
					ImGui_ImplVulkan_RenderDrawData(drawData,
						static_cast<VkCommandBuffer>(nativeContext));
				break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
			case RendererFactory::Backend::Metal:
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
		if (m_backend == RendererFactory::Backend::D3D12)
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
		if (m_backend == RendererFactory::Backend::Vulkan)
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
	m_editorConfig.KeepCPUAssetsInfoAcknoleged = globalstate.getCustomConfigBool("KeepCPUAssetsInfoAcknoleged", m_editorConfig.KeepCPUAssetsInfoAcknoleged);
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
			Logger::Log(Logger::LogLevel::Info, ("EditorApplication: logo loaded from " + logoPath).c_str());
		}
		else
		{
			Logger::Log(Logger::LogLevel::Warning, ("EditorApplication: logo not found at " + logoPath).c_str());
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
				Logger::Log(Logger::LogLevel::Info, ("EditorApplication: icon loaded — " + path).c_str());
			}
			else
			{
				Logger::Log(Logger::LogLevel::Warning, ("EditorApplication: icon not found — " + path).c_str());
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
			Logger::Log(Logger::LogLevel::Info, ("EditorApplication: moon texture loaded from " + moonPath).c_str());
		}
		else
		{
			Logger::Log(Logger::LogLevel::Warning, ("EditorApplication: moon texture not found at " + moonPath).c_str());
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
		Logger::Log(Logger::LogLevel::Warning, "EditorApplication: audio engine init failed (no audio playback)");

	Logger::Log(Logger::LogLevel::Info, "EditorApplication: HorizonWorld created and registered");

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
			// Index every .hasset's (UUID → path) so scene component references
			// (mesh/material UUIDs) resolve after a reload without a bulk preload.
			const size_t indexed = contentManager().scanContentDirectory();
			Logger::Log(Logger::LogLevel::Info,
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
				Logger::Log(Logger::LogLevel::Info,
					("EditorApplication: startup scene loaded from " + sceneAbsPath).c_str());
			}
			else
				Logger::Log(Logger::LogLevel::Warning,
					("EditorApplication: failed to load startup scene from " + sceneAbsPath).c_str());
		}
		else
		{
			Logger::Log(Logger::LogLevel::Info, "EditorApplication: no startup scene defined for this project");
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
}

// Push the current SDL keyboard/mouse state into HE::api::input so input.* nodes
// and scripts can poll it during play. Mouse delta + scroll stay 0 here (the play
// camera controller owns SDL's relative-motion accumulator); position + buttons +
// keys (by SDL scancode name, e.g. "W"/"Space") are polled.
static void pushEngineInputSnapshot()
{
	int n = 0;
	const bool* ks = SDL_GetKeyboardState(&n);
	std::vector<std::string> down;
	if (ks)
		for (int sc = 0; sc < n; ++sc)
			if (ks[sc]) { const char* name = SDL_GetScancodeName((SDL_Scancode)sc); if (name && name[0]) down.emplace_back(name); }
	float mx = 0.0f, my = 0.0f;
	const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
	uint32_t buttons = 0;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   buttons |= 1u << 0;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  buttons |= 1u << 1;
	if (mb & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) buttons |= 1u << 2;
	HE::api::input::setMouse({ mx, my }, { 0.0f, 0.0f }, buttons, 0.0f);
	HE::api::input::setKeysDown(down);
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
		pushEngineInputSnapshot();
		// Zone requests (additive load / unload / show / hide / move) run in PIE
		// against the editor world — leaving play mode restores the pre-play
		// snapshot, which drops zone entities again. Only the FULL level switch
		// and activate stay game-runtime-only (the play snapshot belongs to THIS
		// scene), consumed loudly so a graph author sees why nothing happened.
		for (const auto& r : HE::api::scene::takeRequests())
		{
			HE::api::Ctx c{ m_editorWorld.get(), nullptr, &contentManager(), &m_audioEngine };
			if (r.kind == 1 && m_editorWorld) // additive zone
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
					Logger::Log(Logger::LogLevel::Warning,
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
				Logger::Log(Logger::LogLevel::Info,
					("PIE: zone " + std::to_string(r.zone) + " loaded ('" + r.path + "', "
					 + std::to_string(created.size()) + " entities"
					 + (r.hidden ? ", hidden" : "") + ")").c_str());
			}
			else if (r.kind == 2 && m_editorWorld) // unload zone
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
			else if (r.kind == 4) HE::api::scene::setZoneVisible(c, r.zone, r.flag);
			else if (r.kind == 5) HE::api::scene::setZonePosition(c, r.zone, r.pos);
			else
				Logger::Log(Logger::LogLevel::Warning,
					("scene." + std::string(r.kind == 0 ? "load" : "activate")
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
				m_contentRefreshFuture = std::async(std::launch::async, [gs]()
				{
					gs->refreshContentFolder();
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
	if (m_backend == RendererFactory::Backend::D3D12)
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
	if (m_backend == RendererFactory::Backend::Vulkan)
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
		Logger::Log(Logger::LogLevel::Error, "EditorApplication: no renderer for frame dump");
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

	// ── Sky-test capture (HE_DUMP_SKYTEST): aim the camera up at the sky and override
	// the scene environment so a headless dump exercises the sky features (stars /
	// nebula / 3D clouds / contrails) that the default down-looking editor camera and
	// daytime env never show. Env knobs are read from optional vars so several scenes
	// can be captured without rebuilding. No-op unless HE_DUMP_SKYTEST is set.
	if (const char* st = std::getenv("HE_DUMP_SKYTEST"); st && *st && m_editorWorld)
	{
		auto envF = [](const char* k, float d){ const char* v = std::getenv(k); return v && *v ? std::atof(v) : d; };
		if (auto* e = m_editorWorld->registry().try_get<EnvironmentComponent>(m_editorWorld->rootEntity()))
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
			e->nebulaQuality     = static_cast<int>(envF("HE_DUMP_NEBQUALITY", e->nebulaQuality));
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
				Logger::Log(Logger::LogLevel::Info,
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
				Logger::Log(Logger::LogLevel::Info, ok
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
				Logger::Log(Logger::LogLevel::Info,
					("EditorApplication: HE_DUMP_ENTITYPARAM override '" + ov.name + "' on entity").c_str());
			}
		}
		reg.emplace<MaterialComponent>(e, mc);
		Logger::Log(Logger::LogLevel::Info,
			"EditorApplication: HE_DUMP_MATERIALTEST sphere with custom-shader material added");
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
			Logger::Log(Logger::LogLevel::Info, "EditorApplication: preview stress loop done");
		}
	}
	for (int i = 0; i < 3; ++i)
		r->Render();

	std::vector<uint8_t> rgba;
	uint32_t w = 0, h = 0;
	if (r->CaptureViewport(rgba, w, h) && w > 0 && h > 0 && writeBMP(m_dumpPath, rgba, w, h))
		Logger::Log(Logger::LogLevel::Info,
			("EditorApplication: frame dumped (" + std::to_string(w) + "x" +
			 std::to_string(h) + ") → " + m_dumpPath).c_str());
	else
		Logger::Log(Logger::LogLevel::Error,
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
	auto& reg = m_editorWorld->registry();

	entt::entity cam = entt::null;
	for (auto [e, t, c] : reg.view<TransformComponent, CameraComponent>().each())
	{
		if (cam == entt::null) cam = e;
		if (c.isMain) { cam = e; break; }
	}

	// Re-assert the capture every frame: SDL engages relative mode only while the
	// *flagged* window holds keyboard focus, and with multi-viewport panels the focus
	// can move between OS windows mid-play — the flag set on one window silently stops
	// engaging when another gains focus. Also re-hide the cursor in case anything
	// slipped past ImGuiConfigFlags_NoMouseCursorChange and re-showed it.
	SDL_Window* const focusWin = SDL_GetKeyboardFocus();
	if (focusWin && !SDL_GetWindowRelativeMouseMode(focusWin))
		SDL_SetWindowRelativeMouseMode(focusWin, true);
	if (SDL_CursorVisible())
		SDL_HideCursor();

	// Relative mouse delta accumulated since the last frame. Read exactly once — a
	// second SDL_GetRelativeMouseState() would return zero because reading drains it.
	float dx = 0.0f, dy = 0.0f;
	SDL_GetRelativeMouseState(&dx, &dy);

	// Park the cursor back at the focused window's centre after each frame's relative
	// motion. With relative mode engaged this is a pure internal position update (SDL
	// generates no motion events for it); when it is NOT engaged (focus transition,
	// platform quirk) the OS cursor physically drifts and would stall the look at the
	// screen edge — the warp keeps it centred either way. SDL pre-sets last_x/last_y
	// to the warp target, so the warp never pollutes the relative accumulator.
	if (focusWin)
	{
		int fw = 0, fh = 0;
		SDL_GetWindowSize(focusWin, &fw, &fh);
		SDL_WarpMouseInWindow(focusWin, fw * 0.5f, fh * 0.5f);
	}

	// ── Self-diagnostic (throttled ~once/sec) ──────────────────────────────────────
	// One PIE test should be conclusive: this reports whether a camera is being driven
	// and whether mouse motion is actually reaching us, so a "still doesn't move" report
	// tells us which cause (no camera / input not arriving / extractor) to chase.
	static int   s_diagFrames = 0;
	static float s_diagMotion = 0.0f;
	s_diagMotion += std::abs(dx) + std::abs(dy);
	if (++s_diagFrames >= 60)
	{
		Logger::Log(Logger::LogLevel::Info,
			(std::string("PIE camera controller: ")
			+ (cam == entt::null ? "NO camera to drive" : "driving a scene camera")
			+ ", mouse motion (60 frames) = " + std::to_string(s_diagMotion)
			+ (input().IsKeyDown(SDL_SCANCODE_W) ? " [W held]" : "")
			+ (focusWin && SDL_GetWindowRelativeMouseMode(focusWin) ? "" : " [rel-mode OFF]")
			+ (SDL_CursorVisible() ? " [cursor visible]" : "")).c_str());
		s_diagFrames  = 0;
		s_diagMotion  = 0.0f;
	}

	if (cam == entt::null) return;
	auto& t = reg.get<TransformComponent>(cam);

	constexpr float kSensitivity = 0.12f; // degrees per pixel
	t.rotation.y -= dx * kSensitivity;    // yaw
	t.rotation.x -= dy * kSensitivity;    // pitch
	t.rotation.x = std::clamp(t.rotation.x, -89.0f, 89.0f);

	const glm::quat q = glm::quat(glm::radians(t.rotation));
	const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f);
	const glm::vec3 right   = q * glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);

	Input& in = input();
	glm::vec3 move(0.0f);
	if (in.IsKeyDown(SDL_SCANCODE_W)) move += forward;
	if (in.IsKeyDown(SDL_SCANCODE_S)) move -= forward;
	if (in.IsKeyDown(SDL_SCANCODE_D)) move += right;
	if (in.IsKeyDown(SDL_SCANCODE_A)) move -= right;
	if (in.IsKeyDown(SDL_SCANCODE_E) || in.IsKeyDown(SDL_SCANCODE_SPACE)) move += worldUp;
	if (in.IsKeyDown(SDL_SCANCODE_Q) || in.IsKeyDown(SDL_SCANCODE_LCTRL)) move -= worldUp;
	if (glm::dot(move, move) > 0.0f)
	{
		float speed = 6.0f; // units/sec
		if (in.IsKeyDown(SDL_SCANCODE_LSHIFT) || in.IsKeyDown(SDL_SCANCODE_RSHIFT)) speed *= 3.0f;
		t.position += glm::normalize(move) * speed * dt;
	}
	t.dirty = true;
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
			Logger::Log(Logger::LogLevel::Error,
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
				Logger::Log(Logger::LogLevel::Info,
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
		{
			auto& reg = m_editorWorld->registry();
			for (auto [entity, sc] : reg.view<ScriptComponent>().each())
			{
				if (!sc.enabled) continue;
				const ScriptAsset* asset = contentManager().getScript(sc.scriptAssetId);
				if (!asset || asset->sourceCode.empty()) continue;
				if (!m_scriptContext->isScriptLoaded(sc.moduleName, asset->language))
					m_scriptContext->loadScript(sc.moduleName, asset->sourceCode, asset->language);
				auto instId = m_scriptContext->createInstance(sc.moduleName, entity, asset->language);
				if (instId == ScriptEngine::kInvalidInstance) continue;
				m_scriptContext->injectProperties(instId, sc.properties);
				m_scriptContext->callOnStart(instId);
				m_scriptInstances[static_cast<uint32_t>(entity)] = instId;
			}
		}

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

		Logger::Log(Logger::LogLevel::Info, "EditorApplication: entering play mode");
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
			Logger::Log(Logger::LogLevel::Error,
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

		Logger::Log(Logger::LogLevel::Info, "EditorApplication: returned to edit mode");
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
		m_globalState->setCustomConfigEntry("openTabs:" + m_projectManager.currentProject().path, state.dump());
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
		Logger::Log(Logger::LogLevel::Info, ("EditorApplication: scene saved to " + path).c_str());
	}
	else
	{
		Logger::Log(Logger::LogLevel::Error, ("EditorApplication: failed to save scene to " + path).c_str());
	}
}

void EditorApplication::pushEnvironment(float dt)
{
	if (!renderer() || !m_editorWorld) return;
	auto* env = m_editorWorld->registry().try_get<EnvironmentComponent>(m_editorWorld->rootEntity());
	if (!env) return;

	// Auto-advance the day-night cycle (time flows with real time).
	if (env->dayNightCycle && env->autoAdvance && dt > 0.0f)
	{
		float dayFrac = dt / std::max(env->cycleSeconds, 1.0f);
		env->timeOfDay += dayFrac;
		env->timeOfDay -= std::floor(env->timeOfDay); // wrap to [0,1)
		// Lunar cycle: the moon phase advances one full cycle per moonCycleDays day-night cycles.
		if (env->moonPhaseAuto)
		{
			env->moonPhase += dayFrac / std::max(env->moonCycleDays, 0.1f);
			env->moonPhase -= std::floor(env->moonPhase);
		}
	}

	renderer()->SetEnvironmentSettings(IRenderer::EnvironmentSettings{
		.dayNightCycle = env->dayNightCycle, .timeOfDay = env->timeOfDay,
		.sunColor = env->sunColor, .sunIntensity = env->sunIntensity,
		.moonColor = env->moonColor, .moonIntensity = env->moonIntensity,
		.moonPhase = env->moonPhase,
		.cloudCoverage = env->cloudCoverage,
		.fogDensity = env->fogDensity, .fogHeightFalloff = env->fogHeightFalloff,
		.auroraIntensity = env->auroraIntensity,
		.milkyWayIntensity = env->milkyWayIntensity, .nebulaIntensity = env->nebulaIntensity,
		.nebulaColor = env->nebulaColor, .nebulaColor2 = env->nebulaColor2,
		.nebulaColor3 = env->nebulaColor3, .nebulaSeed = env->nebulaSeed,
		.nebulaQuality = env->nebulaQuality,
		.auroraColor = env->auroraColor,
		.auroraColorTop = env->auroraColorTop,
		.auroraHeight = env->auroraHeight, .auroraFragmentation = env->auroraFragmentation,
		.windDirection = env->windDirection, .windSpeed = env->windSpeed, .flash = env->flash,
		.wetness = env->wetness, .snowAmount = env->snowAmount, .rainAmount = env->rainAmount,
		.cloudMode = env->cloudMode, .cloudHeight = env->cloudHeight,
		.cloudQuality = env->cloudQuality, .lowResClouds = env->lowResClouds,
		.cloudDensity = env->cloudDensity, .cloudFluffiness = env->cloudFluffiness,
		.cloudTint = env->cloudTint,
		.contrailAmount = env->contrailAmount,
		.cirrusAmount = env->cirrusAmount, .cirrusSeed = env->cirrusSeed,
		.godRays = env->godRays, .shootingStars = env->shootingStars, .lensFlare = env->lensFlare,
		.starBrightness = env->starBrightness, .starColor = env->starColor,
		.starSize = env->starSize, .starSizeVariation = env->starSizeVariation,
		.starGlow = env->starGlow, .starTwinkle = env->starTwinkle,
		.starDensity = env->starDensity});
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
		Logger::Log(Logger::LogLevel::Info, ("EditorApplication: scene opened from " + path).c_str());
	}
	else
	{
		m_currentScenePath.clear();
		Logger::Log(Logger::LogLevel::Error, ("EditorApplication: failed to open scene from " + path).c_str());
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
		Logger::Log(Logger::LogLevel::Info, ("EditorApplication: scene merged from " + path).c_str());
	}
	else
	{
		Logger::Log(Logger::LogLevel::Error, ("EditorApplication: failed to merge scene from " + path).c_str());
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
	Logger::Log(Logger::LogLevel::Info, "EditorApplication: new empty scene");
}

void EditorApplication::OnShutdown()
{
	// A project export may still be packing on its worker thread — wait for it
	// (destroying a joinable std::thread would terminate the process).
	EditorUI::joinPendingExport();

#ifdef HE_IMGUI_ENABLED
	if (!m_imguiReady) return;
	Logger::Log(Logger::LogLevel::Info, "EditorApplication::OnShutdown — shutting down ImGui");

	if (renderer()) renderer()->SetOverlayCallback(nullptr);

	switch (m_backend)
	{
	case RendererFactory::Backend::OpenGL:
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
#ifdef _WIN32
	case RendererFactory::Backend::D3D11:
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
	case RendererFactory::Backend::D3D12:
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
	case RendererFactory::Backend::Vulkan:
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
	case RendererFactory::Backend::Metal:
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

	m_audioEngine.shutdown();

	GlobalState& globalstate = GlobalState::getInstance();
	globalstate.setCustomConfigEntry("KeepCPUAssets",               m_editorConfig.KeepCPUAssets);
	globalstate.setCustomConfigEntry("KeepCPUAssetsInfoAcknoleged", m_editorConfig.KeepCPUAssetsInfoAcknoleged);
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
	case RendererFactory::Backend::OpenGL:
	case RendererFactory::Backend::Vulkan:
	case RendererFactory::Backend::Metal:
		// SDL3 platform backend handles keyboard, mouse, window, touch.
		consumed = ImGui_ImplSDL3_ProcessEvent(&event);
		break;
#ifdef _WIN32
	case RendererFactory::Backend::D3D11:
	case RendererFactory::Backend::D3D12:
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
	// the active scene has unsaved edits, veto that here and ask the UI to raise
	// the save-prompt instead. The prompt's "quit" path then exits cleanly through
	// Application::Quit(). Skipped in headless-dump mode, when no project is loaded,
	// or when the scene is clean (let it close normally). For window-close events
	// we only react to the *main* window — ImGui's secondary viewport windows
	// manage their own close.
	if ((event.type == SDL_EVENT_QUIT ||
	     (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && window() &&
	      event.window.windowID == SDL_GetWindowID(window()->GetNativeWindow()))) &&
	    m_dumpPath.empty() && m_projectLoaded &&
	    m_undo.revision() != m_savedRevision)
	{
		if (window()) window()->CancelClose();
		m_exitRequested = true;
		return true; // consume — defer the quit until the user resolves the prompt
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
