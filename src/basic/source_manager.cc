#include "bcc/basic/source_manager.hh"

#include <algorithm>
#include <cassert>

#include "bcc/basic/file_manager.hh"

namespace bcc {

SourceManager::SourceManager(FileManager& fm) : file_manager_(fm) {
  sloc_entries_.push_back(SLocEntry{0, FileInfo{}});
}

FileID SourceManager::RegisterBuffer(const FileEntry* entry,
                                     std::unique_ptr<MemoryBuffer> buf,
                                     SourceLocation include_loc) {
  uint32_t size = static_cast<uint32_t>(buf->GetBufferSize());
  uint32_t start = next_offset_;
  next_offset_ += size + 1;

  auto cc = std::make_unique<ContentCache>(entry, std::move(buf));
  int id = static_cast<int>(sloc_entries_.size());
  sloc_entries_.push_back({start, FileInfo{std::move(cc), include_loc}});

  return FileID::Get(id);
}

FileID SourceManager::CreateFileID(std::string identifier, std::string content,
                                   SourceLocation include_loc) {
  auto buf = MemoryBuffer::GetMemoryBufferCopy(std::move(content),
                                               std::move(identifier));

  return RegisterBuffer(nullptr, std::move(buf), include_loc);
}

FileID SourceManager::CreateFileID(const FileEntry& entry,
                                   SourceLocation include_loc) {
  auto it = file_to_id_.find(&entry);

  if (it != file_to_id_.end()) return it->second;

  auto buf = file_manager_.GetBufferForFile(entry);

  if (!buf) return FileID{};

  FileID fid = RegisterBuffer(&entry, std::move(buf), include_loc);
  file_to_id_[&entry] = fid;

  return fid;
}

FileID SourceManager::CreateExpansionLoc(SourceLocation spelling_loc,
                                         SourceLocation expansion_start,
                                         SourceLocation expansion_end,
                                         uint32_t token_length) {
  uint32_t start = next_offset_;
  next_offset_ += (token_length > 0) ? token_length : 1;

  int id = static_cast<int>(sloc_entries_.size());
  sloc_entries_.push_back(
      {start, ExpansionInfo{spelling_loc, expansion_start, expansion_end}});

  return FileID::Get(id);
}

SourceLocation SourceManager::GetLocForStartOfFile(FileID fid) const noexcept {
  return GetLocForOffset(fid, 0);
}

SourceLocation SourceManager::GetLocForEndOfFile(FileID fid) const noexcept {
  assert(fid.IsValid() && static_cast<size_t>(fid.id_) < sloc_entries_.size());

  const SLocEntry& entry = sloc_entries_[static_cast<size_t>(fid.id_)];

  assert(entry.IsFile() && "GetLocForEndOfFile requires a file SLocEntry");

  uint32_t size =
      static_cast<uint32_t>(entry.GetFileInfo().cache->buffer->GetBufferSize());

  return GetLocForOffset(fid, size);
}

SourceLocation SourceManager::GetLocForOffset(FileID fid,
                                              uint32_t offset) const noexcept {
  assert(fid.IsValid() && static_cast<size_t>(fid.id_) < sloc_entries_.size());

  const SLocEntry& entry = sloc_entries_[static_cast<size_t>(fid.id_)];
  uint32_t global = entry.offset + offset;

  if (entry.IsExpansion()) {
    return SourceLocation{SourceLocation::kMacroFlag | global};
  }

  return SourceLocation{global};
}

SourceLocation SourceManager::TranslateLineCol(FileID fid, unsigned line,
                                               unsigned col) const {
  if (!fid.IsValid() || line == 0) return SourceLocation{};

  const ContentCache& cache = GetContentCache(fid);
  EnsureLineCount(cache, line);

  if (line > static_cast<unsigned>(cache.line_offsets.size())) {
    return SourceLocation{};
  }

  uint32_t offset = cache.line_offsets[line - 1] + (col > 0 ? col - 1 : 0);

  return GetLocForOffset(fid, offset);
}

FileID SourceManager::GetFileID(SourceLocation loc) const noexcept {
  if (!loc.IsValid()) return FileID{};

  uint32_t global = loc.GetOffset();

  // Fast path: check the cached last FileID first.
  if (last_file_id_ >= 0) {
    auto id = static_cast<size_t>(last_file_id_);
    uint32_t lo = sloc_entries_[id].offset;
    uint32_t hi = (id + 1 < sloc_entries_.size()) ? sloc_entries_[id + 1].offset
                                                  : next_offset_;

    if (global >= lo && global < hi) return FileID::Get(last_file_id_);
  }

  // Binary search: find the last SLocEntry whose offset <= global.
  auto it = std::upper_bound(
      sloc_entries_.begin(), sloc_entries_.end(), global,
      [](uint32_t g, const SLocEntry& e) { return g < e.offset; });

  assert(it != sloc_entries_.begin() &&
         "SourceLocation has no owning SLocEntry");

  --it;
  int id = static_cast<int>(it - sloc_entries_.begin());
  last_file_id_ = id;

  return FileID::Get(id);
}

std::pair<FileID, uint32_t> SourceManager::GetDecomposedLoc(
    SourceLocation loc) const noexcept {
  FileID fid = GetFileID(loc);

  return {fid, GetFileOffset(loc)};
}

std::pair<FileID, uint32_t> SourceManager::GetDecomposedSpellingLoc(
    SourceLocation loc) const noexcept {
  return GetDecomposedLoc(GetSpellingLoc(loc));
}

std::pair<FileID, uint32_t> SourceManager::GetDecomposedExpansionLoc(
    SourceLocation loc) const noexcept {
  return GetDecomposedLoc(GetExpansionLoc(loc));
}

uint32_t SourceManager::GetFileOffset(SourceLocation loc) const noexcept {
  FileID fid = GetFileID(loc);

  assert(fid.IsValid());

  return loc.GetOffset() - sloc_entries_[static_cast<size_t>(fid.id_)].offset;
}

SourceLocation SourceManager::GetSpellingLoc(
    SourceLocation loc) const noexcept {
  while (loc.IsMacroExpansion()) {
    loc = GetSLocEntry(GetFileID(loc)).GetExpansionInfo().spelling_loc;
  }

  return loc;
}

SourceLocation SourceManager::GetExpansionLoc(
    SourceLocation loc) const noexcept {
  while (loc.IsMacroExpansion()) {
    loc = GetSLocEntry(GetFileID(loc)).GetExpansionInfo().expansion_start;
  }

  return loc;
}

std::pair<SourceLocation, SourceLocation>
SourceManager::GetImmediateExpansionRange(SourceLocation loc) const noexcept {
  if (!loc.IsMacroExpansion()) return {loc, loc};

  const ExpansionInfo& info = GetSLocEntry(GetFileID(loc)).GetExpansionInfo();

  return {info.expansion_start, info.expansion_end};
}

SourceRange SourceManager::GetExpansionRange(
    SourceLocation loc) const noexcept {
  auto [start, end] = GetImmediateExpansionRange(loc);

  return {start, end};
}

const SLocEntry& SourceManager::GetSLocEntry(FileID fid) const noexcept {
  assert(fid.IsValid() && static_cast<size_t>(fid.id_) < sloc_entries_.size());

  return sloc_entries_[static_cast<size_t>(fid.id_)];
}

const ContentCache& SourceManager::GetContentCache(FileID fid) const noexcept {
  const SLocEntry& entry = GetSLocEntry(fid);

  assert(entry.IsFile() && "Expected a file SLocEntry, not an expansion");

  return *entry.GetFileInfo().cache;
}

void SourceManager::ScanLines(const ContentCache& cache,
                              uint32_t scan_end) const {
  if (cache.line_offsets.empty()) cache.line_offsets.push_back(0);

  std::string_view buf = cache.buffer->GetBuffer();
  const uint32_t end = static_cast<uint32_t>(buf.size());
  uint32_t i = cache.scanned_offset;

  while (i < scan_end) {
    const char ch = buf[i];

    if (ch == '\n') {
      cache.line_offsets.push_back(i + 1);
    } else if (ch == '\r') {
      if (i + 1 < end && buf[i + 1] == '\n') ++i;

      cache.line_offsets.push_back(i + 1);
    }

    ++i;
  }

  cache.scanned_offset = i;
}

void SourceManager::EnsureLinesCoveredTo(const ContentCache& cache,
                                         uint32_t offset) const {
  if (cache.scanned_offset > offset) return;

  const uint32_t end = static_cast<uint32_t>(cache.buffer->GetBuffer().size());
  ScanLines(cache, std::min(offset + 1, end));
}

void SourceManager::EnsureLineCount(const ContentCache& cache,
                                    unsigned n) const {
  if (cache.line_offsets.size() >= n) return;

  const uint32_t end = static_cast<uint32_t>(cache.buffer->GetBuffer().size());
  ScanLines(cache, end);
}

unsigned SourceManager::ComputeLineNumber(const ContentCache& cache,
                                          uint32_t offset) const {
  auto it = std::upper_bound(cache.line_offsets.begin(),
                             cache.line_offsets.end(), offset);

  return static_cast<unsigned>(it - cache.line_offsets.begin());
}

unsigned SourceManager::ComputeColumnNumber(const ContentCache& cache,
                                            uint32_t offset) const {
  auto it = std::upper_bound(cache.line_offsets.begin(),
                             cache.line_offsets.end(), offset);
  --it;

  return static_cast<unsigned>(offset - *it) + 1;
}

std::string_view SourceManager::GetBufferData(FileID fid) const noexcept {
  return GetContentCache(fid).buffer->GetBuffer();
}

std::string_view SourceManager::GetFilename(FileID fid) const noexcept {
  return GetContentCache(fid).buffer->GetBufferIdentifier();
}

const char* SourceManager::GetCharacterData(SourceLocation loc) const noexcept {
  auto [fid, offset] = GetDecomposedSpellingLoc(loc);

  return GetContentCache(fid).buffer->GetBufferStart() + offset;
}

unsigned SourceManager::GetLine(SourceLocation loc) const {
  const ContentCache& cache = GetContentCache(GetFileID(loc));
  uint32_t offset = GetFileOffset(loc);
  EnsureLinesCoveredTo(cache, offset);

  return ComputeLineNumber(cache, offset);
}

unsigned SourceManager::GetColumn(SourceLocation loc) const {
  const ContentCache& cache = GetContentCache(GetFileID(loc));
  uint32_t offset = GetFileOffset(loc);
  EnsureLinesCoveredTo(cache, offset);

  return ComputeColumnNumber(cache, offset);
}

PresumedLoc SourceManager::GetPresumedLoc(SourceLocation loc,
                                          bool use_line_directives) const {
  if (!loc.IsValid()) return PresumedLoc{};

  SourceLocation file_loc = GetSpellingLoc(loc);
  FileID fid = GetFileID(file_loc);
  const ContentCache& cache = GetContentCache(fid);
  uint32_t offset = GetFileOffset(file_loc);
  EnsureLinesCoveredTo(cache, offset);

  auto line_it = std::upper_bound(cache.line_offsets.begin(),
                                  cache.line_offsets.end(), offset);
  unsigned phys_line =
      static_cast<unsigned>(line_it - cache.line_offsets.begin());
  unsigned col = static_cast<unsigned>(offset - *std::prev(line_it)) + 1;

  std::string_view filename = cache.buffer->GetBufferIdentifier();
  SourceLocation include_loc = GetIncludeLoc(fid);

  if (use_line_directives && line_table_.HasEntries(fid)) {
    const LineEntry* entry = line_table_.FindEntry(fid, offset);

    if (entry) {
      auto eit = std::upper_bound(cache.line_offsets.begin(),
                                  cache.line_offsets.end(), entry->file_offset);
      unsigned entry_phys_line =
          static_cast<unsigned>(eit - cache.line_offsets.begin());
      unsigned presumed_line = entry->line_num + (phys_line - entry_phys_line);

      std::string_view presumed_filename =
          (entry->filename_id >= 0)
              ? line_table_.GetFilename(entry->filename_id)
              : filename;

      return PresumedLoc{presumed_filename, presumed_line, col, include_loc};
    }
  }

  return PresumedLoc{filename, phys_line, col, include_loc};
}

std::string_view SourceManager::GetSourceText(SourceRange range) const {
  assert(GetFileID(range.begin) == GetFileID(range.end));

  uint32_t begin_offset = GetFileOffset(range.begin);
  uint32_t end_offset = GetFileOffset(range.end);

  return GetContentCache(GetFileID(range.begin))
      .buffer->GetBuffer()
      .substr(begin_offset, end_offset - begin_offset);
}

SourceLocation SourceManager::GetIncludeLoc(FileID fid) const noexcept {
  if (!fid.IsValid()) return SourceLocation{};

  const SLocEntry& entry = GetSLocEntry(fid);

  if (!entry.IsFile()) return SourceLocation{};

  return entry.GetFileInfo().include_loc;
}

bool SourceManager::IsInFileID(SourceLocation loc, FileID fid,
                               uint32_t* rel_offset) const noexcept {
  if (!loc.IsValid() || !fid.IsValid()) return false;

  bool in_file = GetFileID(loc) == fid;

  if (in_file && rel_offset) *rel_offset = GetFileOffset(loc);

  return in_file;
}

bool SourceManager::IsInMainFile(SourceLocation loc) const noexcept {
  if (!main_file_id_.IsValid()) return false;

  return GetFileID(GetSpellingLoc(loc)) == main_file_id_;
}

bool SourceManager::IsWrittenInMainFile(SourceLocation loc) const noexcept {
  if (!main_file_id_.IsValid()) return false;

  return GetFileID(loc) == main_file_id_;
}

}  // namespace bcc
