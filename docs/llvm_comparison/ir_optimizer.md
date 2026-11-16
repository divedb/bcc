# IR Optimizer Implementation Plan: LLVM-grounded, bcc-specific

This document is the step-by-step engineering plan for bcc's IR
optimization infrastructure. It replaces the earlier overview: the LLVM
vs bcc comparison is still here, but organized *per implementation step*
(LLVM mechanism → current bcc ground truth → exact changes → tests →
metrics). Sibling documents: `compiler_architecture.md` (system view),
`instruction_selection.md` / `register_allocator.md` /
`machine_codegen.md` (backend, which consumes this work), and
`implementation_roadmap.md` (where this plan is milestones M0–M1).

All bcc facts below were verified against the tree at commit `051bbdf`
(quirks that matter to the plan are called out with file:line).
LLVM references are to `llvm-project` `main` (verified Nov 2026).

---

## 0. Conventions used by every step

### 0.1 Build, run, inspect (repository-actual)

```sh
# configure + build the pieces this plan touches
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)" --target cc_dump bcc_opt_test

# run the optimizer test target (created in Phase 1)
./build/tests/bcc_opt_test

# inspect optimization results on a program
./build/cc_dump -emit-ir -O0 /tmp/t.c     # current behavior
./build/cc_dump -emit-ir -O1 /tmp/t.c     # optimized (Phase 3)
./build/cc_dump -emit-ir -O1 -verify-each -stats /tmp/t.c

# differential execution (the oracle; ir.md §4 methodology)
./build/cc_dump -emit-ir -O0 t.c > /tmp/o0.ll && clang /tmp/o0.ll -o /tmp/o0
./build/cc_dump -emit-ir -O1 t.c > /tmp/o1.ll && clang /tmp/o1.ll -o /tmp/o1
/tmp/o0 > /tmp/o0.out; echo "exit=$?" >> /tmp/o0.out
/tmp/o1 > /tmp/o1.out; echo "exit=$?" >> /tmp/o1.out
diff /tmp/o0.out /tmp/o1.out && echo PASS
```

`clang` on the emitted `.ll` works because bcc's IR prints as valid LLVM
(`src/ir/ir_printer.cc`) — this is the standing correctness oracle and
every phase below keeps it green.

### 0.2 IR facts a developer must know (all verified in-tree)

| Fact | Where | Consequence for this plan |
|---|---|---|
| `Value` has type + name hint only; **no use lists** | `include/bcc/ir/value.hh:31-66` | Step 0.1 adds them; nothing else can be built first |
| `Instruction` stores `std::vector<const Value*> operands_`; `SetOperand` exists but is protected and tracks nothing | `include/bcc/ir/instruction.hh:103-114` | Step 0.1 replaces the vector with `Use` records |
| `BasicBlock` owns instructions via `std::vector<std::unique_ptr<Instruction>>`; only `Append` / `Insert(index)` exist — **no erase, no replace, no phi insertion helper** | `include/bcc/ir/basic_block.hh:25-47` | Step 0.2 adds container surgery |
| `Function` owns blocks via `std::vector<std::unique_ptr<BasicBlock>>`; only `AppendBlock` exists | `include/bcc/ir/function.hh:40-52` | Step 0.2 adds block removal/reorder |
| `BasicBlock` is itself a `Value` (branch target); phi operands are interleaved `[value, block, value, block, ...]` | `include/bcc/ir/instruction.hh:339-355` | RAUW works uniformly on values *and* branch targets; phi surgery needs pairing-aware APIs |
| `IRContext` uniques types and simple constants; equality is pointer equality | `include/bcc/ir/ir_context.hh:19-82` | Analyses/passes key on pointers (no deep compare); `GetInt32(v)`, `GetFP(ty, v)`, `GetNullPtr()` are the folding constructors |
| `IRBuilder` is header-only, constructs via `make_unique` + `block_->Append(...)`; allocas insert at entry-block index `num_allocas_++` | `include/bcc/ir/ir_builder.hh:20-153` | Folding hooks in Step 0.4 go directly in each `Create*` |
| Codegen emits clang-`-O0` shape: every local/param an entry alloca, `retval` alloca + `ReturnBlock` funnel | `src/codegen/codegen_function.cc`, `codegen_stmt.cc` | mem2reg (Step 2.1) is the highest-leverage pass |
| Fixed opcode set: mem (`alloca/load/store/gep`), int/FP binops, `icmp/fcmp`, 11 casts, `br/condbr/switch/ret/unreachable/phi`, `call` | `include/bcc/ir/instruction.hh:15-64` | The folder/verifier/passes enumerate exactly this set — no unknown-opcode handling needed |
| Integer widths: i1/i8/i16/i32/i64 only; FP: float/double; opaque `ptr` | `include/bcc/ir/type.hh`, `ir_context.hh:32-40` | Folding arithmetic must mask to the width; no i128 cases |
| `CmpPredicate` mixes icmp (`kEQ..kUGE`) and fcmp (`kOEQ..kOGE`) in one enum | `include/bcc/ir/instruction.hh:220-228` | Folder switches on opcode first, then predicate |
| Branch `kCondBr` operands: `[cond, true_bb, false_bb]`; switch: `[cond, default, (case value, case block)*]` | `instruction.hh:266-306` | CFG successor walk (Step 0.2) is a 3-case switch |
| `Module` owns `IRContext` + globals + functions; declarations have no blocks | `include/bcc/ir/module.hh`, `function.hh:38` | Pass pipeline loops `GetBlocks()` non-empty functions only |
| Test pattern to mirror: `CodeGenTest::EmitIR(code)` → printed IR string, `Contains()` matcher | `tests/unittest/frontend/codegen_test.cc:15-35` | New `OptTest` fixture (§Phase 2) composes `EmitIR` + pass pipeline |

---

## 1. Phase 0 — IR mutation foundations

**Goal**: make the IR *transformable*, not just constructible: def–use
edges both ways, container surgery, a verifier, and a folding core.
**Prerequisites**: none (this phase touches only `bcc_ir`).
**Deliverables**: `Use`/RAUW, CFG utilities, verifier, folder, builder
folding. All 636 existing tests stay green and `-O0` output stays
byte-identical except legally folded constants.
**Completion criteria**: §each step below; overall gate at §1.5.

### 1.1 Step 0.1 — Use lists: `Use`, `RAUW`, `dropAllReferences`

**Optimization objective.** Give every `Value` a doubly-linked list of
its uses, so a pass can (a) find all users of a value, (b) atomically
substitute a new definition everywhere (`replaceAllUsesWith` — the
primitive *every* transformation is built from), (c) test deadness in
O(1) (`use_empty`), and (d) unlink operands before destruction
(`dropAllReferences`). Expected effect: unlocks all passes; no direct
code-quality change.

