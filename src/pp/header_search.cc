#include "bcc/pp/header_search.hh"

#include "bcc/basic/file_manager.hh"

namespace bcc {

namespace {

/// Joins a search directory and a (relative) filename into a lookup path.
/// An absolute filename or an empty directory yields the filename unchanged.
std::string JoinPath(std::string_view dir, std::string_view filename) {
  if (!filename.empty() && filename.front() == '/') {
    return std::string(filename);
  }
  if (dir.empty()) return std::string(filename);

  std::string out(dir);
  if (out.back() != '/') out.push_back('/');
  out.append(filename);
  return out;
}

}  // namespace

const FileEntry* HeaderSearch::LookupFile(std::string_view filename,
                                          bool is_angled,
                                          std::string_view includer_dir) {
  // An absolute filename is used verbatim, regardless of the search lists.
  if (!filename.empty() && filename.front() == '/') {
    return fm_.GetFile(filename);
  }

  if (!is_angled) {
    // Quoted include: try the includer's own directory first, then the quoted
    // search list. Both are user tiers, so a hit here is kUser (the default).
    if (const FileEntry* fe = fm_.GetFile(JoinPath(includer_dir, filename))) {
      return fe;
    }
    for (const std::string& dir : quoted_dirs_) {
      if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
        return fe;
      }
    }
  }

  // Both angled includes and quoted includes that fell through above search the
  // angled (system) list. A hit here promotes the file to a system header.
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

const FileEntry* HeaderSearch::LookupFileNext(std::string_view filename,
                                              bool is_angled,
                                              std::string_view includer_dir) {
  if (!filename.empty() && filename.front() == '/') {
    return fm_.GetFile(filename);
  }

  // Find the index of the directory that would have resolved the includer
  // in the angled search list. We start searching from the entry after it.
  std::size_t start_angled = 0;

  // For quoted includes, also skip the includer_dir test and the quoted list
  // up to and including the matching entry.
  if (!is_angled) {
    // Compute the directory key for the includer (the directory part of the
    // path). We skip the includer_dir itself in the search.
    // We search the quoted list after the entry that matches includer_dir.
    bool found_in_quoted = false;
    for (std::size_t i = 0; i < quoted_dirs_.size(); ++i) {
      if (quoted_dirs_[i] == includer_dir) {
        start_angled = i + 1;
        found_in_quoted = true;
        break;
      }
    }

    if (found_in_quoted) {
      // Search remaining quoted dirs after the includer's dir (user tier).
      for (std::size_t i = start_angled; i < quoted_dirs_.size(); ++i) {
        if (const FileEntry* fe = fm_.GetFile(JoinPath(quoted_dirs_[i], filename))) {
          return fe;
        }
      }
    } else {
      // Includer dir not found in quoted list; search whole quoted list.
      for (const std::string& dir : quoted_dirs_) {
        if (const FileEntry* fe = fm_.GetFile(JoinPath(dir, filename))) {
          return fe;
        }
      }
    }
  }

  // For #include_next, find the includer_dir in the angled list and
  // start searching after it.
  bool found_in_angled = false;
  for (std::size_t i = 0; i < angled_dirs_.size(); ++i) {
    if (!found_in_angled && angled_dirs_[i] == includer_dir) {
      found_in_angled = true;
      continue;  // skip this entry, start from next
    }
    if (found_in_angled || !is_angled) {
      if (const FileEntry* fe = fm_.GetFile(JoinPath(angled_dirs_[i], filename))) {
        file_info_[fe].file_characteristic = CharacteristicKind::kSystem;
        return fe;
      }
    }
  }

  return nullptr;
}

}  // namespace bcc
