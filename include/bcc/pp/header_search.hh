#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bcc/basic/characteristic_kind.hh"

namespace bcc {

class FileEntry;
class FileManager;
class IdentifierInfo;

/// \brief Per-header bookkeeping for the multiple-include optimization.
///
/// Once a header has been fully preprocessed, this records whether re-including
/// it is known to be a no-op: either it declared `#pragma once`, or its whole
/// body is wrapped in a single `#ifndef GUARD` include guard whose controlling
/// macro is captured here. In either case a later #include of the same file can
/// be skipped without re-reading it.
struct HeaderFileInfo {
  /// The file contains `#pragma once`; it must never be entered again.
  bool is_pragma_once = false;

  /// The file was `#import`ed; a subsequent #import of the same file is
  /// a no-op (even without an include guard or #pragma once).
  bool is_imported = false;

  /// The controlling macro of the file's `#ifndef` include guard, or nullptr if
  /// the file has no recognized guard. When this macro is #defined, entering
  /// the file is known to produce no tokens.
  const IdentifierInfo* controlling_macro = nullptr;

  /// The file's system-header characteristic, learned from the search-path tier
  /// that resolved it (system paths -> kSystem) and overridable to kSystem by
  /// `#pragma GCC system_header`. Mirrors Clang's HeaderFileInfo::DirInfo.
  CharacteristicKind file_characteristic = CharacteristicKind::kUser;
};

/// \brief Resolves #include filenames against an ordered list of search
///        directories and tracks the multiple-include optimization.
///
///   * A quoted include ("foo.h") is looked up first relative to the directory
///     of the file performing the include, then in the quoted search list, then
///     in the angled (system) search list.
///   * An angled include (<foo.h>) is looked up only in the angled search list.
///
/// \note An absolute filename bypasses the search list entirely.
class HeaderSearch {
 public:
  explicit HeaderSearch(FileManager& fm) : fm_(fm) {}

  HeaderSearch(const HeaderSearch&) = delete;
  HeaderSearch& operator=(const HeaderSearch&) = delete;

  /// \brief Adds a directory searched for quoted ("...") includes only.
  ///
  /// \param dir The directory to add to the quoted search list.
  void AddQuotedSearchPath(std::string dir) {
    quoted_dirs_.push_back(std::move(dir));
  }

  /// \brief Adds a directory searched for both angled (<...>) and quoted
  ///        includes (the system / -I search list).
  ///
  /// \param dir The directory to add to the angled search list.
  void AddAngledSearchPath(std::string dir) {
    angled_dirs_.push_back(std::move(dir));
  }

  /// \brief Resolves \p filename to a FileEntry, or nullptr if not found.
  ///
  /// \param filename     The header name, without its <> or "" delimiters.
  /// \param is_angled    True for <...>, false for "...".
  /// \param includer_dir Directory of the file performing the include (used for
  ///                     quoted includes); empty if unknown.
  const FileEntry* LookupFile(std::string_view filename, bool is_angled,
                              std::string_view includer_dir);

  /// \brief Resolves \p filename for #include_next: starts searching from the
  ///        directory *after* \p includer_dir in the search path.
  ///
  /// For a quoted include, the includer_dir-relative search is skipped and
  /// the quoted/angled search lists are used, starting after the entry that
  /// would have matched the includer. For an angled include, only the angled
  /// search list is used.
  ///
  /// \param filename     The header name, without its <> or "" delimiters.
  /// \param is_angled    True for <...>, false for "...".
  /// \param includer_dir Directory of the file performing the include.
  /// \return             The FileEntry found, or nullptr.
  const FileEntry* LookupFileNext(std::string_view filename, bool is_angled,
                                  std::string_view includer_dir);

  //===--------------------------------------------------------------------===//
  // Multiple-include optimization.
  //===--------------------------------------------------------------------===//

  /// \brief Returns the HeaderFileInfo for \p fe if one has been recorded, or
  ///        nullptr if the file has never been fully preprocessed.
  ///
  /// \param fe The file to query.
  /// \return   The HeaderFileInfo for \p fe, or nullptr if none exists
  const HeaderFileInfo* GetExistingFileInfo(const FileEntry* fe) const {
    auto it = file_info_.find(fe);

    return it == file_info_.end() ? nullptr : &it->second;
  }

  /// \brief Determine whether this file is intended to be safe from
  ///        multiple inclusions, e.g., it has \#pragma once or a controlling
  ///        macro.
  ///
  /// This routine does not consider the effect of \#import
  bool IsFileMultipleIncludeGuarded(const FileEntry* fe) const {
    if (const HeaderFileInfo* info = GetExistingFileInfo(fe)) {
      return info->is_pragma_once || info->controlling_macro != nullptr;
    }

    return false;
  }

  /// \brief Marks \p fe as `#pragma once`: it must not be entered again.
  ///
  /// \param fe The file to mark as `#pragma once`.
  void MarkFileIncludeOnce(const FileEntry* fe) {
    file_info_[fe].is_pragma_once = true;
  }

  /// \brief Records the include-guard controlling macro learned for \p fe.
  ///
  /// \param fe The file to record the controlling macro for.
  /// \param m  The controlling macro.
  void SetFileControllingMacro(const FileEntry* fe, const IdentifierInfo* m) {
    file_info_[fe].controlling_macro = m;
  }

  /// \brief Marks \p fe as imported via `#import`.
  ///
  /// \param fe The file to mark as imported.
  void SetFileImported(const FileEntry* fe) {
    file_info_[fe].is_imported = true;
  }

  //===--------------------------------------------------------------------===//
  // System-header characteristic.
  //===--------------------------------------------------------------------===//

  /// \brief Returns the system-header characteristic recorded for \p fe.
  ///
  /// A file found on the angled (system) search path reports kSystem; one found
  /// via the includer's directory or the quoted search list reports kUser
  /// unless `#pragma GCC system_header` (see MarkFileAsSystemHeader) has since
  /// promoted it. Returns kUser for a file with no recorded info (e.g. the main
  /// file, or \p fe null).
  CharacteristicKind GetFileCharacteristic(const FileEntry* fe) const {
    if (const HeaderFileInfo* info = GetExistingFileInfo(fe)) {
      return info->file_characteristic;
    }

    return CharacteristicKind::kUser;
  }

  /// \brief Promotes \p fe to a system header, as if by
  ///        `#pragma GCC system_header`.
  ///
  /// \param fe The file to mark as a system header.
  void MarkFileAsSystemHeader(const FileEntry* fe) {
    file_info_[fe].file_characteristic = CharacteristicKind::kSystem;
  }

 private:
  auto LookupInQuotedDirs(std::string_view filename,
                          std::string_view includer_dir) const
      -> const FileEntry*;

  auto LookupInAngledDirs(std::string_view filename) const -> const FileEntry*;

  auto LookupNextInQuotedDirs(std::string_view filename,
                              std::string_view includer_dir) const
      -> const FileEntry*;

  auto LookupNextInAngledDirs(std::string_view filename,
                              std::string_view includer_dir,
                              bool is_angled) const
      -> const FileEntry*;

  FileManager& fm_;
  std::vector<std::string> quoted_dirs_;
  std::vector<std::string> angled_dirs_;
  mutable std::unordered_map<const FileEntry*, HeaderFileInfo> file_info_;
};

}  // namespace bcc
