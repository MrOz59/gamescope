#!/usr/bin/env bash
# Exercise actual helper lookup, layer data discovery and argument forwarding
# without starting a compositor or requiring a GPU.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
prefix="$work/a private prefix/usr/lib/hermes-gamescope"
mkdir -p "$prefix"/{bin,libexec,share/vulkan/implicit_layer.d} "$work/public"
install -m755 "$here/gamescope-launch" "$prefix/bin/gamescope"
ln -s "$prefix/bin/gamescope" "$work/public/gamescope-hermes"
cat > "$prefix/bin/gamescopereaper" <<'HELPER'
#!/bin/sh
printf '%s\n' private-helper
HELPER
cat > "$prefix/libexec/gamescope" <<'PROGRAM'
#!/bin/sh
set -eu
test "$(gamescopereaper)" = private-helper
test "$(command -v gamescopereaper)" = "$EXPECTED_PREFIX/bin/gamescopereaper"
test "$XDG_DATA_DIRS" = "$EXPECTED_PREFIX/share:$EXPECTED_DATA_DIRS"
test "$#" -eq 3
test "$1" = '--'
test "$2" = 'a game with spaces'
test "$3" = ''
exit 37
PROGRAM
chmod 755 "$prefix/bin/gamescopereaper" "$prefix/libexec/gamescope"
export EXPECTED_PREFIX="$prefix"
for launcher in "$prefix/bin/gamescope" "$work/public/gamescope-hermes"; do
  for data in default custom; do
    if [ "$data" = default ]; then
      unset XDG_DATA_DIRS
      export EXPECTED_DATA_DIRS=/usr/local/share:/usr/share
    else
      export XDG_DATA_DIRS='/custom data:/usr/share'
      export EXPECTED_DATA_DIRS="$XDG_DATA_DIRS"
    fi
    result=0
    "$launcher" -- 'a game with spaces' '' || result=$?
    test "$result" -eq 37
  done
done
printf '%s\n' 'Private launcher: PASS'
