#!/usr/bin/env bash
# Upload all .cptheme files to a remote WebDAV server.
# Usage:  ./scripts/upload-themes.sh [WEBDAV_URL]
#   WEBDAV_URL defaults to http://192.168.0.139/themes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WEBDAV_URL="${1:-http://192.168.0.139/themes}"
BUILD_DIR="$PROJECT_ROOT/build/themes"

# ── 1. Build all themes ───────────────────────────────────────────────
echo "=== Building themes ==="
python3 "$SCRIPT_DIR/build_cptheme.py" --all -d "$BUILD_DIR"

# Verify we have something to upload
cptheme_files=("$BUILD_DIR"/*.cptheme)
if [[ ! -e "${cptheme_files[0]}" ]]; then
  echo "ERROR: No .cptheme files found in $BUILD_DIR" >&2
  exit 1
fi

echo ""
echo "=== Uploading to $WEBDAV_URL ==="

# ── 2. Upload each theme via WebDAV (curl PUT) ────────────────────────
for cptheme in "${cptheme_files[@]}"; do
  filename="$(basename "$cptheme")"
  remote_url="${WEBDAV_URL%/}/$filename"

  echo -n "  $filename ... "
  if curl --silent --show-error \
          --request PUT \
          --upload-file "$cptheme" \
          --max-time 30 \
          "$remote_url"; then
    echo "OK ($(du -h "$cptheme" | cut -f1))"
  else
    echo "FAILED" >&2
    exit 1
  fi
done

echo ""
echo "Done — ${#cptheme_files[@]} theme(s) uploaded."
