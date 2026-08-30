#version 450
// Fuer #include unten: shaders/sky_core.glsl ist der gemeinsame analytische
// Himmelskern. Die Direktive muss vor allen Nicht-Praeprozessor-Tokens stehen,
// deshalb hier oben und nicht an der Einbindestelle.
#extension GL_GOOGLE_include_directive : require

// ─── DRIFT WARNING: reduced second copy of the GL sky shader ─────────────────
// This file is NOT the reference implementation. The engine's sky lives twice:
//
//   reference : src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp
//               `kSkyFS`        — the sky fragment shader
//               `kSkyFuncGLSL`  — the shared analytic sky, spliced in at the
//                                 `//#SKYFUNC#` marker (Metal mirrors both in MSL)
//   this file : a hand-maintained, feature-reduced port for the Vulkan backend
//
// They are edited independently, so the GL/Metal sky has moved ahead. Verified
// against the GL source (audit 1a); everything below is present in `kSkyFS` /
// `kSkyFuncGLSL` and MISSING here:
//
//   * ERLEDIGT — atmoScatter / atmoRaySphere / skyColor kommen jetzt per #include
//     aus shaders/sky_core.glsl, derselben Datei, aus der GL sein Literal
//     erzeugt. Das war der groesste sichtbare Unterschied (Sonnenuntergangs-
//     roetung und blaue Stunde waren hier angenaehert statt integriert) und er
//     kann nicht wieder auseinanderlaufen: es gibt den Text nur noch einmal.
//   * applyClouds3D + cloudBillowFbm / cloudCoverFbm — the 3D volumetric cloud
//     raymarch (cloudMode 1). Only the 2D dome `applyClouds()` exists here.
//     NOTE: `hgPhase()` IS present — it is used by the 2D cloud path. What is
//     missing is its SECOND use inside applyClouds3D, not the function.
//   * applyAurora3D + auroraRnd / auroraWeb / auroraSlab — the 3D aurora
//     curtains (independent finite ribbons crossing into a net). The flat 2D
//     `aurora()` below is the older version, so this file is not aurora-less.
//   * cirrus / cirrusFbm (high cirrus layer) and contrails
//   * sunDisk, sunGlare, moonCorona, and the procedural moon surface
//     (moonFbm / moonHash / moonNoise) used when no moon texture is bound
//   * crepuscular / godrayClear (god rays), rainbow (rain-driven),
//     shootingStars (meteors)
//   * nebIso / mwRift / skyHsv — the 3-colour nebula + Milky Way rift. The
//     `nebula()` below is the older single-colour version.
//   * skyIgn — interleaved-gradient noise used to dither the raymarch steps
//
// The SkyEnv block below is likewise a reduced form of HE::SkyFrameParams
// (include/HorizonRendering/SkyFrameParams.h, 336 bytes vs 160 here). The
// renderer therefore reads named fields out of BuildSkyFrameParams instead of
// memcpy'ing it — see VulkanRenderer::drawSky. Everything past `nebulaColor` in
// SkyFrameParams has no slot here: cameraPos/cloudMode, cloud height/density/
// fluffiness/tint/quality, low-res-cloud compositing, contrail + cirrus amounts,
// cirrus/nebula seeds, aurora height/fragmentation/top colour, nebula colours 2
// and 3, nebula coverage/quality, god rays, shooting stars, the whole star block
// (colour, brightness, size, size variation, density, glow, twinkle), moon phase
// and rain amount.
//
// Audit 1a decision: DOCUMENT the drift, do not port. Porting the features (or
// better, generating all three backends from one source) is tracked separately.
//
// Stand P3/Scheibe 1: der analytische KERN ist umgestellt (siehe oben), der Rest
// der Liste steht noch. Fuer den Kern gilt die Warnung also nicht mehr — eine
// Aenderung an sky_core.glsl ist hier sofort sichtbar. Fuer alles andere in
// dieser Liste gilt weiterhin: eine Aenderung am GL-Himmel kommt hier NICHT an.
// ─────────────────────────────────────────────────────────────────────────────

