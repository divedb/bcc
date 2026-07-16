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
class PPCallbacks {
 public:
  virtual ~PPCallbacks() = default;

  /// Why the current file changed (see FileChanged).
  enum class FileChangeReason {
    kEnterFile,  ///< A new file was entered (main file or an #include).
    kExitFile,   ///< The current file ended and its includer resumed.
  };

  /// \brief Called when lexing moves to a different source file.
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

  /// \brief Called for an #include once its filename has been resolved.
  ///
  /// \param hash_loc  Location of the directive.
  /// \param filename  The header name without its delimiters.
  /// \param is_angled True for <...>, false for "...".
  /// \param file      The resolved file, or nullptr if not found.
  /// \param file_type The system-header characteristic of \p file when found
  ///                  (derived from the search-path tier that resolved it);
  ///                  kUser when \p file is null.
  virtual void InclusionDirective(SourceLocation hash_loc,
                                  std::string_view filename, bool is_angled,
                                  const FileEntry* file,
                                  CharacteristicKind file_type) {}

  /// \brief Called when a macro is #defined.
  ///
  /// \param name  The identifier that names the macro.
  /// \param macro The MacroInfo for the macro.
  virtual void MacroDefined(const IdentifierInfo* name,
                            const MacroInfo* macro) {}

  /// \brief Called when a macro is #undef'd (whether or not it was defined).
  ///
  /// \param name The identifier that names the macro.
  virtual void MacroUndefined(const IdentifierInfo* name) {}

  /// \brief Called just before a macro use is expanded.
  ///
  /// \param name  The identifier that names the macro.
  /// \param macro The MacroInfo for the macro.
  virtual void MacroExpands(const Token& name, const MacroInfo* macro) {}

  //===--------------------------------------------------------------------===//
  // Conditional inclusion.
  //===--------------------------------------------------------------------===//

  /// \brief Called for an #if directive.
  ///
  /// \param loc       Location of the directive.
  /// \param condition The condition of the #if directive.
  virtual void If(SourceLocation loc, bool condition) {}

  /// \brief Called for an #elif directive.
  ///
  /// \param loc       Location of the directive.
  /// \param condition The condition of the #elif directive.
  virtual void Elif(SourceLocation loc, bool condition) {}

  /// \brief Called for an #ifdef directive.
  ///
  /// \param loc     Location of the directive.
  /// \param name    The identifier that names the macro.
  /// \param defined True if the macro is defined.
  virtual void Ifdef(SourceLocation loc, const IdentifierInfo* name,
                     bool defined) {}

  /// \brief Called for an #ifndef directive.
  ///
  /// \param loc     Location of the directive.
  /// \param name    The identifier that names the macro.
  /// \param defined True if the macro is defined.
  virtual void Ifndef(SourceLocation loc, const IdentifierInfo* name,
                      bool defined) {}

  /// \brief Called for an #else directive.
  ///
  /// \param loc Location of the directive.
  virtual void Else(SourceLocation loc) {}

  /// \brief Called for an #endif directive.
  ///
  /// \param loc Location of the directive.
  virtual void Endif(SourceLocation loc) {}

  /// \brief Called for a #pragma directive.
  ///
  /// \param loc Location of the directive.
  virtual void PragmaDirective(SourceLocation loc) {}
};

}  // namespace bcc
