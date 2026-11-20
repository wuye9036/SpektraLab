#include "embedded_lens.hpp"
#include "raw_decoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace spk::io {
namespace {
using Bytes = std::span<const std::uint8_t>;
std::uint32_t word(Bytes bytes) {
    std::uint32_t v = 0;
    for (auto b : bytes.first(4)) v = (v << 8) | b;
    return v;
}
double number(Bytes bytes) {
    std::uint64_t bits = 0;
    for (auto b : bytes.first(8)) bits = (bits << 8) | b;
    return std::bit_cast<double>(bits);
}
bool center(Bytes bytes, std::array<double, 2>& out) {
    out = {number(bytes), number(bytes.subspan(8))};
    return std::all_of(out.begin(), out.end(), [](double x) { return x >= 0 && x <= 1; });
}
// DNG 1.4, OpcodeList3. Tabulate the radial polynomial once. A malformed
// coefficient must never reach the float-to-index conversion in the sampler.
EmbeddedLens::Curve polynomial(Bytes bytes, int count, bool vignette) {
    std::array<double, 6> coeff{};
    coeff[0] = 1;
    for (int j = 0; j < count; ++j) coeff[j + int(vignette)] = number(bytes.subspan(8 * j));
    EmbeddedLens::Curve curve(1025);
    for (size_t i = 0; i < curve.size(); ++i) {
        const double r = double(i) / (curve.size() - 1);
        double v = 0;
        for (int k = count + int(vignette); k-- > 0;) v = v * r * r + coeff[k];
        if (!(v > 0 && v <= 32)) return {};
        curve[i] = float(v);
    }
    return curve;
}
float lookup(const EmbeddedLens::Curve& curve, double radius) {
    const double pos = std::min(radius, 1.0) * (curve.size() - 1);
    const size_t i = std::min(size_t(pos), curve.size() - 2);
    return std::lerp(curve[i], curve[i+1], float(pos - i));
}
struct Centre {
    double x, y, inv_radius;
};
Centre oriented(std::array<double, 2> c, int flip, int w, int h) {
    if (flip & 1) c[0] = 1 - c[0];
    if (flip & 2) c[1] = 1 - c[1];
    if (flip & 4) std::swap(c[0], c[1]);
    const double x = c[0] * w - .5, y = c[1] * h - .5;
    return {x, y, 1 / std::hypot(w * std::max(c[0], 1-c[0]), h * std::max(c[1], 1-c[1]))};
}
} // namespace

EmbeddedLens dng_lens(Bytes list) {
    EmbeddedLens lens;
    if (list.size() < 4) return {};
    auto count = word(list);
    list = list.subspan(4);
    while (count--) {
        if (list.size() < 16) return {};
        const auto id = word(list), size = word(list.subspan(12));
        list = list.subspan(16);
        if (size > list.size()) return {};
        const auto data = list.first(size);
        if (id == 1 && size >= 4) { // WarpRectilinear, radial profiles
            const auto planes = word(data);
            if ((planes != 1 && planes != 3) || size != 4 + 48 * planes + 16) return {};
            if (!center(data.last(16), lens.warp_center)) return {};
            for (unsigned i = 0; i < planes; ++i) {
                const auto coeff = data.subspan(4 + i * 48);
                if (number(coeff.subspan(32)) != 0 || number(coeff.subspan(40)) != 0) return {};
                lens.scales[i] = polynomial(coeff, 4, false);
                if (lens.scales[i].empty()) return {};
            }
        } else if (id == 3 && size == 56) { // FixVignetteRadial
            if (!center(data.last(16), lens.gain_center)) return {};
            lens.gain = polynomial(data, 5, true);
            if (lens.gain.empty()) return {};
        }
        list = list.subspan(size);
    }
    return lens;
}

void apply_lens_profile(DecodedRaw& frame, const EmbeddedLens& lens) {
    if (!lens.available() || frame.width < 2 || frame.height < 2) return;
    const int w = int(frame.width), h = int(frame.height);
    const auto warp = oriented(lens.warp_center, frame.metadata.raw_flip, w, h);
    const auto gain = oriented(lens.gain_center, frame.metadata.raw_flip, w, h);
    // Light falloff belongs to the sensor coordinates, before resampling.
    if (!lens.gain.empty()) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto factor = lookup(lens.gain, std::hypot(x-gain.x, y-gain.y) * gain.inv_radius);
            auto* rgb = frame.rgb.data() + (size_t(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c) rgb[c] *= factor;
        }
    }
    if (lens.scales[0].empty()) return;
    // Keep the corrected viewport inside the photograph on every plane.
    double zoom = 1;
    for (const auto& curve : lens.scales)
        if (!curve.empty()) zoom = std::max(zoom, double(*std::max_element(curve.begin(), curve.end())));
    std::vector<float> corrected(frame.rgb.size());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const double dx = (x-warp.x)/zoom, dy = (y-warp.y)/zoom;
        const double r = std::sqrt(dx*dx + dy*dy) * warp.inv_radius;
        const float common = lookup(lens.scales[0], r);
        for (int c = 0; c < 3; ++c) {
            const float scale = c && !lens.scales[c].empty() ? lookup(lens.scales[c], r) : common;
            const double sx = std::clamp(warp.x + dx*scale, 0., double(w-1));
            const double sy = std::clamp(warp.y + dy*scale, 0., double(h-1));
            const int x0 = int(sx), x1 = std::min(x0+1, w-1), y0 = int(sy), y1 = std::min(y0+1, h-1);
            const auto pixel = [&](int xx, int yy) { return frame.rgb[(size_t(yy)*w + xx)*3 + c]; };
            corrected[(size_t(y)*w + x)*3 + c] = std::lerp(
                std::lerp(pixel(x0,y0), pixel(x1,y0), float(sx-x0)),
                std::lerp(pixel(x0,y1), pixel(x1,y1), float(sx-x0)), float(sy-y0));
        }
    }
    frame.rgb.swap(corrected);
}
} // namespace spk::io
