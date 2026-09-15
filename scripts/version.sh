#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ -e .git ]]; then
    if [[ ${GITHUB_REF_TYPE:-} == tag ]]; then
        [[ ${GITHUB_REF_NAME:-} =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.-]+)?$ ]] ||
            { echo "Expected a version tag such as v0.1.0 or v0.1.0-rc.1" >&2; exit 1; }
        version=$(git describe --tags --exact-match --match "$GITHUB_REF_NAME" --dirty)
    else
        version=$(git describe --tags --match 'v[0-9]*' --always --dirty)
    fi
else
    version=$(cat VERSION)
fi
[[ $version =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
    { echo "Invalid firmware version: $version" >&2; exit 1; }
printf '%s\n' "$version"