layout(location = 0) in vec2 vNDC;
layout(location = 0) out vec4 FragColor;

// ── Sky-Konstanten: exakte Spiegelung von HE::SkyFrameParams ─────────────────
// Bis hierher war dieser Block eine REDUZIERTE und anders sortierte Fassung der
// kanonischen Struktur: 160 statt 336 Bytes, timeOfDay bei Offset 76 statt in
// params.x, und die halbe Struktur fehlte ganz. Der Renderer musste deshalb 15
// Felder von Hand umkopieren, und jeder neue Sky-Parameter war eine Stelle, die
// man an drei Orten nachziehen muss -- C++-Struktur, dieser Block, Kopiercode.
// Wird einer vergessen, kommt still der falsche Wert an und kein Test faellt um.
//
// Jetzt steht hier mat4 + 17 vec4 in genau der Reihenfolge von
// BuildSkyFrameParams (SkyFrameParams.cpp), also 336 Bytes offset-gleich. Der
// Renderer memcpy't die Struktur unveraendert. Geprueft wird das nicht per
// Nachrechnen, sondern an den Offsets im kompilierten SPIR-V:
// scripts/check_sky_ubo_layout.py.
//
// REIHENFOLGE NICHT AENDERN, ohne SkyFrameParams.h und die Metal-Kopie
// mitzuziehen -- ein verschobenes vec4 verschiebt alles dahinter.
layout(set = 0, binding = 0) uniform SkyEnv {
    mat4 invViewProj;
    vec4 sunDir;         // xyz Richtung ZUR Sonne, w hasMoonTexture (1/0)
    vec4 sunColor;       // xyz Sonnenfarbe,        w moonPhase
    vec4 params;         // timeOfDay, cloudCoverage, time, auroraIntensity
    vec4 nebulaColor;    // xyz,                    w nebulaIntensity
    vec4 auroraColor;    // xyz,                    w milkyWayIntensity
    vec4 wind;           // xyz Wolkendrift,        w flash
    vec4 cameraPos;      // xyz,                    w cloudMode
    vec4 cloud;          // cloudHeight, cloudDensity, cloudFluffiness, contrailAmount
    vec4 cloudTint;      // xyz,                    w cirrusAmount
    vec4 cirrus;         // cirrusSeed, auroraHeight, auroraFragmentation, nebulaSeed
    vec4 nebulaColor2;   // xyz,                    w nebulaQuality
    vec4 nebulaColor3;   // xyz,                    w godRays
    vec4 auroraColorTop; // xyz,                    w shootingStars
    vec4 starColor;      // xyz,                    w starBrightness
    vec4 star;           // starSize, starSizeVariation, starDensity, starGlow
    vec4 star2;          // starTwinkle, cloudQuality, lowResClouds, rainAmount
    vec4 neb2;           // nebulaCoverage, cloudStyle, cloudInterShadows, cloudEvolution
} sky;

// Lesbare Namen fuer die gepackten Slots. Der Rumpf unten soll weiter von
// uSunDir statt sky.sunDir.xyz reden; die Packung ist eine Layout-Frage, keine
// Frage der Lesbarkeit. Die Namen sind bewusst die der GL-Uniformen
// (OpenGLRenderer kSkyFS), damit ein Vergleich der beiden Shader Zeile fuer
// Zeile moeglich bleibt.
#define uSunDir         sky.sunDir.xyz
#define uHasMoonTex     (sky.sunDir.w != 0.0)
#define uSunColor       sky.sunColor.xyz
#define uMoonPhase      sky.sunColor.w
#define uTimeOfDay      sky.params.x
#define uCloudCoverage  sky.params.y
#define uTime           sky.params.z
#define uAurora         sky.params.w
#define uNebulaColor    sky.nebulaColor.xyz
#define uNebula         sky.nebulaColor.w
#define uAuroraColor    sky.auroraColor.xyz
#define uMilkyWay       sky.auroraColor.w
#define uWind           sky.wind.xyz
#define uFlash          sky.wind.w
// Der Sternblock. Bis P3b lag er gar nicht im UBO -- die sieben Regler waren
// auf Vulkan wirkungslos, HE_DUMP_STARDENS=0 zeichnete weiter Sterne. Das war
// der gemessene Rest im Nacht-Vergleich gegen GL: 2698 Pixel, auf denen Vulkan
// heller ist als die Referenz.
#define uStarColor      sky.starColor.xyz
#define uStarBright     sky.starColor.w
#define uStarSize       sky.star.x
#define uStarSizeVar    sky.star.y
#define uStarDensity    sky.star.z
#define uStarGlow       sky.star.w
#define uStarTwinkle    sky.star2.x
// Fuer die 3D-Aurora aus shaders/sky_aurora.glsl. Bis P3b lagen auroraColorTop,
// auroraHeight und auroraFragmentation gar nicht im UBO -- die Regler waren
// wirkungslos, und die Funktion waere ohne sie nicht portierbar gewesen.
#define uAuroraColorTop sky.auroraColorTop.xyz
#define uAuroraHeight   sky.cirrus.y
#define uAuroraFragment sky.cirrus.z
#define uCameraPos      sky.cameraPos.xyz

