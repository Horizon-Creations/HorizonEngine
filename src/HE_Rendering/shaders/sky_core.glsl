// ── Der analytische Himmelskern — EINE Quelle für alle Backends ──────────────
// Bis hierher lag dieser Kern dreimal im Baum: als C++-Stringliteral
// kSkyFuncGLSL in OpenGLRenderer.cpp, als Handspiegel kSkyFuncHLSL in
// D3D_Shared/HlslSources.h und als handgepflegte Kopie in shaders/sky.frag.
// Die beiden Kopien sind stehengeblieben, während GL weitergezogen ist: sie
// enthalten noch den alten handgestimmten Tag/Dämmerung/Nacht-Gradienten, also
// 43 statt 173 Zeilen und keine Streuungsintegrale. Der Kommentar über
// kSkyFuncHLSL behauptete dabei bis zuletzt, er spiegele GL "exactly".
//
// Deshalb liegt der Text jetzt genau einmal — hier. OpenGL bekommt ihn über
// einen generierten Header (sky_core_glsl.h) als dasselbe Stringliteral wie
// vorher, Vulkan über #include. Wer den Kern ändert, ändert ihn für beide;
// auseinanderlaufen können sie nicht mehr.
//
// Die Datei ist bewusst NUR Funktionsrumpf: kein #version, keine uniforms,
// keine Sampler. Sie wird in GL an den //#SKYFUNC#-Marker gespleißt und in
// Vulkan eingebunden, und beide Seiten bringen ihre eigenen Deklarationen mit.
// Einstiegspunkt ist skyColor(dir, sunDir); alles andere ist intern.
// ─────────────────────────────────────────────────────────────────────────────
// ---- Physically-based single-scattering atmosphere (Rayleigh + Mie + ozone) ----
// Compact fixed-step single scatter for a ground-level camera: 12 view samples,
// each with a 5-sample sun-transmittance march. Sunset reddening, the blue hour
// and the horizon's pale saturation all EMERGE from the optical-depth integrals
// instead of hand-tuned gradient blends. Mirrored in MSL + the CPU IBL bakes —
// keep all four copies in sync.
// "No hit" returns a NEGATIVE near distance. That sign matters: the caller's
// sun-visibility test is `atmoRaySphere(p, sunDir, Rg).x > 0.0` → shadowed, and a
// ray that misses the planet entirely is the one case where the sun is certainly
// VISIBLE. A positive miss sentinel therefore marked every such sample shadowed,
// which is most of the sky the moment the sun nears the horizon — atmoScatter
// collapsed to exactly zero at sunY = 0 and the sky snapped to black at sunset.
// The other two callers only test for a hit IN FRONT, so a negative sentinel is
// correct for them too.
vec2 atmoRaySphere(vec3 ro, vec3 rd, float R)
{
	float b = dot(ro, rd);
	float c = dot(ro, ro) - R * R;
	float d = b * b - c;
	if (d < 0.0) return vec2(-1.0e9, -1.0e9);
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}
vec3 atmoScatter(vec3 dir, vec3 sunDir)
{
	const float Rg = 6360.0e3, Ra = 6440.0e3;                // ground / atmosphere-top radius
	const vec3  bR = vec3(5.802e-6, 13.558e-6, 33.1e-6);     // Rayleigh scattering
	const float bM = 3.996e-6;                               // Mie scattering
	const vec3  bO = vec3(0.650e-6, 1.881e-6, 0.085e-6);     // ozone absorption
	const float HR = 8500.0, HM = 1200.0;                    // scale heights
	vec3 ro = vec3(0.0, Rg + 200.0, 0.0);
	vec2 tA = atmoRaySphere(ro, dir, Ra);
	if (tA.y <= 0.0) return vec3(0.0);
	float t0 = max(tA.x, 0.0), t1 = tA.y;
	vec2 tG = atmoRaySphere(ro, dir, Rg);
	if (tG.x > 0.0) t1 = min(t1, tG.x);                      // stop at the ground
	float ds = (t1 - t0) / 12.0;
	float mu = dot(dir, sunDir);
	float phR = 0.05968310 * (1.0 + mu * mu);                // Rayleigh phase 3/(16π)
	const float g = 0.76, g2 = g * g;
	float phM = 0.11936620 * ((1.0 - g2) * (1.0 + mu * mu)) /   // Cornette-Shanks
	            ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
	vec3  sumR = vec3(0.0), sumM = vec3(0.0);
	float odR = 0.0, odM = 0.0, odO = 0.0;                   // view-path optical depths
	for (int i = 0; i < 12; ++i)
	{
		vec3  p   = ro + dir * (t0 + (float(i) + 0.5) * ds);
		float hgt = length(p) - Rg;
		float dR  = exp(-hgt / HR) * ds;
		float dM  = exp(-hgt / HM) * ds;
		float dO  = max(0.0, 1.0 - abs(hgt - 25.0e3) / 15.0e3) * ds;  // ozone tent layer @25km
		odR += dR; odM += dM; odO += dO;
		if (atmoRaySphere(p, sunDir, Rg).x > 0.0) continue;  // sun below local horizon → shadowed
		float sl = atmoRaySphere(p, sunDir, Ra).y * 0.2;     // 5-sample sun march
		float sR = 0.0, sM = 0.0, sO = 0.0;
		for (int j = 0; j < 5; ++j)
		{
			vec3  q  = p + sunDir * ((float(j) + 0.5) * sl);
			float hq = length(q) - Rg;
			sR += exp(-hq / HR) * sl;
			sM += exp(-hq / HM) * sl;
			sO += max(0.0, 1.0 - abs(hq - 25.0e3) / 15.0e3) * sl;
		}
		vec3 tau = bR * (odR + sR) + (bM * 1.11) * (odM + sM) + bO * (odO + sO);
		vec3 tr  = exp(-tau);
		sumR += tr * dR;
		sumM += tr * dM;
	}
	vec3 L = (sumR * bR * phR + sumM * bM * phM) * 20.0;     // sun irradiance → engine exposure
	// Fake MULTIPLE scattering: single scatter alone leaves long grazing paths
	// yellow/dark at noon (the in-filled skylight is missing). Fill proportional
	// to how opaque the view path is, fading out toward sunset so dusk stays warm.
	vec3 Tcam = exp(-(bR * odR + (bM * 1.11) * odM + bO * odO));
	L += (vec3(1.0) - Tcam) * vec3(0.30, 0.42, 0.60) * (0.35 * smoothstep(0.0, 0.35, sunDir.y));
	return L;
}
vec3 skyColor(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	// The clamp above pins everything below -0.3, which is fine for the day/dusk
	// tints but useless for "how deep into the night are we" — at true midnight it
	// still reads -0.3. The two night ramps therefore use the RAW elevation, so
	// twilight actually reaches zero instead of leaving a permanent glow on the
	// midnight horizon.
	float sunYd = sunDir.y;
	float day  = smoothstep(-0.10, 0.10, sunY);                 // 0 night → 1 day
	float dusk = smoothstep(-0.14, 0.04, sunY)
	           * (1.0 - smoothstep(0.04, 0.26, sunY));
	// Handed over to only once the twilight wedge below has faded, so the two
	// never leave a dark gap between them.
	float toNight = 1.0 - smoothstep(-0.34, -0.14, sunYd);      // twilight vs deep night

	// Physically-based base sky: day blue, sunset reddening and the blue hour all
	// come from the single-scattering integral above. Below-horizon rays reuse the
	// horizon colour (the ground-haze blend takes over there) — without the clamp a
	// hard navy "ocean band" appears where the ray hits the planet after a short path.
	vec3 sky = atmoScatter(normalize(vec3(dir.x, max(dir.y, 0.004), dir.z)), sunDir);

	// ── Twilight wedge ──────────────────────────────────────────────────────
	// Once the sun is under the horizon the 12-step single-scatter march has
	// almost nothing left to integrate: what still lights the sky comes from
	// hundreds of kilometres away, high up, after several scattering events —
	// which is the whole of civil and nautical twilight. Without it the sky drops
	// to the night floor within a couple of degrees of sunset while the clouds
	// are still catching the sun, and the horizon reads as a hard black edge.
	// Put back as an explicit wedge: warm at the horizon toward the sun, violet
	// as it climbs, deep blue on the far side, fading out into the night floor.
	float twi = smoothstep(0.10, -0.01, sunY) * smoothstep(-0.36, -0.12, sunYd);
	if (twi > 0.0)
	{
		vec2  sunAz = normalize(sunDir.xz + vec2(1e-5));
		vec2  dxz   = dir.xz;
		float hlen  = length(dxz);
		// Straight up and straight down have no azimuth; normalising a ~zero
		// vector snaps to an arbitrary fixed heading, which would put the full
		// sun-side glow on the poles. Fade to neutral instead.
		float toward = (hlen > 1e-4)
			? clamp(dot(dxz / hlen, sunAz) * 0.5 + 0.5, 0.0, 1.0) : 0.5;
		toward = mix(0.5, toward, smoothstep(0.0, 0.06, hlen));
		float el    = dir.y;                              // SIGNED — see `above`
		float band  = exp(-max(el, 0.0) * 5.2);           // hugs the horizon
		float climb = clamp(max(el, 0.0) * 3.4, 0.0, 1.0);// horizon → overhead
		// The wedge is a SKY term: below the horizon it hands over to the ground
		// blend. Clamping el to 0 instead gave every downward ray the peak
		// horizon glow, and the ground lit up like a desert in the middle of
		// nautical twilight.
		float above = smoothstep(-0.22, -0.01, el);
		vec3  warm  = vec3(1.00, 0.45, 0.17);
		vec3  mid   = vec3(0.62, 0.34, 0.52);
		vec3  cool  = vec3(0.20, 0.30, 0.62);
		vec3  col   = mix(mix(warm, mid, smoothstep(0.0, 0.50, climb)),
		                  cool, smoothstep(0.35, 1.0, climb));
		col = mix(cool, col, toward * toward);     // anti-sun side stays blue
		sky += col * (twi * above * (0.065 + 0.34 * band * toward * toward));
	}

	// Deep-night floor (the scattering term → 0 once the sun is far below the
	// horizon): faint blue gradient so night reflections aren't pitch black.
	float h = clamp(dir.y, 0.0, 1.0);
	sky += mix(vec3(0.006, 0.009, 0.024), vec3(0.003, 0.005, 0.015), h) * toNight;

	// Below the horizon: ease into ground haze. `sky` still holds the HORIZON
	// colour down here (the dir.y clamp above), so the haze is built out of it —
	// the old fixed grey sat brighter than a twilight sky and drew a hard bright
	// band across the horizon line, and it stayed grey while the sky went warm.
	vec3 ground = mix(sky * 0.32, vec3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.20, dir.y));

	// Sun aureole ON TOP of the physical Mie glow — just the tight glare blooms now;
	// the broad golden scatter comes from the Cornette-Shanks phase itself.
	vec3  sunTint = mix(vec3(1.0, 0.58, 0.24), vec3(1.0, 0.96, 0.88),
	                    smoothstep(0.0, 0.28, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	float bloomDamp = mix(1.0, 0.28, dusk);                        // dimmer at dusk → no white blob
	sky += sunTint * (pow(s, 220.0)  * 0.9  * bloomDamp) * sunVis; // tight bloom
	sky += sunTint * (pow(s, 30.0)   * 0.12 * bloomDamp) * sunVis; // mid aureole

	// Moon: opposite the sun, fading in at night. The lit disk itself is drawn
	// (textured) in the sky pass; here we keep only the soft halo and a faint
	// fill so the night ambient/reflections aren't pitch black.
	// Opposite the sun in azimuth + elevation, but kept on the same hemisphere
	// (z sign) so it rises into the visible sky rather than behind the viewer.
	float night   = 1.0 - day;
	vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	float m       = max(dot(dir, moonDir), 0.0);
	vec3  moonTint= vec3(0.80, 0.86, 1.00);
	sky += moonTint * (pow(m, 60.0)   * 0.05) * night;          // soft halo
	sky += vec3(0.015, 0.018, 0.030) * night;                   // faint moonlit fill
	return sky;
}
