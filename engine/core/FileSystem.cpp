#include "core/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace oe {

namespace {
fs::path U8Path(const std::string& s) { return fs::u8path(s); }
std::string ToU8(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
}  // namespace

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream f(U8Path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::path p = U8Path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

bool ReadBinaryFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(U8Path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(U8Path(path), ec);
}

int64_t FileModifiedTime(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(U8Path(path), ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

bool IsDirectory(const std::string& path) {
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
    std::error_code ec;
    for (auto it = fs::directory_iterator(U8Path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) out.push_back(ToU8(it->path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace oe
