# Compiler Architecture: LLVM vs bcc

This document compares the overall architecture of LLVM/Clang with bcc's
current design: the major subsystems, the internal data structures that
carry information between them, and the interfaces that separate them. It
is the "map" for the six area-specific documents that follow.

All LLVM file references are to `llvm-project` `main` (verified against the
repository in November 2026; structure is stable across LLVM 15–21). bcc
references are to this tree at commit `051bbdf`.

---

## 1. LLVM's architecture

### 1.1 The big picture

LLVM is a *collection of libraries* organized around a shared intermediate
representation, not a monolithic compiler. The canonical full compiler
(Clang) is just one frontend; the same backend serves Swift, Rust, Julia,
and hundreds of smaller frontends.

```
           ┌─────────────────────────────────────────────────────┐
           │ clangDriver (option parsing, tool discovery, jobs)  │
           └───────────────────────┬─────────────────────────────┘
                                   ▼ cc1
           ┌─────────────────────────────────────────────────────┐
           │ clang: Lex → Parse → Sema → AST → lib/CodeGen       │
           │       (AST → LLVM IR; a *consumer*, never re-       │
           │        derives semantic facts)                      │
           └───────────────────────┬─────────────────────────────┘
                                   ▼  llvm::Module  (in-memory, SSA)
           ┌─────────────────────────────────────────────────────┐
           │ llvm/lib/Passes: PassBuilder + PassBuilderPipelines │
           │   middle-end: canonicalization + scalar/loop/IPA    │
           └───────────────────────┬─────────────────────────────┘
                                   ▼  still llvm::Module
           ┌─────────────────────────────────────────────────────┐
           │ llvm/lib/CodeGen (target-independent backend)       │
           │  SelectionDAG or GlobalISel → MachineFunction (MIR) │
           │  schedulers, register allocation, PEI, AsmPrinter   │
           └───────────────────────┬─────────────────────────────┘
                                   ▼  MCInst stream
           ┌─────────────────────────────────────────────────────┐
           │ llvm/lib/MC: MCStreamer → MCCodeEmitter +           │
           │  MCAsmBackend (relaxation/fixups) → MCObjectWriter  │
           │  (ELF/Mach-O/COFF/Wasm)  — or textual .s emission   │
           └─────────────────────────────────────────────────────┘
```

Key layering rules:

- **The IR is the contract.** Frontends produce `llvm::Module`; the
  optimizer transforms it; the backend consumes it. Everything above the
  IR knows nothing about targets; everything below it knows nothing about
  source languages.
- **Target descriptions are data.** `llvm/lib/Target/X86/*.td` (TableGen)
  generates instruction definitions, encodings, scheduling models,
  calling conventions, and ISel patterns (`X86InstrInfo.td`,
  `X86InstrArithmetic.td`, `X86CallingConv.td`, `X86Schedule.td` +
  per-microarchitecture `X86Sched*.td`). The generic code in
  `llvm/lib/CodeGen` is driven by these tables through abstract
  interfaces: `TargetMachine`, `TargetLowering`, `TargetRegisterInfo`,
  `TargetInstrInfo`, `TargetFrameLowering`, `TargetSubtargetInfo`.
- **MC is a standalone assembly/object layer.** `llvm/lib/MC` can parse
  `.s` text, encode instructions, apply fixups, and write ELF/Mach-O/COFF
  with no compiler attached. The backend's `AsmPrinter` is simply an
  `MCStreamer` client.

### 1.2 The IR object model (`llvm/lib/IR`, `llvm/include/llvm/IR`)

Everything in the IR hangs off one class hierarchy, and its central
invariant is **bidirectional def–use information**:

- `Value` (`llvm/include/llvm/IR/Value.h`) — anything usable as an
  operand. Owns a *use list*: an intrusive linked list of `Use` objects
  (`Use.h`), one per operand slot that references the value. `User`
  (anything with operands: instructions, constants) owns its `Use[]`
  array. This gives, in O(1): "who uses me" (`use_begin/use_end`,
  `MaterializedUseIterator`), "replace all uses with"
  (`replaceAllUsesWith`, the single most-used mutation in the optimizer),
  and "am I dead" (`use_empty`).
