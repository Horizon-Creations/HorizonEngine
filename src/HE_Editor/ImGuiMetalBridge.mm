#include "ImGuiMetalBridge.h"
#include <imgui.h>
#include <imgui_impl_metal.h>

#import <Metal/Metal.h>

namespace ImGuiMetalBridge
{
	bool Init(void* device)
	{
		return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device);
	}

	void Shutdown()
	{
		ImGui_ImplMetal_Shutdown();
	}

	void NewFrame(void* renderPassDescriptor)
	{
		ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)renderPassDescriptor);
	}

	void RenderDrawData(ImDrawData* drawData, void* commandBuffer, void* renderEncoder)
	{
		ImGui_ImplMetal_RenderDrawData(drawData,
			(__bridge id<MTLCommandBuffer>)commandBuffer,
			(__bridge id<MTLRenderCommandEncoder>)renderEncoder);
	}
}
