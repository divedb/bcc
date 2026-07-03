#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "bcc/as/diagnostic.hh"
#include "bcc/as/encoder.hh"
#include "bcc/as/lexer.hh"
#include "bcc/as/mccontext.hh"
#include "bcc/as/operand.hh"
#include "bcc/as/token.hh"

namespace bcc::as {

/// \brief Recursive-descent parser for one assembly translation unit.
///
/// Drives the lexer, resolves labels/symbols against the \ref MCContext,
/// evaluates directive and operand expressions, and dispatches instructions to
/// the \ref Encoder. Errors are reported through \ref AsmDiag; parsing recovers
/// to the next statement so multiple errors surface in one run.
class Parser {
 public:
  Parser(Lexer& lexer, MCContext& ctx, AsmDiag& diag)
      : lexer_(lexer), ctx_(ctx), diag_(diag), encoder_(ctx, diag) {
    Advance();
  }

  /// Parses the entire input. \return true if no errors were reported.
  bool Run();

 private:
  //=== token stream ===//
  void Advance() { cur_ = lexer_.Next(); }
  bool Accept(TokKind k) {
    if (cur_.Is(k)) {
      Advance();
      return true;
    }
    return false;
  }
  bool Expect(TokKind k, const char* what);
  void SkipToEndOfStatement();

  //=== statements ===//
  void ParseStatement();
  void ParseDirective();
  void ParseInstruction(std::string_view mnemonic, uint32_t loc);
  void ParseAssignment(std::string_view name);

  //=== operands ===//
  bool ParseOperand(Operand& out);
  bool ParseRegister(Reg& out);
  bool ParseMemoryTail(Operand& out);  // starting at '('

  //=== expressions (produce MCValue) ===//
  bool ParseExpr(MCValue& out);
  bool ParseBinary(int min_prec, MCValue& out);
  bool ParseUnary(MCValue& out);
  bool ParsePrimary(MCValue& out);

  //=== directive helpers ===//
  void EmitData(unsigned size, bool sign_extend);
  void DoAscii(bool nul_terminate);
  void DoAlign(bool power_of_two);
  void DoComm(bool local);
  Symbol* MakeDotSymbol();  // an anonymous symbol at the current location

  void Error(uint32_t off, std::string_view msg) { diag_.Error(off, msg); }

  Lexer& lexer_;
  MCContext& ctx_;
  AsmDiag& diag_;
  Encoder encoder_;
  Token cur_;

  // Anonymous "." location symbols, kept out of the emitted symbol table.
  std::vector<std::unique_ptr<Symbol>> dot_symbols_;
};

}  // namespace bcc::as