- `Instruction` / `User` store operands in a trailing `Use` array with
  hungarian-prefixed accessors (`getOperand(i)`, `setOperand(i, V)`),
  so a two-operand add is one heap allocation plus the list node.
- Constants and globals live in `LLVMContextImpl`-owned pools
  (`llvm/lib/IR/Constants.cpp`, uniqued like bcc's `IRContext`), so
  constant equality is pointer equality. Constant expressions are folded
  eagerly where possible (`llvm/lib/IR/ConstantFold.cpp` for
  constant-expression folding; `llvm/lib/Analysis/ConstantFolding.cpp`
  for folding instructions given a `DataLayout`).
- `Function`, `BasicBlock`, `Module` own their contents via
  `SymbolTableList` (intrusive ilist + symbol table interning), so
  reordering and removal are cheap and iterators stay valid.
- `IRBuilder` (`llvm/lib/IR/IRBuilder.cpp`) is templated on an
  `InstrFolder` (default `ConstantFolder`): *every* builder operation
  folds constants and simplifies obvious identities before creating a
  node. Codegen that "just emits" still gets `-O0`-quality minimal IR.
- A `Verifier` (`llvm/lib/IR/Verifier.cpp`) checks the IR's structural
  invariants (dominance for phis, terminator rules, type agreement) and
  runs between pipeline stages in `opt`/`llc` (`-verify-each`), during
  asserts builds, and at frontend handoff.

### 1.3 The pass/analysis infrastructure (`llvm/lib/Passes`)

The new pass manager (docs: `llvm/docs/NewPassManager.html`;
implementation: `llvm/include/llvm/IR/PassManager.h`,
`llvm/lib/IR/PassManager.cpp`):

- IR units nest: Module → (CGSCC) → Function → Loop. Each unit has a
  `PassManager<IRUnit>` and an `AnalysisManager<IRUnit>` that caches
  analysis results per-unit (`DominatorTreeAnalysis`, `LoopAnalysis`,
  `TargetLibraryAnalysis`, `ScalarEvolutionAnalysis`, `MemorySSAAnalysis`,
  …).
- Passes are `PassInfoMixin` types implementing
  `PreservedAnalyses run(IRUnit&, IRUnitAnalysisManager&, ...)`. A pass
  returns the *set of analyses it preserved*; the manager invalidates
  cached results accordingly. Analysis results themselves implement
  `invalidate()` for fine-grained survival (e.g. DominatorTree survives
  edge removals it is told about via `DomTreeUpdater`).
- **Adaptors** (`FunctionToLoopPassAdaptor`,
  `ModuleToPostOrderCGSCCPassAdaptor`, …) nest lower-unit pipelines inside
  higher units; **proxies** (`FunctionAnalysisManagerModuleProxy`, …)
  expose analysis managers across unit boundaries, with strict rules
  (outer results reachable from inner passes only if already cached, to
  preserve determinism and avoid quadratic recomputation).
- `PassBuilder` (`llvm/lib/Passes/PassBuilder.cpp`) constructs named
  default pipelines; the actual pipelines live in
  `PassBuilderPipelines.cpp` (`buildO0DefaultPipeline`,
  `buildModuleSimplificationPipeline`, `buildFunctionSimplificationPipeline`,
  `buildModuleOptimizationPipeline`, …). Extension-point callbacks let
  targets and frontends inject passes without editing LLVM.
- `PassInstrumentation` (`llvm/lib/IR/PassInstrumentation.cpp`) +
  `StandardInstrumentations.cpp` provide before/after callbacks: timing
  (`-time-passes`), pass printing (`-print-after-all`), crash reproducer
  (`-ir-verifier-only`…), optimization remarks (ORE).
- The **backend** still runs a machine-level pass sequence via
  `TargetPassConfig::addMachinePasses` (see `machine_codegen.md`); the
  per-target composition is being migrated to
  `CodeGenPassBuilder`/`X86CodeGenPassBuilder.cpp`, but the machine passes
  themselves remain `MachineFunctionPass`es under a light PM wrapper.

### 1.4 Frontend (Clang) architecture

Relevant because bcc's frontend deliberately clones it (see `ir.md`):

- Driver (`clang/lib/Driver`) → cc1 → Lex (`clang/lib/Lex`) → Parse
  (`clang/lib/Parse`) → Sema (`clang/lib/Sema`, which type-checks and
  builds the AST while attaching *implicit conversions* and other
  decisions) → CodeGen (`clang/lib/CodeGen`, a single AST consumer
  producing IR; `CodeGenModule` for the TU, `CodeGenFunction` per
  function, `CodeGenTypes` memoizing AST→IR types, `CGCall.cpp` for ABI
  lowering).
- All semantic decisions are made before IR: CodeGen is "translation plus
  placement" (`Address`/`LValue`/`RValue` wrappers carry alignment and
  value-kind through the lowering).

