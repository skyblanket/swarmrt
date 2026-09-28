#!/usr/bin/env bash
# Lay out a SwarmRT install at $1 exactly as a release archive ships it:
#   bin/swc  bin/libswarmrt.a  src/*.h  lib/*.sw  templates/  docs/  + README etc.
# swc finds everything relative to its own binary (<root>/bin/swc), so this
# is the layout `swc build`, `import Std` and `swc new` need.
set -euo pipefail
dest="${1:?usage: install_layout.sh <dest>}"
here="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$dest/bin" "$dest/src" "$dest/lib" "$dest/templates" "$dest/docs"
cp "$here/bin/swc" "$here/bin/libswarmrt.a" "$dest/bin/"
cp "$here"/src/*.h "$dest/src/"          # the generated C includes the runtime headers
cp "$here"/lib/*.sw "$dest/lib/"
cp -R "$here"/templates/. "$dest/templates/"
cp "$here"/docs/*.md "$dest/docs/"      # the release notes point at DEPLOYMENT.md and CHANGELOG.md
cp "$here"/VERSION "$here"/README.md "$here"/LICENSE "$dest/" 2>/dev/null || true
