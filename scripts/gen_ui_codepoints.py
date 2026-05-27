#!/usr/bin/env python3
"""
Generate the UI-codepoint union used to subset theme `.cpfont` files at build time.

Walks every `*.yaml` translation file under `lib/I18n/translations/`, accumulates
every codepoint that appears in any translation value, unions that set with a
hardcoded `ALWAYS_INCLUDE` table covering ASCII printable, Latin-1 Supplement,
common typography (smart quotes / dashes / ellipsis / bullet), currency, and
the Unicode replacement character (U+FFFD).

Output: `build/generated/ui_codepoints.txt`, one hex codepoint per line in sorted
ascending order. `scripts/build_cptheme.py` calls this generator inline and passes
the output to `lib/EpdFont/scripts/fontconvert_sdcard.py --codepoints-file` when
packaging theme bundled fonts, so the resulting `.cpfont`s only carry glyphs the
UI can actually display. The reader's own SD fonts (see
`lib/EpdFont/scripts/build-sd-fonts.py`) deliberately do NOT use this file —
those need full `reading`-preset coverage for book text.

Usage:
    python scripts/gen_ui_codepoints.py [translations_dir [output_path]]

Defaults to `lib/I18n/translations` and `build/generated/ui_codepoints.txt`
relative to the project root.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterable, Set

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from gen_i18n import parse_yaml_file  # noqa: E402  (intentional after sys.path)


# ---------------------------------------------------------------------------
# Hardcoded codepoint set
# ---------------------------------------------------------------------------
#
# These ranges are included unconditionally regardless of what the YAML
# translations happen to contain. Rationale lives in the plan; the short version:
#
#   - ASCII printable: every UI string passes through it.
#   - Latin-1 Supplement (full range): ~55–65 of its 96 codepoints already arrive
#     via Spanish/French/German/etc. YAMLs; adding the remaining ~30–40 covers
#     missing translations, common book-metadata symbols (© ® ¶ § ¡ ¿ °…), and
#     foreign Western European book titles for ~26 KB file-size delta per
#     4-unique-font theme.
#   - Smart quotes, em/en dash, ellipsis, bullet: needed for typography lift.
#   - Common currency glyphs.
#   - U+FFFD: replacement glyph (also added by the fontconvert subset path; we
#     include it here so generators don't have to special-case it).


def _expand_ranges(ranges: Iterable[tuple[int, int]]) -> Set[int]:
    out: Set[int] = set()
    for start, end in ranges:
        out.update(range(start, end + 1))
    return out


ALWAYS_INCLUDE: Set[int] = _expand_ranges([
    (0x0020, 0x007E),  # ASCII printable
    (0x00A0, 0x00FF),  # Latin-1 Supplement (full range)
    (0x2013, 0x2014),  # en dash, em dash
    (0x2018, 0x201D),  # ‘ ’ “ ”
    (0x2022, 0x2022),  # • bullet
    (0x2026, 0x2026),  # … ellipsis
    (0x00A2, 0x00A2),  # ¢ cent
    (0x00A3, 0x00A3),  # £ pound
    (0x00A5, 0x00A5),  # ¥ yen
    (0x20AC, 0x20AC),  # € euro
    (0xFFFD, 0xFFFD),  # replacement
])


# ---------------------------------------------------------------------------
# Codepoint extraction
# ---------------------------------------------------------------------------


def codepoints_from_translations(translations_dir: Path) -> Set[int]:
    """Read every YAML file in *translations_dir* and return the union of
    codepoints across all translation values (keys starting with `_` skipped)."""
    if not translations_dir.is_dir():
        raise FileNotFoundError(f"Translations directory not found: {translations_dir}")

    yaml_files = sorted(translations_dir.glob("*.yaml"))
    if not yaml_files:
        raise FileNotFoundError(f"No .yaml files found in {translations_dir}")

    cps: Set[int] = set()
    for yf in yaml_files:
        data = parse_yaml_file(str(yf))
        for key, value in data.items():
            if key.startswith("_"):
                continue
            for ch in value:
                cps.add(ord(ch))
    return cps


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def _merge_intervals(cps: Iterable[int]) -> list[tuple[int, int]]:
    """Compact a sorted codepoint set into a list of (start, end) intervals
    purely for the log line — the on-disk format is one codepoint per line."""
    sorted_cps = sorted(set(cps))
    if not sorted_cps:
        return []
    intervals: list[tuple[int, int]] = []
    start = prev = sorted_cps[0]
    for cp in sorted_cps[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        intervals.append((start, prev))
        start = prev = cp
    intervals.append((start, prev))
    return intervals


def write_codepoints_file(cps: Set[int], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    sorted_cps = sorted(cps)
    with output_path.open("w", encoding="utf-8", newline="\n") as f:
        for cp in sorted_cps:
            f.write(f"0x{cp:04X}\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def generate(translations_dir: Path, output_path: Path) -> None:
    yaml_cps = codepoints_from_translations(translations_dir)
    union = yaml_cps | ALWAYS_INCLUDE

    write_codepoints_file(union, output_path)

    intervals = _merge_intervals(union)
    added_from_always = len(ALWAYS_INCLUDE - yaml_cps)
    print(
        f"ui codepoints: {len(union)} cps in {len(intervals)} merged intervals "
        f"({len(yaml_cps)} from YAML, +{added_from_always} from ALWAYS_INCLUDE) "
        f"-> {output_path}"
    )


def _default_paths() -> tuple[Path, Path]:
    repo_root = _SCRIPT_DIR.parent
    return (
        repo_root / "lib" / "I18n" / "translations",
        repo_root / "build" / "generated" / "ui_codepoints.txt",
    )


def main(argv: list[str] | None = None) -> None:
    argv = list(sys.argv[1:] if argv is None else argv)
    default_translations, default_output = _default_paths()

    translations_dir = Path(argv[0]) if len(argv) >= 1 else default_translations
    output_path = Path(argv[1]) if len(argv) >= 2 else default_output

    generate(translations_dir, output_path)


if __name__ == "__main__":
    main()