### 1.5 MC layer (`llvm/lib/MC`)

- `MCContext` (symbols, sections), `MCSection`/`MCFragment` (contents are
  *fragments* — data, instructions, alignment, fill — so late relaxation
  just replaces fragments), `MCStreamer` (abstract instruction/section
  API with `MCObjectStreamer` and `MCTextStreamer` implementations),
  `MCInst`/`MCOperand` (post-selection, pre-encoding instructions),
  `MCCodeEmitter` (target, TableGen-generated: e.g.
  `X86MCTargetDesc/X86MCCodeEmitter.cpp` for ModRM/SIB/REX), and
  `MCAsmBackend` (fixup application + relaxation) feeding
  `MCObjectWriter` (`ELFObjectWriter.cpp`: symbols, relocations,
  sections-to-section indices). The same stack powers `llvm-mc`,
  integrated assembling (`clang -c foo.s`), and the disassembler.

---

## 2. bcc's architecture

bcc today is a frontend plus an IR printer plus a standalone assembler:

```
cc_dump (bin/cc_dump.cc)
  FileManager/SourceManager → Preprocessor (lib bcc_pp)
    → Parser + Sema (lib bcc_parse/bcc_sema) → AST (lib bcc_ast)
    → CodeGenModule (lib bcc_codegen) → ir::Module (lib bcc_ir)
    → IRPrinter → stdout (valid LLVM IR *text*)
                       │
                       ▼  (external process)
                     llc -filetype=asm → .s (AT&T)
                       │
                       ▼
bcc-as (lib bcc_as): Lexer → Parser → Encoder → Sections+Fixups
                       → ElfWriter → ELF64 .o
                       │
                       ▼  (external process)
                     system cc (link)
```

What exists, mapped to LLVM counterparts:

| bcc | LLVM counterpart | Fidelity |
|---|---|---|
| `basic/` (FileManager, SourceManager, DiagnosticsEngine, line tables) | `llvm/Support` + `clang/Basic` | High — same concepts, same file-ID/sloc design |
| `lex/`, `pp/` (lexer, preprocessor, macro expansion, header search, `__has_builtin`, include-guard optimization) | `clang/Lex` | High |
| `parse/`, `sema/`, `ast/` | `clang/Parse`, `clang/Sema`, `clang/AST` | High, C11-only |
| `codegen/` (`CodeGenModule`, `CodeGenFunction`, `CodeGenTypes`, `Address`/`LValue`/`RValue`, `ConstantEmitter`) | `clang/CodeGen` | Deliberately faithful minus: SysV aggregate-ABI classification, varargs, volatile, atomics, `long double`, inline asm, builtins (see `ir.md` §1.4) |
| `ir/` (`Module`, `Function`, `BasicBlock`, `Instruction`, `IRContext`, `IRBuilder`, `IRPrinter`) | `llvm/IR` | **Structural clone, minus the optimizer-facing parts**: no `Use`/use-lists, no `replaceAllUsesWith`, no constant folding in the builder, no Verifier, no attributes/metadata/debug info, fixed `i1/i8/i16/i32/i64` + `ptr` types only |
| — | `llvm/Passes` (middle-end) | **Absent** |
| — | `llvm/CodeGen` (ISel, MIR, RA, scheduling, PEI) | **Absent** (delegated to `llc`) |
| `as/` (`MCContext`, `MCValue`, `Section`+`Fixup`+`AlignPoint`, `SymbolTable`, `Reg`/`RegClass`, `Encoder`, `RelType`/`RelModifier`, `ElfWriter` writing `Elf64_Ehdr/Shdr/Sym/Rela`) | `llvm/MC` | Medium: it is MC's *assembler-input* path (text `.s` → ELF), with REX/ModRM/SIB-aware encoding (`as/register.hh` knows REX extension bits, high-byte regs, spl/bpl REX requirement), relocations, alignment; no fragment model (relaxation is not incremental), no COMDAT/DWARF, single object format |

