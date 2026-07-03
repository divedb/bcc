#include "bcc/as/encoder.hh"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bcc::as {

namespace {

bool FitsInt8(int64_t v) { return v >= -128 && v <= 127; }
bool FitsInt32(int64_t v) {
  return v >= -2147483648LL && v <= 2147483647LL;
}

/// A register-or-memory operand, normalized for the ModRM/SIB emitter.
struct RM {
  bool is_reg = false;
  Reg reg;
  Reg base;
  Reg index;
  uint8_t scale = 1;
  MCValue disp;
  bool rip = false;
};

/// An immediate to append after the ModRM/SIB/disp bytes.
struct Imm {
  bool present = false;
  MCValue val;
  uint8_t size = 0;
  bool sign_extend = false;
};

RM MakeRM(const Operand& op) {
  RM rm;
  if (op.IsReg()) {
    rm.is_reg = true;
    rm.reg = op.reg;
  } else {  // memory
    rm.base = op.base;
    rm.index = op.index;
    rm.scale = op.scale;
    rm.disp = op.disp;
    rm.rip = op.rip;
  }
  return rm;
}

uint8_t ScaleBits(uint8_t s, bool& ok) {
  ok = true;
  switch (s) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: ok = false; return 0;
  }
}

/// Condition-code suffix (`e`, `ne`, `l`, ...) → the 4-bit `tttn` code shared by
/// jcc/setcc/cmovcc. Returns -1 for an unknown suffix.
int ConditionCode(std::string_view cc) {
  static const std::unordered_map<std::string_view, int> m = {
      {"o", 0},   {"no", 1},  {"b", 2},   {"c", 2},   {"nae", 2}, {"ae", 3},
      {"nb", 3},  {"nc", 3},  {"e", 4},   {"z", 4},   {"ne", 5},  {"nz", 5},
      {"be", 6},  {"na", 6},  {"a", 7},   {"nbe", 7}, {"s", 8},   {"ns", 9},
      {"p", 10},  {"pe", 10}, {"np", 11}, {"po", 11}, {"l", 12},  {"nge", 12},
      {"ge", 13}, {"nl", 13}, {"le", 14}, {"ng", 14}, {"g", 15},  {"nle", 15},
  };
  auto it = m.find(cc);
  return it == m.end() ? -1 : it->second;
}

/// Mnemonics that carry an optional `b`/`w`/`l`/`q` size suffix. The stem is
/// looked up here to decide whether a trailing size letter should be stripped
/// (distinguishing e.g. `movl` from `call`).
bool IsSuffixable(std::string_view stem) {
  static const std::unordered_map<std::string_view, int> m = {
      {"mov", 0},  {"add", 0},  {"sub", 0},  {"and", 0},  {"or", 0},
      {"xor", 0},  {"cmp", 0},  {"adc", 0},  {"sbb", 0},  {"test", 0},
      {"inc", 0},  {"dec", 0},  {"neg", 0},  {"not", 0},  {"mul", 0},
      {"imul", 0}, {"idiv", 0}, {"div", 0},  {"push", 0}, {"pop", 0},
      {"shl", 0},  {"shr", 0},  {"sar", 0},  {"sal", 0},  {"rol", 0},
      {"ror", 0},  {"lea", 0},  {"xchg", 0},
  };
  return m.count(stem) != 0;
}

}  // namespace

/// Stateful per-instruction encoder emitting into the current section.
class EmitEngine {
 public:
  EmitEngine(MCContext& ctx, AsmDiag& diag, uint32_t loc)
      : diag_(diag), loc_(loc), sec_(*ctx.current()) {}

  bool Encode(std::string_view mnem, std::span<const Operand> ops);

 private:
  //=== byte output ===//
  void Emit8(uint8_t b) { sec_.AppendByte(b); }
  void EmitN(uint64_t v, unsigned n) { sec_.AppendLE(v, n); }

  bool Error(const std::string& msg) {
    diag_.Error(loc_, msg);
    return false;
  }

  //=== immediate / displacement with optional relocation ===//
  void EmitImm(const Imm& imm) {
    if (imm.val.IsRelocatable()) {
      sec_.AddFixup(Fixup{sec_.Offset(), imm.val, imm.size, /*pcrel=*/false,
                          imm.sign_extend, /*trailing=*/0});
      EmitN(0, imm.size);
    } else {
      EmitN(static_cast<uint64_t>(imm.val.addend), imm.size);
    }
  }

