#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace spk::tone_guide {
inline constexpr double scale = 0.03;
inline constexpr double epsilon = 0.5;
inline constexpr uint32_t gridLimit = 512;

// A box mean over a clamped window along one axis, normalised by the count
// the window holds (contrast_mask.cpp's boundary rule, repeated here rather
// than shared so the mask's research path is left exactly as it is).
inline void tone_box_1d(const std::vector<double>& in, std::vector<double>& out, uint32_t w, uint32_t h, int r,
                        bool horizontal) {
    out.assign(in.size(), 0.0);
    const uint32_t len = horizontal ? w : h, lines = horizontal ? h : w;
    std::vector<double> prefix(len + 1);
    for (uint32_t line = 0; line < lines; ++line) {
        auto at = [&](uint32_t k) { return horizontal ? size_t(line) * w + k : size_t(k) * w + line; };
        prefix[0] = 0.0;
        for (uint32_t k = 0; k < len; ++k)
            prefix[k + 1] = prefix[k] + in[at(k)];
        for (uint32_t k = 0; k < len; ++k) {
            const int lo = std::max(0, int(k) - r), hi = std::min(int(len) - 1, int(k) + r);
            out[at(k)] = (prefix[size_t(hi) + 1] - prefix[size_t(lo)]) / double(hi - lo + 1);
        }
    }
}

inline void tone_box(const std::vector<double>& in, std::vector<double>& out, uint32_t w, uint32_t h, int r) {
    std::vector<double> tmp;
    tone_box_1d(in, tmp, w, h, r, true);
    tone_box_1d(tmp, out, w, h, r, false);
}

// Return interleaved (a,b) for a self-guided filter of the cell means in stops.
inline std::vector<float> coefficients(const std::vector<double>& x, uint32_t gw, uint32_t gh) {
    const auto cells = std::max(gw, gh);
    // --- the base: He, Sun & Tang's guided filter, self-guided ------------
    // Two box passes of radius r have a standard deviation of ~0.816 r, so r
    // matches a Gaussian of the scale's sigma. The coefficients go to the GPU
    // and the base is `a * x + b` at each pixel's own `x`: smooth inside a
    // region, following the pixel across an edge, so no halo rings an edge.
    const double sigma = std::max(0.5, scale * double(cells));
    const int r = std::max(1, int(std::lround(sigma / 0.816)));
    std::vector<double> mean_x, x2(x.size()), mean_x2, ak(x.size()), bk(x.size()), a, b;
    for (size_t i = 0; i < x.size(); ++i)
        x2[i] = x[i] * x[i];
    tone_box(x, mean_x, gw, gh, r);
    tone_box(x2, mean_x2, gw, gh, r);
    for (size_t i = 0; i < x.size(); ++i) {
        const double var = std::max(0.0, mean_x2[i] - mean_x[i] * mean_x[i]);
        ak[i] = var / (var + epsilon);
        bk[i] = mean_x[i] - ak[i] * mean_x[i];
    }
    tone_box(ak, a, gw, gh, r);
    tone_box(bk, b, gw, gh, r);
    std::vector<float> coef(2 * x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        coef[2 * i] = float(a[i]);
        coef[2 * i + 1] = float(b[i]);
    }
    return coef;
}
} // namespace spk::tone_guide
