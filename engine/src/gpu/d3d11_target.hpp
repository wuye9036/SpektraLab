#pragma once
#ifdef _WIN32
#include "spektrafilm/spk_engine.h"
#include <functional>
#include <memory>
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

// One publication, created by the caller. Analyze runs while the worker's
// D3D device owns the mutex, before the immutable frame passes to Qt.
struct D3D11Publication {
    ID3D11Device* producer = nullptr;
    std::function<void(ID3D11Texture2D*)> analyze;
    DisplayTexture texture;
    // Destruction order matters when a frame outlives its host. Release the
    // reader's Vulkan buffer before the last reference to its device.
    std::shared_ptr<spk_engine> engineOwner;
    std::function<bool(spk_result&, std::string&)> readPixels;
};
DisplayTexture make_d3d11_display(Gpu& gpu, Buffer* rgb, uint32_t width, uint32_t height,
                                   const D3D11Publication& publication, BufferRef* exact, std::string& error);
spk_status render_d3d11(spk_session* session, const char* tier, bool reprint,
                        D3D11Publication& publication, spk_result& metadata,
                        bool retainExactPixels = false, bool allowHostFallback = false);
}
#endif
