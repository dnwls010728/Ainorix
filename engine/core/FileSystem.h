#pragma once
#include <string>
#include <vector>

namespace oe {

bool ReadTextFile(const std::string& path, std::string& out);
bool WriteTextFile(const std::string& path, const std::string& text);
bool ReadBinaryFile(const std::string& path, std::vector<unsigned char>& out);
bool FileExists(const std::string& path);
bool IsDirectory(const std::string& path);
bool CreateDirectories(const std::string& path);
std::string JoinPath(const std::string& a, const std::string& b);
std::string ParentPath(const std::string& path);
std::string AbsolutePath(const std::string& path);
// Relative to `base` when possible, always with forward slashes.
std::string RelativePath(const std::string& path, const std::string& base);
std::vector<std::string> ListFiles(const std::string& dir, const std::string& extension, bool recursive);

}  // namespace oe
