#pragma once

#include <map>
#include <ostream>
#include <string>

#include "bcc/ir/module.hh"

namespace bcc::ir {

/// \brief Prints a Module as textual LLVM IR (valid input to llvm-as/clang
///        for the emitted subset). Runs a per-function slot tracker that
///        assigns %0, %1, ... to unnamed values and de-duplicates name hints
///        (%x.addr, %x.addr1), like LLVM's asm writer.
class IRPrinter {
 public:
  explicit IRPrinter(std::ostream& os) : os_(os) {}

  void Print(const Module& module);

 private:
  void PrintStructTypes(const Module& module);
  void PrintGlobal(const GlobalVariable& gv);
  void PrintFunctionDefinition(const Function& f);
  void PrintFunctionDeclaration(const Function& f);
  void PrintBlockBody(const BasicBlock& bb);
  void PrintInstruction(const Instruction& inst);

  /// Renders a type ("i32", "ptr", "[4 x i8]", "%struct.S", ...).
  std::string TypeStr(const Type* t);
  /// Renders a value reference ("%x.addr", "@g", "5", "null", ...).
  std::string Ref(const Value* v);
  /// "type ref" (e.g. "i32 %0").
  std::string TypedRef(const Value* v);
  /// Renders a constant's text (no type prefix).
  std::string ConstantStr(const Constant* c);

  /// Assigns printed names to args, blocks and instruction results of \p f.
  void NumberFunction(const Function& f);
  std::string AssignName(const Value* v);

  std::ostream& os_;
  std::map<const Value*, std::string> names_;   ///< per-function slots
  std::map<std::string, unsigned> name_counts_; ///< hint de-duplication
  unsigned next_slot_ = 0;
};

}  // namespace bcc::ir
