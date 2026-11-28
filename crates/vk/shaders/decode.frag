#version 450
// Palette decode (M-VK3b): C64 colour indices -> RGB picture. All colour maths of the picture
// happens here: palette, saturation, brightness, contrast, gamma and the optional C64 PAL/NTSC
// pass (Denise's c64_yuv model: per-line phase error, PAL delay line with Hanover bars, luma
// rise/fall delay, chroma bandwidth). Output: 8-bit picture texture, input of blending/presets.
layout(set = 0, binding = 0) uniform utexture2D indices;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) out vec4 color;

// keep in sync with `DecodePush` (crates/vk/src/renderer.rs)
layout(push_constant) uniform Push {
    uint palette[16];   // 0xRRGGBB
    float brightness;   // offset on the 0..255 scale
    float contrast;     // factor
    float saturation;   // factor
    float gamma;        // exponent
    uint flags;         // 1 PAL pass, 2 NTSC, 4 first picture line is odd
    float phaseError;   // radians
    float hanover;      // -50..50 (PAL delay line weighting of odd/even lines)
    float lumaRise;     // pixels (<= 1: instant)
    float lumaFall;
    int chromaTaps;     // FIR taps per side (0 = off)
    float cutoffU;      // chroma cutoff in cycles per pixel
    float cutoffV;
} pc;

const float PI = 3.14159265358979323846;
ivec2 size;

uint indexAt(int x, int y) {
    ivec2 p = clamp(ivec2(x, y), ivec2(0), size - 1);
    return texelFetch(usampler2D(indices, samp), p, 0).r & 15u;
}

vec3 paletteRgb(uint i) {
    // constant indices only: a push constant array indexed by a non-uniform value reads wrong
    // entries on RADV (the whole wave gets one lane's value)
    uint c = 0u;
    for (uint k = 0u; k < 16u; k++)
        if (k == i)
            c = pc.palette[k];
    return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u));
}

// Denise's CPU RGB path (VideoManager::adjustPalette), 0..255
vec3 plain(uint i) {
    vec3 c = paletteRgb(i);
    float grey = (c.r + c.g + c.b) / 3.0;
    c = (c - grey) * pc.saturation + grey;
    c += pc.brightness;
    float C = pc.contrast * 100.0 - 100.0;
    c = ((255.0 + 2.0 * C) / 255.0) * c - C;   // Michelson contrast
    c = clamp(c, 0.0, 255.0);
    return pow(c / 255.0, vec3(pc.gamma));
}

bool ntsc() { return (pc.flags & 2u) != 0u; }

// Denise's luma/chroma colour (convertPaletteToLumaChroma -> convertLumaChromaToRGB, 10 bit)
// and the c64_yuv encoding pass: YUV (PAL) / YIQ (NTSC) with the phase error of the line
vec3 encode(uint i, bool evenLine) {
    vec3 c = paletteRgb(i);
    vec3 lc;
    if (ntsc()) {
        lc = vec3(dot(c, vec3(0.23485876230514607, 0.6335007388077467, 0.13164049888710716)),
                  dot(c, vec3(0.4409594767911895, -0.27984362502847304, -0.16111585176271648)),
                  dot(c, vec3(0.14630060102591497, -0.5594814826856017, 0.4131808816596867)));
    } else {
        float y = dot(c, vec3(0.299, 0.587, 0.114));
        lc = vec3(y, (c.b - y) * 0.493, (c.r - y) * 0.877);
    }
    lc.yz *= pc.saturation * pc.contrast;
    lc.x = (lc.x + pc.brightness) * pc.contrast;
    vec3 rgb;
    if (ntsc())   // Sony CXA2025AS decoder matrix
        rgb = vec3(lc.x + 1.630 * lc.y + 0.317 * lc.z,
                   lc.x - 0.378 * lc.y - 0.466 * lc.z,
                   lc.x - 1.089 * lc.y + 1.677 * lc.z);
    else
        rgb = vec3(lc.x + (1.0 / 0.877) * lc.z,
                   lc.x - 0.3939307027516405 * lc.y - 0.5808092090310976 * lc.z,
                   lc.x + (1.0 / 0.493) * lc.y);
    rgb = clamp(floor(rgb + 0.5), -256.0, 767.0) / 255.0;

    vec3 e;
    if (ntsc())
        e = vec3(dot(rgb, vec3(0.23485876230514607, 0.6335007388077467, 0.13164049888710716)),
                 dot(rgb, vec3(0.4409594767911895, -0.27984362502847304, -0.16111585176271648)),
                 dot(rgb, vec3(0.14630060102591497, -0.5594814826856017, 0.4131808816596867)));
    else
        e = vec3(dot(rgb, vec3(0.299, 0.587, 0.114)),
                 dot(rgb, vec3(-0.147407, -0.289391, 0.436798)),
                 dot(rgb, vec3(0.614777, -0.514799, -0.099978)));
    float s = sin(pc.phaseError), co = cos(pc.phaseError);
    if (!evenLine)   // PAL: V switched every other line
        s = -s;
    return vec3(e.x, e.y * co - e.z * s, e.z * co + e.y * s);
}

