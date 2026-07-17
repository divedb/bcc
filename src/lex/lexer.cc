#include "bcc/lex/lexer.hh"

#include <cassert>
#include <cstring>
#include <optional>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/common/string_util.hh"
#include "bcc/lex/identifier_util.hh"

namespace bcc {

namespace {

constexpr bool IsEncodingPrefix(uint32_t ch) noexcept {
  return ch == 'u' || ch == 'U' || ch == 'L';
}

constexpr bool IsLiteralDelimiter(uint32_t ch) noexcept {
  return ch == '\'' || ch == '"';
}

constexpr bool IsExponentIntroducer(uint32_t ch) noexcept {
  return ch == 'e' || ch == 'E' || ch == 'p' || ch == 'P';
}

constexpr bool IsPunctuatorStart(uint32_t ch) noexcept {
  switch (ch) {
    case '.':
    case '<':
    case '>':
    case '-':
    case '+':
    case '*':
    case '/':
    case '%':
    case '&':
    case '|':
    case '^':
    case '~':
    case '!':
    case '=':
    case '?':
    case ':':
    case ';':
    case ',':
    case '#':
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
      return true;
    default:
      return false;
  }
}

struct LiteralPrefix {
  TokenKind kind;
  char delimiter;
  Cursor body;  // positioned just past the opening delimiter
};

// Precondition: `cursor` is positioned immediately after `lead` has been
// consumed. `lead` is one of: ' " u U L (as dispatched by LexToken).
//
// Peeks ahead to determine whether `lead` and what follows constitute a valid
// literal introduction. Returns the kind, the opening delimiter character, and
// a cursor positioned just past that delimiter. Returns nullopt when the
// sequence is not a literal and the caller should fall back to LexIdentifier.
//
// All peeking is done on a local copy of `cursor`, so the caller's cursor is
// never modified regardless of the outcome.
std::optional<LiteralPrefix> TryClassifyLiteralPrefix(Cursor cursor,
                                                      uint32_t lead) {
  // Plain delimiters: `lead` itself is the opening delimiter, and `cursor` is
  // already positioned past it.
  if (lead == '\'') {
    return LiteralPrefix{TokenKind::kCharConstant, '\'', cursor};
  }

  if (lead == '"') return LiteralPrefix{TokenKind::kStringLiteral, '"', cursor};

  // Single-character encoding prefixes (U, L, u): peek one character.
  DecodedChar ch1 = cursor.Next();

  if (lead == 'U') {
    if (ch1.codepoint == '\'') {
      return LiteralPrefix{TokenKind::kUtf32CharConstant, '\'', cursor};
    }

    if (ch1.codepoint == '"') {
      return LiteralPrefix{TokenKind::kUtf32StringLiteral, '"', cursor};
    }

    return std::nullopt;
  }

  if (lead == 'L') {
    if (ch1.codepoint == '\'') {
      return LiteralPrefix{TokenKind::kWideCharConstant, '\'', cursor};
    }

    if (ch1.codepoint == '"') {
      return LiteralPrefix{TokenKind::kWideStringLiteral, '"', cursor};
    }

    return std::nullopt;
  }

  // lead == 'u': may be u', u", or the two-character prefix u8.
  if (ch1.codepoint == '\'') {
    return LiteralPrefix{TokenKind::kUtf16CharConstant, '\'', cursor};
  }

  if (ch1.codepoint == '"') {
    return LiteralPrefix{TokenKind::kUtf16StringLiteral, '"', cursor};
  }

  if (ch1.codepoint != '8') return std::nullopt;

  // u8 prefix: peek one more character for the delimiter.
  // u8' is not a valid literal prefix in C11/C17; u8 is lexed as an identifier.
  DecodedChar ch2 = cursor.Next();

  if (ch2.codepoint == '"') {
    return LiteralPrefix{TokenKind::kUtf8StringLiteral, '"', cursor};
  }

  return std::nullopt;
}

// C disallows UCNs naming basic source characters below 0x00A0, except '$',
// '@', and '`', and also forbids surrogate code points outright.
constexpr bool IsForbiddenUCNCodepoint(uint32_t cp) noexcept {
  if (cp >= 0xD800 && cp <= 0xDFFF) return true;  // surrogate range

  return cp < 0x00A0 && cp != 0x0024 && cp != 0x0040 && cp != 0x0060;
}

// Precondition: '\' has already been consumed.
// UCNs have the form \uXXXX or \UXXXXXXXX where X is a hexadecimal digit.
DecodedChar DecodeUCN(Cursor& cursor) noexcept {
  Cursor saved = cursor;
  DecodedChar ch = cursor.Next();

  if (!ch.IsValid() || (ch.codepoint != 'u' && ch.codepoint != 'U')) {
    cursor = saved;

    return {DecodedChar::kInvalid};
  }

  int num_digits = (ch.codepoint == 'u') ? 4 : 8;
  uint32_t codepoint = 0;

  for (int i = 0; i < num_digits; ++i) {
    ch = cursor.Next();

    if (!ch.IsValid() || !IsHexDigit(ch.codepoint)) {
      cursor = saved;

      return {DecodedChar::kInvalid};
    }

    codepoint = (codepoint << 4) + HexDigitValue(ch.codepoint);
  }

  return {codepoint};
}

}  // namespace

BufferedLexer::BufferedLexer(SourceManager& sm, FileID fid,
                             DiagnosticsEngine* diag)
    : sm_(sm),
      fid_(fid),
      diag_(diag),
      cursor_(sm.GetBufferData(fid)),
      current_token_flags_(TokenFlag::kNone),
      is_at_start_of_line_(true),
      has_leading_space_(false) {}

Token BufferedLexer::NextToken() { return Lex(false); }

Token BufferedLexer::LexHeaderName() { return Lex(true); }

Token BufferedLexer::Lex(bool recognize_header_name) {
  while (!cursor_.AtEnd() && *cursor_.Current() == '\0') {
    // Clang ignores raw NUL bytes in the source, but they still behave like
    // separating whitespace for the following token's LeadingSpace state.
    // We currently skip them here without updating lexer spacing state.
    has_leading_space_ = true;
    cursor_.Advance();
  }

  // Skip over any conflict-marker sections we have reached.
  // A conflict start marker may immediately follow another skip.
  while (ApplyConflictSkips());

  if (is_at_start_of_line_ && TryConflictMarker()) {
    while (ApplyConflictSkips());
  }

  InitializeTokenFlags();
  Token token = LexToken(recognize_header_name);
  UpdateLexerState(token.GetKind());

  return token;
}

void BufferedLexer::InitializeTokenFlags() noexcept {
  current_token_flags_ = TokenFlag::kNone;

  if (is_at_start_of_line_) current_token_flags_ |= TokenFlag::kStartOfLine;
  if (has_leading_space_) current_token_flags_ |= TokenFlag::kLeadingSpace;

  cursor_.ResetLineSpliceFlag();
}

void BufferedLexer::UpdateLexerState(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kWhitespace:
    case TokenKind::kComment:
      has_leading_space_ = true;
      break;
    case TokenKind::kNewLine:
      is_at_start_of_line_ = true;
      has_leading_space_ = false;
      break;
    default:
      is_at_start_of_line_ = false;
      has_leading_space_ = false;
      break;
  }
}

