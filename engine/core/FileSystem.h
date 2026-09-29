#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

bool ReadTextFile(const std::string& path, std::string& out);
bool WriteTextFile(const std::string& path, const std::string& text);
bool ReadBinaryFile(const std::string& path, std::vector<unsigned char>& out);
bool FileExists(const std::string& path);
// Last write time as an opaque tick count (0 if the file does not exist).
int64_t FileModifiedTime(const std::string& path);
bool IsDirectory(const std::string& path);
bool CreateDirectories(const std::string& path);
// Copies a file, creating the destination's parent directories.
bool CopyFileTo(const std::string& from, const std::string& to);
// Deletes a file or a directory tree. Returns false if something could not be removed.
bool RemoveAll(const std::string& path);
std::string JoinPath(const std::string& a, const std::string& b);
std::string ParentPath(const std::string& path);
std::string AbsolutePath(const std::string& path);
// Relative to `base` when possible, always with forward slashes.
std::string RelativePath(const std::string& path, const std::string& base);
std::vector<std::string> ListFiles(const std::string& dir, const std::string& extension, bool recursive);

}  // namespace oe