layout(set = 0, binding = 1) uniform sampler2D uMoonTex;
layout(set = 0, binding = 2) uniform sampler3D uNoise;

// ── Analytischer Himmel — gemeinsame Quelle ──────────────────────────────────
// Hier stand bis hierher eine eigene, handgepflegte Kopie von skyColor(): der
// alte Tag/Daemmerung/Nacht-Gradient mit festen Zenit- und Horizontfarben. GL
// hat den Kern laengst durch Rayleigh/Mie/Ozon-Streuungsintegrale ersetzt, diese
// Datei nicht — das war der groesste sichtbare Unterschied zwischen den Backends
// (und der erste Punkt der DRIFT WARNING oben).
//
// Jetzt kommt der Text aus shaders/sky_core.glsl, derselben Datei, aus der auch
// das GL-Stringliteral erzeugt wird (cmake/embed_glsl.cmake). Signatur und
// Aufrufstelle bleiben unveraendert: skyColor(dir, sunDir), einmal in main().
// Der Kern ist uniform- und samplerfrei, deshalb passt er hier ohne Anpassung.
#include "sky_core.glsl"

// Hash / noise functions (pure math)
float starHash(vec3 p)
{
    p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
// Trilinear value noise sampled from the precomputed uNoise volume (texels hold
// starHash at the integer lattice). Pre-smoothstepping the fractional coordinate
// makes the hardware linear filter reproduce the old smoothstep interpolation,
// and +0.5 lands integer lattice points on texel centres.
float starNoise3(vec3 p)
{
    vec3 f = fract(p);
    vec3 q = floor(p) + f * f * (3.0 - 2.0 * f) + 0.5;
    return texture(uNoise, q * (1.0 / 256.0)).r;
}
float starFbm3(vec3 p, int oct)
{
    float v=0.0, a=0.5;
    for(int i=0;i<oct;++i){v+=a*starNoise3(p);p*=2.03;a*=0.5;}
    return v;
}
float cloudHash(vec2 p)
{
    p=fract(p*vec2(127.1,311.7)); p+=dot(p,p+34.56); return fract(p.x*p.y);
}
float cloudNoise(vec2 p)
{
    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);
    return mix(mix(cloudHash(i),cloudHash(i+vec2(1,0)),u.x),
               mix(cloudHash(i+vec2(0,1)),cloudHash(i+vec2(1,1)),u.x),u.y);
}
float cloudFbm(vec2 p)
{
    float v=0.0,a=0.5;
    for(int i=0;i<5;++i){v+=a*cloudNoise(p);p*=2.02;a*=0.5;}
    return v;
}

// Celestial rotation
vec3 celestialDir(vec3 dir, float tod)
{
    float a=tod*6.2831853;
    vec3 axis=normalize(vec3(0.22,0.92,0.32));
    float c=cos(a),s=sin(a);
    return dir*c+cross(axis,dir)*s+axis*dot(axis,dir)*(1.0-c);
}
float galacticBand(vec3 cd)
{
    vec3 gN=normalize(vec3(0.46,0.52,-0.72));
    float d=dot(normalize(cd),gN); return exp(-d*d*7.0);
}

