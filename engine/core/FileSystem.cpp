#include "core/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "core/Pak.h"
#include "platform/Platform.h"

namespace fs = std::filesystem;

namespace oe {

namespace {
fs::path U8Path(const std::string& s) { return fs::u8path(s); }
std::string ToU8(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

struct Mount {
    std::string root;  // absolute, forward slashes, no trailing slash
    PakReader pak;
};
std::vector<std::unique_ptr<Mount>>& Mounts() {
    static std::vector<std::unique_ptr<Mount>> mounts;
    return mounts;
}

// The mount a path lies in and the path inside it ("" for the root itself); nullptr for the disk.
const Mount* FindMount(const std::string& path, std::string* inside) {
    if (Mounts().empty()) return nullptr;
    std::error_code ec;
    const std::string full = ToU8(fs::absolute(U8Path(path), ec).lexically_normal());
    for (const auto& mount : Mounts()) {
        const std::string& root = mount->root;
        if (full.size() < root.size() || full.compare(0, root.size(), root) != 0) continue;
        if (full.size() > root.size() && full[root.size()] != '/') continue;
        *inside = full.size() > root.size() ? full.substr(root.size() + 1) : std::string();
        while (!inside->empty() && inside->back() == '/') inside->pop_back();
        return mount.get();
    }
    return nullptr;
}

bool AddMount(const std::string& root, std::unique_ptr<Mount> mount) {
    std::error_code ec;
    mount->root = ToU8(fs::absolute(U8Path(root), ec).lexically_normal());
    while (mount->root.size() > 1 && mount->root.back() == '/') mount->root.pop_back();
    Mounts().push_back(std::move(mount));
    return true;
}
}  // namespace

bool MountPakFile(const std::string& root, const std::string& pakPath, std::string* error, const unsigned char* key) {
    auto mount = std::make_unique<Mount>();
    return mount->pak.OpenFile(pakPath, error, key) && AddMount(root, std::move(mount));
}

bool MountPakMemory(const std::string& root, std::vector<unsigned char> pak, std::string* error, const unsigned char* key) {
    auto mount = std::make_unique<Mount>();
    return mount->pak.OpenMemory(std::move(pak), error, key) && AddMount(root, std::move(mount));
}

void UnmountPaks() { Mounts().clear(); }

bool ReadTextFile(const std::string& path, std::string& out) {
    std::string inside;
    if (const Mount* mount = FindMount(path, &inside)) {
        std::vector<unsigned char> bytes;
        if (!mount->pak.Read(inside, bytes)) return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    }
    std::ifstream f(U8Path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    std::string inside;
    if (FindMount(path, &inside)) return false;  // packaged game data is read-only
    std::error_code ec;
    fs::path p = U8Path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

bool WriteTextFileAtomic(const std::string& path, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::path destination = U8Path(path);
    if (destination.has_parent_path()) fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create save directory: " + ec.message();
        return false;
    }
    const std::string temporary = path + ".tmp";
    std::ofstream file(U8Path(temporary), std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    bool written = static_cast<bool>(file);
    file.close();
    if (!written || file.fail()) {
        fs::remove(U8Path(temporary), ec);
        if (error) *error = "cannot write temporary save file " + temporary;
        return false;
    }
    if (!PlatformReplaceFile(temporary, path, error)) {
        fs::remove(U8Path(temporary), ec);
        return false;
    }
    return true;
}

bool ReadBinaryFile(const std::string& path, std::vector<unsigned char>& out) {
    std::string inside;
    if (const Mount* mount = FindMount(path, &inside)) return mount->pak.Read(inside, out);
    std::ifstream f(U8Path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool FileExists(const std::string& path) {
    std::string inside;
    if (const Mount* mount = FindMount(path, &inside)) return mount->pak.Find(inside) != nullptr || IsDirectory(path);
    std::error_code ec;
    return fs::exists(U8Path(path), ec);
}

int64_t FileModifiedTime(const std::string& path) {
    std::string inside;
    if (const Mount* mount = FindMount(path, &inside)) return mount->pak.Find(inside) ? 1 : 0;  // never changes
    std::error_code ec;
    auto t = fs::last_write_time(U8Path(path), ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

bool IsDirectory(const std::string& path) {
    std::string inside;
    if (const Mount* mount = FindMount(path, &inside)) {
        if (inside.empty()) return true;
        const std::string prefix = inside + "/";
        for (const std::string& file : mount->pak.Paths())
            if (file.size() > prefix.size() && file.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    }
    std::error_code ec;
    return fs::is_directory(U8Path(path), ec);
}

bool CreateDirectories(const std::string& path) {
    std::error_code ec;
    fs::create_directories(U8Path(path), ec);
    return !ec;
}

bool CopyFileTo(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::path dst = U8Path(to);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    return fs::copy_file(U8Path(from), dst, fs::copy_options::overwrite_existing, ec) && !ec;
}

bool RemoveAll(const std::string& path) {
    std::error_code ec;
    fs::remove_all(U8Path(path), ec);
    return !ec;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return ToU8(U8Path(a) / U8Path(b));
}

std::string ParentPath(const std::string& path) { return ToU8(U8Path(path).parent_path()); }

std::string AbsolutePath(const std::string& path) {
    std::error_code ec;
    // Node's NODERAWFS exposes Windows drive paths to a POSIX libc++ build.
    // Those are already absolute; fs::absolute would prepend the working directory.
    if (path.size() > 2 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) return ToU8(U8Path(path).lexically_normal());
    fs::path p = fs::absolute(U8Path(path), ec);
    return ToU8(p.lexically_normal());
}

std::string RelativePath(const std::string& path, const std::string& base) {
    std::error_code ec;
    fs::path rel = fs::relative(U8Path(path), U8Path(base), ec);
    if (ec || rel.empty()) rel = U8Path(path).lexically_normal().lexically_relative(U8Path(base).lexically_normal());
    if (rel.empty()) return ToU8(U8Path(path));
    return ToU8(rel);
}

std::vector<std::string> ListFiles(const std::string& dir, const std::string& extension, bool recursive) {
    std::vector<std::string> out;
    std::string inside;
    if (const Mount* mount = FindMount(dir, &inside)) {
        const std::string prefix = inside.empty() ? std::string() : inside + "/";
        for (const std::string& file : mount->pak.Paths()) {
            if (file.size() <= prefix.size() || file.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = file.substr(prefix.size());
            if (!recursive && rest.find('/') != std::string::npos) continue;
            if (!extension.empty() && (rest.size() < extension.size() || rest.compare(rest.size() - extension.size(), extension.size(), extension) != 0)) continue;
            out.push_back(JoinPath(dir, rest));
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file()) return;
        std::string p = ToU8(e.path());
        if (extension.empty() || (p.size() >= extension.size() && p.compare(p.size() - extension.size(), extension.size(), extension) == 0)) {
            out.push_back(p);
        }
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(U8Path(dir), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) consider(*it);
    } else {
        for (auto it = fs::directory_iterator(U8Path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) consider(*it);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ListDirectories(const std::string& dir) {
    std::vector<std::string> out;
    std::string inside;
    if (const Mount* mount = FindMount(dir, &inside)) {
        const std::string prefix = inside.empty() ? std::string() : inside + "/";
        std::set<std::string> names;
        for (const std::string& file : mount->pak.Paths()) {
            if (file.size() <= prefix.size() || file.compare(0, prefix.size(), prefix) != 0) continue;
            const size_t slash = file.find('/', prefix.size());
            if (slash != std::string::npos) names.insert(file.substr(prefix.size(), slash - prefix.size()));
        }
        out.assign(names.begin(), names.end());
        return out;
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(U8Path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) out.push_back(ToU8(it->path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace oe