  void EmitDisp32(const MCValue& disp, bool pcrel, uint8_t trailing) {
    if (disp.IsRelocatable()) {
      Fixup f{sec_.Offset(), disp, 4, pcrel, /*sign_extend=*/!pcrel, trailing};
      sec_.AddFixup(f);
      EmitN(0, 4);
    } else {
      EmitN(static_cast<uint64_t>(disp.addend), 4);
    }
  }

  void EmitRel32(const MCValue& target) {
    Fixup f{sec_.Offset(), target, 4, /*pcrel=*/true, false, 0};
    f.is_branch = true;
    sec_.AddFixup(f);
    EmitN(0, 4);
  }

  //=== ModRM / SIB / REX machinery ===//
  bool EmitModRM(uint8_t reg3, const RM& rm, uint8_t trailing);

  // rexw/p66 are computed by the caller from the operation size.
  bool EmitOp(bool rexw, bool p66, std::vector<uint8_t> opcode, int reg_field,
              Reg reg_op, const RM& rm, const Imm& imm, uint8_t mand_prefix = 0);
  bool EmitOpSized(unsigned opsize, std::vector<uint8_t> opcode, int reg_field,
                   Reg reg_op, const RM& rm, const Imm& imm,
                   uint8_t mand_prefix = 0) {
    return EmitOp(opsize == 8, opsize == 2, std::move(opcode), reg_field, reg_op,
                  rm, imm, mand_prefix);
  }

  // opcode+reg forms (0xB8+rd, 0x50+rd, ...).
  bool EmitOpcodeReg(bool rexw, bool p66, uint8_t base_opcode, Reg reg,
                     const Imm& imm);

  //=== instruction groups ===//
  bool EncodeAlu(int op_index, unsigned size, std::span<const Operand> ops);
  bool EncodeTest(unsigned size, std::span<const Operand> ops);
  bool EncodeMov(unsigned size, bool movabs, std::span<const Operand> ops);
  bool EncodeLea(unsigned size, std::span<const Operand> ops);
  bool EncodeUnary(uint8_t ext, unsigned size, std::span<const Operand> ops);
  bool EncodeIncDec(bool dec, unsigned size, std::span<const Operand> ops);
  bool EncodePush(unsigned size, std::span<const Operand> ops);
  bool EncodePop(unsigned size, std::span<const Operand> ops);
  bool EncodeShift(uint8_t ext, unsigned size, std::span<const Operand> ops);
  bool EncodeImul(unsigned size, std::span<const Operand> ops);
  bool EncodeMovExtend(std::string_view base, std::span<const Operand> ops);
  bool EncodeCallJmp(bool is_call, std::span<const Operand> ops);
  bool EncodeJcc(int cc, std::span<const Operand> ops);
  bool EncodeSetcc(int cc, std::span<const Operand> ops);
  bool EncodeCmovcc(int cc, unsigned size, std::span<const Operand> ops);

  /// Resolves the operation size: an explicit suffix wins; otherwise it is
  /// inferred from a register operand. \return false (and reports) if the size
  /// is required but cannot be determined.
  bool ResolveSize(unsigned suffix, std::span<const Operand> ops,
                   unsigned& out);

  AsmDiag& diag_;
  uint32_t loc_;
  Section& sec_;
};

