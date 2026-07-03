# Preprocessor Implementation TODO

Gap analysis: bcc vs Clang's preprocessor (clang/lib/Lex/).

Each item includes the file(s) most likely affected, an estimate of the
effort (Small / Medium / Large), and a brief implementation sketch.

---

## P0 — Missing Standard Directives

### 1. Implement `#include_next`

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/pp/pp_keywords.def` (keyword exists), `include/bcc/pp/header_search.hh`
- **Effort:** Medium
- **Sketch:**
  1. Add a handler branch in `HandleDirective` for `kIncludeNext` (currently falls through to default).
  2. Lex the header name (same as `#include`).
  3. In `HeaderSearch`, add a method that starts searching from the directory *after* the includer's directory in the search path list (instead of the includer's directory), which is the standard `#include_next` semantics.
  4. Wire it into `LookupFile` or add a separate `LookupFileNext` entry point.
  5. Add a `PPCallbacks::InclusionDirective` callback for it.
  6. Register diagnostic `diag::err_pp_include_next_not_found` (or reuse existing file-not-found).

### 2. Implement `#ident` / `#sccs`

- **Files:** `src/pp/pp_directives.cc`
- **Effort:** Small
- **Sketch:**
  1. Add handler branches for `kIdent` and `kSccs`.
  2. Lex the rest of the directive line as a string (or verbatim text).
  3. Silently discard the content (as Clang does on non-ELF targets), or emit an `__ident__`-like object in ELF `.comment` sections later.
  4. Fire `PPCallbacks::IdentDirective` callback.

### 3. Implement `#import`

- **Files:** `src/pp/pp_directives.cc`
- **Effort:** Small
- **Sketch:**
  1. Add handler branch for `kImport`.
  2. Same logic as `#include`, but after processing the file, mark its `HeaderFileInfo` so a subsequent `#import` of the same file is skipped.
  3. Clang's implementation is literally `HandleIncludeDirective` with `isImport = true`.

### 4. Implement `__include_macros`

- **Files:** `src/pp/pp_directives.cc`
- **Effort:** Small
- **Sketch:**
  1. Add handler for `kIncludeMacros`.
  2. Include the named file, but only process macro definitions from it.
  3. This is a rarely-used GCC extension; can be implemented as a thin wrapper around the include machinery.

---

## P0 — Builtin / Predefined Macros

### 5. Register `__STDC__`, `__STDC_VERSION__`, `__STDC_HOSTED__`

- **Files:** `src/pp/preprocessor.cc` (RegisterBuiltinMacros or Predefines)
- **Effort:** Small
- **Sketch:**
  1. Add `__STDC__` → `1`, `__STDC_HOSTED__` → `1`, `__STDC_VERSION__` → `201112L` (or `202311L` for C23) to the Predefines string or as builtin macros.
  2. Wire the values to language-option flags so changing the `-std=` flag automatically updates the version macro.
  3. Add unit tests verifying the expected value per standard mode.

### 6. Register `__BASE_FILE__`

- **Files:** `src/pp/preprocessor.cc`, `include/bcc/pp/preprocessor.hh`
- **Effort:** Small
- **Sketch:**
  1. Store the top-level (main) file name in the `Preprocessor`.
  2. Register builtin macro `__BASE_FILE__` that expands to the stored name as a string literal.
  3. During `ExpandBuiltinMacro`, emit the stringified top-level filename (walk to bottom of include stack when expanding).

### 7. Register `__TIMESTAMP__`

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Small
- **Sketch:**
  1. Register builtin macro `__TIMESTAMP__`.
  2. During `ExpandBuiltinMacro`, use `stat()` on the current source file to get its `st_mtime`, then format with `strftime("%a %b %e %T %Y")`.
  3. Cache the value per file (or compute once per invocation for simplicity).

### 8. Register `__FILE_NAME__`

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Small
- **Sketch:**
  1. Register builtin macro `__FILE_NAME__`.
  2. During expansion, extract the last path component from `__FILE__`'s resolved name.

### 9. Register `_Pragma` operator

