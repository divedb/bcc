#include "bcc/as/parser.hh"

#include <algorithm>
#include <cctype>
#include <string>

#include "bcc/as/elf.hh"

namespace bcc::as {

namespace {

int PrecOf(TokKind k) {
  switch (k) {
    case TokKind::kPipe: return 1;
    case TokKind::kCaret: return 2;
    case TokKind::kAmp: return 3;
    case TokKind::kLShift:
    case TokKind::kRShift: return 4;
    case TokKind::kPlus:
    case TokKind::kMinus: return 5;
    case TokKind::kStar:
    case TokKind::kSlash:
    case TokKind::kPercent: return 6;
    default: return -1;
  }
}

RelModifier ParseModifier(std::string_view s) {
  std::string u(s);
  std::transform(u.begin(), u.end(), u.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (u == "PLT") return RelModifier::kPLT;
  if (u == "GOTPCREL") return RelModifier::kGOTPCREL;
  if (u == "GOT") return RelModifier::kGOT;
  if (u == "GOTOFF") return RelModifier::kGOTOFF;
  if (u == "TPOFF") return RelModifier::kTPOFF;
  return RelModifier::kNone;
}

/// Folds an MCValue to a constant if possible (a pure constant, or a
/// same-section defined symbol difference).
bool ResolveConstant(const MCValue& v, int64_t& out) {
  if (v.IsConstant()) {
    out = v.addend;
    return true;
  }
  if (v.sym && v.sym2 && v.sym->defined && v.sym2->defined &&
      v.sym->section == v.sym2->section) {
    out = static_cast<int64_t>(v.sym->offset) -
          static_cast<int64_t>(v.sym2->offset) + v.addend;
    return true;
  }
  return false;
}

}  // namespace

bool Parser::Expect(TokKind k, const char* what) {
  if (cur_.Is(k)) {
    Advance();
    return true;
  }
  Error(cur_.offset, std::string("expected ") + what);
  return false;
}

void Parser::SkipToEndOfStatement() {
  while (!cur_.IsEndOfStatement()) Advance();
}

Symbol* Parser::MakeDotSymbol() {
  auto s = std::make_unique<Symbol>();
  s->section = ctx_.current();
  s->offset = ctx_.current()->Offset();
  s->defined = true;
  s->binding = Binding::kLocal;
  Symbol* raw = s.get();
  dot_symbols_.push_back(std::move(s));
  return raw;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

bool Parser::ParsePrimary(MCValue& out) {
  out = MCValue{};
  if (cur_.Is(TokKind::kError)) {
    Error(cur_.offset, cur_.str);
    return false;
  }
  if (cur_.Is(TokKind::kNumber)) {
    out.addend = static_cast<int64_t>(cur_.value);
    Advance();
    return true;
  }
  if (cur_.Is(TokKind::kDot)) {
    out.sym = MakeDotSymbol();
    Advance();
    return true;
  }
  if (cur_.Is(TokKind::kLParen)) {
    Advance();
    if (!ParseExpr(out)) return false;
    return Expect(TokKind::kRParen, "')'");
  }
  if (cur_.Is(TokKind::kIdentifier)) {
    Symbol* s = ctx_.symtab().GetOrCreate(cur_.text);
    s->referenced = true;
    out.sym = s;
    Advance();
    if (cur_.Is(TokKind::kAt)) {
      Advance();
      if (!cur_.Is(TokKind::kIdentifier)) {
        Error(cur_.offset, "expected relocation modifier after '@'");
        return false;
      }
      out.mod = ParseModifier(cur_.text);
      if (out.mod == RelModifier::kNone)
        Error(cur_.offset, "unknown relocation modifier");
      Advance();
    }
    return true;
  }
  Error(cur_.offset, "expected an expression");
  return false;
}

bool Parser::ParseUnary(MCValue& out) {
  if (cur_.Is(TokKind::kMinus)) {
    Advance();
    MCValue v;
    if (!ParseUnary(v)) return false;
    if (!v.IsConstant()) {
      Error(cur_.offset, "cannot negate a symbol");
      return false;
    }
    out = MCValue::Const(-v.addend);
    return true;
  }
  if (cur_.Is(TokKind::kPlus)) {
    Advance();
    return ParseUnary(out);
  }
  if (cur_.Is(TokKind::kTilde)) {
    Advance();
    MCValue v;
    if (!ParseUnary(v)) return false;
    if (!v.IsConstant()) {
      Error(cur_.offset, "cannot apply '~' to a symbol");
      return false;
    }
    out = MCValue::Const(~v.addend);
    return true;
  }
  return ParsePrimary(out);
}

bool Parser::ParseBinary(int min_prec, MCValue& out) {
  MCValue lhs;
  if (!ParseUnary(lhs)) return false;
  for (;;) {
    int p = PrecOf(cur_.kind);
    if (p < 0 || p < min_prec) break;
    TokKind op = cur_.kind;
    uint32_t off = cur_.offset;
    Advance();
    MCValue rhs;
    if (!ParseBinary(p + 1, rhs)) return false;

    // Additive operators may combine symbols; the rest require constants.
    if (op == TokKind::kPlus) {
      if (lhs.IsConstant()) {
        int64_t a = lhs.addend;
        lhs = rhs;
        lhs.addend += a;
      } else if (rhs.IsConstant()) {
        lhs.addend += rhs.addend;
      } else {
        Error(off, "cannot add two symbols");
        return false;
      }
    } else if (op == TokKind::kMinus) {
      if (rhs.IsConstant()) {
        lhs.addend -= rhs.addend;
      } else if (lhs.sym && !lhs.sym2 && rhs.sym && !rhs.sym2 &&
                 lhs.mod == RelModifier::kNone &&
                 rhs.mod == RelModifier::kNone) {
        lhs.sym2 = rhs.sym;
        lhs.addend -= rhs.addend;
      } else {
        Error(off, "unsupported symbol subtraction");
        return false;
      }
    } else {
      if (!lhs.IsConstant() || !rhs.IsConstant()) {
        Error(off, "operator requires constant operands");
        return false;
      }
      int64_t a = lhs.addend, b = rhs.addend, r = 0;
      switch (op) {
        case TokKind::kStar: r = a * b; break;
        case TokKind::kSlash:
          if (b == 0) { Error(off, "division by zero"); return false; }
          r = a / b;
          break;
        case TokKind::kPercent:
          if (b == 0) { Error(off, "division by zero"); return false; }
          r = a % b;
          break;
        case TokKind::kLShift: r = a << b; break;
        case TokKind::kRShift: r = a >> b; break;
        case TokKind::kAmp: r = a & b; break;
        case TokKind::kPipe: r = a | b; break;
        case TokKind::kCaret: r = a ^ b; break;
        default: break;
      }
      lhs = MCValue::Const(r);
    }
  }
  out = lhs;
  return true;
}

bool Parser::ParseExpr(MCValue& out) { return ParseBinary(0, out); }

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

bool Parser::ParseRegister(Reg& out) {
  if (!LookupRegister(cur_.text, out)) {
    Error(cur_.offset, std::string("unknown register '%") +
                           std::string(cur_.text) + "'");
    Advance();
    return false;
  }
  Advance();
  return true;
}

bool Parser::ParseMemoryTail(Operand& out) {
  if (!Expect(TokKind::kLParen, "'('")) return false;
  if (cur_.Is(TokKind::kRegister)) {
    Reg base;
    if (!ParseRegister(base)) return false;
    if (base.cls == RegClass::kRip)
      out.rip = true;
    else
      out.base = base;
  }
  if (Accept(TokKind::kComma)) {
    if (cur_.Is(TokKind::kRegister)) {
      Reg idx;
      if (!ParseRegister(idx)) return false;
      out.index = idx;
    }
    if (Accept(TokKind::kComma)) {
      MCValue sc;
      if (!ParseExpr(sc)) return false;
      if (!sc.IsConstant()) {
        Error(cur_.offset, "scale must be a constant");
        return false;
      }
      out.scale = static_cast<uint8_t>(sc.addend);
    }
  }
  if (!Expect(TokKind::kRParen, "')'")) return false;
  if (out.rip && out.index.Present()) {
    Error(cur_.offset, "rip-relative addressing cannot use an index register");
    return false;
  }
  return true;
}

bool Parser::ParseOperand(Operand& out) {
  out = Operand{};
  if (Accept(TokKind::kStar)) out.indirect = true;

  if (cur_.Is(TokKind::kDollar)) {
    Advance();
    out.kind = OperandKind::kImm;
    return ParseExpr(out.imm);
  }
  if (cur_.Is(TokKind::kRegister)) {
    out.kind = OperandKind::kReg;
    return ParseRegister(out.reg);
  }
  if (cur_.Is(TokKind::kLParen)) {
    const Token& nxt = lexer_.Peek();
    if (nxt.Is(TokKind::kRegister) || nxt.Is(TokKind::kComma)) {
      out.kind = OperandKind::kMem;
      return ParseMemoryTail(out);
    }
  }
  MCValue disp;
  if (!ParseExpr(disp)) return false;
  out.kind = OperandKind::kMem;
  out.disp = disp;
  if (cur_.Is(TokKind::kLParen)) return ParseMemoryTail(out);
  return true;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

bool Parser::Run() {
  while (!cur_.Is(TokKind::kEof)) {
    if (cur_.Is(TokKind::kError)) {
      Error(cur_.offset, cur_.str);
      Advance();
      continue;
    }
    ParseStatement();
    if (cur_.Is(TokKind::kNewline)) {
      Advance();
    } else if (!cur_.Is(TokKind::kEof)) {
      Error(cur_.offset, "unexpected token at end of statement");
      SkipToEndOfStatement();
    }
  }
  return !diag_.has_error();
}

void Parser::ParseStatement() {
  // Leading labels: `name:`.
  while (cur_.Is(TokKind::kIdentifier) && lexer_.Peek().Is(TokKind::kColon)) {
    std::string_view lname = cur_.text;
    uint32_t loc = cur_.offset;
    if (!ctx_.DefineLabel(lname))
      Error(loc, std::string("symbol '") + std::string(lname) +
                     "' is already defined");
    Advance();  // name
    Advance();  // ':'
  }

  if (cur_.IsEndOfStatement()) return;

  // Assignment: `name = expr`.
  if (cur_.Is(TokKind::kIdentifier) && lexer_.Peek().Is(TokKind::kEqual)) {
    std::string_view name = cur_.text;
    Advance();  // name
    Advance();  // '='
    ParseAssignment(name);
    return;
  }

  if (cur_.Is(TokKind::kIdentifier)) {
    if (!cur_.text.empty() && cur_.text[0] == '.') {
      ParseDirective();
    } else {
      std::string_view mnem = cur_.text;
      uint32_t loc = cur_.offset;
      Advance();
      ParseInstruction(mnem, loc);
    }
    return;
  }

  Error(cur_.offset, "expected a label, instruction, or directive");
  SkipToEndOfStatement();
}

void Parser::ParseInstruction(std::string_view mnemonic, uint32_t loc) {
  std::vector<Operand> ops;
  if (!cur_.IsEndOfStatement()) {
    do {
      Operand op;
      if (!ParseOperand(op)) {
        SkipToEndOfStatement();
        return;
      }
      ops.push_back(op);
    } while (Accept(TokKind::kComma));
  }
  encoder_.Encode(mnemonic, ops, loc);
}

void Parser::ParseAssignment(std::string_view name) {
  MCValue v;
  if (!ParseExpr(v)) return;
  Symbol* s = ctx_.symtab().GetOrCreate(name);
  s->referenced = true;
  int64_t c;
  if (ResolveConstant(v, c)) {
    s->defined = true;
    s->section = nullptr;  // absolute (SHN_ABS)
    s->offset = static_cast<uint64_t>(c);
  } else if (v.sym && !v.sym2 && v.sym->defined) {
    s->defined = true;
    s->section = v.sym->section;
    s->offset = v.sym->offset + v.addend;
  } else {
    Error(cur_.offset, "unsupported assignment expression");
  }
}

//===----------------------------------------------------------------------===//
// Directives
//===----------------------------------------------------------------------===//

void Parser::EmitData(unsigned size, bool sign_extend) {
  Section* s = ctx_.current();
  if (s->is_bss()) {
    Error(cur_.offset, "cannot emit initialized data in a nobits section");
    SkipToEndOfStatement();
    return;
  }
  do {
    MCValue v;
    if (!ParseExpr(v)) return;
    int64_t c;
    if (ResolveConstant(v, c)) {
      s->AppendLE(static_cast<uint64_t>(c), size);
      continue;
    }
    // A `sym - .` difference at this exact location becomes PC-relative.
    bool pcrel = false;
    if (v.sym && v.sym2 && v.sym2->section == s &&
        v.sym2->offset == s->Offset()) {
      pcrel = true;  // `sym - .` at this location
      v.sym2 = nullptr;
    }
    Fixup f{s->Offset(), v, static_cast<uint8_t>(size), pcrel, sign_extend, 0};
    s->AddFixup(f);
    s->AppendLE(0, size);
  } while (Accept(TokKind::kComma));
}

void Parser::DoAscii(bool nul_terminate) {
  Section* s = ctx_.current();
  do {
    if (!cur_.Is(TokKind::kString)) {
      Error(cur_.offset, "expected a string literal");
      return;
    }
    for (char ch : cur_.str) s->AppendByte(static_cast<uint8_t>(ch));
    if (nul_terminate) s->AppendByte(0);
    Advance();
  } while (Accept(TokKind::kComma));
}

void Parser::DoAlign(bool power_of_two) {
  MCValue n;
  if (!ParseExpr(n) || !n.IsConstant()) {
    Error(cur_.offset, "alignment must be a constant");
    return;
  }
  uint64_t boundary =
      power_of_two ? (uint64_t{1} << n.addend) : static_cast<uint64_t>(n.addend);
  if (boundary == 0) boundary = 1;

  bool have_fill = false;
  int64_t fill = 0;
  if (Accept(TokKind::kComma)) {
    if (!cur_.IsEndOfStatement()) {
      MCValue f;
      if (ParseExpr(f) && f.IsConstant()) {
        have_fill = true;
        fill = f.addend;
      }
    }
    if (Accept(TokKind::kComma)) {  // max-skip: parsed and ignored
      MCValue m;
      ParseExpr(m);
    }
  }

  Section* s = ctx_.current();
  s->BumpAlign(static_cast<uint32_t>(boundary));
  uint64_t cur = s->Offset();
  uint64_t pad = (boundary - (cur % boundary)) % boundary;
  bool use_nop = !have_fill && (s->sh_flags() & kShfExecinstr) != 0;
  uint8_t fill_byte = have_fill ? static_cast<uint8_t>(fill)
                                : (use_nop ? 0x90 : 0x00);
  // Record the point so branch relaxation can recompute the padding if earlier
  // instructions shrink.
  s->AddAlignment(AlignPoint{cur, static_cast<uint32_t>(boundary), fill_byte,
                             use_nop});
  s->Fill(pad, fill_byte);
}

void Parser::DoComm(bool local) {
  if (!cur_.Is(TokKind::kIdentifier)) {
    Error(cur_.offset, "expected a symbol name");
    return;
  }
  std::string_view name = cur_.text;
  Advance();
  if (!Expect(TokKind::kComma, "','")) return;
  MCValue size;
  if (!ParseExpr(size) || !size.IsConstant()) {
    Error(cur_.offset, "common size must be a constant");
    return;
  }
  int64_t align = 1;
  if (Accept(TokKind::kComma)) {
    MCValue a;
    if (ParseExpr(a) && a.IsConstant()) align = a.addend;
  }

  Symbol* s = ctx_.symtab().GetOrCreate(name);
  s->referenced = true;
  if (local) {
    // Place it in .bss.
    Section* b = ctx_.bss();
    uint64_t al = align ? static_cast<uint64_t>(align) : 1;
    uint64_t off = b->Offset();
    off = (off + al - 1) & ~(al - 1);
    b->Fill(off - b->Offset(), 0);
    b->BumpAlign(static_cast<uint32_t>(al));
    s->section = b;
    s->offset = off;
    s->defined = true;
    s->binding = Binding::kLocal;
    b->Fill(static_cast<size_t>(size.addend), 0);
  } else {
    s->is_common = true;
    s->common_size = static_cast<uint64_t>(size.addend);
    s->common_align = static_cast<uint64_t>(align);
    s->binding = Binding::kGlobal;
  }
}

void Parser::ParseDirective() {
  std::string name(cur_.text);
  uint32_t loc = cur_.offset;
  Advance();

  auto binding_list = [&](Binding b) {
    do {
      if (!cur_.Is(TokKind::kIdentifier)) {
        Error(cur_.offset, "expected a symbol name");
        return;
      }
      Symbol* s = ctx_.symtab().GetOrCreate(cur_.text);
      s->binding = b;
      s->referenced = true;
      Advance();
    } while (Accept(TokKind::kComma));
  };

  if (name == ".text") {
    ctx_.SwitchSection(ctx_.text());
  } else if (name == ".data") {
    ctx_.SwitchSection(ctx_.data());
  } else if (name == ".bss") {
    ctx_.SwitchSection(ctx_.bss());
  } else if (name == ".rodata") {
    ctx_.SwitchSection(ctx_.rodata());
  } else if (name == ".section") {
    // .section name [, "flags" [, @type]]
    std::string sec_name;
    if (cur_.Is(TokKind::kIdentifier))
      sec_name = std::string(cur_.text);
    else if (cur_.Is(TokKind::kString))
      sec_name = cur_.str;
    else {
      Error(cur_.offset, "expected a section name");
      SkipToEndOfStatement();
      return;
    }
    Advance();
    uint64_t flags = 0;
    uint32_t type = kShtProgbits;
    bool have_flags = false;
    if (Accept(TokKind::kComma) && cur_.Is(TokKind::kString)) {
      have_flags = true;
      for (char c : cur_.str) {
        switch (c) {
          case 'a': flags |= kShfAlloc; break;
          case 'w': flags |= kShfWrite; break;
          case 'x': flags |= kShfExecinstr; break;
          case 'M': flags |= kShfMerge; break;
          case 'S': flags |= kShfStrings; break;
          default: break;
        }
      }
      Advance();
      if (Accept(TokKind::kComma)) {
        Accept(TokKind::kAt);
        if (cur_.Is(TokKind::kIdentifier)) {
          if (cur_.text == "nobits") type = kShtNobits;
          Advance();
        }
        // optional entsize etc.
        while (Accept(TokKind::kComma))
          if (!cur_.IsEndOfStatement()) Advance();
      }
    }
    if (!have_flags) flags = kShfAlloc;  // reasonable default
    Section* s = ctx_.GetOrCreateSection(sec_name, type, flags);
    if (type == kShtNobits) s->set_is_bss(true);
    ctx_.SwitchSection(s);
  } else if (name == ".globl" || name == ".global") {
    binding_list(Binding::kGlobal);
  } else if (name == ".weak") {
    binding_list(Binding::kWeak);
  } else if (name == ".local") {
    binding_list(Binding::kLocal);
  } else if (name == ".type") {
    if (!cur_.Is(TokKind::kIdentifier)) {
      Error(cur_.offset, "expected a symbol name");
      SkipToEndOfStatement();
      return;
    }
    Symbol* s = ctx_.symtab().GetOrCreate(cur_.text);
    s->referenced = true;
    Advance();
    Expect(TokKind::kComma, "','");
    Accept(TokKind::kAt);
    if (cur_.Is(TokKind::kIdentifier)) {
      if (cur_.text == "function")
        s->type = SymType::kFunc;
      else if (cur_.text == "object")
        s->type = SymType::kObject;
      Advance();
    }
  } else if (name == ".size") {
    if (!cur_.Is(TokKind::kIdentifier)) {
      Error(cur_.offset, "expected a symbol name");
      SkipToEndOfStatement();
      return;
    }
    Symbol* s = ctx_.symtab().GetOrCreate(cur_.text);
    Advance();
    Expect(TokKind::kComma, "','");
    MCValue v;
    if (ParseExpr(v)) {
      int64_t c;
      if (ResolveConstant(v, c)) s->size = static_cast<uint64_t>(c);
    }
  } else if (name == ".set" || name == ".equ" || name == ".equiv") {
    if (!cur_.Is(TokKind::kIdentifier)) {
      Error(cur_.offset, "expected a symbol name");
      SkipToEndOfStatement();
      return;
    }
    std::string_view sym = cur_.text;
    Advance();
    Expect(TokKind::kComma, "','");
    ParseAssignment(sym);
  } else if (name == ".byte") {
    EmitData(1, false);
  } else if (name == ".value" || name == ".short" || name == ".word") {
    EmitData(2, false);
  } else if (name == ".long" || name == ".int") {
    EmitData(4, false);
  } else if (name == ".quad") {
    EmitData(8, false);
  } else if (name == ".ascii") {
    DoAscii(false);
  } else if (name == ".asciz" || name == ".string") {
    DoAscii(true);
  } else if (name == ".zero" || name == ".skip" || name == ".space") {
    MCValue n;
    if (!ParseExpr(n) || !n.IsConstant()) {
      Error(cur_.offset, "expected a constant size");
      SkipToEndOfStatement();
      return;
    }
    int64_t fill = 0;
    if (Accept(TokKind::kComma)) {
      MCValue f;
      if (ParseExpr(f) && f.IsConstant()) fill = f.addend;
    }
    ctx_.current()->Fill(static_cast<size_t>(n.addend),
                         static_cast<uint8_t>(fill));
  } else if (name == ".align" || name == ".balign") {
    DoAlign(false);
  } else if (name == ".p2align") {
    DoAlign(true);
  } else if (name == ".comm") {
    DoComm(false);
  } else if (name == ".lcomm") {
    DoComm(true);
  } else if (name == ".file" || name == ".ident" || name == ".loc" ||
             name == ".cfi_startproc" || name == ".cfi_endproc" ||
             name.rfind(".cfi_", 0) == 0 || name == ".p2align_" ||
             name == ".addrsig" || name.rfind(".addrsig", 0) == 0 ||
             name == ".weakref" || name == ".version") {
    // Accepted and ignored: debugging/unwind/metadata directives.
    SkipToEndOfStatement();
  } else {
    diag_.Warning(loc, std::string("ignoring unknown directive '") + name + "'");
    SkipToEndOfStatement();
  }
}

}  // namespace bcc::as