bool EmitEngine::EmitModRM(uint8_t reg3, const RM& rm, uint8_t trailing) {
  if (rm.is_reg) {
    Emit8(static_cast<uint8_t>(0xC0 | (reg3 << 3) | (rm.reg.num & 7)));
    return true;
  }
  if (rm.rip) {
    Emit8(static_cast<uint8_t>(0x00 | (reg3 << 3) | 0x05));
    EmitDisp32(rm.disp, /*pcrel=*/true, trailing);
    return true;
  }

  bool have_base = rm.base.Present();
  bool have_index = rm.index.Present();
  bool disp_sym = rm.disp.IsRelocatable();
  int64_t d = rm.disp.addend;

  if (have_index && rm.index.num == 4)
    return Error("%rsp cannot be used as an index register");

  // Pure displacement (no base, no index): [disp32] via SIB base=101,index=100.
  if (!have_base && !have_index) {
    Emit8(static_cast<uint8_t>(0x00 | (reg3 << 3) | 0x04));
    Emit8(static_cast<uint8_t>((0 << 6) | (4 << 3) | 5));
    EmitDisp32(rm.disp, /*pcrel=*/false, trailing);
    return true;
  }

  bool need_sib = have_index || (have_base && (rm.base.num & 7) == 4);

  if (!need_sib) {
    uint8_t rmv = rm.base.num & 7;      // != 4 here
    bool base_is_bp = (rmv == 5);       // rbp / r13
    uint8_t mod;
    if (disp_sym)
      mod = 2;
    else if (d == 0 && !base_is_bp)
      mod = 0;
    else if (FitsInt8(d))
      mod = 1;
    else
      mod = 2;
    Emit8(static_cast<uint8_t>((mod << 6) | (reg3 << 3) | rmv));
    if (mod == 1)
      Emit8(static_cast<uint8_t>(d & 0xff));
    else if (mod == 2)
      EmitDisp32(rm.disp, false, trailing);
    return true;
  }

  // SIB form.
  bool ok = true;
  uint8_t scale_bits = ScaleBits(rm.scale, ok);
  if (!ok) return Error("invalid scale (must be 1, 2, 4, or 8)");
  uint8_t index_field = have_index ? (rm.index.num & 7) : 4;

  if (!have_base) {  // [index*scale + disp32]
    Emit8(static_cast<uint8_t>((0 << 6) | (reg3 << 3) | 4));
    Emit8(static_cast<uint8_t>((scale_bits << 6) | (index_field << 3) | 5));
    EmitDisp32(rm.disp, false, trailing);
    return true;
  }

  uint8_t base_field = rm.base.num & 7;
  bool base_is_bp = (base_field == 5);  // rbp / r13 need an explicit disp
  uint8_t mod;
  if (disp_sym)
    mod = 2;
  else if (d == 0 && !base_is_bp)
    mod = 0;
  else if (FitsInt8(d))
    mod = 1;
  else
    mod = 2;
  Emit8(static_cast<uint8_t>((mod << 6) | (reg3 << 3) | 4));
  Emit8(static_cast<uint8_t>((scale_bits << 6) | (index_field << 3) | base_field));
  if (mod == 1)
    Emit8(static_cast<uint8_t>(d & 0xff));
  else if (mod == 2)
    EmitDisp32(rm.disp, false, trailing);
  return true;
}

bool EmitEngine::EmitOp(bool rexw, bool p66, std::vector<uint8_t> opcode,
                        int reg_field, Reg reg_op, const RM& rm, const Imm& imm,
                        uint8_t mand_prefix) {
  bool rex_r = (reg_field & 8) != 0;
  bool rex_x = false, rex_b = false;
  if (rm.is_reg) {
    rex_b = rm.reg.num >= 8;
  } else {
    if (rm.base.Present()) rex_b = rm.base.num >= 8;
    if (rm.index.Present()) rex_x = rm.index.num >= 8;
  }

  bool need_byte_rex = false, forbid_rex = false;
  auto check = [&](const Reg& r) {
    if (!r.Present()) return;
    if (r.NeedsRex()) need_byte_rex = true;
    if (r.IsHigh8()) forbid_rex = true;
  };
  if (reg_op.Present()) check(reg_op);
  if (rm.is_reg) check(rm.reg);

  bool need_rex = rexw || rex_r || rex_x || rex_b || need_byte_rex;
  if (need_rex && forbid_rex)
    return Error("cannot encode high-byte register with a REX prefix");

  if (mand_prefix) Emit8(mand_prefix);
  if (p66) Emit8(0x66);
  if (need_rex) {
    Emit8(static_cast<uint8_t>(0x40 | (rexw << 3) | (rex_r << 2) |
                               (rex_x << 1) | (rex_b << 0)));
  }
  for (uint8_t b : opcode) Emit8(b);
  if (!EmitModRM(static_cast<uint8_t>(reg_field & 7), rm,
                 imm.present ? imm.size : 0))
    return false;
  if (imm.present) EmitImm(imm);
  return true;
}

bool EmitEngine::EmitOpcodeReg(bool rexw, bool p66, uint8_t base_opcode,
                               Reg reg, const Imm& imm) {
  bool rex_b = reg.num >= 8;
  bool need_byte_rex = reg.NeedsRex();
  bool forbid_rex = reg.IsHigh8();
  bool need_rex = rexw || rex_b || need_byte_rex;
  if (need_rex && forbid_rex)
    return Error("cannot encode high-byte register with a REX prefix");
  if (p66) Emit8(0x66);
  if (need_rex)
    Emit8(static_cast<uint8_t>(0x40 | (rexw << 3) | (rex_b << 0)));
  Emit8(static_cast<uint8_t>(base_opcode + (reg.num & 7)));
  if (imm.present) EmitImm(imm);
  return true;
}

