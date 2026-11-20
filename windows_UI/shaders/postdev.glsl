// Post-Dev grading in the print encoding; CPU counterpart: Grade::pixel.
//
// Runs in the canvas fragment shader after it samples the frame and
// before the display LUT. The arithmetic is PostDev.cpp's Grade::pixel line for
// line; spk_qt_postdev_gpu renders this file through QRhi and holds it to the
// CPU path. Read PostDev.hpp's design note before wiring it in.
//
// Include it after `#version 440` (qsb: `#extension GL_GOOGLE_include_directive
// : enable` then `#include "postdev.glsl"`). It declares no resources and no
// block: the includer owns its bindings, because a Qt Quick material has one
// uniform buffer and Post-Dev must live inside it.
//   * `PostDev` below is a std140 struct of ten vec4 (160 bytes). Make it a
//     member of the uniform block at a 16-byte-aligned offset and fill it with
//     postdev::std140() (PostDev.hpp lists every float). Do not name the
//     member `postdev` -- that is the function.
//   * `curves` is a 2-D texture, 256 x 5, R32F (RGBA32F works too: only .r is
//     read), holding postdev::curveTables() row-major: rows rgb, luma, red,
//     green, blue. It is read with texelFetch, so it needs no filtering and
//     the weights are exact floats -- a sampler's carry 8 fractional bits on
//     D3D11, and a 32-bit float texture need not be filterable at all.
//   * The guide holds the local exposure coefficients (r,g) and whole-print L* anchors (b,a).
//   * `rgb` is the frame in its own encoding; `sourceUv` is the sample's place on
//     the uncropped, unturned source in [0,1], which is where the vignette is.
// The result is clamped to [0,1] unless the grade is inactive (bypassed or
// identity), in which case `rgb` comes back untouched, as from Grade::apply.

struct PostDev {
    vec4 wbExposure;   // white-balance gains r, g, b; exposure scale gain^(1/2.2)
    vec4 tone;         // contrast, brightness, saturation (1 = none), highlights
    vec4 range;        // shadows, black point, white point, mid-grey (encoded)
    vec4 cbMaster;     // colour-balance offset r, g, b; unused
    vec4 cbShadows;
    vec4 cbMidtones;
    vec4 cbHighlights;
    vec4 cbLum;        // luminance: master, shadows, midtones, highlights
    vec4 vignette;     // amount (-1..1), midpoint (0..1), unused, unused
    vec4 switches;     // active, curves active, exposure active, working tone space
};

// Shaders.metal:119, Rec. 709 weights on the encoded values.
float postdevLuma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Shaders.metal:136-143: mid-grey at 0.5, each half its own straight line.
float postdevTonePosition(float l, float midGrey)
{
    float m = clamp(midGrey, 0.0, 1.0);
    if (m <= 0.0 || m >= 1.0) m = 0.5;
    return (l <= m) ? 0.5 * l / m : 0.5 + 0.5 * (l - m) / (1.0 - m);
}

