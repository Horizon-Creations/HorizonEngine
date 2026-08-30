#include "SkyCoreHlsl.h"
#include "HlslSources.h"
#include "sky_core_glsl.h"          // kSkyCoreGLSL, erzeugt aus shaders/sky_core.glsl
#include <Diagnostics/Logger.h>
#include <mutex>

#if HE_HAVE_SHADERC
#include <ShaderCompiler.h>
#endif

namespace HE::hlsl
{
namespace
{
bool s_generated = false;

// Der Kern ist reiner Funktionsrumpf -- kein #version, keine Ein-/Ausgaben. Fuer
// glslang muss daraus ein uebersetzbarer Shader werden. Der Entry-Point ruft
// skyColor auf, damit nichts wegoptimiert wird; SPIRV-Cross emittiert nur, was
// tatsaechlich erreichbar ist.
std::string wrapForCompile()
{
	std::string s = "#version 450\n";
	s += "layout(location = 0) in vec2 vNDC;\n";
	s += "layout(location = 0) out vec4 FragColor;\n";
	s += "layout(set = 0, binding = 0) uniform SkyEnv { mat4 invViewProj; vec3 sunDir; float pad0; } sky;\n";
	s += kSkyCoreGLSL;
	s += "\nvoid main()\n{\n"
	     "\tvec4 wp = sky.invViewProj * vec4(vNDC, 1.0, 1.0);\n"
	     "\tFragColor = vec4(skyColor(normalize(wp.xyz / wp.w), sky.sunDir), 1.0);\n"
	     "}\n";
	return s;
}

// Schneidet die drei Funktionen aus dem erzeugten HLSL heraus und entfernt das
// `inout`. Leerer Rueckgabewert heisst: die Marken waren nicht da, also nicht
// raten, sondern zurueckfallen.
std::string extractCore(const std::string& hlsl)
{
	const size_t begin = hlsl.find("float2 atmoRaySphere(");
	if (begin == std::string::npos) return {};
	// SPIRV-Cross legt den Rumpf des Entry-Points immer als frag_main() an; alles
	// davor sind die Hilfsfunktionen, alles danach ist Geruest.
	const size_t end = hlsl.find("\nvoid frag_main(", begin);
	if (end == std::string::npos) return {};

	std::string core = hlsl.substr(begin, end - begin);
	for (const char* param : { "inout float3 dir", "inout float3 sunDir" })
	{
		const std::string from = param;
		const std::string to   = from.substr(6);   // "inout " abschneiden
		for (size_t p = core.find(from); p != std::string::npos; p = core.find(from, p + to.size()))
			core.replace(p, from.size(), to);
	}
	// Die drei muessen danach alle da sein, sonst hat sich das Emissionsformat
	// geaendert und der Ausschnitt ist nur zufaellig plausibel.
	for (const char* fn : { "atmoRaySphere(", "atmoScatter(", "skyColor(" })
		if (core.find(fn) == std::string::npos) return {};
	if (core.find("inout") != std::string::npos) return {};   // ein inout uebersehen
	return core;
}

std::string buildSkyCore()
{
#if HE_HAVE_SHADERC
	const he::shaderc::Result r =
		he::shaderc::compile(wrapForCompile(), he::shaderc::Stage::Fragment,
		                     he::shaderc::Target::HlslSm50);
	if (!r.ok)
	{
		HE_LOG_WARN(RHI, "Himmelskern: GLSL->HLSL fehlgeschlagen, D3D zeichnet den alten "
		                 "Gradienten statt der Streuungsintegrale. %s", r.log.c_str());
		return kSkyFuncHLSL;
	}
	std::string core = extractCore(r.source);
	if (core.empty())
	{
		HE_LOG_WARN(RHI, "%s", "Himmelskern: uebersetzt, aber die drei Funktionen liessen "
		                 "sich nicht herausschneiden (Emissionsformat von SPIRV-Cross "
		                 "geaendert?). D3D zeichnet den alten Gradienten.");
		return kSkyFuncHLSL;
	}
	s_generated = true;
	return core;
#else
	// Ohne Cross-Compiler bleibt es beim Handspiegel. Kein Fehler, aber D3D
	// zeichnet dann einen anderen Himmel als GL/Metal/Vulkan.
	HE_LOG_INFO(RHI, "%s", "Himmelskern: ohne Cross-Compiler gebaut - D3D benutzt den "
	                 "handgeschriebenen Gradienten kSkyFuncHLSL.");
	return kSkyFuncHLSL;
#endif
}
} // namespace

const std::string& SkyCoreHLSL()
{
	// Einmal pro Prozess. Beide D3D-Backends teilen sich das Ergebnis; wer zuerst
	// einen Renderer anlegt, zahlt die Uebersetzung.
	static const std::string s_core = buildSkyCore();
	return s_core;
}

bool SkyCoreIsGenerated()
{
	SkyCoreHLSL();
	return s_generated;
}
} // namespace HE::hlsl