**LLVM implementation.** `llvm/include/llvm/IR/Use.h` and `Use.h`'s
`Use` class (`Value* V, User* U, prev/next` — an intrusive list node
embedded in the operand array), `llvm/include/llvm/IR/Value.h`
(`use_begin`, `uses()`, `materialized_use_empty`), `Value::replaceAllUsesWith`
(`llvm/lib/IR/Value.cpp` — walks the list, swaps each `Use`'s value,
updates the new value's list), `User::dropAllReferences`
(`llvm/lib/IR/User.cpp`). LLVM keeps operands as a trailing `Use[]`
allocated with the `User` so instruction allocation stays one-block.
Operands are mutated *only* through `Use::set`, which is what maintains
both directions — there is no path that can desynchronize the lists.

**Current BCC.** `Value` (`value.hh:31-66`) stores only
`value_kind_/type_/name_`. `Instruction::operands_`
(`instruction.hh:113`) is one-directional; `SetOperand` exists
(protected, `instruction.hh:109`) but updates nothing. `ir.md` §1.4
documents this as the deliberate deferral that ends here. Integration
point: the only writers of operands today are instruction constructors
plus `PhiNode::AddIncoming`/`SwitchInst::AddCase`
(`instruction.hh:184,296` via `AddOperand`) — a handful of sites.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/ir/use.hh` | **new** — `Use` node |
| `include/bcc/ir/value.hh` | add use-list members + public `use_begin/use_empty/ReplaceAllUsesWith/DropAllReferences`; protected `AddUse/RemoveUse` |
| `include/bcc/ir/instruction.hh` | `operands_` becomes `std::vector<Use>`; `AddOperand/SetOperand` maintain lists; `GetOperand` reads through `Use` |
| `src/ir/instruction.cc` | ctor/dtor updates (build/unlink `Use`s); `BranchInst/SwitchInst/PhiNode` accessors cast `Use`'s value to `BasicBlock*` as today |
| `src/ir/ir_printer.cc` | none (reads `GetOperand`) — proves API compatibility |

**Data structures & interfaces.**

```cpp
// include/bcc/ir/use.hh
class Value; class Instruction;
class Use {
 public:
  Value* GetUser() const;               // the instruction owning this slot
  unsigned GetOperandIndex() const;
  Value* Get() const;                   // the used value (mutable view)
  void Set(Value* to);                  // THE mutation primitive
 private:
  Value* val_ = nullptr;                // constant-folded view is via Get()
  Instruction* user_ = nullptr;         // bcc: only Instructions have operands
  unsigned idx_ = 0;
  Use* prev_ = nullptr; Use* next_ = nullptr;
  friend class Value; friend class Instruction;
};

// value.hh additions
class Value {
 public:
  bool UseEmpty() const { return use_list_ == nullptr; }
  Use* UseBegin() { return use_list_; }        // iterate user/idx
  /// Replace every reference to `this` with `to` (type-checked in asserts).
  void ReplaceAllUsesWith(Value* to);
  /// Unlink all operands held by `this` (destructor path for instructions).
  void DropAllReferences();
 protected:
  void AddUse(Use&); void RemoveUse(Use&);
  Use* use_list_ = nullptr;                    // intrusive, not owned
};

// instruction.hh: operands_ becomes
std::vector<Use> uses_;      // Use order == operand order (GetOperand(i)
                             // returns uses_[i].Get(); phi keeps its
                             // interleaved layout inside this vector)
```

Invariants: exactly one `Use` per operand slot, list membership only
through `Use::Set`/`Instruction` ctor/dtor; `Use` nodes die with their
instruction (vector element — *not* heap nodes, so `std::vector`
reallocation must re-link: implement by giving `Instruction` a move-safe
reserve — simplest correct choice: build `uses_` once in the constructor
with `reserve(n)` and never grow except via the two append APIs which
re-link on relocation; document that `AddOperand` may invalidate `Use`
pointers, which nothing in bcc holds long-term).

**Step-by-step implementation.**
1. Add `use.hh`; port `Value` members; compile `bcc_ir` with use-lists
   threaded through `Instruction`'s ctor (`for v in ops: AddUse`).
2. Rewrite `AddOperand`/`SetOperand` to route through `Use::Set`.
3. Implement `ReplaceAllUsesWith`: walk `use_list_`, `u->Set(to)`;
   debug-assert `to->GetType()` null-compat or equal to `this` type
   (`BasicBlock` uses have null type — allow).
4. Implement `DropAllReferences` (iterate `uses_`, `RemoveUse`) and call
   it from a new `Instruction` destruction path; make `~Instruction`
   assert `UseEmpty()` in debug builds (catches double-use bugs).
5. Audit `basic_block.hh`/`function.hh` destruction order: instructions
   destroyed → their `Use`s unlinked → `Value` dtor asserts list empty.
6. Grep-audit every `GetOperand` caller (printer, codegen) — none must
   hold `const Value*` identity assumptions across RAUW.

Edge cases: RAUW with `to == this` (no-op, return early); RAUW on a
`GlobalVariable`/`Function`/`Constant` (constants are uniqued and
immortal — allowed, nothing special); use of an instruction *by itself*
(never legal, verifier catches later).

**Testing.** New `tests/unittest/ir/use_list_test.cc` (new directory +
target wiring, see §1.5): build a function via `IRBuilder`; assert
`UseEmpty` transitions, RAUW rewrites a load's pointer operand and the
old alloca becomes `UseEmpty`, `DropAllReferences` empties uses of both
operands, RAUW on a `BasicBlock` retargets a `br` (this is how CFG
rewrites will work). Regression gate: entire existing suite
(`./build/tests/bcc_*_test`) green; `cc_dump -emit-ir` output on the
codegen test corpus byte-identical to before this step.

Acceptance: RAUW/UseEmpty/DropAllReferences behave as specified; no
output drift; existing 636 tests pass.

**Performance evaluation.** No quality metric yet. Measure compile-time
neutrality: `time ./build/cc_dump -emit-ir input/sqlite-derived.c`
before/after (use any large single file; sqlite itself is blocked on
frontend gaps) — expect < 3% delta. Memory: `valgrind --tool=massif` on
one corpus file before/after to confirm the `Use` vector adds O(1) per
instruction.

### 1.2 Step 0.2 — Container surgery + CFG utilities

**Optimization objective.** The mutation APIs passes need: erase/replace
instructions, insert phis at block top, erase/reorder blocks, redirect
edges, and query the CFG (successors/predecessors). LLVM's equivalents:
`BasicBlock::getInstList()` mutations, `BasicBlock::removePredecessor`
(which also prunes phi entries — `llvm/lib/IR/BasicBlock.cpp`),
`llvm/IR/CFG.h` (`succ_begin/succ_end, pred_begin/pred_end` walking
terminators and predecessors lists maintained by
`SuccessorListTraits`).

**Current BCC.** Only growth APIs exist (`BasicBlock::Append`,
`BasicBlock::Insert(index)`, `Function::AppendBlock`). Predecessors are
computable only by scanning all terminators. Phi entries can be added
(`AddIncoming`) but never removed; phi creation via `IRBuilder::CreatePhi`
appends at the *current* insertion point, which is only phi-legal when
the block is fresh — passes inserting phis into existing blocks need a
top-insertion helper.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/ir/basic_block.hh` | add `EraseInstruction`, `ReplaceInstruction`, `InsertBefore`, `PrependPhi`; non-const `GetTerminator` |
| `include/bcc/ir/instruction.hh` | `PhiNode::RemoveIncoming(unsigned)`, `PhiNode::GetIncomingIndexForBlock(BasicBlock*)` |
| `include/bcc/ir/function.hh` | `EraseBlock(BasicBlock*)` (unlink + unique_ptr release), `InsertBlockAfter` |
| `include/bcc/ir/cfg.hh` | **new** — successors/predecessors/reachability |

```cpp
// cfg.hh (header-only, bcc_ir)
std::vector<BasicBlock*> Successors(const BasicBlock*);   // switch on
    // terminator opcode: kBr → {dest}; kCondBr → {t,f}; kSwitch →
    // {default, cases...}; kRet/kUnreachable → {}
std::vector<BasicBlock*> Predecessors(const Function*, const BasicBlock*);
    // scan all blocks' terminators (O(F)); passes needing repeated
    // queries build a map once
std::vector<BasicBlock*> ReversePostorder(Function&);     // DFS from entry
bool IsReachable(Function&, const BasicBlock*);

// basic_block.hh additions
Instruction* EraseInstruction(Instruction*);   // unlink uses, erase from
    // vector, return next instruction
Instruction* ReplaceInstruction(Instruction* old,
                                std::unique_ptr<Instruction> nu);
    // RAUW old→nu (if nu non-void-typed), then EraseInstruction
void PrependPhi(std::unique_ptr<PhiNode>);     // insert before first
    // non-phi (phis grouped at top — matches printer + verifier rule)
void ReplaceSuccessorsUses(BasicBlock* from, BasicBlock* to); // via RAUW
```

Invariants: `EraseInstruction` must be refused (assert) on a terminator
unless the caller is replacing it; `EraseBlock` requires the block's
value (branch-target) uses to be gone — callers redirect edges first;
phi mutation goes through `RemoveIncoming` so operand pairs stay
interleaved.

**Step-by-step implementation.**
1. `cfg.hh` first (pure reads; unit-testable immediately).
2. `BasicBlock` surgery (erase/replace/insert-before/prepend-phi).
3. `Function::EraseBlock` + `PhiNode` removal APIs.
4. Predecessor maintenance: *computed*, not stored (LLVM stores
   predecessor lists for O(1); bcc passes tolerate O(F) queries or build
   a map — document this as the deliberate simplification and the hook
   where an intrusive list would go if profiling ever demands it).

Edge cases: erasing an instruction that a phi still references (must
`UseEmpty` — assert); erasing the last instruction of an unterminated
block (legal — block was mid-construction); empty block erasure.

**Testing.** `tests/unittest/ir/cfg_test.cc`: successors of all five
terminator kinds (built via `IRBuilder`); predecessors with a diamond;
RPO order on a loop (back edge last); reachability with an unreachable
block. `use_list_test.cc` grows RAUW-a-block + erase + phi-entry-removal
cases. Acceptance: all APIs behave per spec; suite green.

**Performance evaluation.** Compile-time neutral by construction; no
action beyond the suite runtime check.

### 1.3 Step 0.3 — IR Verifier

**Optimization objective.** A structural checker that runs (a) after
codegen in `cc_dump`, (b) between passes under `-verify-each`, (c) in
every optimizer unit test — catching malformed IR at the boundary where
it was created instead of as a mysterious downstream miscompile. This is
LLVM's `llvm/lib/IR/Verifier.cpp` (`Verifier::verifyFunction`, called
from `verifyModule`), whose checks (phi dominance, terminator form, type
agreement) are the direct model. LLVM runs it between every pipeline
stage in `opt`/`llc` under assertions — the habit this step installs.

