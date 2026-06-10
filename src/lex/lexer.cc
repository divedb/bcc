#include "bcc/lex/lexer.hh"

#include <cassert>
#include <cctype>
#include <cstring>

#include "bcc/common/string_util.hh"
#include "bcc/lex/identifier_util.hh"

namespace bcc {

namespace {

// Character classification helpers for preprocessing-token lexing.
// Keep these narrowly scoped so the tokenization rules stay readable at call
// sites without duplicating byte-level checks throughout the lexer.
constexpr bool IsWhitespace(uint32_t ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f';
}

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
  DecodedChar ch1 = cursor.DecodeUTF8();

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
  DecodedChar ch2 = cursor.DecodeUTF8();

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
  DecodedChar ch = cursor.DecodeUTF8();

  if (!ch.IsValid() || (ch.codepoint != 'u' && ch.codepoint != 'U')) {
    cursor = saved;

    return {DecodedChar::kInvalid};
  }

  int num_digits = (ch.codepoint == 'u') ? 4 : 8;
  uint32_t codepoint = 0;

  for (int i = 0; i < num_digits; ++i) {
    ch = cursor.DecodeUTF8();

    if (!ch.IsValid() || !IsHexDigit(ch.codepoint)) {
      cursor = saved;

      return {DecodedChar::kInvalid};
    }

    codepoint = (codepoint << 4) + HexDigitValue(ch.codepoint);
  }

  return {codepoint};
}

}  // namespace

