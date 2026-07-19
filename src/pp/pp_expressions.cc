#include <cstdint>
#include <functional>
#include <string_view>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/common/string_util.hh"
#include "bcc/lex/numeric_literal.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/preprocessor.hh"

namespace bcc {

namespace {

/// A value in a preprocessor constant expression: a 64-bit integer plus its
/// signedness (per the C usual arithmetic conversions, everything is intmax_t
/// or uintmax_t).
struct PPValue {
  int64_t value = 0;
  bool is_unsigned = false;
};

/// Parses an integer preprocessing-number token. Reports floating constants as
/// an error (they are not permitted in #if).
PPValue ParseIntegerConstant(const Token& tok, DiagnosticsEngine& diags,
                             bool& ok) {
  ok = true;
  NumericLiteralParser literal(tok.GetLexeme());
  if (literal.HadError()) {
    diags.Report(tok.GetLocation(), diag::err_pp_invalid_integer_constant);
    ok = false;
    return {};
  }
  if (literal.IsFloatingLiteral()) {
    diags.Report(tok.GetLocation(), diag::err_pp_floating_constant);
    ok = false;
    return {};
  }

  auto parsed = literal.GetValue(64);
  if (!parsed) {
    diags.Report(tok.GetLocation(),
                 parsed.Error() == NumericLiteralParser::Error::kOverflow
                     ? diag::err_pp_integer_constant_too_large
                     : diag::err_pp_invalid_integer_constant);
    ok = false;
    return {};
  }
  const APSInt& value = parsed.Value().GetInt();

  bool is_unsigned = literal.IsUnsigned();
  if (!is_unsigned && value.IsNegative()) {
    // C permits implicit uintmax_t selection for non-decimal constants. Clang
    // also recovers from an oversized decimal constant as unsigned after
    // diagnosing it; BCC treats diagnostics as fatal, so reject that case.
    if (literal.GetRadix() == 10) {
      diags.Report(tok.GetLocation(), diag::err_pp_integer_constant_too_large);
      ok = false;
      return {};
    }
    is_unsigned = true;
  }
  return {static_cast<int64_t>(value.GetZExtValue()), is_unsigned};
}

/// Parses a character-constant token into its integer value.
PPValue ParseCharConstant(const Token& tok) {
  std::string_view s = tok.GetLexeme();
  // Skip an encoding prefix (L, u, U, u8) and the opening quote.
  std::size_t i = 0;
  while (i < s.size() && s[i] != '\'') ++i;
  ++i;  // past the opening '

  int64_t value = 0;
  int count = 0;
  while (i < s.size() && s[i] != '\'') {
    uint32_t c;
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      char e = s[i++];
      switch (e) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        case '0':
          c = 0;
          break;
        case '\\':
          c = '\\';
          break;
        case '\'':
          c = '\'';
          break;
        case '"':
          c = '"';
          break;
        case 'a':
          c = '\a';
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'v':
          c = '\v';
          break;
        case 'x': {
          uint32_t v = 0;
          while (i < s.size() && IsHexDigit(static_cast<unsigned char>(s[i]))) {
            v = v * 16 + static_cast<uint32_t>(
                             HexDigitValue(static_cast<unsigned char>(s[i])));
            ++i;
          }
          c = v;
          break;
        }
        default:
          c = static_cast<unsigned char>(e);
          break;
      }
    } else {
      c = static_cast<unsigned char>(s[i++]);
    }
    // Multi-character constants combine byte-wise (implementation-defined).
    value = (value << 8) | (c & 0xFF);
    ++count;
  }
  (void)count;
  return {value, false};
}

int BinaryPrecedence(TokenKind kind) {
  switch (kind) {
    case TokenKind::kStar:
    case TokenKind::kSlash:
    case TokenKind::kPercent:
      return 10;
    case TokenKind::kPlus:
    case TokenKind::kMinus:
      return 9;
    case TokenKind::kLessLess:
    case TokenKind::kGreaterGreater:
      return 8;
    case TokenKind::kLess:
    case TokenKind::kGreater:
    case TokenKind::kLessEqual:
    case TokenKind::kGreaterEqual:
      return 7;
    case TokenKind::kEqualEqual:
    case TokenKind::kExclaimEqual:
      return 6;
    case TokenKind::kAmp:
      return 5;
    case TokenKind::kCaret:
      return 4;
    case TokenKind::kPipe:
      return 3;
    case TokenKind::kAmpAmp:
      return 2;
    case TokenKind::kPipePipe:
      return 1;
    default:
      return 0;  // not a binary operator
  }
}

