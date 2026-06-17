#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/file_id.hh"

namespace bcc {

/// \brief Records the effect of a single \c #line directive.
///
/// When the preprocessor sees:
///   \code
///   #line 42 "generated.c"
///   \endcode
/// it inserts a LineEntry into LineTableInfo for the enclosing FileID.
/// SourceManager::GetPresumedLoc uses these entries to map a physical
/// (file, offset) pair to the filename and line number that a user or
/// debugger should see, rather than the actual on-disk position.
struct LineEntry {
  /// Byte offset in the owning file's buffer where this directive takes
  /// effect. Entries within a file are stored sorted by this field so
  /// FindEntry can binary-search them.
  uint32_t file_offset;

  /// The line number that the directive asserts for the next source line.
  /// Lines after the directive are presumed_line = line_num + (phys_line -
  /// entry_phys_line).
  unsigned line_num;

  /// Index into LineTableInfo::filenames_ for the filename override, or -1
  /// if the directive did not specify a filename (keep the current one).
  int filename_id;
};

/// \brief Stores \c #line directive overrides for every FileID.
///
/// \c #line (and the equivalent \c # \c lineno \c "file" form emitted by
/// tools like \c bison and \c flex) lets a generated file claim that its
/// content "came from" a different file and line. LineTableInfo collects
/// those overrides and lets SourceManager::GetPresumedLoc substitute them
/// when producing user-visible locations.
///
/// Filenames are interned: GetOrCreateFilenameID deduplicates strings and
/// returns a stable integer ID that LineEntry::filename_id references.
class LineTableInfo {
 public:
  /// Returns true if any \c #line entries have been registered for \p fid.
  bool HasEntries(FileID fid) const noexcept;

  /// Returns the last LineEntry whose \c file_offset <= \p offset within
  /// \p fid, or nullptr if no directive precedes that offset.
  const LineEntry* FindEntry(FileID fid, uint32_t offset) const noexcept;

  /// Returns the interned filename string for \p filename_id.
  /// \p filename_id must be a value previously returned by
  /// GetOrCreateFilenameID.
  std::string_view GetFilename(int filename_id) const noexcept;

  /// Appends a LineEntry for \p fid. Entries must be added in increasing
  /// \c file_offset order within each file (the preprocessor visits
  /// directives top-to-bottom).
  void AddEntry(FileID fid, LineEntry entry);

  /// Interns \p filename and returns a stable integer ID for it. If the
  /// string was already interned the existing ID is returned.
  int GetOrCreateFilenameID(std::string filename);

 private:
  /// Per-file list of LineEntry values, sorted by file_offset.
  std::map<FileID, std::vector<LineEntry>> entries_;

  /// Interned filename strings indexed by their ID.
  std::vector<std::string> filenames_;
};

}  // namespace bcc
