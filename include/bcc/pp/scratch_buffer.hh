#pragma once

#include <string_view>

#include "bcc/basic/source_location.hh"

namespace bcc {

class SourceManager;

/// \brief Stable storage and source locations for synthesized token spellings.
///
/// The `#` (stringize) and `##` (paste) operators produce tokens whose text
/// exists in no original source file. ScratchBuffer copies that text into a
/// buffer registered with the SourceManager so the resulting token has a
/// backing pointer for its spelling and a valid SourceLocation.
///
/// \note This registers one SourceManager buffer per synthesized token, which
///       is simple and correct; Clang packs many tokens into a growing scratch
///       buffer. That optimization can come later.
class ScratchBuffer {
 public:
  explicit ScratchBuffer(SourceManager& sm) : sm_(sm) {}

  /// \brief Copies \p text into scratch space.
  ///
  /// \param text     The spelling to store.
  /// \param out_data Receives a pointer to the stored copy, valid for the
  ///                 SourceManager's lifetime.
  /// \return         The source location of the stored copy.
  SourceLocation GetToken(std::string_view text, const char*& out_data);

 private:
  SourceManager& sm_;
};

}  // namespace bcc
