# bcc Parser & Semantic Analysis — Implementation Plan

Target: a C11 parser and semantic analyzer for bcc, modeled on Clang's
`lib/Parse` + `lib/Sema` + `lib/AST` (reviewed at
`/Users/zlh/Documents/git/llvm-tutorial/third_party/llvm-project/clang`),
consuming the existing bcc Preprocessor and producing a type-checked AST.

---

## 1. Review of Clang's architecture

### 1.1 The three-layer split

Clang separates syntax from semantics with a strict layering:

| Layer | Clang files | Responsibility |
|---|---|---|
| **Parse** | `lib/Parse/Parser.cpp`, `ParseDecl.cpp` (8.7k lines), `ParseExpr.cpp` (4k), `ParseStmt.cpp` (2.9k), `ParseInit.cpp` | Recursive-descent over the token stream. Knows grammar only; builds **no AST nodes** itself. |
| **Sema** | `lib/Sema/Sema.cpp`, `SemaDecl.cpp` (20k), `SemaExpr.cpp` (21k), `SemaStmt.cpp`, `SemaType.cpp` (10k), `SemaInit.cpp`, `SemaLookup.cpp` | All semantic checking and **all AST construction**, exposed to the parser as `ActOn*` callbacks (`ActOnBinOp`, `ActOnDeclarator`, `ActOnStartOfFunctionDef`, …). |
| **AST** | `include/clang/AST/{Type,Decl,Expr,Stmt}.h`, `ASTContext` | Immutable-ish result tree. `ASTContext` bump-allocates every node and **uniques types** so type equality is pointer equality on canonical types. |

Key control flow (per translation unit):

```
ParseAST() → Parser::ParseTopLevelDecl() loop
  → Parser::ParseExternalDeclaration()
    → ParseDeclOrFunctionDefinition()
      → ParseDeclarationSpecifiers()   (fills DeclSpec)
      → ParseDeclarator()              (fills Declarator)
      → Sema::ActOnDeclarator() / ActOnStartOfFunctionDef()
        → GetTypeForDeclarator() (SemaType.cpp), lookup, redecl merge
      → [body] ParseCompoundStatement() → ActOnCompoundStmt() …
      → ActOnFinishFunctionBody()
```

### 1.2 The parser⇄sema handshake: DeclSpec / Declarator

`include/clang/Sema/DeclSpec.h`: the parser accumulates declaration
specifiers into a `DeclSpec` — storage class (TSCS/SCS), type-specifier
width/sign/type (TSW/TSS/TST), qualifiers (TQ), function specifiers —
each recorded with its source location; `DeclSpec::Finish()` diagnoses bad
combinations (`long float`, duplicate specifiers…). A `Declarator` wraps a
`DeclSpec` plus a stack of `DeclaratorChunk`s (pointer/array/function/paren),
built inside-out by `ParseDeclarator`. Sema's `GetTypeForDeclarator` walks the
chunks to build the final `QualType`.

### 1.3 Scope and name lookup

`include/clang/Sema/Scope.h`: parser-owned lexical scopes, a linked stack
with flags (`FnScope`, `BreakScope`, `ContinueScope`, `DeclScope`,
`ControlScope`, `SwitchScope`, …). The parser pushes/pops via
`ParseScope` RAII; Sema hangs declarations off each scope and the
`IdentifierResolver` maps `IdentifierInfo* → Decl` chains per C namespace
(ordinary, tag, label, member). C lookup = walk scopes innermost-out.
Crucially the parser asks Sema `isTypeName(II)` mid-parse to disambiguate
typedef-names — this is what makes C context-sensitive (`T * x;`).

### 1.4 Expressions

`ParseExpr.cpp` uses **precedence climbing**: `ParseCastExpression` handles
unary/primary/postfix, `ParseRHSOfBinaryExpression(LHS, MinPrec)` handles all
binary/ternary/assignment operators from a `prec::Level` table keyed by token
kind. Results flow as `ExprResult` (pointer + error bit); on error the parser
recovers via `SkipUntil` with stop-token sets. Sema checks each operation in
`ActOnBinOp`/`ActOnUnaryOp`/`ActOnCallExpr`… applying:

- lvalue-to-rvalue, array-to-pointer, function-to-pointer decay
  (`DefaultFunctionArrayLvalueConversion`)
- integer promotions and **usual arithmetic conversions**
  (`UsualArithmeticConversions`)
- assignment compatibility (`CheckAssignmentConstraints`) with the familiar
  int↔pointer / incompatible-pointer warnings
