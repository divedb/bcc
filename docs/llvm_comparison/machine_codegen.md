# Machine Code Generation Pipeline: LLVM vs bcc

Covers the machine-level backend as a whole: `MachineFunction`/MIR as a
representation, the full pass sequence from selected code to object file,
prologue/epilogue and frame handling, pseudo-instruction expansion,
branch relaxation, and the AsmPrinter→MC emission path to ELF. Then the
corresponding design for bcc's native backend, built on the existing
`as/` (MC-shaped) object layer.

The LLVM pass list below was extracted from
`llvm/lib/CodeGen/TargetPassConfig.cpp` (`addMachinePasses`, verified
Nov 2026). Per-target composition is migrating to `CodeGenPassBuilder`
(`llvm/lib/Passes/CodeGenPassBuilder.cpp`,
`X86CodeGenPassBuilder.cpp`), which assembles the same passes through
compile-time policy objects — worth knowing, not yet worth copying.

---

## 1. LLVM's implementation

### 1.1 MachineFunction / MIR — the representation

- `MachineFunction` (one per IR function) owns `MachineBasicBlock`s
  (MBBs, mapped from IR blocks + extra blocks created by tail duplication
  etc.), the `MachineFrameInfo` (stack objects: spill slots, allocas,
  variable-size, alignment), `MachineRegisterInfo` (vreg tables, hints),
  and `MachineFunctionInfo` (target extras, e.g. `X86MachineFunctionInfo`).
- `MachineInstr` (header `llvm/include/llvm/CodeGen/MachineInstr.h`):
  opcode (target `MCInstrDesc` row: operand list shape, flags, implicit
  defs/uses, tie info, sched class), `MachineOperand`s — a compact
  tagged union: register (vreg or physreg, with def/use/implicit/early-
  clobber/renamable bits), immediate, predicate, frame index, global
  address (with relocation flags), block address, register mask (calls),
  CPI index, intrinsic id, etc.
- Special pseudo-instructions with IR-like semantics: `PHI` (until PHIElim),
  `COPY`, `IMPLICIT_DEF`, `REG_SEQUENCE`, `INSERT_SUBREG`, and
  target pseudos (`RET_FLAG`, ADJCALLSTACKDOWN/UP, `FRAME_SETUP`-flagged
  prologue ops) — expanded late (`ExpandPostRAPseudos.cpp`, target
  expanders like `X86ExpandPseudo.cpp`).
- MIR is serializable (`llvm/lib/CodeGen/MIRPrinter/MIRParser`) and
  `llc -run-pass=X file.mir` runs *any single machine pass in isolation*
  — the property that makes LLVM's backend testable to death. bcc should
  copy this property, not the serialization syntax.

### 1.2 The pipeline (verified order, `TargetPassConfig.cpp`)

**Pre-ISel (IR):** `CodeGenPrepare` (address-mode formation, alloca
splitting) → (exception/IR work) → ISel prepare.

**ISel:** SelectionDAG or GlobalISel (see `instruction_selection.md`) →
`MachineVerifier` runs between machine passes under asserts.

**Post-ISel machine passes (`addMachineSSAOptimization`, O1+):**
EarlyTailDup → OptimizePHIs → StackColoring →
LocalStackSlotAllocation → DeadMachineInstructionElim →
(target `addILPOpts`, e.g. x86 machine LICM-era hooks) →
EarlyMachineLICM → MachineCSE → MachineSinking → PeepholeOptimizer →
DeadMachineInstructionElim.

**Register pipeline (`addOptimizedRegAlloc`, O1+):**
DetectDeadLanes → InitUndef → ProcessImplicitDefs →
UnreachableMachineBlockElim → LiveIntervals → (optional SSA scheduler) →
LiveIntervals (again) → MachineLoopInfo → **PHIElimination** →
**TwoAddressInstructionPass** → **RegisterCoalescer** →
RenameIndependentSubregs → **pre-RA MachineScheduler** →
**RAGreedy** (or −O0: `addFastRegAlloc` = PHIElim + TwoAddress +
RAFast) → `addPreRewrite` → **VirtRegRewriter** → RegAllocScoring →
StackSlotColoring → MachineCopyPropagation → MachineLICM.

