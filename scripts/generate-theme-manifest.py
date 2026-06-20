#!/usr/bin/env python3
"""Generate a themes.json manifest from a directory of .cptheme archives.

Reads each archive's embedded theme.json for id/name/description, computes
size + crc32, and emits a manifest for device-initiated theme downloads.
Mirrors generate-font-manifest.py.

Usage:
    python3 scripts/generate-theme-manifest.py \
        --input dist \
        --base-url "https://example.com/themes/" \
        --output dist/themes.json
"""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cptheme_version import THEME_MANIFEST_VERSION


def compute_crc32(filepath: Path) -> int:
    """CRC32 matching esp_rom_crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF."""
    crc = 0
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def read_theme_meta(filepath: Path) -> dict | None:
    """Pull id/name/description from the archive's theme.json."""
    try:
        with zipfile.ZipFile(filepath) as zf:
            theme = json.loads(zf.read("theme.json"))
    except (zipfile.BadZipFile, KeyError, json.JSONDecodeError) as exc:
        print(f"  WARNING: {filepath.name}: {exc}, skipping", file=sys.stderr)
        return None

    theme_id = theme.get("id")
    name = theme.get("name")
    if not theme_id or not name:
        print(f"  WARNING: {filepath.name}: missing id/name, skipping", file=sys.stderr)
        return None

    return {
        "id": theme_id,
        "name": name,
        "description": theme.get("description", name),
        "file": filepath.name,
        "size": filepath.stat().st_size,
        "crc32": compute_crc32(filepath),
    }


def main():
    parser = argparse.ArgumentParser(description="Generate themes.json from .cptheme archives")
    parser.add_argument("--input", required=True, help="Directory containing .cptheme files")
    parser.add_argument("--base-url", required=True, help="URL prefix (device concatenates baseUrl + file)")
    parser.add_argument("--output", required=True, help="Output path for themes.json")
    args = parser.parse_args()

    input_dir = Path(args.input)
    if not input_dir.is_dir():
        print(f"ERROR: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    base_url = args.base_url if args.base_url.endswith("/") else args.base_url + "/"

    themes = [m for p in sorted(input_dir.glob("*.cptheme")) if (m := read_theme_meta(p))]
    if not themes:
        print("ERROR: no valid .cptheme files found", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(themes)} theme(s): {', '.join(t['id'] for t in themes)}")

    manifest = {
        "version": THEME_MANIFEST_VERSION,
        "baseUrl": base_url,
        "themes": themes,
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