**Current BCC.** Nothing. Bugs would surface only as invalid text
rejected by `clang`, or as wrong runtime results in differential tests —
far from their origin.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/ir/verifier.hh` | **new** — `std::vector<std::string> VerifyModule(const Module&)` (+ per-function entry) |
| `src/ir/verifier.cc` | **new** — the checks |
| `src/CMakeLists.txt` | add `ir/verifier.cc` to `bcc_ir` |

Design decision (deviation from LLVM, noted): bcc's verifier returns
strings instead of using `DiagnosticsEngine`, so `bcc_ir` gains **no**
dependency on `bcc_basic` (LLVM's verifier lives in lib/IR and uses the
LLVMContext diagnostics; bcc's layering keeps `bcc_ir` dependency-free,
which the CMake graph currently guarantees).

**Checks (each maps to a `Verifier.cpp` counterpart).**

1. Every block's last instruction is a terminator and no earlier
   instruction is (`Verifier::visitTerminator` rules).
2. Operand types: binop operands identical and equal to result; cmp
   operands identical (icmp: integer/ptr; fcmp: float/double); casts:
   operand type compatible per opcode (`trunc` narrower, `sext/zext`
   wider, `ptrtoint/inttoptr` involve `ptr`); `load` pointer operand is
   `ptr`; `gep` base `ptr` + all indices integer; `store` value type ==
   pointee implied by... (opaque pointers: `store` value type is
   unconstrained — check pointer operand only); `call` callee is `ptr`
   or function and arg count/type matches `GetFunctionType()`
   (`CallInst::GetFunctionType` carries it, `instruction.hh:368`).
3. Phi: only at block top (contiguous prefix); one incoming entry per
   predecessor, no duplicates, each incoming block is an actual
   predecessor; incoming values' types equal the phi type; each incoming
   value dominates the end of its incoming block (needs DT — Phase 1;
   until then check the weaker "value's parent block dominates" via
   reachability, and upgrade in Step 1.2).
4. `switch` case values are `ConstantInt` of the condition's type and
   unique (`visitSwitchInst`).
5. Module: unique global names; global initializer is a `Constant` of
   the value type; function redefinitions absent.
6. Every used value is either a constant/argument/global or an
   instruction whose parent chain resolves into the same function
   (catches cross-function operand leaks).
7. Entry block exists; alloca convention (entry-block allocas only) as a
   *warning-level* string prefix (bcc codegen contract, not IR legality).

**Step-by-step implementation.** Implement checks in the order above;
each is an independent commit-able unit. Keep the API allocation-free on
success (return empty vector).

**Testing.** `tests/unittest/ir/verifier_test.cc`: ~20 negative fixtures
(one per check — build invalid IR with `IRBuilder` by hand where
constructible, e.g. two terminators, phi not first, mismatched binop
operand widths, switch duplicate case) and 5 positive fixtures (every
existing codegen test's IR verifies clean — implement by running the
verifier inside `OptTest`'s baseline path in Phase 2). Acceptance: all
negative fixtures produce the expected message substring; verifier
accepts every IR the current codegen produces (run over the whole
codegen test corpus as an integration test inside
`tests/unittest/ir/verifier_test.cc` reusing `FrontendTest`).

**Performance evaluation.** `time cc_dump -emit-ir` with and without
verification on the largest corpus file: verification must add < 10%
(it is linear in instructions); measured once, then only in debug runs.

### 1.4 Step 0.4 — Constant folder + builder folding

**Optimization objective.** One shared routine that computes the value
of pure operations over constants: integer/FP binops, icmp/fcmp, casts.
Two consumers: `IRBuilder` folds at construction (LLVM's
`IRBuilder<ConstantFolder>` behavior — `llvm/include/llvm/IR/IRBuilder.h`,
folding via `llvm/lib/Analysis/ConstantFolding.cpp` +
`llvm/lib/IR/ConstantFold.cpp`), and later passes fold destructively.
Expected effect: `-O0` output stops containing `%x = add i32 1, 2`
instruction nodes (today the builder emits them unconditionally); code
size of emitted IR shrinks modestly; subsequent passes get constants for
free. No semantic change — folding is exact.

**Current BCC.** No folding anywhere. `src/codegen/constant_emitter.cc`
folds *static initializers* at the AST level — unrelated machinery, left
alone.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/ir/folder.hh` | **new** — pure-function folding API |
| `src/ir/folder.cc` | **new** — implementations |
| `include/bcc/ir/ir_builder.hh` | each `Create*` consults the folder; on hit, return the folded `Constant*` instead of an instruction |
| `src/CMakeLists.txt` | add `ir/folder.cc` |

```cpp
// folder.hh — all functions return nullptr when not foldable.
namespace bcc::ir {
const Constant* FoldBinOp(Opcode, const Constant* lhs, const Constant* rhs,
                          IRContext&);
const Constant* FoldICmp(CmpPredicate, const Constant*, const Constant*, IRContext&);
const Constant* FoldFCmp(CmpPredicate, const Constant*, const Constant*, IRContext&);
const Constant* FoldCast(Opcode, const Constant*, const Type* dest, IRContext&);
}
```

Semantics pinned in the implementation:
- Integer ops compute in `uint64_t`, mask to the type width
  (`IntegerType::GetBits()`), with sign where the opcode needs it
  (`sdiv/srem` sign-extend operands first; `lshr/ashr` shift counts
  ≥ width or negative shifts are **not folded** — LLVM leaves poison to
  `ConstantExpr`; bcc has no poison, so the rule is: if C says UB, the
  folder may fold *or* keep — pin to *keep* for shifts/div so behavior
  matches `-O0` clamping concerns never arise); division by zero:
  never folded.
- FP ops use `double` arithmetic; float-typed results round through the
  `ConstantFP` invariant (value exactly representable —
  `value.hh:124-137`); `fdiv` by 0.0 not folded.
- Casts: exact-width tables (trunc masks, sext/zext sign/zero extend,
  `sitofp` on the sign-extended value, `ptrtoint/inttoptr` on
  `ConstantNullPtr`/`ConstantInt(0)` → null/0 only).
- `icmp` implements all 10 predicates on `GetSExtValue()` /
  raw patterns; `fcmp` the 6 ordered/unordered predicates on doubles,
  with NaN handled via the C `!=` semantics of `kUNE`.

**Step-by-step implementation.**
1. `folder.cc` + exhaustive unit tests (`tests/unittest/ir/folder_test.cc`,
   one test per opcode per interesting boundary: INT_MIN, width edges,
   shift widths, NaN, ±0.0).
