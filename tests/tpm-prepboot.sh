#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
directory=$(mktemp -d "${WORK:?}/prepboot-tests.XXXXXX")
trap 'rm -rf -- "$directory"' EXIT
ulimit -c 0
tar -xf "$WORK/upstream.tar" -C "$directory" \
    src/tcgbios.c src/std/tcg.h src/types.h src/byteorder.h src/hw/tpm_drivers.h src/romfile.h
patch --batch --forward --fuzz=0 -p1 -d "$directory" \
    -i "$PWD/patches/0001-tpm-platform-authorization.patch"

# SeaBIOS function definitions end with an unindented closing brace.
awk '
  /^(tpm20_stirrandom|tpm20_getrandom|tpm20_hierarchychangeauth|tpm20_prepboot|tpm_prepboot)\(/ {
      name = $0; sub(/\(.*/, "", name)
      if (seen[name]++ || previous !~ /^(static )?(int|void)$/) { bad = 1; exit }
      print previous; active = 1; count++
  }
  active { print }
  active && /^}/ { active = 0 }
  { previous = $0 }
  END {
      if (count != 5 || active || bad) {
          print "Missing or ambiguous pinned TPM preparation functions" > "/dev/stderr"
          exit 1
      }
  }
' "$directory/src/tcgbios.c" > "$directory/original.h"
cp "$directory/original.h" "$directory/prepboot-functions.h"

compile() {
    "${CC:-gcc}" -std=gnu11 -O2 -Wall -Wextra -Werror \
        -I"$directory/src" -I"$directory" tests/tpm-prepboot.c -o "$directory/prepboot"
}
compile
"$directory/prepboot"

for mutation in invert-handoff omit-stir omit-separators; do
    case "$mutation" in
        invert-handoff)
            sed 's/if (!compat_tpm_handoff_enabled())/if (compat_tpm_handoff_enabled())/' \
                "$directory/original.h" > "$directory/prepboot-functions.h" ;;
        omit-stir)
            sed 's/int ret = tpm20_stirrandom();/int ret = (0 \&\& tpm20_stirrandom());/' \
                "$directory/original.h" > "$directory/prepboot-functions.h" ;;
        omit-separators)
            sed 's/    tpm_add_event_separators();/    if (0) tpm_add_event_separators();/' \
                "$directory/original.h" > "$directory/prepboot-functions.h" ;;
    esac
    if cmp -s "$directory/original.h" "$directory/prepboot-functions.h"; then
        echo "Mutation did not match pinned preparation source: $mutation" >&2
        exit 1
    fi
    compile
    if "$directory/prepboot" > "$directory/mutation.log" 2>&1; then
        echo "Preparation regression test missed mutation: $mutation" >&2
        exit 1
    fi
    grep -Fq 'Preparation assertion failed:' "$directory/mutation.log" || {
        cat "$directory/mutation.log" >&2
        exit 1
    }
    echo "PASS: preparation test rejects $mutation"
done
