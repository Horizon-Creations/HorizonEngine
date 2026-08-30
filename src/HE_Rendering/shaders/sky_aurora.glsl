// ── Aurora — EINE Quelle für alle Backends ───────────────────────────────────
// Die 3D-Aurora lag bis hierher nur in GLs kSkyFS. Vulkan hatte eine ältere,
// flache aurora(), D3D gar keine; gemessen war das die größte belegte Einzel-
// lücke im Himmel: 63,9 % der Bytes gegen die GL-Referenz, auf beiden Backends.
//
// Wie sky_core.glsl wird die Datei von OpenGL an einem Marker gespleißt
// (//#SKYAURORA#, siehe injectSkyFunc), von Vulkan per #include gezogen und für
// D3D nach HLSL übersetzt. Wer sie ändert, ändert sie für alle drei.
//
// ZWEI DINGE, die sie portabel halten und nicht angetastet werden dürfen:
//
//  1. applyAurora3D nimmt die Fragmentkoordinate als PARAMETER. In GL stand hier
//     skyIgn(gl_FragCoord.xy) direkt im Rumpf — das ist ein Stage-Builtin, das
//     HLSL nicht kennt, und SPIRV-Cross macht daraus eine dateiglobale Static,
//     die beim Herausschneiden der Funktionen verlorenginge. Der Aufrufer gibt
//     gl_FragCoord.xy bzw. SV_Position.xy hinein.
//  2. Kein Sampler, kein uniform-Block: die Datei liest nur die u*-Namen, die
//     jeder Verbraucher selbst gegen seinen eigenen Konstantenpuffer definiert
//     (in Vulkan und D3D per #define gegen HE::SkyFrameParams, in GL sind es
//     lose Uniformen). Deshalb ist sie ohne Anpassung überall übersetzbar.
//
// Mitgebracht wird skyIgn: zwei Zeilen Interleaved-Gradient-Rauschen, das der
// Raymarch zum Dithern braucht und das sonst nirgends existierte.
float skyIgn(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

// Per-curtain pseudo-random parameter: curtain index k, slot s → [0,1). EVERY property of a
// curtain (placement, heading, length, thickness, meander, height in the slab, brightness) is
// drawn from this, so no two curtains repeat. The previous version shared one meander function
// and gave every band the SAME heading (all along world-X) — which is exactly why they read as
// a stack of parallel stripes instead of a display.
float auroraRnd(float k, float s)
{
	return starHash(vec3(k * 0.7331 + 1.13, s * 1.9137 + 7.71, 19.37));
}

// Ridged "vein" field. The zero-set of the DIFFERENCE of two decorrelated noise fields is a
// network of thin branching filaments that close on themselves — the classic marble/vein
// pattern. Threading it through a curtain breaks the sheet into a braided web instead of one
// smooth wall, which is what gives an active display its net-like filigree up close.
float auroraWeb(vec2 p)
{
	float a = cloudNoise(p);
	float b = cloudNoise(p * 1.43 + vec2(37.2, 11.9));
	return smoothstep(0.30, 0.0, abs(a - b));       // 1 on a vein, 0 in between
}

// Clip the ray interval [t0,t1] against the slab |x0 + dx*t| <= w (x is any linear coordinate
// along the ray). Returns false when the ray misses it entirely. Two of these — one across the
// curtain, one along it — cut the march down to the ribbon's own oriented box.
bool auroraSlab(float x0, float dx, float w, inout float t0, inout float t1)
{
	if (abs(dx) < 1e-7) return abs(x0) <= w;
	float ta = (-w - x0) / dx, tb = (w - x0) / dx;
	t0 = max(t0, min(ta, tb));
	t1 = min(t1, max(ta, tb));
	return t1 > t0;
}

// Aurora borealis — WORLD-ANCHORED volumetric curtains, built like the 3D cloud slab so they
// are PLACED IN THE WORLD (real parallax as the camera moves) and have NO azimuth seam
// (sampled at world XZ, not atan-azimuth).
//
// Each curtain is an INDEPENDENT FINITE RIBBON with its own centre, heading, length, thickness,
// meander and height inside the slab — all from auroraRnd(). Headings are biased tangential to
// the ring the curtain sits on (real arcs align with the auroral oval) but spread by ±74°, so
// curtains genuinely CROSS one another. That crossing, plus the auroraWeb() vein filigree
// inside each ribbon, is what makes the net: a quiet aurora is a single E-W arc, but near
// magnetic midnight the oval breaks into folds (~20 km), curls (<10 km) and spirals, with N-S
// structures running across the E-W arcs — see earthsky.org "Forms of aurora" and Springer,
// "Physical Processes of Meso-Scale, Dynamic Auroral Forms". The three meander sines are that
// same scale hierarchy (arc sweep → folds → curls) with per-curtain amplitude/frequency/phase.
//
// Depth comes from three things beyond the plain perspective: curtains sit at DIFFERENT
// altitudes with different vertical extents, aerial perspective dims and cools them with
// distance, and the ray striation is sheared along the geomagnetic field line so all rays are
// parallel in 3D and converge perspectively on the magnetic zenith (the corona). Colour follows
// the real emission altitudes — magenta N2+ fringe on the bottom edge, green 557.7 nm oxygen
// body, diffuse red/violet 630 nm top. Motion is layered: the arc drifts, surges race along it,
// the fine rays stream sideways, and each curtain pulses on its own clock.
//
// The march is clipped PER CURTAIN to that ribbon's own oriented box (three auroraSlab calls —
// altitude band, across, along) instead of marching the whole envelope once for all bands: far
// fewer steps and a much tighter fit, which is what pays for the extra curtains, the vein noise
// and the per-curtain altitudes. The Gaussian sheet
// is pre-filtered against the step size (widen σ, renormalise to keep the line integral) so an
// under-sampled distant curtain BLURS instead of beating into moiré.
//
// PURELY EMISSIVE: additive, no Beer's law, no light-march — so curtain order is irrelevant and
// each one can be marched on its own. The soft rolloff keeps overlapping curtains from clipping
// to white (FragColor is LDR). Returns the emission to ADD to the sky BEFORE the clouds (so the
// nearer cloud layer occludes it).
vec3 applyAurora3D(vec3 dir, vec3 camPos, float time, float intensity, vec3 colBase, vec3 colTop,
                   vec2 fragCoord)   // gl_FragCoord.xy — als Parameter, weil HLSL kein gl_FragCoord kennt
{
	if (intensity <= 0.0) return vec3(0.0);
	dir = normalize(dir);
	if (dir.y < 0.02) return vec3(0.0);                         // horizon → ray never reaches the curtains
	float night = 1.0 - smoothstep(-0.20, -0.02, clamp(normalize(uSunDir).y, -0.3, 1.0));
	if (night <= 0.0) return vec3(0.0);

	// World-space altitude envelope. Height control drives the curtain ELEVATION (kept high so
	// the parallax stays subtle — aurora is near-infinite; lower = more exaggerated parallax).
	// Each curtain picks its OWN altitude and vertical extent inside this envelope: real
	// curtains are not all parked on one shelf, and that spread is a large part of the depth
	// read, so the global bracket only has to contain the union of them.
	float altitude = mix(1500.0, 7000.0, clamp(uAuroraHeight, 0.0, 1.0));
	float invY  = 1.0 / dir.y;
	float tNear = max(altitude * 0.72 * invY, 0.0);
	float tFar  = altitude * 3.75 * invY;
	// The layout reaches out to ~13.5 × altitude, so rays that leave the envelope beyond that
	// can never hit a curtain — clamp there rather than at some arbitrary huge distance.
	float layoutR = altitude * 13.5;
	tFar = min(tFar, layoutR * 2.2);
	if (tFar <= tNear) return vec3(0.0);

	float frag = clamp(uAuroraFragment, 0.0, 1.0);
	float jit  = skyIgn(fragCoord);
	vec2  o    = camPos.xz;                                     // ray origin in world XZ
	vec2  dxz  = dir.xz;                                        // ray XZ velocity (NOT unit — |dxz| = cos(elev))

	vec3 acc = vec3(0.0);
	for (int k = 0; k < 22; ++k)
	{
		float fk = float(k);
		// ── Layout: stratified ring radius (near-zenith … horizon, one curtain per stratum,
		//    jittered inside it) and a golden-angle azimuth so they never clump on one side.
		float rr     = (fk + auroraRnd(fk, 0.0)) / 22.0;
		float radius = altitude * (0.18 + 13.3 * rr * rr);
		float phi    = fk * 2.39996323 + auroraRnd(fk, 1.0) * 1.9;
		vec2  cen    = vec2(cos(phi), sin(phi)) * radius;
		// ── Heading: tangential to the ring (the oval runs E-W) ± 74°, so neighbouring curtains
		//    run at genuinely different angles and overlap into a mesh instead of stacking up.
		float head = phi + 1.5707963 + (auroraRnd(fk, 2.0) - 0.5) * 2.6;
		vec2  tang = vec2(cos(head), sin(head));
		vec2  nrm  = vec2(-tang.y, tang.x);
		// ── Everything scales with the ring radius: a curtain twice as far away must be twice
		//    as long, thick and snaky to keep the same apparent size on the dome.
		float scl     = 0.16 * radius + 420.0;
		float halfLen = scl * (2.6 + 3.8 * auroraRnd(fk, 3.0));
		float sigma0  = scl * (0.05 + 0.08 * auroraRnd(fk, 4.0)); // curtains are THIN ribbons
		float a1 = scl * (0.55 + 0.85 * auroraRnd(fk,  5.0));   // broad arc sweep
		float a2 = scl * (0.18 + 0.34 * auroraRnd(fk,  6.0));   // folds
		float a3 = scl * (0.05 + 0.11 * auroraRnd(fk,  7.0));   // curls
		float f1 = (0.55 / scl) * (0.6 + 0.8 * auroraRnd(fk,  8.0));
		float f2 = (1.70 / scl) * (0.6 + 0.9 * auroraRnd(fk,  9.0));
		float f3 = (5.20 / scl) * (0.7 + 0.9 * auroraRnd(fk, 10.0));
		float p1 = auroraRnd(fk, 11.0) * 6.2831853;
		float p2 = auroraRnd(fk, 12.0) * 6.2831853;
		float p3 = auroraRnd(fk, 13.0) * 6.2831853;
		// ── This curtain's own altitude and vertical extent (see the envelope note above).
		float altK   = altitude * (0.72 + 0.62 * auroraRnd(fk, 14.0));
		float thickK = altK * (1.00 + 0.80 * auroraRnd(fk, 18.0));
		float hDec   = 4.6 - 2.4 * auroraRnd(fk, 15.0);         // how fast it fades upward
		float bright = 0.55 + 0.70 * auroraRnd(fk, 16.0);
		// ── Motion. A real display does three things at once: the whole arc drifts (the meander
		//    time terms), surges of brightness race ALONG the arc every few seconds, and the fine
		//    rays stream sideways. Speeds and directions are per-curtain, so the sky never pulses
		//    in unison. Pulsation is per-curtain, so it hoists out of the march entirely.
		float tph    = auroraRnd(fk, 19.0) * 6.2831853;
		float puls   = 0.80 + 0.20 * sin(time * (0.30 + 0.55 * auroraRnd(fk, 20.0)) + tph);
		float surgeF = (1.5 + 1.1 * auroraRnd(fk, 21.0)) / scl;             // surges per unit length
		float surgeV = (0.7 + 1.3 * auroraRnd(fk, 22.0)) * (auroraRnd(fk, 23.0) < 0.5 ? -1.0 : 1.0);
		float rayDrift = (0.5 + 1.1 * auroraRnd(fk, 24.0)) * (auroraRnd(fk, 25.0) < 0.5 ? -1.0 : 1.0);
		float fringeAmt = (0.35 + 0.55 * frag) * (0.45 + 0.55 * auroraRnd(fk, 26.0));
		// Per-curtain hue jitter, so a display is not one flat colour end to end.
		vec3 hueK = mix(vec3(0.92, 1.02, 0.94), vec3(1.07, 0.97, 1.06), auroraRnd(fk, 17.0));
		// Auroral rays follow the geomagnetic FIELD LINE, not local vertical. Every ray in the
		// sky is therefore parallel in 3D, and perspective makes them converge on the magnetic
		// zenith — that convergence IS the corona, and it is the strongest depth cue available
		// here. shearU is how far the field line slides along this curtain per unit of height.
		float shearU = dot(vec2(0.22, 0.13), tang);

		// ── Clip the ray to this ribbon's oriented box: its own altitude band, then
		//    |across| <= meander + 3σ, then |along| <= len.
		float u0 = dot(o - cen, tang), du = dot(dxz, tang);
		float v0 = dot(o - cen, nrm ), dv = dot(dxz, nrm );
		float halfW = a1 + a2 + a3 + sigma0 * 3.0;
		float t0 = tNear, t1 = tFar;
		if (!auroraSlab(-(altK + thickK * 0.5), dir.y, thickK * 0.5, t0, t1)) continue;
		if (!auroraSlab(v0, dv, halfW,                                t0, t1)) continue;
		if (!auroraSlab(u0, du, halfLen * 1.4,                        t0, t1)) continue;

		// Step so we get a few samples ACROSS the sheet. What matters is how fast the distance
		// to the centreline changes, and that has TWO parts: how fast the ray crosses the
		// curtain (dv) AND how fast the centreline itself slides away under it as the ray
		// travels along the curtain (the meander slope × du). Ignoring the second term leaves
		// near-edge-on rays badly undersampled — that is what rings distant curtains with moiré,
		// because those are exactly the rays with dv ≈ 0 but a large du. The slope is the RMS of
		// the three meander derivatives; scl cancels, so it is a pure number around 1.
		float mndSlope = sqrt(0.5 * (a1 * f1 * a1 * f1 + a2 * f2 * a2 * f2 + a3 * f3 * a3 * f3));
		float dRate = max(abs(dv) + mndSlope * abs(du), 0.02);  // |d| change per unit t
		int   Nk = int(clamp((t1 - t0) / (sigma0 * 1.1 / dRate), 6.0, 64.0));
		float ds = (t1 - t0) / float(Nk);
		float dAcross = ds * dRate;                             // how far d moves per step

		for (int i = 0; i < Nk; ++i)
		{
			float t   = t0 + (float(i) + jit) * ds;
			vec3  pos = camPos + dir * t;                       // WORLD position → real parallax
			float u   = dot(pos.xz - cen, tang);
			float e   = abs(u) / halfLen;
			if (e > 1.4) continue;
			// Density envelope toward the tips. This MUST decay smoothly to zero rather than
			// stop at a boundary: viewed near edge-on, the ray's chord through the ribbon jumps
			// from nothing to a long bright path across a hard |u| = halfLen plane, and that
			// plane projects to a straight line — the ribbon visibly tears off mid-sky. A flat
			// top with a steep-but-finite shoulder keeps the middle at full strength and reaches
			// ~0 by e = 1.2, so there is no plane to see.
			float e2 = e * e;
			float ends = exp(-3.0 * e2 * e2 * e2);
			float hf   = clamp((pos.y - camPos.y - altK) / thickK, 0.0, 1.0); // 0 = this curtain's foot
			// Vertical emission: sharp bright lower edge at this curtain's own foot, long
			// exponential fade upward (fall-streaks), and a soft top so the ribbon dissolves
			// instead of ending on a flat lid. The fade also steepens toward the tips, so an arc
			// thins out to a point rather than staying full height and then vanishing.
			float hDecE = hDec * (1.0 + 2.2 * (1.0 - ends));
			float Ev = smoothstep(0.0, 0.05, hf)
			         * exp(-hf * hDecE)
			         * (1.0 - smoothstep(0.62, 1.00, hf));
			if (Ev <= 0.002) continue;
			float v = dot(pos.xz - cen, nrm);
			// Centreline meander in this curtain's OWN frame: arc sweep + folds + curls, each
			// with its own amplitude, frequency, phase and drift speed.
			float mnd = a1 * sin(u * f1 + p1 + time * 0.21)
			          + a2 * sin(u * f2 + p2 - time * 0.33)
			          + a3 * sin(u * f3 + p3 + time * 0.52);
			float d = v - mnd;
			float distLOD = smoothstep(altitude * 1.5, layoutR, t);
			float sigmaG  = sigma0 * (1.0 + 0.8 * distLOD);
			// Pre-filter: never let the sheet be thinner than ~1.25 steps across, or the Gaussian
			// falls between samples and beats into moiré (and the per-pixel IGN jitter turns that
			// into visible dither). Widen it and renormalise so the line integral through the
			// sheet is unchanged — blur, not aliasing.
			float sigmaE = max(sigmaG, dAcross * 1.55);
			float sheet  = exp(-(d * d) / (2.0 * sigmaE * sigmaE)) * (sigmaG / sigmaE);
			if (sheet < 0.004) continue;
			// Ray/web structures are indexed by the FOOT of the field line through this sample,
			// not by the sample itself — that is what leans the whole striation along the field
			// and makes it converge toward the magnetic zenith instead of standing dead vertical.
			float uRay = u - hf * thickK * shearU;
			// Fine vertical RAY striation, streaming sideways along the curtain, and faded out
			// with distance so it does not alias into the far field. All detail frequencies are
			// in units of scl, so near and far curtains carry the SAME amount of structure —
			// absolute world frequencies would leave near ribbons smooth and alias far ones.
			// It also smooths out toward the top: the red 630 nm oxygen line up there has a ~110 s
			// radiative lifetime, so the high part of a curtain is genuinely diffuse, never rayed.
			float rays = 0.62 + 0.38 * cloudNoise(vec2(uRay / scl * 30.0 + fk * 17.0 - time * rayDrift,
			                                           hf * 2.2 + fk));
			rays = mix(rays, 1.0, max(distLOD, smoothstep(0.30, 0.80, hf)));
			// The braided vein web ON the curtain surface (along × height). This is what turns a
			// single ribbon into a net rather than a smooth wall; the fragmentation slider drives
			// how hard the gaps between the veins are punched out.
			float web = auroraWeb(vec2(uRay / scl * 6.0 + fk * 9.0 - time * 0.06 * rayDrift,
			                           hf * 1.3 + fk * 3.0 + time * 0.04));
			float weave = mix(1.0, 0.10 + 1.45 * web, 0.35 + 0.65 * frag);
			// Surges of brightness racing along the arc — the part that reads as "dancing".
			float surge = 0.70 + 0.55 * sin(u * surgeF - time * surgeV + tph);
			// Colour by altitude, following the real auroral spectrum: a magenta N2+ fringe on the
			// very bottom edge of an active curtain (427.8 nm plus N2 red, below ~100 km), the
			// green 557.7 nm oxygen body above it, and the diffuse red/violet 630 nm oxygen top
			// (>200 km). The fringe is derived from the user's top colour rather than hard-coded,
			// so it still tracks whatever palette the scene picked.
			vec3 cCol = mix(colBase, colTop, smoothstep(0.22, 0.70, hf));
			vec3 fringeCol = colTop * vec3(1.55, 0.62, 1.05) + vec3(0.10, 0.0, 0.06);
			cCol = mix(cCol, fringeCol, (1.0 - smoothstep(0.02, 0.19, hf)) * fringeAmt);
			cCol *= hueK;
			// Aerial perspective. The air between the viewer and a curtain tens of km away both
			// dims it and shifts it cool; without that every curtain reads as if it sat at the
			// same distance and the whole display goes flat. The trailing smoothstep takes the
			// contribution cleanly to zero before the tFar clamp so there is no cut.
			float aer  = exp(-t / (layoutR * 0.95));
			cCol = mix(cCol * vec3(0.58, 0.72, 1.02), cCol, aer);
			float fade = mix(0.35, 1.0, aer) * (1.0 - smoothstep(layoutR * 1.5, layoutR * 2.1, t));
			// Normalise by the curtain's OWN thickness, not the envelope: the line integral
			// through a Gaussian sheet is ∝ σ, so dividing by a shared height would make a fat
			// distant curtain many times brighter than a thin near one instead of just wider.
			acc += cCol * (sheet * Ev * rays * weave * ends * bright * puls * surge
			               * (ds / (sigma0 * 6.0)) * fade); // pure ADD (emissive, no extinction)
		}
	}
	// LUMINANCE-preserving rolloff (FragColor is LDR). A per-channel rolloff drives the
	// strongest channel into clipping first, so a saturated green curtain washes out to pale
	// mint long before it is actually bright — compressing on luminance instead holds the hue
	// all the way up, and only the genuinely hottest cores desaturate (which real bright aurora
	// does too). Same trick the nebula uses.
	// Each ray integrates through the WHOLE vertical colour ramp, so green and violet average
	// toward grey along it and the display reads as pale mint rather than the deep green/violet
	// the palette actually asks for. Push saturation back up about luminance (which the step
	// below then preserves) before compressing.
	float lum = dot(acc, vec3(0.30, 0.59, 0.11));
	acc = max(mix(vec3(lum), acc, 1.35), vec3(0.0));
	if (lum > 1e-5) acc *= (lum / (1.0 + lum * 0.80)) / lum;
	float horizonFade = clamp(dir.y * 8.0, 0.0, 1.0);          // mask the 1/dir.y blow-up
	return acc * (intensity * night * horizonFade * 1.50);
}