bool EmitEngine::ResolveSize(unsigned suffix, std::span<const Operand> ops,
                             unsigned& out) {
  if (suffix) {
    out = suffix;
    return true;
  }
  for (const Operand& op : ops)
    if (op.IsReg() && op.reg.SizeBytes()) {
      out = op.reg.SizeBytes();
      return true;
    }
  return Error("ambiguous operand size; add a b/w/l/q suffix");
}

//===----------------------------------------------------------------------===//
// Instruction groups
//===----------------------------------------------------------------------===//

bool EmitEngine::EncodeAlu(int op_index, unsigned size,
                           std::span<const Operand> ops) {
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];
  uint8_t base = static_cast<uint8_t>(op_index * 8);

  if (src.IsImm()) {
    unsigned s;
    if (size)
      s = size;
    else if (dst.IsReg())
      s = dst.reg.SizeBytes();
    else
      return Error("ambiguous operand size; add a b/w/l/q suffix");
    RM rm = MakeRM(dst);

    // 8-bit: only the imm8 group form (0x80 /n).
    if (s == 1) {
      Imm imm{true, src.imm, 1, false};
      // accumulator short form: add $imm, %al -> 0x04+op*8
      if (dst.IsReg() && dst.reg.num == 0 && src.imm.IsConstant()) {
        Emit8(static_cast<uint8_t>(0x04 + base));
        EmitImm(imm);
        return true;
      }
      return EmitOpSized(1, {0x80}, op_index, Reg{}, rm, imm);
    }

    bool use_imm8 = src.imm.IsConstant() && FitsInt8(src.imm.addend);
    if (use_imm8) {
      Imm imm{true, src.imm, 1, true};
      return EmitOpSized(s, {0x83}, op_index, Reg{}, rm, imm);
    }
    uint8_t iw = (s == 2) ? 2 : 4;
    Imm imm{true, src.imm, iw, s == 8};
    // accumulator short form: add $imm, %eax/%rax/%ax -> 0x05+op*8
    if (dst.IsReg() && dst.reg.num == 0) {
      if (s == 8) Emit8(0x48);
      if (s == 2) Emit8(0x66);
      Emit8(static_cast<uint8_t>(0x05 + base));
      EmitImm(imm);
      return true;
    }
    return EmitOpSized(s, {0x81}, op_index, Reg{}, rm, imm);
  }

  // register/memory operands (no immediate)
  if (src.IsReg() && (dst.IsReg() || dst.IsMem())) {
    unsigned s = size ? size : src.reg.SizeBytes();
    RM rm = MakeRM(dst);
    uint8_t opc = (s == 1) ? (base + 0) : (base + 1);
    return EmitOpSized(s, {opc}, src.reg.num, src.reg, rm, {});
  }
  if (src.IsMem() && dst.IsReg()) {
    unsigned s = size ? size : dst.reg.SizeBytes();
    RM rm = MakeRM(src);
    uint8_t opc = (s == 1) ? (base + 2) : (base + 3);
    return EmitOpSized(s, {opc}, dst.reg.num, dst.reg, rm, {});
  }
  return Error("invalid operands for arithmetic instruction");
}

bool EmitEngine::EncodeTest(unsigned size, std::span<const Operand> ops) {
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];

  if (src.IsImm()) {
    unsigned s;
    if (size)
      s = size;
    else if (dst.IsReg())
      s = dst.reg.SizeBytes();
    else
      return Error("ambiguous operand size; add a b/w/l/q suffix");
    RM rm = MakeRM(dst);
    uint8_t iw = (s == 1) ? 1 : (s == 2 ? 2 : 4);
    Imm imm{true, src.imm, iw, s == 8};
    // accumulator short form: test $imm, %al/%ax/%eax/%rax -> 0xA8/0xA9
    if (dst.IsReg() && dst.reg.num == 0 && dst.reg.cls != RegClass::kGpr8h) {
      if (s == 1) {
        Emit8(0xA8);
      } else {
        if (s == 8) Emit8(0x48);
        if (s == 2) Emit8(0x66);
        Emit8(0xA9);
      }
      EmitImm(imm);
      return true;
    }
    uint8_t opc = (s == 1) ? 0xF6 : 0xF7;
    return EmitOpSized(s, {opc}, 0, Reg{}, rm, imm);
  }

  // reg/mem: the register operand goes in ModRM.reg, the other in r/m.
  const Operand *reg_op, *rm_op;
  if (src.IsReg()) {
    reg_op = &src;
    rm_op = &dst;
  } else if (dst.IsReg()) {
    reg_op = &dst;
    rm_op = &src;
  } else {
    return Error("invalid operands for test");
  }
  unsigned s = size ? size : reg_op->reg.SizeBytes();
  RM rm = MakeRM(*rm_op);
  uint8_t opc = (s == 1) ? 0x84 : 0x85;
  return EmitOpSized(s, {opc}, reg_op->reg.num, reg_op->reg, rm, {});
}

