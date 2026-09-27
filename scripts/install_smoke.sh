#!/usr/bin/env bash
# The five-minute path, run from outside the repo against an install laid
# out like a release archive: swc on PATH, `swc new`, the project's offline
# tests, and a build that imports the stdlib. Catches install-relative
# lookups that only work from inside the checkout.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
bash "$here/scripts/install_layout.sh" "$tmp/swarmrt"
export PATH="$tmp/swarmrt/bin:$PATH"
cd "$tmp"
swc --version
swc new demo
if swc new demo 2>/dev/null; then echo "FAIL: swc new overwrote an existing dir"; exit 1; fi
if swc new 'Bad;name' 2>/dev/null; then echo "FAIL: swc new accepted a bad name"; exit 1; fi
if swc new ok --template '../x' 2>/dev/null; then echo "FAIL: swc new accepted a template path"; exit 1; fi
cd demo
make test
make build
printf 'module Hi\nimport Std\nfun main() { print(Std.join(["install", "ok"], " ")) }\n' > hi.sw
swc build hi.sw -o bin/hi >/dev/null 2>&1
./bin/hi | grep -qx "install ok"
echo "install smoke: OK"
