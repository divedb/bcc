# bcc IR & AST-to-IR Lowering — Design

Target: an SSA-form, LLVM-modeled intermediate representation for bcc plus
the lowering pass that turns the type-checked C11 AST (built by
`bcc_parse`/`bcc_sema`) into it, modeled on Clang's `lib/CodeGen`
(reviewed at `/Users/zlh/Documents/git/llvm-tutorial/third_party/llvm-project/clang`).

The textual form of the IR is deliberately **valid LLVM IR** for the subset
we emit, so `clang emitted.ll -o exe` works and the whole pipeline can be
differentially tested by executing the result (same methodology used for the
preprocessor and parser phases).

---

## 1. Review of Clang's IR generation

### 1.1 Architecture

Clang's `lib/CodeGen` is a single consumer pass over the Sema-checked AST.
It never re-derives semantic facts: every implicit conversion is already an
`ImplicitCastExpr` with a `CastKind`, every value carries its `QualType`,
record layout comes from `ASTContext`. CodeGen's job is purely *translation
plus placement* (which block, which alloca, which linkage).

| Component | Files | Responsibility |
|---|---|---|
| `CodeGenModule` | `CodeGenModule.cpp` (7.9k lines) | Per-TU state: walks top-level decls (`EmitTopLevelDecl`), creates/uniques `llvm::GlobalValue`s (`GetOrCreateLLVMFunction` / `GetOrCreateLLVMGlobal`), linkage/mangling, string-literal uniquing, deferred emission of `static` functions actually referenced. |
| `CodeGenTypes` + `CGRecordLayoutBuilder` | `CodeGenTypes.cpp`, `CGRecordLayoutBuilder.cpp` | Memoized `QualType → llvm::Type` conversion. Records get an `llvm::StructType` whose natural layout reproduces the AST `ASTRecordLayout` (explicit padding members); `CGRecordLayout` maps each `FieldDecl` to an IR field index and each bit-field to `(storage unit, offset, width)`. |
| `CodeGenFunction` | `CodeGenFunction.cpp`, `CGStmt.cpp`, `CGDecl.cpp` | Per-function state: current insertion block, `AllocaInsertPt` (all locals become entry-block `alloca`s; mem2reg promotes later), `ReturnBlock` + `ReturnValue` slot, `BreakContinueStack`, label→block map, switch stack. `EmitStmt` dispatches on `StmtClass` (`EmitIfStmt`, `EmitWhileStmt`, …, `CGStmt.cpp:157`). |
| Expression emitters | `CGExpr.cpp`, `CGExprScalar.cpp` (5.9k), `CGExprAgg.cpp`, `CGExprConstant.cpp` | Split by *value class*: `EmitLValue` produces an address, `ScalarExprEmitter` (a `StmtVisitor`, `CGExprScalar.cpp:254`) produces `llvm::Value*`s, `AggExprEmitter` evaluates struct/array expressions *into a destination slot*, `ConstantEmitter` folds initializers of static-storage objects to `llvm::Constant`s. |
| Call/ABI lowering | `CGCall.cpp` (6.1k), `ABIInfo*`, `Targets/` | Classifies arguments/returns per the platform psABI (direct/extend/indirect/sret/byval). |

### 1.2 Key data structures

- **`Address`** (`Address.h:128`) — a pointer `llvm::Value*` plus its known
  alignment (and element type). Everything that names memory flows through
  this; alignment comes from the AST side, not from IR types.
- **`RValue`** (`CGValue.h:42`) — the result of evaluating an expression as
  a value: a scalar `llvm::Value*`, a complex pair, or — for aggregates —
  an `Address` (aggregates are never scalar SSA values in Clang; they live
  in memory).
- **`LValue`** (`CGValue.h:182`) — the result of evaluating an expression
  as a location: an `Address` plus the AST type/qualifiers, or a *bit-field*
  reference (address of storage unit + `CGBitFieldInfo`), or vector-element
  / global-register variants.
- **`CGRecordLayout`** — `FieldDecl` → IR field index / bit-field info.

### 1.3 Lowering strategy (the parts that matter)

