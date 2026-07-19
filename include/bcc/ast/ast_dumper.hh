#pragma once

#include <ostream>
#include <string>

namespace bcc {

class Decl;
class Expr;
class SourceManager;
class Stmt;

/// \brief Prints a Clang-style indented textual dump of the AST, for tests
///        and the -ast-dump driver mode.
class ASTDumper {
 public:
  explicit ASTDumper(std::ostream& os, const SourceManager* sm = nullptr)
      : os_(os), sm_(sm) {}

  void Dump(const Decl* d) { DumpDecl(d, 0); }
  void Dump(const Stmt* s) { DumpStmt(s, 0); }

 private:
  void DumpDecl(const Decl* d, unsigned depth);
  void DumpStmt(const Stmt* s, unsigned depth);
  void Indent(unsigned depth);
  void PrintLoc(const Stmt* s);

  std::ostream& os_;
  const SourceManager* sm_;
};

}  // namespace bcc
