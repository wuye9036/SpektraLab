// Windows Post-Dev compute. Analysis can read the shared print directly.
// Host images and reduced surfaces use a bounded texture cache.
#include "PostDevGpu.hpp"
#include "core/tone_guide.hpp"
#include <QColorSpace>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <deque>
#include <mutex>
#include <rhi/qshader.h>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>

static void loadToneResources() {
  Q_INIT_RESOURCE(spk_tone_kernels);
  Q_INIT_RESOURCE(spk_tone_grade);
}
namespace spk::desktop::postdev {
namespace {
using Microsoft::WRL::ComPtr;
using View = ComPtr<ID3D11ShaderResourceView>;
using Shader = ComPtr<ID3D11ComputeShader>;
void require(HRESULT hr, const char *op) {
  if (FAILED(hr))
    throw std::runtime_error(QString("%1 failed (0x%2)")
                                 .arg(op)
                                 .arg(uint(hr), 8, 16, QChar('0'))
                                 .toStdString());
}
QByteArray resource(const char *path) {
  QFile file(QString::fromLatin1(path));
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error(path);
  return file.readAll();
}
struct Buffer {
  ComPtr<ID3D11Buffer> data;
  View input;
  ComPtr<ID3D11UnorderedAccessView> output, raw;
};
struct Pass {
  int width, height, cols, rows;
  int left = 0, top = 0, x = 0, y = 0;
  int right = 0, bottom = 0, space = 0, radius = 0;
  int axis = 0, operation = 0, stride = 1, pad = 0;
};
struct GradePass {
  decltype(std140(std::declval<const Grade &>())) grade;
  float mapX[4], mapY[4];
  int tile[4];
  float size[2];
  int origin[2];
  int stride, pad[3];
};
class Compute {
public:
  std::recursive_mutex mutex;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Shader analyze, quantiles, box, resize, histogram, normalize, grade;
  View neutral;
  Compute() {
    loadToneResources();
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                              &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                              &context),
            "Post-Dev compute device");
    auto source = resource(":/shaders/postdev_analysis.hlsl");
    analyze = compile(source, "analyze");
    quantiles = compile(source, "percentiles");
    box = compile(source, "boxFilter");
    resize = compile(source, "downscale");
    histogram = compile(source, "histogram");
    normalize = compile(source, "normalizeHistogram");
    auto baked =
        QShader::fromSerialized(resource(":/shaders/postdev_image.comp.qsb"));
    grade = compile(
        baked.shader({QShader::HlslShader, QShaderVersion(50)}).shader(),
        "main");
    float ab[4] = {1, 0, 0, 1};
    neutral = texture(ab, 16, {1, 1}, DXGI_FORMAT_R32G32B32A32_FLOAT);
  }
  Shader compile(const QByteArray &source, const char *entry) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr,
                          nullptr, entry, "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
      throw std::runtime_error(
          errors ? static_cast<const char *>(errors->GetBufferPointer())
                 : entry);
    Shader out;
    require(device->CreateComputeShader(code->GetBufferPointer(),
                                        code->GetBufferSize(), nullptr, &out),
            entry);
    return out;
  }
  Buffer buffer(UINT count, bool vectors) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = count * (vectors ? 16 : 4);
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = vectors ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                          : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    d.StructureByteStride = vectors ? 16 : 0;
    Buffer b;
    require(device->CreateBuffer(&d, nullptr, &b.data), "compute buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Format = vectors ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    u.Buffer.NumElements = count;
    require(device->CreateUnorderedAccessView(b.data.Get(), &u, &b.output),
            "compute output");
    if (vectors) {
      require(device->CreateShaderResourceView(b.data.Get(), nullptr, &b.input),
              "compute input");
    } else {
      u.Format = DXGI_FORMAT_R32_TYPELESS;
      u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      require(device->CreateUnorderedAccessView(b.data.Get(), &u, &b.raw),
              "compute raw output");
      const UINT zero[4] = {};
      context->ClearUnorderedAccessViewUint(b.output.Get(), zero);
    }
    return b;
  }
  template <class Params>
  void dispatch(const Shader &shader, const Params &p, UINT groupsX,
                UINT groupsY,
                std::initializer_list<ID3D11ShaderResourceView *> inputs,
                std::initializer_list<ID3D11UnorderedAccessView *> outputs) {
    static_assert(sizeof(Params) % 16 == 0);
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = sizeof(p);
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&p, 0, 0};
    ComPtr<ID3D11Buffer> constants;
    require(device->CreateBuffer(&d, &init, &constants), "compute constants");
    context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->CSSetShader(shader.Get(), nullptr, 0);
    if (inputs.size())
      context->CSSetShaderResources(0, UINT(inputs.size()), inputs.begin());
    if (outputs.size())
      context->CSSetUnorderedAccessViews(0, UINT(outputs.size()),
                                         outputs.begin(), nullptr);
    context->Dispatch(groupsX, groupsY, 1);
    ID3D11ShaderResourceView *noInputs[3] = {};
    ID3D11UnorderedAccessView *noOutputs[3] = {};
    context->CSSetShaderResources(0, 3, noInputs);
    context->CSSetUnorderedAccessViews(0, 3, noOutputs, nullptr);
  }
  void read(const Buffer &buf, void *dst, UINT bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    require(device->CreateBuffer(&d, nullptr, &stage),
            "compute readback buffer");
    D3D11_BOX part{0, 0, 0, bytes, 1, 1};
    context->CopySubresourceRegion(stage.Get(), 0, 0, 0, 0, buf.data.Get(), 0,
                                   &part);
    D3D11_MAPPED_SUBRESOURCE map{};
    require(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map),
            "compute readback");
    std::memcpy(dst, map.pData, bytes);
    context->Unmap(stage.Get(), 0);
  }
  View texture(const void *data, UINT pitch, QSize size, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = size.width();
    d.Height = size.height();
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    ComPtr<ID3D11Texture2D> tex;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    require(device->CreateTexture2D(&d, &init, &tex), "source texture");
    View view;
    require(device->CreateShaderResourceView(tex.Get(), nullptr, &view),
            "source view");
    return view;
  }
  struct Entry {
    qint64 key;
    QRect rect;
    qsizetype bytes;
    View view;
  };
  std::deque<Entry> cache;
  View cached(const QImage &image, const QRect &rect) {
    for (auto i = cache.begin(); i != cache.end(); ++i)
      if (i->key == image.cacheKey() && i->rect == rect) {
        Entry hit = *i;
        cache.erase(i);
        cache.push_front(hit);
        return hit.view;
      }
    return {};
  }
  View upload(const QImage &image, const QRect &rect) {
    if (auto hit = cached(image, rect))
      return hit;
    DXGI_FORMAT format;
    switch (image.format()) {
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBX16FPx4:
      format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      break;
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBX32FPx4:
      format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      break;
    case QImage::Format_RGBA64:
    case QImage::Format_RGBX64:
      format = DXGI_FORMAT_R16G16B16A16_UNORM;
      break;
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
      format = DXGI_FORMAT_R8G8B8A8_UNORM;
      break;
    case QImage::Format_ARGB32:
    case QImage::Format_RGB32:
      format = DXGI_FORMAT_B8G8R8A8_UNORM;
      break;
    default:
      return upload(image.copy(rect).convertToFormat(QImage::Format_RGBA32FPx4),
                    QRect(QPoint(), rect.size()));
    }
    int step = image.depth() / 8;
    auto view =
        texture(image.constScanLine(rect.y()) + qsizetype(rect.x()) * step,
                UINT(image.bytesPerLine()), rect.size(), format);
    qsizetype bytes = qsizetype(rect.width()) * rect.height() * step;
    remember(image.cacheKey(), rect, bytes, view);
    return view;
  }
  void remember(qint64 key, QRect rect, qsizetype bytes, const View &view) {
    constexpr qsizetype budget = 384LL * 1024 * 1024;
    if (bytes <= budget) {
      qsizetype used = bytes;
      for (const auto &entry : cache)
        used += entry.bytes;
      while (!cache.empty() && (used > budget || cache.size() >= 16)) {
        used -= cache.back().bytes;
        cache.pop_back();
      }
      cache.push_front({key, rect, bytes, view});
    }
  }
  // A normal frame is uploaded once. Panoramas exceeding the texture limit
  // are partitioned on output-cell boundaries, including fractional edge
  // texels.
  template <class F>
  void tiles(const QImage &image, Pass p, F run, int maxCells = 2048, ID3D11ShaderResourceView* shared = nullptr) {
    const bool whole = shared || (p.width <= 16384 && p.height <= 16384 &&
                       image.sizeInBytes() <= 384LL * 1024 * 1024);
    int dx = std::max(1, std::min(maxCells, int(8192LL * p.cols / p.width)));
    int dy = std::max(1, std::min(maxCells, int(2048LL * p.rows / p.height)));
    for (p.y = 0; p.y < p.rows; p.y += dy)
      for (p.x = 0; p.x < p.cols; p.x += dx) {
        p.right = std::min(p.cols, p.x + dx);
        p.bottom = std::min(p.rows, p.y + dy);
        p.left = whole ? 0 : int(qint64(p.x) * p.width / p.cols);
        p.top = whole ? 0 : int(qint64(p.y) * p.height / p.rows);
        if (shared) { run(p,shared); continue; }
        QRect rect =
            whole
                ? image.rect()
                : QRect(
                      p.left, p.top,
                      int((qint64(p.right) * p.width + p.cols - 1) / p.cols) -
                          p.left,
                      int((qint64(p.bottom) * p.height + p.rows - 1) / p.rows) -
                          p.top);
        auto input = upload(image, rect);
        run(p, input.Get());
      }
  }
};
Compute &compute() {
  static Compute instance;
  return instance;
}
UINT groups(int n, int group) { return UINT((n + group - 1) / group); }
std::vector<float> finishHistogram(Compute &g, Buffer &bins) {
  g.dispatch(g.normalize, Pass{}, 1, 1, {}, {bins.output.Get()});
  std::vector<float> result(1024);
  g.read(bins, result.data(), 4096);
  return result;
}
} // namespace