Token BufferedLexer::NextToken() {
  InitializeTokenFlags();
  Token token = LexToken();
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

void BufferedLexer::SkipIgnoredNullBytes() noexcept {
  while (!cursor_.AtEnd() && *cursor_.Current() == '\0') cursor_.Advance();
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
Token BufferedLexer::LexNumericConstant(Cursor cursor) {
  while (!cursor.AtEnd()) {
    Cursor candidate = cursor;
    DecodedChar ch = candidate.DecodeUTF8();

    if (!ch.IsValid()) break;

    const uint32_t cp = ch.codepoint;

    if (IsExponentIntroducer(cp)) {
      Cursor sign_cursor = candidate;
      DecodedChar sign = sign_cursor.DecodeUTF8();

      if (sign.IsValid() && IsSign(sign.codepoint)) {
        cursor = sign_cursor;
        continue;
      }
    }

    if (cp == '.' || IsIdentifierContinue(cp)) {
      cursor = candidate;
      continue;
    }

    break;
  }

  return FinalizeToken(TokenKind::kNumericConstant, cursor);
}

Token BufferedLexer::LexPPNumberOrPeriod(Cursor cursor, uint32_t lead) {
  if (IsDigit(lead)) return LexNumericConstant(cursor);

  assert(lead == '.' &&
         "LexPPNumberOrPeriod should only be called for '.' or digit "
         "leads");

  Cursor saved_cursor = cursor;
  DecodedChar ch = cursor.DecodeUTF8();

  if (ch.IsValid() && IsDigit(ch.codepoint)) {
    return LexNumericConstant(cursor);
  }

  return LexPunctuator(saved_cursor, lead);
}

Token BufferedLexer::LexPunctuator(Cursor cursor, uint32_t lead) {
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
      DecodedChar ch = candidate.DecodeUTF8();

      if (ch.IsEOF() || ch.codepoint != punc.spelling[i]) {
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
Token BufferedLexer::LexIdentifier(Cursor cursor) {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.DecodeUTF8();

    if (!ch.IsValid()) break;

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
// ").
//
// Scans forward to find the matching closing delimiter, treating backslash as
// an escape that skips one character. Returns the token with `kind` on success.
// Returns kUnknown for unterminated literals (unescaped newline or EOF).
Token BufferedLexer::LexDelimitedLiteral(Cursor cursor, TokenKind kind,
                                         char delimiter) {
  while (!cursor.AtEnd()) {
    Cursor saved = cursor;
    DecodedChar ch = cursor.DecodeUTF8();

    if (ch.IsInvalidUTF8()) {
      cursor.Advance();
      continue;
    }

    if (ch.codepoint == '\\') {
      if (!cursor.AtEnd()) cursor.DecodeUTF8();
      continue;
    }

    if (ch.codepoint == static_cast<uint32_t>(delimiter)) {
      return FinalizeToken(kind, cursor);
    }

    if (IsNewLine(ch.codepoint)) {
      return FinalizeToken(TokenKind::kUnknown, saved);
    }
  }

  return FinalizeToken(TokenKind::kUnknown, cursor);
}

Token BufferedLexer::LexDelimitedLiteralOrIdentifier(Cursor cursor,
                                                     uint32_t lead) {
  if (auto prefix = TryClassifyLiteralPrefix(cursor, lead)) {
    return LexDelimitedLiteral(prefix->body, prefix->kind, prefix->delimiter);
  }

  return LexIdentifier(cursor);
}

Token BufferedLexer::LexMultiLineComment(Cursor cursor) {
  bool seen_asterisk = false;

  while (!cursor.AtEnd()) {
    DecodedChar ch = cursor.DecodeUTF8();

    // Invalid UTF-8 sequences are treated as part of the comment text.
    if (ch.IsInvalidUTF8()) {
      cursor.Advance();
      continue;
    }

    if (ch.IsEOF()) break;

    const uint32_t cp = ch.codepoint;

    if (seen_asterisk && cp == '/') {
      return FinalizeToken(TokenKind::kComment, cursor);
    }

    seen_asterisk = (cp == '*');
  }

  // Unterminated comment; emit an unknown token for the entire comment text.
  return FinalizeToken(TokenKind::kUnknown, cursor);
}

Token BufferedLexer::LexSingleLineComment(Cursor cursor) {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.DecodeUTF8();

    // Invalid UTF-8 sequences are treated as part of the comment text.
    if (ch.IsInvalidUTF8()) {
      cursor.Advance();
      continue;
    }

    if (ch.IsEOF()) break;

    if (IsNewLine(ch.codepoint)) {
      return FinalizeToken(TokenKind::kComment, saved_cursor);
    }
  }

  return FinalizeToken(TokenKind::kComment, cursor);
}

// precondition: The first character of the token has already been consumed
//               and is a '/'.
Token BufferedLexer::LexCommentOrSlash(Cursor cursor) {
  Cursor saved_cursor = cursor;
  DecodedChar ch = cursor.DecodeUTF8();

  // If the next character is not '/' or '*', this is not a comment. Delegate
  // to LexPunctuator to check for multi-character punctuators such as "/=".
  if (!ch.IsValid() || (ch.codepoint != '/' && ch.codepoint != '*')) {
    return LexPunctuator(saved_cursor, '/');
  }

  const uint32_t cp = ch.codepoint;

  assert(
      (cp == '/' || cp == '*') &&
      "LexCommentOrSlash should only be called when the next character is '/' "
      "or '*'");

  if (cp == '/') return LexSingleLineComment(cursor);

  return LexMultiLineComment(cursor);
}

Token BufferedLexer::LexNewLine(Cursor cursor, uint32_t lead) {
  // Handle \r\n as a single newline token.
  if (lead == '\r' && !cursor.AtEnd() && *cursor.Current() == '\n') {
    cursor.Advance();
  }

  return FinalizeToken(TokenKind::kNewLine, cursor);
}

// Precondition: The first character of the token has already been consumed
//               and is a whitespace character.
Token BufferedLexer::LexWhiteSpace(Cursor cursor) {
  while (!cursor.AtEnd()) {
    Cursor saved_cursor = cursor;
    DecodedChar ch = cursor.DecodeUTF8();

    if (!ch.IsValid()) break;

    if (!IsWhitespace(ch.codepoint)) {
      return FinalizeToken(TokenKind::kWhitespace, saved_cursor);
    }
  }

  return FinalizeToken(TokenKind::kWhitespace, cursor);
}

Token BufferedLexer::EOFToken() {
  int offset = static_cast<int>(cursor_.End() - cursor_.Begin());

  return Token{SourceLocation{offset}, TokenKind::kEOF, "",
               current_token_flags_};
}

Token BufferedLexer::FinalizeToken(TokenKind kind, Cursor cursor) {
  const char* start = cursor_.Current();
  const char* end = cursor.Current();

  std::string lexeme(start, static_cast<size_t>(end - start));
  SourceLocation loc{static_cast<int>(start - cursor_.Begin())};
  cursor_ = cursor;  // commit here

  if (cursor.HadLineSplice()) [[unlikely]] {
    current_token_flags_ |= TokenFlag::kNeedsCleaning;
  }

  return Token{loc, kind, std::move(lexeme), current_token_flags_};
}

Token BufferedLexer::LexToken() {
  SkipIgnoredNullBytes();

  Cursor lookahead = cursor_;
  DecodedChar ch = lookahead.DecodeUTF8();

  if (ch.IsEOF()) return EOFToken();

  // Invalid UTF-8 sequences are treated as single-character tokens of kind
  // kUnknown.
  if (ch.IsInvalidUTF8()) {
    lookahead.Advance();

    return FinalizeToken(TokenKind::kUnknown, lookahead);
  }

  const uint32_t cp = ch.codepoint;

  if (IsWhitespace(cp)) return LexWhiteSpace(lookahead);
  if (IsNewLine(cp)) return LexNewLine(lookahead, cp);
  if (cp == '/') return LexCommentOrSlash(lookahead);

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
      // Truly malformed UCN — emit only '\', re-lex 'u...' as identifier
      return FinalizeToken(TokenKind::kUnknown, before_ucn);
    }

    if (IsForbiddenUCNCodepoint(ucn.codepoint) ||
        !IsIdentifierStart(ucn.codepoint)) {
      return FinalizeToken(TokenKind::kUnknown, lookahead);
    }

    return LexIdentifier(lookahead);
  }

  if (IsIdentifierStart(cp)) return LexIdentifier(lookahead);
  if (IsPunctuatorStart(cp)) return LexPunctuator(lookahead, cp);

  return FinalizeToken(TokenKind::kUnknown, lookahead);
}

}  // namespace bcc
