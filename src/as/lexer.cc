#include "bcc/as/lexer.hh"

#include <cctype>

namespace bcc::as {

namespace {

// Note: '$' is deliberately excluded — it is the immediate-operand sigil, lexed
// as its own token rather than part of an identifier.
bool IsIdentStart(int c) { return std::isalpha(c) || c == '_'; }
bool IsIdentCont(int c) {
  return std::isalnum(c) || c == '_' || c == '.';
}

// Decodes one escape sequence beginning just after the backslash. Advances i
// past the sequence and returns the byte value.
int DecodeEscape(std::string_view s, size_t& i) {
  if (i >= s.size()) return '\\';
  char c = s[i++];
  switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      int v = c - '0';
      for (int k = 0; k < 2 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++k)
        v = v * 8 + (s[i++] - '0');
      return v & 0xff;
    }
    case 'x': {
      int v = 0;
      while (i < s.size() && std::isxdigit((unsigned char)s[i])) {
        char h = s[i++];
        int d = std::isdigit((unsigned char)h)
                    ? h - '0'
                    : std::tolower(h) - 'a' + 10;
        v = v * 16 + d;
      }
      return v & 0xff;
    }
    default:
      return (unsigned char)c;  // \\ \" \' and any other: literal
  }
}

}  // namespace

Token Lexer::Next() {
  if (has_peek_) {
    has_peek_ = false;
    return std::move(peeked_);
  }
  return Lex();
}

const Token& Lexer::Peek() {
  if (!has_peek_) {
    peeked_ = Lex();
    has_peek_ = true;
  }
  return peeked_;
}

void Lexer::SkipHorizontalWs() {
  for (;;) {
    int c = Peek0();
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r') {
      ++pos_;
    } else if (c == '\\' && Peek1() == '\n') {  // line splice
      pos_ += 2;
    } else if (c == '#') {  // comment to end of line
      while (Peek0() != -1 && Peek0() != '\n') ++pos_;
    } else {
      return;
    }
  }
}

Token Lexer::Make(TokKind kind, uint32_t start) {
  Token t;
  t.kind = kind;
  t.offset = start;
  t.text = src_.substr(start, pos_ - start);
  return t;
}

Token Lexer::Error(uint32_t start, std::string msg) {
  Token t = Make(TokKind::kError, start);
  t.str = std::move(msg);
  return t;
}

Token Lexer::Lex() {
  SkipHorizontalWs();
  uint32_t start = static_cast<uint32_t>(pos_);
  int c = Peek0();

  if (c == -1) return Make(TokKind::kEof, start);

  if (c == '\n' || c == ';') {
    ++pos_;
    return Make(TokKind::kNewline, start);
  }

  if (std::isdigit(c)) return LexNumber();
  if (c == '%') return LexRegister();
  if (c == '"') return LexString();
  if (c == '\'') return LexChar();
  if (IsIdentStart(c)) return LexIdentifier();

  if (c == '.') {
    // ".foo" is an identifier; a lone "." is the current-location token.
    if (IsIdentCont(Peek1())) return LexIdentifier();
    ++pos_;
    return Make(TokKind::kDot, start);
  }

  ++pos_;
  switch (c) {
    case '$': return Make(TokKind::kDollar, start);
    case '(': return Make(TokKind::kLParen, start);
    case ')': return Make(TokKind::kRParen, start);
    case ',': return Make(TokKind::kComma, start);
    case ':': return Make(TokKind::kColon, start);
    case '*': return Make(TokKind::kStar, start);
    case '+': return Make(TokKind::kPlus, start);
    case '-': return Make(TokKind::kMinus, start);
    case '/': return Make(TokKind::kSlash, start);
    case '&': return Make(TokKind::kAmp, start);
    case '|': return Make(TokKind::kPipe, start);
    case '^': return Make(TokKind::kCaret, start);
    case '~': return Make(TokKind::kTilde, start);
    case '=': return Make(TokKind::kEqual, start);
    case '@': return Make(TokKind::kAt, start);
    case '<':
      if (Peek0() == '<') { ++pos_; return Make(TokKind::kLShift, start); }
      break;
    case '>':
      if (Peek0() == '>') { ++pos_; return Make(TokKind::kRShift, start); }
      break;
  }
  return Error(start, "unexpected character");
}

