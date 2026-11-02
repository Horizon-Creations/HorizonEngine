#include <Hpak/SimpleTextureMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <cmath>
#include <vector>

namespace HE
{

namespace
{

bool setWhy(std::string* why, const std::string& text)
{
	if (why) *why = text;
	return false;
}

// The link feeding input pin `pin` of node `node`, or null.
const MatGraphLink* linkInto(const MaterialGraph& g, int node, int pin)
{
	for (const MatGraphLink& l : g.links)
		if (l.dstNode == node && l.dstPin == pin) return &l;
	return nullptr;
}

bool nearlyEqual(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// A tint source: a Color constant (rgb) or a Float constant (splatted).
bool constantTint(const MatGraphNode& n, float out[3])
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

bool classifyGraph(const MaterialAsset& mat, SimpleTextureMaterial& out, std::string* why)
{
	MaterialGraph g;
	if (!materialGraphFromJson(mat.nodeGraphJson, g))
		return setWhy(why, "its node graph cannot be read");

	const MatGraphNode* output = nullptr;
	for (const MatGraphNode& n : g.nodes)
		if (n.type == MatNodeType::Output) { output = &n; break; }
	if (!output) return setWhy(why, "its graph has no Output node");

	const auto& pins = matNodeDesc(MatNodeType::Output).inputs;
	for (int pin = 1; pin < static_cast<int>(pins.size()); ++pin)
		if (linkInto(g, output->id, pin))
			return setWhy(why, std::string("its graph drives ") + pins[static_cast<size_t>(pin)].name);
	if (static_cast<int>(output->p[1]) != static_cast<int>(MatBlendMode::Opaque))
		return setWhy(why, "its blend mode is not opaque");
	if (static_cast<int>(output->p[3]) != 0)
		return setWhy(why, "it is not a Surface material");

	const MatGraphLink* base = linkInto(g, output->id, kMatOutputBaseColorPin);
	if (!base) return setWhy(why, "Base Color is not connected");
	const MatGraphNode* src = g.findNode(base->srcNode);
	if (!src) return setWhy(why, "Base Color links to a missing node");

	float tint[3] = { 1.0f, 1.0f, 1.0f };
	const MatGraphNode* sample = nullptr;
	if (src->type == MatNodeType::TextureSample && base->srcPin == 0)
	{
		sample = src;
	}
	else if (src->type == MatNodeType::Multiply)
	{
		// Exactly one input is the texture's RGB; the other is a constant (or open = 1).
		for (int i = 0; i < 2; ++i)
		{
			const MatGraphLink* a = linkInto(g, src->id, i);
			const MatGraphLink* b = linkInto(g, src->id, 1 - i);
			if (!a) continue;
			const MatGraphNode* an = g.findNode(a->srcNode);
			if (!an || an->type != MatNodeType::TextureSample || a->srcPin != 0) continue;
			if (b)
			{
				const MatGraphNode* bn = g.findNode(b->srcNode);
				if (!bn || !constantTint(*bn, tint)) continue;
			}
			sample = an;
			break;
		}
		if (!sample) return setWhy(why, "Base Color is not a texture times a constant");
	}
	else
		return setWhy(why, "Base Color is not a plain texture sample");

	std::string path = sample->s;
	if (path.empty() && !mat.texturePaths.empty()) path = mat.texturePaths[0];
	if (path.empty()) return setWhy(why, "its texture sample names no texture");

	float scale[2] = { 1.0f, 1.0f }, offset[2] = { 0.0f, 0.0f };
	if (const MatGraphLink* uv = linkInto(g, sample->id, 0))
	{
		const MatGraphNode* un = g.findNode(uv->srcNode);
		if (!un || un->type != MatNodeType::UV || uv->srcPin != 0)
			return setWhy(why, "its texture UV is computed");
		scale[0] = un->p[0]; scale[1] = un->p[1];
		offset[0] = un->p[2]; offset[1] = un->p[3];
	}

	out.albedoPath = path;
	for (int i = 0; i < 3; ++i) out.tint[i] = tint[i];
	for (int i = 0; i < 2; ++i) { out.uvScale[i] = scale[i]; out.uvOffset[i] = offset[i]; }
	out.metallic  = pins[kMatOutputMetallicPin].def;
	out.roughness = pins[kMatOutputRoughnessPin].def;
	out.lit       = output->p[0] > 0.5f;
	out.fromGraph = true;
	return true;
}

} // namespace

bool SimpleTextureMaterial::sameShading(const SimpleTextureMaterial& o) const
{
	return nearlyEqual(metallic, o.metallic) && nearlyEqual(roughness, o.roughness)
	    && doubleSided == o.doubleSided && lit == o.lit;
}

bool SimpleTextureMaterial::sameTint(const SimpleTextureMaterial& o) const
{
	return nearlyEqual(tint[0], o.tint[0]) && nearlyEqual(tint[1], o.tint[1])
	    && nearlyEqual(tint[2], o.tint[2]);
}

bool SimpleTextureMaterial::identityUv() const
{
	return nearlyEqual(uvScale[0], 1.0f) && nearlyEqual(uvScale[1], 1.0f)
	    && nearlyEqual(uvOffset[0], 0.0f) && nearlyEqual(uvOffset[1], 0.0f);
}

bool classifySimpleTextureMaterial(const MaterialAsset& mat, SimpleTextureMaterial& out,
                                   std::string* whyNot)
{
	if (!mat.parentMaterialPath.empty())
		return setWhy(whyNot, "it is a material instance");
	if (!mat.customShaderVertGlsl.empty())
		return setWhy(whyNot, "it moves vertices");
	if (!mat.customShaderDispGlsl.empty())
		return setWhy(whyNot, "it displaces the surface");
	if (mat.domain != 0)
		return setWhy(whyNot, "it is not a Surface material");

	SimpleTextureMaterial r;
	r.doubleSided = mat.doubleSided;
	if (!mat.nodeGraphJson.empty())
	{
		if (!classifyGraph(mat, r, whyNot)) return false;
	}
	else
	{
		// No graph. Hand-written GLSL is the escape hatch: it can do anything.
		if (!mat.customShaderFragGlsl.empty())
			return setWhy(whyNot, "it runs hand-written GLSL");
		if (mat.blendMode != 0)
			return setWhy(whyNot, "its blend mode is not opaque");
		if (mat.opacity < 1.0f)
			return setWhy(whyNot, "it is translucent");
		if (mat.texturePaths.empty() || mat.texturePaths[0].empty())
			return setWhy(whyNot, "it has no texture");
		r.albedoPath = mat.texturePaths[0];
		for (int i = 0; i < 3; ++i) r.tint[i] = mat.baseColor[i];
		r.metallic  = mat.metallic;
		r.roughness = mat.roughness;
	}
	r.doubleSided = mat.doubleSided;
	out = r;
	return true;
}

} // namespace HE