bool EmitEngine::EncodeMov(unsigned size, bool movabs,
                           std::span<const Operand> ops) {
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];

  if (src.IsImm()) {
    if (dst.IsReg()) {
      unsigned s = size ? size : dst.reg.SizeBytes();
      if (s == 1) {
        Imm imm{true, src.imm, 1, false};
        return EmitOpcodeReg(false, false, 0xB0, dst.reg, imm);
      }
      if (s == 2) {
        Imm imm{true, src.imm, 2, false};
        return EmitOpcodeReg(false, true, 0xB8, dst.reg, imm);
      }
      if (s == 4) {
        Imm imm{true, src.imm, 4, false};
        return EmitOpcodeReg(false, false, 0xB8, dst.reg, imm);
      }
      // 64-bit destination.
      bool imm_fits32 = src.imm.IsConstant() && FitsInt32(src.imm.addend);
      if (movabs || !imm_fits32) {
        Imm imm{true, src.imm, 8, false};  // R_X86_64_64 if symbolic
        return EmitOpcodeReg(true, false, 0xB8, dst.reg, imm);
      }
      // movq $imm32, %r64 -> C7 /0 imm32 (sign-extended)
      Imm imm{true, src.imm, 4, true};
      RM rm = MakeRM(dst);
      return EmitOpSized(8, {0xC7}, 0, Reg{}, rm, imm);
    }
    // immediate to memory
    if (!size) return Error("ambiguous operand size; add a b/w/l/q suffix");
    RM rm = MakeRM(dst);
    if (size == 1) {
      Imm imm{true, src.imm, 1, false};
      return EmitOpSized(1, {0xC6}, 0, Reg{}, rm, imm);
    }
    uint8_t iw = (size == 2) ? 2 : 4;
    Imm imm{true, src.imm, iw, size == 8};
    return EmitOpSized(size, {0xC7}, 0, Reg{}, rm, imm);
  }

  if (src.IsReg() && (dst.IsReg() || dst.IsMem())) {
    unsigned s = size ? size : src.reg.SizeBytes();
    RM rm = MakeRM(dst);
    uint8_t opc = (s == 1) ? 0x88 : 0x89;
    return EmitOpSized(s, {opc}, src.reg.num, src.reg, rm, {});
  }
  if (src.IsMem() && dst.IsReg()) {
    unsigned s = size ? size : dst.reg.SizeBytes();
    RM rm = MakeRM(src);
    uint8_t opc = (s == 1) ? 0x8A : 0x8B;
    return EmitOpSized(s, {opc}, dst.reg.num, dst.reg, rm, {});
  }
  return Error("invalid operands for mov");
}

bool EmitEngine::EncodeLea(unsigned size, std::span<const Operand> ops) {
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];
  if (!src.IsMem() || !dst.IsReg())
    return Error("lea requires a memory source and a register destination");
  unsigned s = size ? size : dst.reg.SizeBytes();
  RM rm = MakeRM(src);
  return EmitOpSized(s, {0x8D}, dst.reg.num, dst.reg, rm, {});
}

bool EmitEngine::EncodeUnary(uint8_t ext, unsigned size,
                             std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  unsigned s;
  if (!ResolveSize(size, ops, s)) return false;
  RM rm = MakeRM(ops[0]);
  uint8_t opc = (s == 1) ? 0xF6 : 0xF7;
  return EmitOpSized(s, {opc}, ext, Reg{}, rm, {});
}

bool EmitEngine::EncodeIncDec(bool dec, unsigned size,
                              std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  unsigned s;
  if (!ResolveSize(size, ops, s)) return false;
  RM rm = MakeRM(ops[0]);
  uint8_t opc = (s == 1) ? 0xFE : 0xFF;
  return EmitOpSized(s, {opc}, dec ? 1 : 0, Reg{}, rm, {});
}

bool EmitEngine::EncodePush(unsigned size, std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  const Operand& op = ops[0];
  if (op.IsImm()) {
    if (op.imm.IsConstant() && FitsInt8(op.imm.addend)) {
      Emit8(0x6A);
      EmitN(static_cast<uint64_t>(op.imm.addend), 1);
    } else {
      Emit8(0x68);
      Imm imm{true, op.imm, 4, true};
      EmitImm(imm);
    }
    return true;
  }
  if (op.IsReg()) {
    unsigned s = size ? size : 8;  // default 64-bit
    return EmitOpcodeReg(false, s == 2, 0x50, op.reg, {});
  }
  // memory: 0xFF /6, default 64-bit operand (no REX.W).
  RM rm = MakeRM(op);
  return EmitOp(false, size == 2, {0xFF}, 6, Reg{}, rm, {});
}