QImage gpuToneGuideFrom(const QImage &image, QSize dimensions, ID3D11ShaderResourceView* shared, ToneSpace space) {
  auto &g = compute();
  std::lock_guard lock(g.mutex);
  double scale = std::min(1., double(tone_guide::gridLimit) /
                                  std::max(dimensions.width(), dimensions.height()));
  Pass p{dimensions.width(), dimensions.height(),
         std::max(1, qRound(dimensions.width() * scale)),
         std::max(1, qRound(dimensions.height() * scale))};
  p.space = int(space);
  p.radius =
      std::max(1, qRound(tone_guide::scale * std::max(p.cols, p.rows) / .816));
  auto bins = g.buffer(4098, false), a = g.buffer(p.cols * p.rows, true),
       b = g.buffer(p.cols * p.rows, true);
  g.tiles(image, p, [&](Pass tile, ID3D11ShaderResourceView *input) {
    g.dispatch(g.analyze, tile, groups(tile.right - tile.x, 8),
               groups(tile.bottom - tile.y, 8), {input},
               {bins.output.Get(), a.output.Get()});
  }, 2048, shared);
  g.dispatch(g.quantiles, p, 1, 1, {}, {bins.output.Get()});
  for (int pass = 0; pass < 4; ++pass) {
    p.axis = pass % 2;
    p.operation = int(pass == 1);
    g.dispatch(g.box, p, groups(p.cols, 8), groups(p.rows, 8),
               {nullptr, a.input.Get()}, {bins.output.Get(), b.output.Get()});
    std::swap(a, b);
  }
  QImage grid(p.cols, p.rows, QImage::Format_RGBA32FPx4);
  if (grid.isNull())
    throw std::bad_alloc();
  g.read(a, grid.bits(), UINT(grid.sizeInBytes()));
  return grid;
}

