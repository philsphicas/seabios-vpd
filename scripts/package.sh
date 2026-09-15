#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ -e .git && -n $(git status --porcelain --untracked-files=normal) ]]; then
    echo "Packaging requires a clean, committed source tree." >&2
    exit 1
fi
version=$(bash "$(dirname "${BASH_SOURCE[0]}")/version.sh")
rm -rf -- "${WORK:?}/package"
name="seabios-vpd-$version"
source_dir="$WORK/package/$name"
assets="$WORK/package/assets"
mkdir -p "$source_dir" "$assets"
if [[ -e .git ]]; then
    git archive HEAD | tar -xf - -C "$source_dir"
else
    cp -a Makefile config.mk README.md LICENSE NOTICE \
        patches platform config scripts tests .github .gitattributes .gitignore "$source_dir/"
fi
printf '%s\n' "$version" > "$source_dir/VERSION"
cp "$WORK/upstream.tar" "$source_dir/"
(cd "$source_dir" && sha256sum upstream.tar > upstream.sha256)
tar -czf "$assets/$name.tar.gz" -C "$WORK/package" "$name"
cp "$WORK/bios-vpd.bin" LICENSE NOTICE platform/COPYING.GPL-3.0 \
    platform/COPYING.LGPL-3.0 "$assets/"
(cd "$assets" && sha256sum bios-vpd.bin "$name.tar.gz" LICENSE NOTICE \
    COPYING.GPL-3.0 COPYING.LGPL-3.0 > SHA256SUMS)
rm -rf -- "${DIST:?}"
mv "$assets" "$DIST"
echo "Release files: $DIST/"
