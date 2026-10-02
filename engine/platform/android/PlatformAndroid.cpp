// Android platform (NDK, NativeActivity through android_native_app_glue): the
// activity's window, touch/mouse/keyboard/gamepad input mapped to InputState
// names, AAudio output, lifecycle (pause, rotation, background) and logcat.
// The GLES3 device is in GpuAndroid.cpp. Built by build_android.sh /
// build_android.bat as liboe_player.so; `oe package --android` ships it.
#include <aaudio/AAudio.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/Platform.h"
#include "platform/GamepadInput.h"
#include "platform/android/AndroidApp.h"

namespace oe {

namespace {

const char* kLogTag = "OwnEngine";

class AndroidAudioDevice;

// Input collected by the glue callbacks, applied in PumpEvents.
struct Event {
    enum Kind { Down, Up, Move, Clear, Delta, TouchDown, TouchMove, TouchUp } kind;
    std::string key;
    float x = 0, y = 0;
    int id = 0;  // Touch*: pointer id
};

struct State {
    android_app* app = nullptr;
    ANativeWindow* window = nullptr;
    bool resumed = false;
    std::function<void()> surfaceLost;
    std::vector<Event> events;
    AndroidAudioDevice* audio = nullptr;
    // Touch / mouse: the first pointer down acts as the left mouse button.
    int primaryPointer = -1;
    float lastX = 0, lastY = 0;
    bool mouseLocked = false;  // the game asked for input.lockMouse: drags become mouseDX/DY
    GamepadSnapshot gamepad;
    std::array<bool, 4> dpadKeys{}, hat{};  // Up, Down, Left, Right
    std::array<bool, 2> triggerKeys{}, analogTriggerSeen{};
    std::set<std::string> gamepadLegacy;
};

State& S() {
    static State s;
    return s;
}

bool AndroidGamepadPresent(int device);

// ----- Logcat ------------------------------------------------------------------------

// Engine logs go to stderr (OE_LOG_*); on Android that is /dev/null, so a
// thread copies stdout/stderr lines to logcat (`adb logcat -s OwnEngine`).
void* LogcatPump(void* arg) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    char buf[1024];
    std::string line;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    return nullptr;
}

void RedirectStdioToLogcat() {
    static bool done = false;
    if (done) return;
    done = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, LogcatPump, reinterpret_cast<void*>(static_cast<intptr_t>(fds[0]))) == 0) pthread_detach(thread);
}

// ----- Audio -------------------------------------------------------------------------

// AAudio output stream (API 26+), written without blocking from the game
// loop; frames that do not fit are dropped instead of adding latency.
class AndroidAudioDevice final : public AudioDevice {
public:
    explicit AndroidAudioDevice(int sampleRate) : rate_(sampleRate) {}
    ~AndroidAudioDevice() override {
        if (S().audio == this) S().audio = nullptr;
        Close();
    }

    bool Open() {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, rate_);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (r != AAUDIO_OK || !stream_) {
            stream_ = nullptr;
            return false;
        }
        // ~80 ms of buffer: the mixer runs with the 60 Hz simulation, so a
        // frame hitch must not starve the device.
        int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream_);
        int32_t want = rate_ * 8 / 100;
        AAudioStream_setBufferSizeInFrames(stream_, capacity > 0 && capacity < want ? capacity : want);
        AAudioStream_requestStart(stream_);
        return true;
    }

    const char* Name() const override { return "AAudio (48 kHz stereo)"; }

    void Submit(const float* samples, int frames) override {
        if (paused_ || frames <= 0) return;
        if (!stream_) {
            // Reopen after a disconnect, at most twice a second.
            double now = PlatformTimeSeconds();
            if (now < retryAt_) return;
            retryAt_ = now + 0.5;
            if (!Open()) return;
        }
        aaudio_result_t r = AAudioStream_write(stream_, samples, frames, 0);
        if (r == AAUDIO_ERROR_DISCONNECTED) {  // headphones unplugged, device changed: reopen next time
            Close();
        }
    }

    void SetPaused(bool paused) {
        paused_ = paused;
        if (!stream_) return;
        if (paused) AAudioStream_requestPause(stream_);
        else AAudioStream_requestStart(stream_);
    }

