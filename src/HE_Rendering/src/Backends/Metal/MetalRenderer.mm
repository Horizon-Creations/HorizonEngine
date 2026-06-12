#include "Backends/Metal/MetalRenderer.h"
#include <Window/Window.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <stdexcept>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// Swapchain format shared by every window target and the ImGui pass descriptor.
static constexpr MTLPixelFormat kSwapchainFormat = MTLPixelFormatBGRA8Unorm;

MetalRenderer::MetalRenderer()  = default;
MetalRenderer::~MetalRenderer() = default;

void MetalRenderer::Initialize(HE::Window* window)
{
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device)
		throw std::runtime_error("MetalRenderer: MTLCreateSystemDefaultDevice failed");
	m_device = (void*)CFBridgingRetain(device);

	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (!queue)
		throw std::runtime_error("MetalRenderer: newCommandQueue failed");
	m_commandQueue = (void*)CFBridgingRetain(queue);

	CreateTarget(m_primarySdlWindow, m_primaryTarget);

	// Persistent pass descriptor describing the swapchain attachment layout.
	// ImGui_ImplMetal_NewFrame() only inspects attachment formats / sample
	// count, so a 1×1 placeholder texture in the swapchain format suffices —
	// the real per-frame descriptor carries the actual drawable.
	MTLRenderPassDescriptor* imguiDesc = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLTextureDescriptor* placeholderDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSwapchainFormat width:1 height:1 mipmapped:NO];
	placeholderDesc.usage = MTLTextureUsageRenderTarget;
	imguiDesc.colorAttachments[0].texture     = [device newTextureWithDescriptor:placeholderDesc];
	imguiDesc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
	imguiDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
	m_imguiPassDescriptor = (void*)CFBridgingRetain(imguiDesc);

	m_shaderManager.setDevice(m_device);

	Logger::Log(Logger::LogLevel::Info,
		(std::string("MetalRenderer: initialized on ") + [[device name] UTF8String]).c_str());
}

void MetalRenderer::Shutdown()
{
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: shutdown");
	m_shaderManager.cleanup();

	for (auto& [sdlWin, target] : m_secondaryTargets)
		DestroyTarget(target);
	m_secondaryTargets.clear();
	DestroyTarget(m_primaryTarget);

	if (m_imguiPassDescriptor) { CFBridgingRelease(m_imguiPassDescriptor); m_imguiPassDescriptor = nullptr; }
	if (m_commandQueue)        { CFBridgingRelease(m_commandQueue);        m_commandQueue = nullptr; }
	if (m_device)              { CFBridgingRelease(m_device);              m_device = nullptr; }
	m_primarySdlWindow = nullptr;
}

void MetalRenderer::CreateTarget(SDL_Window* sdlWin, WindowTarget& out)
{
	SDL_MetalView view = SDL_Metal_CreateView(sdlWin);
	if (!view)
		throw std::runtime_error(std::string("MetalRenderer: SDL_Metal_CreateView failed: ") + SDL_GetError());

	CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
	if (!layer)
	{
		SDL_Metal_DestroyView(view);
		throw std::runtime_error("MetalRenderer: SDL_Metal_GetLayer returned null");
	}

	layer.device             = (__bridge id<MTLDevice>)m_device;
	layer.pixelFormat        = kSwapchainFormat;
	layer.framebufferOnly    = YES;
	layer.displaySyncEnabled = m_vsync;

	out.metalView  = (void*)view;
	out.metalLayer = (__bridge void*)layer; // borrowed — the view keeps it alive
}

void MetalRenderer::DestroyTarget(WindowTarget& target)
{
	if (target.metalView)
		SDL_Metal_DestroyView((SDL_MetalView)target.metalView);
	target.metalView  = nullptr;
	target.metalLayer = nullptr;
}