**Post-RA (`addMachinePasses`):** IPRA → MachinePipeliner(opt) →
`addPreRegAlloc` hook → [RA above] → `addPostRegAlloc` →
(opt) PostRAMachineSinking + **ShrinkWrap** →
**PrologEpilogCodeInserter (PEI)** → (O1+) `addMachineLateOptimization`:
MachineLateInstrsCleanup → **BranchFolder** → TailDuplicate →
MachineCopyPropagation → **ExpandPostRAPseudos** → `addPreSched2` →
ImplicitNullChecks(opt) → **post-RA scheduler** → GC passes →
(O1+) **MachineBlockPlacement** (block layout by profile/model) →
instrumentation passes → `addPreEmitPass` (x86:
`X86FixupBWInsts`, `X86FixupLEAs`, `X86ExpandPseudo`, …) →
CFI/layout passes → `addPreEmitPass2` (e.g. branch relaxation — x86 does
it inside AsmParser-era MC relaxation; targets with short/long branches
relax here).

**Emission:** `AsmPrinter::runOnMachineFunction`
(`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`) lowers each MI to
`MCInst` (x86: `X86MCInstLower.cpp`; pseudos resolve here), then streams
it through `MCStreamer`:

- Text path (`-filetype=asm`): `MCInst` → printed AT&T/Intel.
- Object path (`-filetype=obj`): `MCObjectStreamer` fragments the
  section contents; **MCAsmBackend** runs *relaxation*
  (MCObjectStreamer::relaxSections: branch shortening, alignment padding,
  repeated until stable), `MCCodeEmitter::encodeInstruction` produces
  bytes + `MCFixup`s, `MCObjectWriter` (`ELFObjectWriter.cpp`) emits
  symbols/relocations/sections. Relaxation is why MC has fragments:
  instruction size can change after encoding.

### 1.3 PEI in detail (`PrologEpilogInserter.cpp`)

1. Compute callee-saves: intersect target's CSR list with physregs
   actually used (post-RA) → per-function save set; optional shrink
   wrapping (find minimal CFG regions needing the saves).
2. Assign frame-object offsets (`MachineFrameInfo`, alignment-aware,
   callee-saved area + spill area + allocas; `LocalStackSlotAllocation`
   pre-pass helps x86 addressing already).
3. Replace all `FRAME_INDEX` operands with final `[rsp/rbp ± disp]`
   (`replaceFrameIndices` — target hook, x86 in
   `X86FrameLowering.cpp`).
4. Insert prologue/epilogue code (`X86FrameLowering::emitPrologue`:
   push rbp / mov rbp,rsp / pushes / sub rsp,N; `emitEpilogue` mirror).

### 1.4 Calling convention plumbing

`X86CallingConv.td` generates table-driven classification;
`X86ISelLoweringCall.cpp` (split out of X86ISelLowering.cpp, verified)
does SysV argument passing: registers rdi/rsi/rdx/rcx/r8/r9 + xmm0–7
(by class, integer/SSE 8-byte classification for aggregates), stack
spill for the rest, `al` = #vector regs for varargs, register save area
+ `va_start` support, return in rax/rdx (or xmm0/1), hidden sret pointer
when needed. Calls also carry ADJCALLSTACKDOWN/UP pseudos so the frame
and stack-pointer bookkeeping stay explicit.

---

## 2. bcc's implementation

Today's "machine code generation" is split across a process boundary:

- `llc` performs everything in §1 from ISel through MC emission,
  producing AT&T `.s`.
- `bcc-as` (`src/as/`) is a standalone assembler: Lexer → Parser (AT&T
  syntax, operands as `Operand{Reg|Imm|Mem}` per `as/operand.hh`) →
  `Encoder` (REX/ModRM/SIB — the `MCCodeEmitter` role) → `Section`
  objects with `Fixup`s (`RelType`/`RelModifier` — the `MCFixup` role)
  and `AlignPoint`s → `ElfWriter` (`Elf64Ehdr/Shdr/Sym/Rela` — the
  `MCObjectWriter` role) → relocatable ELF64.

What `as/` demonstrates: bcc already has an MC-grade *encoding and object*
layer — including the parts people usually underestimate (REX rules,
rip-relative addressing, relocation modifiers). What it lacks versus MC:
fragment-based relaxation (fixed-size decisions at parse time), a shared
in-memory instruction representation (it encodes from parsed text
operands directly), and any notion of frame/CFI.

What does not exist anywhere in bcc: MachineFunction/MIR, machine
passes, RA (see `register_allocator.md`), PEI, branch handling beyond
what the text says, calling-convention lowering (bcc's codegen relies on
LLVM's for every call — and accordingly can't do varargs or
struct-by-value ABIs itself, per `ir.md` §1.4).

---

## 3. Comparison