Token BufferedLexer::LexHeaderNameBody(Cursor after_open, char close) noexcept {
  // Scan on a copy so an unterminated name can fall back to the ordinary token
  // beginning at the opening delimiter. Cursor::Next applies translation-phase
  // line splicing and propagates kNeedsCleaning through FinalizeToken.
  Cursor scan = after_open;

  while (!scan.AtEnd()) {
    DecodedChar ch = scan.Next();

    if (ch.IsEOF() || IsNewLine(ch.codepoint)) break;

    if (ch.codepoint == static_cast<uint32_t>(close)) {
      return FinalizeToken(TokenKind::kHeaderName, scan);
    }
  }

  if (close == '>') return LexPunctuator(after_open, '<');

  return LexDelimitedLiteral(after_open, TokenKind::kStringLiteral, '"');
}

// 6.4.8 Preprocessing numbers
// Syntax
// pp-number:
//   | digit
//   | "." digit
//   | pp-number identifier-nondigit
//   | pp-number "e" sign
//   | pp-number "E" sign
//   | pp-number "p" sign
//   | pp-number "P" sign
//   | pp-number "."
//
// Precondition: The first character of the pp-number has already been consumed
//               and is either a digit or a '.' followed by a digit.
Token BufferedLexer::LexNumericConstant(Cursor cursor) noexcept {
  while (!cursor.AtEnd()) {
    Cursor candidate = cursor;
    DecodedChar ch = candidate.Next();

    if (!ch.IsValid()) break;

    const uint32_t cp = ch.codepoint;

    if (IsExponentIntroducer(cp)) {
      Cursor sign_cursor = candidate;
      DecodedChar sign = sign_cursor.Next();

      if (sign.IsValid() && IsSign(sign.codepoint)) {
        cursor = sign_cursor;
        continue;
      }
    }

    if (cp == '\\') {
      // A \uXXXX / \UXXXXXXXX UCN is part of the pp-number spelling (clang
      // keeps it literal even when it forms an invalid suffix).
      const char* p = candidate.Current();

      if (p < candidate.End() && (*p == 'u' || *p == 'U')) {
        int digits = (*p == 'u') ? 4 : 8;
        const char* h = p + 1;
        bool ok = (h + digits <= candidate.End());

        for (int k = 0; ok && k < digits; ++k) {
          ok = IsHexDigit(static_cast<unsigned char>(h[k]));
        }

        if (ok) {
          candidate.Advance(static_cast<std::size_t>(1 + digits));
          cursor = candidate;
          continue;
        }
      }

      break;
    }

    if (cp == '.' || IsIdentifierContinue(cp)) {
      cursor = candidate;
      continue;
    }

    break;
  }

  return FinalizeToken(TokenKind::kNumericConstant, cursor);
}

