#include <Hpak/SimpleTextureMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <algorithm>
#include <cmath>

namespace HE
{

namespace
{

bool nearlyEqual(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// Walks one material graph and fills a SimpleTextureMaterial, or stops at the first form it
// does not know. Each reader answers false after setting `why`.
class GraphReader
{
public:
	GraphReader(const MaterialAsset& mat, const MaterialGraph& g, std::string* why)
		: m_mat(mat), m_g(g), m_why(why) {}

	bool read(SimpleTextureMaterial& r)
	{
		const MatGraphNode* output = nullptr;
		for (const MatGraphNode& n : m_g.nodes)
			if (n.type == MatNodeType::Output) { output = &n; break; }
		if (!output) return fail("its graph has no Output node");
		if (static_cast<int>(output->p[1]) != static_cast<int>(MatBlendMode::Opaque))
			return fail("its blend mode is not opaque");
		if (static_cast<int>(output->p[3]) != 0)
			return fail("it is not a Surface material");

		const auto& pins = matNodeDesc(MatNodeType::Output).inputs;
		const auto pinName = [&](int pin) { return std::string(pins[static_cast<size_t>(pin)].name); };
		const auto linked = [&](int pin) { return into(output->id, pin); };

		for (const int pin : { kMatOutputOpacityPin, kMatOutputWPOPin, kMatOutputDisplacementPin })
			if (linked(pin)) return fail("its graph drives " + pinName(pin));

		// Unconnected pins keep the Output node's own defaults; the constants below start there.
		r.baseColor.factor[0] = r.baseColor.factor[1] = r.baseColor.factor[2] =
			pins[kMatOutputBaseColorPin].def;
		r.metallic.factor  = pins[kMatOutputMetallicPin].def;
		r.roughness.factor = pins[kMatOutputRoughnessPin].def;
		r.ao.factor        = pins[kMatOutputAOPin].def;
		r.specular         = pins[kMatOutputSpecularPin].def;
		r.emissive.factor[0] = r.emissive.factor[1] = r.emissive.factor[2] =
			pins[kMatOutputEmissivePin].def;

		if (const MatGraphLink* l = linked(kMatOutputBaseColorPin))
			if (!readColor(*l, r.baseColor, pinName(kMatOutputBaseColorPin))) return false;
		if (const MatGraphLink* l = linked(kMatOutputEmissivePin))
			if (!readColor(*l, r.emissive, pinName(kMatOutputEmissivePin))) return false;
		if (const MatGraphLink* l = linked(kMatOutputMetallicPin))
			if (!readScalar(*l, r.metallic, false, pinName(kMatOutputMetallicPin))) return false;
		if (const MatGraphLink* l = linked(kMatOutputRoughnessPin))
			if (!readScalar(*l, r.roughness, false, pinName(kMatOutputRoughnessPin))) return false;
		if (const MatGraphLink* l = linked(kMatOutputAOPin))
			if (!readScalar(*l, r.ao, true, pinName(kMatOutputAOPin))) return false;
		if (const MatGraphLink* l = linked(kMatOutputSpecularPin))
		{
			const MatGraphNode* n = node(l->srcNode);
			if (!n || n->type != MatNodeType::ConstFloat)
				return fail("Specular is not a constant");
			r.specular = n->p[0];
		}
		if (const MatGraphLink* l = linked(kMatOutputNormalPin))
		{
			const MatGraphNode* n = node(l->srcNode);
			if (!n || n->type != MatNodeType::NormalMapSample)
				return fail("Normal is not a normal map");
			if (!sampleTexture(*n, r.normal, /*uvOptional=*/true)) return false;
			r.normalStrength = n->p[0] > 0.0f ? n->p[0] : 1.0f;
		}
		r.lit       = output->p[0] > 0.5f;
		r.fromGraph = true;
		return true;
	}

private:
	bool fail(const std::string& text)
	{
		if (m_why) *m_why = text;
		return false;
	}

	const MatGraphNode* node(int id) const { return m_g.findNode(id); }

	const MatGraphLink* into(int nodeId, int pin) const
	{
		for (const MatGraphLink& l : m_g.links)
			if (l.dstNode == nodeId && l.dstPin == pin) return &l;
		return nullptr;
	}

	// The texture a sample node names: its own path, or the material's slot 0.
	bool textureOf(const MatGraphNode& sample, std::string& path)
	{
		path = sample.s;
		if (path.empty() && !m_mat.texturePaths.empty()) path = m_mat.texturePaths[0];
		if (path.empty()) return fail("a texture sample names no texture");
		return true;
	}

	// Path + UV transform of a Texture Sample or Normal Map node. A Texture Sample whose UV
	// pin is open reads the pin's default, vec2(0): ONE texel, not the mesh UV, so it is not
	// a texture bake candidate. A Normal Map's open UV falls back to the mesh UV.
	bool sampleTexture(const MatGraphNode& sample, TextureRef& t, bool uvOptional)
	{
		if (!textureOf(sample, t.path)) return false;
		const MatGraphLink* uv = into(sample.id, 0);
		if (!uv)
		{
			if (!uvOptional) return fail("a texture sample has no UV connected");
			return true;
		}
		const MatGraphNode* un = node(uv->srcNode);
		if (!un || un->type != MatNodeType::UV || uv->srcPin != 0)
			return fail("a texture UV is computed");
		t.uvScale[0] = un->p[0];  t.uvScale[1] = un->p[1];
		t.uvOffset[0] = un->p[2]; t.uvOffset[1] = un->p[3];
		return true;
	}

	// A constant colour (or a float splatted over rgb).
	static bool constantColor(const MatGraphNode& n, float out[3])
	{
		if (n.type == MatNodeType::ConstColor)
		{
			out[0] = n.p[0]; out[1] = n.p[1]; out[2] = n.p[2];
			return true;
		}
		if (n.type == MatNodeType::ConstFloat)
		{
			out[0] = out[1] = out[2] = n.p[0];
			return true;
		}
		return false;
	}

	bool readColor(const MatGraphLink& l, ColorRole& role, const std::string& pin)
	{
		const MatGraphNode* src = node(l.srcNode);
		if (!src) return fail(pin + " links to a missing node");
		if (constantColor(*src, role.factor)) return true;
		if (src->type == MatNodeType::TextureSample && l.srcPin == 0)
		{
			role.factor[0] = role.factor[1] = role.factor[2] = 1.0f;
			return sampleTexture(*src, role.tex, false);
		}
		if (src->type == MatNodeType::Multiply)
		{
			// Exactly one input is the texture's RGB; the other is a constant (open = 1).
			for (int i = 0; i < 2; ++i)
			{
				const MatGraphLink* a = into(src->id, i);
				const MatGraphLink* b = into(src->id, 1 - i);
				if (!a) continue;
				const MatGraphNode* an = node(a->srcNode);
				if (!an || an->type != MatNodeType::TextureSample || a->srcPin != 0) continue;
				float f[3] = { 1.0f, 1.0f, 1.0f };
				if (b)
				{
					const MatGraphNode* bn = node(b->srcNode);
					if (!bn || !constantColor(*bn, f)) continue;
				}
				role.factor[0] = f[0]; role.factor[1] = f[1]; role.factor[2] = f[2];
				return sampleTexture(*an, role.tex, false);
			}
		}
		return fail(pin + " is not a texture or a constant");
	}

	// One channel of a texture: Split RGBA fed by a Texture Sample's RGB. Output channel =
	// the link's source pin.
	bool channelOf(const MatGraphLink& l, ScalarRole& role, const std::string& pin)
	{
		const MatGraphNode* split = node(l.srcNode);
		const MatGraphLink* in = split && split->type == MatNodeType::SplitRGBA ? into(split->id, 0) : nullptr;
		const MatGraphNode* sample = in && in->srcPin == 0 ? node(in->srcNode) : nullptr;
		if (!sample || sample->type != MatNodeType::TextureSample)
			return fail(pin + " is not a texture channel or a constant");
		if (!sampleTexture(*sample, role.tex, false)) return false;
		role.channel = l.srcPin;
		return true;
	}

	bool readScalar(const MatGraphLink& l, ScalarRole& role, bool isAO, const std::string& pin)
	{
		const MatGraphNode* src = node(l.srcNode);
		if (!src) return fail(pin + " links to a missing node");
		if (src->type == MatNodeType::ConstFloat) { role.factor = src->p[0]; return true; }
		if (src->type == MatNodeType::SplitRGBA)
		{
			role.factor = 1.0f;
			return channelOf(l, role, pin);
		}
		if (!isAO && src->type == MatNodeType::Multiply)
		{
			for (int i = 0; i < 2; ++i)
			{
				const MatGraphLink* a = into(src->id, i);
				const MatGraphLink* b = into(src->id, 1 - i);
				if (!a) continue;
				const MatGraphNode* an = node(a->srcNode);
				if (!an || an->type != MatNodeType::SplitRGBA) continue;
				float f = 1.0f;
				if (b)
				{
					const MatGraphNode* bn = node(b->srcNode);
					if (!bn || bn->type != MatNodeType::ConstFloat) continue;
					f = bn->p[0];
				}
				ScalarRole tmp;
				if (!channelOf(*a, tmp, pin)) return false;
				role.tex = tmp.tex; role.channel = tmp.channel; role.factor = f;
				return true;
			}
		}
		if (isAO && src->type == MatNodeType::Lerp)
		{
			// ao = lerp(1, sample, strength): the importer's form for occlusionStrength < 1.
			const MatGraphLink* a = into(src->id, 0);
			const MatGraphLink* s = into(src->id, 1);
			const MatGraphLink* t = into(src->id, 2);
			const MatGraphNode* an = a ? node(a->srcNode) : nullptr;
			const MatGraphNode* tn = t ? node(t->srcNode) : nullptr;
			float one[3] = { 0.0f, 0.0f, 0.0f };
			const bool startsAtOne = an && constantColor(*an, one)
				&& nearlyEqual(one[0], 1.0f) && nearlyEqual(one[1], 1.0f) && nearlyEqual(one[2], 1.0f);
			if (startsAtOne && s && tn && tn->type == MatNodeType::ConstFloat)
			{
				ScalarRole tmp;
				if (!channelOf(*s, tmp, pin)) return false;
				role.tex = tmp.tex; role.channel = tmp.channel; role.factor = tn->p[0];
				return true;
			}
		}
		return fail(pin + " is not a texture channel or a constant");
	}

	const MaterialAsset&  m_mat;
	const MaterialGraph&  m_g;
	std::string*          m_why;
};

} // namespace

bool TextureRef::identityUv() const
{
	return nearlyEqual(uvScale[0], 1.0f) && nearlyEqual(uvScale[1], 1.0f)
	    && nearlyEqual(uvOffset[0], 0.0f) && nearlyEqual(uvOffset[1], 0.0f);
}

bool TextureRef::sameSampling(const TextureRef& o) const
{
	return path == o.path
	    && nearlyEqual(uvScale[0], o.uvScale[0]) && nearlyEqual(uvScale[1], o.uvScale[1])
	    && nearlyEqual(uvOffset[0], o.uvOffset[0]) && nearlyEqual(uvOffset[1], o.uvOffset[1]);
}

std::vector<std::string> SimpleTextureMaterial::texturePaths() const
{
	std::vector<std::string> out;
	for (const TextureRef* t : { &baseColor.tex, &metallic.tex, &roughness.tex, &ao.tex,
	                             &emissive.tex, &normal })
		if (!t->empty() && std::find(out.begin(), out.end(), t->path) == out.end())
			out.push_back(t->path);
	return out;
}

bool SimpleTextureMaterial::sameShading(const SimpleTextureMaterial& o) const
{
	return nearlyEqual(specular, o.specular) && doubleSided == o.doubleSided && lit == o.lit;
}

bool SimpleTextureMaterial::sameFactors(const SimpleTextureMaterial& o) const
{
	const auto color = [](const ColorRole& a, const ColorRole& b) {
		return nearlyEqual(a.factor[0], b.factor[0]) && nearlyEqual(a.factor[1], b.factor[1])
		    && nearlyEqual(a.factor[2], b.factor[2]);
	};
	return color(baseColor, o.baseColor) && color(emissive, o.emissive)
	    && nearlyEqual(metallic.factor, o.metallic.factor)
	    && nearlyEqual(roughness.factor, o.roughness.factor)
	    && nearlyEqual(ao.factor, o.ao.factor)
	    && nearlyEqual(normalStrength, o.normalStrength);
}

bool classifySimpleTextureMaterial(const MaterialAsset& mat, SimpleTextureMaterial& out,
                                   std::string* whyNot)
{
	const auto fail = [&](const char* text) {
		if (whyNot) *whyNot = text;
		return false;
	};
	if (!mat.parentMaterialPath.empty()) return fail("it is a material instance");
	if (!mat.customShaderVertGlsl.empty()) return fail("it moves vertices");
	if (!mat.customShaderDispGlsl.empty()) return fail("it displaces the surface");
	if (mat.domain != 0) return fail("it is not a Surface material");

	SimpleTextureMaterial r;
	if (!mat.nodeGraphJson.empty())
	{
		MaterialGraph g;
		if (!materialGraphFromJson(mat.nodeGraphJson, g)) return fail("its node graph cannot be read");
		if (whyNot) whyNot->clear();
		GraphReader reader(mat, g, whyNot);
		if (!reader.read(r)) return false;
	}
	else
	{
		// No graph. Hand-written GLSL is the escape hatch: it can do anything.
		if (!mat.customShaderFragGlsl.empty()) return fail("it runs hand-written GLSL");
		if (mat.blendMode != 0) return fail("its blend mode is not opaque");
		if (mat.opacity < 1.0f) return fail("it is translucent");
		if (mat.texturePaths.empty() || mat.texturePaths[0].empty()) return fail("it has no texture");
		r.baseColor.tex.path = mat.texturePaths[0];
		for (int i = 0; i < 3; ++i) r.baseColor.factor[i] = mat.baseColor[i];
		r.metallic.factor  = mat.metallic;
		r.roughness.factor = mat.roughness;
	}
	r.doubleSided = mat.doubleSided;
	if (r.texturePaths().empty()) return fail("it uses no texture");
	out = r;
	return true;
}

} // namespace HE
