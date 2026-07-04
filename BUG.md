# BUG.md — Known preprocessor bugs identified via Clang's test suite

This file tracks bugs found in bcc's preprocessor by running Clang's C
preprocessor tests (under `tests/regression/preprocessor/clang_c/`) and
diffing bcc's `-E` output against `clang -E -P -std=gnu17`.

Each test in `clang_c` is exercised by the regression runner
(`tests/regression/preprocessor/runner.cc`). To reproduce any failure and see
the token-level diff, from the `build/` directory run:

```
BCC_PP_FILTER="clang_c/<file>.c" BCC_PP_DIFF=1 \
  ./tests/regression/preprocessor/bcc_pp_regression --gtest_brief=1
```

Bugs are grouped into **Open** (still failing, intentionally kept in the suite
so they are tracked) and **Fixed** (resolved while wiring up these tests).
A third section lists test files that were **Excluded** because they exercise
features bcc does not implement (these are *not* bugs).

Current clang_c status: **228 / 237 passing**.

---

## Open bugs

### BCC-PP-001 — `__LINE__` reports the macro-name line, not the closing-`)` line

- **Test case:** `clang_c/builtin_line.c`
- **Standard ref:** C99 6.10.8 / Clang PR3579
- **Symptom:** For a function-like macro whose invocation spans several physical
  lines, `__LINE__` inside it must expand to the line of the closing `)`. bcc
  expands it to the line of the macro *name* token.
  ```
   expected (clang): A 13       // ')' is on line 13
   actual (bcc):    A 11        // 'X(' is on line 11
  ```
- **Root cause:** `Preprocessor::ExpandBuiltinMacro`
  (`src/pp/pp_macro_expansion.cc:136-140`) computes `__LINE__` from
  `sm_.GetExpansionLoc(tok.GetLocation())`, but the expansion location is built
  in `TokenLexer`'s constructor from `macro_name_tok` only; it does not span to
  the invocation's closing paren.
- **Fix direction:** extend the macro-expansion `SourceRange` to end at the `)`
  for function-like macros and resolve `__LINE__` against that end location.

### BCC-PP-002 — `#` after an empty macro expansion is mis-recognized

- **Test case:** `clang_c/hash_line.c`
- **Symptom:** `EMPTY #` (where `#define EMPTY`) must output the `#` as an
  ordinary token on its own line; bcc either glues it to the previous line or
  treats it as a directive.
  ```
   // source:  1\nEMPTY #\n2
   expected:  1\n #\n2
   actual:    1 #\n2          (or directive-consumed)
  ```
- **Root cause:** Interaction between the empty-expansion start-of-line
  propagation (added for BCC-PP-007) and directive recognition in
  `Preprocessor::CLK_Lexer` (`src/pp/preprocessor.cc:255`): a `#` that becomes
  start-of-line through an empty expansion is dispatched to `HandleDirective`.
- **Fix direction:** bcc recognises directives by the `IsStartOfLine()` flag on
  the `#` token; clang keys off the underlying source line position instead.
  Directive recognition should be based on physical line position, not the
  propagated flag.

### BCC-PP-003 — No `AvoidConcat`-style space after macro-argument substitution

- **Test cases:** `clang_c/c99-6_10_3_4_p5.c`, `clang_c/macro_rescan.c`
- **Symptom:** After substituting a macro argument, clang inserts a leading
  space on the following token when needed to prevent the argument's last token
  from re-lexing into a different token with the next one. bcc emits no space.
  ```
   #define M1(a) (a+1)
   M1(17)  ->  clang: (17 +1)   bcc: (17+1)
                  ^^                  ^
   #define C(x) (x + 1)
   C(42)   ->  clang: ... 2 +(3,4) ...   bcc: ... 2+(3,4) ...
  ```
  Note: clang applies this only at argument-substitution boundaries, not in raw
  source (`1+2` stays `1+2`), so it cannot be approximated in the runner's
  output reconstruction.
- **Root cause:** `TokenLexer::BuildExpansion`
  (`src/pp/pp_macro_expansion.cc`) never sets `kLeadingSpace` on the body token
  following a substituted parameter based on clang's `AvoidConcat` rules.