// Die sieben Sternregler sind hier angeschlossen -- an DIESE Funktion, nicht an
// GLs deutlich weitergezogene Fassung (3x3x3-Splat, AA-Boden, Great-Rift). Der
// Algorithmenabgleich ist P3c; hier geht es darum, dass die Werte ueberhaupt
// ankommen. Die Stellen, an die sie greifen, gab es bereits alle -- Schwelle,
// Radius, Halo, Flimmern, Toenung, Helligkeit -- sie standen nur auf Konstanten.
vec3 starField(vec3 dir, vec3 cdir, vec3 sunDir, float t, float mw)
{
    float night=1.0-smoothstep(-0.10,0.10,clamp(sunDir.y,-0.2,1.0));
    if(night<=0.0||dir.y<=0.0) return vec3(0.0);
    float band=galacticBand(cdir), mwc=clamp(mw,0.0,1.0);
    // Dichte setzt die BASISSCHWELLE fuer die ganze Kuppel, wie in GL: bei 0 liegt
    // sie ueber 1.0, und weil starHash nie 1.0 erreicht, qualifiziert sich keine
    // Zelle -- also wirklich null Sterne, nicht nur wenige. Die Bandabsenkung wird
    // mitskaliert, sonst bliebe bei Dichte 0 die Milchstrassenspur stehen.
    float dens=clamp(uStarDensity,0.0,1.0);
    float baseTh=mix(1.001,0.79,dens);
    float thresh=baseTh-band*mix(0.07,0.20,mwc)*dens;
    vec3 p=cdir*70.0, cell=floor(p);
    float present=starHash(cell);
    if(present<thresh) return vec3(0.0);
    vec3 sp=vec3(starHash(cell+1.7),starHash(cell+4.3),starHash(cell+8.9));
    float d=length(fract(p)-sp);
    // Groessenstreuung: bei uStarSizeVar 0 bekommt jeder Stern dieselbe mittlere
    // Groesse, bei 1 die volle kubische Spreizung von vorher.
    float sizeH=starHash(cell+5.7);
    float big=mix(0.125,sizeH*sizeH*sizeH,clamp(uStarSizeVar,0.0,1.0));
    float radius=mix(0.05,0.17,big)*max(uStarSize,0.0);
    float core=smoothstep(radius,0.0,d); core*=core;
    float halo=smoothstep(radius*3.0,radius,d)*(big*big)*0.35*max(uStarGlow,0.0);
    float shape=core+halo;
    float mag=(0.4+0.6*smoothstep(thresh,1.0,present))*mix(0.7,2.7,big);
    float twPhase=starHash(cell+23.5)*6.2831, twFreq=2.0+4.0*starHash(cell+47.1);
    // Flimmern: uStarTwinkle 0 haelt den Stern ruhig, 1 ist der bisherige Hub.
    float twAmp=0.3*clamp(uStarTwinkle,0.0,1.0);
    float tw=(1.0-twAmp)+twAmp*sin(t*twFreq+twPhase);
    float horizon=smoothstep(0.0,0.15,dir.y);
    vec3 tint=mix(vec3(0.80,0.88,1.0),vec3(1.0,0.93,0.82),starHash(cell+12.1));
    float bandDim=mix(1.6,mix(0.9,1.5,mwc),band);
    return tint*uStarColor*(shape*mag*tw*horizon*night*bandDim*uStarBright);
}

// ── 3D-Aurora: gemeinsame Quelle ────────────────────────────────────────────
// Hier stand eine eigene, flache aurora(): 20 Zeilen, die die Vorhaenge als
// zwei uebereinandergelegte Rauschbaender ueber einer projizierten Ebene
// zeichneten. GL hat laengst unabhaengige endliche Baender, die sich zu einem
// Netz kreuzen (auroraSlab/auroraWeb, 270 Zeilen). Gemessen war das die
// groesste belegte Einzelluecke im Himmel: 63,9 % der Bytes gegen GL.
//
// Jetzt kommt der Text aus shaders/sky_aurora.glsl -- derselben Datei, aus der
// GL sein Literal erzeugt und D3D sein HLSL uebersetzt. Die Funktion heisst
// applyAurora3D und nimmt zusaetzlich Kameraposition, Deckfarbe und die
// Fragmentkoordinate; alles davon liegt seit P3b im Konstantenpuffer.
#include "sky_aurora.glsl"

