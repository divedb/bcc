#pragma once

#include <initializer_list>
#include <memory>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/lex/token.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/sema/decl_spec.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

/// \brief Recursive-descent parser for C11. Pulls tokens from the
///        Preprocessor and drives Sema's ActOn* interface; builds no AST
///        nodes itself (mirrors Clang's Parser).
class Parser {
 public:
  Parser(Preprocessor& pp, Sema& sema);

  Parser(const Parser&) = delete;
  Parser& operator=(const Parser&) = delete;

  /// \brief Parses the whole translation unit into the ASTContext's
  ///        TranslationUnitDecl.
  void ParseTranslationUnit();

 private:
  //===--------------------------------------------------------------------===//
  // Token stream primitives.
  //===--------------------------------------------------------------------===//

  const Token& Tok() const noexcept { return tok_; }
  bool TokIs(TokenKind k) const noexcept { return tok_.GetKind() == k; }

  /// \brief Consumes the current token, returning its location.
  SourceLocation ConsumeToken();

  /// \brief Consumes the current token if it is \p k; returns whether it was.
  bool TryConsumeToken(TokenKind k);

  /// \brief The token \p n ahead of the current one (n >= 1).
  Token GetLookAheadToken(unsigned n) { return pp_.LookAhead(n - 1); }
  Token NextToken() { return GetLookAheadToken(1); }

  /// \brief If the current token is \p k, consumes it; otherwise emits
  ///        \p diag_kind (with \p arg if nonempty) and returns true (error).
  bool ExpectAndConsume(TokenKind k, diag::DiagKind diag_kind,
                        std::string_view arg = {});
  bool ExpectAndConsumeSemi(diag::DiagKind diag_kind,
                            std::string_view arg = {});

  enum SkipFlags : unsigned {
    kStopAtSemi = 0x1,      ///< also stop (before) a ';' at nesting depth 0
    kStopBeforeMatch = 0x2  ///< leave the found token unconsumed
  };

  /// \brief Discards tokens until one of \p kinds (or EOF), balancing
  ///        parens/braces/brackets so a stop token inside nesting is skipped.
  ///        Returns true if a requested token was found.
  bool SkipUntil(std::initializer_list<TokenKind> kinds, unsigned flags = 0);

  /// \brief RAII scope: pushes a Scope for its lifetime.
  class ParseScope {
   public:
    ParseScope(Parser* p, unsigned flags) : parser_(p) {
      scope_ = std::make_unique<Scope>(p->sema_.GetCurScope(), flags);
      p->sema_.PushScope(scope_.get());
    }
    ~ParseScope() { parser_->sema_.PopScope(scope_.get()); }

    Scope* Get() noexcept { return scope_.get(); }

   private:
    Parser* parser_;
    std::unique_ptr<Scope> scope_;
  };

  /// \brief Tracks a matched delimiter pair, emitting the "to match this"
  ///        note when the closer is missing (Clang's
  ///        BalancedDelimiterTracker).
  class BalancedDelimiterTracker {
   public:
    BalancedDelimiterTracker(Parser& p, TokenKind open) : parser_(p),
                                                          open_kind_(open) {}

    /// Consumes the expected open token (asserts it is present).
    bool ConsumeOpen();
    /// Consumes the close token or diagnoses, noting the open location.
    bool ConsumeClose();

    SourceLocation GetOpenLocation() const noexcept { return open_loc_; }
    SourceLocation GetCloseLocation() const noexcept { return close_loc_; }

   private:
    Parser& parser_;
    TokenKind open_kind_;
    SourceLocation open_loc_;
    SourceLocation close_loc_;
  };

  DiagnosticBuilder Diag(SourceLocation loc, diag::DiagKind kind) {
    return sema_.GetDiagnostics().Report(loc, kind);
  }
  DiagnosticBuilder Diag(const Token& tok, diag::DiagKind kind) {
    return Diag(tok.GetLocation(), kind);
  }

  //===--------------------------------------------------------------------===//
  // Declarations (parse_decl.cc).
  //===--------------------------------------------------------------------===//

  void ParseExternalDeclaration();

  /// \brief True if the current token can begin a declaration specifier
  ///        (including a typedef-name, resolved through Sema).
  bool IsDeclarationSpecifier();

  /// \brief Whether \p tok begins a type-specifier/qualifier — the test used
  ///        inside parens to distinguish casts/compound literals from
  ///        expressions.
  bool IsTypeSpecifierStart(const Token& tok);