- **Files:** `src/pp/preprocessor.cc`, `src/pp/pp_directives.cc`
- **Effort:** Medium
- **Sketch:**
  1. Register `_Pragma` as a builtin (object-like) macro that expands to nothing, but intercept it in the token stream.
  2. During expansion, parse the string argument, extract the pragma content, and dispatch to the same handler path used by `#pragma`.
  3. Follow Clang's `Handle_Pragma()` in `PPDirectives.cpp`.

### 10. Register `__FLT_EVAL_METHOD__`

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Small
- **Sketch:**
  1. Register builtin macro `__FLT_EVAL_METHOD__`.
  2. Emit the target's `float_eval_method` value as a numeric constant.
  3. Pull from `TargetInfo` (once bcc has target info).

---

## P0 — `#pragma` Handlers

### 11. Implement `#pragma GCC poison` / `#pragma clang poison`

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/pp/preprocessor.hh`
- **Effort:** Medium
- **Sketch:**
  1. Add a poison set (`std::set<IdentifierInfo*>`) to `Preprocessor`.
  2. In `HandlePragmaDirective`, recognise `poison` under both `GCC` and `clang` namespaces.
  3. Lex identifier tokens after `poison`, inserting each into the poison set.
  4. In `LexUnexpandedToken` or during macro lookup, check the poison set and emit `diag::err_pp_poisoned_macro` when a poisoned identifier is referenced.
  5. `#undef` must also clear the poisoned state for that identifier.

### 12. Implement `#pragma GCC system_header`

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/basic/file_manager.hh`
- **Effort:** Small
- **Sketch:**
  1. Recognise `system_header` under `GCC` and `clang` namespaces.
  2. Mark the current `FileEntry` as a system header.
  3. This affects diagnostic suppression (system headers suppress warnings).

### 13. Implement `#pragma GCC dependency`

- **Files:** `src/pp/pp_directives.cc`
- **Effort:** Small
- **Sketch:**
  1. Recognise `dependency` under `GCC` and `clang` namespaces.
  2. Parse the filename string, check if the dependency file is newer than the current file; emit `diag::warn_pp_dependency_out_of_date` if so.
  3. Optionally parse a second string literal for a custom diagnostic message.

### 14. Implement `#pragma GCC diagnostic` / `#pragma clang diagnostic`

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/basic/diagnostic.hh`
- **Effort:** Large
- **Sketch:**
  1. Add a diagnostic-pragma stack to `DiagnosticsEngine`.
  2. Recognise `diagnostic` under both `GCC` and `clang` namespaces.
  3. Parse sub-commands: `push`, `pop`, `error`, `warning`, `ignored`, `fatal`.
  4. For `push`/`pop`, maintain a stack of diagnostic-state snapshots.
  5. For severity changes (`error`/`warning`/`ignored`/`fatal`), parse the `-Wflag` and map it to a `DiagnosticID`, then update its severity.
  6. The `GCC` variant uses `GCC diagnostic ignored "-Wflag"` syntax; the `clang` variant is identical.

### 15. Implement `#pragma push_macro` / `#pragma pop_macro`

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/pp/preprocessor.hh`
- **Effort:** Medium
- **Sketch:**
  1. Add a macro-snapshot stack to `Preprocessor` (`std::map<std::string, std::deque<MacroDirective*>>`).
  2. `#pragma push_macro("NAME")`: snapshot the current `MacroDirective` for `NAME` onto its stack.
  3. `#pragma pop_macro("NAME")`: restore the most recent snapshot (undo any intervening `#define`/`#undef`).
  4. Clang stores `MacroDirective*` pointers; bcc's `MacroDirective` linked-list design already supports this style.

### 16. Implement `#pragma STDC` pragmas

- **Files:** `src/pp/pp_directives.cc`
- **Effort:** Small
- **Sketch:**
  1. Recognise `STDC` namespace in `HandlePragmaDirective`.
  2. Parse `FENV_ACCESS`, `FP_CONTRACT`, `CX_LIMITED_RANGE`.
  3. Silently accept and discard (Clang does the same for unsupported standard pragmas).
  4. Fire the `PPCallbacks::PragmaDirective` callback.

---

## P1 — Missing `#if` Expression Features

