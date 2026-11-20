#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace spk::io {
struct DecodedRaw;

// Measured source-radius / output-radius, sampled from centre to corner.
// Shared as immutable metadata by all preview tiers of an open RAW.
struct EmbeddedLens {
    using Curve = std::vector<float>;
    std::array<Curve, 3> scales; // one shared curve in slot 0, or one per plane
    Curve gain;
    std::array<double, 2> warp_center{0.5, 0.5}, gain_center{0.5, 0.5};
    bool available() const { return !scales[0].empty() || !gain.empty(); }
};

EmbeddedLens dng_lens(std::span<const std::uint8_t> opcode_list3);
void apply_lens_profile(DecodedRaw& frame, const EmbeddedLens& lens);
} // namespace spk::io