  void ParseDeclarationSpecifiers(DeclSpec& ds);

  /// \brief Parses declaration-specifiers declarator ... after the specifiers
  ///        are done: the init-declarator list or a function definition.
  ///        Returns the declarations made.
  std::vector<Decl*> ParseDeclGroup(DeclSpec& ds, DeclaratorContext context,
                                    SourceLocation* decl_end,
                                    bool allow_function_def);

  /// \brief Parses a full simple-declaration (specifiers + init-declarators
  ///        + ';'). Used at block scope and for-inits.
  std::vector<Decl*> ParseSimpleDeclaration(DeclaratorContext context,
                                            SourceLocation* decl_end);

  void ParseDeclarator(Declarator& d);
  void ParseDirectDeclarator(Declarator& d);
  void ParseParenDeclarator(Declarator& d);
  void ParseFunctionDeclarator(Declarator& d, SourceLocation lparen);
  void ParseBracketDeclarator(Declarator& d);
  void ParseParameterDeclarationClause(
      std::vector<DeclaratorChunk::ParamInfo>& params, bool& is_variadic,
      Scope* proto_scope);

  void ParseStructUnionSpecifier(DeclSpec& ds, SourceLocation kw_loc,
                                 bool is_union);
  void ParseStructDeclarationList(RecordDecl* record, Scope* member_scope);
  void ParseEnumSpecifier(DeclSpec& ds, SourceLocation kw_loc);
  void ParseEnumBody(EnumDecl* enum_decl);
  void ParseStaticAssertDeclaration(std::vector<Decl*>* out);

  /// \brief Parses a type-name (for casts, sizeof, _Generic, compound
  ///        literals). Returns a null QualType on error.
  QualType ParseTypeName();

  //===--------------------------------------------------------------------===//
  // Expressions (parse_expr.cc).
  //===--------------------------------------------------------------------===//

  ExprResult ParseExpression();
  ExprResult ParseAssignmentExpression();

  /// \brief Parses a conditional-expression and verifies it is an ICE context
  ///        (the caller evaluates it).
  ExprResult ParseConstantExpression();

  ExprResult ParseCastExpression(bool is_unary_context);
  ExprResult ParseRHSOfBinaryExpression(ExprResult lhs, int min_prec);
  ExprResult ParsePostfixExpressionSuffix(ExprResult lhs);
  ExprResult ParseSizeofAlignofExpression();
  ExprResult ParseGenericSelectionExpression();
  ExprResult ParseStringLiteralExpression();

  /// What a parenthesized construct after '(' turned out to be.
  enum class ParenParseOption : uint8_t {
    kExpression,       ///< (expr)
    kCompoundLiteral,  ///< (type){...}
    kCastExpr          ///< (type)expr
  };

  ExprResult ParseParenExpression(ParenParseOption& parse_kind,
                                  QualType& cast_type,
                                  SourceLocation& rparen_loc);

  //===--------------------------------------------------------------------===//
  // Statements (parse_stmt.cc).
  //===--------------------------------------------------------------------===//

  StmtResult ParseStatement();
  StmtResult ParseCompoundStatement(unsigned scope_flags);
  StmtResult ParseIfStatement();
  StmtResult ParseSwitchStatement();
  StmtResult ParseWhileStatement();
  StmtResult ParseDoStatement();
  StmtResult ParseForStatement();
  StmtResult ParseReturnStatement();
  StmtResult ParseCaseStatement();
  StmtResult ParseDefaultStatement();
  StmtResult ParseGotoStatement();
  StmtResult ParseLabeledStatement();
  StmtResult ParseExprStatement();

  /// \brief Parses `( expression )` for if/while/switch conditions.
  ExprResult ParseParenExprOrCondition(SourceLocation* rparen_loc = nullptr);

  //===--------------------------------------------------------------------===//
  // Initializers (parse_init.cc).
  //===--------------------------------------------------------------------===//

  ExprResult ParseInitializer();
  ExprResult ParseBraceInitializer();
  ExprResult ParseInitializerWithPotentialDesignator();

  //===--------------------------------------------------------------------===//
  // Function definitions (parse_decl.cc).
  //===--------------------------------------------------------------------===//

  Decl* ParseFunctionDefinition(Declarator& d);

  Preprocessor& pp_;
  Sema& sema_;
  Token tok_;
};

}  // namespace bcc