private:
    void Close() {
        if (!stream_) return;
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }

    int rate_;
    AAudioStream* stream_ = nullptr;
    bool paused_ = false;
    double retryAt_ = 0;
};

// ----- Immersive mode (UI thread) ----------------------------------------------------

// Hiding the navigation bar is a View call that must run on the activity's
// UI thread, while the game runs on the glue's thread and there is no Java
// code to post a Runnable. oe_ANativeActivity_onCreate (the entry point named
// in the manifest, on the UI thread) adds a pipe to the UI thread's looper;
// the game thread writes a byte and the callback below runs on the UI thread.
struct UiThread {
    ANativeActivity* activity = nullptr;  // only touched on the UI thread
    ALooper* looper = nullptr;
    int readFd = -1;
    std::atomic<int> writeFd{-1};
    void (*gluedOnDestroy)(ANativeActivity*) = nullptr;
};

UiThread& Ui() {
    static UiThread u;
    return u;
}

void RequestImmersive() {
    int fd = Ui().writeFd.load();
    if (fd >= 0) {
        const char one = 1;
        (void)!write(fd, &one, 1);
    }
}

// On the UI thread: fullscreen with the navigation bar hidden (it comes back
// with a swipe from the edge and hides again), drawing into the display cutout.
void ApplyImmersive(ANativeActivity* activity) {
    JNIEnv* env = activity->env;
    jobject act = activity->clazz;
    jclass activityClass = env->GetObjectClass(act);
    jmethodID getWindow = env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
    jobject window = getWindow ? env->CallObjectMethod(act, getWindow) : nullptr;
    if (window && !env->ExceptionCheck()) {
        jclass windowClass = env->GetObjectClass(window);
        jmethodID getDecorView = env->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;");
        jobject decor = getDecorView ? env->CallObjectMethod(window, getDecorView) : nullptr;
        if (decor && !env->ExceptionCheck()) {
            jmethodID setVisibility = env->GetMethodID(env->GetObjectClass(decor), "setSystemUiVisibility", "(I)V");
            // LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY
            if (setVisibility) env->CallVoidMethod(decor, setVisibility, static_cast<jint>(0x100 | 0x200 | 0x400 | 0x2 | 0x4 | 0x1000));
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        // WindowManager.LayoutParams.layoutInDisplayCutoutMode = SHORT_EDGES (API 28+;
        // older devices have no such field and keep the default).
        jmethodID getAttributes = env->GetMethodID(windowClass, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
        jmethodID setAttributes = env->GetMethodID(windowClass, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
        jobject params = getAttributes ? env->CallObjectMethod(window, getAttributes) : nullptr;
        if (params && setAttributes && !env->ExceptionCheck()) {
            jfieldID cutout = env->GetFieldID(env->GetObjectClass(params), "layoutInDisplayCutoutMode", "I");
            if (cutout && !env->ExceptionCheck()) {
                if (env->GetIntField(params, cutout) != 1) {
                    env->SetIntField(params, cutout, 1);
                    env->CallVoidMethod(window, setAttributes, params);
                }
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

int OnUiPipe(int fd, int, void*) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
    if (Ui().activity) ApplyImmersive(Ui().activity);
    return 1;  // keep the callback
}

void OnUiDestroy(ANativeActivity* activity) {
    UiThread& u = Ui();
    int fd = u.writeFd.exchange(-1);
    if (u.looper && u.readFd >= 0) ALooper_removeFd(u.looper, u.readFd);
    if (u.readFd >= 0) close(u.readFd);
    if (fd >= 0) close(fd);
    u.readFd = -1;
    u.looper = nullptr;
    u.activity = nullptr;
    if (u.gluedOnDestroy) u.gluedOnDestroy(activity);
}

// ----- Input -------------------------------------------------------------------------

// Key names match the other platforms (scene/Systems.h). Gamepads: D-pad =
// arrows, A = Space, B = Escape, X = Shift, Y = Control, Start = Enter.
const char* KeyName(int32_t code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
        static const char* letters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        return letters[code - AKEYCODE_A];
    }
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        static const char* digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        return digits[code - AKEYCODE_0];
    }
    switch (code) {
        case AKEYCODE_SPACE: case AKEYCODE_BUTTON_A: return "Space";
        case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_DPAD_CENTER: case AKEYCODE_BUTTON_START: return "Enter";
        case AKEYCODE_ESCAPE: case AKEYCODE_BACK: case AKEYCODE_BUTTON_B: return "Escape";
        case AKEYCODE_TAB: return "Tab";
        case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: case AKEYCODE_BUTTON_X: return "Shift";
        case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: case AKEYCODE_BUTTON_Y: return "Control";
        case AKEYCODE_DPAD_LEFT: return "Left";
        case AKEYCODE_DPAD_RIGHT: return "Right";
        case AKEYCODE_DPAD_UP: return "Up";
        case AKEYCODE_DPAD_DOWN: return "Down";
        default: return nullptr;
    }
}

void Push(Event::Kind kind, const char* key, float x = 0, float y = 0) {
    S().events.push_back({kind, key ? std::string(key) : std::string(), x, y});
}

void UpdateGamepadLegacy() {
    State& s = S();
    std::set<std::string> keys;
    const char* names[] = {"Space", "Escape", "Shift", "Control"};
    for (size_t i = 0; i < 4; ++i) if (s.gamepad.buttons[i]) keys.insert(names[i]);
    if (s.gamepad.buttons[6]) keys.insert("Enter");
    const char* directions[] = {"Up", "Down", "Left", "Right"};
    const bool stick[] = {s.gamepad.axes[1] > 0.5f, s.gamepad.axes[1] < -0.5f,
                          s.gamepad.axes[0] < -0.5f, s.gamepad.axes[0] > 0.5f};
    for (size_t i = 0; i < 4; ++i) {
        s.gamepad.buttons[10 + i] = s.dpadKeys[i] || s.hat[i];
        if (stick[i] || s.gamepad.buttons[10 + i]) keys.insert(directions[i]);
    }
    for (const std::string& key : s.gamepadLegacy) if (!keys.count(key)) Push(Event::Up, key.c_str());
    for (const std::string& key : keys) if (!s.gamepadLegacy.count(key)) Push(Event::Down, key.c_str());
    s.gamepadLegacy = std::move(keys);
}

void ReleaseAndroidGamepad() {
    State& s = S();
    for (const std::string& key : s.gamepadLegacy) Push(Event::Up, key.c_str());
    s.gamepadLegacy.clear();
    s.gamepad = GamepadSnapshot{};
    s.dpadKeys.fill(false);
    s.hat.fill(false);
    s.triggerKeys.fill(false);
    s.analogTriggerSeen.fill(false);
}

bool SelectAndroidGamepad(const AInputEvent* event) {
    State& s = S();
    int device = AInputEvent_getDeviceId(event);
    if (s.gamepad.connected && s.gamepad.device != device) {
        if (AndroidGamepadPresent(s.gamepad.device)) return false;
        ReleaseAndroidGamepad();
    }
    s.gamepad.connected = true;
    s.gamepad.device = device;
    return true;
}

int GamepadKeyIndex(int32_t key) {
    const int32_t codes[] = {AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_X, AKEYCODE_BUTTON_Y,
        AKEYCODE_BUTTON_L1, AKEYCODE_BUTTON_R1, AKEYCODE_BUTTON_START, AKEYCODE_BUTTON_SELECT,
        AKEYCODE_BUTTON_THUMBL, AKEYCODE_BUTTON_THUMBR, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_DOWN,
        AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_RIGHT};
    for (int i = 0; i < 14; ++i) if (key == codes[i]) return i;
    return -1;
}

// Every finger (InputState::touches), normalized to the window.
void PushTouch(Event::Kind kind, const AInputEvent* event, size_t i) {
    State& s = S();
    if (!s.window) return;
    float w = static_cast<float>(ANativeWindow_getWidth(s.window)), h = static_cast<float>(ANativeWindow_getHeight(s.window));
    if (w <= 0 || h <= 0) return;
    Event e{kind, std::string(), AMotionEvent_getX(event, i) / w, AMotionEvent_getY(event, i) / h};
    e.id = AMotionEvent_getPointerId(event, i);
    s.events.push_back(e);
}

void PushMove(float px, float py) {
    State& s = S();
    if (!s.window) return;
    float w = static_cast<float>(ANativeWindow_getWidth(s.window)), h = static_cast<float>(ANativeWindow_getHeight(s.window));
    if (w <= 0 || h <= 0) return;
    Push(Event::Move, nullptr, px / w, py / h);
}

int32_t OnJoystick(const AInputEvent* event) {
    State& s = S();
    if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_CANCEL) {
        ReleaseAndroidGamepad();
        return 1;
    }
    if (!SelectAndroidGamepad(event)) return 1;
    s.gamepad.axes[0] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
    s.gamepad.axes[1] = -AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
    s.gamepad.axes[2] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
    s.gamepad.axes[3] = -AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
    const float triggers[] = {
        std::max(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0), AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0)),
        std::max(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0))
    };
    for (size_t i = 0; i < 2; ++i) {
        if (triggers[i] > 0) s.analogTriggerSeen[i] = true;
        s.gamepad.axes[4 + i] = s.analogTriggerSeen[i] ? triggers[i] : (s.triggerKeys[i] ? 1.0f : 0.0f);
    }
    float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
    float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
    s.hat = {hy < -0.5f, hy > 0.5f, hx < -0.5f, hx > 0.5f};
    UpdateGamepadLegacy();
    return 1;
}

