#pragma once
#include <string>

// ─── Der analytische Himmelskern als HLSL ────────────────────────────────────
// shaders/sky_core.glsl ist die einzige Quelle des Himmelskerns. OpenGL spleisst
// den Text an //#SKYFUNC#, Vulkan bindet die Datei per #include ein -- beide
// lesen GLSL. D3D kann das nicht, also wird derselbe Text hier uebersetzt:
// GLSL -> SPIR-V (glslang) -> HLSL SM5.0 (SPIRV-Cross), einmal beim Anlegen des
// Renderers, danach gecacht.
//
// Warum uebersetzen und nicht von Hand spiegeln: genau das war kSkyFuncHLSL, und
// es ist stehengeblieben. 43 Zeilen alter Tag/Daemmerung/Nacht-Gradient gegen
// 173 Zeilen Rayleigh/Mie/Ozon in GL -- waehrend der Kommentar darueber bis
// zuletzt behauptete, er spiegele GL "exactly". Ein Spiegel, den niemand prueft,
// ist nach der zweiten Aenderung falsch.
//
// ZWEI STELLEN, die beim Uebersetzen nachbehandelt werden muessen:
//
//  1. SPIRV-Cross emittiert `skyColor(inout float3 dir, inout float3 sunDir)`,
//     weil der GLSL-Rumpf seine Parameter zuweist (dir = normalize(dir)). In
//     GLSL sind Parameter per Wert, in HLSL bindet `inout` aber nur an lvalues
//     -- und der Scene-Aufruf uebergibt `ray/dist`, einen rvalue. Das `inout`
//     wird deshalb gestrichen; by-value ist die GLSL-Semantik.
//  2. Um die drei Funktionen herum stehen cbuffer, Ein-/Ausgabestrukturen und
//     ein Entry-Point, die der Wrapper unten nur braucht, damit ueberhaupt etwas
//     uebersetzt wird. Herausgeschnitten wird der Bereich von der ersten
//     Funktion bis vor frag_main().
//
// Faellt irgendetwas davon aus -- kein Cross-Compiler im Build, Uebersetzung
// schlaegt fehl, Extraktion findet die Marken nicht -- liefert die Funktion
// kSkyFuncHLSL zurueck. Das ist der alte Gradient, also genau das, was D3D vor
// dieser Aenderung ohnehin gezeichnet hat: der Rueckfall ist kein Ausfall.
namespace HE::hlsl
{
// Der Himmelskern in HLSL, mit skyColor(float3 dir, float3 sunDir) als Einstieg.
// Beim ersten Aufruf uebersetzt, danach aus dem Cache. Threadsicher.
const std::string& SkyCoreHLSL();

// Wahr, wenn SkyCoreHLSL() den uebersetzten Kern liefert, und falsch, wenn der
// Rueckfall auf kSkyFuncHLSL gegriffen hat. Nur fuer Diagnose gedacht -- der
// Aufrufer soll den Rueckgabewert benutzen, nicht danach verzweigen.
bool SkyCoreIsGenerated();
}