bool EmitEngine::EncodePop(unsigned size, std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  const Operand& op = ops[0];
  if (op.IsReg()) {
    unsigned s = size ? size : 8;
    return EmitOpcodeReg(false, s == 2, 0x58, op.reg, {});
  }
  RM rm = MakeRM(op);
  return EmitOp(false, size == 2, {0x8F}, 0, Reg{}, rm, {});
}

bool EmitEngine::EncodeShift(uint8_t ext, unsigned size,
                             std::span<const Operand> ops) {
  if (ops.empty() || ops.size() > 2) return Error("wrong operand count");
  const Operand& dst = ops.back();
  unsigned s;
  if (!ResolveSize(size, std::span<const Operand>(&dst, 1), s)) return false;
  RM rm = MakeRM(dst);

  if (ops.size() == 1) {  // shift by 1
    uint8_t opc = (s == 1) ? 0xD0 : 0xD1;
    return EmitOpSized(s, {opc}, ext, Reg{}, rm, {});
  }
  const Operand& cnt = ops[0];
  if (cnt.IsReg() && cnt.reg.cls == RegClass::kGpr8 && cnt.reg.num == 1) {
    uint8_t opc = (s == 1) ? 0xD2 : 0xD3;  // by %cl
    return EmitOpSized(s, {opc}, ext, Reg{}, rm, {});
  }
  if (cnt.IsImm()) {
    if (cnt.imm.IsConstant() && cnt.imm.addend == 1) {
      uint8_t opc = (s == 1) ? 0xD0 : 0xD1;
      return EmitOpSized(s, {opc}, ext, Reg{}, rm, {});
    }
    uint8_t opc = (s == 1) ? 0xC0 : 0xC1;
    Imm imm{true, cnt.imm, 1, false};
    return EmitOpSized(s, {opc}, ext, Reg{}, rm, imm);
  }
  return Error("shift count must be an immediate or %cl");
}

bool EmitEngine::EncodeImul(unsigned size, std::span<const Operand> ops) {
  if (ops.size() == 1) return EncodeUnary(5, size, ops);  // one-operand form
  if (ops.size() == 2) {
    const Operand& src = ops[0];
    const Operand& dst = ops[1];
    if (!dst.IsReg()) return Error("imul destination must be a register");
    unsigned s = size ? size : dst.reg.SizeBytes();
    RM rm = MakeRM(src);
    return EmitOpSized(s, {0x0F, 0xAF}, dst.reg.num, dst.reg, rm, {});
  }
  if (ops.size() == 3) {
    const Operand& imm_op = ops[0];
    const Operand& src = ops[1];
    const Operand& dst = ops[2];
    if (!imm_op.IsImm() || !dst.IsReg())
      return Error("invalid operands for three-operand imul");
    unsigned s = size ? size : dst.reg.SizeBytes();
    RM rm = MakeRM(src);
    if (imm_op.imm.IsConstant() && FitsInt8(imm_op.imm.addend)) {
      Imm imm{true, imm_op.imm, 1, true};
      return EmitOpSized(s, {0x6B}, dst.reg.num, dst.reg, rm, imm);
    }
    uint8_t iw = (s == 2) ? 2 : 4;
    Imm imm{true, imm_op.imm, iw, s == 8};
    return EmitOpSized(s, {0x69}, dst.reg.num, dst.reg, rm, imm);
  }
  return Error("wrong operand count for imul");
}

bool EmitEngine::EncodeMovExtend(std::string_view base,
                                 std::span<const Operand> ops) {
  // base is like "movzbl", "movswq", "movslq"/"movsxd".
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];
  if (!dst.IsReg()) return Error("destination must be a register");

  if (base == "movslq" || base == "movsxd" || base == "movsxdq") {
    RM rm = MakeRM(src);
    return EmitOpSized(8, {0x63}, dst.reg.num, dst.reg, rm, {});
  }
  // movz/movs + <src><dst> suffixes.
  bool is_signed = base.substr(0, 4) == "movs";
  if (base.size() != 6) return Error("unknown mov-extend mnemonic");
  char src_c = base[4];
  char dst_c = base[5];
  unsigned dst_size = dst_c == 'w' ? 2 : dst_c == 'l' ? 4 : dst_c == 'q' ? 8 : 0;
  if (!dst_size) return Error("bad destination size in mov-extend");
  uint8_t opc2;
  if (src_c == 'b')
    opc2 = is_signed ? 0xBE : 0xB6;
  else if (src_c == 'w')
    opc2 = is_signed ? 0xBF : 0xB7;
  else
    return Error("bad source size in mov-extend");
  RM rm = MakeRM(src);
  return EmitOpSized(dst_size, {0x0F, opc2}, dst.reg.num, dst.reg, rm, {});
}

