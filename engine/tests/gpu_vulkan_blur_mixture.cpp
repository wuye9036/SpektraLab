// Compare the first weighted blur to the original explicit-zero accumulation.
#include "pipeline/blur.hpp"
#include <cmath>
#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string error;
    std::unique_ptr<spk::gpu::Gpu> gpu(spk::gpu::Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }

    spk::Image input;
    input.w = 31;
    input.h = 27;
    std::vector<float> values(input.elements());
    for (size_t i = 0; i < values.size(); ++i) values[i] = (int(i * 17 % 131) - 65) / 63.f;
    input.buf = gpu->upload(values.data(), input.bytes(), error);
    spk::Image zero = input;
    zero.buf = gpu->alloc_zeroed(input.bytes(), error);
    if (!input.buf || !zero.buf) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }

    spk::Blur blur(gpu.get());
    const double sigmas[][3] = {{0, 0, 0}, {0.05, 0.10, 0.15}, {0.15, 0.16, 0.17}, {0, 0.6, 1.2}, {5, 0, 8}, {0.7, 4, 0}};
    for (const auto& sigma : sigmas) {
        spk::Blur::Component first{{0.25, 0.7, -0.13}, {sigma[0], sigma[1], sigma[2]}};
        spk::Image reference, actual;
        std::vector<float> expected(values.size()), got(values.size());
        if (!blur.gaussian(input, sigma, reference, error, 3.0, &zero, first.weight) ||
            !blur.mixture(input, {first}, actual, error) ||
            !gpu->read(reference.buf.get(), 0, expected.data(), input.bytes(), error) ||
            !gpu->read(actual.buf.get(), 0, got.data(), input.bytes(), error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (got != expected) {
            std::fprintf(stderr, "weighted blur differs for sigmas %g, %g, %g\n", sigma[0], sigma[1], sigma[2]);
            return 1;
        }
    }
    // Unit-sum scatter weights must preserve a uniform patch at either pitch.
    for (float& value : values) value = 0.5f;
    input.buf = gpu->upload(values.data(), input.bytes(), error);
    if (!input.buf) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    for (double pitchScale : {1.0, 2.0}) {
        spk::Blur::Component core{{0.25, 0.5, 0.75}, {0.1 * pitchScale, 0.1 * pitchScale, 0.1 * pitchScale}};
        spk::Blur::Component tail{{0.75, 0.5, 0.25}, {0.8, 1.0, 1.2}};
        spk::Image actual;
        std::vector<float> got(values.size());
        if (!blur.mixture(input, {core, tail}, actual, error) ||
            !gpu->read(actual.buf.get(), 0, got.data(), input.bytes(), error)) {
            std::fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
        for (float value : got) {
            if (!(std::abs(value - 0.5f) < 1e-6f)) {
                std::fprintf(stderr, "uniform patch changed to %g at scale %g\n", value, pitchScale);
                return 1;
            }
        }
    }
    return 0;
}
