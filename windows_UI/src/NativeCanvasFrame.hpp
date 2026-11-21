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
    std::function<std::shared_ptr<spk::gpu::SharedDisplay>()> restoreTexture;
};
// QML may retain a previous binding value until its next garbage collection.
// The controller and the canvas own the publication only while it is current.
using NativeCanvasFrameRef = std::weak_ptr<const NativeCanvasFrame>;
Q_DECLARE_METATYPE(NativeCanvasFrameRef)
