# bcc Improvement Roadmap: Learning from LLVM

This is the execution plan synthesizing the six area documents
(`compiler_architecture.md`, `ir_optimizer.md`,
`instruction_selection.md`, `instruction_scheduler.md`,
`register_allocator.md`, `machine_codegen.md`). Each milestone lists:
scope, prerequisites, concrete work items, the LLVM mechanism being
adopted, acceptance criteria, and estimated size. Estimates assume the
existing codebase's standards (style, unit tests per component,
clang-diff regression corpus stays green at every step).

Priorities were chosen by: (1) unblock everything else first
(use-lists, verifier), (2) user-visible quality per engineering hour
(mem2reg — the `-O0`-shape fix), (3) correctness-critical ownership
(ABI/RA) before quality-critical (scheduling), and (4) never breaking
the differential-testing oracle that makes this project verifiable.

---

## Milestone M0 — IR manipulation foundations

**Adopting**: LLVM's `Use`/`User` bidirectional def–use model +
`Verifier` (`llvm/lib/IR/Verifier.cpp`).

- `Use` nodes intrusive into `Value`; `Instruction` operands become a
  `Use[]`; `replaceAllUsesWith`, `use_empty`, `dropAllReferences`
  (`ir_optimizer.md` §5.1).
- `IRVerifier` with the invariant list from `ir_optimizer.md` §5.2;
  wired into `cc_dump` (post-codegen, and `-verify-each` between
  passes) and every future pass's tests.
- Constant folding core (shared folder): integer identities + cast
  folds, used by `IRBuilder` (folds even at `-O0`, like LLVM's
  `ConstantFolder`) and later by passes.
- **Unit tests**: use-list mutation invariants; ~20 verifier
  negative tests; printer unchanged (no output diffs at `-O0` beyond
  folded constants).
- **Size**: ~1.5k lines. **Criteria**: all 636 existing tests green;
  `-O0` output byte-identical except legally folded constants.

## Milestone M1 — Pass manager + first scalar pipeline

**Adopting**: the new PM's *shape* at function granularity
(`PassManager`/`AnalysisManager`/explicit invalidation —
`ir_optimizer.md` §5.3), LLVM's round-based canonicalization pipeline
shape.

- `FunctionPassManager` + `FunctionAnalysisManager` + pass registration
  in `cc_dump -O1`.
- `DominatorTreeAnalysis` (Cooper-Harvey-Kennedy; full recompute on
  invalidation).
- Passes, in landing order: **mem2reg** (Cytron promote; the
  highest-value single pass — removes bcc's alloca/load/store sea),
  **DCE**, **SimplifyCFG-lite**, **InstCombine-lite**, **EarlyCSE**
  (+same-alloca store→load forwarding), **SCCP-lite**
  (`ir_optimizer.md` §5.4–5.5).
- Round-based pipeline `×3` with change-driven iteration cap.
- **Tests**: per-pass IR fixtures (exact-match); differential execution
  `-O0` vs `-O1` across the corpus; `-stats`-lite metrics.
- **Size**: ~3.5k lines. **Criteria**: zero non-aggregate entry-block
  allocas remain at `-O1` on the corpus; ≥60% instruction-count
  reduction vs `-O0` on scalar kernels; 100% differential-execution
  green; verifier green between every pass.

## Milestone M2 — Frontend ABI completion (pre-backend enabler)

Not an LLVM-mechanism milestone, but the native backend cannot land
while glibc headers don't parse or varargs doesn't exist:

- Predefined target macros (`__x86_64__`, `__linux__`, `__LP64__`,
  `__STDC_VERSION__`, …) in `Preprocessor` (clang's
  `InitializePredefinedMacros` analog) — known blocker for
  `#include <stdio.h>` (documented when the built-in headers landed).
- `va_start`/`va_arg`/`va_end`/`va_copy` as IR intrinsics
  (`llvm.va_start`-shaped), emitted by bcc codegen, initially *lowered
  by llc* (valid-LLVM text path), natively in M4/M5.
- **Criteria**: hosted hello-world (`#include <stdio.h>`, `printf`
  varargs) runs end-to-end through the text path; corpus grows by the
  hosted-C subset.

## Milestone M3 — Machine IR layer + native instruction selection

**Adopting**: GlobalISel's staged discipline (mechanical generic
lowering → legalize → address-form → select, each printable/testable) on
LLVM's `MachineFunction` data model; CGP's address shaping; the X86
`.td` pattern inventory as C++ tables (`instruction_selection.md` §5).

