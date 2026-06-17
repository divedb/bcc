#pragma once

#include <compare>

namespace bcc {

/// \brief An identifier representing a file managed by SourceManager.
///
/// FileID is an opaque handle used to refer to a file loaded into the system.
class FileID {
 public:
  /// \brief Returns a sentinel (invalid) FileID.
  ///
  /// \return The sentinel value represents an invalid file reference.
  static FileID GetSentinel() noexcept { return Get(kSentinelID); }

  constexpr bool IsValid() const noexcept { return id_ != kSentinelID; }

  /// \brief Returns the raw integer representation of this FileID.
  ///
  /// \return Integer ID associated with this FileID.
  unsigned Hash() const noexcept { return static_cast<unsigned>(id_); }

  constexpr auto operator<=>(const FileID&) const noexcept = default;

 private:
  friend class SourceManager;
  friend class LineTableInfo;

  static constexpr int kSentinelID = 0;

  static FileID Get(int v) noexcept {
    FileID fid;
    fid.id_ = v;

    return fid;
  }

  int id_ = kSentinelID;
};

}  // namespace bcc