Token BufferedLexer::LexPPNumberOrPeriod(Cursor cursor,
                                         uint32_t lead) noexcept {
  if (IsDigit(lead)) return LexNumericConstant(cursor);

  assert(lead == '.' &&
         "LexPPNumberOrPeriod should only be called for '.' or digit "
         "leads");

  Cursor saved_cursor = cursor;
  DecodedChar ch = cursor.Next();

  if (ch.IsValid() && IsDigit(ch.codepoint)) {
    return LexNumericConstant(cursor);
  }

  return LexPunctuator(saved_cursor, lead);
}

Token BufferedLexer::LexPunctuator(Cursor cursor, uint32_t lead) noexcept {
  struct PunctuatorSpelling {
    std::string_view spelling;
    TokenKind kind;
  };

  static constexpr PunctuatorSpelling kPunctuators[] = {
      {"...", TokenKind::kEllipsis},
      {"<<=", TokenKind::kLessLessEqual},
      {">>=", TokenKind::kGreaterGreaterEqual},
      {"->", TokenKind::kArrow},
      {"++", TokenKind::kPlusPlus},
      {"--", TokenKind::kMinusMinus},
      {"<<", TokenKind::kLessLess},
      {">>", TokenKind::kGreaterGreater},
      {"<=", TokenKind::kLessEqual},
      {">=", TokenKind::kGreaterEqual},
      {"==", TokenKind::kEqualEqual},
      {"!=", TokenKind::kExclaimEqual},
      {"&&", TokenKind::kAmpAmp},
      {"||", TokenKind::kPipePipe},
      {"*=", TokenKind::kStarEqual},
      {"/=", TokenKind::kSlashEqual},
      {"%=", TokenKind::kPercentEqual},
      {"+=", TokenKind::kPlusEqual},
      {"-=", TokenKind::kMinusEqual},
      {"&=", TokenKind::kAmpEqual},
      {"^=", TokenKind::kCaretEqual},
      {"|=", TokenKind::kPipeEqual},
      {"##", TokenKind::kHashHash},
      {"[", TokenKind::kLSquare},
      {"]", TokenKind::kRSquare},
      {"(", TokenKind::kLParen},
      {")", TokenKind::kRParen},
      {"{", TokenKind::kLBrace},
      {"}", TokenKind::kRBrace},
      {".", TokenKind::kPeriod},
      {"&", TokenKind::kAmp},
      {"*", TokenKind::kStar},
      {"+", TokenKind::kPlus},
      {"-", TokenKind::kMinus},
      {"~", TokenKind::kTilde},
      {"!", TokenKind::kExclaim},
      {"/", TokenKind::kSlash},
      {"%", TokenKind::kPercent},
      {"<", TokenKind::kLess},
      {">", TokenKind::kGreater},
      {"^", TokenKind::kCaret},
      {"|", TokenKind::kPipe},
      {"?", TokenKind::kQuestion},
      {":", TokenKind::kColon},
      {";", TokenKind::kSemi},
      {"=", TokenKind::kEqual},
      {",", TokenKind::kComma},
      {"#", TokenKind::kHash},
  };

  for (auto& punc : kPunctuators) {
    if (lead != static_cast<unsigned char>(punc.spelling[0])) continue;

    Cursor candidate = cursor;
    bool matches = true;

    for (size_t i = 1; i < punc.spelling.size(); i++) {
      DecodedChar ch = candidate.Next();

      if (!ch.IsValid() || ch.codepoint != punc.spelling[i]) {
        matches = false;
        break;
      }
    }

    if (matches) return FinalizeToken(punc.kind, candidate);
  }

  return FinalizeToken(TokenKind::kUnknown, cursor);
}

