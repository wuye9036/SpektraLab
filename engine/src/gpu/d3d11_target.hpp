#pragma once
#ifdef _WIN32
#include "spektrafilm/spk_engine.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
struct ID3D11Device;
struct ID3D11Texture2D;
namespace spk::gpu {
class Gpu;
struct Buffer;
class BufferRef;
class SharedDisplay {
public:
    ~SharedDisplay();
    std::shared_ptr<ID3D11Texture2D> open(ID3D11Device* device);
    struct Storage;
    explicit SharedDisplay(std::unique_ptr<Storage> storage);
private:
    std::unique_ptr<Storage> storage_;
};
using DisplayTexture = std::shared_ptr<SharedDisplay>;

// Pixel rectangle in the unturned print; flips act on the final output axes.
struct DisplayGeometry {
    uint32_t stride, left, top, width, height, turns, flips;
    double degrees;
    uint32_t outputWidth() const { return turns % 2 ? height : width; }
    uint32_t outputHeight() const { return turns % 2 ? width : height; }
};
// One publication, created by the caller. Analyze runs while the worker's
// D3D device owns the mutex, before the immutable frame passes to Qt.
struct D3D11Publication {
    ID3D11Device* producer = nullptr;
    // Serialize keyed access with other users of this device's immediate
    // context. Individual analysis passes can also take this lock directly.
    std::recursive_mutex* producerMutex = nullptr;
    std::function<void(ID3D11Texture2D*)> analyze;
    DisplayTexture texture;
    // Destruction order matters when a frame outlives its host. Release the
    // reader's Vulkan buffer before the last reference to its device.
    std::shared_ptr<spk_engine> engineOwner;
    std::function<bool(spk_result&, std::string&)> readPixels;
    // Opt in to reprojecting this immutable frame without a host pixel copy.
    bool allowGeometry = false;
    std::function<DisplayTexture(const DisplayGeometry&, const D3D11Publication&, std::string&)> project;
};
DisplayTexture make_d3d11_display(Gpu& gpu, Buffer* rgb, uint32_t width, uint32_t height,
                                   const D3D11Publication& publication, BufferRef* exact, std::string& error,
                                   const DisplayGeometry* geometry = nullptr);
spk_status render_d3d11(spk_session* session, const char* tier, bool reprint,
                        D3D11Publication& publication, spk_result& metadata,
                        bool retainExactPixels = false, bool allowHostFallback = false);
}
#endif
