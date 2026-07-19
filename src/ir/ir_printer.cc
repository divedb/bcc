#include "bcc/ir/ir_printer.hh"

#include <bit>
#include <cassert>
#include <cstdio>

#include "bcc/ir/basic_block.hh"
#include "bcc/ir/function.hh"
#include "bcc/ir/instruction.hh"

namespace bcc::ir {

namespace {

std::string_view LinkageKeyword(Linkage linkage) {
  switch (linkage) {
    case Linkage::kExternal: return "";
    case Linkage::kInternal: return "internal ";
    case Linkage::kPrivate: return "private ";
  }
  return "";
}

/// c"..." escaping: printable ASCII stays literal except `"` and `\`;
/// everything else becomes \XX (uppercase hex), LLVM's own scheme.
std::string EscapeStringBytes(const std::string& bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (unsigned char c : bytes) {
    if (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else {
      static const char kHex[] = "0123456789ABCDEF";
      out.push_back('\\');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

std::string FormatFPBits(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llX",
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(value)));
  return buf;
}

}  // namespace

void IRPrinter::Print(const Module& module) {
  PrintStructTypes(module);

  bool printed_globals = false;
  for (const auto& gv : module.GetGlobals()) {
    PrintGlobal(*gv);
    printed_globals = true;
  }
  if (printed_globals) os_ << "\n";

  bool first = true;
  for (const auto& f : module.GetFunctions()) {
    if (f->IsDeclaration()) continue;
    if (!first) os_ << "\n";
    first = false;
    PrintFunctionDefinition(*f);
  }

  for (const auto& f : module.GetFunctions()) {
    if (!f->IsDeclaration() || !f->IsUsed()) continue;
    if (!first) os_ << "\n";
    first = false;
    PrintFunctionDeclaration(*f);
  }
}

void IRPrinter::PrintStructTypes(const Module& module) {
  // Struct types are registered on the context in creation order, which is
  // already a valid definition order for printing (LLVM allows forward
  // references in type definitions anyway).
  bool printed = false;
  for (const auto& st : module.GetContext().GetStructTypes()) {
    os_ << "%" << st->GetName() << " = type ";
    if (st->IsOpaque()) {
      os_ << "opaque\n";
      printed = true;
      continue;
    }
    os_ << "{";
    bool first = true;
    for (const Type* field : st->GetFields()) {
      os_ << (first ? " " : ", ") << TypeStr(field);
      first = false;
    }
    os_ << (first ? "}" : " }") << "\n";
    printed = true;
  }
  if (printed) os_ << "\n";
}

void IRPrinter::PrintGlobal(const GlobalVariable& gv) {
  os_ << "@" << gv.GetName() << " = ";
  if (gv.IsDeclaration()) {
    os_ << "external ";
  } else {
    os_ << LinkageKeyword(gv.GetLinkage());
  }
  if (gv.IsUnnamedAddr()) os_ << "unnamed_addr ";
  os_ << (gv.IsConst() ? "constant " : "global ");
  os_ << TypeStr(gv.GetValueType());
  if (!gv.IsDeclaration()) {
    os_ << " " << ConstantStr(gv.GetInitializer());
  }
  os_ << ", align " << gv.GetAlign() << "\n";
}

void IRPrinter::PrintFunctionDefinition(const Function& f) {
  NumberFunction(f);

  os_ << "define " << LinkageKeyword(f.GetLinkage())
      << TypeStr(f.GetReturnType()) << " @" << f.GetName() << "(";
  for (unsigned i = 0; i < f.GetNumArgs(); ++i) {
    if (i) os_ << ", ";
    const Argument* arg = f.GetArg(i);
    os_ << TypeStr(arg->GetType()) << " %" << names_[arg];
  }
  if (f.GetFunctionType()->IsVariadic()) {
    if (f.GetNumArgs()) os_ << ", ";
    os_ << "...";
  }
  os_ << ") {\n";

  bool first = true;
  for (const auto& bb : f.GetBlocks()) {
    if (!first) os_ << "\n";
    first = false;
    os_ << names_[bb.get()] << ":\n";
    PrintBlockBody(*bb);
  }
  os_ << "}\n";
}

void IRPrinter::PrintFunctionDeclaration(const Function& f) {
  const FunctionType* ft = f.GetFunctionType();
  os_ << "declare " << TypeStr(ft->GetReturnType()) << " @" << f.GetName()
      << "(";
  for (size_t i = 0; i < ft->GetParams().size(); ++i) {
    if (i) os_ << ", ";
    os_ << TypeStr(ft->GetParams()[i]);
  }
  if (ft->IsVariadic()) {
    if (!ft->GetParams().empty()) os_ << ", ";
    os_ << "...";
  }
  os_ << ")\n";
}

void IRPrinter::PrintBlockBody(const BasicBlock& bb) {
  for (const auto& inst : bb.GetInstructions()) {
    os_ << "  ";
    PrintInstruction(*inst);
    os_ << "\n";
  }
}

void IRPrinter::PrintInstruction(const Instruction& inst) {
  if (!inst.GetType()->IsVoid()) {
    os_ << "%" << names_[&inst] << " = ";
  }

  switch (inst.GetOpcode()) {
    case Opcode::kAlloca: {
      const auto& a = *inst.As<AllocaInst>();
      os_ << "alloca " << TypeStr(a.GetAllocatedType()) << ", align "
          << a.GetAlign();
      return;
    }
    case Opcode::kLoad: {
      const auto& l = *inst.As<LoadInst>();
      os_ << "load " << TypeStr(l.GetType()) << ", "
          << TypedRef(l.GetPointer()) << ", align " << l.GetAlign();
      return;
    }
    case Opcode::kStore: {
      const auto& s = *inst.As<StoreInst>();
      os_ << "store " << TypedRef(s.GetValueOperand()) << ", "
          << TypedRef(s.GetPointer()) << ", align " << s.GetAlign();
      return;
    }
    case Opcode::kGEP: {
      const auto& g = *inst.As<GEPInst>();
      os_ << "getelementptr inbounds " << TypeStr(g.GetSourceType()) << ", "
          << TypedRef(g.GetBase());
      for (unsigned i = 0; i < g.GetNumIndices(); ++i) {
        os_ << ", " << TypedRef(g.GetIndex(i));
      }
      return;
    }
    case Opcode::kICmp:
    case Opcode::kFCmp: {
      const auto& c = *inst.As<CmpInst>();
      os_ << GetOpcodeName(c.GetOpcode()) << " "
          << GetPredicateName(c.GetPredicate()) << " "
          << TypedRef(c.GetLHS()) << ", " << Ref(c.GetRHS());
      return;
    }
    case Opcode::kBr: {
      const auto& b = *inst.As<BranchInst>();
      os_ << "br label %" << names_[b.GetDest()];
      return;
    }
    case Opcode::kCondBr: {
      const auto& b = *inst.As<BranchInst>();
      os_ << "br " << TypedRef(b.GetCondition()) << ", label %"
          << names_[b.GetTrueDest()] << ", label %"
          << names_[b.GetFalseDest()];
      return;
    }
    case Opcode::kSwitch: {
      const auto& s = *inst.As<SwitchInst>();
      os_ << "switch " << TypedRef(s.GetCondition()) << ", label %"
          << names_[s.GetDefaultDest()] << " [\n";
      for (unsigned i = 0; i < s.GetNumCases(); ++i) {
        os_ << "    " << TypedRef(s.GetCaseValue(i)) << ", label %"
            << names_[s.GetCaseDest(i)] << "\n";
      }
      os_ << "  ]";
      return;
    }
    case Opcode::kRet: {
      const auto& r = *inst.As<RetInst>();
      if (const Value* v = r.GetReturnValue()) {
        os_ << "ret " << TypedRef(v);
      } else {
        os_ << "ret void";
      }
      return;
    }
    case Opcode::kUnreachable:
      os_ << "unreachable";
      return;
    case Opcode::kPhi: {
      const auto& p = *inst.As<PhiNode>();
      os_ << "phi " << TypeStr(p.GetType());
      for (unsigned i = 0; i < p.GetNumIncoming(); ++i) {
        os_ << (i ? ", [ " : " [ ") << Ref(p.GetIncomingValue(i)) << ", %"
            << names_[p.GetIncomingBlock(i)] << " ]";
      }
      return;
    }
    case Opcode::kCall: {
      const auto& c = *inst.As<CallInst>();
      const FunctionType* ft = c.GetFunctionType();
      // Variadic calls must spell out the full function type.
      os_ << "call "
          << (ft->IsVariadic() ? TypeStr(ft) : TypeStr(ft->GetReturnType()))
          << " " << Ref(c.GetCallee()) << "(";
      for (unsigned i = 0; i < c.GetNumArgs(); ++i) {
        if (i) os_ << ", ";
        os_ << TypedRef(c.GetArg(i));
      }
      os_ << ")";
      return;
    }
    default:
      break;
  }

  if (inst.IsBinaryOp()) {
    const auto& b = *inst.As<BinaryOperator>();
    os_ << GetOpcodeName(b.GetOpcode()) << " " << TypedRef(b.GetLHS()) << ", "
        << Ref(b.GetRHS());
    return;
  }
  if (inst.IsCast()) {
    const auto& c = *inst.As<CastInst>();
    os_ << GetOpcodeName(c.GetOpcode()) << " " << TypedRef(c.GetOperandValue())
        << " to " << TypeStr(c.GetType());
    return;
  }
  assert(false && "unhandled instruction in printer");
}

std::string IRPrinter::TypeStr(const Type* t) {
  switch (t->GetKind()) {
    case TypeKind::kVoid: return "void";
    case TypeKind::kInteger:
      return "i" + std::to_string(t->As<IntegerType>()->GetBits());
    case TypeKind::kFloat: return "float";
    case TypeKind::kDouble: return "double";
    case TypeKind::kPointer: return "ptr";
    case TypeKind::kArray: {
      const auto* at = t->As<ArrayType>();
      return "[" + std::to_string(at->GetNumElements()) + " x " +
             TypeStr(at->GetElementType()) + "]";
    }
    case TypeKind::kStruct: return "%" + t->As<StructType>()->GetName();
    case TypeKind::kFunction: {
      const auto* ft = t->As<FunctionType>();
      std::string s = TypeStr(ft->GetReturnType()) + " (";
      for (size_t i = 0; i < ft->GetParams().size(); ++i) {
        if (i) s += ", ";
        s += TypeStr(ft->GetParams()[i]);
      }
      if (ft->IsVariadic()) {
        if (!ft->GetParams().empty()) s += ", ";
        s += "...";
      }
      return s + ")";
    }
  }
  return "";
}

std::string IRPrinter::Ref(const Value* v) {
  if (v->IsConstant()) return ConstantStr(v->As<Constant>());
  return "%" + names_[v];
}

std::string IRPrinter::TypedRef(const Value* v) {
  return TypeStr(v->GetType()) + " " + Ref(v);
}

std::string IRPrinter::ConstantStr(const Constant* c) {
  switch (c->GetValueKind()) {
    case ValueKind::kConstantInt: {
      const auto* ci = c->As<ConstantInt>();
      if (ci->GetType()->As<IntegerType>()->GetBits() == 1) {
        return ci->GetValue() & 1 ? "true" : "false";
      }
      return std::to_string(ci->GetSExtValue());
    }
    case ValueKind::kConstantFP:
      return FormatFPBits(c->As<ConstantFP>()->GetValue());
    case ValueKind::kConstantNullPtr:
      return "null";
    case ValueKind::kConstantUndef:
      return "undef";
    case ValueKind::kConstantAggregateZero:
      return "zeroinitializer";
    case ValueKind::kConstantString:
      return "c\"" + EscapeStringBytes(c->As<ConstantString>()->GetBytes()) +
             "\"";
    case ValueKind::kConstantAggregate: {
      const auto* agg = c->As<ConstantAggregate>();
      bool is_struct = agg->GetType()->IsStruct();
      std::string s = is_struct ? "{ " : "[ ";
      const auto& elems = agg->GetElements();
      for (size_t i = 0; i < elems.size(); ++i) {
        if (i) s += ", ";
        s += TypeStr(elems[i]->GetType()) + " " + ConstantStr(elems[i]);
      }
      return s + (is_struct ? " }" : " ]");
    }
    case ValueKind::kGlobalVariable:
    case ValueKind::kFunction:
      return "@" + c->GetName();
    default:
      assert(false && "not a printable constant");
      return "";
  }
}

void IRPrinter::NumberFunction(const Function& f) {
  names_.clear();
  name_counts_.clear();
  next_slot_ = 0;

  for (unsigned i = 0; i < f.GetNumArgs(); ++i) AssignName(f.GetArg(i));
  for (const auto& bb : f.GetBlocks()) {
    AssignName(bb.get());
    for (const auto& inst : bb->GetInstructions()) {
      if (!inst->GetType()->IsVoid()) AssignName(inst.get());
    }
  }
}

std::string IRPrinter::AssignName(const Value* v) {
  std::string name;
  if (v->HasName()) {
    unsigned count = name_counts_[v->GetName()]++;
    name = count == 0 ? v->GetName()
                      : v->GetName() + std::to_string(count);
  } else {
    // Unnamed values and blocks share one numbering, like LLVM.
    name = std::to_string(next_slot_++);
  }
  names_[v] = name;
  return name;
}

}  // namespace bcc::ir