int32_t OnMotion(const AInputEvent* event) {
    State& s = S();
    const int32_t source = AInputEvent_getSource(event);
    if ((source & AINPUT_SOURCE_CLASS_JOYSTICK) == AINPUT_SOURCE_CLASS_JOYSTICK) return OnJoystick(event);
    const int32_t raw = AMotionEvent_getAction(event);
    const int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
    const size_t index = static_cast<size_t>((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const bool mouse = (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
    if (!mouse) {
        // Multi-touch: all fingers, besides the first one acting as the mouse below.
        switch (action) {
            case AMOTION_EVENT_ACTION_DOWN: PushTouch(Event::TouchDown, event, 0); break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN: PushTouch(Event::TouchDown, event, index); break;
            case AMOTION_EVENT_ACTION_MOVE:
                for (size_t i = 0, n = AMotionEvent_getPointerCount(event); i < n; ++i) PushTouch(Event::TouchMove, event, i);
                break;
            case AMOTION_EVENT_ACTION_POINTER_UP: PushTouch(Event::TouchUp, event, index); break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_CANCEL:
                for (size_t i = 0, n = AMotionEvent_getPointerCount(event); i < n; ++i) PushTouch(Event::TouchUp, event, i);
                break;
            default: break;
        }
    }
    const char* button = mouse && (AMotionEvent_getButtonState(event) & AMOTION_EVENT_BUTTON_SECONDARY) ? "MouseRight" : "MouseLeft";

    auto track = [&](size_t i) {
        float x = AMotionEvent_getX(event, i), y = AMotionEvent_getY(event, i);
        if (s.mouseLocked) {
            // Mouse look: drag distance in pixels, like relative mouse motion.
            if (x != s.lastX || y != s.lastY) Push(Event::Delta, nullptr, x - s.lastX, y - s.lastY);
        } else {
            PushMove(x, y);
        }
        s.lastX = x;
        s.lastY = y;
    };
    auto begin = [&](size_t i) {
        s.primaryPointer = AMotionEvent_getPointerId(event, i);
        s.lastX = AMotionEvent_getX(event, i);
        s.lastY = AMotionEvent_getY(event, i);
        if (!s.mouseLocked) PushMove(s.lastX, s.lastY);
        Push(Event::Down, button);
    };

    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN: begin(0); return 1;
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (s.primaryPointer < 0) begin(index);
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_MOVE: {
            const size_t count = AMotionEvent_getPointerCount(event);
            for (size_t i = 0; i < count; ++i) {
                if (action == AMOTION_EVENT_ACTION_HOVER_MOVE || AMotionEvent_getPointerId(event, i) == s.primaryPointer) {
                    track(i);
                    break;
                }
            }
            return 1;
        }
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (AMotionEvent_getPointerId(event, index) != s.primaryPointer) return 1;
            [[fallthrough]];
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            if (s.primaryPointer >= 0) {
                Push(Event::Up, "MouseLeft");
                Push(Event::Up, "MouseRight");
            }
            s.primaryPointer = -1;
            return 1;
        default: return 0;
    }
}

int32_t OnInput(android_app*, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) return OnMotion(event);
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) return 0;
    const int32_t code = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    const int32_t source = AInputEvent_getSource(event);
    int button = GamepadKeyIndex(code);
    bool padSource = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                     (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
                     (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
    if ((button >= 0 && (padSource || code >= AKEYCODE_BUTTON_A)) || code == AKEYCODE_BUTTON_L2 || code == AKEYCODE_BUTTON_R2) {
        if (!SelectAndroidGamepad(event)) return 1;
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 1;
        bool down = action == AKEY_EVENT_ACTION_DOWN;
        State& s = S();
        if (button >= 10) s.dpadKeys[static_cast<size_t>(button - 10)] = down;
        else if (button >= 0) s.gamepad.buttons[static_cast<size_t>(button)] = down;
        else {
            size_t trigger = code == AKEYCODE_BUTTON_L2 ? 0 : 1;
            s.triggerKeys[trigger] = down;
            if (!s.analogTriggerSeen[trigger]) s.gamepad.axes[4 + trigger] = down ? 1.0f : 0.0f;
        }
        UpdateGamepadLegacy();
        return 1;
    }
    const char* key = KeyName(code);
    if (!key) return 0;  // volume, home, ...: the system handles them
    if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) == 0) Push(Event::Down, key);
    if (action == AKEY_EVENT_ACTION_UP) Push(Event::Up, key);
    return 1;  // Back goes to the game as Escape instead of closing the activity
}

