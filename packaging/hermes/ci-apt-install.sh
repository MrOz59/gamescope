#!/usr/bin/env bash
# apt-get install, naming any package the release does not have in an error
# annotation instead of leaving it to the job log.
set -euo pipefail
apt-get update
missing=()
for pkg in "$@"; do
  apt-cache show "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if [ "${#missing[@]}" -gt 0 ]; then
  # shellcheck disable=SC1091
  echo "::error title=Missing packages::$(. /etc/os-release; echo "$PRETTY_NAME") has no ${missing[*]}"
  exit 1
fi
apt-get install -y --no-install-recommends "$@"
