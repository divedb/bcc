#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bcc/basic/file_id.hh"
#include "bcc/basic/line_table.hh"
#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/sloc_entry.hh"
#include "bcc/basic/source_location.hh"
#include "bcc/basic/source_range.hh"

namespace bcc {

class FileEntry;
class FileManager;

class SourceManager {
 public:
  /// \brief Constructs a SourceManager with the given FileManager.
  ///
  /// \param fm The FileManager to use for file access. The SourceManager does
  ///           not take ownership of the FileManager, which must outlive the
  ///           SourceManager.
  explicit SourceManager(FileManager& fm);

  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;
  ~SourceManager() = default;

  /// \brief Loads the given FileEntry and registers it in the SourceManager.
  ///
  /// \param entry       The FileEntry to load.
  /// \param include_loc The location of the directive that brought this file
  ///                    into the TU. Invalid for the main file, valid for
  ///                    #include directives.
  /// \return            A FileID representing the registered file, or an
  ///                    invalid FileID on failure.
  FileID CreateFileID(const FileEntry& entry,
                      SourceLocation include_loc = SourceLocation{});

  /// \brief Creates a new FileID for the given in-memory content.
  ///
  /// \param identifier  A string to identify the content, e.g. a filename or
  ///                    other user-friendly description. This is used in
  ///                    diagnostics and may be empty if no identifier is
  ///                    available.
  /// \param content     The content to register. This will be copied into a
  ///                    MemoryBuffer owned by the SourceManager.
  /// \param include_loc The location of the directive that brought this
  ///                    content into the TU, or an invalid SourceLocation if
  ///                    this is the main file.
  /// \return            A FileID representing the registered content, or an
  ///                    invalid FileID on failure.
  FileID CreateFileID(std::string identifier, std::string content,
                      SourceLocation include_loc = SourceLocation{});

  /// \brief Allocates a synthetic slice in the global offset space for one
  ///        macro expansion and registers an ExpansionInfo SLocEntry.
  ///
  /// The caller uses GetLocForOffset on the returned FileID to produce
  /// individual token SourceLocations for the expanded tokens.
  ///
  /// \param spelling_loc    Location of the macro body text in its defining
  ///                        file.
  /// \param expansion_start Location of the first character of the macro
  ///                        invocation in the caller. May itself be a macro
  ///                        location for nested expansions.
  /// \param expansion_end   One-past-end of the macro invocation in the caller.
  /// \param token_length    Number of offset slots to reserve. Pass the number
  ///                        of tokens the expansion produces; minimum 1.
  /// \return                FileID of the new ExpansionInfo SLocEntry.
  FileID CreateExpansionLoc(SourceLocation spelling_loc,
                            SourceLocation expansion_start,
                            SourceLocation expansion_end,
                            uint32_t token_length);

  /// \brief Returns the FileID that owns \p loc.
  ///
  /// \return The FileID of the SLocEntry that contains \p loc, or an invalid
  ///         FileID if \p loc is invalid.
  FileID GetFileID(SourceLocation loc) const noexcept;

  /// \return The FileID of the main translation-unit file.
  FileID GetMainFileID() const noexcept { return main_file_id_; }

  /// \brief Sets the FileID of the main translation-unit file.
  ///
  /// \param fid The FileID to set as the main file. Must be valid and
  ///            correspond to a file SLocEntry.
  void SetMainFileID(FileID fid) noexcept { main_file_id_ = fid; }

  /// \brief Returns the 1-based line number of \p loc.
  ///
  /// \p loc must already be a file location. Use GetSpellingLoc() or
  /// GetExpansionLoc() to resolve macro locations before calling this.
  unsigned GetLine(SourceLocation loc) const;

  /// \brief Returns the 1-based column number of \p loc.
  ///
  /// \p loc must already be a file location. Use GetSpellingLoc() or
  /// GetExpansionLoc() to resolve macro locations before calling this.
  unsigned GetColumn(SourceLocation loc) const;

