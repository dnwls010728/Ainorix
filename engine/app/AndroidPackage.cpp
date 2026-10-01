#include "app/AndroidPackage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Zip.h"

namespace oe {

namespace {

using Bytes = std::vector<unsigned char>;

void Put8(Bytes& out, uint32_t v) { out.push_back(static_cast<unsigned char>(v & 0xFF)); }
void Put16(Bytes& out, uint32_t v) {
    Put8(out, v);
    Put8(out, v >> 8);
}
void Put32(Bytes& out, uint32_t v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}
void Set32(Bytes& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<size_t>(i)] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
}
void Append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

std::u16string Utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n == 1) cp = c < 0x80 ? c : 0xFFFD;
        else if (i + n <= s.size()) {
            cp = c & (0xFF >> (n + 1));
            for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

// ResStringPool chunk, UTF-16 strings.
Bytes StringPool(const std::vector<std::string>& strings) {
    Bytes data;
    std::vector<uint32_t> offsets;
    for (const std::string& s : strings) {
        offsets.push_back(static_cast<uint32_t>(data.size()));
        std::u16string u = Utf16(s);
        if (u.size() > 0x7FFF) {
            Put16(data, 0x8000 | static_cast<uint32_t>(u.size() >> 16));
            Put16(data, static_cast<uint32_t>(u.size() & 0xFFFF));
        } else {
            Put16(data, static_cast<uint32_t>(u.size()));
        }
        for (char16_t c : u) Put16(data, c);
        Put16(data, 0);
    }
    while (data.size() % 4) data.push_back(0);
    const uint32_t header = 28, count = static_cast<uint32_t>(strings.size());
    Bytes out;
    Put16(out, 0x0001);  // RES_STRING_POOL_TYPE
    Put16(out, header);
    Put32(out, header + 4 * count + static_cast<uint32_t>(data.size()));
    Put32(out, count);
    Put32(out, 0);  // styles
    Put32(out, 0);  // flags: UTF-16, unsorted
    Put32(out, header + 4 * count);
    Put32(out, 0);
    for (uint32_t o : offsets) Put32(out, o);
    Append(out, data);
    return out;
}

// ----- Binary XML ------------------------------------------------------------------

enum : uint8_t { kTypeReference = 0x01, kTypeString = 0x03, kTypeIntDec = 0x10, kTypeIntHex = 0x11, kTypeBool = 0x12 };

struct XmlAttr {
    std::string name;
    uint32_t resId = 0;  // android:<name> attribute id; 0 = attribute without namespace
    uint8_t type = kTypeString;
    uint32_t data = 0;
    std::string text;  // kTypeString
};

struct XmlElement {
    std::string name;
    std::vector<XmlAttr> attrs;
    std::vector<XmlElement> children;
};

XmlAttr Str(const char* name, uint32_t id, const std::string& text) { return {name, id, kTypeString, 0, text}; }
XmlAttr Int(const char* name, uint32_t id, int v) { return {name, id, kTypeIntDec, static_cast<uint32_t>(v), ""}; }
XmlAttr Hex(const char* name, uint32_t id, uint32_t v) { return {name, id, kTypeIntHex, v, ""}; }
XmlAttr Bool(const char* name, uint32_t id, bool v) { return {name, id, kTypeBool, v ? 0xFFFFFFFFu : 0u, ""}; }
XmlAttr Ref(const char* name, uint32_t id, uint32_t target) { return {name, id, kTypeReference, target, ""}; }

class XmlEncoder {
public:
    Bytes Encode(XmlElement root) {
        // android:* attribute names come first in the string pool: the
        // resource map gives the framework their ids by index.
        Collect(root);
        for (const std::string& n : resOrder_) Index(n);
        prefix_ = Index("android");
        uri_ = Index("http://schemas.android.com/apk/res/android");
        Bytes body;
        Element(root, body);
        Bytes pool = StringPool(strings_);
        Bytes map;
        Put16(map, 0x0180);  // RES_XML_RESOURCE_MAP_TYPE
        Put16(map, 8);
        Put32(map, 8 + 4 * static_cast<uint32_t>(resOrder_.size()));
        for (const std::string& n : resOrder_) Put32(map, resNames_[n]);
        Bytes ns;
        Namespace(0x0100, ns);
        Namespace(0x0101, nsEnd_);
        Bytes out;
        Put16(out, 0x0003);  // RES_XML_TYPE
        Put16(out, 8);
        Put32(out, 0);
        Append(out, pool);
        Append(out, map);
        Append(out, ns);
        Append(out, body);
        Append(out, nsEnd_);
        Set32(out, 4, static_cast<uint32_t>(out.size()));
        return out;
    }

private:
    void Collect(const XmlElement& e) {
        for (const XmlAttr& a : e.attrs) {
            if (a.resId && !resNames_.count(a.name)) {
                resNames_[a.name] = a.resId;
                resOrder_.push_back(a.name);
            }
        }
        for (const XmlElement& c : e.children) Collect(c);
    }

    uint32_t Index(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        uint32_t i = static_cast<uint32_t>(strings_.size());
        strings_.push_back(s);
        index_[s] = i;
        return i;
    }

    void Node(Bytes& out, uint32_t type, uint32_t size) {
        Put16(out, type);
        Put16(out, 16);
        Put32(out, size);
        Put32(out, ++line_);
        Put32(out, 0xFFFFFFFFu);  // no comment
    }

    void Namespace(uint32_t type, Bytes& out) {
        Node(out, type, 24);
        Put32(out, prefix_);
        Put32(out, uri_);
    }

    void Element(XmlElement& e, Bytes& out) {
        // Attributes sorted by resource id: the framework merges them with
        // the requested attribute list in one ascending pass.
        std::stable_sort(e.attrs.begin(), e.attrs.end(), [](const XmlAttr& a, const XmlAttr& b) { return a.resId < b.resId; });
        const uint32_t name = Index(e.name);
        Node(out, 0x0102, 16 + 20 + 20 * static_cast<uint32_t>(e.attrs.size()));  // RES_XML_START_ELEMENT_TYPE
        Put32(out, 0xFFFFFFFFu);  // element namespace: none
        Put32(out, name);
        Put16(out, 20);  // attributeStart
        Put16(out, 20);  // attributeSize
        Put16(out, static_cast<uint32_t>(e.attrs.size()));
        Put16(out, 0);  // id / class / style attribute indices
        Put16(out, 0);
        Put16(out, 0);
        for (const XmlAttr& a : e.attrs) {
            Put32(out, a.resId ? uri_ : 0xFFFFFFFFu);
            Put32(out, Index(a.name));
            uint32_t data = a.type == kTypeString ? Index(a.text) : a.data;
            Put32(out, a.type == kTypeString ? data : 0xFFFFFFFFu);  // raw value
            Put16(out, 8);
            Put8(out, 0);
            Put8(out, a.type);
            Put32(out, data);
        }
        for (XmlElement& c : e.children) Element(c, out);
        Node(out, 0x0103, 24);  // RES_XML_END_ELEMENT_TYPE
        Put32(out, 0xFFFFFFFFu);
        Put32(out, name);
    }

    std::map<std::string, uint32_t> resNames_;
    std::vector<std::string> resOrder_;
    std::vector<std::string> strings_;
    std::map<std::string, uint32_t> index_;
    uint32_t prefix_ = 0, uri_ = 0, line_ = 0;
    Bytes nsEnd_;
};

// android.R.attr / android.R.style ids (stable across Android versions).
constexpr uint32_t kAttrTheme = 0x01010000, kAttrLabel = 0x01010001, kAttrIcon = 0x01010002, kAttrName = 0x01010003,
                   kAttrHasCode = 0x0101000c, kAttrDebuggable = 0x0101000f, kAttrExported = 0x01010010, kAttrLaunchMode = 0x0101001d,
                   kAttrScreenOrientation = 0x0101001e, kAttrConfigChanges = 0x0101001f, kAttrValue = 0x01010024,
                   kAttrMinSdkVersion = 0x0101020c, kAttrVersionCode = 0x0101021b, kAttrVersionName = 0x0101021c,
                   kAttrTargetSdkVersion = 0x01010270, kAttrGlEsVersion = 0x01010281, kAttrRequired = 0x0101028e,
                   kAttrIsGame = 0x010103f4, kAttrExtractNativeLibs = 0x010104ea;
constexpr uint32_t kThemeBlackNoTitleBarFullscreen = 0x0103000a;
constexpr uint32_t kIconResource = 0x7f010000;

// Launcher icon when the project has none: a rounded play button.
Bytes DefaultIcon() {
    const int size = 192;
    const float fsize = static_cast<float>(size);
    Image img;
    img.width = img.height = size;
    img.rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) / fsize, fy = (static_cast<float>(y) + 0.5f) / fsize;
            float dx = std::max(std::abs(fx - 0.5f) - 0.3f, 0.0f), dy = std::max(std::abs(fy - 0.5f) - 0.3f, 0.0f);
            if (dx * dx + dy * dy > 0.15f * 0.15f) continue;  // rounded square
            uint32_t r = static_cast<uint32_t>(40 + 40 * fy), g = static_cast<uint32_t>(90 + 50 * fy), b = static_cast<uint32_t>(200 + 40 * fy);
            // Play triangle.
            if (fx > 0.38f && fx < 0.7f && std::abs(fy - 0.5f) < (0.7f - fx) * 0.62f) r = g = b = 255;
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * size + static_cast<size_t>(x)) * 4];
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            p[3] = 255;
        }
    }
    return EncodePng(img, true);
}

}  // namespace

