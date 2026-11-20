#include "UiFonts.hpp"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QTextLayout>
#include <algorithm>

UiFonts::UiFonts() {
    const auto path = QCoreApplication::applicationDirPath() + "/resources/ui-fonts/NotoSansSC-VF.ttf";
    if (QFontDatabase::addApplicationFont(path) < 0)
        qWarning("Unable to load the bundled UI fallback font: %s", qPrintable(path));
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, [this] {
        faces_.clear();
        uncommon_.clear();
    });
}

QFont UiFonts::forText(QFont role, const QString& text) const {
    const QFont primary = role;
    role.setStyleStrategy(QFont::NoFontMerging);
    if (text.isEmpty()) return role;
    auto chars = text.toUcs4();
    std::sort(chars.begin(), chars.end());
    chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
    chars.removeIf([](char32_t c) {
        // Layout controls and variation selectors do not need standalone glyphs.
        const auto category = QChar::category(c);
        return category == QChar::Other_Control || category == QChar::Other_Format ||
               category == QChar::Separator_Line || category == QChar::Separator_Paragraph ||
               (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xe0100 && c <= 0xe01ef);
    });
    QFont best = role;
    qsizetype bestCoverage = -1;
    QStringList tried;
    auto covers = [&](const QString& family) {
        if (tried.contains(family)) return false;
        tried.append(family);
        role.setFamilies({family});
        auto* face = faces_.object(role);
        if (!face) {
            face = new QRawFont(QRawFont::fromFont(role));
            faces_.insert(role, face);
        }
        if (!face->isValid()) return false;
        const auto count = std::count_if(chars.cbegin(), chars.cend(), [face](char32_t c) {
            return face->supportsCharacter(c);
        });
        if (count > bestCoverage) {
            bestCoverage = count;
            best = role;
        }
        return count == chars.size();
    };
    if (covers(primary.family()) || covers(QStringLiteral("Noto Sans SC"))) return best;
    const auto key = qMakePair(primary, QString::fromUcs4(chars.constData(), chars.size()));
    if (const auto* known = uncommon_.object(key)) return *known;

    // Ask Qt which physical faces it would mix for this string. Try each as
    // the face for the entire label before looking through other installed faces.
    QTextLayout layout(text, primary);
    layout.beginLayout();
    while (layout.createLine().isValid()) {}
    layout.endLayout();
    for (const auto& run : layout.glyphRuns())
        if (covers(run.rawFont().familyName())) break;
    if (bestCoverage < chars.size()) {
        for (const auto& family : QFontDatabase::families())
            if (!QFontDatabase::isPrivateFamily(family) && covers(family)) break;
    }
    uncommon_.insert(key, new QFont(best));
    return best;
}