  /// \brief Returns a SourceLocation for the first byte of \p fid's buffer.
  SourceLocation GetLocForStartOfFile(FileID fid) const noexcept;

  /// \brief Returns a SourceLocation one past the last byte of \p fid's buffer.
  SourceLocation GetLocForEndOfFile(FileID fid) const noexcept;

  /// \brief Returns the SourceLocation for byte \p offset within \p fid's
  ///        buffer.
  SourceLocation GetLocForOffset(FileID fid, uint32_t offset) const noexcept;

  /// \brief Converts a 1-based (line, col) pair within \p fid to a
  ///        SourceLocation.
  ///
  /// \return An invalid SourceLocation if the (line, col) pair is out of range.
  SourceLocation TranslateLineCol(FileID fid, unsigned line,
                                  unsigned col) const;

  /// \brief Returns {FileID, byte-offset} for \p loc without resolving macros.
  std::pair<FileID, uint32_t> GetDecomposedLoc(
      SourceLocation loc) const noexcept;

  /// \brief Like GetDecomposedLoc but first resolves \p loc to its spelling
  ///        location.
  std::pair<FileID, uint32_t> GetDecomposedSpellingLoc(
      SourceLocation loc) const noexcept;

  /// \brief Like GetDecomposedLoc but first resolves \p loc to its expansion
  ///        location.
  std::pair<FileID, uint32_t> GetDecomposedExpansionLoc(
      SourceLocation loc) const noexcept;

  /// \brief Returns the byte offset of \p loc within its SLocEntry's buffer.
  uint32_t GetFileOffset(SourceLocation loc) const noexcept;

  /// \brief Returns the location of the macro body text for a macro location.
  ///
  /// Follows spelling_loc links until a file location is reached. Equivalent
  /// to Clang's getSpellingLoc. Returns \p loc unchanged for file locations.
  SourceLocation GetSpellingLoc(SourceLocation loc) const noexcept;

  /// \brief Returns the start of the macro invocation in the caller's file.
  ///
  /// Follows expansion_start links until a file location is reached.
  /// Returns \p loc unchanged for file locations.
  SourceLocation GetExpansionLoc(SourceLocation loc) const noexcept;

  /// \brief Returns {expansion_start, expansion_end} for a macro location.
  ///
  /// Both ends may be macro locations for nested expansions.
  /// Returns {loc, loc} for file locations.
  std::pair<SourceLocation, SourceLocation> GetImmediateExpansionRange(
      SourceLocation loc) const noexcept;

  /// \brief Returns the SourceRange covering the macro invocation in the
  ///        caller's file.
  ///
  /// Returns a zero-length range at \p loc for file locations.
  SourceRange GetExpansionRange(SourceLocation loc) const noexcept;

  /// \brief Returns the presumed location of \p loc.
  ///
  /// The presumed location reflects any \c #line directives in effect.
  ///
  /// \param loc                 The location to resolve.
  /// \param use_line_directives If false, ignore \c #line directives and return
  ///                            the physical spelling location instead.
  /// \return                    The presumed PresumedLoc, or an invalid one if
  ///                            \p loc is invalid.
  PresumedLoc GetPresumedLoc(SourceLocation loc,
                             bool use_line_directives = true) const;

  /// \brief Returns a view of the raw buffer contents for \p fid.
  std::string_view GetBufferData(FileID fid) const noexcept;

  /// \brief Returns the identifier (filename or logical name) for \p fid.
  std::string_view GetFilename(FileID fid) const noexcept;

  /// \brief Returns a pointer to the character at \p loc.
  ///
  /// \note Macro expansion locations are resolved to their spelling location
  ///       first.
  const char* GetCharacterData(SourceLocation loc) const noexcept;

  /// \brief Returns the source text of \p range.
  ///
  /// \pre Both ends of \p range must share the same FileID.
  std::string_view GetSourceText(SourceRange range) const;

