#!/usr/bin/env bash
# Run one CI step and, when it fails, publish the end of its output as an
# error annotation, where it can be read without the full job log.
#
#   ci-step.sh <title> <command> [args...]
set -uo pipefail
title=${1:?title}
shift
log=$(mktemp)
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
  tail -n 60 "$log" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN { ORS = "%0A" } { print }' |
    sed "s/^/::error title=${title} (exit ${status})::/"
  echo
fi
rm -f "$log"
exit "$status"
