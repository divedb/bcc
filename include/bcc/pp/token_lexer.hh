#pragma once

#include <optional>
#include <vector>

#include "bcc/basic/file_id.hh"
#include "bcc/lex/token.hh"

namespace bcc {

class MacroArgs;
class MacroInfo;
class Preprocessor;

/// \brief Replays the tokens of a macro expansion instead of lexing characters.
///
/// A TokenLexer walks a token list — a macro's replacement list (object-like),
/// the substituted result of a function-like invocation, or an arbitrary token
/// stream — handing each token back with an appropriate source location so
/// diagnostics can report both where a token was written (spelling) and where
/// the macro was invoked (expansion).
///
/// For macro modes it disables its macro for the expansion's lifetime so a
/// self-reference terminates, and re-enables it on destruction. Stream mode
/// (used for argument pre-expansion and one-token pushback) carries no macro
/// and emits tokens with their original locations untouched.
class TokenLexer {
 public:
  /// Object-like or function-like macro expansion.
  ///
  /// \param macro_name_tok The identifier token that triggered the expansion;
  ///                       its spacing flags are carried onto the first output
  ///                       token and its location becomes the expansion point.
  /// \param macro          The macro being expanded (disabled for this lexer's
  ///                       lifetime).
  /// \param args           Actual arguments for a function-like macro, else
  ///                       nullptr. Not retained past construction.
  /// \param pp             Owning preprocessor.
  TokenLexer(const Token& macro_name_tok, MacroInfo* macro, MacroArgs* args,
             Preprocessor& pp);

  /// Raw token-stream mode: replays \p tokens verbatim, with no macro and no
  /// location rewriting. Used for argument pre-expansion and token pushback.
  TokenLexer(std::vector<Token> tokens, Preprocessor& pp);

  ~TokenLexer();

  TokenLexer(const TokenLexer&) = delete;
  TokenLexer& operator=(const TokenLexer&) = delete;

  /// \brief Produces the next token, or returns false when exhausted.
  bool Lex(Token& result);

  bool IsAtEnd() const noexcept { return cur_token_ >= num_tokens_; }
  unsigned NumTokens() const noexcept { return num_tokens_; }
  bool IsStream() const noexcept { return is_stream_; }
  bool FirstTokenHasLeadingSpace() const noexcept {
    return first_token_has_leading_space_;
  }
  /// True when this expansion ended with a leading space that no output token
  /// carried, so it must propagate to whatever token follows the expansion:
  /// either an empty expansion whose invocation had a leading space, or a
  /// trailing empty parameter whose space was never consumed.
  bool HasUnconsumedLeadingSpace() const noexcept {
    if (num_tokens_ == 0)
      return first_token_has_leading_space_ || pending_leading_space_;

    return pending_leading_space_;
  }

  /// True when an empty expansion was at the start of a line, so that flag
  /// must propagate to the token following the expansion.
  bool HasUnconsumedStartOfLine() const noexcept {
    return num_tokens_ == 0 && first_token_at_start_of_line_;
  }

  MacroInfo* GetMacro() const noexcept { return macro_; }

  /// Mark the next token produced by Lex() to inherit a leading space. Used
  /// when a nested (child) expansion that had a leading space expanded to
  /// nothing: the space must flow to this lexer's following output token.
  void InheritLeadingSpaceForNext() noexcept { inherit_leading_space_ = true; }
  /// As above, but for the start-of-line flag (an empty nested expansion at
  /// the start of a line must carry that onto the following token).
  void InheritStartOfLineForNext() noexcept { inherit_start_of_line_ = true; }

 private:
  /// Builds the substituted replacement list into owned_tokens_ for a
  /// function-like macro, or an object-like macro that uses `##`.
  void BuildExpansion(MacroArgs* args);

  /// Appends \p produced to \p out, pasting onto out.back() when \p pending.
  void AppendOrPaste(std::vector<Token>& out,
                     const std::vector<Token>& produced, bool pending_paste);

  /// Concatenates two token spellings and re-lexes them into one token; returns
  /// nullopt if the concatenation is not a single valid token.
  std::optional<Token> PasteTokens(const Token& lhs, const Token& rhs);

  /// Builds a string-literal token from an argument's unexpanded tokens.
  Token StringifyArgument(const std::vector<Token>& arg_tokens);

  /// Parameter index of \p tok in the macro, or -1 if it is not a parameter.
  int ParameterIndex(const Token& tok) const;

  Preprocessor& pp_;
  MacroInfo* macro_;  // null in stream mode

  /// Tokens owned by this lexer (function-like result / paste result / stream);
  /// empty for the object-like fast path, which points into the MacroInfo.
  std::vector<Token> owned_tokens_;

  const Token* tokens_;
  unsigned num_tokens_;
  unsigned cur_token_ = 0;

  /// True in raw stream mode: emit tokens as-is without location rewriting.
  bool is_stream_ = false;

  /// FileID of the macro-expansion SLocEntry (macro modes only).
  FileID expansion_fid_;

  bool first_token_at_start_of_line_ = false;
  bool first_token_has_leading_space_ = false;

  /// When a parameter with leading space expands to nothing (empty argument),
  /// this flag is set so the next emitted token inherits that leading space.
  bool pending_leading_space_ = false;

  /// Set by InheritLeadingSpaceForNext(): apply a leading space to the next
  /// token emitted by Lex() (used to propagate spacing across an empty nested
  /// expansion).
  bool inherit_leading_space_ = false;
  bool inherit_start_of_line_ = false;
};

}  // namespace bcc
