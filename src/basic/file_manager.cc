#include "bcc/basic/file_manager.hh"

#include <sys/stat.h>

#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace bcc {

// https://en.cppreference.com/cpp/filesystem/path/lexically_normal
std::string FileManager::ResolvePath(std::string_view path) const {
  fs::path p(path);

  // Note: The path "/" is absolute on a POSIX OS, but is relative on Windows.
  if (p.is_relative() && !opts_.working_dir.empty()) {
    p = fs::path(opts_.working_dir) / p;
  }

  // Note: These conversions are purely lexical. They do not check that the
  // paths exist, do not follow symlinks, and do not access the filesystem at
  // all.
  return p.lexically_normal().string();
}

const FileEntry* FileManager::GetFile(std::string_view path) {
  std::string resolved = ResolvePath(path);
  auto [it, inserted] = path_cache_.emplace(resolved, nullptr);

  if (!inserted) return it->second;

  struct ::stat st{};

  if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return nullptr;
  }

  std::pair key{st.st_dev, st.st_ino};
  auto& entry = inode_to_file_[key];

  if (!entry) {
    entry.reset(new FileEntry(resolved, st.st_size, st.st_mtime, next_uid_++));
  }

  it->second = entry.get();

  return it->second;
}

const FileEntry* FileManager::GetStdin() {
  if (!stdin_entry_) {
    stdin_entry_.reset(new FileEntry("<stdin>", 0, 0, next_uid_++));
  }

  return stdin_entry_.get();
}

std::unique_ptr<MemoryBuffer> FileManager::GetBufferForFile(
    const FileEntry& entry) {
  if (&entry == stdin_entry_.get()) return MemoryBuffer::GetStdin();

  return MemoryBuffer::GetFile(entry.GetName());
}

}  // namespace bcc
