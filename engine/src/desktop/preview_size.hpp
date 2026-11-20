#pragma once
#include <algorithm>

namespace spk::desktop {
inline constexpr int kDefaultPreviewLongEdge = 1280;

// A host can disable live development (0) and still need sized preview images.
constexpr int previewImageLongEdge(int edge) {
    return edge > 0 ? std::clamp(edge, 800, 8192) : kDefaultPreviewLongEdge;
}
}  // namespace spk::desktop