  /// \brief Returns the location of the directive that brought \p fid into the
  ///        translation unit.
  ///
  /// \return An invalid SourceLocation for the main file and for CreateFileID()
  ///         buffers that were not given an explicit include_loc.
  SourceLocation GetIncludeLoc(FileID fid) const noexcept;

  /// \brief Returns true if \p loc belongs to \p fid.
  ///
  /// \param loc        Location to test.
  /// \param fid        FileID to test against.
  /// \param rel_offset If non-null and the result is true, receives the byte
  ///                   offset of \p loc within \p fid's buffer.
  bool IsInFileID(SourceLocation loc, FileID fid,
                  uint32_t* rel_offset = nullptr) const noexcept;

  /// \brief Returns true if the spelling location of \p loc is in the main
  ///        file.
  bool IsInMainFile(SourceLocation loc) const noexcept;

  /// \brief Returns true if \p loc itself (without following macro spelling) is
  ///        in the main file.
  bool IsWrittenInMainFile(SourceLocation loc) const noexcept;

 private:
  const SLocEntry& GetSLocEntry(FileID fid) const noexcept;
  const ContentCache& GetContentCache(FileID fid) const noexcept;
  void EnsureLinesCoveredTo(const ContentCache& cache, uint32_t offset) const;
  void EnsureLineCount(const ContentCache& cache, unsigned n) const;

  /// \brief Registers a new SLocEntry for the given FileEntry and MemoryBuffer.
  ///
  /// \param entry       The FileEntry to register, or nullptr for in-memory
  ///                    buffers.
  /// \param buf         The MemoryBuffer containing the file's contents.
  /// \param include_loc The location of the directive that brought this file
  ///                    into the TU, or an invalid SourceLocation for the main
  ///                    file.
  /// \return            The FileID of the newly registered SLocEntry.
  FileID RegisterBuffer(const FileEntry* entry,
                        std::unique_ptr<MemoryBuffer> buf,
                        SourceLocation include_loc);

  /// \brief Scans the buffer in \p cache for newlines up to \p scan_end and
  ///        updates the line offsets.
  ///
  /// \param cache    The ContentCache containing the buffer to scan and the
  ///                 line offsets to update.
  /// \param scan_end The offset in the buffer up to which to scan for newlines.
  void ScanLines(const ContentCache& cache, uint32_t scan_end) const;

  /// \brief Returns the 1-based line number for \p offset within \p cache.
  ///
  /// \param cache  The ContentCache containing the line offsets.
  /// \param offset The byte offset within the buffer.
  /// \return       The 1-based line number.
  unsigned ComputeLineNumber(const ContentCache& cache, uint32_t offset) const;

  /// \brief Returns the 1-based column number for \p offset within \p cache.
  ///
  /// \param cache  The ContentCache containing the line offsets.
  /// \param offset The byte offset within the buffer.
  /// \return       The 1-based column number.
  unsigned ComputeColumnNumber(const ContentCache& cache,
                               uint32_t offset) const;

  /// The next free position in the global offset space.
  /// Starts at 1 because 0 is reserved as the invalid sentinel.
  uint32_t next_offset_ = 1;

  /// Single-entry cache for GetFileID to avoid repeated binary searches on
  /// sequential access within the same file.
  mutable int last_file_id_ = -1;

  FileID main_file_id_;
  FileManager& file_manager_;

  /// Maps FileEntry pointers to their corresponding FileID.
  std::unordered_map<const FileEntry*, FileID> file_to_id_;

  /// The line table for #line directive overrides.
  LineTableInfo line_table_;

  /// The SLocEntry table, sorted by ascending offset. Each entry represents
  /// either a file or a macro expansion. The offset field gives the starting
  /// offset of the entry in the global offset space. The entries are
  /// non-overlapping and contiguous, so the entry for a given SourceLocation
  /// can be found by binary searching for the last entry whose offset is <= the
  /// location's offset.
  std::vector<SLocEntry> sloc_entries_;
};

}  // namespace bcc