/// Recursive-descent evaluator over a stream of already-expanded tokens.
class ExprParser {
 public:
  ExprParser(std::function<Token()> fetch, DiagnosticsEngine& diags)
      : fetch_(std::move(fetch)), diags_(diags) {}

  PPValue Parse() {
    Advance();
    return ParseConditional();
  }

  const Token& Current() const { return peek_; }

  // Consumes remaining tokens up to (not past) the end-of-directive marker.
  void SkipToEnd() {
    while (peek_.GetKind() != TokenKind::kEod &&
           peek_.GetKind() != TokenKind::kEOF) {
      Advance();
    }
  }

 private:
  void Advance() {
    // Never read past the end of the directive line.
    if (peek_.GetKind() == TokenKind::kEod ||
        peek_.GetKind() == TokenKind::kEOF) {
      return;
    }
    peek_ = fetch_();
  }

  void Error(diag::DiagKind kind) { diags_.Report(peek_.GetLocation(), kind); }

  PPValue ParseConditional() {
    PPValue cond = ParseBinary(1);
    if (peek_.GetKind() != TokenKind::kQuestion) return cond;

    Advance();  // consume '?'
    PPValue lhs = ParseConditional();
    if (peek_.GetKind() == TokenKind::kColon) {
      Advance();
    } else {
      Error(diag::err_pp_expected_value);
    }
    PPValue rhs = ParseConditional();

    PPValue result = cond.value != 0 ? lhs : rhs;
    result.is_unsigned = lhs.is_unsigned || rhs.is_unsigned;
    return result;
  }

  PPValue ParseBinary(int min_prec) {
    PPValue lhs = ParseUnary();
    for (;;) {
      TokenKind op = peek_.GetKind();
      int prec = BinaryPrecedence(op);
      if (prec == 0 || prec < min_prec) break;
      Advance();
      PPValue rhs = ParseBinary(prec + 1);
      lhs = ApplyBinary(op, lhs, rhs);
    }
    return lhs;
  }

  PPValue ParseUnary() {
    switch (peek_.GetKind()) {
      case TokenKind::kPlus: {
        Advance();
        return ParseUnary();
      }
      case TokenKind::kMinus: {
        Advance();
        PPValue v = ParseUnary();
        v.value = -v.value;
        return v;
      }
      case TokenKind::kExclaim: {
        Advance();
        PPValue v = ParseUnary();
        return {v.value == 0 ? 1 : 0, false};
      }
      case TokenKind::kTilde: {
        Advance();
        PPValue v = ParseUnary();
        v.value = ~v.value;
        return v;
      }
      default:
        return ParsePrimary();
    }
  }

  PPValue ParsePrimary() {
    Token t = peek_;
    switch (t.GetKind()) {
      case TokenKind::kNumericConstant: {
        bool ok = true;
        PPValue v = ParseIntegerConstant(t, diags_, ok);
        Advance();
        return v;
      }
      case TokenKind::kCharConstant:
      case TokenKind::kWideCharConstant:
      case TokenKind::kUtf16CharConstant:
      case TokenKind::kUtf32CharConstant: {
        PPValue v = ParseCharConstant(t);
        Advance();
        return v;
      }
      case TokenKind::kLParen: {
        Advance();
        PPValue v = ParseConditional();
        if (peek_.GetKind() == TokenKind::kRParen) {
          Advance();
        } else {
          Error(diag::err_pp_expected_rparen);
        }
        return v;
      }
      default:
        // Any identifier or keyword that survived macro expansion is 0.
        if (t.GetIdentifierInfo() != nullptr) {
          Advance();
          return {0, false};
        }
        Error(diag::err_pp_expected_value);
        return {0, false};
    }
  }

