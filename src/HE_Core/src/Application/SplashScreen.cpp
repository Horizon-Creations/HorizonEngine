#include "Application/SplashScreen.h"
#include "Diagnostics/Log.h"

// PNG only, and private to this file: the splash needs one decoder for one
// image, and nothing else in HorizonCore has an opinion about stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace HE
{
namespace
{
	// Brand palette, sampled from HC_Logo.png (the same three values
	// scripts/dmg_assets/gen_assets.py uses for the installer artwork).
	constexpr SDL_Color kIvory = { 240, 230, 206, 255 };
	constexpr SDL_Color kGold  = { 234, 194,  78, 255 };
	constexpr SDL_Color kAmber = { 206, 124,  36, 255 };

	// Panel: near-black at the top fading to a warm near-black at the bottom, so
	// the gold reads as lit rather than pasted on.
	constexpr SDL_Color kBgTop    = {  16,  18,  24, 255 };
	constexpr SDL_Color kBgBottom = {  28,  22,  18, 255 };
	constexpr SDL_Color kBorder   = {  58,  50,  40, 255 };
	constexpr SDL_Color kTrack    = {  40,  36,  30, 255 };
	constexpr SDL_Color kMuted    = { 150, 142, 126, 255 };

	// Logical (point) layout. Everything is multiplied by the pixel scale when
	// it is drawn, so the panel is the same physical size on a Retina display
	// and on a 1:1 one — and crisp on both.
	constexpr float kPadding   = 26.0f;
	constexpr float kLogoTop   = 26.0f;
	constexpr float kLogoMaxW  = 320.0f;
	// The height is what actually binds — the wordmark is half again as wide as
	// it is tall, so the width cap never comes into play at this panel size.
	constexpr float kLogoMaxH  = 112.0f;
	constexpr float kTitlePx   = 17.0f;
	constexpr float kSmallPx   = 12.0f;
	constexpr float kBarH      = 4.0f;

	void fillRect(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c)
	{
		const SDL_FRect rect{ x, y, w, h };
		SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
		SDL_RenderFillRect(r, &rect);
	}

	SDL_Color lerp(SDL_Color a, SDL_Color b, float t)
	{
		const auto mix = [t](unsigned char x, unsigned char y) {
			return static_cast<unsigned char>(x + (y - x) * t + 0.5f);
		};
		return { mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a) };
	}
} // namespace

SplashScreen::~SplashScreen() { close(); }

