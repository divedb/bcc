#pragma once

#include <cstdint>
#include <string_view>

namespace bcc {

enum class TokenKind : uint16_t {
  //===----------------------------------------------------------------------===//
  // Keyword
  //===----------------------------------------------------------------------===//
  kAuto,          // auto
  kBreak,         // break
  kCase,          // case
  kChar,          // char
  kConst,         // const
  kContinue,      // continue
  kDefault,       // default
  kDo,            // do
  kDouble,        // double
  kElse,          // else
  kEnum,          // enum
  kExtern,        // extern
  kFloat,         // float
  kFor,           // for
  kGoto,          // goto
  kIf,            // if
  kInline,        // inline
  kInt,           // int
  kLong,          // long
  kRegister,      // register
  kRestrict,      // restrict
  kReturn,        // return
  kShort,         // short
  kSigned,        // signed
  kSizeof,        // sizeof
  kStatic,        // static
  kStruct,        // struct
  kSwitch,        // switch
  kTypedef,       // typedef
  kUnion,         // union
  kUnsigned,      // unsigned
  kVoid,          // void
  kVolatile,      // volatile
  kWhile,         // while
  kAlignas,       // _Alignas
  kAlignof,       // _Alignof
  kAtomic,        // _Atomic
  kBool,          // _Bool
  kComplex,       // _Complex
  kGeneric,       // _Generic
  kImaginary,     // _Imaginary
  kNoreturn,      // _Noreturn
  kStaticAssert,  // _Static_assert
  kThreadLocal,   // _Thread_local

  //===----------------------------------------------------------------------===//
  // Punctuator
  //===----------------------------------------------------------------------===//
  kLSquare,              // [
  kRSquare,              // ]
  kLParen,               // (
  kRParen,               // )
  kLBrace,               // {
  kRBrace,               // }
  kPeriod,               // .
  kArrow,                // ->
  kPlusPlus,             // ++
  kMinusMinus,           // --
  kAmp,                  // &
  kStar,                 // *
  kPlus,                 // +
  kMinus,                // -
  kTilde,                // ~
  kExclaim,              // !
  kSlash,                // /
  kPercent,              // %
  kLessLess,             // <<
  kGreaterGreater,       // >>
  kLess,                 // <
  kGreater,              // >
  kLessEqual,            // <=
  kGreaterEqual,         // >=
  kEqualEqual,           // ==
  kExclaimEqual,         // !=
  kCaret,                // ^
  kPipe,                 // |
  kAmpAmp,               // &&
  kPipePipe,             // ||
  kQuestion,             // ?
  kColon,                // :
  kSemi,                 // ;
  kEllipsis,             // ...
  kEqual,                // =
  kStarEqual,            // *=
  kSlashEqual,           // /=
  kPercentEqual,         // %=
  kPlusEqual,            // +=
  kMinusEqual,           // -=
  kLessLessEqual,        // <<=
  kGreaterGreaterEqual,  // >>=
  kAmpEqual,             // &=
  kCaretEqual,           // ^=
  kPipeEqual,            // |=
  kComma,                // ,
  kHash,                 // #
  kHashHash,             // ##

  //===----------------------------------------------------------------------===//
  // Identifier
  //===----------------------------------------------------------------------===//
  kIdentifier,

  //===----------------------------------------------------------------------===//
  // Character Constant
  //===----------------------------------------------------------------------===//
  kCharConstant,       // 'a'
  kWideCharConstant,   // L'a'
  kUtf16CharConstant,  // u'a'
  kUtf32CharConstant,  // U'a'

  //===----------------------------------------------------------------------===//
  // String Literal
  //===----------------------------------------------------------------------===//
  kStringLiteral,       // "abc"
  kUtf8StringLiteral,   // u8"abc"
  kUtf16StringLiteral,  // u"abc"
  kUtf32StringLiteral,  // U"abc"
  kWideStringLiteral,   // L"abc"

  //===----------------------------------------------------------------------===//
  // pp-number
  //===----------------------------------------------------------------------===//
  kNumericConstant,  // integer, float, double