### 2.1 What this means concretely

- bcc's "backend" is a *process boundary*: `-emit-ir` prints text, `llc`
  does all of ISel/scheduling/RA/PEI/codegen, and bcc-as re-parses the
  result. Quality equals LLVM's, at the cost of depending on a host LLVM
  for codegen, and of the C features that text round-trip exposes (the IR
  subset in `ir.md` §2.4 must stay exactly valid-LLVM).
- Because lowering targets LLVM's IR semantics, several ABI items are
  pushed into "LLVM will do it": varargs (`...` calls lower to IR varargs
  calls and rely on `llc`; bcc's own `<stdarg.h>` cannot define
  `va_start`), aggregate passing (ir.md §1.4 explicitly skips
  classification), `long double` (lowered to `double`).

### 2.2 Strengths worth keeping

- The clang-shaped frontend with all semantic decisions in Sema makes any
  future backend's job minimal — mirrors why Clang CodeGen needs no
  re-checking.
- `IRContext` uniquing + pointer-equality types/constants is exactly
  LLVMContext's model and pays off the moment passes appear.
- The textual-LLVM IR gives a *free differential oracle*: every phase can
  be validated by feeding the output to real LLVM and executing it
  (`ir.md` §4). This remains the backbone testing strategy when native
  backend phases appear (compare bcc-emitted objects vs `llc`-emitted).
- `as/` already encodes the hairy part of x86 (REX/ModRM/SIB, rip-relative,
  relocations) — a native backend should target its `Section`/`Fixup`/
  `ElfWriter` layer rather than reinventing it.

---

## 3. Detailed comparison

### 3.1 Where bcc matches LLVM

1. **Layered libraries with the IR as the seam.** `bcc_ir` depends on
   neither AST nor target (`src/CMakeLists.txt` mirrors the
   `llvm/IR ↔ clang/AST` split, per `ir.md` §2). This is the single most
   important architectural decision and it is already right.
2. **Context-owned uniquing.** `IRContext` (types, constants) ≈
   `LLVMContextImpl`; `ASTContext` ≈ Clang's. Equality-by-pointer is the
   invariant all LLVM passes assume.
3. **Builder-mediated construction** with an insertion point
   (`ir_builder.hh`), matching `IRBuilder` minus folding.
4. **Value/Instruction/Constant class split and opcode enums** — a
   reduced `ValueKind`/`Opcode` clone of `Value::ValueTy`/
   `Instruction::OpCodes`.
5. **Diagnostic infrastructure with source locations**, severities,
   options (`basic/`), matching `clang/Basic/Diagnostics*`.

### 3.2 Where bcc diverges (gaps ranked by consequence)

| # | Gap | LLVM mechanism bcc lacks | Consequence |
|---|-----|--------------------------|-------------|
| 1 | No use-lists on `Value` (`ir/value.hh:31-66` stores only `type_`/`name_`; `Instruction::operands_` is one-directional) | `Use`/`User` intrusive lists, `replaceAllUsesWith`, `use_empty` | No transform pass can be written: every optimization above peephole-on-its-own-output needs "find users" and "substitute definition". `ir.md` §1.4 flags this as the first optimizer prerequisite |
| 2 | No pass/analysis infrastructure | `PassManager`/`AnalysisManager`/`PreservedAnalyses` (`llvm/lib/Passes`) | No place to put optimizations, no caching, no invalidation discipline |
| 3 | No IR verifier | `llvm/lib/IR/Verifier.cpp` | Structural bugs surface far from their cause; differential testing against clang catches miscompiles only at execution time |
| 4 | No middle-end passes | `PassBuilderPipelines.cpp` pipelines | Output of `-emit-ir` is clang-`-O0`-shaped: every local is an `alloca`/`load`/`store` (`%retval` pattern in `codegen_function.cc`), everything redundant |
| 5 | No native backend (ISel/RA/PEI/MC-emission) | `llvm/lib/CodeGen` + per-target dirs | Hard dependency on host `llc`; bcc cannot do varargs, aggregate ABIs, or `long double` correctly even in principle, because those need backend decisions that were delegated |
| 6 | Builder does not fold | `IRBuilder` + `ConstantFolder` | Even trivial constants (`1+2`, `x*1`, `x+0`) materialize as instructions |
| 7 | `as/` has no fragment/relaxation model | `MCFragment`, `MCAsmBackend::relaxInstruction` | Encodings are fixed at parse time (branch displacement choices, nop-padding for alignment beyond `AlignPoint`) |
| 8 | No attributes/metadata on IR | `Attribute`s, TBAA metadata, `!dbg` | No `nofree`/`willreturn`-style facts, no alias precision, no debug info — all irrelevant today, load-bearing once an optimizer exists |
| 9 | Single target hard-wired concepts (SysV x86-64 in AST layout & headers) | `TargetMachine`/`Triple`/`DataLayout` | Fine while there is one target; becomes friction when the native backend lands (recommend keeping "one target" but putting the decisions in one place) |