bool EmitEngine::EncodeCallJmp(bool is_call, std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  const Operand& op = ops[0];
  if (op.indirect) {
    RM rm;
    if (op.IsReg()) {
      rm.is_reg = true;
      rm.reg = op.reg;
    } else if (op.IsMem()) {
      rm = MakeRM(op);
    } else {
      return Error("invalid indirect target");
    }
    // 64-bit operand size is default; no REX.W.
    return EmitOp(false, false, {0xFF}, is_call ? 2 : 4, Reg{}, rm, {});
  }
  // Direct: a bare symbol/expression target -> rel32.
  if (op.IsMem() && !op.base.Present() && !op.index.Present() && !op.rip) {
    Emit8(is_call ? 0xE8 : 0xE9);
    EmitRel32(op.disp);
    return true;
  }
  if (op.IsImm()) {  // e.g. `jmp $addr` is unusual but accept the value
    Emit8(is_call ? 0xE8 : 0xE9);
    EmitRel32(op.imm);
    return true;
  }
  return Error("invalid branch target (use '*' for an indirect branch)");
}

bool EmitEngine::EncodeJcc(int cc, std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  const Operand& op = ops[0];
  const MCValue* target = nullptr;
  if (op.IsMem() && !op.base.Present() && !op.index.Present() && !op.rip)
    target = &op.disp;
  else if (op.IsImm())
    target = &op.imm;
  else
    return Error("invalid branch target");
  Emit8(0x0F);
  Emit8(static_cast<uint8_t>(0x80 + cc));
  EmitRel32(*target);
  return true;
}

bool EmitEngine::EncodeSetcc(int cc, std::span<const Operand> ops) {
  if (ops.size() != 1) return Error("expected one operand");
  RM rm = MakeRM(ops[0]);
  return EmitOpSized(1, {0x0F, static_cast<uint8_t>(0x90 + cc)}, 0, Reg{}, rm,
                     {});
}

bool EmitEngine::EncodeCmovcc(int cc, unsigned size,
                              std::span<const Operand> ops) {
  if (ops.size() != 2) return Error("expected two operands");
  const Operand& src = ops[0];
  const Operand& dst = ops[1];
  if (!dst.IsReg()) return Error("cmov destination must be a register");
  unsigned s = size ? size : dst.reg.SizeBytes();
  RM rm = MakeRM(src);
  return EmitOpSized(s, {0x0F, static_cast<uint8_t>(0x40 + cc)}, dst.reg.num,
                     dst.reg, rm, {});
}

//===----------------------------------------------------------------------===//
// Top-level dispatch
//===----------------------------------------------------------------------===//