float decodeChannel(float v, int space)
{
    v = max(0.0, v);
    if (space==3) return pow(v,1.8);
    if (space==1) return v < 1.0/32.0 ? v/16.0 : pow(v,1.8);
    return v <= .04045 ? v/12.92 : pow((v+.055)/1.055,2.4);
}
float encodeChannel(float v, int space)
{
    if (space==3) return pow(v,1.0/1.8);
    if (space==1) return v < 1.0/512.0 ? v*16.0 : pow(v,1.0/1.8);
    return v <= .0031308 ? v*12.92 : 1.055*pow(v,1.0/2.4)-.055;
}
vec3 decodeTone(vec3 c,int space) {
    return vec3(decodeChannel(c.r,space),decodeChannel(c.g,space),decodeChannel(c.b,space));
}
vec3 encodeTone(vec3 c,int space) {
    return vec3(encodeChannel(c.r,space),encodeChannel(c.g,space),encodeChannel(c.b,space));
}
float contrastY(float y,float whiteDistance,float k,out float headroom) {
    if(whiteDistance<0.0) {
        headroom=whiteDistance/k;
        return 1.0-headroom;
    }
    const float p=.4949610761;
    if (y>.18) {
        float root=pow(y,1.0/3.0);
        float t=1.16*whiteDistance/((1.0+root+root*root)*(1.0-p));
        float d=(1.0-p)*t/(k+(1.0-k)*t)/1.16;
        headroom=d*(3.0-3.0*d+d*d);
        return 1.0-headroom;
    }
    float l=y<=216.0/24389.0 ? 9.032963*y : 1.16*pow(y,1.0/3.0)-.16;
    float t=l/p;
    l=p*t/(k+(1.0-k)*t);
    t=(l+.16)/1.16;
    float target=l<=.08 ? l/9.032963 : t*t*t;
    headroom=1.0-target;
    return target;
}
vec4 guideAt(sampler2D guide,vec2 uv) {
    ivec2 last=textureSize(guide,0)-1;
    vec2 p=clamp(uv*vec2(last+1)-.5,vec2(0),vec2(last));
    ivec2 a=ivec2(p), b=min(a+1,last);
    vec2 f=fract(p);
    return mix(mix(texelFetch(guide,a,0),texelFetch(guide,ivec2(b.x,a.y),0),f.x),
               mix(texelFetch(guide,ivec2(a.x,b.y),0),texelFetch(guide,b,0),f.x),f.y);
}
float pinnedSlope(float l, float low, float high, float k) {
    if (l <= low || l >= high) return 1.0;
    // Express distance from the black (or white) end in units of the
    // distance to middle grey. Bend only the interval [a,b] within that half.
    float pivot = .4949610761;
    bool dark = l < pivot;
    float x = dark ? l / pivot : (1 - l) / (1 - pivot);
    float a = dark ? low / pivot : (1 - high) / (1 - pivot);
    float b = dark ? min(high / pivot, 1.0) : min((1 - low) / (1 - pivot), 1.0);
    if (x <= a || x >= b) return 1.0;
    float t = (x - a) / (b - a);
    float den = k + (1 - k) * t;
    // Equivalent slope for the original bend, evaluated without subtracting
    // two nearly equal output lightnesses near the pivot.
    return 1 + (k - 1) * ((x - a) / (a * den + (x - a))) * ((b - x) / (1 - x)) / (b - a);
}
vec3 bendPrint(vec3 lin, vec3 weights, int space, float k) {
    float y=dot(lin,weights), headroom;
    float target=contrastY(y,dot(1.0-lin,weights),k,headroom);
    float gain=target/max(y,1e-12);
    float peak=max(lin.r,max(lin.g,lin.b));
    float spread=dot(peak-lin,weights)*gain;
    float available=max(0.0,headroom+max(0.0,peak-1.0));
    float chroma=spread>available ? available/spread : 1.0;
    return encodeTone(lin*gain*chroma+target*(1.0-chroma),space);
}
vec3 postdevTone(vec3 c, PostDev g, vec4 ab)
{
    float midGrey=g.range.w;
    int space=int(g.switches.w);
    vec3 weights=(space==1 || space==3) ? vec3(.2880402,.7118741,.0000857) : (space==2 ? vec3(.2289746,.6917385,.0792869) : vec3(.2126,.7152,.0722));
    bool local=g.range.x!=0.0 || g.tone.w!=0.0;
    float base=0.0, sourceY=0.0;
    if(local) {
        sourceY=dot(decodeTone(c,space),weights);
        base=ab.x*clamp(log2(max(sourceY,1e-12)/.184),-20.0,20.0)+ab.y;
    }
    // 1. white balance (scan-side gains on encoded values)
    c *= g.wbExposure.rgb;
    // 2. exposure in a pseudo-linear domain: pow(pow(c, 2.2) * gain, 1/2.2)
    //    is c * gain^(1/2.2) for c >= 0, precomputed on the CPU.
    if (g.switches.z > 0.5) c = max(c, vec3(0.0)) * g.wbExposure.w;
    if (g.tone.x != 0.0) {
        float k = 1.0 + g.tone.x * (1.2 + 6.8 * max(g.tone.x, 0.0));
        vec3 lin=decodeTone(c,space);
        c=bendPrint(lin,weights,space,k);
    }
    if (g.tone.y != 0.0) c = pow(max(c, vec3(0.0)), vec3(1.0 / (1.0 + g.tone.y * 0.8)));
    if(local) {
        vec3 lin=decodeTone(c,space);
        float y=dot(lin,weights);
        float amount=y<.18 ? -g.range.x*smoothstep(0.0,2.0,-base) : g.tone.w*smoothstep(0.0,2.0,base);
        if(amount!=0.0 && y<1.0) {
            float l=sourceY<=216.0/24389.0 ? 9.032963*sourceY : 1.16*pow(sourceY,1.0/3.0)-.16;
            float k=pinnedSlope(l,ab.z,ab.w,exp2(1.5*amount));
            if(k!=1.0) c=bendPrint(lin,weights,space,k);
        }
    }
    // 4. black / white point
    c = (c - g.range.y) / max(1.0 - g.range.y - g.range.z, 0.05);
    // 5. saturation
    float l = postdevLuma(c);
    c = l + (c - l) * g.tone.z;
    // 6. colour balance: master + three zones split at mid-grey
    float ls = clamp(postdevTonePosition(l, midGrey), 0.0, 1.0);
    float wS = (1.0 - ls) * (1.0 - ls);
    float wH = ls * ls;
    float wM = max(1.0 - wS - wH, 0.0);
    c += g.cbMaster.rgb + g.cbShadows.rgb * wS + g.cbMidtones.rgb * wM + g.cbHighlights.rgb * wH;
    c *= 1.0 + g.cbLum.x + g.cbLum.y * wS + g.cbLum.z * wM + g.cbLum.w * wH;
    return c;
}