### 3.3 Philosophical differences

- LLVM optimizes for *many clients*: every layer is a library with stable
  textual/binary formats (`.ll`, `.bc`, MIR, MC). bcc optimizes for
  *learnability and differential testability*: the only format is printed
  `.ll`. That is the right call to keep — even the native backend should
  get a printable form (MIR-style) purely for testing, not for tools.
- LLVM's TableGen centralizes target facts; bcc encodes target facts in
  C++ (`as/register.hh`, AST layout). For one target, C++ tables are
  simpler; adopt TableGen-like *separation of description from algorithm*
  without adopting TableGen itself (see `instruction_selection.md` §5).

---

## 4. What to adopt, in what order

The other documents detail each area. Architecturally, the adoptions that
unlock everything else are:

1. **Use-lists + `RAUW` + `dropAllReferences`** on `bcc::ir::Value`
   (`ir_optimizer.md` §5.1). Prerequisite for every pass. Moderate,
   self-contained change: `Use` nodes intrusive into operands.
2. **A `Verifier`** over the printed-IR invariants (`ir_optimizer.md` §5.2)
   plus a unit-test corpus of invalid IR. Cheap, permanent insurance.
3. **A minimal pass manager**: function-granularity, synchronous
   invalidation ("pass X preserves nothing but DominatorTree") — the
   100-line essence of `PreservedAnalyses` (`ir_optimizer.md` §5.3).
4. **`TargetInfo` seam before the backend**: one class owning x86-64
   facts (register set from `as/register.hh`, calling convention, type
   sizes) so `machine_codegen.md`'s `MachineFunction` layer gets facts
   from one place, the way `TargetLowering`+`TargetRegisterInfo` serve
   LLVM's generic CodeGen.
5. **Unify emission**: keep `as/`'s `Section`/`Fixup`/`ElfWriter` as the
   object-writing layer for a future `MachineInstr → encoding` path;
   add the `MCInst`-equivalent indirection (encoder consumes an
   instruction value, not parser operands) so the same encoder serves
   text-`.s` input and compiler-internal input (`machine_codegen.md` §5).

## 5. Risks and trade-offs

- **Scope discipline.** LLVM's generality (CGSCC, proxies, TableGen,
  plugins) exists to serve many clients; bcc should copy mechanisms, not
  surface area. Rule of thumb adopted in the roadmap: adopt a mechanism
  only when the second concrete consumer exists.
- **Differential testing must evolve, not break.** The clang-diff
  regression runners and execute-`.ll`-with-clang methodology stay the
  acceptance gate for every new pass; each pass additionally gets
  before/after IR fixtures (a FileCheck-lite, `implementation_roadmap.md`
  §6).
- **Process-boundary removal** (native backend) changes failure modes:
  miscompiles are no longer absorbed by LLVM. The roadmap sequences
  correctness infrastructure (verifier, MIR printing, object-level diffing
  against `llc`) *before* the backend takes over any construct.

## 6. Testing and success criteria (architecture-level)

- Every new layer gets: unit tests, a printer (IR or MIR), and a
  differential oracle. Success = the oracle still passes with the layer
  enabled, plus layer-specific metrics (e.g. instruction-count deltas for
  the optimizer, object-diff rate vs `llc` for the backend).
- Existing suites (636 gtest cases; PP/lexer clang-diff regression
  corpora) must stay green throughout — they are the regression backbone
  the roadmap relies on.
