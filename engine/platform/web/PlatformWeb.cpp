// Web platform (Emscripten / WebAssembly): the page's <canvas id="canvas">
// is the window, DOM events feed InputState, WebAudio plays the mixer output.
// Built by build_web.sh / build_web.bat; `oe package --web` ships the result.
// The browser owns the main loop (see tools/player/main.cpp), so nothing
// here may block.
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/Platform.h"

namespace oe {

namespace {

const char* kCanvas = "#canvas";

// ----- JavaScript glue ---------------------------------------------------------------

EM_JS(void, oe_web_set_title, (const char* title), { document.title = UTF8ToString(title); });

EM_JS(void, oe_web_show_error, (const char* title, const char* message), {
    var t = UTF8ToString(title), m = UTF8ToString(message);
    console.error(t + ": " + m);
    var box = document.getElementById("oe-error");
    if (!box) {
        box = document.createElement("pre");
        box.id = "oe-error";
        box.style.cssText = "position:fixed;left:16px;right:16px;top:16px;padding:16px;margin:0;white-space:pre-wrap;" +
                            "background:#2a1215;color:#ffd7d7;border:1px solid #a33;border-radius:8px;font:14px/1.4 monospace;z-index:10";
        document.body.appendChild(box);
    }
    box.textContent = t + "\n\n" + m;
});

EM_JS(void, oe_web_open_url, (const char* url), { window.open(UTF8ToString(url), "_blank"); });

// Software-renderer fallback (no WebGL2): RGBA pixels -> 2D canvas, stretched to the canvas size.
EM_JS(void, oe_web_present_rgba, (const uint8_t* pixels, int width, int height), {
    var canvas = Module["canvas"];
    if (!Module.oePresent) {
        var ctx = canvas.getContext("2d");
        if (!ctx) return;
        var back = document.createElement("canvas");
        Module.oePresent = {ctx : ctx, back : back, backCtx : back.getContext("2d")};
    }
    var p = Module.oePresent;
    if (p.back.width != width || p.back.height != height) {
        p.back.width = width;
        p.back.height = height;
        p.image = p.backCtx.createImageData(width, height);
    }
    p.image.data.set(HEAPU8.subarray(pixels, pixels + width * height * 4));
    p.backCtx.putImageData(p.image, 0, 0);
    p.ctx.imageSmoothingEnabled = false;
    p.ctx.drawImage(p.back, 0, 0, canvas.width, canvas.height);
});

// WebAudio output: a ScriptProcessor pulls from a queue of interleaved
// stereo chunks. Browsers only start audio after a user gesture, so the
// context resumes on the first key press, click or touch.
EM_JS(int, oe_web_audio_init, (int sampleRate), {
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return 0;
    var ctx;
    try { ctx = new AC({sampleRate : sampleRate}); } catch (e) { ctx = new AC(); }
    var a = {ctx : ctx, queue : [], offset : 0, queued : 0, rate : ctx.sampleRate / sampleRate};
    var node = ctx.createScriptProcessor(2048, 0, 2);
    node.onaudioprocess = function(e) {
        var l = e.outputBuffer.getChannelData(0), r = e.outputBuffer.getChannelData(1);
        for (var i = 0; i < l.length; i++) {
            var chunk = a.queue[0];
            if (!chunk) { l[i] = 0; r[i] = 0; continue; }
            l[i] = chunk[a.offset];
            r[i] = chunk[a.offset + 1];
            a.offset += 2;
            if (a.offset >= chunk.length) { a.queue.shift(); a.queued -= chunk.length / 2; a.offset = 0; }
        }
    };
    node.connect(ctx.destination);
    a.node = node;
    var resume = function() { if (ctx.state != "running") ctx.resume(); };
    ["keydown", "mousedown", "touchstart", "pointerdown"].forEach(function(ev) { window.addEventListener(ev, resume, true); });
    Module.oeAudio = a;
    return 1;
});

EM_JS(void, oe_web_audio_submit, (const float* samples, int frames), {
    var a = Module.oeAudio;
    if (!a || a.ctx.state != "running") return;  // not started yet: drop instead of building latency
    if (a.queued > 48000 / 5) return;             // > 200 ms buffered: the page was throttled, drop
    a.queue.push(HEAPF32.slice(samples >> 2, (samples >> 2) + frames * 2));
    a.queued += frames;
});

// ----- Input -------------------------------------------------------------------------

std::string KeyName(const EmscriptenKeyboardEvent* e) {
    std::string code = e->code;
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0) return code.substr(3);      // KeyA -> A
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0) return code.substr(5);    // Digit1 -> 1
    if (code == "Space") return "Space";
    if (code == "ArrowLeft") return "Left";
    if (code == "ArrowRight") return "Right";
    if (code == "ArrowUp") return "Up";
    if (code == "ArrowDown") return "Down";
    if (code == "ShiftLeft" || code == "ShiftRight") return "Shift";
    if (code == "ControlLeft" || code == "ControlRight") return "Control";
    if (code == "Escape") return "Escape";
    if (code == "Enter" || code == "NumpadEnter") return "Enter";
    if (code == "Tab") return "Tab";
    return std::string();
}

