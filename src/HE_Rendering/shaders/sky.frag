#version 450

layout(location = 0) in vec2 vNDC;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform SkyEnv {
    mat4  invViewProj;
    vec3  sunDir;    float timeOfDay;
    vec3  sunColor;  float cloudCoverage;
    vec3  wind;      float time;
    vec3  auroraColor; float aurora;
    float milkyWay;  float flash; int hasMoonTex; float _pad;
} sky;

layout(set = 0, binding = 1) uniform sampler2D uMoonTex;

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
float starNoise3(vec3 p)
{
    vec3 i = floor(p), f = fract(p), u = f*f*(3.0-2.0*f);
    return mix(
        mix(mix(starHash(i),            starHash(i+vec3(1,0,0)), u.x),
            mix(starHash(i+vec3(0,1,0)), starHash(i+vec3(1,1,0)), u.x), u.y),
        mix(mix(starHash(i+vec3(0,0,1)), starHash(i+vec3(1,0,1)), u.x),
            mix(starHash(i+vec3(0,1,1)), starHash(i+vec3(1,1,1)), u.x), u.y), u.z);
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

vec3 applyClouds(vec3 baseSky, vec3 dir, vec3 sunDir, float t, float coverage, vec3 sunColor, vec3 wind)
{
    if(coverage<=0.0||dir.y<0.02) return baseSky;
    const int N=8;
    float sBase=1.0/max(dir.y,1e-3), sTop=2.6/max(dir.y,1e-3);
    float ds=(sTop-sBase)/float(N);
    float jitter=cloudHash(dir.xz*173.3+dir.y);
    float sunY=clamp(sunDir.y,-0.2,1.0);
    float day=smoothstep(-0.10,0.10,sunY);
    float dusk=smoothstep(-0.06,0.05,sunY)*(1.0-smoothstep(0.05,0.28,sunY));
    float costh=max(dot(dir,sunDir),0.0);
    float g=0.6,g2=g*g;
    float phase=(1.0-g2)/(12.566371*pow(max(1.0+g2-2.0*g*costh,1e-4),1.5));
    float T=1.0; vec3 L=vec3(0.0);
    for(int i=0;i<N;++i)
    {
        float s=sBase+(float(i)+jitter)*ds;
        vec3 pos=dir*s;
        float lo=mix(0.70,0.22,clamp(coverage,0.0,1.0));
        float dens=smoothstep(lo,lo+0.13,cloudFbm(pos.xz*1.2+wind.xz*t));
        if(dens>0.001)
        {
            float sh=exp(-smoothstep(lo,lo+0.13,cloudFbm((pos+sunDir*0.5).xz*1.2+wind.xz*t))*1.7);
            float powder=1.0-exp(-dens*3.0), lit=sh*powder;
            vec3 dayCol=mix(vec3(0.17,0.20,0.29),sunColor*1.12,lit);
            vec3 nightCol=mix(vec3(0.015,0.018,0.035),vec3(0.26,0.29,0.45),lit);
            vec3 cc=mix(nightCol,dayCol,day);
            cc=mix(cc,sunColor*vec3(1.25,0.55,0.28),dusk*lit*0.9);
            cc+=sunColor*(phase*sh*0.9*max(day,dusk));
            float hT=smoothstep(1.0,2.6,pos.y); cc*=mix(0.5,1.15,hT);
            float a=1.0-exp(-dens*ds*7.0);
            L+=T*a*cc; T*=1.0-a; if(T<0.02) break;
        }
    }
    float horizon=smoothstep(0.02,0.16,dir.y);
    T=1.0-(1.0-T)*horizon; L*=horizon;
    return baseSky*T+L;
}

void main()
{
    // Vulkan NDC: y-flip needed because vNDC comes from the VS which uses GL convention
    // The sky VS outputs p.y directly (no flip), so vNDC.y matches screen-y-down convention.
    // invViewProj accounts for the Vulkan clip-fix already present in the projection matrix.
    // Reconstruct world ray: NDC z=0 near, z=1 far.
    vec4 wp1 = sky.invViewProj * vec4(vNDC, 1.0, 1.0); // far
    vec4 wp0 = sky.invViewProj * vec4(vNDC, 0.0, 1.0); // near
    vec3 dir = wp1.xyz / wp1.w - wp0.xyz / wp0.w;

    vec3 col = skyColor(dir, sky.sunDir);
    float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(sky.sunDir).y, -0.2, 1.0));
    if (nightF > 0.0) {
        vec3 cdir = celestialDir(dir, sky.timeOfDay);
        col += starField(dir, cdir, sky.sunDir, sky.time, sky.milkyWay);
        col += aurora(dir, sky.sunDir, sky.time, sky.aurora, sky.auroraColor);
        col += moonDisk(dir, sky.sunDir);
    }
    col = applyClouds(col, dir, sky.sunDir, sky.time, sky.cloudCoverage, sky.sunColor, sky.wind);
    col += sky.flash * vec3(0.85, 0.90, 1.0);
    FragColor = vec4(col, 1.0);
}