vec3 moonDisk(vec3 dir, vec3 sunDir)
{
    float day=smoothstep(-0.10,0.10,clamp(sunDir.y,-0.2,1.0)), night=1.0-day;
    if(night<=0.0) return vec3(0.0);
    vec3 moonDir2=normalize(vec3(-sunDir.x,-sunDir.y,sunDir.z));
    if(dot(dir,moonDir2)<=0.0) return vec3(0.0);
    vec3 right=normalize(cross(vec3(0,1,0),moonDir2)), up=cross(moonDir2,right);
    const float kR=0.030;
    vec2 q=vec2(dot(dir,right),dot(dir,up))/kR;
    float r=length(q); if(r>1.0) return vec3(0.0);
    float tex=uHasMoonTex?texture(uMoonTex,q*0.5+0.5).r:1.0;
    float limb=sqrt(max(1.0-r*r,0.0)), edge=smoothstep(1.0,0.90,r);
    return vec3(0.92,0.94,1.00)*(tex*limb*edge*3.0*night);
}

// Cloud slab heights (arbitrary world units in the sky-ray hemisphere model).
const float kCloudBase  = 1.0;
const float kCloudTop   = 2.6;
const float kCloudScale = 1.2;    // spatial frequency of the cloud field
// Worley (cellular) lookup from the noise volume's G channel.
float worleyNoise3(vec3 p)
{
    return texture(uNoise, p * (1.0 / 256.0)).g;
}
float worleyFbm(vec3 p)
{
    return worleyNoise3(p)        * 0.625
         + worleyNoise3(p * 2.03) * 0.25
         + worleyNoise3(p * 4.06) * 0.125;
}
// Henyey-Greenstein phase: forward-biased scattering so the sun-facing edges glow.
float hgPhase(float cosT, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}
// Rounded vertical density taper so the slab reads as puffy bodies, not a sheet.
float cloudHeightGrad(float y)
{
    float hf = clamp((y - kCloudBase) / (kCloudTop - kCloudBase), 0.0, 1.0);
    return smoothstep(0.0, 0.25, hf) * (1.0 - smoothstep(0.6, 1.0, hf));
}
// Full density at a world point: billowy Worley over a large-scale perlin coverage
// field, thresholded by the coverage slider and shaped by the slab height.
float cloudDensity(vec3 pos, float time, float coverage, vec3 wind)
{
    float hgrad = cloudHeightGrad(pos.y);
    if (hgrad <= 0.0) return 0.0;                                  // outside slab → no fetches
    vec3  p      = pos * kCloudScale + wind * time;
    float morph  = time * 0.030;                                  // slow forming/dissolving
    float perlin = starFbm3(p + vec3(0.0, morph, 0.0), 4);        // large-scale coverage
    float billow = worleyFbm(p * 0.9 + vec3(morph, 0.0, 0.0));    // fine cauliflower detail
    float base   = perlin * 0.5 + billow * 0.55;
    float lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
    return smoothstep(lo, lo + 0.13, base) * hgrad;
}
// Density for the sun light-march. Slightly fewer octaves than the view density.
float cloudShadowDensity(vec3 pos, float time, float coverage, vec3 wind)
{
    float hgrad = cloudHeightGrad(pos.y);
    if (hgrad <= 0.0) return 0.0;
    vec3  p      = pos * kCloudScale + wind * time;
    float morph  = time * 0.030;
    float perlin = starFbm3(p + vec3(0.0, morph, 0.0), 3);
    float billow = worleyNoise3(p * 0.9 + vec3(morph, 0.0, 0.0)) * 0.7
                 + worleyNoise3(p * 1.8) * 0.3;
    float base   = perlin * 0.5 + billow * 0.55;
    float lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
    return smoothstep(lo, lo + 0.13, base) * hgrad;
}
vec3 applyClouds(vec3 baseSky, vec3 dir, vec3 sunDir, float time, float coverage, vec3 sunColor, vec3 wind)
{
    if (coverage <= 0.0) return baseSky;          // clear sky → skip the whole raymarch
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

    // March the view ray through the cloud slab between base and top heights.
    float s0 = kCloudBase / max(dir.y, 1e-3);
    float s1 = kCloudTop  / max(dir.y, 1e-3);
    const int N = 16;
    float ds = (s1 - s0) / float(N);
    float jitter = cloudHash(dir.xz * 173.3 + vec2(dir.y * 37.1, dir.y * 19.7));

    // Day/night/dusk drive the cloud colour (independent of the drift clock).
    float sunY = clamp(sunDir.y, -0.2, 1.0);
    float day  = smoothstep(-0.10, 0.10, sunY);
    float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

    // Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
    float costh = max(dot(dir, sunDir), 0.0);
    float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

    float T = 1.0;                                 // transmittance along the view ray
    vec3  L = vec3(0.0);                           // accumulated in-scattered colour
    for (int i = 0; i < N; ++i)
    {
        float s   = s0 + (float(i) + jitter) * ds;
        vec3  pos = dir * s;
        float dens = cloudDensity(pos, time, coverage, wind);
        if (dens > 0.001)
        {
            // Light-march toward the sun: Beer's-law self-shadowing (3 steps).
            float shadow = 0.0;
            for (int j = 1; j <= 3; ++j)
                shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25), time, coverage, wind);
            float sun    = exp(-shadow * 1.7);
            float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
            float lit    = sun * powder;

            // Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
            vec3 dayCol   = mix(vec3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
            vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.26, 0.29, 0.45), lit);
            vec3 cloudCol = mix(nightCol, dayCol, day);
            vec3 duskTop  = sunColor * vec3(1.25, 0.55, 0.28);
            cloudCol = mix(cloudCol, duskTop, dusk * lit * 0.9);
            // Moonlit silver: moon rises on the opposite arc from the sun.
            vec3  cMoonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
            float cMoonUp  = clamp((cMoonDir.y + 0.10) / 0.25, 0.0, 1.0);
            cloudCol += vec3(0.20, 0.22, 0.38) * lit * cMoonUp * (1.0 - day) * 0.25;
            // Forward-scatter glow: the silver lining.
            cloudCol += sunColor * (phase * sun * 0.9 * max(day, dusk));
            // Cheap vertical depth: tops catch the light, base sits in self-shadow.
            float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
            cloudCol *= mix(0.5, 1.15, hTone);
            cloudCol += vec3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.25);

            float opticalDepth = dens * ds * 7.0;
            float a = 1.0 - exp(-opticalDepth);
            L += T * a * cloudCol;
            T *= 1.0 - a;
            if (T < 0.02) break;
        }
    }

    // Fade the whole cloud layer out into the horizon haze.
    float horizon = smoothstep(0.02, 0.16, dir.y);
    T = 1.0 - (1.0 - T) * horizon;
    L *= horizon;
    return baseSky * T + L;
}

