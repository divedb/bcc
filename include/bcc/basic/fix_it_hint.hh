#pragma once

#include <string>
#include <string_view>

#include "bcc/basic/source_range.hh"

namespace bcc {

/// \brief A code-modification suggestion attached to a diagnostic.
///
/// A FixItHint specifies a source range to replace and the replacement text.
/// Pure insertions have a zero-length \c remove_range. Pure removals have an
/// empty \c code_to_insert.
struct FixItHint {
  SourceRange remove_range;    ///< Range of source to replace; may be empty.
  std::string code_to_insert;  ///< Replacement text; empty means pure removal.

  bool IsNull() const noexcept {
    return !remove_range.begin.IsValid() && code_to_insert.empty();
  }

  /// \brief Suggest inserting \p code immediately before \p insert_before.
  static FixItHint CreateInsertion(SourceLocation insert_before,
                                   std::string_view code) {
    FixItHint hint;
    hint.remove_range = SourceRange{insert_before, insert_before};
    hint.code_to_insert = code;
    return hint;
  }

  /// \brief Suggest deleting the source covered by \p range.
  static FixItHint CreateRemoval(SourceRange range) {
    FixItHint hint;
    hint.remove_range = range;
    return hint;
  }

  /// \brief Suggest replacing the source covered by \p range with \p code.
  static FixItHint CreateReplacement(SourceRange range, std::string_view code) {
    FixItHint hint;
    hint.remove_range = range;
    hint.code_to_insert = code;
    return hint;
  }
};

}  // namespace bcc
