#include <Hpak/MeshTextureBake.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

namespace HE::MeshBake
{

float srgbToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c)
{
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

namespace
{

constexpr float kEps = 1e-4f;

uint8_t toByte(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

const std::array<float, 256>& srgbLut()
{
	static const std::array<float, 256> lut = [] {
		std::array<float, 256> t{};
		for (int i = 0; i < 256; ++i) t[static_cast<size_t>(i)] = srgbToLinear(static_cast<float>(i) / 255.0f);
		return t;
	}();
	return lut;
}

// One texel with the image's wrap mode (repeat); rgb are decoded to linear when `decode`.
void texel(const Image& img, int64_t x, int64_t y, bool decode, float out[4])
{
	const int64_t w = img.width, h = img.height;
	const size_t xi = static_cast<size_t>(((x % w) + w) % w);
	const size_t yi = static_cast<size_t>(((y % h) + h) % h);
	const uint8_t* p = &img.rgba[(yi * img.width + xi) * 4];
	for (int c = 0; c < 3; ++c)
		out[c] = decode ? srgbLut()[p[c]] : static_cast<float>(p[c]) / 255.0f;
	out[3] = static_cast<float>(p[3]) / 255.0f;
}

// Bilinear, repeat. At a texel centre it returns that texel exactly.
void bilinear(const Image& img, float u, float v, bool decode, float out[4])
{
	const float x = u * static_cast<float>(img.width) - 0.5f;
	const float y = v * static_cast<float>(img.height) - 0.5f;
	const float fx0 = std::floor(x), fy0 = std::floor(y);
	const float tx = x - fx0, ty = y - fy0;
	const int64_t x0 = static_cast<int64_t>(fx0), y0 = static_cast<int64_t>(fy0);
	float a[4], b[4], c[4], d[4];
	texel(img, x0,     y0,     decode, a);
	texel(img, x0 + 1, y0,     decode, b);
	texel(img, x0,     y0 + 1, decode, c);
	texel(img, x0 + 1, y0 + 1, decode, d);
	for (int k = 0; k < 4; ++k)
		out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}

// Everything the bake knows about one merged section.
struct Prep
{
	uint32_t inputSection = 0;
	const SectionInput* s = nullptr;
	float u0 = 0, u1 = 1, v0 = 0, v1 = 1;      // the mesh-UV box the rect covers
	int   innerW = 0, innerH = 0;              // the rect in atlas texels, without padding
	int   cellX = 0, cellY = 0, cellW = 0, cellH = 0;
	const Image* base = nullptr;
	const Image* emissive = nullptr;
	const Image* normal = nullptr;
	const Image* orm[3] = { nullptr, nullptr, nullptr };   // ao, roughness, metallic
};

int roundUp(int v, int to) { return (v + to - 1) / to * to; }

bool shelfPack(std::vector<Prep*>& order, int atlasW, int atlasH)
{
	int x = 0, y = 0, shelfH = 0;
	for (Prep* p : order)
	{
		if (p->cellW > atlasW) return false;
		if (x + p->cellW > atlasW) { x = 0; y += shelfH; shelfH = 0; }
		if (y + p->cellH > atlasH) return false;
		p->cellX = x; p->cellY = y;
		x += p->cellW;
		shelfH = std::max(shelfH, p->cellH);
	}
	return true;
}

Result fail(const std::string& why)
{
	Result r;
	r.why = why;
	return r;
}

} // namespace

Result bake(const MeshInput& mesh, const ImageLookup& images, const Options& opt)
{
	const size_t vertexCount = mesh.vertices.size() / 3;
	if (vertexCount == 0 || mesh.indices.empty()) return fail("the mesh has no geometry");
	if (mesh.uvs.size() != vertexCount * 2) return fail("the mesh has no texture coordinates");
	if (!mesh.normals.empty() && mesh.normals.size() != vertexCount * 3) return fail("the mesh normals do not match its vertices");
	const bool merge = opt.mode == Mode::Merge;

	// ── Which sections are merged ────────────────────────────────────────────────
	// The first simple section sets the shading every merged section must share
	// (specular, lit, side-ness); simple sections that differ stay as they are.
	std::vector<Prep> preps;
	const SimpleTextureMaterial* lead = nullptr;
	std::vector<uint32_t> keptIn;
	for (uint32_t i = 0; i < mesh.sections.size(); ++i)
	{
		const SectionInput& s = mesh.sections[i];
		if (s.indexCount == 0) continue;
		if (static_cast<uint64_t>(s.indexOffset) + s.indexCount > mesh.indices.size())
			return fail("a section reaches past the index buffer");
		if (s.simple && (!lead || s.material.sameShading(*lead)))
		{
			if (!lead) lead = &s.material;
			Prep p; p.inputSection = i; p.s = &s;
			preps.push_back(p);
		}
		else
			keptIn.push_back(i);
	}
	if (preps.size() < 2) return fail("fewer than two simple sections");

	// ── Images and the UV box of every merged section ───────────────────────────
	const auto fetch = [&](const TextureRef& t, const Image*& out, std::string& why) -> bool
	{
		out = nullptr;
		if (t.empty()) return true;
		const Image* img = images ? images(t.path) : nullptr;
		if (!img || !img->valid()) { why = "texture '" + t.path + "' is not available"; return false; }
		out = img;
		return true;
	};
	for (Prep& p : preps)
	{
		const SimpleTextureMaterial& m = p.s->material;
		std::string why;
		if (!fetch(m.baseColor.tex, p.base, why) || !fetch(m.emissive.tex, p.emissive, why)
		    || !fetch(m.normal, p.normal, why) || !fetch(m.ao.tex, p.orm[0], why)
		    || !fetch(m.roughness.tex, p.orm[1], why) || !fetch(m.metallic.tex, p.orm[2], why))
			return fail(why);

		p.u0 = p.v0 = 1e30f; p.u1 = p.v1 = -1e30f;
		for (uint32_t k = 0; k < p.s->indexCount; ++k)
		{
			const uint32_t vi = mesh.indices[p.s->indexOffset + k];
			if (vi >= vertexCount) return fail("an index points past the vertices");
			const float u = mesh.uvs[vi * 2], v = mesh.uvs[vi * 2 + 1];
			p.u0 = std::min(p.u0, u); p.u1 = std::max(p.u1, u);
			p.v0 = std::min(p.v0, v); p.v1 = std::max(p.v1, v);
		}
	}

	// ── What each group is ───────────────────────────────────────────────────────
	const auto anyTex = [&](auto&& pick) {
		for (const Prep& p : preps) if (pick(p) != nullptr) return true;
		return false;
	};
	const bool baseTex     = anyTex([](const Prep& p) { return p.base; });
	const bool emissiveTex = anyTex([](const Prep& p) { return p.emissive; });
	const bool normalTex   = anyTex([](const Prep& p) { return p.normal; });
	const bool ormTex      = anyTex([](const Prep& p) { return p.orm[0] ? p.orm[0] : p.orm[1] ? p.orm[1] : p.orm[2]; });

	const auto colorDiffers = [&](auto&& role) {
		const ColorRole& a = role(preps[0].s->material);
		for (const Prep& p : preps)
		{
			const ColorRole& b = role(p.s->material);
			for (int c = 0; c < 3; ++c) if (std::fabs(a.factor[c] - b.factor[c]) > kEps) return true;
		}
		return false;
	};
	const auto scalarDiffers = [&](auto&& role) {
		const float a = role(preps[0].s->material).factor;
		for (const Prep& p : preps) if (std::fabs(a - role(p.s->material).factor) > kEps) return true;
		return false;
	};
	const auto baseRole = [](const SimpleTextureMaterial& m) -> const ColorRole& { return m.baseColor; };
	const auto emisRole = [](const SimpleTextureMaterial& m) -> const ColorRole& { return m.emissive; };
	const auto aoRole   = [](const SimpleTextureMaterial& m) -> const ScalarRole& { return m.ao; };
	const auto rouRole  = [](const SimpleTextureMaterial& m) -> const ScalarRole& { return m.roughness; };
	const auto metRole  = [](const SimpleTextureMaterial& m) -> const ScalarRole& { return m.metallic; };

	const bool baseAtlas     = baseTex || colorDiffers(baseRole);
	const bool emissiveAtlas = emissiveTex || colorDiffers(emisRole);
	const bool ormAtlas      = ormTex || scalarDiffers(aoRole) || scalarDiffers(rouRole) || scalarDiffers(metRole);
	const bool normalAtlas   = normalTex;

	// Colour space of the colour groups: every source of a group must agree, because the
	// atlas is one texture with one sampling mode.
	bool baseSrgb = false, emissiveSrgb = false;
	{
		bool haveB = false, haveE = false;
		for (const Prep& p : preps)
		{
			if (p.base)
			{
				if (haveB && baseSrgb != p.base->srgb) return fail("the base color textures differ in color space");
				baseSrgb = p.base->srgb; haveB = true;
			}
			if (p.emissive)
			{
				if (haveE && emissiveSrgb != p.emissive->srgb) return fail("the emissive textures differ in color space");
				emissiveSrgb = p.emissive->srgb; haveE = true;
			}
		}
	}

	// ── Lossless eligibility ─────────────────────────────────────────────────────
	// Merge copies texels, so it needs: every texture read straight through the mesh UV (no
	// tiling or offset), the UV box inside one tile, one texture size per section, and one
	// factor per group (a texel-less section is then filled with its constant, which only
	// works when the shared factor is 1).
	float sharedFactorBase[3] = { 1, 1, 1 }, sharedFactorEmissive[3] = { 1, 1, 1 };
	float sharedMetal = 1, sharedRough = 1, sharedAo = 1, sharedNormal = 1;
	if (merge)
	{
		for (Prep& p : preps)
		{
			const SimpleTextureMaterial& m = p.s->material;
			for (const TextureRef* t : { &m.baseColor.tex, &m.emissive.tex, &m.normal, &m.ao.tex,
			                             &m.roughness.tex, &m.metallic.tex })
				if (!t->empty() && !t->identityUv()) return fail("a texture uses tiling or an offset");
			if (p.u0 < -kEps || p.v0 < -kEps || p.u1 > 1 + kEps || p.v1 > 1 + kEps)
				return fail("a section's UVs tile");
			const Image* any[6] = { p.base, p.emissive, p.normal, p.orm[0], p.orm[1], p.orm[2] };
			const Image* first = nullptr;
			for (const Image* img : any)
			{
				if (!img) continue;
				if (!first) first = img;
				else if (img->width != first->width || img->height != first->height)
					return fail("a section's textures differ in size");
			}
		}
		// Per group: textured sections share one factor; untextured ones need factor 1 to be filled.
		const auto colorFactor = [&](auto&& role, auto&& image, float out[3]) -> const char* {
			bool have = false;
			for (const Prep& p : preps)
			{
				const ColorRole& r = role(p.s->material);
				if (!image(p)) continue;
				if (have && (std::fabs(out[0] - r.factor[0]) > kEps || std::fabs(out[1] - r.factor[1]) > kEps
				             || std::fabs(out[2] - r.factor[2]) > kEps))
					return "textured sections have different color factors";
				out[0] = r.factor[0]; out[1] = r.factor[1]; out[2] = r.factor[2]; have = true;
			}
			if (have)
				for (const Prep& p : preps)
					if (!image(p))
					{
						if (std::fabs(out[0] - 1) > kEps || std::fabs(out[1] - 1) > kEps || std::fabs(out[2] - 1) > kEps)
							return "a flat-colored section cannot be filled beside a tinted texture";
					}
			return nullptr;
		};
		if (const char* e = colorFactor(baseRole, [](const Prep& p) { return p.base; }, sharedFactorBase)) return fail(e);
		if (const char* e = colorFactor(emisRole, [](const Prep& p) { return p.emissive; }, sharedFactorEmissive)) return fail(e);
		const auto scalarFactor = [&](auto&& role, int slot, float& out) -> const char* {
			bool have = false;
			for (const Prep& p : preps)
			{
				if (!p.orm[slot]) continue;
				const float f = role(p.s->material).factor;
				if (have && std::fabs(out - f) > kEps) return "textured sections have different factors";
				out = f; have = true;
			}
			if (have)
				for (const Prep& p : preps)
					if (!p.orm[slot] && std::fabs(out - 1) > kEps)
						return "a constant section cannot be filled beside a scaled texture";
			return nullptr;
		};
		if (const char* e = scalarFactor(aoRole, 0, sharedAo)) return fail(e);
		if (const char* e = scalarFactor(rouRole, 1, sharedRough)) return fail(e);
		if (const char* e = scalarFactor(metRole, 2, sharedMetal)) return fail(e);
		bool haveN = false;
		for (const Prep& p : preps)
		{
			if (!p.normal) continue;
			if (haveN && std::fabs(sharedNormal - p.s->material.normalStrength) > kEps)
				return fail("textured sections have different normal strengths");
			sharedNormal = p.s->material.normalStrength; haveN = true;
		}
	}

	// ── Rect sizes and the pack ──────────────────────────────────────────────────
	const int pad = static_cast<int>(opt.padding);
	// Texel density of a section in atlas texels per mesh-UV unit.
	const auto density = [&](const Prep& p, float& dU, float& dV)
	{
		dU = dV = 0.0f;
		const SimpleTextureMaterial& m = p.s->material;
		const struct { const Image* img; const TextureRef* t; } reads[6] = {
			{ p.base, &m.baseColor.tex }, { p.emissive, &m.emissive.tex }, { p.normal, &m.normal },
			{ p.orm[0], &m.ao.tex }, { p.orm[1], &m.roughness.tex }, { p.orm[2], &m.metallic.tex } };
		for (const auto& rd : reads)
		{
			if (!rd.img) continue;
			dU = std::max(dU, static_cast<float>(rd.img->width)  * std::fabs(rd.t->uvScale[0]));
			dV = std::max(dV, static_cast<float>(rd.img->height) * std::fabs(rd.t->uvScale[1]));
		}
	};

	std::vector<Prep*> order;
	for (Prep& p : preps) order.push_back(&p);
	int atlasW = 0, atlasH = 0;
	float usedScale = 1.0f;
	{
		static const float kScales[] = { 1.0f, 0.75f, 0.5f, 0.375f, 0.25f };
		bool packed = false;
		for (const float scale : kScales)
		{
			if (scale < opt.minScale - kEps) break;
			if (merge && scale < 1.0f) break;
			for (Prep& p : preps)
			{
				float dU, dV; density(p, dU, dV);
				if (merge)
				{
					// Texel-aligned box, so a rect texel IS a source texel.
					const Image* img = p.base ? p.base : p.emissive ? p.emissive : p.normal ? p.normal
					                 : p.orm[0] ? p.orm[0] : p.orm[1] ? p.orm[1] : p.orm[2];
					const float w = static_cast<float>(img->width), h = static_cast<float>(img->height);
					const float x0 = std::floor(std::max(p.u0, 0.0f) * w + kEps), x1 = std::ceil(std::min(p.u1, 1.0f) * w - kEps);
					const float y0 = std::floor(std::max(p.v0, 0.0f) * h + kEps), y1 = std::ceil(std::min(p.v1, 1.0f) * h - kEps);
					p.innerW = std::max(1, static_cast<int>(x1 - x0));
					p.innerH = std::max(1, static_cast<int>(y1 - y0));
					p.u0 = x0 / w; p.u1 = (x0 + static_cast<float>(p.innerW)) / w;
					p.v0 = y0 / h; p.v1 = (y0 + static_cast<float>(p.innerH)) / h;
				}
				else
				{
					p.innerW = std::max(4, static_cast<int>(std::ceil((p.u1 - p.u0) * dU * scale)));
					p.innerH = std::max(4, static_cast<int>(std::ceil((p.v1 - p.v0) * dV * scale)));
					if (p.u1 - p.u0 < 1e-6f) p.u1 = p.u0 + 1e-3f;
					if (p.v1 - p.v0 < 1e-6f) p.v1 = p.v0 + 1e-3f;
				}
				p.cellW = roundUp(p.innerW + 2 * pad, 4);
				p.cellH = roundUp(p.innerH + 2 * pad, 4);
			}
			std::sort(order.begin(), order.end(), [](const Prep* a, const Prep* b) {
				return a->cellH != b->cellH ? a->cellH > b->cellH : a->cellW > b->cellW; });
			// Smallest power-of-two atlas, by area then squareness, that fits.
			std::vector<std::pair<int, int>> sizes;
			for (int w = 64; w <= static_cast<int>(opt.maxSize); w *= 2)
				for (int h = 64; h <= static_cast<int>(opt.maxSize); h *= 2)
					sizes.emplace_back(w, h);
			std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) {
				const long aa = long(a.first) * a.second, bb = long(b.first) * b.second;
				return aa != bb ? aa < bb : std::abs(a.first - a.second) < std::abs(b.first - b.second); });
			for (const auto& [w, h] : sizes)
				if (shelfPack(order, w, h)) { atlasW = w; atlasH = h; packed = true; break; }
			if (packed) { usedScale = scale; break; }
		}
		if (!packed)
			return fail(merge ? "the textures do not fit one atlas at full size"
			                  : "the atlas would need less than a quarter of the texture detail");
	}
	(void)usedScale;

	// ── The atlases ──────────────────────────────────────────────────────────────
	Result r;
	r.lossless = merge;
	const size_t pixels = static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH);
	const auto makeImage = [&](bool srgb, uint8_t a, uint8_t b, uint8_t c) {
		Image im; im.width = static_cast<uint32_t>(atlasW); im.height = static_cast<uint32_t>(atlasH); im.srgb = srgb;
		im.rgba.resize(pixels * 4);
		for (size_t i = 0; i < pixels; ++i) { im.rgba[i*4] = a; im.rgba[i*4+1] = b; im.rgba[i*4+2] = c; im.rgba[i*4+3] = 255; }
		return im;
	};
	if (baseAtlas)     r.base.image     = makeImage(baseSrgb, 0, 0, 0);
	if (emissiveAtlas) r.emissive.image = makeImage(emissiveSrgb, 0, 0, 0);
	if (ormAtlas)      r.orm.image      = makeImage(false, 255, 255, 0);
	if (normalAtlas)   r.normal.image   = makeImage(false, 128, 128, 255);

	for (const Prep& p : preps)
	{
		const SimpleTextureMaterial& m = p.s->material;
		const float spanU = p.u1 - p.u0, spanV = p.v1 - p.v0;
		const auto step = [&](const Image* img, const TextureRef& t) {
			// Source texels one atlas pixel covers: how many sub-samples a clean average needs.
			if (!img || merge) return 1;
			const float tpx = std::max(static_cast<float>(img->width)  * std::fabs(t.uvScale[0]) * spanU / static_cast<float>(p.innerW),
			                           static_cast<float>(img->height) * std::fabs(t.uvScale[1]) * spanV / static_cast<float>(p.innerH));
			return std::clamp(static_cast<int>(std::ceil(tpx)), 1, 4);
		};
		// One supersampled read of `img` through `t` at atlas pixel (px, py) of this cell.
		const auto read = [&](const Image* img, const TextureRef& t, int px, int py, int k, bool decode, float out[4]) {
			float acc[4] = { 0, 0, 0, 0 };
			for (int sy = 0; sy < k; ++sy)
				for (int sx = 0; sx < k; ++sx)
				{
					const float lx = static_cast<float>(px - pad) + (static_cast<float>(sx) + 0.5f) / static_cast<float>(k);
					const float ly = static_cast<float>(py - pad) + (static_cast<float>(sy) + 0.5f) / static_cast<float>(k);
					const float u = p.u0 + lx / static_cast<float>(p.innerW) * spanU;
					const float v = p.v0 + ly / static_cast<float>(p.innerH) * spanV;
					float s[4];
					bilinear(*img, u * t.uvScale[0] + t.uvOffset[0], v * t.uvScale[1] + t.uvOffset[1], decode, s);
					for (int c = 0; c < 4; ++c) acc[c] += s[c];
				}
			for (int c = 0; c < 4; ++c) out[c] = acc[c] / static_cast<float>(k * k);
		};
		const int kBase = step(p.base, m.baseColor.tex), kEmis = step(p.emissive, m.emissive.tex),
		          kNorm = step(p.normal, m.normal), kAo = step(p.orm[0], m.ao.tex),
		          kRou = step(p.orm[1], m.roughness.tex), kMet = step(p.orm[2], m.metallic.tex);

		for (int py = 0; py < p.cellH; ++py)
			for (int px = 0; px < p.cellW; ++px)
			{
				const size_t at = (static_cast<size_t>(p.cellY + py) * static_cast<size_t>(atlasW) + static_cast<size_t>(p.cellX + px)) * 4;

				// A colour role: a texel times its factor (folded) or the raw texel / flat fill.
				const auto colour = [&](const Image* img, const ColorRole& role, int k, bool srgb, const float shared[3], Image& dst) {
					float c[3];
					if (img)
					{
						float s[4];
						read(img, role.tex, px, py, k, srgb, s);
						for (int i = 0; i < 3; ++i) c[i] = merge ? s[i] : s[i] * role.factor[i];
					}
					else
						for (int i = 0; i < 3; ++i) c[i] = role.factor[i];
					(void)shared;
					for (int i = 0; i < 3; ++i)
						dst.rgba[at + static_cast<size_t>(i)] = toByte(srgb ? linearToSrgb(c[i]) : c[i]);
				};
				if (baseAtlas)     colour(p.base,     m.baseColor, kBase, baseSrgb,     sharedFactorBase,     r.base.image);
				if (emissiveAtlas) colour(p.emissive, m.emissive,  kEmis, emissiveSrgb, sharedFactorEmissive, r.emissive.image);

				if (ormAtlas)
				{
					const ScalarRole* roles[3] = { &m.ao, &m.roughness, &m.metallic };
					const int ks[3] = { kAo, kRou, kMet };
					for (int c = 0; c < 3; ++c)
					{
						float v;
						if (p.orm[c])
						{
							float s[4];
							read(p.orm[c], roles[c]->tex, px, py, ks[c], false, s);
							const float x = s[std::clamp(roles[c]->channel, 0, 3)];
							v = merge ? x : (c == 0 ? 1.0f + roles[c]->factor * (x - 1.0f) : x * roles[c]->factor);
						}
						else
							v = roles[c]->factor;
						r.orm.image.rgba[at + static_cast<size_t>(c)] = toByte(v);
					}
				}
				if (normalAtlas && p.normal)
				{
					float s[4];
					read(p.normal, m.normal, px, py, kNorm, false, s);
					if (merge)
						for (int c = 0; c < 3; ++c) r.normal.image.rgba[at + static_cast<size_t>(c)] = toByte(s[c]);
					else
					{
						float n[3] = { s[0] * 2 - 1, s[1] * 2 - 1, s[2] * 2 - 1 };
						n[0] *= m.normalStrength; n[1] *= m.normalStrength;
						const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
						for (int c = 0; c < 3; ++c)
							r.normal.image.rgba[at + static_cast<size_t>(c)] = toByte((len > 1e-6f ? n[c] / len : (c == 2 ? 1.0f : 0.0f)) * 0.5f + 0.5f);
					}
				}
			}
	}

	// ── The merged material ──────────────────────────────────────────────────────
	SimpleTextureMaterial& mm = r.material;
	mm = *lead;                                  // specular, lit, side-ness
	mm.baseColor = ColorRole{}; mm.emissive = ColorRole{ {}, { 0, 0, 0 } };
	mm.metallic = ScalarRole{ {}, 0, 0.0f }; mm.roughness = ScalarRole{ {}, 0, 0.5f }; mm.ao = ScalarRole{ {}, 0, 1.0f };
	mm.normal = TextureRef{}; mm.normalStrength = 1.0f;
	const auto setColor = [&](ColorRole& role, bool atlas, const char* name, const float factor[3], const ColorRole& constant) {
		if (atlas) { role.tex.path = name; role.factor[0] = factor[0]; role.factor[1] = factor[1]; role.factor[2] = factor[2]; }
		else       { role.factor[0] = constant.factor[0]; role.factor[1] = constant.factor[1]; role.factor[2] = constant.factor[2]; }
	};
	const float one3[3] = { 1, 1, 1 };
	setColor(mm.baseColor, baseAtlas, kBakedBase,     merge ? sharedFactorBase     : one3, lead->baseColor);
	setColor(mm.emissive,  emissiveAtlas, kBakedEmissive, merge ? sharedFactorEmissive : one3, lead->emissive);
	if (ormAtlas)
	{
		mm.ao.tex.path = kBakedOrm;        mm.ao.channel = 0;        mm.ao.factor = merge ? sharedAo : 1.0f;
		mm.roughness.tex.path = kBakedOrm; mm.roughness.channel = 1; mm.roughness.factor = merge ? sharedRough : 1.0f;
		mm.metallic.tex.path = kBakedOrm;  mm.metallic.channel = 2;  mm.metallic.factor = merge ? sharedMetal : 1.0f;
	}
	else
	{
		mm.ao.factor = lead->ao.factor; mm.roughness.factor = lead->roughness.factor; mm.metallic.factor = lead->metallic.factor;
	}
	if (normalAtlas) { mm.normal.path = kBakedNormal; mm.normalStrength = merge ? sharedNormal : 1.0f; }
	r.base.atlas = baseAtlas; r.emissive.atlas = emissiveAtlas; r.orm.atlas = ormAtlas; r.normal.atlas = normalAtlas;
	for (int c = 0; c < 3; ++c) { r.base.value[c] = lead->baseColor.factor[c]; r.emissive.value[c] = lead->emissive.factor[c]; }

	// ── The geometry ─────────────────────────────────────────────────────────────
	// Merged sections first, in input order, then the kept ones; every section gets its own
	// copy of the vertices it uses, so a vertex two sections shared can carry two UVs.
	r.mergedSections = static_cast<uint32_t>(preps.size());
	const auto emitSection = [&](uint32_t indexOffset, uint32_t indexCount, const Prep* prep)
	{
		std::unordered_map<uint32_t, uint32_t> remap;
		for (uint32_t k = 0; k < indexCount; ++k)
		{
			const uint32_t old = mesh.indices[indexOffset + k];
			auto it = remap.find(old);
			if (it == remap.end())
			{
				it = remap.emplace(old, static_cast<uint32_t>(r.vertices.size() / 3)).first;
				for (int c = 0; c < 3; ++c) r.vertices.push_back(mesh.vertices[old * 3 + static_cast<uint32_t>(c)]);
				if (!mesh.normals.empty())
					for (int c = 0; c < 3; ++c) r.normals.push_back(mesh.normals[old * 3 + static_cast<uint32_t>(c)]);
				float u = mesh.uvs[old * 2], v = mesh.uvs[old * 2 + 1];
				if (prep)
				{
					const float lx = (u - prep->u0) / (prep->u1 - prep->u0) * static_cast<float>(prep->innerW);
					const float ly = (v - prep->v0) / (prep->v1 - prep->v0) * static_cast<float>(prep->innerH);
					u = (static_cast<float>(prep->cellX + pad) + lx) / static_cast<float>(atlasW);
					v = (static_cast<float>(prep->cellY + pad) + ly) / static_cast<float>(atlasH);
				}
				r.uvs.push_back(u); r.uvs.push_back(v);
			}
			r.indices.push_back(it->second);
		}
	};
	for (const Prep& p : preps) emitSection(p.s->indexOffset, p.s->indexCount, &p);
	r.mergedIndexCount = static_cast<uint32_t>(r.indices.size());
	for (const uint32_t i : keptIn)
	{
		const SectionInput& s = mesh.sections[i];
		Result::Kept kept; kept.inputSection = i; kept.indexOffset = static_cast<uint32_t>(r.indices.size());
		emitSection(s.indexOffset, s.indexCount, nullptr);
		kept.indexCount = s.indexCount;
		r.keptSections.push_back(kept);
	}
	r.ok = true;
	return r;
}

} // namespace HE::MeshBake
