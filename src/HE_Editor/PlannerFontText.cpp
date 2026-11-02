#include "PlannerFontText.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <algorithm>
#include <cmath>

namespace PlannerFontText
{
namespace
{
std::vector<std::uint32_t> decode(const std::string& t)
{
	std::vector<std::uint32_t> out;
	for (std::size_t i = 0; i < t.size();)
	{
		const unsigned char c = static_cast<unsigned char>(t[i]);
		std::uint32_t cp = c;
		int extra = 0;
		if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
		else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
		else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
		++i;
		for (; extra > 0 && i < t.size(); --extra, ++i) cp = (cp << 6) | (static_cast<unsigned char>(t[i]) & 0x3F);
		out.push_back(cp);
	}
	return out;
}
} // namespace

bool draw(const unsigned char* fontData, int fontNumber, std::uint8_t* rgba, int width, int height, const std::vector<Run>& runs)
{
	if (!fontData || !rgba || width <= 0 || height <= 0) return false;
	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, fontData, stbtt_GetFontOffsetForIndex(fontData, fontNumber))) return false;
	int asc = 0, desc = 0, gap = 0;
	stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);

	for (const Run& t : runs)
	{
		const float scale = stbtt_ScaleForPixelHeight(&info, t.size);
		const float lineH = static_cast<float>(asc - desc) * scale;
		auto advance = [&](std::uint32_t cp) {
			int aw = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &aw, &lsb);
			return static_cast<float>(aw) * scale;
		};
		const std::vector<std::uint32_t> cps = decode(t.text);
		const float left = t.x, maxX = t.wrapWidth > 0 ? t.x + t.wrapWidth : 1e30f;
		float penX = left, baseY = t.y + static_cast<float>(asc) * scale;
		const int cx0 = std::max(0, static_cast<int>(std::floor(t.clipX0))), cy0 = std::max(0, static_cast<int>(std::floor(t.clipY0)));
		const int cx1 = std::min(width, static_cast<int>(std::ceil(std::min(t.clipX1, 1e8f)))), cy1 = std::min(height, static_cast<int>(std::ceil(std::min(t.clipY1, 1e8f))));
		const float ar = static_cast<float>(t.colour & 0xFF), ag = static_cast<float>((t.colour >> 8) & 0xFF), ab = static_cast<float>((t.colour >> 16) & 0xFF);
		const float aa = static_cast<float>((t.colour >> 24) & 0xFF) / 255.0f;

		auto drawGlyph = [&](std::uint32_t cp, float x) {
			int w = 0, h = 0, xo = 0, yo = 0;
			unsigned char* bm = stbtt_GetCodepointBitmap(&info, scale, scale, static_cast<int>(cp), &w, &h, &xo, &yo);
			if (!bm) return;
			const int gx = static_cast<int>(std::lround(x)) + xo, gy = static_cast<int>(std::lround(baseY)) + yo;
			for (int row = 0; row < h; ++row)
			{
				const int py = gy + row;
				if (py < cy0 || py >= cy1) continue;
				for (int col = 0; col < w; ++col)
				{
					const int px = gx + col;
					if (px < cx0 || px >= cx1) continue;
					const float a = static_cast<float>(bm[row * w + col]) / 255.0f * aa;
					if (a <= 0.0f) continue;
					std::uint8_t* d = &rgba[(static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) * 4];
					d[0] = static_cast<std::uint8_t>(d[0] + (ar - d[0]) * a);
					d[1] = static_cast<std::uint8_t>(d[1] + (ag - d[1]) * a);
					d[2] = static_cast<std::uint8_t>(d[2] + (ab - d[2]) * a);
				}
			}
			stbtt_FreeBitmap(bm, nullptr);
		};

		for (std::size_t i = 0; i < cps.size();)
		{
			if (cps[i] == '\n') { penX = left; baseY += lineH; ++i; continue; }
			std::size_t j = i;
			float w = 0;
			while (j < cps.size() && cps[j] != ' ' && cps[j] != '\n') w += advance(cps[j++]);
			if (penX + w > maxX && penX > left) { penX = left; baseY += lineH; }
			float x = penX;
			for (std::size_t k = i; k < j; ++k) { drawGlyph(cps[k], x); x += advance(cps[k]); }
			penX += w;
			while (j < cps.size() && cps[j] == ' ') { penX += advance(' '); ++j; }
			i = j;
		}
	}
	return true;
}

} // namespace PlannerFontText