// identifier:
//   | identifier-nondigit
//   | identifier identifier-nondigit
//   | identifier digit
//
// identifier-nondigit:
//   ｜ nondigit
//   ｜ universal-character-name
//   ｜ other implementation-defined characters
//
// Precondition: The first character of the identifier has already been
//               consumed.
Token BufferedLexer::LexIdentifier(Cursor cursor) noexcept {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.Next();

    // Invalid UTF-8 ends the identifier; restore so the bad byte is not
    // included in the lexeme and is re-lexed as the next token.
    if (!ch.IsValid()) {
      cursor = saved_cursor;
      break;
    }

    if (ch.codepoint == '\\') {
      ch = DecodeUCN(cursor);

      if (!ch.IsValid() || IsForbiddenUCNCodepoint(ch.codepoint)) {
        cursor = saved_cursor;
        break;
      }
    }

    if (!IsIdentifierContinue(ch.codepoint)) {
      cursor = saved_cursor;
      break;
    }
  }

  return FinalizeToken(TokenKind::kIdentifier, cursor);
}

// precondition: cursor is positioned after the opening delimiter (either ' or
//               ").
//
// Scans forward to find the matching closing delimiter, treating backslash as
// an escape that skips one character. Returns the token with `kind` on success.
// Returns kUnknown for unterminated literals (unescaped newline or EOF).
Token BufferedLexer::LexDelimitedLiteral(Cursor cursor, TokenKind kind,
                                         char delimiter) noexcept {
  while (!cursor.AtEnd()) {
    Cursor saved = cursor;
    DecodedChar ch = cursor.Next();

    // Invalid UTF-8 is kept as literal text; Next() already stepped past the
    // bad byte.
    if (ch.IsInvalidUTF8()) continue;

    if (ch.codepoint == '\\') {
      if (!cursor.AtEnd()) cursor.Next();

      continue;
    }

    if (ch.codepoint == static_cast<uint32_t>(delimiter)) {
      return FinalizeToken(kind, cursor);
    }

    if (IsNewLine(ch.codepoint)) {
      // Unterminated literal ended by a newline.
      if (diag_) {
        diag::DiagKind dk = (delimiter == '\'')
                                ? diag::err_unterminated_char_constant
                                : diag::err_unterminated_string_literal;
        diag_->Report(CurrentTokenLoc(), dk);
      }

      return FinalizeToken(TokenKind::kUnknown, saved);
    }
  }

  // Unterminated literal ended by EOF.
  if (diag_) {
    diag::DiagKind dk = (delimiter == '\'')
                            ? diag::err_unterminated_char_constant
                            : diag::err_unterminated_string_literal;
    diag_->Report(CurrentTokenLoc(), dk);
  }

  return FinalizeToken(TokenKind::kUnknown, cursor);
}

