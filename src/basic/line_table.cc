#include "bcc/basic/line_table.hh"

#include <algorithm>
#include <cassert>

namespace bcc {

bool LineTableInfo::HasEntries(FileID fid) const noexcept {
  return entries_.count(fid) > 0;
}

const LineEntry* LineTableInfo::FindEntry(FileID fid,
                                          uint32_t offset) const noexcept {
  auto it = entries_.find(fid);
  if (it == entries_.end()) return nullptr;

  const std::vector<LineEntry>& vec = it->second;
  // Find the last entry whose file_offset <= offset.
  auto pos = std::upper_bound(
      vec.begin(), vec.end(), offset,
      [](uint32_t off, const LineEntry& e) { return off < e.file_offset; });

  if (pos == vec.begin()) return nullptr;
  return &*std::prev(pos);
}

std::string_view LineTableInfo::GetFilename(int filename_id) const noexcept {
  assert(filename_id >= 0 &&
         static_cast<size_t>(filename_id) < filenames_.size());
  return filenames_[static_cast<size_t>(filename_id)];
}

void LineTableInfo::AddEntry(FileID fid, LineEntry entry) {
  entries_[fid].push_back(entry);
}

int LineTableInfo::GetOrCreateFilenameID(std::string filename) {
  for (size_t i = 0; i < filenames_.size(); ++i) {
    if (filenames_[i] == filename)
      return static_cast<int>(i);
  }
  int id = static_cast<int>(filenames_.size());
  filenames_.push_back(std::move(filename));
  return id;
}

}  // namespace bcc
