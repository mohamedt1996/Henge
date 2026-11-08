#!/bin/sh
# Mach-O arm64 assembly -> ELF: drop the C-symbol underscore, rewrite @PAGE/@PAGEOFF relocations.
sed -E -e 's/([^A-Za-z0-9_.$]|^)_\\/\1\\/g' -e 's/([^A-Za-z0-9_.$)]|^)_([A-Za-z][A-Za-z0-9_]*)/\1\2/g' \
       -e 's/([A-Za-z0-9_.$]+)@PAGEOFF/:lo12:\1/g' \
       -e 's/([A-Za-z0-9_.$]+)@PAGE/\1/g' \
       -e '/^[[:space:]]*\.(subsections_via_symbols|build_version|macosx_version_min|ios_version_min)/d' "$1"