QImage gpuToneGuide(const QImage &image, ToneSpace space) {
  return gpuToneGuideFrom(image, image.size(), nullptr, space);
}

QImage gpuDownscaledFrom(const QImage &image, QSize dimensions, ID3D11ShaderResourceView* shared, QSize size) {
  auto &g = compute();
  std::lock_guard lock(g.mutex);
  Pass p{dimensions.width(), dimensions.height(), size.width(), size.height()};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = p.cols;
  d.Height = p.rows;
  d.MipLevels = d.ArraySize = 1;
  d.SampleDesc.Count = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Texture2D> texture, staging;
  ComPtr<ID3D11UnorderedAccessView> output;
  View resident;
  require(g.device->CreateTexture2D(&d, nullptr, &texture), "reduced texture");
  require(g.device->CreateUnorderedAccessView(texture.Get(), nullptr, &output),
          "reduced texture output");
  require(g.device->CreateShaderResourceView(texture.Get(), nullptr, &resident),
          "reduced texture input");
  g.tiles(image, p, [&](Pass tile, ID3D11ShaderResourceView *input) {
    g.dispatch(g.resize, tile, groups(tile.right - tile.x, 8),
               groups(tile.bottom - tile.y, 8), {input},
               {nullptr, nullptr, output.Get()});
  }, 2048, shared);
  d.BindFlags = 0;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  require(g.device->CreateTexture2D(&d, nullptr, &staging),
          "Qt preview readback");
  g.context->CopyResource(staging.Get(), texture.Get());
  QImage result(size, QImage::Format_RGBX16FPx4);
  if (result.isNull())
    throw std::bad_alloc();
  D3D11_MAPPED_SUBRESOURCE mapped{};
  require(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "map reduced texture");
  for (int y = 0; y < p.rows; ++y)
    std::memcpy(result.scanLine(y),
                static_cast<const uchar *>(mapped.pData) +
                    qsizetype(y) * mapped.RowPitch,
                qsizetype(p.cols) * 8);
  g.context->Unmap(staging.Get(), 0);
  result.setColorSpace(image.colorSpace());
  if (!shared) g.remember(result.cacheKey(), result.rect(), result.sizeInBytes(), resident);
  return result;
}