  PPValue ApplyBinary(TokenKind op, PPValue lhs, PPValue rhs) {
    bool result_unsigned = lhs.is_unsigned || rhs.is_unsigned;
    uint64_t lu = static_cast<uint64_t>(lhs.value);
    uint64_t ru = static_cast<uint64_t>(rhs.value);

    auto arith = [&](int64_t s, uint64_t u) -> PPValue {
      return {result_unsigned ? static_cast<int64_t>(u) : s, result_unsigned};
    };

    switch (op) {
      case TokenKind::kStar:
        return arith(lhs.value * rhs.value, lu * ru);
      case TokenKind::kSlash:
        if (rhs.value == 0) {
          diags_.Report(peek_.GetLocation(), diag::err_pp_division_by_zero);
          return {0, result_unsigned};
        }
        return result_unsigned ? PPValue{static_cast<int64_t>(lu / ru), true}
                               : PPValue{lhs.value / rhs.value, false};
      case TokenKind::kPercent:
        if (rhs.value == 0) {
          diags_.Report(peek_.GetLocation(), diag::err_pp_division_by_zero);
          return {0, result_unsigned};
        }
        return result_unsigned ? PPValue{static_cast<int64_t>(lu % ru), true}
                               : PPValue{lhs.value % rhs.value, false};
      case TokenKind::kPlus:
        return arith(lhs.value + rhs.value, lu + ru);
      case TokenKind::kMinus:
        return arith(lhs.value - rhs.value, lu - ru);
      case TokenKind::kLessLess:
        // Result has the type (signedness) of the left operand.
        return {static_cast<int64_t>(lu << (ru & 63)), lhs.is_unsigned};
      case TokenKind::kGreaterGreater:
        return lhs.is_unsigned
                   ? PPValue{static_cast<int64_t>(lu >> (ru & 63)), true}
                   : PPValue{lhs.value >> (rhs.value & 63), false};
      case TokenKind::kAmp:
        return arith(lhs.value & rhs.value, lu & ru);
      case TokenKind::kCaret:
        return arith(lhs.value ^ rhs.value, lu ^ ru);
      case TokenKind::kPipe:
        return arith(lhs.value | rhs.value, lu | ru);
      case TokenKind::kLess:
        return {Compare(lhs, rhs) < 0 ? 1 : 0, false};
      case TokenKind::kGreater:
        return {Compare(lhs, rhs) > 0 ? 1 : 0, false};
      case TokenKind::kLessEqual:
        return {Compare(lhs, rhs) <= 0 ? 1 : 0, false};
      case TokenKind::kGreaterEqual:
        return {Compare(lhs, rhs) >= 0 ? 1 : 0, false};
      case TokenKind::kEqualEqual:
        return {lhs.value == rhs.value ? 1 : 0, false};
      case TokenKind::kExclaimEqual:
        return {lhs.value != rhs.value ? 1 : 0, false};
      case TokenKind::kAmpAmp:
        return {(lhs.value != 0 && rhs.value != 0) ? 1 : 0, false};
      case TokenKind::kPipePipe:
        return {(lhs.value != 0 || rhs.value != 0) ? 1 : 0, false};
      default:
        return {0, false};
    }
  }

  // -1 / 0 / 1, honoring unsigned comparison when either operand is unsigned.
  static int Compare(PPValue a, PPValue b) {
    if (a.is_unsigned || b.is_unsigned) {
      uint64_t au = static_cast<uint64_t>(a.value);
      uint64_t bu = static_cast<uint64_t>(b.value);
      return au < bu ? -1 : (au > bu ? 1 : 0);
    }
    return a.value < b.value ? -1 : (a.value > b.value ? 1 : 0);
  }