Token Lexer::LexIdentifier() {
  uint32_t start = static_cast<uint32_t>(pos_);
  ++pos_;  // first char already validated as start or '.'
  while (IsIdentCont(Peek0())) ++pos_;
  return Make(TokKind::kIdentifier, start);
}

Token Lexer::LexRegister() {
  uint32_t start = static_cast<uint32_t>(pos_);
  ++pos_;  // consume '%'
  uint32_t name_start = static_cast<uint32_t>(pos_);
  while (std::isalnum(Peek0())) ++pos_;
  Token t = Make(TokKind::kRegister, start);
  t.text = src_.substr(name_start, pos_ - name_start);  // name without '%'
  return t;
}

Token Lexer::LexNumber() {
  uint32_t start = static_cast<uint32_t>(pos_);
  uint64_t value = 0;
  if (Peek0() == '0' && (Peek1() == 'x' || Peek1() == 'X')) {
    pos_ += 2;
    while (std::isxdigit(Peek0())) {
      char h = src_[pos_++];
      int d = std::isdigit((unsigned char)h) ? h - '0'
                                             : std::tolower(h) - 'a' + 10;
      value = value * 16 + d;
    }
  } else if (Peek0() == '0' && (Peek1() == 'b' || Peek1() == 'B')) {
    pos_ += 2;
    while (Peek0() == '0' || Peek0() == '1')
      value = value * 2 + (src_[pos_++] - '0');
  } else if (Peek0() == '0' && std::isdigit(Peek1())) {
    ++pos_;  // leading 0 => octal
    while (Peek0() >= '0' && Peek0() <= '7') value = value * 8 + (src_[pos_++] - '0');
  } else {
    while (std::isdigit(Peek0())) value = value * 10 + (src_[pos_++] - '0');
  }
  Token t = Make(TokKind::kNumber, start);
  t.value = value;
  return t;
}

Token Lexer::LexString() {
  uint32_t start = static_cast<uint32_t>(pos_);
  ++pos_;  // opening quote
  std::string out;
  while (Peek0() != -1 && Peek0() != '"') {
    if (Peek0() == '\\') {
      ++pos_;
      size_t i = pos_;
      out.push_back(static_cast<char>(DecodeEscape(src_, i)));
      pos_ = i;
    } else {
      out.push_back(src_[pos_++]);
    }
  }
  if (Peek0() != '"') return Error(start, "unterminated string literal");
  ++pos_;  // closing quote
  Token t = Make(TokKind::kString, start);
  t.str = std::move(out);
  return t;
}

Token Lexer::LexChar() {
  uint32_t start = static_cast<uint32_t>(pos_);
  ++pos_;  // opening quote
  int value;
  if (Peek0() == '\\') {
    ++pos_;
    size_t i = pos_;
    value = DecodeEscape(src_, i);
    pos_ = i;
  } else if (Peek0() != -1) {
    value = (unsigned char)src_[pos_++];
  } else {
    return Error(start, "unterminated character constant");
  }
  if (Peek0() == '\'') ++pos_;  // optional closing quote (AT&T allows omission)
  Token t = Make(TokKind::kNumber, start);
  t.value = static_cast<uint64_t>(value);
  return t;
}

void Lexer::GetLineCol(uint32_t offset, uint32_t& line, uint32_t& col) const {
  line = 1;
  col = 1;
  for (uint32_t i = 0; i < offset && i < src_.size(); ++i) {
    if (src_[i] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
}

}  // namespace bcc::as