QImage gpuDownscaled(const QImage &image, QSize size) {
  return gpuDownscaledFrom(image, image.size(), nullptr, size);
}

std::vector<float> gpuHistogramFrom(const QImage &image, QSize dimensions, ID3D11ShaderResourceView* shared, int stride) {
  auto &g = compute();
  std::lock_guard lock(g.mutex);
  Pass p{dimensions.width(), dimensions.height(), dimensions.width(), dimensions.height()};
  auto bins = g.buffer(1025, false);
  // A cold histogram only samples every stride-th row. Let the upload's
  // row pitch skip the others instead of transferring a full camera frame.
  const int samplesX = (p.width + stride - 1) / stride;
  const int samplesY = (p.height + stride - 1) / stride;
  if (!shared && stride > 1 && p.width <= 16384 && samplesY <= 16384 &&
      image.format() == QImage::Format_RGBX16FPx4 &&
      !g.cached(image, image.rect())) {
    auto input =
        g.texture(image.constBits(), UINT(image.bytesPerLine() * stride),
                  {p.width, samplesY}, DXGI_FORMAT_R16G16B16A16_FLOAT);
    p.stride = stride;
    p.operation = 2;
    p.right = p.width;
    p.bottom = p.height;
    g.dispatch(g.histogram, p, groups(samplesX, 16), groups(samplesY, 16),
               {input.Get()}, {bins.output.Get()});
    return finishHistogram(g, bins);
  }
  g.tiles(image, p, [&](Pass tile, ID3D11ShaderResourceView *input) {
    tile.stride = stride;
    tile.x = (tile.x + stride - 1) / stride;
    tile.y = (tile.y + stride - 1) / stride;
    int nx = (tile.right + stride - 1) / stride - tile.x,
        ny = (tile.bottom + stride - 1) / stride - tile.y;
    if (nx && ny)
      g.dispatch(g.histogram, tile, groups(nx, 16), groups(ny, 16), {input},
                 {bins.output.Get()});
  }, 2048, shared);
  return finishHistogram(g, bins);
}

