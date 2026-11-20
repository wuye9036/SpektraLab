#pragma once

#include <QCache>
#include <QFont>
#include <QObject>
#include <QRawFont>

class UiFonts final : public QObject {
    Q_OBJECT
public:
    UiFonts();
    Q_INVOKABLE QFont forText(QFont role, const QString& text) const;

private:
    mutable QCache<QFont, QRawFont> faces_{128};
    mutable QCache<QPair<QFont, QString>, QFont> uncommon_{128};
};
