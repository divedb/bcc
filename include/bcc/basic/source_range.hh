#pragma once

#include "bcc/basic/source_location.hh"

namespace bcc {

struct SourceRange {
  SourceLocation begin;
  SourceLocation end;

  SourceRange() = default;
  SourceRange(SourceLocation b, SourceLocation e) : begin(b), end(e) {}
};

}  // namespace bcc