  kNewLine,     // '\n'
  kWhitespace,  // ' ', '\t', '\v', '\f'
  kComment,     // // comment or /* comment */
  kUnknown,     // Any unrecognized token
  kEOF,         // End of file/input
};

inline constexpr std::string_view kTokenKindNames[] = {
    "auto",            // kAuto
    "break",           // kBreak
    "case",            // kCase
    "char",            // kChar
    "const",           // kConst
    "continue",        // kContinue
    "default",         // kDefault
    "do",              // kDo
    "double",          // kDouble
    "else",            // kElse
    "enum",            // kEnum
    "extern",          // kExtern
    "float",           // kFloat
    "for",             // kFor
    "goto",            // kGoto
    "if",              // kIf
    "inline",          // kInline
    "int",             // kInt
    "long",            // kLong
    "register",        // kRegister
    "restrict",        // kRestrict
    "return",          // kReturn
    "short",           // kShort
    "signed",          // kSigned
    "sizeof",          // kSizeof
    "static",          // kStatic
    "struct",          // kStruct
    "switch",          // kSwitch
    "typedef",         // kTypedef
    "union",           // kUnion
    "unsigned",        // kUnsigned
    "void",            // kVoid
    "volatile",        // kVolatile
    "while",           // kWhile
    "_Alignas",        // kAlignas
    "_Alignof",        // kAlignof
    "_Atomic",         // kAtomic
    "_Bool",           // kBool
    "_Complex",        // kComplex
    "_Generic",        // kGeneric
    "_Imaginary",      // kImaginary
    "_Noreturn",       // kNoreturn
    "_Static_assert",  // kStaticAssert
    "_Thread_local",   // kThreadLocal

    "[",    // kLSquare
    "]",    // kRSquare
    "(",    // kLParen
    ")",    // kRParen
    "{",    // kLBrace
    "}",    // kRBrace
    ".",    // kPeriod
    "->",   // kArrow
    "++",   // kPlusPlus
    "--",   // kMinusMinus
    "&",    // kAmp
    "*",    // kStar
    "+",    // kPlus
    "-",    // kMinus
    "~",    // kTilde
    "!",    // kExclaim
    "/",    // kSlash
    "%",    // kPercent
    "<<",   // kLessLess
    ">>",   // kGreaterGreater
    "<",    // kLess
    ">",    // kGreater
    "<=",   // kLessEqual
    ">=",   // kGreaterEqual
    "==",   // kEqualEqual
    "!=",   // kExclaimEqual
    "^",    // kCaret
    "|",    // kPipe
    "&&",   // kAmpAmp
    "||",   // kPipePipe
    "?",    // kQuestion
    ":",    // kColon
    ";",    // kSemi
    "...",  // kEllipsis
    "=",    // kEqual
    "*=",   // kStarEqual
    "/=",   // kSlashEqual
    "%=",   // kPercentEqual
    "+=",   // kPlusEqual
    "-=",   // kMinusEqual
    "<<=",  // kLessLessEqual
    ">>=",  // kGreaterGreaterEqual
    "&=",   // kAmpEqual
    "^=",   // kCaretEqual
    "|=",   // kPipeEqual
    ",",    // kComma
    "#",    // kHash
    "##",   // kHashHash

    "identifier",            // kIdentifier
    "char_const",            // kCharConstant
    "wide_char_const",       // kWideCharConstant
    "utf16_char_const",      // kUtf16CharConstant
    "utf32_char_const",      // kUtf32CharConstant
    "string_literal",        // kStringLiteral
    "utf8_string_literal",   // kUtf8StringLiteral
    "utf16_string_literal",  // kUtf16StringLiteral
    "utf32_string_literal",  // kUtf32StringLiteral
    "wide_string_literal",   // kWideStringLiteral
    "numeric_constant",      // kNumericConstant
    "newline",               // kNewLine
    "whitespace",            // kWhitespace
    "comment",               // kComment
    "unknown",               // kUnknown
    "eof",                   // kEOF
};

static_assert(sizeof(kTokenKindNames) / sizeof(kTokenKindNames[0]) ==
              static_cast<int>(TokenKind::kEOF) + 1);

constexpr std::string_view TokenKindName(TokenKind kind) {
  auto i = static_cast<int>(kind);

  return kTokenKindNames[i];
}

constexpr bool IsCharLiteralKind(TokenKind kind) noexcept {
  return kind == TokenKind::kCharConstant ||
         kind == TokenKind::kWideCharConstant ||
         kind == TokenKind::kUtf16CharConstant ||
         kind == TokenKind::kUtf32CharConstant;
}

constexpr bool IsStringLiteralKind(TokenKind kind) noexcept {
  return kind == TokenKind::kStringLiteral ||
         kind == TokenKind::kUtf8StringLiteral ||
         kind == TokenKind::kUtf16StringLiteral ||
         kind == TokenKind::kUtf32StringLiteral ||
         kind == TokenKind::kWideStringLiteral;
}

}  // namespace bcc