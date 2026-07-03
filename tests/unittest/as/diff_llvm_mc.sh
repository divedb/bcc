#!/usr/bin/env bash
#
# Differential test: assemble each .s corpus file with both bcc-as and llvm-mc
# and compare the resulting .text bytes and relocation entries. Any mismatch
# fails. Skipped automatically when llvm-mc / llvm-objdump are not installed.
#
# Usage: diff_llvm_mc.sh <path-to-bcc-as> [corpus.s ...]
#
# Note: bcc-as emits fixed rel32 branches (no rel8 relaxation yet), so files
# exercising short local jumps may differ in the branch bytes only; the corpus
# here avoids relying on relaxation.

set -u

BCC_AS="${1:?usage: diff_llvm_mc.sh <bcc-as> [files...]}"
shift || true

LLVM_MC="$(command -v llvm-mc || echo /usr/local/opt/llvm/bin/llvm-mc)"
OBJDUMP="$(command -v llvm-objdump || echo /usr/local/opt/llvm/bin/llvm-objdump)"

if [[ ! -x "$LLVM_MC" || ! -x "$OBJDUMP" ]]; then
  echo "SKIP: llvm-mc / llvm-objdump not found"
  exit 0
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
FILES=("$@")
[[ ${#FILES[@]} -eq 0 ]] && FILES=("$HERE"/corpus/*.s)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Compare only the raw encoding bytes (strip address prefix and the
# mnemonic/comment column, which names symbols and thus varies harmlessly).
textbytes() { "$OBJDUMP" -d "$1" | grep -E '^[[:space:]]+[0-9a-f]+:' \
              | sed -E 's/^[[:space:]]*[0-9a-f]+:[[:space:]]*//; s/\t.*$//'; }
relocs()    { "$OBJDUMP" -r "$1" | grep -E 'R_X86_64' | awk '{print $2, $3}'; }

fail=0
for f in "${FILES[@]}"; do
  [[ -e "$f" ]] || continue
  "$BCC_AS" -o "$TMP/our.o" "$f" 2> "$TMP/our.err"
  if [[ $? -ne 0 ]]; then
    echo "FAIL  $f (bcc-as error)"; cat "$TMP/our.err"; fail=1; continue
  fi
  "$LLVM_MC" -triple=x86_64-linux-gnu -filetype=obj -o "$TMP/ref.o" "$f" \
      2> /dev/null || { echo "SKIP  $f (llvm-mc rejected)"; continue; }

  if diff <(textbytes "$TMP/our.o") <(textbytes "$TMP/ref.o") > "$TMP/td" &&
     diff <(relocs "$TMP/our.o")    <(relocs "$TMP/ref.o")    > "$TMP/rd"; then
    echo "PASS  $f"
  else
    echo "FAIL  $f"; cat "$TMP/td" "$TMP/rd"; fail=1
  fi
done
exit $fail