// Space nebula — drifting coloured emission clouds gathered toward the galactic
// band. Sampled as 3D blobs on the celestial sphere (rotates with the stars).
vec3 nebula(vec3 dir, vec3 cdir, vec3 sunDir, float intensity, vec3 nebColor)
{
    if (intensity <= 0.0) return vec3(0.0);
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
    if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

    vec3  cN   = normalize(cdir);
    const vec3 galN = normalize(vec3(0.46, 0.52, -0.72));
    float bd   = dot(cN, galN);
    float band = exp(-bd * bd * 1.5);           // wide soft milky-way bias
    vec3  P    = cN * 3.4;
    float big  = starFbm3(P * 0.7 + 11.0, 4);   // large clouds
    float med  = starFbm3(P * 1.7 + 27.0, 3);   // medium clumps
    float fine = starFbm3(P * 4.0 + 41.0, 2);   // fine mottle / embedded dust
    float blob   = smoothstep(0.35, 0.70, big * 0.5 + med * 0.6);
    // Structural character per region: dense puffy bodies vs. wispy filaments.
    float charF  = starFbm3(P * 0.4 + 150.0, 2);
    float wispy  = smoothstep(0.42, 0.70, charF);
    float fila   = smoothstep(0.55, 0.86, starFbm3(P * 5.5 + 97.0, 2));   // fine filaments
    float detail = (0.30 + 0.70 * smoothstep(0.32, 0.86, fine)) * mix(1.0, 0.65 + 0.9 * fila, wispy);
    float dust   = 1.0 - 0.5 * smoothstep(0.50, 0.88, starFbm3(P * 2.6 + 63.0, 3));
    float density = blob * detail * dust;
    float core   = smoothstep(0.62, 0.95, big * 0.55 + med * 0.55);   // bright centres
    float glow   = (band * 0.85 + 0.15) * (density + 0.6 * core);     // baseline -> off-band patches
    if (glow <= 0.0) return vec3(0.0);

    // Hue wheel across neighbouring blobs.
    float h = clamp(starFbm3(P * 0.5 + 71.0, 3) * 1.7 - 0.35
                  + 0.25 * (starFbm3(P * 1.1 + 83.0, 2) - 0.5), 0.0, 1.0);
    float warm = smoothstep(0.40, 0.72, starFbm3(P * 0.32 + 131.0, 2));
    h = clamp(h + warm * 0.30, 0.0, 1.0);
    vec3  colA = nebColor * vec3(0.42, 0.62, 1.50);   // cool blue
    vec3  colB = nebColor * vec3(0.34, 1.42, 1.18);   // teal/cyan
    vec3  colC = nebColor * vec3(0.55, 1.42, 0.55);   // green
    vec3  colD = nebColor * vec3(1.75, 1.10, 0.40);   // gold/amber
    vec3  colE = nebColor * vec3(1.85, 0.42, 0.95);   // magenta/pink
    vec3  col  = colA;
    col = mix(col, colB, smoothstep(0.14, 0.36, h));
    col = mix(col, colC, smoothstep(0.36, 0.54, h));
    col = mix(col, colD, smoothstep(0.54, 0.72, h));
    col = mix(col, colE, smoothstep(0.72, 0.92, h));
    float horizon = smoothstep(0.0, 0.16, dir.y);
    return col * (glow * 6.0 * horizon * night * intensity);
}