bool SplashScreen::open(const SplashConfig& cfg)
{
	if (!cfg.enabled || m_window) return false;
	// The escape hatch. The splash also decides whether the primary window is
	// held back until startup finishes, and that is the part that could go
	// wrong on a platform nobody has run this on yet — so there has to be a way
	// to get the old behaviour back without a new build.
	if (const char* off = SDL_getenv("HE_NO_SPLASH"); off && *off && *off != '0')
	{
		HE_LOG_INFO(Core, "%s", "Splash: disabled by HE_NO_SPLASH");
		return false;
	}
	m_cfg = cfg;
	m_cfg.width  = std::clamp(m_cfg.width,  200, 1200);
	m_cfg.height = std::clamp(m_cfg.height, 120,  900);

	// Refcounted — the primary Window's SDL_Init(VIDEO) later just increments it,
	// and close() gives our reference back. Everything from here on is
	// best-effort: a machine that cannot open this window still has to start.
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
	{
		HE_LOG_INFO(Core, "Splash: no video subsystem (%s) — starting without it",
		            SDL_GetError());
		return false;
	}
	m_ownsVideo = true;

	// Created hidden so it can be centred before it is ever seen; a borderless
	// window that first appears wherever the OS felt like putting it and then
	// jumps to the middle is worse than no splash.
	m_window = SDL_CreateWindow(m_cfg.title.empty() ? "Horizon Engine" : m_cfg.title.c_str(),
	                            m_cfg.width, m_cfg.height,
	                            SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
	                            SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
	if (!m_window)
	{
		HE_LOG_WARN(Core, "Splash: SDL_CreateWindow failed (%s) — starting without it",
		            SDL_GetError());
		close();
		return false;
	}
	SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	// Renderer choice is about staying out of the engine's way. On Windows and
	// X11 the software renderer draws straight into a native framebuffer (GDI /
	// XShm) and never touches a graphics API, which is exactly what a panel of
	// flat rectangles wants and what keeps it clear of the GL context the engine
	// is about to create. macOS has no native framebuffer path — asking for
	// software there just routes through a renderer anyway — so it takes the
	// default (Metal), which coexists with the engine's own device fine.
#ifdef __APPLE__
	m_renderer = SDL_CreateRenderer(m_window, nullptr);
#else
	m_renderer = SDL_CreateRenderer(m_window, "software");
	if (!m_renderer) m_renderer = SDL_CreateRenderer(m_window, nullptr);
#endif
	if (!m_renderer)
	{
		HE_LOG_WARN(Core, "Splash: SDL_CreateRenderer failed (%s) — starting without it",
		            SDL_GetError());
		close();
		return false;
	}
	SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

	const char* rname = SDL_GetRendererName(m_renderer);
	m_usedGL = rname && (SDL_strstr(rname, "opengl") || SDL_strstr(rname, "gles"));

	// The scale is measured, not assumed: the output size already accounts for
	// HiDPI on the paths that have it and for the ones that do not.
	{
		int ow = 0, oh = 0;
		SDL_GetCurrentRenderOutputSize(m_renderer, &ow, &oh);
		m_scale = (ow > 0 && m_cfg.width > 0)
		        ? static_cast<float>(ow) / static_cast<float>(m_cfg.width)
		        : 1.0f;
		if (m_scale < 0.25f || m_scale > 8.0f) m_scale = 1.0f;
	}

	makeFace(m_faceBig,   kTitlePx * m_scale);
	makeFace(m_faceSmall, kSmallPx * m_scale);

	// ── Logo ────────────────────────────────────────────────────────────────
	// HC_Logo.png is a 1024² square with the wordmark floating in the middle of
	// a lot of transparency. Drawn as-is it would be a stamp-sized mark in the
	// centre of a large empty box, so the alpha bounding box is cropped out
	// first and the panel lays out the artwork, not the padding around it.
	if (!m_cfg.logoPath.empty())
	{
		int w = 0, h = 0, ch = 0;
		unsigned char* px = stbi_load(m_cfg.logoPath.c_str(), &w, &h, &ch, 4);
		if (px)
		{
			int x0 = w, y0 = h, x1 = -1, y1 = -1;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8)
					{
						x0 = std::min(x0, x); y0 = std::min(y0, y);
						x1 = std::max(x1, x); y1 = std::max(y1, y);
					}
			if (x1 >= x0 && y1 >= y0)
			{
				const int cw = x1 - x0 + 1, chh = y1 - y0 + 1;
				std::vector<unsigned char> crop(static_cast<size_t>(cw) * chh * 4);
				for (int y = 0; y < chh; ++y)
					std::memcpy(&crop[static_cast<size_t>(y) * cw * 4],
					            &px[((static_cast<size_t>(y + y0) * w) + x0) * 4],
					            static_cast<size_t>(cw) * 4);

				m_logo = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
				                           SDL_TEXTUREACCESS_STATIC, cw, chh);
				if (m_logo)
				{
					SDL_UpdateTexture(m_logo, nullptr, crop.data(), cw * 4);
					SDL_SetTextureBlendMode(m_logo, SDL_BLENDMODE_BLEND);
					SDL_SetTextureScaleMode(m_logo, SDL_SCALEMODE_LINEAR);
					m_logoW = cw;
					m_logoH = chh;
				}
			}
			stbi_image_free(px);
		}
		if (!m_logo)
			HE_LOG_INFO(Core, "Splash: no logo from '%s' — text only",
			            m_cfg.logoPath.c_str());
	}

	SDL_ShowWindow(m_window);
	// Raised explicitly: on a cold launch the window manager has no reason yet
	// to believe this process should be in front.
	SDL_RaiseWindow(m_window);
	draw();
	HE_LOG_INFO(Core, "Splash: %dx%d points at %.2fx via '%s'",
	            m_cfg.width, m_cfg.height, m_scale, rname ? rname : "?");
	return true;
}

