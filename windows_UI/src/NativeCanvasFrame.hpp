#pragma once
#include <QImage>
#include <QMetaType>
#include <functional>
#include <memory>
namespace spk::gpu { class SharedDisplay; }

// The companion image carries the color tag and the navigator pixels. At 1:1
// the canvas samples this full-resolution texture. A device change can ask
// for the exact host frame on demand.
struct NativeCanvasFrame {
    std::shared_ptr<spk::gpu::SharedDisplay> texture;
    QSize size, logicalSize;
    std::function<QImage()> hostImage;
};
Q_DECLARE_METATYPE(NativeCanvasFrame)
