// End-to-end tests for the standalone x86-64 assembler.
//
// The encoding "golden" byte sequences were validated byte-for-byte against
// `llvm-mc -triple=x86_64-linux-gnu`, so this suite is hermetic (it needs no
// external assembler at test time).

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "bcc/as/elf.hh"
#include "bcc/as/lexer.hh"
#include "bcc/as/mccontext.hh"
#include "bcc/as/parser.hh"
#include "gtest/gtest.h"

namespace bcc::as {
namespace {

// A snapshot of one recorded fixup, decoupled from the (soon-destroyed)
// symbol table so tests can inspect it safely.
struct FixupInfo {
  uint8_t size = 0;
  bool pcrel = false;
  bool is_branch = false;
  std::string sym;
};

struct Assembled {
  std::vector<uint8_t> text;
  std::vector<uint8_t> data;
  std::vector<FixupInfo> text_fixups;
  std::vector<uint8_t> object;
  std::map<std::string, uint64_t> text_syms;  // defined .text label -> offset
  unsigned errors = 0;
  std::string messages;
};

// Assembles `src` fully (front end + ELF writer) and returns the finalized
// `.text`/`.data` bytes, snapshots of the `.text` fixups, and the object image.
Assembled Assemble(const std::string& src) {
  Lexer lexer(src);
  MCContext ctx;
  std::ostringstream errs;
  AsmDiag diag("test.s", lexer, errs);
  Parser parser(lexer, ctx, diag);
  parser.Run();
  ctx.RelaxBranches();

  Assembled r;
  r.errors = diag.num_errors();
  r.messages = errs.str();
  for (const Fixup& f : ctx.text()->fixups()) {
    r.text_fixups.push_back({f.size, f.pcrel, f.is_branch,
                             f.value.sym ? f.value.sym->name : std::string()});
  }

  for (Symbol* sym : ctx.symtab().symbols())
    if (sym->defined && sym->section == ctx.text())
      r.text_syms[sym->name] = sym->offset;

  ElfWriter writer(ctx.sections(), ctx.symtab());
  writer.Build(r.object);  // patches resolved fixups into the section bytes

  r.text = ctx.text()->data();
  if (Section* d = ctx.data(); !d->data().empty()) r.data = d->data();
  return r;
}

// Minimal ELF reader helpers over the produced object image.
uint16_t RdU16(const std::vector<uint8_t>& o, size_t p) {
  return static_cast<uint16_t>(o[p] | (o[p + 1] << 8));
}
uint32_t RdU32(const std::vector<uint8_t>& o, size_t p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(o[p + i]) << (8 * i);
  return v;
}
uint64_t RdU64(const std::vector<uint8_t>& o, size_t p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(o[p + i]) << (8 * i);
  return v;
}

// Collects the relocation types (low 32 bits of r_info) across all .rela
// sections in the object.
std::vector<uint32_t> RelocTypes(const std::vector<uint8_t>& o) {
  std::vector<uint32_t> types;
  uint64_t shoff = RdU64(o, 40);
  uint16_t shnum = RdU16(o, 60);
  for (uint16_t i = 0; i < shnum; ++i) {
    size_t sh = shoff + static_cast<size_t>(i) * 64;
    if (RdU32(o, sh + 4) != kShtRela) continue;
    uint64_t off = RdU64(o, sh + 24), size = RdU64(o, sh + 32);
    for (uint64_t j = 0; j < size / 24; ++j)
      types.push_back(static_cast<uint32_t>(RdU64(o, off + j * 24 + 8)));
  }
  return types;
}

std::vector<uint8_t> TextOf(const std::string& insn) {
  return Assemble("\t.text\n\t" + insn + "\n").text;
}

#define EXPECT_ENCODES(insn, ...) \
  EXPECT_EQ(TextOf(insn), (std::vector<uint8_t>{__VA_ARGS__})) << insn

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

TEST(Lexer, TokenStream) {
  Lexer lex("movq $1, %rax\n");
  EXPECT_EQ(lex.Next().kind, TokKind::kIdentifier);  // movq
  EXPECT_EQ(lex.Next().kind, TokKind::kDollar);      // $
  Token num = lex.Next();
  EXPECT_EQ(num.kind, TokKind::kNumber);
  EXPECT_EQ(num.value, 1u);
  EXPECT_EQ(lex.Next().kind, TokKind::kComma);
  Token reg = lex.Next();
  EXPECT_EQ(reg.kind, TokKind::kRegister);
  EXPECT_EQ(reg.text, "rax");  // '%' stripped
  EXPECT_EQ(lex.Next().kind, TokKind::kNewline);
  EXPECT_EQ(lex.Next().kind, TokKind::kEof);
}

TEST(Lexer, NumbersAndComments) {
  Lexer lex("0x10 0b101 010 42  # comment\n");
  EXPECT_EQ(lex.Next().value, 0x10u);
  EXPECT_EQ(lex.Next().value, 5u);
  EXPECT_EQ(lex.Next().value, 8u);   // octal
  EXPECT_EQ(lex.Next().value, 42u);
  EXPECT_EQ(lex.Next().kind, TokKind::kNewline);  // comment consumed
}

//===----------------------------------------------------------------------===//
// Instruction encoding (golden vectors validated against llvm-mc)
//===----------------------------------------------------------------------===//

TEST(Encode, DataMovement) {
  EXPECT_ENCODES("movq %rax, %rbx", 0x48, 0x89, 0xc3);
  EXPECT_ENCODES("movl $1, %eax", 0xb8, 0x01, 0x00, 0x00, 0x00);
  EXPECT_ENCODES("movb $5, %al", 0xb0, 0x05);
  EXPECT_ENCODES("movzbl %al, %eax", 0x0f, 0xb6, 0xc0);
  EXPECT_ENCODES("movslq %eax, %rax", 0x48, 0x63, 0xc0);
}

TEST(Encode, Arithmetic) {
  EXPECT_ENCODES("addq $10, %rbx", 0x48, 0x83, 0xc3, 0x0a);
  EXPECT_ENCODES("cmpq $0, %rdi", 0x48, 0x83, 0xff, 0x00);
  EXPECT_ENCODES("xor %eax, %eax", 0x31, 0xc0);
  EXPECT_ENCODES("negq %rax", 0x48, 0xf7, 0xd8);
  EXPECT_ENCODES("imulq $3, %rcx, %rdx", 0x48, 0x6b, 0xd1, 0x03);
  EXPECT_ENCODES("shlq $4, %rax", 0x48, 0xc1, 0xe0, 0x04);
  EXPECT_ENCODES("test %rax, %rax", 0x48, 0x85, 0xc0);
  // accumulator short form
  EXPECT_ENCODES("testq $0x10, %rax", 0x48, 0xa9, 0x10, 0x00, 0x00, 0x00);
}

TEST(Encode, Addressing) {
  EXPECT_ENCODES("lea 8(%rbp), %rax", 0x48, 0x8d, 0x45, 0x08);
  EXPECT_ENCODES("mov (%rax,%rbx,4), %rcx", 0x48, 0x8b, 0x0c, 0x98);
  // %rsp base forces a SIB byte
  EXPECT_ENCODES("mov (%rsp), %rax", 0x48, 0x8b, 0x04, 0x24);
  // %r13 base needs an explicit disp8=0
  EXPECT_ENCODES("mov (%r13), %rax", 0x49, 0x8b, 0x45, 0x00);
}

TEST(Encode, StackAndControl) {
  EXPECT_ENCODES("push %rbp", 0x55);
  EXPECT_ENCODES("push %r12", 0x41, 0x54);
  EXPECT_ENCODES("pop %r12", 0x41, 0x5c);
  EXPECT_ENCODES("ret", 0xc3);
  EXPECT_ENCODES("syscall", 0x0f, 0x05);
  EXPECT_ENCODES("endbr64", 0xf3, 0x0f, 0x1e, 0xfa);
}

TEST(Encode, HighRegistersNeedRex) {
  // spl requires a REX prefix even though it is otherwise a low-8 register.
  EXPECT_ENCODES("movb %sil, %dil", 0x40, 0x88, 0xf7);
  // ah/bh must NOT get a REX prefix.
  EXPECT_ENCODES("movb %ah, %al", 0x88, 0xe0);
}

//===----------------------------------------------------------------------===//
// Labels, branches, and relocations
//===----------------------------------------------------------------------===//

TEST(Branch, LocalForwardResolvesAndRelaxes) {
  // A local jump to a nearby label resolves in-place with no relocation and
  // relaxes to a 2-byte rel8 form.
  Assembled a = Assemble(
      "\t.text\n"
      "\tjmp .L1\n"
      ".L1:\n"
      "\tret\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  EXPECT_TRUE(RelocTypes(a.object).empty());  // resolved in-place, no relocation
  // eb 00 (jmp rel8, disp 0), then c3.
  EXPECT_EQ(a.text, (std::vector<uint8_t>{0xeb, 0x00, 0xc3}));
}

TEST(Relax, BackwardJccToRel8) {
  // je back to a label a few bytes earlier relaxes to 74 <disp8>.
  Assembled a = Assemble(
      "\t.text\n"
      ".L0:\n"
      "\tnop\n"
      "\tje .L0\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  // 90 (nop), 74 fd (je -3: from end 0x03 back to 0x00).
  EXPECT_EQ(a.text, (std::vector<uint8_t>{0x90, 0x74, 0xfd}));
}

TEST(Relax, FarBranchStaysRel32) {
  // A forward jump over more than 127 bytes cannot use rel8; it stays as a
  // 5-byte rel32 jmp.
  std::string src = "\t.text\n\tjmp .Lend\n";
  for (int i = 0; i < 200; ++i) src += "\tnop\n";
  src += ".Lend:\n\tret\n";
  Assembled a = Assemble(src);
  EXPECT_EQ(a.errors, 0u) << a.messages;
  EXPECT_TRUE(RelocTypes(a.object).empty());
  EXPECT_EQ(a.text[0], 0xe9);              // still rel32
  // rel32 displacement = 200 (the 200 nop bytes between the jmp and .Lend).
  EXPECT_EQ(a.text[1], 200);
  EXPECT_EQ(a.text[2], 0x00);
  ASSERT_EQ(a.text.size(), 5u + 200u + 1u);
}

TEST(Relax, AlignmentStaysCorrectAfterShrink) {
  // A relaxable branch precedes a `.p2align 4`. After the branch shrinks from
  // 5 to 2 bytes, the aligned label must still land on a 16-byte boundary
  // (padding is recomputed), and it must not have moved to a stale offset.
  Assembled a = Assemble(
      "\t.text\n"
      "\tjmp .Lskip\n"
      "\tnop\n"
      ".Lskip:\n"
      "\t.p2align 4\n"
      "aligned:\n"
      "\tret\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  ASSERT_TRUE(a.text_syms.count("aligned"));
  EXPECT_EQ(a.text_syms["aligned"] % 16, 0u) << "aligned label must be 16-byte";
  // jmp relaxed to eb, so 'aligned' sits at 16 (not the pre-relaxation 16+ slot).
  EXPECT_EQ(a.text_syms["aligned"], 16u);
  EXPECT_EQ(a.text[0], 0xeb);  // jmp relaxed to rel8
}

TEST(Relax, ExternalBranchNotRelaxed) {
  // A branch to an undefined/global symbol keeps rel32 + relocation.
  Assembled a = Assemble("\t.text\n\tjmp foo\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  EXPECT_EQ(a.text[0], 0xe9);  // rel32 jmp
  EXPECT_EQ(a.text.size(), 5u);
  EXPECT_EQ(RelocTypes(a.object),
            (std::vector<uint32_t>{static_cast<uint32_t>(RelType::kPLT32)}));
}

TEST(Branch, ExternalCallEmitsPlt32) {
  Assembled a = Assemble("\t.text\n\tcall foo\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  ASSERT_EQ(a.text_fixups.size(), 1u);
  const FixupInfo& f = a.text_fixups[0];
  EXPECT_TRUE(f.is_branch);
  EXPECT_TRUE(f.pcrel);
  EXPECT_EQ(f.size, 4);
  EXPECT_EQ(f.sym, "foo");
  // opcode e8 present; the rel32 field is left zero for the linker.
  EXPECT_EQ(a.text[0], 0xe8);
  // An external branch links through the PLT: R_X86_64_PLT32 (type 4).
  EXPECT_EQ(RelocTypes(a.object),
            (std::vector<uint32_t>{static_cast<uint32_t>(RelType::kPLT32)}));
}

TEST(Data, DirectivesAndSymbolRelocations) {
  Assembled a = Assemble(
      "\t.data\n"
      "x:\n"
      "\t.byte 1, 2, 3\n"
      "\t.long 0x11223344\n"
      "\t.quad x\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  ASSERT_GE(a.data.size(), 7u);
  EXPECT_EQ(a.data[0], 1);
  EXPECT_EQ(a.data[1], 2);
  EXPECT_EQ(a.data[2], 3);
  // little-endian .long
  EXPECT_EQ(a.data[3], 0x44);
  EXPECT_EQ(a.data[6], 0x11);
}

TEST(Directive, SetFoldsToConstant) {
  Assembled a = Assemble(
      "\t.set N, 7\n"
      "\t.text\n"
      "\tmovl $N, %eax\n");
  EXPECT_EQ(a.errors, 0u) << a.messages;
  // movl $7, %eax  ->  b8 07 00 00 00 (constant folded, no relocation)
  EXPECT_TRUE(RelocTypes(a.object).empty());
  EXPECT_EQ(a.text, (std::vector<uint8_t>{0xb8, 0x07, 0x00, 0x00, 0x00}));
}

//===----------------------------------------------------------------------===//
// ELF object structure
//===----------------------------------------------------------------------===//

TEST(Elf, WellFormedHeader) {
  Assembled a = Assemble(
      "\t.text\n"
      "\t.globl _start\n"
      "_start:\n"
      "\tret\n");
  ASSERT_GE(a.object.size(), 64u);
  EXPECT_EQ(a.object[0], 0x7f);
  EXPECT_EQ(a.object[1], 'E');
  EXPECT_EQ(a.object[2], 'L');
  EXPECT_EQ(a.object[3], 'F');
  EXPECT_EQ(a.object[4], kElfClass64);
  EXPECT_EQ(a.object[5], kElfData2Lsb);

  auto u16 = [&](size_t off) {
    return static_cast<uint16_t>(a.object[off] | (a.object[off + 1] << 8));
  };
  EXPECT_EQ(u16(16), kEtRel);     // e_type = ET_REL
  EXPECT_EQ(u16(18), kEmX8664);   // e_machine = EM_X86_64
}

TEST(Error, UnknownInstructionReported) {
  Assembled a = Assemble("\t.text\n\tfrobnicate %rax\n");
  EXPECT_GT(a.errors, 0u);
}

}  // namespace
}  // namespace bcc::as