| Stage (LLVM) | bcc today | bcc native-backend plan |
|---|---|---|
| Selected MIR (MachineFunction) | — | `MachineFunction`/`MachineBasicBlock`/`MachineInstr` mirroring LLVM's model, vregs-only |
| Machine SSA opts (LICM/CSE/sink) | — | one PeepholeOptimizer-lite pass (fold `mov r,r`, dead store to allocas) |
| PHIElim/TwoAddress/Coalesce | — | as specified in `register_allocator.md` §5.2 |
| RA | — | staged RA-1/2/3 (`register_allocator.md`) |
| PEI | — | PEI-lite: fixed rbp frame, full used-CSR pushes, no shrink wrap |
| Branch folding/layout | — | none initially (ISel order = layout); later: simple tail-merging of return blocks |
| Pseudo expansion | — | `RET`/`CALL`/stack-adj pseudos expand in emit pass |
| Branch relaxation | — | branch-never-shortened policy: always near-jump encoding where legal, else rel32 — decided at selection, revisit when a relaxation loop exists |
| AsmPrinter → MCInst | — | MI → `as/` encoder values (shared with text path) |
| MC relaxation (fragments) | — (fixed encodings) | not needed under the no-short-branch policy; document as permanent simplification or future work |
| Object writing | **exists** (`as/`: sections, fixups, ElfWriter) | reuse unchanged |

---

## 4. What bcc should adopt

1. **The MachineFunction data model**: MBBs, MI with tagged operands
   (reg vreg/phys, imm, frame-index, global-address+reloc, block),
   implicit defs/uses (flags on compares! calls as reg-mask users),
   pseudo-instructions with late expansion. This is the highest-fidelity
   copy in the whole roadmap because it is *the* interface between every
   backend algorithm bcc will implement.
2. **MIR printability + single-pass running**: every machine stage is a
   `FunctionPass` over `MachineFunction` with a printer; tests are
   golden files per stage. (Copy the property; the syntax is up to us.)
3. **The verified pipeline discipline**: MachineVerifier-lite after each
   stage in debug builds (defs dominate uses within a vreg's intervals,
   one terminator/MBB, frame indices referenced before PEI / absent
   after).
4. **PEI's separation**: RA assigns regs; PEI owns *everything* frame
   (CSR set, offsets, FI replacement, prologue/epilogue emission). Keep
   them separate even in the simple version — they mutate different
   invariants.
5. **Explicit call pseudos** (`ADJCALLSTACK`-analogues) so stack-pointer
   bookkeeping stays visible to RA and PEI instead of being baked into
   call encoding.

Not adopted: block placement/profile machinery, instrumentation,
CFI/DWARF, shrink wrapping, fragment relaxation (see §3 policy),
`CodeGenPassBuilder` policy plumbing (bcc's pipeline is one function of
~50 lines; a builder abstraction buys nothing yet).

---

## 5. Concrete design for bcc

### 5.1 Core types (`include/bcc/mi/`)

```cpp
enum class MIOperandKind : uint8_t { kVReg, kPReg, kImm, kFI,
                                     kGlobal, kBlock, kRegMask };
struct MIOperand {
  MIOperandKind kind; unsigned u; int64_t i;
  // kVReg: u = vreg id;        kPReg: u = as::Reg encoding
  // kImm: i;                   kFI: u = frame index
  // kGlobal: u = index into module globals (reloc chosen at emit)
  // kBlock: u = MBB number;    kRegMask: u = mask id
  unsigned flags;  // kIsDef | kIsImplicit | kIsEarlyClobber | kIsKill
};
struct MCIDesc {              // one row per instruction (the MCInstrDesc role)
  const char* name; uint16_t opcode;
  uint8_t num_operands; uint8_t num_defs;
  uint16_t tied_mask;         // operand i tied to operand j (x86 2-addr)
  uint32_t flags;             // kIsCall|kIsTerminator|kMayLoad|kMayStore|
                              // kHasSideEffects|kCommutable
  uint8_t latency, ports;     // machine-model columns (sched doc §5)
};
class MachineInstr {
  const MCIDesc* desc; std::vector<MIOperand> operands;
  DebugLoc-free; // no debug info in scope
};
class MachineBasicBlock { unsigned number; std::list<MachineInstr>;
                          std::vector<MachineBasicBlock*> preds, succs; };
class MachineFunction {
  bcc::ir::Function* ir; FunctionAnalysisManager* fam;   // for passes
  std::vector<std::unique_ptr<MachineBasicBlock>> blocks;
  MachineRegisterInfo mri; MachineFrameInfo frame;
};
```

