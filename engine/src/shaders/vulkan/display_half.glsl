// packHalf2x16 may round towards zero. Qt converts to nearest, ties to
// even. Round positive unit floats in integers, including half subnormals.
uint halfBits(float v) {
    if (v == 0.0) return 0u;
    uint bits = floatBitsToUint(v);
    uint exponent = bits >> 23u;
    uint shift = max(13u, 126u - min(126u, exponent));
    uint significand = (bits & 0x7fffffu) | 0x800000u;
    uint rounded = (significand + ((1u << (shift - 1u)) - 1u) + ((significand >> shift) & 1u)) >> shift;
    return rounded + (exponent > 113u ? ((exponent - 113u) << 10u) : 0u);
}
vec4 displayHalf(vec3 code) {
    vec3 value = code * (1.0 / 65535.0);
    // Match the existing RGBA16 -> Qt half-float conversion before filtering.
    vec2 rg = unpackHalf2x16(halfBits(value.r) | (halfBits(value.g) << 16u));
    vec2 ba = unpackHalf2x16(halfBits(value.b) | 0x3c000000u);
    return vec4(rg, ba);
}
