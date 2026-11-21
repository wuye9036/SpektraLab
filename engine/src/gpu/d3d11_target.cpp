#ifdef _WIN32
#include "d3d11_target.hpp"
#include "gpu.hpp"
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <mutex>

namespace spk::gpu {
using Microsoft::WRL::ComPtr;
struct SharedDisplay::Storage {
    ComPtr<ID3D11Texture2D> source;
    HANDLE handle{};
    std::mutex mutex;
    std::weak_ptr<ID3D11Texture2D> active;
    ~Storage() { if (handle) CloseHandle(handle); }
};
SharedDisplay::SharedDisplay(std::unique_ptr<Storage> storage) : storage_(std::move(storage)) {}
SharedDisplay::~SharedDisplay() = default;

std::shared_ptr<ID3D11Texture2D> SharedDisplay::open(ID3D11Device* device) {
    std::lock_guard lock(storage_->mutex);
    if (auto active = storage_->active.lock()) {
        ComPtr<ID3D11Device> owner;
        active->GetDevice(&owner);
        return owner.Get()==device ? active : nullptr;
    }
    ComPtr<ID3D11Device1> consumer;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&consumer))) ||
        FAILED(consumer->OpenSharedResource1(storage_->handle,IID_PPV_ARGS(&texture))) ||
        FAILED(texture.As(&mutex)) || mutex->AcquireSync(1, 1000) != S_OK) return {};
    std::shared_ptr<ID3D11Texture2D> acquired(texture.Detach(), [mutex](ID3D11Texture2D* tex) {
        mutex->ReleaseSync(1); // immutable: another Qt device can acquire after retirement
        tex->Release();
    });
    storage_->active=acquired;
    return acquired;
}

DisplayTexture make_d3d11_display(Gpu& gpu, Buffer* rgb, uint32_t width, uint32_t height,
                                   const D3D11Publication& publication, BufferRef* exact, std::string& error,
                                   const DisplayGeometry* geometry) {
    auto* device = publication.producer;
    if (!device || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        error = "unsupported D3D11 display size or device"; return nullptr;
    }
    auto check = [&](HRESULT hr, const char* what) {
        if (hr == S_OK) return true; // WAIT_TIMEOUT is positive
        char number[24];
        std::snprintf(number, sizeof number, " (0x%08lx)", static_cast<unsigned long>(hr));
        error = std::string(what) + number;
        return false;
    };
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (!check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "query DXGI device") ||
        !check(dxgi->GetAdapter(&adapter), "query display adapter") ||
        !check(adapter->GetDesc(&desc), "query adapter LUID")) return nullptr;
    ComPtr<IDXGIAdapter3> budgetAdapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
    if (SUCCEEDED(adapter.As(&budgetAdapter)) &&
        SUCCEEDED(budgetAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &budget)) &&
        budget.Budget && (budget.CurrentUsage >= budget.Budget ||
        uint64_t(width)*height*(exact ? 16 : 8) > budget.Budget - budget.CurrentUsage)) {
        error = "shared display frame exceeds the current local memory budget"; return nullptr;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIResource1> resource;
    ComPtr<IDXGIKeyedMutex> mutex;
    auto storage=std::make_unique<SharedDisplay::Storage>();
    if (!check(device->CreateTexture2D(&td, nullptr, &tex), "create shared FP16 display texture") ||
        !check(tex.As(&resource), "query shared resource") ||
        !check(tex.As(&mutex), "query shared mutex") ||
        !check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                            nullptr, &storage->handle), "create shared NT handle") ||
        !gpu.write_d3d11_display(rgb, width, height, storage->handle, &desc.AdapterLuid, exact, geometry, error)) return nullptr;
    if (publication.analyze) {
        // A concurrent staging Map and AcquireSync on this same device can
        // wedge the driver before the keyed-mutex timeout even takes effect.
        std::unique_lock<std::recursive_mutex> contextLock;
        if (publication.producerMutex) contextLock=std::unique_lock(*publication.producerMutex);
        if (!check(mutex->AcquireSync(1,2000),"acquire analysis texture")) return {};
        try { publication.analyze(tex.Get()); }
        catch (const std::exception& e) { mutex->ReleaseSync(1); error=e.what(); return {}; }
        if (!check(mutex->ReleaseSync(1),"release analysis texture")) return {};
    }
    storage->source=std::move(tex);
    return std::make_shared<SharedDisplay>(std::move(storage));
}
}

#endif
