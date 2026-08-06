#include "EditorInput.h"
#include "EditorApplication.h" // AppContext (for the EditorConfig reference)
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_touch.h>

namespace EditorInput
{

bool detectTrackpad()
{
	// Battery presence ≈ laptop ≈ built-in trackpad. One IOKit/system query,
	// cached for the session — whether the machine HAS a battery cannot change.
	static const bool battery = []
	{
		const SDL_PowerState ps = SDL_GetPowerInfo(nullptr, nullptr);
		return ps == SDL_POWERSTATE_ON_BATTERY || ps == SDL_POWERSTATE_CHARGING ||
		       ps == SDL_POWERSTATE_CHARGED;
	}();
	if (battery) return true;

	// Desktop with an external trackpad (e.g. Mac Studio + Magic Trackpad).
	// macOS only registers the device with SDL on its first touch event, so
	// keep probing until one shows up, then remember — Auto upgrades itself
	// the moment the user first touches the pad.
	static bool sawTrackpad = false;
	if (!sawTrackpad)
	{
		int count = 0;
		if (SDL_TouchID* ids = SDL_GetTouchDevices(&count))
		{
			for (int i = 0; i < count; ++i)
			{
				const SDL_TouchDeviceType t = SDL_GetTouchDeviceType(ids[i]);
				if (t == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE ||
				    t == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE)
				{
					sawTrackpad = true;
					break;
				}
			}
			SDL_free(ids);
		}
	}
	return sawTrackpad;
}

bool trackpadPointer(const AppContext& ctx)
{
	return resolveTrackpad(ctx.editorConfig.PointerInput, detectTrackpad());
}

} // namespace EditorInput
