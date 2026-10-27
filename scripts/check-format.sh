#!/usr/bin/env bash
# Fails if any C source deviates from .clang-format. Used by CI and locally.
set -euo pipefail
cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files '*.c' '*.h' 2>/dev/null || find src include tests tools -name '*.[ch]')
[[ ${#files[@]} -eq 0 ]] && mapfile -t files < <(find src include tests tools -name '*.[ch]')
"${CLANG_FORMAT:-clang-format}" --dry-run --Werror "${files[@]}"
echo "format ok (${#files[@]} files)"