bool EmitEngine::Encode(std::string_view mnem, std::span<const Operand> ops) {
  // Zero-operand and fixed instructions first (no size-suffix stripping).
  auto emit_bytes = [&](std::initializer_list<uint8_t> bs) {
    for (uint8_t b : bs) Emit8(b);
    return true;
  };
  if (mnem == "ret" || mnem == "retq") {
    if (ops.size() == 1 && ops[0].IsImm()) {
      Emit8(0xC2);
      EmitN(static_cast<uint64_t>(ops[0].imm.addend), 2);
      return true;
    }
    return emit_bytes({0xC3});
  }
  if (mnem == "leave" || mnem == "leaveq") return emit_bytes({0xC9});
  if (mnem == "nop") return emit_bytes({0x90});
  if (mnem == "hlt") return emit_bytes({0xF4});
  if (mnem == "int3") return emit_bytes({0xCC});
  if (mnem == "ud2") return emit_bytes({0x0F, 0x0B});
  if (mnem == "syscall") return emit_bytes({0x0F, 0x05});
  if (mnem == "endbr64") return emit_bytes({0xF3, 0x0F, 0x1E, 0xFA});
  if (mnem == "cltq" || mnem == "cdqe") return emit_bytes({0x48, 0x98});
  if (mnem == "cqto" || mnem == "cqo") return emit_bytes({0x48, 0x99});
  if (mnem == "cltd" || mnem == "cdq") return emit_bytes({0x99});
  if (mnem == "cwtl" || mnem == "cwde") return emit_bytes({0x98});
  if (mnem == "cwtd" || mnem == "cwd") return emit_bytes({0x66, 0x99});
  if (mnem == "cbtw" || mnem == "cbw") return emit_bytes({0x66, 0x98});
  if (mnem == "int") {
    if (ops.size() == 1 && ops[0].IsImm()) {
      Emit8(0xCD);
      EmitN(static_cast<uint64_t>(ops[0].imm.addend), 1);
      return true;
    }
    return Error("int requires an immediate");
  }

  // Branches.
  if (mnem == "call" || mnem == "callq") return EncodeCallJmp(true, ops);
  if (mnem == "jmp" || mnem == "jmpq") return EncodeCallJmp(false, ops);
  if (mnem.size() >= 2 && mnem[0] == 'j') {
    int cc = ConditionCode(mnem.substr(1));
    if (cc >= 0) return EncodeJcc(cc, ops);
  }
  if (mnem.substr(0, 3) == "set") {
    int cc = ConditionCode(mnem.substr(3));
    if (cc >= 0) return EncodeSetcc(cc, ops);
  }

  // mov-extend family.
  if (mnem.substr(0, 4) == "movz" || mnem == "movslq" || mnem == "movsxd" ||
      (mnem.substr(0, 4) == "movs" && mnem.size() == 6))
    return EncodeMovExtend(mnem, ops);

  // cmovcc.
  if (mnem.substr(0, 4) == "cmov") {
    std::string_view rest = mnem.substr(4);
    unsigned size = 0;
    if (!rest.empty()) {
      char last = rest.back();
      if (last == 'w' || last == 'l' || last == 'q') {
        // Could be a size suffix; try the condition without it first.
        if (ConditionCode(rest) < 0) {
          unsigned sz = last == 'w' ? 2 : last == 'l' ? 4 : 8;
          std::string_view cc = rest.substr(0, rest.size() - 1);
          int c = ConditionCode(cc);
          if (c >= 0) return EncodeCmovcc(c, sz, ops);
        }
      }
    }
    int c = ConditionCode(rest);
    if (c >= 0) return EncodeCmovcc(c, size, ops);
  }

  // Size-suffixed mnemonics.
  std::string base(mnem);
  unsigned size = 0;
  if (!base.empty()) {
    char last = base.back();
    unsigned sz = last == 'b' ? 1 : last == 'w' ? 2 : last == 'l' ? 4
                  : last == 'q'                                   ? 8
                                                                  : 0;
    if (sz) {
      std::string stem = base.substr(0, base.size() - 1);
      if (IsSuffixable(stem)) {
        base = stem;
        size = sz;
      }
    }
  }

  if (mnem == "movabs" || mnem == "movabsq")
    return EncodeMov(8, /*movabs=*/true, ops);

  static const std::unordered_map<std::string, int> alu = {
      {"add", 0}, {"or", 1},  {"adc", 2}, {"sbb", 3},
      {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7},
  };
  if (auto it = alu.find(base); it != alu.end())
    return EncodeAlu(it->second, size, ops);

  if (base == "mov") return EncodeMov(size, false, ops);
  if (base == "lea") return EncodeLea(size, ops);
  if (base == "test") return EncodeTest(size, ops);
  if (base == "not") return EncodeUnary(2, size, ops);
  if (base == "neg") return EncodeUnary(3, size, ops);
  if (base == "mul") return EncodeUnary(4, size, ops);
  if (base == "div") return EncodeUnary(6, size, ops);
  if (base == "idiv") return EncodeUnary(7, size, ops);
  if (base == "imul") return EncodeImul(size, ops);
  if (base == "inc") return EncodeIncDec(false, size, ops);
  if (base == "dec") return EncodeIncDec(true, size, ops);
  if (base == "push") return EncodePush(size, ops);
  if (base == "pop") return EncodePop(size, ops);
  if (base == "shl" || base == "sal") return EncodeShift(4, size, ops);
  if (base == "shr") return EncodeShift(5, size, ops);
  if (base == "sar") return EncodeShift(7, size, ops);
  if (base == "rol") return EncodeShift(0, size, ops);
  if (base == "ror") return EncodeShift(1, size, ops);

  return Error("unknown or unsupported instruction '" + std::string(mnem) + "'");
}

bool Encoder::Encode(std::string_view mnemonic, std::span<const Operand> ops,
                     uint32_t loc) {
  EmitEngine engine(ctx_, diag_, loc);
  return engine.Encode(mnemonic, ops);
}

}  // namespace bcc::as