bool SplashScreen::makeFace(Face& face, float pixelSize)
{
	// 512² holds ASCII at any size this panel uses with room to spare; stb packs
	// top-down and simply stops if it runs out, which `ok` reports.
	face.baked = bakeDefaultUIFont(pixelSize, 512, 512);
	if (!face.baked.ok || face.baked.pixels.empty()) return false;

	// The bake is single-channel coverage. Expanded to white RGBA once here so
	// every draw is a colour-mod away from any tint.
	std::vector<unsigned char> rgba(static_cast<size_t>(face.baked.atlasW) *
	                                face.baked.atlasH * 4);
	for (size_t i = 0; i < face.baked.pixels.size(); ++i)
	{
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = face.baked.pixels[i];
	}
	face.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
	                             SDL_TEXTUREACCESS_STATIC,
	                             face.baked.atlasW, face.baked.atlasH);
	if (!face.tex) return false;
	SDL_UpdateTexture(face.tex, nullptr, rgba.data(), face.baked.atlasW * 4);
	SDL_SetTextureBlendMode(face.tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(face.tex, SDL_SCALEMODE_LINEAR);
	return true;
}

float SplashScreen::textWidth(const Face& face, const std::string& s) const
{
	float w = 0.0f;
	for (size_t i = 0; i < s.size(); )
		if (const BakedGlyph* g = face.baked.glyph(uiUtf8Decode(s, i))) w += g->xadvance;
	return w;
}

float SplashScreen::text(const Face& face, const std::string& s, float x, float y,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a)
{
	const float w = textWidth(face, s);
	if (!face.tex) return w;

	SDL_SetTextureColorMod(face.tex, r, g, b);
	SDL_SetTextureAlphaMod(face.tex, a);
	float pen = x;
	for (size_t i = 0; i < s.size(); )
	{
		const BakedGlyph* glp = face.baked.glyph(uiUtf8Decode(s, i));
		if (!glp) continue;
		const BakedGlyph& gl = *glp;
		const float gw = gl.x1 - gl.x0;
		const float gh = gl.y1 - gl.y0;
		if (gw > 0.0f && gh > 0.0f)
		{
			// yoff is relative to the BASELINE (stb's bake convention), which is
			// why y is the baseline and not the top of the line.
			const SDL_FRect src{ gl.x0, gl.y0, gw, gh };
			const SDL_FRect dst{ pen + gl.xoff, y + gl.yoff, gw, gh };
			SDL_RenderTexture(m_renderer, face.tex, &src, &dst);
		}
		pen += gl.xadvance;
	}
	SDL_SetTextureAlphaMod(face.tex, 255);
	return w;
}

void SplashScreen::setStatus(const std::string& textLine, float progress)
{
	if (!m_window) return;
	m_status = textLine;
	if (progress >= 0.0f) m_progress = std::clamp(progress, 0.0f, 1.0f);
	draw();
}