- every conversion is **materialized as an `ImplicitCastExpr`** with a
  `CastKind` (`LValueToRValue`, `IntegralCast`, `PointerToIntegral`, …), so
  later phases (codegen) never re-derive conversions.

### 1.5 Statements, functions, constants

`ParseStmt.cpp` + `SemaStmt.cpp`: statement parsing is trivial recursive
descent; Sema checks context legality (`break`/`continue` scope flags, case
labels inside a switch with ICE values, return-type conversion), and
`goto`/label resolution is deferred to end-of-function
(`ActOnFinishFunctionBody` diagnoses undefined labels).
Integer constant expressions (array bounds, bitfield widths, enum values,
case labels, `_Static_assert`) run through the AST constant evaluator
(`Expr::EvaluateAsInt`; full engine in `ExprConstant.cpp`).

### 1.6 What we deliberately do NOT copy

C++/ObjC/OpenMP/OpenACC machinery (`ParseTentative.cpp`, templates,
overloading, `SemaLookup`'s argument-dependent complexity), the
table-generated `ParsedAttr` system, `TreeTransform`, the full
`APValue`/`ExprConstant` evaluator, delayed diagnostics, module machinery,
and Clang's `TypeLoc` source-location-in-type system (we store decl-level
locations only).

---

## 2. Mapping to bcc

Style follows the existing tree: snake_case files, `.hh/.cc`, `namespace
bcc`, PascalCase methods, `k`-prefixed enumerators, trailing-underscore
members, `///` doc comments, C++20, no LLVM dependencies (std containers
instead of FoldingSet/BumpPtrAllocator).

New libraries (each mirrors an existing `src/<dir>` + `include/bcc/<dir>` pair):

```
bcc_ast    ← bcc_basic          AST nodes, ASTContext, layout, dumper
bcc_parse  ← bcc_pp, bcc_sema   Parser
bcc_sema   ← bcc_ast, bcc_pp    Sema, Scope, DeclSpec/Declarator
```

(Declarator structures live in `bcc_sema` like Clang's `Sema/DeclSpec.h`;
`bcc_lex` gains `literal_support` for numeric/char/string decoding.)

Target model for layout/`sizeof`: x86-64 SysV (matches `bcc_as`):
`char`=1, `short`=2, `int`=4, `long`/`long long`/pointers=8, `float`=4,
`double`=8, `long double`=16 (align 16), `_Bool`=1; struct layout with
natural alignment + bitfield packing.

### 2.1 AST — `include/bcc/ast/`, `src/ast/`

**`type.hh`** — the type system, uniqued by `ASTContext`:

