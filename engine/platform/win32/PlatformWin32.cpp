#include <windows.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "platform/Platform.h"

namespace oe {

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string KeyName(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Control";
        case VK_ESCAPE: return "Escape";
        case VK_RETURN: return "Enter";
        case VK_TAB: return "Tab";
        default: return std::string();
    }
}

class Win32Window final : public Window {
public:
    bool Init(const std::string& title, int width, int height) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32Window::Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OwnEngineWindow";
        RegisterClassExW(&wc);  // fails harmlessly if already registered

        RECT rect{0, 0, width, height};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, Widen(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        width_ = width;
        height_ = height;
        return true;
    }

    ~Win32Window() override {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool PumpEvents(InputState& input) override {
        input_ = &input;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        input_ = nullptr;
        return !closed_;
    }

    void Present(const RenderTarget& frame) override {
        if (!hwnd_ || frame.width <= 0) return;
        bgra_.resize(frame.color.size());
        for (size_t i = 0; i < frame.color.size(); ++i) {
            uint32_t c = frame.color[i];  // 0xAABBGGRR -> 0xAARRGGBB
            bgra_[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = frame.width;
        bmi.bmiHeader.biHeight = -frame.height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(hwnd_);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, width_, height_, 0, 0, frame.width, frame.height, bgra_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd_, dc);
    }

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    void SetTitle(const std::string& title) override { SetWindowTextW(hwnd_, Widen(title).c_str()); }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Win32Window* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        if (self) {
            switch (msg) {
                case WM_CLOSE:
                    self->closed_ = true;
                    return 0;
                case WM_SIZE:
                    self->width_ = LOWORD(lp);
                    self->height_ = HIWORD(lp);
                    return 0;
                case WM_KILLFOCUS:
                    if (self->input_) self->input_->down.clear();
                    return 0;
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYUP: {
                    std::string key = KeyName(wp);
                    if (!key.empty() && self->input_) {
                        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                        if (down) {
                            if (!self->input_->IsDown(key)) self->input_->pressedThisFrame.insert(key);
                            self->input_->down.insert(key);
                        } else {
                            self->input_->down.erase(key);
                        }
                    }
                    if (wp == VK_ESCAPE && msg == WM_KEYDOWN) self->closed_ = true;
                    return 0;
                }
                default: break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    bool closed_ = false;
    InputState* input_ = nullptr;
    std::vector<uint32_t> bgra_;
};

}  // namespace

std::unique_ptr<Window> CreatePlatformWindow(const std::string& title, int width, int height) {
    auto w = std::make_unique<Win32Window>();
    if (!w->Init(title, width, height)) return nullptr;
    return w;
}

const char* PlatformName() { return "win32"; }

double PlatformTimeSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void PlatformSleep(double seconds) {
    if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool PlatformOpenUrl(const std::string& url) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::string ExecutableDirectory() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::string path = Narrow(std::wstring(buf, n));
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path.substr(0, path.rfind('/'));
}

void PlatformSetBinaryStdio() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

}  // namespace oe