- **Locals are allocas, not SSA registers.** `EmitAutoVarAlloca`
  (`CGDecl.cpp:1461`) creates one entry-block `alloca` per local; every
  read is a `load`, every write a `store`. CodeGen never builds phi nodes
  for user variables — LLVM's mem2reg does SSA construction later. Phis
  appear only for expression-level control flow (`?:`, `&&`, `||`).
- **Casts are table-driven.** `ScalarExprEmitter::VisitCastExpr` is one big
  switch over `CastKind` (`CGExprScalar.cpp:2289`): `CK_LValueToRValue` →
  load, `CK_IntegralCast` → trunc/sext/zext, `CK_ArrayToPointerDecay` →
  address of element 0, etc. Sema already decided everything.
- **Booleans**: `_Bool` is stored as `i8` in memory and used as `i1` in
  control flow; `EmitBranchOnBoolExpr` (`CodeGenFunction.h:5165`)
  special-cases `!`, `&&`, `||` and comparisons to emit short-circuit CFG
  directly instead of materializing `0/1` values.
- **Statements → CFG**: each control construct creates labeled blocks
  (`if.then/if.else/if.end`, `while.cond/body/end`, `for.cond/inc/end`,
  …). After a terminator, Clang keeps emitting into an unreachable block
  (dead code is emitted and left for the optimizer). `return` stores into
  the `ReturnValue` slot and branches to the shared `ReturnBlock`, which
  does the single `ret`.
- **Aggregates are memory operations**: struct assignment is
  `EmitAggregateCopy` (memcpy), initializer lists are stored field-by-field
  into the destination slot, constant-foldable initializers become
  `llvm::Constant` stores / global initializers via `ConstantEmitter`
  (`tryEmitPrivateForMemory`).
- **Globals**: emitted on use or at end of TU; initializers must constant
  fold (C guarantees this for static storage); tentative definitions get
  zero initializers; `static` → `internal` linkage; string literals become
  uniqued `private unnamed_addr constant` arrays.

### 1.4 What we deliberately do NOT copy

- **SysV ABI classification** (`CGCall.cpp`, `ABIInfo`): we pass and return
  small aggregates *directly* as IR struct values and larger ones too —
  no INTEGER/SSE classification, no `byval`/`sret`. The IR stays valid
  LLVM; the produced code just doesn't interoperate with externally
  compiled functions that take/return structs by value. Scalars (the
  overwhelming case) match the C ABI exactly.
- C++/ObjC/OpenMP machinery, exceptions/cleanups (`CGCleanup`), atomics,
  complex numbers, debug info, TBAA/PGO/coverage metadata, builtins
  (`CGBuiltin.cpp`), inline `asm`, VLAs (Sema already rejects the uses that
  would need runtime sizes; a VLA local is diagnosed by CodeGen).
- LLVM's use-list infrastructure and constant folding in the builder: our
  IR has no optimizer yet, so values do not track their users, and the
  builder emits what it is told. (Adding use lists is the first step when
  an optimizer is added; noted as future work.)
- `long double` (x87 80-bit) is lowered as `double`. `_Atomic` and
  `volatile` are lowered as plain accesses (volatile: future work).

---

## 2. The bcc IR

New library `bcc_ir` (`include/bcc/ir/`, `src/ir/`). No dependency on the
AST — the IR is a self-contained target-level representation, mirroring the
`llvm/IR` ↔ `clang/AST` split. Style matches the rest of the tree
(snake_case files, PascalCase methods, `k` enumerators, no LLVM libraries).

### 2.1 Structure

```
Module
 ├── GlobalVariable*   (name, value type, initializer Constant*, linkage,
 │                      is_const, align)
 └── Function*         (name, FunctionType, linkage, Argument*s,
      └── BasicBlock*     list; empty list ⇒ declaration
           └── Instruction*  ordered list; last one is the terminator
```

An `IRContext` (owned by `Module`) uniques all types and all constants, so
type and constant equality are pointer equality — same design as
`ASTContext` for canonical AST types.

### 2.2 Type system (`ir/type.hh`)

Kinds: `void`, `iN` (N ∈ {1, 8, 16, 32, 64}), `float`, `double`,
`ptr` (**opaque**, like modern LLVM — pointee types live only in load /
store / GEP operands), `[N x T]` arrays, named `%struct.X` /
`%union.X` structs, and function types (return, params, variadic).

