#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

bool ReadTextFile(const std::string& path, std::string& out);
bool WriteTextFile(const std::string& path, const std::string& text);
// Writes a sibling temporary file, then replaces the destination atomically.
bool WriteTextFileAtomic(const std::string& path, const std::string& text, std::string* error);
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
// Names (not paths) of the folders directly inside `dir`, sorted.
std::vector<std::string> ListDirectories(const std::string& dir);

// Packaged games: the files of a pak (core/Pak.h) appear below `root`, a folder that need not exist.
// ReadTextFile, ReadBinaryFile, FileExists, FileModifiedTime, IsDirectory, ListFiles and ListDirectories
// answer from the pak for paths below root and never look at the disk there, so loose files cannot
// replace packaged ones. Nothing can be written below root. Mount before other threads read files.
// `key` is the master secret the pak was built with (nullptr = the engine seed; see core/Pak.h).
bool MountPakFile(const std::string& root, const std::string& pakPath, std::string* error, const unsigned char* key = nullptr);
bool MountPakMemory(const std::string& root, std::vector<unsigned char> pak, std::string* error, const unsigned char* key = nullptr);
void UnmountPaks();

}  // namespace oe
