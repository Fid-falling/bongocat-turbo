#!/usr/bin/env bash

set -uo pipefail

log="${RUNNER_TEMP:-.}/bongocat-native-build.log"
set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

if ((status != 0)); then
  matches="$(grep -E \
    '(^FAILED:|(^|: )(fatal )?error:|undefined reference|ld: |collect2:|ninja: build stopped|cannot find -l|No rule to make target)' \
    "$log" | tail -n 30 || true)"
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    line=${line//'%'/'%25'}
    line=${line//$'\r'/'%0D'}
    echo "::error title=Native build failure::$line"
  done <<< "$matches"
  exit "$status"
fi