- `Qualifiers` (const/volatile/restrict/_Atomic bits) + `QualType`
  = `{const Type*, Qualifiers}` value type (we don't pointer-pack).
- `Type` hierarchy (`TypeClass` enum + `Classof`-style casts via
  `As<T>()`/`Is<T>()`):
  - `BuiltinType` (Void, Bool, Char/SChar/UChar, Short…ULongLong, Float,
    Double, LongDouble) — singletons on ASTContext.
  - `PointerType`, `ArrayType` (Constant/Incomplete/Variable),
    `FunctionType` (proto: return, params, variadic, no-proto flag),
    `RecordType`/`EnumType` (point at their `TagDecl`), `TypedefType`
    (sugar; `GetCanonicalType()` desugars).
- Predicates mirrored from Clang's `Type`: `IsIntegerType`,
  `IsArithmeticType`, `IsScalarType`, `IsCompleteType(ctx)`,
  `IsCompatibleWith` (C6.2.7 composite/compatible types on canonical types).

**`decl.hh`** — `Decl` base (kind, loc, `DeclContext*` owner) and:
`TranslationUnitDecl`, `VarDecl` (storage class, init, `IsFileScope`…),
`ParmVarDecl`, `FunctionDecl` (params, body, storage, inline, defined-bit),
`TypedefDecl`, `FieldDecl` (bitfield width), `RecordDecl` (struct/union,
completeness, fields), `EnumDecl` + `EnumConstantDecl`, `LabelDecl`.
`DeclContext` (TU, function, record) holds its decls in order.

**`expr.hh`** — `Expr : Stmt` with `QualType type_` and `ValueKind`
(lvalue/rvalue): `IntegerLiteral` (uint64 + type), `FloatingLiteral`,
`CharacterLiteral`, `StringLiteral`, `DeclRefExpr`, `ParenExpr`,
`UnaryOperator`, `BinaryOperator` (+`CompoundAssignOperator`),
`ConditionalOperator`, `ArraySubscriptExpr`, `CallExpr`, `MemberExpr`
(dot/arrow), `CastExpr` (`ImplicitCastExpr` with `CastKind`,
`CStyleCastExpr`), `SizeOfAlignOfExpr` (type or expr operand),
`CompoundLiteralExpr`, `InitListExpr` (+ designator support),
`GenericSelectionExpr`, `CommaOperator` folded into BinaryOperator.

**`stmt.hh`** — `CompoundStmt`, `DeclStmt`, `ExprStmt` wrapper is not needed
(Clang: exprs are stmts), `IfStmt`, `WhileStmt`, `DoStmt`, `ForStmt`,
`SwitchStmt` (+`CaseStmt`/`DefaultStmt` chain), `BreakStmt`, `ContinueStmt`,
`ReturnStmt`, `GotoStmt`/`LabelStmt`, `NullStmt`.

**`ast_context.hh`** — arena ownership of all nodes (monotonic allocation;
nodes are never individually freed), type uniquing maps
(`GetPointerType`, `GetConstantArrayType`, `GetFunctionType`, …), builtin
singletons, integer-type ranking helpers for conversions, and
**`type_layout.cc`**: `GetTypeSize/Align`, `RecordLayout` (field offsets,
bitfield allocation, union/struct size, padding).

**`ast_dumper.cc`** — Clang-style indented `-ast-dump` textual dump for tests.

### 2.2 Sema — `include/bcc/sema/`, `src/sema/`

**`decl_spec.hh`** — `DeclSpec` (SCS, TSW, TSS, TST, TQ, function specs,
each with loc; `Finish(diags)` validates combinations exactly like Clang's
table in `DeclSpec.cpp`), `Declarator` + `DeclaratorChunk`
(Pointer{quals} / Array{size-expr, static, star, quals} /
Function{params, variadic, proto} / Paren), `DeclaratorContext`
(file, prototype, struct-field, typename, for-init, block).

**`scope.hh`** — `Scope` with parent link + flags
(`kFn|kBreak|kContinue|kDecl|kControl|kSwitch|kBlock|kFnProto`), set of
decls; `Sema` keeps `cur_scope_`.

**`sema.hh`** + per-area `.cc` files mirroring Clang's split:

- `sema.cc` — construction, scope push/pop, common helpers
  (`Diag(loc,kind)`, `RequireCompleteType`).
- `sema_lookup.cc` — `IdentifierResolver`: per-`IdentifierInfo` decl
  chains, separate C namespaces (ordinary / tag / label; members live in
  their RecordDecl). `LookupName`, `LookupTagName`, plus `IsTypeName` used
  by the parser for typedef disambiguation.
- `sema_type.cc` — `ConvertDeclSpecToType` (TST/TSW/TSS → QualType) and
  `GetTypeForDeclarator` (walk chunks; diagnose func-returning-func/array,
  array-of-func, etc.), enforce C constraints on array sizes (ICE > 0 or
  incomplete `[]`).
- `sema_decl.cc` — `ActOnDeclarator` (var/typedef/function decls; linkage
  and redeclaration merging per C rules: type compatibility, tentative
  definitions, extern/static mixing), `ActOnStartOfFunctionDef` /
  `ActOnFinishFunctionBody` (param pushing, label resolution, missing-return
  is codegen's problem, C99 6.9.1 param completeness), tag handling
  `ActOnTag` / `ActOnTagFinishDefinition` (forward declarations, member
  redefinition, anonymous struct/union members), `ActOnEnumConstant`
  (value = previous+1 or ICE), `ActOnField` (bitfield width ICE checks,
  incomplete member, flexible array member), `_Static_assert`.
- `sema_expr.cc` — the conversion engine:
  `DefaultFunctionArrayLvalueConversion`, `UsualUnaryConversions`
  (integer promotion), `UsualArithmeticConversions` (C6.3.1.8 rank walk),
  `ImpCastExprToType` materializing `ImplicitCastExpr`s;
  `ActOnIdExpression` (undeclared-identifier error; implicit function decl
  warning per C89 semantics → error in C11 mode like Clang's
  `-Werror=implicit-function-declaration` default),
  `ActOnNumericConstant/CharacterConstant/StringLiteral` (via
  `literal_support`), `ActOnUnaryOp` (incr/decr lvalue+arithmetic checks,
  `&` on lvalue/function, `*` on pointer-to-complete-or-function, arithmetic
  ops), `ActOnBinOp` dispatching to `CheckAdditionOperands`
  (ptr+int, ptr-ptr), `CheckMultiplyDivide…`, `CheckCompare` (ordered:
  real or same-object pointers; equality: +null constants, void*),
  logical ops, `CheckAssignmentOperands` +
  `CheckSingleAssignmentConstraints` (the compat matrix with
  qualifier-discard / int-conversion / incompatible-pointer warnings —
  two diags for this already exist in `diag_kinds.def`),
  `ActOnConditionalOp` (composite type computation),
  `ActOnCallExpr` (arg count/type, default argument promotions for
  variadic/no-proto), `ActOnMemberAccess` (field lookup in record),
  `ActOnArraySubscript`, `ActOnCStyleCast` (scalar↔scalar legality),
  `ActOnSizeofAlignof` (no function/incomplete operands; VLA → runtime
  is out of scope: diagnose), `ActOnGenericSelection` (type-compat match,
  exactly-one-default rules), compound literals.
- `sema_stmt.cc` — if/while/for/do condition→scalar conversion + wrapping,
  `ActOnCaseStmt` ICE + dup detection per switch (`SwitchStack`),
  switch condition integer promotion, break/continue scope-flag checks,
  `ActOnReturnStmt` (void/value mismatches, assignment-style conversion),
  goto/label via `LabelDecl` map with end-of-function undefined-label check.
- `sema_init.cc` — initializer checking, the hard part ported from
  Clang's `InitListChecker`: scalar init, brace-enclosed aggregate walk
  with designators (`.field`, `[index]`), string-literal array init,
  char-array vs wchar rules, excess/missing element diagnostics,
  incomplete array size deduction from init list, constant-initializer
  requirement for static-storage objects.
- `const_eval.cc` — integer constant expression evaluator over the built
  AST (literals, enum constants, unary/binary/conditional/cast/sizeof of
  complete non-VLA types, comma/assign rejected per C6.6), returning
  `std::optional<int64/uint64 + type>`; overflow/div-by-zero diagnostics.

### 2.3 Parser — `include/bcc/parse/parser.hh`, `src/parse/`

`Parser` owns a `Preprocessor&`, a `Sema&`, current token `tok_`, and
one-token helpers (`ConsumeToken`, `ExpectAndConsume(kind, diag)`,
`SkipUntil(set, flags)` with brace/paren/bracket balance like Clang's).
`BalancedDelimiterTracker` equivalent for matched-paren notes (the
`err_expected_rparen`/`note_to_match_this_lparen` diags already exist).

- `parser.cc` — `ParseTranslationUnit`: loop `ParseExternalDeclaration`
  until kEOF; scope RAII (`ParseScope`).
- `parse_decl.cc` — `ParseDeclarationSpecifiers` (the big switch; consults
  `Sema::IsTypeName` for typedef-names; struct/union/enum specifier parsing
  incl. member `ParseStructDeclaration` with bitfields, enumerator lists),
  `ParseDeclarator`/`ParseDirectDeclarator`/`ParseParenDeclarator`
  (pointer chunks, array chunks with `static`/`*`/qualifiers, prototype
  parameter lists in their own `FunctionPrototypeScope`, abstract
  declarators for typenames), `ParseInitDeclaratorListAfterFirstDeclarator`,
  init parsing hookup, `_Static_assert` declarations,
  `ParseDeclOrFunctionDefinition` deciding `{` body vs `;`/`,`.
  K&R identifier-list definitions: recognized and diagnosed as
  unsupported (Clang-style warning is future work).
- `parse_expr.cc` — precedence-climbing exactly like Clang: `prec::Level`
  table (Comma=1 … Conditional … Assignment right-assoc … LogicalOr …
  PointerToMember not needed), `ParseAssignmentExpression`,
  `ParseRHSOfBinaryExpression`, `ParseCastExpression` (primary, unary
  operators, sizeof/_Alignof with the paren-ambiguity lookahead, `(type)`
  cast vs parenthesized expr vs compound literal — resolved by
  `IsTypeName` after `(`), postfix suffix loop (call/index/member/
  incr-decr), `_Generic` parsing, `ParseParenExpression`.
- `parse_stmt.cc` — statement dispatch incl. `identifier :` label
  lookahead (via `LookAhead(0)`), declaration-vs-expression decision at
  block scope (`IsDeclarationSpecifier`), for-init declarations in their
  own scope, switch/case/default nesting.
- `parse_init.cc` — `ParseInitializer` / `ParseBraceInitializer` /
  designation grammar, producing designator lists for `sema_init`.

### 2.4 Lexer additions — `literal_support`

`src/lex/literal_support.cc` ports the essentials of Clang's
`LiteralSupport.cpp`:

- `NumericLiteralParser`: radix detection (10/8/16 + `0b`), digit-run
  validation, suffix parsing (`u`, `l`, `ll`, `f`, mixed), float exponents
  and hex floats; `GetIntegerValue(uint64&)` with overflow flag,
  `GetFloatValue(double&)`.
- `CharLiteralParser`: escapes (`\n \t \' \" \\ \0 \x \octal \u \U`),
  multi-char constants (int value, warning), wide/u16/u32 kinds.
- `StringLiteralParser`: adjacent-literal concatenation (kind mixing
  rules), escape decoding, resulting byte array + element type.

### 2.5 Diagnostics

Extend `diag_kinds.def` with the parser/sema set (~80 new kinds), keeping
Clang's message wording so differential testing lines up:
`err_expected` family, `err_expected_expression`,
`err_expected_semi_declaration`, `err_typecheck_*` family
(`err_typecheck_invalid_operands`, `err_typecheck_subscript_value`, …),
`err_undeclared_var_use`, `err_redefinition`, `note_previous_definition`,
`warn_missing_declaration_specifiers` etc. Severity/wording copied from
`clang/include/clang/Basic/Diagnostic{Parse,Sema}Kinds.td`.

### 2.6 Driver — `bin/cc_dump.cc`

`cc_dump [-I dir] [-ast-dump] [-fsyntax-only] file.c`:
FileManager/SourceManager/Diagnostics/HeaderSearch/Preprocessor setup as in
`pp_dump.cc`, then `ASTContext` + `Sema` + `Parser`, parse the TU, print the
AST dump or nothing; exit 1 on errors. This is the differential-testing
entry point against `clang -fsyntax-only -ast-dump`.

---

## 3. Implementation phases

Each phase compiles and its tests pass before moving on.

1. **AST core + literal support.** `bcc_ast` library: types with uniquing,
   decls/exprs/stmts, ASTContext + x86-64 layout, dumper.
   `literal_support` in `bcc_lex`. Unit tests: type uniquing/compat,
   record layout, literal decoding.
2. **Parser + Sema skeleton, declarations.** DeclSpec/Declarator/Scope;
   `ParseDeclarationSpecifiers` + declarators; `ConvertDeclSpecToType` +
   `GetTypeForDeclarator`; ActOnDeclarator for vars/typedefs/functions;
   lookup + redeclaration; tags (struct/union/enum) and fields.
   Tests: global decls, typedef disambiguation, tag scoping, redecl errors.
3. **Expressions.** Full expression grammar; the conversion engine and all
   binary/unary/call/member/cast checks; implicit casts in the AST.
   Tests: type of expression after conversions, error cases
   (invalid operands, assignment to rvalue, incompatible pointers).
4. **Statements & function bodies.** Compound stmts, control flow, switch
   machinery, labels/goto, return checking. Tests per statement kind.
5. **Initializers & constants.** const_eval; array bounds, bitfield widths,
   enum values, case labels; InitListChecker port with designators;
   static-storage constant initializers. Tests: designated init, string
   init, size deduction, ICE edge cases.
6. **Driver + integration.** `cc_dump`, CMake wiring, regression run over
   `tests/regression` C inputs and differential spot-checks vs
   `clang -std=c11 -fsyntax-only`.

## 4. Testing strategy

- **Unit tests** (gtest, mirroring `tests/unittest/pp` style): a
  `SemaTestFixture` that parses a string through the full
  PP→Parser→Sema pipeline into an ASTContext, exposing the TU decl +
  collected diagnostics for assertions.
- **AST dump goldens** for shape-sensitive cases (implicit cast placement,
  init list structure).
- **Differential testing** against `clang -std=c11 -fsyntax-only` on
  accept/reject decisions (the same methodology used for the preprocessor
  vs clang on the Linux kernel — see memory notes), deferred until the
  driver exists.

## 5. Explicit non-goals (this iteration)

- C++/ObjC anything; GNU extensions beyond what the PP already does.
- VLA semantic evaluation (`sizeof` of VLA, VM types across scopes) —
  VLAs parse to `VariableArrayType` but uses that require evaluation are
  diagnosed as unsupported.
- K&R function definitions (diagnosed), `_Complex`/`_Imaginary` arithmetic
  (parsed, diagnosed), `_Atomic(T)` atomic semantics (parsed as qualifier),
  full floating constant folding, `#pragma` interactions, attributes.
- Codegen; the AST + layout info is shaped so a later lowering phase to
  `bcc_as` can consume it.