Token BufferedLexer::LexDelimitedLiteralOrIdentifier(Cursor cursor,
                                                     uint32_t lead) noexcept {
  if (auto prefix = TryClassifyLiteralPrefix(cursor, lead)) {
    return LexDelimitedLiteral(prefix->body, prefix->kind, prefix->delimiter);
  }

  return LexIdentifier(cursor);
}

Token BufferedLexer::LexMultiLineComment(Cursor cursor) noexcept {
  bool seen_asterisk = false;

  while (!cursor.AtEnd()) {
    DecodedChar ch = cursor.Next();

    if (ch.IsEOF()) break;

    const uint32_t cp = ch.codepoint;

    if (seen_asterisk && cp == '/') {
      return FinalizeToken(TokenKind::kComment, cursor);
    }

    seen_asterisk = (cp == '*');
  }

  // Unterminated comment.
  if (diag_) {
    diag_->Report(CurrentTokenLoc(), diag::err_unterminated_block_comment);
  }

  return FinalizeToken(TokenKind::kUnknown, cursor);
}

Token BufferedLexer::LexSingleLineComment(Cursor cursor) noexcept {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.Next();

    // Invalid UTF-8 is treated as comment text; Next() already stepped past
    // the bad byte.
    if (ch.IsInvalidUTF8()) continue;

    if (ch.IsEOF()) break;

    if (IsNewLine(ch.codepoint)) {
      return FinalizeToken(TokenKind::kComment, saved_cursor);
    }
  }

  return FinalizeToken(TokenKind::kComment, cursor);
}

// precondition: The first character of the token has already been consumed
//               and is a '/'.
Token BufferedLexer::LexCommentOrSlash(Cursor cursor) noexcept {
  Cursor saved_cursor = cursor;
  DecodedChar ch = cursor.Next();

  // If the next character is not '/' or '*', this is not a comment. Delegate
  // to LexPunctuator to check for multi-character punctuators such as "/=".
  if (!ch.IsValid() || (ch.codepoint != '/' && ch.codepoint != '*')) {
    return LexPunctuator(saved_cursor, '/');
  }

  if (ch.codepoint == '/') return LexSingleLineComment(cursor);

  return LexMultiLineComment(cursor);
}

Token BufferedLexer::LexNewLine(Cursor cursor, uint32_t lead) noexcept {
  // Handle \r\n as a single newline token.
  if (lead == '\r' && !cursor.AtEnd() && *cursor.Current() == '\n') {
    cursor.Advance();
  }

  return FinalizeToken(TokenKind::kNewLine, cursor);
}

// Precondition: The first character of the token has already been consumed
//               and is a whitespace character.
Token BufferedLexer::LexWhiteSpace(Cursor cursor) noexcept {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.Next();

    // Invalid UTF-8 ends the run; restore so the bad byte is re-lexed as the
    // next token rather than swallowed into the whitespace.
    if (!ch.IsValid()) {
      cursor = saved_cursor;
      break;
    }

    if (!IsWhitespace(ch.codepoint)) {
      return FinalizeToken(TokenKind::kWhitespace, saved_cursor);
    }
  }

  return FinalizeToken(TokenKind::kWhitespace, cursor);
}

SourceLocation BufferedLexer::CurrentTokenLoc() const noexcept {
  uint32_t offset = static_cast<uint32_t>(cursor_.Current() - cursor_.Begin());
  return sm_.GetLocForOffset(fid_, offset);
}