- `bcc/mi/` types (MachineFunction/MBB/MI/MIOperand/MCIDesc), MIR
  printer, MachineVerifier-lite, instruction table (~100 x86-64 ops with
  latency/port columns), `Encoder::EncodeMI` bridging to `as/`
  (`machine_codegen.md` §5.1).
- SysV calling-convention classifier + call pseudo lowering
  (`machine_codegen.md` §5.3) — including varargs register-save-area
  prologues.
- Stages: GenericLower → Legalize (i1/i8/i16 promotion rules) →
  AddressForm (MemOperand shapes) → Select (pattern table:
  `instruction_selection.md` §5.5).
- First register strategy is deliberately trivial (everything through
  stack slots except a couple of scratch regs) so M3's acceptance is
  *selection correctness in isolation*.
- **Size**: ~5k lines. **Criteria**: scalar int/FP corpus functions
  compile via `-mbackend=native` to `.o` (through `as/` unchanged) and
  execute correctly; MIR golden tests per stage; per-pattern C coverage
  tests; no reliance on `llc` for any selected construct.

## Milestone M4 — Native RA-1 (local) + PEI-lite: first fully-native TUs

**Adopting**: RAFast's design (block-local, boundary spill/reload),
PEI's separation of frame ownership (`register_allocator.md` §5.4,
`machine_codegen.md` §5.4).

- PHI-elim + parallel-copy lowering + TwoAddress repair (tied operands).
- RA-1 local allocator (caller-saved free lists, LRU spill within block,
  ABI precolored args).
- PEI-lite (rbp frames, used-CSR push/pop, FI replacement), pseudo
  expansion, `EmitObject` through `as/`.
- **Size**: ~2.5k lines. **Criteria**: whole corpus (incl. hosted tests
  from M2) runs `-mbackend=native` end-to-end with zero behavior diffs
  vs the `llc` path; object size ≤ 3× `llc -O0`; deterministic
  output.

## Milestone M5 — Global RA (linear scan → greedy-lite) + coalescing

**Adopting**: LiveIntervals as liveness currency; RAGreedy's
assign/evict/spill loop minus splitting (`register_allocator.md` §5.5–
5.6); CalcSpillWeights priorities.

- SlotIndexes-lite, LiveIntervals, union-find coalescing-lite.
- RA-2 linear scan; then RA-3 greedy-lite (eviction with depth guard,
  constant rematerialization, callee-saved preference across calls).
- **Criteria**: spills < 5% of vregs on scalar corpus; dynamic
  instruction count within 1.5× of `llc -O0` on kernels; RA-3 ≤ RA-2
  spills; stress functions (100+ live values) correct and
  reproducible.

## Milestone M6 — Machine peephole + optional scheduling; quality pass

**Adopting**: PeepholeOptimizer/BranchFolder-shaped cleanups; only if
metrics justify, the post-RA list scheduler
(`instruction_scheduler.md` §5).

- Machine copy-prop, dead-store-to-alloca elim, `cmp+jcc` adjacency
  peephole, return-block tail-merge.
