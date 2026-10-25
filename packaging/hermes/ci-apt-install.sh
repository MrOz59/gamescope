#!/usr/bin/env bash
# apt-get install, naming any package the release does not have in an error
# annotation instead of leaving it to the job log. A package written as
# ?name is optional: installed where the release has it, skipped elsewhere.
set -euo pipefail
apt-get update
wanted=()
missing=()
for pkg in "$@"; do
  optional=0
  if [ "${pkg#\?}" != "$pkg" ]; then
    optional=1
    pkg=${pkg#\?}
  fi
  if apt-cache show "$pkg" >/dev/null 2>&1; then
    wanted+=("$pkg")
  elif [ "$optional" = 1 ]; then
    echo "Skipping optional ${pkg}: not in this release."
  else
    missing+=("$pkg")
  fi
done
if [ "${#missing[@]}" -gt 0 ]; then
  # shellcheck disable=SC1091
  echo "::error title=Missing packages::$(. /etc/os-release; echo "$PRETTY_NAME") has no ${missing[*]}"
  exit 1
fi
apt-get install -y --no-install-recommends "${wanted[@]}"
