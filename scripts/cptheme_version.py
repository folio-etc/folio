# Canonical version constants for the .cptheme archive and theme manifest.
#
# Single source of truth for the build tooling: the CI workflow in the
# folio-themes repo and generate-theme-manifest.py read from here.
#
# The firmware C++ side carries its own copy — bump it manually when the
# firmware learns to parse a new manifest/archive shape.

# JSON manifest schema version. Bump when themes.json shape changes.
THEME_MANIFEST_VERSION = 1