- **Fix direction:** port clang's `AvoidConcat` predicate and apply it when
  emitting the token that follows a parameter substitution.

### BCC-PP-004 — Function-like macros with an unbalanced `(` in the body

- **Test case:** `clang_c/macro_disable.c`
- **Symptom:** A macro whose replacement list opens a parenthesis it does not
  close (e.g. `#define i(x) h(x`) must reach across to consume the caller's
  matching `)`. bcc does not.
  ```
   #define i(x) h(x
   #define h(x) x(void)
   extern int i(i));   ->  clang: extern int i(void)   bcc: extern int h(void;
  ```
- **Root cause:** argument scanning / rescan in `pp_macro_expansion.cc` and
  `preprocessor.cc` does not let a macro body's trailing open-paren pull in
  tokens from outside the original argument list.
- **Fix direction:** when a rescanned body ends mid-expression with an open
  paren, continue consuming tokens from the surrounding stream until balanced.

### BCC-PP-005 — Function-like macro not re-expanded across an expansion/source boundary

- **Test case:** `clang_c/macro_fn_lparen_scan2.c`
- **Symptom:** A function-like macro name produced by one expansion, followed by
  `(args)` coming from the source stream, must be expanded; bcc leaves it.
  ```
   #define F(a) a
   #define FUNC(a) (a+1)
   F(FUNC) FUNC (3);   ->  clang: FUNC (3 +1);   bcc: FUNC FUNC (3);
  ```
  (The first `FUNC` is not followed by `(` and correctly stays; the second
  `FUNC` is followed by `(3)` and should expand.)
- **Root cause:** lparen look-ahead / rescan
  (`Preprocessor::IsNextTokenLParen`, `src/pp/preprocessor.cc:327`) across the
  boundary between an expanded token stream and the parent source stream.
- **Fix direction:** ensure `(`s originating from the source lexer are still
  seen as the invocation opener for a macro name ending an expansion.

### BCC-PP-006 — Variadic `, ## __VA_ARGS__` comma handling differs from clang

- **Test cases:** `clang_c/macro_fn_comma_swallow2.c`,
  `clang_c/macro_paste_commaext.c`
- **Symptom:** bcc elides the comma before `##__VA_ARGS__` whenever the varargs
  token sequence is empty. clang elides only when *zero* variadic arguments were
  supplied; a single (empty) argument keeps the comma.
  ```
   #define debug(format, ...) format, ## __VA_ARGS__)
   debug(Y, );   ->  clang: Y,);   bcc: Y);
   debug(V);     ->  clang: V);    bcc: V);     // both elide (zero varargs)
  ```
- **Root cause:** the GNU comma-elision guard in
  `TokenLexer::BuildExpansion` (`src/pp/pp_macro_expansion.cc:595`) tests
  `args->GetUnexpArgument(pidx).empty()`, which is true both for "no vararg
  arguments" and "one empty vararg argument". It cannot distinguish the two.
- **Fix direction:** track the count of supplied arguments separately from the
  expanded token count, and only elide when the variadic parameter received zero
  arguments.

### BCC-PP-007 — `##` with an empty (placemarker) left operand pastes the wrong token

- **Test case:** `clang_c/macro_paste_spacing.c`
- **Symptom:** When the left operand of `##` is an empty argument (placemarker),
  the result should be just the right operand placed at the left operand's
  position. bcc instead pastes the right operand onto the token *before* the
  empty operand.
  ```
   #define B(x, y) [ ... [y ## z] ... ]
   B(x,)   ->  clang: ... [z] ...    bcc: ... [ z] ...   (wrong token / space)
  ```
- **Root cause:** `TokenLexer::AppendOrPaste` (`src/pp/pp_macro_expansion.cc`)
  treats `out.back()` as the paste left operand, but when the left operand
  contributed nothing, `out.back()` is the preceding token, not a placemarker.
  An attempted fix using `out.size()` deltas was reverted because a successful
  paste also leaves `out.size()` unchanged, causing false positives.
- **Fix direction:** introduce explicit placemarker tokens into the
  `BuildExpansion` output stream, handle placemarker operands in
  `AppendOrPaste` (paste with a placemarker yields the other operand), and strip
  placemarkers at the end. This mirrors how clang implements `##`.

---

## Fixed bugs