void OnCommand(android_app* app, int32_t cmd) {
    State& s = S();
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            s.window = app->window;
            RequestImmersive();
            // Games keep the screen on and use the whole display.
            ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
            break;
        case APP_CMD_TERM_WINDOW:
            ReleaseAndroidGamepad();
            // The window dies when this handler returns: release the EGL surface now.
            if (s.surfaceLost) s.surfaceLost();
            s.window = nullptr;
            break;
        case APP_CMD_GAINED_FOCUS:
            RequestImmersive();  // the system shows the bars again after dialogs, the notification shade, ...
            break;
        case APP_CMD_LOST_FOCUS:
            ReleaseAndroidGamepad();
            s.events.push_back({Event::Clear, std::string(), 0, 0});
            s.primaryPointer = -1;
            break;
        case APP_CMD_RESUME:
            s.resumed = true;
            if (s.audio) s.audio->SetPaused(false);
            break;
        case APP_CMD_PAUSE:
            ReleaseAndroidGamepad();
            s.resumed = false;
            s.events.push_back({Event::Clear, std::string(), 0, 0});
            s.primaryPointer = -1;
            if (s.audio) s.audio->SetPaused(true);
            break;
        default: break;
    }
}

// Handles pending looper events. Blocks while the activity is in the
// background (no window or paused): the game is frozen there, as on desktop
// when minimized. Returns false once the activity is being destroyed.
bool Poll(bool block) {
    State& s = S();
    for (;;) {
        if (!s.app || s.app->destroyRequested) return false;
        const bool active = s.resumed && s.window;
        int events = 0;
        android_poll_source* source = nullptr;
        int id = ALooper_pollOnce(active || !block ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (id >= 0) {
            if (source) source->process(s.app, source);
            continue;
        }
        if (id == ALOOPER_POLL_ERROR) return !s.app->destroyRequested;
        if (id == ALOOPER_POLL_TIMEOUT && (active || !block)) return !s.app->destroyRequested;
        // ALOOPER_POLL_WAKE / CALLBACK: keep waiting.
    }
}

class AndroidWindow final : public Window {
public:
    bool PumpEvents(InputState& input) override {
        State& s = S();
        s.mouseLocked = input.mouseLocked;
        if (!Poll(true)) return false;
        if (s.gamepad.connected && !AndroidGamepadPresent(s.gamepad.device)) ReleaseAndroidGamepad();
        for (const Event& e : s.events) {
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
                case Event::Clear:
                    gamepad_.Reset(input);
                    input.down.clear();
                    input.axes.clear();
                    input.pressedThisFrame.clear();
                    input.touches.clear();
                    break;
                case Event::Delta:
                    input.mouseDX += e.x;
                    input.mouseDY += e.y;
                    break;
                case Event::TouchDown:
                case Event::TouchMove: {
                    auto it = std::find_if(input.touches.begin(), input.touches.end(), [&](const InputState::Touch& t) { return t.id == e.id; });
                    if (it == input.touches.end()) {
                        if (e.kind == Event::TouchMove) break;  // lifted already
                        InputState::Touch t;
                        t.id = e.id;
                        t.began = true;
                        input.touches.push_back(t);
                        it = input.touches.end() - 1;
                    }
                    it->x = e.x;
                    it->y = e.y;
                    break;
                }
                case Event::TouchUp:
                    input.touches.erase(std::remove_if(input.touches.begin(), input.touches.end(), [&](const InputState::Touch& t) { return t.id == e.id; }),
                                        input.touches.end());
                    break;
            }
        }
        s.events.clear();
        gamepad_.Apply(input, s.gamepad);
        if (input.mouseLocked) input.mouseX = input.mouseY = 0.5f;  // UI taps hit the crosshair, like the other platforms
        input.viewWidth = Width();
        input.viewHeight = Height();
        return true;
    }

    // Software renderer: CPU pixels into the window buffer, which the
    // compositor scales to the screen.
    void Present(const RenderTarget& frame) override {
        ANativeWindow* window = S().window;
        if (!window || frame.width <= 0 || frame.height <= 0) return;
        if (window != geometryWindow_ || frame.width != geometryW_ || frame.height != geometryH_) {
            ANativeWindow_setBuffersGeometry(window, frame.width, frame.height, WINDOW_FORMAT_RGBA_8888);
            geometryWindow_ = window;
            geometryW_ = frame.width;
            geometryH_ = frame.height;
        }
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window, &buffer, nullptr) != 0) return;
        const int w = std::min(frame.width, static_cast<int>(buffer.width)), h = std::min(frame.height, static_cast<int>(buffer.height));
        for (int y = 0; y < h; ++y) {
            // RenderTarget pixels are 0xAABBGGRR: R,G,B,A bytes, the RGBA_8888 layout.
            std::memcpy(static_cast<uint32_t*>(buffer.bits) + static_cast<size_t>(y) * static_cast<size_t>(buffer.stride),
                        frame.color.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.width), static_cast<size_t>(w) * 4);
        }
        ANativeWindow_unlockAndPost(window);
    }

    int Width() const override {
        if (S().window) width_ = ANativeWindow_getWidth(S().window);
        return width_;
    }
    int Height() const override {
        if (S().window) height_ = ANativeWindow_getHeight(S().window);
        return height_;
    }
    void SetTitle(const std::string&) override {}
    void* NativeHandle() const override { return S().window; }

