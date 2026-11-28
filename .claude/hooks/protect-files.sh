#!/usr/bin/env bash
set -euo pipefail

INPUT=$(cat)

# Extract file_path from Edit or Write tool_input
FILE_PATH=$(python3 -c 'import sys, json; print(json.load(sys.stdin).get("tool_input", {}).get("file_path", ""))' <<< "$INPUT")

# Block any path inside .claude/ or .git/
if printf '%s' "$FILE_PATH" | grep -Eiq '(^|/)\.(claude|git)(/|$)'; then
  echo "BLOCKED: Modifying .claude/ or .git/ files is prohibited in JS8Call-improved." >&2
  exit 2
fi

exit 0