Struct types carry their field list *including explicit padding fields*
([§3.2](#32-type-lowering-codegen_typescc)) plus the total size and
alignment copied from the AST `RecordLayout`, so no layout logic exists in
the IR itself.

### 2.3 Values (`ir/value.hh`)

```
Value                (type, optional name hint)
 ├── Argument
 ├── BasicBlock      (as a branch target)
 ├── Constant
 │    ├── ConstantInt, ConstantFP, ConstantNullPtr, ConstantUndef
 │    ├── ConstantAggregateZero        (zeroinitializer)
 │    ├── ConstantString              (c"…" byte arrays)
 │    ├── ConstantAggregate           (array/struct initializers)
 │    └── GlobalValue: GlobalVariable, Function   (their *addresses*)
 └── Instruction
```

Values are anonymous; the printer runs a slot tracker that assigns `%0,
%1, …` per function and de-duplicates name hints (`%x.addr`,
`%x.addr1`), exactly like LLVM's asm writer. Name hints carry over from C
identifiers to keep the output readable.

### 2.4 Instruction set (`ir/instruction.hh`)

Deliberately the minimal LLVM subset C needs:

| Group | Instructions |
|---|---|
| Memory | `alloca` (entry-block locals), `load`, `store`, `getelementptr inbounds` |
| Integer arith | `add sub mul sdiv udiv srem urem shl lshr ashr and or xor` |
| FP arith | `fadd fsub fmul fdiv` |
| Compare | `icmp` (eq ne slt sle sgt sge ult ule ugt uge), `fcmp` (oeq une olt ole ogt oge) |
| Casts | `trunc zext sext fptrunc fpext fptosi fptoui sitofp uitofp ptrtoint inttoptr` |
| Control | `br` (cond/uncond), `switch`, `ret`, `unreachable`, `phi` |
| Other | `call` |

Notes:

- `getelementptr` carries a source element type (opaque-pointer style).
  Three shapes are emitted: array/pointer indexing (`gep T, ptr, i64 idx`),
  struct field access (`gep %struct.S, ptr, i32 0, i32 k`), and raw byte
  offsets (`gep i8, ptr, i64 off`) for union members and bit-field storage
  units.
- `phi` is used only for `?:`, `&&`, `||` values — never for variables
  (allocas, per Clang's model).
- Aggregate `load`/`store` (of struct/array type) are allowed, as in LLVM;
  they implement struct assignment and constant aggregate initialization
  without needing a memcpy intrinsic.
- No intrinsics, no `nsw`/`nuw` flags (signed overflow is already UB at the
  C level; the flags only matter to optimizers we don't have).

### 2.5 Construction & printing

- **`IRBuilder`** (`ir/ir_builder.hh`) — insertion point (block + end),
  `CreateAdd/CreateLoad/CreateCondBr/...` helpers; the only way codegen
  makes instructions.
- **`IRPrinter`** (`ir/ir_printer.cc`) — module → LLVM-syntax text:
  struct type definitions first, then globals, then function definitions,
  then declarations. Output is accepted by `llvm-as`/`clang` for the
  emitted subset (verified in tests).

---

## 3. AST → IR lowering (`bcc_codegen`)

New library `bcc_codegen` (`include/bcc/codegen/`, `src/codegen/`),
depending on `bcc_ast` + `bcc_ir` (+ diagnostics). Entry point:

```
CodeGenModule cgm(ast_ctx, diags, module);
cgm.EmitTranslationUnit();      // walks TranslationUnitDecl
```

File split mirrors Clang's:

| bcc file | Clang counterpart | Contents |
|---|---|---|
| `codegen_module.cc` | `CodeGenModule.cpp` | top-level decl walk, global variables, function decl/def creation, linkage, string-literal uniquing, static-local globals |
| `codegen_types.cc` | `CodeGenTypes.cpp` + `CGRecordLayoutBuilder.cpp` | `QualType → ir::Type` cache, record lowering + field maps |
| `codegen_function.cc` | `CodeGenFunction.cpp` + `CGDecl.cpp` | prologue (param allocas), `ReturnBlock`, local `VarDecl` emission incl. initializers |
| `codegen_stmt.cc` | `CGStmt.cpp` | statement dispatch, CFG construction |
| `codegen_expr.cc` | `CGExpr.cpp` + `CGExprScalar.cpp` + `CGExprAgg.cpp` | `EmitLValue`, `EmitScalarExpr`, `EmitAggExpr`, `EmitBranchOnBool` |
| `constant_emitter.cc` | `CGExprConstant.cpp` | initializer → `Constant` folding |

### 3.1 Core value classes (ported from `CGValue.h`/`Address.h`)

```cpp
struct Address   { ir::Value* ptr; uint64_t align; };            // §1.2
struct BitFieldInfo { uint64_t storage_offset;  // bytes, unit-aligned
                      unsigned storage_size;    // bits (8/16/32/64)
                      unsigned offset, width;   // within the unit
                      bool is_signed; };
class  LValue    { Address addr; QualType type; BitFieldInfo* bf; };
class  RValue    { ir::Value* scalar; Address aggregate; };
```

`Address.align` is always derived from the AST (`GetTypeAlign`, record
layout, declared alignment), never from IR types.

### 3.2 Type lowering (`codegen_types.cc`)

`CodeGenTypes::Convert(QualType)` — memoized:

| C type | IR type |
|---|---|
| `void` | `void` |
| `_Bool` | `i8` in memory (`i1` transiently in control flow) |
| `char`/integers/enums | `iN` by size (x86-64 SysV widths from `ASTContext`) |
| `float`/`double` | `float`/`double` |
| `long double` | `double` (documented deviation) |
| pointers | `ptr` |
| `T[N]` | `[N x T']` |
| functions | function type (params decayed per Sema; no-proto ⇒ `(...)` variadic with no fixed params) |
| `struct S` | `%struct.S` with explicit padding |
| `union U` | `%union.U = { [size x i8] }` |

Record lowering walks fields in layout order using the AST
`RecordLayout::field_offsets_bits`:

- a non-bit-field at byte offset *o*: emit `[pad x i8]` up to *o*, then the
  converted field type; record `FieldDecl → ir field index`.
- bit-fields do not get IR fields; their bits are covered by padding.
  Access uses `BitFieldInfo` computed from the AST bit offset: the storage
  unit is the `sizeof(declared type)`-sized, naturally-aligned unit
  containing the bits.
- trailing padding out to `RecordLayout::size`.

Because every field is placed at its AST offset by construction, LLVM's
natural struct layout of the emitted type reproduces the AST layout — this
is precisely Clang's `CGRecordLayoutBuilder` contract.

### 3.3 Expression lowering

Three emitters, selected by the *evaluation kind* of the expression type
(scalar vs aggregate), exactly like Clang:

**`EmitLValue(expr) → LValue`** for expressions that denote objects:

| AST node | lowering |
|---|---|
| `DeclRefExpr` (local) | the var's alloca `Address` |
| `DeclRefExpr` (global/function/static local) | the `GlobalValue` |
| `*p` | `EmitScalarExpr(p)` |
| `a[i]` | `gep T, base, idx` (base is the decayed pointer; Sema already inserted the decay cast) |
| `s.f` | struct: `gep %struct.S, base, 0, k`; union: base pointer; bit-field: `LValue` with `BitFieldInfo`; `s->f`: same on the loaded pointer |
| string literal | uniqued private global |
| compound literal | filled temporary alloca (file-scope: private global) |
| `(e)` / `_Generic` | recurse on the chosen sub-expression |

**`EmitScalarExpr(expr) → ir::Value*`** for scalar rvalues. The key cases:

- **Casts — a pure `CastKind` switch** (Sema materialized every
  conversion, §1.3):
  `kLValueToRValue` → `load` (bit-field: load unit + shift/mask;
  `_Bool`: load `i8`);
  `kIntegralCast` → `trunc`/`sext`/`zext` by source signedness;
  `kIntegralToFloating` → `sitofp`/`uitofp`; `kFloatingToIntegral` →
  `fptosi`/`fptoui`; `kFloatingCast` → `fptrunc`/`fpext`;
  `k*ToBoolean` → `icmp ne 0` / `fcmp une 0.0` then `zext i8`;
  `kArrayToPointerDecay`/`kFunctionToPointerDecay` → the `LValue` address;
  `kNullToPointer` → `null`; `kIntegralToPointer`/`kPointerToIntegral` →
  `inttoptr`/`ptrtoint`; `kBitCast`/`kNoOp` → passthrough;
  `kToVoid` → evaluate for side effects, no value.
- Arithmetic binops: signed/unsigned/FP-selected opcode from the (already
  converted, identical) operand types. `+`/`-` with a pointer operand →
  `gep`; pointer difference → `ptrtoint` − `ptrtoint` then `sdiv exact`
  by `sizeof(*p)`.
- Comparisons: `icmp`/`fcmp` (+ `zext` to `i32`, the C result type).
  Pointer compares use `icmp` on `ptr` directly.
- `&&`/`||`/`?:` as values: short-circuit CFG + `phi`.
- Assignment: `EmitScalarExpr(RHS)`, `EmitLValue(LHS)`, `store`, yield the
  RHS value. Compound assignment: load LHS, convert to the recorded
  computation type, op, convert back, store (conversions re-derived from
  `CompoundAssignOperator::GetComputationType`, which Sema computed).
  `++`/`--`: load, `add 1`/`gep ±1`, store; pre/post picks which value.
- Calls: emit callee (function or function pointer), args left-to-right as
  scalars/aggregate values; declaration created on demand
  (`GetOrCreateFunction`, §1.1).

**`EmitAggExpr(expr, dest: Address)`** evaluates struct/array expressions
into a destination slot (never as free-standing values, except when loaded
as call arguments / return values):

- lvalue sources (`DeclRef`, `s.f`, `*p`, …) → aggregate `load` + `store`
  (Clang: `EmitAggregateCopy`);
- semantic-form `InitListExpr` → if fully constant, one `store` of the
  folded `ConstantAggregate`; otherwise store the constant skeleton
  (zeros included — Sema's semantic form already made zero-fill explicit)
  then `store` each non-constant element through GEPs;
- string-literal initializer of a `char[N]` → `store` of the
  `ConstantString`;
- calls returning aggregates → `store` of the returned struct value;
  `?:` on aggregates → both arms emit into the same slot.

**`EmitBranchOnBool(cond, true_bb, false_bb)`** — ported from
`EmitBranchOnBoolExpr`: special-cases `!` (swap targets), `&&`/`||`
(intermediate block; no materialized `0/1`), comparisons (branch on the
`i1` directly), constants (unconditional `br`); otherwise compares the
scalar value against zero.

### 3.4 Statement lowering (`codegen_stmt.cc`)

One `EmitStmt` dispatch; each construct builds the same CFG shapes Clang
does (block name hints in parentheses):

- `if` → `EmitBranchOnBool` into (`if.then`, `if.else`), join at `if.end`.
- `while` → `br while.cond`; cond branches (`while.body`, `while.end`).
- `do` → `do.body` … `do.cond` branches (`do.body`, `do.end`).
- `for` → init; `for.cond`; body; `for.inc`; back edge. `continue` targets
  `for.inc` (or `cond` when no increment), `break` targets `for.end` —
  a `BreakContinue` stack entry per loop, pushed/popped around the body.
- `switch` → evaluate promoted condition once, emit `switch` instruction;
  walking the body registers each `case`/`default` block into the pending
  `switch` (a switch-state stack, like `CGF::SwitchInsn`); `break` targets
  `sw.epilog`. Case fall-through works naturally since consecutive case
  blocks are emitted adjacently with fallthrough branches.
- `goto`/labels: lazily created block per `LabelDecl` (`GetBlockForLabel`),
  since `goto` may precede the label.
- `return` → store into the `ReturnValue` alloca (if non-void), `br` to
  the shared `ReturnBlock` (§1.3). The epilogue loads and `ret`s. `main`
  gets its `ReturnValue` pre-initialized to `0` (C11 5.1.2.2.3); other
  non-void functions falling off the end return an undefined value —
  matching Clang's observable behavior for defined programs.
- After any jump, codegen keeps emitting into a fresh unreachable block
  (dead code is emitted, not skipped); unterminated blocks are closed with
  `br ReturnBlock` (fall-through) at function end.
- `DeclStmt` → `EmitLocalVarDecl` per var ([§3.5](#35-declarations)).

### 3.5 Declarations

**Locals** (`EmitLocalVarDecl`, cf. `EmitAutoVarAlloca/Init`): one
entry-block `alloca` (inserted at a dedicated *alloca insertion point* so
allocas group at the top regardless of where the decl appears), then the
initializer: scalar → `EmitScalarExpr` + `store`; aggregate →
`EmitAggExpr` into the alloca; none → nothing (garbage, as in C).

**Static locals**: an internal-linkage global named `f.x` (Clang's
scheme), initializer via `ConstantEmitter` (Sema already enforced
constant initializers for static storage).

**Parameters**: `alloca` + `store` of the incoming `Argument` in the
prologue (`%x.addr` pattern); the body only ever sees the alloca.

**Globals** (`EmitGlobalVarDecl`): linkage from storage class
(`static` → `internal`, otherwise external), `extern` without initializer →
declaration only; tentative definitions → `zeroinitializer`; initializer
folded by `ConstantEmitter`. Emission runs in two passes over the TU decls
(create-then-initialize) so forward references (`int *p = &x;` before `x`'s
definition line) resolve.

**Functions**: `GetOrCreateFunction(name, type)` uniques the IR function
across declarations and calls; the definition pass fills in body blocks.
Only *used* declarations are printed (`declare` lines), like Clang's
deferred-decl machinery reduced to its C essence.

**`ConstantEmitter`** folds static initializers structurally: literals,
enum constants, arithmetic/casts over constants, `sizeof`, address
constants (`&global`, functions, string literals, array decay,
`&arr[k]`/`&s.f` constant GEPs folded to byte offsets), `InitListExpr` →
`ConstantAggregate`, strings → `ConstantString`, missing fields →
zero-fill. This intentionally re-implements the (small) constant-address
subset of Clang's `ConstantEmitter` rather than reusing Sema's ICE
evaluator, which only handles integers and is Sema-internal.

### 3.6 Booleans, exactly

`_Bool` is `i8` in memory. Values become `i1` only (a) as `icmp`/`fcmp`
results and (b) as branch conditions; every `*ToBoolean` cast is
`cmp-ne-zero → zext i8`. This dodges the classic i1/i8 mismatch class of
bugs while staying LLVM-legal (Clang does the same, with `i1` kept live a
little longer between its `EmitToMemory`/`EmitFromMemory` hooks).

### 3.7 Unsupported constructs

Anything Sema lets through but codegen cannot lower (currently: VLA
locals) is reported through a new diagnostic
`err_codegen_cannot_compile` ("cannot compile this %0 yet"), Clang's
wording for the same situation, and the function is abandoned — never
silently mis-lowered.

---

## 4. Driver & testing

- `cc_dump` gains `-emit-ir`: after a clean parse, runs
  `CodeGenModule::EmitTranslationUnit()` and prints the module to stdout.
- Unit tests `tests/unittest/frontend/codegen_test.cc`: a `CodeGenTest`
  fixture (extends the existing `FrontendTest` pipeline) that lowers a
  source string and returns the printed IR; assertions are
  substring/pattern based per construct (allocas, cast selection, CFG
  shape, switch tables, aggregate init, globals, linkage).
- Differential/execution testing (matching the PP/parser methodology):
  because the printed IR is valid LLVM IR, `clang emitted.ll` produces a
  runnable binary; test programs' exit codes / output are compared against
  the same source compiled natively with clang. Used as a development
  check; not wired into the default ctest run (host-toolchain dependent).

## 5. Implementation order

1. `bcc_ir`: types/values/instructions/builder/printer (unit-testable
   standalone).
2. `codegen_types` + `CodeGenModule` skeleton: globals, string literals,
   function shells, constant emitter.
3. `CodeGenFunction`: prologue/epilogue, statements, scalar expressions.
4. Aggregates, bit-fields, static locals.
5. Driver flag, tests, LLVM cross-validation.