private:
    mutable int width_ = 1280, height_ = 720;  // last known size while there is no window
    GamepadInput gamepad_;
    ANativeWindow* geometryWindow_ = nullptr;
    int geometryW_ = 0, geometryH_ = 0;
};

// ----- JNI ---------------------------------------------------------------------------

// Runs `body` with a JNIEnv attached to this thread; clears Java exceptions.
template <typename F>
bool WithJni(F body) {
    State& s = S();
    if (!s.app || !s.app->activity || !s.app->activity->vm) return false;
    JavaVM* vm = s.app->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    bool ok = body(env, s.app->activity->clazz);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        ok = false;
    }
    if (attached) vm->DetachCurrentThread();
    return ok;
}

bool AndroidGamepadPresent(int device) {
    bool present = true;
    bool queried = WithJni([&](JNIEnv* env, jobject) {
        jclass cls = env->FindClass("android/view/InputDevice");
        if (!cls) return false;
        jmethodID getDevice = env->GetStaticMethodID(cls, "getDevice", "(I)Landroid/view/InputDevice;");
        if (!getDevice) { env->DeleteLocalRef(cls); return false; }
        jobject value = env->CallStaticObjectMethod(cls, getDevice, device);
        present = value != nullptr;
        if (value) env->DeleteLocalRef(value);
        env->DeleteLocalRef(cls);
        return true;
    });
    return queried ? present : true;  // unavailable JNI must not fabricate a disconnect
}

}  // namespace

