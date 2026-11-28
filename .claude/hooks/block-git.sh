#!/usr/bin/env bash
set -euo pipefail

# Read the JSON payload sent by Claude Code on stdin
INPUT=$(cat)

# Extract the shell command using Python (no external jq dependency required)
CMD=$(python3 -c 'import sys, json; print(json.load(sys.stdin).get("tool_input", {}).get("command", ""))' <<< "$INPUT")

# Block any standalone or chained git or gh command
if printf '%s' "$CMD" | grep -Eiq '(^|[;&|`(]|\$\()\s*(git|gh)\b'; then
  echo "BLOCKED: Git and GitHub CLI commands are prohibited in JS8Call-improved." >&2
  exit 2
fi

exit 0