void MetalRenderer::EncodeClearPass(SDL_Window* sdlWin, WindowTarget& target, bool runOverlay)
{
	@autoreleasepool
	{
		CAMetalLayer* layer = (__bridge CAMetalLayer*)target.metalLayer;

		// Keep the drawable size in sync with the window's pixel size (HiDPI / resize)
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(sdlWin, &pw, &ph);
		if (pw > 0 && ph > 0)
		{
			CGSize size = layer.drawableSize;
			if ((int)size.width != pw || (int)size.height != ph)
				layer.drawableSize = CGSizeMake(pw, ph);
		}

		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (!drawable) return; // window hidden/minimized — skip the frame

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture     = drawable.texture;
		pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.18, 0.18, 0.20, 1.0);

		id<MTLCommandQueue>  queue   = (__bridge id<MTLCommandQueue>)m_commandQueue;
		id<MTLCommandBuffer> cmdBuf  = [queue commandBuffer];
		id<MTLRenderCommandEncoder> encoder = [cmdBuf renderCommandEncoderWithDescriptor:pass];

		// TODO: submit draw calls

		if (runOverlay && m_overlayCallback)
		{
			MetalOverlayContext ctx{
				(__bridge void*)cmdBuf,
				(__bridge void*)encoder,
				(__bridge void*)pass,
			};
			m_overlayCallback(&ctx);
		}

		[encoder endEncoding];
		[cmdBuf presentDrawable:drawable];
		[cmdBuf commit];
	}
}

void MetalRenderer::Render()
{
	if (!m_primarySdlWindow || !m_primaryTarget.metalLayer) return;
	EncodeClearPass(m_primarySdlWindow, m_primaryTarget, /*runOverlay=*/true);
}

IRenderer::Capabilities MetalRenderer::GetCapabilities() const
{
	return { true, true, true };
}

void MetalRenderer::SetVSync(bool enabled)
{
	m_vsync = enabled;
	if (m_primaryTarget.metalLayer)
		((__bridge CAMetalLayer*)m_primaryTarget.metalLayer).displaySyncEnabled = enabled;
	for (auto& [sdlWin, target] : m_secondaryTargets)
		((__bridge CAMetalLayer*)target.metalLayer).displaySyncEnabled = enabled;
}

// ─── Multi-window support ─────────────────────────────────────────────────────

void MetalRenderer::AttachWindow(HE::Window* window)
{
	SDL_Window* sdlWin = window->GetNativeWindow();
	if (m_secondaryTargets.count(sdlWin)) return; // already attached

	WindowTarget target;
	CreateTarget(sdlWin, target);
	m_secondaryTargets[sdlWin] = target;
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: secondary window attached");
}

void MetalRenderer::DetachWindow(HE::Window* window)
{
	auto it = m_secondaryTargets.find(window->GetNativeWindow());
	if (it == m_secondaryTargets.end()) return;
	DestroyTarget(it->second);
	m_secondaryTargets.erase(it);
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: secondary window detached");
}

void MetalRenderer::RenderWindow(HE::Window* window)
{
	auto it = m_secondaryTargets.find(window->GetNativeWindow());
	if (it == m_secondaryTargets.end()) return;
	EncodeClearPass(window->GetNativeWindow(), it->second, /*runOverlay=*/false);
}

// ─── ImGui texture helpers ────────────────────────────────────────────────────

void* MetalRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	if (!m_device || !rgba8Pixels || width <= 0 || height <= 0) return nullptr;

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		MTLTextureDescriptor* desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			                             width:(NSUInteger)width
			                            height:(NSUInteger)height
			                         mipmapped:NO];
		desc.usage       = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;

		id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
		if (!texture) return nullptr;

		[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
		           mipmapLevel:0
		             withBytes:rgba8Pixels
		           bytesPerRow:(NSUInteger)width * 4];

		// Retained — released in DestroyImGuiTexture. The pointer doubles as
		// the ImTextureID the editor hands to ImGui_ImplMetal_RenderDrawData.
		return (void*)CFBridgingRetain(texture);
	}
}

void MetalRenderer::DestroyImGuiTexture(void* handle)
{
	if (handle) CFBridgingRelease(handle);
}

// ─── Accessors ────────────────────────────────────────────────────────────────

void* MetalRenderer::GetDevice() const              { return m_device; }
void* MetalRenderer::GetCommandQueue() const        { return m_commandQueue; }

void* MetalRenderer::GetFramePassDescriptor() const   { return m_imguiPassDescriptor; }