Token BufferedLexer::EOFToken() noexcept {
  uint32_t local_offset =
      static_cast<uint32_t>(cursor_.End() - cursor_.Begin());

  return Token{sm_.GetLocForOffset(fid_, local_offset), TokenKind::kEOF,
               cursor_.End(), 0u, current_token_flags_};
}

Token BufferedLexer::FinalizeToken(TokenKind kind, Cursor cursor) noexcept {
  const char* start = cursor_.Current();
  const char* end = cursor.Current();
  uint32_t length = static_cast<uint32_t>(end - start);
  uint32_t local_offset = static_cast<uint32_t>(start - cursor_.Begin());
  SourceLocation loc = sm_.GetLocForOffset(fid_, local_offset);
  cursor_ = cursor;  // commit here

  if (cursor.HadLineSplice()) [[unlikely]] {
    current_token_flags_ |= TokenFlag::kNeedsCleaning;
  }

  return Token{loc, kind, start, length, current_token_flags_};
}

Token BufferedLexer::LexToken(bool recognize_header_name) noexcept {
  Cursor lookahead = cursor_;
  DecodedChar ch = lookahead.Next();

  if (ch.IsEOF()) return EOFToken();

  // Invalid UTF-8 sequences are single-character tokens of kind kUnknown.
  // Next() has already advanced one byte past the bad lead byte.
  if (ch.IsInvalidUTF8()) {
    if (diag_) diag_->Report(CurrentTokenLoc(), diag::err_invalid_utf8);

    return FinalizeToken(TokenKind::kUnknown, lookahead);
  }

  const uint32_t cp = ch.codepoint;

  if (IsWhitespace(cp)) return LexWhiteSpace(lookahead);
  if (IsNewLine(cp)) return LexNewLine(lookahead, cp);
  if (cp == '/') return LexCommentOrSlash(lookahead);

  if (recognize_header_name) {
    if (cp == '<') return LexHeaderNameBody(lookahead, '>');
    if (cp == '"') return LexHeaderNameBody(lookahead, '"');
  }

  // Handle literals and potentially ambiguous literal-prefix characters
  // (u, U, L) that may also begin identifiers.
  if (IsLiteralDelimiter(cp) || IsEncodingPrefix(cp)) {
    return LexDelimitedLiteralOrIdentifier(lookahead, cp);
  }

  if (IsDigit(cp) || cp == '.') return LexPPNumberOrPeriod(lookahead, cp);

  if (cp == '\\') {
    Cursor before_ucn = lookahead;  // position after '\', before 'u'
    DecodedChar ucn = DecodeUCN(lookahead);

    if (!ucn.IsValid()) {
      // Not a valid UCN.  Treat it as an identifier (includes '\') when the
      // next character can start an identifier -- this accommodates GAS macro
      // parameters such as \name inside .macro / .endm blocks.  Otherwise
      // emit the stray backslash as kUnknown.
      if (!before_ucn.AtEnd()) {
        unsigned char first = *before_ucn.Current();
        // \u and \U are UCN prefix attempts; if the hex digits are bogus we
        // still want the malformed-UCN diagnostic.  For any other letter,
        // underscore, or high-byte start, treat as an assembly-style
        // \name identifier (GAS macro parameter).
        if ((std::isalpha(first) && first != 'u' && first != 'U') ||
            first == '_' || first & 0x80) {
          return LexIdentifier(lookahead);
        }
      }

      if (diag_) diag_->Report(CurrentTokenLoc(), diag::err_malformed_ucn);

      return FinalizeToken(TokenKind::kUnknown, before_ucn);
    }

    if (IsForbiddenUCNCodepoint(ucn.codepoint)) {
      if (diag_) {
        diag_->Report(CurrentTokenLoc(), diag::err_forbidden_ucn_codepoint);
      }

      return FinalizeToken(TokenKind::kUnknown, lookahead);
    }

    if (!IsIdentifierStart(ucn.codepoint)) {
      if (diag_) {
        diag_->Report(CurrentTokenLoc(), diag::err_invalid_ucn_in_identifier);
      }

      return FinalizeToken(TokenKind::kUnknown, lookahead);
    }

    return LexIdentifier(lookahead);
  }

  if (IsIdentifierStart(cp)) return LexIdentifier(lookahead);
  if (IsPunctuatorStart(cp)) return LexPunctuator(lookahead, cp);

  return FinalizeToken(TokenKind::kUnknown, lookahead);
}

