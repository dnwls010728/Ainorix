#include <windows.h>
#include <mmsystem.h>
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
        // Raw mouse input gives unaccelerated relative motion for mouse look.
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // generic desktop
        rid.usUsage = 0x02;      // mouse
        rid.hwndTarget = hwnd_;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        return true;
    }

    ~Win32Window() override {
        SetMouseLock(false);
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
        // Mouse lock requested by the game: hidden cursor kept inside the
        // client area (only while this window is in front).
        if (!input.mouseLocked && mouseLocked_) relockOnClick_ = false;  // the game released it
        bool want = input.mouseLocked && GetForegroundWindow() == hwnd_;
        if (want != mouseLocked_) SetMouseLock(want);
        if (mouseLocked_) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
            ClipCursor(&rc);
            input.mouseX = input.mouseY = 0.5f;  // UI clicks hit the crosshair
        }
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
    void* NativeHandle() const override { return hwnd_; }

private:
    void SetMouseLock(bool on) {
        if (on == mouseLocked_) return;
        mouseLocked_ = on;
        ShowCursor(on ? FALSE : TRUE);
        if (!on) ClipCursor(nullptr);
    }

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
                case WM_INPUT: {
                    if (self->mouseLocked_ && self->input_) {
                        RAWINPUT raw{};
                        UINT size = sizeof(raw);
                        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                            self->input_->mouseDX += static_cast<float>(raw.data.mouse.lLastX);
                            self->input_->mouseDY += static_cast<float>(raw.data.mouse.lLastY);
                        }
                    }
                    break;  // DefWindowProc cleans up the raw input buffer
                }
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP: {
                    if (self->input_ && self->width_ > 0 && self->height_ > 0) {
                        InputState& in = *self->input_;
                        if (!self->mouseLocked_) {
                            in.mouseX = static_cast<float>(static_cast<short>(LOWORD(lp))) / static_cast<float>(self->width_);
                            in.mouseY = static_cast<float>(static_cast<short>(HIWORD(lp))) / static_cast<float>(self->height_);
                        }
                        in.viewWidth = self->width_;
                        in.viewHeight = self->height_;
                        const char* button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? "MouseLeft"
                                             : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? "MouseRight" : nullptr;
                        // After Escape / focus loss, the next click captures the mouse
                        // again and is not passed to the game.
                        if (msg == WM_LBUTTONDOWN && self->relockOnClick_ && !self->mouseLocked_) {
                            self->relockOnClick_ = false;
                            in.mouseLocked = true;
                            return 0;
                        }
                        if (button) {
                            if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
                                if (!in.IsDown(button)) in.pressedThisFrame.insert(button);
                                in.down.insert(button);
                                SetCapture(hwnd);
                            } else {
                                in.down.erase(button);
                                ReleaseCapture();
                            }
                        }
                    }
                    return 0;
                }
                case WM_KILLFOCUS:
                    if (self->input_) {
                        self->input_->down.clear();
                        if (self->mouseLocked_) self->relockOnClick_ = true;
                        self->input_->mouseLocked = false;
                    }
                    self->SetMouseLock(false);
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
                    // Escape releases a locked mouse first; otherwise it closes the window.
                    if (wp == VK_ESCAPE && msg == WM_KEYDOWN) {
                        if (self->mouseLocked_ && self->input_) {
                            self->input_->mouseLocked = false;
                            self->relockOnClick_ = true;
                            self->SetMouseLock(false);
                        } else {
                            self->closed_ = true;
                        }
                    }
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
    bool mouseLocked_ = false;
    bool relockOnClick_ = false;  // released by Escape / focus loss, not by the game
    InputState* input_ = nullptr;
    std::vector<uint32_t> bgra_;
};

// waveOut output. The engine submits one buffer per simulated frame; a few
// buffers of silence are queued first so timing jitter does not starve it.
class WaveOutDevice final : public AudioDevice {
public:
    bool Init(int sampleRate) {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
        return waveOutOpen(&out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }
    ~WaveOutDevice() override {
        if (!out_) return;
        waveOutReset(out_);
        for (auto& b : buffers_) {
            if (b->hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out_, &b->hdr, sizeof(WAVEHDR));
        }
        waveOutClose(out_);
    }
    const char* Name() const override { return "waveOut (48 kHz stereo)"; }
    void Submit(const float* samples, int frames) override {
        int pending = 0;
        for (auto& b : buffers_) pending += (b->hdr.dwFlags & WHDR_PREPARED) && !(b->hdr.dwFlags & WHDR_DONE);
        if (pending == 0) {  // (re)start: prime with ~50 ms of silence
            std::vector<float> silence(static_cast<size_t>(frames) * 2, 0.0f);
            for (int i = 0; i < 3; ++i) Queue(silence.data(), frames);
        } else if (pending > 12) {
            return;  // running ahead of the device: drop rather than add latency
        }
        Queue(samples, frames);
    }

private:
    struct Buffer {
        WAVEHDR hdr{};
        std::vector<int16_t> data;
    };
    void Queue(const float* samples, int frames) {
        Buffer* free = nullptr;
        for (auto& b : buffers_) {
            if (!(b->hdr.dwFlags & WHDR_PREPARED) || (b->hdr.dwFlags & WHDR_DONE)) {
                free = b.get();
                break;
            }
        }
        if (!free) {
            buffers_.push_back(std::make_unique<Buffer>());
            free = buffers_.back().get();
        }
        if (free->hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out_, &free->hdr, sizeof(WAVEHDR));
        free->data.resize(static_cast<size_t>(frames) * 2);
        for (size_t i = 0; i < free->data.size(); ++i) {
            float s = samples[i] < -1.0f ? -1.0f : (samples[i] > 1.0f ? 1.0f : samples[i]);
            free->data[i] = static_cast<int16_t>(s * 32767.0f);
        }
        free->hdr = WAVEHDR{};
        free->hdr.lpData = reinterpret_cast<LPSTR>(free->data.data());
        free->hdr.dwBufferLength = static_cast<DWORD>(free->data.size() * sizeof(int16_t));
        waveOutPrepareHeader(out_, &free->hdr, sizeof(WAVEHDR));
        waveOutWrite(out_, &free->hdr, sizeof(WAVEHDR));
    }
    HWAVEOUT out_ = nullptr;
    std::vector<std::unique_ptr<Buffer>> buffers_;
};

}  // namespace

std::unique_ptr<AudioDevice> CreateAudioDevice(int sampleRate) {
    auto d = std::make_unique<WaveOutDevice>();
    if (!d->Init(sampleRate)) return nullptr;
    return d;
}

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

void PlatformShowError(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
    MessageBoxW(nullptr, Widen(message).c_str(), Widen(title).c_str(), MB_OK | MB_ICONERROR);
}

void PlatformSetBinaryStdio() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

}  // namespace oe
