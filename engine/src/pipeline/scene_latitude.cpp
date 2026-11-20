#include "core/tone_guide.hpp"
// scene_latitude.cpp -- RFC-023's Scene Latitude Mapping, the pipeline half.
//
// Kept out of `pipeline.cpp` for the same reason `contrast_mask.cpp` is: the
// film chain there gained one guarded call in `film_scale_and_expose`, and
// with the feature off nothing in this file runs -- the render is the same
// dispatches with the same arguments as before.
//
// The node sits after auto-exposure and before `upsample`, so its `E = 0` is
// the metered mid-grey and Exp. Comp. is applied after it by `node_exposure`,
// unchanged (§5.1, §15.4: Exp. Comp. has a second consumer in the enlarger,
// so Scene Latitude does not carry it).
#include <algorithm>
#include <cmath>

#include "pipeline.hpp"
#include "latitude_fit.hpp"
#include "timer.hpp"

namespace spk {

bool Pipeline::scene_latitude_wanted() const {
    const SceneLatitudeParams& s = params_.camera.scene_latitude;
    // The print LUT bake renders a neutral chain; a scene transform has no
    // place in a table that stands for the paper.
    const bool first = s.active && (s.highlight_room > 0.0 || s.shadow_room > 0.0);
    const bool second = s.split > 0.0 && s.b_active && (s.b_highlight_room > 0.0 || s.b_shadow_room > 0.0);
    return (first || second) && !params_.debug.lut_mode;
}

namespace {

float norm_id(const std::string& norm) { return norm == "y" ? 1.0f : norm == "max" ? 2.0f : 0.0f; }

// --- the scene tone (SceneToneParams) -------------------------------------
//
// What an amount of 100 is: this many stops at the far end of its side.
constexpr double kSceneToneMaxStops = 2.0;
// Where a side reaches its full amount: a base this many stops from the
// metered mid-grey. A smoothstep from mid-grey, so a mid-tone is barely moved
// and the two sides meet at zero with zero slope.
constexpr double kSceneToneRampStops = 3.0;
// The base's spatial scale, as a fraction of the frame's long edge, and the
// guided filter's edge threshold in stops squared: a region whose brightness
// varies by more than ~0.7 stop inside the window is treated as edges the
// base follows, less as texture it smooths over. Fixed, not on the wire.
constexpr double kSceneToneScale = tone_guide::scale;
// The grid is canonical, as RFC-024's mask grid is: `kCellsPerSigma` cells
// per sigma on the long edge, never fewer than `kGridMinLongEdge`, never more
// than the image's own pixels -- so the meter's sample, the live tier and the
// full tier derive the same base from their own pixels.
constexpr double kSceneToneCellsPerSigma = 4.0;
constexpr uint32_t kSceneToneGridMin = tone_guide::gridLimit;
// The stop axis is held to +/- this; a black pixel sits on the floor.
constexpr float kSceneToneFloorStops = -20.0f;

}  // namespace

bool Pipeline::scene_tone_wanted() const {
    const SceneToneParams& t = params_.camera.scene_tone;
    // The print LUT bake renders a neutral chain, as for Scene Latitude.
    return (t.highlights != 0.0 || t.shadows != 0.0) && !params_.debug.lut_mode;
}

bool Pipeline::node_scene_tone(const Image& in, Image& out, std::string& error) {
    Timer t(this, "preprocess.scene_tone");
    const SceneToneParams& s = params_.camera.scene_tone;
    const float p[8] = {
        float(rgb_to_xyz_ae_.m[1][0]), float(rgb_to_xyz_ae_.m[1][1]), float(rgb_to_xyz_ae_.m[1][2]),
        float(slm::kMidgray),
        float(std::clamp(s.highlights, -100.0, 100.0) / 100.0 * kSceneToneMaxStops),
        float(std::clamp(s.shadows, -100.0, 100.0) / 100.0 * kSceneToneMaxStops),
        float(kSceneToneRampStops), kSceneToneFloorStops};

    // --- the grid: the mean stop-axis brightness of each cell -------------
    const uint32_t W = in.w, H = in.h, long_edge = std::max(W, H);
    const uint32_t wanted = std::max(kSceneToneGridMin, uint32_t(std::ceil(kSceneToneCellsPerSigma / kSceneToneScale)));
    const uint32_t cells = std::min(wanted, long_edge);
    const uint32_t gw = std::max(1u, uint32_t(std::lround(double(W) * cells / long_edge)));
    const uint32_t gh = std::max(1u, uint32_t(std::lround(double(H) * cells / long_edge)));
    gpu::BufferRef p_buf = gpu_->upload(p, sizeof p, error);
    gpu::BufferRef grid = gpu_->alloc(size_t(gw) * gh * sizeof(float), error);
    if (!p_buf || !grid) return false;
    const uint32_t meta[4] = {W, H, gw, gh};
    if (!gpu_->dispatch("spk_scene_tone_reduce",
                        {gpu::Arg::buf(in.buf), gpu::Arg::buf(p_buf), gpu::Arg::inline_bytes(meta, 4),
                         gpu::Arg::buf(grid)},
                        size_t(gw) * gh, error)) return false;
#ifdef _WIN32
    // Vulkan keeps the moments, the coefficients and the output on device.
    const size_t bytes = size_t(gw) * gh * 2 * sizeof(float);
    auto first = gpu_->alloc(bytes, error);
    auto second = gpu_->alloc(bytes, error);
    if (!first || !second) return false;
    auto coef_buf = grid;
    const auto radius = uint32_t(std::max(1L, std::lround(std::max(.5, kSceneToneScale * cells) / .816)));
    for (uint32_t pass = 0; pass < 4; ++pass) {
        const uint32_t filter_meta[] = {gw, gh, radius, pass};
        auto output = pass % 2 ? second : first;
        if (!gpu_->dispatch("spk_scene_tone_filter",
                            {gpu::Arg::buf(coef_buf), gpu::Arg::inline_bytes(filter_meta, 4), gpu::Arg::buf(output)},
                            size_t(gw) * gh, error)) return false;
        coef_buf = output;
    }
#else
    // The host needs the values: the one flush this node cannot avoid.
    if (!gpu_->flush(error)) return false;
    std::vector<float> grid_values(size_t(gw) * gh);
    if (!gpu_->read(grid.get(), 0, grid_values.data(), grid_values.size() * sizeof(float), error)) return false;
    std::vector<double> x(grid_values.begin(), grid_values.end());

    const auto coef = tone_guide::coefficients(x, gw, gh);
    gpu::BufferRef coef_buf = gpu_->upload(coef.data(), coef.size() * sizeof(float), error);
#endif
    if (!coef_buf || !alloc_like(in, out, error)) return false;

    // --- the move, per pixel ----------------------------------------------
    const uint32_t apply_meta[6] = {uint32_t(in.pixels()), W, H, 0u, gw, gh};
    return gpu_->dispatch("spk_scene_tone",
                          {gpu::Arg::buf(in.buf), gpu::Arg::buf(p_buf), gpu::Arg::buf(coef_buf),
                           gpu::Arg::inline_bytes(apply_meta, 6), gpu::Arg::buf(out.buf)},
                          in.pixels(), error);
}

bool Pipeline::node_scene_latitude(const Image& in, Image& out, std::string& error) {
    Timer t(this, "filming.expose.scene_latitude");
    const SceneLatitudeParams& s = params_.camera.scene_latitude;
    // A side that is off is a room of zero, which the kernel skips. On a
    // half-frame pair the second frame's curve applies from `split` along the
    // piece's long edge.
    const bool second = s.split > 0.0 && s.b_active;
    // A strip of RFC-020's executor is part of the piece: full-width, and wider
    // than it is tall on a piece held upright, so read by its own shape the
    // second curve fell across the picture instead of along it. The split is
    // measured along the frame: the plane's height and the strip's first row
    // are the executor's (`strip_plane_h_`, `band_row0_`; both 0 when it is not
    // running, which leaves a whole render as it was, to the bit).
    const uint32_t fw = in.w;
    const uint32_t fh = strip_plane_h_ ? strip_plane_h_ : in.h;
    const uint32_t row0 = band_row0_;
    const float p[19] = {
        float(s.highlight_knee), float(s.active ? s.highlight_room : 0.0),
        float(s.shadow_knee), float(s.active ? s.shadow_room : 0.0),
        float(s.rolloff), float(s.max_lift), norm_id(s.norm),
        float(rgb_to_xyz_ae_.m[1][0]), float(rgb_to_xyz_ae_.m[1][1]), float(rgb_to_xyz_ae_.m[1][2]),
        float(slm::kMidgray),
        float(fw), float(fh), float(s.split),
        float(s.b_highlight_knee), float(second ? s.b_highlight_room : 0.0),
        float(s.b_shadow_knee), float(second ? s.b_shadow_room : 0.0),
        float(row0)};
    gpu::BufferRef p_buf = gpu_->upload(p, sizeof p, error);
    if (!p_buf || !alloc_like(in, out, error)) return false;
    const uint32_t n[1] = {uint32_t(in.pixels())};
    return gpu_->dispatch("spk_scene_latitude",
                          {gpu::Arg::buf(in.buf), gpu::Arg::buf(p_buf), gpu::Arg::inline_bytes(n, 1),
                           gpu::Arg::buf(out.buf)},
                          in.pixels(), error);
}

bool Pipeline::scene_latitude_sample(const Image& in, double ev, std::vector<double>& E,
                                     std::string& error, const double* region) {
    // The node's own upstream nodes, as `measure_meter_evs` runs them: the
    // sample sees what the node would, decoded, cropped and turned.
    const uint32_t saved_long_edge = source_long_edge_;
    Progress* const saved_progress = progress_;
    progress_ = nullptr;
    Image cast, decoded, framed;
    const bool ok = node_input_cast(in, cast, error) &&
                    node_decode_input(cast, decoded, error) &&
                    node_geometry(decoded, framed, error);
    source_long_edge_ = saved_long_edge;
    if (!ok) { progress_ = saved_progress; return false; }

    // The auto-exposure gain as the node applies it (narrowed to float32 first),
    // then the node's norm. A pixel whose norm is not positive and finite takes
    // k = 1 in the node, so it is not on the curve's axis at all and is left
    // out of the statistic rather than parked at the floor.
    double gain = double(float(std::pow(2.0, ev)));
    // The scene's Highlights and Shadows sit between the meter and this node,
    // so the sample takes them too, on the device, after the same gain.
    Image sampled = framed;
    if (scene_tone_wanted()) {
        const double s[3] = {gain, gain, gain}, t0[3] = {0.0, 0.0, 0.0};
        Image exposed;
        const bool toned = blur_.affine(framed, s, t0, exposed, error) && node_scene_tone(exposed, sampled, error);
        if (!toned) { progress_ = saved_progress; return false; }
        gain = 1.0;
    }
    progress_ = saved_progress;
    std::vector<float> host;
    if (!read_back(sampled, host, error)) return false;
    const std::string& norm = params_.camera.scene_latitude.norm;
    const double yr = rgb_to_xyz_ae_.m[1][0], yg = rgb_to_xyz_ae_.m[1][1], yb = rgb_to_xyz_ae_.m[1][2];
    const size_t count = host.size() / 3;
    E.clear();
    E.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // A region (x0, y0, x1, y1, normalised to the frame): one frame of a
        // pair, measured by itself.
        if (region && framed.w > 0 && framed.h > 0) {
            const double x = (double(i % framed.w) + 0.5) / framed.w, y = (double(i / framed.w) + 0.5) / framed.h;
            if (x < region[0] || x >= region[2] || y < region[1] || y >= region[3]) continue;
        }
        const double r = std::max(double(host[3 * i]) * gain, 0.0);
        const double g = std::max(double(host[3 * i + 1]) * gain, 0.0);
        const double b = std::max(double(host[3 * i + 2]) * gain, 0.0);
        double v;
        if (norm == "y") v = yr * r + yg * g + yb * b;
        else if (norm == "max") v = std::max(r, std::max(g, b));
        else {
            const double s2 = r * r + g * g + b * b;
            v = s2 > 0.0 ? (r * r * r + g * g * g + b * b * b) / s2 : 0.0;
        }
        if (!(v > 0.0) || !std::isfinite(v)) continue;
        E.push_back(std::clamp(std::log2(v / slm::kMidgray), -slm::kDomainStops, slm::kDomainStops));
    }
    return true;
}

}  // namespace spk