// raster line parity of picture row y (absolute, independent of the crop)
bool evenLine(int y) {
    return ntsc() || ((y + int((pc.flags >> 2) & 1u)) & 1) == 0;
}

// luma rise/fall delay over the three pixels to the left (c64_yuv lumaLatency)
float lumaDelay(int x, int y, bool even) {
    float rise = 1.0 / pc.lumaRise, fall = 1.0 / pc.lumaFall;
    float src = encode(indexAt(x - 3, y), even).x;
    float l = src, diff = 0.0;
    for (int k = 2; k >= 0; k--) {
        float target = encode(indexAt(x - k, y), even).x;
        if (src != target)
            diff = target - l;
        src = target;
        float dir = sign(target - l);
        l = dir > 0.0 ? min(l + diff * rise, target) : (dir < 0.0 ? max(l + diff * fall, target) : l);
    }
    return l;
}

// windowed sinc low pass weight (Blackman), cutoff in cycles per pixel
float firWeight(int k, float cutoff) {
    float n = float(pc.chromaTaps + 1);
    float x = float(k);
    float sinc = k == 0 ? 2.0 * cutoff : sin(2.0 * PI * cutoff * x) / (PI * x);
    float w = 0.42 + 0.5 * cos(PI * x / n) + 0.08 * cos(2.0 * PI * x / n);
    return sinc * w;
}

vec2 chroma(int x, int y, bool even) {
    if (pc.chromaTaps <= 0)
        return encode(indexAt(x, y), even).yz;
    vec2 sum = vec2(0.0), norm = vec2(0.0);
    for (int k = -pc.chromaTaps; k <= pc.chromaTaps; k++) {
        vec2 w = vec2(firWeight(k, pc.cutoffU), firWeight(k, pc.cutoffV));
        sum += encode(indexAt(x + k, y), even).yz * w;
        norm += w;
    }
    return sum / norm;
}

void main() {
    size = textureSize(usampler2D(indices, samp), 0);
    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);

    if ((pc.flags & 1u) == 0u) {
        color = vec4(plain(indexAt(x, y)), 1.0);
        return;
    }

    bool even = evenLine(y);
    float l = (pc.lumaRise > 1.0 || pc.lumaFall > 1.0) ? lumaDelay(x, y, even)
                                                       : encode(indexAt(x, y), even).x;
    vec2 c = chroma(x, y, even);
    vec3 rgb;
    if (ntsc()) {
        rgb = vec3(l + 1.630 * c.x + 0.317 * c.y,
                   l - 0.378 * c.x - 0.466 * c.y,
                   l - 1.089 * c.x + 1.677 * c.y);
    } else {
        // delay line: mixed with the previous line (the first line has none, it takes the next)
        int yb = y == 0 ? 1 : y - 1;
        vec2 cb = chroma(x, yb, evenLine(yb));
        float h = (100.0 - abs(pc.hanover)) / 100.0;
        float hAlt = pc.hanover > 0.0 ? (100.0 + pc.hanover) / 100.0 : 1.0;
        c = (c + cb) * (even ? hAlt : h) * 0.5;
        rgb = vec3(l + 1.140251 * c.y,
                   l - 0.39393070 * c.x - 0.58080921 * c.y,
                   l + 2.0283976 * c.x);
    }
    color = vec4(pow(clamp(rgb, 0.0, 1.0), vec3(pc.gamma)), 1.0);
}