void SplashScreen::draw()
{
	if (!m_renderer) return;

	// PumpEvents, never PollEvent: draining the queue here would swallow the
	// SDL_EVENT_GAMEPAD_ADDED that SDL posts for already-connected pads at init,
	// and Input only ever sees those through the main loop's first PollEvents —
	// so a splash that "handled" events would cost every user their controller.
	SDL_PumpEvents();

	const float S  = m_scale;
	const float W  = m_cfg.width  * S;
	const float H  = m_cfg.height * S;

	// Background: a vertical gradient in bands. Sixty-four of them across 300
	// points is finer than the eye resolves and cheaper than a texture.
	constexpr int kBands = 64;
	for (int i = 0; i < kBands; ++i)
	{
		const float t = static_cast<float>(i) / (kBands - 1);
		const float y = H * i / kBands;
		fillRect(m_renderer, 0.0f, y, W, H / kBands + 1.0f, lerp(kBgTop, kBgBottom, t));
	}

	// The horizon: a gold-to-amber rule across the top edge, the one piece of
	// the logo's language that survives at 3 pixels tall.
	{
		const float bar = 3.0f * S;
		constexpr int kSteps = 48;
		for (int i = 0; i < kSteps; ++i)
		{
			const float t = static_cast<float>(i) / (kSteps - 1);
			fillRect(m_renderer, W * i / kSteps, 0.0f, W / kSteps + 1.0f, bar,
			         lerp(kGold, kAmber, t));
		}
	}

	// Logo, fitted into its box and centred.
	float cursorY = kLogoTop * S;
	if (m_logo && m_logoW > 0 && m_logoH > 0)
	{
		const float fit = std::min(kLogoMaxW * S / m_logoW, kLogoMaxH * S / m_logoH);
		const float lw  = m_logoW * fit;
		const float lh  = m_logoH * fit;
		const SDL_FRect dst{ (W - lw) * 0.5f, cursorY, lw, lh };
		SDL_RenderTexture(m_renderer, m_logo, nullptr, &dst);
		cursorY += lh + 22.0f * S;
	}

	if (!m_cfg.title.empty())
	{
		const float tw = textWidth(m_faceBig, m_cfg.title);
		cursorY += kTitlePx * S;                       // advance to the baseline
		text(m_faceBig, m_cfg.title, (W - tw) * 0.5f, cursorY,
		     kIvory.r, kIvory.g, kIvory.b, 255);
		cursorY += 8.0f * S;
	}
	if (!m_cfg.subtitle.empty())
	{
		const float sw = textWidth(m_faceSmall, m_cfg.subtitle);
		cursorY += kSmallPx * S;
		text(m_faceSmall, m_cfg.subtitle, (W - sw) * 0.5f, cursorY,
		     kMuted.r, kMuted.g, kMuted.b, 255);
	}

	// ── Status line + progress bar, anchored to the bottom ───────────────────
	const float barY   = H - (kPadding + kBarH) * S;
	const float barX   = kPadding * S;
	const float barW   = W - 2.0f * barX;
	const float barHpx = kBarH * S;

	if (!m_status.empty())
		text(m_faceSmall, m_status, barX, barY - 12.0f * S,
		     kGold.r, kGold.g, kGold.b, 255);

	fillRect(m_renderer, barX, barY, barW, barHpx, kTrack);
	if (m_progress > 0.0f)
	{
		const float fillW = barW * m_progress;
		constexpr int kSteps = 32;
		for (int i = 0; i < kSteps; ++i)
		{
			const float t = static_cast<float>(i) / (kSteps - 1);
			fillRect(m_renderer, barX + fillW * i / kSteps, barY,
			         fillW / kSteps + 1.0f, barHpx, lerp(kAmber, kGold, t));
		}
	}

	// Hairline border last, so nothing paints over it — except along the top,
	// where the gold rule IS the edge and a grey line over it would only mute
	// the one bright thing on the panel.
	{
		const float t = std::max(1.0f, S);
		fillRect(m_renderer, 0.0f,  H - t, W, t, kBorder);
		fillRect(m_renderer, 0.0f,  0.0f,  t, H, kBorder);
		fillRect(m_renderer, W - t, 0.0f,  t, H, kBorder);
	}

	// A splash is gone before anyone can take a picture of it, which makes it
	// the one piece of UI you cannot check by looking. HE_SPLASH_DUMP=<file.bmp>
	// writes every frame it draws (…0.bmp, …1.bmp, …) so the layout and each
	// status line can be inspected after the fact. BMP because SDL writes it
	// without an encoder.
	if (const char* dump = SDL_getenv("HE_SPLASH_DUMP"); dump && *dump)
	{
		if (SDL_Surface* shot = SDL_RenderReadPixels(m_renderer, nullptr))
		{
			std::string path = dump;
			const size_t dot = path.find_last_of('.');
			const std::string n = std::to_string(m_dumpIndex++);
			path = (dot == std::string::npos) ? path + n
			                                  : path.substr(0, dot) + n + path.substr(dot);
			SDL_SaveBMP(shot, path.c_str());
			SDL_DestroySurface(shot);
		}
	}

	SDL_RenderPresent(m_renderer);
}

void SplashScreen::close()
{
	if (m_faceBig.tex)   { SDL_DestroyTexture(m_faceBig.tex);   m_faceBig.tex = nullptr; }
	if (m_faceSmall.tex) { SDL_DestroyTexture(m_faceSmall.tex); m_faceSmall.tex = nullptr; }
	if (m_logo)          { SDL_DestroyTexture(m_logo);          m_logo = nullptr; }
	if (m_renderer)      { SDL_DestroyRenderer(m_renderer);     m_renderer = nullptr; }
	if (m_window)        { SDL_DestroyWindow(m_window);         m_window = nullptr; }
	if (m_ownsVideo)     { SDL_QuitSubSystem(SDL_INIT_VIDEO);   m_ownsVideo = false; }
}

} // namespace HE
