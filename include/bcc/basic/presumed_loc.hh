#pragma once

#include <string_view>

#include "bcc/basic/source_location.hh"

namespace bcc {

struct PresumedLoc {
  std::string_view filename;
  unsigned line = 0;
  unsigned col = 0;
  SourceLocation include_loc;

  PresumedLoc() = default;
  PresumedLoc(std::string_view fn, unsigned l, unsigned c, SourceLocation inc)
      : filename(fn), line(l), col(c), include_loc(inc) {}

  bool IsValid() const noexcept { return line != 0; }
};

}  // namespace bcc