//===----------------------------------------------------------------------===//
// Version-control conflict marker recovery
//===----------------------------------------------------------------------===//

namespace {

// Returns a pointer to the first byte of the line after the line containing
// \p p (i.e. just past its terminating newline), or \p end at EOF.
const char* NextLineStart(const char* p, const char* end) noexcept {
  while (p < end && *p != '\n') {
    if (*p == '\r') {
      ++p;

      if (p < end && *p == '\n') ++p;

      return p;
    }

    ++p;
  }

  if (p < end) ++p;  // past the '\n'

  return p;
}

// True if the line starting at \p p begins with \p marker (length \p len) and
// is followed by horizontal whitespace, a newline, or EOF — i.e. it is a
// conflict-marker line, not merely a prefix of one.
bool LineIsMarker(const char* p, const char* end, const char* marker,
                  std::size_t len) noexcept {
  if (static_cast<std::size_t>(end - p) < len) return false;
  if (std::memcmp(p, marker, len) != 0) return false;

  char c = (p + len < end) ? p[len] : '\n';

  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

bool BufferedLexer::ApplyConflictSkips() noexcept {
  const char* p = cursor_.Current();

  for (const auto& s : conflict_skips_) {
    if (p >= s.start && p < s.end) {
      cursor_.Advance(static_cast<std::size_t>(s.end - p));
      has_leading_space_ = true;
      is_at_start_of_line_ = true;

      return true;
    }
  }

  return false;
}

bool BufferedLexer::TryConflictMarker() noexcept {
  const char* p = cursor_.Current();
  const char* end = cursor_.End();

  // diff3 / normal conflict start: "<<<<<<<" followed by space/newline.
  if (LineIsMarker(p, end, "<<<<<<<", 7)) {
    const char* after_start = NextLineStart(p, end);
    const char* sep = nullptr;
    const char* endmk = nullptr;

    for (const char* l = after_start; l < end; l = NextLineStart(l, end)) {
      if (sep == nullptr && (LineIsMarker(l, end, "|||||||", 7) ||
                             LineIsMarker(l, end, "=======", 7))) {
        sep = l;
      }

      if (LineIsMarker(l, end, ">>>>>>>", 7)) {
        endmk = l;
        break;
      }
    }

    cursor_.Advance(static_cast<std::size_t>(after_start - p));

    if (sep != nullptr && endmk != nullptr) {
      conflict_skips_.push_back({sep, NextLineStart(endmk, end)});
    }

    return true;
  }

  // Perforce conflict start: ">>>> " (4 '>' then space).
  if (end - p >= 5 && std::memcmp(p, ">>>>", 4) == 0 && p[4] == ' ') {
    const char* after_start = NextLineStart(p, end);
    const char* sep = nullptr;
    const char* endmk = nullptr;

    for (const char* l = after_start; l < end; l = NextLineStart(l, end)) {
      if (sep == nullptr && end - l >= 5 && std::memcmp(l, "====", 4) == 0 &&
          l[4] == ' ') {
        sep = l;
      }

      if (LineIsMarker(l, end, "<<<<", 4)) {
        endmk = l;
        break;
      }
    }

    cursor_.Advance(static_cast<std::size_t>(after_start - p));

    if (sep != nullptr) {
      const char* stop = (endmk != nullptr) ? NextLineStart(endmk, end) : end;
      conflict_skips_.push_back({sep, stop});
    }

    // No separator: just consume the start marker line; the rest is kept.
    return true;
  }

  return false;
}

}  // namespace bcc
