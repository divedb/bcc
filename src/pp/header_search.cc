#include "bcc/pp/header_search.hh"

#include <algorithm>

#include "bcc/basic/file_manager.hh"
#include "bcc/common/fs_util.hh"

namespace bcc {

auto HeaderSearch::LookupInQuotedDirs(std::string_view filename,
                                      std::string_view includer_dir) const
    -> const FileEntry* {
  if (const FileEntry* fe = fm_.GetFile(JoinPath(includer_dir, filename))) {
    return fe;
  }

  for (const std::string& dir : quoted_dirs_) {
    if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
      return fe;
    }
  }

  return nullptr;
}

auto HeaderSearch::LookupInBuiltinDirs(std::string_view filename) const
    -> const FileEntry* {
  for (const std::string& dir : builtin_dirs_) {
    if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
      // Built-in headers are compiler-provided and report the system
      // characteristic, like files found on the angled search path.
      file_info_[fe].file_characteristic = CharacteristicKind::kSystem;

      return fe;
    }
  }

  return nullptr;
}

auto HeaderSearch::LookupInAngledDirs(std::string_view filename) const
    -> const FileEntry* {
  for (const std::string& dir : angled_dirs_) {
    if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
      // Upgrade only: a file already marked system (e.g. by #pragma
      // system_header) is never demoted back to user code by a later lookup.
      file_info_[fe].file_characteristic = CharacteristicKind::kSystem;

      return fe;
    }
  }

  return nullptr;
}

const FileEntry* HeaderSearch::LookupFile(std::string_view filename,
                                          bool is_angled,
                                          std::string_view includer_dir) {
  // An absolute filename is used verbatim, regardless of the search lists.
  if (IsAbsolutePath(filename)) return fm_.GetFile(filename);

  if (!is_angled) {
    if (const FileEntry* fe = LookupInQuotedDirs(filename, includer_dir)) {
      return fe;
    }
  }

  // The built-in tier precedes the angled list (see AddBuiltinSearchPath).
  if (const FileEntry* fe = LookupInBuiltinDirs(filename)) {
    return fe;
  }

  return LookupInAngledDirs(filename);
}

auto HeaderSearch::LookupNextInQuotedDirs(std::string_view filename,
                                          std::string_view includer_dir) const
    -> const FileEntry* {
  // #include_next resumes after the includer's own entry on the quoted list;
  // if the includer isn't on that list, the whole list is scanned.
  auto it = std::find(quoted_dirs_.begin(), quoted_dirs_.end(), includer_dir);
  const std::size_t begin =
      it != quoted_dirs_.end()
          ? static_cast<std::size_t>(it - quoted_dirs_.begin()) + 1
          : 0;

  for (std::size_t i = begin; i < quoted_dirs_.size(); ++i) {
    if (const FileEntry* fe =
            fm_.GetFile(JoinPath(quoted_dirs_[i], filename))) {
      return fe;
    }
  }

  return nullptr;
}

auto HeaderSearch::LookupNextInAngledChain(std::string_view filename,
                                           std::string_view includer_dir,
                                           bool is_angled) const
    -> const FileEntry* {
  // Walk the built-in + angled chain, skipping everything up to and
  // including the includer's own entry. An angled include_next ignores the
  // directories before it; a quoted include_next (which already scanned the
  // quoted list) scans the whole chain but still skips that one entry.
  bool passed_includer = false;

  auto try_dir = [&](const std::string& dir) -> const FileEntry* {
    if (!passed_includer && dir == includer_dir) {
      passed_includer = true;
      return nullptr;
    }

    if (passed_includer || !is_angled) {
      if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
        file_info_[fe].file_characteristic = CharacteristicKind::kSystem;

        return fe;
      }
    }

    return nullptr;
  };

  for (const std::string& dir : builtin_dirs_) {
    if (const FileEntry* fe = try_dir(dir)) return fe;
  }

  for (const std::string& dir : angled_dirs_) {
    if (const FileEntry* fe = try_dir(dir)) return fe;
  }

  return nullptr;
}

const FileEntry* HeaderSearch::LookupFileNext(std::string_view filename,
                                              bool is_angled,
                                              std::string_view includer_dir) {
  if (IsAbsolutePath(filename)) return fm_.GetFile(filename);

  if (!is_angled) {
    if (const FileEntry* fe = LookupNextInQuotedDirs(filename, includer_dir)) {
      return fe;
    }
  }

  return LookupNextInAngledChain(filename, includer_dir, is_angled);
}

}  // namespace bcc
