#pragma once
// APK assembly for `oe package --android`, without the Android SDK's aapt2:
// the binary AndroidManifest.xml and resources.arsc are written here, and
// the archive is aligned like zipalign. Signing (apksigner) is up to the
// caller (tools/oe). Pure C++: also exercised by the self tests.
#include <string>
#include <utility>
#include <vector>

namespace oe {

struct AndroidAppInfo {
    std::string packageName;  // application id, e.g. "com.example.mygame"
    std::string label;        // name under the launcher icon
    int versionCode = 1;
    std::string versionName = "1.0";
    int minSdk = 26;  // Android 8.0: AAudio
    int targetSdk = 35;
    std::string orientation = "landscape";  // landscape | portrait | auto
    bool hasIcon = false;     // res/drawable/icon.png is in the APK
    bool debuggable = false;  // lets `adb shell run-as` read the app's files
};

// "com.example.game": two or more segments of [A-Za-z][A-Za-z0-9_]*.
bool IsValidAndroidPackageName(const std::string& name);
// A valid application id derived from a project name ("My Game!" -> "com.ownengine.mygame").
std::string DefaultAndroidPackageName(const std::string& projectName);

// Compiled (binary XML) AndroidManifest.xml: a NativeActivity that loads
// liboe_player.so, no Java code.
std::vector<unsigned char> BuildAndroidManifest(const AndroidAppInfo& app);
// Compiled resource table with one resource, drawable/icon (0x7f010000) =
// res/drawable/icon.png (the launcher icon).
std::vector<unsigned char> BuildAndroidResources(const std::string& packageName);
// The same manifest and resource table in aapt2's protobuf format (app bundles).
std::vector<unsigned char> BuildAndroidManifestProto(const AndroidAppInfo& app);
std::vector<unsigned char> BuildAndroidResourcesProto(const std::string& packageName);

struct ApkContents {
    AndroidAppInfo app;
    std::vector<std::pair<std::string, std::string>> nativeLibs;  // {abi, path of liboe_player.so}
    std::vector<unsigned char> gamePak;  // WriteGamePak output
    std::vector<unsigned char> iconPng;  // optional launcher icon
};

// Writes an unsigned APK (manifest, resources, libs page-aligned and
// uncompressed, assets/game.pak + assets/game.id). Sign it before installing.
bool WriteUnsignedApk(const ApkContents& contents, const std::string& outPath, std::string* error);
// Writes an unsigned Android App Bundle (.aab, the format Google Play takes):
// BundleConfig.pb + base/ module (proto manifest, resources.pb, res/, lib/,
// assets/). Sign it with jarsigner before uploading.
bool WriteUnsignedAppBundle(const ApkContents& contents, const std::string& outPath, std::string* error);

}  // namespace oe
