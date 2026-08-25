#include "ImGuiSoftwareRaster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace he_ui
{
namespace
{
	// Straight source-over into an 8-bit sRGB buffer, no gamma correction — the
	// same simplification the GPU backends make (they blend in the framebuffer's
	// own space with SRC_ALPHA/ONE_MINUS_SRC_ALPHA), so the result matches what
	// the editor looks like rather than what it would look like if anyone got
	// blending right.
	inline void blend(std::uint8_t* dst, int r, int g, int b, int a)
	{
		if (a <= 0) return;
		if (a >= 255) { dst[0] = std::uint8_t(r); dst[1] = std::uint8_t(g);
		                dst[2] = std::uint8_t(b); dst[3] = 255; return; }
		const int ia = 255 - a;
		dst[0] = std::uint8_t((r * a + dst[0] * ia) / 255);
		dst[1] = std::uint8_t((g * a + dst[1] * ia) / 255);
		dst[2] = std::uint8_t((b * a + dst[2] * ia) / 255);
		dst[3] = std::uint8_t(std::min(255, a + dst[3] * ia / 255));
	}

	struct Tex
	{
		const std::uint8_t* pixels = nullptr;
		int width = 0, height = 0, bpp = 4;

		// Nearest-neighbour on purpose. ImGui's font atlas is sampled 1:1 at the
		// positions it computed, so filtering would only blur glyphs that are
		// already pixel-aligned — and the white pixel every solid rectangle
		// samples is a single texel where interpolation could pick up a
		// neighbouring glyph's edge.
		void sample(float u, float v, int& r, int& g, int& b, int& a) const
		{
			r = g = b = a = 255;
			if (!pixels || width <= 0 || height <= 0) return;
			int x = int(u * float(width));
			int y = int(v * float(height));
			x = std::clamp(x, 0, width - 1);
			y = std::clamp(y, 0, height - 1);
			const std::uint8_t* p = pixels + (std::size_t(y) * width + x) * bpp;
			if (bpp == 1) { r = g = b = 255; a = p[0]; }
			else          { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
		}
	};

	struct Vert
	{
		float x, y, u, v;
		int   r, g, b, a;
	};

	Vert toVert(const ImDrawVert& in, const ImVec2& off, const ImVec2& scale)
	{
		Vert o;
		o.x = (in.pos.x - off.x) * scale.x;
		o.y = (in.pos.y - off.y) * scale.y;
		o.u = in.uv.x;
		o.v = in.uv.y;
		// IM_COL32 packs R in the lowest byte.
		o.r = int((in.col >> IM_COL32_R_SHIFT) & 0xFF);
		o.g = int((in.col >> IM_COL32_G_SHIFT) & 0xFF);
		o.b = int((in.col >> IM_COL32_B_SHIFT) & 0xFF);
		o.a = int((in.col >> IM_COL32_A_SHIFT) & 0xFF);
		return o;
	}

	// One triangle, barycentric, with the clip rect applied per pixel. ImGui's
	// anti-aliasing lives in the vertex ALPHA of its fringe triangles, so
	// interpolating colour and uv is all it takes for text and rounded corners to
	// come out smooth — there is nothing extra to do for it here.
	void triangle(Image& img, const Vert& a, const Vert& b, const Vert& c,
	              const Tex& tex, const ImVec4& clip)
	{
		const float minXf = std::min({ a.x, b.x, c.x });
		const float maxXf = std::max({ a.x, b.x, c.x });
		const float minYf = std::min({ a.y, b.y, c.y });
		const float maxYf = std::max({ a.y, b.y, c.y });

		int minX = std::max(int(std::floor(minXf)), std::max(0, int(std::floor(clip.x))));
		int maxX = std::min(int(std::ceil(maxXf)),  std::min(img.width,  int(std::ceil(clip.z))));
		int minY = std::max(int(std::floor(minYf)), std::max(0, int(std::floor(clip.y))));
		int maxY = std::min(int(std::ceil(maxYf)),  std::min(img.height, int(std::ceil(clip.w))));
		if (minX >= maxX || minY >= maxY) return;

		const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		if (std::fabs(area) < 1e-8f) return;
		const float inv = 1.0f / area;

		for (int y = minY; y < maxY; ++y)
		{
			const float py = float(y) + 0.5f;
			for (int x = minX; x < maxX; ++x)
			{
				const float px = float(x) + 0.5f;
				float w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv;
				float w1 = ((px - a.x) * (c.y - a.y) - (py - a.y) * (c.x - a.x)) * inv;
				const float w2 = 1.0f - w0 - w1;
				// The edge test is deliberately inclusive-ish (a small epsilon):
				// ImGui's rectangles are two triangles sharing an edge, and a
				// strict test leaves a one-pixel seam along every diagonal.
				constexpr float kEps = -1e-4f;
				if (w0 < kEps || w1 < kEps || w2 < kEps) continue;
				// Barycentric order: w2 belongs to a, w1 to b, w0 to c.
				const float ba = w2, bb = w1, bc = w0;

				const float u = a.u * ba + b.u * bb + c.u * bc;
				const float v = a.v * ba + b.v * bb + c.v * bc;
				int tr, tg, tb, ta;
				tex.sample(u, v, tr, tg, tb, ta);

				const int vr = int(a.r * ba + b.r * bb + c.r * bc);
				const int vg = int(a.g * ba + b.g * bb + c.g * bc);
				const int vb = int(a.b * ba + b.b * bb + c.b * bc);
				const int va = int(a.a * ba + b.a * bb + c.a * bc);

				const int cr = std::clamp(vr * tr / 255, 0, 255);
				const int cg = std::clamp(vg * tg / 255, 0, 255);
				const int cb = std::clamp(vb * tb / 255, 0, 255);
				const int ca = std::clamp(va * ta / 255, 0, 255);
				blend(&img.rgba[(std::size_t(y) * img.width + x) * 4], cr, cg, cb, ca);
			}
		}
	}
} // namespace

void Image::pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
                  std::uint8_t& b, std::uint8_t& a) const
{
	r = g = b = a = 0;
	if (x < 0 || y < 0 || x >= width || y >= height) return;
	const std::uint8_t* p = &rgba[(std::size_t(y) * width + x) * 4];
	r = p[0]; g = p[1]; b = p[2]; a = p[3];
}

int Image::inkedPixels(std::uint8_t bgR, std::uint8_t bgG, std::uint8_t bgB) const
{
	int n = 0;
	for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
		// A tolerance rather than an exact compare: the near-black backgrounds
		// the editor draws over its clear colour differ from it by a couple of
		// levels, and counting those as "ink" would make the check meaningless.
		if (std::abs(int(rgba[i]) - int(bgR)) > 6 ||
		    std::abs(int(rgba[i + 1]) - int(bgG)) > 6 ||
		    std::abs(int(rgba[i + 2]) - int(bgB)) > 6)
			++n;
	return n;
}

Image rasterize(const ImDrawData* drawData, int width, int height, std::uint32_t clear)
{
	Image img;
	if (width <= 0 || height <= 0) return img;
	img.width  = width;
	img.height = height;
	img.rgba.assign(std::size_t(width) * height * 4, 0);
	for (std::size_t i = 0; i < img.rgba.size(); i += 4)
	{
		img.rgba[i + 0] = std::uint8_t((clear >> IM_COL32_R_SHIFT) & 0xFF);
		img.rgba[i + 1] = std::uint8_t((clear >> IM_COL32_G_SHIFT) & 0xFF);
		img.rgba[i + 2] = std::uint8_t((clear >> IM_COL32_B_SHIFT) & 0xFF);
		img.rgba[i + 3] = 255;
	}
	if (!drawData || !drawData->Valid) return img;

	// ── Textures ─────────────────────────────────────────────────────────────
	// Since 1.92 ImGui owns its atlas and hands it to the renderer as texture
	// REQUESTS. A backend that ignores them gets no glyphs at all, so honour them
	// the cheapest way a CPU renderer can: the identifier IS the ImTextureData,
	// and there is nothing to upload — the pixels are already in memory.
	if (drawData->Textures)
		for (ImTextureData* tex : *drawData->Textures)
		{
			if (!tex) continue;
			if (tex->Status == ImTextureStatus_WantCreate ||
			    tex->Status == ImTextureStatus_WantUpdates)
			{
				tex->SetTexID(reinterpret_cast<ImTextureID>(tex));
				tex->SetStatus(ImTextureStatus_OK);
			}
		}

	const ImVec2 off   = drawData->DisplayPos;
	const ImVec2 scale = drawData->FramebufferScale;

	for (const ImDrawList* list : drawData->CmdLists)
	{
		const ImDrawVert* vtx = list->VtxBuffer.Data;
		const ImDrawIdx*  idx = list->IdxBuffer.Data;
		for (const ImDrawCmd& cmd : list->CmdBuffer)
		{
			if (cmd.UserCallback)
			{
				// ImDrawCallback_ResetRenderState and any user callback: nothing
				// here keeps render state, so both are correctly a no-op.
				continue;
			}

			ImVec4 clip;
			clip.x = (cmd.ClipRect.x - off.x) * scale.x;
			clip.y = (cmd.ClipRect.y - off.y) * scale.y;
			clip.z = (cmd.ClipRect.z - off.x) * scale.x;
			clip.w = (cmd.ClipRect.w - off.y) * scale.y;
			if (clip.z <= clip.x || clip.w <= clip.y) continue;

			Tex tex;
			if (ImTextureData* td = reinterpret_cast<ImTextureData*>(cmd.GetTexID()))
			{
				tex.pixels = td->Pixels;
				tex.width  = td->Width;
				tex.height = td->Height;
				tex.bpp    = td->BytesPerPixel;
			}

			const ImDrawIdx* tri = idx + cmd.IdxOffset;
			for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
			{
				const Vert a = toVert(vtx[cmd.VtxOffset + tri[i + 0]], off, scale);
				const Vert b = toVert(vtx[cmd.VtxOffset + tri[i + 1]], off, scale);
				const Vert c = toVert(vtx[cmd.VtxOffset + tri[i + 2]], off, scale);
				triangle(img, a, b, c, tex, clip);
			}
		}
	}
	return img;
}

bool writeBmp(const Image& img, const std::string& path)
{
	if (!img.valid()) return false;
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;

	const std::uint32_t pixelBytes = std::uint32_t(img.width) * img.height * 4;
	const std::uint32_t headerSize = 14 + 40;
	const std::uint32_t fileSize   = headerSize + pixelBytes;

	auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
	auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto i32 = [&](std::int32_t v)  { std::fwrite(&v, 4, 1, f); };

	std::fwrite("BM", 1, 2, f);
	u32(fileSize); u16(0); u16(0); u32(headerSize);
	u32(40);
	i32(img.width);
	// Negative height = top-down rows, which is the order the image is already
	// in. Every reader worth the name handles it, and it saves flipping.
	i32(-img.height);
	u16(1); u16(32); u32(0); u32(pixelBytes);
	i32(2835); i32(2835); u32(0); u32(0);

	// BMP is BGRA.
	std::vector<std::uint8_t> row(std::size_t(img.width) * 4);
	for (int y = 0; y < img.height; ++y)
	{
		const std::uint8_t* src = &img.rgba[std::size_t(y) * img.width * 4];
		for (int x = 0; x < img.width; ++x)
		{
			row[x * 4 + 0] = src[x * 4 + 2];
			row[x * 4 + 1] = src[x * 4 + 1];
			row[x * 4 + 2] = src[x * 4 + 0];
			row[x * 4 + 3] = src[x * 4 + 3];
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
	return true;
}

} // namespace he_ui
