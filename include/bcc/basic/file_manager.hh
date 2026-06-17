#pragma once

#include <map>
#include <string>
#include <unordered_map>

#include "bcc/support/memory_buffer.hh"

namespace bcc {

/// \brief Represents a file in the filesystem.
class FileEntry {
 public:
  const std::string& GetName() const noexcept { return name_; }

 private:
  friend class FileManager;

  FileEntry(std::string name, off_t size, time_t mtime, unsigned uid)
      : name_(std::move(name)), size_(size), mtime_(mtime), uid_(uid) {}

  std::string name_;
  off_t size_;
  time_t mtime_;
  unsigned uid_;
};

struct FileSystemOptions {
  /// Optional working directory for resolving relative paths.
  std::string working_dir;
};

/// \brief The FileManager is responsible for resolving file paths, caching file
///        metadata, and providing MemoryBuffer instances for file contents.
class FileManager {
 public:
  /// \brief Constructs a FileManager with the specified options.
  ///
  /// \param opts Configuration options for the FileManager, such as working
  ///             directory for path resolution.
  explicit FileManager(FileSystemOptions opts = {}) : opts_(std::move(opts)) {}

  FileManager(const FileManager&) = delete;
  FileManager& operator=(const FileManager&) = delete;

  ~FileManager() = default;

  /// \brief Wraps standard input (stdin) in a FileEntry and returns it.
  ///
  /// \note The returned pointer is owned by the FileManager and remains valid
  ///       for the FileManager's lifetime.
  ///
  /// \return The FileEntry representing standard input, or nullptr if stdin is
  ///         not available.
  const FileEntry* GetStdin();

  /// \brief Returns the FileEntry for the specified path.
  ///
  /// \note The returned pointer is owned by the FileManager and remains valid
  ///       for the FileManager's lifetime.
  ///
  /// \return The FileEntry for the given path, or nullptr if the file does not
  ///         exist or is not a regular file.
  const FileEntry* GetFile(std::string_view path);

  /// \brief Returns a MemoryBuffer for the specified FileEntry.
  ///
  /// \return A unique_ptr to the MemoryBuffer containing the file's contents.
  std::unique_ptr<MemoryBuffer> GetBufferForFile(const FileEntry& entry);

  /// \brief Returns the number of unique files currently managed by this
  ///        FileManager.
  ///
  /// \return The number of unique files currently managed by this FileManager.
  unsigned GetNumUniqueFiles() const noexcept {
    return static_cast<unsigned>(inode_to_file_.size());
  }

 private:
  /// \brief Resolves the given path to an absolute, normalized form based on
  ///        the FileManager's working directory and filesystem semantics.
  ///
  /// \param path The input file path, which may be relative or absolute.
  /// \return     The resolved absolute path, normalized to remove redundant
  ///             components.
  std::string ResolvePath(std::string_view path) const;

  FileSystemOptions opts_;

  /// Unique identifier generator for files, used to assign a distinct UID to
  /// each file for deduplication purposes.
  unsigned next_uid_ = 0;

  std::unique_ptr<FileEntry> stdin_entry_;

  /// Map from resolved file paths to their corresponding FileEntry pointers.
  std::unordered_map<std::string, FileEntry*> path_cache_;

  /// Map from (device ID, inode number) pairs to unique FileEntry instances.
  std::map<std::pair<dev_t, ino_t>, std::unique_ptr<FileEntry>> inode_to_file_;
};

}  // namespace bcc
