// Direct3D 11 device for GpuRenderer, plus the sokol_gfx implementation for
// the D3D11 backend. Uses the hardware adapter and falls back to WARP (the
// software rasterizer built into Windows) so it works on any Windows 10+ PC,
// including VMs and CI machines without a GPU.
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "platform/Platform.h"
#include "render/GpuDevice.h"

// The implementation after the other includes: sokol_gfx.h has no include
// guard around it.
#define SOKOL_IMPL
#define SOKOL_D3D11
#include "sokol_gfx.h"

namespace oe {

namespace {

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

class D3D11Device final : public GpuDevice {
public:
    bool Init(HWND hwnd, std::string* error) {
        hwnd_ = hwnd;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = E_FAIL;
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &device_, nullptr, &context_);
            if (hr == E_INVALIDARG) {  // Windows 7 / old runtimes do not know 11_1
                hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &device_, nullptr, &context_);
            }
            if (SUCCEEDED(hr)) {
                warp_ = type == D3D_DRIVER_TYPE_WARP;
                break;
            }
        }
        if (FAILED(hr)) return Fail(error, "D3D11CreateDevice failed (no hardware or WARP device)", hr);
        if (hwnd_ && !CreateSwapchain(error)) return false;
        return true;
    }

    ~D3D11Device() override {
        Release(staging_);
        Release(rtv_);
        Release(swapchain_);
        if (context_) context_->ClearState();
        Release(context_);
        Release(device_);
    }

    const char* Name() const override { return warp_ ? "d3d11-warp" : "d3d11"; }

    sg_environment Environment() const override {
        sg_environment env{};
        env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
        env.defaults.depth_format = SG_PIXELFORMAT_NONE;
        env.defaults.sample_count = 1;
        env.d3d11.device = device_;
        env.d3d11.device_context = context_;
        return env;
    }

    sg_swapchain Swapchain() override {
        sg_swapchain sc{};
        if (!swapchain_) {
            sc.invalid = true;
            return sc;
        }
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0 || IsIconic(hwnd_)) {
            sc.invalid = true;
            return sc;
        }
        if (w != width_ || h != height_) {
            Release(rtv_);
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            if (FAILED(swapchain_->ResizeBuffers(2, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_R8G8B8A8_UNORM, 0)) || !CreateRenderView()) {
                sc.invalid = true;
                return sc;
            }
            width_ = w;
            height_ = h;
        }
        sc.width = w;
        sc.height = h;
        sc.sample_count = 1;
        sc.color_format = SG_PIXELFORMAT_RGBA8;
        sc.depth_format = SG_PIXELFORMAT_NONE;
        sc.d3d11.render_view = rtv_;
        return sc;
    }

    void Present() override {
        if (!swapchain_) return;
        // Vsync; tearing-free and keeps the game loop from spinning.
        swapchain_->Present(1, 0);
    }

    bool ReadPixels(sg_image image, int width, int height, uint32_t* out) override {
        sg_d3d11_image_info info = sg_d3d11_query_image_info(image);
        auto* tex = static_cast<ID3D11Texture2D*>(const_cast<void*>(info.tex2d));
        if (!tex || width <= 0 || height <= 0) return false;
        if (!staging_ || stagingW_ != width || stagingH_ != height) {
            Release(staging_);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(width);
            d.Height = static_cast<UINT>(height);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging_))) return false;
            stagingW_ = width;
            stagingH_ = height;
        }
        context_->CopyResource(staging_, tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
        for (int y = 0; y < height; ++y) {
            std::memcpy(out + static_cast<size_t>(y) * static_cast<size_t>(width), static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        }
        context_->Unmap(staging_, 0);
        return true;
    }

private:
    bool Fail(std::string* error, const char* message, HRESULT hr) {
        if (error) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " (0x%08lx)", static_cast<unsigned long>(hr));
            *error = std::string("D3D11: ") + message + buf;
        }
        return false;
    }

    bool CreateSwapchain(std::string* error) {
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
            d.Height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.Scaling = DXGI_SCALING_STRETCH;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &d, nullptr, nullptr, &swapchain_);
            if (FAILED(hr)) {
                // Windows 8.1 and older have no FLIP_DISCARD.
                d.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
                d.BufferCount = 1;
                hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &d, nullptr, nullptr, &swapchain_);
            }
            if (SUCCEEDED(hr)) {
                factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
                width_ = static_cast<int>(d.Width);
                height_ = static_cast<int>(d.Height);
            }
        }
        Release(factory);
        Release(adapter);
        Release(dxgiDevice);
        if (FAILED(hr)) return Fail(error, "cannot create the window swap chain", hr);
        if (!CreateRenderView()) return Fail(error, "cannot create the back buffer view", E_FAIL);
        return true;
    }

    bool CreateRenderView() {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
        HRESULT hr = device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
        return SUCCEEDED(hr);
    }

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
    int stagingW_ = 0, stagingH_ = 0;
    int width_ = 0, height_ = 0;
    bool warp_ = false;
};

}  // namespace

std::unique_ptr<GpuDevice> CreateGpuDevice(Window* window, std::string* error) {
    HWND hwnd = window ? static_cast<HWND>(window->NativeHandle()) : nullptr;
    if (window && !hwnd) {
        if (error) *error = "the window has no native handle";
        return nullptr;
    }
    auto device = std::make_unique<D3D11Device>();
    if (!device->Init(hwnd, error)) return nullptr;
    return device;
}

}  // namespace oe
