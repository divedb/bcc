#pragma once

#include <cstdint>

namespace bcc {

/// \brief Encodes a position in the SourceManager's global offset space.
///
/// A SourceLocation is a 32-bit value split into two fields:
///
///   bit 31 (kMacroFlag) — set for macro expansion locations, clear for file
///                         locations.
///   bits 30–0 (offset)  — byte offset within the owning SLocEntry's slice of
///                         the global offset space.
///
/// The default-constructed value (raw_ == 0) is the invalid sentinel.
struct SourceLocation {
  /// High bit of raw_: set iff this location belongs to a macro expansion
  /// SLocEntry rather than a file SLocEntry.
  static constexpr uint32_t kMacroFlag = 0x8000'0000u;

  constexpr SourceLocation() noexcept = default;
  constexpr explicit SourceLocation(uint32_t raw) noexcept : raw_(raw) {}

  /// \return True if this location is not the invalid sentinel.
  bool IsValid() const noexcept { return raw_ != 0; }

  /// \return True if this is a macro expansion location, false if it's a file
  ///         location.
  bool IsMacroExpansion() const noexcept { return (raw_ & kMacroFlag) != 0; }

  /// \return The raw offset with the kMacroFlag bit cleared.
  uint32_t GetOffset() const noexcept { return raw_ & ~kMacroFlag; }

  bool operator==(const SourceLocation&) const noexcept = default;

 private:
  uint32_t raw_ = 0;
};

}  // namespace bcc