// ----- Entry point ---------------------------------------------------------------------

// NativeActivity calls this (manifest meta-data android.app.func_name) on the UI
// thread; it prepares the UI-thread pipe and hands over to the glue, which
// starts android_main on its own thread.
extern "C" JNIEXPORT void oe_ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    UiThread& u = Ui();
    u.activity = activity;
    u.looper = ALooper_forThread();
    int fds[2];
    if (u.looper && pipe(fds) == 0) {
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        fcntl(fds[1], F_SETFL, O_NONBLOCK);
        u.readFd = fds[0];
        u.writeFd.store(fds[1]);
        ALooper_addFd(u.looper, fds[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, OnUiPipe, nullptr);
    }
    ANativeActivity_onCreate(activity, savedState, savedStateSize);
    // After the glue installed its callbacks: release the pipe when the activity goes away.
    u.gluedOnDestroy = activity->callbacks->onDestroy;
    activity->callbacks->onDestroy = OnUiDestroy;
}

// ----- AndroidApp.h --------------------------------------------------------------------

void AndroidSetApp(android_app* app) {
    RedirectStdioToLogcat();
    State& s = S();
    s.app = app;
    s.window = app ? app->window : nullptr;
    if (app) {
        app->onAppCmd = OnCommand;
        app->onInputEvent = OnInput;
    }
}

