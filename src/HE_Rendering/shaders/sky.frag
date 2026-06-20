#version 450

layout(location = 0) in vec2 vNDC;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform SkyEnv {
    mat4  invViewProj;
    vec3  sunDir;    float timeOfDay;
    vec3  sunColor;  float cloudCoverage;
    vec3  wind;      float time;
    vec3  auroraColor; float aurora;
    float milkyWay;  float flash; int hasMoonTex; float nebula;
    vec3  nebulaColor; float _pad2;
} sky;

layout(set = 0, binding = 1) uniform sampler2D uMoonTex;
layout(set = 0, binding = 2) uniform sampler3D uNoise;

// ── Procedural sky ────────────────────────────────────────────────────────────
vec3 skyColor(vec3 dir, vec3 sunDir)
{
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    float sunY = clamp(sunDir.y, -0.2, 1.0);
    float day  = smoothstep(-0.10, 0.10, sunY);
    float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));
    vec3 zenithDay  = vec3(0.08, 0.28, 0.72);
    vec3 horizDay   = vec3(0.42, 0.62, 0.88);
    vec3 zenithNite = vec3(0.003, 0.005, 0.015);
    vec3 horizNite  = vec3(0.006, 0.009, 0.024);
    vec3 zenith  = mix(zenithNite, zenithDay, day);
    vec3 horizon = mix(horizNite,  horizDay,  day);
    vec2  sunAz  = normalize(sunDir.xz + vec2(1e-5));
    float toward = dot(normalize(dir.xz + vec2(1e-5)), sunAz) * 0.5 + 0.5;
    toward = pow(clamp(toward, 0.0, 1.0), 1.5);
    vec3  duskHoriz = mix(vec3(0.52, 0.30, 0.52), vec3(1.20, 0.50, 0.16), toward);
    horizon = mix(horizon, duskHoriz, dusk);
    zenith  = mix(zenith,  vec3(0.20, 0.16, 0.40), dusk * 0.6);
    float h    = clamp(dir.y, 0.0, 1.0);
    float grad = pow(1.0 - h, 2.5);
    vec3 sky2 = mix(zenith, horizon, grad);
    float band = pow(1.0 - h, 8.0) * toward;
    sky2 += vec3(1.25, 0.62, 0.26) * (band * dusk * 0.8);
    vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
    sky2 = mix(sky2, ground, smoothstep(0.0, -0.25, dir.y));
    vec3  sunTint = mix(vec3(1.0, 0.42, 0.20), vec3(1.0, 0.96, 0.88), smoothstep(0.0, 0.25, sunY));
    float s = max(dot(dir, sunDir), 0.0);
    float sunVis = max(day, dusk);
    sky2 += sunTint * (pow(s, 1800.0) * 14.0) * day;
    sky2 += sunTint * (pow(s, 180.0)  * 2.2) * sunVis;
    sky2 += sunTint * (pow(s, 22.0)   * 0.7) * sunVis;
    sky2 += vec3(1.0, 0.5, 0.25) * (pow(s, 5.0) * 0.5) * dusk;
    float night   = 1.0 - day;
    vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
    float m       = max(dot(dir, moonDir), 0.0);
    sky2 += vec3(0.80, 0.86, 1.00) * (pow(m, 60.0) * 0.05) * night;
    sky2 += vec3(0.015, 0.018, 0.030) * night;
    return sky2;
}

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

vec3 starField(vec3 dir, vec3 cdir, vec3 sunDir, float t, float mw)
{
    float night=1.0-smoothstep(-0.10,0.10,clamp(sunDir.y,-0.2,1.0));
    if(night<=0.0||dir.y<=0.0) return vec3(0.0);
    float band=galacticBand(cdir), mwc=clamp(mw,0.0,1.0);
    float thresh=mix(0.92,mix(0.86,0.72,mwc),band);
    vec3 p=cdir*70.0, cell=floor(p);
    float present=starHash(cell);
    if(present<thresh) return vec3(0.0);
    vec3 sp=vec3(starHash(cell+1.7),starHash(cell+4.3),starHash(cell+8.9));
    float d=length(fract(p)-sp);
    float sizeH=starHash(cell+5.7), big=sizeH*sizeH*sizeH;
    float radius=mix(0.05,0.17,big);
    float core=smoothstep(radius,0.0,d); core*=core;
    float halo=smoothstep(radius*3.0,radius,d)*(big*big)*0.35;
    float shape=core+halo;
    float mag=(0.4+0.6*smoothstep(thresh,1.0,present))*mix(0.7,2.7,big);
    float twPhase=starHash(cell+23.5)*6.2831, twFreq=2.0+4.0*starHash(cell+47.1);
    float tw=0.7+0.3*sin(t*twFreq+twPhase);
    float horizon=smoothstep(0.0,0.15,dir.y);
    vec3 tint=mix(vec3(0.80,0.88,1.0),vec3(1.0,0.93,0.82),starHash(cell+12.1));
    float bandDim=mix(1.6,mix(0.9,1.5,mwc),band);
    return tint*(shape*mag*tw*horizon*night*bandDim);
}

vec3 aurora(vec3 dir, vec3 sunDir, float t, float intensity, vec3 auroraCol)
{
    if(intensity<=0.0) return vec3(0.0);
    float night=1.0-smoothstep(-0.10,0.10,clamp(sunDir.y,-0.2,1.0));
    if(night<=0.0||dir.y<=0.04) return vec3(0.0);
    vec2 P=dir.xz/(dir.y+0.45);
    float along=P.x, across=P.y;
    float wave=0.40*sin(along*0.7+t*0.15)+0.30*cloudFbm(vec2(along*0.35-t*0.04,3.0));
    float phase=across*0.30+wave;
    float f=abs(fract(phase)-0.5);
    float ribbon=smoothstep(0.10,0.45,f);
    float stri=cloudFbm(vec2(along*6.0+t*0.25,across*1.2));
    float curtain=ribbon*(0.45+0.55*smoothstep(0.30,0.80,stri));
    float patches=0.65+0.35*smoothstep(0.25,0.85,cloudFbm(vec2(along*0.45+t*0.03,across*0.4+9.0)));
    float hcol=smoothstep(0.05,0.60,dir.y);
    vec3 bCol=auroraCol*vec3(0.60,0.15,0.90), tCol=auroraCol*vec3(0.30,0.90,0.70);
    vec3 col=mix(mix(bCol,auroraCol,smoothstep(0.0,0.5,hcol)),tCol,smoothstep(0.5,1.0,hcol));
    float fade=smoothstep(0.03,0.16,dir.y)*(1.0-smoothstep(0.78,1.0,dir.y));
    return col*(curtain*patches*fade*intensity*night*5.0);
}

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
    float tex=sky.hasMoonTex!=0?texture(uMoonTex,q*0.5+0.5).r:1.0;
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

    vec3 col = skyColor(dir, sky.sunDir);
    float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(sky.sunDir).y, -0.2, 1.0));
    if (nightF > 0.0) {
        vec3 cdir = celestialDir(dir, sky.timeOfDay);
        col += starField(dir, cdir, sky.sunDir, sky.time, sky.milkyWay);
        col += nebula(dir, cdir, sky.sunDir, sky.nebula, sky.nebulaColor);
        col += aurora(dir, sky.sunDir, sky.time, sky.aurora, sky.auroraColor);
        col += moonDisk(dir, sky.sunDir);
    }
    col = applyClouds(col, dir, sky.sunDir, sky.time, sky.cloudCoverage, sky.sunColor, sky.wind);
    col += sky.flash * vec3(0.85, 0.90, 1.0);
    FragColor = vec4(col, 1.0);
}