// One curve row at x, as the Mac's linear clamp-to-edge sampler reads its
// 256-wide table (Shaders.metal:145-147): texel centres at (i + 0.5) / 256.
// postdev::curveLookup is the same arithmetic.
float postdevCurve(sampler2D curves, float x, int row)
{
    float p = clamp(x * 256.0 - 0.5, 0.0, 255.0);
    float i = min(floor(p), 254.0);
    float f = p - i;
    float a = texelFetch(curves, ivec2(int(i), row), 0).r;
    float b = texelFetch(curves, ivec2(int(i) + 1, row), 0).r;
    return a * (1.0 - f) + b * f;
}

vec3 postdev(vec3 rgb, vec2 sourceUv, PostDev g, sampler2D curves, sampler2D guide)
{
    if (g.switches.x < 0.5) return rgb;
    vec4 ab = (g.range.x!=0.0 || g.tone.w!=0.0) ? guideAt(guide,sourceUv) : vec4(1,0,0,1);
    vec3 c = postdevTone(rgb,g,ab);
    // 7. curves -- luma, then RGB master, then per channel
    if (g.switches.y > 0.5) {
        c = clamp(c, 0.0, 1.0);
        float ly = postdevLuma(c);
        float ly2 = postdevCurve(curves, ly, 1);
        c *= (ly > 1e-4) ? (ly2 / ly) : 1.0;
        c = clamp(c, 0.0, 1.0);
        c = vec3(postdevCurve(curves, c.r, 0), postdevCurve(curves, c.g, 0), postdevCurve(curves, c.b, 0));
        c = vec3(postdevCurve(curves, c.r, 2), postdevCurve(curves, c.g, 3), postdevCurve(curves, c.b, 4));
    }
    // 9. vignette -- radial about the source's centre, per axis
    if (g.vignette.x != 0.0) {
        float d = length((sourceUv - 0.5) * 2.0);
        float fall = smoothstep(g.vignette.y * 1.2, 1.5, d);
        c *= 1.0 + g.vignette.x * fall * 0.9;
    }
    return clamp(c, 0.0, 1.0);
}