void main()
{
    // Vulkan NDC: y-flip needed because vNDC comes from the VS which uses GL convention
    // The sky VS outputs p.y directly (no flip), so vNDC.y matches screen-y-down convention.
    // invViewProj accounts for the Vulkan clip-fix already present in the projection matrix.
    // Reconstruct world ray: NDC z=0 near, z=1 far.
    vec4 wp1 = sky.invViewProj * vec4(vNDC, 1.0, 1.0); // far
    vec4 wp0 = sky.invViewProj * vec4(vNDC, 0.0, 1.0); // near
    // Normalize: starField/aurora/moonDisk/clouds all assume a unit direction. Without
    // this, dir has the far-plane magnitude (~1000) and the star cells land at huge
    // coordinates where the hash loses float precision → stars flicker/vanish on rotate.
    vec3 dir = normalize(wp1.xyz / wp1.w - wp0.xyz / wp0.w);

    vec3 col = skyColor(dir, uSunDir);
    float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(uSunDir).y, -0.2, 1.0));
    if (nightF > 0.0) {
        vec3 cdir = celestialDir(dir, uTimeOfDay);
        col += starField(dir, cdir, uSunDir, uTime, uMilkyWay);
        col += nebula(dir, cdir, uSunDir, uNebula, uNebulaColor);
        col += applyAurora3D(dir, uCameraPos, uTime, uAurora, uAuroraColor, uAuroraColorTop,
                             gl_FragCoord.xy);
        col += moonDisk(dir, uSunDir);
    }
    col = applyClouds(col, dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind);
    col += uFlash * vec3(0.85, 0.90, 1.0);
    FragColor = vec4(col, 1.0);
}
