#pragma once

namespace bcc {

inline constexpr int kInvalidOffset = -1;

struct SourceLocation {
  int offset = kInvalidOffset;
};

}  // namespace bcc