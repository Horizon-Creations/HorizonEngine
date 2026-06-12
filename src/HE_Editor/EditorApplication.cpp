#include "EditorApplication.h"
#include "EditorUI.h"
#include <Renderer/RendererFactory.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <array>
#include <future>

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

HE::ApplicationConfig EditorApplication::GetConfig() const
{
	HE::ApplicationConfig cfg;
	cfg.windowprops.title  = "HorizonEngine Editor";
	cfg.windowprops.width  = 1600;
	cfg.windowprops.height = 900;
	cfg.windowprops.vsync  = true;
	cfg.windowprops.mode   = HE::WindowMode::Windowed;
	cfg.backend = m_globalState->getSelectedRHI();
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

		// Scale font sizes by the display's content scale (HiDPI support)
		float dpiScale = 1.0f;
		if (SDL_Window* nativeWin = window() ? window()->GetNativeWindow() : nullptr)
		{
			SDL_DisplayID disp = SDL_GetDisplayForWindow(nativeWin);
			float s = SDL_GetDisplayContentScale(disp);
			if (s > 0.0f) dpiScale = s;
		}

		const float sizeBody       = 13.0f * dpiScale;
		const float sizeSubheading = 16.0f * dpiScale;
		const float sizeHeading    = 19.0f * dpiScale;

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
			vkInfo.DescriptorPoolSize = 8; // let ImGui create an internal pool
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
	}
#endif // HE_IMGUI_ENABLED
	m_backend      = m_globalState->getSelectedRHI();
	m_backend_name = getRHIName(m_backend);

	GlobalState& globalstate = GlobalState::getInstance();
	m_editorConfig.ContentBrowserRefreshRate   = globalstate.getCustomConfigInt("ContentBrowserRefreshRate",   m_editorConfig.ContentBrowserRefreshRate);
	m_editorConfig.KeepCPUAssets               = globalstate.getCustomConfigBool("KeepCPUAssets",               m_editorConfig.KeepCPUAssets);
	m_editorConfig.KeepCPUAssetsInfoAcknoleged = globalstate.getCustomConfigBool("KeepCPUAssetsInfoAcknoleged", m_editorConfig.KeepCPUAssetsInfoAcknoleged);
	m_editorConfig.QsRendererOpen              = globalstate.getCustomConfigBool("QsRendererOpen",              m_editorConfig.QsRendererOpen);
	m_editorConfig.QsEditorOpen                = globalstate.getCustomConfigBool("QsEditorOpen",                m_editorConfig.QsEditorOpen);
	m_editorConfig.CbTreeWidth                 = globalstate.getCustomConfigFloat("CbTreeWidth", m_editorConfig.CbTreeWidth);

#ifdef HE_IMGUI_ENABLED
	// ── Load HC_Logo ──────────────────────────────────────────────────────────
	{
		const char* basePath = SDL_GetBasePath();
		std::string logoPath = std::string(basePath ? basePath : "") + "Images/HC_Logo.png";

		int w = 0, h = 0, ch = 0;
		unsigned char* pixels = stbi_load(logoPath.c_str(), &w, &h, &ch, 4);
		if (pixels)
		{
			// ── Window icon via SDL ───────────────────────────────────────────
			SDL_Surface* iconSurface = SDL_CreateSurfaceFrom(
				w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
			if (iconSurface && window() && window()->GetNativeWindow())
			{
				SDL_SetWindowIcon(window()->GetNativeWindow(), iconSurface);
				SDL_DestroySurface(iconSurface);
			}

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

	// Create the editor world and register it with the base Application
	m_editorWorld = std::make_unique<HorizonWorld>();
	setWorld(m_editorWorld.get());
	Logger::Log(Logger::LogLevel::Info, "EditorApplication: HorizonWorld created and registered");

	// Register the project-loaded callback BEFORE the first loadProject call so
	// the startup scene is already loaded when OnInit returns.
	m_projectManager.setOnProjectLoaded([this](const std::string& sceneAbsPath)
	{
		setWorld(m_editorWorld.get());

		if (!sceneAbsPath.empty())
		{
			SceneSerializer serializer;
			bool ok = serializer.load(*m_editorWorld, sceneAbsPath, SerializeFormat::JSON);
			if (ok)
				Logger::Log(Logger::LogLevel::Info,
					("EditorApplication: startup scene loaded from " + sceneAbsPath).c_str());
			else
				Logger::Log(Logger::LogLevel::Warning,
					("EditorApplication: failed to load startup scene from " + sceneAbsPath).c_str());
		}
		else
		{
			Logger::Log(Logger::LogLevel::Info, "EditorApplication: no startup scene defined for this project");
		}

		m_editorWorld->markHierarchyDirty();
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
}

void EditorApplication::OnRender(float dt)
{
	// ── Window title ─────────────────────────────────────────────────────
	{
		const std::string& projName = m_projectManager.currentProject().name;
		const std::string title = projName.empty()
			? "Horizon Engine"
			: "Horizon Engine — " + projName;
		window()->SetTitle(title);
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

	AppContext ctx = makeContext();
	EditorUI::render(ctx, dt);

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

AppContext EditorApplication::makeContext()
{
	m_sdlDialogBridge.pendingDirResult  = &m_pendingDirResult;
	m_sdlDialogBridge.pendingDirReady   = &m_pendingDirReady;
	m_sdlDialogBridge.pendingFileResult = &m_pendingFileResult;
	m_sdlDialogBridge.pendingFileReady  = &m_pendingFileReady;

	return AppContext{
		.imguiReady          = m_imguiReady,
		.quit                = [this]{ Quit(); },
		.editorConfig        = m_editorConfig,
		.vsync               = m_vsync,
		.backendName         = m_backend_name,
		.backend             = m_backend,
		.globalState         = m_globalState,
		.projectManager      = &m_projectManager,
		.renderer            = renderer(),
		.window              = window(),
		.world               = world(),
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


void EditorApplication::OnShutdown()
{
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

	GlobalState& globalstate = GlobalState::getInstance();
	globalstate.setCustomConfigEntry("KeepCPUAssets",               m_editorConfig.KeepCPUAssets);
	globalstate.setCustomConfigEntry("KeepCPUAssetsInfoAcknoleged", m_editorConfig.KeepCPUAssetsInfoAcknoleged);
	globalstate.setCustomConfigEntry("ContentBrowserRefreshRate",   m_editorConfig.ContentBrowserRefreshRate);
	globalstate.setCustomConfigEntry("QsRendererOpen",              m_editorConfig.QsRendererOpen);
	globalstate.setCustomConfigEntry("QsEditorOpen",                m_editorConfig.QsEditorOpen);
	globalstate.setCustomConfigEntry("CbTreeWidth",                 m_editorConfig.CbTreeWidth);
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