2. Thread the folder into `IRBuilder`: `CreateBinOp` checks both
   operands `IsConstant()`; on fold, return the `Constant*` (builders
   currently return `BinaryOperator*` — widen return types to `Value*`
   where needed; ~10 call sites in `src/codegen/*.cc` adjust
   mechanically; `codegen_value.hh`'s `RValue` already holds
   `ir::Value*`).
3. Builder folding of `CreateICmp/CreateFCmp/CreateCast` likewise.
4. Never fold `call`, memory ops, or anything with side effects.

Edge cases/trade-offs: folding changes `-O0` text (e.g. `add i32 1, 2`
no longer appears) — the Phase 0 gate permits exactly this delta and the
codegen test expectations in `tests/unittest/frontend/codegen_test.cc`
are updated in the same commit (mechanical: expectations there reference
`add i32` for non-constant operands — audit each `Contains` line).

**Testing.** `folder_test.cc` (unit, exhaustive); codegen suite updated;
differential execution over the regression corpus with folded `-O0` must
stay 100% green (this is the step's real gate).

**Performance evaluation.** Metric: instruction count of `-emit-ir` on
10 fixed programs before/after (expect −2..8% on constant-heavy code);
report via a one-off `wc`-based script (`cc_dump -emit-ir f.c | grep -c
'^  %'`). Compile-time check as in §1.1.

### 1.5 Phase 0 exit gate

- [ ] `./build/tests/bcc_as_test bcc_basic_test bcc_frontend_test bcc_lex_test bcc_pp_test` all green (636+ tests).
- [ ] `-O0` IR identical except constant folding; every diff explained.
- [ ] Verifier: 20+ negative fixtures fail with expected messages; whole codegen corpus verifies clean.
- [ ] Folder: exhaustive unit table green; differential corpus green.

New test-target wiring (used from here on): add
`tests/unittest/ir/` files to a new executable in `tests/CMakeLists.txt`:

```cmake
add_executable(bcc_ir_test
  unittest/ir/cfg_test.cc
  unittest/ir/folder_test.cc
  unittest/ir/use_list_test.cc
  unittest/ir/verifier_test.cc
)
target_link_libraries(bcc_ir_test PRIVATE bcc_ir GTest::gtest_main)
gtest_discover_tests(bcc_ir_test)
```

(`bcc_ir_test` needs no frontend; the verifier's corpus integration test
lives in the optimizer target below where `FrontendTest` is available.)

---

## 2. Phase 1 — Pass infrastructure and analyses

**Goal**: the 100-line essence of LLVM's new pass manager at function
granularity, plus the one analysis everything needs (dominators).
**Prerequisites**: Phase 0.
**Deliverables**: `bcc_opt` library, `FunctionPassManager`,
`FunctionAnalysisManager`, `DominatorTreeAnalysis` (+ frontiers), MIR-free
`OptTest` fixture.
**Completion criteria**: §2.3.

### 2.1 Step 1.1 — `FunctionPass` / `FunctionPassManager` / analyses

**Optimization objective.** A uniform home for transformations: passes
implement `Run(Function&, FunctionAnalysisManager&) -> bool` (changed?);
the manager runs sequences, caches analyses, and invalidates on change
with explicit preservation. LLVM: `llvm/include/llvm/IR/PassManager.h`
(`PassManager<IRUnit>::run`, `AnalysisManager` caching +
`invalidate(IRUnit, PreservedAnalyses)`), `PreservedAnalyses`
sets/preserve-set idiom, `PassInstrumentation`
(`llvm/lib/IR/PassInstrumentation.cpp`) for timing/printing. bcc adopts
the shape, not the template metaprogramming: function granularity only,
bool changed instead of `PreservedAnalyses` sets, explicit
`Invalidate<T>()` calls instead — the honest 20-line version of the same
discipline, upgradeable later.

**Current BCC.** No pass concept exists; the driver calls codegen
directly. The `OptTest` fixture (this step) composes
`FrontendTest`-lowered IR with pass execution exactly as
`CodeGenTest` composes it with printing
(`tests/unittest/frontend/codegen_test.cc:18-45`).

**Files.**

| Path | Change |
|---|---|
| `include/bcc/opt/opt_pass.hh` | **new** — `Analysis`, `FunctionPass`, `FunctionAnalysisManager`, `FunctionPassManager` |
| `src/opt/pass_manager.cc` | **new** (small: manager run loop + timing hooks) |
| `src/CMakeLists.txt` | **new library** `bcc_opt` → `target_link_libraries(bcc_opt PUBLIC bcc_ir)` |
| `tests/CMakeLists.txt` | new `bcc_opt_test` target (grows through Phase 2/3) |
| `tests/unittest/opt/opt_test.hh` | **new** — fixture |

```cpp
// opt_pass.hh
class Analysis { public: virtual ~Analysis() = default; };

class FunctionAnalysisManager {
 public:
  template <class A> A& GetResult(Function&);        // compute-or-cached
  template <class A> void Invalidate();              // drop cached
  void InvalidateAll();
 private:
  std::unordered_map<std::type_index, std::unique_ptr<Analysis>> cache_;
};

class FunctionPass {
 public:
  virtual ~FunctionPass() = default;
  /// \return whether anything changed (drives iteration + invalidation).
  virtual bool Run(Function& f, FunctionAnalysisManager& fam) = 0;
  /// Analyses this pass promises still-valid after a change (empty =
  /// invalidate everything). Convention, enforced by the manager.
  virtual bool PreservesDominators() const { return false; }
  virtual const char* GetName() const = 0;
};

class FunctionPassManager {
 public:
  void AddPass(std::unique_ptr<FunctionPass>);
  /// Runs the sequence; repeats the whole sequence up to \p max_rounds
  /// while any pass reports change (fixpoint shape like LLVM's iterated
  /// InstCombine/SimplifyCFG groups in PassBuilderPipelines.cpp).
  bool Run(Function& f, unsigned max_rounds = 3);
};
```

Manager loop semantics: before each pass with cached analyses: nothing;
after a pass returns `true` (changed): if `!PreservesDominators()`
invalidate `DominatorTreeAnalysis` (and everything transitively — with
two analyses total, "all" is fine); return `false`: invalidate nothing.
Instrumentation: optional `SetObserver(PassObserver*)` with
`BeforePass/AfterPass` — enough for `-time-passes`-style reporting and
pass-by-pass IR printing (`-print-after-all` equivalent) in Step 3.1.

**Testing.** `tests/unittest/opt/pass_manager_test.cc`: a fake analysis
(count computations) + fake passes (touch/no-touch) verifying caching,
invalidation-on-change, preservation, round iteration stops at fixpoint.
**Performance evaluation.** None beyond suite runtime (infrastructure).

### 2.2 Step 1.2 — DominatorTree analysis (+ dominance frontiers)

**Optimization objective.** `DominatorTreeAnalysis` provides: immediate
dominators, `Dominates(A, B)`, dominator-tree children, and dominance
frontiers (`DF(B)`) — the two consumers are mem2reg (phi placement) and
the verifier upgrade (phi-operand dominance). LLVM:
`llvm/lib/IR/Dominators.cpp` with `DomTreeBuilder.hpp` (semi-NCA with
snaking, Buchheim et al. + Georgiadis' verification); LLVM's *legacy*
`DominanceFrontier` (`llvm/include/llvm/Analysis/DominanceFrontier.h`)
is the classic Cytron et al. structure. bcc implements the Cooper-
Harvey-Kennedy "A Simple, Fast Dominance Algorithm" (2001) two-loop
iterative idom computation over RPO — ~150 lines, provably correct for
arbitrary graphs including the irreducible CFGs `goto` can produce —
plus the CHK dominance-frontier construction from the dom tree.

**Current BCC.** No CFG analysis exists anywhere (Sema/parser work on
the AST; codegen emits CFG mechanically). Depends on `cfg.hh` (Step 0.2)
for RPO and successors.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/opt/dominators.hh` | **new** — `DominatorTree` (analysis result), `DominatorTreeAnalysis` (pass-facing factory) |
| `src/opt/dominators.cc` | **new** — CHK idoms, frontiers, `Dominates` via walk-up |
| `tests/unittest/opt/dominators_test.cc` | **new** |

```cpp
class DominatorTree : public Analysis {
 public:
  static DominatorTree Compute(Function&);          // CHK algorithm
  BasicBlock* GetIDom(BasicBlock*) const;
  bool Dominates(const BasicBlock* a, const BasicBlock* b) const;
      // walk b's idom chain (depth-cached); a==b ⇒ true
  bool Dominates(const Instruction* a, const Instruction* b) const;
      // block dominance + a before-or-equal position within the block
  std::vector<BasicBlock*> Frontiers(BasicBlock*) const;  // DF set
  const std::vector<BasicBlock*>& GetReversePostorder() const;
};

class DominatorTreeAnalysis {  // registered in FunctionAnalysisManager
 public:
  using Result = DominatorTree;
  static DominatorTree Run(Function& f);
};
```

**Step-by-step implementation.**
1. RPO via `cfg.hh` DFS (iterative; deterministic child order = block
   order in `Function::GetBlocks()`).
2. CHK loop 1 (compute idoms): intersect() over RPO until fixpoint.
3. `Dominates` via depth-numbered ancestor walks.
4. Frontiers: for each block with ≥2 preds, runner-walk from each pred
   to idom, adding the block to the runner's DF (the CHK/CF algorithm
   from the same paper — the standard `computeDF` loop).
5. Instruction-level `Dominates` (position within block: index in
   `GetInstructions()`; phis are "at the top").

Edge cases: unreachable blocks (CHK needs all blocks reachable — for
unreachable blocks assign entry as idom and empty DF; document; better:
`SimplifyCFG` deletes them before DT is computed — pass ordering note);
self-loops; `goto`-built irreducible edges (algorithm handles; add a
test); single-block functions.

**Testing.** `dominators_test.cc` builds IR via source → codegen
(fixture from §2.1) for shapes: straight line, diamond, while loop
(verify header dominates body), nested loops, `goto` cross edges
(irreducible) — assert `Dominates` truth tables and DF sets against
hand-computed values. Also property test: `Dominates(entry, b)` for all
b; idom is unique. **Performance evaluation.** Micro: DT build time on
synthetic 10k-block chain/loop functions (< 50 ms); correctness via
tests; invalidation correctness via manager tests (Step 1.1).

### 2.3 Phase 1 exit gate

- [ ] `bcc_opt` library builds; `bcc_opt_test` runs (manager + dominators suites).
- [ ] Verifier's phi check upgraded to full dominance (uses DT when available; string-flagged otherwise).

---

## 3. Phase 2 — The passes

All passes live in `bcc_opt`, follow one file-each convention, and are
tested through the fixture below. Order is dependency- and leverage-
ordered: mem2reg first (transforms the `-O0` shape everything else
feeds on), then cleanup (DCE, SimplifyCFG), then algebra (InstCombine,
EarlyCSE), then flow (SCCP).

**Shared fixture** (`tests/unittest/opt/opt_test.hh`):

```cpp
/// Lowers source to IR, runs a pass pipeline, returns printed IR.
class OptTest : public FrontendTest {              // frontend_fixture.hh
 protected:
  std::string Optimize(std::string_view code,
                       std::vector<std::unique_ptr<bcc::opt::FunctionPass>>
                           pipeline,
                       unsigned rounds = 3) {
    Parse(code);
    EXPECT_EQ(NumErrors(), 0u) << "input does not sema-check";
    auto module = std::make_unique<ir::Module>();
    codegen::CodeGenModule cgm(*ctx_, *diags_, *module);
    cgm.EmitTranslationUnit();
    for (auto& f : module->GetFunctions()) {       // Module::GetFunctions
      if (f->IsDeclaration()) continue;            // function.hh:38
      bcc::opt::FunctionPassManager fpm;
      for (auto& p : pipeline) fpm.AddPass(std::move(p));
      fpm.Run(*f, rounds);
    }
    EXPECT_TRUE(ir::VerifyModule(*module).empty());
    std::ostringstream os; ir::IRPrinter p(os); p.Print(*module);
    return os.str();
  }
  static bool Contains(const std::string& ir, std::string_view needle);
  static size_t Count(const std::string& ir, std::string_view needle);
};
```

`tests/CMakeLists.txt` grows `bcc_opt_test` linking
`bcc_codegen bcc_opt GTest::gtest_main` (frontend fixture already rides
on `bcc_parse` via `tests/unittest/frontend`).

### 3.1 Step 2.1 — mem2reg (promote memory to SSA registers)

**Optimization objective.** Delete every *promotable* alloca (entry
block, scalar, used only by direct load/store) by replacing loads with
the stored SSA values and inserting phis at join points. This deletes
bcc's entire `%x.addr` codegen pattern — the single largest
quality delta available (clang's `-O0`-vs-`-O1` IR gap is dominated by
exactly this pass). LLVM:
`llvm/lib/Transforms/Utils/PromoteMemoryToRegister.cpp`
(`isAllocaPromotable`: only load/store users; `runPromoteMemoryToRegister`
promotes all promotable allocas in one pass; phi placement historically
by iterated dominance frontiers — Cytron et al., "Efficiently Computing
SSA..." — with the modern implementation using the incremental
`SSAUpdater`; bcc implements the classic DF algorithm) and
`llvm/lib/Transforms/Scalar/SROA.cpp` for the *splitting* half bcc
deliberately defers (bcc's aggregates stay memory-resident, like
clang's).

**Current BCC.** Codegen creates entry-block allocas for every local and
parameter (`codegen_function.cc` prologue: `alloca + store %arg`), a
`retval` alloca, and emits load/store pairs for every access
(`codegen_expr.cc`). All such allocas are promotable by construction
except aggregate-typed ones and the VLA path (already diagnosed, never
emitted). No pass exists.

**Files.**

| Path | Change |
|---|---|
| `include/bcc/opt/mem2reg.hh` + `src/opt/mem2reg.cc` | **new** — the pass |
| `src/opt/pipeline.cc` | (Phase 3) registration |
| `tests/unittest/opt/mem2reg_test.cc` | **new** |

**Data structures.**

```cpp
class PromoteMemoryToRegister : public FunctionPass {
 public:
  bool Run(Function&, FunctionAnalysisManager&) override; // uses DT
  bool PreservesDominators() const override { return true; } // CFG untouched
};

// internal:
struct AllocaInfo {
  AllocaInst* alloca;                       // instruction.hh:118
  std::vector<LoadInst*> loads; std::vector<StoreInst*> stores;
  std::vector<BasicBlock*> def_blocks;      // blocks containing stores
};
```

Promotability test per LLVM's `isAllocaPromotable`, specialized to bcc:
entry block; `GetAllocatedType()` non-aggregate (integer/FP/ptr);
every use is a `LoadInst`/`StoreInst` whose pointer operand *is* the
alloca (`use_list_` walk from Step 0.1).

**Step-by-step implementation** (promote all promotable allocas in one
walk, as LLVM does):
1. Collect promotable `AllocaInfo`s (walk entry block; skip
   non-promotable silently — that's legal, just unoptimized).
2. Phi placement per alloca: worklist = def blocks; place a phi at every
   block in `DF(B)` for each def block B (using `DominatorTree::Frontiers`),
   iterating until closure (a phi-def block adds itself to def blocks).
   Create phi via `BasicBlock::PrependPhi` (Step 0.2), typed
   `alloca->GetAllocatedType()`.
3. Renaming: DFS from entry over the dom tree carrying
   `std::unordered_map<AllocaInst*, std::stack<const Value*>>`;
   in each block (in order): replace each load of a tracked alloca with
   the current stack top (RAUW each load use — loads have exactly the
   value type; then erase the load); a store pushes its value operand
   and is erased; phis in successor-edge processing: for each successor
   `S` of the current block, for each phi of a tracked alloca in `S`,
   `AddIncoming(current-top-or-undef, current-block)`. Pop on DFS exit.
4. Loads with no reaching def use `ctx.GetUndef(type)` (C uninitialized
   read = undef — matches LLVM exactly).
5. After the walk: `UseEmpty()` allocas are erased; a `store`-only
   alloca (dead stores) has its stores erased too (no loads ever read
   them — sound because the alloca was non-escaped; LLVM does the same).
6. Update the `retval` pattern for free: it's an ordinary promotable
   alloca; the shared `ReturnBlock` collapses naturally once SimplifyCFG
   runs after.

Edge cases: alloca stored but never loaded (step 5); load before any
store on some path (undef phi entry); multiple stores in one block
(stack push order); a phi block that is also a def block; loop header
phis (back-edge value arrives during DFS of the back-edge source — the
classic algorithm handles it because phi *placements* were all created
before renaming); irreducible CFGs (DF algorithm is graph-shape
agnostic).

Correctness constraints: the pass never changes the CFG (declared via
`PreservesDominators`); erasing stores is sound only because the
promotability test proved no escaping use.

**Testing.** `mem2reg_test.cc`, each with the source input and the
asserted after-IR facts:

1. Parameter read-through (`int add(int a,int b){return a+b;}`) —
   before (codegen test's own expectation, `codegen_test.cc:43-51`) vs
   after: no `alloca`, no `store`, no `load`; `Contains(ir, "add i32 %a, %b")`.
2. Loop accumulation
   (`int sum(int n){int s=0,i=0;while(i<n){s+=i;i++;}return s;}`):
   header block contains `%s = phi i32 [ 0, %entry ], [ %add, %while.body ]`
   shaped phis; zero allocas.
3. Branch join (`int f(int c){int x=1;if(c)x=2;return x;}`): one phi in
   `if.end` with two incomings.
4. Uninitialized read
   (`int f(int c){int x;if(c)x=1;return x;}`): phi entry `undef`.
5. Address escapes block promotion
   (`void g(int*); int f(void){int x=1;g(&x);return x;}`): alloca
   remains (`Contains(ir, "alloca")`).
6. Aggregate stays (`struct S ...; S s; s.a=1;`): alloca remains.
7. Dead-store-only alloca (`int f(void){int x=1;return 0;}`): store
   gone.

Integration: the whole existing codegen test corpus run through
`[mem2reg]` must verify clean (fixture asserts this) and differential
execution must be green (fold + mem2reg are the first behavior-relevant
steps; the oracle of §0.1 applies in full).

**Performance evaluation.** Metrics via Step 3.2's `-stats`: `allocas`,
`loads`, `stores`, phis, total instructions, per function, before/after.
Targets: zero non-aggregate entry allocas remain on the corpus; load
count −60..80%; instruction count −35..50% on scalar-heavy functions
(these are the shapes clang exhibits for `-O0`→`-O1` IR). Runtime
performance is not yet measurable (code quality still delegated to
`llc` until the native backend); the proxy is the IR metrics plus
`llc -O0`-compiled binary instruction counts (via `objdump | wc`) on 10
kernels, expected −10..25%.

### 3.2 Step 2.2 — Dead-instruction elimination (DCE)

**Optimization objective.** Remove instructions with no side effects
that feed nothing. Cleans up after mem2reg (orphaned casts), SCCP, and
InstCombine. LLVM: `llvm/lib/Transforms/Scalar/DCE.cpp` — mark-and-sweep
backwards from side-effecting roots; the control-dependence-aware
variant is ADCE (`Scalar/ADCE.cpp`, deferred by bcc).

**Current BCC.** Nothing removes instructions. mem2reg already erases
what it makes dead; DCE generalizes.

**Files.** `include/bcc/opt/dce.hh`, `src/opt/dce.cc`,
`tests/unittest/opt/dce_test.cc` (new); no changes elsewhere.

**Algorithm.** Roots: every `store`, `call`, terminator, plus any
instruction with `!UseEmpty()` that is itself live (standard worklist):
1. `live = {}`; enqueue all terminators, all stores/calls.
2. Pop I: for each operand that is an `Instruction`, mark live + enqueue.
3. Sweep: erase every instruction not marked (skip terminators —
   unreachable-code removal is SimplifyCFG's job; skip allocas with uses
   by construction — they'd have been enqueued).
4. Iterate to fixpoint inside the pass (erasing a dead store can... no:
   stores are roots; single sweep is fixpoint for non-memory DCE — run
   once; `Run` returns changed).

`PreservesDominators() = true` (no CFG change).

**Testing.** Fixture cases: dead arithmetic chain
(`int f(int a){int t=a*3; return a;}` → `t` chain gone);
dead-after-mem2reg cast; live-through case (nothing removed);
call kept even if unused (`int f(void){g();return 0;}` keeps `call`).
**Performance evaluation.** Instruction-count delta composed into the
pipeline metric; standalone expect −5..15% after mem2reg on real code.

### 3.3 Step 2.3 — SimplifyCFG (lite)

**Optimization objective.** CFG canonicalization: fold constant
conditions, merge trivially-mergeable blocks, remove unreachable blocks,
retarget redundant edges. Directly collapses the codegen shapes
`if(0)`, `if(1)`, the post-mem2reg single-block `ReturnBlock`, and most
macro-expanded dead branches. LLVM:
`llvm/lib/Transforms/Scalar/SimplifyCFGPass.cpp` driving utilities in
`llvm/lib/Transforms/Utils/Local.cpp` — `ConstantFoldTerminator`
(br on constant → br; switch on constant → br),
`MergeBlockIntoPredecessor` (single-pred/single-succ fusion with phi
splicing), `EliminateDuplicatePHINodes`, unreachable-block deletion.

**Files.** `include/bcc/opt/simplify_cfg.hh`, `src/opt/simplify_cfg.cc`,
`tests/unittest/opt/simplify_cfg_test.cc` (new).

**Transformations (each its own commit + tests).**

1. **Fold constant terminator**: `condbr` whose condition is
   `ConstantInt` (after Step 0.4/3.4 it *is* a `ConstantInt`, never an
   instruction) → replace with unconditional `br` to the taken target;
   `switch` whose condition is constant or with only a default → `br`.
   Prune dead successor's phi incomings via `PhiNode::RemoveIncoming`
   (Step 0.2).
2. **Remove unreachable blocks**: `IsReachable` from entry (cfg.hh);
   erase block; for every phi in every remaining block, drop incomings
   whose block was erased (phi/pred agreement is a verifier check —
   this is where it pays).
3. **Merge single-pred into single-succ** (`A --br--> B`, B's only pred
   is A): append B's instructions to A (erase B's terminator first if A
   already... A's terminator is the br — replace: move B's non-terminator
   insts into A, then replace A's terminator with B's terminator); if B
   began with phis (single pred ⇒ each phi has one incoming), RAUW each
   phi with its incoming value first; B's branch-target uses: RAUW
   `B → A` (BasicBlock is a Value — Step 0.1), so successors' phis
   re-point automatically; erase B.
4. **Eliminate trivial phis**: one incoming → RAUW with that value; two
   incomings where one is the phi itself (self-reference through an
   unreachable... only after (2) — the standard `X = phi [X, a], [Y, b]`
   → `phi [Y, b]`) → RAUW.
5. **ret-unify**: multiple `ret` blocks are legal; do *not* merge (bcc
   codegen's ReturnBlock already unifies; post-mem2reg the funnel block
   merges away via (3) naturally when trivial).

Invalidation: CFG changes ⇒ `PreservesDominators() = false` (the
default).

**Testing.** Cases: `if(0){...}` body removed; `if(1)` straightened;
`while(1)` kept (infinite loops preserved — terminator not constant-foldable
away! `br i1 true` in a *loop header* still folds to `br` — correct, the
loop stays via the back edge); unreachable after `return`
(codegen emits dead code into unreachable blocks — `ir.md` §3.4: they
must vanish); diamond with same-value arms merges phi (via trivial-phi
rule); `goto`-created dead edge prunes. Each case: source in, asserted
IR facts out (e.g. `Count(ir, "br label") == expected`). Full corpus +
verifier + differential gates.

**Performance evaluation.** Block/instruction counts before/after;
expect the codegen corpus to shed all `if.else`-of-constant and dead-code
blocks (macro-heavy code sheds a lot).

### 3.4 Step 2.4 — InstCombine (lite)

**Optimization objective.** Canonicalizing algebraic rewrites applied
destructively (fold-then-RAUW-then-erase), sharing the Step 0.4 folder.
LLVM: `llvm/lib/Transforms/InstCombine/` (`InstCombine.cpp` driver,
per-opcode `InstCombineAddSub.cpp` etc.), pattern DSL
`llvm/include/llvm/IR/PatternMatch.h`, one-use guards
(`m_OneUse`) to avoid duplicating shared subexpressions, shared
simplification core `llvm/lib/Analysis/InstructionSimplify.cpp`. bcc
implements the identity subset that never duplicates operands (so
one-use tracking is not yet needed — called out as the upgrade hook).

**Files.** `include/bcc/opt/inst_combine.hh`, `src/opt/inst_combine.cc`,
`tests/unittest/opt/inst_combine_test.cc` (new).

**Rewrite table** (integer only unless noted; all verified sound for
wrap-around semantics since bcc IR has no `nsw/nuw` — `ir.md` §2.4):

| Pattern | Rewrite | Note |
|---|---|---|
| both operands constant | folder value | Step 0.4 |
| `x + 0`, `0 + x`, `x - 0`, `x * 1`, `x / 1` | `x` | |
| `x * 0` | `0` | ints only (FP sign/NaN) |
| `x & 0` → 0; `x \| -1` → -1 (masked); `x & -1`, `x \| 0`, `x ^ 0` → `x` | | |
| `x ^ x` → 0; `x & x`, `x \| x` → `x` | | |
| `x << 0`, `x >> 0` → `x` | | |
| `zext(trunc(x))` where widths match → `x` | | same-type idempotence |
| `sext(trunc(x))` width-matched → `x` | | |
| `trunc(zext(x))`/`trunc(sext(x))` to x's width → `x` | | |
| `icmp` with constant on LHS | swap operands + mirror predicate (`slt`↔`sgt` …) | canonicalizes for CSE |
| `icmp X, X` | constant (eq/ne true/false; signed incomparable-with-NaN N/A for ints) | |
| `fcmp X, X` | `oeq`→`false` if NaN-possible? **no — skip**: only `une`-family rewrites that are always sound (`ueq` is not in bcc's set; implement `une x,x → true`… careful: NaN ⇒ `une` true, so `une(x,x)` is *always* true; `oeq(x,x)` is *not* always false) | soundness table in tests |
| `to-bool` chain: `icmp ne (and x, y), 0` fed to `condbr` | keep (branch specialization is SimplifyCFG/`EmitBranchOnBool`'s domain) | scope note |

Application discipline (LLVM's driver shape): walk each block
top-to-bottom; for each instruction compute a replacement via folder +
table; on hit: RAUW + erase + mark changed; one full pass per `Run` (the
manager's rounds give the fixpoint); `PreservesDominators() = true`.

**Testing.** Per-table-row fixture (`x+0`, etc., via sources like
`int f(int x){return x+0;}` asserting `add` absent); the fcmp
soundness row gets explicit NaN-commented tests; regression: the codegen
corpus must not change behavior (differential). **Performance
evaluation.** Count of folded rewrites per corpus file (observer hook);
instruction-count contribution to the −60% Phase 3 target.

### 3.5 Step 2.5 — EarlyCSE (+ alloca store→load forwarding)

**Optimization objective.** Dominator-walk common-subexpression
elimination over simple values, plus forwarding of stores into
subsequent loads of the *same alloca*. This catches the repeated
subexpressions macro-expanded C produces and the reload-after-store
shapes mem2reg leaves behind (non-promotable, e.g. escaped allocas).
LLVM: `llvm/lib/Transforms/Scalar/EarlyCSE.cpp` — scoped hash table
keyed on `(opcode, operands...)`, pushed/popped per dom-tree level;
memory handling via MemorySSA in the default configuration, with
`EarlyCSEMemSSA=false` (bcc's model) falling back to conservative
last-store tracking.

**Files.** `include/bcc/opt/early_cse.hh`, `src/opt/early_cse.cc`,
`tests/unittest/opt/early_cse_test.cc` (new).

**Algorithm.**
1. Pre-order DFS over the dom tree (DT from the analysis manager).
2. Per dom-tree level: a scoped hash map
   `key(Opcode, operands, type[, pred]) → Value*`, and a memory table
   `AllocaInst* → {last stored Value*, generation}` plus a global
   `memory_generation` counter.
3. At each instruction:
   - pure (binop/cmp/cast/gep with constant-fold-stable operands): if
     key present and *dominating* (guaranteed by the scoped table),
     RAUW with the table value + erase; else insert.
   - `load` of an alloca whose table entry generation is current:
     RAUW with the stored value + erase; else record the load as the
     alloca's known value (allows `x=y; z=x;` chains).
   - `store` to an alloca: update the entry (value, generation).
   - Any `store` to a *non-alloca* pointer, or any `call`: bump the
     global generation (kills *all* forwarding — deliberately more
     conservative than LLVM's alias analysis; sound and simple; the
     upgrade hook is a proper AA when MemorySSA-grade precision is
     ever wanted).
   - `gep`: CSE by full key (same base + indices).
4. Pop scope on DFS exit; `PreservesDominators() = true`.

**Testing.** Duplicated subexpression
(`int f(int a){return (a*3)+(a*3);}` → one `mul`); store-forward through
branch where store dominates load; invalidation by intervening store to
the same alloca; invalidation by any call; escaped-alloca case that
mem2reg left (`g(&x); x=1; return x;` still forwards after the store);
pointer CSE (`&a[i]` twice → one gep). **Performance evaluation.**
Rewrite counts; instruction-count delta composed into pipeline target.

### 3.6 Step 2.6 — SCCP (lite)

**Optimization objective.** Sparse conditional constant propagation:
interprocedural in LLVM (`IPSCCP`) but function-local here — compute
which blocks are executable and which values are constants under that
control flow, then replace and cut dead branches. Kills the
macro-expanded `if(__STDC_VERSION__ >= 199901L)` and `#ifdef`-driven
constant conditionals *after* preprocessing (preprocessor can't always
remove them: computed constants). LLVM:
`llvm/lib/Transforms/Scalar/SCCP.cpp` + `SCCPSolver`
(`SCCPHelper.h`): value lattice (unknown/constant/overdefined), block
executability lattice, worklist propagation, and the crucial
single-value phi rule (a phi whose *executable* incomings are all the
same constant is that constant) followed by dead-block rewriting to
`unreachable`.

**Files.** `include/bcc/opt/sccp.hh`, `src/opt/sccp.cc`,
`tests/unittest/opt/sccp_test.cc` (new).

**Algorithm.**
1. Lattice per SSA value: `{Unknown, Constant(v), Overdefined}`;
   constants start in their own state; non-foldable ops propagate
   overdefined when any input is overdefined; folder resolves
   all-constant inputs (Step 0.4).
2. Blocks: `{Unexecutable, Executable}`; entry executable; on a
   `condbr`/`switch` with constant condition, only the taken successor
   becomes executable; unknown condition ⇒ all successors.
3. Worklist: SSA edges (def→uses) + CFG edges; phis merge over
   *executable* incomings only (all-same-constant rule above; differing
   constants ⇒ overdefined).
4. Finalize: RAUW constant values (only where safe: any value whose
   lattice is Constant and which is not itself already a Constant);
   terminators of unexecutable blocks → `unreachable`;
   conditional branches with constant condition → unconditional
   `br` (phi pruning as in SimplifyCFG — run SimplifyCFG immediately
   after; declare `PreservesDominators() = false`).
5. Iterate the pass once; rounds via manager.

Edge cases: self-referential phis (`x = phi [0, entry], [x+1, loop]` →
overdefined unless loop unexecutable); `undef` operands treat as
overdefined (conservative); recursion not applicable (function-local).

**Testing.** `if` on a computed constant
(`int f(void){int x=1; if(x<2) return 3; return 4;}` → single `ret 3`);
constant propagation through phi
(`int f(int c){int x=c?5:5; return x+1;}` → `ret 6`); dead branch
removal leaves verifier-clean phis (with SimplifyCFG in the pipeline);
loop phi stays variable. **Performance evaluation.** Dead-branch count
removed on macro-heavy corpus files; composed instruction delta.

### 3.7 Phase 2 exit gate

- [ ] All six passes landed with per-pass fixtures; `bcc_opt_test` green.
- [ ] Pipeline `[mem2reg, instcombine, simplifycfg, earlycse] ×2 → sccp →
      [instcombine, simplifycfg, earlycse] → dce` (assembled in Phase 3)
      achieves on the corpus: zero non-aggregate entry allocas; ≥60%
      instruction-count reduction vs `-O0` on the scalar kernel set;
      ≥30% on the regression corpus median.
- [ ] Differential execution (§0.1) 100% green across the corpus at
      `-O1`; verifier clean after every pass (`-verify-each` run in CI
      mode).

---

## 4. Phase 3 — Pipeline, driver, and measurement

### 4.1 Step 3.1 — Driver integration: `-O0/-O1`, `-verify-each`, `-print-after`

**Optimization objective.** Expose the passes: `cc_dump` flags parse
like the existing ones (`bin/cc_dump.cc:35-48` switch: extend with
`-O0` (default, current behavior), `-O1` (pipeline above), and debug
flags `-verify-each`, `-print-after-all` (via the manager's observer,
Step 1.1), `-stats`. LLVM analog: `opt`'s pipeline construction via
`PassBuilder` (`llvm/lib/Passes/PassBuilderPipelines.cpp`,
`buildO1DefaultPipeline`-shape: canonicalize → iterate → cleanup) and
`StandardInstrumentations.cpp` (`-print-after-all`, `-time-passes`).

**Files.**

| Path | Change |
|---|---|
| `bin/cc_dump.cc` | flag parsing (extend the existing string loop); after `parser.ParseTranslationUnit()` and before `-emit-ir` printing: if `-O1`, run `opt::RunDefaultPipeline(*module)` |
| `include/bcc/opt/pipeline.hh` + `src/opt/pipeline.cc` | **new** — `void RunDefaultPipeline(ir::Module&)`: loops non-declaration functions, builds the §3.7 sequence, honors a stats sink |
| `src/CMakeLists.txt` | `cc_dump` gains `bcc_opt` (root `CMakeLists.txt:27-29` target) |

**Testing.** `tests/unittest/opt/pipeline_test.cc`: end-to-end sources →
`Optimize(code, default pipeline)` assertions (subset of §3.7 gate);
smoke: `./build/cc_dump -emit-ir -O1` on 5 corpus files diffs `-O0` as
expected (manual inspection checklist committed as
`docs/llvm_comparison/` note or test expectations). **Performance
evaluation.** Enabled by Step 3.2.

### 4.2 Step 3.2 — `-stats` metrics

**Optimization objective.** The measurement harness: per-function
counters printed with `-stats` (LLVM's `-stats` via
`llvm/ADT/Statistic.h`, displayed by `opt`/`llc`).

**Files.** `include/bcc/opt/stats.hh` + `src/opt/stats.cc` (**new**):
`struct IRStats { unsigned functions, blocks, instructions, allocas,
loads, stores, phis, calls; }`, `IRStats Collect(const ir::Module&)`,
`EmitStatsTable(std::ostream&, IRStats before, IRStats after)`; wired in
`cc_dump -stats` (collect before + after pipeline) and asserted in
`pipeline_test.cc`. **Metrics defined** (per corpus file): total
instructions, load/store/alloca/phi counts, and derived
`instr_delta%`. These are the numbers §3.7's gate and the roadmap's
M1 criteria are measured with.

### 4.3 Step 3.3 — Differential-execution harness

**Optimization objective.** Make the §0.1 oracle a one-command,
corpus-scale regression tool. (LLVM's analog: `llvm-lit` +
`test/Transforms` execution tests + the whole
clang-tests-clang-output methodology; bcc keeps it as a shell script +
corpus because that matches the repo's existing regression-runner
style, e.g. `tests/unittest/as/diff_llvm_mc.sh`.)

**Files.**

| Path | Purpose |
|---|---|
| `tests/differential/opt_diff_test.sh` | **new** — for each `tests/differential/corpus/*.c`: emit `-O0`/`-O1` `.ll`, `clang`-build both, run both, compare stdout+exit; report summary; `--update-expected` mode writes `*.expected` files |
| `tests/differential/corpus/*.c` + `*.expected` | **new** — seed: 20 programs (scalar kernels: loops, switches, function-pointer calls, `goto`, bitfields, unions, floats; each printing deterministic output) |
| `docs/llvm_comparison/implementation_roadmap.md` | milestone M1 criteria reference this harness |

Acceptance for the step: harness green on all corpus files; wired as a
documented command (not auto-run by ctest — matches the repo's stance
that host-toolchain-dependent tests stay manual, `ir.md` §4).

### 4.4 Phase 3 exit gate (M1 complete)

- [ ] `-O1` end-to-end; `-stats`, `-verify-each`, `-print-after-all` work.
- [ ] §3.7 metric targets met and recorded (one run committed as a
      `docs/llvm_comparison/stats-baseline.md` table).
- [ ] `opt_diff_test.sh` 100% green; all existing suites green.

**Performance evaluation, program-level** (answering "did optimization
help" beyond IR counts): compile each corpus `.c` at `-O0`/`-O1` to
`.ll`, build with `clang -O0` (holding the backend constant), and
measure (a) generated `.text` bytes and dynamic instruction counts
(`objdump -d | wc -l`) and (b) wall-clock runtime of the binaries
(median of 5 runs; corpus programs are deterministic so this is
repeatable). Compile-time regression guard: median `cc_dump -O1` time
over the corpus ≤ 2× the `-O0` time (passes are linear-ish; rounds
bounded at 3). Memory: `/usr/bin/time -v` max RSS on the largest
corpus file, expect < 2× (use lists are the main new allocation).

---

## 5. Deliberate deferrals (with LLVM pointers for the future)

| Deferred | LLVM reference | Trigger to revisit |
|---|---|---|
| SROA (alloca splitting for struct-field access) | `Scalar/SROA.cpp` | aggregate-heavy corpus code |
| GVN / NewGVN (full value numbering + MemorySSA) | `Scalar/GVN.cpp`, `Scalar/NewGVN.cpp` | when EarlyCSE's hit rate plateaus |
| Reassociate, LICM, loop passes generally | `Scalar/Reassociate.cpp`, `Scalar/LICM.cpp` (needs `LoopInfo`) | loop-kernel benchmarks |
| ADCE (control-dependence DCE) | `Scalar/ADCE.cpp` | dead-branch patterns SCCP can't kill |
| Jump threading / block placement | `Scalar/JumpThreading.cpp`, `CodeGen/BlockPlacement.cpp` | backend block layout (roadmap M6) |
| One-use guards & reassociative InstCombine | `InstCombine/*`, `PatternMatch.h` | first InstCombine rewrite that would duplicate an operand |
| Interprocedural (IPSCCP, inlining, DAE) | ` IPO/*.cpp` | multi-TU or LTO-style goals |

## 6. Risk register (optimizer-specific)

| Risk | Guard |
|---|---|
| Use-list corruption (desync) | Single mutation path (`Use::Set`); debug asserts (`~Value` empty list); ASAN run of the whole suite in CI mode |
| mem2reg phi bugs | Verifier phi checks + dominance fixtures + differential on every corpus function |
| FP folding divergence from clang (rounding) | Folder round-trips float constants through exact-representability check; NaN/±0 cases unit-pinned; differential includes FP-printing programs |
| Fixpoint oscillation (InstCombine↔SimplifyCFG) | Round cap 3; passes individually monotone (each only deletes/canonicalizes) |
| `-O0` drift breaking codegen tests | Phase 0 gate: byte-diff review, expectations updated mechanically in the same commit |
| Test flakiness from name hints | Printer's slot tracker is deterministic (ir.md §2.3); fixtures assert structural facts (`Contains`/`Count`), never full-text equality except where stability was proven |

The end state of this plan is milestone **M1** of
`implementation_roadmap.md`: bcc ships `-O1` IR canonicalization with
LLVM-shaped infrastructure (use lists, verifier, pass manager, folding
core, six passes), measured and regression-gated — and every later
milestone (native backend M3+) consumes exactly these primitives.
