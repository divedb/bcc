#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bcc::as {

/// \brief Lexical token kinds for AT&T x86-64 assembly.
enum class TokKind : uint8_t {
  kEof,
  kNewline,     ///< '\n' or ';' — a statement separator.
  kIdentifier,  ///< symbol, mnemonic, or directive name (incl. leading '.').
  kRegister,    ///< `%name`; `text` is the register name without the '%'.
  kNumber,      ///< integer or character constant; value in `value`.
  kString,      ///< "..."; decoded bytes in `str`.
  kDollar,      ///< $
  kLParen,      ///< (
  kRParen,      ///< )
  kComma,       ///< ,
  kColon,       ///< :
  kStar,        ///< *
  kPlus,        ///< +
  kMinus,       ///< -
  kSlash,       ///< /
  kPercent,     ///< %
  kLShift,      ///< <<
  kRShift,      ///< >>
  kAmp,         ///< &
  kPipe,        ///< |
  kCaret,       ///< ^
  kTilde,       ///< ~
  kEqual,       ///< =
  kAt,          ///< @
  kDot,         ///< a lone '.' — the current location counter.
  kError,       ///< a lexing error (message in `str`).
};

/// \brief A single lexical token.
///
/// `text` is a view into the source buffer (valid for the buffer's lifetime).
/// `value` carries the parsed integer for `kNumber`; `str` carries decoded
/// bytes for `kString`/`kError`.
struct Token {
  TokKind kind = TokKind::kEof;
  std::string_view text;
  uint64_t value = 0;
  std::string str;
  uint32_t offset = 0;  ///< byte offset of `text` within the source buffer

  bool Is(TokKind k) const noexcept { return kind == k; }
  bool IsNot(TokKind k) const noexcept { return kind != k; }
  bool IsEndOfStatement() const noexcept {
    return kind == TokKind::kNewline || kind == TokKind::kEof;
  }
};

}  // namespace bcc::as