### 17. Evaluate `__has_include` / `__has_include_next` in `#if`

- **Files:** `src/pp/pp_expressions.cc`, `src/pp/preprocessor.cc`
- **Effort:** Medium
- **Sketch:**
  1. In `LexConditionToken`, intercept `__has_include` and `__has_include_next` tokens (they are registered as PP keywords but currently not handled).
  2. Parse `(` header-name `)`, looking up the header via `HeaderSearch`.
  3. Return `1` if found, `0` otherwise.
  4. `__has_include_next` uses the directory-after-includer logic (same as `#include_next`).
  5. Must guard against recursive `__has_include` calls (Clang sets a `DisableMacroExpansion` flag).

### 18. Evaluate `__has_feature` / `__has_extension` / `__has_warning` / `__is_identifier`

- **Files:** `src/pp/pp_expressions.cc`, `src/pp/preprocessor.cc`
- **Effort:** Medium
- **Sketch:**
  1. Register as builtin macros (following Clang) so they expand to `0` or `1` tokens in `#if` context.
  2. Or handle them in `LexConditionToken` like `__has_builtin`.
  3. For `__has_feature`/`__has_extension`, return 1/0 based on language options (initially all return 0 for unsupported features).
  4. For `__has_warning`, parse the `-Wflag` and check the diagnostic registry.
  5. For `__is_identifier`, check if the argument token kind is `tok::identifier`.
  6. For `__has_constexpr_builtin`, initially return 0.

### 19. Register and evaluate `__has_cpp_attribute` / `__has_c_attribute` / `__has_declspec_attribute`

- **Files:** `src/pp/preprocessor.cc`, `include/bcc/pp/pp_keywords.def`
- **Effort:** Small
- **Sketch:**
  1. Register as builtin macros.
  2. Initially return `0` (no attributes supported).
  3. Can be extended later when bcc gains attribute support.

---

## P1 — Target / Platform Predefined Macros

### 20. Add Target-Info layer and Predefines string

