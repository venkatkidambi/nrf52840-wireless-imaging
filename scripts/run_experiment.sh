#!/bin/bash
# Bash 3.2 (macOS) compatible. Python owns and reaps all child processes.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$SCRIPT_DIR/run_experiment.py" "$@"
