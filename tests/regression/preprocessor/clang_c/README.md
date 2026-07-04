# clang_c — Clang C preprocessor regression tests

These tests are copied from
`llvm-project/clang/test/Preprocessor` (only the C tests — `.c` files and their
header/`Inputs` dependencies; the C++ tests under that directory are excluded).

The regression runner (`../runner.cc`) preprocesses every `.c` file here with
both `clang -E -P -std=gnu17` and bcc, then compares the normalized output.

## Status

228 of the 237 tests pass. The 9 remaining failures are genuine preprocessor
bugs in bcc (see "Known bugs" below) that are deep enough to warrant their own
follow-up work; they are kept here — and intentionally left failing — so they
are tracked rather than hidden.

While wiring these up, several real bcc bugs were fixed (each unblocked the
listed Clang tests):

- **Placemarker `##` pasting** — `##` whose left operand is an empty argument
  was emitted literally instead of consumed. (`macro_paste_none`,
  `macro_paste_empty`, `c99-6_10_3_4_p7`)
- **Empty-argument spacing propagation** — when a macro argument or nested
  expansion expanded to nothing, its leading-space / start-of-line flags were
  dropped instead of carried to the following token. (`macro_space`,
  `macro_arg_empty`, `macro_expand_empty`)

## Known bugs (9, kept failing on purpose)

| File | Bug |
|------|-----|
| `builtin_line.c`         | `__LINE__` in a function-like macro whose invocation spans several lines reports the macro-name line, not the closing-`)` line (expansion-location mapping). |
| `hash_line.c`            | A `#` following an empty macro expansion on the same source line is mis-handled (directive-vs-token recognition after an empty expansion). |
| `c99-6_10_3_4_p5.c`      | No leading space inserted after a macro-argument substitution when needed to match clang's `AvoidConcat` spacing (e.g. `a+x`, a=1 → clang emits `1 +x`). |
| `macro_rescan.c`         | Same `AvoidConcat` spacing-after-substitution gap as above. |
| `macro_disable.c`        | Function-like macros whose replacement list has an unbalanced `(` (e.g. `i(x) h(x`) don't reach across to consume the caller's `)`. |
| `macro_fn_lparen_scan2.c`| A function-like macro name produced by one expansion followed by `(args)` from the source is not re-expanded (lparen rescan across expansion/source boundary). |
| `macro_fn_comma_swallow2.c` | Variadic `,##__VA_ARGS__` comma handling differs from clang for the empty-vs-absent varargs distinction. |
| `macro_paste_commaext.c` | GNU `,##__VA_ARGS__` elides the comma even when a single (empty) vararg argument is present; clang only elides for zero vararg arguments. |
| `macro_paste_spacing.c`  | `##` whose *left* operand is a placemarker (empty argument) pastes onto the wrong token; needs explicit placemarker tokens in the expansion. |

## Excluded from this directory (unsupported by bcc / not valid `-E` comparisons)

These Clang tests exercise features bcc does not implement, or are `-verify` /
`-Eonly` diagnostic tests that are not meaningful as plain `-E` output
comparisons. They were removed from `clang_c` rather than left failing:

- **`_Pragma` operator** (C99): `_Pragma`, `_Pragma-in-macro-arg`,
  `_Pragma-location`, `_Pragma-newline`, `macro_arg_directive`, `macro_expand`.
- **`#embed` / `__has_embed`** (C23): `embed___has_embed`, `embed_parsing_errors`.
- **Feature-test builtins**: `has_attribute`, `has_c_attribute`,
  `invalid-__has_warning1`, `invalid-__has_warning2`.
- **Clang-specific pragma semantics**: `pragma`, `pragma_assume_nonnull`,
  `pragma_diagnostic`, `pragma-captured`, `pragma-missing-string-token`,
  `ignore-pragmas`, `annotate_in_macro_arg`, `minimize-whitespace`.
- **OpenMP pragma macro expansion**: `openmp-macro-expansion`.
- **Microsoft extensions**: `microsoft-ext`, `macro_paste_msextensions`.
- **Per-language-standard `__STDC_VERSION__`** (bcc has no lang-standard mode):
  `c17`, `c2x`, `c2y`.
- **Builtin/freestanding headers** (`stddef.h`, `stdarg.h`, `stdint.h`):
  `header_lookup1`, `import_self`, `include-directive2`, `header_is_main_file`.
- **`__FUNCTION__`**: `function_macro_file`.
- **`-ffile-prefix-map` / `__FILE__` path representation**: `file_test`.
- **`-source-date-epoch`**: `SOURCE_DATE_EPOCH`.
- **`-detailed-preprocessing-record`**: `pp-record`.
- **Assembler-with-cpp mode**: `assembler-with-cpp`.
- **Directives inside macro arguments** (GNU extension): `macro_fn_disable_expand`.
- **`-verify` / `-Eonly` diagnostic tests** (not `-E` output): `macro_fn`,
  `macro_paste_bad`, `macro_paste_bcpl_comment`, `macro_paste_c_block_comment`,
  `ifdef-recover`, `line-directive` (`-fsyntax-only -verify` line markers).