### BCC-PP-FIX-001 — Placemarker `##` pasting emitted a literal `##`

- **Test cases:** `clang_c/macro_paste_none.c`, `clang_c/macro_paste_empty.c`,
  `clang_c/c99-6_10_3_4_p7.c`
- **Symptom (before fix):** `##` whose left operand was an empty argument was
  emitted literally instead of consumed, e.g. `#define A(B,C) B ## C` with
  `A(,)` produced `##` (so `!A(,)!` -> `!##!` instead of `!!`).
- **Fix:** `src/pp/pp_macro_expansion.cc`, `TokenLexer::BuildExpansion` — the
  `##` handler's `!owned_tokens_.empty()` guard caused the operator to be
  emitted as a token when the left operand was a placemarker. The guard was
  removed; `AppendOrPaste` already handled the placemarker cases correctly.

### BCC-PP-FIX-002 — Dropped leading-space / start-of-line across empty expansions

- **Test cases:** `clang_c/macro_space.c`, `clang_c/macro_arg_empty.c`,
  `clang_c/macro_expand_empty.c`
- **Symptom (before fix):** when a macro argument or a nested macro expansion
  expanded to nothing, its `kLeadingSpace` / `kStartOfLine` flags were dropped
  instead of carried onto the following token (e.g. `#define FOO() BAR() second`
  with `BAR()` empty produced `first second` on one line instead of
  `first\n second`).
- **Fix:**
  - `include/bcc/pp/token_lexer.hh` — added `HasUnconsumedLeadingSpace()`,
    `HasUnconsumedStartOfLine()`, `InheritLeadingSpaceForNext()`,
    `InheritStartOfLineForNext()`, and the `inherit_leading_space_` /
    `inherit_start_of_line_` members.
  - `src/pp/pp_macro_expansion.cc` — `Lex()` applies the inherited flags to the
    next emitted token; `BuildExpansion` no longer sets `pending_leading_space_`
    for a `##` operand (a placemarker paste operand must not contribute a
    propagating space).
  - `src/pp/preprocessor.cc` — `RemoveTopOfLexerStack()` propagates an exhausted
    empty expansion's flags to the parent token-lexer (or the source lexer).

---

## Excluded tests (not bugs — features bcc does not implement)

The following Clang tests were removed from `clang_c` because they are not
valid `-E`-output comparisons for bcc. They are listed here so nothing is lost;
full rationale is in `tests/regression/preprocessor/clang_c/README.md`.

| Category | Tests |
|----------|-------|
| `_Pragma` operator (C99) | `_Pragma`, `_Pragma-in-macro-arg`, `_Pragma-location`, `_Pragma-newline`, `macro_arg_directive`, `macro_expand` |
| `#embed` / `__has_embed` (C23) | `embed___has_embed`, `embed_parsing_errors` |
| Feature-test builtins | `has_attribute`, `has_c_attribute`, `invalid-__has_warning1`, `invalid-__has_warning2` |
| Clang-specific pragma semantics | `pragma`, `pragma_assume_nonnull`, `pragma_diagnostic`, `pragma-captured`, `pragma-missing-string-token`, `ignore-pragmas`, `annotate_in_macro_arg`, `minimize-whitespace` |
| OpenMP pragma expansion | `openmp-macro-expansion` |
| Microsoft extensions | `microsoft-ext`, `macro_paste_msextensions` |
| Per-standard `__STDC_VERSION__` | `c17`, `c2x`, `c2y` |
| Builtin/freestanding headers | `header_lookup1`, `import_self`, `include-directive2`, `header_is_main_file` |
| `__FUNCTION__` | `function_macro_file` |
| `-ffile-prefix-map` / `__FILE__` path | `file_test` |
| `-source-date-epoch` | `SOURCE_DATE_EPOCH` |
| `-detailed-preprocessing-record` | `pp-record` |
| Assembler-with-cpp mode | `assembler-with-cpp` |
| Directives inside macro args (GNU ext.) | `macro_fn_disable_expand` |
| `-verify` / `-Eonly` diagnostic tests | `macro_fn`, `macro_paste_bad`, `macro_paste_bcpl_comment`, `macro_paste_c_block_comment`, `ifdef-recover`, `line-directive` |