  std::function<Token()> fetch_;
  DiagnosticsEngine& diags_;
  Token peek_{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
};

// Recognized __has_builtin names.
bool IsKnownBuiltin(std::string_view name) {
  static constexpr const char* kBuiltins[] = {
      "__builtin_add_overflow",
      "__builtin_add_overflow_p",
      "__builtin_alloca",
      "__builtin_alloca_with_align",
      "__builtin_assume_aligned",
      "__builtin_bswap16",
      "__builtin_bswap32",
      "__builtin_bswap64",
      "__builtin_choose_expr",
      "__builtin_clz",
      "__builtin_clzl",
      "__builtin_clzll",
      "__builtin_constant_p",
      "__builtin_ctz",
      "__builtin_ctzl",
      "__builtin_ctzll",
      "__builtin_expect",
      "__builtin_expect_with_probability",
      "__builtin_ffs",
      "__builtin_ffsl",
      "__builtin_ffsll",
      "__builtin_frame_address",
      "__builtin_memcpy",
      "__builtin_memmove",
      "__builtin_memset",
      "__builtin_mul_overflow",
      "__builtin_mul_overflow_p",
      "__builtin_object_size",
      "__builtin_dynamic_object_size",
      "__builtin_parity",
      "__builtin_parityl",
      "__builtin_parityll",
      "__builtin_popcount",
      "__builtin_popcountl",
      "__builtin_popcountll",
      "__builtin_prefetch",
      "__builtin_return_address",
      "__builtin_strcmp",
      "__builtin_strlen",
      "__builtin_sub_overflow",
      "__builtin_sub_overflow_p",
      "__builtin_trap",
      "__builtin_types_compatible_p",
      "__builtin_unreachable",
      "__builtin_va_arg_pack",
      "__builtin_va_arg_pack_len",
      "__compiletime_error",
      "__compiletime_warning",
      "__has_attribute",  // clang allows the meta-check
      "__has_builtin",    // clang allows the meta-check
  };
  for (auto* b : kBuiltins) {
    if (name == b) return true;
  }
  return false;
}

// Recognized __has_attribute names.
bool IsKnownAttribute(std::string_view name) {
  static constexpr const char* kAttributes[] = {
      "alias",
      "aligned",
      "alloc_size",
      "always_inline",
      "artificial",
      "assume_aligned",
      "cleanup",
      "cold",
      "__cold__",
      "const",
      "constructor",
      "copy",
      "deprecated",
      "designated_init",
      "destructor",
      "error",
      "__error__",
      "externally_visible",
      "fallthrough",
      "__fallthrough__",
      "flatten",
      "force_align_arg_pointer",
      "format",
      "format_arg",
      "gnu_inline",
      "hot",
      "ifunc",
      "interrupt",
      "leaf",
      "malloc",
      "mode",
      "no_icf",
      "no_instrument_function",
      "no_profile_instrument_function",
      "no_reorder",
      "no_sanitize_address",
      "no_sanitize_thread",
      "no_sanitize_undefined",
      "noipa",
      "noinline",
      "noclone",
      "noomit_frame_pointer",
      "noplt",
      "noreturn",
      "notail_calls",
      "nonnull",
      "optimize",
      "packed",
      "patchable_function_entry",
      "pure",
      "remove_args",
      "retain",
      "returns_nonnull",
      "returns_twice",
      "section",
      "sentinel",
      "simd",
      "stack_protect",
      "symver",
      "target",
      "target_clones",
      "tiny",
      "tm_regparm",
      "transaction_callable",
      "transaction_cancel",
      "transaction_may_cancel",
      "unused",
      "used",
      "vec_type_hint",
      "visibility",
      "warning",
      "__warning__",
      "weak",
      "weakref",
      "warn_if_not_aligned",
  };
  for (auto* a : kAttributes) {
    if (name == a) return true;
  }
  return false;
}

// Strips a single layer of surrounding double underscores: "__c_alignas__"
// -> "c_alignas". __has_feature/__has_extension accept both spellings.
std::string_view StripUnderscores(std::string_view name) {
  if (name.size() >= 4 && name.substr(0, 2) == "__" &&
      name.substr(name.size() - 2) == "__") {
    return name.substr(2, name.size() - 4);
  }
  return name;
}

// C-language feature names recognised by __has_feature / __has_extension.
// bcc targets a C17 / GNU17 dialect, so every C11 feature is available. Both
// operators accept the bare name and the __name__ spelling.
bool HasCFeature(std::string_view raw) {
  std::string_view name = StripUnderscores(raw);
  static constexpr const char* kFeatures[] = {
      // C11 core features
      "c_atomic",
      "c_static_assert",
      "c_generic_selections",
      "c_alignas",
      "c_alignof",
      "c_thread_local",
      "c_generic_selection_with_controlling_type",
      // Other widely-queried features
      "attribute_overloadable",
      "c_nullable",
      "c_nullability",
  };
  for (auto* f : kFeatures) {
    if (name == f) return true;
  }
  return false;
}

}  // namespace

//===----------------------------------------------------------------------===//
// Token sources
//===----------------------------------------------------------------------===//

