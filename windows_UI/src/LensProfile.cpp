#include "LensProfile.hpp"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace {
using Fields = QHash<QString, QString>;
struct Entry { Fields camera, geometry; };

// Read both XMP property spellings, including the transparent rdf:Description
// wrappers. Nested chromatic and vignette models have their own coefficients.
void properties(QXmlStreamReader& xml, Fields& fields, Fields* geometry, int depth = 0) {
    if (depth > 16) { xml.raiseError(QStringLiteral("Profile too deeply nested")); return; }
    for (const auto& attr : xml.attributes()) fields.insert(attr.name().toString(), attr.value().toString());
    while (xml.readNextStartElement()) {
        const auto name = xml.name().toString();
        if (name == "Description") properties(xml, fields, geometry, depth+1);
        else if (name == "PerspectiveModel" && geometry) properties(xml, *geometry, nullptr, depth+1);
        else if (name.endsWith("Model") || name.startsWith("Alternate")) xml.skipCurrentElement();
        else fields.insert(name, xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed());
    }
}

double value(const Fields& f, const QString& key, double fallback) {
    bool ok = false;
    const double v = f.value(key).toDouble(&ok);
    return ok && std::isfinite(v) ? v : fallback;
}
struct Calibration {
    std::array<double, 3> radial;
    std::array<double, 2> center;
    std::tuple<bool, double, double> rank;
};
} // namespace

QString readLensProfile(const QString& path, const spk::io::RawMetadata& photo) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32*1024*1024) return {};
    QXmlStreamReader xml(&file);
    std::map<double, Calibration> samples;
    const double width = photo.sizes_before_postprocess.width, height = photo.sizes_before_postprocess.height;
    if (!(width > 0 && height > 0)) return {};
    bool inProfiles = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isDTD()) return {};
        if (xml.name() == u"CameraProfiles") {
            inProfiles = xml.isStartElement();
            continue;
        }
        if (!inProfiles || !xml.isStartElement() || xml.name() != u"li") continue;
        Entry e;
        properties(xml, e.camera, &e.geometry);
        const auto& g = e.geometry;
        if (e.camera.value("CameraRawProfile").compare("false", Qt::CaseInsensitive) == 0 ||
            !(g.contains("RadialDistortParam1") || g.contains("RadialDistortParam2") || g.contains("RadialDistortParam3"))) continue;
        const double focal = value(e.camera, "FocalLength", 0);
        double crop = value(e.camera, "SensorFormatFactor", 1);
        const double pw = value(e.camera, "ImageWidth", width), ph = value(e.camera, "ImageLength", height);
        if (!(crop > 0 && pw > 0 && ph > 0)) continue;
        // LCPReader.swift: FocalLengthX is in units of the reference camera's
        // long edge. Convert to units of this shot's farthest-corner radius.
        const double referenceEdge = 43.2666 * std::max(pw, ph) / (crop * std::hypot(pw, ph));
        const double fmm = value(g, "FocalLengthX", 0) * referenceEdge;
        if (photo.focal_mm > 0 && photo.shooting.focal_35mm > 0) crop = photo.shooting.focal_35mm/photo.focal_mm;
        const double cx = value(g, "ImageXCenter", .5), cy = value(g, "ImageYCenter", .5);
        const double radius = 43.2666 / crop * std::hypot(std::max(cx,1-cx)*width, std::max(cy,1-cy)*height) /
                              std::hypot(width, height);
        if (cx < 0 || cx > 1 || cy < 0 || cy > 1 || !(fmm > 0 || focal > 0)) continue;
        const double unit = radius / (fmm > 0 ? fmm : focal), q = unit*unit;
        const double av = photo.shooting.f_number > 0 ? 2*std::log2(photo.shooting.f_number) : 0;
        const double distance = value(e.camera, "FocusDistance", 10000);
        Calibration sample{{}, {cx,cy}, {
            e.camera.value("Model").compare(QString::fromStdString(photo.model), Qt::CaseInsensitive) != 0,
            std::abs(value(e.camera,"ApertureValue",av)-av), 1/std::max(.001,distance)}};
        double power = q;
        for (int i = 0; i < 3; ++i, power *= q)
            sample.radial[i] = value(g, QStringLiteral("RadialDistortParam%1").arg(i+1), 0) * power;
        if (std::all_of(sample.radial.begin(), sample.radial.end(), [](double k){ return k == 0; })) continue;
        auto it = samples.find(focal);
        if (it == samples.end() || sample.rank < it->second.rank) samples[focal] = sample;
    }
    if (xml.hasError() || samples.empty()) return {};
    auto hi = samples.lower_bound(photo.focal_mm);
    if (hi == samples.end()) hi = std::prev(samples.end());
    const auto lo = hi == samples.begin() ? hi : std::prev(hi);
    const double t = hi == lo ? 0 : std::clamp((photo.focal_mm-lo->first)/(hi->first-lo->first), 0., 1.);
    QJsonArray radial, center;
    for (int i = 0; i < 3; ++i) radial.append(std::lerp(lo->second.radial[i], hi->second.radial[i], t));
    for (int i = 0; i < 2; ++i) center.append(std::lerp(lo->second.center[i], hi->second.center[i], t));
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"name", QFileInfo(file).fileName()},
        {"center", center}, {"radial", radial}}).toJson(QJsonDocument::Compact));
}
