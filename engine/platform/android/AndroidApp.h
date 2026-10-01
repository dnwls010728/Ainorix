#pragma once
// Android-only helpers shared by the platform files and the player's
// android_main (tools/player/main.cpp). Not part of Platform.h: nothing
// outside Android code may include this.
#include <functional>
#include <string>
#include <vector>

struct android_app;
struct ANativeWindow;

namespace oe {

// Hands the native_app_glue state to the platform layer (call first in
// android_main). Also routes stdout/stderr (engine logs) to logcat.
void AndroidSetApp(android_app* app);
android_app* AndroidApp();

// The window the activity currently shows, nullptr while it has none
// (paused, rotated, in the background).
ANativeWindow* AndroidCurrentWindow();

// Called on the app thread right before the current window is destroyed
// (the EGL device releases its surface there). One handler at a time.
void AndroidSetSurfaceLostHandler(std::function<void()> handler);

// Reads a file from the APK's assets/ folder.
bool AndroidReadAsset(const std::string& name, std::vector<unsigned char>& out);

// The app's private data folder (Context.getFilesDir()).
std::string AndroidDataDir();

// Closes the activity and handles its events until it is destroyed (the
// last call of android_main).
void AndroidFinish();

}  // namespace oe
