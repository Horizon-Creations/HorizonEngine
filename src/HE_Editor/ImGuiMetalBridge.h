#pragma once
// Thin C++ wrapper around imgui_impl_metal so EditorApplication.cpp and
// EditorUI.cpp (plain C++) never have to include the Objective-C++ backend
// header. Implemented in ImGuiMetalBridge.mm, built only on macOS.
struct ImDrawData;

namespace ImGuiMetalBridge
{
	// device is id<MTLDevice> (from MetalRenderer::GetDevice)
	bool Init(void* device);
	void Shutdown();
	// renderPassDescriptor is MTLRenderPassDescriptor* describing the
	// swapchain attachment formats (MetalRenderer::GetFramePassDescriptor)
	void NewFrame(void* renderPassDescriptor);
	// commandBuffer / renderEncoder come from the MetalOverlayContext
	void RenderDrawData(ImDrawData* drawData, void* commandBuffer, void* renderEncoder);
}
