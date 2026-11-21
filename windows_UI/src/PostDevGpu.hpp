#pragma once
#include "PostDev.hpp"
#include <vector>
#include <mutex>

struct ID3D11Device;
struct ID3D11Texture2D;
namespace spk::desktop::postdev {
ID3D11Device* analysisDevice();
std::recursive_mutex& analysisMutex();
void analyzeDisplay(ID3D11Texture2D* texture, const QColorSpace& space, QSize bound, double histogramSamples,
                    QImage& navigator, QImage& guide, std::vector<float>& histogram, bool needGuide = true);
QImage gpuToneGuide(const QImage &source, ToneSpace space);
QImage gpuDownscaled(const QImage &source, QSize size);
std::vector<float> gpuHistogram(const QImage &image, int stride);
std::vector<float> gpuGradedHistogram(const QImage &image, const Grade &grade,
                                      const std::array<float, 6> &sourceMap,
                                      int stride);
void gpuGradeImage(QImage &image, const Grade &grade,
                   const std::array<float, 6> &sourceMap);
} // namespace spk::desktop::postdev