Token Preprocessor::LexDirectiveToken() {
  Token result{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  while (!cur_lexer_callback_(*this, result)) {
  }
  return result;
}

Token Preprocessor::LexConditionToken() {
  Token tok = LexDirectiveToken();

  // Intercept the `defined` operator so its operand is not macro-expanded.
  IdentifierInfo* ii = tok.GetIdentifierInfo();
  if (ii != nullptr && ii->GetPPKeyword() == PPKeyword::kDefined) {
    static const char kOne[] = "1";
    static const char kZero[] = "0";
    bool is_defined = EvaluateDefinedOperator();
    return Token{tok.GetLocation(), TokenKind::kNumericConstant,
                 is_defined ? kOne : kZero, 1u};
  }

  // Intercept __has_builtin / __has_attribute / __has_feature /
  // __has_extension to evaluate them inline.
  if (ii != nullptr) {
    PPKeyword kw = ii->GetPPKeyword();
    if (kw == PPKeyword::kHasBuiltin || kw == PPKeyword::kHasAttribute ||
        kw == PPKeyword::kHasFeature || kw == PPKeyword::kHasExtension) {
      return EvaluateHasExpression(tok);
    }
    if (kw == PPKeyword::kIsIdentifier) {
      return EvaluateIsIdentifier(tok);
    }
  }

  return tok;
}

bool Preprocessor::EvaluateDefinedOperator() {
  Token tok{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(tok);

  bool has_paren = tok.GetKind() == TokenKind::kLParen;
  if (has_paren) LexUnexpandedToken(tok);

  if (tok.GetKind() != TokenKind::kIdentifier) {
    diags_.Report(tok.GetLocation(), diag::err_pp_expected_macro_name);
    return false;
  }

  IdentifierInfo* ii = LookUpIdentifierInfo(tok);
  bool result = IsMacroDefined(ii);

  // Treat the pseudo-macros __has_builtin and __has_attribute as always
  // "defined" even though they have no MacroInfo entry (they are handled
  // as special keywords in the preprocessor).
  if (!result) {
    PPKeyword kw = ii->GetPPKeyword();
    result = (kw == PPKeyword::kHasBuiltin || kw == PPKeyword::kHasAttribute);
  }

  if (has_paren) {
    Token close{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
    LexUnexpandedToken(close);
    if (close.GetKind() != TokenKind::kRParen) {
      diags_.Report(close.GetLocation(), diag::err_pp_expected_rparen);
    }
  }
  return result;
}

Token Preprocessor::EvaluateHasExpression(Token& tok) {
  PPKeyword kw = tok.GetIdentifierInfo()->GetPPKeyword();

  // The keyword must be followed by '('.
  Token open{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(open);
  if (open.GetKind() != TokenKind::kLParen) {
    // Not a function-like call — return the original identifier token. The
    // expression evaluator will treat it as an unknown identifier (value 0).
    return tok;
  }

  // Read the argument token.
  Token arg{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(arg);

  bool found = false;
  if (arg.GetKind() == TokenKind::kIdentifier) {
    IdentifierInfo* arg_ii = LookUpIdentifierInfo(arg);
    std::string_view name = arg_ii->GetName();
    switch (kw) {
      case PPKeyword::kHasBuiltin:
        found = IsKnownBuiltin(name);
        break;
      case PPKeyword::kHasAttribute:
        found = IsKnownAttribute(name);
        break;
      case PPKeyword::kHasFeature:
      case PPKeyword::kHasExtension:
        found = HasCFeature(name);
        break;
      default:
        break;
    }
  }

  // Consume the closing ')' (or report an error if missing).
  Token close{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(close);
  if (close.GetKind() != TokenKind::kRParen) {
    diags_.Report(close.GetLocation(), diag::err_pp_expected_rparen);
  }

  static const char kOne[] = "1";
  static const char kZero[] = "0";
  return Token{tok.GetLocation(), TokenKind::kNumericConstant,
               found ? kOne : kZero, 1u};
}

Token Preprocessor::EvaluateIsIdentifier(Token& tok) {
  // The keyword must be followed by '('.
  Token open{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(open);

  if (open.GetKind() != TokenKind::kLParen) {
    return tok;
  }

  Token arg{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(arg);

  bool is_identifier = false;

  if (arg.GetKind() == TokenKind::kIdentifier) {
    IdentifierInfo* arg_ii = LookUpIdentifierInfo(arg);
    // GNU-extension keywords (active in gnu* modes) are not identifiers.
    std::string_view name = arg_ii->GetName();
    if (name == "typeof" || name == "asm" || name == "__asm" ||
        name == "__asm__" || name == "__typeof" || name == "__typeof__" ||
        name == "__inline" || name == "__inline__" || name == "__restrict" ||
        name == "__restrict__" || name == "__volatile" ||
        name == "__volatile__" || name == "__alignof" ||
        name == "__alignof__") {
      is_identifier = false;
    } else {
      is_identifier = true;
    }
  } else {
    // A keyword token kind (kRestrict, kInline, _Bool, ...) is not an
    // identifier.
    is_identifier = false;
  }

  Token close{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(close);

  if (close.GetKind() != TokenKind::kRParen) {
    diags_.Report(close.GetLocation(), diag::err_pp_expected_rparen);
  }

  static const char kOne[] = "1";
  static const char kZero[] = "0";

  return Token{tok.GetLocation(), TokenKind::kNumericConstant,
               is_identifier ? kOne : kZero, 1u};
}

//===----------------------------------------------------------------------===//
// #if / #elif expression
//===----------------------------------------------------------------------===//

bool Preprocessor::EvaluateDirectiveExpression() {
  ExprParser parser([this] { return LexConditionToken(); }, diags_);
  PPValue value = parser.Parse();

  // Consume any tokens the grammar did not, up to the end of the line.
  parser.SkipToEnd();

  return value.value != 0;
}

}  // namespace bcc
