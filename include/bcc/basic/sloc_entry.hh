#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "bcc/basic/source_location.hh"

namespace bcc {

class FileEntry;
class MemoryBuffer;

/// \brief Represents the cached content and line offsets for a file registered
///        in SourceManager.
struct ContentCache {
  const FileEntry* entry;
  std::unique_ptr<MemoryBuffer> buffer;
  mutable std::vector<uint32_t> line_offsets;
  mutable uint32_t scanned_offset = 0;
};

/// \brief One entry in the SourceManager's SLocEntry table.
struct FileInfo {
  std::unique_ptr<ContentCache> cache;
  SourceLocation include_loc;
};

/// \brief Represents the information about a macro expansion.
struct ExpansionInfo {
  /// Location of the macro body text in its defining file.
  SourceLocation spelling_loc;

  /// First character of the macro invocation in the caller's file.
  SourceLocation expansion_start;

  /// One-past-end of the macro invocation in the caller's file.
  SourceLocation expansion_end;
};

/// \brief Represents a single entry in the SourceManager's SLocEntry table,
///        which can be either a file or a macro expansion.
struct SLocEntry {
  uint32_t offset;
  std::variant<FileInfo, ExpansionInfo> info;

  /// \brief Returns true if this SLocEntry represents a file.
  ///
  /// \return True if this SLocEntry represents a file; otherwise false.
  bool IsFile() const noexcept {
    return std::holds_alternative<FileInfo>(info);
  }

  /// \brief Returns true if this SLocEntry represents a macro expansion.
  ///
  /// \return True if this SLocEntry represents a macro expansion; otherwise
  ///         false.
  bool IsExpansion() const noexcept {
    return std::holds_alternative<ExpansionInfo>(info);
  }

  const FileInfo& GetFileInfo() const {
    assert(IsFile() && "GetFileInfo() called on a non-file SLocEntry");

    return std::get<FileInfo>(info);
  }

  const ExpansionInfo& GetExpansionInfo() const {
    assert(IsExpansion() &&
           "GetExpansionInfo() called on a non-expansion SLocEntry");

    return std::get<ExpansionInfo>(info);
  }
};

}  // namespace bcc
