#include "PostDev.hpp"
#ifdef Q_OS_WIN
#include "PostDevGpu.hpp"
#endif
#include "../../engine/src/core/tone_guide.hpp"
#include <QCache>
#include <QtGui/qrgbafloat.h>
#include <mutex>
#include <QColorSpace>
#include <QImage>
#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace spk::desktop::postdev {
namespace {

// Metal's saturate/max return the non-NaN operand, as D3D's min/max do; a
// NaN pixel becomes 0 rather than an index into the curve table.
inline float saturate(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
inline float positive(float v) { return v > 0.0f ? v : 0.0f; }

// Shaders.metal:119, Rec. 709 weights on the encoded values.
inline float luma(const std::array<float, 3>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// Shaders.metal:136-143: mid-grey at 0.5, piecewise linear, identity at
// midGrey == 0.5; a degenerate pivot falls back to 0.5.
inline float tonePosition(float l, float midGrey) {
    float m = saturate(midGrey);
    if (m <= 0.0f || m >= 1.0f) m = 0.5f;
    return (l <= m) ? 0.5f * l / m : 0.5f + 0.5f * (l - m) / (1.0f - m);
}

inline float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// Rows are the unit of work, handed out in chunks from one counter: a pixel's
// result never depends on which thread computed it or on its neighbours.
template <class RowFn>
void forRows(int height, int threads, const RowFn& row) {
    constexpr int chunk = 16;
    const int chunks = (height + chunk - 1) / chunk;
    int workers = threads > 0 ? threads : int(std::thread::hardware_concurrency());
    workers = std::clamp(workers, 1, std::max(chunks, 1));
    if (workers == 1) {
        for (int y = 0; y < height; ++y) row(y);
        return;
    }
    std::atomic<int> next{0};
    auto work = [&] {
        for (int c = next.fetch_add(1); c < chunks; c = next.fetch_add(1))
            for (int y = c * chunk, end = std::min(height, y + chunk); y < end; ++y) row(y);
    };
    std::vector<std::thread> pool;
    pool.reserve(size_t(workers - 1));
    for (int i = 1; i < workers; ++i) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

using Rgb = std::array<float, 3>;

float decodeTone(float v, ToneSpace space) {
    v = positive(v);
    if (space == ToneSpace::ProPhotoGamma18) return std::pow(v, 1.8f);
    if (space == ToneSpace::ProPhoto) return v < 1.f / 32 ? v / 16 : std::pow(v, 1.8f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
float encodeTone(float v, ToneSpace space) {
    if (space == ToneSpace::ProPhotoGamma18) return std::pow(v, 1.f / 1.8f);
    if (space == ToneSpace::ProPhoto) return v < 1.f / 512 ? 16 * v : std::pow(v, 1.f / 1.8f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}
Rgb linearRgb(Rgb c, ToneSpace space) {
    for (auto& v : c)
        v = decodeTone(v, space);
    return c;
}
float luminance(const Rgb& linear, ToneSpace space) {
    if (space == ToneSpace::DisplayP3) return .2289746f * linear[0] + .6917385f * linear[1] + .0792869f * linear[2];
    return (space == ToneSpace::ProPhoto || space == ToneSpace::ProPhotoGamma18)
               ? .2880402f * linear[0] + .7118741f * linear[1] + .0000857f * linear[2]
               : luma(linear);
}
float toneStops(float y) { return std::clamp(std::log2(std::max(y, 1e-12f) / .184f), -20.f, 20.f); }

float lightness(float y) { return y <= 216.f / 24389 ? 9.032963f * y : 1.16f * std::cbrt(y) - .16f; }
// Near saturated ProPhoto yellow, Y is within 0.0001 of white. Compute
// the remaining headroom directly so rounding does not change its hue.
float contrastY(float y, float whiteDistance, float k, float& headroom) {
    if (whiteDistance < 0) {
        headroom = whiteDistance / k;
        return 1 - headroom;
    }
    static const float pivot = lightness(.18f);
    auto bend = [k](float t) { return t / (k + (1 - k) * t); };
    if (y > .18f) {
        const float root = std::cbrt(y);
        const float distanceL = 1.16f * whiteDistance / (1 + root + root * root);
        const float d = (1 - pivot) * bend(distanceL / (1 - pivot)) / 1.16f;
        headroom = d * (3 - 3 * d + d * d);
        return 1 - headroom;
    }
    const float l = pivot * bend(lightness(y) / pivot);
    const float t = (l + .16f) / 1.16f;
    const float target = l <= .08f ? l / 9.032963f : t * t * t;
    headroom = 1 - target;
    return target;
}

float pinnedSlope(float l, float low, float high, float k) {
    if (l <= low || l >= high) return 1;
    // Express distance from the black (or white) end in units of the
    // distance to middle grey. Bend only the interval [a,b] within that half.
    const float pivot = .4949610761f;
    const bool dark = l < pivot;
    const float x = dark ? l / pivot : (1 - l) / (1 - pivot);
    const float a = dark ? low / pivot : (1 - high) / (1 - pivot);
    const float b = dark ? std::min(high / pivot, 1.f) : std::min((1 - low) / (1 - pivot), 1.f);
    if (x <= a || x >= b) return 1;
    const float t = (x - a) / (b - a);
    const float den = k + (1 - k) * t;
    // Equivalent slope for the original bend, evaluated without subtracting
    // two nearly equal output lightnesses near the pivot.
    return 1 + (k - 1) * ((x - a) / (a * den + (x - a))) * ((b - x) / (1 - x)) / (b - a);
}

Rgb bendPrint(const Rgb& lin, ToneSpace space, float k) {
    Rgb c;
    const float y = luminance(lin, space);
    const float peak = *std::max_element(lin.begin(), lin.end());
    auto distance = [&](float v) { return luminance({v - lin[0], v - lin[1], v - lin[2]}, space); };
    float headroom;
    const float target = contrastY(y, distance(1), k, headroom);
    const float gain = target / std::max(y, 1e-12f);
    const float spread = distance(peak) * gain;
    // Preserve headroom introduced by exposure or white balance. An
    // already over-white channel must not force its neighbours to white.
    const float available = std::max(0.f, headroom + std::max(0.f, peak - 1));
    const float chroma = spread > available ? available / spread : 1.f;
    for (int i = 0; i < 3; ++i)
        c[i] = encodeTone(lin[i] * gain * chroma + target * (1 - chroma), space);
    return c;
}

template <class Sample> QImage makeGuide(int width, int height, ToneSpace space, Sample sample) {
    const int edge = std::max(width, height);
    const double scale = std::min(1., double(tone_guide::gridLimit) / edge);
    const int gw = std::max(1, int(std::lround(width * scale)));
    const int gh = std::max(1, int(std::lround(height * scale)));
    std::vector<double> cells(size_t(gw) * gh);
    // CPU reference for float-buffer callers; the Windows image path uses compute.
    std::vector<std::array<uint32_t, 4096>> bins(gh);
    forRows(gh, 0, [&](int cy) {
        const int y0 = int(qint64(cy) * height / gh), y1 = int(qint64(cy + 1) * height / gh);
        for (int cx = 0; cx < gw; ++cx) {
            const int x0 = int(qint64(cx) * width / gw), x1 = int(qint64(cx + 1) * width / gw);
            double sum = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    const float Y = luminance(linearRgb(sample(x, y), space), space);
                    sum += toneStops(Y);
                    ++bins[cy][int(saturate(lightness(Y)) * 4095 + .5f)];
                }
            cells[size_t(cy) * gw + cx] = sum / ((x1 - x0) * (y1 - y0));
        }
    });
    const uint64_t count = uint64_t(width) * height;
    const uint64_t trim = (count - 1) / 1000;
    uint64_t cumulative = 0;
    float low = 0, high = 1;
    for (int i = 0; i < 4096; ++i) {
        for (const auto& row : bins) cumulative += row[i];
        if (cumulative <= trim) low = float(i + 1) / 4095;
        if (cumulative >= count - trim) { high = float(i) / 4095; break; }
    }
    // Include the complete bins containing the selected ranks.
    low = std::min(1.f, low + .5f / 4095);
    high = std::max(0.f, high - .5f / 4095);
    const auto ab = tone_guide::coefficients(cells, gw, gh);
    QImage grid(gw, gh, QImage::Format_RGBA32FPx4);
    if (grid.isNull()) throw std::bad_alloc();
    for (int y = 0; y < gh; ++y) {
        auto* row = reinterpret_cast<float*>(grid.scanLine(y));
        for (int x = 0; x < gw; ++x) {
            const size_t i = 2 * (size_t(y) * gw + x);
            row[4 * x] = ab[i];
            row[4 * x + 1] = ab[i + 1];
            row[4 * x + 2] = low;
            row[4 * x + 3] = high;
        }
    }
    return grid;
}

std::array<float, 4> guideAt(const QImage& grid, float u, float v) {
    if (grid.isNull()) return {1, 0, 0, 1};
    const float x = std::clamp(u * grid.width() - .5f, 0.f, float(grid.width() - 1));
    const float y = std::clamp(v * grid.height() - .5f, 0.f, float(grid.height() - 1));
    const int x0 = int(x), x1 = std::min(x0 + 1, grid.width() - 1);
    auto row = [&](int iy, int ch) {
        const auto* p = reinterpret_cast<const float*>(grid.constScanLine(iy));
        return std::lerp(p[4 * x0 + ch], p[4 * x1 + ch], x - x0);
    };
    const auto* first = reinterpret_cast<const float*>(grid.constBits());
    std::array<float, 4> ab{0, 0, first[2], first[3]};
    for (int c = 0; c < 2; ++c)
        ab[c] = std::lerp(row(int(y), c), row(std::min(int(y) + 1, grid.height() - 1), c), y - int(y));
    return ab;
}

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().toStdString()); }

const Field& field(const char* path) {
    for (const auto& f : fields())
        if (qstrcmp(f.path, path) == 0) return f;
    fail(QStringLiteral("Post-Dev has no field ") + QLatin1String(path));
}

// The store's strictness (EditStore.cpp:70-105), with every key optional.
struct Reader {
    QStringList* unknown;

    void keys(const QJsonObject& object, std::initializer_list<const char*> known, const QString& label) const {
        for (auto it = object.begin(); it != object.end(); ++it) {
            const bool isKnown = std::any_of(known.begin(), known.end(),
                                             [&](const char* k) { return it.key() == QLatin1String(k); });
            if (isKnown) continue;
            if (!unknown) fail(QStringLiteral("Saved edits contain missing or unsupported fields in ") + label);
            unknown->append(label + "." + it.key());
        }
    }
    static QString path(const QString& label, const char* key) { return label + "." + QLatin1String(key); }
    double number(const QJsonObject& object, const char* key, const QString& label, const char* fieldPath) const {
        const auto& f = field(fieldPath);
        if (!object.contains(QLatin1String(key))) return f.initial;
        const auto value = object.value(QLatin1String(key));
        if (!value.isDouble()) fail(QStringLiteral("Saved edit field must be numeric: ") + path(label, key));
        const double result = value.toDouble();
        if (!std::isfinite(result) || result < f.low || result > f.high)
            fail(QStringLiteral("Saved edit field is outside its valid range: ") + path(label, key));
        return result;
    }
    bool boolean(const QJsonObject& object, const char* key, const QString& label, bool fallback) const {
        if (!object.contains(QLatin1String(key))) return fallback;
        const auto value = object.value(QLatin1String(key));
        if (!value.isBool()) fail(QStringLiteral("Saved edit field must be true or false: ") + path(label, key));
        return value.toBool();
    }
    std::optional<QJsonObject> object(const QJsonObject& parent, const char* key, const QString& label) const {
        if (!parent.contains(QLatin1String(key))) return std::nullopt;
        const auto value = parent.value(QLatin1String(key));
        if (!value.isObject()) fail(QStringLiteral("Saved edit field must be an object: ") + path(label, key));
        return value.toObject();
    }
    Curve curve(const QJsonObject& object, const QString& label) const {
        keys(object, {"points"}, label);
        if (!object.contains(QStringLiteral("points"))) fail(QStringLiteral("Saved edits are missing ") + label + ".points");
        const auto value = object.value(QStringLiteral("points"));
        const QString name = label + ".points";
        if (!value.isArray()) fail(QStringLiteral("Saved edit field must be a list of [x, y] points: ") + name);
        const auto list = value.toArray();
        if (list.size() < 2 || list.size() > Curve::maxPoints)
            fail(QStringLiteral("Saved edit curve must have 2 to %1 points: ").arg(Curve::maxPoints) + name);
        Curve curve;
        curve.points.clear();
        for (const auto& entry : list) {
            const auto pair = entry.toArray();
            if (!entry.isArray() || pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
                fail(QStringLiteral("Saved edit field must be a list of [x, y] points: ") + name);
            const double x = pair[0].toDouble(), y = pair[1].toDouble();
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
                fail(QStringLiteral("Saved edit curve point is outside the unit square: ") + name);
            if (!curve.points.empty() && x < curve.points.back().x)
                fail(QStringLiteral("Saved edit curve points must be sorted by x: ") + name);
            curve.points.push_back({x, y});
        }
        if (curve.points.front().x != 0 || curve.points.back().x != 1)
            fail(QStringLiteral("Saved edit curve must run from x = 0 to x = 1: ") + name);
        return curve;
    }
    ColorZone zone(const QJsonObject& object, const QString& label, const char* name) const {
        keys(object, {"hue", "saturation", "luminance"}, label);
        const QByteArray base = QByteArray("colorBalance.") + name + ".";
        return {number(object, "hue", label, (base + "hue").constData()),
                number(object, "saturation", label, (base + "saturation").constData()),
                number(object, "luminance", label, (base + "luminance").constData())};
    }
};

QJsonObject curveJson(const Curve& curve) {
    QJsonArray points;
    for (const auto& p : curve.points) points.append(QJsonArray{p.x, p.y});
    return {{QStringLiteral("points"), points}};
}
QJsonObject zoneJson(const ColorZone& z) {
    return {{QStringLiteral("hue"), z.hue}, {QStringLiteral("saturation"), z.saturation},
            {QStringLiteral("luminance"), z.luminance}};
}

} // namespace

// --- curves --------------------------------------------------------------------

bool Curve::isIdentity() const { return points == Curve{}.points; }

// CurveMath.swift:105-137, operation for operation.
double Curve::evaluate(double x) const {
    const auto& p = points;
    const int n = int(p.size());
    if (n < 2) return x;
    if (x <= p[0].x) return p[0].y;
    if (x >= p[n - 1].x) return p[n - 1].y;
    // Secants and tangents.
    std::vector<double> d(size_t(n - 1)), m(size_t(n), 0.0);
    for (int i = 0; i < n - 1; ++i) {
        const double dx = std::max(p[i + 1].x - p[i].x, 1e-6);
        d[i] = (p[i + 1].y - p[i].y) / dx;
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
    // Fritsch-Carlson, in place and in order: a tangent limited on segment i
    // is the one segment i + 1 tests.
    for (int i = 0; i < n - 1; ++i) {
        if (d[i] == 0) continue;
        const double a = m[i] / d[i], b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    // Locate the segment.
    int i = 0;
    while (i < n - 2 && x > p[i + 1].x) ++i;
    const double h = std::max(p[i + 1].x - p[i].x, 1e-6);
    const double t = (x - p[i].x) / h;
    const double t2 = t * t, t3 = t2 * t;
    const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
    const double h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    const double y = h00 * p[i].y + h10 * h * m[i] + h01 * p[i + 1].y + h11 * h * m[i + 1];
    return std::clamp(y, 0.0, 1.0);
}

double Curve::hitTolerance(double width, double height) {
    const double side = std::min(width, height);
    return side > 0 ? 10 / side : 0;
}

std::optional<int> Curve::indexNear(CurvePoint p, double tolerance) const {
    std::optional<int> best;
    double bestDistance = 0;
    for (int i = 0; i < int(points.size()); ++i) {
        const double d = std::hypot(points[i].x - p.x, points[i].y - p.y);
        if (d <= tolerance && (!best || d < bestDistance)) { best = i; bestDistance = d; }
    }
    return best;
}

bool Curve::isDeletable(int i) const { return points.size() > 2 && i > 0 && i < int(points.size()) - 1; }

// The Mac has no cap; this one stops at maxPoints so that every curve the
// editor can make is one the store will save.
int Curve::insert(CurvePoint p) {
    if (int(points.size()) >= maxPoints) return -1;
    const CurvePoint q{std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, 0.0, 1.0)};
    const auto at = std::find_if(points.begin(), points.end(), [&](const CurvePoint& c) { return c.x > q.x; });
    const int index = int(at - points.begin());
    points.insert(at, q);
    return index;
}

void Curve::move(int i, CurvePoint p) {
    if (i < 0 || i >= int(points.size())) return;
    CurvePoint q{std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, 0.0, 1.0)};
    if (i == 0) q.x = 0;
    else if (i == int(points.size()) - 1) q.x = 1;
    else {
        const double lo = points[i - 1].x + 0.01, hi = points[i + 1].x - 0.01;
        q.x = std::clamp(q.x, std::min(lo, hi), std::max(lo, hi));
    }
    points[i] = q;
}

void Curve::remove(int i) {
    if (isDeletable(i)) points.erase(points.begin() + i);
}

Curve& CurveSet::operator[](CurveChannel channel) {
    switch (channel) {
    case CurveChannel::rgb: return rgb;
    case CurveChannel::luma: return luma;
    case CurveChannel::red: return red;
    case CurveChannel::green: return green;
    case CurveChannel::blue: break;
    }
    return blue;
}
const Curve& CurveSet::operator[](CurveChannel channel) const {
    return const_cast<CurveSet&>(*this)[channel];
}
bool CurveSet::isIdentity() const {
    return rgb.isIdentity() && luma.isIdentity() && red.isIdentity() && green.isIdentity() && blue.isIdentity();
}

CurveTables curveTables(const CurveSet& curves) {
    CurveTables out{};
    for (int row = 0; row < 5; ++row) {
        const auto& curve = curves[CurveChannel(row)];
        for (int i = 0; i < curveTableSize; ++i)
            out[size_t(row * curveTableSize + i)] = float(curve.evaluate(double(i) / double(curveTableSize - 1)));
    }
    return out;
}

float curveLookup(const float* row, float x) {
    float p = x * 256.0f - 0.5f;
    p = p > 0.0f ? (p < 255.0f ? p : 255.0f) : 0.0f;
    const float i = std::min(std::floor(p), 254.0f);
    const int k = int(i);
    const float f = p - i;
    return row[k] * (1.0f - f) + row[k + 1] * f;
}

// --- the model -----------------------------------------------------------------

std::array<float, 3> ColorZone::rgbOffset() const {
    constexpr double pi = std::numbers::pi;
    const double h = hue * pi / 180;
    const double r = std::cos(h), g = std::cos(h - 2 * pi / 3), b = std::cos(h + 2 * pi / 3);
    const float s = float(saturation) * 0.25f;
    return {float(r) * s, float(g) * s, float(b) * s};
}

bool ColorBalance::isNeutral() const {
    return master.isNeutral() && shadows.isNeutral() && midtones.isNeutral() && highlights.isNeutral();
}

bool Params::isNeutral() const {
    Params defaults;
    defaults.enabled = enabled;
    return *this == defaults;
}

const std::vector<Field>& fields() {
    // Hue 0..360, saturation 0..1, luminance -1..1 per zone (AgentSchema.swift:
    // 220-226, ColorWheel.swift:181-184). The vignette's midpoint has no
    // slider on the Mac, only the agent's field.
    static const std::vector<Field> table{
        {"temperature", -100, 100, 0, 5}, {"tint", -100, 100, 0, 5},
        {"exposure", -3, 3, 0, 0.25}, {"contrast", -50, 50, 0, 5}, {"brightness", -50, 50, 0, 5},
        {"saturation", -100, 100, 0, 5}, {"highlights", -100, 100, 0, 5}, {"shadows", -100, 100, 0, 5},
        {"blackPoint", 0, 50, 0, 1}, {"whitePoint", 0, 50, 0, 1},
        {"vignette.amount", -100, 100, 0, 5}, {"vignette.midpoint", 0, 100, 50, 0},
        {"colorBalance.master.hue", 0, 360, 0, 0}, {"colorBalance.master.saturation", 0, 1, 0, 0},
        {"colorBalance.master.luminance", -1, 1, 0, 0},
        {"colorBalance.shadows.hue", 0, 360, 0, 0}, {"colorBalance.shadows.saturation", 0, 1, 0, 0},
        {"colorBalance.shadows.luminance", -1, 1, 0, 0},
        {"colorBalance.midtones.hue", 0, 360, 0, 0}, {"colorBalance.midtones.saturation", 0, 1, 0, 0},
        {"colorBalance.midtones.luminance", -1, 1, 0, 0},
        {"colorBalance.highlights.hue", 0, 360, 0, 0}, {"colorBalance.highlights.saturation", 0, 1, 0, 0},
        {"colorBalance.highlights.luminance", -1, 1, 0, 0}};
    return table;
}

// --- uniforms --------------------------------------------------------------------

std::optional<float> midGreyFor(const QColorSpace& space) {
    if (!space.isValid()) return std::nullopt;
    switch (space.transferFunction()) {
    case QColorSpace::TransferFunction::ProPhotoRgb: return proPhotoMidGrey;
    case QColorSpace::TransferFunction::SRgb: return srgbMidGrey;
    case QColorSpace::TransferFunction::Gamma:
        // A pure 1.8 curve differs from ROMM's only below 1/512.
        if (std::abs(space.gamma() - 1.8f) < 1e-6f) return proPhotoMidGrey;
        return std::nullopt;
    default: return std::nullopt;
    }
}

// Adjustments.swift:72-94 and Mask.swift:301-324, in Float as Swift does it.
Uniforms uniforms(const Params& p, float midGrey) {
    Uniforms u;
    const float t = float(p.temperature) / 100.0f, g = float(p.tint) / 100.0f;
    u.wbGain = {1.0f + 0.18f * t, 1.0f - 0.12f * g, 1.0f - 0.18f * t};
    u.exposureGain = float(std::pow(2.0, p.exposure));
    // pow(pow(x, 2.2) * g, 1/2.2) == x * g^(1/2.2) for x >= 0 (Shaders.metal:
    // 274-277): one multiply per channel instead of two pows, on both paths.
    u.exposureScale = float(std::pow(double(u.exposureGain), 1.0 / 2.2));
    u.contrast = float(p.contrast) / 100.0f;
    u.brightness = float(p.brightness) / 100.0f;
    u.saturation = 1.0f + float(p.saturation) / 100.0f;
    u.highlights = float(p.highlights) / 100.0f;
    u.shadows = float(p.shadows) / 100.0f;
    u.blackPoint = float(p.blackPoint) / 100.0f;
    u.whitePoint = float(p.whitePoint) / 100.0f;
    u.midGrey = midGrey;
    u.toneSpace = midGrey == proPhotoMidGrey ? ToneSpace::ProPhoto : ToneSpace::SRgb;
    const auto& cb = p.colorBalance;
    u.cbMaster = cb.master.rgbOffset();
    u.cbShadows = cb.shadows.rgbOffset();
    u.cbMidtones = cb.midtones.rgbOffset();
    u.cbHighlights = cb.highlights.rgbOffset();
    u.cbLum = {float(cb.master.luminance) * 0.25f, float(cb.shadows.luminance) * 0.25f,
               float(cb.midtones.luminance) * 0.25f, float(cb.highlights.luminance) * 0.25f};
    u.vignetteAmount = float(p.vignette.amount) / 100.0f;
    u.vignetteMidpoint = float(p.vignette.midpoint) / 100.0f;
    u.curvesActive = !p.curves.isIdentity();
    u.enabled = p.enabled;
    return u;
}

bool isIdentity(const Uniforms& u) {
    if (!u.enabled) return true;
    const auto zero3 = [](const std::array<float, 3>& v) { return v[0] == 0 && v[1] == 0 && v[2] == 0; };
    return u.wbGain[0] == 1 && u.wbGain[1] == 1 && u.wbGain[2] == 1 && u.exposureGain == 1 &&
           u.contrast == 0 && u.brightness == 0 && u.saturation == 1 && u.highlights == 0 &&
           u.shadows == 0 && u.blackPoint == 0 && u.whitePoint == 0 && zero3(u.cbMaster) &&
           zero3(u.cbShadows) && zero3(u.cbMidtones) && zero3(u.cbHighlights) && u.cbLum[0] == 0 &&
           u.cbLum[1] == 0 && u.cbLum[2] == 0 && u.cbLum[3] == 0 && u.vignetteAmount == 0 && !u.curvesActive;
}

// --- the CPU path ----------------------------------------------------------------

Grade::Grade(const Params& params, float midGrey)
    : Grade(postdev::uniforms(params, midGrey), curveTables(params.curves)) {}

Grade::Grade(const Params& params, float midGrey, const QColorSpace& space) : Grade(params, midGrey) {
    if (space.primaries() == QColorSpace::Primaries::DciP3D65)
        u_.toneSpace = ToneSpace::DisplayP3;
    else if (space.transferFunction() == QColorSpace::TransferFunction::Gamma)
        u_.toneSpace = ToneSpace::ProPhotoGamma18;
}

Grade::Grade(const Uniforms& uniforms, const CurveTables& tables)
    : u_(uniforms), tables_(tables), identity_(postdev::isIdentity(uniforms)) {}

void Grade::prepareToneGuide(const float* data, int w, int h, int channels, std::ptrdiff_t stride) {
    if (!needsToneGuide() || w <= 0 || h <= 0) return;
    if (!stride) stride = std::ptrdiff_t(w) * channels;
    guide_ = makeGuide(w, h, u_.toneSpace, [&](int x, int y) -> Rgb {
        const auto* p = data + y * stride + x * channels;
        return {p[0], p[1], p[2]};
    });
}
void Grade::prepareToneGuide(const QImage& image) {
    if (!needsToneGuide() || image.isNull()) return;
    // Navigator, canvas and histogram use the same ungraded source, and a
    // slider gesture changes only the weights, not its local exposure map.
    static std::mutex lock;
    static QCache<std::pair<qint64, int>, QImage> cache(4);
    const auto space = u_.toneSpace;
    const auto key = std::make_pair(image.cacheKey(), int(space));
    {
        std::lock_guard guard(lock);
        if (const auto* found = cache.object(key)) {
            guide_ = *found;
            return;
        }
    }
#ifdef Q_OS_WIN
    guide_ = gpuToneGuide(image, space);
#else
    auto sample = [&](int x, int y) -> Rgb {
        const auto* row = image.constScanLine(y);
        switch (image.format()) {
        case QImage::Format_RGBX16FPx4:
        case QImage::Format_RGBA16FPx4: {
            auto p = reinterpret_cast<const QRgbaFloat16*>(row)[x];
            return {float(p.red()), float(p.green()), float(p.blue())};
        }
        case QImage::Format_RGBX32FPx4:
        case QImage::Format_RGBA32FPx4: {
            auto p = reinterpret_cast<const float*>(row) + 4 * x;
            return {p[0], p[1], p[2]};
        }
        case QImage::Format_RGBX64:
        case QImage::Format_RGBA64: {
            auto p = reinterpret_cast<const quint16*>(row) + 4 * x;
            return {p[0] / 65535.f, p[1] / 65535.f, p[2] / 65535.f};
        }
        default: {
            const auto p = image.pixelColor(x, y);
            return {p.redF(), p.greenF(), p.blueF()};
        }
        }
    };
    guide_ = makeGuide(image.width(), image.height(), space, sample);
#endif
    std::lock_guard guard(lock);
    cache.insert(key, new QImage(guide_));
}

// layer2Tone, Shaders.metal:270-313, in its order and its float arithmetic.
std::array<float, 3> Grade::tone(std::array<float, 3> c, std::array<float, 4> ab) const {
    const auto& u = u_;
    const auto space = u.toneSpace;
    // Measure on the ungraded print; upstream moves must not move the mask.
    const float sourceY = needsToneGuide() ? luminance(linearRgb(c, space), space) : 0;
    const float base = needsToneGuide() ? ab[0] * toneStops(sourceY) + ab[1] : 0;
    // 1. white balance (scan-side gains on encoded values)
    for (int i = 0; i < 3; ++i) c[i] *= u.wbGain[i];
    // 2. exposure in a pseudo-linear domain, then contrast and brightness
    if (u.exposureGain != 1.0f)
        for (auto& v : c) v = positive(v) * u.exposureScale;
    if (u.contrast != 0.0f) {
        // Keep the slope continuous through neutral. At +50 the S-curve's
        // middle-grey slope is 3.3, calibrated to the old +35 L* variance gain.
        const float k = 1.0f + u.contrast * (1.2f + 6.8f * positive(u.contrast));
        c = bendPrint(linearRgb(c, space), space, k);
    }
    if (u.brightness != 0.0f) {
        const float e = 1.0f / (1.0f + u.brightness * 0.8f);
        for (auto& v : c) v = std::pow(positive(v), e);
    }
    if (needsToneGuide()) {
        auto lin = linearRgb(c, space);
        const float y = luminance(lin, space);
        // The local base selects the range. Bend distances from black or
        // white within that half of L*, leaving both endpoints anchored.
        const float amount = y < .18f ? -u.shadows * smoothstep(0, 2, -base)
                                      : u.highlights * smoothstep(0, 2, base);
        if (amount != 0 && y < 1) {
            const float k = pinnedSlope(lightness(sourceY), ab[2], ab[3], std::exp2(1.5f * amount));
            if (k != 1) c = bendPrint(lin, space, k);
        }
    }
    // 4. black / white point
    const float span = std::max(1.0f - u.blackPoint - u.whitePoint, 0.05f);
    for (auto& v : c) v = (v - u.blackPoint) / span;
    // 5. saturation
    const float l = luma(c);
    for (auto& v : c) v = l + (v - l) * u.saturation;
    // 6. colour balance: master + three zones split at mid-grey
    const float ls = saturate(tonePosition(l, u.midGrey));
    const float wS = (1.0f - ls) * (1.0f - ls);
    const float wH = ls * ls;
    const float wM = std::max(1.0f - wS - wH, 0.0f);
    for (int i = 0; i < 3; ++i)
        c[i] += u.cbMaster[i] + u.cbShadows[i] * wS + u.cbMidtones[i] * wM + u.cbHighlights[i] * wH;
    const float lum = 1.0f + u.cbLum[0] + u.cbLum[1] * wS + u.cbLum[2] * wM + u.cbLum[3] * wH;
    for (auto& v : c) v *= lum;
    return c;
}

// The rest of the kernel, Shaders.metal:350-403 (no masks, no DI decode).
std::array<float, 3> Grade::pixel(std::array<float, 3> rgb, float u, float v) const {
    if (identity_) return rgb;
    auto c = tone(rgb, guideAt(guide_, u, v));
    // 7. curves -- luma, then RGB master, then per channel
    if (u_.curvesActive) {
        const float* rows = tables_.data();
        for (auto& x : c) x = saturate(x);
        const float ly = luma(c);
        const float ly2 = curveLookup(rows + curveTableSize, ly);
        const float k = (ly > 1e-4f) ? (ly2 / ly) : 1.0f;
        for (auto& x : c) x = saturate(x * k);
        for (auto& x : c) x = curveLookup(rows, x);
        c = {curveLookup(rows + 2 * curveTableSize, c[0]), curveLookup(rows + 3 * curveTableSize, c[1]),
             curveLookup(rows + 4 * curveTableSize, c[2])};
    }
    // 9. vignette -- radial about the source's centre, per axis
    if (u_.vignetteAmount != 0.0f) {
        const float px = (u - 0.5f) * 2.0f, py = (v - 0.5f) * 2.0f;
        const float d = std::sqrt(px * px + py * py);
        const float fall = smoothstep(u_.vignetteMidpoint * 1.2f, 1.5f, d);
        const float k = 1.0f + u_.vignetteAmount * fall * 0.9f;
        for (auto& x : c) x *= k;
    }
    for (auto& x : c) x = saturate(x);
    return c;
}

void Grade::apply(float* data, int width, int height, int channels, std::ptrdiff_t rowStride, int threads,
                  SourceRect source) const {
    if (channels != 3 && channels != 4) throw std::invalid_argument("Post-Dev applies to 3 or 4 channels");
    if (width < 0 || height < 0) throw std::invalid_argument("Post-Dev image size is negative");
    if (identity_ || width == 0 || height == 0) return;
    if (rowStride == 0) rowStride = std::ptrdiff_t(width) * channels;
    if (needsToneGuide() && guide_.isNull()) {
        Grade prepared = *this;
        prepared.prepareToneGuide(data, width, height, channels, rowStride);
        prepared.apply(data, width, height, channels, rowStride, threads, source);
        return;
    }
    const float sx = float(source.x), sy = float(source.y), sw = float(source.width), sh = float(source.height);
    const float fw = float(width), fh = float(height);
    forRows(height, threads, [&](int y) {
        float* row = data + std::ptrdiff_t(y) * rowStride;
        // (gid + 0.5) / size, Shaders.metal:340, in float.
        const float v = sy + (float(y) + 0.5f) / fh * sh;
        for (int x = 0; x < width; ++x) {
            float* p = row + std::ptrdiff_t(x) * channels;
            const auto out = pixel({p[0], p[1], p[2]}, sx + (float(x) + 0.5f) / fw * sw, v);
            p[0] = out[0];
            p[1] = out[1];
            p[2] = out[2];
        }
    });
}

void Grade::apply(QImage& image, int threads, SourceRect source) const {
    const auto format = image.format();
    enum class Kind { u16, f32, u8rgba, u8argb } kind;
    switch (format) {
    case QImage::Format_RGBX64: case QImage::Format_RGBA64: kind = Kind::u16; break;
    case QImage::Format_RGBX32FPx4: case QImage::Format_RGBA32FPx4: kind = Kind::f32; break;
    case QImage::Format_RGBX8888: case QImage::Format_RGBA8888: kind = Kind::u8rgba; break;
    case QImage::Format_RGB32: case QImage::Format_ARGB32: kind = Kind::u8argb; break;
    default: throw std::invalid_argument("Post-Dev cannot grade this QImage format; convert it first");
    }
    if (identity_ || image.isNull()) return;
    if (needsToneGuide() && guide_.isNull()) {
        Grade prepared = *this;
        prepared.prepareToneGuide(image);
        prepared.apply(image, threads, source);
        return;
    }
    const int width = image.width(), height = image.height();
    const qsizetype stride = image.bytesPerLine();
    uchar* bits = image.bits();
    const float sx = float(source.x), sy = float(source.y), sw = float(source.width), sh = float(source.height);
    const float fw = float(width), fh = float(height);
    forRows(height, threads, [&](int y) {
        uchar* line = bits + qsizetype(y) * stride;
        const float v = sy + (float(y) + 0.5f) / fh * sh;
        for (int x = 0; x < width; ++x) {
            const float u = sx + (float(x) + 0.5f) / fw * sw;
            switch (kind) {
            case Kind::u16: {
                auto* p = reinterpret_cast<quint16*>(line) + 4 * x;
                const auto out = pixel({p[0] / 65535.0f, p[1] / 65535.0f, p[2] / 65535.0f}, u, v);
                for (int i = 0; i < 3; ++i) p[i] = quint16(out[i] * 65535.0f + 0.5f);
                break;
            }
            case Kind::f32: {
                auto* p = reinterpret_cast<float*>(line) + 4 * x;
                const auto out = pixel({p[0], p[1], p[2]}, u, v);
                for (int i = 0; i < 3; ++i) p[i] = out[i];
                break;
            }
            case Kind::u8rgba: {
                uchar* p = line + 4 * x;
                const auto out = pixel({p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f}, u, v);
                for (int i = 0; i < 3; ++i) p[i] = uchar(out[i] * 255.0f + 0.5f);
                break;
            }
            case Kind::u8argb: {
                auto* p = reinterpret_cast<QRgb*>(line) + x;
                const auto out = pixel({qRed(*p) / 255.0f, qGreen(*p) / 255.0f, qBlue(*p) / 255.0f}, u, v);
                *p = qRgba(int(out[0] * 255.0f + 0.5f), int(out[1] * 255.0f + 0.5f),
                           int(out[2] * 255.0f + 0.5f), qAlpha(*p));
                break;
            }
            }
        }
    });
}

// --- the GPU block -----------------------------------------------------------------

std::array<float, std140Bytes / sizeof(float)> std140(const Grade& grade) {
    const auto& u = grade.uniforms();
    const float on = grade.isIdentity() ? 0.0f : 1.0f;
    return {u.wbGain[0],
            u.wbGain[1],
            u.wbGain[2],
            u.exposureScale,
            u.contrast,
            u.brightness,
            u.saturation,
            u.highlights,
            u.shadows,
            u.blackPoint,
            u.whitePoint,
            u.midGrey,
            u.cbMaster[0],
            u.cbMaster[1],
            u.cbMaster[2],
            0.0f,
            u.cbShadows[0],
            u.cbShadows[1],
            u.cbShadows[2],
            0.0f,
            u.cbMidtones[0],
            u.cbMidtones[1],
            u.cbMidtones[2],
            0.0f,
            u.cbHighlights[0],
            u.cbHighlights[1],
            u.cbHighlights[2],
            0.0f,
            u.cbLum[0],
            u.cbLum[1],
            u.cbLum[2],
            u.cbLum[3],
            u.vignetteAmount,
            u.vignetteMidpoint,
            0.0f,
            0.0f,
            on,
            u.curvesActive ? 1.0f : 0.0f,
            u.exposureGain != 1.0f ? 1.0f : 0.0f,
            float(u.toneSpace)};
}

// --- JSON ------------------------------------------------------------------------

QJsonObject toJson(const Params& p) {
    const auto& c = p.curves;
    const auto& cb = p.colorBalance;
    return {
        {QStringLiteral("enabled"), p.enabled},
        {QStringLiteral("temperature"), p.temperature},
        {QStringLiteral("tint"), p.tint},
        {QStringLiteral("exposure"), p.exposure},
        {QStringLiteral("contrast"), p.contrast},
        {QStringLiteral("brightness"), p.brightness},
        {QStringLiteral("saturation"), p.saturation},
        {QStringLiteral("highlights"), p.highlights},
        {QStringLiteral("shadows"), p.shadows},
        {QStringLiteral("blackPoint"), p.blackPoint},
        {QStringLiteral("whitePoint"), p.whitePoint},
        {QStringLiteral("curves"), QJsonObject{{QStringLiteral("rgb"), curveJson(c.rgb)},
                                               {QStringLiteral("luma"), curveJson(c.luma)},
                                               {QStringLiteral("red"), curveJson(c.red)},
                                               {QStringLiteral("green"), curveJson(c.green)},
                                               {QStringLiteral("blue"), curveJson(c.blue)}}},
        {QStringLiteral("colorBalance"), QJsonObject{{QStringLiteral("master"), zoneJson(cb.master)},
                                                     {QStringLiteral("shadows"), zoneJson(cb.shadows)},
                                                     {QStringLiteral("midtones"), zoneJson(cb.midtones)},
                                                     {QStringLiteral("highlights"), zoneJson(cb.highlights)}}},
        {QStringLiteral("vignette"), QJsonObject{{QStringLiteral("amount"), p.vignette.amount},
                                                 {QStringLiteral("midpoint"), p.vignette.midpoint}}},
    };
}

Params fromJson(const QJsonObject& o, QStringList* unknown, const QString& label) {
    const Reader r{unknown};
    r.keys(o, {"enabled", "temperature", "tint", "exposure", "contrast", "brightness", "saturation",
               "highlights", "shadows", "blackPoint", "whitePoint", "curves", "colorBalance", "vignette"},
           label);
    Params p;
    p.enabled = r.boolean(o, "enabled", label, true);
    p.temperature = r.number(o, "temperature", label, "temperature");
    p.tint = r.number(o, "tint", label, "tint");
    p.exposure = r.number(o, "exposure", label, "exposure");
    p.contrast = r.number(o, "contrast", label, "contrast");
    p.brightness = r.number(o, "brightness", label, "brightness");
    p.saturation = r.number(o, "saturation", label, "saturation");
    p.highlights = r.number(o, "highlights", label, "highlights");
    p.shadows = r.number(o, "shadows", label, "shadows");
    p.blackPoint = r.number(o, "blackPoint", label, "blackPoint");
    p.whitePoint = r.number(o, "whitePoint", label, "whitePoint");
    if (const auto curves = r.object(o, "curves", label)) {
        const QString at = label + QStringLiteral(".curves");
        r.keys(*curves, {"rgb", "luma", "red", "green", "blue"}, at);
        const std::pair<const char*, CurveChannel> channels[] = {
            {"rgb", CurveChannel::rgb}, {"luma", CurveChannel::luma}, {"red", CurveChannel::red},
            {"green", CurveChannel::green}, {"blue", CurveChannel::blue}};
        for (const auto& [name, channel] : channels)
            if (const auto curve = r.object(*curves, name, at))
                p.curves[channel] = r.curve(*curve, at + "." + QLatin1String(name));
    }
    if (const auto balance = r.object(o, "colorBalance", label)) {
        const QString at = label + QStringLiteral(".colorBalance");
        r.keys(*balance, {"master", "shadows", "midtones", "highlights"}, at);
        const std::pair<const char*, ColorZone ColorBalance::*> zones[] = {
            {"master", &ColorBalance::master}, {"shadows", &ColorBalance::shadows},
            {"midtones", &ColorBalance::midtones}, {"highlights", &ColorBalance::highlights}};
        for (const auto& [name, member] : zones)
            if (const auto zone = r.object(*balance, name, at))
                p.colorBalance.*member = r.zone(*zone, at + "." + QLatin1String(name), name);
    }
    if (const auto vignette = r.object(o, "vignette", label)) {
        const QString at = label + QStringLiteral(".vignette");
        r.keys(*vignette, {"amount", "midpoint"}, at);
        p.vignette.amount = r.number(*vignette, "amount", at, "vignette.amount");
        p.vignette.midpoint = r.number(*vignette, "midpoint", at, "vignette.midpoint");
    }
    return p;
}

// --- the wheel -----------------------------------------------------------------

namespace wheel {
namespace {
constexpr double sweep = 130;
double bottomTheta(Side side) { return side == Side::left ? 245 : -65; }
double topTheta(Side side) { return side == Side::left ? 245 - sweep : -65 + sweep; }
} // namespace

ColorZone zoneAt(double x, double y, double diameter, ColorZone zone) {
    const double c = diameter / 2, radius = diameter / 2 - 5;
    const double dx = x - c, dy = c - y;
    const double distance = std::hypot(dx, dy);
    const double rr = radius > 0 ? std::min(distance / radius, 1.0) : (distance > 0 ? 1.0 : 0.0);
    double deg = std::atan2(dy, dx) * 180 / std::numbers::pi;
    if (deg < 0) deg += 360;
    zone.hue = deg;
    zone.saturation = rr;
    return zone;
}

std::pair<double, double> pointOf(const ColorZone& zone, double diameter) {
    const double r = zone.saturation * (diameter / 2 - 5);
    const double a = zone.hue * std::numbers::pi / 180;
    return {diameter / 2 + std::cos(a) * r, diameter / 2 - std::sin(a) * r};
}

double arcTheta(double fraction, Side side) {
    const double f = std::clamp(fraction, 0.0, 1.0);
    return bottomTheta(side) + (topTheta(side) - bottomTheta(side)) * f;
}

double arcAngle(double px, double py, double cx, double cy) {
    const double theta = std::atan2(cy - py, px - cx) * 180 / std::numbers::pi;
    return theta > 180 ? theta - 360 : theta;
}

double arcValue(double thetaDegrees, Side side, double low, double high) {
    const double bottom = bottomTheta(side), top = topTheta(side);
    const double mid = (bottom + top) / 2;
    double t = thetaDegrees;
    while (t - mid > 180) t -= 360;
    while (t - mid < -180) t += 360;
    const double f = std::clamp((t - bottom) / (top - bottom), 0.0, 1.0);
    return low + f * (high - low);
}

double arcFraction(double value, double low, double high) {
    return std::clamp((value - low) / std::max(high - low, 1e-9), 0.0, 1.0);
}
} // namespace wheel

} // namespace spk::desktop::postdev