class WebWindow final : public Window {
public:
    bool Init(const std::string& title) {
        SetTitle(title);
        UpdateSize();
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &WebWindow::OnKey);
        emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &WebWindow::OnKey);
        emscripten_set_mousedown_callback(kCanvas, this, true, &WebWindow::OnMouse);
        emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &WebWindow::OnMouse);
        emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &WebWindow::OnMouse);
        emscripten_set_touchstart_callback(kCanvas, this, true, &WebWindow::OnTouch);
        emscripten_set_touchend_callback(kCanvas, this, true, &WebWindow::OnTouch);
        emscripten_set_touchmove_callback(kCanvas, this, true, &WebWindow::OnTouch);
        emscripten_set_touchcancel_callback(kCanvas, this, true, &WebWindow::OnTouch);
        emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &WebWindow::OnBlur);
        emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, true, &WebWindow::OnPointerLock);
        emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, true, &WebWindow::OnPointerLockError);
        return true;
    }

    bool PumpEvents(InputState& input) override {
        UpdateSize();
        // DOM events arrive between frames; apply them in order now.
        for (const Event& e : events_) {
            switch (e.kind) {
                case Event::Down:
                    if (!input.IsDown(e.key)) input.pressedThisFrame.insert(e.key);
                    input.down.insert(e.key);
                    break;
                case Event::Up: input.down.erase(e.key); break;
                case Event::Move:
                    input.mouseX = e.x;
                    input.mouseY = e.y;
                    break;
                case Event::Clear: input.down.clear(); break;
                case Event::Delta:
                    input.mouseDX += e.x;
                    input.mouseDY += e.y;
                    break;
                case Event::Unlock: input.mouseLocked = false; break;
                case Event::Lock: input.mouseLocked = true; break;
            }
        }
        events_.clear();
        // Pointer lock can only be requested from a user gesture: OnMouse asks
        // for it on the next click while the game wants the mouse locked.
        wantLock_ = input.mouseLocked;
        if (!wantLock_ && pointerLocked_) {
            relockOnClick_ = false;  // the game released it
            emscripten_exit_pointerlock();
        }
        if (pointerLocked_) input.mouseX = input.mouseY = 0.5f;  // UI clicks hit the crosshair
        input.viewWidth = width_;
        input.viewHeight = height_;
        return true;  // a web page is never "closed" by the game loop
    }

    void Present(const RenderTarget& frame) override {
        if (frame.width <= 0) return;
        oe_web_present_rgba(reinterpret_cast<const uint8_t*>(frame.color.data()), frame.width, frame.height);
    }

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    void SetTitle(const std::string& title) override { oe_web_set_title(title.c_str()); }