- Optional Sched-M (off by default; ships only on measured modeled-cycles
  win — the doc's negative-result clause stands).
- **Criteria**: native path ≤ 2× object size vs `llc -O0` (was 3× in
  M4); copy counts after coalesce < 10% of instructions; scheduler
  decision documented with numbers either way.

## Milestone M7 (stretch) — IR-level upgrades unlocked by the backend

- Aggregate ABI correctness through the native path (SysV struct
  classification already in M3; here: retire the `ir.md` §1.4 deviation
  for aggregate passing on both paths).
- `long double` honesty: either keep documenting the double deviation
  on both paths or adopt x87 — a project decision forced by having two
  backends (status quo: document).
- MemorySSA-lite → GVN-lite upgrade; Reassociate; loop passes (LICM)
  if `LoopInfo` exists by then.
- `input/sqlite/sqlite3.c` as compile+run target (blocked until M2's
  varargs + predefined macros are fully in).

---

## Dependency graph

```
M0 ──► M1 ──────────────┐
 │                       ▼
 └──► M3 ──► M4 ──► M5 ──► M6 ──► M7
M2 (frontend ABI) ─── feeds M3/M4 acceptance corpora; independent of M0/M1
```

M0→M3 is a soft edge (MI layer benefits from use-lists/verifier habits
but does not require them); M2 is parallelizable with M0/M1 — good
second workstream.

---

## Testing strategy (cross-cutting)

1. **Never lose an oracle.** The current stack of oracles stays: gtest
   units, clang-diff PP/lexer regressions, execute-`.ll`-via-clang
   differential (`ir.md` §4). Every milestone adds its own oracle before
   changing behavior:
   - M0/M1: IR fixtures + `-O0` vs `-O1` execution differential.
   - M3+: MIR golden files; per-stage `-run-pass`-style single-stage
     mode; object-diff vs `llc` (behavior gate, byte-diff as metric).
   - M4+: ABI interop suite (bcc `.o` linked against gcc/glibc code).
2. **FileCheck-lite**: a tiny fixture-matching helper (substring +
   `{{regex}}` lines against printed IR/MIR) keeps tests declarative
   like `llvm/test/*` without importing LLVM tooling.
3. **Determinism policy**: every new printer and pass must produce
   identical output run-to-run (ordered containers, stable numbering).
   Golden files enforce this for free.
4. **Metrics harness** (`-stats`-lite): instruction/alloca/spill/copy
   counts + object size per corpus file, emitted as CSV for milestone
   acceptance reports.

## Measurable success criteria (end-state, post-M6)

| Metric | Target (vs `llc -O0`) | Measured by |
|---|---|---|
| Corpus correctness | 100% identical program behavior | differential harness |
| Scalar-kernel dynamic instr. count | ≤ 1.5× | execution counts |
| Object size | ≤ 2× | `.text` bytes |
| Spills (RA-3) | < 5% of vregs | `-stats` |
| `-O1` IR instr. count vs `-O0` | ≥ 60% reduction | `-stats` |
| Host toolchain dependence | none for compile+assemble+link bcc code (host `cc` for linking only) | pipeline audit |

## Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Use-list refactor destabilizes codegen output | Medium | Byte-identical `-O0` gate in M0 (only folded constants may change) |
| mem2reg phi bugs (classic source of subtle miscompiles) | Medium | Verifier phi checks + dominance fixtures + execution differential on every corpus function |
| ABI mistakes in native path (varargs/struct rules) | High impact, Medium | M2 texts the varargs ground through `llc` first; M3/M4 ABI suite links against gcc-compiled harnesses |
| Scope creep toward LLVM-scale generality | High (project-wide) | Rule: adopt a mechanism on its *second* concrete consumer; every "not adopted" list in the area docs is normative |
| Scheduler phase delivers nothing measurable | Medium | Explicit negative-result clause: ship disabled with data |
| sqlite corpus unblocks late | Medium | M7 stretch only; scalar corpus designed to stand alone |
| Regression suite breakage mid-refactor | Low | Every milestone lands with corpus green; no long-lived red branches |

## Suggested landing order within each milestone

Each milestone = its own PR series ending in a commit of the shape the
repo already uses (small, self-contained, tests-first where feasible):
(1) types/printers + unit tests, (2) transformations behind a driver
flag default-off, (3) flag flip + corpus green, mirroring how the
preprocessor and codegen phases were landed historically.
