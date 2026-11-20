// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play - adaptive sharpening of the luma plane.
//
// A Remote Play stream arrives compressed and scaled to 1080p, which leaves it softer
// than the console's own HDMI picture. This pass gives some of the crispness back. It
// is an unsharp mask with three guards, so it adds no artefacts of its own:
//   - flat areas are left alone, so compression noise and banding are not amplified
//   - edges that are already strong get less, so nothing is over-sharpened
//   - the result never leaves the range of the pixel's neighbourhood, so edges get
//     steeper without the bright or dark halo ordinary sharpening leaves next to them
//
// @STRENGTH@ is replaced when the shader is loaded (one hook per strength level).

//!HOOK LUMA
//!BIND HOOKED
//!DESC Robz Play adaptive sharpening
//!COMPONENTS 1

#define ROBZ_STRENGTH @STRENGTH@

vec4 hook() {
	//    b
	//  d e f
	//    h
	float e = HOOKED_tex(HOOKED_pos).r;
	float b = HOOKED_texOff(vec2( 0.0, -1.0)).r;
	float d = HOOKED_texOff(vec2(-1.0,  0.0)).r;
	float f = HOOKED_texOff(vec2( 1.0,  0.0)).r;
	float h = HOOKED_texOff(vec2( 0.0,  1.0)).r;

	float lo = min(min(min(b, d), min(f, h)), e);
	float hi = max(max(max(b, d), max(f, h)), e);
	float contrast = hi - lo;

	// how much this pixel stands out from its surroundings
	float detail = e - (b + d + f + h) * 0.25;

	float amount = ROBZ_STRENGTH;
	amount *= smoothstep(0.006, 0.030, contrast);       // nothing in flat areas
	amount *= 1.0 - smoothstep(0.20, 0.55, contrast);   // little on strong edges

	float sharpened = clamp(e + detail * amount, lo, hi);
	return vec4(sharpened, 0.0, 0.0, 1.0);
}
