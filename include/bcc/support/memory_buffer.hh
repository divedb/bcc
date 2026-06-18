#pragma once

#include <string>
#include <memory>

namespace bcc {

/// \brief A immutable view over a contiguous memory region.
///
/// MemoryBuffer represents a read-only byte sequence backed by either:
/// - a file mapping
/// - an in-memory copy
/// - stdin input
class MemoryBuffer {
 public:
  MemoryBuffer(const MemoryBuffer&) = delete;
  MemoryBuffer& operator=(const MemoryBuffer&) = delete;
  virtual ~MemoryBuffer() = default;

  const char* GetBufferStart() const noexcept { return buffer_start_; }
  const char* GetBufferEnd() const noexcept { return buffer_end_; }

  size_t GetBufferSize() const noexcept {
    return static_cast<size_t>(buffer_end_ - buffer_start_);
  }

  std::string_view GetBuffer() const noexcept {
    return {buffer_start_, GetBufferSize()};
  }

  /// \brief Returns an identifier associated with this buffer.
  ///
  /// The identifier is typically:
  /// - file path for file-based buffers
  /// - "<stdin>" for standard input
  /// - custom tag for in-memory buffers
  ///
  /// It is used for diagnostics and SourceLocation printing.
  ///
  /// \return An identifier associated with this buffer.
  const std::string& GetBufferIdentifier() const noexcept {
    return identifier_;
  }

  /// \brief Creates a MemoryBuffer by copying the given string content.
  ///
  /// \param content    Source content to copy into the buffer.
  /// \param identifier Logical name associated with this buffer.
  /// \return           A unique_ptr owning the newly created MemoryBuffer
  ///                   instance, or nullptr on failure.
  static std::unique_ptr<MemoryBuffer> GetMemoryBufferCopy(
      std::string content, std::string identifier = "");

  /// \brief Creates a MemoryBuffer by loading a file from disk.
  ///
  /// The file is opened using the underlying filesystem layer, and its
  /// contents are mapped or copied into memory depending on implementation.
  ///
  /// \param path Path to the file. May be relative or absolute.
  /// \return     A unique_ptr owning the MemoryBuffer, or nullptr on failure.
  static std::unique_ptr<MemoryBuffer> GetFile(std::string_view path);

  /// \brief Creates a MemoryBuffer from standard input (stdin).
  ///
  /// This function reads all available data from stdin until EOF and
  /// stores it into an in-memory buffer.
  ///
  /// The resulting buffer typically uses "<stdin>" as its identifier.
  ///
  /// \return A unique_ptr owning the MemoryBuffer containing stdin data.
  static std::unique_ptr<MemoryBuffer> GetStdin();

 protected:
  MemoryBuffer() noexcept = default;

  void Init(const char* start, const char* end) noexcept {
    buffer_start_ = start;
    buffer_end_ = end;
  }

  std::string identifier_;

 private:
  const char* buffer_start_ = nullptr;
  const char* buffer_end_ = nullptr;
};

}  // namespace bcc