private:
    struct Event {
        enum Kind { Down, Up, Move, Clear, Delta, Unlock, Lock } kind;
        std::string key;
        float x = 0, y = 0;
    };

    // The drawing buffer follows the canvas' CSS size times the device pixel ratio.
    void UpdateSize() {
        double cssW = 0, cssH = 0;
        emscripten_get_element_css_size(kCanvas, &cssW, &cssH);
        double dpr = emscripten_get_device_pixel_ratio();
        cssW_ = cssW > 0 ? cssW : 1;
        cssH_ = cssH > 0 ? cssH : 1;
        int w = static_cast<int>(cssW_ * dpr + 0.5), h = static_cast<int>(cssH_ * dpr + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w != width_ || h != height_) {
            emscripten_set_canvas_element_size(kCanvas, w, h);
            width_ = w;
            height_ = h;
        }
    }

    void Push(Event::Kind kind, const std::string& key) { events_.push_back({kind, key, 0, 0}); }
    void Move(double targetX, double targetY) {
        Event e{Event::Move, std::string(), static_cast<float>(targetX / cssW_), static_cast<float>(targetY / cssH_)};
        events_.push_back(e);
    }

    static bool OnKey(int type, const EmscriptenKeyboardEvent* e, void* user) {
        auto* self = static_cast<WebWindow*>(user);
        std::string key = KeyName(e);
        if (key.empty()) return false;  // let the browser handle it (F5, F12, ...)
        if (!e->repeat || type == EMSCRIPTEN_EVENT_KEYUP) self->Push(type == EMSCRIPTEN_EVENT_KEYDOWN ? Event::Down : Event::Up, key);
        return true;  // prevent scrolling with arrows / space
    }

    static bool OnMouse(int type, const EmscriptenMouseEvent* e, void* user) {
        auto* self = static_cast<WebWindow*>(user);
        if (self->pointerLocked_) {
            if (e->movementX != 0 || e->movementY != 0) {
                self->events_.push_back({Event::Delta, std::string(), static_cast<float>(e->movementX), static_cast<float>(e->movementY)});
            }
        } else {
            self->Move(static_cast<double>(e->targetX), static_cast<double>(e->targetY));
            // The click that captures the pointer (game start, or after Escape)
            // is not passed to the game.
            if (type == EMSCRIPTEN_EVENT_MOUSEDOWN && (self->wantLock_ || self->relockOnClick_) && !self->lockUnavailable_) {
                emscripten_request_pointerlock(kCanvas, false);
                return true;
            }
        }
        const char* button = e->button == 0 ? "MouseLeft" : (e->button == 2 ? "MouseRight" : nullptr);
        if (button && type == EMSCRIPTEN_EVENT_MOUSEDOWN) self->Push(Event::Down, button);
        if (button && type == EMSCRIPTEN_EVENT_MOUSEUP) self->Push(Event::Up, button);
        return type == EMSCRIPTEN_EVENT_MOUSEDOWN;
    }

    // The first touch acts as the left mouse button.
    static bool OnTouch(int type, const EmscriptenTouchEvent* e, void* user) {
        auto* self = static_cast<WebWindow*>(user);
        if (e->numTouches > 0) self->Move(static_cast<double>(e->touches[0].targetX), static_cast<double>(e->touches[0].targetY));
        if (type == EMSCRIPTEN_EVENT_TOUCHSTART) self->Push(Event::Down, "MouseLeft");
        if (type == EMSCRIPTEN_EVENT_TOUCHEND || type == EMSCRIPTEN_EVENT_TOUCHCANCEL) self->Push(Event::Up, "MouseLeft");
        return true;
    }

    // The browser releases the pointer on Escape; tell the game.
    static bool OnPointerLock(int, const EmscriptenPointerlockChangeEvent* e, void* user) {
        auto* self = static_cast<WebWindow*>(user);
        bool was = self->pointerLocked_;
        self->pointerLocked_ = e->isActive != 0;
        if (was && !self->pointerLocked_) {
            if (self->wantLock_) self->relockOnClick_ = true;
            self->events_.push_back({Event::Unlock, std::string(), 0, 0});
        }
        if (!was && self->pointerLocked_) {
            self->relockOnClick_ = false;
            self->events_.push_back({Event::Lock, std::string(), 0, 0});
        }
        return false;
    }

    // Pointer lock refused (some embeds, mobile browsers): stop asking and let
    // clicks through, so the game stays playable with absolute mouse input.
    static bool OnPointerLockError(int, const void*, void* user) {
        auto* self = static_cast<WebWindow*>(user);
        self->lockUnavailable_ = true;
        self->relockOnClick_ = false;
        return false;
    }

    static bool OnBlur(int, const EmscriptenFocusEvent*, void* user) {
        static_cast<WebWindow*>(user)->events_.push_back({Event::Clear, std::string(), 0, 0});
        return false;
    }

    std::vector<Event> events_;
    bool wantLock_ = false;
    bool pointerLocked_ = false;
    bool relockOnClick_ = false;  // released by Escape, not by the game
    bool lockUnavailable_ = false;
    int width_ = 0, height_ = 0;
    double cssW_ = 1, cssH_ = 1;
};

class WebAudioDevice final : public AudioDevice {
public:
    const char* Name() const override { return "WebAudio (48 kHz stereo)"; }
    void Submit(const float* samples, int frames) override { oe_web_audio_submit(samples, frames); }
};

}  // namespace

std::unique_ptr<AudioDevice> CreateAudioDevice(int sampleRate) {
    if (!oe_web_audio_init(sampleRate)) return nullptr;
    return std::make_unique<WebAudioDevice>();
}

std::unique_ptr<Window> CreatePlatformWindow(const std::string& title, int, int) {
    auto w = std::make_unique<WebWindow>();
    if (!w->Init(title)) return nullptr;
    return w;
}

void PlatformEnableHighDpi() {}

std::string PlatformUserLanguage() { return "en"; }  // not needed by the web player yet

const char* PlatformName() { return "web"; }

double PlatformTimeSeconds() { return emscripten_get_now() / 1000.0; }

void PlatformSleep(double) {}  // the browser drives the loop; blocking would freeze the page

bool PlatformOpenUrl(const std::string& url) {
    oe_web_open_url(url.c_str());
    return true;
}

std::string ExecutableDirectory() { return "/"; }

void PlatformShowError(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
    oe_web_show_error(title.c_str(), message.c_str());
}

void PlatformSetBinaryStdio() {}

}  // namespace oe
