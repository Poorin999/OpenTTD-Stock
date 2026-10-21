#!/bin/bash
# Usage: bash apply_overlay.sh <OpenTTD source folder>
set -e
python3 "$(dirname "$0")/apply_overlay.py" "${1:-.}"