- **Files:** `src/pp/preprocessor.cc`, `include/bcc/basic/target_info.hh` (new)
- **Effort:** Large
- **Sketch:**
  1. Create a `TargetInfo` abstraction that captures CPU, OS, ABI, pointer width, endianness, float eval method.
  2. Populate a `Predefines` string (like Clang's `InitPreprocessor.cpp`) with target-specific macros:
     - `__LP64__`, `__i386__`, `__x86_64__`, `__arm__`, etc.
     - `__APPLE__`, `__linux__`, `__FreeBSD__`, etc.
     - `__SIZE_TYPE__`, `__PTRDIFF_TYPE__`, `__INT_MAX__`, etc.
  3. Process the Predefines buffer before the main file (same approach as Clang's `<built-in>` source buffer).
  4. Wire `-m32`/`-m64`/`-target` flags to the TargetInfo construction from the driver.

---

## P1 — Macro Correctness

### 21. ISO macro redefinition checking

- **Files:** `src/pp/pp_directives.cc`, `include/bcc/pp/macro_info.hh`
- **Effort:** Medium
- **Sketch:**
  1. In `HandleDefineDirective` (or `AppendDefMacroDirective`), when a macro with the same name already exists:
     a. Compare the parameter lists (same number of params, same variadic-ness).
     b. Compare replacement token sequences token-by-token (same spelling, excluding leading/trailing whitespace differences).
  2. If not identical, emit `diag::warn_pp_macro_redefined` (or `ext_pp_macro_redefined` as an extension).
  3. If identical, suppress the redefinition (don't create a new `MacroDirective`).

### 22. Diagnose invalid token pastes

- **Files:** `src/pp/pp_macro_expansion.cc`
- **Effort:** Small
- **Sketch:**
  1. In `PasteTokens`, when concatenation fails to produce a single valid token, emit `diag::err_pp_invalid_token_paste`.
  2. Currently the two tokens are kept side-by-side without a diagnostic, matching GCC/Clang extension behavior, but ISO C requires a diagnostic.

---

## P1 — Special Identifiers in `#if`

### 23. Implement `__has_include` / `__has_include_next` via builtin macros (instead of special-casing in LexConditionToken)

- **Files:** `src/pp/preprocessor.cc` (ExpandBuiltinMacro), `src/pp/pp_expressions.cc`
- **Effort:** Medium
- **Sketch:**
  1. Follow Clang's approach: `__has_include` and `__has_include_next` are registered as builtin macros.
  2. During `ExpandBuiltinMacro`, intercept them, parse the header name, look up the file, and emit `1` or `0`.
  3. This naturally works inside `#if` because the macro expansion happens before expression evaluation.
  4. Guard against infinite recursion (disable macro expansion during the lookup).

---

## P1 — Infrastructure Improvements

### 24. Add dependency / `-M` output support

- **Files:** `src/pp/preprocessor.cc`, `include/bcc/pp/pp_callbacks.hh`
- **Effort:** Medium
- **Sketch:**
  1. Add a callback or hook that records every `#include` / `#import` / `#include_next` directive's resolved file path.
  2. The driver can then write the collected dependencies in Makefile format (`-M`, `-MM`, `-MD`, etc.).
  3. Clang uses a separate `DependencyOutput` helper, but the same can be achieved with `PPCallbacks::InclusionDirective`.

### 25. Implement `-C` / `-CC` comment-preservation mode

- **Files:** `src/pp/pp_lexer.cc`, `src/pp/preprocessor.cc`
- **Effort:** Medium
- **Sketch:**
  1. Add a `KeepComments` flag to `Preprocessor` (set by `-C`).
  2. In `PPLexer::Lex()`, when `KeepComments` is true, emit `kComment` tokens to the output instead of dropping them.
  3. `-CC` additionally preserves comments inside macro expansions.
  4. The `PreprocessWithBcc` runner and Clang baseline both use `-P` (strip comments), so this doesn't affect existing regression tests.

---

## P2 — Nice-to-Have

### 26. Implement `__COUNTER__` state serialization for PCH

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Small
- **Sketch:**
  1. `__COUNTER__` is already implemented, but its state isn't saved/restored for PCH.
  2. Add a `setCounterValue()` method and serialize it.

### 27. Implement `__has_embed` (C23)

- **Files:** `src/pp/preprocessor.cc`, `include/bcc/pp/pp_keywords.def`
- **Effort:** Medium
- **Sketch:**
  1. Register `__has_embed` as a builtin macro.
  2. The C23 `#embed` directive is a much larger feature; `__has_embed` can initially return `0`.

### 28. Implement token caching / backtracking

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Large
- **Sketch:**
  1. Add a token cache (ring buffer) and backtracking stack.
  2. This is essential for Clang's `Parser::ParseExpression()` which speculative-parses ambiguous constructs.
  3. bcc's parser would drive this; the PP just provides the API (`EnableBacktrackAtThisPos`, `CommitBacktrackedTokens`, `PopBacktrackState`).

### 29. Statistics and counters

- **Files:** `src/pp/preprocessor.cc`
- **Effort:** Small
- **Sketch:**
  1. Add `PrintStats()` output (directive count, token count, macro count, paste count).
  2. Add `getNumDirectives()`, `getTokenCount()` accessors.
  3. Useful for debugging and regression analysis.

### 30. `_Pragma` operator string parsing

- **Files:** `src/pp/pp_macro_expansion.cc`, `src/pp/pp_directives.cc`
- **Effort:** Medium
- **Sketch:**
  1. Build on item 9 above. Parse the string argument to `_Pragma("...")`, unescape it, extract the pragma name and arguments.
  2. Dispatch to the same code path as `#pragma`.
  3. This is required for correctness when macros expand to `_Pragma` constructs.

---

## Key Design Principles

1. **Test-first:** Every feature should be covered by an existing clang Lexer test (adapted for bcc's output-comparison runner) before implementation.
2. **Minimal surface area:** Add only the code paths needed to produce correct `-E -P` output. Full diagnostic quality can follow later.
3. **Align with Clang's architecture where possible:** The lexer-stack, `TokenLexer`, and `MacroDirective` linked-list already mirror Clang's design.
4. **Predefines buffer:** Use the same `<built-in>` source-buffer approach for predefined macros, keeping the core preprocessor free of backend concerns.