android_app* AndroidApp() { return S().app; }

ANativeWindow* AndroidCurrentWindow() { return S().window; }

void AndroidSetSurfaceLostHandler(std::function<void()> handler) { S().surfaceLost = std::move(handler); }

bool AndroidReadAsset(const std::string& name, std::vector<unsigned char>& out) {
    State& s = S();
    if (!s.app || !s.app->activity || !s.app->activity->assetManager) return false;
    AAsset* asset = AAssetManager_open(s.app->activity->assetManager, name.c_str(), AASSET_MODE_STREAMING);
    if (!asset) return false;
    off64_t length = AAsset_getLength64(asset);
    out.resize(static_cast<size_t>(length > 0 ? length : 0));
    size_t done = 0;
    while (done < out.size()) {
        int n = AAsset_read(asset, out.data() + done, out.size() - done);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    AAsset_close(asset);
    return done == out.size();
}

std::string AndroidDataDir() {
    State& s = S();
    if (s.app && s.app->activity && s.app->activity->internalDataPath) return s.app->activity->internalDataPath;
    return ".";
}

void AndroidFinish() {
    State& s = S();
    if (!s.app || !s.app->activity) return;
    ANativeActivity_finish(s.app->activity);
    // android_main must keep handling events until the activity is gone.
    while (Poll(false)) PlatformSleep(0.01);
}

// ----- Platform.h --------------------------------------------------------------------

std::unique_ptr<AudioDevice> CreateAudioDevice(int sampleRate) {
    auto device = std::make_unique<AndroidAudioDevice>(sampleRate);
    if (!device->Open()) return nullptr;
    S().audio = device.get();
    return device;
}

// Waits until the activity has a window (it arrives a moment after launch).
std::unique_ptr<Window> CreatePlatformWindow(const std::string&, int, int) {
    State& s = S();
    if (!s.app) return nullptr;
    while (!s.window) {
        if (!Poll(true)) return nullptr;
    }
    return std::make_unique<AndroidWindow>();
}

void PlatformEnableHighDpi() {}

std::string PlatformUserLanguage() {
    State& s = S();
    char lang[2] = {0, 0};
    if (s.app && s.app->config) AConfiguration_getLanguage(s.app->config, lang);
    if (lang[0] == 0) return "en";
    return std::string(lang, 2);
}

const char* PlatformName() { return "android"; }

double PlatformTimeSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void PlatformSleep(double seconds) {
    if (seconds > 0) usleep(static_cast<useconds_t>(seconds * 1e6));
}

// Intent.ACTION_VIEW: opens the browser (or the app registered for the link).
bool PlatformOpenUrl(const std::string& url) {
    return WithJni([&](JNIEnv* env, jobject activity) {
        jclass uriClass = env->FindClass("android/net/Uri");
        jclass intentClass = env->FindClass("android/content/Intent");
        if (!uriClass || !intentClass) return false;
        jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jmethodID ctor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
        jmethodID addFlags = env->GetMethodID(intentClass, "addFlags", "(I)Landroid/content/Intent;");
        jmethodID start = env->GetMethodID(env->GetObjectClass(activity), "startActivity", "(Landroid/content/Intent;)V");
        if (!parse || !ctor || !addFlags || !start) return false;
        jstring jurl = env->NewStringUTF(url.c_str());
        jstring action = env->NewStringUTF("android.intent.action.VIEW");
        jobject uri = env->CallStaticObjectMethod(uriClass, parse, jurl);
        jobject intent = env->NewObject(intentClass, ctor, action, uri);
        env->CallObjectMethod(intent, addFlags, static_cast<jint>(0x10000000));  // FLAG_ACTIVITY_NEW_TASK
        env->CallVoidMethod(activity, start, intent);
        return !env->ExceptionCheck();
    });
}

std::string ExecutableDirectory() { return AndroidDataDir(); }

void PlatformShowError(const std::string& title, const std::string& message) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: %s", title.c_str(), message.c_str());
}

void PlatformAttachParentConsole() {}

void PlatformSetBinaryStdio() {}

bool PlatformReplaceFile(const std::string& from, const std::string& to, std::string* error) {
    if (std::rename(from.c_str(), to.c_str()) == 0) return true;
    if (error) *error = "cannot replace save file " + to;
    return false;
}

SaveStorage PlatformSaveStorage(const std::string& gameName) {
    State& s = S();
    if (!s.app || !s.app->activity || !s.app->activity->internalDataPath) return {};
    return {std::string(s.app->activity->internalDataPath) + "/saves/" + gameName, {}, {}};
}

}  // namespace oe
