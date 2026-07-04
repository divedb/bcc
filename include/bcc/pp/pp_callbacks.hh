#pragma once

#include <string_view>

#include "bcc/basic/characteristic_kind.hh"
#include "bcc/basic/file_id.hh"
#include "bcc/basic/source_location.hh"

namespace bcc {

class FileEntry;
class IdentifierInfo;
class MacroInfo;
class Token;

/// \brief Observer interface for preprocessor events.
///
/// A client (dependency scanner, -E printer, tooling, or a test) subclasses
/// PPCallbacks and overrides the events it cares about; every method defaults
/// to a no-op. The Preprocessor owns at most one PPCallbacks and invokes it as
/// it enters files, defines macros, expands macros, and evaluates conditionals.
///
/// This mirrors Clang's PPCallbacks at a smaller scale.
class PPCallbacks {
 public:
  virtual ~PPCallbacks() = default;

  /// Why the current file changed (see FileChanged).
  enum class FileChangeReason {
    kEnterFile,  ///< A new file was entered (main file or an #include).
    kExitFile,   ///< The current file ended and its includer resumed.
  };

  /// Called when lexing moves to a different source file.
  ///
  /// \param loc       Start of the newly-current file (kEnterFile), or the
  ///                  location resumed in the includer (kExitFile).
  /// \param reason    Whether a file was entered or exited.
  /// \param prev_fid  The file being left (kExitFile) or the includer
  ///                  (kEnterFile); invalid for the main file.
  /// \param file_type The system-header characteristic of the file lexing has
  ///                  moved to (the entered file on kEnterFile, the resumed
  ///                  includer on kExitFile).
  virtual void FileChanged(SourceLocation loc, FileChangeReason reason,
                            FileID prev_fid, CharacteristicKind file_type) {}

  /// Called for an #include once its filename has been resolved.
  ///
  /// \param hash_loc    Location of the directive.
  /// \param filename    The header name without its delimiters.
  /// \param is_angled   True for <...>, false for "...".
  /// \param file        The resolved file, or nullptr if not found.
  /// \param file_type   The system-header characteristic of \p file when found
  ///                    (derived from the search-path tier that resolved it);
  ///                    kUser when \p file is null.
  virtual void InclusionDirective(SourceLocation hash_loc,
                                  std::string_view filename, bool is_angled,
                                  const FileEntry* file,
                                  CharacteristicKind file_type) {}

  /// Called when a macro is #defined.
  virtual void MacroDefined(const IdentifierInfo* name, const MacroInfo* macro) {
  }

  /// Called when a macro is #undef'd (whether or not it was defined).
  virtual void MacroUndefined(const IdentifierInfo* name) {}

  /// Called just before a macro use is expanded.
  virtual void MacroExpands(const Token& name, const MacroInfo* macro) {}

  //===--------------------------------------------------------------------===//
  // Conditional inclusion.
  //===--------------------------------------------------------------------===//

  virtual void If(SourceLocation loc, bool condition) {}
  virtual void Elif(SourceLocation loc, bool condition) {}
  virtual void Ifdef(SourceLocation loc, const IdentifierInfo* name,
                     bool defined) {}
  virtual void Ifndef(SourceLocation loc, const IdentifierInfo* name,
                      bool defined) {}
  virtual void Else(SourceLocation loc) {}
  virtual void Endif(SourceLocation loc) {}

  /// Called for a #pragma directive.
  virtual void PragmaDirective(SourceLocation loc) {}
};

}  // namespace bcc
