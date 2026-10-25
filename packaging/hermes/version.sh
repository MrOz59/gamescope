#!/usr/bin/env bash
# The package version of this checkout: the upstream release the branch is
# based on (packaging/hermes/VERSION, bumped when hermes is rebased past a new
# Gamescope release), then the commit count and hash - 3.17.0.r2861.g76e3935.
# Valid as a pacman, Debian and RPM version alike.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
base=$(tr -d '[:space:]' <"$here/VERSION")
printf '%s.r%s.g%s\n' "$base" "$(git -C "$root" rev-list --count HEAD)" "$(git -C "$root" rev-parse --short=7 HEAD)"