std::vector<float> gpuHistogram(const QImage &image, int stride) {
  return gpuHistogramFrom(image, image.size(), nullptr, stride);
}
std::recursive_mutex& analysisMutex() { return compute().mutex; }
ID3D11Device* analysisDevice() { return compute().device.Get(); }

void analyzeDisplay(ID3D11Texture2D* texture, const QColorSpace& space, QSize bound, double histogramSamples,
                    QImage& navigator, QImage& guide, std::vector<float>& histogram, bool needGuide) {
  D3D11_TEXTURE2D_DESC td{};
  texture->GetDesc(&td);
  QSize size(int(td.Width), int(td.Height));
  auto& g = compute();
  View view;
  require(g.device->CreateShaderResourceView(texture,nullptr,&view), "shared print analysis view");
  const double scale = std::min({1., double(bound.width())/td.Width, double(bound.height())/td.Height});
  QSize reducedSize(std::max(1,qRound(td.Width*scale)), std::max(1,qRound(td.Height*scale)));
  navigator = gpuDownscaledFrom({},size,view.Get(),reducedSize);
  navigator.setColorSpace(space);
  const auto mid = midGreyFor(space);
  if (mid && needGuide) guide = gpuToneGuideFrom({},size,view.Get(),Grade(Params{},*mid,space).uniforms().toneSpace);
  const int stride = std::max(1,int(std::sqrt(double(td.Width)*td.Height/histogramSamples)));
  histogram = gpuHistogramFrom({},size,view.Get(),stride);
}
namespace {
std::vector<float> render(const QImage &image, QImage *dest, const Grade &grade,
                          const std::array<float, 6> &map, int stride) {
  auto &g = compute();
  std::lock_guard lock(g.mutex);
  const auto &grid = grade.toneGuide();
  auto guide = grid.isNull() ? g.neutral : g.upload(grid, grid.rect());
  auto curves = g.texture(grade.tables().data(), curveTableSize * 4,
                          {curveTableSize, 5}, DXGI_FORMAT_R32_FLOAT);
  GradePass params{
      std140(grade), {map[0], map[1], map[2], 0}, {map[3], map[4], map[5], 0}};
  params.size[0] = float(image.width());
  params.size[1] = float(image.height());
  params.stride = stride;
  Pass p{image.width(), image.height(), image.width(), image.height()};
  constexpr int edge = 1024;
  auto result = g.buffer(
      stride ? 1025 : 4u * std::min(edge, p.width) * std::min(edge, p.height),
      false);
  g.tiles(
      image, p,
      [&](Pass tile, ID3D11ShaderResourceView *input) {
        const int w = tile.right - tile.x, h = tile.bottom - tile.y;
        params.origin[0] = tile.left;
        params.origin[1] = tile.top;
        params.tile[0] = tile.x;
        params.tile[1] = tile.y;
        params.tile[2] = w;
        params.tile[3] = h;
        g.dispatch(g.grade, params, groups(w, 16), groups(h, 16),
                   {input, curves.Get(), guide.Get()}, {result.raw.Get()});
        if (dest) {
          QImage part(w, h, QImage::Format_RGBA32FPx4);
          if (part.isNull())
            throw std::bad_alloc();
          g.read(result, part.bits(), UINT(part.sizeInBytes()));
          part = part.convertToFormat(dest->format());
          qsizetype row = qsizetype(w) * dest->depth() / 8;
          for (int y = 0; y < h; ++y)
            std::memcpy(dest->scanLine(tile.y + y) +
                            qsizetype(tile.x) * dest->depth() / 8,
                        part.constScanLine(y), row);
        }
      },
      edge);
  return stride ? finishHistogram(g, result) : std::vector<float>{};
}
} // namespace
void gpuGradeImage(QImage &image, const Grade &grade,
                   const std::array<float, 6> &map) {
  QImage output = image.copy();
  if (output.isNull())
    throw std::bad_alloc();
  render(image, &output, grade, map, 0);
  image = std::move(output);
}
std::vector<float> gpuGradedHistogram(const QImage &image, const Grade &grade,
                                      const std::array<float, 6> &map,
                                      int stride) {
  return render(image, nullptr, grade, map, stride);
}
} // namespace spk::desktop::postdev