bool IsValidAndroidPackageName(const std::string& name) {
    int segments = 0;
    size_t start = 0;
    for (;;) {
        size_t end = name.find('.', start);
        std::string seg = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg.empty() || !std::isalpha(static_cast<unsigned char>(seg[0]))) return false;
        for (char c : seg) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        }
        ++segments;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return segments >= 2;
}

std::string DefaultAndroidPackageName(const std::string& projectName) {
    std::string id;
    for (char c : projectName) {
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (id.empty() || !std::isalpha(static_cast<unsigned char>(id[0]))) id = "game" + id;
    return "com.ownengine." + id;
}

std::vector<unsigned char> BuildAndroidManifest(const AndroidAppInfo& app) {
    // ActivityInfo.screenOrientation: sensorLandscape 6, sensorPortrait 7, unspecified -1.
    int orientation = app.orientation == "portrait" ? 7 : app.orientation == "auto" ? -1 : 6;
    // Handle rotation, resizing, keyboards etc. in the running game instead of
    // restarting the activity: keyboard|keyboardHidden|navigation|orientation|
    // screenLayout|uiMode|screenSize|smallestScreenSize|density|layoutDirection.
    const uint32_t configChanges = 0x3FF0;

    XmlElement activity{"activity",
                        {Str("name", kAttrName, "android.app.NativeActivity"), Bool("exported", kAttrExported, true),
                         Int("launchMode", kAttrLaunchMode, 2 /* singleTask */), Int("screenOrientation", kAttrScreenOrientation, orientation),
                         Hex("configChanges", kAttrConfigChanges, configChanges)},
                        {XmlElement{"meta-data", {Str("name", kAttrName, "android.app.lib_name"), Str("value", kAttrValue, "oe_player")}, {}},
                         // Wraps the glue's ANativeActivity_onCreate (immersive mode, platform/android).
                         XmlElement{"meta-data", {Str("name", kAttrName, "android.app.func_name"), Str("value", kAttrValue, "oe_ANativeActivity_onCreate")}, {}},
                         XmlElement{"intent-filter",
                                    {},
                                    {XmlElement{"action", {Str("name", kAttrName, "android.intent.action.MAIN")}, {}},
                                     XmlElement{"category", {Str("name", kAttrName, "android.intent.category.LAUNCHER")}, {}}}}}};
    XmlElement application{"application",
                           {Str("label", kAttrLabel, app.label), Ref("theme", kAttrTheme, kThemeBlackNoTitleBarFullscreen),
                            Bool("hasCode", kAttrHasCode, false), Bool("extractNativeLibs", kAttrExtractNativeLibs, false),
                            Bool("isGame", kAttrIsGame, true)},
                           {activity}};
    if (app.hasIcon) application.attrs.push_back(Ref("icon", kAttrIcon, kIconResource));
    if (app.debuggable) application.attrs.push_back(Bool("debuggable", kAttrDebuggable, true));
    XmlElement manifest{"manifest",
                        {Int("versionCode", kAttrVersionCode, app.versionCode), Str("versionName", kAttrVersionName, app.versionName),
                         Str("package", 0, app.packageName)},
                        {XmlElement{"uses-sdk", {Int("minSdkVersion", kAttrMinSdkVersion, app.minSdk), Int("targetSdkVersion", kAttrTargetSdkVersion, app.targetSdk)}, {}},
                         XmlElement{"uses-feature", {Hex("glEsVersion", kAttrGlEsVersion, 0x00030000), Bool("required", kAttrRequired, true)}, {}},
                         application}};
    return XmlEncoder().Encode(manifest);
}

std::vector<unsigned char> BuildAndroidResources(const std::string& packageName) {
    Bytes typeStrings = StringPool({"drawable"});
    Bytes keyStrings = StringPool({"icon"});

    Bytes spec;  // ResTable_typeSpec: one entry, no configuration variants
    Put16(spec, 0x0202);
    Put16(spec, 16);
    Put32(spec, 20);
    Put8(spec, 1);  // type id (1-based index into typeStrings)
    Put8(spec, 0);
    Put16(spec, 0);
    Put32(spec, 1);
    Put32(spec, 0);

    Bytes type;  // ResTable_type for the default configuration
    const uint32_t configSize = 64, header = 20 + configSize;
    Put16(type, 0x0201);
    Put16(type, header);
    Put32(type, header + 4 + 16);
    Put8(type, 1);
    Put8(type, 0);
    Put16(type, 0);
    Put32(type, 1);           // entryCount
    Put32(type, header + 4);  // entriesStart
    Put32(type, configSize);  // ResTable_config: size, everything else "any"
    type.insert(type.end(), configSize - 4, 0);
    Put32(type, 0);  // offset of entry 0
    Put16(type, 8);  // ResTable_entry: size, flags, key
    Put16(type, 0);
    Put32(type, 0);
    Put16(type, 8);  // Res_value: string 0 of the global pool
    Put8(type, 0);
    Put8(type, kTypeString);
    Put32(type, 0);

    Bytes pkg;
    const uint32_t pkgHeader = 288;
    Put16(pkg, 0x0200);
    Put16(pkg, pkgHeader);
    Put32(pkg, 0);
    Put32(pkg, 0x7f);
    std::u16string name = Utf16(packageName);
    for (size_t i = 0; i < 128; ++i) Put16(pkg, i < name.size() && i < 127 ? name[i] : 0);
    Put32(pkg, pkgHeader);  // typeStrings
    Put32(pkg, 1);          // lastPublicType
    Put32(pkg, pkgHeader + static_cast<uint32_t>(typeStrings.size()));  // keyStrings
    Put32(pkg, 1);          // lastPublicKey
    Put32(pkg, 0);          // typeIdOffset
    Append(pkg, typeStrings);
    Append(pkg, keyStrings);
    Append(pkg, spec);
    Append(pkg, type);
    Set32(pkg, 4, static_cast<uint32_t>(pkg.size()));

    Bytes out;
    Put16(out, 0x0002);  // RES_TABLE_TYPE
    Put16(out, 12);
    Put32(out, 0);
    Put32(out, 1);  // packages
    Append(out, StringPool({"res/drawable/icon.png"}));
    Append(out, pkg);
    Set32(out, 4, static_cast<uint32_t>(out.size()));
    return out;
}

bool WriteUnsignedApk(const ApkContents& contents, const std::string& outPath, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!IsValidAndroidPackageName(contents.app.packageName)) return fail("invalid Android package name '" + contents.app.packageName + "'");
    if (contents.nativeLibs.empty()) return fail("no native library (liboe_player.so) to package");
    AndroidAppInfo app = contents.app;
    app.hasIcon = true;
    const Bytes icon = contents.iconPng.empty() ? DefaultIcon() : contents.iconPng;

    ZipWriter zip;
    const Bytes manifest = BuildAndroidManifest(app);
    zip.AddStored("AndroidManifest.xml", manifest.data(), manifest.size());
    // Uncompressed and 4-byte aligned, as Android 11+ requires (targetSdk >= 30).
    const Bytes resources = BuildAndroidResources(app.packageName);
    zip.AddStored("resources.arsc", resources.data(), resources.size());
    zip.AddStored("res/drawable/icon.png", icon.data(), icon.size());
    for (const auto& lib : contents.nativeLibs) {
        Bytes so;
        if (!ReadBinaryFile(lib.second, so)) return fail("cannot read " + lib.second);
        // extractNativeLibs=false: loaded straight from the APK, so stored and
        // aligned to 16 KB pages (works on 4 KB and 16 KB page devices).
        zip.AddStored("lib/" + lib.first + "/liboe_player.so", so.data(), so.size(), 16384);
    }
    // game.id changes whenever the data changes: the player unpacks game.pak again.
    const uint64_t hash = Fnv1a64(contents.gamePak.data(), contents.gamePak.size());
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(hash));
    zip.AddStored("assets/game.id", reinterpret_cast<const unsigned char*>(id), 16);
    zip.AddStored("assets/game.pak", contents.gamePak.data(), contents.gamePak.size());

    const Bytes& apk = zip.Finish();
    CreateDirectories(ParentPath(outPath));
    FILE* f = std::fopen(outPath.c_str(), "wb");
    bool ok = f && std::fwrite(apk.data(), 1, apk.size(), f) == apk.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    if (!ok) return fail("cannot write " + outPath);
    return true;
}

}  // namespace oe