Instruction table: ~100 x86-64 entries (`ADD64rm`-style names reused for
traceability against LLVM's `X86InstrInfo.td`), each with encoder hints
consumed by a new `Encoder::EncodeMI(MachineInstr&, Section&)` overload —
the same REX/ModRM/SIB code paths `as/` already has, now driven from MI
instead of parsed operands.

### 5.2 Machine pass interface + pipeline

```cpp
class MachineFunctionPass {
 public: virtual bool Run(MachineFunction&) = 0;
};
// bcc-native pipeline (driver -mbackend=native):
// GenericLower → Legalize → AddressForm → Select
//   → [verifier] → PHIElim → TwoAddrRepair → Coalesce-lite
//   → LiveIntervals → RA-1/2/3 → VirtRegRewrite
//   → PEI-lite → ExpandPseudos → EmitObject (as/ layer)
```

### 5.3 Calling convention (SysV x86-64, the critical spec)

Implement as a small classifier over the AST-derived signature
(LLVM reference: `X86CallingConv.td` + `X86ISelLoweringCall.cpp`):

- Scalars: INTEGER class → rdi,rsi,rdx,rcx,r8,r9 (ints/pointers, any
  width ≤ 8 — upper bits undefined for <8 bytes), SSE → xmm0–7
  (float/double).
- Aggregates ≤16 bytes: per-8-byte classification (INTEGER/SSE), passed
  in register pairs; >16 bytes: MEMORY (caller pushes copy, pointer
  passed). Return: rax (+rdx/xmm1 pair); >16 bytes → hidden first
  argument `sret` with pointer return.
- Varargs: named args per above; `al` = number of SSE regs used; callees
  with va support spill the register save area in the prologue
  (`bcc`'s `<stdarg.h>` `va_list` struct — already shipped — is exactly
  this layout: gp_offset/fp_offset/overflow/reg_save_area).
- Stack alignment 16 at call sites; red zone not used (frame-pointer
  discipline chosen instead).

This is the single most correctness-critical component of the native
backend; test it against the psABI's own examples and against
differential calls into glibc (`printf` varargs is the acid test).

### 5.4 PEI-lite

Frame layout (fixed order, rbp-based):
`[saved regs][rbp][return addr] ← rbp | locals+spill+allocas | outgoing
args` with 16-byte call-site alignment padding; prologue = pushes + `mov
rsp,rbp` variant chosen accordingly; epilogue mirrored. CSR set = all
used callee-saved (rbx, r12–r15, rbp) — computed from RA's physreg
usage.

### 5.5 Emission

`EmitObject` walks MBBs in order (layout = selection order initially),
expands pseudos, maps `kGlobal` operands to `as/` symbols + `RelType`
choices, appends encoded bytes + `Fixup`s into `.text`/`.data`/`.rodata`
`Section`s, and reuses `ElfWriter` verbatim. `.s` text emission
(same MI walker through the AT&T printer of `as/`) stays available as a
debug mode — bcc's version of AsmPrinter's two streamers.

---

## 6. Testing strategy and success criteria

- **MIR golden files** per stage (per pass, per pattern) — exact-match,
  stable numbering, deterministic output.
- **Object-level differential**: for each corpus function, bcc-native
  `.o` vs `llc`-produced `.o`: compare *behavior* by linking both into
  harnesses; additionally disassemble both (objdump) and diff
  instruction *classes* (not encodings) as a quality signal.
- **ABI conformance**: a dedicated suite of struct-by-value / varargs /
  mixed-signature call tests exercising every §5.3 rule, cross-compiled
  with system gcc to link against gcc-compiled callers/callees (bcc .o
  must interoperate with glibc and gcc code — that is the real ABI
  contract).
- **Verifier-lite** in debug runs after every machine stage.
- **Success criteria, M4** (see roadmap): corpus fully executable via
  native path; zero behavior diffs vs `llc` path; object sizes within
  2.5× of `llc -O0`; MIR printers golden-stable.

## 7. Challenges and trade-offs

- **ABI ownership is all-or-nothing per construct**: the moment the
  native backend compiles a function, *its* ABI decisions rule. Strategy:
  keep `-mbackend=llc` (current text path) as the default until the
  corpus is green, then flip by construct-safe order (scalars →
  aggregates → varargs), with a driver escape hatch per TU.
- **EFLAGS modeling**: compares define flags; conditionals consume. MI
  marks EFLAGS as implicit def/use (LLVM does exactly this); RA must
  keep flag intervals short (LLVM's `X86FlagsCopyLowering.cpp` handles
  the pathological cases — bcc simply re-materializes compares when
  flags don't survive, initially).
- **Relaxation-free branch policy**: always encoding `jcc rel32` costs
  bytes, not correctness; revisit with an MC-fragment-style pass only if
  size matters.
- **The `as/` encoder as single source of encoding truth** means the
  text and native paths share bugs *and* tests — a feature; keep the
  encoder API operand-shaped (not parser-shaped) to make that clean.
